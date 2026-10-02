// System page, btop-style but laid out for a 1024x600 panel and readable at a glance. Values are
// colour-coded (load and temperature turn green / amber / red) and every figure has an icon.
//   CPU: load ring | temp, power, clock, load-average tiles | per-thread heat strip
//   GPU: load ring | VRAM, power, fan bars | temperature, clock
//   Memory: used of total, stacked bar, one bar per kind   Storage (data volumes only)
//   Network                                                Processes (icon, name, CPU bar, memory)
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"

#define PROC_ROWS 9
#define DISK_ROWS 2
#define MEM_ROWS 3
#define GPU_ROWS 4

typedef struct {
    lv_obj_t *icon, *value, *unit, *caption;
} tile_t;

// CPU
static lv_obj_t *s_cpu_name, *s_cpu_arc, *s_cpu_pct, *s_cores[MAX_CORES], *s_cores_box;
static tile_t s_cpu_tile[4];
static int s_ncores_laid;
// GPU
static lv_obj_t *s_gpu_name, *s_gpu_arc, *s_gpu_pct, *s_gpu_bar[GPU_ROWS], *s_gpu_val[GPU_ROWS];
// Memory
static lv_obj_t *s_mem_big, *s_mem_of, *s_mem_seg[3], *s_mem_val[MEM_ROWS], *s_mem_bar[MEM_ROWS], *s_mem_pct[MEM_ROWS];
// Storage
static lv_obj_t *s_disk_row[DISK_ROWS], *s_disk_name[DISK_ROWS], *s_disk_val[DISK_ROWS], *s_disk_bar[DISK_ROWS],
    *s_disk_free[DISK_ROWS], *s_io;
// Network
static lv_obj_t *s_net_iface, *s_net_down, *s_net_up, *s_net_chart, *s_net_total;
// Processes
static lv_obj_t *s_proc_icon[PROC_ROWS], *s_proc_name[PROC_ROWS], *s_proc_bar[PROC_ROWS], *s_proc_cpu[PROC_ROWS],
    *s_proc_mem[PROC_ROWS];

// ---------------------------------------------------------------- colour coding

static lv_color_t load_color(float pct)
{
    if (isnan(pct) || pct < 50) return C_DONE;
    return pct < 80 ? C_QUESTION : C_ERROR;
}

static lv_color_t temp_color(float c)
{
    if (isnan(c) || c < 65) return C_DONE;
    return c < 82 ? C_QUESTION : C_ERROR;
}

// ---------------------------------------------------------------- builders

static lv_obj_t *header(lv_obj_t *card, const lv_image_dsc_t *icon, const char *title, int right_w)
{
    ui_icon(card, icon);
    lv_obj_t *t = ui_title(card, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 36, 14 - lv_font_get_line_height(lv_obj_get_style_text_font(t, 0)) / 2);
    lv_obj_t *r = ui_label(card, P.f14, P.text3, "");
    if (right_w) {
        lv_obj_set_width(r, right_w);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        ui_one_line(r);
    }
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, 0, 4);
    return r;
}

/// Load ring with the percentage in the middle.
static lv_obj_t *ring(lv_obj_t *parent, int x, int y, int size, const lv_image_dsc_t *tex, lv_color_t flat, lv_obj_t **pct)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_set_pos(a, x, y);
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_range(a, 0, 1000);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 10, 0);
    lv_obj_set_style_arc_width(a, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, P.flat ? P.card2 : P.light ? lv_color_hex(0xe3e8f2) : lv_color_hex(0x1f2740), 0);
    lv_obj_set_style_arc_rounded(a, !P.editorial, LV_PART_INDICATOR);
    if (P.flat) lv_obj_set_style_arc_color(a, flat, LV_PART_INDICATOR);
    else lv_obj_set_style_arc_image_src(a, tex, LV_PART_INDICATOR);
    *pct = ui_label(parent, P.f_num, P.text, "–");
    lv_obj_align_to(*pct, a, LV_ALIGN_CENTER, 0, 0);
    return a;
}

/// Icon + value + caption tile (CPU facts).
static void tile_create(tile_t *t, lv_obj_t *parent, int x, int y, const lv_image_dsc_t *img, const char *glyph, lv_color_t gc)
{
    t->icon = img ? ui_icon(parent, img) : ui_glyph(parent, &icons_22, gc, glyph);
    lv_obj_set_pos(t->icon, x, y + 4);
    t->value = ui_label(parent, P.f20, P.text, "–");
    lv_obj_set_pos(t->value, x + 30, y);
    // Units in a smaller, quieter face so the number reads first and tiles don't crowd.
    t->unit = ui_label(parent, P.f14, P.text3, "");
    t->caption = ui_label(parent, P.f14, P.text3, "");
    lv_obj_set_pos(t->caption, x + 30, y + 30);
    lv_obj_set_width(t->caption, 96);
    ui_one_line(t->caption);
}

/// Icon + label, value on the right, a bar underneath.
static void bar_row(lv_obj_t *parent, int y, int w, const char *glyph, lv_color_t c, const char *label, lv_obj_t **val,
                    lv_obj_t **bar, lv_obj_t **pct)
{
    lv_obj_t *g = ui_glyph(parent, &icons_22, c, glyph);
    lv_obj_set_pos(g, 0, y);
    lv_obj_t *l = ui_label(parent, P.f16, P.text2, label);
    lv_obj_set_pos(l, 30, y + 1);
    *val = ui_label(parent, P.f18, P.text, "–");
    lv_obj_set_width(*val, w - 150);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(*val, 150, y);
    *bar = ui_bar(parent, pct ? w - 30 - 48 : w - 30, 6, c, lv_color_mix(c, lv_color_white(), 200));
    lv_obj_set_pos(*bar, 30, y + 30);
    if (pct) {
        *pct = ui_label(parent, P.f14, P.text3, "");
        lv_obj_set_width(*pct, 42);
        lv_obj_set_style_text_align(*pct, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(*pct, w - 42, y + 23);
    }
}

static lv_obj_t *core_bar(lv_obj_t *parent)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_radius(b, P.editorial ? 0 : 2, 0);
    lv_obj_set_style_radius(b, P.editorial ? 0 : 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, P.light ? lv_color_darken(P.card2, 18) : lv_color_lighten(P.card2, 10), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_DONE, LV_PART_INDICATOR);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

// ---------------------------------------------------------------- create

#define TOP_H 196
#define BOT_Y (4 + TOP_H + 8)
#define BOT_H (520 - BOT_Y)
#define STORAGE_H 150
#define CORES_W 136
#define CORES_H 124

static void procs_click(lv_event_t *e)
{
    (void)e;
    popup_procs_open();
}

void page_system_create(lv_obj_t *tile)
{
    // ---- CPU
    lv_obj_t *c = ui_card(tile, 16, 4, 560, TOP_H);
    s_cpu_name = header(c, &img_hw_cpu_28, "CPU", 340);
    s_cpu_arc = ring(c, 0, 40, 120, &img_ring_cpu, C_CPU, &s_cpu_pct);
    tile_create(&s_cpu_tile[0], c, 136, 40, &img_hw_temp_22, NULL, P.text);
    tile_create(&s_cpu_tile[1], c, 262, 40, &img_hw_bolt_22, NULL, P.text);
    tile_create(&s_cpu_tile[2], c, 136, 106, NULL, ICON_SPEED, C_RAM);
    tile_create(&s_cpu_tile[3], c, 262, 106, NULL, ICON_PULSE, C_GPU);
    s_cores_box = ui_box(c);
    lv_obj_set_pos(s_cores_box, 560 - 32 - CORES_W, 40);
    lv_obj_set_size(s_cores_box, CORES_W, CORES_H);
    for (int i = 0; i < MAX_CORES; i++) s_cores[i] = core_bar(s_cores_box);
    s_ncores_laid = 0;  // fresh bars (a theme change rebuilds the page): lay them out again

    // ---- GPU: ring | VRAM, power, temperature, fan
    lv_obj_t *g = ui_card(tile, 584, 4, 424, TOP_H);
    s_gpu_name = header(g, &img_hw_gpu_28, "GPU", 240);
    s_gpu_arc = ring(g, 0, 40, 120, &img_ring_gpu, C_GPU, &s_gpu_pct);
    static const char *names[] = {"VRAM", "Power", "Temp", "Fan"};
    static const char *glyphs[] = {ICON_RAM, ICON_POWER, ICON_TEMP, ICON_FAN};
    const lv_color_t col[] = {C_VRAM, lv_color_hex(0xf59e0b), C_DONE, C_RAM};
    for (int i = 0; i < GPU_ROWS; i++) {
        lv_obj_t *box = ui_box(g);
        lv_obj_set_pos(box, 136, 34 + i * 32);
        lv_obj_set_size(box, 256, 32);
        lv_obj_t *gl = ui_glyph(box, &icons_18, col[i], glyphs[i]);
        lv_obj_set_pos(gl, 0, 2);
        lv_obj_t *l = ui_label(box, P.f16, P.text2, names[i]);
        lv_obj_set_pos(l, 24, 0);
        s_gpu_val[i] = ui_label(box, P.f16, P.text, "–");
        lv_obj_set_width(s_gpu_val[i], 170);
        lv_obj_set_style_text_align(s_gpu_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_gpu_val[i], LV_ALIGN_TOP_RIGHT, 0, 0);
        s_gpu_bar[i] = ui_bar(box, 232, 5, col[i], lv_color_mix(col[i], lv_color_white(), 200));
        lv_obj_set_pos(s_gpu_bar[i], 24, 25);
    }

    // ---- Memory: used of total, stacked bar (used | cached | free), then one bar per kind
    lv_obj_t *m = ui_card(tile, 16, BOT_Y, 300, BOT_H);
    header(m, &img_hw_ram_28, "Memory", 0);
    s_mem_big = ui_label(m, P.f_num, P.text, "–");
    lv_obj_set_pos(s_mem_big, 0, 34);
    s_mem_of = ui_label(m, P.f16, P.text3, "");
    lv_obj_t *track = ui_box(m);
    lv_obj_set_pos(track, 0, 76);
    lv_obj_set_size(track, 268, 12);
    lv_obj_set_style_radius(track, P.editorial ? 0 : 6, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_set_style_bg_color(track, P.card2, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(track, LV_FLEX_FLOW_ROW);
    const lv_color_t mc[] = {C_RAM, lv_color_hex(0xa78bfa), C_DONE, lv_color_hex(0xf59e0b)};
    for (int i = 0; i < 2; i++) {
        s_mem_seg[i] = ui_box(track);
        lv_obj_set_size(s_mem_seg[i], 0, 12);
        lv_obj_set_style_bg_color(s_mem_seg[i], mc[i], 0);
        lv_obj_set_style_bg_opa(s_mem_seg[i], LV_OPA_COVER, 0);
    }
    s_mem_seg[2] = NULL;
    static const char *rows[] = {"Used", "Cached", "Available"};
    static const char *mg[] = {ICON_RAM, ICON_LAYERS, ICON_DONE};
    for (int i = 0; i < MEM_ROWS; i++)
        bar_row(m, 104 + i * 56, 268, mg[i], mc[i], rows[i], &s_mem_val[i], &s_mem_bar[i], &s_mem_pct[i]);

    // ---- Storage: data volumes only. Each: mount, percent used, a thick bar; one volume also
    // gets used / free spelled out.
    lv_obj_t *d = ui_card(tile, 324, BOT_Y, 300, STORAGE_H);
    header(d, &img_hw_ssd_28, "Storage", 0);
    s_io = ui_label(d, P.f14, P.text3, "");
    lv_obj_align(s_io, LV_ALIGN_BOTTOM_LEFT, 0, 4);
    for (int i = 0; i < DISK_ROWS; i++) {
        lv_obj_t *r = s_disk_row[i] = ui_box(d);
        lv_obj_set_pos(r, 0, 34 + i * 46);
        lv_obj_set_size(r, 268, 64);
        lv_obj_t *gl = ui_glyph(r, &icons_22, C_DISK, ICON_DISK);
        lv_obj_set_pos(gl, 0, 0);
        s_disk_name[i] = ui_label(r, P.f16, P.text, "");
        lv_obj_set_pos(s_disk_name[i], 30, 0);
        lv_obj_set_width(s_disk_name[i], 110);
        ui_one_line(s_disk_name[i]);
        s_disk_val[i] = ui_label(r, P.f18, P.text, "");
        lv_obj_align(s_disk_val[i], LV_ALIGN_TOP_RIGHT, 0, -2);
        s_disk_bar[i] = ui_bar(r, 238, 10, C_DISK, lv_color_mix(C_DISK, lv_color_white(), 200));
        lv_obj_set_pos(s_disk_bar[i], 30, 27);
        s_disk_free[i] = ui_label(r, P.f14, P.text3, "");
        lv_obj_set_pos(s_disk_free[i], 30, 41);
        lv_obj_add_flag(s_disk_free[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Network
    lv_obj_t *n = ui_card(tile, 324, BOT_Y + STORAGE_H + 8, 300, BOT_H - STORAGE_H - 8);
    s_net_iface = header(n, &img_hw_net_28, "Network", 110);
    lv_obj_t *dn = ui_glyph(n, &icons_22, C_NET_DOWN, ICON_DOWN);
    lv_obj_set_pos(dn, 0, 38);
    s_net_down = ui_label(n, P.f20, P.text, "–");
    lv_obj_set_pos(s_net_down, 26, 34);
    lv_obj_t *up = ui_glyph(n, &icons_22, C_NET_UP, ICON_UP);
    lv_obj_set_pos(up, 140, 38);
    s_net_up = ui_label(n, P.f20, P.text, "–");
    lv_obj_set_pos(s_net_up, 166, 34);
    s_net_chart = dotgraph_create(n, 268, 44, true, lv_color_mix(C_NET_DOWN, P.card, 150), C_NET_DOWN,
                                  lv_color_mix(C_NET_UP, P.card, 150), C_NET_UP);
    lv_obj_set_pos(s_net_chart, 0, 64);
    s_net_total = ui_label(n, P.f14, P.text3, "");
    lv_obj_align(s_net_total, LV_ALIGN_BOTTOM_LEFT, 0, 4);

    // ---- Processes: icon, name, CPU bar + value, memory
    lv_obj_t *p = ui_card(tile, 632, BOT_Y, 376, BOT_H);
    header(p, &img_fluent_gauge_28, "Processes", 0);
    // Tap for the full table (btop-style).
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p, procs_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *hc = ui_label(p, P.f14, P.text3, "CPU %");
    lv_obj_set_pos(hc, 180, 34);
    lv_obj_t *hm = ui_label(p, P.f14, P.text3, "MEMORY");
    lv_obj_set_width(hm, 74);
    lv_obj_set_style_text_align(hm, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(hm, 270, 34);
    for (int i = 0; i < PROC_ROWS; i++) {
        int y = 58 + i * 24;
        s_proc_icon[i] = ui_glyph(p, &icons_18, P.text3, ICON_APPS);
        lv_obj_set_pos(s_proc_icon[i], 0, y + 2);
        s_proc_name[i] = ui_label(p, P.f16, P.text, "");
        lv_obj_set_pos(s_proc_name[i], 28, y);
        lv_obj_set_width(s_proc_name[i], 148);
        ui_one_line(s_proc_name[i]);
        s_proc_bar[i] = ui_bar(p, 32, 6, C_CPU, C_CPU);
        lv_obj_set_pos(s_proc_bar[i], 180, y + 9);
        s_proc_cpu[i] = ui_label(p, P.fmono, P.text, "");
        lv_obj_set_width(s_proc_cpu[i], 46);
        lv_obj_set_style_text_align(s_proc_cpu[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_proc_cpu[i], 212, y + 1);
        s_proc_mem[i] = ui_label(p, P.fmono, P.text2, "");
        lv_obj_set_width(s_proc_mem[i], 74);
        lv_obj_set_style_text_align(s_proc_mem[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_proc_mem[i], 270, y + 1);
        ui_one_line(s_proc_mem[i]);
        lv_obj_remove_flag(s_proc_bar[i], LV_OBJ_FLAG_CLICKABLE);  // bars take taps by default: the card gets them
    }
}

// ---------------------------------------------------------------- update

static void layout_cores(int n)
{
    if (n == s_ncores_laid) return;
    s_ncores_laid = n;
    // Eight bars per row, as many rows as it takes: a grid of thread heat cells.
    int rows = n > 8 ? (n + 7) / 8 : 1, per = (n + rows - 1) / rows, gap = 3;
    int w = per ? (CORES_W - gap * (per - 1)) / per : 0, h = (CORES_H - gap * (rows - 1)) / rows;
    for (int i = 0; i < MAX_CORES; i++) {
        if (i < n) {
            lv_obj_remove_flag(s_cores[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_cores[i], w, h);
            lv_obj_set_pos(s_cores[i], (i % per) * (w + gap), (i / per) * (h + 4));
        } else {
            lv_obj_add_flag(s_cores[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/// A recognisable glyph for well-known programs.
static const char *proc_icon(const char *name, lv_color_t *c)
{
    static const struct {
        const char *needle, *icon;
        uint32_t color;
    } map[] = {
        {"chrom", ICON_GLOBE, 0x60a5fa},   {"firefox", ICON_GLOBE, 0xfb923c}, {"brave", ICON_GLOBE, 0xfb923c},
        {"claude", ICON_AGENT, 0xd97757},  {"codex", ICON_AGENT, 0x10a37f},   {"pi", ICON_AGENT, 0xa78bfa},
        {"omp", ICON_AGENT, 0xa78bfa},     {"opencode", ICON_AGENT, 0xf59e0b},
        {"code", ICON_CODE, 0x3b82f6},     {"clangd", ICON_CODE, 0x3b82f6},   {"cargo", ICON_BUILD, 0xf97316},
        {"rustc", ICON_BUILD, 0xf97316},   {"gcc", ICON_BUILD, 0xf97316},     {"node", ICON_CODE, 0x22c55e},
        {"kitty", ICON_TERMINAL, 0xa3a3a3}, {"alacritty", ICON_TERMINAL, 0xa3a3a3}, {"foot", ICON_TERMINAL, 0xa3a3a3},
        {"bash", ICON_TERMINAL, 0xa3a3a3}, {"zsh", ICON_TERMINAL, 0xa3a3a3},  {"btop", ICON_PULSE, 0x34d399},
        {"Hyprland", ICON_DESKTOP, 0x22d3ee}, {"quickshell", ICON_DESKTOP, 0x22d3ee}, {"steam", ICON_GAME, 0x60a5fa},
        {"spotify", ICON_MUSIC, 0x22c55e}, {"pipewire", ICON_MUSIC, 0xa78bfa}, {"deskhud", ICON_PULSE, 0xfacc15},
        {"ttlcd", ICON_PULSE, 0xfacc15},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strstr(name, map[i].needle)) {
            *c = lv_color_hex(map[i].color);
            return map[i].icon;
        }
    *c = P.text3;
    return ICON_APPS;
}

/// Data volumes only: no /boot, /efi or other small system partitions.
static bool worth_showing(const char *mount, uint64_t total)
{
    if (!strncmp(mount, "/boot", 5) || !strncmp(mount, "/efi", 4)) return false;
    return total >= 8ULL * 1024 * 1024 * 1024;
}

static void tile_unit(tile_t *t, const char *unit)
{
    lv_label_set_text(t->unit, unit);
    lv_obj_align_to(t->unit, t->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -3);
}

void page_system_update(const ui_data_t *d)
{
    if (!d->stats_changed) return;
    const stats_t *s = d->stats;
    char a[24], b[24], t[16];

    // ---- CPU
    fmt_duration(a, sizeof a, s->uptime);
    lv_label_set_text_fmt(s_cpu_name, "%s  ·  %d threads  ·  up %s", s->cpu_name, s->ncores, a);
    lv_arc_set_value(s_cpu_arc, isnan(s->cpu_load) ? 0 : (int)(s->cpu_load * 10));
    lv_label_set_text_fmt(s_cpu_pct, isnan(s->cpu_load) ? "–" : "%.0f%%", s->cpu_load);
    lv_obj_align_to(s_cpu_pct, s_cpu_arc, LV_ALIGN_CENTER, 0, 0);
    fmt_temp(t, sizeof t, s->cpu_temp);
    lv_label_set_text(s_cpu_tile[0].value, t);
    lv_obj_set_style_text_color(s_cpu_tile[0].value, temp_color(s->cpu_temp), 0);
    if (s->ntemps >= 2) {
        char t1[16], t2[16];
        fmt_temp(t1, sizeof t1, s->temps[0].temp);
        fmt_temp(t2, sizeof t2, s->temps[1].temp);
        lv_label_set_text_fmt(s_cpu_tile[0].caption, "CCD %s %s", t1, t2);
    } else {
        lv_label_set_text(s_cpu_tile[0].caption, "package");
    }
    if (!isnan(s->cpu_power)) lv_label_set_text_fmt(s_cpu_tile[1].value, "%.0f", s->cpu_power);
    else lv_label_set_text(s_cpu_tile[1].value, "–");
    tile_unit(&s_cpu_tile[1], isnan(s->cpu_power) ? "" : "W");
    lv_label_set_text(s_cpu_tile[1].caption, "package");
    if (s->cpu_mhz) lv_label_set_text_fmt(s_cpu_tile[2].value, "%.1f", s->cpu_mhz / 1000.0);
    tile_unit(&s_cpu_tile[2], s->cpu_mhz ? "GHz" : "");
    lv_label_set_text(s_cpu_tile[2].caption, "fastest core");
    lv_label_set_text_fmt(s_cpu_tile[3].value, "%.2f", s->load[0]);
    lv_label_set_text_fmt(s_cpu_tile[3].caption, "%.1f  %.1f", s->load[1], s->load[2]);
    layout_cores(s->ncores);
    for (int i = 0; i < s->ncores; i++) {
        // A heat map: each cell tinted by its thread's load (faint when idle), the bar fills on top.
        float c = isnan(s->cores[i]) ? 0 : s->cores[i];
        lv_color_t lc = load_color(c), track = P.light ? lv_color_darken(P.card2, 18) : lv_color_lighten(P.card2, 10);
        lv_obj_set_style_bg_color(s_cores[i], lv_color_mix(lc, track, (lv_opa_t)(60 + fminf(c, 100) * 1.2f)), 0);
        lv_bar_set_value(s_cores[i], c < 2 ? 2 : (int)c, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cores[i], lc, LV_PART_INDICATOR);
    }

    // ---- GPU
    if (s->ngpus) {
        const typeof(s->gpus[0]) *g = &s->gpus[0];
        lv_label_set_text_fmt(s_gpu_name, "%s  ·  %d MHz", g->name, g->mhz);
        lv_arc_set_value(s_gpu_arc, isnan(g->load) ? 0 : (int)(g->load * 10));
        lv_label_set_text_fmt(s_gpu_pct, isnan(g->load) ? "–" : "%.0f%%", g->load);
        lv_obj_align_to(s_gpu_pct, s_gpu_arc, LV_ALIGN_CENTER, 0, 0);
        lv_bar_set_value(s_gpu_bar[0], g->vram_total ? (int)(g->vram_used * 1000 / g->vram_total) : 0, LV_ANIM_ON);
        fmt_bytes(a, sizeof a, g->vram_used);
        fmt_bytes(b, sizeof b, g->vram_total);
        lv_label_set_text_fmt(s_gpu_val[0], "%s / %s", a, b);
        bool pw = !isnan(g->power) && !isnan(g->power_max) && g->power_max > 0;
        lv_bar_set_value(s_gpu_bar[1], pw ? (int)(g->power * 1000 / g->power_max) : 0, LV_ANIM_ON);
        if (pw) lv_label_set_text_fmt(s_gpu_val[1], "%.0f / %.0f W", g->power, g->power_max);
        else if (!isnan(g->power)) lv_label_set_text_fmt(s_gpu_val[1], "%.0f W", g->power);
        lv_bar_set_value(s_gpu_bar[2], isnan(g->temp) ? 0 : (int)(g->temp * 10), LV_ANIM_ON);
        lv_obj_set_style_bg_color(s_gpu_bar[2], temp_color(g->temp), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(s_gpu_bar[2], temp_color(g->temp), LV_PART_INDICATOR);
        fmt_temp(t, sizeof t, g->temp);
        lv_label_set_text(s_gpu_val[2], t);
        lv_obj_set_style_text_color(s_gpu_val[2], temp_color(g->temp), 0);
        lv_bar_set_value(s_gpu_bar[3], isnan(g->fan) ? 0 : (int)(g->fan * 10), LV_ANIM_ON);
        lv_label_set_text_fmt(s_gpu_val[3], isnan(g->fan) ? "–" : "%.0f%%", g->fan);
    }

    // ---- Memory
    fmt_bytes(a, sizeof a, s->mem_used);
    lv_label_set_text(s_mem_big, a);
    fmt_bytes(b, sizeof b, s->mem_total);
    lv_label_set_text_fmt(s_mem_of, "of %s", b);
    lv_obj_align_to(s_mem_of, s_mem_big, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -4);
    if (s->mem_total) {
        lv_obj_set_width(s_mem_seg[0], (int32_t)(s->mem_used * 268 / s->mem_total));
        lv_obj_set_width(s_mem_seg[1], (int32_t)(s->mem_cached * 268 / s->mem_total));
    }
    const uint64_t vals[] = {s->mem_used, s->mem_cached, s->mem_available};
    for (int i = 0; i < MEM_ROWS; i++) {
        uint64_t total = s->mem_total;
        fmt_bytes(a, sizeof a, vals[i]);
        lv_label_set_text(s_mem_val[i], a);
        int pct = total ? (int)(vals[i] * 100 / total) : 0;
        lv_bar_set_value(s_mem_bar[i], pct * 10, LV_ANIM_ON);
        lv_label_set_text_fmt(s_mem_pct[i], "%d%%", pct);
    }

    // ---- Storage
    int shown = 0;
    for (int i = 0; i < s->ndisks && shown < DISK_ROWS; i++) {
        if (!worth_showing(s->disks[i].mount, s->disks[i].total)) continue;
        int k = shown++;
        lv_obj_remove_flag(s_disk_row[k], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_disk_name[k], s->disks[i].mount);
        float pct = s->disks[i].used * 100.0f / s->disks[i].total;
        lv_label_set_text_fmt(s_disk_val[k], "%.0f%%", pct);
        lv_obj_set_style_text_color(s_disk_val[k], pct < 80 ? P.text : load_color(pct), 0);
        lv_bar_set_value(s_disk_bar[k], (int)(pct * 10), LV_ANIM_ON);
        lv_obj_set_style_bg_color(s_disk_bar[k], pct < 80 ? C_DISK : load_color(pct), LV_PART_INDICATOR);
        char f[24];
        fmt_bytes(a, sizeof a, s->disks[i].used);
        fmt_bytes(f, sizeof f, s->disks[i].total - s->disks[i].used);
        fmt_bytes(b, sizeof b, s->disks[i].total);
        lv_label_set_text_fmt(s_disk_free[k], "%s used  ·  %s free", a, f);
        (void)b;
    }
    for (int k = 0; k < DISK_ROWS; k++) {
        if (k >= shown) lv_obj_add_flag(s_disk_row[k], LV_OBJ_FLAG_HIDDEN);
        // A single volume gets its free space spelled out under the bar.
        if (shown == 1 && k == 0) lv_obj_remove_flag(s_disk_free[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_disk_free[k], LV_OBJ_FLAG_HIDDEN);
    }
    fmt_rate(a, sizeof a, s->io_read);
    fmt_rate(b, sizeof b, s->io_write);
    lv_label_set_text_fmt(s_io, "Read %s  ·  Write %s", a, b);
    // Two volumes fill the card; the I/O line only fits under one.
    if (shown > 1) lv_obj_add_flag(s_io, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_io, LV_OBJ_FLAG_HIDDEN);

    // ---- Network
    lv_label_set_text(s_net_iface, s->iface);
    fmt_rate(a, sizeof a, s->net_down);
    lv_label_set_text(s_net_down, a);
    fmt_rate(b, sizeof b, s->net_up);
    lv_label_set_text(s_net_up, b);
    // Each half scales to its own recent peak (btop's auto scaling), never below 10 KB/s.
    float pd = 10240, pu = 10240;
    for (int i = 0; i < d->hist->len; i++) {
        pd = fmaxf(pd, d->hist->down[i]);
        pu = fmaxf(pu, d->hist->up[i]);
    }
    dotgraph_set(s_net_chart, d->hist->down, d->hist->up, d->hist->len, pd, pu);
    fmt_bytes(a, sizeof a, s->rx_total);
    fmt_bytes(b, sizeof b, s->tx_total);
    char pk1[16], pk2[16];
    fmt_rate(pk1, sizeof pk1, pd);
    fmt_rate(pk2, sizeof pk2, pu);
    lv_label_set_text_fmt(s_net_total, "peak %s / %s  ·  %s \xe2\x86\x93", pk1, pk2, a);
    (void)b;

    // ---- Processes (bars relative to the busiest one, so small loads still read)
    float top = 1;
    for (int i = 0; i < s->nprocs && i < PROC_ROWS; i++) top = fmaxf(top, s->procs[i].cpu);
    for (int i = 0; i < PROC_ROWS; i++) {
        bool on = i < s->nprocs;
        lv_obj_t *objs[] = {s_proc_icon[i], s_proc_name[i], s_proc_bar[i], s_proc_cpu[i], s_proc_mem[i]};
        for (int k = 0; k < 5; k++) {
            if (on) lv_obj_remove_flag(objs[k], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(objs[k], LV_OBJ_FLAG_HIDDEN);
        }
        if (!on) continue;
        lv_color_t ic;
        lv_label_set_text(s_proc_icon[i], proc_icon(s->procs[i].name, &ic));
        lv_obj_set_style_text_color(s_proc_icon[i], ic, 0);
        lv_label_set_text(s_proc_name[i], s->procs[i].name);
        lv_bar_set_value(s_proc_bar[i], (int)(s->procs[i].cpu * 1000 / top), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_proc_bar[i], load_color(s->procs[i].cpu), LV_PART_INDICATOR);
        lv_label_set_text_fmt(s_proc_cpu[i], "%.1f", s->procs[i].cpu);
        fmt_bytes(a, sizeof a, s->procs[i].mem);
        lv_label_set_text(s_proc_mem[i], a);
    }
}
