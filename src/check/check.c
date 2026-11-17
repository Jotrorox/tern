/* check.c — design checks over the elaborated IR (see check.h):
 *
 *   drivers   every signal has exactly one driver block (§6.1): a var or an
 *             output driven by two comb blocks, a reg by two on blocks, or a
 *             signal by a block and a continuous assignment (or by two
 *             continuous assignments to overlapping bits) is a "multiple
 *             drivers" error; an output or var that nothing assigns is an
 *             error too;
 *   assign    full assignment in comb blocks (§6.2): a var or output that a
 *             comb block assigns must be assigned on every path ("`alu_out`
 *             may be unassigned when `op == Nop`") — there are no latches;
 *   cdc       clock-domain typing (§5.6, §8.2): logic of one domain may not
 *             read signals of another ("pix-domain signal `line` used in
 *             sys-domain logic; cross with std::cdc"); wires, vars and
 *             outputs take the domain of what they read. Modules marked
 *             #[cdc(..)] (the std::cdc primitives) and wires made by
 *             unsafe_cross are exempt.
 *
 * Extern modules are black boxes and are not checked. Errors in a generic
 * module are reported once per source location, not per instantiation. */
#include "check.h"
#include "../base/diag.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../base/vec.h"

#define CHK_NONE 0xFFFFFFFFu
#define CHK_MIXED 0xFFFFFFFEu

typedef struct ChkCtx {
    const IrModule *m;
    Map seen;                 /* reported (span, message) hashes */
    int errors;
} ChkCtx;

static int chk_once(ChkCtx *c, Span at, const char *msg)
{
    uint64_t h = hash_combine(hash_combine(hash_u64(at.file), hash_u64(at.off)), hash_bytes(msg, strlen(msg)));
    if (map_has(&c->seen, h))
        return 0;
    map_put(&c->seen, h, 1);
    return 1;
}

static void chk_error(ChkCtx *c, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
static void chk_error(ChkCtx *c, Span at, const char *fmt, ...)
{
    va_list ap;
    StrBuf sb = {0};
    va_start(ap, fmt);
    sb_vprintf(&sb, fmt, ap);
    va_end(ap);
    if (chk_once(c, at, sb_cstr(&sb))) {
        diag_error(at, "%s", sb_cstr(&sb));
        c->errors++;
    }
    sb_free(&sb);
}

static const char *chk_sig_name(const IrModule *m, uint32_t s)
{
    return m->sigs[s].path ? m->sigs[s].path : m->sigs[s].name;
}

/* ================================================================ drivers */

typedef struct ChkDrv {
    uint32_t unit;            /* 0: none; assign i: 1 + i; process p: 0x40000000 + p */
    uint32_t lo, hi;          /* bits [lo, hi) */
    Span loc;
} ChkDrv;

typedef VEC(ChkDrv) ChkDrvVec;

static void chk_lv_range(const IrModule *m, const IrLvalue *lv, uint32_t *lo, uint32_t *hi)
{
    uint32_t sw = m->sigs[lv->sig].type->width;
    if (lv->index) {
        *lo = lv->lo;
        *hi = lv->len ? lv->lo + lv->len * lv->width : sw;
    } else {
        *lo = lv->lo;
        *hi = lv->lo + lv->width;
    }
    if (*hi > sw)
        *hi = sw;
}

static void chk_drv_block(const IrModule *m, const IrBlock *b, uint32_t unit, ChkDrvVec *drv)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN:
            if (s->lhs.sig < m->nsigs) {
                ChkDrvVec *v = &drv[s->lhs.sig];
                ChkDrv d;
                chk_lv_range(m, &s->lhs, &d.lo, &d.hi);
                d.unit = unit;
                d.loc = s->loc;
                /* one entry per (signal, process): the first assignment */
                if (!v->len || vec_last(*v).unit != unit)
                    vec_push(*v, d);
            }
            break;
        case IRST_IF:
            chk_drv_block(m, &s->then_b, unit, drv);
            chk_drv_block(m, &s->else_b, unit, drv);
            break;
        case IRST_CASE:
            for (k = 0; k < s->narms; k++)
                chk_drv_block(m, &s->arms[k].body, unit, drv);
            break;
        default:
            break;
        }
    }
}

static int chk_unit_kind(const IrModule *m, uint32_t unit)
{
    if (unit >= 0x40000000u)
        return m->procs[unit - 0x40000000u].kind == IRP_COMB ? 1 : 2;
    return unit == 0x3FFFFFFFu ? 3 : 0;
}

/* "two `comb` blocks", "an `on` block and a continuous assignment" */
static void chk_units_text(const IrModule *m, uint32_t ua, uint32_t ub, StrBuf *out)
{
    static const char *const one[] = {"a continuous assignment", "a `comb` block", "an `on` block",
                                      "an instance output"};
    static const char *const two[] = {"two continuous assignments", "two `comb` blocks", "two `on` blocks",
                                      "two instance outputs"};
    int a = chk_unit_kind(m, ua), b = chk_unit_kind(m, ub);
    if (a == b)
        sb_puts(out, two[a]);
    else
        sb_printf(out, "%s and %s", one[a], one[b]);
}

static void chk_drivers(ChkCtx *c)
{
    const IrModule *m = c->m;
    ChkDrvVec *drv = (ChkDrvVec *)xcalloc(m->nsigs ? m->nsigs : 1, sizeof(ChkDrvVec));
    uint32_t i, j, k;
    for (i = 0; i < m->nassigns; i++) {
        const IrAssign *a = &m->assigns[i];
        ChkDrv d;
        if (a->lhs.sig >= m->nsigs)
            continue;
        chk_lv_range(m, &a->lhs, &d.lo, &d.hi);
        d.unit = 1 + i;
        d.loc = a->loc;
        vec_push(drv[a->lhs.sig], d);
    }
    for (i = 0; i < m->nprocs; i++)
        chk_drv_block(m, &m->procs[i].body, 0x40000000u + i, drv);
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        for (k = 0; k < in->nconns; k++) {
            const IrConn *cn = &in->conns[k];
            if (cn->port < in->module->nsigs && in->module->sigs[cn->port].kind == IRS_OUT && cn->expr &&
                cn->expr->kind == IRE_SIG && cn->expr->sig < m->nsigs) {
                ChkDrv d;
                d.unit = 0x3FFFFFFFu;
                d.lo = 0;
                d.hi = m->sigs[cn->expr->sig].type->width;
                d.loc = in->loc;
                vec_push(drv[cn->expr->sig], d);
            }
        }
    }
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        ChkDrvVec *v = &drv[i];
        int reported = 0;
        if (s->kind == IRS_MEM || s->kind == IRS_IN)
            continue;
        if (!v->len) {
            if (s->kind == IRS_OUT && s->type->kind != IRT_CLOCK && s->type->kind != IRT_RESET)
                chk_error(c, s->loc, "output `%s` is never assigned", chk_sig_name(m, i));
            else if (s->kind == IRS_VAR)
                chk_error(c, s->loc, "`var %s` is never assigned", chk_sig_name(m, i));
            continue;
        }
        for (j = 1; j < v->len && !reported; j++)
            for (k = 0; k < j && !reported; k++) {
                const ChkDrv *a = &v->data[k], *b = &v->data[j];
                int conflict;
                if (a->unit == b->unit)
                    continue;
                /* two blocks always conflict; continuous assignments only on shared bits */
                conflict = (a->unit >= 0x3FFFFFFFu || b->unit >= 0x3FFFFFFFu) || (a->lo < b->hi && b->lo < a->hi);
                if (!conflict)
                    continue;
                {
                    StrBuf u = {0};
                    chk_units_text(m, a->unit, b->unit, &u);
                    chk_error(c, b->loc, "multiple drivers: `%s` is driven by %s", chk_sig_name(m, i), sb_cstr(&u));
                    sb_free(&u);
                }
                diag_note(a->loc, "the other driver");
                diag_help("each signal has exactly one driver block; assign `%s` in one place", chk_sig_name(m, i));
                reported = 1;
            }
    }
    for (i = 0; i < m->nsigs; i++)
        vec_free(drv[i]);
    free(drv);
}

/* ========================================================= full assignment */

typedef struct ChkCov {       /* bits of the signal assigned so far */
    uint64_t *w;
    uint32_t nw;
} ChkCov;

typedef struct ChkFull {
    ChkCtx *c;
    uint32_t sig, width;
    StrBuf path;              /* conditions of the current path */
    StrBuf fail;              /* first incomplete path found */
    int have_fail;
} ChkFull;

static int chk_cov_full(const ChkCov *v, uint32_t width)
{
    uint32_t i;
    for (i = 0; i < width; i++)
        if (!(v->w[i / 64] >> (i % 64) & 1))
            return 0;
    return 1;
}

static void chk_cov_set(ChkCov *v, uint32_t lo, uint32_t hi)
{
    uint32_t i;
    for (i = lo; i < hi; i++)
        v->w[i / 64] |= (uint64_t)1 << (i % 64);
}

static ChkCov chk_cov_dup(const ChkCov *v)
{
    ChkCov r;
    r.nw = v->nw;
    r.w = (uint64_t *)xmalloc(v->nw * sizeof(uint64_t));
    memcpy(r.w, v->w, v->nw * sizeof(uint64_t));
    return r;
}

/* source text of an expression: a signal's path, `!x`, or its source span */
static void chk_expr_text(const IrModule *m, const IrExpr *e, StrBuf *out)
{
    const SourceFile *sf;
    if (e->kind == IRE_SIG && e->sig < m->nsigs) {
        sb_puts(out, chk_sig_name(m, e->sig));
        return;
    }
    if (e->kind == IRE_UNARY && e->op == IROP_LNOT && e->args[0]->kind == IRE_SIG) {
        sb_putc(out, '!');
        chk_expr_text(m, e->args[0], out);
        return;
    }
    sf = e->loc.file ? src_get(e->loc.file) : NULL;
    if (sf && e->loc.len && e->loc.len <= 60 && (size_t)e->loc.off + e->loc.len <= sf->len) {
        size_t i;
        for (i = 0; i < e->loc.len; i++) {
            char ch = sf->text[e->loc.off + i];
            if (ch == '\n' || ch == '\r')
                break;
            sb_putc(out, ch);
        }
        if (i == e->loc.len)
            return;
    }
    sb_puts(out, "the condition");
}

/* "x == Name" for a case value */
static void chk_case_value(const IrModule *m, const IrExpr *subj, const IrBits *v, StrBuf *out)
{
    const IrType *t = subj->type;
    uint32_t k;
    chk_expr_text(m, subj, out);
    sb_puts(out, " == ");
    if (!t && subj->kind == IRE_SIG && subj->sig < m->nsigs)
        t = m->sigs[subj->sig].type;
    if (t && t->kind == IRT_ENUM)
        for (k = 0; k < t->nvariants; k++)
            if (ir_bits_eq(&t->variants[k].tag, v) || (t->variants[k].tag.width && v->width >= t->tag_width &&
                                                       ir_bits_low64(&t->variants[k].tag) == ir_bits_low64(v) &&
                                                       v->width == t->width && !t->variants[k].payload)) {
                sb_puts(out, t->variants[k].name);
                return;
            }
    if (t && t->kind == IRT_BOOL) {
        sb_puts(out, ir_bits_low64(v) ? "true" : "false");
        return;
    }
    sb_printf(out, "%llu", (unsigned long long)ir_bits_low64(v));
}

/* the subject values no arm of a case covers (for the message) */
static int chk_missing_value(const IrModule *m, const IrStmt *s, IrBits *out, Arena *a)
{
    const IrType *t = s->subject->type;
    uint32_t k, i, j;
    if (!t && s->subject->kind == IRE_SIG && s->subject->sig < m->nsigs)
        t = m->sigs[s->subject->sig].type;
    if (t && t->kind == IRT_ENUM && t->width == t->tag_width) {
        for (k = 0; k < t->nvariants; k++) {
            int hit = 0;
            for (i = 0; i < s->narms && !hit; i++)
                for (j = 0; j < s->arms[i].nvals; j++)
                    if (ir_bits_low64(&s->arms[i].vals[j]) == ir_bits_low64(&t->variants[k].tag))
                        hit = 1;
            if (!hit) {
                *out = ir_bits_copy(a, &t->variants[k].tag);
                return 1;
            }
        }
        return 0;
    }
    if (s->subject->width <= 12) {
        uint64_t v, n = (uint64_t)1 << s->subject->width;
        for (v = 0; v < n; v++) {
            int hit = 0;
            for (i = 0; i < s->narms && !hit; i++)
                for (j = 0; j < s->arms[i].nvals; j++)
                    if (ir_bits_low64(&s->arms[i].vals[j]) == v)
                        hit = 1;
            if (!hit) {
                *out = ir_bits_u64(a, s->subject->width, v);
                return 1;
            }
        }
    }
    return 0;
}

static void chk_full_block(ChkFull *f, const IrBlock *b, ChkCov *cov);

/* runs `b` on a copy of `cov` under the extra path condition `cond`
 * (appended to f->path); returns the copy */
static ChkCov chk_full_branch(ChkFull *f, const IrBlock *b, const ChkCov *cov, const char *cond)
{
    ChkCov r = chk_cov_dup(cov);
    size_t mark = f->path.len;
    if (cond) {
        if (f->path.len)
            sb_puts(&f->path, " and ");
        sb_puts(&f->path, cond);
    }
    chk_full_block(f, b, &r);
    if (!f->have_fail && !chk_cov_full(&r, f->width) && cond) {
        sb_reset(&f->fail);
        sb_puts(&f->fail, sb_cstr(&f->path));
        f->have_fail = 2; /* tentative: confirmed if the merge leaves the signal incomplete */
    }
    sb_truncate(&f->path, mark);
    return r;
}

static void chk_full_block(ChkFull *f, const IrBlock *b, ChkCov *cov)
{
    const IrModule *m = f->c->m;
    uint32_t i, k, w;
    Arena a = {0};
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (chk_cov_full(cov, f->width))
            break; /* later statements cannot un-assign it */
        switch (s->kind) {
        case IRST_ASSIGN:
            if (s->lhs.sig == f->sig && !s->lhs.index)
                chk_cov_set(cov, s->lhs.lo, s->lhs.lo + s->lhs.width);
            break;
        case IRST_IF: {
            StrBuf ct = {0}, ce = {0}, tx = {0};
            ChkCov t, e;
            chk_expr_text(m, s->cond, &tx);
            sb_printf(&ct, "`%s`", sb_cstr(&tx));
            sb_printf(&ce, "`%s` is false", sb_cstr(&tx));
            sb_free(&tx);
            t = chk_full_branch(f, &s->then_b, cov, sb_cstr(&ct));
            e = chk_full_branch(f, &s->else_b, cov, sb_cstr(&ce));
            for (w = 0; w < cov->nw; w++)
                cov->w[w] = t.w[w] & e.w[w];
            free(t.w);
            free(e.w);
            sb_free(&ct);
            sb_free(&ce);
            break;
        }
        case IRST_CASE: {
            ChkCov acc;
            int first = 1, has_default = 0;
            acc.nw = cov->nw;
            acc.w = (uint64_t *)xcalloc(cov->nw, sizeof(uint64_t));
            for (k = 0; k < s->narms; k++) {
                StrBuf cd = {0};
                ChkCov r;
                IrBits mv;
                sb_putc(&cd, '`');
                if (s->arms[k].nvals)
                    chk_case_value(m, s->subject, &s->arms[k].vals[0], &cd);
                else if (chk_missing_value(m, s, &mv, &a))
                    chk_case_value(m, s->subject, &mv, &cd);
                else {
                    chk_expr_text(m, s->subject, &cd);
                    sb_puts(&cd, "` matches no arm");
                }
                if (cd.data[cd.len - 1] != 'm')
                    sb_putc(&cd, '`');
                if (!s->arms[k].nvals)
                    has_default = 1;
                r = chk_full_branch(f, &s->arms[k].body, cov, sb_cstr(&cd));
                for (w = 0; w < cov->nw; w++)
                    acc.w[w] = first ? r.w[w] : (acc.w[w] & r.w[w]);
                first = 0;
                free(r.w);
                sb_free(&cd);
            }
            if (!has_default) {
                /* values without an arm (should not happen: the elaborator adds a default) */
                IrBits mv;
                if (chk_missing_value(m, s, &mv, &a)) {
                    for (w = 0; w < cov->nw; w++)
                        acc.w[w] = first ? cov->w[w] : (acc.w[w] & cov->w[w]);
                    first = 0;
                }
            }
            if (!first)
                memcpy(cov->w, acc.w, cov->nw * sizeof(uint64_t));
            free(acc.w);
            break;
        }
        default:
            break;
        }
    }
    arena_release(&a);
}

static void chk_collect_targets(const IrBlock *b, Map *out)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (s->kind == IRST_ASSIGN)
            map_put(out, s->lhs.sig, 1);
        chk_collect_targets(&s->then_b, out);
        chk_collect_targets(&s->else_b, out);
        for (k = 0; k < s->narms; k++)
            chk_collect_targets(&s->arms[k].body, out);
    }
}

static void chk_assign(ChkCtx *c)
{
    const IrModule *m = c->m;
    uint32_t p;
    for (p = 0; p < m->nprocs; p++) {
        const IrProcess *pr = &m->procs[p];
        Map targets = {0};
        size_t it = 0;
        uint64_t key, val;
        if (pr->kind != IRP_COMB)
            continue;
        chk_collect_targets(&pr->body, &targets);
        while (map_next(&targets, &it, &key, &val)) {
            uint32_t sig = (uint32_t)key;
            ChkFull f;
            ChkCov cov;
            if (sig >= m->nsigs || (m->sigs[sig].kind != IRS_VAR && m->sigs[sig].kind != IRS_OUT))
                continue;
            memset(&f, 0, sizeof f);
            f.c = c;
            f.sig = sig;
            f.width = m->sigs[sig].type->width;
            cov.nw = (f.width + 63) / 64;
            cov.w = (uint64_t *)xcalloc(cov.nw ? cov.nw : 1, sizeof(uint64_t));
            chk_full_block(&f, &pr->body, &cov);
            if (!chk_cov_full(&cov, f.width)) {
                if (f.have_fail)
                    chk_error(c, m->sigs[sig].loc, "`%s` may be unassigned when %s", chk_sig_name(m, sig),
                              sb_cstr(&f.fail));
                else
                    chk_error(c, m->sigs[sig].loc, "`%s` is not assigned on every path of its `comb` block",
                              chk_sig_name(m, sig));
                diag_note(pr->loc, "in this `comb` block");
                diag_help("assign `%s` on every path, or give it a default: `var %s: T = ..`", chk_sig_name(m, sig),
                          m->sigs[sig].name);
            }
            free(cov.w);
            sb_free(&f.path);
            sb_free(&f.fail);
        }
        map_free(&targets);
    }
}

/* ===================================================================== cdc */

typedef struct ChkDom {
    ChkCtx *c;
    uint32_t *dom;            /* per signal: canonical domain, CHK_NONE */
    int *declared;            /* the domain is declared (not inferred) */
    uint32_t sig_a, sig_b;    /* the two signals of a mixed expression */
} ChkDom;

/* the clock a derived clock wire comes from (`clk as clock<falling>`) */
static uint32_t chk_clock_root(const IrModule *m, uint32_t sig)
{
    uint32_t steps, i;
    for (steps = 0; steps < 16; steps++) {
        const IrExpr *e = NULL;
        if (sig >= m->nsigs || m->sigs[sig].kind != IRS_WIRE)
            break;
        for (i = 0; i < m->nassigns; i++)
            if (m->assigns[i].lhs.sig == sig) {
                e = m->assigns[i].rhs;
                break;
            }
        while (e && (e->kind == IRE_RESIZE || (e->kind == IRE_UNARY && e->op == IROP_NOT) ||
                     (e->kind == IRE_SLICE && e->width == 1)))
            e = e->args[0];
        if (!e || e->kind != IRE_SIG)
            break;
        sig = e->sig;
    }
    return sig;
}

static uint32_t chk_canon(const IrModule *m, uint32_t d)
{
    uint32_t i, root;
    if (d == CHK_NONE || d >= m->ndomains)
        return CHK_NONE;
    root = chk_clock_root(m, m->domains[d].clock);
    for (i = 0; i < d; i++)
        if (chk_clock_root(m, m->domains[i].clock) == root)
            return i;
    return d;
}

static const char *chk_dom_name(const IrModule *m, uint32_t d)
{
    if (d >= m->ndomains)
        return "?";
    if (m->domains[d].name && m->domains[d].name[0])
        return m->domains[d].name;
    return m->sigs[m->domains[d].clock].name;
}

/* domain of an expression; CHK_MIXED with dd->sig_a/sig_b set when it mixes two */
static uint32_t chk_expr_dom(ChkDom *dd, const IrExpr *e, uint32_t acc, uint32_t *src)
{
    uint32_t i;
    if (!e || acc == CHK_MIXED)
        return acc;
    if (e->kind == IRE_SIG && e->sig < dd->c->m->nsigs) {
        uint32_t d = dd->dom[e->sig];
        if (d != CHK_NONE) {
            if (acc == CHK_NONE) {
                *src = e->sig;
                return d;
            }
            if (acc != d) {
                dd->sig_a = *src;
                dd->sig_b = e->sig;
                return CHK_MIXED;
            }
        }
        return acc;
    }
    if (e->kind == IRE_PAST)
        return acc;
    for (i = 0; i < e->nargs; i++)
        acc = chk_expr_dom(dd, e->args[i], acc, src);
    return acc;
}

static void chk_mixed(ChkDom *dd, Span at)
{
    const IrModule *m = dd->c->m;
    chk_error(dd->c, at, "%s-domain signal `%s` used in %s-domain logic; cross with std::cdc",
              chk_dom_name(m, dd->dom[dd->sig_b]), chk_sig_name(m, dd->sig_b), chk_dom_name(m, dd->dom[dd->sig_a]));
}

/* a value of domain d (from signal src) flows into target sig / domain td */
static void chk_flow(ChkDom *dd, uint32_t d, uint32_t src, uint32_t td, Span at)
{
    const IrModule *m = dd->c->m;
    if (d == CHK_NONE || td == CHK_NONE || d == td)
        return;
    chk_error(dd->c, at, "%s-domain signal `%s` used in %s-domain logic; cross with std::cdc", chk_dom_name(m, d),
              chk_sig_name(m, src), chk_dom_name(m, td));
}

typedef struct ChkCond {
    uint32_t dom, src;
} ChkCond;

/* mode 0: infer domains of undeclared targets (returns 1 on change);
 * mode 1: report */
static int chk_dom_block(ChkDom *dd, const IrBlock *b, ChkCond cond, uint32_t pdom, int mode)
{
    const IrModule *m = dd->c->m;
    uint32_t i, k;
    int changed = 0;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        uint32_t src = CHK_NONE, d;
        switch (s->kind) {
        case IRST_ASSIGN:
        case IRST_MEMWRITE:
        case IRST_ASSERT: {
            const IrExpr *e1 = s->kind == IRST_ASSIGN ? s->rhs : s->kind == IRST_MEMWRITE ? s->data : s->cond;
            src = cond.src;
            d = chk_expr_dom(dd, e1, cond.dom, &src);
            if (s->kind == IRST_ASSIGN)
                d = chk_expr_dom(dd, s->lhs.index, d, &src);
            else if (s->kind == IRST_MEMWRITE) {
                d = chk_expr_dom(dd, s->addr, d, &src);
                d = chk_expr_dom(dd, s->mask, d, &src);
            }
            if (mode == 1) {
                if (d == CHK_MIXED)
                    chk_mixed(dd, s->loc);
                else if (pdom != CHK_NONE)
                    chk_flow(dd, d, src, pdom, s->loc);
                else if (s->kind == IRST_ASSIGN && s->lhs.sig < m->nsigs && dd->declared[s->lhs.sig])
                    chk_flow(dd, d, src, dd->dom[s->lhs.sig], s->loc);
            } else if (s->kind == IRST_ASSIGN && s->lhs.sig < m->nsigs && !dd->declared[s->lhs.sig] && d != CHK_MIXED &&
                       d != CHK_NONE && dd->dom[s->lhs.sig] == CHK_NONE && pdom == CHK_NONE) {
                dd->dom[s->lhs.sig] = d;
                changed = 1;
            }
            break;
        }
        case IRST_IF:
        case IRST_CASE: {
            ChkCond c2 = cond;
            src = cond.src;
            c2.dom = chk_expr_dom(dd, s->kind == IRST_IF ? s->cond : s->subject, cond.dom, &src);
            c2.src = src;
            if (c2.dom == CHK_MIXED) {
                if (mode == 1)
                    chk_mixed(dd, s->loc);
                c2 = cond;
            } else if (mode == 1 && pdom != CHK_NONE && c2.dom != CHK_NONE && c2.dom != pdom) {
                chk_flow(dd, c2.dom, src, pdom, s->loc);
                c2 = cond;
            }
            if (s->kind == IRST_IF) {
                changed |= chk_dom_block(dd, &s->then_b, c2, pdom, mode);
                changed |= chk_dom_block(dd, &s->else_b, c2, pdom, mode);
            } else {
                for (k = 0; k < s->narms; k++)
                    changed |= chk_dom_block(dd, &s->arms[k].body, c2, pdom, mode);
            }
            break;
        }
        }
    }
    return changed;
}

static void chk_cdc(ChkCtx *c)
{
    const IrModule *m = c->m;
    ChkDom dd;
    uint32_t i, k, iter;
    ChkCond none;
    if (m->is_cdc || !m->ndomains)
        return;
    for (i = 0; i < m->nattrs; i++)
        if (strcmp(m->attrs[i].name, "clock_gate") == 0 || strcmp(m->attrs[i].name, "clock_gen") == 0)
            return; /* clock generation mixes clocks and enables by design */
    memset(&dd, 0, sizeof dd);
    dd.c = c;
    dd.dom = (uint32_t *)xcalloc(m->nsigs ? m->nsigs : 1, sizeof(uint32_t));
    dd.declared = (int *)xcalloc(m->nsigs ? m->nsigs : 1, sizeof(int));
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        int exempt = s->type->kind == IRT_CLOCK || s->type->kind == IRT_RESET || s->kind == IRS_MEM;
        for (k = 0; k < s->nattrs && !exempt; k++)
            if (strcmp(s->attrs[k].name, "unsafe_cross") == 0)
                exempt = 1;
        dd.dom[i] = exempt ? CHK_NONE : chk_canon(m, s->domain);
        dd.declared[i] = exempt || dd.dom[i] != CHK_NONE;
    }
    none.dom = CHK_NONE;
    none.src = CHK_NONE;
    /* infer the domains of wires, vars and outputs from what drives them */
    for (iter = 0; iter < 64; iter++) {
        int changed = 0;
        for (i = 0; i < m->nassigns; i++) {
            const IrAssign *a = &m->assigns[i];
            uint32_t src = CHK_NONE, d;
            if (a->lhs.sig >= m->nsigs || dd.declared[a->lhs.sig] || dd.dom[a->lhs.sig] != CHK_NONE)
                continue;
            d = chk_expr_dom(&dd, a->rhs, CHK_NONE, &src);
            if (d != CHK_NONE && d != CHK_MIXED) {
                dd.dom[a->lhs.sig] = d;
                changed = 1;
            }
        }
        for (i = 0; i < m->nprocs; i++)
            if (m->procs[i].kind == IRP_COMB)
                changed |= chk_dom_block(&dd, &m->procs[i].body, none, CHK_NONE, 0);
        if (!changed)
            break;
    }
    /* report */
    for (i = 0; i < m->nassigns; i++) {
        const IrAssign *a = &m->assigns[i];
        uint32_t src = CHK_NONE, d = chk_expr_dom(&dd, a->rhs, CHK_NONE, &src);
        if (d == CHK_MIXED)
            chk_mixed(&dd, a->loc);
        else if (a->lhs.sig < m->nsigs && (dd.declared[a->lhs.sig] || dd.dom[a->lhs.sig] != CHK_NONE))
            chk_flow(&dd, d, src, dd.dom[a->lhs.sig], a->loc);
    }
    for (i = 0; i < m->nprocs; i++) {
        const IrProcess *p = &m->procs[i];
        chk_dom_block(&dd, &p->body, none, p->kind == IRP_CLOCKED ? chk_canon(m, p->domain) : CHK_NONE, 1);
    }
    free(dd.dom);
    free(dd.declared);
}

/* ================================================================== entry */

int check_design(IrDesign *d, const CheckOptions *opt)
{
    ChkCtx c;
    uint32_t i;
    TERN_UNUSED(opt);
    memset(&c, 0, sizeof c);
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        if (m->is_extern)
            continue;
        c.m = m;
        chk_drivers(&c);
        chk_assign(&c);
        chk_cdc(&c);
    }
    map_free(&c.seen);
    return c.errors;
}

#undef CHK_NONE
#undef CHK_MIXED
