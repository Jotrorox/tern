/* test_backend.c — tests for src/backend: build targets, the formal file
 * generator, the sby log parser, the VCD reader, counterexample -> test,
 * synthesis scripts and report parsers (canned reports in
 * tests/fixtures/synth/, a hand-written trace in tests/fixtures/formal/).
 * No external tool is required; with Icarus Verilog on PATH the generated
 * formal files and the golden `tern build` outputs (tests/fixtures/build/)
 * are compiled as a smoke test.
 *
 *     cc -o build/test_backend tests/unit/test_backend.c && build/test_backend
 *
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
#include "../../src/emit/sv.c"
#include "../../src/emit/constraints.c"
#include "../../src/emit/regmap.c"
#include "../../src/pkg/toml.c"
#include "../../src/backend/target.c"
#include "../../src/backend/vcdread.c"
#include "../../src/backend/formal.c"
#include "../../src/backend/synth.c"
#include "../../src/backend/verilator.c"
#include "../../src/os/os.h"

/* ------------------------------------------------------------ framework */

static int tb_checks, tb_fails, tb_skips;
static char *tb_iverilog;

static void tb_check(int ok, const char *expr, const char *file, int line)
{
    tb_checks++;
    if (!ok) {
        tb_fails++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

#define TB_CHECK(c) tb_check((c) ? 1 : 0, #c, __FILE__, __LINE__)

static void tb_check_str(const char *got, const char *want, const char *what, int line)
{
    tb_checks++;
    if (strcmp(got ? got : "(null)", want) != 0) {
        tb_fails++;
        fprintf(stderr, "test_backend.c:%d: %s differs\n--- want\n%s\n--- got\n%s\n", line, what, want,
                got ? got : "(null)");
    }
}

static int tb_near(double a, double b)
{
    double d = a - b;
    return d < 0.01 && d > -0.01;
}

static char *tb_read(const char *path)
{
    char *text = NULL;
    size_t len = 0;
    if (os_read_file(path, &text, &len) != 0) {
        fprintf(stderr, "test_backend: cannot read %s (run from the repository root)\n", path);
        tb_fails++;
        return NULL;
    }
    return text;
}

static int tb_run(char *const *argv, const char *cwd, char **out_text)
{
    OsSpawnOpts o;
    OsRunResult r;
    int rc;
    memset(&o, 0, sizeof o);
    o.cwd = cwd;
    o.stdin_null = 1;
    o.capture_stdout = 1;
    o.merge_stderr = 1;
    rc = os_run(argv, &o, &r);
    if (out_text)
        *out_text = r.out ? xstrdup(r.out) : xstrdup("");
    os_run_result_free(&r);
    return rc;
}

/* ------------------------------------------------------------- targets */

static void tb_targets(void)
{
    Arena a = {0};
    BkTarget t;
    TernTarget tt;
    TernTargetKV kv[8];
    TomlValue *doc, *v;
    uint32_t src;
    const char *text = "[t]\nvendor = \"intel\"\nconstraints = [\"xdc\", \"sdc\"]\nmem_init = \"coe,mif\"\n"
                       "features = [\"fast\", \"-debug\"]\nclock_hz = 50_000_000\npackage = \"sg48\"\n";
    TB_CHECK(bk_family_from_device("xc7a35tcpg236-1") == BK_FAM_XILINX);
    TB_CHECK(bk_family_from_device("XCKU040") == BK_FAM_XILINX);
    TB_CHECK(bk_family_from_device("ice40up5k") == BK_FAM_ICE40);
    TB_CHECK(bk_family_from_device("hx8k") == BK_FAM_ICE40);
    TB_CHECK(bk_family_from_device("LFE5U-25F") == BK_FAM_ECP5);
    TB_CHECK(bk_family_from_device("5CEBA4F23C7") == BK_FAM_INTEL);
    TB_CHECK(bk_family_from_device("10M50DAF484C7G") == BK_FAM_INTEL);
    TB_CHECK(bk_family_from_device("EP4CE22F17C6") == BK_FAM_INTEL);
    TB_CHECK(bk_family_from_device("GW1NR-LV9QN88PC6/I5") == BK_FAM_GOWIN);
    TB_CHECK(bk_family_from_device("sky130_fd_sc_hd") == BK_FAM_ASIC);
    TB_CHECK(bk_family_from_device("mystery") == BK_FAM_GENERIC);
    TB_CHECK(bk_family_from_device(NULL) == BK_FAM_GENERIC);
    TB_CHECK(bk_family_default_tool(BK_FAM_XILINX) == BK_TOOL_VIVADO);
    TB_CHECK(bk_family_default_tool(BK_FAM_INTEL) == BK_TOOL_QUARTUS);
    TB_CHECK(bk_family_default_tool(BK_FAM_ICE40) == BK_TOOL_YOSYS);

    /* no [target]: defaults */
    TB_CHECK(bk_target_resolve(NULL, NULL, &t, &a) == 0);
    TB_CHECK(strcmp(t.name, "default") == 0);
    TB_CHECK(t.std == SV_STD_SV2012 && t.kind == SV_TARGET_FPGA);
    TB_CHECK(t.constraints == (EMIT_CONS_XDC | EMIT_CONS_SDC) && t.mem_init == 0);

    /* a [target] table with extra keys */
    src = src_add("tern.toml", text, strlen(text));
    doc = toml_parse(&a, src);
    TB_CHECK(doc != NULL);
    memset(&tt, 0, sizeof tt);
    tt.name = "t";
    tt.device = "5CEBA4F23C7";
    tt.top = "Top";
    tt.backend = "quartus";
    tt.std = "verilog2005";
    tt.extra = kv;
    for (v = doc ? (TomlValue *)toml_get(doc, "t")->first : NULL; v && tt.nextra < 8; v = v->next) {
        kv[tt.nextra].key = v->key;
        kv[tt.nextra].value = v;
        tt.nextra++;
    }
    TB_CHECK(bk_target_resolve(&tt, NULL, &t, &a) == 0);
    TB_CHECK(t.family == BK_FAM_INTEL && t.tool == BK_TOOL_QUARTUS);
    TB_CHECK(t.std == SV_STD_V2005);
    TB_CHECK(t.constraints == (EMIT_CONS_XDC | EMIT_CONS_SDC));
    TB_CHECK(t.mem_init == (SV_MEMINIT_COE | SV_MEMINIT_MIF));
    TB_CHECK(t.features && strcmp(t.features, "fast,-debug") == 0);
    TB_CHECK(t.clock_hz == 50000000u);
    TB_CHECK(t.package && strcmp(t.package, "sg48") == 0);
    TB_CHECK(t.top && strcmp(t.top, "Top") == 0);
    /* family defaults when the keys are absent */
    tt.nextra = 0;
    tt.device = "xc7a35t";
    tt.backend = NULL;
    TB_CHECK(bk_target_resolve(&tt, NULL, &t, &a) == 0);
    TB_CHECK(t.family == BK_FAM_XILINX && t.constraints == EMIT_CONS_XDC && t.tool == BK_TOOL_NONE);
    arena_release(&a);
}

static void tb_rewrite(void)
{
    SvOutput o;
    StrBuf sb = {0};
    const char *from[2] = {"/home/u/tern/std", "/home/u/proj"}, *to[2] = {"std/", ""};
    memset(&o, 0, sizeof o);
    sb_puts(&sb, "x;  // tern: /home/u/proj/src/a.tern:3\ny;  // tern: /home/u/tern/std/fifo/fifo.tern:28\n"
                 "z;  // tern: /home/u/projx/b.tern:1\n");
    sv_output_add(&o, "a.sv", SV_FILE_MODULE, &sb);
    bk_rewrite_paths(&o, from, to, 2);
    tb_check_str(o.files[0].data,
                 "x;  // tern: src/a.tern:3\ny;  // tern: std/fifo/fifo.tern:28\nz;  // tern: /home/u/projx/b.tern:1\n",
                 "rewritten paths", __LINE__);
    sv_output_free(&o);
    sb_free(&sb);
}

/* ------------------------------------------------------- a small design */

static Arena tb_arena;
static IrDesign *D;
static uint32_t tb_file;

static Span SP(uint32_t line)
{
    Span s;
    memset(&s, 0, sizeof s);
    s.file = tb_file;
    s.line = line;
    s.col = 5;
    return s;
}

static IrExpr *S(IrModule *m, uint32_t sig) { return ir_sig(D->arena, m, sig, SP(1)); }
static IrExpr *K(uint32_t w, uint64_t v) { return ir_const_u64(D->arena, w, v, SP(1)); }
static IrExpr *BIN(IrOp op, IrExpr *a, IrExpr *b) { return ir_binary(D->arena, op, a, b, 0, SP(1)); }

/* Counter: count += din while en; an immediate assertion under `if en`,
 * properties `always en |=> count != 0`, `always (count == 1 ##[1:3]
 * count == 2) |-> en`, cover `count == 3`, and an unbounded eventually
 * (not translatable) */
static IrModule *tb_counter(void)
{
    static const char *const vn[] = {"Idle", "Run", "Halt"};
    IrVariant v[3];
    IrModule *m;
    IrType *st;
    uint32_t clk, rst, en, din, mode, delta, q, count, i;
    IrStmt *as, *upd;
    IrStmtList body = {0}, then_l = {0};
    for (i = 0; i < 3; i++) {
        v[i].name = vn[i];
        v[i].tag = ir_bits_u64(D->arena, 2, i);
        v[i].payload = NULL;
    }
    st = ir_type_enum(D, "State", 3, v, 2, IR_ENC_BINARY, SP(2));
    m = ir_module_new(D, "Counter", "Counter", "Counter", SP(3));
    m->package = "demo";
    clk = ir_add_signal(D, m, "clk", NULL, IRS_IN, ir_type_clock(D, 0), SP(4));
    rst = ir_add_signal(D, m, "rst", NULL, IRS_IN, ir_type_reset(D, 0, 0), SP(5));
    en = ir_add_signal(D, m, "en", NULL, IRS_IN, ir_type_bool(D), SP(6));
    din = ir_add_signal(D, m, "din", NULL, IRS_IN, ir_type_uint(D, 8), SP(7));
    mode = ir_add_signal(D, m, "mode", NULL, IRS_IN, st, SP(8));
    delta = ir_add_signal(D, m, "delta", NULL, IRS_IN, ir_type_sint(D, 4), SP(9));
    q = ir_add_signal(D, m, "q", NULL, IRS_OUT, ir_type_uint(D, 8), SP(10));
    count = ir_add_signal(D, m, "count", NULL, IRS_REG, ir_type_uint(D, 8), SP(11));
    TERN_UNUSED(mode);
    TERN_UNUSED(delta);
    ir_add_domain(D, m, "", clk, rst, 0);
    m->default_domain = 0;
    m->sigs[count].domain = 0;
    ir_set_init(D, m, count, ir_bits_u64(D->arena, 8, 0));
    ir_add_assign(D, m, ir_lv_sig(m, q), S(m, count), SP(12));
    as = ir_stmt_assert(D->arena, IRA_ASSERT, BIN(IROP_NE, S(m, count), K(8, 0xff)), "overflow", SP(14));
    upd = ir_stmt_assign(D->arena, ir_lv_sig(m, count), BIN(IROP_ADD, S(m, count), S(m, din)), SP(15));
    ir_stmts_push(&then_l, as);
    ir_stmts_push(&then_l, upd);
    ir_stmts_push(&body, ir_stmt_if(D->arena, S(m, en), ir_stmts_finish(D->arena, &then_l), ir_block(D->arena, 0, NULL),
                                    SP(13)));
    ir_add_process(D, m, IRP_CLOCKED, 0, ir_stmts_finish(D->arena, &body), SP(13));
    ir_add_property(D, m, "p1", IRA_ASSERT, 0,
                    ir_seq_unary(D->arena, IRQ_ALWAYS,
                                 ir_seq_binary(D->arena, IRQ_IMPL_NEXT, ir_seq_expr(D->arena, S(m, en)),
                                               ir_seq_expr(D->arena, BIN(IROP_NE, S(m, count), K(8, 0))))),
                    SP(17));
    ir_add_property(D, m, "p2", IRA_ASSERT, 0,
                    ir_seq_unary(D->arena, IRQ_ALWAYS,
                                 ir_seq_binary(D->arena, IRQ_IMPL,
                                               ir_seq_delay(D->arena, ir_seq_expr(D->arena, BIN(IROP_EQ, S(m, count), K(8, 1))),
                                                            1, 3,
                                                            ir_seq_expr(D->arena, BIN(IROP_EQ, S(m, count), K(8, 2)))),
                                               ir_seq_expr(D->arena, S(m, en)))),
                    SP(18));
    ir_add_property(D, m, "c1", IRA_COVER, 0, ir_seq_expr(D->arena, BIN(IROP_EQ, S(m, count), K(8, 3))), SP(19));
    ir_add_property(D, m, "live", IRA_ASSERT, 0,
                    ir_seq_unary(D->arena, IRQ_ALWAYS,
                                 ir_seq_eventually(D->arena, ir_seq_expr(D->arena, S(m, en)), IR_NONE)),
                    SP(20));
    ir_design_set_top(D, m);
    return m;
}

static const char *tb_counter_src =
    "package demo\n"
    "enum State { Idle, Run, Halt }\n"
    "module Counter {\n"
    "    in clk: clock\n"
    "    in rst: reset\n"
    "    in en: bool\n"
    "    in din: u8\n"
    "    in mode: State\n"
    "    in delta: sint<4>\n"
    "    out q: u8\n"
    "    reg count: u8 = 0\n"
    "    q = count\n"
    "    on clk { if en {\n"
    "        assert count != 0xff, \"overflow\"\n"
    "        count = count +% din } }\n"
    "\n"
    "    property p1 { always en |=> count != 0 }\n"
    "    property p2 { always (count == 1 ##[1:3] count == 2) |-> en }\n"
    "    cover property c1 { count == 3 }\n"
    "    property live { always eventually en }\n"
    "}\n";

static const FvCheck *tb_find_check(const FvPlan *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->checks.len; i++)
        if (p->checks.data[i].name ? strcmp(p->checks.data[i].name, name) == 0 : strcmp(name, "assert") == 0)
            return &p->checks.data[i];
    return NULL;
}

/* the text of line `line` (1-based) of a file of the plan */
static char *tb_line_of(const FvPlan *p, const char *file, uint32_t line)
{
    const SvFile *f = sv_output_find(&p->files, file);
    const char *s;
    uint32_t l = 1;
    if (!f)
        return xstrdup("");
    for (s = f->data; *s && l < line; s++)
        if (*s == '\n')
            l++;
    return xstrndup(s, strcspn(s, "\n"));
}

static void tb_formal(void)
{
    IrModule *m;
    FvPlan plan;
    FvOptions fo;
    const FvCheck *c;
    const SvFile *f;
    char *l;
    tb_file = src_add("counter.tern", tb_counter_src, strlen(tb_counter_src));
    arena_release(&tb_arena);
    D = ir_design_new(&tb_arena);
    m = tb_counter();
    fv_options_init(&fo);
    fo.depth = 12;
    fo.induct = 1;
    TB_CHECK(fv_generate(D, m, &fo, &plan) == 0);
    TB_CHECK(plan.name && strcmp(plan.name, "Counter") == 0);
    TB_CHECK(plan.has_cover && plan.has_assert && !plan.multiclock);
    TB_CHECK(plan.checks.len == 5);
    TB_CHECK(sv_output_find(&plan.files, "Counter.v") != NULL);
    TB_CHECK(sv_output_find(&plan.files, "tern_formal_top.v") != NULL);
    TB_CHECK((f = sv_output_find(&plan.files, "Counter.sby")) != NULL);
    if (f) {
        TB_CHECK(strstr(f->data, "[tasks]\nbmc\nprove\ncover\n") != NULL);
        TB_CHECK(strstr(f->data, "depth 12\n") != NULL);
        TB_CHECK(strstr(f->data, "smtbmc yices") != NULL);
        TB_CHECK(strstr(f->data, "read_verilog -formal Counter.v\nread_verilog -formal tern_formal_top.v\n") != NULL);
        TB_CHECK(strstr(f->data, "prep -top tern_formal_top") != NULL);
    }
    f = sv_output_find(&plan.files, "tern_formal_top.v");
    if (f) {
        TB_CHECK(strstr(f->data, "Counter dut (") != NULL);
        TB_CHECK(strstr(f->data, "assume (rst); else assume (!rst);") != NULL);
        TB_CHECK(strstr(f->data, "input  wire [7:0] din,") != NULL);
    }
    f = sv_output_find(&plan.files, "Counter.v");
    if (f) {
        TB_CHECK(strstr(f->data, "`ifdef FORMAL") != NULL);
        TB_CHECK(strstr(f->data, "`endif\nendmodule") != NULL);
    }
    /* every translated check points at its `assert/cover (...)` line */
    c = tb_find_check(&plan, "assert");
    TB_CHECK(c && c->status == FV_UNKNOWN && c->msg && strcmp(c->msg, "overflow") == 0);
    if (c) {
        l = tb_line_of(&plan, c->file, c->line);
        TB_CHECK(strstr(l, "always @* if (") && strstr(l, ") assert ("));
        free(l);
    }
    c = tb_find_check(&plan, "p1");
    TB_CHECK(c && c->status == FV_UNKNOWN && c->kind == IRA_ASSERT);
    if (c) {
        l = tb_line_of(&plan, c->file, c->line);
        TB_CHECK(strstr(l, "assert (") != NULL);
        free(l);
    }
    c = tb_find_check(&plan, "p2");
    TB_CHECK(c && c->status == FV_UNKNOWN);
    c = tb_find_check(&plan, "c1");
    TB_CHECK(c && c->kind == IRA_COVER && c->status == FV_UNKNOWN);
    if (c) {
        l = tb_line_of(&plan, c->file, c->line);
        TB_CHECK(strstr(l, "cover (") != NULL);
        free(l);
    }
    c = tb_find_check(&plan, "live");
    TB_CHECK(c && c->status == FV_SKIPPED && c->note && strstr(c->note, "liveness"));

    /* the generated files compile (iverilog accepts immediate assertions) */
    if (tb_iverilog) {
        char *dir = xstrdup("build/backend_work/formal"), *log = NULL;
        size_t i;
        char *argv[8];
        int rc;
        os_mkdir_p(dir);
        for (i = 0; i < plan.files.nfiles; i++) {
            char *p = os_path_join(dir, plan.files.files[i].name);
            os_write_file(p, plan.files.files[i].data, plan.files.files[i].len);
            free(p);
        }
        argv[0] = tb_iverilog;
        argv[1] = (char *)"-g2012";
        argv[2] = (char *)"-DFORMAL";
        argv[3] = (char *)"-o";
        argv[4] = (char *)"formal.vvp";
        argv[5] = (char *)"Counter.v";
        argv[6] = (char *)"tern_formal_top.v";
        argv[7] = NULL;
        rc = tb_run(argv, dir, &log);
        if (rc != 0)
            fprintf(stderr, "iverilog on the formal files:\n%s\n", log);
        TB_CHECK(rc == 0);
        free(log);
        free(dir);
    } else {
        tb_skips++;
    }

    /* sby output: line numbers of this plan */
    {
        StrBuf log = {0};
        const FvCheck *a = tb_find_check(&plan, "p1"), *cv = tb_find_check(&plan, "c1");
        if (a && cv) {
            sb_printf(&log, "SBY 1 [Counter_bmc] engine_0: ##   0:00:00  Checking assertions in step 4..\n");
            sb_printf(&log, "SBY 1 [Counter_bmc] engine_0: ##   0:00:00  Assert failed in tern_formal_top.dut: %s:%u.13-%u.40\n",
                      a->file, (unsigned)a->line, (unsigned)a->line);
            sb_printf(&log, "SBY 1 [Counter_bmc] engine_0: ##   0:00:00  Writing trace to VCD file: engine_0/trace.vcd\n");
            sb_printf(&log, "SBY 1 [Counter_bmc] DONE (FAIL, rc=2)\n");
            TB_CHECK(fv_parse_sby_log(&plan, "bmc", log.data) == FV_FAIL);
            a = tb_find_check(&plan, "p1");
            TB_CHECK(a->status == FV_FAIL && a->step == 4);
            TB_CHECK(a->trace && strcmp(a->trace, "Counter_bmc/engine_0/trace.vcd") == 0);
            TB_CHECK(tb_find_check(&plan, "assert")->status == FV_UNKNOWN);
            sb_reset(&log);
            sb_printf(&log, "SBY 1 [Counter_cover] DONE (PASS, rc=0)\n");
            TB_CHECK(fv_parse_sby_log(&plan, "cover", log.data) == FV_PASS);
            TB_CHECK(tb_find_check(&plan, "c1")->status == FV_REACHED);
            sb_reset(&log);
            sb_printf(&log, "SBY 1 [Counter_prove] DONE (PASS, rc=0)\n");
            TB_CHECK(fv_parse_sby_log(&plan, "prove", log.data) == FV_PASS);
            TB_CHECK(tb_find_check(&plan, "assert")->status == FV_PASS);
            TB_CHECK(tb_find_check(&plan, "p1")->status == FV_FAIL);
            TB_CHECK(tb_find_check(&plan, "live")->status == FV_SKIPPED);
        }
        sb_free(&log);
    }
    fv_plan_free(&plan);
}

/* the canned SymbiYosys log against a hand-made plan */
static void tb_sby_log(void)
{
    FvPlan plan;
    FvCheck c;
    char *log = tb_read("tests/fixtures/formal/sby_bmc_fail.log");
    if (!log)
        return;
    memset(&plan, 0, sizeof plan);
    plan.name = "Counter";
    memset(&c, 0, sizeof c);
    c.file = "Counter.v";
    c.kind = IRA_ASSERT;
    c.name = "a40";
    c.line = 40;
    vec_push(plan.checks, c);
    c.name = "a41";
    c.line = 41;
    vec_push(plan.checks, c);
    c.kind = IRA_COVER;
    c.name = "c44";
    c.line = 44;
    vec_push(plan.checks, c);
    c.name = "c45";
    c.line = 45;
    vec_push(plan.checks, c);
    TB_CHECK(fv_parse_sby_log(&plan, "bmc", log) == FV_FAIL);
    TB_CHECK(plan.checks.data[0].status == FV_UNKNOWN);
    TB_CHECK(plan.checks.data[1].status == FV_FAIL && plan.checks.data[1].step == 5);
    TB_CHECK(plan.checks.data[1].trace && strcmp(plan.checks.data[1].trace, "Counter_bmc/engine_0/trace.vcd") == 0);
    TB_CHECK(fv_parse_sby_log(&plan, "cover", log) == FV_FAIL);
    TB_CHECK(plan.checks.data[2].status == FV_REACHED && plan.checks.data[2].step == 3);
    TB_CHECK(plan.checks.data[2].trace && strcmp(plan.checks.data[2].trace, "Counter_cover/engine_0/trace0.vcd") == 0);
    TB_CHECK(plan.checks.data[3].status == FV_UNREACHED);
    vec_free(plan.checks);
    arena_release(&plan.arena);
    free(log);
}

/* ------------------------------------------------------------ VCD + cex */

static void tb_vcd(void)
{
    char *text = tb_read("tests/fixtures/formal/counter_trace.vcd");
    VcdrTrace t;
    char *err = NULL;
    int clk, din, q, cnt, step;
    uint64_t *edges = NULL;
    size_t n;
    if (!text)
        return;
    TB_CHECK(vcdr_parse(text, strlen(text), &t, &err) == 0);
    if (err) {
        fprintf(stderr, "vcd: %s\n", err);
        free(err);
        free(text);
        return;
    }
    TB_CHECK(t.timescale && strcmp(t.timescale, "1ns") == 0);
    TB_CHECK(t.nvars == 11);
    clk = vcdr_find(&t, "tern_formal_top.clk");
    din = vcdr_find(&t, "din");
    q = vcdr_find(&t, "q"); /* the shallowest: tern_formal_top.q */
    cnt = vcdr_find(&t, "dut.count");
    step = vcdr_find(&t, "smt_step");
    TB_CHECK(clk >= 0 && din >= 0 && q >= 0 && cnt >= 0 && step >= 0);
    TB_CHECK(q >= 0 && strcmp(t.vars[q].full, "tern_formal_top.q") == 0);
    TB_CHECK(din >= 0 && t.vars[din].width == 8 && strcmp(t.vars[din].name, "din") == 0);
    TB_CHECK(vcdr_find(&t, "nope") == -1);
    tb_check_str(vcdr_value_at(&t, din, 0), "00000000", "din@0", __LINE__);
    tb_check_str(vcdr_value_at(&t, din, 25), "00010010", "din@25", __LINE__);
    tb_check_str(vcdr_value_at(&t, din, 40), "00000101", "din@40 (left-extended)", __LINE__);
    tb_check_str(vcdr_value_at(&t, step, 50), "00000000000000000000000000000101", "smt_step@50", __LINE__);
    /* aliases share the identifier code */
    TB_CHECK(t.vars[vcdr_find(&t, "dut.q")].code == t.vars[q].code);
    n = vcdr_rising_edges(&t, clk, &edges);
    TB_CHECK(n == 6 && edges[0] == 0 && edges[5] == 50);
    free(edges);
    vcdr_free(&t);
    TB_CHECK(vcdr_parse("$var wire 1 ! a $end\n$enddefinitions $end\n#0\nq!\n", 46, &t, &err) == -1);
    TB_CHECK(err != NULL);
    free(err);
    free(text);
}

static void tb_cex(void)
{
    char *text = tb_read("tests/fixtures/formal/counter_trace.vcd");
    VcdrTrace t;
    char *err = NULL;
    IrModule *m;
    FvCex cx;
    StrBuf out = {0};
    if (!text)
        return;
    arena_release(&tb_arena);
    D = ir_design_new(&tb_arena);
    m = tb_counter();
    TB_CHECK(vcdr_parse(text, strlen(text), &t, &err) == 0);
    memset(&cx, 0, sizeof cx);
    cx.module = m;
    cx.scope = "tern_formal_top";
    cx.test_name = "formal: Counter p1";
    cx.package = "demo_test";
    cx.use_path = "demo::Counter";
    cx.failing = "property `p1` (counter.tern:17)";
    cx.source_line = "property p1 { always en |=> count != 0 }";
    cx.reset_cycles = 2;
    cx.fail_step = 5;
    TB_CHECK(fv_cex_to_test(&cx, &t, &out, &err) == 0);
    tb_check_str(out.data,
                 "// Generated by `tern prove` from a counterexample: property `p1` (counter.tern:17).\n"
                 "//     property p1 { always en |=> count != 0 }\n"
                 "// The simulator reports the failing check during the last step. Registers\n"
                 "// without a reset value are free in the proof; if the test passes, the\n"
                 "// failure depends on their power-on values.\n"
                 "package demo_test\n"
                 "\n"
                 "use demo::Counter\n"
                 "\n"
                 "test \"formal: Counter p1\" {\n"
                 "    let dut = sim Counter()\n"
                 "    dut.reset()\n"
                 "    dut.en = true; dut.din = 0x12; dut.mode = .Run; dut.delta = -2\n"
                 "    dut.step(2)\n"
                 "    dut.din = 5\n"
                 "    dut.step()\n"
                 "    dut.en = false\n"
                 "    dut.step()\n"
                 "}\n",
                 "generated test", __LINE__);
    /* a shorter failure: the trace is cut at fail_step */
    sb_reset(&out);
    cx.fail_step = 3;
    cx.package = NULL;
    cx.use_path = NULL;
    cx.source_line = NULL;
    TB_CHECK(fv_cex_to_test(&cx, &t, &out, &err) == 0);
    TB_CHECK(strstr(out.data, "dut.step(2)\n}\n") != NULL && !strstr(out.data, "package"));
    /* a port missing from the trace is an error */
    sb_reset(&out);
    cx.scope = "elsewhere";
    TB_CHECK(fv_cex_to_test(&cx, &t, &out, &err) == -1 && err && strstr(err, "not in the trace"));
    free(err);
    sb_free(&out);
    vcdr_free(&t);
    free(text);
}

/* --------------------------------------------------------------- synth */

static void tb_synth_scripts(void)
{
    static const char *const files[] = {"Top_types_pkg.sv", "Fifo.sv", "Top.sv"};
    SynScriptIn in;
    StrBuf sb = {0};
    memset(&in, 0, sizeof in);
    in.top = "Top";
    in.files = files;
    in.nfiles = 3;
    in.src_dir = ".";
    in.tool = BK_TOOL_YOSYS;
    in.family = BK_FAM_ICE40;
    in.device = "up5k";
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "read_verilog -DSYNTHESIS Top_types_pkg.sv Fifo.sv Top.sv\n") != NULL);
    TB_CHECK(strstr(sb.data, "hierarchy -check -top Top\n") != NULL);
    TB_CHECK(strstr(sb.data, "synth_ice40 -top Top -json Top.json\n") != NULL);
    TB_CHECK(strstr(sb.data, "tee -o utilization.txt stat\n") != NULL);
    sb_reset(&sb);
    in.family = BK_FAM_ECP5;
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "synth_ecp5 -top Top -json Top.json\n") != NULL);
    sb_reset(&sb);
    in.family = BK_FAM_XILINX;
    in.device = "xcku040";
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "synth_xilinx -top Top -family xcu -flatten -edif Top.edif\n") != NULL);
    sb_reset(&sb);
    in.family = BK_FAM_GENERIC;
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "synth -top Top -flatten\n") != NULL);

    sb_reset(&sb);
    in.tool = BK_TOOL_VIVADO;
    in.family = BK_FAM_XILINX;
    in.device = "xc7a35tcpg236-1";
    in.src_dir = "..";
    in.sv = 1;
    in.xdc = "Top.xdc";
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "set_part xc7a35tcpg236-1\n") != NULL);
    TB_CHECK(strstr(sb.data, "read_verilog -sv { ../Top_types_pkg.sv ../Fifo.sv ../Top.sv }\n") != NULL);
    TB_CHECK(strstr(sb.data, "read_xdc ../Top.xdc\n") != NULL);
    TB_CHECK(strstr(sb.data, "synth_design -top Top -part xc7a35tcpg236-1\nopt_design\nplace_design\nroute_design\n") !=
             NULL);
    TB_CHECK(strstr(sb.data, "report_utilization -file utilization.rpt\n") != NULL);
    TB_CHECK(strstr(sb.data, "report_timing_summary -file timing.rpt\n") != NULL);

    sb_reset(&sb);
    in.tool = BK_TOOL_QUARTUS;
    in.family = BK_FAM_INTEL;
    in.device = "10M50DAF484C7G";
    in.sdc = "Top.sdc";
    syn_script(&in, &sb);
    TB_CHECK(strstr(sb.data, "set_global_assignment -name FAMILY \"MAX 10\"\n") != NULL);
    TB_CHECK(strstr(sb.data, "set_global_assignment -name DEVICE 10M50DAF484C7G\n") != NULL);
    TB_CHECK(strstr(sb.data, "set_global_assignment -name SYSTEMVERILOG_FILE ../Top.sv\n") != NULL);
    TB_CHECK(strstr(sb.data, "set_global_assignment -name SDC_FILE ../Top.sdc\n") != NULL);
    TB_CHECK(strstr(sb.data, "execute_flow -compile\n") != NULL);
    TB_CHECK(strcmp(syn_script_name(BK_TOOL_QUARTUS), "synth_quartus.tcl") == 0);
    sb_free(&sb);
}

static void tb_synth_reports(void)
{
    SynReport r;
    char *t;
    StrBuf sb = {0};

    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/yosys_ice40_stat.txt")) != NULL) {
        TB_CHECK(syn_parse_yosys_stat(t, &r) > 0);
        TB_CHECK(r.luts == 30 && r.ffs == 27 && r.brams == 2 && r.dsps == 1 && r.cells == 84);
        free(t);
    }
    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/yosys_xilinx_stat_new.txt")) != NULL) {
        TB_CHECK(syn_parse_yosys_stat(t, &r) > 0);
        TB_CHECK(r.luts == 56 && r.ffs == 44 && r.brams == 1 && r.dsps == 2 && r.cells == 120);
        free(t);
    }
    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/yosys_generic_hier_stat.txt")) != NULL) {
        TB_CHECK(syn_parse_yosys_stat(t, &r) > 0);
        TB_CHECK(r.luts == -1 && r.ffs == 8 && r.brams == 0 && r.dsps == 0 && r.cells == 24);
        syn_report_print(&r, 0, &sb);
        TB_CHECK(strstr(sb.data, "  cells  24\n") != NULL);
        TB_CHECK(strstr(sb.data, "no timing analysis") != NULL);
        free(t);
    }
    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/nextpnr.log")) != NULL) {
        TB_CHECK(syn_parse_nextpnr(t, &r) == 2);
        TB_CHECK(tb_near(r.fmax_mhz, 98.52)); /* the post-route figure */
        TB_CHECK(r.has_wns && tb_near(r.period_ns, 83.333));
        free(t);
    }
    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/vivado_utilization.rpt")) != NULL) {
        TB_CHECK(syn_parse_vivado_util(t, &r) == 4);
        TB_CHECK(r.luts == 123 && r.ffs == 87 && r.brams == 2 && r.dsps == 2);
        free(t);
    }
    if ((t = tb_read("tests/fixtures/synth/vivado_timing.rpt")) != NULL) {
        TB_CHECK(syn_parse_vivado_timing(t, &r) == 2);
        TB_CHECK(r.has_wns && tb_near(r.wns_ns, 3.877));
        TB_CHECK(tb_near(r.period_ns, 10.0));
        TB_CHECK(tb_near(r.fmax_mhz, 163.32));
        sb_reset(&sb);
        syn_report_print(&r, 100.0, &sb);
        tb_check_str(sb.data,
                     "  LUTs   123\n  FFs    87\n  BRAM   2\n  DSP    2\n"
                     "  Fmax   163.3 MHz (target 100.0 MHz, met), WNS 3.877 ns\n",
                     "vivado summary", __LINE__);
        free(t);
    }
    syn_report_init(&r);
    if ((t = tb_read("tests/fixtures/synth/quartus.fit.summary")) != NULL) {
        TB_CHECK(syn_parse_quartus_fit(t, &r) == 4);
        TB_CHECK(r.luts == 52 && r.ffs == 64 && r.brams == 1 && r.dsps == 1);
        free(t);
    }
    if ((t = tb_read("tests/fixtures/synth/quartus.sta.summary")) != NULL) {
        TB_CHECK(syn_parse_quartus_sta(t, &r) == 2);
        TB_CHECK(r.has_wns && tb_near(r.wns_ns, 4.21));
        free(t);
    }
    if ((t = tb_read("tests/fixtures/synth/quartus.sta.rpt")) != NULL) {
        TB_CHECK(syn_parse_quartus_sta(t, &r) == 1);
        TB_CHECK(tb_near(r.fmax_mhz, 172.68));
        sb_reset(&sb);
        syn_report_print(&r, 200.0, &sb);
        TB_CHECK(strstr(sb.data, "Fmax   172.7 MHz (target 200.0 MHz, NOT MET), WNS 4.210 ns") != NULL);
        free(t);
    }
    sb_free(&sb);
}

/* ------------------------------------------- iverilog over .sv goldens */

static void tb_compile_goldens(void)
{
    OsDirList list;
    size_t i;
    if (!tb_iverilog) {
        tb_skips++;
        return;
    }
    if (os_list_dir("tests/fixtures/build", &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        const char *name = list.items[i].name;
        char *path, *text, *dir, *log = NULL;
        const char *p;
        VEC(char *) files = {0};
        VEC(char *) argv = {0};
        size_t k;
        int rc;
        if (strcmp(os_path_ext(name), ".sv") != 0)
            continue;
        path = os_path_join("tests/fixtures/build", name);
        text = tb_read(path);
        free(path);
        if (!text)
            continue;
        dir = os_path_join("build/backend_work/goldens", name);
        os_mkdir_p(dir);
        /* split the `// ---- file: X ----` concatenation */
        for (p = text; (p = strstr(p, "// ---- file: ")) != NULL;) {
            const char *nb = p + 14, *ne = strstr(nb, " ----\n"), *body, *next;
            char *fname, *fp;
            if (!ne)
                break;
            fname = xstrndup(nb, (size_t)(ne - nb));
            body = ne + 6;
            next = strstr(body, "// ---- file: ");
            fp = os_path_join(dir, fname);
            os_write_file(fp, body, next ? (size_t)(next - body) : strlen(body));
            free(fp);
            if (strcmp(os_path_ext(fname), ".sv") == 0 || strcmp(os_path_ext(fname), ".v") == 0)
                vec_push(files, fname);
            else
                free(fname);
            p = next ? next : body + strlen(body);
        }
        vec_push(argv, tb_iverilog);
        vec_push(argv, (char *)"-g2012");
        vec_push(argv, (char *)"-DTERN_NO_SVA");
        vec_push(argv, (char *)"-o");
        vec_push(argv, (char *)"out.vvp");
        for (k = 0; k < files.len; k++) /* the package first */
            if (strstr(files.data[k], "_types_pkg"))
                vec_push(argv, files.data[k]);
        for (k = 0; k < files.len; k++)
            if (!strstr(files.data[k], "_types_pkg"))
                vec_push(argv, files.data[k]);
        vec_push(argv, NULL);
        rc = tb_run(argv.data, dir, &log);
        if (rc != 0)
            fprintf(stderr, "iverilog on tests/fixtures/build/%s:\n%s\n", name, log);
        TB_CHECK(rc == 0);
        for (k = 0; k < files.len; k++)
            free(files.data[k]);
        vec_free(files);
        vec_free(argv);
        free(log);
        free(dir);
        free(text);
    }
    os_dir_list_free(&list);
}

/* --------------------------------------- the tern binary: build, prove */

static const char *tb_cli_src =
    "use std::fifo::Fifo\n"
    "module A {\n    in a: u8\n    out y: u8\n    y = a\n}\n"
    "module B {\n    in a: u8\n    out y: u8\n    y = a\n}\n";

static int tb_exists(const char *dir, const char *name)
{
    char *p = os_path_join(dir, name);
    int r = os_is_file(p);
    free(p);
    return r;
}

static void tb_cli(void)
{
    char *cwd = os_getcwd(), *tern, *dir = xstrdup("build/backend_work/cli"), *log = NULL, *p;
    char *argv[10];
    int rc;
    tern = os_path_join(cwd, "tern");
    if (!os_is_file(tern)) {
        tb_skips++;
        free(cwd);
        free(tern);
        free(dir);
        return;
    }
    os_mkdir_p(dir);
    p = os_path_join(dir, "two.tern");
    os_write_file(p, tb_cli_src, strlen(tb_cli_src));
    free(p);
    /* tern build removes the files the previous build generated, and only those */
    argv[0] = tern;
    argv[1] = (char *)"build";
    argv[2] = (char *)"-q";
    argv[3] = (char *)"-o";
    argv[4] = (char *)"out";
    argv[5] = (char *)"--top";
    argv[6] = (char *)"A";
    argv[7] = (char *)"two.tern";
    argv[8] = NULL;
    rc = tb_run(argv, dir, &log);
    TB_CHECK(rc == 0);
    free(log);
    p = os_path_join(dir, "out/mine.txt");
    os_write_file(p, "keep\n", 5);
    free(p);
    p = os_path_join(dir, "out");
    TB_CHECK(tb_exists(p, "A.sv") && tb_exists(p, ".tern-build"));
    argv[6] = (char *)"B";
    rc = tb_run(argv, dir, &log);
    TB_CHECK(rc == 0);
    free(log);
    TB_CHECK(!tb_exists(p, "A.sv") && !tb_exists(p, "A.f"));
    TB_CHECK(tb_exists(p, "B.sv") && tb_exists(p, "B.f"));
    TB_CHECK(tb_exists(p, "mine.txt"));
    free(p);
    /* tern prove takes a module with generic arguments, as tern sim does */
    argv[1] = (char *)"prove";
    argv[2] = (char *)"--no-run";
    argv[3] = (char *)"Fifo<u8, 4>";
    argv[4] = (char *)"two.tern";
    argv[5] = NULL;
    rc = tb_run(argv, dir, &log);
    if (rc != 0)
        fprintf(stderr, "tern prove 'Fifo<u8, 4>':\n%s\n", log);
    TB_CHECK(rc == 0);
    free(log);
    TB_CHECK(tb_exists(dir, "build/formal/Fifo_u8_4_3/Fifo_u8_4_3.sby"));
    free(cwd);
    free(tern);
    free(dir);
}

int main(void)
{
    tb_iverilog = os_find_program("iverilog");
    diag_set_format(DIAG_FMT_SHORT);
    tb_targets();
    tb_rewrite();
    tb_formal();
    tb_sby_log();
    tb_vcd();
    tb_cex();
    tb_synth_scripts();
    tb_synth_reports();
    tb_compile_goldens();
    tb_cli();
    arena_release(&tb_arena);
    printf("test_backend: %d checks, %d failed, %d skipped%s\n", tb_checks, tb_fails, tb_skips,
           tb_iverilog ? "" : " (no iverilog)");
    free(tb_iverilog);
    return tb_fails ? 1 : 0;
}
