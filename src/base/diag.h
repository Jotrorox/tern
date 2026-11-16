/* diag.h — source registry, spans and diagnostics.
 *
 * Sources: every file the compiler reads is registered once with src_add()
 * and referred to by its id (ids start at 1; id 0 means "no source", so a
 * zero-initialized Span is "no location"). The text is copied: lex from
 * src_get(id)->text so spans and the registry agree.
 *
 * Diagnostics:
 *     diag_error(span, "width mismatch: expected %s, found %s", a, b);
 *     diag_note(other_span, "`x` is declared here");   // attaches to the error
 *     diag_help("truncate explicitly with `x.trunc<8>()`");
 * The error is emitted (to stderr, or the sink) when the next diagnostic
 * starts or on diag_flush(); call diag_flush() before exiting.
 *
 * Elaboration path: push a frame per entered instance; every diagnostic
 * records the path at creation, rendered as `  in: Top > rx_fifo: Fifo<u8, 64>`.
 *     diag_push_path("Top"); diag_push_path("rx_fifo: Fifo<u8, 64>"); ... diag_pop_path();
 */
#ifndef TERN_DIAG_H
#define TERN_DIAG_H

#include "common.h"

struct StrBuf;
struct JsonWriter;

/* ------------------------------------------------------------- sources -- */

/* line/col are 1-based; col counts Unicode code points from the line start
 * (a tab counts as one). off/len are byte offsets into the source text. */
typedef struct Span {
    uint32_t file, off, len, line, col;
} Span;

typedef struct SourceFile {
    uint32_t id;
    const char *path;     /* as given to src_add (used verbatim in messages) */
    const char *text;     /* NUL-terminated copy */
    size_t len;
    uint32_t *lines;      /* byte offset of the start of each line */
    uint32_t nlines;
} SourceFile;

uint32_t src_add(const char *path, const char *text, size_t len);
void src_update(uint32_t id, const char *text, size_t len);   /* replace contents (LSP edits) */
const SourceFile *src_get(uint32_t id);                       /* NULL for 0 / unknown ids */
uint32_t src_find(const char *path);                          /* id of a path, or 0 */
uint32_t src_count(void);                                     /* ids are 1..src_count() */
void src_free_all(void);

/* Line text (without the line terminator) of 1-based `line`; NULL if out of range. */
const char *src_line(const SourceFile *f, uint32_t line, size_t *len);
/* 1-based line and code-point column of byte offset `off` in source `id`. */
void src_linecol(uint32_t id, uint32_t off, uint32_t *line, uint32_t *col);

Span span_make(uint32_t file, uint32_t off, uint32_t len);    /* computes line/col */
Span span_join(Span a, Span b);                               /* smallest span covering both */
Span span_none(void);                                         /* no location */

/* --------------------------------------------------------- diagnostics -- */

typedef enum DiagLevel { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE, DIAG_HELP } DiagLevel;

typedef struct DiagSub {      /* a note or help line attached to a diagnostic */
    DiagLevel level;          /* DIAG_NOTE or DIAG_HELP */
    Span span;                /* file == 0: no location */
    char *msg;
} DiagSub;

typedef struct Diagnostic {
    DiagLevel level;          /* ERROR, WARNING (or NOTE when standalone) */
    Span span;
    char *msg;
    char *path;               /* elaboration path "Top > rx_fifo: Fifo<u8, 64>", or NULL */
    int promoted;             /* a warning turned into an error by --strict */
    uint32_t nsubs;
    DiagSub *subs;
} Diagnostic;

typedef enum DiagFormat {
    DIAG_FMT_HUMAN,           /* rustc-style with source excerpt (default) */
    DIAG_FMT_SHORT,           /* `file:line:col: error: message`, one line each */
    DIAG_FMT_JSON             /* one JSON object per line */
} DiagFormat;

/* Receives each finished diagnostic instead of it being printed; `d` is only
 * valid during the call. */
typedef void (*DiagSink)(const Diagnostic *d, void *user);

void diag_error(Span span, const char *fmt, ...) TERN_PRINTF(2, 3);
void diag_warning(Span span, const char *fmt, ...) TERN_PRINTF(2, 3);
void diag_note(Span span, const char *fmt, ...) TERN_PRINTF(2, 3);
void diag_help(const char *fmt, ...) TERN_PRINTF(1, 2);
void diag_verror(Span span, const char *fmt, va_list ap);
void diag_vwarning(Span span, const char *fmt, va_list ap);

void diag_push_path(const char *frame);   /* frame is copied */
void diag_pop_path(void);
size_t diag_path_depth(void);
void diag_path_truncate(size_t depth);    /* pop back to a saved depth */

void diag_flush(void);                    /* emit the pending diagnostic, if any */

void diag_set_sink(DiagSink sink, void *user);  /* NULL: print (default) */
void diag_get_sink(DiagSink *sink, void **user);  /* current sink (to restore it) */
void diag_set_format(DiagFormat fmt);
DiagFormat diag_get_format(void);
int diag_parse_format(const char *name, DiagFormat *out);  /* "human"/"short"/"json" */
void diag_set_color(int on);              /* ANSI colour in human output (default off) */
void diag_set_strict(int on);             /* warnings become errors */
void diag_set_output(FILE *f);            /* default (NULL) is stderr */

int diag_error_count(void);
int diag_warning_count(void);
void diag_reset(void);                    /* drop pending diagnostic, counts and path */

const char *diag_level_name(DiagLevel level);   /* "error", "warning", "note", "help" */
void diag_render(const Diagnostic *d, DiagFormat fmt, int color, struct StrBuf *out);
void diag_render_json(const Diagnostic *d, struct JsonWriter *w);   /* one object */

#endif /* TERN_DIAG_H */
