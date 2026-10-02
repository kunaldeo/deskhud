#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "settings.h"

static const char *TAG = "net";
static net_status_t s_st;
static SemaphoreHandle_t s_mu;
static esp_netif_t *s_netif;
static int s_retries;
static bool s_scanning;
static esp_timer_handle_t s_reconnect;

static void reconnect_cb(void *arg)
{
    char ssid[33], pass[65];
    if (!s_scanning && settings_wifi(ssid, pass)) esp_wifi_connect();
}

static void set_state(net_state_t st)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.state = st;
    if (st != NET_CONNECTED) s_st.ip[0] = 0;
    xSemaphoreGive(s_mu);
}

void net_apply_tz(void)
{
    // "auto" follows the active PC; the UI applies that zone once a PC reports it.
    settings_t s = settings_get();
    setenv("TZ", strcmp(s.tz, "auto") ? s.tz : "UTC0", 1);
    tzset();
}

void net_update_mdns(void)
{
    settings_t s = settings_get();
    const esp_app_desc_t *app = esp_app_get_description();
    mdns_instance_name_set(s.name);
    mdns_service_remove("_deskhud", "_tcp");
    mdns_txt_item_t txt[] = {
        {"id", s_st.id}, {"name", s.name}, {"fw", app->version}, {"proto", "1"},
    };
    mdns_service_add(s.name, "_deskhud", "_tcp", 80, txt, sizeof txt / sizeof *txt);
    mdns_service_add(s.name, "_http", "_tcp", 80, NULL, 0);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        char ssid[33], pass[65];
        if (settings_wifi(ssid, pass)) {
            set_state(NET_CONNECTING);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_scanning) return;  // scan aborts the connect; the scan restarts it
        ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        set_state(s_retries > 5 ? NET_FAILED : NET_CONNECTING);
        // Back off a little but never give up: the router may just be rebooting.
        int delay_ms = s_retries < 5 ? 500 : 5000;
        s_retries++;
        esp_timer_stop(s_reconnect);
        esp_timer_start_once(s_reconnect, delay_ms * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        snprintf(s_st.ip, sizeof s_st.ip, IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_st.ssid, sizeof s_st.ssid, "%s", (char *)ap.ssid);
        s_st.rssi = ap.rssi;
        s_st.state = NET_CONNECTED;
        xSemaphoreGive(s_mu);
        s_retries = 0;
        ESP_LOGI(TAG, "connected to %s as %s (%s.local)", s_st.ssid, s_st.ip, s_st.hostname);
        net_update_mdns();
    }
}

static void on_time(struct timeval *tv)
{
    s_st.time_synced = true;
    ESP_LOGI(TAG, "time synced");
}

void net_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    esp_timer_create_args_t ta = {.callback = reconnect_cb, .name = "wifi_retry"};
    esp_timer_create(&ta, &s_reconnect);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_st.id, sizeof s_st.id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_st.hostname, sizeof s_st.hostname, "deskhud-%02x%02x%02x", mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(s_netif, s_st.hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    char ssid[33], pass[65];
    if (settings_wifi(ssid, pass)) {
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
        snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;  // pick the strongest mesh point
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        snprintf(s_st.ssid, sizeof s_st.ssid, "%s", ssid);
    } else {
        s_st.state = NET_NO_CONFIG;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save adds 100+ ms latency to every inbound frame; we're on USB power.
    esp_wifi_set_ps(WIFI_PS_NONE);

    mdns_init();
    mdns_hostname_set(s_st.hostname);

    net_apply_tz();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.sync_cb = on_time;
    esp_netif_sntp_init(&sc);
}

net_status_t net_status(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_st.state == NET_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_st.rssi = ap.rssi;
    }
    if (!s_st.time_synced && time(NULL) > 1735689600) s_st.time_synced = true;  // 2025-01-01: RTC kept across OTA reboots
    net_status_t st = s_st;
    xSemaphoreGive(s_mu);
    return st;
}

void net_connect(const char *ssid, const char *pass)
{
    settings_set_wifi(ssid, pass);
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass ? pass : "");
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.ssid, sizeof s_st.ssid, "%s", ssid);
    xSemaphoreGive(s_mu);
    s_retries = 0;
    set_state(NET_CONNECTING);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const net_ap_t *)b)->rssi - ((const net_ap_t *)a)->rssi;
}

int net_scan(net_ap_t *out, int max)
{
    bool was_connecting = s_st.state == NET_CONNECTING || s_st.state == NET_FAILED;
    s_scanning = was_connecting;
    if (was_connecting) esp_wifi_disconnect();
    wifi_scan_config_t sc = {.show_hidden = false};
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof *recs);
        esp_wifi_scan_get_ap_records(&num, recs);
        for (int i = 0; i < num; i++) {
            if (!recs[i].ssid[0]) continue;
            int j;
            for (j = 0; j < n && strcmp(out[j].ssid, (char *)recs[i].ssid); j++) {}
            if (j < n) {  // mesh networks repeat the SSID: keep the strongest
                if (recs[i].rssi > out[j].rssi) out[j].rssi = recs[i].rssi;
                continue;
            }
            if (n == max) continue;
            snprintf(out[n].ssid, sizeof out[n].ssid, "%s", (char *)recs[i].ssid);
            out[n].rssi = recs[i].rssi;
            out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            n++;
        }
        free(recs);
    }
    qsort(out, n, sizeof *out, cmp_rssi);
    s_scanning = false;
    if (was_connecting) esp_wifi_connect();
    return n;
}
