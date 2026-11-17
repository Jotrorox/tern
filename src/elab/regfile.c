/* regfile.c — the compiler-generated body of std::bus::RegisterFile<Map, Bus>
 * (spec §10.3).
 *
 * RegisterFile is declared in std/bus/regfile.tern with its generics and the
 * common ports (clk, rst, bus). For each instantiation, el_regfile_body()
 * writes the rest of the module as Tern source, parses it and hands the
 * statements to elab_module(), which elaborates them like any module body:
 *
 *   - the bus front-end matching `Bus` (Axi4LiteRegPort, ApbRegPort or
 *     WishboneRegPort, written in Tern in the same file) on `bus`, giving a
 *     RegBus: one access per cycle, answered in the same cycle;
 *   - one port per field of Map::Regs: `rw`/`wo` registers are outputs of
 *     their type (`Map::<Name>`), `ro` registers are inputs, `wo T ->
 *     Stream<T>` entries are output streams carrying one beat per bus write,
 *     and `w1c` / `rw1s` registers are outputs plus an input `<name>_set`
 *     (bits the hardware sets) / `<name>_clr` (bits the hardware clears);
 *   - storage: one `reg` per register in the register's bus layout (fields at
 *     their `@` bit positions, gaps read as zero), reset to the entry's
 *     `= value` or zero;
 *   - decode on the word address: writes honour the byte strobes (`w1c`
 *     clears the written one bits, `rw1s` sets them; a hardware set/clear in
 *     the same cycle wins over the bus for w1c, the bus wins for rw1s),
 *     writes to `ro` registers are ignored, reads return the register (`ro`:
 *     the input, `wo`: zero), registers wider than the bus occupy
 *     consecutive words (least significant first);
 *   - `err` (SLVERR / PSLVERR / ERR) for an access to an offset not in the
 *     map and for a write to a stream register whose previous beat was not
 *     taken yet (the write is dropped).
 *
 * Internal names start with `rf__`. The generated source is registered as
 * "<RegisterFile<Map, Bus>>" so diagnostics in it are recognizable.
 */
#include "elab_impl.h"
#include "../syntax/parser.h"

typedef struct RfField {
    uint32_t lo, hi;          /* layout bits */
    uint32_t plo;             /* bit offset in the packed struct */
} RfField;

typedef struct RfReg {
    RegEntry *e;
    const char *name;
    uint64_t offset;          /* byte offset */
    IrAccess acc;
    int stream;
    int has_ready;            /* stream type has a `ready` field */
    uint32_t width;           /* layout width */
    uint32_t nf;              /* fields (descending positions), 0 for scalars */
    RfField *f;
    char *reset;              /* hex literal of the reset value in layout, or NULL */
    char *fmask;              /* hex literal of the field bits (NULL: every bit) */
    uint64_t word;            /* first word address */
    uint32_t nwords;
} RfReg;

/* hex literal of bits [lo +: w] of b ("0x0" when zero) */
static char *rf_hex(Elab *el, const BigInt *b, uint32_t lo, uint32_t w)
{
    BigInt x = {0}, m = {0};
    char *s, *r;
    bn_copy(&x, b);
    bn_shr(&x, &x, lo);
    bn_set_i64(&m, 1);
    bn_shl(&m, &m, w);
    bn_add_i64(&m, &m, -1);
    bn_and(&x, &x, &m);
    s = bn_to_str(&x, 16);
    r = arena_printf(&el->arena, "0x%s", s);
    free(s);
    bn_free(&x);
    bn_free(&m);
    return r;
}

static int rf_is_name(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

/* layout bits of a value of the register's type (packed bits pb) */
static void rf_layout_of_packed(const RfReg *r, const BigInt *pb, BigInt *out)
{
    uint32_t i, k;
    bn_set_i64(out, 0);
    if (!r->nf) {
        bn_copy(out, pb);
        return;
    }
    for (i = 0; i < r->nf; i++)
        for (k = 0; k <= r->f[i].hi - r->f[i].lo; k++)
            bn_set_bit(out, r->f[i].lo + k, bn_get_bit(pb, r->f[i].plo + k));
}

/* Tern expression: the packed value (bit<pwidth>) of layout bits `lay` */
static void rf_packed_expr(const RfReg *r, const char *lay, StrBuf *sb)
{
    uint32_t i;
    if (!r->nf) {
        sb_puts(sb, lay);
        return;
    }
    for (i = 0; i < r->nf; i++)
        sb_printf(sb, "%s%s[%u:%u]", i ? " ++ " : "", lay, (unsigned)r->f[i].hi, (unsigned)r->f[i].lo);
}

/* Tern expression: layout bits (bit<width>) of packed bits `pk` */
static void rf_layout_expr(const RfReg *r, const char *pk, StrBuf *sb)
{
    uint32_t i, top;
    int first = 1;
    if (!r->nf) {
        sb_puts(sb, pk);
        return;
    }
    top = r->width;
    for (i = 0; i < r->nf; i++) {
        uint32_t w = r->f[i].hi - r->f[i].lo + 1;
        if (top > r->f[i].hi + 1)
            sb_printf(sb, "%s(0 as bit<%u>)", first ? "" : " ++ ", (unsigned)(top - r->f[i].hi - 1)), first = 0;
        sb_printf(sb, "%s%s[%u:%u]", first ? "" : " ++ ", pk, (unsigned)(r->f[i].plo + w - 1), (unsigned)r->f[i].plo);
        first = 0;
        top = r->f[i].lo;
    }
    if (top > 0)
        sb_printf(sb, " ++ (0 as bit<%u>)", (unsigned)top);
}

/* reads the regmap entries; 0 after reporting errors */
static int rf_collect(Elab *el, Item *rm, FileUnit *file, uint32_t dw, uint32_t aw, RfReg *regs)
{
    Sema *S = el->S;
    Env *env = elab_env_new(el, NULL, file);
    uint32_t i, k, lsb = ty_clog2(dw / 8);
    int ok = 1;
    for (i = 0; i < rm->u.regmap.nentries; i++) {
        RegEntry *e = rm->u.regmap.entries[i];
        RfReg *r = &regs[i];
        const char *acc = e->access.name ? e->access.name : "rw";
        Value off = elab_eval_const(el, e->offset, env, S->t_int, "a register offset");
        Ty *t;
        r->e = e;
        r->name = e->name.name;
        r->acc = strcmp(acc, "ro") == 0 ? IR_RO : strcmp(acc, "wo") == 0 ? IR_WO : strcmp(acc, "w1c") == 0 ? IR_W1C
               : strcmp(acc, "rw1s") == 0 ? IR_RW1S : IR_RW;
        r->stream = e->kind == REG_STREAM;
        if (!r->name || off.kind == VAL_ERROR) {
            ok = 0;
            continue;
        }
        if (!val_get_u64(&off, &r->offset)) {
            el_error(el, e->offset->span, "invalid register offset");
            ok = 0;
            continue;
        }
        if (r->offset % (dw / 8)) {
            el_error(el, e->offset->span, "register `%s` at offset 0x%llx is not aligned to the %u-byte bus words",
                     r->name, (unsigned long long)r->offset, (unsigned)(dw / 8));
            ok = 0;
            continue;
        }
        if (rf_is_name(r->name, "clk") || rf_is_name(r->name, "rst") || rf_is_name(r->name, "bus")) {
            el_error(el, e->name.span, "register `%s` has the name of a RegisterFile port", r->name);
            ok = 0;
            continue;
        }
        t = el_regmap_type(el, rm, file, r->name, e->name.span);
        if (t->kind == TY_ERROR) {
            ok = 0;
            continue;
        }
        if (!t->hw) {
            el_error(el, e->name.span, "register `%s` has type %s, which is not a hardware type", r->name, ty_str(t));
            ok = 0;
            continue;
        }
        if (r->stream) {
            Ty *st = el_regmap_type(el, rm, file, arena_printf(&el->arena, "__stream_%s", r->name), e->name.span);
            int kv, kd;
            if (st->kind == TY_ERROR) {
                ok = 0;
                continue;
            }
            kv = ty_field_index(st, intern_cstr("valid"));
            kd = ty_field_index(st, intern_cstr("data"));
            if (r->acc != IR_WO) {
                el_error(el, e->access.span, "a stream register (`T -> Stream<T>`) must be `wo`");
                ok = 0;
                continue;
            }
            if (st->kind != TY_BUNDLE || kv < 0 || kd < 0 || st->fields[kv].flip || st->fields[kd].flip ||
                st->fields[kd].ty != t) {
                el_error(el, e->stream_type->span, "register `%s`: %s is not a stream of %s (fields `valid` and `data`)",
                         r->name, ty_str(st), ty_str(t));
                ok = 0;
                continue;
            }
            k = (uint32_t)ty_field_index(st, intern_cstr("ready"));
            r->has_ready = (int)k >= 0 && st->fields[k].flip;
            if (t->width > dw) {
                el_error(el, e->span, "stream register `%s` (%u bits) is wider than the %u-bit bus", r->name,
                         (unsigned)t->width, (unsigned)dw);
                ok = 0;
                continue;
            }
            if (e->reset)
                el_error(el, e->reset->span, "a stream register has no reset value");
        }
        if (e->kind == REG_FIELDS) {
            BigInt m = {0};
            uint32_t a, b, plo = 0;
            r->nf = e->nfields;
            r->f = ANEWN(&el->arena, RfField, r->nf ? r->nf : 1);
            r->width = 0;
            for (k = 0; k < r->nf; k++) {
                RegField *f = e->fields[k];
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
                if (l < 0 || h > 4095) {
                    el_error(el, f->span, "invalid bit position for field `%s`", f->name.name);
                    ok = 0;
                    l = h = 0;
                }
                r->f[k].lo = (uint32_t)l;
                r->f[k].hi = (uint32_t)h;
                if ((uint32_t)h + 1 > r->width)
                    r->width = (uint32_t)h + 1;
            }
            /* descending positions, like the packed struct (MSB first) */
            for (a = 0; a < r->nf; a++)
                for (b = a + 1; b < r->nf; b++)
                    if (r->f[b].lo > r->f[a].lo) {
                        RfField x = r->f[a];
                        r->f[a] = r->f[b];
                        r->f[b] = x;
                    }
            for (a = r->nf; a-- > 0;) {
                r->f[a].plo = plo;
                plo += r->f[a].hi - r->f[a].lo + 1;
                if (a + 1 < r->nf && r->f[a].lo <= r->f[a + 1].hi) {
                    el_error(el, e->span, "fields of register `%s` overlap", r->name);
                    ok = 0;
                }
            }
            for (a = 0; a < r->nf; a++)
                for (k = r->f[a].lo; k <= r->f[a].hi; k++)
                    bn_set_bit(&m, k, 1);
            r->fmask = rf_hex(el, &m, 0, r->width);
            bn_free(&m);
            if (plo != t->width)
                ok = 0; /* reported by the type of the register */
        } else {
            r->width = t->width;
        }
        if (e->reset && !r->stream) {
            Value v = elab_eval_const(el, e->reset, env, NULL, "a reset value");
            BigInt b = {0}, lay = {0};
            if (v.kind == VAL_INT && v.ty && v.ty->kind == TY_INT) {
                /* an integer: the register's bits as seen on the bus */
                val_get_bn(&v, &b);
                if (bn_is_neg(&b) || bn_bitlen(&b) > r->width) {
                    el_error(el, e->reset->span, "reset value does not fit the %u bits of register `%s`",
                             (unsigned)r->width, r->name);
                    ok = 0;
                }
                bn_copy(&lay, &b);
            } else if (v.kind != VAL_ERROR) {
                v = elab_convert(el, v, t, e->reset, arena_printf(&el->arena, "register `%s`", r->name), e->reset->span);
                if (v.kind != VAL_ERROR) {
                    el_bits_of_const(el, &v, t, &b);
                    rf_layout_of_packed(r, &b, &lay);
                } else {
                    ok = 0;
                }
            } else {
                ok = 0;
            }
            r->reset = rf_hex(el, &lay, 0, r->width);
            bn_free(&b);
            bn_free(&lay);
        }
        r->word = r->offset >> lsb;
        r->nwords = (r->width + dw - 1) / dw;
        if (aw - lsb < 64 && (r->word + r->nwords - 1) >> (aw - lsb)) {
            el_error(el, e->offset->span, "register `%s` at offset 0x%llx is outside the %u-bit address space",
                     r->name, (unsigned long long)r->offset, (unsigned)aw);
            ok = 0;
        }
    }
    /* overlapping offsets */
    for (i = 0; i < rm->u.regmap.nentries; i++)
        for (k = 0; k < i; k++)
            if (regs[i].nwords && regs[k].nwords && regs[i].word < regs[k].word + regs[k].nwords && regs[k].word < regs[i].word + regs[i].nwords) {
                el_error(el, regs[i].e->offset->span, "register `%s` overlaps register `%s`", regs[i].name, regs[k].name);
                ok = 0;
            }
    elab_env_free(env);
    return ok;
}

static void rf_gen(RfReg *regs, uint32_t n, uint32_t aw, uint32_t dw, const char *front, StrBuf *sb)
{
    int storage = 0;
    uint32_t i, k, lsb = ty_clog2(dw / 8), nb = dw / 8, nhit = 0;
    StrBuf lay = {0};
    sb_puts(sb, "module RegisterFile {\n");
    /* ports */
    for (i = 0; i < n; i++) {
        RfReg *r = &regs[i];
        if (r->stream)
            sb_printf(sb, "out %s: Map::__stream_%s\n", r->name, r->name);
        else
            sb_printf(sb, "%s %s: Map::%s\n", r->acc == IR_RO ? "in" : "out", r->name, r->name);
        if (r->acc == IR_W1C)
            sb_printf(sb, "in %s_set: Map::%s\n", r->name, r->name);
        if (r->acc == IR_RW1S)
            sb_printf(sb, "in %s_clr: Map::%s\n", r->name, r->name);
    }
    /* the bus side */
    sb_printf(sb, "let rf__port = %s<%u, %u>(s: bus)\n", front, (unsigned)aw, (unsigned)dw);
    sb_puts(sb, "let rf__req = rf__port.regs.req.data\n");
    sb_puts(sb, "let rf__valid = rf__port.regs.req.valid\n");
    sb_puts(sb, "let rf__wr = rf__valid and rf__req.write\n");
    if (lsb)
        sb_printf(sb, "let rf__word = rf__req.addr[%u:%u]\n", (unsigned)(aw - 1), (unsigned)lsb);
    else
        sb_puts(sb, "let rf__word = rf__req.addr\n");
    sb_puts(sb, "let rf__wdata = rf__req.wdata\n");
    sb_printf(sb, "let rf__wmask: bit<%u> = ", (unsigned)dw);
    for (k = nb; k-- > 0;)
        sb_printf(sb, "%s(if rf__req.strb[%u] { 0xFFu8 } else { 0u8 })", k + 1 < nb ? " ++ " : "", (unsigned)k);
    sb_putc(sb, '\n');
    /* per register */
    for (i = 0; i < n; i++) {
        RfReg *r = &regs[i];
        for (k = 0; k < r->nwords; k++) {
            sb_printf(sb, "let rf__%s_hit%u = rf__word == %llu\n", r->name, (unsigned)k,
                      (unsigned long long)(r->word + k));
            nhit++;
        }
        if (r->stream) {
            sb_printf(sb, "reg rf__%s_valid: bool = false\n", r->name);
            sb_printf(sb, "reg rf__%s_data: bit<%u> = 0\n", r->name, (unsigned)r->width);
            if (r->has_ready)
                sb_printf(sb, "let rf__%s_room = !rf__%s_valid or %s.ready\n", r->name, r->name, r->name);
            else
                sb_printf(sb, "let rf__%s_room = true\n", r->name);
            sb_printf(sb, "%s.valid = rf__%s_valid\n", r->name, r->name);
            sb_printf(sb, "%s.data = Map::%s::from_bits(rf__%s_data)\n", r->name, r->name, r->name);
            continue;
        }
        if (r->acc == IR_RO) {
            sb_printf(sb, "let rf__%s_pk = bits(%s)\n", r->name, r->name);
            sb_printf(sb, "let rf__%s_rd: bit<%u> = ", r->name, (unsigned)r->width);
            sb_reset(&lay);
            sb_printf(&lay, "rf__%s_pk", r->name);
            rf_layout_expr(r, sb_cstr(&lay), sb);
            sb_putc(sb, '\n');
            continue;
        }
        sb_printf(sb, "reg rf__%s_q: bit<%u> = %s\n", r->name, (unsigned)r->width, r->reset ? r->reset : "0");
        sb_reset(&lay);
        sb_printf(&lay, "rf__%s_q", r->name);
        sb_printf(sb, "%s = Map::%s::from_bits(", r->name, r->name);
        rf_packed_expr(r, sb_cstr(&lay), sb);
        sb_puts(sb, ")\n");
        if (r->acc == IR_W1C || r->acc == IR_RW1S) {
            /* written one bits, in layout */
            for (k = 0; k < r->nwords; k++) {
                uint32_t w = TERN_MIN(dw, r->width - k * dw);
                sb_printf(sb, "let rf__%s_wo%u: bit<%u> = if rf__wr and rf__%s_hit%u { rf__wdata[%u:0] & rf__wmask[%u:0] } else { 0 }\n",
                          r->name, (unsigned)k, (unsigned)w, r->name, (unsigned)k, (unsigned)(w - 1), (unsigned)(w - 1));
            }
            sb_printf(sb, "let rf__%s_ones: bit<%u> = ", r->name, (unsigned)r->width);
            for (k = r->nwords; k-- > 0;)
                sb_printf(sb, "%srf__%s_wo%u", k + 1 < r->nwords ? " ++ " : "", r->name, (unsigned)k);
            if (r->fmask)
                sb_printf(sb, " & %s", r->fmask);
            sb_putc(sb, '\n');
            sb_printf(sb, "let rf__%s_hwpk = bits(%s_%s)\n", r->name, r->name, r->acc == IR_W1C ? "set" : "clr");
            sb_printf(sb, "let rf__%s_hw: bit<%u> = ", r->name, (unsigned)r->width);
            sb_reset(&lay);
            sb_printf(&lay, "rf__%s_hwpk", r->name);
            rf_layout_expr(r, sb_cstr(&lay), sb);
            sb_putc(sb, '\n');
        }
    }
    /* storage updates */
    for (i = 0; i < n; i++)
        if (regs[i].acc != IR_RO)
            storage = 1;
    if (storage)
        sb_puts(sb, "on clk {\n");
    for (i = 0; i < n; i++) {
        RfReg *r = &regs[i];
        if (r->stream) {
            if (r->has_ready)
                sb_printf(sb, "if %s.ready { rf__%s_valid = false }\n", r->name, r->name);
            else
                sb_printf(sb, "rf__%s_valid = false\n", r->name);
            sb_printf(sb, "if rf__wr and rf__%s_hit0 and rf__%s_room { rf__%s_valid = true; rf__%s_data = rf__wdata[%u:0] }\n",
                      r->name, r->name, r->name, r->name, (unsigned)(r->width - 1));
            continue;
        }
        switch (r->acc) {
        case IR_RW:
        case IR_WO:
            for (k = 0; k < r->nwords; k++) {
                uint32_t lo = k * dw, w = TERN_MIN(dw, r->width - lo);
                char m[160];
                if (r->fmask)
                    snprintf(m, sizeof m, "(rf__wmask[%u:0] & %s[%u:%u])", (unsigned)(w - 1), r->fmask, (unsigned)(lo + w - 1),
                             (unsigned)lo);
                else
                    snprintf(m, sizeof m, "rf__wmask[%u:0]", (unsigned)(w - 1));
                sb_printf(sb, "if rf__wr and rf__%s_hit%u { rf__%s_q[%u:%u] = (rf__%s_q[%u:%u] & ~%s) | (rf__wdata[%u:0] & %s) }\n",
                          r->name, (unsigned)k, r->name, (unsigned)(lo + w - 1), (unsigned)lo, r->name,
                          (unsigned)(lo + w - 1), (unsigned)lo, m, (unsigned)(w - 1), m);
            }
            break;
        case IR_W1C:
            sb_printf(sb, "rf__%s_q = (rf__%s_q & ~rf__%s_ones) | rf__%s_hw\n", r->name, r->name, r->name, r->name);
            break;
        case IR_RW1S:
            sb_printf(sb, "rf__%s_q = (rf__%s_q & ~rf__%s_hw) | rf__%s_ones\n", r->name, r->name, r->name, r->name);
            break;
        case IR_RO:
            break;
        }
    }
    if (storage)
        sb_puts(sb, "}\n");
    /* read data and errors */
    sb_printf(sb, "rf__port.regs.rdata = 0u%u", (unsigned)dw);
    for (i = 0; i < n; i++) {
        RfReg *r = &regs[i];
        if (r->stream || r->acc == IR_WO)
            continue;
        for (k = 0; k < r->nwords; k++) {
            uint32_t lo = k * dw, w = TERN_MIN(dw, r->width - lo);
            sb_printf(sb, " | (if rf__%s_hit%u { rf__%s_%s[%u:%u] as uint<%u> } else { 0 })", r->name, (unsigned)k,
                      r->name, r->acc == IR_RO ? "rd" : "q", (unsigned)(lo + w - 1), (unsigned)lo, (unsigned)dw);
        }
    }
    sb_putc(sb, '\n');
    sb_puts(sb, "let rf__mapped = false");
    for (i = 0; i < n; i++)
        for (k = 0; k < regs[i].nwords; k++)
            sb_printf(sb, " or rf__%s_hit%u", regs[i].name, (unsigned)k);
    sb_putc(sb, '\n');
    sb_puts(sb, "rf__port.regs.err = rf__valid and (!rf__mapped");
    for (i = 0; i < n; i++)
        if (regs[i].stream)
            sb_printf(sb, " or (rf__req.write and rf__%s_hit0 and !rf__%s_room)", regs[i].name, regs[i].name);
    sb_puts(sb, ")\n}\n");
    TERN_UNUSED(nhit);
    sb_free(&lay);
}

int el_regfile_is(const Item *it, const FileUnit *file)
{
    return it && it->kind == ITEM_MODULE && it->name.name && strcmp(it->name.name, "RegisterFile") == 0 && file &&
           file->is_std && file->pkg && strcmp(file->pkg->path, "std::bus") == 0;
}

int el_regfile_body(Elab *el, ValModule *vm, Stmt ***body_out, uint32_t *n_out)
{
    Ty *map = NULL, *bus = NULL;
    uint32_t i, n, aw = 0, dw = 0;
    const char *front = NULL, *bn;
    RfReg *regs;
    StrBuf sb = {0};
    uint32_t src;
    AstFile *af;
    Item *rm;
    int ok;
    for (i = 0; i < vm->args.n; i++) {
        Value *v = &vm->args.items[i].val;
        if (v->kind != VAL_TYPE)
            continue;
        if (rf_is_name(vm->args.items[i].name, "Map"))
            map = v->u.type;
        else if (rf_is_name(vm->args.items[i].name, "Bus"))
            bus = v->u.type;
    }
    if (!map || !map->def || map->def->kind != ITEM_REGMAP) {
        el_error(el, vm->span, "`RegisterFile` needs a regmap as its first generic argument, found %s",
                 map ? ty_str(map) : "nothing");
        el_help(el, "declare the registers with `regmap Name { .. }` and write `RegisterFile<Name>`");
        return 0;
    }
    rm = map->def;
    bn = bus && bus->def && bus->def->kind == ITEM_BUNDLE ? bus->def->name.name : NULL;
    if (bn && bus->file && bus->file->is_std && strcmp(bus->file->pkg->path, "std::bus") == 0 && bus->ngvals >= 2) {
        const Value *gv = (const Value *)bus->gvals;
        uint64_t a = 0, d = 0;
        if (val_get_u64(&gv[0], &a) && val_get_u64(&gv[1], &d)) {
            aw = (uint32_t)a;
            dw = (uint32_t)d;
        }
        if (strcmp(bn, "Axi4Lite") == 0)
            front = "Axi4LiteRegPort";
        else if (strcmp(bn, "Apb") == 0)
            front = "ApbRegPort";
        else if (strcmp(bn, "Wishbone") == 0)
            front = "WishboneRegPort";
    }
    if (!front) {
        el_error(el, vm->span, "`RegisterFile` supports the buses Axi4Lite, Apb and Wishbone of std::bus, not %s",
                 bus ? ty_str(bus) : "this");
        return 0;
    }
    if (dw < 8 || dw > 1024 || (dw & (dw - 1)) || aw < 1 || aw > 64 || ty_clog2(dw / 8) >= aw) {
        el_error(el, vm->span, "`RegisterFile` needs a power-of-two data width of 8 ..= 1024 bits, found %s",
                 ty_str(bus));
        return 0;
    }
    n = rm->u.regmap.nentries;
    if (!n) {
        el_error(el, rm->name.span, "regmap `%s` has no registers", rm->name.name);
        return 0;
    }
    regs = ANEWN(&el->arena, RfReg, n);
    diag_push_path(arena_printf(&el->arena, "regmap %s", rm->name.name));
    ok = rf_collect(el, rm, map->file ? map->file : sema_file_of_item(el->S, rm), dw, aw, regs);
    diag_pop_path();
    if (!ok)
        return 0;
    rf_gen(regs, n, aw, dw, front, &sb);
    src = src_add(arena_printf(&el->arena, "<%s>", vm->display), sb_cstr(&sb), sb.len);
    sb_free(&sb);
    af = parse_file(&el->S->arena, src);
    if (af->nerrors || af->nitems != 1 || af->items[0]->kind != ITEM_MODULE) {
        el_error(el, vm->span, "internal error: the generated body of `%s` does not parse", vm->display);
        return 0;
    }
    *body_out = af->items[0]->u.module.body;
    *n_out = af->items[0]->u.module.nbody;
    return 1;
}
