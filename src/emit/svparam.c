/* svparam.c — parameterized SystemVerilog modules, the IR side (spec
 * §13.3; see emit.h at sv_params_plan and svparam.h): the lockstep walk
 * that lines up the numbers of several elaborations of one declaration,
 * matching those numbers with expressions over the generics, and printing
 * the expressions. */
#include "svparam.h"
#include "../ir/ir.h"

/* ------------------------------------------------------- expressions -- */

#define SVX_LIMIT (INT64_C(1) << 31)

static int svx_ok(int64_t v) { return v >= 0 && v < SVX_LIMIT; }

int sv_xexpr_eval(const SvXExpr *x, const int64_t *params, int64_t *out)
{
    int64_t a = 0, b = 0, r = 0;
    if (!x)
        return 0;
    switch (x->op) {
    case SVX_K: r = x->k; break;
    case SVX_PARAM: r = params[x->param]; break;
    case SVX_LOCAL:
        if (!sv_xexpr_eval(x->a, params, &r))
            return 0;
        break;
    case SVX_CLOG2: {
        int64_t n;
        if (!sv_xexpr_eval(x->a, params, &n))
            return 0;
        r = 0;
        while (r < 62 && (INT64_C(1) << r) < n)
            r++;
        break;
    }
    case SVX_COND:
        if (!sv_xexpr_eval(x->a, params, &a))
            return 0;
        if (!sv_xexpr_eval(a ? x->b : x->c, params, &r))
            return 0;
        break;
    default:
        if (!sv_xexpr_eval(x->a, params, &a) || !sv_xexpr_eval(x->b, params, &b))
            return 0;
        switch (x->op) {
        case SVX_ADD: r = a + b; break;
        case SVX_SUB: r = a - b; break;
        case SVX_MUL:
            if (a && b > SVX_LIMIT / a)
                return 0;
            r = a * b;
            break;
        case SVX_DIV:
            if (!b)
                return 0;
            r = a / b;
            break;
        case SVX_MOD:
            if (!b)
                return 0;
            r = a % b;
            break;
        case SVX_SHL:
            if (b >= 31)
                return 0;
            r = a << b;
            break;
        case SVX_SHR: r = b >= 63 ? 0 : a >> b; break;
        case SVX_AND: r = a & b; break;
        case SVX_OR: r = a | b; break;
        case SVX_XOR: r = a ^ b; break;
        case SVX_EQ: r = a == b; break;
        case SVX_NE: r = a != b; break;
        case SVX_LT: r = a < b; break;
        case SVX_LE: r = a <= b; break;
        case SVX_GT: r = a > b; break;
        case SVX_GE: r = a >= b; break;
        case SVX_LAND: r = a && b; break;
        case SVX_LOR: r = a || b; break;
        default: return 0;
        }
        break;
    }
    if (!svx_ok(r))
        return 0;
    *out = r;
    return 1;
}

static int svx_prec(const SvXExpr *x)
{
    switch (x->op) {
    case SVX_ADD:
    case SVX_SUB: return SVX_PREC_ADD;
    case SVX_MUL:
    case SVX_DIV:
    case SVX_MOD: return SVX_PREC_MUL;
    case SVX_SHL:
    case SVX_SHR: return SVX_PREC_SHIFT;
    case SVX_AND: return SVX_PREC_BAND;
    case SVX_OR: return SVX_PREC_BOR;
    case SVX_XOR: return SVX_PREC_BXOR;
    case SVX_EQ:
    case SVX_NE: return SVX_PREC_EQ;
    case SVX_LT:
    case SVX_LE:
    case SVX_GT:
    case SVX_GE: return SVX_PREC_REL;
    case SVX_LAND: return SVX_PREC_LAND;
    case SVX_LOR: return SVX_PREC_LOR;
    case SVX_COND: return SVX_PREC_COND;
    default: return SVX_PREC_PRIM;
    }
}

static const char *svx_sym(SvXOp op)
{
    switch (op) {
    case SVX_ADD: return "+";
    case SVX_SUB: return "-";
    case SVX_MUL: return "*";
    case SVX_DIV: return "/";
    case SVX_MOD: return "%";
    case SVX_SHL: return "<<";
    case SVX_SHR: return ">>";
    case SVX_AND: return "&";
    case SVX_OR: return "|";
    case SVX_XOR: return "^";
    case SVX_EQ: return "==";
    case SVX_NE: return "!=";
    case SVX_LT: return "<";
    case SVX_LE: return "<=";
    case SVX_GT: return ">";
    case SVX_GE: return ">=";
    case SVX_LAND: return "&&";
    case SVX_LOR: return "||";
    default: return "?";
    }
}

void svp_print(const SvXExpr *x, const char *const *pnames, int prec, StrBuf *out)
{
    int p = svx_prec(x);
    int paren = p < prec;
    if (paren)
        sb_putc(out, '(');
    switch (x->op) {
    case SVX_K: sb_printf(out, "%lld", (long long)x->k); break;
    case SVX_PARAM: sb_puts(out, pnames[x->param]); break;
    case SVX_LOCAL: sb_puts(out, x->name); break;
    case SVX_CLOG2:
        sb_puts(out, "$clog2(");
        svp_print(x->a, pnames, SVX_PREC_COND, out);
        sb_putc(out, ')');
        break;
    case SVX_COND:
        svp_print(x->a, pnames, SVX_PREC_LOR, out);
        sb_puts(out, " ? ");
        svp_print(x->b, pnames, SVX_PREC_LOR, out);
        sb_puts(out, " : ");
        svp_print(x->c, pnames, SVX_PREC_COND, out);
        break;
    default:
        svp_print(x->a, pnames, p, out);
        sb_printf(out, " %s ", svx_sym(x->op));
        svp_print(x->b, pnames, p + 1, out);
        break;
    }
    if (paren)
        sb_putc(out, ')');
}

void svp_print_m1(const SvXExpr *x, const char *const *pnames, StrBuf *out)
{
    if (x->op == SVX_K) {
        sb_printf(out, "%lld", (long long)(x->k - 1));
        return;
    }
    if (x->op == SVX_ADD && x->b->op == SVX_K && x->b->k >= 1) {
        if (x->b->k == 1) {
            svp_print(x->a, pnames, SVX_PREC_ADD, out);
        } else {
            svp_print(x->a, pnames, SVX_PREC_ADD, out);
            sb_printf(out, " + %lld", (long long)(x->b->k - 1));
        }
        return;
    }
    if (svx_prec(x) == SVX_PREC_PRIM) {
        svp_print(x, pnames, SVX_PREC_PRIM, out);
        sb_puts(out, "-1");
        return;
    }
    svp_print(x, pnames, SVX_PREC_ADD, out);
    sb_puts(out, " - 1");
}

void svp_print_castw(const SvXExpr *x, const char *const *pnames, StrBuf *out)
{
    if (x->op == SVX_K || x->op == SVX_PARAM || x->op == SVX_LOCAL) {
        svp_print(x, pnames, SVX_PREC_PRIM, out);
        return;
    }
    sb_putc(out, '(');
    svp_print(x, pnames, SVX_PREC_COND, out);
    sb_putc(out, ')');
}

void svp_collect_locals(const SvXExpr *x, SvPGroup *g)
{
    size_t i;
    if (!x)
        return;
    svp_collect_locals(x->a, g);
    svp_collect_locals(x->b, g);
    svp_collect_locals(x->c, g);
    if (x->op != SVX_LOCAL)
        return;
    for (i = 0; i < g->locals.len; i++)
        if (g->locals.data[i]->name == x->name)
            return;
    vec_push(g->locals, x);
}

int svp_equal(const SvXExpr *a, const SvXExpr *b)
{
    if (!a || !b)
        return a == b;
    if (a == b)
        return 1;
    return a->op == b->op && a->k == b->k && a->param == b->param && a->name == b->name && svp_equal(a->a, b->a) &&
           svp_equal(a->b, b->b) && svp_equal(a->c, b->c);
}

/* a - b as a constant when it is one (x + k - x, k1 - k2), else 0 */
int svp_const_diff(const SvXExpr *a, const SvXExpr *b, int64_t *out)
{
    int64_t ka = 0, kb = 0;
    if (a->op == SVX_ADD && a->b->op == SVX_K) {
        ka = a->b->k;
        a = a->a;
    }
    if (b->op == SVX_ADD && b->b->op == SVX_K) {
        kb = b->b->k;
        b = b->a;
    }
    if (a->op == SVX_K && b->op == SVX_K) {
        *out = a->k + ka - b->k - kb;
        return 1;
    }
    if (!svp_equal(a, b))
        return 0;
    *out = ka - kb;
    return 1;
}

static int svx_uses_param(const SvXExpr *x)
{
    if (!x)
        return 0;
    if (x->op == SVX_PARAM)
        return 1;
    return svx_uses_param(x->a) || svx_uses_param(x->b) || svx_uses_param(x->c);
}

/* -------------------------------------------------------- candidates -- */

static SvXExpr *svx_new(Arena *a, SvXOp op, const SvXExpr *l, const SvXExpr *r, int64_t k)
{
    SvXExpr *x = ANEW(a, SvXExpr);
    x->op = op;
    x->a = (SvXExpr *)l;
    x->b = (SvXExpr *)r;
    x->k = k;
    return x;
}

void svp_cands_init(SvPCands *c, const SvXExpr *const *base, uint32_t nbase, const int64_t *const *pvals,
                    uint32_t nelab, Arena *a)
{
    VEC(const SvXExpr *) all = {0};
    VEC(const SvXExpr *) b = {0};
    uint32_t i, j, k;
    static const int64_t deltas[] = {1, -1, 2, -2};
    for (i = 0; i < nbase; i++)
        if (svx_uses_param(base[i]))
            vec_push(b, base[i]);
    for (i = 0; i < b.len; i++)
        vec_push(all, b.data[i]);
    for (k = 0; k < TERN_ARRAY_LEN(deltas); k++)
        for (i = 0; i < b.len; i++) {
            int64_t d = deltas[k];
            vec_push(all, svx_new(a, d > 0 ? SVX_ADD : SVX_SUB, b.data[i], svx_new(a, SVX_K, NULL, NULL, d > 0 ? d : -d), 0));
        }
    for (i = 0; i < b.len; i++)
        vec_push(all, svx_new(a, SVX_MUL, svx_new(a, SVX_K, NULL, NULL, 2), b.data[i], 0));
    /* Tern widths of counts and shift amounts */
    for (i = 0; i < b.len; i++) {
        vec_push(all, svx_new(a, SVX_CLOG2, b.data[i], NULL, 0));
        vec_push(all, svx_new(a, SVX_CLOG2, svx_new(a, SVX_ADD, b.data[i], svx_new(a, SVX_K, NULL, NULL, 1), 0), NULL, 0));
    }
    for (i = 0; i < b.len && i < 24; i++)
        for (j = i + 1; j < b.len && j < 24; j++)
            vec_push(all, svx_new(a, SVX_ADD, b.data[i], b.data[j], 0));
    c->n = (uint32_t)all.len;
    c->nelab = nelab;
    c->x = (const SvXExpr **)xcalloc(all.len + 1, sizeof(SvXExpr *));
    c->vals = (int64_t *)xcalloc((size_t)all.len * nelab + 1, sizeof(int64_t));
    c->ok = (unsigned char *)xcalloc(all.len + 1, 1);
    for (i = 0; i < all.len; i++) {
        c->x[i] = all.data[i];
        c->ok[i] = 1;
        for (k = 0; k < nelab; k++)
            if (!sv_xexpr_eval(all.data[i], pvals[k], &c->vals[(size_t)i * nelab + k]))
                c->ok[i] = 0;
    }
    vec_free(all);
    vec_free(b);
}

void svp_cands_free(SvPCands *c)
{
    free(c->x);
    free(c->vals);
    free(c->ok);
    memset(c, 0, sizeof *c);
}

const SvXExpr *svp_match(const SvPCands *c, const int64_t *v, Arena *a)
{
    uint32_t i, k;
    TERN_UNUSED(a);
    for (i = 0; i < c->n; i++) {
        if (!c->ok[i])
            continue;
        for (k = 0; k < c->nelab; k++)
            if (c->vals[(size_t)i * c->nelab + k] != v[k])
                break;
        if (k == c->nelab)
            return c->x[i];
    }
    return NULL;
}

/* ------------------------------------------------------ lockstep walk -- */

SvPGroup *svp_group_of(const struct SvParamPlan *plan, const IrModule *m)
{
    uint64_t v;
    if (!plan || !m || !m->origin || !map_get(&plan->groups, MAP_PTR(m->origin->item), &v))
        return NULL;
    return (SvPGroup *)(uintptr_t)v;
}

typedef struct SvwCtx {
    SvPWalk *w;
    uint32_t k;               /* elaboration compared with 0 */
    size_t cursor;            /* next slot (passes after the first) */
    int first;                /* first pass: creates the slots */
} SvwCtx;

static int svw_fail(SvwCtx *c, const char *why)
{
    if (!c->w->why)
        c->w->why = why;
    return 0;
}

static int svw_slot(SvwCtx *c, int tag, const void *p0, const void *pk, int64_t v0, int64_t vk)
{
    SvPWalk *w = c->w;
    if (c->first) {
        SvPSlot s;
        uint32_t i;
        s.tag = tag;
        s.at = (uint32_t)w->keys.len;
        vec_push(w->slots, s);
        for (i = 0; i < w->n; i++) {
            vec_push(w->keys, 0);
            vec_push(w->vals, 0);
        }
        w->keys.data[s.at] = SVP_KEY(p0, tag);
        w->vals.data[s.at] = v0;
        w->keys.data[s.at + c->k] = SVP_KEY(pk, tag);
        w->vals.data[s.at + c->k] = vk;
        return 1;
    }
    if (c->cursor >= w->slots.len || w->slots.data[c->cursor].tag != tag)
        return svw_fail(c, "different structure");
    {
        SvPSlot *s = &w->slots.data[c->cursor++];
        w->keys.data[s->at + c->k] = SVP_KEY(pk, tag);
        w->vals.data[s->at + c->k] = vk;
    }
    return 1;
}

/* a slot keyed by (pointer + index) (child generic arguments) */
static int svw_slot_i(SvwCtx *c, int tag, const void *p0, const void *pk, uint32_t i, int64_t v0, int64_t vk)
{
    size_t at = c->first ? c->w->slots.len : c->cursor;
    if (!svw_slot(c, tag, p0, pk, v0, vk))
        return 0;
    {
        SvPSlot *s = &c->w->slots.data[at];
        if (c->first)
            c->w->keys.data[s->at] = SVP_KEYI(p0, i, tag);
        c->w->keys.data[s->at + c->k] = SVP_KEYI(pk, i, tag);
    }
    return 1;
}

static int svw_str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return a == b || strcmp(a, b) == 0;
}

static int svw_scalar(const IrType *t)
{
    return t->kind == IRT_BOOL || t->kind == IRT_BITS || t->kind == IRT_UINT || t->kind == IRT_SINT ||
           t->kind == IRT_CLOCK || t->kind == IRT_RESET;
}

static int svw_type(SvwCtx *c, const IrType *a, const IrType *b)
{
    uint32_t i;
    if (a == b)
        return 1;
    if (!a || !b || a->kind != b->kind || a->width != b->width || (a->name == NULL) != (b->name == NULL))
        return svw_fail(c, "different types");
    switch (a->kind) {
    case IRT_STRUCT:
        if (a->nfields != b->nfields)
            return svw_fail(c, "different types");
        for (i = 0; i < a->nfields; i++)
            if (!svw_str_eq(a->fields[i].name, b->fields[i].name) || a->fields[i].lsb != b->fields[i].lsb ||
                !svw_type(c, a->fields[i].type, b->fields[i].type))
                return svw_fail(c, "different types");
        break;
    case IRT_ENUM:
        if (a->nvariants != b->nvariants || a->tag_width != b->tag_width || a->encoding != b->encoding)
            return svw_fail(c, "different types");
        for (i = 0; i < a->nvariants; i++) {
            const IrVariant *va = &a->variants[i], *vb = &b->variants[i];
            if (!svw_str_eq(va->name, vb->name) || !ir_bits_eq(&va->tag, &vb->tag) ||
                (va->payload == NULL) != (vb->payload == NULL) ||
                (va->payload && !svw_type(c, va->payload, vb->payload)))
                return svw_fail(c, "different types");
        }
        break;
    case IRT_ARRAY:
        if (a->len != b->len || !svw_type(c, a->elem, b->elem))
            return svw_fail(c, "different types");
        break;
    case IRT_CLOCK:
        if (a->falling != b->falling)
            return svw_fail(c, "different types");
        break;
    case IRT_RESET:
        if (a->async != b->async || a->active_low != b->active_low)
            return svw_fail(c, "different types");
        break;
    default: break;
    }
    if (a->name) {
        SvPTypePair p;
        p.k = c->k;
        p.t0 = a;
        p.tk = b;
        vec_push(c->w->types, p);
    }
    return 1;
}

/* signal types: scalars may differ in width (a slot), the rest must match */
static int svw_sigtype(SvwCtx *c, const IrSignal *a, const IrSignal *b)
{
    const IrType *ta = a->type, *tb = b->type;
    if (svw_scalar(ta) && svw_scalar(tb)) {
        if (ta->kind != tb->kind || (ta->kind == IRT_CLOCK && ta->falling != tb->falling) ||
            (ta->kind == IRT_RESET && (ta->async != tb->async || ta->active_low != tb->active_low)))
            return svw_fail(c, "different signal types");
        return svw_slot(c, SVK_SIGW, a, b, ta->width, tb->width);
    }
    return svw_type(c, ta, tb);
}

static int svw_bits(SvwCtx *c, const IrBits *a, const IrBits *b)
{
    if (a->width <= 63 && b->width <= 63)
        return svw_slot(c, SVK_CONST, a->w, b->w, (int64_t)ir_bits_low64(a), (int64_t)ir_bits_low64(b));
    if (!ir_bits_eq(a, b))
        return svw_fail(c, "different wide constants");
    return 1;
}

static int svw_attrs(SvwCtx *c, uint32_t na, const IrAttr *a, uint32_t nb, const IrAttr *b)
{
    uint32_t i;
    if (na != nb)
        return svw_fail(c, "different attributes");
    for (i = 0; i < na; i++)
        if (!svw_str_eq(a[i].name, b[i].name) || !svw_str_eq(a[i].args, b[i].args))
            return svw_fail(c, "different attributes");
    return 1;
}

static int svw_expr(SvwCtx *c, const IrExpr *a, const IrExpr *b)
{
    uint32_t i;
    if (!a || !b)
        return a == b ? 1 : svw_fail(c, "different expressions");
    if (a->kind != b->kind || a->nargs != b->nargs || a->is_signed != b->is_signed ||
        ((a->kind == IRE_UNARY || a->kind == IRE_BINARY) && a->op != b->op) || !svw_str_eq(a->field, b->field))
        return svw_fail(c, "different expressions");
    if ((a->type == NULL) != (b->type == NULL))
        return svw_fail(c, "different expression types");
    if (a->type && (a->type->name || b->type->name) && !svw_type(c, a->type, b->type))
        return 0;
    if (a->type && a->type->kind != b->type->kind)
        return svw_fail(c, "different expression types");
    if (!svw_slot(c, SVK_EXPRW, a, b, a->width, b->width))
        return 0;
    switch (a->kind) {
    case IRE_CONST:
        if (!svw_bits(c, &a->bits, &b->bits))
            return 0;
        break;
    case IRE_SIG:
    case IRE_MEMREAD:
        if (a->sig != b->sig)
            return svw_fail(c, "different signals");
        break;
    case IRE_SLICE:
        if (!svw_slot(c, SVK_LO, a, b, a->lo, b->lo))
            return 0;
        break;
    case IRE_PAST:
        if (a->lo != b->lo)
            return svw_fail(c, "different $past depth");
        break;
    default: break;
    }
    for (i = 0; i < a->nargs; i++)
        if (!svw_expr(c, a->args[i], b->args[i]))
            return 0;
    return 1;
}

static int svw_lv(SvwCtx *c, const IrLvalue *a, const IrLvalue *b)
{
    if (a->sig != b->sig || a->len != b->len || (a->index == NULL) != (b->index == NULL) ||
        !svw_str_eq(a->field, b->field))
        return svw_fail(c, "different assignment targets");
    if (!svw_slot(c, SVK_LVLO, a, b, a->lo, b->lo) || !svw_slot(c, SVK_LVW, a, b, a->width, b->width))
        return 0;
    return a->index ? svw_expr(c, a->index, b->index) : 1;
}

static int svw_block(SvwCtx *c, const IrBlock *a, const IrBlock *b);

static int svw_stmt(SvwCtx *c, const IrStmt *a, const IrStmt *b)
{
    uint32_t i, j;
    if (a->kind != b->kind)
        return svw_fail(c, "different statements");
    switch (a->kind) {
    case IRST_ASSIGN: return svw_lv(c, &a->lhs, &b->lhs) && svw_expr(c, a->rhs, b->rhs);
    case IRST_IF:
        return svw_expr(c, a->cond, b->cond) && svw_block(c, &a->then_b, &b->then_b) &&
               svw_block(c, &a->else_b, &b->else_b);
    case IRST_CASE:
        if (!svw_expr(c, a->subject, b->subject) || a->narms != b->narms)
            return svw_fail(c, "different case statements");
        for (i = 0; i < a->narms; i++) {
            if (a->arms[i].nvals != b->arms[i].nvals)
                return svw_fail(c, "different case statements");
            for (j = 0; j < a->arms[i].nvals; j++)
                if (!svw_bits(c, &a->arms[i].vals[j], &b->arms[i].vals[j]))
                    return 0;
            if (!svw_block(c, &a->arms[i].body, &b->arms[i].body))
                return 0;
        }
        return 1;
    case IRST_MEMWRITE:
        if (a->mem != b->mem)
            return svw_fail(c, "different memory writes");
        return svw_expr(c, a->addr, b->addr) && svw_expr(c, a->data, b->data) && svw_expr(c, a->mask, b->mask);
    case IRST_ASSERT:
        if (a->akind != b->akind || !svw_str_eq(a->msg, b->msg))
            return svw_fail(c, "different assertions");
        return svw_expr(c, a->cond, b->cond);
    }
    return svw_fail(c, "unknown statement");
}

static int svw_block(SvwCtx *c, const IrBlock *a, const IrBlock *b)
{
    uint32_t i;
    if (a->n != b->n)
        return svw_fail(c, "different statement counts");
    for (i = 0; i < a->n; i++)
        if (!svw_stmt(c, a->stmts[i], b->stmts[i]))
            return 0;
    return 1;
}

static int svw_seq(SvwCtx *c, const IrSeq *a, const IrSeq *b)
{
    if (!a || !b)
        return a == b ? 1 : svw_fail(c, "different properties");
    if (a->kind != b->kind || a->lo != b->lo || a->hi != b->hi)
        return svw_fail(c, "different properties");
    return svw_expr(c, a->expr, b->expr) && svw_seq(c, a->a, b->a) && svw_seq(c, a->b, b->b);
}

static int svw_inst(SvwCtx *c, const IrInstance *a, const IrInstance *b)
{
    const IrModule *ca = a->module, *cb = b->module;
    SvPGroup *ga = svp_group_of(c->w->plan, ca), *gb = svp_group_of(c->w->plan, cb);
    uint32_t i;
    if (!svw_str_eq(a->name, b->name) || a->nconns != b->nconns)
        return svw_fail(c, "different instances");
    if (ga && ga->accepted && ga == gb) {
        if (ca->origin->ngenerics != cb->origin->ngenerics || ca->origin->ngenerics > 32)
            return svw_fail(c, "different instances");
        for (i = 0; i < ca->origin->ngenerics; i++)
            if (!svw_slot_i(c, SVK_INSTP, a, b, i, ca->origin->generics[i].value, cb->origin->generics[i].value))
                return 0;
    } else if (ca != cb && !svw_str_eq(ca->name, cb->name)) {
        return svw_fail(c, "instances of different modules");
    }
    for (i = 0; i < a->nconns; i++) {
        if (a->conns[i].port != b->conns[i].port)
            return svw_fail(c, "different connections");
        if (!svw_expr(c, a->conns[i].expr, b->conns[i].expr))
            return 0;
    }
    return 1;
}

static int svw_module(SvwCtx *c, const IrModule *a, const IrModule *b)
{
    uint32_t i, j;
    if (a->nsigs != b->nsigs || a->nports != b->nports || a->ndomains != b->ndomains ||
        a->default_domain != b->default_domain || a->nassigns != b->nassigns || a->nprocs != b->nprocs ||
        a->ninsts != b->ninsts || a->nprops != b->nprops || a->is_extern != b->is_extern || a->is_cdc != b->is_cdc)
        return svw_fail(c, "different module structure");
    if (!svw_attrs(c, a->nattrs, a->attrs, b->nattrs, b->attrs))
        return 0;
    for (i = 0; i < a->nports; i++)
        if (a->ports[i] != b->ports[i])
            return svw_fail(c, "different ports");
    for (i = 0; i < a->nsigs; i++) {
        const IrSignal *sa = &a->sigs[i], *sb = &b->sigs[i];
        if (!svw_str_eq(sa->name, sb->name) || !svw_str_eq(sa->path, sb->path) || sa->kind != sb->kind ||
            sa->domain != sb->domain || sa->has_init != sb->has_init || sa->test_port != sb->test_port ||
            sa->flipped != sb->flipped)
            return svw_fail(c, "different signals");
        if (sa->mem_init || sb->mem_init || sa->mem_init_file || sb->mem_init_file)
            return svw_fail(c, "initialized memory");
        if (!svw_attrs(c, sa->nattrs, sa->attrs, sb->nattrs, sb->attrs) || !svw_sigtype(c, sa, sb))
            return 0;
        if (sa->has_init) {
            if (svw_scalar(sa->type)) {
                if (!svw_bits(c, &sa->init, &sb->init))
                    return 0;
            } else if (!ir_bits_eq(&sa->init, &sb->init)) {
                return svw_fail(c, "different reset values");
            }
        }
        if (sa->kind == IRS_MEM && !svw_slot(c, SVK_DEPTH, sa, sb, sa->depth, sb->depth))
            return 0;
    }
    for (i = 0; i < a->ndomains; i++) {
        const IrDomain *da = &a->domains[i], *db = &b->domains[i];
        if (!svw_str_eq(da->name, db->name) || da->clock != db->clock || da->reset != db->reset || da->hz != db->hz)
            return svw_fail(c, "different domains");
    }
    for (i = 0; i < a->nassigns; i++)
        if (!svw_lv(c, &a->assigns[i].lhs, &b->assigns[i].lhs) || !svw_expr(c, a->assigns[i].rhs, b->assigns[i].rhs))
            return 0;
    for (i = 0; i < a->nprocs; i++) {
        const IrProcess *pa = &a->procs[i], *pb = &b->procs[i];
        if (pa->kind != pb->kind || pa->domain != pb->domain || pa->nregs != pb->nregs)
            return svw_fail(c, "different processes");
        for (j = 0; j < pa->nregs; j++)
            if (pa->regs[j] != pb->regs[j])
                return svw_fail(c, "different processes");
        if (!svw_block(c, &pa->body, &pb->body))
            return 0;
    }
    for (i = 0; i < a->ninsts; i++)
        if (!svw_inst(c, &a->insts[i], &b->insts[i]))
            return 0;
    for (i = 0; i < a->nprops; i++) {
        const IrProperty *pa = &a->props[i], *pb = &b->props[i];
        if (!svw_str_eq(pa->name, pb->name) || pa->kind != pb->kind || pa->domain != pb->domain)
            return svw_fail(c, "different properties");
        if (!svw_seq(c, pa->seq, pb->seq))
            return 0;
    }
    return 1;
}

int svp_walk(SvPWalk *w)
{
    uint32_t k;
    for (k = 1; k < w->n; k++) {
        SvwCtx c;
        memset(&c, 0, sizeof c);
        c.w = w;
        c.k = k;
        c.first = k == 1;
        if (!svw_module(&c, w->mods[0], w->mods[k]))
            return 0;
        if (!c.first && c.cursor != w->slots.len) {
            svw_fail(&c, "different structure");
            return 0;
        }
    }
    return 1;
}

void svp_walk_free(SvPWalk *w)
{
    vec_free(w->slots);
    vec_free(w->keys);
    vec_free(w->vals);
    vec_free(w->types);
}
