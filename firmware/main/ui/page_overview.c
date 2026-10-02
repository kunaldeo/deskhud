#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"

#define AGENT_ROWS 4

typedef struct {
    lv_obj_t *card, *name, *arc, *value, *unit, *chip[3];
} gauge_t;

typedef struct {
    lv_obj_t *value, *sub, *bar, *value2;
} tile_t;

static gauge_t s_cpu, s_gpu;
static lv_obj_t *s_chart, *s_hist_cpu, *s_hist_gpu;
static tile_t s_ram, s_vram, s_net, s_disk;
static agent_row_t s_rows[AGENT_ROWS];
static lv_obj_t *s_agents_count, *s_agents_empty, *s_agents_more, *s_agents_list;
// One session: header + conversation fill the card.
static lv_obj_t *s_single, *s_single_logo, *s_single_project;
static status_view_t s_single_status;
static tools_view_t s_single_tools;
static ctx_view_t s_single_ctx;
static char s_single_id[96];

static lv_obj_t *chip(lv_obj_t *parent, const lv_image_dsc_t *img, const char *glyph)
{
    lv_obj_t *c = ui_box(parent);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 4, 0);
    if (img) ui_icon(c, img);
    else ui_glyph(c, &icons_18, P.text3, glyph);
    ui_label(c, P.f16, P.text2, "–");
    return c;
}

static void chip_set(lv_obj_t *c, const char *text)
{
    lv_label_set_text(lv_obj_get_child(c, 1), text);
}

static void gauge_create(gauge_t *g, lv_obj_t *tile, int x, const lv_image_dsc_t *icon, const char *title,
                         const lv_image_dsc_t *ring)
{
    g->card = ui_card(tile, x, 4, 296, 262);
    ui_icon(g->card, icon);
    lv_obj_t *t = ui_title(g->card, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 36, 14 - lv_font_get_line_height(lv_obj_get_style_text_font(t, 0)) / 2);
    g->name = ui_label(g->card, P.f14, P.text3, "");
    ui_one_line(g->name);
    lv_obj_set_width(g->name, 180);
    lv_obj_set_style_text_align(g->name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g->name, LV_ALIGN_TOP_RIGHT, 0, 6);

    g->arc = lv_arc_create(g->card);
    lv_obj_set_size(g->arc, 184, 184);
    lv_obj_align(g->arc, LV_ALIGN_TOP_MID, 0, 32);
    lv_arc_set_bg_angles(g->arc, 135, 45);
    lv_arc_set_range(g->arc, 0, 1000);
    lv_arc_set_value(g->arc, 0);
    lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g->arc, 16, 0);
    lv_obj_set_style_arc_width(g->arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g->arc, P.flat ? P.card2 : P.light ? lv_color_hex(0xe3e8f2) : lv_color_hex(0x1f2740), 0);
    lv_obj_set_style_arc_rounded(g->arc, true, LV_PART_INDICATOR);
    if (P.flat) {  // one quiet color; square ends in Newsprint
        lv_obj_set_style_arc_color(g->arc, ring == &img_ring_cpu ? C_CPU : C_GPU, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(g->arc, !P.editorial, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(g->arc, P.arc_w, 0);
        lv_obj_set_style_arc_width(g->arc, P.arc_w, LV_PART_INDICATOR);
    } else if (!(g_perf & 4)) lv_obj_set_style_arc_image_src(g->arc, ring, LV_PART_INDICATOR);
    else lv_obj_set_style_arc_color(g->arc, P.accent, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(g->arc, 0, 0);

    // Fixed width, content centred: the figure stays in the middle of the ring at 1 % or 100 %.
    lv_obj_t *mid = ui_box(g->card);
    lv_obj_set_width(mid, 152);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    g->value = ui_label(mid, P.f_num_xl, P.text, "–");
    g->unit = ui_label(mid, P.f20, P.text3, "%");
    lv_obj_set_style_pad_bottom(g->unit, 8, 0);
    lv_obj_align_to(mid, g->arc, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *cap = ui_label(g->card, P.f14, P.text3, "LOAD");
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_align_to(cap, g->arc, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *row = ui_box(g->card);
    lv_obj_set_width(row, 264);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 2);
    g->chip[0] = chip(row, &img_hw_temp_22, NULL);
    g->chip[1] = chip(row, NULL, ICON_SPEED);
    g->chip[2] = chip(row, &img_hw_bolt_22, NULL);
}

static void gauge_set(gauge_t *g, float load, const char *name, const char *c0, const char *c1, const char *c2)
{
    if (isnan(load)) {
        lv_label_set_text(g->value, "–");
        lv_arc_set_value(g->arc, 0);
    } else {
        lv_label_set_text_fmt(g->value, "%d", (int)lroundf(load));
        lv_arc_set_value(g->arc, (int)(load * 10));
    }
    lv_label_set_text(g->name, name);
    chip_set(g->chip[0], c0);
    chip_set(g->chip[1], c1);
    chip_set(g->chip[2], c2);
}

static void tile_create(tile_t *t, lv_obj_t *tile, int x, const lv_image_dsc_t *icon, const char *title, lv_color_t c1,
                        lv_color_t c2, bool two_values)
{
    lv_obj_t *c = ui_card(tile, x, 386, 144, 134);
    lv_obj_set_style_pad_all(c, 14, 0);
    ui_icon(c, icon);
    lv_obj_t *l = P.caps_titles ? ui_title(c, title) : ui_label(c, P.f14, P.text2, title);
    if (P.caps_titles) lv_obj_set_style_text_letter_space(l, 0, 0);  // narrow tiles: "NETWORK" must fit
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 36, 14 - lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)) / 2);
    if (two_values) {
        lv_obj_t *g = ui_glyph(c, &icons_18, C_NET_DOWN, ICON_DOWN);
        lv_obj_set_pos(g, -2, 46);
        t->value = ui_label(c, P.f18, P.text, "–");
        lv_obj_set_pos(t->value, 20, 44);
        g = ui_glyph(c, &icons_18, C_NET_UP, ICON_UP);
        lv_obj_set_pos(g, -2, 76);
        t->value2 = ui_label(c, P.f18, P.text, "–");
        lv_obj_set_pos(t->value2, 20, 74);
        return;
    }
    t->value = ui_label(c, P.f_num, P.text, "–");
    lv_obj_set_pos(t->value, 0, 38);
    t->sub = ui_label(c, P.f14, P.text3, "");
    lv_obj_set_pos(t->sub, 0, 68);  // >= 14 px above the bar
    t->bar = ui_bar(c, 116, 6, c1, c2);
    lv_obj_align(t->bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

void page_overview_create(lv_obj_t *tile)
{
    gauge_create(&s_cpu, tile, 16, &img_hw_cpu_28, "CPU", &img_ring_cpu);
    gauge_create(&s_gpu, tile, 320, &img_hw_gpu_28, "GPU", &img_ring_gpu);

    lv_obj_t *h = ui_card(tile, 16, 274, 600, 104);
    lv_obj_set_style_pad_all(h, 14, 0);
    lv_obj_t *t = ui_label(h, P.f14, P.text2, "Last 2 minutes");
    lv_obj_set_pos(t, 0, -2);
    s_hist_cpu = ui_pill(h, C_CPU, "CPU");
    lv_obj_align(s_hist_cpu, LV_ALIGN_TOP_RIGHT, -76, -5);
    s_hist_gpu = ui_pill(h, C_GPU, "GPU");
    lv_obj_align(s_hist_gpu, LV_ALIGN_TOP_RIGHT, 0, -5);
    lv_color_t cols[] = {C_CPU, C_GPU};
    s_chart = sparkline_create(h, 572, 50, 2, cols);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 2);

    tile_create(&s_ram, tile, 16, &img_hw_ram_28, "Memory", C_RAM, lv_color_hex(0x3b82f6), false);
    tile_create(&s_vram, tile, 168, &img_hw_gpu_28, "VRAM", C_VRAM, lv_color_hex(0x14b8a6), false);
    tile_create(&s_net, tile, 320, &img_hw_net_28, "Network", C_NET_DOWN, C_NET_UP, true);
    tile_create(&s_disk, tile, 472, &img_hw_ssd_28, "Disk", C_DISK, lv_color_hex(0xe11d48), false);

    lv_obj_t *a = ui_card(tile, 624, 4, 384, 516);
    ui_icon(a, &img_fluent_agents_28);
    lv_obj_t *at = ui_title(a, "Agents");
    lv_obj_align(at, LV_ALIGN_TOP_LEFT, 36, 14 - lv_font_get_line_height(lv_obj_get_style_text_font(at, 0)) / 2);
    s_agents_count = ui_pill(a, P.accent, "");
    lv_obj_align(s_agents_count, LV_ALIGN_TOP_RIGHT, 0, -1);
    lv_obj_t *list = s_agents_list = ui_box(a);
    lv_obj_set_pos(list, 0, 44);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    for (int i = 0; i < AGENT_ROWS; i++) {
        agent_row_create(&s_rows[i], list, 352, false);
        lv_obj_add_flag(s_rows[i].root, LV_OBJ_FLAG_HIDDEN);
    }
    // One session: give it the whole card, laid out like the Agents page: header, a status line
    // (state dot, model, tokens, timer), context, any question / result, the conversation, and the
    // tool calls.
    s_single = ui_box(a);
    lv_obj_set_pos(s_single, 0, 40);
    lv_obj_set_size(s_single, 352, 444);
    lv_obj_set_flex_flow(s_single, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_single, 8, 0);
    lv_obj_t *hd = ui_box(s_single);
    lv_obj_set_size(hd, 352, 40);
    s_single_logo = ui_icon(hd, agent_logo("claude", true));
    lv_obj_align(s_single_logo, LV_ALIGN_LEFT_MID, 0, 0);
    s_single_project = ui_label(hd, P.f_head_s, P.text, "");
    lv_obj_set_width(s_single_project, 352 - 46);
    ui_one_line(s_single_project);
    lv_obj_align(s_single_project, LV_ALIGN_LEFT_MID, 46, 0);
    status_create(&s_single_status, s_single, 352);
    ctx_create(&s_single_ctx, s_single, 352);
    lv_obj_set_parent(s_single_status.callout, s_single);  // below the context line
    tools_create(&s_single_tools, s_single, 352, 150, true);  // one timeline: messages and calls
    lv_obj_set_flex_grow(s_single_tools.list.obj, 1);
    lv_obj_add_flag(s_single, LV_OBJ_FLAG_HIDDEN);
    s_agents_more = ui_label(a, P.f14, P.text3, "");
    lv_obj_align(s_agents_more, LV_ALIGN_BOTTOM_RIGHT, 0, 6);

    s_agents_empty = ui_box(a);
    lv_obj_set_flex_flow(s_agents_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_agents_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_agents_empty, 10, 0);
    lv_obj_center(s_agents_empty);
    ui_icon(s_agents_empty, &img_fluent_bot_sparkle_96);
    ui_label(s_agents_empty, P.f20, P.text, "No agents running");
    lv_obj_t *hint = ui_label(s_agents_empty, P.f14, P.text3, "Claude Code, Codex and pi sessions\nshow up here as they work.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void update_stats(const stats_t *s, const history_t *h)
{
    char c0[16], c1[16], c2[16], name[80];
    fmt_temp(c0, sizeof c0, s->cpu_temp);
    if (s->cpu_mhz) snprintf(c1, sizeof c1, "%.1f GHz", s->cpu_mhz / 1000.0);
    else snprintf(c1, sizeof c1, "–");
    if (!isnan(s->cpu_power)) snprintf(c2, sizeof c2, "%.0f W", s->cpu_power);
    else snprintf(c2, sizeof c2, "%d cores", s->ncores);
    // "AMD Ryzen 9 9950X3D 16-Core Processor" -> "Ryzen 9 9950X3D"
    snprintf(name, sizeof name, "%s", s->cpu_name);
    char *p = strstr(name, " Processor");
    if (p) *p = 0;
    if ((p = strstr(name, "-Core"))) {  // drop the " 16-Core" token
        char *start = p;
        while (start > name && start[-1] != ' ') start--;
        if (start > name) start--;
        memmove(start, p + 5, strlen(p + 5) + 1);
    }
    gauge_set(&s_cpu, s->cpu_load, strncmp(name, "AMD ", 4) ? name : name + 4, c0, c1, c2);
    lv_obj_set_style_text_color(s_cpu.value, heat_color(s->cpu_load), 0);

    if (s->ngpus) {
        const typeof(s->gpus[0]) *g = &s->gpus[0];
        fmt_temp(c0, sizeof c0, g->temp);
        if (g->mhz) snprintf(c1, sizeof c1, "%d MHz", g->mhz);
        else snprintf(c1, sizeof c1, "–");
        if (!isnan(g->power)) snprintf(c2, sizeof c2, "%.0f W", g->power);
        else snprintf(c2, sizeof c2, "–");
        gauge_set(&s_gpu, g->load, g->name, c0, c1, c2);
        lv_obj_set_style_text_color(s_gpu.value, heat_color(g->load), 0);
    } else {
        gauge_set(&s_gpu, NAN, "No GPU data", "–", "–", "–");
    }

    const float *series[2] = {h->cpu, h->gpu};
    float peak = 0;
    for (int i = 0; i < h->len; i++) peak = fmaxf(peak, fmaxf(h->cpu[i], h->gpu[i]));
    sparkline_set(s_chart, series, h->len, fminf(100, fmaxf(20, peak * 1.3f)));

    char a[16], b[16];
    fmt_bytes(a, sizeof a, s->mem_used);
    lv_label_set_text(s_ram.value, a);
    fmt_bytes(b, sizeof b, s->mem_total);
    lv_label_set_text_fmt(s_ram.sub, "of %s", b);
    lv_bar_set_value(s_ram.bar, s->mem_total ? (int)(s->mem_used * 1000 / s->mem_total) : 0, LV_ANIM_ON);

    if (s->ngpus && s->gpus[0].vram_total) {
        fmt_bytes(a, sizeof a, s->gpus[0].vram_used);
        lv_label_set_text(s_vram.value, a);
        fmt_bytes(b, sizeof b, s->gpus[0].vram_total);
        lv_label_set_text_fmt(s_vram.sub, "of %s", b);
        lv_bar_set_value(s_vram.bar, (int)(s->gpus[0].vram_used * 1000 / s->gpus[0].vram_total), LV_ANIM_ON);
    } else {
        lv_label_set_text(s_vram.value, "–");
        lv_label_set_text(s_vram.sub, "");
        lv_bar_set_value(s_vram.bar, 0, LV_ANIM_OFF);
    }

    fmt_rate(a, sizeof a, s->net_down);
    lv_label_set_text(s_net.value, a);
    fmt_rate(b, sizeof b, s->net_up);
    lv_label_set_text(s_net.value2, b);

    if (s->ndisks && s->disks[0].total) {
        int pct = (int)(s->disks[0].used * 100 / s->disks[0].total);
        lv_label_set_text_fmt(s_disk.value, "%d%%", pct);
        fmt_bytes(a, sizeof a, s->disks[0].total - s->disks[0].used);
        lv_label_set_text_fmt(s_disk.sub, "%s free", a);
        lv_bar_set_value(s_disk.bar, pct * 10, LV_ANIM_ON);
    }
}

static void single_set(const agent_t *a, time_t now)
{
    if (strcmp(s_single_id, a->id)) lv_image_set_src(s_single_logo, agent_logo(a->agent, true));
    snprintf(s_single_id, sizeof s_single_id, "%s", a->id);
    lv_label_set_text(s_single_project, a->project[0] ? a->project : a->agent);
    status_set(&s_single_status, a, now);
    ctx_set(&s_single_ctx, a);
    tools_set(&s_single_tools, a);
}

static void update_agents(const agent_t *a, int n, time_t now, bool changed)
{
    if (!changed) {  // between data updates only the elapsed timer moves
        if (n == 1) status_time(&s_single_status, &a[0], now);
        else
            for (int i = 0; i < n && i < AGENT_ROWS; i++) agent_row_set(&s_rows[i], &a[i], now);
        return;
    }
    int waiting = 0, working = 0;
    for (int i = 0; i < n; i++) {
        waiting += a[i].state == AG_PERMISSION || a[i].state == AG_QUESTION;
        working += a[i].state == AG_WORKING;
    }
    char txt[48];
    if (waiting) {
        snprintf(txt, sizeof txt, "%d waiting for you", waiting);
        ui_pill_set(s_agents_count, C_PERMISSION, txt);
    } else if (working) {
        snprintf(txt, sizeof txt, "%d working", working);
        ui_pill_set(s_agents_count, C_WORKING, txt);
    } else {
        snprintf(txt, sizeof txt, "%d session%s", n, n == 1 ? "" : "s");
        ui_pill_set(s_agents_count, C_IDLE, txt);
    }
    if (n == 1) {
        lv_obj_add_flag(s_agents_count, LV_OBJ_FLAG_HIDDEN);  // the session's status line says it
        lv_obj_add_flag(s_agents_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_single, LV_OBJ_FLAG_HIDDEN);
        single_set(&a[0], now);
    } else {
        lv_obj_remove_flag(s_agents_count, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_agents_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_single, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < AGENT_ROWS; i++) {
            if (i < n) agent_row_set(&s_rows[i], &a[i], now);
            else lv_obj_add_flag(s_rows[i].root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (n > AGENT_ROWS) lv_label_set_text_fmt(s_agents_more, "+%d more on the Agents page", n - AGENT_ROWS);
    else lv_label_set_text(s_agents_more, "");
    if (n) lv_obj_add_flag(s_agents_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_agents_empty, LV_OBJ_FLAG_HIDDEN);
}

void page_overview_update(const ui_data_t *d)
{
    if (d->stats_changed) update_stats(d->stats, d->hist);
    // Agents also refresh every tick so the elapsed timers keep running.
    update_agents(d->agents, d->nagents, d->now, d->agents_changed);
}
