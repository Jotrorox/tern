/* test_elab.c — front-end semantics checked through the simulator.
 *
 *     cc -o build/test_elab tests/unit/test_elab.c && build/test_elab [tern]
 *
 * Each test elaborates a design from tests/fixtures/elab/ with
 * `tern ir --json --top M file` (the tern binary is ./tern, or the first
 * argument), reads the design back with ir_from_json() and drives it with the
 * built-in simulator (src/sim/sim.h): register-file bus transactions,
 * out-of-range dynamic indices, dynamic indices into arrays of bundles and
 * `return` inside branches of fns inlined on signals.  Run from the
 * repository root (make test does). */
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

static int tl_checks, tl_fails;
static const char *tl_tern = "./tern";

static void tl_check(int ok, const char *expr, const char *file, int line)
{
    tl_checks++;
    if (!ok) {
        tl_fails++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

#define TL_CHECK(c) tl_check((c) ? 1 : 0, #c, __FILE__, __LINE__)
#define TL_EQ(a, b)                                                                                   \
    do {                                                                                              \
        uint64_t tl_a_ = (uint64_t)(a), tl_b_ = (uint64_t)(b);                                        \
        if (tl_a_ != tl_b_)                                                                           \
            fprintf(stderr, "  %s = 0x%llx, expected 0x%llx\n", #a, (unsigned long long)tl_a_,          \
                    (unsigned long long)tl_b_);                                                       \
        tl_check(tl_a_ == tl_b_, #a " == " #b, __FILE__, __LINE__);                                   \
    } while (0)

typedef struct TlDesign {
    Arena arena;
    IrDesign *d;
    SimModel *m;
} TlDesign;

/* Elaborates `top` from tests/fixtures/elab/<file> and builds a simulator. */
static int tl_open(TlDesign *t, const char *file, const char *top)
{
    char path[512];
    char *argv[8];
    OsSpawnOpts o;
    OsRunResult r;
    int rc;
    memset(t, 0, sizeof *t);
    snprintf(path, sizeof path, "tests/fixtures/elab/%s", file);
    argv[0] = (char *)tl_tern;
    argv[1] = (char *)"ir";
    argv[2] = (char *)"--json";
    argv[3] = (char *)"--top";
    argv[4] = (char *)top;
    argv[5] = path;
    argv[6] = NULL;
    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    memset(&r, 0, sizeof r);
    rc = os_run(argv, &o, &r);
    if (rc != 0 || !r.out || !r.out_len) {
        fprintf(stderr, "test_elab: `%s ir --json --top %s %s` failed (exit %d):\n%s\n", tl_tern, top, path, rc,
                r.err ? r.err : "");
        os_run_result_free(&r);
        tl_check(0, "elaboration", file, 0);
        return 0;
    }
    t->d = ir_from_json(&t->arena, r.out, r.out_len);
    os_run_result_free(&r);
    if (!t->d || !t->d->top) {
        tl_check(0, "ir_from_json", file, 0);
        return 0;
    }
    t->m = sim_new(t->d, t->d->top, 1);
    if (!t->m) {
        tl_check(0, "sim_new", file, 0);
        return 0;
    }
    return 1;
}

static void tl_close(TlDesign *t)
{
    if (t->m)
        sim_free(t->m);
    arena_release(&t->arena);
}

static int tl_h(TlDesign *t, const char *path)
{
    int h = sim_find(t->m, path);
    if (h < 0) {
        fprintf(stderr, "test_elab: no signal `%s`\n", path);
        tl_check(0, "sim_find", path, 0);
    }
    return h;
}

static void tl_poke(TlDesign *t, const char *path, uint64_t v)
{
    int h = tl_h(t, path);
    if (h >= 0 && sim_poke_u64(t->m, h, v) != SIM_OK) {
        fprintf(stderr, "test_elab: cannot poke `%s`\n", path);
        tl_check(0, "sim_poke", path, 0);
    }
}

static uint64_t tl_peek(TlDesign *t, const char *path)
{
    int h = tl_h(t, path);
    return h >= 0 ? sim_peek_u64(t->m, h) : 0xDEADDEADull;
}

static void tl_step(TlDesign *t, uint32_t n)
{
    sim_step(t->m, (uint32_t)sim_default_domain(t->m), n);
}

/* ------------------------------------------------------- index ranges -- */

static void test_index_range(void)
{
    TlDesign t;
    uint64_t i;
    if (!tl_open(&t, "index_range.tern", "IndexRange"))
        goto done;
    sim_reset(t.m);
    tl_poke(&t, "we", 0);
    tl_poke(&t, "bidx", 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "tag"), 0xA5);
    TL_EQ(tl_peek(&t, "s"), 0xA5000000u);
    /* write every slot, then try index 3 (past the end) */
    for (i = 0; i < 4; i++) {
        tl_poke(&t, "we", 1);
        tl_poke(&t, "widx", i);
        tl_poke(&t, "wdata", 0x10 + i);
        tl_step(&t, 1);
    }
    tl_poke(&t, "we", 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "tag"), 0xA5); /* the write at index 3 did not reach `tag` */
    for (i = 0; i < 3; i++) {
        tl_poke(&t, "ridx", i);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "rdata"), 0x10 + i);
    }
    tl_poke(&t, "ridx", 3);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "rdata"), 0); /* out of range reads 0 */
    /* bit index into uint<5> with a uint<3> index */
    TL_EQ(tl_peek(&t, "flags"), 0x16 | 0x01);
    for (i = 5; i < 8; i++) {
        tl_poke(&t, "we", 1);
        tl_poke(&t, "bidx", i);
        tl_step(&t, 1);
        TL_EQ(tl_peek(&t, "bit"), 0);
    }
    TL_EQ(tl_peek(&t, "flags"), 0x17);
    /* comb var: v[widx] = 0xEE skipped for widx 3 */
    tl_poke(&t, "widx", 3);
    tl_poke(&t, "ridx", 2);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "vsel"), 3);
    tl_poke(&t, "widx", 2);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "vsel"), 0xEE);
done:
    tl_close(&t);
}

/* ------------------------------------------------ arrays of bundles -- */

static void test_bundle_index(void)
{
    TlDesign t;
    uint64_t s, o;
    char p[64];
    if (!tl_open(&t, "bundle_index.tern", "BundleIndex"))
        goto done;
    sim_reset(t.m);
    for (s = 0; s < 3; s++) {
        snprintf(p, sizeof p, "ins[%u].valid", (unsigned)s);
        tl_poke(&t, p, s != 1);
        snprintf(p, sizeof p, "ins[%u].data", (unsigned)s);
        tl_poke(&t, p, ((0x30 + s) << 1) | (s == 2));
    }
    tl_poke(&t, "picked.ready", 1);
    for (s = 0; s < 4; s++) {
        tl_poke(&t, "sel", s);
        for (o = 0; o < 4; o++) {
            uint64_t k;
            tl_poke(&t, "osel", o);
            sim_settle(t.m);
            TL_EQ(tl_peek(&t, "picked.valid"), s < 3 && s != 1);
            TL_EQ(tl_peek(&t, "picked.data"), s < 3 ? (((0x30 + s) << 1) | (s == 2)) : 0);
            TL_EQ(tl_peek(&t, "data"), s < 3 ? 0x30 + s : 0);
            for (k = 0; k < 3; k++) {
                snprintf(p, sizeof p, "ins[%u].ready", (unsigned)k);
                TL_EQ(tl_peek(&t, p), k == s);
                snprintf(p, sizeof p, "outs[%u].valid", (unsigned)k);
                TL_EQ(tl_peek(&t, p), k == o);
                snprintf(p, sizeof p, "outs[%u].data", (unsigned)k);
                TL_EQ(tl_peek(&t, p), k == o && s < 3 ? 0x30 + s : 0);
            }
        }
    }
    tl_poke(&t, "sel", 0);
    tl_step(&t, 3);
    tl_poke(&t, "sel", 1); /* not valid */
    tl_step(&t, 2);
    TL_EQ(tl_peek(&t, "count"), 3);
done:
    tl_close(&t);
}

/* ------------------------------------------------ return in branches -- */

static uint64_t tl_ref_first(uint64_t x)
{
    uint64_t i;
    for (i = 0; i < 8; i++)
        if (x >> i & 1)
            return i;
    return 8;
}

static uint64_t tl_ref_class(uint64_t x)
{
    if (x == 0)
        return 0;
    if (x < 16)
        return ((x + 100) & 0xFF) == 105 ? 3 : 1;
    return x == 0xFF ? 3 : 2;
}

static void test_fn_return(void)
{
    TlDesign t;
    uint64_t x;
    if (!tl_open(&t, "fn_return.tern", "FnReturn"))
        goto done;
    for (x = 0; x < 256; x++) {
        uint64_t a = (x * 37) & 0xFF, b = (x * 91 + 7) & 0xFF;
        tl_poke(&t, "x", x);
        tl_poke(&t, "a", a);
        tl_poke(&t, "b", b);
        tl_poke(&t, "c", x & 1);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "first"), tl_ref_first(x));
        TL_EQ(tl_peek(&t, "class"), tl_ref_class(x));
        TL_EQ(tl_peek(&t, "pick"), (x & 1) ? a : b);
        TL_EQ(tl_peek(&t, "sum"), a + b > 255 ? 255 : a + b);
    }
done:
    tl_close(&t);
}

/* ----------------------------------------------------- register files -- */

/* AXI4-Lite write; returns the B response (0 OKAY, 2 SLVERR), or 99 on timeout */
static uint64_t tl_axi_write(TlDesign *t, uint64_t addr, uint64_t data, uint64_t strb)
{
    int aw_done = 0, w_done = 0, i;
    tl_poke(t, "bus.aw.valid", 1);
    tl_poke(t, "bus.aw.data", addr);
    tl_poke(t, "bus.w.valid", 1);
    tl_poke(t, "bus.w.data", (data << 4) | strb);
    tl_poke(t, "bus.b.ready", 1);
    for (i = 0; i < 50; i++) {
        uint64_t b;
        sim_settle(t->m);
        if (!aw_done && tl_peek(t, "bus.aw.ready"))
            aw_done = 2;
        if (!w_done && tl_peek(t, "bus.w.ready"))
            w_done = 2;
        b = tl_peek(t, "bus.b.valid");
        if (b) {
            uint64_t resp = tl_peek(t, "bus.b.data");
            tl_step(t, 1);
            return resp;
        }
        tl_step(t, 1);
        if (aw_done == 2) {
            aw_done = 1;
            tl_poke(t, "bus.aw.valid", 0);
        }
        if (w_done == 2) {
            w_done = 1;
            tl_poke(t, "bus.w.valid", 0);
        }
    }
    return 99;
}

/* AXI4-Lite read; *resp gets the response */
static uint64_t tl_axi_read(TlDesign *t, uint64_t addr, uint64_t *resp)
{
    int ar_done = 0, i;
    tl_poke(t, "bus.ar.valid", 1);
    tl_poke(t, "bus.ar.data", addr);
    tl_poke(t, "bus.r.ready", 1);
    for (i = 0; i < 50; i++) {
        sim_settle(t->m);
        if (!ar_done && tl_peek(t, "bus.ar.ready"))
            ar_done = 2;
        if (tl_peek(t, "bus.r.valid")) {
            uint64_t r = tl_peek(t, "bus.r.data");
            tl_step(t, 1);
            *resp = r & 3;
            return r >> 2;
        }
        tl_step(t, 1);
        if (ar_done == 2) {
            ar_done = 1;
            tl_poke(t, "bus.ar.valid", 0);
        }
    }
    *resp = 99;
    return 0;
}

static void test_regfile_axi(void)
{
    TlDesign t;
    uint64_t resp, v;
    if (!tl_open(&t, "regfile.tern", "RegsAxi"))
        goto done;
    tl_poke(&t, "bus.aw.valid", 0);
    tl_poke(&t, "bus.w.valid", 0);
    tl_poke(&t, "bus.ar.valid", 0);
    tl_poke(&t, "bus.b.ready", 1);
    tl_poke(&t, "bus.r.ready", 1);
    tl_poke(&t, "busy", 1);
    tl_poke(&t, "irq_in", 0);
    tl_poke(&t, "go_done", 0);
    tl_poke(&t, "txq.ready", 0);
    sim_reset(&t.m[0]);
    sim_settle(t.m);
    /* reset values: ctrl = 0x305 (enable, mode Slow... level 3), scratch */
    TL_EQ(tl_peek(&t, "enable"), 1);
    TL_EQ(tl_peek(&t, "mode"), 2);
    TL_EQ(tl_peek(&t, "level"), 3);
    TL_EQ(tl_peek(&t, "scratch"), 0x12345678);
    v = tl_axi_read(&t, 0x00, &resp);
    TL_EQ(resp, 0);
    TL_EQ(v, 0x305);
    /* rw with byte strobes */
    TL_EQ(tl_axi_write(&t, 0x08, 0xDEADBEEF, 0xF), 0);
    TL_EQ(tl_peek(&t, "scratch"), 0xDEADBEEF);
    TL_EQ(tl_axi_write(&t, 0x08, 0x11223344, 0x4), 0);
    TL_EQ(tl_peek(&t, "scratch"), 0xDE22BEEF);
    v = tl_axi_read(&t, 0x08, &resp);
    TL_EQ(v, 0xDE22BEEF);
    /* fields with a gap: bits outside the fields stay zero */
    TL_EQ(tl_axi_write(&t, 0x00, 0xFFFFFFFF, 0xF), 0);
    TL_EQ(tl_axi_read(&t, 0x00, &resp), 0xF07);
    TL_EQ(tl_peek(&t, "mode"), 3);
    TL_EQ(tl_peek(&t, "level"), 15);
    TL_EQ(tl_axi_write(&t, 0x00, 0x00000002, 0x1), 0); /* byte 0 only: level keeps 15 */
    TL_EQ(tl_peek(&t, "enable"), 0);
    TL_EQ(tl_peek(&t, "mode"), 1);
    TL_EQ(tl_peek(&t, "level"), 15);
    /* ro: reads the input in layout, writes ignored with OKAY */
    v = tl_axi_read(&t, 0x04, &resp);
    TL_EQ(resp, 0);
    TL_EQ(v, 0x5A01);
    TL_EQ(tl_axi_write(&t, 0x04, 0, 0xF), 0);
    /* w1c: hardware sets, software clears written ones */
    tl_poke(&t, "irq_in", 0x5);
    tl_step(&t, 1);
    tl_poke(&t, "irq_in", 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "irq"), 0x5);
    TL_EQ(tl_axi_read(&t, 0x0C, &resp), 0x5);
    TL_EQ(tl_axi_write(&t, 0x0C, 0x4, 0xF), 0);
    TL_EQ(tl_peek(&t, "irq"), 0x1);
    /* rw1s: software sets, hardware clears */
    TL_EQ(tl_axi_write(&t, 0x10, 0x2, 0xF), 0);
    TL_EQ(tl_peek(&t, "go"), 0x2);
    TL_EQ(tl_axi_write(&t, 0x10, 0x1, 0xF), 0);
    TL_EQ(tl_peek(&t, "go"), 0x3);
    tl_poke(&t, "go_done", 0x2);
    tl_step(&t, 1);
    tl_poke(&t, "go_done", 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "go"), 0x1);
    /* wo: output follows writes, reads give 0 */
    TL_EQ(tl_axi_write(&t, 0x14, 0xABCD, 0x3), 0);
    TL_EQ(tl_peek(&t, "key"), 0xABCD);
    TL_EQ(tl_axi_read(&t, 0x14, &resp), 0);
    TL_EQ(resp, 0);
    /* stream register: one beat per write; a second write while full is SLVERR */
    TL_EQ(tl_axi_write(&t, 0x18, 0x41, 0x1), 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "txq.valid"), 1);
    TL_EQ(tl_peek(&t, "txq.data"), 0x41);
    TL_EQ(tl_axi_write(&t, 0x18, 0x42, 0x1), 2);
    TL_EQ(tl_peek(&t, "txq.data"), 0x41);
    tl_poke(&t, "txq.ready", 1);
    tl_step(&t, 1);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "txq.valid"), 0);
    tl_poke(&t, "txq.ready", 0);
    TL_EQ(tl_axi_write(&t, 0x18, 0x43, 0x1), 0);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "txq.valid"), 1);
    TL_EQ(tl_peek(&t, "txq.data"), 0x43);
    tl_poke(&t, "txq.ready", 1);
    tl_step(&t, 1);
    sim_settle(t.m);
    TL_EQ(tl_peek(&t, "txq.valid"), 0);
    /* a register wider than the bus: two words */
    TL_EQ(tl_axi_write(&t, 0x20, 0x89ABCDEF, 0xF), 0);
    TL_EQ(tl_axi_write(&t, 0x24, 0x76543210, 0xF), 0);
    TL_EQ(tl_peek(&t, "stamp"), 0x3210ull << 32 | 0x89ABCDEFull);
    TL_EQ(tl_axi_read(&t, 0x24, &resp), 0x3210);
    /* unmapped offsets */
    tl_axi_read(&t, 0x40, &resp);
    TL_EQ(resp, 2);
    TL_EQ(tl_axi_write(&t, 0x1C, 1, 0xF), 2);
done:
    tl_close(&t);
}

static void test_regfile_apb_wb(void)
{
    TlDesign t;
    if (tl_open(&t, "regfile.tern", "RegsApb")) {
        tl_poke(&t, "b", 0x77);
        tl_poke(&t, "c_set", 0);
        tl_poke(&t, "bus.pprot", 0);
        tl_poke(&t, "bus.psel", 0);
        tl_poke(&t, "bus.penable", 0);
        sim_reset(t.m);
        /* write a = 0x5C */
        tl_poke(&t, "bus.psel", 1);
        tl_poke(&t, "bus.pwrite", 1);
        tl_poke(&t, "bus.paddr", 0x0);
        tl_poke(&t, "bus.pwdata", 0x5C);
        tl_poke(&t, "bus.pstrb", 0xF);
        tl_step(&t, 1);
        tl_poke(&t, "bus.penable", 1);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.pready"), 1);
        TL_EQ(tl_peek(&t, "bus.pslverr"), 0);
        tl_step(&t, 1);
        tl_poke(&t, "bus.psel", 0);
        tl_poke(&t, "bus.penable", 0);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "a"), 0x5C);
        /* read b (ro) */
        tl_poke(&t, "bus.psel", 1);
        tl_poke(&t, "bus.pwrite", 0);
        tl_poke(&t, "bus.paddr", 0x4);
        tl_step(&t, 1);
        tl_poke(&t, "bus.penable", 1);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.prdata"), 0x77);
        TL_EQ(tl_peek(&t, "bus.pslverr"), 0);
        /* unmapped */
        tl_poke(&t, "bus.paddr", 0x100);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.pslverr"), 1);
        tl_poke(&t, "bus.psel", 0);
        tl_poke(&t, "bus.penable", 0);
        /* w1c c */
        tl_poke(&t, "c_set", 0x81);
        tl_step(&t, 1);
        tl_poke(&t, "c_set", 0);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "c"), 0x81);
    }
    tl_close(&t);
    if (tl_open(&t, "regfile.tern", "RegsWb")) {
        tl_poke(&t, "b", 0x66);
        tl_poke(&t, "bus.cyc", 0);
        tl_poke(&t, "bus.stb", 0);
        sim_reset(t.m);
        tl_poke(&t, "bus.cyc", 1);
        tl_poke(&t, "bus.stb", 1);
        tl_poke(&t, "bus.we", 1);
        tl_poke(&t, "bus.adr", 0x0);
        tl_poke(&t, "bus.dat_w", 0x3E);
        tl_poke(&t, "bus.sel", 0xF);
        tl_step(&t, 1);
        tl_poke(&t, "bus.stb", 0);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.ack"), 1);
        TL_EQ(tl_peek(&t, "a"), 0x3E);
        tl_step(&t, 1);
        tl_poke(&t, "bus.stb", 1);
        tl_poke(&t, "bus.we", 0);
        tl_poke(&t, "bus.adr", 0x4);
        tl_step(&t, 1);
        tl_poke(&t, "bus.stb", 0);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.ack"), 1);
        TL_EQ(tl_peek(&t, "bus.dat_r"), 0x66);
        tl_step(&t, 1);
        tl_poke(&t, "bus.stb", 1);
        tl_poke(&t, "bus.adr", 0x40);
        tl_step(&t, 1);
        sim_settle(t.m);
        TL_EQ(tl_peek(&t, "bus.err"), 1);
        TL_EQ(tl_peek(&t, "bus.ack"), 0);
    }
    tl_close(&t);
}

int main(int argc, char **argv)
{
    if (argc > 1)
        tl_tern = argv[1];
    test_regfile_axi();
    test_regfile_apb_wb();
    test_index_range();
    test_bundle_index();
    test_fn_return();
    src_free_all();
    intern_free_all();
    if (tl_fails) {
        printf("elab unit tests: %d of %d checks FAILED\n", tl_fails, tl_checks);
        return 1;
    }
    printf("elab unit tests: all %d checks passed\n", tl_checks);
    return 0;
}
