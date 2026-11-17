/* test_simc.c — the compiled simulator (src/sim/simc.c).
 *
 *     cc -o build/test_simc tests/unit/test_simc.c && build/test_simc [tern]
 *
 * Run from the repository root (make test does); the tern binary is ./tern
 * or the first argument.  Three parts:
 *   1. every check of test_sim.c again, with sim_new compiling each model
 *      (SIM_ENGINE_COMPILED): all instruction kinds against their expected
 *      values, wide arithmetic against a bignum reference;
 *   2. a differential test: every module of tests/fixtures/ and std/ that
 *      `tern ir --json --top <M>` elaborates (std: generics with defaults) is
 *      simulated by the interpreter and by the compiled model side by side
 *      on the same random stimulus
 *      (pokes, #[test_port] releases, steps of random domains, run_ns,
 *      resets) and every signal, memory word, failure count and coverage
 *      count is compared after every step;
 *   3. --sim auto: a model switches from the interpreter to its compiled
 *      code (compiled in the background) without a visible difference;
 *   4. a benchmark: interpreter vs compiled on a counter, the smoother of
 *      examples/ and a CRC-32 pipeline (tests/unit/simc_bench.tern).
 * Without a host C compiler the test is skipped.  Models are cached in
 * build/simcache. */
#define main ts_sim_main
#include "test_sim.c"
#undef main

static const char *tc_tern = "./tern";
static int tc_strict = 1;

/* ------------------------------------------------------- diagnostics */

static char tc_diag[2048];
static int tc_diag_errors;

static void tc_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(user);
    if (d->level == DIAG_ERROR) {
        tc_diag_errors++;
        snprintf(tc_diag, sizeof tc_diag, "%s", d->msg);
    }
}

/* ------------------------------------------------------ fixture list */

typedef struct TcItem {
    char *file;
    char *top;
    char *json;
    size_t json_len;
} TcItem;

typedef VEC(TcItem) TcItemVec;

static int tc_is_ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* `module Name` / `pub module Name` at the start of a line */
static void tc_scan_modules(const char *path, TcItemVec *out)
{
    char *data;
    size_t len;
    const char *p;
    if (os_read_file(path, &data, &len) != 0)
        return;
    p = data;
    while (*p) {
        const char *q = p, *e;
        while (*q == ' ' || *q == '\t')
            q++;
        if (strncmp(q, "pub ", 4) == 0)
            q += 4;
        if (strncmp(q, "module ", 7) == 0) {
            q += 7;
            while (*q == ' ')
                q++;
            e = q;
            while (tc_is_ident(*e))
                e++;
            if (e > q) {
                TcItem it;
                memset(&it, 0, sizeof it);
                it.file = xstrdup(path);
                it.top = xstrndup(q, (size_t)(e - q));
                vec_push(*out, it);
            }
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    free(data);
}

static void tc_walk(const char *dir, TcItemVec *out)
{
    OsDirList l;
    size_t i;
    if (os_list_dir(dir, &l) != 0)
        return;
    for (i = 0; i < l.len; i++) {
        char *p = os_path_join(dir, l.items[i].name);
        if (l.items[i].is_dir)
            tc_walk(p, out);
        else if (strcmp(os_path_ext(p), ".tern") == 0)
            tc_scan_modules(p, out);
        free(p);
    }
    os_dir_list_free(&l);
}

static OsProc *tc_spawn_ir(const TcItem *it)
{
    char *argv[8];
    OsSpawnOpts o;
    argv[0] = (char *)tc_tern;
    argv[1] = (char *)"ir";
    argv[2] = (char *)"--json";
    argv[3] = (char *)"--top";
    argv[4] = it->top;
    argv[5] = it->file;
    argv[6] = NULL;
    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    o.stdin_null = 1;
    return os_spawn(argv, &o, NULL);
}

/* elaborates every item in parallel; items that do not elaborate keep json NULL */
static void tc_elaborate_all(TcItemVec *items)
{
    size_t nw = (size_t)os_cpu_count(), next = 0, running = 0, i;
    OsProc **procs;
    size_t *which;
    if (nw > 16)
        nw = 16;
    procs = (OsProc **)xcalloc(nw, sizeof(OsProc *));
    which = (size_t *)xcalloc(nw, sizeof(size_t));
    for (;;) {
        int w;
        for (i = 0; i < nw && next < items->len; i++) {
            if (procs[i])
                continue;
            procs[i] = tc_spawn_ir(&items->data[next]);
            which[i] = next++;
            if (procs[i])
                running++;
        }
        if (!running)
            break;
        w = os_proc_wait_any(procs, nw, -1);
        if (w < 0)
            break;
        if (os_proc_exit_code(procs[w]) == 0) {
            size_t len;
            const char *out = os_proc_stdout(procs[w], &len);
            if (len) {
                items->data[which[w]].json = xstrndup(out, len);
                items->data[which[w]].json_len = len;
            }
        }
        os_proc_free(procs[w]);
        procs[w] = NULL;
        running--;
    }
    free(procs);
    free(which);
}

/* --------------------------------------------------------- lockstep */

typedef struct TcFails {
    uint32_t n;
    uint32_t last_line;
    int last_kind;
} TcFails;

static void tc_on_fail(void *user, const SimFailure *f)
{
    TcFails *t = (TcFails *)user;
    t->n++;
    t->last_line = f->loc.line;
    t->last_kind = (int)f->kind;
}

static void tc_hex(const uint64_t *w, uint32_t width, char *out, size_t cap)
{
    StrBuf sb = {0};
    bv_print_hex(&sb, w, width);
    snprintf(out, cap, "%s", sb_cstr(&sb));
    sb_free(&sb);
}

/* compares every signal and memory word; returns 0 (and reports) on a difference */
static int tc_compare(const char *name, SimModel *a, SimModel *b, uint32_t step, const TcFails *fa, const TcFails *fb)
{
    int h, n = sim_signal_count(a);
    uint64_t va[64], vb[64];
    IrBits ba, bb;
    char sa[160], sb2[160];
    ba.w = va;
    bb.w = vb;
    if (fa->n != fb->n) {
        fprintf(stderr, "test_simc: %s, step %u: %u failures interpreted, %u compiled (line %u vs %u)\n", name, step,
                fa->n, fb->n, fa->last_line, fb->last_line);
        return 0;
    }
    for (h = 0; h < n; h++) {
        uint32_t w = sim_width(a, h), limbs = (w + 63) / 64;
        if (limbs > 64)
            continue;
        if (sim_is_memory(a, h)) {
            uint32_t d = sim_mem_depth(a, h), k;
            if (d > 512)
                d = 512;
            for (k = 0; k < d; k++) {
                memset(va, 0, sizeof va);
                memset(vb, 0, sizeof vb);
                sim_mem_peek(a, h, k, &ba);
                sim_mem_peek(b, h, k, &bb);
                if (memcmp(va, vb, sizeof(uint64_t) * limbs) != 0) {
                    tc_hex(va, w, sa, sizeof sa);
                    tc_hex(vb, w, sb2, sizeof sb2);
                    fprintf(stderr, "test_simc: %s, step %u: %s[%u] is %s interpreted, %s compiled\n", name, step,
                            sim_signal_path(a, h), k, sa, sb2);
                    return 0;
                }
            }
            continue;
        }
        memset(va, 0, sizeof va);
        memset(vb, 0, sizeof vb);
        sim_peek(a, h, &ba);
        sim_peek(b, h, &bb);
        if (memcmp(va, vb, sizeof(uint64_t) * limbs) != 0) {
            tc_hex(va, w, sa, sizeof sa);
            tc_hex(vb, w, sb2, sizeof sb2);
            fprintf(stderr, "test_simc: %s, step %u: %s is %s interpreted, %s compiled\n", name, step,
                    sim_signal_path(a, h), sa, sb2);
            return 0;
        }
    }
    return 1;
}

static int tc_compare_cov(const char *name, SimModel *a, SimModel *b)
{
    const SimCovPoint *pa, *pb;
    size_t na = sim_coverage(a, &pa), nb, i;
    SimCovPoint *copy = (SimCovPoint *)xcalloc(na + 1, sizeof(SimCovPoint));
    int ok = 1;
    if (na)
        memcpy(copy, pa, sizeof(SimCovPoint) * na);
    nb = sim_coverage(b, &pb);
    if (na != nb) {
        fprintf(stderr, "test_simc: %s: %u coverage points interpreted, %u compiled\n", name, (unsigned)na,
                (unsigned)nb);
        ok = 0;
    }
    for (i = 0; ok && i < na; i++)
        if (copy[i].count != pb[i].count || copy[i].kind != pb[i].kind) {
            fprintf(stderr, "test_simc: %s: coverage point at line %u counts %llu interpreted, %llu compiled\n", name,
                    copy[i].loc.line, (unsigned long long)copy[i].count, (unsigned long long)pb[i].count);
            ok = 0;
        }
    free(copy);
    return ok;
}

static void tc_poke_both(SimModel *a, SimModel *b, int h)
{
    uint64_t w[64];
    IrBits v;
    uint32_t width = sim_width(a, h);
    if ((width + 63) / 64 > 64)
        return;
    ts_rand_bits(w, width);
    v.width = width;
    v.w = w;
    sim_poke(a, h, &v);
    sim_poke(b, h, &v);
}

/* runs the interpreted model `a` and the compiled model `b` side by side */
static int tc_lockstep(const char *name, SimModel *a, SimModel *b, uint32_t steps)
{
    TcFails fa, fb;
    VEC(int) wr = {0};
    int h, n = sim_signal_count(a), ok = 1, nd = sim_domain_count(a);
    uint32_t s;
    memset(&fa, 0, sizeof fa);
    memset(&fb, 0, sizeof fb);
    sim_set_fail_handler(a, tc_on_fail, &fa);
    sim_set_fail_handler(b, tc_on_fail, &fb);
    ts_rng = hash_bytes(name, strlen(name)) | 1u;
    for (h = 0; h < n; h++)
        if (sim_is_writable(a, h) && !sim_is_memory(a, h))
            vec_push(wr, h);
    sim_settle(a);
    sim_settle(b);
    ok = tc_compare(name, a, b, 0, &fa, &fb);
    if (ok) {
        sim_reset(a);
        sim_reset(b);
        ok = tc_compare(name, a, b, 0, &fa, &fb);
    }
    for (s = 1; ok && s <= steps; s++) {
        uint32_t r = (uint32_t)(ts_rand() % 100), i;
        for (i = 0; i < wr.len; i++) {
            uint32_t q = (uint32_t)(ts_rand() % 16);
            if (q < 9)
                tc_poke_both(a, b, wr.data[i]);
            else if (q == 9) {
                sim_release(a, wr.data[i]);
                sim_release(b, wr.data[i]);
            }
        }
        if (r < 3) {
            sim_reset(a);
            sim_reset(b);
        } else if (nd > 1 && r < 25) {
            uint64_t ns = 1 + ts_rand() % 37;
            sim_run_ns(a, ns);
            sim_run_ns(b, ns);
        } else if (nd > 0 && r < 90) {
            uint32_t d = nd > 1 ? (uint32_t)(ts_rand() % (uint64_t)nd) : (uint32_t)sim_default_domain(a);
            uint32_t k = 1 + (uint32_t)(ts_rand() % 3);
            sim_step(a, d, k);
            sim_step(b, d, k);
        }
        sim_settle(a);
        sim_settle(b);
        ok = tc_compare(name, a, b, s, &fa, &fb);
    }
    if (ok) {
        uint32_t pa = sim_finish(a), pb = sim_finish(b);
        if (pa != pb) {
            fprintf(stderr, "test_simc: %s: %u pending obligations interpreted, %u compiled\n", name, pa, pb);
            ok = 0;
        }
    }
    if (ok)
        ok = tc_compare_cov(name, a, b);
    vec_free(wr);
    return ok;
}

typedef struct TcPair {
    Arena arena;
    IrDesign *d;
    SimModel *a, *b;
    const TcItem *it;
} TcPair;

static void tc_pair_free(TcPair *p)
{
    sim_free(p->a);
    sim_free(p->b);
    arena_release(&p->arena);
    memset(p, 0, sizeof *p);
}

static void test_differential(void)
{
    TcItemVec items = {0};
    TcPair *batch;
    size_t i, nb = (size_t)os_cpu_count() * 2, designs = 0, compiled = 0;
    uint64_t t0 = os_time_ns();
    tc_walk("tests/fixtures", &items);
    tc_walk("std", &items); /* modules whose generics all have defaults */
    tc_elaborate_all(&items);
    if (nb > 32)
        nb = 32;
    batch = (TcPair *)xcalloc(nb, sizeof(TcPair));
    for (i = 0; i < items.len;) {
        size_t k = 0, j;
        /* build a batch of models and start their compiles in parallel */
        while (k < nb && i < items.len) {
            TcItem *it = &items.data[i++];
            TcPair *p = &batch[k];
            if (!it->json)
                continue;
            memset(p, 0, sizeof *p);
            p->it = it;
            tc_diag_errors = 0;
            p->d = ir_from_json(&p->arena, it->json, it->json_len);
            if (!p->d || !p->d->top) {
                arena_release(&p->arena);
                continue;
            }
            sim_set_engine_defaults(SIM_ENGINE_INTERP, "build/simcache");
            p->a = sim_new(p->d, p->d->top, 7);
            p->b = p->a ? sim_new(p->d, p->d->top, 7) : NULL;
            if (!p->a || !p->b) { /* e.g. a combinational loop: not simulated */
                tc_pair_free(p);
                continue;
            }
            if (sim_compile_start(p->b) != 0) {
                fprintf(stderr, "test_simc: %s (%s): %s\n", it->top, it->file, tc_diag);
                TS_CHECK(!"compile start");
                tc_pair_free(p);
                continue;
            }
            k++;
        }
        for (j = 0; j < k; j++) {
            TcPair *p = &batch[j];
            char name[300];
            snprintf(name, sizeof name, "%s (%s)", p->it->top, p->it->file);
            designs++;
            if (sim_compile(p->b) != 0) {
                fprintf(stderr, "test_simc: %s: %s\n", name, tc_diag);
                TS_CHECK(!"compile");
            } else {
                compiled++;
                TS_CHECK(sim_is_compiled(p->b));
                TS_CHECK(tc_lockstep(name, p->a, p->b, 48));
            }
            tc_pair_free(p);
        }
    }
    printf("simc differential: %u fixture and std designs (of %u modules), %u compiled and identical to the interpreter "
           "(%.1f s)\n",
           (unsigned)designs, (unsigned)items.len, (unsigned)compiled, (double)(os_time_ns() - t0) / 1e9);
    TS_CHECK(designs > 50);
    for (i = 0; i < items.len; i++) {
        free(items.data[i].file);
        free(items.data[i].top);
        free(items.data[i].json);
    }
    vec_free(items);
    free(batch);
}

/* -------------------------------------------------------- benchmark */

static IrDesign *tc_load(Arena *a, const char *file, const char *top)
{
    TcItem it;
    OsProc *p;
    IrDesign *d = NULL;
    memset(&it, 0, sizeof it);
    it.file = (char *)file;
    it.top = (char *)top;
    p = tc_spawn_ir(&it);
    if (!p)
        return NULL;
    os_proc_wait(p);
    if (os_proc_exit_code(p) == 0) {
        size_t len;
        const char *out = os_proc_stdout(p, &len);
        d = ir_from_json(a, out, len);
    } else {
        size_t len;
        fprintf(stderr, "test_simc: %s %s: %s\n", file, top, os_proc_stderr(p, &len));
    }
    os_proc_free(p);
    return d;
}

static void tc_quiet(void *user, const SimFailure *f)
{
    TERN_UNUSED(f);
    ++*(uint32_t *)user;
}

static double tc_time_run(SimModel *m, uint32_t cycles, const char **inputs, int ninputs)
{
    uint64_t t0;
    int i;
    static uint32_t nfail;
    sim_set_fail_handler(m, tc_quiet, &nfail);
    for (i = 0; i < ninputs; i++)
        sim_poke_u64(m, sim_find(m, inputs[i]), 1);
    sim_reset(m);
    /* `start` is a one-cycle pulse */
    sim_step(m, (uint32_t)sim_default_domain(m), 1);
    if (sim_find(m, "start") >= 0)
        sim_poke_u64(m, sim_find(m, "start"), 0);
    t0 = os_time_ns();
    sim_step(m, (uint32_t)sim_default_domain(m), cycles);
    return (double)(os_time_ns() - t0) / 1e9;
}

static void tc_bench(const char *label, const IrDesign *d, uint32_t cycles, const char **inputs, int ninputs,
                     const char *out)
{
    SimModel *a, *b;
    double ta, tb;
    sim_set_engine_defaults(SIM_ENGINE_INTERP, "build/simcache");
    a = sim_new(d, d->top, 3);
    b = sim_new(d, d->top, 3);
    TS_CHECK(a && b);
    if (!a || !b) {
        sim_free(a);
        sim_free(b);
        return;
    }
    TS_CHECK(sim_compile(b) == 0);
    ta = tc_time_run(a, cycles, inputs, ninputs);
    tb = tc_time_run(b, cycles, inputs, ninputs);
    TS_EQ(sim_peek_u64(b, sim_find(b, out)), sim_peek_u64(a, sim_find(a, out)));
    TS_EQ(sim_fail_count(b), sim_fail_count(a));
    printf("simc bench: %-28s %8u cycles: interpreter %7.1f ms (%6.2f M cycles/s), compiled %7.1f ms "
           "(%6.2f M cycles/s): %.1fx\n",
           label, cycles, ta * 1e3, cycles / ta / 1e6, tb * 1e3, cycles / tb / 1e6, ta / tb);
    sim_free(a);
    sim_free(b);
}

/* --sim auto: a model starts interpreted, compiles in the background once
 * it has done enough work (an empty cache directory forces a real compile)
 * and switches over without a visible difference */
static void test_auto(void)
{
    Arena ar = {0};
    IrDesign *d = tc_load(&ar, "tests/unit/simc_bench.tern", "CrcBench");
    const char *dir = "build/simcache-auto";
    SimModel *a, *b;
    OsDirList l;
    uint32_t k;
    int hs, ha, he;
    int switched_at = -1;
    TS_CHECK(d != NULL);
    if (!d)
        return;
    os_mkdir_p(dir);
    if (os_list_dir(dir, &l) == 0) {
        size_t i;
        for (i = 0; i < l.len; i++) {
            char *p = os_path_join(dir, l.items[i].name);
            os_remove(p);
            free(p);
        }
        os_dir_list_free(&l);
    }
    sim_set_engine_defaults(SIM_ENGINE_INTERP, dir);
    a = sim_new(d, d->top, 5);
    sim_set_engine_defaults(SIM_ENGINE_AUTO, dir);
    b = sim_new(d, d->top, 5);
    TS_CHECK(a && b);
    if (!a || !b) {
        sim_free(a);
        sim_free(b);
        arena_release(&ar);
        return;
    }
    TS_CHECK(!sim_is_compiled(b));
    ha = sim_find(a, "en");
    he = sim_find(b, "en");
    sim_poke_u64(a, ha, 1);
    sim_poke_u64(b, he, 1);
    sim_reset(a);
    sim_reset(b);
    hs = sim_find(a, "sig");
    for (k = 0; k < 600; k++) {
        sim_step(a, 0, 500);
        sim_step(b, 0, 500);
        TS_EQ(sim_peek_u64(b, hs), sim_peek_u64(a, hs));
        if (sim_is_compiled(b)) {
            if (switched_at < 0)
                switched_at = (int)k;
            if ((int)k > switched_at + 4)
                break;
        }
    }
    TS_CHECK(switched_at >= 0);
    TS_CHECK(sim_is_compiled(b));
    printf("simc auto: switched to compiled code after %d cycles\n", switched_at < 0 ? -1 : (switched_at + 1) * 500);
    sim_free(a);
    sim_free(b);
    arena_release(&ar);
}

static void test_bench(void)
{
    Arena a1 = {0}, a2 = {0};
    IrDesign *d;
    const char *en[1];
    const char *smoother_in[2];
    en[0] = "en";
    ts_new_design();
    ir_design_set_top(D, ts_counter("Counter", ir_type_reset(D, 0, 0), 32));
    tc_bench("counter (32 bit)", D, 2000000, en, 1, "count");
    d = tc_load(&a1, "examples/smoother/src/smoother.tern", "Smoother");
    TS_CHECK(d != NULL);
    smoother_in[0] = "start";
    smoother_in[1] = "samples.valid";
    if (d)
        tc_bench("smoother (examples/)", d, 300000, smoother_in, 2, "crc");
    d = tc_load(&a2, "tests/unit/simc_bench.tern", "CrcBench");
    TS_CHECK(d != NULL);
    if (d)
        tc_bench("CRC-32 x8 pipeline", d, 100000, en, 1, "sig");
    arena_release(&a1);
    arena_release(&a2);
}

/* ------------------------------------------------------------- main */

#ifndef TC_NO_MAIN
int main(int argc, char **argv)
{
    int rc, bench_only = 0, i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0)
            bench_only = 1;
        else
            tc_tern = argv[i];
    }
    if (!sim_host_cc()) {
        printf("simc unit tests: no host C compiler ($CC, cc, cl, gcc, clang), skipped\n");
        return 0;
    }
    /* cl needs a developer prompt; elsewhere a compiler that cannot build a
     * model is a failure */
    tc_strict = strcmp(os_dl_ext(), ".dll") != 0;
    diag_set_sink(tc_sink, NULL);
    ts_setup_source();
    {
        SimModel *m;
        ts_new_design();
        ir_design_set_top(D, ts_counter("Probe", ir_type_reset(D, 0, 0), 8));
        sim_set_engine_defaults(SIM_ENGINE_INTERP, "build/simcache");
        m = sim_new(D, D->top, 1);
        if (!m || sim_compile(m) != 0) {
            printf("simc unit tests: the host compiler (%s) cannot build a model: %s\n", sim_host_cc(), tc_diag);
            sim_free(m);
            if (!tc_strict) {
                printf("simc unit tests: skipped\n");
                return 0;
            }
            return 1;
        }
        sim_free(m);
    }
    if (!bench_only) {
        src_free_all();
        intern_free_all();
        /* 1. the simulator's unit tests on compiled models */
        sim_set_engine_defaults(SIM_ENGINE_COMPILED, "build/simcache");
        rc = ts_sim_main();
        if (rc != 0)
            return rc;
        ts_checks = ts_fails = 0;
        ts_setup_source();
        diag_set_sink(tc_sink, NULL);
        /* 2. differential */
        test_differential();
        test_auto();
    }
    /* 3. benchmark */
    test_bench();
    arena_release(&ts_arena);
    src_free_all();
    intern_free_all();
    if (ts_fails) {
        printf("simc unit tests: %d of %d checks FAILED\n", ts_fails, ts_checks);
        return 1;
    }
    printf("simc unit tests: all %d checks passed\n", ts_checks);
    return 0;
}
#endif
