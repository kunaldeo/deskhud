#pragma once
// Syntax highlighting for code on the display: a one-pass tokenizer (keywords, strings, comments,
// numbers, function calls) with a keyword list per language, plus diff and command-output modes.
#include "lvgl.h"

typedef struct hl_lang hl_lang_t;

/// A language by code-fence tag (`rust`, `py`, …) or file name (`main.rs`); NULL: plain text.
const hl_lang_t *hl_find(const char *name);

typedef enum {
    HL_CODE,    // source code
    HL_DIFF,    // "- old" / "+ new" lines (the rest highlighted as code)
    HL_OUTPUT,  // command output: "$ command" first, errors and warnings picked out
} hl_mode_t;

/// Monospace, highlighted `src` at width `w` in `parent`; `max_lines` > 0 cuts it with "…".
/// Very long text (over 4 KiB) comes back as a plain label: highlighting it would cost too much.
lv_obj_t *hl_render(lv_obj_t *parent, const char *src, int w, const hl_lang_t *lang, hl_mode_t mode, int max_lines);
