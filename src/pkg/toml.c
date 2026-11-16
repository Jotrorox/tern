/* toml.c — the TOML subset behind tern.toml, tern.lock and registry index
 * files (see pkg.h for the supported subset), plus writer helpers.
 *
 * The parser works directly on the registered source text so every error
 * carries an exact span. It keeps going after an error (resynchronizing at
 * the next line that looks like a key or a header), so one run reports
 * every problem in the file. */
#include "pkg.h"
#include "../base/utf8.h"
#include "../os/os.h"

#define TOML_F_INLINE 1u   /* inline table / array value: immutable */
#define TOML_F_DEFINED 2u  /* table defined by a [header] or a key */
#define TOML_F_DOTTED 4u   /* table created by a dotted key */
#define TOML_F_AOT 8u      /* array of tables ([[x]]) */
#define TOML_MAX_KEY_PARTS 16
#define TOML_MAX_DEPTH 64

typedef struct TomlKeyPart {
    const char *s;
    Span span;
} TomlKeyPart;

typedef struct TomlParser {
    Arena *a;
    uint32_t file;
    const char *s;
    size_t len, p;
    TomlValue *root, *cur;
    int errors;
} TomlParser;

const char *toml_kind_name(TomlKind k)
{
    switch (k) {
    case TOML_STRING: return "string";
    case TOML_INT: return "integer";
    case TOML_BOOL: return "boolean";
    case TOML_ARRAY: return "array";
    case TOML_TABLE: return "table";
    }
    return "value";
}

static void toml_err(TomlParser *P, size_t off, size_t len, const char *fmt, ...) TERN_PRINTF(4, 5);

static void toml_err(TomlParser *P, size_t off, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (off > P->len)
        off = P->len;
    if (off + len > P->len)
        len = P->len - off;
    va_start(ap, fmt);
    diag_verror(span_make(P->file, (uint32_t)off, (uint32_t)len), fmt, ap);
    va_end(ap);
    P->errors++;
}

/* Description of the character at p for messages: "`x`", "end of line". */
static const char *toml_describe(TomlParser *P, size_t p, char buf[16])
{
    unsigned char c;
    if (p >= P->len)
        return "end of file";
    c = (unsigned char)P->s[p];
    if (c == '\n' || (c == '\r' && p + 1 < P->len && P->s[p + 1] == '\n'))
        return "end of line";
    if (c < 0x20 || c == 0x7F) {
        snprintf(buf, 16, "byte 0x%02X", c);
        return buf;
    }
    if (c >= 0x80)
        return "a non-ASCII character";
    buf[0] = '`';
    buf[1] = (char)c;
    buf[2] = '`';
    buf[3] = 0;
    return buf;
}

static int toml_peek(TomlParser *P, size_t ahead)
{
    return P->p + ahead < P->len ? (unsigned char)P->s[P->p + ahead] : -1;
}

static void toml_skip_ws(TomlParser *P)
{
    while (P->p < P->len && (P->s[P->p] == ' ' || P->s[P->p] == '\t'))
        P->p++;
}

static int toml_at_eol(TomlParser *P)
{
    return P->p >= P->len || P->s[P->p] == '\n' ||
           (P->s[P->p] == '\r' && P->p + 1 < P->len && P->s[P->p + 1] == '\n');
}

static void toml_skip_comment(TomlParser *P)
{
    if (P->p < P->len && P->s[P->p] == '#') {
        while (P->p < P->len && !toml_at_eol(P))
            P->p++;
    }
}

static void toml_consume_eol(TomlParser *P)
{
    if (P->p < P->len && P->s[P->p] == '\r')
        P->p++;
    if (P->p < P->len && P->s[P->p] == '\n')
        P->p++;
}

static void toml_skip_line(TomlParser *P)
{
    while (P->p < P->len && P->s[P->p] != '\n')
        P->p++;
    if (P->p < P->len)
        P->p++;
}

/* After an error: skip to the start of the next line that looks like a
 * header or a `key =` line, so a broken multi-line array does not produce
 * a cascade of follow-up errors. */
static void toml_resync(TomlParser *P)
{
    toml_skip_line(P);
    while (P->p < P->len) {
        size_t q = P->p, e;
        int c;
        while (q < P->len && (P->s[q] == ' ' || P->s[q] == '\t'))
            q++;
        c = q < P->len ? (unsigned char)P->s[q] : -1;
        if (c == '[' && q == P->p)
            return;
        if (tern_is_ident(c) || c == '"' || c == '\'') {
            for (e = q; e < P->len && P->s[e] != '\n'; e++) {
                if (P->s[e] == '=')
                    return;
            }
        }
        if (c == '#' || c == '\n' || c == '\r')
            return;
        toml_skip_line(P);
    }
}

/* Skip whitespace, comments and newlines (inside arrays). */
static void toml_skip_ws_nl(TomlParser *P)
{
    for (;;) {
        toml_skip_ws(P);
        toml_skip_comment(P);
        if (P->p < P->len && toml_at_eol(P))
            toml_consume_eol(P);
        else
            return;
    }
}

/* Expect end of line (after optional whitespace and comment); consumes it.
 * Returns 0, or -1 after reporting. */
static int toml_expect_eol(TomlParser *P, const char *after)
{
    char buf[16];
    toml_skip_ws(P);
    toml_skip_comment(P);
    if (toml_at_eol(P)) {
        toml_consume_eol(P);
        return 0;
    }
    toml_err(P, P->p, 1, "expected end of line after %s, found %s", after, toml_describe(P, P->p, buf));
    diag_help("put each key/value pair and each [table] header on its own line");
    return -1;
}

static TomlValue *toml_new(TomlParser *P, TomlKind kind, size_t off, size_t len)
{
    TomlValue *v = ANEW(P->a, TomlValue);
    v->kind = kind;
    v->span = span_make(P->file, (uint32_t)off, (uint32_t)len);
    return v;
}

static void toml_append(TomlValue *parent, TomlValue *child)
{
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
    parent->count++;
}

static TomlValue *toml_find(const TomlValue *t, const char *key)
{
    TomlValue *c;
    if (!t || t->kind != TOML_TABLE)
        return NULL;
    for (c = t->first; c; c = c->next) {
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    }
    return NULL;
}

const TomlValue *toml_get(const TomlValue *table, const char *key)
{
    return toml_find(table, key);
}

/* ------------------------------------------------------------- strings -- */

static int toml_hex_escape(TomlParser *P, int ndigits, StrBuf *sb)
{
    uint32_t cp = 0;
    int i;
    char enc[4];
    int n;
    size_t start = P->p - 2;
    for (i = 0; i < ndigits; i++) {
        int c = toml_peek(P, 0), h = tern_hex_value(c);
        if (h < 0) {
            toml_err(P, start, P->p - start + 1, "invalid escape: \\%c needs %d hex digits", ndigits == 4 ? 'u' : 'U',
                     ndigits);
            return -1;
        }
        cp = cp * 16 + (uint32_t)h;
        P->p++;
    }
    n = utf8_encode(cp, enc);
    if (n == 0) {
        toml_err(P, start, P->p - start, "invalid escape: U+%04X is not a Unicode scalar value", (unsigned)cp);
        return -1;
    }
    sb_append(sb, enc, (size_t)n);
    return 0;
}

/* Parses a string starting at P->p (one of " ' """ '''). On success stores
 * the arena copy and returns 0. */
static int toml_parse_string(TomlParser *P, const char **out, size_t *out_len)
{
    StrBuf sb = {0};
    size_t start = P->p;
    int quote = P->s[P->p];
    int multi = toml_peek(P, 1) == quote && toml_peek(P, 2) == quote;
    int literal = quote == '\'';
    P->p += multi ? 3 : 1;
    if (multi) {
        /* a newline right after the opening delimiter is trimmed */
        if (toml_peek(P, 0) == '\n')
            P->p++;
        else if (toml_peek(P, 0) == '\r' && toml_peek(P, 1) == '\n')
            P->p += 2;
    }
    for (;;) {
        int c = toml_peek(P, 0);
        if (c < 0) {
            toml_err(P, start, multi ? 3 : 1, "unterminated string");
            diag_help("add the closing %s", multi ? (literal ? "'''" : "\"\"\"") : (literal ? "'" : "\""));
            sb_free(&sb);
            return -1;
        }
        if (c == quote) {
            if (!multi) {
                P->p++;
                break;
            }
            if (toml_peek(P, 1) == quote && toml_peek(P, 2) == quote) {
                /* up to two extra quotes before the closing delimiter belong to the string */
                int extra = 0;
                while (extra < 2 && toml_peek(P, 3 + extra) == quote)
                    extra++;
                sb_append(&sb, P->s + P->p, (size_t)extra);
                P->p += 3 + (size_t)extra;
                break;
            }
            sb_putc(&sb, c);
            P->p++;
            continue;
        }
        if (c == '\n' || (c == '\r' && toml_peek(P, 1) == '\n')) {
            if (!multi) {
                toml_err(P, start, P->p - start, "unterminated string: strings cannot span lines");
                diag_help("use a multi-line string (\"\"\"...\"\"\") or close the quote");
                sb_free(&sb);
                return -1;
            }
            if (c == '\r')
                P->p++;
            sb_putc(&sb, '\n');
            P->p++;
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            toml_err(P, P->p, 1, "control character 0x%02X is not allowed in strings", (unsigned)c);
            diag_help("write it as an escape such as \\u%04X", (unsigned)c);
            sb_free(&sb);
            return -1;
        }
        if (c == '\\' && !literal) {
            int e = toml_peek(P, 1);
            P->p += 2;
            switch (e) {
            case 'b': sb_putc(&sb, '\b'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case 'n': sb_putc(&sb, '\n'); break;
            case 'f': sb_putc(&sb, '\f'); break;
            case 'r': sb_putc(&sb, '\r'); break;
            case '"': sb_putc(&sb, '"'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            case 'u':
                if (toml_hex_escape(P, 4, &sb) != 0) {
                    sb_free(&sb);
                    return -1;
                }
                break;
            case 'U':
                if (toml_hex_escape(P, 8, &sb) != 0) {
                    sb_free(&sb);
                    return -1;
                }
                break;
            default:
                if (multi && (e == ' ' || e == '\t' || e == '\n' || e == '\r')) {
                    /* line-ending backslash: trim whitespace and newlines */
                    size_t q = P->p - 1;
                    int saw_nl = 0;
                    while (q < P->len && (P->s[q] == ' ' || P->s[q] == '\t' || P->s[q] == '\n' || P->s[q] == '\r')) {
                        if (P->s[q] == '\n')
                            saw_nl = 1;
                        q++;
                    }
                    if (saw_nl) {
                        P->p = q;
                        break;
                    }
                }
                toml_err(P, P->p - 2, 2, "invalid escape sequence `\\%c`", e > 0x20 && e < 0x7F ? e : '?');
                diag_help("valid escapes: \\\" \\\\ \\n \\t \\r \\b \\f \\uXXXX \\UXXXXXXXX; or use a 'literal string'");
                sb_free(&sb);
                return -1;
            }
            continue;
        }
        if (c >= 0x80) {
            size_t adv;
            if (utf8_decode(P->s + P->p, P->len - P->p, &adv) == UTF8_INVALID) {
                toml_err(P, P->p, 1, "invalid UTF-8 in string");
                sb_free(&sb);
                return -1;
            }
            sb_append(&sb, P->s + P->p, adv);
            P->p += adv;
            continue;
        }
        sb_putc(&sb, c);
        P->p++;
    }
    *out = arena_strndup(P->a, sb.data ? sb.data : "", sb.len);
    *out_len = sb.len;
    sb_free(&sb);
    return 0;
}

/* ---------------------------------------------------------------- keys -- */

static int toml_is_bare(int c)
{
    return tern_is_alnum(c) || c == '_' || c == '-';
}

static int toml_parse_key(TomlParser *P, TomlKeyPart *parts, int *nparts)
{
    char buf[16];
    int n = 0;
    for (;;) {
        size_t start;
        toml_skip_ws(P);
        start = P->p;
        if (n == TOML_MAX_KEY_PARTS) {
            toml_err(P, start, 1, "key has too many dotted parts (at most %d)", TOML_MAX_KEY_PARTS);
            return -1;
        }
        if (toml_peek(P, 0) == '"' || toml_peek(P, 0) == '\'') {
            size_t len;
            if (toml_peek(P, 1) == toml_peek(P, 0) && toml_peek(P, 2) == toml_peek(P, 0)) {
                toml_err(P, start, 3, "multi-line strings cannot be used as keys");
                return -1;
            }
            if (toml_parse_string(P, &parts[n].s, &len) != 0)
                return -1;
        } else if (toml_is_bare(toml_peek(P, 0))) {
            while (toml_is_bare(toml_peek(P, 0)))
                P->p++;
            parts[n].s = arena_strndup(P->a, P->s + start, P->p - start);
        } else {
            toml_err(P, start, 1, "expected a key, found %s", toml_describe(P, start, buf));
            if (toml_peek(P, 0) == '=')
                diag_help("a key is missing before `=`");
            else
                diag_help("bare keys may contain A-Z a-z 0-9 _ -; quote anything else (\"my key\")");
            return -1;
        }
        parts[n].span = span_make(P->file, (uint32_t)start, (uint32_t)(P->p - start));
        n++;
        toml_skip_ws(P);
        if (toml_peek(P, 0) != '.')
            break;
        P->p++;
    }
    *nparts = n;
    return 0;
}

/* -------------------------------------------------------------- values -- */

static TomlValue *toml_parse_value(TomlParser *P, int depth);

static int toml_insert_kv(TomlParser *P, TomlValue *table, TomlKeyPart *parts, int n, TomlValue *val);

static TomlValue *toml_parse_number(TomlParser *P)
{
    size_t start = P->p, end = P->p, i;
    int neg = 0, base = 10;
    uint64_t mag = 0;
    const char *d;
    size_t dlen;
    int prev_digit = 0;
    TomlValue *v;
    while (end < P->len) {
        int c = (unsigned char)P->s[end];
        if (tern_is_alnum(c) || c == '_' || c == '+' || c == '-' || c == '.' || c == ':')
            end++;
        else
            break;
    }
    d = P->s + start;
    dlen = end - start;
    P->p = end;
    for (i = 0; i < dlen; i++) {
        if (d[i] == ':' || (i > 0 && d[i] == '-' && tern_is_digit(d[i - 1]) && dlen >= 10)) {
            toml_err(P, start, dlen, "dates and times are not supported in this file");
            diag_help("write it as a string: \"%.*s\"", (int)dlen, d);
            return NULL;
        }
    }
    if (*d == '+' || *d == '-') {
        neg = *d == '-';
        d++;
        dlen--;
    }
    if (dlen >= 2 && d[0] == '0' && (d[1] == 'x' || d[1] == 'o' || d[1] == 'b')) {
        base = d[1] == 'x' ? 16 : d[1] == 'o' ? 8 : 2;
        if (d != P->s + start) {
            toml_err(P, start, end - start, "a sign is not allowed on hexadecimal, octal or binary integers");
            return NULL;
        }
        d += 2;
        dlen -= 2;
    } else {
        for (i = 0; i < dlen; i++) {
            if (d[i] == '.' || d[i] == 'e' || d[i] == 'E' || (dlen == 3 && (memcmp(d, "inf", 3) == 0 || memcmp(d, "nan", 3) == 0))) {
                toml_err(P, start, end - start, "floating-point values are not supported in this file");
                diag_help("use an integer, or a string such as \"%.*s\"", (int)(end - start), P->s + start);
                return NULL;
            }
        }
        if (dlen > 1 && d[0] == '0') {
            toml_err(P, start, end - start, "leading zeros are not allowed in decimal integers");
            diag_help("write `%.*s` without the leading zero, or use the 0o prefix for octal", (int)(end - start),
                      P->s + start);
            return NULL;
        }
    }
    if (dlen == 0) {
        toml_err(P, start, end - start, "invalid integer `%.*s`", (int)(end - start), P->s + start);
        return NULL;
    }
    for (i = 0; i < dlen; i++) {
        int c = (unsigned char)d[i], dv;
        if (c == '_') {
            if (!prev_digit || i + 1 == dlen || d[i + 1] == '_') {
                toml_err(P, start, end - start, "`_` in an integer must be between two digits");
                return NULL;
            }
            prev_digit = 0;
            continue;
        }
        dv = tern_hex_value(c);
        if (dv < 0 || dv >= base) {
            toml_err(P, start, end - start, "invalid digit `%c` in %s integer `%.*s`", c,
                     base == 16 ? "hexadecimal" : base == 8 ? "octal" : base == 2 ? "binary" : "decimal",
                     (int)(end - start), P->s + start);
            return NULL;
        }
        if (mag > (UINT64_MAX - (uint64_t)dv) / (uint64_t)base) {
            toml_err(P, start, end - start, "integer `%.*s` does not fit in 64 bits", (int)(end - start), P->s + start);
            return NULL;
        }
        mag = mag * (uint64_t)base + (uint64_t)dv;
        prev_digit = 1;
    }
    if ((!neg && mag > (uint64_t)INT64_MAX) || (neg && mag > (uint64_t)INT64_MAX + 1)) {
        toml_err(P, start, end - start, "integer `%.*s` does not fit in 64 bits", (int)(end - start), P->s + start);
        return NULL;
    }
    v = toml_new(P, TOML_INT, start, end - start);
    if (neg)
        v->num = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
    else
        v->num = (int64_t)mag;
    return v;
}

static TomlValue *toml_parse_array(TomlParser *P, int depth)
{
    size_t start = P->p;
    TomlValue *arr = toml_new(P, TOML_ARRAY, start, 1);
    char buf[16];
    arr->flags = TOML_F_INLINE;
    P->p++; /* [ */
    for (;;) {
        TomlValue *item;
        toml_skip_ws_nl(P);
        if (P->p >= P->len) {
            toml_err(P, start, 1, "unterminated array");
            diag_help("add the closing `]`");
            return NULL;
        }
        if (P->s[P->p] == ']') {
            P->p++;
            break;
        }
        item = toml_parse_value(P, depth + 1);
        if (!item)
            return NULL;
        toml_append(arr, item);
        toml_skip_ws_nl(P);
        if (P->p < P->len && P->s[P->p] == ',') {
            P->p++;
            continue;
        }
        if (P->p < P->len && P->s[P->p] == ']') {
            P->p++;
            break;
        }
        toml_err(P, P->p, 1, "expected `,` or `]` in array, found %s", toml_describe(P, P->p, buf));
        diag_help("separate array elements with commas");
        return NULL;
    }
    arr->span = span_make(P->file, (uint32_t)start, (uint32_t)(P->p - start));
    return arr;
}

static TomlValue *toml_parse_inline_table(TomlParser *P, int depth)
{
    size_t start = P->p;
    TomlValue *t = toml_new(P, TOML_TABLE, start, 1);
    char buf[16];
    int first = 1;
    t->flags = TOML_F_INLINE | TOML_F_DEFINED;
    P->p++; /* { */
    for (;;) {
        TomlKeyPart parts[TOML_MAX_KEY_PARTS];
        int n;
        TomlValue *val;
        toml_skip_ws(P);
        if (toml_at_eol(P)) {
            toml_err(P, start, P->p - start, "inline tables must be written on one line");
            diag_help("close it with `}` on this line, or use a [table] section instead");
            return NULL;
        }
        if (first && P->s[P->p] == '}') {
            P->p++;
            break;
        }
        first = 0;
        if (toml_parse_key(P, parts, &n) != 0)
            return NULL;
        toml_skip_ws(P);
        if (toml_peek(P, 0) != '=') {
            toml_err(P, P->p, 1, "expected `=` after key `%s`, found %s", parts[n - 1].s, toml_describe(P, P->p, buf));
            return NULL;
        }
        P->p++;
        toml_skip_ws(P);
        val = toml_parse_value(P, depth + 1);
        if (!val)
            return NULL;
        if (toml_insert_kv(P, t, parts, n, val) != 0)
            return NULL;
        toml_skip_ws(P);
        if (toml_peek(P, 0) == ',') {
            P->p++;
            toml_skip_ws(P);
            if (toml_peek(P, 0) == '}') {
                toml_err(P, P->p - 1, 1, "trailing comma is not allowed in an inline table");
                return NULL;
            }
            continue;
        }
        if (toml_peek(P, 0) == '}') {
            P->p++;
            break;
        }
        if (toml_at_eol(P)) {
            toml_err(P, start, P->p - start, "inline tables must be written on one line");
            diag_help("close it with `}` on this line, or use a [table] section instead");
        } else {
            toml_err(P, P->p, 1, "expected `,` or `}` in inline table, found %s", toml_describe(P, P->p, buf));
        }
        return NULL;
    }
    t->span = span_make(P->file, (uint32_t)start, (uint32_t)(P->p - start));
    return t;
}

static int toml_word_at(TomlParser *P, const char *w)
{
    size_t n = strlen(w);
    return P->p + n <= P->len && memcmp(P->s + P->p, w, n) == 0 &&
           (P->p + n == P->len || !toml_is_bare((unsigned char)P->s[P->p + n]));
}

static TomlValue *toml_parse_value(TomlParser *P, int depth)
{
    int c = toml_peek(P, 0);
    char buf[16];
    if (depth > TOML_MAX_DEPTH) {
        toml_err(P, P->p, 1, "values are nested too deeply");
        return NULL;
    }
    if (c == '"' || c == '\'') {
        size_t start = P->p;
        TomlValue *v;
        const char *s;
        size_t len;
        if (toml_parse_string(P, &s, &len) != 0)
            return NULL;
        v = toml_new(P, TOML_STRING, start, P->p - start);
        v->str = s;
        v->str_len = len;
        return v;
    }
    if (c == '[')
        return toml_parse_array(P, depth);
    if (c == '{')
        return toml_parse_inline_table(P, depth);
    if (toml_word_at(P, "true") || toml_word_at(P, "false")) {
        int t = c == 't';
        TomlValue *v = toml_new(P, TOML_BOOL, P->p, t ? 4 : 5);
        v->boolean = t;
        P->p += t ? 4 : 5;
        return v;
    }
    if (tern_is_digit(c) || c == '+' || c == '-' || toml_word_at(P, "inf") || toml_word_at(P, "nan"))
        return toml_parse_number(P);
    if (toml_at_eol(P) || c == '#') {
        toml_err(P, P->p, 1, "expected a value after `=`, found %s", c == '#' ? "a comment" : toml_describe(P, P->p, buf));
        return NULL;
    }
    if (tern_is_alpha(c)) {
        size_t start = P->p, e = P->p;
        while (e < P->len && toml_is_bare((unsigned char)P->s[e]))
            e++;
        toml_err(P, start, e - start, "expected a value, found `%.*s`", (int)(e - start), P->s + start);
        if ((e - start == 4 && tern_strcasecmp("true", arena_strndup(P->a, P->s + start, 4)) == 0) ||
            (e - start == 5 && tern_strcasecmp("false", arena_strndup(P->a, P->s + start, 5)) == 0))
            diag_help("booleans are lowercase: `true` / `false`");
        else
            diag_help("strings must be quoted: \"%.*s\"", (int)(e - start), P->s + start);
        return NULL;
    }
    toml_err(P, P->p, 1, "expected a value (string, integer, boolean, array or inline table), found %s",
             toml_describe(P, P->p, buf));
    return NULL;
}

/* ------------------------------------------------------- table building -- */

static void toml_note_prev(const TomlValue *prev)
{
    if (prev->key_span.file)
        diag_note(prev->key_span, "`%s` was first defined here", prev->key);
    else if (prev->span.file)
        diag_note(prev->span, "first defined here");
}

static int toml_insert_kv(TomlParser *P, TomlValue *table, TomlKeyPart *parts, int n, TomlValue *val)
{
    TomlValue *t = table, *c;
    int i;
    for (i = 0; i < n - 1; i++) {
        c = toml_find(t, parts[i].s);
        if (!c) {
            c = toml_new(P, TOML_TABLE, parts[i].span.off, parts[i].span.len);
            c->key = parts[i].s;
            c->key_span = parts[i].span;
            c->flags = TOML_F_DOTTED;
            toml_append(t, c);
        } else if (c->kind != TOML_TABLE || (c->flags & TOML_F_INLINE) || !(c->flags & TOML_F_DOTTED)) {
            toml_err(P, parts[i].span.off, parts[i].span.len, "cannot add keys to `%s`: it is already defined as %s",
                     parts[i].s, c->kind == TOML_TABLE ? "a table" : toml_kind_name(c->kind));
            toml_note_prev(c);
            return -1;
        }
        t = c;
    }
    c = toml_find(t, parts[n - 1].s);
    if (c) {
        toml_err(P, parts[n - 1].span.off, parts[n - 1].span.len, "duplicate key `%s`", parts[n - 1].s);
        toml_note_prev(c);
        diag_help("remove one of the two definitions");
        return -1;
    }
    val->key = parts[n - 1].s;
    val->key_span = parts[n - 1].span;
    if (val->kind == TOML_TABLE)
        val->flags |= TOML_F_DEFINED;
    toml_append(t, val);
    return 0;
}

static int toml_header(TomlParser *P)
{
    size_t start = P->p;
    int aot = toml_peek(P, 1) == '[';
    TomlKeyPart parts[TOML_MAX_KEY_PARTS];
    int n, i;
    char buf[16];
    TomlValue *t = P->root, *c;
    P->p += aot ? 2 : 1;
    if (toml_parse_key(P, parts, &n) != 0)
        return -1;
    toml_skip_ws(P);
    if (toml_peek(P, 0) != ']' || (aot && toml_peek(P, 1) != ']')) {
        toml_err(P, P->p, 1, "expected `%s` to close the table header, found %s", aot ? "]]" : "]",
                 toml_describe(P, P->p, buf));
        return -1;
    }
    P->p += aot ? 2 : 1;
    for (i = 0; i < n - 1; i++) {
        c = toml_find(t, parts[i].s);
        if (!c) {
            c = toml_new(P, TOML_TABLE, parts[i].span.off, parts[i].span.len);
            c->key = parts[i].s;
            c->key_span = parts[i].span;
            toml_append(t, c);
        } else if (c->kind == TOML_ARRAY && (c->flags & TOML_F_AOT)) {
            c = c->last;
        } else if (c->kind != TOML_TABLE || (c->flags & TOML_F_INLINE)) {
            toml_err(P, parts[i].span.off, parts[i].span.len, "`%s` is already defined as %s, not a table", parts[i].s,
                     c->kind == TOML_TABLE ? "an inline table" : toml_kind_name(c->kind));
            toml_note_prev(c);
            return -1;
        }
        t = c;
    }
    c = toml_find(t, parts[n - 1].s);
    if (aot) {
        TomlValue *elem;
        if (!c) {
            c = toml_new(P, TOML_ARRAY, start, P->p - start);
            c->key = parts[n - 1].s;
            c->key_span = parts[n - 1].span;
            c->flags = TOML_F_AOT;
            toml_append(t, c);
        } else if (c->kind != TOML_ARRAY || !(c->flags & TOML_F_AOT)) {
            toml_err(P, parts[n - 1].span.off, parts[n - 1].span.len,
                     "`%s` is already defined as %s, so [[%s]] cannot add to it", parts[n - 1].s,
                     c->kind == TOML_TABLE ? "a table" : toml_kind_name(c->kind), parts[n - 1].s);
            toml_note_prev(c);
            return -1;
        }
        elem = toml_new(P, TOML_TABLE, start, P->p - start);
        elem->flags = TOML_F_DEFINED;
        toml_append(c, elem);
        P->cur = elem;
        return 0;
    }
    if (!c) {
        c = toml_new(P, TOML_TABLE, start, P->p - start);
        c->key = parts[n - 1].s;
        c->key_span = parts[n - 1].span;
        c->flags = TOML_F_DEFINED;
        toml_append(t, c);
    } else if (c->kind == TOML_TABLE && !(c->flags & (TOML_F_DEFINED | TOML_F_DOTTED | TOML_F_INLINE))) {
        c->flags |= TOML_F_DEFINED; /* was created implicitly by an earlier [a.b.c] */
        c->span = span_make(P->file, (uint32_t)start, (uint32_t)(P->p - start));
    } else {
        Span hs = span_make(P->file, (uint32_t)start, (uint32_t)(P->p - start));
        if (c->kind == TOML_TABLE) {
            diag_error(hs, "table [%.*s] is defined more than once", (int)(hs.len - 2), P->s + start + 1);
            P->errors++;
            toml_note_prev(c);
            diag_help("merge the two sections into one");
        } else {
            diag_error(hs, "`%s` is already defined as %s, not a table", parts[n - 1].s, toml_kind_name(c->kind));
            P->errors++;
            toml_note_prev(c);
        }
        return -1;
    }
    P->cur = c;
    return 0;
}

TomlValue *toml_parse(Arena *a, uint32_t src_id)
{
    TomlParser P;
    const SourceFile *f = src_get(src_id);
    char buf[16];
    if (!f)
        return NULL;
    memset(&P, 0, sizeof P);
    P.a = a;
    P.file = src_id;
    P.s = f->text;
    P.len = f->len;
    P.root = toml_new(&P, TOML_TABLE, 0, 0);
    P.root->flags = TOML_F_DEFINED;
    P.cur = P.root;
    /* skip a UTF-8 byte order mark */
    if (P.len >= 3 && (unsigned char)P.s[0] == 0xEF && (unsigned char)P.s[1] == 0xBB && (unsigned char)P.s[2] == 0xBF)
        P.p = 3;
    while (P.p < P.len) {
        int c;
        toml_skip_ws(&P);
        toml_skip_comment(&P);
        if (P.p >= P.len)
            break;
        if (toml_at_eol(&P)) {
            toml_consume_eol(&P);
            continue;
        }
        c = (unsigned char)P.s[P.p];
        if (c == '[') {
            if (toml_header(&P) != 0 || toml_expect_eol(&P, "the table header") != 0) {
                toml_resync(&P);
                /* keys after a broken header must not land in the previous table */
                P.cur = toml_new(&P, TOML_TABLE, P.p, 0);
            }
            continue;
        }
        if (c == '\r') {
            toml_err(&P, P.p, 1, "stray carriage return (line endings must be LF or CRLF)");
            toml_skip_line(&P);
            continue;
        }
        {
            TomlKeyPart parts[TOML_MAX_KEY_PARTS];
            int n;
            TomlValue *val;
            if (toml_parse_key(&P, parts, &n) != 0) {
                toml_resync(&P);
                continue;
            }
            toml_skip_ws(&P);
            if (toml_peek(&P, 0) != '=') {
                toml_err(&P, P.p, 1, "expected `=` after key `%s`, found %s", parts[n - 1].s, toml_describe(&P, P.p, buf));
                if (toml_at_eol(&P))
                    diag_help("every key needs a value: `%s = ...`", parts[n - 1].s);
                toml_resync(&P);
                continue;
            }
            P.p++;
            toml_skip_ws(&P);
            val = toml_parse_value(&P, 0);
            if (!val || toml_insert_kv(&P, P.cur, parts, n, val) != 0 || toml_expect_eol(&P, "the value") != 0) {
                toml_resync(&P);
                continue;
            }
        }
    }
    return P.errors ? NULL : P.root;
}

TomlValue *toml_parse_file(Arena *a, const char *path, uint32_t *src_id_out)
{
    char *data;
    size_t len;
    uint32_t id;
    if (src_id_out)
        *src_id_out = 0;
    if (os_read_file(path, &data, &len) != 0) {
        diag_error(span_none(), "cannot read `%s`", path);
        return NULL;
    }
    id = src_find(path);
    if (id)
        src_update(id, data, len); /* re-reading a file (LSP, repeated loads) reuses its source */
    else
        id = src_add(path, data, len);
    free(data);
    if (src_id_out)
        *src_id_out = id;
    return toml_parse(a, id);
}

/* -------------------------------------------------------------- writer -- */

void toml_put_string(StrBuf *sb, const char *s)
{
    sb_putc(sb, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        default:
            if (c < 0x20 || c == 0x7F)
                sb_printf(sb, "\\u%04X", (unsigned)c);
            else
                sb_putc(sb, (int)c);
        }
    }
    sb_putc(sb, '"');
}

void toml_put_key(StrBuf *sb, const char *key)
{
    const char *k;
    int bare = key[0] != 0;
    for (k = key; *k; k++) {
        if (!toml_is_bare((unsigned char)*k))
            bare = 0;
    }
    if (bare)
        sb_puts(sb, key);
    else
        toml_put_string(sb, key);
}

#undef TOML_F_INLINE
#undef TOML_F_DEFINED
#undef TOML_F_DOTTED
#undef TOML_F_AOT
#undef TOML_MAX_KEY_PARTS
#undef TOML_MAX_DEPTH
