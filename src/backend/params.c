/* params.c — `tern build`: the front-end half of parameterized
 * SystemVerilog modules (spec §13.3; method in emit.h at sv_params_plan).
 * For each generic module of a built design whose generics are all uint or
 * bool values, this collects
 *
 *   - candidate expressions: every expression of the declaration (types,
 *     consts, generic defaults, `where`, the body outside fns) built only
 *     from its generics, integer literals, module consts with such values,
 *     + - * / % << >> & | ^, comparisons, and/or, `if` and clog2, as SvXExpr
 *     trees (module consts become localparams);
 *   - probes: one extra elaboration per generic with that generic changed
 *     (2v, v+1, v+2, v-1 or v/2 — the first that elaborates without errors;
 *     a bool is flipped), with diagnostics captured;
 *
 * and hands them to sv_params_plan(), which decides and attaches the plan
 * to the design. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/diag.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../driver.h"
#include "../elab/value.h"
#include "backend.h"

#define BKP_MAX_CANDS 256
#define BKP_MAX_PROBES 256

typedef VEC(SvXExpr *) BkpExprVec;

typedef struct BkpHarvest {
    Arena *a;
    const GenericParams *gp;
    Map consts;               /* interned const name -> SvXExpr* (SVX_LOCAL) */
    BkpExprVec out;
} BkpHarvest;

static SvXExpr *bkp_new(Arena *a, SvXOp op, SvXExpr *l, SvXExpr *r)
{
    SvXExpr *x = ANEW(a, SvXExpr);
    x->op = op;
    x->a = l;
    x->b = r;
    return x;
}

static int bkp_binop(BinaryOp op, SvXOp *out)
{
    switch (op) {
    case BIN_MUL: *out = SVX_MUL; return 1;
    case BIN_DIV: *out = SVX_DIV; return 1;
    case BIN_MOD: *out = SVX_MOD; return 1;
    case BIN_ADD: *out = SVX_ADD; return 1;
    case BIN_SUB: *out = SVX_SUB; return 1;
    case BIN_SHL: *out = SVX_SHL; return 1;
    case BIN_SHR: *out = SVX_SHR; return 1;
    case BIN_BITAND: *out = SVX_AND; return 1;
    case BIN_BITXOR: *out = SVX_XOR; return 1;
    case BIN_BITOR: *out = SVX_OR; return 1;
    case BIN_EQ: *out = SVX_EQ; return 1;
    case BIN_NE: *out = SVX_NE; return 1;
    case BIN_LT: *out = SVX_LT; return 1;
    case BIN_LE: *out = SVX_LE; return 1;
    case BIN_GT: *out = SVX_GT; return 1;
    case BIN_GE: *out = SVX_GE; return 1;
    case BIN_AND: *out = SVX_LAND; return 1;
    case BIN_OR: *out = SVX_LOR; return 1;
    default: return 0;
    }
}

/* the value of a block that is only a tail expression */
static const Expr *bkp_tail(const Expr *e)
{
    if (e && e->kind == EXPR_BLOCK && e->u.block && e->u.block->nstmts == 1 && e->u.block->tail)
        return e->u.block->tail;
    return NULL;
}

static SvXExpr *bkp_tr(BkpHarvest *h, const Expr *e)
{
    SvXOp op;
    uint32_t i;
    if (!e)
        return NULL;
    switch (e->kind) {
    case EXPR_PAREN: return bkp_tr(h, e->u.inner);
    case EXPR_INT:
        if (e->u.int_lit.overflow || e->u.int_lit.value >= (UINT64_C(1) << 31) || e->u.int_lit.suffix == 's')
            return NULL;
        {
            SvXExpr *x = bkp_new(h->a, SVX_K, NULL, NULL);
            x->k = (int64_t)e->u.int_lit.value;
            return x;
        }
    case EXPR_BOOL: {
        SvXExpr *x = bkp_new(h->a, SVX_K, NULL, NULL);
        x->k = e->u.bool_val ? 1 : 0;
        return x;
    }
    case EXPR_PATH: {
        const char *n;
        uint64_t v;
        if (e->u.path.nsegs != 1 || e->u.path.segs[0].has_args)
            return NULL;
        n = e->u.path.segs[0].name.name;
        for (i = 0; i < h->gp->n; i++)
            if (h->gp->items[i].name.name == n) {
                SvXExpr *x;
                if (h->gp->items[i].kind != GENERIC_VALUE)
                    return NULL;
                x = bkp_new(h->a, SVX_PARAM, NULL, NULL);
                x->param = i;
                return x;
            }
        if (map_get(&h->consts, MAP_PTR(n), &v))
            return (SvXExpr *)(uintptr_t)v;
        return NULL;
    }
    case EXPR_BINARY: {
        SvXExpr *a, *b;
        if (!bkp_binop(e->u.binary.op, &op))
            return NULL;
        a = bkp_tr(h, e->u.binary.lhs);
        b = a ? bkp_tr(h, e->u.binary.rhs) : NULL;
        return b ? bkp_new(h->a, op, a, b) : NULL;
    }
    case EXPR_CALL: {
        const Expr *c = e->u.call.callee;
        SvXExpr *a;
        if (!c || c->kind != EXPR_PATH || c->u.path.nsegs != 1 || c->u.path.segs[0].has_args ||
            strcmp(c->u.path.segs[0].name.name, "clog2") != 0 || e->u.call.nargs != 1 ||
            e->u.call.args[0].name.name || e->u.call.domain)
            return NULL;
        a = bkp_tr(h, e->u.call.args[0].value);
        return a ? bkp_new(h->a, SVX_CLOG2, a, NULL) : NULL;
    }
    case EXPR_IF: {
        SvXExpr *c, *t, *f;
        const Expr *els = e->u.if_expr.else_expr;
        if (!e->u.if_expr.then_block || e->u.if_expr.then_block->nstmts != 1 || !e->u.if_expr.then_block->tail || !els)
            return NULL;
        c = bkp_tr(h, e->u.if_expr.cond);
        t = c ? bkp_tr(h, e->u.if_expr.then_block->tail) : NULL;
        f = t ? (els->kind == EXPR_IF ? bkp_tr(h, els) : bkp_tr(h, bkp_tail(els))) : NULL;
        if (!f)
            return NULL;
        c = bkp_new(h->a, SVX_COND, c, t);
        c->c = f;
        return c;
    }
    default: return NULL;
    }
}

static int bkp_eq(const SvXExpr *a, const SvXExpr *b)
{
    if (!a || !b)
        return a == b;
    return a->op == b->op && a->k == b->k && a->param == b->param && a->name == b->name && bkp_eq(a->a, b->a) &&
           bkp_eq(a->b, b->b) && bkp_eq(a->c, b->c);
}

static int bkp_uses_param(const SvXExpr *x)
{
    if (!x)
        return 0;
    return x->op == SVX_PARAM || bkp_uses_param(x->a) || bkp_uses_param(x->b) || bkp_uses_param(x->c);
}

static void bkp_add(BkpHarvest *h, SvXExpr *x)
{
    size_t i;
    if (!x || !bkp_uses_param(x) || h->out.len >= BKP_MAX_CANDS)
        return;
    for (i = 0; i < h->out.len; i++)
        if (bkp_eq(h->out.data[i], x))
            return;
    vec_push(h->out, x);
}

static void bkp_expr(BkpHarvest *h, const Expr *e);
static void bkp_block(BkpHarvest *h, const Block *b);

static void bkp_type(BkpHarvest *h, const Type *t)
{
    uint32_t i, j;
    if (!t)
        return;
    switch (t->kind) {
    case TYPE_PATH:
        for (i = 0; i < t->u.path.nsegs; i++)
            for (j = 0; j < t->u.path.segs[i].nargs; j++) {
                bkp_type(h, t->u.path.segs[i].args[j].type);
                bkp_expr(h, t->u.path.segs[i].args[j].expr);
            }
        break;
    case TYPE_ARRAY:
        bkp_type(h, t->u.array.elem);
        bkp_expr(h, t->u.array.len);
        break;
    case TYPE_STRUCT:
        for (i = 0; i < t->u.anon_struct.nfields; i++)
            bkp_type(h, t->u.anon_struct.fields[i]->type);
        break;
    case TYPE_TUPLE:
        for (i = 0; i < t->u.tuple.n; i++)
            bkp_type(h, t->u.tuple.elems[i]);
        break;
    default: break;
    }
}

static void bkp_path(BkpHarvest *h, const Path *p)
{
    uint32_t i, j;
    for (i = 0; i < p->nsegs; i++)
        for (j = 0; j < p->segs[i].nargs; j++) {
            bkp_type(h, p->segs[i].args[j].type);
            bkp_expr(h, p->segs[i].args[j].expr);
        }
}

static void bkp_expr(BkpHarvest *h, const Expr *e)
{
    uint32_t i;
    if (!e)
        return;
    bkp_add(h, bkp_tr(h, e));
    switch (e->kind) {
    case EXPR_INT: bkp_expr(h, e->u.int_lit.width); break;
    case EXPR_PATH: bkp_path(h, &e->u.path); break;
    case EXPR_VARIANT:
        for (i = 0; i < e->u.variant.nfields; i++)
            bkp_expr(h, e->u.variant.fields[i].value);
        break;
    case EXPR_STRUCT:
        bkp_path(h, &e->u.struct_lit.path);
        for (i = 0; i < e->u.struct_lit.nfields; i++)
            bkp_expr(h, e->u.struct_lit.fields[i].value);
        break;
    case EXPR_FIELD:
        bkp_expr(h, e->u.field.base);
        for (i = 0; i < e->u.field.nargs; i++) {
            bkp_type(h, e->u.field.args[i].type);
            bkp_expr(h, e->u.field.args[i].expr);
        }
        break;
    case EXPR_INDEX:
        bkp_expr(h, e->u.index.base);
        bkp_expr(h, e->u.index.index);
        break;
    case EXPR_SLICE:
        bkp_expr(h, e->u.slice.base);
        bkp_expr(h, e->u.slice.hi);
        bkp_expr(h, e->u.slice.lo);
        break;
    case EXPR_CALL:
        bkp_expr(h, e->u.call.callee);
        for (i = 0; i < e->u.call.nargs; i++)
            bkp_expr(h, e->u.call.args[i].value);
        break;
    case EXPR_CAST:
        bkp_expr(h, e->u.cast.expr);
        bkp_type(h, e->u.cast.type);
        break;
    case EXPR_UNARY: bkp_expr(h, e->u.unary.operand); break;
    case EXPR_BINARY:
        bkp_expr(h, e->u.binary.lhs);
        bkp_expr(h, e->u.binary.rhs);
        break;
    case EXPR_RANGE:
        bkp_expr(h, e->u.range.lo);
        bkp_expr(h, e->u.range.hi);
        break;
    case EXPR_IF:
        bkp_expr(h, e->u.if_expr.cond);
        bkp_block(h, e->u.if_expr.then_block);
        bkp_expr(h, e->u.if_expr.else_expr);
        break;
    case EXPR_MATCH:
        bkp_expr(h, e->u.match.scrutinee);
        for (i = 0; i < e->u.match.narms; i++)
            if (e->u.match.arms[i].body && e->u.match.arms[i].body->kind == STMT_EXPR)
                bkp_expr(h, e->u.match.arms[i].body->u.expr);
        break;
    case EXPR_BLOCK: bkp_block(h, e->u.block); break;
    case EXPR_TUPLE:
    case EXPR_ARRAY:
        for (i = 0; i < e->u.list.n; i++)
            bkp_expr(h, e->u.list.items[i]);
        break;
    case EXPR_PAREN: bkp_expr(h, e->u.inner); break;
    case EXPR_TYPE: bkp_type(h, e->u.type); break;
    default: break;
    }
}

static void bkp_stmt(BkpHarvest *h, const Stmt *s)
{
    if (!s)
        return;
    switch (s->kind) {
    case STMT_PORT: bkp_type(h, s->u.port.type); break;
    case STMT_LET:
        bkp_type(h, s->u.let.type);
        bkp_expr(h, s->u.let.init);
        break;
    case STMT_VAR:
        bkp_type(h, s->u.var.type);
        bkp_expr(h, s->u.var.init);
        break;
    case STMT_REG:
        bkp_type(h, s->u.reg.type);
        bkp_expr(h, s->u.reg.init);
        break;
    case STMT_MEM:
        bkp_type(h, s->u.mem.type);
        bkp_expr(h, s->u.mem.init);
        break;
    case STMT_ITEM:
        if (s->u.item && s->u.item->kind == ITEM_CONST) {
            bkp_type(h, s->u.item->u.const_def.type);
            bkp_expr(h, s->u.item->u.const_def.value);
        }
        break;
    case STMT_COMB: bkp_block(h, s->u.comb); break;
    case STMT_ON: bkp_block(h, s->u.on.body); break;
    case STMT_ASSIGN:
        bkp_expr(h, s->u.assign.lhs);
        bkp_expr(h, s->u.assign.rhs);
        break;
    case STMT_EXPR: bkp_expr(h, s->u.expr); break;
    case STMT_FOR:
        bkp_expr(h, s->u.for_stmt.iter);
        bkp_block(h, s->u.for_stmt.body);
        break;
    case STMT_WHILE:
        bkp_expr(h, s->u.while_stmt.cond);
        bkp_block(h, s->u.while_stmt.body);
        break;
    case STMT_ASSERT: bkp_expr(h, s->u.assert_stmt.cond); break;
    case STMT_STATIC_ASSERT: bkp_expr(h, s->u.sassert.cond); break;
    default: break;
    }
}

static void bkp_block(BkpHarvest *h, const Block *b)
{
    uint32_t i;
    if (!b)
        return;
    for (i = 0; i < b->nstmts; i++)
        bkp_stmt(h, b->stmts[i]);
}

/* candidate expressions of a module declaration (allocated in a) */
static void bkp_harvest(const Item *it, Arena *a, BkpExprVec *out)
{
    BkpHarvest h;
    uint32_t i, j;
    int more = 1;
    const GenericParams *gp = &it->u.module.generics;
    memset(&h, 0, sizeof h);
    h.a = a;
    h.gp = gp;
    /* module consts whose values are expressions of the generics (in any
     * order: a const may use one declared after it) */
    while (more) {
        more = 0;
        for (i = 0; i < it->u.module.nbody; i++) {
            const Stmt *s = it->u.module.body[i];
            const Item *ci;
            SvXExpr *v;
            if (s->kind != STMT_ITEM || !s->u.item || s->u.item->kind != ITEM_CONST)
                continue;
            ci = s->u.item;
            if (!ci->name.name || map_has(&h.consts, MAP_PTR(ci->name.name)))
                continue;
            for (j = 0; j < gp->n; j++)
                if (gp->items[j].name.name == ci->name.name)
                    break;
            if (j < gp->n)
                continue;
            v = bkp_tr(&h, ci->u.const_def.value);
            if (v) {
                SvXExpr *x = bkp_new(a, SVX_LOCAL, v, NULL);
                x->name = ci->name.name;
                map_put(&h.consts, MAP_PTR(ci->name.name), MAP_PTR(x));
                more = 1;
            }
        }
    }
    for (i = 0; i < gp->n; i++) {
        bkp_type(&h, gp->items[i].type);
        bkp_expr(&h, gp->items[i].default_value);
    }
    for (i = 0; i < it->u.module.where.n; i++)
        bkp_expr(&h, it->u.module.where.items[i]);
    for (i = 0; i < it->u.module.nbody; i++)
        bkp_stmt(&h, it->u.module.body[i]);
    for (i = 0; i < h.out.len; i++)
        vec_push(*out, h.out.data[i]);
    vec_free(h.out);
    map_free(&h.consts);
}

/* ---------------------------------------------------------------- probes */

static void bkp_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(d);
    TERN_UNUSED(user);
}

static int bkp_value_generics(const IrModule *m)
{
    uint32_t i;
    if (!m->origin || !m->origin->ngenerics || m->is_extern || m->is_cdc)
        return 0;
    for (i = 0; i < m->origin->ngenerics; i++) {
        const IrGeneric *g = &m->origin->generics[i];
        if (g->kind == IRG_BOOL)
            continue;
        if (g->kind != IRG_UINT || !g->fits || g->value < 0 || g->value >= (INT64_C(1) << 31))
            return 0;
    }
    return 1;
}

/* elaborates the module with generic p set to v; NULL if that fails */
static IrDesign *bkp_probe(Driver *dr, const IrModule *m, uint32_t p, int64_t v, Arena *arena, uint32_t *budget)
{
    const IrOrigin *o = m->origin;
    ElabArgs args;
    uint32_t i;
    int errs = diag_error_count(), warns = diag_warning_count();
    DiagSink sink;
    void *user;
    IrDesign *d;
    if (!*budget)
        return NULL;
    (*budget)--;
    args.n = o->ngenerics;
    args.items = (ElabArg *)xcalloc(o->ngenerics, sizeof(ElabArg));
    for (i = 0; i < o->ngenerics; i++) {
        const IrGeneric *g = &o->generics[i];
        int64_t x = i == p ? v : g->value;
        args.items[i].name = g->name;
        args.items[i].val = g->kind == IRG_BOOL ? val_bool(dr->S, x != 0) : val_int(dr->S->t_int, x);
    }
    diag_get_sink(&sink, &user);
    diag_set_sink(bkp_sink, NULL);
    d = driver_elaborate_module(dr, (Item *)o->item, (FileUnit *)o->file, &args, arena);
    diag_flush();
    if (diag_error_count() != errs)
        d = NULL;
    diag_set_sink(sink, user);
    diag_set_counts(errs, warns);
    free(args.items);
    return d && d->top ? d : NULL;
}

int bk_params_prepare(struct Driver *dr, IrDesign *d, const SvOptions *o)
{
    VEC(SvParamItem) items = {0};
    Arena arena = {0};
    uint32_t i, j, budget = BKP_MAX_PROBES;
    int accepted;
    SvParamInput in;
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        const Item *it;
        SvParamItem pi;
        BkpExprVec cands = {0};
        VEC(IrDesign *) probes = {0};
        size_t k;
        int dup = 0;
        if (!bkp_value_generics(m))
            continue;
        for (k = 0; k < items.len; k++)
            if (items.data[k].item == m->origin->item)
                dup = 1;
        it = (const Item *)m->origin->item;
        if (dup || it->kind != ITEM_MODULE)
            continue;
        bkp_harvest(it, &arena, &cands);
        for (j = 0; j < m->origin->ngenerics; j++) {
            const IrGeneric *g = &m->origin->generics[j];
            int64_t tries[5];
            uint32_t nt = 0, t;
            if (g->kind == IRG_BOOL) {
                tries[nt++] = g->value ? 0 : 1;
            } else {
                tries[nt++] = g->value * 2;
                tries[nt++] = g->value + 1;
                tries[nt++] = g->value + 2;
                if (g->value >= 1)
                    tries[nt++] = g->value - 1;
                if (g->value >= 4 && g->value % 2 == 0)
                    tries[nt++] = g->value / 2;
            }
            for (t = 0; t < nt; t++) {
                IrDesign *pd;
                if (tries[t] < 0 || tries[t] >= (INT64_C(1) << 31) || (t && tries[t] == tries[0]))
                    continue;
                pd = bkp_probe(dr, m, j, tries[t], &arena, &budget);
                if (pd) {
                    vec_push(probes, pd);
                    break;
                }
            }
        }
        memset(&pi, 0, sizeof pi);
        pi.item = it;
        pi.ncands = (uint32_t)cands.len;
        pi.cands = ANEWN(&arena, SvXExpr *, cands.len ? cands.len : 1);
        for (k = 0; k < cands.len; k++)
            pi.cands[k] = cands.data[k];
        pi.nprobes = (uint32_t)probes.len;
        pi.probes = ANEWN(&arena, IrDesign *, probes.len ? probes.len : 1);
        for (k = 0; k < probes.len; k++)
            pi.probes[k] = probes.data[k];
        vec_push(items, pi);
        vec_free(cands);
        vec_free(probes);
    }
    in.items = items.data;
    in.n = items.len;
    accepted = items.len ? sv_params_plan(d, &in, o) : 0;
    /* the probe designs are not referenced by the plan */
    for (i = 0; i < items.len; i++)
        for (j = 0; j < items.data[i].nprobes; j++) {
            IrDesign *pd = items.data[i].probes[j];
            sv_params_free(pd);
        }
    vec_free(items);
    arena_release(&arena);
    return accepted;
}
