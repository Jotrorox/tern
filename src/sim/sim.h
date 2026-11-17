/* sim.h — the built-in cycle-based simulator.
 *
 *     SimModel *m = sim_new(design, design->top, seed);   // NULL on error (diag_error)
 *     int a = sim_find(m, "a"), y = sim_find(m, "rx_fifo.count");
 *     sim_reset(m);                    // all top resets asserted for 2 cycles
 *     sim_poke_u64(m, a, 5);
 *     sim_settle(m);                   // combinational settle
 *     sim_step(m, sim_default_domain(m), 1);  // one clock cycle (settles after it)
 *     uint64_t v = sim_peek_u64(m, y);
 *     sim_free(m);
 *
 * Model
 *   - The instance hierarchy below `top` is flattened.  Every signal of every
 *     instance has a handle and a hierarchical Tern path: top signals by their
 *     own path ("tx.valid", "count"), instance signals prefixed with the
 *     instance path ("rx_fifo.count", "a.b.mem").  The SV name works too
 *     ("rx_fifo.count" and "tx_valid").
 *   - Expressions are compiled to a compact instruction list over one value
 *     array (values <= 64 bits in one uint64_t, wider ones as limb arrays).
 *   - Two-phase semantics: sim_settle() evaluates continuous assignments and
 *     comb processes once, in topological order (combinational loops are
 *     rejected by sim_new with the signal names).  On a clock edge every
 *     clocked process of the domains clocked by that edge runs against the
 *     pre-edge values; next values (and memory writes) are committed together
 *     afterwards; the last assignment in program order wins and unassigned
 *     registers hold.  Edges of several clocks at the same instant
 *     (sim_run_ns) are evaluated together, then committed together.
 *   - A cycle of a top domain is one full clock period: the active edge
 *     (rising, or falling for clock<falling>), then the opposite edge (which
 *     clocks child domains declared clock<falling> on the same clock).
 *   - Clocks and resets of the top are driven by the harness (sim_step,
 *     sim_run_ns, sim_reset); sim_poke on them fails.
 *   - Reset: a domain's reset (sync/async, active-high/low per IRT_RESET) is
 *     sampled at each edge; while it is active registers with an init value
 *     get it and registers without one update normally.  Async resets also
 *     act during settle, without an edge.  Power-on: registers with an init
 *     value start at it, registers without one (and memories without
 *     contents) start with pseudo-random values derived from the seed and
 *     the signal's path (deterministic per seed).
 *   - Immediate assertions run when their statement executes (comb: during
 *     settle; clocked: at the edge, pre-edge values).  They are suppressed
 *     while their domain is in reset (clocked) or while sim_reset() runs
 *     (comb).  Failures go to the fail handler (default: print to stderr).
 *   - Properties are checked at every active edge of their domain with
 *     pre-edge (sampled) values, LTL-style: an assert/assume property is one
 *     obligation that starts at the first edge out of reset (write `always`
 *     to check every cycle); a cover property starts an attempt every cycle
 *     and counts the attempts that complete non-vacuously.  Attempts are
 *     discarded while the domain is in reset.  Unbounded `eventually` /
 *     `##[n:$]` obligations still open at sim_finish() are reported as
 *     pending.  past(x, n) reads a history buffer (0 before n cycles exist);
 *     the history restarts (reads 0) while the domain is in reset, so a
 *     property never compares with values sampled during reset.
 *   - Coverage: every block entry (process bodies, then/else branches, case
 *     arms) is counted; sim_coverage() lists statements, branches, arms and
 *     cover properties keyed by span, summed over instances.
 */
#ifndef TERN_SIM_H
#define TERN_SIM_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../ir/ir.h"

struct StrBuf;

typedef struct SimModel SimModel;

/* status codes */
#define SIM_OK 0
#define SIM_ERR_HANDLE (-1)        /* invalid handle / domain */
#define SIM_ERR_NOT_WRITABLE (-2)  /* not a top input / #[test_port], or a clock/reset */
#define SIM_ERR_KIND (-3)          /* memory handle where a signal is needed or vice versa */
#define SIM_ERR_RANGE (-4)         /* memory address out of range */
#define SIM_ERR_IO (-5)

/* ---------------------------------------------------------- lifecycle -- */

/* Build a model of `top` (normally d->top).  Returns NULL after reporting
 * errors with diag_error() (combinational loops). The design must outlive
 * the model. */
SimModel *sim_new(const IrDesign *d, const IrModule *top, uint64_t seed);
void sim_free(SimModel *m);

/* ------------------------------------------------------------ signals -- */

int sim_find(SimModel *m, const char *path);    /* handle or -1 */
/* Handles of all signals whose path starts with `prefix` + "." or "[" (the
 * flattened fields of a bundle port "tx" -> "tx.valid", "tx.data", ...; the
 * elements of an array of bundles "out" -> "out[0].valid", ...), in
 * declaration order. Returns the total count (may exceed max). */
int sim_find_prefix(SimModel *m, const char *prefix, int *out, int max);
int sim_signal_count(const SimModel *m);         /* handles are 0..count-1 */
const char *sim_signal_path(const SimModel *m, int h);    /* "rx_fifo.count" */
const char *sim_signal_name(const SimModel *m, int h);    /* SV name in its module ("count") */
const char *sim_signal_instance(const SimModel *m, int h); /* instance path ("" = top) */
const IrSignal *sim_signal_ir(const SimModel *m, int h);
const IrType *sim_type(const SimModel *m, int h);
uint32_t sim_width(const SimModel *m, int h);    /* 0 for invalid handles */
int sim_is_memory(const SimModel *m, int h);
uint32_t sim_mem_depth(const SimModel *m, int h);
int sim_is_writable(const SimModel *m, int h);

/* Values.  sim_poke zero-extends/truncates to the signal width.  sim_peek
 * writes IR_LIMBS(width) limbs to out->w and sets out->width; peeks return
 * the current values (call sim_settle first after pokes). */
int sim_poke(SimModel *m, int h, const IrBits *v);
int sim_poke_u64(SimModel *m, int h, uint64_t v);
int sim_peek(SimModel *m, int h, IrBits *out);
IrBits sim_peek_bits(SimModel *m, int h, Arena *a);     /* width 0 on error */
uint64_t sim_peek_u64(SimModel *m, int h);              /* low 64 bits */
int sim_mem_peek(SimModel *m, int h, uint64_t addr, IrBits *out);
int sim_mem_poke(SimModel *m, int h, uint64_t addr, const IrBits *v);

/* ------------------------------------------------------------ running -- */

void sim_settle(SimModel *m);
int sim_domain_count(const SimModel *m);                /* domains of the top */
int sim_default_domain(const SimModel *m);              /* -1 if none */
int sim_find_domain(const SimModel *m, const char *name);  /* "" = default; -1 if absent */
const char *sim_domain_name(const SimModel *m, int domain);
uint64_t sim_domain_hz(const SimModel *m, int domain);  /* effective (100 MHz if unknown) */
void sim_step(SimModel *m, uint32_t domain, uint32_t n);  /* n cycles of a top domain */
void sim_run_ns(SimModel *m, uint64_t ns);              /* all top clocks by their hz */
void sim_reset(SimModel *m);                            /* assert top resets 2 cycles, release */
uint64_t sim_cycle(const SimModel *m, uint32_t domain); /* active edges so far */
uint64_t sim_time_ps(const SimModel *m);

/* ----------------------------------------------------- verification -- */

typedef enum SimFailKind {
    SIM_FAIL_ASSERT,     /* immediate assert failed */
    SIM_FAIL_ASSUME,     /* immediate assume or assume property violated */
    SIM_FAIL_PROPERTY,   /* assert property failed */
    SIM_FAIL_PENDING     /* unbounded obligation still open at sim_finish() */
} SimFailKind;

typedef struct SimFailure {
    SimFailKind kind;
    const char *what;      /* one-line description: "assertion failed: push into full fifo" */
    const char *msg;       /* assertion message, or NULL */
    const char *property;  /* property name for property failures, else NULL */
    Span loc;
    uint64_t cycle;        /* cycles of the domain completed when the values were sampled,
                              i.e. the 0-based index of the edge (comb: of the default top
                              domain) */
    uint64_t start_cycle;  /* properties: cycle the failing attempt started */
    const char *inst_path; /* instance path, "" for the top */
    uint64_t time_ps;
} SimFailure;

typedef void (*SimFailFn)(void *user, const SimFailure *f);
void sim_set_fail_handler(SimModel *m, SimFailFn fn, void *user);
uint32_t sim_fail_count(const SimModel *m);
/* End of test: reports pending unbounded property obligations; returns how
 * many were reported. */
uint32_t sim_finish(SimModel *m);

/* ---------------------------------------------------------- coverage -- */

typedef enum SimCovKind {
    SIM_COV_STMT,        /* a statement (line coverage) */
    SIM_COV_THEN,        /* if: then branch taken */
    SIM_COV_ELSE,        /* if: else branch taken (also when there is no else) */
    SIM_COV_ARM,         /* case arm `arm` taken */
    SIM_COV_COVER,       /* immediate `cover` statement held */
    SIM_COV_PROPERTY     /* cover property hits */
} SimCovKind;

typedef struct SimCovPoint {
    SimCovKind kind;
    Span loc;            /* the statement / if / case / property */
    uint32_t arm;        /* SIM_COV_ARM: arm index */
    const char *name;    /* property name or NULL */
    uint64_t count;
} SimCovPoint;

/* Coverage points sorted by (file, line, col, kind, arm), merged over
 * instances and modules with the same span.  The array stays valid until the
 * next call or sim_free. */
size_t sim_coverage(SimModel *m, const SimCovPoint **out);
void sim_coverage_reset(SimModel *m);

/* ---------------------------------------------------------------- VCD -- */

/* Dump every signal (memories excepted) with a scope per instance; timescale
 * 1ps.  Values are written at the current time and after every step; the
 * file is flushed after each sim_step/sim_run_ns/sim_settle call. */
int sim_vcd_open(SimModel *m, const char *path);
void sim_vcd_close(SimModel *m);

/* ============================================================ bitvec == */
/* Little-endian uint64_t limb arrays of `w` bits (IR_LIMBS(w) limbs; bits >=
 * w are zero on input and kept zero on output).  Unless noted, the result d
 * may alias any operand.  Exposed for the simulator and its tests. */

void bv_zero(uint64_t *d, uint32_t w);
void bv_copy(uint64_t *d, const uint64_t *a, uint32_t w);
int bv_is_zero(const uint64_t *a, uint32_t w);
int bv_eq(const uint64_t *a, const uint64_t *b, uint32_t w);
void bv_not(uint64_t *d, const uint64_t *a, uint32_t w);
void bv_neg(uint64_t *d, const uint64_t *a, uint32_t w);
void bv_and(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w);
void bv_or(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w);
void bv_xor(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w);
void bv_add(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w);
void bv_sub(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w);
/* mul/divmod need scratch space of 4*IR_LIMBS(w) limbs; d must not alias
 * scratch.  divmod: q and/or r may be NULL; x/0 == 0, x%0 == x; signed is
 * truncating (C semantics) and wraps on MIN / -1. */
void bv_mul(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w, uint64_t *scratch);
void bv_divmod(uint64_t *q, uint64_t *r, const uint64_t *a, const uint64_t *b, uint32_t w,
               int is_signed, uint64_t *scratch);
/* shifts by an amount (saturating: >= w shifts everything out) */
void bv_shl(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt);
void bv_shr(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt);
void bv_ashr(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt);
int bv_cmp(const uint64_t *a, const uint64_t *b, uint32_t w, int is_signed);   /* -1, 0, 1 */
int bv_red_and(const uint64_t *a, uint32_t w);
int bv_red_or(const uint64_t *a, uint32_t w);
int bv_red_xor(const uint64_t *a, uint32_t w);
int bv_get_bit(const uint64_t *a, uint32_t bit);
/* d (dw bits) = a[lo +: dw] (bits beyond aw read as 0); d must not alias a */
void bv_extract(uint64_t *d, uint32_t dw, const uint64_t *a, uint32_t aw, uint64_t lo);
uint64_t bv_get64(const uint64_t *a, uint32_t aw, uint64_t lo, uint32_t n); /* n <= 64 bits */
/* d[lo +: aw] = a, bits at >= dw dropped (d keeps its other bits) */
void bv_deposit(uint64_t *d, uint32_t dw, uint64_t lo, const uint64_t *a, uint32_t aw);
/* d (dw bits) = a (aw bits) zero- or sign-extended / truncated; may alias */
void bv_resize(uint64_t *d, uint32_t dw, const uint64_t *a, uint32_t aw, int sign_extend);
/* an unsigned amount operand as a saturated uint64 */
uint64_t bv_to_u64_sat(const uint64_t *a, uint32_t w);
void bv_print_bin(struct StrBuf *out, const uint64_t *a, uint32_t w, int strip_zeros);
void bv_print_hex(struct StrBuf *out, const uint64_t *a, uint32_t w);

#endif /* TERN_SIM_H */
