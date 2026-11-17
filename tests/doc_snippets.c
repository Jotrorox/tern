/* doc_snippets.c — keeps the Tern code in the user documentation compiling.
 *
 *     cc -o build/doc_snippets tests/doc_snippets.c   (standalone translation unit)
 *     build/doc_snippets [--tern ./tern] [--out build/doc-snippets] [-j N] [-v]
 *                        [<file.md | dir>...]
 *
 * Reads Markdown files (default: README.md and every .md file below
 * docs/guide/) and extracts every fenced code block whose info string starts
 * with `tern`. Each block is written to <out>/<md-stem>_L<line>.tern and
 * checked with the compiler. Attributes after the language, separated by
 * commas or spaces, choose what happens:
 *
 *     ```tern            tern check <snippet> must exit 0
 *     ```tern,test       also tern test <snippet> must exit 0
 *     ```tern,fail       tern check <snippet> must report an error, i.e. exit
 *                        with status 1 (a crash does not count): a snippet
 *                        that demonstrates a compile error
 *     ```tern,ignore     not checked (an intentionally partial fragment)
 *
 * Blocks with any other language (```sh, ```toml, ```systemverilog) are
 * ignored. Stale snippets of an earlier run are removed first; the files of
 * passing snippets are removed again, failing ones are kept for inspection
 * and the compiler's output is printed. Snippets run in parallel (-j,
 * default: CPU count) with the output directory as working directory. Exit status 0
 * when every snippet passes, 1 otherwise, 2 on usage errors.
 */
#include "../src/os/os_posix.c"
#include "../src/os/os_win32.c"
#include "../src/os/os_common.c"
#include "../src/base/common.c"
#include "../src/base/arena.c"
#include "../src/base/map.c"
#include "../src/base/str.c"

#define DS_TIMEOUT_NS (120ull * 1000000000ull)

enum { DS_CHECK, DS_TEST, DS_FAIL };

typedef struct DsJob {
    char *md;              /* Markdown file the block came from */
    unsigned line;         /* line of the opening fence */
    char *path;            /* extracted snippet */
    int mode;              /* DS_CHECK / DS_TEST / DS_FAIL */
    int phase;             /* 0: tern check, 1: tern test */
    int failed;
    OsProc *proc;
    uint64_t start;
} DsJob;

typedef struct DsJobs {
    DsJob *items;
    size_t len, cap;
} DsJobs;

typedef struct DsState {
    const char *tern_bin;
    const char *out_dir;
    int verbose;
    int passed, failed, ignored;
    StrBuf report;
} DsState;

static DsJob *ds_add_job(DsJobs *jobs)
{
    if (jobs->len == jobs->cap) {
        jobs->cap = jobs->cap ? jobs->cap * 2 : 32;
        jobs->items = (DsJob *)xrealloc(jobs->items, jobs->cap * sizeof(DsJob));
    }
    memset(&jobs->items[jobs->len], 0, sizeof(DsJob));
    return &jobs->items[jobs->len++];
}

/* Does the comma/space separated attribute list contain `word`? */
static int ds_has_attr(const char *attrs, size_t n, const char *word)
{
    size_t i = 0, wl = strlen(word);
    while (i < n) {
        size_t j;
        while (i < n && (attrs[i] == ',' || attrs[i] == ' ' || attrs[i] == '\t'))
            i++;
        j = i;
        while (j < n && attrs[j] != ',' && attrs[j] != ' ' && attrs[j] != '\t')
            j++;
        if (j - i == wl && memcmp(attrs + i, word, wl) == 0)
            return 1;
        i = j;
    }
    return 0;
}

/* A fence line: up to three spaces, then ``` (or more backticks). Returns the
 * number of backticks (0 when it is not a fence) and the info string. */
static size_t ds_fence(const char *s, size_t n, const char **info, size_t *info_len)
{
    size_t i = 0, ticks = 0;
    while (i < n && i < 3 && s[i] == ' ')
        i++;
    while (i < n && s[i] == '`') {
        ticks++;
        i++;
    }
    if (ticks < 3)
        return 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    *info = s + i;
    *info_len = n - i;
    while (*info_len > 0 && ((*info)[*info_len - 1] == ' ' || (*info)[*info_len - 1] == '\t' ||
                             (*info)[*info_len - 1] == '\r'))
        (*info_len)--;
    return ticks;
}

static void ds_extract(DsState *st, const char *md, DsJobs *jobs)
{
    char *data = NULL, *stem;
    size_t len = 0, i = 0;
    unsigned line = 0, open_line = 0;
    size_t open_ticks = 0;
    int in_block = 0, keep = 0, mode = DS_CHECK;
    StrBuf code = {0};
    const char *base;

    if (os_read_file(md, &data, &len) != 0) {
        sb_printf(&st->report, "%s: cannot read\n", md);
        st->failed++;
        return;
    }
    base = os_path_basename(md);
    stem = xstrdup(base);
    if (strlen(stem) > 3 && tern_ends_with(stem, ".md"))
        stem[strlen(stem) - 3] = 0;

    while (i < len) {
        size_t j = i, n;
        const char *info = NULL;
        size_t info_len = 0, ticks;
        while (j < len && data[j] != '\n')
            j++;
        n = j - i;
        line++;
        ticks = ds_fence(data + i, n, &info, &info_len);
        if (!in_block) {
            if (ticks) {
                size_t lang = 0;
                while (lang < info_len && info[lang] != ',' && info[lang] != ' ' && info[lang] != '\t' &&
                       info[lang] != '{')
                    lang++;
                in_block = 1;
                open_ticks = ticks;
                open_line = line;
                keep = lang == 4 && memcmp(info, "tern", 4) == 0;
                if (keep) {
                    const char *attrs = info + lang;
                    size_t an = info_len - lang;
                    if (ds_has_attr(attrs, an, "ignore")) {
                        keep = 0;
                        st->ignored++;
                    } else if (ds_has_attr(attrs, an, "fail"))
                        mode = DS_FAIL;
                    else if (ds_has_attr(attrs, an, "test"))
                        mode = DS_TEST;
                    else
                        mode = DS_CHECK;
                }
                sb_reset(&code);
            }
        } else if (ticks >= open_ticks && info_len == 0) {
            in_block = 0;
            if (keep) {
                StrBuf p = {0};
                char name[256];
                DsJob *job;
                snprintf(name, sizeof name, "%s_L%u.tern", stem, open_line);
                sb_puts(&p, st->out_dir);
                job = ds_add_job(jobs);
                job->md = xstrdup(md);
                job->line = open_line;
                job->path = os_path_join(sb_cstr(&p), name);
                job->mode = mode;
                if (os_write_file(job->path, code.data ? code.data : "", code.len) != 0) {
                    sb_printf(&st->report, "%s:%u: cannot write %s\n", md, open_line, job->path);
                    job->failed = 1;
                }
                sb_free(&p);
            }
        } else if (keep) {
            sb_append(&code, data + i, n);
            sb_putc(&code, '\n');
        }
        i = j + 1;
    }
    if (in_block && keep) {
        sb_printf(&st->report, "%s:%u: unterminated code block\n", md, open_line);
        st->failed++;
    }
    sb_free(&code);
    free(stem);
    free(data);
}

static void ds_discover(DsState *st, const char *path, DsJobs *jobs)
{
    OsDirList list;
    size_t i;
    if (!os_is_dir(path)) {
        ds_extract(st, path, jobs);
        return;
    }
    if (os_list_dir(path, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        const char *name = list.items[i].name;
        char *p;
        if (name[0] == '.')
            continue;
        p = os_path_join(path, name);
        if (list.items[i].is_dir || strcmp(os_path_ext(name), ".md") == 0)
            ds_discover(st, p, jobs);
        free(p);
    }
    os_dir_list_free(&list);
}

static OsProc *ds_start(DsState *st, DsJob *j, char **err)
{
    char *argv[8];
    int n = 0;
    OsSpawnOpts o;
    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    o.stdin_null = 1;
    o.cwd = st->out_dir;
    argv[n++] = (char *)st->tern_bin;
    argv[n++] = (char *)(j->phase == 0 ? "check" : "test");
    argv[n++] = (char *)"--diag-format=short";
    argv[n++] = (char *)"--color=never";
    argv[n++] = j->path;
    argv[n] = NULL;
    j->start = os_time_ns();
    return os_spawn(argv, &o, err);
}

/* Records the result of one phase. Returns 1 when another phase follows. */
static int ds_finish(DsState *st, DsJob *j)
{
    size_t out_len = 0, err_len = 0;
    const char *out = os_proc_stdout(j->proc, &out_len);
    const char *err = os_proc_stderr(j->proc, &err_len);
    int code = os_proc_exit_code(j->proc);
    int ok = j->mode == DS_FAIL ? (code == 1) : (code == 0);
    const char *cmd = j->phase == 0 ? "check" : "test";
    if (!ok) {
        sb_printf(&st->report, "FAIL %s:%u (tern %s %s, exit %d%s)\n", j->md, j->line, cmd, j->path, code,
                  j->mode == DS_FAIL ? ", expected a compile error" : "");
        if (out_len)
            sb_append(&st->report, out, out_len);
        if (err_len)
            sb_append(&st->report, err, err_len);
        sb_putc(&st->report, '\n');
        j->failed = 1;
        return 0;
    }
    if (j->mode == DS_TEST && j->phase == 0) {
        j->phase = 1;
        return 1;
    }
    return 0;
}

static void ds_run(DsState *st, DsJobs *jobs, int parallel)
{
    OsProc **slots = (OsProc **)xcalloc((size_t)parallel, sizeof(OsProc *));
    size_t *slot_job = (size_t *)xcalloc((size_t)parallel, sizeof(size_t));
    size_t next = 0, running = 0;
    int s;
    for (;;) {
        for (s = 0; s < parallel && next < jobs->len; s++) {
            DsJob *j;
            char *err = NULL;
            if (slots[s])
                continue;
            while (next < jobs->len && jobs->items[next].failed)
                next++;
            if (next >= jobs->len)
                break;
            j = &jobs->items[next];
            j->proc = ds_start(st, j, &err);
            if (!j->proc) {
                sb_printf(&st->report, "FAIL %s:%u: %s\n", j->md, j->line, err ? err : "cannot start tern");
                j->failed = 1;
                free(err);
                next++;
                continue;
            }
            slots[s] = j->proc;
            slot_job[s] = next++;
            running++;
        }
        if (running == 0 && next >= jobs->len)
            break;
        s = os_proc_wait_any(slots, (size_t)parallel, 250);
        if (s >= 0) {
            DsJob *j = &jobs->items[slot_job[s]];
            int again = ds_finish(st, j);
            os_proc_free(j->proc);
            j->proc = NULL;
            slots[s] = NULL;
            running--;
            if (again) {
                char *err = NULL;
                j->proc = ds_start(st, j, &err);
                if (j->proc) {
                    slots[s] = j->proc;
                    running++;
                } else {
                    sb_printf(&st->report, "FAIL %s:%u: %s\n", j->md, j->line, err ? err : "cannot start tern");
                    j->failed = 1;
                    free(err);
                }
            }
        } else {
            uint64_t now = os_time_ns();
            int t;
            for (t = 0; t < parallel; t++) {
                DsJob *j;
                if (!slots[t])
                    continue;
                j = &jobs->items[slot_job[t]];
                if (now - j->start > DS_TIMEOUT_NS) {
                    sb_printf(&st->report, "FAIL %s:%u: timed out\n", j->md, j->line);
                    j->failed = 1;
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

/* Removes the snippets a previous run left behind (only `*.tern` files
 * directly in the output directory, which belongs to this program). */
static void ds_clear_stale(const char *dir)
{
    OsDirList list;
    size_t i;
    if (os_list_dir(dir, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        if (!list.items[i].is_dir && strcmp(os_path_ext(list.items[i].name), ".tern") == 0) {
            char *p = os_path_join(dir, list.items[i].name);
            os_remove(p);
            free(p);
        }
    }
    os_dir_list_free(&list);
}

static void ds_usage(void)
{
    printf("usage: doc_snippets [--tern <path>] [--out <dir>] [-j <n>] [-v] [<file.md|dir>...]\n");
}

int main(int argc, char **argv)
{
    DsState st;
    DsJobs jobs = {0};
    const char *tern_arg = NULL;
    char **inputs = (char **)xcalloc((size_t)argc + 1, sizeof(char *));
    int ninputs = 0, parallel = os_cpu_count(), i;
    char *env_tern = NULL, *tern_abs, *out_abs;
    const char *out_dir = "build/doc-snippets";
    uint64_t t0 = os_time_ns();
    size_t k;

    memset(&st, 0, sizeof st);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--tern") == 0 && i + 1 < argc)
            tern_arg = argv[++i];
        else if (strcmp(a, "--out") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (strcmp(a, "-j") == 0 && i + 1 < argc)
            parallel = atoi(argv[++i]);
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0)
            st.verbose = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            ds_usage();
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "doc_snippets: unknown option %s\n", a);
            ds_usage();
            return 2;
        } else
            inputs[ninputs++] = argv[i];
    }
    if (parallel < 1)
        parallel = 1;

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
        fprintf(stderr, "doc_snippets: compiler binary not found: %s (build it first, or pass --tern)\n", tern_abs);
        return 2;
    }
    st.tern_bin = tern_abs;
    out_abs = os_path_abs(out_dir);
    if (os_mkdir_p(out_abs) != 0) {
        fprintf(stderr, "doc_snippets: cannot create %s\n", out_abs);
        return 2;
    }
    st.out_dir = out_abs;
    ds_clear_stale(out_abs);

    if (ninputs == 0) {
        if (os_is_file("README.md"))
            ds_discover(&st, "README.md", &jobs);
        if (os_is_dir("docs/guide"))
            ds_discover(&st, "docs/guide", &jobs);
    } else {
        for (i = 0; i < ninputs; i++)
            ds_discover(&st, inputs[i], &jobs);
    }

    ds_run(&st, &jobs, parallel);

    for (k = 0; k < jobs.len; k++) {
        DsJob *j = &jobs.items[k];
        if (j->failed)
            st.failed++;
        else {
            st.passed++;
            if (st.verbose)
                printf("ok   %s:%u\n", j->md, j->line);
            os_remove(j->path);
        }
    }
    if (st.report.len) {
        fputs("\n", stdout);
        fwrite(st.report.data, 1, st.report.len, stdout);
    }
    printf("doc snippets: %d passed, %d failed, %d ignored (%.0f ms)\n", st.passed, st.failed, st.ignored,
           (double)(os_time_ns() - t0) / 1e6);

    for (k = 0; k < jobs.len; k++) {
        free(jobs.items[k].md);
        free(jobs.items[k].path);
    }
    free(jobs.items);
    free(inputs);
    sb_free(&st.report);
    free(tern_abs);
    free(out_abs);
    free(env_tern);
    intern_free_all();
    return st.failed ? 1 : 0;
}

#undef DS_TIMEOUT_NS
