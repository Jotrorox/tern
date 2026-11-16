/* lexer.h — Tern lexer: UTF-8 source text to an array of tokens.
 *
 *     LexOutput lx;
 *     lex_source(&arena, src_id, &lx);   // tokens, comments, doc blocks in the arena
 *     for (i = 0; i < lx.ntoks; i++) ... lx.toks[i] ...   // last one is TK_EOF
 *
 * Lexing never stops at an error: `===` lexes as `==`, `+=` as `=`, `?` and
 * SystemVerilog-only characters as TK_ERROR (which the parser skips
 * silently). Errors are not printed by lex_source; they are collected in
 * LexOutput.diags (source order) and reported with lex_emit_diag(). The
 * parser does that as it passes their position, so lexical and syntax
 * errors come out interleaved in source order.
 */
#ifndef TERN_LEXER_H
#define TERN_LEXER_H

#include "token.h"
#include "../base/arena.h"

typedef struct LexDiag {
    Span span;
    const char *msg;      /* arena */
    const char *help;     /* one-line fix, or NULL */
} LexDiag;

typedef struct LexOutput {
    Token *toks;          /* ntoks entries, the last is TK_EOF */
    uint32_t ntoks;
    Comment *comments;    /* in source order */
    uint32_t ncomments;
    TokDoc *docs;         /* doc blocks, referenced by Token.doc */
    uint32_t ndocs;
    LexDiag *diags;       /* lexical errors, in source order (not yet reported) */
    uint32_t ndiags;
    int nerrors;          /* == ndiags */
} LexOutput;

/* Lexes source `file` (registered with src_add). Everything lives in `a`. */
void lex_source(Arena *a, uint32_t file, LexOutput *out);

/* Reports one lexical error through diag.h (diag_error + diag_help). */
void lex_emit_diag(const LexDiag *d);

/* Keyword kind of an identifier spelling, or TK_IDENT. */
TokenKind lex_keyword_kind(const char *s, size_t len);

#endif /* TERN_LEXER_H */
