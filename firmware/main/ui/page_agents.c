// Agents page: sessions as horizontal tabs across the top, then the selected session across the
// full width: the conversation on the left; on the wider right, its status, the tool calls with
// their edits and output, and context.
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"

typedef struct {
    lv_obj_t *root, *logo, *name, *dot, *time;
    char id[96];
} tab_t;

static tab_t s_tabs[MAX_AGENTS];
static lv_obj_t *s_strip, *s_conv_card, *s_status_card, *s_empty, *s_title;
static chat_view_t s_chat;
static tools_view_t s_tools;
static status_view_t s_status;
static ctx_view_t s_ctx;
static char s_sel[96];
static const agent_t *s_last;  // the UI's session buffer: stable between agent updates
static int s_nlast;
static time_t s_now;

static void render(void);

static void tab_click(lv_event_t *e)
{
    tab_t *t = lv_event_get_user_data(e);
    snprintf(s_sel, sizeof s_sel, "%s", t->id);
    render();
}

void page_agents_select(const char *id) { snprintf(s_sel, sizeof s_sel, "%s", id); }
bool page_agents_showing(const char *id) { return !strcmp(s_sel, id); }

static void tab_create(tab_t *t, lv_obj_t *parent)
{
    t->root = ui_box(parent);
    lv_obj_set_height(t->root, 52);
    lv_obj_set_width(t->root, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(t->root, 14, 0);
    lv_obj_set_style_radius(t->root, P.editorial ? 0 : 12, 0);
    lv_obj_set_style_bg_color(t->root, P.card, 0);
    lv_obj_set_flex_flow(t->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t->root, 10, 0);
    lv_obj_add_flag(t->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->root, tab_click, LV_EVENT_CLICKED, t);
    t->logo = ui_icon(t->root, agent_logo("claude", false));
    t->name = ui_label(t->root, P.f18, P.text2, "");
    ui_one_line(t->name);
    lv_obj_set_style_max_width(t->name, 260, 0);
    t->dot = ui_box(t->root);
    lv_obj_set_size(t->dot, 9, 9);
    lv_obj_set_style_radius(t->dot, 5, 0);
    lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
    t->time = ui_label(t->root, P.f16, P.text3, "");
    // Selected: the tab becomes a card (filled, outlined); no extra bar that reads as a gauge.
    lv_obj_set_style_border_color(t->root, P.border, 0);
    lv_obj_add_flag(t->root, LV_OBJ_FLAG_HIDDEN);
}

static void tab_set(tab_t *t, const agent_t *a, bool selected)
{
    lv_obj_remove_flag(t->root, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(t->id, a->id)) lv_image_set_src(t->logo, agent_logo(a->agent, false));
    snprintf(t->id, sizeof t->id, "%s", a->id);
    lv_label_set_text(t->name, a->project[0] ? a->project : a->agent);
    lv_obj_set_style_bg_color(t->dot, state_color(a->state), 0);
    char d[24] = "";
    if (a->state == AG_WORKING && a->turn_start && s_now) fmt_duration(d, sizeof d, s_now - a->turn_start);
    else if (a->state == AG_PERMISSION) snprintf(d, sizeof d, "needs you");
    else if (a->state == AG_QUESTION) snprintf(d, sizeof d, "question");
    lv_label_set_text(t->time, d);
    lv_obj_set_style_bg_opa(t->root, selected ? P.card_opa : 0, 0);
    lv_obj_set_style_text_color(t->name, selected ? P.text : P.text2, 0);
    lv_obj_set_style_border_width(t->root, selected ? 1 : 0, 0);
}

void page_agents_create(lv_obj_t *tile)
{
    s_strip = ui_box(tile);
    lv_obj_set_pos(s_strip, 16, 4);
    lv_obj_set_size(s_strip, 992, 52);
    lv_obj_add_flag(s_strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_strip, 6, 0);
    for (int i = 0; i < MAX_AGENTS; i++) tab_create(&s_tabs[i], s_strip);

    // Left: the conversation.
    s_conv_card = ui_card(tile, 16, 64, 384, 456);
    lv_obj_set_style_pad_all(s_conv_card, 18, 0);
    s_title = ui_label(s_conv_card, P.f20, P.text, "");
    lv_obj_set_width(s_title, 348);
    ui_one_line(s_title);
    chat_create(&s_chat, s_conv_card, 348, 374, false);
    lv_obj_set_pos(s_chat.list.obj, 0, 46);

    // Right, the wider side: one header line (state dot, model and tokens, context bar, timer),
    // the question / permission / result, then the tool calls with their edits and output taking
    // all the height that's left.
    s_status_card = ui_card(tile, 408, 64, 600, 456);
    lv_obj_set_style_pad_all(s_status_card, 18, 0);
    lv_obj_set_flex_flow(s_status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_status_card, 8, 0);
    status_create(&s_status, s_status_card, 564);
    ctx_create(&s_ctx, s_status_card, 220);
    status_attach_ctx(&s_status, &s_ctx);  // one header line: dot, model, context, timer
    lv_obj_set_parent(s_status.callout, s_status_card);
    tools_create(&s_tools, s_status_card, 564, 200, false);
    lv_obj_set_flex_grow(s_tools.list.obj, 1);
    lv_obj_set_style_min_height(s_tools.list.obj, 140, 0);

    s_empty = ui_card(tile, 16, 64, 992, 456);
    lv_obj_set_flex_flow(s_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_empty, 12, 0);
    ui_icon(s_empty, &img_fluent_bot_sparkle_96);
    ui_label(s_empty, P.f_head, P.text, "All quiet");
    lv_obj_t *h = ui_label(s_empty, P.f16, P.text3,
                           "When Claude Code, Codex or pi work on the active PC,\nyou'll see what they're doing here, live.");
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

}

static const agent_t *selected(void)
{
    for (int i = 0; i < s_nlast; i++)
        if (!strcmp(s_last[i].id, s_sel)) return &s_last[i];
    return NULL;
}

static void render(void)
{
    const agent_t *a = NULL;
    for (int i = 0; i < s_nlast; i++)
        if (!strcmp(s_last[i].id, s_sel)) a = &s_last[i];
    if (!a && s_nlast) {
        a = &s_last[0];
        snprintf(s_sel, sizeof s_sel, "%s", a->id);
    }
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (i < s_nlast) tab_set(&s_tabs[i], &s_last[i], !strcmp(s_last[i].id, s_sel));
        else {
            lv_obj_add_flag(s_tabs[i].root, LV_OBJ_FLAG_HIDDEN);
            s_tabs[i].id[0] = 0;
        }
    }
    bool none = !a;
    if (none) {
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_conv_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_status_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_conv_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_status_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_title, a->title[0] ? a->title : "New session");
    status_set(&s_status, a, s_now);
    chat_set(&s_chat, a);
    tools_set(&s_tools, a);
    ctx_set(&s_ctx, a);
}

void page_agents_update(const ui_data_t *d)
{
    s_now = d->now;
    if (d->agents_changed) {
        s_nlast = d->nagents;
        s_last = d->agents;
        render();
        return;
    }
    // Between data updates only the clocks move.
    const agent_t *a = selected();
    if (a) status_time(&s_status, a, s_now);
    for (int i = 0; i < s_nlast; i++) {
        char t[24] = "";
        if (s_last[i].state == AG_WORKING && s_last[i].turn_start && s_now) fmt_duration(t, sizeof t, s_now - s_last[i].turn_start);
        else continue;
        if (strcmp(lv_label_get_text(s_tabs[i].time), t)) lv_label_set_text(s_tabs[i].time, t);
    }
}
