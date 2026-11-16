/* build.c — `tern build` (spec §13.2): driver -> elaborate the top(s) ->
 * check_design -> emit_design -> build/<target>/ (or stdout), plus the
 * target resolution shared by `tern synth` and `tern prove`.
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

/* ============================================================== targets */

static int bk_istarts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tern_to_lower((unsigned char)*s) != tern_to_lower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int bk_icontains(const char *s, const char *needle)
{
    for (; *s; s++)
        if (bk_istarts(s, needle))
            return 1;
    return 0;
}

BkFamily bk_family_from_device(const char *dev)
{
    if (!dev || !*dev)
        return BK_FAM_GENERIC;
    if (bk_istarts(dev, "xc") || bk_istarts(dev, "xa7") || bk_istarts(dev, "xq"))
        return BK_FAM_XILINX;
    if (bk_istarts(dev, "ice40") || bk_istarts(dev, "ice5") || bk_istarts(dev, "hx") || bk_istarts(dev, "lp") ||
        bk_istarts(dev, "up5k") || bk_istarts(dev, "up3k") || bk_istarts(dev, "u4k") || bk_istarts(dev, "ice"))
        return BK_FAM_ICE40;
    if (bk_istarts(dev, "lfe5") || bk_istarts(dev, "ecp5") || bk_istarts(dev, "25k") || bk_istarts(dev, "45k") ||
        bk_istarts(dev, "85k") || bk_istarts(dev, "um5g") || bk_istarts(dev, "um-"))
        return BK_FAM_ECP5;
    if (bk_istarts(dev, "gw1n") || bk_istarts(dev, "gw2a") || bk_istarts(dev, "gw5a"))
        return BK_FAM_GOWIN;
    if (bk_istarts(dev, "5c") || bk_istarts(dev, "5s") || bk_istarts(dev, "5a") || bk_istarts(dev, "10m") ||
        bk_istarts(dev, "10c") || bk_istarts(dev, "10a") || bk_istarts(dev, "ep4") || bk_istarts(dev, "ep3") ||
        bk_istarts(dev, "1s") || bk_istarts(dev, "agf") || bk_istarts(dev, "ag") || bk_istarts(dev, "a5e"))
        return BK_FAM_INTEL;
    if (bk_icontains(dev, "sky130") || bk_icontains(dev, "gf180") || bk_icontains(dev, "ihp") ||
        bk_icontains(dev, "asic"))
        return BK_FAM_ASIC;
    return BK_FAM_GENERIC;
}

const char *bk_family_name(BkFamily f)
{
    switch (f) {
    case BK_FAM_XILINX: return "xilinx";
    case BK_FAM_INTEL: return "intel";
    case BK_FAM_ICE40: return "ice40";
    case BK_FAM_ECP5: return "ecp5";
    case BK_FAM_GOWIN: return "gowin";
    case BK_FAM_ASIC: return "asic";
    case BK_FAM_GENERIC: break;
    }
    return "generic";
}

const char *bk_tool_name(BkTool t)
{
    switch (t) {
    case BK_TOOL_YOSYS: return "yosys";
    case BK_TOOL_VIVADO: return "vivado";
    case BK_TOOL_QUARTUS: return "quartus";
    case BK_TOOL_NONE: break;
    }
    return "none";
}

int bk_tool_parse(const char *s, BkTool *out)
{
    if (tern_strcasecmp(s, "yosys") == 0)
        *out = BK_TOOL_YOSYS;
    else if (tern_strcasecmp(s, "vivado") == 0)
        *out = BK_TOOL_VIVADO;
    else if (tern_strcasecmp(s, "quartus") == 0)
        *out = BK_TOOL_QUARTUS;
    else
        return -1;
    return 0;
}

BkTool bk_family_default_tool(BkFamily f)
{
    if (f == BK_FAM_XILINX)
        return BK_TOOL_VIVADO;
    if (f == BK_FAM_INTEL)
        return BK_TOOL_QUARTUS;
    return BK_TOOL_YOSYS;
}

static const TomlValue *bk_extra(const TernTarget *tt, const char *key)
{
    size_t i;
    for (i = 0; tt && i < tt->nextra; i++)
        if (strcmp(tt->extra[i].key, key) == 0)
            return tt->extra[i].value;
    return NULL;
}

/* a string or an array of strings, as "a,b,c" */
static const char *bk_list_value(const TomlValue *v, const char *key, const char *where, Arena *a, int *errs)
{
    StrBuf sb = {0};
    const TomlValue *it;
    const char *r;
    if (!v)
        return NULL;
    if (v->kind == TOML_STRING)
        return arena_strdup(a, v->str);
    if (v->kind != TOML_ARRAY) {
        diag_error(v->span, "`%s` in [%s] must be a string or an array of strings", key, where);
        (*errs)++;
        return NULL;
    }
    for (it = v->first; it; it = it->next) {
        if (it->kind != TOML_STRING) {
            diag_error(it->span, "`%s` in [%s] must contain strings", key, where);
            (*errs)++;
            continue;
        }
        if (sb.len)
            sb_putc(&sb, ',');
        sb_puts(&sb, it->str);
    }
    r = arena_strdup(a, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static const char *bk_str_value(const TomlValue *v, const char *key, const char *where, Arena *a, int *errs)
{
    if (!v)
        return NULL;
    if (v->kind != TOML_STRING) {
        diag_error(v->span, "`%s` in [%s] must be a string", key, where);
        (*errs)++;
        return NULL;
    }
    return arena_strdup(a, v->str);
}

/* "xdc,sdc" -> bits; returns -1 on an unknown word (reported at span) */
static int bk_parse_bits(const char *list, const char *const *names, const unsigned *bits, size_t n, unsigned *out,
                         Span span, const char *what)
{
    const char *p = list;
    *out = 0;
    while (*p) {
        const char *e = p;
        size_t i, len;
        int ok = 0;
        while (*e && *e != ',' && *e != ' ')
            e++;
        len = (size_t)(e - p);
        for (i = 0; i < n; i++)
            if (len && strlen(names[i]) == len && strncmp(names[i], p, len) == 0) {
                *out |= bits[i];
                ok = 1;
            }
        if (len && !ok) {
            diag_error(span, "unknown %s `%.*s`", what, (int)len, p);
            return -1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

int bk_target_resolve(const TernTarget *tt, const char *name, BkTarget *t, Arena *a)
{
    static const char *const cons_names[] = {"none", "xdc", "sdc"};
    static const unsigned cons_bits[] = {0, EMIT_CONS_XDC, EMIT_CONS_SDC};
    static const char *const mem_names[] = {"hex", "mif", "coe"};
    static const unsigned mem_bits[] = {0, SV_MEMINIT_MIF, SV_MEMINIT_COE};
    const char *where, *s;
    const TomlValue *v;
    int errs = 0;
    memset(t, 0, sizeof *t);
    t->name = arena_strdup(a, tt ? tt->name : (name ? name : "default"));
    t->std = SV_STD_SV2012;
    t->kind = SV_TARGET_FPGA;
    t->constraints = EMIT_CONS_XDC | EMIT_CONS_SDC;
    if (!tt)
        return 0;
    t->from_manifest = 1;
    where = arena_printf(a, "target.%s", tt->name);
    t->device = tt->device ? arena_strdup(a, tt->device) : NULL;
    t->top = tt->top ? arena_strdup(a, tt->top) : NULL;
    if (tt->std && strcmp(tt->std, "verilog2005") == 0)
        t->std = SV_STD_V2005;
    t->family = bk_family_from_device(t->device);
    s = bk_str_value(bk_extra(tt, "vendor"), "vendor", where, a, &errs);
    if (s) {
        if (tern_strcasecmp(s, "xilinx") == 0 || tern_strcasecmp(s, "amd") == 0)
            t->family = BK_FAM_XILINX;
        else if (tern_strcasecmp(s, "intel") == 0 || tern_strcasecmp(s, "altera") == 0)
            t->family = BK_FAM_INTEL;
        else if (tern_strcasecmp(s, "ice40") == 0 ||
                 (tern_strcasecmp(s, "lattice") == 0 && bk_family_from_device(t->device) != BK_FAM_ECP5))
            t->family = BK_FAM_ICE40;
        else if (tern_strcasecmp(s, "ecp5") == 0 || tern_strcasecmp(s, "lattice") == 0)
            t->family = BK_FAM_ECP5;
        else if (tern_strcasecmp(s, "gowin") == 0)
            t->family = BK_FAM_GOWIN;
        else if (tern_strcasecmp(s, "asic") == 0)
            t->family = BK_FAM_ASIC;
        else if (tern_strcasecmp(s, "generic") == 0)
            t->family = BK_FAM_GENERIC;
        else {
            diag_error(bk_extra(tt, "vendor")->span, "unknown vendor \"%s\" in [%s]", s, where);
            diag_help("use one of xilinx, intel, lattice, ice40, ecp5, gowin, asic, generic");
            errs++;
        }
    }
    if (t->family == BK_FAM_ASIC)
        t->kind = SV_TARGET_ASIC;
    s = bk_str_value((v = bk_extra(tt, "kind")) ? v : bk_extra(tt, "target_kind"), "kind", where, a, &errs);
    if (s) {
        if (strcmp(s, "fpga") == 0)
            t->kind = SV_TARGET_FPGA;
        else if (strcmp(s, "asic") == 0)
            t->kind = SV_TARGET_ASIC;
        else {
            diag_error(v->span, "invalid kind \"%s\" in [%s] (fpga or asic)", s, where);
            errs++;
        }
    }
    /* defaults by family */
    switch (t->family) {
    case BK_FAM_XILINX: t->constraints = EMIT_CONS_XDC; break;
    case BK_FAM_INTEL:
        t->constraints = EMIT_CONS_SDC;
        t->mem_init = SV_MEMINIT_MIF;
        break;
    case BK_FAM_ICE40:
    case BK_FAM_ECP5:
    case BK_FAM_GOWIN:
    case BK_FAM_ASIC: t->constraints = EMIT_CONS_SDC; break;
    case BK_FAM_GENERIC: break;
    }
    v = bk_extra(tt, "constraints");
    s = bk_list_value(v, "constraints", where, a, &errs);
    if (s && bk_parse_bits(s, cons_names, cons_bits, 3, &t->constraints, v->span, "constraint format") != 0)
        errs++;
    v = bk_extra(tt, "mem_init");
    s = bk_list_value(v, "mem_init", where, a, &errs);
    if (s && bk_parse_bits(s, mem_names, mem_bits, 3, &t->mem_init, v->span, "memory-init format") != 0)
        errs++;
    t->features = bk_list_value(bk_extra(tt, "features"), "features", where, a, &errs);
    t->package = bk_str_value(bk_extra(tt, "package"), "package", where, a, &errs);
    t->part = bk_str_value(bk_extra(tt, "part"), "part", where, a, &errs);
    t->family_name = bk_str_value(bk_extra(tt, "family"), "family", where, a, &errs);
    v = bk_extra(tt, "clock_hz");
    if (v) {
        if (v->kind != TOML_INT || v->num <= 0) {
            diag_error(v->span, "`clock_hz` in [%s] must be a positive integer", where);
            errs++;
        } else {
            t->clock_hz = (uint64_t)v->num;
        }
    }
    if (tt->backend && bk_tool_parse(tt->backend, &t->tool) != 0) {
        diag_error(tt->span, "unknown backend \"%s\" in [%s]", tt->backend, where);
        diag_help("the synthesis tool: yosys, vivado or quartus");
        errs++;
    }
    return errs;
}

/* ================================================================ build */

void bk_build_options_init(BkBuildOptions *o)
{
    memset(o, 0, sizeof *o);
    o->cmd = "build";
}

void bk_rewrite_paths(SvOutput *out, const char *const *from, const char *const *to, size_t n)
{
    size_t f, k;
    for (f = 0; f < out->nfiles; f++) {
        SvFile *file = &out->files[f];
        for (k = 0; k < n; k++) {
            StrBuf sb = {0};
            size_t flen;
            const char *p, *hit;
            if (!from[k] || !*from[k])
                continue;
            flen = strlen(from[k]);
            p = file->data;
            if (!strstr(p, from[k]))
                continue;
            while ((hit = strstr(p, from[k])) != NULL) {
                if (hit[flen] != '/') {
                    sb_append(&sb, p, (size_t)(hit - p) + flen);
                    p = hit + flen;
                    continue;
                }
                sb_append(&sb, p, (size_t)(hit - p));
                sb_puts(&sb, to[k]);
                p = hit + flen + 1;
            }
            sb_puts(&sb, p);
            free(file->data);
            file->len = sb.len;
            file->data = sb_take(&sb);
        }
    }
}

int bk_is_extern_file(const SvOutput *out, const char *name)
{
    size_t i;
    for (i = 0; i < out->nextern; i++)
        if (strcmp(os_path_basename(out->extern_files[i]), name) == 0)
            return 1;
    return 0;
}

void bk_hdl_files(const SvOutput *out, BkPathVec *files)
{
    size_t i;
    /* the package first (the modules import it), then extern sources, then
     * the emitted modules children-first */
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_PACKAGE)
            vec_push(*files, out->files[i].name);
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_MODULE && bk_is_extern_file(out, out->files[i].name))
            vec_push(*files, out->files[i].name);
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_MODULE && !bk_is_extern_file(out, out->files[i].name))
            vec_push(*files, out->files[i].name);
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
static char *bk_find_extern(const char *path, const char *root, Span loc)
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
            src = bk_find_extern(m->extern_file, b->root, m->loc);
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
            else
                diag_error(span_none(), "cannot find module `%s`%s", name,
                           o->top ? "" : " (the `top` of the build target)");
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
            if (tops.len > 1)
                tops.len = 1; /* the first module that needs no generic arguments */
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
    size_t i, g, changed = 0;
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
    if (!summary)
        return 0;
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
    printf("  %u file%s, %u changed\n", (unsigned)b->out.nfiles, b->out.nfiles == 1 ? "" : "s", (unsigned)changed);
    return 0;
}

void bk_build_free(BkBuild *b)
{
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
