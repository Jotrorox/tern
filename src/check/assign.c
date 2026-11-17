/* assign.c — the no-latch proof and combinational loops (spec §6.2,
 * §14 decision 4).
 *
 * No latch: for every comb block, every bit of every signal the block
 * assigns must be assigned on every path through it (definite assignment
 * over if / case, bit-accurate for slices, struct fields and constant
 * indices; an assignment with a dynamic index assigns no particular
 * element). A `var` default is the first statement of its block (the
 * elaborator puts it there), so defaulted vars always pass. A `case`
 * without a default arm whose values do not cover the subject has an
 * implicit empty path. The error names the signal and one path that
 * misses it: "`alu_out` may be unassigned when op == Nop".
 *
 * Read before assign: a comb block that reads bits it drives itself
 * before assigning them on every path would read its own output (a
 * combinational loop through a var).
 *
 * Combinational loops: the strongly connected components of the model's
 * combinational graph (continuous assignments, the per-signal parts of
 * comb blocks, and instance outputs with the inputs they depend on
 * combinationally). */
#include "check_impl.h"

typedef struct AsgTarget {
    uint32_t sig;
    uint32_t off;         /* bit offset of the signal in the state bitset */
    uint32_t width;
    uint32_t node;
    int read_early;       /* read-before-assign already reported */
} AsgTarget;

typedef struct AsgCtx {
    ChkMod *cm;
    const IrProcess *p;
    VEC(AsgTarget) targets;
    Map tindex;           /* sig + 1 -> target index + 1 */
    uint32_t words;       /* bitset words */
    VEC(uint64_t *) pool; /* free state bitsets */
    StrBuf path;
    const IrStmt *first_stmt;  /* statement where the reported path diverges */
} AsgCtx;

/* ------------------------------------------------------------ bitsets -- */

static uint64_t *asg_alloc(AsgCtx *a)
{
    uint64_t *b;
    if (a->pool.len)
        b = vec_pop(a->pool);
    else
        b = (uint64_t *)xmalloc((a->words ? a->words : 1) * sizeof(uint64_t));
    return b;
}

static void asg_release(AsgCtx *a, uint64_t *b)
{
    vec_push(a->pool, b);
}

static uint64_t *asg_copy(AsgCtx *a, const uint64_t *src)
{
    uint64_t *b = asg_alloc(a);
    memcpy(b, src, (a->words ? a->words : 1) * sizeof(uint64_t));
    return b;
}

static const AsgTarget *asg_target(AsgCtx *a, uint32_t sig)
{
    uint64_t v;
    if (map_get(&a->tindex, (uint64_t)sig + 1, &v))
        return &a->targets.data[v - 1];
    return NULL;
}

/* ---------------------------------------------------------- case info -- */

static uint64_t asg_case_count_distinct(const IrStmt *s, Map *vals)
{
    uint32_t i, k;
    for (i = 0; i < s->narms; i++)
        for (k = 0; k < s->arms[i].nvals; k++)
            map_put(vals, ir_bits_low64(&s->arms[i].vals[k]) ^ 0x9e3779b97f4a7c15ull, 1);
    return (uint64_t)map_len(vals);
}

static int asg_case_has_default(const IrStmt *s)
{
    uint32_t i;
    for (i = 0; i < s->narms; i++)
        if (s->arms[i].nvals == 0)
            return 1;
    return 0;
}

/* 1 if a case without a default arm covers every value of its subject;
   otherwise *missing describes one uncovered value */
static int asg_case_exhaustive(AsgCtx *a, const IrStmt *s, const char **missing)
{
    Map vals = {0};
    uint64_t n = asg_case_count_distinct(s, &vals);
    uint32_t w = s->subject ? s->subject->width : 1;
    const IrType *t = s->subject ? s->subject->type : NULL;
    int ok = 0;
    *missing = NULL;
    if (t && t->kind == IRT_ENUM && t->tag_width == w) {
        uint32_t v;
        ok = 1;
        for (v = 0; v < t->nvariants; v++)
            if (!map_has(&vals, ir_bits_low64(&t->variants[v].tag) ^ 0x9e3779b97f4a7c15ull)) {
                ok = 0;
                *missing = t->variants[v].name;
                break;
            }
    } else if (w < 64 && n >= ((uint64_t)1 << w)) {
        ok = 1;
    } else if (w >= 64) {
        ok = 0;
    }
    if (!ok && !*missing) {
        uint64_t v;
        for (v = 0; v <= n; v++)
            if (!map_has(&vals, v ^ 0x9e3779b97f4a7c15ull))
                break;
        *missing = arena_printf(&a->cm->cx->arena, "%llu", (unsigned long long)v);
    }
    map_free(&vals);
    return ok;
}

static const char *asg_val_name(AsgCtx *a, const IrStmt *s, const IrBits *v)
{
    const IrType *t = s->subject ? s->subject->type : NULL;
    if (t && t->kind == IRT_ENUM) {
        uint32_t i;
        for (i = 0; i < t->nvariants; i++)
            if (ir_bits_eq(&t->variants[i].tag, v))
                return t->variants[i].name;
    }
    if (v->width <= 64)
        return arena_printf(&a->cm->cx->arena, "%llu", (unsigned long long)ir_bits_low64(v));
    {
        StrBuf sb = {0};
        const char *r;
        ir_bits_print(v, &sb);
        r = arena_strdup(&a->cm->cx->arena, sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }
}

static const char *asg_text(AsgCtx *a, const IrExpr *e)
{
    const char *t = e ? chk_span_text(a->cm->cx, e->loc, 60) : "";
    if (!*t)
        return e && e->loc.line ? arena_printf(&a->cm->cx->arena, "the condition on line %u", (unsigned)e->loc.line)
                                : "a condition";
    return t;
}

/* ------------------------------------------------- definite assignment -- */

/* checks that reads of the block's own targets read assigned bits */
static void asg_reads(AsgCtx *a, const IrExpr *e, const uint64_t *st);

static void asg_read_bits(AsgCtx *a, uint32_t sig, uint32_t lo, uint32_t hi, Span loc, const uint64_t *st)
{
    AsgTarget *t;
    uint64_t v;
    uint32_t f, e, clo, chi;
    if (!map_get(&a->tindex, (uint64_t)sig + 1, &v))
        return;
    t = &a->targets.data[v - 1];
    if (t->read_early)
        return;
    /* only the bits this block drives */
    chk_ranges_from(a->cm, sig, lo, &f, &e);
    for (; f < e; f++) {
        const ChkRange *r = &a->cm->ranges.data[f];
        uint32_t l, h;
        if (r->lo >= hi)
            break;
        if (r->node != t->node || !chk_range_overlaps(r, lo, hi))
            continue;
        l = r->lo > lo ? r->lo : lo;
        h = r->hi < hi ? r->hi : hi;
        if (chk_bits_first_clear(st, t->off + l, t->off + h, &clo, &chi)) {
            const IrSignal *s = &a->cm->m->sigs[sig];
            t->read_early = 1;
            if (chk_error(a->cm->cx, loc, "%s is read before it is assigned in this `comb` block",
                          chk_bits_name(a->cm->cx, a->cm->m, sig, clo - t->off, chi - t->off))) {
                diag_help("reading a signal the block has not assigned yet reads its own output (a combinational "
                          "loop); assign `%s` first or give it a default",
                          s->path);
            }
            return;
        }
    }
}

static void asg_reads(AsgCtx *a, const IrExpr *e, const uint64_t *st)
{
    uint32_t i, lo, off;
    const IrExpr *b;
    if (!e)
        return;
    switch (e->kind) {
    case IRE_CONST: return;
    case IRE_SIG: asg_read_bits(a, e->sig, 0, e->width, e->loc, st); return;
    case IRE_SLICE:
    case IRE_INDEX:
        b = e->kind == IRE_SLICE ? e : e->args[0];
        off = 0;
        while (b && b->kind == IRE_SLICE && b->nargs == 1) {
            off += b->lo;
            b = b->args[0];
        }
        if (b && b->kind == IRE_SIG) {
            lo = off;
            asg_read_bits(a, b->sig, lo, lo + (e->kind == IRE_SLICE ? e->width : e->args[0]->width), e->loc, st);
            if (e->kind == IRE_INDEX)
                asg_reads(a, e->args[1], st);
            return;
        }
        break;
    default: break;
    }
    for (i = 0; i < e->nargs; i++)
        asg_reads(a, e->args[i], st);
}

static void asg_and(AsgCtx *a, uint64_t *dst, const uint64_t *src)
{
    uint32_t i;
    for (i = 0; i < a->words; i++)
        dst[i] &= src[i];
}

static void asg_block(AsgCtx *a, const IrBlock *blk, uint64_t *st);

static void asg_stmt(AsgCtx *a, const IrStmt *s, uint64_t *st)
{
    uint32_t i;
    switch (s->kind) {
    case IRST_ASSIGN: {
        const AsgTarget *t;
        asg_reads(a, s->rhs, st);
        asg_reads(a, s->lhs.index, st);
        t = asg_target(a, s->lhs.sig);
        if (t && !s->lhs.index) {
            uint32_t hi = s->lhs.lo + s->lhs.width;
            if (hi > t->width)
                hi = t->width;
            if (s->lhs.lo < hi)
                chk_bits_set_range(st, t->off + s->lhs.lo, t->off + hi);
        }
        return;
    }
    case IRST_ASSERT: asg_reads(a, s->cond, st); return;
    case IRST_MEMWRITE:
        asg_reads(a, s->addr, st);
        asg_reads(a, s->data, st);
        asg_reads(a, s->mask, st);
        return;
    case IRST_IF: {
        uint64_t *e;
        asg_reads(a, s->cond, st);
        e = asg_copy(a, st);
        asg_block(a, &s->then_b, st);
        asg_block(a, &s->else_b, e);
        asg_and(a, st, e);
        asg_release(a, e);
        return;
    }
    case IRST_CASE: {
        uint64_t *in, *arm;
        const char *missing;
        int fall = !asg_case_has_default(s) && !asg_case_exhaustive(a, s, &missing);
        asg_reads(a, s->subject, st);
        if (!s->narms)
            return;
        in = asg_copy(a, st);
        arm = asg_alloc(a);
        for (i = 0; i < s->narms; i++) {
            memcpy(arm, in, a->words * sizeof(uint64_t));
            asg_block(a, &s->arms[i].body, arm);
            if (i == 0)
                memcpy(st, arm, a->words * sizeof(uint64_t));
            else
                asg_and(a, st, arm);
        }
        if (fall)
            asg_and(a, st, in);
        asg_release(a, in);
        asg_release(a, arm);
        return;
    }
    }
}

static void asg_block(AsgCtx *a, const IrBlock *blk, uint64_t *st)
{
    uint32_t i;
    for (i = 0; i < blk->n; i++)
        asg_stmt(a, blk->stmts[i], st);
}

/* ---------------------------------------------------------- witnesses -- */

/* Does the statement / block definitely assign bit `bit` of target sig? */
static int asg_w_stmt_assigns(const IrStmt *s, uint32_t sig, uint32_t bit, AsgCtx *a);

static int asg_w_block_assigns(const IrBlock *b, uint32_t sig, uint32_t bit, AsgCtx *a)
{
    uint32_t i;
    for (i = 0; i < b->n; i++)
        if (asg_w_stmt_assigns(b->stmts[i], sig, bit, a))
            return 1;
    return 0;
}

static int asg_w_stmt_assigns(const IrStmt *s, uint32_t sig, uint32_t bit, AsgCtx *a)
{
    uint32_t i;
    const char *missing;
    switch (s->kind) {
    case IRST_ASSIGN:
        return s->lhs.sig == sig && !s->lhs.index && bit >= s->lhs.lo && bit < s->lhs.lo + s->lhs.width;
    case IRST_IF:
        return asg_w_block_assigns(&s->then_b, sig, bit, a) && asg_w_block_assigns(&s->else_b, sig, bit, a);
    case IRST_CASE:
        if (!s->narms)
            return 0;
        if (!asg_case_has_default(s) && !asg_case_exhaustive(a, s, &missing))
            return 0;
        for (i = 0; i < s->narms; i++)
            if (!asg_w_block_assigns(&s->arms[i].body, sig, bit, a))
                return 0;
        return 1;
    default: return 0;
    }
}

/* may the statement assign the bit on some path? */
static int asg_w_stmt_may(const IrStmt *s, uint32_t sig, uint32_t bit);

static int asg_w_block_may(const IrBlock *b, uint32_t sig, uint32_t bit)
{
    uint32_t i;
    for (i = 0; i < b->n; i++)
        if (asg_w_stmt_may(b->stmts[i], sig, bit))
            return 1;
    return 0;
}

static int asg_w_stmt_may(const IrStmt *s, uint32_t sig, uint32_t bit)
{
    uint32_t i;
    switch (s->kind) {
    case IRST_ASSIGN:
        return s->lhs.sig == sig && !s->lhs.index && bit >= s->lhs.lo && bit < s->lhs.lo + s->lhs.width;
    case IRST_IF: return asg_w_block_may(&s->then_b, sig, bit) || asg_w_block_may(&s->else_b, sig, bit);
    case IRST_CASE:
        for (i = 0; i < s->narms; i++)
            if (asg_w_block_may(&s->arms[i].body, sig, bit))
                return 1;
        return 0;
    default: return 0;
    }
}

static void asg_path_add(AsgCtx *a, const IrStmt *s, const char *text)
{
    if (!a->first_stmt)
        a->first_stmt = s;
    sb_puts(&a->path, a->path.len ? " and " : " when ");
    sb_puts(&a->path, text);
}

/* builds the path (conditions of the branches that matter) on which the
   block leaves the bit unassigned; the block must not assign it definitely */
static void asg_w_block(AsgCtx *a, const IrBlock *b, uint32_t sig, uint32_t bit)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (!asg_w_stmt_may(s, sig, bit))
            continue;
        if (s->kind == IRST_IF) {
            if (!asg_w_block_assigns(&s->then_b, sig, bit, a)) {
                asg_path_add(a, s, arena_printf(&a->cm->cx->arena, "%s is true", asg_text(a, s->cond)));
                asg_w_block(a, &s->then_b, sig, bit);
            } else {
                asg_path_add(a, s, arena_printf(&a->cm->cx->arena, "%s is false", asg_text(a, s->cond)));
                asg_w_block(a, &s->else_b, sig, bit);
            }
        } else if (s->kind == IRST_CASE) {
            const char *subj = asg_text(a, s->subject), *missing = NULL;
            int fall = !asg_case_has_default(s) && !asg_case_exhaustive(a, s, &missing);
            int done = 0;
            for (k = 0; k < s->narms && !done; k++) {
                const IrCaseArm *arm = &s->arms[k];
                if (asg_w_block_assigns(&arm->body, sig, bit, a))
                    continue;
                if (arm->nvals)
                    asg_path_add(a, s,
                                 arena_printf(&a->cm->cx->arena, "%s == %s", subj, asg_val_name(a, s, &arm->vals[0])));
                else
                    asg_path_add(a, s, arena_printf(&a->cm->cx->arena, "%s matches no other arm", subj));
                asg_w_block(a, &arm->body, sig, bit);
                done = 1;
            }
            if (!done && fall)
                asg_path_add(a, s, arena_printf(&a->cm->cx->arena, "%s == %s", subj, missing ? missing : "?"));
        }
    }
}

/* ------------------------------------------------------------ driver -- */

static void asg_process(ChkMod *cm, uint32_t pi, uint32_t nfirst, uint32_t nend)
{
    AsgCtx a;
    uint64_t *st;
    uint32_t i, total = 0;
    ChkCtx *cx = cm->cx;
    memset(&a, 0, sizeof a);
    a.cm = cm;
    a.p = &cm->m->procs[pi];
    for (i = nfirst; i < nend; i++) {
        const ChkNode *n = &cm->nodes.data[i];
        AsgTarget t;
        if (n->kind != CHK_N_COMB || n->sig == IR_NONE)
            continue;
        t.sig = n->sig;
        t.off = total;
        t.width = cm->m->sigs[n->sig].type ? cm->m->sigs[n->sig].type->width : 1;
        t.node = i;
        t.read_early = 0;
        total += t.width;
        vec_push(a.targets, t);
        map_put(&a.tindex, (uint64_t)n->sig + 1, a.targets.len);
    }
    a.words = CHK_WORDS(total);
    if (!a.words)
        a.words = 1;
    st = asg_alloc(&a);
    memset(st, 0, a.words * sizeof(uint64_t));
    asg_block(&a, &a.p->body, st);
    for (i = 0; i < a.targets.len; i++) {
        const AsgTarget *t = &a.targets.data[i];
        const IrSignal *s = &cm->m->sigs[t->sig];
        uint32_t f, e;
        if (t->read_early)
            continue;
        chk_ranges_of(cm, t->sig, &f, &e);
        for (; f < e; f++) {
            const ChkRange *r = &cm->ranges.data[f];
            uint32_t clo, chi;
            Span at;
            int ok;
            if (r->node != t->node)
                continue;
            if (!chk_bits_first_clear(st, t->off + r->lo, t->off + r->hi, &clo, &chi))
                continue;
            clo -= t->off;
            chi -= t->off;
            sb_reset(&a.path);
            a.first_stmt = NULL;
            asg_w_block(&a, &a.p->body, t->sig, clo);
            at = a.first_stmt ? a.first_stmt->loc : a.p->loc;
            if (!at.file)
                at = a.p->loc;
            if (a.path.len)
                ok = chk_error(cx, at, "%s may be unassigned%s", chk_bits_name(cx, cm->m, t->sig, clo, chi),
                               sb_cstr(&a.path));
            else
                ok = chk_error(cx, at,
                               "%s may be unassigned: an assignment with a dynamic index does not assign every element",
                               chk_bits_name(cx, cm->m, t->sig, clo, chi));
            if (ok) {
                if (s->kind == IRS_VAR) {
                    StrBuf ty = {0};
                    if (s->type)
                        ir_type_print(s->type, &ty);
                    diag_help("assign it on every path, or give it a default: `var %s: %s = ...`", s->path,
                              ty.len ? sb_cstr(&ty) : "T");
                    sb_free(&ty);
                }
                else
                    diag_help("assign it on every path (add the missing `else` or match arm), or assign a default at "
                              "the start of the block");
            }
            break;
        }
    }
    asg_release(&a, st);
    while (a.pool.len) {
        uint64_t *b = vec_pop(a.pool);
        free(b);
    }
    vec_free(a.pool);
    vec_free(a.targets);
    map_free(&a.tindex);
    sb_free(&a.path);
}

void chk_assign(ChkMod *cm)
{
    uint32_t i = 0, n = (uint32_t)cm->nodes.len;
    /* the nodes of one process are consecutive */
    while (i < n) {
        const ChkNode *nd = &cm->nodes.data[i];
        uint32_t j = i + 1;
        if ((nd->kind != CHK_N_COMB && nd->kind != CHK_N_PROC) || cm->m->procs[nd->idx].kind != IRP_COMB) {
            i++;
            continue;
        }
        while (j < n && (cm->nodes.data[j].kind == CHK_N_COMB || cm->nodes.data[j].kind == CHK_N_PROC) &&
               cm->nodes.data[j].idx == nd->idx)
            j++;
        asg_process(cm, nd->idx, i, j);
        i = j;
    }
}

/* -------------------------------------------------------------- loops -- */

static const char *asg_node_name(const ChkMod *cm, uint32_t node)
{
    const ChkNode *n = &cm->nodes.data[node];
    if (n->sig != IR_NONE && n->sig < cm->m->nsigs)
        return cm->m->sigs[n->sig].path;
    return "?";
}

void chk_loops(ChkMod *cm)
{
    uint32_t k, nn = (uint32_t)cm->nodes.len;
    uint32_t *comp = NULL, *prev = NULL, *queue = NULL;
    ChkCtx *cx = cm->cx;
    for (k = 0; k < cm->nscc; k++) {
        uint32_t sz = cm->sfirst[k + 1] - cm->sfirst[k];
        uint32_t u = cm->order[cm->sfirst[k]];
        StrBuf sb = {0};
        VEC(uint32_t) cyc = {0};
        uint32_t i;
        if (sz == 1 && !cm->self_loop[u])
            continue;
        if (sz > 1) {
            uint32_t qh = 0, qt = 0, q;
            int found = 0;
            if (!comp) {
                comp = (uint32_t *)xmalloc((nn + 1) * sizeof(uint32_t));
                prev = (uint32_t *)xmalloc((nn + 1) * sizeof(uint32_t));
                queue = (uint32_t *)xmalloc((nn + 1) * sizeof(uint32_t));
                for (i = 0; i < nn; i++)
                    comp[i] = IR_NONE;
                for (i = 0; i < cm->nscc; i++)
                    for (q = cm->sfirst[i]; q < cm->sfirst[i + 1]; q++)
                        comp[cm->order[q]] = i;
            }
            for (q = cm->sfirst[k]; q < cm->sfirst[k + 1]; q++)
                prev[cm->order[q]] = IR_NONE;
            /* BFS from u back to u inside the component */
            queue[qt++] = u;
            while (qh < qt && !found) {
                uint32_t v = queue[qh++], ei;
                for (ei = cm->efirst[v]; ei < cm->efirst[v + 1]; ei++) {
                    uint32_t w = cm->edges[ei];
                    if (comp[w] != k)
                        continue;
                    if (w == u) {
                        prev[u] = v;
                        found = 1;
                        break;
                    }
                    if (prev[w] == IR_NONE) {
                        prev[w] = v;
                        queue[qt++] = w;
                    }
                }
            }
            if (found) {
                uint32_t v = prev[u];
                vec_push(cyc, u);
                while (v != u && cyc.len <= sz) {
                    vec_push(cyc, v);
                    v = prev[v];
                }
                /* cyc holds u, then its predecessors: reverse into driver order */
                for (i = 0; i < cyc.len / 2; i++) {
                    uint32_t t = cyc.data[i];
                    cyc.data[i] = cyc.data[cyc.len - 1 - i];
                    cyc.data[cyc.len - 1 - i] = t;
                }
            } else {
                vec_push(cyc, u);
            }
        } else {
            vec_push(cyc, u);
        }
        for (i = 0; i < cyc.len; i++) {
            sb_puts(&sb, asg_node_name(cm, cyc.data[i]));
            sb_puts(&sb, " -> ");
        }
        sb_puts(&sb, asg_node_name(cm, cyc.data[0]));
        if (chk_error(cx, cm->nodes.data[cyc.data[0]].loc, "combinational loop: %s", sb_cstr(&sb))) {
            for (i = 1; i < cyc.len; i++) {
                const ChkNode *nd = &cm->nodes.data[cyc.data[i]];
                if (nd->loc.file)
                    diag_note(nd->loc, "`%s` is driven here", asg_node_name(cm, cyc.data[i]));
            }
            diag_help("break the loop with a register, or compute the value without reading it back");
        }
        sb_free(&sb);
        vec_free(cyc);
    }
    free(comp);
    free(prev);
    free(queue);
}
