#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "hub.h"
#include "lvgl.h"
#include "settings.h"

static const char *TAG = "server";
static httpd_handle_t s_srv;
static volatile ota_progress_t s_ota;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

ota_progress_t server_ota_progress(void) { return s_ota; }

/// Console "otatest": shows the update screen with fake progress (UI check without flashing).
void server_ota_simulate(void)
{
    snprintf((char *)s_ota.from, sizeof s_ota.from, "%s", "test");
    s_ota.active = true;
    for (int p = 0; p <= 100; p += 2) {
        s_ota.percent = p;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    s_ota.active = false;
}

// ---------------------------------------------------------------- WebSocket

typedef struct {
    int fd;
    size_t len;
    char text[];
} out_msg_t;

static void send_work(void *arg)
{
    out_msg_t *m = arg;
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)m->text, .len = m->len, .final = true};
    esp_err_t err = httpd_ws_send_frame_async(s_srv, m->fd, &f);
    if (err != ESP_OK) ESP_LOGW(TAG, "ws send fd=%d: %s", m->fd, esp_err_to_name(err));
    free(m);
}

void server_send(int fd, const char *text)
{
    if (!s_srv || fd < 0) return;
    size_t len = strlen(text);
    out_msg_t *m = malloc(sizeof *m + len + 1);
    if (!m) return;
    m->fd = fd;
    m->len = len;
    memcpy(m->text, text, len + 1);
    if (httpd_queue_work(s_srv, send_work, m) != ESP_OK) {
        ESP_LOGW(TAG, "ws queue fd=%d failed", fd);
        free(m);
    }
}

void server_close(int fd)
{
    if (s_srv && fd >= 0) httpd_sess_trigger_close(s_srv, fd);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {  // handshake done
        ESP_LOGI(TAG, "ws open fd=%d", fd);
        hub_on_open(fd);
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.type == HTTPD_WS_TYPE_CLOSE) return ESP_OK;
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len == 0) return ESP_OK;
    if (f.len > 40 * 1024) {
        ESP_LOGW(TAG, "frame of %u bytes dropped", (unsigned)f.len);
        return ESP_FAIL;
    }
    f.payload = malloc(f.len + 1);
    if (!f.payload) return ESP_ERR_NO_MEM;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK) {
        f.payload[f.len] = 0;
        ESP_LOGD(TAG, "ws rx fd=%d %.*s", fd, (int)(f.len > 80 ? 80 : f.len), (char *)f.payload);
        hub_on_message(fd, (char *)f.payload, f.len);
    }
    free(f.payload);
    return err;
}

static void on_close(httpd_handle_t hd, int fd)
{
    hub_on_close(fd);
    close(fd);
}

// ---------------------------------------------------------------- HTTP helpers

static bool authorized(httpd_req_t *req)
{
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK && !strncmp(hdr, "Bearer ", 7) &&
        settings_token_valid(hdr + 7))
        return true;
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"pair this PC first (deskhud daemon) and use its token\"}");
    return false;
}

static esp_err_t send_cjson(httpd_req_t *req, cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    cJSON_free(txt);
    cJSON_Delete(o);
    return err;
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) return NULL;
    char *buf = malloc(req->content_len + 1);
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    cJSON *o = cJSON_ParseWithLength(buf, got);
    free(buf);
    return o;
}

// ---------------------------------------------------------------- endpoints

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_info(httpd_req_t *req)
{
    return send_cjson(req, hub_device_json(false));
}

static esp_err_t h_settings_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    return send_cjson(req, settings_to_json());
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *patch = read_json(req);
    if (!patch) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a JSON object");
        return ESP_OK;
    }
    settings_apply_json(patch);
    cJSON_Delete(patch);
    return send_cjson(req, settings_to_json());
}

static void restart_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (s_ota.active) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"update already in progress\"}");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");
        return ESP_OK;
    }
    char from[40] = "a PC";
    httpd_req_get_hdr_value_str(req, "X-DeskHUD-Host", from, sizeof from);
    snprintf((char *)s_ota.from, sizeof s_ota.from, "%s", from);
    s_ota.percent = 0;
    s_ota.active = true;
    ESP_LOGI(TAG, "OTA from %s: %d bytes -> %s", from, (int)req->content_len, part->label);

    esp_ota_handle_t ota;
    // Erase the whole image size up front: 64 KB block erases are ~10x faster than the 4 KB sector
    // erases OTA_WITH_SEQUENTIAL_WRITES does as data arrives (which made uploads crawl).
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    char *buf = malloc(8192);
    int total = 0;
    while (err == ESP_OK && total < (int)req->content_len) {
        int r = httpd_req_recv(req, buf, 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        total += r;
        s_ota.percent = (int)((int64_t)total * 100 / req->content_len);
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(ota);  // validates the image (magic, SHA)
    else esp_ota_abort(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        s_ota.active = false;
        hub_push_toast(2, "Update failed", esp_err_to_name(err), 8);
        httpd_resp_set_status(req, "500 Internal Server Error");
        char msg[96];
        snprintf(msg, sizeof msg, "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_sendstr(req, msg);
    }
    s_ota.percent = 100;
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/// What is on the glass right now (including toasts and dialogs) as a 16-bit BMP, streamed
/// straight from the panel's frame buffer: no extra 1.2 MB allocation.
static esp_err_t h_screenshot(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const uint16_t *fb = board_framebuffer();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame buffer");
        return ESP_OK;
    }
    const int w = BOARD_W, h = BOARD_H;
    // BITMAPINFOHEADER + RGB565 masks; rows are stored bottom-up.
    uint8_t hdr[66] = {'B', 'M'};
    uint32_t data = (uint32_t)w * h * 2, *u32;
#define PUT32(off, v) (u32 = (uint32_t *)(hdr + (off)), *u32 = (v))
    PUT32(2, sizeof hdr + data);
    PUT32(10, sizeof hdr);
    PUT32(14, 40);
    PUT32(18, w);
    PUT32(22, h);
    hdr[26] = 1;
    hdr[28] = 16;
    PUT32(30, 3);  // BI_BITFIELDS
    PUT32(34, data);
    PUT32(54, 0xF800);
    PUT32(58, 0x07E0);
    PUT32(62, 0x001F);
    const int rows_per_chunk = 16;
    uint8_t *rows = heap_caps_malloc(w * 2 * rows_per_chunk, MALLOC_CAP_SPIRAM);
    esp_err_t err = rows ? httpd_resp_set_type(req, "image/bmp") : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (char *)hdr, sizeof hdr);
    for (int y = h - 1; y >= 0 && err == ESP_OK; y -= rows_per_chunk) {
        int n = y + 1 < rows_per_chunk ? y + 1 : rows_per_chunk;
        // Copy under the LVGL lock: between frames both direct-mode buffers hold the same image,
        // mid-frame the back buffer is half drawn.
        board_lock();
        for (int k = 0; k < n; k++) memcpy(rows + k * w * 2, fb + (size_t)(y - k) * w, w * 2);
        board_unlock();
        err = httpd_resp_send_chunk(req, (char *)rows, n * w * 2);
    }
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    free(rows);
    return err;
}

static esp_err_t h_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type, X-DeskHUD-Host");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    return httpd_resp_send(req, NULL, 0);
}

void server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 10;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    cfg.stack_size = 8192;
    cfg.core_id = 0;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = h_settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/screenshot", .method = HTTP_GET, .handler = h_screenshot},
        {.uri = "/api/*", .method = HTTP_OPTIONS, .handler = h_options},
    };
    for (size_t i = 0; i < sizeof uris / sizeof *uris; i++) httpd_register_uri_handler(s_srv, &uris[i]);
    ESP_LOGI(TAG, "listening on :80");
}
