/* testrun.c — the `test` block interpreter on the built-in simulator
 * (spec §11.1, §11.2; interface and semantics in testrun.h). */
#include "testrun.h"
#include "sim_impl.h"
#include "../base/diag.h"
#include "../base/bignum.h"
#include "../elab/elab_impl.h"
#include "../os/os.h"

#define TR_MAX_FAILS 8            /* failure entries printed per test */
#define TR_STEP_UNTIL_MAX 1000

typedef struct TrCtx TrCtx;

/* One `sim M<..>(..)` handle: the elaborated design (with the elaborator
 * kept alive for its type cache) and the simulator model. */
typedef struct TrSim {
    TrCtx *ctx;
    Arena ira;
    IrDesign *d;
    Elab *del;
    ModInfo *mi;
    SimModel *m;
    Map irty;                 /* IrType* -> Ty* (named and aggregate types) */
    const char *display;      /* "Fifo<uint<8>, 4>" */
    char *vcd_path;           /* --waves file (removed when the test passes) */
} TrSim;

/* A value reachable from a handle that is not a plain signal value. */
enum { TRN_ROOT, TRN_PATH, TRN_DOMAIN, TRN_MEM };
typedef struct TrNode {
    TrSim *s;
    int kind;
    const char *path;         /* "" (root), "push", "rx_fifo", "out[1]" */
    Ty *ty;                   /* TRN_PATH: bundle type when known */
    int h;                    /* TRN_MEM: signal handle; TRN_DOMAIN: domain index */
} TrNode;

struct TrCtx {
    Driver *dr;
    Elab *el;
    const TestRunOptions *opt;
    TestCov *cov;
    StrBuf *out;
    uint64_t rng;
    int failed;
    int stopped;
    uint32_t nfails;          /* failure entries (printed up to TR_MAX_FAILS) */
    uint32_t nsims;
    VEC(TrSim *) sims;
    TrSim *last;              /* most recent handle: its cycle is printed */
    int cover_noted;          /* the coverage note for a verilator model was printed */
};

static const ValOpaqueOps tr_ops;
static Value tr_read_bundle(struct TrSim *s, const char *path, Ty *t);

/* ------------------------------------------------------------ helpers -- */

static uint64_t tr_splitmix(uint64_t *s)
{
    uint64_t z = (*s += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

char *testrun_simcache_dir(Driver *dr)
{
    char *env = os_getenv("TERN_SIMCACHE");
    if (env && *env)
        return env;
    free(env);
    if (dr && dr->has_project)
        return os_path_join(dr->prj.manifest.root, "build/simcache");
    return NULL;
}

int testrun_set_engine(Driver *dr, const char *cmd, const char *engine, int verbose)
{
    SimEngine e = SIM_ENGINE_AUTO;
    char *cache;
    if (engine && !sim_parse_engine(engine, &e)) {
        fprintf(stderr, "tern %s: error: unknown --sim `%s` (auto, interp or compiled)\n", cmd, engine);
        return -1;
    }
    if (e == SIM_ENGINE_COMPILED && !sim_host_cc()) {
        fprintf(stderr, "tern %s: error: --sim compiled needs a host C compiler ($CC, cc, cl, gcc or clang)\n", cmd);
        return -1;
    }
    if (verbose && e == SIM_ENGINE_AUTO && !sim_host_cc())
        fprintf(stderr, "tern %s: note: no host C compiler found ($CC, cc, cl, gcc, clang); simulating with the "
                        "interpreter\n", cmd);
    cache = testrun_simcache_dir(dr);
    sim_set_engine_defaults(e, cache);
    free(cache);
    return 0;
}

uint64_t testrun_test_seed(uint64_t base, const char *name)
{
    uint64_t s = base ^ hash_bytes(name ? name : "", name ? strlen(name) : 0);
    return tr_splitmix(&s);
}

static void tr_loc(Span sp, StrBuf *out)
{
    const SourceFile *f = src_get(sp.file);
    sb_printf(out, "%s:%u:%u", f ? diag_path_display(f->path) : "<unknown>", (unsigned)sp.line, (unsigned)sp.col);
}

/* source text of a span with whitespace runs collapsed */
static void tr_span_text(Span sp, StrBuf *out)
{
    const SourceFile *f = src_get(sp.file);
    uint32_t i;
    int space = 0;
    if (!f || sp.off + sp.len > f->len) {
        sb_puts(out, "?");
        return;
    }
    for (i = 0; i < sp.len; i++) {
        char ch = f->text[sp.off + i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            space = 1;
            continue;
        }
        if (space)
            sb_putc(out, ' ');
        space = 0;
        sb_putc(out, ch);
    }
}

static void tr_print_value(const Value *v, StrBuf *out)
{
    if (v->kind == VAL_OPAQUE) {
        v->u.opq->ops->to_string(v->u.opq->obj, out);
        return;
    }
    if (v->kind == VAL_STRING) {
        sb_putc(out, '"');
        sb_append(out, v->u.s, v->aux);
        sb_putc(out, '"');
        return;
    }
    val_print(v, out);
    if (v->kind == VAL_INT && v->ty && ty_is_sized_int(v->ty)) {
        BigInt b = {0};
        val_get_bn(v, &b);
        if (bn_cmp_i64(&b, 10) >= 0) {
            sb_puts(out, " (0x");
            bn_to_sb(out, &b, 16);
            sb_putc(out, ')');
        }
        bn_free(&b);
    }
}

static void tr_flush_diags(void)
{
    diag_flush();
}

/* Start a failure entry ("    loc: ") and count it; returns 0 when the entry
 * should not be printed (too many). */
static int tr_fail_begin(TrCtx *c, Span loc)
{
    tr_flush_diags();
    c->failed = 1;
    if (++c->nfails > TR_MAX_FAILS)
        return 0;
    sb_puts(c->out, "    ");
    tr_loc(loc, c->out);
    sb_puts(c->out, ": ");
    return 1;
}

static void tr_stop(TrCtx *c)
{
    c->stopped = 1;
    c->el->limit_hit = 1;     /* unwinds the interpreter */
}

static uint64_t tr_cycle(TrCtx *c, int *has)
{
    int d;
    *has = 0;
    if (!c->last || !c->last->m)
        return 0;
    d = sim_default_domain(c->last->m);
    if (d < 0)
        return 0;
    *has = 1;
    return sim_cycle(c->last->m, (uint32_t)d);
}

static void tr_at_cycle(TrCtx *c, StrBuf *out)
{
    int has;
    uint64_t cy = tr_cycle(c, &has);
    if (has)
        sb_printf(out, " at cycle %llu", (unsigned long long)cy);
}

/* ------------------------------------------------------------- types --- */

static Ty *tr_ty_of_ir(TrSim *s, const IrType *t)
{
    Sema *S = s->ctx->el->S;
    uint64_t v;
    if (!t)
        return ty_bits(S, 1);
    if (map_get(&s->irty, MAP_PTR(t), &v))
        return (Ty *)(uintptr_t)v;
    switch (t->kind) {
    case IRT_BOOL:
    case IRT_CLOCK:
    case IRT_RESET:
        return ty_bool(S);
    case IRT_UINT:
        return ty_uint(S, t->width);
    case IRT_SINT:
        return ty_sint(S, t->width);
    case IRT_ARRAY:
        return ty_array(S, tr_ty_of_ir(s, t->elem), t->len);
    default:
        return ty_bits(S, t->width);
    }
}

static Ty *tr_sig_ty(TrSim *s, int h)
{
    return tr_ty_of_ir(s, sim_type(s->m, h));
}

/* bundle type of a top-level port path ("push", "out[1]", "a.b"), or NULL */
static Ty *tr_path_ty(TrSim *s, const char *path)
{
    const char *p = path;
    size_t n = 0;
    uint32_t i;
    Ty *t = NULL;
    const char *name;
    if (!s->mi)
        return NULL;
    while (p[n] && p[n] != '.' && p[n] != '[')
        n++;
    name = intern(p, n);
    for (i = 0; i < s->mi->nports; i++)
        if (s->mi->ports[i].name == name)
            t = s->mi->ports[i].ty;
    p += n;
    while (t && *p) {
        if (*p == '.') {
            int k;
            p++;
            n = 0;
            while (p[n] && p[n] != '.' && p[n] != '[')
                n++;
            k = ty_field_index(t, intern(p, n));
            t = k >= 0 ? t->fields[k].ty : NULL;
            p += n;
        } else if (*p == '[') {
            while (*p && *p != ']')
                p++;
            if (*p)
                p++;
            t = t->kind == TY_ARRAY ? t->elem : NULL;
        } else {
            return NULL;
        }
    }
    return t;
}

/* ------------------------------------------------------------ handles -- */

static Value tr_node_value(TrSim *s, int kind, const char *path, Ty *ty, int h)
{
    TrCtx *c = s->ctx;
    TrNode *n = ANEW(&c->el->arena, TrNode);
    n->s = s;
    n->kind = kind;
    n->path = path;
    n->ty = ty;
    n->h = h;
    return val_opaque(&c->el->arena, &tr_ops, n, kind == TRN_PATH ? ty : NULL);
}

static const char *tr_join(TrCtx *c, const char *path, const char *name)
{
    if (!*path)
        return name;
    return arena_printf(&c->el->arena, "%s.%s", path, name);
}

static void tr_settle(TrSim *s)
{
    if (s->m->dirty)
        sim_settle(s->m);
}

static Value tr_read_sig(TrSim *s, int h)
{
    TrCtx *c = s->ctx;
    IrBits b;
    tr_settle(s);
    b = sim_peek_bits(s->m, h, &c->el->arena);
    return elab_value_from_bits(c->el, &b, tr_sig_ty(s, h));
}

static int tr_is_harness_driven(TrSim *s, int h)
{
    const IrSignal *ir = sim_signal_ir(s->m, h);
    return ir && ir->kind == IRS_IN && !*sim_signal_instance(s->m, h) &&
           (ir->type->kind == IRT_CLOCK || ir->type->kind == IRT_RESET);
}

static int tr_write_sig(TrSim *s, int h, Value v, const char *path, Span at)
{
    TrCtx *c = s->ctx;
    Elab *el = c->el;
    Ty *t;
    IrBits b;
    if (!sim_is_writable(s->m, h)) {
        const IrSignal *ir = sim_signal_ir(s->m, h);
        if (tr_is_harness_driven(s, h)) {
            el_error(el, at, "`%s` is driven by the test harness", path);
            el_help(el, ir->type->kind == IRT_CLOCK ? "advance clocks with step(), step_until() or run(ns: ..)"
                                                    : "use reset() to reset the design");
        } else if (ir && ir->kind == IRS_OUT && !*sim_signal_instance(s->m, h)) {
            el_error(el, at, "cannot drive `%s`: it is an output of `%s`", path, s->display);
        } else {
            el_error(el, at, "cannot drive internal signal `%s` from a test", path);
            el_help(el, "a signal a test must drive is exposed with #[test_port]");
        }
        return -1;
    }
    t = tr_sig_ty(s, h);
    if (v.kind == VAL_ERROR)
        return -1;
    v = elab_convert(el, v, t, NULL, arena_printf(&el->arena, "`%s`", path), at);
    if (v.kind == VAL_ERROR)
        return -1;
    if (!val_is_const(&v)) {
        el_error(el, at, "cannot drive `%s` with a value that is not known in the test", path);
        return -1;
    }
    b = elab_const_bits(el, &v, t);
    sim_poke(s->m, h, &b);
    return 1;
}

/* closest signal path for "did you mean" */
static void tr_suggest(TrSim *s, const char *path)
{
    int i, n = sim_signal_count(s->m);
    const char *best = NULL;
    uint32_t bd = 3;
    for (i = 0; i < n; i++) {
        const char *p = sim_signal_path(s->m, i);
        uint32_t d = sema_edit_distance(p, path, bd);
        if (d < bd || (d == bd && !best && d < 3)) {
            bd = d;
            best = p;
        }
    }
    if (best)
        el_help(s->ctx->el, "did you mean `%s`?", best);
}

static int tr_get(TrSim *s, const char *path, int root, const char *name, Span at, Value *out)
{
    TrCtx *c = s->ctx;
    const char *p = tr_join(c, path, name);
    int h = sim_find(s->m, p);
    if (root && h >= 0 && sim_type(s->m, h)->kind == IRT_CLOCK) {
        /* a clock port names its domain: `dut.dst.step()` */
        int d = sim_find_domain(s->m, name);
        uint32_t k;
        for (k = 0; d < 0 && k < s->m->topdoms.len; k++)
            if (s->m->clocks.data[s->m->topdoms.data[k].clock].sig == (uint32_t)h)
                d = (int)k;
        if (d >= 0) {
            *out = tr_node_value(s, TRN_DOMAIN, name, NULL, d);
            return 1;
        }
    }
    if (h >= 0) {
        if (sim_is_memory(s->m, h))
            *out = tr_node_value(s, TRN_MEM, p, NULL, h);
        else
            *out = tr_read_sig(s, h);
        return 1;
    }
    if (sim_find_prefix(s->m, p, NULL, 0) > 0) {
        *out = tr_node_value(s, TRN_PATH, p, tr_path_ty(s, p), -1);
        return 1;
    }
    if (root) {
        int d = sim_find_domain(s->m, name);
        if (d >= 0) {
            *out = tr_node_value(s, TRN_DOMAIN, name, NULL, d);
            return 1;
        }
    }
    el_error(c->el, at, "`%s` has no signal `%s`", s->display, p);
    tr_suggest(s, p);
    return -1;
}

static int tr_set(TrSim *s, const char *path, const char *name, Value v, Span at)
{
    TrCtx *c = s->ctx;
    const char *p = tr_join(c, path, name);
    int h = sim_find(s->m, p);
    if (h >= 0 && !sim_is_memory(s->m, h))
        return tr_write_sig(s, h, v, p, at);
    if (h >= 0) {
        el_error(c->el, at, "cannot write memory `%s` from a test", p);
        return -1;
    }
    if (sim_find_prefix(s->m, p, NULL, 0) > 0) {
        /* a bundle: drive every field the value gives */
        uint32_t i;
        int r = 1;
        if (v.kind == VAL_ERROR)
            return -1;
        if (v.kind == VAL_OPAQUE && v.u.opq->ops == &tr_ops) {
            /* another handle's bundle: `dec.in = enc.out` */
            TrNode *src = (TrNode *)v.u.opq->obj;
            if (src->kind == TRN_PATH && src->ty && src->ty->kind == TY_BUNDLE)
                v = tr_read_bundle(src->s, src->path, src->ty);
        }
        if ((v.kind != VAL_BUNDLE && v.kind != VAL_STRUCT) || !v.ty) {
            el_error(c->el, at, "`%s` is a bundle: assign its fields, or a bundle literal", p);
            return -1;
        }
        for (i = 0; i < v.ty->nfields && i < v.u.list->n; i++) {
            Value fv = v.u.list->v[i];
            if (fv.kind == VAL_VOID)
                continue;
            if (v.ty->fields[i].flip && v.kind == VAL_BUNDLE) {
                /* flipped fields are outputs of the design: skip unless writable */
                const char *fp = tr_join(c, p, v.ty->fields[i].name);
                int fh = sim_find(s->m, fp);
                if (fh >= 0 && !sim_is_writable(s->m, fh))
                    continue;
            }
            if (tr_set(s, p, v.ty->fields[i].name, fv, at) < 0)
                r = -1;
        }
        return r;
    }
    el_error(c->el, at, "`%s` has no signal `%s`", s->display, p);
    tr_suggest(s, p);
    return -1;
}

static Value tr_read_bundle(TrSim *s, const char *path, Ty *t)
{
    TrCtx *c = s->ctx;
    Value v = val_list(&c->el->arena, VAL_BUNDLE, t, t->nfields);
    uint32_t i;
    for (i = 0; i < t->nfields; i++) {
        const char *fp = tr_join(c, path, t->fields[i].name);
        Ty *ft = t->fields[i].ty;
        if (ft->kind == TY_BUNDLE) {
            v.u.list->v[i] = tr_read_bundle(s, fp, ft);
        } else {
            int h = sim_find(s->m, fp);
            if (h >= 0 && !sim_is_memory(s->m, h))
                v.u.list->v[i] = tr_read_sig(s, h);
        }
    }
    return v;
}

/* ------------------------------------------------------------ methods -- */

static const Value *tr_arg(const Value *args, const Ident *names, uint32_t n, uint32_t pos, const char *name)
{
    uint32_t i, k = 0;
    for (i = 0; i < n; i++)
        if (names[i].name && strcmp(names[i].name, name) == 0)
            return &args[i];
    for (i = 0; i < n; i++) {
        if (names[i].name)
            continue;
        if (k++ == pos)
            return &args[i];
    }
    return NULL;
}

static int tr_arg_u64(Elab *el, const Value *v, uint64_t dflt, uint64_t *out, const char *what, Span at)
{
    if (!v) {
        *out = dflt;
        return 1;
    }
    if (!val_get_u64(v, out)) {
        if (v->kind != VAL_ERROR)
            el_error(el, at, "%s must be a non-negative integer", what);
        return 0;
    }
    return 1;
}

static int tr_check_args(Elab *el, const Ident *names, uint32_t n, uint32_t maxpos, const char *const *allowed,
                         const char *method, Span at)
{
    uint32_t i, pos = 0;
    for (i = 0; i < n; i++) {
        if (names[i].name) {
            const char *const *a;
            int ok = 0;
            for (a = allowed; *a; a++)
                if (strcmp(*a, names[i].name) == 0)
                    ok = 1;
            if (!ok) {
                el_error(el, names[i].span, "`%s()` has no parameter `%s`", method, names[i].name);
                return 0;
            }
        } else {
            pos++;
        }
    }
    if (pos > maxpos) {
        el_error(el, at, "`%s()` takes at most %u argument%s", method, (unsigned)maxpos, maxpos == 1 ? "" : "s");
        return 0;
    }
    return 1;
}

static void tr_step(TrSim *s, uint32_t dom, uint64_t n)
{
    TrCtx *c = s->ctx;
    uint64_t i;
    for (i = 0; i < n && !c->stopped; i++)
        sim_step(s->m, dom, 1);
}

static int tr_domain_or_error(TrSim *s, int dom, Span at)
{
    if (dom < 0) {
        const SimModel *m = s->m;
        dom = sim_default_domain(m);
        if (dom < 0) {
            el_error(s->ctx->el, at, "`%s` has no clock domain to step", s->display);
            el_help(s->ctx->el, "use settle() to evaluate combinational logic");
        } else if (m->top->default_domain == IR_NONE) {
            /* several clocks and no default domain: say which to step */
            StrBuf names = {0};
            uint32_t k, j, nclk = 0;
            for (k = 0; k < m->topdoms.len; k++) {
                uint32_t ck = m->topdoms.data[k].clock;
                for (j = 0; j < k && m->topdoms.data[j].clock != ck; j++)
                    ;
                if (j < k)
                    continue; /* domains over the same clock step together */
                nclk++;
                sb_printf(&names, "%s`dut.%s.step()`", names.len ? ", " : "",
                          sim_signal_name(m, (int)m->clocks.data[ck].sig));
            }
            if (nclk > 1) {
                el_error(s->ctx->el, at, "`%s` has %u clocks and no default domain: step() does not know which",
                         s->display, (unsigned)nclk);
                el_help(s->ctx->el, "step one clock (%s), or advance all of them with `dut.run(ns: ..)`",
                        sb_cstr(&names));
                dom = -1;
            }
            sb_free(&names);
        }
    }
    return dom;
}

static int tr_step_until(TrSim *s, int dom, const Value *args, const Ident *names, uint32_t n, Span at, Value *out)
{
    TrCtx *c = s->ctx;
    Elab *el = c->el;
    static const char *const allowed[] = {"cond", "max", NULL};
    Expr *call = el->opq_call;
    Env *env = el->opq_env;
    Expr *ce = NULL;
    uint64_t max, k;
    uint32_t i, pos = 0;
    if (!tr_check_args(el, names, n, 2, allowed, "step_until", at))
        return -1;
    if (!tr_arg(args, names, n, 0, "cond")) {
        el_error(el, at, "`step_until()` needs a condition");
        return -1;
    }
    if (!tr_arg_u64(el, tr_arg(args, names, n, 1, "max"), TR_STEP_UNTIL_MAX, &max, "`max`", at))
        return -1;
    /* the condition expression, re-evaluated after every cycle */
    for (i = 0; call && i < call->u.call.nargs; i++) {
        Arg *a = &call->u.call.args[i];
        if (a->name.name ? strcmp(a->name.name, "cond") == 0 : pos++ == 0) {
            ce = a->value;
            break;
        }
    }
    dom = tr_domain_or_error(s, dom, at);
    if (dom < 0)
        return -1;
    for (k = 0;; k++) {
        Value v;
        if (k == 0 || !ce)
            v = *tr_arg(args, names, n, 0, "cond");
        else
            v = elab_eval(el, ce, env, el->S->t_bool);
        if (v.kind == VAL_ERROR || c->stopped)
            return -1;
        if (v.kind != VAL_BOOL) {
            el_error(el, ce ? ce->span : at, "the condition of `step_until()` must be bool");
            return -1;
        }
        if (v.u.i) {
            *out = val_int(el->S->t_int, (int64_t)k);
            return 1;
        }
        if (k >= max)
            break;
        tr_step(s, (uint32_t)dom, 1);
        if (c->stopped)
            return 1;
    }
    if (tr_fail_begin(c, at)) {
        sb_puts(c->out, "step_until failed");
        tr_at_cycle(c, c->out);
        sb_puts(c->out, ": `");
        if (ce)
            tr_span_text(ce->span, c->out);
        else
            sb_puts(c->out, "condition");
        sb_printf(c->out, "` did not hold within %llu cycles\n", (unsigned long long)max);
    }
    tr_stop(c);
    return 1;
}

static int tr_root_method(TrSim *s, const char *name, const Value *args, const Ident *names, uint32_t n, Span at,
                          Value *out)
{
    TrCtx *c = s->ctx;
    Elab *el = c->el;
    static const char *const none[] = {NULL};
    static const char *const step_names[] = {"n", NULL};
    static const char *const run_names[] = {"ns", NULL};
    static const char *const dump_names[] = {"path", NULL};
    *out = val_void(el->S);
    if (strcmp(name, "reset") == 0) {
        if (!tr_check_args(el, names, n, 0, none, name, at))
            return -1;
        sim_reset(s->m);
        return 1;
    }
    if (strcmp(name, "step") == 0) {
        uint64_t k;
        int dom;
        if (!tr_check_args(el, names, n, 1, step_names, name, at) ||
            !tr_arg_u64(el, tr_arg(args, names, n, 0, "n"), 1, &k, "the cycle count", at))
            return -1;
        dom = tr_domain_or_error(s, -1, at);
        if (dom < 0)
            return -1;
        tr_step(s, (uint32_t)dom, k);
        return 1;
    }
    if (strcmp(name, "step_until") == 0)
        return tr_step_until(s, -1, args, names, n, at, out);
    if (strcmp(name, "settle") == 0) {
        if (!tr_check_args(el, names, n, 0, none, name, at))
            return -1;
        sim_settle(s->m);
        return 1;
    }
    if (strcmp(name, "run") == 0) {
        uint64_t ns;
        const Value *a;
        if (!tr_check_args(el, names, n, 1, run_names, name, at))
            return -1;
        a = tr_arg(args, names, n, 0, "ns");
        if (!a) {
            el_error(el, at, "`run()` needs a duration: run(ns: 500)");
            return -1;
        }
        if (!tr_arg_u64(el, a, 0, &ns, "`ns`", at))
            return -1;
        sim_run_ns(s->m, ns);
        return 1;
    }
    if (strcmp(name, "dump") == 0) {
        const Value *a;
        char *p;
        if (!tr_check_args(el, names, n, 1, dump_names, name, at))
            return -1;
        a = tr_arg(args, names, n, 0, "path");
        if (!a || a->kind != VAL_STRING) {
            el_error(el, at, "`dump()` needs a file name: dump(\"waves.vcd\")");
            return -1;
        }
        p = xstrndup(a->u.s, a->aux);
        if (sim_vcd_open(s->m, p) != SIM_OK) {
            el_error(el, at, "cannot write waveform file `%s`", p);
            free(p);
            return -1;
        }
        free(p);
        return 1;
    }
    if (strcmp(name, "cycle") == 0) {
        int dom;
        if (!tr_check_args(el, names, n, 0, none, name, at))
            return -1;
        dom = sim_default_domain(s->m);
        *out = val_int(el->S->t_int, dom < 0 ? 0 : (int64_t)sim_cycle(s->m, (uint32_t)dom));
        return 1;
    }
    if (strcmp(name, "time_ns") == 0) {
        if (!tr_check_args(el, names, n, 0, none, name, at))
            return -1;
        *out = val_int(el->S->t_int, (int64_t)(sim_time_ps(s->m) / 1000u));
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------- ValOpaque -- */

static int tr_op_get_field(Elab *el, void *obj, const char *name, Span at, Value *out)
{
    TrNode *n = (TrNode *)obj;
    TERN_UNUSED(el);
    switch (n->kind) {
    case TRN_ROOT:
    case TRN_PATH:
        return tr_get(n->s, n->path, n->kind == TRN_ROOT, name, at, out);
    case TRN_DOMAIN:
        if (strcmp(name, "cycle") == 0) {
            *out = val_int(el->S->t_int, (int64_t)sim_cycle(n->s->m, (uint32_t)n->h));
            return 1;
        }
        el_error(el, at, "clock domain `%s` has no field `%s`", n->path, name);
        return -1;
    case TRN_MEM:
        if (strcmp(name, "len") == 0 || strcmp(name, "depth") == 0) {
            *out = val_int(el->S->t_int, (int64_t)sim_mem_depth(n->s->m, n->h));
            return 1;
        }
        break;
    }
    return 0;
}

static int tr_op_set_field(Elab *el, void *obj, const char *name, Value v, Span at)
{
    TrNode *n = (TrNode *)obj;
    if (n->kind == TRN_ROOT || n->kind == TRN_PATH)
        return tr_set(n->s, n->path, name, v, at);
    el_error(el, at, "cannot assign to `%s`", name);
    return -1;
}

static int tr_op_index(Elab *el, void *obj, Value idx, Span at, Value *out)
{
    TrNode *n = (TrNode *)obj;
    uint64_t i;
    if (!val_get_u64(&idx, &i)) {
        if (idx.kind != VAL_ERROR)
            el_error(el, at, "index must be a non-negative integer known in the test");
        return -1;
    }
    if (n->kind == TRN_MEM) {
        IrBits b;
        Ty *t = tr_sig_ty(n->s, n->h);
        if (i >= sim_mem_depth(n->s->m, n->h)) {
            el_error(el, at, "index %llu out of range for memory `%s` of depth %u", (unsigned long long)i, n->path,
                     (unsigned)sim_mem_depth(n->s->m, n->h));
            return -1;
        }
        tr_settle(n->s);
        b.width = t->width;
        b.w = ANEWN(&el->arena, uint64_t, IR_LIMBS(t->width) ? IR_LIMBS(t->width) : 1);
        sim_mem_peek(n->s->m, n->h, i, &b);
        *out = elab_value_from_bits(el, &b, t);
        return 1;
    }
    if (n->kind == TRN_PATH) {
        const char *p = arena_printf(&el->arena, "%s[%llu]", n->path, (unsigned long long)i);
        int h = sim_find(n->s->m, p);
        if (h >= 0) {
            *out = tr_read_sig(n->s, h);
            return 1;
        }
        if (sim_find_prefix(n->s->m, p, NULL, 0) > 0) {
            *out = tr_node_value(n->s, TRN_PATH, p, tr_path_ty(n->s, p), -1);
            return 1;
        }
        el_error(el, at, "`%s` has no element %llu", n->path, (unsigned long long)i);
        return -1;
    }
    return 0;
}

static int tr_op_set_index(Elab *el, void *obj, Value idx, Value v, Span at)
{
    TrNode *n = (TrNode *)obj;
    uint64_t i;
    if (n->kind == TRN_MEM) {
        el_error(el, at, "cannot write memory `%s` from a test", n->path);
        return -1;
    }
    if (n->kind == TRN_PATH && val_get_u64(&idx, &i)) {
        const char *p = arena_printf(&el->arena, "%llu", (unsigned long long)i);
        const char *full = arena_printf(&el->arena, "%s[%s]", n->path, p);
        int h = sim_find(n->s->m, full);
        if (h >= 0)
            return tr_write_sig(n->s, h, v, full, at);
        if (sim_find_prefix(n->s->m, full, NULL, 0) > 0) {
            /* bundle element: reuse the bundle assignment with an empty field name */
            Ty *t = tr_path_ty(n->s, full);
            uint32_t k;
            int r = 1;
            if ((v.kind != VAL_BUNDLE && v.kind != VAL_STRUCT) || !v.ty || !t) {
                el_error(el, at, "`%s` is a bundle: assign its fields, or a bundle literal", full);
                return -1;
            }
            for (k = 0; k < v.ty->nfields && k < v.u.list->n; k++)
                if (v.u.list->v[k].kind != VAL_VOID) {
                    int fh = sim_find(n->s->m, tr_join(n->s->ctx, full, v.ty->fields[k].name));
                    if (v.ty->fields[k].flip && fh >= 0 && !sim_is_writable(n->s->m, fh))
                        continue;
                    if (tr_set(n->s, full, v.ty->fields[k].name, v.u.list->v[k], at) < 0)
                        r = -1;
                }
            return r;
        }
    }
    el_error(el, at, "cannot assign to this element");
    return -1;
}

static int tr_op_call(Elab *el, void *obj, const char *name, const Value *args, const Ident *names, uint32_t nargs,
                      Span at, Value *out)
{
    TrNode *n = (TrNode *)obj;
    TrSim *s = n->s;
    static const char *const step_names[] = {"n", NULL};
    static const char *const none[] = {NULL};
    switch (n->kind) {
    case TRN_ROOT:
        return tr_root_method(s, name, args, names, nargs, at, out);
    case TRN_DOMAIN:
        if (strcmp(name, "step") == 0) {
            uint64_t k;
            if (!tr_check_args(el, names, nargs, 1, step_names, name, at) ||
                !tr_arg_u64(el, tr_arg(args, names, nargs, 0, "n"), 1, &k, "the cycle count", at))
                return -1;
            tr_step(s, (uint32_t)n->h, k);
            *out = val_void(el->S);
            return 1;
        }
        if (strcmp(name, "step_until") == 0)
            return tr_step_until(s, n->h, args, names, nargs, at, out);
        if (strcmp(name, "cycle") == 0) {
            if (!tr_check_args(el, names, nargs, 0, none, name, at))
                return -1;
            *out = val_int(el->S->t_int, (int64_t)sim_cycle(s->m, (uint32_t)n->h));
            return 1;
        }
        break;
    case TRN_PATH:
        if (n->ty && n->ty->kind == TY_BUNDLE && n->ty->def && n->ty->def->kind == ITEM_BUNDLE) {
            Item *bi = n->ty->def;
            uint32_t i;
            for (i = 0; i < bi->u.bundle.nfns; i++) {
                Item *f = bi->u.bundle.fns[i];
                if (f->name.name && strcmp(f->name.name, name) == 0) {
                    Value fv, self = tr_read_bundle(s, n->path, n->ty);
                    memset(&fv, 0, sizeof fv);
                    fv.kind = VAL_FN;
                    fv.u.fn = ANEW(&el->arena, ValFn);
                    fv.u.fn->item = f;
                    fv.u.fn->file = n->ty->file;
                    fv.u.fn->ctx = (Env *)n->ty->genv;
                    fv.u.fn->self = ANEW(&el->arena, Value);
                    *fv.u.fn->self = self;
                    fv.u.fn->self_ty = n->ty;
                    *out = elab_call(el, fv, args, names, nargs, at);
                    return out->kind == VAL_ERROR ? -1 : 1;
                }
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

static void tr_op_to_string(void *obj, StrBuf *out)
{
    TrNode *n = (TrNode *)obj;
    switch (n->kind) {
    case TRN_ROOT: sb_printf(out, "sim %s", n->s->display); break;
    case TRN_DOMAIN: sb_printf(out, "domain %s", n->path); break;
    default: sb_printf(out, "%s", n->path); break;
    }
}

static const ValOpaqueOps tr_ops = {
    "simulation handle", tr_op_get_field, tr_op_set_field, tr_op_index, tr_op_set_index, tr_op_call,
    tr_op_to_string,
};

/* ----------------------------------------------------------- failures -- */

static void tr_on_fail(void *user, const SimFailure *f)
{
    TrSim *s = (TrSim *)user;
    TrCtx *c = s->ctx;
    if (tr_fail_begin(c, f->loc)) {
        sb_printf(c->out, "%s at cycle %llu", f->what, (unsigned long long)f->cycle);
        if (f->property && f->start_cycle != f->cycle)
            sb_printf(c->out, " (attempt started at cycle %llu)", (unsigned long long)f->start_cycle);
        if (f->inst_path && *f->inst_path)
            sb_printf(c->out, ", in %s", f->inst_path);
        sb_putc(c->out, '\n');
    }
    if (f->kind == SIM_FAIL_ASSERT || f->kind == SIM_FAIL_PROPERTY)
        tr_stop(c);
}

/* ------------------------------------------------------------ `sim M` -- */

static void tr_sim_free(TrSim *s)
{
    TrCtx *c = s->ctx;
    if (s->m) {
        if (c->cov && c->opt->cover && !sim_is_remote(s->m)) {
            const SimCovPoint *pts;
            size_t n = sim_coverage(s->m, &pts), i;
            for (i = 0; i < n; i++) {
                TestCovPoint p;
                const SourceFile *sf = src_get(pts[i].loc.file);
                if (!sf)
                    continue;
                p.kind = pts[i].kind;
                p.file = intern_cstr(sf->path);
                p.line = pts[i].loc.line;
                p.col = pts[i].loc.col;
                p.arm = pts[i].arm;
                p.name = pts[i].name ? intern_cstr(pts[i].name) : NULL;
                p.count = pts[i].count;
                testcov_add(c->cov, &p);
            }
        }
        sim_vcd_close(s->m);
        sim_free(s->m);
    }
    if (s->del)
        elab_free(s->del);
    map_free(&s->irty);
    arena_release(&s->ira);
    free(s->vcd_path);
    free(s);
}

static TrSim *tr_sim_new(TrCtx *c, Item *item, FileUnit *file, const ElabArgs *args, Span at)
{
    TrSim *s = (TrSim *)xcalloc(1, sizeof(TrSim));
    int errs = diag_error_count();
    ValModule *vm;
    size_t it = 0;
    uint64_t k, v;
    uint64_t seed;
    s->ctx = c;
    s->d = ir_design_new(&s->ira);
    s->del = driver_elab_new(c->dr, s->d);
    vm = elab_bind_module(s->del, item, file, args, at);
    if (vm) {
        diag_push_path(arena_printf(&c->el->arena, "sim %s", vm->display));
        s->display = arena_strdup(&c->el->arena, vm->display);
        s->mi = elab_module(s->del, vm, at);
        diag_pop_path();
    }
    tr_flush_diags();
    if (!s->mi || !s->mi->ir || s->mi->failed || diag_error_count() != errs) {
        if (diag_error_count() == errs)
            el_error(c->el, at, "cannot simulate `%s`: elaboration failed", s->display ? s->display : item->name.name);
        tr_sim_free(s);
        return NULL;
    }
    ir_design_set_top(s->d, s->mi->ir);
    while (map_next(&s->del->tycache.map, &it, &k, &v)) {
        const Ty *t = (const Ty *)(uintptr_t)k;
        const IrType *irt = (const IrType *)(uintptr_t)v;
        if (irt && (irt->kind == IRT_STRUCT || irt->kind == IRT_ENUM || irt->kind == IRT_ARRAY) &&
            !map_has(&s->irty, v))
            map_put(&s->irty, v, MAP_PTR(t));
    }
    seed = c->opt->seed ^ hash_u64(++c->nsims);
    {
        /* built-in simulator, or a Verilator model (spec 13.4) */
        const IrModule *ext = NULL;
        uint32_t i;
        for (i = 0; i < s->d->nmodules && !ext; i++)
            if (s->d->modules[i]->is_extern)
                ext = s->d->modules[i];
        if (c->opt->backend == TR_BACKEND_VERILATOR || (c->opt->backend == TR_BACKEND_AUTO && ext)) {
            if (!c->opt->remote) {
                if (ext)
                    el_error(c->el, at, "`%s` contains extern module `%s`: needs --backend verilator (verilator not found)",
                             s->display, ext->tern_name ? ext->tern_name : ext->name);
                else
                    el_error(c->el, at, "cannot simulate `%s` on verilator: verilator not found", s->display);
                el_help(c->el, "install Verilator (https://verilator.org) and put it on PATH");
                tr_sim_free(s);
                return NULL;
            }
            s->m = c->opt->remote(c->opt->remote_user, c->dr, s->d, seed);
            if (s->m && c->opt->cover && c->opt->backend != TR_BACKEND_VERILATOR && !c->cover_noted) {
                c->cover_noted = 1;
                sb_printf(c->out, "    note: `%s` runs on verilator: coverage is collected by the built-in simulator only\n",
                          s->display);
            }
        } else {
            s->m = sim_new(s->d, s->mi->ir, seed);
        }
    }
    tr_flush_diags();
    if (!s->m) {
        tr_sim_free(s);
        return NULL;
    }
    sim_set_fail_handler(s->m, tr_on_fail, s);
    if (c->opt->waves_path) {
        if (c->nsims == 1)
            s->vcd_path = xstrdup(c->opt->waves_path);
        else {
            StrBuf sb = {0};
            const char *dot = strrchr(c->opt->waves_path, '.');
            size_t base = dot ? (size_t)(dot - c->opt->waves_path) : strlen(c->opt->waves_path);
            sb_append(&sb, c->opt->waves_path, base);
            sb_printf(&sb, "_%u.vcd", (unsigned)c->nsims);
            s->vcd_path = sb_take(&sb);
        }
        if (sim_vcd_open(s->m, s->vcd_path) != SIM_OK) {
            free(s->vcd_path);
            s->vcd_path = NULL;
        }
    }
    vec_push(c->sims, s);
    c->last = s;
    return s;
}

static int tr_hook_sim(Elab *el, Expr *e, Env *env, Value *out)
{
    TrCtx *c = (TrCtx *)el->hooks.user;
    Expr *call = e->u.inner;
    ValModule *vm;
    TrSim *s;
    TrNode *root;
    uint32_t i;
    *out = val_error();
    if (!call || call->kind != EXPR_CALL) {
        el_error(el, e->span, "`sim` needs a module instantiation: sim M<..>(inputs)");
        return 1;
    }
    vm = elab_eval_module(el, call, env);
    if (!vm)
        return 1;
    if (vm->item->kind != ITEM_MODULE && c->opt->backend == TR_BACKEND_BUILTIN) {
        el_error(el, call->span, "cannot simulate extern module `%s` on the built-in simulator", vm->display);
        el_help(el, "extern modules run on verilator: tern test --backend verilator");
        return 1;
    }
    s = tr_sim_new(c, vm->item, vm->file, &vm->args, call->span);
    if (!s)
        return 1;
    root = ANEW(&el->arena, TrNode);
    root->s = s;
    root->kind = TRN_ROOT;
    root->path = "";
    root->h = -1;
    /* construction inputs */
    for (i = 0; i < call->u.call.nargs; i++) {
        Arg *a = &call->u.call.args[i];
        Value cur, v;
        int q = el->quiet;
        if (!a->name.name) {
            el_error(el, a->span, "inputs given to `sim` must be named: sim %s(name: value)", s->display);
            continue;
        }
        el->quiet = 1;
        if (tr_get(s, "", 0, a->name.name, a->span, &cur) != 1)
            cur = val_error();
        el->quiet = q;
        v = elab_eval(el, a->value, env, cur.kind != VAL_ERROR && cur.ty ? cur.ty : NULL);
        tr_set(s, "", a->name.name, v, a->span);
    }
    *out = val_opaque(&el->arena, &tr_ops, root, NULL);
    return 1;
}

/* ------------------------------------------------------ rand / log ---- */

static Value tr_rand_value(TrCtx *c, Ty *t, Span at)
{
    Elab *el = c->el;
    uint32_t i;
    switch (t->kind) {
    case TY_BOOL:
        return val_bool(el->S, (int)(tr_splitmix(&c->rng) & 1u));
    case TY_BIT:
    case TY_UINT:
    case TY_SINT: {
        /* half uniform, half biased towards corner cases: values of a random
         * bit length (small numbers) and edge values, so that `where`
         * constraints such as `x <= 0x1000` are met quickly */
        IrBits b;
        uint32_t n = IR_LIMBS(t->width), w = t->width;
        uint64_t mode = tr_splitmix(&c->rng) % 8;
        b.width = w;
        b.w = ANEWN(&el->arena, uint64_t, n ? n : 1);
        for (i = 0; i < n; i++)
            b.w[i] = tr_splitmix(&c->rng);
        if (mode >= 5) {
            uint32_t len = (uint32_t)(tr_splitmix(&c->rng) % (w + 1));
            for (i = 0; i < n; i++)
                if (i * 64 >= len)
                    b.w[i] = 0;
                else if (len - i * 64 < 64)
                    b.w[i] &= (UINT64_C(1) << (len - i * 64)) - 1u;
        } else if (mode == 4) {
            uint64_t e = tr_splitmix(&c->rng) % 5;
            for (i = 0; i < n; i++)
                b.w[i] = e == 2 || e == 3 ? ~UINT64_C(0) : 0;
            if (e == 1)
                b.w[0] = 1;
            else if (e == 3)
                b.w[0] &= ~UINT64_C(1);
            else if (e == 4) {
                uint32_t bit = (uint32_t)(tr_splitmix(&c->rng) % w);
                b.w[bit / 64] = UINT64_C(1) << (bit % 64);
            }
        }
        if (w % 64)
            b.w[n - 1] &= (UINT64_C(1) << (w % 64)) - 1u;
        return elab_value_from_bits(el, &b, t);
    }
    case TY_ENUM: {
        uint32_t k = t->nvariants ? (uint32_t)(tr_splitmix(&c->rng) % t->nvariants) : 0;
        if (t->variants[k].payload) {
            Value p = tr_rand_value(c, t->variants[k].payload, at);
            return val_enum(&el->arena, t, k, p.kind == VAL_STRUCT ? p.u.list : NULL);
        }
        return val_enum(&el->arena, t, k, NULL);
    }
    case TY_STRUCT: {
        Value v = val_list(&el->arena, VAL_STRUCT, t, t->nfields);
        for (i = 0; i < t->nfields; i++)
            v.u.list->v[i] = tr_rand_value(c, t->fields[i].ty, at);
        return v;
    }
    case TY_ARRAY: {
        Value v = val_list(&el->arena, VAL_ARRAY, t, t->len);
        for (i = 0; i < t->len; i++)
            v.u.list->v[i] = tr_rand_value(c, t->elem, at);
        return v;
    }
    case TY_TUPLE: {
        Value v = val_list(&el->arena, VAL_TUPLE, t, t->nfields);
        for (i = 0; i < t->nfields; i++)
            v.u.list->v[i] = tr_rand_value(c, t->fields[i].ty, at);
        return v;
    }
    default:
        el_error(el, at, "rand<%s>() needs a sized type (bool, uint<N>, sint<N>, bit<N>, an enum, a struct or an array)",
                 ty_str(t));
        return val_error();
    }
}

static void tr_format(Elab *el, const Value *args, uint32_t n, StrBuf *out, Span at)
{
    uint32_t i, k = 1;
    const char *s;
    uint32_t len;
    if (n == 0 || args[0].kind != VAL_STRING) {
        if (n == 0 || args[0].kind != VAL_ERROR)
            el_error(el, at, "the first argument of log() must be a format string");
        return;
    }
    s = args[0].u.s;
    len = args[0].aux;
    for (i = 0; i < len; i++) {
        if (s[i] == '{' && i + 1 < len && s[i + 1] == '}') {
            if (k < n) {
                if (args[k].kind == VAL_STRING)
                    sb_append(out, args[k].u.s, args[k].aux);
                else
                    tr_print_value(&args[k], out);
            } else {
                sb_puts(out, "{}");
            }
            k++;
            i++;
        } else {
            sb_putc(out, s[i]);
        }
    }
}

static void tr_log_line(TrCtx *c, const char *text)
{
    const char *p = text;
    tr_flush_diags();
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        sb_puts(c->out, "    log: ");
        sb_append(c->out, p, len);
        sb_putc(c->out, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
}

static int tr_hook_call(Elab *el, Expr *call, const char *name, const Value *args, const Ident *names, uint32_t nargs,
                        Env *env, Value *out)
{
    TrCtx *c = (TrCtx *)el->hooks.user;
    uint32_t i;
    TERN_UNUSED(names);
    for (i = 0; i < nargs; i++)
        if (args[i].kind == VAL_ERROR) {
            *out = val_error();
            return 1;
        }
    if (strcmp(name, "rand") == 0) {
        PathSeg *seg = &call->u.call.callee->u.path.segs[0];
        Ty *t;
        if (!seg->has_args || seg->nargs != 1 || nargs != 0) {
            el_error(el, call->span, "usage: rand<T>()");
            *out = val_error();
            return 1;
        }
        if (seg->args[0].type)
            t = elab_eval_type(el, seg->args[0].type, env);
        else {
            el_error(el, seg->args[0].span, "rand<T>() needs a type");
            *out = val_error();
            return 1;
        }
        *out = t->kind == TY_ERROR ? val_error() : tr_rand_value(c, t, call->span);
        return 1;
    }
    if (strcmp(name, "log") == 0) {
        StrBuf sb = {0};
        tr_format(el, args, nargs, &sb, call->span);
        tr_log_line(c, sb_cstr(&sb));
        sb_free(&sb);
        *out = val_void(el->S);
        return 1;
    }
    return 0;
}

static void tr_hook_log(Elab *el, Span at, const char *text)
{
    TERN_UNUSED(at);
    tr_log_line((TrCtx *)el->hooks.user, text);
}

/* ---------------------------------------------------- expect / assert -- */

typedef VEC(Expr *) TrExprVec;

static void tr_operands(Expr *e, TrExprVec *out)
{
    uint32_t i;
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_PAREN:
        tr_operands(e->u.inner, out);
        break;
    case EXPR_UNARY:
        tr_operands(e->u.unary.operand, out);
        break;
    case EXPR_BINARY:
        tr_operands(e->u.binary.lhs, out);
        tr_operands(e->u.binary.rhs, out);
        break;
    case EXPR_CAST:
        tr_operands(e->u.cast.expr, out);
        break;
    case EXPR_PATH:
        if (e->u.path.nsegs == 1 && !e->u.path.segs[0].has_args)
            vec_push(*out, e);
        break;
    case EXPR_FIELD:
    case EXPR_INDEX:
    case EXPR_SLICE:
        vec_push(*out, e);
        break;
    case EXPR_CALL: {
        Expr *cl = e->u.call.callee;
        const char *n = cl->kind == EXPR_FIELD ? cl->u.field.name.name
                      : cl->kind == EXPR_PATH ? cl->u.path.segs[cl->u.path.nsegs - 1].name.name : NULL;
        static const char *const impure[] = {"rand", "log", "step", "step_until", "reset", "settle", "run", "dump",
                                             NULL};
        const char *const *p;
        int ok = n != NULL;
        for (p = impure; ok && *p; p++)
            if (strcmp(*p, n) == 0)
                ok = 0;
        if (ok)
            vec_push(*out, e);
        if (cl->kind == EXPR_FIELD)
            tr_operands(cl->u.field.base, out);
        for (i = 0; i < e->u.call.nargs; i++)
            tr_operands(e->u.call.args[i].value, out);
        break;
    }
    case EXPR_TUPLE:
    case EXPR_ARRAY:
        for (i = 0; i < e->u.list.n; i++)
            tr_operands(e->u.list.items[i], out);
        break;
    default:
        break;
    }
}

static int tr_hook_check(Elab *el, Stmt *s, int passed, Env *env)
{
    TrCtx *c = (TrCtx *)el->hooks.user;
    int is_assert = s->u.assert_stmt.kind == ASSERT_ASSERT;
    const char *kw = is_assert ? "assert" : "expect";
    if (passed == 1 || c->stopped)
        return 1;
    if (passed < 0) {
        el_error(el, s->u.assert_stmt.cond->span, "the condition of `%s` must be known in the test", kw);
        return 1;
    }
    if (tr_fail_begin(c, s->span)) {
        TrExprVec ops = {0};
        size_t i, j;
        StrBuf seen = {0};
        int q = el->quiet;
        sb_printf(c->out, "%s failed", kw);
        tr_at_cycle(c, c->out);
        sb_puts(c->out, ": ");
        tr_span_text(s->u.assert_stmt.cond->span, c->out);
        sb_putc(c->out, '\n');
        if (s->u.assert_stmt.msg) {
            Value m = elab_eval(el, s->u.assert_stmt.msg, env, NULL);
            if (m.kind == VAL_STRING)
                sb_printf(c->out, "        message: %.*s\n", (int)m.aux, m.u.s);
        }
        tr_operands(s->u.assert_stmt.cond, &ops);
        el->quiet = 1;
        for (i = 0; i < ops.len; i++) {
            StrBuf txt = {0};
            Value v;
            int dup = 0;
            tr_span_text(ops.data[i]->span, &txt);
            for (j = 0; j < i; j++) {
                StrBuf o = {0};
                tr_span_text(ops.data[j]->span, &o);
                if (strcmp(sb_cstr(&o), sb_cstr(&txt)) == 0)
                    dup = 1;
                sb_free(&o);
            }
            if (!dup) {
                v = elab_eval(el, ops.data[i], env, NULL);
                if (v.kind != VAL_ERROR && v.kind != VAL_OPAQUE && v.kind != VAL_FN && v.kind != VAL_ITEM &&
                    v.kind != VAL_TYPE && v.kind != VAL_MODULE && v.kind != VAL_PACKAGE && v.kind != VAL_VOID) {
                    sb_printf(c->out, "        %s = ", sb_cstr(&txt));
                    tr_print_value(&v, c->out);
                    sb_putc(c->out, '\n');
                }
            }
            sb_free(&txt);
        }
        el->quiet = q;
        el->quiet_errors = 0;
        sb_free(&seen);
        vec_free(ops);
    }
    if (is_assert)
        tr_stop(c);
    return 1;
}

/* ------------------------------------------------------------ running -- */

static void tr_diag_sink(const Diagnostic *d, void *user)
{
    TrCtx *c = (TrCtx *)user;
    StrBuf sb = {0};
    const char *p, *e;
    if (d->level != DIAG_ERROR && !d->promoted)
        return;
    c->failed = 1;
    diag_render(d, diag_get_format(), 0, &sb);
    p = sb_cstr(&sb);
    while (*p) {
        e = strchr(p, '\n');
        if (!e)
            e = p + strlen(p);
        if (e > p) {
            sb_puts(c->out, "    ");
            sb_append(c->out, p, (size_t)(e - p));
        }
        sb_putc(c->out, '\n');
        p = *e ? e + 1 : e;
    }
    sb_free(&sb);
}

static void tr_ctx_init(TrCtx *c, Driver *dr, const TestRunOptions *o, TestCov *cov, StrBuf *report)
{
    memset(c, 0, sizeof *c);
    c->dr = dr;
    c->opt = o;
    c->cov = cov;
    c->out = report;
    c->rng = o->seed;
    c->el = driver_elab_new(dr, NULL);
    elab_set_step_limit(c->el, o->step_limit ? o->step_limit : dr->opt.const_eval_limit
                                                                ? dr->opt.const_eval_limit
                                                                : UINT64_C(50000000));
    c->el->hooks.user = c;
    c->el->hooks.sim = tr_hook_sim;
    c->el->hooks.call = tr_hook_call;
    c->el->hooks.check = tr_hook_check;
    c->el->hooks.log = tr_hook_log;
    diag_flush();
    diag_set_sink(tr_diag_sink, c);
}

static void tr_ctx_finish(TrCtx *c)
{
    size_t i;
    tr_flush_diags();
    for (i = 0; i < c->sims.len; i++) {
        TrSim *s = c->sims.data[i];
        if (s->vcd_path && !c->failed)
            os_remove(s->vcd_path);
        tr_sim_free(s);
    }
    vec_free(c->sims);
    tr_flush_diags();
    diag_set_sink(NULL, NULL);
    elab_free(c->el);
}

int testrun_run(Driver *dr, const DriverItem *t, const TestRunOptions *o, TestCov *cov, StrBuf *report)
{
    TrCtx c;
    Env *env;
    size_t i;
    int errs;
    tr_ctx_init(&c, dr, o, cov, report);
    errs = diag_error_count();
    env = elab_env_new(c.el, NULL, t->file);
    diag_push_path(arena_printf(&c.el->arena, "test \"%s\"", t->item->u.test.name ? t->item->u.test.name : ""));
    elab_exec_block(c.el, t->item->u.test.body, env, NULL);
    diag_pop_path();
    if (c.el->limit_hit && !c.stopped)
        c.failed = 1;
    c.el->limit_hit = 0;
    /* open unbounded obligations */
    if (!c.stopped)
        for (i = 0; i < c.sims.len; i++)
            sim_finish(c.sims.data[i]->m);
    elab_env_free(env);
    tr_flush_diags();
    if (diag_error_count() != errs)
        c.failed = 1;
    if (c.nfails > TR_MAX_FAILS)
        sb_printf(report, "    ... %u more failures\n", (unsigned)(c.nfails - TR_MAX_FAILS));
    tr_ctx_finish(&c);
    return !c.failed;
}

/* --------------------------------------------------------------- REPL -- */

struct TestSession {
    TrCtx c;
    Env *env;
    TestRunOptions opt;
};

TestSession *testrun_session_new(Driver *dr, FileUnit *file, const TestRunOptions *o, StrBuf *report)
{
    TestSession *ts = (TestSession *)xcalloc(1, sizeof(TestSession));
    ts->opt = *o;
    tr_ctx_init(&ts->c, dr, &ts->opt, NULL, report);
    ts->env = elab_env_new(ts->c.el, NULL, file);
    return ts;
}

void testrun_session_free(TestSession *ts)
{
    if (!ts)
        return;
    elab_env_free(ts->env);
    tr_ctx_finish(&ts->c);
    free(ts);
}

Elab *testrun_session_elab(TestSession *ts)
{
    return ts->c.el;
}

Env *testrun_session_env(TestSession *ts)
{
    return ts->env;
}

int testrun_session_sim(TestSession *ts, const char *name, Item *module, FileUnit *file, const char *vcd)
{
    TrSim *s = tr_sim_new(&ts->c, module, file, NULL, module->name.span);
    TrNode *root;
    if (!s)
        return 0;
    if (vcd && sim_vcd_open(s->m, vcd) != SIM_OK) {
        el_error(ts->c.el, span_none(), "cannot write waveform file `%s`", vcd);
        tr_flush_diags();
        return 0;
    }
    root = ANEW(&ts->c.el->arena, TrNode);
    root->s = s;
    root->kind = TRN_ROOT;
    root->path = "";
    root->h = -1;
    elab_env_bind(ts->c.el, ts->env, intern_cstr(name), val_opaque(&ts->c.el->arena, &tr_ops, root, NULL),
                  span_none());
    return 1;
}

int testrun_session_exec(TestSession *ts, Block *b, StrBuf *out)
{
    TrCtx *c = &ts->c;
    int errs = diag_error_count();
    Value tail;
    tail.kind = VAL_VOID;
    c->failed = 0;
    c->stopped = 0;
    c->nfails = 0;
    c->el->steps = 0;
    el_exec_stmts(c->el, b->stmts, b->nstmts, ts->env, b->tail, b->tail ? &tail : NULL);
    c->el->limit_hit = 0;
    c->el->returned = 0;
    tr_flush_diags();
    if (b->tail && tail.kind != VAL_VOID && tail.kind != VAL_ERROR && diag_error_count() == errs) {
        tr_print_value(&tail, out);
        sb_putc(out, '\n');
    }
    return !c->failed && diag_error_count() == errs;
}

void testrun_session_signals(TestSession *ts, const char *name, StrBuf *out)
{
    Binding *b = elab_env_find(ts->env, intern_cstr(name));
    TrNode *n;
    int i, cnt;
    if (!b || b->val.kind != VAL_OPAQUE || b->val.u.opq->ops != &tr_ops)
        return;
    n = (TrNode *)b->val.u.opq->obj;
    cnt = sim_signal_count(n->s->m);
    for (i = 0; i < cnt; i++) {
        const IrSignal *ir = sim_signal_ir(n->s->m, i);
        const char *kind = "";
        if (!ir)
            continue;
        switch (ir->kind) {
        case IRS_IN: kind = sim_is_writable(n->s->m, i) ? "in" : tr_is_harness_driven(n->s, i) ? "harness" : "in"; break;
        case IRS_OUT: kind = "out"; break;
        case IRS_WIRE: kind = "let"; break;
        case IRS_VAR: kind = "var"; break;
        case IRS_REG: kind = "reg"; break;
        case IRS_MEM: kind = "mem"; break;
        }
        if (*sim_signal_instance(n->s->m, i) && ir->kind == IRS_IN)
            kind = "in";
        sb_printf(out, "  %-24s %-8s %s%s\n", sim_signal_path(n->s->m, i), kind,
                  ty_str(tr_sig_ty(n->s, i)), sim_is_writable(n->s->m, i) ? "  (writable)" : "");
    }
    for (i = 0; i < sim_domain_count(n->s->m); i++)
        sb_printf(out, "  domain %s (%llu Hz)\n", *sim_domain_name(n->s->m, i) ? sim_domain_name(n->s->m, i) : "(default)",
                  (unsigned long long)sim_domain_hz(n->s->m, i));
}

void testrun_session_clear(TestSession *ts)
{
    ts->c.failed = 0;
    ts->c.stopped = 0;
    ts->c.nfails = 0;
}

/* ----------------------------------------------------------- coverage -- */

static uint64_t tr_cov_key(const TestCovPoint *p)
{
    uint64_t h = hash_u64(MAP_PTR(p->file));
    h = hash_combine(h, p->line);
    h = hash_combine(h, p->col);
    h = hash_combine(h, (uint64_t)p->kind);
    h = hash_combine(h, p->arm);
    h = hash_combine(h, MAP_PTR(p->name));
    return h;
}

void testcov_add(TestCov *c, const TestCovPoint *p)
{
    uint64_t key = tr_cov_key(p), v;
    if (map_get(&c->index, key, &v)) {
        c->pts.data[v - 1].count += p->count;
        return;
    }
    vec_push(c->pts, *p);
    map_put(&c->index, key, (uint64_t)c->pts.len);
}

void testcov_free(TestCov *c)
{
    vec_free(c->pts);
    map_free(&c->index);
}

void testcov_serialize(const TestCov *c, StrBuf *out)
{
    size_t i;
    for (i = 0; i < c->pts.len; i++) {
        const TestCovPoint *p = &c->pts.data[i];
        sb_printf(out, "@@cov\t%d\t%u\t%u\t%u\t%llu\t%s\t%s\n", (int)p->kind, (unsigned)p->line, (unsigned)p->col,
                  (unsigned)p->arm, (unsigned long long)p->count, p->name ? p->name : "", p->file);
    }
}

static const char *tr_field(const char *p, char *buf, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '\t' && *p != '\n') {
        if (n + 1 < cap)
            buf[n++] = *p;
        p++;
    }
    buf[n] = 0;
    return *p == '\t' ? p + 1 : p;
}

int testcov_parse_line(TestCov *c, const char *line)
{
    char f[7][1024];
    int i;
    TestCovPoint p;
    uint64_t u;
    const char *q;
    if (strncmp(line, "@@cov\t", 6) != 0)
        return 0;
    q = line + 6;
    for (i = 0; i < 7; i++)
        q = tr_field(q, f[i], sizeof f[i]);
    memset(&p, 0, sizeof p);
    p.kind = (SimCovKind)atoi(f[0]);
    p.line = (uint32_t)strtoul(f[1], NULL, 10);
    p.col = (uint32_t)strtoul(f[2], NULL, 10);
    p.arm = (uint32_t)strtoul(f[3], NULL, 10);
    u = 0;
    tern_parse_u64(f[4], &u);
    p.count = u;
    p.name = f[5][0] ? intern_cstr(f[5]) : NULL;
    p.file = intern_cstr(f[6]);
    testcov_add(c, &p);
    return 1;
}

static int tr_cov_cmp(const void *a, const void *b)
{
    const TestCovPoint *x = (const TestCovPoint *)a, *y = (const TestCovPoint *)b;
    int r = strcmp(x->file, y->file);
    if (r)
        return r;
    if (x->line != y->line)
        return x->line < y->line ? -1 : 1;
    if (x->col != y->col)
        return x->col < y->col ? -1 : 1;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    if (x->arm != y->arm)
        return x->arm < y->arm ? -1 : 1;
    return 0;
}

static void tr_pct(StrBuf *out, uint32_t a, uint32_t b)
{
    if (!b)
        sb_puts(out, "-");
    else
        sb_printf(out, "%u/%u (%u%%)", (unsigned)a, (unsigned)b, (unsigned)((uint64_t)a * 100u / b));
}

void testcov_report(TestCov *c, StrBuf *out)
{
    size_t i = 0, n = c->pts.len;
    TestCovPoint *pts = c->pts.data;
    int any_cover = 0;
    if (!n) {
        sb_puts(out, "coverage: no statements, branches or cover properties of these files were simulated\n");
        return;
    }
    qsort(pts, n, sizeof *pts, tr_cov_cmp);
    map_clear(&c->index);
    for (i = 0; i < n; i++)
        map_put(&c->index, tr_cov_key(&pts[i]), (uint64_t)i + 1);
    sb_puts(out, "coverage:\n");
    i = 0;
    while (i < n) {
        const char *file = pts[i].file;
        size_t j = i, k;
        uint32_t lines = 0, lines_hit = 0, br = 0, br_hit = 0;
        StrBuf miss_lines = {0}, miss_br = {0};
        uint32_t last_line = 0, nmiss_l = 0, nmiss_b = 0;
        while (j < n && pts[j].file == file)
            j++;
        /* lines: a line is covered when any statement on it ran */
        for (k = i; k < j;) {
            uint32_t line = pts[k].line;
            int has_stmt = 0, hit = 0;
            size_t e = k;
            while (e < j && pts[e].line == line) {
                if (pts[e].kind == SIM_COV_STMT) {
                    has_stmt = 1;
                    if (pts[e].count)
                        hit = 1;
                }
                e++;
            }
            if (has_stmt) {
                lines++;
                if (hit)
                    lines_hit++;
                else if (nmiss_l++ < 12)
                    sb_printf(&miss_lines, "%s%u", miss_lines.len ? ", " : "", (unsigned)line);
            }
            k = e;
        }
        TERN_UNUSED(last_line);
        for (k = i; k < j; k++) {
            const TestCovPoint *p = &pts[k];
            if (p->kind == SIM_COV_THEN || p->kind == SIM_COV_ELSE || p->kind == SIM_COV_ARM) {
                br++;
                if (p->count)
                    br_hit++;
                else if (nmiss_b++ < 12) {
                    sb_printf(&miss_br, "%s%u:%u ", miss_br.len ? ", " : "", (unsigned)p->line, (unsigned)p->col);
                    if (p->kind == SIM_COV_THEN)
                        sb_puts(&miss_br, "then");
                    else if (p->kind == SIM_COV_ELSE)
                        sb_puts(&miss_br, "else");
                    else
                        sb_printf(&miss_br, "arm %u", (unsigned)p->arm + 1);
                }
            }
            if (p->kind == SIM_COV_PROPERTY || p->kind == SIM_COV_COVER)
                any_cover = 1;
        }
        if (lines || br) {
            sb_printf(out, "  %s: lines ", diag_path_display(file));
            tr_pct(out, lines_hit, lines);
            sb_puts(out, ", branches ");
            tr_pct(out, br_hit, br);
            sb_putc(out, '\n');
            if (nmiss_l)
                sb_printf(out, "    lines not run: %s%s\n", sb_cstr(&miss_lines), nmiss_l > 12 ? ", ..." : "");
            if (nmiss_b)
                sb_printf(out, "    branches not taken: %s%s\n", sb_cstr(&miss_br), nmiss_b > 12 ? ", ..." : "");
        }
        sb_free(&miss_lines);
        sb_free(&miss_br);
        i = j;
    }
    if (any_cover) {
        sb_puts(out, "  cover:\n");
        for (i = 0; i < n; i++) {
            const TestCovPoint *p = &pts[i];
            if (p->kind != SIM_COV_PROPERTY && p->kind != SIM_COV_COVER)
                continue;
            if (p->kind == SIM_COV_PROPERTY)
                sb_printf(out, "    property %s", p->name ? p->name : "_");
            else
                sb_puts(out, "    cover");
            sb_printf(out, " (%s:%u:%u): %llu hit%s%s\n", diag_path_display(p->file), (unsigned)p->line,
                      (unsigned)p->col, (unsigned long long)p->count, p->count == 1 ? "" : "s",
                      p->count ? "" : "  <- never hit");
        }
    }
}

#undef TR_MAX_FAILS
#undef TR_STEP_UNTIL_MAX
