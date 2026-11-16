/* token.h — Tern token kinds and the token / comment records produced by
 * the lexer (src/syntax/lexer.c).
 *
 * Newlines are not tokens: every token records whether a line break
 * separates it from the previous token (`nl_before`), and the parser decides
 * where that is significant (docs/grammar.md, "Newlines").
 *
 * Comments are not tokens either; the lexer keeps them, with spans, in a
 * per-file side list (Comment) for the formatter and the doc generator.
 * `///` doc comments are additionally collected into doc blocks (TokDoc):
 * consecutive `///` lines form one block, which is attached to the next
 * token (`Token.doc`); the parser moves it onto the item, port, field or
 * variant that token starts.
 */
#ifndef TERN_TOKEN_H
#define TERN_TOKEN_H

#include "../base/common.h"
#include "../base/diag.h"

typedef enum TokenKind {
    TK_EOF = 0,
    TK_ERROR,         /* malformed input; the lexer has already reported it */

    TK_IDENT,         /* also the contextual words static_assert always never eventually until */
    TK_INT,           /* 42, 0xFF, 0b1010, 0o17, 1_000, typed 42u8 / 3s12 / 0xFFu (+ `<W>`) */
    TK_STRING,        /* "...", escapes decoded into Token.str */

    /* the 38 keywords (spec §3.3) */
    TK_KW_PACKAGE, TK_KW_USE, TK_KW_PUB, TK_KW_MODULE, TK_KW_EXTERN, TK_KW_BUNDLE,
    TK_KW_STRUCT, TK_KW_ENUM, TK_KW_CONST, TK_KW_FN, TK_KW_TYPE, TK_KW_REGMAP,
    TK_KW_DOMAIN, TK_KW_WHERE, TK_KW_TEST, TK_KW_PROPERTY, TK_KW_SEQUENCE, TK_KW_IN,
    TK_KW_OUT, TK_KW_FLIP, TK_KW_LET, TK_KW_VAR, TK_KW_REG, TK_KW_MEM, TK_KW_ON,
    TK_KW_COMB, TK_KW_MATCH, TK_KW_IF, TK_KW_ELSE, TK_KW_FOR, TK_KW_WHILE,
    TK_KW_RETURN, TK_KW_AS, TK_KW_SIM, TK_KW_EXPECT, TK_KW_ASSERT, TK_KW_ASSUME,
    TK_KW_COVER,

    /* word operators and literals */
    TK_AND, TK_OR, TK_NOT, TK_XOR,          /* also && || ! (lexed as these kinds) */
    TK_TRUE, TK_FALSE, TK_ALL0, TK_ALL1,

    /* impl trait async yield generate initial wire logic: reserved, rejected
     * by the parser with a dedicated message (Token.str says which) */
    TK_RESERVED,

    /* punctuation and operators */
    TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET, TK_LBRACE, TK_RBRACE,
    TK_COMMA, TK_SEMI, TK_COLON, TK_COLONCOLON,
    TK_DOT, TK_DOTDOT, TK_DOTDOTEQ,
    TK_ASSIGN,                              /* = */
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_PLUS_WRAP, TK_MINUS_WRAP,            /* +% -% */
    TK_SHL, TK_SHR, TK_SHRA,                /* << >> >>> */
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE,    /* & | ^ ~ */
    TK_CONCAT,                              /* ++ */
    TK_ARROW, TK_FAT_ARROW,                 /* -> => */
    TK_IMPLIES, TK_IMPLIES_NEXT,            /* |-> |=> */
    TK_HASHHASH,                            /* ## */
    TK_AT, TK_DOLLAR,                       /* @ $ */
    TK_ATTR,                                /* #[ (an attribute opens; `]` closes it) */

    TK_COUNT
} TokenKind;

typedef struct Token {
    uint16_t kind;        /* TokenKind */
    uint8_t nl_before;    /* a line break separates it from the previous token */
    uint8_t suffix;       /* TK_INT: 0, 'u' or 's' (typed literal) */
    uint32_t off, len;    /* bytes in the source (a typed literal includes its suffix) */
    uint32_t line, col;   /* of `off`, as in Span (1-based; col counts code points) */
    /* TK_IDENT, keywords, TK_RESERVED: the interned spelling.
     * TK_INT: the number without suffix, as written (arena copy, `_` kept).
     * TK_STRING: the decoded contents (arena copy, NUL-terminated). */
    const char *str;
    uint32_t str_len;     /* TK_STRING: decoded length; otherwise strlen(str) */
    /* TK_INT with suffix: the width digits (`8` in 42u8); width_len == 0 when
     * the width is a const expression in `<...>` that follows (0xFFu<W>). */
    uint32_t width_off, width_len;
    int32_t doc;          /* index into the doc blocks of the `///` block before it, or -1 */
} Token;

typedef enum CommentKind {
    COMMENT_LINE,         /* // ... */
    COMMENT_BLOCK,        /* / * ... * / (nestable) */
    COMMENT_DOC           /* /// ... (one entry per line) */
} CommentKind;

typedef struct Comment {
    CommentKind kind;
    Span span;            /* the whole comment, without the line terminator */
} Comment;

/* A block of consecutive `///` lines. */
typedef struct TokDoc {
    Span span;            /* from the first `///` to the end of the last line */
    const char *text;     /* lines without `///` and one following space, joined by '\n' */
} TokDoc;

/* "`=>`", "identifier", "keyword `in`", "end of file", ... (static strings). */
const char *tok_kind_name(TokenKind k);
/* The fixed spelling of a punctuation/keyword kind ("=>", "module"), or NULL. */
const char *tok_kind_text(TokenKind k);

#endif /* TERN_TOKEN_H */
