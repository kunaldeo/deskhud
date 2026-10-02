// A small markdown renderer: CommonMark's everyday subset, line by line, no AST.
//   blocks:  # headings, ``` fenced code, - / * / + / 1. lists, > quotes, --- rules, | tables,
//            paragraphs (consecutive lines joined; a blank line ends one)
//   inline:  **bold** / __bold__, *italic* / _italic_, `code`, [text](url), \escapes
// Inline runs become spans of one spangroup per block; everything is laid out once, when the text
// changes. Unclosed markers simply style the rest of the block, which suits text cut short.
#include "md.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "hl.h"
#include "ui_internal.h"

typedef struct {
    const lv_font_t *body, *bold;
    lv_color_t color;
} md_ctx_t;

uint32_t md_hash(const char *s)
{
    uint32_t h = 2166136261u;  // FNV-1a
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// ---------------------------------------------------------------- inline

typedef struct {
    lv_obj_t *sg;
    char *buf;
    int len;
} runs_t;

static void flush(runs_t *r, const lv_font_t *font, lv_color_t color)
{
    if (!r->len) return;
    r->buf[r->len] = 0;
    lv_span_t *sp = lv_spangroup_add_span(r->sg);
    lv_span_set_text(sp, r->buf);  // copies
    lv_style_t *st = lv_span_get_style(sp);
    lv_style_set_text_font(st, font);
    lv_style_set_text_color(st, color);
    r->len = 0;
}

static bool is_word(char c) { return isalnum((unsigned char)c) || (c & 0x80); }

/// One block of inline text (`s`, `n` bytes) as a spangroup of styled runs.
static lv_obj_t *inline_block(lv_obj_t *parent, const char *s, int n, int w, const md_ctx_t *c, const lv_font_t *base)
{
    lv_obj_t *sg = lv_spangroup_create(parent);
    // Taps go to the message (it opens in a popup), not the text.
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sg, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, base, 0);
    lv_obj_set_style_text_line_space(sg, 3, 0);
    runs_t r = {.sg = sg, .buf = lv_malloc(n + 1)};
    if (!r.buf) return sg;
    bool bold = base == c->bold, ital = false, code = false;
    const lv_font_t *font = base;
    lv_color_t color = c->color;
#define STYLE()                                                                          \
    do {                                                                                 \
        font = code ? P.fmono : bold ? c->bold : c->body;                                \
        color = code ? P.accent : ital ? P.text2 : c->color;                             \
    } while (0)
    STYLE();
    for (int i = 0; i < n; i++) {
        char ch = s[i], nx = i + 1 < n ? s[i + 1] : 0, pv = i ? s[i - 1] : ' ';
        if (code) {
            if (ch == '`') {
                flush(&r, font, color);
                code = false;
                STYLE();
            } else {
                r.buf[r.len++] = ch;
            }
            continue;
        }
        if (ch == '\\' && nx && ispunct((unsigned char)nx)) {
            r.buf[r.len++] = nx;
            i++;
        } else if (ch == '`') {
            flush(&r, font, color);
            code = true;
            STYLE();
        } else if ((ch == '*' || ch == '_') && nx == ch && (ch == '*' || !is_word(pv) || !is_word(i + 2 < n ? s[i + 2] : ' '))) {
            flush(&r, font, color);
            bold = !bold;
            STYLE();
            i++;
        } else if (ch == '*' && (ital ? pv != ' ' : (nx && nx != ' '))) {
            flush(&r, font, color);
            ital = !ital;
            STYLE();
        } else if (ch == '_' && (ital ? !is_word(nx) : !is_word(pv) && nx && nx != ' ')) {
            flush(&r, font, color);
            ital = !ital;
            STYLE();
        } else if (ch == '[') {
            // [text](url): the text, in the accent colour; the url is dropped.
            const char *close = memchr(s + i, ']', n - i);
            int ci = close ? (int)(close - s) : -1;
            const char *end = ci > 0 && ci + 1 < n && s[ci + 1] == '(' ? memchr(s + ci, ')', n - ci) : NULL;
            if (end) {
                flush(&r, font, color);
                memcpy(r.buf, s + i + 1, ci - i - 1);
                r.len = ci - i - 1;
                flush(&r, font, P.accent);
                i = (int)(end - s);
            } else {
                r.buf[r.len++] = ch;
            }
        } else {
            r.buf[r.len++] = ch;
        }
    }
    flush(&r, font, color);
#undef STYLE
    lv_free(r.buf);
    lv_spangroup_refresh(sg);
    return sg;
}

// ---------------------------------------------------------------- blocks

static const char *skip_sp(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/// "- ", "* ", "+ " or "12. " / "12) ": the marker's length, 0 if not a list item.
static int list_marker(const char *p, const char *end, char *num, size_t nn)
{
    if (p + 1 < end && (*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
        snprintf(num, nn, "\xe2\x80\xa2");  // •
        return 2;
    }
    const char *q = p;
    while (q < end && isdigit((unsigned char)*q) && q - p < 3) q++;
    if (q > p && q + 1 < end && (*q == '.' || *q == ')') && q[1] == ' ') {
        snprintf(num, nn, "%.*s.", (int)(q - p), p);
        return (int)(q - p) + 2;
    }
    return 0;
}

static bool is_rule(const char *p, const char *end)
{
    char m = *p;
    if (m != '-' && m != '*' && m != '_') return false;
    int k = 0;
    for (; p < end; p++) {
        if (*p == m) k++;
        else if (*p != ' ') return false;
    }
    return k >= 3;
}

static lv_obj_t *box_column(lv_obj_t *parent, int w, int gap)
{
    lv_obj_t *b = ui_box(parent);
    lv_obj_set_width(b, w);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, gap, 0);
    return b;
}

void md_render(lv_obj_t *box, const char *src, int w, const lv_font_t *body, const lv_font_t *bold, lv_color_t color)
{
    md_ctx_t c = {.body = body, .bold = bold, .color = color};
    lv_obj_clean(box);
    lv_obj_set_width(box, w);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);

    size_t total = strlen(src);
    char *para = lv_malloc(total + 1);
    if (!para) return;
    int plen = 0;
#define FLUSH_PARA()                                                       \
    do {                                                                   \
        if (plen) inline_block(box, para, plen, w, &c, c.body);            \
        plen = 0;                                                          \
    } while (0)

    const char *p = src, *end = src + total;
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        if (!eol) eol = end;
        const char *t = skip_sp(p, eol);
        int indent = (int)(t - p);
        char num[8];
        int lm;
        if (t + 3 <= eol && !strncmp(t, "```", 3)) {
            // Fenced code: verbatim, monospace, on a tinted block. An unclosed fence runs to the end.
            FLUSH_PARA();
            // The fence's info string names the language: ```rust
            char tag[16] = "";
            const char *tg = skip_sp(t + 3, eol);
            int tl = 0;
            while (tg + tl < eol && tl < (int)sizeof tag - 1 && !isspace((unsigned char)tg[tl])) tl++;
            memcpy(tag, tg, tl);
            tag[tl] = 0;
            const char *q = eol < end ? eol + 1 : end, *code = q;
            const char *stop = end;
            while (q < end) {
                const char *e2 = memchr(q, '\n', end - q);
                if (!e2) e2 = end;
                const char *t2 = skip_sp(q, e2);
                if (t2 + 3 <= e2 && !strncmp(t2, "```", 3)) {
                    stop = q;
                    q = e2 < end ? e2 + 1 : end;
                    goto fenced;
                }
                q = e2 < end ? e2 + 1 : end;
            }
            stop = end;
        fenced:;
            int cl = (int)(stop - code);
            while (cl > 0 && (code[cl - 1] == '\n' || code[cl - 1] == ' ')) cl--;
            memcpy(para, code, cl);
            para[cl] = 0;
            lv_obj_t *cb = ui_box(box);
            lv_obj_set_width(cb, w);
            lv_obj_set_style_bg_color(cb, P.card2, 0);
            lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cb, P.editorial ? 0 : 6, 0);
            lv_obj_set_style_pad_all(cb, 8, 0);
            bool diff = !strcmp(tag, "diff") || !strcmp(tag, "patch");
            hl_render(cb, para, w - 16, diff ? NULL : hl_find(tag), diff ? HL_DIFF : HL_CODE, 0);
            p = q;
            continue;
        }
        if (t == eol) {  // blank line: end of paragraph
            FLUSH_PARA();
        } else if (*t == '#') {
            FLUSH_PARA();
            const char *h = t;
            while (h < eol && *h == '#') h++;
            h = skip_sp(h, eol);
            inline_block(box, h, (int)(eol - h), w, &c, c.bold);
        } else if (is_rule(t, eol)) {
            FLUSH_PARA();
            lv_obj_t *rule = ui_box(box);
            lv_obj_set_size(rule, w, 1);
            lv_obj_set_style_bg_color(rule, P.border, 0);
            lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        } else if (*t == '>') {
            FLUSH_PARA();
            const char *q = skip_sp(t + 1, eol);
            lv_obj_t *b = box_column(box, w, 0);
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(b, 3, 0);
            lv_obj_set_style_border_color(b, P.border, 0);
            lv_obj_set_style_pad_left(b, 10, 0);
            inline_block(b, q, (int)(eol - q), w - 13, &c, c.body);
        } else if ((lm = list_marker(t, eol, num, sizeof num))) {
            FLUSH_PARA();
            int in = 6 + (indent / 2) * 16, mw = num[0] == '\xe2' ? 16 : 28;
            lv_obj_t *row = ui_box(box);
            lv_obj_set_size(row, w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_left(row, in, 0);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_t *m = ui_label(row, c.body, P.text3, num);
            lv_obj_set_width(m, mw);
            const char *q = t + lm;
            inline_block(row, q, (int)(eol - q), w - in - mw - 6, &c, c.body);
        } else if (*t == '|') {
            // Table row: cells spaced out on one monospace line; the |---| divider is skipped.
            FLUSH_PARA();
            bool divider = true;
            for (const char *q = t; q < eol; q++)
                if (!strchr("|-: ", *q)) divider = false;
            if (!divider) {
                int o = 0;
                for (const char *q = t + 1; q < eol; q++) {
                    if (*q == '|') {
                        while (o && para[o - 1] == ' ') o--;
                        if (q + 1 < eol) o += snprintf(para + o, total + 1 - o, "   ");
                        q = skip_sp(q + 1, eol) - 1;
                    } else {
                        para[o++] = *q;
                    }
                }
                para[o] = 0;
                lv_obj_t *l = ui_label(box, P.fmono, c.color, para);
                lv_obj_set_width(l, w);
                lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            }
        } else {
            // Paragraph text: lines join with a space.
            if (plen) para[plen++] = ' ';
            memcpy(para + plen, t, eol - t);
            plen += (int)(eol - t);
        }
        p = eol < end ? eol + 1 : end;
    }
    FLUSH_PARA();
#undef FLUSH_PARA
    lv_free(para);
}
