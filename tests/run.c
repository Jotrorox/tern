/* run.c — golden-file test runner for the Tern compiler.
 *
 *     cc -o tests/run tests/run.c        (standalone translation unit)
 *     tests/run [--tern ./tern] [--fixtures tests/fixtures] [-j N]
 *               [--update] [--no-std] [-v] [<filter>...]
 *
 * Walks the fixtures directory. Each `foo.tern` is paired with any sibling
 * expectation files; for each one, ./tern runs with the fixture's directory
 * as working directory and the bare file name as argument:
 *
 *     foo.parse   tern parse --dump foo.tern                       (stdout)
 *     foo.err     tern check --diag-format=short foo.tern          (stderr)
 *     foo.sv      tern build --stdout --diag-format=short foo.tern (stdout)
 *     foo.test    tern test --diag-format=short foo.tern           (stdout)
 *     foo.ir      tern ir --diag-format=short foo.tern             (stdout: elaborated IR)
 *
 * Output and expectation are compared after normalization (CRLF -> LF,
 * trailing whitespace and trailing blank lines removed). A fixture whose
 * first line starts with `// skip` is skipped. A crash (signal / abnormal
 * exit) fails the case even when the output matches. With UPDATE=1 in the
 * environment (or --update) mismatching expectations are rewritten.
 *
 * Unless filters or --no-std are given, it also runs `tern test std` from
 * the current directory and requires exit status 0; while `tern test` is
 * still a stub ("not implemented yet", exit 2) this case is skipped.
 *
 * Fixtures run in parallel worker processes (-j, default: CPU count).
 */
#include "../src/os/os_posix.c"
#include "../src/os/os_win32.c"
#include "../src/os/os_common.c"
#include "../src/base/common.c"
#include "../src/base/arena.c"
#include "../src/base/map.c"
#include "../src/base/str.c"

#define RUN_TIMEOUT_NS (300ull * 1000000000ull)

typedef struct RunKind {
    const char *ext;
    const char *args[4];   /* tern arguments before the file name */
    int use_stderr;        /* compare stderr instead of stdout */
} RunKind;

static const RunKind run_kinds[] = {
    {".parse", {"parse", "--dump", NULL, NULL}, 0},
    {".err", {"check", "--diag-format=short", NULL, NULL}, 1},
    {".sv", {"build", "--stdout", "--diag-format=short", NULL}, 0},
    {".test", {"test", "--diag-format=short", NULL, NULL}, 0},
    {".ir", {"ir", "--diag-format=short", NULL, NULL}, 0},
};

typedef struct RunJob {
    char *tern;            /* fixture path as found (for messages) */
    char *dir;             /* its directory (child cwd) */
    const char *base;      /* its file name (points into tern) */
    char *expect;          /* expectation file, or NULL for the std job */
    const RunKind *kind;   /* NULL for the std job */
    int skipped;
    OsProc *proc;
    uint64_t start;
} RunJob;

typedef struct RunState {
    const char *tern_bin;
    int update;
    int verbose;
    int color;
    int passed, failed, skipped, updated;
    StrBuf report;         /* failure details, printed at the end */
} RunState;

/* --------------------------------------------------------------- helpers */

static const char *run_c(const RunState *st, const char *code)
{
    return st->color ? code : "";
}

/* CRLF -> LF, strip trailing whitespace per line and trailing blank lines. */
static char *run_normalize(const char *s, size_t n)
{
    StrBuf out = {0};
    size_t i = 0;
    while (i < n) {
        size_t j = i, end;
        while (j < n && s[j] != '\n')
            j++;
        end = j;
        while (end > i && (s[end - 1] == '\r' || s[end - 1] == ' ' || s[end - 1] == '\t'))
            end--;
        sb_append(&out, s + i, end - i);
        sb_putc(&out, '\n');
        i = j + 1;
    }
    while (out.len > 0 && out.data[out.len - 1] == '\n' &&
           (out.len == 1 || out.data[out.len - 2] == '\n'))
        out.len--;
    sb_cstr(&out);
    return sb_take(&out);
}

typedef struct RunLines {
    const char **p;
    size_t *n;
    size_t len;
} RunLines;

static RunLines run_split(const char *s)
{
    RunLines L;
    size_t cap = 16;
    L.p = (const char **)xmalloc(cap * sizeof(char *));
    L.n = (size_t *)xmalloc(cap * sizeof(size_t));
    L.len = 0;
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (L.len == cap) {
            cap *= 2;
            L.p = (const char **)xrealloc((void *)L.p, cap * sizeof(char *));
            L.n = (size_t *)xrealloc(L.n, cap * sizeof(size_t));
        }
        L.p[L.len] = s;
        L.n[L.len] = len;
        L.len++;
        s += len + (nl ? 1 : 0);
    }
    return L;
}

static int run_line_eq(const RunLines *a, size_t i, const RunLines *b, size_t j)
{
    return a->n[i] == b->n[j] && memcmp(a->p[i], b->p[j], a->n[i]) == 0;
}

/* Line diff (LCS) of expected vs actual into out, with 2 lines of context
 * and at most `max_lines` lines printed. */
static void run_diff(RunState *st, StrBuf *out, const char *expected, const char *actual)
{
    RunLines a = run_split(expected), b = run_split(actual);
    size_t n = a.len, m = b.len, i, j, printed = 0, max_lines = 80;
    unsigned short *dp = NULL;
    char *ops = NULL; /* ' ', '-', '+' */
    const char **opl = NULL;
    size_t *opn = NULL, nops = 0, k;
    if (n * m > 16u * 1000u * 1000u) {
        sb_printf(out, "    (outputs too large to diff: %lu vs %lu lines)\n", (unsigned long)n,
                  (unsigned long)m);
        goto done;
    }
    /* dp[i][j] = LCS length of a[i..] and b[j..], saturating */
    dp = (unsigned short *)xcalloc((n + 1) * (m + 1), sizeof(unsigned short));
    for (i = n; i-- > 0;) {
        for (j = m; j-- > 0;) {
            unsigned short v;
            if (run_line_eq(&a, i, &b, j)) {
                v = dp[(i + 1) * (m + 1) + j + 1];
                v = (unsigned short)(v == 0xFFFF ? v : v + 1);
            } else {
                unsigned short d1 = dp[(i + 1) * (m + 1) + j], d2 = dp[i * (m + 1) + j + 1];
                v = d1 > d2 ? d1 : d2;
            }
            dp[i * (m + 1) + j] = v;
        }
    }
    ops = (char *)xmalloc(n + m + 1);
    opl = (const char **)xmalloc((n + m + 1) * sizeof(char *));
    opn = (size_t *)xmalloc((n + m + 1) * sizeof(size_t));
    i = j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && run_line_eq(&a, i, &b, j)) {
            ops[nops] = ' ';
            opl[nops] = a.p[i];
            opn[nops++] = a.n[i];
            i++;
            j++;
        } else if (i < n && (j == m || dp[(i + 1) * (m + 1) + j] >= dp[i * (m + 1) + j + 1])) {
            ops[nops] = '-';
            opl[nops] = a.p[i];
            opn[nops++] = a.n[i];
            i++;
        } else {
            ops[nops] = '+';
            opl[nops] = b.p[j];
            opn[nops++] = b.n[j];
            j++;
        }
    }
    sb_printf(out, "    %s--- expected%s  %s+++ actual%s\n", run_c(st, "\x1b[31m"), run_c(st, "\x1b[0m"),
              run_c(st, "\x1b[32m"), run_c(st, "\x1b[0m"));
    for (k = 0; k < nops; k++) {
        int in_ctx = 0;
        size_t d;
        if (ops[k] == ' ') {
            for (d = (k >= 2 ? k - 2 : 0); d <= k + 2 && d < nops; d++) {
                if (ops[d] != ' ')
                    in_ctx = 1;
            }
            if (!in_ctx) {
                if (k > 0 && ops[k - 1] != ' ' && printed < max_lines) {
                    sb_puts(out, "    ...\n");
                    printed++;
                }
                continue;
            }
        }
        if (printed >= max_lines) {
            sb_puts(out, "    ... (diff truncated)\n");
            break;
        }
        sb_puts(out, "    ");
        if (ops[k] == '-')
            sb_puts(out, run_c(st, "\x1b[31m"));
        else if (ops[k] == '+')
            sb_puts(out, run_c(st, "\x1b[32m"));
        sb_putc(out, ops[k]);
        sb_append(out, opl[k], opn[k]);
        if (ops[k] != ' ')
            sb_puts(out, run_c(st, "\x1b[0m"));
        sb_putc(out, '\n');
        printed++;
    }
done:
    free(dp);
    free(ops);
    free((void *)opl);
    free(opn);
    free((void *)a.p);
    free(a.n);
    free((void *)b.p);
    free(b.n);
}

static int run_first_line_is_skip(const char *path)
{
    char *data;
    size_t len;
    const char *p;
    int skip;
    if (os_read_file(path, &data, &len) != 0)
        return 0;
    p = data;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3; /* UTF-8 BOM */
    while (*p == ' ' || *p == '\t')
        p++;
    skip = strncmp(p, "// skip", 7) == 0;
    free(data);
    return skip;
}

/* ------------------------------------------------------------ discovery */

typedef struct RunJobs {
    RunJob *items;
    size_t len, cap;
} RunJobs;

static RunJob *run_add_job(RunJobs *jobs)
{
    if (jobs->len == jobs->cap) {
        jobs->cap = jobs->cap ? jobs->cap * 2 : 64;
        jobs->items = (RunJob *)xrealloc(jobs->items, jobs->cap * sizeof(RunJob));
    }
    memset(&jobs->items[jobs->len], 0, sizeof(RunJob));
    return &jobs->items[jobs->len++];
}

static int run_matches(const char *path, char **filters, int nfilters)
{
    int i;
    if (nfilters == 0)
        return 1;
    for (i = 0; i < nfilters; i++) {
        if (strstr(path, filters[i]))
            return 1;
    }
    return 0;
}

static void run_discover(const char *dir, RunJobs *jobs, char **filters, int nfilters)
{
    OsDirList list;
    size_t i, k;
    if (os_list_dir(dir, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        const char *name = list.items[i].name;
        char *path;
        if (name[0] == '.')
            continue;
        path = os_path_join(dir, name);
        if (list.items[i].is_dir) {
            run_discover(path, jobs, filters, nfilters);
        } else if (strcmp(os_path_ext(name), ".tern") == 0 && run_matches(path, filters, nfilters)) {
            size_t stem = strlen(path) - 5;
            int skip = run_first_line_is_skip(path);
            for (k = 0; k < TERN_ARRAY_LEN(run_kinds); k++) {
                StrBuf e = {0};
                RunJob *j;
                sb_append(&e, path, stem);
                sb_puts(&e, run_kinds[k].ext);
                if (!os_is_file(e.data)) {
                    sb_free(&e);
                    continue;
                }
                j = run_add_job(jobs);
                j->tern = xstrdup(path);
                j->dir = os_path_dirname(path);
                j->base = os_path_basename(j->tern);
                j->expect = sb_take(&e);
                j->kind = &run_kinds[k];
                j->skipped = skip;
            }
        }
        free(path);
    }
    os_dir_list_free(&list);
}

/* ------------------------------------------------------------- running */

static OsProc *run_start(RunState *st, RunJob *j, char **err)
{
    char *argv[8];
    int n = 0, k;
    OsSpawnOpts o;
    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    o.stdin_null = 1;
    argv[n++] = (char *)st->tern_bin;
    if (j->kind) {
        for (k = 0; j->kind->args[k]; k++)
            argv[n++] = (char *)j->kind->args[k];
        argv[n++] = (char *)j->base;
        o.cwd = j->dir;
    } else {
        argv[n++] = (char *)"test";
        argv[n++] = (char *)"--diag-format=short";
        argv[n++] = (char *)"std";
    }
    argv[n] = NULL;
    j->start = os_time_ns();
    return os_spawn(argv, &o, err);
}

static int run_crashed(int code)
{
#ifdef _WIN32
    return code < 0 || code > 255; /* NTSTATUS exception codes */
#else
    return code >= 128 && code != 255;
#endif
}

static void run_fail_header(RunState *st, const RunJob *j, const char *why)
{
    sb_printf(&st->report, "%sFAIL%s %s%s%s: %s\n", run_c(st, "\x1b[1;31m"), run_c(st, "\x1b[0m"),
              j->tern, j->kind ? " " : "", j->kind ? j->kind->ext + 1 : "", why);
}

static void run_finish(RunState *st, RunJob *j)
{
    size_t out_len, err_len;
    const char *out = os_proc_stdout(j->proc, &out_len);
    const char *err = os_proc_stderr(j->proc, &err_len);
    int code = os_proc_exit_code(j->proc);
    double ms = (double)(os_time_ns() - j->start) / 1e6;

    if (!j->kind) {
        /* `tern test std` */
        if (code == 2 && (strstr(out, "not implemented yet") || strstr(err, "not implemented yet"))) {
            st->skipped++;
            if (st->verbose)
                printf("skip tern test std (not implemented yet)\n");
        } else if (code == 0) {
            st->passed++;
            if (st->verbose)
                printf("ok   tern test std (%.0f ms)\n", ms);
        } else {
            st->failed++;
            sb_printf(&st->report, "%sFAIL%s tern test std: exit status %d\n", run_c(st, "\x1b[1;31m"),
                      run_c(st, "\x1b[0m"), code);
            sb_append(&st->report, out, out_len);
            sb_append(&st->report, err, err_len);
            sb_putc(&st->report, '\n');
        }
        return;
    }
    {
        const char *raw = j->kind->use_stderr ? err : out;
        size_t raw_len = j->kind->use_stderr ? err_len : out_len;
        char *actual = run_normalize(raw, raw_len);
        char *exp_raw = NULL, *expected;
        size_t exp_len = 0;
        int same;
        if (os_read_file(j->expect, &exp_raw, &exp_len) != 0) {
            exp_raw = xstrdup("");
            exp_len = 0;
        }
        expected = run_normalize(exp_raw, exp_len);
        same = strcmp(actual, expected) == 0;
        if (run_crashed(code)) {
            char why[64];
            snprintf(why, sizeof why, "tern exited abnormally (status %d)", code);
            run_fail_header(st, j, why);
            if (!j->kind->use_stderr && err_len) {
                sb_puts(&st->report, "    stderr:\n");
                sb_append(&st->report, err, err_len);
            }
            st->failed++;
        } else if (same) {
            st->passed++;
            if (st->verbose)
                printf("ok   %s %s (%.0f ms)\n", j->tern, j->kind->ext + 1, ms);
        } else if (st->update) {
            if (os_write_file(j->expect, actual, strlen(actual)) == 0) {
                st->updated++;
                printf("updated %s\n", j->expect);
            } else {
                run_fail_header(st, j, "cannot write expectation file");
                st->failed++;
            }
        } else {
            run_fail_header(st, j, "output differs from expectation");
            run_diff(st, &st->report, expected, actual);
            if (!j->kind->use_stderr && err_len) {
                char *e = run_normalize(err, err_len);
                sb_puts(&st->report, "    stderr:\n");
                sb_puts(&st->report, e);
                free(e);
            }
            st->failed++;
        }
        free(actual);
        free(exp_raw);
        free(expected);
    }
}

static void run_all(RunState *st, RunJobs *jobs, int parallel)
{
    OsProc **slots = (OsProc **)xcalloc((size_t)parallel, sizeof(OsProc *));
    size_t *slot_job = (size_t *)xcalloc((size_t)parallel, sizeof(size_t));
    size_t next = 0, running = 0;
    int s;
    for (;;) {
        /* fill free slots */
        for (s = 0; s < parallel && next < jobs->len; s++) {
            RunJob *j;
            char *err = NULL;
            if (slots[s])
                continue;
            while (next < jobs->len && jobs->items[next].skipped) {
                st->skipped++;
                if (st->verbose)
                    printf("skip %s %s\n", jobs->items[next].tern, jobs->items[next].kind->ext + 1);
                next++;
            }
            if (next >= jobs->len)
                break;
            j = &jobs->items[next];
            j->proc = run_start(st, j, &err);
            if (!j->proc) {
                run_fail_header(st, j, err ? err : "cannot start tern");
                st->failed++;
                free(err);
                next++;
                continue;
            }
            slots[s] = j->proc;
            slot_job[s] = next;
            running++;
            next++;
        }
        if (running == 0 && next >= jobs->len)
            break;
        s = os_proc_wait_any(slots, (size_t)parallel, 250);
        if (s >= 0) {
            RunJob *j = &jobs->items[slot_job[s]];
            run_finish(st, j);
            os_proc_free(j->proc);
            j->proc = NULL;
            slots[s] = NULL;
            running--;
        } else {
            /* timeouts */
            int t;
            uint64_t now = os_time_ns();
            for (t = 0; t < parallel; t++) {
                RunJob *j;
                if (!slots[t])
                    continue;
                j = &jobs->items[slot_job[t]];
                if (now - j->start > RUN_TIMEOUT_NS) {
                    run_fail_header(st, j, "timed out");
                    st->failed++;
                    os_proc_free(j->proc);
                    j->proc = NULL;
                    slots[t] = NULL;
                    running--;
                }
            }
        }
    }
    free(slots);
    free(slot_job);
}

/* ---------------------------------------------------------------- main */

static void run_usage(void)
{
    printf("usage: tests/run [--tern <path>] [--fixtures <dir>] [-j <n>] [--update] [--no-std] [-v] "
           "[<filter>...]\n");
}

int main(int argc, char **argv)
{
    RunState st;
    RunJobs jobs = {0};
    const char *tern_arg = NULL, *fixtures = "tests/fixtures";
    char **filters = (char **)xcalloc((size_t)argc + 1, sizeof(char *));
    int nfilters = 0, parallel = os_cpu_count(), no_std = 0, i;
    char *env_update, *env_tern = NULL, *tern_abs;
    uint64_t t0 = os_time_ns();
    size_t k;

    memset(&st, 0, sizeof st);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--tern") == 0 && i + 1 < argc)
            tern_arg = argv[++i];
        else if (strcmp(a, "--fixtures") == 0 && i + 1 < argc)
            fixtures = argv[++i];
        else if (strcmp(a, "-j") == 0 && i + 1 < argc)
            parallel = atoi(argv[++i]);
        else if (strcmp(a, "--update") == 0)
            st.update = 1;
        else if (strcmp(a, "--no-std") == 0)
            no_std = 1;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0)
            st.verbose = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            run_usage();
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "tests/run: unknown option %s\n", a);
            run_usage();
            return 2;
        } else
            filters[nfilters++] = argv[i];
    }
    if (parallel < 1)
        parallel = 1;
    env_update = os_getenv("UPDATE");
    if (env_update && env_update[0] && strcmp(env_update, "0") != 0)
        st.update = 1;
    free(env_update);
    st.color = os_color_enabled(1);

    if (!tern_arg) {
        env_tern = os_getenv("TERN");
        tern_arg = env_tern ? env_tern : "./tern";
    }
    tern_abs = os_path_abs(tern_arg);
#ifdef _WIN32
    if (!os_is_file(tern_abs) && strcmp(os_path_ext(tern_abs), ".exe") != 0) {
        size_t n = strlen(tern_abs);
        tern_abs = (char *)xrealloc(tern_abs, n + 5);
        memcpy(tern_abs + n, ".exe", 5);
    }
#endif
    if (!os_is_file(tern_abs)) {
        fprintf(stderr, "tests/run: compiler binary not found: %s (build it first, or pass --tern)\n", tern_abs);
        return 2;
    }
    st.tern_bin = tern_abs;

    if (os_is_dir(fixtures))
        run_discover(fixtures, &jobs, filters, nfilters);
    if (!no_std && nfilters == 0 && os_is_dir("std")) {
        RunJob *j = run_add_job(&jobs);
        j->tern = xstrdup("std");
        j->base = j->tern;
    }

    run_all(&st, &jobs, parallel);

    if (st.report.len) {
        fputs("\n", stdout);
        fwrite(st.report.data, 1, st.report.len, stdout);
    }
    printf("\ngolden tests: %s%d passed%s, %s%d failed%s, %d skipped",
           st.passed ? run_c(&st, "\x1b[32m") : "", st.passed, st.passed ? run_c(&st, "\x1b[0m") : "",
           st.failed ? run_c(&st, "\x1b[1;31m") : "", st.failed, st.failed ? run_c(&st, "\x1b[0m") : "",
           st.skipped);
    if (st.updated)
        printf(", %d updated", st.updated);
    printf(" (%.0f ms)\n", (double)(os_time_ns() - t0) / 1e6);

    for (k = 0; k < jobs.len; k++) {
        free(jobs.items[k].tern);
        free(jobs.items[k].dir);
        free(jobs.items[k].expect);
    }
    free(jobs.items);
    free(filters);
    sb_free(&st.report);
    free(tern_abs);
    free(env_tern);
    intern_free_all();
    return st.failed ? 1 : 0;
}

#undef RUN_TIMEOUT_NS
