/* lsp_nav.c — navigation features of the language server: declarations
 * walked from the AST, go-to-definition, hover, references, document and
 * workspace symbols, completion and the instance tree (see lsp_impl.h).
 *
 * The semantic side table (sema.h, SemInfo) answers for code that was
 * elaborated; everything else (generic modules without defaults, code in a
 * file with syntax errors, item names, `use` paths) falls back to the AST
 * plus package-level name resolution (sema_lookup). */
#include "lsp_impl.h"
#include "../os/os.h"

/* ======================================================== declarations === */

typedef struct NavWalk {
    LspDeclVec *out;
    Item *top;
    int local;
} NavWalk;

static int nav_add(NavWalk *w, Ident name, AstDoc *doc, SemKind kind, Span decl, int parent)
{
    LspDecl d;
    if (!name.name || !name.span.file)
        return parent;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.doc = doc;
    d.kind = w->local && (kind == SEM_WIRE || kind == SEM_VAR) ? SEM_LOCAL : kind;
    d.decl = decl.file == name.span.file ? span_join(decl, name.span) : name.span;
    d.parent = parent;
    d.local = w->local;
    d.top = w->top;
    vec_push(*w->out, d);
    return (int)w->out->len - 1;
}

static void nav_item(NavWalk *w, Item *it, AstDoc *doc, int parent);
static void nav_stmts(NavWalk *w, Stmt **stmts, uint32_t n, int parent);
static void nav_expr(NavWalk *w, Expr *e, int parent);

static void nav_block(NavWalk *w, Block *b, int parent)
{
    if (b)
        nav_stmts(w, b->stmts, b->nstmts, parent);
}

static void nav_pattern(NavWalk *w, Pattern *p, SemKind kind, Span decl, int parent, Type *type, Expr *init)
{
    uint32_t i;
    if (!p)
        return;
    switch (p->kind) {
    case PAT_BIND: {
        int k = nav_add(w, p->u.bind, NULL, kind, decl, parent);
        if (k != parent) {
            w->out->data[k].type = type;
            w->out->data[k].init = init;
        }
        break;
    }
    case PAT_TUPLE:
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            nav_pattern(w, p->u.list.items[i], kind, decl, parent, NULL, NULL);
        break;
    case PAT_STRUCT:
        for (i = 0; i < p->u.struct_pat.nfields; i++) {
            FieldPat *fp = &p->u.struct_pat.fields[i];
            if (fp->pat)
                nav_pattern(w, fp->pat, kind, decl, parent, NULL, NULL);
            else
                nav_add(w, fp->name, NULL, kind, decl, parent);
        }
        break;
    case PAT_VARIANT:
        for (i = 0; i < p->u.variant.nfields; i++) {
            FieldPat *fp = &p->u.variant.fields[i];
            if (fp->pat)
                nav_pattern(w, fp->pat, kind, decl, parent, NULL, NULL);
            else
                nav_add(w, fp->name, NULL, kind, decl, parent);
        }
        break;
    default:
        break;
    }
}

static void nav_expr(NavWalk *w, Expr *e, int parent)
{
    uint32_t i;
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_IF:
        nav_block(w, e->u.if_expr.then_block, parent);
        nav_expr(w, e->u.if_expr.else_expr, parent);
        break;
    case EXPR_MATCH:
        for (i = 0; i < e->u.match.narms; i++) {
            MatchArm *a = &e->u.match.arms[i];
            int save = w->local;
            w->local = 1;
            nav_pattern(w, a->pat, SEM_LOCAL, a->span, parent, NULL, NULL);
            w->local = save;
            if (a->body)
                nav_stmts(w, &a->body, 1, parent);
        }
        break;
    case EXPR_BLOCK: nav_block(w, e->u.block, parent); break;
    case EXPR_PAREN: nav_expr(w, e->u.inner, parent); break;
    default: break;
    }
}

static void nav_stmt(NavWalk *w, Stmt *s, int parent)
{
    int k;
    switch (s->kind) {
    case STMT_PORT:
        k = nav_add(w, s->u.port.name, s->doc, s->u.port.dir == PORT_IN ? SEM_PORT_IN : SEM_PORT_OUT, s->span,
                    parent);
        if (k != parent) {
            w->out->data[k].type = s->u.port.type;
            w->out->data[k].dir = s->u.port.dir;
        }
        break;
    case STMT_DOMAIN: nav_add(w, s->u.domain.name, s->doc, SEM_DOMAIN, s->span, parent); break;
    case STMT_LET:
        if (s->u.let.pat && s->u.let.pat->kind == PAT_BIND) {
            k = nav_add(w, s->u.let.pat->u.bind, s->doc, SEM_WIRE, s->span, parent);
            if (k != parent) {
                w->out->data[k].type = s->u.let.type;
                w->out->data[k].init = s->u.let.init;
            }
        } else {
            nav_pattern(w, s->u.let.pat, SEM_WIRE, s->span, parent, NULL, NULL);
        }
        nav_expr(w, s->u.let.init, parent);
        break;
    case STMT_VAR:
        k = nav_add(w, s->u.var.name, s->doc, SEM_VAR, s->span, parent);
        if (k != parent) {
            w->out->data[k].type = s->u.var.type;
            w->out->data[k].init = s->u.var.init;
        }
        nav_expr(w, s->u.var.init, parent);
        break;
    case STMT_REG:
        k = nav_add(w, s->u.reg.name, s->doc, SEM_REG, s->span, parent);
        if (k != parent) {
            w->out->data[k].type = s->u.reg.type;
            w->out->data[k].init = s->u.reg.init;
        }
        break;
    case STMT_MEM:
        k = nav_add(w, s->u.mem.name, s->doc, SEM_MEM, s->span, parent);
        if (k != parent)
            w->out->data[k].type = s->u.mem.type;
        break;
    case STMT_ITEM:
        if (s->u.item)
            nav_item(w, s->u.item, s->doc, parent);
        break;
    case STMT_COMB: nav_block(w, s->u.comb, parent); break;
    case STMT_ON: nav_block(w, s->u.on.body, parent); break;
    case STMT_ASSIGN: nav_expr(w, s->u.assign.rhs, parent); break;
    case STMT_EXPR: nav_expr(w, s->u.expr, parent); break;
    case STMT_FOR: {
        int save = w->local;
        w->local = 1;
        nav_pattern(w, s->u.for_stmt.pat, SEM_LOCAL, s->span, parent, NULL, NULL);
        w->local = save;
        nav_block(w, s->u.for_stmt.body, parent);
        break;
    }
    case STMT_WHILE: nav_block(w, s->u.while_stmt.body, parent); break;
    default: break;
    }
}

static void nav_stmts(NavWalk *w, Stmt **stmts, uint32_t n, int parent)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (stmts[i])
            nav_stmt(w, stmts[i], parent);
}

static void nav_generics(NavWalk *w, const GenericParams *g, int parent)
{
    uint32_t i;
    for (i = 0; i < g->n; i++) {
        int k = nav_add(w, g->items[i].name, NULL, SEM_GENERIC, g->items[i].span, parent);
        if (k != parent)
            w->out->data[k].type = g->items[i].type;
    }
}

static void nav_fields(NavWalk *w, Field **fields, uint32_t n, int parent)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (fields[i]) {
            int k = nav_add(w, fields[i]->name, fields[i]->doc, SEM_FIELD, fields[i]->span, parent);
            if (k != parent)
                w->out->data[k].type = fields[i]->type;
        }
}

static SemKind nav_item_kind(const Item *it)
{
    switch (it->kind) {
    case ITEM_MODULE:
    case ITEM_EXTERN: return SEM_MODULE;
    case ITEM_CONST: return SEM_CONST;
    case ITEM_FN: return SEM_FN;
    default: return SEM_TYPE;
    }
}

static void nav_item(NavWalk *w, Item *it, AstDoc *doc, int parent)
{
    int k, save = w->local;
    uint32_t i;
    Ident name = it->name;
    if (it->kind == ITEM_TEST && !name.name) {
        name.name = intern_cstr(it->u.test.name ? it->u.test.name : "test");
        name.span = it->u.test.name_span.file ? it->u.test.name_span : it->span;
    }
    k = nav_add(w, name, it->doc ? it->doc : doc, nav_item_kind(it), it->span, parent);
    if (k == parent && name.name)
        return;
    if (k != parent) {
        w->out->data[k].item = it;
        if (it->kind == ITEM_CONST)
            w->out->data[k].type = it->u.const_def.type;
    }
    switch (it->kind) {
    case ITEM_MODULE:
        nav_generics(w, &it->u.module.generics, k);
        nav_stmts(w, it->u.module.body, it->u.module.nbody, k);
        break;
    case ITEM_EXTERN:
        nav_generics(w, &it->u.extern_mod.generics, k);
        nav_stmts(w, it->u.extern_mod.body, it->u.extern_mod.nbody, k);
        break;
    case ITEM_BUNDLE:
        nav_generics(w, &it->u.bundle.generics, k);
        nav_fields(w, it->u.bundle.fields, it->u.bundle.nfields, k);
        for (i = 0; i < it->u.bundle.nfns; i++)
            nav_item(w, it->u.bundle.fns[i], NULL, k);
        break;
    case ITEM_STRUCT:
        nav_generics(w, &it->u.struct_def.generics, k);
        nav_fields(w, it->u.struct_def.fields, it->u.struct_def.nfields, k);
        break;
    case ITEM_ENUM:
        nav_generics(w, &it->u.enum_def.generics, k);
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            Variant *v = it->u.enum_def.variants[i];
            if (v)
                nav_add(w, v->name, v->doc, SEM_VARIANT, v->span, k);
        }
        break;
    case ITEM_FN:
        nav_generics(w, &it->u.fn.generics, k);
        w->local = 1;
        for (i = 0; i < it->u.fn.nparams; i++) {
            int pk = nav_add(w, it->u.fn.params[i].name, NULL, SEM_PARAM, it->u.fn.params[i].span, k);
            if (pk != k)
                w->out->data[pk].type = it->u.fn.params[i].type;
        }
        nav_block(w, it->u.fn.body, k);
        w->local = save;
        break;
    case ITEM_TYPE: nav_generics(w, &it->u.type_alias.generics, k); break;
    case ITEM_REGMAP:
        for (i = 0; i < it->u.regmap.nentries; i++) {
            RegEntry *re = it->u.regmap.entries[i];
            uint32_t f;
            int rk;
            if (!re)
                continue;
            rk = nav_add(w, re->name, re->doc, SEM_FIELD, re->span, k);
            for (f = 0; f < re->nfields; f++)
                if (re->fields[f])
                    nav_add(w, re->fields[f]->name, re->fields[f]->doc, SEM_FIELD, re->fields[f]->span, rk);
        }
        break;
    case ITEM_TEST:
        w->local = 1;
        nav_block(w, it->u.test.body, k);
        w->local = save;
        break;
    default: break;
    }
}

void lsp_collect_decls(const AstFile *f, LspDeclVec *out)
{
    NavWalk w;
    uint32_t i;
    vec_clear(*out);
    memset(&w, 0, sizeof w);
    w.out = out;
    for (i = 0; i < f->nitems; i++) {
        w.top = f->items[i];
        w.local = 0;
        nav_item(&w, f->items[i], NULL, -1);
    }
}

/* ============================================================= helpers === */

static int nav_span_eq(Span a, Span b)
{
    return a.file == b.file && a.off == b.off && a.len == b.len;
}

static int nav_span_has(Span s, uint32_t file, uint32_t off)
{
    return s.file == file && off >= s.off && off < s.off + (s.len ? s.len : 1);
}

static Sema *nav_sema(LspDoc *doc)
{
    return doc->group && doc->group->dr ? driver_sema(doc->group->dr) : NULL;
}

/* The identifier token at (or just before) `off`; -1 if none. */
static int nav_token_at(const AstFile *f, uint32_t off)
{
    uint32_t lo = 0, hi = f->ntokens;
    int best = -1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->tokens[mid].off <= off) {
            best = (int)mid;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (best < 0)
        return -1;
    if (f->tokens[best].kind == TK_EOF)
        best--;
    if (best < 0)
        return -1;
    if (off < f->tokens[best].off + f->tokens[best].len)
        return best;
    if (off == f->tokens[best].off + f->tokens[best].len && f->tokens[best].kind == TK_IDENT)
        return best;
    return -1;
}

static Span nav_tok_span(const AstFile *f, int t)
{
    Span s;
    memset(&s, 0, sizeof s);
    s.file = f->src;
    s.off = f->tokens[t].off;
    s.len = f->tokens[t].len;
    s.line = f->tokens[t].line;
    s.col = f->tokens[t].col;
    return s;
}

/* Whether declaration `d` is in scope at `off` of the same file. */
static int nav_decl_visible(const LspDeclVec *decls, const LspDecl *d, uint32_t off)
{
    const LspDecl *p;
    if (d->kind == SEM_FIELD || d->kind == SEM_VARIANT)
        return 0;
    if (d->parent < 0)
        return 1;
    p = &decls->data[d->parent];
    if (!(off >= p->decl.off && off < p->decl.off + p->decl.len))
        return 0;
    /* locals are visible after their declaration */
    if (d->local && d->name.span.off > off)
        return 0;
    return 1;
}

/* The visible declaration of `name` nearest before `off` (else after). */
static const LspDecl *nav_find_local(const LspDeclVec *decls, const char *name, uint32_t off)
{
    const LspDecl *before = NULL, *after = NULL;
    size_t i;
    for (i = 0; i < decls->len; i++) {
        const LspDecl *d = &decls->data[i];
        if (d->name.name != name || !nav_decl_visible(decls, d, off))
            continue;
        if (d->parent < 0)
            continue; /* top-level items: resolved through sema */
        if (d->name.span.off <= off) {
            if (!before || d->name.span.off >= before->name.span.off)
                before = d;
        } else if (!after) {
            after = d;
        }
    }
    return before ? before : after;
}

/* Declaration whose name is exactly `def`, in any file of the group. */
static const LspDecl *nav_decl_at(LspGroup *g, Span def, LspDeclVec *tmp)
{
    LspFile *lf = lsp_group_file_by_src(g, def.file);
    size_t i;
    if (!lf)
        return NULL;
    lsp_collect_decls(lf->fu->ast, tmp);
    for (i = 0; i < tmp->len; i++)
        if (tmp->data[i].name.span.off == def.off && tmp->data[i].name.span.file == def.file)
            return &tmp->data[i];
    return NULL;
}

/* Resolves a path of identifiers: package / item / enum variant / module
 * constant. Returns the declaration span (file 0 if none), and the item. */
static Span nav_resolve_path(Sema *S, FileUnit *fu, const char *const *segs, size_t n, Item **item_out)
{
    Symbol sym;
    Span none = span_none();
    size_t i;
    *item_out = NULL;
    if (!n)
        return none;
    memset(&sym, 0, sizeof sym);
    if (!sema_lookup(S, fu, segs[0], &sym)) {
        Package *p = sema_root_package(S, segs[0]);
        if (!p)
            return none;
        sym.kind = SYM_PACKAGE;
        sym.pkg = p;
    }
    for (i = 1; i < n; i++) {
        if (sym.kind == SYM_PACKAGE) {
            Symbol next;
            int priv = 0;
            if (!sema_lookup_member(S, fu, sym.pkg, segs[i], &next, &priv)) {
                uint64_t v;
                if (!map_get(&sym.pkg->items, MAP_PTR(segs[i]), &v))
                    return none;
                next = *(Symbol *)(uintptr_t)v;
            }
            sym = next;
        } else if (sym.kind == SYM_ITEM && sym.item) {
            Item *it = sym.item;
            uint32_t k;
            if (it->kind == ITEM_ENUM) {
                for (k = 0; k < it->u.enum_def.nvariants; k++)
                    if (it->u.enum_def.variants[k] && it->u.enum_def.variants[k]->name.name == segs[i]) {
                        *item_out = it;
                        return it->u.enum_def.variants[k]->name.span;
                    }
            } else if (it->kind == ITEM_MODULE) {
                for (k = 0; k < it->u.module.nbody; k++) {
                    Stmt *s = it->u.module.body[k];
                    if (s && s->kind == STMT_ITEM && s->u.item && s->u.item->name.name == segs[i]) {
                        *item_out = s->u.item;
                        return s->u.item->name.span;
                    }
                }
            }
            return none;
        } else {
            return none;
        }
    }
    if (sym.kind == SYM_ITEM && sym.item) {
        *item_out = sym.item;
        return sym.item->name.span;
    }
    if (sym.kind == SYM_PACKAGE && sym.pkg && sym.pkg->files.len) {
        AstFile *pf = sym.pkg->files.data[0]->ast;
        if (pf->has_package)
            return pf->package_span;
    }
    return none;
}

/* ============================================================== targets === */

typedef struct NavTarget {
    Span use;                 /* the name under the cursor */
    Span def;                 /* its declaration (file 0: unknown) */
    const SemInfo *si;        /* semantic info, or NULL */
    Item *item;               /* the declared item, when known */
} NavTarget;

static int nav_target(LspDoc *doc, LspFile *lf, uint32_t off, NavTarget *t)
{
    Sema *S = nav_sema(doc);
    const AstFile *f;
    const Token *tk;
    LspDeclVec decls = {0};
    const LspDecl *d;
    size_t i;
    int ti;
    memset(t, 0, sizeof *t);
    if (!S || !lf)
        return 0;
    f = lf->fu->ast;
    ti = nav_token_at(f, off);
    if (ti < 0)
        return 0;
    tk = &f->tokens[ti];
    t->use = nav_tok_span(f, ti);
    if (tk->kind != TK_IDENT && tk->kind != TK_KW_TYPE)
        return 0;
    /* 1. the elaborator's facts */
    t->si = sema_info_at(S, lf->src, tk->off);
    if (t->si && t->si->span.off <= tk->off && t->si->span.off + t->si->span.len >= tk->off + tk->len &&
        t->si->span.len <= tk->len + 1) {
        t->use = t->si->span;
        if (t->si->def.file) {
            t->def = t->si->def;
            return 1;
        }
    } else {
        t->si = NULL;
    }
    if (tk->kind != TK_IDENT)
        return t->si != NULL;
    /* 2. `use` paths */
    for (i = 0; i < lf->fu->all_uses.len; i++) {
        UseEntry *u = lf->fu->all_uses.data[i];
        uint32_t k;
        for (k = 0; k < u->nsegs; k++) {
            if (!nav_span_has(u->seg_spans[k], lf->src, tk->off))
                continue;
            if (k + 1 == u->nsegs && !u->glob && u->sym.kind == SYM_ITEM && u->sym.item) {
                t->def = u->sym.item->name.span;
                t->item = u->sym.item;
                return 1;
            }
            t->def = nav_resolve_path(S, lf->fu, u->segs, k + 1, &t->item);
            return t->def.file != 0;
        }
    }
    lsp_collect_decls(f, &decls);
    /* 3. a declaration's own name */
    for (i = 0; i < decls.len; i++)
        if (decls.data[i].name.span.off == tk->off && decls.data[i].name.span.file == lf->src) {
            t->def = decls.data[i].name.span;
            t->item = decls.data[i].item;
            vec_free(decls);
            return 1;
        }
    /* 4. qualified paths a::b::c */
    if (ti >= 2 && f->tokens[ti - 1].kind == TK_COLONCOLON) {
        const char *segs[16];
        size_t n = 0;
        int j = ti;
        while (j >= 2 && f->tokens[j - 1].kind == TK_COLONCOLON && f->tokens[j - 2].kind == TK_IDENT && n < 15)
            j -= 2;
        for (; j <= ti && n < 16; j += 2)
            segs[n++] = f->tokens[j].str;
        t->def = nav_resolve_path(S, lf->fu, segs, n, &t->item);
        vec_free(decls);
        return t->def.file != 0;
    }
    if (ti >= 1 && f->tokens[ti - 1].kind == TK_DOT) {
        vec_free(decls);
        return t->si != NULL; /* members need type information */
    }
    /* 5. local declarations */
    d = nav_find_local(&decls, tk->str, tk->off);
    if (d) {
        t->def = d->name.span;
        t->item = d->item;
        vec_free(decls);
        return 1;
    }
    vec_free(decls);
    /* 6. package scope */
    {
        Item *it = NULL;
        const char *seg = tk->str;
        t->def = nav_resolve_path(S, lf->fu, &seg, 1, &it);
        t->item = it;
    }
    return t->def.file != 0 || t->si != NULL;
}

void lsp_nav_definition(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out)
{
    NavTarget t;
    JsonWriter w;
    StrBuf loc = {0};
    jw_init(&w, &loc, 0);
    if (nav_target(doc, lf, off, &t) && t.def.file && lsp_w_location(srv, &w, t.def))
        sb_append(out, loc.data, loc.len);
    else
        sb_puts(out, "null");
    sb_free(&loc);
}

/* ================================================================ hover === */

/* Appends source text with each line's indentation reduced by `indent`. */
static void nav_put_source(StrBuf *out, const char *p, size_t n, size_t max_lines)
{
    size_t i = 0, lines = 0;
    while (i < n && lines < max_lines) {
        size_t s = i, e;
        while (i < n && p[i] != '\n')
            i++;
        e = i;
        if (i < n)
            i++;
        while (s < e && (p[s] == ' ' || p[s] == '\t'))
            s++;
        while (e > s && (p[e - 1] == ' ' || p[e - 1] == '\t' || p[e - 1] == '\r'))
            e--;
        if (lines)
            sb_putc(out, '\n');
        if (lines && e > s && p[s] != '}' && p[s] != 'w')
            sb_puts(out, "    ");
        sb_append(out, p + s, e - s);
        lines++;
    }
    if (i < n)
        sb_puts(out, "\n    ...");
}

static void nav_put_span(StrBuf *out, Span s)
{
    size_t n;
    const char *p = lsp_span_text(s, &n);
    sb_append(out, p, n);
}

/* Header of an item: from its keyword to the `{` of its body (or all of it). */
static void nav_item_header(const Item *it, StrBuf *out)
{
    size_t n, i, start = 0;
    const char *p = lsp_span_text(it->span, &n);
    int depth = 0;
    if (it->attrs.n) {
        const Attr *a = &it->attrs.items[it->attrs.n - 1];
        if (a->span.off + a->span.len >= it->span.off && a->span.off + a->span.len - it->span.off <= n)
            start = a->span.off + a->span.len - it->span.off;
    }
    while (start < n && (p[start] == ' ' || p[start] == '\t' || p[start] == '\r' || p[start] == '\n'))
        start++;
    for (i = start; i < n; i++) {
        char c = p[i];
        if (c == '(' || c == '[')
            depth++;
        else if ((c == ')' || c == ']') && depth > 0)
            depth--;
        else if (c == '{' && depth == 0)
            break;
    }
    while (i > start && (p[i - 1] == ' ' || p[i - 1] == '\n' || p[i - 1] == '\r' || p[i - 1] == '\t'))
        i--;
    nav_put_source(out, p + start, i - start, 12);
}

static void nav_item_signature(const Item *it, StrBuf *out)
{
    uint32_t i, shown = 0;
    nav_item_header(it, out);
    switch (it->kind) {
    case ITEM_MODULE:
    case ITEM_EXTERN: {
        Stmt **body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
        uint32_t n = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody;
        sb_puts(out, " {");
        for (i = 0; i < n; i++) {
            Stmt *s = body[i];
            if (!s || s->kind != STMT_PORT || !s->u.port.name.name)
                continue;
            if (shown++ == 40) {
                sb_puts(out, "\n    ...");
                break;
            }
            sb_printf(out, "\n    %s %s: ", s->u.port.dir == PORT_IN ? "in" : "out", s->u.port.name.name);
            nav_put_span(out, s->u.port.type->span);
            if (s->u.port.domain) {
                sb_puts(out, " @ ");
                nav_put_span(out, s->u.port.domain->span);
            }
        }
        sb_puts(out, shown ? "\n}" : " }");
        break;
    }
    case ITEM_STRUCT:
    case ITEM_BUNDLE: {
        Field **fs = it->kind == ITEM_STRUCT ? it->u.struct_def.fields : it->u.bundle.fields;
        uint32_t n = it->kind == ITEM_STRUCT ? it->u.struct_def.nfields : it->u.bundle.nfields;
        sb_puts(out, " {");
        for (i = 0; i < n && i < 40; i++) {
            if (!fs[i] || !fs[i]->name.name)
                continue;
            sb_printf(out, "\n    %s%s: ", fs[i]->flip ? "flip " : "", fs[i]->name.name);
            nav_put_span(out, fs[i]->type->span);
        }
        if (n > 40)
            sb_puts(out, "\n    ...");
        sb_puts(out, n ? "\n}" : " }");
        break;
    }
    case ITEM_ENUM:
        sb_puts(out, " {");
        for (i = 0; i < it->u.enum_def.nvariants && i < 40; i++) {
            Variant *v = it->u.enum_def.variants[i];
            if (!v)
                continue;
            sb_puts(out, "\n    ");
            nav_put_span(out, v->has_fields ? v->span : v->name.span);
        }
        if (it->u.enum_def.nvariants > 40)
            sb_puts(out, "\n    ...");
        sb_puts(out, it->u.enum_def.nvariants ? "\n}" : " }");
        break;
    default: break;
    }
}

static void nav_put_doc(StrBuf *md, const AstDoc *doc)
{
    if (doc && doc->text && doc->text[0]) {
        sb_puts(md, "\n\n---\n\n");
        sb_puts(md, doc->text);
    }
}

void lsp_nav_hover(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out)
{
    NavTarget t;
    StrBuf md = {0};
    LspDeclVec tmp = {0};
    const LspDecl *d = NULL;
    const SemInfo *decl_si = NULL;
    Sema *S = nav_sema(doc);
    JsonWriter w;
    TERN_UNUSED(srv);
    if (!nav_target(doc, lf, off, &t)) {
        sb_puts(out, "null");
        return;
    }
    if (t.def.file)
        d = nav_decl_at(doc->group, t.def, &tmp);
    if (t.def.file && S)
        decl_si = sema_info_decl(S, t.def);
    if (!t.item && d)
        t.item = d->item;
    if (t.si && (t.si->type || t.si->value || !t.item)) {
        const SemInfo *si = t.si;
        const char *dom = si->domain ? si->domain : decl_si ? decl_si->domain : NULL;
        const char *driver = si->driver ? si->driver : decl_si ? decl_si->driver : NULL;
        Span dspan = si->driver ? si->driver_span : decl_si ? decl_si->driver_span : span_none();
        uint32_t width = si->width ? si->width : decl_si ? decl_si->width : 0;
        sb_puts(&md, "```tern\n");
        if (si->kind == SEM_PORT_IN || si->kind == SEM_PORT_OUT)
            sb_puts(&md, si->kind == SEM_PORT_IN ? "in " : "out ");
        else if (si->kind == SEM_REG || si->kind == SEM_VAR || si->kind == SEM_MEM || si->kind == SEM_CONST)
            sb_printf(&md, "%s ", sema_kind_name(si->kind));
        else if (si->kind == SEM_WIRE)
            sb_puts(&md, "let ");
        sb_puts(&md, si->name ? si->name : "?");
        if (si->type)
            sb_printf(&md, ": %s", si->type);
        if (dom)
            sb_printf(&md, " @ %s", dom);
        if (si->value && !(si->type && strcmp(si->value, si->type) == 0))
            sb_printf(&md, " = %s", si->value);
        sb_puts(&md, "\n```\n");
        if (si->kind != SEM_NONE && si->kind != SEM_EXPR)
            sb_printf(&md, "\n*%s*", sema_kind_name(si->kind));
        if (width)
            sb_printf(&md, "%s%u bit%s", si->kind != SEM_NONE && si->kind != SEM_EXPR ? " · " : "\n",
                      (unsigned)width, width == 1 ? "" : "s");
        if (driver) {
            sb_printf(&md, " · driven by `%s`", driver);
            if (dspan.file && src_get(dspan.file))
                sb_printf(&md, " at %s:%u", os_path_basename(src_get(dspan.file)->path), (unsigned)dspan.line);
        }
    } else if (t.item) {
        sb_puts(&md, "```tern\n");
        nav_item_signature(t.item, &md);
        sb_puts(&md, "\n```");
        if (t.item->kind == ITEM_MODULE || t.item->kind == ITEM_EXTERN || t.item->kind == ITEM_FN ||
            t.item->kind == ITEM_STRUCT || t.item->kind == ITEM_ENUM || t.item->kind == ITEM_BUNDLE) {
            FileUnit *fu = S ? sema_file_of_item(S, t.item) : NULL;
            if (fu && fu->pkg && fu->pkg->path && fu->pkg->path[0])
                sb_printf(&md, "\n\n*%s*", fu->pkg->path);
        }
    } else if (d) {
        size_t n;
        const char *p = lsp_span_text(d->decl, &n);
        sb_puts(&md, "```tern\n");
        nav_put_source(&md, p, n, 1);
        sb_puts(&md, "\n```\n\n*");
        sb_puts(&md, sema_kind_name(d->kind));
        sb_puts(&md, "*");
    } else {
        vec_free(tmp);
        sb_puts(out, "null");
        return;
    }
    if (t.si && t.item && (t.si->type || t.si->value) && t.item->kind != ITEM_CONST) {
        sb_puts(&md, "\n\n```tern\n");
        nav_item_signature(t.item, &md);
        sb_puts(&md, "\n```");
    }
    nav_put_doc(&md, d ? d->doc : t.item ? t.item->doc : NULL);
    jw_init(&w, out, 0);
    jw_object_begin(&w);
    jw_key(&w, "contents");
    jw_object_begin(&w);
    jw_kv_str(&w, "kind", "markdown");
    jw_key(&w, "value");
    jw_strn(&w, md.data ? md.data : "", md.len);
    jw_object_end(&w);
    jw_key(&w, "range");
    lsp_w_range(&w, t.use);
    jw_object_end(&w);
    sb_free(&md);
    vec_free(tmp);
}

/* =========================================================== references === */

typedef VEC(Span) NavSpanVec;

static void nav_add_ref(NavSpanVec *v, Span s)
{
    size_t i;
    if (!s.file)
        return;
    for (i = 0; i < v->len; i++)
        if (v->data[i].file == s.file && v->data[i].off == s.off)
            return;
    vec_push(*v, s);
}

void lsp_nav_references(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, int include_decl,
                        int highlight, StrBuf *out)
{
    NavTarget t;
    NavSpanVec refs = {0};
    Sema *S = nav_sema(doc);
    JsonWriter w;
    size_t i, n;
    const SemInfo *all;
    if (!S || !nav_target(doc, lf, off, &t) || !t.def.file) {
        sb_puts(out, "[]");
        return;
    }
    if (include_decl)
        nav_add_ref(&refs, t.def);
    n = sema_info_all(S, &all);
    for (i = 0; i < n; i++)
        if (nav_span_eq(all[i].def, t.def) && !nav_span_eq(all[i].span, t.def))
            nav_add_ref(&refs, all[i].span);
    for (i = 0; i < doc->group->files.len; i++) {
        FileUnit *fu = doc->group->files.data[i].fu;
        size_t k;
        for (k = 0; k < fu->all_uses.len; k++) {
            UseEntry *u = fu->all_uses.data[k];
            if (u->file == fu && !u->glob && u->nsegs && u->sym.kind == SYM_ITEM && u->sym.item &&
                nav_span_eq(u->sym.item->name.span, t.def))
                nav_add_ref(&refs, u->seg_spans[u->nsegs - 1]);
        }
    }
    jw_init(&w, out, 0);
    jw_array_begin(&w);
    for (i = 0; i < refs.len; i++) {
        if (highlight) {
            if (refs.data[i].file != lf->src)
                continue;
            jw_object_begin(&w);
            jw_key(&w, "range");
            lsp_w_range(&w, refs.data[i]);
            jw_kv_int(&w, "kind", nav_span_eq(refs.data[i], t.def) ? 3 : 2);
            jw_object_end(&w);
        } else {
            lsp_w_location(srv, &w, refs.data[i]);
        }
    }
    jw_array_end(&w);
    vec_free(refs);
}

/* ============================================================== symbols === */

/* LSP SymbolKind */
static int nav_symbol_kind(const LspDecl *d)
{
    if (d->item) {
        switch (d->item->kind) {
        case ITEM_MODULE:
        case ITEM_EXTERN: return 2;      /* Module */
        case ITEM_BUNDLE: return 11;     /* Interface */
        case ITEM_STRUCT: return 23;     /* Struct */
        case ITEM_ENUM: return 10;       /* Enum */
        case ITEM_CONST: return 14;      /* Constant */
        case ITEM_FN: return 12;         /* Function */
        case ITEM_TYPE: return 26;       /* TypeParameter */
        case ITEM_REGMAP: return 3;      /* Namespace */
        case ITEM_TEST: return 24;       /* Event */
        case ITEM_PROPERTY:
        case ITEM_SEQUENCE: return 17;   /* Boolean */
        default: return 13;
        }
    }
    switch (d->kind) {
    case SEM_PORT_IN:
    case SEM_PORT_OUT: return 7;         /* Property */
    case SEM_INSTANCE: return 19;        /* Object */
    case SEM_DOMAIN: return 20;          /* Key */
    case SEM_FIELD: return 8;            /* Field */
    case SEM_VARIANT: return 22;         /* EnumMember */
    case SEM_GENERIC: return 26;         /* TypeParameter */
    case SEM_CONST: return 14;
    default: return 13;                  /* Variable */
    }
}

/* The module item an instance expression `M<..>(..)` names, or NULL. */
static Item *nav_callee_module(Sema *S, FileUnit *fu, Expr *init)
{
    const char *segs[16];
    size_t n = 0;
    uint32_t i;
    Item *it = NULL;
    Span def;
    while (init && (init->kind == EXPR_PAREN || init->kind == EXPR_SIM))
        init = init->u.inner;
    if (!init || init->kind != EXPR_CALL || !init->u.call.callee || init->u.call.callee->kind != EXPR_PATH)
        return NULL;
    for (i = 0; i < init->u.call.callee->u.path.nsegs && n < 16; i++)
        if (init->u.call.callee->u.path.segs[i].name.name)
            segs[n++] = init->u.call.callee->u.path.segs[i].name.name;
    if (!S || !n)
        return NULL;
    def = nav_resolve_path(S, fu, segs, n, &it);
    TERN_UNUSED(def);
    return it && (it->kind == ITEM_MODULE || it->kind == ITEM_EXTERN) ? it : NULL;
}

static void nav_symbol(JsonWriter *w, const LspDeclVec *decls, size_t idx, Sema *S, FileUnit *fu, int depth)
{
    const LspDecl *d = &decls->data[idx];
    StrBuf detail = {0};
    size_t i;
    int kind = nav_symbol_kind(d);
    Item *inst = d->kind == SEM_WIRE && d->init ? nav_callee_module(S, fu, d->init) : NULL;
    if (inst)
        kind = 19;
    if (d->kind == SEM_PORT_IN || d->kind == SEM_PORT_OUT) {
        sb_puts(&detail, d->kind == SEM_PORT_IN ? "in " : "out ");
        if (d->type)
            nav_put_span(&detail, d->type->span);
    } else if (inst) {
        nav_put_span(&detail, d->init->kind == EXPR_CALL ? d->init->u.call.callee->span : d->init->span);
    } else if (d->type) {
        nav_put_span(&detail, d->type->span);
    } else if (d->item) {
        static const char *const names[] = {"",       "module", "extern module", "bundle", "struct",
                                            "enum",   "const",  "fn",            "type",   "regmap",
                                            "test",   "property", "sequence",    "static_assert"};
        if ((size_t)d->item->kind < TERN_ARRAY_LEN(names))
            sb_puts(&detail, names[d->item->kind]);
    } else if (d->kind != SEM_NONE) {
        sb_puts(&detail, sema_kind_name(d->kind));
    }
    jw_object_begin(w);
    jw_kv_str(w, "name", d->name.name);
    if (detail.len)
        jw_kv_str(w, "detail", sb_cstr(&detail));
    jw_kv_int(w, "kind", kind);
    jw_key(w, "range");
    lsp_w_range(w, d->decl);
    jw_key(w, "selectionRange");
    lsp_w_range(w, d->name.span);
    if (depth < 8) {
        int any = 0;
        for (i = idx + 1; i < decls->len; i++) {
            const LspDecl *c = &decls->data[i];
            if (c->parent != (int)idx || c->local || c->kind == SEM_GENERIC)
                continue;
            if (!any) {
                jw_key(w, "children");
                jw_array_begin(w);
                any = 1;
            }
            nav_symbol(w, decls, i, S, fu, depth + 1);
        }
        if (any)
            jw_array_end(w);
    }
    jw_object_end(w);
    sb_free(&detail);
}

void lsp_nav_document_symbols(LspServer *srv, LspDoc *doc, LspFile *lf, StrBuf *out)
{
    LspDeclVec decls = {0};
    JsonWriter w;
    size_t i;
    TERN_UNUSED(srv);
    if (!lf) {
        sb_puts(out, "[]");
        return;
    }
    lsp_collect_decls(lf->fu->ast, &decls);
    jw_init(&w, out, 0);
    jw_array_begin(&w);
    for (i = 0; i < decls.len; i++)
        if (decls.data[i].parent < 0)
            nav_symbol(&w, &decls, i, nav_sema(doc), lf->fu, 0);
    jw_array_end(&w);
    vec_free(decls);
}

static int nav_contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i, k;
    if (!n)
        return 1;
    for (i = 0; hay[i]; i++) {
        for (k = 0; k < n && hay[i + k]; k++)
            if (tern_to_lower((unsigned char)hay[i + k]) != tern_to_lower((unsigned char)needle[k]))
                break;
        if (k == n)
            return 1;
    }
    return 0;
}

void lsp_nav_workspace_symbols(LspServer *srv, const char *query, StrBuf *out)
{
    JsonWriter w;
    NavSpanVec seen = {0};
    size_t g, i, count = 0;
    uint32_t k;
    jw_init(&w, out, 0);
    jw_array_begin(&w);
    for (g = 0; g < srv->groups.len; g++) {
        LspGroup *gr = srv->groups.data[g];
        for (i = 0; i < gr->files.len && count < 1000; i++) {
            LspFile *lf = &gr->files.data[i];
            if (!lf->fu->is_user || !lf->path)
                continue;
            for (k = 0; k < lf->fu->ast->nitems && count < 1000; k++) {
                Item *it = lf->fu->ast->items[k];
                LspDecl d;
                size_t before = seen.len;
                if (!it->name.name || !nav_contains_ci(it->name.name, query))
                    continue;
                nav_add_ref(&seen, it->name.span);
                if (seen.len == before)
                    continue;
                memset(&d, 0, sizeof d);
                d.item = it;
                jw_object_begin(&w);
                jw_kv_str(&w, "name", it->name.name);
                jw_kv_int(&w, "kind", nav_symbol_kind(&d));
                jw_key(&w, "location");
                if (!lsp_w_location(srv, &w, it->name.span))
                    jw_null(&w);
                if (lf->fu->pkg && lf->fu->pkg->path && lf->fu->pkg->path[0])
                    jw_kv_str(&w, "containerName", lf->fu->pkg->path);
                jw_object_end(&w);
                count++;
            }
        }
    }
    jw_array_end(&w);
    vec_free(seen);
}

/* =========================================================== completion === */

static const char *const nav_keywords[] = {
    "package", "use", "pub", "module", "extern", "bundle", "struct", "enum", "const", "fn",
    "type", "regmap", "domain", "where", "test", "property", "sequence", "in", "out", "flip",
    "let", "var", "reg", "mem", "on", "comb", "match", "if", "else", "for", "while", "return",
    "as", "sim", "expect", "assert", "assume", "cover", "and", "or", "not", "xor", "true", "false",
    "all0", "all1", "static_assert", "always", "never", "eventually", "until"};

static const char *const nav_builtin_types[] = {
    "uint", "sint", "bit", "bool", "clock", "reset", "tri", "u1", "u8", "u16", "u32", "u64",
    "s8", "s16", "s32", "s64"};

typedef struct NavComp {
    JsonWriter *w;
    const char *prefix;
    size_t plen;
    Map seen;
    size_t count;
} NavComp;

static int nav_prefix_ok(const NavComp *c, const char *label)
{
    size_t i;
    for (i = 0; i < c->plen; i++)
        if (!label[i] || tern_to_lower((unsigned char)label[i]) != tern_to_lower((unsigned char)c->prefix[i]))
            return 0;
    return 1;
}

/* LSP CompletionItemKind */
static int nav_comp_kind_of_item(const Item *it)
{
    switch (it->kind) {
    case ITEM_MODULE:
    case ITEM_EXTERN: return 9;   /* Module */
    case ITEM_BUNDLE: return 8;   /* Interface */
    case ITEM_STRUCT: return 22;  /* Struct */
    case ITEM_ENUM: return 13;    /* Enum */
    case ITEM_CONST: return 21;   /* Constant */
    case ITEM_FN: return 3;       /* Function */
    case ITEM_TYPE: return 25;    /* TypeParameter */
    default: return 18;           /* Reference */
    }
}

static void nav_comp_add(NavComp *c, const char *label, int kind, const char *detail)
{
    const char *key;
    int inserted = 0;
    if (!label || !label[0] || c->count >= 2000 || !nav_prefix_ok(c, label))
        return;
    key = intern_cstr(label);
    map_ref(&c->seen, MAP_PTR(key), &inserted);
    if (!inserted)
        return;
    jw_object_begin(c->w);
    jw_kv_str(c->w, "label", label);
    jw_kv_int(c->w, "kind", kind);
    if (detail && detail[0])
        jw_kv_str(c->w, "detail", detail);
    jw_object_end(c->w);
    c->count++;
}

static void nav_comp_item(NavComp *c, const Item *it)
{
    static const char *const names[] = {"",     "module", "extern module", "bundle",   "struct",
                                        "enum", "const",  "fn",            "type",     "regmap",
                                        "test", "property", "sequence",    "static_assert"};
    if (!it || !it->name.name)
        return;
    nav_comp_add(c, it->name.name, nav_comp_kind_of_item(it),
                 (size_t)it->kind < TERN_ARRAY_LEN(names) ? names[it->kind] : NULL);
}

static void nav_comp_package(NavComp *c, Package *p, int only_pub)
{
    size_t it = 0;
    uint64_t k, v;
    if (!p)
        return;
    while (map_next(&p->items, &it, &k, &v)) {
        Symbol *s = (Symbol *)(uintptr_t)v;
        if (s && s->kind == SYM_ITEM && s->item && (!only_pub || s->item->is_pub))
            nav_comp_item(c, s->item);
    }
    it = 0;
    while (map_next(&p->reexports, &it, &k, &v)) {
        UseEntry *u = (UseEntry *)(uintptr_t)v;
        if (u && u->sym.kind == SYM_ITEM)
            nav_comp_item(c, u->sym.item);
        else if (u && u->name)
            nav_comp_add(c, u->name, 9, "package");
    }
    it = 0;
    while (map_next(&p->children, &it, &k, &v))
        nav_comp_add(c, (const char *)(uintptr_t)k, 9, "package");
}

/* Fields / ports / variants of a type or module item. */
static void nav_comp_members(NavComp *c, const Item *it)
{
    uint32_t i;
    if (!it)
        return;
    switch (it->kind) {
    case ITEM_STRUCT:
        for (i = 0; i < it->u.struct_def.nfields; i++)
            if (it->u.struct_def.fields[i])
                nav_comp_add(c, it->u.struct_def.fields[i]->name.name, 5, "field");
        break;
    case ITEM_BUNDLE:
        for (i = 0; i < it->u.bundle.nfields; i++)
            if (it->u.bundle.fields[i])
                nav_comp_add(c, it->u.bundle.fields[i]->name.name, 5,
                             it->u.bundle.fields[i]->flip ? "flip field" : "field");
        for (i = 0; i < it->u.bundle.nfns; i++)
            if (it->u.bundle.fns[i])
                nav_comp_add(c, it->u.bundle.fns[i]->name.name, 2, "method");
        break;
    case ITEM_MODULE:
    case ITEM_EXTERN: {
        Stmt **body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
        uint32_t n = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody;
        for (i = 0; i < n; i++)
            if (body[i] && body[i]->kind == STMT_PORT)
                nav_comp_add(c, body[i]->u.port.name.name, 10, body[i]->u.port.dir == PORT_IN ? "in" : "out");
        break;
    }
    case ITEM_ENUM:
        for (i = 0; i < it->u.enum_def.nvariants; i++)
            if (it->u.enum_def.variants[i])
                nav_comp_add(c, it->u.enum_def.variants[i]->name.name, 20, "variant");
        break;
    default: break;
    }
}

/* The item named by a type: `Stream<u8>` -> Stream, `std::x::Y` -> Y. */
static Item *nav_type_item(Sema *S, FileUnit *fu, const char *type_text, size_t n)
{
    const char *segs[16];
    size_t ns = 0, i = 0;
    Item *it = NULL;
    while (i < n && ns < 16) {
        size_t s = i;
        while (i < n && tern_is_ident((unsigned char)type_text[i]))
            i++;
        if (i == s)
            break;
        segs[ns++] = intern(type_text + s, i - s);
        if (i + 1 < n && type_text[i] == ':' && type_text[i + 1] == ':')
            i += 2;
        else
            break;
    }
    if (!ns)
        return NULL;
    nav_resolve_path(S, fu, segs, ns, &it);
    return it;
}

void lsp_nav_completion(LspServer *srv, LspDoc *doc, LspFile *lf, uint32_t off, StrBuf *out)
{
    Sema *S = nav_sema(doc);
    const char *text = doc->text;
    NavComp c;
    JsonWriter w;
    uint32_t p = off;
    size_t i;
    TERN_UNUSED(srv);
    if (off > doc->len)
        off = p = (uint32_t)doc->len;
    while (p > 0 && tern_is_ident((unsigned char)text[p - 1]))
        p--;
    memset(&c, 0, sizeof c);
    c.prefix = text + p;
    c.plen = off - p;
    c.w = &w;
    jw_init(&w, out, 0);
    jw_object_begin(&w);
    jw_kv_bool(&w, "isIncomplete", 0);
    jw_key(&w, "items");
    jw_array_begin(&w);
    if (p > 0 && text[p - 1] == '.' && !(p > 1 && text[p - 2] == '.')) {
        /* members: x.<field / port> */
        uint32_t q = p - 1, r = q;
        while (r > 0 && tern_is_ident((unsigned char)text[r - 1]))
            r--;
        if (r < q && S && lf) {
            const SemInfo *si = sema_info_at(S, lf->src, r);
            const char *name = intern(text + r, q - r);
            Item *it = NULL;
            if (si && si->type && si->span.off == r)
                it = nav_type_item(S, lf->fu, si->type, strlen(si->type));
            if (!it) {
                LspDeclVec decls = {0};
                const LspDecl *d;
                lsp_collect_decls(lf->fu->ast, &decls);
                d = nav_find_local(&decls, name, r);
                if (d && d->type) {
                    size_t n;
                    const char *tt = lsp_span_text(d->type->span, &n);
                    it = nav_type_item(S, lf->fu, tt, n);
                } else if (d && d->init) {
                    it = nav_callee_module(S, lf->fu, d->init);
                }
                vec_free(decls);
            }
            nav_comp_members(&c, it);
        }
    } else if (p >= 2 && text[p - 1] == ':' && text[p - 2] == ':') {
        /* paths: a::b::<item> */
        const char *segs[16];
        size_t ns = 0;
        uint32_t e = p - 2;
        const char *rev[16];
        while (ns < 16) {
            uint32_t s = e;
            while (s > 0 && tern_is_ident((unsigned char)text[s - 1]))
                s--;
            if (s == e)
                break;
            rev[ns++] = intern(text + s, e - s);
            if (s >= 2 && text[s - 1] == ':' && text[s - 2] == ':')
                e = s - 2;
            else
                break;
        }
        for (i = 0; i < ns; i++)
            segs[i] = rev[ns - 1 - i];
        if (ns && S && lf) {
            Item *it = NULL;
            Package *pkg = NULL;
            Symbol sym;
            size_t k;
            /* walk packages as far as possible */
            if (sema_lookup(S, lf->fu, segs[0], &sym) && sym.kind == SYM_PACKAGE)
                pkg = sym.pkg;
            else if (!sema_lookup(S, lf->fu, segs[0], &sym))
                pkg = sema_root_package(S, segs[0]);
            for (k = 1; pkg && k < ns; k++) {
                Symbol next;
                int priv = 0;
                if (sema_lookup_member(S, lf->fu, pkg, segs[k], &next, &priv) && next.kind == SYM_PACKAGE) {
                    pkg = next.pkg;
                } else {
                    pkg = NULL;
                }
            }
            if (pkg) {
                nav_comp_package(&c, pkg, pkg != lf->fu->pkg);
            } else {
                nav_resolve_path(S, lf->fu, segs, ns, &it);
                if (it && it->kind == ITEM_ENUM)
                    nav_comp_members(&c, it);
                else if (it && it->kind == ITEM_MODULE) {
                    uint32_t b;
                    for (b = 0; b < it->u.module.nbody; b++) {
                        Stmt *s = it->u.module.body[b];
                        if (s && s->kind == STMT_ITEM && s->u.item && s->u.item->kind == ITEM_CONST)
                            nav_comp_item(&c, s->u.item);
                    }
                }
            }
        }
    } else {
        /* locals, file scope, keywords */
        if (lf) {
            LspDeclVec decls = {0};
            lsp_collect_decls(lf->fu->ast, &decls);
            for (i = 0; i < decls.len; i++) {
                const LspDecl *d = &decls.data[i];
                if (d->parent >= 0 && nav_decl_visible(&decls, d, off) && d->name.span.off + d->name.span.len < off + 1 &&
                    !(d->name.span.off <= off && off <= d->name.span.off + d->name.span.len)) {
                    StrBuf det = {0};
                    if (d->type)
                        nav_put_span(&det, d->type->span);
                    else
                        sb_puts(&det, sema_kind_name(d->kind));
                    nav_comp_add(&c, d->name.name, d->kind == SEM_CONST ? 21 : d->kind == SEM_FN ? 3 : 6,
                                 sb_cstr(&det));
                    sb_free(&det);
                }
            }
            vec_free(decls);
        }
        if (S && lf) {
            size_t it = 0;
            uint64_t k, v;
            nav_comp_package(&c, lf->fu->pkg, 0);
            while (map_next(&lf->fu->uses, &it, &k, &v)) {
                UseEntry *u = (UseEntry *)(uintptr_t)v;
                if (u->sym.kind == SYM_ITEM)
                    nav_comp_item(&c, u->sym.item);
                else if (u->name)
                    nav_comp_add(&c, u->name, 9, "package");
            }
            for (i = 0; i < lf->fu->globs.len; i++)
                if (lf->fu->globs.data[i]->sym.kind == SYM_PACKAGE)
                    nav_comp_package(&c, lf->fu->globs.data[i]->sym.pkg, 1);
            nav_comp_package(&c, S->prelude, 1);
            it = 0;
            while (map_next(&S->root->children, &it, &k, &v))
                nav_comp_add(&c, (const char *)(uintptr_t)k, 9, "package");
        }
        for (i = 0; i < TERN_ARRAY_LEN(nav_builtin_types); i++)
            nav_comp_add(&c, nav_builtin_types[i], 25, "built-in type");
        for (i = 0; i < TERN_ARRAY_LEN(nav_keywords); i++)
            nav_comp_add(&c, nav_keywords[i], 14, NULL);
    }
    jw_array_end(&w);
    jw_object_end(&w);
    map_free(&c.seen);
}

/* ======================================================== instance tree === */

typedef struct NavTree {
    LspServer *srv;
    JsonWriter *w;
    size_t nodes;
} NavTree;

static void nav_tree_node(NavTree *t, const IrModule *m, const IrInstance *inst, int depth)
{
    JsonWriter *w = t->w;
    uint32_t i;
    t->nodes++;
    jw_object_begin(w);
    jw_kv_str(w, "name", inst ? inst->name : m->tern_name ? m->tern_name : m->name);
    jw_kv_str(w, "module", m->tern_name ? m->tern_name : m->name);
    jw_kv_str(w, "display", m->display ? m->display : m->name);
    jw_kv_str(w, "emitted", m->name);
    if (m->package && m->package[0])
        jw_kv_str(w, "package", m->package);
    jw_key(w, "params");
    jw_array_begin(w);
    for (i = 0; i < m->nparams; i++) {
        BigInt b = {0};
        char *s;
        bn_from_limbs(&b, m->params[i].value.w, m->params[i].value.width, m->params[i].is_signed);
        s = bn_to_str(&b, 10);
        jw_object_begin(w);
        jw_kv_str(w, "name", m->params[i].name);
        jw_kv_str(w, "value", s);
        jw_object_end(w);
        free(s);
        bn_free(&b);
    }
    jw_array_end(w);
    if (m->is_extern)
        jw_kv_bool(w, "extern", 1);
    jw_key(w, "location");
    if (!lsp_w_location(t->srv, w, inst ? inst->loc : m->loc))
        jw_null(w);
    jw_key(w, "definition");
    if (!lsp_w_location(t->srv, w, m->loc))
        jw_null(w);
    jw_key(w, "children");
    jw_array_begin(w);
    if (depth < 64)
        for (i = 0; i < m->ninsts && t->nodes < 20000; i++)
            if (m->insts[i].module)
                nav_tree_node(t, m->insts[i].module, &m->insts[i], depth + 1);
    jw_array_end(w);
    jw_object_end(w);
}

void lsp_nav_instance_tree(LspServer *srv, LspGroup *only, StrBuf *out)
{
    JsonWriter w;
    NavTree t;
    size_t g;
    jw_init(&w, out, 0);
    memset(&t, 0, sizeof t);
    t.srv = srv;
    t.w = &w;
    jw_array_begin(&w);
    for (g = 0; g < srv->groups.len; g++) {
        LspGroup *gr = srv->groups.data[g];
        Map child = {0};
        uint32_t m, i;
        if ((only && gr != only) || !gr->design)
            continue;
        for (m = 0; m < gr->design->nmodules; m++)
            for (i = 0; i < gr->design->modules[m]->ninsts; i++)
                map_put(&child, MAP_PTR(gr->design->modules[m]->insts[i].module), 1);
        /* roots: modules nobody instantiates (the top first) */
        for (m = gr->design->nmodules; m-- > 0;) {
            IrModule *mod = gr->design->modules[m];
            if (!map_has(&child, MAP_PTR(mod)))
                nav_tree_node(&t, mod, NULL, 0);
        }
        map_free(&child);
    }
    jw_array_end(&w);
}
