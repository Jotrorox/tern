/* test_emit.c — tests for src/emit (SystemVerilog / Verilog-2005 emitter,
 * constraints, regmap outputs) and `tern emit`.
 *
 *     cc -o build/test_emit tests/unit/test_emit.c && build/test_emit
 *     UPDATE=1 build/test_emit          rewrite the golden files
 *
 * Designs are built with the IR builder API.  For each design the emitted
 * files (SV-2012 in tests/fixtures/emit/<name>/, Verilog-2005 in
 * tests/fixtures/emit/<name>_v2005/) are compared with golden copies, and
 * when Icarus Verilog is on PATH every file set is compiled with
 * `iverilog -g2012` / `-g2005`.  The cross-check designs additionally run a
 * generated random-stimulus testbench (tests/fixtures/emit/<name>/tb/tb.v)
 * under vvp and compare every cycle with the Tern simulator on the same IR.
 * Run from the repository root (make test does). */
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
#include "../../src/emit/sv.c"
#include "../../src/emit/svparam.c"
#include "../../src/emit/constraints.c"
#include "../../src/emit/regmap.c"
#include "../../src/emit/cmd_emit.c"
#include "../../src/os/os.h"

/* cmd_emit calls this (defined in main.c) */
int cmd_print_usage(const char *name)
{
    TERN_UNUSED(name);
    return 0;
}

/* ------------------------------------------------------------ framework */

static int te_checks, te_fails, te_skips;
static int te_update;
static char *te_iverilog, *te_vvp;

static void te_check(int ok, const char *expr, const char *file, int line)
{
    te_checks++;
    if (!ok) {
        te_fails++;
        if (te_fails <= 80)
            fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

#define TE_CHECK(c) te_check((c) ? 1 : 0, #c, __FILE__, __LINE__)

static void te_fail(const char *fmt, ...)
{
    va_list ap;
    te_checks++;
    te_fails++;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t te_rng = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t te_rand(void)
{
    te_rng ^= te_rng >> 12;
    te_rng ^= te_rng << 25;
    te_rng ^= te_rng >> 27;
    return te_rng * UINT64_C(2685821657736338717);
}

/* a fake source so that spans have files and lines */
static uint32_t te_file;

static Span SP(uint32_t line)
{
    Span s;
    const SourceFile *f = src_get(te_file);
    s.file = te_file;
    s.line = line;
    s.col = 5;
    s.off = f && line >= 1 && line <= f->nlines ? f->lines[line - 1] + 4 : 0;
    s.len = 3;
    return s;
}

/* diagnostics capture */
static int te_warnings, te_errors;
static char te_last_warning[512];

static void te_diag_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(user);
    if (d->level == DIAG_WARNING) {
        te_warnings++;
        snprintf(te_last_warning, sizeof te_last_warning, "%s", d->msg);
    } else if (d->level == DIAG_ERROR) {
        te_errors++;
        fprintf(stderr, "diagnostic: %s\n", d->msg);
    }
}

/* ------------------------------------------------------- IR shorthands */

static Arena te_arena;
static IrDesign *D;
#define TA (D->arena)

static uint32_t SIG(IrModule *m, const char *name, IrSigKind k, IrType *t, uint32_t line)
{
    return ir_add_signal(D, m, name, NULL, k, t, SP(line));
}
static uint32_t SIGP(IrModule *m, const char *name, const char *path, IrSigKind k, IrType *t, uint32_t line)
{
    return ir_add_signal(D, m, name, path, k, t, SP(line));
}
static IrType *U(uint32_t w) { return ir_type_uint(D, w); }
static IrType *SI(uint32_t w) { return ir_type_sint(D, w); }
static IrType *BITS(uint32_t w) { return ir_type_bits(D, w); }
static IrType *T_BOOL(void) { return ir_type_bool(D); }
static IrType *CLK(void) { return ir_type_clock(D, 0); }
static IrExpr *S(IrModule *m, uint32_t s) { return ir_sig(TA, m, s, SP(1)); }
static IrExpr *K(uint32_t w, uint64_t v) { return ir_const_u64(TA, w, v, SP(1)); }
static IrExpr *BIN(IrOp op, IrExpr *a, IrExpr *b) { return ir_binary(TA, op, a, b, 0, SP(1)); }
static IrExpr *SBIN(IrOp op, IrExpr *a, IrExpr *b) { return ir_binary(TA, op, a, b, 1, SP(1)); }
static IrExpr *UN(IrOp op, IrExpr *a) { return ir_unary(TA, op, a, SP(1)); }
static IrExpr *ZX(IrExpr *x, uint32_t w) { return ir_resize(TA, x, w, 0, SP(1)); }
static IrExpr *SX(IrExpr *x, uint32_t w) { return ir_resize(TA, x, w, 1, SP(1)); }
static IrExpr *SL(IrExpr *x, uint32_t lo, uint32_t w) { return ir_slice(TA, x, lo, w, SP(1)); }
static IrExpr *MUX(IrExpr *c, IrExpr *a, IrExpr *b) { return ir_mux(TA, c, a, b, SP(1)); }
static IrExpr *IDX(IrExpr *a, IrExpr *i, uint32_t ew) { return ir_index(TA, a, i, ew, SP(1)); }
static IrExpr *CAT2(IrExpr *a, IrExpr *b)
{
    IrExpr *v[2];
    v[0] = a;
    v[1] = b;
    return ir_concat(TA, 2, v, SP(1));
}
static IrExpr *CAT3(IrExpr *a, IrExpr *b, IrExpr *c)
{
    IrExpr *v[3];
    v[0] = a;
    v[1] = b;
    v[2] = c;
    return ir_concat(TA, 3, v, SP(1));
}
/* typed constant (an enum value) */
static IrExpr *KT(IrType *t, uint64_t v)
{
    IrExpr *e = K(t->width, v);
    e->type = t;
    return e;
}
static IrStmt *ASG(IrModule *m, uint32_t s, IrExpr *e, uint32_t line)
{
    return ir_stmt_assign(TA, ir_lv_sig(m, s), e, SP(line));
}
static IrStmt *ASGL(IrLvalue lv, IrExpr *e, uint32_t line) { return ir_stmt_assign(TA, lv, e, SP(line)); }
static void CONT(IrModule *m, uint32_t s, IrExpr *e, uint32_t line)
{
    ir_add_assign(D, m, ir_lv_sig(m, s), e, SP(line));
}

/* block of statements, NULL-terminated */
static IrBlock BL(IrStmt *first, ...)
{
    IrStmtList l = {0};
    va_list ap;
    IrStmt *s = first;
    va_start(ap, first);
    while (s) {
        ir_stmts_push(&l, s);
        s = va_arg(ap, IrStmt *);
    }
    va_end(ap);
    return ir_stmts_finish(TA, &l);
}
static IrStmt *IF(IrExpr *c, IrBlock t, IrBlock e, uint32_t line) { return ir_stmt_if(TA, c, t, e, SP(line)); }

/* case arm with up to two values (nvals 0 = default) */
static IrCaseArm ARM(uint32_t w, int nvals, uint64_t v0, uint64_t v1, IrBlock body)
{
    IrCaseArm a;
    a.nvals = (uint32_t)nvals;
    a.vals = nvals ? ANEWN(TA, IrBits, 2) : NULL;
    if (nvals >= 1)
        a.vals[0] = ir_bits_u64(TA, w, v0);
    if (nvals >= 2)
        a.vals[1] = ir_bits_u64(TA, w, v1);
    a.body = body;
    return a;
}

static IrType *ENUM(const char *name, uint32_t n, const char *const *names, uint32_t tw)
{
    IrVariant v[16];
    uint32_t i;
    for (i = 0; i < n; i++) {
        v[i].name = names[i];
        v[i].tag = ir_bits_u64(TA, tw, i);
        v[i].payload = NULL;
    }
    return ir_type_enum(D, name, n, v, tw, IR_ENC_BINARY, SP(3));
}

static void te_new_design(void)
{
    arena_release(&te_arena);
    D = ir_design_new(&te_arena);
}

/* ============================================================ designs == */

/* §6.3: a loadable counter with a synchronous reset */
static void te_counter(void)
{
    IrModule *m = ir_module_new(D, "Counter", "Counter", "Counter<8>", SP(10));
    uint32_t clk = SIG(m, "clk", IRS_IN, CLK(), 11);
    uint32_t rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 12);
    uint32_t en = SIG(m, "en", IRS_IN, T_BOOL(), 13);
    uint32_t load = SIG(m, "load", IRS_IN, T_BOOL(), 14);
    uint32_t din = SIG(m, "din", IRS_IN, U(8), 15);
    uint32_t cnt = SIG(m, "count", IRS_OUT, U(8), 16);
    uint32_t q = SIG(m, "q", IRS_REG, U(8), 17);
    m->package = "demo";
    ir_add_param(D, m, "W", ir_bits_u64(TA, 32, 8), 0);
    ir_add_domain(D, m, "", clk, rst, 0);
    m->sigs[q].domain = 0;
    ir_set_init(D, m, q, ir_bits_u64(TA, 8, 0));
    CONT(m, cnt, S(m, q), 18);
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   BL(IF(S(m, load), BL(ASG(m, q, S(m, din), 20), NULL),
                         BL(IF(S(m, en), BL(ASG(m, q, BIN(IROP_ADD, S(m, q), K(8, 1)), 21), NULL), BL(NULL), 21),
                            NULL),
                         20),
                      NULL),
                   SP(19));
    ir_design_set_top(D, m);
}

/* §2: the UART transmitter of the language tour */
static void te_uart(void)
{
    static const char *const sn[] = {"Idle", "Start", "Data", "Stop"};
    IrModule *m = ir_module_new(D, "UartTx_16_1", "UartTx", "UartTx<16, 1>", SP(30));
    IrType *st;
    uint32_t clk, rst, txv, txd_in, txr, txd, state, tick, bit, shift, baud;
    IrCaseArm arms[4], carms[3];
    m->package = "uart";
    ir_add_param(D, m, "ClkHz", ir_bits_u64(TA, 32, 16), 0);
    ir_add_param(D, m, "Baud", ir_bits_u64(TA, 32, 1), 0);
    st = ENUM("UartTx_16_1_State", 4, sn, 2);
    clk = SIG(m, "clk", IRS_IN, CLK(), 31);
    rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 32);
    txv = SIGP(m, "tx_valid", "tx.valid", IRS_IN, T_BOOL(), 33);
    txd_in = SIGP(m, "tx_data", "tx.data", IRS_IN, U(8), 33);
    txr = SIGP(m, "tx_ready", "tx.ready", IRS_OUT, T_BOOL(), 33);
    txd = SIG(m, "txd", IRS_OUT, T_BOOL(), 34);
    state = SIG(m, "state", IRS_REG, st, 38);
    tick = SIG(m, "tick", IRS_REG, U(4), 39);
    bit = SIG(m, "bit", IRS_REG, U(3), 40);
    shift = SIG(m, "shift", IRS_REG, U(8), 41);
    baud = SIG(m, "baud_tick", IRS_WIRE, T_BOOL(), 42);
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, state, ir_bits_u64(TA, 2, 0));
    ir_set_init(D, m, tick, ir_bits_u64(TA, 4, 0));
    ir_set_init(D, m, bit, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, m, shift, ir_bits_u64(TA, 8, 0));
    CONT(m, baud, BIN(IROP_EQ, S(m, tick), K(4, 15)), 42);
    CONT(m, txr, BIN(IROP_EQ, S(m, state), KT(st, 0)), 43);
    /* txd = match state { .Idle | .Stop => true, .Start => false, .Data => shift[0] } */
    carms[0] = ARM(2, 2, 0, 3, BL(ASG(m, txd, K(1, 1), 45), NULL));
    carms[1] = ARM(2, 1, 1, 0, BL(ASG(m, txd, K(1, 0), 46), NULL));
    carms[2] = ARM(2, 1, 2, 0, BL(ASG(m, txd, SL(S(m, shift), 0, 1), 47), NULL));
    ir_add_process(D, m, IRP_COMB, IR_NONE, BL(ir_stmt_case(TA, S(m, state), 3, carms, SP(44)), NULL), SP(44));
    /* on clk { ... } */
    arms[0] = ARM(2, 1, 0, 0,
                  BL(IF(S(m, txv), BL(ASG(m, shift, S(m, txd_in), 52), ASG(m, state, KT(st, 1), 52), NULL), BL(NULL),
                        52),
                     NULL));
    arms[1] = ARM(2, 1, 1, 0, BL(IF(S(m, baud), BL(ASG(m, state, KT(st, 2), 53), NULL), BL(NULL), 53), NULL));
    arms[2] = ARM(2, 1, 2, 0,
                  BL(IF(S(m, baud),
                        BL(ASG(m, shift, BIN(IROP_SHR, S(m, shift), K(1, 1)), 55),
                           ASG(m, bit, BIN(IROP_ADD, S(m, bit), K(3, 1)), 56),
                           IF(BIN(IROP_EQ, S(m, bit), K(3, 7)), BL(ASG(m, state, KT(st, 3), 57), NULL), BL(NULL), 57),
                           NULL),
                        BL(NULL), 54),
                     NULL));
    arms[3] = ARM(2, 1, 3, 0, BL(IF(S(m, baud), BL(ASG(m, state, KT(st, 0), 59), NULL), BL(NULL), 59), NULL));
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   BL(ASG(m, tick,
                          MUX(BIN(IROP_LOR, S(m, baud), BIN(IROP_EQ, S(m, state), KT(st, 0))), K(4, 0),
                              BIN(IROP_ADD, S(m, tick), K(4, 1))),
                          50),
                      ir_stmt_case(TA, S(m, state), 4, arms, SP(51)), NULL),
                   SP(49));
    ir_design_set_top(D, m);
}

/* Reset styles: sync, async active-low, mixed reset/reset-less registers,
 * a falling-edge child, a reset-less domain with power-on values and a
 * data-driven asynchronous reset of a child. */
static void te_resets(void)
{
    IrModule *neg, *pon, *areg, *top;
    uint32_t c, d, q, r, i, clk, rst, arst, soft, din, oa, ob, oc, od, oe, on, op, oq, a, b, cc, dd, ee, w_n, w_p,
        w_q, p, cnt, sel;
    /* NegReg: clock<falling>, no reset */
    neg = ir_module_new(D, "NegReg", NULL, NULL, SP(60));
    c = SIG(neg, "clk", IRS_IN, ir_type_clock(D, 1), 61);
    d = SIG(neg, "d", IRS_IN, U(8), 62);
    q = SIG(neg, "q", IRS_OUT, U(8), 63);
    r = SIG(neg, "r", IRS_REG, U(8), 64);
    ir_add_domain(D, neg, "", c, IR_NONE, 0);
    CONT(neg, q, S(neg, r), 65);
    ir_add_process(D, neg, IRP_CLOCKED, 0, BL(ASG(neg, r, S(neg, d), 66), NULL), SP(66));
    ir_design_add_module(D, neg);
    /* PowerOn: no reset; power-on values */
    pon = ir_module_new(D, "PowerOn", NULL, NULL, SP(70));
    c = SIG(pon, "clk", IRS_IN, CLK(), 71);
    q = SIG(pon, "q", IRS_OUT, U(4), 72);
    p = SIG(pon, "p", IRS_REG, U(4), 73);
    cnt = SIG(pon, "seen", IRS_REG, T_BOOL(), 74);
    ir_add_domain(D, pon, "", c, IR_NONE, 0);
    ir_set_init(D, pon, p, ir_bits_u64(TA, 4, 5));
    ir_set_init(D, pon, cnt, ir_bits_u64(TA, 1, 0));
    CONT(pon, q, S(pon, p), 75);
    ir_add_process(D, pon, IRP_CLOCKED, 0,
                   BL(ASG(pon, p, BIN(IROP_ADD, S(pon, p), K(4, 3)), 76), ASG(pon, cnt, K(1, 1), 77), NULL), SP(76));
    ir_design_add_module(D, pon);
    /* AReg: async active-low reset port */
    areg = ir_module_new(D, "AReg", NULL, NULL, SP(80));
    c = SIG(areg, "clk", IRS_IN, CLK(), 81);
    rst = SIG(areg, "rst_n", IRS_IN, ir_type_reset(D, 1, 1), 82);
    d = SIG(areg, "d", IRS_IN, U(8), 83);
    q = SIG(areg, "q", IRS_OUT, U(8), 84);
    r = SIG(areg, "r", IRS_REG, U(8), 85);
    ir_add_domain(D, areg, "", c, rst, 0);
    ir_set_init(D, areg, r, ir_bits_u64(TA, 8, 0xAA));
    CONT(areg, q, S(areg, r), 86);
    ir_add_process(D, areg, IRP_CLOCKED, 0, BL(ASG(areg, r, BIN(IROP_ADD, S(areg, r), S(areg, d)), 87), NULL),
                   SP(87));
    ir_design_add_module(D, areg);
    /* top */
    top = ir_module_new(D, "Resets", NULL, NULL, SP(90));
    clk = SIG(top, "clk", IRS_IN, CLK(), 91);
    rst = SIG(top, "rst", IRS_IN, ir_type_reset(D, 0, 0), 92);
    arst = SIG(top, "arst_n", IRS_IN, ir_type_reset(D, 1, 1), 93);
    soft = SIG(top, "soft_rst_n", IRS_IN, T_BOOL(), 94);
    din = SIG(top, "din", IRS_IN, U(8), 95);
    sel = SIG(top, "sel", IRS_IN, T_BOOL(), 96);
    oa = SIG(top, "oa", IRS_OUT, U(8), 97);
    ob = SIG(top, "ob", IRS_OUT, U(8), 98);
    oc = SIG(top, "oc", IRS_OUT, U(8), 99);
    od = SIG(top, "od", IRS_OUT, U(8), 100);
    oe = SIG(top, "oe", IRS_OUT, U(8), 101);
    on = SIG(top, "on", IRS_OUT, U(8), 102);
    op = SIG(top, "op", IRS_OUT, U(4), 103);
    oq = SIG(top, "oq", IRS_OUT, U(8), 104);
    a = SIG(top, "a", IRS_REG, U(8), 105);
    b = SIG(top, "b", IRS_REG, U(8), 106);
    cc = SIG(top, "c", IRS_REG, U(8), 107);
    dd = SIG(top, "d", IRS_REG, U(8), 108);
    ee = SIG(top, "e", IRS_REG, U(8), 109);
    w_n = SIG(top, "u_neg_q", IRS_WIRE, U(8), 110);
    w_p = SIG(top, "u_pon_q", IRS_WIRE, U(4), 111);
    w_q = SIG(top, "u_areg_q", IRS_WIRE, U(8), 112);
    ir_add_domain(D, top, "sys", clk, rst, 0);
    ir_add_domain(D, top, "io", clk, arst, 0);
    top->default_domain = 0;
    ir_set_init(D, top, a, ir_bits_u64(TA, 8, 0));
    ir_set_init(D, top, cc, ir_bits_u64(TA, 8, 0x11));
    ir_set_init(D, top, dd, ir_bits_u64(TA, 8, 0));
    top->sigs[a].domain = top->sigs[b].domain = 0;
    top->sigs[cc].domain = top->sigs[dd].domain = top->sigs[ee].domain = 1;
    /* sys: a has a reset value, b has none (split into two always_ff) */
    ir_add_process(D, top, IRP_CLOCKED, 0,
                   BL(IF(S(top, sel), BL(ASG(top, a, BIN(IROP_ADD, S(top, a), S(top, din)), 115), NULL),
                         BL(ASG(top, a, BIN(IROP_XOR, S(top, a), K(8, 0x5A)), 116), NULL), 114),
                      ASG(top, b, BIN(IROP_XOR, S(top, a), S(top, din)), 117), NULL),
                   SP(113));
    /* io: async active-low; c only reset registers, then d (reset) + e (no reset) */
    ir_add_process(D, top, IRP_CLOCKED, 1, BL(ASG(top, cc, BIN(IROP_SUB, S(top, cc), S(top, din)), 119), NULL), SP(118));
    ir_add_process(D, top, IRP_CLOCKED, 1,
                   BL(ASG(top, dd, BIN(IROP_ADD, S(top, dd), K(8, 1)), 121),
                      IF(S(top, sel), BL(ASG(top, ee, S(top, dd), 122), NULL), BL(ASG(top, ee, S(top, din), 123), NULL),
                         122),
                      NULL),
                   SP(120));
    CONT(top, oa, S(top, a), 124);
    CONT(top, ob, S(top, b), 125);
    CONT(top, oc, S(top, cc), 126);
    CONT(top, od, S(top, dd), 127);
    CONT(top, oe, S(top, ee), 128);
    CONT(top, on, S(top, w_n), 129);
    CONT(top, op, S(top, w_p), 130);
    CONT(top, oq, S(top, w_q), 131);
    i = ir_add_instance(D, top, "u_neg", neg, SP(132));
    ir_connect_name(D, top, i, "clk", S(top, clk));
    ir_connect_name(D, top, i, "d", S(top, din));
    ir_connect_name(D, top, i, "q", S(top, w_n));
    i = ir_add_instance(D, top, "u_pon", pon, SP(133));
    ir_connect_name(D, top, i, "clk", S(top, clk));
    ir_connect_name(D, top, i, "q", S(top, w_p));
    i = ir_add_instance(D, top, "u_areg", areg, SP(134));
    ir_connect_name(D, top, i, "clk", S(top, clk));
    ir_connect_name(D, top, i, "rst_n", S(top, soft));
    ir_connect_name(D, top, i, "d", S(top, din));
    ir_connect_name(D, top, i, "q", S(top, w_q));
    ir_design_set_top(D, top);
}

/* case / mux / if-chains, an enum state machine */
static void te_case(void)
{
    static const char *const on[] = {"Add", "Sub", "And", "Or", "Xor", "Pass"};
    static const char *const sn[] = {"Idle", "Run", "Done"};
    IrModule *m = ir_module_new(D, "CaseMux", NULL, NULL, SP(140));
    IrType *opt = ENUM("Op", 6, on, 3), *st = ENUM("Phase", 3, sn, 2);
    uint32_t clk, rst, op, a, b, go, y, flag, z, w, ph, opc, yv, fv, zv, state, cnt, osel;
    IrCaseArm arms[6], sarms[3], parms[3];
    clk = SIG(m, "clk", IRS_IN, CLK(), 141);
    rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 142);
    op = SIG(m, "op", IRS_IN, U(3), 143);
    a = SIG(m, "a", IRS_IN, U(8), 144);
    b = SIG(m, "b", IRS_IN, U(8), 145);
    go = SIG(m, "go", IRS_IN, T_BOOL(), 146);
    y = SIG(m, "y", IRS_OUT, U(8), 147);
    flag = SIG(m, "flag", IRS_OUT, T_BOOL(), 148);
    z = SIG(m, "z", IRS_OUT, U(2), 149);
    w = SIG(m, "w", IRS_OUT, U(8), 150);
    ph = SIG(m, "phase", IRS_OUT, U(2), 151);
    osel = SIG(m, "osel", IRS_OUT, U(8), 152);
    opc = SIG(m, "opc", IRS_WIRE, opt, 153);
    yv = SIG(m, "yv", IRS_VAR, U(8), 154);
    fv = SIG(m, "fv", IRS_VAR, T_BOOL(), 155);
    zv = SIG(m, "zv", IRS_VAR, U(2), 156);
    state = SIG(m, "state", IRS_REG, st, 157);
    cnt = SIG(m, "cnt", IRS_REG, U(8), 158);
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, state, ir_bits_u64(TA, 2, 0));
    ir_set_init(D, m, cnt, ir_bits_u64(TA, 8, 0));
    CONT(m, opc, S(m, op), 159);
    /* comb: defaults, a case without default over the 6 valid codes (codes
     * 6 and 7 assign nothing), a second case with a default arm */
    arms[0] = ARM(3, 1, 0, 0, BL(ASG(m, yv, BIN(IROP_ADD, S(m, a), S(m, b)), 162), NULL));
    arms[1] = ARM(3, 1, 1, 0, BL(ASG(m, yv, BIN(IROP_SUB, S(m, a), S(m, b)), 163), NULL));
    arms[2] = ARM(3, 2, 2, 3,
                  BL(ASG(m, yv,
                         MUX(BIN(IROP_EQ, S(m, opc), KT(opt, 2)), BIN(IROP_AND, S(m, a), S(m, b)),
                             BIN(IROP_OR, S(m, a), S(m, b))),
                         164),
                     NULL));
    arms[3] = ARM(3, 1, 4, 0, BL(ASG(m, yv, BIN(IROP_XOR, S(m, a), S(m, b)), 165), ASG(m, fv, K(1, 1), 165), NULL));
    arms[4] = ARM(3, 1, 5, 0, BL(ASG(m, yv, S(m, a), 166), NULL));
    parms[0] = ARM(2, 1, 0, 0, BL(ASG(m, zv, K(2, 1), 168), NULL));
    parms[1] = ARM(2, 1, 3, 0, BL(ASG(m, zv, K(2, 2), 169), NULL));
    parms[2] = ARM(2, 0, 0, 0, BL(ASG(m, zv, SL(S(m, b), 0, 2), 170), NULL));
    ir_add_process(D, m, IRP_COMB, IR_NONE,
                   BL(ASG(m, yv, K(8, 0), 160), ASG(m, fv, K(1, 0), 161),
                      ir_stmt_case(TA, S(m, opc), 5, arms, SP(161)),
                      ir_stmt_case(TA, SL(S(m, a), 6, 2), 3, parms, SP(167)), NULL),
                   SP(160));
    CONT(m, y, S(m, yv), 171);
    CONT(m, flag, S(m, fv), 172);
    CONT(m, z, S(m, zv), 173);
    /* if/else-if chain in a comb process driving an output directly */
    ir_add_process(D, m, IRP_COMB, IR_NONE,
                   BL(IF(BIN(IROP_LT, S(m, a), S(m, b)), BL(ASG(m, w, K(8, 1), 175), NULL),
                         BL(IF(BIN(IROP_EQ, S(m, a), S(m, b)), BL(ASG(m, w, K(8, 2), 176), NULL),
                               BL(ASG(m, w, BIN(IROP_SUB, S(m, a), S(m, b)), 177), NULL), 176),
                            NULL),
                         175),
                      NULL),
                   SP(174));
    /* mux chain */
    CONT(m, osel,
         MUX(BIN(IROP_EQ, S(m, op), K(3, 0)), S(m, a),
             MUX(BIN(IROP_EQ, S(m, op), K(3, 1)), S(m, b),
                 MUX(S(m, go), BIN(IROP_XOR, S(m, a), S(m, b)), K(8, 0x3c)))),
         178);
    /* clocked state machine */
    sarms[0] = ARM(2, 1, 0, 0, BL(IF(S(m, go), BL(ASG(m, state, KT(st, 1), 181), NULL), BL(NULL), 181), NULL));
    sarms[1] = ARM(2, 1, 1, 0,
                   BL(ASG(m, cnt, BIN(IROP_ADD, S(m, cnt), S(m, a)), 182),
                      IF(BIN(IROP_GT, S(m, cnt), K(8, 200)), BL(ASG(m, state, KT(st, 2), 183), NULL), BL(NULL), 183),
                      NULL));
    sarms[2] = ARM(2, 1, 2, 0, BL(ASG(m, state, KT(st, 0), 184), ASG(m, cnt, K(8, 0), 184), NULL));
    ir_add_process(D, m, IRP_CLOCKED, 0, BL(ir_stmt_case(TA, S(m, state), 3, sarms, SP(180)), NULL), SP(179));
    CONT(m, ph, S(m, state), 185);
    ir_design_set_top(D, m);
}

/* structs, enums, payload enums, arrays */
static void te_types(void)
{
    static const char *const kn[] = {"A", "B", "C"};
    IrModule *m = ir_module_new(D, "Types", NULL, NULL, SP(190));
    IrField f[3];
    IrVariant v[3];
    IrType *pix, *pos, *kind, *obj, *cmd, *arr4, *parr, *wr, *rd;
    uint32_t clk, rst, px, idx, val, cmdin, grey, kout, isw, addr, objr, arr, pa, pdyn, eout, rdata, whole;
    f[0].name = "r";
    f[0].type = U(8);
    f[1].name = "g";
    f[1].type = U(8);
    f[2].name = "b";
    f[2].type = U(8);
    pix = ir_type_struct(D, "Pixel", 3, f, SP(191));
    f[0].name = "x";
    f[0].type = SI(8);
    f[1].name = "y";
    f[1].type = U(8);
    pos = ir_type_struct(D, "Pos", 2, f, SP(192));
    kind = ENUM("Kind", 3, kn, 2);
    f[0].name = "pos";
    f[0].type = pos;
    f[1].name = "color";
    f[1].type = pix;
    f[2].name = "kind";
    f[2].type = kind;
    obj = ir_type_struct(D, "Obj", 3, f, SP(194));
    f[0].name = "addr";
    f[0].type = U(16);
    f[1].name = "data";
    f[1].type = U(32);
    wr = ir_type_struct(D, NULL, 2, f, SP(195));
    rd = ir_type_struct(D, NULL, 1, f, SP(196));
    v[0].name = "Write";
    v[0].tag = ir_bits_u64(TA, 2, 0);
    v[0].payload = wr;
    v[1].name = "Read";
    v[1].tag = ir_bits_u64(TA, 2, 1);
    v[1].payload = rd;
    v[2].name = "Idle";
    v[2].tag = ir_bits_u64(TA, 2, 2);
    v[2].payload = NULL;
    cmd = ir_type_enum(D, "Cmd", 3, v, 2, IR_ENC_BINARY, SP(197));
    arr4 = ir_type_array(D, U(8), 4);
    parr = ir_type_array(D, pix, 2);
    clk = SIG(m, "clk", IRS_IN, CLK(), 200);
    rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 201);
    px = SIG(m, "px", IRS_IN, pix, 202);
    idx = SIG(m, "idx", IRS_IN, U(2), 203);
    val = SIG(m, "val", IRS_IN, U(8), 204);
    cmdin = SIG(m, "cmd", IRS_IN, cmd, 205);
    grey = SIG(m, "grey", IRS_OUT, U(10), 206);
    kout = SIG(m, "kind_out", IRS_OUT, kind, 207);
    isw = SIG(m, "is_write", IRS_OUT, T_BOOL(), 208);
    addr = SIG(m, "addr", IRS_OUT, U(16), 209);
    eout = SIG(m, "elem", IRS_OUT, U(8), 210);
    rdata = SIG(m, "green1", IRS_OUT, U(8), 211);
    whole = SIG(m, "obj_out", IRS_OUT, obj, 212);
    objr = SIG(m, "obj", IRS_REG, obj, 213);
    arr = SIG(m, "arr", IRS_REG, arr4, 214);
    pa = SIG(m, "pixels", IRS_REG, parr, 215);
    pdyn = SIG(m, "last_cmd", IRS_REG, cmd, 216);
    ir_add_domain(D, m, "", clk, rst, 0);
    {
        IrBits init = ir_bits_zero(TA, obj->width);
        ir_bits_set(&init, 0, 1); /* kind = B */
        ir_set_init(D, m, objr, init);
    }
    ir_set_init(D, m, arr, ir_bits_u64(TA, 32, 0x04030201));
    {
        IrBits idle = ir_bits_zero(TA, 50);
        ir_bits_set(&idle, 49, 1); /* tag 2 = Idle */
        ir_set_init(D, m, pdyn, idle);
    }
    /* grey = (r + g + b) / 3, in 10 bits */
    CONT(m, grey,
         BIN(IROP_DIV,
             BIN(IROP_ADD, BIN(IROP_ADD, ZX(ir_field(TA, S(m, px), "r", SP(1)), 10), ZX(ir_field(TA, S(m, px), "g", SP(1)), 10)),
                 ZX(ir_field(TA, S(m, px), "b", SP(1)), 10)),
             K(10, 3)),
         217);
    CONT(m, kout, ir_field(TA, S(m, objr), "kind", SP(1)), 218);
    CONT(m, isw, BIN(IROP_EQ, SL(S(m, cmdin), 48, 2), K(2, 0)), 219);
    CONT(m, addr, SL(S(m, cmdin), 32, 16), 220);
    CONT(m, eout, IDX(S(m, arr), S(m, idx), 8), 221);
    CONT(m, rdata, SL(S(m, pa), 24 + 8, 8), 222);
    CONT(m, whole, S(m, objr), 223);
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   BL(ASGL(ir_lv_slice(m, objr, obj->fields[0].lsb + pos->fields[0].lsb, 8),
                           BIN(IROP_ADD, ir_field(TA, ir_field(TA, S(m, objr), "pos", SP(1)), "x", SP(1)), K(8, 1)),
                           225),
                      ASGL(ir_lv_slice(m, objr, obj->fields[1].lsb, 24), S(m, px), 226),
                      ASGL(ir_lv_slice(m, objr, obj->fields[2].lsb, 2), KT(kind, 2), 227),
                      ASGL(ir_lv_index(m, arr, 0, 8, S(m, idx)), S(m, val), 228),
                      ASGL(ir_lv_slice(m, pa, 24, 24), S(m, px), 229),
                      ASGL(ir_lv_slice(m, pa, 8, 8), SL(S(m, val), 0, 8), 230),
                      IF(BIN(IROP_NE, SL(S(m, cmdin), 48, 2), K(2, 2)), BL(ASG(m, pdyn, S(m, cmdin), 231), NULL),
                         BL(ASG(m, pdyn, K(50, (uint64_t)0x123456789ULL), 232), NULL), 231),
                      NULL),
                   SP(224));
    ir_design_set_top(D, m);
}

/* memories: init files, inline ROM, byte-mask writes, sync and async reads */
static void te_mem(void)
{
    IrModule *m = ir_module_new(D, "Mem", NULL, NULL, SP(240));
    uint32_t clk, rst, we, wa, wd, wm, ra, ra2, ra3, bwe, oread, oasync, orom, obig, ram, rom, big, rdata, bigq, cnt,
        ocnt, i;
    clk = SIG(m, "clk", IRS_IN, CLK(), 241);
    rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 242);
    we = SIG(m, "we", IRS_IN, T_BOOL(), 243);
    wa = SIG(m, "waddr", IRS_IN, U(4), 244);
    wd = SIG(m, "wdata", IRS_IN, U(16), 245);
    wm = SIG(m, "wmask", IRS_IN, BITS(2), 246);
    ra = SIG(m, "raddr", IRS_IN, U(4), 247);
    ra2 = SIG(m, "raddr2", IRS_IN, U(4), 248);
    ra3 = SIG(m, "raddr3", IRS_IN, U(3), 249);
    bwe = SIG(m, "big_we", IRS_IN, T_BOOL(), 250);
    oread = SIG(m, "rdata", IRS_OUT, U(16), 251);
    oasync = SIG(m, "adata", IRS_OUT, U(16), 252);
    orom = SIG(m, "romdata", IRS_OUT, U(8), 253);
    obig = SIG(m, "bigdata", IRS_OUT, U(8), 254);
    ocnt = SIG(m, "count", IRS_OUT, U(8), 255);
    ram = ir_add_memory(D, m, "ram", U(16), 12, SP(256));
    rom = ir_add_memory(D, m, "rom", U(8), 8, SP(257));
    big = ir_add_memory(D, m, "lut", U(8), 32, SP(258));
    rdata = SIG(m, "rdata_q", IRS_REG, U(16), 259);
    bigq = SIG(m, "lut_q", IRS_REG, U(8), 260);
    cnt = SIG(m, "cnt", IRS_REG, U(8), 261);
    ir_signal_add_attr(D, m, ram, "ram", "block");
    ir_signal_add_attr(D, m, big, "ram", "distributed");
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, cnt, ir_bits_u64(TA, 8, 0));
    m->sigs[ram].mem_init = ANEWN(TA, IrBits, 12);
    for (i = 0; i < 12; i++)
        m->sigs[ram].mem_init[i] = ir_bits_u64(TA, 16, i * 0x1111u + 3);
    m->sigs[rom].mem_init = ANEWN(TA, IrBits, 8);
    for (i = 0; i < 8; i++)
        m->sigs[rom].mem_init[i] = ir_bits_u64(TA, 8, (i * 37u + 11) & 0xFF);
    m->sigs[big].mem_init = ANEWN(TA, IrBits, 32);
    for (i = 0; i < 32; i++)
        m->sigs[big].mem_init[i] = ir_bits_u64(TA, 8, (i * i + 1) & 0xFF);
    m->sigs[big].mem_init_file = "sine.hex";
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   BL(IF(S(m, we), BL(ir_stmt_memwrite(TA, m, ram, S(m, wa), S(m, wd), S(m, wm), SP(263)), NULL),
                         BL(NULL), 263),
                      ASG(m, rdata, ir_memread(TA, m, ram, S(m, ra), SP(1)), 264),
                      IF(S(m, bwe),
                         BL(ir_stmt_memwrite(TA, m, big, ZX(S(m, ra3), 5), SL(S(m, wd), 4, 8), NULL, SP(265)), NULL),
                         BL(NULL), 265),
                      ASG(m, bigq, ir_memread(TA, m, big, ZX(S(m, ra), 5), SP(1)), 266),
                      ASG(m, cnt, BIN(IROP_ADD, S(m, cnt), K(8, 1)), 267), NULL),
                   SP(262));
    CONT(m, oread, S(m, rdata), 268);
    CONT(m, oasync, ir_memread(TA, m, ram, S(m, ra2), SP(1)), 269);
    CONT(m, orom, ir_memread(TA, m, rom, S(m, ra3), SP(1)), 270);
    CONT(m, obig, S(m, bigq), 271);
    CONT(m, ocnt, S(m, cnt), 272);
    ir_design_set_top(D, m);
}

/* hierarchy: monomorphized children, expression connections, an extern module */
static void te_hier(void)
{
    IrModule *add, *pll, *top;
    uint32_t a, b, s, c, i, clk, rst, x, y, sum, carry, locked, w_s1, w_c1, w_s2, w_c2, w_lk, w_oc, sum9;
    add = ir_module_new(D, "Adder_8", "Adder", "Adder<8>", SP(280));
    a = SIG(add, "a", IRS_IN, U(8), 281);
    b = SIG(add, "b", IRS_IN, U(8), 282);
    s = SIG(add, "s", IRS_OUT, U(8), 283);
    c = SIG(add, "c", IRS_OUT, T_BOOL(), 284);
    sum9 = SIG(add, "full", IRS_WIRE, U(9), 285);
    ir_add_param(D, add, "W", ir_bits_u64(TA, 32, 8), 0);
    CONT(add, sum9, BIN(IROP_ADD, ZX(S(add, a), 9), ZX(S(add, b), 9)), 285);
    CONT(add, s, SL(S(add, sum9), 0, 8), 286);
    CONT(add, c, SL(S(add, sum9), 8, 1), 287);
    ir_design_add_module(D, add);
    pll = ir_module_new(D, "pll_wrapper", NULL, NULL, SP(290));
    pll->is_extern = 1;
    pll->extern_file = "ip/pll_wrapper.sv";
    SIG(pll, "refclk", IRS_IN, CLK(), 291);
    SIG(pll, "rst", IRS_IN, ir_type_reset(D, 1, 0), 292);
    SIG(pll, "outclk", IRS_OUT, CLK(), 293);
    SIG(pll, "locked", IRS_OUT, T_BOOL(), 294);
    ir_add_param(D, pll, "DIV", ir_bits_u64(TA, 32, 4), 0);
    ir_design_add_module(D, pll);
    top = ir_module_new(D, "Hier", NULL, NULL, SP(300));
    clk = SIG(top, "clk", IRS_IN, CLK(), 301);
    rst = SIG(top, "rst", IRS_IN, ir_type_reset(D, 1, 0), 302);
    x = SIG(top, "x", IRS_IN, U(8), 303);
    y = SIG(top, "y", IRS_IN, U(8), 304);
    sum = SIG(top, "sum", IRS_OUT, U(8), 305);
    carry = SIG(top, "carry", IRS_OUT, T_BOOL(), 306);
    locked = SIG(top, "locked", IRS_OUT, T_BOOL(), 307);
    w_s1 = SIG(top, "u1_s", IRS_WIRE, U(8), 308);
    w_c1 = SIG(top, "u1_c", IRS_WIRE, T_BOOL(), 309);
    w_s2 = SIG(top, "u2_s", IRS_WIRE, U(8), 310);
    w_c2 = SIG(top, "u2_c", IRS_WIRE, T_BOOL(), 311);
    w_lk = SIG(top, "pll_locked", IRS_WIRE, T_BOOL(), 312);
    w_oc = SIG(top, "pll_outclk", IRS_WIRE, CLK(), 313);
    ir_add_domain(D, top, "", clk, rst, 100000000);
    i = ir_add_instance(D, top, "u1", add, SP(314));
    ir_connect(D, top, i, a, S(top, x));
    ir_connect(D, top, i, b, CAT2(SL(S(top, y), 0, 4), SL(S(top, x), 4, 4)));
    ir_connect(D, top, i, s, S(top, w_s1));
    ir_connect(D, top, i, c, S(top, w_c1));
    i = ir_add_instance(D, top, "u2", add, SP(315));
    ir_connect(D, top, i, a, BIN(IROP_XOR, S(top, w_s1), S(top, y)));
    ir_connect(D, top, i, b, K(8, 7));
    ir_connect(D, top, i, s, S(top, w_s2));
    ir_connect(D, top, i, c, S(top, w_c2));
    i = ir_add_instance(D, top, "pll", pll, SP(316));
    ir_connect_name(D, top, i, "refclk", S(top, clk));
    ir_connect_name(D, top, i, "rst", S(top, rst));
    ir_connect_name(D, top, i, "outclk", S(top, w_oc));
    ir_connect_name(D, top, i, "locked", S(top, w_lk));
    CONT(top, sum, S(top, w_s2), 317);
    CONT(top, carry, BIN(IROP_LOR, S(top, w_c1), S(top, w_c2)), 318);
    CONT(top, locked, S(top, w_lk), 319);
    ir_design_set_top(D, top);
}

/* signed/unsigned arithmetic, division semantics, shifts, wide values */
static void te_arith(void)
{
    IrModule *m = ir_module_new(D, "Arith", NULL, NULL, SP(330));
    uint32_t a, b, sa, sb, sh, c, w1, w2, o;
    IrBits big = ir_bits_zero(TA, 100);
    int line = 340;
    a = SIG(m, "a", IRS_IN, U(8), 331);
    b = SIG(m, "b", IRS_IN, U(8), 332);
    sa = SIG(m, "sa", IRS_IN, SI(8), 333);
    sb = SIG(m, "sb", IRS_IN, SI(8), 334);
    sh = SIG(m, "sh", IRS_IN, U(4), 335);
    c = SIG(m, "c", IRS_IN, T_BOOL(), 336);
    w1 = SIG(m, "w1", IRS_IN, BITS(100), 337);
    w2 = SIG(m, "w2", IRS_IN, BITS(100), 338);
    big.w[0] = UINT64_C(0xDEADBEEFCAFEF00D);
    big.w[1] = UINT64_C(0xABCDE12345);
#define TE_OUT(nm, ty, e)                                    \
    do {                                                     \
        o = SIG(m, nm, IRS_OUT, ty, (uint32_t)line);         \
        CONT(m, o, e, (uint32_t)line);                       \
        line++;                                              \
    } while (0)
    TE_OUT("add", U(8), BIN(IROP_ADD, S(m, a), S(m, b)));
    TE_OUT("sub", U(8), BIN(IROP_SUB, S(m, a), S(m, b)));
    TE_OUT("mul", U(8), BIN(IROP_MUL, S(m, a), S(m, b)));
    TE_OUT("smul", SI(8), SBIN(IROP_MUL, S(m, sa), S(m, sb)));
    TE_OUT("udiv", U(8), BIN(IROP_DIV, S(m, a), S(m, b)));
    TE_OUT("umod", U(8), BIN(IROP_MOD, S(m, a), S(m, b)));
    TE_OUT("sdiv", SI(8), SBIN(IROP_DIV, S(m, sa), S(m, sb)));
    TE_OUT("smod", SI(8), SBIN(IROP_MOD, S(m, sa), S(m, sb)));
    TE_OUT("divc", U(8), BIN(IROP_DIV, S(m, a), K(8, 3)));
    TE_OUT("modc", U(8), BIN(IROP_MOD, S(m, a), K(8, 5)));
    TE_OUT("sdivc", SI(8), SBIN(IROP_DIV, S(m, sa), K(8, 0xFD)));
    TE_OUT("smodc", SI(8), SBIN(IROP_MOD, S(m, sa), K(8, 0xFD)));
    TE_OUT("ult", T_BOOL(), BIN(IROP_LT, S(m, a), S(m, b)));
    TE_OUT("ule", T_BOOL(), BIN(IROP_LE, S(m, a), S(m, b)));
    TE_OUT("slt", T_BOOL(), SBIN(IROP_LT, S(m, sa), S(m, sb)));
    TE_OUT("sge", T_BOOL(), SBIN(IROP_GE, S(m, sa), S(m, sb)));
    TE_OUT("sgt_u", T_BOOL(), BIN(IROP_GT, S(m, sa), S(m, sb))); /* unsigned compare of signed signals */
    TE_OUT("sltk", T_BOOL(), SBIN(IROP_LT, S(m, sa), K(8, 0xF0)));
    TE_OUT("shl", U(8), BIN(IROP_SHL, S(m, a), S(m, sh)));
    TE_OUT("shr", U(8), BIN(IROP_SHR, S(m, a), S(m, sh)));
    TE_OUT("ashr", SI(8), SBIN(IROP_ASHR, S(m, sa), S(m, sh)));
    TE_OUT("shr_s", SI(8), BIN(IROP_SHR, S(m, sa), SL(S(m, b), 0, 3)));
    TE_OUT("shl_far", U(8), BIN(IROP_SHL, S(m, a), S(m, b)));
    TE_OUT("neg", SI(8), UN(IROP_NEG, S(m, sa)));
    TE_OUT("inv", U(8), UN(IROP_NOT, S(m, a)));
    TE_OUT("rand", T_BOOL(), UN(IROP_RAND, S(m, a)));
    TE_OUT("ror", T_BOOL(), UN(IROP_ROR, S(m, a)));
    TE_OUT("rxor", T_BOOL(), UN(IROP_RXOR, BIN(IROP_AND, S(m, a), S(m, b))));
    TE_OUT("logic", T_BOOL(),
           BIN(IROP_LOR, BIN(IROP_LAND, S(m, c), UN(IROP_LNOT, BIN(IROP_EQ, S(m, a), S(m, b)))),
               BIN(IROP_NE, S(m, sa), S(m, sb))));
    /* signed operations inside unsigned contexts */
    TE_OUT("mix1", U(8), BIN(IROP_ADD, SBIN(IROP_ASHR, S(m, sa), K(2, 1)), S(m, a)));
    TE_OUT("mix2", U(8), BIN(IROP_XOR, SBIN(IROP_DIV, S(m, sa), S(m, sb)), S(m, a)));
    TE_OUT("mix3", U(8), MUX(S(m, c), SBIN(IROP_DIV, S(m, sa), K(8, 3)), S(m, a)));
    TE_OUT("mix4", T_BOOL(), BIN(IROP_LT, SBIN(IROP_ASHR, S(m, sa), K(2, 2)), S(m, a)));
    TE_OUT("mix5", SI(8), SBIN(IROP_ADD, SBIN(IROP_MOD, S(m, sa), S(m, sb)), UN(IROP_NEG, S(m, sb))));
    TE_OUT("mix6", U(16), BIN(IROP_MUL, ZX(S(m, a), 16), SX(S(m, sb), 16)));
    TE_OUT("mix7", T_BOOL(), SBIN(IROP_LT, SX(SBIN(IROP_ADD, S(m, sa), S(m, sb)), 9), SX(S(m, sb), 9)));
    TE_OUT("mix8", U(8), BIN(IROP_SUB, BIN(IROP_ADD, S(m, a), S(m, b)), BIN(IROP_ADD, S(m, b), S(m, a))));
    TE_OUT("mix9", U(8), BIN(IROP_SHL, BIN(IROP_ADD, S(m, a), S(m, b)), K(3, 2)));
    TE_OUT("mix10", T_BOOL(), BIN(IROP_EQ, BIN(IROP_AND, S(m, a), K(8, 0x0F)), BIN(IROP_OR, S(m, b), K(8, 0xF0))));
    /* wide */
    TE_OUT("wadd", BITS(100), BIN(IROP_ADD, S(m, w1), S(m, w2)));
    TE_OUT("wmul", BITS(100), BIN(IROP_MUL, S(m, w1), S(m, w2)));
    TE_OUT("wxor", BITS(100), BIN(IROP_XOR, S(m, w1), ir_const(TA, big, SP(1))));
    TE_OUT("wshr", BITS(100), BIN(IROP_SHR, S(m, w1), S(m, a)));
    TE_OUT("washr", BITS(100), SBIN(IROP_ASHR, S(m, w2), SL(S(m, a), 0, 7)));
    TE_OUT("wlt", T_BOOL(), SBIN(IROP_LT, S(m, w1), S(m, w2)));
    /* (Icarus Verilog 13 returns 0 for a >64-bit unsigned division whose
     * dividend has its MSB set, so the dividend is shifted right by one) */
    TE_OUT("wdiv", BITS(100), BIN(IROP_DIV, BIN(IROP_SHR, S(m, w1), K(1, 1)), ZX(S(m, b), 100)));
    TE_OUT("wsdiv", BITS(100), SBIN(IROP_DIV, S(m, w1), SX(S(m, sb), 100)));
#undef TE_OUT
    ir_design_set_top(D, m);
}

/* resize, truncation and selects of expressions; dynamic indices */
static void te_resize(void)
{
    IrModule *m = ir_module_new(D, "Resize", NULL, NULL, SP(400));
    IrType *arr4 = ir_type_array(D, U(8), 4), *arr3 = ir_type_array(D, U(8), 3);
    uint32_t a, b, s, idx, arr, o, v, vec, small, osm, osm2, ol1, ol2, ovec, w9;
    int line = 410;
    a = SIG(m, "a", IRS_IN, U(8), 401);
    b = SIG(m, "b", IRS_IN, U(8), 402);
    s = SIG(m, "s", IRS_IN, SI(8), 403);
    idx = SIG(m, "idx", IRS_IN, U(2), 404);
    arr = SIG(m, "arr", IRS_IN, arr4, 405);
#define TE_OUT(nm, ty, e)                                    \
    do {                                                     \
        o = SIG(m, nm, IRS_OUT, ty, (uint32_t)line);         \
        CONT(m, o, e, (uint32_t)line);                       \
        line++;                                              \
    } while (0)
    TE_OUT("z9", U(9), ZX(BIN(IROP_ADD, S(m, a), S(m, b)), 9));
    TE_OUT("carry", T_BOOL(), SL(BIN(IROP_ADD, ZX(S(m, a), 9), ZX(S(m, b), 9)), 8, 1));
    TE_OUT("t4", U(4), ir_resize(TA, BIN(IROP_MUL, S(m, a), S(m, b)), 4, 0, SP(1)));
    TE_OUT("tpath", U(3), ir_resize(TA, S(m, b), 3, 0, SP(1)));
    TE_OUT("sx", SI(16), SX(SBIN(IROP_ADD, S(m, s), K(8, 3)), 16));
    TE_OUT("sx2", SI(12), SX(S(m, s), 12));
    TE_OUT("sx3", U(12), SX(S(m, a), 12));
    TE_OUT("sx1", U(5), SX(SL(S(m, a), 7, 1), 5));
    TE_OUT("hi", U(4), SL(BIN(IROP_XOR, S(m, a), S(m, b)), 4, 4));
    TE_OUT("cat", BITS(13), CAT3(SL(S(m, a), 0, 4), S(m, b), SL(S(m, s), 7, 1)));
    TE_OUT("rep", BITS(4), CAT2(CAT2(SL(S(m, a), 0, 1), SL(S(m, a), 0, 1)), CAT2(SL(S(m, a), 0, 1), SL(S(m, a), 0, 1))));
    TE_OUT("elem", U(8), IDX(S(m, arr), S(m, idx), 8));
    TE_OUT("elem_lo", U(4), SL(IDX(S(m, arr), S(m, idx), 8), 0, 4));
    TE_OUT("elem_x", U(8), IDX(BIN(IROP_XOR, S(m, arr), K(32, 0x0F0F0F0F)), S(m, idx), 8));
    TE_OUT("bit", T_BOOL(), IDX(S(m, a), SL(S(m, b), 0, 3), 1));
    TE_OUT("scat", U(8), SL(CAT2(S(m, a), S(m, b)), 4, 8));
    TE_OUT("sres", U(8), SL(ZX(S(m, a), 16), 4, 8));
    TE_OUT("sres2", U(4), SL(SX(S(m, s), 16), 10, 4));
    TE_OUT("slsl", U(2), SL(SL(BIN(IROP_ADD, S(m, a), S(m, b)), 2, 5), 1, 2));
    TE_OUT("trunc_div", U(4), ir_resize(TA, BIN(IROP_DIV, S(m, a), S(m, b)), 4, 0, SP(1)));
    TE_OUT("ssl", U(4), SL(SBIN(IROP_ASHR, S(m, s), K(2, 1)), 2, 4));
#undef TE_OUT
    /* comb process: temporaries that read process-assigned variables */
    v = SIG(m, "v", IRS_VAR, U(8), 440);
    w9 = SIG(m, "w9", IRS_VAR, U(9), 441);
    vec = SIG(m, "vec", IRS_VAR, arr4, 442);
    small = SIG(m, "small", IRS_VAR, arr3, 443);
    ol1 = SIG(m, "loc1", IRS_OUT, U(4), 444);
    ol2 = SIG(m, "loc2", IRS_OUT, U(12), 445);
    ovec = SIG(m, "vec_out", IRS_OUT, U(32), 446);
    osm = SIG(m, "small_out", IRS_OUT, U(24), 447);
    osm2 = SIG(m, "small_rd", IRS_OUT, U(8), 448);
    ir_add_process(D, m, IRP_COMB, IR_NONE,
                   BL(ASG(m, v, BIN(IROP_ADD, S(m, a), S(m, b)), 450),
                      ASG(m, w9, BIN(IROP_ADD, ZX(S(m, v), 9), ZX(S(m, a), 9)), 451),
                      ASG(m, ol1, SL(BIN(IROP_ADD, S(m, v), K(8, 1)), 4, 4), 452),
                      ASG(m, ol2, ir_resize(TA, BIN(IROP_MUL, ZX(S(m, v), 16), ZX(S(m, b), 16)), 12, 0, SP(1)), 453),
                      ASG(m, vec, S(m, arr), 454),
                      ASGL(ir_lv_index(m, vec, 0, 8, S(m, idx)), BIN(IROP_ADD, IDX(S(m, vec), S(m, idx), 8), S(m, v)), 455),
                      ASG(m, small, SL(S(m, arr), 0, 24), 456),
                      ASGL(ir_lv_index(m, small, 0, 8, S(m, idx)), S(m, b), 457),
                      IF(SL(S(m, w9), 8, 1), BL(ASG(m, ol1, SL(BIN(IROP_XOR, S(m, w9), K(9, 0x155)), 3, 4), 458), NULL),
                         BL(NULL), 458),
                      NULL),
                   SP(449));
    CONT(m, ovec, S(m, vec), 459);
    CONT(m, osm, S(m, small), 460);
    CONT(m, osm2, IDX(S(m, small), S(m, idx), 8), 461);
    ir_design_set_top(D, m);
}

/* immediate assertions and properties */
static void te_asserts(void)
{
    IrModule *m = ir_module_new(D, "Checked", NULL, NULL, SP(470));
    uint32_t clk, rst, pv, pr, pd, popr, count, full, ocnt;
    IrExpr *fire;
    clk = SIG(m, "clk", IRS_IN, CLK(), 471);
    rst = SIG(m, "rst", IRS_IN, ir_type_reset(D, 0, 0), 472);
    pv = SIGP(m, "push_valid", "push.valid", IRS_IN, T_BOOL(), 473);
    pd = SIGP(m, "push_data", "push.data", IRS_IN, U(8), 473);
    pr = SIGP(m, "push_ready", "push.ready", IRS_OUT, T_BOOL(), 473);
    popr = SIG(m, "pop_ready", IRS_IN, T_BOOL(), 474);
    ocnt = SIG(m, "level", IRS_OUT, U(3), 475);
    count = SIG(m, "count", IRS_REG, U(3), 476);
    full = SIG(m, "full", IRS_WIRE, T_BOOL(), 477);
    ir_add_domain(D, m, "", clk, rst, 0);
    ir_set_init(D, m, count, ir_bits_u64(TA, 3, 0));
    CONT(m, full, BIN(IROP_EQ, S(m, count), K(3, 4)), 478);
    CONT(m, pr, UN(IROP_LNOT, S(m, full)), 479);
    CONT(m, ocnt, S(m, count), 480);
    fire = BIN(IROP_LAND, S(m, pv), S(m, pr));
    ir_add_process(D, m, IRP_CLOCKED, 0,
                   BL(ir_stmt_assert(TA, IRA_ASSERT, UN(IROP_LNOT, BIN(IROP_LAND, S(m, pv), S(m, full))),
                                     "push into full fifo", SP(482)),
                      IF(fire, BL(ASG(m, count, BIN(IROP_ADD, S(m, count), K(3, 1)), 483), NULL),
                         BL(IF(BIN(IROP_LAND, S(m, popr), BIN(IROP_NE, S(m, count), K(3, 0))),
                               BL(ASG(m, count, BIN(IROP_SUB, S(m, count), K(3, 1)), 484), NULL), BL(NULL), 484),
                            NULL),
                         483),
                      ir_stmt_assert(TA, IRA_COVER, S(m, full), NULL, SP(485)), NULL),
                   SP(481));
    ir_add_process(D, m, IRP_COMB, IR_NONE,
                   BL(ir_stmt_assert(TA, IRA_ASSUME, BIN(IROP_LE, S(m, count), K(3, 4)), "count in range", SP(487)),
                      NULL),
                   SP(486));
    /* property valid_holds { always (push.valid and !push.ready) |=> push.valid and stable(push.data) } */
    ir_add_property(D, m, "valid_holds", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL_NEXT,
                                               ir_seq_expr(TA, BIN(IROP_LAND, S(m, pv), UN(IROP_LNOT, S(m, pr)))),
                                               ir_seq_expr(TA, BIN(IROP_LAND, S(m, pv),
                                                                   BIN(IROP_EQ, S(m, pd), ir_past(TA, S(m, pd), 1, SP(1))))))),
                    SP(490));
    ir_add_property(D, m, "eventually_drains", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL,
                                               ir_seq_expr(TA, BIN(IROP_LAND, BIN(IROP_GT, S(m, count), K(3, 0)),
                                                                   S(m, popr))),
                                               ir_seq_eventually(TA, ir_seq_expr(TA, BIN(IROP_EQ, S(m, count), K(3, 0))),
                                                                 4))),
                    SP(491));
    ir_add_property(D, m, "no_push_when_full", IRA_ASSUME, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL, ir_seq_expr(TA, S(m, full)),
                                               ir_seq_expr(TA, UN(IROP_LNOT, S(m, pv))))),
                    SP(492));
    ir_add_property(D, m, "wraps_around", IRA_COVER, 0,
                    ir_seq_delay(TA, ir_seq_expr(TA, BIN(IROP_EQ, S(m, count), K(3, 4))), 1, IR_NONE,
                                 ir_seq_expr(TA, BIN(IROP_EQ, S(m, count), K(3, 0)))),
                    SP(493));
    ir_add_property(D, m, "rises", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL,
                                               ir_seq_expr(TA, BIN(IROP_LAND, S(m, pv),
                                                                   UN(IROP_LNOT, ir_past(TA, S(m, pv), 1, SP(1))))),
                                               ir_seq_delay(TA, ir_seq_expr(TA, K(1, 1)), 2, 2,
                                                            ir_seq_expr(TA, BIN(IROP_NE, S(m, count),
                                                                                ir_past(TA, S(m, count), 2, SP(1))))))),
                    SP(494));
    ir_add_property(D, m, "never_over", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_NEVER, ir_seq_expr(TA, BIN(IROP_GT, S(m, count), K(3, 4)))), SP(495));
    ir_add_property(D, m, "starts_empty", IRA_ASSERT, 0, ir_seq_expr(TA, BIN(IROP_EQ, S(m, count), K(3, 0))),
                    SP(496));
    ir_add_property(D, m, "ready_until", IRA_ASSERT, 0,
                    ir_seq_unary(TA, IRQ_ALWAYS,
                                 ir_seq_binary(TA, IRQ_IMPL, ir_seq_expr(TA, BIN(IROP_EQ, S(m, count), K(3, 0))),
                                               ir_seq_binary(TA, IRQ_UNTIL, ir_seq_expr(TA, S(m, pr)),
                                                             ir_seq_expr(TA, S(m, full))))),
                    SP(497));
    ir_design_set_top(D, m);
}

/* std::cdc-style primitives, constraints and the CDC report */
static void te_cdc(void)
{
    IrModule *sync, *fifo, *rsync, *top;
    uint32_t rs_clk, rs_in, rs_out, rs_act, rs_chain, pix_arst, w_prst, w_pact, pcnt, opcnt, opact;
    uint32_t src, dst, d, q, s1, s2, i, wclk, wrst, rclk, rrst, wen, wdat, rdat, wgray, wbin, rgray, rq1, rq2, wq1, wq2,
        mem, odata, sclk, srst, pclk, flag, cfg, cfgp, oflag, odat, ocfg, w_q, w_d, din, rd_q, c_sync, w_cfg;
    IrCrossing x;
    sync = ir_module_new(D, "Sync2_bool", "Sync2", "Sync2<bool>", SP(500));
    sync->package = "std::cdc";
    sync->is_cdc = 1;
    sync->cdc_kind = IRC_SYNC2;
    ir_module_add_attr(D, sync, "cdc", "sync2");
    src = SIG(sync, "src", IRS_IN, CLK(), 501);
    dst = SIG(sync, "dst", IRS_IN, CLK(), 502);
    d = SIG(sync, "d", IRS_IN, T_BOOL(), 503);
    q = SIG(sync, "q", IRS_OUT, T_BOOL(), 504);
    s1 = SIG(sync, "s1", IRS_REG, T_BOOL(), 505);
    s2 = SIG(sync, "s2", IRS_REG, T_BOOL(), 506);
    ir_add_domain(D, sync, "src", src, IR_NONE, 0);
    ir_add_domain(D, sync, "dst", dst, IR_NONE, 0);
    sync->sigs[d].domain = 0;
    sync->sigs[q].domain = 1;
    sync->sigs[s1].domain = sync->sigs[s2].domain = 1;
    ir_signal_add_attr(D, sync, s1, "async_reg", NULL);
    ir_signal_add_attr(D, sync, s2, "async_reg", NULL);
    ir_set_init(D, sync, s1, ir_bits_u64(TA, 1, 0));
    ir_set_init(D, sync, s2, ir_bits_u64(TA, 1, 0));
    ir_add_process(D, sync, IRP_CLOCKED, 1, BL(ASG(sync, s1, S(sync, d), 508), ASG(sync, s2, S(sync, s1), 509), NULL),
                   SP(507));
    CONT(sync, q, S(sync, s2), 510);
    ir_design_add_module(D, sync);

    fifo = ir_module_new(D, "GrayXfer_8", "GrayXfer", "GrayXfer<8>", SP(520));
    fifo->is_cdc = 1;
    fifo->cdc_kind = IRC_ASYNC_FIFO;
    wclk = SIG(fifo, "wclk", IRS_IN, CLK(), 521);
    wrst = SIG(fifo, "wrst", IRS_IN, ir_type_reset(D, 1, 1), 522);
    rclk = SIG(fifo, "rclk", IRS_IN, CLK(), 523);
    rrst = SIG(fifo, "rrst", IRS_IN, ir_type_reset(D, 1, 1), 524);
    wen = SIG(fifo, "wen", IRS_IN, T_BOOL(), 525);
    wdat = SIG(fifo, "wdata", IRS_IN, U(8), 526);
    rdat = SIG(fifo, "rdata", IRS_OUT, U(8), 527);
    wbin = SIG(fifo, "wbin", IRS_REG, U(3), 528);
    wgray = SIG(fifo, "wgray", IRS_REG, BITS(3), 529);
    wq1 = SIG(fifo, "wq1_rgray", IRS_REG, BITS(3), 530);
    wq2 = SIG(fifo, "wq2_rgray", IRS_REG, BITS(3), 531);
    rgray = SIG(fifo, "rgray", IRS_REG, BITS(3), 532);
    rq1 = SIG(fifo, "rq1_wgray", IRS_REG, BITS(3), 533);
    rq2 = SIG(fifo, "rq2_wgray", IRS_REG, BITS(3), 534);
    odata = SIG(fifo, "odata", IRS_REG, U(8), 535);
    mem = ir_add_memory(D, fifo, "mem", U(8), 4, SP(536));
    ir_signal_add_attr(D, fifo, mem, "ram", "distributed");
    ir_add_domain(D, fifo, "src_d", wclk, wrst, 0);
    ir_add_domain(D, fifo, "dst_d", rclk, rrst, 0);
    fifo->sigs[wbin].domain = fifo->sigs[wgray].domain = fifo->sigs[wq1].domain = fifo->sigs[wq2].domain = 0;
    fifo->sigs[rgray].domain = fifo->sigs[rq1].domain = fifo->sigs[rq2].domain = fifo->sigs[odata].domain = 1;
    ir_signal_add_attr(D, fifo, wq1, "async_reg", NULL);
    ir_signal_add_attr(D, fifo, wq2, "async_reg", NULL);
    ir_signal_add_attr(D, fifo, rq1, "async_reg", NULL);
    ir_signal_add_attr(D, fifo, rq2, "async_reg", NULL);
    ir_set_init(D, fifo, wbin, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, wgray, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, wq1, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, wq2, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, rgray, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, rq1, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, rq2, ir_bits_u64(TA, 3, 0));
    ir_set_init(D, fifo, odata, ir_bits_u64(TA, 8, 0));
    ir_add_process(D, fifo, IRP_CLOCKED, 0,
                   BL(ASG(fifo, wq1, S(fifo, rgray), 538), ASG(fifo, wq2, S(fifo, wq1), 539),
                      IF(S(fifo, wen),
                         BL(ir_stmt_memwrite(TA, fifo, mem, SL(S(fifo, wbin), 0, 2), S(fifo, wdat), NULL, SP(540)),
                            ASG(fifo, wbin, BIN(IROP_ADD, S(fifo, wbin), K(3, 1)), 541),
                            ASG(fifo, wgray,
                                BIN(IROP_XOR, BIN(IROP_ADD, S(fifo, wbin), K(3, 1)),
                                    BIN(IROP_SHR, BIN(IROP_ADD, S(fifo, wbin), K(3, 1)), K(1, 1))),
                                542),
                            NULL),
                         BL(NULL), 540),
                      NULL),
                   SP(537));
    ir_add_process(D, fifo, IRP_CLOCKED, 1,
                   BL(ASG(fifo, rq1, S(fifo, wgray), 544), ASG(fifo, rq2, S(fifo, rq1), 545),
                      IF(BIN(IROP_NE, S(fifo, rq2), S(fifo, rgray)),
                         BL(ASG(fifo, odata, ir_memread(TA, fifo, mem, SL(S(fifo, rgray), 0, 2), SP(1)), 546),
                            ASG(fifo, rgray, BIN(IROP_XOR, S(fifo, rgray), K(3, 1)), 547), NULL),
                         BL(NULL), 546),
                      NULL),
                   SP(543));
    CONT(fifo, rdat, S(fifo, odata), 548);
    ir_design_add_module(D, fifo);

    /* ResetSync<2>: asynchronous assertion, synchronous release */
    rsync = ir_module_new(D, "ResetSync_2", "ResetSync", "ResetSync<2>", SP(550));
    rsync->is_cdc = 1;
    rsync->cdc_kind = IRC_RESET_SYNC;
    rs_clk = SIG(rsync, "clk", IRS_IN, CLK(), 551);
    rs_in = SIG(rsync, "rst_in", IRS_IN, ir_type_reset(D, 1, 0), 552);
    rs_out = SIG(rsync, "rst_out", IRS_OUT, ir_type_reset(D, 1, 0), 553);
    rs_act = SIG(rsync, "active", IRS_OUT, T_BOOL(), 554);
    rs_chain = SIG(rsync, "chain", IRS_REG, BITS(2), 555);
    ir_add_domain(D, rsync, "d", rs_clk, rs_in, 0);
    rsync->sigs[rs_chain].domain = 0;
    ir_signal_add_attr(D, rsync, rs_chain, "async_reg", NULL);
    ir_set_init(D, rsync, rs_chain, ir_bits_u64(TA, 2, 3));
    ir_add_process(D, rsync, IRP_CLOCKED, 0, BL(ASG(rsync, rs_chain, BIN(IROP_SHL, S(rsync, rs_chain), K(1, 1)), 557), NULL),
                   SP(556));
    CONT(rsync, rs_act, SL(S(rsync, rs_chain), 1, 1), 558);
    CONT(rsync, rs_out, SL(S(rsync, rs_chain), 1, 1), 559);
    ir_design_add_module(D, rsync);

    top = ir_module_new(D, "CdcTop", NULL, NULL, SP(560));
    sclk = SIG(top, "sys_clk", IRS_IN, CLK(), 561);
    srst = SIG(top, "sys_rst_n", IRS_IN, ir_type_reset(D, 1, 1), 562);
    pclk = SIG(top, "pix_clk", IRS_IN, CLK(), 563);
    din = SIG(top, "din", IRS_IN, U(8), 564);
    cfg = SIG(top, "cfg", IRS_IN, U(4), 565);
    oflag = SIG(top, "flag_pix", IRS_OUT, T_BOOL(), 566);
    odat = SIG(top, "data_pix", IRS_OUT, U(8), 567);
    ocfg = SIG(top, "cfg_pix", IRS_OUT, U(4), 568);
    flag = SIG(top, "flag", IRS_REG, T_BOOL(), 569);
    cfgp = SIG(top, "cfg_q", IRS_REG, U(4), 570);
    w_q = SIG(top, "u_sync_q", IRS_WIRE, T_BOOL(), 571);
    w_d = SIG(top, "u_xfer_rdata", IRS_WIRE, U(8), 572);
    w_cfg = SIG(top, "cfg_cross", IRS_WIRE, U(4), 573);
    rd_q = SIG(top, "pix_rst_n", IRS_WIRE, ir_type_reset(D, 1, 1), 574);
    ir_signal_add_attr(D, top, w_cfg, "unsafe_cross", NULL);
    ir_add_domain(D, top, "sys", sclk, srst, 100000000);
    ir_add_domain(D, top, "pix", pclk, IR_NONE, 25000000);
    top->sigs[flag].domain = 0;
    top->sigs[cfgp].domain = 1;
    ir_set_init(D, top, flag, ir_bits_u64(TA, 1, 0));
    ir_set_init(D, top, cfgp, ir_bits_u64(TA, 4, 9)); /* power-on value (pix has no reset) */
    ir_add_process(D, top, IRP_CLOCKED, 0, BL(ASG(top, flag, UN(IROP_LNOT, S(top, flag)), 576), NULL), SP(575));
    CONT(top, w_cfg, S(top, cfg), 577);
    CONT(top, rd_q, S(top, srst), 578);
    ir_add_process(D, top, IRP_CLOCKED, 1, BL(ASG(top, cfgp, S(top, w_cfg), 580), NULL), SP(579));
    c_sync = ir_add_instance(D, top, "u_sync", sync, SP(581));
    ir_connect(D, top, c_sync, src, S(top, sclk));
    ir_connect(D, top, c_sync, dst, S(top, pclk));
    ir_connect(D, top, c_sync, d, S(top, flag));
    ir_connect(D, top, c_sync, q, S(top, w_q));
    i = ir_add_instance(D, top, "u_xfer", fifo, SP(582));
    ir_connect(D, top, i, wclk, S(top, sclk));
    ir_connect(D, top, i, wrst, S(top, srst));
    ir_connect(D, top, i, rclk, S(top, pclk));
    ir_connect(D, top, i, rrst, S(top, rd_q));
    ir_connect(D, top, i, wen, S(top, flag));
    ir_connect(D, top, i, wdat, S(top, din));
    ir_connect(D, top, i, rdat, S(top, w_d));
    CONT(top, oflag, S(top, w_q), 583);
    CONT(top, odat, S(top, w_d), 584);
    CONT(top, ocfg, S(top, cfgp), 585);
    /* a counter in a domain reset by the ResetSync output */
    pix_arst = SIG(top, "pix_arst", IRS_IN, ir_type_reset(D, 1, 0), 586);
    w_prst = SIG(top, "u_rsync_rst_out", IRS_WIRE, ir_type_reset(D, 1, 0), 587);
    w_pact = SIG(top, "u_rsync_active", IRS_WIRE, T_BOOL(), 588);
    pcnt = SIG(top, "pix_cnt", IRS_REG, U(4), 589);
    opcnt = SIG(top, "pix_count", IRS_OUT, U(4), 590);
    opact = SIG(top, "pix_in_reset", IRS_OUT, T_BOOL(), 591);
    ir_add_domain(D, top, "pix_r", pclk, w_prst, 0);
    top->sigs[pcnt].domain = 2;
    ir_set_init(D, top, pcnt, ir_bits_u64(TA, 4, 0));
    i = ir_add_instance(D, top, "u_rsync", rsync, SP(592));
    ir_connect(D, top, i, rs_clk, S(top, pclk));
    ir_connect(D, top, i, rs_in, S(top, pix_arst));
    ir_connect(D, top, i, rs_out, S(top, w_prst));
    ir_connect(D, top, i, rs_act, S(top, w_pact));
    ir_add_process(D, top, IRP_CLOCKED, 2, BL(ASG(top, pcnt, BIN(IROP_ADD, S(top, pcnt), K(4, 1)), 594), NULL), SP(593));
    CONT(top, opcnt, S(top, pcnt), 595);
    CONT(top, opact, S(top, w_pact), 596);
    ir_design_set_top(D, top);
    memset(&x, 0, sizeof x);
    x.path = "CdcTop > u_sync";
    x.kind = IRC_SYNC2;
    x.from_domain = "sys";
    x.to_domain = "pix";
    x.signal = "flag";
    x.loc = SP(581);
    ir_design_add_crossing(D, &x);
    x.path = "CdcTop > u_xfer";
    x.kind = IRC_ASYNC_FIFO;
    x.signal = "din";
    x.loc = SP(582);
    ir_design_add_crossing(D, &x);
    x.path = "CdcTop";
    x.kind = IRC_UNSAFE;
    x.signal = "cfg";
    x.loc = SP(577);
    ir_design_add_crossing(D, &x);
}

/* §10.3: register map */
static void te_regmap(void)
{
    static const char *const mn[] = {"Off", "Slow", "Fast", "Turbo"};
    IrModule *m = ir_module_new(D, "RegTop", NULL, NULL, SP(600));
    IrType *mode = ENUM("Mode", 4, mn, 2);
    IrRegmap r;
    IrReg regs[6];
    IrRegField fctrl[2], fstat[2], firq[2];
    uint32_t a, y;
    a = SIG(m, "a", IRS_IN, U(8), 601);
    y = SIG(m, "y", IRS_OUT, U(8), 602);
    CONT(m, y, UN(IROP_NOT, S(m, a)), 603);
    ir_design_set_top(D, m);
    memset(&r, 0, sizeof r);
    memset(regs, 0, sizeof regs);
    fctrl[0].name = "enable";
    fctrl[0].lo = 0;
    fctrl[0].width = 1;
    fctrl[0].type = T_BOOL();
    fctrl[1].name = "mode";
    fctrl[1].lo = 1;
    fctrl[1].width = 2;
    fctrl[1].type = mode;
    fstat[0].name = "busy";
    fstat[0].lo = 0;
    fstat[0].width = 1;
    fstat[0].type = T_BOOL();
    fstat[1].name = "err";
    fstat[1].lo = 1;
    fstat[1].width = 1;
    fstat[1].type = T_BOOL();
    firq[0].name = "done";
    firq[0].lo = 0;
    firq[0].width = 1;
    firq[0].type = T_BOOL();
    firq[1].name = "overflow";
    firq[1].lo = 1;
    firq[1].width = 1;
    firq[1].type = T_BOOL();
    regs[0].offset = 0x00;
    regs[0].name = "ctrl";
    regs[0].access = IR_RW;
    regs[0].width = 32;
    regs[0].nfields = 2;
    regs[0].fields = fctrl;
    regs[0].doc = "Engine control.";
    regs[1].offset = 0x04;
    regs[1].name = "status";
    regs[1].access = IR_RO;
    regs[1].width = 32;
    regs[1].nfields = 2;
    regs[1].fields = fstat;
    regs[2].offset = 0x08;
    regs[2].name = "count";
    regs[2].access = IR_RW;
    regs[2].width = 32;
    regs[2].doc = "Bytes processed <since reset> & so on.";
    regs[3].offset = 0x10;
    regs[3].name = "fifo";
    regs[3].access = IR_WO;
    regs[3].width = 8;
    regs[3].is_stream = 1;
    regs[4].offset = 0x14;
    regs[4].name = "irq";
    regs[4].access = IR_W1C;
    regs[4].width = 32;
    regs[4].nfields = 2;
    regs[4].fields = firq;
    regs[5].offset = 0x18;
    regs[5].name = "irq_set";
    regs[5].access = IR_RW1S;
    regs[5].width = 32;
    r.name = "Ctrl";
    r.package = "soc";
    r.nregs = 6;
    r.regs = regs;
    r.loc = SP(610);
    ir_design_add_regmap(D, &r);
}

/* identifiers that are SystemVerilog keywords, and collisions */
static void te_names(void)
{
    static const char *const vn[] = {"default", "Run"};
    IrModule *child, *top;
    IrType *e = ENUM("type", 2, vn, 1), *st;
    IrField f[2];
    uint32_t i, cin, cout, b, lg, ty, b2, w, clk, ev, pr;
    f[0].name = "int";
    f[0].type = U(4);
    f[1].name = "end";
    f[1].type = e;
    st = ir_type_struct(D, "packed", 2, f, SP(620));
    child = ir_module_new(D, "end", NULL, NULL, SP(621));
    cin = SIG(child, "input", IRS_IN, U(4), 622);
    cout = SIG(child, "output", IRS_OUT, U(4), 623);
    CONT(child, cout, UN(IROP_NOT, S(child, cin)), 624);
    ir_design_add_module(D, child);
    top = ir_module_new(D, "module", NULL, NULL, SP(625));
    clk = SIG(top, "clk", IRS_IN, CLK(), 626);
    b = SIG(top, "bit", IRS_IN, st, 627);
    lg = SIG(top, "logic", IRS_OUT, U(4), 628);
    ty = SIG(top, "type_default", IRS_WIRE, T_BOOL(), 629);
    b2 = SIG(top, "bit_", IRS_REG, U(4), 630);
    w = SIG(top, "begin_output", IRS_WIRE, U(4), 631);
    ev = SIG(top, "event", IRS_OUT, e, 632);
    pr = SIG(top, "9lives", IRS_OUT, T_BOOL(), 633);
    ir_add_domain(D, top, "", clk, IR_NONE, 0);
    CONT(top, ty, BIN(IROP_EQ, ir_field(TA, S(top, b), "end", SP(1)), KT(e, 0)), 634);
    ir_add_process(D, top, IRP_CLOCKED, 0, BL(ASG(top, b2, ir_field(TA, S(top, b), "int", SP(1)), 635), NULL), SP(635));
    i = ir_add_instance(D, top, "begin", child, SP(636));
    ir_connect(D, top, i, cin, S(top, b2));
    ir_connect(D, top, i, cout, S(top, w));
    CONT(top, lg, S(top, w), 637);
    CONT(top, ev, MUX(S(top, ty), KT(e, 1), KT(e, 0)), 638);
    CONT(top, pr, UN(IROP_LNOT, S(top, ty)), 639);
    ir_design_set_top(D, top);
}

/* ====================================================== infrastructure == */

typedef struct TeDesign {
    const char *name;
    void (*build)(void);
    int xcheck;            /* random-stimulus cross-check against the simulator */
    int assertions;        /* emit_assertions */
    unsigned mem_init;
    unsigned constraints;
    const char *rare_low;  /* data input that is mostly 1 (an active-low reset) */
} TeDesign;

static const TeDesign te_designs[] = {
    {"counter", te_counter, 1, 0, 0, 0, NULL},
    {"uart_tx", te_uart, 1, 0, 0, 0, NULL},
    {"resets", te_resets, 1, 0, 0, 0, "soft_rst_n"},
    {"case_mux", te_case, 1, 0, 0, 0, NULL},
    {"types", te_types, 1, 0, 0, 0, NULL},
    {"memory", te_mem, 1, 0, SV_MEMINIT_MIF | SV_MEMINIT_COE, 0, NULL},
    {"hierarchy", te_hier, 0, 0, 0, EMIT_CONS_XDC | EMIT_CONS_SDC, NULL},
    {"arith", te_arith, 1, 0, 0, 0, NULL},
    {"resize", te_resize, 1, 0, 0, 0, NULL},
    {"assertions", te_asserts, 1, 1, 0, 0, NULL},
    {"cdc", te_cdc, 1, 0, 0, EMIT_CONS_XDC | EMIT_CONS_SDC, NULL},
    {"regmap", te_regmap, 1, 0, 0, 0, NULL},
    {"names", te_names, 1, 0, 0, 0, NULL},
};

static char *te_path2(const char *a, const char *b) { return os_path_join(a, b); }

/* Compare an emitted file set with the golden directory (or rewrite it). */
static void te_golden(const char *dirname, const SvOutput *out)
{
    char *dir = te_path2("tests/fixtures/emit", dirname);
    OsDirList dl;
    size_t i;
    if (te_update) {
        os_mkdir_p(dir);
        if (os_list_dir(dir, &dl) == 0) {
            for (i = 0; i < dl.len; i++) {
                if (!dl.items[i].is_dir && !sv_output_find(out, dl.items[i].name)) {
                    char *p = te_path2(dir, dl.items[i].name);
                    os_remove(p);
                    free(p);
                }
            }
            os_dir_list_free(&dl);
        }
        sv_output_write_dir(out, dir);
        free(dir);
        return;
    }
    for (i = 0; i < out->nfiles; i++) {
        char *p = te_path2(dir, out->files[i].name), *data = NULL;
        size_t len = 0;
        if (os_read_file(p, &data, &len) != 0) {
            te_fail("golden: missing %s (run with UPDATE=1)", p);
        } else if (len != out->files[i].len || memcmp(data, out->files[i].data, len) != 0) {
            size_t k = 0, line = 1;
            while (k < len && k < out->files[i].len && data[k] == out->files[i].data[k]) {
                if (data[k] == '\n')
                    line++;
                k++;
            }
            te_fail("golden: %s differs at line %u (run with UPDATE=1 to accept)", p, (unsigned)line);
        }
        free(data);
        free(p);
    }
    if (os_list_dir(dir, &dl) == 0) {
        for (i = 0; i < dl.len; i++)
            if (!dl.items[i].is_dir && !sv_output_find(out, dl.items[i].name))
                te_fail("golden: stale file %s/%s (run with UPDATE=1)", dir, dl.items[i].name);
        os_dir_list_free(&dl);
    }
    free(dir);
}

static int te_run(char *const *argv, const char *cwd, char **out_text)
{
    OsSpawnOpts o;
    OsRunResult r;
    int rc;
    memset(&o, 0, sizeof o);
    o.cwd = cwd;
    o.stdin_null = 1;
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    rc = os_run(argv, &o, &r);
    if (out_text) {
        StrBuf sb = {0};
        if (r.out)
            sb_append(&sb, r.out, r.out_len);
        if (r.err)
            sb_append(&sb, r.err, r.err_len);
        *out_text = sb_take(&sb);
    }
    os_run_result_free(&r);
    return rc;
}

/* writes the HDL files of `out` into dir and compiles them with iverilog
 * (plus `extra` files); returns 1 on success */
static int te_iverilog_compile(const char *dir, const SvOutput *out, int v2005, const char *const *extra,
                               size_t nextra, const char *outfile, const char *top_or_null, int no_sva)
{
    char *argv[96];
    size_t n = 0, i;
    int rc;
    char *log = NULL;
    argv[n++] = te_iverilog;
    argv[n++] = (char *)(v2005 ? "-g2005" : "-g2012");
    if (no_sva)
        argv[n++] = (char *)"-DTERN_NO_SVA";
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)outfile;
    if (top_or_null) {
        argv[n++] = (char *)"-s";
        argv[n++] = (char *)top_or_null;
    }
    for (i = 0; i < nextra && n < 80; i++)
        argv[n++] = (char *)extra[i];
    for (i = 0; i < out->nfiles && n < 94; i++)
        if (out->files[i].kind == SV_FILE_PACKAGE)
            argv[n++] = out->files[i].name;
    for (i = 0; i < out->nfiles && n < 94; i++)
        if (out->files[i].kind == SV_FILE_MODULE)
            argv[n++] = out->files[i].name;
    argv[n] = NULL;
    rc = te_run(argv, dir, &log);
    if (rc != 0) {
        te_fail("iverilog %s failed in %s (exit %d):\n%s", v2005 ? "-g2005" : "-g2012", dir, rc, log ? log : "");
        free(log);
        return 0;
    }
    free(log);
    return 1;
}

/* ---------------------------------------------------------- cross-check -- */

static void te_hexw(StrBuf *out, const uint64_t *w, uint32_t width)
{
    uint32_t nd = (width + 3) / 4, i;
    for (i = nd; i-- > 0;) {
        uint32_t bit = i * 4;
        unsigned nib = (unsigned)((w[bit / 64] >> (bit % 64)) & 0xFu);
        if (bit + 4 > width)
            nib &= (1u << (width - bit)) - 1u;
        sb_putc(out, "0123456789abcdef"[nib]);
    }
}

static void te_on_simfail(void *user, const SimFailure *f)
{
    TERN_UNUSED(user);
    TERN_UNUSED(f);
}

typedef struct TePort {
    uint32_t sig;
    const char *name;
    uint32_t width;
} TePort;

#define TE_CYCLES 240

/* Generates a Verilog-2005 testbench driving random stimulus, runs it under
 * vvp for both emitted dialects and compares every cycle with the
 * simulator. */
static void te_xcheck(const TeDesign *td, const SvOutput *sv_out, const SvOutput *v_out)
{
    const IrModule *top = D->top;
    SvNames *nm = sv_names_new(D);
    TePort ins[64], outs[96];
    uint32_t nin = 0, nout = 0, i, k, tw = 0;
    uint32_t clocks[8], cdom[8], nclk = 0, rclocks[8], nrclk = 0;
    uint32_t resets[8], nrst = 0;
    StrBuf tb = {0}, stim = {0}, expect = {0};
    uint64_t *stimw[TE_CYCLES];
    SimModel *sm;
    char *base;
    int std;
    for (i = 0; i < top->nports; i++) {
        uint32_t p = top->ports[i];
        const IrSignal *s = &top->sigs[p];
        if (s->kind == IRS_IN && s->type->kind == IRT_CLOCK)
            continue;
        if (s->kind == IRS_IN && s->type->kind == IRT_RESET) {
            resets[nrst++] = p;
            continue;
        }
        if (s->kind == IRS_IN) {
            ins[nin].sig = p;
            ins[nin].name = sv_names_signal(nm, top, p);
            ins[nin].width = s->type->width;
            tw += s->type->width;
            nin++;
        } else {
            outs[nout].sig = p;
            outs[nout].name = sv_names_signal(nm, top, p);
            outs[nout].width = s->type->width;
            nout++;
        }
    }
    for (i = 0; i < top->ndomains; i++) {
        uint32_t c = top->domains[i].clock;
        for (k = 0; k < nclk; k++)
            if (clocks[k] == c)
                break;
        if (k == nclk && nclk < 8) {
            clocks[nclk] = c;
            cdom[nclk] = i;
            nclk++;
        }
        if (top->domains[i].reset != IR_NONE) {
            for (k = 0; k < nrclk; k++)
                if (rclocks[k] == c)
                    break;
            if (k == nrclk && nrclk < 8)
                rclocks[nrclk++] = c;
        }
    }
    if (!nrclk)
        for (k = 0; k < nclk; k++)
            rclocks[nrclk++] = clocks[k];
    if (!tw)
        tw = 1;
    /* stimulus */
    for (k = 0; k < TE_CYCLES; k++) {
        uint32_t pos = 0;
        stimw[k] = (uint64_t *)xcalloc(IR_LIMBS(tw) + 1, sizeof(uint64_t));
        for (i = nin; i-- > 0;) {
            uint32_t b, w = ins[i].width;
            int mode = (int)(te_rand() % 6);
            int rare = td->rare_low && strcmp(ins[i].name, td->rare_low) == 0;
            for (b = 0; b < w; b++) {
                int bit;
                if (rare)
                    bit = (te_rand() % 10) != 0;
                else if (mode == 0)
                    bit = 0;
                else if (mode == 1)
                    bit = 1;
                else if (mode == 2)
                    bit = b + 1 == w ? 1 : (b == 0 ? (int)(te_rand() & 1) : 0);
                else
                    bit = (int)(te_rand() & 1);
                if (bit)
                    stimw[k][(pos + b) / 64] |= UINT64_C(1) << ((pos + b) % 64);
            }
            pos += w;
        }
        te_hexw(&stim, stimw[k], tw);
        sb_putc(&stim, '\n');
    }
    /* simulator reference */
    sm = sim_new(D, top, 1);
    TE_CHECK(sm != NULL);
    if (sm)
        sim_set_fail_handler(sm, te_on_simfail, NULL);
    if (!sm) {
        for (k = 0; k < TE_CYCLES; k++)
            free(stimw[k]);
        sv_names_free(nm);
        sb_free(&stim);
        return;
    }
    for (k = 0; k < TE_CYCLES; k++) {
        uint32_t pos = 0;
        if (k == 0 || k == TE_CYCLES / 2)
            sim_reset(sm);
        for (i = nin; i-- > 0;) {
            IrBits v = ir_bits_zero(&te_arena, ins[i].width);
            bv_extract(v.w, ins[i].width, stimw[k], tw, pos);
            sim_poke(sm, sim_find(sm, top->sigs[ins[i].sig].path), &v);
            pos += ins[i].width;
        }
        sim_settle(sm);
        sb_printf(&expect, "D %u", k);
        for (i = 0; i < nout; i++) {
            IrBits v = sim_peek_bits(sm, sim_find(sm, top->sigs[outs[i].sig].path), &te_arena);
            sb_putc(&expect, ' ');
            te_hexw(&expect, v.w, outs[i].width);
        }
        sb_putc(&expect, '\n');
        for (i = 0; i < nclk; i++)
            sim_step(sm, cdom[i], 1);
    }
    sim_free(sm);
    /* testbench */
    sb_printf(&tb, "// tb.v: random-stimulus testbench for %s, generated by tests/unit/test_emit.c.\n",
              sv_names_module(nm, top));
    sb_puts(&tb, "// Prints the outputs every cycle; test_emit compares them with the Tern simulator.\n");
    sb_puts(&tb, "`timescale 1ns/1ps\nmodule tb;\n");
    for (i = 0; i < nclk; i++)
        sb_printf(&tb, "    reg %s = 1'b0;\n", sv_names_signal(nm, top, clocks[i]));
    for (i = 0; i < nrst; i++)
        sb_printf(&tb, "    reg %s = 1'b%d;\n", sv_names_signal(nm, top, resets[i]),
                  top->sigs[resets[i]].type->active_low ? 1 : 0);
    for (i = 0; i < nin; i++) {
        if (ins[i].width > 1)
            sb_printf(&tb, "    reg [%u:0] %s = %u'd0;\n", ins[i].width - 1, ins[i].name, ins[i].width);
        else
            sb_printf(&tb, "    reg %s = 1'b0;\n", ins[i].name);
    }
    for (i = 0; i < nout; i++) {
        if (outs[i].width > 1)
            sb_printf(&tb, "    wire [%u:0] %s;\n", outs[i].width - 1, outs[i].name);
        else
            sb_printf(&tb, "    wire %s;\n", outs[i].name);
    }
    sb_printf(&tb, "    reg [%u:0] stim [0:%u];\n    integer i;\n\n", tw - 1, TE_CYCLES - 1);
    sb_printf(&tb, "    %s dut (\n", sv_names_module(nm, top));
    for (i = 0; i < top->nports; i++) {
        const char *pn = sv_names_signal(nm, top, top->ports[i]);
        sb_printf(&tb, "        .%s(%s)%s\n", pn, pn, i + 1 < top->nports ? "," : "");
    }
    sb_puts(&tb, "    );\n\n");
    for (i = 0; i < nclk; i++) {
        const char *cn = sv_names_signal(nm, top, clocks[i]);
        sb_printf(&tb, "    task cycle_%s;\n        begin\n            %s = 1'b1;\n            #5;\n            %s = 1'b0;\n"
                       "            #5;\n        end\n    endtask\n\n",
                  cn, cn, cn);
    }
    sb_puts(&tb, "    task do_reset;\n        begin\n");
    for (i = 0; i < nrst; i++)
        sb_printf(&tb, "            %s = 1'b%d;\n", sv_names_signal(nm, top, resets[i]),
                  top->sigs[resets[i]].type->active_low ? 0 : 1);
    sb_puts(&tb, "            #1;\n");
    for (i = 0; i < nrclk; i++)
        sb_printf(&tb, "            cycle_%s;\n            cycle_%s;\n", sv_names_signal(nm, top, rclocks[i]),
                  sv_names_signal(nm, top, rclocks[i]));
    for (i = 0; i < nrst; i++)
        sb_printf(&tb, "            %s = 1'b%d;\n", sv_names_signal(nm, top, resets[i]),
                  top->sigs[resets[i]].type->active_low ? 1 : 0);
    sb_puts(&tb, "            #1;\n        end\n    endtask\n\n");
    sb_puts(&tb, "    initial begin\n        $readmemh(\"stim.hex\", stim);\n        #1;\n");
    sb_printf(&tb, "        for (i = 0; i < %u; i = i + 1) begin\n", TE_CYCLES);
    sb_printf(&tb, "            if (i == 0 || i == %u)\n                do_reset;\n", TE_CYCLES / 2);
    if (nin) {
        sb_puts(&tb, "            {");
        for (i = 0; i < nin; i++)
            sb_printf(&tb, "%s%s", i ? ", " : "", ins[i].name);
        sb_puts(&tb, "} = stim[i];\n");
    }
    sb_puts(&tb, "            #1;\n            $display(\"D %0d");
    for (i = 0; i < nout; i++)
        sb_puts(&tb, " %h");
    sb_puts(&tb, "\", i");
    for (i = 0; i < nout; i++)
        sb_printf(&tb, ", %s", outs[i].name);
    sb_puts(&tb, ");\n");
    if (nclk)
        for (i = 0; i < nclk; i++)
            sb_printf(&tb, "            cycle_%s;\n", sv_names_signal(nm, top, clocks[i]));
    else
        sb_puts(&tb, "            #9;\n");
    sb_puts(&tb, "        end\n        $finish;\n    end\nendmodule\n");
    /* golden copy of the testbench */
    {
        SvOutput g;
        StrBuf t = {0};
        char *gd;
        memset(&g, 0, sizeof g);
        sb_append(&t, tb.data, tb.len);
        sv_output_add(&g, "tb.v", SV_FILE_MODULE, &t);
        gd = te_path2(td->name, "tb");
        te_golden(gd, &g);
        free(gd);
        sv_output_free(&g);
    }
    if (!te_iverilog || !te_vvp) {
        te_skips++;
        goto done;
    }
    base = te_path2("build/emit_work/xcheck", td->name);
    for (std = 0; std < 2; std++) {
        const SvOutput *out = std ? v_out : sv_out;
        char *dir = te_path2(base, std ? "v2005" : "sv");
        char *p;
        const char *extra[1];
        char *argv[4], *log = NULL;
        int rc;
        os_remove_all(dir);
        os_mkdir_p(dir);
        sv_output_write_dir(out, dir);
        p = te_path2(dir, "tb.v");
        os_write_file(p, tb.data, tb.len);
        free(p);
        p = te_path2(dir, "stim.hex");
        os_write_file(p, stim.data, stim.len);
        free(p);
        extra[0] = "tb.v";
        if (!te_iverilog_compile(dir, out, std, extra, 1, "sim.vvp", "tb", 1)) {
            free(dir);
            continue;
        }
        argv[0] = te_vvp;
        argv[1] = (char *)"-n";
        argv[2] = (char *)"sim.vvp";
        argv[3] = NULL;
        rc = te_run(argv, dir, &log);
        if (rc != 0) {
            te_fail("xcheck %s/%s: vvp failed (exit %d):\n%s", td->name, std ? "v2005" : "sv", rc, log ? log : "");
        } else {
            /* keep the "D ..." lines */
            StrBuf got = {0};
            const char *s = log;
            while (s && *s) {
                const char *e = strchr(s, '\n');
                size_t l = e ? (size_t)(e - s) : strlen(s);
                if (l >= 2 && s[0] == 'D' && s[1] == ' ') {
                    sb_append(&got, s, l);
                    sb_putc(&got, '\n');
                }
                s = e ? e + 1 : s + l;
            }
            if (got.len != expect.len || memcmp(got.data ? got.data : "", expect.data, expect.len) != 0) {
                /* report the first differing cycles */
                const char *a = expect.data, *b = got.data ? got.data : "";
                int shown = 0;
                te_fail("xcheck %s/%s: emitted %s disagrees with the simulator", td->name,
                        std ? "Verilog-2005" : "SystemVerilog", std ? "Verilog" : "SystemVerilog");
                while (*a && shown < 6) {
                    const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
                    size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
                    if (la != lb || memcmp(a, b, la) != 0) {
                        fprintf(stderr, "    sim: %.*s\n    hdl: %.*s\n", (int)la, a, (int)lb, b);
                        shown++;
                    }
                    a = ea ? ea + 1 : a + la;
                    b = eb ? eb + 1 : b + lb;
                }
                {
                    fprintf(stderr, "    outputs:");
                    for (i = 0; i < nout; i++)
                        fprintf(stderr, " %s", outs[i].name);
                    fprintf(stderr, "\n    (files in %s)\n", dir);
                }
            }
            sb_free(&got);
        }
        free(log);
        free(dir);
    }
    free(base);
done:
    for (k = 0; k < TE_CYCLES; k++)
        free(stimw[k]);
    sv_names_free(nm);
    sb_free(&tb);
    sb_free(&stim);
    sb_free(&expect);
}

/* -------------------------------------------------------------- per design */

static int te_emit(const TeDesign *td, SvStd std, int asic, SvOutput *out)
{
    SvOptions o;
    sv_options_init(&o);
    o.std = std;
    o.emit_assertions = td->assertions;
    o.mem_init_formats = td->mem_init;
    o.constraints = td->constraints;
    if (asic)
        o.target_kind = SV_TARGET_ASIC;
    memset(out, 0, sizeof *out);
    return emit_design(D, &o, out);
}

static void te_compile_check(const TeDesign *td, const char *variant, const SvOutput *out, int v2005)
{
    char *dir, *base;
    const char *extra[1];
    size_t nextra = 0;
    if (!te_iverilog) {
        te_skips++;
        return;
    }
    base = te_path2("build/emit_work/compile", td->name);
    dir = te_path2(base, variant);
    os_remove_all(dir);
    os_mkdir_p(dir);
    sv_output_write_dir(out, dir);
    if (out->nextern) {
        /* stand-in for the extern module, shared with tests/lint_emitted.sh */
        char *src = te_path2("tests/fixtures/emit", td->name), *sp = te_path2(src, "extern/pll_wrapper.v");
        char *p = te_path2(dir, "pll_wrapper.v");
        TE_CHECK(os_copy_file(sp, p) == 0);
        free(src);
        free(sp);
        free(p);
        extra[nextra++] = "pll_wrapper.v";
    }
    te_iverilog_compile(dir, out, v2005, extra, nextra, "out.vvp", NULL, 1);
    free(dir);
    free(base);
}

/* emit from the JSON round trip of the design: identical files */
static void te_json_roundtrip(const TeDesign *td, const SvOutput *ref, SvStd std)
{
    StrBuf js = {0};
    Arena a2;
    IrDesign *d2, *saved = D;
    SvOutput out;
    size_t i;
    memset(&a2, 0, sizeof a2);
    ir_to_json(D, &js);
    d2 = ir_from_json(&a2, js.data, js.len);
    TE_CHECK(d2 != NULL);
    if (d2) {
        D = d2;
        te_emit(td, std, 0, &out);
        D = saved;
        TE_CHECK(out.nfiles == ref->nfiles);
        for (i = 0; i < out.nfiles && i < ref->nfiles; i++) {
            if (strcmp(out.files[i].name, ref->files[i].name) != 0 || out.files[i].len != ref->files[i].len ||
                memcmp(out.files[i].data, ref->files[i].data, ref->files[i].len) != 0)
                te_fail("json round trip of %s: %s differs", td->name, ref->files[i].name);
        }
        sv_output_free(&out);
    }
    sb_free(&js);
    arena_release(&a2);
}

/* regmap outputs: the C header compiles warning-free, the SVD is well-formed XML */
static void te_regmap_tools(const TeDesign *td, const SvOutput *out)
{
    size_t i;
    char *cc = os_find_program("cc"), *xmllint = os_find_program("xmllint");
    char *dir = te_path2("build/emit_work/regmap", td->name);
    os_remove_all(dir);
    os_mkdir_p(dir);
    sv_output_write_dir(out, dir);
    for (i = 0; i < out->nfiles; i++) {
        const SvFile *f = &out->files[i];
        char *log = NULL;
        if (f->kind == SV_FILE_C_HEADER && cc) {
            StrBuf src = {0};
            char *p = te_path2(dir, "use_header.c");
            char *argv[12];
            int rc;
            sb_printf(&src, "#include \"%s\"\nint use_header(void);\nint use_header(void) { return 0; }\n", f->name);
            os_write_file(p, src.data, src.len);
            argv[0] = cc;
            argv[1] = (char *)"-std=c99";
            argv[2] = (char *)"-Wall";
            argv[3] = (char *)"-Wextra";
            argv[4] = (char *)"-Wpedantic";
            argv[5] = (char *)"-Werror";
            argv[6] = (char *)"-c";
            argv[7] = (char *)"use_header.c";
            argv[8] = (char *)"-o";
            argv[9] = (char *)"use_header.o";
            argv[10] = NULL;
            rc = te_run(argv, dir, &log);
            if (rc != 0)
                te_fail("regmap header %s does not compile:\n%s", f->name, log ? log : "");
            sb_free(&src);
            free(p);
        } else if (f->kind == SV_FILE_SVD && xmllint) {
            char *argv[4];
            int rc;
            argv[0] = xmllint;
            argv[1] = (char *)"--noout";
            argv[2] = f->name;
            argv[3] = NULL;
            rc = te_run(argv, dir, &log);
            if (rc != 0)
                te_fail("regmap SVD %s is not well-formed:\n%s", f->name, log ? log : "");
        }
        free(log);
    }
    free(cc);
    free(xmllint);
    free(dir);
}

static void te_design(const TeDesign *td)
{
    SvOutput sv, v;
    char name[128];
    int e1, e2;
    te_new_design();
    td->build();
    e1 = te_emit(td, SV_STD_SV2012, 0, &sv);
    e2 = te_emit(td, SV_STD_V2005, 0, &v);
    TE_CHECK(e1 == 0 && e2 == 0);
    te_golden(td->name, &sv);
    snprintf(name, sizeof name, "%s_v2005", td->name);
    te_golden(name, &v);
    te_compile_check(td, "sv", &sv, 0);
    te_compile_check(td, "v2005", &v, 1);
    te_json_roundtrip(td, &sv, SV_STD_SV2012);
    if (td->xcheck)
        te_xcheck(td, &sv, &v);
    if (D->nregmaps)
        te_regmap_tools(td, &sv);
    sv_output_free(&sv);
    sv_output_free(&v);
}

/* ------------------------------------------------------------ unit tests */

/* sv_emit_wrapper: the port-less test wrapper of the Verilator backend
 * (golden tests/fixtures/emit/wrapper/: the design's files plus tern_tb.sv,
 * compiled together with iverilog) */
static void te_test_wrapper(void)
{
    static const TeDesign wd = {"wrapper", te_types, 0, 0, 0, 0, NULL};
    SvOutput out;
    SvOptions o;
    StrBuf w = {0};
    te_new_design();
    te_types();
    sv_options_init(&o);
    o.constraints = 0;
    memset(&out, 0, sizeof out);
    TE_CHECK(emit_design(D, &o, &out) == 0);
    sv_emit_wrapper(D, &o, "tern_tb", &w);
    TE_CHECK(strstr(sb_cstr(&w), "module tern_tb") != NULL);
    TE_CHECK(strstr(sb_cstr(&w), " dut (") != NULL);
    sv_output_add(&out, "tern_tb.sv", SV_FILE_MODULE, &w);
    te_golden("wrapper", &out);
    te_compile_check(&wd, "sv", &out, 0);
    sv_output_free(&out);
    sb_free(&w);
}

static void te_test_names(void)
{
    TE_CHECK(sv_is_keyword("bit"));
    TE_CHECK(sv_is_keyword("logic"));
    TE_CHECK(sv_is_keyword("always_ff"));
    TE_CHECK(sv_is_keyword("xor"));
    TE_CHECK(!sv_is_keyword("bits"));
    TE_CHECK(!sv_is_keyword("clk"));
    TE_CHECK(!sv_is_keyword(""));
    {
        size_t i;
        for (i = 1; i < TERN_ARRAY_LEN(sv_keywords); i++)
            TE_CHECK(strcmp(sv_keywords[i - 1], sv_keywords[i]) < 0);
    }
    te_new_design();
    te_names();
    {
        SvNames *n = sv_names_new(D);
        const IrModule *top = D->top;
        TE_CHECK(strcmp(sv_names_module(n, top), "module_") == 0);
        TE_CHECK(strcmp(sv_names_signal(n, top, ir_find_signal(top, "bit")), "bit_") == 0);
        TE_CHECK(strcmp(sv_names_signal(n, top, ir_find_signal(top, "bit_")), "bit__") == 0);
        TE_CHECK(strcmp(sv_names_signal(n, top, ir_find_signal(top, "logic")), "logic_") == 0);
        TE_CHECK(strcmp(sv_names_signal(n, top, ir_find_signal(top, "9lives")), "_9lives") == 0);
        TE_CHECK(strcmp(sv_names_instance(n, top, 0), "begin_") == 0);
        TE_CHECK(strcmp(sv_names_module(n, top->insts[0].module), "end_") == 0);
        TE_CHECK(strcmp(sv_names_types_pkg(n), "module_types_pkg") == 0);
        sv_names_free(n);
    }
}

/* ASIC target: power-on values are dropped with a warning */
static void te_test_asic(void)
{
    SvOutput out;
    const SvFile *f;
    te_new_design();
    te_resets();
    te_warnings = 0;
    te_emit(&te_designs[2], SV_STD_SV2012, 1, &out);
    diag_flush();
    if (te_warnings != 2)
        te_fail("asic: %d warnings (expected 2)", te_warnings);
    TE_CHECK(strstr(te_last_warning, "power-on value") != NULL);
    f = sv_output_find(&out, "PowerOn.sv");
    TE_CHECK(f && strstr(f->data, "WARNING: power-on value of p") && !strstr(f->data, "initial"));
    {
        SvOutput g;
        StrBuf t = {0};
        memset(&g, 0, sizeof g);
        if (f) {
            sb_append(&t, f->data, f->len);
            sv_output_add(&g, "PowerOn.sv", SV_FILE_MODULE, &t);
        }
        te_golden("resets_asic", &g);
        sv_output_free(&g);
    }
    sv_output_free(&out);
}

/* options: no line markers, assertions stripped, concatenated output */
static void te_test_options(void)
{
    SvOptions o;
    SvOutput out;
    StrBuf cat = {0};
    const SvFile *f;
    te_new_design();
    te_asserts();
    sv_options_init(&o);
    o.line_markers = 0;
    o.constraints = 0;
    memset(&out, 0, sizeof out);
    TE_CHECK(emit_design(D, &o, &out) == 0);
    f = sv_output_find(&out, "Checked.sv");
    TE_CHECK(f != NULL);
    if (f) {
        TE_CHECK(strstr(f->data, "// tern:") == NULL);
        TE_CHECK(strstr(f->data, "assert") == NULL);
        TE_CHECK(strstr(f->data, "property") == NULL);
    }
    sv_output_concat(&out, &cat);
    TE_CHECK(strncmp(sb_cstr(&cat), "// ---- file: Checked.sv ----\n", 30) == 0);
    sb_free(&cat);
    sv_output_free(&out);
}

/* `tern emit --ir` end to end */
static void te_test_cmd(void)
{
    StrBuf js = {0};
    char *argv[8];
    const char *dir = "build/emit_work/cmd";
    char *jp, *op, *data = NULL;
    size_t len = 0;
    SvOutput ref;
    const SvFile *f;
    te_new_design();
    te_counter();
    ir_to_json(D, &js);
    os_remove_all(dir);
    os_mkdir_p(dir);
    jp = te_path2(dir, "design.json");
    os_write_file(jp, js.data, js.len);
    op = te_path2(dir, "out");
    argv[0] = (char *)"emit";
    argv[1] = (char *)"--ir";
    argv[2] = jp;
    argv[3] = (char *)"--std";
    argv[4] = (char *)"verilog2005";
    argv[5] = (char *)"-o";
    argv[6] = op;
    argv[7] = NULL;
    TE_CHECK(cmd_emit(7, argv) == 0);
    {
        char *p = te_path2(op, "Counter.v");
        TE_CHECK(os_read_file(p, &data, &len) == 0);
        free(p);
    }
    {
        SvOptions o;
        sv_options_init(&o);
        o.std = SV_STD_V2005;
        memset(&ref, 0, sizeof ref);
        emit_design(D, &o, &ref);
        f = sv_output_find(&ref, "Counter.v");
        TE_CHECK(f && data && f->len == len && memcmp(f->data, data, len) == 0);
        sv_output_free(&ref);
    }
    argv[1] = (char *)"--bogus";
    TE_CHECK(cmd_emit(2, argv) == TERN_EXIT_USAGE);
    free(data);
    free(jp);
    free(op);
    sb_free(&js);
}

int main(void)
{
    StrBuf src = {0};
    size_t i;
    char *up = os_getenv("UPDATE");
    te_update = up && *up && strcmp(up, "0") != 0;
    free(up);
    for (i = 1; i <= 700; i++)
        sb_printf(&src, "    line %u\n", (unsigned)i);
    te_file = src_add("src/demo.tern", src.data, src.len);
    sb_free(&src);
    diag_set_sink(te_diag_sink, NULL);
    te_iverilog = os_find_program("iverilog");
    te_vvp = os_find_program("vvp");
    if (!te_iverilog)
        printf("emit tests: iverilog not found on PATH: compile checks and simulator cross-checks skipped\n");

    te_test_names();
    for (i = 0; i < TERN_ARRAY_LEN(te_designs); i++)
        te_design(&te_designs[i]);
    te_test_asic();
    te_test_wrapper();
    te_test_options();
    te_test_cmd();
    TE_CHECK(te_errors == 0);

    free(te_iverilog);
    free(te_vvp);
    arena_release(&te_arena);
    src_free_all();
    intern_free_all();
    if (te_fails) {
        printf("emit unit tests: %d of %d checks FAILED\n", te_fails, te_checks);
        return 1;
    }
    printf("emit unit tests: all %d checks passed%s%s\n", te_checks, te_skips ? " " : "",
           te_skips ? "(some iverilog checks skipped)" : "");
    if (te_update)
        printf("emit unit tests: golden files updated (UPDATE=1)\n");
    return 0;
}
