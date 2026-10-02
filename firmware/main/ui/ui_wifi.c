// Wi-Fi setup, rename and PC-switcher overlays.
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "ui_internal.h"

// ---------------------------------------------------------------- Wi-Fi

#define MAX_APS 20
static net_ap_t s_aps[MAX_APS];
static volatile int s_naps = -1;  // -1 scanning
static lv_obj_t *s_wifi, *s_list, *s_ssid, *s_pass, *s_kb, *s_status;
static lv_timer_t *s_poll;

static void scan_task(void *arg)
{
    net_ap_t *tmp = heap_caps_malloc(sizeof(net_ap_t) * MAX_APS, MALLOC_CAP_SPIRAM);
    int n = tmp ? net_scan(tmp, MAX_APS) : 0;
    if (tmp) {
        memcpy(s_aps, tmp, sizeof(net_ap_t) * n);
        heap_caps_free(tmp);
    }
    s_naps = n;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    s_naps = -1;
    xTaskCreate(scan_task, "scan", 4096, NULL, 3, NULL);
}

static void ap_click(lv_event_t *e)
{
    net_ap_t *ap = lv_event_get_user_data(e);
    lv_textarea_set_text(s_ssid, ap->ssid);
    lv_textarea_set_text(s_pass, "");
    lv_keyboard_set_textarea(s_kb, s_pass);
    lv_obj_add_state(s_pass, LV_STATE_FOCUSED);
    lv_obj_remove_state(s_ssid, LV_STATE_FOCUSED);
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    if (s_naps < 0) {
        lv_obj_t *sp = lv_spinner_create(s_list);
        lv_obj_set_size(sp, 48, 48);
        lv_obj_set_style_arc_color(sp, P.accent, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(sp, 5, 0);
        ui_label(s_list, P.f16, P.text3, "Looking for networks…");
        return;
    }
    if (!s_naps) ui_label(s_list, P.f16, P.text3, "No networks found");
    for (int i = 0; i < s_naps; i++) {
        lv_obj_t *r = ui_box(s_list);
        lv_obj_set_size(r, LV_PCT(100), 52);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_bg_color(r, P.card2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, P.border, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        int bars = s_aps[i].rssi > -55 ? 3 : s_aps[i].rssi > -70 ? 2 : 1;
        lv_obj_t *g = ui_glyph(r, &icons_22, bars == 3 ? C_DONE : bars == 2 ? C_QUESTION : C_PERMISSION, ICON_WIFI);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *l = ui_label(r, P.f18, P.text, s_aps[i].ssid);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 34, 0);
        ui_one_line(l);
        lv_obj_set_width(l, 300);
        if (s_aps[i].secure) {
            lv_obj_t *k = ui_glyph(r, &icons_18, P.text3, ICON_KEY);
            lv_obj_align(k, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        lv_obj_add_event_cb(r, ap_click, LV_EVENT_CLICKED, &s_aps[i]);
    }
}

static void poll_cb(lv_timer_t *t)
{
    static int shown = -2;
    if (s_naps != shown) {
        shown = s_naps;
        fill_list();
    }
    net_status_t n = net_status();
    if (lv_obj_has_flag(s_status, LV_OBJ_FLAG_USER_1)) {  // a connect was requested
        if (n.state == NET_CONNECTED && !strcmp(n.ssid, lv_textarea_get_text(s_ssid))) {
            lv_label_set_text_fmt(s_status, "Connected  ·  %s", n.ip);
            lv_obj_set_style_text_color(s_status, C_DONE, 0);
        } else if (n.state == NET_FAILED) {
            lv_label_set_text(s_status, "Couldn't connect. Check the password.");
            lv_obj_set_style_text_color(s_status, C_ERROR, 0);
        }
    }
}

static void wifi_close(lv_event_t *e)
{
    lv_timer_delete(s_poll);
    s_poll = NULL;
    lv_obj_delete_async(s_wifi);
}

static void connect_cb(lv_event_t *e)
{
    const char *ssid = lv_textarea_get_text(s_ssid);
    if (!ssid[0]) return;
    net_connect(ssid, lv_textarea_get_text(s_pass));
    lv_label_set_text_fmt(s_status, "Connecting to %s…", ssid);
    lv_obj_set_style_text_color(s_status, P.text2, 0);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_USER_1);
}

static void rescan_cb(lv_event_t *e) { start_scan(); }

static void ta_focus(lv_event_t *e) { lv_keyboard_set_textarea(s_kb, lv_event_get_target(e)); }

static void show_pass(lv_event_t *e)
{
    lv_textarea_set_password_mode(s_pass, !lv_textarea_get_password_mode(s_pass));
}

static lv_obj_t *field(lv_obj_t *parent, int x, int y, int w, const char *label, const char *placeholder)
{
    lv_obj_t *l = ui_label(parent, P.f16, P.text2, label);
    lv_obj_set_pos(l, x, y);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_pos(ta, x, y + 26);
    lv_obj_set_width(ta, w);
    lv_obj_set_style_text_font(ta, P.f20, 0);
    lv_obj_set_style_text_color(ta, P.text, 0);
    lv_obj_set_style_bg_color(ta, P.light ? lv_color_white() : lv_color_hex(0x0d1220), 0);
    lv_obj_set_style_border_color(ta, P.border, 0);
    lv_obj_set_style_border_color(ta, P.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_pad_all(ta, 12, 0);
    lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, P.light ? lv_color_hex(0xdfe4ee) : lv_color_hex(0x0a0e18), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb, 8, 0);
    lv_obj_set_style_pad_gap(kb, 6, 0);
    lv_obj_set_style_bg_color(kb, P.card2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, P.border, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, P.accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, P.text, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, P.f20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
}

void ui_wifi_open(void)
{
    s_wifi = ui_box(lv_layer_top());
    lv_obj_set_size(s_wifi, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(s_wifi, P.bg, 0);
    lv_obj_set_style_bg_opa(s_wifi, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = ui_icon(s_wifi, &img_hw_wifi_28);
    lv_obj_set_pos(icon, 24, 20);
    lv_obj_t *t = ui_label(s_wifi, &inter_semi_28, P.text, "Wi-Fi");
    lv_obj_set_pos(t, 62, 16);
    lv_obj_t *close = ui_button(s_wifi, ICON_CLOSE, NULL, false);
    lv_obj_set_pos(close, BOARD_W - 24 - 52, 10);
    lv_obj_set_width(close, 52);
    lv_obj_set_style_pad_hor(close, 0, 0);
    lv_obj_add_event_cb(close, wifi_close, LV_EVENT_CLICKED, NULL);

    s_list = ui_box(s_wifi);
    lv_obj_set_pos(s_list, 24, 70);
    lv_obj_set_size(s_list, 440, 262);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_t *rs = ui_button(s_wifi, ICON_RESTART, "Scan", false);
    lv_obj_set_pos(rs, 340, 10);
    lv_obj_add_event_cb(rs, rescan_cb, LV_EVENT_CLICKED, NULL);

    net_status_t n = net_status();
    s_ssid = field(s_wifi, 496, 66, 504, "Network", "Network name");
    if (n.ssid[0]) lv_textarea_set_text(s_ssid, n.ssid);
    s_pass = field(s_wifi, 496, 150, 432, "Password", "Password");
    lv_textarea_set_password_mode(s_pass, true);
    lv_obj_t *eye = ui_button(s_wifi, ICON_EYE, NULL, false);
    lv_obj_set_pos(eye, 940, 176);
    lv_obj_set_size(eye, 60, 50);
    lv_obj_set_style_pad_hor(eye, 0, 0);
    lv_obj_add_event_cb(eye, show_pass, LV_EVENT_CLICKED, NULL);
    lv_obj_t *go = ui_button(s_wifi, ICON_LINK, "Connect", true);
    lv_obj_set_pos(go, 496, 250);
    lv_obj_add_event_cb(go, connect_cb, LV_EVENT_CLICKED, NULL);
    s_status = ui_label(s_wifi, P.f16, P.text2, "");
    lv_obj_set_pos(s_status, 660, 266);

    s_kb = lv_keyboard_create(s_wifi);
    lv_obj_set_size(s_kb, BOARD_W, 256);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_keyboard(s_kb);
    lv_keyboard_set_textarea(s_kb, n.ssid[0] ? s_pass : s_ssid);
    lv_obj_add_state(n.ssid[0] ? s_pass : s_ssid, LV_STATE_FOCUSED);

    s_poll = lv_timer_create(poll_cb, 250, NULL);
    start_scan();
}

// ---------------------------------------------------------------- rename

static lv_obj_t *s_rename;

static void rename_done(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_user_data(e);
    if (code == LV_EVENT_READY) {
        const char *name = lv_textarea_get_text(ta);
        if (name[0]) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "name", name);
            settings_apply_json(p);
            cJSON_Delete(p);
        }
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) lv_obj_delete_async(s_rename);
}

void ui_rename_open(void)
{
    s_rename = ui_box(lv_layer_top());
    lv_obj_set_size(s_rename, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(s_rename, P.bg, 0);
    lv_obj_set_style_bg_opa(s_rename, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_rename, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ta = field(s_rename, 212, 120, 600, "Display name", "DeskHUD");
    lv_textarea_set_max_length(ta, 31);
    lv_textarea_set_text(ta, settings_get().name);
    lv_obj_t *kb = lv_keyboard_create(s_rename);
    lv_obj_set_size(kb, BOARD_W, 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_keyboard(kb);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, rename_done, LV_EVENT_READY, ta);
    lv_obj_add_event_cb(kb, rename_done, LV_EVENT_CANCEL, ta);
}

// ---------------------------------------------------------------- PC switcher

static lv_obj_t *s_switcher;
static pc_info_t s_sw_pcs[MAX_PAIRED];

static void sw_pick(lv_event_t *e)
{
    pc_info_t *pc = lv_event_get_user_data(e);
    if (pc->online) hub_activate(pc->pc_id, false);
    lv_obj_delete_async(s_switcher);
}

static void sw_manage(lv_event_t *e)
{
    lv_obj_delete_async(s_switcher);
    ui_show_page(PAGE_SETTINGS, false);
}

static void sw_backdrop(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_switcher) lv_obj_delete_async(s_switcher);
}

void ui_pc_switcher_open(lv_obj_t *anchor)
{
    s_switcher = ui_box(lv_layer_top());
    lv_obj_set_size(s_switcher, BOARD_W, BOARD_H);
    lv_obj_add_flag(s_switcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_switcher, sw_backdrop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu = ui_card(s_switcher, 12, TOPBAR_H - 4, 380, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(menu, 40, 0);
    lv_obj_set_style_shadow_opa(menu, 140, 0);
    lv_obj_set_style_shadow_color(menu, lv_color_black(), 0);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(menu, 6, 0);
    lv_obj_set_style_pad_all(menu, 10, 0);
    int n = hub_pcs(s_sw_pcs, MAX_PAIRED);
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = ui_box(menu);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 10, 0);
        lv_obj_set_style_bg_color(r, s_sw_pcs[i].active ? lv_color_mix(P.accent, P.card, 60) : P.card2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, P.border, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *dot = ui_box(r);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, s_sw_pcs[i].online ? C_DONE : C_IDLE, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *h = ui_label(r, P.f18, s_sw_pcs[i].online ? P.text : P.text3, PRIV(s_sw_pcs[i].host, "workstation"));
        lv_obj_align(h, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *st = ui_label(r, P.f14, P.text3,
                                s_sw_pcs[i].active ? "On screen" : s_sw_pcs[i].online ? "Tap to show" : "Offline");
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(r, sw_pick, LV_EVENT_CLICKED, &s_sw_pcs[i]);
    }
    if (!n) ui_label(menu, P.f16, P.text3, "No PCs paired yet");
    lv_obj_t *m = ui_button(menu, ICON_SETTINGS, "Manage PCs", false);
    lv_obj_set_width(m, LV_PCT(100));
    lv_obj_add_event_cb(m, sw_manage, LV_EVENT_CLICKED, NULL);
}
