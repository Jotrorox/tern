/* lsp_impl.h — private interface between the LSP server (lsp.c: protocol,
 * documents, analyses, diagnostics, formatting) and its navigation features
 * (lsp_nav.c: definition, hover, symbols, completion, references, instance
 * tree). Not used outside src/lsp/. */
#ifndef TERN_LSP_IMPL_H
#define TERN_LSP_IMPL_H

#include "lsp.h"
#include "../base/arena.h"
#include "../base/diag.h"
#include "../base/map.h"
#include "../base/vec.h"
#include "../driver.h"

typedef VEC(char *) LspStrVec;

typedef struct LspNote {
    Span span;
    char *msg;
} LspNote;

/* A captured diagnostic (help lines and the elaboration path are folded
 * into msg; notes with a location become related information). */
typedef struct LspDiag {
    DiagLevel level;
    Span span;
    char *msg;
    LspNote *notes;
    uint32_t nnotes;
} LspDiag;

typedef VEC(LspDiag) LspDiagVec;

typedef struct LspFile {
    FileUnit *fu;
    uint32_t src;
    char *path;               /* absolute, or NULL (embedded std without a copy on disk) */
} LspFile;

/* One analysis: a project (root, file == NULL) or a single file (root =
 * its project or NULL). */
typedef struct LspGroup {
    char *root;
    char *file;
    int dirty;
    int analyzed;
    Driver *dr;               /* NULL when the inputs could not be opened */
    Arena ir;
    IrDesign *design;         /* NULL when there were syntax errors */
    LspDiagVec diags;
    VEC(LspFile) files;
    LspStrVec published;      /* paths that got diagnostics last time */
} LspGroup;

typedef struct LspDoc {
    char *uri;
    char *path;               /* absolute, normalized */
    char *text;
    size_t len;
    int64_t version;
    LspGroup *group;
} LspDoc;

typedef struct LspServer {
    const LspIo *io;
    LspOptions opt;
    RpcReader rd;
    int initialized;
    int shutdown;
    int exit_requested;
    char *root;               /* workspace root (initialize), or NULL */
    VEC(LspDoc *) docs;
    VEC(LspGroup *) groups;
    Map srcpath;              /* source id -> char* absolute path ("" when virtual) */
    LspDiagVec *capture;      /* diagnostics sink target during an analysis */
} LspServer;

/* ------------------------------------------------------------- lsp.c --- */

void lsp_log(LspServer *srv, const char *fmt, ...) TERN_PRINTF(2, 3);
/* Absolute path of a source, or NULL when it has no file on disk / buffer. */
const char *lsp_src_path(LspServer *srv, uint32_t src);
/* Byte offset of an LSP position (UTF-16 units) in text. */
uint32_t lsp_offset_of(const char *text, size_t len, int64_t line, int64_t character);
/* LSP position of byte offset `off` in source `src` (clamped). */
void lsp_pos_of(uint32_t src, uint32_t off, uint32_t *line, uint32_t *character);
void lsp_w_range(JsonWriter *w, Span s);
/* {"uri":..,"range":..}; returns 0 (writes nothing) when the span has no file. */
int lsp_w_location(LspServer *srv, JsonWriter *w, Span s);
int lsp_span_valid(Span s);
LspFile *lsp_group_file(LspGroup *g, const char *path);
LspFile *lsp_group_file_by_src(LspGroup *g, uint32_t src);
LspDoc *lsp_find_doc(LspServer *srv, const char *path);
/* Source text of a span ("" when unknown). */
const char *lsp_span_text(Span s, size_t *len);

/* --------------------------------------------------------- lsp_nav.c --- */

/* A declaration found by walking the AST. */
typedef struct LspDecl {
    Ident name;
    AstDoc *doc;
    SemKind kind;
    Span decl;                /* whole declaration */
    int parent;               /* index of the containing declaration, or -1 */
    int local;                /* fn/test body local, loop or match binding */
    Item *item;               /* the declaration is this item */
    Item *top;                /* enclosing top-level item */
    Type *type;               /* declared type, or NULL */
    Expr *init;               /* let/var/reg initializer, or NULL */
    PortDir dir;              /* ports */
} LspDecl;

typedef VEC(LspDecl) LspDeclVec;

void lsp_collect_decls(const AstFile *f, LspDeclVec *out);

/* Request handlers: write the JSON result value into `out`. `doc` is
 * analysed and `lf` is its file in the analysis (may be NULL). */
void lsp_nav_definition(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out);
void lsp_nav_hover(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out);
void lsp_nav_references(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, int include_decl,
                        int highlight, StrBuf *out);
void lsp_nav_document_symbols(LspServer *srv, LspDoc *doc, LspFile *lf, StrBuf *out);
void lsp_nav_completion(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out);
void lsp_nav_workspace_symbols(LspServer *srv, const char *query, StrBuf *out);
void lsp_nav_instance_tree(LspServer *srv, LspGroup *only, StrBuf *out);

#endif /* TERN_LSP_IMPL_H */
