#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "ui_internal.h"

// ---------------------------------------------------------------- agent row

static void pulse_cb(void *obj, int32_t v) { lv_obj_set_style_opa(obj, v, 0); }

static void start_pulse(lv_obj_t *dot, bool on)
{
    lv_anim_delete(dot, pulse_cb);
    lv_obj_set_style_opa(dot, LV_OPA_COVER, 0);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dot);
    lv_anim_set_exec_cb(&a, pulse_cb);
    lv_anim_set_values(&a, 255, 60);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void agent_row_create(agent_row_t *r, lv_obj_t *parent, int w, bool compact)
{
    memset(r, 0, sizeof *r);
    r->compact = compact;
    r->state = AG_IDLE;
    lv_obj_t *o = r->root = ui_box(parent);
    lv_obj_set_width(o, w);
    lv_obj_set_height(o, compact ? 64 : 104);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_bg_color(o, P.card2, 0);
    lv_obj_set_style_bg_opa(o, P.light ? 255 : 150, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, P.border, 0);
    lv_obj_set_style_pad_all(o, compact ? 10 : 12, 0);

    r->logo = ui_icon(o, agent_logo("claude", !compact));
    lv_obj_align(r->logo, LV_ALIGN_TOP_LEFT, 0, compact ? 0 : 2);
    int tx = compact ? 32 : 48;

    r->project = ui_label(o, compact ? P.f16 : P.f18, P.text, "");
    lv_obj_set_pos(r->project, tx, compact ? 0 : 0);
    ui_one_line(r->project);
    lv_obj_set_width(r->project, w - tx - 170);

    r->pill = ui_pill(o, C_IDLE, "Idle");
    lv_obj_align(r->pill, LV_ALIGN_TOP_RIGHT, 0, -2);
    r->dot = ui_box(r->pill);
    lv_obj_set_size(r->dot, 8, 8);
    lv_obj_set_style_radius(r->dot, 4, 0);
    lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(r->pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r->pill, 6, 0);
    lv_obj_move_to_index(r->dot, 0);

    r->title = ui_label(o, P.f14, P.text2, "");
    lv_obj_set_pos(r->title, tx, compact ? 24 : 26);
    ui_one_line(r->title);
    lv_obj_set_width(r->title, w - tx - (compact ? 20 : 24));
    if (compact) return;

    r->activity = ui_label(o, P.fmono, P.text3, "");
    lv_obj_set_pos(r->activity, tx, 46);
    ui_one_line(r->activity);
    lv_obj_set_width(r->activity, w - tx - 24);

    r->ctx_bar = ui_bar(o, w - tx - 110, 5, P.flat ? P.accent : lv_color_hex(0xa78bfa), lv_color_hex(0xf472b6));
    lv_obj_set_pos(r->ctx_bar, tx, 73);
    r->ctx_label = ui_label(o, P.f14, P.text3, "");
    lv_obj_align(r->ctx_label, LV_ALIGN_TOP_RIGHT, 0, 65);
}

void agent_row_set(agent_row_t *r, const agent_t *a, time_t now)
{
    lv_obj_remove_flag(r->root, LV_OBJ_FLAG_HIDDEN);
    bool new_id = strcmp(r->id, a->id) != 0;
    snprintf(r->id, sizeof r->id, "%s", a->id);
    if (new_id) lv_image_set_src(r->logo, agent_logo(a->agent, !r->compact));
    lv_label_set_text(r->project, a->project[0] ? a->project : a->agent);
    lv_label_set_text(r->title, a->title[0] ? a->title : "New session");

    lv_color_t sc = state_color(a->state);
    char pill[48];
    const char *name = state_name(a->state);
    if (a->state == AG_PERMISSION) name = "Permission";
    if (a->state == AG_QUESTION) name = "Question";
    if (a->state == AG_WORKING && a->turn_start && now) {
        char d[16];
        fmt_duration(d, sizeof d, now - a->turn_start);
        snprintf(pill, sizeof pill, "%s  %s", name, d);
    } else {
        snprintf(pill, sizeof pill, "%s", name);
    }
    ui_pill_set(r->pill, sc, pill);
    lv_obj_set_style_bg_color(r->dot, sc, 0);
    if (new_id || a->state != r->state) start_pulse(r->dot, a->state == AG_WORKING || a->state == AG_PERMISSION || a->state == AG_QUESTION);

    bool needs = a->state == AG_PERMISSION || a->state == AG_QUESTION;
    lv_obj_set_style_border_color(r->root, needs ? sc : P.border, 0);
    lv_obj_set_style_border_width(r->root, needs ? 2 : 1, 0);
    lv_obj_set_style_bg_color(r->root, needs ? lv_color_mix(sc, P.card2, 40) : P.card2, 0);
    r->state = a->state;
    if (r->compact) return;

    char act[220];
    if (needs && a->prompt[0]) {
        snprintf(act, sizeof act, "%s", a->prompt);
        lv_obj_set_style_text_color(r->activity, lv_color_lighten(sc, 30), 0);
        lv_obj_set_style_text_font(r->activity, P.f14, 0);
    } else if ((a->state == AG_DONE || a->state == AG_ERROR) && a->summary[0]) {
        snprintf(act, sizeof act, "%s", a->summary);
        lv_obj_set_style_text_color(r->activity, P.text2, 0);
        lv_obj_set_style_text_font(r->activity, P.f14, 0);
    } else {
        snprintf(act, sizeof act, "%s", a->activity[0] ? a->activity : "…");
        lv_obj_set_style_text_color(r->activity, P.text3, 0);
        lv_obj_set_style_text_font(r->activity, P.fmono, 0);
    }
    for (char *p = act; *p; p++)
        if (*p == '\n') *p = ' ';
    lv_label_set_text(r->activity, act);
    ui_one_line(r->activity);

    if (a->ctx_max) {
        lv_bar_set_value(r->ctx_bar, (int)(a->ctx * 1000 / a->ctx_max), LV_ANIM_ON);
        char c1[12], c2[12];
        fmt_tokens(c1, sizeof c1, a->ctx);
        fmt_tokens(c2, sizeof c2, a->ctx_max);
        lv_label_set_text_fmt(r->ctx_label, "%s / %s", c1, c2);
    } else {
        lv_bar_set_value(r->ctx_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(r->ctx_label, "");
    }
}

// ---------------------------------------------------------------- sparkline

// Rendered into a cached ARGB canvas once per data update; frames only blit it. Drawing the
// lines and fills live costs ~90 ms per full-screen frame, the blit ~2 ms.
typedef struct {
    int n, w, h;
    lv_color_t colors[3];
} spark_t;

static void spark_free(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_target_obj(e);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(c);
    if (buf) lv_draw_buf_destroy(buf);
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *sparkline_create(lv_obj_t *parent, int w, int h, int nseries, const lv_color_t *colors)
{
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(c, lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0));
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    spark_t *s = lv_malloc_zeroed(sizeof *s);
    s->n = nseries > 3 ? 3 : nseries;
    s->w = w;
    s->h = h;
    for (int i = 0; i < s->n; i++) s->colors[i] = colors[i];
    lv_obj_set_user_data(c, s);
    lv_obj_add_event_cb(c, spark_free, LV_EVENT_DELETE, s);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

void sparkline_set(lv_obj_t *c, const float *const *values, int len, float max)
{
    spark_t *s = lv_obj_get_user_data(c);
    if (max <= 0) {
        max = 1;
        for (int i = 0; i < s->n; i++)
            for (int k = 0; k < len; k++)
                if (values[i][k] > max) max = values[i][k];
        max *= 1.15f;
    }
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    if (len < 2) return;
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    const float dx = (float)(s->w - 1) / (HISTORY - 1);
    const int pad = HISTORY - len;  // newest sample at the right edge
    for (int i = 0; i < s->n; i++) {
        // fill first, line on top
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_grad.dir = LV_GRAD_DIR_VER;
        rd.bg_grad.stops[0].color = s->colors[i];
        rd.bg_grad.stops[1].color = s->colors[i];
        rd.bg_grad.stops[1].frac = 255;
        rd.bg_grad.stops[1].opa = 0;
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = s->colors[i];
        ld.width = 2;
        ld.round_start = ld.round_end = 1;
        float px = 0, py = 0;
        for (int k = 0; k < len; k++) {
            float v = values[i][k] / max;
            if (v > 1) v = 1;
            if (v < 0) v = 0;
            float x = (pad + k) * dx, y = (s->h - 2) - v * (s->h - 4);
            if (k > 0) {
                int top = (int)((py + y) / 2);
                lv_area_t a = {.x1 = (int)px, .x2 = (int)x, .y1 = top, .y2 = s->h - 1};
                rd.bg_grad.stops[0].opa = (lv_opa_t)(40 + 70 * (s->h - top) / s->h);
                if (a.y2 > a.y1) lv_draw_rect(&layer, &rd, &a);
            }
            px = x;
            py = y;
        }
        for (int k = 1; k < len; k++) {
            float v0 = fminf(fmaxf(values[i][k - 1] / max, 0), 1), v1 = fminf(fmaxf(values[i][k] / max, 0), 1);
            ld.p1.x = (pad + k - 1) * dx;
            ld.p1.y = (s->h - 2) - v0 * (s->h - 4);
            ld.p2.x = (pad + k) * dx;
            ld.p2.y = (s->h - 2) - v1 * (s->h - 4);
            lv_draw_line(&layer, &ld);
        }
    }
    lv_canvas_finish_layer(c, &layer);
}

// ---------------------------------------------------------------- modal

static void backdrop_click(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_obj_delete(lv_event_get_current_target(e));
}

lv_obj_t *ui_modal(int w, int h)
{
    lv_obj_t *bd = ui_box(lv_layer_top());
    lv_obj_set_size(bd, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bd, 150, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bd, backdrop_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *card = ui_card(bd, 0, 0, w, h);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // swallow clicks so they don't close the modal
    lv_obj_center(card);
    lv_obj_fade_in(bd, 150, 0);
    return card;
}

void ui_modal_close(lv_obj_t *card)
{
    lv_obj_delete_async(lv_obj_get_parent(card));
}

static void (*s_confirm_cb)(void);

static void confirm_ok(lv_event_t *e)
{
    ui_modal_close(lv_event_get_user_data(e));
    if (s_confirm_cb) s_confirm_cb();
}

static void close_btn(lv_event_t *e) { ui_modal_close(lv_event_get_user_data(e)); }

void ui_confirm(const char *title, const char *body, const char *ok_text, void (*on_ok)(void))
{
    s_confirm_cb = on_ok;
    lv_obj_t *m = ui_modal(520, 260);
    lv_obj_t *t = ui_label(m, &inter_semi_28, P.text, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *b = ui_label(m, P.f18, P.text2, body);
    lv_obj_set_width(b, 472);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_t *ok = ui_button(m, NULL, ok_text, true);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok, confirm_ok, LV_EVENT_CLICKED, m);
    lv_obj_t *cancel = ui_button(m, NULL, "Cancel", false);
    lv_obj_align_to(cancel, ok, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_event_cb(cancel, close_btn, LV_EVENT_CLICKED, m);
}

// ---------------------------------------------------------------- dot graph (btop-style)

// Braille-like dot matrix drawn straight into a cached ARGB canvas: every sample is a column of
// dots, each row coloured by its height (cool near the baseline, hot far from it), on a faint dot
// grid. Mirrored mode puts series A above a centre line and series B below it (btop's net box).
#define DOT 3
#define PITCH 5

typedef struct {
    int w, h, cols, rows;
    bool mirrored;
    lv_color_t a_lo, a_hi, b_lo, b_hi;
} dotgraph_t;

static void dot(lv_draw_buf_t *buf, int x, int y, lv_color_t c, uint8_t opa)
{
    for (int dy = 0; dy < DOT; dy++) {
        uint8_t *row = buf->data + (y + dy) * buf->header.stride + x * 4;
        for (int dx = 0; dx < DOT; dx++, row += 4) {
            row[0] = c.blue;
            row[1] = c.green;
            row[2] = c.red;
            row[3] = opa;
        }
    }
}

static void dg_free(lv_event_t *e)
{
    lv_obj_t *c = lv_event_get_target_obj(e);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(c);
    if (buf) lv_draw_buf_destroy(buf);
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *dotgraph_create(lv_obj_t *parent, int w, int h, bool mirrored, lv_color_t a_lo, lv_color_t a_hi, lv_color_t b_lo,
                          lv_color_t b_hi)
{
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(c, lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0));
    dotgraph_t *g = lv_malloc_zeroed(sizeof *g);
    *g = (dotgraph_t){.w = w, .h = h, .cols = w / PITCH, .rows = h / PITCH, .mirrored = mirrored,
                      .a_lo = a_lo, .a_hi = a_hi, .b_lo = b_lo, .b_hi = b_hi};
    lv_obj_set_user_data(c, g);
    lv_obj_add_event_cb(c, dg_free, LV_EVENT_DELETE, g);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    return c;
}

/// Lit rows for a value: at least one dot for anything above zero, so activity always shows.
static int lit_rows(float v, float max, int rows)
{
    if (v <= 0 || max <= 0) return 0;
    int n = (int)lroundf(fminf(v / max, 1.0f) * rows);
    return n < 1 ? 1 : n;
}

void dotgraph_set(lv_obj_t *c, const float *a, const float *b, int len, float max_a, float max_b)
{
    dotgraph_t *g = lv_obj_get_user_data(c);
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(c);
    lv_draw_buf_clear(buf, NULL);
    lv_color_t grid = P.light ? lv_color_hex(0x9a958a) : lv_color_hex(0x8a8f9c);
    int half = g->mirrored ? g->rows / 2 : g->rows;
    int x0 = (g->w - g->cols * PITCH) / 2;
    for (int col = 0; col < g->cols; col++) {
        // Newest sample at the right edge, one sample per column.
        int k = len - g->cols + col;
        float va = k >= 0 && a ? a[k] : 0, vb = k >= 0 && b ? b[k] : 0;
        int x = x0 + col * PITCH;
        if (!g->mirrored) {
            int n = lit_rows(va, max_a, g->rows);
            for (int r = 0; r < g->rows; r++) {
                int y = g->h - (r + 1) * PITCH + 1;
                if (r < n) dot(buf, x, y, lv_color_mix(g->a_hi, g->a_lo, (r * 255) / (g->rows > 1 ? g->rows - 1 : 1)), 255);
                else dot(buf, x, y, grid, 30);
            }
            continue;
        }
        int mid = half * PITCH;
        int na = lit_rows(va, max_a, half), nb = lit_rows(vb, max_b, half);
        for (int r = 0; r < half; r++) {
            int ya = mid - (r + 1) * PITCH + 1, yb = mid + r * PITCH + 1;
            uint8_t t = (r * 255) / (half > 1 ? half - 1 : 1);
            if (r < na) dot(buf, x, ya, lv_color_mix(g->a_hi, g->a_lo, t), 255);
            else dot(buf, x, ya, grid, 30);
            if (r < nb) dot(buf, x, yb, lv_color_mix(g->b_hi, g->b_lo, t), 255);
            else dot(buf, x, yb, grid, 30);
        }
    }
    lv_obj_invalidate(c);
}
