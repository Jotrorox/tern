/* driver.c — the shared front half (see driver.h). */
#include "driver.h"
#include "base/str.h"
#include "base/diag.h"
#include "os/os.h"
#include "syntax/parser.h"
#include "std_embed.h"

typedef VEC(char *) DrvPaths;

static void drv_collect(const char *path, DrvPaths *out, int top)
{
    OsDirList list;
    size_t i;
    if (!os_is_dir(path)) {
        if (top || strcmp(os_path_ext(path), ".tern") == 0)
            vec_push(*out, xstrdup(path));
        return;
    }
    if (os_list_dir(path, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        char *child;
        if (list.items[i].name[0] == '.')
            continue;
        child = os_path_join(path, list.items[i].name);
        if (list.items[i].is_dir || strcmp(os_path_ext(child), ".tern") == 0)
            drv_collect(child, out, 0);
        free(child);
    }
    os_dir_list_free(&list);
}

static void drv_free_paths(DrvPaths *p)
{
    size_t i;
    for (i = 0; i < p->len; i++)
        free(p->data[i]);
    vec_free(*p);
}

static AstFile *drv_parse(Driver *dr, const char *path, const char *text, size_t len)
{
    uint32_t id = src_find(path);
    Arena *a;
    AstFile *f;
    if (id) {
        const SourceFile *sf = src_get(id);
        if (sf->len != len || memcmp(sf->text, text, len) != 0)
            src_update(id, text, len);
    } else {
        id = src_add(path, text, len);
    }
    a = (Arena *)xcalloc(1, sizeof(Arena));
    vec_push(dr->arenas, a);
    f = parse_file(a, id);
    if (f->nerrors)
        dr->parse_errors = 1;
    return f;
}

static int drv_parse_file(Driver *dr, const char *path, int is_user, int is_std)
{
    char *text = NULL;
    size_t len = 0;
    AstFile *f;
    const char *over = dr->opt.overlay ? dr->opt.overlay(dr->opt.overlay_user, path, &len) : NULL;
    if (over) {
        f = drv_parse(dr, path, over, len);
    } else {
        if (os_read_file(path, &text, &len) != 0) {
            fprintf(stderr, "tern %s: error: cannot read %s\n", dr->opt.cmd ? dr->opt.cmd : "check", path);
            return 0;
        }
        f = drv_parse(dr, path, text, len);
        free(text);
    }
    if (f->nerrors && is_user)
        dr->user_parse_errors = 1;
    sema_add_file(dr->S, f, is_user, is_std);
    if (is_user)
        dr->nuser++;
    return 1;
}

/* a library std file is replaced by a user file at the same std-relative
 * path (`tern check std`, `tern check std/fifo/fifo.tern`) */
static int drv_pkg_overridden(Map *user_std, const char *rel)
{
    return map_has(user_std, MAP_PTR(intern_cstr(rel)));
}

static void drv_user_std_paths(Driver *dr, Map *out)
{
    size_t i;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        char *abs;
        const char *p, *hit = NULL;
        if (!f->is_user || !f->pkg->is_std || !f->pkg->path[0])
            continue;
        f->is_std = 1;
        abs = os_path_abs(f->ast->path);
        for (p = abs; (p = strstr(p, "std/")) != NULL; p++)
            if (p == abs || p[-1] == '/')
                hit = p + 4;
        if (hit)
            map_put(out, MAP_PTR(intern_cstr(hit)), 1);
        free(abs);
    }
}

static char *drv_std_dir(void)
{
    char *env = os_getenv("TERN_STD");
    char *exe, *dir, *cand;
    if (env && env[0] && os_is_dir(env))
        return env;
    free(env);
    exe = os_exe_path();
    if (!exe)
        return NULL;
    dir = os_path_dirname(exe);
    free(exe);
    cand = os_path_join(dir, "std");
    free(dir);
    if (os_is_dir(cand)) {
        char *pre = os_path_join(cand, "prelude");
        int ok = os_is_dir(pre);
        free(pre);
        if (ok)
            return cand;
    }
    free(cand);
    return NULL;
}

static void drv_load_std(Driver *dr, Map *user_std)
{
    char *dir = drv_std_dir();
    size_t i;
    if (dir) {
        DrvPaths files = {0};
        size_t dl = strlen(dir);
        drv_collect(dir, &files, 0);
        for (i = 0; i < files.len; i++) {
            const char *rel = files.data[i] + dl + (files.data[i][dl] == '/' ? 1 : 0);
            if (drv_pkg_overridden(user_std, rel))
                continue;
            drv_parse_file(dr, files.data[i], 0, 1);
        }
        drv_free_paths(&files);
        dr->std_source = intern_cstr(dir);
        free(dir);
        return;
    }
    for (i = 0; i < tern_std_file_count; i++) {
        const StdEmbedFile *ef = &tern_std_files[i];
        const char *rel = strncmp(ef->path, "std/", 4) == 0 ? ef->path + 4 : ef->path;
        AstFile *f;
        if (strcmp(os_path_ext(ef->path), ".tern") != 0)
            continue;
        if (drv_pkg_overridden(user_std, rel))
            continue;
        f = drv_parse(dr, ef->path, tern_std_text(i), ef->len);
        sema_add_file(dr->S, f, 0, 1);
    }
    dr->std_source = intern_cstr("embedded");
}

/* single-file mode: `--features a,-b` defines the features (cfg::a is
 * true, cfg::b false); later lists update them */
static void drv_loose_features(Driver *dr, const char *list)
{
    Sema *S = dr->S;
    const char *p = list;
    while (p && *p) {
        const char *e = p;
        size_t n, i;
        int on = 1;
        const char *name;
        while (*e && *e != ',' && *e != ' ')
            e++;
        n = (size_t)(e - p);
        if (n && (*p == '-' || *p == '!')) {
            on = 0;
            p++;
            n--;
        } else if (n > 3 && strncmp(p, "no-", 3) == 0) {
            on = 0;
            p += 3;
            n -= 3;
        }
        if (n) {
            name = intern(p, n);
            for (i = 0; i < S->nfeatures; i++)
                if (S->feature_names[i] == name || strcmp(S->feature_names[i], name) == 0)
                    break;
            if (i == S->nfeatures) {
                S->feature_names = (const char **)xrealloc((void *)S->feature_names, (i + 1) * sizeof(char *));
                S->feature_values = (int *)xrealloc(S->feature_values, (i + 1) * sizeof(int));
                S->feature_names[i] = name;
                S->nfeatures = i + 1;
            }
            S->feature_values[i] = on;
        }
        p = *e ? e + 1 : e;
    }
}

static int drv_path_in(const DrvPaths *p, const char *path)
{
    size_t i;
    char *a = os_path_abs(path);
    for (i = 0; i < p->len; i++) {
        char *b = os_path_abs(p->data[i]);
        int eq = strcmp(a, b) == 0;
        free(b);
        if (eq) {
            free(a);
            return 1;
        }
    }
    free(a);
    return 0;
}

Driver *driver_open(const DriverOptions *opt)
{
    Driver *dr = (Driver *)xcalloc(1, sizeof(Driver));
    DrvPaths user = {0}, lib = {0};
    Map user_std = {0};
    size_t i, first_lib;
    int r;
    char *cwd;
    dr->opt = *opt;
    dr->S = sema_new();
    cwd = opt->root ? NULL : os_getcwd();
    r = pkg_project_open(opt->root ? opt->root : cwd ? cwd : ".", &dr->prj);
    free(cwd);
    dr->has_project = r == 1;
    for (i = 0; i < opt->npaths; i++) {
        size_t olen;
        if (opt->overlay && opt->overlay(opt->overlay_user, opt->paths[i], &olen)) {
            vec_push(user, xstrdup(opt->paths[i]));
            continue;
        }
        if (!os_is_dir(opt->paths[i]) && !os_is_file(opt->paths[i])) {
            fprintf(stderr, "tern %s: error: no such file or directory: %s\n", opt->cmd ? opt->cmd : "check",
                    opt->paths[i]);
            drv_free_paths(&user);
            driver_free(dr);
            return NULL;
        }
        drv_collect(opt->paths[i], &user, 1);
    }
    if (dr->has_project) {
        char *src = os_path_join(dr->prj.manifest.root, "src");
        DrvPaths proj = {0};
        drv_collect(src, &proj, 0);
        free(src);
        if (opt->include_tests) {
            char *tests = os_path_join(dr->prj.manifest.root, "tests");
            if (os_is_dir(tests))
                drv_collect(tests, &proj, 0);
            free(tests);
        }
        for (i = 0; i < proj.len; i++) {
            if (opt->npaths == 0)
                vec_push(user, xstrdup(proj.data[i]));
            else if (!drv_path_in(&user, proj.data[i]))
                vec_push(lib, xstrdup(proj.data[i]));
        }
        drv_free_paths(&proj);
        for (i = 0; i < dr->prj.deps.len; i++)
            if (dr->prj.deps.items[i].src && os_is_dir(dr->prj.deps.items[i].src))
                drv_collect(dr->prj.deps.items[i].src, &lib, 0);
        if (opt->features)
            pkg_features_apply(&dr->prj.manifest, opt->features);
        dr->S->has_manifest = 1;
        dr->S->nfeatures = dr->prj.manifest.nfeatures;
        dr->S->feature_names = (const char **)xcalloc(dr->S->nfeatures ? dr->S->nfeatures : 1, sizeof(char *));
        dr->S->feature_values = (int *)xcalloc(dr->S->nfeatures ? dr->S->nfeatures : 1, sizeof(int));
        for (i = 0; i < dr->S->nfeatures; i++) {
            dr->S->feature_names[i] = dr->prj.manifest.features[i].name;
            dr->S->feature_values[i] = dr->prj.manifest.features[i].enabled;
        }
    } else if (opt->features) {
        drv_loose_features(dr, opt->features);
    }
    if (r < 0)
        diag_flush(); /* the manifest errors first */
    if (user.len == 0) {
        if (r >= 0)
            fprintf(stderr, "tern %s: error: no input files%s\n", opt->cmd ? opt->cmd : "check",
                    dr->has_project ? " (src/ is empty)" : " (and no tern.toml found)");
        drv_free_paths(&user);
        drv_free_paths(&lib);
        driver_free(dr);
        return NULL;
    }
    for (i = 0; i < user.len; i++)
        drv_parse_file(dr, user.data[i], 1, 0);
    first_lib = dr->S->files.len;
    for (i = 0; i < lib.len; i++)
        drv_parse_file(dr, lib.data[i], 0, 0);
    TERN_UNUSED(first_lib);
    /* user files that are std files replace the library copy */
    drv_user_std_paths(dr, &user_std);
    drv_load_std(dr, &user_std);
    map_free(&user_std);
    drv_free_paths(&user);
    drv_free_paths(&lib);
    sema_resolve(dr->S);
    return dr;
}

void driver_free(Driver *dr)
{
    size_t i;
    if (!dr)
        return;
    if (dr->S) {
        free((void *)dr->S->feature_names);
        free(dr->S->feature_values);
        sema_free(dr->S);
    }
    for (i = 0; i < dr->arenas.len; i++) {
        arena_release(dr->arenas.data[i]);
        free(dr->arenas.data[i]);
    }
    vec_free(dr->arenas);
    if (dr->has_project)
        pkg_project_close(&dr->prj);
    free(dr);
}

Sema *driver_sema(Driver *dr)
{
    return dr->S;
}

int driver_parse_errors(Driver *dr)
{
    return dr->parse_errors;
}

Item *driver_find_item(Driver *dr, const char *path, FileUnit **file_out)
{
    const char *sep = NULL, *s;
    Symbol sym;
    size_t i;
    for (s = path; (s = strstr(s, "::")) != NULL; s += 2)
        sep = s;
    if (sep) {
        char *pk = xstrndup(path, (size_t)(sep - path));
        Package *p = sema_find_package(dr->S, pk);
        free(pk);
        if (p && sema_lookup_member(dr->S, NULL, p, intern_cstr(sep + 2), &sym, NULL) && sym.kind == SYM_ITEM) {
            if (file_out)
                *file_out = sym.file;
            return sym.item;
        }
        /* private items are fine for tools */
        if (p) {
            uint64_t v;
            if (map_get(&p->items, MAP_PTR(intern_cstr(sep + 2)), &v)) {
                Symbol *ps = (Symbol *)(uintptr_t)v;
                if (file_out)
                    *file_out = ps->file;
                return ps->item;
            }
        }
        return NULL;
    }
    /* a bare name: user files first */
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        uint64_t v;
        if (!f->is_user)
            continue;
        if (map_get(&f->pkg->items, MAP_PTR(intern_cstr(path)), &v)) {
            Symbol *ps = (Symbol *)(uintptr_t)v;
            if (file_out)
                *file_out = ps->file;
            return ps->item;
        }
    }
    return NULL;
}

void driver_tops(Driver *dr, DriverItemVec *out)
{
    size_t i;
    uint32_t k, a;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++) {
            Item *it = f->ast->items[k];
            if (it->kind != ITEM_MODULE)
                continue;
            for (a = 0; a < it->attrs.n; a++)
                if (it->attrs.items[a].name.name && strcmp(it->attrs.items[a].name.name, "top") == 0) {
                    DriverItem di;
                    di.item = it;
                    di.file = f;
                    vec_push(*out, di);
                    break;
                }
        }
    }
}

void driver_default_roots(Driver *dr, DriverItemVec *out)
{
    size_t i;
    uint32_t k, g;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++) {
            Item *it = f->ast->items[k];
            int ok = 1;
            DriverItem di;
            if (it->kind != ITEM_MODULE || !it->name.name)
                continue;
            for (g = 0; g < it->u.module.generics.n; g++)
                if (!it->u.module.generics.items[g].default_type && !it->u.module.generics.items[g].default_value)
                    ok = 0;
            if (!ok)
                continue;
            di.item = it;
            di.file = f;
            vec_push(*out, di);
        }
    }
}

void driver_suggest_module(Driver *dr, const char *name)
{
    size_t i;
    uint32_t k;
    const char *best = NULL;
    uint32_t bd = 99, cap = (uint32_t)(strlen(name) <= 3 ? 1 : strlen(name) / 3);
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++) {
            Item *it = f->ast->items[k];
            uint32_t d;
            if (it->kind != ITEM_MODULE || !it->name.name)
                continue;
            d = sema_edit_distance(name, it->name.name, cap);
            if (d <= cap && d < bd) {
                bd = d;
                best = it->name.name;
            }
        }
    }
    if (best)
        diag_help("did you mean `%s`?", best);
}

void driver_tests(Driver *dr, DriverItemVec *out)
{
    size_t i;
    uint32_t k;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++)
            if (f->ast->items[k]->kind == ITEM_TEST) {
                DriverItem di;
                di.item = f->ast->items[k];
                di.file = f;
                vec_push(*out, di);
            }
    }
}

size_t driver_targets(Driver *dr, const TernTarget **out)
{
    if (!dr->has_project) {
        *out = NULL;
        return 0;
    }
    *out = dr->prj.manifest.targets;
    return dr->prj.manifest.ntargets;
}

int driver_apply_features(Driver *dr, const char *list)
{
    size_t i;
    if (!list || !*list)
        return 0;
    if (!dr->has_project) {
        drv_loose_features(dr, list);
        return 0;
    }
    if (pkg_features_apply(&dr->prj.manifest, list) != 0)
        return -1;
    for (i = 0; i < dr->S->nfeatures && i < dr->prj.manifest.nfeatures; i++)
        dr->S->feature_values[i] = dr->prj.manifest.features[i].enabled;
    return 0;
}

Elab *driver_elab_new(Driver *dr, IrDesign *design)
{
    Elab *el = elab_new(dr->S, design);
    if (dr->opt.const_eval_limit)
        elab_set_step_limit(el, dr->opt.const_eval_limit);
    return el;
}

static void drv_add_regmaps(Driver *dr, Elab *el)
{
    size_t i;
    uint32_t k;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++)
            if (f->ast->items[k]->kind == ITEM_REGMAP)
                elab_add_regmap(el, f->ast->items[k], f);
    }
}

static void drv_check_items(Driver *dr, Elab *el)
{
    size_t i;
    for (i = 0; i < dr->S->files.len; i++) {
        FileUnit *f = dr->S->files.data[i];
        if (f->is_user)
            elab_check_file_items(el, f);
    }
}

void driver_check_items(Driver *dr)
{
    Elab *el = driver_elab_new(dr, NULL);
    drv_check_items(dr, el);
    elab_free(el);
}

static ModInfo *drv_elab_root(Elab *el, Item *item, FileUnit *file, const ElabArgs *args)
{
    ValModule *vm;
    ModInfo *mi;
    diag_push_path(item->name.name);
    vm = elab_bind_module(el, item, file, args, item->name.span);
    if (!vm) {
        diag_pop_path();
        return NULL;
    }
    diag_pop_path();
    diag_push_path(vm->display);
    mi = elab_module(el, vm, item->name.span);
    diag_pop_path();
    return mi;
}

IrDesign *driver_elaborate_module(Driver *dr, Item *item, FileUnit *file, const ElabArgs *args, Arena *ir_arena)
{
    IrDesign *d = ir_design_new(ir_arena);
    Elab *el = driver_elab_new(dr, d);
    ModInfo *mi;
    if (!file)
        file = sema_file_of_item(dr->S, item);
    mi = drv_elab_root(el, item, file, args);
    if (mi && mi->ir)
        ir_design_set_top(d, mi->ir);
    drv_add_regmaps(dr, el);
    if (diag_error_count() == 0)
        elab_ir_verify(d);
    elab_free(el);
    sema_info_syntactic(dr->S); /* hover/definition for code elaboration did not reach */
    return mi ? d : NULL;
}

IrDesign *driver_elaborate_top(Driver *dr, const char *name, Arena *ir_arena)
{
    Item *it = NULL;
    FileUnit *f = NULL;
    DriverItemVec v = {0};
    if (!name && dr->has_project) {
        const TernTarget *t = pkg_manifest_target(&dr->prj.manifest, NULL);
        if (t && t->top)
            name = t->top;
    }
    if (name) {
        it = driver_find_item(dr, name, &f);
        if (!it || (it->kind != ITEM_MODULE && it->kind != ITEM_EXTERN)) {
            diag_error(span_none(), "cannot find module `%s`", name);
            return NULL;
        }
    } else {
        driver_tops(dr, &v);
        if (!v.len)
            driver_default_roots(dr, &v);
        if (!v.len) {
            vec_free(v);
            diag_error(span_none(), "no module to elaborate");
            return NULL;
        }
        it = v.data[0].item;
        f = v.data[0].file;
        vec_free(v);
    }
    return driver_elaborate_module(dr, it, f, NULL, ir_arena);
}

FileUnit *driver_module_scope(Driver *dr, const char *spec)
{
    size_t n = 0, i;
    char *base;
    FileUnit *scope = NULL;
    while (spec[n] && spec[n] != '<' && spec[n] != ' ' && spec[n] != '(')
        n++;
    base = xstrndup(spec, n);
    driver_find_item(dr, base, &scope);
    free(base);
    for (i = 0; i < dr->S->files.len && !scope; i++)
        if (dr->S->files.data[i]->is_user)
            scope = dr->S->files.data[i];
    return scope;
}

IrDesign *driver_elaborate_spec(Driver *dr, const char *spec, Arena *ir_arena)
{
    Arena *a;
    Expr *e;
    FileUnit *scope;
    IrDesign *d;
    Elab *el;
    Env *env;
    ValModule *vm;
    ModInfo *mi = NULL;
    int errs = diag_error_count();
    if (!strchr(spec, '<'))
        return driver_elaborate_top(dr, spec, ir_arena);
    a = (Arena *)xcalloc(1, sizeof(Arena));
    vec_push(dr->arenas, a);
    {
        /* parsed as a call so that `<` opens generic arguments */
        char *text = arena_printf(a, "%s()", spec);
        e = parse_expr_string(a, text, strlen(text), "<command line>", NULL);
    }
    if (diag_error_count() != errs)
        return NULL;
    scope = driver_module_scope(dr, spec);
    if (!scope) {
        diag_error(span_none(), "cannot find module `%s`", spec);
        return NULL;
    }
    d = ir_design_new(ir_arena);
    el = driver_elab_new(dr, d);
    env = elab_env_new(el, NULL, scope);
    vm = elab_eval_module(el, e, env);
    if (vm) {
        diag_push_path(vm->display);
        mi = elab_module(el, vm, e->span);
        diag_pop_path();
    }
    if (mi && mi->ir)
        ir_design_set_top(d, mi->ir);
    drv_add_regmaps(dr, el);
    if (diag_error_count() == errs)
        elab_ir_verify(d);
    elab_env_free(env);
    elab_free(el);
    return mi && mi->ir && diag_error_count() == errs ? d : NULL;
}

/* ------------------------------------------------ checking test bodies --
 *
 * `tern check` runs the front half on the tests too (spec §13.1), without
 * simulating: every `sim M<..>(..)` target is elaborated (with the test's
 * generic arguments), names resolve through the test's lexical scopes, the
 * file scope and the builtins, and the signal paths a test reads, writes
 * or steps (`dut.push.valid`, `dut.rx_fifo.count`, `dut.sys.step()`) exist
 * on the module bound to the handle (a `let` of a `sim` expression). What
 * depends on simulated values (types of reads, widths of writes) is left
 * to `tern test`. */

typedef struct DrvTcEnt {
    const char *name;
    ModInfo *mi;              /* a simulation handle, or NULL */
} DrvTcEnt;

typedef struct DrvTc {
    Driver *dr;
    Elab *el;
    Item *test;
    FileUnit *file;
    VEC(DrvTcEnt) scope;
    Map paths;                /* IrModule* -> Map* of signal paths (1) and prefixes (2) */
    VEC(Map *) path_maps;
} DrvTc;

static void drv_tc_expr(DrvTc *t, Expr *e);
static void drv_tc_block(DrvTc *t, Block *b);
static void drv_tc_stmt(DrvTc *t, Stmt *s);
static void drv_tc_type(DrvTc *t, Type *ty);

static const DrvTcEnt *drv_tc_find(DrvTc *t, const char *name)
{
    size_t i = t->scope.len;
    while (i-- > 0)
        if (t->scope.data[i].name == name)
            return &t->scope.data[i];
    return NULL;
}

static void drv_tc_declare(DrvTc *t, const char *name, ModInfo *mi)
{
    DrvTcEnt e;
    if (!name)
        return;
    e.name = name;
    e.mi = mi;
    vec_push(t->scope, e);
}

static int drv_tc_builtin_call(const char *n)
{
    static const char *const names[] = {"bits",    "const_log", "fell", "include_bin", "include_hex",   "log",
                                        "past",    "rose",      "stable", "static_assert", "unsafe_cross",
                                        "rand",    "trunc",     NULL};
    size_t i;
    for (i = 0; names[i]; i++)
        if (strcmp(n, names[i]) == 0)
            return 1;
    return 0;
}

static int drv_tc_builtin_name(const char *n)
{
    static const char *const names[] = {"bool", "uint", "sint", "bit", "clock", "reset",
                                        "tri",  "string", "cfg", "self", NULL};
    size_t i;
    for (i = 0; names[i]; i++)
        if (strcmp(n, names[i]) == 0)
            return 1;
    return 0;
}

static void drv_tc_gargs(DrvTc *t, PathSeg *s)
{
    uint32_t i;
    for (i = 0; i < s->nargs; i++) {
        if (s->args[i].type)
            drv_tc_type(t, s->args[i].type);
        else
            drv_tc_expr(t, s->args[i].expr);
    }
}

/* a path's first segment resolves; `call`: it is called */
static void drv_tc_path(DrvTc *t, Path *p, int call)
{
    Symbol sym;
    uint32_t i;
    const char *n0 = p->nsegs ? p->segs[0].name.name : NULL;
    for (i = 0; i < p->nsegs; i++)
        drv_tc_gargs(t, &p->segs[i]);
    if (!n0 || drv_tc_find(t, n0) || sema_lookup(t->dr->S, t->file, n0, &sym) || drv_tc_builtin_name(n0))
        return;
    if (call && p->nsegs == 1 && drv_tc_builtin_call(n0))
        return;
    if ((n0[0] == 'u' || n0[0] == 's') && tern_is_digit((unsigned char)n0[1]))
        return; /* uint/sint aliases: the evaluator explains them */
    {
        const char *names[256];
        size_t n = 0, k = t->scope.len;
        while (k-- > 0 && n < TERN_ARRAY_LEN(names))
            names[n++] = t->scope.data[k].name;
        diag_error(p->segs[0].name.span, "cannot find `%s` in this scope", n0);
        sema_suggest(t->dr->S, t->file, n0, names, n);
    }
}

static void drv_tc_type(DrvTc *t, Type *ty)
{
    uint32_t i;
    if (!ty)
        return;
    switch (ty->kind) {
    case TYPE_PATH: drv_tc_path(t, &ty->u.path, 0); break;
    case TYPE_ARRAY: drv_tc_type(t, ty->u.array.elem); drv_tc_expr(t, ty->u.array.len); break;
    case TYPE_STRUCT:
        for (i = 0; i < ty->u.anon_struct.nfields; i++)
            drv_tc_type(t, ty->u.anon_struct.fields[i]->type);
        break;
    case TYPE_TYPEOF: drv_tc_expr(t, ty->u.typeof_expr); break;
    case TYPE_TUPLE:
        for (i = 0; i < ty->u.tuple.n; i++)
            drv_tc_type(t, ty->u.tuple.elems[i]);
        break;
    case TYPE_ERROR: break;
    }
}

static void drv_tc_pattern(DrvTc *t, Pattern *p)
{
    uint32_t i;
    Symbol sym;
    if (!p)
        return;
    switch (p->kind) {
    case PAT_BIND:
        /* a name that resolves to a const is a use; otherwise it binds */
        if (!(p->u.bind.name && !drv_tc_find(t, p->u.bind.name) && sema_lookup(t->dr->S, t->file, p->u.bind.name, &sym)))
            drv_tc_declare(t, p->u.bind.name, NULL);
        break;
    case PAT_LIT: drv_tc_expr(t, p->u.lit); break;
    case PAT_RANGE: drv_tc_expr(t, p->u.range.lo); drv_tc_expr(t, p->u.range.hi); break;
    case PAT_VARIANT:
        for (i = 0; i < p->u.variant.nfields; i++) {
            FieldPat *fp = &p->u.variant.fields[i];
            if (fp->pat)
                drv_tc_pattern(t, fp->pat);
            else
                drv_tc_declare(t, fp->name.name, NULL);
        }
        break;
    case PAT_PATH: drv_tc_path(t, &p->u.path, 0); break;
    case PAT_STRUCT:
        drv_tc_path(t, &p->u.struct_pat.path, 0);
        for (i = 0; i < p->u.struct_pat.nfields; i++) {
            FieldPat *fp = &p->u.struct_pat.fields[i];
            if (fp->pat)
                drv_tc_pattern(t, fp->pat);
            else
                drv_tc_declare(t, fp->name.name, NULL);
        }
        break;
    case PAT_TUPLE:
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            drv_tc_pattern(t, p->u.list.items[i]);
        break;
    case PAT_WILDCARD: case PAT_ERROR: break;
    }
}

/* the signal paths of a module's simulation (hierarchical, Tern and SV
 * names, plus every prefix: bundles, instances) and its domains */
static void drv_tc_add_path(Map *m, const char *p, int full)
{
    size_t n = strlen(p), i;
    uint64_t *slot;
    int ins;
    slot = map_ref(m, hash_bytes(p, n), &ins);
    if (ins || full)
        *slot = full ? 1 : 2;
    for (i = 1; i < n; i++)
        if (p[i] == '.' || p[i] == '[') {
            slot = map_ref(m, hash_bytes(p, i), &ins);
            if (ins)
                *slot = 2;
        }
}

static void drv_tc_add_module(Map *m, const IrModule *mod, const char *prefix, uint32_t depth)
{
    uint32_t i;
    StrBuf sb = {0};
    for (i = 0; i < mod->nsigs; i++) {
        const IrSignal *s = &mod->sigs[i];
        sb_reset(&sb);
        sb_printf(&sb, "%s%s", prefix, s->path ? s->path : s->name);
        drv_tc_add_path(m, sb_cstr(&sb), 1);
        sb_reset(&sb);
        sb_printf(&sb, "%s%s", prefix, s->name);
        drv_tc_add_path(m, sb_cstr(&sb), 1);
    }
    if (depth < 64)
        for (i = 0; i < mod->ninsts; i++) {
            sb_reset(&sb);
            sb_printf(&sb, "%s%s.", prefix, mod->insts[i].name);
            drv_tc_add_module(m, mod->insts[i].module, sb_cstr(&sb), depth + 1);
        }
    sb_free(&sb);
}

static Map *drv_tc_paths(DrvTc *t, const IrModule *mod)
{
    uint64_t v;
    Map *m;
    uint32_t i;
    if (map_get(&t->paths, MAP_PTR(mod), &v))
        return (Map *)(uintptr_t)v;
    m = (Map *)xcalloc(1, sizeof(Map));
    drv_tc_add_module(m, mod, "", 0);
    for (i = 0; i < mod->ndomains; i++)
        if (mod->domains[i].name && mod->domains[i].name[0] && !map_has(m, hash_bytes(mod->domains[i].name, strlen(mod->domains[i].name))))
            map_put(m, hash_bytes(mod->domains[i].name, strlen(mod->domains[i].name)), 3);
    map_put(&t->paths, MAP_PTR(mod), MAP_PTR(m));
    vec_push(t->path_maps, m);
    return m;
}

/* closest signal path for "did you mean" (top-level paths only) */
static void drv_tc_suggest(const IrModule *mod, const char *prefix, const char *path)
{
    uint32_t i, bd = 3;
    const char *best = NULL;
    size_t pl = strlen(prefix);
    if (pl)
        return;
    for (i = 0; i < mod->nsigs; i++) {
        const char *p = mod->sigs[i].path ? mod->sigs[i].path : mod->sigs[i].name;
        uint32_t d = sema_edit_distance(p, path, bd);
        if (d < bd) {
            bd = d;
            best = p;
        }
    }
    if (best)
        diag_help("did you mean `%s`?", best);
}

/* `h.a.b` (method: the last field is called): the path exists on h's module */
static void drv_tc_access(DrvTc *t, Expr *e, int method)
{
    Expr *chain[64];
    uint32_t n = 0, i;
    Expr *b = e;
    const DrvTcEnt *ent;
    ModInfo *mi;
    Map *paths;
    StrBuf p = {0};
    while (b->kind == EXPR_FIELD && n < TERN_ARRAY_LEN(chain)) {
        chain[n++] = b;
        b = b->u.field.base;
    }
    if (b->kind != EXPR_PATH || b->u.path.nsegs != 1 || !(ent = drv_tc_find(t, b->u.path.segs[0].name.name)) ||
        !ent->mi || !ent->mi->ir)
        return;
    mi = ent->mi;
    paths = drv_tc_paths(t, mi->ir);
    for (i = n; i-- > 0;) {
        Expr *f = chain[i];
        const char *name = f->u.field.name.name;
        uint64_t v = 0;
        int is_method = method && i == 0;
        if (!name)
            break;
        if (is_method) {
            static const char *const root[] = {"reset", "step", "step_until", "settle", "run", "dump", "cycle",
                                               "time_ns", NULL};
            uint32_t k;
            if (p.len)
                break; /* methods of domains, bundles, memories, values */
            for (k = 0; root[k]; k++)
                if (strcmp(root[k], name) == 0)
                    break;
            if (!root[k]) {
                diag_error(f->u.field.name.span, "`%s` has no method `%s`", mi->display, name);
                diag_help("a simulation handle has reset(), step(n), step_until(cond, max: n), settle(), "
                          "run(ns: n), dump(file), cycle() and time_ns()");
            }
            break;
        }
        if (p.len)
            sb_putc(&p, '.');
        sb_puts(&p, name);
        if (!map_get(paths, hash_bytes(p.data, p.len), &v)) {
            diag_error(f->u.field.name.span, "`%s` has no signal `%s`", mi->display, p.data);
            drv_tc_suggest(mi->ir, "", p.data);
            break;
        }
        if (v == 1 || v == 3)
            break; /* a signal (its fields are values) or a domain */
    }
    sb_free(&p);
}

/* does the expression mention a local of the test (generic arguments of a
 * `sim` target computed by the test cannot be evaluated here)? */
static int drv_tc_uses_locals_type(DrvTc *t, Type *ty);

static int drv_tc_uses_locals(DrvTc *t, Expr *e)
{
    uint32_t i, k;
    if (!e)
        return 0;
    switch (e->kind) {
    case EXPR_PATH:
        if (e->u.path.nsegs && e->u.path.segs[0].name.name && drv_tc_find(t, e->u.path.segs[0].name.name))
            return 1;
        for (i = 0; i < e->u.path.nsegs; i++)
            for (k = 0; k < e->u.path.segs[i].nargs; k++)
                if (drv_tc_uses_locals(t, e->u.path.segs[i].args[k].expr) ||
                    drv_tc_uses_locals_type(t, e->u.path.segs[i].args[k].type))
                    return 1;
        return 0;
    case EXPR_PAREN: return drv_tc_uses_locals(t, e->u.inner);
    case EXPR_UNARY: return drv_tc_uses_locals(t, e->u.unary.operand);
    case EXPR_BINARY: return drv_tc_uses_locals(t, e->u.binary.lhs) || drv_tc_uses_locals(t, e->u.binary.rhs);
    case EXPR_CAST: return drv_tc_uses_locals(t, e->u.cast.expr);
    case EXPR_FIELD: return drv_tc_uses_locals(t, e->u.field.base);
    case EXPR_INDEX: return drv_tc_uses_locals(t, e->u.index.base) || drv_tc_uses_locals(t, e->u.index.index);
    case EXPR_CALL:
        if (drv_tc_uses_locals(t, e->u.call.callee))
            return 1;
        for (i = 0; i < e->u.call.nargs; i++)
            if (drv_tc_uses_locals(t, e->u.call.args[i].value))
                return 1;
        return 0;
    case EXPR_TYPE: return drv_tc_uses_locals_type(t, e->u.type);
    case EXPR_INT: case EXPR_BOOL: case EXPR_STRING: case EXPR_FILL: return 0;
    default: return 1; /* anything else: assume it may */
    }
}

static int drv_tc_uses_locals_type(DrvTc *t, Type *ty)
{
    uint32_t i, k;
    if (!ty)
        return 0;
    switch (ty->kind) {
    case TYPE_PATH:
        if (ty->u.path.nsegs && ty->u.path.segs[0].name.name && drv_tc_find(t, ty->u.path.segs[0].name.name))
            return 1;
        for (i = 0; i < ty->u.path.nsegs; i++)
            for (k = 0; k < ty->u.path.segs[i].nargs; k++)
                if (drv_tc_uses_locals(t, ty->u.path.segs[i].args[k].expr) ||
                    drv_tc_uses_locals_type(t, ty->u.path.segs[i].args[k].type))
                    return 1;
        return 0;
    case TYPE_ARRAY: return drv_tc_uses_locals_type(t, ty->u.array.elem) || drv_tc_uses_locals(t, ty->u.array.len);
    case TYPE_ERROR: return 0;
    default: return 1;
    }
}

/* elaborates the module of a `sim` expression (quietly first, then with
 * diagnostics); the ModInfo, or NULL */
static ModInfo *drv_tc_sim(DrvTc *t, Expr *sim)
{
    Elab *el = t->el;
    Expr *call = sim->u.inner;
    Env *env;
    ValModule *vm;
    ModInfo *mi = NULL;
    uint32_t i;
    if (!call || call->kind != EXPR_CALL)
        return NULL;
    for (i = 0; i < call->u.call.nargs; i++)
        drv_tc_expr(t, call->u.call.args[i].value);
    env = elab_env_new(el, NULL, t->file);
    el->quiet = 1;
    el->quiet_errors = 0;
    vm = elab_eval_module(el, call, env);
    el->quiet = 0;
    if (vm && !el->quiet_errors) {
        char *frame = arena_printf(&el->arena, "test \"%s\" > %s", t->test->u.test.name ? t->test->u.test.name : "",
                                   vm->display);
        diag_push_path(frame);
        /* re-bind with diagnostics (where clauses) */
        vm = elab_eval_module(el, call, env);
        if (vm)
            mi = elab_module(el, vm, call->span);
        diag_pop_path();
    } else if (!drv_tc_uses_locals(t, call->u.call.callee)) {
        /* fails without the test's values: report why (an unknown module,
         * an unsatisfied `where`) */
        diag_push_path(arena_printf(&el->arena, "test \"%s\"", t->test->u.test.name ? t->test->u.test.name : ""));
        elab_eval_module(el, call, env);
        diag_pop_path();
    }
    el->quiet_errors = 0;
    el->limit_hit = 0;
    el->steps = 0;
    elab_env_free(env);
    return mi;
}

/* a field chain `x.a.b` (method: `x.a.f(..)` with e the callee): the root
 * expression, generic arguments, then the path on a handle */
static void drv_tc_fields(DrvTc *t, Expr *e, int method)
{
    Expr *b = e;
    uint32_t i;
    while (b->kind == EXPR_FIELD) {
        for (i = 0; i < b->u.field.nargs; i++) {
            if (b->u.field.args[i].type)
                drv_tc_type(t, b->u.field.args[i].type);
            else
                drv_tc_expr(t, b->u.field.args[i].expr);
        }
        b = b->u.field.base;
    }
    drv_tc_expr(t, b);
    drv_tc_access(t, e, method);
}

static Expr *drv_tc_strip(Expr *e)
{
    while (e && e->kind == EXPR_PAREN)
        e = e->u.inner;
    return e;
}

static void drv_tc_expr(DrvTc *t, Expr *e)
{
    uint32_t i;
    size_t mark;
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_INT: drv_tc_expr(t, e->u.int_lit.width); break;
    case EXPR_PATH: drv_tc_path(t, &e->u.path, 0); break;
    case EXPR_VARIANT:
        for (i = 0; i < e->u.variant.nfields; i++)
            drv_tc_expr(t, e->u.variant.fields[i].value);
        break;
    case EXPR_STRUCT:
        drv_tc_path(t, &e->u.struct_lit.path, 0);
        for (i = 0; i < e->u.struct_lit.nfields; i++) {
            FieldInit *fi = &e->u.struct_lit.fields[i];
            if (fi->value)
                drv_tc_expr(t, fi->value);
        }
        break;
    case EXPR_FIELD: drv_tc_fields(t, e, 0); break;
    case EXPR_INDEX: drv_tc_expr(t, e->u.index.base); drv_tc_expr(t, e->u.index.index); break;
    case EXPR_SLICE: drv_tc_expr(t, e->u.slice.base); drv_tc_expr(t, e->u.slice.hi); drv_tc_expr(t, e->u.slice.lo); break;
    case EXPR_CALL: {
        Expr *c = e->u.call.callee;
        if (c->kind == EXPR_PATH)
            drv_tc_path(t, &c->u.path, 1);
        else if (c->kind == EXPR_FIELD)
            drv_tc_fields(t, c, 1); /* a method: the receiver path, then the method */
        else
            drv_tc_expr(t, c);
        for (i = 0; i < e->u.call.nargs; i++)
            drv_tc_expr(t, e->u.call.args[i].value);
        drv_tc_expr(t, e->u.call.domain);
        break;
    }
    case EXPR_CAST: drv_tc_expr(t, e->u.cast.expr); drv_tc_type(t, e->u.cast.type); break;
    case EXPR_UNARY: drv_tc_expr(t, e->u.unary.operand); break;
    case EXPR_BINARY: drv_tc_expr(t, e->u.binary.lhs); drv_tc_expr(t, e->u.binary.rhs); break;
    case EXPR_RANGE: drv_tc_expr(t, e->u.range.lo); drv_tc_expr(t, e->u.range.hi); break;
    case EXPR_IF:
        drv_tc_expr(t, e->u.if_expr.cond);
        drv_tc_block(t, e->u.if_expr.then_block);
        drv_tc_expr(t, e->u.if_expr.else_expr);
        break;
    case EXPR_MATCH:
        drv_tc_expr(t, e->u.match.scrutinee);
        for (i = 0; i < e->u.match.narms; i++) {
            mark = t->scope.len;
            drv_tc_pattern(t, e->u.match.arms[i].pat);
            drv_tc_stmt(t, e->u.match.arms[i].body);
            t->scope.len = mark;
        }
        break;
    case EXPR_BLOCK: drv_tc_block(t, e->u.block); break;
    case EXPR_TUPLE:
    case EXPR_ARRAY:
        for (i = 0; i < e->u.list.n; i++)
            drv_tc_expr(t, e->u.list.items[i]);
        break;
    case EXPR_PAREN: drv_tc_expr(t, e->u.inner); break;
    case EXPR_SIM: drv_tc_sim(t, e); break;
    case EXPR_TYPE: drv_tc_type(t, e->u.type); break;
    case EXPR_TEMPORAL: drv_tc_expr(t, e->u.temporal.bound); drv_tc_expr(t, e->u.temporal.body); break;
    case EXPR_DELAY:
        drv_tc_expr(t, e->u.delay.lhs);
        drv_tc_expr(t, e->u.delay.lo);
        drv_tc_expr(t, e->u.delay.hi);
        drv_tc_expr(t, e->u.delay.rhs);
        break;
    case EXPR_QPATH:
        drv_tc_type(t, e->u.qpath.type);
        for (i = 0; i < e->u.qpath.path.nsegs; i++)
            drv_tc_gargs(t, &e->u.qpath.path.segs[i]);
        break;
    case EXPR_ERROR: case EXPR_BOOL: case EXPR_FILL: case EXPR_STRING: break;
    }
}

static void drv_tc_stmt(DrvTc *t, Stmt *s)
{
    size_t mark;
    if (!s)
        return;
    switch (s->kind) {
    case STMT_LET: {
        Expr *init = drv_tc_strip(s->u.let.init);
        ModInfo *mi = NULL;
        drv_tc_type(t, s->u.let.type);
        if (init && init->kind == EXPR_SIM)
            mi = drv_tc_sim(t, init);
        else if (init && init->kind == EXPR_PATH && init->u.path.nsegs == 1) {
            /* `let d2 = dut`: the same handle */
            const DrvTcEnt *src = drv_tc_find(t, init->u.path.segs[0].name.name);
            drv_tc_expr(t, init);
            mi = src ? src->mi : NULL;
        } else
            drv_tc_expr(t, init);
        if (s->u.let.pat && s->u.let.pat->kind == PAT_BIND)
            drv_tc_declare(t, s->u.let.pat->u.bind.name, mi);
        else
            drv_tc_pattern(t, s->u.let.pat);
        drv_tc_expr(t, s->u.let.constraint);
        break;
    }
    case STMT_VAR:
        drv_tc_type(t, s->u.var.type);
        drv_tc_expr(t, s->u.var.init);
        drv_tc_declare(t, s->u.var.name.name, NULL);
        drv_tc_expr(t, s->u.var.constraint);
        break;
    case STMT_ITEM:
        /* local items (fn, const, struct ...) are checked when used */
        drv_tc_declare(t, s->u.item->name.name, NULL);
        break;
    case STMT_ASSIGN: drv_tc_expr(t, s->u.assign.lhs); drv_tc_expr(t, s->u.assign.rhs); drv_tc_expr(t, s->u.assign.constraint); break;
    case STMT_EXPR: drv_tc_expr(t, s->u.expr); break;
    case STMT_FOR:
        drv_tc_expr(t, s->u.for_stmt.iter);
        mark = t->scope.len;
        drv_tc_pattern(t, s->u.for_stmt.pat);
        drv_tc_block(t, s->u.for_stmt.body);
        t->scope.len = mark;
        break;
    case STMT_WHILE: drv_tc_expr(t, s->u.while_stmt.cond); drv_tc_block(t, s->u.while_stmt.body); break;
    case STMT_ASSERT: drv_tc_expr(t, s->u.assert_stmt.cond); drv_tc_expr(t, s->u.assert_stmt.msg); break;
    case STMT_STATIC_ASSERT: drv_tc_expr(t, s->u.sassert.cond); drv_tc_expr(t, s->u.sassert.msg); break;
    case STMT_RETURN: drv_tc_expr(t, s->u.ret); break;
    default: break; /* hardware statements are errors in a test (reported by tern test) */
    }
}

static void drv_tc_block(DrvTc *t, Block *b)
{
    size_t mark = t->scope.len;
    uint32_t i;
    if (!b)
        return;
    for (i = 0; i < b->nstmts; i++)
        drv_tc_stmt(t, b->stmts[i]);
    t->scope.len = mark;
}

static void drv_check_tests(Driver *dr, Elab *el)
{
    DriverItemVec tests = {0};
    DrvTc t;
    size_t i;
    memset(&t, 0, sizeof t);
    t.dr = dr;
    t.el = el;
    driver_tests(dr, &tests);
    for (i = 0; i < tests.len; i++) {
        t.test = tests.data[i].item;
        t.file = tests.data[i].file;
        t.scope.len = 0;
        drv_tc_block(&t, t.test->u.test.body);
    }
    for (i = 0; i < t.path_maps.len; i++) {
        map_free(t.path_maps.data[i]);
        free(t.path_maps.data[i]);
    }
    vec_free(t.path_maps);
    map_free(&t.paths);
    vec_free(t.scope);
    vec_free(tests);
}

IrDesign *driver_elaborate_all(Driver *dr, const char *top, Arena *ir_arena)
{
    IrDesign *d = ir_design_new(ir_arena);
    Elab *el = driver_elab_new(dr, d);
    DriverItemVec roots = {0}, tests = {0};
    size_t i;
    if (top) {
        FileUnit *f = NULL;
        Item *it = driver_find_item(dr, top, &f);
        if (!it || it->kind != ITEM_MODULE) {
            diag_error(span_none(), "cannot find module `%s`", top);
        } else {
            DriverItem di;
            di.item = it;
            di.file = f;
            vec_push(roots, di);
        }
    } else {
        driver_tops(dr, &roots);
        if (!roots.len)
            driver_default_roots(dr, &roots);
    }
    for (i = 0; i < roots.len; i++) {
        ModInfo *mi = drv_elab_root(el, roots.data[i].item, roots.data[i].file, NULL);
        if (mi && mi->ir && mi->ir->is_top && !d->top)
            ir_design_set_top(d, mi->ir);
        el->limit_hit = 0;
        el->steps = 0;
    }
    if (!top) {
        /* the tests: the modules they simulate, with the tests' generic
         * arguments, and their names and signal paths */
        drv_check_tests(dr, el);
    }
    drv_check_items(dr, el);
    drv_add_regmaps(dr, el);
    if (diag_error_count() == 0)
        elab_ir_verify(d);
    vec_free(roots);
    vec_free(tests);
    elab_free(el);
    sema_info_syntactic(dr->S); /* hover/definition for code elaboration did not reach */
    return d;
}
