// Agent session views shared by the Agents page and the Overview:
//  - chat:   the conversation as a clean transcript: the person's messages marked by an accent rule,
//            the agent's replies as plain text. Scrolls; follows the newest unless scrolled up.
//  - tools:  one row per tool call (tool chip + description), the newest highlighted, with two lines
//            of what it changed or printed underneath; tap a row for all of it in a popup.
//  - status: state, model and tokens, timer, and the question / permission prompt / error.
//  - ctx:    context usage as one thin bar with the count at its end.
#include <stdio.h>
#include <string.h>
#include "hl.h"
#include "md.h"
#include "ui_internal.h"

static void hide(lv_obj_t *o, bool h)
{
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = ui_label(parent, P.f14, color, text);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

/// Splits "Tool · description" into its parts; false when there is no separator.
static bool split_tool(const char *line, char *tool, size_t tn, const char **rest)
{
    const char *sep = strstr(line, " · ");
    if (!sep || sep == line || sep - line >= (int)tn) return false;
    memcpy(tool, line, sep - line);
    tool[sep - line] = 0;
    *rest = sep + strlen(" · ");
    return true;
}

// ---------------------------------------------------------------- scrolling list (shared)

static void list_scrolled(lv_event_t *e)
{
    scroll_list_t *l = lv_event_get_user_data(e);
    l->follow = lv_obj_get_scroll_bottom(l->obj) <= 8;
    if (l->follow) hide(l->more, true);
}

static void list_jump(lv_event_t *e)
{
    scroll_list_t *l = lv_event_get_user_data(e);
    l->follow = true;
    hide(l->more, true);
    lv_obj_scroll_to_y(l->obj, LV_COORD_MAX, LV_ANIM_ON);
}

static void list_init(scroll_list_t *l, lv_obj_t *parent, int w, int gap)
{
    l->obj = ui_box(parent);
    lv_obj_set_width(l->obj, w);
    // Clickable too: a box isn't by default, and without it drags never reach the list to scroll.
    lv_obj_add_flag(l->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(l->obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l->obj, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(l->obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l->obj, gap, 0);
    lv_obj_add_event_cb(l->obj, list_scrolled, LV_EVENT_SCROLL_END, l);
    l->follow = true;
    // "New" pill, floating over the bottom of the list.
    l->more = ui_box(parent);
    lv_obj_add_flag(l->more, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(l->more, P.accent, 0);
    lv_obj_set_style_bg_opa(l->more, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l->more, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(l->more, 14, 0);
    lv_obj_set_style_pad_ver(l->more, 6, 0);
    lv_obj_set_flex_flow(l->more, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(l->more, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(l->more, 6, 0);
    lv_color_t fg = P.light ? lv_color_white() : lv_color_hex(0x111111);
    ui_glyph(l->more, &icons_18, fg, ICON_DOWN);
    ui_label(l->more, P.f14, fg, "New");
    lv_obj_add_event_cb(l->more, list_jump, LV_EVENT_CLICKED, l);
    hide(l->more, true);
}

/// After the items changed: follow the newest, or offer the "new" pill if scrolled up.
static void list_after(scroll_list_t *l, int count, const char *newest)
{
    bool grew = count != l->count || strncmp(l->newest, newest, sizeof l->newest - 1);
    l->count = count;
    snprintf(l->newest, sizeof l->newest, "%s", newest);
    lv_obj_update_layout(l->obj);
    lv_obj_align_to(l->more, l->obj, LV_ALIGN_BOTTOM_MID, 0, -6);
    if (l->follow) lv_obj_scroll_to_y(l->obj, LV_COORD_MAX, grew ? LV_ANIM_ON : LV_ANIM_OFF);
    else if (grew) hide(l->more, false);
}

// ---------------------------------------------------------------- chat

static void text_popup(const char *chip_text, const char *title, const char *body, bool mono, const char *session,
                       const char *tool);

/// One message, on its own and at full width.
static void chat_popup(lv_event_t *e)
{
    chat_view_t *v = lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_current_target(e);
    int i = lv_obj_get_index(b);
    if (i < 0 || i >= CHAT_MAX) return;
    bool mine = lv_obj_get_style_border_width(b, 0) > 0;
    const char *mid = v->a && v->idx[i] >= 0 ? v->a->lines[v->idx[i]].i : "";
    text_popup("", mine ? "You" : "Agent", lv_label_get_text(v->text[i]), false, mid[0] ? v->a->id : NULL, mid);
}

void chat_create(chat_view_t *v, lv_obj_t *parent, int w, int h, bool compact)
{
    memset(v, 0, sizeof *v);
    v->w = w;
    v->compact = compact;
    list_init(&v->list, parent, w, compact ? 12 : 16);
    lv_obj_set_height(v->list.obj, h);
    for (int i = 0; i < CHAT_MAX; i++) {
        // One block per message; the person's get an accent rule down the left edge.
        lv_obj_t *b = v->bubble[i] = ui_box(v->list.obj);
        lv_obj_set_size(b, w - 8, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(b, P.accent, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);  // opens the message on its own
        lv_obj_add_event_cb(b, chat_popup, LV_EVENT_CLICKED, v);
        v->text[i] = ui_label(b, compact ? P.fa_small : P.fa_body, P.text, "");
        lv_label_set_long_mode(v->text[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(v->text[i], LV_PCT(100));
        v->md[i] = ui_box(b);  // the agent's replies: rendered markdown
        hide(v->md[i], true);
        hide(b, true);
    }
}

void chat_set(chat_view_t *v, const agent_t *a)
{
    // Messages only: the person's and the agent's prose. The pinned `ask` stands in for the
    // person's message when it scrolled out of the history window.
    const char *msg[CHAT_MAX];
    bool mine[CHAT_MAX];
    int at[CHAT_MAX];  // index into a->lines, -1 for the pinned ask
    v->a = a;
    int n = 0;
    bool any_user = false;
    for (int k = 0; k < a->nlines; k++)
        if (a->lines[k].kind == LINE_USER) any_user = true;
    if (!any_user && a->ask[0]) {
        msg[n] = a->ask;
        at[n] = -1;
        mine[n++] = true;
    }
    int first = 0, total = 0;
    for (int k = 0; k < a->nlines; k++)
        if (a->lines[k].kind != LINE_TOOL) total++;
    if (total + n > CHAT_MAX) first = total + n - CHAT_MAX;
    for (int k = 0, seen = 0; k < a->nlines && n < CHAT_MAX; k++) {
        if (a->lines[k].kind == LINE_TOOL) continue;
        if (seen++ < first) continue;
        msg[n] = a->lines[k].s;
        at[n] = k;
        mine[n++] = a->lines[k].kind == LINE_USER;
    }
    // The newest user message: the full text from `ask` beats the shortened history line.
    for (int i = n - 1; i >= 0; i--)
        if (mine[i]) {
            if (a->ask[0]) msg[i] = a->ask;
            break;
        }
    for (int i = 0; i < CHAT_MAX; i++) {
        lv_obj_t *b = v->bubble[i];
        if (i >= n) {
            hide(b, true);
            continue;
        }
        hide(b, false);
        v->idx[i] = at[i];
        // The person's messages stay plain; the agent's are markdown.
        set_text(v->text[i], msg[i]);
        hide(v->text[i], !mine[i]);
        hide(v->md[i], mine[i]);
        uint32_t h = md_hash(msg[i]);
        if (!mine[i] && v->hash[i] != h) {
            v->hash[i] = h;
            md_render(v->md[i], msg[i], v->w - 8, v->compact ? P.fa_small : P.fa_body, v->compact ? P.fa_small_b : P.fa_body_b, P.text);
        }
        lv_obj_set_style_border_width(b, mine[i] ? 3 : 0, 0);
        lv_obj_set_style_pad_left(b, mine[i] ? 12 : 0, 0);
        lv_obj_set_style_text_color(v->text[i], mine[i] ? P.text2 : P.text, 0);
    }
    list_after(&v->list, n, n ? msg[n - 1] : "");
}

// ---------------------------------------------------------------- tools

static void popup_close(lv_event_t *e) { ui_modal_close(lv_event_get_user_data(e)); }

/// How to highlight a tool call's detail: edits as diffs, written files as code, the rest as output.
static hl_mode_t tool_mode(const char *tool, const char *file, const hl_lang_t **lang)
{
    *lang = hl_find(file);
    if (!strcmp(tool, "Edit") || !strcmp(tool, "MultiEdit") || !strcmp(tool, "edit")) return HL_DIFF;
    if (!strcmp(tool, "Write") || !strcmp(tool, "NotebookEdit") || !strcmp(tool, "write")) return HL_CODE;
    return HL_OUTPUT;
}

// The open popup's request for the full text from the PC, and what to fill in when it arrives.
static lv_obj_t *s_pop_body, *s_pop_status;
static bool s_pop_md;  // the body is markdown (messages), else verbatim (tool output)
static const hl_lang_t *s_pop_lang;  // tool output: how to highlight it
static hl_mode_t s_pop_mode;
static lv_timer_t *s_pop_timer;
static int s_pop_req;

static void pop_poll(lv_timer_t *t)
{
    const char *full = hub_full_result(s_pop_req);
    if (!full && lv_tick_elaps((uint32_t)(uintptr_t)lv_timer_get_user_data(t)) < 8000) return;
    if (full && full[0]) {
        if (s_pop_md) {
            md_render(s_pop_body, full, 804, P.fa_body, P.fa_body_b, P.text);
        } else {
            lv_obj_clean(s_pop_body);
            hl_render(s_pop_body, full, 804, s_pop_lang, s_pop_mode, 0);
        }
    }
    lv_label_set_text(s_pop_status, full ? "" : "Couldn't load the full text");
    lv_timer_delete(t);
    s_pop_timer = NULL;
}

static void pop_deleted(lv_event_t *e)
{
    if (s_pop_timer) lv_timer_delete(s_pop_timer);
    s_pop_timer = NULL;
    s_pop_body = s_pop_status = NULL;
}

/// Full text in a popup: a chip (tool name or caption), a title, and the body in a scrolling box.
/// With a session, the complete text is fetched from the PC (`tool`: a call id, "" for the result)
/// and replaces the shortened `body` when it arrives.
static void text_popup(const char *chip_text, const char *title, const char *body, bool mono, const char *session,
                       const char *tool)
{
    lv_obj_t *m = ui_modal(880, 500);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m, 14, 0);
    lv_obj_t *hd = ui_box(m);
    lv_obj_set_size(hd, 832, 40);
    lv_obj_t *chip = ui_label(hd, P.fmono, P.accent, chip_text);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_update_layout(chip);
    int tx = chip_text[0] ? lv_obj_get_width(chip) + 16 : 0;
    lv_obj_t *t = ui_label(hd, P.f20, P.text, title);
    lv_obj_set_width(t, 832 - tx - 130);
    ui_one_line(t);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, tx, 0);
    lv_obj_t *st = ui_label(hd, P.f14, P.text3, "");
    lv_obj_t *x = ui_button(hd, ICON_CLOSE, "Close", false);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(x, popup_close, LV_EVENT_CLICKED, m);
    lv_obj_align_to(st, x, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_t *box = ui_box(m);
    lv_obj_set_width(box, 832);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_bg_color(box, P.card2, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, P.editorial ? 0 : 10, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    // Clickable too, or drags never reach it and it won't scroll.
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *d;
    if (mono) {
        d = ui_box(box);
        lv_obj_set_width(d, 804);
        hl_render(d, body, 804, s_pop_lang, s_pop_mode, 0);
    } else {
        d = ui_box(box);
        md_render(d, body, 804, P.fa_body, P.fa_body_b, P.text);
    }

    int req = session ? hub_request_full(session, tool) : 0;
    if (req) {
        if (s_pop_timer) lv_timer_delete(s_pop_timer);
        s_pop_body = d;
        s_pop_md = !mono;
        s_pop_status = st;
        s_pop_req = req;
        lv_label_set_text(st, "Loading the full text…");
        s_pop_timer = lv_timer_create(pop_poll, 100, (void *)(uintptr_t)lv_tick_get());
        lv_obj_add_event_cb(m, pop_deleted, LV_EVENT_DELETE, NULL);
    }
}

/// The whole edit / output of one tool call.
static void tool_popup(lv_event_t *e)
{
    tools_view_t *v = lv_event_get_user_data(e);
    int i = lv_obj_get_index(lv_event_get_current_target(e));
    if (!v->a || i < 0 || i >= MAX_LINES) return;
    if (v->a->lines[v->idx[i]].kind != LINE_TOOL) {
        const char *mid = v->a->lines[v->idx[i]].i;
        text_popup("", v->a->lines[v->idx[i]].kind == LINE_USER ? "You" : "Agent", v->a->lines[v->idx[i]].s, false,
                   mid[0] ? v->a->id : NULL, mid);
        return;
    }
    const char *line = v->a->lines[v->idx[i]].s, *rest = "";
    char tool[24];
    if (!split_tool(line, tool, sizeof tool, &rest)) snprintf(tool, sizeof tool, "%s", line);
    const char *id = v->a->lines[v->idx[i]].i;
    const char *d = v->a->lines[v->idx[i]].d;
    s_pop_mode = tool_mode(tool, v->a->lines[v->idx[i]].f, &s_pop_lang);
    text_popup(tool, rest, d[0] ? d : rest, true, id[0] ? v->a->id : NULL, id);
}

/// The whole result / question / permission prompt.
static void callout_popup(lv_event_t *e)
{
    status_view_t *v = lv_event_get_user_data(e);
    text_popup("", lv_label_get_text(v->callout_title), v->callout_full, false, v->result ? v->sid : NULL, "");
}

void tools_create(tools_view_t *v, lv_obj_t *parent, int w, int h, bool compact)
{
    memset(v, 0, sizeof *v);
    v->w = w;
    v->compact = compact;
    list_init(&v->list, parent, w, 6);
    lv_obj_set_height(v->list.obj, h);
    // Each call is one box: the name inline before the description, the detail below at full
    // width. Compact (the home page) also interleaves the messages: one timeline.
    int tw = w - 6 - 16;
    for (int i = 0; i < MAX_LINES; i++) {
        lv_obj_t *r = v->row[i] = ui_box(v->list.obj);
        lv_obj_set_size(r, w - 6, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 4, 0);
        lv_obj_set_style_radius(r, P.editorial ? 0 : 8, 0);
        lv_obj_set_style_pad_ver(r, 6, 0);
        lv_obj_set_style_pad_hor(r, 8, 0);
        lv_obj_set_style_bg_color(r, P.accent, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);  // every call opens in full
        lv_obj_add_event_cb(r, tool_popup, LV_EVENT_CLICKED, v);
        lv_obj_t *ln = ui_box(r);
        lv_obj_set_size(ln, w - 6 - 16, lv_font_get_line_height(P.fa_small) + 2);
        lv_obj_set_flex_flow(ln, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ln, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ln, 8, 0);
        v->ln[i] = ln;
        if (compact) {  // timeline rows can also be messages
            lv_obj_t *m = v->msg[i] = ui_label(r, P.fa_small, P.text, "");
            lv_obj_set_width(m, LV_PCT(100));
            lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
            lv_obj_move_to_index(m, 0);
            hide(m, true);
            v->md[i] = ui_box(r);
            lv_obj_move_to_index(v->md[i], 1);
            hide(v->md[i], true);
        }
        v->chip[i] = ui_label(ln, P.fmono, P.accent, "");
        ui_one_line(v->chip[i]);
        v->text[i] = ui_label(ln, P.fa_small, P.text2, "");
        lv_obj_set_flex_grow(v->text[i], 1);
        ui_one_line(v->text[i]);
        // The edit, the file written, or the command's output: two lines here, the rest on tap.
        lv_obj_t *d = v->detail[i] = ui_box(r);  // filled by hl_render when it changes
        lv_obj_set_width(d, tw);
        hide(d, true);
        hide(r, true);
    }
}

void tools_set(tools_view_t *v, const agent_t *a)
{
    v->a = a;
    int idx[MAX_LINES], n = 0;
    // Compact (home): a timeline of everything, messages and tool calls in order.
    for (int k = 0; k < a->nlines; k++)
        if (v->compact || a->lines[k].kind == LINE_TOOL) idx[n++] = k;
    for (int i = 0; i < MAX_LINES; i++) {
        lv_obj_t *r = v->row[i];
        if (i >= n) {
            hide(r, true);
            continue;
        }
        hide(r, false);
        v->idx[i] = idx[i];
        if (v->compact) {
            bool tool = a->lines[idx[i]].kind == LINE_TOOL, mine = a->lines[idx[i]].kind == LINE_USER;
            hide(v->ln[i], !tool);
            hide(v->msg[i], tool || !mine);
            hide(v->md[i], tool || mine);
            if (!tool) {
                hide(v->detail[i], true);
                set_text(v->msg[i], a->lines[idx[i]].s);
                uint32_t h = md_hash(a->lines[idx[i]].s);
                if (!mine && v->hash[i] != h) {
                    v->hash[i] = h;
                    md_render(v->md[i], a->lines[idx[i]].s, v->w - 6 - 16, P.fa_small, P.fa_small_b, P.text);
                }
                lv_obj_set_style_text_color(v->msg[i], mine ? P.text2 : P.text, 0);
                // A message: plain text, the person's with an accent rule (like the conversation).
                lv_obj_set_style_bg_opa(r, 0, 0);
                lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_color(r, P.accent, 0);
                lv_obj_set_style_border_width(r, mine ? 3 : 0, 0);
                lv_obj_set_style_pad_left(r, mine ? 10 : 0, 0);
                continue;
            }
            lv_obj_set_style_border_width(r, 0, 0);
            lv_obj_set_style_pad_left(r, 8, 0);
        }
        const char *line = a->lines[idx[i]].s, *rest = "";
        char tool[24];
        if (!split_tool(line, tool, sizeof tool, &rest)) snprintf(tool, sizeof tool, "%s", line);
        set_text(v->chip[i], tool);
        set_text(v->text[i], rest);
        const char *det = a->lines[idx[i]].d;
        hide(v->detail[i], !det[0]);
        if (det[0]) {
            // Two lines at most, highlighted; the row opens the rest.
            uint32_t h = md_hash(det) ^ (md_hash(a->lines[idx[i]].f) * 31) ^ md_hash(tool);
            if (v->dhash[i] != h) {
                v->dhash[i] = h;
                const hl_lang_t *lang;
                hl_mode_t mode = tool_mode(tool, a->lines[idx[i]].f, &lang);
                lv_obj_clean(v->detail[i]);
                hl_render(v->detail[i], det, v->w - 6 - 16, lang, mode, 2);
            }
        }
        bool newest = i == n - 1;
        bool live = newest && a->state == AG_WORKING;
        lv_obj_set_style_bg_color(r, live ? lv_color_mix(P.accent, P.card2, P.light ? 50 : 70) : P.card2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(v->text[i], newest ? P.text : P.text2, 0);
        lv_obj_set_style_text_color(v->chip[i], newest ? P.accent : P.text3, 0);
    }
    list_after(&v->list, n, n ? a->lines[idx[n - 1]].s : "");
}

// ---------------------------------------------------------------- status header + callout

static void pulse_cb(void *o, int32_t val) { lv_obj_set_style_opa(o, val, 0); }

void status_create(status_view_t *v, lv_obj_t *parent, int w)
{
    memset(v, 0, sizeof *v);
    v->root = ui_box(parent);
    lv_obj_set_width(v->root, w);
    lv_obj_set_height(v->root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->root, 8, 0);

    // One line: the state as a coloured dot (pulsing while there's something going on), the model
    // and output tokens, the turn's timer on the right.
    lv_obj_t *head = ui_box(v->root);
    lv_obj_set_size(head, w, 30);
    v->dot = ui_box(head);
    lv_obj_set_size(v->dot, 14, 14);
    lv_obj_set_style_radius(v->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(v->dot, LV_OPA_COVER, 0);
    lv_obj_align(v->dot, LV_ALIGN_LEFT_MID, 0, 0);
    v->state = NULL;
    v->meta = ui_label(head, P.f18, P.text2, "");
    lv_obj_set_width(v->meta, w - 24 - 100);
    ui_one_line(v->meta);
    lv_obj_align(v->meta, LV_ALIGN_LEFT_MID, 24, 0);
    v->timer = ui_label(head, P.f_num, P.text, "");
    lv_obj_set_width(v->timer, 84);
    lv_obj_set_style_text_align(v->timer, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v->timer, LV_ALIGN_RIGHT_MID, 0, 0);

    v->callout = ui_box(v->root);
    lv_obj_set_width(v->callout, w);
    lv_obj_set_height(v->callout, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(v->callout, P.editorial ? 0 : 12, 0);
    lv_obj_set_style_bg_opa(v->callout, P.light ? 34 : 40, 0);
    lv_obj_set_style_pad_all(v->callout, 12, 0);
    lv_obj_set_flex_flow(v->callout, LV_FLEX_FLOW_COLUMN);
    // Three lines here; tap for all of it.
    lv_obj_add_flag(v->callout, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v->callout, callout_popup, LV_EVENT_CLICKED, v);
    lv_obj_set_style_pad_row(v->callout, 6, 0);
    v->callout_title = caption(v->callout, "", P.text);
    v->callout_text = ui_label(v->callout, P.fa_body, P.text, "");
    lv_obj_set_width(v->callout_text, w - 24);
    lv_label_set_long_mode(v->callout_text, LV_LABEL_LONG_DOT);
    lv_obj_set_height(v->callout_text, 3 * lv_font_get_line_height(P.fa_body));
    hide(v->callout, true);
}

void status_attach_ctx(status_view_t *v, ctx_view_t *c)
{
    lv_obj_t *head = lv_obj_get_parent(v->dot);
    lv_obj_set_parent(c->root, head);
    lv_obj_align(c->root, LV_ALIGN_RIGHT_MID, -84 - 16, 0);
    // Style widths: this runs before the first layout pass.
    lv_obj_set_width(v->meta, lv_obj_get_style_width(head, 0) - 24 - 84 - 16 - lv_obj_get_style_width(c->root, 0) - 16);
    // Model and tokens sit right against the context bar.
    lv_obj_set_style_text_align(v->meta, LV_TEXT_ALIGN_RIGHT, 0);
}

void status_time(status_view_t *v, const agent_t *a, time_t now)
{
    char t[24] = "";
    if (a->state == AG_WORKING && a->turn_start && now) fmt_duration(t, sizeof t, now - a->turn_start);
    set_text(v->timer, t);
    lv_obj_align(v->timer, LV_ALIGN_RIGHT_MID, 0, 0);
}

void status_set(status_view_t *v, const agent_t *a, time_t now)
{
    lv_color_t sc = state_color(a->state);
    bool live = a->state == AG_WORKING || a->state == AG_PERMISSION || a->state == AG_QUESTION;
    if (!v->inited || a->state != v->last_state) {
        v->inited = true;
        v->last_state = a->state;
        lv_anim_delete(v->dot, pulse_cb);
        lv_obj_set_style_opa(v->dot, LV_OPA_COVER, 0);
        if (live) {
            lv_anim_t an;
            lv_anim_init(&an);
            lv_anim_set_var(&an, v->dot);
            lv_anim_set_exec_cb(&an, pulse_cb);
            lv_anim_set_values(&an, 255, 60);
            lv_anim_set_duration(&an, 900);
            lv_anim_set_playback_duration(&an, 900);
            lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&an);
        }
    }
    lv_obj_set_style_bg_color(v->dot, sc, 0);
    lv_obj_set_style_shadow_color(v->dot, sc, 0);
    lv_obj_set_style_shadow_width(v->dot, live ? 12 : 0, 0);
    char o[12], meta[96];
    fmt_tokens(o, sizeof o, a->out);
    snprintf(meta, sizeof meta, "%s%s%s out", a->model[0] ? a->model : a->agent, "  ·  ", o);
    set_text(v->meta, meta);
    status_time(v, a, now);

    bool needs = a->state == AG_PERMISSION || a->state == AG_QUESTION;
    // A finished turn's reply is the last message in the conversation, where it scrolls with the
    // rest; only an error (which isn't part of the conversation) gets the box.
    bool result = !needs && a->state == AG_ERROR && a->summary[0];
    hide(v->callout, !needs && !result);
    if (needs || result) {
        lv_color_t c = needs ? sc : a->state == AG_ERROR ? C_ERROR : C_DONE;
        lv_obj_set_style_bg_color(v->callout, c, 0);
        set_text(v->callout_title, a->state == AG_PERMISSION ? "ALLOW THIS?"
                                   : a->state == AG_QUESTION ? "ANSWER IN THE TERMINAL"
                                   : a->state == AG_ERROR    ? "ERROR"
                                                             : "RESULT");
        const char *body = needs ? (a->prompt[0] ? a->prompt : "Switch to the terminal to respond.") : a->summary;
        snprintf(v->callout_full, sizeof v->callout_full, "%s", body);
        snprintf(v->sid, sizeof v->sid, "%s", a->id);
        v->result = result;
        set_text(v->callout_text, body);
    }
}

// ---------------------------------------------------------------- context footer

void ctx_create(ctx_view_t *v, lv_obj_t *parent, int w)
{
    v->root = ui_box(parent);
    lv_obj_set_size(v->root, w, 18);
    v->text = ui_label(v->root, P.f14, P.text3, "");
    lv_obj_align(v->text, LV_ALIGN_RIGHT_MID, 0, 0);
    v->bar = ui_bar(v->root, w - 104, 4, P.flat ? P.accent : lv_color_hex(0xa78bfa), P.flat ? P.accent : lv_color_hex(0xf472b6));
    lv_obj_align(v->bar, LV_ALIGN_LEFT_MID, 0, 0);
}

void ctx_set(ctx_view_t *v, const agent_t *a)
{
    char c1[12], c2[12], line[48];
    fmt_tokens(c1, sizeof c1, a->ctx);
    fmt_tokens(c2, sizeof c2, a->ctx_max);
    lv_bar_set_value(v->bar, a->ctx_max ? (int)(a->ctx * 1000 / a->ctx_max) : 0, LV_ANIM_OFF);
    if (a->ctx_max) snprintf(line, sizeof line, "%s / %s", c1, c2);
    else snprintf(line, sizeof line, "%s", c1);
    set_text(v->text, line);
    lv_obj_align(v->text, LV_ALIGN_RIGHT_MID, 0, 0);
}
