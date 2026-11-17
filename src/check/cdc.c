/* cdc.c — clock-domain checks and the CDC report (spec §5.6, §8.2, §8.3).
 *
 * Domains: two IrDomains with the same clock (through `let` aliases) are
 * the same clock domain. Registers, ports and instance wires carry their
 * domain; wires, vars and comb-instance outputs get the domain of what
 * they read, inferred in topological order over the combinational graph
 * of the module model; a memory has the domain of the `on` block writing
 * it. Clocks are domain-free here (using one as data is an elaboration
 * error).
 *
 * Errors, outside std::cdc primitives (#[cdc] modules, whose job is to
 * cross):
 *   - an `on` block reading a signal of another domain;
 *   - logic driving a signal of a known domain (an output `@ dom`, the
 *     input of an instance) from another domain;
 *   - logic mixing two domains: "pix-domain signal `line` used in
 *     sys-domain logic; cross with std::cdc".
 * Values wrapped in unsafe_cross(x) are domain-free (the elaborator warns)
 * and listed in the report. A domain whose reset comes from another clock
 * domain (also an instance reset port fed that way) is a warning: the
 * release of the reset is not synchronous to the clock; use
 * std::cdc::ResetSync / ResetBridge.
 *
 * The CDC report (IrDesign.crossings, printed by `tern check`): every
 * instance of a #[cdc] module and every unsafe_cross, along every
 * instance path from the roots of the design ("Top > s: sync2, sys -> pix,
 * signal flag"). */
#include "check_impl.h"

typedef struct CdcLocal {
    uint32_t inst;        /* instance index, IR_NONE for unsafe_cross */
    IrCdcKind kind;
    const char *from, *to, *signal;
    Span loc;
} CdcLocal;

typedef VEC(CdcLocal) CdcLocalVec;

typedef struct ChkCdcMod {
    uint32_t n;
    CdcLocal *items;
} ChkCdcMod;

typedef struct CdcCtx {
    ChkMod *cm;
    IrModule *m;
    uint32_t *canon;      /* per domain: its key (the clock signal behind aliases) */
    uint32_t *sigkey;     /* per signal: the key of its declared domain, or IR_NONE */
    uint32_t *nodedom;    /* per node: inferred domain key or IR_NONE */
    uint32_t *memdom;     /* per signal: memory domain */
    Map reported;         /* hash(sig, domain) of reported crossings */
    Map unsafe_reader;    /* unsafe wire + 1 -> reader node + 1 */
} CdcCtx;

/* -------------------------------------------------------------- domains */

/* the clock signal behind `let` aliases */
static uint32_t cdc_resolve_clock(const ChkMod *cm, uint32_t sig)
{
    int guard = 0;
    while (sig < cm->m->nsigs && cm->m->sigs[sig].kind == IRS_WIRE && guard++ < 64) {
        uint32_t f, e;
        const ChkNode *n;
        const IrAssign *a;
        chk_ranges_of(cm, sig, &f, &e);
        if (e - f != 1)
            break;
        n = &cm->nodes.data[cm->ranges.data[f].node];
        if (n->kind != CHK_N_ASSIGN)
            break;
        a = &cm->m->assigns[n->idx];
        if (!a->rhs || a->rhs->kind != IRE_SIG)
            break;
        sig = a->rhs->sig;
    }
    return sig;
}

/* Domains are identified by a key: the clock signal behind `let`
   aliases, so two IrDomains with the same clock are one clock domain and
   a clock without a `domain` declaration still has one. */
static uint32_t cdc_domain_of_clock(const CdcCtx *c, uint32_t clk)
{
    return cdc_resolve_clock(c->cm, clk);
}

static const char *cdc_domain_name(const CdcCtx *c, uint32_t key)
{
    uint32_t i;
    if (key == IR_NONE || key >= c->m->nsigs)
        return "?";
    for (i = 0; i < c->m->ndomains; i++)
        if (c->canon[i] == key && c->m->domains[i].name && c->m->domains[i].name[0])
            return c->m->domains[i].name;
    return c->m->sigs[key].path;
}

static int cdc_is_clock(const IrSignal *s)
{
    return s->type && s->type->kind == IRT_CLOCK;
}

/* domain of the bits a read covers */
static uint32_t cdc_read_domain(CdcCtx *c, const ChkRead *r)
{
    const IrSignal *s = &c->m->sigs[r->sig];
    uint32_t f, e;
    if (cdc_is_clock(s) || chk_sig_attr(s, "unsafe_cross"))
        return IR_NONE;
    if (c->sigkey[r->sig] != IR_NONE)
        return c->sigkey[r->sig];
    if (s->kind == IRS_MEM)
        return c->memdom[r->sig];
    chk_ranges_from(c->cm, r->sig, r->lo, &f, &e);
    for (; f < e; f++) {
        const ChkRange *rg = &c->cm->ranges.data[f];
        if (rg->lo >= r->hi)
            break;
        if (chk_range_overlaps(rg, r->lo, r->hi) && c->nodedom[rg->node] != IR_NONE)
            return c->nodedom[rg->node];
    }
    return IR_NONE;
}

/* domain of a whole signal (fixed, or inferred from its drivers) */
static uint32_t cdc_sig_domain(CdcCtx *c, uint32_t sig)
{
    ChkRead r;
    memset(&r, 0, sizeof r);
    r.sig = sig;
    r.lo = 0;
    r.hi = c->m->sigs[sig].kind == IRS_MEM ? 1 : c->m->sigs[sig].type->width;
    return cdc_read_domain(c, &r);
}

static void cdc_cross_error(CdcCtx *c, const ChkRead *r, uint32_t d, uint32_t logic)
{
    uint64_t h = hash_combine(hash_u64(r->sig), hash_u64(logic));
    const IrSignal *s = &c->m->sigs[r->sig];
    if (map_has(&c->reported, h))
        return;
    map_put(&c->reported, h, 1);
    if (chk_error(c->cm->cx, r->loc, "%s-domain signal `%s` used in %s-domain logic; cross with std::cdc",
                  cdc_domain_name(c, d), s->path, cdc_domain_name(c, logic)))
        diag_help("use std::cdc::Sync2 for a level, PulseSync for a pulse, HandshakeSync or AsyncFifo for data, or "
                  "unsafe_cross(x) for a quasi-static value");
}

static void cdc_note_unsafe(CdcCtx *c, const ChkRead *r, uint32_t node)
{
    int ins;
    uint64_t *slot;
    if (!chk_sig_attr(&c->m->sigs[r->sig], "unsafe_cross"))
        return;
    slot = map_ref(&c->unsafe_reader, (uint64_t)r->sig + 1, &ins);
    if (ins)
        *slot = (uint64_t)node + 1;
}

/* one node whose logic has domain `logic` (IR_NONE: inferred from its
   reads); returns the node's domain */
static uint32_t cdc_check_node(CdcCtx *c, uint32_t ni, uint32_t logic)
{
    const ChkNode *n = &c->cm->nodes.data[ni];
    uint32_t r;
    for (r = n->rd; r < n->rd + n->nrd; r++) {
        const ChkRead *rd = &c->cm->reads.data[r];
        uint32_t d;
        cdc_note_unsafe(c, rd, ni);
        d = cdc_read_domain(c, rd);
        if (d == IR_NONE)
            continue;
        if (logic == IR_NONE)
            logic = d;
        else if (d != logic)
            cdc_cross_error(c, rd, d, logic);
    }
    return logic;
}

/* ------------------------------------------------------- local crossings */

static const char *cdc_parent_name_of_child_domain(CdcCtx *c, const IrInstance *in, uint32_t cd)
{
    const IrModule *ch = in->module;
    uint32_t k;
    if (cd == IR_NONE || cd >= ch->ndomains)
        return NULL;
    for (k = 0; k < in->nconns; k++) {
        const IrConn *cn = &in->conns[k];
        if (cn->port == ch->domains[cd].clock && cn->expr && cn->expr->kind == IRE_SIG && cn->expr->sig < c->m->nsigs) {
            return cdc_domain_name(c, cdc_domain_of_clock(c, cn->expr->sig));
        }
    }
    return NULL;
}

/* what feeds the parent wire w (the source of an instance input) */
static const char *cdc_source_text(CdcCtx *c, uint32_t w)
{
    uint32_t f, e;
    chk_ranges_of(c->cm, w, &f, &e);
    if (e - f == 1) {
        const ChkNode *n = &c->cm->nodes.data[c->cm->ranges.data[f].node];
        if (n->kind == CHK_N_ASSIGN) {
            const IrAssign *a = &c->m->assigns[n->idx];
            if (a->rhs && a->rhs->kind == IRE_SIG && a->rhs->sig < c->m->nsigs)
                return c->m->sigs[a->rhs->sig].path;
        }
    }
    return c->m->sigs[w].path;
}

/* Level and pulse synchronizers sample `d` asynchronously: it must come
   straight from a register (or an input port / instance output, whose
   origin is not known here), never from combinational logic that can
   glitch (std::cdc::Sync2 documents this). */
static void cdc_sync_input(CdcCtx *c, const IrInstance *in)
{
    const IrModule *ch = in->module;
    uint32_t k;
    for (k = 0; k < in->nconns; k++) {
        const IrConn *cn = &in->conns[k];
        const IrSignal *ps;
        uint32_t f, e, w, lo;
        const ChkNode *n;
        const IrExpr *x;
        if (cn->port >= ch->nsigs || !cn->expr || cn->expr->kind != IRE_SIG || cn->expr->sig >= c->m->nsigs)
            continue;
        ps = &ch->sigs[cn->port];
        if (ps->kind != IRS_IN || !ps->type || ps->type->kind == IRT_CLOCK || ps->type->kind == IRT_RESET)
            continue;
        w = cn->expr->sig;
        chk_ranges_of(c->cm, w, &f, &e);
        if (e - f != 1)
            continue;
        n = &c->cm->nodes.data[c->cm->ranges.data[f].node];
        if (n->kind != CHK_N_ASSIGN)
            continue;
        x = c->m->assigns[n->idx].rhs;
        if (x && x->kind == IRE_CONST)
            continue;
        if (x && chk_expr_base(x, &lo) != IR_NONE) {
            const IrSignal *src = &c->m->sigs[chk_expr_base(x, &lo)];
            if (src->kind == IRS_REG || src->kind == IRS_IN || c->cm->inst_wire_inst[chk_expr_base(x, &lo)] ||
                chk_sig_attr(src, "unsafe_cross"))
                continue;
        }
        if (chk_warning(c->cm->cx, x && x->loc.file ? x->loc : in->loc,
                        "the input `%s` of synchronizer `%s` comes from combinational logic", ps->path, in->name))
            diag_help("register the value in its source domain first: a glitch in front of a synchronizer can "
                      "be captured");
    }
}

static void cdc_instance_crossing(CdcCtx *c, uint32_t ii, CdcLocalVec *out)
{
    const IrInstance *in = &c->m->insts[ii];
    const IrModule *ch = in->module;
    uint32_t k, from_d = IR_NONE, to_d = IR_NONE, src_port = IR_NONE;
    CdcLocal l;
    memset(&l, 0, sizeof l);
    l.inst = ii;
    l.kind = ch->cdc_kind;
    l.loc = in->loc;
    /* the first data input gives the source domain, the first output in
       another domain the destination */
    for (k = 0; k < ch->nports; k++) {
        const IrSignal *ps = &ch->sigs[ch->ports[k]];
        if (ps->kind == IRS_IN && ps->type && ps->type->kind != IRT_CLOCK && ps->type->kind != IRT_RESET &&
            ps->domain != IR_NONE) {
            from_d = ps->domain;
            src_port = ch->ports[k];
            break;
        }
    }
    for (k = 0; k < ch->nports; k++) {
        const IrSignal *ps = &ch->sigs[ch->ports[k]];
        if (ps->kind != IRS_OUT || ps->domain == IR_NONE)
            continue;
        if (from_d == IR_NONE || ch->domains[ps->domain].clock != ch->domains[from_d].clock) {
            to_d = ps->domain;
            break;
        }
        if (to_d == IR_NONE)
            to_d = ps->domain;
    }
    if (to_d == IR_NONE && ch->default_domain != IR_NONE)
        to_d = ch->default_domain;
    if (src_port == IR_NONE) {
        /* reset synchronizers: the source is the reset input */
        for (k = 0; k < ch->nports; k++) {
            const IrSignal *ps = &ch->sigs[ch->ports[k]];
            if (ps->kind == IRS_IN && ps->type && ps->type->kind == IRT_RESET) {
                src_port = ch->ports[k];
                /* ResetSync takes a raw asynchronous reset from anywhere */
                from_d = ch->cdc_kind == IRC_RESET_SYNC ? IR_NONE : ps->domain;
                break;
            }
        }
    }
    l.to = cdc_parent_name_of_child_domain(c, in, to_d);
    l.from = cdc_parent_name_of_child_domain(c, in, from_d);
    for (k = 0; k < in->nconns && src_port != IR_NONE; k++) {
        const IrConn *cn = &in->conns[k];
        if (cn->port != src_port || !cn->expr || cn->expr->kind != IRE_SIG || cn->expr->sig >= c->m->nsigs)
            continue;
        l.signal = cdc_source_text(c, cn->expr->sig);
        if (!l.from) {
            uint32_t sd = cdc_sig_domain(c, cn->expr->sig);
            if (sd != IR_NONE)
                l.from = cdc_domain_name(c, sd);
        }
    }
    if (!l.from)
        l.from = from_d == IR_NONE && src_port != IR_NONE ? "async" : "?";
    if (!l.to)
        l.to = "?";
    vec_push(*out, l);
}

/* ---------------------------------------------------------------- resets */

static void cdc_resets(CdcCtx *c)
{
    IrModule *m = c->m;
    uint32_t i, k, cd;
    for (i = 0; i < m->ndomains; i++) {
        uint32_t rs = m->domains[i].reset, rd;
        if (rs == IR_NONE || rs >= m->nsigs)
            continue;
        rd = cdc_sig_domain(c, rs);
        if (rd != IR_NONE && rd != c->canon[i] &&
            chk_warning(c->cm->cx, m->sigs[rs].loc, "reset `%s` of domain `%s` comes from domain `%s`", m->sigs[rs].path,
                        cdc_domain_name(c, c->canon[i]), cdc_domain_name(c, rd)))
            diag_help("synchronize it with std::cdc::ResetSync or std::cdc::ResetBridge");
    }
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        const IrModule *ch = in->module;
        if (!ch || (ch->is_cdc && (ch->cdc_kind == IRC_RESET_SYNC || ch->cdc_kind == IRC_RESET_BRIDGE)))
            continue;
        for (cd = 0; cd < ch->ndomains; cd++) {
            uint32_t pclk = IR_NONE, prst = IR_NONE, pd, rd;
            if (ch->domains[cd].reset == IR_NONE)
                continue;
            for (k = 0; k < in->nconns; k++) {
                const IrConn *cn = &in->conns[k];
                if (!cn->expr || cn->expr->kind != IRE_SIG || cn->expr->sig >= m->nsigs)
                    continue;
                if (cn->port == ch->domains[cd].clock)
                    pclk = cn->expr->sig;
                else if (cn->port == ch->domains[cd].reset)
                    prst = cn->expr->sig;
            }
            if (pclk == IR_NONE || prst == IR_NONE)
                continue;
            pd = cdc_domain_of_clock(c, pclk);
            rd = cdc_sig_domain(c, prst);
            if (pd != IR_NONE && rd != IR_NONE && pd != rd &&
                chk_warning(c->cm->cx, in->loc, "instance `%s`: reset `%s` comes from domain `%s` but resets domain `%s`",
                            in->name, m->sigs[prst].path, cdc_domain_name(c, rd), cdc_domain_name(c, pd)))
                diag_help("synchronize it with std::cdc::ResetSync or std::cdc::ResetBridge");
        }
    }
}

/* ------------------------------------------------------------------ pass */

void chk_cdc(ChkMod *cm)
{
    CdcCtx c;
    IrModule *m = cm->m;
    ChkCtx *cx = cm->cx;
    uint32_t i, j, k, nn = (uint32_t)cm->nodes.len;
    CdcLocalVec locals = {0};
    memset(&c, 0, sizeof c);
    c.cm = cm;
    c.m = m;
    c.canon = (uint32_t *)xmalloc((m->ndomains + 1) * sizeof(uint32_t));
    for (i = 0; i < m->ndomains; i++)
        c.canon[i] = cdc_resolve_clock(cm, m->domains[i].clock);
    c.sigkey = (uint32_t *)xmalloc((m->nsigs + 1) * sizeof(uint32_t));
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        c.sigkey[i] = !cdc_is_clock(s) && s->domain < m->ndomains ? c.canon[s->domain] : IR_NONE;
    }
    /* instance wires the elaborator left domain-free (the child's clock is
       connected to a parent clock without a `domain`) */
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        const IrModule *ch = in->module;
        for (j = 0; ch && j < in->nconns; j++) {
            const IrConn *cn = &in->conns[j];
            uint32_t cd, w;
            if (!cn->expr || cn->expr->kind != IRE_SIG || cn->port >= ch->nsigs)
                continue;
            w = cn->expr->sig;
            cd = ch->sigs[cn->port].domain;
            if (ch->sigs[cn->port].kind == IRS_IN && ch->sigs[cn->port].type &&
                ch->sigs[cn->port].type->kind == IRT_RESET)
                continue; /* connected directly to a parent reset */
            if (w >= m->nsigs || c.sigkey[w] != IR_NONE || cd >= ch->ndomains || cdc_is_clock(&m->sigs[w]))
                continue;
            for (k = 0; k < in->nconns; k++)
                if (in->conns[k].port == ch->domains[cd].clock && in->conns[k].expr &&
                    in->conns[k].expr->kind == IRE_SIG && in->conns[k].expr->sig < m->nsigs)
                    c.sigkey[w] = cdc_resolve_clock(cm, in->conns[k].expr->sig);
        }
    }
    c.nodedom = (uint32_t *)xmalloc((nn + 1) * sizeof(uint32_t));
    for (i = 0; i < nn; i++)
        c.nodedom[i] = IR_NONE;
    c.memdom = (uint32_t *)xmalloc((m->nsigs + 1) * sizeof(uint32_t));
    for (i = 0; i < m->nsigs; i++)
        c.memdom[i] = IR_NONE;
    for (i = 0; i < nn; i++) {
        const ChkNode *n = &cm->nodes.data[i];
        if (n->kind == CHK_N_CLOCKED && n->sig != IR_NONE && n->domain < m->ndomains) {
            c.nodedom[i] = c.canon[n->domain];
            if (m->sigs[n->sig].kind == IRS_MEM && c.memdom[n->sig] == IR_NONE)
                c.memdom[n->sig] = c.canon[n->domain];
        }
    }
    if (!m->is_cdc) {
        /* combinational nodes, drivers first */
        for (k = 0; k < cm->nscc; k++) {
            uint32_t q;
            for (q = cm->sfirst[k]; q < cm->sfirst[k + 1]; q++) {
                uint32_t ni = cm->order[q];
                const ChkNode *n = &cm->nodes.data[ni];
                const IrSignal *t = n->sig != IR_NONE ? &m->sigs[n->sig] : NULL;
                uint32_t fixed = IR_NONE;
                if (t)
                    fixed = c.sigkey[n->sig];
                if (t && cdc_is_clock(t))
                    continue;
                if (n->kind == CHK_N_INST_OUT && fixed != IR_NONE) {
                    c.nodedom[ni] = fixed;    /* checked inside the child */
                    continue;
                }
                c.nodedom[ni] = cdc_check_node(&c, ni, fixed);
            }
        }
        /* clocked processes */
        for (i = 0; i < nn; i++) {
            const ChkNode *n = &cm->nodes.data[i];
            if ((n->kind == CHK_N_CLOCKED || n->kind == CHK_N_PROC) && n->domain != IR_NONE && n->domain < m->ndomains)
                cdc_check_node(&c, i, c.canon[n->domain]);
        }
        cdc_resets(&c);
    }
    /* local crossings: primitive instances and unsafe_cross */
    for (i = 0; i < m->ninsts; i++) {
        const IrModule *ch = m->insts[i].module;
        if (!ch || !ch->is_cdc)
            continue;
        cdc_instance_crossing(&c, i, &locals);
        if (!m->is_cdc && (ch->cdc_kind == IRC_SYNC2 || ch->cdc_kind == IRC_SYNC3 || ch->cdc_kind == IRC_PULSE))
            cdc_sync_input(&c, &m->insts[i]);
    }
    for (i = 0; i < m->nsigs; i++) {
        CdcLocal l;
        uint64_t rn;
        uint32_t f, e, sd = IR_NONE, td = IR_NONE;
        if (!chk_sig_attr(&m->sigs[i], "unsafe_cross"))
            continue;
        memset(&l, 0, sizeof l);
        l.inst = IR_NONE;
        l.kind = IRC_UNSAFE;
        l.loc = m->sigs[i].loc;
        l.signal = cdc_source_text(&c, i);
        chk_ranges_of(cm, i, &f, &e);
        if (f < e) {
            const ChkNode *dn = &cm->nodes.data[cm->ranges.data[f].node];
            uint32_t r;
            for (r = dn->rd; r < dn->rd + dn->nrd && sd == IR_NONE; r++)
                sd = cdc_read_domain(&c, &cm->reads.data[r]);
        }
        if (map_get(&c.unsafe_reader, (uint64_t)i + 1, &rn)) {
            const ChkNode *rnode = &cm->nodes.data[rn - 1];
            if ((rnode->kind == CHK_N_CLOCKED || rnode->kind == CHK_N_PROC) && rnode->domain < m->ndomains)
                td = c.canon[rnode->domain];
            else
                td = c.nodedom[rn - 1];
            if (td == IR_NONE && rnode->sig != IR_NONE)
                td = cdc_sig_domain(&c, rnode->sig);
        }
        l.from = sd != IR_NONE ? cdc_domain_name(&c, sd) : "?";
        l.to = td != IR_NONE ? cdc_domain_name(&c, td) : "?";
        vec_push(locals, l);
    }
    if (locals.len) {
        ChkCdcMod *cdm = ANEW(&cx->arena, ChkCdcMod);
        cdm->n = (uint32_t)locals.len;
        cdm->items = ANEWN(&cx->arena, CdcLocal, locals.len);
        memcpy(cdm->items, locals.data, locals.len * sizeof(CdcLocal));
        map_put(&cx->cdc_mods, MAP_PTR(m), MAP_PTR(cdm));
    }
    vec_free(locals);
    free(c.canon);
    free(c.sigkey);
    free(c.nodedom);
    free(c.memdom);
    map_free(&c.reported);
    map_free(&c.unsafe_reader);
}

/* ---------------------------------------------------------------- report */

#define CDC_MAX_CROSSINGS 100000u

static void cdc_walk(ChkCtx *cx, const IrModule *m, StrBuf *path, int depth)
{
    uint64_t v;
    uint32_t i;
    size_t len = path->len;
    if (depth > 64 || cx->d->ncrossings >= CDC_MAX_CROSSINGS)
        return;
    if (map_get(&cx->cdc_mods, MAP_PTR(m), &v)) {
        const ChkCdcMod *cdm = (const ChkCdcMod *)(uintptr_t)v;
        for (i = 0; i < cdm->n; i++) {
            const CdcLocal *l = &cdm->items[i];
            IrCrossing x;
            memset(&x, 0, sizeof x);
            if (l->inst != IR_NONE)
                sb_printf(path, " > %s", m->insts[l->inst].name);
            x.path = arena_strdup(cx->d->arena, sb_cstr(path));
            sb_truncate(path, len);
            x.kind = l->kind;
            x.from_domain = l->from;
            x.to_domain = l->to;
            x.signal = l->signal ? arena_strdup(cx->d->arena, l->signal) : NULL;
            x.loc = l->loc;
            ir_design_add_crossing(cx->d, &x);
        }
    }
    for (i = 0; i < m->ninsts; i++) {
        const IrModule *ch = m->insts[i].module;
        if (!ch || ch->is_cdc || ch->is_extern)
            continue;
        sb_printf(path, " > %s", m->insts[i].name);
        cdc_walk(cx, ch, path, depth + 1);
        sb_truncate(path, len);
    }
}

void check_print_cdc_report(const IrDesign *d, FILE *out)
{
    uint32_t i;
    if (!out)
        out = stdout;
    fprintf(out, "CDC report: %u crossing%s\n", (unsigned)d->ncrossings, d->ncrossings == 1 ? "" : "s");
    for (i = 0; i < d->ncrossings; i++) {
        const IrCrossing *x = &d->crossings[i];
        const SourceFile *f = x->loc.file ? src_get(x->loc.file) : NULL;
        fprintf(out, "  %s: %s, %s -> %s", x->path ? x->path : "?", ir_cdc_kind_name(x->kind),
                x->from_domain && *x->from_domain ? x->from_domain : "?",
                x->to_domain && *x->to_domain ? x->to_domain : "?");
        if (x->signal)
            fprintf(out, ", signal %s", x->signal);
        if (f && f->path)
            fprintf(out, " (%s:%u)", diag_path_display(f->path), (unsigned)x->loc.line);
        fputc('\n', out);
    }
}

void chk_crossings(ChkCtx *cx)
{
    IrDesign *d = cx->d;
    uint32_t i;
    StrBuf path = {0};
    if (map_len(&cx->cdc_mods)) {
        /* roots: modules nobody instantiates */
        for (i = 0; i < d->nmodules; i++) {
            const IrModule *m = d->modules[i];
            if (!m || map_has(&cx->instantiated, MAP_PTR(m)))
                continue;
            sb_reset(&path);
            sb_puts(&path, m->display ? m->display : m->name);
            cdc_walk(cx, m, &path, 0);
        }
    }
    sb_free(&path);
    if (cx->opt->cdc_report >= 2 || (cx->opt->cdc_report == 1 && d->ncrossings)) {
        diag_flush();
        check_print_cdc_report(d, cx->opt->report_out);
    }
}

#undef CDC_MAX_CROSSINGS
