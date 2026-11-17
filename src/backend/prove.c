/* prove.c — the `tern prove` command (spec §11.5): elaborate the module,
 * write the formal files (formal.c) to build/formal/<Module>/, run
 * SymbiYosys when it is installed, report PASS/FAIL per check and turn the
 * first counterexample into a Tern regression test.
 *
 *     tern prove <module> [--depth <n>] [--induct] [--engine "<e>"] [--keep]
 *                [--print-test] [--no-run] [--features <l>] [<files>...]
 */
#include "../base/common.h"
#include "../base/args.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/diag.h"
#include "../os/os.h"
#include "../driver.h"
#include "../check/check.h"
#include "../cmds.h"
#include "backend.h"

static const char *pv_status_name(FvStatus s)
{
    switch (s) {
    case FV_PASS: return "PASS";
    case FV_FAIL: return "FAIL";
    case FV_REACHED: return "REACHED";
    case FV_UNREACHED: return "UNREACHED";
    case FV_SKIPPED: return "SKIPPED";
    case FV_UNKNOWN: break;
    }
    return "UNKNOWN";
}

static const char *pv_kind_word(const FvCheck *c)
{
    int prop = c->name != NULL;
    switch (c->kind) {
    case IRA_ASSUME: return prop ? "assume property" : "assume";
    case IRA_COVER: return prop ? "cover property" : "cover";
    default: return prop ? "property" : "assert";
    }
}

/* "property `no_overflow` (fifo.tern:31)" (heap) */
static char *pv_describe(const FvCheck *c)
{
    StrBuf sb = {0};
    const SourceFile *sf = c->loc.file ? src_get(c->loc.file) : NULL;
    sb_puts(&sb, pv_kind_word(c));
    if (c->name)
        sb_printf(&sb, " `%s`", c->name);
    else if (c->msg)
        sb_printf(&sb, " \"%s\"", c->msg);
    if (sf && sf->path)
        sb_printf(&sb, " (%s:%u)", diag_path_display(sf->path), (unsigned)c->loc.line);
    return sb_take(&sb);
}

/* the package declared by the .tern files already in dir (files of one
 * directory must agree), or NULL (heap) */
static char *pv_tests_package(const char *dir)
{
    OsDirList list;
    size_t i;
    char *pkg = NULL;
    if (!os_is_dir(dir) || os_list_dir(dir, &list) != 0)
        return NULL;
    for (i = 0; i < list.len && !pkg; i++) {
        char *path, *text = NULL;
        size_t len = 0;
        const char *p;
        if (list.items[i].is_dir || strcmp(os_path_ext(list.items[i].name), ".tern") != 0)
            continue;
        path = os_path_join(dir, list.items[i].name);
        if (os_read_file(path, &text, &len) == 0) {
            for (p = text; p && *p;) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (strncmp(p, "package ", 8) == 0) {
                    const char *q = p + 8, *e;
                    while (*q == ' ')
                        q++;
                    e = q;
                    while (*e && (tern_is_ident((unsigned char)*e) || *e == ':'))
                        e++;
                    if (e > q)
                        pkg = xstrndup(q, (size_t)(e - q));
                    break;
                }
                if (*p != '/' && *p != '\n' && *p != '\r')
                    break; /* the first item: no package line */
                p = strchr(p, '\n');
                if (p)
                    p++;
            }
        }
        free(text);
        free(path);
    }
    os_dir_list_free(&list);
    return pkg;
}

static void pv_sanitize(char *s)
{
    for (; *s; s++)
        if (!tern_is_alnum((unsigned char)*s) && *s != '_')
            *s = '_';
}

/* the lines of `log` tagged "[<name>_<task>]" (bmc: all lines if none is) */
static char *pv_task_lines(const char *log, const char *name, const char *task)
{
    StrBuf sb = {0}, tag = {0};
    const char *p = log;
    sb_printf(&tag, "[%s_%s]", name, task);
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p + 1) : strlen(p);
        char *line = xstrndup(p, n);
        if (strstr(line, tag.data))
            sb_append(&sb, p, n);
        free(line);
        p += n;
    }
    if (!sb.len && strcmp(task, "bmc") == 0)
        sb_puts(&sb, log);
    sb_free(&tag);
    return sb_take(&sb);
}

/* elaborates the module to prove: with its default generics, else the
 * specialization the design's top instantiates */
static const IrModule *pv_find_module(Driver *dr, const char *name, Arena *ira, IrDesign **dout)
{
    FileUnit *f = NULL;
    Item *it = strchr(name, '<') ? NULL : driver_find_item(dr, name, &f);
    uint32_t g, i;
    int defaults = 1;
    IrDesign *d;
    if (strchr(name, '<')) {
        /* `Fifo<u8, 4>`: elaborated with those generic arguments, exactly
         * as `tern sim 'Fifo<u8, 4>'` does */
        d = driver_elaborate_spec(dr, name, ira);
        *dout = d;
        if (d && d->top && d->top->is_extern) {
            diag_error(span_none(), "cannot find module `%s` (extern modules are black boxes and cannot be proven)",
                       name);
            return NULL;
        }
        return d ? d->top : NULL;
    }
    if (!it) {
        /* not a user item: a module the design instantiates (std::fifo::Fifo
         * imported with `use`), by source name or emitted name */
        const char *iname = intern_cstr(name);
        int errs = diag_error_count();
        d = driver_elaborate_top(dr, NULL, ira);
        *dout = d;
        if (d && diag_error_count() == errs)
            for (i = 0; i < d->nmodules; i++)
                if (!d->modules[i]->is_extern &&
                    (strcmp(d->modules[i]->tern_name, iname) == 0 || strcmp(d->modules[i]->name, name) == 0))
                    return d->modules[i];
        if (diag_error_count() != errs)
            return NULL;
    }
    if (!it || it->kind != ITEM_MODULE) {
        diag_error(span_none(), "cannot find module `%s`%s", name,
                   it && it->kind == ITEM_EXTERN ? " (extern modules are black boxes and cannot be proven)" : "");
        return NULL;
    }
    for (g = 0; g < it->u.module.generics.n; g++)
        if (!it->u.module.generics.items[g].default_type && !it->u.module.generics.items[g].default_value)
            defaults = 0;
    if (defaults) {
        d = driver_elaborate_module(dr, it, f, NULL, ira);
        *dout = d;
        return d ? d->top : NULL;
    }
    d = driver_elaborate_top(dr, NULL, ira);
    *dout = d;
    if (!d)
        return NULL;
    for (i = 0; i < d->nmodules; i++)
        if (d->modules[i]->tern_name == it->name.name && d->modules[i]->loc.file == it->name.span.file) {
            uint32_t k, others = 0;
            for (k = i + 1; k < d->nmodules; k++)
                if (d->modules[k]->tern_name == it->name.name && d->modules[k]->loc.file == it->name.span.file)
                    others++;
            if (others)
                printf("tern prove: %s is instantiated with %u other generic argument list%s; proving %s\n", name,
                       (unsigned)others, others == 1 ? "" : "s", d->modules[i]->display);
            return d->modules[i];
        }
    diag_error(it->name.span, "`%s` needs generic arguments and the design's top does not instantiate it", name);
    diag_help("give the generics defaults, or prove a module that instantiates it");
    return NULL;
}

/* writes (or prints) the regression test for the first failing check */
static void pv_write_test(Driver *dr, const IrDesign *d, const IrModule *m, FvPlan *plan, const FvOptions *fo,
                          FvCheck *fail, const char *dir, const char *root, const char *cwd, int print_test)
{
    char *vp = os_path_join(dir, fail->trace), *text = NULL, *err = NULL;
    char *vdisp = cwd ? os_path_relative(vp, cwd) : xstrdup(vp);
    size_t len = 0;
    VcdrTrace tr;
    TERN_UNUSED(dr);
    printf("counterexample: %s\n", vdisp);
    if (os_read_file(vp, &text, &len) == 0 && vcdr_parse(text, len, &tr, &err) == 0) {
        FvCex cx;
        StrBuf test = {0}, tn = {0}, up = {0}, fname = {0};
        SvNames *names = sv_names_new(d);
        const SourceFile *sf = fail->loc.file ? src_get(fail->loc.file) : NULL;
        char *tdir = os_path_join(root, "tests");
        char *pkg = pv_tests_package(tdir);
        char *desc = pv_describe(fail);
        char *srcl = NULL;
        memset(&cx, 0, sizeof cx);
        cx.module = m;
        cx.scope = plan->wrapper;
        cx.names = names;
        sb_printf(&tn, "formal: %s %s", m->tern_name, fail->name ? fail->name : "assert");
        cx.test_name = tn.data;
        cx.package = pkg;
        if (m->package && *m->package && (!pkg || strcmp(pkg, m->package) != 0)) {
            sb_printf(&up, "%s::%s", m->package, m->tern_name);
            cx.use_path = up.data;
        }
        cx.failing = desc;
        if (sf) {
            size_t ll = 0;
            const char *l = src_line(sf, fail->loc.line, &ll);
            if (l) {
                while (ll && (*l == ' ' || *l == '\t')) {
                    l++;
                    ll--;
                }
                while (ll && (l[ll - 1] == '\n' || l[ll - 1] == '\r'))
                    ll--;
                srcl = xstrndup(l, ll);
                cx.source_line = srcl;
            }
        }
        cx.reset_cycles = fo->reset_cycles;
        cx.fail_step = fail->step;
        if (fv_cex_to_test(&cx, &tr, &test, &err) == 0) {
            if (print_test) {
                fwrite(test.data, 1, test.len, stdout);
            } else {
                char *fp;
                sb_printf(&fname, "formal_%s_%s", m->tern_name, fail->name ? fail->name : "assert");
                pv_sanitize(fname.data);
                sb_puts(&fname, ".tern");
                os_mkdir_p(tdir);
                fp = os_path_join(tdir, fname.data);
                if (os_write_file(fp, test.data, test.len) == 0) {
                    char *fd = cwd ? os_path_relative(fp, cwd) : xstrdup(fp);
                    printf("regression test: %s (run it with `tern test`)\n", fd);
                    free(fd);
                } else {
                    diag_error(span_none(), "cannot write `%s`", fp);
                }
                free(fp);
            }
        } else {
            printf("  (no test generated: %s)\n", err ? err : "?");
        }
        free(srcl);
        free(desc);
        free(pkg);
        free(tdir);
        sb_free(&test);
        sb_free(&tn);
        sb_free(&up);
        sb_free(&fname);
        sv_names_free(names);
        vcdr_free(&tr);
    } else {
        printf("  (cannot read the trace%s%s)\n", err ? ": " : "", err ? err : "");
    }
    free(err);
    free(text);
    free(vp);
    free(vdisp);
}

int cmd_prove(int argc, char **argv)
{
    static const char *const tasks[] = {"bmc", "prove", "cover"};
    ArgParser ap;
    BkPathVec paths = {0};
    const char *module = NULL, *val = NULL, *features = NULL;
    FvOptions fo;
    uint64_t depth = 0;
    int keep = 0, print_test = 0, no_run = 0, rc = TERN_EXIT_OK;
    DriverOptions dopt;
    Driver *dr;
    Arena ira = {0};
    IrDesign *d = NULL;
    const IrModule *m;
    FvPlan plan;
    CheckOptions co;
    char *cwd = NULL, *root = NULL, *dir = NULL, *dir_disp = NULL, *sby = NULL;
    size_t i;

    fv_options_init(&fo);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("prove");
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_opt_u64(&ap, "depth", &depth)) {
            fo.depth = depth ? (uint32_t)depth : 1;
        } else if (args_flag(&ap, "induct")) {
            fo.induct = 1;
        } else if (args_opt(&ap, "engine", &val)) {
            fo.engine = val;
        } else if (args_flag(&ap, "keep")) {
            keep = 1;
        } else if (args_flag(&ap, "print-test")) {
            print_test = 1;
        } else if (args_flag(&ap, "no-run")) {
            no_run = 1;
        } else if (args_opt(&ap, "features", &val)) {
            features = val;
        } else if (args_positional(&ap)) {
            if (!module && !os_exists(ap.arg))
                module = ap.arg;
            else
                vec_push(paths, ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors) {
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    memset(&dopt, 0, sizeof dopt);
    dopt.paths = paths.data;
    dopt.npaths = paths.len;
    dopt.cmd = "prove";
    dr = driver_open(&dopt);
    vec_free(paths);
    if (!dr)
        return TERN_EXIT_USAGE;
    memset(&plan, 0, sizeof plan);
    if (driver_parse_errors(dr) || driver_apply_features(dr, features) != 0) {
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    if (!module) {
        /* default: the one #[top] */
        DriverItemVec tops = {0};
        driver_tops(dr, &tops);
        if (tops.len == 1)
            module = tops.data[0].item->name.name;
        vec_free(tops);
        if (!module) {
            fprintf(stderr, "tern prove: error: which module? (tern prove <module> [files...])\n");
            rc = TERN_EXIT_USAGE;
            goto done;
        }
    }
    m = pv_find_module(dr, module, &ira, &d);
    memset(&co, 0, sizeof co);
    if (!m || diag_error_count() || check_design(d, &co) || diag_error_count() || fv_generate(d, m, &fo, &plan) != 0) {
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    cwd = os_getcwd();
    root = dr->has_project ? xstrdup(dr->prj.manifest.root) : xstrdup(cwd ? cwd : ".");
    {
        char *b = os_path_join(root, "build"), *f = os_path_join(b, "formal");
        dir = os_path_join(f, plan.name);
        free(b);
        free(f);
    }
    dir_disp = cwd ? os_path_relative(dir, cwd) : xstrdup(dir);
    {
        /* project-relative source paths in the generated files */
        const char *from[3], *to[3];
        size_t n = 0;
        if (dr->std_source && strcmp(dr->std_source, "embedded") != 0) {
            from[n] = dr->std_source;
            to[n++] = "std/";
        }
        from[n] = root;
        to[n++] = "";
        if (cwd && strcmp(cwd, root) != 0) {
            from[n] = cwd;
            to[n++] = "";
        }
        bk_rewrite_paths(&plan.files, from, to, n);
    }
    if (os_mkdir_p(dir) != 0) {
        diag_error(span_none(), "cannot create directory `%s`", dir);
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    for (i = 0; i < plan.files.nfiles; i++) {
        char *p = os_path_join(dir, plan.files.files[i].name);
        if (os_write_file_if_changed(p, plan.files.files[i].data, plan.files.files[i].len) < 0) {
            diag_error(span_none(), "cannot write `%s`", p);
            rc = TERN_EXIT_FAIL;
        }
        free(p);
    }
    if (rc != TERN_EXIT_OK)
        goto done;
    {
        size_t na = 0, nc = 0, ns = 0;
        for (i = 0; i < plan.checks.len; i++) {
            const FvCheck *c = &plan.checks.data[i];
            if (c->status == FV_SKIPPED)
                ns++;
            else if (c->kind == IRA_COVER)
                nc++;
            else
                na++;
        }
        printf("tern prove: %s (depth %u%s) -> %s/\n", m->display, (unsigned)fo.depth,
               fo.induct ? ", k-induction" : "", dir_disp);
        printf("  %u assertion%s, %u cover%s", (unsigned)na, na == 1 ? "" : "s", (unsigned)nc, nc == 1 ? "" : "s");
        if (ns)
            printf(", %u not translated", (unsigned)ns);
        printf("\n");
        if (!plan.checks.len)
            printf("  note: %s has no assertions, covers or properties\n", m->display);
    }
    sby = os_find_program("sby");
    if (no_run || !sby) {
        for (i = 0; i < plan.checks.len; i++) {
            const FvCheck *c = &plan.checks.data[i];
            if (c->status == FV_SKIPPED) {
                char *desc = pv_describe(c);
                printf("  %-9s %s -- %s\n", "SKIPPED", desc, c->note ? c->note : "");
                free(desc);
            }
        }
    }
    if (no_run) {
        printf("  run it: cd %s && sby -f %s.sby\n", dir_disp, plan.name);
        goto done;
    }
    if (!sby) {
        fflush(stdout);
        fprintf(stderr,
                "tern prove: error: SymbiYosys (`sby`) is not on PATH; the formal files are in %s/\n"
                "  install Yosys, SymbiYosys and an SMT solver (yices2), e.g. from the OSS CAD Suite\n"
                "  (https://github.com/YosysHQ/oss-cad-suite-build), then run `tern prove %s` again\n"
                "  or run sby by hand: cd %s && sby -f %s.sby\n",
                dir_disp, module, dir_disp, plan.name);
        rc = TERN_EXIT_USAGE;
        goto done;
    }
    {
        char *av[4];
        StrBuf sbyfile = {0};
        OsSpawnOpts so;
        OsRunResult res;
        int any_fail = 0, any_error = 0;
        FvCheck *first_fail = NULL;
        char *logp;
        sb_printf(&sbyfile, "%s.sby", plan.name);
        av[0] = sby;
        av[1] = (char *)"-f";
        av[2] = sbyfile.data;
        av[3] = NULL;
        memset(&so, 0, sizeof so);
        so.cwd = dir;
        so.stdin_null = 1;
        so.capture_stdout = 1;
        so.merge_stderr = 1;
        printf("  running sby ...\n");
        fflush(stdout);
        os_run(av, &so, &res);
        logp = os_path_join(dir, "sby.log");
        os_write_file(logp, res.out ? res.out : "", res.out_len);
        for (i = 0; i < TERN_ARRAY_LEN(tasks); i++) {
            char *lines;
            FvStatus st;
            if ((i == 1 && !fo.induct) || (i == 2 && !plan.has_cover))
                continue;
            lines = pv_task_lines(res.out ? res.out : "", plan.name, tasks[i]);
            st = fv_parse_sby_log(&plan, tasks[i], lines);
            if (st == FV_FAIL && i != 2)
                any_fail = 1;
            if (st == FV_UNKNOWN) {
                if (i == 1 && strstr(lines, "nduction failed"))
                    printf("  prove: induction failed at depth %u (not proven; try a larger --depth)\n",
                           (unsigned)fo.depth);
                else
                    any_error = 1;
            } else if (i == 1 && st == FV_PASS) {
                printf("  prove: PASS (unbounded, by k-induction)\n");
            }
            free(lines);
        }
        if (any_error)
            printf("  sby did not finish cleanly%s; see %s/sby.log\n", res.exit_code < 0 ? " (could not start it)" : "",
                   dir_disp);
        for (i = 0; i < plan.checks.len; i++) {
            FvCheck *c = &plan.checks.data[i];
            char *desc = pv_describe(c);
            printf("  %-9s %s", pv_status_name(c->status), desc);
            if (c->status == FV_FAIL || c->status == FV_REACHED)
                printf(" at step %u", (unsigned)c->step);
            if (c->status == FV_SKIPPED && c->note)
                printf(" -- %s", c->note);
            printf("\n");
            free(desc);
            if (c->status == FV_FAIL && !first_fail && c->trace)
                first_fail = c;
        }
        if (first_fail)
            pv_write_test(dr, d, m, &plan, &fo, first_fail, dir, root, cwd, print_test);
        if (!keep && !any_fail && !any_error)
            for (i = 0; i < TERN_ARRAY_LEN(tasks); i++) {
                StrBuf wd = {0};
                char *p;
                sb_printf(&wd, "%s_%s", plan.name, tasks[i]);
                p = os_path_join(dir, wd.data);
                if (os_is_dir(p))
                    os_remove_all(p);
                free(p);
                sb_free(&wd);
            }
        free(logp);
        sb_free(&sbyfile);
        os_run_result_free(&res);
        if (any_fail || any_error)
            rc = TERN_EXIT_FAIL;
    }
done:
    free(sby);
    free(cwd);
    free(root);
    free(dir);
    free(dir_disp);
    fv_plan_free(&plan);
    driver_free(dr);
    arena_release(&ira);
    return rc;
}
