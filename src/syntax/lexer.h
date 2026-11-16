/* lexer.h — Tern lexer: UTF-8 source text to an array of tokens.
 *
 *     LexOutput lx;
 *     lex_source(&arena, src_id, &lx);   // tokens, comments, doc blocks in the arena
 *     for (i = 0; i < lx.ntoks; i++) ... lx.toks[i] ...   // last one is TK_EOF
 *
 * Lexical errors are reported through diag.h (each one once) and lexing
 * continues: `===` lexes as `==`, `+=` as `=`, `?` and SystemVerilog-only
 * characters as TK_ERROR (which the parser skips silently).
 */
#ifndef TERN_LEXER_H
#define TERN_LEXER_H

#include "token.h"
#include "../base/arena.h"

typedef struct LexOutput {
    Token *toks;          /* ntoks entries, the last is TK_EOF */
    uint32_t ntoks;
    Comment *comments;    /* in source order */
    uint32_t ncomments;
    TokDoc *docs;         /* doc blocks, referenced by Token.doc */
    uint32_t ndocs;
    int nerrors;          /* lexical errors reported */
} LexOutput;

/* Lexes source `file` (registered with src_add). Everything lives in `a`. */
void lex_source(Arena *a, uint32_t file, LexOutput *out);

/* Keyword kind of an identifier spelling, or TK_IDENT. */
TokenKind lex_keyword_kind(const char *s, size_t len);

#endif /* TERN_LEXER_H */
