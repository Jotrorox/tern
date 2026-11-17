/* elab.h — the compile-time interpreter and the elaborator (AST -> IR).
 *
 * One evaluator serves three purposes:
 *
 *   1. constant evaluation (const items, generic arguments and defaults,
 *      `where`, static_assert, const fn) — every Value is compile-time;
 *   2. elaboration of modules — the same evaluator, where values may be
 *      hardware (VAL_HW: an IR expression with its Tern type); `if`/`match`
 *      on hardware values become muxes, IR `if`/`case` statements or merges
 *      of local variables, `for` over compile-time ranges is unrolled, and
 *      fn calls are inlined;
 *   3. the `test` interpreter (src/sim/testrun.c, written later) — test
 *      bodies are statement lists run by elab_exec_block() with extra
 *      builtins supplied through ElabHooks and simulation handles as
 *      VAL_OPAQUE values (value.h, ValOpaqueOps).
 *
 * Running a test body (sketch for testrun.c):
 *
 *     Elab *el = elab_new(sema, NULL);          // no design: pure interpreter
 *     el->hooks.sim = my_sim;     // `sim M<..>(..)`: elab_eval_module() gives the
 *                                 // module + generic args; driver_elaborate_module()
 *                                 // builds the IrDesign; return val_opaque(...)
 *     el->hooks.call = my_call;   // rand<T>(), log(..), step helpers ...
 *     el->hooks.check = my_check; // expect / assert results
 *     Env *env = elab_env_new(el, NULL, file);  // file scope of the test
 *     elab_exec_block(el, item->u.test.body, env, NULL);
 *
 *   Assignments `dut.x = v` call ValOpaqueOps.set_field on the handle (also
 *   through nested fields/indices: `dut.push.valid = 1` gets the `push`
 *   field and sets `valid` on what it returns); reads call get_field /
 *   index; method calls (`dut.step(3)`) call call_method.
 *   `let x = e where c` is retried up to 1000 times until `c` holds (so
 *   rand<T>() from the call hook gives rejection sampling for free).
 *
 * Elaborating a module (the driver does this):
 *
 *     Elab *el = elab_new(sema, design);
 *     ModInfo *mi = elab_module(el, item, file, &args, call_span);
 *     // mi->ir is the IrModule (cached per (module, generic args)); children
 *     // were added to the design first
 *
 * Errors are reported through diag.h with the elaboration path
 * (diag_push_path) and never stop elaboration: a failed expression yields
 * VAL_ERROR, which later operations accept silently.
 */
#ifndef TERN_ELAB_H
#define TERN_ELAB_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/map.h"
#include "../base/vec.h"
#include "../sema/sema.h"
#include "../ir/ir.h"
#include "value.h"

/* ------------------------------------------------------- environments -- */

typedef struct Binding {
    const char *name;     /* interned */
    Value val;
    uint8_t lazy;         /* declaration not elaborated yet (module-level / local item) */
    uint8_t state;        /* lazy: 0 pending, 1 in progress, 2 done */
    uint8_t is_mut;       /* `var` local of a fn / const fn / test */
    uint8_t sem;          /* SemKind of the declaration */
    Stmt *decl;           /* lazy statement (module-level declaration) */
    Item *item;           /* lazy local item (const, fn, struct, ...) */
    struct Env *decl_env; /* scope of the lazy declaration */
    struct Env *owner;    /* the scope holding this binding */
    Span def;             /* declaration span */
} Binding;

typedef struct Env {
    struct Env *parent;
    FileUnit *file;       /* file scope for names the chain does not bind */
    struct ModCtx *mod;   /* module whose body this scope belongs to, or NULL */
    Binding *b;           /* heap array */
    uint32_t n, cap;
    const char *suffix;   /* appended to names declared here ("_0_1" in unrolled loops) */
    uint8_t is_fn;        /* fn body boundary */
    uint8_t keep;         /* module scopes: never freed while the Elab lives */
} Env;

Env *elab_env_new(Elab *el, Env *parent, FileUnit *file);
void el_env_keep(Elab *el, Env *e);              /* e lives until elab_free */
void elab_env_free(Env *e);                      /* frees the binding array (not values) */
Binding *elab_env_bind(Elab *el, Env *e, const char *name, Value v, Span def);
Binding *elab_env_find(Env *e, const char *name);   /* walks parents; NULL if unbound */

/* ------------------------------------------------------------- places -- */

typedef enum PlaceKind { PL_SIG, PL_AGG, PL_LOCAL, PL_DYN } PlaceKind;

typedef struct PlaceSel {     /* a step from a local variable to the assigned part */
    uint8_t kind;             /* 0 field/element index a, 1 dynamic index (idx), 2 bit slice [a +: b] */
    uint32_t a, b;
    Value idx;
} PlaceSel;

/* An assignable location. PL_SIG: bits [lo +: width] of signal `sig` of the
 * current module, or element `index` (elem_width bits each) of the array
 * starting at bit lo. PL_AGG: a bundle (subs[i] per field) or an array of
 * bundles (subs[i] per element). PL_LOCAL: (part of) a mutable local.
 * PL_DYN: the one of subs[0..n) (places of type `ty`) selected by the
 * hardware value `index` (a dynamic index into an array of bundles: reads
 * are a mux, writes a decode; an index >= n selects nothing). */
typedef struct Place {
    uint8_t kind;
    Ty *ty;
    uint32_t sig, lo, width;
    IrExpr *index;
    uint32_t elem_width;
    uint32_t len;             /* index: elements of the indexed array (writes past it are skipped) */
    const char *field;        /* readable field path for the IR ("carry", "pos.x") */
    uint32_t n;
    struct Place **subs;
    Binding *bind;
    uint32_t nsel;
    PlaceSel *sels;
} Place;

/* ---------------------------------------------------------- modules ---- */

typedef struct ElabArg {
    const char *name;         /* named generic argument, or NULL (positional) */
    Value val;                /* VAL_TYPE for type generics */
    Span span;
} ElabArg;

typedef struct ElabArgs {
    ElabArg *items;
    uint32_t n;
} ElabArgs;

typedef struct ModPort {
    const char *name;         /* interned Tern name */
    Ty *ty;
    PortDir dir;
    Place *place;             /* child-side place (signals of the child module) */
    uint32_t domain;          /* child domain index (`@ dom` or default), IR_NONE */
    int is_clock, is_reset;
    Span loc;
} ModPort;

/* One elaborated (module, generic arguments) pair. */
typedef struct ModInfo {
    Item *item;
    FileUnit *file;
    const char *key;          /* cache key */
    const char *display;      /* "Fifo<uint<8>, 4, 3>" */
    IrModule *ir;
    int state;                /* 0 new, 1 elaborating, 2 done */
    int failed;               /* errors while elaborating */
    ModPort *ports;
    uint32_t nports;
    struct Env *scope;        /* module scope (consts readable as `M<..>::NAME`) */
    uint32_t nclocks;         /* clock ports */
} ModInfo;

/* A module item with bound generic arguments (complete: defaults filled). */
typedef struct ValModule {
    Item *item;
    FileUnit *file;
    struct Env *genv;         /* generic bindings */
    ElabArgs args;            /* every generic, in declaration order */
    const char *key;          /* mangled name (Fifo_u8_64) */
    const char *cache_key;    /* unique key of (item, generic arguments) */
    const char *display;
    Span span;
} ValModule;

typedef struct ValInst {
    const char *name;         /* instance name in the parent */
    ModInfo *mi;
    uint32_t inst;            /* IrModule.insts index in the parent */
    Value *ports;             /* parent-side value of each port (mi->ports order) */
    uint32_t domain;          /* parent domain the instance runs in, or IR_NONE */
    Span loc;
} ValInst;

typedef struct ValMem {
    uint32_t sig;
    Ty *elem;
    uint32_t depth;
    uint32_t nreads;
    Span loc;
} ValMem;

typedef struct ValDomain {
    uint32_t idx;             /* IrModule.domains index */
} ValDomain;

/* ------------------------------------------------------------ hooks ---- */

typedef struct ElabHooks {
    void *user;
    /* `sim M<..>(..)` (EXPR_SIM): return 1 and set *out when handled. */
    int (*sim)(Elab *el, Expr *e, Env *env, Value *out);
    /* call of a name that is not bound anywhere (rand, log, ...), with
     * evaluated arguments: return 1 when handled, 0 to report "cannot find". */
    int (*call)(Elab *el, Expr *call, const char *name, const Value *args, const Ident *names,
                uint32_t nargs, Env *env, Value *out);
    /* `assert` / `expect` outside hardware blocks (tests): return 1 when
     * handled; otherwise a false assertion is a compile-time error. */
    int (*check)(Elab *el, Stmt *s, int passed, Env *env);
    /* const_log / log output (default: printed to stderr as a note). */
    void (*log)(Elab *el, Span at, const char *text);
} ElabHooks;

/* ------------------------------------------------------------- Elab ---- */

struct Elab {
    Sema *S;
    Arena arena;              /* values, places, environments' module scopes */
    IrDesign *design;         /* target design (NULL: interpreter only) */
    TyIrCache tycache;
    Map modules;              /* interned key -> ModInfo* */
    Map modtypes;             /* Item* (struct/enum) cache helper */
    uint64_t steps, step_limit;
    int limit_hit;            /* the step limit was reported (abort evaluation) */
    ElabHooks hooks;
    struct ModCtx *mod;       /* module being elaborated */
    struct BlkCtx *blk;       /* statement sink of the current comb/on block */
    int in_fn;                /* inside an inlined fn body */
    uint32_t depth;           /* call depth */
    VEC(Item *) hw_calls;     /* fns being inlined with hardware arguments */
    VEC(Env *) kept;          /* scopes that live as long as the Elab (freed by elab_free) */
    /* undo log of assignments to mutable locals (merging under hardware conditions) */
    VEC(struct UndoEntry) undo;
    uint32_t undo_depth;
    int returned;             /* a `return` is propagating */
    Value ret;
    /* `return` under conditions on signals in a fn inlined on signals: the
     * condition (relative to the fn's entry) of the arm being run, and the
     * merged result of the returns seen so far (taken when ret_cond holds) */
    int fn_active;            /* inside el_call_fn */
    Value fn_path;            /* path condition of the current arm (bool true at the fn level) */
    int fn_ret_have;
    Value fn_ret_cond, fn_ret_val;
    Ty *fn_ret_ty;
    Ty *tail_expect;          /* expected type of the next block's tail value */
    struct BlkCtx *temp_blk;  /* block receiving intermediate values (see el_maybe_temp) */
    Map seen_errors;          /* (span, message) hashes: report each error once */
    int quiet;                /* suppress diagnostics (speculative evaluation) */
    int quiet_errors;         /* errors suppressed while quiet */
    int check_strict;
    const char *root_frame;   /* elaboration path of the current root */
    void *user;
    /* method call on a VAL_OPAQUE being dispatched (set just before
     * ValOpaqueOps.call_method): lets a host re-evaluate an argument lazily,
     * e.g. the condition of `dut.step_until(cond)` */
    Expr *opq_call;
    struct Env *opq_env;
};

typedef struct UndoEntry {
    Env *env;             /* scope of the assigned local */
    const char *name;
    Value old;
} UndoEntry;

Elab *elab_new(Sema *S, IrDesign *design);
void elab_free(Elab *el);
void elab_set_step_limit(Elab *el, uint64_t limit);   /* default 10^7 */

/* Expressions. `expect` (may be NULL) is the type expected by the context:
 * it types `.Variant`, all0/all1, untyped literals and struct literals. */
Value elab_eval(Elab *el, Expr *e, Env *env, Ty *expect);
/* Same, but reports an error unless the result is compile-time. */
Value elab_eval_const(Elab *el, Expr *e, Env *env, Ty *expect, const char *what);
Ty *elab_eval_type(Elab *el, Type *t, Env *env);
/* Runs a block (test body, const fn body); *result gets its tail value. */
void elab_exec_block(Elab *el, Block *b, Env *env, Value *result);
/* Calls a fn value (generic arguments inferred from args). */
Value elab_call(Elab *el, Value fn, const Value *args, const Ident *names, uint32_t nargs, Span at);
/* Implicit conversion (widening, literal fitting) with the spec's messages.
 * `src` (may be NULL) is the expression of v; `what` names the target,
 * e.g. "`y`". */
Value elab_convert(Elab *el, Value v, Ty *to, Expr *src, const char *what, Span at);
/* Evaluates a module path (`Fifo<u8, 4>`, `std::fifo::Fifo`) to a module
 * with all generics bound; NULL after reporting an error. */
ValModule *elab_eval_module(Elab *el, Expr *callee, Env *env);
/* Binds generic arguments of a module item (defaults, `where`). */
ValModule *elab_bind_module(Elab *el, Item *item, FileUnit *file, const ElabArgs *args, Span at);
/* Elaborates (or fetches from the cache) a module into el->design. */
ModInfo *elab_module(Elab *el, ValModule *vm, Span at);
/* The value of a package-level symbol (const, fn, type, module item ...). */
Value elab_symbol_value(Elab *el, const Symbol *sym, Span at);
/* Compile-time value -> packed bits of type t (struct, enum, array, ints). */
IrBits elab_const_bits(Elab *el, const Value *v, Ty *t);
/* Packed bits -> compile-time value of type t. */
Value elab_value_from_bits(Elab *el, const IrBits *b, Ty *t);
/* Debug check of the IR invariants of a whole design; reports violations
 * through tern_fatal-free diag_error and returns their count. */
int elab_ir_verify(const IrDesign *d);
/* Regmaps of a file are added to the design (IrRegmap). */
void elab_add_regmap(Elab *el, Item *regmap, FileUnit *file);

#endif /* TERN_ELAB_H */
