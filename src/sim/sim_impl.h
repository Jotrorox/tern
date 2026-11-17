/* sim_impl.h — simulator internals shared by netlist.c (build/compile),
 * eval.c (execution, properties, coverage, API) and vcd.c.  Not part of the
 * public interface (see sim.h). */
#ifndef TERN_SIM_IMPL_H
#define TERN_SIM_IMPL_H

#include "sim.h"
#include "../base/str.h"
#include "../base/map.h"
#include "../base/vec.h"

/* ------------------------------------------------------- instructions -- */

/* Operands d/a/b/c are offsets (in limbs) into SimModel.v.  While compiling,
 * offsets into the constant pool / temporaries / misc region carry a tag in
 * the high bits and are relocated once the layout is final. */
#define SIM_TAG_CONST 0x80000000u
#define SIM_TAG_TEMP 0x40000000u
#define SIM_TAG_MISC 0x20000000u
#define SIM_TAG_MASK 0xE0000000u

typedef enum SimOp {
    /* narrow: every operand and the result are <= 64 bits (one limb) */
    SOP_COPY,        /* d = a */
    SOP_SLICE,       /* d = (a >> p) & mask */
    SOP_XSLICE,      /* d = a(aw bits, wide)[p +: w] */
    SOP_INDEX,       /* d = a[b*w +: w] (0 if out of range) */
    SOP_XINDEX,      /* same, a wide */
    SOP_NOT, SOP_NEG, SOP_LNOT, SOP_RAND, SOP_ROR, SOP_RXOR,
    SOP_ADD, SOP_SUB, SOP_MUL, SOP_DIVU, SOP_DIVS, SOP_MODU, SOP_MODS,
    SOP_AND, SOP_OR, SOP_XOR, SOP_SHL, SOP_SHR, SOP_ASHR,
    SOP_EQ, SOP_NE, SOP_LTU, SOP_LTS, SOP_LEU, SOP_LES, SOP_LAND, SOP_LOR,
    SOP_SHLOR,       /* d = ((a << p) | b) & mask */
    SOP_MUX,         /* d = a ? b : c */
    SOP_SEXT,        /* d = sign-extend a (aw bits) to w */
    SOP_DEPOSIT,     /* d[p +: aw] = a   (d is w <= 64 bits) */
    SOP_DEPOSIT_DYN, /* d[p + b*aw +: aw] = a (skipped if out of range) */
    /* wide / generic */
    SOP_WCOPY,       /* d = a (w bits) */
    SOP_WZERO,       /* d = 0 (w bits) */
    SOP_WEXTRACT,    /* d (w) = a (aw) [p +: w] */
    SOP_WINDEX,      /* d (w) = a (aw) [b*w +: w] */
    SOP_WUN,         /* sub = IrOp NOT/NEG (w) or RAND/ROR/RXOR (1 bit) */
    SOP_WBIN,        /* sub = IrOp ADD..ASHR, sgn for DIV/MOD */
    SOP_WCMP,        /* sub = IrOp EQ/NE/LT/LE, sgn */
    SOP_WMUX,
    SOP_WRESIZE,     /* sgn: sign-extend */
    SOP_WDEPOSIT,    /* d (w bits) [p +: aw] = a */
    SOP_WDEPOSIT_DYN,/* d (w bits) [p + b*aw +: aw] = a */
    /* memories */
    SOP_MEMREAD,     /* d (w) = mems[p][a] */
    SOP_MEMWRITE,    /* queue mems[p][a] = b (mask c if sub) */
    /* control and bookkeeping */
    SOP_JMP,         /* pc = p */
    SOP_JZ,          /* if !a: pc = p */
    SOP_SWITCH,      /* switches[p] on a */
    SOP_ASSERT,      /* asserts[p] with condition a */
    SOP_COV          /* cov[p]++ */
} SimOp;

typedef struct SimInsn {
    uint8_t op, sub, sgn, pad;
    uint32_t w, aw, bw;
    uint32_t d, a, b, c;
    uint32_t p;
    uint64_t mask;
} SimInsn;

typedef struct SimSwitch {
    uint32_t n;
    uint64_t *vals;      /* sorted */
    uint32_t *targets;
    uint32_t dflt;
} SimSwitch;

/* ------------------------------------------------------------ netlist -- */

typedef struct SimInst {
    const char *path;        /* "" for the top, "rx_fifo", "a.b" */
    const char *name;        /* instance name ("" for the top) */
    const IrModule *mod;
    uint32_t parent;         /* IR_NONE for the top */
    uint32_t sig_base;       /* first flattened signal */
    uint32_t dom_base;       /* first flattened domain */
    uint32_t nchildren;
    uint32_t *children;
} SimInst;

typedef struct SimSig {
    const char *path;        /* interned hierarchical Tern path */
    const char *svpath;      /* interned hierarchical SV name path */
    const IrSignal *ir;
    uint32_t inst;
    uint32_t width;
    uint32_t limbs;
    uint32_t off;            /* current value (IR_NONE for memories) */
    uint32_t next;           /* next value for state signals, else IR_NONE */
    uint32_t group;          /* edge group of a state signal, else IR_NONE */
    uint32_t mem;            /* memory index or IR_NONE */
    uint32_t clock;          /* top clock index if this is a root clock input */
    uint8_t writable;
    uint8_t is_state;
    const char *vcd_id;
} SimSig;

typedef struct SimMem {
    uint32_t sig, width, limbs, depth;
    uint64_t *data;
} SimMem;

typedef struct SimInitReg {
    uint32_t cur, next, init, limbs;   /* init: offset of the init constant */
} SimInitReg;

typedef struct SimDom {
    uint32_t inst;
    const IrDomain *ir;
    uint32_t clock, reset;   /* flattened signals; reset IR_NONE if none */
    int async, low;
    uint32_t group;
    uint32_t code_lo, code_hi;
    uint32_t init_lo, init_n;
    int in_reset;
} SimDom;

/* property formula node kinds (progression engine in eval.c).  Static
 * nodes 0, 1, 2 are the constants TRUE, FALSE and VAC (vacuously true). */
enum {
    SPK_TRUE, SPK_FALSE, SPK_VAC,
    SPK_EXPR,    /* leaf */
    SPK_CAT,     /* a ##[lo:hi] b */
    SPK_SKIP,    /* b starts in [lo, hi] cycles (0 = now) */
    SPK_IMPL, SPK_IMPLN, SPK_ALWAYS, SPK_NEVER,
    SPK_EV,      /* eventually within hi cycles (SIM_PINF: unbounded) */
    SPK_UNTIL, SPK_AND, SPK_OR, SPK_NOT
};
#define SIM_PINF 0xFFFFFFFFu
#define SIM_PDYN 0x80000000u   /* node reference into a group's dynamic pool */

/* property formula node */
typedef struct SimPNode {
    uint8_t kind;
    uint32_t a, b;
    uint32_t lo, hi;
    uint32_t leaf;           /* EXPR: value offset of the 1-bit leaf */
} SimPNode;

typedef struct SimPPool {
    VEC(SimPNode) nodes;
    uint32_t *table;         /* hash-consing: node index + 1, 0 = empty */
    uint32_t cap;
} SimPPool;

typedef struct SimAttempt {
    uint32_t ref;
    uint64_t start;
} SimAttempt;
typedef VEC(SimAttempt) SimAttemptVec;

typedef struct SimMon {
    uint32_t inst;
    const IrProperty *prop;
    uint32_t fd;
    uint32_t group;
    uint32_t root;           /* static node */
    uint32_t misc_lo, misc_hi; /* misc limbs of its leaves and past() histories
                                  (relative to misc_base): cleared in reset */
    int started;
    uint64_t hits;
    SimAttemptVec att;
} SimMon;

typedef struct SimGroup {
    uint32_t root;           /* flattened clock net */
    int level;               /* clock value after the edge */
    uint32_t clock;          /* top clock index, or IR_NONE (derived clock) */
    uint32_t cur_lo, nlimbs, next_lo;
    VEC(uint32_t) doms;
    VEC(uint32_t) mons;
    uint32_t pcode_lo, pcode_hi;
    uint64_t edges;
    int last;                /* derived clocks: last seen value */
    SimPPool pool[2];
    int cur_pool;
} SimGroup;

typedef struct SimClock {
    uint32_t sig;
    int falling;
    uint64_t hz;
    uint64_t half_ps;
    uint64_t next_toggle;
    int level;
    uint64_t cycles;
    uint32_t g_active, g_inactive;
    VEC(uint32_t) copies;    /* flattened clock nets that are copies of it */
} SimClock;

typedef struct SimTopDom {
    uint32_t clock;          /* SimClock index */
    uint32_t fd;
} SimTopDom;

typedef struct SimAssert {
    IrAssertKind kind;
    const char *msg;
    Span loc;
    uint32_t inst;
    uint32_t fd;             /* IR_NONE: comb */
    uint32_t cov;            /* cover statements: counter */
} SimAssert;

typedef struct SimCovRaw {
    SimCovKind kind;
    Span loc;
    uint32_t arm;
    const char *name;
    uint32_t counter;        /* index into cov[] (IR_NONE: property, use mon) */
    uint32_t mon;
} SimCovRaw;

typedef struct SimPendWrite {
    uint32_t mem;
    uint64_t addr;
    uint32_t data;           /* offset into pend_data */
    uint32_t mask;           /* offset into pend_data, IR_NONE if none */
} SimPendWrite;

/* value operations of a remote model (remote.c) */
typedef struct SimRemote SimRemote;
typedef struct SimRemoteOps {
    int (*poke)(SimModel *m, int h, const IrBits *v);
    int (*peek)(SimModel *m, int h, IrBits *out);
    int (*mem_peek)(SimModel *m, int h, uint64_t addr, IrBits *out);
    void (*settle)(SimModel *m);
    void (*step)(SimModel *m, uint32_t clock, uint32_t n);
    void (*run_ns)(SimModel *m, uint64_t ns);
    void (*reset)(SimModel *m);
    int (*vcd_open)(SimModel *m, const char *path);
    void (*destroy)(SimModel *m);
} SimRemoteOps;

struct SimModel {
    Arena arena;
    const SimRemoteOps *rops; /* non-NULL: values live in a remote simulator */
    SimRemote *remote;
    const IrDesign *design;
    const IrModule *top;
    uint64_t seed;

    VEC(SimInst) insts;
    VEC(SimSig) sigs;
    VEC(SimMem) mems;
    VEC(SimDom) doms;
    VEC(SimGroup) groups;
    VEC(SimClock) clocks;
    VEC(SimTopDom) topdoms;
    VEC(SimInitReg) initregs;
    VEC(SimAssert) asserts;
    VEC(SimSwitch) switches;
    VEC(SimMon) mons;
    VEC(SimPNode) pstatic;
    VEC(SimCovRaw) covraw;
    VEC(SimCovPoint) covout;
    VEC(uint32_t) derived;   /* groups clocked by derived clocks */
    Map by_path;             /* hash of path / SV path -> handle + 1 (first one wins) */

    /* code */
    VEC(SimInsn) code;
    uint32_t comb_end;

    /* values */
    uint64_t *v;
    size_t nv;
    uint32_t sig_limbs;      /* signals + next regions */
    uint32_t const_base, temp_base, misc_base;
    uint64_t *scratch;       /* 4 * max limbs */
    uint64_t *cov;
    uint32_t ncov;

    /* pending memory writes */
    VEC(SimPendWrite) pend;
    VEC(uint64_t) pend_data;

    /* state */
    int dirty;
    int in_init;
    int harness_reset;
    int derived_depth;
    uint64_t now_ps;
    uint32_t default_clock;  /* SimClock index for comb assertion cycles, or IR_NONE */

    /* failures */
    SimFailFn fail_fn;
    void *fail_user;
    uint32_t nfail;
    Map pseen;               /* attempt dedupe scratch */

    /* VCD */
    FILE *vcd;
    uint64_t *vcd_last;
    uint64_t vcd_time;
    int vcd_time_valid;
};

/* netlist.c */
int sim_build(SimModel *m);   /* 0 on error (diagnostics reported) */

/* eval.c */
void sim_exec(SimModel *m, uint32_t pc, uint32_t end);
void sim_settle_now(SimModel *m);
void sim_assert_hit(SimModel *m, uint32_t id, uint64_t val);
void sim_mon_reset_all(SimModel *m);

/* vcd.c */
void sim_vcd_sample(SimModel *m);
void sim_vcd_flush(SimModel *m);

#endif /* TERN_SIM_IMPL_H */
