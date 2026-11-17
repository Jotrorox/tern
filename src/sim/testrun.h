/* testrun.h — the `test` block interpreter (spec §11) on the built-in
 * simulator, shared by `tern test` (src/sim/cmd_test.c) and the `tern sim`
 * REPL (src/sim/cmd_sim.c).
 *
 *     TestRunOptions o = {0};
 *     o.seed = testrun_test_seed(base_seed, item->u.test.name);
 *     StrBuf report = {0};
 *     int ok = testrun_run(dr, &di, &o, cov, &report);   // 1 passed, 0 failed
 *
 * A test body runs in the const evaluator (src/elab/consteval.c) with
 * ElabHooks: `sim M<..>(inputs)` elaborates M into its own IrDesign and
 * returns a simulation handle (VAL_OPAQUE); `rand<T>()` draws from a per-test
 * RNG; `log(fmt, ..)` appends to the report; `expect`/`assert` failures are
 * recorded with the operand values and the cycle. Assertions and properties
 * of the design are checked by the simulator every cycle; a failing
 * assertion, property or `assert` stops the test, `expect` and assumption
 * violations are recorded and the test continues.
 *
 * Handles (see the TrNode comment in testrun.c): `dut.sig` reads (after a
 * combinational settle), `dut.sig = v` drives top inputs and #[test_port]s,
 * `dut.bundle = Stream { .. }` drives every given field, `dut.inst.sig`
 * reads internal signals, `dut.mem[i]` reads memories, methods reset(),
 * step(n), step_until(cond, max: n), settle(), run(ns: n), dump(file),
 * cycle(); `dut.<domain>.step(n)` steps one domain.
 *
 * The report is plain text: one entry per failure / log line / diagnostic,
 * each indented by four spaces (diagnostics are rendered in the current
 * diag format, without colour).
 */
#ifndef TERN_TESTRUN_H
#define TERN_TESTRUN_H

#include "../base/common.h"
#include "../base/str.h"
#include "../base/map.h"
#include "../base/vec.h"
#include "../driver.h"
#include "sim.h"

typedef struct TestRunOptions {
    uint64_t seed;            /* this test's seed (testrun_test_seed) */
    const char *waves_path;   /* record a VCD of the (first) simulated design here, or NULL */
    int cover;                /* collect coverage into the TestCov */
    uint64_t step_limit;      /* interpreter step limit (0: 10^8) */
} TestRunOptions;

/* Coverage merged over designs, tests and worker processes. Points are
 * keyed by (file path, line, col, kind, arm). */
typedef struct TestCovPoint {
    SimCovKind kind;
    const char *file;         /* interned source path */
    uint32_t line, col, arm;
    const char *name;         /* cover property name (interned) or NULL */
    uint64_t count;
} TestCovPoint;

typedef struct TestCov {
    VEC(TestCovPoint) pts;
    Map index;                /* key hash -> index + 1 */
} TestCov;

void testcov_add(TestCov *c, const TestCovPoint *p);   /* merges counts */
void testcov_free(TestCov *c);
/* Line / branch / cover-property report (§11.6), per file. */
void testcov_report(TestCov *c, StrBuf *out);
/* One line per point: "kind\tline\tcol\tarm\tcount\tname\tfile" (worker protocol) */
void testcov_serialize(const TestCov *c, StrBuf *out);
int testcov_parse_line(TestCov *c, const char *line);  /* 1 if it was a coverage line */

/* Per-test seed derived from the run's base seed and the test name. */
uint64_t testrun_test_seed(uint64_t base, const char *name);

/* Runs one test; returns 1 when it passed. Failures, logs and diagnostics go
 * to `report`. `cov` may be NULL. */
int testrun_run(Driver *dr, const DriverItem *t, const TestRunOptions *o, TestCov *cov, StrBuf *report);

/* ------------------------------------------------------- REPL support -- */

/* An interpreter session outside a test block (tern sim): hooks installed,
 * one persistent environment in which statements run. */
typedef struct TestSession TestSession;

TestSession *testrun_session_new(Driver *dr, FileUnit *file, const TestRunOptions *o, StrBuf *report);
void testrun_session_free(TestSession *ts);
Elab *testrun_session_elab(TestSession *ts);
struct Env *testrun_session_env(TestSession *ts);
/* Creates `name = sim <module>()` in the session (module elaborated with
 * default generics). Returns 0 after reporting errors. */
int testrun_session_sim(TestSession *ts, const char *name, Item *module, FileUnit *file, const char *vcd);
/* Runs statements in the session environment; when the last one is an
 * expression statement its value is printed to `out`. Returns 0 if the
 * statements reported errors or failed a check. */
int testrun_session_exec(TestSession *ts, Block *b, StrBuf *out);
/* Lists the signals of the session's handle `name` (path, width, kind). */
void testrun_session_signals(TestSession *ts, const char *name, StrBuf *out);
/* Clears the failure state after a failed statement. */
void testrun_session_clear(TestSession *ts);

#endif /* TERN_TESTRUN_H */
