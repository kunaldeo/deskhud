#include "hub.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "hub";

#define SILENT_MS 6000    // active PC considered gone (failover)
#define DEAD_MS 30000     // connection closed
#define PAIR_TIMEOUT_MS 60000

typedef struct {
    int fd;  // -1 = free slot
    bool authed;
    bool control;  // web settings page / CLI tools: never the data source
    char pc_id[40], host[40], os[40];
    char tz[48];  // the PC's POSIX time zone
    int64_t last_rx;
} conn_t;

static SemaphoreHandle_t s_mu;
static conn_t s_conns[MAX_CONNS];
static int s_active = -1;  // index into s_conns
static stats_t *s_stats;
static history_t *s_hist;
static agent_t *s_agents;

static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

// Full text of one tool output / result, fetched on demand for the popup.
#define FULL_MAX (16 * 1024 + 1)
static char *s_full;
static int s_full_seq, s_full_req, s_full_ready;
static int s_nagents;
static hub_versions_t s_ver;
static char s_tz[48];  // active PC's POSIX time zone

static volatile int s_page_req = -1;

void hub_request_page(int p) { s_page_req = p; }

int hub_take_page_request(void)
{
    int p = s_page_req;
    s_page_req = -1;
    return p;
}

static struct {
    int fd;  // -1 = none
    char pc_id[40], host[40], os[40], code[7];
    bool control;
    int64_t since;
} s_pair = {.fd = -1};

#define TOASTS 6
static toast_t s_toasts[TOASTS];
static int s_toast_head, s_toast_count;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void lock(void) { xSemaphoreTakeRecursive(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mu); }

void hub_active_tz(char out[48])
{
    lock();
    memcpy(out, s_tz, 48);
    unlock();
}

/// Remember the active PC's time zone; the UI applies it (TZ must change on one task only).
static void set_active_tz(const char *tz)
{
    if (strcmp(tz, s_tz)) {
        snprintf(s_tz, sizeof s_tz, "%s", tz);
        s_ver.pcs_ver++;
    }
}

static void take_tz(int i, const cJSON *m)
{
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(m, "tz");
    if (!cJSON_IsString(tz) || !tz->valuestring[0]) return;
    snprintf(s_conns[i].tz, sizeof s_conns[i].tz, "%s", tz->valuestring);
    if (i == s_active) set_active_tz(s_conns[i].tz);
}

static int conn_by_fd(int fd)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (s_conns[i].fd == fd) return i;
    return -1;
}

static void send_json(int fd, cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    if (txt) {
        server_send(fd, txt);
        cJSON_free(txt);
    }
    cJSON_Delete(o);
}

static void send_simple(int fd, const char *type)
{
    char buf[32];
    snprintf(buf, sizeof buf, "{\"t\":\"%s\"}", type);
    server_send(fd, buf);
}

void hub_push_toast(int level, const char *title, const char *body, int ttl)
{
    lock();
    int idx = (s_toast_head + s_toast_count) % TOASTS;
    if (s_toast_count == TOASTS) {  // drop the oldest
        s_toast_head = (s_toast_head + 1) % TOASTS;
        s_toast_count--;
    }
    toast_t *t = &s_toasts[idx];
    t->level = level;
    snprintf(t->title, sizeof t->title, "%s", title ? title : "");
    snprintf(t->body, sizeof t->body, "%s", body ? body : "");
    t->ttl = ttl > 0 ? ttl : 6;
    s_toast_count++;
    unlock();
}

bool hub_next_toast(toast_t *out)
{
    lock();
    bool ok = s_toast_count > 0;
    if (ok) {
        *out = s_toasts[s_toast_head];
        s_toast_head = (s_toast_head + 1) % TOASTS;
        s_toast_count--;
    }
    unlock();
    return ok;
}

cJSON *hub_device_json(bool with_pcs)
{
    net_status_t ns = net_status();
    settings_t st = settings_get();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "id", ns.id);
    cJSON_AddStringToObject(d, "name", st.name);
    cJSON_AddStringToObject(d, "fw", app->version);
    cJSON_AddStringToObject(d, "idf", app->idf_ver);
    cJSON_AddNumberToObject(d, "w", 1024);
    cJSON_AddNumberToObject(d, "h", 600);
    cJSON_AddStringToObject(d, "ip", ns.ip);
    cJSON_AddStringToObject(d, "hostname", ns.hostname);
    cJSON_AddNumberToObject(d, "rssi", ns.rssi);
    cJSON_AddStringToObject(d, "ssid", ns.ssid);
    cJSON_AddNumberToObject(d, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(d, "uptime", (double)(esp_timer_get_time() / 1000000));
    static const char *const reasons[] = {"unknown", "poweron", "external", "software", "panic", "int_wdt", "task_wdt", "wdt",
                                          "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch", "cpu_lockup"};
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddStringToObject(d, "reset", rr < sizeof reasons / sizeof *reasons ? reasons[rr] : "unknown");
    if (with_pcs) {
        pc_info_t pcs[MAX_PAIRED + MAX_CONNS];
        int n = hub_pcs(pcs, sizeof pcs / sizeof *pcs);
        cJSON *arr = cJSON_AddArrayToObject(d, "pcs");
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "pc_id", pcs[i].pc_id);
            cJSON_AddStringToObject(p, "host", pcs[i].host);
            cJSON_AddBoolToObject(p, "online", pcs[i].online);
            cJSON_AddBoolToObject(p, "active", pcs[i].active);
            cJSON_AddBoolToObject(p, "paired", pcs[i].paired);
            cJSON_AddItemToArray(arr, p);
        }
    }
    return d;
}

static void send_settings(int fd)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "settings");
    cJSON_AddItemToObject(o, "settings", settings_to_json());
    send_json(fd, o);
}

static void send_role(int i)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "role");
    cJSON_AddBoolToObject(o, "active", i == s_active);
    cJSON_AddStringToObject(o, "active_host", s_active >= 0 ? s_conns[s_active].host : "");
    send_json(s_conns[i].fd, o);
}

static void reset_data(void)
{
    memset(s_stats, 0, sizeof *s_stats);
    s_stats->cpu_load = s_stats->cpu_temp = s_stats->cpu_power = NAN;
    memset(s_hist, 0, sizeof *s_hist);
    s_nagents = 0;
    s_ver.stats_ver++;
    s_ver.agents_ver++;
}

/// Picks the active PC (see PROTOCOL.md) and tells everyone if it changed. Call with the lock held.
static void elect(void)
{
    settings_t st = settings_get();
    int64_t now = now_ms();
    int pick = -1;
    if (st.pinned_pc[0]) {
        for (int i = 0; i < MAX_CONNS; i++)
            if (s_conns[i].fd >= 0 && s_conns[i].authed && !s_conns[i].control && !strcmp(s_conns[i].pc_id, st.pinned_pc)) pick = i;
    }
    bool cur_ok = s_active >= 0 && s_conns[s_active].fd >= 0 && s_conns[s_active].authed;
    bool cur_live = cur_ok && now - s_conns[s_active].last_rx < SILENT_MS;
    if (pick < 0 && cur_ok && (cur_live || st.failover == FAILOVER_MANUAL)) pick = s_active;
    if (pick < 0) {
        int64_t best = 0;
        for (int i = 0; i < MAX_CONNS; i++) {
            conn_t *c = &s_conns[i];
            if (c->fd >= 0 && c->authed && !c->control && now - c->last_rx < SILENT_MS && c->last_rx > best) {
                best = c->last_rx;
                pick = i;
            }
        }
    }
    if (pick == s_active) return;
    int prev = s_active;
    s_active = pick;
    reset_data();
    s_ver.pcs_ver++;
    s_ver.has_active = pick >= 0;
    snprintf(s_ver.active_host, sizeof s_ver.active_host, "%s", pick >= 0 ? s_conns[pick].host : "");
    snprintf(s_ver.active_os, sizeof s_ver.active_os, "%s", pick >= 0 ? s_conns[pick].os : "");
    if (pick >= 0 && s_conns[pick].tz[0]) set_active_tz(s_conns[pick].tz);
    ESP_LOGI(TAG, "active PC: %s -> %s", prev >= 0 ? s_conns[prev].host : "none", pick >= 0 ? s_conns[pick].host : "none");
    for (int i = 0; i < MAX_CONNS; i++)
        if (s_conns[i].fd >= 0 && s_conns[i].authed) send_role(i);
    if (pick >= 0 && prev >= 0) {
        char body[96];
        snprintf(body, sizeof body, "Now showing %s", s_conns[pick].host);
        hub_push_toast(0, "Switched PC", body, 4);
    }
}

static void welcome(int i)
{
    conn_t *c = &s_conns[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "t", "welcome");
    cJSON_AddItemToObject(o, "device", hub_device_json(false));
    cJSON_AddItemToObject(o, "settings", settings_to_json());
    send_json(c->fd, o);
    s_ver.pcs_ver++;
    elect();
    if (i != s_active) send_role(i);  // elect() only notifies on change
}

void hub_on_open(int fd)
{
    lock();
    int i = conn_by_fd(-1);
    if (i < 0) {
        unlock();
        ESP_LOGW(TAG, "too many connections; closing fd %d", fd);
        server_close(fd);
        return;
    }
    memset(&s_conns[i], 0, sizeof s_conns[i]);
    s_conns[i].fd = fd;
    s_conns[i].last_rx = now_ms();
    unlock();
}

void hub_on_close(int fd)
{
    lock();
    int i = conn_by_fd(fd);
    if (i >= 0) {
        ESP_LOGI(TAG, "%s disconnected", s_conns[i].host[0] ? s_conns[i].host : "client");
        s_conns[i].fd = -1;
        s_conns[i].authed = false;
        if (s_pair.fd == fd) s_pair.fd = -1;
        s_ver.pcs_ver++;
        if (i == s_active) {
            s_active = -1;  // force a fresh election
            s_ver.has_active = false;
            reset_data();
        }
        elect();
    }
    unlock();
}

static double num(const cJSON *o, const char *k, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static void str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static void push_hist(float *arr, float v)
{
    if (s_hist->len < HISTORY) {
        arr[s_hist->len] = v;
    } else {
        memmove(arr, arr + 1, sizeof(float) * (HISTORY - 1));
        arr[HISTORY - 1] = v;
    }
}

static void parse_stats(const cJSON *m)
{
    stats_t *s = s_stats;
    memset(s, 0, sizeof *s);
    const cJSON *cpu = cJSON_GetObjectItemCaseSensitive(m, "cpu");
    str(cpu, "name", s->cpu_name, sizeof s->cpu_name);
    s->cpu_load = num(cpu, "load", NAN);
    s->cpu_temp = num(cpu, "temp", NAN);
    s->cpu_power = num(cpu, "power", NAN);
    s->cpu_mhz = num(cpu, "mhz", 0);
    const cJSON *tc;
    cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(cpu, "temps"))
    {
        if (s->ntemps == MAX_TEMPS) break;
        str(tc, "name", s->temps[s->ntemps].name, sizeof s->temps[0].name);
        s->temps[s->ntemps++].temp = num(tc, "temp", NAN);
    }
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(cpu, "cores"))
    {
        if (s->ncores == MAX_CORES) break;
        s->cores[s->ncores++] = cJSON_IsNumber(c) ? c->valuedouble : 0;
    }
    const cJSON *mem = cJSON_GetObjectItemCaseSensitive(m, "mem");
    s->mem_used = num(mem, "used", 0);
    s->mem_total = num(mem, "total", 0);
    s->swap_used = num(mem, "swap_used", 0);
    s->swap_total = num(mem, "swap_total", 0);
    s->mem_available = num(mem, "available", 0);
    s->mem_cached = num(mem, "cached", 0);
    s->mem_free = num(mem, "free", 0);
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(m, "gpus"))
    {
        if (s->ngpus == MAX_GPUS) break;
        typeof(s->gpus[0]) *g = &s->gpus[s->ngpus++];
        str(c, "name", g->name, sizeof g->name);
        g->load = num(c, "load", NAN);
        g->temp = num(c, "temp", NAN);
        g->power = num(c, "power", NAN);
        g->power_max = num(c, "power_max", NAN);
        g->fan = num(c, "fan", NAN);
        g->mhz = num(c, "mhz", 0);
        g->vram_used = num(c, "vram_used", 0);
        g->vram_total = num(c, "vram_total", 0);
    }
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(m, "disks"))
    {
        if (s->ndisks == MAX_DISKS) break;
        typeof(s->disks[0]) *d = &s->disks[s->ndisks++];
        str(c, "mount", d->mount, sizeof d->mount);
        d->used = num(c, "used", 0);
        d->total = num(c, "total", 0);
    }
    const cJSON *io = cJSON_GetObjectItemCaseSensitive(m, "io");
    s->io_read = num(io, "read", 0);
    s->io_write = num(io, "write", 0);
    const cJSON *net = cJSON_GetObjectItemCaseSensitive(m, "net");
    s->net_down = num(net, "down", 0);
    s->net_up = num(net, "up", 0);
    str(net, "iface", s->iface, sizeof s->iface);
    str(net, "ip", s->ip, sizeof s->ip);
    s->rx_total = num(net, "rx_total", 0);
    s->tx_total = num(net, "tx_total", 0);
    int k = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(m, "load"))
    {
        if (k < 3) s->load[k++] = cJSON_IsNumber(c) ? c->valuedouble : 0;
    }
    s->uptime = num(m, "uptime", 0);
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(m, "procs"))
    {
        if (s->nprocs == MAX_PROCS) break;
        typeof(s->procs[0]) *p = &s->procs[s->nprocs++];
        str(c, "name", p->name, sizeof p->name);
        p->cpu = num(c, "cpu", 0);
        p->mem = num(c, "mem", 0);
        p->pid = num(c, "pid", 0);
        p->threads = num(c, "threads", 0);
    }
    push_hist(s_hist->cpu, isnan(s->cpu_load) ? 0 : s->cpu_load);
    push_hist(s_hist->gpu, s->ngpus && !isnan(s->gpus[0].load) ? s->gpus[0].load : 0);
    push_hist(s_hist->down, s->net_down);
    push_hist(s_hist->up, s->net_up);
    if (s_hist->len < HISTORY) s_hist->len++;
    s_ver.stats_ver++;
}

static ag_state_t parse_state(const char *s)
{
    static const char *const names[] = {"working", "permission", "question", "done", "error", "idle"};
    for (int i = 0; i < 6; i++)
        if (!strcmp(s, names[i])) return i;
    return AG_IDLE;
}

static void parse_agents(const cJSON *m)
{
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(m, "sessions"))
    {
        if (n == MAX_AGENTS) break;
        agent_t *g = &s_agents[n++];
        memset(g, 0, sizeof *g);
        str(a, "id", g->id, sizeof g->id);
        str(a, "agent", g->agent, sizeof g->agent);
        str(a, "project", g->project, sizeof g->project);
        str(a, "title", g->title, sizeof g->title);
        char st[16];
        str(a, "state", st, sizeof st);
        g->state = parse_state(st);
        str(a, "activity", g->activity, sizeof g->activity);
        str(a, "prompt", g->prompt, sizeof g->prompt);
        str(a, "ask", g->ask, sizeof g->ask);
        str(a, "summary", g->summary, sizeof g->summary);
        str(a, "model", g->model, sizeof g->model);
        g->ctx = num(a, "ctx", 0);
        g->ctx_max = num(a, "ctx_max", 0);
        g->out = num(a, "out", 0);
        g->turn_start = num(a, "turn_start", 0);
        g->updated = num(a, "updated", 0);
        const cJSON *l;
        cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(a, "lines"))
        {
            if (g->nlines == MAX_LINES) break;
            char k[8];
            str(l, "k", k, sizeof k);
            g->lines[g->nlines].kind = !strcmp(k, "tool") ? LINE_TOOL : !strcmp(k, "user") ? LINE_USER : LINE_TEXT;
            str(l, "s", g->lines[g->nlines].s, sizeof g->lines[0].s);
            str(l, "d", g->lines[g->nlines].d, sizeof g->lines[0].d);
            str(l, "i", g->lines[g->nlines].i, sizeof g->lines[0].i);
            str(l, "f", g->lines[g->nlines].f, sizeof g->lines[0].f);
            g->nlines++;
        }
    }
    s_nagents = n;
    s_ver.agents_ver++;
}

static void start_pairing(int i)
{
    conn_t *c = &s_conns[i];
    if (s_pair.fd >= 0 && s_pair.fd != c->fd) {
        server_send(c->fd, "{\"t\":\"error\",\"msg\":\"another PC is pairing; try again in a minute\"}");
        return;
    }
    s_pair.fd = c->fd;
    snprintf(s_pair.pc_id, sizeof s_pair.pc_id, "%s", c->pc_id);
    snprintf(s_pair.host, sizeof s_pair.host, "%s", c->host);
    snprintf(s_pair.os, sizeof s_pair.os, "%s", c->os);
    s_pair.control = c->control;
    snprintf(s_pair.code, sizeof s_pair.code, "%06lu", (unsigned long)(esp_random() % 1000000));
    s_pair.since = now_ms();
    char msg[64];
    snprintf(msg, sizeof msg, "{\"t\":\"pair_pending\",\"code\":\"%s\"}", s_pair.code);
    server_send(c->fd, msg);
    s_ver.pcs_ver++;
    ESP_LOGI(TAG, "pairing request from %s (code %s)", c->host, s_pair.code);
}

bool hub_pair_pending(char host[40], char code[7])
{
    lock();
    bool p = s_pair.fd >= 0;
    if (p) {
        memcpy(host, s_pair.host, 40);
        memcpy(code, s_pair.code, 7);
    }
    unlock();
    return p;
}

void hub_pair_respond(bool allow)
{
    lock();
    int i = s_pair.fd >= 0 ? conn_by_fd(s_pair.fd) : -1;
    if (i >= 0) {
        if (allow) {
            char token[65];
            // Browsers (control clients) are stored as "web:<host>" so PC lists can leave them out.
            char stored[40];
            snprintf(stored, sizeof stored, "%s%s", s_pair.control ? "web:" : "", s_pair.host);
            settings_pc_add(s_pair.pc_id, stored, token);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "t", "pair_result");
            cJSON_AddBoolToObject(o, "ok", true);
            cJSON_AddStringToObject(o, "token", token);
            send_json(s_conns[i].fd, o);
            s_conns[i].authed = true;
            s_conns[i].last_rx = now_ms();
            ESP_LOGI(TAG, "paired %s", s_pair.host);
            welcome(i);
        } else {
            server_send(s_conns[i].fd, "{\"t\":\"pair_result\",\"ok\":false}");
            server_close(s_conns[i].fd);
        }
    }
    s_pair.fd = -1;
    s_ver.pcs_ver++;
    unlock();
}

static void handle_cmd(int i, const cJSON *m)
{
    char cmd[16], arg[48];
    str(m, "cmd", cmd, sizeof cmd);
    str(m, "arg", arg, sizeof arg);
    if (!strcmp(cmd, "activate")) {
        // arg: "" / "pin" act on the sender; anything else is the pc_id to show (web page)
        if (!arg[0] || !strcmp(arg, "pin")) hub_activate(s_conns[i].pc_id, !strcmp(arg, "pin"));
        else hub_activate(arg, false);
    } else if (!strcmp(cmd, "pin") || !strcmp(cmd, "unpin")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "pinned_pc", !strcmp(cmd, "pin") ? arg : "");
        unlock();
        settings_apply_json(p);
        lock();
        cJSON_Delete(p);
    } else if (!strcmp(cmd, "reboot")) {
        hub_push_toast(1, "Restarting", "Requested from a PC", 3);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(cmd, "identify")) {
        settings_t st = settings_get();
        hub_push_toast(2, st.name, s_conns[i].host, 3);
    } else if (!strcmp(cmd, "privacy")) {  // screenshot mode: placeholders for names and addresses
        extern void ui_set_privacy(bool on);
        ui_set_privacy(!strcmp(arg, "on"));
    } else if (!strcmp(cmd, "page")) {
        // The pages, then the popups (10 + n) for screenshots: system info, processes.
        static const char *const pages[] = {"overview", "agents", "system", "settings"};
        for (int k = 0; k < 4; k++)
            if (!strcmp(arg, pages[k])) s_page_req = k;
        if (!strcmp(arg, "sysinfo")) s_page_req = 10;
        if (!strcmp(arg, "procs")) s_page_req = 11;
    } else if (!strcmp(cmd, "forget")) {
        settings_pc_forget(arg[0] ? arg : s_conns[i].pc_id);
        s_ver.pcs_ver++;
    }
}

void hub_on_message(int fd, const char *data, int len)
{
    cJSON *m = cJSON_ParseWithLength(data, len);
    if (!m) return;
    char t[16];
    str(m, "t", t, sizeof t);
    lock();
    int i = conn_by_fd(fd);
    if (i < 0) {
        // esp_http_server doesn't call the handler for the handshake: register on the first frame.
        hub_on_open(fd);
        i = conn_by_fd(fd);
    }
    if (i < 0) goto out;
    conn_t *c = &s_conns[i];
    c->last_rx = now_ms();

    if (!strcmp(t, "ping")) {
        send_simple(fd, "pong");
    } else if (!strcmp(t, "hello")) {
        str(m, "pc_id", c->pc_id, sizeof c->pc_id);
        str(m, "host", c->host, sizeof c->host);
        str(m, "os", c->os, sizeof c->os);
        take_tz(i, m);
        char role[12];
        str(m, "role", role, sizeof role);
        c->control = !strcmp(role, "control");
        char token[80];
        str(m, "token", token, sizeof token);
        if (!c->pc_id[0]) {
            server_send(fd, "{\"t\":\"error\",\"msg\":\"hello needs pc_id\"}");
            goto out;
        }
        // A reconnecting PC may still have a stale socket open: drop it. Control clients (browser
        // tabs share one id) may hold several connections, or two tabs would evict each other forever.
        for (int j = 0; j < MAX_CONNS; j++)
            if (!c->control && j != i && s_conns[j].fd >= 0 && !s_conns[j].control && !strcmp(s_conns[j].pc_id, c->pc_id)) {
                int old = s_conns[j].fd;
                if (j == s_active) s_active = i;  // keep the role across the reconnect
                s_conns[j].fd = -1;
                s_conns[j].authed = false;
                server_close(old);
            }
        if (settings_pc_check(c->pc_id, token)) {
            c->authed = true;
            if (c->control) ESP_LOGD(TAG, "control client %s", c->host);
            else ESP_LOGI(TAG, "hello from %s (%s)", c->host, c->os);
            if (s_active == i) {
                s_active = -1;  // re-run election so it sends the role
                reset_data();
            }
            welcome(i);
        } else {
            start_pairing(i);
        }
    } else if (!c->authed) {
        server_send(fd, "{\"t\":\"error\",\"msg\":\"not paired\"}");
    } else if (!strcmp(t, "stats")) {
        if (i == s_active) {
            parse_stats(m);
            take_tz(i, m);
        }
    } else if (!strcmp(t, "agents")) {
        if (i == s_active) parse_agents(m);
    } else if (!strcmp(t, "full")) {  // reply to hub_request_full
        if (i == s_active && num(m, "req", 0) == s_full_req) {
            str(m, "text", s_full, FULL_MAX);
            s_full_ready = s_full_req;
        }
    } else if (!strcmp(t, "sysinfo") || !strcmp(t, "procs")) {  // reply to hub_request: kept whole
        if (i == s_active && num(m, "req", 0) == s_full_req && s_full) {
            int n = len < FULL_MAX - 1 ? len : FULL_MAX - 1;
            memcpy(s_full, data, n);
            s_full[n] = 0;
            s_full_ready = s_full_req;
        }
    } else if (!strcmp(t, "notify")) {
        char title[80], body[200], lvl[8], full[96];
        str(m, "title", title, sizeof title);
        str(m, "body", body, sizeof body);
        str(m, "level", lvl, sizeof lvl);
        if (i != s_active) {
            snprintf(full, sizeof full, "%s · %s", c->host, title);
        } else {
            snprintf(full, sizeof full, "%s", title);
        }
        hub_push_toast(!strcmp(lvl, "alert") ? 2 : !strcmp(lvl, "warn") ? 1 : 0, full, body, num(m, "ttl", 6));
    } else if (!strcmp(t, "set")) {
        unlock();  // settings listeners may call back into the hub
        settings_apply_json(cJSON_GetObjectItemCaseSensitive(m, "settings"));
        lock();
        send_settings(fd);
    } else if (!strcmp(t, "get")) {
        send_settings(fd);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "device");
        cJSON_AddItemToObject(o, "device", hub_device_json(true));
        send_json(fd, o);
    } else if (!strcmp(t, "cmd")) {
        handle_cmd(i, m);
    }
    elect();  // a standby PC speaking up may take over from a silent active one
out:
    unlock();
    cJSON_Delete(m);
}

void hub_activate(const char *pc_id, bool pin)
{
    // An explicit choice overrides a pin on some other PC.
    settings_t st = settings_get();
    if (st.pinned_pc[0] && strcmp(st.pinned_pc, pc_id)) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "pinned_pc", "");
        settings_apply_json(p);
        cJSON_Delete(p);
    }
    lock();
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].fd >= 0 && s_conns[i].authed && !s_conns[i].control && !strcmp(s_conns[i].pc_id, pc_id)) {
            s_conns[i].last_rx = now_ms();
            if (s_active != i) {
                // Make the requested PC win the election: it is now the freshest and the old
                // active one is demoted by clearing the slot first.
                s_active = -1;
                s_ver.has_active = false;
                int64_t keep = s_conns[i].last_rx;
                for (int j = 0; j < MAX_CONNS; j++)
                    if (j != i && s_conns[j].fd >= 0 && s_conns[j].last_rx >= keep) s_conns[j].last_rx = keep - 1;
                elect();
            }
            break;
        }
    }
    unlock();
    if (pin) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "pinned_pc", pc_id);
        settings_apply_json(p);
        cJSON_Delete(p);
    }
}

hub_versions_t hub_versions(void)
{
    lock();
    hub_versions_t v = s_ver;
    unlock();
    return v;
}

void hub_stats(stats_t *out, history_t *hist)
{
    lock();
    if (out) *out = *s_stats;
    if (hist) *hist = *s_hist;
    unlock();
}

int hub_request_full(const char *session, const char *tool)
{
    lock();
    int req = 0;
    if (s_active >= 0 && s_full) {
        req = s_full_req = ++s_full_seq;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", "full");
        cJSON_AddNumberToObject(o, "req", req);
        cJSON_AddStringToObject(o, "session", session);
        cJSON_AddStringToObject(o, "tool", tool);
        char *txt = cJSON_PrintUnformatted(o);
        if (txt) server_send(s_conns[s_active].fd, txt);
        cJSON_free(txt);
        cJSON_Delete(o);
    }
    unlock();
    return req;
}

int hub_request(const char *kind)
{
    lock();
    int req = 0;
    if (s_active >= 0 && s_full) {
        req = s_full_req = ++s_full_seq;
        char msg[64];
        snprintf(msg, sizeof msg, "{\"t\":\"%s\",\"req\":%d}", kind, req);
        server_send(s_conns[s_active].fd, msg);
    }
    unlock();
    return req;
}

const char *hub_full_result(int req)
{
    lock();
    bool ready = req && s_full_ready == req;
    unlock();
    return ready ? s_full : NULL;
}

int hub_agents(agent_t *out, int max)
{
    lock();
    int n = s_nagents < max ? s_nagents : max;
    memcpy(out, s_agents, sizeof(agent_t) * n);
    unlock();
    return n;
}

int hub_pcs(pc_info_t *out, int max)
{
    paired_pc_t paired[MAX_PAIRED];
    int np = settings_pc_list(paired, MAX_PAIRED), n = 0;
    lock();
    for (int i = 0; i < np && n < max; i++) {
        // Paired browsers are not PCs (older firmware stored them as plain "Web browser").
        if (!strncmp(paired[i].host, "web:", 4) || !strcmp(paired[i].host, "Web browser")) continue;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].pc_id, sizeof out[n].pc_id, "%s", paired[i].pc_id);
        snprintf(out[n].host, sizeof out[n].host, "%s", paired[i].host);
        out[n].paired = true;
        for (int j = 0; j < MAX_CONNS; j++)
            if (s_conns[j].fd >= 0 && s_conns[j].authed && !strcmp(s_conns[j].pc_id, paired[i].pc_id)) {
                out[n].online = !s_conns[j].control;
                out[n].active = j == s_active;
                snprintf(out[n].host, sizeof out[n].host, "%s", s_conns[j].host);
                snprintf(out[n].os, sizeof out[n].os, "%s", s_conns[j].os);
            }
        n++;
    }
    unlock();
    return n;
}

void hub_broadcast_settings(void)
{
    lock();
    for (int i = 0; i < MAX_CONNS; i++)
        if (s_conns[i].fd >= 0 && s_conns[i].authed) send_settings(s_conns[i].fd);
    unlock();
}

static void on_settings(uint32_t changed)
{
    if (changed & SET_ROUTING) {
        lock();
        elect();
        s_ver.pcs_ver++;
        unlock();
    }
    hub_broadcast_settings();
}

static void tick_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        lock();
        int64_t now = now_ms();
        for (int i = 0; i < MAX_CONNS; i++) {
            conn_t *c = &s_conns[i];
            if (c->fd >= 0 && now - c->last_rx > DEAD_MS) {
                ESP_LOGW(TAG, "%s silent for %ds; closing", c->host[0] ? c->host : "client", DEAD_MS / 1000);
                server_close(c->fd);
            }
        }
        if (s_pair.fd >= 0 && now - s_pair.since > PAIR_TIMEOUT_MS) {
            int i = conn_by_fd(s_pair.fd);
            if (i >= 0) server_send(s_pair.fd, "{\"t\":\"pair_result\",\"ok\":false}");
            s_pair.fd = -1;
            s_ver.pcs_ver++;
        }
        elect();
        unlock();
    }
}

void hub_init(void)
{
    s_mu = xSemaphoreCreateRecursiveMutex();
    for (int i = 0; i < MAX_CONNS; i++) s_conns[i].fd = -1;
    s_stats = heap_caps_calloc(1, sizeof *s_stats, MALLOC_CAP_SPIRAM);
    s_hist = heap_caps_calloc(1, sizeof *s_hist, MALLOC_CAP_SPIRAM);
    // Agent frames parse into a few thousand small cJSON nodes: keep them out of internal RAM.
    static const cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks((cJSON_Hooks *)&hooks);
    s_agents = heap_caps_calloc(MAX_AGENTS, sizeof *s_agents, MALLOC_CAP_SPIRAM);
    s_full = heap_caps_calloc(1, FULL_MAX, MALLOC_CAP_SPIRAM);
    reset_data();
    settings_on_change(on_settings);
    xTaskCreatePinnedToCore(tick_task, "hub", 4096, NULL, 3, NULL, 0);
}
