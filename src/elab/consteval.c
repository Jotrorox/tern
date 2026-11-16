/* consteval.c — the evaluator shared by constant evaluation, elaboration
 * and (later) the test interpreter: environments, name lookup, types,
 * implicit conversions, the operators with the width rules of Table 5.2
 * and §5.3, literals, calls (inlined fns, const fns, builtins, methods),
 * patterns and the statement executor (see elab.h for the overview).
 *
 * Hardware values (VAL_HW) are IR expressions; every IR expression built
 * here satisfies the ir.h invariants (operands resized to the result width
 * first). Module structure (signals, blocks, instances, memories,
 * properties) lives in elab.c; generic binding and types of items in
 * mono.c. */
#include "elab_impl.h"
#include "../os/os.h"

#define CE_MAX_DEPTH 200

/* ============================================================ diagnostics */

static int ce_error_seen;

void el_error(Elab *el, Span at, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    uint64_t h;
    if (el->quiet) {
        el->quiet_errors++;
        return;
    }
    if (el->mod)
        el->mod->mi->failed = 1;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    h = hash_combine(hash_bytes(buf, strlen(buf)), hash_combine(hash_u64(at.file), hash_u64(at.off))) | 1;
    if (map_has(&el->seen_errors, h)) {
        ce_error_seen = 1; /* the help/notes of a duplicate are dropped too */
        return;
    }
    map_put(&el->seen_errors, h, 1);
    ce_error_seen = 0;
    diag_error(at, "%s", buf);
}

void el_help(Elab *el, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    if (el->quiet || ce_error_seen)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_help("%s", buf);
}

void el_note(Elab *el, Span at, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    if (el->quiet || ce_error_seen)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    diag_note(at, "%s", buf);
}

void el_warning(Elab *el, Span at, const char *fmt, ...)
{
    va_list ap;
    if (el->quiet)
        return;
    va_start(ap, fmt);
    diag_vwarning(at, fmt, ap);
    va_end(ap);
}

int el_step(Elab *el, Span at)
{
    if (el->limit_hit)
        return 0;
    if (++el->steps <= el->step_limit)
        return 1;
    el->limit_hit = 1;
    if (!el->quiet) {
        diag_error(at, "compile-time evaluation exceeded the step limit (%llu steps)",
                   (unsigned long long)el->step_limit);
        diag_help("a const fn loop may not terminate; raise the limit with --const-eval-limit <n>");
    }
    return 0;
}

const char *el_span_text(Span s)
{
    const SourceFile *f = src_get(s.file);
    char buf[64];
    size_t n;
    if (!f || s.off + s.len > f->len)
        return "?";
    n = s.len;
    if (n > 48) {
        memcpy(buf, f->text + s.off, 45);
        memcpy(buf + 45, "...", 4);
        return intern_cstr(buf);
    }
    return intern(f->text + s.off, n);
}

const char *el_text(const Expr *e)
{
    return e ? el_span_text(e->span) : "?";
}

/* ======================================================== environments */

Env *elab_env_new(Elab *el, Env *parent, FileUnit *file)
{
    Env *e = (Env *)xcalloc(1, sizeof(Env));
    TERN_UNUSED(el);
    e->parent = parent;
    e->file = file ? file : parent ? parent->file : NULL;
    e->mod = parent ? parent->mod : NULL;
    e->suffix = parent ? parent->suffix : "";
    return e;
}

void el_env_keep(Elab *el, Env *e)
{
    if (!e || e->keep)
        return;
    e->keep = 1;
    vec_push(el->kept, e);
}

void elab_env_free(Env *e)
{
    if (!e || e->keep)
        return;
    free(e->b);
    free(e);
}

Binding *elab_env_bind(Elab *el, Env *e, const char *name, Value v, Span def)
{
    Binding *b;
    uint32_t i;
    TERN_UNUSED(el);
    for (i = 0; i < e->n; i++)
        if (e->b[i].name == name) {
            b = &e->b[i];
            memset(b, 0, sizeof *b);
            b->name = name;
            b->val = v;
            b->def = def;
            b->owner = e;
            return b;
        }
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->b = (Binding *)xrealloc(e->b, e->cap * sizeof(Binding));
    }
    b = &e->b[e->n++];
    memset(b, 0, sizeof *b);
    b->name = name;
    b->val = v;
    b->def = def;
    b->owner = e;
    return b;
}

static Binding *ce_env_find_local(Env *e, const char *name)
{
    uint32_t i;
    for (i = e->n; i-- > 0;)
        if (e->b[i].name == name)
            return &e->b[i];
    return NULL;
}

Binding *elab_env_find(Env *e, const char *name)
{
    for (; e; e = e->parent) {
        Binding *b = ce_env_find_local(e, name);
        if (b)
            return b;
    }
    return NULL;
}

/* ============================================================ Elab ===== */

Elab *elab_new(Sema *S, IrDesign *design)
{
    Elab *el = (Elab *)xcalloc(1, sizeof(Elab));
    el->S = S;
    el->design = design;
    el->step_limit = 10000000ull;
    return el;
}

void elab_free(Elab *el)
{
    size_t it = 0;
    uint64_t k, v;
    if (!el)
        return;
    TERN_UNUSED(k);
    TERN_UNUSED(v);
    TERN_UNUSED(it);
    {
        /* interned types may point into these scopes and values: they live
         * as long as the Sema */
        size_t i;
        Arena *a = (Arena *)xmalloc(sizeof(Arena));
        for (i = 0; i < el->kept.len; i++) {
            vec_push(el->S->retired_blocks, el->kept.data[i]->b);
            vec_push(el->S->retired_blocks, el->kept.data[i]);
        }
        vec_free(el->kept);
        *a = el->arena;
        memset(&el->arena, 0, sizeof el->arena);
        vec_push(el->S->retired_arenas, a);
    }
    map_free(&el->modules);
    map_free(&el->modtypes);
    map_free(&el->seen_errors);
    ty_ir_cache_free(&el->tycache);
    vec_free(el->hw_calls);
    vec_free(el->undo);
    free(el);
}

void elab_set_step_limit(Elab *el, uint64_t limit)
{
    el->step_limit = limit ? limit : 10000000ull;
}

/* ======================================================= basic values */

Value el_mk_bool(Elab *el, int b)
{
    return val_bool(el->S, b);
}

IrType *el_irty(Elab *el, Ty *t)
{
    if (!el->design)
        return NULL;
    return ty_to_ir(el->design, &el->tycache, t);
}

Value el_hw(Elab *el, Ty *t, IrExpr *e)
{
    if (e && !e->type && el->design)
        e->type = el_irty(el, t);
    if (e && t && t->kind == TY_SINT)
        e->is_signed = e->kind == IRE_RESIZE ? e->is_signed : 1;
    return val_hw(t, e);
}

int el_fits(const BigInt *b, Ty *t)
{
    switch (t->kind) {
    case TY_UINT: case TY_BIT: return bn_fits_width(b, t->width, 0);
    case TY_SINT: return bn_fits_width(b, t->width, 1);
    case TY_INT: return t->is_signed || !bn_is_neg(b);
    case TY_BOOL: return bn_cmp_i64(b, 0) >= 0 && bn_cmp_i64(b, 1) <= 0;
    default: return 0;
    }
}

void el_range_text(Ty *t, StrBuf *out)
{
    BigInt lo = {0}, hi = {0};
    if (t->kind == TY_SINT) {
        bn_set_i64(&lo, -1);
        bn_shl(&lo, &lo, t->width - 1);
        bn_set_i64(&hi, 1);
        bn_shl(&hi, &hi, t->width - 1);
        bn_add_i64(&hi, &hi, -1);
    } else {
        bn_set_i64(&hi, 1);
        bn_shl(&hi, &hi, t->width);
        bn_add_i64(&hi, &hi, -1);
    }
    bn_to_sb(out, &lo, 10);
    sb_puts(out, "..=");
    bn_to_sb(out, &hi, 10);
    bn_free(&lo);
    bn_free(&hi);
}

/* wraps b into the range of a sized type */
static void ce_wrap(BigInt *b, Ty *t)
{
    uint32_t w = t->width;
    if (!ty_is_sized_int(t) && t->kind != TY_BOOL && t->kind != TY_ENUM)
        return;
    if (w == 0)
        return;
    {
        uint64_t *limbs = (uint64_t *)xcalloc(IR_LIMBS(w), sizeof(uint64_t));
        bn_to_limbs(b, limbs, w);
        bn_from_limbs(b, limbs, w, t->kind == TY_SINT);
        free(limbs);
    }
}

static Value ce_int_norm(Elab *el, Ty *t, BigInt *b)
{
    Value v;
    if (t->kind != TY_INT)
        ce_wrap(b, t);
    v = val_int_bn(&el->arena, t, b);
    return v;
}

static Value ce_int_small(Elab *el, Ty *t, int64_t x)
{
    if (t->kind == TY_INT || (t->width < 62 && t->kind != TY_SINT && x >= 0 &&
                              (uint64_t)x < ((uint64_t)1 << t->width)))
        return val_int(t, x);
    {
        BigInt b = {0};
        Value v;
        bn_set_i64(&b, x);
        v = ce_int_norm(el, t, &b);
        bn_free(&b);
        return v;
    }
}

int el_bits_of_const(Elab *el, const Value *v, Ty *t, BigInt *out)
{
    uint32_t i;
    bn_set_i64(out, 0);
    if (!t)
        return 0;
    switch (v->kind) {
    case VAL_INT:
    case VAL_BOOL:
        val_get_bn(v, out);
        if (t->width)
            ce_wrap(out, t->kind == TY_SINT ? ty_uint(el->S, t->width) : t);
        if (bn_is_neg(out) && t->width) {
            Ty *u = ty_uint(el->S, t->width);
            ce_wrap(out, u);
        }
        return 1;
    case VAL_ENUM: {
        TyVariant *var;
        if (t->kind != TY_ENUM || v->aux >= t->nvariants)
            return 0;
        var = &t->variants[v->aux];
        bn_set_u64(out, var->tag);
        bn_shl(out, out, t->payload_width);
        if (v->u.list && var->payload) {
            BigInt p = {0};
            Value sv = *v;
            sv.kind = VAL_STRUCT;
            sv.ty = var->payload;
            if (!el_bits_of_const(el, &sv, var->payload, &p)) {
                bn_free(&p);
                return 0;
            }
            bn_or(out, out, &p);
            bn_free(&p);
        }
        return 1;
    }
    case VAL_STRUCT:
        if (t->kind != TY_STRUCT || v->u.list->n != t->nfields)
            return 0;
        for (i = 0; i < t->nfields; i++) {
            BigInt f = {0};
            if (!el_bits_of_const(el, &v->u.list->v[i], t->fields[i].ty, &f)) {
                bn_free(&f);
                return 0;
            }
            bn_shl(out, out, t->fields[i].ty->width);
            bn_or(out, out, &f);
            bn_free(&f);
        }
        return 1;
    case VAL_ARRAY:
        if (t->kind != TY_ARRAY || v->u.list->n != t->len)
            return 0;
        for (i = t->len; i-- > 0;) {
            BigInt f = {0};
            if (!el_bits_of_const(el, &v->u.list->v[i], t->elem, &f)) {
                bn_free(&f);
                return 0;
            }
            bn_shl(out, out, t->elem->width);
            bn_or(out, out, &f);
            bn_free(&f);
        }
        return 1;
    default:
        return 0;
    }
}

IrBits elab_const_bits(Elab *el, const Value *v, Ty *t)
{
    BigInt b = {0};
    IrBits r;
    Arena *a = el->design ? el->design->arena : &el->arena;
    uint32_t w = t && t->width ? t->width : 1;
    el_bits_of_const(el, v, t, &b);
    r.width = w;
    r.w = ANEWN(a, uint64_t, IR_LIMBS(w));
    bn_to_limbs(&b, r.w, w);
    bn_free(&b);
    return r;
}

static Value ce_from_bn_bits(Elab *el, const BigInt *b, uint32_t lo, Ty *t)
{
    BigInt x = {0};
    Value v;
    uint32_t i;
    bn_shr(&x, b, lo);
    switch (t->kind) {
    case TY_BOOL:
        v = el_mk_bool(el, bn_get_bit(&x, 0));
        break;
    case TY_BIT: case TY_UINT: case TY_SINT: case TY_CLOCK: case TY_RESET: {
        Ty *u = ty_uint(el->S, t->width);
        ce_wrap(&x, u);
        if (t->kind == TY_SINT)
            ce_wrap(&x, t);
        v = val_int_bn(&el->arena, t->kind == TY_CLOCK || t->kind == TY_RESET ? u : t, &x);
        break;
    }
    case TY_STRUCT:
        v = val_list(&el->arena, VAL_STRUCT, t, t->nfields);
        for (i = 0; i < t->nfields; i++)
            v.u.list->v[i] = ce_from_bn_bits(el, b, lo + t->fields[i].lsb, t->fields[i].ty);
        break;
    case TY_ARRAY:
        v = val_list(&el->arena, VAL_ARRAY, t, t->len);
        for (i = 0; i < t->len; i++)
            v.u.list->v[i] = ce_from_bn_bits(el, b, lo + i * t->elem->width, t->elem);
        break;
    case TY_ENUM: {
        BigInt tag = {0};
        uint64_t tg = 0;
        uint32_t k, found = 0;
        bn_shr(&tag, &x, t->payload_width);
        ce_wrap(&tag, ty_uint(el->S, t->tag_width));
        bn_get_u64(&tag, &tg);
        bn_free(&tag);
        for (k = 0; k < t->nvariants; k++)
            if (t->variants[k].tag == tg) {
                found = k;
                break;
            }
        v = val_enum(&el->arena, t, found, NULL);
        if (t->variants[found].payload) {
            Value pv = ce_from_bn_bits(el, b, lo, t->variants[found].payload);
            v.u.list = pv.u.list;
        }
        break;
    }
    default:
        v = val_error();
        break;
    }
    bn_free(&x);
    return v;
}

Value elab_value_from_bits(Elab *el, const IrBits *bits, Ty *t)
{
    BigInt b = {0};
    Value v;
    bn_from_limbs(&b, bits->w, bits->width, 0);
    v = ce_from_bn_bits(el, &b, 0, t);
    bn_free(&b);
    return v;
}

IrExpr *el_to_ir(Elab *el, const Value *v, Span at)
{
    Arena *a = el->design->arena;
    IrExpr *e;
    if (v->kind == VAL_HW)
        return v->u.e;
    if (!v->ty || !v->ty->hw) {
        el_error(el, at, "a %s is not a hardware value", val_kind_name((ValKind)v->kind));
        return ir_const_u64(a, 1, 0, at);
    }
    e = ir_const(a, elab_const_bits(el, v, v->ty), at);
    e->type = el_irty(el, v->ty);
    e->is_signed = v->ty->kind == TY_SINT;
    return e;
}

Value el_zero(Elab *el, Ty *t)
{
    uint32_t i;
    Value v;
    switch (t->kind) {
    case TY_BOOL: return el_mk_bool(el, 0);
    case TY_BIT: case TY_UINT: case TY_SINT: case TY_INT: return val_int(t, 0);
    case TY_ARRAY:
        v = val_list(&el->arena, VAL_ARRAY, t, t->len);
        for (i = 0; i < t->len; i++)
            v.u.list->v[i] = el_zero(el, t->elem);
        return v;
    case TY_STRUCT:
        v = val_list(&el->arena, VAL_STRUCT, t, t->nfields);
        for (i = 0; i < t->nfields; i++)
            v.u.list->v[i] = el_zero(el, t->fields[i].ty);
        return v;
    case TY_TUPLE:
        v = val_list(&el->arena, VAL_TUPLE, t, t->nfields);
        for (i = 0; i < t->nfields; i++)
            v.u.list->v[i] = el_zero(el, t->fields[i].ty);
        return v;
    case TY_ENUM: {
        IrBits b;
        b.width = t->width;
        b.w = ANEWN(&el->arena, uint64_t, IR_LIMBS(t->width));
        return elab_value_from_bits(el, &b, t);
    }
    case TY_STRING: return val_string(el->S, "", 0);
    default: return val_void(el->S);
    }
}

Value el_fill(Elab *el, Ty *t, int ones, Span at)
{
    IrBits b;
    if (!t || t->kind == TY_ERROR)
        return val_error();
    if (t->kind == TY_INT || !t->hw) {
        el_error(el, at, "`%s` needs a type from its context (a sized integer, array or struct)",
                 ones ? "all1" : "all0");
        return val_error();
    }
    if (t->kind == TY_BOOL)
        return el_mk_bool(el, ones);
    b = ones ? ir_bits_ones(&el->arena, t->width) : ir_bits_zero(&el->arena, t->width);
    return elab_value_from_bits(el, &b, t);
}

/* ================================================================ types */

static Ty *ce_builtin_type(Elab *el, const char *name, PathSeg *seg, Env *env, Span at, int *found);

static uint32_t ce_width_arg(Elab *el, Expr *e, Env *env)
{
    Value v = elab_eval_const(el, e, env, el->S->t_int, "a width");
    uint64_t w;
    if (v.kind == VAL_ERROR)
        return 0;
    if (!val_get_u64(&v, &w) || w == 0 || w > (1u << 24)) {
        StrBuf sb = {0};
        val_print(&v, &sb);
        el_error(el, e->span, "invalid width %s (must be 1 ..= 16777216)", sb_cstr(&sb));
        sb_free(&sb);
        return 0;
    }
    return (uint32_t)w;
}

static Ty *ce_builtin_type(Elab *el, const char *name, PathSeg *seg, Env *env, Span at, int *found)
{
    Sema *S = el->S;
    *found = 1;
    if (strcmp(name, "bool") == 0)
        return S->t_bool;
    if (strcmp(name, "string") == 0)
        return S->t_string;
    if (strcmp(name, "uint") == 0 || strcmp(name, "sint") == 0 || strcmp(name, "bit") == 0) {
        uint32_t w;
        if (!seg || !seg->has_args) {
            if (name[0] == 'b') {
                el_error(el, at, "`bit` needs a width: `bit<N>`");
                return S->t_error;
            }
            return ty_int(S, name[0] == 's');
        }
        if (seg->nargs != 1) {
            el_error(el, seg->span, "`%s` takes one width argument", name);
            return S->t_error;
        }
        w = ce_width_arg(el, seg->args[0].expr, env);
        if (!w)
            return S->t_error;
        return ty_scalar(S, name[0] == 'u' ? TY_UINT : name[0] == 's' ? TY_SINT : TY_BIT, w);
    }
    if (strcmp(name, "clock") == 0) {
        int falling = 0;
        uint32_t i;
        for (i = 0; seg && i < seg->nargs; i++) {
            Expr *a = seg->args[i].expr;
            if (a->kind == EXPR_PATH && a->u.path.nsegs == 1 && strcmp(a->u.path.segs[0].name.name, "falling") == 0)
                falling = 1;
            else if (a->kind == EXPR_PATH && a->u.path.nsegs == 1 && strcmp(a->u.path.segs[0].name.name, "rising") == 0)
                falling = 0;
            else
                el_error(el, a->span, "unknown clock option `%s` (expected `falling`)", el_text(a));
        }
        return ty_clock(S, falling);
    }
    if (strcmp(name, "reset") == 0) {
        int async = 0, low = 0;
        uint32_t i;
        for (i = 0; seg && i < seg->nargs; i++) {
            Expr *a = seg->args[i].expr;
            const char *w = a->kind == EXPR_PATH && a->u.path.nsegs == 1 ? a->u.path.segs[0].name.name : "";
            if (strcmp(w, "async") == 0)
                async = 1;
            else if (strcmp(w, "sync") == 0)
                async = 0;
            else if (strcmp(w, "low") == 0)
                low = 1;
            else if (strcmp(w, "high") == 0)
                low = 0;
            else
                el_error(el, a->span, "unknown reset option `%s` (expected sync/async, high/low)", el_text(a));
        }
        return ty_reset(S, async, low);
    }
    if (strcmp(name, "tri") == 0) {
        Ty *inner;
        if (!seg || seg->nargs != 1 || !seg->args[0].type) {
            el_error(el, at, "`tri` needs one type argument: `tri<bool>`");
            return S->t_error;
        }
        inner = elab_eval_type(el, seg->args[0].type, env);
        if (inner->kind == TY_ERROR)
            return inner;
        return ty_tri(S, inner);
    }
    *found = 0;
    return NULL;
}

static int ce_is_builtin_type_name(const char *n)
{
    return strcmp(n, "bool") == 0 || strcmp(n, "uint") == 0 || strcmp(n, "sint") == 0 ||
           strcmp(n, "bit") == 0 || strcmp(n, "clock") == 0 || strcmp(n, "reset") == 0 ||
           strcmp(n, "tri") == 0 || strcmp(n, "string") == 0;
}

static Value ce_eval_path(Elab *el, Path *p, Env *env, Ty *expect, int type_ctx);

Ty *elab_eval_type(Elab *el, Type *t, Env *env)
{
    Sema *S = el->S;
    if (!t)
        return S->t_error;
    switch (t->kind) {
    case TYPE_ERROR:
        return S->t_error;
    case TYPE_PATH: {
        Value v = ce_eval_path(el, &t->u.path, env, NULL, 1);
        if (v.kind == VAL_TYPE)
            return v.u.type;
        if (v.kind == VAL_ERROR)
            return S->t_error;
        if (v.kind == VAL_ITEM && v.u.fn && v.u.fn->item) {
            Item *it = v.u.fn->item;
            if (it->kind == ITEM_STRUCT || it->kind == ITEM_ENUM || it->kind == ITEM_BUNDLE ||
                it->kind == ITEM_TYPE) {
                Ty *r = el_type_item(el, it, v.u.fn->file, v.u.fn->ctx, NULL, 0, env, t->span, NULL);
                return r;
            }
        }
        el_error(el, t->span, "`%s` is not a type", el_span_text(t->span));
        return S->t_error;
    }
    case TYPE_ARRAY: {
        Ty *e = elab_eval_type(el, t->u.array.elem, env);
        Value n = elab_eval_const(el, t->u.array.len, env, S->t_int, "an array length");
        uint64_t len;
        if (e->kind == TY_ERROR || n.kind == VAL_ERROR)
            return S->t_error;
        if (!val_get_u64(&n, &len) || len > (1u << 24)) {
            el_error(el, t->u.array.len->span, "invalid array length");
            return S->t_error;
        }
        if (len == 0) {
            el_error(el, t->u.array.len->span, "an array needs at least one element");
            return S->t_error;
        }
        return ty_array(S, e, (uint32_t)len);
    }
    case TYPE_STRUCT: {
        uint32_t i, n = t->u.anon_struct.nfields;
        TyField *fs = (TyField *)xcalloc(n ? n : 1, sizeof(TyField));
        StrBuf key = {0}, name = {0};
        Ty *r;
        sb_puts(&key, "struct{");
        sb_puts(&name, "struct { ");
        for (i = 0; i < n; i++) {
            Field *f = t->u.anon_struct.fields[i];
            fs[i].name = f->name.name;
            fs[i].ty = elab_eval_type(el, f->type, env);
            fs[i].loc = f->span;
            if (fs[i].ty->kind == TY_ERROR) {
                free(fs);
                sb_free(&key);
                sb_free(&name);
                return S->t_error;
            }
            sb_printf(&key, "%s:%s;", f->name.name, fs[i].ty->key);
            sb_printf(&name, "%s%s: %s", i ? ", " : "", f->name.name, fs[i].ty->name);
        }
        sb_puts(&key, "}");
        sb_puts(&name, " }");
        r = ty_struct(S, sb_cstr(&key), sb_cstr(&name), NULL, NULL, n, fs, t->span);
        free(fs);
        sb_free(&key);
        sb_free(&name);
        return r;
    }
    case TYPE_TYPEOF: {
        Value v;
        int q = el->quiet;
        v = elab_eval(el, t->u.typeof_expr, env, NULL);
        el->quiet = q;
        if (v.kind == VAL_ERROR)
            return S->t_error;
        if (!v.ty) {
            el_error(el, t->span, "`typeof` of a %s", val_kind_name((ValKind)v.kind));
            return S->t_error;
        }
        return v.ty;
    }
    case TYPE_TUPLE: {
        uint32_t i, n = t->u.tuple.n;
        Ty **es = (Ty **)xcalloc(n ? n : 1, sizeof(Ty *));
        Ty *r;
        for (i = 0; i < n; i++) {
            es[i] = elab_eval_type(el, t->u.tuple.elems[i], env);
            if (es[i]->kind == TY_ERROR) {
                free(es);
                return S->t_error;
            }
        }
        r = ty_tuple(S, n, es);
        free(es);
        return r;
    }
    }
    return S->t_error;
}

/* ========================================================== semantic info */

void el_record_decl(Elab *el, Span at, const char *name, SemKind k, Ty *t, const char *domain,
                    const Value *val)
{
    SemInfo si;
    if (!el->S->record_info || el->quiet || !at.file)
        return;
    memset(&si, 0, sizeof si);
    si.span = at;
    si.def = at;
    si.kind = k;
    si.name = name;
    si.type = t && t->kind != TY_TYPE ? ty_str(t) : NULL;
    si.domain = domain;
    if (val && val_is_const(val) && val->kind != VAL_TYPE) {
        StrBuf sb = {0};
        val_print(val, &sb);
        if (sb.len < 80)
            si.value = intern_cstr(sb_cstr(&sb));
        sb_free(&sb);
    } else if (val && val->kind == VAL_TYPE) {
        si.value = ty_str(val->u.type);
    }
    sema_info_record(el->S, &si);
}

void el_record_use(Elab *el, Span at, Binding *b, const char *name)
{
    SemInfo si;
    if (!el->S->record_info || el->quiet || !at.file || sema_info_decl(el->S, at))
        return;
    memset(&si, 0, sizeof si);
    si.span = at;
    si.name = name;
    if (b) {
        si.def = b->def;
        si.kind = (SemKind)b->sem;
        if (b->val.ty && b->val.kind != VAL_TYPE && b->val.kind != VAL_ERROR)
            si.type = ty_str(b->val.ty);
        if (b->val.kind == VAL_TYPE)
            si.value = ty_str(b->val.u.type);
        else if (!b->is_mut && val_is_const(&b->val) && b->val.kind != VAL_VOID) {
            StrBuf sb = {0};
            val_print(&b->val, &sb);
            if (sb.len < 80)
                si.value = intern_cstr(sb_cstr(&sb));
            sb_free(&sb);
        }
    }
    sema_info_record(el->S, &si);
}

static void ce_record_item_use(Elab *el, Span at, Item *it, const char *name)
{
    SemInfo si;
    if (!el->S->record_info || el->quiet || !at.file || !it || sema_info_decl(el->S, at))
        return;
    memset(&si, 0, sizeof si);
    si.span = at;
    si.name = name;
    si.def = it->name.span;
    switch (it->kind) {
    case ITEM_MODULE: case ITEM_EXTERN: si.kind = SEM_MODULE; break;
    case ITEM_FN: si.kind = SEM_FN; break;
    case ITEM_CONST: si.kind = SEM_CONST; break;
    default: si.kind = SEM_TYPE; break;
    }
    sema_info_record(el->S, &si);
}

/* ============================================================== lookup */

static void ce_collect_names(Env *env, const char **out, size_t *n, size_t max)
{
    for (; env; env = env->parent) {
        uint32_t i;
        for (i = 0; i < env->n && *n < max; i++)
            out[(*n)++] = env->b[i].name;
    }
}

Value el_force(Elab *el, Binding *b)
{
    Env *denv;
    const char *nm;
    Value v;
    if (!b->lazy || b->state == 2)
        return b->val;
    if (b->state == 1) {
        el_error(el, b->def, "`%s` is defined in terms of itself", b->name);
        b->val = val_error();
        b->state = 2;
        return b->val;
    }
    if (!b->item)
        return el_force_decl(el, b);
    b->state = 1;
    denv = b->decl_env;
    nm = b->name;
    v = el_item_value(el, b->item, denv ? denv->file : NULL, denv, b->item->name.span);
    /* the binding array may have moved while evaluating */
    b = denv ? ce_env_find_local(denv, nm) : b;
    if (b) {
        b->val = v;
        b->state = 2;
    }
    return v;
}

Value el_lookup_name(Elab *el, Env *env, const char *name, Span at, int report)
{
    Binding *b = elab_env_find(env, name);
    Symbol sym;
    if (b) {
        Value v = b->lazy ? el_force(el, b) : b->val;
        b = elab_env_find(env, name);
        if (v.kind == VAL_VOID && v.aux == 1) {
            el_error(el, at, "cannot infer generic parameter `%s`", name);
            el_help(el, "give it explicitly: `f<..>(..)`");
            return val_error();
        }
        el_record_use(el, at, b, name);
        return v;
    }
    if (sema_lookup(el->S, env ? env->file : NULL, name, &sym)) {
        if (sym.kind == SYM_ITEM)
            ce_record_item_use(el, at, sym.item, name);
        return elab_symbol_value(el, &sym, at);
    }
    if (ce_is_builtin_type_name(name)) {
        int found;
        Ty *t = ce_builtin_type(el, name, NULL, env, at, &found);
        if (t)
            return val_type(el->S, t);
    }
    if (report) {
        const char *names[256];
        size_t n = 0;
        el_error(el, at, "cannot find `%s` in this scope", name);
        if (!el->quiet) {
            ce_collect_names(env, names, &n, TERN_ARRAY_LEN(names));
            sema_suggest(el->S, env ? env->file : NULL, name, names, n);
        }
    }
    return val_error();
}

static Value ce_item_ref(Elab *el, ValKind k, Item *it, FileUnit *file, Env *ctx)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = (uint8_t)k;
    v.u.fn = ANEW(&el->arena, ValFn);
    v.u.fn->item = it;
    v.u.fn->file = file;
    v.u.fn->ctx = ctx;
    return v;
}

Value elab_symbol_value(Elab *el, const Symbol *sym, Span at)
{
    Value v;
    if (sym->kind == SYM_PACKAGE) {
        memset(&v, 0, sizeof v);
        v.kind = VAL_PACKAGE;
        v.u.pkg = sym->pkg;
        return v;
    }
    if (sym->kind == SYM_ITEM)
        return el_item_value(el, sym->item, sym->file, NULL, at);
    return val_error();
}

Value el_item_value(Elab *el, Item *it, FileUnit *file, Env *ctx, Span at)
{
    Sema *S = el->S;
    TERN_UNUSED(at);
    if (!file)
        file = sema_file_of_item(S, it);
    switch (it->kind) {
    case ITEM_CONST: {
        uint64_t cv;
        Value *slot;
        Env *env;
        Ty *t = NULL;
        Value v;
        if (!ctx && map_get(&el->modtypes, MAP_PTR(it), &cv)) {
            if (cv == 1) {
                el_error(el, it->name.span, "const `%s` is defined in terms of itself", it->name.name);
                return val_error();
            }
            return *(Value *)(uintptr_t)cv;
        }
        if (!ctx)
            map_put(&el->modtypes, MAP_PTR(it), 1);
        env = elab_env_new(el, ctx, file);
        if (it->u.const_def.type)
            t = elab_eval_type(el, it->u.const_def.type, env);
        v = elab_eval_const(el, it->u.const_def.value, env, t, "a const value");
        if (t && v.kind != VAL_ERROR && t->kind != TY_ERROR)
            v = elab_convert(el, v, t, it->u.const_def.value, NULL, it->u.const_def.value->span);
        elab_env_free(env);
        el_record_decl(el, it->name.span, it->name.name, SEM_CONST, v.ty, NULL, &v);
        if (!ctx) {
            slot = ANEW(&el->arena, Value);
            *slot = v;
            map_put(&el->modtypes, MAP_PTR(it), MAP_PTR(slot));
        }
        return v;
    }
    case ITEM_FN:
        return ce_item_ref(el, VAL_FN, it, file, ctx);
    case ITEM_STRUCT: case ITEM_ENUM: case ITEM_BUNDLE: case ITEM_TYPE: {
        GenericParams *gp = it->kind == ITEM_STRUCT ? &it->u.struct_def.generics
                          : it->kind == ITEM_ENUM ? &it->u.enum_def.generics
                          : it->kind == ITEM_BUNDLE ? &it->u.bundle.generics
                          : &it->u.type_alias.generics;
        if (gp->n == 0) {
            Ty *t = el_type_item(el, it, file, ctx, NULL, 0, ctx, it->name.span, NULL);
            if (t->kind == TY_ERROR)
                return val_error();
            return val_type(S, t);
        }
        return ce_item_ref(el, VAL_ITEM, it, file, ctx);
    }
    default:
        return ce_item_ref(el, VAL_ITEM, it, file, ctx);
    }
}

static Value ce_module_const(Elab *el, ValModule *vm, const char *name, Span at)
{
    Item *it = vm->item;
    Stmt **body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
    uint32_t n = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody, i;
    Env *env;
    Binding *found = NULL;
    Stmt *decl = NULL;
    for (i = 0; i < n; i++)
        if (body[i]->kind == STMT_ITEM && body[i]->u.item->name.name == name)
            decl = body[i];
    if (!decl) {
        el_error(el, at, "module `%s` has no const `%s`", vm->display, name);
        return val_error();
    }
    if (!decl->u.item->is_pub && !(el->mod && el->mod->item == it)) {
        el_error(el, at, "`%s` is private to module `%s`", name, it->name.name);
        el_help(el, "declare it `pub const %s` to read it from outside", name);
        return val_error();
    }
    env = elab_env_new(el, vm->genv, vm->file);
    el_env_keep(el, env); /* lazily evaluated items keep pointing at it */
    for (i = 0; i < n; i++) {
        Stmt *s = body[i];
        if (s->kind == STMT_ITEM && s->u.item->name.name) {
            Binding *b = elab_env_bind(el, env, s->u.item->name.name, val_void(el->S), s->u.item->name.span);
            b->lazy = 1;
            b->item = s->u.item;
            b->decl_env = env;
        }
    }
    found = ce_env_find_local(env, name);
    return found ? el_force(el, found) : val_error();
}

Value el_member(Elab *el, Value base, const char *name, Span at, Env *env)
{
    Sema *S = el->S;
    switch (base.kind) {
    case VAL_ERROR:
        return base;
    case VAL_PACKAGE: {
        Symbol sym;
        int priv = 0;
        if (sema_lookup_member(S, env ? env->file : NULL, base.u.pkg, name, &sym, &priv)) {
            if (sym.kind == SYM_ITEM)
                ce_record_item_use(el, at, sym.item, name);
            return elab_symbol_value(el, &sym, at);
        }
        if (priv)
            el_error(el, at, "`%s` is private to package `%s`", name, base.u.pkg->path);
        else
            el_error(el, at, "cannot find `%s` in package `%s`", name, base.u.pkg->path);
        return val_error();
    }
    case VAL_TYPE: {
        Ty *t = base.u.type;
        if (base.aux) {
            el_error(el, at, "`::%s` on an enum variant", name);
            return val_error();
        }
        if (strcmp(name, "WIDTH") == 0) {
            if (!t->hw) {
                el_error(el, at, "`%s` has no fixed width", ty_str(t));
                return val_error();
            }
            return val_int(S->t_int, t->width);
        }
        if (strcmp(name, "MAX") == 0 || strcmp(name, "MIN") == 0) {
            BigInt b = {0};
            Value v;
            if (!ty_is_sized_int(t)) {
                el_error(el, at, "`%s::%s` needs a sized integer type", ty_str(t), name);
                return val_error();
            }
            if (t->kind == TY_SINT) {
                bn_set_i64(&b, 1);
                bn_shl(&b, &b, t->width - 1);
                if (name[1] == 'A')
                    bn_add_i64(&b, &b, -1);
                else
                    bn_neg(&b, &b);
            } else if (name[1] == 'A') {
                bn_set_i64(&b, 1);
                bn_shl(&b, &b, t->width);
                bn_add_i64(&b, &b, -1);
            }
            v = val_int_bn(&el->arena, t, &b);
            bn_free(&b);
            return v;
        }
        if (strcmp(name, "from_bits") == 0) {
            Value v;
            memset(&v, 0, sizeof v);
            v.kind = VAL_FN;
            v.u.fn = ANEW(&el->arena, ValFn);
            v.u.fn->builtin = 1;
            v.u.fn->bty = t;
            v.u.fn->name = intern_cstr("from_bits");
            return v;
        }
        if (t->kind == TY_ENUM) {
            int k = ty_variant_index(t, name);
            if (k >= 0) {
                if (t->variants[k].payload) {
                    Value v = val_type(S, t);
                    v.aux = (uint32_t)k + 1;
                    return v;
                }
                return val_enum(&el->arena, t, (uint32_t)k, NULL);
            }
            el_error(el, at, "enum `%s` has no variant `%s`", ty_str(t), name);
            return val_error();
        }
        el_error(el, at, "`%s` has no associated item `%s`", ty_str(t), name);
        return val_error();
    }
    case VAL_ITEM: {
        Item *it = base.u.fn->item;
        if (it->kind == ITEM_REGMAP) {
            Ty *t = el_regmap_type(el, it, base.u.fn->file, name, at);
            return t->kind == TY_ERROR ? val_error() : val_type(S, t);
        }
        if (it->kind == ITEM_MODULE || it->kind == ITEM_EXTERN) {
            ValModule *vm = elab_bind_module(el, it, base.u.fn->file, NULL, at);
            Value mv;
            if (!vm)
                return val_error();
            memset(&mv, 0, sizeof mv);
            mv.kind = VAL_MODULE;
            mv.u.mod = vm;
            return el_member(el, mv, name, at, env);
        }
        if (it->kind == ITEM_STRUCT || it->kind == ITEM_ENUM || it->kind == ITEM_BUNDLE || it->kind == ITEM_TYPE) {
            Ty *t = el_type_item(el, it, base.u.fn->file, base.u.fn->ctx, NULL, 0, env, at, NULL);
            if (t->kind == TY_ERROR)
                return val_error();
            return el_member(el, val_type(S, t), name, at, env);
        }
        el_error(el, at, "`%s::%s` is not valid", it->name.name ? it->name.name : "?", name);
        return val_error();
    }
    case VAL_MODULE:
        return ce_module_const(el, base.u.mod, name, at);
    default:
        el_error(el, at, "`::%s` on a %s", name, val_kind_name((ValKind)base.kind));
        return val_error();
    }
}

Value el_apply_generics(Elab *el, Value base, PathSeg *seg, Env *env, Ty *expect)
{
    if (base.kind == VAL_ERROR)
        return base;
    if (base.kind == VAL_ITEM) {
        Item *it = base.u.fn->item;
        switch (it->kind) {
        case ITEM_STRUCT: case ITEM_ENUM: case ITEM_BUNDLE: case ITEM_TYPE: {
            Ty *t = el_type_item(el, it, base.u.fn->file, base.u.fn->ctx, seg->args, seg->nargs, env,
                                 seg->span, expect);
            return t->kind == TY_ERROR ? val_error() : val_type(el->S, t);
        }
        case ITEM_MODULE: case ITEM_EXTERN: {
            ValModule *vm = el_bind_module_ast(el, it, base.u.fn->file, seg->args, seg->nargs, env, seg->span);
            Value mv;
            if (!vm)
                return val_error();
            memset(&mv, 0, sizeof mv);
            mv.kind = VAL_MODULE;
            mv.u.mod = vm;
            return mv;
        }
        default:
            break;
        }
    }
    if (base.kind == VAL_FN && base.u.fn->item) {
        Item *it = base.u.fn->item;
        GenericParams *gp = &it->u.fn.generics;
        uint32_t i, k = 0;
        Value r = base;
        r.u.fn = ANEW(&el->arena, ValFn);
        *r.u.fn = *base.u.fn;
        r.u.fn->ngargs = seg->nargs;
        r.u.fn->gargs = ANEWN(&el->arena, Value, seg->nargs ? seg->nargs : 1);
        r.u.fn->gnames = ANEWN(&el->arena, Ident, seg->nargs ? seg->nargs : 1);
        for (i = 0; i < seg->nargs; i++) {
            GenericArg *ga = &seg->args[i];
            GenericParam *p = NULL;
            uint32_t j;
            if (ga->name.name) {
                for (j = 0; j < gp->n; j++)
                    if (gp->items[j].name.name == ga->name.name)
                        p = &gp->items[j];
            } else if (k < gp->n) {
                p = &gp->items[k++];
            }
            if (!p) {
                el_error(el, ga->span, "too many generic arguments for `%s`", it->name.name);
                return val_error();
            }
            r.u.fn->gnames[i] = p->name;
            if (p->kind == GENERIC_TYPE) {
                Ty *t = ga->type ? elab_eval_type(el, ga->type, env) : NULL;
                if (!t) {
                    el_error(el, ga->span, "generic `%s` expects a type", p->name.name);
                    return val_error();
                }
                r.u.fn->gargs[i] = val_type(el->S, t);
            } else {
                Ty *pt = p->type ? elab_eval_type(el, p->type, env) : NULL;
                r.u.fn->gargs[i] = elab_eval_const(el, ga->expr, env, pt && pt->kind != TY_ERROR ? pt : NULL,
                                                   "a generic argument");
            }
        }
        return r;
    }
    if (base.kind == VAL_FN && base.u.fn->builtin)
        return base;
    if (base.kind == VAL_TYPE) {
        el_error(el, seg->span, "`%s` takes no generic arguments", ty_str(base.u.type));
        return val_error();
    }
    el_error(el, seg->span, "generic arguments on a %s", val_kind_name((ValKind)base.kind));
    return val_error();
}

static Value ce_eval_path(Elab *el, Path *p, Env *env, Ty *expect, int type_ctx)
{
    PathSeg *s0 = &p->segs[0];
    const char *name = s0->name.name;
    Value base;
    uint32_t i;
    if (!name)
        return val_error();
    if ((type_ctx || s0->has_args || p->nsegs > 1) && ce_is_builtin_type_name(name) &&
        (s0->has_args || !elab_env_find(env, name))) {
        int found;
        Ty *t = ce_builtin_type(el, name, s0, env, s0->span, &found);
        base = t ? (t->kind == TY_ERROR ? val_error() : val_type(el->S, t)) : val_error();
    } else if (strcmp(name, "cfg") == 0 && p->nsegs == 2 && !elab_env_find(env, name)) {
        int on = 0;
        const char *f = p->segs[1].name.name;
        if (!f)
            return val_error();
        if (!sema_feature(el->S, f, &on)) {
            el_error(el, p->segs[1].name.span, "unknown feature `cfg::%s`", f);
            el_help(el, "%s", el->S->has_manifest ? "declare it under [features] in tern.toml"
                                                  : "features come from [features] in tern.toml");
            return val_error();
        }
        return el_mk_bool(el, on);
    } else {
        base = el_lookup_name(el, env, name, s0->name.span, 1);
        if (s0->has_args)
            base = el_apply_generics(el, base, s0, env, p->nsegs == 1 ? expect : NULL);
    }
    for (i = 1; i < p->nsegs && base.kind != VAL_ERROR; i++) {
        PathSeg *s = &p->segs[i];
        if (!s->name.name)
            return val_error();
        base = el_member(el, base, s->name.name, s->name.span, env);
        if (s->has_args)
            base = el_apply_generics(el, base, s, env, i + 1 == p->nsegs ? expect : NULL);
    }
    return base;
}

/* ========================================================== conversions */

/* "<prefix><name>`", interned (no allocation growth in loops) */
static const char *ce_what(const char *prefix, const char *name)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s%s`", prefix, name ? name : "?");
    return intern_cstr(buf);
}

static int ce_int_family(Ty *t)
{
    /* 1 unsigned (uint, bit), 2 signed, 0 other */
    if (!t)
        return 0;
    if (t->kind == TY_UINT || t->kind == TY_BIT)
        return 1;
    if (t->kind == TY_SINT)
        return 2;
    if (t->kind == TY_INT)
        return t->is_signed ? 2 : 1;
    return 0;
}

static void ce_width_help(Elab *el, Expr *src, Ty *to)
{
    if (src && src->kind == EXPR_PAREN)
        src = src->u.inner;
    if (src && src->kind == EXPR_BINARY && (src->u.binary.op == BIN_ADD || src->u.binary.op == BIN_SUB))
        el_help(el, "use `%s` for a wrapping %s, or truncate with `as %s`",
                src->u.binary.op == BIN_ADD ? "+%" : "-%", src->u.binary.op == BIN_ADD ? "add" : "subtract",
                ty_str(to));
    else
        el_help(el, "truncate explicitly with `as %s`", ty_str(to));
}

static void ce_mismatch(Elab *el, Value *v, Ty *to, Expr *src, const char *what, Span at)
{
    Ty *from = v->ty;
    const char *s = src ? el_text(src) : NULL;
    if (ce_int_family(from) && ce_int_family(to) && from->kind != TY_INT && to->kind != TY_INT &&
        ce_int_family(from) == ce_int_family(to) && from->width > to->width) {
        if (s && what)
            el_error(el, at, "width mismatch: `%s` is %s, %s is %s", s, ty_str(from), what, ty_str(to));
        else if (s)
            el_error(el, at, "width mismatch: `%s` is %s, expected %s", s, ty_str(from), ty_str(to));
        else
            el_error(el, at, "width mismatch: found %s, expected %s", ty_str(from), ty_str(to));
        ce_width_help(el, src, to);
        return;
    }
    if (ce_int_family(from) && ce_int_family(to) && ce_int_family(from) != ce_int_family(to)) {
        el_error(el, at, "signedness mismatch: %s%s%s is %s, %s is %s", s ? "`" : "", s ? s : "value",
                 s ? "`" : "", ty_str(from), what ? what : "the target", ty_str(to));
        el_help(el, "cast explicitly with `as %s`, or use std::math::widen_signed", ty_str(to));
        return;
    }
    if (from && from->kind == TY_BOOL && ce_int_family(to)) {
        el_error(el, at, "type mismatch: %s%s%s is bool, %s is %s; bool is not an integer", s ? "`" : "",
                 s ? s : "value", s ? "`" : "", what ? what : "the target", ty_str(to));
        el_help(el, "convert with `%s as uint<1>`", s ? s : "x");
        return;
    }
    el_error(el, at, "type mismatch: %s%s%s is %s, %s is %s", s ? "`" : "", s ? s : "value", s ? "`" : "",
             from ? ty_str(from) : val_kind_name((ValKind)v->kind), what ? what : "expected", ty_str(to));
}

static Value ce_materialize_array(Elab *el, Value v, Ty *to, Span at)
{
    /* a VAL_ARRAY of hardware-typed elements, some not constant -> VAL_HW */
    uint32_t i, n = v.u.list->n;
    IrExpr **parts;
    IrExpr *e;
    if (val_is_const(&v))
        return v;
    if (!el->design) {
        el_error(el, at, "a hardware value outside a module");
        return val_error();
    }
    parts = (IrExpr **)xcalloc(n ? n : 1, sizeof(IrExpr *));
    for (i = 0; i < n; i++)
        parts[n - 1 - i] = el_to_ir(el, &v.u.list->v[i], at);
    e = ir_concat(el->design->arena, n, parts, at);
    free(parts);
    return el_hw(el, to, e);
}

Value elab_convert(Elab *el, Value v, Ty *to, Expr *src, const char *what, Span at)
{
    Sema *S = el->S;
    Ty *from = v.ty;
    uint32_t i;
    if (v.kind == VAL_ERROR || !to || to->kind == TY_ERROR)
        return v.kind == VAL_ERROR ? v : val_error();
    if (from == to)
        return v;
    switch (to->kind) {
    case TY_BIT: case TY_UINT: case TY_SINT:
        if (v.kind == VAL_INT && from && from->kind == TY_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&v, &b);
            if (!el_fits(&b, to)) {
                StrBuf sb = {0}, rs = {0};
                bn_to_sb(&sb, &b, 10);
                el_range_text(to, &rs);
                el_error(el, at, "%s %s does not fit in %s (%s)",
                         src && (src->kind == EXPR_INT || (src->kind == EXPR_UNARY && src->u.unary.operand->kind == EXPR_INT))
                             ? "literal" : "value",
                         sb_cstr(&sb), ty_str(to), sb_cstr(&rs));
                sb_free(&sb);
                sb_free(&rs);
                bn_free(&b);
                return val_error();
            }
            r = val_int_bn(&el->arena, to, &b);
            bn_free(&b);
            return r;
        }
        if ((v.kind == VAL_INT || v.kind == VAL_HW) && from && ty_is_sized_int(from)) {
            int ff = ce_int_family(from), tf = ce_int_family(to);
            if (ff == tf && to->width >= from->width) {
                if (v.kind == VAL_INT) {
                    v.ty = to;
                    return v;
                }
                if (to->width == from->width)
                    return el_hw(el, to, v.u.e);
                return el_hw(el, to, ir_resize(el->design->arena, v.u.e, to->width, ff == 2, at));
            }
        }
        if (v.kind == VAL_HW && from && from->kind == TY_TRI && from->elem == to)
            return el_hw(el, to, v.u.e);
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_INT:
        if (v.kind == VAL_INT) {
            if (!to->is_signed && ((v.flags & VAL_F_BIG) ? bn_is_neg(v.u.big) : v.u.i < 0)) {
                el_error(el, at, "negative value where a widthless `uint` is expected");
                return val_error();
            }
            v.ty = to;
            return v;
        }
        if (v.kind == VAL_HW) {
            el_error(el, at, "`%s` is a signal; a compile-time integer is needed here", src ? el_text(src) : "value");
            return val_error();
        }
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_BOOL:
        if (v.kind == VAL_BOOL || (v.kind == VAL_HW && from && from->kind == TY_BOOL))
            return v;
        if (v.kind == VAL_HW && from && from->kind == TY_TRI && from->elem == to)
            return el_hw(el, to, v.u.e);
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_ARRAY:
        if (v.kind == VAL_ARRAY) {
            Value r;
            if (v.u.list->n != to->len) {
                el_error(el, at, "array length mismatch: %u elements, %s has %u", (unsigned)v.u.list->n,
                         ty_str(to), (unsigned)to->len);
                return val_error();
            }
            r = val_list(&el->arena, VAL_ARRAY, to, to->len);
            for (i = 0; i < to->len; i++) {
                r.u.list->v[i] = elab_convert(el, v.u.list->v[i], to->elem, NULL, what, at);
                if (r.u.list->v[i].kind == VAL_ERROR)
                    return val_error();
            }
            if (to->hw)
                return ce_materialize_array(el, r, to, at);
            return r;
        }
        if (v.kind == VAL_HW && from && from->kind == TY_ARRAY && from->len == to->len &&
            from->elem->width == to->elem->width && from->hw && to->hw) {
            /* element-wise widening of identical width (bit<N> <-> uint<N>) */
            if (ce_int_family(from->elem) && ce_int_family(from->elem) == ce_int_family(to->elem))
                return el_hw(el, to, v.u.e);
        }
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_TUPLE:
        if (v.kind == VAL_TUPLE && v.u.list->n == to->nfields) {
            Value r = val_list(&el->arena, VAL_TUPLE, to, to->nfields);
            for (i = 0; i < to->nfields; i++) {
                r.u.list->v[i] = elab_convert(el, v.u.list->v[i], to->fields[i].ty, NULL, what, at);
                if (r.u.list->v[i].kind == VAL_ERROR)
                    return val_error();
            }
            return r;
        }
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_TYPE:
        if (v.kind == VAL_TYPE)
            return v;
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_STRING:
        if (v.kind == VAL_STRING)
            return v;
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    case TY_TRI:
        if (from == to->elem && v.kind == VAL_HW)
            return el_hw(el, to, v.u.e);
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    default:
        TERN_UNUSED(S);
        ce_mismatch(el, &v, to, src, what, at);
        return val_error();
    }
}

Ty *el_common_type(Elab *el, Value *a, Value *b, Span at)
{
    Ty *ta = a->ty, *tb = b->ty;
    if (a->kind == VAL_ERROR || b->kind == VAL_ERROR)
        return el->S->t_error;
    if (ta == tb)
        return ta;
    if (ta && ta->kind == TY_INT && a->kind == VAL_INT && tb && tb->kind != TY_INT) {
        *a = elab_convert(el, *a, tb, NULL, NULL, at);
        return tb;
    }
    if (tb && tb->kind == TY_INT && b->kind == VAL_INT && ta && ta->kind != TY_INT) {
        *b = elab_convert(el, *b, ta, NULL, NULL, at);
        return ta;
    }
    if (ce_int_family(ta) && ce_int_family(ta) == ce_int_family(tb) && ta->kind != TY_INT && tb->kind != TY_INT) {
        uint32_t w = TERN_MAX(ta->width, tb->width);
        Ty *t = ta->kind == TY_SINT ? ty_sint(el->S, w)
              : (ta->kind == TY_UINT || tb->kind == TY_UINT) ? ty_uint(el->S, w) : ty_bits(el->S, w);
        *a = elab_convert(el, *a, t, NULL, NULL, at);
        *b = elab_convert(el, *b, t, NULL, NULL, at);
        return t;
    }
    if (ta && tb && ta->kind == TY_INT && tb->kind == TY_INT)
        return (ta->is_signed || tb->is_signed) ? el->S->t_sint : el->S->t_int;
    el_error(el, at, "branches have different types: %s and %s", ta ? ty_str(ta) : val_kind_name((ValKind)a->kind),
             tb ? ty_str(tb) : val_kind_name((ValKind)b->kind));
    return el->S->t_error;
}

static Ty *ce_min_type(Elab *el, const Value *c, int signed_ctx);

Value el_unify_mux(Elab *el, Value c, Value t, Value f, Ty *expect, Span at)
{
    Ty *ty;
    if (c.kind == VAL_ERROR || t.kind == VAL_ERROR || f.kind == VAL_ERROR)
        return val_error();
    if (c.kind == VAL_BOOL)
        return c.u.i ? t : f;
    if (expect && expect->kind != TY_ERROR && expect->hw) {
        t = elab_convert(el, t, expect, NULL, NULL, at);
        f = elab_convert(el, f, expect, NULL, NULL, at);
        if (t.kind == VAL_ERROR || f.kind == VAL_ERROR)
            return val_error();
    }
    ty = el_common_type(el, &t, &f, at);
    if (ty->kind == TY_ERROR || t.kind == VAL_ERROR || f.kind == VAL_ERROR)
        return val_error();
    if (ty->kind == TY_INT && t.kind == VAL_INT && f.kind == VAL_INT) {
        /* two untyped constants selected at run time: smallest common type */
        Ty *a = ce_min_type(el, &t, 0), *b = ce_min_type(el, &f, 0);
        int sg = a->kind == TY_SINT || b->kind == TY_SINT;
        if (sg) {
            a = ce_min_type(el, &t, 1);
            b = ce_min_type(el, &f, 1);
        }
        ty = ty_scalar(el->S, sg ? TY_SINT : TY_UINT, TERN_MAX(a->width, b->width));
        t = elab_convert(el, t, ty, NULL, NULL, at);
        f = elab_convert(el, f, ty, NULL, NULL, at);
    }
    if (t.kind == VAL_BUNDLE || f.kind == VAL_BUNDLE || !ty->hw) {
        el_error(el, at, "cannot select between %s values at run time", ty_str(ty));
        return val_error();
    }
    if (val_is_const(&t) && val_is_const(&f) && val_equal(&t, &f))
        return t;
    return el_hw(el, ty, ir_mux(el->design->arena, el_to_ir(el, &c, at), el_to_ir(el, &t, at),
                                el_to_ir(el, &f, at), at));
}

/* ============================================================ operators */

/* v (int-like or bool, const or hw) as an IR expression of width w,
 * zero- or sign-extended according to its own type */
static IrExpr *ce_ir_ext(Elab *el, const Value *v, uint32_t w, Span at)
{
    Arena *a = el->design->arena;
    if (v->kind == VAL_HW) {
        IrExpr *e = v->u.e;
        if (e->width == w)
            return e;
        return ir_resize(a, e, w, v->ty && v->ty->kind == TY_SINT, at);
    }
    {
        BigInt b = {0};
        IrBits bits;
        IrExpr *e;
        val_get_bn(v, &b);
        bits.width = w;
        bits.w = ANEWN(a, uint64_t, IR_LIMBS(w));
        bn_to_limbs(&b, bits.w, w);
        bn_free(&b);
        e = ir_const(a, bits, at);
        e->is_signed = v->ty && (v->ty->kind == TY_SINT || (v->ty->kind == TY_INT && v->ty->is_signed));
        return e;
    }
}

/* minimal sized type of a widthless constant, given the other operand's family */
static Ty *ce_min_type(Elab *el, const Value *c, int signed_ctx)
{
    BigInt b = {0};
    uint32_t w;
    Ty *t;
    val_get_bn(c, &b);
    if (signed_ctx || bn_is_neg(&b)) {
        w = (uint32_t)bn_signed_width(&b);
        if (w == 0)
            w = 1;
        t = ty_sint(el->S, w);
    } else {
        w = (uint32_t)bn_bitlen(&b);
        if (w == 0)
            w = 1;
        t = ty_uint(el->S, w);
    }
    bn_free(&b);
    return t;
}

static int ce_is_widthless(const Value *v)
{
    return v->kind == VAL_INT && v->ty && v->ty->kind == TY_INT;
}

static const char *ce_op_text(BinaryOp op)
{
    return ast_binary_op_text(op);
}

/* Checks two integer operands of an arithmetic/bitwise/comparison operator
 * and gives both a sized type (widthless constants get their minimal type).
 * Returns 0 after reporting an error, 2 when both are widthless constants. */
static int ce_int_operands(Elab *el, BinaryOp op, Value *a, Value *b, Span at, Expr *ea, Expr *eb)
{
    Ty *ta = a->ty, *tb = b->ty;
    int fa = ce_int_family(ta), fb = ce_int_family(tb);
    if ((ta && ta->kind == TY_BOOL) || (tb && tb->kind == TY_BOOL)) {
        Expr *be = ta && ta->kind == TY_BOOL ? ea : eb;
        el_error(el, at, "bool is not an integer: `%s` on bool", ce_op_text(op));
        el_help(el, "convert with `%s as uint<1>`, or count set bools with `count_ones(...)`",
                be ? el_text(be) : "x");
        return 0;
    }
    if (!fa || !fb) {
        Ty *bad = !fa ? ta : tb;
        el_error(el, at, "`%s` needs integer operands, found %s", ce_op_text(op),
                 bad ? ty_str(bad) : val_kind_name((ValKind)(!fa ? a->kind : b->kind)));
        return 0;
    }
    if (ce_is_widthless(a) && ce_is_widthless(b))
        return 2;
    if (ce_is_widthless(a))
        a->ty = ce_min_type(el, a, fb == 2), fa = ce_int_family(a->ty);
    if (ce_is_widthless(b))
        b->ty = ce_min_type(el, b, fa == 2), fb = ce_int_family(b->ty);
    if (fa != fb) {
        el_error(el, at, "cannot mix %s and %s in `%s`", ty_str(a->ty), ty_str(b->ty), ce_op_text(op));
        el_help(el, "cast one side, or use std::math::widen_signed");
        return 0;
    }
    return 1;
}

static Value ce_fold_int(Elab *el, BinaryOp op, const Value *a, const Value *b, Ty *rt, Span at)
{
    BigInt x = {0}, y = {0}, r = {0};
    Value v;
    int64_t xa, xb;
    int small = !(a->flags & VAL_F_BIG) && !(b->flags & VAL_F_BIG) && a->kind == VAL_INT && b->kind == VAL_INT;
    xa = a->u.i;
    xb = b->u.i;
    if (small && xa > -((int64_t)1 << 61) && xa < ((int64_t)1 << 61) && xb > -((int64_t)1 << 61) &&
        xb < ((int64_t)1 << 61)) {
        int64_t res = 0;
        int ok = 1;
        switch (op) {
        case BIN_ADD: case BIN_ADD_WRAP: res = xa + xb; break;
        case BIN_SUB: case BIN_SUB_WRAP: res = xa - xb; break;
        case BIN_MUL:
            if (xa > -((int64_t)1 << 30) && xa < ((int64_t)1 << 30) && xb > -((int64_t)1 << 30) &&
                xb < ((int64_t)1 << 30))
                res = xa * xb;
            else
                ok = 0;
            break;
        case BIN_BITAND: res = xa & xb; break;
        case BIN_BITOR: res = xa | xb; break;
        case BIN_BITXOR: res = xa ^ xb; break;
        default: ok = 0; break;
        }
        if (ok)
            return ce_int_small(el, rt, res);
    }
    val_get_bn(a, &x);
    val_get_bn(b, &y);
    switch (op) {
    case BIN_ADD: case BIN_ADD_WRAP: bn_add(&r, &x, &y); break;
    case BIN_SUB: case BIN_SUB_WRAP: bn_sub(&r, &x, &y); break;
    case BIN_MUL: bn_mul(&r, &x, &y); break;
    case BIN_DIV: case BIN_MOD: {
        BigInt q = {0}, m = {0};
        if (!bn_divmod_trunc(&q, &m, &x, &y)) {
            el_error(el, at, "division by zero");
            bn_free(&q);
            bn_free(&m);
            bn_free(&x);
            bn_free(&y);
            return val_error();
        }
        bn_copy(&r, op == BIN_DIV ? &q : &m);
        bn_free(&q);
        bn_free(&m);
        break;
    }
    case BIN_BITAND: bn_and(&r, &x, &y); break;
    case BIN_BITOR: bn_or(&r, &x, &y); break;
    case BIN_BITXOR: bn_xor(&r, &x, &y); break;
    case BIN_SHL: case BIN_SHR: case BIN_SHRA: {
        uint64_t n = 0;
        if (!bn_get_u64(&y, &n) || n > (1u << 24)) {
            el_error(el, at, "shift amount out of range");
            bn_free(&x);
            bn_free(&y);
            return val_error();
        }
        if (op == BIN_SHL)
            bn_shl(&r, &x, n);
        else if (op == BIN_SHRA && rt->kind != TY_INT && rt->kind != TY_SINT) {
            /* arithmetic shift of an unsigned value: replicate its MSB */
            BigInt s = {0};
            bn_copy(&s, &x);
            if (rt->width && bn_get_bit(&x, rt->width - 1))
                ce_wrap(&s, ty_sint(el->S, rt->width));
            bn_shr(&r, &s, n);
            bn_free(&s);
        } else {
            bn_shr(&r, &x, n);
        }
        break;
    }
    default:
        break;
    }
    v = ce_int_norm(el, rt, &r);
    bn_free(&x);
    bn_free(&y);
    bn_free(&r);
    return v;
}

static int ce_cmp_const(const Value *a, const Value *b)
{
    BigInt x = {0}, y = {0};
    int r;
    if (!(a->flags & VAL_F_BIG) && !(b->flags & VAL_F_BIG) && (a->kind == VAL_INT || a->kind == VAL_BOOL) &&
        (b->kind == VAL_INT || b->kind == VAL_BOOL))
        return a->u.i < b->u.i ? -1 : a->u.i > b->u.i;
    val_get_bn(a, &x);
    val_get_bn(b, &y);
    r = bn_cmp(&x, &y);
    bn_free(&x);
    bn_free(&y);
    return r;
}

static IrOp ce_cmp_irop(BinaryOp op)
{
    switch (op) {
    case BIN_EQ: return IROP_EQ;
    case BIN_NE: return IROP_NE;
    case BIN_LT: return IROP_LT;
    case BIN_LE: return IROP_LE;
    case BIN_GT: return IROP_GT;
    default: return IROP_GE;
    }
}

static int ce_cmp_result(BinaryOp op, int c)
{
    switch (op) {
    case BIN_EQ: return c == 0;
    case BIN_NE: return c != 0;
    case BIN_LT: return c < 0;
    case BIN_LE: return c <= 0;
    case BIN_GT: return c > 0;
    default: return c >= 0;
    }
}

static Value ce_need_design(Elab *el, Span at)
{
    el_error(el, at, "hardware values can only be used inside a module");
    return val_error();
}

static Value el_mk_hw_bool(Elab *el, IrExpr *e)
{
    return el_hw(el, el->S->t_bool, e);
}

/* tag bits of an enum value (hw) */
static IrExpr *ce_enum_tag(Elab *el, const Value *v, Span at)
{
    IrExpr *e = el_to_ir(el, v, at);
    Ty *t = v->ty;
    if (t->payload_width == 0)
        return e;
    return ir_slice(el->design->arena, e, t->payload_width, t->tag_width, at);
}

static Value ce_compare(Elab *el, BinaryOp op, Value a, Value b, Span at, Expr *ea, Expr *eb)
{
    Sema *S = el->S;
    Ty *ta = a.ty, *tb = b.ty;
    int r;
    /* enum: variant shorthands are typed by the other side in elab_eval */
    if (ta && tb && ta->kind == TY_ENUM && ta == tb) {
        if (op != BIN_EQ && op != BIN_NE) {
            el_error(el, at, "enums can only be compared with `==` and `!=`");
            return val_error();
        }
        if (a.kind == VAL_ENUM && b.kind == VAL_ENUM) {
            r = a.aux == b.aux && (!ta->variants[a.aux].payload || val_equal(&a, &b));
            return el_mk_bool(el, op == BIN_EQ ? r : !r);
        }
        if (!el->design)
            return ce_need_design(el, at);
        {
            IrExpr *x, *y;
            int tag_only = (a.kind == VAL_ENUM && !ta->variants[a.aux].payload) ||
                           (b.kind == VAL_ENUM && !ta->variants[b.aux].payload);
            if (tag_only) {
                x = ce_enum_tag(el, &a, at);
                y = ce_enum_tag(el, &b, at);
            } else {
                x = el_to_ir(el, &a, at);
                y = el_to_ir(el, &b, at);
            }
            return el_mk_hw_bool(el, ir_binary(el->design->arena, op == BIN_EQ ? IROP_EQ : IROP_NE, x, y, 0, at));
        }
    }
    if (ta && tb && ta->kind == TY_BOOL && tb->kind == TY_BOOL) {
        if (op != BIN_EQ && op != BIN_NE) {
            el_error(el, at, "bools can only be compared with `==` and `!=`");
            return val_error();
        }
        if (a.kind == VAL_BOOL && b.kind == VAL_BOOL)
            return el_mk_bool(el, op == BIN_EQ ? a.u.i == b.u.i : a.u.i != b.u.i);
        if (!el->design)
            return ce_need_design(el, at);
        return el_mk_hw_bool(el, ir_binary(el->design->arena, op == BIN_EQ ? IROP_EQ : IROP_NE,
                                           el_to_ir(el, &a, at), el_to_ir(el, &b, at), 0, at));
    }
    if (ce_int_family(ta) || ce_int_family(tb) || (ta && ta->kind == TY_BOOL) || (tb && tb->kind == TY_BOOL)) {
        int k = ce_int_operands(el, op, &a, &b, at, ea, eb);
        uint32_t w;
        int sg;
        if (!k)
            return val_error();
        if (val_is_const(&a) && val_is_const(&b))
            return el_mk_bool(el, ce_cmp_result(op, ce_cmp_const(&a, &b)));
        if (!el->design)
            return ce_need_design(el, at);
        w = TERN_MAX(a.ty->width, b.ty->width);
        sg = a.ty->kind == TY_SINT;
        return el_mk_hw_bool(el, ir_binary(el->design->arena, ce_cmp_irop(op), ce_ir_ext(el, &a, w, at),
                                           ce_ir_ext(el, &b, w, at), sg, at));
    }
    if (ta && ta == tb && (ta->kind == TY_STRUCT || ta->kind == TY_ARRAY) && ta->hw) {
        if (op != BIN_EQ && op != BIN_NE) {
            el_error(el, at, "%s values can only be compared with `==` and `!=`", ty_str(ta));
            return val_error();
        }
        if (val_is_const(&a) && val_is_const(&b)) {
            r = val_equal(&a, &b);
            return el_mk_bool(el, op == BIN_EQ ? r : !r);
        }
        if (!el->design)
            return ce_need_design(el, at);
        return el_mk_hw_bool(el, ir_binary(el->design->arena, op == BIN_EQ ? IROP_EQ : IROP_NE,
                                           el_to_ir(el, &a, at), el_to_ir(el, &b, at), 0, at));
    }
    if (val_is_const(&a) && val_is_const(&b) && (op == BIN_EQ || op == BIN_NE) && a.kind == b.kind) {
        r = val_equal(&a, &b);
        return el_mk_bool(el, op == BIN_EQ ? r : !r);
    }
    el_error(el, at, "cannot compare %s with %s", ta ? ty_str(ta) : val_kind_name((ValKind)a.kind),
             tb ? ty_str(tb) : val_kind_name((ValKind)b.kind));
    TERN_UNUSED(S);
    return val_error();
}

Value el_not(Elab *el, Value v, Span at)
{
    if (v.kind == VAL_ERROR)
        return v;
    v = el_prop_reset(el, v, at);
    if (v.kind == VAL_BOOL)
        return el_mk_bool(el, !v.u.i);
    if (v.kind == VAL_HW && v.ty->kind == TY_BOOL)
        return el_mk_hw_bool(el, ir_unary(el->design->arena, IROP_LNOT, v.u.e, at));
    el_error(el, at, "`not` needs a bool, found %s", v.ty ? ty_str(v.ty) : val_kind_name((ValKind)v.kind));
    if (ce_int_family(v.ty))
        el_help(el, "use `~` for a bitwise not, or compare: `x == 0`");
    return val_error();
}

/* inside a property a reset reads as "asserted" (spec §11.4: `always rst |-> ..`) */
Value el_prop_reset(Elab *el, Value v, Span at)
{
    if (v.kind == VAL_HW && v.ty && v.ty->kind == TY_RESET && el->mod && el->mod->in_property) {
        IrExpr *e = v.u.e;
        if (v.ty->active_low)
            e = ir_unary(el->design->arena, IROP_NOT, e, at);
        return el_hw(el, el->S->t_bool, e);
    }
    return v;
}

static Value ce_logic(Elab *el, BinaryOp op, Value a, Value b, Span at)
{
    int ba, bb;
    a = el_prop_reset(el, a, at);
    b = el_prop_reset(el, b, at);
    ba = a.ty && a.ty->kind == TY_BOOL;
    bb = b.ty && b.ty->kind == TY_BOOL;
    if (!ba || !bb) {
        Value *bad = !ba ? &a : &b;
        el_error(el, at, "`%s` needs bool operands, found %s", ce_op_text(op),
                 bad->ty ? ty_str(bad->ty) : val_kind_name((ValKind)bad->kind));
        if (ce_int_family(bad->ty))
            el_help(el, "compare explicitly (`x != 0`) or use a bitwise operator");
        return val_error();
    }
    if (a.kind == VAL_BOOL && b.kind == VAL_BOOL) {
        int r = op == BIN_AND ? (a.u.i && b.u.i) : op == BIN_OR ? (a.u.i || b.u.i) : (a.u.i != b.u.i);
        return el_mk_bool(el, r);
    }
    if (op == BIN_AND && ((a.kind == VAL_BOOL && !a.u.i) || (b.kind == VAL_BOOL && !b.u.i)))
        return el_mk_bool(el, 0);
    if (op == BIN_OR && ((a.kind == VAL_BOOL && a.u.i) || (b.kind == VAL_BOOL && b.u.i)))
        return el_mk_bool(el, 1);
    if (op != BIN_XOR) {
        if (a.kind == VAL_BOOL)
            return b;
        if (b.kind == VAL_BOOL)
            return a;
    }
    if (!el->design)
        return ce_need_design(el, at);
    return el_mk_hw_bool(el, ir_binary(el->design->arena,
                                       op == BIN_AND ? IROP_LAND : op == BIN_OR ? IROP_LOR : IROP_XOR,
                                       el_to_ir(el, &a, at), el_to_ir(el, &b, at), 0, at));
}

Value el_and(Elab *el, Value a, Value b, Span at)
{
    return ce_logic(el, BIN_AND, a, b, at);
}

Value el_or(Elab *el, Value a, Value b, Span at)
{
    return ce_logic(el, BIN_OR, a, b, at);
}

static Value ce_concat(Elab *el, Value a, Value b, Span at, Expr *ea, Expr *eb)
{
    Value *ops[2];
    Expr *es[2];
    IrExpr *parts[2];
    uint32_t i, w = 0;
    ops[0] = &a;
    ops[1] = &b;
    es[0] = ea;
    es[1] = eb;
    for (i = 0; i < 2; i++) {
        Value *v = ops[i];
        if (v->kind == VAL_ERROR)
            return val_error();
        if (ce_is_widthless(v)) {
            el_error(el, es[i] ? es[i]->span : at, "untyped integer `%s` in `++` has no width",
                     es[i] ? el_text(es[i]) : "?");
            el_help(el, "write a typed literal such as `0u4`, or cast: `x as uint<4>`");
            return val_error();
        }
        if (!v->ty || !v->ty->hw) {
            el_error(el, es[i] ? es[i]->span : at, "`++` needs hardware values, found %s",
                     v->ty ? ty_str(v->ty) : val_kind_name((ValKind)v->kind));
            return val_error();
        }
        w += v->ty->width;
    }
    if (val_is_const(&a) && val_is_const(&b)) {
        BigInt x = {0}, y = {0};
        Value r;
        el_bits_of_const(el, &a, a.ty, &x);
        el_bits_of_const(el, &b, b.ty, &y);
        bn_shl(&x, &x, b.ty->width);
        bn_or(&x, &x, &y);
        r = val_int_bn(&el->arena, ty_bits(el->S, w), &x);
        bn_free(&x);
        bn_free(&y);
        return r;
    }
    if (!el->design)
        return ce_need_design(el, at);
    parts[0] = el_to_ir(el, &a, at);
    parts[1] = el_to_ir(el, &b, at);
    return el_hw(el, ty_bits(el->S, w), ir_concat(el->design->arena, 2, parts, at));
}

Value el_binary(Elab *el, BinaryOp op, Value a, Value b, Span at, Expr *ea, Expr *eb)
{
    Sema *S = el->S;
    Ty *rt;
    int k;
    IrOp irop;
    uint32_t w;
    if (a.kind == VAL_ERROR || b.kind == VAL_ERROR)
        return val_error();
    switch (op) {
    case BIN_AND: case BIN_OR: case BIN_XOR:
        return ce_logic(el, op, a, b, at);
    case BIN_EQ: case BIN_NE: case BIN_LT: case BIN_LE: case BIN_GT: case BIN_GE:
        return ce_compare(el, op, a, b, at, ea, eb);
    case BIN_CONCAT:
        return ce_concat(el, a, b, at, ea, eb);
    case BIN_BITAND: case BIN_BITOR: case BIN_BITXOR:
        if (a.ty && b.ty && a.ty->kind == TY_BOOL && b.ty->kind == TY_BOOL) {
            if (a.kind == VAL_BOOL && b.kind == VAL_BOOL)
                return el_mk_bool(el, op == BIN_BITAND ? (a.u.i & b.u.i) : op == BIN_BITOR ? (a.u.i | b.u.i)
                                                                                        : (a.u.i ^ b.u.i));
            if (!el->design)
                return ce_need_design(el, at);
            return el_mk_hw_bool(el, ir_binary(el->design->arena,
                                               op == BIN_BITAND ? IROP_AND : op == BIN_BITOR ? IROP_OR : IROP_XOR,
                                               el_to_ir(el, &a, at), el_to_ir(el, &b, at), 0, at));
        }
        break;
    default:
        break;
    }
    if (op == BIN_IN || op == BIN_UNTIL || op == BIN_IMPLIES || op == BIN_IMPLIES_NEXT) {
        el_error(el, at, "`%s` is only valid in a property", ce_op_text(op));
        return val_error();
    }
    /* shifts: the result has the type of the left operand */
    if (op == BIN_SHL || op == BIN_SHR || op == BIN_SHRA) {
        int fa = ce_int_family(a.ty), fb = ce_int_family(b.ty);
        if (!fa || !fb) {
            if ((a.ty && a.ty->kind == TY_BOOL) || (b.ty && b.ty->kind == TY_BOOL)) {
                el_error(el, at, "bool is not an integer: `%s` on bool", ce_op_text(op));
                el_help(el, "convert with `x as uint<1>`");
            } else {
                el_error(el, at, "`%s` needs integer operands", ce_op_text(op));
            }
            return val_error();
        }
        if (fb == 2 && !ce_is_widthless(&b)) {
            el_error(el, at, "shift amount must be unsigned, found %s", ty_str(b.ty));
            return val_error();
        }
        if (val_is_const(&b)) {
            BigInt y = {0};
            val_get_bn(&b, &y);
            if (bn_is_neg(&y)) {
                el_error(el, at, "negative shift amount");
                bn_free(&y);
                return val_error();
            }
            bn_free(&y);
        }
        rt = a.ty;
        if (val_is_const(&a) && val_is_const(&b))
            return ce_fold_int(el, op, &a, &b, rt, at);
        if (ce_is_widthless(&a)) {
            el_error(el, at, "untyped integer `%s` shifted by a signal has no width", ea ? el_text(ea) : "?");
            el_help(el, "give it a type: `1u8 << n` or `(1 as uint<8>) << n`");
            return val_error();
        }
        if (!el->design)
            return ce_need_design(el, at);
        irop = op == BIN_SHL ? IROP_SHL : op == BIN_SHR ? IROP_SHR : IROP_ASHR;
        {
            IrExpr *amt;
            if (b.kind == VAL_HW)
                amt = b.u.e;
            else {
                uint64_t n = 0;
                val_get_u64(&b, &n);
                if (n >= rt->width)
                    n = rt->width;
                amt = ir_const_u64(el->design->arena, ty_clog2(rt->width + 1) ? ty_clog2(rt->width + 1) : 1, n, at);
            }
            return el_hw(el, rt, ir_binary(el->design->arena, irop, el_to_ir(el, &a, at), amt,
                                           rt->kind == TY_SINT, at));
        }
    }
    if (op == BIN_DIV || op == BIN_MOD) {
        if (!val_is_const(&b)) {
            el_error(el, at, "division by a signal: `%s` is not a compile-time constant", eb ? el_text(eb) : "?");
            el_help(el, "divide by a constant, or use std::math::Divider");
            return val_error();
        }
    }
    k = ce_int_operands(el, op, &a, &b, at, ea, eb);
    if (!k)
        return val_error();
    if (k == 2) {
        rt = (a.ty->is_signed || b.ty->is_signed || op == BIN_SUB || op == BIN_SUB_WRAP) ? S->t_sint : S->t_int;
        {
            Value r = ce_fold_int(el, op, &a, &b, S->t_sint, at);
            if (r.kind == VAL_INT && !(a.ty->is_signed || b.ty->is_signed)) {
                BigInt x = {0};
                val_get_bn(&r, &x);
                r.ty = bn_is_neg(&x) ? S->t_sint : S->t_int;
                bn_free(&x);
            }
            TERN_UNUSED(rt);
            return r;
        }
    }
    {
        Ty *ta = a.ty, *tb = b.ty;
        int sg = ta->kind == TY_SINT;
        uint32_t na = ta->width, nb = tb->width;
        TyKind rk = sg ? TY_SINT : TY_UINT;
        switch (op) {
        case BIN_ADD: case BIN_SUB: w = TERN_MAX(na, nb) + 1; break;
        case BIN_ADD_WRAP: case BIN_SUB_WRAP: w = TERN_MAX(na, nb); break;
        case BIN_MUL: w = na + nb; break;
        case BIN_DIV: case BIN_MOD: w = na; rk = ta->kind == TY_BIT ? TY_UINT : ta->kind; break;
        case BIN_BITAND: case BIN_BITOR: case BIN_BITXOR:
            w = TERN_MAX(na, nb);
            rk = sg ? TY_SINT : (ta->kind == TY_BIT && tb->kind == TY_BIT) ? TY_BIT : TY_UINT;
            break;
        default: w = TERN_MAX(na, nb); break;
        }
        rt = ty_scalar(S, rk, w);
        if (val_is_const(&a) && val_is_const(&b)) {
            if ((op == BIN_DIV || op == BIN_MOD) && val_is_const(&b)) {
                BigInt y = {0};
                val_get_bn(&b, &y);
                if (bn_is_zero(&y)) {
                    el_error(el, at, "division by zero");
                    bn_free(&y);
                    return val_error();
                }
                bn_free(&y);
            }
            return ce_fold_int(el, op, &a, &b, rt, at);
        }
        if (!el->design)
            return ce_need_design(el, at);
        switch (op) {
        case BIN_ADD: case BIN_ADD_WRAP: irop = IROP_ADD; break;
        case BIN_SUB: case BIN_SUB_WRAP: irop = IROP_SUB; break;
        case BIN_MUL: irop = IROP_MUL; break;
        case BIN_DIV: irop = IROP_DIV; break;
        case BIN_MOD: irop = IROP_MOD; break;
        case BIN_BITAND: irop = IROP_AND; break;
        case BIN_BITOR: irop = IROP_OR; break;
        default: irop = IROP_XOR; break;
        }
        if (op == BIN_DIV || op == BIN_MOD) {
            BigInt y = {0};
            uint32_t ww = TERN_MAX(na, nb);
            IrExpr *r;
            val_get_bn(&b, &y);
            if (bn_is_zero(&y)) {
                el_error(el, at, "division by zero");
                bn_free(&y);
                return val_error();
            }
            bn_free(&y);
            r = ir_binary(el->design->arena, irop, ce_ir_ext(el, &a, ww, at), ce_ir_ext(el, &b, ww, at), sg, at);
            if (ww != w)
                r = ir_resize(el->design->arena, r, w, sg, at);
            return el_hw(el, rt, r);
        }
        return el_hw(el, rt, ir_binary(el->design->arena, irop, ce_ir_ext(el, &a, w, at), ce_ir_ext(el, &b, w, at),
                                       sg, at));
    }
}

static Value ce_unary(Elab *el, UnaryOp op, Value v, Span at, Expr *src)
{
    Sema *S = el->S;
    Ty *t = v.ty;
    if (v.kind == VAL_ERROR)
        return v;
    switch (op) {
    case UN_NOT:
        return el_not(el, v, at);
    case UN_NEG: {
        int f = ce_int_family(t);
        Ty *rt;
        if (!f) {
            if (t && t->kind == TY_BOOL) {
                el_error(el, at, "bool is not an integer: `-` on bool");
                el_help(el, "use `not` for a logical negation");
            } else {
                el_error(el, at, "`-` needs an integer, found %s", t ? ty_str(t) : val_kind_name((ValKind)v.kind));
            }
            return val_error();
        }
        if (t->kind == TY_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&v, &b);
            bn_neg(&b, &b);
            r = val_int_bn(&el->arena, bn_is_neg(&b) || t->is_signed ? S->t_sint : S->t_int, &b);
            bn_free(&b);
            return r;
        }
        rt = ty_sint(S, t->width + 1);
        if (v.kind == VAL_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&v, &b);
            bn_neg(&b, &b);
            r = ce_int_norm(el, rt, &b);
            bn_free(&b);
            return r;
        }
        return el_hw(el, rt, ir_unary(el->design->arena, IROP_NEG, ce_ir_ext(el, &v, rt->width, at), at));
    }
    case UN_BITNOT:
        if (t && t->kind == TY_BOOL)
            return el_not(el, v, at);
        if (!ty_is_sized_int(t)) {
            if (t && t->kind == TY_INT)
                el_error(el, at, "`~` on an untyped integer has no width");
            else
                el_error(el, at, "`~` needs a sized integer, found %s", t ? ty_str(t) : val_kind_name((ValKind)v.kind));
            return val_error();
        }
        if (v.kind == VAL_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&v, &b);
            bn_not(&b, &b);
            r = ce_int_norm(el, t, &b);
            bn_free(&b);
            return r;
        }
        return el_hw(el, t, ir_unary(el->design->arena, IROP_NOT, v.u.e, at));
    default: {
        IrOp irop = op == UN_RED_AND ? IROP_RAND : op == UN_RED_OR ? IROP_ROR : IROP_RXOR;
        if (!t || !t->hw || t->kind == TY_CLOCK || t->kind == TY_RESET) {
            el_error(el, at, "reduction needs a sized value, found %s",
                     t ? ty_str(t) : val_kind_name((ValKind)v.kind));
            return val_error();
        }
        if (val_is_const(&v)) {
            BigInt b = {0};
            uint32_t i, ones = 0, w = t->width;
            int r;
            el_bits_of_const(el, &v, t, &b);
            for (i = 0; i < w; i++)
                ones += (uint32_t)bn_get_bit(&b, i);
            bn_free(&b);
            r = op == UN_RED_AND ? ones == w : op == UN_RED_OR ? ones != 0 : (int)(ones & 1);
            return el_mk_bool(el, r);
        }
        TERN_UNUSED(src);
        return el_mk_hw_bool(el, ir_unary(el->design->arena, irop, el_to_ir(el, &v, at), at));
    }
    }
}

/* ================================================================ casts */

static int ce_is_clock_ok(Elab *el)
{
    return !el->mod || el->mod->attr_clock_gen;
}

Value el_cast(Elab *el, Value v, Ty *to, Span at, Expr *src)
{
    Sema *S = el->S;
    Ty *from = v.ty;
    if (v.kind == VAL_ERROR || to->kind == TY_ERROR)
        return val_error();
    if (from == to)
        return v;
    if (!from) {
        el_error(el, at, "cannot cast a %s", val_kind_name((ValKind)v.kind));
        return val_error();
    }
    if (from->kind == TY_RESET && to->kind != TY_RESET) {
        if (!el->mod || !el->mod->attr_cdc) {
            el_error(el, at, "reset `%s` cannot be read as data", src ? el_text(src) : "?");
            el_help(el, "to know whether the design is in reset, use a `reg in_reset: bool = true` that you clear");
            return val_error();
        }
    }
    if (from->kind == TY_CLOCK && to->kind != TY_CLOCK && !ce_is_clock_ok(el)) {
        el_error(el, at, "clock `%s` cannot be used as data", src ? el_text(src) : "?");
        el_help(el, "clock logic belongs in std::clk (Divider, Gate)");
        return val_error();
    }
    switch (to->kind) {
    case TY_INT:
        if (v.kind == VAL_INT || v.kind == VAL_BOOL) {
            v.kind = VAL_INT;
            if (!to->is_signed && ((v.flags & VAL_F_BIG) ? bn_is_neg(v.u.big) : v.u.i < 0)) {
                el_error(el, at, "negative value cast to widthless `uint`");
                return val_error();
            }
            v.ty = to;
            return v;
        }
        if (v.kind == VAL_ENUM) {
            Value r = val_int(to, (int64_t)from->variants[v.aux].tag);
            return r;
        }
        el_error(el, at, "`%s` is not a compile-time integer", src ? el_text(src) : "value");
        return val_error();
    case TY_BOOL:
        if (from->kind == TY_TRI && from->elem->kind == TY_BOOL)
            return el_hw(el, to, v.u.e);
        if (!from->hw && from->kind != TY_INT) {
            el_error(el, at, "cannot cast %s to bool", ty_str(from));
            return val_error();
        }
        if (from->kind == TY_CLOCK || from->kind == TY_RESET) {
            if (v.kind == VAL_HW)
                return el_hw(el, to, v.u.e);
        }
        if (val_is_const(&v)) {
            BigInt b = {0};
            int nz;
            if (v.kind == VAL_INT || v.kind == VAL_BOOL)
                val_get_bn(&v, &b);
            else
                el_bits_of_const(el, &v, from, &b);
            nz = !bn_is_zero(&b);
            bn_free(&b);
            return el_mk_bool(el, nz);
        }
        if (from->width == 1)
            return el_hw(el, to, v.u.e);
        return el_mk_hw_bool(el, ir_unary(el->design->arena, IROP_ROR, v.u.e, at));
    case TY_BIT: case TY_UINT: case TY_SINT: {
        int ext_signed = from->kind == TY_SINT;
        if (from->kind == TY_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&v, &b);
            r = ce_int_norm(el, to, &b);
            bn_free(&b);
            return r;
        }
        if (!from->hw) {
            el_error(el, at, "cannot cast %s to %s", ty_str(from), ty_str(to));
            return val_error();
        }
        if (val_is_const(&v)) {
            BigInt b = {0};
            Value r;
            if (v.kind == VAL_INT || v.kind == VAL_BOOL) {
                val_get_bn(&v, &b);
            } else {
                el_bits_of_const(el, &v, from, &b);
            }
            r = ce_int_norm(el, to, &b);
            bn_free(&b);
            return r;
        }
        if (to->width == from->width) {
            if (from->kind == TY_STRUCT || from->kind == TY_ENUM || from->kind == TY_ARRAY ||
                (to->kind == TY_SINT) != (from->kind == TY_SINT))
                return el_hw(el, to, ir_resize(el->design->arena, v.u.e, to->width, 0, at));
            return el_hw(el, to, v.u.e);
        }
        return el_hw(el, to, ir_resize(el->design->arena, v.u.e, to->width, ext_signed, at));
    }
    case TY_ENUM: case TY_STRUCT: case TY_ARRAY:
        if (to->hw && from->hw && (ty_is_sized_int(from) || from->kind == TY_BOOL) && from->width == to->width) {
            if (val_is_const(&v)) {
                IrBits bits = elab_const_bits(el, &v, from);
                return elab_value_from_bits(el, &bits, to);
            }
            return el_hw(el, to, ir_resize(el->design->arena, v.u.e, to->width, 0, at));
        }
        if (from->kind == TY_INT && to->hw && val_is_const(&v)) {
            BigInt b = {0};
            IrBits bits;
            val_get_bn(&v, &b);
            bits.width = to->width;
            bits.w = ANEWN(&el->arena, uint64_t, IR_LIMBS(to->width));
            bn_to_limbs(&b, bits.w, to->width);
            bn_free(&b);
            return elab_value_from_bits(el, &bits, to);
        }
        el_error(el, at, "cannot cast %s to %s", ty_str(from), ty_str(to));
        if (to->hw && from->hw)
            el_help(el, "use `%s::from_bits(bits(x))` when the widths match", ty_str(to));
        return val_error();
    case TY_CLOCK:
        if (from->kind == TY_CLOCK || from->kind == TY_BOOL) {
            if (v.kind == VAL_HW)
                return el_hw(el, to, v.u.e);
            el_error(el, at, "a constant clock");
            return val_error();
        }
        el_error(el, at, "cannot cast %s to clock", ty_str(from));
        return val_error();
    case TY_RESET:
        if (from->kind == TY_RESET || from->kind == TY_BOOL) {
            if (v.kind == VAL_HW) {
                if (from->kind == TY_RESET && from->active_low != to->active_low)
                    return el_hw(el, to, ir_unary(el->design->arena, IROP_NOT, v.u.e, at));
                return el_hw(el, to, v.u.e);
            }
            if (v.kind == VAL_BOOL) {
                IrExpr *e;
                if (!el->design)
                    return ce_need_design(el, at);
                e = ir_const_u64(el->design->arena, 1, (uint64_t)(to->active_low ? !v.u.i : v.u.i), at);
                return el_hw(el, to, e);
            }
        }
        el_error(el, at, "cannot cast %s to %s", ty_str(from), ty_str(to));
        return val_error();
    case TY_TRI:
        el_error(el, at, "cannot cast to %s", ty_str(to));
        return val_error();
    default:
        el_error(el, at, "cannot cast %s to %s", ty_str(from), ty_str(to));
        TERN_UNUSED(S);
        return val_error();
    }
}

/* ============================================================== selection */

static Place *ce_sub_place(Elab *el, Place *p, uint32_t lo, uint32_t width, Ty *ty, const char *field)
{
    Place *q;
    if (p && p->kind == PL_DYN) {
        /* a field of the element selected by a dynamic index */
        uint32_t k;
        q = ANEW(&el->arena, Place);
        *q = *p;
        q->ty = ty;
        q->subs = ANEWN(&el->arena, Place *, p->n ? p->n : 1);
        for (k = 0; k < p->n; k++) {
            q->subs[k] = ce_sub_place(el, p->subs[k], lo, width, ty, field);
            if (!q->subs[k])
                return NULL;
        }
        return q;
    }
    if (!p || p->kind != PL_SIG || p->index)
        return NULL;
    q = ANEW(&el->arena, Place);
    *q = *p;
    q->lo = p->lo + lo;
    q->width = width;
    q->ty = ty;
    if (field) {
        if (p->field)
            q->field = intern_cstr(arena_printf(&el->arena, "%s.%s", p->field, field));
        else
            q->field = field;
    }
    return q;
}

static Value ce_hw_slice(Elab *el, const Value *base, uint32_t lo, uint32_t width, Ty *ty, const char *field,
                         Span at)
{
    IrExpr *e = ir_slice(el->design->arena, base->u.e, lo, width, at);
    Value r;
    e->field = field;
    r = el_hw(el, ty, e);
    r.place = ce_sub_place(el, base->place, lo, width, ty, field);
    return r;
}

Value el_field(Elab *el, Value base, const char *name, Span at)
{
    Ty *t = base.ty;
    int k;
    switch (base.kind) {
    case VAL_ERROR:
        return base;
    case VAL_INST:
        return el_inst_field(el, base, name, at);
    case VAL_DOMAIN:
        return el_domain_field(el, base, name, at);
    case VAL_OPAQUE: {
        Value out = val_error();
        int r = base.u.opq->ops->get_field ? base.u.opq->ops->get_field(el, base.u.opq->obj, name, at, &out) : 0;
        if (r == 0)
            el_error(el, at, "%s has no field `%s`", base.u.opq->ops->type_name, name);
        return r > 0 ? out : val_error();
    }
    default:
        break;
    }
    if (strcmp(name, "len") == 0 && t && t->kind == TY_ARRAY)
        return val_int(el->S->t_int, t->len);
    if (strcmp(name, "len") == 0 && base.kind == VAL_ARRAY)
        return val_int(el->S->t_int, base.u.list->n);
    if (base.kind == VAL_BUNDLE) {
        k = ty_field_index(t, name);
        if (k < 0) {
            el_error(el, at, "bundle `%s` has no field `%s`", ty_str(t), name);
            return val_error();
        }
        if (base.u.list->v[k].kind == VAL_VOID) {
            el_error(el, at, "field `%s` is not part of this bundle literal", name);
            return val_error();
        }
        return base.u.list->v[k];
    }
    if (t && t->kind == TY_STRUCT) {
        k = ty_field_index(t, name);
        if (k < 0) {
            el_error(el, at, "struct `%s` has no field `%s`", ty_str(t), name);
            return val_error();
        }
        if (base.kind == VAL_STRUCT)
            return base.u.list->v[k];
        if (base.kind == VAL_HW)
            return ce_hw_slice(el, &base, t->fields[k].lsb, t->fields[k].ty->width, t->fields[k].ty, name, at);
    }
    if (base.kind == VAL_TUPLE && t) {
        k = ty_field_index(t, name);
        if (k >= 0)
            return base.u.list->v[k];
    }
    el_error(el, at, "no field `%s` on %s", name, t ? ty_str(t) : val_kind_name((ValKind)base.kind));
    return val_error();
}

static int ce_check_index_width(Elab *el, Ty *it, uint32_t len, Span at, int is_bits);

/* Element `ie` (hardware index) of the n values elems[] (an array of bundles,
 * or of arrays of bundles): bundles are selected field by field, hardware
 * leaves by an index into their concatenation (out of range: 0). The result
 * carries a PL_DYN place (write decode) when every element has a place. */
static Value ce_dyn_pick(Elab *el, Value *elems, uint32_t n, IrExpr *ie, Span at)
{
    Value e0 = elems[0], r;
    uint32_t k, f;
    for (k = 0; k < n; k++)
        if (elems[k].kind == VAL_ERROR)
            return val_error();
    if (e0.kind == VAL_BUNDLE || (e0.kind == VAL_ARRAY && (!e0.ty || !e0.ty->hw))) {
        uint32_t nf = e0.u.list->n;
        Value *sub = ANEWN(&el->arena, Value, n);
        Place *agg = ANEW(&el->arena, Place);
        int places = 1;
        r = val_list(&el->arena, (ValKind)e0.kind, e0.ty, nf);
        agg->kind = PL_AGG;
        agg->ty = e0.ty;
        agg->n = nf;
        agg->subs = ANEWN(&el->arena, Place *, nf ? nf : 1);
        for (f = 0; f < nf; f++) {
            int missing = 0;
            for (k = 0; k < n; k++) {
                if (elems[k].kind != e0.kind || elems[k].u.list->n != nf) {
                    el_error(el, at, "the elements of this array have different shapes");
                    return val_error();
                }
                sub[k] = elems[k].u.list->v[f];
                if (sub[k].kind == VAL_VOID)
                    missing = 1;
            }
            if (missing) {
                r.u.list->v[f] = val_void(el->S);
                places = 0;
                continue;
            }
            r.u.list->v[f] = ce_dyn_pick(el, sub, n, ie, at);
            if (r.u.list->v[f].kind == VAL_ERROR)
                return val_error();
            agg->subs[f] = r.u.list->v[f].place;
            if (!agg->subs[f])
                places = 0;
        }
        if (places)
            r.place = agg;
        return r;
    }
    if (e0.kind == VAL_INST || e0.kind == VAL_MEM || e0.kind == VAL_DOMAIN) {
        el_error(el, at, "an array of %s can only be indexed with a compile-time index",
                 e0.kind == VAL_INST ? "instances" : e0.kind == VAL_MEM ? "memories" : "domains");
        return val_error();
    }
    if (!e0.ty || !e0.ty->hw) {
        el_error(el, at, "cannot index an array of %s with a signal", e0.ty ? ty_str(e0.ty) : val_kind_name((ValKind)e0.kind));
        return val_error();
    }
    {
        IrExpr **parts = (IrExpr **)xcalloc(n, sizeof(IrExpr *));
        Place *dp = ANEW(&el->arena, Place);
        int places = 1;
        dp->kind = PL_DYN;
        dp->ty = e0.ty;
        dp->n = n;
        dp->index = ie;
        dp->subs = ANEWN(&el->arena, Place *, n);
        for (k = 0; k < n; k++) {
            Value x = elems[k].ty == e0.ty ? elems[k] : elab_convert(el, elems[k], e0.ty, NULL, NULL, at);
            if (x.kind == VAL_ERROR) {
                free(parts);
                return x;
            }
            parts[n - 1 - k] = el_to_ir(el, &x, at);
            dp->subs[k] = elems[k].place;
            if (!dp->subs[k])
                places = 0;
        }
        r = el_hw(el, e0.ty, ir_index(el->design->arena, n == 1 ? parts[0] : ir_concat(el->design->arena, n, parts, at),
                                      ie, e0.ty->width, at));
        free(parts);
        if (places)
            r.place = dp;
        return r;
    }
}

static int ce_check_index_width(Elab *el, Ty *it, uint32_t len, Span at, int is_bits)
{
    uint32_t need = ty_clog2(len);
    if (it->kind == TY_ENUM) {
        if (it->payload_width || it->enc != IR_ENC_BINARY || it->nvariants > len) {
            el_error(el, at, "enum index `%s` does not fit an array of %u elements", ty_str(it), (unsigned)len);
            return 0;
        }
        return 1;
    }
    if (it->kind == TY_SINT) {
        el_error(el, at, "array index must be unsigned, found %s", ty_str(it));
        return 0;
    }
    if (it->kind == TY_BOOL && len >= 2 && need == 1)
        return 1;
    if (!ty_is_sized_int(it)) {
        el_error(el, at, "index must be an unsigned integer or an enum, found %s", ty_str(it));
        return 0;
    }
    if (need == 0)
        need = 1;
    if (it->width > need) {
        el_error(el, at, "index %s is too wide for %u elements: it may be out of range", ty_str(it), (unsigned)len);
        el_help(el, "an index into %u elements must be uint<%u>; truncate with `as uint<%u>`", (unsigned)len,
                (unsigned)need, (unsigned)need);
        return 0;
    }
    if (it->width < need && !is_bits) {
        el_error(el, at, "index %s is too narrow for %u elements: it must be exactly uint<%u>", ty_str(it),
                 (unsigned)len, (unsigned)need);
        el_help(el, "widen it with `as uint<%u>`", (unsigned)need);
        return 0;
    }
    return 1;
}

Value el_index(Elab *el, Value base, Value idx, Span at, Expr *idx_expr)
{
    Ty *t = base.ty;
    if (base.kind == VAL_ERROR || idx.kind == VAL_ERROR)
        return val_error();
    if (base.kind == VAL_MEM)
        return el_mem_index(el, base, idx, at);
    if (base.kind == VAL_OPAQUE) {
        Value out = val_error();
        int r = base.u.opq->ops->index ? base.u.opq->ops->index(el, base.u.opq->obj, idx, at, &out) : 0;
        if (r == 0)
            el_error(el, at, "%s cannot be indexed", base.u.opq->ops->type_name);
        return r > 0 ? out : val_error();
    }
    if (val_is_const(&idx) && idx.kind == VAL_INT) {
        int64_t i = -1;
        uint32_t len;
        if (!val_get_i64(&idx, &i))
            i = -1;
        if (base.kind == VAL_ARRAY || base.kind == VAL_TUPLE) {
            len = base.u.list->n;
            if (i < 0 || (uint64_t)i >= len) {
                el_error(el, at, "index %lld out of range for %u elements", (long long)i, (unsigned)len);
                return val_error();
            }
            return base.u.list->v[i];
        }
        if (t && t->kind == TY_ARRAY && base.kind == VAL_HW) {
            if (i < 0 || (uint64_t)i >= t->len) {
                el_error(el, at, "index %lld out of range for %s", (long long)i, ty_str(t));
                return val_error();
            }
            return ce_hw_slice(el, &base, (uint32_t)i * t->elem->width, t->elem->width, t->elem, NULL, at);
        }
        if (t && (ty_is_sized_int(t))) {
            if (i < 0 || (uint64_t)i >= t->width) {
                el_error(el, at, "bit index %lld out of range for %s", (long long)i, ty_str(t));
                return val_error();
            }
            if (base.kind == VAL_INT) {
                BigInt b = {0};
                int bit;
                val_get_bn(&base, &b);
                bit = bn_get_bit(&b, (uint64_t)i);
                bn_free(&b);
                return el_mk_bool(el, bit);
            }
            return ce_hw_slice(el, &base, (uint32_t)i, 1, el->S->t_bool, NULL, at);
        }
        if (t && t->kind == TY_INT && base.kind == VAL_INT) {
            BigInt b = {0};
            int bit;
            if (i < 0) {
                el_error(el, at, "negative bit index");
                return val_error();
            }
            val_get_bn(&base, &b);
            bit = bn_get_bit(&b, (uint64_t)i);
            bn_free(&b);
            return el_mk_bool(el, bit);
        }
    }
    if (idx.kind == VAL_ENUM && val_is_const(&idx) && (base.kind == VAL_ARRAY || (t && t->kind == TY_ARRAY))) {
        Value iv = val_int(el->S->t_int, (int64_t)idx.ty->variants[idx.aux].tag);
        return el_index(el, base, iv, at, idx_expr);
    }
    /* dynamic index */
    if (!idx.ty || !idx.ty->hw) {
        el_error(el, at, "index must be an integer, found %s", idx.ty ? ty_str(idx.ty) : val_kind_name((ValKind)idx.kind));
        return val_error();
    }
    if (base.kind == VAL_ARRAY) {
        if (t && t->hw) {
            base = ce_materialize_array(el, base, t, at);
            if (base.kind == VAL_ARRAY) {
                Value hv = el_hw(el, t, el_to_ir(el, &base, at));
                base = hv;
            }
        } else {
            /* array of bundles: a mux per field for reads, a decode for writes */
            IrExpr *ie;
            uint32_t n = base.u.list->n;
            if (!el->design)
                return ce_need_design(el, at);
            if (!n || !ce_check_index_width(el, idx.ty, n, idx_expr ? idx_expr->span : at, 0))
                return val_error();
            ie = el_to_ir(el, &idx, at);
            return ce_dyn_pick(el, base.u.list->v, n, ie, at);
        }
    }
    if (!el->design)
        return ce_need_design(el, at);
    if (t && t->kind == TY_ARRAY && base.kind == VAL_HW) {
        Value r;
        IrExpr *ie;
        if (!ce_check_index_width(el, idx.ty, t->len, idx_expr ? idx_expr->span : at, 0))
            return val_error();
        ie = idx.ty->kind == TY_ENUM ? el_to_ir(el, &idx, at) : el_to_ir(el, &idx, at);
        r = el_hw(el, t->elem, ir_index(el->design->arena, base.u.e, ie, t->elem->width, at));
        if (base.place && base.place->kind == PL_SIG && !base.place->index) {
            Place *q = ANEW(&el->arena, Place);
            *q = *base.place;
            q->index = ie;
            q->elem_width = t->elem->width;
            q->len = t->len;
            q->width = t->elem->width;
            q->ty = t->elem;
            r.place = q;
        }
        return r;
    }
    if (t && ty_is_sized_int(t)) {
        Value r;
        IrExpr *ie;
        if (base.kind == VAL_INT)
            base = el_hw(el, t, el_to_ir(el, &base, at));
        if (!ce_check_index_width(el, idx.ty, t->width, idx_expr ? idx_expr->span : at, 1))
            return val_error();
        ie = el_to_ir(el, &idx, at);
        r = el_hw(el, el->S->t_bool, ir_index(el->design->arena, base.u.e, ie, 1, at));
        if (base.place && base.place->kind == PL_SIG && !base.place->index) {
            Place *q = ANEW(&el->arena, Place);
            *q = *base.place;
            q->index = ie;
            q->elem_width = 1;
            q->len = t->width;
            q->width = 1;
            q->ty = el->S->t_bool;
            r.place = q;
        }
        return r;
    }
    el_error(el, at, "cannot index %s", t ? ty_str(t) : val_kind_name((ValKind)base.kind));
    return val_error();
}

static Value ce_slice(Elab *el, Value base, Expr *ehi, Expr *elo, Env *env, Span at)
{
    Value hi = elab_eval_const(el, ehi, env, el->S->t_int, "a slice bound");
    Value lo = elab_eval_const(el, elo, env, el->S->t_int, "a slice bound");
    int64_t h, l;
    Ty *t = base.ty, *rt;
    if (base.kind == VAL_ERROR || hi.kind == VAL_ERROR || lo.kind == VAL_ERROR)
        return val_error();
    if (!val_get_i64(&hi, &h) || !val_get_i64(&lo, &l) || l < 0 || h < l) {
        el_error(el, at, "invalid slice bounds [%s:%s] (write [hi:lo] with hi >= lo)", el_text(ehi), el_text(elo));
        return val_error();
    }
    if (!t || !t->hw || t->kind == TY_CLOCK || t->kind == TY_RESET) {
        if (t && t->kind == TY_INT && base.kind == VAL_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&base, &b);
            bn_shr(&b, &b, (uint64_t)l);
            rt = ty_bits(el->S, (uint32_t)(h - l + 1));
            r = ce_int_norm(el, rt, &b);
            bn_free(&b);
            return r;
        }
        el_error(el, at, "cannot slice %s", t ? ty_str(t) : val_kind_name((ValKind)base.kind));
        return val_error();
    }
    if ((uint64_t)h >= t->width) {
        el_error(el, at, "slice [%lld:%lld] out of range for %s (%u bits)", (long long)h, (long long)l, ty_str(t),
                 (unsigned)t->width);
        return val_error();
    }
    rt = ty_bits(el->S, (uint32_t)(h - l + 1));
    if (val_is_const(&base)) {
        BigInt b = {0};
        Value r;
        el_bits_of_const(el, &base, t, &b);
        bn_shr(&b, &b, (uint64_t)l);
        r = ce_int_norm(el, rt, &b);
        bn_free(&b);
        return r;
    }
    return ce_hw_slice(el, &base, (uint32_t)l, (uint32_t)(h - l + 1), rt, NULL, at);
}

/* ============================================================== literals */

static Value ce_int_literal(Elab *el, Expr *e, Env *env, Ty *expect)
{
    Sema *S = el->S;
    BigInt b = {0};
    Value v;
    if (e->u.int_lit.overflow)
        bn_parse(&b, e->u.int_lit.text, strlen(e->u.int_lit.text), 0);
    else
        bn_set_u64(&b, e->u.int_lit.value);
    if (e->u.int_lit.suffix) {
        uint32_t w = ce_width_arg(el, e->u.int_lit.width, env);
        Ty *t;
        if (!w) {
            bn_free(&b);
            return val_error();
        }
        t = ty_scalar(S, e->u.int_lit.suffix == 's' ? TY_SINT : TY_UINT, w);
        if (!el_fits(&b, t)) {
            StrBuf sb = {0}, rs = {0};
            bn_to_sb(&sb, &b, 10);
            el_range_text(t, &rs);
            el_error(el, e->span, "literal %s does not fit in %s (%s)", sb_cstr(&sb), ty_str(t), sb_cstr(&rs));
            sb_free(&sb);
            sb_free(&rs);
            bn_free(&b);
            return val_error();
        }
        v = val_int_bn(&el->arena, t, &b);
        bn_free(&b);
        return v;
    }
    v = val_int_bn(&el->arena, S->t_int, &b);
    bn_free(&b);
    if (expect && (ty_is_sized_int(expect) || expect->kind == TY_INT))
        return elab_convert(el, v, expect, e, NULL, e->span);
    return v;
}

static Ty *ce_payload_or_struct(Ty *t, uint32_t variant)
{
    if (t->kind == TY_ENUM)
        return t->variants[variant].payload;
    return t;
}

/* builds the field values of a struct/bundle/variant literal in declaration order */
static int ce_literal_fields(Elab *el, Ty *ft, FieldInit *fs, uint32_t nfs, int has_rest, Env *env, int bundle,
                             ValList *out, Span at)
{
    uint32_t i;
    int ok = 1;
    out->n = ft ? ft->nfields : 0;
    out->v = ANEWN(&el->arena, Value, out->n ? out->n : 1);
    for (i = 0; i < out->n; i++)
        out->v[i].kind = VAL_VOID;
    for (i = 0; i < nfs; i++) {
        FieldInit *f = &fs[i];
        int k = ty_field_index(ft, f->name.name);
        Value v;
        if (k < 0) {
            el_error(el, f->name.span, "`%s` has no field `%s`", ft ? ty_str(ft) : "?", f->name.name);
            ok = 0;
            continue;
        }
        if (bundle && ft->fields[k].flip) {
            el_error(el, f->name.span, "flip field `%s` cannot be set in a bundle literal", f->name.name);
            el_help(el, "a literal only gives the fields flowing with the bundle; assign `%s` separately",
                    f->name.name);
            ok = 0;
            continue;
        }
        if (f->value)
            v = elab_eval(el, f->value, env, ft->fields[k].ty);
        else
            v = el_lookup_name(el, env, f->name.name, f->name.span, 1);
        v = elab_convert(el, v, ft->fields[k].ty, f->value, ce_what("field `", f->name.name),
                         f->span);
        if (v.kind == VAL_ERROR)
            ok = 0;
        out->v[k] = v;
    }
    for (i = 0; i < out->n; i++) {
        if (out->v[i].kind != VAL_VOID)
            continue;
        if (bundle && ft->fields[i].flip)
            continue;
        if (has_rest) {
            out->v[i] = el_zero(el, ft->fields[i].ty);
            continue;
        }
        if (ok)
            el_error(el, at, "missing field `%s` in `%s` literal", ft->fields[i].name, ty_str(ft));
        ok = 0;
    }
    return ok;
}

static Value ce_build_struct(Elab *el, Ty *t, ValList *fv, Span at)
{
    uint32_t i;
    int all_const = 1;
    IrExpr **parts;
    IrExpr *e;
    Value v;
    for (i = 0; i < fv->n; i++)
        if (!val_is_const(&fv->v[i]))
            all_const = 0;
    if (all_const) {
        v = val_list(&el->arena, VAL_STRUCT, t, 0);
        v.u.list = ANEW(&el->arena, ValList);
        *v.u.list = *fv;
        return v;
    }
    if (!el->design)
        return ce_need_design(el, at);
    parts = (IrExpr **)xcalloc(fv->n ? fv->n : 1, sizeof(IrExpr *));
    for (i = 0; i < fv->n; i++)
        parts[i] = el_to_ir(el, &fv->v[i], at);
    e = ir_concat(el->design->arena, fv->n, parts, at);
    free(parts);
    return el_hw(el, t, e);
}

static Value ce_build_enum(Elab *el, Ty *t, uint32_t variant, ValList *payload, Span at)
{
    uint32_t i;
    int all_const = 1;
    Ty *pt = t->variants[variant].payload;
    if (payload)
        for (i = 0; i < payload->n; i++)
            if (!val_is_const(&payload->v[i]))
                all_const = 0;
    if (all_const)
        return val_enum(&el->arena, t, variant, payload);
    if (!el->design)
        return ce_need_design(el, at);
    {
        IrExpr *parts[3];
        uint32_t n = 0;
        Value pv = ce_build_struct(el, pt, payload, at);
        parts[n++] = ir_const_u64(el->design->arena, t->tag_width, t->variants[variant].tag, at);
        if (t->payload_width > pt->width)
            parts[n++] = ir_const_u64(el->design->arena, t->payload_width - pt->width, 0, at);
        parts[n++] = el_to_ir(el, &pv, at);
        return el_hw(el, t, ir_concat(el->design->arena, n, parts, at));
    }
}

static Value ce_struct_literal(Elab *el, Expr *e, Env *env, Ty *expect)
{
    Value base;
    Ty *t = NULL;
    ValList fv;
    int variant = -1;
    Path *p = &e->u.struct_lit.path;
    base = ce_eval_path(el, p, env, expect, 1);
    if (base.kind == VAL_ERROR)
        return base;
    if (base.kind == VAL_TYPE && base.aux) {
        t = base.u.type;
        variant = (int)base.aux - 1;
    } else if (base.kind == VAL_TYPE) {
        t = base.u.type;
    } else if (base.kind == VAL_ITEM && base.u.fn->item &&
               (base.u.fn->item->kind == ITEM_STRUCT || base.u.fn->item->kind == ITEM_BUNDLE ||
                base.u.fn->item->kind == ITEM_TYPE)) {
        Item *it = base.u.fn->item;
        if (expect && expect->def == it) {
            t = expect;
        } else {
            /* infer the generic arguments from the field values */
            t = NULL;
            if (!el_item_needs_args(it))
                t = el_type_item(el, it, base.u.fn->file, base.u.fn->ctx, NULL, 0, env, e->span, expect);
            if (t == NULL) {
                /* inference through the literal's fields */
                Env *genv;
                GenericParams *gp = it->kind == ITEM_STRUCT ? &it->u.struct_def.generics : &it->u.bundle.generics;
                Field **fields = it->kind == ITEM_STRUCT ? it->u.struct_def.fields : it->u.bundle.fields;
                uint32_t nf = it->kind == ITEM_STRUCT ? it->u.struct_def.nfields : it->u.bundle.nfields;
                uint32_t i, j;
                ElabArgs args;
                if (it->kind == ITEM_TYPE) {
                    el_error(el, e->span, "cannot infer the generic arguments of `%s`", it->name.name);
                    return val_error();
                }
                genv = elab_env_new(el, base.u.fn->ctx, base.u.fn->file);
                for (i = 0; i < gp->n; i++) {
                    Value ph = val_void(el->S);
                    ph.aux = 1;
                    elab_env_bind(el, genv, gp->items[i].name.name, ph, gp->items[i].name.span);
                }
                for (i = 0; i < e->u.struct_lit.nfields; i++) {
                    FieldInit *fi = &e->u.struct_lit.fields[i];
                    Value v;
                    for (j = 0; j < nf; j++)
                        if (fields[j]->name.name == fi->name.name)
                            break;
                    if (j == nf)
                        continue;
                    v = fi->value ? elab_eval(el, fi->value, env, NULL)
                                  : el_lookup_name(el, env, fi->name.name, fi->name.span, 1);
                    if (v.kind != VAL_ERROR && v.ty)
                        el_unify_type(el, fields[j]->type, v.ty, genv);
                }
                memset(&args, 0, sizeof args);
                args.n = gp->n;
                args.items = ANEWN(&el->arena, ElabArg, gp->n ? gp->n : 1);
                for (i = 0; i < gp->n; i++) {
                    Binding *b = ce_env_find_local(genv, gp->items[i].name.name);
                    if (!b || (b->val.kind == VAL_VOID && b->val.aux == 1)) {
                        if (gp->items[i].default_type || gp->items[i].default_value) {
                            args.n = i;
                            break;
                        }
                        el_error(el, e->span, "cannot infer generic `%s` of `%s` from this literal",
                                 gp->items[i].name.name, it->name.name);
                        elab_env_free(genv);
                        return val_error();
                    }
                    args.items[i].name = gp->items[i].name.name;
                    args.items[i].val = b->val;
                    args.items[i].span = e->span;
                }
                elab_env_free(genv);
                t = el_type_item_vals(el, it, base.u.fn->file, base.u.fn->ctx, &args, e->span);
            }
        }
    } else {
        el_error(el, p->span, "`%s` is not a struct, bundle or enum variant", el_span_text(p->span));
        return val_error();
    }
    if (!t || t->kind == TY_ERROR)
        return val_error();
    if (variant >= 0) {
        if (!ce_literal_fields(el, ce_payload_or_struct(t, (uint32_t)variant), e->u.struct_lit.fields,
                               e->u.struct_lit.nfields, e->u.struct_lit.has_rest, env, 0, &fv, e->span))
            return val_error();
        return ce_build_enum(el, t, (uint32_t)variant, &fv, e->span);
    }
    if (t->kind == TY_STRUCT) {
        if (!ce_literal_fields(el, t, e->u.struct_lit.fields, e->u.struct_lit.nfields, e->u.struct_lit.has_rest,
                               env, 0, &fv, e->span))
            return val_error();
        return ce_build_struct(el, t, &fv, e->span);
    }
    if (t->kind == TY_BUNDLE) {
        Value v;
        if (!ce_literal_fields(el, t, e->u.struct_lit.fields, e->u.struct_lit.nfields, e->u.struct_lit.has_rest,
                               env, 1, &fv, e->span))
            return val_error();
        v = val_list(&el->arena, VAL_BUNDLE, t, 0);
        *v.u.list = fv;
        return v;
    }
    el_error(el, p->span, "`%s` is not a struct, bundle or enum variant", ty_str(t));
    return val_error();
}

static Value ce_variant_literal(Elab *el, Expr *e, Env *env, Ty *expect)
{
    const char *name = e->u.variant.name.name;
    int k;
    if (!expect || expect->kind != TY_ENUM) {
        if (expect && expect->kind == TY_ERROR)
            return val_error();
        el_error(el, e->span, "cannot infer the enum type of `.%s` here", name ? name : "?");
        el_help(el, "write the enum name: `MyEnum::%s`", name ? name : "Variant");
        return val_error();
    }
    k = ty_variant_index(expect, name);
    if (k < 0) {
        el_error(el, e->u.variant.name.span, "enum `%s` has no variant `%s`", ty_str(expect), name);
        return val_error();
    }
    if (expect->variants[k].payload) {
        ValList fv;
        if (!e->u.variant.has_fields && expect->variants[k].payload->nfields) {
            el_error(el, e->span, "variant `%s` needs its fields: `.%s { .. }`", name, name);
            return val_error();
        }
        if (!ce_literal_fields(el, expect->variants[k].payload, e->u.variant.fields, e->u.variant.nfields, 0, env, 0,
                               &fv, e->span))
            return val_error();
        return ce_build_enum(el, expect, (uint32_t)k, &fv, e->span);
    }
    if (e->u.variant.has_fields && e->u.variant.nfields) {
        el_error(el, e->span, "variant `%s` has no fields", name);
        return val_error();
    }
    return val_enum(&el->arena, expect, (uint32_t)k, NULL);
}

static Value ce_array_literal(Elab *el, Expr *e, Env *env, Ty *expect)
{
    uint32_t i, n = e->u.list.n;
    Ty *et = expect && expect->kind == TY_ARRAY ? expect->elem : NULL;
    Value v;
    Ty *common = NULL;
    int ok = 1;
    if (expect && expect->kind == TY_ARRAY && expect->len != n) {
        el_error(el, e->span, "array literal has %u elements, %s has %u", (unsigned)n, ty_str(expect),
                 (unsigned)expect->len);
        return val_error();
    }
    v = val_list(&el->arena, VAL_ARRAY, NULL, n);
    for (i = 0; i < n; i++) {
        Value x = elab_eval(el, e->u.list.items[i], env, et);
        if (et && x.kind != VAL_ERROR)
            x = elab_convert(el, x, et, e->u.list.items[i], "the array element", e->u.list.items[i]->span);
        if (x.kind == VAL_ERROR)
            ok = 0;
        v.u.list->v[i] = x;
    }
    if (!ok)
        return val_error();
    if (et) {
        v.ty = expect;
    } else if (n) {
        /* element type: common type of the elements */
        for (i = 0; i < n; i++) {
            Ty *xt = v.u.list->v[i].ty;
            if (!xt) {
                common = NULL;
                break;
            }
            if (!common)
                common = xt;
            else if (common != xt) {
                if (common->kind == TY_INT && xt->kind == TY_INT)
                    common = common->is_signed ? common : xt;
                else if (ce_int_family(common) && ce_int_family(xt) && common->kind != TY_INT && xt->kind != TY_INT)
                    common = common->width >= xt->width ? common : xt;
                else if (common->kind == TY_INT && xt->kind != TY_INT)
                    common = xt;
                else if (!(xt->kind == TY_INT && common->kind != TY_INT)) {
                    common = NULL;
                    break;
                }
            }
        }
        if (common) {
            Ty *at = ty_array(el->S, common, n);
            if (common->kind != TY_INT) {
                for (i = 0; i < n; i++)
                    v.u.list->v[i] = elab_convert(el, v.u.list->v[i], common, e->u.list.items[i], NULL,
                                                  e->u.list.items[i]->span);
            }
            v.ty = at;
        }
    }
    if (v.ty && v.ty->hw)
        return ce_materialize_array(el, v, v.ty, e->span);
    return v;
}

/* ================================================================ calls */

static int ce_is_placeholder(const Binding *b)
{
    return b && b->val.kind == VAL_VOID && b->val.aux == 1;
}

static void ce_unify_ident(Elab *el, Expr *e, Value v, Env *genv)
{
    const char *n;
    Binding *b;
    Symbol sym;
    if (!e || e->kind != EXPR_PATH || e->u.path.nsegs != 1 || e->u.path.segs[0].has_args)
        return;
    n = e->u.path.segs[0].name.name;
    if (!n)
        return;
    b = ce_env_find_local(genv, n);
    if (b) {
        if (ce_is_placeholder(b))
            b->val = v;
        return;
    }
    if (elab_env_find(genv, n) || sema_lookup(el->S, genv->file, n, &sym))
        return;
    /* a free width/length name (`fn parity(x: bit<N>)`): bind it */
    elab_env_bind(el, genv, n, v, e->span)->sem = SEM_GENERIC;
}

void el_unify_type(Elab *el, Type *pt, Ty *at, Env *genv)
{
    uint32_t i;
    if (!pt || !at || at->kind == TY_ERROR)
        return;
    switch (pt->kind) {
    case TYPE_PATH: {
        PathSeg *s = &pt->u.path.segs[0];
        const char *n = s->name.name;
        if (pt->u.path.nsegs != 1 || !n)
            return;
        if (!s->has_args) {
            Binding *b = ce_env_find_local(genv, n);
            if (ce_is_placeholder(b))
                b->val = val_type(el->S, at);
            return;
        }
        if ((strcmp(n, "uint") == 0 || strcmp(n, "sint") == 0 || strcmp(n, "bit") == 0) && s->nargs == 1) {
            if (ty_is_sized_int(at) || (at->kind == TY_BOOL && strcmp(n, "bit") == 0))
                ce_unify_ident(el, s->args[0].expr, val_int(el->S->t_int, at->width), genv);
            return;
        }
        if (at->def && at->gvals && (at->kind == TY_STRUCT || at->kind == TY_BUNDLE || at->kind == TY_ENUM)) {
            Value base;
            int q = el->quiet;
            el->quiet = 1;
            base = el_lookup_name(el, genv, n, s->name.span, 0);
            el->quiet = q;
            if (base.kind == VAL_ITEM && base.u.fn->item == at->def) {
                Value *gv = (Value *)at->gvals;
                for (i = 0; i < s->nargs && i < at->ngvals; i++) {
                    if (gv[i].kind == VAL_TYPE && s->args[i].type)
                        el_unify_type(el, s->args[i].type, gv[i].u.type, genv);
                    else
                        ce_unify_ident(el, s->args[i].expr, gv[i], genv);
                }
            }
        }
        return;
    }
    case TYPE_ARRAY:
        if (at->kind == TY_ARRAY) {
            el_unify_type(el, pt->u.array.elem, at->elem, genv);
            ce_unify_ident(el, pt->u.array.len, val_int(el->S->t_int, at->len), genv);
        }
        return;
    case TYPE_TUPLE:
        if (at->kind == TY_TUPLE)
            for (i = 0; i < pt->u.tuple.n && i < at->nfields; i++)
                el_unify_type(el, pt->u.tuple.elems[i], at->fields[i].ty, genv);
        return;
    default:
        return;
    }
}

Value el_call_fn(Elab *el, ValFn *fn, const Value *args, const Ident *names, Expr *const *argexprs,
                 uint32_t nargs, Span at, Ty *expect)
{
    Item *it = fn->item;
    GenericParams *gp = &it->u.fn.generics;
    Param *ps = it->u.fn.params;
    uint32_t np = it->u.fn.nparams, i, j;
    Env *genv, *benv;
    Value *pv;
    Expr **pe;
    Value result = val_void(el->S);
    int hw = 0, ok = 1;
    struct BlkCtx *saved_blk;
    int saved_in_fn, saved_fn_active, saved_ret_have;
    Value saved_fn_path, saved_ret_cond, saved_ret_val;
    Ty *saved_ret_ty;
    char frame[256];
    TERN_UNUSED(expect);
    if (el->depth >= CE_MAX_DEPTH) {
        el_error(el, at, "calls nested too deeply (recursion?) in `%s`", it->name.name);
        return val_error();
    }
    genv = elab_env_new(el, fn->ctx, fn->file);
    genv->is_fn = 1;
    genv->mod = el->mod;
    if (fn->self)
        elab_env_bind(el, genv, intern_cstr("self"), *fn->self, at)->sem = SEM_PARAM;
    for (i = 0; i < gp->n; i++) {
        Value ph = val_void(el->S);
        int found = 0;
        ph.aux = 1;
        for (j = 0; j < fn->ngargs; j++)
            if (fn->gnames[j].name == gp->items[i].name.name) {
                ph = fn->gargs[j];
                found = 1;
            }
        TERN_UNUSED(found);
        elab_env_bind(el, genv, gp->items[i].name.name, ph, gp->items[i].name.span)->sem = SEM_GENERIC;
    }
    pv = (Value *)xcalloc(np ? np : 1, sizeof(Value));
    pe = (Expr **)xcalloc(np ? np : 1, sizeof(Expr *));
    for (i = 0; i < np; i++)
        pv[i].kind = VAL_ERROR, pv[i].aux = 0xFFFF;
    {
        uint32_t k = 0;
        for (i = 0; i < nargs; i++) {
            uint32_t slot = np;
            if (names && names[i].name) {
                for (j = 0; j < np; j++)
                    if (ps[j].name.name == names[i].name)
                        slot = j;
                if (slot == np) {
                    el_error(el, names[i].span, "`%s` has no parameter `%s`", it->name.name, names[i].name);
                    ok = 0;
                    continue;
                }
            } else {
                slot = k++;
            }
            if (slot >= np) {
                el_error(el, at, "too many arguments to `%s` (it takes %u)", it->name.name, (unsigned)np);
                ok = 0;
                break;
            }
            pv[slot] = args[i];
            pe[slot] = argexprs ? argexprs[i] : NULL;
        }
    }
    for (i = 0; ok && i < np; i++)
        if (pv[i].kind == VAL_ERROR && pv[i].aux == 0xFFFF) {
            el_error(el, at, "missing argument `%s` in call to `%s`", ps[i].name.name, it->name.name);
            ok = 0;
        }
    for (i = 0; ok && i < np; i++) {
        if (pv[i].kind == VAL_ERROR) {
            ok = 0;
            break;
        }
        if (pv[i].ty && pv[i].ty->kind != TY_INT)
            el_unify_type(el, ps[i].type, pv[i].ty, genv);
        if (!val_is_const(&pv[i]))
            hw = 1;
    }
    /* defaults of generics not given or inferred */
    for (i = 0; ok && i < gp->n; i++) {
        Binding *b = ce_env_find_local(genv, gp->items[i].name.name);
        if (!ce_is_placeholder(b))
            continue;
        if (gp->items[i].kind == GENERIC_TYPE && gp->items[i].default_type) {
            Ty *t = elab_eval_type(el, gp->items[i].default_type, genv);
            b = ce_env_find_local(genv, gp->items[i].name.name);
            b->val = val_type(el->S, t);
        } else if (gp->items[i].kind == GENERIC_VALUE && gp->items[i].default_value) {
            Ty *t = gp->items[i].type ? elab_eval_type(el, gp->items[i].type, genv) : NULL;
            Value v = elab_eval_const(el, gp->items[i].default_value, genv, t, "a generic default");
            if (t && v.kind != VAL_ERROR)
                v = elab_convert(el, v, t, gp->items[i].default_value, NULL, gp->items[i].default_value->span);
            b = ce_env_find_local(genv, gp->items[i].name.name);
            b->val = v;
        } else {
            el_error(el, at, "cannot infer generic `%s` of `%s`", gp->items[i].name.name, it->name.name);
            el_help(el, "give it explicitly: `%s<..>(..)`", it->name.name);
            ok = 0;
        }
    }
    if (ok && it->u.fn.where.n)
        ok = el_check_where(el, &it->u.fn.where, genv, it->name.name, at);
    benv = elab_env_new(el, genv, NULL);
    for (i = 0; ok && i < np; i++) {
        Ty *pt = elab_eval_type(el, ps[i].type, genv);
        Value a;
        Binding *b;
        if (pt->kind == TY_ERROR) {
            ok = 0;
            break;
        }
        a = elab_convert(el, pv[i], pt, pe[i], ce_what("parameter `", ps[i].name.name),
                         pe[i] ? pe[i]->span : at);
        if (a.kind == VAL_ERROR) {
            ok = 0;
            break;
        }
        b = elab_env_bind(el, benv, ps[i].name.name, el_maybe_temp(el, val_copy_deep(&el->arena, &a), at),
                          ps[i].name.span);
        b->is_mut = 1;
        b->sem = SEM_PARAM;
    }
    free(pv);
    free(pe);
    if (!ok) {
        elab_env_free(benv);
        elab_env_free(genv);
        return val_error();
    }
    if (hw) {
        for (i = 0; i < el->hw_calls.len; i++)
            if (el->hw_calls.data[i] == it) {
                el_error(el, at, "recursive call of `%s` on signals: recursion only works at compile time",
                         it->name.name);
                elab_env_free(benv);
                elab_env_free(genv);
                return val_error();
            }
        vec_push(el->hw_calls, it);
    }
    snprintf(frame, sizeof frame, "%s()", it->name.name);
    saved_blk = el->blk;
    saved_in_fn = el->in_fn;
    saved_fn_active = el->fn_active;
    saved_fn_path = el->fn_path;
    saved_ret_have = el->fn_ret_have;
    saved_ret_cond = el->fn_ret_cond;
    saved_ret_val = el->fn_ret_val;
    saved_ret_ty = el->fn_ret_ty;
    el->blk = NULL;
    el->in_fn = 1;
    el->fn_active = 1;
    el->fn_path = el_mk_bool(el, 1);
    el->fn_ret_have = 0;
    el->fn_ret_ty = NULL;
    el->depth++;
    if (it->u.fn.body) {
        Value tail = val_void(el->S);
        if (it->u.fn.ret) {
            int q = el->quiet;
            Ty *rt;
            el->quiet = 1;
            rt = elab_eval_type(el, it->u.fn.ret, genv);
            el->quiet = q;
            el->quiet_errors = 0;
            el->tail_expect = rt && rt->kind != TY_ERROR ? rt : NULL;
            el->fn_ret_ty = el->tail_expect;
        }
        el_exec_stmts(el, it->u.fn.body->stmts, it->u.fn.body->nstmts, benv, it->u.fn.body->tail, &tail);
        if (el->returned) {
            result = el->ret;
            el->returned = 0;
        } else if (el->fn_ret_have && el->fn_ret_val.kind != VAL_ERROR) {
            /* some paths returned early: they take priority over the tail */
            if (tail.kind == VAL_VOID || tail.kind == VAL_ERROR) {
                if (tail.kind == VAL_VOID && it->u.fn.ret)
                    el_error(el, it->u.fn.body->span, "`%s` does not return a value on every path", it->name.name);
                result = val_error();
            } else {
                result = el_unify_mux(el, el->fn_ret_cond, el->fn_ret_val, tail, el->fn_ret_ty, at);
            }
        } else if (el->fn_ret_have) {
            result = val_error();
        } else {
            result = tail;
        }
    }
    el->fn_active = saved_fn_active;
    el->fn_path = saved_fn_path;
    el->fn_ret_have = saved_ret_have;
    el->fn_ret_cond = saved_ret_cond;
    el->fn_ret_val = saved_ret_val;
    el->fn_ret_ty = saved_ret_ty;
    el->depth--;
    el->blk = saved_blk;
    el->in_fn = saved_in_fn;
    if (hw)
        el->hw_calls.len--;
    if (it->u.fn.ret && result.kind != VAL_ERROR && !el->limit_hit) {
        Ty *rt = elab_eval_type(el, it->u.fn.ret, genv);
        if (rt->kind != TY_ERROR) {
            if (result.kind == VAL_VOID) {
                el_error(el, it->u.fn.body ? it->u.fn.body->span : at, "`%s` must return a %s value",
                         it->name.name, ty_str(rt));
                result = val_error();
            } else {
                result = elab_convert(el, result, rt, it->u.fn.body ? it->u.fn.body->tail : NULL,
                                      arena_printf(&el->arena, "the return type of `%s`", it->name.name),
                                      it->u.fn.body && it->u.fn.body->tail ? it->u.fn.body->tail->span : at);
            }
        } else {
            result = val_error();
        }
    }
    if (el->limit_hit)
        result = val_error();
    elab_env_free(benv);
    if (result.kind != VAL_FN)
        elab_env_free(genv);
    return result;
}

/* ------------------------------------------------------------- builtins */

static void ce_format(Elab *el, const Value *args, uint32_t nargs, StrBuf *out, Span at)
{
    uint32_t k = 1;
    const char *s;
    uint32_t i, n;
    if (nargs == 0 || args[0].kind != VAL_STRING) {
        el_error(el, at, "the first argument must be a format string");
        return;
    }
    s = args[0].u.s;
    n = args[0].aux;
    for (i = 0; i < n; i++) {
        if (s[i] == '{' && i + 1 < n && s[i + 1] == '}') {
            if (k < nargs) {
                if (args[k].kind == VAL_STRING)
                    sb_append(out, args[k].u.s, args[k].aux);
                else
                    val_print(&args[k], out);
            } else {
                sb_puts(out, "{}");
            }
            k++;
            i++;
        } else {
            sb_putc(out, s[i]);
        }
    }
}

static int ce_hexval(int c)
{
    return tern_hex_value(c);
}

/* include_hex / include_bin: one word per whitespace-separated token,
 * `//` comments ignored; `@addr` sets the next address ($readmemh style) */
static Value ce_include_file(Elab *el, const Value *path, Ty *expect, int is_bin, Span at)
{
    char *text = NULL, *full;
    size_t len = 0, i = 0;
    uint32_t addr = 0;
    Value v;
    const SourceFile *sf = src_get(at.file);
    char *dir;
    if (path->kind != VAL_STRING) {
        el_error(el, at, "%s needs a file name", is_bin ? "include_bin" : "include_hex");
        return val_error();
    }
    if (!expect || expect->kind != TY_ARRAY || !ty_is_sized_int(expect->elem)) {
        el_error(el, at, "%s initializes a memory or array of integers", is_bin ? "include_bin" : "include_hex");
        return val_error();
    }
    dir = os_path_dirname(sf ? sf->path : ".");
    full = os_path_join(dir, path->u.s);
    free(dir);
    if (os_read_file(full, &text, &len) != 0) {
        el_error(el, at, "cannot read `%s`", full);
        free(full);
        return val_error();
    }
    free(full);
    v = el_zero(el, expect);
    while (i < len) {
        BigInt b = {0};
        size_t st;
        if (tern_is_space((unsigned char)text[i]) || text[i] == '_') {
            i++;
            continue;
        }
        if (text[i] == '/' && i + 1 < len && text[i + 1] == '/') {
            while (i < len && text[i] != '\n')
                i++;
            continue;
        }
        if (text[i] == '@') {
            uint64_t a = 0;
            i++;
            while (i < len && ce_hexval((unsigned char)text[i]) >= 0)
                a = a * 16 + (uint64_t)ce_hexval((unsigned char)text[i++]);
            addr = (uint32_t)a;
            continue;
        }
        st = i;
        while (i < len && !tern_is_space((unsigned char)text[i]))
            i++;
        if (!bn_parse(&b, text + st, i - st, is_bin ? 2 : 16)) {
            el_error(el, at, "invalid %s word `%.*s`", is_bin ? "binary" : "hex", (int)(i - st), text + st);
            bn_free(&b);
            break;
        }
        if (addr < expect->len)
            v.u.list->v[addr] = ce_int_norm(el, expect->elem, &b);
        addr++;
        bn_free(&b);
    }
    free(text);
    return v;
}

static Value ce_builtin(Elab *el, const char *name, Expr *call, Env *env, Ty *expect, int *handled)
{
    Arg *args = call->u.call.args;
    uint32_t n = call->u.call.nargs;
    Span at = call->span;
    *handled = 1;
    if (strcmp(name, "bits") == 0) {
        Value v;
        if (n != 1) {
            el_error(el, at, "`bits` takes one argument");
            return val_error();
        }
        v = elab_eval(el, args[0].value, env, NULL);
        if (v.kind == VAL_ERROR)
            return v;
        if (!v.ty || !v.ty->hw) {
            el_error(el, at, "`bits` needs a value with a width, found %s",
                     v.ty ? ty_str(v.ty) : val_kind_name((ValKind)v.kind));
            return val_error();
        }
        if (val_is_const(&v)) {
            BigInt b = {0};
            Value r;
            el_bits_of_const(el, &v, v.ty, &b);
            r = val_int_bn(&el->arena, ty_bits(el->S, v.ty->width), &b);
            bn_free(&b);
            return r;
        }
        if (v.ty->kind == TY_BIT)
            return v;
        return el_hw(el, ty_bits(el->S, v.ty->width), v.u.e);
    }
    if (strcmp(name, "static_assert") == 0) {
        Value c, m = val_void(el->S);
        if (n < 1) {
            el_error(el, at, "static_assert needs a condition");
            return val_error();
        }
        c = elab_eval_const(el, args[0].value, env, el->S->t_bool, "a static_assert condition");
        if (n > 1)
            m = elab_eval_const(el, args[1].value, env, NULL, "a message");
        if (c.kind == VAL_BOOL && !c.u.i) {
            if (m.kind == VAL_STRING)
                el_error(el, at, "static assertion failed: %.*s", (int)m.aux, m.u.s);
            else
                el_error(el, at, "static assertion failed: `%s`", el_text(args[0].value));
        } else if (c.kind != VAL_BOOL && c.kind != VAL_ERROR) {
            el_error(el, args[0].value->span, "static_assert needs a bool condition");
        }
        return val_void(el->S);
    }
    if (strcmp(name, "const_log") == 0 || (strcmp(name, "log") == 0 && !el->hooks.call)) {
        Value *vals = (Value *)xcalloc(n ? n : 1, sizeof(Value));
        StrBuf sb = {0};
        uint32_t i;
        for (i = 0; i < n; i++)
            vals[i] = elab_eval(el, args[i].value, env, NULL);
        ce_format(el, vals, n, &sb, at);
        if (el->hooks.log)
            el->hooks.log(el, at, sb_cstr(&sb));
        else if (!el->quiet)
            diag_note(at, "const_log: %s", sb_cstr(&sb));
        sb_free(&sb);
        free(vals);
        return val_void(el->S);
    }
    if (strcmp(name, "include_hex") == 0 || strcmp(name, "include_bin") == 0) {
        Value p;
        if (n != 1) {
            el_error(el, at, "`%s` takes a file name", name);
            return val_error();
        }
        p = elab_eval_const(el, args[0].value, env, NULL, "a file name");
        return ce_include_file(el, &p, expect, name[8] == 'b', at);
    }
    if (strcmp(name, "unsafe_cross") == 0) {
        Value v;
        if (n != 1) {
            el_error(el, at, "`unsafe_cross` takes one argument");
            return val_error();
        }
        v = elab_eval(el, args[0].value, env, expect);
        return el_unsafe_cross(el, v, at);
    }
    if (strcmp(name, "stable") == 0 || strcmp(name, "rose") == 0 || strcmp(name, "fell") == 0 ||
        strcmp(name, "past") == 0)
        return el_temporal_call(el, name, call, env);
    *handled = 0;
    return val_error();
}

static Value ce_from_bits(Elab *el, Ty *t, Expr *call, Env *env)
{
    Value b;
    Span at = call->span;
    if (call->u.call.nargs != 1) {
        el_error(el, at, "`from_bits` takes one argument");
        return val_error();
    }
    if (!t->hw) {
        el_error(el, at, "`%s` has no bit representation", ty_str(t));
        return val_error();
    }
    b = elab_eval(el, call->u.call.args[0].value, env, ty_bits(el->S, t->width));
    if (b.kind == VAL_ERROR)
        return b;
    if (ce_is_widthless(&b))
        b = elab_convert(el, b, ty_bits(el->S, t->width), call->u.call.args[0].value, NULL, at);
    if (b.kind == VAL_ERROR)
        return b;
    if (!b.ty || !b.ty->hw || b.ty->width != t->width) {
        el_error(el, at, "`%s::from_bits` needs %u bits, found %s", ty_str(t), (unsigned)t->width,
                 b.ty ? ty_str(b.ty) : val_kind_name((ValKind)b.kind));
        return val_error();
    }
    if (val_is_const(&b)) {
        IrBits bits = elab_const_bits(el, &b, b.ty);
        return elab_value_from_bits(el, &bits, t);
    }
    if (b.ty->kind == t->kind && t->kind != TY_STRUCT && t->kind != TY_ENUM && t->kind != TY_ARRAY)
        return el_hw(el, t, b.u.e);
    return el_hw(el, t, ir_resize(el->design->arena, b.u.e, t->width, 0, at));
}

static Value ce_eval_args(Elab *el, Expr *call, Env *env, Value **vals, Ident **names, Expr ***exprs)
{
    uint32_t i, n = call->u.call.nargs;
    *vals = ANEWN(&el->arena, Value, n ? n : 1);
    *names = ANEWN(&el->arena, Ident, n ? n : 1);
    *exprs = ANEWN(&el->arena, Expr *, n ? n : 1);
    for (i = 0; i < n; i++) {
        (*vals)[i] = elab_eval(el, call->u.call.args[i].value, env, NULL);
        (*names)[i] = call->u.call.args[i].name;
        (*exprs)[i] = call->u.call.args[i].value;
        if ((*vals)[i].kind == VAL_ERROR)
            return val_error();
    }
    return val_void(el->S);
}

/* evaluates arguments with the parameter types of `fn` as hints where they are
 * not generic (so `.Variant`, all0 and literals get typed) */
static void ce_eval_fn_args(Elab *el, ValFn *fn, Expr *call, Env *env, Value *vals)
{
    Item *it = fn->item;
    uint32_t i, n = call->u.call.nargs, k = 0;
    for (i = 0; i < n; i++) {
        Arg *a = &call->u.call.args[i];
        Param *p = NULL;
        Ty *hint = NULL;
        uint32_t j;
        if (a->name.name) {
            for (j = 0; j < it->u.fn.nparams; j++)
                if (it->u.fn.params[j].name.name == a->name.name)
                    p = &it->u.fn.params[j];
        } else if (k < it->u.fn.nparams) {
            p = &it->u.fn.params[k++];
        }
        if (p && (a->value->kind == EXPR_VARIANT || a->value->kind == EXPR_FILL ||
                  a->value->kind == EXPR_ARRAY || a->value->kind == EXPR_STRUCT)) {
            /* typed in the fn's generic context when it does not depend on inference */
            int q = el->quiet;
            Env *genv = elab_env_new(el, fn->ctx, fn->file);
            uint32_t g;
            for (g = 0; g < it->u.fn.generics.n; g++) {
                Value ph = val_void(el->S);
                ph.aux = 1;
                for (j = 0; j < fn->ngargs; j++)
                    if (fn->gnames[j].name == it->u.fn.generics.items[g].name.name)
                        ph = fn->gargs[j];
                elab_env_bind(el, genv, it->u.fn.generics.items[g].name.name, ph, a->span);
            }
            el->quiet = 1;
            hint = elab_eval_type(el, p->type, genv);
            el->quiet = q;
            el->quiet_errors = 0;
            elab_env_free(genv);
            if (hint && hint->kind == TY_ERROR)
                hint = NULL;
        }
        vals[i] = elab_eval(el, a->value, env, hint);
    }
}

Value el_call_value(Elab *el, Value fnv, Expr *call, Env *env, Ty *expect)
{
    uint32_t i, n = call->u.call.nargs;
    Value *vals;
    Ident *names;
    Expr **exprs;
    if (fnv.kind == VAL_ERROR)
        return fnv;
    if (fnv.kind == VAL_FN && fnv.u.fn->builtin) {
        if (fnv.u.fn->bty)
            return ce_from_bits(el, fnv.u.fn->bty, call, env);
        return val_error();
    }
    if (fnv.kind == VAL_FN) {
        vals = ANEWN(&el->arena, Value, n ? n : 1);
        names = ANEWN(&el->arena, Ident, n ? n : 1);
        exprs = ANEWN(&el->arena, Expr *, n ? n : 1);
        ce_eval_fn_args(el, fnv.u.fn, call, env, vals);
        for (i = 0; i < n; i++) {
            names[i] = call->u.call.args[i].name;
            exprs[i] = call->u.call.args[i].value;
            if (vals[i].kind == VAL_ERROR)
                return val_error();
        }
        return el_call_fn(el, fnv.u.fn, vals, names, exprs, n, call->span, expect);
    }
    if (fnv.kind == VAL_ITEM && (fnv.u.fn->item->kind == ITEM_MODULE || fnv.u.fn->item->kind == ITEM_EXTERN))
        return el_instance(el, call, env, NULL, call->span, fnv);
    if (fnv.kind == VAL_MODULE)
        return el_instance(el, call, env, NULL, call->span, fnv);
    if (fnv.kind == VAL_TYPE) {
        el_error(el, call->span, "`%s` is a type, not a function", ty_str(fnv.u.type));
        el_help(el, "build a value with a literal `%s { .. }`, or convert with `as`", ty_str(fnv.u.type));
        return val_error();
    }
    TERN_UNUSED(ce_eval_args);
    el_error(el, call->span, "a %s is not callable", val_kind_name((ValKind)fnv.kind));
    return val_error();
}

/* method call x.name(args) */
static Value ce_method(Elab *el, Expr *call, Env *env, Ty *expect)
{
    Expr *callee = call->u.call.callee;
    Expr *be = callee->u.field.base;
    const char *name = callee->u.field.name.name;
    Value base;
    Span at = call->span;
    uint32_t i, n = call->u.call.nargs;
    if (!name)
        return val_error();
    base = elab_eval(el, be, env, NULL);
    if (base.kind == VAL_ERROR)
        return base;
    if (base.kind == VAL_MEM)
        return el_mem_method(el, base, name, call, env);
    if (base.kind == VAL_OPAQUE) {
        Value *vals = ANEWN(&el->arena, Value, n ? n : 1);
        Ident *names = ANEWN(&el->arena, Ident, n ? n : 1);
        Value out = val_void(el->S);
        int r;
        for (i = 0; i < n; i++) {
            vals[i] = elab_eval(el, call->u.call.args[i].value, env, NULL);
            names[i] = call->u.call.args[i].name;
            if (vals[i].kind == VAL_ERROR)
                return val_error();
        }
        r = base.u.opq->ops->call_method
                ? base.u.opq->ops->call_method(el, base.u.opq->obj, name, vals, names, n, at, &out)
                : 0;
        if (r == 0)
            el_error(el, callee->u.field.name.span, "%s has no method `%s`", base.u.opq->ops->type_name, name);
        return r > 0 ? out : val_error();
    }
    if (base.ty && base.ty->kind == TY_BUNDLE && base.ty->def && base.ty->def->kind == ITEM_BUNDLE) {
        Item *bi = base.ty->def;
        for (i = 0; i < bi->u.bundle.nfns; i++) {
            Item *f = bi->u.bundle.fns[i];
            if (f->name.name == name) {
                Value fv;
                memset(&fv, 0, sizeof fv);
                fv.kind = VAL_FN;
                fv.u.fn = ANEW(&el->arena, ValFn);
                fv.u.fn->item = f;
                fv.u.fn->file = base.ty->file;
                fv.u.fn->ctx = (Env *)base.ty->genv;
                fv.u.fn->self = ANEW(&el->arena, Value);
                *fv.u.fn->self = base;
                fv.u.fn->self_ty = base.ty;
                if (callee->u.field.has_args) {
                    PathSeg seg;
                    memset(&seg, 0, sizeof seg);
                    seg.has_args = 1;
                    seg.args = callee->u.field.args;
                    seg.nargs = callee->u.field.nargs;
                    seg.span = callee->span;
                    fv = el_apply_generics(el, fv, &seg, env, NULL);
                }
                return el_call_value(el, fv, call, env, expect);
            }
        }
        el_error(el, callee->u.field.name.span, "bundle `%s` has no method `%s`", ty_str(base.ty), name);
        return val_error();
    }
    if (strcmp(name, "is_pow2") == 0 && n == 0) {
        BigInt b = {0}, m = {0};
        int r;
        if (base.kind != VAL_INT) {
            el_error(el, at, "`is_pow2()` needs a compile-time integer");
            return val_error();
        }
        val_get_bn(&base, &b);
        bn_add_i64(&m, &b, -1);
        bn_and(&m, &m, &b);
        r = bn_sign(&b) > 0 && bn_is_zero(&m);
        bn_free(&b);
        bn_free(&m);
        return el_mk_bool(el, r);
    }
    if (strcmp(name, "trunc") == 0 && callee->u.field.has_args && callee->u.field.nargs == 1 && n == 0) {
        uint32_t w = ce_width_arg(el, callee->u.field.args[0].expr, env);
        if (!w || !base.ty)
            return val_error();
        return el_cast(el, base, ty_scalar(el->S, base.ty->kind == TY_SINT ? TY_SINT : TY_UINT, w), at, be);
    }
    if ((base.kind == VAL_ARRAY || (base.ty && base.ty->kind == TY_ARRAY)) &&
        (strcmp(name, "map") == 0 || strcmp(name, "reduce") == 0 || strcmp(name, "zip") == 0)) {
        Ty *at_ = base.ty;
        uint32_t len = base.kind == VAL_ARRAY ? base.u.list->n : at_->len, k;
        Value fnv, r;
        if (n != 1) {
            el_error(el, at, "`%s` takes one argument", name);
            return val_error();
        }
        fnv = elab_eval(el, call->u.call.args[0].value, env, NULL);
        if (fnv.kind == VAL_ERROR)
            return fnv;
        if (strcmp(name, "zip") == 0) {
            Value other = fnv;
            uint32_t ol = other.kind == VAL_ARRAY ? other.u.list->n : other.ty && other.ty->kind == TY_ARRAY ? other.ty->len : 0;
            if (ol != len) {
                el_error(el, at, "`zip` needs arrays of the same length (%u and %u)", (unsigned)len, (unsigned)ol);
                return val_error();
            }
            r = val_list(&el->arena, VAL_ARRAY, NULL, len);
            for (k = 0; k < len; k++) {
                Value pair = val_list(&el->arena, VAL_TUPLE, NULL, 2);
                Value iv = val_int(el->S->t_int, k);
                pair.u.list->v[0] = el_index(el, base, iv, at, NULL);
                pair.u.list->v[1] = el_index(el, other, iv, at, NULL);
                {
                    Ty *ts[2];
                    ts[0] = pair.u.list->v[0].ty ? pair.u.list->v[0].ty : el->S->t_error;
                    ts[1] = pair.u.list->v[1].ty ? pair.u.list->v[1].ty : el->S->t_error;
                    pair.ty = ty_tuple(el->S, 2, ts);
                }
                r.u.list->v[k] = pair;
            }
            return r;
        }
        if (fnv.kind != VAL_FN) {
            el_error(el, at, "`%s` needs a function", name);
            return val_error();
        }
        if (strcmp(name, "map") == 0) {
            Ty *et = NULL;
            r = val_list(&el->arena, VAL_ARRAY, NULL, len);
            for (k = 0; k < len; k++) {
                Value e = el_index(el, base, val_int(el->S->t_int, k), at, NULL);
                r.u.list->v[k] = el_call_fn(el, fnv.u.fn, &e, NULL, NULL, 1, at, NULL);
                if (r.u.list->v[k].kind == VAL_ERROR)
                    return val_error();
                et = r.u.list->v[k].ty;
            }
            if (et && len)
                r.ty = ty_array(el->S, et, len);
            if (r.ty && r.ty->hw)
                return ce_materialize_array(el, r, r.ty, at);
            return r;
        }
        /* reduce */
        if (len == 0) {
            el_error(el, at, "`reduce` of an empty array");
            return val_error();
        }
        r = el_index(el, base, val_int(el->S->t_int, 0), at, NULL);
        for (k = 1; k < len && r.kind != VAL_ERROR; k++) {
            Value two[2];
            two[0] = r;
            two[1] = el_index(el, base, val_int(el->S->t_int, k), at, NULL);
            r = el_call_fn(el, fnv.u.fn, two, NULL, NULL, 2, at, NULL);
        }
        return r;
    }
    el_error(el, callee->u.field.name.span, "no method `%s` on %s", name,
             base.ty ? ty_str(base.ty) : val_kind_name((ValKind)base.kind));
    return val_error();
}

static Value ce_call(Elab *el, Expr *e, Env *env, Ty *expect)
{
    Expr *callee = e->u.call.callee;
    Value fnv;
    if (callee->kind == EXPR_FIELD)
        return ce_method(el, e, env, expect);
    if (callee->kind == EXPR_PATH && callee->u.path.nsegs == 1) {
        const char *name = callee->u.path.segs[0].name.name;
        Symbol sym;
        if (!name)
            return val_error();
        if (!elab_env_find(env, name) && !sema_lookup(el->S, env ? env->file : NULL, name, &sym)) {
            int handled = 0;
            Value r = ce_builtin(el, name, e, env, expect, &handled);
            if (handled)
                return r;
            if (el->hooks.call) {
                uint32_t i, n = e->u.call.nargs;
                Value *vals = ANEWN(&el->arena, Value, n ? n : 1);
                Ident *names = ANEWN(&el->arena, Ident, n ? n : 1);
                Value out = val_void(el->S);
                for (i = 0; i < n; i++) {
                    vals[i] = elab_eval(el, e->u.call.args[i].value, env, NULL);
                    names[i] = e->u.call.args[i].name;
                }
                if (el->hooks.call(el, e, name, vals, names, n, env, &out))
                    return out;
            }
        }
    }
    if (callee->kind != EXPR_PATH) {
        el_error(el, callee->span, "this expression cannot be called");
        return val_error();
    }
    fnv = ce_eval_path(el, &callee->u.path, env, NULL, 0);
    return el_call_value(el, fnv, e, env, expect);
}

/* ============================================================== patterns */

static int ce_pat_irrefutable(Pattern *p)
{
    uint32_t i;
    switch (p->kind) {
    case PAT_WILDCARD: case PAT_BIND: case PAT_ERROR: return 1;
    case PAT_TUPLE:
        for (i = 0; i < p->u.list.n; i++)
            if (!ce_pat_irrefutable(p->u.list.items[i]))
                return 0;
        return 1;
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            if (ce_pat_irrefutable(p->u.list.items[i]))
                return 1;
        return 0;
    default: return 0;
    }
}

/* PAT_BIND naming a compile-time constant in scope compares against it */
static int ce_bind_is_const(Elab *el, Pattern *p, Env *env, Value *out)
{
    Binding *b = elab_env_find(env, p->u.bind.name);
    Symbol sym;
    if (b) {
        if (b->sem == SEM_CONST || b->sem == SEM_GENERIC) {
            Value v = b->lazy ? el_force(el, b) : b->val;
            if (val_is_const(&v) && v.kind != VAL_TYPE) {
                *out = v;
                return 1;
            }
        }
        return 0;
    }
    if (sema_lookup(el->S, env->file, p->u.bind.name, &sym) && sym.kind == SYM_ITEM &&
        sym.item->kind == ITEM_CONST) {
        *out = elab_symbol_value(el, &sym, p->span);
        return out->kind != VAL_ERROR;
    }
    return 0;
}

static Value ce_payload_value(Elab *el, Value subj, uint32_t variant, Span at)
{
    /* the payload struct of variant `variant` of an enum value */
    Ty *t = subj.ty;
    Ty *pt = t->variants[variant].payload;
    if (subj.kind == VAL_ENUM) {
        Value v = val_list(&el->arena, VAL_STRUCT, pt, 0);
        if (subj.u.list)
            *v.u.list = *subj.u.list;
        else
            v = el_zero(el, pt);
        return v;
    }
    return el_hw(el, pt, ir_slice(el->design->arena, subj.u.e, 0, pt->width, at));
}

void el_bind_pattern(Elab *el, Pattern *p, Value v, Env *env, int is_mut)
{
    uint32_t i;
    switch (p->kind) {
    case PAT_BIND: {
        Binding *b;
        Value cv;
        if (ce_bind_is_const(el, p, env, &cv))
            return;
        b = elab_env_bind(el, env, p->u.bind.name, el_maybe_temp(el, is_mut ? val_copy_deep(&el->arena, &v) : v,
                                                                   p->u.bind.span), p->u.bind.span);
        b->is_mut = (uint8_t)is_mut;
        b->sem = SEM_LOCAL;
        el_record_decl(el, p->u.bind.span, p->u.bind.name, SEM_LOCAL, v.ty, NULL, NULL);
        return;
    }
    case PAT_TUPLE:
        if (v.kind != VAL_TUPLE || v.u.list->n != p->u.list.n) {
            if (v.kind != VAL_ERROR)
                el_error(el, p->span, "tuple pattern with %u elements does not match %s", (unsigned)p->u.list.n,
                         v.ty ? ty_str(v.ty) : val_kind_name((ValKind)v.kind));
            for (i = 0; i < p->u.list.n; i++)
                el_bind_pattern(el, p->u.list.items[i], val_error(), env, is_mut);
            return;
        }
        for (i = 0; i < p->u.list.n; i++)
            el_bind_pattern(el, p->u.list.items[i], v.u.list->v[i], env, is_mut);
        return;
    case PAT_STRUCT: {
        /* struct / bundle / instance destructuring, or enum payload */
        uint32_t k;
        for (k = 0; k < p->u.struct_pat.nfields; k++) {
            FieldPat *fp = &p->u.struct_pat.fields[k];
            Value fv = v.kind == VAL_ERROR ? v : el_field(el, v, fp->name.name, fp->name.span);
            if (fp->pat)
                el_bind_pattern(el, fp->pat, fv, env, is_mut);
            else {
                Binding *b = elab_env_bind(el, env, fp->name.name, fv, fp->name.span);
                b->is_mut = (uint8_t)is_mut;
                b->sem = SEM_LOCAL;
            }
        }
        return;
    }
    case PAT_VARIANT: {
        uint32_t k;
        int vi;
        Value pv;
        if (v.kind == VAL_ERROR || !v.ty || v.ty->kind != TY_ENUM)
            return;
        vi = ty_variant_index(v.ty, p->u.variant.name.name);
        if (vi < 0 || !v.ty->variants[vi].payload)
            return;
        pv = ce_payload_value(el, v, (uint32_t)vi, p->span);
        for (k = 0; k < p->u.variant.nfields; k++) {
            FieldPat *fp = &p->u.variant.fields[k];
            Value fv = el_field(el, pv, fp->name.name, fp->name.span);
            if (fp->pat)
                el_bind_pattern(el, fp->pat, fv, env, is_mut);
            else {
                Binding *b = elab_env_bind(el, env, fp->name.name, fv, fp->name.span);
                b->is_mut = (uint8_t)is_mut;
                b->sem = SEM_LOCAL;
                el_record_decl(el, fp->name.span, fp->name.name, SEM_LOCAL, fv.ty, NULL, NULL);
            }
        }
        return;
    }
    case PAT_OR:
        /* every alternative binds the same names; for a hardware subject the
         * value of a name is selected by which alternative matched */
        if (p->u.list.n > 1 && v.kind == VAL_HW && el->design) {
            uint32_t n = p->u.list.n, k;
            Env **tmps = (Env **)xcalloc(n, sizeof(Env *));
            Value *conds = (Value *)xcalloc(n, sizeof(Value));
            for (i = 0; i < n; i++) {
                tmps[i] = elab_env_new(el, NULL, env->file);
                el_bind_pattern(el, p->u.list.items[i], v, tmps[i], is_mut);
                conds[i] = el_match_test(el, p->u.list.items[i], v, env);
            }
            for (k = 0; k < tmps[0]->n; k++) {
                const char *nm = tmps[0]->b[k].name;
                Binding *last = ce_env_find_local(tmps[n - 1], nm);
                Value r = last ? last->val : tmps[0]->b[k].val;
                Binding *nb;
                uint32_t j;
                for (j = n - 1; j-- > 0;) {
                    Binding *bj = ce_env_find_local(tmps[j], nm);
                    if (bj)
                        r = el_unify_mux(el, conds[j], bj->val, r, bj->val.ty, p->span);
                }
                nb = elab_env_bind(el, env, nm, r, tmps[0]->b[k].def);
                nb->is_mut = (uint8_t)is_mut;
                nb->sem = SEM_LOCAL;
            }
            for (i = 0; i < n; i++)
                elab_env_free(tmps[i]);
            free(tmps);
            free(conds);
            return;
        }
        if (p->u.list.n) {
            Pattern *first = p->u.list.items[0];
            if (v.ty && v.ty->kind == TY_ENUM && v.kind == VAL_ENUM) {
                for (i = 0; i < p->u.list.n; i++) {
                    Pattern *q = p->u.list.items[i];
                    if (q->kind == PAT_VARIANT && q->u.variant.name.name == v.ty->variants[v.aux].name) {
                        el_bind_pattern(el, q, v, env, is_mut);
                        return;
                    }
                }
            }
            el_bind_pattern(el, first, v, env, is_mut);
        }
        return;
    default:
        return;
    }
}

Value el_match_test(Elab *el, Pattern *p, Value subj, Env *env)
{
    Sema *S = el->S;
    uint32_t i;
    if (subj.kind == VAL_ERROR)
        return val_error();
    switch (p->kind) {
    case PAT_ERROR: return val_error();
    case PAT_WILDCARD: return el_mk_bool(el, 1);
    case PAT_BIND: {
        Value cv;
        if (ce_bind_is_const(el, p, env, &cv))
            return el_binary(el, BIN_EQ, subj, cv, p->span, NULL, NULL);
        return el_mk_bool(el, 1);
    }
    case PAT_LIT: {
        Value lv = elab_eval(el, p->u.lit, env, subj.ty && subj.ty->kind != TY_INT ? subj.ty : NULL);
        return el_binary(el, BIN_EQ, subj, lv, p->span, NULL, p->u.lit);
    }
    case PAT_RANGE: {
        Value r = el_mk_bool(el, 1);
        if (p->u.range.lo) {
            Value lo = elab_eval(el, p->u.range.lo, env, NULL);
            r = el_binary(el, BIN_GE, subj, lo, p->span, NULL, p->u.range.lo);
        }
        if (p->u.range.hi) {
            Value hi = elab_eval(el, p->u.range.hi, env, NULL);
            r = el_and(el, r, el_binary(el, p->u.range.inclusive ? BIN_LE : BIN_LT, subj, hi, p->span, NULL,
                                        p->u.range.hi), p->span);
        }
        return r;
    }
    case PAT_VARIANT: {
        Ty *t = subj.ty;
        int vi;
        Value r;
        uint32_t k;
        if (!t || t->kind != TY_ENUM) {
            el_error(el, p->span, "variant pattern `.%s` on %s", p->u.variant.name.name,
                     t ? ty_str(t) : val_kind_name((ValKind)subj.kind));
            return val_error();
        }
        vi = ty_variant_index(t, p->u.variant.name.name);
        if (vi < 0) {
            el_error(el, p->u.variant.name.span, "enum `%s` has no variant `%s`", ty_str(t), p->u.variant.name.name);
            return val_error();
        }
        if (subj.kind == VAL_ENUM)
            r = el_mk_bool(el, subj.aux == (uint32_t)vi);
        else
            r = el_hw(el, S->t_bool,
                      ir_binary(el->design->arena, IROP_EQ, ce_enum_tag(el, &subj, p->span),
                                ir_const_u64(el->design->arena, t->tag_width, t->variants[vi].tag, p->span), 0,
                                p->span));
        if (t->variants[vi].payload && p->u.variant.nfields) {
            Value pv = ce_payload_value(el, subj, (uint32_t)vi, p->span);
            for (k = 0; k < p->u.variant.nfields; k++) {
                FieldPat *fp = &p->u.variant.fields[k];
                if (fp->pat) {
                    Value fv = el_field(el, pv, fp->name.name, fp->name.span);
                    r = el_and(el, r, el_match_test(el, fp->pat, fv, env), p->span);
                }
            }
        }
        return r;
    }
    case PAT_PATH: {
        Value v = ce_eval_path(el, &p->u.path, env, subj.ty, 0);
        return el_binary(el, BIN_EQ, subj, v, p->span, NULL, NULL);
    }
    case PAT_STRUCT: {
        Value base = ce_eval_path(el, &p->u.struct_pat.path, env, subj.ty, 1);
        Value r = el_mk_bool(el, 1), target = subj;
        uint32_t k;
        if (base.kind == VAL_TYPE && base.aux && subj.ty && subj.ty->kind == TY_ENUM) {
            uint32_t vi = base.aux - 1;
            if (subj.kind == VAL_ENUM)
                r = el_mk_bool(el, subj.aux == vi);
            else
                r = el_hw(el, S->t_bool,
                          ir_binary(el->design->arena, IROP_EQ, ce_enum_tag(el, &subj, p->span),
                                    ir_const_u64(el->design->arena, subj.ty->tag_width, subj.ty->variants[vi].tag,
                                                 p->span), 0, p->span));
            target = ce_payload_value(el, subj, vi, p->span);
        }
        for (k = 0; k < p->u.struct_pat.nfields; k++) {
            FieldPat *fp = &p->u.struct_pat.fields[k];
            if (fp->pat) {
                Value fv = el_field(el, target, fp->name.name, fp->name.span);
                r = el_and(el, r, el_match_test(el, fp->pat, fv, env), p->span);
            }
        }
        return r;
    }
    case PAT_TUPLE: {
        Value r = el_mk_bool(el, 1);
        if (subj.kind != VAL_TUPLE || subj.u.list->n != p->u.list.n) {
            el_error(el, p->span, "tuple pattern does not match %s",
                     subj.ty ? ty_str(subj.ty) : val_kind_name((ValKind)subj.kind));
            return val_error();
        }
        for (i = 0; i < p->u.list.n; i++)
            r = el_and(el, r, el_match_test(el, p->u.list.items[i], subj.u.list->v[i], env), p->span);
        return r;
    }
    case PAT_OR: {
        Value r = el_mk_bool(el, 0);
        for (i = 0; i < p->u.list.n; i++)
            r = el_or(el, r, el_match_test(el, p->u.list.items[i], subj, env), p->span);
        return r;
    }
    }
    return val_error();
}

/* ----------------------------------------------------- exhaustiveness -- */

typedef struct CeSpace {     /* abstract value domain for coverage */
    Ty *ty;
    uint64_t size;
} CeSpace;

static uint64_t ce_space_size(Ty *t)
{
    uint32_t i;
    uint64_t s = 1;
    if (!t)
        return 0;
    switch (t->kind) {
    case TY_BOOL: return 2;
    case TY_ENUM: return t->nvariants;
    case TY_UINT: case TY_BIT: case TY_SINT: return t->width <= 16 ? ((uint64_t)1 << t->width) : 0;
    case TY_TUPLE:
        for (i = 0; i < t->nfields; i++) {
            uint64_t k = ce_space_size(t->fields[i].ty);
            if (!k || s * k > 65536)
                return 0;
            s *= k;
        }
        return s;
    default: return 0;
    }
}

/* abstract value number n of type t as a compile-time Value */
static Value ce_space_value(Elab *el, Ty *t, uint64_t n)
{
    uint32_t i;
    switch (t->kind) {
    case TY_BOOL: return el_mk_bool(el, (int)n);
    case TY_ENUM: {
        Value v = val_enum(&el->arena, t, (uint32_t)n, NULL);
        if (t->variants[n].payload)
            v.u.list = el_zero(el, t->variants[n].payload).u.list;
        return v;
    }
    case TY_SINT: {
        int64_t x = (int64_t)n;
        if (n & ((uint64_t)1 << (t->width - 1)))
            x = (int64_t)n - ((int64_t)1 << t->width);
        return val_int(t, x);
    }
    case TY_UINT: case TY_BIT: return val_int(t, (int64_t)n);
    case TY_TUPLE: {
        Value v = val_list(&el->arena, VAL_TUPLE, t, t->nfields);
        for (i = 0; i < t->nfields; i++) {
            uint64_t k = ce_space_size(t->fields[i].ty);
            v.u.list->v[i] = ce_space_value(el, t->fields[i].ty, n % k);
            n /= k;
        }
        return v;
    }
    default: return val_error();
    }
}

/* does pattern p certainly match the abstract value v (payload contents unknown)? */
static int ce_pat_covers(Elab *el, Pattern *p, const Value *v, Env *env)
{
    uint32_t i;
    switch (p->kind) {
    case PAT_WILDCARD: case PAT_ERROR: return 1;
    case PAT_BIND: {
        Value cv;
        if (ce_bind_is_const(el, p, env, &cv))
            return val_equal(&cv, v);
        return 1;
    }
    case PAT_VARIANT:
        if (v->kind != VAL_ENUM || v->ty->variants[v->aux].name != p->u.variant.name.name)
            return 0;
        for (i = 0; i < p->u.variant.nfields; i++)
            if (p->u.variant.fields[i].pat && !ce_pat_irrefutable(p->u.variant.fields[i].pat))
                return 0;
        return 1;
    case PAT_TUPLE:
        if (v->kind != VAL_TUPLE || v->u.list->n != p->u.list.n)
            return 0;
        for (i = 0; i < p->u.list.n; i++)
            if (!ce_pat_covers(el, p->u.list.items[i], &v->u.list->v[i], env))
                return 0;
        return 1;
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            if (ce_pat_covers(el, p->u.list.items[i], v, env))
                return 1;
        return 0;
    default: {
        Value r;
        int q = el->quiet;
        el->quiet = 1;
        r = el_match_test(el, p, *v, env);
        el->quiet = q;
        el->quiet_errors = 0;
        return r.kind == VAL_BOOL && r.u.i;
    }
    }
}

static void ce_check_exhaustive(Elab *el, Expr *m, Ty *t, Env *env)
{
    uint32_t a, nmiss = 0;
    uint64_t size, n;
    StrBuf miss = {0};
    for (a = 0; a < m->u.match.narms; a++)
        if (ce_pat_irrefutable(m->u.match.arms[a].pat))
            return;
    size = ce_space_size(t);
    if (!size) {
        el_error(el, m->span, "non-exhaustive match on %s: add a `_ =>` arm", t ? ty_str(t) : "this value");
        return;
    }
    for (n = 0; n < size; n++) {
        Value v = ce_space_value(el, t, n);
        int covered = 0;
        for (a = 0; a < m->u.match.narms && !covered; a++)
            covered = ce_pat_covers(el, m->u.match.arms[a].pat, &v, env);
        if (!covered) {
            if (nmiss < 4) {
                if (nmiss)
                    sb_puts(&miss, ", ");
                sb_puts(&miss, "`");
                if (v.kind == VAL_ENUM)
                    sb_printf(&miss, ".%s", t->variants[v.aux].name);
                else
                    val_print(&v, &miss);
                sb_puts(&miss, "`");
            }
            nmiss++;
        }
    }
    if (nmiss) {
        el_error(el, m->u.match.scrutinee->span, "non-exhaustive match: %s %s not covered%s", sb_cstr(&miss),
                 nmiss == 1 ? "is" : "are", nmiss > 4 ? " (and more)" : "");
        el_help(el, "add the missing arm%s, or a `_ =>` arm", nmiss == 1 ? "" : "s");
    }
    sb_free(&miss);
}

/* ================================================ branches and merging */

void el_log_assign(Elab *el, Binding *b, Env *env)
{
    UndoEntry u;
    TERN_UNUSED(env);
    if (!el->undo_depth)
        return;
    u.env = b->owner;
    if (!u.env)
        return;
    u.name = b->name;
    u.old = val_copy_deep(&el->arena, &b->val);
    vec_push(el->undo, u);
}

static int ce_env_in_chain(Env *e, Env *target)
{
    for (; e; e = e->parent)
        if (e == target)
            return 1;
    return 0;
}

typedef struct CeMod {       /* a local modified in a branch */
    Env *env;
    const char *name;
    Value val;
} CeMod;

typedef VEC(CeMod) CeModVec;

/* collects locals modified since `mark` (alive in `env`), then restores their
 * values from the undo log */
static void ce_collect_restore(Elab *el, uint32_t mark, Env *env, CeModVec *out)
{
    size_t i, j;
    for (i = mark; i < el->undo.len; i++) {
        UndoEntry *u = &el->undo.data[i];
        Binding *b;
        int dup = 0;
        if (!ce_env_in_chain(env, u->env))
            continue;
        for (j = 0; j < out->len; j++)
            if (out->data[j].env == u->env && out->data[j].name == u->name)
                dup = 1;
        if (dup)
            continue;
        b = ce_env_find_local(u->env, u->name);
        if (b) {
            CeMod m;
            m.env = u->env;
            m.name = u->name;
            m.val = b->val;
            vec_push(*out, m);
        }
    }
    for (i = el->undo.len; i-- > mark;) {
        UndoEntry *u = &el->undo.data[i];
        Binding *b;
        if (!ce_env_in_chain(env, u->env))
            continue;
        b = ce_env_find_local(u->env, u->name);
        if (b)
            b->val = u->old;
    }
    el->undo.len = mark;
}

static Value ce_mod_lookup(CeModVec *v, Env *env, const char *name, int *found)
{
    size_t i;
    for (i = 0; i < v->len; i++)
        if (v->data[i].env == env && v->data[i].name == name) {
            *found = 1;
            return v->data[i].val;
        }
    *found = 0;
    return val_error();
}

typedef struct CeArmRun {
    Value cond;               /* hw bool, or const true for a default */
    Value excl;               /* inside fns: the condition under which this arm runs
                                 (cond and not the earlier arms); VAL_VOID if unknown */
    IrStmtList stmts;
    CeModVec mods;
    Value val;
    int returned;             /* the arm ended in a `return` */
} CeArmRun;

/* merges locals modified in the arms: value = mux chain over the arm
 * conditions (arms[n-1] is the fallback when `exhaustive`) */
static void ce_merge_arms(Elab *el, CeArmRun *arms, uint32_t n, int exhaustive, Env *env, Span at)
{
    CeModVec all = {0};
    uint32_t a;
    size_t i;
    for (a = 0; a < n; a++)
        for (i = 0; i < arms[a].mods.len; i++) {
            int f;
            ce_mod_lookup(&all, arms[a].mods.data[i].env, arms[a].mods.data[i].name, &f);
            if (!f)
                vec_push(all, arms[a].mods.data[i]);
        }
    for (i = 0; i < all.len; i++) {
        Binding *b = ce_env_find_local(all.data[i].env, all.data[i].name);
        Value cur, r;
        int f;
        uint32_t k;
        if (!b)
            continue;
        cur = b->val;
        if (exhaustive) {
            r = ce_mod_lookup(&arms[n - 1].mods, all.data[i].env, all.data[i].name, &f);
            if (!f)
                r = cur;
            k = n - 1;
        } else {
            r = cur;
            k = n;
        }
        while (k-- > 0) {
            Value av = ce_mod_lookup(&arms[k].mods, all.data[i].env, all.data[i].name, &f);
            if (!f)
                av = cur;
            r = el_unify_mux(el, arms[k].cond, av, r, b->val.ty && b->val.ty->hw ? b->val.ty : NULL, at);
        }
        el_log_assign(el, b, env);
        b = ce_env_find_local(all.data[i].env, all.data[i].name);
        if (b)
            b->val = el_maybe_temp(el, r, at);
    }
    vec_free(all);
}

/* Runs `body` (a statement or an expression) as one arm under a hardware
 * condition: emitted statements go to arm->stmts, modified locals are
 * recorded and restored. */
static void ce_run_arm(Elab *el, CeArmRun *arm, Stmt *body_stmt, Expr *body_expr, Pattern *pat, Value subj,
                       Env *env, Ty *expect, int want_value)
{
    uint32_t mark = (uint32_t)el->undo.len;
    IrStmtList *saved = el->blk ? el->blk->out : NULL;
    Env *aenv = elab_env_new(el, env, NULL);
    Value saved_path = el->fn_path;
    if (el->blk)
        el->blk->out = &arm->stmts;
    if (el->fn_active && arm->excl.kind != VAL_VOID)
        el->fn_path = el_and(el, el->fn_path, arm->excl, body_stmt ? body_stmt->span : body_expr->span);
    el->undo_depth++;
    if (pat)
        el_bind_pattern(el, pat, subj, aenv, 0);
    if (body_expr) {
        if (want_value)
            arm->val = elab_eval(el, body_expr, aenv, expect);
        else {
            Stmt tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.kind = STMT_EXPR;
            tmp.span = body_expr->span;
            tmp.u.expr = body_expr;
            el_exec_stmt(el, &tmp, aenv);
        }
    } else if (body_stmt) {
        if (want_value && body_stmt->kind == STMT_EXPR)
            arm->val = elab_eval(el, body_stmt->u.expr, aenv, expect);
        else
            el_exec_stmt(el, body_stmt, aenv);
    }
    el->undo_depth--;
    el->fn_path = saved_path;
    if (el->returned && el->fn_active) {
        /* a `return` under this arm's condition was recorded in fn_ret_*;
         * the code after the branch still runs for the other paths */
        arm->returned = 1;
        el->returned = 0;
    }
    ce_collect_restore(el, mark, env, &arm->mods);
    if (el->blk)
        el->blk->out = saved;
    elab_env_free(aenv);
}

/* every arm of a branch returned: the branch as a whole returns */
static void ce_all_returned(Elab *el, CeArmRun *arms, uint32_t n)
{
    uint32_t a;
    if (!el->fn_active || !el->fn_ret_have || !n)
        return;
    for (a = 0; a < n; a++)
        if (!arms[a].returned)
            return;
    el->ret = el->fn_ret_val;
    el->returned = 1;
}

static IrBlock ce_finish(Elab *el, IrStmtList *l)
{
    return ir_stmts_finish(el->design ? el->design->arena : &el->arena, l);
}

static void ce_emit(Elab *el, IrStmt *s)
{
    if (el->blk && el->blk->out)
        ir_stmts_push(el->blk->out, s);
}

static void ce_exec_block_noval(Elab *el, Block *b, Env *env);

/* if/else with a hardware condition (statement or value context) */
static Value ce_if_hw(Elab *el, Value c, Block *then_b, Expr *else_e, Env *env, Ty *expect, int want_value, Span at)
{
    CeArmRun arms[2];
    Expr then_expr;
    Value r = val_void(el->S);
    uint32_t a;
    memset(arms, 0, sizeof arms);
    memset(&then_expr, 0, sizeof then_expr);
    then_expr.kind = EXPR_BLOCK;
    then_expr.span = then_b->span;
    then_expr.u.block = then_b;
    arms[0].cond = c;
    arms[1].cond = el_mk_bool(el, 1);
    arms[0].excl = val_void(el->S);
    arms[1].excl = val_void(el->S);
    if (el->fn_active) {
        arms[0].excl = c;
        arms[1].excl = el_not(el, c, at);
    }
    ce_run_arm(el, &arms[0], NULL, &then_expr, NULL, val_void(el->S), env, expect, want_value);
    if (else_e)
        ce_run_arm(el, &arms[1], NULL, else_e, NULL, val_void(el->S), env, expect, want_value);
    if (el->blk && (arms[0].stmts.len || arms[1].stmts.len))
        ce_emit(el, ir_stmt_if(el->design->arena, el_to_ir(el, &c, at), ce_finish(el, &arms[0].stmts),
                               ce_finish(el, &arms[1].stmts), at));
    else {
        ir_stmts_finish(&el->arena, &arms[0].stmts);
        ir_stmts_finish(&el->arena, &arms[1].stmts);
    }
    ce_merge_arms(el, arms, 2, 1, env, at);
    if (else_e)
        ce_all_returned(el, arms, 2);
    if (want_value && el->returned) {
        r = val_void(el->S); /* both arms returned from the fn */
    } else if (want_value) {
        if (!else_e) {
            el_error(el, at, "an `if` used as a value needs an `else`");
            r = val_error();
        } else if (arms[0].returned || arms[1].returned) {
            /* the arm that returned has no value: the fn result covers that path */
            r = arms[0].returned ? arms[1].val : arms[0].val;
        } else {
            r = el_unify_mux(el, c, arms[0].val, arms[1].val, expect, at);
        }
    }
    for (a = 0; a < 2; a++)
        vec_free(arms[a].mods);
    return r;
}

static Value ce_eval_if(Elab *el, Expr *e, Env *env, Ty *expect, int want_value)
{
    Value c = elab_eval(el, e->u.if_expr.cond, env, el->S->t_bool);
    if (c.kind == VAL_ERROR)
        return c;
    if (!c.ty || c.ty->kind != TY_BOOL) {
        el_error(el, e->u.if_expr.cond->span, "`if` condition must be bool, found %s",
                 c.ty ? ty_str(c.ty) : val_kind_name((ValKind)c.kind));
        if (ce_int_family(c.ty))
            el_help(el, "compare explicitly: `%s != 0`", el_text(e->u.if_expr.cond));
        return val_error();
    }
    if (c.kind == VAL_BOOL) {
        if (c.u.i) {
            if (want_value)
                return el_eval_block(el, e->u.if_expr.then_block, env, expect);
            ce_exec_block_noval(el, e->u.if_expr.then_block, env);
            return val_void(el->S);
        }
        if (e->u.if_expr.else_expr) {
            if (want_value)
                return elab_eval(el, e->u.if_expr.else_expr, env, expect);
            if (e->u.if_expr.else_expr->kind == EXPR_IF)
                return ce_eval_if(el, e->u.if_expr.else_expr, env, NULL, 0);
            if (e->u.if_expr.else_expr->kind == EXPR_BLOCK)
                ce_exec_block_noval(el, e->u.if_expr.else_expr->u.block, env);
            else
                elab_eval(el, e->u.if_expr.else_expr, env, NULL);
            return val_void(el->S);
        }
        return val_void(el->S);
    }
    if (!el->design)
        return ce_need_design(el, e->span);
    return ce_if_hw(el, c, e->u.if_expr.then_block, e->u.if_expr.else_expr, env, expect, want_value, e->span);
}

/* constant case values of an arm pattern, for IRST_CASE; 0 if not simple */
typedef VEC(IrBits) CeBitsVec;

static int ce_case_vals(Elab *el, Pattern *p, Ty *t, Env *env, CeBitsVec *out, int *is_default)
{
    uint32_t i;
    switch (p->kind) {
    case PAT_WILDCARD:
        *is_default = 1;
        return 1;
    case PAT_VARIANT: {
        int vi;
        if (t->kind != TY_ENUM || p->u.variant.nfields || t->payload_width)
            return 0;
        vi = ty_variant_index(t, p->u.variant.name.name);
        if (vi < 0)
            return 0;
        vec_push(*out, ir_bits_u64(el->design->arena, t->width, t->variants[vi].tag));
        return 1;
    }
    case PAT_LIT: case PAT_PATH: {
        Value v;
        int q = el->quiet;
        if (!ty_is_sized_int(t) && t->kind != TY_ENUM && t->kind != TY_BOOL)
            return 0;
        el->quiet = 1;
        v = p->kind == PAT_LIT ? elab_eval(el, p->u.lit, env, t) : ce_eval_path(el, &p->u.path, env, t, 0);
        if (v.kind != VAL_ERROR)
            v = elab_convert(el, v, t, NULL, NULL, p->span);
        el->quiet = q;
        if (el->quiet_errors && !q) {
            el->quiet_errors = 0;
            return 0;
        }
        if (v.kind == VAL_ERROR || !val_is_const(&v))
            return 0;
        vec_push(*out, elab_const_bits(el, &v, t));
        return 1;
    }
    case PAT_OR:
        for (i = 0; i < p->u.list.n; i++)
            if (!ce_case_vals(el, p->u.list.items[i], t, env, out, is_default))
                return 0;
        return 1;
    default:
        return 0;
    }
}

static int ce_arms_have_stmts(CeArmRun *arms, uint32_t n)
{
    uint32_t a;
    for (a = 0; a < n; a++)
        if (arms[a].stmts.len)
            return 1;
    return 0;
}

static Value ce_eval_match(Elab *el, Expr *e, Env *env, Ty *expect, int want_value)
{
    Value subj = elab_eval(el, e->u.match.scrutinee, env, NULL);
    uint32_t a, n = e->u.match.narms;
    CeArmRun *arms;
    Value r = val_void(el->S);
    int exhaustive;
    if (subj.kind == VAL_ERROR)
        return subj;
    if (subj.ty)
        ce_check_exhaustive(el, e, subj.ty, env);
    else if (subj.kind == VAL_TUPLE) {
        /* tuple of mixed values: build its type for the check */
        Ty **ts = (Ty **)xcalloc(subj.u.list->n ? subj.u.list->n : 1, sizeof(Ty *));
        int ok = 1;
        for (a = 0; a < subj.u.list->n; a++) {
            ts[a] = subj.u.list->v[a].ty;
            if (!ts[a])
                ok = 0;
        }
        if (ok) {
            subj.ty = ty_tuple(el->S, subj.u.list->n, ts);
            ce_check_exhaustive(el, e, subj.ty, env);
        }
        free(ts);
    }
    if (val_is_const(&subj)) {
        for (a = 0; a < n; a++) {
            MatchArm *arm = &e->u.match.arms[a];
            Value t = el_match_test(el, arm->pat, subj, env);
            if (t.kind == VAL_ERROR)
                return t;
            if (t.kind == VAL_BOOL && t.u.i) {
                Env *aenv = elab_env_new(el, env, NULL);
                el_bind_pattern(el, arm->pat, subj, aenv, 0);
                if (want_value && arm->body->kind == STMT_EXPR)
                    r = elab_eval(el, arm->body->u.expr, aenv, expect);
                else
                    el_exec_stmt(el, arm->body, aenv);
                elab_env_free(aenv);
                return r;
            }
            if (t.kind != VAL_BOOL) {
                el_error(el, arm->pat->span, "match on a compile-time value with a pattern that depends on signals");
                return val_error();
            }
        }
        if (want_value) {
            el_error(el, e->span, "no match arm matches this value");
            return val_error();
        }
        return r;
    }
    if (!el->design)
        return ce_need_design(el, e->span);
    arms = (CeArmRun *)xcalloc(n ? n : 1, sizeof(CeArmRun));
    /* IRST_CASE when every pattern is a constant of an enum/int subject */
    {
        int simple = el->blk && !want_value && subj.kind == VAL_HW && subj.ty &&
                     (subj.ty->kind == TY_ENUM || ty_is_sized_int(subj.ty) || subj.ty->kind == TY_BOOL);
        CeBitsVec *vals = (CeBitsVec *)xcalloc(n ? n : 1, sizeof(*vals));
        int *defs = (int *)xcalloc(n ? n : 1, sizeof(int));
        for (a = 0; simple && a < n; a++)
            if (!ce_case_vals(el, e->u.match.arms[a].pat, subj.ty, env, &vals[a], &defs[a]))
                simple = 0;
        Value prev = el_mk_bool(el, 0);
        for (a = 0; a < n; a++) {
            MatchArm *arm = &e->u.match.arms[a];
            arms[a].cond = el_match_test(el, arm->pat, subj, env);
            arms[a].excl = val_void(el->S);
            if (el->fn_active && arms[a].cond.kind != VAL_ERROR) {
                arms[a].excl = el_and(el, arms[a].cond, el_not(el, prev, arm->span), arm->span);
                prev = el_or(el, prev, arms[a].cond, arm->span);
            }
            ce_run_arm(el, &arms[a], arm->body, NULL, arm->pat, subj, env, expect, want_value);
        }
        exhaustive = 1;
        if (simple) {
            IrCaseArm *ca = (IrCaseArm *)xcalloc(n ? n : 1, sizeof(IrCaseArm));
            uint32_t k = 0;
            for (a = 0; a < n; a++) {
                if (defs[a]) {
                    ca[k].nvals = 0;
                } else {
                    ca[k].nvals = (uint32_t)vals[a].len;
                    ca[k].vals = vals[a].data;
                }
                ca[k].body = ce_finish(el, &arms[a].stmts);
                k++;
                if (defs[a])
                    break;
            }
            for (; a + 1 < n; a++)
                ir_stmts_finish(&el->arena, &arms[a + 1].stmts);
            ce_emit(el, ir_stmt_case(el->design->arena, el_to_ir(el, &subj, e->span), k, ca, e->span));
            free(ca);
        } else if (el->blk && ce_arms_have_stmts(arms, n)) {
            /* if-chain, built from the last arm backwards */
            IrBlock tail;
            int have = 0;
            memset(&tail, 0, sizeof tail);
            for (a = n; a-- > 0;) {
                IrBlock body = ce_finish(el, &arms[a].stmts);
                if (arms[a].cond.kind == VAL_BOOL) {
                    if (arms[a].cond.u.i) {
                        tail = body;
                        have = 1;
                    }
                    continue;
                }
                if (arms[a].cond.kind == VAL_ERROR)
                    continue;
                if (!have && body.n == 0)
                    continue;
                tail = ir_block1(el->design->arena,
                                 ir_stmt_if(el->design->arena, el_to_ir(el, &arms[a].cond, e->u.match.arms[a].span), body,
                                            tail, e->span));
                have = 1;
            }
            for (a = 0; a < tail.n; a++)
                ce_emit(el, tail.stmts[a]);
        } else {
            for (a = 0; a < n; a++)
                ir_stmts_finish(&el->arena, &arms[a].stmts);
        }
        for (a = 0; a < n; a++)
            vec_free(vals[a]);
        free(vals);
        free(defs);
    }
    ce_merge_arms(el, arms, n, exhaustive, env, e->span);
    ce_all_returned(el, arms, n);
    if (want_value && n && el->returned) {
        r = val_void(el->S); /* every arm returned from the fn */
    } else if (want_value && n) {
        uint32_t last = n;
        while (last > 0 && arms[last - 1].returned)
            last--;
        r = last ? arms[last - 1].val : val_void(el->S);
        for (a = last ? last - 1 : 0; a-- > 0;)
            if (!arms[a].returned)
                r = el_unify_mux(el, arms[a].cond, arms[a].val, r, expect, e->span);
    }
    for (a = 0; a < n; a++)
        vec_free(arms[a].mods);
    free(arms);
    return r;
}

/* ============================================================ statements */

static void ce_exec_block_noval(Elab *el, Block *b, Env *env)
{
    Env *benv = elab_env_new(el, env, NULL);
    el_exec_stmts(el, b->stmts, b->nstmts, benv, NULL, NULL);
    elab_env_free(benv);
}

Value el_eval_block(Elab *el, Block *b, Env *env, Ty *expect)
{
    Env *benv = elab_env_new(el, env, NULL);
    Value tail = val_void(el->S);
    el->tail_expect = expect;
    el_exec_stmts(el, b->stmts, b->nstmts, benv, b->tail, &tail);
    elab_env_free(benv);
    return tail;
}

static int ce_is_local_root(Env *env, Expr *e, Binding **out)
{
    while (e->kind == EXPR_FIELD || e->kind == EXPR_INDEX || e->kind == EXPR_SLICE || e->kind == EXPR_PAREN)
        e = e->kind == EXPR_FIELD ? e->u.field.base : e->kind == EXPR_INDEX ? e->u.index.base
          : e->kind == EXPR_SLICE ? e->u.slice.base : e->u.inner;
    if (e->kind == EXPR_PATH && e->u.path.nsegs == 1 && e->u.path.segs[0].name.name) {
        Binding *b = elab_env_find(env, e->u.path.segs[0].name.name);
        if (b && !b->lazy && (b->is_mut || b->val.kind == VAL_OPAQUE)) {
            *out = b;
            return 1;
        }
    }
    return 0;
}

static Place *ce_local_place(Elab *el, Expr *e, Env *env, Binding *root)
{
    Place *p;
    Place *base;
    switch (e->kind) {
    case EXPR_PAREN:
        return ce_local_place(el, e->u.inner, env, root);
    case EXPR_PATH:
        p = ANEW(&el->arena, Place);
        p->kind = PL_LOCAL;
        p->bind = root;
        p->ty = root->val.ty;
        return p;
    case EXPR_FIELD:
    case EXPR_INDEX:
    case EXPR_SLICE: {
        PlaceSel sel;
        Value cur;
        Expr *be = e->kind == EXPR_FIELD ? e->u.field.base : e->kind == EXPR_INDEX ? e->u.index.base : e->u.slice.base;
        base = ce_local_place(el, be, env, root);
        if (!base)
            return NULL;
        memset(&sel, 0, sizeof sel);
        cur = el_read_place(el, base);
        if (e->kind == EXPR_FIELD) {
            int k;
            if (cur.kind == VAL_OPAQUE) {
                sel.kind = 3;
                sel.idx = val_string(el->S, e->u.field.name.name, (uint32_t)strlen(e->u.field.name.name));
            } else {
                k = ty_field_index(cur.ty, e->u.field.name.name);
                if (k < 0) {
                    el_error(el, e->u.field.name.span, "no field `%s` on %s", e->u.field.name.name,
                             cur.ty ? ty_str(cur.ty) : "this value");
                    return NULL;
                }
                sel.kind = 0;
                sel.a = (uint32_t)k;
            }
        } else if (e->kind == EXPR_INDEX) {
            Value iv = elab_eval(el, e->u.index.index, env, NULL);
            if (iv.kind == VAL_ERROR)
                return NULL;
            if (cur.kind == VAL_OPAQUE) {
                sel.kind = 4;
                sel.idx = iv;
            } else if (val_is_const(&iv) && iv.kind == VAL_INT) {
                int64_t i;
                uint32_t lim = cur.kind == VAL_ARRAY || cur.kind == VAL_TUPLE ? cur.u.list->n
                             : cur.ty && cur.ty->kind == TY_ARRAY ? cur.ty->len
                             : cur.ty ? (cur.ty->kind == TY_INT ? 0xFFFFFFu : cur.ty->width) : 0;
                if (!val_get_i64(&iv, &i) || i < 0 || (uint64_t)i >= lim) {
                    el_error(el, e->u.index.index->span, "index out of range");
                    return NULL;
                }
                if (cur.kind == VAL_ARRAY || cur.kind == VAL_TUPLE || (cur.ty && cur.ty->kind == TY_ARRAY)) {
                    sel.kind = 0;
                    sel.a = (uint32_t)i;
                } else {
                    sel.kind = 2;
                    sel.a = (uint32_t)i;
                    sel.b = 1;
                }
            } else {
                sel.kind = 1;
                sel.idx = iv;
            }
        } else {
            Value hi = elab_eval_const(el, e->u.slice.hi, env, el->S->t_int, "a slice bound");
            Value lo = elab_eval_const(el, e->u.slice.lo, env, el->S->t_int, "a slice bound");
            int64_t h, l;
            if (!val_get_i64(&hi, &h) || !val_get_i64(&lo, &l) || l < 0 || h < l)
                return NULL;
            sel.kind = 2;
            sel.a = (uint32_t)l;
            sel.b = (uint32_t)(h - l + 1);
        }
        p = ANEW(&el->arena, Place);
        *p = *base;
        p->nsel = base->nsel + 1;
        p->sels = ANEWN(&el->arena, PlaceSel, p->nsel);
        if (base->nsel)
            memcpy(p->sels, base->sels, base->nsel * sizeof(PlaceSel));
        p->sels[base->nsel] = sel;
        return p;
    }
    default:
        el_error(el, e->span, "cannot assign to this expression");
        return NULL;
    }
}

Place *el_place_of(Elab *el, Expr *e, Env *env)
{
    Binding *root;
    Value v;
    if (ce_is_local_root(env, e, &root))
        return ce_local_place(el, e, env, root);
    v = elab_eval(el, e, env, NULL);
    if (v.kind == VAL_ERROR)
        return NULL;
    if (v.place)
        return v.place;
    if (v.kind == VAL_INST) {
        el_error(el, e->span, "cannot assign to instance `%s`", el_text(e));
        return NULL;
    }
    {
        Binding *b = NULL;
        if (e->kind == EXPR_PATH && e->u.path.nsegs == 1 && e->u.path.segs[0].name.name)
            b = elab_env_find(env, e->u.path.segs[0].name.name);
        if (b && !b->is_mut && b->sem == SEM_LOCAL) {
            el_error(el, e->span, "cannot assign to `%s`: `let` bindings are immutable", el_text(e));
            el_help(el, "declare it with `var` to assign it");
        } else if (b && b->sem == SEM_WIRE) {
            el_error(el, e->span, "cannot assign to `%s`: a `let` is assigned once, at its declaration", el_text(e));
        } else if (b && (b->sem == SEM_CONST || b->sem == SEM_GENERIC)) {
            el_error(el, e->span, "cannot assign to compile-time constant `%s`", el_text(e));
        } else {
            el_error(el, e->span, "cannot assign to `%s`", el_text(e));
        }
    }
    return NULL;
}

/* value of (part of) a local, following the selector chain */
static Value ce_sel_read(Elab *el, Value cur, const PlaceSel *s, Span at)
{
    switch (s->kind) {
    case 0:
        if (cur.kind == VAL_ARRAY || cur.kind == VAL_STRUCT || cur.kind == VAL_TUPLE || cur.kind == VAL_BUNDLE)
            return cur.u.list->v[s->a];
        if (cur.ty && cur.ty->kind == TY_STRUCT)
            return el_field(el, cur, cur.ty->fields[s->a].name, at);
        return el_index(el, cur, val_int(el->S->t_int, s->a), at, NULL);
    case 1:
        return el_index(el, cur, s->idx, at, NULL);
    case 2: {
        if (cur.ty && cur.ty->kind == TY_INT && cur.kind == VAL_INT) {
            BigInt b = {0};
            Value r;
            val_get_bn(&cur, &b);
            bn_shr(&b, &b, s->a);
            r = ce_int_norm(el, s->b == 1 ? ty_uint(el->S, 1) : ty_bits(el->S, s->b), &b);
            bn_free(&b);
            if (s->b == 1)
                return el_mk_bool(el, r.u.i != 0);
            return r;
        }
        if (s->b == 1)
            return el_index(el, cur, val_int(el->S->t_int, s->a), at, NULL);
        if (val_is_const(&cur)) {
            BigInt b = {0};
            Value r;
            el_bits_of_const(el, &cur, cur.ty, &b);
            bn_shr(&b, &b, s->a);
            r = ce_int_norm(el, ty_bits(el->S, s->b), &b);
            bn_free(&b);
            return r;
        }
        return el_hw(el, ty_bits(el->S, s->b), ir_slice(el->design->arena, cur.u.e, s->a, s->b, at));
    }
    case 3:
        return el_field(el, cur, s->idx.u.s, at);
    case 4:
        return el_index(el, cur, s->idx, at, NULL);
    }
    return val_error();
}

Value el_read_place(Elab *el, Place *p)
{
    uint32_t i;
    switch (p->kind) {
    case PL_LOCAL: {
        Value cur = p->bind->val;
        for (i = 0; i < p->nsel && cur.kind != VAL_ERROR; i++)
            cur = ce_sel_read(el, cur, &p->sels[i], p->bind->def);
        return cur;
    }
    case PL_AGG: {
        Value v = val_list(&el->arena, p->ty && p->ty->kind == TY_BUNDLE ? VAL_BUNDLE : VAL_ARRAY, p->ty, p->n);
        for (i = 0; i < p->n; i++)
            v.u.list->v[i] = el_read_place(el, p->subs[i]);
        v.place = p;
        return v;
    }
    case PL_DYN: {
        Value *vs = ANEWN(&el->arena, Value, p->n ? p->n : 1);
        Value v;
        for (i = 0; i < p->n; i++)
            vs[i] = el_read_place(el, p->subs[i]);
        v = ce_dyn_pick(el, vs, p->n, p->index, span_none());
        v.place = p;
        return v;
    }
    case PL_SIG: {
        IrModule *m = el->mod->ir;
        IrExpr *e = ir_sig(el->design->arena, m, p->sig, span_none());
        Value v;
        if (p->index)
            e = ir_index(el->design->arena, p->lo ? ir_slice(el->design->arena, e, p->lo, m->sigs[p->sig].type->width - p->lo, span_none()) : e,
                         p->index, p->elem_width, span_none());
        else if (p->lo != 0 || p->width != e->width) {
            e = ir_slice(el->design->arena, e, p->lo, p->width, span_none());
            e->field = p->field;
        }
        v = el_hw(el, p->ty, e);
        v.place = p;
        return v;
    }
    }
    return val_error();
}

/* functional update of `cur` at selector chain sels[0..n) with `nv` */
static Value ce_sel_update(Elab *el, Value cur, const PlaceSel *sels, uint32_t n, Value nv, Span at)
{
    const PlaceSel *s = &sels[0];
    if (n == 0)
        return nv;
    if (s->kind == 3 || s->kind == 4) {
        /* opaque handle: set through the ops */
        ValOpaque *o = cur.u.opq;
        int r;
        if (cur.kind != VAL_OPAQUE)
            return val_error();
        if (n > 1) {
            Value sub = ce_sel_read(el, cur, s, at);
            ce_sel_update(el, sub, sels + 1, n - 1, nv, at);
            return cur;
        }
        if (s->kind == 3)
            r = o->ops->set_field ? o->ops->set_field(el, o->obj, s->idx.u.s, nv, at) : 0;
        else
            r = o->ops->set_index ? o->ops->set_index(el, o->obj, s->idx, nv, at) : 0;
        if (r == 0)
            el_error(el, at, "cannot assign to this part of %s", o->ops->type_name);
        return cur;
    }
    if (s->kind == 0 && (cur.kind == VAL_ARRAY || cur.kind == VAL_STRUCT || cur.kind == VAL_TUPLE ||
                         cur.kind == VAL_BUNDLE)) {
        Value sub = cur.u.list->v[s->a];
        Ty *et = cur.ty ? (cur.ty->kind == TY_ARRAY ? cur.ty->elem : s->a < cur.ty->nfields ? cur.ty->fields[s->a].ty : NULL)
                        : NULL;
        Value r = ce_sel_update(el, sub, sels + 1, n - 1, nv, at);
        if (n == 1 && et && r.kind != VAL_ERROR)
            r = elab_convert(el, r, et, NULL, NULL, at);
        cur.u.list->v[s->a] = r;
        if (!val_is_const(&r) && cur.ty && cur.ty->hw && cur.kind != VAL_BUNDLE) {
            if (cur.kind == VAL_ARRAY)
                return ce_materialize_array(el, cur, cur.ty, at);
            return ce_build_struct(el, cur.ty, cur.u.list, at);
        }
        return cur;
    }
    if (!cur.ty || !cur.ty->hw) {
        if (cur.kind == VAL_INT && cur.ty && cur.ty->kind == TY_INT && s->kind == 2 && n == 1) {
            BigInt b = {0}, x = {0};
            uint32_t i;
            Value r;
            val_get_bn(&cur, &b);
            if (nv.kind == VAL_BOOL)
                bn_set_i64(&x, nv.u.i);
            else
                val_get_bn(&nv, &x);
            for (i = 0; i < s->b; i++)
                bn_set_bit(&b, s->a + i, bn_get_bit(&x, i));
            r = val_int_bn(&el->arena, cur.ty, &b);
            bn_free(&b);
            bn_free(&x);
            return r;
        }
        el_error(el, at, "cannot assign to a part of a %s", cur.ty ? ty_str(cur.ty) : val_kind_name((ValKind)cur.kind));
        return val_error();
    }
    /* hardware-typed value: work on bits */
    {
        Ty *t = cur.ty;
        uint32_t lo, w;
        Ty *st;
        Value sub, r;
        if (s->kind == 1) {
            /* dynamic element/bit: mux every element */
            uint32_t len = t->kind == TY_ARRAY ? t->len : t->width, ew = t->kind == TY_ARRAY ? t->elem->width : 1, i;
            Ty *et = t->kind == TY_ARRAY ? t->elem : el->S->t_bool;
            IrExpr **parts;
            Value cv;
            if (!el->design)
                return ce_need_design(el, at);
            cv = cur.kind == VAL_HW ? cur : el_hw(el, t, el_to_ir(el, &cur, at));
            parts = (IrExpr **)xcalloc(len, sizeof(IrExpr *));
            for (i = 0; i < len; i++) {
                Value old = el_hw(el, et, ir_slice(el->design->arena, cv.u.e, i * ew, ew, at));
                Value upd = ce_sel_update(el, old, sels + 1, n - 1, nv, at);
                Value hit = el_binary(el, BIN_EQ, s->idx, val_int(el->S->t_int, i), at, NULL, NULL);
                upd = elab_convert(el, upd, et, NULL, NULL, at);
                if (upd.kind == VAL_ERROR || hit.kind == VAL_ERROR) {
                    free(parts);
                    return val_error();
                }
                parts[len - 1 - i] = el_unify_mux(el, hit, upd, old, et, at).u.e;
            }
            r = el_hw(el, t, ir_concat(el->design->arena, len, parts, at));
            free(parts);
            return r;
        }
        if (s->kind == 0) {
            if (t->kind == TY_ARRAY) {
                lo = s->a * t->elem->width;
                w = t->elem->width;
                st = t->elem;
            } else {
                lo = t->fields[s->a].lsb;
                w = t->fields[s->a].ty->width;
                st = t->fields[s->a].ty;
            }
        } else {
            lo = s->a;
            w = s->b;
            st = w == 1 && ty_is_sized_int(t) ? el->S->t_bool : ty_bits(el->S, w);
        }
        sub = ce_sel_read(el, cur, s, at);
        r = ce_sel_update(el, sub, sels + 1, n - 1, nv, at);
        r = elab_convert(el, r, st, NULL, NULL, at);
        if (r.kind == VAL_ERROR)
            return r;
        if (val_is_const(&cur) && val_is_const(&r)) {
            BigInt b = {0}, x = {0};
            uint32_t i;
            IrBits bits;
            el_bits_of_const(el, &cur, t, &b);
            el_bits_of_const(el, &r, st, &x);
            for (i = 0; i < w; i++)
                bn_set_bit(&b, lo + i, bn_get_bit(&x, i));
            bits.width = t->width;
            bits.w = ANEWN(&el->arena, uint64_t, IR_LIMBS(t->width));
            bn_to_limbs(&b, bits.w, t->width);
            bn_free(&b);
            bn_free(&x);
            if (ty_is_sized_int(t))
                return elab_value_from_bits(el, &bits, t);
            return elab_value_from_bits(el, &bits, t);
        }
        if (!el->design)
            return ce_need_design(el, at);
        {
            IrExpr *ce = el_to_ir(el, &cur, at);
            IrExpr *parts[3];
            uint32_t k = 0;
            if (lo + w < t->width)
                parts[k++] = ir_slice(el->design->arena, ce, lo + w, t->width - lo - w, at);
            parts[k++] = el_to_ir(el, &r, at);
            if (lo > 0)
                parts[k++] = ir_slice(el->design->arena, ce, 0, lo, at);
            return el_hw(el, t, k == 1 ? parts[0] : ir_concat(el->design->arena, k, parts, at));
        }
    }
}

void el_assign_local(Elab *el, Place *p, Value v, Span at)
{
    Binding *b = p->bind;
    Value nv;
    if (b->val.kind == VAL_OPAQUE && p->nsel) {
        ce_sel_update(el, b->val, p->sels, p->nsel, v, at);
        return;
    }
    if (!b->is_mut) {
        el_error(el, at, "cannot assign to `%s`: `let` bindings are immutable", b->name);
        el_help(el, "declare it with `var` to assign it");
        return;
    }
    if (p->nsel == 0) {
        if (b->val.ty && b->val.ty->kind != TY_ERROR && b->val.kind != VAL_VOID && v.kind != VAL_ERROR) {
            Ty *t = b->val.ty;
            if (t->kind == TY_INT && v.ty && v.ty->kind == TY_INT)
                t = v.ty->is_signed ? v.ty : t;
            v = elab_convert(el, v, t, NULL, ce_what("`", b->name), at);
        }
        if (v.kind == VAL_ERROR)
            return;
        el_log_assign(el, b, NULL);
        b->val = el_maybe_temp(el, val_copy_deep(&el->arena, &v), at);
        return;
    }
    el_log_assign(el, b, NULL);
    nv = ce_sel_update(el, el->undo_depth ? val_copy_deep(&el->arena, &b->val) : b->val, p->sels, p->nsel, v, at);
    if (nv.kind != VAL_ERROR)
        b->val = el_maybe_temp(el, nv, at);
}

static void ce_bind_local_items(Elab *el, Stmt **stmts, uint32_t n, Env *env)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        Stmt *s = stmts[i];
        if (s->kind == STMT_ITEM && s->u.item->name.name && s->u.item->kind != ITEM_PROPERTY &&
            s->u.item->kind != ITEM_SEQUENCE) {
            Binding *b = elab_env_bind(el, env, s->u.item->name.name, val_void(el->S), s->u.item->name.span);
            b->lazy = 1;
            b->item = s->u.item;
            b->decl_env = env;
            b->sem = s->u.item->kind == ITEM_CONST ? SEM_CONST : s->u.item->kind == ITEM_FN ? SEM_FN : SEM_TYPE;
        }
    }
}

void el_exec_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env, Expr *tail, Value *tail_out)
{
    uint32_t i;
    Ty *texp = el->tail_expect;
    el->tail_expect = NULL;
    ce_bind_local_items(el, stmts, n, env);
    for (i = 0; i < n; i++) {
        Stmt *s = stmts[i];
        if (el->returned || el->limit_hit)
            break;
        if (tail_out && tail && s->kind == STMT_EXPR && s->u.expr == tail) {
            Expr *e = tail;
            if (!el_step(el, s->span))
                break;
            if (e->kind == EXPR_IF)
                *tail_out = ce_eval_if(el, e, env, texp, 1);
            else if (e->kind == EXPR_MATCH)
                *tail_out = ce_eval_match(el, e, env, texp, 1);
            else
                *tail_out = elab_eval(el, e, env, texp);
            continue;
        }
        el_exec_stmt(el, s, env);
    }
}

static int ce_iter_values(Elab *el, Value it, Span at, ValList *out)
{
    uint32_t i;
    if (it.kind == VAL_RANGE) {
        int64_t lo, hi;
        if (!val_get_i64(&it.u.list->v[0], &lo) || !val_get_i64(&it.u.list->v[1], &hi)) {
            el_error(el, at, "range bounds out of range");
            return 0;
        }
        if (it.aux)
            hi++;
        if (hi < lo)
            hi = lo;
        if (hi - lo > (int64_t)el->step_limit) {
            el_step(el, at);
            el->steps = el->step_limit;
            el_step(el, at);
            return 0;
        }
        out->n = (uint32_t)(hi - lo);
        out->v = ANEWN(&el->arena, Value, out->n ? out->n : 1);
        for (i = 0; i < out->n; i++) {
            out->v[i] = val_int(it.u.list->v[0].ty && it.u.list->v[0].ty->kind != TY_INT ? it.u.list->v[0].ty
                                                                                        : el->S->t_int,
                                lo + (int64_t)i);
        }
        return 1;
    }
    if (it.kind == VAL_ARRAY || it.kind == VAL_TUPLE) {
        *out = *it.u.list;
        return 1;
    }
    if (it.kind == VAL_HW && it.ty && it.ty->kind == TY_ARRAY) {
        out->n = it.ty->len;
        out->v = ANEWN(&el->arena, Value, out->n);
        for (i = 0; i < out->n; i++)
            out->v[i] = el_index(el, it, val_int(el->S->t_int, i), at, NULL);
        return 1;
    }
    if (it.kind == VAL_ERROR)
        return 0;
    if (it.kind == VAL_HW) {
        el_error(el, at, "`for` needs a compile-time range or array; this depends on signals");
        return 0;
    }
    el_error(el, at, "cannot iterate over a %s", it.ty ? ty_str(it.ty) : val_kind_name((ValKind)it.kind));
    return 0;
}

void el_exec_stmt(Elab *el, Stmt *s, Env *env)
{
    Sema *S = el->S;
    if (!el_step(el, s->span))
        return;
    switch (s->kind) {
    case STMT_ERROR:
        return;
    case STMT_LET: {
        Ty *t = s->u.let.type ? elab_eval_type(el, s->u.let.type, env) : NULL;
        Value v;
        int tries = 0;
        if (t && t->kind == TY_ERROR)
            t = NULL;
        for (;;) {
            Env *probe;
            Value c;
            v = elab_eval(el, s->u.let.init, env, t);
            if (t && v.kind != VAL_ERROR)
                v = elab_convert(el, v, t, s->u.let.init, s->u.let.pat->kind == PAT_BIND
                                     ? ce_what("`", s->u.let.pat->u.bind.name) : NULL,
                                 s->u.let.init->span);
            if (!s->u.let.constraint)
                break;
            probe = elab_env_new(el, env, NULL);
            el_bind_pattern(el, s->u.let.pat, v, probe, 0);
            c = elab_eval_const(el, s->u.let.constraint, probe, S->t_bool, "a `where` constraint");
            elab_env_free(probe);
            if (c.kind != VAL_BOOL || c.u.i || el->limit_hit)
                break;
            if (++tries >= 1000) {
                el_error(el, s->u.let.constraint->span, "`where` constraint not satisfied after 1000 tries");
                break;
            }
        }
        el_bind_pattern(el, s->u.let.pat, v, env, 0);
        return;
    }
    case STMT_VAR: {
        Ty *t = s->u.var.type ? elab_eval_type(el, s->u.var.type, env) : NULL;
        Value v;
        Binding *b;
        if (s->u.var.init) {
            v = elab_eval(el, s->u.var.init, env, t && t->kind != TY_ERROR ? t : NULL);
            if (t && t->kind != TY_ERROR && v.kind != VAL_ERROR)
                v = elab_convert(el, v, t, s->u.var.init, ce_what("`", s->u.var.name.name),
                                 s->u.var.init->span);
        } else if (t && t->kind != TY_ERROR) {
            v = el_zero(el, t);
        } else {
            if (!t)
                el_error(el, s->span, "`var %s` needs a type or an initial value", s->u.var.name.name);
            v = val_error();
        }
        if (v.kind == VAL_INT && v.ty && v.ty->kind == TY_INT && !t)
            v.ty = (v.flags & VAL_F_BIG ? bn_is_neg(v.u.big) : v.u.i < 0) ? S->t_sint : S->t_int;
        b = elab_env_bind(el, env, s->u.var.name.name, el_maybe_temp(el, val_copy_deep(&el->arena, &v), s->span),
                          s->u.var.name.span);
        b->is_mut = 1;
        b->sem = SEM_LOCAL;
        el_record_decl(el, s->u.var.name.span, s->u.var.name.name, SEM_LOCAL, v.ty, NULL, NULL);
        return;
    }
    case STMT_ASSIGN: {
        Place *p;
        Value v;
        Ty *hint = NULL;
        if (s->u.assign.constraint)
            el_error(el, s->u.assign.constraint->span, "`where` on an assignment is only allowed in tests");
        p = el_place_of(el, s->u.assign.lhs, env);
        if (!p) {
            elab_eval(el, s->u.assign.rhs, env, NULL);
            return;
        }
        if (p->kind == PL_LOCAL) {
            Value cur = el_read_place(el, p);
            hint = cur.ty;
        } else {
            hint = p->ty;
        }
        v = elab_eval(el, s->u.assign.rhs, env, hint && hint->kind != TY_ERROR ? hint : NULL);
        el_assign(el, p, v, s->u.assign.rhs, s->u.assign.lhs, s->span);
        return;
    }
    case STMT_EXPR: {
        Expr *e = s->u.expr;
        if (e->kind == EXPR_IF)
            ce_eval_if(el, e, env, NULL, 0);
        else if (e->kind == EXPR_MATCH)
            ce_eval_match(el, e, env, NULL, 0);
        else if (e->kind == EXPR_BLOCK)
            ce_exec_block_noval(el, e->u.block, env);
        else
            elab_eval(el, e, env, NULL);
        return;
    }
    case STMT_FOR: {
        Value it = elab_eval(el, s->u.for_stmt.iter, env, NULL);
        ValList vals;
        uint32_t i;
        if (!ce_iter_values(el, it, s->u.for_stmt.iter->span, &vals))
            return;
        for (i = 0; i < vals.n && !el->returned && !el->limit_hit; i++) {
            Env *ienv = elab_env_new(el, env, NULL);
            el_bind_pattern(el, s->u.for_stmt.pat, vals.v[i], ienv, 0);
            {
                Env *benv = elab_env_new(el, ienv, NULL);
                el_exec_stmts(el, s->u.for_stmt.body->stmts, s->u.for_stmt.body->nstmts, benv, NULL, NULL);
                elab_env_free(benv);
            }
            elab_env_free(ienv);
        }
        return;
    }
    case STMT_WHILE:
        for (;;) {
            Value c = elab_eval(el, s->u.while_stmt.cond, env, S->t_bool);
            Env *benv;
            if (c.kind == VAL_ERROR || el->limit_hit)
                return;
            if (c.kind != VAL_BOOL) {
                if (c.kind == VAL_HW)
                    el_error(el, s->u.while_stmt.cond->span,
                             "`while` condition depends on a signal: `while` only runs at compile time");
                else
                    el_error(el, s->u.while_stmt.cond->span, "`while` condition must be bool");
                if (c.kind == VAL_HW)
                    el_help(el, "use `for` over a compile-time range");
                return;
            }
            if (!c.u.i || el->returned)
                return;
            benv = elab_env_new(el, env, NULL);
            el_exec_stmts(el, s->u.while_stmt.body->stmts, s->u.while_stmt.body->nstmts, benv, NULL, NULL);
            elab_env_free(benv);
            if (el->returned || el->limit_hit)
                return;
        }
    case STMT_ASSERT: {
        Value c = elab_eval(el, s->u.assert_stmt.cond, env, S->t_bool);
        const char *msg = NULL;
        if (s->u.assert_stmt.msg) {
            Value m = elab_eval(el, s->u.assert_stmt.msg, env, NULL);
            if (m.kind == VAL_STRING)
                msg = arena_strndup(el->design ? el->design->arena : &el->arena, m.u.s, m.aux);
        }
        if (c.kind == VAL_ERROR)
            return;
        if (!c.ty || c.ty->kind != TY_BOOL) {
            el_error(el, s->u.assert_stmt.cond->span, "assertion condition must be bool");
            return;
        }
        if (el->hooks.check && !el->blk && el->hooks.check(el, s, c.kind == VAL_BOOL ? (int)c.u.i : -1, env))
            return;
        if (c.kind == VAL_HW) {
            if (!el->blk) {
                el_error(el, s->span, "`%s` on signals belongs in a `comb` or `on` block",
                         s->u.assert_stmt.kind == ASSERT_EXPECT ? "expect" : "assert");
                return;
            }
            ce_emit(el, ir_stmt_assert(el->design->arena,
                                       s->u.assert_stmt.kind == ASSERT_EXPECT ? IRA_EXPECT : IRA_ASSERT, c.u.e, msg,
                                       s->span));
            return;
        }
        if (!c.u.i) {
            if (msg)
                el_error(el, s->span, "assertion failed: %s", msg);
            else
                el_error(el, s->span, "assertion failed: `%s`", el_text(s->u.assert_stmt.cond));
        }
        return;
    }
    case STMT_STATIC_ASSERT: {
        Value c = elab_eval_const(el, s->u.static_assert.cond, env, S->t_bool, "a static_assert condition");
        if (c.kind == VAL_BOOL && !c.u.i) {
            Value m = s->u.static_assert.msg ? elab_eval_const(el, s->u.static_assert.msg, env, NULL, "a message")
                                             : val_error();
            if (m.kind == VAL_STRING)
                el_error(el, s->span, "static assertion failed: %.*s", (int)m.aux, m.u.s);
            else
                el_error(el, s->span, "static assertion failed: `%s`", el_text(s->u.static_assert.cond));
        } else if (c.kind != VAL_BOOL && c.kind != VAL_ERROR) {
            el_error(el, s->u.static_assert.cond->span, "static_assert needs a bool condition");
        }
        return;
    }
    case STMT_RETURN: {
        Value v;
        if (el->blk && !el->in_fn) {
            el_error(el, s->span, "`return` outside a function");
            return;
        }
        v = s->u.ret ? elab_eval(el, s->u.ret, env, el->fn_active ? el->fn_ret_ty : NULL) : val_void(S);
        if (el->fn_active && el->fn_ret_ty && v.kind != VAL_ERROR && v.kind != VAL_VOID)
            v = elab_convert(el, v, el->fn_ret_ty, s->u.ret, "the return type", s->u.ret ? s->u.ret->span : s->span);
        if (el->fn_active && (el->fn_ret_have || el->fn_path.kind != VAL_BOOL)) {
            /* returns under conditions on signals: the result is a mux over
             * the return points, the earliest one taking priority */
            if (v.kind == VAL_ERROR || v.kind == VAL_VOID) {
                if (v.kind == VAL_VOID)
                    el_error(el, s->span, "a `return` under a condition on signals needs a value");
                el->ret = val_error();
                el->returned = 1;
                return;
            }
            if (!el->fn_ret_have) {
                el->fn_ret_val = v;
                el->fn_ret_cond = el->fn_path;
                el->fn_ret_have = 1;
            } else if (el->fn_ret_val.kind != VAL_ERROR) {
                el->fn_ret_val = el_maybe_temp(el, el_unify_mux(el, el->fn_ret_cond, el->fn_ret_val, v, el->fn_ret_ty,
                                                                s->span), s->span);
                el->fn_ret_cond = el_or(el, el->fn_ret_cond, el->fn_path, s->span);
            }
            el->ret = el->fn_ret_val;
            el->returned = 1;
            return;
        }
        el->ret = v;
        el->returned = 1;
        return;
    }
    case STMT_ITEM:
        if (s->u.item->kind == ITEM_PROPERTY || s->u.item->kind == ITEM_SEQUENCE)
            el_error(el, s->span, "properties are declared at module level");
        return;
    case STMT_PORT: case STMT_DOMAIN: case STMT_REG: case STMT_MEM: case STMT_COMB: case STMT_ON: {
        static const char *const names[] = {"", "a port", "a domain", "", "", "a reg", "a mem", "", "a comb block",
                                            "an on block"};
        el_error(el, s->span, "%s is only allowed at module level", names[s->kind]);
        return;
    }
    }
}

/* ================================================================ eval */

static int ce_ctx_typed(Expr *e)
{
    while (e->kind == EXPR_PAREN)
        e = e->u.inner;
    return e->kind == EXPR_VARIANT || e->kind == EXPR_FILL;
}

static Value ce_eval_binary(Elab *el, Expr *e, Env *env)
{
    BinaryOp op = e->u.binary.op;
    Expr *lhs = e->u.binary.lhs, *rhs = e->u.binary.rhs;
    Value a, b;
    if (op == BIN_IN) {
        Value r = el_mk_bool(el, 0);
        uint32_t i;
        Expr *set = rhs;
        while (set->kind == EXPR_PAREN)
            set = set->u.inner;
        a = elab_eval(el, lhs, env, NULL);
        if (a.kind == VAL_ERROR)
            return a;
        if (set->kind == EXPR_RANGE) {
            Value lo = set->u.range.lo ? elab_eval(el, set->u.range.lo, env, NULL) : val_int(el->S->t_int, 0);
            Value hi = elab_eval(el, set->u.range.hi, env, NULL);
            r = el_and(el, el_binary(el, BIN_GE, a, lo, e->span, lhs, set->u.range.lo),
                       el_binary(el, set->u.range.inclusive ? BIN_LE : BIN_LT, a, hi, e->span, lhs, set->u.range.hi),
                       e->span);
            return r;
        }
        if (set->kind != EXPR_ARRAY) {
            Value arr = elab_eval(el, set, env, NULL);
            if (arr.kind == VAL_ARRAY) {
                for (i = 0; i < arr.u.list->n; i++)
                    r = el_or(el, r, el_binary(el, BIN_EQ, a, arr.u.list->v[i], e->span, lhs, NULL), e->span);
                return r;
            }
            el_error(el, rhs->span, "`in` needs a list `[a, b, ..]` or a range");
            return val_error();
        }
        for (i = 0; i < set->u.list.n; i++) {
            Value x = elab_eval(el, set->u.list.items[i], env, a.ty);
            r = el_or(el, r, el_binary(el, BIN_EQ, a, x, e->span, lhs, set->u.list.items[i]), e->span);
        }
        return r;
    }
    if (op == BIN_AND || op == BIN_OR || op == BIN_XOR) {
        a = elab_eval(el, lhs, env, el->S->t_bool);
        if (a.kind == VAL_BOOL && ((op == BIN_AND && !a.u.i) || (op == BIN_OR && a.u.i)))
            return a; /* short circuit: the other side may be invalid for this configuration */
        b = elab_eval(el, rhs, env, el->S->t_bool);
        return el_binary(el, op, a, b, e->span, lhs, rhs);
    }
    if (ce_ctx_typed(lhs) && !ce_ctx_typed(rhs)) {
        b = elab_eval(el, rhs, env, NULL);
        a = elab_eval(el, lhs, env, b.ty);
    } else {
        a = elab_eval(el, lhs, env, NULL);
        b = elab_eval(el, rhs, env, ce_ctx_typed(rhs) ? a.ty : NULL);
    }
    return el_binary(el, op, a, b, e->span, lhs, rhs);
}

Value elab_eval(Elab *el, Expr *e, Env *env, Ty *expect)
{
    Sema *S = el->S;
    if (!e)
        return val_error();
    if (!el_step(el, e->span))
        return val_error();
    switch (e->kind) {
    case EXPR_ERROR:
        return val_error();
    case EXPR_INT:
        return ce_int_literal(el, e, env, expect);
    case EXPR_BOOL:
        return el_mk_bool(el, e->u.bool_val);
    case EXPR_FILL:
        return el_fill(el, expect, e->u.fill_ones, e->span);
    case EXPR_STRING:
        return val_string(S, e->u.string.value, e->u.string.len);
    case EXPR_PATH: {
        Value v = ce_eval_path(el, &e->u.path, env, expect, 0);
        if (v.kind == VAL_TYPE && v.aux) {
            el_error(el, e->span, "variant `%s` needs its fields: `%s { .. }`", el_text(e), el_text(e));
            return val_error();
        }
        return v;
    }
    case EXPR_VARIANT:
        return ce_variant_literal(el, e, env, expect);
    case EXPR_STRUCT:
        return ce_struct_literal(el, e, env, expect);
    case EXPR_FIELD: {
        Value base = elab_eval(el, e->u.field.base, env, NULL);
        return el_field(el, base, e->u.field.name.name, e->u.field.name.span);
    }
    case EXPR_INDEX: {
        Value base = elab_eval(el, e->u.index.base, env, NULL);
        Value idx;
        if (base.kind == VAL_ERROR)
            return base;
        idx = elab_eval(el, e->u.index.index, env, NULL);
        return el_index(el, base, idx, e->span, e->u.index.index);
    }
    case EXPR_SLICE: {
        Value base = elab_eval(el, e->u.slice.base, env, NULL);
        return ce_slice(el, base, e->u.slice.hi, e->u.slice.lo, env, e->span);
    }
    case EXPR_CALL:
        if (e->u.call.domain && el->mod) {
            Value cv = e->u.call.callee->kind == EXPR_PATH ? ce_eval_path(el, &e->u.call.callee->u.path, env, NULL, 0)
                                                           : val_error();
            return el_instance(el, e, env, NULL, e->span, cv);
        }
        return ce_call(el, e, env, expect);
    case EXPR_CAST: {
        Ty *to = elab_eval_type(el, e->u.cast.type, env);
        Value v = elab_eval(el, e->u.cast.expr, env, NULL);
        if (to->kind == TY_ERROR)
            return val_error();
        return el_cast(el, v, to, e->span, e->u.cast.expr);
    }
    case EXPR_UNARY: {
        Expr *o = e->u.unary.operand;
        Value v;
        if (e->u.unary.op == UN_NEG && o->kind == EXPR_INT && o->u.int_lit.suffix) {
            Value lit = ce_int_literal(el, o, env, NULL);
            BigInt b = {0};
            Value r;
            if (lit.kind == VAL_ERROR)
                return lit;
            if (lit.ty->kind != TY_SINT) {
                el_error(el, e->span, "negative unsigned literal `%s`", el_text(e));
                return val_error();
            }
            val_get_bn(&lit, &b);
            bn_neg(&b, &b);
            r = val_int_bn(&el->arena, lit.ty, &b);
            bn_free(&b);
            return r;
        }
        v = elab_eval(el, o, env, e->u.unary.op == UN_NOT ? S->t_bool
                                  : (e->u.unary.op == UN_BITNOT && expect && ty_is_sized_int(expect)) ? expect : NULL);
        return ce_unary(el, e->u.unary.op, v, e->span, o);
    }
    case EXPR_BINARY:
        if (e->u.binary.op == BIN_UNTIL || e->u.binary.op == BIN_IMPLIES || e->u.binary.op == BIN_IMPLIES_NEXT) {
            el_error(el, e->span, "`%s` is only valid in a property", ce_op_text(e->u.binary.op));
            return val_error();
        }
        return ce_eval_binary(el, e, env);
    case EXPR_RANGE: {
        Value r = val_list(&el->arena, VAL_RANGE, NULL, 2);
        r.u.list->v[0] = e->u.range.lo ? elab_eval_const(el, e->u.range.lo, env, NULL, "a range bound")
                                       : val_int(S->t_int, 0);
        r.u.list->v[1] = elab_eval_const(el, e->u.range.hi, env, NULL, "a range bound");
        r.aux = (uint32_t)e->u.range.inclusive;
        if (r.u.list->v[0].kind == VAL_ERROR || r.u.list->v[1].kind == VAL_ERROR)
            return val_error();
        if (r.u.list->v[0].kind != VAL_INT || r.u.list->v[1].kind != VAL_INT) {
            el_error(el, e->span, "range bounds must be integers");
            return val_error();
        }
        return r;
    }
    case EXPR_IF:
        return ce_eval_if(el, e, env, expect, 1);
    case EXPR_MATCH:
        return ce_eval_match(el, e, env, expect, 1);
    case EXPR_BLOCK:
        return el_eval_block(el, e->u.block, env, expect);
    case EXPR_TUPLE: {
        uint32_t i, n = e->u.list.n;
        Value v = val_list(&el->arena, VAL_TUPLE, NULL, n);
        Ty **ts = (Ty **)xcalloc(n ? n : 1, sizeof(Ty *));
        int typed = 1;
        for (i = 0; i < n; i++) {
            Ty *h = expect && expect->kind == TY_TUPLE && i < expect->nfields ? expect->fields[i].ty : NULL;
            v.u.list->v[i] = elab_eval(el, e->u.list.items[i], env, h);
            if (v.u.list->v[i].kind == VAL_ERROR) {
                free(ts);
                return val_error();
            }
            ts[i] = v.u.list->v[i].ty;
            if (!ts[i])
                typed = 0;
        }
        if (typed)
            v.ty = ty_tuple(S, n, ts);
        free(ts);
        if (n == 0)
            return val_void(S);
        return v;
    }
    case EXPR_ARRAY:
        return ce_array_literal(el, e, env, expect);
    case EXPR_PAREN:
        return elab_eval(el, e->u.inner, env, expect);
    case EXPR_SIM:
        if (el->hooks.sim) {
            Value out = val_error();
            if (el->hooks.sim(el, e, env, &out))
                return out;
        }
        el_error(el, e->span, "`sim` is only allowed in tests");
        return val_error();
    case EXPR_TYPE: {
        Ty *t = elab_eval_type(el, e->u.type, env);
        return t->kind == TY_ERROR ? val_error() : val_type(S, t);
    }
    case EXPR_QPATH: {
        Ty *t = elab_eval_type(el, e->u.qpath.type, env);
        Value base;
        uint32_t i;
        if (t->kind == TY_ERROR)
            return val_error();
        base = val_type(S, t);
        for (i = 0; i < e->u.qpath.path.nsegs && base.kind != VAL_ERROR; i++) {
            PathSeg *s = &e->u.qpath.path.segs[i];
            if (!s->name.name)
                return val_error();
            base = el_member(el, base, s->name.name, s->name.span, env);
            if (s->has_args)
                base = el_apply_generics(el, base, s, env, i + 1 == e->u.qpath.path.nsegs ? expect : NULL);
        }
        return base;
    }
    case EXPR_TEMPORAL:
    case EXPR_DELAY:
        el_error(el, e->span, "temporal operators are only valid in a property");
        return val_error();
    }
    return val_error();
}

Value el_eval_path(Elab *el, Path *p, Env *env, Ty *expect, int type_ctx)
{
    return ce_eval_path(el, p, env, expect, type_ctx);
}

Value elab_eval_const(Elab *el, Expr *e, Env *env, Ty *expect, const char *what)
{
    Value v = elab_eval(el, e, env, expect);
    if (v.kind == VAL_ERROR)
        return v;
    if (v.kind == VAL_TYPE || v.kind == VAL_FN || v.kind == VAL_ITEM || v.kind == VAL_PACKAGE ||
        v.kind == VAL_MODULE || v.kind == VAL_OPAQUE)
        return v;
    if (!val_is_const(&v)) {
        el_error(el, e->span, "%s must be a compile-time value; `%s` depends on signals", what ? what : "this",
                 el_text(e));
        return val_error();
    }
    return v;
}

void elab_exec_block(Elab *el, Block *b, Env *env, Value *result)
{
    Env *benv = elab_env_new(el, env, NULL);
    Value tail = val_void(el->S);
    el_exec_stmts(el, b->stmts, b->nstmts, benv, b->tail, result ? &tail : NULL);
    if (el->returned) {
        tail = el->ret;
        el->returned = 0;
    }
    if (result)
        *result = tail;
    elab_env_free(benv);
}

Value elab_call(Elab *el, Value fn, const Value *args, const Ident *names, uint32_t nargs, Span at)
{
    if (fn.kind != VAL_FN || !fn.u.fn->item) {
        el_error(el, at, "not a function");
        return val_error();
    }
    return el_call_fn(el, fn.u.fn, args, names, NULL, nargs, at, NULL);
}

Binding *ce_lookup_local_binding(Env *e, const char *name)
{
    return e ? ce_env_find_local(e, name) : NULL;
}
