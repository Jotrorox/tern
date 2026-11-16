/* parser.c — recursive-descent parser with Pratt expressions.
 *
 * See docs/grammar.md for the grammar as implemented, including the newline
 * rules (newlines are a flag on tokens; ParseCtx.nl_ignore says whether they
 * are significant) and the disambiguation rules (speculative generic
 * arguments, struct literals in condition heads, `in`/`out` as names).
 */
#include "parser.h"
#include "lexer.h"
#include "../base/str.h"
#include "../base/vec.h"

/* -------------------------------------------------------------- state */

typedef struct ParseCtx {
    int nl_ignore;        /* > 0: line breaks are insignificant (inside (), [], <>) */
    int no_struct;        /* `Path {` is not a struct literal (if/match/while/for heads, where) */
    int generic;          /* inside generic arguments/params: `>` closes, is no operator */
    int no_at;            /* `@` after a call is not an instance domain (reg init) */
    int temporal;         /* property/sequence body: always/never/eventually/until */
} ParseCtx;

typedef struct Parser {
    Arena *a;
    uint32_t file;
    const char *src;
    Token *toks;
    uint32_t ntoks;
    TokDoc *docs;
    LexDiag *ldiags;      /* lexical errors, reported as the parser passes them */
    uint32_t nldiags, next_ldiag;
    uint32_t pos;
    uint32_t split;       /* bytes of toks[pos] already consumed (`>>` closing two generic lists) */
    uint32_t prev_end;    /* end offset of the last consumed token */
    ParseCtx ctx;
    int spec;             /* > 0 while speculating: errors are not reported */
    int panic;            /* an error was reported; unwinding to a sync point */
    int eof_reported;     /* an unclosed `{` at end of file was reported */
    uint32_t construct_pos; /* first token of the statement/item/entry being parsed */
    int nerrors;
} Parser;

typedef struct ParseMark {
    uint32_t pos, split, prev_end;
    ParseCtx ctx;
    int panic;
    ArenaMark arena;
} ParseMark;

typedef VEC(Expr *) PsExprVec;
typedef VEC(Stmt *) PsStmtVec;
typedef VEC(Item *) PsItemVec;
typedef VEC(Field *) PsFieldVec;
typedef VEC(Pattern *) PsPatVec;
typedef VEC(Attr) PsAttrVec;
typedef VEC(AttrArg) PsAttrArgVec;
typedef VEC(GenericArg) PsGArgVec;
typedef VEC(GenericParam) PsGParamVec;
typedef VEC(PathSeg) PsSegVec;
typedef VEC(Arg) PsArgVec;
typedef VEC(FieldInit) PsFieldInitVec;
typedef VEC(FieldPat) PsFieldPatVec;
typedef VEC(MatchArm) PsArmVec;
typedef VEC(Variant *) PsVariantVec;
typedef VEC(Param) PsParamVec;
typedef VEC(RegEntry *) PsRegEntryVec;
typedef VEC(RegField *) PsRegFieldVec;
typedef VEC(UseDecl *) PsUseVec;
typedef VEC(UseTree *) PsUseTreeVec;
typedef VEC(Ident) PsIdentVec;
typedef VEC(Type *) PsTypeVec;

/* Copies a vector's contents into the arena (NULL when empty) and frees it. */
#define PS_TAKE(p, v, T, out, n)                                                           \
    do {                                                                                   \
        (n) = (uint32_t)(v).len;                                                           \
        (out) = (v).len ? (T *)arena_memdup((p)->a, (v).data, (v).len * sizeof(T)) : NULL; \
        vec_free(v);                                                                       \
    } while (0)

/* binding powers (higher binds tighter) */
#define PS_BP_RANGE 2
#define PS_BP_TEMP_BODY 3
#define PS_BP_IMPLIES 4
#define PS_BP_EVENTUALLY 5
#define PS_BP_UNTIL 6
#define PS_BP_DELAY 8
#define PS_BP_OR 10
#define PS_BP_XOR 12
#define PS_BP_AND 14
#define PS_BP_CMP 16
#define PS_BP_CONCAT 18
#define PS_BP_BITOR 20
#define PS_BP_BITXOR 22
#define PS_BP_BITAND 24
#define PS_BP_SHIFT 26
#define PS_BP_ADD 28
#define PS_BP_MUL 30
#define PS_BP_UNARY 32

static Expr *ps_expr(Parser *p);
static Expr *ps_expr_bp(Parser *p, int min_bp);
static Type *ps_type(Parser *p);
static Pattern *ps_pattern(Parser *p);
static Block *ps_block(Parser *p);
static Stmt *ps_stmt(Parser *p);
static Item *ps_item_rest(Parser *p, uint32_t start, AstDoc *doc, AttrList attrs, int is_pub);
static void ps_generic_args(Parser *p, PathSeg *seg);

/* ------------------------------------------------------------ tokens */

static Token *ps_tok(Parser *p)
{
    return &p->toks[p->pos];
}

static TokenKind ps_kind(Parser *p)
{
    Token *t = &p->toks[p->pos];
    if (p->split) {
        switch (t->kind) {
        case TK_SHR: return TK_GT;
        case TK_SHRA: return t->len - p->split == 2 ? TK_SHR : TK_GT;
        case TK_GE: return TK_ASSIGN;
        default: break;
        }
    }
    return (TokenKind)t->kind;
}

static TokenKind ps_peek(Parser *p, uint32_t k)
{
    uint32_t i = p->pos + k;
    return i < p->ntoks ? (TokenKind)p->toks[i].kind : TK_EOF;
}

/* The current token starts a new line and line breaks are significant here. */
static int ps_nl(Parser *p)
{
    return p->ctx.nl_ignore == 0 && p->split == 0 && p->toks[p->pos].nl_before;
}

static uint32_t ps_off(Parser *p)
{
    return p->toks[p->pos].off + p->split;
}

/* Reports the lexical errors located before byte offset `off`. */
static void ps_flush_lex(Parser *p, uint32_t off)
{
    while (p->next_ldiag < p->nldiags && p->ldiags[p->next_ldiag].span.off < off)
        lex_emit_diag(&p->ldiags[p->next_ldiag++]);
}

static void ps_advance(Parser *p)
{
    Token *t = &p->toks[p->pos];
    if (t->kind == TK_EOF)
        return;
    p->prev_end = t->off + t->len;
    p->split = 0;
    p->pos++;
    if (p->next_ldiag < p->nldiags)
        ps_flush_lex(p, p->toks[p->pos].off);
}

static int ps_accept(Parser *p, TokenKind k)
{
    if (ps_kind(p) == k) {
        ps_advance(p);
        return 1;
    }
    return 0;
}

static int ps_is_gt(Parser *p)
{
    TokenKind k = ps_kind(p);
    return k == TK_GT || k == TK_SHR || k == TK_SHRA || k == TK_GE;
}

/* Consumes one `>` (possibly the first character of `>>`, `>>>` or `>=`). */
static void ps_eat_gt(Parser *p)
{
    Token *t = &p->toks[p->pos];
    if (ps_kind(p) == TK_GT && (t->kind == TK_GT || t->len - p->split == 1)) {
        ps_advance(p);
        return;
    }
    p->split++;
    p->prev_end = t->off + p->split;
    if (p->split >= t->len)
        ps_advance(p);
}

static Span ps_span(Parser *p, uint32_t start)
{
    uint32_t end = p->prev_end > start ? p->prev_end : start;
    return span_make(p->file, start, end - start);
}

static Span ps_tok_span(Parser *p)
{
    Token *t = &p->toks[p->pos];
    return span_make(p->file, t->off + p->split, t->len - p->split);
}

/* Where to report "expected X, found Y": the token, or the end of the
 * previous line when the token starts a new line that ends the construct. */
static Span ps_err_span(Parser *p)
{
    if (ps_nl(p) && p->pos > 0 && p->pos != p->construct_pos)
        return span_make(p->file, p->prev_end, 0);
    return ps_tok_span(p);
}

static ParseMark ps_mark(Parser *p)
{
    ParseMark m;
    m.pos = p->pos;
    m.split = p->split;
    m.prev_end = p->prev_end;
    m.ctx = p->ctx;
    m.panic = p->panic;
    m.arena = arena_mark(p->a);
    return m;
}

static void ps_reset(Parser *p, ParseMark m)
{
    p->pos = m.pos;
    p->split = m.split;
    p->prev_end = m.prev_end;
    p->ctx = m.ctx;
    p->panic = m.panic;
    arena_rewind(p->a, m.arena);
}

/* ------------------------------------------------------------ errors */

static const char *ps_describe(Parser *p)
{
    Token *t = &p->toks[p->pos];
    TokenKind k = ps_kind(p);
    if (p->split)
        return tok_kind_name(k);
    if (ps_nl(p) && k != TK_EOF && p->pos != p->construct_pos)
        return "end of line";
    switch (k) {
    case TK_IDENT: return arena_printf(p->a, "identifier `%s`", t->str);
    case TK_INT: return arena_printf(p->a, "`%.*s`", (int)t->len, p->src + t->off);
    case TK_RESERVED: return arena_printf(p->a, "reserved word `%s`", t->str);
    default: return tok_kind_name(k);
    }
}

static int ps_verror(Parser *p, Span sp, int fatal, const char *fmt, va_list ap)
{
    if (p->spec) {
        p->panic = 1;
        return 0;
    }
    if (p->panic)
        return 0;
    ps_flush_lex(p, sp.off + 1);
    diag_verror(sp, fmt, ap);
    p->nerrors++;
    if (fatal)
        p->panic = 1;
    return 1;
}

/* Reports an error and enters panic mode (unwind to the next sync point).
 * Returns 1 when the error was actually reported (so a help line may follow). */
static int ps_error(Parser *p, Span sp, const char *fmt, ...) TERN_PRINTF(3, 4);
static int ps_error(Parser *p, Span sp, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = ps_verror(p, sp, 1, fmt, ap);
    va_end(ap);
    return r;
}

/* Reports an error but keeps parsing normally (the construct was understood). */
static int ps_error_soft(Parser *p, Span sp, const char *fmt, ...) TERN_PRINTF(3, 4);
static int ps_error_soft(Parser *p, Span sp, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = ps_verror(p, sp, 0, fmt, ap);
    va_end(ap);
    return r;
}

/* An invalid token the lexer already reported: unwind silently. */
static void ps_silent_fail(Parser *p)
{
    p->panic = 1;
}

static int ps_expect(Parser *p, TokenKind k, const char *context)
{
    if (ps_kind(p) == k) {
        ps_advance(p);
        return 1;
    }
    if (ps_kind(p) == TK_ERROR) {
        ps_silent_fail(p);
        return 0;
    }
    ps_error(p, ps_err_span(p), "expected %s%s%s, found %s", tok_kind_name(k), context ? " " : "",
             context ? context : "", ps_describe(p));
    return 0;
}

static const char *ps_reserved_help(const char *w)
{
    if (strcmp(w, "wire") == 0)
        return "a wire is `let name = expr`; a combinational variable is `var name: T`";
    if (strcmp(w, "logic") == 0)
        return "declare `let x = ...` (wire), `var x: T` (combinational) or `reg x: T = init` (flip-flop)";
    if (strcmp(w, "initial") == 0)
        return "reset values go on the register (`reg x: u8 = 0`); stimulus goes in a `test \"...\" { }` block";
    if (strcmp(w, "generate") == 0)
        return "write an ordinary `for`/`if` over compile-time values; there is no separate generate syntax";
    return NULL;
}

/* Reports the use of a reserved word (once per token). */
static void ps_reserved(Parser *p, int fatal)
{
    Token *t = ps_tok(p);
    const char *help = ps_reserved_help(t->str);
    int r;
    if (help) {
        r = fatal ? ps_error(p, ps_tok_span(p), "`%s` is SystemVerilog, not Tern", t->str)
                  : ps_error_soft(p, ps_tok_span(p), "`%s` is SystemVerilog, not Tern", t->str);
        if (r)
            diag_help("%s", help);
    } else {
        if (fatal)
            ps_error(p, ps_tok_span(p), "`%s` is a reserved word", t->str);
        else
            ps_error_soft(p, ps_tok_span(p), "`%s` is a reserved word", t->str);
    }
}

/* Expects the `}` closing the `{` at offset lbrace. At end of file only the
 * innermost unclosed brace is reported. */
static void ps_close_brace(Parser *p, uint32_t lbrace)
{
    if (ps_kind(p) == TK_RBRACE) {
        ps_advance(p);
        return;
    }
    if (ps_kind(p) == TK_EOF) {
        if (p->eof_reported) {
            p->panic = 1;
            return;
        }
        if (ps_error(p, ps_tok_span(p), "unexpected end of file: a `{` is not closed")) {
            p->eof_reported = 1;
            diag_note(span_make(p->file, lbrace, 1), "unclosed `{` opens here");
        }
        return;
    }
    if (ps_kind(p) == TK_ERROR) {
        ps_silent_fail(p);
        return;
    }
    if (ps_error(p, ps_err_span(p), "expected `}`, found %s", ps_describe(p)))
        diag_note(span_make(p->file, lbrace, 1), "unclosed `{` opens here");
}

/* ------------------------------------------------------------ helpers */

static Expr *ps_new_expr(Parser *p, ExprKind k, uint32_t start)
{
    Expr *e = ANEW(p->a, Expr);
    e->kind = k;
    e->span = span_make(p->file, start, 0);
    return e;
}

static Expr *ps_done(Parser *p, Expr *e, uint32_t start)
{
    e->span = ps_span(p, start);
    return e;
}

static Expr *ps_error_expr(Parser *p, uint32_t start)
{
    Expr *e = ps_new_expr(p, EXPR_ERROR, start);
    e->span = ps_span(p, start);
    return e;
}

static Type *ps_error_type(Parser *p, uint32_t start)
{
    Type *t = ANEW(p->a, Type);
    t->kind = TYPE_ERROR;
    t->span = ps_span(p, start);
    return t;
}

static Stmt *ps_new_stmt(Parser *p, StmtKind k)
{
    Stmt *s = ANEW(p->a, Stmt);
    s->kind = k;
    return s;
}

static int ps_is_name_tok(TokenKind k)
{
    return k == TK_IDENT || k == TK_KW_IN || k == TK_KW_OUT;
}

/* Identifier, keyword or reserved word: anything spelled like a name.
 * Keywords are accepted where no keyword reading is possible (path segments
 * after `::`, package paths: std::mem, std::sim). */
static int ps_is_word_tok(TokenKind k)
{
    const char *t = tok_kind_text(k);
    return k == TK_IDENT || k == TK_RESERVED || (t && tern_is_alpha((unsigned char)t[0]));
}

static Ident ps_ident_here(Parser *p)
{
    Ident id;
    Token *t = ps_tok(p);
    id.name = t->str ? t->str : intern_cstr(tok_kind_text((TokenKind)t->kind));
    id.span = ps_tok_span(p);
    return id;
}

/* Expects a name (identifier; `in`/`out` too when allow_inout). */
static Ident ps_name(Parser *p, const char *what, int allow_inout)
{
    Ident id;
    TokenKind k = ps_kind(p);
    memset(&id, 0, sizeof id);
    if (k == TK_IDENT || (allow_inout && (k == TK_KW_IN || k == TK_KW_OUT))) {
        id = ps_ident_here(p);
        ps_advance(p);
        return id;
    }
    id.span = ps_tok_span(p);
    if (k == TK_RESERVED) {
        ps_reserved(p, 1);
    } else if (k == TK_ERROR) {
        ps_silent_fail(p);
    } else if (tok_kind_text(k) && tern_is_alpha((unsigned char)tok_kind_text(k)[0])) {
        if (ps_error(p, ps_err_span(p), "expected %s, found %s", what, ps_describe(p)))
            diag_help("`%s` is a keyword and cannot be used as a name", tok_kind_text(k));
    } else {
        ps_error(p, ps_err_span(p), "expected %s, found %s", what, ps_describe(p));
    }
    return id;
}

/* A path segment in `package`/`use` paths: any word (std::mem, std::sim). */
static Ident ps_word_name(Parser *p, const char *what)
{
    if (ps_is_word_tok(ps_kind(p))) {
        Ident id = ps_ident_here(p);
        ps_advance(p);
        return id;
    }
    return ps_name(p, what, 0);
}

static AstDoc *ps_doc_of(Parser *p, Token *t)
{
    AstDoc *d;
    if (t->doc < 0)
        return NULL;
    d = ANEW(p->a, AstDoc);
    d->text = p->docs[t->doc].text;
    d->span = p->docs[t->doc].span;
    return d;
}

/* Is the current token the end of a statement (newline, `;`, `}`, EOF)? */
static int ps_at_stmt_end(Parser *p)
{
    TokenKind k = ps_kind(p);
    return k == TK_SEMI || k == TK_RBRACE || k == TK_EOF || ps_nl(p);
}

/* ------------------------------------------------------------ attributes */

static void ps_attr(Parser *p, PsAttrVec *out)
{
    Attr at;
    uint32_t start = ps_off(p);
    ParseCtx saved = p->ctx;
    memset(&at, 0, sizeof at);
    ps_advance(p); /* #[ */
    p->ctx.nl_ignore++;
    p->ctx.generic = 0;
    p->ctx.no_struct = 0;
    {
        TokenKind k = ps_kind(p);
        if (k == TK_IDENT || (tok_kind_text(k) && tern_is_alpha((unsigned char)tok_kind_text(k)[0])) ||
            k == TK_RESERVED) {
            at.name = ps_ident_here(p);
            ps_advance(p);
        } else {
            ps_error(p, ps_err_span(p), "expected an attribute name after `#[`, found %s", ps_describe(p));
        }
    }
    if (!p->panic && ps_kind(p) == TK_LPAREN) {
        PsAttrArgVec args = {0};
        at.has_args = 1;
        ps_advance(p);
        while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
            AttrArg arg;
            uint32_t astart = ps_off(p);
            memset(&arg, 0, sizeof arg);
            if (ps_is_name_tok(ps_kind(p)) && ps_peek(p, 1) == TK_COLON) {
                arg.name = ps_ident_here(p);
                ps_advance(p);
                ps_advance(p);
            }
            arg.value = ps_expr(p);
            arg.span = ps_span(p, astart);
            vec_push(args, arg);
            if (!ps_accept(p, TK_COMMA))
                break;
        }
        PS_TAKE(p, args, AttrArg, at.args, at.nargs);
        if (!p->panic)
            ps_expect(p, TK_RPAREN, "to close the attribute arguments");
    }
    if (!p->panic)
        ps_expect(p, TK_RBRACKET, "to close the attribute");
    p->ctx = saved;
    at.span = ps_span(p, start);
    vec_push(*out, at);
}

/* Attributes before a declaration; picks up a doc comment on the first token
 * or right after the attributes. */
static void ps_prefix(Parser *p, AttrList *attrs, AstDoc **doc)
{
    PsAttrVec v = {0};
    if (!*doc)
        *doc = ps_doc_of(p, ps_tok(p));
    while (!p->panic && ps_kind(p) == TK_ATTR) {
        ps_attr(p, &v);
        if (!*doc)
            *doc = ps_doc_of(p, ps_tok(p));
    }
    PS_TAKE(p, v, Attr, attrs->items, attrs->n);
}

/* ------------------------------------------------------------ literals */

static void ps_int_value(Expr *e, const char *text)
{
    const char *s = text;
    uint64_t v = 0;
    int base = 10, overflow = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        base = 8;
        s += 2;
    }
    for (; *s; s++) {
        int d;
        if (*s == '_')
            continue;
        d = tern_hex_value((unsigned char)*s);
        if (d < 0 || d >= base)
            break;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
            overflow = 1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    e->u.int_lit.text = text;
    e->u.int_lit.value = overflow ? 0 : v;
    e->u.int_lit.overflow = overflow;
    e->u.int_lit.base = base;
}

static Expr *ps_int_lit(Parser *p)
{
    Token *t = ps_tok(p);
    uint32_t start = ps_off(p);
    Expr *e = ps_new_expr(p, EXPR_INT, start);
    ps_int_value(e, t->str);
    ps_advance(p);
    if (t->suffix) {
        e->u.int_lit.suffix = (char)t->suffix;
        if (t->width_len) {
            Expr *w = ps_new_expr(p, EXPR_INT, t->width_off);
            ps_int_value(w, arena_strndup(p->a, p->src + t->width_off, t->width_len));
            w->span = span_make(p->file, t->width_off, t->width_len);
            e->u.int_lit.width = w;
        } else {
            /* 0xFFu<W> */
            ParseCtx saved = p->ctx;
            if (ps_kind(p) != TK_LT || p->toks[p->pos].off != p->prev_end) {
                ps_error(p, span_make(p->file, start, p->prev_end - start),
                         "typed literal needs a width after `%c`", t->suffix);
                return ps_done(p, e, start);
            }
            ps_advance(p);
            p->ctx.nl_ignore++;
            p->ctx.generic = 1;
            p->ctx.no_struct = 0;
            e->u.int_lit.width = ps_expr(p);
            p->ctx = saved;
            if (!p->panic) {
                if (ps_is_gt(p))
                    ps_eat_gt(p);
                else
                    ps_error(p, ps_err_span(p), "expected `>` after the literal's width, found %s",
                             ps_describe(p));
            }
        }
    }
    return ps_done(p, e, start);
}

/* ------------------------------------------------------------ paths */

typedef enum PsPathMode { PS_PATH_EXPR, PS_PATH_TYPE } PsPathMode;

/* Can the token after a speculatively parsed `<...>` follow a generic list? */
static int ps_generic_follows(Parser *p)
{
    TokenKind k = ps_kind(p);
    if (k == TK_COLONCOLON)
        return 1;
    if (ps_nl(p))
        return 0;
    if (k == TK_LPAREN)
        return 1;
    if (k == TK_LBRACE && !p->ctx.no_struct)
        return 1;
    return 0;
}

/* Parses a path a::b<..>::c. Generic arguments are always parsed in type
 * position and inside generic lists; in expressions they are speculative. */
static void ps_path(Parser *p, Path *out, PsPathMode mode)
{
    PsSegVec segs = {0};
    uint32_t start = ps_off(p);
    for (;;) {
        PathSeg seg;
        TokenKind k = ps_kind(p);
        memset(&seg, 0, sizeof seg);
        if (ps_is_name_tok(k) || k == TK_RESERVED || (segs.len > 0 && ps_is_word_tok(k))) {
            if (k == TK_RESERVED && segs.len == 0) {
                /* reset<async, low>: `async` is reserved but names a reset style */
                if (!(p->ctx.generic && strcmp(ps_tok(p)->str, "async") == 0))
                    ps_reserved(p, 0);
            }
            seg.name = ps_ident_here(p);
            ps_advance(p);
        } else {
            seg.name = ps_name(p, "a name", 1);
            seg.span = seg.name.span;
            vec_push(segs, seg);
            break;
        }
        if (ps_kind(p) == TK_LT && !ps_nl(p)) {
            if (mode == PS_PATH_TYPE || p->ctx.generic) {
                ps_generic_args(p, &seg);
            } else {
                ParseMark m = ps_mark(p);
                int ok;
                p->spec++;
                ps_generic_args(p, &seg);
                p->spec--;
                ok = !p->panic && ps_generic_follows(p);
                if (!ok) {
                    ps_reset(p, m);
                    seg.args = NULL;
                    seg.nargs = 0;
                    seg.has_args = 0;
                }
            }
        }
        seg.span = ps_span(p, seg.name.span.off);
        vec_push(segs, seg);
        if (p->panic)
            break;
        if (ps_kind(p) == TK_COLONCOLON && ps_is_word_tok(ps_peek(p, 1))) {
            ps_advance(p);
            continue;
        }
        break;
    }
    PS_TAKE(p, segs, PathSeg, out->segs, out->nsegs);
    out->span = ps_span(p, start);
}

static void ps_generic_args(Parser *p, PathSeg *seg)
{
    PsGArgVec args = {0};
    ParseCtx saved = p->ctx;
    ps_advance(p); /* < */
    seg->has_args = 1;
    p->ctx.nl_ignore++;
    p->ctx.generic = 1;
    p->ctx.no_struct = 0;
    p->ctx.no_at = 0;
    while (!p->panic && !ps_is_gt(p) && ps_kind(p) != TK_EOF) {
        GenericArg ga;
        uint32_t astart = ps_off(p);
        memset(&ga, 0, sizeof ga);
        if (ps_is_name_tok(ps_kind(p)) && ps_peek(p, 1) == TK_COLON) {
            ga.name = ps_ident_here(p);
            ps_advance(p);
            ps_advance(p);
        }
        ga.expr = ps_expr(p);
        ga.type = ast_type_from_expr(p->a, ga.expr);
        ga.span = ps_span(p, astart);
        vec_push(args, ga);
        if (!ps_accept(p, TK_COMMA))
            break;
    }
    PS_TAKE(p, args, GenericArg, seg->args, seg->nargs);
    p->ctx = saved;
    if (p->panic)
        return;
    if (ps_is_gt(p))
        ps_eat_gt(p);
    else
        ps_error(p, ps_err_span(p), "expected `,` or `>` in generic arguments, found %s", ps_describe(p));
}

/* ------------------------------------------------------------ types */

static Type *ps_path_to_type(Parser *p, Path path)
{
    Type *t = ANEW(p->a, Type);
    t->kind = TYPE_PATH;
    t->u.path = path;
    t->span = path.span;
    return t;
}

Type *ast_type_from_expr(Arena *a, Expr *e)
{
    Type *t;
    if (!e)
        return NULL;
    switch (e->kind) {
    case EXPR_PATH:
        t = ANEW(a, Type);
        t->kind = TYPE_PATH;
        t->u.path = e->u.path;
        t->span = e->span;
        return t;
    case EXPR_TYPE:
        return e->u.type;
    case EXPR_PAREN:
        return ast_type_from_expr(a, e->u.inner);
    case EXPR_CALL:
        if (e->u.call.callee->kind == EXPR_PATH && e->u.call.callee->u.path.nsegs == 1 &&
            !e->u.call.callee->u.path.segs[0].has_args &&
            strcmp(e->u.call.callee->u.path.segs[0].name.name, "typeof") == 0 && e->u.call.nargs == 1 &&
            !e->u.call.args[0].name.name && !e->u.call.domain) {
            t = ANEW(a, Type);
            t->kind = TYPE_TYPEOF;
            t->u.typeof_expr = e->u.call.args[0].value;
            t->span = e->span;
            return t;
        }
        return NULL;
    case EXPR_TUPLE: {
        uint32_t i;
        Type **elems;
        if (e->u.list.n == 0)
            return NULL;
        elems = ANEWN(a, Type *, e->u.list.n);
        for (i = 0; i < e->u.list.n; i++) {
            elems[i] = ast_type_from_expr(a, e->u.list.items[i]);
            if (!elems[i])
                return NULL;
        }
        t = ANEW(a, Type);
        t->kind = TYPE_TUPLE;
        t->u.tuple.elems = elems;
        t->u.tuple.n = e->u.list.n;
        t->span = e->span;
        return t;
    }
    default:
        return NULL;
    }
}

static void ps_field_list(Parser *p, PsFieldVec *fields, PsItemVec *fns, int allow_flip, const char *what);

static Type *ps_type(Parser *p)
{
    uint32_t start = ps_off(p);
    TokenKind k = ps_kind(p);
    Type *t;
    if (k == TK_LBRACKET) {
        ParseCtx saved = p->ctx;
        t = ANEW(p->a, Type);
        t->kind = TYPE_ARRAY;
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.generic = 0;
        p->ctx.no_struct = 0;
        t->u.array.elem = ps_type(p);
        if (!p->panic && ps_expect(p, TK_SEMI, "in array type `[T; N]`"))
            t->u.array.len = ps_expr(p);
        else
            t->u.array.len = ps_error_expr(p, ps_off(p));
        if (!p->panic)
            ps_expect(p, TK_RBRACKET, "to close the array type");
        p->ctx = saved;
        t->span = ps_span(p, start);
        return t;
    }
    if (k == TK_KW_STRUCT) {
        PsFieldVec fields = {0};
        t = ANEW(p->a, Type);
        t->kind = TYPE_STRUCT;
        ps_advance(p);
        ps_field_list(p, &fields, NULL, 0, "anonymous struct");
        PS_TAKE(p, fields, Field *, t->u.anon_struct.fields, t->u.anon_struct.nfields);
        t->span = ps_span(p, start);
        return t;
    }
    if (k == TK_LPAREN) {
        ParseCtx saved = p->ctx;
        PsTypeVec elems = {0};
        int comma = 0;
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.generic = 0;
        while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
            vec_push(elems, ps_type(p));
            if (!ps_accept(p, TK_COMMA))
                break;
            comma = 1;
        }
        if (!p->panic)
            ps_expect(p, TK_RPAREN, "to close the tuple type");
        p->ctx = saved;
        if (elems.len == 1 && !comma) {
            t = elems.data[0];
            vec_free(elems);
            return t;
        }
        t = ANEW(p->a, Type);
        t->kind = TYPE_TUPLE;
        PS_TAKE(p, elems, Type *, t->u.tuple.elems, t->u.tuple.n);
        t->span = ps_span(p, start);
        return t;
    }
    if (k == TK_IDENT && strcmp(ps_tok(p)->str, "typeof") == 0 && ps_peek(p, 1) == TK_LPAREN) {
        ParseCtx saved = p->ctx;
        t = ANEW(p->a, Type);
        t->kind = TYPE_TYPEOF;
        ps_advance(p);
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.generic = 0;
        p->ctx.no_struct = 0;
        t->u.typeof_expr = ps_expr(p);
        if (!p->panic)
            ps_expect(p, TK_RPAREN, "to close `typeof(`");
        p->ctx = saved;
        t->span = ps_span(p, start);
        return t;
    }
    if (k == TK_IDENT) {
        Path path;
        ps_path(p, &path, PS_PATH_TYPE);
        return ps_path_to_type(p, path);
    }
    if (k == TK_RESERVED) {
        ps_reserved(p, 1);
        return ps_error_type(p, start);
    }
    if (k == TK_ERROR) {
        ps_silent_fail(p);
        return ps_error_type(p, start);
    }
    ps_error(p, ps_err_span(p), "expected a type, found %s", ps_describe(p));
    return ps_error_type(p, start);
}

/* ------------------------------------------------------------ expressions */

static Expr *ps_paren(Parser *p)
{
    uint32_t start = ps_off(p);
    ParseCtx saved = p->ctx;
    PsExprVec items = {0};
    int comma = 0;
    Expr *e;
    ps_advance(p);
    p->ctx.nl_ignore++;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
        vec_push(items, ps_expr(p));
        if (!ps_accept(p, TK_COMMA))
            break;
        comma = 1;
    }
    if (!p->panic)
        ps_expect(p, TK_RPAREN, items.len > 1 ? "or `,` in tuple" : "to close `(`");
    p->ctx = saved;
    if (items.len == 1 && !comma) {
        e = ps_new_expr(p, EXPR_PAREN, start);
        e->u.inner = items.data[0];
        vec_free(items);
        return ps_done(p, e, start);
    }
    e = ps_new_expr(p, EXPR_TUPLE, start);
    PS_TAKE(p, items, Expr *, e->u.list.items, e->u.list.n);
    return ps_done(p, e, start);
}

static Expr *ps_array(Parser *p)
{
    uint32_t start = ps_off(p);
    ParseCtx saved = p->ctx;
    PsExprVec items = {0};
    Expr *e;
    ps_advance(p);
    p->ctx.nl_ignore++;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (!p->panic && ps_kind(p) != TK_RBRACKET && ps_kind(p) != TK_EOF) {
        Expr *x = ps_expr(p);
        if (items.len == 0 && ps_kind(p) == TK_SEMI && !p->panic) {
            /* [T; N] in expression position (generic arguments) */
            Type *elem = ast_type_from_expr(p->a, x);
            Type *t;
            uint32_t semi = ps_off(p);
            if (!elem) {
                if (ps_error(p, x->span, "expected a type before `;` in `[T; N]`"))
                    diag_help("array repeat expressions `[x; n]` do not exist; use `all0`/`all1` or a const fn");
                elem = ps_error_type(p, start);
            }
            TERN_UNUSED(semi);
            ps_advance(p);
            t = ANEW(p->a, Type);
            t->kind = TYPE_ARRAY;
            t->u.array.elem = elem;
            t->u.array.len = ps_expr(p);
            if (!p->panic)
                ps_expect(p, TK_RBRACKET, "to close the array type");
            p->ctx = saved;
            t->span = ps_span(p, start);
            vec_free(items);
            e = ps_new_expr(p, EXPR_TYPE, start);
            e->u.type = t;
            return ps_done(p, e, start);
        }
        vec_push(items, x);
        if (!ps_accept(p, TK_COMMA))
            break;
    }
    if (!p->panic)
        ps_expect(p, TK_RBRACKET, "or `,` in array literal");
    p->ctx = saved;
    e = ps_new_expr(p, EXPR_ARRAY, start);
    PS_TAKE(p, items, Expr *, e->u.list.items, e->u.list.n);
    return ps_done(p, e, start);
}

/* Separator in brace-delimited lists: `,`, `;` or a line break. Returns 0 at `}`. */
static int ps_list_sep(Parser *p, const char *what)
{
    TokenKind k = ps_kind(p);
    if (k == TK_RBRACE || k == TK_EOF)
        return 0;
    if (k == TK_COMMA || k == TK_SEMI) {
        ps_advance(p);
        return 1;
    }
    if (ps_tok(p)->nl_before && !p->split)
        return 1;
    if (k == TK_ERROR) {
        ps_silent_fail(p);
        return 0;
    }
    ps_error(p, ps_err_span(p), "expected `,` or a new line between %s, found %s", what, ps_describe(p));
    return 0;
}

/* Skips to the next list element after an error inside a brace list. */
static void ps_sync_list(Parser *p)
{
    int depth = 0;
    p->panic = 0;
    while (ps_kind(p) != TK_EOF) {
        TokenKind k = ps_kind(p);
        if (depth == 0 && (k == TK_RBRACE))
            return;
        if (depth == 0 && k == TK_COMMA) {
            ps_advance(p);
            return;
        }
        if (k == TK_LBRACE || k == TK_LPAREN || k == TK_LBRACKET)
            depth++;
        else if (k == TK_RBRACE || k == TK_RPAREN || k == TK_RBRACKET) {
            if (depth > 0)
                depth--;
        }
        ps_advance(p);
        if (depth == 0 && ps_tok(p)->nl_before)
            return;
    }
}

/* `{ name: value, name, .. }` after a struct path or `.Variant`. */
static void ps_field_inits(Parser *p, FieldInit **out, uint32_t *n, int *has_rest)
{
    PsFieldInitVec v = {0};
    ParseCtx saved = p->ctx;
    ps_advance(p); /* { */
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        FieldInit fi;
        uint32_t fstart = ps_off(p);
        uint32_t before = p->pos;
        memset(&fi, 0, sizeof fi);
        if (ps_kind(p) == TK_DOTDOT) {
            ps_advance(p);
            if (has_rest)
                *has_rest = 1;
            else
                ps_error_soft(p, span_make(p->file, fstart, 2), "`..` is not allowed in this literal");
        } else {
            fi.name = ps_name(p, "a field name", 1);
            if (!p->panic && ps_accept(p, TK_COLON))
                fi.value = ps_expr(p);
            fi.span = ps_span(p, fstart);
            if (fi.name.name)
                vec_push(v, fi);
        }
        if (!p->panic && !ps_list_sep(p, "fields"))
            break;
        if (p->panic) {
            ps_sync_list(p);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    PS_TAKE(p, v, FieldInit, *out, *n);
    p->ctx = saved;
    if (!p->panic)
        ps_expect(p, TK_RBRACE, "to close the literal");
}

static Expr *ps_if_expr(Parser *p)
{
    uint32_t start = ps_off(p);
    Expr *e = ps_new_expr(p, EXPR_IF, start);
    ParseCtx saved = p->ctx;
    ps_advance(p); /* if */
    p->ctx.no_struct = 1;
    p->ctx.generic = 0;
    e->u.if_expr.cond = ps_expr(p);
    p->ctx = saved;
    e->u.if_expr.then_block = ps_block(p);
    if (!p->panic && ps_kind(p) == TK_KW_ELSE) {
        uint32_t estart;
        ps_advance(p);
        estart = ps_off(p);
        if (ps_kind(p) == TK_KW_IF) {
            e->u.if_expr.else_expr = ps_if_expr(p);
        } else {
            Expr *b = ps_new_expr(p, EXPR_BLOCK, estart);
            b->u.block = ps_block(p);
            e->u.if_expr.else_expr = ps_done(p, b, estart);
        }
    }
    return ps_done(p, e, start);
}

static void ps_sync_stmt(Parser *p, uint32_t start_pos);

static Expr *ps_match_expr(Parser *p)
{
    uint32_t start = ps_off(p);
    Expr *e = ps_new_expr(p, EXPR_MATCH, start);
    ParseCtx saved = p->ctx;
    PsArmVec arms = {0};
    uint32_t lbrace;
    ps_advance(p); /* match */
    p->ctx.no_struct = 1;
    p->ctx.generic = 0;
    e->u.match.scrutinee = ps_expr(p);
    p->ctx = saved;
    if (p->panic)
        return ps_done(p, e, start);
    lbrace = ps_off(p);
    if (!ps_expect(p, TK_LBRACE, "to open the match arms"))
        return ps_done(p, e, start);
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        MatchArm arm;
        uint32_t astart = ps_off(p), before = p->pos;
        memset(&arm, 0, sizeof arm);
        p->construct_pos = p->pos;
        arm.pat = ps_pattern(p);
        if (!p->panic && ps_kind(p) != TK_FAT_ARROW) {
            if (ps_kind(p) == TK_COLON) {
                if (ps_error(p, ps_err_span(p), "expected `=>` after match pattern, found `:`"))
                    diag_help("`case` labels are SystemVerilog; write `pattern => value,`");
            } else if (ps_kind(p) == TK_ERROR) {
                ps_silent_fail(p);
            } else {
                ps_error(p, ps_err_span(p), "expected `=>` after match pattern, found %s", ps_describe(p));
            }
        }
        if (!p->panic) {
            ps_advance(p);
            arm.body = ps_stmt(p);
        } else {
            arm.body = ps_new_stmt(p, STMT_ERROR);
            arm.body->span = ps_span(p, astart);
        }
        arm.span = ps_span(p, astart);
        vec_push(arms, arm);
        if (!p->panic) {
            TokenKind k = ps_kind(p);
            if (k == TK_COMMA)
                ps_advance(p);
            else if (k == TK_RBRACE || ps_tok(p)->nl_before)
                ;
            else if (k == TK_ERROR)
                ps_silent_fail(p);
            else
                ps_error(p, ps_err_span(p), "expected `,` or a new line after match arm, found %s",
                         ps_describe(p));
        }
        if (p->panic) {
            ps_sync_list(p);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    PS_TAKE(p, arms, MatchArm, e->u.match.arms, e->u.match.narms);
    p->ctx = saved;
    if (ps_kind(p) != TK_RBRACE) {
        if (ps_error(p, ps_err_span(p), "expected `}` to close the match, found %s", ps_describe(p)))
            diag_note(span_make(p->file, lbrace, 1), "the match arms open here");
    } else {
        ps_advance(p);
    }
    return ps_done(p, e, start);
}

static Expr *ps_struct_lit(Parser *p, Expr *path_expr, uint32_t start)
{
    Expr *e = ps_new_expr(p, EXPR_STRUCT, start);
    e->u.struct_lit.path = path_expr->u.path;
    ps_field_inits(p, &e->u.struct_lit.fields, &e->u.struct_lit.nfields, &e->u.struct_lit.has_rest);
    return ps_done(p, e, start);
}

static Expr *ps_call_args(Parser *p, Expr *callee, uint32_t start)
{
    Expr *e = ps_new_expr(p, EXPR_CALL, start);
    ParseCtx saved = p->ctx;
    PsArgVec args = {0};
    e->u.call.callee = callee;
    ps_advance(p); /* ( */
    p->ctx.nl_ignore++;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
        Arg a;
        uint32_t astart = ps_off(p);
        memset(&a, 0, sizeof a);
        if (ps_is_name_tok(ps_kind(p)) && ps_peek(p, 1) == TK_COLON) {
            a.name = ps_ident_here(p);
            ps_advance(p);
            ps_advance(p);
        }
        a.value = ps_expr(p);
        a.span = ps_span(p, astart);
        vec_push(args, a);
        if (!ps_accept(p, TK_COMMA))
            break;
    }
    PS_TAKE(p, args, Arg, e->u.call.args, e->u.call.nargs);
    if (!p->panic)
        ps_expect(p, TK_RPAREN, "or `,` in argument list");
    p->ctx = saved;
    if (!p->panic && ps_kind(p) == TK_AT && !p->ctx.no_at && !ps_nl(p)) {
        ParseCtx s2 = p->ctx;
        ps_advance(p);
        p->ctx.no_at = 1;
        p->ctx.no_struct = 1;
        e->u.call.domain = ps_expr_bp(p, PS_BP_UNARY);
        p->ctx = s2;
    }
    return ps_done(p, e, start);
}

static Expr *ps_postfix(Parser *p, Expr *e, uint32_t start)
{
    while (!p->panic) {
        TokenKind k = ps_kind(p);
        if (ps_nl(p))
            break;
        if (k == TK_DOT) {
            Expr *f = ps_new_expr(p, EXPR_FIELD, start);
            ps_advance(p);
            f->u.field.base = e;
            if (ps_kind(p) == TK_INT && !ps_tok(p)->suffix) {
                f->u.field.name.name = intern_cstr(ps_tok(p)->str);
                f->u.field.name.span = ps_tok_span(p);
                ps_advance(p);
            } else {
                f->u.field.name = ps_name(p, "a field name after `.`", 1);
            }
            if (!p->panic && ps_kind(p) == TK_LT && !ps_nl(p)) {
                /* method generics: x.trunc<8>() */
                PathSeg seg;
                ParseMark m = ps_mark(p);
                memset(&seg, 0, sizeof seg);
                p->spec++;
                ps_generic_args(p, &seg);
                p->spec--;
                if (!p->panic && ps_kind(p) == TK_LPAREN && !ps_nl(p)) {
                    f->u.field.has_args = 1;
                    f->u.field.args = seg.args;
                    f->u.field.nargs = seg.nargs;
                } else {
                    ps_reset(p, m);
                }
            }
            e = ps_done(p, f, start);
        } else if (k == TK_LBRACKET) {
            ParseCtx saved = p->ctx;
            Expr *x;
            ps_advance(p);
            p->ctx.nl_ignore++;
            p->ctx.no_struct = 0;
            p->ctx.generic = 0;
            p->ctx.no_at = 0;
            x = ps_expr(p);
            if (!p->panic && ps_kind(p) == TK_COLON) {
                Expr *s = ps_new_expr(p, EXPR_SLICE, start);
                ps_advance(p);
                s->u.slice.base = e;
                s->u.slice.hi = x;
                s->u.slice.lo = ps_expr(p);
                if (!p->panic && (ps_kind(p) == TK_PLUS || ps_kind(p) == TK_MINUS) && ps_peek(p, 1) == TK_COLON) {
                    if (ps_error(p, ps_tok_span(p), "`%s:` part-selects are SystemVerilog", ps_kind(p) == TK_PLUS ? "+" : "-"))
                        diag_help("write the slice with both bounds: `x[hi:lo]`");
                }
                if (!p->panic)
                    ps_expect(p, TK_RBRACKET, "to close the slice");
                p->ctx = saved;
                e = ps_done(p, s, start);
            } else {
                Expr *ix = ps_new_expr(p, EXPR_INDEX, start);
                ix->u.index.base = e;
                ix->u.index.index = x;
                if (!p->panic && (ps_kind(p) == TK_PLUS || ps_kind(p) == TK_MINUS) && ps_peek(p, 1) == TK_COLON) {
                    if (ps_error(p, ps_tok_span(p), "`%s:` part-selects are SystemVerilog", ps_kind(p) == TK_PLUS ? "+" : "-"))
                        diag_help("write the slice with both bounds: `x[hi:lo]`");
                }
                if (!p->panic)
                    ps_expect(p, TK_RBRACKET, "or `:` in index");
                p->ctx = saved;
                e = ps_done(p, ix, start);
            }
        } else if (k == TK_LPAREN) {
            e = ps_call_args(p, e, start);
        } else if (k == TK_KW_AS) {
            Expr *c = ps_new_expr(p, EXPR_CAST, start);
            ps_advance(p);
            c->u.cast.expr = e;
            c->u.cast.type = ps_type(p);
            e = ps_done(p, c, start);
        } else if (k == TK_LBRACE && e->kind == EXPR_PATH && !p->ctx.no_struct) {
            e = ps_struct_lit(p, e, start);
        } else {
            break;
        }
    }
    return e;
}

static Expr *ps_temporal_prefix(Parser *p, TemporalOp op)
{
    uint32_t start = ps_off(p);
    Expr *e = ps_new_expr(p, EXPR_TEMPORAL, start);
    ps_advance(p);
    e->u.temporal.op = op;
    if (op == TEMP_EVENTUALLY && ps_kind(p) == TK_LBRACKET && !ps_nl(p)) {
        ParseCtx saved = p->ctx;
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.generic = 0;
        p->ctx.no_struct = 0;
        if (ps_expect(p, TK_DOTDOT, "in `eventually[..n]`")) {
            e->u.temporal.bound = ps_expr(p);
            if (!p->panic)
                ps_expect(p, TK_RBRACKET, "to close `eventually[..n]`");
        }
        p->ctx = saved;
    }
    e->u.temporal.body = ps_expr_bp(p, op == TEMP_EVENTUALLY ? PS_BP_EVENTUALLY : PS_BP_TEMP_BODY);
    return ps_done(p, e, start);
}

/* `##n` / `##[a:b]` / `##[a:$]` after the `##` token. */
static void ps_delay_spec(Parser *p, Expr *e)
{
    if (ps_kind(p) == TK_LBRACKET) {
        ParseCtx saved = p->ctx;
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.generic = 0;
        p->ctx.no_struct = 0;
        e->u.delay.ranged = 1;
        e->u.delay.lo = ps_expr(p);
        if (!p->panic && ps_expect(p, TK_COLON, "in delay range `##[a:b]`")) {
            if (ps_kind(p) == TK_DOLLAR) {
                ps_advance(p);
                e->u.delay.unbounded = 1;
            } else {
                e->u.delay.hi = ps_expr(p);
            }
            if (!p->panic)
                ps_expect(p, TK_RBRACKET, "to close the delay range");
        }
        p->ctx = saved;
    } else {
        /* ##n: a literal, a name or a parenthesized expression (no postfix:
         * in `a ##1 (b)` the parenthesis is the operand, not a call) */
        uint32_t start = ps_off(p);
        TokenKind k = ps_kind(p);
        if (k == TK_INT) {
            e->u.delay.lo = ps_int_lit(p);
        } else if (k == TK_LPAREN) {
            e->u.delay.lo = ps_paren(p);
        } else if (ps_is_name_tok(k)) {
            Expr *x = ps_new_expr(p, EXPR_PATH, start);
            ps_path(p, &x->u.path, PS_PATH_EXPR);
            e->u.delay.lo = ps_done(p, x, start);
        } else {
            ps_error(p, ps_err_span(p), "expected a cycle count after `##`, found %s", ps_describe(p));
            e->u.delay.lo = ps_error_expr(p, start);
        }
    }
}

static Expr *ps_primary(Parser *p)
{
    uint32_t start = ps_off(p);
    TokenKind k = ps_kind(p);
    Expr *e;
    switch (k) {
    case TK_INT:
        return ps_int_lit(p);
    case TK_TRUE:
    case TK_FALSE:
        e = ps_new_expr(p, EXPR_BOOL, start);
        e->u.bool_val = k == TK_TRUE;
        ps_advance(p);
        return ps_done(p, e, start);
    case TK_ALL0:
    case TK_ALL1:
        e = ps_new_expr(p, EXPR_FILL, start);
        e->u.fill_ones = k == TK_ALL1;
        ps_advance(p);
        return ps_done(p, e, start);
    case TK_STRING:
        e = ps_new_expr(p, EXPR_STRING, start);
        e->u.string.value = ps_tok(p)->str;
        e->u.string.len = ps_tok(p)->str_len;
        ps_advance(p);
        return ps_done(p, e, start);
    case TK_IDENT:
    case TK_KW_IN:
    case TK_KW_OUT:
    case TK_RESERVED:
        e = ps_new_expr(p, EXPR_PATH, start);
        ps_path(p, &e->u.path, PS_PATH_EXPR);
        return ps_done(p, e, start);
    case TK_DOT:
        e = ps_new_expr(p, EXPR_VARIANT, start);
        ps_advance(p);
        e->u.variant.name = ps_name(p, "a variant name after `.`", 0);
        if (!p->panic && ps_kind(p) == TK_LBRACE && !p->ctx.no_struct && !ps_nl(p)) {
            e->u.variant.has_fields = 1;
            ps_field_inits(p, &e->u.variant.fields, &e->u.variant.nfields, NULL);
        }
        return ps_done(p, e, start);
    case TK_LPAREN:
        return ps_paren(p);
    case TK_LBRACKET:
        return ps_array(p);
    case TK_LBRACE:
        e = ps_new_expr(p, EXPR_BLOCK, start);
        e->u.block = ps_block(p);
        return ps_done(p, e, start);
    case TK_KW_IF:
        return ps_if_expr(p);
    case TK_KW_MATCH:
        return ps_match_expr(p);
    case TK_KW_SIM: {
        Expr *inner;
        e = ps_new_expr(p, EXPR_SIM, start);
        ps_advance(p);
        inner = ps_expr_bp(p, PS_BP_UNARY);
        if (!p->panic && inner->kind != EXPR_CALL)
            ps_error_soft(p, inner->span, "expected a module instance after `sim`, such as `sim Fifo<u8, 4>()`");
        e->u.inner = inner;
        return ps_done(p, e, start);
    }
    case TK_KW_STRUCT: {
        Type *t = ps_type(p);
        e = ps_new_expr(p, EXPR_TYPE, start);
        e->u.type = t;
        return ps_done(p, e, start);
    }
    case TK_DOTDOT:
    case TK_DOTDOTEQ:
        e = ps_new_expr(p, EXPR_RANGE, start);
        ps_advance(p);
        e->u.range.inclusive = k == TK_DOTDOTEQ;
        e->u.range.hi = ps_expr_bp(p, PS_BP_RANGE + 1);
        return ps_done(p, e, start);
    case TK_DOLLAR:
        if (ps_peek(p, 1) == TK_IDENT && p->toks[p->pos + 1].off == ps_tok(p)->off + 1) {
            const char *name = p->toks[p->pos + 1].str;
            ps_advance(p);
            if (ps_error(p, ps_span(p, start), "`$%s` is a SystemVerilog system function", name)) {
                if (strcmp(name, "clog2") == 0)
                    diag_help("Tern has `clog2(n)` (std::prelude)");
                else if (strcmp(name, "signed") == 0)
                    diag_help("write `x as sint<N>` or `widen_signed(x)`");
                else if (strcmp(name, "unsigned") == 0)
                    diag_help("write `x as uint<N>`");
                else if (strcmp(name, "display") == 0 || strcmp(name, "write") == 0)
                    diag_help("in a test, print with `log(\"...{}...\", x)`");
                else if (strcmp(name, "bits") == 0)
                    diag_help("use `T::WIDTH` or `bits(x)`");
            }
            ps_advance(p);
            return ps_error_expr(p, start);
        }
        ps_error(p, ps_tok_span(p), "unexpected `$`");
        ps_advance(p);
        return ps_error_expr(p, start);
    case TK_ERROR:
        ps_silent_fail(p);
        ps_advance(p);
        return ps_error_expr(p, start);
    default:
        break;
    }
    ps_error(p, ps_err_span(p), "expected an expression, found %s", ps_describe(p));
    return ps_error_expr(p, start);
}

static Expr *ps_unary(Parser *p)
{
    uint32_t start = ps_off(p);
    TokenKind k = ps_kind(p);
    Expr *e;
    UnaryOp op;
    switch (k) {
    case TK_MINUS: op = UN_NEG; break;
    case TK_NOT: op = UN_NOT; break;
    case TK_TILDE: op = UN_BITNOT; break;
    case TK_AMP: op = UN_RED_AND; break;
    case TK_PIPE: op = UN_RED_OR; break;
    case TK_CARET: op = UN_RED_XOR; break;
    case TK_IDENT:
        if (p->ctx.temporal) {
            const char *s = ps_tok(p)->str;
            if (strcmp(s, "always") == 0)
                return ps_temporal_prefix(p, TEMP_ALWAYS);
            if (strcmp(s, "never") == 0)
                return ps_temporal_prefix(p, TEMP_NEVER);
            if (strcmp(s, "eventually") == 0)
                return ps_temporal_prefix(p, TEMP_EVENTUALLY);
        }
        e = ps_primary(p);
        return ps_postfix(p, e, start);
    case TK_HASHHASH:
        e = ps_new_expr(p, EXPR_DELAY, start);
        if (!p->ctx.temporal)
            ps_error_soft(p, ps_tok_span(p), "`##` delays are only allowed in a `property` or `sequence`");
        ps_advance(p);
        ps_delay_spec(p, e);
        e->u.delay.rhs = ps_expr_bp(p, PS_BP_DELAY + 1);
        return ps_done(p, e, start);
    default:
        e = ps_primary(p);
        return ps_postfix(p, e, start);
    }
    e = ps_new_expr(p, EXPR_UNARY, start);
    ps_advance(p);
    e->u.unary.op = op;
    e->u.unary.operand = ps_unary(p);
    return ps_done(p, e, start);
}

/* Binary operator at the current token: returns 1 and fills op/lbp/rbp. */
static int ps_binop(Parser *p, BinaryOp *op, int *lbp, int *rbp)
{
    TokenKind k = ps_kind(p);
    int l;
    switch (k) {
    case TK_STAR: *op = BIN_MUL; l = PS_BP_MUL; break;
    case TK_SLASH: *op = BIN_DIV; l = PS_BP_MUL; break;
    case TK_PERCENT: *op = BIN_MOD; l = PS_BP_MUL; break;
    case TK_PLUS: *op = BIN_ADD; l = PS_BP_ADD; break;
    case TK_MINUS: *op = BIN_SUB; l = PS_BP_ADD; break;
    case TK_PLUS_WRAP: *op = BIN_ADD_WRAP; l = PS_BP_ADD; break;
    case TK_MINUS_WRAP: *op = BIN_SUB_WRAP; l = PS_BP_ADD; break;
    case TK_SHL: *op = BIN_SHL; l = PS_BP_SHIFT; break;
    case TK_SHR:
        if (p->ctx.generic)
            return 0;
        *op = BIN_SHR; l = PS_BP_SHIFT; break;
    case TK_SHRA:
        if (p->ctx.generic)
            return 0;
        *op = BIN_SHRA; l = PS_BP_SHIFT; break;
    case TK_AMP: *op = BIN_BITAND; l = PS_BP_BITAND; break;
    case TK_CARET: *op = BIN_BITXOR; l = PS_BP_BITXOR; break;
    case TK_PIPE: *op = BIN_BITOR; l = PS_BP_BITOR; break;
    case TK_CONCAT: *op = BIN_CONCAT; l = PS_BP_CONCAT; break;
    case TK_EQ: *op = BIN_EQ; l = PS_BP_CMP; break;
    case TK_NE: *op = BIN_NE; l = PS_BP_CMP; break;
    case TK_LT: *op = BIN_LT; l = PS_BP_CMP; break;
    case TK_LE: *op = BIN_LE; l = PS_BP_CMP; break;
    case TK_GT:
        if (p->ctx.generic)
            return 0;
        *op = BIN_GT; l = PS_BP_CMP; break;
    case TK_GE:
        if (p->ctx.generic)
            return 0;
        *op = BIN_GE; l = PS_BP_CMP; break;
    case TK_KW_IN: *op = BIN_IN; l = PS_BP_CMP; break;
    case TK_AND: *op = BIN_AND; l = PS_BP_AND; break;
    case TK_XOR: *op = BIN_XOR; l = PS_BP_XOR; break;
    case TK_OR: *op = BIN_OR; l = PS_BP_OR; break;
    case TK_IMPLIES: *op = BIN_IMPLIES; *lbp = PS_BP_IMPLIES; *rbp = PS_BP_IMPLIES; return 1;
    case TK_IMPLIES_NEXT: *op = BIN_IMPLIES_NEXT; *lbp = PS_BP_IMPLIES; *rbp = PS_BP_IMPLIES; return 1;
    case TK_IDENT:
        if (p->ctx.temporal && strcmp(ps_tok(p)->str, "until") == 0) {
            *op = BIN_UNTIL;
            l = PS_BP_UNTIL;
            break;
        }
        return 0;
    default:
        return 0;
    }
    *lbp = l;
    *rbp = l + 1;
    return 1;
}

static Expr *ps_expr_bp(Parser *p, int min_bp)
{
    uint32_t start = ps_off(p);
    Expr *lhs = ps_unary(p);
    while (!p->panic) {
        BinaryOp op;
        int lbp, rbp;
        TokenKind k = ps_kind(p);
        if (ps_nl(p))
            break;
        if (k == TK_DOTDOT || k == TK_DOTDOTEQ) {
            Expr *r;
            if (PS_BP_RANGE < min_bp)
                break;
            r = ps_new_expr(p, EXPR_RANGE, start);
            ps_advance(p);
            r->u.range.lo = lhs;
            r->u.range.inclusive = k == TK_DOTDOTEQ;
            r->u.range.hi = ps_expr_bp(p, PS_BP_RANGE + 1);
            lhs = ps_done(p, r, start);
            continue;
        }
        if (k == TK_HASHHASH) {
            Expr *d;
            if (PS_BP_DELAY < min_bp)
                break;
            d = ps_new_expr(p, EXPR_DELAY, start);
            if (!p->ctx.temporal)
                ps_error_soft(p, ps_tok_span(p), "`##` delays are only allowed in a `property` or `sequence`");
            ps_advance(p);
            d->u.delay.lhs = lhs;
            ps_delay_spec(p, d);
            d->u.delay.rhs = ps_expr_bp(p, PS_BP_DELAY + 1);
            lhs = ps_done(p, d, start);
            continue;
        }
        if (!ps_binop(p, &op, &lbp, &rbp))
            break;
        if (lbp < min_bp)
            break;
        if ((k == TK_PLUS || k == TK_MINUS) && ps_peek(p, 1) == TK_COLON)
            break; /* x[i +: w]: reported by the index parser */
        if ((op == BIN_IMPLIES || op == BIN_IMPLIES_NEXT) && !p->ctx.temporal)
            ps_error_soft(p, ps_tok_span(p), "`%s` is only allowed in a `property` or `sequence`",
                          ast_binary_op_text(op));
        {
            Expr *b = ps_new_expr(p, EXPR_BINARY, start);
            ps_advance(p);
            b->u.binary.op = op;
            b->u.binary.lhs = lhs;
            b->u.binary.rhs = ps_expr_bp(p, rbp);
            lhs = ps_done(p, b, start);
        }
    }
    return lhs;
}

static Expr *ps_expr(Parser *p)
{
    return ps_expr_bp(p, 0);
}

/* ------------------------------------------------------------ patterns */

static void ps_field_pats(Parser *p, FieldPat **out, uint32_t *n, int *has_rest)
{
    PsFieldPatVec v = {0};
    ParseCtx saved = p->ctx;
    ps_advance(p); /* { */
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        FieldPat fp;
        uint32_t fstart = ps_off(p), before = p->pos;
        memset(&fp, 0, sizeof fp);
        if (ps_kind(p) == TK_DOTDOT) {
            ps_advance(p);
            *has_rest = 1;
        } else {
            fp.name = ps_name(p, "a field name", 1);
            if (!p->panic && ps_accept(p, TK_COLON))
                fp.pat = ps_pattern(p);
            fp.span = ps_span(p, fstart);
            if (fp.name.name)
                vec_push(v, fp);
        }
        if (!p->panic && !ps_list_sep(p, "fields"))
            break;
        if (p->panic) {
            ps_sync_list(p);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    PS_TAKE(p, v, FieldPat, *out, *n);
    p->ctx = saved;
    if (!p->panic)
        ps_expect(p, TK_RBRACE, "to close the field patterns");
}

static Pattern *ps_new_pat(Parser *p, PatternKind k)
{
    Pattern *pt = ANEW(p->a, Pattern);
    pt->kind = k;
    return pt;
}

static Pattern *ps_pattern1(Parser *p)
{
    uint32_t start = ps_off(p);
    TokenKind k = ps_kind(p);
    Pattern *pt;
    if (k == TK_IDENT && strcmp(ps_tok(p)->str, "_") == 0) {
        pt = ps_new_pat(p, PAT_WILDCARD);
        ps_advance(p);
    } else if (k == TK_LPAREN) {
        ParseCtx saved = p->ctx;
        PsPatVec items = {0};
        int comma = 0;
        ps_advance(p);
        p->ctx.nl_ignore++;
        while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
            vec_push(items, ps_pattern(p));
            if (!ps_accept(p, TK_COMMA))
                break;
            comma = 1;
        }
        if (!p->panic)
            ps_expect(p, TK_RPAREN, "or `,` in tuple pattern");
        p->ctx = saved;
        if (items.len == 1 && !comma) {
            pt = items.data[0];
            vec_free(items);
            return pt;
        }
        pt = ps_new_pat(p, PAT_TUPLE);
        PS_TAKE(p, items, Pattern *, pt->u.list.items, pt->u.list.n);
    } else if (k == TK_DOT) {
        pt = ps_new_pat(p, PAT_VARIANT);
        ps_advance(p);
        pt->u.variant.name = ps_name(p, "a variant name after `.`", 0);
        if (!p->panic && ps_kind(p) == TK_LBRACE && !ps_nl(p)) {
            pt->u.variant.has_fields = 1;
            ps_field_pats(p, &pt->u.variant.fields, &pt->u.variant.nfields, &pt->u.variant.has_rest);
        }
    } else if (k == TK_INT || k == TK_MINUS || k == TK_TRUE || k == TK_FALSE || k == TK_STRING ||
               k == TK_ALL0 || k == TK_ALL1) {
        Expr *lo = ps_expr_bp(p, PS_BP_UNARY);
        if (!p->panic && (ps_kind(p) == TK_DOTDOT || ps_kind(p) == TK_DOTDOTEQ)) {
            pt = ps_new_pat(p, PAT_RANGE);
            pt->u.range.inclusive = ps_kind(p) == TK_DOTDOTEQ;
            ps_advance(p);
            pt->u.range.lo = lo;
            pt->u.range.hi = ps_expr_bp(p, PS_BP_UNARY);
        } else {
            pt = ps_new_pat(p, PAT_LIT);
            pt->u.lit = lo;
        }
    } else if (ps_is_name_tok(k)) {
        Path path;
        ParseCtx saved = p->ctx;
        p->ctx.generic = 0;
        ps_path(p, &path, PS_PATH_TYPE);
        p->ctx = saved;
        if (!p->panic && ps_kind(p) == TK_LBRACE && !ps_nl(p)) {
            pt = ps_new_pat(p, PAT_STRUCT);
            pt->u.struct_pat.path = path;
            ps_field_pats(p, &pt->u.struct_pat.fields, &pt->u.struct_pat.nfields, &pt->u.struct_pat.has_rest);
        } else if (!p->panic && (ps_kind(p) == TK_DOTDOT || ps_kind(p) == TK_DOTDOTEQ)) {
            Expr *lo = ps_new_expr(p, EXPR_PATH, start);
            lo->u.path = path;
            lo->span = path.span;
            pt = ps_new_pat(p, PAT_RANGE);
            pt->u.range.inclusive = ps_kind(p) == TK_DOTDOTEQ;
            ps_advance(p);
            pt->u.range.lo = lo;
            pt->u.range.hi = ps_expr_bp(p, PS_BP_UNARY);
        } else if (path.nsegs > 1 || path.segs[0].has_args) {
            pt = ps_new_pat(p, PAT_PATH);
            pt->u.path = path;
        } else {
            pt = ps_new_pat(p, PAT_BIND);
            pt->u.bind = path.segs[0].name;
        }
    } else {
        if (k == TK_ERROR)
            ps_silent_fail(p);
        else if (k == TK_RESERVED)
            ps_reserved(p, 1);
        else if (ps_error(p, ps_err_span(p), "expected a pattern, found %s", ps_describe(p)) && ps_is_word_tok(k))
            diag_help("`%s` is a keyword and cannot be used as a name", tok_kind_text(k));
        pt = ps_new_pat(p, PAT_ERROR);
    }
    pt->span = ps_span(p, start);
    return pt;
}

static Pattern *ps_pattern(Parser *p)
{
    uint32_t start = ps_off(p);
    Pattern *first = ps_pattern1(p);
    PsPatVec alts = {0};
    Pattern *pt;
    if (p->panic || ps_kind(p) != TK_PIPE)
        return first;
    vec_push(alts, first);
    while (!p->panic && ps_kind(p) == TK_PIPE) {
        ps_advance(p);
        vec_push(alts, ps_pattern1(p));
    }
    pt = ps_new_pat(p, PAT_OR);
    PS_TAKE(p, alts, Pattern *, pt->u.list.items, pt->u.list.n);
    pt->span = ps_span(p, start);
    return pt;
}

/* ------------------------------------------------------------ statements */

/* Does the token start a statement for sure (a declaration keyword)? */
static int ps_starts_stmt_kw(TokenKind k)
{
    switch (k) {
    case TK_KW_LET: case TK_KW_VAR: case TK_KW_REG: case TK_KW_MEM: case TK_KW_CONST: case TK_KW_FN:
    case TK_KW_ON: case TK_KW_COMB: case TK_KW_FOR: case TK_KW_WHILE: case TK_KW_RETURN:
    case TK_KW_EXPECT: case TK_KW_ASSERT: case TK_KW_ASSUME: case TK_KW_COVER: case TK_KW_DOMAIN:
    case TK_KW_STRUCT: case TK_KW_ENUM: case TK_KW_TYPE: case TK_KW_PUB: case TK_KW_PROPERTY:
    case TK_KW_SEQUENCE: case TK_KW_MODULE: case TK_KW_EXTERN: case TK_KW_TEST: case TK_KW_REGMAP:
    case TK_KW_BUNDLE: case TK_ATTR:
        return 1;
    default:
        return 0;
    }
}

/* Skips to the start of the next statement: a line break, `;` or `}` at the
 * current nesting level. A declaration keyword at the start of a line where
 * the error was found is itself the next statement. */
static void ps_sync_stmt(Parser *p, uint32_t start_pos)
{
    int depth = 0;
    if (p->pos != start_pos && ps_tok(p)->nl_before && ps_starts_stmt_kw(ps_kind(p))) {
        p->panic = 0;
        return;
    }
    while (ps_kind(p) != TK_EOF) {
        TokenKind k = ps_kind(p);
        if (depth == 0) {
            if (k == TK_RBRACE)
                break;
            if (k == TK_SEMI) {
                ps_advance(p);
                break;
            }
        }
        if (k == TK_LBRACE || k == TK_LPAREN || k == TK_LBRACKET)
            depth++;
        else if (k == TK_RBRACE || k == TK_RPAREN || k == TK_RBRACKET) {
            if (depth > 0)
                depth--;
        }
        ps_advance(p);
        if (depth == 0 && ps_tok(p)->nl_before)
            break;
    }
    p->panic = 0;
}

/* Parses statements up to (not including) `}` / EOF. */
static void ps_stmt_list(Parser *p, PsStmtVec *out)
{
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        uint32_t before = p->pos;
        Stmt *s;
        if (ps_kind(p) == TK_SEMI) {
            ps_advance(p);
            continue;
        }
        s = ps_stmt(p);
        vec_push(*out, s);
        if (!p->panic) {
            if (ps_kind(p) == TK_SEMI)
                ps_advance(p);
            else if (!ps_at_stmt_end(p)) {
                if (ps_kind(p) == TK_ERROR)
                    ps_silent_fail(p);
                else
                    ps_error(p, ps_err_span(p), "expected a new line or `;` after the statement, found %s",
                             ps_describe(p));
            }
        }
        if (p->panic) {
            ps_sync_stmt(p, before);
            if (p->pos == before)
                ps_advance(p);
        }
    }
}

static Block *ps_block(Parser *p)
{
    Block *b = ANEW(p->a, Block);
    uint32_t start = ps_off(p);
    ParseCtx saved = p->ctx;
    PsStmtVec stmts = {0};
    if (p->panic) {
        b->span = ps_span(p, start);
        return b;
    }
    if (ps_kind(p) != TK_LBRACE) {
        if (ps_kind(p) == TK_IDENT && strcmp(ps_tok(p)->str, "begin") == 0) {
            if (ps_error(p, ps_tok_span(p), "`begin`/`end` is SystemVerilog; Tern uses `{ }`"))
                diag_help("write `{ ... }`");
        } else if (ps_kind(p) == TK_ERROR) {
            ps_silent_fail(p);
        } else {
            ps_error(p, ps_err_span(p), "expected `{`, found %s", ps_describe(p));
        }
        b->span = ps_span(p, start);
        return b;
    }
    ps_advance(p);
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    ps_stmt_list(p, &stmts);
    p->ctx = saved;
    PS_TAKE(p, stmts, Stmt *, b->stmts, b->nstmts);
    if (ps_kind(p) == TK_RBRACE) {
        ps_advance(p);
        if (b->nstmts) {
            /* the value: a final expression statement not followed by `;` */
            Stmt *last = b->stmts[b->nstmts - 1];
            uint32_t i = p->pos - 2; /* token before `}` */
            if (last->kind == STMT_EXPR && p->toks[i].kind != TK_SEMI)
                b->tail = last->u.expr;
        }
    } else {
        ps_close_brace(p, start);
    }
    b->span = ps_span(p, start);
    return b;
}

typedef struct PsSvWord {
    const char *word;
    const char *msg;
    const char *help;
} PsSvWord;

static const PsSvWord ps_sv_words[] = {
    {"begin", "`begin`/`end` is SystemVerilog; Tern uses `{ }`", NULL},
    {"end", "`begin`/`end` is SystemVerilog; Tern uses `{ }`", NULL},
    {"endmodule", "`endmodule` is SystemVerilog", "a module body ends with `}`"},
    {"endcase", "`endcase` is SystemVerilog", "a `match` body ends with `}`"},
    {"endfunction", "`endfunction` is SystemVerilog", "a `fn` body ends with `}`"},
    {"endgenerate", "`endgenerate` is SystemVerilog", "elaboration-time `for`/`if` need no generate block"},
    {"always_ff", "`always_ff` is SystemVerilog", "write `on clk { ... }`; reset values go on the `reg` declarations"},
    {"always_comb", "`always_comb` is SystemVerilog", "write `comb { ... }`"},
    {"always_latch", "`always_latch` is SystemVerilog", "latches cannot be written in Tern; import them with `extern module`"},
    {"always", "`always @(...)` is SystemVerilog", "write `on clk { ... }` for flip-flops or `comb { ... }` for logic"},
    {"assign", "`assign` is SystemVerilog", "write a continuous assignment as `y = a & b`, or `let y = a & b` for a new wire"},
    {"input", "`input` is SystemVerilog", "declare ports in the module body: `in name: T`"},
    {"output", "`output` is SystemVerilog", "declare ports in the module body: `out name: T`"},
    {"inout", "`inout` does not exist in Tern", "a bidirectional pad is `in pad: tri<T>` on a `#[top]` module"},
    {"parameter", "`parameter` is SystemVerilog", "use a generic: `module M<W: uint = 8>`"},
    {"localparam", "`localparam` is SystemVerilog", "use `const W: uint = ...`"},
    {"typedef", "`typedef` is SystemVerilog", "use `struct`, `enum` or `type Name = ...`"},
    {"function", "`function` is SystemVerilog", "use `fn name(x: T) -> T { ... }`"},
    {"integer", "`integer` is SystemVerilog", "use `const` with type `uint`/`sint`, or a sized type such as `s32`"},
};

/* SystemVerilog keywords used as a statement start. Returns 1 if reported. */
static int ps_sv_statement(Parser *p)
{
    size_t i;
    const char *w;
    TokenKind next;
    if (ps_kind(p) != TK_IDENT)
        return 0;
    w = ps_tok(p)->str;
    next = ps_peek(p, 1);
    for (i = 0; i < TERN_ARRAY_LEN(ps_sv_words); i++) {
        int bare;
        if (strcmp(w, ps_sv_words[i].word) != 0)
            continue;
        bare = next == TK_EOF || next == TK_SEMI || next == TK_RBRACE || p->toks[p->pos + 1].nl_before;
        if (strcmp(w, "always") == 0 && next != TK_AT)
            return 0;
        if (!(next == TK_IDENT || next == TK_RESERVED || next == TK_AT || next == TK_ERROR || next == TK_LBRACKET ||
              bare || (next == TK_KW_REG && strcmp(w, "output") == 0)))
            return 0;
        if (next == TK_LBRACKET && !(strcmp(w, "input") == 0 || strcmp(w, "output") == 0))
            return 0;
        if ((strcmp(w, "begin") == 0 || strcmp(w, "end") == 0 || strncmp(w, "end", 3) == 0) && !bare &&
            next != TK_IDENT)
            return 0;
        if (ps_error(p, ps_tok_span(p), "%s", ps_sv_words[i].msg) && ps_sv_words[i].help)
            diag_help("%s", ps_sv_words[i].help);
        return 1;
    }
    return 0;
}

/* `where cond` constraint on the same line (rand<T>() where c). */
static Expr *ps_opt_constraint(Parser *p)
{
    if (!p->panic && ps_kind(p) == TK_KW_WHERE && !ps_nl(p)) {
        ps_advance(p);
        return ps_expr(p);
    }
    return NULL;
}

static Stmt *ps_port(Parser *p, Stmt *s)
{
    s->kind = STMT_PORT;
    s->u.port.dir = ps_kind(p) == TK_KW_IN ? PORT_IN : PORT_OUT;
    ps_advance(p);
    s->u.port.name = ps_name(p, "a port name", 1);
    if (!p->panic && ps_expect(p, TK_COLON, "after the port name"))
        s->u.port.type = ps_type(p);
    else
        s->u.port.type = ps_error_type(p, ps_off(p));
    if (!p->panic && ps_kind(p) == TK_AT && !ps_nl(p)) {
        ParseCtx saved = p->ctx;
        ps_advance(p);
        p->ctx.no_struct = 1;
        s->u.port.domain = ps_expr_bp(p, PS_BP_UNARY);
        p->ctx = saved;
    }
    if (!p->panic && ps_kind(p) == TK_ATTR && !ps_nl(p)) {
        /* trailing attributes: in clk: clock #[clock(hz: 100_000_000)] */
        PsAttrVec v = {0};
        uint32_t i;
        for (i = 0; i < s->attrs.n; i++)
            vec_push(v, s->attrs.items[i]);
        while (!p->panic && ps_kind(p) == TK_ATTR && !ps_nl(p))
            ps_attr(p, &v);
        PS_TAKE(p, v, Attr, s->attrs.items, s->attrs.n);
    }
    return s;
}

static int ps_is_item_kw(TokenKind k)
{
    switch (k) {
    case TK_KW_CONST: case TK_KW_FN: case TK_KW_STRUCT: case TK_KW_ENUM: case TK_KW_TYPE:
    case TK_KW_BUNDLE: case TK_KW_PROPERTY: case TK_KW_SEQUENCE: case TK_KW_MODULE:
    case TK_KW_EXTERN: case TK_KW_REGMAP: case TK_KW_TEST:
        return 1;
    default:
        return 0;
    }
}

static Stmt *ps_stmt(Parser *p)
{
    uint32_t start = ps_off(p);
    Stmt *s = ps_new_stmt(p, STMT_ERROR);
    AstDoc *doc = NULL;
    TokenKind k;
    p->construct_pos = p->pos;
    ps_prefix(p, &s->attrs, &doc);
    s->doc = doc;
    if (p->panic) {
        s->span = ps_span(p, start);
        return s;
    }
    k = ps_kind(p);
    switch (k) {
    case TK_KW_PUB:
        ps_advance(p);
        if (!ps_is_item_kw(ps_kind(p))) {
            ps_error(p, ps_err_span(p), "expected an item after `pub`, found %s", ps_describe(p));
            break;
        }
        s->kind = STMT_ITEM;
        s->u.item = ps_item_rest(p, start, doc, s->attrs, 1);
        break;
    case TK_KW_ASSERT:
    case TK_KW_ASSUME:
    case TK_KW_COVER:
        if (ps_peek(p, 1) == TK_KW_PROPERTY) {
            s->kind = STMT_ITEM;
            s->u.item = ps_item_rest(p, start, doc, s->attrs, 0);
            break;
        }
        if (k != TK_KW_ASSERT) {
            if (ps_error(p, ps_err_span(p), "expected `property` after `%s`", tok_kind_text(k)))
                diag_help("`%s property name { ... }` declares a property", tok_kind_text(k));
            break;
        }
        /* fallthrough */
    case TK_KW_EXPECT:
        s->kind = STMT_ASSERT;
        s->u.assert_stmt.kind = k == TK_KW_EXPECT ? ASSERT_EXPECT : ASSERT_ASSERT;
        ps_advance(p);
        s->u.assert_stmt.cond = ps_expr(p);
        if (!p->panic && ps_accept(p, TK_COMMA))
            s->u.assert_stmt.msg = ps_expr(p);
        break;
    case TK_KW_CONST:
    case TK_KW_FN:
    case TK_KW_STRUCT:
    case TK_KW_ENUM:
    case TK_KW_TYPE:
    case TK_KW_BUNDLE:
    case TK_KW_PROPERTY:
    case TK_KW_SEQUENCE:
        s->kind = STMT_ITEM;
        s->u.item = ps_item_rest(p, start, doc, s->attrs, 0);
        break;
    case TK_KW_MODULE:
    case TK_KW_EXTERN:
    case TK_KW_REGMAP:
    case TK_KW_TEST:
    case TK_KW_USE:
    case TK_KW_PACKAGE:
        if (ps_error(p, ps_tok_span(p), "`%s` is not allowed here", tok_kind_text(k)))
            diag_help("`%s` declarations go at file level", tok_kind_text(k));
        break;
    case TK_KW_IN:
    case TK_KW_OUT:
        if ((ps_is_name_tok(ps_peek(p, 1)) || (ps_is_word_tok(ps_peek(p, 1)) && ps_peek(p, 1) != TK_KW_IN)) &&
            ps_peek(p, 2) == TK_COLON) {
            ps_port(p, s);
            break;
        }
        goto expr_stmt;
    case TK_KW_DOMAIN: {
        ParseCtx saved;
        s->kind = STMT_DOMAIN;
        ps_advance(p);
        s->u.domain.name = ps_name(p, "a domain name", 0);
        if (p->panic || !ps_expect(p, TK_ASSIGN, "after the domain name"))
            break;
        if (ps_kind(p) != TK_LPAREN) {
            if (ps_error(p, ps_err_span(p), "expected `(` after `domain %s =`, found %s",
                         s->u.domain.name.name, ps_describe(p)))
                diag_help("a domain is a clock and an optional reset: `domain sys = (clk, rst)`");
            break;
        }
        saved = p->ctx;
        ps_advance(p);
        p->ctx.nl_ignore++;
        p->ctx.no_struct = 0;
        s->u.domain.clock = ps_expr(p);
        if (!p->panic && ps_accept(p, TK_COMMA) && ps_kind(p) != TK_RPAREN)
            s->u.domain.reset = ps_expr(p);
        if (!p->panic)
            ps_expect(p, TK_RPAREN, "after the domain's clock and reset");
        p->ctx = saved;
        break;
    }
    case TK_KW_LET:
        s->kind = STMT_LET;
        ps_advance(p);
        s->u.let.pat = ps_pattern(p);
        if (!p->panic && ps_accept(p, TK_COLON))
            s->u.let.type = ps_type(p);
        if (p->panic)
            break;
        if (ps_kind(p) != TK_ASSIGN) {
            if (ps_kind(p) == TK_ERROR) {
                ps_silent_fail(p);
            } else if (ps_error(p, ps_err_span(p), "expected `=` in `let`, found %s", ps_describe(p))) {
                diag_help("a `let` is a wire and needs a value (`let x = expr`); use `var x: T` for a variable "
                          "assigned later");
            }
            break;
        }
        ps_advance(p);
        s->u.let.init = ps_expr(p);
        s->u.let.constraint = ps_opt_constraint(p);
        break;
    case TK_KW_VAR:
        s->kind = STMT_VAR;
        ps_advance(p);
        s->u.var.name = ps_name(p, "a variable name", 1);
        if (!p->panic && ps_accept(p, TK_COLON))
            s->u.var.type = ps_type(p);
        if (!p->panic && ps_accept(p, TK_ASSIGN)) {
            s->u.var.init = ps_expr(p);
            s->u.var.constraint = ps_opt_constraint(p);
        }
        break;
    case TK_KW_REG:
        s->kind = STMT_REG;
        ps_advance(p);
        s->u.reg.name = ps_name(p, "a register name", 1);
        if (p->panic)
            break;
        if (ps_kind(p) != TK_COLON) {
            if (ps_error(p, ps_err_span(p), "expected `:` and a type after `reg %s`, found %s",
                         s->u.reg.name.name, ps_describe(p)))
                diag_help("a register needs a type: `reg %s: u8 = 0`", s->u.reg.name.name);
            break;
        }
        ps_advance(p);
        s->u.reg.type = ps_type(p);
        if (!p->panic && ps_accept(p, TK_ASSIGN)) {
            ParseCtx saved = p->ctx;
            p->ctx.no_at = 1;
            s->u.reg.init = ps_expr(p);
            p->ctx = saved;
        }
        if (!p->panic && ps_kind(p) == TK_AT && !ps_nl(p)) {
            ParseCtx saved = p->ctx;
            ps_advance(p);
            p->ctx.no_struct = 1;
            s->u.reg.domain = ps_expr_bp(p, PS_BP_UNARY);
            p->ctx = saved;
        }
        break;
    case TK_KW_MEM:
        s->kind = STMT_MEM;
        ps_advance(p);
        s->u.mem.name = ps_name(p, "a memory name", 1);
        if (p->panic)
            break;
        if (!ps_expect(p, TK_COLON, "and a type `[T; N]` after the memory name"))
            break;
        s->u.mem.type = ps_type(p);
        if (!p->panic && ps_accept(p, TK_ASSIGN))
            s->u.mem.init = ps_expr(p);
        break;
    case TK_KW_COMB:
        s->kind = STMT_COMB;
        ps_advance(p);
        s->u.comb = ps_block(p);
        break;
    case TK_KW_ON: {
        ParseCtx saved = p->ctx;
        s->kind = STMT_ON;
        ps_advance(p);
        p->ctx.no_struct = 1;
        if (ps_kind(p) == TK_LBRACE) {
            if (ps_error(p, ps_err_span(p), "expected a clock or domain after `on`, found `{`"))
                diag_help("write `on clk { ... }`");
            p->ctx = saved;
            break;
        }
        s->u.on.clock = ps_expr_bp(p, PS_BP_UNARY);
        p->ctx = saved;
        s->u.on.body = ps_block(p);
        break;
    }
    case TK_KW_FOR: {
        ParseCtx saved = p->ctx;
        s->kind = STMT_FOR;
        ps_advance(p);
        s->u.for_stmt.pat = ps_pattern(p);
        if (!p->panic && !ps_expect(p, TK_KW_IN, "after the loop pattern"))
            break;
        p->ctx.no_struct = 1;
        p->ctx.generic = 0;
        s->u.for_stmt.iter = ps_expr(p);
        p->ctx = saved;
        s->u.for_stmt.body = ps_block(p);
        break;
    }
    case TK_KW_WHILE: {
        ParseCtx saved = p->ctx;
        s->kind = STMT_WHILE;
        ps_advance(p);
        p->ctx.no_struct = 1;
        p->ctx.generic = 0;
        s->u.while_stmt.cond = ps_expr(p);
        p->ctx = saved;
        s->u.while_stmt.body = ps_block(p);
        break;
    }
    case TK_KW_RETURN:
        s->kind = STMT_RETURN;
        ps_advance(p);
        if (!ps_at_stmt_end(p) && ps_kind(p) != TK_COMMA)
            s->u.ret = ps_expr(p);
        break;
    case TK_RESERVED:
        ps_reserved(p, 1);
        break;
    case TK_IDENT:
        if (strcmp(ps_tok(p)->str, "static_assert") == 0 && ps_peek(p, 1) == TK_LPAREN) {
            ParseCtx saved = p->ctx;
            s->kind = STMT_STATIC_ASSERT;
            ps_advance(p);
            ps_advance(p);
            p->ctx.nl_ignore++;
            p->ctx.no_struct = 0;
            p->ctx.generic = 0;
            s->u.static_assert.cond = ps_expr(p);
            if (!p->panic && ps_accept(p, TK_COMMA) && ps_kind(p) != TK_RPAREN) {
                s->u.static_assert.msg = ps_expr(p);
                ps_accept(p, TK_COMMA);
            }
            if (!p->panic)
                ps_expect(p, TK_RPAREN, "to close `static_assert(`");
            p->ctx = saved;
            break;
        }
        if (ps_sv_statement(p))
            break;
        goto expr_stmt;
    default:
    expr_stmt: {
        Expr *lhs = ps_expr(p);
        if (p->panic) {
            s->kind = STMT_EXPR;
            s->u.expr = lhs;
            break;
        }
        if (ps_kind(p) == TK_ASSIGN) {
            s->kind = STMT_ASSIGN;
            ps_advance(p);
            s->u.assign.lhs = lhs;
            s->u.assign.rhs = ps_expr(p);
            s->u.assign.constraint = ps_opt_constraint(p);
            break;
        }
        s->kind = STMT_EXPR;
        s->u.expr = lhs;
        if (lhs->kind == EXPR_BINARY && lhs->u.binary.op == BIN_LE &&
            (lhs->u.binary.lhs->kind == EXPR_PATH || lhs->u.binary.lhs->kind == EXPR_FIELD ||
             lhs->u.binary.lhs->kind == EXPR_INDEX || lhs->u.binary.lhs->kind == EXPR_SLICE)) {
            Span op = span_make(p->file, lhs->u.binary.lhs->span.off + lhs->u.binary.lhs->span.len, 0);
            uint32_t i;
            /* find the `<=` token for the span */
            for (i = p->pos; i > 0; i--) {
                if (p->toks[i - 1].kind == TK_LE && p->toks[i - 1].off >= lhs->span.off &&
                    p->toks[i - 1].off < lhs->u.binary.rhs->span.off) {
                    op = span_make(p->file, p->toks[i - 1].off, 2);
                    break;
                }
            }
            if (ps_error_soft(p, op, "`<=` non-blocking assignment does not exist in Tern"))
                diag_help("use `=`; inside `on clk { }` every assignment to a `reg` is non-blocking");
        }
        break;
    }
    }
    s->span = ps_span(p, start);
    return s;
}

/* ------------------------------------------------------------ items */

static void ps_generic_params(Parser *p, GenericParams *out)
{
    PsGParamVec v = {0};
    ParseCtx saved = p->ctx;
    uint32_t start = ps_off(p);
    if (ps_kind(p) != TK_LT)
        return;
    out->present = 1;
    ps_advance(p);
    p->ctx.nl_ignore++;
    p->ctx.generic = 1;
    p->ctx.no_struct = 0;
    p->ctx.no_at = 0;
    while (!p->panic && !ps_is_gt(p) && ps_kind(p) != TK_EOF) {
        GenericParam gp;
        uint32_t gstart = ps_off(p);
        memset(&gp, 0, sizeof gp);
        gp.name = ps_name(p, "a generic parameter name", 0);
        if (p->panic)
            break;
        if (ps_kind(p) != TK_COLON) {
            if (ps_error(p, ps_err_span(p), "expected `:` after generic parameter `%s`, found %s", gp.name.name,
                         ps_describe(p)))
                diag_help("write `%s: type` for a type parameter or `%s: uint` for a value", gp.name.name,
                          gp.name.name);
            break;
        }
        ps_advance(p);
        if (ps_kind(p) == TK_KW_TYPE) {
            gp.kind = GENERIC_TYPE;
            ps_advance(p);
            if (ps_accept(p, TK_ASSIGN))
                gp.default_type = ps_type(p);
        } else {
            gp.kind = GENERIC_VALUE;
            gp.type = ps_type(p);
            if (!p->panic && ps_accept(p, TK_ASSIGN))
                gp.default_value = ps_expr(p);
        }
        gp.span = ps_span(p, gstart);
        vec_push(v, gp);
        if (!ps_accept(p, TK_COMMA))
            break;
    }
    PS_TAKE(p, v, GenericParam, out->items, out->n);
    p->ctx = saved;
    if (!p->panic) {
        if (ps_is_gt(p))
            ps_eat_gt(p);
        else
            ps_error(p, ps_err_span(p), "expected `,` or `>` in generic parameters, found %s", ps_describe(p));
    }
    out->span = ps_span(p, start);
}

static void ps_where(Parser *p, WhereList *out)
{
    PsExprVec v = {0};
    ParseCtx saved = p->ctx;
    if (ps_kind(p) != TK_KW_WHERE)
        return;
    ps_advance(p);
    p->ctx.no_struct = 1;
    p->ctx.generic = 0;
    while (!p->panic) {
        vec_push(v, ps_expr(p));
        if (!ps_accept(p, TK_COMMA))
            break;
        if (ps_kind(p) == TK_LBRACE)
            break; /* trailing comma */
    }
    p->ctx = saved;
    PS_TAKE(p, v, Expr *, out->items, out->n);
}

/* `{` statements `}` of a module / extern module. */
static void ps_body(Parser *p, Stmt ***out, uint32_t *n, const char *what)
{
    PsStmtVec v = {0};
    ParseCtx saved = p->ctx;
    uint32_t lbrace = ps_off(p);
    if (!ps_expect(p, TK_LBRACE, what))
        return;
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    p->ctx.temporal = 0;
    ps_stmt_list(p, &v);
    p->ctx = saved;
    PS_TAKE(p, v, Stmt *, *out, *n);
    ps_close_brace(p, lbrace);
}

static Field *ps_field(Parser *p, int allow_flip)
{
    Field *f = ANEW(p->a, Field);
    uint32_t start = ps_off(p);
    AstDoc *doc = NULL;
    ps_prefix(p, &f->attrs, &doc);
    f->doc = doc;
    if (ps_kind(p) == TK_KW_FLIP) {
        if (!allow_flip)
            ps_error_soft(p, ps_tok_span(p), "`flip` is only allowed on bundle fields");
        f->flip = 1;
        ps_advance(p);
    }
    f->name = ps_name(p, "a field name", 1);
    if (!p->panic && ps_expect(p, TK_COLON, "after the field name"))
        f->type = ps_type(p);
    else
        f->type = ps_error_type(p, ps_off(p));
    f->span = ps_span(p, start);
    return f;
}

/* `{ fields }` of a struct, anonymous struct or bundle (bundles may contain fns). */
static void ps_field_list(Parser *p, PsFieldVec *fields, PsItemVec *fns, int allow_flip, const char *what)
{
    ParseCtx saved = p->ctx;
    uint32_t lbrace = ps_off(p);
    if (!ps_expect(p, TK_LBRACE, what))
        return;
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        uint32_t before = p->pos;
        TokenKind k;
        p->construct_pos = p->pos;
        {
            /* look past attributes/docs to see whether a fn follows */
            uint32_t i = p->pos;
            while (i < p->ntoks && p->toks[i].kind == TK_ATTR) {
                int depth = 0;
                for (; i < p->ntoks; i++) {
                    if (p->toks[i].kind == TK_ATTR || p->toks[i].kind == TK_LBRACKET)
                        depth++;
                    else if (p->toks[i].kind == TK_RBRACKET && --depth == 0) {
                        i++;
                        break;
                    }
                }
            }
            k = i < p->ntoks ? (TokenKind)p->toks[i].kind : TK_EOF;
            if (k == TK_KW_PUB && i + 1 < p->ntoks)
                k = (TokenKind)p->toks[i + 1].kind;
            if (k == TK_KW_CONST && i + 1 < p->ntoks && p->toks[i + 1].kind == TK_KW_FN)
                k = TK_KW_FN;
        }
        if (k == TK_KW_FN && fns) {
            uint32_t start = ps_off(p);
            AstDoc *doc = NULL;
            AttrList attrs;
            int is_pub;
            ps_prefix(p, &attrs, &doc);
            is_pub = ps_accept(p, TK_KW_PUB);
            vec_push(*fns, ps_item_rest(p, start, doc, attrs, is_pub));
        } else {
            Field *f = ps_field(p, allow_flip);
            if (f->name.name)
                vec_push(*fields, f);
        }
        if (!p->panic && !ps_list_sep(p, "fields"))
            break;
        if (p->panic) {
            ps_sync_list(p);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    p->ctx = saved;
    ps_close_brace(p, lbrace);
}

static void ps_variants(Parser *p, PsVariantVec *out)
{
    ParseCtx saved = p->ctx;
    uint32_t lbrace = ps_off(p);
    if (!ps_expect(p, TK_LBRACE, "to open the enum variants"))
        return;
    p->ctx.nl_ignore = 0;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
        uint32_t before = p->pos, start = ps_off(p);
        Variant *v = ANEW(p->a, Variant);
        p->construct_pos = p->pos;
        AstDoc *doc = NULL;
        ps_prefix(p, &v->attrs, &doc);
        v->doc = doc;
        v->name = ps_name(p, "a variant name", 0);
        if (!p->panic && ps_kind(p) == TK_LBRACE && !ps_nl(p)) {
            PsFieldVec fields = {0};
            v->has_fields = 1;
            ps_field_list(p, &fields, NULL, 0, "variant fields");
            PS_TAKE(p, fields, Field *, v->fields, v->nfields);
        } else if (!p->panic && ps_kind(p) == TK_LPAREN) {
            if (ps_error(p, ps_tok_span(p), "tuple variants do not exist"))
                diag_help("give the payload named fields: `%s { value: T }`", v->name.name);
        }
        v->span = ps_span(p, start);
        if (v->name.name)
            vec_push(*out, v);
        if (!p->panic && !ps_list_sep(p, "variants"))
            break;
        if (p->panic) {
            ps_sync_list(p);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    p->ctx = saved;
    ps_close_brace(p, lbrace);
}

static void ps_params(Parser *p, Param **out, uint32_t *n)
{
    PsParamVec v = {0};
    ParseCtx saved = p->ctx;
    if (!ps_expect(p, TK_LPAREN, "to open the parameter list"))
        return;
    p->ctx.nl_ignore++;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    while (!p->panic && ps_kind(p) != TK_RPAREN && ps_kind(p) != TK_EOF) {
        Param pr;
        uint32_t start = ps_off(p);
        memset(&pr, 0, sizeof pr);
        pr.name = ps_name(p, "a parameter name", 1);
        if (!p->panic && ps_expect(p, TK_COLON, "and a type after the parameter name"))
            pr.type = ps_type(p);
        else
            pr.type = ps_error_type(p, ps_off(p));
        pr.span = ps_span(p, start);
        vec_push(v, pr);
        if (!ps_accept(p, TK_COMMA))
            break;
    }
    PS_TAKE(p, v, Param, *out, *n);
    if (!p->panic)
        ps_expect(p, TK_RPAREN, "or `,` in the parameter list");
    p->ctx = saved;
}

static int ps_is_access(const char *s)
{
    return strcmp(s, "rw") == 0 || strcmp(s, "ro") == 0 || strcmp(s, "wo") == 0 || strcmp(s, "w1c") == 0 ||
           strcmp(s, "rw1s") == 0;
}

static RegField *ps_reg_field(Parser *p)
{
    RegField *f = ANEW(p->a, RegField);
    uint32_t start = ps_off(p);
    AttrList attrs;
    AstDoc *doc = NULL;
    ps_prefix(p, &attrs, &doc);
    f->doc = doc;
    f->name = ps_name(p, "a field name", 1);
    if (!p->panic && ps_expect(p, TK_COLON, "after the field name"))
        f->type = ps_type(p);
    else
        f->type = ps_error_type(p, ps_off(p));
    if (!p->panic) {
        if (ps_kind(p) != TK_AT) {
            if (ps_error(p, ps_err_span(p), "expected `@` and a bit position after field `%s`, found %s",
                         f->name.name, ps_describe(p)))
                diag_help("write `%s: bool @0` or `%s: uint<2> @[2:1]`", f->name.name, f->name.name);
        } else {
            ps_advance(p);
            if (ps_kind(p) == TK_LBRACKET) {
                ParseCtx saved = p->ctx;
                ps_advance(p);
                p->ctx.nl_ignore++;
                f->bit_hi = ps_expr(p);
                if (!p->panic && ps_expect(p, TK_COLON, "in the bit range `@[hi:lo]`"))
                    f->bit_lo = ps_expr(p);
                if (!p->panic)
                    ps_expect(p, TK_RBRACKET, "to close the bit range");
                p->ctx = saved;
            } else {
                f->bit_hi = ps_expr_bp(p, PS_BP_UNARY);
            }
        }
    }
    f->span = ps_span(p, start);
    return f;
}

static RegEntry *ps_reg_entry(Parser *p)
{
    RegEntry *r = ANEW(p->a, RegEntry);
    uint32_t start = ps_off(p);
    AstDoc *doc = NULL;
    ps_prefix(p, &r->attrs, &doc);
    r->doc = doc;
    if (p->panic)
        return r;
    if (ps_kind(p) == TK_IDENT && ps_peek(p, 1) == TK_COLON) {
        ps_error(p, ps_err_span(p), "expected the register's offset before its name, as in `0x00 %s: rw u32`",
                 ps_tok(p)->str);
        return r;
    }
    r->offset = ps_expr_bp(p, PS_BP_UNARY);
    if (!p->panic)
        r->name = ps_name(p, "a register name after the offset", 1);
    if (!p->panic)
        ps_expect(p, TK_COLON, "after the register name");
    if (p->panic)
        return r;
    if (ps_kind(p) == TK_IDENT && ps_is_access(ps_tok(p)->str)) {
        r->access = ps_ident_here(p);
        ps_advance(p);
    } else {
        if (ps_error(p, ps_err_span(p), "expected a register access (rw, ro, wo, w1c, rw1s), found %s",
                     ps_describe(p)))
            diag_help("write `offset name: access type`, e.g. `0x08 count: rw u32`");
        return r;
    }
    if (ps_kind(p) == TK_LBRACE) {
        ParseCtx saved = p->ctx;
        PsRegFieldVec fields = {0};
        uint32_t lbrace = ps_off(p);
        r->kind = REG_FIELDS;
        ps_advance(p);
        p->ctx.nl_ignore = 0;
        while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
            uint32_t before = p->pos;
            vec_push(fields, ps_reg_field(p));
            if (!p->panic && !ps_list_sep(p, "register fields"))
                break;
            if (p->panic) {
                ps_sync_list(p);
                if (p->pos == before)
                    ps_advance(p);
            }
        }
        p->ctx = saved;
        PS_TAKE(p, fields, RegField *, r->fields, r->nfields);
        ps_close_brace(p, lbrace);
    } else {
        r->kind = REG_SCALAR;
        r->type = ps_type(p);
        if (!p->panic && ps_kind(p) == TK_ARROW) {
            ps_advance(p);
            r->kind = REG_STREAM;
            r->stream_type = ps_type(p);
        }
    }
    if (!p->panic && ps_accept(p, TK_ASSIGN))
        r->reset = ps_expr(p);
    r->span = ps_span(p, start);
    return r;
}

static Expr *ps_temporal_body(Parser *p, const char *what)
{
    ParseCtx saved = p->ctx;
    uint32_t lbrace = ps_off(p);
    Expr *e;
    if (!ps_expect(p, TK_LBRACE, what))
        return ps_error_expr(p, ps_off(p));
    p->ctx.nl_ignore = 1;
    p->ctx.temporal = 1;
    p->ctx.no_struct = 0;
    p->ctx.generic = 0;
    p->ctx.no_at = 0;
    e = ps_expr(p);
    p->ctx = saved;
    if (p->panic) {
        /* skip to the matching brace */
        int depth = 1;
        p->panic = 0;
        while (ps_kind(p) != TK_EOF) {
            if (ps_kind(p) == TK_LBRACE)
                depth++;
            else if (ps_kind(p) == TK_RBRACE && --depth == 0) {
                ps_advance(p);
                break;
            }
            ps_advance(p);
        }
        return e;
    }
    if (ps_kind(p) == TK_RBRACE) {
        ps_advance(p);
    } else if (ps_error(p, ps_err_span(p), "expected `}` after the property expression, found %s", ps_describe(p))) {
        diag_note(span_make(p->file, lbrace, 1), "the property body opens here");
    }
    return e;
}

static Item *ps_new_item(Parser *p, ItemKind k, AstDoc *doc, AttrList attrs, int is_pub)
{
    Item *it = ANEW(p->a, Item);
    it->kind = k;
    it->doc = doc;
    it->attrs = attrs;
    it->is_pub = is_pub;
    TERN_UNUSED(p);
    return it;
}

/* An item after its attributes, doc and `pub`. */
static Item *ps_item_rest(Parser *p, uint32_t start, AstDoc *doc, AttrList attrs, int is_pub)
{
    TokenKind k = ps_kind(p);
    Item *it = ps_new_item(p, ITEM_ERROR, doc, attrs, is_pub);
    switch (k) {
    case TK_KW_MODULE:
        it->kind = ITEM_MODULE;
        ps_advance(p);
        it->name = ps_name(p, "a module name", 0);
        if (p->panic)
            break;
        if (ps_kind(p) == TK_LPAREN || ps_kind(p) == TK_ERROR) {
            if (ps_kind(p) == TK_ERROR) {
                ps_silent_fail(p);
                break;
            }
            if (ps_error(p, ps_tok_span(p), "SystemVerilog-style port list after `module %s`", it->name.name))
                diag_help("declare ports inside the body: `module %s { in a: u8  out y: u8 }`", it->name.name);
            break;
        }
        ps_generic_params(p, &it->u.module.generics);
        if (!p->panic)
            ps_where(p, &it->u.module.where);
        if (!p->panic)
            ps_body(p, &it->u.module.body, &it->u.module.nbody, "to open the module body");
        break;
    case TK_KW_EXTERN:
        it->kind = ITEM_EXTERN;
        ps_advance(p);
        if (!ps_expect(p, TK_KW_MODULE, "after `extern`"))
            break;
        it->name = ps_name(p, "a module name", 0);
        if (p->panic)
            break;
        if (ps_kind(p) != TK_LPAREN) {
            if (ps_error(p, ps_err_span(p), "expected `(` and the SystemVerilog file after `extern module %s`, found %s",
                         it->name.name, ps_describe(p)))
                diag_help("write `extern module %s(\"rtl/file.sv\") { ... }` (`\"\"` for a vendor library cell)",
                          it->name.name);
            break;
        }
        ps_advance(p);
        if (ps_kind(p) == TK_STRING) {
            it->u.extern_mod.file = ps_tok(p)->str;
            it->u.extern_mod.file_span = ps_tok_span(p);
            ps_advance(p);
        } else {
            ps_error(p, ps_err_span(p), "expected the SystemVerilog file name as a string, found %s", ps_describe(p));
            break;
        }
        if (!ps_expect(p, TK_RPAREN, "after the file name"))
            break;
        ps_generic_params(p, &it->u.extern_mod.generics);
        if (!p->panic)
            ps_where(p, &it->u.extern_mod.where);
        if (!p->panic)
            ps_body(p, &it->u.extern_mod.body, &it->u.extern_mod.nbody, "to open the extern module body");
        break;
    case TK_KW_BUNDLE: {
        PsFieldVec fields = {0};
        PsItemVec fns = {0};
        it->kind = ITEM_BUNDLE;
        ps_advance(p);
        it->name = ps_name(p, "a bundle name", 0);
        if (!p->panic)
            ps_generic_params(p, &it->u.bundle.generics);
        if (!p->panic)
            ps_field_list(p, &fields, &fns, 1, "to open the bundle body");
        PS_TAKE(p, fields, Field *, it->u.bundle.fields, it->u.bundle.nfields);
        PS_TAKE(p, fns, Item *, it->u.bundle.fns, it->u.bundle.nfns);
        break;
    }
    case TK_KW_STRUCT: {
        PsFieldVec fields = {0};
        it->kind = ITEM_STRUCT;
        ps_advance(p);
        it->name = ps_name(p, "a struct name", 0);
        if (!p->panic)
            ps_generic_params(p, &it->u.struct_def.generics);
        if (!p->panic)
            ps_field_list(p, &fields, NULL, 0, "to open the struct body");
        PS_TAKE(p, fields, Field *, it->u.struct_def.fields, it->u.struct_def.nfields);
        break;
    }
    case TK_KW_ENUM: {
        PsVariantVec vs = {0};
        it->kind = ITEM_ENUM;
        ps_advance(p);
        it->name = ps_name(p, "an enum name", 0);
        if (!p->panic)
            ps_generic_params(p, &it->u.enum_def.generics);
        if (!p->panic)
            ps_variants(p, &vs);
        PS_TAKE(p, vs, Variant *, it->u.enum_def.variants, it->u.enum_def.nvariants);
        break;
    }
    case TK_KW_CONST:
        if (ps_peek(p, 1) == TK_KW_FN) {
            ps_advance(p);
            it->u.fn.is_const = 1;
            goto fn_item;
        }
        it->kind = ITEM_CONST;
        ps_advance(p);
        it->name = ps_name(p, "a constant name", 0);
        if (!p->panic && ps_accept(p, TK_COLON))
            it->u.const_def.type = ps_type(p);
        if (p->panic)
            break;
        if (!ps_expect(p, TK_ASSIGN, "and a value in `const`")) {
            it->u.const_def.value = ps_error_expr(p, ps_off(p));
            break;
        }
        it->u.const_def.value = ps_expr(p);
        break;
    case TK_KW_FN:
    fn_item:
        it->kind = ITEM_FN;
        ps_advance(p);
        it->name = ps_name(p, "a function name", 1);
        if (!p->panic)
            ps_generic_params(p, &it->u.fn.generics);
        if (!p->panic)
            ps_params(p, &it->u.fn.params, &it->u.fn.nparams);
        if (!p->panic && ps_accept(p, TK_ARROW))
            it->u.fn.ret = ps_type(p);
        if (!p->panic)
            ps_where(p, &it->u.fn.where);
        if (!p->panic)
            it->u.fn.body = ps_block(p);
        else
            it->u.fn.body = ANEW(p->a, Block);
        break;
    case TK_KW_TYPE:
        it->kind = ITEM_TYPE;
        ps_advance(p);
        it->name = ps_name(p, "a type name", 0);
        if (!p->panic)
            ps_generic_params(p, &it->u.type_alias.generics);
        if (!p->panic && ps_expect(p, TK_ASSIGN, "in the type alias"))
            it->u.type_alias.type = ps_type(p);
        else
            it->u.type_alias.type = ps_error_type(p, ps_off(p));
        break;
    case TK_KW_REGMAP: {
        PsRegEntryVec entries = {0};
        ParseCtx saved = p->ctx;
        uint32_t lbrace;
        it->kind = ITEM_REGMAP;
        ps_advance(p);
        it->name = ps_name(p, "a regmap name", 0);
        if (p->panic)
            break;
        lbrace = ps_off(p);
        if (!ps_expect(p, TK_LBRACE, "to open the regmap"))
            break;
        p->ctx.nl_ignore = 0;
        p->ctx.no_struct = 0;
        p->ctx.generic = 0;
        while (ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
            uint32_t before = p->pos;
            p->construct_pos = p->pos;
            vec_push(entries, ps_reg_entry(p));
            if (!p->panic && !ps_list_sep(p, "register entries"))
                break;
            if (p->panic) {
                ps_sync_stmt(p, before);
                if (p->pos == before)
                    ps_advance(p);
            }
        }
        p->ctx = saved;
        PS_TAKE(p, entries, RegEntry *, it->u.regmap.entries, it->u.regmap.nentries);
        ps_close_brace(p, lbrace);
        break;
    }
    case TK_KW_TEST:
        it->kind = ITEM_TEST;
        ps_advance(p);
        if (ps_kind(p) == TK_STRING) {
            it->u.test.name = ps_tok(p)->str;
            it->u.test.name_span = ps_tok_span(p);
            ps_advance(p);
        } else {
            if (ps_error(p, ps_err_span(p), "expected the test name as a string, found %s", ps_describe(p)))
                diag_help("write `test \"what it checks\" { ... }`");
            break;
        }
        it->u.test.body = ps_block(p);
        break;
    case TK_KW_ASSERT:
    case TK_KW_ASSUME:
    case TK_KW_COVER:
    case TK_KW_PROPERTY:
        it->kind = ITEM_PROPERTY;
        it->u.property.kind = k == TK_KW_ASSERT ? PROP_ASSERT
                              : k == TK_KW_ASSUME ? PROP_ASSUME
                              : k == TK_KW_COVER  ? PROP_COVER
                                                  : PROP_PLAIN;
        if (k != TK_KW_PROPERTY) {
            ps_advance(p);
            if (!ps_expect(p, TK_KW_PROPERTY, NULL))
                break;
        } else {
            ps_advance(p);
        }
        it->name = ps_name(p, "a property name", 0);
        if (!p->panic)
            it->u.property.body = ps_temporal_body(p, "to open the property body");
        else
            it->u.property.body = ps_error_expr(p, ps_off(p));
        break;
    case TK_KW_SEQUENCE:
        it->kind = ITEM_SEQUENCE;
        ps_advance(p);
        it->name = ps_name(p, "a sequence name", 0);
        if (!p->panic)
            it->u.sequence.body = ps_temporal_body(p, "to open the sequence body");
        else
            it->u.sequence.body = ps_error_expr(p, ps_off(p));
        break;
    default:
        if (k == TK_ERROR)
            ps_silent_fail(p);
        else
            ps_error(p, ps_err_span(p), "expected an item (module, fn, struct, enum, const, ...), found %s",
                     ps_describe(p));
        break;
    }
    it->span = ps_span(p, start);
    return it;
}

/* ------------------------------------------------------------ file level */

static UseTree *ps_use_tree(Parser *p)
{
    UseTree *u = ANEW(p->a, UseTree);
    PsIdentVec path = {0};
    uint32_t start = ps_off(p);
    for (;;) {
        Ident id = ps_word_name(p, "a package or item name");
        if (p->panic)
            break;
        vec_push(path, id);
        if (ps_kind(p) != TK_COLONCOLON)
            break;
        ps_advance(p);
        if (ps_kind(p) == TK_STAR) {
            ps_advance(p);
            u->glob = 1;
            break;
        }
        if (ps_kind(p) == TK_LBRACE) {
            PsUseTreeVec group = {0};
            ParseCtx saved = p->ctx;
            ps_advance(p);
            u->is_group = 1;
            p->ctx.nl_ignore++;
            while (!p->panic && ps_kind(p) != TK_RBRACE && ps_kind(p) != TK_EOF) {
                vec_push(group, ps_use_tree(p));
                if (!ps_accept(p, TK_COMMA))
                    break;
            }
            if (!p->panic)
                ps_expect(p, TK_RBRACE, "or `,` in the use list");
            p->ctx = saved;
            PS_TAKE(p, group, UseTree *, u->group, u->ngroup);
            break;
        }
    }
    PS_TAKE(p, path, Ident, u->path, u->npath);
    if (!p->panic && ps_kind(p) == TK_KW_AS && !u->glob && !u->is_group) {
        ps_advance(p);
        u->alias = ps_name(p, "an alias name after `as`", 0);
    }
    u->span = ps_span(p, start);
    return u;
}

static int ps_starts_item(Parser *p)
{
    switch (ps_kind(p)) {
    case TK_KW_MODULE: case TK_KW_PUB: case TK_KW_EXTERN: case TK_KW_BUNDLE: case TK_KW_STRUCT:
    case TK_KW_ENUM: case TK_KW_CONST: case TK_KW_FN: case TK_KW_TYPE: case TK_KW_REGMAP:
    case TK_KW_TEST: case TK_KW_PROPERTY: case TK_KW_SEQUENCE: case TK_KW_USE: case TK_ATTR:
    case TK_KW_ASSERT: case TK_KW_ASSUME: case TK_KW_COVER: case TK_KW_PACKAGE:
    case TK_RESERVED:
    /* not items, but reported as "... outside a module" by ps_file */
    case TK_KW_REG: case TK_KW_LET: case TK_KW_VAR: case TK_KW_ON: case TK_KW_COMB: case TK_KW_MEM:
    case TK_KW_IN: case TK_KW_OUT:
        return 1;
    default:
        return 0;
    }
}

static void ps_sync_item(Parser *p, uint32_t start_pos)
{
    int depth = 0;
    p->panic = 0;
    if (p->pos != start_pos && ps_tok(p)->nl_before && ps_starts_item(p))
        return;
    while (ps_kind(p) != TK_EOF) {
        TokenKind k = ps_kind(p);
        if (k == TK_LBRACE)
            depth++;
        else if (k == TK_RBRACE) {
            if (depth > 0)
                depth--;
        }
        ps_advance(p);
        if (depth == 0 && ps_tok(p)->nl_before &&
            (ps_starts_item(p) || (ps_kind(p) == TK_IDENT && strcmp(ps_tok(p)->str, "endmodule") == 0)))
            return;
    }
}

static int ps_top_level_sv(Parser *p)
{
    const char *w;
    if (ps_kind(p) != TK_IDENT)
        return 0;
    w = ps_tok(p)->str;
    if (strcmp(w, "endmodule") == 0) {
        if (ps_error(p, ps_tok_span(p), "`endmodule` is SystemVerilog"))
            diag_help("a module body ends with `}`");
        return 1;
    }
    if (strcmp(w, "interface") == 0) {
        if (ps_error(p, ps_tok_span(p), "`interface` is SystemVerilog"))
            diag_help("a group of signals with directions is a `bundle` (use `flip` for the reverse direction)");
        return 1;
    }
    if (strcmp(w, "typedef") == 0 || strcmp(w, "function") == 0 || strcmp(w, "parameter") == 0 ||
        strcmp(w, "localparam") == 0 || strcmp(w, "import") == 0 || strcmp(w, "include") == 0) {
        static const char *const helps[][2] = {
            {"typedef", "use `struct`, `enum` or `type Name = ...`"},
            {"function", "use `fn name(x: T) -> T { ... }`"},
            {"parameter", "use `const NAME: uint = ...` or a module generic"},
            {"localparam", "use `const NAME: uint = ...`"},
            {"import", "use `use pkg::Item`"},
            {"include", "use `use pkg::Item`"},
        };
        size_t i;
        if (ps_error(p, ps_tok_span(p), "`%s` is SystemVerilog, not Tern", w)) {
            for (i = 0; i < TERN_ARRAY_LEN(helps); i++)
                if (strcmp(helps[i][0], w) == 0)
                    diag_help("%s", helps[i][1]);
        }
        return 1;
    }
    return 0;
}

static void ps_file(Parser *p, AstFile *f)
{
    PsUseVec uses = {0};
    PsItemVec items = {0};
    if (ps_kind(p) == TK_KW_PACKAGE) {
        PsIdentVec path = {0};
        uint32_t start = ps_off(p), pkg_pos = p->pos;
        ps_advance(p);
        f->has_package = 1;
        for (;;) {
            Ident id = ps_word_name(p, "a package name");
            if (p->panic)
                break;
            vec_push(path, id);
            if (!ps_accept(p, TK_COLONCOLON))
                break;
        }
        PS_TAKE(p, path, Ident, f->package, f->npackage);
        f->package_span = ps_span(p, start);
        if (!p->panic && !ps_at_stmt_end(p))
            ps_error(p, ps_err_span(p), "expected a new line after the package name, found %s", ps_describe(p));
        if (p->panic)
            ps_sync_item(p, pkg_pos);
        ps_accept(p, TK_SEMI);
    }
    while (ps_kind(p) != TK_EOF) {
        uint32_t before = p->pos;
        uint32_t start = ps_off(p);
        p->construct_pos = p->pos;
        if (ps_kind(p) == TK_SEMI) {
            ps_advance(p);
            continue;
        }
        if (ps_kind(p) == TK_KW_USE || (ps_kind(p) == TK_KW_PUB && ps_peek(p, 1) == TK_KW_USE)) {
            UseDecl *u = ANEW(p->a, UseDecl);
            u->doc = ps_doc_of(p, ps_tok(p));
            u->is_pub = ps_accept(p, TK_KW_PUB);
            ps_advance(p); /* use */
            u->tree = ps_use_tree(p);
            u->span = ps_span(p, start);
            vec_push(uses, u);
        } else if (ps_kind(p) == TK_KW_PACKAGE) {
            ps_error(p, ps_tok_span(p), "`package` must be the first line of the file");
        } else if (ps_kind(p) == TK_RBRACE) {
            ps_error(p, ps_tok_span(p), "unexpected `}` at file level (no block is open)");
        } else if (ps_kind(p) == TK_IDENT && strcmp(ps_tok(p)->str, "static_assert") == 0 &&
                   ps_peek(p, 1) == TK_LPAREN) {
            Stmt *s = ps_stmt(p);
            Item *it = ps_new_item(p, ITEM_STATIC_ASSERT, s->doc, s->attrs, 0);
            it->u.static_assert.cond = s->u.static_assert.cond;
            it->u.static_assert.msg = s->u.static_assert.msg;
            it->span = s->span;
            vec_push(items, it);
        } else if (ps_top_level_sv(p)) {
            /* reported */
        } else {
            AstDoc *doc = NULL;
            AttrList attrs;
            int is_pub;
            ps_prefix(p, &attrs, &doc);
            is_pub = !p->panic && ps_accept(p, TK_KW_PUB);
            if (!p->panic) {
                if (ps_kind(p) == TK_RESERVED) {
                    ps_reserved(p, 1);
                } else if (ps_kind(p) == TK_KW_IN || ps_kind(p) == TK_KW_OUT || ps_kind(p) == TK_KW_REG ||
                           ps_kind(p) == TK_KW_LET || ps_kind(p) == TK_KW_VAR || ps_kind(p) == TK_KW_ON ||
                           ps_kind(p) == TK_KW_COMB || ps_kind(p) == TK_KW_MEM) {
                    if (ps_error(p, ps_tok_span(p), "`%s` outside a module", tok_kind_text(ps_kind(p))))
                        diag_help("ports, signals and blocks are declared inside `module Name { ... }`");
                } else {
                    vec_push(items, ps_item_rest(p, start, doc, attrs, is_pub));
                }
            }
        }
        if (!p->panic) {
            if (ps_kind(p) == TK_SEMI)
                ps_advance(p);
            else if (ps_kind(p) != TK_EOF && !ps_tok(p)->nl_before) {
                if (ps_kind(p) == TK_ERROR)
                    ps_silent_fail(p);
                else
                    ps_error(p, ps_err_span(p), "expected a new line after the item, found %s", ps_describe(p));
            }
        }
        if (p->panic) {
            ps_sync_item(p, before);
            if (p->pos == before)
                ps_advance(p);
        }
    }
    PS_TAKE(p, uses, UseDecl *, f->uses, f->nuses);
    PS_TAKE(p, items, Item *, f->items, f->nitems);
}

static void ps_init(Parser *p, Arena *a, uint32_t src, LexOutput *lx)
{
    const SourceFile *sf = src_get(src);
    memset(p, 0, sizeof *p);
    p->a = a;
    p->file = src;
    p->src = sf ? sf->text : "";
    p->toks = lx->toks;
    p->ntoks = lx->ntoks;
    p->docs = lx->docs;
    p->ldiags = lx->diags;
    p->nldiags = lx->ndiags;
    ps_flush_lex(p, p->toks[0].off + 1);
}

AstFile *parse_file(Arena *a, uint32_t src_id)
{
    AstFile *f = ANEW(a, AstFile);
    const SourceFile *sf = src_get(src_id);
    LexOutput lx;
    Parser p;
    lex_source(a, src_id, &lx);
    f->src = src_id;
    f->path = sf ? sf->path : "";
    f->arena = a;
    f->comments = lx.comments;
    f->ncomments = lx.ncomments;
    f->tokens = lx.toks;
    f->ntokens = lx.ntoks;
    ps_init(&p, a, src_id, &lx);
    ps_file(&p, f);
    ps_flush_lex(&p, UINT32_MAX);
    f->nerrors = lx.nerrors + p.nerrors;
    return f;
}

Expr *parse_expr_string(Arena *a, const char *text, size_t len, const char *name, uint32_t *src_out)
{
    uint32_t src = src_add(name ? name : "<expr>", text, len);
    LexOutput lx;
    Parser p;
    Expr *e;
    if (src_out)
        *src_out = src;
    lex_source(a, src, &lx);
    ps_init(&p, a, src, &lx);
    p.ctx.nl_ignore = 1;
    e = ps_expr(&p);
    if (!p.panic && ps_kind(&p) != TK_EOF)
        ps_error(&p, ps_err_span(&p), "unexpected %s after the expression", ps_describe(&p));
    ps_flush_lex(&p, UINT32_MAX);
    return e;
}

Type *parse_type_string(Arena *a, const char *text, size_t len, const char *name, uint32_t *src_out)
{
    uint32_t src = src_add(name ? name : "<type>", text, len);
    LexOutput lx;
    Parser p;
    Type *t;
    if (src_out)
        *src_out = src;
    lex_source(a, src, &lx);
    ps_init(&p, a, src, &lx);
    p.ctx.nl_ignore = 1;
    t = ps_type(&p);
    if (!p.panic && ps_kind(&p) != TK_EOF)
        ps_error(&p, ps_err_span(&p), "unexpected %s after the type", ps_describe(&p));
    ps_flush_lex(&p, UINT32_MAX);
    return t;
}

/* ------------------------------------------------------------ op names */

const char *ast_unary_op_text(UnaryOp op)
{
    switch (op) {
    case UN_NEG: return "-";
    case UN_NOT: return "!";
    case UN_BITNOT: return "~";
    case UN_RED_AND: return "&";
    case UN_RED_OR: return "|";
    case UN_RED_XOR: return "^";
    }
    return "?";
}

const char *ast_binary_op_text(BinaryOp op)
{
    switch (op) {
    case BIN_MUL: return "*";
    case BIN_DIV: return "/";
    case BIN_MOD: return "%";
    case BIN_ADD: return "+";
    case BIN_SUB: return "-";
    case BIN_ADD_WRAP: return "+%";
    case BIN_SUB_WRAP: return "-%";
    case BIN_SHL: return "<<";
    case BIN_SHR: return ">>";
    case BIN_SHRA: return ">>>";
    case BIN_BITAND: return "&";
    case BIN_BITXOR: return "^";
    case BIN_BITOR: return "|";
    case BIN_CONCAT: return "++";
    case BIN_EQ: return "==";
    case BIN_NE: return "!=";
    case BIN_LT: return "<";
    case BIN_LE: return "<=";
    case BIN_GT: return ">";
    case BIN_GE: return ">=";
    case BIN_IN: return "in";
    case BIN_AND: return "and";
    case BIN_XOR: return "xor";
    case BIN_OR: return "or";
    case BIN_UNTIL: return "until";
    case BIN_IMPLIES: return "|->";
    case BIN_IMPLIES_NEXT: return "|=>";
    }
    return "?";
}

#undef PS_TAKE
#undef PS_BP_RANGE
#undef PS_BP_TEMP_BODY
#undef PS_BP_IMPLIES
#undef PS_BP_EVENTUALLY
#undef PS_BP_UNTIL
#undef PS_BP_DELAY
#undef PS_BP_OR
#undef PS_BP_XOR
#undef PS_BP_AND
#undef PS_BP_CMP
#undef PS_BP_CONCAT
#undef PS_BP_BITOR
#undef PS_BP_BITXOR
#undef PS_BP_BITAND
#undef PS_BP_SHIFT
#undef PS_BP_ADD
#undef PS_BP_MUL
#undef PS_BP_UNARY
