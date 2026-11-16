/* tools.h — library entry points of the source tools in src/tools/ that other
 * commands and the tests use directly (fmt.c, doc.c).
 *
 * Formatter (fmt.c, canonical style in docs/style.md):
 *
 *     AstFile *f = parse_file(&arena, src_id);   // only files without syntax errors
 *     StrBuf out = {0};
 *     fmt_format(f, &out);                       // the canonical text
 *     if (!fmt_verify(f, out.data, out.len, &why)) ...   // same AST, same comments?
 *     fmt_check_naming(f);                       // naming-convention warnings
 *
 * Documentation (doc.c): `tern doc` renders packages to static HTML or to
 * terminal text; doc_render_text() is the offline reference used by
 * `tern doc std::fifo`, doc_write_html() the site generator. Both work from
 * the AST only.
 */
#ifndef TERN_TOOLS_H
#define TERN_TOOLS_H

#include "../syntax/ast.h"

struct StrBuf;

/* ------------------------------------------------------------- formatter */

/* Appends the canonical formatting of `f` to `out`. `f` must have no syntax
 * errors (f->nerrors == 0); its source must still be registered. */
void fmt_format(const AstFile *f, struct StrBuf *out);

/* Re-parses `text` (the formatted form of `f`) and checks that it has no
 * syntax errors, that its AST equals that of `f` ignoring positions, and that
 * every comment is kept with the same text in the same order. Returns 1 when
 * all of that holds; otherwise 0 and a one-line reason in `why` (may be
 * NULL). Diagnostics of the re-parse are suppressed. */
int fmt_verify(const AstFile *f, const char *text, size_t len, struct StrBuf *why);

/* The AST dump of `f` without `@line:col` positions (used by fmt_verify). */
void fmt_dump_no_pos(const AstFile *f, struct StrBuf *out);

/* Naming conventions (spec §3.2): modules, types, structs, enums, bundles,
 * regmaps, type aliases and enum variants UpperCamel; ports, signals, fields,
 * functions, parameters, domains, properties snake_case; consts UpperCamel or
 * SCREAMING_CASE. Extern modules (SystemVerilog names) are exempt. Reports a
 * diag_warning with a diag_help suggesting the converted name for each
 * violation; returns their number. */
int fmt_check_naming(const AstFile *f);

/* ---------------------------------------------------------- doc generator */

/* Text rendering width for the terminal (80 when unknown). */
#define DOC_TEXT_WIDTH 80

typedef struct DocSet DocSet;

DocSet *doc_set_new(void);
void doc_set_free(DocSet *ds);
/* Adds a source file (copied and registered with src_add). `is_std` marks
 * standard-library files (only their pub items are documented);
 * `fallback_pkg` names the package of a file without a `package` line.
 * Files with syntax errors are reported and skipped: returns -1. */
int doc_set_add_source(DocSet *ds, const char *path, const char *text, size_t len, int is_std,
                       const char *fallback_pkg);
/* Adds the standard library: the directory $TERN_STD when set, else the
 * sources embedded in the binary (std_embed.h). */
int doc_set_add_std(DocSet *ds);
/* Groups items by package, resolves re-exports and attributes tests to the
 * items they simulate (called by the renderers when needed). */
void doc_set_finish(DocSet *ds);
/* Terminal rendering of `what`: a package (`std::fifo`), an item
 * (`std::fifo::Fifo`), `std` (its packages) or NULL (all packages),
 * wrapped to `width` columns. Returns 0, or -1 when nothing matches (the
 * message, with a suggestion, is appended to `out`). */
int doc_render_text(DocSet *ds, const char *what, int width, struct StrBuf *out);
/* Writes the static HTML site (index.html, one page per package, style.css,
 * search.js, search-index.js) into `outdir`. Returns 0, or -1 with a
 * message in `err`. */
int doc_write_html(DocSet *ds, const char *outdir, struct StrBuf *err);

#endif /* TERN_TOOLS_H */
