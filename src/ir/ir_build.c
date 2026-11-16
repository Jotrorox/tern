/* ir_build.c — IR construction helpers, lookups and the textual dump.
 *
 * Arrays inside IrModule/IrDesign grow in the IR arena by doubling: an
 * array of n elements has capacity irb_cap(n) (0, 4, 8, 16, ...), so appending
 * at index n needs a new allocation exactly when n == 0 or n is a power of
 * two >= 4.  No capacity field is needed and the IR stays a plain arena
 * structure.
 */
#include "ir.h"
#include "../base/str.h"
#include "../base/map.h"

/* ------------------------------------------------------------ helpers -- */

uint32_t irb_cap(uint32_t n);
void *irb_grow(Arena *a, void *arr, uint32_t n, size_t elem);

/* capacity of an array holding n elements (shared with ir_json.c) */
uint32_t irb_cap(uint32_t n)
{
    uint32_t c = 4;
    if (n == 0)
        return 0;
    while (c < n)
        c *= 2;
    return c;
}

/* make room for element index n in an array of n elements */
void *irb_grow(Arena *a, void *arr, uint32_t n, size_t elem)
{
    void *na;
    if (n < irb_cap(n))
        return arr;
    na = arena_zalloc(a, elem * (size_t)(n == 0 ? 4 : n * 2));
    if (n)
        memcpy(na, arr, elem * n);
    return na;
}

#define IRB_PUSH(arena, arr, n, T) \
    ((arr) = (T *)irb_grow((arena), (arr), (n), sizeof(T)), &(arr)[(n)++])

static const char *irb_intern(const char *s)
{
    return s ? intern_cstr(s) : NULL;
}

/* --------------------------------------------------------------- bits -- */

IrDesign *ir_design_new(Arena *a)
{
    IrDesign *d = ANEW(a, IrDesign);
    d->arena = a;
    return d;
}

IrBits ir_bits_zero(Arena *a, uint32_t width)
{
    IrBits b;
    b.width = width;
    b.w = ANEWN(a, uint64_t, IR_LIMBS(width) ? IR_LIMBS(width) : 1);
    return b;
}

static uint64_t irb_top_mask(uint32_t width)
{
    return (width % 64) ? ((UINT64_C(1) << (width % 64)) - 1u) : ~UINT64_C(0);
}

IrBits ir_bits_u64(Arena *a, uint32_t width, uint64_t v)
{
    IrBits b = ir_bits_zero(a, width);
    if (width) {
        b.w[0] = v;
        if (width < 64)
            b.w[0] &= (UINT64_C(1) << width) - 1u;
    }
    return b;
}

IrBits ir_bits_ones(Arena *a, uint32_t width)
{
    IrBits b = ir_bits_zero(a, width);
    uint32_t i, n = IR_LIMBS(width);
    for (i = 0; i < n; i++)
        b.w[i] = ~UINT64_C(0);
    if (n)
        b.w[n - 1] &= irb_top_mask(width);
    return b;
}

IrBits ir_bits_copy(Arena *a, const IrBits *b)
{
    IrBits r = ir_bits_zero(a, b->width);
    if (b->width && b->w)
        memcpy(r.w, b->w, sizeof(uint64_t) * IR_LIMBS(b->width));
    return r;
}

IrBits ir_bits_from_limbs(Arena *a, uint32_t width, const uint64_t *w)
{
    IrBits r = ir_bits_zero(a, width);
    uint32_t n = IR_LIMBS(width);
    if (n) {
        memcpy(r.w, w, sizeof(uint64_t) * n);
        r.w[n - 1] &= irb_top_mask(width);
    }
    return r;
}

int ir_bits_is_zero(const IrBits *b)
{
    uint32_t i, n = IR_LIMBS(b->width);
    for (i = 0; i < n; i++)
        if (b->w[i])
            return 0;
    return 1;
}

uint64_t ir_bits_low64(const IrBits *b)
{
    return b->width ? b->w[0] : 0;
}

int ir_bits_get(const IrBits *b, uint32_t bit)
{
    if (bit >= b->width)
        return 0;
    return (int)((b->w[bit / 64] >> (bit % 64)) & 1u);
}

void ir_bits_set(IrBits *b, uint32_t bit, int v)
{
    uint64_t m;
    if (bit >= b->width)
        return;
    m = UINT64_C(1) << (bit % 64);
    if (v)
        b->w[bit / 64] |= m;
    else
        b->w[bit / 64] &= ~m;
}

int ir_bits_eq(const IrBits *a, const IrBits *b)
{
    uint32_t i, n;
    if (a->width != b->width)
        return 0;
    n = IR_LIMBS(a->width);
    for (i = 0; i < n; i++)
        if (a->w[i] != b->w[i])
            return 0;
    return 1;
}

IrBits ir_bits_resize(Arena *a, const IrBits *b, uint32_t width, int sign_extend)
{
    IrBits r = ir_bits_zero(a, width);
    uint32_t i;
    int sign = sign_extend && b->width && ir_bits_get(b, b->width - 1);
    for (i = 0; i < width; i++) {
        int bit = i < b->width ? ir_bits_get(b, i) : sign;
        if (bit)
            r.w[i / 64] |= UINT64_C(1) << (i % 64);
    }
    return r;
}

void ir_bits_print(const IrBits *b, StrBuf *out)
{
    uint32_t ndig = (b->width + 3) / 4, i;
    int started = 0;
    sb_printf(out, "%u'h", b->width);
    if (ndig == 0) {
        sb_putc(out, '0');
        return;
    }
    for (i = ndig; i-- > 0;) {
        uint32_t bit = i * 4;
        unsigned nib = (unsigned)((b->w[bit / 64] >> (bit % 64)) & 0xFu);
        if (!started && nib == 0 && i != 0)
            continue;
        started = 1;
        sb_putc(out, "0123456789abcdef"[nib]);
    }
}

int ir_bits_parse(Arena *a, const char *s, size_t n, IrBits *out)
{
    size_t i = 0;
    uint64_t width = 0;
    char base;
    IrBits r;
    int any = 0;
    while (i < n && tern_is_digit((unsigned char)s[i])) {
        width = width * 10 + (uint64_t)(s[i] - '0');
        if (width > 0xFFFFFFFFu)
            return 0;
        i++;
        any = 1;
    }
    if (!any || width == 0 || i + 2 > n || s[i] != '\'')
        return 0;
    base = (char)tern_to_lower((unsigned char)s[i + 1]);
    i += 2;
    if (i >= n)
        return 0;
    r = ir_bits_zero(a, (uint32_t)width);
    if (base == 'h' || base == 'b') {
        unsigned per = base == 'h' ? 4u : 1u;
        size_t j;
        uint64_t pos = 0;
        for (j = n; j-- > i;) {
            int dv;
            unsigned k;
            if (s[j] == '_')
                continue;
            dv = base == 'h' ? tern_hex_value((unsigned char)s[j])
                             : (s[j] == '0' ? 0 : s[j] == '1' ? 1 : -1);
            if (dv < 0)
                return 0;
            for (k = 0; k < per; k++, pos++)
                if ((dv >> k) & 1) {
                    if (pos >= width)
                        return 0;
                    r.w[pos / 64] |= UINT64_C(1) << (pos % 64);
                }
        }
    } else if (base == 'd') {
        /* decimal: r = r*10 + d over limbs */
        uint32_t nl = IR_LIMBS((uint32_t)width), k;
        size_t j;
        for (j = i; j < n; j++) {
            uint64_t carry;
            if (s[j] == '_')
                continue;
            if (!tern_is_digit((unsigned char)s[j]))
                return 0;
            carry = (uint64_t)(s[j] - '0');
            for (k = 0; k < nl; k++) {
                uint64_t lo = (r.w[k] & 0xFFFFFFFFu) * 10u + carry;
                uint64_t hi = (r.w[k] >> 32) * 10u + (lo >> 32);
                r.w[k] = (lo & 0xFFFFFFFFu) | (hi << 32);
                carry = hi >> 32;
            }
            if (carry || (r.w[nl - 1] & ~irb_top_mask((uint32_t)width)))
                return 0;
        }
    } else {
        return 0;
    }
    *out = r;
    return 1;
}

/* -------------------------------------------------------------- types -- */

typedef struct IrBuildState {
    IrType **slots;
    uint32_t cap, n;
} IrBuildState;

static uint64_t irb_type_hash(const IrType *t)
{
    uint64_t h = hash_u64((uint64_t)t->kind + 1);
    uint32_t i;
    h = hash_combine(h, t->width);
    h = hash_combine(h, MAP_PTR(t->elem));
    h = hash_combine(h, t->len);
    h = hash_combine(h, (uint64_t)(t->falling * 4 + t->async * 2 + t->active_low));
    for (i = 0; i < t->nfields; i++) {
        h = hash_combine(h, MAP_PTR(t->fields[i].name));
        h = hash_combine(h, MAP_PTR(t->fields[i].type));
    }
    for (i = 0; i < t->nvariants; i++) {
        h = hash_combine(h, MAP_PTR(t->variants[i].name));
        h = hash_combine(h, MAP_PTR(t->variants[i].payload));
        h = hash_combine(h, ir_bits_low64(&t->variants[i].tag));
    }
    h = hash_combine(h, t->tag_width * 8u + (uint32_t)t->encoding);
    return h;
}

static int irb_type_same(const IrType *a, const IrType *b)
{
    uint32_t i;
    if (a->kind != b->kind || a->width != b->width || a->elem != b->elem || a->len != b->len ||
        a->falling != b->falling || a->async != b->async || a->active_low != b->active_low ||
        a->nfields != b->nfields || a->nvariants != b->nvariants || a->tag_width != b->tag_width ||
        a->encoding != b->encoding || a->name != b->name)
        return 0;
    for (i = 0; i < a->nfields; i++)
        if (a->fields[i].name != b->fields[i].name || a->fields[i].type != b->fields[i].type ||
            a->fields[i].lsb != b->fields[i].lsb)
            return 0;
    for (i = 0; i < a->nvariants; i++)
        if (a->variants[i].name != b->variants[i].name ||
            a->variants[i].payload != b->variants[i].payload ||
            !ir_bits_eq(&a->variants[i].tag, &b->variants[i].tag))
            return 0;
    return 1;
}

/* Return the canonical copy of the anonymous type *proto (copied into the
 * arena on first use). */
static IrType *irb_type_intern(IrDesign *d, const IrType *proto)
{
    IrBuildState *st = d->build;
    uint64_t h;
    uint32_t i;
    IrType *t;
    if (!st) {
        st = ANEW(d->arena, IrBuildState);
        st->cap = 64;
        st->slots = ANEWN(d->arena, IrType *, st->cap);
        d->build = st;
    }
    h = irb_type_hash(proto);
    for (i = (uint32_t)h & (st->cap - 1); st->slots[i]; i = (i + 1) & (st->cap - 1))
        if (irb_type_same(st->slots[i], proto))
            return st->slots[i];
    t = (IrType *)arena_memdup(d->arena, proto, sizeof(IrType));
    st->slots[i] = t;
    st->n++;
    if (st->n * 2 > st->cap) {
        uint32_t ncap = st->cap * 2, j;
        IrType **ns = ANEWN(d->arena, IrType *, ncap);
        for (j = 0; j < st->cap; j++) {
            IrType *x = st->slots[j];
            uint32_t k;
            if (!x)
                continue;
            for (k = (uint32_t)irb_type_hash(x) & (ncap - 1); ns[k]; k = (k + 1) & (ncap - 1)) {
            }
            ns[k] = x;
        }
        st->slots = ns;
        st->cap = ncap;
    }
    return t;
}

static IrType *irb_scalar(IrDesign *d, IrTypeKind k, uint32_t width)
{
    IrType p;
    TERN_ASSERT(width > 0);
    memset(&p, 0, sizeof p);
    p.kind = k;
    p.width = width;
    return irb_type_intern(d, &p);
}

IrType *ir_type_bool(IrDesign *d) { return irb_scalar(d, IRT_BOOL, 1); }
IrType *ir_type_bits(IrDesign *d, uint32_t width) { return irb_scalar(d, IRT_BITS, width); }
IrType *ir_type_uint(IrDesign *d, uint32_t width) { return irb_scalar(d, IRT_UINT, width); }
IrType *ir_type_sint(IrDesign *d, uint32_t width) { return irb_scalar(d, IRT_SINT, width); }

IrType *ir_type_clock(IrDesign *d, int falling)
{
    IrType p;
    memset(&p, 0, sizeof p);
    p.kind = IRT_CLOCK;
    p.width = 1;
    p.falling = falling ? 1 : 0;
    return irb_type_intern(d, &p);
}

IrType *ir_type_reset(IrDesign *d, int async, int active_low)
{
    IrType p;
    memset(&p, 0, sizeof p);
    p.kind = IRT_RESET;
    p.width = 1;
    p.async = async ? 1 : 0;
    p.active_low = active_low ? 1 : 0;
    return irb_type_intern(d, &p);
}

IrType *ir_type_array(IrDesign *d, IrType *elem, uint32_t len)
{
    IrType p;
    TERN_ASSERT(elem && len > 0);
    memset(&p, 0, sizeof p);
    p.kind = IRT_ARRAY;
    p.elem = elem;
    p.len = len;
    p.width = elem->width * len;
    return irb_type_intern(d, &p);
}

static void irb_add_named_type(IrDesign *d, IrType *t)
{
    *IRB_PUSH(d->arena, d->types, d->ntypes, IrType *) = t;
}

IrType *ir_type_struct(IrDesign *d, const char *name, uint32_t nfields, const IrField *fields,
                       Span loc)
{
    IrType p;
    IrField *fs = ANEWN(d->arena, IrField, nfields ? nfields : 1);
    uint32_t i, w = 0;
    for (i = nfields; i-- > 0;) {
        fs[i].name = irb_intern(fields[i].name);
        fs[i].type = fields[i].type;
        fs[i].lsb = w;
        TERN_ASSERT(fields[i].type != NULL);
        w += fields[i].type->width;
    }
    memset(&p, 0, sizeof p);
    p.kind = IRT_STRUCT;
    p.width = w;
    p.name = irb_intern(name);
    p.nfields = nfields;
    p.fields = fs;
    p.loc = loc;
    TERN_ASSERT(w > 0);
    if (name) {
        IrType *t = (IrType *)arena_memdup(d->arena, &p, sizeof p);
        irb_add_named_type(d, t);
        return t;
    }
    return irb_type_intern(d, &p);
}

IrType *ir_type_enum(IrDesign *d, const char *name, uint32_t nvariants, const IrVariant *variants,
                     uint32_t tag_width, IrEncoding enc, Span loc)
{
    IrType p;
    IrVariant *vs = ANEWN(d->arena, IrVariant, nvariants ? nvariants : 1);
    uint32_t i, maxp = 0;
    for (i = 0; i < nvariants; i++) {
        vs[i].name = irb_intern(variants[i].name);
        vs[i].tag = ir_bits_resize(d->arena, &variants[i].tag, tag_width, 0);
        vs[i].payload = variants[i].payload;
        if (vs[i].payload && vs[i].payload->width > maxp)
            maxp = vs[i].payload->width;
    }
    memset(&p, 0, sizeof p);
    p.kind = IRT_ENUM;
    p.width = tag_width + maxp;
    p.name = irb_intern(name);
    p.nvariants = nvariants;
    p.variants = vs;
    p.tag_width = tag_width;
    p.encoding = enc;
    p.loc = loc;
    TERN_ASSERT(p.width > 0);
    if (name) {
        IrType *t = (IrType *)arena_memdup(d->arena, &p, sizeof p);
        irb_add_named_type(d, t);
        return t;
    }
    return irb_type_intern(d, &p);
}

int ir_type_is_signed(const IrType *t)
{
    return t && t->kind == IRT_SINT;
}

const IrField *ir_type_field(const IrType *t, const char *name)
{
    uint32_t i;
    if (!t || t->kind != IRT_STRUCT || !name)
        return NULL;
    for (i = 0; i < t->nfields; i++)
        if (tern_streq(t->fields[i].name, name))
            return &t->fields[i];
    return NULL;
}

const IrVariant *ir_type_variant(const IrType *t, const char *name)
{
    uint32_t i;
    if (!t || t->kind != IRT_ENUM || !name)
        return NULL;
    for (i = 0; i < t->nvariants; i++)
        if (tern_streq(t->variants[i].name, name))
            return &t->variants[i];
    return NULL;
}

void ir_type_print(const IrType *t, StrBuf *out)
{
    uint32_t i;
    if (!t) {
        sb_puts(out, "?");
        return;
    }
    switch (t->kind) {
    case IRT_BOOL: sb_puts(out, "bool"); break;
    case IRT_BITS: sb_printf(out, "bit<%u>", t->width); break;
    case IRT_UINT: sb_printf(out, "uint<%u>", t->width); break;
    case IRT_SINT: sb_printf(out, "sint<%u>", t->width); break;
    case IRT_CLOCK: sb_puts(out, t->falling ? "clock<falling>" : "clock"); break;
    case IRT_RESET:
        sb_printf(out, "reset<%s, %s>", t->async ? "async" : "sync", t->active_low ? "low" : "high");
        break;
    case IRT_ARRAY:
        sb_putc(out, '[');
        ir_type_print(t->elem, out);
        sb_printf(out, "; %u]", t->len);
        break;
    case IRT_STRUCT:
        if (t->name) {
            sb_puts(out, t->name);
            break;
        }
        sb_puts(out, "struct { ");
        for (i = 0; i < t->nfields; i++) {
            if (i)
                sb_puts(out, ", ");
            sb_printf(out, "%s: ", t->fields[i].name);
            ir_type_print(t->fields[i].type, out);
        }
        sb_puts(out, " }");
        break;
    case IRT_ENUM:
        if (t->name) {
            sb_puts(out, t->name);
            break;
        }
        sb_puts(out, "enum { ");
        for (i = 0; i < t->nvariants; i++) {
            if (i)
                sb_puts(out, ", ");
            sb_puts(out, t->variants[i].name);
        }
        sb_puts(out, " }");
        break;
    }
}

/* ------------------------------------------------------ design/module -- */

IrModule *ir_module_new(IrDesign *d, const char *name, const char *tern_name, const char *display,
                        Span loc)
{
    IrModule *m = ANEW(d->arena, IrModule);
    m->name = irb_intern(name);
    m->tern_name = irb_intern(tern_name ? tern_name : name);
    m->display = irb_intern(display ? display : name);
    m->default_domain = IR_NONE;
    m->loc = loc;
    return m;
}

void ir_design_add_module(IrDesign *d, IrModule *m)
{
    *IRB_PUSH(d->arena, d->modules, d->nmodules, IrModule *) = m;
    if (m->is_top)
        d->top = m;
}

void ir_design_set_top(IrDesign *d, IrModule *m)
{
    uint32_t i, found = IR_NONE;
    for (i = 0; i < d->nmodules; i++) {
        d->modules[i]->is_top = 0;
        if (d->modules[i] == m)
            found = i;
    }
    if (found == IR_NONE) {
        ir_design_add_module(d, m);
    } else {
        for (i = found; i + 1 < d->nmodules; i++)
            d->modules[i] = d->modules[i + 1];
        d->modules[d->nmodules - 1] = m;
    }
    m->is_top = 1;
    d->top = m;
}

IrModule *ir_design_find_module(const IrDesign *d, const char *name)
{
    uint32_t i;
    for (i = 0; i < d->nmodules; i++)
        if (tern_streq(d->modules[i]->name, name))
            return d->modules[i];
    return NULL;
}

void ir_design_add_regmap(IrDesign *d, const IrRegmap *r)
{
    IrRegmap *nr = IRB_PUSH(d->arena, d->regmaps, d->nregmaps, IrRegmap);
    uint32_t i;
    *nr = *r;
    nr->name = irb_intern(r->name);
    nr->package = irb_intern(r->package);
    nr->regs = r->nregs ? ANEWN(d->arena, IrReg, r->nregs) : NULL;
    for (i = 0; i < r->nregs; i++) {
        IrReg *g = &nr->regs[i];
        *g = r->regs[i];
        g->name = irb_intern(g->name);
        g->doc = g->doc ? arena_strdup(d->arena, g->doc) : NULL;
        if (g->nfields) {
            g->fields = (IrRegField *)arena_memdup(d->arena, r->regs[i].fields,
                                                   sizeof(IrRegField) * g->nfields);
        }
    }
}

void ir_design_add_crossing(IrDesign *d, const IrCrossing *c)
{
    *IRB_PUSH(d->arena, d->crossings, d->ncrossings, IrCrossing) = *c;
}

uint32_t ir_add_signal(IrDesign *d, IrModule *m, const char *name, const char *path,
                       IrSigKind kind, IrType *type, Span loc)
{
    uint32_t id = m->nsigs;
    IrSignal *s;
    TERN_ASSERT(type != NULL && type->width > 0);
    s = IRB_PUSH(d->arena, m->sigs, m->nsigs, IrSignal);
    memset(s, 0, sizeof *s);
    s->name = irb_intern(name);
    s->path = irb_intern(path ? path : name);
    s->kind = kind;
    s->type = type;
    s->domain = IR_NONE;
    s->loc = loc;
    if (kind == IRS_IN || kind == IRS_OUT)
        *IRB_PUSH(d->arena, m->ports, m->nports, uint32_t) = id;
    return id;
}

uint32_t ir_add_memory(IrDesign *d, IrModule *m, const char *name, IrType *elem, uint32_t depth,
                       Span loc)
{
    uint32_t id = ir_add_signal(d, m, name, NULL, IRS_MEM, elem, loc);
    TERN_ASSERT(depth > 0);
    m->sigs[id].depth = depth;
    return id;
}

void ir_set_init(IrDesign *d, IrModule *m, uint32_t sig, IrBits init)
{
    IrSignal *s;
    TERN_ASSERT(sig < m->nsigs);
    s = &m->sigs[sig];
    s->has_init = 1;
    s->init = ir_bits_resize(d->arena, &init, s->type->width, 0);
}

void ir_signal_add_attr(IrDesign *d, IrModule *m, uint32_t sig, const char *name, const char *args)
{
    IrSignal *s = &m->sigs[sig];
    IrAttr *at = IRB_PUSH(d->arena, s->attrs, s->nattrs, IrAttr);
    at->name = irb_intern(name);
    at->args = args ? arena_strdup(d->arena, args) : NULL;
}

void ir_module_add_attr(IrDesign *d, IrModule *m, const char *name, const char *args)
{
    IrAttr *at = IRB_PUSH(d->arena, m->attrs, m->nattrs, IrAttr);
    at->name = irb_intern(name);
    at->args = args ? arena_strdup(d->arena, args) : NULL;
}

uint32_t ir_add_param(IrDesign *d, IrModule *m, const char *name, IrBits value, int is_signed)
{
    uint32_t id = m->nparams;
    IrParam *p = IRB_PUSH(d->arena, m->params, m->nparams, IrParam);
    p->name = irb_intern(name);
    p->value = ir_bits_copy(d->arena, &value);
    p->is_signed = is_signed;
    return id;
}

uint32_t ir_add_domain(IrDesign *d, IrModule *m, const char *name, uint32_t clock, uint32_t reset,
                       uint64_t hz)
{
    uint32_t id = m->ndomains;
    IrDomain *dm;
    TERN_ASSERT(clock < m->nsigs && m->sigs[clock].type->kind == IRT_CLOCK);
    TERN_ASSERT(reset == IR_NONE || (reset < m->nsigs && m->sigs[reset].type->kind == IRT_RESET));
    dm = IRB_PUSH(d->arena, m->domains, m->ndomains, IrDomain);
    dm->name = irb_intern(name ? name : "");
    dm->clock = clock;
    dm->reset = reset;
    dm->hz = hz;
    if (id == 0 && (!name || !*name))
        m->default_domain = 0;
    return id;
}

void ir_add_assign(IrDesign *d, IrModule *m, IrLvalue lhs, IrExpr *rhs, Span loc)
{
    IrAssign *as;
    TERN_ASSERT(lhs.sig < m->nsigs && rhs && rhs->width == lhs.width);
    as = IRB_PUSH(d->arena, m->assigns, m->nassigns, IrAssign);
    as->lhs = lhs;
    as->rhs = rhs;
    as->loc = loc;
}

static void irb_collect_targets(const IrBlock *b, Map *seen)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN: map_put(seen, s->lhs.sig, 1); break;
        case IRST_IF:
            irb_collect_targets(&s->then_b, seen);
            irb_collect_targets(&s->else_b, seen);
            break;
        case IRST_CASE:
            for (j = 0; j < s->narms; j++)
                irb_collect_targets(&s->arms[j].body, seen);
            break;
        default: break;
        }
    }
}

uint32_t ir_add_process(IrDesign *d, IrModule *m, IrProcKind kind, uint32_t domain, IrBlock body,
                        Span loc)
{
    uint32_t id = m->nprocs;
    IrProcess *p = IRB_PUSH(d->arena, m->procs, m->nprocs, IrProcess);
    p->kind = kind;
    p->domain = kind == IRP_CLOCKED ? domain : IR_NONE;
    p->body = body;
    p->loc = loc;
    if (kind == IRP_CLOCKED) {
        Map seen = {0};
        uint32_t s, n = 0;
        TERN_ASSERT(domain < m->ndomains);
        irb_collect_targets(&body, &seen);
        p->regs = ANEWN(d->arena, uint32_t, map_len(&seen) ? map_len(&seen) : 1);
        for (s = 0; s < m->nsigs; s++)
            if (map_has(&seen, s))
                p->regs[n++] = s;
        p->nregs = n;
        map_free(&seen);
    }
    return id;
}

uint32_t ir_add_instance(IrDesign *d, IrModule *m, const char *name, IrModule *child, Span loc)
{
    uint32_t id = m->ninsts;
    IrInstance *in = IRB_PUSH(d->arena, m->insts, m->ninsts, IrInstance);
    in->name = irb_intern(name);
    in->module = child;
    in->loc = loc;
    return id;
}

void ir_connect(IrDesign *d, IrModule *m, uint32_t inst, uint32_t port, IrExpr *expr)
{
    IrInstance *in;
    IrConn *c;
    uint32_t i;
    TERN_ASSERT(inst < m->ninsts);
    in = &m->insts[inst];
    TERN_ASSERT(port < in->module->nsigs);
    TERN_ASSERT(!expr || expr->width == in->module->sigs[port].type->width);
    for (i = 0; i < in->nconns; i++)
        if (in->conns[i].port == port) {
            in->conns[i].expr = expr;
            return;
        }
    c = IRB_PUSH(d->arena, in->conns, in->nconns, IrConn);
    c->port = port;
    c->expr = expr;
}

int ir_connect_name(IrDesign *d, IrModule *m, uint32_t inst, const char *port, IrExpr *expr)
{
    const IrModule *child = m->insts[inst].module;
    uint32_t p = ir_find_signal(child, port);
    if (p == IR_NONE)
        p = ir_find_path(child, port);
    if (p == IR_NONE)
        return 0;
    ir_connect(d, m, inst, p, expr);
    return 1;
}

uint32_t ir_add_property(IrDesign *d, IrModule *m, const char *name, IrAssertKind kind,
                         uint32_t domain, IrSeq *seq, Span loc)
{
    uint32_t id = m->nprops;
    IrProperty *p = IRB_PUSH(d->arena, m->props, m->nprops, IrProperty);
    p->name = irb_intern(name);
    p->kind = kind;
    p->domain = domain;
    p->seq = seq;
    p->loc = loc;
    return id;
}

uint32_t ir_find_signal(const IrModule *m, const char *name)
{
    uint32_t i;
    if (!name)
        return IR_NONE;
    for (i = 0; i < m->nsigs; i++)
        if (tern_streq(m->sigs[i].name, name))
            return i;
    return IR_NONE;
}

uint32_t ir_find_path(const IrModule *m, const char *path)
{
    uint32_t i;
    if (!path)
        return IR_NONE;
    for (i = 0; i < m->nsigs; i++)
        if (tern_streq(m->sigs[i].path, path))
            return i;
    return IR_NONE;
}

/* --------------------------------------------------------- expressions -- */

static IrExpr *irb_expr(Arena *a, IrExprKind k, uint32_t width, uint32_t nargs, Span loc)
{
    IrExpr *e = ANEW(a, IrExpr);
    TERN_ASSERT(width > 0);
    e->kind = k;
    e->width = width;
    e->sig = IR_NONE;
    e->nargs = nargs;
    e->args = nargs ? ANEWN(a, IrExpr *, nargs) : NULL;
    e->loc = loc;
    return e;
}

IrExpr *ir_const(Arena *a, IrBits bits, Span loc)
{
    IrExpr *e = irb_expr(a, IRE_CONST, bits.width, 0, loc);
    e->bits = bits;
    return e;
}

IrExpr *ir_const_u64(Arena *a, uint32_t width, uint64_t v, Span loc)
{
    return ir_const(a, ir_bits_u64(a, width, v), loc);
}

IrExpr *ir_sig(Arena *a, const IrModule *m, uint32_t sig, Span loc)
{
    const IrSignal *s;
    IrExpr *e;
    TERN_ASSERT(sig < m->nsigs);
    s = &m->sigs[sig];
    TERN_ASSERT(s->kind != IRS_MEM);
    e = irb_expr(a, IRE_SIG, s->type->width, 0, loc);
    e->sig = sig;
    e->type = s->type;
    e->is_signed = s->type->kind == IRT_SINT;
    return e;
}

IrExpr *ir_slice(Arena *a, IrExpr *x, uint32_t lo, uint32_t width, Span loc)
{
    IrExpr *e;
    TERN_ASSERT(x && width > 0 && (uint64_t)lo + width <= x->width);
    e = irb_expr(a, IRE_SLICE, width, 1, loc);
    e->args[0] = x;
    e->lo = lo;
    return e;
}

IrExpr *ir_field(Arena *a, IrExpr *x, const char *field, Span loc)
{
    const IrField *f = ir_type_field(x->type, field);
    IrExpr *e;
    TERN_ASSERT(f != NULL);
    e = ir_slice(a, x, f->lsb, f->type->width, loc);
    e->field = f->name;
    e->type = f->type;
    e->is_signed = f->type->kind == IRT_SINT;
    return e;
}

IrExpr *ir_index(Arena *a, IrExpr *x, IrExpr *idx, uint32_t elem_width, Span loc)
{
    IrExpr *e;
    TERN_ASSERT(x && idx && elem_width > 0 && elem_width <= x->width);
    e = irb_expr(a, IRE_INDEX, elem_width, 2, loc);
    e->args[0] = x;
    e->args[1] = idx;
    if (x->type && x->type->kind == IRT_ARRAY && x->type->elem->width == elem_width) {
        e->type = x->type->elem;
        e->is_signed = e->type->kind == IRT_SINT;
    }
    return e;
}

IrExpr *ir_unary(Arena *a, IrOp op, IrExpr *x, Span loc)
{
    IrExpr *e;
    uint32_t w;
    TERN_ASSERT(x && op <= IROP_RXOR);
    switch (op) {
    case IROP_NOT:
    case IROP_NEG: w = x->width; break;
    case IROP_LNOT: TERN_ASSERT(x->width == 1); w = 1; break;
    default: w = 1; break;
    }
    e = irb_expr(a, IRE_UNARY, w, 1, loc);
    e->op = op;
    e->args[0] = x;
    if (op == IROP_NOT || op == IROP_NEG) {
        e->is_signed = x->is_signed;
        e->type = x->type;
    }
    return e;
}

IrExpr *ir_binary(Arena *a, IrOp op, IrExpr *x, IrExpr *y, int is_signed, Span loc)
{
    IrExpr *e;
    uint32_t w;
    TERN_ASSERT(x && y && op >= IROP_ADD && op <= IROP_LOR);
    if (op >= IROP_SHL && op <= IROP_ASHR) {
        w = x->width;
    } else if (op >= IROP_EQ && op <= IROP_GE) {
        TERN_ASSERT(x->width == y->width);
        w = 1;
    } else if (op == IROP_LAND || op == IROP_LOR) {
        TERN_ASSERT(x->width == 1 && y->width == 1);
        w = 1;
    } else {
        TERN_ASSERT(x->width == y->width);
        w = x->width;
    }
    e = irb_expr(a, IRE_BINARY, w, 2, loc);
    e->op = op;
    e->args[0] = x;
    e->args[1] = y;
    e->is_signed = is_signed ? 1 : 0;
    return e;
}

IrExpr *ir_concat(Arena *a, uint32_t n, IrExpr **parts, Span loc)
{
    IrExpr *e;
    uint64_t w = 0;
    uint32_t i;
    TERN_ASSERT(n > 0);
    for (i = 0; i < n; i++) {
        TERN_ASSERT(parts[i] != NULL);
        w += parts[i]->width;
    }
    TERN_ASSERT(w <= 0xFFFFFFFFu);
    e = irb_expr(a, IRE_CONCAT, (uint32_t)w, n, loc);
    for (i = 0; i < n; i++)
        e->args[i] = parts[i];
    return e;
}

IrExpr *ir_mux(Arena *a, IrExpr *c, IrExpr *t, IrExpr *f, Span loc)
{
    IrExpr *e;
    TERN_ASSERT(c && t && f && c->width == 1 && t->width == f->width);
    e = irb_expr(a, IRE_MUX, t->width, 3, loc);
    e->args[0] = c;
    e->args[1] = t;
    e->args[2] = f;
    e->type = t->type;
    e->is_signed = t->is_signed;
    return e;
}

IrExpr *ir_resize(Arena *a, IrExpr *x, uint32_t width, int sign_extend, Span loc)
{
    IrExpr *e;
    TERN_ASSERT(x);
    e = irb_expr(a, IRE_RESIZE, width, 1, loc);
    e->args[0] = x;
    e->is_signed = sign_extend ? 1 : 0;
    return e;
}

IrExpr *ir_memread(Arena *a, const IrModule *m, uint32_t mem, IrExpr *addr, Span loc)
{
    const IrSignal *s;
    IrExpr *e;
    TERN_ASSERT(mem < m->nsigs && addr);
    s = &m->sigs[mem];
    TERN_ASSERT(s->kind == IRS_MEM);
    e = irb_expr(a, IRE_MEMREAD, s->type->width, 1, loc);
    e->sig = mem;
    e->args[0] = addr;
    e->type = s->type;
    e->is_signed = s->type->kind == IRT_SINT;
    return e;
}

IrExpr *ir_past(Arena *a, IrExpr *x, uint32_t cycles, Span loc)
{
    IrExpr *e;
    TERN_ASSERT(x);
    e = irb_expr(a, IRE_PAST, x->width, 1, loc);
    e->args[0] = x;
    e->lo = cycles;
    e->type = x->type;
    e->is_signed = x->is_signed;
    return e;
}

/* ------------------------------------------------------------ lvalues -- */

IrLvalue ir_lv_sig(const IrModule *m, uint32_t sig)
{
    IrLvalue lv;
    TERN_ASSERT(sig < m->nsigs);
    memset(&lv, 0, sizeof lv);
    lv.sig = sig;
    lv.width = m->sigs[sig].type->width;
    return lv;
}

IrLvalue ir_lv_slice(const IrModule *m, uint32_t sig, uint32_t lo, uint32_t width)
{
    IrLvalue lv;
    TERN_ASSERT(sig < m->nsigs && width > 0 && (uint64_t)lo + width <= m->sigs[sig].type->width);
    memset(&lv, 0, sizeof lv);
    lv.sig = sig;
    lv.lo = lo;
    lv.width = width;
    return lv;
}

IrLvalue ir_lv_index(const IrModule *m, uint32_t sig, uint32_t lo, uint32_t elem_width,
                     IrExpr *index)
{
    IrLvalue lv;
    TERN_ASSERT(sig < m->nsigs && index && elem_width > 0 &&
                (uint64_t)lo + elem_width <= m->sigs[sig].type->width);
    memset(&lv, 0, sizeof lv);
    lv.sig = sig;
    lv.lo = lo;
    lv.width = elem_width;
    lv.index = index;
    return lv;
}

/* --------------------------------------------------------- statements -- */

void ir_stmts_push(IrStmtList *l, IrStmt *s)
{
    if (l->len == l->cap)
        l->data = (IrStmt **)tern_vec_grow(l->data, &l->cap, l->len + 1, sizeof(IrStmt *));
    l->data[l->len++] = s;
}

IrBlock ir_stmts_finish(Arena *a, IrStmtList *l)
{
    IrBlock b = ir_block(a, (uint32_t)l->len, l->data);
    free(l->data);
    l->data = NULL;
    l->len = l->cap = 0;
    return b;
}

IrBlock ir_block(Arena *a, uint32_t n, IrStmt *const *stmts)
{
    IrBlock b;
    b.n = n;
    b.stmts = NULL;
    if (n) {
        b.stmts = ANEWN(a, IrStmt *, n);
        memcpy(b.stmts, stmts, sizeof(IrStmt *) * n);
    }
    return b;
}

IrBlock ir_block1(Arena *a, IrStmt *s)
{
    return ir_block(a, 1, &s);
}

static IrStmt *irb_stmt(Arena *a, IrStmtKind k, Span loc)
{
    IrStmt *s = ANEW(a, IrStmt);
    s->kind = k;
    s->mem = IR_NONE;
    s->lhs.sig = IR_NONE;
    s->loc = loc;
    return s;
}

IrStmt *ir_stmt_assign(Arena *a, IrLvalue lhs, IrExpr *rhs, Span loc)
{
    IrStmt *s;
    TERN_ASSERT(rhs && rhs->width == lhs.width);
    s = irb_stmt(a, IRST_ASSIGN, loc);
    s->lhs = lhs;
    s->rhs = rhs;
    return s;
}

IrStmt *ir_stmt_if(Arena *a, IrExpr *cond, IrBlock then_b, IrBlock else_b, Span loc)
{
    IrStmt *s;
    TERN_ASSERT(cond && cond->width == 1);
    s = irb_stmt(a, IRST_IF, loc);
    s->cond = cond;
    s->then_b = then_b;
    s->else_b = else_b;
    return s;
}

IrStmt *ir_stmt_case(Arena *a, IrExpr *subject, uint32_t narms, const IrCaseArm *arms, Span loc)
{
    IrStmt *s;
    uint32_t i, j;
    TERN_ASSERT(subject);
    s = irb_stmt(a, IRST_CASE, loc);
    s->subject = subject;
    s->narms = narms;
    s->arms = ANEWN(a, IrCaseArm, narms ? narms : 1);
    for (i = 0; i < narms; i++) {
        s->arms[i] = arms[i];
        if (arms[i].nvals) {
            s->arms[i].vals = ANEWN(a, IrBits, arms[i].nvals);
            for (j = 0; j < arms[i].nvals; j++) {
                TERN_ASSERT(arms[i].vals[j].width == subject->width);
                s->arms[i].vals[j] = ir_bits_copy(a, &arms[i].vals[j]);
            }
        }
    }
    return s;
}

IrStmt *ir_stmt_memwrite(Arena *a, const IrModule *m, uint32_t mem, IrExpr *addr, IrExpr *data,
                         IrExpr *mask, Span loc)
{
    IrStmt *s;
    TERN_ASSERT(mem < m->nsigs && m->sigs[mem].kind == IRS_MEM && addr && data);
    TERN_ASSERT(data->width == m->sigs[mem].type->width);
    TERN_ASSERT(!mask || mask->width == (data->width + 7) / 8);
    s = irb_stmt(a, IRST_MEMWRITE, loc);
    s->mem = mem;
    s->addr = addr;
    s->data = data;
    s->mask = mask;
    return s;
}

IrStmt *ir_stmt_assert(Arena *a, IrAssertKind kind, IrExpr *cond, const char *msg, Span loc)
{
    IrStmt *s;
    TERN_ASSERT(cond && cond->width == 1);
    s = irb_stmt(a, IRST_ASSERT, loc);
    s->akind = kind;
    s->cond = cond;
    s->msg = msg ? arena_strdup(a, msg) : NULL;
    return s;
}

/* ---------------------------------------------------------- sequences -- */

static IrSeq *irb_seq(Arena *a, IrSeqKind k)
{
    IrSeq *q = ANEW(a, IrSeq);
    q->kind = k;
    return q;
}

IrSeq *ir_seq_expr(Arena *a, IrExpr *e)
{
    IrSeq *q = irb_seq(a, IRQ_EXPR);
    TERN_ASSERT(e);
    q->expr = e;
    return q;
}

IrSeq *ir_seq_delay(Arena *a, IrSeq *x, uint32_t lo, uint32_t hi, IrSeq *y)
{
    IrSeq *q = irb_seq(a, IRQ_DELAY);
    TERN_ASSERT(x && y && (hi == IR_NONE || hi >= lo));
    q->a = x;
    q->b = y;
    q->lo = lo;
    q->hi = hi;
    return q;
}

IrSeq *ir_seq_binary(Arena *a, IrSeqKind kind, IrSeq *x, IrSeq *y)
{
    IrSeq *q = irb_seq(a, kind);
    TERN_ASSERT(x && y);
    TERN_ASSERT(kind == IRQ_IMPL || kind == IRQ_IMPL_NEXT || kind == IRQ_UNTIL || kind == IRQ_AND ||
                kind == IRQ_OR);
    q->a = x;
    q->b = y;
    return q;
}

IrSeq *ir_seq_unary(Arena *a, IrSeqKind kind, IrSeq *x)
{
    IrSeq *q = irb_seq(a, kind);
    TERN_ASSERT(x && (kind == IRQ_ALWAYS || kind == IRQ_NEVER || kind == IRQ_NOT));
    q->a = x;
    return q;
}

IrSeq *ir_seq_eventually(Arena *a, IrSeq *x, uint32_t hi)
{
    IrSeq *q = irb_seq(a, IRQ_EVENTUALLY);
    TERN_ASSERT(x);
    q->a = x;
    q->hi = hi;
    return q;
}

/* -------------------------------------------------------------- names -- */

const char *ir_sig_kind_name(IrSigKind k)
{
    static const char *const n[] = {"in", "out", "wire", "var", "reg", "mem"};
    return (unsigned)k < TERN_ARRAY_LEN(n) ? n[k] : "?";
}

static const char *const irb_op_names[] = {
    "not", "neg", "lnot", "rand", "ror", "rxor", "add", "sub", "mul", "div", "mod", "and",
    "or",  "xor", "shl",  "shr",  "ashr", "eq",  "ne",  "lt",  "le",  "gt",  "ge",  "land", "lor"};
static const char *const irb_op_syms[] = {
    "~", "-", "!", "&", "|", "^", "+", "-", "*", "/", "%", "&", "|",
    "^", "<<", ">>", ">>>", "==", "!=", "<", "<=", ">", ">=", "&&", "||"};

const char *ir_op_name(IrOp op)
{
    return (unsigned)op < TERN_ARRAY_LEN(irb_op_names) ? irb_op_names[op] : "?";
}

const char *ir_op_symbol(IrOp op)
{
    return (unsigned)op < TERN_ARRAY_LEN(irb_op_syms) ? irb_op_syms[op] : "?";
}

const char *ir_expr_kind_name(IrExprKind k)
{
    static const char *const n[] = {"const", "sig",    "slice",  "index",   "unary", "binary",
                                    "concat", "mux",   "resize", "memread", "past"};
    return (unsigned)k < TERN_ARRAY_LEN(n) ? n[k] : "?";
}

const char *ir_seq_kind_name(IrSeqKind k)
{
    static const char *const n[] = {"expr",   "delay",      "impl",  "impl_next", "always", "never",
                                    "eventually", "until", "and",  "or",        "not"};
    return (unsigned)k < TERN_ARRAY_LEN(n) ? n[k] : "?";
}

const char *ir_assert_kind_name(IrAssertKind k)
{
    static const char *const n[] = {"assert", "assume", "cover", "expect"};
    return (unsigned)k < TERN_ARRAY_LEN(n) ? n[k] : "?";
}

const char *ir_cdc_kind_name(IrCdcKind k)
{
    static const char *const n[] = {"sync2",      "sync3",       "pulse",        "handshake", "async_fifo",
                                    "reset_sync", "reset_bridge", "gray",        "unsafe"};
    return (unsigned)k < TERN_ARRAY_LEN(n) ? n[k] : "?";
}

const char *ir_access_name(IrAccess a)
{
    static const char *const n[] = {"rw", "ro", "wo", "w1c", "rw1s"};
    return (unsigned)a < TERN_ARRAY_LEN(n) ? n[a] : "?";
}

const char *ir_encoding_name(IrEncoding e)
{
    static const char *const n[] = {"binary", "onehot", "gray"};
    return (unsigned)e < TERN_ARRAY_LEN(n) ? n[e] : "?";
}

/* --------------------------------------------------------------- dump -- */

static void irb_dump_expr(const IrModule *m, const IrExpr *e, StrBuf *out);

static void irb_dump_sig(const IrModule *m, uint32_t sig, StrBuf *out)
{
    if (m && sig < m->nsigs)
        sb_puts(out, m->sigs[sig].name);
    else
        sb_printf(out, "$%u", sig);
}

static void irb_dump_expr(const IrModule *m, const IrExpr *e, StrBuf *out)
{
    uint32_t i;
    if (!e) {
        sb_puts(out, "<null>");
        return;
    }
    switch (e->kind) {
    case IRE_CONST: ir_bits_print(&e->bits, out); break;
    case IRE_SIG: irb_dump_sig(m, e->sig, out); break;
    case IRE_SLICE:
        irb_dump_expr(m, e->args[0], out);
        if (e->field)
            sb_printf(out, ".%s", e->field);
        else if (e->width == 1)
            sb_printf(out, "[%u]", e->lo);
        else
            sb_printf(out, "[%u+:%u]", e->lo, e->width);
        break;
    case IRE_INDEX:
        irb_dump_expr(m, e->args[0], out);
        sb_putc(out, '[');
        irb_dump_expr(m, e->args[1], out);
        sb_printf(out, "]:%u", e->width);
        break;
    case IRE_UNARY:
        sb_puts(out, ir_op_symbol(e->op));
        irb_dump_expr(m, e->args[0], out);
        break;
    case IRE_BINARY:
        sb_putc(out, '(');
        irb_dump_expr(m, e->args[0], out);
        sb_printf(out, " %s%s ", ir_op_symbol(e->op),
                  e->is_signed && (e->op == IROP_DIV || e->op == IROP_MOD ||
                                   (e->op >= IROP_LT && e->op <= IROP_GE))
                      ? "s"
                      : "");
        irb_dump_expr(m, e->args[1], out);
        sb_putc(out, ')');
        break;
    case IRE_CONCAT:
        sb_putc(out, '{');
        for (i = 0; i < e->nargs; i++) {
            if (i)
                sb_puts(out, ", ");
            irb_dump_expr(m, e->args[i], out);
        }
        sb_putc(out, '}');
        break;
    case IRE_MUX:
        sb_putc(out, '(');
        irb_dump_expr(m, e->args[0], out);
        sb_puts(out, " ? ");
        irb_dump_expr(m, e->args[1], out);
        sb_puts(out, " : ");
        irb_dump_expr(m, e->args[2], out);
        sb_putc(out, ')');
        break;
    case IRE_RESIZE:
        sb_printf(out, "%s%u(", e->is_signed ? "sext" : "zext", e->width);
        irb_dump_expr(m, e->args[0], out);
        sb_putc(out, ')');
        break;
    case IRE_MEMREAD:
        irb_dump_sig(m, e->sig, out);
        sb_putc(out, '[');
        irb_dump_expr(m, e->args[0], out);
        sb_putc(out, ']');
        break;
    case IRE_PAST:
        sb_puts(out, "past(");
        irb_dump_expr(m, e->args[0], out);
        sb_printf(out, ", %u)", e->lo);
        break;
    }
}

static void irb_dump_lv(const IrModule *m, const IrLvalue *lv, StrBuf *out)
{
    irb_dump_sig(m, lv->sig, out);
    if (lv->index) {
        sb_printf(out, "[%u + ", lv->lo);
        irb_dump_expr(m, lv->index, out);
        sb_printf(out, "*%u +: %u]", lv->width, lv->width);
    } else if (m && lv->sig < m->nsigs && (lv->lo != 0 || lv->width != m->sigs[lv->sig].type->width)) {
        if (lv->field)
            sb_printf(out, ".%s", lv->field);
        else
            sb_printf(out, "[%u+:%u]", lv->lo, lv->width);
    }
}

static void irb_dump_block(const IrModule *m, const IrBlock *b, int ind, StrBuf *out);

static void irb_dump_stmt(const IrModule *m, const IrStmt *s, int ind, StrBuf *out)
{
    uint32_t i, j;
    sb_indent(out, ind);
    switch (s->kind) {
    case IRST_ASSIGN:
        irb_dump_lv(m, &s->lhs, out);
        sb_puts(out, " = ");
        irb_dump_expr(m, s->rhs, out);
        sb_putc(out, '\n');
        break;
    case IRST_IF:
        sb_puts(out, "if ");
        irb_dump_expr(m, s->cond, out);
        sb_puts(out, " {\n");
        irb_dump_block(m, &s->then_b, ind + 2, out);
        if (s->else_b.n) {
            sb_indent(out, ind);
            sb_puts(out, "} else {\n");
            irb_dump_block(m, &s->else_b, ind + 2, out);
        }
        sb_indent(out, ind);
        sb_puts(out, "}\n");
        break;
    case IRST_CASE:
        sb_puts(out, "case ");
        irb_dump_expr(m, s->subject, out);
        sb_puts(out, " {\n");
        for (i = 0; i < s->narms; i++) {
            sb_indent(out, ind + 2);
            if (!s->arms[i].nvals)
                sb_puts(out, "default");
            for (j = 0; j < s->arms[i].nvals; j++) {
                if (j)
                    sb_puts(out, ", ");
                ir_bits_print(&s->arms[i].vals[j], out);
            }
            sb_puts(out, " => {\n");
            irb_dump_block(m, &s->arms[i].body, ind + 4, out);
            sb_indent(out, ind + 2);
            sb_puts(out, "}\n");
        }
        sb_indent(out, ind);
        sb_puts(out, "}\n");
        break;
    case IRST_MEMWRITE:
        irb_dump_sig(m, s->mem, out);
        sb_putc(out, '[');
        irb_dump_expr(m, s->addr, out);
        sb_puts(out, "] = ");
        irb_dump_expr(m, s->data, out);
        if (s->mask) {
            sb_puts(out, " mask ");
            irb_dump_expr(m, s->mask, out);
        }
        sb_putc(out, '\n');
        break;
    case IRST_ASSERT:
        sb_printf(out, "%s ", ir_assert_kind_name(s->akind));
        irb_dump_expr(m, s->cond, out);
        if (s->msg)
            sb_printf(out, ", \"%s\"", s->msg);
        sb_putc(out, '\n');
        break;
    }
}

static void irb_dump_block(const IrModule *m, const IrBlock *b, int ind, StrBuf *out)
{
    uint32_t i;
    for (i = 0; i < b->n; i++)
        irb_dump_stmt(m, b->stmts[i], ind, out);
}

static void irb_dump_seq(const IrModule *m, const IrSeq *q, StrBuf *out)
{
    if (!q) {
        sb_puts(out, "<null>");
        return;
    }
    switch (q->kind) {
    case IRQ_EXPR: irb_dump_expr(m, q->expr, out); break;
    case IRQ_DELAY:
        sb_putc(out, '(');
        irb_dump_seq(m, q->a, out);
        if (q->hi == q->lo)
            sb_printf(out, " ##%u ", q->lo);
        else if (q->hi == IR_NONE)
            sb_printf(out, " ##[%u:$] ", q->lo);
        else
            sb_printf(out, " ##[%u:%u] ", q->lo, q->hi);
        irb_dump_seq(m, q->b, out);
        sb_putc(out, ')');
        break;
    case IRQ_IMPL:
    case IRQ_IMPL_NEXT:
    case IRQ_UNTIL:
    case IRQ_AND:
    case IRQ_OR:
        sb_putc(out, '(');
        irb_dump_seq(m, q->a, out);
        sb_puts(out, q->kind == IRQ_IMPL        ? " |-> "
                     : q->kind == IRQ_IMPL_NEXT ? " |=> "
                     : q->kind == IRQ_UNTIL     ? " until "
                     : q->kind == IRQ_AND       ? " and "
                                                : " or ");
        irb_dump_seq(m, q->b, out);
        sb_putc(out, ')');
        break;
    case IRQ_ALWAYS:
    case IRQ_NEVER:
    case IRQ_NOT:
        sb_puts(out, q->kind == IRQ_ALWAYS ? "always " : q->kind == IRQ_NEVER ? "never " : "not ");
        irb_dump_seq(m, q->a, out);
        break;
    case IRQ_EVENTUALLY:
        if (q->hi == IR_NONE)
            sb_puts(out, "eventually ");
        else
            sb_printf(out, "eventually[..%u] ", q->hi);
        irb_dump_seq(m, q->a, out);
        break;
    }
}

static void irb_dump_attrs(uint32_t n, const IrAttr *a, StrBuf *out)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        sb_printf(out, " #[%s", a[i].name);
        if (a[i].args)
            sb_printf(out, "(%s)", a[i].args);
        sb_putc(out, ']');
    }
}

static void irb_dump_module(const IrModule *m, StrBuf *out)
{
    uint32_t i, j;
    sb_printf(out, "module %s", m->name);
    if (m->display && !tern_streq(m->display, m->name))
        sb_printf(out, " \"%s\"", m->display);
    if (m->package)
        sb_printf(out, " in %s", m->package);
    if (m->is_top)
        sb_puts(out, " top");
    if (m->is_extern)
        sb_printf(out, " extern(\"%s\")", m->extern_file ? m->extern_file : "");
    if (m->is_cdc)
        sb_printf(out, " cdc(%s)", ir_cdc_kind_name(m->cdc_kind));
    irb_dump_attrs(m->nattrs, m->attrs, out);
    sb_puts(out, "\n");
    for (i = 0; i < m->nparams; i++) {
        sb_printf(out, "  param %s = ", m->params[i].name);
        ir_bits_print(&m->params[i].value, out);
        sb_puts(out, m->params[i].is_signed ? " signed\n" : "\n");
    }
    for (i = 0; i < m->ndomains; i++) {
        const IrDomain *dm = &m->domains[i];
        sb_printf(out, "  domain %u \"%s\" clock=", i, dm->name ? dm->name : "");
        irb_dump_sig(m, dm->clock, out);
        sb_puts(out, " reset=");
        if (dm->reset == IR_NONE)
            sb_puts(out, "none");
        else
            irb_dump_sig(m, dm->reset, out);
        if (dm->hz)
            sb_printf(out, " hz=%llu", (unsigned long long)dm->hz);
        if (m->default_domain == i)
            sb_puts(out, " default");
        sb_putc(out, '\n');
    }
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        sb_printf(out, "  %u: %s %s", i, ir_sig_kind_name(s->kind), s->name);
        if (!tern_streq(s->path, s->name))
            sb_printf(out, " (%s)", s->path);
        sb_puts(out, ": ");
        ir_type_print(s->type, out);
        if (s->kind == IRS_MEM)
            sb_printf(out, " x %u", s->depth);
        if (s->domain != IR_NONE)
            sb_printf(out, " @%u", s->domain);
        if (s->has_init) {
            sb_puts(out, " = ");
            ir_bits_print(&s->init, out);
        }
        if (s->test_port)
            sb_puts(out, " test_port");
        if (s->flipped)
            sb_puts(out, " flipped");
        if (s->mem_init)
            sb_puts(out, " mem_init");
        if (s->mem_init_file)
            sb_printf(out, " %s(\"%s\")", s->mem_init_is_bin ? "include_bin" : "include_hex",
                      s->mem_init_file);
        irb_dump_attrs(s->nattrs, s->attrs, out);
        sb_putc(out, '\n');
    }
    for (i = 0; i < m->nassigns; i++) {
        sb_puts(out, "  assign ");
        irb_dump_lv(m, &m->assigns[i].lhs, out);
        sb_puts(out, " = ");
        irb_dump_expr(m, m->assigns[i].rhs, out);
        sb_putc(out, '\n');
    }
    for (i = 0; i < m->nprocs; i++) {
        const IrProcess *p = &m->procs[i];
        if (p->kind == IRP_COMB) {
            sb_puts(out, "  comb {\n");
        } else {
            sb_printf(out, "  on %u regs(", p->domain);
            for (j = 0; j < p->nregs; j++) {
                if (j)
                    sb_puts(out, ", ");
                irb_dump_sig(m, p->regs[j], out);
            }
            sb_puts(out, ") {\n");
        }
        irb_dump_block(m, &p->body, 4, out);
        sb_puts(out, "  }\n");
    }
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        sb_printf(out, "  inst %s: %s (", in->name, in->module ? in->module->name : "?");
        for (j = 0; j < in->nconns; j++) {
            const IrSignal *ps = in->module ? &in->module->sigs[in->conns[j].port] : NULL;
            if (j)
                sb_puts(out, ", ");
            sb_printf(out, "%s %s ", ps ? ps->name : "?", ps && ps->kind == IRS_OUT ? "->" : "=");
            if (in->conns[j].expr)
                irb_dump_expr(m, in->conns[j].expr, out);
            else
                sb_puts(out, "_");
        }
        sb_puts(out, ")\n");
    }
    for (i = 0; i < m->nprops; i++) {
        const IrProperty *p = &m->props[i];
        sb_printf(out, "  %s property %s @%u: ", ir_assert_kind_name(p->kind),
                  p->name ? p->name : "_", p->domain);
        irb_dump_seq(m, p->seq, out);
        sb_putc(out, '\n');
    }
    sb_puts(out, "end\n");
}

void ir_dump(const IrDesign *d, StrBuf *out)
{
    uint32_t i, j;
    for (i = 0; i < d->ntypes; i++) {
        const IrType *t = d->types[i];
        if (t->kind == IRT_STRUCT) {
            sb_printf(out, "struct %s (%u bits) {", t->name ? t->name : "_", t->width);
            for (j = 0; j < t->nfields; j++) {
                sb_printf(out, "%s %s: ", j ? "," : "", t->fields[j].name);
                ir_type_print(t->fields[j].type, out);
                sb_printf(out, " @%u", t->fields[j].lsb);
            }
            sb_puts(out, " }\n");
        } else if (t->kind == IRT_ENUM) {
            sb_printf(out, "enum %s (%u bits, tag %u, %s) {", t->name ? t->name : "_", t->width,
                      t->tag_width, ir_encoding_name(t->encoding));
            for (j = 0; j < t->nvariants; j++) {
                sb_printf(out, "%s %s = ", j ? "," : "", t->variants[j].name);
                ir_bits_print(&t->variants[j].tag, out);
                if (t->variants[j].payload) {
                    sb_puts(out, " ");
                    ir_type_print(t->variants[j].payload, out);
                }
            }
            sb_puts(out, " }\n");
        } else {
            sb_puts(out, "type ");
            ir_type_print(t, out);
            sb_putc(out, '\n');
        }
    }
    for (i = 0; i < d->nmodules; i++)
        irb_dump_module(d->modules[i], out);
    for (i = 0; i < d->nregmaps; i++) {
        const IrRegmap *r = &d->regmaps[i];
        sb_printf(out, "regmap %s\n", r->name);
        for (j = 0; j < r->nregs; j++) {
            const IrReg *g = &r->regs[j];
            uint32_t k;
            sb_printf(out, "  0x%llx %s: %s %u%s", (unsigned long long)g->offset, g->name,
                      ir_access_name(g->access), g->width, g->is_stream ? " stream" : "");
            for (k = 0; k < g->nfields; k++)
                sb_printf(out, " %s@[%u+:%u]", g->fields[k].name, g->fields[k].lo, g->fields[k].width);
            sb_putc(out, '\n');
        }
    }
    for (i = 0; i < d->ncrossings; i++) {
        const IrCrossing *c = &d->crossings[i];
        sb_printf(out, "crossing %s %s %s -> %s %s\n", c->path ? c->path : "", ir_cdc_kind_name(c->kind),
                  c->from_domain ? c->from_domain : "?", c->to_domain ? c->to_domain : "?",
                  c->signal ? c->signal : "");
    }
}

#undef IRB_PUSH
