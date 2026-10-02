// UI root: backdrop, top bar, swipeable pages, idle screen and the overlays (alerts, toasts,
// pairing, OTA). One LVGL timer pulls snapshots from the hub and pushes them into the pages.
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "ui_internal.h"

static const char *TAG = "ui";
const lv_image_dsc_t *bg_render(void);

static lv_obj_t *s_tv, *s_tiles[PAGE_COUNT], *s_tabs[PAGE_COUNT], *s_badge;
static lv_obj_t *s_host, *s_host_sub, *s_clock, *s_date, *s_wifi;
static lv_obj_t *s_idle, *s_idle_clock, *s_idle_date, *s_idle_status, *s_idle_dot, *s_idle_hint, *s_idle_wifi_btn;
static lv_obj_t *s_alert, *s_toasts, *s_pair, *s_ota, *s_ota_arc, *s_ota_pct;
static const lv_image_dsc_t *s_bg;

static stats_t *s_stats;
static history_t *s_hist;
static agent_t *s_agents;
// Just what the "finished" toast needs from the previous update: no second copy of the sessions.
static struct {
    char id[96];
    ag_state_t state;
} *s_prev_agents;
static int s_nagents, s_nprev;
static hub_versions_t s_seen;
static volatile uint32_t s_settings_changed;
static bool s_rebuild, s_force;
static int s_page;
static int64_t s_last_active_ms;
static int s_bl_target = -1;
static bool s_idle_dismissed;
static int64_t s_idle_dismissed_at;
static char s_alert_id[96];
static int s_alert_state = -1;
static char s_tz_applied[48];

/// The clock's time zone: the configured one, or with "auto" the active PC's.
static void apply_tz(void)
{
    settings_t st = settings_get();
    char tz[48];
    if (strcmp(st.tz, "auto")) snprintf(tz, sizeof tz, "%s", st.tz);
    else {
        hub_active_tz(tz);
        if (!tz[0]) return;  // keep whatever we had until a PC tells us
    }
    if (!strcmp(tz, s_tz_applied)) return;
    snprintf(s_tz_applied, sizeof s_tz_applied, "%s", tz);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "time zone %s", tz);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------- pages & top bar

int ui_current_page(void) { return s_page; }
lv_obj_t *ui_active_tile(void) { return s_tiles[s_page]; }
uint32_t ui_count_objs(lv_obj_t *o)
{
    uint32_t n = 1, c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c; i++) n += ui_count_objs(lv_obj_get_child(o, i));
    return n;
}

static void set_tab_styles(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool on = i == s_page;
        lv_obj_set_style_bg_opa(s_tabs[i], on ? (P.light ? 60 : 70) : 0, 0);
        uint32_t n = lv_obj_get_child_count(s_tabs[i]);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *c = lv_obj_get_child(s_tabs[i], k);
            if (c != s_badge) lv_obj_set_style_text_color(c, on ? (P.light ? lv_color_darken(P.accent, 60) : lv_color_lighten(P.accent, 70)) : P.text2, 0);
        }
    }
}

void ui_show_page(int page, bool anim)
{
    if (page < 0 || page >= PAGE_COUNT) return;
    s_page = page;
    lv_tileview_set_tile_by_index(s_tv, page, 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    set_tab_styles();
    s_force = true;
}

static void tv_changed(lv_event_t *e)
{
    lv_obj_t *t = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (s_tiles[i] == t && i != s_page) {
            s_page = i;
            set_tab_styles();
            s_force = true;
        }
}

static void swipe_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) ui_show_page(s_page + 1, false);
    else if (dir == LV_DIR_RIGHT) ui_show_page(s_page - 1, false);
}

static void tab_click(lv_event_t *e) { ui_show_page((int)(intptr_t)lv_event_get_user_data(e), false); }
static void host_click(lv_event_t *e) { (void)e; popup_sysinfo_open(); }
static void switch_click(lv_event_t *e) { ui_pc_switcher_open(lv_event_get_target(e)); }

static lv_obj_t *tab(lv_obj_t *bar, int page, const char *glyph, const char *text)
{
    lv_obj_t *t = ui_box(bar);
    lv_obj_set_height(t, 44);
    lv_obj_set_style_pad_hor(t, text ? 16 : 12, 0);
    lv_obj_set_style_radius(t, P.editorial ? 3 : 12, 0);
    lv_obj_set_style_bg_color(t, P.accent, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t, 8, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(t, 40, LV_STATE_PRESSED);
    ui_glyph(t, &icons_22, P.text2, glyph);
    if (text) ui_label(t, P.f16, P.text2, text);
    lv_obj_add_event_cb(t, tab_click, LV_EVENT_CLICKED, (void *)(intptr_t)page);
    return t;
}

static void build_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = ui_box(scr);
    lv_obj_set_size(bar, BOARD_W, TOPBAR_H);
    // The whole bar opens the PC's system info; the tabs and the PC switcher keep their own taps.
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, host_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *host = ui_box(bar);
    lv_obj_set_pos(host, 12, 8);
    lv_obj_set_size(host, 300, 48);
    lv_obj_set_style_radius(host, 12, 0);
    lv_obj_set_style_bg_color(host, P.card2, 0);
    lv_obj_set_style_bg_opa(host, 0, 0);
    lv_obj_set_style_bg_opa(host, 160, LV_STATE_PRESSED);
    lv_obj_add_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(host, host_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = ui_icon(host, &img_hw_pc_40);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
    s_host = ui_label(host, P.f_title, P.text, "");
    lv_obj_set_pos(s_host, 54, 2);
    ui_one_line(s_host);
    lv_obj_set_width(s_host, 210);
    s_host_sub = ui_label(host, P.f14, P.text3, "");
    lv_obj_set_pos(s_host_sub, 54, 26);
    ui_one_line(s_host_sub);
    lv_obj_set_width(s_host_sub, 210);
    // The name opens the PC's system info; the chevron, its own button, switches PCs.
    lv_obj_t *sw = ui_box(host);
    lv_obj_set_size(sw, 44, 48);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(sw, 12, 0);
    lv_obj_set_style_bg_color(sw, P.card2, 0);
    lv_obj_set_style_bg_opa(sw, 200, LV_STATE_PRESSED);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sw, switch_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *chev = ui_glyph(sw, &icons_22, P.text3, ICON_EXPAND);
    lv_obj_center(chev);

    lv_obj_t *tabs = ui_box(bar);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 4, 0);
    lv_obj_set_style_pad_all(tabs, 4, 0);
    lv_obj_set_style_radius(tabs, P.editorial ? 0 : 16, 0);
    lv_obj_set_style_bg_color(tabs, P.card, 0);
    lv_obj_set_style_bg_opa(tabs, P.card_opa, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, P.border, 0);
    s_tabs[PAGE_OVERVIEW] = tab(tabs, PAGE_OVERVIEW, ICON_PULSE, "Overview");
    s_tabs[PAGE_AGENTS] = tab(tabs, PAGE_AGENTS, ICON_AGENT, "Agents");
    s_tabs[PAGE_SYSTEM] = tab(tabs, PAGE_SYSTEM, ICON_CPU, "System");
    s_tabs[PAGE_SETTINGS] = tab(tabs, PAGE_SETTINGS, ICON_SETTINGS, NULL);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 30, 6);
    s_badge = ui_label(s_tabs[PAGE_AGENTS], P.f14, lv_color_white(), "");
    lv_obj_set_style_bg_color(s_badge, C_PERMISSION, 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_badge, 10, 0);
    lv_obj_set_style_pad_hor(s_badge, 7, 0);
    lv_obj_set_style_pad_ver(s_badge, 1, 0);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    s_clock = ui_label(bar, P.f_num, P.text, "");
    lv_obj_align(s_clock, LV_ALIGN_TOP_RIGHT, -16, 6);
    s_date = ui_label(bar, P.f14, P.text3, "");
    lv_obj_align(s_date, LV_ALIGN_TOP_RIGHT, -16, 38);
    s_wifi = ui_glyph(bar, &icons_22, P.text2, ICON_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_RIGHT, -112, 18);
}

// ---------------------------------------------------------------- idle screen

static void idle_wifi(lv_event_t *e) { ui_wifi_open(); }
static void idle_settings(lv_event_t *e)
{
    s_idle_dismissed = true;
    s_idle_dismissed_at = now_ms();
    lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
    ui_show_page(PAGE_SETTINGS, false);
}

static void pulse_cb(void *o, int32_t v)
{
    lv_obj_set_style_shadow_spread(o, v / 40, 0);
    lv_obj_set_style_shadow_opa(o, 255 - v, 0);
}

static void build_idle(lv_obj_t *scr)
{
    s_idle = ui_box(scr);
    lv_obj_set_size(s_idle, BOARD_W, BOARD_H);
    lv_obj_add_flag(s_idle, LV_OBJ_FLAG_CLICKABLE);
    if (s_bg) lv_image_set_src(lv_image_create(s_idle), s_bg);
    else {
        lv_obj_set_style_bg_color(s_idle, P.bg, 0);
        lv_obj_set_style_bg_opa(s_idle, LV_OPA_COVER, 0);
    }
    s_idle_clock = ui_label(s_idle, P.f_clock, P.text, "");
    lv_obj_set_pos(s_idle_clock, 64, 92);
    lv_obj_set_style_text_letter_space(s_idle_clock, -4, 0);
    s_idle_date = ui_label(s_idle, P.f_date, P.text2, "");
    lv_obj_set_pos(s_idle_date, 70, 262);

    lv_obj_t *cap = ui_box(s_idle);
    lv_obj_set_pos(cap, 70, 340);
    lv_obj_set_style_bg_color(cap, P.card, 0);
    lv_obj_set_style_bg_opa(cap, P.card_opa, 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_color(cap, P.border, 0);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(cap, 18, 0);
    lv_obj_set_style_pad_ver(cap, 10, 0);
    lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cap, 12, 0);
    s_idle_dot = ui_box(cap);
    lv_obj_set_size(s_idle_dot, 12, 12);
    lv_obj_set_style_radius(s_idle_dot, 6, 0);
    lv_obj_set_style_bg_opa(s_idle_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_idle_dot, P.accent, 0);
    lv_obj_set_style_shadow_color(s_idle_dot, P.accent, 0);
    lv_obj_set_style_shadow_width(s_idle_dot, 18, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_idle_dot);
    lv_anim_set_exec_cb(&a, pulse_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    s_idle_status = ui_label(cap, P.f20, P.text, "");

    s_idle_hint = ui_label(s_idle, P.f16, P.text3, "");
    lv_obj_set_pos(s_idle_hint, 72, 404);
    lv_obj_set_width(s_idle_hint, 520);
    lv_label_set_long_mode(s_idle_hint, LV_LABEL_LONG_WRAP);

    lv_obj_t *glow = ui_icon(s_idle, &img_glow_accent);
    lv_obj_set_pos(glow, 640, 100);
    lv_obj_set_style_image_recolor(glow, P.accent, 0);
    lv_obj_set_style_image_recolor_opa(glow, LV_OPA_COVER, 0);
    lv_obj_t *pc = ui_icon(s_idle, &img_hw_pc_120);
    lv_obj_align_to(pc, glow, LV_ALIGN_CENTER, 0, -6);

    s_idle_wifi_btn = ui_button(s_idle, ICON_WIFI, "Set up Wi-Fi", true);
    lv_obj_set_pos(s_idle_wifi_btn, 70, 470);
    lv_obj_add_event_cb(s_idle_wifi_btn, idle_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear = ui_button(s_idle, ICON_SETTINGS, NULL, false);
    lv_obj_set_size(gear, 56, 56);
    lv_obj_set_style_pad_hor(gear, 0, 0);
    lv_obj_align(gear, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
    lv_obj_add_event_cb(gear, idle_settings, LV_EVENT_CLICKED, NULL);
}

static void update_idle(bool show, const net_status_t *n, const struct tm *tm, bool synced)
{
    if (show && s_idle_dismissed && now_ms() - s_idle_dismissed_at < 120000 && board_idle_ms() < 60000) show = false;
    if (!show) {
        lv_obj_add_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    s_idle_dismissed = false;
    lv_obj_remove_flag(s_idle, LV_OBJ_FLAG_HIDDEN);
    settings_t st = settings_get();
    if (synced) {
        char buf[48];
        strftime(buf, sizeof buf, st.clock_24h ? "%H:%M" : "%l:%M", tm);
        lv_label_set_text(s_idle_clock, buf[0] == ' ' ? buf + 1 : buf);
        strftime(buf, sizeof buf, "%A, %e %B", tm);
        lv_label_set_text(s_idle_date, buf);
    } else {
        lv_label_set_text(s_idle_clock, "--:--");
        lv_label_set_text(s_idle_date, st.name);
    }
    if (n->state == NET_NO_CONFIG) {
        lv_label_set_text(s_idle_status, "Wi-Fi isn't set up");
        lv_label_set_text(s_idle_hint, "Connect this display to the same network as your PCs.");
        lv_obj_remove_flag(s_idle_wifi_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (n->state != NET_CONNECTED) {
        lv_label_set_text_fmt(s_idle_status, "Connecting to %s…", n->ssid);
        lv_label_set_text(s_idle_hint, "");
        if (n->state == NET_FAILED) lv_obj_remove_flag(s_idle_wifi_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_idle_wifi_btn, LV_OBJ_FLAG_HIDDEN);
        static pc_info_t pcs[MAX_PAIRED];
        int np = hub_pcs(pcs, MAX_PAIRED);
        lv_label_set_text(s_idle_status, np ? "Waiting for a PC" : "Ready to pair");
        if (np)
            lv_label_set_text_fmt(s_idle_hint, "%d paired PC%s, none online.  ·  %s.local  ·  %s", np, np == 1 ? "" : "s",
                                  PRIV(n->hostname, "deskhud"), PRIV(n->ip, "192.168.1.40"));
        else
            lv_label_set_text_fmt(s_idle_hint,
                                  "On your PC, run  deskhud install  and approve it here.\n%s.local  ·  %s", PRIV(n->hostname, "deskhud"), PRIV(n->ip, "192.168.1.40"));
    }
}

// ---------------------------------------------------------------- alert banner & toasts

static void alert_click(lv_event_t *e)
{
    page_agents_select(s_alert_id);
    ui_show_page(PAGE_AGENTS, false);
}

static void slide_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

static void show_alert(const agent_t *a)
{
    if (s_alert) lv_obj_delete(s_alert);
    lv_color_t c = state_color(a->state);
    s_alert = ui_box(lv_layer_top());
    lv_obj_set_size(s_alert, 720, 84);
    lv_obj_align(s_alert, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(s_alert, 20, 0);
    lv_obj_set_style_bg_color(s_alert, lv_color_mix(c, lv_color_hex(0x1a1206), 110), 0);
    lv_obj_set_style_bg_grad_color(s_alert, lv_color_mix(c, lv_color_hex(0x2a1030), 70), 0);
    lv_obj_set_style_bg_grad_dir(s_alert, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_alert, 1, 0);
    lv_obj_set_style_border_color(s_alert, lv_color_lighten(c, 60), 0);
    lv_obj_set_style_shadow_color(s_alert, c, 0);
    lv_obj_set_style_shadow_width(s_alert, 40, 0);
    lv_obj_set_style_shadow_opa(s_alert, 120, 0);
    lv_obj_set_style_pad_all(s_alert, 14, 0);
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_alert, alert_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = ui_icon(s_alert, a->state == AG_PERMISSION ? &img_fluent_shield_32 : &img_fluent_question_circle_32);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *logo = ui_icon(s_alert, agent_logo(a->agent, false));
    lv_obj_set_pos(logo, 52, 0);
    lv_obj_t *t = ui_label(s_alert, P.f18, lv_color_white(), "");
    lv_label_set_text_fmt(t, "%s %s", a->project[0] ? a->project : a->agent,
                          a->state == AG_PERMISSION ? "needs your permission" : "has a question for you");
    lv_obj_set_pos(t, 82, 0);
    lv_obj_t *p = ui_label(s_alert, P.f16, lv_color_hex(0xfde7d2), a->prompt[0] ? a->prompt : "Open the terminal to respond");
    lv_obj_set_pos(p, 52, 30);
    lv_obj_set_width(p, 600);
    ui_one_line(p);
    lv_obj_t *go = ui_glyph(s_alert, &icons_28, lv_color_white(), ICON_RIGHT);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, s_alert);
    lv_anim_set_exec_cb(&an, slide_y);
    lv_anim_set_values(&an, -100, 10);
    lv_anim_set_duration(&an, 350);
    lv_anim_set_path_cb(&an, lv_anim_path_overshoot);
    lv_anim_start(&an);
}

static void hide_alert(void)
{
    if (s_alert) {
        lv_obj_delete(s_alert);
        s_alert = NULL;
    }
    s_alert_id[0] = 0;
    s_alert_state = -1;
}

static void show_toast(const toast_t *t)
{
    lv_color_t c = t->level == 2 ? C_PERMISSION : t->level == 1 ? C_QUESTION : P.accent;
    lv_obj_t *card = ui_card(s_toasts, 0, 0, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_mix(c, P.border, 140), 0);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_opa(card, 110, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    const lv_image_dsc_t *ic = t->level == 2 ? &img_fluent_warning_22 : t->level == 1 ? &img_fluent_warning_22
                                                                                         : &img_fluent_checkmark_circle_22;
    ui_icon(card, ic);
    lv_obj_t *ti = ui_label(card, P.f16, P.text, t->title);
    lv_obj_set_pos(ti, 32, 0);
    lv_obj_set_width(ti, 276);
    ui_one_line(ti);
    if (t->body[0]) {
        lv_obj_t *b = ui_label(card, P.f14, P.text2, t->body);
        lv_obj_set_pos(b, 32, 24);
        lv_obj_set_width(b, 276);
        // Two lines at most: a notice, not the result (that's on the Agents page).
        lv_obj_set_height(b, lv_font_get_line_height(P.f14) * 2);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
    }
    lv_obj_fade_in(card, 200, 0);
    lv_obj_fade_out(card, 300, t->ttl * 1000);
    lv_obj_delete_delayed(card, t->ttl * 1000 + 320);
    if (lv_obj_get_child_count(s_toasts) > 2) lv_obj_delete(lv_obj_get_child(s_toasts, 0));
}

// ---------------------------------------------------------------- pairing & OTA

static void pair_allow(lv_event_t *e) { hub_pair_respond(true); }
static void pair_deny(lv_event_t *e) { hub_pair_respond(false); }

static void update_pairing(void)
{
    char host[40], code[7];
    bool pending = hub_pair_pending(host, code);
    if (!pending) {
        if (s_pair) {
            lv_obj_delete(s_pair);
            s_pair = NULL;
        }
        return;
    }
    if (s_pair) return;
    s_pair = ui_box(lv_layer_top());
    lv_obj_set_size(s_pair, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(s_pair, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pair, 170, 0);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *m = ui_card(s_pair, 0, 0, 620, 380);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m, 26, 0);
    lv_obj_center(m);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(m, 10, 0);
    lv_obj_set_style_pad_all(m, 28, 0);
    ui_icon(m, &img_hw_pc_120);
    lv_obj_t *t = ui_label(m, P.f_head, P.text, "");
    lv_label_set_text_fmt(t, "%s wants to connect", host);
    ui_label(m, P.f16, P.text2, "It will be able to show its stats here and update this display.");
    lv_obj_t *btns = ui_box(m);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_set_style_pad_top(btns, 10, 0);
    lv_obj_t *d = ui_button(btns, ICON_CLOSE, "Deny", false);
    lv_obj_set_width(d, 200);
    lv_obj_add_event_cb(d, pair_deny, LV_EVENT_CLICKED, NULL);
    lv_obj_t *a = ui_button(btns, ICON_DONE, "Allow", true);
    lv_obj_set_width(a, 200);
    lv_obj_add_event_cb(a, pair_allow, LV_EVENT_CLICKED, NULL);
    lv_obj_fade_in(s_pair, 200, 0);
}

static void update_ota(void)
{
    ota_progress_t o = server_ota_progress();
    if (!o.active) {
        if (s_ota) {
            lv_obj_delete(s_ota);
            s_ota = NULL;
            lv_obj_remove_flag(s_tv, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!s_ota) {
        // Opaque: nothing else redraws while the flash is being written.
        s_ota = ui_box(lv_layer_top());
        lv_obj_set_size(s_ota, BOARD_W, BOARD_H);
        lv_obj_set_style_bg_color(s_ota, P.bg, 0);
        lv_obj_set_style_bg_opa(s_ota, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_ota, LV_OBJ_FLAG_CLICKABLE);
        if (s_bg) lv_image_set_src(lv_image_create(s_ota), s_bg);
        lv_obj_t *glow = ui_icon(s_ota, &img_glow_accent);
        lv_obj_align(glow, LV_ALIGN_CENTER, 0, -70);
        lv_obj_set_style_image_recolor(glow, P.accent, 0);
        lv_obj_set_style_image_recolor_opa(glow, LV_OPA_COVER, 0);
        s_ota_arc = lv_arc_create(s_ota);
        lv_obj_set_size(s_ota_arc, 184, 184);
        lv_obj_align(s_ota_arc, LV_ALIGN_CENTER, 0, -70);
        lv_arc_set_bg_angles(s_ota_arc, 135, 45);
        lv_arc_set_range(s_ota_arc, 0, 100);
        lv_obj_remove_style(s_ota_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_ota_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_ota_arc, 16, 0);
        lv_obj_set_style_arc_width(s_ota_arc, 16, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_ota_arc, P.light ? lv_color_hex(0xe3e8f2) : lv_color_hex(0x1f2740), 0);
        lv_obj_set_style_arc_image_src(s_ota_arc, &img_ring_cpu, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_ota_arc, true, LV_PART_INDICATOR);
        s_ota_pct = ui_label(s_ota, P.f_num_xl, P.text, "");
        lv_obj_align_to(s_ota_pct, s_ota_arc, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *icon = ui_icon(s_ota, &img_hw_pc_40);
        lv_obj_align(icon, LV_ALIGN_CENTER, -170, 82);
        lv_obj_t *t = ui_label(s_ota, &inter_semi_28, P.text, "");
        lv_label_set_text_fmt(t, "Installing update from %s", o.from);
        lv_obj_align_to(t, icon, LV_ALIGN_OUT_RIGHT_MID, 14, 0);
        lv_obj_t *sub = ui_label(s_ota, P.f16, P.text3, "Keep the display plugged in. It restarts by itself when done.");
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 130);
        lv_obj_add_flag(s_tv, LV_OBJ_FLAG_HIDDEN);  // stop drawing the dashboard underneath
    }
    int p = o.percent < 0 ? 0 : o.percent;
    lv_arc_set_value(s_ota_arc, p);
    lv_label_set_text_fmt(s_ota_pct, "%d%%", p);
    lv_obj_align_to(s_ota_pct, s_ota_arc, LV_ALIGN_CENTER, 0, 0);
}

// ---------------------------------------------------------------- build / tick

static void on_settings(uint32_t changed) { __atomic_fetch_or(&s_settings_changed, changed, __ATOMIC_SEQ_CST); }
void ui_request_rebuild(void) { s_rebuild = true; }

void ui_set_privacy(bool on)
{
    if (g_privacy == on) return;
    g_privacy = on;
    s_rebuild = true;
}

static void build(void)
{
    theme_load();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_clean(lv_layer_top());
    s_alert = s_pair = s_ota = NULL;
    s_alert_id[0] = 0;
    s_alert_state = -1;
    lv_obj_set_style_bg_color(scr, P.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    int64_t t0 = now_ms();
    s_bg = bg_render();
    ESP_LOGI(TAG, "backdrop rendered in %d ms", (int)(now_ms() - t0));
    if (s_bg) lv_image_set_src(lv_image_create(scr), s_bg);

    build_topbar(scr);
    s_tv = lv_tileview_create(scr);
    lv_obj_set_pos(s_tv, 0, TOPBAR_H);
    lv_obj_set_size(s_tv, BOARD_W, PAGE_H);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < PAGE_COUNT - 1 ? LV_DIR_RIGHT : 0);
        s_tiles[i] = lv_tileview_add_tile(s_tv, i, 0, dir);
        lv_obj_set_scrollbar_mode(s_tiles[i], LV_SCROLLBAR_MODE_OFF);
    }
    lv_obj_add_event_cb(s_tv, tv_changed, LV_EVENT_VALUE_CHANGED, NULL);
    // Dragging a full page renders ~5 fps on this chip: switch instantly on a swipe instead.
    lv_obj_remove_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < PAGE_COUNT; i++) lv_obj_add_event_cb(s_tiles[i], swipe_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_tv, swipe_cb, LV_EVENT_GESTURE, NULL);
    page_overview_create(s_tiles[PAGE_OVERVIEW]);
    page_agents_create(s_tiles[PAGE_AGENTS]);
    page_system_create(s_tiles[PAGE_SYSTEM]);
    page_settings_create(s_tiles[PAGE_SETTINGS]);
    build_idle(scr);

    s_toasts = ui_box(lv_layer_top());
    lv_obj_set_pos(s_toasts, BOARD_W - 16 - 340, TOPBAR_H + 8);
    lv_obj_set_flex_flow(s_toasts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_toasts, 10, 0);
    lv_obj_remove_flag(s_toasts, LV_OBJ_FLAG_CLICKABLE);

    ui_show_page(s_page, false);
    memset(&s_seen, 0, sizeof s_seen);  // force a full refresh
    s_force = true;
}

static void update_brightness(bool alert)
{
    settings_t st = settings_get();
    int64_t now = now_ms();
    bool busy = s_seen.has_active || alert || s_pair || s_ota;
    if (busy || board_idle_ms() < 1000) s_last_active_ms = now;
    // dim_after >= 1 day means "never": the display stays at full brightness around the clock.
    bool dim = st.dim_after < 86400 && (now - s_last_active_ms) >= (int64_t)st.dim_after * 1000;
    int target = dim ? st.dim_brightness : st.brightness;
    if (target != s_bl_target) {
        bool waking = target > s_bl_target;
        s_bl_target = target;
        board_set_brightness(target, waking ? 300 : 1500);
    }
}

static void tick(lv_timer_t *timer)
{
    // Atomic swap: a change set by another task between a read and a clear would be lost.
    uint32_t ch = __atomic_exchange_n(&s_settings_changed, 0, __ATOMIC_SEQ_CST);
    if (ch) {
        if (ch & SET_THEME) s_rebuild = true;
        if (ch & SET_CLOCK) s_tz_applied[0] = 0;  // re-evaluate below
        if (ch & SET_NAME) net_update_mdns();
        if (ch & (SET_DISPLAY | SET_THEME)) s_bl_target = -1;
        if (ch & (SET_ROUTING | SET_NAME)) s_force = true;
    }
    if (s_rebuild) {
        s_rebuild = false;
        build();
    }

    int req = hub_take_page_request();
    if (req == 10) popup_sysinfo_open();
    else if (req == 11) popup_procs_open();
    else if (req >= 0) ui_show_page(req, false);
    hub_versions_t v = hub_versions();
    ui_data_t d = {.stats = s_stats, .hist = s_hist, .agents = s_agents};
    d.stats_changed = v.stats_ver != s_seen.stats_ver || s_force;
    d.agents_changed = v.agents_ver != s_seen.agents_ver || s_force;
    d.pcs_changed = v.pcs_ver != s_seen.pcs_ver || s_force;
    if (d.stats_changed) hub_stats(s_stats, s_hist);
    if (d.agents_changed) {
        for (int i = 0; i < s_nagents; i++) {
            memcpy(s_prev_agents[i].id, s_agents[i].id, sizeof s_prev_agents[i].id);
            s_prev_agents[i].state = s_agents[i].state;
        }
        s_nprev = s_nagents;
        s_nagents = hub_agents(s_agents, MAX_AGENTS);
    }
    d.nagents = s_nagents;
    s_seen = v;
    s_force = false;

    apply_tz();
    net_status_t n = net_status();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    d.now = n.time_synced ? now : 0;

    // top bar
    if (d.pcs_changed) {
        if (v.has_active) {
            lv_label_set_text(s_host, PRIV(v.active_host, "workstation"));
            static pc_info_t pcs[MAX_PAIRED];
            int np = hub_pcs(pcs, MAX_PAIRED), online = 0;
            for (int i = 0; i < np; i++) online += pcs[i].online;
            if (online > 1) lv_label_set_text_fmt(s_host_sub, "%s  ·  %d PCs online", PRIV(v.active_os, "Linux"), online);
            else lv_label_set_text(s_host_sub, PRIV(v.active_os, "Linux"));
        } else {
            lv_label_set_text(s_host, settings_get().name);
            lv_label_set_text(s_host_sub, "No PC connected");
        }
    }
    if (n.time_synced) {
        char buf[32];
        settings_t st = settings_get();
        strftime(buf, sizeof buf, st.clock_24h ? "%H:%M" : "%l:%M %p", &tm);
        lv_label_set_text(s_clock, buf[0] == ' ' ? buf + 1 : buf);
        strftime(buf, sizeof buf, "%a %e %b", &tm);
        lv_label_set_text(s_date, buf);
    }
    lv_obj_set_style_text_color(s_wifi, n.state != NET_CONNECTED ? C_ERROR : n.rssi > -67 ? P.text2 : n.rssi > -75 ? C_QUESTION : C_PERMISSION, 0);
    lv_label_set_text(s_wifi, n.state == NET_CONNECTED ? ICON_WIFI : ICON_WIFI_OFF);

    // agents needing attention: badge, banner, auto page
    const agent_t *need = NULL;
    int waiting = 0;
    for (int i = 0; i < s_nagents; i++)
        if (s_agents[i].state == AG_PERMISSION || s_agents[i].state == AG_QUESTION) {
            waiting++;
            if (!need) need = &s_agents[i];
        }
    if (waiting) {
        lv_label_set_text_fmt(s_badge, "%d", waiting);
        lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    }
    if (d.agents_changed) {
        settings_t st = settings_get();
        if (need && (strcmp(need->id, s_alert_id) || (int)need->state != s_alert_state)) {
            snprintf(s_alert_id, sizeof s_alert_id, "%s", need->id);
            s_alert_state = need->state;
            if (s_page != PAGE_AGENTS) show_alert(need);
            if (st.auto_page && s_page != PAGE_SETTINGS) {
                page_agents_select(need->id);
                ui_show_page(PAGE_AGENTS, false);
                hide_alert();
                snprintf(s_alert_id, sizeof s_alert_id, "%s", need->id);
                s_alert_state = need->state;
            }
            if (st.wake_on_alert) s_last_active_ms = now_ms();
        } else if (!need && s_alert_id[0]) {
            hide_alert();
        }
        // finished turns: a quiet toast
        for (int i = 0; i < s_nagents; i++) {
            if (s_agents[i].state != AG_DONE) continue;
            // Already on screen as the Agents page's result.
            if (s_page == PAGE_AGENTS && page_agents_showing(s_agents[i].id)) continue;
            for (int k = 0; k < s_nprev; k++)
                if (!strcmp(s_prev_agents[k].id, s_agents[i].id) && s_prev_agents[k].state == AG_WORKING) {
                    toast_t t = {.level = 0, .ttl = 6};
                    snprintf(t.title, sizeof t.title, "%s finished", s_agents[i].project[0] ? s_agents[i].project : s_agents[i].agent);
                    snprintf(t.body, sizeof t.body, "%s", s_agents[i].summary);
                    show_toast(&t);
                }
        }
    }
    if (s_alert && s_page == PAGE_AGENTS) {
        lv_obj_delete(s_alert);
        s_alert = NULL;
    }

    toast_t t;
    while (hub_next_toast(&t)) show_toast(&t);

    switch (s_page) {
    case PAGE_OVERVIEW: page_overview_update(&d); break;
    case PAGE_AGENTS: page_agents_update(&d); break;
    case PAGE_SYSTEM: page_system_update(&d); break;
    case PAGE_SETTINGS: page_settings_update(&d); break;
    }
    update_idle(!v.has_active, &n, &tm, n.time_synced);
    update_pairing();
    update_ota();
    update_brightness(waiting > 0 && settings_get().wake_on_alert);
}

volatile int64_t g_bench_until;  // console "bench": keep the whole screen dirty until then

static void bench_cb(lv_timer_t *t)
{
    if (esp_timer_get_time() < g_bench_until) lv_obj_invalidate(lv_screen_active());
}

void ui_init(void)
{
    s_stats = heap_caps_calloc(1, sizeof *s_stats, MALLOC_CAP_SPIRAM);
    s_hist = heap_caps_calloc(1, sizeof *s_hist, MALLOC_CAP_SPIRAM);
    s_agents = heap_caps_calloc(MAX_AGENTS, sizeof(agent_t), MALLOC_CAP_SPIRAM);
    s_prev_agents = heap_caps_calloc(MAX_AGENTS, sizeof *s_prev_agents, MALLOC_CAP_SPIRAM);
    settings_on_change(on_settings);
    settings_t st = settings_get();
    s_page = !strcmp(st.page, "agents") ? PAGE_AGENTS : !strcmp(st.page, "system") ? PAGE_SYSTEM : PAGE_OVERVIEW;
    s_last_active_ms = now_ms();
    board_lock();
    build();
    lv_timer_create(tick, 250, NULL);
    lv_timer_create(bench_cb, 5, NULL);
    board_unlock();
}
