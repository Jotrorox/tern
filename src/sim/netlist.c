/* netlist.c — builds a simulation model from an IrDesign:
 *
 *   1. flatten the instance hierarchy (signals, memories, domains)
 *   2. collect combinational nodes (continuous assigns, comb processes,
 *      port connections) and resolve every domain's clock to a root net
 *      (a top clock input or a derived clock) -> edge groups
 *   3. classify state (targets of clocked processes) and lay out values
 *      (state grouped per edge group so hold/commit are two memcpys)
 *   4. order the combinational nodes topologically (loops are errors; a
 *      comb process caught in a loop is first split into one node per
 *      target, since a block-level loop need not be a signal-level one)
 *   5. compile everything to SimInsn (comb segment, one segment per domain,
 *      one property segment per group), relocate, initialize values.
 */
#include "sim_impl.h"

#define NL_LIMBS(w) (((w) + 63u) / 64u)

/* ------------------------------------------------------------ flatten -- */

static const char *nl_join(const char *a, const char *b)
{
    StrBuf sb = {0};
    const char *r;
    if (*a) {
        sb_puts(&sb, a);
        sb_putc(&sb, '.');
    }
    sb_puts(&sb, b);
    r = intern(sb.data, sb.len);
    sb_free(&sb);
    return r;
}

static void nl_flatten(SimModel *m, const IrModule *mod, uint32_t parent, const char *name, Span loc)
{
    uint32_t idx = (uint32_t)m->insts.len, i;
    SimInst in;
    const char *path;
    memset(&in, 0, sizeof in);
    path = parent == IR_NONE ? intern_cstr("") : nl_join(m->insts.data[parent].path, name);
    in.path = path;
    in.name = intern_cstr(name);
    in.mod = mod;
    in.parent = parent;
    in.sig_base = (uint32_t)m->sigs.len;
    in.dom_base = (uint32_t)m->doms.len;
    in.nchildren = mod->ninsts;
    in.children = mod->ninsts ? ANEWN(&m->arena, uint32_t, mod->ninsts) : NULL;
    vec_push(m->insts, in);

    for (i = 0; i < mod->nsigs; i++) {
        const IrSignal *is = &mod->sigs[i];
        SimSig s;
        uint32_t h = (uint32_t)m->sigs.len;
        uint64_t *slot;
        int inserted;
        memset(&s, 0, sizeof s);
        s.path = nl_join(path, is->path ? is->path : is->name);
        s.svpath = nl_join(path, is->name);
        s.ir = is;
        s.inst = idx;
        s.width = is->type->width;
        s.limbs = NL_LIMBS(s.width);
        s.off = IR_NONE;
        s.next = IR_NONE;
        s.group = IR_NONE;
        s.mem = IR_NONE;
        s.clock = IR_NONE;
        if (is->kind == IRS_MEM) {
            SimMem mm;
            memset(&mm, 0, sizeof mm);
            mm.sig = h;
            mm.width = s.width;
            mm.limbs = s.limbs;
            mm.depth = is->depth;
            s.mem = (uint32_t)m->mems.len;
            vec_push(m->mems, mm);
        }
        s.force = IR_NONE;
        if (is->test_port && is->kind != IRS_MEM)
            s.writable = 1;
        if (parent == IR_NONE && is->kind == IRS_IN && is->type->kind != IRT_CLOCK &&
            is->type->kind != IRT_RESET)
            s.writable = 1;
        else if (s.writable) {
            /* driven by the design too: the test's value overrides it */
            SimForce f;
            f.sig = h;
            f.active = 0;
            f.val = (uint64_t *)xcalloc(s.limbs + 1, sizeof(uint64_t));
            s.force = (uint32_t)m->forces.len;
            vec_push(m->forces, f);
        }
        vec_push(m->sigs, s);
        slot = map_ref(&m->by_path, hash_bytes(s.path, strlen(s.path)), &inserted);
        /* a child's #[test_port] input shares its path with the parent's
         * connection wire ("i.inject"): the port is what a test drives */
        if (inserted || (is->test_port && is->kind == IRS_IN))
            *slot = h + 1;
        slot = map_ref(&m->by_path, hash_bytes(s.svpath, strlen(s.svpath)), &inserted);
        if (inserted)
            *slot = h + 1;
    }
    for (i = 0; i < mod->ndomains; i++) {
        const IrDomain *dm = &mod->domains[i];
        SimDom d;
        memset(&d, 0, sizeof d);
        d.inst = idx;
        d.ir = dm;
        d.clock = in.sig_base + dm->clock;
        d.reset = dm->reset == IR_NONE ? IR_NONE : in.sig_base + dm->reset;
        if (dm->reset != IR_NONE) {
            d.async = mod->sigs[dm->reset].type->async;
            d.low = mod->sigs[dm->reset].type->active_low;
        }
        d.group = IR_NONE;
        vec_push(m->doms, d);
    }
    if (mod->is_extern) {
        if (m->rops) /* simulated by the remote model */
            return;
        diag_warning(parent == IR_NONE ? mod->loc : loc,
                     "extern module `%s` (instance `%s`) is a black box in simulation; its outputs "
                     "read as 0",
                     mod->name, *path ? path : mod->name);
        return;
    }
    for (i = 0; i < mod->ninsts; i++) {
        m->insts.data[idx].children[i] = (uint32_t)m->insts.len;
        nl_flatten(m, mod->insts[i].module, idx, mod->insts[i].name, mod->insts[i].loc);
    }
}

/* -------------------------------------------------------------- nodes -- */

typedef struct NlNode {
    int is_proc;
    uint32_t lhs_inst, rhs_inst;
    IrLvalue lhs;
    const IrExpr *rhs;
    const IrProcess *proc;
    uint32_t split;           /* proc split per target (false loops): flat target + 1, else 0 */
    const IrAssign *asg;      /* a continuous assignment of the module (line coverage), or NULL */
    Span loc;
} NlNode;

typedef struct NlRange {
    uint32_t node, sig, lo, w;
} NlRange;

typedef struct NlEdge {
    uint32_t from, to, sig;
} NlEdge;

typedef struct NlBuild {
    VEC(NlNode) nodes;
    VEC(NlRange) reads;
    VEC(NlRange) writes;
    uint32_t *alias;          /* clock nets: flattened source of a pure copy */
    uint32_t *order;          /* topological order of nodes */
} NlBuild;

static uint32_t nl_flat(const SimModel *m, uint32_t inst, uint32_t sig)
{
    return m->insts.data[inst].sig_base + sig;
}

static void nl_add_node(NlBuild *b, const NlNode *n)
{
    vec_push(b->nodes, *n);
}

static void nl_collect_nodes(SimModel *m, NlBuild *b)
{
    uint32_t ii, i, j;
    for (ii = 0; ii < m->insts.len; ii++) {
        const SimInst *in = &m->insts.data[ii];
        const IrModule *mod = in->mod;
        NlNode n;
        if (mod->is_extern)
            continue;
        for (i = 0; i < mod->nassigns; i++) {
            memset(&n, 0, sizeof n);
            n.lhs_inst = n.rhs_inst = ii;
            n.lhs = mod->assigns[i].lhs;
            n.rhs = mod->assigns[i].rhs;
            n.loc = mod->assigns[i].loc;
            n.asg = &mod->assigns[i];
            nl_add_node(b, &n);
        }
        for (i = 0; i < mod->nprocs; i++) {
            if (mod->procs[i].kind != IRP_COMB)
                continue;
            memset(&n, 0, sizeof n);
            n.is_proc = 1;
            n.lhs_inst = n.rhs_inst = ii;
            n.proc = &mod->procs[i];
            n.loc = mod->procs[i].loc;
            nl_add_node(b, &n);
        }
        for (i = 0; i < mod->ninsts; i++) {
            const IrInstance *ci = &mod->insts[i];
            uint32_t child = in->children[i];
            for (j = 0; j < ci->nconns; j++) {
                const IrConn *cn = &ci->conns[j];
                const IrSignal *ps = &ci->module->sigs[cn->port];
                if (!cn->expr)
                    continue;
                memset(&n, 0, sizeof n);
                n.loc = ci->loc;
                if (ps->kind == IRS_OUT) {
                    const IrExpr *x = cn->expr;
                    IrExpr *pe;
                    n.lhs_inst = ii;
                    n.rhs_inst = child;
                    if (x->kind == IRE_SIG) {
                        n.lhs.sig = x->sig;
                        n.lhs.width = mod->sigs[x->sig].type->width;
                    } else if (x->kind == IRE_SLICE && x->args[0]->kind == IRE_SIG) {
                        n.lhs.sig = x->args[0]->sig;
                        n.lhs.lo = x->lo;
                        n.lhs.width = x->width;
                    } else {
                        continue;
                    }
                    pe = ANEW(&m->arena, IrExpr);
                    pe->kind = IRE_SIG;
                    pe->width = ps->type->width;
                    pe->sig = cn->port;
                    pe->type = ps->type;
                    n.rhs = pe;
                } else {
                    n.lhs_inst = child;
                    n.rhs_inst = ii;
                    n.lhs.sig = cn->port;
                    n.lhs.width = ps->type->width;
                    n.rhs = cn->expr;
                }
                if (n.lhs.width != n.rhs->width)
                    continue;
                nl_add_node(b, &n);
            }
        }
    }
}

/* ------------------------------------------------- clocks and groups -- */

static uint32_t nl_root(const NlBuild *b, uint32_t s)
{
    uint32_t k;
    for (k = 0; k < 1000 && b->alias[s] != IR_NONE; k++)
        s = b->alias[s];
    return s;
}

static uint32_t nl_clock_for(SimModel *m, uint32_t sig)
{
    uint32_t i;
    SimClock c;
    for (i = 0; i < m->clocks.len; i++)
        if (m->clocks.data[i].sig == sig)
            return i;
    memset(&c, 0, sizeof c);
    c.sig = sig;
    c.falling = m->sigs.data[sig].ir->type->falling;
    c.level = c.falling ? 1 : 0;
    c.g_active = c.g_inactive = IR_NONE;
    vec_push(m->clocks, c);
    m->sigs.data[sig].clock = (uint32_t)m->clocks.len - 1;
    return (uint32_t)m->clocks.len - 1;
}

static int nl_is_top_clock(const SimModel *m, uint32_t s)
{
    const SimSig *sg = &m->sigs.data[s];
    return sg->inst == 0 && sg->ir->kind == IRS_IN && sg->ir->type->kind == IRT_CLOCK;
}

static void nl_groups(SimModel *m, NlBuild *b)
{
    uint32_t i, j, nsig = (uint32_t)m->sigs.len;
    b->alias = (uint32_t *)xmalloc(sizeof(uint32_t) * (nsig ? nsig : 1));
    for (i = 0; i < nsig; i++)
        b->alias[i] = IR_NONE;
    for (i = 0; i < b->nodes.len; i++) {
        const NlNode *n = &b->nodes.data[i];
        uint32_t l;
        if (n->is_proc || n->lhs.index || n->rhs->kind != IRE_SIG)
            continue;
        l = nl_flat(m, n->lhs_inst, n->lhs.sig);
        if (m->sigs.data[l].ir->type->kind != IRT_CLOCK || n->lhs.lo != 0 ||
            n->lhs.width != m->sigs.data[l].width)
            continue;
        b->alias[l] = nl_flat(m, n->rhs_inst, n->rhs->sig);
    }
    /* top clocks first, in domain order, so clock indices are stable */
    for (i = 0; i < m->top->ndomains; i++)
        nl_clock_for(m, nl_root(b, m->top->domains[i].clock));
    for (i = 0; i < m->doms.len; i++) {
        SimDom *d = &m->doms.data[i];
        uint32_t r = nl_root(b, d->clock), g;
        int level = m->sigs.data[d->clock].ir->type->falling ? 0 : 1;
        for (g = 0; g < m->groups.len; g++)
            if (m->groups.data[g].root == r && m->groups.data[g].level == level)
                break;
        if (g == m->groups.len) {
            SimGroup ng;
            memset(&ng, 0, sizeof ng);
            ng.root = r;
            ng.level = level;
            ng.clock = IR_NONE;
            if (nl_is_top_clock(m, r)) {
                SimClock *c;
                ng.clock = nl_clock_for(m, r);
                c = &m->clocks.data[ng.clock];
                if (level == (c->falling ? 0 : 1))
                    c->g_active = g;
                else
                    c->g_inactive = g;
            } else {
                vec_push(m->derived, g);
            }
            vec_push(m->groups, ng);
        }
        d->group = g;
        vec_push(m->groups.data[g].doms, i);
    }
    /* clock copies (for toggling without a settle) */
    for (i = 0; i < nsig; i++) {
        uint32_t r;
        if (m->sigs.data[i].ir->type->kind != IRT_CLOCK || m->sigs.data[i].ir->kind == IRS_MEM)
            continue;
        r = nl_root(b, i);
        if (r == i)
            continue;
        for (j = 0; j < m->clocks.len; j++)
            if (m->clocks.data[j].sig == r)
                vec_push(m->clocks.data[j].copies, i);
    }
    /* top domains */
    for (i = 0; i < m->top->ndomains; i++) {
        SimTopDom td;
        SimClock *c;
        td.fd = i;
        td.clock = nl_clock_for(m, nl_root(b, m->top->domains[i].clock));
        c = &m->clocks.data[td.clock];
        if (!c->hz)
            c->hz = m->top->domains[i].hz;
        vec_push(m->topdoms, td);
    }
    for (i = 0; i < m->clocks.len; i++) {
        SimClock *c = &m->clocks.data[i];
        uint64_t period;
        if (!c->hz)
            c->hz = UINT64_C(100000000);
        period = UINT64_C(1000000000000) / c->hz;
        c->half_ps = period / 2 ? period / 2 : 1;
        c->next_toggle = c->half_ps;
    }
    m->default_clock = IR_NONE;
    if (m->top->default_domain != IR_NONE && m->top->default_domain < m->topdoms.len)
        m->default_clock = m->topdoms.data[m->top->default_domain].clock;
    else if (m->topdoms.len)
        m->default_clock = m->topdoms.data[0].clock;
}

/* -------------------------------------------------------------- state -- */

typedef void (*NlTargetFn)(SimModel *m, uint32_t inst, const IrLvalue *lv, void *user);

static void nl_walk_targets(SimModel *m, uint32_t inst, const IrBlock *b, NlTargetFn fn, void *user)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN: fn(m, inst, &s->lhs, user); break;
        case IRST_IF:
            nl_walk_targets(m, inst, &s->then_b, fn, user);
            nl_walk_targets(m, inst, &s->else_b, fn, user);
            break;
        case IRST_CASE:
            for (j = 0; j < s->narms; j++)
                nl_walk_targets(m, inst, &s->arms[j].body, fn, user);
            break;
        default: break;
        }
    }
}

static void nl_mark_state(SimModel *m, uint32_t inst, const IrLvalue *lv, void *user)
{
    SimSig *s = &m->sigs.data[nl_flat(m, inst, lv->sig)];
    if (!s->is_state && s->mem == IR_NONE) {
        s->is_state = 1;
        s->group = *(const uint32_t *)user;
    }
}

static void nl_classify(SimModel *m)
{
    uint32_t ii, i, k;
    for (ii = 0; ii < m->insts.len; ii++) {
        const SimInst *in = &m->insts.data[ii];
        if (in->mod->is_extern)
            continue;
        for (i = 0; i < in->mod->nprocs; i++) {
            const IrProcess *p = &in->mod->procs[i];
            uint32_t g;
            if (p->kind != IRP_CLOCKED || p->domain >= in->mod->ndomains)
                continue;
            g = m->doms.data[in->dom_base + p->domain].group;
            nl_walk_targets(m, ii, &p->body, nl_mark_state, &g);
            for (k = 0; k < p->nregs; k++) {
                IrLvalue lv;
                memset(&lv, 0, sizeof lv);
                lv.sig = p->regs[k];
                if (lv.sig < in->mod->nsigs)
                    nl_mark_state(m, ii, &lv, &g);
            }
        }
    }
}

static void nl_layout(SimModel *m)
{
    uint32_t cur = 0, g, i;
    for (g = 0; g < m->groups.len; g++) {
        SimGroup *gr = &m->groups.data[g];
        gr->cur_lo = cur;
        for (i = 0; i < m->sigs.len; i++) {
            SimSig *s = &m->sigs.data[i];
            if (s->is_state && s->group == g) {
                s->off = cur;
                cur += s->limbs;
            }
        }
        gr->nlimbs = cur - gr->cur_lo;
    }
    for (i = 0; i < m->sigs.len; i++) {
        SimSig *s = &m->sigs.data[i];
        if (!s->is_state && s->mem == IR_NONE) {
            s->off = cur;
            cur += s->limbs;
        }
    }
    for (g = 0; g < m->groups.len; g++) {
        SimGroup *gr = &m->groups.data[g];
        gr->next_lo = cur;
        cur += gr->nlimbs;
    }
    for (i = 0; i < m->sigs.len; i++) {
        SimSig *s = &m->sigs.data[i];
        if (s->is_state)
            s->next = m->groups.data[s->group].next_lo + (s->off - m->groups.data[s->group].cur_lo);
    }
    if (cur >= SIM_TAG_MISC)
        tern_fatal("simulation model too large");
    m->sig_limbs = cur;
}

/* --------------------------------------------------- topological sort -- */

static void nl_range(NlBuild *b, int write, uint32_t node, uint32_t sig, uint32_t lo, uint32_t w)
{
    NlRange r;
    r.node = node;
    r.sig = sig;
    r.lo = lo;
    r.w = w;
    if (write)
        vec_push(b->writes, r);
    else
        vec_push(b->reads, r);
}

static void nl_reads_expr(SimModel *m, NlBuild *b, uint32_t node, uint32_t inst, const IrExpr *e)
{
    uint32_t i;
    if (!e)
        return;
    switch (e->kind) {
    case IRE_CONST: return;
    case IRE_SIG: {
        uint32_t s = nl_flat(m, inst, e->sig);
        if (!m->sigs.data[s].is_state)
            nl_range(b, 0, node, s, 0, e->width);
        return;
    }
    case IRE_SLICE:
        if (e->args[0]->kind == IRE_SIG) {
            uint32_t s = nl_flat(m, inst, e->args[0]->sig);
            if (!m->sigs.data[s].is_state)
                nl_range(b, 0, node, s, e->lo, e->width);
            return;
        }
        break;
    case IRE_MEMREAD: nl_reads_expr(m, b, node, inst, e->args[0]); return;
    default: break;
    }
    for (i = 0; i < e->nargs; i++)
        nl_reads_expr(m, b, node, inst, e->args[i]);
}

static void nl_rw_block(SimModel *m, NlBuild *b, uint32_t node, uint32_t inst, const IrBlock *blk)
{
    uint32_t i, j;
    for (i = 0; i < blk->n; i++) {
        const IrStmt *s = blk->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN: {
            uint32_t f = nl_flat(m, inst, s->lhs.sig);
            nl_reads_expr(m, b, node, inst, s->rhs);
            nl_reads_expr(m, b, node, inst, s->lhs.index);
            if (s->lhs.index)
                nl_range(b, 1, node, f, 0, m->sigs.data[f].width);
            else
                nl_range(b, 1, node, f, s->lhs.lo, s->lhs.width);
            break;
        }
        case IRST_IF:
            nl_reads_expr(m, b, node, inst, s->cond);
            nl_rw_block(m, b, node, inst, &s->then_b);
            nl_rw_block(m, b, node, inst, &s->else_b);
            break;
        case IRST_CASE:
            nl_reads_expr(m, b, node, inst, s->subject);
            for (j = 0; j < s->narms; j++)
                nl_rw_block(m, b, node, inst, &s->arms[j].body);
            break;
        case IRST_MEMWRITE:
            nl_reads_expr(m, b, node, inst, s->addr);
            nl_reads_expr(m, b, node, inst, s->data);
            nl_reads_expr(m, b, node, inst, s->mask);
            break;
        case IRST_ASSERT: nl_reads_expr(m, b, node, inst, s->cond); break;
        }
    }
}

static void nl_report_loop(SimModel *m, NlBuild *b, const uint32_t *cyc_nodes, const uint32_t *cyc_sigs,
                           uint32_t n)
{
    StrBuf sb = {0};
    uint32_t i;
    const NlNode *first;
    if (n == 0)
        return;
    first = &b->nodes.data[cyc_nodes[0]];
    for (i = 0; i < n; i++) {
        sb_puts(&sb, m->sigs.data[cyc_sigs[i]].path);
        sb_puts(&sb, " -> ");
    }
    sb_puts(&sb, m->sigs.data[cyc_sigs[0]].path);
    diag_error(first->loc, "combinational loop: %s", sb_cstr(&sb));
    for (i = 1; i < n; i++) {
        const NlNode *nd = &b->nodes.data[cyc_nodes[i]];
        if (nd->loc.file)
            diag_note(nd->loc, "`%s` is driven here", m->sigs.data[cyc_sigs[i]].path);
    }
    diag_help("break the loop with a register, or compute the value without reading it back");
    sb_free(&sb);
}

/* Split processes: a comb process that writes several signals is one node,
 * so `a = x; c = b` with `b` computed from `a` elsewhere looks like a loop
 * at process granularity although no signal depends on itself. Such
 * processes (those left over by a failed sort) become one node per target;
 * the node of target T reads what T's assignments read (right-hand sides,
 * indices, enclosing conditions), following the block's other targets that
 * those read. Every node of a split process runs the whole body; the
 * values of T are right from T's node on (see nl_compile). */

typedef struct NlSlice {
    SimModel *m;
    uint32_t inst;
    Map *rel;                 /* flat targets the node computes */
    VEC(const IrExpr *) conds;
    NlBuild tmp;              /* reads collected (node field unused) */
} NlSlice;

static void nl_slice_block(NlSlice *s, const IrBlock *blk)
{
    uint32_t i, j, k;
    for (i = 0; i < blk->n; i++) {
        const IrStmt *st = blk->stmts[i];
        switch (st->kind) {
        case IRST_ASSIGN:
            if (map_has(s->rel, nl_flat(s->m, s->inst, st->lhs.sig))) {
                nl_reads_expr(s->m, &s->tmp, 0, s->inst, st->rhs);
                nl_reads_expr(s->m, &s->tmp, 0, s->inst, st->lhs.index);
                for (k = 0; k < s->conds.len; k++)
                    nl_reads_expr(s->m, &s->tmp, 0, s->inst, s->conds.data[k]);
            }
            break;
        case IRST_IF:
            vec_push(s->conds, st->cond);
            nl_slice_block(s, &st->then_b);
            nl_slice_block(s, &st->else_b);
            s->conds.len--;
            break;
        case IRST_CASE:
            vec_push(s->conds, st->subject);
            for (j = 0; j < st->narms; j++)
                nl_slice_block(s, &st->arms[j].body);
            s->conds.len--;
            break;
        default: break;
        }
    }
}

static void nl_collect_target(SimModel *m, uint32_t inst, const IrLvalue *lv, void *user)
{
    uint32_t f = nl_flat(m, inst, lv->sig);
    Map *own = (Map *)user;
    if (!map_has(own, f))
        map_put(own, f, (uint64_t)map_len(own));
}

static void nl_rw_split(SimModel *m, NlBuild *b, uint32_t node, const NlNode *n)
{
    NlSlice s;
    Map own = {0}, rel = {0};
    uint32_t target = n->split - 1, i;
    int changed = 1;
    memset(&s, 0, sizeof s);
    s.m = m;
    s.inst = n->lhs_inst;
    s.rel = &rel;
    nl_walk_targets(m, n->lhs_inst, &n->proc->body, nl_collect_target, &own);
    map_put(&rel, target, 1);
    while (changed) {
        changed = 0;
        s.tmp.reads.len = 0;
        nl_slice_block(&s, &n->proc->body);
        for (i = 0; i < s.tmp.reads.len; i++) {
            uint32_t sig = s.tmp.reads.data[i].sig;
            if (map_has(&own, sig) && !map_has(&rel, sig)) {
                map_put(&rel, sig, 1);
                changed = 1;
            }
        }
    }
    for (i = 0; i < s.tmp.reads.len; i++) {
        NlRange r = s.tmp.reads.data[i];
        if (!map_has(&own, r.sig))
            nl_range(b, 0, node, r.sig, r.lo, r.w);
    }
    nl_range(b, 1, node, target, 0, m->sigs.data[target].width);
    vec_free(s.conds);
    vec_free(s.tmp.reads);
    vec_free(s.tmp.writes);
    map_free(&own);
    map_free(&rel);
}

/* replaces the unsorted multi-target processes by one node per target;
 * returns how many were split */
static uint32_t nl_split_procs(SimModel *m, NlBuild *b, const unsigned char *stuck)
{
    VEC(NlNode) nodes = {0};
    uint32_t i, nsplit = 0;
    for (i = 0; i < b->nodes.len; i++) {
        const NlNode *n = &b->nodes.data[i];
        Map own = {0};
        if (stuck[i] && n->is_proc && !n->split) {
            nl_walk_targets(m, n->lhs_inst, &n->proc->body, nl_collect_target, &own);
            if (map_len(&own) >= 2) {
                size_t it = 0;
                uint64_t k, v;
                while (map_next(&own, &it, &k, &v)) {
                    NlNode c = *n;
                    c.split = (uint32_t)k + 1;
                    vec_push(nodes, c);
                }
                nsplit++;
                map_free(&own);
                continue;
            }
            map_free(&own);
        }
        vec_push(nodes, *n);
    }
    vec_free(b->nodes);
    b->nodes.data = nodes.data;
    b->nodes.len = nodes.len;
    b->nodes.cap = nodes.cap;
    return nsplit;
}

static int nl_order_try(SimModel *m, NlBuild *b, int report, unsigned char **stuck_out);

static int nl_order(SimModel *m, NlBuild *b)
{
    unsigned char *stuck = NULL;
    if (nl_order_try(m, b, 0, &stuck))
        return 1;
    /* split what can be split; the retry sorts again and reports a loop
     * that remains (a true one, at signal granularity) */
    nl_split_procs(m, b, stuck);
    free(stuck);
    return nl_order_try(m, b, 1, NULL);
}

static int nl_order_try(SimModel *m, NlBuild *b, int report, unsigned char **stuck_out)
{
    uint32_t nn = (uint32_t)b->nodes.len, nsig = (uint32_t)m->sigs.len, i, k;
    uint32_t *wstart, *wlist, *indeg, *estart, *queue, qh = 0, qt = 0, nord = 0;
    VEC(NlEdge) edges = {0};
    NlEdge *sorted;
    int ok = 1;

    b->reads.len = 0;
    b->writes.len = 0;
    free(b->order);
    b->order = NULL;
    for (i = 0; i < nn; i++) {
        const NlNode *n = &b->nodes.data[i];
        if (n->is_proc && n->split) {
            nl_rw_split(m, b, i, n);
        } else if (n->is_proc) {
            nl_rw_block(m, b, i, n->lhs_inst, &n->proc->body);
        } else {
            uint32_t f = nl_flat(m, n->lhs_inst, n->lhs.sig);
            nl_reads_expr(m, b, i, n->rhs_inst, n->rhs);
            nl_reads_expr(m, b, i, n->lhs_inst, n->lhs.index);
            if (n->lhs.index)
                nl_range(b, 1, i, f, 0, m->sigs.data[f].width);
            else
                nl_range(b, 1, i, f, n->lhs.lo, n->lhs.width);
        }
    }
    /* writers bucketed by signal */
    wstart = (uint32_t *)xcalloc(nsig + 1, sizeof(uint32_t));
    wlist = (uint32_t *)xmalloc(sizeof(uint32_t) * (b->writes.len ? b->writes.len : 1));
    for (i = 0; i < b->writes.len; i++)
        wstart[b->writes.data[i].sig + 1]++;
    for (i = 0; i < nsig; i++)
        wstart[i + 1] += wstart[i];
    {
        uint32_t *fill = (uint32_t *)xmalloc(sizeof(uint32_t) * (nsig ? nsig : 1));
        memcpy(fill, wstart, sizeof(uint32_t) * nsig);
        for (i = 0; i < b->writes.len; i++)
            wlist[fill[b->writes.data[i].sig]++] = i;
        free(fill);
    }
    for (i = 0; i < b->reads.len; i++) {
        const NlRange *r = &b->reads.data[i];
        for (k = wstart[r->sig]; k < wstart[r->sig + 1]; k++) {
            const NlRange *w = &b->writes.data[wlist[k]];
            NlEdge e;
            if (!(r->lo < w->lo + w->w && w->lo < r->lo + r->w))
                continue;
            if (w->node == r->node && b->nodes.data[r->node].is_proc)
                continue;
            e.from = w->node;
            e.to = r->node;
            e.sig = r->sig;
            vec_push(edges, e);
        }
    }
    /* CSR by source */
    estart = (uint32_t *)xcalloc(nn + 1, sizeof(uint32_t));
    indeg = (uint32_t *)xcalloc(nn ? nn : 1, sizeof(uint32_t));
    sorted = (NlEdge *)xmalloc(sizeof(NlEdge) * (edges.len ? edges.len : 1));
    for (i = 0; i < edges.len; i++) {
        estart[edges.data[i].from + 1]++;
        indeg[edges.data[i].to]++;
    }
    for (i = 0; i < nn; i++)
        estart[i + 1] += estart[i];
    {
        uint32_t *fill = (uint32_t *)xmalloc(sizeof(uint32_t) * (nn ? nn : 1));
        memcpy(fill, estart, sizeof(uint32_t) * nn);
        for (i = 0; i < edges.len; i++)
            sorted[fill[edges.data[i].from]++] = edges.data[i];
        free(fill);
    }
    queue = (uint32_t *)xmalloc(sizeof(uint32_t) * (nn ? nn : 1));
    b->order = (uint32_t *)xmalloc(sizeof(uint32_t) * (nn ? nn : 1));
    for (i = 0; i < nn; i++)
        if (!indeg[i])
            queue[qt++] = i;
    while (qh < qt) {
        uint32_t n = queue[qh++];
        b->order[nord++] = n;
        for (k = estart[n]; k < estart[n + 1]; k++)
            if (--indeg[sorted[k].to] == 0)
                queue[qt++] = sorted[k].to;
    }
    if (nord < nn && !report) {
        /* the caller may split the processes among the remaining nodes */
        ok = 0;
        if (stuck_out) {
            *stuck_out = (unsigned char *)xcalloc(nn ? nn : 1, 1);
            for (i = 0; i < nn; i++)
                (*stuck_out)[i] = indeg[i] != 0;
        }
    } else if (nord < nn) {
        /* find one cycle among the remaining nodes (iterative DFS) */
        unsigned char *color = (unsigned char *)xcalloc(nn, 1);
        uint32_t *stk = (uint32_t *)xmalloc(sizeof(uint32_t) * nn);
        uint32_t *it = (uint32_t *)xmalloc(sizeof(uint32_t) * nn);
        uint32_t *insig = (uint32_t *)xmalloc(sizeof(uint32_t) * nn);
        uint32_t sp = 0, start, found = 0;
        ok = 0;
        for (start = 0; start < nn && !found; start++) {
            if (!indeg[start] || color[start])
                continue;
            stk[0] = start;
            it[0] = estart[start];
            insig[0] = IR_NONE;
            color[start] = 1;
            sp = 1;
            while (sp && !found) {
                uint32_t n = stk[sp - 1];
                if (it[sp - 1] < estart[n + 1]) {
                    const NlEdge *e = &sorted[it[sp - 1]++];
                    if (!indeg[e->to])
                        continue;
                    if (color[e->to] == 1) {
                        uint32_t pos = sp, cnt;
                        uint32_t *cn, *cs;
                        while (pos > 0 && stk[pos - 1] != e->to)
                            pos--;
                        pos--;
                        cnt = sp - pos;
                        cn = (uint32_t *)xmalloc(sizeof(uint32_t) * cnt);
                        cs = (uint32_t *)xmalloc(sizeof(uint32_t) * cnt);
                        for (k = 0; k < cnt; k++) {
                            cn[k] = stk[pos + k];
                            /* signal flowing out of node k into node k+1 */
                            cs[k] = (k + 1 < cnt) ? insig[pos + k + 1] : e->sig;
                        }
                        nl_report_loop(m, b, cn, cs, cnt);
                        free(cn);
                        free(cs);
                        found = 1;
                    } else if (color[e->to] == 0) {
                        color[e->to] = 1;
                        stk[sp] = e->to;
                        it[sp] = estart[e->to];
                        insig[sp] = e->sig;
                        sp++;
                    }
                } else {
                    color[n] = 2;
                    sp--;
                }
            }
        }
        if (!found)
            diag_error(span_none(), "combinational loop in the design");
        free(color);
        free(stk);
        free(it);
        free(insig);
    }
    free(wstart);
    free(wlist);
    free(indeg);
    free(estart);
    free(sorted);
    free(queue);
    vec_free(edges);
    return ok;
}

/* ------------------------------------------------------------ compile -- */

typedef struct NlComp {
    SimModel *m;
    uint32_t inst;
    int clocked;
    uint32_t fd;
    uint32_t tmp, tmp_max;
    uint32_t misc_n;
    uint32_t max_w;
    VEC(uint64_t) consts;
    Map const_map;
    Map cov_map;              /* IrBlock* / IrStmt* -> counter + 1 */
    int quiet;                /* no coverage counts or assertions (early runs of a split process) */
} NlComp;

static uint64_t nl_mask(uint32_t w)
{
    return w >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << w) - 1u);
}

static uint32_t nl_pc(NlComp *c)
{
    return (uint32_t)c->m->code.len;
}

static SimInsn *nl_emit(NlComp *c, SimOp op, uint32_t w)
{
    SimInsn in;
    memset(&in, 0, sizeof in);
    in.op = (uint8_t)op;
    in.w = w;
    in.mask = nl_mask(w);
    vec_push(c->m->code, in);
    if (w > c->max_w)
        c->max_w = w;
    return &vec_last(c->m->code);
}

static uint32_t nl_tmp(NlComp *c, uint32_t w)
{
    uint32_t off = c->tmp;
    c->tmp += NL_LIMBS(w);
    if (c->tmp > c->tmp_max)
        c->tmp_max = c->tmp;
    return SIM_TAG_TEMP | off;
}

static uint32_t nl_misc(NlComp *c, uint32_t limbs)
{
    uint32_t off = c->misc_n;
    c->misc_n += limbs;
    return SIM_TAG_MISC | off;
}

static uint32_t nl_const(NlComp *c, const uint64_t *w, uint32_t width)
{
    uint32_t n = NL_LIMBS(width), i, off;
    uint64_t h = hash_combine(hash_bytes(w, sizeof(uint64_t) * n), width), got;
    if (map_get(&c->const_map, h, &got)) {
        off = (uint32_t)got;
        if (off + n <= c->consts.len && memcmp(&c->consts.data[off], w, sizeof(uint64_t) * n) == 0)
            return SIM_TAG_CONST | off;
    }
    off = (uint32_t)c->consts.len;
    for (i = 0; i < n; i++)
        vec_push(c->consts, w[i]);
    map_put(&c->const_map, h, off);
    return SIM_TAG_CONST | off;
}

static uint32_t nl_dst(NlComp *c, uint32_t w, uint32_t hint)
{
    if (hint != IR_NONE && w <= 64)
        return hint;
    return nl_tmp(c, w);
}

static void nl_copy(NlComp *c, uint32_t d, uint32_t a, uint32_t w)
{
    SimInsn *in = nl_emit(c, w <= 64 ? SOP_COPY : SOP_WCOPY, w);
    in->d = d;
    in->a = a;
}

static uint32_t nl_expr(NlComp *c, const IrExpr *e, uint32_t hint);

static uint32_t nl_unary(NlComp *c, const IrExpr *e, uint32_t hint)
{
    uint32_t a = nl_expr(c, e->args[0], IR_NONE), aw = e->args[0]->width, w = e->width, d;
    SimInsn *in;
    int narrow = w <= 64 && aw <= 64;
    if (narrow) {
        static const SimOp ops[] = {SOP_NOT, SOP_NEG, SOP_LNOT, SOP_RAND, SOP_ROR, SOP_RXOR};
        d = nl_dst(c, w, hint);
        in = nl_emit(c, ops[e->op], w);
    } else {
        d = nl_dst(c, w, w <= 64 ? hint : IR_NONE);
        in = nl_emit(c, SOP_WUN, w);
        in->sub = (uint8_t)e->op;
    }
    in->d = d;
    in->a = a;
    in->aw = aw;
    if (e->op == IROP_RAND && narrow)
        in->mask = nl_mask(aw);
    return d;
}

static uint32_t nl_binary(NlComp *c, const IrExpr *e, uint32_t hint)
{
    const IrExpr *x = e->args[0], *y = e->args[1];
    IrOp op = e->op;
    uint32_t a, b, d, w = e->width, aw = x->width, bw = y->width;
    SimInsn *in;
    int sgn = e->is_signed ? 1 : 0;
    if (op == IROP_GT || op == IROP_GE) {   /* swap into LT/LE */
        const IrExpr *t = x;
        x = y;
        y = t;
        op = op == IROP_GT ? IROP_LT : IROP_LE;
    }
    a = nl_expr(c, x, IR_NONE);
    b = nl_expr(c, y, IR_NONE);
    if (w <= 64 && aw <= 64 && bw <= 64) {
        SimOp sop;
        switch (op) {
        case IROP_ADD: sop = SOP_ADD; break;
        case IROP_SUB: sop = SOP_SUB; break;
        case IROP_MUL: sop = SOP_MUL; break;
        case IROP_DIV: sop = sgn ? SOP_DIVS : SOP_DIVU; break;
        case IROP_MOD: sop = sgn ? SOP_MODS : SOP_MODU; break;
        case IROP_AND: sop = SOP_AND; break;
        case IROP_OR: sop = SOP_OR; break;
        case IROP_XOR: sop = SOP_XOR; break;
        case IROP_SHL: sop = SOP_SHL; break;
        case IROP_SHR: sop = SOP_SHR; break;
        case IROP_ASHR: sop = SOP_ASHR; break;
        case IROP_EQ: sop = SOP_EQ; break;
        case IROP_NE: sop = SOP_NE; break;
        case IROP_LT: sop = sgn ? SOP_LTS : SOP_LTU; break;
        case IROP_LE: sop = sgn ? SOP_LES : SOP_LEU; break;
        case IROP_LAND: sop = SOP_LAND; break;
        default: sop = SOP_LOR; break;
        }
        d = nl_dst(c, w, hint);
        in = nl_emit(c, sop, w);
    } else if (op >= IROP_EQ && op <= IROP_GE) {
        d = nl_dst(c, 1, hint);
        in = nl_emit(c, SOP_WCMP, 1);
        in->sub = (uint8_t)op;
    } else {
        d = nl_dst(c, w, w <= 64 ? hint : IR_NONE);
        in = nl_emit(c, SOP_WBIN, w);
        in->sub = (uint8_t)op;
    }
    in->sgn = (uint8_t)sgn;
    in->d = d;
    in->a = a;
    in->b = b;
    in->aw = aw;
    in->bw = bw;
    return d;
}

static uint32_t nl_expr(NlComp *c, const IrExpr *e, uint32_t hint)
{
    SimModel *m = c->m;
    uint32_t w = e->width, a, b, d, i;
    SimInsn *in;
    if (w > c->max_w)
        c->max_w = w;
    switch (e->kind) {
    case IRE_CONST: return nl_const(c, e->bits.w, w);
    case IRE_SIG: {
        const SimSig *s = &m->sigs.data[nl_flat(m, c->inst, e->sig)];
        TERN_ASSERT(s->off != IR_NONE);
        return s->off;
    }
    case IRE_SLICE: {
        uint32_t aw = e->args[0]->width;
        a = nl_expr(c, e->args[0], IR_NONE);
        if (e->lo == 0 && w == aw)
            return a;
        if (w <= 64) {
            d = nl_dst(c, w, hint);
            in = nl_emit(c, aw <= 64 ? SOP_SLICE : SOP_XSLICE, w);
        } else {
            d = nl_tmp(c, w);
            in = nl_emit(c, SOP_WEXTRACT, w);
        }
        in->d = d;
        in->a = a;
        in->aw = aw;
        in->p = e->lo;
        return d;
    }
    case IRE_INDEX: {
        uint32_t aw = e->args[0]->width, bw = e->args[1]->width;
        a = nl_expr(c, e->args[0], IR_NONE);
        b = nl_expr(c, e->args[1], IR_NONE);
        if (w <= 64 && bw <= 64) {
            d = nl_dst(c, w, hint);
            in = nl_emit(c, aw <= 64 ? SOP_INDEX : SOP_XINDEX, w);
        } else {
            d = nl_dst(c, w, w <= 64 ? hint : IR_NONE);
            in = nl_emit(c, SOP_WINDEX, w);
        }
        in->d = d;
        in->a = a;
        in->b = b;
        in->aw = aw;
        in->bw = bw;
        return d;
    }
    case IRE_UNARY: return nl_unary(c, e, hint);
    case IRE_BINARY: return nl_binary(c, e, hint);
    case IRE_CONCAT:
        if (e->nargs == 1)
            return nl_expr(c, e->args[0], hint);
        if (w <= 64) {
            uint32_t acc = nl_expr(c, e->args[0], IR_NONE), accw = e->args[0]->width;
            for (i = 1; i < e->nargs; i++) {
                uint32_t pw = e->args[i]->width;
                uint32_t part = nl_expr(c, e->args[i], IR_NONE);
                d = (i + 1 == e->nargs) ? nl_dst(c, w, hint) : nl_tmp(c, accw + pw);
                in = nl_emit(c, SOP_SHLOR, accw + pw);
                in->d = d;
                in->a = acc;
                in->b = part;
                in->p = pw;
                acc = d;
                accw += pw;
            }
            return acc;
        } else {
            uint32_t pos = w;
            uint32_t *parts = (uint32_t *)xmalloc(sizeof(uint32_t) * e->nargs);
            for (i = 0; i < e->nargs; i++)
                parts[i] = nl_expr(c, e->args[i], IR_NONE);
            d = nl_tmp(c, w);
            in = nl_emit(c, SOP_WZERO, w);
            in->d = d;
            for (i = 0; i < e->nargs; i++) {
                pos -= e->args[i]->width;
                in = nl_emit(c, SOP_WDEPOSIT, w);
                in->d = d;
                in->a = parts[i];
                in->aw = e->args[i]->width;
                in->p = pos;
            }
            free(parts);
            return d;
        }
    case IRE_MUX: {
        uint32_t cc = nl_expr(c, e->args[0], IR_NONE);
        a = nl_expr(c, e->args[1], IR_NONE);
        b = nl_expr(c, e->args[2], IR_NONE);
        d = nl_dst(c, w, hint);
        in = nl_emit(c, w <= 64 ? SOP_MUX : SOP_WMUX, w);
        in->d = d;
        in->a = cc;
        in->b = a;
        in->c = b;
        return d;
    }
    case IRE_RESIZE: {
        uint32_t aw = e->args[0]->width;
        a = nl_expr(c, e->args[0], IR_NONE);
        if (aw == w)
            return a;
        if (w <= 64 && aw <= 64) {
            if (w > aw && !e->is_signed)
                return a; /* zero-extension within one limb is free */
            d = nl_dst(c, w, hint);
            if (w < aw) {
                in = nl_emit(c, SOP_SLICE, w);
            } else {
                in = nl_emit(c, SOP_SEXT, w);
            }
        } else {
            d = nl_dst(c, w, w <= 64 ? hint : IR_NONE);
            in = nl_emit(c, SOP_WRESIZE, w);
            in->sgn = (uint8_t)(e->is_signed ? 1 : 0);
        }
        in->d = d;
        in->a = a;
        in->aw = aw;
        return d;
    }
    case IRE_MEMREAD: {
        const SimSig *s = &m->sigs.data[nl_flat(m, c->inst, e->sig)];
        a = nl_expr(c, e->args[0], IR_NONE);
        d = nl_dst(c, w, hint);
        in = nl_emit(c, SOP_MEMREAD, w);
        in->d = d;
        in->a = a;
        in->aw = e->args[0]->width;
        in->p = s->mem;
        return d;
    }
    case IRE_PAST: {
        uint32_t n = e->lo, lw = NL_LIMBS(w), base, k;
        a = nl_expr(c, e->args[0], IR_NONE);
        if (n == 0)
            return a;
        base = nl_misc(c, (n + 1) * lw);
        for (k = n; k >= 1; k--)
            nl_copy(c, base + k * lw, base + (k - 1) * lw, w);
        nl_copy(c, base, a, w);
        return base + n * lw;
    }
    }
    tern_fatal("sim: unknown expression kind %d", (int)e->kind);
}

/* coverage counter of a block (shared by all instances of a module) */
static uint32_t nl_cov_block(NlComp *c, const IrBlock *b, int kind, Span loc, uint32_t arm)
{
    SimModel *m = c->m;
    uint64_t got;
    uint32_t ctr, i;
    SimCovRaw r;
    if (map_get(&c->cov_map, MAP_PTR(b), &got))
        return (uint32_t)got - 1;
    ctr = m->ncov++;
    map_put(&c->cov_map, MAP_PTR(b), (uint64_t)ctr + 1);
    memset(&r, 0, sizeof r);
    r.counter = ctr;
    r.mon = IR_NONE;
    if (kind >= 0) {
        r.kind = (SimCovKind)kind;
        r.loc = loc;
        r.arm = arm;
        vec_push(m->covraw, r);
    }
    for (i = 0; i < b->n; i++) {
        r.kind = SIM_COV_STMT;
        r.loc = b->stmts[i]->loc;
        r.arm = 0;
        vec_push(m->covraw, r);
    }
    return ctr;
}

/* coverage counter of a continuous assignment (shared by all instances) */
static uint32_t nl_cov_assign(NlComp *c, const IrAssign *a)
{
    SimModel *m = c->m;
    uint64_t got;
    uint32_t ctr;
    SimCovRaw r;
    if (map_get(&c->cov_map, MAP_PTR(a), &got))
        return (uint32_t)got - 1;
    ctr = m->ncov++;
    map_put(&c->cov_map, MAP_PTR(a), (uint64_t)ctr + 1);
    memset(&r, 0, sizeof r);
    r.counter = ctr;
    r.mon = IR_NONE;
    r.kind = SIM_COV_STMT;
    r.loc = a->loc;
    vec_push(m->covraw, r);
    return ctr;
}

static void nl_stmt(NlComp *c, const IrStmt *s);

static void nl_block(NlComp *c, const IrBlock *b, int kind, Span loc, uint32_t arm)
{
    uint32_t i;
    SimInsn *in;
    if (b->n == 0 && kind < 0)
        return;
    if (!c->quiet) {
        in = nl_emit(c, SOP_COV, 0);
        in->p = nl_cov_block(c, b, kind, loc, arm);
    }
    for (i = 0; i < b->n; i++)
        nl_stmt(c, b->stmts[i]);
}

static void nl_assign(NlComp *c, uint32_t lhs_inst, const IrLvalue *lv, uint32_t rhs_inst,
                      const IrExpr *rhs)
{
    SimModel *m = c->m;
    const SimSig *s = &m->sigs.data[nl_flat(m, lhs_inst, lv->sig)];
    uint32_t base = (c->clocked && s->next != IR_NONE) ? s->next : s->off;
    uint32_t sw = s->width, saved = c->tmp, saved_inst = c->inst, r;
    SimInsn *in;
    TERN_ASSERT(base != IR_NONE);
    c->inst = rhs_inst;
    if (!lv->index && lv->lo == 0 && lv->width == sw) {
        r = nl_expr(c, rhs, sw <= 64 ? base : IR_NONE);
        if (r != base)
            nl_copy(c, base, r, sw);
    } else if (!lv->index) {
        r = nl_expr(c, rhs, IR_NONE);
        in = nl_emit(c, sw <= 64 ? SOP_DEPOSIT : SOP_WDEPOSIT, sw);
        in->d = base;
        in->a = r;
        in->aw = lv->width;
        in->p = lv->lo;
        in->mask = nl_mask(lv->width);
    } else {
        uint32_t ix;
        r = nl_expr(c, rhs, IR_NONE);
        c->inst = lhs_inst;
        ix = nl_expr(c, lv->index, IR_NONE);
        in = nl_emit(c, (sw <= 64 && lv->index->width <= 64) ? SOP_DEPOSIT_DYN : SOP_WDEPOSIT_DYN, sw);
        in->d = base;
        in->a = r;
        in->b = ix;
        in->aw = lv->width;
        in->bw = lv->index->width;
        in->p = lv->lo;
        in->mask = nl_mask(lv->width);
        /* elements of the indexed array: an index past the last one writes nothing */
        in->c = lv->len ? lv->len : (lv->lo < sw ? (sw - lv->lo) / lv->width : 0);
    }
    if (s->force != IR_NONE) {
        in = nl_emit(c, SOP_FORCE, sw);
        in->d = base;
        in->p = s->force;
    }
    c->tmp = saved;
    c->inst = saved_inst;
}

typedef struct NlCaseEnt {
    uint64_t val;
    uint32_t arm;
} NlCaseEnt;

static int nl_case_cmp(const void *pa, const void *pb)
{
    const NlCaseEnt *a = (const NlCaseEnt *)pa, *b = (const NlCaseEnt *)pb;
    if (a->val != b->val)
        return a->val < b->val ? -1 : 1;
    return a->arm < b->arm ? -1 : a->arm > b->arm ? 1 : 0;
}

static void nl_case(NlComp *c, const IrStmt *s)
{
    SimModel *m = c->m;
    uint32_t sw = s->subject->width, subj, i, j, saved = c->tmp;
    uint32_t *targets = (uint32_t *)xmalloc(sizeof(uint32_t) * (s->narms ? s->narms : 1));
    VEC(uint32_t) jumps = {0};   /* JMP-to-end instructions */
    uint32_t dflt_arm = IR_NONE, end;
    for (i = 0; i < s->narms; i++)
        if (s->arms[i].nvals == 0 && dflt_arm == IR_NONE)
            dflt_arm = i;
    subj = nl_expr(c, s->subject, IR_NONE);
    if (sw <= 64) {
        uint32_t sidx = (uint32_t)m->switches.len, pc_sw = nl_pc(c);
        SimSwitch swt;
        SimInsn *in = nl_emit(c, SOP_SWITCH, sw);
        size_t nent = 0;
        NlCaseEnt *ents;
        in->a = subj;
        in->p = sidx;
        memset(&swt, 0, sizeof swt);
        vec_push(m->switches, swt);
        c->tmp = saved;
        for (i = 0; i < s->narms; i++) {
            targets[i] = nl_pc(c);
            nl_block(c, &s->arms[i].body, SIM_COV_ARM, s->loc, i);
            vec_push(jumps, nl_pc(c));
            nl_emit(c, SOP_JMP, 0);
            nent += s->arms[i].nvals;
        }
        end = nl_pc(c);
        ents = (NlCaseEnt *)xmalloc(sizeof(NlCaseEnt) * (nent ? nent : 1));
        nent = 0;
        for (i = 0; i < s->narms; i++)
            for (j = 0; j < s->arms[i].nvals; j++) {
                ents[nent].val = ir_bits_low64(&s->arms[i].vals[j]);
                ents[nent].arm = i;
                nent++;
            }
        qsort(ents, nent, sizeof(NlCaseEnt), nl_case_cmp);
        {
            SimSwitch *t = &m->switches.data[sidx];
            size_t k, n = 0;
            t->vals = ANEWN(&m->arena, uint64_t, nent ? nent : 1);
            t->targets = ANEWN(&m->arena, uint32_t, nent ? nent : 1);
            for (k = 0; k < nent; k++) {
                if (n && t->vals[n - 1] == ents[k].val)
                    continue; /* first arm wins */
                t->vals[n] = ents[k].val;
                t->targets[n] = targets[ents[k].arm];
                n++;
            }
            t->n = (uint32_t)n;
            t->dflt = dflt_arm != IR_NONE ? targets[dflt_arm] : end;
        }
        free(ents);
        TERN_UNUSED(pc_sw);
    } else {
        VEC(uint32_t) cmpj = {0};   /* JZ instructions jumping to an arm */
        VEC(uint32_t) cmpa = {0};
        uint32_t dj;
        for (i = 0; i < s->narms; i++)
            for (j = 0; j < s->arms[i].nvals; j++) {
                uint32_t cst = nl_const(c, s->arms[i].vals[j].w, sw), t = nl_tmp(c, 1);
                SimInsn *in = nl_emit(c, SOP_WCMP, 1);
                in->sub = IROP_NE;
                in->d = t;
                in->a = subj;
                in->b = cst;
                in->aw = in->bw = sw;
                vec_push(cmpj, nl_pc(c));
                vec_push(cmpa, i);
                in = nl_emit(c, SOP_JZ, 0);
                in->a = t;
            }
        dj = nl_pc(c);
        nl_emit(c, SOP_JMP, 0);
        c->tmp = saved;
        for (i = 0; i < s->narms; i++) {
            targets[i] = nl_pc(c);
            nl_block(c, &s->arms[i].body, SIM_COV_ARM, s->loc, i);
            vec_push(jumps, nl_pc(c));
            nl_emit(c, SOP_JMP, 0);
        }
        end = nl_pc(c);
        for (i = 0; i < cmpj.len; i++)
            m->code.data[cmpj.data[i]].p = targets[cmpa.data[i]];
        m->code.data[dj].p = dflt_arm != IR_NONE ? targets[dflt_arm] : end;
        vec_free(cmpj);
        vec_free(cmpa);
    }
    for (i = 0; i < jumps.len; i++)
        m->code.data[jumps.data[i]].p = end;
    vec_free(jumps);
    free(targets);
    c->tmp = saved;
}

static void nl_stmt(NlComp *c, const IrStmt *s)
{
    SimModel *m = c->m;
    uint32_t saved = c->tmp;
    SimInsn *in;
    switch (s->kind) {
    case IRST_ASSIGN: nl_assign(c, c->inst, &s->lhs, c->inst, s->rhs); break;
    case IRST_IF: {
        uint32_t cond = nl_expr(c, s->cond, IR_NONE), jz, jmp;
        jz = nl_pc(c);
        in = nl_emit(c, SOP_JZ, 0);
        in->a = cond;
        c->tmp = saved;
        nl_block(c, &s->then_b, SIM_COV_THEN, s->loc, 0);
        jmp = nl_pc(c);
        nl_emit(c, SOP_JMP, 0);
        m->code.data[jz].p = nl_pc(c);
        nl_block(c, &s->else_b, SIM_COV_ELSE, s->loc, 0);
        m->code.data[jmp].p = nl_pc(c);
        break;
    }
    case IRST_CASE: nl_case(c, s); break;
    case IRST_MEMWRITE: {
        const SimSig *ms = &m->sigs.data[nl_flat(m, c->inst, s->mem)];
        uint32_t addr = nl_expr(c, s->addr, IR_NONE);
        uint32_t data = nl_expr(c, s->data, IR_NONE);
        uint32_t mask = s->mask ? nl_expr(c, s->mask, IR_NONE) : 0;
        in = nl_emit(c, SOP_MEMWRITE, s->data->width);
        in->a = addr;
        in->aw = s->addr->width;
        in->b = data;
        in->c = mask;
        in->bw = s->mask ? s->mask->width : 0;
        in->sub = s->mask ? 1 : 0;
        in->p = ms->mem;
        break;
    }
    case IRST_ASSERT: {
        uint32_t cond;
        SimAssert as;
        if (c->quiet)
            break;
        cond = nl_expr(c, s->cond, IR_NONE);
        memset(&as, 0, sizeof as);
        as.kind = s->akind;
        as.msg = s->msg;
        as.loc = s->loc;
        as.inst = c->inst;
        as.fd = c->clocked ? c->fd : IR_NONE;
        as.cov = IR_NONE;
        if (s->akind == IRA_COVER) {
            uint64_t got;
            if (map_get(&c->cov_map, MAP_PTR(s), &got)) {
                as.cov = (uint32_t)got - 1;
            } else {
                SimCovRaw r;
                as.cov = m->ncov++;
                map_put(&c->cov_map, MAP_PTR(s), (uint64_t)as.cov + 1);
                memset(&r, 0, sizeof r);
                r.kind = SIM_COV_COVER;
                r.loc = s->loc;
                r.name = s->msg;
                r.counter = as.cov;
                r.mon = IR_NONE;
                vec_push(m->covraw, r);
            }
        }
        in = nl_emit(c, SOP_ASSERT, 1);
        in->a = cond;
        in->p = (uint32_t)m->asserts.len;
        vec_push(m->asserts, as);
        break;
    }
    }
    c->tmp = saved;
}

/* properties: static formula nodes */
static uint32_t nl_pnode(SimModel *m, uint8_t kind, uint32_t a, uint32_t b, uint32_t lo, uint32_t hi,
                         uint32_t leaf)
{
    SimPNode n;
    memset(&n, 0, sizeof n);
    n.kind = kind;
    n.a = a;
    n.b = b;
    n.lo = lo;
    n.hi = hi;
    n.leaf = leaf;
    vec_push(m->pstatic, n);
    return (uint32_t)m->pstatic.len - 1;
}

static uint32_t nl_seq(NlComp *c, const IrSeq *q)
{
    SimModel *m = c->m;
    uint32_t a, b;
    if (!q)
        return SPK_TRUE;
    switch (q->kind) {
    case IRQ_EXPR: {
        uint32_t leaf = nl_misc(c, 1), r, saved = c->tmp;
        if (q->expr->width == 1) {
            r = nl_expr(c, q->expr, leaf);
            if (r != leaf)
                nl_copy(c, leaf, r, 1);
        } else {
            SimInsn *in;
            r = nl_expr(c, q->expr, IR_NONE);
            in = nl_emit(c, q->expr->width <= 64 ? SOP_ROR : SOP_WUN, 1);
            in->sub = IROP_ROR;
            in->d = leaf;
            in->a = r;
            in->aw = q->expr->width;
        }
        c->tmp = saved;
        return nl_pnode(m, SPK_EXPR, 0, 0, 0, 0, leaf);
    }
    case IRQ_DELAY:
        a = nl_seq(c, q->a);
        b = nl_seq(c, q->b);
        return nl_pnode(m, SPK_CAT, a, b, q->lo, q->hi == IR_NONE ? SIM_PINF : q->hi, 0);
    case IRQ_IMPL:
    case IRQ_IMPL_NEXT:
    case IRQ_UNTIL:
    case IRQ_AND:
    case IRQ_OR: {
        uint8_t k = q->kind == IRQ_IMPL        ? SPK_IMPL
                    : q->kind == IRQ_IMPL_NEXT ? SPK_IMPLN
                    : q->kind == IRQ_UNTIL     ? SPK_UNTIL
                    : q->kind == IRQ_AND       ? SPK_AND
                                               : SPK_OR;
        a = nl_seq(c, q->a);
        b = nl_seq(c, q->b);
        return nl_pnode(m, k, a, b, 0, 0, 0);
    }
    case IRQ_ALWAYS: return nl_pnode(m, SPK_ALWAYS, nl_seq(c, q->a), 0, 0, 0, 0);
    case IRQ_NEVER: return nl_pnode(m, SPK_NEVER, nl_seq(c, q->a), 0, 0, 0, 0);
    case IRQ_NOT: return nl_pnode(m, SPK_NOT, nl_seq(c, q->a), 0, 0, 0, 0);
    case IRQ_EVENTUALLY:
        return nl_pnode(m, SPK_EV, nl_seq(c, q->a), 0, 0, q->hi == IR_NONE ? SIM_PINF : q->hi, 0);
    }
    return SPK_TRUE;
}

static void nl_init_regs(SimModel *m, NlComp *c, uint32_t inst, const IrLvalue *lv, void *user)
{
    SimSig *s = &m->sigs.data[nl_flat(m, inst, lv->sig)];
    Map *seen = (Map *)user;
    uint32_t h = nl_flat(m, inst, lv->sig);
    SimInitReg r;
    IrBits ib;
    if (!s->is_state || !s->ir->has_init || map_has(seen, h))
        return;
    map_put(seen, h, 1);
    ib = ir_bits_resize(&m->arena, &s->ir->init, s->width, 0);
    r.cur = s->off;
    r.next = s->next;
    r.limbs = s->limbs;
    r.init = nl_const(c, ib.w, s->width);
    vec_push(m->initregs, r);
}

typedef struct NlInitCtx {
    NlComp *c;
    Map seen;
} NlInitCtx;

static void nl_init_cb(SimModel *m, uint32_t inst, const IrLvalue *lv, void *user)
{
    NlInitCtx *ic = (NlInitCtx *)user;
    nl_init_regs(m, ic->c, inst, lv, &ic->seen);
}

static void nl_compile(SimModel *m, NlBuild *b, NlComp *c)
{
    uint32_t i, j, g, k;
    /* combinational segment */
    c->clocked = 0;
    c->fd = IR_NONE;
    for (i = 0; i < b->nodes.len; i++) {
        const NlNode *n = &b->nodes.data[b->order[i]];
        if (n->is_proc && n->split) {
            /* a split process runs once per target node (consecutive runs
             * once); only its last run counts coverage and checks assertions */
            uint32_t j2, later = 0;
            const NlNode *nx = i + 1 < b->nodes.len ? &b->nodes.data[b->order[i + 1]] : NULL;
            if (nx && nx->split && nx->proc == n->proc && nx->lhs_inst == n->lhs_inst)
                continue;
            for (j2 = i + 1; j2 < b->nodes.len && !later; j2++) {
                const NlNode *o = &b->nodes.data[b->order[j2]];
                later = o->split && o->proc == n->proc && o->lhs_inst == n->lhs_inst;
            }
            c->inst = n->lhs_inst;
            c->quiet = later;
            nl_block(c, &n->proc->body, -1, n->loc, 0);
            c->quiet = 0;
        } else if (n->is_proc) {
            c->inst = n->lhs_inst;
            nl_block(c, &n->proc->body, -1, n->loc, 0);
        } else {
            c->inst = n->rhs_inst;
            if (n->asg && n->asg->loc.file) {
                /* a continuous assignment (`let`, `x = e`) is a line that
                 * runs whenever its module is simulated */
                SimInsn *ci = nl_emit(c, SOP_COV, 0);
                ci->p = nl_cov_assign(c, n->asg);
            }
            nl_assign(c, n->lhs_inst, &n->lhs, n->rhs_inst, n->rhs);
        }
    }
    m->comb_end = nl_pc(c);
    /* clocked segments, one per flattened domain */
    for (i = 0; i < m->doms.len; i++) {
        SimDom *d = &m->doms.data[i];
        const SimInst *in = &m->insts.data[d->inst];
        uint32_t local = i - in->dom_base;
        NlInitCtx ic;
        memset(&ic, 0, sizeof ic);
        ic.c = c;
        d->code_lo = nl_pc(c);
        d->init_lo = (uint32_t)m->initregs.len;
        c->clocked = 1;
        c->fd = i;
        c->inst = d->inst;
        for (j = 0; j < in->mod->nprocs; j++) {
            const IrProcess *p = &in->mod->procs[j];
            if (p->kind != IRP_CLOCKED || p->domain != local)
                continue;
            nl_block(c, &p->body, -1, p->loc, 0);
            nl_walk_targets(m, d->inst, &p->body, nl_init_cb, &ic);
            for (k = 0; k < p->nregs; k++) {
                IrLvalue lv;
                memset(&lv, 0, sizeof lv);
                lv.sig = p->regs[k];
                if (lv.sig < in->mod->nsigs)
                    nl_init_cb(m, d->inst, &lv, &ic);
            }
        }
        d->code_hi = nl_pc(c);
        d->init_n = (uint32_t)m->initregs.len - d->init_lo;
        map_free(&ic.seen);
    }
    c->clocked = 0;
    c->fd = IR_NONE;
    /* property monitors */
    for (i = 0; i < m->insts.len; i++) {
        const SimInst *in = &m->insts.data[i];
        for (j = 0; j < in->mod->nprops; j++) {
            const IrProperty *p = &in->mod->props[j];
            uint32_t dom = p->domain != IR_NONE ? p->domain : in->mod->default_domain;
            SimMon mon;
            if (dom == IR_NONE || dom >= in->mod->ndomains || !p->seq) {
                diag_warning(p->loc, "property `%s` has no clock domain; it is not simulated",
                             p->name ? p->name : "_");
                continue;
            }
            memset(&mon, 0, sizeof mon);
            mon.inst = i;
            mon.prop = p;
            mon.fd = in->dom_base + dom;
            mon.group = m->doms.data[mon.fd].group;
            vec_push(m->mons, mon);
        }
    }
    for (g = 0; g < m->groups.len; g++) {
        SimGroup *gr = &m->groups.data[g];
        gr->pcode_lo = nl_pc(c);
        for (i = 0; i < m->mons.len; i++) {
            SimMon *mon = &m->mons.data[i];
            if (mon->group != g)
                continue;
            c->inst = mon->inst;
            mon->misc_lo = c->misc_n;
            mon->root = nl_seq(c, mon->prop->seq);
            mon->misc_hi = c->misc_n;
            vec_push(gr->mons, i);
            if (mon->prop->kind == IRA_COVER) {
                SimCovRaw r;
                memset(&r, 0, sizeof r);
                r.kind = SIM_COV_PROPERTY;
                r.loc = mon->prop->loc;
                r.name = mon->prop->name;
                r.counter = IR_NONE;
                r.mon = i;
                vec_push(m->covraw, r);
            }
        }
        gr->pcode_hi = nl_pc(c);
    }
}

static uint32_t nl_reloc(const SimModel *m, uint32_t x)
{
    switch (x & SIM_TAG_MASK) {
    case SIM_TAG_CONST: return m->const_base + (x & ~SIM_TAG_MASK);
    case SIM_TAG_TEMP: return m->temp_base + (x & ~SIM_TAG_MASK);
    case SIM_TAG_MISC: return m->misc_base + (x & ~SIM_TAG_MASK);
    default: return x;
    }
}

static uint64_t nl_splitmix(uint64_t *s)
{
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static void nl_random(SimModel *m, const char *path, uint64_t *dst, uint32_t width, uint64_t salt)
{
    uint64_t st = m->seed ^ hash_bytes(path, strlen(path)) ^ hash_u64(salt);
    uint32_t i, n = NL_LIMBS(width);
    for (i = 0; i < n; i++)
        dst[i] = nl_splitmix(&st);
    if (width % 64)
        dst[n - 1] &= (UINT64_C(1) << (width % 64)) - 1u;
}

static void nl_init_values(SimModel *m, NlComp *c)
{
    uint32_t i, k;
    if (c->consts.len)
        memcpy(m->v + m->const_base, c->consts.data, sizeof(uint64_t) * c->consts.len);
    for (i = 0; i < m->sigs.len; i++) {
        SimSig *s = &m->sigs.data[i];
        const IrSignal *ir = s->ir;
        uint64_t *dst;
        if (s->mem != IR_NONE)
            continue;
        dst = m->v + s->off;
        if (s->inst == 0 && ir->kind == IRS_IN) {
            if (ir->type->kind == IRT_CLOCK)
                dst[0] = ir->type->falling ? 1u : 0u;
            else if (ir->type->kind == IRT_RESET)
                dst[0] = ir->type->active_low ? 1u : 0u;
        } else if (ir->kind == IRS_REG || s->is_state) {
            if (ir->has_init)
                bv_resize(dst, s->width, ir->init.w, ir->init.width, 0);
            else
                nl_random(m, s->path, dst, s->width, 0);
        }
    }
    for (i = 0; i < m->mems.len; i++) {
        SimMem *mm = &m->mems.data[i];
        const SimSig *s = &m->sigs.data[mm->sig];
        mm->data = (uint64_t *)xcalloc((size_t)mm->depth * mm->limbs + 1, sizeof(uint64_t));
        for (k = 0; k < mm->depth; k++) {
            uint64_t *dst = mm->data + (size_t)k * mm->limbs;
            if (s->ir->mem_init)
                bv_resize(dst, mm->width, s->ir->mem_init[k].w, s->ir->mem_init[k].width, 0);
            else
                nl_random(m, s->path, dst, mm->width, (uint64_t)k + 1);
        }
    }
}

int sim_build(SimModel *m)
{
    NlBuild b;
    NlComp c;
    uint32_t i, maxl;
    memset(&b, 0, sizeof b);
    memset(&c, 0, sizeof c);
    c.m = m;
    /* static property constants */
    nl_pnode(m, SPK_TRUE, 0, 0, 0, 0, 0);
    nl_pnode(m, SPK_FALSE, 0, 0, 0, 0, 0);
    nl_pnode(m, SPK_VAC, 0, 0, 0, 0, 0);

    nl_flatten(m, m->top, IR_NONE, "", m->top->loc);
    nl_collect_nodes(m, &b);
    nl_groups(m, &b);
    nl_classify(m);
    nl_layout(m);
    if (!nl_order(m, &b)) {
        free(b.alias);
        free(b.order);
        vec_free(b.nodes);
        vec_free(b.reads);
        vec_free(b.writes);
        return 0;
    }
    nl_compile(m, &b, &c);

    m->const_base = m->sig_limbs;
    m->temp_base = m->const_base + (uint32_t)c.consts.len;
    m->misc_base = m->temp_base + c.tmp_max;
    m->nv = (size_t)m->misc_base + c.misc_n + 1;
    m->v = (uint64_t *)xcalloc(m->nv, sizeof(uint64_t));
    for (i = 0; i < m->code.len; i++) {
        SimInsn *in = &m->code.data[i];
        in->d = nl_reloc(m, in->d);
        in->a = nl_reloc(m, in->a);
        in->b = nl_reloc(m, in->b);
        in->c = nl_reloc(m, in->c);
    }
    for (i = 0; i < m->pstatic.len; i++)
        m->pstatic.data[i].leaf = nl_reloc(m, m->pstatic.data[i].leaf);
    for (i = 0; i < m->initregs.len; i++)
        m->initregs.data[i].init = nl_reloc(m, m->initregs.data[i].init);
    maxl = NL_LIMBS(c.max_w) + 1;
    for (i = 0; i < m->sigs.len; i++)
        if (m->sigs.data[i].limbs + 1 > maxl)
            maxl = m->sigs.data[i].limbs + 1;
    m->scratch = (uint64_t *)xcalloc((size_t)maxl * 4, sizeof(uint64_t));
    m->cov = (uint64_t *)xcalloc(m->ncov + 1, sizeof(uint64_t));
    nl_init_values(m, &c);

    free(b.alias);
    free(b.order);
    vec_free(b.nodes);
    vec_free(b.reads);
    vec_free(b.writes);
    vec_free(c.consts);
    map_free(&c.const_map);
    map_free(&c.cov_map);
    return 1;
}

#undef NL_LIMBS
