#include <stdio.h>
#include <string.h>
#include "board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "net.h"
#include "settings.h"
#include "ui_internal.h"

#define COL_W 484
static lv_obj_t *s_bright_val, *s_dim_val, *s_pcs_box, *s_wifi_info, *s_dev_info, *s_qr, *s_name;
static char s_qr_url[40];

// ---------------------------------------------------------------- helpers

static void apply_kv(const char *key, cJSON *val)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddItemToObject(p, key, val);
    settings_apply_json(p);
    cJSON_Delete(p);
}

static lv_obj_t *section(lv_obj_t *col, const lv_image_dsc_t *icon, const char *title)
{
    lv_obj_t *c = ui_card(col, 0, 0, COL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_t *h = ui_box(c);
    ui_icon(h, icon);
    lv_obj_t *t = ui_title(h, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 38, 14 - lv_font_get_line_height(lv_obj_get_style_text_font(t, 0)) / 2);
    return c;
}

static lv_obj_t *row(lv_obj_t *card, const char *label)
{
    lv_obj_t *r = ui_box(card);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, 40);
    lv_obj_t *l = ui_label(r, P.f18, P.text2, label);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return r;
}

static void slider_style(lv_obj_t *s)
{
    lv_obj_set_style_bg_color(s, P.light ? lv_color_hex(0xe3e8f2) : lv_color_hex(0x232b40), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, P.accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s, lv_color_mix(P.accent, lv_color_hex(0xf0abfc), 150), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 12, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(s, 80, LV_PART_KNOB);
    lv_obj_set_height(s, 10);
    lv_obj_set_ext_click_area(s, 18);
}

static lv_obj_t *segmented(lv_obj_t *parent, const char *const *map, int checked, int w)
{
    lv_obj_t *bm = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    if (checked >= 0) lv_buttonmatrix_set_button_ctrl(bm, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, w, 46);
    lv_obj_set_style_bg_color(bm, P.light ? lv_color_hex(0xe9edf5) : lv_color_hex(0x0d1220), 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bm, 1, 0);
    lv_obj_set_style_border_color(bm, P.border, 0);
    lv_obj_set_style_radius(bm, 12, 0);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_pad_gap(bm, 4, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 9, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, P.f16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, P.text2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, P.accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    // The accents are all light: dark text on the selected button reads; white didn't.
    lv_obj_set_style_text_color(bm, lv_color_hex(0x111111), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bm, LV_OPA_40, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bm, P.accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    return bm;
}

// ---------------------------------------------------------------- display

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    lv_label_set_text_fmt(s_bright_val, "%d%%", v);
    board_set_brightness(v, 0);  // live preview
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) apply_kv("brightness", cJSON_CreateNumber(v));
}

static void dim_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    lv_label_set_text_fmt(s_dim_val, v ? "%d%%" : "Off", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) apply_kv("dim_brightness", cJSON_CreateNumber(v));
}

static const int DIM_AFTER[] = {15, 30, 60, 300, 86400};
static void dim_after_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i < 5) apply_kv("dim_after", cJSON_CreateNumber(DIM_AFTER[i]));
}

static void switch_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    apply_kv(lv_event_get_user_data(e), cJSON_CreateBool(lv_obj_has_state(s, LV_STATE_CHECKED)));
}

static lv_obj_t *switch_row(lv_obj_t *card, const char *label, const char *key, bool on)
{
    lv_obj_t *r = row(card, label);
    lv_obj_t *sw = ui_switch(r);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
    return sw;
}

static void build_display(lv_obj_t *col, const settings_t *st)
{
    lv_obj_t *c = section(col, &img_fluent_lightbulb_filament_28, "Display");
    lv_obj_t *r = row(c, "Brightness");
    s_bright_val = ui_label(r, P.f18, P.text, "");
    lv_obj_align(s_bright_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text_fmt(s_bright_val, "%d%%", st->brightness);
    lv_obj_t *s = lv_slider_create(c);
    lv_obj_set_width(s, LV_PCT(96));
    lv_slider_set_range(s, 5, 100);
    lv_slider_set_value(s, st->brightness, LV_ANIM_OFF);
    slider_style(s);
    lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, bright_cb, LV_EVENT_RELEASED, NULL);

    r = row(c, "When no PC is active, dim to");
    s_dim_val = ui_label(r, P.f18, P.text, "");
    lv_obj_align(s_dim_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text_fmt(s_dim_val, st->dim_brightness ? "%d%%" : "Off", st->dim_brightness);
    s = lv_slider_create(c);
    lv_obj_set_width(s, LV_PCT(96));
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, st->dim_brightness, LV_ANIM_OFF);
    slider_style(s);
    lv_obj_add_event_cb(s, dim_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, dim_cb, LV_EVENT_RELEASED, NULL);

    ui_label(c, P.f18, P.text2, "Dim after");
    static const char *map[] = {"15 s", "30 s", "1 min", "5 min", "Never", ""};
    int sel = 4;
    for (int i = 0; i < 5; i++)
        if (st->dim_after <= DIM_AFTER[i]) {
            sel = i;
            break;
        }
    lv_obj_t *bm = segmented(c, map, sel, COL_W - 40);
    lv_obj_add_event_cb(bm, dim_after_cb, LV_EVENT_VALUE_CHANGED, NULL);

    switch_row(c, "Wake up when an agent needs you", "wake_on_alert", st->wake_on_alert);
    switch_row(c, "Jump to Agents when one needs you", "auto_page", st->auto_page);
}

// ---------------------------------------------------------------- appearance

static const char *THEMES[] = {"midnight", "aurora", "graphite", "paper", "ink", "newsprint"};
static void theme_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i < 6) apply_kv("theme", cJSON_CreateString(THEMES[i]));
}

static const uint32_t ACCENTS[] = {0x7aa2f7, 0xa78bfa, 0xf472b6, 0xfb7185, 0xfb923c, 0xfacc15, 0x34d399, 0x22d3ee};
static void accent_cb(lv_event_t *e)
{
    char hex[8];
    snprintf(hex, sizeof hex, "#%06lx", (unsigned long)ACCENTS[(int)(intptr_t)lv_event_get_user_data(e)]);
    apply_kv("accent", cJSON_CreateString(hex));
}

static void agent_text_cb(lv_event_t *e)
{
    static const char *const sizes[] = {"small", "medium", "large"};
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i < 3) apply_kv("agent_text", cJSON_CreateString(sizes[i]));
}

static void unit_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    apply_kv("temp_unit", cJSON_CreateString(i == 1 ? "F" : "C"));
}

static void build_appearance(lv_obj_t *col, const settings_t *st)
{
    lv_obj_t *c = section(col, &img_fluent_planet_28, "Appearance");
    ui_label(c, P.f18, P.text2, "Theme");
    static const char *map[] = {"Midnight", "Aurora", "Graphite", "Paper", "Ink", "News", ""};
    int sel = 0;
    for (int i = 0; i < 6; i++)
        if (!strcmp(st->theme, THEMES[i])) sel = i;
    lv_obj_t *bm = segmented(c, map, sel, COL_W - 40);
    // One line: buttons sized to their names (relative widths) so every name fits.
    static const uint8_t widths[] = {5, 4, 5, 4, 3, 4};
    for (int i = 0; i < 6; i++) lv_buttonmatrix_set_button_width(bm, i, widths[i]);
    lv_obj_set_style_pad_gap(bm, 2, 0);
    lv_obj_add_event_cb(bm, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);

    ui_label(c, P.f18, P.text2, "Accent");
    lv_obj_t *sw = ui_box(c);
    lv_obj_set_width(sw, LV_PCT(100));
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(sw, 7, 0);  // room for the selection ring, which draws outside the dot
    for (int i = 0; i < 8; i++) {
        lv_obj_t *dot = ui_box(sw);
        lv_obj_set_size(dot, 38, 38);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(ACCENTS[i]), 0);
        lv_obj_set_style_bg_grad_color(dot, lv_color_mix(lv_color_hex(ACCENTS[i]), lv_color_white(), 190), 0);
        lv_obj_set_style_bg_grad_dir(dot, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        if (ACCENTS[i] == st->accent) {
            lv_obj_set_style_outline_width(dot, 3, 0);
            lv_obj_set_style_outline_color(dot, P.text, 0);
            lv_obj_set_style_outline_pad(dot, 3, 0);
        }
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(dot, 6);
        lv_obj_add_event_cb(dot, accent_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_t *ar = row(c, "Agent text");
    static const char *amap[] = {"Small", "Medium", "Large", ""};
    lv_obj_t *as = segmented(ar, amap, st->agent_text, 260);
    lv_obj_align(as, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_height(as, 40);
    lv_obj_add_event_cb(as, agent_text_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *r = row(c, "Temperature");
    static const char *umap[] = {"°C", "°F", ""};
    lv_obj_t *u = segmented(r, umap, st->temp_unit == 'F', 140);
    lv_obj_align(u, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_height(u, 40);
    lv_obj_add_event_cb(u, unit_cb, LV_EVENT_VALUE_CHANGED, NULL);
    switch_row(c, "24-hour clock", "clock_24h", st->clock_24h);
}

// ---------------------------------------------------------------- PCs

static void pc_show(lv_event_t *e) { hub_activate(lv_event_get_user_data(e), false); }

static void pc_pin(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    settings_t st = settings_get();
    if (!strcmp(st.pinned_pc, id)) apply_kv("pinned_pc", cJSON_CreateString(""));
    else hub_activate(id, true);
}

static char s_forget_id[40];
static void do_forget(void)
{
    settings_pc_forget(s_forget_id);
    ui_request_rebuild();
}

static void pc_forget(lv_event_t *e)
{
    snprintf(s_forget_id, sizeof s_forget_id, "%s", (const char *)lv_event_get_user_data(e));
    ui_confirm("Forget this PC?", "It will have to be approved on this screen again before it can send data.", "Forget", do_forget);
}

static void failover_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    apply_kv("failover", cJSON_CreateString(i == 1 ? "manual" : "auto"));
}

static lv_obj_t *icon_button(lv_obj_t *parent, const char *glyph, lv_color_t color)
{
    lv_obj_t *b = ui_box(parent);
    lv_obj_set_size(b, 44, 44);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, P.card2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, P.border, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *g = ui_glyph(b, &icons_22, color, glyph);
    lv_obj_center(g);
    return b;
}

static void rebuild_pcs(void)
{
    lv_obj_clean(s_pcs_box);
    static pc_info_t pcs[MAX_PAIRED];
    int n = hub_pcs(pcs, MAX_PAIRED);
    settings_t st = settings_get();
    if (!n) {
        lv_obj_t *l = ui_label(s_pcs_box, P.f16, P.text3,
                               "No PCs paired yet. Run  deskhud install  on a PC on this network,\nthen approve it here.");
        lv_obj_set_width(l, COL_W - 40);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = ui_box(s_pcs_box);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_t *icon = ui_icon(r, &img_hw_pc_40);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *h = ui_label(r, P.f18, P.text, PRIV(pcs[i].host, "workstation"));
        lv_obj_set_pos(h, 52, 4);
        ui_one_line(h);
        lv_obj_set_width(h, 200);
        bool pinned = !strcmp(st.pinned_pc, pcs[i].pc_id);
        const char *status = pcs[i].active ? "Showing now" : pcs[i].online ? "Online · standby" : "Offline";
        lv_obj_t *s = ui_label(r, P.f14, pcs[i].active ? C_DONE : pcs[i].online ? P.text2 : P.text3, "");
        lv_label_set_text_fmt(s, "%s%s", status, pinned ? "  ·  pinned" : "");
        lv_obj_set_pos(s, 52, 30);
        // pc_id strings must outlive the row: keep them in the static array
        lv_obj_t *fb = icon_button(r, ICON_DELETE, C_ERROR);
        lv_obj_align(fb, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(fb, pc_forget, LV_EVENT_CLICKED, pcs[i].pc_id);
        lv_obj_t *pb = icon_button(r, ICON_PIN, pinned ? P.accent : P.text3);
        lv_obj_align_to(pb, fb, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        lv_obj_add_event_cb(pb, pc_pin, LV_EVENT_CLICKED, pcs[i].pc_id);
        if (pcs[i].online && !pcs[i].active) {
            lv_obj_t *sb = icon_button(r, ICON_EYE, P.text);
            lv_obj_align_to(sb, pb, LV_ALIGN_OUT_LEFT_MID, -8, 0);
            lv_obj_add_event_cb(sb, pc_show, LV_EVENT_CLICKED, pcs[i].pc_id);
        }
    }
}

static void build_pcs(lv_obj_t *col, const settings_t *st)
{
    lv_obj_t *c = section(col, &img_fluent_link_22, "PCs");
    s_pcs_box = ui_box(c);
    lv_obj_set_width(s_pcs_box, LV_PCT(100));
    lv_obj_set_flex_flow(s_pcs_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pcs_box, 10, 0);
    rebuild_pcs();
    lv_obj_t *r = row(c, "Switch PCs");
    static const char *map[] = {"Automatic", "Manual", ""};
    lv_obj_t *bm = segmented(r, map, st->failover == FAILOVER_MANUAL, 230);
    lv_obj_set_height(bm, 40);
    lv_obj_align(bm, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(bm, failover_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *hint = ui_label(c, P.f14, P.text3,
                              "Automatic: when the PC on screen goes quiet, the next one that's online takes over. "
                              "Pin a PC to always prefer it.");
    lv_obj_set_width(hint, COL_W - 40);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

// ---------------------------------------------------------------- Wi-Fi & device

static void wifi_btn(lv_event_t *e) { ui_wifi_open(); }
static void rename_btn(lv_event_t *e) { ui_rename_open(); }
static void do_restart(void) { esp_restart(); }
static void do_factory(void)
{
    settings_factory_reset();
    esp_restart();
}
static void do_flip(void)
{
    apply_kv("flip", cJSON_CreateBool(!settings_get().flip));
    esp_restart();
}
static void restart_btn(lv_event_t *e) { ui_confirm("Restart?", "The display will be back in a few seconds.", "Restart", do_restart); }
static void flip_btn(lv_event_t *e)
{
    ui_confirm("Flip the screen?", "Rotates everything 180° (for mounting upside down). The display restarts.", "Flip", do_flip);
}
static void factory_btn(lv_event_t *e)
{
    ui_confirm("Factory reset?", "Erases Wi-Fi, paired PCs and all settings, then restarts.", "Erase everything", do_factory);
}

static void build_device(lv_obj_t *col, const settings_t *st)
{
    lv_obj_t *c = section(col, &img_hw_wifi_28, "Wi-Fi");
    lv_obj_t *r = ui_box(c);
    lv_obj_set_width(r, LV_PCT(100));
    s_wifi_info = ui_label(r, P.f16, P.text2, "");
    lv_obj_t *b = ui_button(r, ICON_WIFI, "Change", false);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(b, wifi_btn, LV_EVENT_CLICKED, NULL);

    c = section(col, &img_fluent_settings_28, "Device");
    r = ui_box(c);
    lv_obj_set_width(r, LV_PCT(100));
    s_name = ui_label(r, P.f_head, P.text, st->name);
    lv_obj_add_flag(s_name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_name, rename_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pen = ui_glyph(r, &icons_18, P.text3, ICON_BUILD);
    lv_obj_align_to(pen, s_name, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    s_dev_info = ui_label(r, P.f14, P.text2, "");
    lv_obj_set_pos(s_dev_info, 0, 44);
    s_qr = lv_qrcode_create(r);
    lv_qrcode_set_size(s_qr, 112);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x0b1020));
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 6, 0);
    lv_obj_set_style_radius(s_qr, 6, 0);
    lv_obj_align(s_qr, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *qrl = ui_label(r, P.f14, P.text3, "Settings in\nyour browser");
    lv_obj_set_style_text_align(qrl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(qrl, s_qr, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    s_qr_url[0] = 0;

    lv_obj_t *btns = ui_box(c);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 10, 0);
    b = ui_button(btns, ICON_RESTART, "Restart", false);
    lv_obj_add_event_cb(b, restart_btn, LV_EVENT_CLICKED, NULL);
    b = ui_button(btns, ICON_SWAP, "Flip", false);
    lv_obj_add_event_cb(b, flip_btn, LV_EVENT_CLICKED, NULL);
    b = ui_button(btns, ICON_DELETE, "Reset", false);
    lv_obj_add_event_cb(b, factory_btn, LV_EVENT_CLICKED, NULL);
}

void page_settings_create(lv_obj_t *tile)
{
    settings_t st = settings_get();
    lv_obj_set_scroll_dir(tile, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_t *wrap = ui_box(tile);
    lv_obj_set_width(wrap, BOARD_W);
    lv_obj_set_style_pad_hor(wrap, 16, 0);
    lv_obj_set_style_pad_top(wrap, 4, 0);
    lv_obj_set_style_pad_bottom(wrap, 24, 0);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *left = ui_box(wrap), *right = ui_box(wrap);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 12, 0);
    lv_obj_set_style_pad_row(right, 12, 0);
    build_appearance(left, &st);
    build_display(left, &st);
    build_pcs(right, &st);
    build_device(right, &st);
}

void page_settings_update(const ui_data_t *d)
{
    if (d->pcs_changed) rebuild_pcs();
    net_status_t n = net_status();
    if (n.state == NET_CONNECTED)
        lv_label_set_text_fmt(s_wifi_info, "%s\n%s  ·  %d dBm", PRIV(n.ssid, "home"), PRIV(n.ip, "192.168.1.40"), n.rssi);
    else lv_label_set_text(s_wifi_info, n.state == NET_NO_CONFIG ? "Not set up" : "Connecting…");
    const esp_app_desc_t *app = esp_app_get_description();
    char up[24];
    fmt_duration(up, sizeof up, esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_dev_info, "Firmware %s  ·  up %s\n%s.local\nMemory %u KB  ·  PSRAM %u KB free", app->version, up,
                          PRIV(n.hostname, "deskhud"), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(s_name, settings_get().name);
    char url[40];
    snprintf(url, sizeof url, "http://%s/", PRIV(n.ip[0] ? n.ip : "0.0.0.0", "deskhud.local"));
    if (strcmp(url, s_qr_url)) {
        snprintf(s_qr_url, sizeof s_qr_url, "%s", url);
        lv_qrcode_update(s_qr, url, strlen(url));
    }
}
