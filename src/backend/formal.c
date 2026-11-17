/* formal.c — `tern prove` (spec §11.5): SymbiYosys files for one module,
 * the sby result parser, and the counterexample -> Tern test converter.
 *
 * The command itself is in prove.c.
 *
 * Files in build/formal/<Module>/ (docs/ARCHITECTURE.md, "Back ends"):
 *   <Mod>.v ...           the module and everything below it as Verilog-2005
 *                         (what open-source Yosys reads best), each with a
 *                         checker block under `ifdef FORMAL` appended:
 *                         - every immediate assert/assume/cover/expect as an
 *                           `always @*` check guarded by its path condition
 *                           (the if/case branches leading to it) and by the
 *                           domain's reset;
 *                         - every property compiled to monitor registers
 *                           (history shift registers, obligation vectors,
 *                           first-cycle flags) plus one immediate check.
 *   tern_formal_top.v     wrapper: instantiates the module with free inputs
 *                         and assumes each top reset for the first
 *                         `reset_cycles` cycles and released afterwards,
 *                         exactly what `dut.reset()` does in a test.
 *   <Mod>.sby             tasks bmc / prove (--induct) / cover.
 *
 * The property translation follows the simulator's semantics (sim.h):
 * properties are sampled at the active edge, attempts are discarded in
 * reset, a property without `always` is one obligation at the first edge
 * out of reset, past(x, n) is 0 before n cycles exist.  Unbounded
 * obligations (`eventually` without a bound, `##[n:$]` in a consequent) are
 * liveness and cannot fail in a bounded proof; they are reported as skipped.
 */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/map.h"
#include "../base/diag.h"
#include "../os/os.h"
#include "backend.h"

/* ============================================================ generator */

void fv_options_init(FvOptions *o)
{
    memset(o, 0, sizeof *o);
    o->depth = 20;
    o->reset_cycles = 2;
}

typedef struct FvGen {
    FvPlan *plan;
    const IrDesign *d;
    SvNames *names;
    const IrModule *m;
    const char *pfx;          /* unique name prefix in this module */
    uint32_t ntmp;
    StrBuf decl;              /* wire / reg declarations */
    StrBuf body;              /* always blocks */
    Map cache;                /* identical wires: hash(rhs) -> {name, rhs} */
    Map hist;                 /* history registers: hash(key) -> name */
    VEC(uint32_t) pend_check; /* check indices waiting for a line number */
    VEC(uint32_t) pend_line;  /* their line within body */
    uint32_t body_lines;
    int errors;
    /* property context */
    uint32_t pdom;            /* domain of the property */
    const char *note;         /* why a property cannot be translated */
} FvGen;

static const char *fv_new_name(FvGen *g)
{
    return arena_printf(&g->plan->arena, "%s%u", g->pfx, g->ntmp++);
}

static void fv_body_line(FvGen *g, const char *fmt, ...) TERN_PRINTF(2, 3);
static void fv_body_line(FvGen *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&g->body, fmt, ap);
    va_end(ap);
    sb_putc(&g->body, '\n');
    g->body_lines++;
}

static const char *fv_range(FvGen *g, uint32_t w)
{
    return arena_printf(&g->plan->arena, "[%u:0]", w ? w - 1 : 0);
}

/* declares `wire [w-1:0] name = rhs;` and returns name */
static const char *fv_wire(FvGen *g, uint32_t w, const char *rhs)
{
    const char *n;
    uint64_t key = hash_bytes(rhs, strlen(rhs)) ^ hash_u64(w), v;
    /* reuse an identical wire */
    if (map_get(&g->cache, key, &v)) {
        const char **pair = (const char **)(uintptr_t)v;
        if (strcmp(pair[1], rhs) == 0)
            return pair[0];
    }
    n = fv_new_name(g);
    sb_printf(&g->decl, "    wire %s %s = %s;\n", fv_range(g, w), n, rhs);
    {
        const char **pair = ANEWN(&g->plan->arena, const char *, 2);
        pair[0] = n;
        pair[1] = arena_strdup(&g->plan->arena, rhs);
        map_put(&g->cache, key, MAP_PTR(pair));
    }
    return n;
}

static const char *fv_lit(FvGen *g, const IrBits *b)
{
    StrBuf sb = {0};
    const char *r;
    ir_bits_print(b, &sb);
    r = arena_strdup(&g->plan->arena, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static int fv_is_ident(const char *s)
{
    return tern_is_alpha((unsigned char)s[0]) || s[0] == '_';
}

static const IrDomain *fv_domain(FvGen *g, uint32_t dom)
{
    if (dom == IR_NONE || dom >= g->m->ndomains)
        dom = g->m->default_domain;
    if (dom == IR_NONE || dom >= g->m->ndomains)
        return NULL;
    return &g->m->domains[dom];
}

/* "rst" / "!rst_n" / NULL (no reset) */
static const char *fv_reset_active(FvGen *g, const IrDomain *dm)
{
    const IrSignal *r;
    const char *n;
    if (!dm || dm->reset == IR_NONE || dm->reset >= g->m->nsigs)
        return NULL;
    r = &g->m->sigs[dm->reset];
    n = sv_names_signal(g->names, g->m, dm->reset);
    if (r->type && r->type->kind == IRT_RESET && r->type->active_low)
        return arena_printf(&g->plan->arena, "!%s", n);
    return n;
}

/* "posedge clk" / "negedge clk" */
static const char *fv_edge(FvGen *g, const IrDomain *dm)
{
    const IrSignal *c;
    if (!dm || dm->clock == IR_NONE || dm->clock >= g->m->nsigs)
        return NULL;
    c = &g->m->sigs[dm->clock];
    return arena_printf(&g->plan->arena, "%s %s", c->type && c->type->falling ? "negedge" : "posedge",
                        sv_names_signal(g->names, g->m, dm->clock));
}

static const char *fv_expr(FvGen *g, const IrExpr *e);

/* the operand as an identifier (literals get a wire) */
static const char *fv_named(FvGen *g, const IrExpr *e)
{
    const char *x = fv_expr(g, e);
    if (!x)
        return NULL;
    if (fv_is_ident(x))
        return x;
    return fv_wire(g, e->width, x);
}

/* value of x (w bits) n cycles ago in the property domain: history
 * registers, power-on 0, never reset (sim: "0 before n cycles exist") */
static const char *fv_past_data(FvGen *g, const char *x, uint32_t w, uint32_t n)
{
    const char *edge = fv_edge(g, fv_domain(g, g->pdom)), *prev = x;
    uint32_t k;
    if (!edge) {
        g->note = "past() in a module without a clock";
        return NULL;
    }
    for (k = 1; k <= n; k++) {
        char key[512];
        uint64_t v;
        const char *h;
        snprintf(key, sizeof key, "past|%s|%s|%u", edge, prev, w);
        if (map_get(&g->hist, hash_bytes(key, strlen(key)), &v)) {
            prev = (const char *)(uintptr_t)v;
            continue;
        }
        h = fv_new_name(g);
        sb_printf(&g->decl, "    reg %s %s = %u'd0;\n", fv_range(g, w), h, w);
        fv_body_line(g, "    always @(%s) %s <= %s;", edge, h, prev);
        map_put(&g->hist, hash_bytes(key, strlen(key)), MAP_PTR(h));
        prev = h;
    }
    return prev;
}

static const char *fv_expr(FvGen *g, const IrExpr *e)
{
    Arena *A = &g->plan->arena;
    const char *a, *b;
    uint32_t W = e->width;
    switch (e->kind) {
    case IRE_CONST: return fv_lit(g, &e->bits);
    case IRE_SIG: return sv_names_signal(g->names, g->m, e->sig);
    case IRE_SLICE:
        if (!(a = fv_named(g, e->args[0])))
            return NULL;
        if (e->lo == 0 && W == e->args[0]->width)
            return a;
        if (W == 1)
            return fv_wire(g, 1, arena_printf(A, "%s[%u]", a, e->lo));
        return fv_wire(g, W, arena_printf(A, "%s[%u:%u]", a, e->lo + W - 1, e->lo));
    case IRE_INDEX:
        if (!(a = fv_named(g, e->args[0])) || !(b = fv_named(g, e->args[1])))
            return NULL;
        return fv_wire(g, W, arena_printf(A, "%s >> (%s * 32'd%u)", a, b, W));
    case IRE_UNARY:
        if (!(a = fv_expr(g, e->args[0])))
            return NULL;
        switch (e->op) {
        case IROP_NOT: return fv_wire(g, W, arena_printf(A, "~%s", a));
        case IROP_NEG: return fv_wire(g, W, arena_printf(A, "-%s", a));
        case IROP_LNOT: return fv_wire(g, W, arena_printf(A, "!%s", a));
        case IROP_RAND: return fv_wire(g, W, arena_printf(A, "&%s", a));
        case IROP_ROR: return fv_wire(g, W, arena_printf(A, "|%s", a));
        case IROP_RXOR: return fv_wire(g, W, arena_printf(A, "^%s", a));
        default: break;
        }
        break;
    case IRE_BINARY: {
        const char *sa, *sb2;
        if (!(a = fv_expr(g, e->args[0])) || !(b = fv_expr(g, e->args[1])))
            return NULL;
        sa = e->is_signed ? arena_printf(A, "$signed(%s)", a) : a;
        sb2 = e->is_signed ? arena_printf(A, "$signed(%s)", b) : b;
        switch (e->op) {
        case IROP_ADD: return fv_wire(g, W, arena_printf(A, "%s + %s", a, b));
        case IROP_SUB: return fv_wire(g, W, arena_printf(A, "%s - %s", a, b));
        case IROP_MUL: return fv_wire(g, W, arena_printf(A, "%s * %s", a, b));
        case IROP_AND: return fv_wire(g, W, arena_printf(A, "%s & %s", a, b));
        case IROP_OR: return fv_wire(g, W, arena_printf(A, "%s | %s", a, b));
        case IROP_XOR: return fv_wire(g, W, arena_printf(A, "%s ^ %s", a, b));
        case IROP_DIV:
            return fv_wire(g, W, arena_printf(A, "(%s == %u'd0) ? %u'd0 : %s / %s", b, W, W, sa, sb2));
        case IROP_MOD: return fv_wire(g, W, arena_printf(A, "(%s == %u'd0) ? %s : %s %% %s", b, W, a, sa, sb2));
        case IROP_SHL: return fv_wire(g, W, arena_printf(A, "%s << %s", a, b));
        case IROP_SHR: return fv_wire(g, W, arena_printf(A, "%s >> %s", a, b));
        case IROP_ASHR: return fv_wire(g, W, arena_printf(A, "$signed(%s) >>> %s", a, b));
        case IROP_EQ: return fv_wire(g, 1, arena_printf(A, "%s == %s", a, b));
        case IROP_NE: return fv_wire(g, 1, arena_printf(A, "%s != %s", a, b));
        case IROP_LT: return fv_wire(g, 1, arena_printf(A, "%s < %s", sa, sb2));
        case IROP_LE: return fv_wire(g, 1, arena_printf(A, "%s <= %s", sa, sb2));
        case IROP_GT: return fv_wire(g, 1, arena_printf(A, "%s > %s", sa, sb2));
        case IROP_GE: return fv_wire(g, 1, arena_printf(A, "%s >= %s", sa, sb2));
        case IROP_LAND: return fv_wire(g, 1, arena_printf(A, "%s && %s", a, b));
        case IROP_LOR: return fv_wire(g, 1, arena_printf(A, "%s || %s", a, b));
        default: break;
        }
        break;
    }
    case IRE_CONCAT: {
        StrBuf sb = {0};
        uint32_t i;
        const char *r;
        sb_putc(&sb, '{');
        for (i = 0; i < e->nargs; i++) {
            if (!(a = fv_expr(g, e->args[i]))) {
                sb_free(&sb);
                return NULL;
            }
            sb_printf(&sb, "%s%s", i ? ", " : "", a);
        }
        sb_putc(&sb, '}');
        r = fv_wire(g, W, sb_cstr(&sb));
        sb_free(&sb);
        return r;
    }
    case IRE_MUX: {
        const char *c;
        if (!(c = fv_expr(g, e->args[0])) || !(a = fv_expr(g, e->args[1])) || !(b = fv_expr(g, e->args[2])))
            return NULL;
        return fv_wire(g, W, arena_printf(A, "%s ? %s : %s", c, a, b));
    }
    case IRE_RESIZE: {
        uint32_t w = e->args[0]->width;
        if (!(a = fv_named(g, e->args[0])))
            return NULL;
        if (W == w)
            return a;
        if (W < w)
            return fv_wire(g, W, W == 1 ? arena_printf(A, "%s[0]", a) : arena_printf(A, "%s[%u:0]", a, W - 1));
        if (e->is_signed)
            return fv_wire(g, W,
                           w == 1 ? arena_printf(A, "{{%u{%s}}, %s}", W - w, a, a)
                                  : arena_printf(A, "{{%u{%s[%u]}}, %s}", W - w, a, w - 1, a));
        return fv_wire(g, W, arena_printf(A, "{%u'd0, %s}", W - w, a));
    }
    case IRE_MEMREAD:
        if (!(a = fv_named(g, e->args[0])))
            return NULL;
        return fv_wire(g, W, arena_printf(A, "%s[%s]", sv_names_signal(g->names, g->m, e->sig), a));
    case IRE_PAST:
        if (!(a = fv_named(g, e->args[0])))
            return NULL;
        return fv_past_data(g, a, W, e->lo);
    }
    g->note = "unsupported expression";
    return NULL;
}

/* 1-bit helpers over names */
static const char *fv_and(FvGen *g, const char *a, const char *b)
{
    if (!a || strcmp(a, "1'b1") == 0)
        return b;
    if (!b || strcmp(b, "1'b1") == 0)
        return a;
    return fv_wire(g, 1, arena_printf(&g->plan->arena, "%s && %s", a, b));
}

static const char *fv_or(FvGen *g, const char *a, const char *b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    return fv_wire(g, 1, arena_printf(&g->plan->arena, "%s || %s", a, b));
}

static const char *fv_not(FvGen *g, const char *a)
{
    return fv_wire(g, 1, arena_printf(&g->plan->arena, "!%s", a));
}

/* ------------------------------------------------------------ checks */

static uint32_t fv_add_check(FvGen *g, IrAssertKind kind, const char *name, const char *msg, Span loc)
{
    FvCheck c;
    memset(&c, 0, sizeof c);
    c.kind = kind == IRA_EXPECT ? IRA_ASSERT : kind;
    c.module = sv_names_module(g->names, g->m);
    c.inst_path = "";
    c.name = name;
    c.msg = msg;
    c.loc = loc;
    c.status = FV_UNKNOWN;
    vec_push(g->plan->checks, c);
    if (c.kind == IRA_COVER)
        g->plan->has_cover = 1;
    else if (c.kind == IRA_ASSERT)
        g->plan->has_assert = 1;
    return (uint32_t)(g->plan->checks.len - 1);
}

static void fv_skip_check(FvGen *g, uint32_t ci, const char *note)
{
    g->plan->checks.data[ci].status = FV_SKIPPED;
    g->plan->checks.data[ci].note = note;
}

/* `always @* if (guard) assert(cond);` and remember its line */
static void fv_emit_check(FvGen *g, uint32_t ci, const char *guard, const char *cond)
{
    const FvCheck *c = &g->plan->checks.data[ci];
    const char *kw = c->kind == IRA_ASSUME ? "assume" : c->kind == IRA_COVER ? "cover" : "assert";
    const SourceFile *sf = c->loc.file ? src_get(c->loc.file) : NULL;
    sb_printf(&g->body, "    // %s%s%s%s (%s:%u)\n", kw, c->name ? " " : "", c->name ? c->name : "",
              c->msg ? arena_printf(&g->plan->arena, ": \"%s\"", c->msg) : "", sf && sf->path ? sf->path : "?",
              (unsigned)c->loc.line);
    g->body_lines++;
    vec_push(g->pend_check, ci);
    vec_push(g->pend_line, g->body_lines + 1);
    if (guard && strcmp(guard, "1'b1") != 0)
        fv_body_line(g, "    always @* if (%s) %s (%s);", guard, kw, cond);
    else
        fv_body_line(g, "    always @* %s (%s);", kw, cond);
}

/* --------------------------------------------- immediate assertions */

static int fv_block_has_assert(const IrBlock *b)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (s->kind == IRST_ASSERT)
            return 1;
        if (s->kind == IRST_IF && (fv_block_has_assert(&s->then_b) || fv_block_has_assert(&s->else_b)))
            return 1;
        if (s->kind == IRST_CASE)
            for (k = 0; k < s->narms; k++)
                if (fv_block_has_assert(&s->arms[k].body))
                    return 1;
    }
    return 0;
}

static void fv_collect_writes(const IrBlock *b, Map *out)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN: map_put(out, s->lhs.sig, 1); break;
        case IRST_IF:
            fv_collect_writes(&s->then_b, out);
            fv_collect_writes(&s->else_b, out);
            break;
        case IRST_CASE:
            for (k = 0; k < s->narms; k++)
                fv_collect_writes(&s->arms[k].body, out);
            break;
        default: break;
        }
    }
}

static int fv_expr_reads(const IrExpr *e, const Map *sigs)
{
    uint32_t i;
    if (!e)
        return 0;
    if ((e->kind == IRE_SIG || e->kind == IRE_MEMREAD) && map_has(sigs, e->sig))
        return 1;
    for (i = 0; i < e->nargs; i++)
        if (fv_expr_reads(e->args[i], sigs))
            return 1;
    return 0;
}

typedef struct FvProcCtx {
    const IrProcess *p;
    const char *gate;     /* "not in reset" condition, or NULL */
    Map writes;           /* comb: signals the process assigns */
    int comb;
} FvProcCtx;

static void fv_walk(FvGen *g, FvProcCtx *pc, const IrBlock *b, const char *path, int path_dirty)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (s->kind == IRST_ASSERT) {
            uint32_t ci = fv_add_check(g, s->akind, NULL, s->msg, s->loc);
            const char *c;
            if (pc->comb && (path_dirty || fv_expr_reads(s->cond, &pc->writes))) {
                fv_skip_check(g, ci, "reads a variable its own comb block assigns");
                continue;
            }
            g->note = NULL;
            c = fv_expr(g, s->cond);
            if (!c) {
                fv_skip_check(g, ci, g->note ? g->note : "unsupported expression");
                continue;
            }
            fv_emit_check(g, ci, fv_and(g, pc->gate, path), c);
        } else if (s->kind == IRST_IF) {
            const char *c;
            int dirty;
            if (!fv_block_has_assert(&s->then_b) && !fv_block_has_assert(&s->else_b))
                continue;
            dirty = path_dirty || (pc->comb && fv_expr_reads(s->cond, &pc->writes));
            c = fv_expr(g, s->cond);
            if (!c)
                c = "1'b0", dirty = 1;
            if (fv_block_has_assert(&s->then_b))
                fv_walk(g, pc, &s->then_b, fv_and(g, path, c), dirty);
            if (fv_block_has_assert(&s->else_b))
                fv_walk(g, pc, &s->else_b, fv_and(g, path, fv_not(g, c)), dirty);
        } else if (s->kind == IRST_CASE) {
            const char *subj, *any = NULL;
            int dirty, has = 0;
            for (k = 0; k < s->narms; k++)
                has |= fv_block_has_assert(&s->arms[k].body);
            if (!has)
                continue;
            dirty = path_dirty || (pc->comb && fv_expr_reads(s->subject, &pc->writes));
            subj = fv_named(g, s->subject);
            if (!subj)
                subj = "1'b0", dirty = 1;
            /* arm conditions; a default arm takes what no other arm matched */
            for (k = 0; k < s->narms; k++) {
                const IrCaseArm *arm = &s->arms[k];
                const char *m = NULL;
                uint32_t v;
                for (v = 0; v < arm->nvals; v++)
                    m = fv_or(g, m, fv_wire(g, 1, arena_printf(&g->plan->arena, "%s == %s", subj,
                                                               fv_lit(g, &arm->vals[v]))));
                if (arm->nvals)
                    any = fv_or(g, any, m);
                if (arm->nvals && fv_block_has_assert(&arm->body))
                    fv_walk(g, pc, &arm->body, fv_and(g, path, m), dirty);
            }
            for (k = 0; k < s->narms; k++)
                if (!s->arms[k].nvals && fv_block_has_assert(&s->arms[k].body))
                    fv_walk(g, pc, &s->arms[k].body, fv_and(g, path, any ? fv_not(g, any) : NULL), dirty);
        }
    }
}

static void fv_gen_asserts(FvGen *g)
{
    uint32_t i;
    for (i = 0; i < g->m->nprocs; i++) {
        const IrProcess *p = &g->m->procs[i];
        FvProcCtx pc;
        const char *ra;
        if (!fv_block_has_assert(&p->body))
            continue;
        memset(&pc, 0, sizeof pc);
        pc.p = p;
        pc.comb = p->kind == IRP_COMB;
        if (pc.comb)
            fv_collect_writes(&p->body, &pc.writes);
        ra = fv_reset_active(g, fv_domain(g, pc.comb ? IR_NONE : p->domain));
        pc.gate = ra ? arena_printf(&g->plan->arena, "!(%s)", ra) : NULL;
        if (pc.gate)
            pc.gate = fv_wire(g, 1, pc.gate);
        g->pdom = pc.comb ? IR_NONE : p->domain;
        fv_walk(g, &pc, &p->body, NULL, 0);
        map_free(&pc.writes);
    }
}

/* ---------------------------------------------------------- properties */

typedef struct FvProp {
    FvGen *g;
    const IrDomain *dm;
    const char *edge;      /* "posedge clk" */
    const char *rst;       /* reset active expression, or NULL */
    const char *nrst;      /* "1'b1" or a wire: not in reset */
    const char *fail;      /* OR of failure terms */
    int ok;
} FvProp;

static int fv_seq_is_bool(const IrSeq *s)
{
    switch (s->kind) {
    case IRQ_EXPR: return 1;
    case IRQ_AND:
    case IRQ_OR: return fv_seq_is_bool(s->a) && fv_seq_is_bool(s->b);
    case IRQ_NOT: return fv_seq_is_bool(s->a);
    default: return 0;
    }
}

static const char *fv_bool(FvProp *p, const IrSeq *s)
{
    FvGen *g = p->g;
    const char *a, *b;
    switch (s->kind) {
    case IRQ_EXPR: return fv_named(g, s->expr);
    case IRQ_AND:
    case IRQ_OR:
        if (!(a = fv_bool(p, s->a)) || !(b = fv_bool(p, s->b)))
            return NULL;
        return s->kind == IRQ_AND ? fv_and(g, a, b) : fv_or(g, a, b);
    case IRQ_NOT:
        if (!(a = fv_bool(p, s->a)))
            return NULL;
        return fv_not(g, a);
    default: return NULL;
    }
}

/* a monitor register: reg [0:0] r = 0; cleared in reset, else next */
static const char *fv_mreg(FvProp *p, const char *next)
{
    FvGen *g = p->g;
    const char *r = fv_new_name(g);
    sb_printf(&g->decl, "    reg [0:0] %s = 1'b0;\n", r);
    if (p->rst)
        fv_body_line(g, "    always @(%s) %s <= (%s) ? 1'b0 : %s;", p->edge, r, p->rst, next);
    else
        fv_body_line(g, "    always @(%s) %s <= %s;", p->edge, r, next);
    return r;
}

/* x delayed by n cycles through monitor registers (cleared in reset) */
static const char *fv_delay(FvProp *p, const char *x, uint32_t n)
{
    FvGen *g = p->g;
    uint32_t k;
    for (k = 0; k < n; k++) {
        char key[512];
        uint64_t v;
        snprintf(key, sizeof key, "dly|%s|%s", p->edge, x);
        if (map_get(&g->hist, hash_bytes(key, strlen(key)), &v)) {
            x = (const char *)(uintptr_t)v;
            continue;
        }
        {
            const char *r = fv_mreg(p, x);
            map_put(&g->hist, hash_bytes(key, strlen(key)), MAP_PTR(r));
            x = r;
        }
    }
    return x;
}

static IrSeq *fv_seq_new(FvProp *p, IrSeqKind k, IrSeq *a, IrSeq *b, uint32_t lo, uint32_t hi)
{
    IrSeq *s = ANEW(&p->g->plan->arena, IrSeq);
    s->kind = k;
    s->a = a;
    s->b = b;
    s->lo = lo;
    s->hi = hi;
    return s;
}

static void fv_unsupported(FvProp *p, const char *why)
{
    if (p->ok)
        p->g->note = why;
    p->ok = 0;
}

/* end-match signal of a sequence: 1 when s has matched an interval ending now */
static const char *fv_match(FvProp *p, const IrSeq *s)
{
    FvGen *g = p->g;
    if (!p->ok)
        return NULL;
    if (fv_seq_is_bool(s))
        return fv_bool(p, s);
    switch (s->kind) {
    case IRQ_DELAY: {
        const char *ma, *mb, *r = NULL;
        uint32_t k;
        if (s->b->kind == IRQ_DELAY) /* a ##x (b ##y c) == (a ##x b) ##y c */
            return fv_match(p, fv_seq_new(p, IRQ_DELAY, fv_seq_new(p, IRQ_DELAY, s->a, s->b->a, s->lo, s->hi), s->b->b,
                                          s->b->lo, s->b->hi));
        if (!fv_seq_is_bool(s->b)) {
            fv_unsupported(p, "sequence operand after ## is not a boolean or a delay chain");
            return NULL;
        }
        if (!(ma = fv_match(p, s->a)) || !(mb = fv_bool(p, s->b)))
            return NULL;
        if (s->hi == IR_NONE) {
            /* a matched at least lo cycles ago: sticky flag */
            const char *h = fv_delay(p, ma, s->lo), *seen, *r2;
            seen = fv_new_name(g);
            sb_printf(&g->decl, "    reg [0:0] %s = 1'b0;\n", seen);
            r2 = fv_or(g, h, seen);
            if (p->rst)
                fv_body_line(g, "    always @(%s) %s <= (%s) ? 1'b0 : %s;", p->edge, seen, p->rst, r2);
            else
                fv_body_line(g, "    always @(%s) %s <= %s;", p->edge, seen, r2);
            return fv_and(g, r2, mb);
        }
        if (s->hi < s->lo || s->hi - s->lo > 256) {
            fv_unsupported(p, "delay range too large");
            return NULL;
        }
        for (k = s->lo; k <= s->hi; k++)
            r = fv_or(g, r, fv_delay(p, ma, k));
        return fv_and(g, r, mb);
    }
    case IRQ_OR: {
        const char *a = fv_match(p, s->a), *b = fv_match(p, s->b);
        return a && b ? fv_or(g, a, b) : NULL;
    }
    default: fv_unsupported(p, "unsupported sequence operator in an antecedent or cover"); return NULL;
    }
}

static void fv_fail(FvProp *p, const char *term)
{
    if (term)
        p->fail = fv_or(p->g, p->fail, term);
}

/* obligation: s must hold starting now wherever trig is 1 */
static void fv_oblig(FvProp *p, const IrSeq *s, const char *trig, int trig_always)
{
    FvGen *g = p->g;
    if (!p->ok)
        return;
    if (fv_seq_is_bool(s)) {
        const char *e = fv_bool(p, s);
        if (e)
            fv_fail(p, fv_and(g, trig, fv_not(g, e)));
        else
            fv_unsupported(p, g->note ? g->note : "unsupported expression");
        return;
    }
    switch (s->kind) {
    case IRQ_DELAY: {
        const char *a, *t0;
        if (s->a->kind == IRQ_DELAY) { /* (a ##x b) ##y c == a ##x (b ##y c) */
            fv_oblig(p, fv_seq_new(p, IRQ_DELAY, s->a->a, fv_seq_new(p, IRQ_DELAY, s->a->b, s->b, s->lo, s->hi),
                                   s->a->lo, s->a->hi),
                     trig, trig_always);
            return;
        }
        if (!fv_seq_is_bool(s->a)) {
            fv_unsupported(p, "sequence before ## is not a boolean");
            return;
        }
        if (!(a = fv_bool(p, s->a)))
            return;
        fv_fail(p, fv_and(g, trig, fv_not(g, a)));
        t0 = fv_and(g, trig, a);
        if (s->hi == s->lo) {
            fv_oblig(p, s->b, fv_delay(p, t0, s->lo), 0);
            return;
        }
        if (s->hi == IR_NONE) {
            fv_unsupported(p, "unbounded ##[n:$] obligation (liveness; not checked by a bounded proof)");
            return;
        }
        if (!fv_seq_is_bool(s->b)) {
            fv_unsupported(p, "a ranged delay must be followed by a boolean in a consequent");
            return;
        }
        {
            /* obligation vector: pending[k] = started k cycles ago, not discharged */
            const char *b = fv_bool(p, s->b), *prev = t0;
            uint32_t k;
            if (!b || s->hi - s->lo > 256) {
                fv_unsupported(p, "delay range too large");
                return;
            }
            for (k = 1; k <= s->hi; k++) {
                const char *keep = k - 1 >= s->lo ? fv_and(g, prev, fv_not(g, b)) : prev;
                prev = fv_mreg(p, keep);
            }
            fv_fail(p, fv_and(g, prev, fv_not(g, b)));
        }
        return;
    }
    case IRQ_EVENTUALLY:
        if (s->hi == IR_NONE) {
            fv_unsupported(p, "unbounded `eventually` (liveness; not checked by a bounded proof)");
            return;
        }
        {
            IrSeq one;
            memset(&one, 0, sizeof one);
            one.kind = IRQ_EXPR;
            one.expr = ir_const_u64(&g->plan->arena, 1, 1, span_none());
            fv_oblig(p, fv_seq_new(p, IRQ_DELAY, &one, s->a, 0, s->hi), trig, trig_always);
        }
        return;
    case IRQ_IMPL:
    case IRQ_IMPL_NEXT: {
        const char *m;
        if (trig_always)
            m = fv_match(p, s->a);
        else if (fv_seq_is_bool(s->a))
            m = fv_bool(p, s->a);
        else {
            fv_unsupported(p, "a sequence antecedent is only supported under `always`");
            return;
        }
        if (!m)
            return;
        m = fv_and(g, trig, m);
        if (s->kind == IRQ_IMPL_NEXT)
            m = fv_delay(p, m, 1);
        fv_oblig(p, s->b, m, 0);
        return;
    }
    case IRQ_UNTIL: {
        const char *a, *b, *pend, *act;
        if (!fv_seq_is_bool(s->a) || !fv_seq_is_bool(s->b)) {
            fv_unsupported(p, "`until` operands must be booleans");
            return;
        }
        if (!(a = fv_bool(p, s->a)) || !(b = fv_bool(p, s->b)))
            return;
        pend = fv_new_name(g);
        sb_printf(&g->decl, "    reg [0:0] %s = 1'b0;\n", pend);
        act = fv_or(g, trig, pend);
        fv_fail(p, fv_and(g, act, fv_wire(g, 1, arena_printf(&g->plan->arena, "!%s && !%s", b, a))));
        if (p->rst)
            fv_body_line(g, "    always @(%s) %s <= (%s) ? 1'b0 : (%s && !%s);", p->edge, pend, p->rst, act, b);
        else
            fv_body_line(g, "    always @(%s) %s <= %s && !%s;", p->edge, pend, act, b);
        return;
    }
    case IRQ_AND:
        fv_oblig(p, s->a, trig, trig_always);
        fv_oblig(p, s->b, trig, trig_always);
        return;
    case IRQ_ALWAYS:
    case IRQ_NEVER: {
        const char *act = trig;
        if (!trig_always) {
            const char *st = fv_new_name(g);
            sb_printf(&g->decl, "    reg [0:0] %s = 1'b0;\n", st);
            act = fv_or(g, trig, st);
            if (p->rst)
                fv_body_line(g, "    always @(%s) %s <= (%s) ? 1'b0 : %s;", p->edge, st, p->rst, act);
            else
                fv_body_line(g, "    always @(%s) %s <= %s;", p->edge, st, act);
        }
        if (s->kind == IRQ_ALWAYS)
            fv_oblig(p, s->a, act, trig_always);
        else {
            const char *m = trig_always || fv_seq_is_bool(s->a) ? fv_match(p, s->a) : NULL;
            if (!m) {
                fv_unsupported(p, "`never` of a sequence is only supported at the top of a property");
                return;
            }
            fv_fail(p, fv_and(g, act, m));
        }
        return;
    }
    default: fv_unsupported(p, "unsupported property operator"); return;
    }
}

static void fv_gen_property(FvGen *g, const IrProperty *pr)
{
    FvProp p;
    uint32_t ci = fv_add_check(g, pr->kind, pr->name ? pr->name : "property", NULL, pr->loc);
    const char *ra;
    memset(&p, 0, sizeof p);
    p.g = g;
    p.ok = 1;
    g->note = NULL;
    g->pdom = pr->domain;
    p.dm = fv_domain(g, pr->domain);
    p.edge = fv_edge(g, p.dm);
    if (!p.edge) {
        fv_skip_check(g, ci, "the property has no clock domain");
        return;
    }
    ra = fv_reset_active(g, p.dm);
    p.rst = ra;
    p.nrst = ra ? fv_wire(g, 1, arena_printf(&g->plan->arena, "!(%s)", ra)) : "1'b1";
    sb_printf(&g->body, "    // property %s\n", pr->name ? pr->name : "");
    g->body_lines++;
    if (pr->kind == IRA_COVER) {
        const IrSeq *s = pr->seq;
        const char *m = NULL;
        if (s->kind == IRQ_ALWAYS)
            s = s->a;
        if (s->kind == IRQ_IMPL || s->kind == IRQ_IMPL_NEXT) {
            const char *a = fv_match(&p, s->a), *c = fv_seq_is_bool(s->b) ? fv_bool(&p, s->b) : NULL;
            if (a && s->kind == IRQ_IMPL_NEXT)
                a = fv_delay(&p, a, 1);
            if (a && c)
                m = fv_and(g, a, c);
            else
                fv_unsupported(&p, "cover of an implication needs a boolean consequent");
        } else {
            m = fv_match(&p, s);
        }
        if (!p.ok || !m) {
            fv_skip_check(g, ci, g->note ? g->note : "unsupported cover");
            return;
        }
        fv_emit_check(g, ci, p.nrst, m);
        return;
    }
    if (pr->seq->kind == IRQ_ALWAYS || pr->seq->kind == IRQ_NEVER) {
        fv_oblig(&p, pr->seq, p.nrst, 1);
    } else {
        /* one obligation at the first edge out of reset */
        const char *first = fv_new_name(g), *trig;
        sb_printf(&g->decl, "    reg [0:0] %s = 1'b1;\n", first);
        if (ra)
            fv_body_line(g, "    always @(%s) %s <= (%s) ? 1'b1 : 1'b0;", p.edge, first, ra);
        else
            fv_body_line(g, "    always @(%s) %s <= 1'b0;", p.edge, first);
        trig = fv_and(g, first, p.nrst);
        fv_oblig(&p, pr->seq, trig, 0);
    }
    if (!p.ok) {
        fv_skip_check(g, ci, g->note ? g->note : "unsupported property");
        return;
    }
    if (!p.fail) {
        fv_skip_check(g, ci, "the property can never fail");
        return;
    }
    fv_emit_check(g, ci, p.nrst, fv_not(g, p.fail));
}

/* ------------------------------------------------------------ modules */

static int fv_module_has_checks(const IrModule *m)
{
    uint32_t i;
    if (m->nprops)
        return 1;
    for (i = 0; i < m->nprocs; i++)
        if (fv_block_has_assert(&m->procs[i].body))
            return 1;
    return 0;
}

static uint32_t fv_count_lines(const char *s, size_t n)
{
    uint32_t c = 0;
    size_t i;
    for (i = 0; i < n; i++)
        c += s[i] == '\n';
    return c;
}

/* the checker block appended to module m's file (before its endmodule) */
static void fv_gen_module(FvGen *g, const IrModule *m, SvFile *file)
{
    uint32_t i;
    StrBuf out = {0};
    const char *endm;
    size_t cut;
    uint32_t base;
    int clash = 1;
    g->m = m;
    g->ntmp = 0;
    sb_reset(&g->decl);
    sb_reset(&g->body);
    map_clear(&g->cache);
    map_clear(&g->hist);
    vec_clear(g->pend_check);
    vec_clear(g->pend_line);
    g->body_lines = 0;
    g->pfx = "tern_fv_";
    while (clash) {
        clash = 0;
        for (i = 0; i < m->nsigs && !clash; i++)
            if (tern_starts_with(sv_names_signal(g->names, m, i), g->pfx))
                clash = 1;
        for (i = 0; i < m->ninsts && !clash; i++)
            if (tern_starts_with(sv_names_instance(g->names, m, i), g->pfx))
                clash = 1;
        if (clash)
            g->pfx = arena_printf(&g->plan->arena, "%sx_", g->pfx);
    }
    fv_gen_asserts(g);
    for (i = 0; i < m->nprops; i++)
        fv_gen_property(g, &m->props[i]);
    /* splice before the last `endmodule` */
    endm = NULL;
    {
        const char *p = file->data;
        while ((p = strstr(p, "endmodule")) != NULL) {
            endm = p;
            p += 9;
        }
    }
    cut = endm ? (size_t)(endm - file->data) : file->len;
    sb_append(&out, file->data, cut);
    sb_puts(&out, "`ifdef FORMAL\n    // ---- formal checker generated by tern prove ----\n");
    sb_append(&out, g->decl.data ? g->decl.data : "", g->decl.len);
    base = fv_count_lines(out.data, out.len);
    sb_append(&out, g->body.data ? g->body.data : "", g->body.len);
    sb_puts(&out, "`endif\n");
    sb_append(&out, file->data + cut, file->len - cut);
    for (i = 0; i < g->pend_check.len; i++) {
        FvCheck *c = &g->plan->checks.data[g->pend_check.data[i]];
        c->file = arena_strdup(&g->plan->arena, file->name);
        c->line = base + g->pend_line.data[i];
    }
    free(file->data);
    file->len = out.len;
    file->data = sb_take(&out);
}

static void fv_port_decl(StrBuf *sb, const char *dir, uint32_t w, const char *name, int last)
{
    if (w > 1)
        sb_printf(sb, "    %s wire [%u:0] %s%s\n", dir, w - 1, name, last ? "" : ",");
    else
        sb_printf(sb, "    %s wire %s%s\n", dir, name, last ? "" : ",");
}

static void fv_gen_wrapper(FvGen *g, const IrModule *m, const FvOptions *o, StrBuf *sb)
{
    uint32_t i, d;
    const char *mname = sv_names_module(g->names, m);
    Map seen = {0};
    unsigned cw = 1;
    while ((1u << cw) <= o->reset_cycles)
        cw++;
    sb_printf(sb, "// tern_formal_top.v: formal wrapper generated by tern prove for %s. Do not edit.\n", m->display);
    sb_printf(sb, "module tern_formal_top (\n");
    for (i = 0; i < m->nports; i++) {
        const IrSignal *s = &m->sigs[m->ports[i]];
        fv_port_decl(sb, s->kind == IRS_IN ? "input " : "output", s->type ? s->type->width : 1,
                     sv_names_signal(g->names, m, m->ports[i]), i + 1 == m->nports);
    }
    sb_printf(sb, ");\n\n    %s dut (\n", mname);
    for (i = 0; i < m->nports; i++) {
        const char *n = sv_names_signal(g->names, m, m->ports[i]);
        sb_printf(sb, "        .%s(%s)%s\n", n, n, i + 1 == m->nports ? "" : ",");
    }
    sb_puts(sb, "    );\n\n`ifdef FORMAL\n");
    sb_printf(sb, "    // every top reset is asserted for the first %u cycles of its domain and\n"
                  "    // released afterwards (as `dut.reset()` in a test)\n",
              (unsigned)o->reset_cycles);
    for (d = 0; d < m->ndomains; d++) {
        const IrDomain *dm = &m->domains[d];
        const IrSignal *r;
        const char *rn, *cn;
        int low;
        if (dm->reset == IR_NONE || dm->clock == IR_NONE || m->sigs[dm->reset].kind != IRS_IN ||
            m->sigs[dm->clock].kind != IRS_IN || map_has(&seen, dm->reset))
            continue;
        map_put(&seen, dm->reset, 1);
        r = &m->sigs[dm->reset];
        low = r->type && r->type->active_low;
        rn = sv_names_signal(g->names, m, dm->reset);
        cn = sv_names_signal(g->names, m, dm->clock);
        sb_printf(sb, "    reg [%u:0] tern_fv_rst_cnt_%u = %u'd0;\n", cw - 1, d, cw);
        sb_printf(sb, "    always @(%s %s) if (tern_fv_rst_cnt_%u != %u'd%u) tern_fv_rst_cnt_%u <= tern_fv_rst_cnt_%u + %u'd1;\n",
                  m->sigs[dm->clock].type && m->sigs[dm->clock].type->falling ? "negedge" : "posedge", cn, d, cw,
                  (unsigned)o->reset_cycles, d, d, cw);
        sb_printf(sb, "    always @* if (tern_fv_rst_cnt_%u != %u'd%u) assume (%s%s); else assume (%s%s);\n", d, cw,
                  (unsigned)o->reset_cycles, low ? "!" : "", rn, low ? "" : "!", rn);
    }
    sb_puts(sb, "`endif\n\nendmodule\n");
    map_free(&seen);
}

/* the modules below m (m included), children first, in design order */
static void fv_mark(const IrModule *m, Map *keep)
{
    uint32_t i;
    if (map_has(keep, MAP_PTR(m)))
        return;
    map_put(keep, MAP_PTR(m), 1);
    for (i = 0; i < m->ninsts; i++)
        fv_mark(m->insts[i].module, keep);
}

int fv_generate(const IrDesign *d, const IrModule *m, const FvOptions *o, FvPlan *plan)
{
    IrDesign *sub;
    Map keep = {0};
    uint32_t i, nclocks = 0;
    SvOptions so;
    FvGen g;
    StrBuf sb = {0};
    int errs;
    memset(plan, 0, sizeof *plan);
    plan->module = m;
    plan->wrapper = "tern_formal_top";
    /* a design restricted to m and its descendants, with m as the top */
    fv_mark(m, &keep);
    sub = ANEW(&plan->arena, IrDesign);
    *sub = *d;
    sub->modules = ANEWN(&plan->arena, IrModule *, d->nmodules ? d->nmodules : 1);
    sub->nmodules = 0;
    for (i = 0; i < d->nmodules; i++)
        if (map_has(&keep, MAP_PTR(d->modules[i])) && d->modules[i] != m)
            sub->modules[sub->nmodules++] = d->modules[i];
    sub->modules[sub->nmodules++] = (IrModule *)m;
    sub->top = (IrModule *)m;
    sub->nregmaps = 0;
    sub->ncrossings = 0;
    map_free(&keep);

    sv_options_init(&so);
    so.std = SV_STD_V2005;
    so.emit_assertions = 0;
    so.constraints = 0;
    so.regmaps = 0;
    errs = emit_design(sub, &so, &plan->files);
    if (errs)
        return errs;

    memset(&g, 0, sizeof g);
    g.plan = plan;
    g.d = sub;
    g.names = sv_names_new(sub);
    plan->name = arena_strdup(&plan->arena, sv_names_module(g.names, m));
    for (i = 0; i < sub->nmodules; i++) {
        const IrModule *mi = sub->modules[i];
        SvFile *f;
        char *fname;
        if (mi->is_extern || !fv_module_has_checks(mi))
            continue;
        fname = arena_printf(&plan->arena, "%s.v", sv_names_module(g.names, mi));
        f = (SvFile *)sv_output_find(&plan->files, fname);
        if (f)
            fv_gen_module(&g, mi, f);
    }
    for (i = 0; i < m->nports; i++) {
        const IrSignal *s = &m->sigs[m->ports[i]];
        if (s->kind == IRS_IN && s->type && s->type->kind == IRT_CLOCK)
            nclocks++;
    }
    plan->multiclock = nclocks > 1;
    fv_gen_wrapper(&g, m, o, &sb);
    sv_output_add(&plan->files, "tern_formal_top.v", SV_FILE_MODULE, &sb);

    /* the .sby file */
    sb_printf(&sb, "# %s.sby: generated by tern prove for %s. Do not edit.\n", plan->name, m->display);
    sb_puts(&sb, "[tasks]\nbmc\n");
    if (o->induct)
        sb_puts(&sb, "prove\n");
    if (plan->has_cover)
        sb_puts(&sb, "cover\n");
    sb_puts(&sb, "\n[options]\nbmc: mode bmc\n");
    if (o->induct)
        sb_puts(&sb, "prove: mode prove\n");
    if (plan->has_cover)
        sb_puts(&sb, "cover: mode cover\n");
    sb_printf(&sb, "depth %u\n", (unsigned)o->depth);
    if (plan->multiclock)
        sb_puts(&sb, "multiclock on\n");
    sb_printf(&sb, "\n[engines]\n%s\n\n[script]\n", o->engine && *o->engine ? o->engine : "smtbmc yices");
    for (i = 0; i < plan->files.nfiles; i++)
        if (plan->files.files[i].kind == SV_FILE_MODULE)
            sb_printf(&sb, "read_verilog -formal %s\n", plan->files.files[i].name);
    sb_printf(&sb, "prep -top %s\n\n[files]\n", plan->wrapper);
    for (i = 0; i < plan->files.nfiles; i++)
        sb_printf(&sb, "%s\n", plan->files.files[i].name);
    plan->sby = xstrdup(sb_cstr(&sb));
    {
        char *n = arena_printf(&plan->arena, "%s.sby", plan->name);
        sv_output_add(&plan->files, n, SV_FILE_MARKDOWN, &sb);
    }
    sb_free(&sb);
    sb_free(&g.decl);
    sb_free(&g.body);
    map_free(&g.cache);
    map_free(&g.hist);
    vec_free(g.pend_check);
    vec_free(g.pend_line);
    sv_names_free(g.names);
    return g.errors;
}

void fv_plan_free(FvPlan *plan)
{
    sv_output_free(&plan->files);
    vec_free(plan->checks);
    free(plan->sby);
    arena_release(&plan->arena);
    memset(plan, 0, sizeof *plan);
}

/* =========================================================== sby output */

/* "<file>.v:<line>" anywhere in s; returns 1 and the parts */
static int fv_find_loc(const char *s, char *file, size_t fcap, uint32_t *line)
{
    const char *p;
    for (p = s; (p = strstr(p, ".v:")) != NULL; p++) {
        const char *b = p, *q = p + 3;
        size_t n;
        uint32_t l = 0;
        if (!tern_is_digit((unsigned char)*q))
            continue;
        while (b > s && !tern_is_space((unsigned char)b[-1]) && b[-1] != '(' && b[-1] != '/' && b[-1] != '\'' &&
               b[-1] != '$')
            b--;
        while (tern_is_digit((unsigned char)*q))
            l = l * 10 + (uint32_t)(*q++ - '0');
        n = (size_t)(p + 2 - b);
        if (n >= fcap)
            n = fcap - 1;
        memcpy(file, b, n);
        file[n] = 0;
        *line = l;
        return 1;
    }
    return 0;
}

static int fv_find_step(const char *s, uint32_t *step)
{
    const char *p = strstr(s, "step ");
    uint32_t v = 0;
    if (!p || !tern_is_digit((unsigned char)p[5]))
        return 0;
    for (p += 5; tern_is_digit((unsigned char)*p); p++)
        v = v * 10 + (uint32_t)(*p - '0');
    *step = v;
    return 1;
}

static FvCheck *fv_check_at(FvPlan *plan, const char *file, uint32_t line)
{
    size_t i;
    for (i = 0; i < plan->checks.len; i++) {
        FvCheck *c = &plan->checks.data[i];
        if (c->file && c->line == line && strcmp(c->file, file) == 0)
            return c;
    }
    return NULL;
}

/* the text after "marker", trimmed, up to the end of the line */
static const char *fv_after(FvPlan *plan, const char *line, const char *marker)
{
    const char *p = strstr(line, marker), *e;
    if (!p)
        return NULL;
    p += strlen(marker);
    while (*p == ' ')
        p++;
    e = p + strlen(p);
    while (e > p && tern_is_space((unsigned char)e[-1]))
        e--;
    return arena_strndup(&plan->arena, p, (size_t)(e - p));
}

FvStatus fv_parse_sby_log(FvPlan *plan, const char *task, const char *log)
{
    const char *p = log;
    FvStatus st = FV_UNKNOWN;
    const char *trace = NULL;
    uint32_t last_step = 0;
    int have_step = 0, done = 0;
    char *prefix = arena_printf(&plan->arena, "%s_%s/", plan->name ? plan->name : "", task);
    size_t i;
    VEC(FvCheck *) failed = {0};
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = (char *)xmalloc(n + 1);
        char file[256];
        uint32_t ln, step;
        const char *t;
        memcpy(line, p, n);
        line[n] = 0;
        if (strstr(line, "Checking assertions in step ") || strstr(line, "Checking cover reachability in step ")) {
            if (fv_find_step(line, &step)) {
                last_step = step;
                have_step = 1;
            }
        }
        if ((strstr(line, "Assert failed in") || strstr(line, "failed assertion")) && fv_find_loc(line, file, sizeof file, &ln)) {
            FvCheck *c = fv_check_at(plan, file, ln);
            if (c) {
                size_t k;
                int dup = 0;
                c->status = FV_FAIL;
                if (strstr(line, "Assert failed in"))
                    c->trace = NULL; /* the trace file named next */
                if (fv_find_step(line, &step))
                    c->step = step;
                else if (have_step)
                    c->step = last_step;
                for (k = 0; k < failed.len; k++)
                    dup |= failed.data[k] == c;
                if (!dup)
                    vec_push(failed, c);
            }
        }
        if (strstr(line, "Unreached cover statement") && fv_find_loc(line, file, sizeof file, &ln)) {
            FvCheck *c = fv_check_at(plan, file, ln);
            if (c)
                c->status = FV_UNREACHED;
        } else if (strstr(line, "Reached cover statement") && fv_find_loc(line, file, sizeof file, &ln)) {
            FvCheck *c = fv_check_at(plan, file, ln);
            if (c) {
                c->status = FV_REACHED;
                c->trace = NULL; /* the trace file named next */
                if (fv_find_step(line, &step))
                    c->step = step;
                vec_push(failed, c); /* the next trace file belongs to it */
            }
        }
        if ((t = fv_after(plan, line, "Writing trace to VCD file:")) != NULL) {
            trace = arena_printf(&plan->arena, "%s%s", prefix, t);
            for (i = 0; i < failed.len; i++)
                if (!failed.data[i]->trace)
                    failed.data[i]->trace = trace;
        }
        if (strstr(line, "DONE (")) {
            const char *d = strstr(line, "DONE (") + 6;
            done = 1;
            if (strncmp(d, "PASS", 4) == 0)
                st = FV_PASS;
            else if (strncmp(d, "FAIL", 4) == 0)
                st = FV_FAIL;
            else
                st = FV_UNKNOWN;
        }
        free(line);
        p = e ? e + 1 : p + n;
    }
    TERN_UNUSED(done);
    if (st == FV_PASS && strcmp(task, "cover") != 0)
        for (i = 0; i < plan->checks.len; i++) {
            FvCheck *c = &plan->checks.data[i];
            if (c->kind != IRA_COVER && c->status == FV_UNKNOWN)
                c->status = FV_PASS;
        }
    if (strcmp(task, "cover") == 0 && (st == FV_PASS || st == FV_FAIL))
        for (i = 0; i < plan->checks.len; i++) {
            FvCheck *c = &plan->checks.data[i];
            if (c->kind == IRA_COVER && c->status == FV_UNKNOWN)
                c->status = st == FV_PASS ? FV_REACHED : FV_UNREACHED;
        }
    vec_free(failed);
    return st;
}

/* ================================================== counterexample test */

static uint64_t fv_bin_u64(const char *v, size_t n)
{
    uint64_t r = 0;
    size_t i;
    for (i = 0; i < n; i++)
        r = (r << 1) | (uint64_t)(v[i] == '1');
    return r;
}

/* the value (MSB-first digit string of exactly t->width bits) as Tern */
static void fv_tern_value(const IrType *t, const char *v, StrBuf *out)
{
    uint32_t w = t ? t->width : (uint32_t)strlen(v), i;
    switch (t ? t->kind : IRT_UINT) {
    case IRT_BOOL:
    case IRT_CLOCK:
    case IRT_RESET: sb_puts(out, v[w - 1] == '1' ? "true" : "false"); return;
    case IRT_ENUM: {
        uint64_t tag = fv_bin_u64(v, t->tag_width);
        for (i = 0; i < t->nvariants; i++)
            if (ir_bits_low64(&t->variants[i].tag) == tag && !t->variants[i].payload) {
                sb_printf(out, ".%s", t->variants[i].name);
                return;
            }
        break;
    }
    case IRT_STRUCT:
        if (t->name) {
            sb_printf(out, "%s { ", t->name);
            for (i = 0; i < t->nfields; i++) {
                const IrField *f = &t->fields[i];
                sb_printf(out, "%s%s: ", i ? ", " : "", f->name);
                fv_tern_value(f->type, v + (w - f->lsb - f->type->width), out);
            }
            sb_puts(out, " }");
            return;
        }
        break;
    case IRT_ARRAY:
        sb_putc(out, '[');
        for (i = 0; i < t->len; i++) {
            uint32_t ew = t->elem->width;
            char *ev = xstrndup(v + (w - (i + 1) * ew), ew);
            if (i)
                sb_puts(out, ", ");
            fv_tern_value(t->elem, ev, out);
            free(ev);
        }
        sb_putc(out, ']');
        return;
    case IRT_SINT:
        if (w <= 64 && v[0] == '1') {
            uint64_t u = fv_bin_u64(v, w);
            uint64_t mag = (~u + 1) & (w == 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1));
            sb_printf(out, "-%llu", (unsigned long long)mag);
            return;
        }
        break;
    default: break;
    }
    if (w <= 64) {
        uint64_t u = fv_bin_u64(v, w);
        if (u < 10)
            sb_printf(out, "%llu", (unsigned long long)u);
        else
            sb_printf(out, "0x%llx", (unsigned long long)u);
        return;
    }
    /* wide: hex digits from the binary string */
    sb_puts(out, "0x");
    {
        uint32_t nd = (w + 3) / 4, d;
        int started = 0;
        for (d = nd; d-- > 0;) {
            unsigned nib = 0, b;
            for (b = 0; b < 4; b++) {
                uint32_t bit = d * 4 + b;
                if (bit < w && v[w - 1 - bit] == '1')
                    nib |= 1u << b;
            }
            if (!started && nib == 0 && d)
                continue;
            started = 1;
            sb_putc(out, "0123456789abcdef"[nib]);
        }
    }
}

int fv_cex_to_test(const FvCex *cx, const VcdrTrace *t, StrBuf *out, char **err)
{
    const IrModule *m = cx->module;
    VEC(int) vars = {0};
    VEC(uint32_t) sigs = {0};
    VEC(const char *) last = {0};
    uint64_t *steps = NULL;
    size_t nsteps = 0, k, i;
    uint32_t pending = 0, first = cx->reset_cycles;
    int clk = -1;
    *err = NULL;
    /* the inputs to drive: top inputs except clocks and resets */
    for (i = 0; i < m->nports; i++) {
        const IrSignal *s = &m->sigs[m->ports[i]];
        const char *svn = cx->names ? sv_names_signal(cx->names, m, m->ports[i]) : s->name;
        StrBuf full = {0};
        int v;
        if (s->kind != IRS_IN)
            continue;
        if (cx->scope)
            sb_printf(&full, "%s.", cx->scope);
        sb_puts(&full, svn);
        v = vcdr_find(t, full.data);
        sb_free(&full);
        if (s->type && s->type->kind == IRT_CLOCK) {
            if (clk < 0)
                clk = v;
            continue;
        }
        if (s->type && s->type->kind == IRT_RESET)
            continue;
        if (v < 0) {
            StrBuf e = {0};
            sb_printf(&e, "input `%s` of %s is not in the trace", svn, m->name);
            *err = sb_take(&e);
            vec_free(vars);
            vec_free(sigs);
            vec_free(last);
            return -1;
        }
        vec_push(vars, v);
        vec_push(sigs, m->ports[i]);
        vec_push(last, NULL);
    }
    if (cx->clock) {
        int c = vcdr_find(t, cx->clock);
        if (c >= 0)
            clk = c;
    }
    if (clk >= 0)
        nsteps = vcdr_rising_edges(t, clk, &steps);
    if (nsteps == 0) {
        free(steps);
        nsteps = t->ntimes;
        steps = (uint64_t *)xcalloc(nsteps ? nsteps : 1, sizeof(uint64_t));
        if (nsteps)
            memcpy(steps, t->times, nsteps * sizeof(uint64_t));
    }
    if (nsteps == 0) {
        *err = xstrdup("the trace has no time steps");
        vec_free(vars);
        vec_free(sigs);
        vec_free(last);
        free(steps);
        return -1;
    }
    if (cx->fail_step && cx->fail_step < nsteps)
        nsteps = cx->fail_step + 1;

    sb_printf(out, "// Generated by `tern prove` from a counterexample: %s.\n",
              cx->failing ? cx->failing : "a failing check");
    if (cx->source_line)
        sb_printf(out, "//     %s\n", cx->source_line);
    sb_puts(out, "// The simulator reports the failing check during the last step. Registers\n"
                 "// without a reset value are free in the proof; if the test passes, the\n"
                 "// failure depends on their power-on values.\n");
    if (cx->package)
        sb_printf(out, "package %s\n", cx->package);
    if (cx->use_path)
        sb_printf(out, "%suse %s\n", cx->package ? "\n" : "", cx->use_path);
    if (cx->package || cx->use_path)
        sb_putc(out, '\n');
    sb_printf(out, "test \"%s\" {\n", cx->test_name ? cx->test_name : "formal counterexample");
    sb_printf(out, "    let dut = sim %s()\n", m->display);
    if (first > 0)
        sb_puts(out, "    dut.reset()\n");
    else
        first = 0;
    for (k = first; k < nsteps; k++) {
        StrBuf line = {0};
        for (i = 0; i < vars.len; i++) {
            const char *v = vcdr_value_at(t, vars.data[i], steps[k]);
            const IrSignal *s = &m->sigs[sigs.data[i]];
            if (last.data[i] && strcmp(last.data[i], v) == 0)
                continue;
            last.data[i] = v;
            if (line.len)
                sb_puts(&line, "; ");
            sb_printf(&line, "dut.%s = ", s->path ? s->path : s->name);
            fv_tern_value(s->type, v, &line);
        }
        if (line.len) {
            if (pending == 1)
                sb_puts(out, "    dut.step()\n");
            else if (pending > 1)
                sb_printf(out, "    dut.step(%u)\n", pending);
            pending = 0;
            sb_printf(out, "    %s\n", sb_cstr(&line));
        }
        pending++;
        sb_free(&line);
    }
    if (pending == 1)
        sb_puts(out, "    dut.step()\n");
    else if (pending > 1)
        sb_printf(out, "    dut.step(%u)\n", pending);
    sb_puts(out, "}\n");
    vec_free(vars);
    vec_free(sigs);
    vec_free(last);
    free(steps);
    return 0;
}
