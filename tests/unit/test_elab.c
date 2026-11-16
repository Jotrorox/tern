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

int main(int argc, char **argv)
{
    if (argc > 1)
        tl_tern = argv[1];
    test_index_range();
    src_free_all();
    intern_free_all();
    if (tl_fails) {
        printf("elab unit tests: %d of %d checks FAILED\n", tl_fails, tl_checks);
        return 1;
    }
    printf("elab unit tests: all %d checks passed\n", tl_checks);
    return 0;
}
