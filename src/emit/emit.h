/* emit.h — the back-end emitters: IR -> SystemVerilog / Verilog-2005
 * (sv.c), timing-constraint fragments (constraints.c) and register-map
 * outputs (regmap.c).  Conventions of the emitted code: docs/emitted-sv.md.
 *
 *     SvOptions opt;
 *     SvOutput out = {0};
 *     sv_options_init(&opt);              // SV-2012, FPGA, line markers on
 *     opt.std = SV_STD_V2005;             // --std verilog2005
 *     if (emit_design(design, &opt, &out) == 0)       // everything `tern build` writes
 *         sv_output_write_dir(&out, "build/arty");    // or sv_output_concat() for --stdout
 *     sv_output_free(&out);
 *
 * Files produced by emit_design (all names are relative to the output
 * directory):
 *   <Module>.sv | <Module>.v     one per non-extern module (emitted name)
 *   <top>_types_pkg.sv           SV-2012 only, when the design has named
 *                                struct/enum types (imported by the modules
 *                                that use them)
 *   <Module>_<mem>.hex           $readmemh contents of initialized memories
 *   <Module>_<mem>.mif / .coe    with SV_MEMINIT_MIF / SV_MEMINIT_COE
 *   <top>.xdc, <top>.sdc         clocks, CDC false paths / max delays,
 *                                ASYNC_REG cells and the CDC report
 *   <Map>.h, <Map>.svd, <Map>.md per IrRegmap
 * Files are added in a deterministic order: package, modules (children
 * first), memory init files, constraints, regmap outputs.
 *
 * The emitters never modify the design.  Malformed IR (invariant violations)
 * is reported with diag_error() and counted in the return value; warnings
 * (power-on values on ASIC targets) go to diag_warning().
 */
#ifndef TERN_EMIT_H
#define TERN_EMIT_H

#include "../base/common.h"
#include "../base/str.h"
#include "../ir/ir.h"

/* ------------------------------------------------------------ options -- */

typedef enum SvStd {
    SV_STD_SV2012,   /* SystemVerilog IEEE 1800-2012 (default) */
    SV_STD_V2005     /* Verilog IEEE 1364-2005 (--std verilog2005) */
} SvStd;

typedef enum SvTargetKind {
    SV_TARGET_FPGA,  /* power-on values of reset-less registers as `initial` */
    SV_TARGET_ASIC   /* ... dropped with a warning (comment + diag_warning) */
} SvTargetKind;

/* SvOptions.mem_init_formats: extra memory-initialization files written next
 * to the .hex file that $readmemh loads. */
#define SV_MEMINIT_MIF 1u    /* Intel/Altera .mif */
#define SV_MEMINIT_COE 2u    /* Xilinx .coe */

/* SvOptions.constraints */
#define EMIT_CONS_XDC 1u     /* <top>.xdc (Vivado) */
#define EMIT_CONS_SDC 2u     /* <top>.sdc (Quartus / generic SDC) */

typedef struct SvOptions {
    SvStd std;
    int emit_assertions;        /* --emit-assertions: keep assert/assume/cover */
    SvTargetKind target_kind;
    int line_markers;           /* `// tern: file.tern:42` markers (default 1) */
    unsigned mem_init_formats;  /* SV_MEMINIT_* bit set (default 0: .hex only) */
    unsigned constraints;       /* emit_design: EMIT_CONS_* (default both) */
    int regmaps;                /* emit_design: regmap outputs (default 1) */
    const char *types_pkg;      /* package name; NULL = "<top>_types_pkg" */
} SvOptions;

void sv_options_init(SvOptions *o);

/* ------------------------------------------------------------- output -- */

typedef enum SvFileKind {
    SV_FILE_MODULE,    /* .sv / .v module */
    SV_FILE_PACKAGE,   /* types package */
    SV_FILE_MEMINIT,   /* .hex / .mif / .coe */
    SV_FILE_XDC,
    SV_FILE_SDC,
    SV_FILE_C_HEADER,  /* regmap .h */
    SV_FILE_SVD,       /* regmap CMSIS-SVD */
    SV_FILE_MARKDOWN   /* regmap documentation */
} SvFileKind;

typedef struct SvFile {
    char *name;        /* file name (no directory) */
    SvFileKind kind;
    char *data;        /* NUL-terminated contents */
    size_t len;
} SvFile;

/* A set of emitted files (zero-init is empty; heap-owned). */
typedef struct SvOutput {
    SvFile *files;
    size_t nfiles, cap;
    /* source files of extern modules (`extern module m("ip/m.sv")`), in
     * module order, without duplicates: `tern build` copies them next to the
     * emitted files.  Heap strings. */
    char **extern_files;
    size_t nextern;
} SvOutput;

/* Adds a file, taking ownership of the buffer's contents (sb is reset).
 * A file of the same name is replaced. */
void sv_output_add(SvOutput *o, const char *name, SvFileKind kind, StrBuf *contents);
const SvFile *sv_output_find(const SvOutput *o, const char *name);
/* Writes every file into dir (created with parents); unchanged files keep
 * their modification time.  Returns 0, or -1 after a diag_error(). */
int sv_output_write_dir(const SvOutput *o, const char *dir);
/* All files concatenated (for --stdout), each preceded by a header line
 * `// ---- file: <name> ----`. */
void sv_output_concat(const SvOutput *o, StrBuf *out);
void sv_output_free(SvOutput *o);

/* ----------------------------------------------------------- emitters -- */

/* Everything `tern build` emits: sv_emit_design + cons_emit (per
 * o->constraints) + rmap_emit (when o->regmaps).  Returns the number of
 * errors (0 = success). */
int emit_design(const IrDesign *d, const SvOptions *o, SvOutput *out);

/* SystemVerilog / Verilog-2005 modules, the types package and memory
 * initialization files.  Returns the number of errors. */
int sv_emit_design(const IrDesign *d, const SvOptions *o, SvOutput *out);

/* Constraint fragments for the design's top (o->constraints selects .xdc
 * and/or .sdc; o may be NULL for both).  Returns the number of errors. */
int cons_emit(const IrDesign *d, const SvOptions *o, SvOutput *out);
void cons_emit_xdc(const IrDesign *d, StrBuf *out);
void cons_emit_sdc(const IrDesign *d, StrBuf *out);

/* Register-map outputs <Map>.h, <Map>.svd and <Map>.md for every regmap. */
int rmap_emit(const IrDesign *d, SvOutput *out);
void rmap_emit_header(const IrRegmap *r, StrBuf *out);
void rmap_emit_svd(const IrRegmap *r, StrBuf *out);
void rmap_emit_markdown(const IrRegmap *r, StrBuf *out);

/* A port-less SystemVerilog module `name` with one variable per port of
 * d->top (same names and types) and the top instantiated as `dut` with
 * those variables: the Verilator test harness (`tern test --backend
 * verilator`) drives and reads them through VPI. */
void sv_emit_wrapper(const IrDesign *d, const SvOptions *o, const char *name, StrBuf *out);

/* ------------------------------------------- parameterized modules -- */

/* Spec §13.3: value generics become SystemVerilog parameters where that is
 * exact, and stay monomorphized otherwise.  The front end hands the emitter,
 * per generic module declaration, expressions over its generics found in
 * the source (SvXExpr trees) and extra elaborations with other argument
 * values ("probes"); sv_params_plan() then
 *
 *   1. groups the modules of the design by declaration (IrModule.origin;
 *      only declarations whose generics are all uint (< 2^31) or bool, not
 *      extern / CDC primitives / with initialized memories),
 *   2. walks every elaboration (the design's instances plus the probes) in
 *      lockstep: the IR must be identical except for numbers (signal
 *      widths, memory depths, constants, reset values, slice positions,
 *      child generic arguments), and each number that differs must equal
 *      one candidate expression in every elaboration (candidates: the
 *      source expressions, the generics, and simple derived forms such as
 *      `e + 1`); every generic must take two values among the elaborations,
 *   3. emits each elaboration with those numbers printed as the expressions
 *      (SV-2012 and Verilog-2005, with and without assertions) and accepts
 *      the group only if all texts are identical (types declared inside the
 *      module are compared by position, not name).
 *
 * An accepted group is one SV module named after the declaration
 * (`Blinky`, with `parameter int unsigned CLK_HZ = 50000000`), and its
 * instances pass `#(.CLK_HZ(..))`; the plan is attached to the design
 * (IrDesign.sv_params) and used by every later emission and by SvNames. */

typedef enum SvXOp {
    SVX_K,        /* constant k */
    SVX_PARAM,    /* generic number `param` */
    SVX_LOCAL,    /* a module const `name` = a (emitted as a localparam) */
    SVX_ADD, SVX_SUB, SVX_MUL, SVX_DIV, SVX_MOD, SVX_SHL, SVX_SHR,
    SVX_AND, SVX_OR, SVX_XOR,
    SVX_EQ, SVX_NE, SVX_LT, SVX_LE, SVX_GT, SVX_GE, SVX_LAND, SVX_LOR,
    SVX_CLOG2,    /* clog2(a) == $clog2(a) */
    SVX_COND      /* a ? b : c */
} SvXOp;

typedef struct SvXExpr {
    SvXOp op;
    int64_t k;
    uint32_t param;
    const char *name;         /* SVX_LOCAL (interned) */
    struct SvXExpr *a, *b, *c;
} SvXExpr;

typedef struct SvParamItem {
    const void *item;         /* IrOrigin.item of the declaration */
    uint32_t ncands;
    SvXExpr **cands;          /* expressions over the generics, in source order */
    uint32_t nprobes;
    IrDesign **probes;        /* other elaborations: designs whose top is the module */
} SvParamItem;

typedef struct SvParamInput {
    const SvParamItem *items;
    size_t n;
} SvParamInput;

/* Decides which module groups of `d` are emitted parameterized and attaches
 * the plan to d (replacing an earlier one).  Probe designs are not
 * referenced afterwards.  Returns the number of accepted groups. */
int sv_params_plan(IrDesign *d, const SvParamInput *in, const SvOptions *o);
void sv_params_free(IrDesign *d);
/* Evaluates x with the given generic values; 0 if it is undefined or
 * leaves [0, 2^31) anywhere (SystemVerilog `int unsigned` arithmetic). */
int sv_xexpr_eval(const SvXExpr *x, const int64_t *params, int64_t *out);

/* -------------------------------------------------------------- names -- */

/* The identifiers the emitter uses (keyword escaping and collision
 * renaming are deterministic; docs/emitted-sv.md).  Other back ends that
 * refer to emitted modules — constraints, testbench and formal script
 * generators — use these so that they always agree with the .sv files. */
typedef struct SvNames SvNames;

SvNames *sv_names_new(const IrDesign *d);
void sv_names_free(SvNames *n);
const char *sv_names_module(SvNames *n, const IrModule *m);                 /* module / file stem */
const char *sv_names_signal(SvNames *n, const IrModule *m, uint32_t sig);   /* port, signal, memory */
const char *sv_names_instance(SvNames *n, const IrModule *m, uint32_t inst);
const char *sv_names_types_pkg(SvNames *n);                                 /* "<top>_types_pkg" */

/* 1 if s is a reserved word of SystemVerilog (IEEE 1800-2017, which
 * includes every Verilog-2005 keyword). */
int sv_is_keyword(const char *s);

#endif /* TERN_EMIT_H */
