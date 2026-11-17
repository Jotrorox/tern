/* value.h — values of the compile-time interpreter and the elaborator.
 *
 * A Value is a small by-value record (32 bytes): scalars (integers that fit
 * in int64, bools) need no allocation; aggregates point to an arena list.
 * The same Value type flows through constant evaluation, elaboration
 * (where VAL_HW carries an IR expression of a hardware value) and the
 * `test` interpreter (where VAL_OPAQUE carries simulation handles).
 *
 *   VAL_INT     u.i, or u.big when (flags & VAL_F_BIG); ty is a widthless
 *               uint/sint (TY_INT) or a sized bit/uint/sint
 *   VAL_BOOL    u.i (0/1)
 *   VAL_STRING  u.s (interned or arena), aux = length
 *   VAL_ENUM    aux = variant index, u.list = payload field values or NULL
 *   VAL_STRUCT  u.list = field values (declaration order), ty = struct
 *   VAL_ARRAY   u.list = elements; ty = [T; N] (T may be widthless int for
 *               untyped literals)
 *   VAL_TUPLE   u.list = elements
 *   VAL_TYPE    u.type
 *   VAL_RANGE   u.list = {lo, hi}, aux = inclusive
 *   VAL_FN      u.fn: a fn item (+ explicit generic args, context env)
 *   VAL_ITEM    u.item: an item that still needs generic arguments or is
 *               used as a path prefix (module, struct, enum, bundle, regmap,
 *               type alias)
 *   VAL_PACKAGE u.pkg
 *   VAL_HW      u.e = IR expression; place = Place* when the expression is
 *               a direct reference to (part of) a signal
 *   VAL_BUNDLE  u.list = field values (missing flip fields of a literal are
 *               VAL_VOID); place = Place* for references
 *   VAL_INST    u.inst: a module instance
 *   VAL_MEM     u.mem: a memory
 *   VAL_DOMAIN  u.dom: a clock domain (fields .clock, .reset)
 *   VAL_MODULE  u.mod: a module with bound generic arguments (callable)
 *   VAL_OPAQUE  u.opq: host object with an ops vtable (simulation handles)
 *
 * Mutable locals own their aggregates: binding a value to a `var` copies
 * aggregate storage (value_copy_deep) so element updates can be in place.
 */
#ifndef TERN_VALUE_H
#define TERN_VALUE_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/bignum.h"
#include "../base/str.h"
#include "../sema/sema.h"

typedef enum ValKind {
    VAL_ERROR, VAL_VOID, VAL_INT, VAL_BOOL, VAL_STRING, VAL_ENUM, VAL_STRUCT, VAL_ARRAY,
    VAL_TUPLE, VAL_TYPE, VAL_RANGE, VAL_FN, VAL_ITEM, VAL_PACKAGE, VAL_HW, VAL_BUNDLE,
    VAL_INST, VAL_MEM, VAL_DOMAIN, VAL_MODULE, VAL_OPAQUE
} ValKind;

#define VAL_F_BIG 1u
/* VAL_STRING holding the name of an enum variant `.Name` whose enum type was
 * not known where it was written (test interpreter only: `[(.Add, 1)]`);
 * elab_convert to an enum type resolves it. */
#define VAL_F_VARIANT 2u

typedef struct Value Value;
typedef struct ValList ValList;
typedef struct ValFn ValFn;
typedef struct ValOpaque ValOpaque;
typedef struct Elab Elab;
struct Env;
struct Place;
struct ValInst;
struct ValMem;
struct ValDomain;
struct ValModule;

struct Value {
    uint8_t kind;         /* ValKind */
    uint8_t flags;        /* VAL_F_* */
    uint16_t pad;
    uint32_t aux;
    Ty *ty;
    union {
        int64_t i;
        BigInt *big;
        const char *s;
        ValList *list;
        Ty *type;
        ValFn *fn;
        Package *pkg;
        IrExpr *e;
        struct ValInst *inst;
        struct ValMem *mem;
        struct ValDomain *dom;
        struct ValModule *mod;
        ValOpaque *opq;
    } u;
    struct Place *place;
};

struct ValList {
    uint32_t n;
    Value *v;
};

struct ValFn {
    Item *item;           /* ITEM_FN (or the item of VAL_ITEM) */
    FileUnit *file;
    struct Env *ctx;      /* lexical context (module scope for module-local items), or NULL */
    Value *gargs;         /* explicit generic arguments (VAL_FN), or NULL */
    Ident *gnames;
    uint32_t ngargs;
    Ty *self_ty;          /* bundle method: the bundle type */
    Value *self;          /* bundle method: receiver */
    int builtin;          /* builtin function id (item == NULL), see consteval.c */
    Ty *bty;              /* builtin bound to a type (T::from_bits) */
    const char *name;     /* builtin / method name */
};

/* Host objects for the `test` interpreter. Every op returns 1 on success, 0
 * when it does not apply (the interpreter then reports a generic error),
 * -1 after reporting an error itself. */
typedef struct ValOpaqueOps {
    const char *type_name;
    int (*get_field)(Elab *el, void *obj, const char *name, Span at, Value *out);
    int (*set_field)(Elab *el, void *obj, const char *name, Value v, Span at);
    int (*index)(Elab *el, void *obj, Value idx, Span at, Value *out);
    int (*set_index)(Elab *el, void *obj, Value idx, Value v, Span at);
    int (*call_method)(Elab *el, void *obj, const char *name, const Value *args, const Ident *names,
                       uint32_t nargs, Span at, Value *out);
    void (*to_string)(void *obj, StrBuf *out);
} ValOpaqueOps;

struct ValOpaque {
    const ValOpaqueOps *ops;
    void *obj;
};

/* constructors (aggregates allocate in `a`) */
Value val_error(void);
Value val_void(Sema *S);
Value val_bool(Sema *S, int b);
Value val_int(Ty *ty, int64_t v);
Value val_int_bn(Arena *a, Ty *ty, const BigInt *v);      /* normalizes to small when it fits */
Value val_string(Sema *S, const char *s, uint32_t len);
Value val_type(Sema *S, Ty *t);
Value val_list(Arena *a, ValKind kind, Ty *ty, uint32_t n);  /* elements VAL_VOID-initialized */
Value val_enum(Arena *a, Ty *ty, uint32_t variant, const ValList *payload);
Value val_hw(Ty *ty, IrExpr *e);
Value val_opaque(Arena *a, const ValOpaqueOps *ops, void *obj, Ty *ty);

int val_is_const(const Value *v);   /* fully compile-time (no HW / instance / opaque inside) */
int val_is_int(const Value *v);
/* integer value into a BigInt (caller owns r; bn_free it) */
void val_get_bn(const Value *v, BigInt *r);
int val_get_i64(const Value *v, int64_t *out);            /* 1 if an int that fits */
int val_get_u64(const Value *v, uint64_t *out);
int val_truthy(const Value *v);                           /* bool value (const) */
/* structural equality of two compile-time values */
int val_equal(const Value *a, const Value *b);
/* deep copy of aggregate storage (for mutable locals) */
Value val_copy_deep(Arena *a, const Value *v);
/* "42", "true", "[1, 2]", "Pixel { r: 1, ... }", ".Idle", "uint<8>" */
void val_print(const Value *v, StrBuf *out);
/* canonical key fragment for caching / mangling: "u8", "64", "n3", "true" */
void val_mangle(const Value *v, StrBuf *out);
const char *val_kind_name(ValKind k);

#endif /* TERN_VALUE_H */
