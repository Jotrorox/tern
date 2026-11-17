/* poweron.c — reset-less registers read before any write (spec §6.5).
 *
 * "A reset-less reg read before any write on some path is a warning from
 * tern check (an error with --strict)." A register declared without a
 * reset value (`reg x: T`, no `= v`) starts with an unknown value (random
 * per seed in simulation). Reading it is harmless when the value is data
 * that is overwritten before anything depends on it — a data register
 * loaded under a valid bit or a start strobe that has a reset value — so
 * the rule looks at how the register gets its value:
 *
 *   A *load* is an assignment to the register whose value and whose
 *   enclosing conditions do not depend on the register itself (through
 *   wires and comb variables, bit-accurately). A register with a load can
 *   be written before it is read, and is not reported (data registers,
 *   iterative datapaths loaded on `start`, accumulators with a clear).
 *
 * A register without any load is reported when
 *   (a) feedback: its next value is computed from its current value
 *       (`acc = acc +% d`, a counter, an LFSR); pure bit routing of its own
 *       bits (`sr = sr[6:0] ++ din`, a shift register that keeps shifting
 *       data in) does not count, or
 *   (b) control: its value decides a branch (an `if`/`match` condition, the
 *       select of an if-expression), a memory address or a dynamic index,
 *       directly or through wires and comb variables, anywhere in the
 *       module (a state register whose transitions all depend on its own
 *       value), unless it is written on every clock edge (an assignment
 *       outside any condition), which defines it while reset is asserted.
 * Registers read only as data that flows into outputs, other registers or
 * memories are not reported.
 *
 * Exempt: registers with a reset or power-on value, the registers the
 * elaborator creates for synchronous memory reads (assigned only
 * `IRE_MEMREAD`), #[test_port] registers and signals declared outside the
 * user's files (CheckOptions.is_user_file). Assertions and properties do
 * not count as reads.
 */
#include "check_impl.h"

typedef struct PoRead {
    uint32_t sig;
    uint32_t lo, hi;      /* bits [lo, hi) read */
    uint8_t computed;     /* the value passes through a non-routing operator */
    Span loc;             /* the reading expression */
} PoRead;

typedef VEC(PoRead) PoReadVec;

/* one assignment: to a comb-driven signal (drivers) or to a register */
typedef struct PoAsg {
    uint32_t sig, lo, hi;     /* target bits */
    uint32_t rd, nrd;         /* value reads (rhs, dynamic index): reads[rd ..) */
    uint32_t cd, ncd;         /* reads of the enclosing conditions: reads[cd ..) */
    uint8_t uncond;           /* whole register, outside any condition */
    uint8_t memread;          /* rhs is a synchronous memory read */
} PoAsg;

typedef struct PoCtl {        /* why a register is control */
    Span cond;                /* the condition / address expression */
    Span read;                /* where the register is read on the way there */
    uint32_t via;             /* the wire / var it flows through, or IR_NONE */
    uint8_t set;
} PoCtl;

typedef struct PoMod {
    ChkCtx *cx;
    const IrModule *m;
    PoReadVec reads;          /* all reads, referenced by PoAsg */
    VEC(PoAsg) comb;          /* assignments to comb-driven signals */
    VEC(PoAsg) clk;           /* assignments to registers */
    uint32_t *cfirst;         /* per signal: comb assignments [cfirst[s], cfirst[s+1]) (sorted) */
    PoReadVec conds;          /* reads of conditions / addresses */
    VEC(Span) cond_of;        /* per conds entry: the condition expression */
    PoReadVec cstack;         /* reads of the enclosing conditions while walking */
    Map seen;
    PoCtl *ctl;
} PoMod;

static Span po_loc(const IrExpr *e, Span fallback)
{
    return e && e->loc.file ? e->loc : fallback;
}

static void po_add(PoReadVec *v, uint32_t sig, uint32_t lo, uint32_t hi, int computed, Span loc)
{
    PoRead r;
    r.sig = sig;
    r.lo = lo;
    r.hi = hi;
    r.computed = (uint8_t)(computed != 0);
    r.loc = loc;
    vec_push(*v, r);
}

/* a signal or a constant slice of one: its bits */
static int po_sig_bits(const IrExpr *e, uint32_t *sig, uint32_t *lo, uint32_t *hi)
{
    uint32_t off = 0, w = e->width;
    while (e->kind == IRE_SLICE) {
        off += e->lo;
        e = e->args[0];
    }
    if (e->kind != IRE_SIG)
        return 0;
    *sig = e->sig;
    *lo = off;
    *hi = off + w;
    return 1;
}

static void po_expr(PoMod *pm, const IrExpr *e, int computed, PoReadVec *sink, Span at);

/* a condition / address: control reads (and computed reads of sink) */
static void po_cond(PoMod *pm, const IrExpr *e, PoReadVec *sink, Span at)
{
    PoReadVec tmp = {0};
    size_t i;
    Span cs = po_loc(e, at);
    po_expr(pm, e, 1, &tmp, cs);
    for (i = 0; i < tmp.len; i++) {
        vec_push(pm->conds, tmp.data[i]);
        vec_push(pm->cond_of, cs);
        if (sink)
            vec_push(*sink, tmp.data[i]);
    }
    vec_free(tmp);
}

static void po_expr(PoMod *pm, const IrExpr *e, int computed, PoReadVec *sink, Span at)
{
    uint32_t i, sig, lo, hi;
    if (!e)
        return;
    at = po_loc(e, at);
    switch (e->kind) {
    case IRE_CONST:
    case IRE_PAST: return;
    case IRE_SIG:
    case IRE_SLICE:
        if (po_sig_bits(e, &sig, &lo, &hi)) {
            if (sig < pm->m->nsigs && sink)
                po_add(sink, sig, lo, hi, computed, at);
            return;
        }
        po_expr(pm, e->args[0], computed, sink, at);
        return;
    case IRE_RESIZE:
    case IRE_CONCAT:
        for (i = 0; i < e->nargs; i++)
            po_expr(pm, e->args[i], computed, sink, at);
        return;
    case IRE_MUX:
        po_cond(pm, e->args[0], sink, at);
        po_expr(pm, e->args[1], computed, sink, at);
        po_expr(pm, e->args[2], computed, sink, at);
        return;
    case IRE_INDEX:
        po_expr(pm, e->args[0], computed, sink, at);
        po_cond(pm, e->args[1], sink, at);
        return;
    case IRE_MEMREAD:
        po_cond(pm, e->args[0], sink, at);
        return;
    case IRE_BINARY:
        /* a shift by a constant only moves bits */
        if ((e->op == IROP_SHL || e->op == IROP_SHR) && e->nargs == 2 && e->args[1]->kind == IRE_CONST) {
            po_expr(pm, e->args[0], computed, sink, at);
            return;
        }
        break;
    default: break;
    }
    for (i = 0; i < e->nargs; i++)
        po_expr(pm, e->args[i], 1, sink, at);
}

static void po_record(PoMod *pm, const IrLvalue *lv, const IrExpr *rhs, Span loc, int clocked, uint32_t depth)
{
    PoAsg a;
    PoReadVec tmp = {0};
    size_t i;
    uint32_t sw;
    if (lv->sig >= pm->m->nsigs)
        return;
    sw = pm->m->sigs[lv->sig].type->width;
    if (clocked && pm->m->sigs[lv->sig].kind != IRS_REG)
        return;
    memset(&a, 0, sizeof a);
    a.sig = lv->sig;
    if (lv->index) {
        a.lo = lv->lo;
        a.hi = sw;
    } else {
        a.lo = lv->lo;
        a.hi = lv->lo + lv->width;
    }
    a.memread = rhs && rhs->kind == IRE_MEMREAD;
    a.uncond = depth == 0 && !lv->index && lv->lo == 0 && lv->width == sw;
    po_expr(pm, rhs, 0, &tmp, loc);
    if (lv->index)
        po_cond(pm, lv->index, &tmp, loc);
    a.rd = (uint32_t)pm->reads.len;
    a.nrd = (uint32_t)tmp.len;
    for (i = 0; i < tmp.len; i++)
        vec_push(pm->reads, tmp.data[i]);
    a.cd = (uint32_t)pm->reads.len;
    a.ncd = (uint32_t)pm->cstack.len;
    for (i = 0; i < pm->cstack.len; i++)
        vec_push(pm->reads, pm->cstack.data[i]);
    vec_free(tmp);
    if (clocked)
        vec_push(pm->clk, a);
    else
        vec_push(pm->comb, a);
}

static void po_block(PoMod *pm, const IrBlock *b, int clocked, uint32_t depth);

static void po_stmt(PoMod *pm, const IrStmt *s, int clocked, uint32_t depth)
{
    size_t mark = pm->cstack.len;
    uint32_t a;
    switch (s->kind) {
    case IRST_ASSIGN: po_record(pm, &s->lhs, s->rhs, s->loc, clocked, depth); return;
    case IRST_IF:
        po_cond(pm, s->cond, &pm->cstack, s->loc);
        po_block(pm, &s->then_b, clocked, depth + 1);
        po_block(pm, &s->else_b, clocked, depth + 1);
        pm->cstack.len = mark;
        return;
    case IRST_CASE:
        po_cond(pm, s->subject, &pm->cstack, s->loc);
        for (a = 0; a < s->narms; a++)
            po_block(pm, &s->arms[a].body, clocked, depth + 1);
        pm->cstack.len = mark;
        return;
    case IRST_MEMWRITE:
        po_cond(pm, s->addr, NULL, s->loc);
        return;
    case IRST_ASSERT: return;
    }
}

static void po_block(PoMod *pm, const IrBlock *b, int clocked, uint32_t depth)
{
    uint32_t i;
    for (i = 0; i < b->n; i++)
        po_stmt(pm, b->stmts[i], clocked, depth);
}

static int po_asg_cmp(const void *pa, const void *pb)
{
    const PoAsg *a = (const PoAsg *)pa, *b = (const PoAsg *)pb;
    if (a->sig != b->sig)
        return a->sig < b->sig ? -1 : 1;
    if (a->lo != b->lo)
        return a->lo < b->lo ? -1 : 1;
    return 0;
}

static uint64_t po_key(uint32_t sig, uint32_t lo, uint32_t hi, uint32_t tag)
{
    return hash_combine(hash_combine(hash_u64(sig), hash_u64(((uint64_t)lo << 32) | hi)), hash_u64(tag)) | 1;
}

/* How the reads [first, first + n) reach register r through the comb
 * drivers: 0 not at all, 1 only by routing its bits, 2 through a
 * computation. *where gets the read that reaches it. */
static int po_reach(PoMod *pm, uint32_t r, uint32_t first, uint32_t n, int force_computed, Span *where)
{
    PoReadVec stack = {0};
    uint32_t i;
    int best = 0;
    map_clear(&pm->seen);
    for (i = 0; i < n; i++) {
        PoRead rd = pm->reads.data[first + i];
        if (force_computed)
            rd.computed = 1;
        vec_push(stack, rd);
    }
    while (stack.len && best < 2) {
        PoRead rd = vec_pop(stack);
        uint32_t k;
        if (rd.sig == r) {
            if (1 + rd.computed > best) {
                best = 1 + rd.computed;
                *where = rd.loc;
            }
            continue;
        }
        if (map_has(&pm->seen, po_key(rd.sig, rd.lo, rd.hi, rd.computed)))
            continue;
        map_put(&pm->seen, po_key(rd.sig, rd.lo, rd.hi, rd.computed), 1);
        for (k = pm->cfirst[rd.sig]; k < pm->cfirst[rd.sig + 1]; k++) {
            const PoAsg *a = &pm->comb.data[k];
            uint32_t j;
            if (a->hi <= rd.lo || a->lo >= rd.hi)
                continue;
            for (j = 0; j < a->nrd + a->ncd; j++) {
                PoRead nx = pm->reads.data[(j < a->nrd ? a->rd : a->cd - a->nrd) + j];
                nx.computed = (uint8_t)(nx.computed | rd.computed | (j >= a->nrd));
                if (nx.sig == r)
                    nx.loc = rd.loc; /* where the wire is read */
                vec_push(stack, nx);
            }
        }
    }
    vec_free(stack);
    return best;
}

/* control closure: conditions, then back through the comb drivers */
static void po_control(PoMod *pm)
{
    typedef struct PoWork {
        PoRead rd;
        uint32_t ci;          /* conds index of the origin */
        uint32_t via;
    } PoWork;
    VEC(PoWork) work = {0};
    size_t i;
    map_clear(&pm->seen);
    for (i = 0; i < pm->conds.len; i++) {
        PoWork w;
        w.rd = pm->conds.data[i];
        w.ci = (uint32_t)i;
        w.via = IR_NONE;
        vec_push(work, w);
    }
    while (work.len) {
        PoWork w = vec_pop(work);
        const IrSignal *sg = &pm->m->sigs[w.rd.sig];
        uint32_t k;
        if (sg->kind == IRS_REG && !pm->ctl[w.rd.sig].set) {
            PoCtl *c = &pm->ctl[w.rd.sig];
            c->set = 1;
            c->cond = pm->cond_of.data[w.ci];
            c->read = w.rd.loc;
            c->via = w.via;
        }
        if (map_has(&pm->seen, po_key(w.rd.sig, w.rd.lo, w.rd.hi, 0)))
            continue;
        map_put(&pm->seen, po_key(w.rd.sig, w.rd.lo, w.rd.hi, 0), 1);
        for (k = pm->cfirst[w.rd.sig]; k < pm->cfirst[w.rd.sig + 1]; k++) {
            const PoAsg *a = &pm->comb.data[k];
            uint32_t j;
            if (a->hi <= w.rd.lo || a->lo >= w.rd.hi)
                continue;
            for (j = 0; j < a->nrd + a->ncd; j++) {
                PoWork nx;
                nx.rd = pm->reads.data[(j < a->nrd ? a->rd : a->cd - a->nrd) + j];
                nx.ci = w.ci;
                nx.via = w.via != IR_NONE ? w.via : w.rd.sig;
                vec_push(work, nx);
            }
        }
    }
    vec_free(work);
}

static const char *po_name(const PoMod *pm, uint32_t s)
{
    const IrSignal *sg = &pm->m->sigs[s];
    return sg->path ? sg->path : sg->name;
}

static void po_help(const PoMod *pm, uint32_t s)
{
    const IrSignal *sg = &pm->m->sigs[s];
    const IrType *t = sg->type;
    char ty[96];
    const char *init = "0";
    switch (t->kind) {
    case IRT_BOOL: snprintf(ty, sizeof ty, "bool"); init = "false"; break;
    case IRT_UINT:
        if (t->width == 8 || t->width == 16 || t->width == 32 || t->width == 64)
            snprintf(ty, sizeof ty, "u%u", (unsigned)t->width);
        else
            snprintf(ty, sizeof ty, "uint<%u>", (unsigned)t->width);
        break;
    case IRT_SINT:
        if (t->width == 8 || t->width == 16 || t->width == 32 || t->width == 64)
            snprintf(ty, sizeof ty, "s%u", (unsigned)t->width);
        else
            snprintf(ty, sizeof ty, "sint<%u>", (unsigned)t->width);
        break;
    case IRT_BITS: snprintf(ty, sizeof ty, "bit<%u>", (unsigned)t->width); init = "all0"; break;
    case IRT_ENUM:
        snprintf(ty, sizeof ty, "%s", t->name ? t->name : "T");
        init = t->nvariants ? t->variants[0].name : "...";
        break;
    default: snprintf(ty, sizeof ty, "T"); init = "..."; break;
    }
    diag_note(sg->loc, "`%s` is declared without a reset value", po_name(pm, s));
    if (t->kind == IRT_ENUM && t->nvariants)
        diag_help("give it a reset value (`reg %s: %s = .%s`) or write it before reading", sg->name, ty, init);
    else
        diag_help("give it a reset value (`reg %s: %s = %s`) or write it before reading", sg->name, ty, init);
}

static void po_check_reg(PoMod *pm, uint32_t r, uint32_t first, uint32_t end)
{
    ChkCtx *cx = pm->cx;
    const IrSignal *sg = &pm->m->sigs[r];
    const CheckOptions *o = cx->opt;
    uint32_t i;
    int feedback = 0, uncond = 0, all_memread = 1;
    Span fb_at = span_none();
    if (sg->has_init || sg->test_port)
        return;
    if (o->is_user_file && !o->is_user_file(sg->loc.file, o->user))
        return;
    for (i = first; i < end; i++)
        if (!pm->clk.data[i].memread)
            all_memread = 0;
    if (all_memread)
        return;
    for (i = first; i < end; i++) {
        const PoAsg *a = &pm->clk.data[i];
        Span at = span_none();
        int v = po_reach(pm, r, a->rd, a->nrd, 0, &at);
        int c = po_reach(pm, r, a->cd, a->ncd, 1, &at);
        if (!v && !c)
            return; /* a load: it can be written before it is read */
        if (v == 2 && !feedback) {
            feedback = 1;
            fb_at = at;
        }
        if (a->uncond && v < 2)
            uncond = 1;
    }
    if (feedback) {
        const char *txt = chk_span_text(cx, fb_at, 60);
        if (chk_warning(cx, fb_at,
                        "reset-less register `%s` is read before any write: its next value is computed from its "
                        "current value%s%s%s",
                        po_name(pm, r), *txt ? " (`" : "", txt, *txt ? "`)" : ""))
            po_help(pm, r);
    } else if (pm->ctl[r].set && !uncond) {
        const PoCtl *c = &pm->ctl[r];
        const char *txt = chk_span_text(cx, c->cond, 60);
        int ok;
        if (!*txt)
            txt = "a condition";
        if (c->via != IR_NONE && strcmp(txt, po_name(pm, c->via)) != 0)
            ok = chk_warning(cx, c->read,
                             "reset-less register `%s` is read before any write: the decision on `%s` depends on "
                             "it through `%s`",
                             po_name(pm, r), txt, po_name(pm, c->via));
        else
            ok = chk_warning(cx, c->read,
                             "reset-less register `%s` is read before any write: the decision on `%s` depends on it",
                             po_name(pm, r), txt);
        if (ok)
            po_help(pm, r);
    }
}

void chk_poweron(ChkMod *cm)
{
    const IrModule *m = cm->m;
    PoMod pm;
    uint32_t i, n = m->nsigs, cand = 0;
    for (i = 0; i < n; i++)
        if (m->sigs[i].kind == IRS_REG && !m->sigs[i].has_init && !m->sigs[i].test_port)
            cand = 1;
    if (!cand)
        return;
    memset(&pm, 0, sizeof pm);
    pm.cx = cm->cx;
    pm.m = m;
    for (i = 0; i < m->nprocs; i++) {
        const IrProcess *p = &m->procs[i];
        po_block(&pm, &p->body, p->kind == IRP_CLOCKED, 0);
        pm.cstack.len = 0;
    }
    for (i = 0; i < m->nassigns; i++)
        po_record(&pm, &m->assigns[i].lhs, m->assigns[i].rhs, m->assigns[i].loc, 0, 0);
    for (i = 0; i < m->ninsts; i++) {
        uint32_t k;
        const IrInstance *in = &m->insts[i];
        for (k = 0; k < in->nconns; k++)
            if (in->conns[k].expr && in->module->sigs[in->conns[k].port].kind != IRS_OUT)
                po_expr(&pm, in->conns[k].expr, 0, NULL, in->loc);
    }
    if (pm.comb.len)
        qsort(pm.comb.data, pm.comb.len, sizeof(PoAsg), po_asg_cmp);
    if (pm.clk.len)
        qsort(pm.clk.data, pm.clk.len, sizeof(PoAsg), po_asg_cmp);
    pm.cfirst = (uint32_t *)xcalloc(n + 1, sizeof(uint32_t));
    {
        uint32_t k = 0;
        for (i = 0; i <= n; i++) {
            while (k < pm.comb.len && pm.comb.data[k].sig < i)
                k++;
            pm.cfirst[i] = k;
        }
    }
    pm.ctl = (PoCtl *)xcalloc(n, sizeof(PoCtl));
    po_control(&pm);
    for (i = 0; i < pm.clk.len;) {
        uint32_t j = i;
        while (j < pm.clk.len && pm.clk.data[j].sig == pm.clk.data[i].sig)
            j++;
        po_check_reg(&pm, pm.clk.data[i].sig, i, j);
        i = j;
    }
    free(pm.cfirst);
    free(pm.ctl);
    map_free(&pm.seen);
    vec_free(pm.reads);
    vec_free(pm.comb);
    vec_free(pm.clk);
    vec_free(pm.conds);
    vec_free(pm.cond_of);
    vec_free(pm.cstack);
}
