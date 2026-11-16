/* parser.h — Tern parser: hand-written recursive descent with Pratt
 * expressions (concrete grammar: docs/grammar.md; AST: ast.h).
 *
 *     uint32_t id = src_add(path, text, len);
 *     Arena a = {0};
 *     AstFile *f = parse_file(&a, id);     // never NULL
 *     if (f->nerrors) ...                  // errors were reported through diag.h
 *     ast_dump(f, &sb);                    // S-expression dump (golden tests)
 *
 * The parser never stops at the first error: it reports, resynchronizes at
 * the next statement (newline / `;` / `}`) or item, and leaves *_ERROR nodes
 * in the tree. All nodes live in the arena.
 */
#ifndef TERN_PARSER_H
#define TERN_PARSER_H

#include "ast.h"

struct StrBuf;

/* Lexes and parses source `src_id` (registered with src_add). */
AstFile *parse_file(Arena *a, uint32_t src_id);

/* Parses one expression from `text` (registered as a source named `name`,
 * "<expr>" when NULL). Trailing tokens are an error. Never NULL. Used by tests
 * and the LSP. If `src_out` is non-NULL it receives the source id. */
Expr *parse_expr_string(Arena *a, const char *text, size_t len, const char *name, uint32_t *src_out);

/* Parses one type the same way. Never NULL. */
Type *parse_type_string(Arena *a, const char *text, size_t len, const char *name, uint32_t *src_out);

/* The type an expression denotes when it is syntactically a type (paths with
 * generic arguments, EXPR_TYPE, `typeof(e)` calls, tuples and parentheses of
 * those), allocated in `a`; NULL otherwise. GenericArg.type is computed with it. */
Type *ast_type_from_expr(Arena *a, Expr *e);

/* Deterministic S-expression dump of a file (ast_dump.c), as used by
 * `tern parse --dump` and the tests/fixtures/<dir>/<name>.parse golden files. */
void ast_dump(const AstFile *f, struct StrBuf *out);
void ast_dump_expr(const Expr *e, struct StrBuf *out);
void ast_dump_type(const Type *t, struct StrBuf *out);

#endif /* TERN_PARSER_H */
