/* types.c — the Tern type representation: hash-consed concrete types,
 * printing, and conversion to the IR (see sema.h). The width rules of
 * Table 5.2 live with the operators in src/elab/consteval.c and elab.c,
 * which call the constructors here. */
#include "sema.h"

static Ty *ty_new(Sema *S, TyKind k, const char *key)
{
    Ty *t = ANEW(&S->arena, Ty);
    t->kind = k;
    t->key = key;
    map_put(&S->types, MAP_PTR(key), MAP_PTR(t));
    return t;
}

Ty *ty_find(Sema *S, const char *key)
{
    uint64_t v;
    if (map_get(&S->types, MAP_PTR(intern_cstr(key)), &v))
        return (Ty *)(uintptr_t)v;
    return NULL;
}

static Ty *ty_lookup_key(Sema *S, const char *key)
{
    uint64_t v;
    if (map_get(&S->types, MAP_PTR(key), &v))
        return (Ty *)(uintptr_t)v;
    return NULL;
}

static const char *ty_fmt(char *buf, size_t n, const char *fmt, const char *a, uint32_t w)
{
    snprintf(buf, n, fmt, a, (unsigned)w);
    return intern_cstr(buf);
}

Ty *ty_scalar(Sema *S, TyKind kind, uint32_t width)
{
    char buf[64];
    const char *key, *nm = "?";
    Ty *t;
    if (kind == TY_BOOL)
        width = 1;
    switch (kind) {
    case TY_BOOL: nm = "bool"; break;
    case TY_BIT: nm = "bit"; break;
    case TY_UINT: nm = "uint"; break;
    case TY_SINT: nm = "sint"; break;
    default: TERN_ASSERT(0);
    }
    if (kind == TY_BOOL)
        key = intern_cstr("bool");
    else
        key = ty_fmt(buf, sizeof buf, "%s<%u>", nm, width);
    t = ty_lookup_key(S, key);
    if (t)
        return t;
    t = ty_new(S, kind, key);
    t->width = width;
    t->is_signed = kind == TY_SINT;
    t->hw = width > 0;
    t->name = key;
    if (kind == TY_BOOL)
        t->mangle = key;
    else
        t->mangle = ty_fmt(buf, sizeof buf, "%s%u", kind == TY_UINT ? "u" : kind == TY_SINT ? "s" : "b",
                           width);
    return t;
}

Ty *ty_bool(Sema *S) { return S->t_bool ? S->t_bool : ty_scalar(S, TY_BOOL, 1); }
Ty *ty_bits(Sema *S, uint32_t width) { return ty_scalar(S, TY_BIT, width); }
Ty *ty_uint(Sema *S, uint32_t width) { return ty_scalar(S, TY_UINT, width); }
Ty *ty_sint(Sema *S, uint32_t width) { return ty_scalar(S, TY_SINT, width); }

static Ty *ty_simple(Sema *S, TyKind k, const char *name)
{
    const char *key = intern_cstr(name);
    Ty *t = ty_lookup_key(S, key);
    if (t)
        return t;
    t = ty_new(S, k, key);
    t->name = key;
    t->mangle = key;
    return t;
}

Ty *ty_int(Sema *S, int is_signed)
{
    Ty *t = ty_simple(S, TY_INT, is_signed ? "sint" : "uint");
    t->is_signed = is_signed ? 1 : 0;
    return t;
}

Ty *ty_clock(Sema *S, int falling)
{
    Ty *t = ty_simple(S, TY_CLOCK, falling ? "clock<falling>" : "clock");
    t->width = 1;
    t->hw = 1;
    t->falling = falling ? 1 : 0;
    t->mangle = intern_cstr(falling ? "clockn" : "clock");
    return t;
}

Ty *ty_reset(Sema *S, int async, int active_low)
{
    const char *nm = async ? (active_low ? "reset<async, low>" : "reset<async>")
                           : (active_low ? "reset<low>" : "reset");
    Ty *t = ty_simple(S, TY_RESET, nm);
    t->width = 1;
    t->hw = 1;
    t->async = async ? 1 : 0;
    t->active_low = active_low ? 1 : 0;
    t->mangle = intern_cstr(async ? (active_low ? "areset_n" : "areset") : (active_low ? "reset_n" : "reset"));
    return t;
}

Ty *ty_array(Sema *S, Ty *elem, uint32_t len)
{
    char buf[64];
    StrBuf sb = {0};
    const char *key;
    Ty *t;
    snprintf(buf, sizeof buf, "; %u]", (unsigned)len);
    sb_puts(&sb, "[");
    sb_puts(&sb, elem->key);
    sb_puts(&sb, buf);
    key = intern_cstr(sb_cstr(&sb));
    t = ty_lookup_key(S, key);
    if (t) {
        sb_free(&sb);
        return t;
    }
    t = ty_new(S, TY_ARRAY, key);
    t->elem = elem;
    t->len = len;
    t->hw = elem->hw && len > 0;
    t->width = t->hw ? elem->width * len : 0;
    sb_reset(&sb);
    sb_printf(&sb, "[%s; %u]", elem->name, (unsigned)len);
    t->name = intern_cstr(sb_cstr(&sb));
    sb_reset(&sb);
    sb_printf(&sb, "%sx%u", elem->mangle ? elem->mangle : "t", (unsigned)len);
    t->mangle = intern_cstr(sb_cstr(&sb));
    sb_free(&sb);
    return t;
}

Ty *ty_tuple(Sema *S, uint32_t n, Ty *const *elems)
{
    StrBuf sb = {0};
    const char *key;
    uint32_t i;
    Ty *t;
    sb_puts(&sb, "(");
    for (i = 0; i < n; i++) {
        if (i)
            sb_puts(&sb, ", ");
        sb_puts(&sb, elems[i]->key);
    }
    sb_puts(&sb, n == 1 ? ",)" : ")");
    key = intern_cstr(sb_cstr(&sb));
    t = ty_lookup_key(S, key);
    if (t) {
        sb_free(&sb);
        return t;
    }
    t = ty_new(S, n ? TY_TUPLE : TY_VOID, key);
    t->nfields = n;
    t->fields = ANEWN(&S->arena, TyField, n ? n : 1);
    for (i = 0; i < n; i++) {
        char nb[16];
        snprintf(nb, sizeof nb, "%u", (unsigned)i);
        t->fields[i].name = intern_cstr(nb);
        t->fields[i].ty = elems[i];
    }
    sb_reset(&sb);
    sb_puts(&sb, "(");
    for (i = 0; i < n; i++) {
        if (i)
            sb_puts(&sb, ", ");
        sb_puts(&sb, elems[i]->name);
    }
    sb_puts(&sb, ")");
    t->name = intern_cstr(sb_cstr(&sb));
    t->mangle = intern_cstr("tuple");
    sb_free(&sb);
    return t;
}

Ty *ty_tri(Sema *S, Ty *elem)
{
    StrBuf sb = {0};
    const char *key;
    Ty *t;
    sb_printf(&sb, "tri<%s>", elem->key);
    key = intern_cstr(sb_cstr(&sb));
    t = ty_lookup_key(S, key);
    if (!t) {
        t = ty_new(S, TY_TRI, key);
        t->elem = elem;
        t->width = elem->width;
        t->hw = elem->hw;
        sb_reset(&sb);
        sb_printf(&sb, "tri<%s>", elem->name);
        t->name = intern_cstr(sb_cstr(&sb));
        t->mangle = intern_cstr("tri");
    }
    sb_free(&sb);
    return t;
}

static Ty *ty_named(Sema *S, TyKind k, const char *key, const char *name, const char *emit_name,
                    Item *def, uint32_t nfields, const TyField *fields, Span loc)
{
    const char *ik = intern_cstr(key);
    Ty *t = ty_lookup_key(S, ik);
    uint32_t i, w = 0;
    int hw = 1;
    if (t)
        return t;
    t = ty_new(S, k, ik);
    t->name = intern_cstr(name);
    t->emit_name = emit_name ? intern_cstr(emit_name) : NULL;
    t->mangle = t->emit_name ? t->emit_name : intern_cstr("struct");
    t->def = def;
    t->loc = loc;
    t->nfields = nfields;
    t->fields = ANEWN(&S->arena, TyField, nfields ? nfields : 1);
    for (i = 0; i < nfields; i++) {
        t->fields[i] = fields[i];
        if (!fields[i].ty->hw)
            hw = 0;
        else
            w += fields[i].ty->width;
    }
    if (k == TY_STRUCT) {
        uint32_t lsb = w;
        for (i = 0; i < nfields; i++) {
            lsb -= t->fields[i].ty->width;
            t->fields[i].lsb = lsb;
        }
        t->hw = hw && w > 0;
        t->width = t->hw ? w : 0;
    }
    return t;
}

Ty *ty_struct(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
              uint32_t nfields, const TyField *fields, Span loc)
{
    return ty_named(S, TY_STRUCT, key, name, emit_name, def, nfields, fields, loc);
}

Ty *ty_bundle(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
              uint32_t nfields, const TyField *fields, Span loc)
{
    return ty_named(S, TY_BUNDLE, key, name, emit_name, def, nfields, fields, loc);
}

uint32_t ty_clog2(uint64_t n)
{
    uint32_t r = 0;
    while (r < 64 && ((uint64_t)1 << r) < n)
        r++;
    return r;
}

Ty *ty_enum(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
            uint32_t nvariants, const TyVariant *variants, IrEncoding enc, Span loc)
{
    const char *ik = intern_cstr(key);
    Ty *t = ty_lookup_key(S, ik);
    uint32_t i, pw = 0, tw;
    int hw = 1;
    if (t)
        return t;
    t = ty_new(S, TY_ENUM, ik);
    t->name = intern_cstr(name);
    t->emit_name = intern_cstr(emit_name ? emit_name : name);
    t->mangle = t->emit_name;
    t->def = def;
    t->loc = loc;
    t->enc = enc;
    t->nvariants = nvariants;
    t->variants = ANEWN(&S->arena, TyVariant, nvariants ? nvariants : 1);
    if (enc == IR_ENC_ONEHOT)
        tw = nvariants ? nvariants : 1;
    else
        tw = ty_clog2(nvariants);
    if (tw == 0)
        tw = 1;
    if (enc == IR_ENC_ONEHOT && tw > 64)
        enc = t->enc = IR_ENC_BINARY, tw = ty_clog2(nvariants);
    for (i = 0; i < nvariants; i++) {
        t->variants[i] = variants[i];
        if (enc == IR_ENC_ONEHOT)
            t->variants[i].tag = (uint64_t)1 << i;
        else if (enc == IR_ENC_GRAY)
            t->variants[i].tag = (uint64_t)i ^ ((uint64_t)i >> 1);
        else
            t->variants[i].tag = i;
        if (variants[i].payload && variants[i].payload->width > pw)
            pw = variants[i].payload->width;
        if (variants[i].payload && !variants[i].payload->hw)
            hw = 0;
    }
    t->tag_width = tw;
    t->payload_width = pw;
    /* a payload without a bit representation (widthless `uint`, arrays of
     * them: `Weighted<W: [uint; N]>`) makes a compile-time-only enum, usable
     * as a generic parameter or const but not as a signal */
    t->hw = hw;
    t->width = hw ? tw + pw : 0;
    return t;
}

void ty_print(const Ty *t, StrBuf *out)
{
    if (!t) {
        sb_puts(out, "?");
        return;
    }
    if (t->kind == TY_ERROR) {
        sb_puts(out, "{error}");
        return;
    }
    sb_puts(out, t->name ? t->name : "?");
}

const char *ty_str(const Ty *t)
{
    if (!t)
        return intern_cstr("?");
    if (t->kind == TY_ERROR)
        return intern_cstr("{error}");
    return t->name ? t->name : intern_cstr("?");
}

int ty_is_int(const Ty *t)
{
    return t && (t->kind == TY_BIT || t->kind == TY_UINT || t->kind == TY_SINT || t->kind == TY_INT);
}

int ty_is_sized_int(const Ty *t)
{
    return t && (t->kind == TY_BIT || t->kind == TY_UINT || t->kind == TY_SINT);
}

int ty_is_hw(const Ty *t)
{
    return t && t->hw;
}

int ty_has_bundle(const Ty *t)
{
    uint32_t i;
    if (!t)
        return 0;
    if (t->kind == TY_BUNDLE)
        return 1;
    if (t->kind == TY_ARRAY)
        return ty_has_bundle(t->elem);
    if (t->kind == TY_TUPLE) {
        for (i = 0; i < t->nfields; i++)
            if (ty_has_bundle(t->fields[i].ty))
                return 1;
    }
    return 0;
}

int ty_field_index(const Ty *t, const char *name)
{
    uint32_t i;
    if (!t)
        return -1;
    for (i = 0; i < t->nfields; i++)
        if (t->fields[i].name == name)
            return (int)i;
    return -1;
}

int ty_variant_index(const Ty *t, const char *name)
{
    uint32_t i;
    if (!t || t->kind != TY_ENUM)
        return -1;
    for (i = 0; i < t->nvariants; i++)
        if (t->variants[i].name == name)
            return (int)i;
    return -1;
}

/* ------------------------------------------------------------ to IR ---- */

static const char *ty_ir_unique_name(TyIrCache *c, const char *base)
{
    const char *nm = intern_cstr(base);
    char buf[256];
    int k = 2;
    while (map_has(&c->names, MAP_PTR(nm))) {
        snprintf(buf, sizeof buf, "%s_%d", base, k++);
        nm = intern_cstr(buf);
    }
    map_put(&c->names, MAP_PTR(nm), 1);
    return nm;
}

IrType *ty_to_ir(IrDesign *d, TyIrCache *c, const Ty *t)
{
    uint64_t v;
    IrType *r = NULL;
    uint32_t i;
    if (!t || !t->hw)
        return NULL;
    if (map_get(&c->map, MAP_PTR(t), &v))
        return (IrType *)(uintptr_t)v;
    switch (t->kind) {
    case TY_BOOL: r = ir_type_bool(d); break;
    case TY_BIT: r = ir_type_bits(d, t->width); break;
    case TY_UINT: r = ir_type_uint(d, t->width); break;
    case TY_SINT: r = ir_type_sint(d, t->width); break;
    case TY_CLOCK: r = ir_type_clock(d, t->falling); break;
    case TY_RESET: r = ir_type_reset(d, t->async, t->active_low); break;
    case TY_TRI: r = ty_to_ir(d, c, t->elem); break;
    case TY_ARRAY: {
        IrType *e = ty_to_ir(d, c, t->elem);
        if (e)
            r = ir_type_array(d, e, t->len);
        break;
    }
    case TY_STRUCT: {
        IrField *fs = (IrField *)xcalloc(t->nfields ? t->nfields : 1, sizeof(IrField));
        const char *nm = NULL;
        for (i = 0; i < t->nfields; i++) {
            fs[i].name = t->fields[i].name;
            fs[i].type = ty_to_ir(d, c, t->fields[i].ty);
            if (!fs[i].type) {
                free(fs);
                return NULL;
            }
        }
        if (t->emit_name)
            nm = ty_ir_unique_name(c, t->emit_name);
        r = ir_type_struct(d, nm, t->nfields, fs, t->loc);
        free(fs);
        break;
    }
    case TY_ENUM: {
        IrVariant *vs = (IrVariant *)xcalloc(t->nvariants ? t->nvariants : 1, sizeof(IrVariant));
        for (i = 0; i < t->nvariants; i++) {
            vs[i].name = t->variants[i].name;
            vs[i].tag = ir_bits_u64(d->arena, t->tag_width, t->variants[i].tag);
            vs[i].payload = t->variants[i].payload ? ty_to_ir(d, c, t->variants[i].payload) : NULL;
        }
        r = ir_type_enum(d, ty_ir_unique_name(c, t->emit_name ? t->emit_name : t->name), t->nvariants,
                         vs, t->tag_width, t->enc, t->loc);
        free(vs);
        break;
    }
    default: break;
    }
    if (r)
        map_put(&c->map, MAP_PTR(t), MAP_PTR(r));
    return r;
}

void ty_ir_cache_free(TyIrCache *c)
{
    map_free(&c->map);
    map_free(&c->names);
}
