/* value.c — Value constructors, comparison, copying and printing (value.h). */
#include "value.h"
#include "elab.h"

Value val_error(void)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_ERROR;
    return v;
}

Value val_void(Sema *S)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_VOID;
    v.ty = S->t_void;
    return v;
}

Value val_bool(Sema *S, int b)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_BOOL;
    v.ty = S->t_bool;
    v.u.i = b ? 1 : 0;
    return v;
}

Value val_int(Ty *ty, int64_t i)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_INT;
    v.ty = ty;
    v.u.i = i;
    return v;
}

Value val_int_bn(Arena *a, Ty *ty, const BigInt *b)
{
    Value v;
    int64_t i;
    memset(&v, 0, sizeof v);
    v.kind = VAL_INT;
    v.ty = ty;
    if (bn_get_i64(b, &i)) {
        v.u.i = i;
        return v;
    }
    /* arena-backed and read-only: never bn_free'd or mutated */
    v.flags = VAL_F_BIG;
    v.u.big = ANEW(a, BigInt);
    v.u.big->len = b->len;
    v.u.big->cap = b->len;
    v.u.big->neg = b->neg;
    v.u.big->d = (uint32_t *)arena_memdup(a, b->d, (b->len ? b->len : 1) * sizeof(uint32_t));
    return v;
}

Value val_string(Sema *S, const char *s, uint32_t len)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_STRING;
    v.ty = S->t_string;
    v.u.s = s;
    v.aux = len;
    return v;
}

Value val_type(Sema *S, Ty *t)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_TYPE;
    v.ty = S->t_type;
    v.u.type = t;
    return v;
}

Value val_list(Arena *a, ValKind kind, Ty *ty, uint32_t n)
{
    Value v;
    uint32_t i;
    memset(&v, 0, sizeof v);
    v.kind = (uint8_t)kind;
    v.ty = ty;
    v.u.list = ANEW(a, ValList);
    v.u.list->n = n;
    v.u.list->v = ANEWN(a, Value, n ? n : 1);
    for (i = 0; i < n; i++)
        v.u.list->v[i].kind = VAL_VOID;
    return v;
}

Value val_enum(Arena *a, Ty *ty, uint32_t variant, const ValList *payload)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_ENUM;
    v.ty = ty;
    v.aux = variant;
    if (payload) {
        v.u.list = ANEW(a, ValList);
        v.u.list->n = payload->n;
        v.u.list->v = (Value *)arena_memdup(a, payload->v, sizeof(Value) * (payload->n ? payload->n : 1));
    }
    return v;
}

Value val_hw(Ty *ty, IrExpr *e)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_HW;
    v.ty = ty;
    v.u.e = e;
    return v;
}

Value val_opaque(Arena *a, const ValOpaqueOps *ops, void *obj, Ty *ty)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.kind = VAL_OPAQUE;
    v.ty = ty;
    v.u.opq = ANEW(a, ValOpaque);
    v.u.opq->ops = ops;
    v.u.opq->obj = obj;
    return v;
}

int val_is_const(const Value *v)
{
    uint32_t i;
    switch (v->kind) {
    case VAL_INT: case VAL_BOOL: case VAL_STRING: case VAL_TYPE: case VAL_VOID:
        return 1;
    case VAL_ENUM:
        if (!v->u.list)
            return 1;
        /* fallthrough */
    case VAL_STRUCT: case VAL_ARRAY: case VAL_TUPLE: case VAL_RANGE:
        for (i = 0; i < v->u.list->n; i++)
            if (!val_is_const(&v->u.list->v[i]))
                return 0;
        return 1;
    default:
        return 0;
    }
}

int val_is_int(const Value *v)
{
    return v->kind == VAL_INT;
}

void val_get_bn(const Value *v, BigInt *r)
{
    if (v->kind == VAL_INT && (v->flags & VAL_F_BIG))
        bn_copy(r, v->u.big);
    else if (v->kind == VAL_INT || v->kind == VAL_BOOL)
        bn_set_i64(r, v->u.i);
    else if (v->kind == VAL_ENUM && v->ty && v->ty->kind == TY_ENUM)
        bn_set_u64(r, v->ty->variants[v->aux].tag);
    else
        bn_set_i64(r, 0);
}

int val_get_i64(const Value *v, int64_t *out)
{
    if (v->kind == VAL_INT) {
        if (v->flags & VAL_F_BIG)
            return bn_get_i64(v->u.big, out);
        *out = v->u.i;
        return 1;
    }
    return 0;
}

int val_get_u64(const Value *v, uint64_t *out)
{
    if (v->kind == VAL_INT) {
        if (v->flags & VAL_F_BIG)
            return bn_get_u64(v->u.big, out);
        if (v->u.i < 0)
            return 0;
        *out = (uint64_t)v->u.i;
        return 1;
    }
    return 0;
}

int val_truthy(const Value *v)
{
    if (v->kind == VAL_BOOL || v->kind == VAL_INT)
        return v->u.i != 0 || ((v->flags & VAL_F_BIG) != 0);
    return 0;
}

int val_equal(const Value *a, const Value *b)
{
    uint32_t i;
    if (a->kind == VAL_INT && b->kind == VAL_INT) {
        if (!(a->flags & VAL_F_BIG) && !(b->flags & VAL_F_BIG))
            return a->u.i == b->u.i;
        {
            BigInt x = {0}, y = {0};
            int r;
            val_get_bn(a, &x);
            val_get_bn(b, &y);
            r = bn_cmp(&x, &y) == 0;
            bn_free(&x);
            bn_free(&y);
            return r;
        }
    }
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case VAL_BOOL: return a->u.i == b->u.i;
    case VAL_STRING: return a->aux == b->aux && memcmp(a->u.s, b->u.s, a->aux) == 0;
    case VAL_TYPE: return a->u.type == b->u.type;
    case VAL_VOID: return 1;
    case VAL_ENUM:
        if (a->aux != b->aux)
            return 0;
        if (!a->u.list || !b->u.list)
            return a->u.list == b->u.list;
        /* fallthrough */
    case VAL_STRUCT: case VAL_ARRAY: case VAL_TUPLE: case VAL_RANGE:
        if (a->u.list->n != b->u.list->n)
            return 0;
        for (i = 0; i < a->u.list->n; i++)
            if (!val_equal(&a->u.list->v[i], &b->u.list->v[i]))
                return 0;
        return a->kind != VAL_RANGE || a->aux == b->aux;
    case VAL_PACKAGE: return a->u.pkg == b->u.pkg;
    default: return 0;
    }
}

Value val_copy_deep(Arena *a, const Value *v)
{
    Value r = *v;
    uint32_t i;
    switch (v->kind) {
    case VAL_ENUM:
        if (!v->u.list)
            return r;
        /* fallthrough */
    case VAL_STRUCT: case VAL_ARRAY: case VAL_TUPLE: case VAL_BUNDLE:
        r.u.list = ANEW(a, ValList);
        r.u.list->n = v->u.list->n;
        r.u.list->v = ANEWN(a, Value, v->u.list->n ? v->u.list->n : 1);
        for (i = 0; i < v->u.list->n; i++)
            r.u.list->v[i] = val_copy_deep(a, &v->u.list->v[i]);
        return r;
    default:
        return r;
    }
}

static void val_print_list(const ValList *l, StrBuf *out, const char *open, const char *close)
{
    uint32_t i;
    sb_puts(out, open);
    for (i = 0; l && i < l->n; i++) {
        if (i)
            sb_puts(out, ", ");
        val_print(&l->v[i], out);
    }
    sb_puts(out, close);
}

void val_print(const Value *v, StrBuf *out)
{
    uint32_t i;
    switch (v->kind) {
    case VAL_ERROR: sb_puts(out, "{error}"); break;
    case VAL_VOID: sb_puts(out, "()"); break;
    case VAL_INT:
        if (v->flags & VAL_F_BIG)
            bn_to_sb(out, v->u.big, 10);
        else
            sb_printf(out, "%lld", (long long)v->u.i);
        break;
    case VAL_BOOL: sb_puts(out, v->u.i ? "true" : "false"); break;
    case VAL_STRING:
        if (v->flags & VAL_F_VARIANT) {
            sb_putc(out, '.');
            sb_append(out, v->u.s, v->aux);
            break;
        }
        sb_putc(out, '"');
        sb_append(out, v->u.s, v->aux);
        sb_putc(out, '"');
        break;
    case VAL_ENUM:
        sb_printf(out, ".%s", v->ty && v->ty->kind == TY_ENUM ? v->ty->variants[v->aux].name : "?");
        if (v->u.list) {
            Ty *p = v->ty->variants[v->aux].payload;
            sb_puts(out, " { ");
            for (i = 0; i < v->u.list->n; i++) {
                if (i)
                    sb_puts(out, ", ");
                if (p && i < p->nfields)
                    sb_printf(out, "%s: ", p->fields[i].name);
                val_print(&v->u.list->v[i], out);
            }
            sb_puts(out, " }");
        }
        break;
    case VAL_STRUCT:
    case VAL_BUNDLE:
        sb_printf(out, "%s { ", v->ty ? ty_str(v->ty) : "?");
        for (i = 0; i < v->u.list->n; i++) {
            if (i)
                sb_puts(out, ", ");
            if (v->ty && i < v->ty->nfields)
                sb_printf(out, "%s: ", v->ty->fields[i].name);
            val_print(&v->u.list->v[i], out);
        }
        sb_puts(out, " }");
        break;
    case VAL_ARRAY: val_print_list(v->u.list, out, "[", "]"); break;
    case VAL_TUPLE: val_print_list(v->u.list, out, "(", ")"); break;
    case VAL_TYPE: ty_print(v->u.type, out); break;
    case VAL_RANGE:
        val_print(&v->u.list->v[0], out);
        sb_puts(out, v->aux ? "..=" : "..");
        val_print(&v->u.list->v[1], out);
        break;
    case VAL_FN: case VAL_ITEM:
        sb_puts(out, v->u.fn && v->u.fn->item && v->u.fn->item->name.name ? v->u.fn->item->name.name : "fn");
        break;
    case VAL_PACKAGE: sb_puts(out, v->u.pkg->path); break;
    case VAL_HW: sb_printf(out, "<signal: %s>", ty_str(v->ty)); break;
    case VAL_INST: sb_puts(out, "<instance>"); break;
    case VAL_MEM: sb_puts(out, "<mem>"); break;
    case VAL_DOMAIN: sb_puts(out, "<domain>"); break;
    case VAL_MODULE: sb_puts(out, "<module>"); break;
    case VAL_OPAQUE:
        if (v->u.opq->ops->to_string)
            v->u.opq->ops->to_string(v->u.opq->obj, out);
        else
            sb_printf(out, "<%s>", v->u.opq->ops->type_name);
        break;
    default: sb_puts(out, "?"); break;
    }
}

void val_mangle(const Value *v, StrBuf *out)
{
    uint32_t i;
    switch (v->kind) {
    case VAL_INT:
        if (v->flags & VAL_F_BIG) {
            if (bn_is_neg(v->u.big)) {
                BigInt t = {0};
                bn_neg(&t, v->u.big);
                sb_putc(out, 'n');
                bn_to_sb(out, &t, 10);
                bn_free(&t);
            } else {
                bn_to_sb(out, v->u.big, 10);
            }
        } else if (v->u.i < 0) {
            sb_printf(out, "n%llu", (unsigned long long)(-(v->u.i + 1)) + 1ull);
        } else {
            sb_printf(out, "%lld", (long long)v->u.i);
        }
        break;
    case VAL_BOOL: sb_puts(out, v->u.i ? "1" : "0"); break;
    case VAL_TYPE: sb_puts(out, v->u.type->mangle ? v->u.type->mangle : "t"); break;
    case VAL_ENUM:
        sb_puts(out, v->ty && v->ty->kind == TY_ENUM ? v->ty->variants[v->aux].name : "e");
        if (v->u.list)
            for (i = 0; i < v->u.list->n; i++) {
                sb_putc(out, '_');
                val_mangle(&v->u.list->v[i], out);
            }
        break;
    case VAL_STRUCT: case VAL_ARRAY: case VAL_TUPLE:
        for (i = 0; i < v->u.list->n; i++) {
            if (i)
                sb_putc(out, '_');
            val_mangle(&v->u.list->v[i], out);
        }
        if (v->u.list->n == 0)
            sb_puts(out, "e");
        break;
    case VAL_STRING: {
        uint32_t k;
        for (k = 0; k < v->aux; k++)
            sb_putc(out, tern_is_ident((unsigned char)v->u.s[k]) ? v->u.s[k] : '_');
        break;
    }
    default: sb_puts(out, "x"); break;
    }
}

const char *val_kind_name(ValKind k)
{
    static const char *const names[] = {
        "error", "()", "integer", "bool", "string", "enum value", "struct value", "array",
        "tuple", "type", "range", "function", "item", "package", "signal", "bundle",
        "instance", "memory", "domain", "module", "object"};
    if ((size_t)k < TERN_ARRAY_LEN(names))
        return names[k];
    return "?";
}
