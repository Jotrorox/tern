/* diag.c — source registry and diagnostic rendering. */
#include "diag.h"
#include "str.h"
#include "vec.h"
#include "utf8.h"
#include "json.h"

/* ------------------------------------------------------------- sources -- */

typedef VEC(SourceFile *) DiagSourceVec;
static DiagSourceVec diag_sources;

static void diag_build_lines(SourceFile *f)
{
    size_t i;
    uint32_t n = 1;
    for (i = 0; i < f->len; i++) {
        if (f->text[i] == '\n')
            n++;
    }
    f->lines = (uint32_t *)xmalloc((size_t)n * sizeof(uint32_t));
    f->nlines = n;
    f->lines[0] = 0;
    n = 1;
    for (i = 0; i < f->len; i++) {
        if (f->text[i] == '\n')
            f->lines[n++] = (uint32_t)(i + 1);
    }
}

uint32_t src_add(const char *path, const char *text, size_t len)
{
    SourceFile *f = (SourceFile *)xcalloc(1, sizeof(SourceFile));
    if (len > 0xFFFFFFF0u)
        tern_fatal("source file too large: %s", path);
    f->path = xstrdup(path);
    f->text = xstrndup(text, len);
    f->len = len;
    diag_build_lines(f);
    vec_push(diag_sources, f);
    f->id = (uint32_t)diag_sources.len;
    return f->id;
}

void src_update(uint32_t id, const char *text, size_t len)
{
    SourceFile *f;
    if (id == 0 || id > diag_sources.len)
        return;
    f = diag_sources.data[id - 1];
    free((void *)f->text);
    free(f->lines);
    f->text = xstrndup(text, len);
    f->len = len;
    diag_build_lines(f);
}

const SourceFile *src_get(uint32_t id)
{
    if (id == 0 || id > diag_sources.len)
        return NULL;
    return diag_sources.data[id - 1];
}

uint32_t src_find(const char *path)
{
    size_t i;
    for (i = 0; i < diag_sources.len; i++) {
        if (strcmp(diag_sources.data[i]->path, path) == 0)
            return (uint32_t)(i + 1);
    }
    return 0;
}

uint32_t src_count(void)
{
    return (uint32_t)diag_sources.len;
}

void src_free_all(void)
{
    size_t i;
    for (i = 0; i < diag_sources.len; i++) {
        SourceFile *f = diag_sources.data[i];
        free((void *)f->path);
        free((void *)f->text);
        free(f->lines);
        free(f);
    }
    vec_free(diag_sources);
}

const char *src_line(const SourceFile *f, uint32_t line, size_t *len)
{
    size_t start, end;
    if (!f || line == 0 || line > f->nlines)
        return NULL;
    start = f->lines[line - 1];
    end = line < f->nlines ? f->lines[line] - 1 : f->len; /* excludes '\n' */
    if (end > start && f->text[end - 1] == '\r')
        end--;
    if (len)
        *len = end - start;
    return f->text + start;
}

/* 1-based line containing byte offset off. */
static uint32_t diag_line_of(const SourceFile *f, uint32_t off)
{
    uint32_t lo = 0, hi = f->nlines; /* lines[lo] <= off < lines[hi] */
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->lines[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    return lo + 1;
}

void src_linecol(uint32_t id, uint32_t off, uint32_t *line, uint32_t *col)
{
    const SourceFile *f = src_get(id);
    uint32_t l;
    if (!f) {
        *line = 0;
        *col = 0;
        return;
    }
    if (off > f->len)
        off = (uint32_t)f->len;
    l = diag_line_of(f, off);
    *line = l;
    *col = (uint32_t)utf8_count(f->text + f->lines[l - 1], off - f->lines[l - 1]) + 1;
}

Span span_make(uint32_t file, uint32_t off, uint32_t len)
{
    Span s;
    s.file = file;
    s.off = off;
    s.len = len;
    src_linecol(file, off, &s.line, &s.col);
    return s;
}

Span span_join(Span a, Span b)
{
    uint32_t start, end;
    if (a.file == 0)
        return b;
    if (b.file == 0 || b.file != a.file)
        return a;
    start = a.off < b.off ? a.off : b.off;
    end = a.off + a.len > b.off + b.len ? a.off + a.len : b.off + b.len;
    if (start == a.off) {
        a.len = end - start;
        return a;
    }
    b.len = end - start;
    return b;
}

Span span_none(void)
{
    Span s;
    memset(&s, 0, sizeof s);
    return s;
}

/* --------------------------------------------------------- diagnostics -- */

typedef VEC(char *) DiagPathVec;
typedef VEC(DiagSub) DiagSubVec;

static struct {
    int has_pending;
    Diagnostic cur;
    DiagSubVec subs;
    DiagPathVec path;
    DiagSink sink;
    void *sink_user;
    DiagFormat format;
    int color;
    int strict;
    FILE *out;
    int errors;
    int warnings;
} diag_st;

const char *diag_level_name(DiagLevel level)
{
    switch (level) {
    case DIAG_ERROR: return "error";
    case DIAG_WARNING: return "warning";
    case DIAG_NOTE: return "note";
    case DIAG_HELP: return "help";
    }
    return "error";
}

static char *diag_vfmt(const char *fmt, va_list ap)
{
    StrBuf sb = {0};
    sb_vprintf(&sb, fmt, ap);
    return sb_take(&sb);
}

static char *diag_path_string(void)
{
    StrBuf sb = {0};
    size_t i;
    if (!diag_st.path.len)
        return NULL;
    for (i = 0; i < diag_st.path.len; i++) {
        if (i)
            sb_puts(&sb, " > ");
        sb_puts(&sb, diag_st.path.data[i]);
    }
    return sb_take(&sb);
}

static void diag_free_cur(void)
{
    size_t i;
    free(diag_st.cur.msg);
    free(diag_st.cur.path);
    for (i = 0; i < diag_st.subs.len; i++)
        free(diag_st.subs.data[i].msg);
    vec_clear(diag_st.subs);
    memset(&diag_st.cur, 0, sizeof diag_st.cur);
    diag_st.has_pending = 0;
}

void diag_flush(void)
{
    if (!diag_st.has_pending)
        return;
    diag_st.cur.nsubs = (uint32_t)diag_st.subs.len;
    diag_st.cur.subs = diag_st.subs.data;
    if (diag_st.sink) {
        diag_st.sink(&diag_st.cur, diag_st.sink_user);
    } else {
        StrBuf sb = {0};
        FILE *out = diag_st.out ? diag_st.out : stderr;
        diag_render(&diag_st.cur, diag_st.format, diag_st.color, &sb);
        fflush(stdout);
        if (sb.len)
            fwrite(sb.data, 1, sb.len, out);
        fflush(out);
        sb_free(&sb);
    }
    diag_free_cur();
}

static void diag_begin(DiagLevel level, Span span, const char *fmt, va_list ap)
{
    diag_flush();
    diag_st.has_pending = 1;
    diag_st.cur.level = level;
    diag_st.cur.span = span;
    diag_st.cur.msg = diag_vfmt(fmt, ap);
    diag_st.cur.path = diag_path_string();
    diag_st.cur.promoted = 0;
}

static void diag_add_sub(DiagLevel level, Span span, char *msg)
{
    DiagSub s;
    s.level = level;
    s.span = span;
    s.msg = msg;
    vec_push(diag_st.subs, s);
}

void diag_verror(Span span, const char *fmt, va_list ap)
{
    diag_begin(DIAG_ERROR, span, fmt, ap);
    diag_st.errors++;
}

void diag_vwarning(Span span, const char *fmt, va_list ap)
{
    if (diag_st.strict) {
        diag_begin(DIAG_ERROR, span, fmt, ap);
        diag_st.cur.promoted = 1;
        diag_st.errors++;
        diag_add_sub(DIAG_NOTE, span_none(), xstrdup("warning treated as an error because of --strict"));
        return;
    }
    diag_begin(DIAG_WARNING, span, fmt, ap);
    diag_st.warnings++;
}

void diag_error(Span span, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_verror(span, fmt, ap);
    va_end(ap);
}

void diag_warning(Span span, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vwarning(span, fmt, ap);
    va_end(ap);
}

void diag_note(Span span, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (!diag_st.has_pending) {
        /* nothing to attach to: emit a standalone note */
        diag_begin(DIAG_NOTE, span, fmt, ap);
    } else {
        diag_add_sub(DIAG_NOTE, span, diag_vfmt(fmt, ap));
    }
    va_end(ap);
}

void diag_help(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (!diag_st.has_pending)
        diag_begin(DIAG_HELP, span_none(), fmt, ap);
    else
        diag_add_sub(DIAG_HELP, span_none(), diag_vfmt(fmt, ap));
    va_end(ap);
}

void diag_push_path(const char *frame)
{
    vec_push(diag_st.path, xstrdup(frame));
}

void diag_pop_path(void)
{
    if (diag_st.path.len)
        free(vec_pop(diag_st.path));
}

size_t diag_path_depth(void)
{
    return diag_st.path.len;
}

void diag_path_truncate(size_t depth)
{
    while (diag_st.path.len > depth)
        diag_pop_path();
}

void diag_set_sink(DiagSink sink, void *user)
{
    diag_flush();
    diag_st.sink = sink;
    diag_st.sink_user = user;
}

void diag_set_format(DiagFormat fmt) { diag_st.format = fmt; }
DiagFormat diag_get_format(void) { return diag_st.format; }
void diag_set_color(int on) { diag_st.color = on; }
void diag_set_strict(int on) { diag_st.strict = on; }
void diag_set_output(FILE *f) { diag_st.out = f; }
int diag_error_count(void) { return diag_st.errors; }
int diag_warning_count(void) { return diag_st.warnings; }

int diag_parse_format(const char *name, DiagFormat *out)
{
    if (strcmp(name, "human") == 0)
        *out = DIAG_FMT_HUMAN;
    else if (strcmp(name, "short") == 0)
        *out = DIAG_FMT_SHORT;
    else if (strcmp(name, "json") == 0)
        *out = DIAG_FMT_JSON;
    else
        return 0;
    return 1;
}

void diag_reset(void)
{
    diag_free_cur();
    vec_free(diag_st.subs);
    diag_path_truncate(0);
    vec_free(diag_st.path);
    diag_st.errors = 0;
    diag_st.warnings = 0;
}

/* ----------------------------------------------------------- rendering -- */

#define DIAG_C_RESET "\x1b[0m"
#define DIAG_C_BOLD "\x1b[1m"
#define DIAG_C_RED "\x1b[1;31m"
#define DIAG_C_YELLOW "\x1b[1;33m"
#define DIAG_C_GREEN "\x1b[1;32m"
#define DIAG_C_CYAN "\x1b[1;36m"
#define DIAG_C_BLUE "\x1b[1;34m"

static const char *diag_level_color(DiagLevel level)
{
    switch (level) {
    case DIAG_ERROR: return DIAG_C_RED;
    case DIAG_WARNING: return DIAG_C_YELLOW;
    case DIAG_NOTE: return DIAG_C_GREEN;
    case DIAG_HELP: return DIAG_C_CYAN;
    }
    return DIAG_C_BOLD;
}

static void diag_col(StrBuf *out, int color, const char *code)
{
    if (color)
        sb_puts(out, code);
}

/* Resolved location of a span: prefers the offset when the source is known. */
typedef struct DiagLoc {
    const SourceFile *f;
    uint32_t line, col;
} DiagLoc;

static DiagLoc diag_resolve(Span s)
{
    DiagLoc l;
    l.f = src_get(s.file);
    l.line = s.line;
    l.col = s.col;
    if (l.f && s.off <= l.f->len)
        src_linecol(s.file, s.off, &l.line, &l.col);
    return l;
}

static void diag_put_location(StrBuf *out, Span s)
{
    DiagLoc l = diag_resolve(s);
    if (!l.f)
        return;
    sb_printf(out, "%s:%u:%u: ", l.f->path, (unsigned)l.line, (unsigned)l.col);
}

static int diag_digits(uint32_t n)
{
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

/* Source excerpt with caret underline. `gutter` is the width of the line
 * number column. */
static void diag_put_excerpt(StrBuf *out, Span s, DiagLevel level, int color, int gutter)
{
    DiagLoc l = diag_resolve(s);
    const char *text;
    size_t tlen, i, start_byte, end_byte;
    uint32_t caret_from = 0, caret_to = 0, colno = 0;
    if (!l.f || l.line == 0)
        return;
    text = src_line(l.f, l.line, &tlen);
    if (!text)
        return;
    start_byte = s.off >= l.f->lines[l.line - 1] ? s.off - l.f->lines[l.line - 1] : 0;
    if (start_byte > tlen)
        start_byte = tlen;
    end_byte = start_byte + s.len;
    if (end_byte > tlen)
        end_byte = tlen;

    diag_col(out, color, DIAG_C_BLUE);
    sb_indent(out, gutter + 1);
    sb_puts(out, "|\n");
    sb_printf(out, "%*u | ", gutter, (unsigned)l.line);
    diag_col(out, color, DIAG_C_RESET);
    /* line text: tabs expanded to 4 columns; compute caret columns */
    i = 0;
    while (i <= tlen) {
        size_t adv = 1;
        if (i == start_byte)
            caret_from = colno;
        if (i == end_byte)
            caret_to = colno;
        if (i == tlen)
            break;
        if (text[i] == '\t') {
            sb_puts(out, "    ");
            colno += 4;
        } else if ((unsigned char)text[i] < 0x80) {
            sb_putc(out, text[i]);
            colno++;
        } else {
            utf8_decode(text + i, tlen - i, &adv);
            sb_append(out, text + i, adv);
            colno++;
        }
        i += adv;
    }
    sb_putc(out, '\n');
    if (caret_to <= caret_from)
        caret_to = caret_from + 1;
    diag_col(out, color, DIAG_C_BLUE);
    sb_indent(out, gutter + 1);
    sb_puts(out, "| ");
    diag_col(out, color, DIAG_C_RESET);
    sb_indent(out, (int)caret_from);
    diag_col(out, color, diag_level_color(level));
    for (i = caret_from; i < caret_to; i++)
        sb_putc(out, '^');
    diag_col(out, color, DIAG_C_RESET);
    sb_putc(out, '\n');
}

static void diag_put_header(StrBuf *out, DiagLevel level, Span s, const char *msg, int color)
{
    diag_col(out, color, DIAG_C_BOLD);
    diag_put_location(out, s);
    diag_col(out, color, DIAG_C_RESET);
    diag_col(out, color, diag_level_color(level));
    sb_puts(out, diag_level_name(level));
    diag_col(out, color, DIAG_C_RESET);
    diag_col(out, color, DIAG_C_BOLD);
    sb_puts(out, ": ");
    sb_puts(out, msg);
    diag_col(out, color, DIAG_C_RESET);
    sb_putc(out, '\n');
}

static void diag_render_human(const Diagnostic *d, int color, StrBuf *out)
{
    int gutter = 1;
    uint32_t i;
    DiagLoc l = diag_resolve(d->span);
    if (l.f)
        gutter = diag_digits(l.line);
    for (i = 0; i < d->nsubs; i++) {
        DiagLoc sl = diag_resolve(d->subs[i].span);
        if (sl.f && diag_digits(sl.line) > gutter)
            gutter = diag_digits(sl.line);
    }
    diag_put_header(out, d->level, d->span, d->msg, color);
    diag_put_excerpt(out, d->span, d->level, color, gutter);
    /* help/notes without a location first, as `= help:` lines */
    for (i = 0; i < d->nsubs; i++) {
        const DiagSub *s = &d->subs[i];
        if (src_get(s->span.file))
            continue;
        sb_indent(out, gutter + 1);
        diag_col(out, color, DIAG_C_BLUE);
        sb_puts(out, "= ");
        diag_col(out, color, DIAG_C_RESET);
        diag_col(out, color, DIAG_C_BOLD);
        sb_puts(out, diag_level_name(s->level));
        sb_puts(out, ":");
        diag_col(out, color, DIAG_C_RESET);
        sb_putc(out, ' ');
        sb_puts(out, s->msg);
        sb_putc(out, '\n');
    }
    if (d->path) {
        sb_puts(out, "  ");
        diag_col(out, color, DIAG_C_BOLD);
        sb_puts(out, "in:");
        diag_col(out, color, DIAG_C_RESET);
        sb_putc(out, ' ');
        sb_puts(out, d->path);
        sb_putc(out, '\n');
    }
    /* notes with their own location: a sub-block each */
    for (i = 0; i < d->nsubs; i++) {
        const DiagSub *s = &d->subs[i];
        if (!src_get(s->span.file))
            continue;
        diag_put_header(out, s->level, s->span, s->msg, color);
        diag_put_excerpt(out, s->span, s->level, color, gutter);
    }
    sb_putc(out, '\n'); /* blank line between diagnostics */
}

static void diag_render_short(const Diagnostic *d, StrBuf *out)
{
    diag_put_location(out, d->span);
    sb_puts(out, diag_level_name(d->level));
    sb_puts(out, ": ");
    sb_puts(out, d->msg);
    sb_putc(out, '\n');
}

static void diag_json_loc(JsonWriter *w, Span s)
{
    DiagLoc l = diag_resolve(s);
    if (!l.f) {
        jw_key(w, "file");
        jw_null(w);
        return;
    }
    jw_kv_str(w, "file", l.f->path);
    jw_kv_uint(w, "line", l.line);
    jw_kv_uint(w, "col", l.col);
    jw_kv_uint(w, "offset", s.off);
    jw_kv_uint(w, "length", s.len);
    if (s.off + s.len <= l.f->len) {
        uint32_t el, ec;
        src_linecol(s.file, s.off + s.len, &el, &ec);
        jw_kv_uint(w, "end_line", el);
        jw_kv_uint(w, "end_col", ec);
    }
}

void diag_render_json(const Diagnostic *d, JsonWriter *w)
{
    uint32_t i;
    jw_object_begin(w);
    jw_kv_str(w, "severity", diag_level_name(d->level));
    jw_kv_str(w, "message", d->msg);
    diag_json_loc(w, d->span);
    jw_kv_str(w, "path", d->path);
    if (d->promoted)
        jw_kv_bool(w, "promoted", 1);
    jw_key(w, "notes");
    jw_array_begin(w);
    for (i = 0; i < d->nsubs; i++) {
        jw_object_begin(w);
        jw_kv_str(w, "severity", diag_level_name(d->subs[i].level));
        jw_kv_str(w, "message", d->subs[i].msg);
        diag_json_loc(w, d->subs[i].span);
        jw_object_end(w);
    }
    jw_array_end(w);
    jw_object_end(w);
}

void diag_render(const Diagnostic *d, DiagFormat fmt, int color, StrBuf *out)
{
    switch (fmt) {
    case DIAG_FMT_SHORT:
        diag_render_short(d, out);
        break;
    case DIAG_FMT_JSON: {
        JsonWriter w;
        jw_init(&w, out, 0);
        diag_render_json(d, &w);
        sb_putc(out, '\n');
        break;
    }
    case DIAG_FMT_HUMAN:
    default:
        diag_render_human(d, color, out);
        break;
    }
}

#undef DIAG_C_RESET
#undef DIAG_C_BOLD
#undef DIAG_C_RED
#undef DIAG_C_YELLOW
#undef DIAG_C_GREEN
#undef DIAG_C_CYAN
#undef DIAG_C_BLUE
