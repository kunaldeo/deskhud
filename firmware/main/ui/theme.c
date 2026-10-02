#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"

palette_t P;
int g_perf;
bool g_privacy;

void theme_load(void)
{
    settings_t s = settings_get();
    P.accent = lv_color_hex(s.accent);
    P.light = false;
    P.editorial = false;
    P.flat = false;
    P.caps_titles = false;
    P.radius = 18;
    P.arc_w = 16;
    P.card_opa = 200;
    P.f_title = &inter_20;
    P.f_head = &inter_semi_28;
    P.f_head_s = &inter_20;
    P.f_num_xl = &mono_semi_44;
    P.f_num = &mono_semi_24;
    P.f_clock = &inter_bold_140;
    P.f_date = &inter_semi_36;
    // Readability floor for a 7" panel at desk distance: nothing below 16 px.
    // Role names are historical: f14 = captions, f16 = body, f18 = emphasis, f20 = headings.
    P.f14 = &inter_16;
    P.f16 = &inter_18;
    P.f18 = &inter_20;
    P.f20 = &inter_24;
    P.fmono = &mono_16;
    P.working = lv_color_hex(0x7aa2f7);
    P.permission = lv_color_hex(0xff9e64);
    P.question = lv_color_hex(0xe0af68);
    P.done = lv_color_hex(0x9ece6a);
    P.error = lv_color_hex(0xf7768e);
    P.idle = lv_color_hex(0x737aa2);
    P.cpu = lv_color_hex(0x818cf8);
    P.gpu = lv_color_hex(0x34d399);
    P.ram = lv_color_hex(0x22d3ee);
    P.vram = lv_color_hex(0x2dd4bf);
    P.net_down = lv_color_hex(0xfbbf24);
    P.net_up = lv_color_hex(0xf472b6);
    P.disk = lv_color_hex(0xfb7185);
    if (!strcmp(s.theme, "newsprint")) {
        // A business broadsheet in Anthropic's palette: ivory stock, ink, clay.
        P.light = true;
        P.editorial = true;
        P.flat = true;
        P.radius = 0;
        P.arc_w = 10;
        P.bg = lv_color_hex(0xf0eee6);
        P.card = lv_color_hex(0xf7f5ee);
        P.card2 = lv_color_hex(0xe9e6db);
        P.border = lv_color_hex(0xd3cfc1);
        P.text = lv_color_hex(0x141413);
        P.text2 = lv_color_hex(0x3d3d3a);
        P.text3 = lv_color_hex(0x87867f);
        P.accent = lv_color_hex(0xcc785c);  // clay
        P.card_opa = LV_OPA_COVER;
        P.f_title = &serif_22;
        P.f_head = &serif_28;
        P.f_head_s = &serif_22;
        P.f_num_xl = &serif_num_44;
        P.f_num = &serif_num_26;
        P.f_clock = &serif_num_140;
        P.f_date = &serif_36;
        P.working = lv_color_hex(0x6a9bcc);     // sky
        P.permission = lv_color_hex(0xcc785c);  // clay
        P.question = lv_color_hex(0xc08b3e);    // kraft
        P.done = lv_color_hex(0x788c5d);        // olive
        P.error = lv_color_hex(0xbf4d43);
        P.idle = lv_color_hex(0x87867f);
        P.cpu = lv_color_hex(0x141413);         // ink
        P.gpu = lv_color_hex(0xcc785c);
        P.ram = lv_color_hex(0x6a9bcc);
        P.vram = lv_color_hex(0x788c5d);
        P.net_down = lv_color_hex(0xcc785c);
        P.net_up = lv_color_hex(0x6a9bcc);
        P.disk = lv_color_hex(0x3d3d3a);
        return;
    }
    if (!strcmp(s.theme, "paper") || !strcmp(s.theme, "ink")) {
        // Typographic themes: IBM Plex on a quiet canvas. Light figures carry the numbers,
        // small letter-spaced capitals label them; gauges and bars are a single flat color.
        bool dark = !strcmp(s.theme, "ink");
        P.light = !dark;
        P.flat = true;
        P.caps_titles = true;
        P.radius = 14;
        P.arc_w = 8;
        P.card_opa = LV_OPA_COVER;
        P.f_title = &plex_semi_18;
        P.f_head = &plex_semi_28;
        P.f_head_s = &plex_semi_22;
        P.f_num_xl = &plex_light_num_56;
        P.f_num = &plex_light_num_30;
        P.f_clock = &plex_thin_140;
        P.f_date = &plex_light_36;
        P.f14 = &plex_16;
        P.f16 = &plex_18;
        P.f18 = &plex_med_20;
        P.f20 = &plex_med_22;
        P.fmono = &plex_mono_16;
        if (dark) {
            P.bg = lv_color_hex(0x141311);
            P.card = lv_color_hex(0x1c1b18);
            P.card2 = lv_color_hex(0x262420);
            P.border = lv_color_hex(0x2f2d28);
            P.text = lv_color_hex(0xedeae3);
            P.text2 = lv_color_hex(0xa8a397);
            P.text3 = lv_color_hex(0x6e695f);
            P.working = lv_color_hex(0x7cb2ff);
            P.permission = lv_color_hex(0xff9a5c);
            P.question = lv_color_hex(0xf2c94c);
            P.done = lv_color_hex(0x6fd08c);
            P.error = lv_color_hex(0xff7b72);
            P.idle = lv_color_hex(0x7d786d);
            P.ram = lv_color_hex(0x7cb2ff);
            P.vram = lv_color_hex(0x6fd08c);
            P.disk = lv_color_hex(0xa8a397);
            P.net_up = lv_color_hex(0x7d786d);
        } else {
            P.bg = lv_color_hex(0xf7f5f0);
            P.card = lv_color_hex(0xffffff);
            P.card2 = lv_color_hex(0xf0ede6);
            P.border = lv_color_hex(0xe5e0d6);
            P.text = lv_color_hex(0x171614);
            P.text2 = lv_color_hex(0x55524b);
            P.text3 = lv_color_hex(0x8f8a80);
            P.working = lv_color_hex(0x2563eb);
            P.permission = lv_color_hex(0xea580c);
            P.question = lv_color_hex(0xb7791f);
            P.done = lv_color_hex(0x15803d);
            P.error = lv_color_hex(0xdc2626);
            P.idle = lv_color_hex(0x8f8a80);
            P.ram = lv_color_hex(0x2563eb);
            P.vram = lv_color_hex(0x15803d);
            P.disk = lv_color_hex(0x55524b);
            P.net_up = lv_color_hex(0x8f8a80);
        }
        // Ink figures, accent for the second series.
        P.cpu = P.text;
        P.gpu = P.accent;
        P.net_down = P.accent;
    } else if (!strcmp(s.theme, "graphite")) {
        P.bg = lv_color_hex(0x0c0d10);
        P.card = lv_color_hex(0x18191d);
        P.card2 = lv_color_hex(0x222328);
        P.border = lv_color_hex(0x2a2c33);
        P.text = lv_color_hex(0xececf1);
        P.text2 = lv_color_hex(0xa1a1aa);
        P.text3 = lv_color_hex(0x6b6b76);
    } else if (!strcmp(s.theme, "aurora")) {  // teal-tinted glass under green / violet light
        P.bg = lv_color_hex(0x040b0c);
        P.card = lv_color_hex(0x0b1a1c);
        P.card2 = lv_color_hex(0x12282a);
        P.border = lv_color_hex(0x1c3b3b);
        P.text = lv_color_hex(0xe6f4f1);
        P.text2 = lv_color_hex(0x94b8b2);
        P.text3 = lv_color_hex(0x587a75);
        P.card_opa = 205;
        // The backdrop is green/teal: chart and metric colors must not be.
        P.cpu = lv_color_hex(0xc4b5fd);       // lavender
        P.gpu = lv_color_hex(0xfbbf24);       // amber
        P.ram = lv_color_hex(0x7dd3fc);
        P.vram = lv_color_hex(0xfda4af);
        P.net_down = lv_color_hex(0xfbbf24);
        P.net_up = lv_color_hex(0xf0abfc);
        P.disk = lv_color_hex(0xfb7185);
        P.done = lv_color_hex(0xa3e635);
    } else {  // midnight: navy glass under indigo light
        P.bg = lv_color_hex(0x060914);
        P.card = lv_color_hex(0x10162a);
        P.card2 = lv_color_hex(0x19213a);
        P.border = lv_color_hex(0x252f4d);
        P.text = lv_color_hex(0xe8ecf8);
        P.text2 = lv_color_hex(0x9aa4c4);
        P.text3 = lv_color_hex(0x5f6888);
        P.card_opa = 190;
    }
    // Agent text size: messages use fa_body, tool rows and the home page's compact card fa_small.
    const lv_font_t *body[] = {P.f16, P.f18, P.f20}, *small[] = {P.f14, P.f16, P.f18};
    int at = s.agent_text < 0 || s.agent_text > 2 ? 2 : s.agent_text;
    P.fa_body = body[at];
    P.fa_small = small[at];
    // Bold: the same family a weight up (Plex for Paper / Ink, Inter elsewhere).
    bool plex = P.f14 == &plex_16;
    const lv_font_t *bold_plex[] = {&plex_semi_16, &plex_semi_18, &plex_semi_20, &plex_semi_22};
    const lv_font_t *bold_inter[] = {&inter_semi_16, &inter_semi_18, &inter_semi_20, &inter_semi_24};
    const lv_font_t **bold = plex ? bold_plex : bold_inter;
    P.fa_body_b = bold[at + 1];
    P.fa_small_b = bold[at];
}

lv_obj_t *ui_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = ui_box(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, P.card, 0);
    lv_obj_set_style_bg_opa(c, P.card_opa, 0);
    lv_obj_set_style_radius(c, (g_perf & 2) ? 0 : P.radius, 0);
    if (g_perf & 1) lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    if (P.editorial) {  // a heavy ink rule over each section, like a newspaper column head
        lv_obj_set_style_border_side(c, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(c, 3, 0);
        lv_obj_set_style_border_color(c, P.text, 0);
    } else {
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, P.border, 0);
    }
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    return c;
}

lv_obj_t *ui_title(lv_obj_t *parent, const char *text)
{
    if (!P.caps_titles) return ui_label(parent, P.f_title, P.text, text);
    char caps[64];
    size_t i = 0;
    for (; text[i] && i < sizeof caps - 1; i++) caps[i] = (text[i] >= 'a' && text[i] <= 'z') ? text[i] - 32 : text[i];
    caps[i] = 0;
    lv_obj_t *l = ui_label(parent, P.f_title, P.text2, caps);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    return i;
}

lv_obj_t *ui_glyph(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon)
{
    return ui_label(parent, font, color, icon);
}

void ui_pill_set(lv_obj_t *pill, lv_color_t color, const char *text)
{
    lv_obj_set_style_bg_color(pill, color, 0);
    lv_obj_set_style_border_color(pill, color, 0);
    lv_obj_t *l = NULL;  // the label; a pill may also hold a status dot
    for (uint32_t i = 0; i < lv_obj_get_child_count(pill) && !l; i++)
        if (lv_obj_check_type(lv_obj_get_child(pill, i), &lv_label_class)) l = lv_obj_get_child(pill, i);
    if (!l) return;
    lv_obj_set_style_text_color(l, P.light ? lv_color_darken(color, 80) : lv_color_lighten(color, 40), 0);
    lv_label_set_text(l, text);
}

lv_obj_t *ui_pill(lv_obj_t *parent, lv_color_t color, const char *text)
{
    lv_obj_t *p = ui_box(parent);
    lv_obj_set_style_bg_opa(p, 48, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_opa(p, 110, 0);
    lv_obj_set_style_radius(p, P.editorial ? 3 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(p, 10, 0);
    lv_obj_set_style_pad_ver(p, 3, 0);
    ui_label(p, P.f14, color, text);
    ui_pill_set(p, color, text);
    return p;
}

lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, lv_color_t c1, lv_color_t c2)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 1000);
    lv_obj_set_style_bg_color(b, P.flat ? P.card2 : P.light ? lv_color_hex(0xe3e8f2) : lv_color_hex(0x232b40), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, P.editorial ? 0 : h / 2, 0);
    lv_obj_set_style_radius(b, P.editorial ? 0 : h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, c1, LV_PART_INDICATOR);
    if (P.flat) lv_obj_set_style_bg_color(b, c1, LV_PART_INDICATOR);
    else if (!(g_perf & 4)) {
        lv_obj_set_style_bg_grad_color(b, c2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    }
    lv_obj_set_style_anim_duration(b, 600, 0);
    return b;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *icon, const char *text, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 52);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_radius(b, P.editorial ? 4 : 14, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, primary ? P.accent : P.card2, 0);
    if (primary && !P.flat) {
        lv_obj_set_style_bg_grad_color(b, lv_color_mix(P.accent, lv_color_hex(0xc084fc), 160), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_HOR, 0);
    } else {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, P.border, 0);
    }
    lv_obj_set_style_transform_scale(b, 245, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(b, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(b, LV_PCT(50), 0);
    lv_obj_set_style_bg_opa(b, 200, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_color_t fg = primary ? lv_color_white() : P.text;
    if (icon) ui_glyph(b, &icons_22, fg, icon);
    if (text) ui_label(b, P.f18, fg, text);
    return b;
}

lv_obj_t *ui_switch(lv_obj_t *parent)
{
    lv_obj_t *s = lv_switch_create(parent);
    lv_obj_set_size(s, 58, 32);
    lv_obj_set_style_bg_color(s, P.light ? lv_color_hex(0xd5dbe7) : lv_color_hex(0x2a3350), 0);
    lv_obj_set_style_bg_color(s, P.accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    return s;
}

void fmt_bytes(char *buf, size_t n, double b)
{
    static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1000 && i < 4) {
        b /= 1024;
        i++;
    }
    snprintf(buf, n, b >= 100 || i == 0 ? "%.0f %s" : "%.1f %s", b, u[i]);
}

void fmt_rate(char *buf, size_t n, double b)
{
    static const char *u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (b >= 1000 && i < 3) {
        b /= 1024;
        i++;
    }
    snprintf(buf, n, b >= 100 || i == 0 ? "%.0f %s" : "%.1f %s", b, u[i]);
}

void fmt_tokens(char *buf, size_t n, uint64_t t)
{
    if (t >= 1000000) snprintf(buf, n, "%.1fM", t / 1e6);
    else if (t >= 1000) snprintf(buf, n, "%lluk", (unsigned long long)(t + 500) / 1000);
    else snprintf(buf, n, "%llu", (unsigned long long)t);
}

void fmt_temp(char *buf, size_t n, float c)
{
    if (isnan(c)) {
        snprintf(buf, n, "–");
        return;
    }
    settings_t s = settings_get();
    snprintf(buf, n, "%.0f°", s.temp_unit == 'F' ? c * 9 / 5 + 32 : c);
}

void fmt_duration(char *buf, size_t n, int64_t s)
{
    if (s < 0) s = 0;
    if (s < 3600) snprintf(buf, n, "%lld:%02lld", s / 60, s % 60);
    else if (s < 86400) snprintf(buf, n, "%lldh %02lldm", s / 3600, (s / 60) % 60);
    else snprintf(buf, n, "%lldd %lldh", s / 86400, (s / 3600) % 24);
}

lv_color_t state_color(ag_state_t s)
{
    switch (s) {
    case AG_WORKING: return C_WORKING;
    case AG_PERMISSION: return C_PERMISSION;
    case AG_QUESTION: return C_QUESTION;
    case AG_DONE: return C_DONE;
    case AG_ERROR: return C_ERROR;
    default: return C_IDLE;
    }
}

const char *state_name(ag_state_t s)
{
    static const char *n[] = {"Working", "Needs permission", "Has a question", "Done", "Error", "Idle"};
    return n[s];
}

const lv_image_dsc_t *agent_logo(const char *agent, bool large)
{
    if (!strcmp(agent, "codex")) return large ? &img_brand_codex_36 : &img_brand_codex_22;
    if (!strcmp(agent, "pi")) return large ? &img_brand_pi_36 : &img_brand_pi_22;
    if (!strcmp(agent, "omp")) return large ? &img_brand_omp_36 : &img_brand_omp_22;
    if (!strcmp(agent, "opencode")) return large ? &img_brand_opencode_36 : &img_brand_opencode_22;
    if (!strcmp(agent, "claude")) return large ? &img_brand_claude_36 : &img_brand_claude_22;
    return large ? &img_fluent_bot_sparkle_40 : &img_fluent_bot_sparkle_28;
}

lv_color_t heat_color(float pct)
{
    if (pct < 50) return P.text;
    if (pct < 80) return lv_color_hex(0xfbbf24);
    return lv_color_hex(0xfb7185);
}

void ui_one_line(lv_obj_t *l)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}
