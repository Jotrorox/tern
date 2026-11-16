/* lexer.c — Tern lexer (see lexer.h and docs/grammar.md, "Lexical"). */
#include "lexer.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/utf8.h"

/* ------------------------------------------------------------ token names */

static const char *const lex_kind_texts[TK_COUNT] = {
    /* TK_EOF */ NULL, /* TK_ERROR */ NULL, /* TK_IDENT */ NULL, /* TK_INT */ NULL,
    /* TK_STRING */ NULL,
    "package", "use", "pub", "module", "extern", "bundle",
    "struct", "enum", "const", "fn", "type", "regmap",
    "domain", "where", "test", "property", "sequence", "in",
    "out", "flip", "let", "var", "reg", "mem", "on",
    "comb", "match", "if", "else", "for", "while",
    "return", "as", "sim", "expect", "assert", "assume",
    "cover",
    "and", "or", "not", "xor",
    "true", "false", "all0", "all1",
    /* TK_RESERVED */ NULL,
    "(", ")", "[", "]", "{", "}",
    ",", ";", ":", "::",
    ".", "..", "..=",
    "=",
    "==", "!=", "<", "<=", ">", ">=",
    "+", "-", "*", "/", "%",
    "+%", "-%",
    "<<", ">>", ">>>",
    "&", "|", "^", "~",
    "++",
    "->", "=>",
    "|->", "|=>",
    "##",
    "@", "$",
    "#[",
};

TERN_STATIC_ASSERT(TK_ATTR + 1 == TK_COUNT, lex_kind_table_complete);

const char *tok_kind_text(TokenKind k)
{
    if ((int)k < 0 || k >= TK_COUNT)
        return NULL;
    return lex_kind_texts[k];
}

const char *tok_kind_name(TokenKind k)
{
    static const char *const quoted[TK_COUNT] = {
        "end of file", "invalid token", "identifier", "integer literal", "string literal",
        "keyword `package`", "keyword `use`", "keyword `pub`", "keyword `module`",
        "keyword `extern`", "keyword `bundle`", "keyword `struct`", "keyword `enum`",
        "keyword `const`", "keyword `fn`", "keyword `type`", "keyword `regmap`",
        "keyword `domain`", "keyword `where`", "keyword `test`", "keyword `property`",
        "keyword `sequence`", "keyword `in`", "keyword `out`", "keyword `flip`",
        "keyword `let`", "keyword `var`", "keyword `reg`", "keyword `mem`", "keyword `on`",
        "keyword `comb`", "keyword `match`", "keyword `if`", "keyword `else`",
        "keyword `for`", "keyword `while`", "keyword `return`", "keyword `as`",
        "keyword `sim`", "keyword `expect`", "keyword `assert`", "keyword `assume`",
        "keyword `cover`",
        "`and`", "`or`", "`not`", "`xor`", "`true`", "`false`", "`all0`", "`all1`",
        "reserved word",
        "`(`", "`)`", "`[`", "`]`", "`{`", "`}`",
        "`,`", "`;`", "`:`", "`::`",
        "`.`", "`..`", "`..=`",
        "`=`",
        "`==`", "`!=`", "`<`", "`<=`", "`>`", "`>=`",
        "`+`", "`-`", "`*`", "`/`", "`%`",
        "`+%`", "`-%`",
        "`<<`", "`>>`", "`>>>`",
        "`&`", "`|`", "`^`", "`~`",
        "`++`",
        "`->`", "`=>`",
        "`|->`", "`|=>`",
        "`##`",
        "`@`", "`$`",
        "`#[`",
    };
    if ((int)k < 0 || k >= TK_COUNT)
        return "token";
    return quoted[k];
}

/* -------------------------------------------------------------- keywords */

typedef struct LexKeyword {
    const char *name;
    TokenKind kind;
} LexKeyword;

static const LexKeyword lex_keywords[] = {
    {"package", TK_KW_PACKAGE}, {"use", TK_KW_USE}, {"pub", TK_KW_PUB},
    {"module", TK_KW_MODULE}, {"extern", TK_KW_EXTERN}, {"bundle", TK_KW_BUNDLE},
    {"struct", TK_KW_STRUCT}, {"enum", TK_KW_ENUM}, {"const", TK_KW_CONST},
    {"fn", TK_KW_FN}, {"type", TK_KW_TYPE}, {"regmap", TK_KW_REGMAP},
    {"domain", TK_KW_DOMAIN}, {"where", TK_KW_WHERE}, {"test", TK_KW_TEST},
    {"property", TK_KW_PROPERTY}, {"sequence", TK_KW_SEQUENCE}, {"in", TK_KW_IN},
    {"out", TK_KW_OUT}, {"flip", TK_KW_FLIP}, {"let", TK_KW_LET}, {"var", TK_KW_VAR},
    {"reg", TK_KW_REG}, {"mem", TK_KW_MEM}, {"on", TK_KW_ON}, {"comb", TK_KW_COMB},
    {"match", TK_KW_MATCH}, {"if", TK_KW_IF}, {"else", TK_KW_ELSE}, {"for", TK_KW_FOR},
    {"while", TK_KW_WHILE}, {"return", TK_KW_RETURN}, {"as", TK_KW_AS},
    {"sim", TK_KW_SIM}, {"expect", TK_KW_EXPECT}, {"assert", TK_KW_ASSERT},
    {"assume", TK_KW_ASSUME}, {"cover", TK_KW_COVER},
    {"and", TK_AND}, {"or", TK_OR}, {"not", TK_NOT}, {"xor", TK_XOR},
    {"true", TK_TRUE}, {"false", TK_FALSE}, {"all0", TK_ALL0}, {"all1", TK_ALL1},
    {"impl", TK_RESERVED}, {"trait", TK_RESERVED}, {"async", TK_RESERVED},
    {"yield", TK_RESERVED}, {"generate", TK_RESERVED}, {"initial", TK_RESERVED},
    {"wire", TK_RESERVED}, {"logic", TK_RESERVED},
};

TokenKind lex_keyword_kind(const char *s, size_t len)
{
    size_t i;
    if (len < 2 || len > 8 || !tern_is_lower((unsigned char)s[0]))
        return TK_IDENT;
    for (i = 0; i < TERN_ARRAY_LEN(lex_keywords); i++) {
        const char *k = lex_keywords[i].name;
        if (k[0] == s[0] && strlen(k) == len && memcmp(k, s, len) == 0)
            return lex_keywords[i].kind;
    }
    return TK_IDENT;
}

/* ----------------------------------------------------------------- lexer */

typedef VEC(Token) LexTokVec;
typedef VEC(Comment) LexCommentVec;
typedef VEC(TokDoc) LexDocVec;
typedef VEC(LexDiag) LexDiagVec;

typedef struct Lexer {
    Arena *arena;
    uint32_t file;
    const char *s;
    size_t n;
    size_t i;
    int nl;               /* line break seen since the last token */
    LexTokVec toks;
    LexCommentVec comments;
    LexDocVec docs;
    StrBuf doc_text;      /* pending doc block */
    size_t doc_start, doc_end;
    int doc_pending;
    LexDiagVec diags;     /* errors, reported later in source order (lex_emit_diag) */
} Lexer;

/* The lexer running now (the compiler is single-threaded); lex_help attaches
 * to its last error. */
static Lexer *lex_current;

static Span lex_span(Lexer *lx, size_t off, size_t len)
{
    return span_make(lx->file, (uint32_t)off, (uint32_t)len);
}

static void lex_error(Lexer *lx, size_t off, size_t len, const char *fmt, ...) TERN_PRINTF(4, 5);

static void lex_error(Lexer *lx, size_t off, size_t len, const char *fmt, ...)
{
    LexDiag d;
    va_list ap;
    va_start(ap, fmt);
    d.span = lex_span(lx, off, len);
    d.msg = arena_vprintf(lx->arena, fmt, ap);
    d.help = NULL;
    va_end(ap);
    vec_push(lx->diags, d);
}

static void lex_help(const char *fmt, ...) TERN_PRINTF(1, 2);
static void lex_help(const char *fmt, ...)
{
    va_list ap;
    if (!lex_current || lex_current->diags.len == 0)
        return;
    va_start(ap, fmt);
    vec_last(lex_current->diags).help = arena_vprintf(lex_current->arena, fmt, ap);
    va_end(ap);
}

void lex_emit_diag(const LexDiag *d)
{
    diag_error(d->span, "%s", d->msg);
    if (d->help)
        diag_help("%s", d->help);
}

static Token *lex_push(Lexer *lx, TokenKind kind, size_t off, size_t len)
{
    Token t;
    memset(&t, 0, sizeof t);
    t.kind = (uint16_t)kind;
    t.nl_before = (uint8_t)(lx->nl ? 1 : 0);
    t.off = (uint32_t)off;
    t.len = (uint32_t)len;
    t.doc = -1;
    if (lx->doc_pending) {
        TokDoc d;
        d.span = lex_span(lx, lx->doc_start, lx->doc_end - lx->doc_start);
        d.text = arena_strndup(lx->arena, sb_cstr(&lx->doc_text), lx->doc_text.len);
        vec_push(lx->docs, d);
        t.doc = (int32_t)(lx->docs.len - 1);
        lx->doc_pending = 0;
        sb_reset(&lx->doc_text);
    }
    lx->nl = 0;
    vec_push(lx->toks, t);
    return &vec_last(lx->toks);
}

static void lex_comment(Lexer *lx, CommentKind kind, size_t off, size_t len)
{
    Comment c;
    c.kind = kind;
    c.span = lex_span(lx, off, len);
    vec_push(lx->comments, c);
}

static int lex_peek(Lexer *lx, size_t k)
{
    return lx->i + k < lx->n ? (unsigned char)lx->s[lx->i + k] : 0;
}

/* Skips a line comment starting at lx->i ("//"); records it. */
static void lex_line_comment(Lexer *lx)
{
    size_t start = lx->i, end;
    int doc = lex_peek(lx, 2) == '/' && lex_peek(lx, 3) != '/';
    while (lx->i < lx->n && lx->s[lx->i] != '\n')
        lx->i++;
    end = lx->i;
    if (end > start && lx->s[end - 1] == '\r')
        end--;
    {
        size_t bad;
        if (!utf8_valid(lx->s + start, end - start, &bad))
            lex_error(lx, start + bad, 1, "invalid UTF-8 in comment");
    }
    if (doc) {
        size_t t = start + 3;
        lex_comment(lx, COMMENT_DOC, start, end - start);
        if (t < end && lx->s[t] == ' ')
            t++;
        if (!lx->doc_pending) {
            lx->doc_pending = 1;
            lx->doc_start = start;
        } else {
            sb_putc(&lx->doc_text, '\n');
        }
        sb_append(&lx->doc_text, lx->s + t, end - t);
        lx->doc_end = end;
    } else {
        lex_comment(lx, COMMENT_LINE, start, end - start);
    }
}

static void lex_block_comment(Lexer *lx)
{
    size_t start = lx->i;
    int depth = 0;
    while (lx->i < lx->n) {
        int c = (unsigned char)lx->s[lx->i];
        if (c == '/' && lex_peek(lx, 1) == '*') {
            depth++;
            lx->i += 2;
        } else if (c == '*' && lex_peek(lx, 1) == '/') {
            depth--;
            lx->i += 2;
            if (depth == 0)
                break;
        } else {
            if (c == '\n')
                lx->nl = 1;
            lx->i++;
        }
    }
    if (depth != 0) {
        lex_error(lx, start, 2, "unterminated block comment");
    } else {
        size_t bad;
        if (!utf8_valid(lx->s + start, lx->i - start, &bad))
            lex_error(lx, start + bad, 1, "invalid UTF-8 in comment");
    }
    lex_comment(lx, COMMENT_BLOCK, start, lx->i - start);
}

static void lex_ident(Lexer *lx)
{
    size_t start = lx->i;
    TokenKind k;
    Token *t;
    while (lx->i < lx->n && tern_is_ident((unsigned char)lx->s[lx->i]))
        lx->i++;
    k = lex_keyword_kind(lx->s + start, lx->i - start);
    t = lex_push(lx, k, start, lx->i - start);
    t->str = intern(lx->s + start, lx->i - start);
    t->str_len = (uint32_t)(lx->i - start);
}

static void lex_number(Lexer *lx)
{
    size_t start = lx->i, digits_start, end;
    int base = 10, bad = 0;
    Token *t;
    if (lx->s[lx->i] == '0' && (lex_peek(lx, 1) == 'x' || lex_peek(lx, 1) == 'X')) {
        base = 16;
        lx->i += 2;
    } else if (lx->s[lx->i] == '0' && (lex_peek(lx, 1) == 'b' || lex_peek(lx, 1) == 'B')) {
        base = 2;
        lx->i += 2;
    } else if (lx->s[lx->i] == '0' && (lex_peek(lx, 1) == 'o' || lex_peek(lx, 1) == 'O')) {
        base = 8;
        lx->i += 2;
    }
    digits_start = lx->i;
    for (; lx->i < lx->n; lx->i++) {
        int c = (unsigned char)lx->s[lx->i];
        if (c == '_')
            continue;
        if (base == 16 ? tern_is_xdigit(c) : tern_is_digit(c)) {
            if ((base == 2 && c > '1') || (base == 8 && c > '7')) {
                if (!bad)
                    lex_error(lx, lx->i, 1, "invalid digit `%c` in %s literal", c,
                              base == 2 ? "binary" : "octal");
                bad = 1;
            }
            continue;
        }
        break;
    }
    {
        size_t k, nd = 0;
        for (k = digits_start; k < lx->i; k++)
            nd += lx->s[k] != '_';
        if (nd == 0 && !bad) {
            lex_error(lx, start, lx->i - start, "missing digits after `%.*s`",
                      (int)(digits_start - start), lx->s + start);
            bad = 1;
        }
    }
    end = lx->i;
    /* 1.5: there are no floating-point literals (1..5 is a range) */
    if (lex_peek(lx, 0) == '.' && tern_is_digit(lex_peek(lx, 1))) {
        size_t f = lx->i + 1;
        while (f < lx->n && (tern_is_digit((unsigned char)lx->s[f]) || lx->s[f] == '_'))
            f++;
        lex_error(lx, start, f - start, "floating-point literals do not exist in Tern");
        lex_help("use an integer, or std::math::Fixed for fractional values");
        lx->i = f;
        end = f;
        bad = 1;
    }
    t = lex_push(lx, bad ? TK_ERROR : TK_INT, start, end - start);
    t->str = arena_strndup(lx->arena, lx->s + start, end - start);
    t->str_len = (uint32_t)(end - start);
    /* typed literal suffix */
    if (lx->i < lx->n && (lx->s[lx->i] == 'u' || lx->s[lx->i] == 's')) {
        int c1 = lex_peek(lx, 1);
        char sfx = lx->s[lx->i];
        if (tern_is_digit(c1)) {
            size_t w = lx->i + 1;
            while (w < lx->n && tern_is_digit((unsigned char)lx->s[w]))
                w++;
            if (w < lx->n && tern_is_ident((unsigned char)lx->s[w])) {
                size_t e = w;
                while (e < lx->n && tern_is_ident((unsigned char)lx->s[e]))
                    e++;
                lex_error(lx, lx->i, e - lx->i, "invalid suffix `%.*s` on integer literal",
                          (int)(e - lx->i), lx->s + lx->i);
                lex_help("a typed literal is written `42u8`, `3s12` or `0xFFu<W>`");
                t->kind = TK_ERROR;
                w = e;
            }
            t->suffix = (uint8_t)sfx;
            t->width_off = (uint32_t)(lx->i + 1);
            t->width_len = (uint32_t)(w - lx->i - 1);
            lx->i = w;
        } else if (c1 == '<') {
            t->suffix = (uint8_t)sfx;
            t->width_off = (uint32_t)(lx->i + 1);
            t->width_len = 0;
            lx->i++;
        } else {
            size_t e = lx->i;
            while (e < lx->n && tern_is_ident((unsigned char)lx->s[e]))
                e++;
            if (e - lx->i == 1)
                lex_error(lx, lx->i, 1, "typed literal `%.*s` needs a width",
                          (int)(e - start), lx->s + start);
            else
                lex_error(lx, lx->i, e - lx->i, "invalid suffix `%.*s` on integer literal",
                          (int)(e - lx->i), lx->s + lx->i);
            lex_help("a typed literal is written `42u8`, `3s12` or `0xFFu<W>`");
            t->kind = TK_ERROR;
            lx->i = e;
        }
        t->len = (uint32_t)(lx->i - start);
    } else if (lx->i < lx->n && tern_is_ident((unsigned char)lx->s[lx->i])) {
        size_t e = lx->i;
        while (e < lx->n && tern_is_ident((unsigned char)lx->s[e]))
            e++;
        if (t->kind != TK_ERROR) {
            lex_error(lx, lx->i, e - lx->i, "invalid suffix `%.*s` on integer literal",
                      (int)(e - lx->i), lx->s + lx->i);
            lex_help("a typed literal is written `42u8`, `3s12` or `0xFFu<W>`");
        }
        t->kind = TK_ERROR;
        lx->i = e;
        t->len = (uint32_t)(lx->i - start);
    }
}

static void lex_string(Lexer *lx)
{
    size_t start = lx->i;
    StrBuf sb = {0};
    int ok = 1;
    Token *t;
    lx->i++; /* opening quote */
    for (;;) {
        int c;
        if (lx->i >= lx->n || lx->s[lx->i] == '\n') {
            lex_error(lx, start, 1, "unterminated string literal");
            lex_help("close it with `\"` on the same line");
            ok = 0;
            break;
        }
        c = (unsigned char)lx->s[lx->i];
        if (c == '"') {
            lx->i++;
            break;
        }
        if (c == '\\') {
            int e = lex_peek(lx, 1);
            size_t esc = lx->i;
            lx->i += 2;
            switch (e) {
            case 'n': sb_putc(&sb, '\n'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case 'r': sb_putc(&sb, '\r'); break;
            case '0': sb_putc(&sb, '\0'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            case '"': sb_putc(&sb, '"'); break;
            case '\'': sb_putc(&sb, '\''); break;
            case '{': sb_putc(&sb, '{'); break;
            case '}': sb_putc(&sb, '}'); break;
            case 'x': {
                int h1 = tern_hex_value(lex_peek(lx, 0)), h2 = tern_hex_value(lex_peek(lx, 1));
                if (h1 < 0 || h2 < 0 || h1 * 16 + h2 > 0x7F) {
                    lex_error(lx, esc, 2, "invalid escape `\\x`: expected two hex digits 00-7F");
                    ok = 0;
                } else {
                    sb_putc(&sb, h1 * 16 + h2);
                    lx->i += 2;
                }
                break;
            }
            case 'u': {
                uint32_t cp = 0;
                int nd = 0, good = lex_peek(lx, 0) == '{';
                if (good) {
                    lx->i++;
                    while (tern_hex_value(lex_peek(lx, 0)) >= 0 && nd < 6) {
                        cp = cp * 16 + (uint32_t)tern_hex_value(lex_peek(lx, 0));
                        lx->i++;
                        nd++;
                    }
                    good = nd > 0 && lex_peek(lx, 0) == '}';
                    if (good)
                        lx->i++;
                }
                if (good) {
                    char buf[4];
                    int nb = utf8_encode(cp, buf);
                    if (nb == 0)
                        good = 0;
                    else
                        sb_append(&sb, buf, (size_t)nb);
                }
                if (!good) {
                    lex_error(lx, esc, lx->i - esc, "invalid escape: expected `\\u{XXXX}` with a valid code point");
                    ok = 0;
                }
                break;
            }
            default:
                if (e == '\n' || e == 0) {
                    lx->i = esc + 1;
                    lex_error(lx, esc, 1, "unterminated string literal");
                    ok = 0;
                    goto done;
                }
                lex_error(lx, esc, 2, "unknown escape `\\%c`", e);
                lex_help("valid escapes: \\n \\t \\r \\0 \\\\ \\\" \\xHH \\u{XXXX}");
                ok = 0;
                break;
            }
            continue;
        }
        if (c >= 0x80) {
            size_t adv;
            uint32_t cp = utf8_decode(lx->s + lx->i, lx->n - lx->i, &adv);
            if (cp == UTF8_INVALID) {
                lex_error(lx, lx->i, 1, "invalid UTF-8 in string literal");
                ok = 0;
            } else {
                sb_append(&sb, lx->s + lx->i, adv);
            }
            lx->i += adv;
            continue;
        }
        sb_putc(&sb, c);
        lx->i++;
    }
done:
    t = lex_push(lx, ok ? TK_STRING : TK_ERROR, start, lx->i - start);
    t->str = arena_strndup(lx->arena, sb_cstr(&sb), sb.len);
    t->str_len = (uint32_t)sb.len;
    sb_free(&sb);
}

/* `'` — SystemVerilog literal syntax ('0, '1, 8'hFF). */
static void lex_sv_quote(Lexer *lx)
{
    size_t start = lx->i, e = lx->i + 1;
    size_t tok_start = start;
    Token *prev = lx->toks.len ? &vec_last(lx->toks) : NULL;
    while (e < lx->n && tern_is_ident((unsigned char)lx->s[e]))
        e++;
    if (prev && prev->kind == TK_INT && prev->off + prev->len == start && !lx->nl) {
        tok_start = prev->off;
        lx->toks.len--; /* fold the size into the error token */
    }
    if (e == start + 2 && (lx->s[start + 1] == '0' || lx->s[start + 1] == '1')) {
        lex_error(lx, tok_start, e - tok_start, "`%.*s` is SystemVerilog, not Tern",
                  (int)(e - tok_start), lx->s + tok_start);
        lex_help("write `all%c`", lx->s[start + 1]);
    } else {
        lex_error(lx, tok_start, e - tok_start, "SystemVerilog sized literal `%.*s` does not exist in Tern",
                  (int)(e - tok_start), lx->s + tok_start);
        lex_help("write a typed literal such as `0xFFu8`, or a plain `0xFF` where the width comes from context");
    }
    lx->i = e;
    lex_push(lx, TK_ERROR, tok_start, e - tok_start);
}

static void lex_compound_assign(Lexer *lx, size_t oplen)
{
    lex_error(lx, lx->i, oplen + 1, "compound assignment `%.*s=` does not exist in Tern",
              (int)oplen, lx->s + lx->i);
    lex_help("write it out: `x = x %.*s y`", (int)oplen, lx->s + lx->i);
    lex_push(lx, TK_ASSIGN, lx->i, oplen + 1);
    lx->i += oplen + 1;
}

static void lex_op(Lexer *lx, TokenKind k, size_t len)
{
    lex_push(lx, k, lx->i, len);
    lx->i += len;
}

void lex_source(Arena *a, uint32_t file, LexOutput *out)
{
    const SourceFile *sf = src_get(file);
    Lexer lx;
    memset(&lx, 0, sizeof lx);
    memset(out, 0, sizeof *out);
    lx.arena = a;
    lx.file = file;
    lx.s = sf ? sf->text : "";
    lx.n = sf ? sf->len : 0;
    lx.nl = 1;
    lex_current = &lx;
    if (lx.n >= 3 && (unsigned char)lx.s[0] == 0xEF && (unsigned char)lx.s[1] == 0xBB &&
        (unsigned char)lx.s[2] == 0xBF)
        lx.i = 3;

    while (lx.i < lx.n) {
        int c = (unsigned char)lx.s[lx.i];
        int c1 = lex_peek(&lx, 1), c2 = lex_peek(&lx, 2);
        switch (c) {
        case '\n':
            lx.nl = 1;
            lx.i++;
            continue;
        case ' ': case '\t': case '\r': case '\f': case '\v':
            lx.i++;
            continue;
        case '(': lex_op(&lx, TK_LPAREN, 1); continue;
        case ')': lex_op(&lx, TK_RPAREN, 1); continue;
        case '[': lex_op(&lx, TK_LBRACKET, 1); continue;
        case ']': lex_op(&lx, TK_RBRACKET, 1); continue;
        case '{': lex_op(&lx, TK_LBRACE, 1); continue;
        case '}': lex_op(&lx, TK_RBRACE, 1); continue;
        case ',': lex_op(&lx, TK_COMMA, 1); continue;
        case ';': lex_op(&lx, TK_SEMI, 1); continue;
        case '@': lex_op(&lx, TK_AT, 1); continue;
        case '$': lex_op(&lx, TK_DOLLAR, 1); continue;
        case '~': lex_op(&lx, TK_TILDE, 1); continue;
        case ':':
            if (c1 == ':') lex_op(&lx, TK_COLONCOLON, 2);
            else lex_op(&lx, TK_COLON, 1);
            continue;
        case '.':
            if (c1 == '.' && c2 == '=') lex_op(&lx, TK_DOTDOTEQ, 3);
            else if (c1 == '.') lex_op(&lx, TK_DOTDOT, 2);
            else lex_op(&lx, TK_DOT, 1);
            continue;
        case '=':
            if (c1 == '=' && c2 == '=') {
                lex_error(&lx, lx.i, 3, "`===` does not exist; equality is always 2-state");
                lex_help("use `==`");
                lex_op(&lx, TK_EQ, 3);
            } else if (c1 == '=') lex_op(&lx, TK_EQ, 2);
            else if (c1 == '>') lex_op(&lx, TK_FAT_ARROW, 2);
            else lex_op(&lx, TK_ASSIGN, 1);
            continue;
        case '!':
            if (c1 == '=' && c2 == '=') {
                lex_error(&lx, lx.i, 3, "`!==` does not exist; equality is always 2-state");
                lex_help("use `!=`");
                lex_op(&lx, TK_NE, 3);
            } else if (c1 == '=') lex_op(&lx, TK_NE, 2);
            else lex_op(&lx, TK_NOT, 1);
            continue;
        case '<':
            if (c1 == '<') lex_op(&lx, TK_SHL, 2);
            else if (c1 == '=') lex_op(&lx, TK_LE, 2);
            else lex_op(&lx, TK_LT, 1);
            continue;
        case '>':
            if (c1 == '>' && c2 == '>') lex_op(&lx, TK_SHRA, 3);
            else if (c1 == '>') lex_op(&lx, TK_SHR, 2);
            else if (c1 == '=') lex_op(&lx, TK_GE, 2);
            else lex_op(&lx, TK_GT, 1);
            continue;
        case '+':
            if (c1 == '%') lex_op(&lx, TK_PLUS_WRAP, 2);
            else if (c1 == '+') lex_op(&lx, TK_CONCAT, 2);
            else if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_PLUS, 1);
            continue;
        case '-':
            if (c1 == '%') lex_op(&lx, TK_MINUS_WRAP, 2);
            else if (c1 == '>') lex_op(&lx, TK_ARROW, 2);
            else if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_MINUS, 1);
            continue;
        case '*':
            if (c1 == '*') {
                lex_error(&lx, lx.i, 2, "`**` does not exist in Tern");
                lex_help("use `1 << n` for powers of two, or a const fn");
                lex_op(&lx, TK_ERROR, 2);
            } else if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_STAR, 1);
            continue;
        case '/':
            if (c1 == '/') { lex_line_comment(&lx); continue; }
            if (c1 == '*') { lex_block_comment(&lx); continue; }
            if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_SLASH, 1);
            continue;
        case '%':
            if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_PERCENT, 1);
            continue;
        case '&':
            if (c1 == '&') lex_op(&lx, TK_AND, 2);
            else if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_AMP, 1);
            continue;
        case '|':
            if (c1 == '-' && c2 == '>') lex_op(&lx, TK_IMPLIES, 3);
            else if (c1 == '=' && c2 == '>') lex_op(&lx, TK_IMPLIES_NEXT, 3);
            else if (c1 == '|') lex_op(&lx, TK_OR, 2);
            else if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_PIPE, 1);
            continue;
        case '^':
            if (c1 == '=') lex_compound_assign(&lx, 1);
            else lex_op(&lx, TK_CARET, 1);
            continue;
        case '#':
            if (c1 == '#') lex_op(&lx, TK_HASHHASH, 2);
            else if (c1 == '[') lex_op(&lx, TK_ATTR, 2);
            else {
                if (tern_is_digit(c1)) {
                    lex_error(&lx, lx.i, 1, "`#` delays do not exist; the simulator is cycle-based");
                    lex_help("in a test, advance time with `dut.step(n)`");
                } else {
                    lex_error(&lx, lx.i, 1, "unexpected `#`");
                    lex_help("attributes are written `#[name(args)]`");
                }
                lex_op(&lx, TK_ERROR, 1);
            }
            continue;
        case '?':
            lex_error(&lx, lx.i, 1, "`?:` does not exist in Tern");
            lex_help("use `if c { a } else { b }`; `if` is an expression");
            lex_op(&lx, TK_ERROR, 1);
            continue;
        case '\'':
            lex_sv_quote(&lx);
            continue;
        case '`': {
            size_t e = lx.i + 1;
            while (e < lx.n && tern_is_ident((unsigned char)lx.s[e]))
                e++;
            lex_error(&lx, lx.i, e - lx.i, "SystemVerilog directive `%.*s` does not exist: there is no preprocessor in Tern",
                      (int)(e - lx.i - 1), lx.s + lx.i + 1);
            lex_help("use `const`/`const fn` for macros, `use` for includes and a bool generic or "
                      "`cfg::feature` for `ifdef`");
            lex_push(&lx, TK_ERROR, lx.i, e - lx.i);
            lx.i = e;
            continue;
        }
        case '"':
            lex_string(&lx);
            continue;
        default:
            break;
        }
        if (tern_is_digit(c)) {
            lex_number(&lx);
        } else if (tern_is_alpha(c) || c == '_') {
            lex_ident(&lx);
        } else if (c >= 0x80) {
            size_t adv;
            uint32_t cp = utf8_decode(lx.s + lx.i, lx.n - lx.i, &adv);
            if (cp == UTF8_INVALID)
                lex_error(&lx, lx.i, 1, "invalid UTF-8 byte 0x%02X", c);
            else
                lex_error(&lx, lx.i, adv, "non-ASCII character `%.*s` outside a string or comment",
                          (int)adv, lx.s + lx.i);
            lex_help("identifiers are ASCII; Unicode is allowed only in strings and comments");
            lex_push(&lx, TK_ERROR, lx.i, adv);
            lx.i += adv;
        } else {
            if (c >= 0x20 && c < 0x7F)
                lex_error(&lx, lx.i, 1, "unexpected character `%c`", c);
            else
                lex_error(&lx, lx.i, 1, "unexpected control character 0x%02X", c);
            lex_push(&lx, TK_ERROR, lx.i, 1);
            lx.i++;
        }
    }
    lx.doc_pending = 0; /* a trailing doc block documents nothing */
    lex_push(&lx, TK_EOF, lx.n, 0);

    out->ntoks = (uint32_t)lx.toks.len;
    out->toks = (Token *)arena_memdup(a, lx.toks.data, lx.toks.len * sizeof(Token));
    out->ncomments = (uint32_t)lx.comments.len;
    out->comments = lx.comments.len
                        ? (Comment *)arena_memdup(a, lx.comments.data, lx.comments.len * sizeof(Comment))
                        : NULL;
    out->ndocs = (uint32_t)lx.docs.len;
    out->docs = lx.docs.len ? (TokDoc *)arena_memdup(a, lx.docs.data, lx.docs.len * sizeof(TokDoc)) : NULL;
    out->ndiags = (uint32_t)lx.diags.len;
    out->diags = lx.diags.len ? (LexDiag *)arena_memdup(a, lx.diags.data, lx.diags.len * sizeof(LexDiag)) : NULL;
    out->nerrors = (int)lx.diags.len;
    vec_free(lx.diags);
    lex_current = NULL;
    vec_free(lx.toks);
    vec_free(lx.comments);
    vec_free(lx.docs);
    sb_free(&lx.doc_text);
}
