/* check.c — entry point of the IR checks (see check.h) and the per-module
 * model the passes share (see check_impl.h). */
#include "check_impl.h"

/* =============================================================== helpers */

int chk_is_comb_node(const ChkNode *n)
{
    return n->kind == CHK_N_ASSIGN || n->kind == CHK_N_COMB || n->kind == CHK_N_INST_OUT;
}

int chk_range_overlaps(const ChkRange *r, uint32_t lo, uint32_t hi)
{
    return r->lo < hi && lo < r->hi;
}

void chk_ranges_of(const ChkMod *cm, uint32_t sig, uint32_t *first, uint32_t *end)
{
    if (sig >= cm->m->nsigs) {
        *first = *end = 0;
        return;
    }
    *first = cm->rfirst[sig];
    *end = cm->rfirst[sig + 1];
}

void chk_ranges_from(const ChkMod *cm, uint32_t sig, uint32_t lo, uint32_t *first, uint32_t *end)
{
    uint32_t a, z, base;
    chk_ranges_of(cm, sig, first, end);
    base = *first;
    a = *first;
    z = *end;
    while (a < z) {      /* first range with r.lo >= lo */
        uint32_t mid = a + (z - a) / 2;
        if (cm->ranges.data[mid].lo < lo)
            a = mid + 1;
        else
            z = mid;
    }
    while (a > base && cm->ranges.data[a - 1].hi > lo)
        a--;
    *first = a;
}

static int chk_vreport(ChkCtx *cx, int warn, Span at, const char *fmt, va_list ap)
{
    char buf[1024];
    uint64_t h;
    vsnprintf(buf, sizeof buf, fmt, ap);
    h = hash_combine(hash_bytes(buf, strlen(buf)),
                     hash_combine(hash_u64(at.file), hash_combine(hash_u64(at.off), hash_u64((uint64_t)warn)))) |
        1;
    if (map_has(&cx->seen, h))
        return 0;
    map_put(&cx->seen, h, 1);
    if (warn) {
        diag_warning(at, "%s", buf);
    } else {
        diag_error(at, "%s", buf);
        cx->errors++;
    }
    return 1;
}

int chk_error(ChkCtx *cx, Span at, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = chk_vreport(cx, 0, at, fmt, ap);
    va_end(ap);
    return r;
}

int chk_warning(ChkCtx *cx, Span at, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = chk_vreport(cx, 1, at, fmt, ap);
    va_end(ap);
    return r;
}

static uint32_t chk_sig_width(const IrModule *m, uint32_t s)
{
    if (m->sigs[s].kind == IRS_MEM)
        return 1;
    return m->sigs[s].type ? m->sigs[s].type->width : 1;
}

const char *chk_bits_name(ChkCtx *cx, const IrModule *m, uint32_t sig, uint32_t lo, uint32_t hi)
{
    const IrSignal *s = &m->sigs[sig];
    const IrType *t = s->type;
    StrBuf sb = {0};
    const char *r;
    if ((lo == 0 && hi >= chk_sig_width(m, sig)) || s->kind == IRS_MEM || !t)
        return arena_printf(&cx->arena, "`%s`", s->path);
    /* name struct fields and array elements: `f.carry`, `arr[2]` */
    sb_puts(&sb, s->path);
    while (t && !(lo == 0 && hi == t->width)) {
        if (t->kind == IRT_STRUCT) {
            uint32_t i;
            const IrField *f = NULL;
            for (i = 0; i < t->nfields; i++)
                if (lo >= t->fields[i].lsb && hi <= t->fields[i].lsb + t->fields[i].type->width)
                    f = &t->fields[i];
            if (!f)
                break;
            sb_printf(&sb, ".%s", f->name);
            lo -= f->lsb;
            hi -= f->lsb;
            t = f->type;
        } else if (t->kind == IRT_ARRAY && t->elem && t->elem->width) {
            uint32_t ew = t->elem->width, k = lo / ew;
            if (hi > (k + 1) * ew)
                break;
            sb_printf(&sb, "[%u]", (unsigned)k);
            lo -= k * ew;
            hi -= k * ew;
            t = t->elem;
        } else {
            break;
        }
    }
    if (t && lo == 0 && hi == t->width)
        r = arena_printf(&cx->arena, "`%s`", sb_cstr(&sb));
    else if (hi == lo + 1)
        r = arena_printf(&cx->arena, "bit %u of `%s`", (unsigned)lo, sb_cstr(&sb));
    else
        r = arena_printf(&cx->arena, "bits [%u:%u] of `%s`", (unsigned)(hi - 1), (unsigned)lo, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

const char *chk_span_text(ChkCtx *cx, Span s, size_t max)
{
    const SourceFile *f;
    size_t i;
    if (!s.file || !s.len || s.len > max)
        return "";
    f = src_get(s.file);
    if (!f || (size_t)s.off + s.len > f->len)
        return "";
    for (i = 0; i < s.len; i++)
        if (f->text[s.off + i] == '\n')
            return "";
    return arena_strndup(&cx->arena, f->text + s.off, s.len);
}

uint32_t chk_array_extent(const IrType *t, uint32_t lo, uint32_t ew)
{
    uint32_t base = lo;
    const IrType *cur = t;
    while (cur) {
        if (cur->kind == IRT_ARRAY && lo == 0 && cur->elem && cur->elem->width == ew)
            return cur->width;
        if (cur->kind == IRT_ARRAY && cur->elem && cur->elem->width) {
            lo %= cur->elem->width;
            cur = cur->elem;
            continue;
        }
        if (cur->kind == IRT_STRUCT) {
            uint32_t i;
            const IrType *nx = NULL;
            for (i = 0; i < cur->nfields; i++) {
                const IrField *f = &cur->fields[i];
                if (lo >= f->lsb && lo < f->lsb + f->type->width) {
                    lo -= f->lsb;
                    nx = f->type;
                    break;
                }
            }
            cur = nx;
            continue;
        }
        break;
    }
    return t && t->width > base ? t->width - base : ew;
}

const IrAttr *chk_sig_attr(const IrSignal *s, const char *name)
{
    uint32_t i;
    for (i = 0; i < s->nattrs; i++)
        if (s->attrs[i].name && strcmp(s->attrs[i].name, name) == 0)
            return &s->attrs[i];
    return NULL;
}

const char *chk_kind_word(IrSigKind k)
{
    switch (k) {
    case IRS_IN: return "input";
    case IRS_OUT: return "output";
    case IRS_WIRE: return "wire";
    case IRS_VAR: return "var";
    case IRS_REG: return "register";
    case IRS_MEM: return "memory";
    }
    return "signal";
}

void chk_bits_set_range(uint64_t *b, uint32_t lo, uint32_t hi)
{
    uint32_t i;
    for (i = lo; i < hi && (i & 63u); i++)
        b[i >> 6] |= (uint64_t)1 << (i & 63u);
    for (; i + 64 <= hi; i += 64)
        b[i >> 6] = ~(uint64_t)0;
    for (; i < hi; i++)
        b[i >> 6] |= (uint64_t)1 << (i & 63u);
}

int chk_bits_first_clear(const uint64_t *b, uint32_t lo, uint32_t hi, uint32_t *clo, uint32_t *chi)
{
    uint32_t i = lo;
    while (i < hi) {
        if (!(i & 63u) && i + 64 <= hi && b[i >> 6] == ~(uint64_t)0) {
            i += 64;
            continue;
        }
        if (!((b[i >> 6] >> (i & 63u)) & 1u))
            break;
        i++;
    }
    if (i >= hi)
        return 0;
    *clo = i;
    while (i < hi && !((b[i >> 6] >> (i & 63u)) & 1u))
        i++;
    *chi = i;
    return 1;
}

int chk_bits_all(const uint64_t *b, uint32_t lo, uint32_t hi)
{
    uint32_t a, z;
    return !chk_bits_first_clear(b, lo, hi, &a, &z);
}

/* ========================================================= model build */

typedef struct ChkTmpRead {
    uint32_t node, seq;
    ChkRead r;
} ChkTmpRead;

typedef struct ChkBuild {
    ChkMod *cm;
    VEC(ChkTmpRead) reads;
    VEC(ChkRange) ranges;
    Map proc_nodes;       /* target sig + 1 -> node + 1 (per process) */
    Map conds_added;      /* hash(node, cond expr) of conditions already read */
    VEC(const IrExpr *) conds;
    uint32_t cur;         /* node receiving reads */
} ChkBuild;

static void chkb_read(ChkBuild *b, uint32_t sig, uint32_t lo, uint32_t hi, Span loc)
{
    ChkTmpRead t;
    t.node = b->cur;
    t.seq = (uint32_t)b->reads.len;
    t.r.sig = sig;
    t.r.lo = lo;
    t.r.hi = hi;
    t.r.loc = loc;
    vec_push(b->reads, t);
}

/* the signal and bit offset an expression made of slices of one signal
   denotes; IR_NONE if it is something else */
static uint32_t chk_expr_base(const IrExpr *e, uint32_t *lo)
{
    uint32_t off = 0;
    while (e && e->kind == IRE_SLICE && e->nargs == 1) {
        off += e->lo;
        e = e->args[0];
    }
    if (e && e->kind == IRE_SIG) {
        *lo = off;
        return e->sig;
    }
    return IR_NONE;
}

static void chkb_expr(ChkBuild *b, const IrExpr *e)
{
    uint32_t i, lo, s;
    const IrModule *m = b->cm->m;
    if (!e)
        return;
    switch (e->kind) {
    case IRE_CONST: return;
    case IRE_SIG:
        if (e->sig < m->nsigs)
            chkb_read(b, e->sig, 0, chk_sig_width(m, e->sig), e->loc);
        return;
    case IRE_SLICE:
        s = chk_expr_base(e, &lo);
        if (s != IR_NONE && s < m->nsigs) {
            chkb_read(b, s, lo, lo + e->width, e->loc);
            return;
        }
        break;
    case IRE_INDEX:
        if (e->nargs == 2) {
            s = chk_expr_base(e->args[0], &lo);
            if (s != IR_NONE && s < m->nsigs) {
                chkb_read(b, s, lo, lo + e->args[0]->width, e->loc);
                chkb_expr(b, e->args[1]);
                return;
            }
        }
        break;
    case IRE_MEMREAD:
        if (e->sig < m->nsigs)
            chkb_read(b, e->sig, 0, 1, e->loc);
        break;
    default: break;
    }
    for (i = 0; i < e->nargs; i++)
        chkb_expr(b, e->args[i]);
}

static uint32_t chkb_node(ChkBuild *b, ChkNodeKind kind, uint32_t idx, uint32_t sig, uint32_t domain, Span loc)
{
    ChkNode n;
    memset(&n, 0, sizeof n);
    n.kind = kind;
    n.idx = idx;
    n.aux = IR_NONE;
    n.sig = sig;
    n.domain = domain;
    n.loc = loc;
    vec_push(b->cm->nodes, n);
    return (uint32_t)b->cm->nodes.len - 1;
}

static void chkb_range(ChkBuild *b, uint32_t sig, uint32_t lo, uint32_t hi, uint32_t node, Span loc)
{
    ChkRange r;
    if (hi <= lo)
        return;
    r.sig = sig;
    r.lo = lo;
    r.hi = hi;
    r.node = node;
    r.loc = loc;
    vec_push(b->ranges, r);
}

/* the bits an lvalue may write */
static void chk_lvalue_bits(const IrModule *m, const IrLvalue *lv, uint32_t *lo, uint32_t *hi)
{
    uint32_t w = chk_sig_width(m, lv->sig);
    *lo = lv->lo;
    if (lv->index)
        *hi = lv->lo + chk_array_extent(m->sigs[lv->sig].type, lv->lo, lv->width);
    else
        *hi = lv->lo + lv->width;
    if (*hi > w)
        *hi = w;
    if (*lo > *hi)
        *lo = *hi;
}

/* the node of process p driving `sig` (IR_NONE: the reads-only node) */
static uint32_t chkb_proc_node(ChkBuild *b, const IrProcess *p, uint32_t pi, uint32_t sig, Span loc)
{
    uint64_t v;
    uint64_t key = (uint64_t)sig + 1;
    ChkNodeKind kind;
    if (map_get(&b->proc_nodes, key, &v))
        return (uint32_t)v - 1;
    if (sig == IR_NONE)
        kind = CHK_N_PROC;
    else
        kind = p->kind == IRP_COMB ? CHK_N_COMB : CHK_N_CLOCKED;
    v = chkb_node(b, kind, pi, sig, p->domain, loc);
    map_put(&b->proc_nodes, key, v + 1);
    return (uint32_t)v;
}

/* reads of the enclosing conditions into the current node (once each) */
static void chkb_conds(ChkBuild *b)
{
    size_t i;
    for (i = 0; i < b->conds.len; i++) {
        uint64_t h = hash_combine(hash_u64(b->cur), hash_u64((uint64_t)(uintptr_t)b->conds.data[i]));
        if (map_has(&b->conds_added, h))
            continue;
        map_put(&b->conds_added, h, 1);
        chkb_expr(b, b->conds.data[i]);
    }
}

static void chkb_block(ChkBuild *b, const IrProcess *p, uint32_t pi, const IrBlock *blk);

static void chkb_stmt(ChkBuild *b, const IrProcess *p, uint32_t pi, const IrStmt *s)
{
    const IrModule *m = b->cm->m;
    uint32_t i, lo, hi;
    switch (s->kind) {
    case IRST_ASSIGN:
        if (s->lhs.sig >= m->nsigs)
            return;
        b->cur = chkb_proc_node(b, p, pi, s->lhs.sig, s->loc);
        chk_lvalue_bits(m, &s->lhs, &lo, &hi);
        chkb_range(b, s->lhs.sig, lo, hi, b->cur, s->loc);
        chkb_expr(b, s->rhs);
        chkb_expr(b, s->lhs.index);
        chkb_conds(b);
        return;
    case IRST_MEMWRITE:
        if (s->mem >= m->nsigs)
            return;
        b->cur = chkb_proc_node(b, p, pi, s->mem, s->loc);
        chkb_range(b, s->mem, 0, 1, b->cur, s->loc);
        chkb_expr(b, s->addr);
        chkb_expr(b, s->data);
        chkb_expr(b, s->mask);
        chkb_conds(b);
        return;
    case IRST_ASSERT:
        b->cur = chkb_proc_node(b, p, pi, IR_NONE, p->loc);
        chkb_expr(b, s->cond);
        chkb_conds(b);
        return;
    case IRST_IF:
        vec_push(b->conds, s->cond);
        chkb_block(b, p, pi, &s->then_b);
        chkb_block(b, p, pi, &s->else_b);
        b->conds.len--;
        return;
    case IRST_CASE:
        vec_push(b->conds, s->subject);
        for (i = 0; i < s->narms; i++)
            chkb_block(b, p, pi, &s->arms[i].body);
        b->conds.len--;
        return;
    }
}

static void chkb_block(ChkBuild *b, const IrProcess *p, uint32_t pi, const IrBlock *blk)
{
    uint32_t i;
    for (i = 0; i < blk->n; i++)
        chkb_stmt(b, p, pi, blk->stmts[i]);
}

static void chkb_seq(ChkBuild *b, const IrSeq *q)
{
    if (!q)
        return;
    chkb_expr(b, q->expr);
    chkb_seq(b, q->a);
    chkb_seq(b, q->b);
}

static int chk_cmp_range(const void *pa, const void *pb)
{
    const ChkRange *a = (const ChkRange *)pa, *b = (const ChkRange *)pb;
    if (a->sig != b->sig)
        return a->sig < b->sig ? -1 : 1;
    if (a->lo != b->lo)
        return a->lo < b->lo ? -1 : 1;
    if (a->node != b->node)
        return a->node < b->node ? -1 : 1;
    if (a->hi != b->hi)
        return a->hi < b->hi ? -1 : 1;
    return 0;
}

static const ChkSummary *chk_summary_of(ChkCtx *cx, const IrModule *m)
{
    uint64_t v;
    if (map_get(&cx->summaries, MAP_PTR(m), &v))
        return (const ChkSummary *)(uintptr_t)v;
    return NULL;
}

/* Tarjan's algorithm (iterative) over the combinational nodes */
static void chk_scc(ChkMod *cm)
{
    uint32_t n = (uint32_t)cm->nodes.len, i, idx = 0, sp = 0, cs = 0, nout = 0, k;
    uint32_t *index = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *low = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *stack = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint8_t *on = (uint8_t *)xcalloc(n + 1, 1);
    uint32_t *cnode = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *cedge = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *rorder = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t *rsizes = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    uint32_t nsz = 0;
    for (i = 0; i < n; i++)
        index[i] = IR_NONE;
    for (i = 0; i < n; i++) {
        if (index[i] != IR_NONE || !chk_is_comb_node(&cm->nodes.data[i]))
            continue;
        /* push i */
        cs = 0;
        cnode[cs] = i;
        cedge[cs] = cm->efirst[i];
        cs++;
        index[i] = low[i] = idx++;
        stack[sp++] = i;
        on[i] = 1;
        while (cs) {
            uint32_t v = cnode[cs - 1];
            if (cedge[cs - 1] < cm->efirst[v + 1]) {
                uint32_t w = cm->edges[cedge[cs - 1]++];
                if (index[w] == IR_NONE) {
                    index[w] = low[w] = idx++;
                    stack[sp++] = w;
                    on[w] = 1;
                    cnode[cs] = w;
                    cedge[cs] = cm->efirst[w];
                    cs++;
                } else if (on[w] && index[w] < low[v]) {
                    low[v] = index[w];
                }
                continue;
            }
            if (low[v] == index[v]) {
                uint32_t sz = 0, w;
                do {
                    w = stack[--sp];
                    on[w] = 0;
                    rorder[nout++] = w;
                    sz++;
                } while (w != v);
                rsizes[nsz++] = sz;
            }
            cs--;
            if (cs) {
                uint32_t u = cnode[cs - 1];
                if (low[v] < low[u])
                    low[u] = low[v];
            }
        }
    }
    /* Tarjan emits components readers-first; reverse into drivers-first */
    cm->nscc = nsz;
    cm->order = ANEWN(&cm->cx->arena, uint32_t, nout + 1);
    cm->sfirst = ANEWN(&cm->cx->arena, uint32_t, nsz + 1);
    {
        uint32_t pos = 0, src = nout, c;
        for (c = 0; c < nsz; c++) {
            uint32_t sz = rsizes[nsz - 1 - c];
            src -= sz;
            cm->sfirst[c] = pos;
            for (k = 0; k < sz; k++)
                cm->order[pos++] = rorder[src + k];
        }
        cm->sfirst[nsz] = pos;
    }
    free(index);
    free(low);
    free(stack);
    free(on);
    free(cnode);
    free(cedge);
    free(rorder);
    free(rsizes);
}

static void chk_mod_build(ChkMod *cm)
{
    IrModule *m = cm->m;
    ChkBuild b;
    uint32_t i, k;
    size_t j;
    memset(&b, 0, sizeof b);
    b.cm = cm;
    /* continuous assignments */
    for (i = 0; i < m->nassigns; i++) {
        const IrAssign *a = &m->assigns[i];
        uint32_t lo, hi;
        if (a->lhs.sig >= m->nsigs)
            continue;
        b.cur = chkb_node(&b, CHK_N_ASSIGN, i, a->lhs.sig, IR_NONE, a->loc);
        chk_lvalue_bits(m, &a->lhs, &lo, &hi);
        chkb_range(&b, a->lhs.sig, lo, hi, b.cur, a->loc);
        chkb_expr(&b, a->rhs);
        chkb_expr(&b, a->lhs.index);
    }
    /* processes */
    for (i = 0; i < m->nprocs; i++) {
        const IrProcess *p = &m->procs[i];
        map_clear(&b.proc_nodes);
        map_clear(&b.conds_added);
        b.conds.len = 0;
        chkb_block(&b, p, i, &p->body);
    }
    /* instances */
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        const IrModule *c = in->module;
        const ChkSummary *sum = c ? chk_summary_of(cm->cx, c) : NULL;
        Map conn_of = {0};
        b.cur = chkb_node(&b, CHK_N_INST_IN, i, IR_NONE, IR_NONE, in->loc);
        for (k = 0; k < in->nconns; k++) {
            const IrConn *cn = &in->conns[k];
            if (!c || cn->port >= c->nsigs)
                continue;
            map_put(&conn_of, cn->port, k);
            if (c->sigs[cn->port].kind == IRS_IN)
                chkb_expr(&b, cn->expr);
        }
        for (k = 0; k < in->nconns; k++) {
            const IrConn *cn = &in->conns[k];
            uint32_t w, nd, q;
            if (!c || cn->port >= c->nsigs || c->sigs[cn->port].kind != IRS_OUT || !cn->expr ||
                cn->expr->kind != IRE_SIG || cn->expr->sig >= m->nsigs)
                continue;
            w = cn->expr->sig;
            nd = chkb_node(&b, CHK_N_INST_OUT, i, w, IR_NONE, in->loc);
            cm->nodes.data[nd].aux = cn->port;
            chkb_range(&b, w, 0, chk_sig_width(m, w), nd, in->loc);
            cm->inst_wire_inst[w] = i + 1;
            if (!sum || cn->port >= sum->nports)
                continue;
            b.cur = nd;
            for (q = sum->first[cn->port]; q < sum->first[cn->port + 1]; q++) {
                uint64_t ci;
                if (map_get(&conn_of, sum->deps[q], &ci)) {
                    uint32_t before = (uint32_t)b.reads.len;
                    chkb_expr(&b, in->conns[ci].expr);
                    for (; before < b.reads.len; before++)
                        b.reads.data[before].r.loc = in->loc;
                }
            }
        }
        map_free(&conn_of);
    }
    /* properties */
    for (i = 0; i < m->nprops; i++) {
        b.cur = chkb_node(&b, CHK_N_PROP, i, IR_NONE, m->props[i].domain, m->props[i].loc);
        chkb_seq(&b, m->props[i].seq);
    }
    /* reads, grouped by node in program order (counting sort) */
    {
        uint32_t nn = (uint32_t)cm->nodes.len;
        uint32_t *cnt = (uint32_t *)xcalloc(nn + 1, sizeof(uint32_t));
        vec_resize(cm->reads, b.reads.len);
        for (j = 0; j < b.reads.len; j++)
            cnt[b.reads.data[j].node + 1]++;
        for (i = 0; i < nn; i++) {
            cnt[i + 1] += cnt[i];
            cm->nodes.data[i].rd = cnt[i];
            cm->nodes.data[i].nrd = 0;
        }
        for (j = 0; j < b.reads.len; j++) {
            ChkNode *nd = &cm->nodes.data[b.reads.data[j].node];
            cm->reads.data[nd->rd + nd->nrd++] = b.reads.data[j].r;
        }
        free(cnt);
    }
    /* ranges: sorted, consecutive ranges of one node merged */
    qsort(b.ranges.data, b.ranges.len, sizeof(ChkRange), chk_cmp_range);
    for (j = 0; j < b.ranges.len; j++) {
        ChkRange *r = &b.ranges.data[j];
        if (cm->ranges.len) {
            ChkRange *l = &vec_last(cm->ranges);
            if (l->sig == r->sig && l->node == r->node && r->lo <= l->hi) {
                if (r->hi > l->hi)
                    l->hi = r->hi;
                continue;
            }
        }
        vec_push(cm->ranges, *r);
    }
    cm->rfirst = ANEWN(&cm->cx->arena, uint32_t, m->nsigs + 1);
    {
        uint32_t s = 0;
        for (j = 0; j < cm->ranges.len; j++)
            while (s <= cm->ranges.data[j].sig)
                cm->rfirst[s++] = (uint32_t)j;
        while (s <= m->nsigs)
            cm->rfirst[s++] = (uint32_t)cm->ranges.len;
    }
    /* combinational edges driver -> reader */
    {
        uint32_t nn = (uint32_t)cm->nodes.len;
        VEC(uint32_t) src = {0};
        VEC(uint32_t) dst = {0};
        uint32_t *cnt;
        cm->self_loop = (uint8_t *)arena_zalloc(&cm->cx->arena, nn + 1);
        for (i = 0; i < nn; i++) {
            const ChkNode *rn = &cm->nodes.data[i];
            uint32_t r;
            if (!chk_is_comb_node(rn))
                continue;
            for (r = rn->rd; r < rn->rd + rn->nrd; r++) {
                const ChkRead *rd = &cm->reads.data[r];
                uint32_t f, e;
                chk_ranges_from(cm, rd->sig, rd->lo, &f, &e);
                for (; f < e; f++) {
                    const ChkRange *rg = &cm->ranges.data[f];
                    if (rg->lo >= rd->hi)
                        break;
                    if (!chk_range_overlaps(rg, rd->lo, rd->hi) || !chk_is_comb_node(&cm->nodes.data[rg->node]))
                        continue;
                    if (rg->node == i) {
                        if (rn->kind != CHK_N_COMB)
                            cm->self_loop[i] = 1;
                        continue;
                    }
                    vec_push(src, rg->node);
                    vec_push(dst, i);
                }
            }
        }
        cnt = (uint32_t *)xcalloc(nn + 2, sizeof(uint32_t));
        for (j = 0; j < src.len; j++)
            cnt[src.data[j] + 1]++;
        cm->efirst = ANEWN(&cm->cx->arena, uint32_t, nn + 1);
        cm->edges = ANEWN(&cm->cx->arena, uint32_t, src.len + 1);
        for (i = 0; i < nn; i++)
            cnt[i + 1] += cnt[i];
        for (i = 0; i <= nn; i++)
            cm->efirst[i] = cnt[i];
        for (j = 0; j < src.len; j++)
            cm->edges[cnt[src.data[j]]++] = dst.data[j];
        free(cnt);
        vec_free(src);
        vec_free(dst);
    }
    chk_scc(cm);
    vec_free(b.reads);
    vec_free(b.ranges);
    vec_free(b.conds);
    map_free(&b.proc_nodes);
    map_free(&b.conds_added);
}

/* ============================================================ summary */

void chk_summary(ChkMod *cm)
{
    IrModule *m = cm->m;
    ChkCtx *cx = cm->cx;
    uint32_t nin = 0, i, k, w, nn = (uint32_t)cm->nodes.len;
    uint32_t *in_index = (uint32_t *)xmalloc((m->nsigs + 1) * sizeof(uint32_t));
    uint32_t *in_sig;
    uint64_t *bits;
    ChkSummary *sum = ANEW(&cx->arena, ChkSummary);
    VEC(uint32_t) deps = {0};
    for (i = 0; i < m->nsigs; i++)
        in_index[i] = IR_NONE;
    for (i = 0; i < m->nports; i++) {
        uint32_t s = m->ports[i];
        if (s < m->nsigs && m->sigs[s].kind == IRS_IN && m->sigs[s].type && m->sigs[s].type->kind != IRT_CLOCK)
            in_index[s] = nin++;
    }
    in_sig = (uint32_t *)xmalloc((nin + 1) * sizeof(uint32_t));
    for (i = 0; i < m->nsigs; i++)
        if (in_index[i] != IR_NONE)
            in_sig[in_index[i]] = i;
    w = CHK_WORDS(nin);
    if (!w)
        w = 1;
    bits = (uint64_t *)xcalloc((size_t)nn * w + 1, sizeof(uint64_t));
    for (k = 0; k < cm->nscc; k++) {
        uint32_t q;
        for (q = cm->sfirst[k]; q < cm->sfirst[k + 1]; q++) {
            uint32_t ni = cm->order[q], r;
            const ChkNode *nd = &cm->nodes.data[ni];
            uint64_t *mine = bits + (size_t)ni * w;
            for (r = nd->rd; r < nd->rd + nd->nrd; r++) {
                const ChkRead *rd = &cm->reads.data[r];
                uint32_t f, e, x;
                if (in_index[rd->sig] != IR_NONE) {
                    mine[in_index[rd->sig] >> 6] |= (uint64_t)1 << (in_index[rd->sig] & 63u);
                    continue;
                }
                chk_ranges_from(cm, rd->sig, rd->lo, &f, &e);
                for (; f < e; f++) {
                    const ChkRange *rg = &cm->ranges.data[f];
                    const uint64_t *theirs;
                    if (rg->lo >= rd->hi)
                        break;
                    if (rg->node == ni || !chk_range_overlaps(rg, rd->lo, rd->hi) ||
                        !chk_is_comb_node(&cm->nodes.data[rg->node]))
                        continue;
                    theirs = bits + (size_t)rg->node * w;
                    for (x = 0; x < w; x++)
                        mine[x] |= theirs[x];
                }
            }
        }
    }
    sum->nports = m->nsigs;
    sum->first = ANEWN(&cx->arena, uint32_t, m->nsigs + 1);
    for (i = 0; i < m->nsigs; i++) {
        sum->first[i] = (uint32_t)deps.len;
        if (m->sigs[i].kind == IRS_OUT && nin) {
            uint64_t *acc = (uint64_t *)xcalloc(w, sizeof(uint64_t));
            uint32_t f, e, x;
            chk_ranges_of(cm, i, &f, &e);
            for (; f < e; f++)
                if (chk_is_comb_node(&cm->nodes.data[cm->ranges.data[f].node]))
                    for (x = 0; x < w; x++)
                        acc[x] |= bits[(size_t)cm->ranges.data[f].node * w + x];
            for (x = 0; x < nin; x++)
                if ((acc[x >> 6] >> (x & 63u)) & 1u)
                    vec_push(deps, in_sig[x]);
            free(acc);
        }
    }
    sum->first[m->nsigs] = (uint32_t)deps.len;
    sum->deps = ANEWN(&cx->arena, uint32_t, deps.len + 1);
    if (deps.len)
        memcpy(sum->deps, deps.data, deps.len * sizeof(uint32_t));
    map_put(&cx->summaries, MAP_PTR(m), MAP_PTR(sum));
    vec_free(deps);
    free(bits);
    free(in_index);
    free(in_sig);
}

/* ============================================================== entry */

int check_design(IrDesign *d, const CheckOptions *opt)
{
    ChkCtx cx;
    CheckOptions defaults;
    uint32_t i;
    if (!d)
        return 0;
    memset(&defaults, 0, sizeof defaults);
    memset(&cx, 0, sizeof cx);
    cx.d = d;
    cx.opt = opt ? opt : &defaults;
    for (i = 0; i < d->nmodules; i++) {
        IrModule *m = d->modules[i];
        ChkMod cm;
        if (!m)
            continue;
        memset(&cm, 0, sizeof cm);
        cm.cx = &cx;
        cm.m = m;
        cm.inst_wire_inst = ANEWN(&cx.arena, uint32_t, m->nsigs + 1);
        if (m->is_extern) {
            /* black box: no combinational paths known */
            ChkSummary *sum = ANEW(&cx.arena, ChkSummary);
            sum->nports = 0;
            map_put(&cx.summaries, MAP_PTR(m), MAP_PTR(sum));
            continue;
        }
        diag_push_path(m->display ? m->display : m->name);
        chk_mod_build(&cm);
        chk_drivers(&cm);
        chk_assign(&cm);
        chk_loops(&cm);
        chk_summary(&cm);
        chk_cdc(&cm);
        if (cx.opt->unused)
            chk_unused(&cm);
        diag_pop_path();
        vec_free(cm.nodes);
        vec_free(cm.reads);
        vec_free(cm.ranges);
    }
    chk_crossings(&cx);
    map_free(&cx.summaries);
    map_free(&cx.seen);
    map_free(&cx.cdc_mods);
    arena_release(&cx.arena);
    return cx.errors;
}
