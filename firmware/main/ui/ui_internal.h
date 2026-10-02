#pragma once
#include <time.h>
#include "theme.h"

#define TOPBAR_H 64
#define PAGE_H (BOARD_H - TOPBAR_H)
enum { PAGE_OVERVIEW, PAGE_AGENTS, PAGE_SYSTEM, PAGE_SETTINGS, PAGE_COUNT };

// Data handed to pages each tick (copies owned by ui.c, valid during the call).
typedef struct {
    const stats_t *stats;
    const history_t *hist;
    const agent_t *agents;
    int nagents;
    bool stats_changed, agents_changed, pcs_changed;
    time_t now;  // 0 while the clock is not synced
} ui_data_t;

void page_overview_create(lv_obj_t *tile);
void page_overview_update(const ui_data_t *d);
void page_agents_create(lv_obj_t *tile);
void page_agents_update(const ui_data_t *d);
void page_agents_select(const char *id);
bool page_agents_showing(const char *id);  // the session selected on the Agents page
void page_system_create(lv_obj_t *tile);
void page_system_update(const ui_data_t *d);
void page_settings_create(lv_obj_t *tile);
void page_settings_update(const ui_data_t *d);  // called every tick while visible

void ui_show_page(int page, bool anim);
int ui_current_page(void);
void ui_request_rebuild(void);

// Shared widgets
typedef struct {
    lv_obj_t *root, *logo, *project, *pill, *title, *activity, *ctx_bar, *ctx_label, *dot;
    char id[96];
    ag_state_t state;
    bool compact;
} agent_row_t;
void agent_row_create(agent_row_t *r, lv_obj_t *parent, int w, bool compact);
void agent_row_set(agent_row_t *r, const agent_t *a, time_t now);

/// A vertically scrolling list that follows its newest item unless scrolled up.
typedef struct {
    lv_obj_t *obj, *more;
    bool follow;
    int count;
    char newest[64];
} scroll_list_t;

#define CHAT_MAX MAX_LINES
/// The conversation: the person's messages and the agent's replies as chat bubbles.
typedef struct {
    scroll_list_t list;
    lv_obj_t *bubble[CHAT_MAX], *text[CHAT_MAX], *md[CHAT_MAX];  // text: the person's; md: the agent's
    uint32_t hash[CHAT_MAX];  // what md[i] shows (re-rendered only when it changes)
    const agent_t *a;    // the session shown, for the popup
    int idx[CHAT_MAX];   // bubble -> index into a->lines (-1: the pinned ask)
    int w;
    bool compact;
} chat_view_t;
void chat_create(chat_view_t *v, lv_obj_t *parent, int w, int h, bool compact);
void chat_set(chat_view_t *v, const agent_t *a);

/// One row per tool call (tool chip + description), with two lines of its edit / output
/// underneath; tapping a row opens the whole thing.
typedef struct {
    scroll_list_t list;
    lv_obj_t *row[MAX_LINES], *chip[MAX_LINES], *text[MAX_LINES], *detail[MAX_LINES];
    int w;
    const agent_t *a;     // the session shown (the UI's buffer), for the full-output popup
    int idx[MAX_LINES];   // row -> index into a->lines
    bool compact;         // home page: a timeline of messages and calls, one box per call
    lv_obj_t *ln[MAX_LINES], *msg[MAX_LINES], *md[MAX_LINES];
    uint32_t hash[MAX_LINES];   // what md[i] shows
    uint32_t dhash[MAX_LINES];  // what detail[i] shows
} tools_view_t;
void tools_create(tools_view_t *v, lv_obj_t *parent, int w, int h, bool compact);
void tools_set(tools_view_t *v, const agent_t *a);

/// State, timer, and the question / permission prompt / result.
typedef struct {
    lv_obj_t *root, *dot, *state, *meta, *timer, *callout, *callout_title, *callout_text;
    char callout_full[608];  // untruncated, for the popup
    char sid[96];            // the session shown, to fetch its full result
    bool result;             // the callout is the result (not a question / permission prompt)
    ag_state_t last_state;
    bool inited;
} status_view_t;
void status_create(status_view_t *v, lv_obj_t *parent, int w);
void status_set(status_view_t *v, const agent_t *a, time_t now);
void status_time(status_view_t *v, const agent_t *a, time_t now);  // timer only, cheap

/// Context usage: a thin bar, the count at its end.
typedef struct {
    lv_obj_t *root, *bar, *text;
} ctx_view_t;
void ctx_create(ctx_view_t *v, lv_obj_t *parent, int w);
void ctx_set(ctx_view_t *v, const agent_t *a);
/// Move a context view into the status line (between the model and the timer).
void status_attach_ctx(status_view_t *v, ctx_view_t *c);

/// btop-style dot graph. Mirrored: series A above a centre line, B below. One sample per column.
lv_obj_t *dotgraph_create(lv_obj_t *parent, int w, int h, bool mirrored, lv_color_t a_lo, lv_color_t a_hi, lv_color_t b_lo,
                          lv_color_t b_hi);
void dotgraph_set(lv_obj_t *graph, const float *a, const float *b, int len, float max_a, float max_b);

lv_obj_t *sparkline_create(lv_obj_t *parent, int w, int h, int nseries, const lv_color_t *colors);
/// values: nseries arrays of `len` floats; `max` <= 0 autoscales.
void sparkline_set(lv_obj_t *chart, const float *const *values, int len, float max);

// Overlays (top layer)
lv_obj_t *ui_modal(int w, int h);  // dimmed backdrop + centered card; delete the backdrop to close
void popup_sysinfo_open(void);     // the PC's fastfetch, with its icons and logo
void popup_procs_open(void);       // btop-style process table
void ui_modal_close(lv_obj_t *card);
void ui_wifi_open(void);
void ui_pc_switcher_open(lv_obj_t *anchor);
void ui_confirm(const char *title, const char *body, const char *ok_text, void (*on_ok)(void));
void ui_rename_open(void);
