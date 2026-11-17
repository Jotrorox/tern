/* check_impl.h — private model shared by the IR checks (src/check/).
 *
 * check.c builds one ChkMod per module (chk_mod_build): the module's
 * "nodes" (the units that drive signals), the bit ranges each node drives,
 * the signal bits each node reads, and the combinational dependency graph
 * between nodes with its strongly connected components in topological
 * order. The passes read it:
 *
 *   drivers.c  multiple / missing drivers           (chk_drivers)
 *   assign.c   no-latch proof, read-before-assign,
 *              combinational loops                   (chk_assign, chk_loops)
 *   cdc.c      clock-domain crossings + CDC report   (chk_cdc, chk_crossings)
 *   unused.c   unused signals (--unused)             (chk_unused)
 *
 * Node kinds:
 *   CHK_N_ASSIGN   one continuous assignment (IrAssign)
 *   CHK_N_COMB     the assignments of one comb process to one signal
 *   CHK_N_CLOCKED  the assignments of one clocked process to one register
 *                  (or memory)
 *   CHK_N_PROC     the reads of a process that drive nothing (assertions)
 *   CHK_N_INST_OUT one output connection of an instance; it reads the
 *                  parent signals of the inputs that output depends on
 *                  combinationally (the child's port summary)
 *   CHK_N_INST_IN  the input connections of one instance (reads only)
 *   CHK_N_PROP     a property (reads only)
 * A node's reads include the conditions (if/case) that control its
 * assignments. Combinational nodes are ASSIGN, COMB and INST_OUT.
 */
#ifndef TERN_CHECK_IMPL_H
#define TERN_CHECK_IMPL_H

#include "check.h"
#include "../base/arena.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../base/vec.h"

typedef enum ChkNodeKind {
    CHK_N_ASSIGN,
    CHK_N_COMB,
    CHK_N_CLOCKED,
    CHK_N_PROC,
    CHK_N_INST_OUT,
    CHK_N_INST_IN,
    CHK_N_PROP
} ChkNodeKind;

typedef struct ChkRead {
    uint32_t sig;
    uint32_t lo, hi;      /* bits [lo, hi) of sig (memories: [0, 1)) */
    Span loc;             /* the reading expression */
} ChkRead;

typedef struct ChkNode {
    ChkNodeKind kind;
    uint32_t idx;         /* assign / process / instance / property index */
    uint32_t aux;         /* INST_OUT: child port signal id */
    uint32_t sig;         /* the driven signal, or IR_NONE */
    uint32_t domain;      /* CLOCKED/PROC of a clocked process: the process domain */
    uint32_t rd, nrd;     /* reads[rd .. rd + nrd) */
    Span loc;             /* first assignment (or the instance / process) */
} ChkNode;

typedef struct ChkRange {     /* bits [lo, hi) of sig are driven by node */
    uint32_t sig, lo, hi, node;
    Span loc;
} ChkRange;

/* Combinational port dependencies of a module (for its instances): for
 * each port (index into m->ports) the input port indices an output reads
 * combinationally. */
typedef struct ChkSummary {
    uint32_t nports;
    uint32_t *first;      /* nports + 1 offsets into deps */
    uint32_t *deps;
} ChkSummary;

typedef struct ChkCtx ChkCtx;

/* unused.c: one --unused candidate, aggregated over every elaboration of
   the declaration (reported only when unread in all of them) */
typedef struct ChkUnused {
    Span loc;
    const char *msg;      /* arena */
    const char *help;     /* arena or NULL */
    uint32_t seen, unread;
} ChkUnused;

typedef struct ChkMod {
    ChkCtx *cx;
    IrModule *m;
    VEC(ChkNode) nodes;
    VEC(ChkRead) reads;
    VEC(ChkRange) ranges;     /* sorted by (sig, lo) */
    uint32_t *rfirst;         /* nsigs + 1: ranges of sig s are [rfirst[s], rfirst[s+1]) */
    /* combinational graph over nodes: edges[efirst[n] .. efirst[n+1]) are the
       nodes that read bits node n drives */
    uint32_t *efirst;
    uint32_t *edges;
    /* strongly connected components in topological order (drivers first):
       nodes order[sfirst[k] .. sfirst[k+1]) form component k */
    uint32_t nscc;
    uint32_t *order;
    uint32_t *sfirst;
    uint8_t *self_loop;       /* per node: it reads bits it drives itself */
    uint32_t *inst_wire_inst; /* per signal: instance index + 1 whose output drives it, else 0 */
} ChkMod;

struct ChkCtx {
    IrDesign *d;
    const CheckOptions *opt;
    Arena arena;
    Map summaries;            /* IrModule* -> ChkSummary* */
    Map seen;                 /* reported diagnostics (hash of message + span) */
    Map cdc_mods;             /* IrModule* -> ChkCdcMod* (cdc.c: local crossings) */
    Map instantiated;         /* IrModule* -> 1 when some module instantiates it */
    Map unused_idx;           /* unused.c: candidate key -> index + 1 into unused */
    VEC(struct ChkUnused) unused;
    int errors;
};

/* ---- check.c: model and helpers ---- */
int chk_is_comb_node(const ChkNode *n);
/* the ranges of sig: [*first, *end) */
void chk_ranges_of(const ChkMod *cm, uint32_t sig, uint32_t *first, uint32_t *end);
/* the same, starting at the first range that may overlap bit lo or later
   (iterate while ranges.data[f].lo < hi) */
void chk_ranges_from(const ChkMod *cm, uint32_t sig, uint32_t lo, uint32_t *first, uint32_t *end);
int chk_range_overlaps(const ChkRange *r, uint32_t lo, uint32_t hi);
/* Reports an error/warning unless the same message was reported at the
 * same span before (monomorphized copies of a module). Returns 1 if it was
 * reported (so notes/help may follow). */
int chk_error(ChkCtx *cx, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
int chk_warning(ChkCtx *cx, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
/* "`x`", "bits [7:4] of `x`" (into an arena string) */
const char *chk_bits_name(ChkCtx *cx, const IrModule *m, uint32_t sig, uint32_t lo, uint32_t hi);
/* Source text of a span ("" when unknown or too long). */
const char *chk_span_text(ChkCtx *cx, Span s, size_t max);
/* the width covered by the array that starts at bit lo of type t with
   elements of `ew` bits (an lvalue with a dynamic index) */
uint32_t chk_array_extent(const IrType *t, uint32_t lo, uint32_t ew);
/* the signal an expression made only of slices of one signal reads (and
   the bit offset), IR_NONE otherwise */
uint32_t chk_expr_base(const IrExpr *e, uint32_t *lo);
/* first attribute named `name` (interned or not) */
const IrAttr *chk_sig_attr(const IrSignal *s, const char *name);
const char *chk_kind_word(IrSigKind k);   /* "input", "output", "wire", "var", "register", "memory" */

/* ---- bitsets ---- */
#define CHK_WORDS(n) (((n) + 63u) / 64u)
void chk_bits_set_range(uint64_t *b, uint32_t lo, uint32_t hi);
int chk_bits_all(const uint64_t *b, uint32_t lo, uint32_t hi);
/* first clear bit in [lo, hi) and the end of that clear run; 0 if none */
int chk_bits_first_clear(const uint64_t *b, uint32_t lo, uint32_t hi, uint32_t *clo, uint32_t *chi);

/* ---- passes ---- */
void chk_drivers(ChkMod *cm);
void chk_assign(ChkMod *cm);
void chk_loops(ChkMod *cm);
void chk_summary(ChkMod *cm);              /* records the module's port summary (check.c) */
void chk_cdc(ChkMod *cm);
void chk_unused(ChkMod *cm);
void chk_unused_report(ChkCtx *cx);        /* after all modules */
void chk_crossings(ChkCtx *cx);            /* fills d->crossings, prints the report */

#endif /* TERN_CHECK_IMPL_H */
