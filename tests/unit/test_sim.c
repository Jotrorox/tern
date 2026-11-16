/* test_sim.c — unit tests for src/ir (builders, dump, JSON) and src/sim
 * (bit vectors, netlist, evaluation, properties, coverage, VCD).
 *
 *     cc -o build/test_sim tests/unit/test_sim.c && build/test_sim
 *
 * A single translation unit that includes the sources under test.  Designs
 * are built directly with the IR builder API. */
#include "../../src/os/os_posix.c"
#include "../../src/os/os_win32.c"
#include "../../src/os/os_common.c"
#include "../../src/base/common.c"
#include "../../src/base/arena.c"
#include "../../src/base/str.c"
#include "../../src/base/map.c"
#include "../../src/base/bignum.c"
#include "../../src/base/json.c"
#include "../../src/base/utf8.c"
#include "../../src/base/diag.c"
#include "../../src/base/args.c"
#include "../../src/ir/ir_build.c"
#include "../../src/ir/ir_json.c"
#include "../../src/sim/bitvec.c"
#include "../../src/sim/netlist.c"
#include "../../src/sim/eval.c"
#include "../../src/sim/vcd.c"
#include "../../src/os/os.h"

/* ------------------------------------------------------------ framework */

static int ts_checks, ts_fails;

static void ts_check(int ok, const char *expr, const char *file, int line)
{
    ts_checks++;
    if (!ok) {
        ts_fails++;
        if (ts_fails <= 60)
            fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

#define TS_CHECK(c) ts_check((c) ? 1 : 0, #c, __FILE__, __LINE__)
#define TS_EQ(a, b) ts_eq_u64((uint64_t)(a), (uint64_t)(b), #a " == " #b, __FILE__, __LINE__)

static void ts_eq_u64(uint64_t a, uint64_t b, const char *expr, const char *file, int line)
{
    ts_checks++;
    if (a != b) {
        ts_fails++;
        if (ts_fails <= 60)
            fprintf(stderr, "%s:%d: %s\n    got:      %llu (0x%llx)\n    expected: %llu (0x%llx)\n",
                    file, line, expr, (unsigned long long)a, (unsigned long long)a,
                    (unsigned long long)b, (unsigned long long)b);
    }
}

static uint64_t ts_rng = UINT64_C(0x243F6A8885A308D3);

static uint64_t ts_rand(void)
{
    ts_rng ^= ts_rng >> 12;
    ts_rng ^= ts_rng << 25;
    ts_rng ^= ts_rng >> 27;
    return ts_rng * UINT64_C(2685821657736338717);
}

/* random limbs of width w, biased towards edge values */
static void ts_rand_bits(uint64_t *d, uint32_t w)
{
    uint32_t n = (w + 63) / 64, i;
    int mode = (int)(ts_rand() % 8);
    for (i = 0; i < n; i++) {
        switch (mode) {
        case 0: d[i] = 0; break;
        case 1: d[i] = ~UINT64_C(0); break;
        case 2: d[i] = i == 0 ? (ts_rand() & 0xFF) : 0; break;
        case 3: d[i] = i == n - 1 ? ts_rand() : 0; break;
        default: d[i] = ts_rand(); break;
        }
    }
    if (mode == 5 && w > 1) { /* MIN of the signed range */
        for (i = 0; i < n; i++)
            d[i] = 0;
        d[(w - 1) / 64] = UINT64_C(1) << ((w - 1) % 64);
    }
    if (w % 64)
        d[n - 1] &= (UINT64_C(1) << (w % 64)) - 1u;
}

/* a fake source so spans have files/lines */
static uint32_t ts_file;

static Span SP(uint32_t line)
{
    Span s;
    const SourceFile *f = src_get(ts_file);
    s.file = ts_file;
    s.line = line;
    s.col = 5;
    s.off = f && line >= 1 && line <= f->nlines ? f->lines[line - 1] + 4 : 0;
    s.len = 3;
    return s;
}

static void ts_setup_source(void)
{
    StrBuf sb = {0};
    int i;
    for (i = 1; i <= 400; i++)
        sb_printf(&sb, "    line %d\n", i);
    ts_file = src_add("test.tern", sb.data, sb.len);
    sb_free(&sb);
}

/* failure recorder */
typedef struct TsFails {
    int n;
    SimFailKind kind;
    char what[200];
    char inst[64];
    uint32_t line;
    uint64_t cycle, start;
} TsFails;

static void ts_on_fail(void *user, const SimFailure *f)
{
    TsFails *t = (TsFails *)user;
    t->n++;
    t->kind = f->kind;
    snprintf(t->what, sizeof t->what, "%s", f->what);
    snprintf(t->inst, sizeof t->inst, "%s", f->inst_path);
    t->line = f->loc.line;
    t->cycle = f->cycle;
    t->start = f->start_cycle;
}

/* diagnostics capture */
static char ts_diag_msg[512];
static int ts_diag_n;

static void ts_diag_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(user);
    if (d->level == DIAG_ERROR) {
        ts_diag_n++;
        snprintf(ts_diag_msg, sizeof ts_diag_msg, "%s", d->msg);
    }
}

/* ------------------------------------------------------- IR shorthands */

static Arena ts_arena;
static IrDesign *D;
#define TA (D->arena)

static uint32_t ts_sig(IrModule *m, const char *name, IrSigKind k, IrType *t, uint32_t line)
{
    return ir_add_signal(D, m, name, NULL, k, t, SP(line));
}

static IrExpr *S(IrModule *m, uint32_t s) { return ir_sig(TA, m, s, SP(1)); }
static IrExpr *K(uint32_t w, uint64_t v) { return ir_const_u64(TA, w, v, SP(1)); }
static IrExpr *BIN(IrOp op, IrExpr *a, IrExpr *b) { return ir_binary(TA, op, a, b, 0, SP(1)); }
static IrExpr *SBIN(IrOp op, IrExpr *a, IrExpr *b) { return ir_binary(TA, op, a, b, 1, SP(1)); }
static IrStmt *ASG(IrModule *m, uint32_t s, IrExpr *e, uint32_t line)
{
    return ir_stmt_assign(TA, ir_lv_sig(m, s), e, SP(line));
}
static void CONT(IrModule *m, uint32_t s, IrExpr *e) { ir_add_assign(D, m, ir_lv_sig(m, s), e, SP(1)); }
static IrBlock B0(void)
{
    IrBlock b;
    b.n = 0;
    b.stmts = NULL;
    return b;
}
static IrBlock B1(IrStmt *s) { return ir_block1(TA, s); }
static IrBlock B2(IrStmt *a, IrStmt *b)
{
    IrStmt *v[2];
    v[0] = a;
    v[1] = b;
    return ir_block(TA, 2, v);
}
static IrSeq *QE(IrExpr *e) { return ir_seq_expr(TA, e); }

static void ts_new_design(void)
{
    D = ir_design_new(&ts_arena);
}

/* ============================================================ bitvec == */

static void ts_bn_ref(BigInt *r, const uint64_t *w, uint32_t width, int sgn)
{
    bn_from_limbs(r, w, width, sgn);
}

static int ts_limbs_eq(const uint64_t *a, const BigInt *ref, uint32_t w)
{
    uint64_t buf[8];
    uint32_t n = (w + 63) / 64;
    memset(buf, 0, sizeof buf);
    bn_to_limbs(ref, buf, w);
    return memcmp(a, buf, sizeof(uint64_t) * n) == 0;
}

static void test_bitvec(void)
{
    int iter;
    uint64_t scratch[64];
    for (iter = 0; iter < 3000; iter++) {
        uint32_t w = (uint32_t)(ts_rand() % 200) + 1, n = (w + 63) / 64, i;
        uint64_t a[8], b[8], d[8], q[8], r[8];
        BigInt ba = {0}, bb = {0}, bx = {0}, bq = {0}, br = {0}, sa = {0}, sb = {0};
        uint64_t amt;
        int sgn;
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        ts_rand_bits(a, w);
        ts_rand_bits(b, w);
        ts_bn_ref(&ba, a, w, 0);
        ts_bn_ref(&bb, b, w, 0);
        ts_bn_ref(&sa, a, w, 1);
        ts_bn_ref(&sb, b, w, 1);

        bv_add(d, a, b, w);
        bn_add(&bx, &ba, &bb);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_sub(d, a, b, w);
        bn_sub(&bx, &ba, &bb);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_mul(d, a, b, w, scratch);
        bn_mul(&bx, &ba, &bb);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_neg(d, a, w);
        bn_neg(&bx, &ba);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_not(d, a, w);
        bn_not(&bx, &ba);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        /* aliasing: d == a */
        memcpy(d, a, sizeof a);
        bv_add(d, d, b, w);
        bn_add(&bx, &ba, &bb);
        TS_CHECK(ts_limbs_eq(d, &bx, w));

        for (sgn = 0; sgn < 2; sgn++) {
            const BigInt *xa = sgn ? &sa : &ba, *xb = sgn ? &sb : &bb;
            int c = bv_cmp(a, b, w, sgn);
            TS_EQ(c, bn_cmp(xa, xb));
            bv_divmod(q, r, a, b, w, sgn, scratch);
            if (bn_is_zero(xb)) {
                TS_CHECK(bv_is_zero(q, w));
                TS_CHECK(bv_eq(r, a, w));
            } else {
                bn_divmod_trunc(&bq, &br, xa, xb);
                TS_CHECK(ts_limbs_eq(q, &bq, w));
                TS_CHECK(ts_limbs_eq(r, &br, w));
            }
        }

        amt = ts_rand() % (w + 3);
        if (ts_rand() % 16 == 0)
            amt = ~UINT64_C(0) - (ts_rand() % 4);
        bv_shl(d, a, w, amt);
        bn_shl(&bx, &ba, amt > 4096 ? 4096 : amt);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_shr(d, a, w, amt);
        bn_shr(&bx, &ba, amt > 4096 ? 4096 : amt);
        TS_CHECK(ts_limbs_eq(d, &bx, w));
        bv_ashr(d, a, w, amt);
        bn_shr(&bx, &sa, amt > 4096 ? 4096 : amt);
        TS_CHECK(ts_limbs_eq(d, &bx, w));

        {   /* resize */
            uint32_t dw = (uint32_t)(ts_rand() % 200) + 1;
            uint64_t e[8];
            memset(e, 0xAA, sizeof e);
            bv_resize(e, dw, a, w, 0);
            TS_CHECK(ts_limbs_eq(e, &ba, dw));
            bv_resize(e, dw, a, w, 1);
            TS_CHECK(ts_limbs_eq(e, &sa, dw));
            /* in place */
            memcpy(e, a, sizeof a);
            bv_resize(e, dw, e, w, 1);
            TS_CHECK(ts_limbs_eq(e, &sa, dw));
        }
        {   /* extract / deposit */
            uint32_t lo = (uint32_t)(ts_rand() % (w + 10)), dw = (uint32_t)(ts_rand() % 130) + 1;
            uint64_t e[8], t[8];
            bv_extract(e, dw, a, w, lo);
            bn_shr(&bx, &ba, lo);
            TS_CHECK(ts_limbs_eq(e, &bx, dw));
            /* deposit b[0 +: dw'] into a copy of a at lo, check bitwise */
            memcpy(t, a, sizeof a);
            {
                uint32_t pw = dw;
                int ok = 1;
                bv_deposit(t, w, lo, b, pw > w ? w : pw);
                if (pw > w)
                    pw = w;
                for (i = 0; i < w; i++) {
                    int want = (i >= lo && i < lo + pw) ? bv_get_bit(b, i - lo) : bv_get_bit(a, i);
                    if (bv_get_bit(t, i) != want)
                        ok = 0;
                }
                if (w % 64 && (t[n - 1] >> (w % 64)))
                    ok = 0;
                TS_CHECK(ok);
            }
        }
        {   /* reductions */
            int pop = 0, all = 1;
            for (i = 0; i < w; i++) {
                pop += bv_get_bit(a, i);
                all &= bv_get_bit(a, i);
            }
            TS_EQ(bv_red_xor(a, w), pop & 1);
            TS_EQ(bv_red_or(a, w), pop != 0);
            TS_EQ(bv_red_and(a, w), all);
        }
        bn_free(&ba);
        bn_free(&bb);
        bn_free(&bx);
        bn_free(&bq);
        bn_free(&br);
        bn_free(&sa);
        bn_free(&sb);
    }
}

/* ========================================================= IR basics == */

static void test_ir_build(void)
{
    IrType *u8, *u8b, *pix, *arr, *anon1, *anon2;
    IrField f[3];
    IrBits b;
    StrBuf sb = {0};
    ts_new_design();
    u8 = ir_type_uint(D, 8);
    u8b = ir_type_uint(D, 8);
    TS_CHECK(u8 == u8b);
    TS_CHECK(ir_type_uint(D, 9) != u8);
    TS_CHECK(ir_type_sint(D, 8) != u8);
    TS_CHECK(ir_type_clock(D, 0) == ir_type_clock(D, 0));
    TS_CHECK(ir_type_clock(D, 1) != ir_type_clock(D, 0));
    TS_CHECK(ir_type_reset(D, 1, 1) == ir_type_reset(D, 1, 1));
    arr = ir_type_array(D, u8, 4);
    TS_CHECK(arr == ir_type_array(D, u8b, 4));
    TS_EQ(arr->width, 32);
    memset(f, 0, sizeof f);
    f[0].name = "r";
    f[0].type = u8;
    f[1].name = "g";
    f[1].type = u8;
    f[2].name = "b";
    f[2].type = u8;
    pix = ir_type_struct(D, "Pixel", 3, f, SP(2));
    TS_EQ(pix->width, 24);
    TS_EQ(pix->fields[0].lsb, 16);
    TS_EQ(pix->fields[2].lsb, 0);
    TS_EQ(D->ntypes, 1);
    anon1 = ir_type_struct(D, NULL, 2, f, SP(2));
    anon2 = ir_type_struct(D, NULL, 2, f, SP(3));
    TS_CHECK(anon1 == anon2);
    TS_EQ(D->ntypes, 1);
    /* many types: exercises the interning table growth */
    {
        uint32_t i;
        for (i = 1; i < 300; i++)
            TS_CHECK(ir_type_bits(D, i) == ir_type_bits(D, i));
        TS_CHECK(ir_type_bits(D, 17)->width == 17);
    }
    b = ir_bits_u64(TA, 12, 0xABC);
    ir_bits_print(&b, &sb);
    TS_CHECK(strcmp(sb_cstr(&sb), "12'habc") == 0);
    {
        IrBits p;
        TS_CHECK(ir_bits_parse(TA, "12'habc", 7, &p) && ir_bits_eq(&p, &b));
        TS_CHECK(ir_bits_parse(TA, "12'd2748", 8, &p) && ir_bits_eq(&p, &b));
        TS_CHECK(ir_bits_parse(TA, "12'b1010_1011_1100", 18, &p) && ir_bits_eq(&p, &b));
        TS_CHECK(!ir_bits_parse(TA, "4'h1f", 5, &p));
        TS_CHECK(!ir_bits_parse(TA, "x'h1", 4, &p));
        {
            char lit[64];
            int k;
            strcpy(lit, "130'h3");
            for (k = 0; k < 32; k++)
                strcat(lit, "f");
            TS_CHECK(ir_bits_parse(TA, lit, strlen(lit), &p));
        }
        TS_EQ(p.w[2], 3);
        TS_EQ(p.w[1], ~UINT64_C(0));
    }
    {
        IrBits x = ir_bits_u64(TA, 8, 0x80), y = ir_bits_resize(TA, &x, 70, 1);
        TS_CHECK(ir_bits_get(&y, 69) == 1 && ir_bits_get(&y, 7) == 1 && ir_bits_get(&y, 6) == 0);
        y = ir_bits_resize(TA, &x, 4, 0);
        TS_CHECK(ir_bits_is_zero(&y));
    }
    {   /* expression widths */
        IrModule *m = ir_module_new(D, "M", NULL, NULL, SP(1));
        uint32_t a = ts_sig(m, "a", IRS_IN, u8, 1), p = ts_sig(m, "p", IRS_IN, pix, 1);
        IrExpr *parts[2];
        TS_EQ(BIN(IROP_ADD, S(m, a), S(m, a))->width, 8);
        TS_EQ(BIN(IROP_LT, S(m, a), S(m, a))->width, 1);
        TS_EQ(BIN(IROP_SHL, S(m, a), K(3, 1))->width, 8);
        TS_EQ(ir_unary(TA, IROP_RXOR, S(m, a), SP(1))->width, 1);
        parts[0] = S(m, a);
        parts[1] = S(m, p);
        TS_EQ(ir_concat(TA, 2, parts, SP(1))->width, 32);
        TS_EQ(ir_field(TA, S(m, p), "g", SP(1))->lo, 8);
        TS_EQ(ir_resize(TA, S(m, a), 3, 0, SP(1))->width, 3);
        TS_EQ(ir_find_signal(m, "p"), p);
        TS_EQ(ir_find_path(m, "zzz"), IR_NONE);
        TS_EQ(m->nports, 2);
    }
    sb_free(&sb);
}

/* ======================================================== designs ==== */

/* Counter: in clk, rst (sync high), en; out count; reg q: u8 = 0 */
static IrModule *ts_counter(const char *name, IrType *rst_type, uint32_t width)
{
    IrModule *m = ir_module_new(D, name, NULL, NULL, SP(1));
    uint32_t clk = ts_sig(m, "clk", IRS_IN, ir_type_clock(D, 0), 2);
    uint32_t rst = rst_type ? ts_sig(m, "rst", IRS_IN, rst_type, 3) : IR_NONE;
    uint32_t en = ts_sig(m, "en", IRS_IN, ir_type_bool(D), 4);
    uint32_t cnt = ts_sig(m, "count", IRS_OUT, ir_type_uint(D, width), 5);
    uint32_t q = ts_sig(m, "q", IRS_REG, ir_type_uint(D, width), 6);
    ir_add_domain(D, m, "", clk, rst, 0);
    m->sigs[q].domain = 0;
    ir_set_init(D, m, q, ir_bits_u64(TA, width, 0));
    CONT(m, cnt, S(m, q));
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   B1(ir_stmt_if(TA, S(m, en), B1(ASG(m, q, BIN(IROP_ADD, S(m, q), K(width, 1)), 8)),
                                 B0(), SP(7))),
                   SP(7));
    return m;
}

static void test_counter(void)
{
    SimModel *sm;
    int en, cnt, q;
    ts_new_design();
    ir_design_set_top(D, ts_counter("Counter", ir_type_reset(D, 0, 0), 8));
    sm = sim_new(D, D->top, 1);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    en = sim_find(sm, "en");
    cnt = sim_find(sm, "count");
    q = sim_find(sm, "q");
    TS_CHECK(en >= 0 && cnt >= 0 && q >= 0);
    TS_EQ(sim_width(sm, cnt), 8);
    TS_EQ(sim_poke_u64(sm, cnt, 1), SIM_ERR_NOT_WRITABLE);
    TS_EQ(sim_poke_u64(sm, sim_find(sm, "clk"), 1), SIM_ERR_NOT_WRITABLE);
    TS_EQ(sim_poke_u64(sm, sim_find(sm, "rst"), 1), SIM_ERR_NOT_WRITABLE);
    TS_EQ(sim_poke_u64(sm, 9999, 1), SIM_ERR_HANDLE);
    sim_poke_u64(sm, en, 1);
    sim_settle(sm);
    sim_reset(sm);                    /* counting is held in reset */
    TS_EQ(sim_peek_u64(sm, cnt), 0);
    sim_step(sm, 0, 5);
    TS_EQ(sim_peek_u64(sm, cnt), 5);
    TS_EQ(sim_cycle(sm, 0), 7);       /* 2 reset cycles + 5 */
    sim_poke_u64(sm, en, 0);
    sim_step(sm, 0, 3);
    TS_EQ(sim_peek_u64(sm, cnt), 5);  /* holds */
    sim_poke_u64(sm, en, 1);
    sim_step(sm, 0, 300);
    TS_EQ(sim_peek_u64(sm, cnt), (5 + 300) % 256); /* wraps */
    sim_reset(sm);
    TS_EQ(sim_peek_u64(sm, q), 0);
    TS_EQ(sim_time_ps(sm), (uint64_t)(2 + 5 + 3 + 300 + 2) * 10000u);
    sim_free(sm);
}

/* async active-low reset: at the top (harness) and through a child port
 * driven by a data input (acts without a clock edge) */
static void test_async_reset(void)
{
    IrModule *child, *top;
    uint32_t c_clk, c_rst, c_d, c_q, c_r, t_clk, t_rstn, t_d, t_q, inst, w;
    SimModel *sm;
    ts_new_design();
    child = ir_module_new(D, "AReg", NULL, NULL, SP(10));
    c_clk = ts_sig(child, "clk", IRS_IN, ir_type_clock(D, 0), 11);
    c_rst = ts_sig(child, "rst_n", IRS_IN, ir_type_reset(D, 1, 1), 12);
    c_d = ts_sig(child, "d", IRS_IN, ir_type_uint(D, 8), 13);
    c_q = ts_sig(child, "q", IRS_OUT, ir_type_uint(D, 8), 14);
    c_r = ts_sig(child, "r", IRS_REG, ir_type_uint(D, 8), 15);
    ir_add_domain(D, child, "", c_clk, c_rst, 0);
    ir_set_init(D, child, c_r, ir_bits_u64(TA, 8, 0xAA));
    CONT(child, c_q, S(child, c_r));
    ir_add_process(D, child, IRP_CLOCKED, 0, B1(ASG(child, c_r, S(child, c_d), 16)), SP(16));
    ir_design_add_module(D, child);

    top = ir_module_new(D, "Top", NULL, NULL, SP(20));
    t_clk = ts_sig(top, "clk", IRS_IN, ir_type_clock(D, 0), 21);
    t_rstn = ts_sig(top, "arst_n", IRS_IN, ir_type_bool(D), 22);
    t_d = ts_sig(top, "d", IRS_IN, ir_type_uint(D, 8), 23);
    t_q = ts_sig(top, "q", IRS_OUT, ir_type_uint(D, 8), 24);
    w = ts_sig(top, "u_q", IRS_WIRE, ir_type_uint(D, 8), 25);
    ir_add_domain(D, top, "", t_clk, IR_NONE, 0);
    inst = ir_add_instance(D, top, "u", child, SP(26));
    ir_connect(D, top, inst, c_clk, S(top, t_clk));
    TS_CHECK(ir_connect_name(D, top, inst, "rst_n", S(top, t_rstn)));
    ir_connect(D, top, inst, c_d, S(top, t_d));
    ir_connect(D, top, inst, c_q, S(top, w));
    TS_CHECK(!ir_connect_name(D, top, inst, "nope", S(top, t_d)));
    CONT(top, t_q, S(top, w));
    ir_design_set_top(D, top);

    sm = sim_new(D, D->top, 7);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    {
        int hq = sim_find(sm, "q"), hd = sim_find(sm, "d"), hr = sim_find(sm, "arst_n");
        int ur = sim_find(sm, "u.r");
        TS_CHECK(ur >= 0);
        TS_EQ(sim_peek_u64(sm, hq), 0xAA);   /* power-on init */
        sim_poke_u64(sm, hr, 1);
        sim_poke_u64(sm, hd, 0x55);
        sim_step(sm, 0, 1);
        TS_EQ(sim_peek_u64(sm, hq), 0x55);
        TS_EQ(sim_peek_u64(sm, ur), 0x55);
        sim_poke_u64(sm, hr, 0);              /* assert async reset: no edge needed */
        sim_settle(sm);
        TS_EQ(sim_peek_u64(sm, hq), 0xAA);
        sim_step(sm, 0, 2);                    /* held in reset */
        TS_EQ(sim_peek_u64(sm, hq), 0xAA);
        sim_poke_u64(sm, hr, 1);
        sim_step(sm, 0, 1);
        TS_EQ(sim_peek_u64(sm, hq), 0x55);
        TS_CHECK(strcmp(sim_signal_instance(sm, ur), "u") == 0);
        TS_CHECK(strcmp(sim_signal_path(sm, ur), "u.r") == 0);
    }
    sim_free(sm);

    /* top-level async low reset driven by the harness */
    ts_new_design();
    ir_design_set_top(D, ts_counter("C2", ir_type_reset(D, 1, 1), 4));
    sm = sim_new(D, D->top, 3);
    if (sm) {
        int en = sim_find(sm, "en"), c = sim_find(sm, "count");
        sim_poke_u64(sm, en, 1);
        sim_step(sm, 0, 3);
        TS_EQ(sim_peek_u64(sm, c), 3);
        sim_reset(sm);
        TS_EQ(sim_peek_u64(sm, c), 0);
        sim_step(sm, 0, 20);
        TS_EQ(sim_peek_u64(sm, c), 20 % 16);
        sim_free(sm);
    }
}

/* FIFO, depth 4, u8, async read of the head; plus a sync-read port */
static IrModule *ts_fifo(void)
{
    IrModule *m = ir_module_new(D, "Fifo_u8_4", "Fifo", "Fifo<u8, 4>", SP(30));
    IrType *u8 = ir_type_uint(D, 8), *u2 = ir_type_uint(D, 2), *u3 = ir_type_uint(D, 3);
    IrType *bl = ir_type_bool(D);
    uint32_t clk = ts_sig(m, "clk", IRS_IN, ir_type_clock(D, 0), 31);
    uint32_t rst = ts_sig(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 32);
    uint32_t pv = ir_add_signal(D, m, "push_valid", "push.valid", IRS_IN, bl, SP(33));
    uint32_t pd = ir_add_signal(D, m, "push_data", "push.data", IRS_IN, u8, SP(33));
    uint32_t pr = ir_add_signal(D, m, "push_ready", "push.ready", IRS_OUT, bl, SP(33));
    uint32_t ov = ir_add_signal(D, m, "pop_valid", "pop.valid", IRS_OUT, bl, SP(34));
    uint32_t od = ir_add_signal(D, m, "pop_data", "pop.data", IRS_OUT, u8, SP(34));
    uint32_t orr = ir_add_signal(D, m, "pop_ready", "pop.ready", IRS_IN, bl, SP(34));
    uint32_t count = ts_sig(m, "count", IRS_OUT, u3, 35);
    uint32_t raddr = ts_sig(m, "raddr", IRS_IN, u2, 36);
    uint32_t rdata = ts_sig(m, "rdata", IRS_REG, u8, 36);
    uint32_t mem = ir_add_memory(D, m, "mem", u8, 4, SP(37));
    uint32_t wp = ts_sig(m, "wp", IRS_REG, u2, 38), rp = ts_sig(m, "rp", IRS_REG, u2, 38);
    uint32_t cnt = ts_sig(m, "cnt", IRS_REG, u3, 39);
    uint32_t pf = ts_sig(m, "push_fire", IRS_WIRE, bl, 40), of = ts_sig(m, "pop_fire", IRS_WIRE, bl, 40);
    IrStmtList body = {0};
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, wp, ir_bits_u64(TA, 2, 0));
    ir_set_init(D, m, rp, ir_bits_u64(TA, 2, 0));
    ir_set_init(D, m, cnt, ir_bits_u64(TA, 3, 0));
    m->sigs[rdata].domain = m->sigs[wp].domain = m->sigs[rp].domain = m->sigs[cnt].domain = 0;
    CONT(m, pr, BIN(IROP_NE, S(m, cnt), K(3, 4)));
    CONT(m, ov, BIN(IROP_NE, S(m, cnt), K(3, 0)));
    CONT(m, od, ir_memread(TA, m, mem, S(m, rp), SP(41)));
    CONT(m, count, S(m, cnt));
    CONT(m, pf, BIN(IROP_LAND, S(m, pv), S(m, pr)));
    CONT(m, of, BIN(IROP_LAND, S(m, ov), S(m, orr)));
    ir_stmts_push(&body, ir_stmt_if(TA, S(m, pf),
                                    B2(ir_stmt_memwrite(TA, m, mem, S(m, wp), S(m, pd), NULL, SP(43)),
                                       ASG(m, wp, BIN(IROP_ADD, S(m, wp), K(2, 1)), 44)),
                                    B0(), SP(42)));
    ir_stmts_push(&body, ir_stmt_if(TA, S(m, of), B1(ASG(m, rp, BIN(IROP_ADD, S(m, rp), K(2, 1)), 46)),
                                    B0(), SP(45)));
    /* cnt = cnt + push - pop */
    ir_stmts_push(&body, ASG(m, cnt,
                             BIN(IROP_SUB, BIN(IROP_ADD, S(m, cnt), ir_resize(TA, S(m, pf), 3, 0, SP(47))),
                                 ir_resize(TA, S(m, of), 3, 0, SP(47))),
                             47));
    /* sync read: lowered to a reg assigned from an async read */
    ir_stmts_push(&body, ASG(m, rdata, ir_memread(TA, m, mem, S(m, raddr), SP(48)), 48));
    ir_stmts_push(&body, ir_stmt_assert(TA, IRA_ASSERT,
                                        ir_unary(TA, IROP_LNOT,
                                                 BIN(IROP_LAND, S(m, pv), BIN(IROP_EQ, S(m, cnt), K(3, 4))),
                                                 SP(49)),
                                        "push into full fifo", SP(49)));
    ir_add_process(D, m, IRP_CLOCKED, 0, ir_stmts_finish(TA, &body), SP(42));
    return m;
}

static void test_fifo(void)
{
    SimModel *sm;
    TsFails tf;
    int pv, pd, pr, ov, od, orr, cnt, raddr, rdata, mem, h[8], i;
    ts_new_design();
    ir_design_set_top(D, ts_fifo());
    sm = sim_new(D, D->top, 11);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    memset(&tf, 0, sizeof tf);
    sim_set_fail_handler(sm, ts_on_fail, &tf);
    pv = sim_find(sm, "push.valid");
    pd = sim_find(sm, "push.data");
    pr = sim_find(sm, "push.ready");
    ov = sim_find(sm, "pop.valid");
    od = sim_find(sm, "pop.data");
    orr = sim_find(sm, "pop.ready");
    cnt = sim_find(sm, "count");
    raddr = sim_find(sm, "raddr");
    rdata = sim_find(sm, "rdata");
    mem = sim_find(sm, "mem");
    TS_CHECK(sim_find(sm, "push_valid") == pv); /* SV name works too */
    TS_EQ(sim_find_prefix(sm, "push", h, 8), 3);
    TS_EQ(h[0], pv);
    TS_EQ(h[2], pr);
    TS_CHECK(sim_is_memory(sm, mem));
    TS_EQ(sim_mem_depth(sm, mem), 4);
    TS_EQ(sim_peek_u64(sm, mem), 0);
    {
        IrBits tmp;
        uint64_t limb;
        tmp.w = &limb;
        TS_EQ(sim_peek(sm, mem, &tmp), SIM_ERR_KIND);
    }
    sim_reset(sm);
    TS_EQ(sim_peek_u64(sm, ov), 0);
    TS_EQ(sim_peek_u64(sm, pr), 1);
    for (i = 1; i <= 4; i++) {
        sim_poke_u64(sm, pv, 1);
        sim_poke_u64(sm, pd, (uint64_t)i * 11);
        sim_step(sm, 0, 1);
    }
    sim_poke_u64(sm, pv, 0);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, cnt), 4);
    TS_EQ(sim_peek_u64(sm, pr), 0);      /* full */
    TS_EQ(tf.n, 0);
    /* push into full: assertion fires (clocked, pre-edge values) */
    sim_poke_u64(sm, pv, 1);
    sim_poke_u64(sm, pd, 99);
    sim_step(sm, 0, 1);
    TS_EQ(tf.n, 1);
    TS_EQ(tf.kind, SIM_FAIL_ASSERT);
    TS_CHECK(strstr(tf.what, "push into full fifo") != NULL);
    TS_EQ(tf.line, 49);
    TS_EQ(tf.cycle, 6);
    TS_EQ(sim_peek_u64(sm, cnt), 4);     /* rejected */
    sim_poke_u64(sm, pv, 0);
    /* sync read port: data appears after the edge */
    sim_poke_u64(sm, raddr, 2);
    sim_step(sm, 0, 1);
    TS_EQ(sim_peek_u64(sm, rdata), 33);
    {
        IrBits mv;
        uint64_t limb = 0;
        mv.w = &limb;
        TS_EQ(sim_mem_peek(sm, mem, 3, &mv), SIM_OK);
        TS_EQ(limb, 44);
        TS_EQ(sim_mem_peek(sm, mem, 4, &mv), SIM_ERR_RANGE);
    }
    sim_poke_u64(sm, orr, 1);
    sim_settle(sm);
    for (i = 1; i <= 4; i++) {
        TS_EQ(sim_peek_u64(sm, ov), 1);
        TS_EQ(sim_peek_u64(sm, od), (uint64_t)i * 11);
        sim_step(sm, 0, 1);
    }
    TS_EQ(sim_peek_u64(sm, ov), 0);       /* empty */
    TS_EQ(sim_peek_u64(sm, cnt), 0);
    /* read-during-write in the same process: old data */
    sim_poke_u64(sm, orr, 0);
    sim_poke_u64(sm, pv, 1);
    sim_poke_u64(sm, pd, 0x77);
    sim_poke_u64(sm, raddr, 0);           /* wp is 0 again after wrapping */
    sim_step(sm, 0, 1);
    TS_EQ(sim_peek_u64(sm, rdata), 11);   /* old contents of mem[0] */
    sim_poke_u64(sm, pv, 0);
    sim_step(sm, 0, 1);
    TS_EQ(sim_peek_u64(sm, rdata), 0x77);
    sim_free(sm);
}

/* comb mux / case design (also used for coverage) */
static IrModule *ts_alu(void)
{
    IrModule *m = ir_module_new(D, "Alu", NULL, NULL, SP(60));
    IrType *u8 = ir_type_uint(D, 8);
    uint32_t op = ts_sig(m, "op", IRS_IN, ir_type_uint(D, 2), 61);
    uint32_t a = ts_sig(m, "a", IRS_IN, u8, 62), b = ts_sig(m, "b", IRS_IN, u8, 62);
    uint32_t sel = ts_sig(m, "sel", IRS_IN, ir_type_bool(D), 63);
    uint32_t y = ts_sig(m, "y", IRS_OUT, u8, 64), z = ts_sig(m, "z", IRS_OUT, u8, 65);
    uint32_t v = ts_sig(m, "v", IRS_VAR, u8, 66), w = ts_sig(m, "w", IRS_OUT, u8, 67);
    IrCaseArm arms[4];
    IrBits vals[4];
    memset(arms, 0, sizeof arms);
    vals[0] = ir_bits_u64(TA, 2, 0);
    vals[1] = ir_bits_u64(TA, 2, 1);
    vals[2] = ir_bits_u64(TA, 2, 2);
    arms[0].nvals = 1;
    arms[0].vals = &vals[0];
    arms[0].body = B1(ASG(m, y, BIN(IROP_ADD, S(m, a), S(m, b)), 69));
    arms[1].nvals = 1;
    arms[1].vals = &vals[1];
    arms[1].body = B1(ASG(m, y, BIN(IROP_SUB, S(m, a), S(m, b)), 70));
    arms[2].nvals = 1;
    arms[2].vals = &vals[2];
    arms[2].body = B1(ASG(m, y, BIN(IROP_AND, S(m, a), S(m, b)), 71));
    arms[3].nvals = 0;
    arms[3].body = B1(ASG(m, y, K(8, 0), 72));
    ir_add_process(D, m, IRP_COMB, IR_NONE, B1(ir_stmt_case(TA, S(m, op), 4, arms, SP(68))), SP(68));
    CONT(m, z, ir_mux(TA, S(m, sel), S(m, a), S(m, b), SP(73)));
    /* var with default and an if, read back inside the process */
    {
        IrStmt *st[3];
        st[0] = ASG(m, v, S(m, a), 74);
        st[1] = ir_stmt_if(TA, S(m, sel), B1(ASG(m, v, BIN(IROP_ADD, S(m, v), K(8, 1)), 76)), B0(), SP(75));
        st[2] = ASG(m, w, BIN(IROP_XOR, S(m, v), K(8, 0xFF)), 77);
        ir_add_process(D, m, IRP_COMB, IR_NONE, ir_block(TA, 3, st), SP(74));
    }
    return m;
}

static const SimCovPoint *ts_cov_find(const SimCovPoint *p, size_t n, SimCovKind k, uint32_t line,
                                      uint32_t arm)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i].kind == k && p[i].loc.line == line && p[i].arm == arm)
            return &p[i];
    return NULL;
}

static void test_alu_and_coverage(void)
{
    SimModel *sm;
    int op, a, b, sel, y, z, w;
    const SimCovPoint *cp, *x;
    size_t n;
    ts_new_design();
    ir_design_set_top(D, ts_alu());
    sm = sim_new(D, D->top, 0);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    TS_EQ(sim_domain_count(sm), 0);
    TS_EQ(sim_default_domain(sm), -1);
    op = sim_find(sm, "op");
    a = sim_find(sm, "a");
    b = sim_find(sm, "b");
    sel = sim_find(sm, "sel");
    y = sim_find(sm, "y");
    z = sim_find(sm, "z");
    w = sim_find(sm, "w");
    sim_coverage_reset(sm);
    sim_poke_u64(sm, a, 200);
    sim_poke_u64(sm, b, 100);
    sim_poke_u64(sm, op, 0);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, y), 44);
    TS_EQ(sim_peek_u64(sm, z), 100);
    TS_EQ(sim_peek_u64(sm, w), 200 ^ 0xFF);
    sim_poke_u64(sm, op, 1);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, y), 100);
    sim_poke_u64(sm, b, 201);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, y), (200 - 201) & 0xFF);
    sim_poke_u64(sm, op, 2);
    sim_poke_u64(sm, sel, 1);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, y), 200 & 201);
    TS_EQ(sim_peek_u64(sm, z), 200);
    TS_EQ(sim_peek_u64(sm, w), 201 ^ 0xFF);
    sim_settle(sm); /* not dirty: no evaluation */

    n = sim_coverage(sm, &cp);
    TS_CHECK(n > 0);
    x = ts_cov_find(cp, n, SIM_COV_ARM, 68, 0);
    TS_CHECK(x && x->count == 1);
    x = ts_cov_find(cp, n, SIM_COV_ARM, 68, 1);
    TS_CHECK(x && x->count == 2);
    x = ts_cov_find(cp, n, SIM_COV_ARM, 68, 2);
    TS_CHECK(x && x->count == 1);
    x = ts_cov_find(cp, n, SIM_COV_ARM, 68, 3);
    TS_CHECK(x && x->count == 0);          /* default never taken */
    x = ts_cov_find(cp, n, SIM_COV_STMT, 70, 0);
    TS_CHECK(x && x->count == 2);
    x = ts_cov_find(cp, n, SIM_COV_THEN, 75, 0);
    TS_CHECK(x && x->count == 1);
    x = ts_cov_find(cp, n, SIM_COV_ELSE, 75, 0);
    TS_CHECK(x && x->count == 3);
    x = ts_cov_find(cp, n, SIM_COV_STMT, 74, 0);
    TS_CHECK(x && x->count == 4);
    {   /* sorted */
        size_t i;
        int sorted = 1;
        for (i = 1; i < n; i++)
            if (cp[i - 1].loc.line > cp[i].loc.line)
                sorted = 0;
        TS_CHECK(sorted);
    }
    sim_free(sm);
}

/* two-instance hierarchy with output wires, and a clocked child */
static void test_hierarchy(void)
{
    IrModule *inc, *acc, *top;
    uint32_t i_x, i_y, a_clk, a_in, a_q, a_r, t_clk, t_a, t_o, t_w0, t_w1, t_qa, u0, u1, u2;
    SimModel *sm;
    IrType *u8 = NULL;
    ts_new_design();
    u8 = ir_type_uint(D, 8);
    inc = ir_module_new(D, "Inc", NULL, NULL, SP(80));
    i_x = ts_sig(inc, "x", IRS_IN, u8, 81);
    i_y = ts_sig(inc, "y", IRS_OUT, u8, 82);
    CONT(inc, i_y, BIN(IROP_ADD, S(inc, i_x), K(8, 1)));
    ir_design_add_module(D, inc);

    acc = ir_module_new(D, "Acc", NULL, NULL, SP(84));
    a_clk = ts_sig(acc, "clk", IRS_IN, ir_type_clock(D, 0), 85);
    a_in = ts_sig(acc, "inp", IRS_IN, u8, 86);
    a_q = ts_sig(acc, "q", IRS_OUT, u8, 87);
    a_r = ts_sig(acc, "r", IRS_REG, u8, 88);
    ir_add_domain(D, acc, "", a_clk, IR_NONE, 0);
    ir_set_init(D, acc, a_r, ir_bits_u64(TA, 8, 0));
    CONT(acc, a_q, S(acc, a_r));
    ir_add_process(D, acc, IRP_CLOCKED, 0, B1(ASG(acc, a_r, BIN(IROP_ADD, S(acc, a_r), S(acc, a_in)), 89)),
                   SP(89));
    ir_design_add_module(D, acc);

    top = ir_module_new(D, "Top", NULL, NULL, SP(90));
    t_clk = ts_sig(top, "clk", IRS_IN, ir_type_clock(D, 0), 91);
    t_a = ts_sig(top, "a", IRS_IN, u8, 92);
    t_o = ts_sig(top, "o", IRS_OUT, u8, 93);
    t_qa = ts_sig(top, "qa", IRS_OUT, u8, 93);
    t_w0 = ts_sig(top, "c0_y", IRS_WIRE, u8, 94);
    t_w1 = ts_sig(top, "c1_y", IRS_WIRE, u8, 94);
    ir_add_domain(D, top, "", t_clk, IR_NONE, 0);
    /* declared out of order on purpose: c1 reads c0's output */
    u1 = ir_add_instance(D, top, "c1", inc, SP(96));
    ir_connect(D, top, u1, i_x, S(top, t_w0));
    ir_connect(D, top, u1, i_y, S(top, t_w1));
    u0 = ir_add_instance(D, top, "c0", inc, SP(95));
    ir_connect(D, top, u0, i_x, S(top, t_a));
    ir_connect(D, top, u0, i_y, S(top, t_w0));
    u2 = ir_add_instance(D, top, "acc", acc, SP(97));
    ir_connect(D, top, u2, a_clk, S(top, t_clk));
    ir_connect(D, top, u2, a_in, S(top, t_w1));
    ir_connect(D, top, u2, a_q, S(top, t_qa));
    CONT(top, t_o, S(top, t_w1));
    ir_design_set_top(D, top);

    sm = sim_new(D, D->top, 5);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    sim_poke_u64(sm, sim_find(sm, "a"), 10);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "o")), 12);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "c0.y")), 11);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "c1.x")), 11);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "c1.y")), 12);
    TS_EQ(sim_poke_u64(sm, sim_find(sm, "c1.x"), 3), SIM_ERR_NOT_WRITABLE);
    TS_EQ(sim_find(sm, "c2.y"), -1);
    sim_step(sm, 0, 3);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "acc.r")), 36);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qa")), 36);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "acc.clk")), 0);
    {
        int i, n = sim_signal_count(sm), found = 0;
        for (i = 0; i < n; i++)
            if (strcmp(sim_signal_path(sm, i), "acc.q") == 0 && strcmp(sim_signal_name(sm, i), "q") == 0)
                found = 1;
        TS_CHECK(found);
    }
    sim_free(sm);
}

/* two domains, run_ns, falling-edge clock */
static void test_domains(void)
{
    IrModule *m;
    uint32_t ca, cb, cf, ra, rb, rf, qa, qb, qf;
    SimModel *sm;
    IrType *u16;
    ts_new_design();
    u16 = ir_type_uint(D, 16);
    m = ir_module_new(D, "Two", NULL, NULL, SP(100));
    ca = ts_sig(m, "clk_a", IRS_IN, ir_type_clock(D, 0), 101);
    cb = ts_sig(m, "clk_b", IRS_IN, ir_type_clock(D, 0), 102);
    cf = ts_sig(m, "clk_f", IRS_IN, ir_type_clock(D, 1), 103);
    ra = ts_sig(m, "ra", IRS_REG, u16, 104);
    rb = ts_sig(m, "rb", IRS_REG, u16, 105);
    rf = ts_sig(m, "rf", IRS_REG, u16, 106);
    qa = ts_sig(m, "qa", IRS_OUT, u16, 107);
    qb = ts_sig(m, "qb", IRS_OUT, u16, 108);
    qf = ts_sig(m, "qf", IRS_OUT, u16, 109);
    ir_add_domain(D, m, "a", ca, IR_NONE, 100000000);
    ir_add_domain(D, m, "b", cb, IR_NONE, 50000000);
    ir_add_domain(D, m, "f", cf, IR_NONE, 0);
    ir_set_init(D, m, ra, ir_bits_u64(TA, 16, 0));
    ir_set_init(D, m, rb, ir_bits_u64(TA, 16, 0));
    ir_set_init(D, m, rf, ir_bits_u64(TA, 16, 0));
    /* rb samples ra (a crossing, fine in simulation) */
    ir_add_process(D, m, IRP_CLOCKED, 0, B1(ASG(m, ra, BIN(IROP_ADD, S(m, ra), K(16, 1)), 110)), SP(110));
    ir_add_process(D, m, IRP_CLOCKED, 1, B1(ASG(m, rb, BIN(IROP_ADD, S(m, rb), K(16, 1)), 111)), SP(111));
    ir_add_process(D, m, IRP_CLOCKED, 2, B1(ASG(m, rf, BIN(IROP_ADD, S(m, rf), K(16, 1)), 112)), SP(112));
    CONT(m, qa, S(m, ra));
    CONT(m, qb, S(m, rb));
    CONT(m, qf, S(m, rf));
    ir_design_set_top(D, m);
    TS_EQ(m->default_domain, IR_NONE);

    sm = sim_new(D, D->top, 1);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    TS_EQ(sim_domain_count(sm), 3);
    TS_EQ(sim_find_domain(sm, "b"), 1);
    TS_EQ(sim_find_domain(sm, "zz"), -1);
    TS_EQ(sim_domain_hz(sm, 2), 100000000);
    sim_step(sm, 0, 3);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qa")), 3);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qb")), 0);
    sim_step(sm, 1, 2);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qa")), 3);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qb")), 2);
    /* falling-edge clock: starts high, one step = one falling edge */
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "clk_f")), 1);
    sim_step(sm, 2, 1);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qf")), 1);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "clk_f")), 1);
    sim_free(sm);

    sm = sim_new(D, D->top, 1);
    if (!sm)
        return;
    sim_run_ns(sm, 100);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qa")), 10);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qb")), 5);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qf")), 10);
    TS_EQ(sim_cycle(sm, 1), 5);
    TS_EQ(sim_time_ps(sm), 100000);
    sim_run_ns(sm, 1000);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qa")), 110);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "qb")), 55);
    sim_free(sm);
}

/* wide arithmetic (130 bits) against bignum */
static void test_wide(void)
{
    IrModule *m;
    IrType *u130, *s130;
    uint32_t a, b, sh, sum, dif, prd, shl, shr, ashr, ltu, lts, cat, slc, quo, rem, neg, mx, idx, sx;
    SimModel *sm;
    int iter;
    ts_new_design();
    u130 = ir_type_uint(D, 130);
    s130 = ir_type_sint(D, 130);
    m = ir_module_new(D, "Wide", NULL, NULL, SP(120));
    a = ts_sig(m, "a", IRS_IN, u130, 121);
    b = ts_sig(m, "b", IRS_IN, u130, 122);
    sh = ts_sig(m, "sh", IRS_IN, ir_type_uint(D, 8), 123);
    sum = ts_sig(m, "sum", IRS_OUT, u130, 124);
    dif = ts_sig(m, "dif", IRS_OUT, u130, 125);
    prd = ts_sig(m, "prd", IRS_OUT, u130, 126);
    shl = ts_sig(m, "shl", IRS_OUT, u130, 127);
    shr = ts_sig(m, "shr", IRS_OUT, u130, 128);
    ashr = ts_sig(m, "ashr", IRS_OUT, s130, 129);
    ltu = ts_sig(m, "ltu", IRS_OUT, ir_type_bool(D), 130);
    lts = ts_sig(m, "lts", IRS_OUT, ir_type_bool(D), 131);
    cat = ts_sig(m, "cat", IRS_OUT, ir_type_bits(D, 200), 132);
    slc = ts_sig(m, "slc", IRS_OUT, ir_type_bits(D, 70), 133);
    quo = ts_sig(m, "quo", IRS_OUT, s130, 134);
    rem = ts_sig(m, "rem", IRS_OUT, u130, 135);
    neg = ts_sig(m, "neg", IRS_OUT, u130, 136);
    mx = ts_sig(m, "mx", IRS_OUT, u130, 137);
    idx = ts_sig(m, "idx", IRS_OUT, ir_type_bits(D, 13), 138);
    sx = ts_sig(m, "sx", IRS_OUT, ir_type_bits(D, 190), 139);
    CONT(m, sum, BIN(IROP_ADD, S(m, a), S(m, b)));
    CONT(m, dif, BIN(IROP_SUB, S(m, a), S(m, b)));
    CONT(m, prd, BIN(IROP_MUL, S(m, a), S(m, b)));
    CONT(m, shl, BIN(IROP_SHL, S(m, a), S(m, sh)));
    CONT(m, shr, BIN(IROP_SHR, S(m, a), S(m, sh)));
    CONT(m, ashr, SBIN(IROP_ASHR, S(m, a), S(m, sh)));
    CONT(m, ltu, BIN(IROP_LT, S(m, a), S(m, b)));
    CONT(m, lts, SBIN(IROP_GT, S(m, a), S(m, b)));
    {
        IrExpr *parts[3];
        parts[0] = ir_slice(TA, S(m, a), 0, 64, SP(1));     /* bits 199..136 */
        parts[1] = S(m, b);                                  /* bits 135..6 */
        parts[2] = ir_slice(TA, S(m, a), 124, 6, SP(1));    /* bits 5..0 */
        CONT(m, cat, ir_concat(TA, 3, parts, SP(1)));
    }
    CONT(m, slc, ir_slice(TA, S(m, a), 37, 70, SP(1)));
    CONT(m, quo, SBIN(IROP_DIV, S(m, a), S(m, b)));
    CONT(m, rem, BIN(IROP_MOD, S(m, a), S(m, b)));
    CONT(m, neg, ir_unary(TA, IROP_NEG, S(m, a), SP(1)));
    CONT(m, mx, ir_mux(TA, BIN(IROP_LT, S(m, a), S(m, b)), S(m, b), S(m, a), SP(1)));
    /* dynamic index into a 130-bit vector of 13-bit elements (10 of them) */
    CONT(m, idx, ir_index(TA, S(m, a), ir_resize(TA, S(m, sh), 4, 0, SP(1)), 13, SP(1)));
    CONT(m, sx, ir_resize(TA, S(m, a), 190, 1, SP(1)));
    ir_design_set_top(D, m);
    sm = sim_new(D, D->top, 0);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    for (iter = 0; iter < 400; iter++) {
        uint64_t va[3], vb[3], out[4];
        IrBits ba, bb, ob;
        BigInt A = {0}, Bn = {0}, SA = {0}, SB = {0}, X = {0}, Q = {0}, R = {0};
        uint64_t s = ts_rand() % 140;
        ts_rand_bits(va, 130);
        ts_rand_bits(vb, 130);
        if (iter % 7 == 0)
            memcpy(vb, va, sizeof va);
        ba.width = bb.width = 130;
        ba.w = va;
        bb.w = vb;
        sim_poke(sm, sim_find(sm, "a"), &ba);
        sim_poke(sm, sim_find(sm, "b"), &bb);
        sim_poke_u64(sm, sim_find(sm, "sh"), s);
        sim_settle(sm);
        bn_from_limbs(&A, va, 130, 0);
        bn_from_limbs(&Bn, vb, 130, 0);
        bn_from_limbs(&SA, va, 130, 1);
        bn_from_limbs(&SB, vb, 130, 1);
        ob.w = out;
#define TS_WIDE(name, ref, wd)                         \
    do {                                                  \
        memset(out, 0, sizeof out);                       \
        sim_peek(sm, sim_find(sm, name), &ob);            \
        TS_CHECK(ob.width == (wd));                    \
        TS_CHECK(ts_limbs_eq(out, (ref), (wd)));       \
    } while (0)
        bn_add(&X, &A, &Bn);
        TS_WIDE("sum", &X, 130);
        bn_sub(&X, &A, &Bn);
        TS_WIDE("dif", &X, 130);
        bn_mul(&X, &A, &Bn);
        TS_WIDE("prd", &X, 130);
        bn_shl(&X, &A, s);
        TS_WIDE("shl", &X, 130);
        bn_shr(&X, &A, s);
        TS_WIDE("shr", &X, 130);
        bn_shr(&X, &SA, s);
        TS_WIDE("ashr", &X, 130);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "ltu")), bn_cmp(&A, &Bn) < 0);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "lts")), bn_cmp(&SA, &SB) > 0);
        /* cat = a[63:0] ++ b ++ a[129:124] */
        {
            BigInt lo64 = {0}, hi6 = {0}, mask = {0};
            bn_set_u64(&mask, ~UINT64_C(0));
            bn_and(&lo64, &A, &mask);
            bn_shr(&hi6, &A, 124);
            bn_shl(&X, &lo64, 136);
            bn_shl(&lo64, &Bn, 6);
            bn_or(&X, &X, &lo64);
            bn_or(&X, &X, &hi6);
            TS_WIDE("cat", &X, 200);
            bn_free(&lo64);
            bn_free(&hi6);
            bn_free(&mask);
        }
        bn_shr(&X, &A, 37);
        TS_WIDE("slc", &X, 70);
        if (bn_is_zero(&SB)) {
            bn_set_u64(&Q, 0);
            bn_copy(&R, &A);
        } else {
            bn_divmod_trunc(&Q, NULL, &SA, &SB);
            bn_divmod_trunc(NULL, &R, &A, &Bn);
        }
        TS_WIDE("quo", &Q, 130);
        TS_WIDE("rem", &R, 130);
        bn_neg(&X, &A);
        TS_WIDE("neg", &X, 130);
        TS_WIDE("mx", bn_cmp(&A, &Bn) < 0 ? &Bn : &A, 130);
        {
            uint64_t ix = s & 15;
            if (ix < 10)
                bn_shr(&X, &A, ix * 13);
            else
                bn_set_u64(&X, 0);
            TS_WIDE("idx", &X, 13);
        }
        TS_WIDE("sx", &SA, 190);
#undef TS_WIDE
        bn_free(&A);
        bn_free(&Bn);
        bn_free(&SA);
        bn_free(&SB);
        bn_free(&X);
        bn_free(&Q);
        bn_free(&R);
    }
    sim_free(sm);
}

/* narrow arithmetic corner cases through the interpreter (vs bitvec) */
static void test_narrow_ops(void)
{
    static const IrOp ops[] = {IROP_ADD, IROP_SUB, IROP_MUL, IROP_DIV, IROP_MOD, IROP_AND, IROP_OR,
                               IROP_XOR, IROP_SHL, IROP_SHR, IROP_ASHR, IROP_EQ, IROP_NE, IROP_LT,
                               IROP_LE, IROP_GT, IROP_GE};
    static const uint32_t widths[] = {1, 5, 31, 63, 64};
    uint32_t wi, oi;
    for (wi = 0; wi < TERN_ARRAY_LEN(widths); wi++) {
        uint32_t w = widths[wi];
        IrModule *m;
        uint32_t a, b, outs[2 * TERN_ARRAY_LEN(ops)];
        SimModel *sm;
        int iter;
        ts_new_design();
        m = ir_module_new(D, "N", NULL, NULL, SP(1));
        a = ts_sig(m, "a", IRS_IN, ir_type_uint(D, w), 1);
        b = ts_sig(m, "b", IRS_IN, ir_type_uint(D, w), 1);
        for (oi = 0; oi < 2 * TERN_ARRAY_LEN(ops); oi++) {
            IrOp op = ops[oi / 2];
            int sg = (int)(oi & 1);
            int cmp = op >= IROP_EQ;
            char name[16];
            snprintf(name, sizeof name, "o%u", oi);
            outs[oi] = ts_sig(m, name, IRS_OUT, cmp ? ir_type_bool(D) : ir_type_uint(D, w), 1);
            CONT(m, outs[oi], ir_binary(TA, op, S(m, a), S(m, b), sg, SP(1)));
        }
        ir_design_set_top(D, m);
        sm = sim_new(D, D->top, 0);
        TS_CHECK(sm != NULL);
        if (!sm)
            continue;
        for (iter = 0; iter < 300; iter++) {
            uint64_t va, vb, scratch[8];
            ts_rand_bits(&va, w);
            ts_rand_bits(&vb, w);
            if (iter % 5 == 0)
                vb = (ts_rand() % (w + 2)) & (w == 64 ? ~UINT64_C(0) : ((UINT64_C(1) << w) - 1u));
            sim_poke_u64(sm, sim_find(sm, "a"), va);
            sim_poke_u64(sm, sim_find(sm, "b"), vb);
            sim_settle(sm);
            for (oi = 0; oi < 2 * TERN_ARRAY_LEN(ops); oi++) {
                IrOp op = ops[oi / 2];
                int sg = (int)(oi & 1);
                uint64_t want = 0;
                switch (op) {
                case IROP_ADD: bv_add(&want, &va, &vb, w); break;
                case IROP_SUB: bv_sub(&want, &va, &vb, w); break;
                case IROP_MUL: bv_mul(&want, &va, &vb, w, scratch); break;
                case IROP_DIV: bv_divmod(&want, NULL, &va, &vb, w, sg, scratch); break;
                case IROP_MOD: bv_divmod(NULL, &want, &va, &vb, w, sg, scratch); break;
                case IROP_AND: want = va & vb; break;
                case IROP_OR: want = va | vb; break;
                case IROP_XOR: want = va ^ vb; break;
                case IROP_SHL: bv_shl(&want, &va, w, vb); break;
                case IROP_SHR: bv_shr(&want, &va, w, vb); break;
                case IROP_ASHR: bv_ashr(&want, &va, w, vb); break;
                case IROP_EQ: want = va == vb; break;
                case IROP_NE: want = va != vb; break;
                case IROP_LT: want = bv_cmp(&va, &vb, w, sg) < 0; break;
                case IROP_LE: want = bv_cmp(&va, &vb, w, sg) <= 0; break;
                case IROP_GT: want = bv_cmp(&va, &vb, w, sg) > 0; break;
                default: want = bv_cmp(&va, &vb, w, sg) >= 0; break;
                }
                if (op == IROP_ASHR && !sg)
                    bv_ashr(&want, &va, w, vb); /* ASHR is arithmetic regardless */
                TS_EQ(sim_peek_u64(sm, sim_find(sm, sim_signal_path(sm, (int)(outs[oi])))), want);
            }
        }
        sim_free(sm);
    }
}

static void test_comb_loop(void)
{
    IrModule *m;
    uint32_t x, y, z, o;
    SimModel *sm;
    ts_new_design();
    m = ir_module_new(D, "Loop", NULL, NULL, SP(140));
    x = ts_sig(m, "x", IRS_IN, ir_type_uint(D, 4), 141);
    y = ts_sig(m, "a", IRS_WIRE, ir_type_uint(D, 4), 142);
    z = ts_sig(m, "b", IRS_WIRE, ir_type_uint(D, 4), 143);
    o = ts_sig(m, "o", IRS_OUT, ir_type_uint(D, 4), 144);
    ir_add_assign(D, m, ir_lv_sig(m, y), BIN(IROP_ADD, S(m, z), S(m, x)), SP(145));
    ir_add_assign(D, m, ir_lv_sig(m, z), S(m, y), SP(146));
    CONT(m, o, S(m, z));
    ir_design_set_top(D, m);
    ts_diag_n = 0;
    ts_diag_msg[0] = 0;
    diag_set_sink(ts_diag_sink, NULL);
    sm = sim_new(D, D->top, 0);
    diag_flush();
    diag_set_sink(NULL, NULL);
    TS_CHECK(sm == NULL);
    TS_EQ(ts_diag_n, 1);
    TS_CHECK(strstr(ts_diag_msg, "combinational loop") != NULL);
    TS_CHECK(strstr(ts_diag_msg, "a -> b -> a") != NULL || strstr(ts_diag_msg, "b -> a -> b") != NULL);
    sim_free(sm);

    /* bit-level independence is not a loop: v[0] = x[0]; v[1] = v[0] */
    ts_new_design();
    m = ir_module_new(D, "NoLoop", NULL, NULL, SP(150));
    x = ts_sig(m, "x", IRS_IN, ir_type_uint(D, 1), 151);
    y = ts_sig(m, "v", IRS_WIRE, ir_type_uint(D, 2), 152);
    ir_add_assign(D, m, ir_lv_slice(m, y, 0, 1), S(m, x), SP(153));
    ir_add_assign(D, m, ir_lv_slice(m, y, 1, 1), ir_slice(TA, S(m, y), 0, 1, SP(1)), SP(154));
    ir_design_set_top(D, m);
    sm = sim_new(D, D->top, 0);
    TS_CHECK(sm != NULL);
    if (sm) {
        sim_poke_u64(sm, sim_find(sm, "x"), 1);
        sim_settle(sm);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "v")), 3);
        sim_free(sm);
    }
}

/* comb assertion in a child, reported with the instance path */
static void test_comb_assert(void)
{
    IrModule *c, *top;
    uint32_t cx, tx, inst;
    SimModel *sm;
    TsFails tf;
    ts_new_design();
    c = ir_module_new(D, "Chk", NULL, NULL, SP(160));
    cx = ts_sig(c, "x", IRS_IN, ir_type_uint(D, 4), 161);
    ir_add_process(D, c, IRP_COMB, IR_NONE,
                   B1(ir_stmt_assert(TA, IRA_ASSERT, BIN(IROP_NE, S(c, cx), K(4, 7)), "x must not be 7",
                                     SP(162))),
                   SP(162));
    ir_design_add_module(D, c);
    top = ir_module_new(D, "T", NULL, NULL, SP(165));
    tx = ts_sig(top, "x", IRS_IN, ir_type_uint(D, 4), 166);
    inst = ir_add_instance(D, top, "chk", c, SP(167));
    ir_connect(D, top, inst, cx, S(top, tx));
    ir_design_set_top(D, top);
    sm = sim_new(D, D->top, 0);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    memset(&tf, 0, sizeof tf);
    sim_set_fail_handler(sm, ts_on_fail, &tf);
    sim_poke_u64(sm, sim_find(sm, "x"), 3);
    sim_settle(sm);
    TS_EQ(tf.n, 0);
    sim_poke_u64(sm, sim_find(sm, "x"), 7);
    sim_settle(sm);
    TS_EQ(tf.n, 1);
    TS_CHECK(strcmp(tf.inst, "chk") == 0);
    TS_CHECK(strstr(tf.what, "assertion failed: x must not be 7") != NULL);
    TS_EQ(tf.line, 162);
    TS_EQ(sim_fail_count(sm), 1);
    sim_free(sm);
}

/* property monitor harness: clk, rst, inputs a, b, c; y and q = reg(y) */
typedef IrSeq *(*TsSeqFn)(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q);

static SimModel *ts_prop_model(TsSeqFn fn, IrAssertKind kind, TsFails *tf)
{
    IrModule *m;
    uint32_t clk, rst, a, b, y, q;
    SimModel *sm;
    ts_new_design();
    m = ir_module_new(D, "P", NULL, NULL, SP(170));
    clk = ts_sig(m, "clk", IRS_IN, ir_type_clock(D, 0), 171);
    rst = ts_sig(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 172);
    a = ts_sig(m, "a", IRS_IN, ir_type_bool(D), 173);
    b = ts_sig(m, "b", IRS_IN, ir_type_bool(D), 174);
    y = ts_sig(m, "y", IRS_IN, ir_type_uint(D, 8), 175);
    q = ts_sig(m, "q", IRS_REG, ir_type_uint(D, 8), 176);
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_add_process(D, m, IRP_CLOCKED, 0, B1(ASG(m, q, S(m, y), 177)), SP(177));
    ir_add_property(D, m, "p", kind, 0, fn(m, a, b, y, q), SP(178));
    ir_design_set_top(D, m);
    sm = sim_new(D, D->top, 9);
    if (sm) {
        memset(tf, 0, sizeof *tf);
        sim_set_fail_handler(sm, ts_on_fail, tf);
        sim_reset(sm);
    }
    return sm;
}

static void ts_ab(SimModel *sm, int a, int b, uint32_t n)
{
    sim_poke_u64(sm, sim_find(sm, "a"), (uint64_t)a);
    sim_poke_u64(sm, sim_find(sm, "b"), (uint64_t)b);
    sim_step(sm, 0, n);
}

static IrSeq *ts_seq_next(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_ALWAYS, ir_seq_binary(TA, IRQ_IMPL_NEXT, QE(S(m, a)), QE(S(m, b))));
}

static IrSeq *ts_seq_range(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    /* always (a |-> ##[1:3] b)  ==  always (a |-> (true ##[1:3] b)) */
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_ALWAYS,
                        ir_seq_binary(TA, IRQ_IMPL, QE(S(m, a)),
                                      ir_seq_delay(TA, QE(K(1, 1)), 1, 3, QE(S(m, b)))));
}

static IrSeq *ts_seq_ev(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_ALWAYS,
                        ir_seq_binary(TA, IRQ_IMPL, QE(S(m, a)), ir_seq_eventually(TA, QE(S(m, b)), 4)));
}

static IrSeq *ts_seq_ev_unb(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_ALWAYS,
                        ir_seq_binary(TA, IRQ_IMPL, QE(S(m, a)), ir_seq_eventually(TA, QE(S(m, b)), IR_NONE)));
}

static IrSeq *ts_seq_past1(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(a);
    TERN_UNUSED(b);
    return ir_seq_unary(TA, IRQ_ALWAYS, QE(BIN(IROP_EQ, S(m, q), ir_past(TA, S(m, y), 1, SP(1)))));
}

static IrSeq *ts_seq_past2(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(a);
    TERN_UNUSED(b);
    return ir_seq_unary(TA, IRQ_ALWAYS, QE(BIN(IROP_EQ, S(m, q), ir_past(TA, S(m, y), 2, SP(1)))));
}

static IrSeq *ts_seq_cover(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_delay(TA, QE(S(m, a)), 1, 1, QE(S(m, b)));
}

static IrSeq *ts_seq_never(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_NEVER, QE(BIN(IROP_LAND, S(m, a), S(m, b))));
}

static IrSeq *ts_seq_until(IrModule *m, uint32_t a, uint32_t b, uint32_t y, uint32_t q)
{
    /* always (a |=> (a until b)) : once a, a stays until b */
    TERN_UNUSED(y);
    TERN_UNUSED(q);
    return ir_seq_unary(TA, IRQ_ALWAYS,
                        ir_seq_binary(TA, IRQ_IMPL_NEXT, QE(S(m, a)),
                                      ir_seq_binary(TA, IRQ_UNTIL, QE(S(m, a)), QE(S(m, b)))));
}

static void test_properties(void)
{
    SimModel *sm;
    TsFails tf;
    int i;

    /* a |=> b */
    sm = ts_prop_model(ts_seq_next, IRA_ASSERT, &tf);
    TS_CHECK(sm != NULL);
    if (sm) {
        ts_ab(sm, 0, 0, 3);
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 1, 1);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 1);
        TS_EQ(tf.n, 1);
        TS_EQ(tf.kind, SIM_FAIL_PROPERTY);
        TS_CHECK(strstr(tf.what, "property `p` failed") != NULL);
        TS_EQ(tf.line, 178);
        TS_EQ(tf.cycle, 8);   /* 0-based index of the failing edge: 2 reset + 3 + 1 + 1 + 1 before it */
        ts_ab(sm, 0, 0, 5);
        TS_EQ(tf.n, 1);
        /* a every cycle with b one cycle late but always true afterwards */
        ts_ab(sm, 1, 1, 10);
        TS_EQ(tf.n, 1);
        sim_free(sm);
    }

    /* a |-> ##[1:3] b */
    sm = ts_prop_model(ts_seq_range, IRA_ASSERT, &tf);
    TS_CHECK(sm != NULL);
    if (sm) {
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 2);
        ts_ab(sm, 0, 1, 1);   /* third cycle after: ok */
        TS_EQ(tf.n, 0);
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 2);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 0, 0, 1);   /* window closed */
        TS_EQ(tf.n, 1);
        TS_EQ(tf.start, 6);
        ts_ab(sm, 0, 0, 4);
        TS_EQ(tf.n, 1);
        /* overlapping attempts collapse but each is checked */
        ts_ab(sm, 1, 0, 3);
        ts_ab(sm, 0, 1, 1);
        TS_EQ(tf.n, 1);
        sim_free(sm);
    }

    /* a |-> eventually[..4] b */
    sm = ts_prop_model(ts_seq_ev, IRA_ASSERT, &tf);
    if (sm) {
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 3);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 0, 1, 1);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 3);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 0, 0, 1);
        TS_EQ(tf.n, 1);
        TS_EQ(sim_finish(sm), 0);
        sim_free(sm);
    }

    /* unbounded eventually: pending at the end */
    sm = ts_prop_model(ts_seq_ev_unb, IRA_ASSERT, &tf);
    if (sm) {
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 0, 50);
        TS_EQ(tf.n, 0);
        TS_EQ(sim_finish(sm), 1);
        TS_EQ(tf.kind, SIM_FAIL_PENDING);
        ts_ab(sm, 0, 1, 1);
        TS_EQ(sim_finish(sm), 0);
        sim_free(sm);
    }

    /* past(y, 1) holds for q = reg(y); past(y, 2) does not */
    sm = ts_prop_model(ts_seq_past1, IRA_ASSERT, &tf);
    if (sm) {
        for (i = 0; i < 40; i++) {
            sim_poke_u64(sm, sim_find(sm, "y"), ts_rand() & 0xFF);
            sim_step(sm, 0, 1);
        }
        TS_EQ(tf.n, 0);
        sim_free(sm);
    }
    sm = ts_prop_model(ts_seq_past2, IRA_ASSERT, &tf);
    if (sm) {
        for (i = 0; i < 10; i++) {
            sim_poke_u64(sm, sim_find(sm, "y"), (uint64_t)i);
            sim_step(sm, 0, 1);
        }
        TS_CHECK(tf.n >= 1);
        sim_free(sm);
    }

    /* cover a ##1 b: counts completed matches */
    sm = ts_prop_model(ts_seq_cover, IRA_COVER, &tf);
    if (sm) {
        const SimCovPoint *cp, *x;
        size_t n;
        ts_ab(sm, 1, 0, 1);
        ts_ab(sm, 0, 1, 1);   /* hit */
        ts_ab(sm, 1, 1, 3);   /* hits at the 2nd and 3rd of these edges */
        ts_ab(sm, 0, 0, 3);   /* the last a is followed by !b: no hit */
        n = sim_coverage(sm, &cp);
        x = ts_cov_find(cp, n, SIM_COV_PROPERTY, 178, 0);
        TS_CHECK(x != NULL);
        if (x) {
            TS_EQ(x->count, 3);
            TS_CHECK(tern_streq(x->name, "p"));
        }
        TS_EQ(tf.n, 0);
        sim_free(sm);
    }

    /* assume: reported as an assumption violation */
    sm = ts_prop_model(ts_seq_never, IRA_ASSUME, &tf);
    if (sm) {
        ts_ab(sm, 1, 0, 2);
        ts_ab(sm, 0, 1, 2);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 1, 1, 1);
        TS_EQ(tf.n, 1);
        TS_EQ(tf.kind, SIM_FAIL_ASSUME);
        TS_CHECK(strstr(tf.what, "assumption violated") != NULL);
        sim_free(sm);
    }

    /* until */
    sm = ts_prop_model(ts_seq_until, IRA_ASSERT, &tf);
    if (sm) {
        ts_ab(sm, 1, 0, 3);
        ts_ab(sm, 0, 1, 1);   /* released by b */
        ts_ab(sm, 0, 0, 2);
        TS_EQ(tf.n, 0);
        ts_ab(sm, 1, 0, 2);
        ts_ab(sm, 0, 0, 1);   /* a dropped without b */
        TS_EQ(tf.n, 1);
        sim_free(sm);
    }
}

/* immediate assertions are suppressed during reset; coverage for cover */
static void test_vcd(void)
{
    SimModel *sm;
    char *text = NULL;
    size_t len = 0;
    ts_new_design();
    ir_design_set_top(D, ts_fifo());
    sm = sim_new(D, D->top, 2);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    os_mkdir_p("build");
    TS_EQ(sim_vcd_open(sm, "build/test_sim.vcd"), SIM_OK);
    sim_reset(sm);
    sim_poke_u64(sm, sim_find(sm, "push.valid"), 1);
    sim_poke_u64(sm, sim_find(sm, "push.data"), 0x5A);
    sim_step(sm, 0, 3);
    sim_vcd_close(sm);
    TS_CHECK(os_read_file("build/test_sim.vcd", &text, &len) == 0);
    if (text) {
        TS_CHECK(strstr(text, "$timescale 1ps $end") != NULL);
        TS_CHECK(strstr(text, "$scope module Fifo_u8_4 $end") != NULL);
        TS_CHECK(strstr(text, " push_data [7:0] $end") != NULL);
        TS_CHECK(strstr(text, "$var reg 3 ") != NULL);
        TS_CHECK(strstr(text, "$enddefinitions $end") != NULL);
        TS_CHECK(strstr(text, "#50000\n") != NULL);
        TS_CHECK(strstr(text, "b1011010 ") != NULL);
        TS_CHECK(strstr(text, "mem") == NULL);
        free(text);
    }
    sim_free(sm);
}

/* nested hierarchy with VCD scopes */
static void test_vcd_scopes(void)
{
    IrModule *leaf, *mid, *top;
    uint32_t lx, ly, mx, my, tx, ty, w1, w2, i1, i2;
    SimModel *sm;
    char *text = NULL;
    size_t len = 0;
    ts_new_design();
    leaf = ir_module_new(D, "Leaf", NULL, NULL, SP(180));
    lx = ts_sig(leaf, "x", IRS_IN, ir_type_uint(D, 4), 181);
    ly = ts_sig(leaf, "y", IRS_OUT, ir_type_uint(D, 4), 182);
    CONT(leaf, ly, ir_unary(TA, IROP_NOT, S(leaf, lx), SP(1)));
    ir_design_add_module(D, leaf);
    mid = ir_module_new(D, "Mid", NULL, NULL, SP(183));
    mx = ts_sig(mid, "x", IRS_IN, ir_type_uint(D, 4), 184);
    my = ts_sig(mid, "y", IRS_OUT, ir_type_uint(D, 4), 185);
    w1 = ts_sig(mid, "l_y", IRS_WIRE, ir_type_uint(D, 4), 186);
    i1 = ir_add_instance(D, mid, "l", leaf, SP(187));
    ir_connect(D, mid, i1, lx, S(mid, mx));
    ir_connect(D, mid, i1, ly, S(mid, w1));
    CONT(mid, my, S(mid, w1));
    ir_design_add_module(D, mid);
    top = ir_module_new(D, "Top", NULL, NULL, SP(188));
    tx = ts_sig(top, "x", IRS_IN, ir_type_uint(D, 4), 189);
    ty = ts_sig(top, "y", IRS_OUT, ir_type_uint(D, 4), 190);
    w2 = ts_sig(top, "m_y", IRS_WIRE, ir_type_uint(D, 4), 191);
    i2 = ir_add_instance(D, top, "m", mid, SP(192));
    ir_connect(D, top, i2, mx, S(top, tx));
    ir_connect(D, top, i2, my, S(top, w2));
    CONT(top, ty, S(top, w2));
    ir_design_set_top(D, top);
    sm = sim_new(D, D->top, 0);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    TS_CHECK(sim_find(sm, "m.l.y") >= 0);
    TS_EQ(sim_vcd_open(sm, "build/test_sim_scopes.vcd"), SIM_OK);
    sim_poke_u64(sm, sim_find(sm, "x"), 5);
    sim_settle(sm);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "y")), 10);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "m.l.y")), 10);
    sim_vcd_close(sm);
    TS_CHECK(os_read_file("build/test_sim_scopes.vcd", &text, &len) == 0);
    if (text) {
        const char *a = strstr(text, "$scope module Top $end");
        const char *b = strstr(text, "$scope module m $end");
        const char *c = strstr(text, "$scope module l $end");
        TS_CHECK(a && b && c && a < b && b < c);
        free(text);
    }
    sim_free(sm);
}

/* kitchen-sink design for the JSON round trip */
static IrDesign *ts_kitchen_sink(void)
{
    IrModule *fifo, *top;
    IrType *u8, *pix, *st, *arr;
    IrField f[2];
    IrVariant v[3];
    uint32_t clk, rst, a, b, o, p, e, inst, w, ar;
    IrRegmap rm;
    IrReg regs[2];
    IrRegField rf[1];
    IrCrossing cr;
    ts_new_design();
    u8 = ir_type_uint(D, 8);
    memset(f, 0, sizeof f);
    f[0].name = "x";
    f[0].type = u8;
    f[1].name = "y";
    f[1].type = ir_type_sint(D, 4);
    pix = ir_type_struct(D, "Pos", 2, f, SP(200));
    memset(v, 0, sizeof v);
    v[0].name = "Idle";
    v[0].tag = ir_bits_u64(TA, 2, 0);
    v[1].name = "Run";
    v[1].tag = ir_bits_u64(TA, 2, 1);
    v[1].payload = ir_type_struct(D, NULL, 1, f, SP(201));
    v[2].name = "Done";
    v[2].tag = ir_bits_u64(TA, 2, 2);
    st = ir_type_enum(D, "State", 3, v, 2, IR_ENC_BINARY, SP(202));
    arr = ir_type_array(D, u8, 3);
    fifo = ts_fifo();
    ir_module_add_attr(D, fifo, "keep", NULL);
    ir_add_param(D, fifo, "Depth", ir_bits_u64(TA, 32, 4), 0);
    ir_signal_add_attr(D, fifo, ir_find_signal(fifo, "mem"), "ram", "distributed");
    fifo->package = intern_cstr("std::fifo");
    ir_design_add_module(D, fifo);

    top = ir_module_new(D, "Top", NULL, NULL, SP(210));
    clk = ts_sig(top, "clk", IRS_IN, ir_type_clock(D, 0), 211);
    rst = ts_sig(top, "rst", IRS_IN, ir_type_reset(D, 0, 0), 212);
    a = ts_sig(top, "a", IRS_IN, u8, 213);
    b = ts_sig(top, "b", IRS_IN, ir_type_bool(D), 214);
    o = ts_sig(top, "o", IRS_OUT, u8, 215);
    p = ts_sig(top, "p", IRS_REG, pix, 216);
    e = ts_sig(top, "e", IRS_REG, st, 217);
    w = ts_sig(top, "u_pop_data", IRS_WIRE, u8, 218);
    ar = ts_sig(top, "ar", IRS_WIRE, arr, 219);
    top->sigs[b].test_port = 1;
    ir_set_init(D, top, e, ir_bits_u64(TA, st->width, 0));
    ir_add_domain(D, top, "", clk, rst, 100000000);
    {
        uint32_t mi = ir_add_memory(D, top, "rom", u8, 3, SP(220));
        top->sigs[mi].mem_init = ANEWN(TA, IrBits, 3);
        top->sigs[mi].mem_init[0] = ir_bits_u64(TA, 8, 1);
        top->sigs[mi].mem_init[1] = ir_bits_u64(TA, 8, 2);
        top->sigs[mi].mem_init[2] = ir_bits_u64(TA, 8, 3);
        CONT(top, o, BIN(IROP_ADD, ir_memread(TA, top, mi, ir_resize(TA, S(top, b), 2, 0, SP(1)), SP(1)),
                         ir_index(TA, S(top, ar), ir_resize(TA, S(top, b), 2, 0, SP(1)), 8, SP(1))));
    }
    {
        IrExpr *parts[3];
        parts[0] = S(top, a);
        parts[1] = S(top, w);
        parts[2] = K(8, 0x42);
        CONT(top, ar, ir_concat(TA, 3, parts, SP(1)));
    }
    inst = ir_add_instance(D, top, "u", fifo, SP(221));
    ir_connect_name(D, top, inst, "clk", S(top, clk));
    ir_connect_name(D, top, inst, "rst", S(top, rst));
    ir_connect_name(D, top, inst, "push.valid", S(top, b));
    ir_connect_name(D, top, inst, "push_data", S(top, a));
    ir_connect_name(D, top, inst, "pop.ready", K(1, 1));
    ir_connect_name(D, top, inst, "raddr", K(2, 0));
    ir_connect_name(D, top, inst, "pop_data", S(top, w));
    {
        IrCaseArm arms[2];
        IrBits vals[2];
        IrStmt *body[2];
        memset(arms, 0, sizeof arms);
        vals[0] = ir_bits_u64(TA, 2, 0);
        vals[1] = ir_bits_u64(TA, 2, 1);
        arms[0].nvals = 2;
        arms[0].vals = vals;
        arms[0].body = B1(ir_stmt_assign(TA, ir_lv_slice(top, e, 0, 2), K(2, 2), SP(223)));
        arms[1].body = B1(ASG(top, e, K(st->width, 0), 224));
        body[0] = ir_stmt_case(TA, ir_slice(TA, S(top, e), st->width - 2, 2, SP(1)), 2, arms, SP(222));
        body[1] = ir_stmt_assign(TA, ir_lv_index(top, p, 0, 4, ir_resize(TA, S(top, b), 2, 0, SP(1))),
                                 ir_field(TA, S(top, p), "y", SP(1)), SP(225));
        body[1]->lhs.field = intern_cstr("y");
        ir_add_process(D, top, IRP_CLOCKED, 0, ir_block(TA, 2, body), SP(222));
    }
    ir_add_property(D, top, "prop", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL, QE(S(top, b)),
                                               ir_seq_delay(TA, QE(K(1, 1)), 1, IR_NONE,
                                                            QE(ir_past(TA, S(top, b), 2, SP(1)))))),
                    SP(226));
    ir_add_property(D, top, "cov", IRA_COVER, 0,
                    ir_seq_eventually(TA, ir_seq_unary(TA, IRQ_NOT, QE(S(top, b))), 3), SP(227));
    ir_design_set_top(D, top);

    memset(&rm, 0, sizeof rm);
    memset(regs, 0, sizeof regs);
    memset(rf, 0, sizeof rf);
    rf[0].name = intern_cstr("enable");
    rf[0].lo = 0;
    rf[0].width = 1;
    rf[0].type = ir_type_bool(D);
    regs[0].offset = 0;
    regs[0].name = intern_cstr("ctrl");
    regs[0].access = IR_RW;
    regs[0].width = 32;
    regs[0].nfields = 1;
    regs[0].fields = rf;
    regs[0].doc = "control \"register\"";
    regs[1].offset = 0x10;
    regs[1].name = intern_cstr("fifo");
    regs[1].access = IR_WO;
    regs[1].width = 8;
    regs[1].is_stream = 1;
    rm.name = intern_cstr("Ctrl");
    rm.nregs = 2;
    rm.regs = regs;
    rm.loc = SP(230);
    ir_design_add_regmap(D, &rm);
    memset(&cr, 0, sizeof cr);
    cr.path = intern_cstr("Top > u");
    cr.kind = IRC_SYNC2;
    cr.from_domain = intern_cstr("sys");
    cr.to_domain = intern_cstr("pix");
    cr.signal = intern_cstr("flag");
    ir_design_add_crossing(D, &cr);
    return D;
}

static void test_json_roundtrip(void)
{
    IrDesign *d1, *d2;
    StrBuf j1 = {0}, j2 = {0}, dump1 = {0}, dump2 = {0};
    Arena a2 = {0};
    d1 = ts_kitchen_sink();
    ir_to_json(d1, &j1);
    d2 = ir_from_json(&a2, j1.data, j1.len);
    TS_CHECK(d2 != NULL);
    if (d2) {
        ir_to_json(d2, &j2);
        TS_CHECK(j1.len == j2.len && strcmp(sb_cstr(&j1), sb_cstr(&j2)) == 0);
        ir_dump(d1, &dump1);
        ir_dump(d2, &dump2);
        TS_CHECK(dump1.len > 200 && strcmp(sb_cstr(&dump1), sb_cstr(&dump2)) == 0);
        TS_CHECK(strstr(sb_cstr(&dump1), "module Fifo_u8_4 \"Fifo<u8, 4>\" in std::fifo") != NULL);
        TS_CHECK(strstr(sb_cstr(&dump1), "assign push_ready = (cnt != 3'h4)") != NULL);
        TS_CHECK(d2->top && tern_streq(d2->top->name, "Top"));
        TS_EQ(d2->nmodules, 2);
        TS_EQ(d2->ntypes, 2);
        /* both designs simulate identically */
        {
            SimModel *s1 = sim_new(d1, d1->top, 4), *s2 = sim_new(d2, d2->top, 4);
            int i;
            TS_CHECK(s1 && s2);
            if (s1 && s2) {
                sim_reset(s1);
                sim_reset(s2);
                for (i = 0; i < 30; i++) {
                    uint64_t av = ts_rand() & 0xFF, bv = ts_rand() & 1;
                    sim_poke_u64(s1, sim_find(s1, "a"), av);
                    sim_poke_u64(s2, sim_find(s2, "a"), av);
                    TS_EQ(sim_poke_u64(s1, sim_find(s1, "b"), bv), SIM_OK); /* test_port */
                    sim_poke_u64(s2, sim_find(s2, "b"), bv);
                    sim_step(s1, 0, 1);
                    sim_step(s2, 0, 1);
                    TS_EQ(sim_peek_u64(s1, sim_find(s1, "o")), sim_peek_u64(s2, sim_find(s2, "o")));
                    TS_EQ(sim_peek_u64(s1, sim_find(s1, "u.count")), sim_peek_u64(s2, sim_find(s2, "u.count")));
                    TS_EQ(sim_peek_u64(s1, sim_find(s1, "e")), sim_peek_u64(s2, sim_find(s2, "e")));
                    TS_EQ(sim_peek_u64(s1, sim_find(s1, "p")), sim_peek_u64(s2, sim_find(s2, "p")));
                }
                TS_EQ(sim_fail_count(s1), sim_fail_count(s2));
            }
            sim_free(s1);
            sim_free(s2);
        }
    }
    /* malformed input */
    ts_diag_n = 0;
    diag_set_sink(ts_diag_sink, NULL);
    {
        const char *bad = "{\"format\":\"tern-ir\",\"version\":1,\"modules\":[{\"name\":\"M\","
                          "\"signals\":[{\"name\":\"x\",\"kind\":\"wire\",\"type\":5}]}]}";
        TS_CHECK(ir_from_json(&a2, bad, strlen(bad)) == NULL);
    }
    TS_CHECK(ir_from_json(&a2, "[1,2", 4) == NULL);
    TS_CHECK(ir_from_json(&a2, "{}", 2) == NULL);
    diag_flush();
    diag_set_sink(NULL, NULL);
    TS_EQ(ts_diag_n, 3);
    arena_release(&a2);
    sb_free(&j1);
    sb_free(&j2);
    sb_free(&dump1);
    sb_free(&dump2);
}

/* determinism of uninitialized state per seed */
static void test_seeds(void)
{
    IrModule *m;
    uint32_t clk, r, o, mem;
    SimModel *s1, *s2, *s3;
    ts_new_design();
    m = ir_module_new(D, "Rnd", NULL, NULL, SP(240));
    clk = ts_sig(m, "clk", IRS_IN, ir_type_clock(D, 0), 241);
    r = ts_sig(m, "r", IRS_REG, ir_type_uint(D, 100), 242);
    o = ts_sig(m, "o", IRS_OUT, ir_type_uint(D, 100), 243);
    mem = ir_add_memory(D, m, "mem", ir_type_uint(D, 32), 16, SP(244));
    ir_add_domain(D, m, "", clk, IR_NONE, 0);
    ir_add_process(D, m, IRP_CLOCKED, 0, B1(ASG(m, r, S(m, r), 245)), SP(245));
    CONT(m, o, S(m, r));
    TERN_UNUSED(mem);
    ir_design_set_top(D, m);
    s1 = sim_new(D, D->top, 42);
    s2 = sim_new(D, D->top, 42);
    s3 = sim_new(D, D->top, 43);
    TS_CHECK(s1 && s2 && s3);
    if (s1 && s2 && s3) {
        uint64_t v1[2], v2[2], v3[2];
        IrBits b1, b2, b3;
        b1.w = v1;
        b2.w = v2;
        b3.w = v3;
        sim_peek(s1, sim_find(s1, "o"), &b1);
        sim_peek(s2, sim_find(s2, "o"), &b2);
        sim_peek(s3, sim_find(s3, "o"), &b3);
        TS_CHECK(v1[0] == v2[0] && v1[1] == v2[1]);
        TS_CHECK(v1[0] != v3[0] || v1[1] != v3[1]);
        TS_CHECK((v1[1] >> 36) == 0);
        sim_mem_peek(s1, sim_find(s1, "mem"), 5, &b1);
        sim_mem_peek(s2, sim_find(s2, "mem"), 5, &b2);
        TS_EQ(v1[0], v2[0]);
        TS_CHECK(v1[0] <= 0xFFFFFFFFu);
    }
    sim_free(s1);
    sim_free(s2);
    sim_free(s3);
}

static void test_perf(void)
{
    SimModel *sm;
    uint64_t t0, t1;
    const uint32_t cycles = 2000000;
    ts_new_design();
    ir_design_set_top(D, ts_counter("Counter", ir_type_reset(D, 0, 0), 32));
    sm = sim_new(D, D->top, 1);
    if (!sm)
        return;
    sim_poke_u64(sm, sim_find(sm, "en"), 1);
    sim_reset(sm);
    t0 = os_time_ns();
    sim_step(sm, 0, cycles);
    t1 = os_time_ns();
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "count")), cycles);
    printf("sim perf: counter, %u cycles in %.1f ms (%.1f M cycles/s)\n", cycles,
           (double)(t1 - t0) / 1e6, (double)cycles / ((double)(t1 - t0) / 1e3));
    sim_free(sm);
}


/* byte-masked memory writes, dynamic-index lvalues, immediate cover,
 * a register listed only in IrProcess.regs */
static void test_mem_mask_and_index(void)
{
    IrModule *m;
    IrType *u8 = NULL, *arr;
    uint32_t clk, rst, we, addr, data, mask, idx, d, rd, arr_r, held, mem, sel, o;
    SimModel *sm;
    IrStmtList body = {0};
    ts_new_design();
    u8 = ir_type_uint(D, 8);
    arr = ir_type_array(D, u8, 4);
    m = ir_module_new(D, "MemMask", NULL, NULL, SP(250));
    clk = ts_sig(m, "clk", IRS_IN, ir_type_clock(D, 0), 251);
    rst = ts_sig(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 251);
    we = ts_sig(m, "we", IRS_IN, ir_type_bool(D), 252);
    addr = ts_sig(m, "addr", IRS_IN, ir_type_uint(D, 3), 253);
    data = ts_sig(m, "data", IRS_IN, ir_type_uint(D, 20), 254);
    mask = ts_sig(m, "mask", IRS_IN, ir_type_bits(D, 3), 255);
    idx = ts_sig(m, "idx", IRS_IN, ir_type_uint(D, 2), 256);
    d = ts_sig(m, "d", IRS_IN, u8, 257);
    sel = ts_sig(m, "sel", IRS_IN, ir_type_uint(D, 2), 258);
    rd = ts_sig(m, "rd", IRS_OUT, ir_type_uint(D, 20), 259);
    o = ts_sig(m, "o", IRS_OUT, u8, 260);
    arr_r = ts_sig(m, "arr", IRS_REG, arr, 261);
    held = ts_sig(m, "held", IRS_REG, u8, 262);
    mem = ir_add_memory(D, m, "mem", ir_type_uint(D, 20), 6, SP(263));
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, arr_r, ir_bits_u64(TA, 32, 0));
    ir_set_init(D, m, held, ir_bits_u64(TA, 8, 0x5C));
    CONT(m, rd, ir_memread(TA, m, mem, S(m, addr), SP(264)));
    CONT(m, o, ir_index(TA, S(m, arr_r), S(m, sel), 8, SP(265)));
    ir_stmts_push(&body, ir_stmt_if(TA, S(m, we),
                                    B1(ir_stmt_memwrite(TA, m, mem, S(m, addr), S(m, data), S(m, mask), SP(267))),
                                    B0(), SP(266)));
    ir_stmts_push(&body, ir_stmt_assign(TA, ir_lv_index(m, arr_r, 0, 8, S(m, idx)), S(m, d), SP(268)));
    ir_stmts_push(&body, ir_stmt_assert(TA, IRA_COVER, BIN(IROP_EQ, S(m, d), K(8, 0x99)), "d is 0x99",
                                        SP(269)));
    ir_add_process(D, m, IRP_CLOCKED, 0, ir_stmts_finish(TA, &body), SP(266));
    /* `held` is only listed as a register of the process (reset, never assigned) */
    {
        IrProcess *p = &m->procs[m->nprocs - 1];
        uint32_t *regs = ANEWN(TA, uint32_t, p->nregs + 1);
        memcpy(regs, p->regs, sizeof(uint32_t) * p->nregs);
        regs[p->nregs] = held;
        p->regs = regs;
        p->nregs++;
    }
    ir_design_set_top(D, m);
    sm = sim_new(D, D->top, 3);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    {
        int hm = sim_find(sm, "mem");
        IrBits z;
        uint64_t zl = 0;
        const SimCovPoint *cp, *x;
        size_t n;
        z.width = 20;
        z.w = &zl;
        sim_mem_poke(sm, hm, 2, &z);
        sim_reset(sm);
        sim_coverage_reset(sm);
        sim_poke_u64(sm, sim_find(sm, "we"), 1);
        sim_poke_u64(sm, sim_find(sm, "addr"), 2);
        sim_poke_u64(sm, sim_find(sm, "data"), 0xABCDE);
        sim_poke_u64(sm, sim_find(sm, "mask"), 5);     /* bytes 0 and 2 (a 4-bit partial byte) */
        sim_poke_u64(sm, sim_find(sm, "idx"), 2);
        sim_poke_u64(sm, sim_find(sm, "d"), 0x99);
        sim_step(sm, 0, 1);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "rd")), 0xA00DE);
        sim_poke_u64(sm, sim_find(sm, "mask"), 2);
        sim_poke_u64(sm, sim_find(sm, "data"), 0x12345);
        sim_poke_u64(sm, sim_find(sm, "idx"), 0);
        sim_poke_u64(sm, sim_find(sm, "d"), 0x11);
        sim_step(sm, 0, 1);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "rd")), 0xA23DE);
        sim_poke_u64(sm, sim_find(sm, "addr"), 7);    /* out of range: write ignored, read 0 */
        sim_step(sm, 0, 1);
        sim_settle(sm);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "rd")), 0);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "arr")), 0x00990011u);
        sim_poke_u64(sm, sim_find(sm, "sel"), 2);
        sim_settle(sm);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "o")), 0x99);
        TS_EQ(sim_peek_u64(sm, sim_find(sm, "held")), 0x5C);
        n = sim_coverage(sm, &cp);
        x = ts_cov_find(cp, n, SIM_COV_COVER, 269, 0);
        TS_CHECK(x && x->count == 1);
    }
    sim_free(sm);
}

/* a child domain on the falling edge of the top clock, and a derived clock
 * (a register toggling every cycle feeding a child's clock port) */
static void test_falling_and_derived(void)
{
    IrModule *cnt, *top;
    uint32_t clk, tog, inst1, inst2, w1, w2, o1, o2, cclk, ccnt;
    SimModel *sm;
    ts_new_design();
    cnt = ir_module_new(D, "FCnt", NULL, NULL, SP(270));
    cclk = ts_sig(cnt, "clk", IRS_IN, ir_type_clock(D, 1), 271);
    ccnt = ts_sig(cnt, "count", IRS_OUT, ir_type_uint(D, 8), 272);
    {
        uint32_t q = ts_sig(cnt, "q", IRS_REG, ir_type_uint(D, 8), 273);
        ir_add_domain(D, cnt, "", cclk, IR_NONE, 0);
        ir_set_init(D, cnt, q, ir_bits_u64(TA, 8, 0));
        CONT(cnt, ccnt, S(cnt, q));
        ir_add_process(D, cnt, IRP_CLOCKED, 0, B1(ASG(cnt, q, BIN(IROP_ADD, S(cnt, q), K(8, 1)), 274)), SP(274));
    }
    ir_design_add_module(D, cnt);
    top = ir_module_new(D, "Top", NULL, NULL, SP(275));
    clk = ts_sig(top, "clk", IRS_IN, ir_type_clock(D, 0), 276);
    tog = ts_sig(top, "tog", IRS_REG, ir_type_bool(D), 277);
    w1 = ts_sig(top, "f_count", IRS_WIRE, ir_type_uint(D, 8), 278);
    w2 = ts_sig(top, "d_count", IRS_WIRE, ir_type_uint(D, 8), 279);
    o1 = ts_sig(top, "fc", IRS_OUT, ir_type_uint(D, 8), 280);
    o2 = ts_sig(top, "dc", IRS_OUT, ir_type_uint(D, 8), 281);
    ir_add_domain(D, top, "", clk, IR_NONE, 0);
    ir_set_init(D, top, tog, ir_bits_u64(TA, 1, 0));
    ir_add_process(D, top, IRP_CLOCKED, 0, B1(ASG(top, tog, ir_unary(TA, IROP_LNOT, S(top, tog), SP(1)), 282)),
                   SP(282));
    inst1 = ir_add_instance(D, top, "f", cnt, SP(283));
    ir_connect(D, top, inst1, cclk, S(top, clk));
    ir_connect(D, top, inst1, ccnt, S(top, w1));
    inst2 = ir_add_instance(D, top, "dv", cnt, SP(284));
    ir_connect(D, top, inst2, cclk, S(top, tog));     /* derived clock: falling edges of tog */
    ir_connect(D, top, inst2, ccnt, S(top, w2));
    CONT(top, o1, S(top, w1));
    CONT(top, o2, S(top, w2));
    ir_design_set_top(D, top);
    sm = sim_new(D, D->top, 1);
    TS_CHECK(sm != NULL);
    if (!sm)
        return;
    sim_step(sm, 0, 8);
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "fc")), 8);   /* one falling edge per cycle */
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "dc")), 4);   /* tog falls every other cycle */
    TS_EQ(sim_peek_u64(sm, sim_find(sm, "f.clk")), 0);
    sim_free(sm);
}

int main(void)
{
    ts_setup_source();
    test_bitvec();
    test_ir_build();
    test_counter();
    test_async_reset();
    test_fifo();
    test_alu_and_coverage();
    test_hierarchy();
    test_domains();
    test_wide();
    test_narrow_ops();
    test_comb_loop();
    test_comb_assert();
    test_properties();
    test_vcd();
    test_vcd_scopes();
    test_json_roundtrip();
    test_seeds();
    test_mem_mask_and_index();
    test_falling_and_derived();
    test_perf();
    arena_release(&ts_arena);
    src_free_all();
    intern_free_all();
    if (ts_fails) {
        printf("sim unit tests: %d of %d checks FAILED\n", ts_fails, ts_checks);
        return 1;
    }
    printf("sim unit tests: all %d checks passed\n", ts_checks);
    return 0;
}
