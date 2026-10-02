#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static SemaphoreHandle_t s_mu;
static settings_t s_set;
static paired_pc_t s_pcs[MAX_PAIRED];
static int s_npcs;
static settings_cb_t s_cbs[6];
static int s_ncbs;

static const settings_t DEFAULTS = {
    .name = "DeskHUD",
    .brightness = 80,
    .dim_brightness = 30,
    .dim_after = 86400,  // never: it's a status display, not a monitor
    .wake_on_alert = true,
    .accent = 0x7aa2f7,
    .theme = "midnight",
    .temp_unit = 'C',
    .clock_24h = true,
    .tz = "auto",  // follow the active PC
    .page = "overview",
    .auto_page = true,
    .flip = false,
    .failover = FAILOVER_AUTO,
    .pinned_pc = "",
    .agent_text = 2,
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("deskhud", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "settings", &s_set, sizeof s_set);
    nvs_set_blob(h, "pcs", s_pcs, sizeof(paired_pc_t) * s_npcs);
    nvs_set_i32(h, "npcs", s_npcs);
    nvs_commit(h);
    nvs_close(h);
}

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_mu = xSemaphoreCreateRecursiveMutex();
    s_set = DEFAULTS;
    nvs_handle_t h;
    if (nvs_open("deskhud", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_set;
        settings_t tmp = DEFAULTS;
        // Fields are only ever appended: a shorter blob from older firmware fills the start and the
        // new fields keep their defaults. A longer one (a downgrade) doesn't fit: defaults.
        if (nvs_get_blob(h, "settings", &tmp, &len) == ESP_OK && len <= sizeof s_set) s_set = tmp;
        int32_t n = 0;
        nvs_get_i32(h, "npcs", &n);
        len = sizeof(paired_pc_t) * clampi(n, 0, MAX_PAIRED);
        if (n > 0 && nvs_get_blob(h, "pcs", s_pcs, &len) == ESP_OK) s_npcs = len / sizeof(paired_pc_t);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "loaded: name=%s brightness=%d theme=%s paired=%d", s_set.name, s_set.brightness, s_set.theme, s_npcs);
}

settings_t settings_get(void)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    settings_t s = s_set;
    xSemaphoreGiveRecursive(s_mu);
    return s;
}

void settings_on_change(settings_cb_t cb)
{
    if (s_ncbs < (int)(sizeof s_cbs / sizeof *s_cbs)) s_cbs[s_ncbs++] = cb;
}

static bool get_str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v) || strcmp(dst, v->valuestring) == 0) return false;
    snprintf(dst, n, "%s", v->valuestring);
    return true;
}

static bool get_int(const cJSON *o, const char *k, int *dst, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    int n = clampi(v->valueint, lo, hi);
    if (n == *dst) return false;
    *dst = n;
    return true;
}

static bool get_bool(const cJSON *o, const char *k, bool *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsBool(v) || cJSON_IsTrue(v) == *dst) return false;
    *dst = cJSON_IsTrue(v);
    return true;
}

static bool one_of(const char *v, const char *const *opts)
{
    for (; *opts; opts++)
        if (!strcmp(v, *opts)) return true;
    return false;
}

uint32_t settings_apply_json(const cJSON *p)
{
    if (!cJSON_IsObject(p)) return 0;
    uint32_t ch = 0;
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    settings_t old = s_set;
    if (get_str(p, "name", s_set.name, sizeof s_set.name)) ch |= SET_NAME;
    if (get_int(p, "brightness", &s_set.brightness, 5, 100)) ch |= SET_DISPLAY;
    if (get_int(p, "dim_brightness", &s_set.dim_brightness, 0, 100)) ch |= SET_DISPLAY;
    if (get_int(p, "dim_after", &s_set.dim_after, 5, 86400)) ch |= SET_DISPLAY;
    if (get_bool(p, "wake_on_alert", &s_set.wake_on_alert)) ch |= SET_DISPLAY;
    if (get_bool(p, "auto_page", &s_set.auto_page)) ch |= SET_DISPLAY;
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(p, "accent");
    if (cJSON_IsString(acc)) {
        const char *h = acc->valuestring[0] == '#' ? acc->valuestring + 1 : acc->valuestring;
        unsigned v;
        if (strlen(h) == 6 && sscanf(h, "%6x", &v) == 1 && v != s_set.accent) {
            s_set.accent = v;
            ch |= SET_THEME;
        }
    }
    static const char *const themes[] = {"midnight", "graphite", "aurora", "paper", "ink", "newsprint", NULL};
    const cJSON *th = cJSON_GetObjectItemCaseSensitive(p, "theme");
    if (cJSON_IsString(th) && one_of(th->valuestring, themes) && strcmp(th->valuestring, s_set.theme)) {
        snprintf(s_set.theme, sizeof s_set.theme, "%s", th->valuestring);
        ch |= SET_THEME;
    }
    const cJSON *tu = cJSON_GetObjectItemCaseSensitive(p, "temp_unit");
    if (cJSON_IsString(tu) && (tu->valuestring[0] == 'C' || tu->valuestring[0] == 'F') && tu->valuestring[0] != s_set.temp_unit) {
        s_set.temp_unit = tu->valuestring[0];
        ch |= SET_THEME;
    }
    if (get_bool(p, "clock_24h", &s_set.clock_24h)) ch |= SET_CLOCK;
    if (get_str(p, "tz", s_set.tz, sizeof s_set.tz)) ch |= SET_CLOCK;
    static const char *const pages[] = {"overview", "agents", "system", NULL};
    const cJSON *pg = cJSON_GetObjectItemCaseSensitive(p, "page");
    if (cJSON_IsString(pg) && one_of(pg->valuestring, pages) && strcmp(pg->valuestring, s_set.page)) {
        snprintf(s_set.page, sizeof s_set.page, "%s", pg->valuestring);
        ch |= SET_DISPLAY;
    }
    if (get_bool(p, "flip", &s_set.flip)) ch |= SET_REBOOT;
    const cJSON *fo = cJSON_GetObjectItemCaseSensitive(p, "failover");
    if (cJSON_IsString(fo)) {
        failover_t f = strcmp(fo->valuestring, "manual") ? FAILOVER_AUTO : FAILOVER_MANUAL;
        if (f != s_set.failover) {
            s_set.failover = f;
            ch |= SET_ROUTING;
        }
    }
    if (get_str(p, "pinned_pc", s_set.pinned_pc, sizeof s_set.pinned_pc)) ch |= SET_ROUTING;
    static const char *const sizes[] = {"small", "medium", "large", NULL};
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(p, "agent_text");
    if (cJSON_IsString(at) && one_of(at->valuestring, sizes)) {
        int v = at->valuestring[0] == 's' ? 0 : at->valuestring[0] == 'm' ? 1 : 2;
        if (v != s_set.agent_text) {
            s_set.agent_text = v;
            ch |= SET_THEME;  // a rebuild re-creates the agent views with the new fonts
        }
    }
    if (memcmp(&old, &s_set, sizeof old)) save_locked();
    xSemaphoreGiveRecursive(s_mu);
    if (ch)
        for (int i = 0; i < s_ncbs; i++) s_cbs[i](ch);
    return ch;
}

cJSON *settings_to_json(void)
{
    settings_t s = settings_get();
    cJSON *o = cJSON_CreateObject();
    char acc[8];
    snprintf(acc, sizeof acc, "#%06lx", (unsigned long)s.accent);
    cJSON_AddStringToObject(o, "name", s.name);
    cJSON_AddNumberToObject(o, "brightness", s.brightness);
    cJSON_AddNumberToObject(o, "dim_brightness", s.dim_brightness);
    cJSON_AddNumberToObject(o, "dim_after", s.dim_after);
    cJSON_AddBoolToObject(o, "wake_on_alert", s.wake_on_alert);
    cJSON_AddStringToObject(o, "accent", acc);
    cJSON_AddStringToObject(o, "theme", s.theme);
    cJSON_AddStringToObject(o, "temp_unit", s.temp_unit == 'F' ? "F" : "C");
    cJSON_AddBoolToObject(o, "clock_24h", s.clock_24h);
    cJSON_AddStringToObject(o, "tz", s.tz);
    cJSON_AddStringToObject(o, "page", s.page);
    cJSON_AddBoolToObject(o, "auto_page", s.auto_page);
    cJSON_AddBoolToObject(o, "flip", s.flip);
    cJSON_AddStringToObject(o, "failover", s.failover == FAILOVER_MANUAL ? "manual" : "auto");
    cJSON_AddStringToObject(o, "pinned_pc", s.pinned_pc);
    static const char *const sizes[] = {"small", "medium", "large"};
    cJSON_AddStringToObject(o, "agent_text", sizes[s.agent_text < 0 || s.agent_text > 2 ? 2 : s.agent_text]);
    return o;
}

bool settings_wifi(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t a = 33, b = 65;
    bool ok = nvs_get_str(h, "ssid", ssid, &a) == ESP_OK && nvs_get_str(h, "pass", pass, &b) == ESP_OK && ssid[0];
    nvs_close(h);
    return ok;
}

void settings_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

static int pc_index(const char *pc_id)
{
    for (int i = 0; i < s_npcs; i++)
        if (!strcmp(s_pcs[i].pc_id, pc_id)) return i;
    return -1;
}

bool settings_pc_find(const char *pc_id, paired_pc_t *out)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    int i = pc_index(pc_id);
    if (i >= 0 && out) *out = s_pcs[i];
    xSemaphoreGiveRecursive(s_mu);
    return i >= 0;
}

/// Constant-time compare so the token can't be guessed byte by byte over the network.
static bool token_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = la != lb;
    for (size_t i = 0; i < la && i < lb; i++) d |= a[i] ^ b[i];
    return d == 0 && la == 64;
}

bool settings_pc_check(const char *pc_id, const char *token)
{
    if (!pc_id || !token) return false;
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    int i = pc_index(pc_id);
    bool ok = i >= 0 && token_eq(s_pcs[i].token, token);
    xSemaphoreGiveRecursive(s_mu);
    return ok;
}

bool settings_token_valid(const char *token)
{
    if (!token) return false;
    bool ok = false;
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    for (int i = 0; i < s_npcs; i++) ok |= token_eq(s_pcs[i].token, token);
    xSemaphoreGiveRecursive(s_mu);
    return ok;
}

void settings_pc_add(const char *pc_id, const char *host, char *token_out)
{
    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof rnd);
    for (int i = 0; i < 32; i++) sprintf(token_out + i * 2, "%02x", rnd[i]);
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    int i = pc_index(pc_id);
    if (i < 0) {
        if (s_npcs == MAX_PAIRED) {  // evict the oldest pairing
            memmove(s_pcs, s_pcs + 1, sizeof(paired_pc_t) * (MAX_PAIRED - 1));
            s_npcs--;
        }
        i = s_npcs++;
    }
    snprintf(s_pcs[i].pc_id, sizeof s_pcs[i].pc_id, "%s", pc_id);
    snprintf(s_pcs[i].host, sizeof s_pcs[i].host, "%s", host);
    memcpy(s_pcs[i].token, token_out, 65);
    save_locked();
    xSemaphoreGiveRecursive(s_mu);
}

void settings_pc_forget(const char *pc_id)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    int i = pc_index(pc_id);
    if (i >= 0) {
        memmove(s_pcs + i, s_pcs + i + 1, sizeof(paired_pc_t) * (s_npcs - i - 1));
        s_npcs--;
        if (!strcmp(s_set.pinned_pc, pc_id)) s_set.pinned_pc[0] = 0;
        save_locked();
    }
    xSemaphoreGiveRecursive(s_mu);
}

int settings_pc_list(paired_pc_t *out, int max)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    int n = s_npcs < max ? s_npcs : max;
    memcpy(out, s_pcs, sizeof(paired_pc_t) * n);
    xSemaphoreGiveRecursive(s_mu);
    return n;
}

void settings_factory_reset(void)
{
    nvs_flash_erase();
}
