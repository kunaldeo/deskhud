// Syntax highlighting: one pass over the text, line by line, emitting a span per colour run.
// Not a parser: it knows comments, strings, numbers, keywords and `name(` calls, which is what
// makes code readable at a glance. Block comments carry over between lines.
#include "hl.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include "ui_internal.h"

#define HL_MAX 4096  // longer text renders plain

struct hl_lang {
    const char *names;          // tags and extensions, space separated (" rs rust ")
    const char *const *kw;      // keywords, NULL-terminated
    bool slash, hash, backtick; // `//` + `/* */` comments, `#` comments, `...` strings
    bool json;                  // keys before ':' get their own colour
};

static const char *const KW_C[] = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "int", "long", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "bool", "true", "false", "NULL",
    "class", "namespace", "template", "typename", "public", "private", "protected", "virtual", "new", "delete", "nullptr",
    "include", "define", "ifdef", "ifndef", "endif", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int32_t",
    "int64_t", "size_t", "inline", NULL};
static const char *const KW_RUST[] = {"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
    "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub",
    "ref", "return", "self", "Self", "static", "struct", "super", "trait", "true", "type", "unsafe", "use", "where",
    "while", "Some", "None", "Ok", "Err", "Option", "Result", "String", "Vec", "Box", "u8", "u16", "u32", "u64", "i32",
    "i64", "usize", "f32", "f64", "bool", "str", NULL};
static const char *const KW_PY[] = {"and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None",
    "nonlocal", "not", "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", "self", NULL};
static const char *const KW_JS[] = {"async", "await", "break", "case", "catch", "class", "const", "continue", "default",
    "delete", "do", "else", "export", "extends", "false", "finally", "for", "from", "function", "if", "import", "in",
    "instanceof", "let", "new", "null", "of", "return", "static", "super", "switch", "this", "throw", "true", "try",
    "typeof", "undefined", "var", "void", "while", "yield", "interface", "type", "enum", "implements", "readonly",
    "string", "number", "boolean", "any", NULL};
static const char *const KW_GO[] = {"break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface", "map", "package", "range", "return",
    "select", "struct", "switch", "type", "var", "nil", "true", "false", "string", "int", "error", "bool", "byte", NULL};
static const char *const KW_SH[] = {"if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "until",
    "case", "esac", "function", "return", "local", "export", "set", "unset", "echo", "exit", "source", "cd", "sudo",
    "readonly", "shift", NULL};
static const char *const KW_DATA[] = {"true", "false", "null", "yes", "no", "on", "off", NULL};

static const hl_lang_t LANGS[] = {
    {" c h cc cpp hpp cxx ino java kt kts swift cs ", KW_C, true, false, false, false},
    {" rs rust ", KW_RUST, true, false, false, false},
    {" py python pyi ", KW_PY, false, true, false, false},
    {" js jsx ts tsx mjs cjs javascript typescript ", KW_JS, true, false, true, false},
    {" go golang ", KW_GO, true, false, true, false},
    {" sh bash zsh shell console fish ", KW_SH, false, true, false, false},
    {" json jsonc ", KW_DATA, false, false, false, true},
    {" toml yaml yml ini cfg conf cmake txt make mk dockerfile ", KW_DATA, false, true, false, false},
};

const hl_lang_t *hl_find(const char *name)
{
    if (!name || !name[0]) return NULL;
    const char *dot = strrchr(name, '.');
    const char *key = dot ? dot + 1 : name;
    char k[24];
    size_t n = 0;
    for (; key[n] && n < sizeof k - 3; n++) k[n] = (char)tolower((unsigned char)key[n]);
    if (!strcmp(name, "Makefile") || !strcmp(name, "CMakeLists.txt")) return &LANGS[7];
    char pat[28];
    snprintf(pat, sizeof pat, " %.*s ", (int)n, k);
    for (size_t i = 0; i < sizeof LANGS / sizeof *LANGS; i++)
        if (strstr(LANGS[i].names, pat)) return &LANGS[i];
    return NULL;
}

// ---------------------------------------------------------------- colours

typedef enum { H_PLAIN, H_KW, H_STR, H_COM, H_NUM, H_FN, H_KEY, H_ADD, H_DEL, H_WARN, H_CMD, H_N } hl_cls_t;

static lv_color_t cls_color(hl_cls_t c)
{
    // One-Dark-ish on dark themes, Catppuccin-Latte-ish on light ones.
    static const uint32_t dark[H_N] = {0, 0xc678dd, 0x98c379, 0, 0xd19a66, 0x61afef, 0xe5c07b, 0x98c379, 0xe06c75, 0xe5c07b, 0};
    static const uint32_t light[H_N] = {0, 0x8839ef, 0x40a02b, 0, 0xc2410c, 0x1e66f5, 0xb45309, 0x2f8132, 0xd20f39, 0xb45309, 0};
    switch (c) {
    case H_PLAIN: return P.text;
    case H_COM: return P.text3;
    case H_CMD: return P.accent;
    default: return lv_color_hex(P.light ? light[c] : dark[c]);
    }
}

// ---------------------------------------------------------------- span runs

typedef struct {
    lv_obj_t *sg;
    char *buf;
    int len;
    hl_cls_t cls;
} out_t;

static void flush(out_t *o)
{
    if (!o->len) return;
    o->buf[o->len] = 0;
    lv_span_t *sp = lv_spangroup_add_span(o->sg);
    lv_span_set_text(sp, o->buf);
    lv_style_set_text_color(lv_span_get_style(sp), cls_color(o->cls));
    o->len = 0;
}

static void emit(out_t *o, const char *s, int n, hl_cls_t c)
{
    if (n <= 0) return;
    if (c != o->cls) {
        flush(o);
        o->cls = c;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool is_kw(const hl_lang_t *l, const char *s, int n)
{
    for (const char *const *k = l->kw; *k; k++)
        if ((int)strlen(*k) == n && !memcmp(*k, s, n)) return true;
    return false;
}

/// One line of code; `in_block` carries a /* comment */ across lines.
static void code_line(out_t *o, const char *s, int n, const hl_lang_t *l, bool *in_block)
{
    int i = 0;
    while (i < n) {
        if (*in_block) {
            const char *e = NULL;
            for (int j = i; j + 1 < n; j++)
                if (s[j] == '*' && s[j + 1] == '/') {
                    e = s + j + 2;
                    break;
                }
            int end = e ? (int)(e - s) : n;
            emit(o, s + i, end - i, H_COM);
            i = end;
            if (e) *in_block = false;
            continue;
        }
        char c = s[i], nx = i + 1 < n ? s[i + 1] : 0;
        if (!l) {
            emit(o, s + i, n - i, H_PLAIN);
            return;
        }
        if ((l->slash && c == '/' && nx == '/') || (l->hash && c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))) {
            emit(o, s + i, n - i, H_COM);
            return;
        }
        if (l->slash && c == '/' && nx == '*') {
            *in_block = true;
            emit(o, s + i, 2, H_COM);
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && l->backtick)) {
            int j = i + 1;
            while (j < n && s[j] != c) j += s[j] == '\\' ? 2 : 1;
            if (j > n) j = n;
            int end = j < n ? j + 1 : n;
            // JSON object keys: a string followed by ':'
            int k = end;
            while (k < n && s[k] == ' ') k++;
            emit(o, s + i, end - i, l->json && k < n && s[k] == ':' ? H_KEY : H_STR);
            i = end;
            continue;
        }
        if (isdigit((unsigned char)c) && (i == 0 || !word_ch(s[i - 1]))) {
            int j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            emit(o, s + i, j - i, H_NUM);
            i = j;
            continue;
        }
        if (word_ch(c)) {
            int j = i;
            while (j < n && word_ch(s[j])) j++;
            hl_cls_t cls = is_kw(l, s + i, j - i) ? H_KW : (j < n && s[j] == '(') ? H_FN : H_PLAIN;
            emit(o, s + i, j - i, cls);
            i = j;
            continue;
        }
        emit(o, s + i, 1, H_PLAIN);
        i++;
    }
}

/// `w` appears in the line, and not as a zero count ("0 failed", "0 errors" are good news).
static bool has_word(const char *s, int n, const char *w)
{
    int wl = (int)strlen(w);
    for (int i = 0; i + wl <= n; i++)
        if (!strncasecmp(s + i, w, wl) && !(i >= 2 && s[i - 1] == ' ' && s[i - 2] == '0' && (i < 3 || !isdigit((unsigned char)s[i - 3]))))
            return true;
    return false;
}

/// A line of command output: the command itself, failures, warnings, successes.
static hl_cls_t output_class(const char *p, int n, int ln)
{
    if (ln == 0 && n > 1 && p[0] == '$') return H_CMD;
    if (has_word(p, n, "error") || has_word(p, n, "failed") || has_word(p, n, "panicked") || has_word(p, n, "fatal"))
        return H_DEL;
    if (has_word(p, n, "warning")) return H_WARN;
    if (has_word(p, n, "passed") || has_word(p, n, "result: ok") || has_word(p, n, "success") || has_word(p, n, "complete"))
        return H_ADD;
    return H_PLAIN;
}

lv_obj_t *hl_render(lv_obj_t *parent, const char *src, int w, const hl_lang_t *lang, hl_mode_t mode, int max_lines)
{
    size_t total = strlen(src);
    if (total > HL_MAX || (!lang && mode == HL_CODE)) {
        lv_obj_t *l = ui_label(parent, P.fmono, P.text, src);
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, max_lines > 0 ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(l, 2, 0);
        if (max_lines > 0) {
            lv_obj_set_height(l, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(l, max_lines * (lv_font_get_line_height(P.fmono) + 2), 0);
        }
        return l;
    }
    lv_obj_t *sg = lv_spangroup_create(parent);
    // Taps go to whatever holds the code (a tool row opens its popup), not the text.
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sg, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(sg, w);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, P.fmono, 0);
    lv_obj_set_style_text_line_space(sg, 2, 0);
    if (max_lines > 0) {
        lv_spangroup_set_max_lines(sg, max_lines);
        lv_spangroup_set_overflow(sg, LV_SPAN_OVERFLOW_ELLIPSIS);
    }
    out_t o = {.sg = sg, .buf = lv_malloc(total + 2), .cls = H_PLAIN};
    if (!o.buf) return sg;
    bool in_block = false;
    const char *p = src, *end = src + total;
    for (int ln = 0; p < end; ln++) {
        const char *eol = memchr(p, '\n', end - p);
        if (!eol) eol = end;
        int n = (int)(eol - p);
        if (mode == HL_DIFF && n >= 1 && p[0] == '-') {
            emit(&o, p, n, H_DEL);
        } else if (mode == HL_DIFF && n >= 1 && p[0] == '+') {
            emit(&o, p, 1, H_ADD);
            code_line(&o, p + 1, n - 1, lang, &in_block);
        } else if (mode == HL_OUTPUT) {
            emit(&o, p, n, output_class(p, n, ln));
        } else {
            code_line(&o, p, n, lang, &in_block);
        }
        if (eol < end) emit(&o, "\n", 1, o.cls);
        p = eol < end ? eol + 1 : end;
    }
    flush(&o);
    lv_free(o.buf);
    lv_spangroup_refresh(sg);
    return sg;
}
