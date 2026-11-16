/* fmt.c — `tern fmt`: the canonical formatter, and the naming-convention
 * check (spec §3.2, §13.2; the style is documented in docs/style.md).
 *
 * The formatter prints the AST again rather than editing the token stream,
 * so the layout is canonical. A few author choices are deliberately kept and
 * are read back from the source text (the AST file keeps tokens and
 * comments): blank lines inside bodies (at most one), statements joined by
 * `;` on one line, one-line blocks, lists the author broke after the opening
 * bracket, line breaks after binary operators and before `else`, the
 * placement of statement attributes, the spelling of literals and of `!` vs
 * `not`, and the column of trailing comments.
 *
 * Layout engine: output goes to a StrBuf with lazy indentation and lazy
 * spaces (never trailing whitespace). A breakable construct (argument and
 * generic lists, struct literals, brace bodies, blocks, match arms) is first
 * rendered "flat" on the current line; a newline, a comment or exceeding the
 * width (100) makes the attempt fail, the output is rewound and the
 * construct is rendered broken (one element per line, trailing comma). A
 * construct is broken without an attempt when it contains a comment or the
 * author broke it (a line break right after the opening bracket; for blocks
 * and bodies, any line break inside). Every decision depends only on the
 * source layout facts the output reproduces, which makes fmt idempotent.
 *
 * Comments are emitted by position: before each expression, statement,
 * list element and closing bracket, every comment that starts before it is
 * flushed. A comment that followed code on its line stays trailing (keeping
 * its column when the code still ends to the left of it, else one space
 * after the code); others go on their own line at the current indentation.
 * `///` doc comments are ordinary comments here: they keep their text and
 * relative order, so they stay attached to the same token.
 *
 * Safety: cmd_fmt re-parses the result and compares the AST (ignoring
 * positions) and the comments with the original (fmt_verify) before writing
 * anything. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/diag.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../syntax/parser.h"
#include "../cmds.h"
#include "tools.h"

#define FMT_WIDTH 100
#define FMT_IND 4

typedef struct FmtState {
    const AstFile *f;
    const char *src;
    uint32_t srclen;
    StrBuf *out;
    int indent;         /* indentation of new lines */
    int next_ind;       /* indentation of the next line when >= 0 (continuation) */
    int line_ind;       /* indentation written on the current line */
    int cont;           /* continuation indentation inside the current expression */
    int col;            /* output column (code points) */
    int bol;            /* nothing written on the current line yet */
    int pending_nl;     /* a line comment ended the line */
    int pending_sp;     /* a space goes before the next text (dropped at line start) */
    int after_open;     /* the last thing written opened a bracket: no blank line now */
    uint32_t ci;        /* next comment to emit */
    int flat;           /* > 0: everything must stay on the current line */
    int temporal;       /* inside a property/sequence body (line breaks are free) */
    uint32_t hint;      /* source offset of the node being written */
    uint32_t line_hint; /* `hint` when the current output line started */
    int fail;           /* the flat attempt failed */
} FmtState;

typedef struct FmtMark {
    size_t len;
    int col, bol, pending_nl, pending_sp, after_open, line_ind, next_ind, indent, cont;
    uint32_t ci;
} FmtMark;

static void fmt_expr(FmtState *st, const Expr *e);
static void fmt_type(FmtState *st, const Type *t);
static void fmt_pat(FmtState *st, const Pattern *p);
static void fmt_stmt(FmtState *st, const Stmt *s);
static void fmt_item(FmtState *st, const Item *it);
static void fmt_block(FmtState *st, const Block *b);

/* --------------------------------------------------------------- output */

static int fmt_cols(const char *s, size_t n)
{
    int c = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            c++;
    }
    return c;
}

static void fmt_nl(FmtState *st)
{
    if (st->flat) {
        st->fail = 1;
        return;
    }
    if (!st->bol) {
        while (st->out->len > 0 && st->out->data[st->out->len - 1] == ' ')
            st->out->len--;
        sb_putc(st->out, '\n');
        st->bol = 1;
        st->col = 0;
    }
    st->pending_nl = 0;
    st->pending_sp = 0;
    st->next_ind = -1;
}

/* One empty line (never two, never at the start or right after a bracket). */
static void fmt_blank(FmtState *st)
{
    fmt_nl(st);
    if (st->flat || st->after_open || st->out->len == 0)
        return;
    if (st->out->len >= 2 && st->out->data[st->out->len - 1] == '\n' && st->out->data[st->out->len - 2] == '\n')
        return;
    sb_putc(st->out, '\n');
}

static void fmt_raw(FmtState *st, const char *s, size_t n)
{
    if (st->fail)
        return;
    if (st->pending_nl) {
        if (st->flat) {
            st->fail = 1;
            return;
        }
        fmt_nl(st);
        st->next_ind = st->cont;
    }
    if (st->bol) {
        int ind = st->next_ind >= 0 ? st->next_ind : st->indent;
        st->line_hint = st->hint;
        sb_indent(st->out, ind);
        st->line_ind = ind;
        st->col = ind;
        st->bol = 0;
        st->next_ind = -1;
    } else if (st->pending_sp) {
        sb_putc(st->out, ' ');
        st->col++;
    }
    st->pending_sp = 0;
    st->after_open = 0;
    sb_append(st->out, s, n);
    st->col += fmt_cols(s, n);
    if (st->flat && st->col > FMT_WIDTH)
        st->fail = 1;
}

static void fmt_hint(FmtState *st, uint32_t off)
{
    st->hint = off;
}

static void fmt_str(FmtState *st, const char *s)
{
    fmt_raw(st, s, strlen(s));
}

static void fmt_sp(FmtState *st)
{
    st->pending_sp = 1;
}

static void fmt_open(FmtState *st, const char *s)
{
    fmt_str(st, s);
    st->after_open = 1;
}

static void fmt_name(FmtState *st, Ident id)
{
    fmt_str(st, id.name ? id.name : "_");
}

/* Indentation the current (or next) line has. */
static int fmt_line_base(const FmtState *st)
{
    if (st->bol)
        return st->next_ind >= 0 ? st->next_ind : st->indent;
    return st->line_ind;
}

static FmtMark fmt_mark(const FmtState *st)
{
    FmtMark m;
    m.len = st->out->len;
    m.col = st->col;
    m.bol = st->bol;
    m.pending_nl = st->pending_nl;
    m.pending_sp = st->pending_sp;
    m.after_open = st->after_open;
    m.line_ind = st->line_ind;
    m.next_ind = st->next_ind;
    m.indent = st->indent;
    m.cont = st->cont;
    m.ci = st->ci;
    return m;
}

static void fmt_rewind(FmtState *st, const FmtMark *m)
{
    st->out->len = m->len;
    if (st->out->data)
        st->out->data[m->len] = '\0';
    st->col = m->col;
    st->bol = m->bol;
    st->pending_nl = m->pending_nl;
    st->pending_sp = m->pending_sp;
    st->after_open = m->after_open;
    st->line_ind = m->line_ind;
    st->next_ind = m->next_ind;
    st->indent = m->indent;
    st->cont = m->cont;
    st->ci = m->ci;
    st->fail = 0;
}

/* Flat attempts: only started when not already flat. */
static void fmt_flat_begin(FmtState *st, FmtMark *m)
{
    *m = fmt_mark(st);
    st->flat++;
}

static int fmt_flat_end(FmtState *st, const FmtMark *m)
{
    st->flat--;
    if (!st->fail)
        return 1;
    fmt_rewind(st, m);
    return 0;
}

/* ---------------------------------------------------------- source facts */

static uint32_t fmt_end(Span sp)
{
    return sp.off + sp.len;
}

static int fmt_src_has_nl(const FmtState *st, uint32_t a, uint32_t b)
{
    if (b > st->srclen)
        b = st->srclen;
    return a < b && memchr(st->src + a, '\n', b - a) != NULL;
}

/* Only whitespace precedes `off` on its line? */
static int fmt_src_line_start(const FmtState *st, uint32_t off)
{
    while (off > 0 && st->src[off - 1] != '\n') {
        char c = st->src[off - 1];
        if (c != ' ' && c != '\t' && c != '\r')
            return 0;
        off--;
    }
    return 1;
}

/* Only whitespace follows `off` up to the end of its line? */
static int fmt_src_eol_after(const FmtState *st, uint32_t off)
{
    while (off < st->srclen && st->src[off] != '\n') {
        char c = st->src[off];
        if (c != ' ' && c != '\t' && c != '\r')
            return 0;
        off++;
    }
    return 1;
}

/* An empty line lies between the previous token/comment and `off`. */
static int fmt_src_blank_before(const FmtState *st, uint32_t off)
{
    int nls = 0;
    while (off > 0) {
        char c = st->src[off - 1];
        if (c == '\n')
            nls++;
        else if (c != ' ' && c != '\t' && c != '\r')
            break;
        off--;
    }
    return nls >= 2 && off > 0;
}

/* Index of the first token at or after `off`. */
static uint32_t fmt_tok_lb(const FmtState *st, uint32_t off)
{
    uint32_t lo = 0, hi = st->f->ntokens;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (st->f->tokens[mid].off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static const Token *fmt_tok_at(const FmtState *st, uint32_t off)
{
    uint32_t i = fmt_tok_lb(st, off);
    return i < st->f->ntokens ? &st->f->tokens[i] : &st->f->tokens[st->f->ntokens - 1];
}

/* Offset of the first token of kind `k` at or after `off`. */
static uint32_t fmt_find(const FmtState *st, uint32_t off, TokenKind k)
{
    uint32_t i;
    for (i = fmt_tok_lb(st, off); i < st->f->ntokens; i++) {
        if (st->f->tokens[i].kind == k)
            return st->f->tokens[i].off;
    }
    return off;
}

/* Offset of the bracket matching the one at `open`. */
static uint32_t fmt_match_close(const FmtState *st, uint32_t open)
{
    uint32_t i = fmt_tok_lb(st, open);
    int depth = 0;
    for (; i < st->f->ntokens; i++) {
        TokenKind k = (TokenKind)st->f->tokens[i].kind;
        if (k == TK_LPAREN || k == TK_LBRACKET || k == TK_LBRACE || k == TK_ATTR)
            depth++;
        else if (k == TK_RPAREN || k == TK_RBRACKET || k == TK_RBRACE) {
            if (--depth == 0)
                return st->f->tokens[i].off;
        }
    }
    return open;
}

/* A line break follows the token at `open`. */
static int fmt_nl_after(const FmtState *st, uint32_t open)
{
    uint32_t i = fmt_tok_lb(st, open);
    return i + 1 < st->f->ntokens && st->f->tokens[i + 1].nl_before;
}

static void fmt_src_text(FmtState *st, uint32_t off, uint32_t len)
{
    fmt_raw(st, st->src + off, len);
}

/* --------------------------------------------------------------- comments */

static int fmt_has_comment(const FmtState *st, uint32_t a, uint32_t b)
{
    uint32_t lo = 0, hi = st->f->ncomments;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (st->f->comments[mid].span.off < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < st->f->ncomments && st->f->comments[lo].span.off < b;
}

static void fmt_emit_comment(FmtState *st, const Comment *c, int force_blank)
{
    const char *text = st->src + c->span.off;
    size_t len = c->span.len;
    uint32_t end = fmt_end(c->span);
    int multi = memchr(text, '\n', len) != NULL;
    if (!st->bol && !st->pending_nl && !multi && !fmt_src_line_start(st, c->span.off)) {
        /* trailing: keep the column when the code still ends left of it and
         * the line starts with the same code as in the source */
        int want = (int)c->span.col - 1;
        uint32_t hline = 0, hcol = 0;
        src_linecol(st->f->src, st->line_hint, &hline, &hcol);
        if (hline != c->span.line || !fmt_src_line_start(st, st->line_hint) || want < st->col + 1)
            want = st->col + 1;
        sb_indent(st->out, want - st->col);
        sb_append(st->out, text, len);
        st->col = want + fmt_cols(text, len);
        st->pending_sp = 0;
        st->after_open = 0;
        if (c->kind != COMMENT_BLOCK || fmt_src_eol_after(st, end))
            st->pending_nl = 1;
        else
            st->pending_sp = 1;
        return;
    }
    fmt_nl(st);
    if (force_blank || fmt_src_blank_before(st, c->span.off))
        fmt_blank(st);
    fmt_raw(st, text, len);
    if (c->kind == COMMENT_BLOCK && !multi && !fmt_src_eol_after(st, end))
        st->pending_sp = 1;
    else
        fmt_nl(st);
}

static void fmt_comments_before(FmtState *st, uint32_t off)
{
    while (st->ci < st->f->ncomments && st->f->comments[st->ci].span.off < off) {
        if (st->flat) {
            st->fail = 1;
            return;
        }
        fmt_emit_comment(st, &st->f->comments[st->ci++], 0);
    }
}

/* ------------------------------------------------------------------ lists */

typedef void (*FmtElemFn)(FmtState *st, const void *ctx, uint32_t i);
typedef uint32_t (*FmtOffFn)(const void *ctx, uint32_t i);
typedef int (*FmtNoCommaFn)(const void *ctx, uint32_t i);

typedef struct FmtList {
    const char *open, *close;
    uint32_t open_off, close_off;   /* bracket offsets in the source */
    uint32_t n;
    int spaces;                     /* `{ a, b }` when flat */
    int one_comma;                  /* a single element keeps its comma: `(a,)` */
    int force;                      /* always one element per line */
    int hug;                        /* a single element is never put on its own line */
    int fill;                       /* broken: as many elements per line as fit (tables) */
    int reserve;                    /* columns needed after the closing bracket */
    FmtElemFn elem;
    FmtOffFn off;
    FmtOffFn end;                   /* end offset of an element (fill only) */
    FmtNoCommaFn no_comma;          /* element takes no comma (bundle fns), or NULL */
    const void *ctx;
} FmtList;

static void fmt_list_flat(FmtState *st, const FmtList *L)
{
    uint32_t i;
    fmt_open(st, L->open);
    if (L->spaces)
        fmt_sp(st);
    for (i = 0; i < L->n && !st->fail; i++) {
        if (i) {
            fmt_str(st, ",");
            fmt_sp(st);
        }
        L->elem(st, L->ctx, i);
    }
    if (L->n == 1 && L->one_comma)
        fmt_str(st, ",");
    if (L->spaces)
        fmt_sp(st);
    fmt_str(st, L->close);
    if (st->flat && st->col + L->reserve > FMT_WIDTH)
        st->fail = 1;
}

/* Table layout: elements fill the lines; a line breaks where the author broke
 * it or where the next element would not fit. */
static void fmt_list_fill(FmtState *st, const FmtList *L)
{
    uint32_t i;
    int saved_indent = st->indent, saved_cont = st->cont, base;
    fmt_open(st, L->open);
    base = st->line_ind;
    st->indent = base + FMT_IND;
    st->cont = st->indent + FMT_IND;
    for (i = 0; i < L->n; i++) {
        uint32_t off = L->off(L->ctx, i);
        int brk = 1;
        if (i > 0) {
            uint32_t prev_end = L->end(L->ctx, i - 1);
            fmt_str(st, ",");
            brk = fmt_src_has_nl(st, prev_end, off) || fmt_has_comment(st, prev_end, off);
            if (!brk) {
                FmtMark m;
                fmt_flat_begin(st, &m);
                fmt_sp(st);
                L->elem(st, L->ctx, i);
                fmt_str(st, ",");
                brk = st->fail;
                st->flat--;
                fmt_rewind(st, &m);
            }
        }
        if (brk) {
            fmt_comments_before(st, off);
            fmt_nl(st);
            if (i > 0 && fmt_src_blank_before(st, off))
                fmt_blank(st);
        } else {
            fmt_sp(st);
        }
        L->elem(st, L->ctx, i);
    }
    fmt_str(st, ",");
    fmt_comments_before(st, L->close_off);
    st->indent = base;
    fmt_nl(st);
    fmt_hint(st, L->close_off);
    fmt_str(st, L->close);
    st->indent = saved_indent;
    st->cont = saved_cont;
}

static void fmt_list_broken(FmtState *st, const FmtList *L)
{
    uint32_t i;
    int saved_indent = st->indent, saved_cont = st->cont, base;
    fmt_open(st, L->open);
    base = st->line_ind;
    st->indent = base + FMT_IND;
    for (i = 0; i < L->n; i++) {
        uint32_t off = L->off(L->ctx, i);
        fmt_comments_before(st, off);
        fmt_nl(st);
        if (fmt_src_blank_before(st, off))
            fmt_blank(st);
        st->cont = st->indent + FMT_IND;
        L->elem(st, L->ctx, i);
        if (!L->no_comma || !L->no_comma(L->ctx, i))
            fmt_str(st, ",");
    }
    fmt_comments_before(st, L->close_off);
    st->indent = base;
    fmt_nl(st);
    fmt_hint(st, L->close_off);
    fmt_str(st, L->close);
    st->indent = saved_indent;
    st->cont = saved_cont;
}

static void fmt_list(FmtState *st, const FmtList *L)
{
    int forced;
    FmtMark m;
    fmt_comments_before(st, L->open_off);
    if (L->n == 0 && !fmt_has_comment(st, L->open_off + 1, L->close_off)) {
        fmt_str(st, L->open);
        fmt_str(st, L->close);
        return;
    }
    forced = L->force || fmt_has_comment(st, L->open_off + 1, L->close_off) || fmt_nl_after(st, L->open_off);
    if (st->flat) {
        if (forced)
            st->fail = 1;
        else
            fmt_list_flat(st, L);
        return;
    }
    if (!forced && L->hug && L->n == 1) {
        /* `f(x)`: the element breaks inside if it must, the brackets hug it */
        fmt_open(st, L->open);
        L->elem(st, L->ctx, 0);
        if (L->one_comma)
            fmt_str(st, ",");
        fmt_str(st, L->close);
        return;
    }
    if (!forced) {
        fmt_flat_begin(st, &m);
        fmt_list_flat(st, L);
        if (fmt_flat_end(st, &m))
            return;
    }
    if (L->fill && L->end)
        fmt_list_fill(st, L);
    else
        fmt_list_broken(st, L);
}

static void fmt_list_init(FmtList *L, const char *open, const char *close, uint32_t open_off, uint32_t close_off,
                          uint32_t n, FmtElemFn elem, FmtOffFn off, const void *ctx)
{
    memset(L, 0, sizeof *L);
    L->open = open;
    L->close = close;
    L->open_off = open_off;
    L->close_off = close_off;
    L->n = n;
    L->elem = elem;
    L->off = off;
    L->ctx = ctx;
}

/* ------------------------------------------------------------- attributes */

static void fmt_attr_arg(FmtState *st, const void *ctx, uint32_t i)
{
    const AttrArg *a = &((const AttrArg *)ctx)[i];
    if (a->name.name) {
        fmt_name(st, a->name);
        fmt_str(st, ":");
        fmt_sp(st);
    }
    fmt_expr(st, a->value);
}

static uint32_t fmt_attr_arg_off(const void *ctx, uint32_t i)
{
    return ((const AttrArg *)ctx)[i].span.off;
}

static void fmt_attr(FmtState *st, const Attr *a)
{
    fmt_comments_before(st, a->span.off);
    fmt_hint(st, a->span.off);
    fmt_str(st, "#[");
    fmt_name(st, a->name);
    if (a->has_args) {
        FmtList L;
        uint32_t open = fmt_find(st, fmt_end(a->name.span), TK_LPAREN);
        fmt_list_init(&L, "(", ")", open, fmt_match_close(st, open), a->nargs, fmt_attr_arg, fmt_attr_arg_off,
                      a->args);
        L.hug = 1;
        fmt_list(st, &L);
    }
    fmt_str(st, "]");
}

/* Attributes written before a declaration starting at `kw_off`: each on its
 * own line when `own_line` or when the author put a line break after it,
 * else followed by a space. Attributes at or after kw_off are skipped. */
static void fmt_lead_attrs(FmtState *st, AttrList attrs, uint32_t kw_off, int own_line)
{
    uint32_t i;
    for (i = 0; i < attrs.n; i++) {
        const Attr *a = &attrs.items[i];
        if (a->span.off >= kw_off)
            continue;
        fmt_attr(st, a);
        if (own_line || fmt_src_has_nl(st, fmt_end(a->span), i + 1 < attrs.n && attrs.items[i + 1].span.off < kw_off
                                                                 ? attrs.items[i + 1].span.off
                                                                 : kw_off))
            fmt_nl(st);
        else
            fmt_sp(st);
    }
}

/* Offset of the first token after the leading attributes of a declaration
 * that starts at `start` (`pub`, the keyword or the name). */
static uint32_t fmt_kw_off(const FmtState *st, uint32_t start, AttrList attrs)
{
    uint32_t i = fmt_tok_lb(st, start), k;
    for (k = 0; k < attrs.n && i < st->f->ntokens; k++) {
        if (st->f->tokens[i].off != attrs.items[k].span.off)
            break;
        i = fmt_tok_lb(st, fmt_end(attrs.items[k].span));
    }
    return i < st->f->ntokens ? st->f->tokens[i].off : start;
}

/* -------------------------------------------------------- paths, generics */

static void fmt_garg(FmtState *st, const void *ctx, uint32_t i)
{
    const GenericArg *g = &((const GenericArg *)ctx)[i];
    if (g->name.name) {
        fmt_name(st, g->name);
        fmt_str(st, ":");
        fmt_sp(st);
    }
    fmt_expr(st, g->expr);
}

static uint32_t fmt_garg_off(const void *ctx, uint32_t i)
{
    return ((const GenericArg *)ctx)[i].span.off;
}

static void fmt_gargs(FmtState *st, uint32_t name_end, uint32_t seg_end, const GenericArg *args, uint32_t n)
{
    FmtList L;
    uint32_t open = fmt_find(st, name_end, TK_LT);
    uint32_t close = seg_end > 0 ? seg_end - 1 : open;
    fmt_list_init(&L, "<", ">", open, close, n, fmt_garg, fmt_garg_off, args);
    L.hug = 1;
    fmt_list(st, &L);
}

static void fmt_path(FmtState *st, const Path *p)
{
    uint32_t i;
    for (i = 0; i < p->nsegs; i++) {
        const PathSeg *s = &p->segs[i];
        if (i)
            fmt_str(st, "::");
        fmt_name(st, s->name);
        if (s->has_args)
            fmt_gargs(st, fmt_end(s->name.span), fmt_end(s->span), s->args, s->nargs);
    }
}

static void fmt_gparam(FmtState *st, const void *ctx, uint32_t i)
{
    const GenericParam *g = &((const GenericParam *)ctx)[i];
    fmt_name(st, g->name);
    fmt_str(st, ":");
    fmt_sp(st);
    if (g->kind == GENERIC_TYPE) {
        fmt_str(st, "type");
        if (g->default_type) {
            fmt_sp(st);
            fmt_str(st, "=");
            fmt_sp(st);
            fmt_type(st, g->default_type);
        }
    } else {
        fmt_type(st, g->type);
        if (g->default_value) {
            fmt_sp(st);
            fmt_str(st, "=");
            fmt_sp(st);
            fmt_expr(st, g->default_value);
        }
    }
}

static uint32_t fmt_gparam_off(const void *ctx, uint32_t i)
{
    return ((const GenericParam *)ctx)[i].span.off;
}

static void fmt_gparams(FmtState *st, const GenericParams *g, int reserve)
{
    FmtList L;
    if (!g->present)
        return;
    fmt_list_init(&L, "<", ">", g->span.off, fmt_end(g->span) - 1, g->n, fmt_gparam, fmt_gparam_off, g->items);
    L.reserve = reserve;
    fmt_list(st, &L);
}

/* `where a, b, ...` on its own line; breaks after a comma where the author
 * did or where the next constraint would not fit (continuation aligned
 * after `where `). */
static void fmt_where(FmtState *st, const WhereList *w)
{
    uint32_t i;
    int saved_cont = st->cont, align;
    fmt_comments_before(st, w->items[0]->span.off);
    fmt_nl(st);
    fmt_str(st, "where");
    fmt_sp(st);
    align = st->line_ind + 6;
    st->cont = align;
    for (i = 0; i < w->n; i++) {
        const Expr *c = w->items[i];
        if (i) {
            int brk = fmt_src_has_nl(st, fmt_end(w->items[i - 1]->span), c->span.off);
            fmt_str(st, ",");
            if (!brk) {
                FmtMark m;
                int width;
                fmt_flat_begin(st, &m);
                fmt_sp(st);
                fmt_expr(st, c);
                width = st->col;
                brk = st->fail || width > FMT_WIDTH;
                st->flat--;
                fmt_rewind(st, &m);
            }
            if (brk) {
                fmt_comments_before(st, c->span.off);
                fmt_nl(st);
                st->next_ind = align;
            } else {
                fmt_sp(st);
            }
        }
        fmt_expr(st, c);
    }
    st->cont = saved_cont;
}

/* ------------------------------------------------------------------ types */

static void fmt_field(FmtState *st, const Field *f)
{
    fmt_comments_before(st, f->span.off);
    {
        uint32_t kw = fmt_kw_off(st, f->span.off, f->attrs);
        fmt_lead_attrs(st, f->attrs, kw, 0);
        fmt_comments_before(st, kw);
    }
    if (f->flip) {
        fmt_str(st, "flip");
        fmt_sp(st);
    }
    fmt_name(st, f->name);
    fmt_str(st, ":");
    fmt_sp(st);
    fmt_type(st, f->type);
}

static void fmt_field_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_field(st, ((Field *const *)ctx)[i]);
}

static uint32_t fmt_field_off(const void *ctx, uint32_t i)
{
    return ((Field *const *)ctx)[i]->span.off;
}

static void fmt_fields(FmtState *st, uint32_t open, uint32_t close, Field *const *fields, uint32_t n)
{
    FmtList L;
    fmt_list_init(&L, "{", "}", open, close, n, fmt_field_elem, fmt_field_off, fields);
    L.spaces = 1;
    fmt_list(st, &L);
}

static void fmt_type_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_type(st, ((Type *const *)ctx)[i]);
}

static uint32_t fmt_type_off(const void *ctx, uint32_t i)
{
    return ((Type *const *)ctx)[i]->span.off;
}

static void fmt_type(FmtState *st, const Type *t)
{
    fmt_comments_before(st, t->span.off);
    fmt_hint(st, t->span.off);
    switch (t->kind) {
    case TYPE_ERROR:
        fmt_str(st, "_");
        break;
    case TYPE_PATH:
        fmt_path(st, &t->u.path);
        break;
    case TYPE_ARRAY:
        fmt_open(st, "[");
        fmt_type(st, t->u.array.elem);
        fmt_str(st, ";");
        fmt_sp(st);
        fmt_expr(st, t->u.array.len);
        fmt_str(st, "]");
        break;
    case TYPE_STRUCT: {
        uint32_t open = fmt_find(st, t->span.off, TK_LBRACE);
        fmt_str(st, "struct");
        fmt_sp(st);
        fmt_fields(st, open, fmt_end(t->span) - 1, t->u.anon_struct.fields, t->u.anon_struct.nfields);
        break;
    }
    case TYPE_TYPEOF:
        fmt_open(st, "typeof(");
        fmt_expr(st, t->u.typeof_expr);
        fmt_str(st, ")");
        break;
    case TYPE_TUPLE: {
        FmtList L;
        fmt_list_init(&L, "(", ")", t->span.off, fmt_end(t->span) - 1, t->u.tuple.n, fmt_type_elem, fmt_type_off,
                      t->u.tuple.elems);
        L.one_comma = 1;
        fmt_list(st, &L);
        break;
    }
    }
}

/* ------------------------------------------------------------ expressions */

typedef struct FmtInits {
    const FieldInit *fields;
    uint32_t n;
    int has_rest;
    uint32_t rest_off;
} FmtInits;

static void fmt_init_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const FmtInits *fi = (const FmtInits *)ctx;
    const FieldInit *f;
    if (i == fi->n) {
        fmt_str(st, "..");
        return;
    }
    f = &fi->fields[i];
    fmt_comments_before(st, f->span.off);
    fmt_name(st, f->name);
    if (f->value) {
        fmt_str(st, ":");
        fmt_sp(st);
        fmt_expr(st, f->value);
    }
}

static uint32_t fmt_init_off(const void *ctx, uint32_t i)
{
    const FmtInits *fi = (const FmtInits *)ctx;
    return i == fi->n ? fi->rest_off : fi->fields[i].span.off;
}

/* Offset of the `..` token of a brace list whose `}` is at `close`. */
static uint32_t fmt_rest_off(const FmtState *st, uint32_t close)
{
    uint32_t i = fmt_tok_lb(st, close);
    while (i > 0) {
        i--;
        if (st->f->tokens[i].kind == TK_DOTDOT)
            return st->f->tokens[i].off;
    }
    return close;
}

static void fmt_inits(FmtState *st, uint32_t open, uint32_t close, const FieldInit *fields, uint32_t n, int has_rest)
{
    FmtList L;
    FmtInits fi;
    fi.fields = fields;
    fi.n = n;
    fi.has_rest = has_rest;
    fi.rest_off = has_rest ? fmt_rest_off(st, close) : close;
    fmt_list_init(&L, "{", "}", open, close, n + (has_rest ? 1u : 0u), fmt_init_elem, fmt_init_off, &fi);
    L.spaces = 1;
    fmt_list(st, &L);
}

static void fmt_arg_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const Arg *a = &((const Arg *)ctx)[i];
    fmt_comments_before(st, a->span.off);
    if (a->name.name) {
        fmt_name(st, a->name);
        fmt_str(st, ":");
        fmt_sp(st);
    }
    fmt_expr(st, a->value);
}

static uint32_t fmt_arg_off(const void *ctx, uint32_t i)
{
    return ((const Arg *)ctx)[i].span.off;
}

static void fmt_expr_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_expr(st, ((Expr *const *)ctx)[i]);
}

static uint32_t fmt_expr_off(const void *ctx, uint32_t i)
{
    return ((Expr *const *)ctx)[i]->span.off;
}

static uint32_t fmt_expr_end(const void *ctx, uint32_t i)
{
    return fmt_end(((Expr *const *)ctx)[i]->span);
}

/* Elements of an array literal laid out as a table: literals, names,
 * variants, `-x`, and tuples of those. */
static int fmt_is_table_cell(const Expr *e, int allow_tuple)
{
    uint32_t i;
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_BOOL:
    case EXPR_FILL:
    case EXPR_STRING:
        return 1;
    case EXPR_PATH:
        return e->u.path.nsegs <= 2 && !e->u.path.segs[e->u.path.nsegs - 1].has_args;
    case EXPR_VARIANT:
        return !e->u.variant.has_fields;
    case EXPR_UNARY:
        return fmt_is_table_cell(e->u.unary.operand, 0);
    case EXPR_TUPLE:
        if (!allow_tuple)
            return 0;
        for (i = 0; i < e->u.list.n; i++) {
            if (!fmt_is_table_cell(e->u.list.items[i], 0))
                return 0;
        }
        return 1;
    default:
        return 0;
    }
}

/* Space, or a line break where the author broke between `a` and `b`. */
static void fmt_break_or_space(FmtState *st, uint32_t gap_start, uint32_t next_off)
{
    if (fmt_src_has_nl(st, gap_start, next_off)) {
        fmt_comments_before(st, next_off);
        fmt_nl(st);
        st->next_ind = st->cont;
    } else {
        fmt_sp(st);
    }
}

/* ` = value`, keeping an author line break after the `=`. */
static void fmt_assign_rhs(FmtState *st, const Expr *rhs)
{
    const Token *t = fmt_tok_at(st, rhs->span.off);
    fmt_sp(st);
    fmt_str(st, "=");
    if (t->nl_before) {
        fmt_comments_before(st, rhs->span.off);
        fmt_nl(st);
        st->next_ind = st->cont;
    } else {
        fmt_sp(st);
    }
    fmt_expr(st, rhs);
}

static void fmt_int(FmtState *st, const Expr *e)
{
    const Token *t = fmt_tok_at(st, e->span.off);
    if (t->off != e->span.off || t->kind != TK_INT) {
        fmt_str(st, e->u.int_lit.text);
        return;
    }
    fmt_src_text(st, t->off, t->len);
    if (t->suffix && t->width_len == 0 && e->u.int_lit.width) {
        fmt_str(st, "<");
        fmt_expr(st, e->u.int_lit.width);
        fmt_str(st, ">");
    }
}

/* Is the block on one source line (from `{` to `}`)? */
static int fmt_block_one_line(const FmtState *st, const Block *b)
{
    return !fmt_src_has_nl(st, b->span.off, fmt_end(b->span));
}

static void fmt_stmts(FmtState *st, Stmt *const *stmts, uint32_t n, const Block *blk)
{
    uint32_t i, prev_end = 0;
    for (i = 0; i < n; i++) {
        const Stmt *s = stmts[i];
        uint32_t off = s->span.off;
        if (i > 0 && !fmt_src_has_nl(st, prev_end, off) && !fmt_has_comment(st, prev_end, off)) {
            fmt_str(st, ";");
            fmt_sp(st);
        } else {
            fmt_comments_before(st, off);
            fmt_nl(st);
            if (fmt_src_blank_before(st, off))
                fmt_blank(st);
        }
        st->cont = fmt_line_base(st) + FMT_IND;
        fmt_stmt(st, s);
        prev_end = fmt_end(s->span);
    }
    if (blk && n > 0 && stmts[n - 1]->kind == STMT_EXPR && !blk->tail)
        fmt_str(st, ";");
}

static void fmt_body_flat(FmtState *st, Stmt *const *stmts, uint32_t n, const Block *blk)
{
    uint32_t i;
    fmt_open(st, "{");
    fmt_sp(st);
    for (i = 0; i < n && !st->fail; i++) {
        if (i) {
            fmt_str(st, ";");
            fmt_sp(st);
        }
        fmt_stmt(st, stmts[i]);
    }
    if (blk && n > 0 && stmts[n - 1]->kind == STMT_EXPR && !blk->tail)
        fmt_str(st, ";");
    fmt_sp(st);
    fmt_str(st, "}");
}

static void fmt_body_broken(FmtState *st, Stmt *const *stmts, uint32_t n, uint32_t close, const Block *blk)
{
    int saved_indent = st->indent, saved_cont = st->cont, base;
    fmt_open(st, "{");
    base = st->line_ind;
    st->indent = base + FMT_IND;
    fmt_stmts(st, stmts, n, blk);
    fmt_comments_before(st, close);
    st->indent = base;
    fmt_nl(st);
    fmt_hint(st, close);
    fmt_str(st, "}");
    st->indent = saved_indent;
    st->cont = saved_cont;
}

/* `{ statements }` from `open` to `close`: on one line when the author wrote
 * it on one line and it fits, else one statement per line. */
static void fmt_body(FmtState *st, Stmt *const *stmts, uint32_t n, uint32_t open, uint32_t close, const Block *blk,
                     int force_broken)
{
    int forced;
    FmtMark m;
    fmt_comments_before(st, open);
    if (n == 0 && !fmt_has_comment(st, open + 1, close)) {
        fmt_str(st, "{}");
        return;
    }
    forced = force_broken || fmt_src_has_nl(st, open, close) || fmt_has_comment(st, open + 1, close);
    if (st->flat) {
        if (forced)
            st->fail = 1;
        else
            fmt_body_flat(st, stmts, n, blk);
        return;
    }
    if (!forced) {
        fmt_flat_begin(st, &m);
        fmt_body_flat(st, stmts, n, blk);
        if (fmt_flat_end(st, &m))
            return;
    }
    fmt_body_broken(st, stmts, n, close, blk);
}

static void fmt_block_mode(FmtState *st, const Block *b, int force_broken)
{
    fmt_body(st, b->stmts, b->nstmts, b->span.off, fmt_end(b->span) - 1, b, force_broken);
}

static void fmt_block(FmtState *st, const Block *b)
{
    fmt_block_mode(st, b, 0);
}

/* Offset of the `else` after an if's then-block. */
static uint32_t fmt_else_off(const FmtState *st, const Expr *e)
{
    return fmt_find(st, fmt_end(e->u.if_expr.then_block->span), TK_KW_ELSE);
}

/* Renders an if/else chain. Stacked: an `else` that the author put on a new
 * line after a one-line block stays on its own line (only after a block
 * that is still on one line). Returns the number of such stacked elses. */
static int fmt_if_chain(FmtState *st, const Expr *e, int stacked, int force_broken)
{
    /* a stacked `else` lines up with the `if` when it starts the line */
    int ind = st->bol && !st->pending_nl ? fmt_line_base(st) : st->cont;
    int nstacked = 0;
    const Expr *cur = e;
    for (;;) {
        uint32_t else_off;
        size_t before;
        int block_broken;
        const Expr *els;
        fmt_comments_before(st, cur->span.off);
        fmt_str(st, "if");
        fmt_sp(st);
        fmt_expr(st, cur->u.if_expr.cond);
        fmt_sp(st);
        before = st->out->len;
        fmt_block_mode(st, cur->u.if_expr.then_block, force_broken);
        block_broken = memchr(st->out->data + before, '\n', st->out->len - before) != NULL;
        els = cur->u.if_expr.else_expr;
        if (!els || st->fail)
            break;
        else_off = fmt_else_off(st, cur);
        fmt_comments_before(st, else_off);
        fmt_hint(st, else_off);
        if (stacked && !block_broken && fmt_tok_at(st, else_off)->nl_before) {
            fmt_nl(st);
            st->next_ind = ind;
            nstacked++;
        } else {
            fmt_sp(st);
        }
        fmt_str(st, "else");
        fmt_sp(st);
        if (els->kind == EXPR_IF) {
            cur = els;
            continue;
        }
        fmt_comments_before(st, els->span.off);
        fmt_block_mode(st, els->u.block, force_broken);
        break;
    }
    return nstacked;
}

static void fmt_if(FmtState *st, const Expr *e)
{
    const Expr *cur;
    int stacked = 0, one_line = 1;
    FmtMark m;
    for (cur = e; cur && cur->kind == EXPR_IF; cur = cur->u.if_expr.else_expr) {
        const Block *tb = cur->u.if_expr.then_block;
        if (!fmt_block_one_line(st, tb))
            one_line = 0;
        if (cur->u.if_expr.else_expr) {
            if (fmt_tok_at(st, fmt_else_off(st, cur))->nl_before && !fmt_src_line_start(st, fmt_end(tb->span) - 1))
                stacked = 1;
            if (cur->u.if_expr.else_expr->kind == EXPR_BLOCK &&
                !fmt_block_one_line(st, cur->u.if_expr.else_expr->u.block))
                one_line = 0;
        }
    }
    if (st->flat) {
        fmt_if_chain(st, e, stacked, 0);
        return;
    }
    if (stacked) {
        m = fmt_mark(st);
        if (fmt_if_chain(st, e, 1, 0) > 0)
            return;
        fmt_rewind(st, &m);
    }
    if (one_line && !fmt_has_comment(st, e->span.off, fmt_end(e->span))) {
        fmt_flat_begin(st, &m);
        fmt_if_chain(st, e, 0, 0);
        if (fmt_flat_end(st, &m))
            return;
    }
    fmt_if_chain(st, e, 0, 1);
}

static void fmt_arm_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const MatchArm *a = &((const MatchArm *)ctx)[i];
    fmt_pat(st, a->pat);
    fmt_sp(st);
    fmt_str(st, "=>");
    fmt_sp(st);
    fmt_stmt(st, a->body);
}

static uint32_t fmt_arm_off(const void *ctx, uint32_t i)
{
    return ((const MatchArm *)ctx)[i].span.off;
}

static void fmt_match(FmtState *st, const Expr *e)
{
    FmtList L;
    uint32_t open = fmt_find(st, fmt_end(e->u.match.scrutinee->span), TK_LBRACE);
    fmt_str(st, "match");
    fmt_sp(st);
    fmt_expr(st, e->u.match.scrutinee);
    fmt_sp(st);
    fmt_list_init(&L, "{", "}", open, fmt_end(e->span) - 1, e->u.match.narms, fmt_arm_elem, fmt_arm_off,
                  e->u.match.arms);
    L.spaces = 1;
    fmt_list(st, &L);
}

static void fmt_expr(FmtState *st, const Expr *e)
{
    uint32_t i;
    fmt_comments_before(st, e->span.off);
    fmt_hint(st, e->span.off);
    switch (e->kind) {
    case EXPR_ERROR:
        fmt_str(st, "_");
        break;
    case EXPR_INT:
        fmt_int(st, e);
        break;
    case EXPR_BOOL:
        fmt_str(st, e->u.bool_val ? "true" : "false");
        break;
    case EXPR_FILL:
        fmt_str(st, e->u.fill_ones ? "all1" : "all0");
        break;
    case EXPR_STRING: {
        const Token *t = fmt_tok_at(st, e->span.off);
        fmt_src_text(st, t->off, t->len);
        break;
    }
    case EXPR_PATH:
        fmt_path(st, &e->u.path);
        break;
    case EXPR_VARIANT:
        fmt_str(st, ".");
        fmt_name(st, e->u.variant.name);
        if (e->u.variant.has_fields) {
            fmt_sp(st);
            fmt_inits(st, fmt_find(st, fmt_end(e->u.variant.name.span), TK_LBRACE), fmt_end(e->span) - 1,
                      e->u.variant.fields, e->u.variant.nfields, 0);
        }
        break;
    case EXPR_STRUCT:
        fmt_path(st, &e->u.struct_lit.path);
        fmt_sp(st);
        fmt_inits(st, fmt_find(st, fmt_end(e->u.struct_lit.path.span), TK_LBRACE), fmt_end(e->span) - 1,
                  e->u.struct_lit.fields, e->u.struct_lit.nfields, e->u.struct_lit.has_rest);
        break;
    case EXPR_FIELD:
        fmt_expr(st, e->u.field.base);
        fmt_str(st, ".");
        fmt_name(st, e->u.field.name);
        if (e->u.field.has_args) {
            uint32_t close = fmt_find(st, fmt_end(e->u.field.name.span), TK_LPAREN);
            fmt_gargs(st, fmt_end(e->u.field.name.span), close, e->u.field.args, e->u.field.nargs);
        }
        break;
    case EXPR_INDEX:
        fmt_expr(st, e->u.index.base);
        fmt_open(st, "[");
        fmt_expr(st, e->u.index.index);
        fmt_str(st, "]");
        break;
    case EXPR_SLICE:
        fmt_expr(st, e->u.slice.base);
        fmt_open(st, "[");
        fmt_expr(st, e->u.slice.hi);
        fmt_str(st, ":");
        fmt_expr(st, e->u.slice.lo);
        fmt_str(st, "]");
        break;
    case EXPR_CALL: {
        FmtList L;
        uint32_t open = fmt_find(st, fmt_end(e->u.call.callee->span), TK_LPAREN);
        fmt_expr(st, e->u.call.callee);
        fmt_list_init(&L, "(", ")", open, fmt_match_close(st, open), e->u.call.nargs, fmt_arg_elem, fmt_arg_off,
                      e->u.call.args);
        L.hug = 1;
        fmt_list(st, &L);
        if (e->u.call.domain) {
            fmt_sp(st);
            fmt_str(st, "@");
            fmt_sp(st);
            fmt_expr(st, e->u.call.domain);
        }
        break;
    }
    case EXPR_CAST:
        fmt_expr(st, e->u.cast.expr);
        fmt_sp(st);
        fmt_str(st, "as");
        fmt_sp(st);
        fmt_type(st, e->u.cast.type);
        break;
    case EXPR_UNARY: {
        const Expr *o = e->u.unary.operand;
        if (e->u.unary.op == UN_NOT && st->src[e->span.off] == 'n') {
            fmt_str(st, "not");
            fmt_sp(st);
        } else {
            fmt_str(st, ast_unary_op_text(e->u.unary.op));
            /* `& &x` must not become `&&x` (and), `| |x` not `||x` (or) */
            if (o->kind == EXPR_UNARY && (e->u.unary.op == UN_RED_AND || e->u.unary.op == UN_RED_OR) &&
                o->u.unary.op == e->u.unary.op)
                fmt_sp(st);
        }
        fmt_expr(st, o);
        break;
    }
    case EXPR_BINARY: {
        uint32_t lhs_end = fmt_end(e->u.binary.lhs->span), op = fmt_tok_at(st, lhs_end)->off;
        fmt_expr(st, e->u.binary.lhs);
        if (st->temporal && fmt_src_has_nl(st, lhs_end, op)) {
            /* properties may break before `|=>`, `and`, ...: keep it there */
            fmt_comments_before(st, op);
            fmt_nl(st);
            st->next_ind = st->cont;
            fmt_str(st, ast_binary_op_text(e->u.binary.op));
            fmt_break_or_space(st, op, e->u.binary.rhs->span.off);
        } else {
            fmt_sp(st);
            fmt_str(st, ast_binary_op_text(e->u.binary.op));
            fmt_break_or_space(st, lhs_end, e->u.binary.rhs->span.off);
        }
        fmt_expr(st, e->u.binary.rhs);
        break;
    }
    case EXPR_RANGE:
        if (e->u.range.lo)
            fmt_expr(st, e->u.range.lo);
        fmt_str(st, e->u.range.inclusive ? "..=" : "..");
        fmt_expr(st, e->u.range.hi);
        break;
    case EXPR_IF:
        fmt_if(st, e);
        break;
    case EXPR_MATCH:
        fmt_match(st, e);
        break;
    case EXPR_BLOCK:
        fmt_block(st, e->u.block);
        break;
    case EXPR_TUPLE:
    case EXPR_ARRAY: {
        FmtList L;
        int tuple = e->kind == EXPR_TUPLE;
        fmt_list_init(&L, tuple ? "(" : "[", tuple ? ")" : "]", e->span.off, fmt_end(e->span) - 1, e->u.list.n,
                      fmt_expr_elem, fmt_expr_off, e->u.list.items);
        L.one_comma = tuple;
        L.hug = 1;
        L.end = fmt_expr_end;
        if (!tuple && e->u.list.n > 1) {
            L.fill = 1;
            for (i = 0; i < e->u.list.n; i++) {
                if (!fmt_is_table_cell(e->u.list.items[i], 1))
                    L.fill = 0;
            }
        }
        fmt_list(st, &L);
        break;
    }
    case EXPR_PAREN:
        fmt_open(st, "(");
        fmt_expr(st, e->u.inner);
        fmt_comments_before(st, fmt_end(e->span) - 1);
        fmt_str(st, ")");
        break;
    case EXPR_SIM:
        fmt_str(st, "sim");
        fmt_sp(st);
        fmt_expr(st, e->u.inner);
        break;
    case EXPR_TYPE:
        fmt_type(st, e->u.type);
        break;
    case EXPR_TEMPORAL:
        fmt_str(st, e->u.temporal.op == TEMP_ALWAYS ? "always" : e->u.temporal.op == TEMP_NEVER ? "never"
                                                                                                   : "eventually");
        if (e->u.temporal.bound) {
            fmt_open(st, "[..");
            fmt_expr(st, e->u.temporal.bound);
            fmt_str(st, "]");
        }
        fmt_sp(st);
        fmt_expr(st, e->u.temporal.body);
        break;
    case EXPR_DELAY:
        if (e->u.delay.lhs) {
            uint32_t lhs_end = fmt_end(e->u.delay.lhs->span);
            fmt_expr(st, e->u.delay.lhs);
            fmt_break_or_space(st, lhs_end, fmt_tok_at(st, lhs_end)->off);
        }
        fmt_str(st, "##");
        if (e->u.delay.ranged) {
            fmt_open(st, "[");
            fmt_expr(st, e->u.delay.lo);
            fmt_str(st, ":");
            if (e->u.delay.unbounded)
                fmt_str(st, "$");
            else
                fmt_expr(st, e->u.delay.hi);
            fmt_str(st, "]");
        } else {
            fmt_expr(st, e->u.delay.lo);
        }
        fmt_sp(st);
        fmt_expr(st, e->u.delay.rhs);
        break;
    }
    TERN_UNUSED(i);
}

/* --------------------------------------------------------------- patterns */

typedef struct FmtFPats {
    const FieldPat *fields;
    uint32_t n;
    uint32_t rest_off;
} FmtFPats;

static void fmt_fpat_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const FmtFPats *fp = (const FmtFPats *)ctx;
    const FieldPat *f;
    if (i == fp->n) {
        fmt_str(st, "..");
        return;
    }
    f = &fp->fields[i];
    fmt_name(st, f->name);
    if (f->pat) {
        fmt_str(st, ":");
        fmt_sp(st);
        fmt_pat(st, f->pat);
    }
}

static uint32_t fmt_fpat_off(const void *ctx, uint32_t i)
{
    const FmtFPats *fp = (const FmtFPats *)ctx;
    return i == fp->n ? fp->rest_off : fp->fields[i].span.off;
}

static void fmt_fpats(FmtState *st, uint32_t open, uint32_t close, const FieldPat *fields, uint32_t n, int has_rest)
{
    FmtList L;
    FmtFPats fp;
    fp.fields = fields;
    fp.n = n;
    fp.rest_off = has_rest ? fmt_rest_off(st, close) : close;
    fmt_list_init(&L, "{", "}", open, close, n + (has_rest ? 1u : 0u), fmt_fpat_elem, fmt_fpat_off, &fp);
    L.spaces = 1;
    fmt_list(st, &L);
}

static void fmt_pat_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_pat(st, ((Pattern *const *)ctx)[i]);
}

static uint32_t fmt_pat_off(const void *ctx, uint32_t i)
{
    return ((Pattern *const *)ctx)[i]->span.off;
}

static void fmt_pat(FmtState *st, const Pattern *p)
{
    uint32_t i;
    fmt_comments_before(st, p->span.off);
    fmt_hint(st, p->span.off);
    switch (p->kind) {
    case PAT_ERROR:
    case PAT_WILDCARD:
        fmt_str(st, "_");
        break;
    case PAT_BIND:
        fmt_name(st, p->u.bind);
        break;
    case PAT_LIT:
        fmt_expr(st, p->u.lit);
        break;
    case PAT_RANGE:
        fmt_expr(st, p->u.range.lo);
        fmt_str(st, p->u.range.inclusive ? "..=" : "..");
        fmt_expr(st, p->u.range.hi);
        break;
    case PAT_VARIANT:
        fmt_str(st, ".");
        fmt_name(st, p->u.variant.name);
        if (p->u.variant.has_fields) {
            fmt_sp(st);
            fmt_fpats(st, fmt_find(st, fmt_end(p->u.variant.name.span), TK_LBRACE), fmt_end(p->span) - 1,
                      p->u.variant.fields, p->u.variant.nfields, p->u.variant.has_rest);
        }
        break;
    case PAT_PATH:
        fmt_path(st, &p->u.path);
        break;
    case PAT_STRUCT:
        fmt_path(st, &p->u.struct_pat.path);
        fmt_sp(st);
        fmt_fpats(st, fmt_find(st, fmt_end(p->u.struct_pat.path.span), TK_LBRACE), fmt_end(p->span) - 1,
                  p->u.struct_pat.fields, p->u.struct_pat.nfields, p->u.struct_pat.has_rest);
        break;
    case PAT_TUPLE: {
        FmtList L;
        fmt_list_init(&L, "(", ")", p->span.off, fmt_end(p->span) - 1, p->u.list.n, fmt_pat_elem, fmt_pat_off,
                      p->u.list.items);
        L.one_comma = 1;
        fmt_list(st, &L);
        break;
    }
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++) {
            if (i) {
                fmt_sp(st);
                fmt_str(st, "|");
                fmt_sp(st);
            }
            fmt_pat(st, p->u.list.items[i]);
        }
        break;
    }
}

/* ------------------------------------------------------------- statements */

typedef struct FmtSA {
    const Expr *e[2];
} FmtSA;

static void fmt_sa_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_expr(st, ((const FmtSA *)ctx)->e[i]);
}

static uint32_t fmt_sa_off(const void *ctx, uint32_t i)
{
    return ((const FmtSA *)ctx)->e[i]->span.off;
}

/* `static_assert(cond[, msg])`; `kw` is the offset of `static_assert`. */
static void fmt_static_assert(FmtState *st, uint32_t kw, const Expr *cond, const Expr *msg)
{
    FmtList L;
    FmtSA sa;
    uint32_t open = fmt_find(st, kw, TK_LPAREN);
    sa.e[0] = cond;
    sa.e[1] = msg;
    fmt_str(st, "static_assert");
    fmt_list_init(&L, "(", ")", open, fmt_match_close(st, open), msg ? 2u : 1u, fmt_sa_elem, fmt_sa_off, &sa);
    fmt_list(st, &L);
}

static void fmt_where_constraint(FmtState *st, const Expr *c)
{
    if (!c)
        return;
    fmt_sp(st);
    fmt_str(st, "where");
    fmt_sp(st);
    fmt_expr(st, c);
}

static void fmt_stmt(FmtState *st, const Stmt *s)
{
    uint32_t kw;
    fmt_comments_before(st, s->span.off);
    fmt_hint(st, s->span.off);
    if (s->kind == STMT_ITEM) {
        fmt_item(st, s->u.item);
        return;
    }
    kw = fmt_kw_off(st, s->span.off, s->attrs);
    if (s->kind == STMT_PORT)
        kw = TERN_MIN(kw, s->u.port.name.span.off);
    fmt_lead_attrs(st, s->attrs, kw, 0);
    fmt_comments_before(st, kw);
    switch (s->kind) {
    case STMT_ERROR:
    case STMT_ITEM:
        break;
    case STMT_PORT: {
        uint32_t i;
        fmt_str(st, s->u.port.dir == PORT_IN ? "in" : "out");
        fmt_sp(st);
        fmt_name(st, s->u.port.name);
        fmt_str(st, ":");
        fmt_sp(st);
        fmt_type(st, s->u.port.type);
        if (s->u.port.domain) {
            fmt_sp(st);
            fmt_str(st, "@");
            fmt_sp(st);
            fmt_expr(st, s->u.port.domain);
        }
        for (i = 0; i < s->attrs.n; i++) {
            if (s->attrs.items[i].span.off > s->u.port.name.span.off) {
                fmt_sp(st);
                fmt_attr(st, &s->attrs.items[i]);
            }
        }
        break;
    }
    case STMT_DOMAIN:
        fmt_str(st, "domain");
        fmt_sp(st);
        fmt_name(st, s->u.domain.name);
        fmt_sp(st);
        fmt_str(st, "=");
        fmt_sp(st);
        fmt_open(st, "(");
        fmt_expr(st, s->u.domain.clock);
        if (s->u.domain.reset) {
            fmt_str(st, ",");
            fmt_sp(st);
            fmt_expr(st, s->u.domain.reset);
        }
        fmt_str(st, ")");
        break;
    case STMT_LET:
        fmt_str(st, "let");
        fmt_sp(st);
        fmt_pat(st, s->u.let.pat);
        if (s->u.let.type) {
            fmt_str(st, ":");
            fmt_sp(st);
            fmt_type(st, s->u.let.type);
        }
        fmt_assign_rhs(st, s->u.let.init);
        fmt_where_constraint(st, s->u.let.constraint);
        break;
    case STMT_VAR:
        fmt_str(st, "var");
        fmt_sp(st);
        fmt_name(st, s->u.var.name);
        if (s->u.var.type) {
            fmt_str(st, ":");
            fmt_sp(st);
            fmt_type(st, s->u.var.type);
        }
        if (s->u.var.init)
            fmt_assign_rhs(st, s->u.var.init);
        fmt_where_constraint(st, s->u.var.constraint);
        break;
    case STMT_REG:
        fmt_str(st, "reg");
        fmt_sp(st);
        fmt_name(st, s->u.reg.name);
        fmt_str(st, ":");
        fmt_sp(st);
        fmt_type(st, s->u.reg.type);
        if (s->u.reg.init)
            fmt_assign_rhs(st, s->u.reg.init);
        if (s->u.reg.domain) {
            fmt_sp(st);
            fmt_str(st, "@");
            fmt_sp(st);
            fmt_expr(st, s->u.reg.domain);
        }
        break;
    case STMT_MEM:
        fmt_str(st, "mem");
        fmt_sp(st);
        fmt_name(st, s->u.mem.name);
        fmt_str(st, ":");
        fmt_sp(st);
        fmt_type(st, s->u.mem.type);
        if (s->u.mem.init)
            fmt_assign_rhs(st, s->u.mem.init);
        break;
    case STMT_COMB:
        fmt_str(st, "comb");
        fmt_sp(st);
        fmt_block(st, s->u.comb);
        break;
    case STMT_ON:
        fmt_str(st, "on");
        fmt_sp(st);
        fmt_expr(st, s->u.on.clock);
        fmt_sp(st);
        fmt_block(st, s->u.on.body);
        break;
    case STMT_ASSIGN:
        fmt_expr(st, s->u.assign.lhs);
        fmt_assign_rhs(st, s->u.assign.rhs);
        fmt_where_constraint(st, s->u.assign.constraint);
        break;
    case STMT_EXPR:
        fmt_expr(st, s->u.expr);
        break;
    case STMT_FOR:
        fmt_str(st, "for");
        fmt_sp(st);
        fmt_pat(st, s->u.for_stmt.pat);
        fmt_sp(st);
        fmt_str(st, "in");
        fmt_sp(st);
        fmt_expr(st, s->u.for_stmt.iter);
        fmt_sp(st);
        fmt_block(st, s->u.for_stmt.body);
        break;
    case STMT_WHILE:
        fmt_str(st, "while");
        fmt_sp(st);
        fmt_expr(st, s->u.while_stmt.cond);
        fmt_sp(st);
        fmt_block(st, s->u.while_stmt.body);
        break;
    case STMT_ASSERT:
        fmt_str(st, s->u.assert_stmt.kind == ASSERT_EXPECT ? "expect" : "assert");
        fmt_sp(st);
        fmt_expr(st, s->u.assert_stmt.cond);
        if (s->u.assert_stmt.msg) {
            fmt_str(st, ",");
            fmt_break_or_space(st, fmt_end(s->u.assert_stmt.cond->span), s->u.assert_stmt.msg->span.off);
            fmt_expr(st, s->u.assert_stmt.msg);
        }
        break;
    case STMT_STATIC_ASSERT:
        fmt_static_assert(st, kw, s->u.static_assert.cond, s->u.static_assert.msg);
        break;
    case STMT_RETURN:
        fmt_str(st, "return");
        if (s->u.ret) {
            fmt_sp(st);
            fmt_expr(st, s->u.ret);
        }
        break;
    }
}

/* ------------------------------------------------------------------ items */

static void fmt_param_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const Param *p = &((const Param *)ctx)[i];
    fmt_name(st, p->name);
    fmt_str(st, ":");
    fmt_sp(st);
    fmt_type(st, p->type);
}

static uint32_t fmt_param_off(const void *ctx, uint32_t i)
{
    return ((const Param *)ctx)[i].span.off;
}

static void fmt_variant_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const Variant *v = ((Variant *const *)ctx)[i];
    fmt_lead_attrs(st, v->attrs, fmt_kw_off(st, v->span.off, v->attrs), 0);
    fmt_comments_before(st, v->name.span.off);
    fmt_name(st, v->name);
    if (v->has_fields) {
        uint32_t open = fmt_find(st, fmt_end(v->name.span), TK_LBRACE);
        fmt_sp(st);
        fmt_fields(st, open, fmt_end(v->span) - 1, v->fields, v->nfields);
    }
}

static uint32_t fmt_variant_off(const void *ctx, uint32_t i)
{
    return ((Variant *const *)ctx)[i]->span.off;
}

/* Bundle bodies interleave fields and fns in source order. */
typedef struct FmtBundleEnt {
    const Field *field;
    const Item *fn;
    uint32_t off;
} FmtBundleEnt;

static void fmt_bundle_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const FmtBundleEnt *b = &((const FmtBundleEnt *)ctx)[i];
    if (b->field)
        fmt_field(st, b->field);
    else
        fmt_item(st, b->fn);
}

static uint32_t fmt_bundle_off(const void *ctx, uint32_t i)
{
    return ((const FmtBundleEnt *)ctx)[i].off;
}

static int fmt_bundle_no_comma(const void *ctx, uint32_t i)
{
    return ((const FmtBundleEnt *)ctx)[i].fn != NULL;
}

static void fmt_regfield_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const RegField *f = ((RegField *const *)ctx)[i];
    fmt_lead_attrs(st, f->attrs, fmt_kw_off(st, f->span.off, f->attrs), 0);
    fmt_comments_before(st, f->name.span.off);
    fmt_name(st, f->name);
    fmt_str(st, ":");
    fmt_sp(st);
    fmt_type(st, f->type);
    fmt_sp(st);
    fmt_str(st, "@");
    if (f->bit_lo) {
        fmt_open(st, "[");
        fmt_expr(st, f->bit_hi);
        fmt_str(st, ":");
        fmt_expr(st, f->bit_lo);
        fmt_str(st, "]");
    } else {
        fmt_expr(st, f->bit_hi);
    }
}

static uint32_t fmt_regfield_off(const void *ctx, uint32_t i)
{
    return ((RegField *const *)ctx)[i]->span.off;
}

static void fmt_regentry_elem(FmtState *st, const void *ctx, uint32_t i)
{
    const RegEntry *r = ((RegEntry *const *)ctx)[i];
    fmt_lead_attrs(st, r->attrs, fmt_kw_off(st, r->span.off, r->attrs), 0);
    fmt_expr(st, r->offset);
    fmt_sp(st);
    fmt_name(st, r->name);
    fmt_str(st, ":");
    fmt_sp(st);
    fmt_name(st, r->access);
    fmt_sp(st);
    if (r->kind == REG_FIELDS) {
        FmtList L;
        uint32_t open = fmt_find(st, fmt_end(r->access.span), TK_LBRACE);
        fmt_list_init(&L, "{", "}", open, fmt_match_close(st, open), r->nfields, fmt_regfield_elem,
                      fmt_regfield_off, r->fields);
        L.spaces = 1;
        fmt_list(st, &L);
    } else {
        fmt_type(st, r->type);
        if (r->kind == REG_STREAM) {
            fmt_sp(st);
            fmt_str(st, "->");
            fmt_sp(st);
            fmt_type(st, r->stream_type);
        }
    }
    if (r->reset)
        fmt_assign_rhs(st, r->reset);
}

static uint32_t fmt_regentry_off(const void *ctx, uint32_t i)
{
    return ((RegEntry *const *)ctx)[i]->span.off;
}

/* Body of a property/sequence: `{ expr }` or broken over lines. */
static void fmt_temporal_body(FmtState *st, uint32_t name_end, uint32_t close, const Expr *body)
{
    uint32_t open = fmt_find(st, name_end, TK_LBRACE);
    int forced = fmt_src_has_nl(st, open, close) || fmt_has_comment(st, open + 1, close);
    int saved_indent = st->indent, saved_cont = st->cont, base;
    FmtMark m;
    fmt_comments_before(st, open);
    if (st->flat && forced) {
        st->fail = 1;
        return;
    }
    st->temporal++;
    if (!forced) {
        int was_flat = st->flat;
        if (!was_flat)
            fmt_flat_begin(st, &m);
        fmt_open(st, "{");
        fmt_sp(st);
        fmt_expr(st, body);
        fmt_sp(st);
        fmt_str(st, "}");
        if (was_flat || fmt_flat_end(st, &m)) {
            st->temporal--;
            return;
        }
    }
    fmt_open(st, "{");
    base = st->line_ind;
    st->indent = base + FMT_IND;
    fmt_comments_before(st, body->span.off);
    fmt_nl(st);
    st->cont = st->indent + FMT_IND;
    fmt_expr(st, body);
    fmt_comments_before(st, close);
    st->indent = base;
    fmt_nl(st);
    fmt_hint(st, close);
    fmt_str(st, "}");
    st->indent = saved_indent;
    st->cont = saved_cont;
    st->temporal--;
}

/* Generic parameters, `where` and the opening of a body. */
static void fmt_head_tail(FmtState *st, const WhereList *where)
{
    if (where && where->n) {
        fmt_where(st, where);
        fmt_nl(st);
    } else {
        fmt_sp(st);
    }
}

static void fmt_kw_name(FmtState *st, const char *kw, Ident name)
{
    fmt_str(st, kw);
    fmt_sp(st);
    fmt_name(st, name);
}

static void fmt_item(FmtState *st, const Item *it)
{
    uint32_t kw, close = fmt_end(it->span) - 1, i;
    fmt_comments_before(st, it->span.off);
    fmt_hint(st, it->span.off);
    kw = fmt_kw_off(st, it->span.off, it->attrs);
    fmt_lead_attrs(st, it->attrs, kw, 1);
    fmt_comments_before(st, kw);
    if (it->is_pub) {
        fmt_str(st, "pub");
        fmt_sp(st);
    }
    switch (it->kind) {
    case ITEM_ERROR:
        break;
    case ITEM_MODULE:
        fmt_kw_name(st, "module", it->name);
        fmt_gparams(st, &it->u.module.generics, it->u.module.where.n ? 0 : 2);
        fmt_head_tail(st, &it->u.module.where);
        fmt_body(st, it->u.module.body, it->u.module.nbody, fmt_find(st, fmt_end(it->name.span), TK_LBRACE), close,
                 NULL, 0);
        break;
    case ITEM_EXTERN:
        fmt_str(st, "extern");
        fmt_sp(st);
        fmt_kw_name(st, "module", it->name);
        fmt_open(st, "(");
        fmt_src_text(st, it->u.extern_mod.file_span.off, it->u.extern_mod.file_span.len);
        fmt_str(st, ")");
        if (it->u.extern_mod.generics.present)
            fmt_sp(st);
        fmt_gparams(st, &it->u.extern_mod.generics, it->u.extern_mod.where.n ? 0 : 2);
        fmt_head_tail(st, &it->u.extern_mod.where);
        fmt_body(st, it->u.extern_mod.body, it->u.extern_mod.nbody,
                 fmt_find(st, fmt_end(it->u.extern_mod.file_span), TK_LBRACE), close, NULL, 0);
        break;
    case ITEM_BUNDLE: {
        FmtList L;
        uint32_t nf = it->u.bundle.nfields, nfn = it->u.bundle.nfns, a = 0, b = 0, k = 0;
        FmtBundleEnt *ents = (FmtBundleEnt *)xcalloc(nf + nfn + 1, sizeof *ents);
        while (a < nf || b < nfn) {
            if (b >= nfn || (a < nf && it->u.bundle.fields[a]->span.off < it->u.bundle.fns[b]->span.off)) {
                ents[k].field = it->u.bundle.fields[a];
                ents[k].off = it->u.bundle.fields[a++]->span.off;
            } else {
                ents[k].fn = it->u.bundle.fns[b];
                ents[k].off = it->u.bundle.fns[b++]->span.off;
            }
            k++;
        }
        fmt_kw_name(st, "bundle", it->name);
        fmt_gparams(st, &it->u.bundle.generics, 2);
        fmt_sp(st);
        fmt_list_init(&L, "{", "}", fmt_find(st, it->u.bundle.generics.present ? fmt_end(it->u.bundle.generics.span)
                                                                                 : fmt_end(it->name.span),
                                             TK_LBRACE),
                      close, k, fmt_bundle_elem, fmt_bundle_off, ents);
        L.spaces = 1;
        L.force = nfn > 0;
        L.no_comma = fmt_bundle_no_comma;
        fmt_list(st, &L);
        free(ents);
        break;
    }
    case ITEM_STRUCT:
        fmt_kw_name(st, "struct", it->name);
        fmt_gparams(st, &it->u.struct_def.generics, 2);
        fmt_sp(st);
        fmt_fields(st,
                   fmt_find(st, it->u.struct_def.generics.present ? fmt_end(it->u.struct_def.generics.span)
                                                                  : fmt_end(it->name.span),
                            TK_LBRACE),
                   close, it->u.struct_def.fields, it->u.struct_def.nfields);
        break;
    case ITEM_ENUM: {
        FmtList L;
        fmt_kw_name(st, "enum", it->name);
        fmt_gparams(st, &it->u.enum_def.generics, 2);
        fmt_sp(st);
        fmt_list_init(&L, "{", "}",
                      fmt_find(st, it->u.enum_def.generics.present ? fmt_end(it->u.enum_def.generics.span)
                                                                   : fmt_end(it->name.span),
                               TK_LBRACE),
                      close, it->u.enum_def.nvariants, fmt_variant_elem, fmt_variant_off, it->u.enum_def.variants);
        L.spaces = 1;
        fmt_list(st, &L);
        break;
    }
    case ITEM_CONST:
        fmt_kw_name(st, "const", it->name);
        if (it->u.const_def.type) {
            fmt_str(st, ":");
            fmt_sp(st);
            fmt_type(st, it->u.const_def.type);
        }
        fmt_assign_rhs(st, it->u.const_def.value);
        break;
    case ITEM_FN: {
        FmtList L;
        uint32_t open;
        if (it->u.fn.is_const) {
            fmt_str(st, "const");
            fmt_sp(st);
        }
        fmt_kw_name(st, "fn", it->name);
        fmt_gparams(st, &it->u.fn.generics, 0);
        open = fmt_find(st, it->u.fn.generics.present ? fmt_end(it->u.fn.generics.span) : fmt_end(it->name.span),
                        TK_LPAREN);
        fmt_list_init(&L, "(", ")", open, fmt_match_close(st, open), it->u.fn.nparams, fmt_param_elem,
                      fmt_param_off, it->u.fn.params);
        /* the return type and ` {` must fit on the line of a flat list */
        L.reserve = it->u.fn.where.n ? 0 : 2;
        if (it->u.fn.ret && !st->flat) {
            FmtMark m;
            int c0 = st->col;
            fmt_flat_begin(st, &m);
            fmt_str(st, " -> ");
            fmt_type(st, it->u.fn.ret);
            if (!st->fail)
                L.reserve += st->col - c0;
            st->flat--;
            fmt_rewind(st, &m);
        }
        fmt_list(st, &L);
        if (it->u.fn.ret) {
            fmt_sp(st);
            fmt_str(st, "->");
            fmt_sp(st);
            fmt_type(st, it->u.fn.ret);
        }
        fmt_head_tail(st, &it->u.fn.where);
        fmt_block(st, it->u.fn.body);
        break;
    }
    case ITEM_TYPE:
        fmt_kw_name(st, "type", it->name);
        fmt_gparams(st, &it->u.type_alias.generics, 0);
        fmt_sp(st);
        fmt_str(st, "=");
        fmt_sp(st);
        fmt_type(st, it->u.type_alias.type);
        break;
    case ITEM_REGMAP: {
        FmtList L;
        fmt_kw_name(st, "regmap", it->name);
        fmt_sp(st);
        fmt_list_init(&L, "{", "}", fmt_find(st, fmt_end(it->name.span), TK_LBRACE), close, it->u.regmap.nentries,
                      fmt_regentry_elem, fmt_regentry_off, it->u.regmap.entries);
        L.spaces = 1;
        fmt_list(st, &L);
        break;
    }
    case ITEM_TEST:
        fmt_str(st, "test");
        fmt_sp(st);
        fmt_src_text(st, it->u.test.name_span.off, it->u.test.name_span.len);
        fmt_sp(st);
        fmt_block(st, it->u.test.body);
        break;
    case ITEM_PROPERTY: {
        static const char *const kinds[] = {"", "assert", "assume", "cover"};
        if (it->u.property.kind != PROP_PLAIN) {
            fmt_str(st, kinds[it->u.property.kind]);
            fmt_sp(st);
        }
        fmt_kw_name(st, "property", it->name);
        fmt_sp(st);
        fmt_temporal_body(st, fmt_end(it->name.span), close, it->u.property.body);
        break;
    }
    case ITEM_SEQUENCE:
        fmt_kw_name(st, "sequence", it->name);
        fmt_sp(st);
        fmt_temporal_body(st, fmt_end(it->name.span), close, it->u.sequence.body);
        break;
    case ITEM_STATIC_ASSERT:
        fmt_static_assert(st, kw, it->u.static_assert.cond, it->u.static_assert.msg);
        break;
    }
    TERN_UNUSED(i);
}

/* ------------------------------------------------------------------- file */

static void fmt_use_tree(FmtState *st, const UseTree *u);

static void fmt_use_elem(FmtState *st, const void *ctx, uint32_t i)
{
    fmt_use_tree(st, ((UseTree *const *)ctx)[i]);
}

static uint32_t fmt_use_off(const void *ctx, uint32_t i)
{
    return ((UseTree *const *)ctx)[i]->span.off;
}

static void fmt_use_tree(FmtState *st, const UseTree *u)
{
    uint32_t i;
    for (i = 0; i < u->npath; i++) {
        if (i)
            fmt_str(st, "::");
        fmt_name(st, u->path[i]);
    }
    if (u->glob)
        fmt_str(st, "::*");
    if (u->is_group) {
        FmtList L;
        uint32_t open = fmt_find(st, u->npath ? fmt_end(u->path[u->npath - 1].span) : u->span.off, TK_LBRACE);
        fmt_str(st, "::");
        fmt_list_init(&L, "{", "}", open, fmt_match_close(st, open), u->ngroup, fmt_use_elem, fmt_use_off, u->group);
        fmt_list(st, &L);
    }
    if (u->alias.name) {
        fmt_sp(st);
        fmt_str(st, "as");
        fmt_sp(st);
        fmt_name(st, u->alias);
    }
}

typedef struct FmtTop {
    const UseDecl *use;
    const Item *item;
    uint32_t off;
} FmtTop;

static int fmt_top_cmp(const void *a, const void *b)
{
    uint32_t x = ((const FmtTop *)a)->off, y = ((const FmtTop *)b)->off;
    return x < y ? -1 : x > y;
}

/* Comments before a top-level entry; with `force_blank` the first own-line
 * thing after the previous entry is preceded by an empty line. Returns 1 if
 * that blank line has been written. */
static int fmt_top_comments(FmtState *st, uint32_t off, int force_blank)
{
    while (st->ci < st->f->ncomments && st->f->comments[st->ci].span.off < off) {
        const Comment *c = &st->f->comments[st->ci++];
        int own = st->bol || fmt_src_line_start(st, c->span.off) || memchr(st->src + c->span.off, '\n', c->span.len);
        fmt_emit_comment(st, c, own && force_blank);
        if (own)
            force_blank = 0;
    }
    return !force_blank;
}

void fmt_format(const AstFile *f, StrBuf *out)
{
    FmtState st;
    FmtTop *ents;
    uint32_t n = 0, i;
    int prev = 0; /* 0 nothing, 1 package, 2 use, 3 item */
    const SourceFile *sf = src_get(f->src);
    memset(&st, 0, sizeof st);
    st.f = f;
    st.src = sf ? sf->text : "";
    st.srclen = sf ? (uint32_t)sf->len : 0;
    st.out = out;
    st.next_ind = -1;
    st.bol = 1;
    ents = (FmtTop *)xcalloc((size_t)f->nuses + f->nitems + 1, sizeof *ents);
    for (i = 0; i < f->nuses; i++) {
        ents[n].use = f->uses[i];
        ents[n++].off = f->uses[i]->span.off;
    }
    for (i = 0; i < f->nitems; i++) {
        ents[n].item = f->items[i];
        ents[n++].off = f->items[i]->span.off;
    }
    qsort(ents, n, sizeof *ents, fmt_top_cmp);
    if (f->has_package) {
        fmt_top_comments(&st, f->package_span.off, 0);
        fmt_nl(&st);
        if (fmt_src_blank_before(&st, f->package_span.off))
            fmt_blank(&st);
        fmt_str(&st, "package");
        fmt_sp(&st);
        for (i = 0; i < f->npackage; i++) {
            if (i)
                fmt_str(&st, "::");
            fmt_name(&st, f->package[i]);
        }
        prev = 1;
    }
    for (i = 0; i < n; i++) {
        int kind = ents[i].use ? 2 : 3;
        int force = prev != 0 && !(prev == 2 && kind == 2);
        int done = fmt_top_comments(&st, ents[i].off, force);
        fmt_nl(&st);
        if (!done || (prev != 0 && fmt_src_blank_before(&st, ents[i].off)))
            fmt_blank(&st);
        st.indent = 0;
        st.cont = FMT_IND;
        if (ents[i].use) {
            const UseDecl *u = ents[i].use;
            if (u->is_pub) {
                fmt_str(&st, "pub");
                fmt_sp(&st);
            }
            fmt_str(&st, "use");
            fmt_sp(&st);
            fmt_use_tree(&st, u->tree);
        } else {
            fmt_item(&st, ents[i].item);
        }
        prev = kind;
    }
    fmt_comments_before(&st, UINT32_MAX);
    fmt_nl(&st);
    while (out->len >= 2 && out->data[out->len - 1] == '\n' && out->data[out->len - 2] == '\n')
        out->len--;
    if (out->len == 1 && out->data[0] == '\n')
        out->len = 0;
    sb_cstr(out);
    free(ents);
}

/* -------------------------------------------------------------- verifying */

void fmt_dump_no_pos(const AstFile *f, StrBuf *out)
{
    StrBuf raw = {0};
    size_t i = 0;
    ast_dump(f, &raw);
    while (i < raw.len) {
        char c = raw.data[i];
        if (c == '@' && i + 1 < raw.len && tern_is_digit((unsigned char)raw.data[i + 1])) {
            size_t j = i + 1;
            while (j < raw.len && tern_is_digit((unsigned char)raw.data[j]))
                j++;
            if (j < raw.len && raw.data[j] == ':') {
                j++;
                while (j < raw.len && tern_is_digit((unsigned char)raw.data[j]))
                    j++;
                i = j;
                continue;
            }
        }
        sb_putc(out, c);
        i++;
    }
    sb_cstr(out);
    sb_free(&raw);
}

static void fmt_null_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(d);
    TERN_UNUSED(user);
}

int fmt_verify(const AstFile *f, const char *text, size_t len, StrBuf *why)
{
    Arena a = {0};
    AstFile *g;
    StrBuf d1 = {0}, d2 = {0};
    StrBuf name = {0};
    uint32_t id, i;
    int ok = 1;
    const SourceFile *sf = src_get(f->src);
    sb_printf(&name, "%s (formatted)", f->path ? f->path : "");
    diag_flush();
    diag_set_sink(fmt_null_sink, NULL);
    id = src_add(name.data, text, len);
    g = parse_file(&a, id);
    diag_flush();
    diag_set_sink(NULL, NULL);
    sb_free(&name);
    if (g->nerrors) {
        if (why)
            sb_puts(why, "the formatted text has syntax errors");
        ok = 0;
    }
    if (ok) {
        fmt_dump_no_pos(f, &d1);
        fmt_dump_no_pos(g, &d2);
        if (d1.len != d2.len || memcmp(d1.data, d2.data, d1.len) != 0) {
            if (why) {
                size_t k = 0, line = 1;
                while (k < d1.len && k < d2.len && d1.data[k] == d2.data[k]) {
                    if (d1.data[k] == '\n')
                        line++;
                    k++;
                }
                sb_printf(why, "the syntax tree changed (dump line %u)", (unsigned)line);
            }
            ok = 0;
        }
    }
    if (ok && f->ncomments != g->ncomments) {
        if (why)
            sb_printf(why, "comment count changed (%u -> %u)", (unsigned)f->ncomments, (unsigned)g->ncomments);
        ok = 0;
    }
    for (i = 0; ok && i < f->ncomments; i++) {
        const Comment *x = &f->comments[i], *y = &g->comments[i];
        if (x->span.len != y->span.len || !sf ||
            memcmp(sf->text + x->span.off, text + y->span.off, x->span.len) != 0) {
            if (why)
                sb_printf(why, "comment at line %u changed", (unsigned)x->span.line);
            ok = 0;
        }
    }
    sb_free(&d1);
    sb_free(&d2);
    arena_release(&a);
    return ok;
}

/* ----------------------------------------------------------------- naming */

static int fmt_is_upper_camel(const char *s)
{
    if (!tern_is_upper((unsigned char)s[0]))
        return 0;
    for (; *s; s++) {
        if (!tern_is_alnum((unsigned char)*s))
            return 0;
    }
    return 1;
}

static int fmt_is_snake(const char *s)
{
    for (; *s; s++) {
        if (!(tern_is_lower((unsigned char)*s) || tern_is_digit((unsigned char)*s) || *s == '_'))
            return 0;
    }
    return 1;
}

static int fmt_is_screaming(const char *s)
{
    if (!tern_is_upper((unsigned char)s[0]))
        return 0;
    for (; *s; s++) {
        if (!(tern_is_upper((unsigned char)*s) || tern_is_digit((unsigned char)*s) || *s == '_'))
            return 0;
    }
    return 1;
}

static void fmt_to_snake(const char *s, StrBuf *out)
{
    size_t i, n = strlen(s);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (tern_is_upper(c)) {
            int prev_lower = i > 0 && (tern_is_lower((unsigned char)s[i - 1]) || tern_is_digit((unsigned char)s[i - 1]));
            int acronym_end = i > 0 && tern_is_upper((unsigned char)s[i - 1]) && i + 1 < n &&
                              tern_is_lower((unsigned char)s[i + 1]);
            if ((prev_lower || acronym_end) && out->len > 0 && out->data[out->len - 1] != '_')
                sb_putc(out, '_');
            sb_putc(out, tern_to_lower(c));
        } else {
            sb_putc(out, c);
        }
    }
    sb_cstr(out);
}

static void fmt_to_camel(const char *s, StrBuf *out)
{
    int up = 1;
    int all_upper = fmt_is_screaming(s);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '_') {
            up = 1;
            continue;
        }
        if (up)
            sb_putc(out, tern_to_upper(c));
        else
            sb_putc(out, all_upper ? tern_to_lower(c) : c);
        up = 0;
    }
    if (out->len == 0)
        sb_putc(out, 'X');
    sb_cstr(out);
}

typedef enum FmtCase { FMT_CAMEL, FMT_SNAKE, FMT_CONST } FmtCase;

static int fmt_name_check(Ident id, FmtCase want, const char *what)
{
    StrBuf sugg = {0};
    const char *n = id.name;
    int ok;
    if (!n || !n[0] || strcmp(n, "_") == 0)
        return 0;
    if (want == FMT_CAMEL)
        ok = fmt_is_upper_camel(n);
    else if (want == FMT_SNAKE)
        ok = fmt_is_snake(n);
    else
        ok = fmt_is_upper_camel(n) || fmt_is_screaming(n);
    if (ok)
        return 0;
    if (want == FMT_SNAKE)
        fmt_to_snake(n, &sugg);
    else
        fmt_to_camel(n, &sugg);
    diag_warning(id.span, "%s `%s` should be %s", what, n,
                 want == FMT_CAMEL ? "UpperCamelCase" : want == FMT_SNAKE ? "snake_case"
                                                                          : "UpperCamelCase or SCREAMING_CASE");
    diag_help("rename it to `%s`", sugg.data);
    sb_free(&sugg);
    return 1;
}

static int fmt_naming_item(const Item *it);

/* `u8`, `s32`: the prelude's primitive aliases are spelled like built-ins. */
static int fmt_is_primitive_alias(const char *s)
{
    if (!s || (s[0] != 'u' && s[0] != 's') || !s[1])
        return 0;
    for (s++; *s; s++) {
        if (!tern_is_digit((unsigned char)*s))
            return 0;
    }
    return 1;
}

static int fmt_naming_fields(Field *const *fields, uint32_t n)
{
    uint32_t i;
    int w = 0;
    for (i = 0; i < n; i++)
        w += fmt_name_check(fields[i]->name, FMT_SNAKE, "field");
    return w;
}

static int fmt_naming_pat(const Pattern *p)
{
    uint32_t i;
    int w = 0;
    if (!p)
        return 0;
    switch (p->kind) {
    case PAT_BIND:
        return fmt_name_check(p->u.bind, FMT_SNAKE, "signal");
    case PAT_TUPLE:
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            w += fmt_naming_pat(p->u.list.items[i]);
        return w;
    default:
        return 0;
    }
}

static int fmt_naming_block(const Block *b);

static int fmt_naming_expr(const Expr *e)
{
    uint32_t i;
    int w = 0;
    if (!e)
        return 0;
    switch (e->kind) {
    case EXPR_BLOCK:
        return fmt_naming_block(e->u.block);
    case EXPR_IF:
        return fmt_naming_block(e->u.if_expr.then_block) + fmt_naming_expr(e->u.if_expr.else_expr);
    case EXPR_MATCH:
        for (i = 0; i < e->u.match.narms; i++) {
            const Stmt *s = e->u.match.arms[i].body;
            if (s && s->kind == STMT_EXPR)
                w += fmt_naming_expr(s->u.expr);
        }
        return w;
    default:
        return 0;
    }
}

static int fmt_naming_stmts(Stmt *const *stmts, uint32_t n)
{
    uint32_t i;
    int w = 0;
    for (i = 0; i < n; i++) {
        const Stmt *s = stmts[i];
        switch (s->kind) {
        case STMT_PORT:
            w += fmt_name_check(s->u.port.name, FMT_SNAKE, "port");
            break;
        case STMT_DOMAIN:
            w += fmt_name_check(s->u.domain.name, FMT_SNAKE, "domain");
            break;
        case STMT_LET:
            w += fmt_naming_pat(s->u.let.pat);
            w += fmt_naming_expr(s->u.let.init);
            break;
        case STMT_VAR:
            w += fmt_name_check(s->u.var.name, FMT_SNAKE, "variable");
            break;
        case STMT_REG:
            w += fmt_name_check(s->u.reg.name, FMT_SNAKE, "register");
            break;
        case STMT_MEM:
            w += fmt_name_check(s->u.mem.name, FMT_SNAKE, "memory");
            break;
        case STMT_ITEM:
            w += fmt_naming_item(s->u.item);
            break;
        case STMT_COMB:
            w += fmt_naming_block(s->u.comb);
            break;
        case STMT_ON:
            w += fmt_naming_block(s->u.on.body);
            break;
        case STMT_FOR:
            w += fmt_naming_pat(s->u.for_stmt.pat);
            w += fmt_naming_block(s->u.for_stmt.body);
            break;
        case STMT_WHILE:
            w += fmt_naming_block(s->u.while_stmt.body);
            break;
        case STMT_EXPR:
            w += fmt_naming_expr(s->u.expr);
            break;
        default:
            break;
        }
    }
    return w;
}

static int fmt_naming_block(const Block *b)
{
    return b ? fmt_naming_stmts(b->stmts, b->nstmts) : 0;
}

static int fmt_naming_item(const Item *it)
{
    uint32_t i, j;
    int w = 0;
    switch (it->kind) {
    case ITEM_MODULE:
        w += fmt_name_check(it->name, FMT_CAMEL, "module");
        w += fmt_naming_stmts(it->u.module.body, it->u.module.nbody);
        break;
    case ITEM_EXTERN:
        break; /* SystemVerilog names */
    case ITEM_BUNDLE:
        w += fmt_name_check(it->name, FMT_CAMEL, "bundle");
        w += fmt_naming_fields(it->u.bundle.fields, it->u.bundle.nfields);
        for (i = 0; i < it->u.bundle.nfns; i++)
            w += fmt_naming_item(it->u.bundle.fns[i]);
        break;
    case ITEM_STRUCT:
        w += fmt_name_check(it->name, FMT_CAMEL, "struct");
        w += fmt_naming_fields(it->u.struct_def.fields, it->u.struct_def.nfields);
        break;
    case ITEM_ENUM:
        w += fmt_name_check(it->name, FMT_CAMEL, "enum");
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            const Variant *v = it->u.enum_def.variants[i];
            w += fmt_name_check(v->name, FMT_CAMEL, "enum variant");
            w += fmt_naming_fields(v->fields, v->nfields);
        }
        break;
    case ITEM_CONST:
        w += fmt_name_check(it->name, FMT_CONST, "constant");
        break;
    case ITEM_FN:
        w += fmt_name_check(it->name, FMT_SNAKE, "function");
        for (i = 0; i < it->u.fn.nparams; i++)
            w += fmt_name_check(it->u.fn.params[i].name, FMT_SNAKE, "parameter");
        w += fmt_naming_block(it->u.fn.body);
        break;
    case ITEM_TYPE:
        if (!fmt_is_primitive_alias(it->name.name))
            w += fmt_name_check(it->name, FMT_CAMEL, "type");
        break;
    case ITEM_REGMAP:
        w += fmt_name_check(it->name, FMT_CAMEL, "regmap");
        for (i = 0; i < it->u.regmap.nentries; i++) {
            const RegEntry *r = it->u.regmap.entries[i];
            w += fmt_name_check(r->name, FMT_SNAKE, "register");
            for (j = 0; j < r->nfields; j++)
                w += fmt_name_check(r->fields[j]->name, FMT_SNAKE, "register field");
        }
        break;
    case ITEM_TEST:
        w += fmt_naming_block(it->u.test.body);
        break;
    case ITEM_PROPERTY:
        w += fmt_name_check(it->name, FMT_SNAKE, "property");
        break;
    case ITEM_SEQUENCE:
        w += fmt_name_check(it->name, FMT_SNAKE, "sequence");
        break;
    case ITEM_ERROR:
    case ITEM_STATIC_ASSERT:
        break;
    }
    return w;
}

int fmt_check_naming(const AstFile *f)
{
    uint32_t i;
    int w = 0;
    for (i = 0; i < f->nitems; i++)
        w += fmt_naming_item(f->items[i]);
    return w;
}

/* ---------------------------------------------------------------- command */

typedef VEC(char *) FmtPaths;

static void fmt_collect(const char *path, FmtPaths *out, int top)
{
    OsDirList list;
    size_t i;
    if (!os_is_dir(path)) {
        if (top || strcmp(os_path_ext(path), ".tern") == 0)
            vec_push(*out, xstrdup(path));
        return;
    }
    if (os_list_dir(path, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        char *child;
        if (list.items[i].name[0] == '.')
            continue;
        child = os_path_join(path, list.items[i].name);
        if (list.items[i].is_dir || strcmp(os_path_ext(child), ".tern") == 0)
            fmt_collect(child, out, 0);
        free(child);
    }
    os_dir_list_free(&list);
}

/* Default inputs: the project's src/ and tests/ (project root = the nearest
 * directory with tern.toml), else the .tern files of the current directory. */
static void fmt_default_paths(FmtPaths *out)
{
    char *dir = os_getcwd();
    char *root = NULL;
    while (dir) {
        char *manifest = os_path_join(dir, "tern.toml");
        char *parent;
        int found = os_is_file(manifest);
        free(manifest);
        if (found) {
            root = dir;
            break;
        }
        parent = os_path_dirname(dir);
        if (!parent || strcmp(parent, dir) == 0) {
            free(parent);
            free(dir);
            dir = NULL;
            break;
        }
        free(dir);
        dir = parent;
    }
    if (root) {
        static const char *const subdirs[] = {"src", "tests"};
        char *cwd = os_getcwd();
        size_t i;
        for (i = 0; i < TERN_ARRAY_LEN(subdirs); i++) {
            char *p = os_path_join(root, subdirs[i]);
            char *rel = cwd ? os_path_relative(p, cwd) : xstrdup(p);
            if (os_is_dir(rel))
                fmt_collect(rel, out, 0);
            free(rel);
            free(p);
        }
        free(cwd);
        free(root);
    } else {
        OsDirList list;
        size_t i;
        if (os_list_dir(".", &list) == 0) {
            for (i = 0; i < list.len; i++) {
                if (!list.items[i].is_dir && list.items[i].name[0] != '.' &&
                    strcmp(os_path_ext(list.items[i].name), ".tern") == 0)
                    vec_push(*out, xstrdup(list.items[i].name));
            }
            os_dir_list_free(&list);
        }
    }
}

int cmd_fmt(int argc, char **argv)
{
    ArgParser ap;
    FmtPaths paths = {0};
    int check = 0, to_stdout = 0, failed = 0, changed = 0, any_path = 0;
    size_t i;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "check"))
            check = 1;
        else if (args_flag(&ap, "stdout"))
            to_stdout = 1;
        else if (args_flag(&ap, "help|h")) {
            cmd_print_usage("fmt");
            for (i = 0; i < paths.len; i++)
                free(paths.data[i]);
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_positional(&ap)) {
            any_path = 1;
            if (!os_exists(ap.arg)) {
                args_error(&ap, "no such file or directory: %s", ap.arg);
                continue;
            }
            fmt_collect(ap.arg, &paths, 1);
        } else {
            args_unknown(&ap);
        }
    }
    if (!ap.errors && !any_path)
        fmt_default_paths(&paths);
    if (!ap.errors && to_stdout && paths.len != 1)
        args_error(&ap, "--stdout takes exactly one file");
    if (!ap.errors && check && to_stdout)
        args_error(&ap, "--check and --stdout cannot be combined");
    if (ap.errors) {
        for (i = 0; i < paths.len; i++)
            free(paths.data[i]);
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    for (i = 0; i < paths.len; i++) {
        const char *path = paths.data[i];
        char *text = NULL;
        size_t len = 0;
        uint32_t id;
        Arena arena = {0};
        AstFile *f;
        StrBuf out = {0}, why = {0};
        if (os_read_file(path, &text, &len) != 0) {
            fprintf(stderr, "tern fmt: error: cannot read %s\n", path);
            failed = 1;
            continue;
        }
        id = src_add(path, text, len);
        f = parse_file(&arena, id);
        if (f->nerrors) {
            diag_flush();
            fprintf(stderr, "tern fmt: %s: not formatted (syntax errors)\n", path);
            failed = 1;
        } else {
            fmt_format(f, &out);
            if (!fmt_verify(f, out.data, out.len, &why)) {
                fprintf(stderr,
                        "tern fmt: internal error: formatting %s would change it (%s); the file is left "
                        "unchanged, please report this\n",
                        path, why.data);
                failed = 1;
            } else {
                int same = out.len == len && memcmp(out.data, text, len) == 0;
                fmt_check_naming(f);
                diag_flush();
                if (to_stdout) {
                    fwrite(out.data, 1, out.len, stdout);
                } else if (check) {
                    if (!same) {
                        printf("%s\n", path);
                        changed = 1;
                    }
                } else if (!same && os_write_file_if_changed(path, out.data, out.len) < 0) {
                    fprintf(stderr, "tern fmt: error: cannot write %s\n", path);
                    failed = 1;
                }
            }
        }
        sb_free(&out);
        sb_free(&why);
        free(text);
        arena_release(&arena);
    }
    for (i = 0; i < paths.len; i++)
        free(paths.data[i]);
    vec_free(paths);
    if (diag_error_count() > 0)
        failed = 1; /* --strict naming warnings */
    return failed || changed ? TERN_EXIT_FAIL : TERN_EXIT_OK;
}

#undef FMT_WIDTH
#undef FMT_IND
