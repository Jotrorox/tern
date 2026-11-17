/* syninfo.c — semantic info from the resolver alone (see sema.h,
 * sema_info_syntactic): declarations with their declared types as written,
 * and the definition of every name use that lexical scoping and package
 * resolution can bind, for code that elaboration never reached (generic
 * modules without instances, unused fns). Records made by the elaborator
 * win: sema_info_record only fills fields that are still unknown. */
#include "sema.h"
#include "../base/diag.h"

typedef struct SynEnt {
    const char *name;
    Span def;
    SemKind kind;
    const char *type;
} SynEnt;

typedef struct SynWalk {
    Sema *S;
    FileUnit *f;
    VEC(SynEnt) scope;
} SynWalk;

static void syn_expr(SynWalk *w, Expr *e);
static void syn_type(SynWalk *w, Type *t);
static void syn_block(SynWalk *w, Block *b, int module_level);
static void syn_stmt(SynWalk *w, Stmt *s, int module_level);
static void syn_item(SynWalk *w, Item *it);

/* source text of a span, interned */
static const char *syn_text(Span s)
{
    const SourceFile *sf = s.file ? src_get(s.file) : NULL;
    if (!sf || (size_t)s.off + s.len > sf->len || !s.len || s.len > 200)
        return NULL;
    return intern(sf->text + s.off, s.len);
}

static const char *syn_type_text(Type *t)
{
    return t && t->kind != TYPE_ERROR ? syn_text(t->span) : NULL;
}

static void syn_record(SynWalk *w, Span at, Span def, SemKind k, const char *name, const char *type)
{
    SemInfo si;
    if (!at.file || !name)
        return;
    memset(&si, 0, sizeof si);
    si.span = at;
    si.def = def;
    si.kind = k;
    si.name = name;
    si.type = type;
    sema_info_record(w->S, &si);
}

static void syn_declare(SynWalk *w, Ident id, SemKind k, const char *type)
{
    SynEnt e;
    if (!id.name)
        return;
    syn_record(w, id.span, id.span, k, id.name, type);
    e.name = id.name;
    e.def = id.span;
    e.kind = k;
    e.type = type;
    vec_push(w->scope, e);
}

static const SynEnt *syn_find(SynWalk *w, const char *name)
{
    size_t i = w->scope.len;
    while (i-- > 0)
        if (w->scope.data[i].name == name)
            return &w->scope.data[i];
    return NULL;
}

static SemKind syn_item_kind(const Item *it)
{
    switch (it->kind) {
    case ITEM_MODULE: case ITEM_EXTERN: return SEM_MODULE;
    case ITEM_FN: return SEM_FN;
    case ITEM_CONST: return SEM_CONST;
    default: return SEM_TYPE;
    }
}

static const char *syn_item_type(const Item *it)
{
    if (it->kind == ITEM_CONST)
        return syn_type_text(it->u.const_def.type);
    return NULL;
}

static void syn_gargs(SynWalk *w, PathSeg *s)
{
    uint32_t i;
    for (i = 0; i < s->nargs; i++) {
        if (s->args[i].type && s->args[i].type->kind != TYPE_PATH)
            syn_type(w, s->args[i].type);
        else
            syn_expr(w, s->args[i].expr);
    }
}

/* a path: the first segment through the lexical scopes, else the file
 * scope; later segments as package members */
static void syn_path(SynWalk *w, Path *p)
{
    Symbol sym;
    uint32_t i = 0;
    const SynEnt *loc;
    const char *n0 = p->segs[0].name.name;
    if (!n0)
        return;
    loc = syn_find(w, n0);
    if (loc) {
        syn_record(w, p->segs[0].name.span, loc->def, loc->kind, n0, loc->type);
        for (i = 0; i < p->nsegs; i++)
            syn_gargs(w, &p->segs[i]);
        return;
    }
    memset(&sym, 0, sizeof sym);
    if (sema_lookup(w->S, w->f, n0, &sym)) {
        for (;;) {
            PathSeg *s = &p->segs[i];
            if (sym.kind == SYM_ITEM && sym.item) {
                syn_record(w, s->name.span, sym.item->name.span, syn_item_kind(sym.item), s->name.name,
                           syn_item_type(sym.item));
            } else if (sym.kind == SYM_PACKAGE) {
                SemInfo si;
                memset(&si, 0, sizeof si);
                si.span = s->name.span;
                si.kind = SEM_PACKAGE;
                si.name = sym.pkg ? sym.pkg->path : s->name.name;
                sema_info_record(w->S, &si);
            }
            syn_gargs(w, s);
            i++;
            if (i >= p->nsegs || sym.kind != SYM_PACKAGE || !p->segs[i].name.name)
                break;
            if (!sema_lookup_member(w->S, w->f, sym.pkg, p->segs[i].name.name, &sym, NULL))
                break;
        }
    }
    for (; i < p->nsegs; i++)
        syn_gargs(w, &p->segs[i]);
}

static void syn_type(SynWalk *w, Type *t)
{
    uint32_t i;
    if (!t)
        return;
    switch (t->kind) {
    case TYPE_PATH: syn_path(w, &t->u.path); break;
    case TYPE_ARRAY: syn_type(w, t->u.array.elem); syn_expr(w, t->u.array.len); break;
    case TYPE_STRUCT:
        for (i = 0; i < t->u.anon_struct.nfields; i++) {
            Field *f = t->u.anon_struct.fields[i];
            syn_record(w, f->name.span, f->name.span, SEM_FIELD, f->name.name, syn_type_text(f->type));
            syn_type(w, f->type);
        }
        break;
    case TYPE_TYPEOF: syn_expr(w, t->u.typeof_expr); break;
    case TYPE_TUPLE:
        for (i = 0; i < t->u.tuple.n; i++)
            syn_type(w, t->u.tuple.elems[i]);
        break;
    case TYPE_ERROR: break;
    }
}

/* binds the names a pattern introduces; names that resolve to consts are uses */
static void syn_pattern(SynWalk *w, Pattern *p, SemKind k)
{
    uint32_t i;
    if (!p)
        return;
    switch (p->kind) {
    case PAT_BIND: {
        Symbol sym;
        const SynEnt *loc = syn_find(w, p->u.bind.name);
        if (loc && (loc->kind == SEM_CONST || loc->kind == SEM_GENERIC)) {
            syn_record(w, p->u.bind.span, loc->def, loc->kind, p->u.bind.name, loc->type);
        } else if (!loc && p->u.bind.name && sema_lookup(w->S, w->f, p->u.bind.name, &sym) && sym.kind == SYM_ITEM &&
                   sym.item->kind == ITEM_CONST) {
            syn_record(w, p->u.bind.span, sym.item->name.span, SEM_CONST, p->u.bind.name, syn_item_type(sym.item));
        } else {
            syn_declare(w, p->u.bind, k, NULL);
        }
        break;
    }
    case PAT_LIT: syn_expr(w, p->u.lit); break;
    case PAT_RANGE: syn_expr(w, p->u.range.lo); syn_expr(w, p->u.range.hi); break;
    case PAT_VARIANT:
        for (i = 0; i < p->u.variant.nfields; i++) {
            FieldPat *fp = &p->u.variant.fields[i];
            if (fp->pat)
                syn_pattern(w, fp->pat, k);
            else
                syn_declare(w, fp->name, k, NULL);
        }
        break;
    case PAT_PATH: syn_path(w, &p->u.path); break;
    case PAT_STRUCT:
        syn_path(w, &p->u.struct_pat.path);
        for (i = 0; i < p->u.struct_pat.nfields; i++) {
            FieldPat *fp = &p->u.struct_pat.fields[i];
            if (fp->pat)
                syn_pattern(w, fp->pat, k);
            else
                syn_declare(w, fp->name, k, NULL);
        }
        break;
    case PAT_TUPLE:
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            syn_pattern(w, p->u.list.items[i], k);
        break;
    case PAT_WILDCARD: case PAT_ERROR: break;
    }
}

static void syn_expr(SynWalk *w, Expr *e)
{
    uint32_t i;
    size_t mark;
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_INT: syn_expr(w, e->u.int_lit.width); break;
    case EXPR_PATH: syn_path(w, &e->u.path); break;
    case EXPR_VARIANT:
        for (i = 0; i < e->u.variant.nfields; i++) {
            FieldInit *fi = &e->u.variant.fields[i];
            if (fi->value)
                syn_expr(w, fi->value);
            else if (fi->name.name) {
                Path p;
                PathSeg s;
                memset(&s, 0, sizeof s);
                s.name = fi->name;
                s.span = fi->name.span;
                p.segs = &s;
                p.nsegs = 1;
                p.span = fi->name.span;
                syn_path(w, &p);
            }
        }
        break;
    case EXPR_STRUCT:
        syn_path(w, &e->u.struct_lit.path);
        for (i = 0; i < e->u.struct_lit.nfields; i++) {
            FieldInit *fi = &e->u.struct_lit.fields[i];
            if (fi->value)
                syn_expr(w, fi->value);
            else if (fi->name.name) {
                const SynEnt *loc = syn_find(w, fi->name.name);
                if (loc)
                    syn_record(w, fi->name.span, loc->def, loc->kind, fi->name.name, loc->type);
            }
        }
        break;
    case EXPR_FIELD:
        syn_expr(w, e->u.field.base);
        for (i = 0; i < e->u.field.nargs; i++)
            syn_expr(w, e->u.field.args[i].expr);
        break;
    case EXPR_INDEX: syn_expr(w, e->u.index.base); syn_expr(w, e->u.index.index); break;
    case EXPR_SLICE: syn_expr(w, e->u.slice.base); syn_expr(w, e->u.slice.hi); syn_expr(w, e->u.slice.lo); break;
    case EXPR_CALL:
        syn_expr(w, e->u.call.callee);
        for (i = 0; i < e->u.call.nargs; i++)
            syn_expr(w, e->u.call.args[i].value);
        syn_expr(w, e->u.call.domain);
        break;
    case EXPR_CAST: syn_expr(w, e->u.cast.expr); syn_type(w, e->u.cast.type); break;
    case EXPR_UNARY: syn_expr(w, e->u.unary.operand); break;
    case EXPR_BINARY: syn_expr(w, e->u.binary.lhs); syn_expr(w, e->u.binary.rhs); break;
    case EXPR_RANGE: syn_expr(w, e->u.range.lo); syn_expr(w, e->u.range.hi); break;
    case EXPR_IF:
        syn_expr(w, e->u.if_expr.cond);
        syn_block(w, e->u.if_expr.then_block, 0);
        syn_expr(w, e->u.if_expr.else_expr);
        break;
    case EXPR_MATCH:
        syn_expr(w, e->u.match.scrutinee);
        for (i = 0; i < e->u.match.narms; i++) {
            mark = w->scope.len;
            syn_pattern(w, e->u.match.arms[i].pat, SEM_LOCAL);
            syn_stmt(w, e->u.match.arms[i].body, 0);
            w->scope.len = mark;
        }
        break;
    case EXPR_BLOCK: syn_block(w, e->u.block, 0); break;
    case EXPR_TUPLE:
    case EXPR_ARRAY:
        for (i = 0; i < e->u.list.n; i++)
            syn_expr(w, e->u.list.items[i]);
        break;
    case EXPR_PAREN: case EXPR_SIM: syn_expr(w, e->u.inner); break;
    case EXPR_TYPE: syn_type(w, e->u.type); break;
    case EXPR_TEMPORAL: syn_expr(w, e->u.temporal.bound); syn_expr(w, e->u.temporal.body); break;
    case EXPR_DELAY:
        syn_expr(w, e->u.delay.lhs);
        syn_expr(w, e->u.delay.lo);
        syn_expr(w, e->u.delay.hi);
        syn_expr(w, e->u.delay.rhs);
        break;
    case EXPR_QPATH:
        syn_type(w, e->u.qpath.type);
        for (i = 0; i < e->u.qpath.path.nsegs; i++)
            syn_gargs(w, &e->u.qpath.path.segs[i]);
        break;
    case EXPR_ERROR: case EXPR_BOOL: case EXPR_FILL: case EXPR_STRING: break;
    }
}

/* module-level declarations are visible in the whole body */
static void syn_predeclare(SynWalk *w, Stmt *s)
{
    switch (s->kind) {
    case STMT_PORT:
        syn_declare(w, s->u.port.name, s->u.port.dir == PORT_IN ? SEM_PORT_IN : SEM_PORT_OUT,
                    syn_type_text(s->u.port.type));
        break;
    case STMT_DOMAIN: syn_declare(w, s->u.domain.name, SEM_DOMAIN, NULL); break;
    case STMT_VAR: syn_declare(w, s->u.var.name, SEM_VAR, syn_type_text(s->u.var.type)); break;
    case STMT_REG: syn_declare(w, s->u.reg.name, SEM_REG, syn_type_text(s->u.reg.type)); break;
    case STMT_MEM: syn_declare(w, s->u.mem.name, SEM_MEM, syn_type_text(s->u.mem.type)); break;
    case STMT_LET:
        if (s->u.let.pat->kind == PAT_BIND)
            syn_declare(w, s->u.let.pat->u.bind, SEM_WIRE, syn_type_text(s->u.let.type));
        else
            syn_pattern(w, s->u.let.pat, SEM_WIRE);
        break;
    case STMT_ITEM:
        if (s->u.item->name.name)
            syn_declare(w, s->u.item->name, syn_item_kind(s->u.item), syn_item_type(s->u.item));
        break;
    default:
        break;
    }
}

static void syn_stmt(SynWalk *w, Stmt *s, int module_level)
{
    size_t mark;
    if (!s)
        return;
    switch (s->kind) {
    case STMT_PORT:
        syn_type(w, s->u.port.type);
        syn_expr(w, s->u.port.domain);
        syn_expr(w, s->u.port.deflt);
        break;
    case STMT_DOMAIN: syn_expr(w, s->u.domain.clock); syn_expr(w, s->u.domain.reset); break;
    case STMT_LET:
        syn_type(w, s->u.let.type);
        syn_expr(w, s->u.let.init);
        syn_expr(w, s->u.let.constraint);
        if (!module_level) {
            if (s->u.let.pat->kind == PAT_BIND)
                syn_declare(w, s->u.let.pat->u.bind, SEM_LOCAL, syn_type_text(s->u.let.type));
            else
                syn_pattern(w, s->u.let.pat, SEM_LOCAL);
        }
        break;
    case STMT_VAR:
        syn_type(w, s->u.var.type);
        syn_expr(w, s->u.var.init);
        syn_expr(w, s->u.var.constraint);
        if (!module_level)
            syn_declare(w, s->u.var.name, SEM_LOCAL, syn_type_text(s->u.var.type));
        break;
    case STMT_REG: syn_type(w, s->u.reg.type); syn_expr(w, s->u.reg.init); syn_expr(w, s->u.reg.domain); break;
    case STMT_MEM: syn_type(w, s->u.mem.type); syn_expr(w, s->u.mem.init); break;
    case STMT_ITEM:
        if (!module_level && s->u.item->name.name)
            syn_declare(w, s->u.item->name, syn_item_kind(s->u.item), syn_item_type(s->u.item));
        syn_item(w, s->u.item);
        break;
    case STMT_COMB: syn_block(w, s->u.comb, 0); break;
    case STMT_ON: syn_expr(w, s->u.on.clock); syn_block(w, s->u.on.body, 0); break;
    case STMT_ASSIGN: syn_expr(w, s->u.assign.lhs); syn_expr(w, s->u.assign.rhs); syn_expr(w, s->u.assign.constraint); break;
    case STMT_EXPR:
        if (module_level && s->u.expr->kind == EXPR_IF) {
            /* elaboration-time `if`: module statements in its blocks */
            Expr *e = s->u.expr;
            while (e && e->kind == EXPR_IF) {
                syn_expr(w, e->u.if_expr.cond);
                syn_block(w, e->u.if_expr.then_block, 1);
                e = e->u.if_expr.else_expr;
                if (e && e->kind == EXPR_BLOCK) {
                    syn_block(w, e->u.block, 1);
                    break;
                }
            }
        } else {
            syn_expr(w, s->u.expr);
        }
        break;
    case STMT_FOR:
        syn_expr(w, s->u.for_stmt.iter);
        mark = w->scope.len;
        syn_pattern(w, s->u.for_stmt.pat, SEM_CONST);
        syn_block(w, s->u.for_stmt.body, module_level);
        w->scope.len = mark;
        break;
    case STMT_WHILE: syn_expr(w, s->u.while_stmt.cond); syn_block(w, s->u.while_stmt.body, 0); break;
    case STMT_ASSERT: syn_expr(w, s->u.assert_stmt.cond); syn_expr(w, s->u.assert_stmt.msg); break;
    case STMT_STATIC_ASSERT: syn_expr(w, s->u.sassert.cond); syn_expr(w, s->u.sassert.msg); break;
    case STMT_RETURN: syn_expr(w, s->u.ret); break;
    case STMT_ERROR: break;
    }
}

static void syn_block(SynWalk *w, Block *b, int module_level)
{
    size_t mark = w->scope.len;
    uint32_t i;
    if (!b)
        return;
    if (module_level)
        for (i = 0; i < b->nstmts; i++)
            syn_predeclare(w, b->stmts[i]);
    for (i = 0; i < b->nstmts; i++)
        syn_stmt(w, b->stmts[i], module_level);
    w->scope.len = mark;
}

static void syn_generics(SynWalk *w, GenericParams *gp)
{
    uint32_t i;
    for (i = 0; i < gp->n; i++) {
        GenericParam *g = &gp->items[i];
        syn_type(w, g->type);
        syn_type(w, g->default_type);
        syn_expr(w, g->default_value);
        syn_declare(w, g->name, SEM_GENERIC, g->kind == GENERIC_TYPE ? intern_cstr("type") : syn_type_text(g->type));
    }
}

static void syn_where(SynWalk *w, WhereList *wl)
{
    uint32_t i;
    for (i = 0; i < wl->n; i++)
        syn_expr(w, wl->items[i]);
}

static void syn_fields(SynWalk *w, Field **fs, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        syn_record(w, fs[i]->name.span, fs[i]->name.span, SEM_FIELD, fs[i]->name.name, syn_type_text(fs[i]->type));
        syn_type(w, fs[i]->type);
    }
}

static void syn_item(SynWalk *w, Item *it)
{
    size_t mark = w->scope.len;
    uint32_t i;
    if (!it)
        return;
    if (it->name.name)
        syn_record(w, it->name.span, it->name.span, syn_item_kind(it), it->name.name, syn_item_type(it));
    switch (it->kind) {
    case ITEM_MODULE:
    case ITEM_EXTERN: {
        Stmt **body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
        uint32_t n = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody;
        syn_generics(w, it->kind == ITEM_MODULE ? &it->u.module.generics : &it->u.extern_mod.generics);
        syn_where(w, it->kind == ITEM_MODULE ? &it->u.module.where : &it->u.extern_mod.where);
        for (i = 0; i < n; i++)
            syn_predeclare(w, body[i]);
        for (i = 0; i < n; i++)
            syn_stmt(w, body[i], 1);
        break;
    }
    case ITEM_FN:
        syn_generics(w, &it->u.fn.generics);
        for (i = 0; i < it->u.fn.nparams; i++) {
            syn_type(w, it->u.fn.params[i].type);
            syn_declare(w, it->u.fn.params[i].name, SEM_PARAM, syn_type_text(it->u.fn.params[i].type));
        }
        syn_type(w, it->u.fn.ret);
        syn_where(w, &it->u.fn.where);
        syn_block(w, it->u.fn.body, 0);
        break;
    case ITEM_STRUCT:
        syn_generics(w, &it->u.struct_def.generics);
        syn_fields(w, it->u.struct_def.fields, it->u.struct_def.nfields);
        break;
    case ITEM_BUNDLE:
        syn_generics(w, &it->u.bundle.generics);
        syn_fields(w, it->u.bundle.fields, it->u.bundle.nfields);
        for (i = 0; i < it->u.bundle.nfns; i++)
            syn_item(w, it->u.bundle.fns[i]);
        break;
    case ITEM_ENUM:
        syn_generics(w, &it->u.enum_def.generics);
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            Variant *v = it->u.enum_def.variants[i];
            syn_record(w, v->name.span, v->name.span, SEM_VARIANT, v->name.name, NULL);
            syn_fields(w, v->fields, v->nfields);
        }
        break;
    case ITEM_CONST:
        syn_type(w, it->u.const_def.type);
        syn_expr(w, it->u.const_def.value);
        break;
    case ITEM_TYPE:
        syn_generics(w, &it->u.type_alias.generics);
        syn_type(w, it->u.type_alias.type);
        break;
    case ITEM_REGMAP:
        for (i = 0; i < it->u.regmap.nentries; i++) {
            RegEntry *e = it->u.regmap.entries[i];
            uint32_t k;
            syn_record(w, e->name.span, e->name.span, SEM_FIELD, e->name.name, syn_type_text(e->type));
            syn_expr(w, e->offset);
            syn_type(w, e->type);
            syn_type(w, e->stream_type);
            syn_expr(w, e->reset);
            for (k = 0; k < e->nfields; k++) {
                RegField *f = e->fields[k];
                syn_record(w, f->name.span, f->name.span, SEM_FIELD, f->name.name, syn_type_text(f->type));
                syn_type(w, f->type);
                syn_expr(w, f->bit_hi);
                syn_expr(w, f->bit_lo);
            }
        }
        break;
    case ITEM_TEST: syn_block(w, it->u.test.body, 0); break;
    case ITEM_PROPERTY: syn_expr(w, it->u.property.body); break;
    case ITEM_SEQUENCE: syn_expr(w, it->u.sequence.body); break;
    case ITEM_STATIC_ASSERT: syn_expr(w, it->u.sassert.cond); syn_expr(w, it->u.sassert.msg); break;
    case ITEM_ERROR: break;
    }
    w->scope.len = mark;
}

void sema_info_syntactic_file(Sema *S, FileUnit *f)
{
    SynWalk w;
    uint32_t i;
    if (!S->record_info || !f || !f->ast)
        return;
    memset(&w, 0, sizeof w);
    w.S = S;
    w.f = f;
    for (i = 0; i < f->ast->nitems; i++)
        syn_item(&w, f->ast->items[i]);
    vec_free(w.scope);
}

void sema_info_syntactic(Sema *S)
{
    size_t i;
    for (i = 0; i < S->files.len; i++)
        if (S->files.data[i]->is_user)
            sema_info_syntactic_file(S, S->files.data[i]);
}
