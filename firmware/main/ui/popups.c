// Popups with data fetched from the PC on demand:
//  - system info: the PC's fastfetch output, grouped as its config groups it, with its Nerd Font
//    icons and its logo drawn as pixel art
//  - processes: a btop-style process table, refreshed every 2 s while open
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "ui_internal.h"

// ---------------------------------------------------------------- request / poll

typedef void (*reply_fn)(const cJSON *msg);

static struct {
    lv_obj_t *modal, *status;
    lv_timer_t *timer;
    int req;
    uint32_t asked_at;
    reply_fn on_reply;
    const char *kind;
    uint32_t every;  // re-ask this often (ms), 0: once
} s_pop;

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    const char *raw = s_pop.req ? hub_full_result(s_pop.req) : NULL;
    if (raw) {
        cJSON *m = cJSON_Parse(raw);
        if (m) {
            if (s_pop.status) lv_label_set_text(s_pop.status, "");
            s_pop.on_reply(m);
            cJSON_Delete(m);
        }
        s_pop.req = 0;
    } else if (s_pop.req && lv_tick_elaps(s_pop.asked_at) > 6000) {
        if (s_pop.status) lv_label_set_text(s_pop.status, "The PC didn't answer");
        s_pop.req = 0;
    }
    if (!s_pop.req && s_pop.every && lv_tick_elaps(s_pop.asked_at) >= s_pop.every) {
        s_pop.req = hub_request(s_pop.kind);
        s_pop.asked_at = lv_tick_get();
    }
}

static void pop_deleted(lv_event_t *e)
{
    (void)e;
    if (s_pop.timer) lv_timer_delete(s_pop.timer);
    memset(&s_pop, 0, sizeof s_pop);
}

static void close_cb(lv_event_t *e) { ui_modal_close(lv_event_get_user_data(e)); }

/// A modal with a title row (title, status, Close); returns the card.
static lv_obj_t *pop_open(const char *title, const char *kind, reply_fn on_reply, uint32_t every, int w, int h)
{
    if (s_pop.modal) lv_obj_delete(lv_obj_get_parent(s_pop.modal));  // one at a time (resets s_pop)
    lv_obj_t *m = ui_modal(w, h);
    lv_obj_t *t = ui_label(m, P.f_head_s, P.text, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t *x = ui_button(m, ICON_CLOSE, "Close", false);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(x, close_cb, LV_EVENT_CLICKED, m);
    s_pop.status = ui_label(m, P.f14, P.text3, "Loading…");
    lv_obj_align_to(s_pop.status, x, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    s_pop.modal = m;
    s_pop.on_reply = on_reply;
    s_pop.kind = kind;
    s_pop.every = every;
    s_pop.req = hub_request(kind);
    s_pop.asked_at = lv_tick_get();
    if (!s_pop.req) lv_label_set_text(s_pop.status, "No PC connected");
    s_pop.timer = lv_timer_create(poll_cb, 100, NULL);
    lv_obj_add_event_cb(m, pop_deleted, LV_EVENT_DELETE, NULL);
    return m;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

// ---------------------------------------------------------------- system info

static lv_obj_t *s_sys_logo_box, *s_sys_list;

static void logo_freed(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

/// fastfetch's logo (block art) as pixels: each character cell a block, half blocks halved.
static void draw_logo(lv_obj_t *parent, const cJSON *lines, int max_w, int max_h)
{
    int rows = cJSON_GetArraySize(lines), cols = 0;
    if (!rows) return;
    for (int r = 0; r < rows; r++) {
        const char *s = cJSON_GetArrayItem(lines, r)->valuestring;
        int n = 0;
        for (const char *p = s; p && *p; p++) n += ((*p & 0xc0) != 0x80);
        if (n > cols) cols = n;
    }
    int cw = max_w / cols, ch = cw * 2;
    if (ch * rows > max_h) {
        ch = max_h / rows;
        cw = ch / 2;
    }
    if (cw < 1 || ch < 2) return;
    int w = cw * cols, h = ch * rows;
    lv_color32_t *buf = lv_malloc_zeroed((size_t)w * h * 4);
    if (!buf) return;
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_event_cb(cv, logo_freed, LV_EVENT_DELETE, buf);
    // The accent, fading a little towards the bottom.
    lv_color_t top = P.accent, bottom = lv_color_mix(P.accent, P.light ? lv_color_black() : lv_color_white(), 200);
    for (int r = 0; r < rows; r++) {
        const char *p = cJSON_GetArrayItem(lines, r)->valuestring;
        lv_color_t c = lv_color_mix(bottom, top, (uint8_t)(255 * r / (rows > 1 ? rows - 1 : 1)));
        for (int col = 0; p && *p; col++) {
            uint32_t cp;
            int len = (*p & 0x80) == 0 ? 1 : (*p & 0xe0) == 0xc0 ? 2 : (*p & 0xf0) == 0xe0 ? 3 : 4;
            cp = len == 1 ? (uint8_t)*p : len == 3 ? ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f) : 0x2588;
            p += len;
            if (cp == ' ') continue;
            int y0 = 0, y1 = ch, x0 = 0, x1 = cw;
            uint8_t a = 255;
            if (cp == 0x2580) y1 = ch / 2;        // ▀
            else if (cp == 0x2584) y0 = ch / 2;   // ▄
            else if (cp == 0x258c) x1 = cw / 2;   // ▌
            else if (cp == 0x2590) x0 = cw / 2;   // ▐
            else if (cp == 0x2591) a = 70;        // ░
            else if (cp == 0x2592) a = 130;       // ▒
            else if (cp == 0x2593) a = 190;       // ▓
            else if (cp != 0x2588) a = 150;       // letters of an ASCII logo
            for (int y = r * ch + y0; y < r * ch + y1; y++)
                for (int x = col * cw + x0; x < col * cw + x1 && x < w; x++)
                    buf[y * w + x] = (lv_color32_t){.red = c.red, .green = c.green, .blue = c.blue, .alpha = a};
        }
    }
    lv_obj_invalidate(cv);
}

static void sysinfo_reply(const cJSON *m)
{
    lv_obj_clean(s_sys_list);
    const char *err = jstr(m, "error");
    if (err[0]) {
        ui_label(s_sys_list, P.f16, P.text3, err);
        return;
    }
    lv_obj_clean(s_sys_logo_box);
    draw_logo(s_sys_logo_box, cJSON_GetObjectItemCaseSensitive(m, "logo"), 250, 300);
    // fastfetch's key colours, one per row.
    static const uint32_t pal[] = {0x60a5fa, 0xa78bfa, 0xf472b6, 0xfb923c, 0xfacc15, 0x34d399, 0x22d3ee, 0xf87171};
    int k = 0;
    const cJSON *sec;
    cJSON_ArrayForEach(sec, cJSON_GetObjectItemCaseSensitive(m, "sections"))
    {
        const char *title = jstr(sec, "title");
        if (title[0]) {
            lv_obj_t *t = ui_title(s_sys_list, title);
            lv_obj_set_style_text_color(t, P.accent, 0);
            lv_obj_set_style_margin_top(t, lv_obj_get_child_count(s_sys_list) > 1 ? 10 : 0, 0);
        }
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(sec, "rows"))
        {
            lv_obj_t *r = ui_box(s_sys_list);
            lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_column(r, 10, 0);
            lv_color_t c = lv_color_hex(pal[k++ % (sizeof pal / sizeof *pal)]);
            if (P.light) c = lv_color_darken(c, 40);
            lv_obj_t *ic = ui_label(r, &nf_22, c, jstr(row, "icon"));
            lv_obj_set_width(ic, 28);
            lv_obj_t *key = ui_label(r, P.f16, c, jstr(row, "key"));
            lv_obj_set_width(key, 104);
            ui_one_line(key);
            lv_obj_t *v = ui_label(r, P.f16, P.text, jstr(row, "value"));
            lv_obj_set_flex_grow(v, 1);
            lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
            // Colour swatches (a theme's palette)
            const cJSON *col;
            cJSON_ArrayForEach(col, cJSON_GetObjectItemCaseSensitive(row, "colors"))
            {
                lv_obj_t *d = ui_box(r);
                lv_obj_set_size(d, 14, 14);
                lv_obj_set_style_margin_top(d, 4, 0);
                lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(d, lv_color_hex(strtoul(col->valuestring + 1, NULL, 16)), 0);
            }
        }
    }
}

void popup_sysinfo_open(void)
{
    lv_obj_t *m = pop_open("System", "sysinfo", sysinfo_reply, 0, 980, 556);
    if (!m) return;
    s_sys_logo_box = ui_box(m);
    lv_obj_set_size(s_sys_logo_box, 260, 440);
    lv_obj_set_pos(s_sys_logo_box, 0, 60);
    lv_obj_set_flex_flow(s_sys_logo_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_sys_logo_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_sys_list = ui_box(m);
    lv_obj_set_size(s_sys_list, 932 - 284, 444);
    lv_obj_set_pos(s_sys_list, 284, 60);
    lv_obj_set_flex_flow(s_sys_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sys_list, 8, 0);
    lv_obj_add_flag(s_sys_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_sys_list, LV_DIR_VER);
}

// ---------------------------------------------------------------- processes (btop)

#define PROC_N 40
typedef struct {
    lv_obj_t *row, *pid, *name, *cmd, *thr, *user, *mem, *bar, *cpu;
} prow_t;
static prow_t *s_rows;
static lv_obj_t *s_proc_count;

static lv_color_t load_color(double pct)
{
    // btop's cpu gradient: green, through yellow, to red.
    if (pct < 30) return C_DONE;
    return pct < 70 ? C_QUESTION : C_ERROR;
}

static void procs_reply(const cJSON *m)
{
    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(m, "procs");
    int n = cJSON_GetArraySize(ps);
    lv_label_set_text_fmt(s_proc_count, "%d processes  ·  by CPU", (int)jnum(m, "count"));
    double top = 1;
    for (int i = 0; i < n; i++) {
        double c = jnum(cJSON_GetArrayItem(ps, i), "cpu");
        if (c > top) top = c;
    }
    for (int i = 0; i < PROC_N; i++) {
        prow_t *r = &s_rows[i];
        if (i >= n) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        const cJSON *p = cJSON_GetArrayItem(ps, i);
        char b[24];
        double cpu = jnum(p, "cpu");
        lv_label_set_text_fmt(r->pid, "%d", (int)jnum(p, "pid"));
        lv_label_set_text(r->name, jstr(p, "name"));
        lv_label_set_text(r->cmd, jstr(p, "cmd"));
        lv_label_set_text_fmt(r->thr, "%d", (int)jnum(p, "threads"));
        lv_label_set_text(r->user, jstr(p, "user"));
        fmt_bytes(b, sizeof b, jnum(p, "mem"));
        lv_label_set_text(r->mem, b);
        lv_label_set_text_fmt(r->cpu, "%.1f", cpu);
        lv_obj_set_style_text_color(r->cpu, cpu >= 1 ? load_color(cpu) : P.text2, 0);
        lv_bar_set_value(r->bar, (int)(cpu * 1000 / top), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(r->bar, load_color(cpu), LV_PART_INDICATOR);
    }
}

// Columns: x, width
enum { X_PID = 0, W_PID = 70, X_NAME = 78, W_NAME = 150, X_CMD = 236, W_CMD = 330, X_THR = 574, W_THR = 56,
       X_USER = 640, W_USER = 92, X_MEM = 740, W_MEM = 84, X_BAR = 836, W_BAR = 40, X_CPU = 882, W_CPU = 54 };

static lv_obj_t *cell(lv_obj_t *row, const lv_font_t *f, lv_color_t c, int x, int w, bool right)
{
    lv_obj_t *l = ui_label(row, f, c, "");
    lv_obj_set_pos(l, x, 0);
    lv_obj_set_width(l, w);
    ui_one_line(l);
    if (right) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

static void procs_deleted(lv_event_t *e)
{
    (void)e;
    lv_free(s_rows);
    s_rows = NULL;
}

void popup_procs_open(void)
{
    lv_obj_t *m = pop_open("Processes", "procs", procs_reply, 2000, 1000, 572);
    if (!m) return;
    s_proc_count = ui_label(m, P.f14, P.text3, "");
    lv_obj_align(s_proc_count, LV_ALIGN_TOP_LEFT, 150, 10);
    // Column heads, btop's
    lv_obj_t *hd = ui_box(m);
    lv_obj_set_size(hd, 952, 22);
    lv_obj_set_pos(hd, 0, 58);
    static const struct {
        const char *t;
        int x, w;
        bool right;
    } cols[] = {{"Pid", X_PID, W_PID, true}, {"Program", X_NAME, W_NAME, false}, {"Command", X_CMD, W_CMD, false},
                {"Thr", X_THR, W_THR, true}, {"User", X_USER, W_USER, false}, {"MemB", X_MEM, W_MEM, true},
                {"Cpu%", X_CPU, W_CPU, true}};
    for (size_t i = 0; i < sizeof cols / sizeof *cols; i++) {
        lv_obj_t *l = cell(hd, P.f14, P.accent, cols[i].x, cols[i].w, cols[i].right);
        lv_label_set_text(l, cols[i].t);
    }
    lv_obj_t *list = ui_box(m);
    lv_obj_set_size(list, 952, 572 - 48 - 86);
    lv_obj_set_pos(list, 0, 86);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    s_rows = lv_malloc_zeroed(sizeof(prow_t) * PROC_N);
    lv_obj_add_event_cb(m, procs_deleted, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < PROC_N; i++) {
        prow_t *r = &s_rows[i];
        r->row = ui_box(list);
        lv_obj_set_size(r->row, 952, 24);
        r->pid = cell(r->row, P.fmono, P.text3, X_PID, W_PID, true);
        r->name = cell(r->row, P.f16, P.text, X_NAME, W_NAME, false);
        r->cmd = cell(r->row, P.fmono, P.text3, X_CMD, W_CMD, false);
        r->thr = cell(r->row, P.fmono, P.text2, X_THR, W_THR, true);
        r->user = cell(r->row, P.f16, P.text2, X_USER, W_USER, false);
        r->mem = cell(r->row, P.fmono, P.text2, X_MEM, W_MEM, true);
        r->bar = ui_bar(r->row, W_BAR, 6, C_DONE, C_DONE);
        lv_obj_set_pos(r->bar, X_BAR, 9);
        r->cpu = cell(r->row, P.fmono, P.text, X_CPU, W_CPU, true);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
}
