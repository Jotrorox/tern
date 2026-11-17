/* build.c — `tern build` (spec §13.2): driver -> elaborate the top(s) ->
 * check_design -> emit_design -> build/<target>/ (or stdout).  `tern synth`
 * runs the same pipeline (bk_build_run); targets are resolved in target.c.
 *
 *     tern build [--target <t>] [--top <m>] [--stdout] [-o <dir>]
 *                [--std sv|verilog2005] [--emit-assertions] [--features <l>]
 *                [--no-line-markers] [--lint] [-q] [<file.tern|dir>...]
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

/* ================================================================ build */

void bk_build_options_init(BkBuildOptions *o)
{
    memset(o, 0, sizeof *o);
    o->cmd = "build";
}

const char *bk_build_top_name(BkBuild *b)
{
    SvNames *n;
    const char *r;
    if (!b->ndesigns)
        return NULL;
    n = sv_names_new(b->designs[0]);
    r = arena_strdup(&b->arena, sv_names_module(n, b->designs[0]->top));
    sv_names_free(n);
    return r;
}

static void bk_merge(SvOutput *dst, SvOutput *src)
{
    size_t i;
    for (i = 0; i < src->nfiles; i++) {
        StrBuf sb = {0};
        sb_append(&sb, src->files[i].data, src->files[i].len);
        sv_output_add(dst, src->files[i].name, src->files[i].kind, &sb);
        sb_free(&sb);
    }
    for (i = 0; i < src->nextern; i++) {
        size_t k;
        int dup = 0;
        for (k = 0; k < dst->nextern; k++)
            if (strcmp(dst->extern_files[k], src->extern_files[i]) == 0)
                dup = 1;
        if (!dup) {
            dst->extern_files = (char **)xrealloc(dst->extern_files, (dst->nextern + 1) * sizeof(char *));
            dst->extern_files[dst->nextern++] = xstrdup(src->extern_files[i]);
        }
    }
}

/* the source of an extern module's file: absolute, relative to the project
 * root (or the current directory), or relative to the declaring file */
char *bk_find_extern_source(const char *path, const char *root, Span loc)
{
    char *p;
    const SourceFile *sf;
    if (os_path_is_abs(path))
        return os_is_file(path) ? xstrdup(path) : NULL;
    p = os_path_join(root, path);
    if (os_is_file(p))
        return p;
    free(p);
    sf = loc.file ? src_get(loc.file) : NULL;
    if (sf && sf->path) {
        char *dir = os_path_dirname(sf->path);
        p = os_path_join(dir, path);
        free(dir);
        if (os_is_file(p))
            return p;
        free(p);
    }
    return NULL;
}

/* copies the extern module sources into the output (as files) */
static int bk_add_externs(BkBuild *b)
{
    size_t di;
    uint32_t mi;
    int errs = 0;
    for (di = 0; di < b->ndesigns; di++) {
        const IrDesign *d = b->designs[di];
        for (mi = 0; mi < d->nmodules; mi++) {
            const IrModule *m = d->modules[mi];
            char *src, *text = NULL;
            size_t len = 0;
            StrBuf sb = {0};
            const char *base;
            if (!m->is_extern || !m->extern_file || !*m->extern_file)
                continue;
            base = os_path_basename(m->extern_file);
            if (sv_output_find(&b->out, base))
                continue;
            src = bk_find_extern_source(m->extern_file, b->root, m->loc);
            if (!src || os_read_file(src, &text, &len) != 0) {
                diag_error(m->loc, "cannot find the source of extern module `%s`: \"%s\"", m->name, m->extern_file);
                diag_help("extern paths are relative to the project root (or to the declaring file)");
                free(src);
                errs++;
                continue;
            }
            sb_append(&sb, text, len);
            sv_output_add(&b->out, base, SV_FILE_MODULE, &sb);
            sb_free(&sb);
            free(text);
            free(src);
        }
    }
    return errs;
}

int bk_build_run(const BkBuildOptions *o, BkBuild *b)
{
    DriverOptions dopt;
    Driver *dr;
    const TernTarget *tt = NULL;
    DriverItemVec tops = {0};
    CheckOptions co;
    size_t i;
    int errs = 0;
    char *cwd;
    const char *cmd = o->cmd ? o->cmd : "build";

    memset(b, 0, sizeof *b);
    memset(&dopt, 0, sizeof dopt);
    dopt.paths = o->paths;
    dopt.npaths = o->npaths;
    dopt.cmd = cmd;
    dr = driver_open(&dopt);
    if (!dr) {
        b->usage_error = 1;
        return -1;
    }
    b->dr = dr;
    cwd = os_getcwd();
    b->root = dr->has_project ? xstrdup(dr->prj.manifest.root) : xstrdup(cwd ? cwd : ".");

    /* target */
    if (dr->has_project) {
        tt = pkg_manifest_target(&dr->prj.manifest, o->target);
        if (o->target && !tt) {
            StrBuf names = {0};
            for (i = 0; i < dr->prj.manifest.ntargets; i++)
                sb_printf(&names, "%s%s", i ? ", " : "", dr->prj.manifest.targets[i].name);
            fprintf(stderr, "tern %s: error: no [target.%s] in tern.toml%s%s%s\n", cmd, o->target,
                    names.len ? " (targets: " : "", sb_cstr(&names), names.len ? ")" : "");
            sb_free(&names);
            b->usage_error = 1;
            free(cwd);
            return -1;
        }
        if (!o->target && !tt && dr->prj.manifest.ntargets > 1) {
            StrBuf names = {0};
            for (i = 0; i < dr->prj.manifest.ntargets; i++)
                sb_printf(&names, "%s%s", i ? ", " : "", dr->prj.manifest.targets[i].name);
            fprintf(stderr, "tern %s: error: several targets and none is named `default`; choose one with --target (%s)\n",
                    cmd, sb_cstr(&names));
            sb_free(&names);
            b->usage_error = 1;
            free(cwd);
            return -1;
        }
    } else if (o->target) {
        fprintf(stderr, "tern %s: error: --target %s needs a project (no tern.toml found)\n", cmd, o->target);
        b->usage_error = 1;
        free(cwd);
        return -1;
    }
    errs += bk_target_resolve(tt, o->target, &b->target, &b->arena);

    /* output directory */
    if (o->out_dir)
        b->out_dir = xstrdup(o->out_dir);
    else if (dr->has_project) {
        char *bd = os_path_join(b->root, "build");
        b->out_dir = os_path_join(bd, b->target.name);
        free(bd);
    } else {
        b->out_dir = xstrdup("build");
    }
    b->out_dir_display = cwd ? os_path_relative(b->out_dir, cwd) : xstrdup(b->out_dir);

    if (driver_parse_errors(dr) || errs) {
        free(cwd);
        return -1;
    }
    /* features: the target's, then the command line's */
    if (driver_apply_features(dr, b->target.features) != 0 || driver_apply_features(dr, o->features) != 0) {
        free(cwd);
        return -1;
    }

    /* tops */
    if (o->top || b->target.top) {
        const char *name = o->top ? o->top : b->target.top;
        FileUnit *f = NULL;
        Item *it = driver_find_item(dr, name, &f);
        DriverItem di;
        if (!it || it->kind != ITEM_MODULE) {
            if (it && it->kind == ITEM_EXTERN)
                diag_error(it->name.span, "`%s` is an extern module; it cannot be the top of a build", name);
            else {
                diag_error(span_none(), "cannot find module `%s`%s", name,
                           o->top ? "" : " (the `top` of the build target in tern.toml)");
                driver_suggest_module(dr, name);
            }
            free(cwd);
            return -1;
        }
        di.item = it;
        di.file = f;
        vec_push(tops, di);
    } else {
        driver_tops(dr, &tops);
        if (!tops.len) {
            driver_default_roots(dr, &tops);
            if (tops.len > 1) {
                tops.len = 1; /* the first module that needs no generic arguments */
                fprintf(stderr,
                        "tern build: note: no #[top] module; building `%s`, the first module without required "
                        "generics\n  (mark the top with #[top] or name it with --top)\n",
                        tops.data[0].item->name.name);
            }
        }
    }
    if (!tops.len) {
        diag_error(span_none(), "nothing to build: no #[top] module and no module without required generics");
        diag_help("mark the top module with #[top], or name it with --top");
        vec_free(tops);
        free(cwd);
        return -1;
    }
    b->designs = ANEWN(&b->arena, IrDesign *, tops.len);
    for (i = 0; i < tops.len; i++) {
        IrDesign *d = driver_elaborate_module(dr, tops.data[i].item, tops.data[i].file, NULL, &b->arena);
        if (d && d->top)
            b->designs[b->ndesigns++] = d;
    }
    vec_free(tops);
    if (diag_error_count()) {
        free(cwd);
        return -1;
    }

    /* checks */
    memset(&co, 0, sizeof co);
    for (i = 0; i < b->ndesigns; i++)
        check_design(b->designs[i], &co);
    if (diag_error_count()) {
        free(cwd);
        return -1;
    }

    /* emit */
    sv_options_init(&b->sv);
    b->sv.std = o->std_set ? o->std : b->target.std;
    b->sv.emit_assertions = o->emit_assertions;
    b->sv.target_kind = b->target.kind;
    b->sv.mem_init_formats = b->target.mem_init;
    b->sv.constraints = b->target.constraints;
    if (o->no_line_markers)
        b->sv.line_markers = 0;
    /* value generics -> SystemVerilog parameters where exact (spec 13.3) */
    if (!o->monomorphize)
        for (i = 0; i < b->ndesigns; i++)
            bk_params_prepare(dr, b->designs[i], &b->sv);
    for (i = 0; i < b->ndesigns; i++) {
        SvOutput one;
        memset(&one, 0, sizeof one);
        if (emit_design(b->designs[i], &b->sv, &one) != 0)
            errs++;
        bk_merge(&b->out, &one);
        sv_output_free(&one);
    }
    if (errs) {
        free(cwd);
        return -1;
    }
    {
        /* absolute source paths -> project-relative (std -> std/...) */
        const char *from[3], *to[3];
        size_t n = 0;
        if (dr->std_source && strcmp(dr->std_source, "embedded") != 0) {
            from[n] = dr->std_source;
            to[n++] = "std/";
        }
        from[n] = b->root;
        to[n++] = "";
        if (cwd && strcmp(cwd, b->root) != 0) {
            from[n] = cwd;
            to[n++] = "";
        }
        bk_rewrite_paths(&b->out, from, to, n);
    }
    free(cwd);
    if (bk_add_externs(b))
        return -1;
    return 0;
}

static const char *bk_kind_label(const SvOutput *out, const SvFile *f)
{
    switch (f->kind) {
    case SV_FILE_MODULE: return bk_is_extern_file(out, f->name) ? "extern" : "modules";
    case SV_FILE_PACKAGE: return "package";
    case SV_FILE_MEMINIT: return "memory init";
    case SV_FILE_XDC:
    case SV_FILE_SDC: return "constraints";
    case SV_FILE_C_HEADER:
    case SV_FILE_SVD:
    case SV_FILE_MARKDOWN: return "regmaps";
    }
    return "other";
}

int bk_build_write(BkBuild *b, int summary)
{
    static const char *const groups[] = {"package", "modules", "extern", "memory init", "constraints", "regmaps"};
    size_t i, g, changed = 0, nfiles = b->out.nfiles;
    StrBuf flname = {0};
    if (os_mkdir_p(b->out_dir) != 0) {
        diag_error(span_none(), "cannot create directory `%s`", b->out_dir);
        return -1;
    }
    for (i = 0; i < b->out.nfiles; i++) {
        const SvFile *f = &b->out.files[i];
        char *p = os_path_join(b->out_dir, f->name);
        int r = os_write_file_if_changed(p, f->data, f->len);
        if (r < 0) {
            diag_error(span_none(), "cannot write `%s`", p);
            free(p);
            return -1;
        }
        changed += r == 1;
        free(p);
    }
    if (b->ndesigns) {
        /* <top>.f: the HDL files in compile order (package first) for
         * simulators and synthesis tools, with paths relative to the
         * directory tern build ran in: `iverilog -g2012 -c build/default/Top.f` */
        StrBuf fl = {0};
        char *p;
        int r;
        for (g = 0; g < 2; g++)
            for (i = 0; i < b->out.nfiles; i++)
                if (b->out.files[i].kind == (g == 0 ? SV_FILE_PACKAGE : SV_FILE_MODULE))
                    sb_printf(&fl, "%s/%s\n", b->out_dir_display, b->out.files[i].name);
        sb_printf(&flname, "%s.f", bk_build_top_name(b));
        p = os_path_join(b->out_dir, flname.data);
        r = os_write_file_if_changed(p, fl.data ? fl.data : "", fl.len);
        sb_free(&fl);
        if (r < 0) {
            diag_error(span_none(), "cannot write `%s`", p);
            free(p);
            return -1;
        }
        free(p);
        changed += r == 1;
        nfiles++;
    }
    if (!summary) {
        sb_free(&flname);
        return 0;
    }
    printf("tern build: %s", b->ndesigns ? b->designs[0]->top->display : "?");
    for (i = 1; i < b->ndesigns; i++)
        printf(", %s", b->designs[i]->top->display);
    printf(" -> %s/ (target %s, %s)\n", b->out_dir_display, b->target.name,
           b->sv.std == SV_STD_V2005 ? "Verilog-2005" : "SystemVerilog");
    for (g = 0; g < TERN_ARRAY_LEN(groups); g++) {
        size_t col = 0;
        for (i = 0; i < b->out.nfiles; i++) {
            const char *name = b->out.files[i].name;
            if (strcmp(bk_kind_label(&b->out, &b->out.files[i]), groups[g]) != 0)
                continue;
            if (col == 0)
                col = (size_t)printf("  %-12s", groups[g]);
            else if (col + 1 + strlen(name) > 78)
                col = (size_t)printf("\n  %-12s", "") - 1;
            col += (size_t)printf(" %s", name);
        }
        if (col)
            printf("\n");
    }
    if (flname.len)
        printf("  %-12s %s\n", "file list", flname.data);
    printf("  %u file%s, %u changed\n", (unsigned)nfiles, nfiles == 1 ? "" : "s", (unsigned)changed);
    sb_free(&flname);
    return 0;
}

void bk_build_free(BkBuild *b)
{
    size_t i;
    for (i = 0; i < b->ndesigns; i++)
        sv_params_free(b->designs[i]);
    if (b->dr)
        driver_free(b->dr);
    sv_output_free(&b->out);
    arena_release(&b->arena);
    free(b->root);
    free(b->out_dir);
    free(b->out_dir_display);
    memset(b, 0, sizeof *b);
}

/* ============================================================ command */

int cmd_build(int argc, char **argv)
{
    ArgParser ap;
    BkBuildOptions o;
    BkBuild b;
    BkPathVec paths = {0};
    const char *val = NULL;
    int to_stdout = 0, quiet = 0, lint = 0, rc = TERN_EXIT_OK;

    bk_build_options_init(&o);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("build");
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "target|t", &val)) {
            o.target = val;
        } else if (args_opt(&ap, "top", &val)) {
            o.top = val;
        } else if (args_opt(&ap, "features", &val)) {
            o.features = val;
        } else if (args_opt(&ap, "output|o", &val)) {
            o.out_dir = val;
        } else if (args_flag(&ap, "stdout")) {
            to_stdout = 1;
        } else if (args_opt(&ap, "std", &val)) {
            o.std_set = 1;
            if (strcmp(val, "sv") == 0 || strcmp(val, "sv2012") == 0 || strcmp(val, "systemverilog") == 0)
                o.std = SV_STD_SV2012;
            else if (strcmp(val, "verilog2005") == 0 || strcmp(val, "v2005") == 0 || strcmp(val, "verilog") == 0)
                o.std = SV_STD_V2005;
            else
                args_error(&ap, "invalid --std `%s` (sv or verilog2005)", val);
        } else if (args_flag(&ap, "emit-assertions")) {
            o.emit_assertions = 1;
        } else if (args_flag(&ap, "no-line-markers")) {
            o.no_line_markers = 1;
        } else if (args_flag(&ap, "monomorphize")) {
            o.monomorphize = 1;
        } else if (args_flag(&ap, "lint")) {
            lint = 1;
        } else if (args_flag(&ap, "quiet|q")) {
            quiet = 1;
        } else if (args_positional(&ap)) {
            vec_push(paths, ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (!ap.errors && to_stdout && o.out_dir)
        args_error(&ap, "-o and --stdout are mutually exclusive");
    if (!ap.errors && to_stdout && lint)
        args_error(&ap, "--lint needs the files on disk (drop --stdout)");
    if (ap.errors) {
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    o.paths = paths.data;
    o.npaths = paths.len;
    if (bk_build_run(&o, &b) != 0) {
        rc = b.usage_error ? TERN_EXIT_USAGE : TERN_EXIT_FAIL;
    } else if (to_stdout) {
        StrBuf sb = {0};
        sv_output_concat(&b.out, &sb);
        fwrite(sb.data ? sb.data : "", 1, sb.len, stdout);
        sb_free(&sb);
    } else if (bk_build_write(&b, !quiet) != 0) {
        rc = TERN_EXIT_FAIL;
    } else if (lint) {
        int r = vl_lint(b.out_dir, &b.out, bk_build_top_name(&b), b.sv.std);
        if (r == 1)
            rc = TERN_EXIT_FAIL;
    }
    bk_build_free(&b);
    vec_free(paths);
    return rc;
}
