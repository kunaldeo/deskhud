#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fonts.h"
#include "hub.h"
#include "images.h"
#include "lvgl.h"

typedef struct {
    lv_color_t bg, card, card2, border, text, text2, text3, accent;
    lv_opa_t card_opa;
    bool light;
    // Newsprint: square ink-ruled sections, flat gauges and bars, ink-tinted icons.
    bool editorial;
    bool flat;        // flat single-color gauges and bars (Paper, Ink, Newsprint)
    bool caps_titles; // card titles as small letter-spaced capitals (Paper, Ink)
    int radius, arc_w;
    // Type roles
    const lv_font_t *f_title;    // card titles
    const lv_font_t *f_head;     // big headings (agent project, dialogs)
    const lv_font_t *f_head_s;   // compact headings
    const lv_font_t *f_num_xl;   // gauge figures
    const lv_font_t *f_num;      // tile figures, top-bar clock
    const lv_font_t *f_clock;    // idle clock
    const lv_font_t *f_date;     // idle date
    const lv_font_t *f14, *f16, *f18, *f20, *fmono;  // body text
    const lv_font_t *fa_body, *fa_small;  // agent text (messages / tool rows), per the agent_text setting
    const lv_font_t *fa_body_b, *fa_small_b;  // their bold (markdown **strong**, headings)
    // Color roles
    lv_color_t working, permission, question, done, error, idle;
    lv_color_t cpu, gpu, ram, vram, net_down, net_up, disk;
} palette_t;

extern palette_t P;
/// Profiling switches (console "perf <bits>"): 1 opaque cards, 2 square cards, 4 no gradients.
extern int g_perf;

// State colors
#define C_WORKING (P.working)
#define C_PERMISSION (P.permission)
#define C_QUESTION (P.question)
#define C_DONE (P.done)
#define C_ERROR (P.error)
#define C_IDLE (P.idle)
// Metric colors
#define C_CPU (P.cpu)
#define C_GPU (P.gpu)
#define C_RAM (P.ram)
#define C_VRAM (P.vram)
#define C_NET_DOWN (P.net_down)
#define C_NET_UP (P.net_up)
#define C_DISK (P.disk)

void theme_load(void);

/// Screenshot privacy (console "privacy 1"): network names and addresses show as placeholders.
extern bool g_privacy;
#define PRIV(real, fake) (g_privacy ? (fake) : (real))

// Builders. Everything is created non-scrollable unless stated.
lv_obj_t *ui_box(lv_obj_t *parent);  // bare container, no style
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h);
/// Card / section title in the theme's title style.
lv_obj_t *ui_title(lv_obj_t *parent, const char *text);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img);
lv_obj_t *ui_glyph(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *icon);
lv_obj_t *ui_pill(lv_obj_t *parent, lv_color_t color, const char *text);  // tinted chip
void ui_pill_set(lv_obj_t *pill, lv_color_t color, const char *text);
lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, lv_color_t c1, lv_color_t c2);
lv_obj_t *ui_button(lv_obj_t *parent, const char *icon, const char *text, bool primary);
lv_obj_t *ui_switch(lv_obj_t *parent);
/// Single line, truncated with an ellipsis (LVGL wraps "dot" labels unless the height is fixed).
void ui_one_line(lv_obj_t *label);

// Formatting
void fmt_bytes(char *buf, size_t n, double bytes);       // 1.2 GB
void fmt_rate(char *buf, size_t n, double bytes_per_s);  // 12.4 MB/s
void fmt_tokens(char *buf, size_t n, uint64_t t);         // 65k, 1.2M
void fmt_temp(char *buf, size_t n, float c);              // 54° (unit from settings)
void fmt_duration(char *buf, size_t n, int64_t secs);     // 2:31, 1h 04m

lv_color_t state_color(ag_state_t s);
const char *state_name(ag_state_t s);
const lv_image_dsc_t *agent_logo(const char *agent, bool large);
/// Load-dependent color for a 0..100 value (cool → hot), for numbers next to gauges.
lv_color_t heat_color(float pct);
