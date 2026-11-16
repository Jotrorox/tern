/* resolve.c — packages, items, `use` declarations, re-exports, the prelude
 * and package-level name lookup (see sema.h). */
#include "sema.h"
#include "../base/diag.h"
#include "../os/os.h"

Sema *sema_new(void)
{
    Sema *S = (Sema *)xcalloc(1, sizeof(Sema));
    S->root = ANEW(&S->arena, Package);
    S->root->path = intern_cstr("");
    S->root->name = S->root->path;
    S->record_info = 1;
    S->t_error = ANEW(&S->arena, Ty);
    S->t_error->kind = TY_ERROR;
    S->t_error->key = intern_cstr("{error}");
    S->t_error->name = S->t_error->key;
    S->t_error->mangle = intern_cstr("err");
    S->t_bool = ty_scalar(S, TY_BOOL, 1);
    S->t_int = ty_int(S, 0);
    S->t_sint = ty_int(S, 1);
    S->t_void = ty_tuple(S, 0, NULL);
    S->t_void->name = intern_cstr("()");
    {
        const char *k = intern_cstr("string");
        Ty *t = ANEW(&S->arena, Ty);
        t->kind = TY_STRING;
        t->key = k;
        t->name = k;
        t->mangle = k;
        map_put(&S->types, MAP_PTR(k), MAP_PTR(t));
        S->t_string = t;
        k = intern_cstr("type");
        t = ANEW(&S->arena, Ty);
        t->kind = TY_TYPE;
        t->key = k;
        t->name = k;
        t->mangle = k;
        map_put(&S->types, MAP_PTR(k), MAP_PTR(t));
        S->t_type = t;
    }
    return S;
}

static void sema_free_pkg(Package *p)
{
    size_t it = 0;
    uint64_t k, v;
    while (map_next(&p->children, &it, &k, &v))
        sema_free_pkg((Package *)(uintptr_t)v);
    map_free(&p->children);
    map_free(&p->items);
    map_free(&p->reexports);
    vec_free(p->files);
    vec_free(p->reexport_globs);
}

void sema_free(Sema *S)
{
    size_t i;
    if (!S)
        return;
    for (i = 0; i < S->files.len; i++) {
        FileUnit *f = S->files.data[i];
        map_free(&f->uses);
        vec_free(f->globs);
        vec_free(f->all_uses);
    }
    vec_free(S->files);
    sema_free_pkg(S->root);
    map_free(&S->types);
    map_free(&S->info_index);
    vec_free(S->info);
    for (i = 0; i < S->retired_arenas.len; i++) {
        arena_release(S->retired_arenas.data[i]);
        free(S->retired_arenas.data[i]);
    }
    vec_free(S->retired_arenas);
    for (i = 0; i < S->retired_blocks.len; i++)
        free(S->retired_blocks.data[i]);
    vec_free(S->retired_blocks);
    arena_release(&S->arena);
    free(S);
}

static Package *sema_child(Sema *S, Package *p, const char *seg)
{
    uint64_t v;
    Package *c;
    StrBuf sb = {0};
    if (map_get(&p->children, MAP_PTR(seg), &v))
        return (Package *)(uintptr_t)v;
    c = ANEW(&S->arena, Package);
    c->parent = p;
    c->name = seg;
    if (p->path[0])
        sb_printf(&sb, "%s::%s", p->path, seg);
    else
        sb_puts(&sb, seg);
    c->path = intern_cstr(sb_cstr(&sb));
    sb_free(&sb);
    c->is_std = p->is_std || (p == S->root && strcmp(seg, "std") == 0);
    map_put(&p->children, MAP_PTR(seg), MAP_PTR(c));
    return c;
}

FileUnit *sema_add_file(Sema *S, AstFile *ast, int is_user, int is_std)
{
    FileUnit *f = ANEW(&S->arena, FileUnit);
    Package *p = S->root;
    uint32_t i;
    char *dir;
    f->ast = ast;
    f->is_user = is_user;
    f->is_std = is_std;
    dir = os_path_dirname(ast->path);
    f->dir = arena_strdup(&S->arena, dir);
    free(dir);
    if (ast->has_package)
        for (i = 0; i < ast->npackage; i++)
            if (ast->package[i].name)
                p = sema_child(S, p, ast->package[i].name);
    if (p == S->root && !ast->has_package) {
        /* unnamed files: one root package per directory */
        StrBuf sb = {0};
        sb_printf(&sb, "<dir:%s>", f->dir);
        p = sema_child(S, S->root, intern_cstr(sb_cstr(&sb)));
        p->path = intern_cstr("");
        sb_free(&sb);
    }
    f->pkg = p;
    vec_push(p->files, f);
    if (!p->dir)
        p->dir = f->dir;
    vec_push(S->files, f);
    return f;
}

Package *sema_find_package(Sema *S, const char *path)
{
    Package *p = S->root;
    const char *s = path;
    while (*s) {
        const char *e = strstr(s, "::");
        size_t n = e ? (size_t)(e - s) : strlen(s);
        uint64_t v;
        if (!map_get(&p->children, MAP_PTR(intern(s, n)), &v))
            return NULL;
        p = (Package *)(uintptr_t)v;
        s = e ? e + 2 : s + n;
    }
    return p;
}

Package *sema_root_package(Sema *S, const char *name)
{
    uint64_t v;
    if (map_get(&S->root->children, MAP_PTR(name), &v))
        return (Package *)(uintptr_t)v;
    return NULL;
}

FileUnit *sema_file_of_item(Sema *S, const Item *it)
{
    size_t i;
    for (i = 0; i < S->files.len; i++) {
        FileUnit *f = S->files.data[i];
        if (f->ast->src == it->span.file)
            return f;
    }
    return NULL;
}

int sema_feature(Sema *S, const char *name, int *value)
{
    size_t i;
    for (i = 0; i < S->nfeatures; i++)
        if (strcmp(S->feature_names[i], name) == 0) {
            *value = S->feature_values[i];
            return 1;
        }
    return 0;
}

/* --------------------------------------------------------------- uses -- */

static void sema_flatten_use(Sema *S, FileUnit *f, UseDecl *ud, UseTree *t, const char **prefix,
                             Span *pspans, uint32_t nprefix)
{
    uint32_t n = nprefix + t->npath, i;
    const char **segs = ANEWN(&S->arena, const char *, n ? n : 1);
    Span *spans = ANEWN(&S->arena, Span, n ? n : 1);
    for (i = 0; i < nprefix; i++) {
        segs[i] = prefix[i];
        spans[i] = pspans[i];
    }
    for (i = 0; i < t->npath; i++) {
        segs[nprefix + i] = t->path[i].name;
        spans[nprefix + i] = t->path[i].span;
    }
    if (t->is_group) {
        for (i = 0; i < t->ngroup; i++)
            sema_flatten_use(S, f, ud, t->group[i], segs, spans, n);
        return;
    }
    for (i = 0; i < n; i++)
        if (!segs[i])
            return; /* syntax error */
    {
        UseEntry *u = ANEW(&S->arena, UseEntry);
        u->segs = segs;
        u->seg_spans = spans;
        u->nsegs = n;
        u->glob = t->glob;
        u->is_pub = ud->is_pub;
        u->span = t->span;
        u->file = f;
        vec_push(f->all_uses, u);
        if (t->glob) {
            vec_push(f->globs, u);
            if (u->is_pub)
                vec_push(f->pkg->reexport_globs, u);
            return;
        }
        if (n == 0)
            return;
        u->name = t->alias.name ? t->alias.name : segs[n - 1];
        {
            uint64_t ov;
            if (map_get(&f->uses, MAP_PTR(u->name), &ov)) {
                UseEntry *o = (UseEntry *)(uintptr_t)ov;
                int same = o->nsegs == n;
                for (i = 0; same && i < n; i++)
                    if (o->segs[i] != segs[i])
                        same = 0;
                if (!same) {
                    diag_error(u->span, "`%s` is imported twice", u->name);
                    return;
                }
                if (u->is_pub && !map_has(&f->pkg->reexports, MAP_PTR(u->name)))
                    map_put(&f->pkg->reexports, MAP_PTR(u->name), MAP_PTR(u));
                return;
            }
        }
        map_put(&f->uses, MAP_PTR(u->name), MAP_PTR(u));
        if (u->is_pub) {
            if (map_has(&f->pkg->reexports, MAP_PTR(u->name)) || map_has(&f->pkg->items, MAP_PTR(u->name)))
                diag_error(u->span, "`%s` is defined twice in package `%s`", u->name, f->pkg->path);
            else
                map_put(&f->pkg->reexports, MAP_PTR(u->name), MAP_PTR(u));
        }
    }
}

static int sema_resolve_use(Sema *S, UseEntry *u, int report);

static int sema_item_visible(const Package *owner, const FileUnit *from, const Item *it)
{
    if (it->is_pub)
        return 1;
    if (!from)
        return 0;
    return from->pkg == owner;
}

int sema_lookup_member(Sema *S, FileUnit *from, Package *pkg, const char *name, Symbol *out,
                       int *private_out)
{
    uint64_t v;
    size_t i;
    if (private_out)
        *private_out = 0;
    if (map_get(&pkg->items, MAP_PTR(name), &v)) {
        Symbol *s = (Symbol *)(uintptr_t)v;
        if (sema_item_visible(pkg, from, s->item)) {
            *out = *s;
            return 1;
        }
        if (private_out)
            *private_out = 1;
    }
    if (map_get(&pkg->reexports, MAP_PTR(name), &v)) {
        UseEntry *u = (UseEntry *)(uintptr_t)v;
        if (sema_resolve_use(S, u, 0)) {
            *out = u->sym;
            return 1;
        }
    }
    if (map_get(&pkg->children, MAP_PTR(name), &v)) {
        out->kind = SYM_PACKAGE;
        out->pkg = (Package *)(uintptr_t)v;
        out->item = NULL;
        out->file = NULL;
        return 1;
    }
    for (i = 0; i < pkg->reexport_globs.len; i++) {
        UseEntry *u = pkg->reexport_globs.data[i];
        if (u->state == 1)
            continue;
        if (sema_resolve_use(S, u, 0) && u->sym.kind == SYM_PACKAGE && u->sym.pkg != pkg &&
            sema_lookup_member(S, from, u->sym.pkg, name, out, NULL))
            return 1;
    }
    return 0;
}

static void sema_report_member_missing(Sema *S, FileUnit *from, Package *pkg, const char *name, Span sp,
                                       int priv)
{
    if (priv) {
        diag_error(sp, "`%s` is private to package `%s`", name, pkg->path);
        diag_help("mark it `pub` to use it from another package");
        return;
    }
    diag_error(sp, "cannot find `%s` in `%s`", name, pkg->path);
    {
        /* closest public name of that package */
        size_t it = 0;
        uint64_t k, v;
        const char *best = NULL;
        uint32_t bd = 99, cap = (uint32_t)(strlen(name) / 3 + 1);
        while (map_next(&pkg->items, &it, &k, &v)) {
            Symbol *s = (Symbol *)(uintptr_t)v;
            uint32_t d;
            if (!sema_item_visible(pkg, from, s->item))
                continue;
            d = sema_edit_distance(name, (const char *)(uintptr_t)k, cap);
            if (d <= cap && d < bd) {
                bd = d;
                best = (const char *)(uintptr_t)k;
            }
        }
        if (best)
            diag_help("did you mean `%s::%s`?", pkg->path, best);
    }
    TERN_UNUSED(S);
}

static int sema_resolve_use(Sema *S, UseEntry *u, int report)
{
    Package *p;
    uint32_t i, last;
    Symbol sym;
    int priv = 0;
    if (u->state == 2)
        return 1;
    if (u->state == 3 || u->state == 1)
        return 0;
    u->state = 1;
    p = sema_root_package(S, u->segs[0]);
    if (!p) {
        /* a name visible in the file's own package (sibling item import is pointless but harmless) */
        if (report) {
            diag_error(u->seg_spans[0], "cannot find package `%s`", u->segs[0]);
            if (strcmp(u->segs[0], "self") == 0 || strcmp(u->segs[0], "crate") == 0 ||
                strcmp(u->segs[0], "super") == 0)
                diag_help("paths in `use` start with a package name, e.g. `use std::fifo::Fifo`");
        }
        u->state = 3;
        return 0;
    }
    last = u->glob ? u->nsegs : u->nsegs - 1;
    sym.kind = SYM_PACKAGE;
    sym.pkg = p;
    sym.item = NULL;
    sym.file = NULL;
    for (i = 1; i < u->nsegs; i++) {
        if (sym.kind != SYM_PACKAGE) {
            if (report)
                diag_error(u->seg_spans[i], "`%s` is not a package", u->segs[i - 1]);
            u->state = 3;
            return 0;
        }
        if (!sema_lookup_member(S, u->file, sym.pkg, u->segs[i], &sym, &priv)) {
            if (report)
                sema_report_member_missing(S, u->file, sym.pkg, u->segs[i], u->seg_spans[i], priv);
            u->state = 3;
            return 0;
        }
    }
    TERN_UNUSED(last);
    if (u->glob && sym.kind != SYM_PACKAGE) {
        if (report)
            diag_error(u->span, "`use ...::*` needs a package");
        u->state = 3;
        return 0;
    }
    u->sym = sym;
    u->state = 2;
    return 1;
}

/* ------------------------------------------------------------ resolve -- */

static void sema_add_items(Sema *S, Package *p)
{
    size_t fi;
    uint32_t i;
    for (fi = 0; fi < p->files.len; fi++) {
        FileUnit *f = p->files.data[fi];
        for (i = 0; i < f->ast->nitems; i++) {
            Item *it = f->ast->items[i];
            Symbol *s;
            uint64_t v;
            if (!it->name.name || it->kind == ITEM_TEST || it->kind == ITEM_ERROR)
                continue;
            if (map_get(&p->items, MAP_PTR(it->name.name), &v)) {
                Symbol *o = (Symbol *)(uintptr_t)v;
                diag_error(it->name.span, "`%s` is defined twice in package `%s`", it->name.name,
                           p->path[0] ? p->path : "(root)");
                diag_note(o->item->name.span, "first definition here");
                continue;
            }
            s = ANEW(&S->arena, Symbol);
            s->kind = SYM_ITEM;
            s->item = it;
            s->file = f;
            s->pkg = p;
            map_put(&p->items, MAP_PTR(it->name.name), MAP_PTR(s));
        }
    }
}

static void sema_walk_pkgs(Sema *S, Package *p, void (*fn)(Sema *, Package *))
{
    size_t it = 0;
    uint64_t k, v;
    fn(S, p);
    while (map_next(&p->children, &it, &k, &v))
        sema_walk_pkgs(S, (Package *)(uintptr_t)v, fn);
}

void sema_resolve(Sema *S)
{
    size_t i, j;
    uint32_t k;
    sema_walk_pkgs(S, S->root, sema_add_items);
    /* directory agreement */
    for (i = 0; i < S->files.len; i++) {
        FileUnit *a = S->files.data[i];
        if (!a->ast->has_package || !a->is_user)
            continue;
        for (j = 0; j < i; j++) {
            FileUnit *b = S->files.data[j];
            if (!b->ast->has_package || !b->is_user || strcmp(a->dir, b->dir) != 0)
                continue;
            if (a->pkg != b->pkg) {
                diag_error(a->ast->package_span,
                           "files in one directory must declare the same package: `%s` here, `%s` in %s",
                           a->pkg->path, b->pkg->path, b->ast->path);
                break;
            }
        }
    }
    S->prelude = sema_find_package(S, "std::prelude");
    for (i = 0; i < S->files.len; i++) {
        FileUnit *f = S->files.data[i];
        for (k = 0; k < f->ast->nuses; k++)
            sema_flatten_use(S, f, f->ast->uses[k], f->ast->uses[k]->tree, NULL, NULL, 0);
    }
    for (i = 0; i < S->files.len; i++) {
        FileUnit *f = S->files.data[i];
        for (j = 0; j < f->all_uses.len; j++)
            sema_resolve_use(S, f->all_uses.data[j], 1);
    }
}

int sema_lookup(Sema *S, FileUnit *from, const char *name, Symbol *out)
{
    uint64_t v;
    size_t i;
    Package *rp;
    if (!name)
        return 0;
    if (from) {
        if (map_get(&from->pkg->items, MAP_PTR(name), &v)) {
            *out = *(Symbol *)(uintptr_t)v;
            return 1;
        }
        if (map_get(&from->uses, MAP_PTR(name), &v)) {
            UseEntry *u = (UseEntry *)(uintptr_t)v;
            if (sema_resolve_use(S, u, 0)) {
                *out = u->sym;
                return 1;
            }
            return 0;
        }
        /* re-exports of the own package declared in sibling files */
        if (map_get(&from->pkg->reexports, MAP_PTR(name), &v)) {
            UseEntry *u = (UseEntry *)(uintptr_t)v;
            if (sema_resolve_use(S, u, 0)) {
                *out = u->sym;
                return 1;
            }
        }
        for (i = 0; i < from->globs.len; i++) {
            UseEntry *u = from->globs.data[i];
            if (sema_resolve_use(S, u, 0) && u->sym.kind == SYM_PACKAGE &&
                sema_lookup_member(S, from, u->sym.pkg, name, out, NULL))
                return 1;
        }
    }
    if (S->prelude && (!from || from->pkg != S->prelude) &&
        sema_lookup_member(S, from, S->prelude, name, out, NULL) && out->kind == SYM_ITEM)
        return 1;
    rp = sema_root_package(S, name);
    if (rp && rp->path[0]) {
        out->kind = SYM_PACKAGE;
        out->pkg = rp;
        out->item = NULL;
        out->file = NULL;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------- suggestions -- */

uint32_t sema_edit_distance(const char *a, const char *b, uint32_t cap)
{
    size_t la = strlen(a), lb = strlen(b), i, j;
    uint32_t prev[64], cur[64];
    if (la > 62 || lb > 62)
        return cap + 1;
    if ((la > lb ? la - lb : lb - la) > cap)
        return cap + 1;
    for (j = 0; j <= lb; j++)
        prev[j] = (uint32_t)j;
    for (i = 1; i <= la; i++) {
        uint32_t rowmin;
        cur[0] = (uint32_t)i;
        rowmin = cur[0];
        for (j = 1; j <= lb; j++) {
            uint32_t c = (tern_to_lower((unsigned char)a[i - 1]) == tern_to_lower((unsigned char)b[j - 1]))
                             ? (a[i - 1] == b[j - 1] ? 0u : 1u)
                             : 1u;
            uint32_t d = prev[j - 1] + c;
            if (prev[j] + 1 < d)
                d = prev[j] + 1;
            if (cur[j - 1] + 1 < d)
                d = cur[j - 1] + 1;
            cur[j] = d;
            if (d < rowmin)
                rowmin = d;
        }
        if (rowmin > cap)
            return cap + 1;
        memcpy(prev, cur, (lb + 1) * sizeof(uint32_t));
    }
    return prev[lb];
}

typedef struct SemaSugg {
    const char *name;
    const char *best;
    uint32_t bd, cap;
} SemaSugg;

static void sema_sugg_try(SemaSugg *s, const char *cand)
{
    uint32_t d;
    if (!cand || cand == s->name || cand[0] == '<')
        return;
    d = sema_edit_distance(s->name, cand, s->cap);
    if (d <= s->cap && d < s->bd) {
        s->bd = d;
        s->best = cand;
    }
}

static void sema_sugg_pkg(SemaSugg *s, Package *p, FileUnit *from)
{
    size_t it = 0;
    uint64_t k, v;
    while (map_next(&p->items, &it, &k, &v)) {
        Symbol *sym = (Symbol *)(uintptr_t)v;
        if (sema_item_visible(p, from, sym->item))
            sema_sugg_try(s, (const char *)(uintptr_t)k);
    }
    it = 0;
    while (map_next(&p->reexports, &it, &k, &v))
        sema_sugg_try(s, (const char *)(uintptr_t)k);
}

static int sema_find_exact(Sema *S, Package *p, const char *name, StrBuf *out)
{
    uint64_t v;
    size_t it = 0;
    uint64_t k;
    if (p->path[0] && p != S->prelude && map_get(&p->items, MAP_PTR(name), &v)) {
        Symbol *sym = (Symbol *)(uintptr_t)v;
        if (sym->item->is_pub) {
            sb_printf(out, "%s::%s", p->path, name);
            return 1;
        }
    }
    while (map_next(&p->children, &it, &k, &v))
        if (sema_find_exact(S, (Package *)(uintptr_t)v, name, out))
            return 1;
    return 0;
}

void sema_suggest(Sema *S, FileUnit *from, const char *name, const char *const *extra, size_t nextra)
{
    SemaSugg s;
    size_t i;
    StrBuf sb = {0};
    if (!name)
        return;
    s.name = name;
    s.best = NULL;
    s.bd = 99;
    s.cap = (uint32_t)(strlen(name) <= 3 ? 1 : strlen(name) / 3);
    if (sema_find_exact(S, S->root, name, &sb)) {
        diag_help("did you mean to `use %s`?", sb_cstr(&sb));
        sb_free(&sb);
        return;
    }
    sb_free(&sb);
    for (i = 0; i < nextra; i++)
        sema_sugg_try(&s, extra[i]);
    if (from) {
        size_t it = 0;
        uint64_t k, v;
        sema_sugg_pkg(&s, from->pkg, from);
        while (map_next(&from->uses, &it, &k, &v))
            sema_sugg_try(&s, (const char *)(uintptr_t)k);
        for (i = 0; i < from->globs.len; i++)
            if (from->globs.data[i]->state == 2 && from->globs.data[i]->sym.kind == SYM_PACKAGE)
                sema_sugg_pkg(&s, from->globs.data[i]->sym.pkg, from);
    }
    if (S->prelude)
        sema_sugg_pkg(&s, S->prelude, from);
    if (s.best)
        diag_help("did you mean `%s`?", s.best);
}
