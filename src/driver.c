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
    } else if (r < 0) {
        /* manifest errors were reported; continue in single-file mode */
    }
    if (user.len == 0) {
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
        diag_error(span_none(), "features `%s` given, but there is no tern.toml declaring [features]", list);
        return -1;
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

/* `sim M<..>(..)` expressions in test bodies */
typedef VEC(Expr *) DrvExprVec;
static void drv_find_sims_expr(Expr *e, DrvExprVec *out);

static void drv_find_sims_block(Block *b, DrvExprVec *out)
{
    uint32_t i;
    for (i = 0; b && i < b->nstmts; i++) {
        Stmt *s = b->stmts[i];
        switch (s->kind) {
        case STMT_LET: drv_find_sims_expr(s->u.let.init, out); break;
        case STMT_VAR: if (s->u.var.init) drv_find_sims_expr(s->u.var.init, out); break;
        case STMT_ASSIGN: drv_find_sims_expr(s->u.assign.rhs, out); break;
        case STMT_EXPR: drv_find_sims_expr(s->u.expr, out); break;
        case STMT_FOR: drv_find_sims_block(s->u.for_stmt.body, out); break;
        case STMT_WHILE: drv_find_sims_block(s->u.while_stmt.body, out); break;
        default: break;
        }
    }
}

static void drv_find_sims_expr(Expr *e, DrvExprVec *out)
{
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_SIM: vec_push(*out, e); break;
    case EXPR_PAREN: drv_find_sims_expr(e->u.inner, out); break;
    case EXPR_BLOCK: drv_find_sims_block(e->u.block, out); break;
    case EXPR_IF:
        drv_find_sims_block(e->u.if_expr.then_block, out);
        drv_find_sims_expr(e->u.if_expr.else_expr, out);
        break;
    default: break;
    }
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
        /* modules instantiated by the tests, with the tests' generic arguments */
        driver_tests(dr, &tests);
        for (i = 0; i < tests.len; i++) {
            DrvExprVec sims = {0};
            size_t k;
            Item *t = tests.data[i].item;
            drv_find_sims_block(t->u.test.body, &sims);
            for (k = 0; k < sims.len; k++) {
                Expr *call = sims.data[k]->u.inner;
                Env *env = elab_env_new(el, NULL, tests.data[i].file);
                ValModule *vm;
                int errs = diag_error_count();
                el->quiet = 1;
                el->quiet_errors = 0;
                vm = call && call->kind == EXPR_CALL ? elab_eval_module(el, call, env) : NULL;
                el->quiet = 0;
                if (vm && !el->quiet_errors) {
                    char *frame = arena_printf(&el->arena, "test \"%s\" > %s", t->u.test.name ? t->u.test.name : "",
                                               vm->display);
                    diag_push_path(frame);
                    /* re-bind with diagnostics (where clauses) */
                    vm = elab_eval_module(el, call, env);
                    if (vm)
                        elab_module(el, vm, call->span);
                    diag_pop_path();
                }
                TERN_UNUSED(errs);
                el->quiet_errors = 0;
                el->limit_hit = 0;
                el->steps = 0;
                elab_env_free(env);
            }
            vec_free(sims);
        }
    }
    drv_add_regmaps(dr, el);
    if (diag_error_count() == 0)
        elab_ir_verify(d);
    vec_free(roots);
    vec_free(tests);
    elab_free(el);
    return d;
}
