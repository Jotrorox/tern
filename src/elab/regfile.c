/* regfile.c — the compiler-assisted part of std::bus::RegisterFile<Map, Bus>
 * (spec §10.3, std/bus/regfile.tern).
 *
 * The declaration in std fixes the generics and the common ports (clk, rst,
 * bus). For each instantiation with a `regmap` as Map, the elaborator appends
 * statements generated here to the module body before elaborating it, written
 * as ordinary Tern in the scope of the std module (so `Map::<Name>`, `Bus`
 * and the std::bus front-ends resolve there):
 *
 *   - one port per entry: `ro` registers are inputs of type Map::<Name>, the
 *     others outputs; `wo T -> Stream<T>` entries are output streams;
 *   - the front-end instance on `bus` (Axi4LiteRegPort, ApbRegPort or
 *     WishboneRegPort, picked from Bus) giving a RegBus;
 *   - the decode: each register is a word of the bus width with its fields at
 *     their regmap bit positions; a write merges `wdata` under the byte
 *     strobes (`w1c` clears the written one bits, `rw1s` sets them), a read
 *     returns the word (`wo` reads 0), an access to an unmapped offset sets
 *     `err`; registers reset to zero or the entry's `= value`.
 */
#include "elab_impl.h"
#include "../base/str.h"
#include "../syntax/parser.h"

int el_regfile_wants(Elab *el, ValModule *vm)
{
    FileUnit *f = vm->file;
    TERN_UNUSED(el);
    return vm->item && vm->item->name.name && strcmp(vm->item->name.name, "RegisterFile") == 0 && f && f->pkg &&
           f->pkg->path && strcmp(f->pkg->path, "std::bus") == 0;
}

static Ty *rf_arg_type(ValModule *vm, uint32_t i)
{
    if (i >= vm->args.n || vm->args.items[i].val.kind != VAL_TYPE)
        return NULL;
    return vm->args.items[i].val.u.type;
}

static int rf_u64(Elab *el, Expr *e, Env *env, uint64_t *out)
{
    Value v = elab_eval_const(el, e, env, el->S->t_int, "a register offset");
    return val_get_u64(&v, out);
}

/* `bits(src)[hi:lo] as uint<DW>` placed at bit `pos` of the word */
static void rf_word_part(StrBuf *t, const char *src, uint32_t lo, uint32_t w, uint32_t pos, uint64_t dw)
{
    sb_printf(t, "((bits(%s)[%u:%u] as uint<%llu>) << %u)", src, (unsigned)(lo + w - 1), (unsigned)lo,
              (unsigned long long)dw, (unsigned)pos);
}

typedef struct RfField {
    uint32_t lsb, width, pos;   /* struct lsb, width, regmap bit position */
} RfField;

/* Field layout of a register: one entry for scalars (pos 0). Returns the
 * count, 0 on errors. */
static uint32_t rf_layout(Elab *el, RegEntry *e, Ty *t, Env *renv, RfField *out, uint32_t max)
{
    uint32_t i, n = 0;
    if (e->kind != REG_FIELDS || t->kind != TY_STRUCT) {
        out[0].lsb = 0;
        out[0].width = t->width;
        out[0].pos = 0;
        return 1;
    }
    for (i = 0; i < t->nfields && n < max; i++) {
        uint32_t k;
        for (k = 0; k < e->nfields; k++) {
            RegField *f = e->fields[k];
            uint64_t hi = 0, lo = 0;
            if (f->name.name != t->fields[i].name)
                continue;
            if (!rf_u64(el, f->bit_hi, renv, &hi))
                return 0;
            lo = hi;
            if (f->bit_lo && !rf_u64(el, f->bit_lo, renv, &lo))
                return 0;
            if (lo > hi) {
                uint64_t x = lo;
                lo = hi;
                hi = x;
            }
            out[n].lsb = t->fields[i].lsb;
            out[n].width = t->fields[i].ty->width;
            out[n].pos = (uint32_t)lo;
            n++;
        }
    }
    return n;
}

int el_regfile_body(Elab *el, ValModule *vm, Stmt ***body, uint32_t *nbody)
{
    Sema *S = el->S;
    Ty *map = rf_arg_type(vm, 0), *bus = rf_arg_type(vm, 1);
    Item *rm;
    FileUnit *rfile;
    Env *renv;
    StrBuf t = {0}, rd = {0}, hits = {0}, clk = {0}, tail = {0};
    uint64_t aw = 32, dw = 32;
    const char *fe;
    uint32_t i, k, src, nentries;
    Arena *a = &el->arena;
    AstFile *af;
    Item *gen = NULL;
    int ok = 1;
    if (!map || map->kind != TY_BUNDLE || !map->def || map->def->kind != ITEM_REGMAP) {
        el_error(el, vm->span, "RegisterFile needs a `regmap` as its first generic argument");
        el_help(el, "declare `regmap MyRegs { 0x00 ctrl: rw u32 }` and use RegisterFile<MyRegs>");
        return 0;
    }
    rm = map->def;
    rfile = map->file ? map->file : sema_file_of_item(S, rm);
    if (!bus || !bus->def || !bus->def->name.name) {
        el_error(el, vm->span, "RegisterFile: the bus must be Axi4Lite<AW, DW>, Apb<AW, DW> or Wishbone<AW, DW>");
        return 0;
    }
    if (strcmp(bus->def->name.name, "Axi4Lite") == 0)
        fe = "Axi4LiteRegPort";
    else if (strcmp(bus->def->name.name, "Apb") == 0)
        fe = "ApbRegPort";
    else if (strcmp(bus->def->name.name, "Wishbone") == 0)
        fe = "WishboneRegPort";
    else {
        el_error(el, vm->span, "RegisterFile: unsupported bus `%s` (Axi4Lite, Apb or Wishbone)", ty_str(bus));
        return 0;
    }
    if (bus->ngvals >= 2 && bus->gvals) {
        const Value *gv = (const Value *)bus->gvals;
        val_get_u64(&gv[0], &aw);
        val_get_u64(&gv[1], &dw);
    }
    renv = elab_env_new(el, NULL, rfile);
    nentries = rm->u.regmap.nentries;
    sb_puts(&t, "module RegisterFileGenerated {\n");
    sb_printf(&t, "let rf_fe = %s<%llu, %llu>(s: bus)\n", fe, (unsigned long long)aw, (unsigned long long)dw);
    sb_puts(&t, "let rf_wr = rf_fe.regs.req.valid and rf_fe.regs.req.data.write\n");
    sb_printf(&t, "let rf_wmask: uint<%llu> = ", (unsigned long long)dw);
    for (k = 0; k < dw / 8; k++)
        sb_printf(&t, "%s((if rf_fe.regs.req.data.strb[%u] { 0xFFu%llu } else { 0u%llu }) << %u)", k ? " | " : "",
                  (unsigned)k, (unsigned long long)dw, (unsigned long long)dw, (unsigned)(k * 8));
    sb_puts(&t, "\n");
    sb_printf(&t, "let rf_wbits: uint<%llu> = rf_fe.regs.req.data.wdata & rf_wmask\n", (unsigned long long)dw);
    for (i = 0; i < nentries && ok; i++) {
        RegEntry *e = rm->u.regmap.entries[i];
        const char *n = e->name.name;
        const char *acc = e->access.name ? e->access.name : "rw";
        int ro = strcmp(acc, "ro") == 0, wo = strcmp(acc, "wo") == 0;
        Ty *rt = el_regmap_type(el, rm, rfile, n, e->span);
        uint64_t off = 0;
        RfField lay[64];
        uint32_t nl;
        char src_name[256];
        if (rt->kind == TY_ERROR || !rf_u64(el, e->offset, renv, &off)) {
            ok = 0;
            break;
        }
        if (rt->width > dw) {
            el_error(el, e->span, "register `%s` is %u bits wide, wider than the %llu-bit bus", n, (unsigned)rt->width,
                     (unsigned long long)dw);
            ok = 0;
            break;
        }
        nl = rf_layout(el, e, rt, renv, lay, 64);
        if (!nl) {
            ok = 0;
            break;
        }
        sb_printf(&t, "let rf_hit_%s = rf_fe.regs.req.data.addr == %llu\n", n, (unsigned long long)off);
        sb_printf(&hits, "%srf_hit_%s", hits.len ? " or " : "", n);
        if (e->kind == REG_STREAM) {
            /* one beat per bus write, held until accepted */
            sb_printf(&t, "out %s: Stream<Map::%s>\n", n, n);
            sb_printf(&t, "reg rf_%s_v: bool = false\n", n);
            sb_printf(&t, "reg rf_%s_d: Map::%s = Map::%s::from_bits(all0)\n", n, n, n);
            sb_printf(&clk, "if rf_%s_v and %s.ready { rf_%s_v = false }\n", n, n, n);
            sb_printf(&clk, "if rf_wr and rf_hit_%s {\nrf_%s_v = true\nrf_%s_d = Map::%s::from_bits(rf_fe.regs.req.data.wdata[%u:0])\n}\n",
                      n, n, n, n, (unsigned)(rt->width - 1));
            sb_printf(&tail, "%s.valid = rf_%s_v\n%s.data = rf_%s_d\n", n, n, n, n);
            continue;
        }
        if (ro) {
            sb_printf(&t, "in %s: Map::%s\n", n, n);
            snprintf(src_name, sizeof src_name, "%s", n);
        } else {
            IrBits init;
            sb_printf(&t, "out %s: Map::%s\n", n, n);
            snprintf(src_name, sizeof src_name, "rf_%s_q", n);
            if (e->reset) {
                Value rv = elab_eval_const(el, e->reset, renv, rt, "a register reset value");
                rv = elab_convert(el, rv, rt, e->reset, "the reset value", e->reset->span);
                if (rv.kind == VAL_ERROR) {
                    ok = 0;
                    break;
                }
                init = elab_const_bits(el, &rv, rt);
            } else {
                init = ir_bits_zero(a, rt->width);
            }
            sb_printf(&t, "reg rf_%s_q: Map::%s = Map::%s::from_bits(", n, n, n);
            {
                BigInt b = {0};
                bn_from_limbs(&b, init.w, init.width, 0);
                sb_puts(&t, "0x");
                bn_to_sb(&t, &b, 16);
                sb_printf(&t, "u%u)\n", (unsigned)rt->width);
                bn_free(&b);
            }
            sb_printf(&tail, "%s = rf_%s_q\n", n, n);
        }
        /* the register as a bus word */
        sb_printf(&t, "let rf_%s_word: uint<%llu> = ", n, (unsigned long long)dw);
        for (k = 0; k < nl; k++) {
            if (k)
                sb_puts(&t, " | ");
            rf_word_part(&t, src_name, lay[k].lsb, lay[k].width, lay[k].pos, dw);
        }
        sb_puts(&t, "\n");
        if (!wo)
            sb_printf(&rd, "if rf_hit_%s { rf_%s_word } else ", n, n);
        if (!ro) {
            if (strcmp(acc, "w1c") == 0)
                sb_printf(&t, "let rf_%s_new: uint<%llu> = rf_%s_word & ~rf_wbits\n", n, (unsigned long long)dw, n);
            else if (strcmp(acc, "rw1s") == 0)
                sb_printf(&t, "let rf_%s_new: uint<%llu> = rf_%s_word | rf_wbits\n", n, (unsigned long long)dw, n);
            else
                sb_printf(&t, "let rf_%s_new: uint<%llu> = (rf_%s_word & ~rf_wmask) | rf_wbits\n", n,
                          (unsigned long long)dw, n);
            /* struct fields are MSB first: concatenate the new field bits in that order */
            sb_printf(&clk, "if rf_wr and rf_hit_%s { rf_%s_q = Map::%s::from_bits(", n, n, n);
            for (k = 0; k < nl; k++)   /* lay[] follows the struct's field order */
                sb_printf(&clk, "%srf_%s_new[%u:%u]", k ? " ++ " : "", n, (unsigned)(lay[k].pos + lay[k].width - 1),
                          (unsigned)lay[k].pos);
            sb_puts(&clk, ") }\n");
        }
    }
    if (ok) {
        sb_printf(&t, "rf_fe.regs.err = rf_fe.regs.req.valid and !(%s)\n", hits.len ? sb_cstr(&hits) : "false");
        if (rd.len)
            sb_printf(&t, "rf_fe.regs.rdata = %s{ 0u%llu }\n", sb_cstr(&rd), (unsigned long long)dw);
        else
            sb_printf(&t, "rf_fe.regs.rdata = 0u%llu\n", (unsigned long long)dw);
        if (clk.len)
            sb_printf(&t, "on clk {\n%s}\n", sb_cstr(&clk));
        sb_puts(&t, sb_cstr(&tail));
        sb_puts(&t, "}\n");
    }
    elab_env_free(renv);
    if (ok) {
        char *name = arena_printf(a, "<generated RegisterFile<%s>>", rm->name.name);
        src = src_add(name, t.data, t.len);
        af = parse_file(a, src);
        if (af->nerrors || af->nitems != 1 || af->items[0]->kind != ITEM_MODULE) {
            el_error(el, vm->span, "internal error: generated RegisterFile body does not parse");
            ok = 0;
        } else {
            gen = af->items[0];
        }
    }
    if (ok && gen) {
        uint32_t n = *nbody + gen->u.module.nbody;
        Stmt **nb = ANEWN(a, Stmt *, n ? n : 1);
        if (*nbody)
            memcpy(nb, *body, *nbody * sizeof(Stmt *));
        if (gen->u.module.nbody)
            memcpy(nb + *nbody, gen->u.module.body, gen->u.module.nbody * sizeof(Stmt *));
        *body = nb;
        *nbody = n;
    }
    sb_free(&t);
    sb_free(&rd);
    sb_free(&hits);
    sb_free(&clk);
    sb_free(&tail);
    return ok;
}
