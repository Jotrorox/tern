/* elab_impl.h — internals shared by consteval.c, elab.c and mono.c (not
 * part of the public API; see elab.h). */
#ifndef TERN_ELAB_IMPL_H
#define TERN_ELAB_IMPL_H

#include "elab.h"
#include "../base/diag.h"

typedef struct VarDefault {
    uint32_t sig;
    Value val;
    Span loc;
} VarDefault;

typedef struct PendingIn {
    uint32_t sig;             /* parent wire feeding an instance input */
    ValInst *inst;
    const char *port;         /* "push.valid" */
    int assigned;
} PendingIn;

typedef struct MemWrite {     /* recorded for read-during-write forwarding */
    uint32_t mem;
    IrExpr *cond, *addr, *data;
} MemWrite;

typedef struct MemReadNew {
    uint32_t mem, reg;
    IrExpr *addr;
    uint32_t proc;
} MemReadNew;

typedef struct ModCtx {
    ModInfo *mi;
    ValModule *vm;
    IrModule *ir;
    Env *scope;
    FileUnit *file;
    Item *item;
    int is_top, is_extern;
    VEC(uint32_t) clock_ports;
    VEC(uint32_t) reset_ports;
    Map sig_comb;             /* sig -> comb process index + 1 (driver) */
    Map sig_driven;           /* sig -> 1 when driven anywhere */
    VEC(VarDefault) var_defaults;
    Map pending;              /* sig -> index into pendings + 1 */
    VEC(PendingIn) pendings;
    VEC(MemWrite) memwrites;
    VEC(MemReadNew) memreads_new;
    uint32_t ninline;         /* inline instances */
    int attr_cdc, attr_clock_gen;
    Map inst_out;             /* sig -> 1: wire driven by an instance output */
    Map let_wire;             /* sig -> 1: module-level let */
    int in_property;
    uint32_t nunsafe;
} ModCtx;

typedef struct BlkCtx {
    int clocked;
    uint32_t domain;
    IrStmtList *out;
    VEC(uint32_t) assigned;   /* signals assigned in this block */
    Span loc;
} BlkCtx;

/* consteval.c */
void el_error(Elab *el, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
void el_help(Elab *el, const char *fmt, ...) TERN_PRINTF(2, 3);
void el_note(Elab *el, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
void el_warning(Elab *el, Span at, const char *fmt, ...) TERN_PRINTF(3, 4);
int el_step(Elab *el, Span at);
const char *el_text(const Expr *e);             /* source text (truncated) */
const char *el_span_text(Span s);
IrType *el_irty(Elab *el, Ty *t);
Value el_hw(Elab *el, Ty *t, IrExpr *e);
IrExpr *el_to_ir(Elab *el, const Value *v, Span at);   /* v must have a hardware type */
Value el_zero(Elab *el, Ty *t);
Value el_fill(Elab *el, Ty *t, int ones, Span at);
Value el_read_place(Elab *el, Place *p);
Value el_lookup_name(Elab *el, Env *env, const char *name, Span at, int report);
Value el_force(Elab *el, Binding *b);
Value el_item_value(Elab *el, Item *it, FileUnit *file, Env *ctx, Span at);
Value el_apply_generics(Elab *el, Value base, PathSeg *seg, Env *env, Ty *expect);
Value el_member(Elab *el, Value base, const char *name, Span at, Env *env);
Value el_field(Elab *el, Value base, const char *name, Span at);
Value el_index(Elab *el, Value base, Value idx, Span at, Expr *idx_expr);
Value el_cast(Elab *el, Value v, Ty *to, Span at, Expr *src);
Value el_binary(Elab *el, BinaryOp op, Value a, Value b, Span at, Expr *ea, Expr *eb);
Value el_unify_mux(Elab *el, Value c, Value t, Value f, Ty *expect, Span at);
Ty *el_common_type(Elab *el, Value *a, Value *b, Span at);
void el_bind_pattern(Elab *el, Pattern *p, Value v, Env *env, int is_mut);
Value el_match_test(Elab *el, Pattern *p, Value subj, Env *env);   /* bool (const or hw) */
void el_assign_local(Elab *el, Place *p, Value v, Span at);
void el_log_assign(Elab *el, Binding *b, Env *env);
void el_unify_type(Elab *el, Type *pt, Ty *at, Env *genv);
Value el_call_fn(Elab *el, ValFn *fn, const Value *args, const Ident *names, Expr *const *argexprs,
                 uint32_t nargs, Span at, Ty *expect);
Place *el_place_of(Elab *el, Expr *e, Env *env);
Value el_mk_bool(Elab *el, int b);
Value el_not(Elab *el, Value v, Span at);
Value el_prop_reset(Elab *el, Value v, Span at);
Value el_and(Elab *el, Value a, Value b, Span at);
Value el_or(Elab *el, Value a, Value b, Span at);
int el_bits_of_const(Elab *el, const Value *v, Ty *t, BigInt *out);  /* packed value */
int el_fits(const BigInt *b, Ty *t);
void el_range_text(Ty *t, StrBuf *out);
Value el_call_value(Elab *el, Value fn, Expr *call, Env *env, Ty *expect);
void el_exec_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env, Expr *tail, Value *tail_out);
void el_exec_stmt(Elab *el, Stmt *s, Env *env);
Value el_eval_block(Elab *el, Block *b, Env *env, Ty *expect);
void el_record_use(Elab *el, Span at, Binding *b, const char *name);
void el_record_decl(Elab *el, Span at, const char *name, SemKind k, Ty *t, const char *domain,
                    const Value *val);

/* elab.c */
void el_assign(Elab *el, Place *p, Value v, Expr *src, Expr *lhs, Span at);
Value el_force_decl(Elab *el, Binding *b);
Value el_instance(Elab *el, Expr *call, Env *env, const char *name, Span at, Value calleev);
Value el_inst_field(Elab *el, Value inst, const char *name, Span at);
Value el_mem_method(Elab *el, Value mem, const char *name, Expr *call, Env *env);
Value el_mem_index(Elab *el, Value mem, Value idx, Span at);
Value el_domain_field(Elab *el, Value dom, const char *name, Span at);
uint32_t el_domain_of(Elab *el, Expr *e, Env *env, Span at);
Value el_temporal_call(Elab *el, const char *name, Expr *call, Env *env);
void el_mod_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env);
Value el_unsafe_cross(Elab *el, Value v, Span at);
int el_is_signal_place(const Place *p);

Value el_eval_path(Elab *el, Path *p, Env *env, Ty *expect, int type_ctx);
Binding *ce_lookup_local_binding(Env *e, const char *name);   /* this scope only */

/* mono.c */
ValModule *el_bind_module_ast(Elab *el, Item *item, FileUnit *file, GenericArg *args, uint32_t nargs, Env *argenv,
                              Span at);
int el_item_needs_args(Item *it);
const char *el_mangle_module(Elab *el, Item *item, const ElabArgs *args);
/* Binds generic parameters into a new environment (child of ctx, file
 * scope `file`): AST arguments (evaluated in argenv) or values, then
 * defaults. Returns NULL after reporting an error. */
Env *el_bind_generics(Elab *el, GenericParams *gp, const char *owner, FileUnit *file, Env *ctx,
                      GenericArg *args, uint32_t nargs, Env *argenv, const ElabArgs *vals, Span at,
                      ElabArgs *out);
Ty *el_type_item(Elab *el, Item *it, FileUnit *file, Env *ctx, GenericArg *args, uint32_t nargs,
                 Env *argenv, Span at, Ty *expect);
Ty *el_type_item_vals(Elab *el, Item *it, FileUnit *file, Env *ctx, const ElabArgs *args, Span at);
int el_check_where(Elab *el, WhereList *w, Env *genv, const char *owner, Span at);
const char *el_ctx_prefix(Env *ctx);
Ty *el_regmap_type(Elab *el, Item *regmap, FileUnit *file, const char *name, Span at);

#endif /* TERN_ELAB_IMPL_H */
