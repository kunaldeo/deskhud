// Serial console on the USB UART (115200): provisioning and diagnostics without a network.
//   wifi <ssid> [password]   save credentials and connect (quote values with spaces)
//   status                   network / memory / firmware summary
//   pcs                      paired PCs
//   forget <pc_id|all>       unpair
//   reboot | factory-reset
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net.h"
#include "settings.h"
#include "board.h"
#include "esp_lv_adapter.h"
#include "hub.h"

/// Splits on spaces, honoring "double quotes". Returns argc.
static int split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

static void run(char *line)
{
    char *argv[4];
    int argc = split(line, argv, 4);
    if (!argc) return;
    if (!strcmp(argv[0], "wifi") && argc >= 2) {
        net_connect(argv[1], argc > 2 ? argv[2] : "");
        printf("ok: connecting to %s\n", argv[1]);
    } else if (!strcmp(argv[0], "status")) {
        net_status_t n = net_status();
        static const char *st[] = {"no-config", "connecting", "connected", "failed"};
        printf("fw %s | wifi %s ssid=%s ip=%s rssi=%d host=%s.local | heap %u psram %u\n",
               esp_app_get_description()->version, st[n.state], n.ssid, n.ip, n.rssi, n.hostname,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (!strcmp(argv[0], "pcs")) {
        paired_pc_t pcs[MAX_PAIRED];
        int n = settings_pc_list(pcs, MAX_PAIRED);
        for (int i = 0; i < n; i++) printf("%s  %s\n", pcs[i].pc_id, pcs[i].host);
        printf("%d paired\n", n);
    } else if (!strcmp(argv[0], "forget") && argc == 2) {
        paired_pc_t pcs[MAX_PAIRED];
        int n = settings_pc_list(pcs, MAX_PAIRED);
        for (int i = 0; i < n; i++)
            if (!strcmp(argv[1], "all") || !strcmp(argv[1], pcs[i].pc_id)) settings_pc_forget(pcs[i].pc_id);
        printf("ok\n");
    } else if (!strcmp(argv[0], "fps")) {
        // Frames actually flushed per second, sampled for 8 s (swipe around meanwhile).
        esp_lv_adapter_fps_stats_enable(board_display(), true);
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            uint32_t fps = 0;
            esp_lv_adapter_get_fps(board_display(), &fps);
            printf("fps %lu\n", (unsigned long)fps);
        }
    } else if (!strcmp(argv[0], "bench")) {
        // Continuous full-screen redraws for 6 s, measured by the adapter's flush counter.
        extern volatile int64_t g_bench_until;
        esp_lv_adapter_fps_stats_enable(board_display(), true);
        esp_lv_adapter_fps_stats_reset(board_display());
        g_bench_until = esp_timer_get_time() + 6000000;
        vTaskDelay(pdMS_TO_TICKS(6200));
        uint32_t fps = 0;
        esp_lv_adapter_get_fps(board_display(), &fps);
        printf("bench fps %lu\n", (unsigned long)fps);
    } else if (!strcmp(argv[0], "bg") && argc == 2) {
        // Toggle the backdrop image (first child of the screen) for profiling.
        board_lock();
        lv_obj_t *bg = lv_obj_get_child(lv_screen_active(), 0);
        if (atoi(argv[1])) lv_obj_remove_flag(bg, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);
        board_unlock();
        printf("ok\n");
    } else if (!strcmp(argv[0], "otatest")) {
        extern void server_ota_simulate(void);
        server_ota_simulate();
        printf("ok\n");
    } else if (!strcmp(argv[0], "objs")) {
        board_lock();
        uint32_t scr = lv_obj_get_child_count_by_type(lv_screen_active(), NULL);
        extern uint32_t ui_count_objs(lv_obj_t *);
        uint32_t n = ui_count_objs(lv_screen_active()), top = ui_count_objs(lv_layer_top());
        uint32_t anims = lv_anim_count_running(), timers = 0;
        for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) timers++;
        board_unlock();
        printf("objs screen=%lu top=%lu anims=%lu timers=%lu psram=%u (%lu direct)\n", (unsigned long)n, (unsigned long)top,
               (unsigned long)anims, (unsigned long)timers, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)scr);
    } else if (!strcmp(argv[0], "hide") && argc == 3) {
        // hide <path> <0|1>: path "s" = screen children, "t" = current tile's children; e.g. "t3"
        extern lv_obj_t *ui_active_tile(void);
        board_lock();
        lv_obj_t *parent = argv[1][0] == 't' ? ui_active_tile() : lv_screen_active();
        lv_obj_t *o = lv_obj_get_child(parent, atoi(argv[1] + 1));
        if (o) {
            if (atoi(argv[2])) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
        board_unlock();
        printf("ok %s\n", o ? "" : "missing");
    } else if (!strcmp(argv[0], "privacy") && argc == 2) {
        extern bool g_privacy;
        extern void ui_request_rebuild(void);
        g_privacy = atoi(argv[1]);
        ui_request_rebuild();
        printf("ok\n");
    } else if (!strcmp(argv[0], "perf") && argc == 2) {
        extern int g_perf;
        extern void ui_request_rebuild(void);
        g_perf = atoi(argv[1]);
        ui_request_rebuild();
        printf("ok\n");
    } else if (!strcmp(argv[0], "page") && argc == 2) {
        extern void hub_request_page(int);
        hub_request_page(atoi(argv[1]));
    } else if (!strcmp(argv[0], "reboot")) {
        esp_restart();
    } else if (!strcmp(argv[0], "factory-reset")) {
        settings_factory_reset();
        esp_restart();
    } else {
        printf("commands: wifi <ssid> [pass] | status | pcs | forget <pc_id|all> | reboot | factory-reset\n");
    }
}

static void console_task(void *arg)
{
    char line[160];
    int len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) run(line);
            len = 0;
        } else if (len < (int)sizeof line - 1) {
            line[len++] = c;
        }
    }
}

void console_start(void)
{
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
}
