/* backend.h — the commands that turn a design into files for, and results
 * from, external tools (spec §13.2): `tern build` (build.c), `tern prove`
 * (formal.c, with the VCD reader in vcdread.c), `tern synth` (synth.c) and
 * the Verilator lint helper (verilator.c).  docs/backend.md describes the
 * generated files and the external tools.
 *
 *     // build: elaborate, check, emit (shared by build / synth)
 *     BkBuildOptions bo; BkBuild b;
 *     bk_build_options_init(&bo);
 *     bo.target = "arty";
 *     if (bk_build_run(&bo, &b) == 0)          // errors were reported
 *         bk_build_write(&b, 1);               // build/<target>/ + summary
 *     bk_build_free(&b);
 *
 *     // formal: SymbiYosys files for one module, results, counterexamples
 *     FvPlan plan;
 *     fv_generate(design, module, &fvopt, &plan);   // files + .sby text
 *     fv_parse_sby_log(&plan, log_text);            // PASS/FAIL per check
 *     fv_cex_to_test(&cex, &trace, &test_text);     // VCD trace -> Tern test
 *
 *     // synth: tool scripts and report parsers
 *     syn_script(&in, &script);
 *     syn_parse_vivado_util(text, &report);
 *
 * Nothing here requires an external tool: scripts and files are always
 * generated, and tools found on PATH (os_find_program) are run.
 */
#ifndef TERN_BACKEND_H
#define TERN_BACKEND_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../ir/ir.h"
#include "../emit/emit.h"

struct Driver;
struct TernTarget;

/* ================================================================ targets */

/* Device family: picks the Yosys synth command, the vendor tool, the
 * constraint format and the memory-init formats. */
typedef enum BkFamily {
    BK_FAM_GENERIC,     /* unknown device: generic Yosys `synth` */
    BK_FAM_XILINX,      /* xc7*, xcku*, xczu*, ... : Vivado / synth_xilinx, .xdc */
    BK_FAM_INTEL,       /* 5C*, 10M*, EP4C*, ... : Quartus, .sdc */
    BK_FAM_ICE40,       /* iCE40 (hx/lp/up): synth_ice40 (+ nextpnr-ice40) */
    BK_FAM_ECP5,        /* LFE5U*: synth_ecp5 (+ nextpnr-ecp5) */
    BK_FAM_GOWIN,       /* GW1N*, GW2A*: synth_gowin */
    BK_FAM_ASIC         /* ASIC: generic synth, no power-on values */
} BkFamily;

typedef enum BkTool { BK_TOOL_NONE, BK_TOOL_YOSYS, BK_TOOL_VIVADO, BK_TOOL_QUARTUS } BkTool;

/* A resolved build target: [target.<name>] of tern.toml with defaults.
 * Recognized keys besides device/top/backend/std (docs/backend.md):
 *   vendor = "xilinx" | "intel" | "lattice" | "ice40" | "ecp5" | "gowin" | "asic" | "generic"
 *   kind = "fpga" | "asic"                (default: asic for vendor "asic")
 *   constraints = "xdc,sdc" | "none" | ["xdc"]   (default: by family)
 *   mem_init = "mif,coe" | ["coe"]        (default: by family)
 *   features = "a,-b" | ["a", "-b"]       (applied before --features)
 *   clock_hz = 100_000_000                (synth timing goal; default: #[clock(hz)])
 *   package = "sg48"                      (nextpnr package), part = "..." (full part),
 *   family = "Cyclone V"                  (Quartus FAMILY) */
typedef struct BkTarget {
    const char *name;        /* "arty"; "default" when there is no target */
    const char *device;      /* NULL if unknown */
    const char *part;        /* full part name (default: device) */
    const char *package;     /* nextpnr package, or NULL */
    const char *family_name; /* Quartus family override, or NULL */
    const char *top;         /* NULL: #[top] detection */
    BkTool tool;             /* `backend` key; BK_TOOL_NONE: by family */
    BkFamily family;
    SvStd std;
    SvTargetKind kind;
    unsigned constraints;    /* EMIT_CONS_* */
    unsigned mem_init;       /* SV_MEMINIT_* */
    const char *features;    /* "a,-b" or NULL */
    uint64_t clock_hz;       /* 0: unknown */
    int from_manifest;       /* 1 if a [target] table exists */
} BkTarget;

BkFamily bk_family_from_device(const char *device);
const char *bk_family_name(BkFamily f);     /* "xilinx", "ice40", ... */
const char *bk_tool_name(BkTool t);         /* "yosys", "vivado", "quartus" */
int bk_tool_parse(const char *s, BkTool *out);   /* 0 / -1 */
/* The tool a family uses by default (Xilinx: Vivado, Intel: Quartus, else Yosys). */
BkTool bk_family_default_tool(BkFamily f);
/* Defaults for `name` (tt may be NULL: no [target] table).  Strings live in
 * `a`.  Invalid values are reported with diag_error (spans into tern.toml);
 * returns the number of errors. */
int bk_target_resolve(const struct TernTarget *tt, const char *name, BkTarget *out, Arena *a);

/* ================================================================== build */

typedef VEC(const char *) BkPathVec;

typedef struct BkBuildOptions {
    const char *const *paths;  /* files / directories (none: the project's src/) */
    size_t npaths;
    const char *target;        /* --target, or NULL */
    const char *top;           /* --top, or NULL */
    const char *features;      /* --features, or NULL */
    const char *out_dir;       /* -o, or NULL (build/<target>) */
    int std_set;               /* --std given */
    SvStd std;
    int emit_assertions;       /* --emit-assertions */
    int no_line_markers;       /* --no-line-markers */
    const char *cmd;           /* "build" / "synth" (messages) */
} BkBuildOptions;

void bk_build_options_init(BkBuildOptions *o);

typedef struct BkBuild {
    struct Driver *dr;
    Arena arena;               /* IR and target strings */
    BkTarget target;
    IrDesign **designs;        /* one per top (arena) */
    size_t ndesigns;
    SvOptions sv;
    SvOutput out;              /* every emitted file plus copied extern sources */
    char *root;                /* project root, or the current directory (heap) */
    char *out_dir;             /* output directory as used for writing (heap) */
    char *out_dir_display;     /* relative to the current directory (heap) */
    int usage_error;           /* bk_build_run failed because of the command line */
} BkBuild;

/* Elaborates the top(s), runs check_design and emits into b->out (nothing
 * is written).  Returns 0 on success; on failure the diagnostics were
 * reported (b->usage_error tells usage errors apart).  Always call
 * bk_build_free. */
int bk_build_run(const BkBuildOptions *o, BkBuild *b);
/* Writes b->out into b->out_dir (unchanged files keep their mtime) and, if
 * summary, prints what was written.  Returns 0 or -1 (reported). */
int bk_build_write(BkBuild *b, int summary);
void bk_build_free(BkBuild *b);

/* Rewrites absolute source paths in emitted text (line markers, assertion
 * messages, the CDC report): `<from>/x` becomes `<to>x` for each pair, in
 * order.  from[i] must not end in '/'. */
void bk_rewrite_paths(SvOutput *out, const char *const *from, const char *const *to, size_t n);

/* HDL files of an output in compile order (the package, extern module
 * sources, then the emitted modules children-first); names only. */
void bk_hdl_files(const SvOutput *out, BkPathVec *files);
/* 1 if `name` is the copied source of an extern module. */
int bk_is_extern_file(const SvOutput *out, const char *name);
/* Emitted name of the (first) top module (in b's arena), or NULL. */
const char *bk_build_top_name(BkBuild *b);

/* ============================================================== verilator */

/* `verilator --lint-only` over the HDL files in dir.  Returns 0 clean,
 * 1 lint errors, -1 verilator not installed (a message was printed). */
int vl_lint(const char *dir, const SvOutput *out, const char *top, SvStd std);

/* ================================================================= formal */

typedef enum FvStatus {
    FV_UNKNOWN,     /* not reported by the tool (yet) */
    FV_PASS,        /* assertion held (to the depth, or proven with --induct) */
    FV_FAIL,        /* counterexample found */
    FV_REACHED,     /* cover reached */
    FV_UNREACHED,   /* cover not reached within the depth */
    FV_SKIPPED      /* not translated (unsupported construct; see note) */
} FvStatus;

typedef struct FvCheck {
    IrAssertKind kind;        /* ASSERT (EXPECT counts as ASSERT), ASSUME, COVER */
    const char *module;       /* emitted module name holding the check */
    const char *inst_path;    /* "" for the proven module, "sub.fifo" below it */
    const char *name;         /* property name, or "assert" for immediate assertions */
    const char *msg;          /* assertion message, or NULL */
    Span loc;                 /* Tern source */
    const char *file;         /* generated file holding the check */
    uint32_t line;            /* line of the check in that file */
    FvStatus status;
    uint32_t step;            /* FAIL / REACHED: trace step */
    const char *trace;        /* FAIL / REACHED: VCD path relative to the formal dir */
    const char *note;         /* FV_SKIPPED: why */
} FvCheck;

typedef VEC(FvCheck) FvCheckVec;

typedef struct FvOptions {
    uint32_t depth;           /* default 20 */
    int induct;               /* add a k-induction `prove` task */
    const char *engine;       /* [engines] line; NULL: "smtbmc yices" */
    uint32_t reset_cycles;    /* cycles of reset at the start (default 2, as dut.reset()) */
} FvOptions;

void fv_options_init(FvOptions *o);

typedef struct FvPlan {
    Arena arena;
    const IrModule *module;   /* the module under proof */
    SvOutput files;           /* Verilog-2005 modules with checkers, the wrapper, <name>.sby */
    const char *name;         /* emitted module name = .sby stem */
    const char *wrapper;      /* "tern_formal_top" */
    int multiclock;           /* several top clocks */
    int has_cover;
    int has_assert;
    FvCheckVec checks;
    char *sby;                /* the .sby text (also in files) */
} FvPlan;

/* Generates the formal files for module m of design d (m and the modules
 * below it): Verilog-2005 modules each with a checker block appended
 * (immediate assertions with their path conditions, and the properties
 * compiled to monitor registers and immediate assert/assume/cover), a
 * wrapper top that holds the reset assumption, and the .sby file.  Returns
 * the number of errors (malformed IR). */
int fv_generate(const IrDesign *d, const IrModule *m, const FvOptions *o, FvPlan *plan);
void fv_plan_free(FvPlan *plan);

/* Applies the output of one sby task (its stdout or logfile.txt) to the
 * checks: "Assert failed in ...: <file>:<line>", "Reached cover statement
 * at <file>:<line> ... in step N", "Unreached cover statement ...", trace
 * file names and the final DONE status.  task is "bmc", "prove" or
 * "cover" (trace paths are prefixed with "<name>_<task>/").  Returns the
 * task status: FV_PASS, FV_FAIL or FV_UNKNOWN (error / induction failed). */
FvStatus fv_parse_sby_log(FvPlan *plan, const char *task, const char *log);

/* ----------------------------------------------------- VCD reader */

typedef struct VcdrVar {
    const char *id;           /* identifier code */
    const char *scope;        /* "top.dut" (dot-separated scopes) */
    const char *name;         /* reference name without the bit range */
    const char *full;         /* "top.dut.name" */
    uint32_t width;
    uint32_t code;            /* index into series (aliases share one) */
} VcdrVar;

typedef struct VcdrSeries {   /* the value changes of one identifier code */
    uint64_t *times;          /* ascending */
    const char **values;      /* MSB-first '0'/'1'/'x'/'z' digits, `width` long */
    size_t n;
    uint32_t width;
    const char *unknown;      /* `width` x digits (the value before the first change) */
} VcdrSeries;

typedef struct VcdrTrace {
    Arena arena;
    VcdrVar *vars;            /* in declaration order */
    size_t nvars;
    VcdrSeries *series;
    size_t nseries;
    uint64_t *times;          /* distinct timestamps, ascending */
    size_t ntimes;
    const char *timescale;    /* "1ps", or NULL */
} VcdrTrace;

/* Parses a VCD file.  Returns 0, or -1 with a heap message in *err. */
int vcdr_parse(const char *text, size_t len, VcdrTrace *out, char **err);
void vcdr_free(VcdrTrace *t);
/* Variable by name: "clk" (the shallowest match), "top.dut.clk" (full),
 * or "dut.clk" (a suffix of the full name).  -1 if absent. */
int vcdr_find(const VcdrTrace *t, const char *name);
/* Value of var at time (the last change at or before it), MSB first; a
 * string of 'x' before the first change.  Valid until vcdr_free. */
const char *vcdr_value_at(const VcdrTrace *t, int var, uint64_t time);
/* Times of the rising edges of a 1-bit variable (0 -> 1 or x -> 1). */
size_t vcdr_rising_edges(const VcdrTrace *t, int var, uint64_t **out_times);

/* ------------------------------------------ counterexample -> test */

typedef struct FvCex {
    const IrModule *module;   /* the proven module (ports) */
    const char *scope;        /* VCD scope of its ports ("tern_formal_top"); NULL: search */
    const char *test_name;    /* test "<name>" */
    const char *package;      /* `package` line of the test file, or NULL */
    const char *use_path;     /* `use` line ("uart::UartTx"), or NULL */
    const char *failing;      /* description of the failing check (comment) */
    const char *source_line;  /* the Tern source of the check, or NULL */
    uint32_t reset_cycles;    /* leading reset cycles replaced by dut.reset() (2) */
    uint32_t fail_step;       /* step at which the check fails (last driven step) */
    const char *clock;        /* clock variable for edge sampling, or NULL (the
                                 module's first clock port) */
    SvNames *names;           /* SV names of the ports in the trace (NULL: IR names) */
} FvCex;

/* Writes a Tern `test` that reproduces the trace: `sim M<..>()`,
 * `dut.reset()`, then per step the top inputs (except clocks and resets)
 * that changed and `dut.step()`.  The failing assertion or property is
 * checked by the simulator during the last step.  Returns 0, or -1 with a
 * heap message in *err (e.g. the ports are not in the trace). */
int fv_cex_to_test(const FvCex *cex, const VcdrTrace *trace, StrBuf *out, char **err);

/* ================================================================== synth */

typedef struct SynScriptIn {
    BkTool tool;
    BkFamily family;
    const char *device;       /* may be NULL (generic) */
    const char *part;         /* full part (Vivado), default device */
    const char *package;      /* nextpnr package */
    const char *family_name;  /* Quartus FAMILY */
    const char *top;          /* emitted top module name */
    const char *src_dir;      /* emitted files, relative to the script directory ("..") */
    const char *const *files; /* HDL file names in compile order */
    size_t nfiles;
    const char *xdc;          /* constraint file names (in src_dir), or NULL */
    const char *sdc;
    double clock_mhz;         /* 0: unknown */
    int sv;                   /* SystemVerilog sources (else Verilog-2005) */
} SynScriptIn;

/* The script file name for a tool: "synth.ys", "synth.tcl" (Vivado),
 * "synth_quartus.tcl". */
const char *syn_script_name(BkTool tool);
void syn_script(const SynScriptIn *in, StrBuf *out);
/* The command line that runs the script (in the synth directory), as text
 * for messages: "yosys -q -l synth.log -s synth.ys". */
void syn_command(BkTool tool, StrBuf *out);

typedef struct SynReport {
    int64_t luts, ffs, brams, dsps;   /* -1: unknown */
    int64_t cells;                    /* Yosys total cells, -1 unknown */
    double fmax_mhz;                  /* < 0: unknown */
    double wns_ns;                    /* valid when has_wns */
    int has_wns;
    double period_ns;                 /* constrained period, < 0 unknown */
} SynReport;

void syn_report_init(SynReport *r);
/* Each parser fills what it finds and returns the number of values found. */
int syn_parse_yosys_stat(const char *text, SynReport *r);
int syn_parse_nextpnr(const char *text, SynReport *r);
int syn_parse_vivado_util(const char *text, SynReport *r);
int syn_parse_vivado_timing(const char *text, SynReport *r);
int syn_parse_quartus_fit(const char *text, SynReport *r);
int syn_parse_quartus_sta(const char *text, SynReport *r);
/* The summary table; target_mhz 0 when unknown. */
void syn_report_print(const SynReport *r, double target_mhz, StrBuf *out);

#endif /* TERN_BACKEND_H */
