#pragma once
// Markdown for agent messages: the subset agents actually write (headings, **bold**, *italic*,
// `code`, fenced code blocks, lists, quotes, links, rules, tables), rendered once into LVGL objects.
#include "lvgl.h"

/// Clears `box` and fills it with `src` rendered at width `w`: paragraphs in `body` (bold in
/// `bold`) and `color`. `box` becomes a flex column sized to its content.
void md_render(lv_obj_t *box, const char *src, int w, const lv_font_t *body, const lv_font_t *bold, lv_color_t color);

/// A cheap fingerprint of a text, to skip re-rendering what is already on screen.
uint32_t md_hash(const char *s);
