/* cmd_test.c — `tern test [paths] [filter...] [--waves] [--seed n] [--cover]
 * [-j n] [--list]` (spec §11, §13.2).
 *
 * Positional arguments that name an existing file or directory are inputs
 * (default: the project's src/ and tests/); the others are filters (a test
 * runs when its name contains any of them).
 *
 * Tests run in parallel worker processes: the parent re-runs itself as
 * `tern test --worker i,j,k ...` on chunks of the test list (indices into
 * the unfiltered list, so every worker sees the same tests) and prints the
 * results in source order. A worker prints, per test:
 *
 *     @@start <i>
 *     | <report line>          (failures, logs, diagnostics)
 *     @@result <i> <1 passed | 0 failed>
 *
 * and with --cover the merged coverage points at the end (testcov_serialize).
 * A worker that dies is reported for the test it was running; the rest of
 * its chunk is retried in a new worker. Each test's seed is derived from
 * --seed (default 1) and its name, so results do not depend on scheduling
 * and a failure replays with `tern test --seed <n> "<name>"`.
 *
 * --backend builtin|verilator (spec 13.4) picks the simulator behind `sim
 * M<..>()`; without it, designs that contain an extern module run on
 * Verilator when it is installed (src/backend/vlsim.c) and are an error
 * otherwise. With --backend verilator the tests run one at a time in this
 * process.
 *
 * --sim auto|interp|compiled picks the built-in simulator's engine
 * (src/sim/simc.c; passed on to the workers): compiled models are cached in
 * <project>/build/simcache ($TERN_SIMCACHE overrides it). */
#include "../base/common.h"
#include "../base/args.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/diag.h"
#include "../driver.h"
#include "../cmds.h"
#include "../os/os.h"
#include "testrun.h"
#include "../backend/backend.h"

#define TEST_DEFAULT_SEED 1u

typedef struct TestJob {
    DriverItem di;
    size_t index;             /* in the unfiltered list */
    int done;
    int passed;
    char *report;
    char *waves;              /* VCD path (--waves) */
} TestJob;

typedef VEC(TestJob) TestJobVec;
typedef VEC(const char *) TestStrVec;

typedef struct TestCmd {
    uint64_t seed;
    int seed_given;
    int waves;
    int cover;
    int list;
    int jobs;
    uint64_t limit;
    const char *features;
    const char *worker;       /* --worker index list */
    const char *backend;      /* --backend as given, or NULL */
    const char *engine;       /* --sim as given, or NULL */
    int verbose;
    TestRunBackend be;
    int have_verilator;
    TestStrVec paths;
    TestStrVec filters;
    TestJobVec tests;
    TestCov cov;
    size_t next_print;
    int color;
    uint32_t passed, failed;
} TestCmd;

static const char *test_c(const TestCmd *t, const char *code)
{
    return t->color ? code : "";
}

static char *test_waves_path(Driver *dr, const DriverItem *di)
{
    StrBuf sb = {0};
    const char *name = di->item->u.test.name ? di->item->u.test.name : "test";
    const char *base = os_path_basename(di->file->ast->path);
    const char *dot = strrchr(base, '.');
    char *dir;
    const char *p;
    if (dr->has_project)
        dir = os_path_join(dr->prj.manifest.root, "build/waves");
    else
        dir = xstrdup("build/waves");
    sb_printf(&sb, "%s/", dir);
    sb_append(&sb, base, dot ? (size_t)(dot - base) : strlen(base));
    sb_putc(&sb, '-');
    for (p = name; *p; p++)
        sb_putc(&sb, tern_is_alnum((unsigned char)*p) || *p == '-' || *p == '_' ? *p : '_');
    sb_puts(&sb, ".vcd");
    free(dir);
    return sb_take(&sb);
}

static void test_run_one(TestCmd *t, Driver *dr, TestJob *j, StrBuf *report)
{
    TestRunOptions o;
    memset(&o, 0, sizeof o);
    o.seed = testrun_test_seed(t->seed, j->di.item->u.test.name);
    o.cover = t->cover;
    o.step_limit = t->limit;
    o.waves_path = j->waves;
    o.backend = t->be;
    o.remote = t->be != TR_BACKEND_BUILTIN && t->have_verilator ? vls_test_model : NULL;
    j->passed = testrun_run(dr, &j->di, &o, &t->cov, report);
    j->done = 1;
}

/* ----------------------------------------------------------- printing -- */

static void test_print_ready(TestCmd *t)
{
    while (t->next_print < t->tests.len && t->tests.data[t->next_print].done) {
        TestJob *j = &t->tests.data[t->next_print++];
        const char *name = j->di.item->u.test.name ? j->di.item->u.test.name : "";
        if (j->passed) {
            t->passed++;
            printf("test %s ... %sok%s\n", name, test_c(t, "\x1b[32m"), test_c(t, "\x1b[0m"));
        } else {
            t->failed++;
            printf("test %s ... %sFAILED%s\n", name, test_c(t, "\x1b[1;31m"), test_c(t, "\x1b[0m"));
        }
        if (j->report && *j->report)
            fputs(j->report, stdout);
        if (!j->passed && j->waves && os_is_file(j->waves))
            printf("    waveform: %s\n", diag_path_display(j->waves));
    }
    fflush(stdout);
}

/* ------------------------------------------------------------ workers -- */

static int test_worker(TestCmd *t, Driver *dr, DriverItemVec *all)
{
    const char *p = t->worker;
    StrBuf out = {0};
    while (*p) {
        uint64_t idx = 0;
        TestJob j;
        StrBuf rep = {0};
        const char *q;
        while (*p >= '0' && *p <= '9')
            idx = idx * 10 + (uint64_t)(*p++ - '0');
        if (*p == ',')
            p++;
        if (idx >= all->len)
            continue;
        memset(&j, 0, sizeof j);
        j.di = all->data[idx];
        j.index = (size_t)idx;
        if (t->waves)
            j.waves = test_waves_path(dr, &j.di);
        printf("@@start %llu\n", (unsigned long long)idx);
        fflush(stdout);
        test_run_one(t, dr, &j, &rep);
        q = sb_cstr(&rep);
        sb_reset(&out);
        while (*q) {
            const char *e = strchr(q, '\n');
            if (!e)
                e = q + strlen(q);
            sb_puts(&out, "| ");
            sb_append(&out, q, (size_t)(e - q));
            sb_putc(&out, '\n');
            q = *e ? e + 1 : e;
        }
        sb_printf(&out, "@@result %llu %d\n", (unsigned long long)idx, j.passed);
        fwrite(out.data, 1, out.len, stdout);
        fflush(stdout);
        sb_free(&rep);
        free(j.waves);
    }
    if (t->cover) {
        sb_reset(&out);
        testcov_serialize(&t->cov, &out);
        fwrite(out.data ? out.data : "", 1, out.len, stdout);
    }
    sb_free(&out);
    fflush(stdout);
    return TERN_EXIT_OK;
}

typedef struct TestChunk {
    size_t *items;            /* indices into t->tests */
    size_t n;
} TestChunk;

typedef VEC(TestChunk) TestChunkVec;

static TestJob *test_by_index(TestCmd *t, size_t index)
{
    size_t i;
    for (i = 0; i < t->tests.len; i++)
        if (t->tests.data[i].index == index)
            return &t->tests.data[i];
    return NULL;
}

static OsProc *test_spawn(TestCmd *t, const TestChunk *ch, char **err)
{
    VEC(char *) argv = {0};
    StrBuf list = {0};
    char seed[32], limit[32];
    char *exe = os_exe_path();
    const char *fmt = diag_get_format() == DIAG_FMT_SHORT ? "--diag-format=short"
                    : diag_get_format() == DIAG_FMT_JSON ? "--diag-format=json" : "--diag-format=human";
    OsSpawnOpts o;
    OsProc *p;
    size_t i;
    for (i = 0; i < ch->n; i++)
        sb_printf(&list, "%s%llu", i ? "," : "", (unsigned long long)t->tests.data[ch->items[i]].index);
    snprintf(seed, sizeof seed, "%llu", (unsigned long long)t->seed);
    snprintf(limit, sizeof limit, "%llu", (unsigned long long)t->limit);
    vec_push(argv, exe ? exe : (char *)(os_get_argv0() ? os_get_argv0() : "tern"));
    vec_push(argv, (char *)fmt);
    vec_push(argv, (char *)"test");
    vec_push(argv, (char *)"--worker");
    vec_push(argv, (char *)sb_cstr(&list));
    vec_push(argv, (char *)"--seed");
    vec_push(argv, seed);
    if (t->limit) {
        vec_push(argv, (char *)"--const-eval-limit");
        vec_push(argv, limit);
    }
    if (t->cover)
        vec_push(argv, (char *)"--cover");
    if (t->waves)
        vec_push(argv, (char *)"--waves");
    if (t->backend) {
        vec_push(argv, (char *)"--backend");
        vec_push(argv, (char *)t->backend);
    }
    if (t->engine) {
        vec_push(argv, (char *)"--sim");
        vec_push(argv, (char *)t->engine);
    }
    if (t->features) {
        vec_push(argv, (char *)"--features");
        vec_push(argv, (char *)t->features);
    }
    vec_push(argv, (char *)"--");
    for (i = 0; i < t->paths.len; i++)
        vec_push(argv, (char *)t->paths.data[i]);
    vec_push(argv, NULL);
    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    o.stdin_null = 1;
    p = os_spawn(argv.data, &o, err);
    free(exe);
    vec_free(argv);
    sb_free(&list);
    return p;
}

/* parses a finished worker; returns the chunk of tests still to run (n = 0
 * when everything got a result) */
static TestChunk test_collect(TestCmd *t, OsProc *p, const TestChunk *ch)
{
    size_t len, elen, i;
    const char *out = os_proc_stdout(p, &len);
    const char *err = os_proc_stderr(p, &elen);
    const char *q = out;
    TestJob *cur = NULL;
    StrBuf rep = {0};
    TestChunk rest;
    long crashed = -1;
    while (*q) {
        const char *e = strchr(q, '\n');
        size_t n;
        if (!e)
            e = q + strlen(q);
        n = (size_t)(e - q);
        if (n > 8 && strncmp(q, "@@start ", 8) == 0) {
            cur = test_by_index(t, (size_t)strtoull(q + 8, NULL, 10));
            sb_reset(&rep);
        } else if (n > 9 && strncmp(q, "@@result ", 9) == 0) {
            char *sp;
            size_t idx = (size_t)strtoull(q + 9, &sp, 10);
            TestJob *j = test_by_index(t, idx);
            if (j) {
                j->done = 1;
                j->passed = atoi(sp) == 1;
                j->report = xstrdup(sb_cstr(&rep));
            }
            cur = NULL;
            sb_reset(&rep);
        } else if (n >= 2 && q[0] == '|' && q[1] == ' ') {
            sb_append(&rep, q + 2, n - 2);
            sb_putc(&rep, '\n');
        } else if (n > 6 && strncmp(q, "@@cov\t", 6) == 0) {
            char *line = xstrndup(q, n);
            testcov_parse_line(&t->cov, line);
            free(line);
        }
        q = *e ? e + 1 : e;
    }
    rest.items = NULL;
    rest.n = 0;
    if (cur && !cur->done) {
        /* died while running `cur` */
        int code = os_proc_exit_code(p);
        sb_printf(&rep, "    the test process exited abnormally (status %d)\n", code);
        if (elen) {
            const char *s = err;
            if (elen > 2000)
                s = err + elen - 2000;
            sb_puts(&rep, "    stderr:\n");
            while (*s) {
                const char *e = strchr(s, '\n');
                if (!e)
                    e = s + strlen(s);
                sb_puts(&rep, "      ");
                sb_append(&rep, s, (size_t)(e - s));
                sb_putc(&rep, '\n');
                s = *e ? e + 1 : e;
            }
        }
        cur->done = 1;
        cur->passed = 0;
        cur->report = xstrdup(sb_cstr(&rep));
        crashed = 1;
    }
    for (i = 0; i < ch->n; i++) {
        TestJob *j = &t->tests.data[ch->items[i]];
        if (j->done)
            continue;
        if (crashed < 0 && !cur && rest.n == 0 && i == 0 && !strstr(out, "@@start")) {
            /* the worker failed before running anything: report it once */
            StrBuf r = {0};
            sb_printf(&r, "    the test process failed (status %d)\n", os_proc_exit_code(p));
            if (elen) {
                sb_puts(&r, "    ");
                sb_append(&r, err, elen > 2000 ? 2000 : elen);
                sb_putc(&r, '\n');
            }
            j->done = 1;
            j->passed = 0;
            j->report = sb_take(&r);
            continue;
        }
        if (!rest.items)
            rest.items = (size_t *)xcalloc(ch->n, sizeof(size_t));
        rest.items[rest.n++] = ch->items[i];
    }
    sb_free(&rep);
    return rest;
}

static void test_run_parallel(TestCmd *t, Driver *dr)
{
    TestChunkVec queue = {0};
    OsProc **procs;
    TestChunk *running;
    size_t i, nrun = 0, qi = 0, per;
    size_t nworkers = (size_t)t->jobs;
    TERN_UNUSED(dr);
    per = (t->tests.len + nworkers * 4 - 1) / (nworkers * 4);
    if (per < 1)
        per = 1;
    for (i = 0; i < t->tests.len; i += per) {
        TestChunk ch;
        size_t k;
        ch.n = TERN_MIN(per, t->tests.len - i);
        ch.items = (size_t *)xcalloc(ch.n, sizeof(size_t));
        for (k = 0; k < ch.n; k++)
            ch.items[k] = i + k;
        vec_push(queue, ch);
    }
    procs = (OsProc **)xcalloc(nworkers, sizeof(OsProc *));
    running = (TestChunk *)xcalloc(nworkers, sizeof(TestChunk));
    for (;;) {
        int w;
        /* fill free slots */
        for (i = 0; i < nworkers && qi < queue.len; i++) {
            char *err = NULL;
            if (procs[i])
                continue;
            procs[i] = test_spawn(t, &queue.data[qi], &err);
            if (!procs[i]) {
                size_t k;
                for (k = 0; k < queue.data[qi].n; k++) {
                    TestJob *j = &t->tests.data[queue.data[qi].items[k]];
                    StrBuf r = {0};
                    sb_printf(&r, "    cannot start a test process: %s\n", err ? err : "?");
                    j->done = 1;
                    j->passed = 0;
                    j->report = sb_take(&r);
                }
                free(err);
                qi++;
                continue;
            }
            running[i] = queue.data[qi++];
            nrun++;
        }
        if (!nrun)
            break;
        w = os_proc_wait_any(procs, nworkers, -1);
        if (w < 0)
            break;
        {
            TestChunk rest = test_collect(t, procs[w], &running[w]);
            os_proc_free(procs[w]);
            procs[w] = NULL;
            nrun--;
            if (rest.n)
                vec_push(queue, rest);
        }
        test_print_ready(t);
    }
    for (i = 0; i < queue.len; i++)
        free(queue.data[i].items);
    vec_free(queue);
    free(procs);
    free(running);
}

/* ---------------------------------------------------------------- main -- */

static int test_matches(const TestCmd *t, const char *name)
{
    size_t i;
    if (!t->filters.len)
        return 1;
    for (i = 0; i < t->filters.len; i++)
        if (strstr(name ? name : "", t->filters.data[i]))
            return 1;
    return 0;
}

int cmd_test(int argc, char **argv)
{
    ArgParser ap;
    TestCmd t;
    DriverOptions o;
    Driver *dr;
    DriverItemVec all = {0};
    const char *val;
    size_t i;
    uint64_t jobs = 0;
    SimEngine engine = SIM_ENGINE_AUTO;
    int rc = TERN_EXIT_OK;
    uint64_t start = os_time_ns();
    memset(&t, 0, sizeof t);
    memset(&o, 0, sizeof o);
    t.seed = TEST_DEFAULT_SEED;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("test");
            vec_free(t.paths);
            vec_free(t.filters);
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "waves"))
            t.waves = 1;
        else if (args_flag(&ap, "cover"))
            t.cover = 1;
        else if (args_flag(&ap, "list"))
            t.list = 1;
        else if (args_opt_u64(&ap, "seed|s", &t.seed))
            t.seed_given = 1;
        else if (args_opt_u64(&ap, "jobs|j", &jobs))
            ;
        else if (args_opt_u64(&ap, "const-eval-limit", &t.limit))
            ;
        else if (args_opt(&ap, "features", &val))
            t.features = val;
        else if (args_opt(&ap, "worker", &val))
            t.worker = val;
        else if (args_opt(&ap, "backend", &val)) {
            t.backend = val;
            if (strcmp(val, "builtin") == 0)
                t.be = TR_BACKEND_BUILTIN;
            else if (strcmp(val, "verilator") == 0)
                t.be = TR_BACKEND_VERILATOR;
            else {
                fprintf(stderr, "tern test: error: unknown backend `%s` (builtin or verilator)\n", val);
                ap.errors++;
            }
        } else if (args_opt(&ap, "sim", &val)) {
            t.engine = val;
            if (!sim_parse_engine(val, &engine)) {
                fprintf(stderr, "tern test: error: unknown --sim `%s` (auto, interp or compiled)\n", val);
                ap.errors++;
            }
        } else if (args_flag(&ap, "verbose|v"))
            t.verbose = 1;
        else if (args_positional(&ap)) {
            if (t.worker || os_exists(ap.arg))
                vec_push(t.paths, ap.arg);
            else
                vec_push(t.filters, ap.arg);
        } else
            args_unknown(&ap);
    }
    t.have_verilator = t.be != TR_BACKEND_BUILTIN && vls_available();
    if (!ap.errors && t.be == TR_BACKEND_VERILATOR && !t.have_verilator) {
        fprintf(stderr, "tern test: error: --backend verilator needs Verilator on PATH (https://verilator.org)\n");
        ap.errors++;
    }
    if (!ap.errors && engine == SIM_ENGINE_COMPILED && !sim_host_cc()) {
        fprintf(stderr, "tern test: error: --sim compiled needs a host C compiler ($CC, cc, gcc, clang or cl)\n");
        ap.errors++;
    }
    if (ap.errors) {
        vec_free(t.paths);
        vec_free(t.filters);
        return TERN_EXIT_USAGE;
    }
    t.jobs = jobs ? (int)TERN_MIN(jobs, 256) : os_cpu_count();
    /* Verilator models are separate processes (and compile once): one test at a time */
    if (t.be == TR_BACKEND_VERILATOR) {
        t.jobs = 1;
        if (t.cover && !t.worker)
            fprintf(stderr, "tern test: note: --cover counts only the built-in simulator; with --backend verilator "
                            "no coverage is collected\n");
    }
    t.color = !t.worker && os_color_enabled(1);
    o.paths = t.paths.data;
    o.npaths = t.paths.len;
    o.include_tests = 1;
    o.features = t.features;
    o.const_eval_limit = t.limit;
    o.cmd = "test";
    dr = driver_open(&o);
    if (!dr) {
        vec_free(t.paths);
        vec_free(t.filters);
        return TERN_EXIT_USAGE;
    }
    if (driver_parse_errors(dr)) {
        driver_free(dr);
        vec_free(t.paths);
        vec_free(t.filters);
        return TERN_EXIT_FAIL;
    }
    driver_sema(dr)->record_info = 0;   /* no hover/definition info needed */
    testrun_set_engine(dr, "test", t.engine, t.verbose && !t.worker);
    if (!t.worker) {
        driver_check_items(dr); /* file-level consts and static_asserts */
        if (diag_error_count()) {
            driver_free(dr);
            vec_free(t.paths);
            vec_free(t.filters);
            return TERN_EXIT_FAIL;
        }
    }
    driver_tests(dr, &all);
    if (t.worker) {
        rc = test_worker(&t, dr, &all);
        goto done;
    }
    for (i = 0; i < all.len; i++) {
        TestJob j;
        if (!test_matches(&t, all.data[i].item->u.test.name))
            continue;
        memset(&j, 0, sizeof j);
        j.di = all.data[i];
        j.index = i;
        vec_push(t.tests, j);
    }
    if (t.list) {
        for (i = 0; i < t.tests.len; i++) {
            const TestJob *j = &t.tests.data[i];
            printf("%s:%u: %s\n", diag_path_display(j->di.file->ast->path), (unsigned)j->di.item->span.line,
                   j->di.item->u.test.name ? j->di.item->u.test.name : "");
        }
        goto done;
    }
    if (!t.tests.len) {
        printf("no tests%s\n", t.filters.len ? " match the filter" : " found");
        goto done;
    }
    if (t.waves) {
        char *dir = NULL;
        for (i = 0; i < t.tests.len; i++)
            t.tests.data[i].waves = test_waves_path(dr, &t.tests.data[i].di);
        dir = os_path_dirname(t.tests.data[0].waves);
        os_mkdir_p(dir);
        free(dir);
    }
    printf("running %u test%s\n", (unsigned)t.tests.len, t.tests.len == 1 ? "" : "s");
    fflush(stdout);
    if (t.jobs <= 1 || t.tests.len < 4) {
        for (i = 0; i < t.tests.len; i++) {
            StrBuf rep = {0};
            test_run_one(&t, dr, &t.tests.data[i], &rep);
            t.tests.data[i].report = sb_take(&rep);
            test_print_ready(&t);
        }
    } else {
        test_run_parallel(&t, dr);
        test_print_ready(&t);
    }
    if (t.cover) {
        /* report the user's files only (not std or dependencies they instantiate) */
        StrBuf sb = {0};
        TestCov mine;
        Map user = {0};
        Sema *S = driver_sema(dr);
        memset(&mine, 0, sizeof mine);
        for (i = 0; i < S->files.len; i++)
            if (S->files.data[i]->is_user)
                map_put(&user, MAP_PTR(intern_cstr(S->files.data[i]->ast->path)), 1);
        for (i = 0; i < t.cov.pts.len; i++)
            if (map_has(&user, MAP_PTR(t.cov.pts.data[i].file)))
                testcov_add(&mine, &t.cov.pts.data[i]);
        map_free(&user);
        testcov_free(&t.cov);
        t.cov = mine;
        putchar('\n');
        testcov_report(&t.cov, &sb);
        fputs(sb_cstr(&sb), stdout);
        sb_free(&sb);
    }
    printf("\n%s: %u passed, %u failed", t.failed ? "FAILED" : "ok", (unsigned)t.passed, (unsigned)t.failed);
    if (os_isatty(1))
        printf(" (%.2fs)", (double)(os_time_ns() - start) / 1e9);
    putchar('\n');
    if (t.failed) {
        printf("replay a failing test with: tern test --seed %llu \"<name>\"\n", (unsigned long long)t.seed);
        rc = TERN_EXIT_FAIL;
    }
done:
    fflush(stdout);
    for (i = 0; i < t.tests.len; i++) {
        free(t.tests.data[i].report);
        free(t.tests.data[i].waves);
    }
    vec_free(t.tests);
    vec_free(all);
    testcov_free(&t.cov);
    driver_free(dr);
    vec_free(t.paths);
    vec_free(t.filters);
    return rc;
}

#undef TEST_DEFAULT_SEED
