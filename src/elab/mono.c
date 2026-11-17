/* mono.c — generics: binding generic parameters (positional/named
 * arguments, defaults that refer to earlier generics, `where` clauses),
 * monomorphized names (§13.3: Fifo_u8_64), the types of generic struct /
 * enum / bundle / alias items and the types generated from a `regmap`. */
#include "elab_impl.h"

/* ------------------------------------------------------------- mangling */

static void mono_sanitize(StrBuf *sb, size_t from)
{
    size_t i;
    for (i = from; i < sb->len; i++)
        if (!tern_is_ident((unsigned char)sb->data[i]))
            sb->data[i] = '_';
}

static const char *mono_mangle(const char *base, const ElabArgs *args)
{
    StrBuf sb = {0};
    uint32_t i;
    const char *r;
    sb_puts(&sb, base);
    for (i = 0; args && i < args->n; i++) {
        size_t at;
        sb_putc(&sb, '_');
        at = sb.len;
        val_mangle(&args->items[i].val, &sb);
        mono_sanitize(&sb, at);
    }
    if (sb.len > 96) {
        uint64_t h = hash_bytes(sb.data, sb.len);
        sb_truncate(&sb, strlen(base));
        sb_printf(&sb, "_%08llx", (unsigned long long)(h & 0xffffffffull));
    }
    r = intern_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

/* unique key fragment of a generic argument (types by their interning key) */
static void mono_key(const Value *v, StrBuf *out)
{
    uint32_t i;
    if (v->kind == VAL_TYPE) {
        sb_puts(out, v->u.type->key);
        return;
    }
    if ((v->kind == VAL_ARRAY || v->kind == VAL_STRUCT || v->kind == VAL_TUPLE) && v->u.list) {
        sb_putc(out, '[');
        for (i = 0; i < v->u.list->n; i++) {
            mono_key(&v->u.list->v[i], out);
            sb_putc(out, ',');
        }
        sb_putc(out, ']');
        return;
    }
    if (v->kind == VAL_ENUM && v->ty)
        sb_printf(out, "%s.", v->ty->key);
    val_mangle(v, out);
}

const char *el_mangle_module(Elab *el, Item *item, const ElabArgs *args)
{
    TERN_UNUSED(el);
    return mono_mangle(item->name.name, args);
}

static const char *mono_display(const char *base, const ElabArgs *args)
{
    StrBuf sb = {0};
    uint32_t i;
    const char *r;
    sb_puts(&sb, base);
    if (args && args->n) {
        sb_putc(&sb, '<');
        for (i = 0; i < args->n; i++) {
            if (i)
                sb_puts(&sb, ", ");
            val_print(&args->items[i].val, &sb);
        }
        sb_putc(&sb, '>');
    }
    r = intern_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

const char *el_ctx_prefix(Env *ctx)
{
    if (ctx && ctx->mod && ctx->mod->ir)
        return ctx->mod->ir->name;
    return "";
}

/* ------------------------------------------------------------- generics */

int el_item_needs_args(Item *it)
{
    GenericParams *gp;
    uint32_t i;
    switch (it->kind) {
    case ITEM_STRUCT: gp = &it->u.struct_def.generics; break;
    case ITEM_ENUM: gp = &it->u.enum_def.generics; break;
    case ITEM_BUNDLE: gp = &it->u.bundle.generics; break;
    case ITEM_TYPE: gp = &it->u.type_alias.generics; break;
    case ITEM_MODULE: gp = &it->u.module.generics; break;
    case ITEM_EXTERN: gp = &it->u.extern_mod.generics; break;
    case ITEM_FN: gp = &it->u.fn.generics; break;
    default: return 0;
    }
    for (i = 0; i < gp->n; i++)
        if (!gp->items[i].default_type && !gp->items[i].default_value)
            return 1;
    return 0;
}

Env *el_bind_generics(Elab *el, GenericParams *gp, const char *owner, FileUnit *file, Env *ctx,
                      GenericArg *args, uint32_t nargs, Env *argenv, const ElabArgs *vals, Span at,
                      ElabArgs *out)
{
    Env *genv = elab_env_new(el, ctx, file);
    uint32_t i, j, npos = 0;
    int ok = 1;
    uint32_t given_n = args ? nargs : vals ? vals->n : 0;
    int *used = (int *)xcalloc(given_n ? given_n : 1, sizeof(int));
    el_env_keep(el, genv);
    genv->mod = ctx ? ctx->mod : NULL;
    if (out) {
        out->n = gp->n;
        out->items = ANEWN(&el->arena, ElabArg, gp->n ? gp->n : 1);
    }
    for (i = 0; i < given_n; i++) {
        const char *nm = args ? args[i].name.name : vals->items[i].name;
        if (nm) {
            for (j = 0; j < gp->n; j++)
                if (gp->items[j].name.name == nm)
                    break;
            if (j == gp->n) {
                el_error(el, args ? args[i].span : at, "`%s` has no generic parameter `%s`", owner, nm);
                ok = 0;
            }
        } else {
            npos++;
        }
    }
    if (npos > gp->n) {
        el_error(el, at, "too many generic arguments for `%s`: %u given, %u expected", owner, (unsigned)npos,
                 (unsigned)gp->n);
        ok = 0;
    }
    for (i = 0; ok && i < gp->n; i++) {
        GenericParam *p = &gp->items[i];
        int found = -1;
        Value v = val_error();
        Ty *pt = NULL;
        uint32_t pos = 0;
        /* named argument, else the i-th positional one */
        for (j = 0; j < given_n; j++) {
            const char *nm = args ? args[j].name.name : vals->items[j].name;
            if (nm && nm == p->name.name) {
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            uint32_t k = 0;
            for (j = 0; j < given_n; j++) {
                const char *nm = args ? args[j].name.name : vals->items[j].name;
                if (nm)
                    continue;
                if (k == i - pos) {
                    /* positional slot i counts the positional arguments seen so far */
                }
                k++;
            }
            /* positional arguments fill parameters in order, skipping named ones */
            k = 0;
            for (j = 0; j < given_n; j++) {
                const char *nm = args ? args[j].name.name : vals->items[j].name;
                if (nm || used[j])
                    continue;
                found = (int)j;
                break;
            }
            /* only take it if no later named argument names this parameter */
            TERN_UNUSED(k);
        }
        if (found >= 0)
            used[found] = 1;
        if (p->kind == GENERIC_VALUE && p->type) {
            pt = elab_eval_type(el, p->type, genv);
            if (pt->kind == TY_ERROR) {
                ok = 0;
                break;
            }
        }
        if (found >= 0) {
            if (args) {
                GenericArg *ga = &args[found];
                if (p->kind == GENERIC_TYPE) {
                    if (!ga->type) {
                        el_error(el, ga->span, "generic `%s` of `%s` expects a type", p->name.name, owner);
                        ok = 0;
                        break;
                    }
                    {
                        Ty *t = elab_eval_type(el, ga->type, argenv);
                        if (t->kind == TY_ERROR) {
                            ok = 0;
                            break;
                        }
                        v = val_type(el->S, t);
                    }
                } else {
                    v = elab_eval_const(el, ga->expr, argenv, pt, "a generic argument");
                    if (v.kind == VAL_TYPE && pt && pt->kind != TY_TYPE) {
                        el_error(el, ga->span, "generic `%s` of `%s` expects a value of type %s, found a type",
                                 p->name.name, owner, ty_str(pt));
                        ok = 0;
                        break;
                    }
                    if (pt && v.kind != VAL_ERROR)
                        v = elab_convert(el, v, pt, ga->expr,
                                         arena_printf(&el->arena, "generic `%s`", p->name.name), ga->span);
                }
            } else {
                v = vals->items[found].val;
                if (p->kind == GENERIC_TYPE && v.kind != VAL_TYPE) {
                    el_error(el, at, "generic `%s` of `%s` expects a type", p->name.name, owner);
                    ok = 0;
                    break;
                }
                if (p->kind == GENERIC_VALUE && pt && v.kind != VAL_ERROR)
                    v = elab_convert(el, v, pt, NULL, arena_printf(&el->arena, "generic `%s`", p->name.name), at);
            }
        } else if (p->kind == GENERIC_TYPE && p->default_type) {
            Ty *t = elab_eval_type(el, p->default_type, genv);
            v = t->kind == TY_ERROR ? val_error() : val_type(el->S, t);
        } else if (p->kind == GENERIC_VALUE && p->default_value) {
            /* a module's default may read the instantiation's clock (clock_hz) */
            Item *gi = el->gdef_item;
            Expr *gc = el->gdef_call;
            Env *ge = el->gdef_env;
            Item *bi = el->bind_item;
            el->gdef_item = el->bind_item && el->bind_item->name.name == owner ? el->bind_item : NULL;
            el->gdef_call = el->inst_call;
            el->gdef_env = el->inst_env;
            el->bind_item = NULL;
            v = elab_eval_const(el, p->default_value, genv, pt, "a generic default");
            el->bind_item = bi;
            el->gdef_item = gi;
            el->gdef_call = gc;
            el->gdef_env = ge;
            if (pt && v.kind != VAL_ERROR)
                v = elab_convert(el, v, pt, p->default_value, NULL, p->default_value->span);
        } else {
            el_error(el, at, "missing generic argument `%s` for `%s`", p->name.name, owner);
            el_help(el, "give it %s: `%s<%s%s ..>`", p->kind == GENERIC_TYPE ? "a type" : "a value", owner,
                    p->name.name, p->kind == GENERIC_TYPE ? ": u8," : ": 4,");
            ok = 0;
            break;
        }
        if (v.kind == VAL_ERROR) {
            ok = 0;
            break;
        }
        {
            Binding *b = elab_env_bind(el, genv, p->name.name, v, p->name.span);
            b->sem = SEM_GENERIC;
        }
        if (out) {
            out->items[i].name = p->name.name;
            out->items[i].val = v;
            out->items[i].span = at;
        }
    }
    free(used);
    if (!ok)
        return NULL; /* kept: freed by elab_free */
    return genv;
}

int el_check_where(Elab *el, WhereList *w, Env *genv, const char *owner, Span at)
{
    uint32_t i;
    int ok = 1;
    for (i = 0; i < w->n; i++) {
        Value c = elab_eval_const(el, w->items[i], genv, el->S->t_bool, "a `where` constraint");
        if (c.kind == VAL_ERROR) {
            ok = 0;
            continue;
        }
        if (c.kind != VAL_BOOL) {
            el_error(el, w->items[i]->span, "`where` constraint must be bool");
            ok = 0;
            continue;
        }
        if (!c.u.i) {
            StrBuf sb = {0};
            uint32_t k;
            el_error(el, at, "`where` constraint `%s` of `%s` is not satisfied", el_text(w->items[i]), owner);
            for (k = 0; k < genv->n; k++) {
                if (genv->b[k].sem != SEM_GENERIC)
                    continue;
                if (sb.len)
                    sb_puts(&sb, ", ");
                sb_printf(&sb, "%s = ", genv->b[k].name);
                val_print(&genv->b[k].val, &sb);
            }
            el_note(el, w->items[i]->span, "constraint declared here%s%s", sb.len ? " (with " : "",
                    sb.len ? arena_printf(&el->arena, "%s)", sb_cstr(&sb)) : "");
            sb_free(&sb);
            ok = 0;
        }
    }
    return ok;
}

static ValModule *mono_make_module(Elab *el, Item *item, FileUnit *file, Env *genv, ElabArgs *args, Span at)
{
    ValModule *vm = ANEW(&el->arena, ValModule);
    GenericParams *gp = item->kind == ITEM_MODULE ? &item->u.module.generics : &item->u.extern_mod.generics;
    WhereList *w = item->kind == ITEM_MODULE ? &item->u.module.where : &item->u.extern_mod.where;
    vm->item = item;
    vm->file = file;
    vm->genv = genv;
    vm->args = *args;
    vm->span = at;
    vm->key = gp->n ? el_mangle_module(el, item, args) : item->name.name;
    if (gp->n) {
        StrBuf k = {0};
        uint32_t i;
        sb_printf(&k, "%p|", (void *)item);
        for (i = 0; i < args->n; i++) {
            mono_key(&args->items[i].val, &k);
            sb_putc(&k, ';');
        }
        vm->cache_key = intern_cstr(sb_cstr(&k));
        sb_free(&k);
    } else {
        vm->cache_key = intern_cstr(arena_printf(&el->arena, "%p|", (void *)item));
    }
    vm->display = mono_display(item->name.name, args);
    if (w->n && !el_check_where(el, w, genv, vm->display, at))
        return NULL;
    {
        /* `ClkHz: uint = clock_hz(clk)`: the generic's value (default or
         * explicit) is the nominal frequency of `clk` inside the module */
        uint32_t i, n = 0;
        for (i = 0; i < gp->n && i < args->n; i++)
            if (elab_clock_hz_port(gp->items[i].default_value))
                n++;
        if (n) {
            vm->hz_ports = ANEWN(&el->arena, const char *, n);
            vm->hz_vals = ANEWN(&el->arena, uint64_t, n);
            for (i = 0; i < gp->n && i < args->n; i++) {
                const char *pn = elab_clock_hz_port(gp->items[i].default_value);
                uint64_t hz = 0;
                if (pn && val_get_u64(&args->items[i].val, &hz) && hz) {
                    vm->hz_ports[vm->nhz] = pn;
                    vm->hz_vals[vm->nhz++] = hz;
                }
            }
        }
    }
    return vm;
}

const char *elab_clock_hz_port(const Expr *def)
{
    const Expr *a;
    if (!def || def->kind != EXPR_CALL || def->u.call.nargs != 1 || def->u.call.callee->kind != EXPR_PATH ||
        def->u.call.callee->u.path.nsegs != 1 || def->u.call.callee->u.path.segs[0].has_args ||
        !def->u.call.callee->u.path.segs[0].name.name ||
        strcmp(def->u.call.callee->u.path.segs[0].name.name, "clock_hz") != 0)
        return NULL;
    a = def->u.call.args[0].value;
    if (a->kind != EXPR_PATH || a->u.path.nsegs != 1 || a->u.path.segs[0].has_args)
        return NULL;
    return a->u.path.segs[0].name.name;
}

ValModule *el_bind_module_ast(Elab *el, Item *item, FileUnit *file, GenericArg *args, uint32_t nargs, Env *argenv,
                              Span at)
{
    GenericParams *gp = item->kind == ITEM_MODULE ? &item->u.module.generics : &item->u.extern_mod.generics;
    ElabArgs out;
    Env *genv;
    if (!file)
        file = sema_file_of_item(el->S, item);
    {
        Item *bi = el->bind_item;
        el->bind_item = item;
        genv = el_bind_generics(el, gp, item->name.name, file, NULL, args, nargs, argenv, NULL, at, &out);
        el->bind_item = bi;
    }
    if (!genv)
        return NULL;
    return mono_make_module(el, item, file, genv, &out, at);
}

ValModule *elab_bind_module(Elab *el, Item *item, FileUnit *file, const ElabArgs *args, Span at)
{
    GenericParams *gp = item->kind == ITEM_MODULE ? &item->u.module.generics : &item->u.extern_mod.generics;
    ElabArgs out;
    ElabArgs none;
    Env *genv;
    if (!file)
        file = sema_file_of_item(el->S, item);
    memset(&none, 0, sizeof none);
    {
        Item *bi = el->bind_item;
        el->bind_item = item;
        genv = el_bind_generics(el, gp, item->name.name, file, NULL, NULL, 0, NULL, args ? args : &none, at, &out);
        el->bind_item = bi;
    }
    if (!genv)
        return NULL;
    return mono_make_module(el, item, file, genv, &out, at);
}

ValModule *elab_eval_module(Elab *el, Expr *callee, Env *env)
{
    Value v;
    if (callee->kind == EXPR_CALL)
        callee = callee->u.call.callee;
    if (callee->kind != EXPR_PATH) {
        el_error(el, callee->span, "expected a module");
        return NULL;
    }
    v = el_eval_path(el, &callee->u.path, env, NULL, 0);
    if (v.kind == VAL_MODULE)
        return v.u.mod;
    if (v.kind == VAL_ITEM && (v.u.fn->item->kind == ITEM_MODULE || v.u.fn->item->kind == ITEM_EXTERN))
        return elab_bind_module(el, v.u.fn->item, v.u.fn->file, NULL, callee->span);
    if (v.kind != VAL_ERROR)
        el_error(el, callee->span, "`%s` is not a module", el_text(callee));
    return NULL;
}

/* ---------------------------------------------------------------- types */

static IrEncoding mono_encoding(Elab *el, Item *it)
{
    uint32_t i;
    for (i = 0; i < it->attrs.n; i++) {
        Attr *a = &it->attrs.items[i];
        if (a->name.name && strcmp(a->name.name, "encoding") == 0 && a->nargs == 1 && a->args[0].value &&
            a->args[0].value->kind == EXPR_PATH) {
            const char *n = a->args[0].value->u.path.segs[0].name.name;
            if (strcmp(n, "onehot") == 0)
                return IR_ENC_ONEHOT;
            if (strcmp(n, "gray") == 0)
                return IR_ENC_GRAY;
            if (strcmp(n, "binary") == 0)
                return IR_ENC_BINARY;
            el_error(el, a->args[0].value->span, "unknown encoding `%s` (binary, onehot, gray)", n);
        }
    }
    return IR_ENC_BINARY;
}

static Ty *mono_build(Elab *el, Item *it, FileUnit *file, Env *ctx, Env *genv, ElabArgs *args, Span at)
{
    Sema *S = el->S;
    StrBuf key = {0}, name = {0}, emit = {0};
    const char *prefix = el_ctx_prefix(ctx);
    uint32_t i;
    Ty *t = NULL;
    Value *gv = NULL;
    TERN_UNUSED(at);
    if (it->kind == ITEM_TYPE) {
        t = elab_eval_type(el, it->u.type_alias.type, genv);
        return t;
    }
    sb_printf(&key, "%d|%p|%s|", (int)it->kind, (void *)it, prefix);
    for (i = 0; i < args->n; i++) {
        mono_key(&args->items[i].val, &key);
        sb_putc(&key, ';');
    }
    t = ty_find(S, sb_cstr(&key));
    if (t) {
        sb_free(&key);
        return t;
    }
    sb_puts(&name, mono_display(it->name.name, args));
    if (prefix[0])
        sb_printf(&emit, "%s_", prefix);
    sb_puts(&emit, mono_mangle(it->name.name, args));
    if (args->n) {
        gv = ANEWN(&S->arena, Value, args->n);
        for (i = 0; i < args->n; i++)
            gv[i] = args->items[i].val;
    }
    if (it->kind == ITEM_STRUCT || it->kind == ITEM_BUNDLE) {
        Field **fs = it->kind == ITEM_STRUCT ? it->u.struct_def.fields : it->u.bundle.fields;
        uint32_t n = it->kind == ITEM_STRUCT ? it->u.struct_def.nfields : it->u.bundle.nfields;
        TyField *tf = (TyField *)xcalloc(n ? n : 1, sizeof(TyField));
        int ok = 1;
        for (i = 0; i < n; i++) {
            tf[i].name = fs[i]->name.name;
            tf[i].ty = elab_eval_type(el, fs[i]->type, genv);
            tf[i].flip = fs[i]->flip;
            tf[i].loc = fs[i]->span;
            tf[i].doc = fs[i]->doc;
            if (tf[i].ty->kind == TY_ERROR)
                ok = 0;
        }
        if (ok) {
            if (it->kind == ITEM_STRUCT)
                t = ty_struct(S, sb_cstr(&key), sb_cstr(&name), sb_cstr(&emit), it, n, tf, it->name.span);
            else
                t = ty_bundle(S, sb_cstr(&key), sb_cstr(&name), sb_cstr(&emit), it, n, tf, it->name.span);
        } else {
            t = S->t_error;
        }
        free(tf);
    } else if (it->kind == ITEM_ENUM) {
        uint32_t n = it->u.enum_def.nvariants;
        TyVariant *tv = (TyVariant *)xcalloc(n ? n : 1, sizeof(TyVariant));
        int ok = 1;
        for (i = 0; i < n; i++) {
            Variant *v = it->u.enum_def.variants[i];
            tv[i].name = v->name.name;
            tv[i].loc = v->span;
            tv[i].angle = v->angle;
            if (v->has_fields) {
                TyField *tf = (TyField *)xcalloc(v->nfields ? v->nfields : 1, sizeof(TyField));
                StrBuf pk = {0}, pn = {0};
                uint32_t k;
                for (k = 0; k < v->nfields; k++) {
                    tf[k].name = v->fields[k]->name.name;
                    tf[k].ty = elab_eval_type(el, v->fields[k]->type, genv);
                    tf[k].loc = v->fields[k]->span;
                    if (tf[k].ty->kind == TY_ERROR)
                        ok = 0;
                }
                if (ok && v->nfields) {
                    sb_printf(&pk, "%s|%s", sb_cstr(&key), v->name.name);
                    sb_printf(&pn, "%s::%s", sb_cstr(&name), v->name.name);
                    tv[i].payload = ty_struct(S, sb_cstr(&pk), sb_cstr(&pn), NULL, NULL, v->nfields, tf, v->span);
                }
                sb_free(&pk);
                sb_free(&pn);
                free(tf);
            }
        }
        if (n == 0) {
            el_error(el, it->name.span, "enum `%s` has no variants", it->name.name);
            ok = 0;
        }
        t = ok ? ty_enum(S, sb_cstr(&key), sb_cstr(&name), sb_cstr(&emit), it, n, tv, mono_encoding(el, it),
                         it->name.span)
               : S->t_error;
        free(tv);
    }
    if (t && t->kind != TY_ERROR) {
        t->file = file;
        t->genv = genv;
        t->gvals = gv;
        t->ngvals = args->n;
        el_env_keep(el, genv);
    }
    sb_free(&key);
    sb_free(&name);
    sb_free(&emit);
    return t ? t : S->t_error;
}

static GenericParams *mono_generics(Item *it)
{
    switch (it->kind) {
    case ITEM_STRUCT: return &it->u.struct_def.generics;
    case ITEM_ENUM: return &it->u.enum_def.generics;
    case ITEM_BUNDLE: return &it->u.bundle.generics;
    case ITEM_TYPE: return &it->u.type_alias.generics;
    default: return NULL;
    }
}

Ty *el_type_item(Elab *el, Item *it, FileUnit *file, Env *ctx, GenericArg *args, uint32_t nargs, Env *argenv,
                 Span at, Ty *expect)
{
    GenericParams *gp = mono_generics(it);
    ElabArgs out;
    Env *genv;
    if (!gp)
        return el->S->t_error;
    if (!file)
        file = sema_file_of_item(el->S, it);
    if (nargs == 0 && expect && expect->def == it)
        return expect;
    genv = el_bind_generics(el, gp, it->name.name, file, ctx, args, nargs, argenv, NULL, at, &out);
    if (!genv)
        return el->S->t_error;
    return mono_build(el, it, file, ctx, genv, &out, at);
}

Ty *el_type_item_vals(Elab *el, Item *it, FileUnit *file, Env *ctx, const ElabArgs *args, Span at)
{
    GenericParams *gp = mono_generics(it);
    ElabArgs out;
    Env *genv;
    if (!gp)
        return el->S->t_error;
    if (!file)
        file = sema_file_of_item(el->S, it);
    genv = el_bind_generics(el, gp, it->name.name, file, ctx, NULL, 0, NULL, args, at, &out);
    if (!genv)
        return el->S->t_error;
    return mono_build(el, it, file, ctx, genv, &out, at);
}

/* -------------------------------------------------------------- regmaps */

static const char *mono_capitalize(const char *s)
{
    char buf[256];
    size_t i = 0, j = 0;
    int up = 1;
    for (; s[i] && j + 1 < sizeof buf; i++) {
        if (s[i] == '_') {
            up = 1;
            continue;
        }
        buf[j++] = up ? (char)tern_to_upper((unsigned char)s[i]) : s[i];
        up = 0;
    }
    buf[j] = 0;
    return intern_cstr(buf);
}

static Ty *mono_reg_type(Elab *el, Item *rm, FileUnit *file, RegEntry *r, Env *env)
{
    Sema *S = el->S;
    if (r->kind == REG_SCALAR)
        return elab_eval_type(el, r->type, env);
    if (r->kind == REG_STREAM)
        return elab_eval_type(el, r->type, env);
    {
        uint32_t i, n = r->nfields;
        TyField *tf = (TyField *)xcalloc(n ? n : 1, sizeof(TyField));
        uint32_t *pos = (uint32_t *)xcalloc(n ? n : 1, sizeof(uint32_t));
        StrBuf key = {0}, name = {0}, emit = {0};
        Ty *t;
        int ok = 1;
        for (i = 0; i < n; i++) {
            RegField *f = r->fields[i];
            Value hi = elab_eval_const(el, f->bit_hi, env, S->t_int, "a bit position");
            Value lo = f->bit_lo ? elab_eval_const(el, f->bit_lo, env, S->t_int, "a bit position") : hi;
            int64_t h = 0, l = 0;
            val_get_i64(&hi, &h);
            val_get_i64(&lo, &l);
            if (h < l) {
                int64_t x = h;
                h = l;
                l = x;
            }
            tf[i].name = f->name.name;
            tf[i].ty = elab_eval_type(el, f->type, env);
            tf[i].loc = f->span;
            tf[i].doc = f->doc;
            pos[i] = (uint32_t)l;
            if (tf[i].ty->kind == TY_ERROR)
                ok = 0;
            else if (tf[i].ty->width != (uint32_t)(h - l + 1)) {
                el_error(el, f->span, "field `%s` is %u bits wide but its position covers %u bits", f->name.name,
                         (unsigned)tf[i].ty->width, (unsigned)(h - l + 1));
                ok = 0;
            }
        }
        /* MSB first */
        {
            uint32_t a, b;
            for (a = 0; a < n; a++)
                for (b = a + 1; b < n; b++)
                    if (pos[b] > pos[a]) {
                        TyField x = tf[a];
                        uint32_t p = pos[a];
                        tf[a] = tf[b];
                        pos[a] = pos[b];
                        tf[b] = x;
                        pos[b] = p;
                    }
        }
        sb_printf(&key, "regmap|%p|%s", (void *)rm, r->name.name);
        sb_printf(&name, "%s::%s", rm->name.name, mono_capitalize(r->name.name));
        sb_printf(&emit, "%s_%s", rm->name.name, mono_capitalize(r->name.name));
        t = ok ? ty_struct(S, sb_cstr(&key), sb_cstr(&name), sb_cstr(&emit), NULL, n, tf, r->span) : S->t_error;
        if (t->kind != TY_ERROR)
            t->file = file;
        free(tf);
        free(pos);
        sb_free(&key);
        sb_free(&name);
        sb_free(&emit);
        return t;
    }
}

Ty *el_regmap_type(Elab *el, Item *rm, FileUnit *file, const char *name, Span at)
{
    Sema *S = el->S;
    uint32_t i;
    Env *env = elab_env_new(el, NULL, file ? file : sema_file_of_item(S, rm));
    Ty *r = NULL;
    if (strcmp(name, "Regs") == 0) {
        uint32_t n = rm->u.regmap.nentries, nf = 0;
        TyField *tf = (TyField *)xcalloc(2 * n + 1, sizeof(TyField));
        StrBuf key = {0}, nm = {0}, emit = {0};
        int ok = 1;
        for (i = 0; i < n; i++) {
            RegEntry *e = rm->u.regmap.entries[i];
            Ty *t = mono_reg_type(el, rm, file, e, env);
            const char *acc = e->access.name ? e->access.name : "rw";
            if (t->kind == TY_ERROR) {
                ok = 0;
                continue;
            }
            if (e->kind == REG_STREAM) {
                /* the stream the register file produces */
                Ty *st = elab_eval_type(el, e->stream_type, env);
                if (st->kind == TY_ERROR) {
                    ok = 0;
                    continue;
                }
                t = st;
            }
            /* the register file drives rw/wo/w1c/rw1s values (outputs) and the
             * hardware provides ro values (inputs: flip); w1c registers also
             * take `<name>_set` (bits the hardware sets) and rw1s registers
             * `<name>_clr` (bits the hardware clears) */
            tf[nf].name = e->name.name;
            tf[nf].ty = t;
            tf[nf].loc = e->span;
            tf[nf].doc = e->doc;
            tf[nf].flip = strcmp(acc, "ro") == 0;
            nf++;
            if (strcmp(acc, "w1c") == 0 || strcmp(acc, "rw1s") == 0) {
                tf[nf].name = intern_cstr(arena_printf(&el->arena, "%s_%s", e->name.name, acc[0] == 'w' ? "set" : "clr"));
                tf[nf].ty = t;
                tf[nf].loc = e->span;
                tf[nf].flip = 1;
                nf++;
            }
        }
        sb_printf(&key, "regmap|%p|Regs", (void *)rm);
        sb_printf(&nm, "%s::Regs", rm->name.name);
        sb_printf(&emit, "%s_Regs", rm->name.name);
        r = ok ? ty_bundle(S, sb_cstr(&key), sb_cstr(&nm), sb_cstr(&emit), rm, nf, tf, rm->name.span) : S->t_error;
        if (r->kind != TY_ERROR && !r->file)
            r->file = file ? file : sema_file_of_item(S, rm);
        free(tf);
        sb_free(&key);
        sb_free(&nm);
        sb_free(&emit);
    } else if (strncmp(name, "__stream_", 9) == 0) {
        /* the stream type of a `wo T -> Stream<T>` entry (generated RegisterFile ports) */
        for (i = 0; i < rm->u.regmap.nentries; i++) {
            RegEntry *e = rm->u.regmap.entries[i];
            if (e->kind == REG_STREAM && strcmp(e->name.name, name + 9) == 0) {
                r = elab_eval_type(el, e->stream_type, env);
                break;
            }
        }
        if (!r) {
            el_error(el, at, "regmap `%s` has no stream register `%s`", rm->name.name, name + 9);
            r = S->t_error;
        }
    } else {
        for (i = 0; i < rm->u.regmap.nentries; i++) {
            RegEntry *e = rm->u.regmap.entries[i];
            if (mono_capitalize(e->name.name) == intern_cstr(name) || e->name.name == intern_cstr(name)) {
                r = mono_reg_type(el, rm, file, e, env);
                break;
            }
        }
        if (!r) {
            el_error(el, at, "regmap `%s` has no register `%s`", rm->name.name, name);
            r = S->t_error;
        }
    }
    elab_env_free(env);
    return r;
}
