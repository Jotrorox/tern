/* json.c — recursive-descent JSON reader and streaming writer. */
#include "json.h"
#include "utf8.h"
#include "vec.h"

/* ------------------------------------------------------------- reader --- */

typedef struct JsonParser {
    Arena *arena;
    const char *s;
    size_t len;
    size_t pos;
    int failed;
    JsonError *err;
} JsonParser;

typedef VEC(JsonValue *) JsonItemVec;
typedef VEC(JsonMember) JsonMemberVec;

static void json_fail(JsonParser *p, const char *msg)
{
    size_t i;
    uint32_t line = 1, col = 1;
    if (p->failed)
        return;
    p->failed = 1;
    if (!p->err)
        return;
    for (i = 0; i < p->pos && i < p->len; i++) {
        if (p->s[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    p->err->offset = p->pos;
    p->err->line = line;
    p->err->col = col;
    snprintf(p->err->msg, sizeof p->err->msg, "%s", msg);
}

static void json_skip_ws(JsonParser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->pos++;
    }
}

static int json_peek(JsonParser *p)
{
    return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1;
}

static int json_literal(JsonParser *p, const char *word)
{
    size_t n = strlen(word);
    if (p->len - p->pos >= n && memcmp(p->s + p->pos, word, n) == 0) {
        p->pos += n;
        return 1;
    }
    return 0;
}

static int json_hex4(JsonParser *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    if (p->len - p->pos < 4)
        return 0;
    for (i = 0; i < 4; i++) {
        int d = tern_hex_value((unsigned char)p->s[p->pos + (size_t)i]);
        if (d < 0)
            return 0;
        v = (v << 4) | (uint32_t)d;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

/* Parses a string starting at the opening quote. */
static int json_parse_string(JsonParser *p, const char **out, size_t *out_len)
{
    StrBuf sb = {0};
    p->pos++; /* opening quote */
    for (;;) {
        unsigned char c;
        if (p->pos >= p->len) {
            json_fail(p, "unterminated string");
            sb_free(&sb);
            return 0;
        }
        c = (unsigned char)p->s[p->pos];
        if (c == '"') {
            p->pos++;
            break;
        }
        if (c < 0x20) {
            json_fail(p, "control character in string");
            sb_free(&sb);
            return 0;
        }
        if (c == '\\') {
            int e;
            p->pos++;
            e = json_peek(p);
            p->pos++;
            switch (e) {
            case '"': sb_putc(&sb, '"'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            case '/': sb_putc(&sb, '/'); break;
            case 'b': sb_putc(&sb, '\b'); break;
            case 'f': sb_putc(&sb, '\f'); break;
            case 'n': sb_putc(&sb, '\n'); break;
            case 'r': sb_putc(&sb, '\r'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case 'u': {
                uint32_t cp, lo;
                char buf[4];
                int n;
                if (!json_hex4(p, &cp)) {
                    json_fail(p, "bad \\u escape");
                    sb_free(&sb);
                    return 0;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->len - p->pos >= 6 && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                        size_t save = p->pos;
                        p->pos += 2;
                        if (json_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p->pos = save;
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                n = utf8_encode(cp, buf);
                sb_append(&sb, buf, (size_t)n);
                break;
            }
            default:
                p->pos--;
                json_fail(p, "bad escape in string");
                sb_free(&sb);
                return 0;
            }
            continue;
        }
        sb_putc(&sb, c);
        p->pos++;
    }
    *out_len = sb.len;
    *out = arena_strndup(p->arena, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return 1;
}

static JsonValue *json_parse_number(JsonParser *p)
{
    size_t start = p->pos;
    int is_int = 1;
    JsonValue *v;
    char buf[64];
    char *heap = NULL;
    const char *text;
    size_t n;
    if (json_peek(p) == '-')
        p->pos++;
    if (json_peek(p) == '0') {
        p->pos++;
    } else if (tern_is_digit(json_peek(p))) {
        while (tern_is_digit(json_peek(p)))
            p->pos++;
    } else {
        json_fail(p, "bad number");
        return NULL;
    }
    if (json_peek(p) == '.') {
        is_int = 0;
        p->pos++;
        if (!tern_is_digit(json_peek(p))) {
            json_fail(p, "bad number: digit expected after '.'");
            return NULL;
        }
        while (tern_is_digit(json_peek(p)))
            p->pos++;
    }
    if (json_peek(p) == 'e' || json_peek(p) == 'E') {
        is_int = 0;
        p->pos++;
        if (json_peek(p) == '+' || json_peek(p) == '-')
            p->pos++;
        if (!tern_is_digit(json_peek(p))) {
            json_fail(p, "bad number: digit expected in exponent");
            return NULL;
        }
        while (tern_is_digit(json_peek(p)))
            p->pos++;
    }
    n = p->pos - start;
    if (n < sizeof buf) {
        memcpy(buf, p->s + start, n);
        buf[n] = '\0';
        text = buf;
    } else {
        heap = xstrndup(p->s + start, n);
        text = heap;
    }
    v = json_new(p->arena, JSON_NUMBER);
    v->number = strtod(text, NULL);
    if (is_int) {
        /* exact int64 when it fits */
        const char *d = text;
        int neg = 0;
        uint64_t m = 0;
        int overflow = 0;
        if (*d == '-') {
            neg = 1;
            d++;
        }
        for (; *d; d++) {
            unsigned dig = (unsigned)(*d - '0');
            if (m > (UINT64_MAX - dig) / 10u) {
                overflow = 1;
                break;
            }
            m = m * 10u + dig;
        }
        if (!overflow) {
            if (!neg && m <= (uint64_t)INT64_MAX) {
                v->is_int = 1;
                v->integer = (int64_t)m;
            } else if (neg && m <= (uint64_t)INT64_MAX + 1u) {
                v->is_int = 1;
                v->integer = m == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)m;
            }
        }
    }
    free(heap);
    return v;
}

static JsonValue *json_parse_value(JsonParser *p, int depth)
{
    int c;
    JsonValue *v;
    if (depth > JSON_MAX_DEPTH) {
        json_fail(p, "nesting too deep");
        return NULL;
    }
    json_skip_ws(p);
    c = json_peek(p);
    switch (c) {
    case '{': {
        JsonMemberVec ms = {0};
        p->pos++;
        json_skip_ws(p);
        if (json_peek(p) == '}') {
            p->pos++;
        } else {
            for (;;) {
                JsonMember m;
                json_skip_ws(p);
                if (json_peek(p) != '"') {
                    json_fail(p, "expected string key");
                    vec_free(ms);
                    return NULL;
                }
                if (!json_parse_string(p, &m.key, &m.key_len)) {
                    vec_free(ms);
                    return NULL;
                }
                json_skip_ws(p);
                if (json_peek(p) != ':') {
                    json_fail(p, "expected ':'");
                    vec_free(ms);
                    return NULL;
                }
                p->pos++;
                m.value = json_parse_value(p, depth + 1);
                if (!m.value) {
                    vec_free(ms);
                    return NULL;
                }
                vec_push(ms, m);
                json_skip_ws(p);
                if (json_peek(p) == ',') {
                    p->pos++;
                    continue;
                }
                if (json_peek(p) == '}') {
                    p->pos++;
                    break;
                }
                json_fail(p, "expected ',' or '}'");
                vec_free(ms);
                return NULL;
            }
        }
        v = json_new(p->arena, JSON_OBJECT);
        v->n = ms.len;
        v->members = (JsonMember *)arena_memdup(p->arena, ms.data ? (const void *)ms.data : (const void *)"",
                                                ms.len * sizeof(JsonMember));
        vec_free(ms);
        return v;
    }
    case '[': {
        JsonItemVec items = {0};
        p->pos++;
        json_skip_ws(p);
        if (json_peek(p) == ']') {
            p->pos++;
        } else {
            for (;;) {
                JsonValue *item = json_parse_value(p, depth + 1);
                if (!item) {
                    vec_free(items);
                    return NULL;
                }
                vec_push(items, item);
                json_skip_ws(p);
                if (json_peek(p) == ',') {
                    p->pos++;
                    continue;
                }
                if (json_peek(p) == ']') {
                    p->pos++;
                    break;
                }
                json_fail(p, "expected ',' or ']'");
                vec_free(items);
                return NULL;
            }
        }
        v = json_new(p->arena, JSON_ARRAY);
        v->n = items.len;
        v->items = (JsonValue **)arena_memdup(p->arena, items.data ? (const void *)items.data : (const void *)"",
                                              items.len * sizeof(JsonValue *));
        vec_free(items);
        return v;
    }
    case '"':
        v = json_new(p->arena, JSON_STRING);
        if (!json_parse_string(p, &v->str, &v->len))
            return NULL;
        return v;
    case 't':
    case 'f':
    case 'n':
        if (json_literal(p, "true")) {
            v = json_new(p->arena, JSON_BOOL);
            v->boolean = 1;
            return v;
        }
        if (json_literal(p, "false"))
            return json_new(p->arena, JSON_BOOL);
        if (json_literal(p, "null"))
            return json_new(p->arena, JSON_NULL);
        json_fail(p, "unexpected token");
        return NULL;
    default:
        if (c == '-' || tern_is_digit(c))
            return json_parse_number(p);
        json_fail(p, c < 0 ? "unexpected end of input" : "unexpected character");
        return NULL;
    }
}

JsonValue *json_parse(Arena *a, const char *text, size_t len, JsonError *err)
{
    JsonParser p;
    JsonValue *v;
    p.arena = a;
    p.s = text;
    p.len = len;
    p.pos = 0;
    p.failed = 0;
    p.err = err;
    v = json_parse_value(&p, 0);
    if (!v)
        return NULL;
    json_skip_ws(&p);
    if (p.pos != p.len) {
        json_fail(&p, "trailing characters after JSON value");
        return NULL;
    }
    return v;
}

/* ---------------------------------------------------------- accessors --- */

JsonValue *json_get(const JsonValue *obj, const char *key)
{
    size_t i, kl;
    if (!obj || obj->kind != JSON_OBJECT)
        return NULL;
    kl = strlen(key);
    for (i = obj->n; i-- > 0;) {
        const JsonMember *m = &obj->members[i];
        if (m->key_len == kl && memcmp(m->key, key, kl) == 0)
            return m->value;
    }
    return NULL;
}

JsonValue *json_at(const JsonValue *arr, size_t i)
{
    if (!arr || arr->kind != JSON_ARRAY || i >= arr->n)
        return NULL;
    return arr->items[i];
}

size_t json_len(const JsonValue *v)
{
    if (!v || (v->kind != JSON_ARRAY && v->kind != JSON_OBJECT))
        return 0;
    return v->n;
}

int json_is(const JsonValue *v, JsonKind kind)
{
    return v && v->kind == kind;
}

const char *json_as_str(const JsonValue *v, const char *dflt)
{
    return v && v->kind == JSON_STRING ? v->str : dflt;
}

int64_t json_as_int(const JsonValue *v, int64_t dflt)
{
    if (!v || v->kind != JSON_NUMBER)
        return dflt;
    if (v->is_int)
        return v->integer;
    if (v->number >= -9.2e18 && v->number <= 9.2e18)
        return (int64_t)v->number;
    return dflt;
}

double json_as_num(const JsonValue *v, double dflt)
{
    return v && v->kind == JSON_NUMBER ? v->number : dflt;
}

int json_as_bool(const JsonValue *v, int dflt)
{
    return v && v->kind == JSON_BOOL ? v->boolean : dflt;
}

JsonValue *json_new(Arena *a, JsonKind kind)
{
    JsonValue *v = ANEW(a, JsonValue);
    v->kind = kind;
    return v;
}

JsonValue *json_new_str(Arena *a, const char *s, size_t len)
{
    JsonValue *v = json_new(a, JSON_STRING);
    v->str = arena_strndup(a, s, len);
    v->len = len;
    return v;
}

JsonValue *json_new_int(Arena *a, int64_t i)
{
    JsonValue *v = json_new(a, JSON_NUMBER);
    v->is_int = 1;
    v->integer = i;
    v->number = (double)i;
    return v;
}

/* ------------------------------------------------------------- writer --- */

void json_escape(StrBuf *out, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i = 0;
    sb_putc(out, '"');
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            size_t adv;
            if (utf8_decode(s + i, n - i, &adv) == UTF8_INVALID) {
                sb_puts(out, "\\ufffd");
                i += 1;
            } else {
                sb_append(out, s + i, adv);
                i += adv;
            }
            continue;
        }
        switch (c) {
        case '"': sb_puts(out, "\\\""); break;
        case '\\': sb_puts(out, "\\\\"); break;
        case '\n': sb_puts(out, "\\n"); break;
        case '\r': sb_puts(out, "\\r"); break;
        case '\t': sb_puts(out, "\\t"); break;
        case '\b': sb_puts(out, "\\b"); break;
        case '\f': sb_puts(out, "\\f"); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                sb_puts(out, "\\u00");
                sb_putc(out, hex[c >> 4]);
                sb_putc(out, hex[c & 15]);
            } else {
                sb_putc(out, (char)c);
            }
        }
        i++;
    }
    sb_putc(out, '"');
}

void jw_init(JsonWriter *w, StrBuf *out, int pretty)
{
    w->out = out;
    w->start = out->len;
    w->depth = 0;
    w->pretty = pretty;
}

/* Last significant character written by this writer, or 0. */
static int jw_last(const JsonWriter *w)
{
    size_t i = w->out->len;
    while (i > w->start) {
        char c = w->out->data[i - 1];
        if (c != ' ' && c != '\n')
            return (unsigned char)c;
        i--;
    }
    return 0;
}

static void jw_newline(JsonWriter *w)
{
    sb_putc(w->out, '\n');
    sb_indent(w->out, w->depth * 2);
}

/* Emits the separator before a value or key. */
static void jw_pre(JsonWriter *w)
{
    int last = jw_last(w);
    if (last == ':')
        return;
    if (last && last != '[' && last != '{')
        sb_putc(w->out, ',');
    if (w->pretty && w->depth > 0)
        jw_newline(w);
}

static void jw_end(JsonWriter *w, int open, int close)
{
    int last = jw_last(w);
    w->depth--;
    if (w->pretty && last != open)
        jw_newline(w);
    sb_putc(w->out, close);
}

void jw_object_begin(JsonWriter *w)
{
    jw_pre(w);
    sb_putc(w->out, '{');
    w->depth++;
}

void jw_object_end(JsonWriter *w) { jw_end(w, '{', '}'); }

void jw_array_begin(JsonWriter *w)
{
    jw_pre(w);
    sb_putc(w->out, '[');
    w->depth++;
}

void jw_array_end(JsonWriter *w) { jw_end(w, '[', ']'); }

void jw_key(JsonWriter *w, const char *key)
{
    jw_pre(w);
    json_escape(w->out, key, strlen(key));
    sb_puts(w->out, w->pretty ? ": " : ":");
}

void jw_strn(JsonWriter *w, const char *s, size_t n)
{
    jw_pre(w);
    json_escape(w->out, s, n);
}

void jw_str(JsonWriter *w, const char *s)
{
    if (!s) {
        jw_null(w);
        return;
    }
    jw_strn(w, s, strlen(s));
}

void jw_int(JsonWriter *w, int64_t v)
{
    char buf[32];
    if (v < 0)
        snprintf(buf, sizeof buf, "-%llu", (unsigned long long)((uint64_t)(-(v + 1)) + 1u));
    else
        snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    jw_raw(w, buf);
}

void jw_uint(JsonWriter *w, uint64_t v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    jw_raw(w, buf);
}

void jw_num(JsonWriter *w, double v)
{
    char buf[40];
    if (v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
        jw_null(w);
        return;
    }
    snprintf(buf, sizeof buf, "%.15g", v);
    if (strtod(buf, NULL) != v)
        snprintf(buf, sizeof buf, "%.17g", v);
    jw_raw(w, buf);
}

void jw_bool(JsonWriter *w, int v) { jw_raw(w, v ? "true" : "false"); }
void jw_null(JsonWriter *w) { jw_raw(w, "null"); }

void jw_raw(JsonWriter *w, const char *json)
{
    jw_pre(w);
    sb_puts(w->out, json);
}

void jw_value(JsonWriter *w, const JsonValue *v)
{
    size_t i;
    if (!v) {
        jw_null(w);
        return;
    }
    switch (v->kind) {
    case JSON_NULL: jw_null(w); break;
    case JSON_BOOL: jw_bool(w, v->boolean); break;
    case JSON_NUMBER:
        if (v->is_int)
            jw_int(w, v->integer);
        else
            jw_num(w, v->number);
        break;
    case JSON_STRING: jw_strn(w, v->str, v->len); break;
    case JSON_ARRAY:
        jw_array_begin(w);
        for (i = 0; i < v->n; i++)
            jw_value(w, v->items[i]);
        jw_array_end(w);
        break;
    case JSON_OBJECT:
        jw_object_begin(w);
        for (i = 0; i < v->n; i++) {
            jw_pre(w);
            json_escape(w->out, v->members[i].key, v->members[i].key_len);
            sb_puts(w->out, w->pretty ? ": " : ":");
            jw_value(w, v->members[i].value);
        }
        jw_object_end(w);
        break;
    }
}

void jw_kv_str(JsonWriter *w, const char *key, const char *s)
{
    jw_key(w, key);
    jw_str(w, s);
}

void jw_kv_int(JsonWriter *w, const char *key, int64_t v)
{
    jw_key(w, key);
    jw_int(w, v);
}

void jw_kv_uint(JsonWriter *w, const char *key, uint64_t v)
{
    jw_key(w, key);
    jw_uint(w, v);
}

void jw_kv_bool(JsonWriter *w, const char *key, int v)
{
    jw_key(w, key);
    jw_bool(w, v);
}

void jw_kv_num(JsonWriter *w, const char *key, double v)
{
    jw_key(w, key);
    jw_num(w, v);
}

void json_to_sb(StrBuf *out, const JsonValue *v, int pretty)
{
    JsonWriter w;
    jw_init(&w, out, pretty);
    jw_value(&w, v);
}
