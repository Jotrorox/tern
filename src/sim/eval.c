/* eval.c — the simulator's execution engine: the instruction interpreter,
 * combinational settle, clock edges (hold/evaluate/commit), resets, the
 * harness API (step, run_ns, reset, poke/peek), immediate assertions,
 * property monitors (formula progression) and coverage. */
#include "sim_impl.h"

#define EV_LIMBS(w) (((w) + 63u) / 64u)

/* --------------------------------------------------------- interpreter -- */

static int ev_parity(uint64_t x)
{
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (int)(x & 1u);
}

static void ev_memwrite(SimModel *m, const SimInsn *in)
{
    SimPendWrite pw;
    uint64_t *v = m->v;
    uint32_t dl = EV_LIMBS(in->w), i;
    pw.mem = in->p;
    pw.addr = in->aw <= 64 ? v[in->a] : bv_to_u64_sat(v + in->a, in->aw);
    pw.data = (uint32_t)m->pend_data.len;
    for (i = 0; i < dl; i++)
        vec_push(m->pend_data, v[in->b + i]);
    pw.mask = IR_NONE;
    if (in->sub) {
        uint32_t ml = EV_LIMBS(in->bw);
        pw.mask = (uint32_t)m->pend_data.len;
        for (i = 0; i < ml; i++)
            vec_push(m->pend_data, v[in->c + i]);
    }
    vec_push(m->pend, pw);
}

void sim_exec(SimModel *m, uint32_t pc, uint32_t end)
{
    uint64_t *v = m->v;
    const SimInsn *code = m->code.data;
    while (pc < end) {
        const SimInsn *in = &code[pc++];
        switch ((SimOp)in->op) {
        case SOP_COPY: v[in->d] = v[in->a]; break;
        case SOP_SLICE: v[in->d] = (v[in->a] >> in->p) & in->mask; break;
        case SOP_XSLICE: v[in->d] = bv_get64(v + in->a, in->aw, in->p, in->w); break;
        case SOP_INDEX: {
            uint64_t idx = v[in->b];
            v[in->d] = idx < in->aw / in->w ? (v[in->a] >> (idx * in->w)) & in->mask : 0;
            break;
        }
        case SOP_XINDEX: {
            uint64_t idx = v[in->b];
            v[in->d] = idx < in->aw / in->w ? bv_get64(v + in->a, in->aw, idx * in->w, in->w) : 0;
            break;
        }
        case SOP_NOT: v[in->d] = ~v[in->a] & in->mask; break;
        case SOP_NEG: v[in->d] = (0 - v[in->a]) & in->mask; break;
        case SOP_LNOT: v[in->d] = v[in->a] == 0; break;
        case SOP_RAND: v[in->d] = v[in->a] == in->mask; break;
        case SOP_ROR: v[in->d] = v[in->a] != 0; break;
        case SOP_RXOR: v[in->d] = (uint64_t)ev_parity(v[in->a]); break;
        case SOP_ADD: v[in->d] = (v[in->a] + v[in->b]) & in->mask; break;
        case SOP_SUB: v[in->d] = (v[in->a] - v[in->b]) & in->mask; break;
        case SOP_MUL: v[in->d] = (v[in->a] * v[in->b]) & in->mask; break;
        case SOP_DIVU: {
            uint64_t y = v[in->b];
            v[in->d] = y ? v[in->a] / y : 0;
            break;
        }
        case SOP_MODU: {
            uint64_t y = v[in->b];
            v[in->d] = y ? v[in->a] % y : v[in->a];
            break;
        }
        case SOP_DIVS:
        case SOP_MODS: {
            uint64_t x = v[in->a], y = v[in->b], sb = UINT64_C(1) << (in->w - 1), r;
            int sx = (x & sb) != 0, sy = (y & sb) != 0;
            uint64_t ux, uy;
            if (!y) {
                v[in->d] = in->op == SOP_DIVS ? 0 : x;
                break;
            }
            ux = sx ? (0 - x) & in->mask : x;
            uy = sy ? (0 - y) & in->mask : y;
            if (in->op == SOP_DIVS) {
                r = ux / uy;
                if (sx != sy)
                    r = 0 - r;
            } else {
                r = ux % uy;
                if (sx)
                    r = 0 - r;
            }
            v[in->d] = r & in->mask;
            break;
        }
        case SOP_AND: v[in->d] = v[in->a] & v[in->b]; break;
        case SOP_OR: v[in->d] = v[in->a] | v[in->b]; break;
        case SOP_XOR: v[in->d] = v[in->a] ^ v[in->b]; break;
        case SOP_SHL: {
            uint64_t s = v[in->b];
            v[in->d] = s >= in->w ? 0 : (v[in->a] << s) & in->mask;
            break;
        }
        case SOP_SHR: {
            uint64_t s = v[in->b];
            v[in->d] = s >= in->w ? 0 : v[in->a] >> s;
            break;
        }
        case SOP_ASHR: {
            uint64_t s = v[in->b], x = v[in->a];
            int sign = (int)((x >> (in->w - 1)) & 1u);
            if (s >= in->w)
                v[in->d] = sign ? in->mask : 0;
            else
                v[in->d] = (x >> s) | (sign ? (in->mask & ~(in->mask >> s)) : 0);
            break;
        }
        case SOP_EQ: v[in->d] = v[in->a] == v[in->b]; break;
        case SOP_NE: v[in->d] = v[in->a] != v[in->b]; break;
        case SOP_LTU: v[in->d] = v[in->a] < v[in->b]; break;
        case SOP_LEU: v[in->d] = v[in->a] <= v[in->b]; break;
        case SOP_LTS: {
            uint64_t sb = UINT64_C(1) << (in->aw - 1);
            v[in->d] = (v[in->a] ^ sb) < (v[in->b] ^ sb);
            break;
        }
        case SOP_LES: {
            uint64_t sb = UINT64_C(1) << (in->aw - 1);
            v[in->d] = (v[in->a] ^ sb) <= (v[in->b] ^ sb);
            break;
        }
        case SOP_LAND: v[in->d] = (v[in->a] & v[in->b]) & 1u; break;
        case SOP_LOR: v[in->d] = (v[in->a] | v[in->b]) & 1u; break;
        case SOP_SHLOR:
            v[in->d] = ((in->p >= 64 ? 0 : v[in->a] << in->p) | v[in->b]) & in->mask;
            break;
        case SOP_MUX: v[in->d] = v[in->a] ? v[in->b] : v[in->c]; break;
        case SOP_SEXT: {
            uint64_t sb = UINT64_C(1) << (in->aw - 1);
            v[in->d] = ((v[in->a] ^ sb) - sb) & in->mask;
            break;
        }
        case SOP_DEPOSIT: {
            uint64_t mk = in->mask << in->p;
            v[in->d] = (v[in->d] & ~mk) | ((v[in->a] << in->p) & mk);
            break;
        }
        case SOP_DEPOSIT_DYN: {
            uint64_t idx = v[in->b], lo;
            if (in->p + in->aw > in->w || idx > (uint64_t)(in->w - in->p - in->aw) / in->aw)
                break;
            lo = in->p + idx * in->aw;
            {
                uint64_t mk = in->mask << lo;
                v[in->d] = (v[in->d] & ~mk) | ((v[in->a] << lo) & mk);
            }
            break;
        }
        case SOP_WCOPY: bv_copy(v + in->d, v + in->a, in->w); break;
        case SOP_WZERO: bv_zero(v + in->d, in->w); break;
        case SOP_WEXTRACT: bv_extract(v + in->d, in->w, v + in->a, in->aw, in->p); break;
        case SOP_WINDEX: {
            uint64_t idx = in->bw <= 64 ? v[in->b] : bv_to_u64_sat(v + in->b, in->bw);
            if (idx < in->aw / in->w)
                bv_extract(v + in->d, in->w, v + in->a, in->aw, idx * in->w);
            else
                bv_zero(v + in->d, in->w);
            break;
        }
        case SOP_WUN:
            switch ((IrOp)in->sub) {
            case IROP_NOT: bv_not(v + in->d, v + in->a, in->w); break;
            case IROP_NEG: bv_neg(v + in->d, v + in->a, in->w); break;
            case IROP_RAND: v[in->d] = (uint64_t)bv_red_and(v + in->a, in->aw); break;
            case IROP_ROR: v[in->d] = (uint64_t)bv_red_or(v + in->a, in->aw); break;
            case IROP_RXOR: v[in->d] = (uint64_t)bv_red_xor(v + in->a, in->aw); break;
            default: v[in->d] = v[in->a] == 0; break;
            }
            break;
        case SOP_WBIN: {
            uint64_t *d = v + in->d;
            const uint64_t *a = v + in->a, *b = v + in->b;
            switch ((IrOp)in->sub) {
            case IROP_ADD: bv_add(d, a, b, in->w); break;
            case IROP_SUB: bv_sub(d, a, b, in->w); break;
            case IROP_MUL: bv_mul(d, a, b, in->w, m->scratch); break;
            case IROP_DIV: bv_divmod(d, NULL, a, b, in->w, in->sgn, m->scratch); break;
            case IROP_MOD: bv_divmod(NULL, d, a, b, in->w, in->sgn, m->scratch); break;
            case IROP_AND: bv_and(d, a, b, in->w); break;
            case IROP_OR: bv_or(d, a, b, in->w); break;
            case IROP_XOR: bv_xor(d, a, b, in->w); break;
            case IROP_SHL:
            case IROP_SHR:
            case IROP_ASHR: {
                uint64_t s = in->bw <= 64 ? b[0] : bv_to_u64_sat(b, in->bw);
                if (in->sub == IROP_SHL)
                    bv_shl(d, a, in->w, s);
                else if (in->sub == IROP_SHR)
                    bv_shr(d, a, in->w, s);
                else
                    bv_ashr(d, a, in->w, s);
                break;
            }
            case IROP_LAND: d[0] = (a[0] & b[0]) & 1u; break;
            case IROP_LOR: d[0] = (a[0] | b[0]) & 1u; break;
            default: break;
            }
            break;
        }
        case SOP_WCMP: {
            int r = bv_cmp(v + in->a, v + in->b, in->aw, in->sgn);
            switch ((IrOp)in->sub) {
            case IROP_EQ: v[in->d] = r == 0; break;
            case IROP_NE: v[in->d] = r != 0; break;
            case IROP_LT: v[in->d] = r < 0; break;
            default: v[in->d] = r <= 0; break;
            }
            break;
        }
        case SOP_WMUX: bv_copy(v + in->d, v[in->a] ? v + in->b : v + in->c, in->w); break;
        case SOP_WRESIZE: bv_resize(v + in->d, in->w, v + in->a, in->aw, in->sgn); break;
        case SOP_WDEPOSIT: bv_deposit(v + in->d, in->w, in->p, v + in->a, in->aw); break;
        case SOP_WDEPOSIT_DYN: {
            uint64_t idx = in->bw <= 64 ? v[in->b] : bv_to_u64_sat(v + in->b, in->bw);
            if (in->p + in->aw > in->w || idx > (uint64_t)(in->w - in->p - in->aw) / in->aw)
                break;
            bv_deposit(v + in->d, in->w, in->p + idx * in->aw, v + in->a, in->aw);
            break;
        }
        case SOP_MEMREAD: {
            const SimMem *mm = &m->mems.data[in->p];
            uint64_t addr = in->aw <= 64 ? v[in->a] : bv_to_u64_sat(v + in->a, in->aw);
            if (addr < mm->depth) {
                if (mm->limbs == 1)
                    v[in->d] = mm->data[addr];
                else
                    memcpy(v + in->d, mm->data + addr * mm->limbs, sizeof(uint64_t) * mm->limbs);
            } else {
                bv_zero(v + in->d, in->w);
            }
            break;
        }
        case SOP_MEMWRITE:
            ev_memwrite(m, in);
            v = m->v;
            break;
        case SOP_JMP: pc = in->p; break;
        case SOP_JZ:
            if (!v[in->a])
                pc = in->p;
            break;
        case SOP_SWITCH: {
            const SimSwitch *t = &m->switches.data[in->p];
            uint64_t x = v[in->a];
            uint32_t lo = 0, hi = t->n;
            pc = t->dflt;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                if (t->vals[mid] < x)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < t->n && t->vals[lo] == x)
                pc = t->targets[lo];
            break;
        }
        case SOP_ASSERT: sim_assert_hit(m, in->p, v[in->a]); break;
        case SOP_COV: m->cov[in->p]++; break;
        }
    }
}

/* -------------------------------------------------------- reporting -- */

static uint64_t ev_dom_cycle(const SimModel *m, uint32_t fd)
{
    const SimGroup *g;
    if (fd == IR_NONE)
        return m->default_clock != IR_NONE ? m->clocks.data[m->default_clock].cycles : 0;
    g = &m->groups.data[m->doms.data[fd].group];
    return g->clock != IR_NONE ? m->clocks.data[g->clock].cycles : g->edges;
}

static void ev_report(SimModel *m, SimFailure *f)
{
    m->nfail++;
    f->time_ps = m->now_ps;
    if (m->fail_fn) {
        m->fail_fn(m->fail_user, f);
    } else {
        const SourceFile *sf = src_get(f->loc.file);
        fprintf(stderr, "%s:%u:%u: %s (cycle %llu%s%s)\n", sf ? sf->path : "<design>", f->loc.line,
                f->loc.col, f->what, (unsigned long long)f->cycle, *f->inst_path ? ", in " : "",
                f->inst_path);
    }
}

void sim_assert_hit(SimModel *m, uint32_t id, uint64_t val)
{
    const SimAssert *a = &m->asserts.data[id];
    SimFailure f;
    StrBuf sb = {0};
    if (a->fd != IR_NONE) {
        if (m->doms.data[a->fd].in_reset)
            return;
    } else if (m->harness_reset || m->in_init) {
        return;
    }
    if (a->kind == IRA_COVER) {
        if (val && a->cov != IR_NONE)
            m->cov[a->cov]++;
        return;
    }
    if (val)
        return;
    memset(&f, 0, sizeof f);
    f.kind = a->kind == IRA_ASSUME ? SIM_FAIL_ASSUME : SIM_FAIL_ASSERT;
    sb_puts(&sb, a->kind == IRA_ASSUME ? "assumption violated" : "assertion failed");
    if (a->msg)
        sb_printf(&sb, ": %s", a->msg);
    f.what = sb_cstr(&sb);
    f.msg = a->msg;
    f.loc = a->loc;
    f.cycle = ev_dom_cycle(m, a->fd);
    f.start_cycle = f.cycle;
    f.inst_path = m->insts.data[a->inst].path;
    ev_report(m, &f);
    sb_free(&sb);
}

/* -------------------------------------------------- property engine -- */
/* Formula progression: each attempt is a formula; at every edge of its
 * domain it is rewritten against the sampled values into TRUE, FALSE or a
 * residual formula for the next cycle.  Residual nodes live in one of the
 * group's two pools (the one being written this tick, the other holding the
 * attempts of the previous tick) and are hash-consed, so identical
 * obligations collapse and the attempt sets stay small. */

typedef struct EvSeq {
    int matched;
    uint32_t rest;
} EvSeq;

typedef struct EvProg {
    SimModel *m;
    SimGroup *g;
    SimPPool *old;
    SimPPool *nw;
} EvProg;

static const SimPNode *ev_old(const EvProg *p, uint32_t ref)
{
    if (ref & SIM_PDYN)
        return &p->old->nodes.data[ref & ~SIM_PDYN];
    return &p->m->pstatic.data[ref];
}

static const SimPNode *ev_new(const EvProg *p, uint32_t ref)
{
    if (ref & SIM_PDYN)
        return &p->nw->nodes.data[ref & ~SIM_PDYN];
    return &p->m->pstatic.data[ref];
}

static uint64_t ev_node_hash(uint8_t k, uint32_t a, uint32_t b, uint32_t lo, uint32_t hi)
{
    uint64_t h = hash_u64(k);
    h = hash_combine(h, a);
    h = hash_combine(h, b);
    h = hash_combine(h, lo);
    return hash_combine(h, hi);
}

static void ev_pool_rehash(SimPPool *pl, uint32_t ncap)
{
    uint32_t i;
    free(pl->table);
    pl->table = (uint32_t *)xcalloc(ncap, sizeof(uint32_t));
    pl->cap = ncap;
    for (i = 0; i < pl->nodes.len; i++) {
        const SimPNode *n = &pl->nodes.data[i];
        uint32_t s = (uint32_t)ev_node_hash(n->kind, n->a, n->b, n->lo, n->hi) & (ncap - 1);
        while (pl->table[s])
            s = (s + 1) & (ncap - 1);
        pl->table[s] = i + 1;
    }
}

static void ev_pool_clear(SimPPool *pl)
{
    pl->nodes.len = 0;
    if (pl->table)
        memset(pl->table, 0, sizeof(uint32_t) * pl->cap);
}

static uint32_t ev_mk(EvProg *p, uint8_t k, uint32_t a, uint32_t b, uint32_t lo, uint32_t hi)
{
    SimPPool *pl = p->nw;
    uint32_t s;
    SimPNode n;
    if (!pl->table || (pl->nodes.len + 1) * 2 > pl->cap)
        ev_pool_rehash(pl, pl->cap ? pl->cap * 2 : 64);
    s = (uint32_t)ev_node_hash(k, a, b, lo, hi) & (pl->cap - 1);
    while (pl->table[s]) {
        const SimPNode *x = &pl->nodes.data[pl->table[s] - 1];
        if (x->kind == k && x->a == a && x->b == b && x->lo == lo && x->hi == hi)
            return (pl->table[s] - 1) | SIM_PDYN;
        s = (s + 1) & (pl->cap - 1);
    }
    memset(&n, 0, sizeof n);
    n.kind = k;
    n.a = a;
    n.b = b;
    n.lo = lo;
    n.hi = hi;
    vec_push(pl->nodes, n);
    pl->table[s] = (uint32_t)pl->nodes.len;
    return ((uint32_t)pl->nodes.len - 1) | SIM_PDYN;
}

static int ev_is_true(uint32_t r)
{
    return r == SPK_TRUE || r == SPK_VAC;
}

static uint32_t ev_and(EvProg *p, uint32_t x, uint32_t y)
{
    if (x == SPK_FALSE || y == SPK_FALSE)
        return SPK_FALSE;
    if (ev_is_true(x) && ev_is_true(y))
        return (x == SPK_TRUE || y == SPK_TRUE) ? SPK_TRUE : SPK_VAC;
    if (ev_is_true(x))
        return y;
    if (ev_is_true(y))
        return x;
    if (x == y)
        return x;
    return ev_mk(p, SPK_AND, x, y, 0, 0);
}

static uint32_t ev_or(EvProg *p, uint32_t x, uint32_t y)
{
    if (x == SPK_TRUE || y == SPK_TRUE)
        return SPK_TRUE;
    if (x == SPK_VAC || y == SPK_VAC)
        return SPK_VAC;
    if (x == SPK_FALSE)
        return y;
    if (y == SPK_FALSE)
        return x;
    if (x == y)
        return x;
    return ev_mk(p, SPK_OR, x, y, 0, 0);
}

static uint32_t ev_not(EvProg *p, uint32_t x)
{
    if (ev_is_true(x))
        return SPK_FALSE;
    if (x == SPK_FALSE)
        return SPK_TRUE;
    if (ev_new(p, x)->kind == SPK_NOT)
        return ev_new(p, x)->a;
    return ev_mk(p, SPK_NOT, x, 0, 0, 0);
}

static uint32_t ev_dec(uint32_t hi)
{
    return hi == SIM_PINF ? SIM_PINF : hi - 1;
}

static uint32_t ev_prop(EvProg *p, uint32_t ref, int depth);

static EvSeq ev_seq(EvProg *p, uint32_t ref, int depth)
{
    const SimPNode *n = ev_old(p, ref);
    EvSeq r, sa, sb;
    r.matched = 0;
    r.rest = SPK_FALSE;
    if (depth > 4096)
        return r;
    switch (n->kind) {
    case SPK_TRUE:
    case SPK_VAC: r.matched = 1; return r;
    case SPK_FALSE: return r;
    case SPK_EXPR: r.matched = (p->m->v[n->leaf] & 1u) != 0; return r;
    case SPK_CAT: {
        uint32_t a = n->a, b = n->b, lo = n->lo, hi = n->hi;
        sa = ev_seq(p, a, depth + 1);
        if (sa.rest != SPK_FALSE)
            r.rest = ev_mk(p, SPK_CAT, sa.rest, b, lo, hi);
        if (sa.matched) {
            if (lo == 0) {
                sb = ev_seq(p, b, depth + 1);
                r.matched |= sb.matched;
                r.rest = ev_or(p, r.rest, sb.rest);
            }
            if (hi == SIM_PINF || hi > 0)
                r.rest = ev_or(p, r.rest, ev_mk(p, SPK_SKIP, 0, b, lo ? lo - 1 : 0, ev_dec(hi)));
        }
        return r;
    }
    case SPK_SKIP: {
        uint32_t b = n->b, lo = n->lo, hi = n->hi;
        if (lo == 0) {
            sb = ev_seq(p, b, depth + 1);
            r.matched = sb.matched;
            r.rest = sb.rest;
            if (hi == SIM_PINF || hi > 0)
                r.rest = ev_or(p, r.rest, ev_mk(p, SPK_SKIP, 0, b, 0, ev_dec(hi)));
        } else {
            r.rest = ev_mk(p, SPK_SKIP, 0, b, lo - 1, ev_dec(hi));
        }
        return r;
    }
    case SPK_OR: {
        uint32_t a = n->a, b = n->b;
        sa = ev_seq(p, a, depth + 1);
        sb = ev_seq(p, b, depth + 1);
        r.matched = sa.matched || sb.matched;
        r.rest = ev_or(p, sa.rest, sb.rest);
        return r;
    }
    case SPK_AND: {
        uint32_t a = n->a, b = n->b;
        sa = ev_seq(p, a, depth + 1);
        sb = ev_seq(p, b, depth + 1);
        r.matched = sa.matched && sb.matched;
        if (sa.rest != SPK_FALSE && sb.rest != SPK_FALSE)
            r.rest = ev_mk(p, SPK_AND, sa.rest, sb.rest, 0, 0);
        return r;
    }
    default: {
        uint32_t x = ev_prop(p, ref, depth + 1);
        r.matched = ev_is_true(x);
        return r;
    }
    }
}

static uint32_t ev_prop(EvProg *p, uint32_t ref, int depth)
{
    const SimPNode *n = ev_old(p, ref);
    uint8_t k = n->kind;
    uint32_t a = n->a, b = n->b, hi = n->hi;
    if (depth > 4096)
        return SPK_FALSE;
    switch (k) {
    case SPK_TRUE:
    case SPK_FALSE:
    case SPK_VAC: return ref;
    case SPK_EXPR:
    case SPK_CAT:
    case SPK_SKIP: {
        EvSeq s = ev_seq(p, ref, depth + 1);
        return s.matched ? SPK_TRUE : s.rest;
    }
    case SPK_IMPL:
    case SPK_IMPLN: {
        EvSeq s = ev_seq(p, a, depth + 1);
        uint32_t cont = s.rest == SPK_FALSE ? SPK_VAC : ev_mk(p, k, s.rest, b, 0, 0);
        if (!s.matched)
            return cont;
        /* |=> : the consequent starts next cycle (an unprogressed static node) */
        return ev_and(p, k == SPK_IMPL ? ev_prop(p, b, depth + 1) : b, cont);
    }
    case SPK_ALWAYS: return ev_and(p, ev_prop(p, a, depth + 1), ref);
    case SPK_NEVER: return ev_and(p, ev_not(p, ev_prop(p, a, depth + 1)), ref);
    case SPK_EV: {
        uint32_t x = ev_prop(p, a, depth + 1), rest;
        if (ev_is_true(x))
            return SPK_TRUE;
        if (hi == SIM_PINF)
            rest = ref;
        else if (hi == 0)
            rest = SPK_FALSE;
        else
            rest = ev_mk(p, SPK_EV, a, 0, 0, hi - 1);
        return ev_or(p, x, rest);
    }
    case SPK_UNTIL: {
        uint32_t xb = ev_prop(p, b, depth + 1);
        if (ev_is_true(xb))
            return SPK_TRUE;
        return ev_or(p, xb, ev_and(p, ev_prop(p, a, depth + 1), ref));
    }
    case SPK_AND: {
        uint32_t x = ev_prop(p, a, depth + 1);
        if (x == SPK_FALSE)
            return SPK_FALSE;
        return ev_and(p, x, ev_prop(p, b, depth + 1));
    }
    case SPK_OR: {
        uint32_t x = ev_prop(p, a, depth + 1);
        if (x == SPK_TRUE)
            return SPK_TRUE;
        return ev_or(p, x, ev_prop(p, b, depth + 1));
    }
    case SPK_NOT: return ev_not(p, ev_prop(p, a, depth + 1));
    }
    return SPK_FALSE;
}

/* add residual `ref` (splitting top-level conjunctions) to the attempt set;
 * the parent itself keeps its start cycle, anything else gets `start` */
static void ev_push_attempt(EvProg *p, SimAttemptVec *out, uint32_t ref, uint32_t parent,
                            uint64_t parent_start, uint64_t start)
{
    const SimPNode *n = ev_new(p, ref);
    uint64_t *slot;
    int inserted;
    if (n->kind == SPK_AND) {
        uint32_t a = n->a, b = n->b;
        ev_push_attempt(p, out, a, parent, parent_start, start);
        ev_push_attempt(p, out, b, parent, parent_start, start);
        return;
    }
    if (ref == parent)
        start = parent_start;
    slot = map_ref(&p->m->pseen, ref, &inserted);
    if (!inserted) {
        if (out->data[*slot].start > start)
            out->data[*slot].start = start;
        return;
    }
    *slot = out->len;
    {
        SimAttempt at;
        at.ref = ref;
        at.start = start;
        vec_push(*out, at);
    }
}

static void ev_mon_fail(SimModel *m, const SimMon *mon, uint64_t start, int pending)
{
    SimFailure f;
    StrBuf sb = {0};
    const char *name = mon->prop->name ? mon->prop->name : "_";
    memset(&f, 0, sizeof f);
    if (pending) {
        f.kind = SIM_FAIL_PENDING;
        sb_printf(&sb, "property `%s`: eventually still pending at end of test", name);
    } else if (mon->prop->kind == IRA_ASSUME) {
        f.kind = SIM_FAIL_ASSUME;
        sb_printf(&sb, "assumption violated: property `%s`", name);
    } else {
        f.kind = SIM_FAIL_PROPERTY;
        sb_printf(&sb, "property `%s` failed", name);
    }
    f.what = sb_cstr(&sb);
    f.property = name;
    f.loc = mon->prop->loc;
    f.cycle = ev_dom_cycle(m, mon->fd);
    f.start_cycle = start;
    f.inst_path = m->insts.data[mon->inst].path;
    ev_report(m, &f);
    sb_free(&sb);
}

static void ev_props_tick(SimModel *m, uint32_t gi)
{
    SimGroup *g = &m->groups.data[gi];
    EvProg p;
    uint32_t i, j;
    if (!g->mons.len)
        return;
    sim_exec(m, g->pcode_lo, g->pcode_hi);
    p.m = m;
    p.g = g;
    p.old = &g->pool[g->cur_pool];
    p.nw = &g->pool[1 - g->cur_pool];
    ev_pool_clear(p.nw);
    for (i = 0; i < g->mons.len; i++) {
        SimMon *mon = &m->mons.data[g->mons.data[i]];
        SimAttemptVec next = {0};
        uint64_t cyc = ev_dom_cycle(m, mon->fd);
        int cover = mon->prop->kind == IRA_COVER;
        if (m->doms.data[mon->fd].in_reset) {
            mon->att.len = 0;
            mon->started = 0;
            continue;
        }
        if (!mon->started || cover) {
            SimAttempt at;
            at.ref = mon->root;
            at.start = cyc;
            vec_push(mon->att, at);
            mon->started = 1;
        }
        map_clear(&m->pseen);
        for (j = 0; j < mon->att.len; j++) {
            uint32_t ref = mon->att.data[j].ref;
            uint32_t r = ev_prop(&p, ref, 0);
            uint64_t st = mon->att.data[j].start;
            if (r == SPK_TRUE) {
                if (cover)
                    mon->hits++;
            } else if (r == SPK_VAC) {
                /* vacuous success */
            } else if (r == SPK_FALSE) {
                if (!cover)
                    ev_mon_fail(m, mon, st, 0);
            } else {
                /* obligations spawned by a static `always`/`never` start now */
                int spawn = !(ref & SIM_PDYN) && (m->pstatic.data[ref].kind == SPK_ALWAYS ||
                                                 m->pstatic.data[ref].kind == SPK_NEVER);
                ev_push_attempt(&p, &next, r, ref, st, spawn ? cyc : st);
            }
        }
        vec_free(mon->att);
        mon->att = next;
    }
    g->cur_pool = 1 - g->cur_pool;
}

static int ev_pending(const SimModel *m, const SimPPool *pl, uint32_t ref, int depth)
{
    const SimPNode *n = (ref & SIM_PDYN) ? &pl->nodes.data[ref & ~SIM_PDYN] : &m->pstatic.data[ref];
    if (depth > 4096)
        return 0;
    switch (n->kind) {
    case SPK_EV:
    case SPK_CAT:
    case SPK_SKIP: return n->hi == SIM_PINF;
    case SPK_AND:
    case SPK_OR: return ev_pending(m, pl, n->a, depth + 1) || ev_pending(m, pl, n->b, depth + 1);
    case SPK_NOT: return ev_pending(m, pl, n->a, depth + 1);
    default: return 0;
    }
}

uint32_t sim_finish(SimModel *m)
{
    uint32_t i, j, n = 0;
    for (i = 0; i < m->mons.len; i++) {
        SimMon *mon = &m->mons.data[i];
        const SimGroup *g = &m->groups.data[mon->group];
        if (mon->prop->kind == IRA_COVER)
            continue;
        for (j = 0; j < mon->att.len; j++)
            if (ev_pending(m, &g->pool[g->cur_pool], mon->att.data[j].ref, 0)) {
                ev_mon_fail(m, mon, mon->att.data[j].start, 1);
                n++;
            }
    }
    return n;
}

void sim_mon_reset_all(SimModel *m)
{
    uint32_t i;
    for (i = 0; i < m->mons.len; i++) {
        m->mons.data[i].att.len = 0;
        m->mons.data[i].started = 0;
    }
}

/* ---------------------------------------------------- settle / edges -- */

static int ev_reset_active(const SimModel *m, const SimDom *d)
{
    uint64_t val;
    if (d->reset == IR_NONE)
        return 0;
    val = m->v[m->sigs.data[d->reset].off] & 1u;
    return d->low ? val == 0 : val == 1;
}

/* async resets act without an edge; returns 1 if a register changed */
static int ev_apply_async(SimModel *m)
{
    uint32_t i, k;
    int changed = 0;
    for (i = 0; i < m->doms.len; i++) {
        const SimDom *d = &m->doms.data[i];
        if (!d->async || !ev_reset_active(m, d))
            continue;
        for (k = 0; k < d->init_n; k++) {
            const SimInitReg *r = &m->initregs.data[d->init_lo + k];
            if (memcmp(m->v + r->cur, m->v + r->init, sizeof(uint64_t) * r->limbs) != 0) {
                memcpy(m->v + r->cur, m->v + r->init, sizeof(uint64_t) * r->limbs);
                changed = 1;
            }
        }
    }
    return changed;
}

static void ev_fire(SimModel *m, const uint32_t *gs, uint32_t ng);

void sim_settle_now(SimModel *m)
{
    int iter;
    uint32_t i;
    if (!m->dirty)
        return;
    for (iter = 0; iter < 32; iter++) {
        m->dirty = 0;
        sim_exec(m, 0, m->comb_end);
        if (!ev_apply_async(m))
            break;
    }
    /* derived clocks (clock dividers, gated clocks): edges seen after settle */
    if (m->derived_depth < 64) {
        for (i = 0; i < m->derived.len; i++) {
            uint32_t gi = m->derived.data[i];
            SimGroup *g = &m->groups.data[gi];
            int val = (int)(m->v[m->sigs.data[g->root].off] & 1u);
            if (val == g->last)
                continue;
            g->last = val;
            if (m->in_init || val != g->level)
                continue;
            m->derived_depth++;
            ev_fire(m, &gi, 1);
            sim_settle_now(m);
            m->derived_depth--;
        }
    }
}

static void ev_apply_writes(SimModel *m)
{
    uint32_t i, k;
    for (i = 0; i < m->pend.len; i++) {
        const SimPendWrite *pw = &m->pend.data[i];
        SimMem *mm = &m->mems.data[pw->mem];
        uint64_t *dst;
        const uint64_t *src = m->pend_data.data + pw->data;
        if (pw->addr >= mm->depth)
            continue;
        dst = mm->data + pw->addr * mm->limbs;
        if (pw->mask == IR_NONE) {
            memcpy(dst, src, sizeof(uint64_t) * mm->limbs);
        } else {
            const uint64_t *mk = m->pend_data.data + pw->mask;
            uint32_t nbytes = (mm->width + 7) / 8;
            for (k = 0; k < nbytes; k++)
                if ((mk[k / 64] >> (k % 64)) & 1u) {
                    uint32_t bits = mm->width - 8 * k < 8 ? mm->width - 8 * k : 8;
                    uint64_t byte = bv_get64(src, mm->width, (uint64_t)8 * k, bits);
                    bv_deposit(dst, mm->width, (uint64_t)8 * k, &byte, bits);
                }
        }
    }
    m->pend.len = 0;
    m->pend_data.len = 0;
}

/* evaluate the clocked logic of the given groups against the current
 * (pre-edge) values, then commit all of them together */
static void ev_fire(SimModel *m, const uint32_t *gs, uint32_t ng)
{
    uint32_t i, j, k;
    uint64_t *v = m->v;
    for (i = 0; i < ng; i++) {
        SimGroup *g = &m->groups.data[gs[i]];
        memcpy(v + g->next_lo, v + g->cur_lo, sizeof(uint64_t) * g->nlimbs);
        for (j = 0; j < g->doms.len; j++) {
            SimDom *d = &m->doms.data[g->doms.data[j]];
            d->in_reset = ev_reset_active(m, d);
        }
    }
    for (i = 0; i < ng; i++) {
        SimGroup *g = &m->groups.data[gs[i]];
        for (j = 0; j < g->doms.len; j++) {
            SimDom *d = &m->doms.data[g->doms.data[j]];
            sim_exec(m, d->code_lo, d->code_hi);
            if (d->in_reset)
                for (k = 0; k < d->init_n; k++) {
                    const SimInitReg *r = &m->initregs.data[d->init_lo + k];
                    memcpy(v + r->next, v + r->init, sizeof(uint64_t) * r->limbs);
                }
        }
        ev_props_tick(m, gs[i]);
    }
    for (i = 0; i < ng; i++) {
        SimGroup *g = &m->groups.data[gs[i]];
        memcpy(v + g->cur_lo, v + g->next_lo, sizeof(uint64_t) * g->nlimbs);
        g->edges++;
        for (j = 0; j < g->doms.len; j++)
            m->doms.data[g->doms.data[j]].in_reset = 0;
    }
    ev_apply_writes(m);
    m->dirty = 1;
}

static void ev_set_clock(SimModel *m, SimClock *c, int level)
{
    uint32_t i;
    c->level = level;
    m->v[m->sigs.data[c->sig].off] = (uint64_t)level;
    for (i = 0; i < c->copies.len; i++)
        m->v[m->sigs.data[c->copies.data[i]].off] = (uint64_t)level;
}

/* toggle a set of top clocks at the same instant */
static void ev_toggle(SimModel *m, const uint32_t *cs, uint32_t nc)
{
    uint32_t gs[64], ng = 0, i;
    sim_settle_now(m);
    for (i = 0; i < nc; i++) {
        SimClock *c = &m->clocks.data[cs[i]];
        int nl = !c->level, active = nl == (c->falling ? 0 : 1);
        uint32_t g = active ? c->g_active : c->g_inactive;
        if (g != IR_NONE && ng < 64)
            gs[ng++] = g;
    }
    if (ng)
        ev_fire(m, gs, ng);
    for (i = 0; i < nc; i++) {
        SimClock *c = &m->clocks.data[cs[i]];
        int nl = !c->level;
        ev_set_clock(m, c, nl);
        if (nl == (c->falling ? 0 : 1))
            c->cycles++;
    }
    sim_settle_now(m);
    sim_vcd_sample(m);
}

static void ev_step_clock(SimModel *m, uint32_t ci, uint32_t n)
{
    uint32_t k, h;
    for (k = 0; k < n; k++)
        for (h = 0; h < 2; h++) {
            SimClock *c = &m->clocks.data[ci];
            m->now_ps += c->half_ps;
            ev_toggle(m, &ci, 1);
            m->clocks.data[ci].next_toggle = m->now_ps + m->clocks.data[ci].half_ps;
        }
}

/* ---------------------------------------------------------- lifecycle -- */

SimModel *sim_new(const IrDesign *d, const IrModule *top, uint64_t seed)
{
    SimModel *m;
    uint32_t i;
    if (!top)
        top = d ? d->top : NULL;
    if (!top) {
        diag_error(span_none(), "simulation: no top module");
        return NULL;
    }
    m = (SimModel *)xcalloc(1, sizeof(SimModel));
    m->design = d;
    m->top = top;
    m->seed = seed;
    if (!sim_build(m)) {
        sim_free(m);
        return NULL;
    }
    m->dirty = 1;
    m->in_init = 1;
    sim_settle_now(m);
    for (i = 0; i < m->derived.len; i++) {
        SimGroup *g = &m->groups.data[m->derived.data[i]];
        g->last = (int)(m->v[m->sigs.data[g->root].off] & 1u);
    }
    m->in_init = 0;
    return m;
}

void sim_free(SimModel *m)
{
    uint32_t i;
    if (!m)
        return;
    sim_vcd_close(m);
    for (i = 0; i < m->groups.len; i++) {
        SimGroup *g = &m->groups.data[i];
        vec_free(g->doms);
        vec_free(g->mons);
        vec_free(g->pool[0].nodes);
        vec_free(g->pool[1].nodes);
        free(g->pool[0].table);
        free(g->pool[1].table);
    }
    for (i = 0; i < m->clocks.len; i++)
        vec_free(m->clocks.data[i].copies);
    for (i = 0; i < m->mems.len; i++)
        free(m->mems.data[i].data);
    for (i = 0; i < m->mons.len; i++)
        vec_free(m->mons.data[i].att);
    vec_free(m->insts);
    vec_free(m->sigs);
    vec_free(m->mems);
    vec_free(m->doms);
    vec_free(m->groups);
    vec_free(m->clocks);
    vec_free(m->topdoms);
    vec_free(m->initregs);
    vec_free(m->asserts);
    vec_free(m->switches);
    vec_free(m->mons);
    vec_free(m->pstatic);
    vec_free(m->covraw);
    vec_free(m->covout);
    vec_free(m->derived);
    vec_free(m->code);
    vec_free(m->pend);
    vec_free(m->pend_data);
    map_free(&m->by_path);
    map_free(&m->pseen);
    free(m->v);
    free(m->scratch);
    free(m->cov);
    arena_release(&m->arena);
    free(m);
}

/* ------------------------------------------------------------ signals -- */

static const SimSig *ev_sig(const SimModel *m, int h)
{
    if (!m || h < 0 || (size_t)h >= m->sigs.len)
        return NULL;
    return &m->sigs.data[h];
}

int sim_find(SimModel *m, const char *path)
{
    uint64_t v;
    if (!m || !path)
        return -1;
    if (map_get(&m->by_path, MAP_PTR(intern_cstr(path)), &v))
        return (int)v - 1;
    return -1;
}

int sim_find_prefix(SimModel *m, const char *prefix, int *out, int max)
{
    size_t n = strlen(prefix), i;
    int cnt = 0;
    for (i = 0; i < m->sigs.len; i++) {
        const char *p = m->sigs.data[i].path;
        if (strncmp(p, prefix, n) == 0 && p[n] == '.') {
            if (cnt < max && out)
                out[cnt] = (int)i;
            cnt++;
        }
    }
    return cnt;
}

int sim_signal_count(const SimModel *m)
{
    return m ? (int)m->sigs.len : 0;
}

const char *sim_signal_path(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? s->path : NULL;
}

const char *sim_signal_name(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? s->ir->name : NULL;
}

const char *sim_signal_instance(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? m->insts.data[s->inst].path : NULL;
}

const IrSignal *sim_signal_ir(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? s->ir : NULL;
}

const IrType *sim_type(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? s->ir->type : NULL;
}

uint32_t sim_width(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s ? s->width : 0;
}

int sim_is_memory(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s && s->mem != IR_NONE;
}

uint32_t sim_mem_depth(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s && s->mem != IR_NONE ? m->mems.data[s->mem].depth : 0;
}

int sim_is_writable(const SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    return s && s->writable;
}

int sim_poke(SimModel *m, int h, const IrBits *val)
{
    const SimSig *s = ev_sig(m, h);
    if (!s)
        return SIM_ERR_HANDLE;
    if (s->mem != IR_NONE)
        return SIM_ERR_KIND;
    if (!s->writable)
        return SIM_ERR_NOT_WRITABLE;
    if (val->width == 0)
        bv_zero(m->v + s->off, s->width);
    else
        bv_resize(m->v + s->off, s->width, val->w, val->width, 0);
    m->dirty = 1;
    return SIM_OK;
}

int sim_poke_u64(SimModel *m, int h, uint64_t v)
{
    IrBits b;
    b.width = 64;
    b.w = &v;
    return sim_poke(m, h, &b);
}

int sim_peek(SimModel *m, int h, IrBits *out)
{
    const SimSig *s = ev_sig(m, h);
    if (!s)
        return SIM_ERR_HANDLE;
    if (s->mem != IR_NONE)
        return SIM_ERR_KIND;
    out->width = s->width;
    memcpy(out->w, m->v + s->off, sizeof(uint64_t) * s->limbs);
    return SIM_OK;
}

IrBits sim_peek_bits(SimModel *m, int h, Arena *a)
{
    const SimSig *s = ev_sig(m, h);
    IrBits b;
    if (!s || s->mem != IR_NONE) {
        b.width = 0;
        b.w = NULL;
        return b;
    }
    b = ir_bits_zero(a, s->width);
    sim_peek(m, h, &b);
    return b;
}

uint64_t sim_peek_u64(SimModel *m, int h)
{
    const SimSig *s = ev_sig(m, h);
    if (!s || s->mem != IR_NONE)
        return 0;
    return m->v[s->off];
}

int sim_mem_peek(SimModel *m, int h, uint64_t addr, IrBits *out)
{
    const SimSig *s = ev_sig(m, h);
    const SimMem *mm;
    if (!s)
        return SIM_ERR_HANDLE;
    if (s->mem == IR_NONE)
        return SIM_ERR_KIND;
    mm = &m->mems.data[s->mem];
    if (addr >= mm->depth)
        return SIM_ERR_RANGE;
    out->width = mm->width;
    memcpy(out->w, mm->data + addr * mm->limbs, sizeof(uint64_t) * mm->limbs);
    return SIM_OK;
}

int sim_mem_poke(SimModel *m, int h, uint64_t addr, const IrBits *val)
{
    const SimSig *s = ev_sig(m, h);
    SimMem *mm;
    if (!s)
        return SIM_ERR_HANDLE;
    if (s->mem == IR_NONE)
        return SIM_ERR_KIND;
    mm = &m->mems.data[s->mem];
    if (addr >= mm->depth)
        return SIM_ERR_RANGE;
    bv_resize(mm->data + addr * mm->limbs, mm->width, val->w, val->width, 0);
    m->dirty = 1;
    return SIM_OK;
}

/* ------------------------------------------------------------ running -- */

void sim_settle(SimModel *m)
{
    sim_settle_now(m);
    sim_vcd_sample(m);
    sim_vcd_flush(m);
}

int sim_domain_count(const SimModel *m)
{
    return (int)m->topdoms.len;
}

int sim_default_domain(const SimModel *m)
{
    if (m->top->default_domain != IR_NONE && m->top->default_domain < m->topdoms.len)
        return (int)m->top->default_domain;
    return m->topdoms.len ? 0 : -1;
}

int sim_find_domain(const SimModel *m, const char *name)
{
    uint32_t i;
    if (!name || !*name)
        return sim_default_domain(m);
    for (i = 0; i < m->top->ndomains; i++)
        if (tern_streq(m->top->domains[i].name, name))
            return (int)i;
    return -1;
}

const char *sim_domain_name(const SimModel *m, int domain)
{
    if (domain < 0 || (size_t)domain >= m->topdoms.len)
        return NULL;
    return m->top->domains[domain].name;
}

uint64_t sim_domain_hz(const SimModel *m, int domain)
{
    if (domain < 0 || (size_t)domain >= m->topdoms.len)
        return 0;
    return m->clocks.data[m->topdoms.data[domain].clock].hz;
}

void sim_step(SimModel *m, uint32_t domain, uint32_t n)
{
    if (domain >= m->topdoms.len)
        return;
    ev_step_clock(m, m->topdoms.data[domain].clock, n);
    sim_vcd_flush(m);
}

void sim_run_ns(SimModel *m, uint64_t ns)
{
    uint64_t target = m->now_ps + ns * 1000u;
    uint32_t cs[64];
    for (;;) {
        uint64_t t = ~UINT64_C(0);
        uint32_t i, nc = 0;
        for (i = 0; i < m->clocks.len; i++) {
            uint64_t nt = TERN_MAX(m->clocks.data[i].next_toggle, m->now_ps);
            if (nt < t)
                t = nt;
        }
        if (t > target || !m->clocks.len)
            break;
        m->now_ps = t;
        for (i = 0; i < m->clocks.len && nc < 64; i++)
            if (TERN_MAX(m->clocks.data[i].next_toggle, m->now_ps) == t)
                cs[nc++] = i;
        ev_toggle(m, cs, nc);
        for (i = 0; i < nc; i++)
            m->clocks.data[cs[i]].next_toggle = t + m->clocks.data[cs[i]].half_ps;
    }
    m->now_ps = target;
    sim_vcd_flush(m);
}

void sim_reset(SimModel *m)
{
    uint32_t i, j;
    VEC(uint32_t) clks = {0};
    const IrModule *top = m->top;
    m->harness_reset = 1;
    for (i = 0; i < top->nsigs; i++) {
        const IrSignal *s = &top->sigs[i];
        if (s->kind == IRS_IN && s->type->kind == IRT_RESET)
            m->v[m->sigs.data[i].off] = s->type->active_low ? 0u : 1u;
    }
    m->dirty = 1;
    sim_settle_now(m);
    sim_vcd_sample(m);
    for (i = 0; i < top->ndomains; i++) {
        uint32_t c = m->topdoms.data[i].clock;
        if (top->domains[i].reset == IR_NONE)
            continue;
        for (j = 0; j < clks.len; j++)
            if (clks.data[j] == c)
                break;
        if (j == clks.len)
            vec_push(clks, c);
    }
    if (!clks.len)
        for (i = 0; i < m->clocks.len; i++)
            vec_push(clks, i);
    for (i = 0; i < clks.len; i++)
        ev_step_clock(m, clks.data[i], 2);
    vec_free(clks);
    for (i = 0; i < top->nsigs; i++) {
        const IrSignal *s = &top->sigs[i];
        if (s->kind == IRS_IN && s->type->kind == IRT_RESET)
            m->v[m->sigs.data[i].off] = s->type->active_low ? 1u : 0u;
    }
    m->dirty = 1;
    sim_settle_now(m);
    m->harness_reset = 0;
    sim_mon_reset_all(m);
    sim_vcd_sample(m);
    sim_vcd_flush(m);
}

uint64_t sim_cycle(const SimModel *m, uint32_t domain)
{
    if (domain >= m->topdoms.len)
        return 0;
    return m->clocks.data[m->topdoms.data[domain].clock].cycles;
}

uint64_t sim_time_ps(const SimModel *m)
{
    return m->now_ps;
}

void sim_set_fail_handler(SimModel *m, SimFailFn fn, void *user)
{
    m->fail_fn = fn;
    m->fail_user = user;
}

uint32_t sim_fail_count(const SimModel *m)
{
    return m->nfail;
}

/* ----------------------------------------------------------- coverage -- */

static int ev_cov_cmp(const void *pa, const void *pb)
{
    const SimCovPoint *a = (const SimCovPoint *)pa, *b = (const SimCovPoint *)pb;
    if (a->loc.file != b->loc.file)
        return a->loc.file < b->loc.file ? -1 : 1;
    if (a->loc.line != b->loc.line)
        return a->loc.line < b->loc.line ? -1 : 1;
    if (a->loc.col != b->loc.col)
        return a->loc.col < b->loc.col ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->arm != b->arm)
        return a->arm < b->arm ? -1 : 1;
    if (a->loc.off != b->loc.off)
        return a->loc.off < b->loc.off ? -1 : 1;
    return 0;
}

size_t sim_coverage(SimModel *m, const SimCovPoint **out)
{
    size_t i, n = 0;
    m->covout.len = 0;
    for (i = 0; i < m->covraw.len; i++) {
        const SimCovRaw *r = &m->covraw.data[i];
        SimCovPoint p;
        p.kind = r->kind;
        p.loc = r->loc;
        p.arm = r->arm;
        p.name = r->name;
        p.count = r->counter != IR_NONE ? m->cov[r->counter] : m->mons.data[r->mon].hits;
        vec_push(m->covout, p);
    }
    if (m->covout.len)
        qsort(m->covout.data, m->covout.len, sizeof(SimCovPoint), ev_cov_cmp);
    for (i = 0; i < m->covout.len; i++) {
        if (n && ev_cov_cmp(&m->covout.data[n - 1], &m->covout.data[i]) == 0)
            m->covout.data[n - 1].count += m->covout.data[i].count;
        else
            m->covout.data[n++] = m->covout.data[i];
    }
    m->covout.len = n;
    *out = m->covout.data;
    return n;
}

void sim_coverage_reset(SimModel *m)
{
    uint32_t i;
    memset(m->cov, 0, sizeof(uint64_t) * (m->ncov + 1));
    for (i = 0; i < m->mons.len; i++)
        m->mons.data[i].hits = 0;
}

#undef EV_LIMBS
