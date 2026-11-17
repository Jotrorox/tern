/* doc.c — `tern doc`: documentation from `///` comments (spec §12, §13.2).
 *
 *     tern doc                       HTML site for the project (+ std) in build/doc/
 *     tern doc std -o out/           HTML site for the standard library
 *     tern doc std::fifo             terminal rendering of a package (offline reference)
 *     tern doc std::fifo::Fifo       ... of one item
 *     tern doc src/uart.tern --text  terminal rendering of the packages of a file
 *
 * Everything works from the AST (no elaboration): signatures, generics,
 * ports, fields, variants, published consts and regmap registers are shown
 * as written in the source. The `///` text is a Markdown subset (paragraphs,
 * `code`, fenced code blocks, *emphasis*, **strong**, lists, [links](url),
 * [Item] links, # headings). Type names are cross-linked syntactically: same
 * package, `use`d names (also globs and groups), `pub use` re-exports, full
 * paths and the prelude.
 *
 * HTML: one page per package (`std.fifo.html`), an index, one shared
 * style.css (light and dark), search.js and a generated search-index.js;
 * no external resources. Tern code is highlighted here in C (spans with
 * classes kw/ty/num/str/com/attr).
 *
 * std comes from the embedded sources (std_embed.h), or from the directory
 * in $TERN_STD when it is set. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/map.h"
#include "../base/utf8.h"
#include "../base/diag.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../syntax/parser.h"
#include "../std_embed.h"
#include "../cmds.h"
#include "tools.h"

typedef struct DocPkg DocPkg;
typedef struct DocFile DocFile;

typedef struct DocAlias {
    const char *name;     /* interned local name */
    const char *full;     /* interned full path it stands for */
} DocAlias;

typedef VEC(DocAlias) DocAliasVec;
typedef VEC(const char *) DocStrVec;
typedef VEC(const Item *) DocItemPtrVec;

struct DocFile {
    char *path;
    Arena arena;
    AstFile *ast;
    const char *src;
    uint32_t srclen;
    DocPkg *pkg;
    DocAliasVec aliases;  /* from `use` */
    DocStrVec globs;      /* package paths imported with `::*` */
};

typedef struct DocItem {
    const Item *it;
    DocFile *file;
    DocPkg *pkg;
    const char *name;
    const char *full;     /* interned "std::fifo::Fifo" */
    char *anchor;         /* "module.Fifo" */
    DocItemPtrVec tests;  /* tests attributed to it */
} DocItem;

typedef VEC(DocItem *) DocItemVec;
typedef VEC(DocFile *) DocFileVec;

typedef struct DocReexport {
    DocFile *file;
    const UseDecl *use;
} DocReexport;

typedef VEC(DocReexport) DocReexportVec;

struct DocPkg {
    const char *name;     /* interned "std::fifo" */
    char *page;           /* "std.fifo.html" */
    int is_std;
    DocFileVec files;
    DocItemVec items;
    DocItemPtrVec tests;  /* tests not attributed to an item */
    DocFile *main;        /* file whose leading comment describes the package */
    DocReexportVec reexports;
};

typedef VEC(DocPkg *) DocPkgVec;

struct DocSet {            /* typedef in tools.h */
    DocFileVec files;
    DocPkgVec pkgs;
    Map index;            /* interned full name -> DocItem* */
    Arena arena;          /* strings */
    int nerrors;
    int finished;
};

/* ------------------------------------------------------------ utilities */

static const char *doc_intern2(const char *a, const char *b)
{
    StrBuf sb = {0};
    const char *r;
    sb_puts(&sb, a);
    sb_puts(&sb, "::");
    sb_puts(&sb, b);
    r = intern(sb.data, sb.len);
    sb_free(&sb);
    return r;
}

static uint32_t doc_end(Span sp)
{
    return sp.off + sp.len;
}

/* Source text of a span with whitespace runs collapsed to one space. */
static char *doc_src(DocSet *ds, const DocFile *f, Span sp)
{
    StrBuf sb = {0};
    uint32_t i, end = doc_end(sp);
    int ws = 0, nl = 0;
    char *r;
    if (end > f->srclen)
        end = f->srclen;
    for (i = sp.off; i < end; i++) {
        char c = f->src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ws = 1;
            if (c == '\n')
                nl = 1;
            continue;
        }
        if (ws && sb.len > 0) {
            char p = sb.data[sb.len - 1];
            /* a line break after an opening or before a closing bracket of a
             * multi-line list disappears; other whitespace becomes one space */
            if (!nl || (p != '(' && p != '[' && p != '<' && p != '{' && c != ')' && c != ']' && c != '>' &&
                        c != '}' && c != ','))
                sb_putc(&sb, ' ');
        }
        ws = 0;
        nl = 0;
        sb_putc(&sb, c);
    }
    /* a trailing comma before a closing bracket (multi-line lists) */
    r = arena_strndup(&ds->arena, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    {
        char *w = r, *q;
        for (q = r; *q; q++) {
            if (*q == ',' && (q[1] == ')' || q[1] == ']' || q[1] == '>' || q[1] == '}'))
                continue;
            *w++ = *q;
        }
        *w = '\0';
    }
    return r;
}

static const char *doc_kind_name(const Item *it)
{
    switch (it->kind) {
    case ITEM_MODULE: return "module";
    case ITEM_EXTERN: return "extern module";
    case ITEM_BUNDLE: return "bundle";
    case ITEM_STRUCT: return "struct";
    case ITEM_ENUM: return "enum";
    case ITEM_CONST: return "const";
    case ITEM_FN: return it->u.fn.is_const ? "const fn" : "fn";
    case ITEM_TYPE: return "type";
    case ITEM_REGMAP: return "regmap";
    case ITEM_TEST: return "test";
    case ITEM_PROPERTY: return "property";
    case ITEM_SEQUENCE: return "sequence";
    default: return "item";
    }
}

static const char *doc_anchor_kind(const Item *it)
{
    switch (it->kind) {
    case ITEM_EXTERN: return "extern";
    case ITEM_FN: return "fn";
    default: return doc_kind_name(it);
    }
}

/* Sections of a package page, in order. */
static const struct {
    ItemKind kind;
    const char *title;
} doc_sections[] = {
    {ITEM_MODULE, "Modules"},       {ITEM_EXTERN, "Extern modules"}, {ITEM_BUNDLE, "Bundles"},
    {ITEM_STRUCT, "Structs"},       {ITEM_ENUM, "Enums"},            {ITEM_TYPE, "Type aliases"},
    {ITEM_CONST, "Constants"},      {ITEM_FN, "Functions"},          {ITEM_REGMAP, "Register maps"},
    {ITEM_PROPERTY, "Properties"},  {ITEM_SEQUENCE, "Sequences"},
};

/* `///` text of the doc comments right before the token at `off` (generic
 * parameters keep theirs only in the comment list), or NULL. */
static char *doc_comment_before(DocSet *ds, const DocFile *f, uint32_t off)
{
    const AstFile *a = f->ast;
    uint32_t lo = 0, hi = a->ntokens, prev_end = 0, i;
    StrBuf sb = {0};
    char *r;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a->tokens[mid].off < off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > 0)
        prev_end = a->tokens[lo - 1].off + a->tokens[lo - 1].len;
    for (i = 0; i < a->ncomments; i++) {
        const Comment *c = &a->comments[i];
        if (c->kind != COMMENT_DOC || c->span.off < prev_end || c->span.off >= off)
            continue;
        if (sb.len)
            sb_putc(&sb, '\n');
        {
            uint32_t s = c->span.off + 3, e = doc_end(c->span);
            if (s < e && f->src[s] == ' ')
                s++;
            sb_append(&sb, f->src + s, e - s);
        }
    }
    if (!sb.len)
        return NULL;
    r = arena_strndup(&ds->arena, sb.data, sb.len);
    sb_free(&sb);
    return r;
}

/* First sentence (or line) of a doc text, Markdown kept. */
static char *doc_summary(DocSet *ds, const char *text)
{
    StrBuf sb = {0};
    const char *p = text;
    char *r;
    if (!text)
        return NULL;
    while (*p == ' ' || *p == '\n')
        p++;
    for (; *p; p++) {
        if (*p == '\n') {
            if (p[1] == '\n' || p[1] == '\0' || p[1] == '*' || p[1] == '-' || p[1] == '#' || p[1] == '`')
                break;
            sb_putc(&sb, ' ');
            continue;
        }
        sb_putc(&sb, *p);
        if (*p == '.' && (p[1] == ' ' || p[1] == '\n' || p[1] == '\0') && sb.len > 3)
            break;
    }
    r = arena_strndup(&ds->arena, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return r;
}

/* ---------------------------------------------------------------- loading */

DocSet *doc_set_new(void)
{
    return (DocSet *)xcalloc(1, sizeof(DocSet));
}

void doc_set_free(DocSet *ds)
{
    size_t i, j;
    if (!ds)
        return;
    for (i = 0; i < ds->files.len; i++) {
        DocFile *f = ds->files.data[i];
        arena_release(&f->arena);
        vec_free(f->aliases);
        vec_free(f->globs);
        free(f->path);
        free(f);
    }
    for (i = 0; i < ds->pkgs.len; i++) {
        DocPkg *p = ds->pkgs.data[i];
        for (j = 0; j < p->items.len; j++) {
            vec_free(p->items.data[j]->tests);
            free(p->items.data[j]->anchor);
            free(p->items.data[j]);
        }
        vec_free(p->items);
        vec_free(p->files);
        vec_free(p->tests);
        vec_free(p->reexports);
        free(p->page);
        free(p);
    }
    vec_free(ds->files);
    vec_free(ds->pkgs);
    map_free(&ds->index);
    arena_release(&ds->arena);
    free(ds);
}

static DocPkg *doc_pkg_get(DocSet *ds, const char *name, int is_std)
{
    size_t i;
    DocPkg *p;
    char *q;
    for (i = 0; i < ds->pkgs.len; i++) {
        if (ds->pkgs.data[i]->name == name)
            return ds->pkgs.data[i];
    }
    p = (DocPkg *)xcalloc(1, sizeof *p);
    p->name = name;
    p->is_std = is_std;
    p->page = (char *)xmalloc(strlen(name) + 8);
    for (q = p->page; *name; name++) {
        if (name[0] == ':' && name[1] == ':') {
            *q++ = '.';
            name++;
        } else {
            *q++ = *name;
        }
    }
    strcpy(q, ".html");
    vec_push(ds->pkgs, p);
    return p;
}

static void doc_use_aliases(DocFile *f, const UseTree *u, const char *prefix)
{
    StrBuf sb = {0};
    uint32_t i;
    const char *full;
    if (prefix) {
        sb_puts(&sb, prefix);
    }
    for (i = 0; i < u->npath; i++) {
        if (sb.len)
            sb_puts(&sb, "::");
        sb_puts(&sb, u->path[i].name);
    }
    full = intern(sb.data ? sb.data : "", sb.len);
    if (u->glob) {
        vec_push(f->globs, full);
    } else if (u->is_group) {
        for (i = 0; i < u->ngroup; i++)
            doc_use_aliases(f, u->group[i], full);
    } else if (u->npath > 0) {
        DocAlias al;
        al.name = u->alias.name ? u->alias.name : u->path[u->npath - 1].name;
        al.full = full;
        vec_push(f->aliases, al);
    }
    sb_free(&sb);
}

static void doc_null_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(d);
    TERN_UNUSED(user);
}

/* Adds one source file. Files with syntax errors are reported and skipped
 * (returns -1). `fallback_pkg` names the package of a file without
 * `package` line. */
int doc_set_add_source(DocSet *ds, const char *path, const char *text, size_t len, int is_std,
                       const char *fallback_pkg)
{
    DocFile *f = (DocFile *)xcalloc(1, sizeof *f);
    uint32_t id, i;
    const SourceFile *sf;
    StrBuf pk = {0};
    f->path = xstrdup(path);
    id = src_add(path, text, len);
    sf = src_get(id);
    f->src = sf->text;
    f->srclen = (uint32_t)sf->len;
    if (is_std)
        diag_set_sink(doc_null_sink, NULL);
    f->ast = parse_file(&f->arena, id);
    diag_flush();
    if (is_std)
        diag_set_sink(NULL, NULL);
    if (f->ast->nerrors) {
        ds->nerrors++;
        arena_release(&f->arena);
        free(f->path);
        free(f);
        return -1;
    }
    if (f->ast->has_package) {
        for (i = 0; i < f->ast->npackage; i++) {
            if (i)
                sb_puts(&pk, "::");
            sb_puts(&pk, f->ast->package[i].name);
        }
    } else {
        sb_puts(&pk, fallback_pkg ? fallback_pkg : "main");
    }
    f->pkg = doc_pkg_get(ds, intern(pk.data, pk.len), is_std);
    sb_free(&pk);
    for (i = 0; i < f->ast->nuses; i++)
        doc_use_aliases(f, f->ast->uses[i]->tree, NULL);
    vec_push(f->pkg->files, f);
    vec_push(ds->files, f);
    return 0;
}

/* The standard library: $TERN_STD (a std/ directory) or the embedded copy. */
static void doc_collect_dir(const char *dir, DocStrVec *out)
{
    OsDirList list;
    size_t i;
    if (os_list_dir(dir, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        char *p;
        if (list.items[i].name[0] == '.')
            continue;
        p = os_path_join(dir, list.items[i].name);
        if (list.items[i].is_dir) {
            doc_collect_dir(p, out);
            free(p);
        } else if (strcmp(os_path_ext(p), ".tern") == 0) {
            vec_push(*out, p);
        } else {
            free(p);
        }
    }
    os_dir_list_free(&list);
}

int doc_set_add_std(DocSet *ds)
{
    char *env = os_getenv("TERN_STD");
    size_t i;
    if (env && env[0]) {
        DocStrVec paths = {0};
        doc_collect_dir(env, &paths);
        for (i = 0; i < paths.len; i++) {
            char *text = NULL;
            size_t len = 0;
            if (os_read_file(paths.data[i], &text, &len) == 0) {
                doc_set_add_source(ds, paths.data[i], text, len, 1, NULL);
                free(text);
            }
            free((char *)paths.data[i]);
        }
        vec_free(paths);
        free(env);
        return 0;
    }
    free(env);
    for (i = 0; i < tern_std_file_count; i++)
        doc_set_add_source(ds, tern_std_files[i].path, tern_std_text(i), tern_std_files[i].len, 1, NULL);
    return 0;
}

/* -------------------------------------------------------- tests, indexing */

static void doc_sims_expr(const Expr *e, DocStrVec *out);

static void doc_sims_block(const Block *b, DocStrVec *out);

static void doc_sims_stmt(const Stmt *s, DocStrVec *out)
{
    if (!s)
        return;
    switch (s->kind) {
    case STMT_LET:
        doc_sims_expr(s->u.let.init, out);
        break;
    case STMT_VAR:
        doc_sims_expr(s->u.var.init, out);
        break;
    case STMT_ASSIGN:
        doc_sims_expr(s->u.assign.rhs, out);
        break;
    case STMT_EXPR:
        doc_sims_expr(s->u.expr, out);
        break;
    case STMT_FOR:
        doc_sims_block(s->u.for_stmt.body, out);
        break;
    case STMT_WHILE:
        doc_sims_block(s->u.while_stmt.body, out);
        break;
    default:
        break;
    }
}

static void doc_sims_block(const Block *b, DocStrVec *out)
{
    uint32_t i;
    if (!b)
        return;
    for (i = 0; i < b->nstmts; i++)
        doc_sims_stmt(b->stmts[i], out);
}

static void doc_sims_expr(const Expr *e, DocStrVec *out)
{
    uint32_t i;
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_SIM: {
        const Expr *c = e->u.inner;
        if (c->kind == EXPR_CALL && c->u.call.callee->kind == EXPR_PATH) {
            const Path *p = &c->u.call.callee->u.path;
            vec_push(*out, p->segs[p->nsegs - 1].name.name);
        }
        break;
    }
    case EXPR_BLOCK:
        doc_sims_block(e->u.block, out);
        break;
    case EXPR_IF:
        doc_sims_block(e->u.if_expr.then_block, out);
        doc_sims_expr(e->u.if_expr.else_expr, out);
        break;
    case EXPR_MATCH:
        for (i = 0; i < e->u.match.narms; i++)
            doc_sims_stmt(e->u.match.arms[i].body, out);
        break;
    case EXPR_PAREN:
        doc_sims_expr(e->u.inner, out);
        break;
    default:
        break;
    }
}

static DocItem *doc_find_in_pkg(DocPkg *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->items.len; i++) {
        if (p->items.data[i]->name == name)
            return p->items.data[i];
    }
    return NULL;
}

static int doc_word_prefix(const char *s, const char *word)
{
    size_t n = strlen(word);
    if (tern_strcasecmp(word, "") == 0 || strlen(s) < n)
        return 0;
    {
        size_t i;
        for (i = 0; i < n; i++) {
            if (tern_to_lower((unsigned char)s[i]) != tern_to_lower((unsigned char)word[i]))
                return 0;
        }
    }
    return !tern_is_ident((unsigned char)s[n]);
}

static void doc_attach_test(DocPkg *p, const Item *t)
{
    DocStrVec sims = {0};
    DocItem *target = NULL;
    size_t i;
    doc_sims_block(t->u.test.body, &sims);
    for (i = 0; i < sims.len && !target; i++) {
        DocItem *d = doc_find_in_pkg(p, sims.data[i]);
        if (d && (d->it->kind == ITEM_MODULE || d->it->kind == ITEM_EXTERN))
            target = d;
    }
    vec_free(sims);
    for (i = 0; i < p->items.len && !target; i++) {
        if (t->u.test.name && doc_word_prefix(t->u.test.name, p->items.data[i]->name))
            target = p->items.data[i];
    }
    if (target)
        vec_push(target->tests, t);
    else
        vec_push(p->tests, t);
}

static int doc_pkg_cmp(const void *a, const void *b)
{
    const DocPkg *x = *(DocPkg *const *)a, *y = *(DocPkg *const *)b;
    if (x->is_std != y->is_std)
        return x->is_std ? 1 : -1;
    /* std::prelude first among std */
    if (x->is_std && strcmp(x->name, "std::prelude") == 0)
        return -1;
    if (y->is_std && strcmp(y->name, "std::prelude") == 0)
        return 1;
    return strcmp(x->name, y->name);
}

/* Which items a package page documents: pub items for std; everything but
 * tests for project packages (they rarely mark items pub). */
static int doc_documented(const DocPkg *p, const Item *it)
{
    if (it->kind == ITEM_TEST || it->kind == ITEM_STATIC_ASSERT || it->kind == ITEM_ERROR || !it->name.name)
        return 0;
    return it->is_pub || !p->is_std;
}

void doc_set_finish(DocSet *ds)
{
    size_t i, j, k;
    if (ds->finished)
        return;
    ds->finished = 1;
    if (ds->pkgs.len > 1)
        qsort(ds->pkgs.data, ds->pkgs.len, sizeof(DocPkg *), doc_pkg_cmp);
    for (i = 0; i < ds->pkgs.len; i++) {
        DocPkg *p = ds->pkgs.data[i];
        const char *last = strrchr(p->name, ':');
        last = last ? last + 1 : p->name;
        for (j = 0; j < p->files.len; j++) {
            DocFile *f = p->files.data[j];
            const char *base = os_path_basename(f->path);
            if (!p->main && strncmp(base, last, strlen(last)) == 0 && strcmp(base + strlen(last), ".tern") == 0)
                p->main = f;
        }
        if (!p->main && p->files.len)
            p->main = p->files.data[0];
        for (j = 0; j < p->files.len; j++) {
            if (p->files.data[j] == p->main && j > 0) {
                memmove(p->files.data + 1, p->files.data, j * sizeof(DocFile *));
                p->files.data[0] = p->main;
                break;
            }
        }
        for (j = 0; j < p->files.len; j++) {
            DocFile *f = p->files.data[j];
            for (k = 0; k < f->ast->nitems; k++) {
                const Item *it = f->ast->items[k];
                DocItem *d;
                StrBuf a = {0};
                if (!doc_documented(p, it))
                    continue;
                d = (DocItem *)xcalloc(1, sizeof *d);
                d->it = it;
                d->file = f;
                d->pkg = p;
                d->name = it->name.name;
                d->full = doc_intern2(p->name, d->name);
                sb_printf(&a, "%s.%s", doc_anchor_kind(it), d->name);
                d->anchor = sb_take(&a);
                vec_push(p->items, d);
                if (!map_has(&ds->index, MAP_PTR(d->full)))
                    map_put(&ds->index, MAP_PTR(d->full), MAP_PTR(d));
            }
            for (k = 0; k < f->ast->nuses; k++) {
                if (f->ast->uses[k]->is_pub) {
                    DocReexport r;
                    r.file = f;
                    r.use = f->ast->uses[k];
                    vec_push(p->reexports, r);
                }
            }
        }
    }
    /* re-exports resolve to the original item */
    for (i = 0; i < ds->pkgs.len; i++) {
        DocPkg *p = ds->pkgs.data[i];
        for (j = 0; j < p->reexports.len; j++) {
            DocFile *f = p->reexports.data[j].file;
            for (k = 0; k < f->aliases.len; k++) {
                uint64_t v;
                const char *as = doc_intern2(p->name, f->aliases.data[k].name);
                if (map_get(&ds->index, MAP_PTR(f->aliases.data[k].full), &v) && !map_has(&ds->index, MAP_PTR(as)))
                    map_put(&ds->index, MAP_PTR(as), v);
            }
        }
    }
    /* tests */
    for (i = 0; i < ds->pkgs.len; i++) {
        DocPkg *p = ds->pkgs.data[i];
        for (j = 0; j < p->files.len; j++) {
            DocFile *f = p->files.data[j];
            for (k = 0; k < f->ast->nitems; k++) {
                if (f->ast->items[k]->kind == ITEM_TEST)
                    doc_attach_test(p, f->ast->items[k]);
            }
        }
    }
}

static DocItem *doc_lookup(DocSet *ds, const char *full)
{
    uint64_t v;
    if (map_get(&ds->index, MAP_PTR(intern_cstr(full)), &v))
        return (DocItem *)(uintptr_t)v;
    return NULL;
}

/* Resolves a path as written in `f` (a, a::b::C) to an item. */
static DocItem *doc_resolve(DocSet *ds, const DocFile *f, const char *path)
{
    const char *sep = strstr(path, "::");
    size_t first_len = sep ? (size_t)(sep - path) : strlen(path);
    const char *first = intern(path, first_len);
    DocItem *d;
    size_t i;
    StrBuf sb = {0};
    if (!f)
        return NULL;
    for (i = 0; i < f->aliases.len; i++) {
        if (f->aliases.data[i].name == first) {
            sb_puts(&sb, f->aliases.data[i].full);
            if (sep)
                sb_puts(&sb, sep);
            d = doc_lookup(ds, sb.data);
            sb_free(&sb);
            if (d)
                return d;
            break;
        }
    }
    sb_reset(&sb);
    sb_printf(&sb, "%s::%s", f->pkg->name, path);
    d = doc_lookup(ds, sb.data);
    for (i = 0; !d && i < f->globs.len; i++) {
        sb_reset(&sb);
        sb_printf(&sb, "%s::%s", f->globs.data[i], path);
        d = doc_lookup(ds, sb.data);
    }
    if (!d) {
        sb_reset(&sb);
        sb_printf(&sb, "std::prelude::%s", path);
        d = doc_lookup(ds, sb.data);
    }
    if (!d && sep)
        d = doc_lookup(ds, path);
    sb_free(&sb);
    return d;
}

/* ------------------------------------------------------------ signatures */

static void doc_gparams_text(DocSet *ds, const DocFile *f, const GenericParams *g, StrBuf *out, int multiline)
{
    uint32_t i;
    if (!g->present)
        return;
    sb_putc(out, '<');
    for (i = 0; i < g->n; i++) {
        const GenericParam *p = &g->items[i];
        if (multiline)
            sb_puts(out, "\n    ");
        else if (i)
            sb_puts(out, ", ");
        sb_printf(out, "%s: ", p->name.name);
        if (p->kind == GENERIC_TYPE) {
            sb_puts(out, "type");
            if (p->default_type)
                sb_printf(out, " = %s", doc_src(ds, f, p->default_type->span));
        } else {
            sb_puts(out, doc_src(ds, f, p->type->span));
            if (p->default_value)
                sb_printf(out, " = %s", doc_src(ds, f, p->default_value->span));
        }
        if (multiline)
            sb_putc(out, ',');
    }
    if (multiline)
        sb_putc(out, '\n');
    sb_putc(out, '>');
}

static void doc_where_text(DocSet *ds, const DocFile *f, const WhereList *w, StrBuf *out)
{
    uint32_t i;
    size_t line_start;
    if (!w->n)
        return;
    sb_puts(out, "\nwhere ");
    line_start = out->len - 6;
    for (i = 0; i < w->n; i++) {
        const char *c = doc_src(ds, f, w->items[i]->span);
        if (i) {
            sb_putc(out, ',');
            if (out->len - line_start + strlen(c) + 2 > 90) {
                sb_puts(out, "\n      ");
                line_start = out->len - 6;
            } else {
                sb_putc(out, ' ');
            }
        }
        sb_puts(out, c);
    }
}

/* Plain-text signature of an item (as written; generic lists over 90
 * columns one per line). */
static void doc_signature(DocSet *ds, const DocItem *d, StrBuf *out)
{
    const Item *it = d->it;
    const DocFile *f = d->file;
    const GenericParams *g = NULL;
    const WhereList *w = NULL;
    size_t start = out->len;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        int multi = pass == 1;
        sb_truncate(out, start);
        if (it->kind == ITEM_PROPERTY && it->u.property.kind != PROP_PLAIN) {
            static const char *const k[] = {"", "assert ", "assume ", "cover "};
            sb_puts(out, k[it->u.property.kind]);
        }
        if (it->is_pub)
            sb_puts(out, "pub ");
        switch (it->kind) {
        case ITEM_MODULE:
            sb_printf(out, "module %s", it->name.name);
            g = &it->u.module.generics;
            w = &it->u.module.where;
            break;
        case ITEM_EXTERN:
            sb_printf(out, "extern module %s(%s)%s", it->name.name, doc_src(ds, f, it->u.extern_mod.file_span),
                      it->u.extern_mod.generics.present ? " " : "");
            g = &it->u.extern_mod.generics;
            w = &it->u.extern_mod.where;
            break;
        case ITEM_BUNDLE:
            sb_printf(out, "bundle %s", it->name.name);
            g = &it->u.bundle.generics;
            break;
        case ITEM_STRUCT:
            sb_printf(out, "struct %s", it->name.name);
            g = &it->u.struct_def.generics;
            break;
        case ITEM_ENUM:
            sb_printf(out, "enum %s", it->name.name);
            g = &it->u.enum_def.generics;
            break;
        case ITEM_TYPE:
            sb_printf(out, "type %s", it->name.name);
            g = &it->u.type_alias.generics;
            break;
        case ITEM_FN:
            sb_printf(out, "%sfn %s", it->u.fn.is_const ? "const " : "", it->name.name);
            g = &it->u.fn.generics;
            w = &it->u.fn.where;
            break;
        case ITEM_CONST:
            sb_printf(out, "const %s", it->name.name);
            break;
        case ITEM_REGMAP:
            sb_printf(out, "regmap %s", it->name.name);
            break;
        case ITEM_PROPERTY:
            sb_printf(out, "property %s", it->name.name);
            break;
        case ITEM_SEQUENCE:
            sb_printf(out, "sequence %s", it->name.name);
            break;
        default:
            sb_puts(out, it->name.name ? it->name.name : "?");
            break;
        }
        if (g)
            doc_gparams_text(ds, f, g, out, multi && g->n > 1);
        if (it->kind == ITEM_FN) {
            uint32_t i;
            int pmulti = multi && it->u.fn.nparams > 1;
            sb_putc(out, '(');
            for (i = 0; i < it->u.fn.nparams; i++) {
                if (pmulti)
                    sb_puts(out, "\n    ");
                else if (i)
                    sb_puts(out, ", ");
                sb_printf(out, "%s: %s", it->u.fn.params[i].name.name, doc_src(ds, f, it->u.fn.params[i].type->span));
                if (pmulti)
                    sb_putc(out, ',');
            }
            sb_puts(out, pmulti ? "\n)" : ")");
            if (it->u.fn.ret)
                sb_printf(out, " -> %s", doc_src(ds, f, it->u.fn.ret->span));
        } else if (it->kind == ITEM_CONST) {
            if (it->u.const_def.type)
                sb_printf(out, ": %s", doc_src(ds, f, it->u.const_def.type->span));
            sb_printf(out, " = %s", doc_src(ds, f, it->u.const_def.value->span));
        } else if (it->kind == ITEM_TYPE) {
            sb_printf(out, " = %s", doc_src(ds, f, it->u.type_alias.type->span));
        } else if (it->kind == ITEM_PROPERTY || it->kind == ITEM_SEQUENCE) {
            const Expr *b = it->kind == ITEM_PROPERTY ? it->u.property.body : it->u.sequence.body;
            sb_printf(out, " { %s }", doc_src(ds, f, b->span));
        }
        if (w)
            doc_where_text(ds, f, w, out);
        {
            /* first line short enough? */
            const char *nl = memchr(out->data + start, '\n', out->len - start);
            size_t first = nl ? (size_t)(nl - (out->data + start)) : out->len - start;
            if (first <= 90)
                break;
        }
    }
    sb_cstr(out);
}

/* ------------------------------------------------------- HTML primitives */

static void doc_esc(StrBuf *out, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '<')
            sb_puts(out, "&lt;");
        else if (c == '>')
            sb_puts(out, "&gt;");
        else if (c == '&')
            sb_puts(out, "&amp;");
        else if (c == '"')
            sb_puts(out, "&quot;");
        else
            sb_putc(out, c);
    }
}

static void doc_escs(StrBuf *out, const char *s)
{
    doc_esc(out, s, strlen(s));
}

static const char *const doc_keywords[] = {
    "package", "use", "pub", "module", "extern", "bundle", "struct", "enum", "const", "fn", "type",
    "regmap", "domain", "where", "test", "property", "sequence", "in", "out", "flip", "let", "var",
    "reg", "mem", "on", "comb", "match", "if", "else", "for", "while", "return", "as", "sim",
    "expect", "assert", "assume", "cover", "and", "or", "not", "xor", "static_assert", "always",
    "never", "eventually", "until", "typeof",
};

static const char *const doc_builtin_types[] = {
    "uint", "sint", "bit", "bool", "clock", "reset", "tri", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
};

static int doc_in_list(const char *s, size_t n, const char *const *list, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (strlen(list[i]) == n && memcmp(list[i], s, n) == 0)
            return 1;
    }
    return 0;
}

static void doc_span(StrBuf *out, const char *cls, const char *s, size_t n)
{
    sb_printf(out, "<span class=\"%s\">", cls);
    doc_esc(out, s, n);
    sb_puts(out, "</span>");
}

/* Writes the page URL of an item relative to the site root. */
static void doc_href(StrBuf *out, const DocItem *d)
{
    sb_printf(out, "%s#%s", d->pkg->page, d->anchor);
}

/* Highlighted Tern code; paths are linked when `f` is given. */
static void doc_code_html(DocSet *ds, const DocFile *f, const char *s, size_t n, StrBuf *out)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            size_t j = i;
            while (j < n && s[j] != '\n')
                j++;
            doc_span(out, "com", s + i, j - i);
            i = j;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/'))
                j++;
            j = j + 2 <= n ? j + 2 : n;
            doc_span(out, "com", s + i, j - i);
            i = j;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < n && s[j] != '"' && s[j] != '\n') {
                if (s[j] == '\\' && j + 1 < n)
                    j++;
                j++;
            }
            if (j < n && s[j] == '"')
                j++;
            doc_span(out, "str", s + i, j - i);
            i = j;
        } else if (tern_is_digit(c)) {
            size_t j = i;
            while (j < n && (tern_is_alnum((unsigned char)s[j]) || s[j] == '_'))
                j++;
            doc_span(out, "num", s + i, j - i);
            i = j;
        } else if (c == '#' && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && tern_is_ident((unsigned char)s[j]))
                j++;
            doc_span(out, "attr", s + i, j - i);
            i = j;
        } else if (tern_is_alpha(c) || c == '_') {
            size_t j = i, seg_end;
            while (j < n && tern_is_ident((unsigned char)s[j]))
                j++;
            seg_end = j;
            /* a path a::b::C */
            while (j + 2 < n && s[j] == ':' && s[j + 1] == ':' && (tern_is_alpha((unsigned char)s[j + 2]) || s[j + 2] == '_')) {
                j += 2;
                while (j < n && tern_is_ident((unsigned char)s[j]))
                    j++;
            }
            if (doc_in_list(s + i, j - i, doc_keywords, TERN_ARRAY_LEN(doc_keywords)) && j == seg_end) {
                doc_span(out, "kw", s + i, j - i);
            } else if ((j - i == 4 && memcmp(s + i, "true", 4) == 0) || (j - i == 5 && memcmp(s + i, "false", 5) == 0) ||
                       (j - i == 4 && (memcmp(s + i, "all0", 4) == 0 || memcmp(s + i, "all1", 4) == 0))) {
                doc_span(out, "num", s + i, j - i);
            } else {
                DocItem *d = NULL;
                const char *last = s + i;
                size_t k;
                if (f) {
                    char *path = xstrndup(s + i, j - i);
                    d = doc_resolve(ds, f, path);
                    free(path);
                }
                for (k = i; k + 1 < j; k++) {
                    if (s[k] == ':' && s[k + 1] == ':')
                        last = s + k + 2;
                }
                if (d) {
                    sb_puts(out, "<a href=\"");
                    doc_href(out, d);
                    sb_puts(out, "\">");
                }
                if (tern_is_upper((unsigned char)*last) ||
                    doc_in_list(last, (size_t)(s + j - last), doc_builtin_types, TERN_ARRAY_LEN(doc_builtin_types))) {
                    doc_esc(out, s + i, (size_t)(last - (s + i)));
                    doc_span(out, "ty", last, (size_t)(s + j - last));
                } else {
                    doc_esc(out, s + i, j - i);
                }
                if (d)
                    sb_puts(out, "</a>");
            }
            i = j;
        } else {
            doc_esc(out, s + i, 1);
            i++;
        }
    }
}

/* ------------------------------------------------------------- Markdown */

typedef enum DocBlockKind { DOC_B_PARA, DOC_B_CODE, DOC_B_HEADING, DOC_B_ITEM } DocBlockKind;

typedef struct DocBlock {
    DocBlockKind kind;
    int level;            /* heading level; list: 1 = ordered */
    StrBuf text;          /* inline text (lines joined by ' ') or code (lines joined by '\n') */
} DocBlock;

typedef VEC(DocBlock) DocBlockVec;

static int doc_list_marker(const char *l, size_t n, size_t *skip, int *ordered)
{
    size_t i = 0;
    while (i < n && l[i] == ' ')
        i++;
    if (i + 1 < n && (l[i] == '*' || l[i] == '-' || l[i] == '+') && l[i + 1] == ' ') {
        *skip = i + 2;
        *ordered = 0;
        return 1;
    }
    {
        size_t j = i;
        while (j < n && tern_is_digit((unsigned char)l[j]))
            j++;
        if (j > i && j + 1 < n && (l[j] == '.' || l[j] == ')') && l[j + 1] == ' ') {
            *skip = j + 2;
            *ordered = 1;
            return 1;
        }
    }
    return 0;
}

static void doc_parse_md(const char *text, DocBlockVec *out)
{
    const char *p = text ? text : "";
    DocBlock *cur = NULL;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        size_t i = 0, skip;
        int ordered;
        while (i < n && p[i] == ' ')
            i++;
        if (n - i >= 3 && memcmp(p + i, "```", 3) == 0) {
            /* fenced code block */
            DocBlock b;
            memset(&b, 0, sizeof b);
            b.kind = DOC_B_CODE;
            p = nl ? nl + 1 : p + n;
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t m = e ? (size_t)(e - p) : strlen(p);
                size_t k = 0;
                while (k < m && p[k] == ' ')
                    k++;
                if (m - k >= 3 && memcmp(p + k, "```", 3) == 0) {
                    p = e ? e + 1 : p + m;
                    break;
                }
                if (b.text.len)
                    sb_putc(&b.text, '\n');
                sb_append(&b.text, p, m);
                p = e ? e + 1 : p + m;
            }
            sb_cstr(&b.text);
            vec_push(*out, b);
            cur = NULL;
            continue;
        }
        if (i == n) {
            cur = NULL;
        } else if (p[i] == '#') {
            DocBlock b;
            size_t k = i;
            memset(&b, 0, sizeof b);
            while (k < n && p[k] == '#')
                k++;
            b.kind = DOC_B_HEADING;
            b.level = (int)(k - i);
            while (k < n && p[k] == ' ')
                k++;
            sb_append(&b.text, p + k, n - k);
            vec_push(*out, b);
            cur = NULL;
        } else if (doc_list_marker(p, n, &skip, &ordered)) {
            DocBlock b;
            memset(&b, 0, sizeof b);
            b.kind = DOC_B_ITEM;
            b.level = ordered;
            sb_append(&b.text, p + skip, n - skip);
            vec_push(*out, b);
            cur = &vec_last(*out);
        } else if (cur && cur->kind == DOC_B_CODE && i >= 4) {
            /* next line of an indented code block */
            sb_putc(&cur->text, '\n');
            sb_append(&cur->text, p + 4, n - 4);
        } else if (cur && (cur->kind == DOC_B_PARA || (cur->kind == DOC_B_ITEM && i >= 2))) {
            sb_putc(&cur->text, ' ');
            sb_append(&cur->text, p + i, n - i);
        } else if (i >= 4 && (!cur || cur->kind != DOC_B_PARA)) {
            /* indented code block */
            DocBlock b;
            memset(&b, 0, sizeof b);
            b.kind = DOC_B_CODE;
            sb_append(&b.text, p + 4, n - 4);
            vec_push(*out, b);
            cur = &vec_last(*out);
        } else {
            DocBlock b;
            memset(&b, 0, sizeof b);
            b.kind = DOC_B_PARA;
            sb_append(&b.text, p + i, n - i);
            vec_push(*out, b);
            cur = &vec_last(*out);
        }
        p = nl ? nl + 1 : p + n;
    }
}

static void doc_free_md(DocBlockVec *v)
{
    size_t i;
    for (i = 0; i < v->len; i++)
        sb_free(&v->data[i].text);
    vec_free(*v);
}

/* Emphasis run of `m` marker characters at s[i]: opens at a word start and
 * is followed by text; finds the matching closing run (preceded by text, not
 * followed by a word character). */
static int doc_emph_close(const char *s, size_t n, size_t i, size_t m, size_t *close)
{
    char c = s[i];
    size_t j;
    if (i > 0 && tern_is_alnum((unsigned char)s[i - 1]))
        return 0;
    if (i + m >= n || s[i + m] == ' ' || s[i + m] == c)
        return 0;
    for (j = i + m + 1; j + m <= n; j++) {
        if (s[j] == '`')
            return 0;
        if (s[j] == c && (m == 1 || s[j + 1] == c) && s[j - 1] != ' ' &&
            (j + m >= n || !tern_is_alnum((unsigned char)s[j + m])) && (m == 2 || j + 1 >= n || s[j + 1] != c)) {
            *close = j;
            return 1;
        }
    }
    return 0;
}

/* Inline Markdown to HTML. */
static void doc_inline_html(DocSet *ds, const DocFile *f, const char *s, StrBuf *out)
{
    size_t i = 0, n = strlen(s);
    while (i < n) {
        char c = s[i];
        if (c == '`') {
            const char *e = strchr(s + i + 1, '`');
            if (e) {
                size_t len = (size_t)(e - (s + i + 1));
                DocItem *d = NULL;
                size_t k;
                int is_path = len > 0;
                for (k = 0; k < len; k++) {
                    char x = s[i + 1 + k];
                    if (!(tern_is_ident((unsigned char)x) || x == ':'))
                        is_path = 0;
                }
                if (is_path && f) {
                    char *p = xstrndup(s + i + 1, len);
                    d = doc_resolve(ds, f, p);
                    free(p);
                }
                if (d) {
                    sb_puts(out, "<a href=\"");
                    doc_href(out, d);
                    sb_puts(out, "\">");
                }
                sb_puts(out, "<code>");
                doc_esc(out, s + i + 1, len);
                sb_puts(out, "</code>");
                if (d)
                    sb_puts(out, "</a>");
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            size_t m = (i + 1 < n && s[i + 1] == c) ? 2 : 1, close;
            if (doc_emph_close(s, n, i, m, &close)) {
                char *tmp = xstrndup(s + i + m, close - i - m);
                sb_puts(out, m == 2 ? "<strong>" : "<em>");
                doc_inline_html(ds, f, tmp, out);
                sb_puts(out, m == 2 ? "</strong>" : "</em>");
                free(tmp);
                i = close + m;
                continue;
            }
        }
        if (c == '[') {
            const char *close = strchr(s + i + 1, ']');
            if (close) {
                size_t tlen = (size_t)(close - (s + i + 1));
                if (close[1] == '(') {
                    const char *pe = strchr(close + 2, ')');
                    if (pe) {
                        sb_puts(out, "<a href=\"");
                        doc_esc(out, close + 2, (size_t)(pe - (close + 2)));
                        sb_puts(out, "\">");
                        doc_esc(out, s + i + 1, tlen);
                        sb_puts(out, "</a>");
                        i = (size_t)(pe - s) + 1;
                        continue;
                    }
                } else if (tlen > 0 && f) {
                    char *p = xstrndup(s + i + 1, tlen);
                    DocItem *d = doc_resolve(ds, f, p);
                    free(p);
                    if (d) {
                        sb_puts(out, "<a href=\"");
                        doc_href(out, d);
                        sb_puts(out, "\"><code>");
                        doc_esc(out, s + i + 1, tlen);
                        sb_puts(out, "</code></a>");
                        i = (size_t)(close - s) + 1;
                        continue;
                    }
                }
            }
        }
        doc_esc(out, &c, 1);
        i++;
    }
}

static void doc_md_html(DocSet *ds, const DocFile *f, const char *text, StrBuf *out)
{
    DocBlockVec blocks = {0};
    size_t i;
    int in_list = -1;
    doc_parse_md(text, &blocks);
    for (i = 0; i < blocks.len; i++) {
        DocBlock *b = &blocks.data[i];
        if (b->kind != DOC_B_ITEM && in_list >= 0) {
            sb_puts(out, in_list ? "</ol>\n" : "</ul>\n");
            in_list = -1;
        }
        switch (b->kind) {
        case DOC_B_PARA:
            sb_puts(out, "<p>");
            doc_inline_html(ds, f, sb_cstr(&b->text), out);
            sb_puts(out, "</p>\n");
            break;
        case DOC_B_HEADING: {
            int lv = b->level + 3 > 6 ? 6 : b->level + 3;
            sb_printf(out, "<h%d>", lv);
            doc_inline_html(ds, f, sb_cstr(&b->text), out);
            sb_printf(out, "</h%d>\n", lv);
            break;
        }
        case DOC_B_CODE:
            sb_puts(out, "<pre class=\"code\"><code>");
            doc_code_html(ds, NULL, b->text.data ? b->text.data : "", b->text.len, out);
            sb_puts(out, "</code></pre>\n");
            break;
        case DOC_B_ITEM:
            if (in_list != b->level) {
                if (in_list >= 0)
                    sb_puts(out, in_list ? "</ol>\n" : "</ul>\n");
                sb_puts(out, b->level ? "<ol>\n" : "<ul>\n");
                in_list = b->level;
            }
            sb_puts(out, "<li>");
            doc_inline_html(ds, f, sb_cstr(&b->text), out);
            sb_puts(out, "</li>\n");
            break;
        }
    }
    if (in_list >= 0)
        sb_puts(out, in_list ? "</ol>\n" : "</ul>\n");
    doc_free_md(&blocks);
}

/* Inline Markdown to plain text: markers dropped, `code` kept. */
static void doc_inline_text(const char *s, StrBuf *out)
{
    size_t i = 0, n = strlen(s);
    while (i < n) {
        char c = s[i];
        if (c == '`') {
            const char *e = strchr(s + i + 1, '`');
            if (e) {
                sb_append(out, s + i, (size_t)(e - (s + i)) + 1);
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            /* emphasis markers are dropped only in matching pairs */
            size_t m = (i + 1 < n && s[i + 1] == c) ? 2 : 1, close;
            if (doc_emph_close(s, n, i, m, &close)) {
                StrBuf inner = {0};
                char *tmp = xstrndup(s + i + m, close - i - m);
                doc_inline_text(tmp, &inner);
                sb_append(out, inner.data ? inner.data : "", inner.len);
                sb_free(&inner);
                free(tmp);
                i = close + m;
                continue;
            }
        }
        if (c == '[') {
            const char *close = strchr(s + i + 1, ']');
            if (close && close[1] == '(' && strchr(close + 2, ')')) {
                const char *pe = strchr(close + 2, ')');
                sb_append(out, s + i + 1, (size_t)(close - (s + i + 1)));
                sb_puts(out, " (");
                sb_append(out, close + 2, (size_t)(pe - (close + 2)));
                sb_putc(out, ')');
                i = (size_t)(pe - s) + 1;
                continue;
            }
            if (close && close > s + i + 1 && (i == 0 || s[i - 1] != '#')) { /* not #[attr] */
                size_t k;
                int ident = 1;
                for (k = i + 1; s + k < close; k++) {
                    if (!(tern_is_ident((unsigned char)s[k]) || s[k] == ':'))
                        ident = 0;
                }
                if (ident) {
                    sb_append(out, s + i + 1, (size_t)(close - (s + i + 1)));
                    i = (size_t)(close - s) + 1;
                    continue;
                }
            }
        }
        sb_putc(out, c);
        i++;
    }
    sb_cstr(out);
}

/* Word-wraps `text` to `width` columns; the first line is prefixed with
 * `first`, the others with `indent` spaces. */
static void doc_wrap_at(const char *text, int width, int col, int indent, StrBuf *out)
{
    const char *p = text;
    int nwords = 0;
    while (*p) {
        const char *w;
        int wl;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        w = p;
        while (*p && *p != ' ')
            p++;
        wl = (int)utf8_count(w, (size_t)(p - w));
        if (nwords > 0 && col + 1 + wl > width) {
            sb_putc(out, '\n');
            sb_indent(out, indent);
            col = indent;
        } else if (nwords > 0) {
            sb_putc(out, ' ');
            col++;
        }
        sb_append(out, w, (size_t)(p - w));
        col += wl;
        nwords++;
    }
    sb_putc(out, '\n');
}

static void doc_wrap(const char *text, int width, const char *first, int indent, StrBuf *out)
{
    sb_puts(out, first);
    doc_wrap_at(text, width, (int)utf8_count(first, strlen(first)), indent, out);
}

static void doc_md_text(const char *text, int width, int indent, StrBuf *out)
{
    DocBlockVec blocks = {0};
    size_t i;
    StrBuf pre = {0}, line = {0};
    int prev_item = 0;
    doc_parse_md(text, &blocks);
    for (i = 0; i < blocks.len; i++) {
        DocBlock *b = &blocks.data[i];
        sb_reset(&line);
        if (i > 0 && !(b->kind == DOC_B_ITEM && prev_item))
            sb_putc(out, '\n');
        prev_item = b->kind == DOC_B_ITEM;
        sb_reset(&pre);
        sb_indent(&pre, indent);
        switch (b->kind) {
        case DOC_B_PARA:
            doc_inline_text(sb_cstr(&b->text), &line);
            doc_wrap(line.data, width, sb_cstr(&pre), indent, out);
            break;
        case DOC_B_HEADING:
            doc_inline_text(sb_cstr(&b->text), &line);
            doc_wrap(line.data, width, sb_cstr(&pre), indent, out);
            break;
        case DOC_B_ITEM:
            doc_inline_text(sb_cstr(&b->text), &line);
            sb_puts(&pre, b->level ? "- " : "* ");
            doc_wrap(line.data, width, sb_cstr(&pre), indent + 2, out);
            break;
        case DOC_B_CODE: {
            const char *p = b->text.data ? b->text.data : "";
            while (*p) {
                const char *e = strchr(p, '\n');
                size_t m = e ? (size_t)(e - p) : strlen(p);
                sb_indent(out, indent + 4);
                sb_append(out, p, m);
                sb_putc(out, '\n');
                p = e ? e + 1 : p + m;
            }
            break;
        }
        }
    }
    sb_free(&pre);
    sb_free(&line);
    doc_free_md(&blocks);
}

/* --------------------------------------------------------- item details */

/* Package description: the comment block right after the `package` line
 * of the package's main file (separator lines of dashes are skipped). */
static char *doc_pkg_text(DocSet *ds, const DocPkg *p)
{
    const DocFile *f = p->main;
    StrBuf sb = {0};
    uint32_t i, last_line = 0;
    char *r;
    if (!f || !f->ast->has_package)
        return NULL;
    for (i = 0; i < f->ast->ncomments; i++) {
        const Comment *c = &f->ast->comments[i];
        uint32_t s, e;
        int dashes = 1;
        if (c->span.off < doc_end(f->ast->package_span))
            continue;
        if (c->kind != COMMENT_LINE)
            break;
        if (last_line == 0) {
            /* must start right after the package line (one blank line allowed) */
            if (c->span.line > f->ast->package_span.line + 2)
                break;
            if (f->ast->ntokens > 1) {
                uint32_t t;
                int token_between = 0;
                for (t = 0; t < f->ast->ntokens; t++) {
                    if (f->ast->tokens[t].off > doc_end(f->ast->package_span) && f->ast->tokens[t].off < c->span.off &&
                        f->ast->tokens[t].kind != TK_EOF)
                        token_between = 1;
                }
                if (token_between)
                    break;
            }
        } else if (c->span.line != last_line + 1) {
            break;
        }
        last_line = c->span.line;
        s = c->span.off + 2;
        e = doc_end(c->span);
        if (s < e && f->src[s] == ' ')
            s++;
        {
            uint32_t k;
            for (k = s; k < e; k++) {
                if (f->src[k] != '-' && f->src[k] != '=' && f->src[k] != ' ')
                    dashes = 0;
            }
        }
        if (dashes && e > s)
            continue;
        sb_append(&sb, f->src + s, e - s);
        sb_putc(&sb, '\n');
    }
    if (!sb.len)
        return NULL;
    {
        /* "std::fifo — buffering." -> "Buffering." */
        static const char *const seps[] = {" \xE2\x80\x94 ", " -- ", " - ", ": "};
        size_t n = strlen(p->name), k, skip = 0;
        if (sb.len > n && memcmp(sb.data, p->name, n) == 0) {
            for (k = 0; k < TERN_ARRAY_LEN(seps); k++) {
                size_t m = strlen(seps[k]);
                if (sb.len > n + m && memcmp(sb.data + n, seps[k], m) == 0) {
                    skip = n + m;
                    break;
                }
            }
        }
        r = arena_strndup(&ds->arena, sb.data + skip, sb.len - skip);
        if (skip)
            r[0] = (char)tern_to_upper((unsigned char)r[0]);
    }
    sb_free(&sb);
    return r;
}

/* Statements of a module body that the docs list. */
static int doc_is_pub_const(const Stmt *s)
{
    return s->kind == STMT_ITEM && s->u.item->kind == ITEM_CONST && s->u.item->is_pub;
}

static int doc_is_property(const Stmt *s)
{
    return s->kind == STMT_ITEM && (s->u.item->kind == ITEM_PROPERTY || s->u.item->kind == ITEM_SEQUENCE);
}

static void doc_body(const Item *it, Stmt ***body, uint32_t *n)
{
    *body = NULL;
    *n = 0;
    if (it->kind == ITEM_MODULE) {
        *body = it->u.module.body;
        *n = it->u.module.nbody;
    } else if (it->kind == ITEM_EXTERN) {
        *body = it->u.extern_mod.body;
        *n = it->u.extern_mod.nbody;
    }
}

static const GenericParams *doc_generics(const Item *it)
{
    switch (it->kind) {
    case ITEM_MODULE: return &it->u.module.generics;
    case ITEM_EXTERN: return &it->u.extern_mod.generics;
    case ITEM_BUNDLE: return &it->u.bundle.generics;
    case ITEM_STRUCT: return &it->u.struct_def.generics;
    case ITEM_ENUM: return &it->u.enum_def.generics;
    case ITEM_TYPE: return &it->u.type_alias.generics;
    case ITEM_FN: return &it->u.fn.generics;
    default: return NULL;
    }
}

static const char *doc_text_of(const AstDoc *d)
{
    return d ? d->text : NULL;
}

/* ------------------------------------------------------------ text mode */

static int doc_cols(const char *s)
{
    return (int)utf8_count(s, strlen(s));
}

static void doc_pad(StrBuf *out, int n)
{
    if (n > 0)
        sb_indent(out, n);
}

/* A table row: fixed columns, then the doc wrapped into the rest (or on
 * the next line when the columns are too wide). */
static void doc_text_row(StrBuf *out, const char *const *cells, const int *widths, int ncells, const char *doc,
                         int width)
{
    int i, col = 2;
    StrBuf line = {0};
    sb_indent(out, 2);
    for (i = 0; i < ncells; i++) {
        sb_puts(out, cells[i]);
        col += doc_cols(cells[i]);
        if (i + 1 < ncells || doc) {
            int pad = widths[i] - doc_cols(cells[i]) + 2;
            if (pad < 1)
                pad = 1;
            doc_pad(out, pad);
            col += pad;
        }
    }
    if (doc && doc[0]) {
        char *sum;
        doc_inline_text(doc, &line);
        sum = line.data;
        if (width - col >= 24) {
            /* drop the trailing spaces we just wrote and wrap */
            doc_wrap_at(sum, width, col, col, out);
        } else {
            while (out->len && out->data[out->len - 1] == ' ')
                out->len--;
            sb_putc(out, '\n');
            sb_indent(out, 6);
            doc_wrap(sum, width, "", 6, out);
        }
    } else {
        while (out->len && out->data[out->len - 1] == ' ')
            out->len--;
        sb_putc(out, '\n');
    }
    sb_free(&line);
}

/* Doc text of table rows in text mode: first paragraph, joined to a line. */
static char *doc_row_text(DocSet *ds, const char *doc)
{
    StrBuf sb = {0};
    const char *p;
    char *r;
    if (!doc)
        return NULL;
    for (p = doc; *p; p++) {
        if (*p == '\n') {
            if (p[1] == '\n')
                break;
            sb_putc(&sb, ' ');
        } else {
            sb_putc(&sb, *p);
        }
    }
    r = arena_strndup(&ds->arena, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return r;
}

static void doc_text_item(DocSet *ds, const DocItem *d, int width, StrBuf *out)
{
    const Item *it = d->it;
    const DocFile *f = d->file;
    StrBuf sig = {0};
    const GenericParams *g = doc_generics(it);
    Stmt **body;
    uint32_t nbody, i;
    int w[4];
    doc_signature(ds, d, &sig);
    sb_printf(out, "%s %s\n\n", doc_kind_name(it), d->full);
    {
        const char *p = sig.data;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t m = e ? (size_t)(e - p) : strlen(p);
            sb_indent(out, 4);
            sb_append(out, p, m);
            sb_putc(out, '\n');
            p = e ? e + 1 : p + m;
        }
    }
    if (it->doc) {
        sb_putc(out, '\n');
        doc_md_text(it->doc->text, width, 0, out);
    }
    if (g && g->present && g->n) {
        sb_puts(out, "\nGenerics:\n");
        char **kinds = (char **)xcalloc(g->n, sizeof(char *));
        w[0] = w[1] = 0;
        for (i = 0; i < g->n; i++) {
            const GenericParam *p = &g->items[i];
            StrBuf kind = {0};
            sb_puts(&kind, p->kind == GENERIC_TYPE ? "type" : doc_src(ds, f, p->type->span));
            if (p->default_type)
                sb_printf(&kind, " = %s", doc_src(ds, f, p->default_type->span));
            if (p->default_value)
                sb_printf(&kind, " = %s", doc_src(ds, f, p->default_value->span));
            kinds[i] = sb_take(&kind);
            w[0] = TERN_MAX(w[0], doc_cols(p->name.name));
            w[1] = TERN_MAX(w[1], doc_cols(kinds[i]));
        }
        w[1] = TERN_MIN(w[1], 28);
        for (i = 0; i < g->n; i++) {
            const GenericParam *p = &g->items[i];
            const char *cells[2];
            char *gdoc = doc_comment_before(ds, f, p->name.span.off);
            cells[0] = p->name.name;
            cells[1] = kinds[i];
            doc_text_row(out, cells, w, 2, doc_row_text(ds, gdoc), width);
            free(kinds[i]);
        }
        free(kinds);
    }
    doc_body(it, &body, &nbody);
    if (nbody) {
        int any = 0;
        w[0] = 3;
        w[1] = w[2] = 0;
        for (i = 0; i < nbody; i++) {
            const Stmt *s = body[i];
            if (s->kind != STMT_PORT)
                continue;
            any = 1;
            w[1] = TERN_MAX(w[1], doc_cols(s->u.port.name.name));
            w[2] = TERN_MAX(w[2], doc_cols(doc_src(ds, f, s->u.port.type->span)) +
                                      (s->u.port.domain ? 3 + doc_cols(doc_src(ds, f, s->u.port.domain->span)) : 0));
        }
        w[2] = TERN_MIN(w[2], 28);
        if (any) {
            sb_puts(out, "\nPorts:\n");
            for (i = 0; i < nbody; i++) {
                const Stmt *s = body[i];
                StrBuf ty = {0};
                const char *cells[3];
                if (s->kind != STMT_PORT)
                    continue;
                sb_puts(&ty, doc_src(ds, f, s->u.port.type->span));
                if (s->u.port.domain)
                    sb_printf(&ty, " @ %s", doc_src(ds, f, s->u.port.domain->span));
                cells[0] = s->u.port.dir == PORT_IN ? "in" : "out";
                cells[1] = s->u.port.name.name;
                cells[2] = ty.data;
                doc_text_row(out, cells, w, 3, doc_row_text(ds, doc_text_of(s->doc)), width);
                sb_free(&ty);
            }
        }
        any = 0;
        for (i = 0; i < nbody; i++) {
            const Stmt *s = body[i];
            const Item *c;
            if (!doc_is_pub_const(s))
                continue;
            c = s->u.item;
            if (!any)
                sb_puts(out, "\nConstants:\n");
            any = 1;
            sb_printf(out, "  %s", c->name.name);
            if (c->u.const_def.type)
                sb_printf(out, ": %s", doc_src(ds, f, c->u.const_def.type->span));
            sb_printf(out, " = %s\n", doc_src(ds, f, c->u.const_def.value->span));
            if (c->doc) {
                StrBuf l = {0};
                doc_inline_text(doc_row_text(ds, c->doc->text), &l);
                doc_wrap(l.data, width, "      ", 6, out);
                sb_free(&l);
            }
        }
        any = 0;
        w[0] = 0;
        for (i = 0; i < nbody; i++) {
            if (doc_is_property(body[i]))
                w[0] = TERN_MAX(w[0], doc_cols(body[i]->u.item->name.name));
        }
        w[0] = TERN_MIN(w[0], 28);
        for (i = 0; i < nbody; i++) {
            const char *cells[1];
            const Item *pr;
            const char *txt;
            if (!doc_is_property(body[i]))
                continue;
            pr = body[i]->u.item;
            if (!any)
                sb_puts(out, "\nProperties:\n");
            any = 1;
            cells[0] = pr->name.name;
            txt = doc_row_text(ds, doc_text_of(pr->doc));
            if (!txt) {
                /* no description: show what it states */
                static const char *const k[] = {"", "assert: ", "assume: ", "cover: "};
                StrBuf e = {0};
                if (pr->kind == ITEM_PROPERTY)
                    sb_puts(&e, k[pr->u.property.kind]);
                sb_puts(&e, "`");
                sb_puts(&e, doc_src(ds, f, pr->kind == ITEM_PROPERTY ? pr->u.property.body->span
                                                                      : pr->u.sequence.body->span));
                sb_puts(&e, "`");
                txt = arena_strdup(&ds->arena, e.data);
                sb_free(&e);
            }
            doc_text_row(out, cells, w, 1, txt, width);
        }
    }
    if (it->kind == ITEM_STRUCT || it->kind == ITEM_BUNDLE) {
        Field **fields = it->kind == ITEM_STRUCT ? it->u.struct_def.fields : it->u.bundle.fields;
        uint32_t nf = it->kind == ITEM_STRUCT ? it->u.struct_def.nfields : it->u.bundle.nfields;
        if (nf) {
            sb_puts(out, "\nFields:\n");
            w[0] = w[1] = 0;
            for (i = 0; i < nf; i++) {
                w[0] = TERN_MAX(w[0], doc_cols(fields[i]->name.name) + (fields[i]->flip ? 5 : 0));
                w[1] = TERN_MAX(w[1], doc_cols(doc_src(ds, f, fields[i]->type->span)));
            }
            w[1] = TERN_MIN(w[1], 28);
            for (i = 0; i < nf; i++) {
                StrBuf nm = {0};
                const char *cells[2];
                if (fields[i]->flip)
                    sb_puts(&nm, "flip ");
                sb_puts(&nm, fields[i]->name.name);
                cells[0] = nm.data;
                cells[1] = doc_src(ds, f, fields[i]->type->span);
                doc_text_row(out, cells, w, 2, doc_row_text(ds, doc_text_of(fields[i]->doc)), width);
                sb_free(&nm);
            }
        }
        if (it->kind == ITEM_BUNDLE && it->u.bundle.nfns) {
            sb_puts(out, "\nMethods:\n");
            for (i = 0; i < it->u.bundle.nfns; i++) {
                const Item *fn = it->u.bundle.fns[i];
                DocItem tmp;
                StrBuf s2 = {0};
                memset(&tmp, 0, sizeof tmp);
                tmp.it = fn;
                tmp.file = d->file;
                tmp.pkg = d->pkg;
                doc_signature(ds, &tmp, &s2);
                sb_printf(out, "  %s\n", s2.data);
                if (fn->doc) {
                    StrBuf l = {0};
                    doc_inline_text(doc_row_text(ds, fn->doc->text), &l);
                    doc_wrap(l.data, width, "      ", 6, out);
                    sb_free(&l);
                }
                sb_free(&s2);
            }
        }
    }
    if (it->kind == ITEM_ENUM && it->u.enum_def.nvariants) {
        sb_puts(out, "\nVariants:\n");
        w[0] = 0;
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            const Variant *v = it->u.enum_def.variants[i];
            int vw = doc_cols(v->name.name);
            if (v->has_fields)
                vw += (v->angle ? 0 : 1) + doc_cols(doc_src(ds, f, span_make(0, doc_end(v->name.span), doc_end(v->span) - doc_end(v->name.span))));
            w[0] = TERN_MAX(w[0], vw);
        }
        w[0] = TERN_MIN(w[0], 36);
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            const Variant *v = it->u.enum_def.variants[i];
            StrBuf nm = {0};
            const char *cells[1];
            sb_puts(&nm, v->name.name);
            if (v->has_fields) {
                Span sp = v->span;
                sp.off = doc_end(v->name.span);
                sp.len = doc_end(v->span) - sp.off;
                sb_printf(&nm, v->angle ? "%s" : " %s", doc_src(ds, f, sp));
            }
            cells[0] = nm.data;
            doc_text_row(out, cells, w, 1, doc_row_text(ds, doc_text_of(v->doc)), width);
            sb_free(&nm);
        }
    }
    if (it->kind == ITEM_REGMAP && it->u.regmap.nentries) {
        sb_puts(out, "\nRegisters:\n");
        for (i = 0; i < it->u.regmap.nentries; i++) {
            const RegEntry *r = it->u.regmap.entries[i];
            StrBuf l = {0};
            sb_printf(&l, "%s  %s: %s ", doc_src(ds, f, r->offset->span), r->name.name, r->access.name);
            if (r->kind == REG_FIELDS) {
                uint32_t j;
                sb_putc(&l, '{');
                for (j = 0; j < r->nfields; j++) {
                    const RegField *rf = r->fields[j];
                    sb_printf(&l, "%s %s: %s @", j ? "," : "", rf->name.name, doc_src(ds, f, rf->type->span));
                    if (rf->bit_lo)
                        sb_printf(&l, "[%s:%s]", doc_src(ds, f, rf->bit_hi->span), doc_src(ds, f, rf->bit_lo->span));
                    else
                        sb_puts(&l, doc_src(ds, f, rf->bit_hi->span));
                }
                sb_puts(&l, " }");
            } else {
                sb_puts(&l, doc_src(ds, f, r->type->span));
                if (r->kind == REG_STREAM)
                    sb_printf(&l, " -> %s", doc_src(ds, f, r->stream_type->span));
            }
            if (r->reset)
                sb_printf(&l, " = %s", doc_src(ds, f, r->reset->span));
            sb_printf(out, "  %s\n", l.data);
            if (r->doc) {
                StrBuf t = {0};
                doc_inline_text(doc_row_text(ds, r->doc->text), &t);
                doc_wrap(t.data, width, "      ", 6, out);
                sb_free(&t);
            }
            sb_free(&l);
        }
    }
    if (d->tests.len) {
        sb_puts(out, "\nTested behaviours:\n");
        for (i = 0; i < d->tests.len; i++) {
            StrBuf t = {0};
            sb_puts(&t, d->tests.data[i]->u.test.name ? d->tests.data[i]->u.test.name : "");
            doc_wrap(t.data, width, "  * ", 4, out);
            sb_free(&t);
        }
    }
    sb_free(&sig);
}

static void doc_text_pkg(DocSet *ds, const DocPkg *p, int width, StrBuf *out)
{
    char *text = doc_pkg_text(ds, p);
    size_t s, i;
    sb_printf(out, "package %s\n", p->name);
    if (text) {
        sb_putc(out, '\n');
        doc_md_text(text, width, 0, out);
    }
    for (s = 0; s < TERN_ARRAY_LEN(doc_sections); s++) {
        int any = 0;
        for (i = 0; i < p->items.len; i++) {
            const DocItem *d = p->items.data[i];
            StrBuf sig = {0};
            if (d->it->kind != doc_sections[s].kind)
                continue;
            if (!any)
                sb_printf(out, "\n%s:\n", doc_sections[s].title);
            any = 1;
            doc_signature(ds, d, &sig);
            {
                /* first line of the signature only */
                char *nl = strchr(sig.data, '\n');
                if (nl)
                    *nl = '\0';
            }
            sb_printf(out, "  %s\n", sig.data);
            if (d->it->doc) {
                StrBuf l = {0};
                doc_inline_text(doc_summary(ds, d->it->doc->text), &l);
                doc_wrap(l.data, width, "      ", 6, out);
                sb_free(&l);
            }
            sb_free(&sig);
        }
    }
    if (p->reexports.len) {
        sb_puts(out, "\nRe-exports:\n");
        for (i = 0; i < p->reexports.len; i++) {
            const DocReexport *r = &p->reexports.data[i];
            sb_printf(out, "  pub use %s\n", doc_src(ds, r->file, r->use->tree->span));
        }
    }
    sb_printf(out, "\nRun `tern doc %s::<Item>` for the details of an item.\n", p->name);
}

static int doc_edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i, j;
    size_t row[64];
    if (lb >= TERN_ARRAY_LEN(row))
        return 99;
    for (j = 0; j <= lb; j++)
        row[j] = j;
    for (i = 1; i <= la; i++) {
        size_t prev = row[0];
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t cur = row[j];
            size_t sub = prev + (tern_to_lower((unsigned char)a[i - 1]) == tern_to_lower((unsigned char)b[j - 1]) ? 0 : 1);
            size_t del = row[j] + 1, ins = row[j - 1] + 1;
            row[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
            prev = cur;
        }
    }
    return (int)row[lb];
}

/* Text rendering of a package (`std::fifo`), an item (`std::fifo::Fifo`), or
 * the list of packages (`std`, or NULL for all). Returns 0, or -1 when
 * nothing matches (a message with suggestions is appended to `out`). */
int doc_render_text(DocSet *ds, const char *what, int width, StrBuf *out)
{
    size_t i;
    const char *best = NULL;
    int best_d = 99;
    doc_set_finish(ds);
    if (width < 40)
        width = 40;
    if (!what || strcmp(what, "std") == 0) {
        sb_puts(out, what ? "Packages of std:\n\n" : "Packages:\n\n");
        for (i = 0; i < ds->pkgs.len; i++) {
            const DocPkg *p = ds->pkgs.data[i];
            char *t;
            if (what && !p->is_std)
                continue;
            t = doc_pkg_text(ds, p);
            sb_printf(out, "  %s\n", p->name);
            if (t) {
                StrBuf l = {0};
                doc_inline_text(doc_summary(ds, t), &l);
                doc_wrap(l.data, width, "      ", 6, out);
                sb_free(&l);
            }
        }
        sb_puts(out, "\nRun `tern doc <package>` for a package, `tern doc <package>::<Item>` for an item.\n");
        return 0;
    }
    for (i = 0; i < ds->pkgs.len; i++) {
        if (strcmp(ds->pkgs.data[i]->name, what) == 0) {
            doc_text_pkg(ds, ds->pkgs.data[i], width, out);
            return 0;
        }
    }
    {
        DocItem *d = doc_lookup(ds, what), *only = NULL;
        int count = 0;
        if (!d && !strstr(what, "::")) {
            /* a bare item name that is unique */
            for (i = 0; i < ds->pkgs.len; i++) {
                size_t j;
                for (j = 0; j < ds->pkgs.data[i]->items.len; j++) {
                    if (strcmp(ds->pkgs.data[i]->items.data[j]->name, what) == 0) {
                        only = ds->pkgs.data[i]->items.data[j];
                        count++;
                    }
                }
            }
            if (count == 1)
                d = only;
        }
        if (d) {
            doc_text_item(ds, d, width, out);
            return 0;
        }
    }
    /* suggestions */
    for (i = 0; i < ds->pkgs.len; i++) {
        size_t j;
        const DocPkg *p = ds->pkgs.data[i];
        int dd = doc_edit_distance(what, p->name);
        if (dd < best_d) {
            best_d = dd;
            best = p->name;
        }
        for (j = 0; j < p->items.len; j++) {
            dd = doc_edit_distance(what, p->items.data[j]->full);
            if (dd < best_d) {
                best_d = dd;
                best = p->items.data[j]->full;
            }
            /* the bare item name anywhere */
            if (strcmp(what, p->items.data[j]->name) == 0) {
                best_d = 0;
                best = p->items.data[j]->full;
            }
        }
    }
    sb_printf(out, "no package or item `%s`", what);
    if (best && best_d <= 3 + (int)strlen(what) / 4)
        sb_printf(out, "; did you mean `%s`?", best);
    sb_putc(out, '\n');
    return -1;
}

/* ------------------------------------------------------------ HTML pages */

static void doc_page_head(StrBuf *out, const char *title)
{
    sb_puts(out, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    doc_escs(out, title);
    sb_puts(out, "</title>\n<link rel=\"stylesheet\" href=\"style.css\">\n</head>\n<body>\n"
                 "<header class=\"top\"><a class=\"brand\" href=\"index.html\">Tern docs</a>"
                 "<div class=\"search\"><input id=\"search\" type=\"search\" placeholder=\"Search items\" "
                 "autocomplete=\"off\" aria-label=\"Search items\"><ul id=\"results\" hidden></ul></div></header>\n");
}

static void doc_page_tail(StrBuf *out)
{
    sb_puts(out, "<footer>Generated by <code>tern doc</code> " TERN_VERSION ".</footer>\n"
                 "<script src=\"search-index.js\"></script>\n<script src=\"search.js\"></script>\n</body>\n</html>\n");
}

static void doc_sidebar(DocSet *ds, const DocPkg *cur, StrBuf *out)
{
    size_t i, s;
    int std_open = 0;
    sb_puts(out, "<nav class=\"side\">\n");
    if (cur) {
        sb_printf(out, "<h2>%s</h2>\n", "Contents");
        for (s = 0; s < TERN_ARRAY_LEN(doc_sections); s++) {
            int any = 0;
            for (i = 0; i < cur->items.len; i++) {
                const DocItem *d = cur->items.data[i];
                if (d->it->kind != doc_sections[s].kind)
                    continue;
                if (!any)
                    sb_printf(out, "<h3>%s</h3>\n<ul>\n", doc_sections[s].title);
                any = 1;
                sb_printf(out, "<li><a href=\"#%s\">", d->anchor);
                doc_escs(out, d->name);
                sb_puts(out, "</a></li>\n");
            }
            if (any)
                sb_puts(out, "</ul>\n");
        }
    }
    sb_puts(out, "<h2>Packages</h2>\n<ul>\n");
    for (i = 0; i < ds->pkgs.len; i++) {
        const DocPkg *p = ds->pkgs.data[i];
        if (p->is_std && !std_open) {
            sb_puts(out, "</ul>\n<h3>Standard library</h3>\n<ul>\n");
            std_open = 1;
        }
        sb_printf(out, "<li%s><a href=\"%s\">", p == cur ? " class=\"cur\"" : "", p->page);
        doc_escs(out, p->name);
        sb_puts(out, "</a></li>\n");
    }
    sb_puts(out, "</ul>\n</nav>\n");
}

static void doc_html_code(DocSet *ds, const DocFile *f, const char *text, StrBuf *out)
{
    doc_code_html(ds, f, text, strlen(text), out);
}

static void doc_html_doc_cell(DocSet *ds, const DocFile *f, const char *doc, StrBuf *out)
{
    sb_puts(out, "<td class=\"doc\">");
    if (doc)
        doc_md_html(ds, f, doc, out);
    sb_puts(out, "</td>");
}

static void doc_html_item(DocSet *ds, const DocItem *d, StrBuf *out)
{
    const Item *it = d->it;
    const DocFile *f = d->file;
    StrBuf sig = {0};
    const GenericParams *g = doc_generics(it);
    Stmt **body;
    uint32_t nbody, i;
    doc_signature(ds, d, &sig);
    sb_printf(out, "<section class=\"item\" id=\"%s\">\n<h2><span class=\"kind\">%s</span> ", d->anchor,
              doc_kind_name(it));
    doc_escs(out, d->name);
    sb_printf(out, "<a class=\"anchor\" href=\"#%s\" aria-label=\"link to %s\">#</a></h2>\n", d->anchor, d->name);
    sb_puts(out, "<pre class=\"sig\"><code>");
    doc_html_code(ds, f, sig.data, out);
    sb_puts(out, "</code></pre>\n");
    if (it->doc) {
        sb_puts(out, "<div class=\"doc\">\n");
        doc_md_html(ds, f, it->doc->text, out);
        sb_puts(out, "</div>\n");
    }
    if (g && g->present && g->n) {
        sb_puts(out, "<h3>Generics</h3>\n<table class=\"generics\">\n<thead><tr><th>Name</th><th>Kind</th>"
                     "<th>Default</th><th>Description</th></tr></thead>\n<tbody>\n");
        for (i = 0; i < g->n; i++) {
            const GenericParam *p = &g->items[i];
            sb_puts(out, "<tr><td><code>");
            doc_escs(out, p->name.name);
            sb_puts(out, "</code></td><td><code>");
            if (p->kind == GENERIC_TYPE)
                sb_puts(out, "<span class=\"kw\">type</span>");
            else
                doc_html_code(ds, f, doc_src(ds, f, p->type->span), out);
            sb_puts(out, "</code></td><td>");
            if (p->default_type || p->default_value) {
                sb_puts(out, "<code>");
                doc_html_code(ds, f, doc_src(ds, f, p->default_type ? p->default_type->span : p->default_value->span), out);
                sb_puts(out, "</code>");
            }
            sb_puts(out, "</td>");
            doc_html_doc_cell(ds, f, doc_comment_before(ds, f, p->name.span.off), out);
            sb_puts(out, "</tr>\n");
        }
        sb_puts(out, "</tbody>\n</table>\n");
    }
    doc_body(it, &body, &nbody);
    if (nbody) {
        int any = 0;
        for (i = 0; i < nbody; i++) {
            const Stmt *s = body[i];
            if (s->kind != STMT_PORT)
                continue;
            if (!any)
                sb_puts(out, "<h3>Ports</h3>\n<table class=\"ports\">\n<thead><tr><th>Name</th><th>Dir</th>"
                             "<th>Type</th><th>Domain</th><th>Description</th></tr></thead>\n<tbody>\n");
            any = 1;
            sb_puts(out, "<tr><td><code>");
            doc_escs(out, s->u.port.name.name);
            sb_printf(out, "</code></td><td class=\"dir %s\">%s</td><td><code>",
                      s->u.port.dir == PORT_IN ? "in" : "out", s->u.port.dir == PORT_IN ? "in" : "out");
            doc_html_code(ds, f, doc_src(ds, f, s->u.port.type->span), out);
            sb_puts(out, "</code></td><td>");
            if (s->u.port.domain) {
                sb_puts(out, "<code>");
                doc_html_code(ds, f, doc_src(ds, f, s->u.port.domain->span), out);
                sb_puts(out, "</code>");
            }
            sb_puts(out, "</td>");
            doc_html_doc_cell(ds, f, doc_text_of(s->doc), out);
            sb_puts(out, "</tr>\n");
        }
        if (any)
            sb_puts(out, "</tbody>\n</table>\n");
        any = 0;
        for (i = 0; i < nbody; i++) {
            const Stmt *s = body[i];
            const Item *c;
            if (!doc_is_pub_const(s))
                continue;
            c = s->u.item;
            if (!any)
                sb_puts(out, "<h3>Published constants</h3>\n<table class=\"consts\">\n<thead><tr><th>Name</th>"
                             "<th>Type</th><th>Value</th><th>Description</th></tr></thead>\n<tbody>\n");
            any = 1;
            sb_puts(out, "<tr><td><code>");
            doc_escs(out, c->name.name);
            sb_puts(out, "</code></td><td><code>");
            if (c->u.const_def.type)
                doc_html_code(ds, f, doc_src(ds, f, c->u.const_def.type->span), out);
            sb_puts(out, "</code></td><td><code>");
            doc_html_code(ds, f, doc_src(ds, f, c->u.const_def.value->span), out);
            sb_puts(out, "</code></td>");
            doc_html_doc_cell(ds, f, doc_text_of(c->doc), out);
            sb_puts(out, "</tr>\n");
        }
        if (any)
            sb_puts(out, "</tbody>\n</table>\n");
        any = 0;
        for (i = 0; i < nbody; i++) {
            const Item *pr;
            if (!doc_is_property(body[i]))
                continue;
            pr = body[i]->u.item;
            if (!any)
                sb_puts(out, "<h3>Properties</h3>\n<table class=\"props\">\n<thead><tr><th>Name</th>"
                             "<th>Property</th><th>Description</th></tr></thead>\n<tbody>\n");
            any = 1;
            sb_puts(out, "<tr><td><code>");
            doc_escs(out, pr->name.name);
            sb_puts(out, "</code></td><td><code>");
            {
                static const char *const k[] = {"", "assert ", "assume ", "cover "};
                if (pr->kind == ITEM_PROPERTY && pr->u.property.kind != PROP_PLAIN) {
                    sb_printf(out, "<span class=\"kw\">%s</span>", k[pr->u.property.kind]);
                }
                doc_html_code(ds, f,
                              doc_src(ds, f, pr->kind == ITEM_PROPERTY ? pr->u.property.body->span
                                                                       : pr->u.sequence.body->span),
                              out);
            }
            sb_puts(out, "</code></td>");
            doc_html_doc_cell(ds, f, doc_text_of(pr->doc), out);
            sb_puts(out, "</tr>\n");
        }
        if (any)
            sb_puts(out, "</tbody>\n</table>\n");
    }
    if (it->kind == ITEM_STRUCT || it->kind == ITEM_BUNDLE) {
        int bundle = it->kind == ITEM_BUNDLE;
        Field **fields = bundle ? it->u.bundle.fields : it->u.struct_def.fields;
        uint32_t nf = bundle ? it->u.bundle.nfields : it->u.struct_def.nfields;
        if (nf) {
            sb_printf(out, "<h3>Fields</h3>\n<table class=\"fields\">\n<thead><tr><th>Name</th>%s<th>Type</th>"
                           "<th>Description</th></tr></thead>\n<tbody>\n",
                      bundle ? "<th>Flip</th>" : "");
            for (i = 0; i < nf; i++) {
                sb_puts(out, "<tr><td><code>");
                doc_escs(out, fields[i]->name.name);
                sb_puts(out, "</code></td>");
                if (bundle)
                    sb_printf(out, "<td class=\"flip\">%s</td>", fields[i]->flip ? "flip" : "");
                sb_puts(out, "<td><code>");
                doc_html_code(ds, f, doc_src(ds, f, fields[i]->type->span), out);
                sb_puts(out, "</code></td>");
                doc_html_doc_cell(ds, f, doc_text_of(fields[i]->doc), out);
                sb_puts(out, "</tr>\n");
            }
            sb_puts(out, "</tbody>\n</table>\n");
        }
        if (bundle && it->u.bundle.nfns) {
            sb_puts(out, "<h3>Methods</h3>\n<dl class=\"methods\">\n");
            for (i = 0; i < it->u.bundle.nfns; i++) {
                const Item *fn = it->u.bundle.fns[i];
                DocItem tmp;
                StrBuf s2 = {0};
                memset(&tmp, 0, sizeof tmp);
                tmp.it = fn;
                tmp.file = d->file;
                tmp.pkg = d->pkg;
                doc_signature(ds, &tmp, &s2);
                sb_puts(out, "<dt><code>");
                doc_html_code(ds, f, s2.data, out);
                sb_puts(out, "</code></dt>\n<dd>");
                if (fn->doc)
                    doc_md_html(ds, f, fn->doc->text, out);
                sb_puts(out, "</dd>\n");
                sb_free(&s2);
            }
            sb_puts(out, "</dl>\n");
        }
    }
    if (it->kind == ITEM_ENUM && it->u.enum_def.nvariants) {
        sb_puts(out, "<h3>Variants</h3>\n<table class=\"variants\">\n<thead><tr><th>Variant</th><th>Fields</th>"
                     "<th>Description</th></tr></thead>\n<tbody>\n");
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            const Variant *v = it->u.enum_def.variants[i];
            sb_puts(out, "<tr><td><code>");
            doc_escs(out, v->name.name);
            sb_puts(out, "</code></td><td>");
            if (v->has_fields) {
                Span sp = v->span;
                sp.off = doc_end(v->name.span);
                sp.len = doc_end(v->span) - sp.off;
                sb_puts(out, "<code>");
                doc_html_code(ds, f, doc_src(ds, f, sp), out);
                sb_puts(out, "</code>");
            }
            sb_puts(out, "</td>");
            doc_html_doc_cell(ds, f, doc_text_of(v->doc), out);
            sb_puts(out, "</tr>\n");
        }
        sb_puts(out, "</tbody>\n</table>\n");
    }
    if (it->kind == ITEM_REGMAP && it->u.regmap.nentries) {
        sb_puts(out, "<h3>Registers</h3>\n<table class=\"regs\">\n<thead><tr><th>Offset</th><th>Name</th>"
                     "<th>Access</th><th>Type / fields</th><th>Reset</th><th>Description</th></tr></thead>\n<tbody>\n");
        for (i = 0; i < it->u.regmap.nentries; i++) {
            const RegEntry *r = it->u.regmap.entries[i];
            sb_puts(out, "<tr><td><code>");
            doc_html_code(ds, f, doc_src(ds, f, r->offset->span), out);
            sb_puts(out, "</code></td><td><code>");
            doc_escs(out, r->name.name);
            sb_printf(out, "</code></td><td>%s</td><td>", r->access.name);
            if (r->kind == REG_FIELDS) {
                uint32_t j;
                sb_puts(out, "<ul class=\"regfields\">");
                for (j = 0; j < r->nfields; j++) {
                    const RegField *rf = r->fields[j];
                    sb_puts(out, "<li><code>");
                    doc_escs(out, rf->name.name);
                    sb_puts(out, ": ");
                    doc_html_code(ds, f, doc_src(ds, f, rf->type->span), out);
                    sb_puts(out, " @");
                    if (rf->bit_lo) {
                        sb_putc(out, '[');
                        doc_html_code(ds, f, doc_src(ds, f, rf->bit_hi->span), out);
                        sb_putc(out, ':');
                        doc_html_code(ds, f, doc_src(ds, f, rf->bit_lo->span), out);
                        sb_putc(out, ']');
                    } else {
                        doc_html_code(ds, f, doc_src(ds, f, rf->bit_hi->span), out);
                    }
                    sb_puts(out, "</code>");
                    if (rf->doc) {
                        sb_puts(out, " — ");
                        doc_inline_html(ds, f, doc_row_text(ds, rf->doc->text), out);
                    }
                    sb_puts(out, "</li>");
                }
                sb_puts(out, "</ul>");
            } else {
                sb_puts(out, "<code>");
                doc_html_code(ds, f, doc_src(ds, f, r->type->span), out);
                if (r->kind == REG_STREAM) {
                    sb_puts(out, " -&gt; ");
                    doc_html_code(ds, f, doc_src(ds, f, r->stream_type->span), out);
                }
                sb_puts(out, "</code>");
            }
            sb_puts(out, "</td><td>");
            if (r->reset) {
                sb_puts(out, "<code>");
                doc_html_code(ds, f, doc_src(ds, f, r->reset->span), out);
                sb_puts(out, "</code>");
            }
            sb_puts(out, "</td>");
            doc_html_doc_cell(ds, f, doc_text_of(r->doc), out);
            sb_puts(out, "</tr>\n");
        }
        sb_puts(out, "</tbody>\n</table>\n");
    }
    if (d->tests.len) {
        sb_puts(out, "<h3>Tested behaviours</h3>\n<ul class=\"tests\">\n");
        for (i = 0; i < d->tests.len; i++) {
            sb_puts(out, "<li>");
            doc_escs(out, d->tests.data[i]->u.test.name ? d->tests.data[i]->u.test.name : "");
            sb_puts(out, "</li>\n");
        }
        sb_puts(out, "</ul>\n");
    }
    sb_puts(out, "</section>\n");
    sb_free(&sig);
}

static void doc_html_pkg(DocSet *ds, const DocPkg *p, StrBuf *out)
{
    size_t s, i;
    char *text = doc_pkg_text(ds, p);
    StrBuf title = {0};
    sb_printf(&title, "%s - Tern docs", p->name);
    doc_page_head(out, title.data);
    sb_free(&title);
    sb_puts(out, "<div class=\"layout\">\n");
    doc_sidebar(ds, p, out);
    sb_puts(out, "<main>\n<h1><span class=\"kind\">package</span> ");
    doc_escs(out, p->name);
    sb_puts(out, "</h1>\n");
    if (text) {
        sb_puts(out, "<div class=\"doc\">\n");
        doc_md_html(ds, p->main, text, out);
        sb_puts(out, "</div>\n");
    }
    for (s = 0; s < TERN_ARRAY_LEN(doc_sections); s++) {
        int any = 0;
        for (i = 0; i < p->items.len; i++) {
            const DocItem *d = p->items.data[i];
            if (d->it->kind != doc_sections[s].kind)
                continue;
            if (!any)
                sb_printf(out, "<h2 class=\"section\">%s</h2>\n<table class=\"summary\">\n<tbody>\n",
                          doc_sections[s].title);
            any = 1;
            sb_printf(out, "<tr><td><a href=\"#%s\"><code>", d->anchor);
            doc_escs(out, d->name);
            sb_puts(out, "</code></a></td><td>");
            if (d->it->doc)
                doc_inline_html(ds, d->file, doc_summary(ds, d->it->doc->text), out);
            sb_puts(out, "</td></tr>\n");
        }
        if (any)
            sb_puts(out, "</tbody>\n</table>\n");
    }
    if (p->reexports.len) {
        sb_puts(out, "<h2 class=\"section\">Re-exports</h2>\n<ul class=\"reexports\">\n");
        for (i = 0; i < p->reexports.len; i++) {
            const DocReexport *r = &p->reexports.data[i];
            sb_puts(out, "<li><code><span class=\"kw\">pub use</span> ");
            doc_html_code(ds, r->file, doc_src(ds, r->file, r->use->tree->span), out);
            sb_puts(out, "</code></li>\n");
        }
        sb_puts(out, "</ul>\n");
    }
    for (s = 0; s < TERN_ARRAY_LEN(doc_sections); s++) {
        for (i = 0; i < p->items.len; i++) {
            if (p->items.data[i]->it->kind == doc_sections[s].kind)
                doc_html_item(ds, p->items.data[i], out);
        }
    }
    if (p->tests.len) {
        sb_puts(out, "<section class=\"item\" id=\"tests\">\n<h2><span class=\"kind\">tests</span> Other tested "
                     "behaviours</h2>\n<ul class=\"tests\">\n");
        for (i = 0; i < p->tests.len; i++) {
            sb_puts(out, "<li>");
            doc_escs(out, p->tests.data[i]->u.test.name ? p->tests.data[i]->u.test.name : "");
            sb_puts(out, "</li>\n");
        }
        sb_puts(out, "</ul>\n</section>\n");
    }
    sb_puts(out, "</main>\n</div>\n");
    doc_page_tail(out);
}

static void doc_html_index(DocSet *ds, StrBuf *out)
{
    size_t i;
    int std_open = 0, any_proj = 0;
    doc_page_head(out, "Tern docs");
    sb_puts(out, "<div class=\"layout\">\n");
    doc_sidebar(ds, NULL, out);
    sb_puts(out, "<main>\n<h1>Documentation</h1>\n");
    for (i = 0; i < ds->pkgs.len; i++) {
        const DocPkg *p = ds->pkgs.data[i];
        char *t = doc_pkg_text(ds, p);
        if (!p->is_std && !any_proj) {
            sb_puts(out, "<h2 class=\"section\">Packages</h2>\n<table class=\"summary\">\n<tbody>\n");
            any_proj = 1;
        }
        if (p->is_std && !std_open) {
            if (any_proj)
                sb_puts(out, "</tbody>\n</table>\n");
            sb_puts(out, "<h2 class=\"section\">Standard library</h2>\n<table class=\"summary\">\n<tbody>\n");
            std_open = 1;
        }
        sb_printf(out, "<tr><td><a href=\"%s\"><code>", p->page);
        doc_escs(out, p->name);
        sb_puts(out, "</code></a></td><td>");
        if (t)
            doc_inline_html(ds, p->main, doc_summary(ds, t), out);
        sb_printf(out, "</td><td class=\"count\">%u items</td></tr>\n", (unsigned)p->items.len);
    }
    if (any_proj || std_open)
        sb_puts(out, "</tbody>\n</table>\n");
    sb_puts(out, "</main>\n</div>\n");
    doc_page_tail(out);
}

static void doc_js_str(StrBuf *out, const char *s)
{
    sb_putc(out, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            sb_printf(out, "\\%c", c);
        else if (c == '<')
            sb_puts(out, "\\u003c");
        else if (c < 0x20)
            sb_printf(out, "\\u%04x", c);
        else
            sb_putc(out, (int)c);
    }
    sb_putc(out, '"');
}

static void doc_search_index(DocSet *ds, StrBuf *out)
{
    size_t i, j;
    sb_puts(out, "/* generated by tern doc: [name, kind, url, summary] */\nwindow.TERN_SEARCH = [\n");
    for (i = 0; i < ds->pkgs.len; i++) {
        const DocPkg *p = ds->pkgs.data[i];
        StrBuf sum = {0};
        char *t = doc_pkg_text(ds, p);
        if (t)
            doc_inline_text(doc_summary(ds, t), &sum);
        sb_puts(out, "[");
        doc_js_str(out, p->name);
        sb_puts(out, ",\"package\",");
        doc_js_str(out, p->page);
        sb_putc(out, ',');
        doc_js_str(out, sum.data ? sum.data : "");
        sb_puts(out, "],\n");
        sb_free(&sum);
        for (j = 0; j < p->items.len; j++) {
            const DocItem *d = p->items.data[j];
            StrBuf url = {0}, s2 = {0};
            doc_href(&url, d);
            if (d->it->doc)
                doc_inline_text(doc_summary(ds, d->it->doc->text), &s2);
            sb_puts(out, "[");
            doc_js_str(out, d->full);
            sb_putc(out, ',');
            doc_js_str(out, doc_kind_name(d->it));
            sb_putc(out, ',');
            doc_js_str(out, url.data);
            sb_putc(out, ',');
            doc_js_str(out, s2.data ? s2.data : "");
            sb_puts(out, "],\n");
            sb_free(&url);
            sb_free(&s2);
        }
    }
    sb_puts(out, "];\n");
}

static const char doc_style_css[] =
    ":root{--bg:#fdfdfc;--fg:#1d2021;--muted:#5f6368;--line:#e3e3e0;--panel:#f4f4f1;--link:#1a5fb4;"
    "--kw:#8e3b9a;--ty:#00737a;--num:#a65e00;--str:#2d7d2d;--com:#7c7f84;--attr:#9b5f00;--accent:#3f6b3f;"
    "--in:#1a5fb4;--out:#b5402d;color-scheme:light dark}\n"
    "@media (prefers-color-scheme:dark){:root{--bg:#17191b;--fg:#e2e2df;--muted:#9ea3a8;--line:#2e3236;"
    "--panel:#1f2225;--link:#7fb0ff;--kw:#d49be0;--ty:#5fcfd6;--num:#f0b46a;--str:#8fd68f;--com:#8a8f95;"
    "--attr:#e0b062;--accent:#9fd09f;--in:#7fb0ff;--out:#ff9b85}}\n"
    "*{box-sizing:border-box}\n"
    "html{-webkit-text-size-adjust:100%}\n"
    "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,-apple-system,\"Segoe UI\","
    "Roboto,sans-serif}\n"
    "a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}\n"
    "code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.88em}\n"
    "header.top{position:sticky;top:0;z-index:2;display:flex;gap:16px;align-items:center;padding:10px 20px;"
    "background:var(--panel);border-bottom:1px solid var(--line)}\n"
    ".brand{font-weight:700;color:var(--fg)}\n"
    ".search{position:relative;flex:1;max-width:520px}\n"
    "#search{width:100%;padding:7px 10px;border:1px solid var(--line);border-radius:6px;background:var(--bg);"
    "color:var(--fg);font:inherit}\n"
    "#results{position:absolute;left:0;right:0;top:100%;margin:4px 0 0;padding:4px 0;list-style:none;"
    "background:var(--bg);border:1px solid var(--line);border-radius:6px;max-height:70vh;overflow:auto;"
    "box-shadow:0 6px 20px rgba(0,0,0,.15)}\n"
    "#results li a{display:block;padding:5px 12px;color:var(--fg)}\n"
    "#results li a:hover,#results li.sel a{background:var(--panel);text-decoration:none}\n"
    "#results .k{color:var(--muted);font-size:.8em;margin-left:6px}\n"
    "#results .s{display:block;color:var(--muted);font-size:.85em;white-space:nowrap;overflow:hidden;"
    "text-overflow:ellipsis}\n"
    ".layout{display:flex;max-width:1280px;margin:0 auto}\n"
    "nav.side{flex:0 0 240px;padding:16px 20px;border-right:1px solid var(--line);position:sticky;top:53px;"
    "align-self:flex-start;max-height:calc(100vh - 53px);overflow:auto;font-size:.92em}\n"
    "nav.side h2{font-size:.8em;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:18px 0 6px}\n"
    "nav.side h3{font-size:.85em;margin:12px 0 4px;color:var(--muted)}\n"
    "nav.side ul{list-style:none;margin:0;padding:0}nav.side li{margin:2px 0}\n"
    "nav.side li.cur a{font-weight:700;color:var(--fg)}\n"
    "main{flex:1;min-width:0;padding:20px 32px 48px}\n"
    "h1{font-size:1.7em;margin:.3em 0 .6em}\n"
    "h2.section{font-size:1.25em;margin:1.6em 0 .5em;padding-bottom:4px;border-bottom:1px solid var(--line)}\n"
    ".kind{color:var(--muted);font-weight:400;font-size:.75em;margin-right:6px}\n"
    "section.item{margin:2.2em 0;padding-top:.6em;border-top:1px solid var(--line)}\n"
    "section.item h2{font-size:1.3em;margin:.2em 0 .5em}\n"
    "section.item h3{font-size:1em;margin:1.4em 0 .4em}\n"
    ".anchor{visibility:hidden;margin-left:6px;color:var(--muted)}h2:hover .anchor{visibility:visible}\n"
    "pre{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:10px 14px;overflow:auto}\n"
    "pre.sig{border-left:3px solid var(--accent)}\n"
    "p code,li code,td code{background:var(--panel);padding:1px 4px;border-radius:4px}\n"
    "table{border-collapse:collapse;width:100%;margin:.4em 0 1em;font-size:.94em;display:block;overflow-x:auto}\n"
    "th,td{text-align:left;vertical-align:top;padding:6px 10px;border-bottom:1px solid var(--line)}\n"
    "th{font-weight:600;color:var(--muted);font-size:.85em}\n"
    "td.doc p{margin:0 0 .4em}td.doc p:last-child{margin:0}\n"
    "table.summary td:first-child{white-space:nowrap;width:1%}\n"
    "td.count{color:var(--muted);white-space:nowrap}\n"
    "td.dir.in{color:var(--in)}td.dir.out{color:var(--out)}td.flip{color:var(--out)}\n"
    "ul.regfields{margin:0;padding-left:1.1em}\n"
    "dl.methods dt{margin-top:.6em}dl.methods dd{margin:.2em 0 .6em 1.4em}\n"
    ".kw{color:var(--kw)}.ty{color:var(--ty)}.num{color:var(--num)}.str{color:var(--str)}"
    ".com{color:var(--com);font-style:italic}.attr{color:var(--attr)}\n"
    "footer{max-width:1280px;margin:0 auto;padding:16px 32px 32px;color:var(--muted);font-size:.85em}\n"
    "@media (max-width:820px){nav.side{display:none}main{padding:16px}header.top{padding:8px 12px}"
    "th,td{padding:5px 6px}}\n";

static const char doc_search_js[] =
    "/* tern doc: client-side search over window.TERN_SEARCH */\n"
    "(function () {\n"
    "  var input = document.getElementById('search');\n"
    "  var list = document.getElementById('results');\n"
    "  var data = window.TERN_SEARCH || [];\n"
    "  var sel = -1;\n"
    "  if (!input || !list) return;\n"
    "  function score(name, q) {\n"
    "    var n = name.toLowerCase(), last = n.slice(n.lastIndexOf(':') + 1);\n"
    "    if (last === q) return 0;\n"
    "    if (last.indexOf(q) === 0) return 1;\n"
    "    if (last.indexOf(q) >= 0) return 2;\n"
    "    if (n.indexOf(q) >= 0) return 3;\n"
    "    return -1;\n"
    "  }\n"
    "  function render() {\n"
    "    var q = input.value.trim().toLowerCase();\n"
    "    list.innerHTML = '';\n"
    "    sel = -1;\n"
    "    if (!q) { list.hidden = true; return; }\n"
    "    var hits = [];\n"
    "    for (var i = 0; i < data.length; i++) {\n"
    "      var s = score(data[i][0], q);\n"
    "      if (s >= 0) hits.push([s, data[i]]);\n"
    "    }\n"
    "    hits.sort(function (a, b) { return a[0] - b[0] || a[1][0].length - b[1][0].length; });\n"
    "    hits = hits.slice(0, 30);\n"
    "    for (var j = 0; j < hits.length; j++) {\n"
    "      var e = hits[j][1];\n"
    "      var li = document.createElement('li');\n"
    "      var a = document.createElement('a');\n"
    "      a.href = e[2];\n"
    "      a.textContent = e[0];\n"
    "      var k = document.createElement('span'); k.className = 'k'; k.textContent = e[1];\n"
    "      a.appendChild(k);\n"
    "      if (e[3]) { var s2 = document.createElement('span'); s2.className = 's'; s2.textContent = e[3]; "
    "a.appendChild(s2); }\n"
    "      li.appendChild(a);\n"
    "      list.appendChild(li);\n"
    "    }\n"
    "    list.hidden = hits.length === 0;\n"
    "  }\n"
    "  function move(d) {\n"
    "    var items = list.querySelectorAll('li');\n"
    "    if (!items.length) return;\n"
    "    if (sel >= 0) items[sel].className = '';\n"
    "    sel = (sel + d + items.length) % items.length;\n"
    "    items[sel].className = 'sel';\n"
    "    items[sel].scrollIntoView({block: 'nearest'});\n"
    "  }\n"
    "  input.addEventListener('input', render);\n"
    "  input.addEventListener('keydown', function (ev) {\n"
    "    if (ev.key === 'ArrowDown') { move(1); ev.preventDefault(); }\n"
    "    else if (ev.key === 'ArrowUp') { move(-1); ev.preventDefault(); }\n"
    "    else if (ev.key === 'Enter') {\n"
    "      var items = list.querySelectorAll('li a');\n"
    "      if (items.length) window.location.href = items[sel >= 0 ? sel : 0].getAttribute('href');\n"
    "    } else if (ev.key === 'Escape') { input.value = ''; render(); }\n"
    "  });\n"
    "  document.addEventListener('keydown', function (ev) {\n"
    "    if (ev.key === '/' && document.activeElement !== input) { input.focus(); ev.preventDefault(); }\n"
    "  });\n"
    "  document.addEventListener('click', function (ev) {\n"
    "    if (ev.target !== input && !list.contains(ev.target)) list.hidden = true;\n"
    "  });\n"
    "})();\n";

static int doc_write(const char *dir, const char *name, const StrBuf *sb, StrBuf *err)
{
    char *path = os_path_join(dir, name);
    int rc = os_write_file_if_changed(path, sb->data ? sb->data : "", sb->len);
    if (rc < 0 && err)
        sb_printf(err, "cannot write %s", path);
    free(path);
    return rc < 0 ? -1 : 0;
}

/* Writes the HTML site into `outdir`. Returns 0, or -1 with a message. */
int doc_write_html(DocSet *ds, const char *outdir, StrBuf *err)
{
    size_t i;
    StrBuf sb = {0};
    int rc = 0;
    doc_set_finish(ds);
    if (os_mkdir_p(outdir) != 0) {
        if (err)
            sb_printf(err, "cannot create %s", outdir);
        return -1;
    }
    for (i = 0; i < ds->pkgs.len && rc == 0; i++) {
        sb_reset(&sb);
        doc_html_pkg(ds, ds->pkgs.data[i], &sb);
        rc = doc_write(outdir, ds->pkgs.data[i]->page, &sb, err);
    }
    if (rc == 0) {
        sb_reset(&sb);
        doc_html_index(ds, &sb);
        rc = doc_write(outdir, "index.html", &sb, err);
    }
    if (rc == 0) {
        sb_reset(&sb);
        sb_puts(&sb, doc_style_css);
        rc = doc_write(outdir, "style.css", &sb, err);
    }
    if (rc == 0) {
        sb_reset(&sb);
        sb_puts(&sb, doc_search_js);
        rc = doc_write(outdir, "search.js", &sb, err);
    }
    if (rc == 0) {
        sb_reset(&sb);
        doc_search_index(ds, &sb);
        rc = doc_write(outdir, "search-index.js", &sb, err);
    }
    sb_free(&sb);
    return rc;
}

/* Page of a package (for --open), or NULL. */
static const char *doc_page_of(DocSet *ds, const char *what)
{
    size_t i;
    if (!what)
        return NULL;
    for (i = 0; i < ds->pkgs.len; i++) {
        size_t n = strlen(ds->pkgs.data[i]->name);
        if (strncmp(what, ds->pkgs.data[i]->name, n) == 0 && (what[n] == '\0' || what[n] == ':'))
            return ds->pkgs.data[i]->page;
    }
    return NULL;
}

/* ---------------------------------------------------------------- command */

static char *doc_find_root(void)
{
    char *dir = os_getcwd();
    while (dir) {
        char *m = os_path_join(dir, "tern.toml");
        char *parent;
        int found = os_is_file(m);
        free(m);
        if (found)
            return dir;
        parent = os_path_dirname(dir);
        if (!parent || strcmp(parent, dir) == 0) {
            free(parent);
            free(dir);
            return NULL;
        }
        free(dir);
        dir = parent;
    }
    return NULL;
}

static int doc_add_path(DocSet *ds, const char *path, const char *fallback)
{
    DocStrVec files = {0};
    size_t i;
    int bad = 0;
    if (os_is_dir(path))
        doc_collect_dir(path, &files);
    else
        vec_push(files, xstrdup(path));
    for (i = 0; i < files.len; i++) {
        char *text = NULL;
        size_t len = 0;
        if (os_read_file(files.data[i], &text, &len) != 0) {
            fprintf(stderr, "tern doc: error: cannot read %s\n", files.data[i]);
            bad = 1;
        } else {
            char *stem = NULL;
            if (!fallback) {
                const char *b = os_path_basename(files.data[i]);
                const char *dot = strrchr(b, '.');
                stem = xstrndup(b, dot ? (size_t)(dot - b) : strlen(b));
            }
            if (doc_set_add_source(ds, files.data[i], text, len, 0, fallback ? fallback : stem) != 0) {
                fprintf(stderr, "tern doc: %s: skipped (syntax errors)\n", files.data[i]);
                bad = 1;
            }
            free(stem);
        }
        free(text);
        free((char *)files.data[i]);
    }
    vec_free(files);
    return bad;
}

static int doc_text_width(void)
{
    char *cols = os_getenv("COLUMNS");
    uint64_t w = 0;
    int r = DOC_TEXT_WIDTH;
    if (cols && os_isatty(1) && tern_parse_u64(cols, &w) && w >= 40 && w <= 400)
        r = (int)w;
    free(cols);
    return r;
}

static void doc_open_browser(const char *path)
{
    static const char *const openers[] = {"xdg-open", "open", "explorer"};
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(openers); i++) {
        char *prog = os_find_program(openers[i]);
        if (prog) {
            char *argv[3];
            OsRunResult res;
            OsSpawnOpts opts;
            memset(&opts, 0, sizeof opts);
            memset(&res, 0, sizeof res);
            opts.stdin_null = 1;
            argv[0] = prog;
            argv[1] = (char *)path;
            argv[2] = NULL;
            os_run(argv, &opts, &res);
            os_run_result_free(&res);
            free(prog);
            return;
        }
    }
    fprintf(stderr, "tern doc: open %s in a browser\n", path);
}

int cmd_doc(int argc, char **argv)
{
    ArgParser ap;
    const char *target = NULL, *outdir = NULL;
    int html = 0, text = 0, open = 0, rc = TERN_EXIT_OK, is_path = 0, want_std = 1;
    DocSet *ds;
    char *root = NULL;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "html"))
            html = 1;
        else if (args_flag(&ap, "text"))
            text = 1;
        else if (args_flag(&ap, "open"))
            open = 1;
        else if (args_opt(&ap, "output|o", &outdir))
            ;
        else if (args_flag(&ap, "help|h")) {
            cmd_print_usage("doc");
            return TERN_EXIT_OK;
        } else if (args_positional(&ap)) {
            if (target)
                args_error(&ap, "only one package, item or path can be given");
            target = ap.arg;
        } else {
            args_unknown(&ap);
        }
    }
    if (!ap.errors && html && text)
        args_error(&ap, "--html and --text cannot be combined");
    if (ap.errors)
        return TERN_EXIT_USAGE;
    /* `std` and `std::...` name the library even where a std/ directory exists */
    is_path = target && strcmp(target, "std") != 0 && strncmp(target, "std::", 5) != 0 &&
              (os_exists(target) || tern_ends_with(target, ".tern") || strchr(target, '/') || strchr(target, '\\'));
    if (is_path && !os_exists(target)) {
        fprintf(stderr, "tern doc: error: no such file or directory: %s\n", target);
        return TERN_EXIT_USAGE;
    }
    if (!html && !text)
        text = target && !is_path && !outdir;
    ds = doc_set_new();
    if (is_path) {
        if (doc_add_path(ds, target, NULL))
            rc = TERN_EXIT_FAIL;
    } else if (!target || (strcmp(target, "std") != 0 && strncmp(target, "std::", 5) != 0)) {
        /* the project (or the .tern files of the current directory) */
        root = doc_find_root();
        if (root) {
            char *src = os_path_join(root, "src");
            const char *name = os_path_basename(root);
            if (os_is_dir(src) && doc_add_path(ds, src, name))
                rc = TERN_EXIT_FAIL;
            free(src);
        } else {
            OsDirList list;
            size_t i;
            if (os_list_dir(".", &list) == 0) {
                for (i = 0; i < list.len; i++) {
                    if (!list.items[i].is_dir && strcmp(os_path_ext(list.items[i].name), ".tern") == 0 &&
                        doc_add_path(ds, list.items[i].name, NULL))
                        rc = TERN_EXIT_FAIL;
                }
                os_dir_list_free(&list);
            }
        }
    }
    if (want_std)
        doc_set_add_std(ds);
    doc_set_finish(ds);
    if (text) {
        StrBuf out = {0};
        int width = doc_text_width();
        size_t i;
        if (is_path) {
            /* every package of the given files */
            for (i = 0; i < ds->pkgs.len; i++) {
                if (ds->pkgs.data[i]->is_std)
                    continue;
                if (out.len)
                    sb_puts(&out, "\n");
                doc_text_pkg(ds, ds->pkgs.data[i], width, &out);
            }
        } else if (doc_render_text(ds, target, width, &out) != 0) {
            fflush(stdout);
            fprintf(stderr, "tern doc: error: %s", out.data);
            sb_free(&out);
            doc_set_free(ds);
            free(root);
            return TERN_EXIT_FAIL;
        }
        fwrite(out.data ? out.data : "", 1, out.len, stdout);
        sb_free(&out);
    } else {
        StrBuf err = {0};
        char *dir;
        if (outdir)
            dir = xstrdup(outdir);
        else if (root)
            dir = os_path_join(root, "build/doc");
        else
            dir = xstrdup("build/doc");
        if (doc_write_html(ds, dir, &err) != 0) {
            fprintf(stderr, "tern doc: error: %s\n", err.data ? err.data : "cannot write the documentation");
            rc = TERN_EXIT_FAIL;
        } else {
            const char *page = doc_page_of(ds, target);
            char *index = os_path_join(dir, page ? page : "index.html");
            fprintf(stderr, "tern doc: wrote %u packages to %s\n", (unsigned)ds->pkgs.len, dir);
            if (open)
                doc_open_browser(index);
            free(index);
        }
        sb_free(&err);
        free(dir);
    }
    doc_set_free(ds);
    free(root);
    return rc;
}
