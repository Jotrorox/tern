/* ir.h — the typed intermediate representation of an elaborated Tern design.
 *
 * The IR is the only thing the back ends see (SystemVerilog emitter,
 * simulator, formal, IR JSON).  It is produced by src/elab/elab.c through the
 * helpers in ir_build.c and serialized by ir_json.c.
 *
 * Invariants (the elaborator guarantees them; back ends may assert them):
 *   - Every width is concrete and > 0 (a zero-width value never reaches the IR).
 *   - Arithmetic operands are already resized to the result width (see IrOp).
 *   - Signal ids index IrModule.sigs; IR_NONE marks "absent".
 *   - All IR memory lives in the arena passed to ir_design_new().
 *
 * Changes to the original contract (made while implementing ir_build.c,
 * ir_json.c and the simulator; additive only):
 *   - IrDesign.build: private state of ir_build.c (the interning table for
 *     anonymous types); NULL until the first ir_type_* call.
 *   - The "builder API" section at the end: type constructors (anonymous
 *     types are interned per design, so structurally equal types are the
 *     same pointer), module/signal/process/instance/property builders that
 *     append to the IrModule arrays in place, statement/lvalue/sequence
 *     constructors, IrBits helpers and ir_from_json().
 *   - Arrays that the ir_add_* helpers append to grow by doubling inside the
 *     arena; an array a module was given by other means must not be appended
 *     to with them (ir_from_json() allocates compatible arrays).
 *   - IrModule.origin (declaration + generic arguments) and IrDesign.sv_params
 *     (the emitter's parameterization plan): side channels for `tern build`,
 *     not part of the JSON form.
 */
#ifndef TERN_IR_H
#define TERN_IR_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/diag.h"

#define IR_NONE 0xFFFFFFFFu

/* ---------------------------------------------------------------- bits --- */

/* A two-state bit vector.  ceil(width/64) little-endian 64-bit limbs; bits at
 * positions >= width are always zero. */
typedef struct IrBits {
    uint32_t width;
    uint64_t *w;
} IrBits;

#define IR_LIMBS(width) (((width) + 63u) / 64u)

/* ---------------------------------------------------------------- types -- */

typedef enum IrTypeKind {
    IRT_BOOL,   /* 1 bit, logical */
    IRT_BITS,   /* bit<N> */
    IRT_UINT,   /* uint<N> */
    IRT_SINT,   /* sint<N> */
    IRT_ENUM,   /* tag ++ payload; tag occupies the MSBs */
    IRT_STRUCT, /* packed; fields[0] is the most significant */
    IRT_ARRAY,  /* element i occupies bits [i*elem->width +: elem->width] */
    IRT_CLOCK,  /* 1 bit */
    IRT_RESET   /* 1 bit; style in async/active_low */
} IrTypeKind;

typedef enum IrEncoding { IR_ENC_BINARY, IR_ENC_ONEHOT, IR_ENC_GRAY } IrEncoding;

typedef struct IrType IrType;

typedef struct IrField {
    const char *name;   /* interned */
    IrType *type;
    uint32_t lsb;       /* bit offset of the field's LSB within the struct */
} IrField;

typedef struct IrVariant {
    const char *name;   /* interned */
    IrBits tag;         /* width == enum tag_width */
    IrType *payload;    /* NULL, or an IRT_STRUCT holding the payload fields,
                           placed in the LSBs (zero-padded to the largest) */
} IrVariant;

struct IrType {
    IrTypeKind kind;
    uint32_t width;           /* total width in bits */
    const char *name;         /* STRUCT/ENUM: emitted type name ("Pixel",
                                 "Fifo_u8_4_State"); NULL if anonymous */
    /* IRT_STRUCT */
    uint32_t nfields;
    IrField *fields;
    /* IRT_ENUM */
    uint32_t nvariants;
    IrVariant *variants;
    uint32_t tag_width;
    IrEncoding encoding;
    /* IRT_ARRAY */
    IrType *elem;
    uint32_t len;
    /* IRT_CLOCK */
    int falling;
    /* IRT_RESET */
    int async;
    int active_low;
    Span loc;                 /* declaration, for named types */
};

/* ---------------------------------------------------------- attributes -- */

typedef struct IrAttr {
    const char *name;   /* "ram", "keep", "max_fanout", ... (interned) */
    const char *args;   /* raw argument text, e.g. "block", "hz: 100000000"; NULL if none */
} IrAttr;

/* ------------------------------------------------------------- signals -- */

typedef enum IrSigKind {
    IRS_IN,    /* input port */
    IRS_OUT,   /* output port (driven like a wire or var) */
    IRS_WIRE,  /* let */
    IRS_VAR,   /* combinational variable, driven by one comb process */
    IRS_REG,   /* flip-flop, driven by one clocked process */
    IRS_MEM    /* memory; `type` is the element type, `depth` the entry count */
} IrSigKind;

typedef struct IrSignal {
    const char *name;     /* SV identifier, unique in the module (tx_valid, r_0) */
    const char *path;     /* Tern-level path used by tests/LSP ("tx.valid", "r_0") */
    IrSigKind kind;
    IrType *type;
    uint32_t domain;      /* index into IrModule.domains; IR_NONE = domain-free */
    int has_init;         /* REG: reset value (or power-on value if the domain has no reset) */
    IrBits init;
    int test_port;        /* #[test_port]: tests may drive it */
    int flipped;          /* port that came from a `flip` bundle field (informational) */
    /* IRS_MEM */
    uint32_t depth;
    IrBits *mem_init;     /* NULL, or `depth` entries of type->width bits */
    const char *mem_init_file; /* include_hex/include_bin source path, or NULL */
    int mem_init_is_bin;
    /* attributes passed through to the emitted SV */
    uint32_t nattrs;
    IrAttr *attrs;
    Span loc;
} IrSignal;

/* --------------------------------------------------------- expressions -- */

typedef enum IrExprKind {
    IRE_CONST,    /* bits */
    IRE_SIG,      /* sig (any kind except MEM) */
    IRE_SLICE,    /* args[0][lo +: width]; also struct fields (field != NULL),
                     enum payload fields, constant array indices */
    IRE_INDEX,    /* dynamic: element args[1] of args[0]; element i is
                     args[0][i*width +: width].  args[0] holds
                     args[0].width / width elements; an index past the last
                     one reads 0 (2-state; possible for array lengths that
                     are not a power of two, spec §5.5) */
    IRE_UNARY,    /* op in IROP_NOT..IROP_RXOR */
    IRE_BINARY,   /* op in IROP_ADD..IROP_LOR */
    IRE_CONCAT,   /* args[0] is the most significant */
    IRE_MUX,      /* args[0] (1 bit) ? args[1] : args[2] */
    IRE_RESIZE,   /* zero-extend (is_signed=0) or sign-extend (is_signed=1)
                     args[0] to width, or truncate to its low `width` bits */
    IRE_MEMREAD,  /* combinational (async) read: mem signal `sig`, address args[0] */
    IRE_PAST      /* properties only: value of args[0] `lo` cycles ago */
} IrExprKind;

typedef enum IrOp {
    /* unary */
    IROP_NOT,     /* bitwise ~ */
    IROP_NEG,     /* two's complement negate (width preserved) */
    IROP_LNOT,    /* logical not of a 1-bit operand */
    IROP_RAND,    /* reduction & → 1 bit */
    IROP_ROR,     /* reduction | → 1 bit */
    IROP_RXOR,    /* reduction ^ → 1 bit */
    /* binary: operands and result have the same width */
    IROP_ADD, IROP_SUB, IROP_MUL,   /* modulo 2^width */
    IROP_DIV, IROP_MOD,             /* is_signed selects signed division; x/0 == 0, x%0 == x */
    IROP_AND, IROP_OR, IROP_XOR,
    /* shifts: args[0].width == width; args[1] is an unsigned amount of any width */
    IROP_SHL, IROP_SHR, IROP_ASHR,
    /* comparisons: operands equal width; result width 1; is_signed for LT..GE */
    IROP_EQ, IROP_NE, IROP_LT, IROP_LE, IROP_GT, IROP_GE,
    /* logical: 1-bit operands, 1-bit result */
    IROP_LAND, IROP_LOR
} IrOp;

typedef struct IrExpr IrExpr;
struct IrExpr {
    IrExprKind kind;
    IrOp op;              /* UNARY / BINARY */
    uint32_t width;       /* result width */
    int is_signed;        /* see IrOp / IRE_RESIZE; also: result is a signed value */
    IrType *type;         /* optional semantic type of the result (enum/struct/...);
                             may be NULL; never changes the bit semantics */
    IrBits bits;          /* CONST */
    uint32_t sig;         /* SIG, MEMREAD */
    uint32_t lo;          /* SLICE: low bit; PAST: cycles */
    const char *field;    /* SLICE: struct/bundle field name for readable SV, or NULL */
    uint32_t nargs;
    IrExpr **args;
    Span loc;
};

/* ----------------------------------------------------------- lvalues ---- */

/* Assignment target: bits [lo +: width] of signal `sig`, or — when `index` is
 * non-NULL — element `index` (each element `width` bits wide) of the array
 * that starts at bit `lo` of `sig` and has `len` elements.  An assignment
 * with index >= len writes nothing (2-state; possible for array lengths that
 * are not a power of two).  A whole-signal assignment has lo == 0 and
 * width == type->width. */
typedef struct IrLvalue {
    uint32_t sig;
    uint32_t lo;
    uint32_t width;
    IrExpr *index;
    const char *field;    /* optional field path for readable SV ("carry", "pos.x") */
    uint32_t len;         /* index != NULL: elements of the indexed array; 0 means
                             as many whole elements as fit from lo to the end of
                             the signal (lo + len * width <= signal width) */
} IrLvalue;

/* -------------------------------------------------------- statements ---- */

typedef enum IrAssertKind { IRA_ASSERT, IRA_ASSUME, IRA_COVER, IRA_EXPECT } IrAssertKind;

typedef enum IrStmtKind {
    IRST_ASSIGN,    /* lhs = rhs (blocking in comb, next-value in clocked) */
    IRST_IF,        /* if cond then_b else else_b */
    IRST_CASE,      /* unique case (subject): arms; exhaustive by construction */
    IRST_MEMWRITE,  /* mem[addr] = data (clocked only); mask: NULL or one bit per byte */
    IRST_ASSERT     /* immediate assertion (stripped from synthesis output) */
} IrStmtKind;

typedef struct IrStmt IrStmt;

typedef struct IrBlock {
    uint32_t n;
    IrStmt **stmts;
} IrBlock;

typedef struct IrCaseArm {
    uint32_t nvals;   /* 0 means `default` */
    IrBits *vals;     /* each subject->width wide */
    IrBlock body;
} IrCaseArm;

struct IrStmt {
    IrStmtKind kind;
    /* ASSIGN */
    IrLvalue lhs;
    IrExpr *rhs;
    /* IF / ASSERT */
    IrExpr *cond;
    IrBlock then_b;
    IrBlock else_b;
    /* CASE */
    IrExpr *subject;
    uint32_t narms;
    IrCaseArm *arms;
    /* MEMWRITE */
    uint32_t mem;
    IrExpr *addr;
    IrExpr *data;
    IrExpr *mask;
    /* ASSERT */
    IrAssertKind akind;
    const char *msg;   /* NULL if none */
    Span loc;
};

/* ----------------------------------------------------------- module ---- */

typedef struct IrAssign {     /* continuous assignment (let, module-level `x = e`) */
    IrLvalue lhs;
    IrExpr *rhs;
    Span loc;
} IrAssign;

typedef enum IrProcKind { IRP_COMB, IRP_CLOCKED } IrProcKind;

typedef struct IrProcess {
    IrProcKind kind;
    uint32_t domain;          /* CLOCKED: the domain; COMB: IR_NONE */
    IrBlock body;
    uint32_t nregs;           /* CLOCKED: registers driven here (reset branch is
                                 generated from their has_init/init) */
    uint32_t *regs;
    Span loc;
} IrProcess;

typedef struct IrDomain {
    const char *name;         /* "" for the implicit default domain */
    uint32_t clock;           /* signal id of the clock (IRT_CLOCK) */
    uint32_t reset;           /* signal id of the reset (IRT_RESET) or IR_NONE */
    uint64_t hz;              /* from #[clock(hz: N)], 0 if unknown */
} IrDomain;

typedef struct IrModule IrModule;

typedef struct IrConn {
    uint32_t port;            /* signal id of the port in the child module */
    IrExpr *expr;             /* IN: expression in the parent's scope.
                                 OUT: IRE_SIG of a parent signal the output drives
                                 (the elaborator creates a wire per used output,
                                 named "<inst>_<port>"), or NULL if unused. */
} IrConn;

typedef struct IrInstance {
    const char *name;         /* instance name in the parent ("rx_fifo") */
    IrModule *module;
    uint32_t nconns;
    IrConn *conns;
    Span loc;
} IrInstance;

typedef enum IrSeqKind {
    IRQ_EXPR,       /* boolean expression evaluated in one cycle */
    IRQ_DELAY,      /* a ##[lo:hi] b (hi == IR_NONE means $) */
    IRQ_IMPL,       /* a |-> b (overlapping) */
    IRQ_IMPL_NEXT,  /* a |=> b */
    IRQ_ALWAYS,     /* always a */
    IRQ_NEVER,      /* never a */
    IRQ_EVENTUALLY, /* eventually[..hi] a (hi == IR_NONE: unbounded) */
    IRQ_UNTIL,      /* a until b */
    IRQ_AND,
    IRQ_OR,
    IRQ_NOT
} IrSeqKind;

typedef struct IrSeq IrSeq;
struct IrSeq {
    IrSeqKind kind;
    IrExpr *expr;   /* IRQ_EXPR */
    IrSeq *a;
    IrSeq *b;
    uint32_t lo;
    uint32_t hi;
};

typedef struct IrProperty {
    const char *name;
    IrAssertKind kind;   /* ASSERT (default), ASSUME or COVER */
    uint32_t domain;
    IrSeq *seq;
    Span loc;
} IrProperty;

typedef struct IrParam {      /* resolved value generic, for readability */
    const char *name;
    IrBits value;
    int is_signed;
} IrParam;

/* Where a module came from, for back ends that re-elaborate a declaration
 * with other generic arguments (parameterized SystemVerilog, §13.3).  Set
 * by the elaborator; not serialized (NULL for IR read from JSON or built
 * with the builder API). */
typedef enum IrGenericKind {
    IRG_UINT,     /* unsigned integer value */
    IRG_SINT,     /* signed integer value */
    IRG_BOOL,     /* bool value (0 / 1) */
    IRG_OTHER     /* any other value (enum, array, ...) or a type */
} IrGenericKind;

typedef struct IrGeneric {
    const char *name;         /* generic parameter name (interned) */
    IrGenericKind kind;
    int fits;                 /* UINT/SINT/BOOL: value fits in int64 */
    int64_t value;
} IrGeneric;

typedef struct IrOrigin {
    const void *item;         /* the module declaration (an AST Item) */
    const void *file;         /* its FileUnit */
    uint32_t ngenerics;       /* every generic parameter, in declaration order */
    IrGeneric *generics;
} IrOrigin;

typedef enum IrCdcKind {
    IRC_SYNC2, IRC_SYNC3, IRC_PULSE, IRC_HANDSHAKE, IRC_ASYNC_FIFO,
    IRC_RESET_SYNC, IRC_RESET_BRIDGE, IRC_GRAY, IRC_UNSAFE
} IrCdcKind;

struct IrModule {
    const char *name;         /* emitted (mangled) name */
    const char *tern_name;    /* source name ("Fifo") */
    const char *display;      /* "Fifo<u8, 64>" for diagnostics and docs */
    const char *package;      /* "std::fifo" */
    uint32_t nparams;
    IrParam *params;
    uint32_t nsigs;
    IrSignal *sigs;
    uint32_t nports;
    uint32_t *ports;          /* port signal ids in declaration order */
    uint32_t ndomains;
    IrDomain *domains;
    uint32_t default_domain;  /* index or IR_NONE */
    uint32_t nassigns;
    IrAssign *assigns;
    uint32_t nprocs;
    IrProcess *procs;
    uint32_t ninsts;
    IrInstance *insts;
    uint32_t nprops;
    IrProperty *props;
    uint32_t nattrs;
    IrAttr *attrs;
    int is_top;
    int is_extern;            /* black box: only ports are meaningful */
    const char *extern_file;  /* path given in `extern module m("file.sv")` */
    int is_cdc;               /* a std::cdc primitive (#[cdc(...)]) */
    IrCdcKind cdc_kind;
    IrOrigin *origin;         /* elaborator side channel (see IrOrigin), or NULL */
    Span loc;
};

/* --------------------------------------------------------- regmaps ---- */

typedef enum IrAccess { IR_RW, IR_RO, IR_WO, IR_W1C, IR_RW1S } IrAccess;

typedef struct IrRegField {
    const char *name;
    uint32_t lo, width;
    IrType *type;
} IrRegField;

typedef struct IrReg {
    uint64_t offset;
    const char *name;
    IrAccess access;
    uint32_t width;           /* register data width */
    uint32_t nfields;         /* 0: scalar register */
    IrRegField *fields;
    int is_stream;            /* `T -> Stream<T>` FIFO-style register */
    const char *doc;
} IrReg;

typedef struct IrRegmap {
    const char *name;
    const char *package;
    uint32_t nregs;
    IrReg *regs;
    Span loc;
} IrRegmap;

/* -------------------------------------------------------- crossings ---- */

typedef struct IrCrossing {   /* one entry of the CDC report */
    const char *path;         /* instance path "Top > xfifo" */
    IrCdcKind kind;
    const char *from_domain;
    const char *to_domain;
    const char *signal;
    Span loc;
} IrCrossing;

/* ----------------------------------------------------------- design ---- */

typedef struct IrDesign {
    Arena *arena;
    uint32_t nmodules;
    IrModule **modules;       /* children before parents; last is `top` */
    IrModule *top;
    uint32_t ntypes;
    IrType **types;           /* named struct/enum types, dependency order */
    uint32_t nregmaps;
    IrRegmap *regmaps;
    uint32_t ncrossings;
    IrCrossing *crossings;
    struct IrBuildState *build; /* ir_build.c private (type interning); may be NULL */
    struct SvParamPlan *sv_params; /* src/emit private: modules emitted as parameterized
                                      SV modules (sv_params_plan); NULL: all monomorphized */
} IrDesign;

/* ------------------------------------------------- builder helpers ---- */
/* Implemented in ir_build.c. */

IrDesign *ir_design_new(Arena *a);
IrBits ir_bits_zero(Arena *a, uint32_t width);
IrBits ir_bits_u64(Arena *a, uint32_t width, uint64_t v);
int ir_bits_is_zero(const IrBits *b);
uint64_t ir_bits_low64(const IrBits *b);
int ir_bits_get(const IrBits *b, uint32_t bit);
void ir_bits_set(IrBits *b, uint32_t bit, int v);
int ir_bits_eq(const IrBits *a, const IrBits *b);

IrExpr *ir_const(Arena *a, IrBits bits, Span loc);
IrExpr *ir_sig(Arena *a, const IrModule *m, uint32_t sig, Span loc);
IrExpr *ir_slice(Arena *a, IrExpr *x, uint32_t lo, uint32_t width, Span loc);
IrExpr *ir_index(Arena *a, IrExpr *x, IrExpr *idx, uint32_t elem_width, Span loc);
IrExpr *ir_unary(Arena *a, IrOp op, IrExpr *x, Span loc);
IrExpr *ir_binary(Arena *a, IrOp op, IrExpr *x, IrExpr *y, int is_signed, Span loc);
IrExpr *ir_concat(Arena *a, uint32_t n, IrExpr **parts, Span loc);
IrExpr *ir_mux(Arena *a, IrExpr *c, IrExpr *t, IrExpr *e, Span loc);
IrExpr *ir_resize(Arena *a, IrExpr *x, uint32_t width, int sign_extend, Span loc);

/* Look up a module's port/signal by SV name or Tern path; IR_NONE if absent. */
uint32_t ir_find_signal(const IrModule *m, const char *name);
uint32_t ir_find_path(const IrModule *m, const char *path);

/* Debug: human-readable dump (used by golden tests). */
struct StrBuf;
void ir_dump(const IrDesign *d, struct StrBuf *out);

/* JSON (ir_json.c): documented schema in docs/ir-schema.md. */
void ir_to_json(const IrDesign *d, struct StrBuf *out);
/* Read a design written by ir_to_json (all memory in `a`; source files named
 * in spans are registered with src_add() when not known yet). Returns NULL
 * and reports a diag_error() on malformed input. */
IrDesign *ir_from_json(Arena *a, const char *text, size_t len);

/* ====================================================== builder API ==== */
/* Everything below is implemented in ir_build.c.  Constructors assert the
 * IR invariants (TERN_ASSERT) and compute result widths. */

/* -- bits ------------------------------------------------------------- */
IrBits ir_bits_ones(Arena *a, uint32_t width);
IrBits ir_bits_copy(Arena *a, const IrBits *b);
IrBits ir_bits_from_limbs(Arena *a, uint32_t width, const uint64_t *w); /* masks to width */
/* zero-/sign-extend or truncate */
IrBits ir_bits_resize(Arena *a, const IrBits *b, uint32_t width, int sign_extend);
/* Verilog-style literal "<width>'h<hex>" (e.g. "8'h1f"); parse accepts 'h, 'd
 * and 'b and returns 0 on malformed input. */
void ir_bits_print(const IrBits *b, struct StrBuf *out);
int ir_bits_parse(Arena *a, const char *s, size_t n, IrBits *out);

/* -- types -------------------------------------------------------------- */
/* Anonymous types are interned per design: the same arguments return the
 * same pointer.  Named struct/enum types are created fresh and appended to
 * d->types (callers create them in dependency order). */
IrType *ir_type_bool(IrDesign *d);
IrType *ir_type_bits(IrDesign *d, uint32_t width);
IrType *ir_type_uint(IrDesign *d, uint32_t width);
IrType *ir_type_sint(IrDesign *d, uint32_t width);
IrType *ir_type_clock(IrDesign *d, int falling);
IrType *ir_type_reset(IrDesign *d, int async, int active_low);
IrType *ir_type_array(IrDesign *d, IrType *elem, uint32_t len);
/* fields[0] is the MSB; the lsb members are computed (input lsb ignored). */
IrType *ir_type_struct(IrDesign *d, const char *name, uint32_t nfields, const IrField *fields,
                       Span loc);
/* width = tag_width + the widest payload; variant tags are copied. */
IrType *ir_type_enum(IrDesign *d, const char *name, uint32_t nvariants, const IrVariant *variants,
                     uint32_t tag_width, IrEncoding enc, Span loc);
int ir_type_is_signed(const IrType *t);                   /* IRT_SINT */
const IrField *ir_type_field(const IrType *t, const char *name);   /* struct field or NULL */
const IrVariant *ir_type_variant(const IrType *t, const char *name);
void ir_type_print(const IrType *t, struct StrBuf *out); /* "uint<8>", "Pixel", "[u8; 4]"-style */

/* -- design and modules --------------------------------------------------- */
/* A new, empty module (not yet part of the design: add it with
 * ir_design_add_module once its children have been added). Names are
 * interned; tern_name/display default to name when NULL. */
IrModule *ir_module_new(IrDesign *d, const char *name, const char *tern_name, const char *display,
                        Span loc);
void ir_design_add_module(IrDesign *d, IrModule *m);      /* appends (children first) */
void ir_design_set_top(IrDesign *d, IrModule *m);         /* is_top = 1; moved to the end */
IrModule *ir_design_find_module(const IrDesign *d, const char *name);
void ir_design_add_regmap(IrDesign *d, const IrRegmap *r);    /* deep copy */
void ir_design_add_crossing(IrDesign *d, const IrCrossing *c);

/* Adds a signal and returns its id.  path NULL means path = name.  IN/OUT
 * signals are appended to m->ports.  domain starts as IR_NONE.  The returned
 * id stays valid; pointers into m->sigs only until the next ir_add_signal. */
uint32_t ir_add_signal(IrDesign *d, IrModule *m, const char *name, const char *path,
                       IrSigKind kind, IrType *type, Span loc);
uint32_t ir_add_memory(IrDesign *d, IrModule *m, const char *name, IrType *elem, uint32_t depth,
                       Span loc);
void ir_set_init(IrDesign *d, IrModule *m, uint32_t sig, IrBits init); /* has_init = 1 (copied) */
void ir_signal_add_attr(IrDesign *d, IrModule *m, uint32_t sig, const char *name, const char *args);
void ir_module_add_attr(IrDesign *d, IrModule *m, const char *name, const char *args);
uint32_t ir_add_param(IrDesign *d, IrModule *m, const char *name, IrBits value, int is_signed);
uint32_t ir_add_domain(IrDesign *d, IrModule *m, const char *name, uint32_t clock, uint32_t reset,
                       uint64_t hz);
void ir_add_assign(IrDesign *d, IrModule *m, IrLvalue lhs, IrExpr *rhs, Span loc);
/* Clocked: `regs` is computed from the assignment targets in `body`. */
uint32_t ir_add_process(IrDesign *d, IrModule *m, IrProcKind kind, uint32_t domain, IrBlock body,
                        Span loc);
uint32_t ir_add_instance(IrDesign *d, IrModule *m, const char *name, IrModule *child, Span loc);
/* Connect port `port` (child signal id) of instance `inst`; see IrConn. */
void ir_connect(IrDesign *d, IrModule *m, uint32_t inst, uint32_t port, IrExpr *expr);
/* Convenience: connect by port name/path; returns 0 if the child has no such port. */
int ir_connect_name(IrDesign *d, IrModule *m, uint32_t inst, const char *port, IrExpr *expr);
uint32_t ir_add_property(IrDesign *d, IrModule *m, const char *name, IrAssertKind kind,
                         uint32_t domain, IrSeq *seq, Span loc);

/* -- expressions (in addition to the constructors above) ---------------- */
IrExpr *ir_const_u64(Arena *a, uint32_t width, uint64_t v, Span loc);
IrExpr *ir_memread(Arena *a, const IrModule *m, uint32_t mem, IrExpr *addr, Span loc);
IrExpr *ir_past(Arena *a, IrExpr *x, uint32_t cycles, Span loc);
/* Struct field read; x->type must be an IRT_STRUCT with that field. */
IrExpr *ir_field(Arena *a, IrExpr *x, const char *field, Span loc);

/* -- lvalues ---------------------------------------------------------- */
IrLvalue ir_lv_sig(const IrModule *m, uint32_t sig);
IrLvalue ir_lv_slice(const IrModule *m, uint32_t sig, uint32_t lo, uint32_t width);
IrLvalue ir_lv_index(const IrModule *m, uint32_t sig, uint32_t lo, uint32_t elem_width,
                     IrExpr *index);

/* -- statements and blocks ----------------------------------------------- */
/* Heap-backed statement list (zero-init valid); ir_stmts_finish copies it
 * into the arena as an IrBlock and frees the list. */
typedef struct IrStmtList {
    IrStmt **data;
    size_t len, cap;
} IrStmtList;
void ir_stmts_push(IrStmtList *l, IrStmt *s);
IrBlock ir_stmts_finish(Arena *a, IrStmtList *l);
IrBlock ir_block(Arena *a, uint32_t n, IrStmt *const *stmts);   /* copies the array */
IrBlock ir_block1(Arena *a, IrStmt *s);                          /* one statement */
IrStmt *ir_stmt_assign(Arena *a, IrLvalue lhs, IrExpr *rhs, Span loc);
IrStmt *ir_stmt_if(Arena *a, IrExpr *cond, IrBlock then_b, IrBlock else_b, Span loc);
/* arms (and their vals) are copied */
IrStmt *ir_stmt_case(Arena *a, IrExpr *subject, uint32_t narms, const IrCaseArm *arms, Span loc);
IrStmt *ir_stmt_memwrite(Arena *a, const IrModule *m, uint32_t mem, IrExpr *addr, IrExpr *data,
                         IrExpr *mask, Span loc);
IrStmt *ir_stmt_assert(Arena *a, IrAssertKind kind, IrExpr *cond, const char *msg, Span loc);

/* -- property sequences ------------------------------------------------ */
IrSeq *ir_seq_expr(Arena *a, IrExpr *e);
IrSeq *ir_seq_delay(Arena *a, IrSeq *x, uint32_t lo, uint32_t hi, IrSeq *y); /* hi IR_NONE: $ */
IrSeq *ir_seq_binary(Arena *a, IrSeqKind kind, IrSeq *x, IrSeq *y); /* IMPL, IMPL_NEXT, UNTIL, AND, OR */
IrSeq *ir_seq_unary(Arena *a, IrSeqKind kind, IrSeq *x);            /* ALWAYS, NEVER, NOT */
IrSeq *ir_seq_eventually(Arena *a, IrSeq *x, uint32_t hi);          /* hi IR_NONE: unbounded */

/* -- names ---------------------------------------------------------- */
const char *ir_sig_kind_name(IrSigKind k);     /* "in", "out", "wire", "var", "reg", "mem" */
const char *ir_op_name(IrOp op);               /* "add", "shl", ... (JSON names) */
const char *ir_op_symbol(IrOp op);             /* "+", "<<", ... */
const char *ir_expr_kind_name(IrExprKind k);   /* "const", "sig", ... */
const char *ir_seq_kind_name(IrSeqKind k);
const char *ir_assert_kind_name(IrAssertKind k);
const char *ir_cdc_kind_name(IrCdcKind k);
const char *ir_access_name(IrAccess a);
const char *ir_encoding_name(IrEncoding e);

#endif /* TERN_IR_H */
