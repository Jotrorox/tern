/* ast.h — the Tern abstract syntax tree.
 *
 * One AstFile per source file; every node lives in the arena passed to
 * parse_file() and carries a Span (byte offsets plus 1-based line/col of its
 * first byte). Names are interned strings (compare with ==) paired with
 * their span (Ident). Lists are `T **items` (or `T *items` for small value
 * records) plus a uint32_t count; an empty list is {NULL, 0}.
 *
 * Nodes are tagged unions: `node->kind` selects the member of `node->u`,
 * named after the kind (EXPR_BINARY -> e->u.binary, STMT_REG -> s->u.reg,
 * ITEM_MODULE -> it->u.module, ...).
 *
 * Required children are never NULL: a syntax error yields EXPR_ERROR /
 * TYPE_ERROR / PAT_ERROR / STMT_ERROR nodes, so later phases can walk a
 * broken tree (they should stay quiet about error nodes: the parser has
 * reported them). Optional children are NULL when absent (documented per
 * field).
 *
 * Shared design points (see docs/grammar.md for the concrete syntax):
 *  - Module bodies, blocks, fn bodies, test bodies, `on`/`comb` bodies and
 *    the bodies of elaboration-time `if`/`for` are all statement lists of the
 *    same Stmt type; which statements are legal where is checked later.
 *  - `if` and `match` are expressions (EXPR_IF / EXPR_MATCH) everywhere; in
 *    statement position they appear as STMT_EXPR. An elaboration `if` at
 *    module level is such an STMT_EXPR whose blocks hold module statements.
 *  - A Block keeps all its statements in `stmts`; `tail` additionally points
 *    at the expression of the last statement when that statement is an
 *    STMT_EXPR not followed by `;` (the block's value). Statement-list
 *    consumers just iterate `stmts`.
 *  - Instances and function calls are both EXPR_CALL (sema decides);
 *    `sim M<..>(..)` is EXPR_SIM around the EXPR_CALL; `@ dom` after an
 *    instance is EXPR_CALL.domain.
 *  - Generic arguments (GenericArg) keep both readings: `expr` always, and
 *    `type` when the argument is also syntactically a type (`u8`,
 *    `Stream<T>`, `[T; N]`, `struct {..}`, `typeof(x)`, tuples of those).
 *    A type generic uses `type`, a value generic uses `expr`.
 *  - Built-in types (uint, sint, bit, bool, clock, reset, tri, u8...) are
 *    ordinary TYPE_PATHs; `uint<8>`, `clock<falling>`, `reset<async, low>`,
 *    `tri<T>` are paths with generic arguments (`async` is accepted there).
 */
#ifndef TERN_AST_H
#define TERN_AST_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/diag.h"
#include "token.h"

typedef struct Expr Expr;
typedef struct Type Type;
typedef struct Pattern Pattern;
typedef struct Stmt Stmt;
typedef struct Item Item;
typedef struct Block Block;

typedef struct Ident {
    const char *name;     /* interned; NULL only inside error nodes */
    Span span;
} Ident;

/* `/// ...` block attached to an item, port, field, variant or statement. */
typedef struct AstDoc {
    const char *text;     /* lines without `///` and one leading space, joined by '\n' */
    Span span;
} AstDoc;

/* ------------------------------------------------------------ attributes */

typedef struct AttrArg {
    Ident name;           /* `hz` in #[clock(hz: 100)]; name.name == NULL when positional */
    Expr *value;          /* `block` in #[ram(block)] is an EXPR_PATH */
    Span span;
} AttrArg;

typedef struct Attr {
    Ident name;           /* ram, keep, clock, encoding, cdc, async_reg, top, test_port, ... */
    int has_args;         /* `#[name(...)]` (possibly empty parens) vs `#[name]` */
    AttrArg *args;
    uint32_t nargs;
    Span span;            /* from `#[` to `]` */
} Attr;

typedef struct AttrList {
    Attr *items;
    uint32_t n;
} AttrList;

/* ------------------------------------------------------- paths, generics */

typedef struct GenericArg {
    Ident name;           /* named argument `Depth: 64`; name.name == NULL when positional */
    Expr *expr;           /* always set (EXPR_TYPE for type-only forms such as `[u8; 4]`) */
    Type *type;           /* set when the argument also reads as a type; else NULL */
    Span span;
} GenericArg;

typedef struct PathSeg {
    Ident name;
    int has_args;         /* `<...>` present (may be empty) */
    GenericArg *args;
    uint32_t nargs;
    Span span;            /* name plus its `<...>` */
} PathSeg;

/* a, a::b::C, Fifo<u8, 64>, Cordic<16>::GAIN_COMP, std::bus::RegisterFile<Ctrl> */
typedef struct Path {
    PathSeg *segs;        /* nsegs >= 1 */
    uint32_t nsegs;
    Span span;
} Path;

typedef enum GenericKind {
    GENERIC_TYPE,         /* T: type [= Type] */
    GENERIC_VALUE         /* N: uint [= expr], Policy: ArbPolicy = .RoundRobin, Data: [T; N] */
} GenericKind;

typedef struct GenericParam {
    GenericKind kind;
    Ident name;
    Type *type;           /* GENERIC_VALUE: the value's type; NULL for GENERIC_TYPE */
    Type *default_type;   /* GENERIC_TYPE default, or NULL */
    Expr *default_value;  /* GENERIC_VALUE default, or NULL */
    Span span;
} GenericParam;

typedef struct GenericParams {
    int present;          /* `<...>` was written */
    GenericParam *items;
    uint32_t n;
    Span span;
} GenericParams;

/* ------------------------------------------------------------------ types */

typedef enum TypeKind {
    TYPE_ERROR,
    TYPE_PATH,            /* u.path: uint<8>, Stream<T>, Ctrl::Status, T, clock<falling> */
    TYPE_ARRAY,           /* u.array: [T; N] */
    TYPE_STRUCT,          /* u.anon_struct: struct { data: uint<DW>, strb: bit<DW/8> } */
    TYPE_TYPEOF,          /* u.typeof_expr: typeof(expr) */
    TYPE_TUPLE            /* u.tuple: (A, B) */
} TypeKind;

typedef struct Field {    /* struct / anonymous struct / bundle field */
    Ident name;
    Type *type;
    int flip;             /* bundle fields only: `flip ready: bool` */
    AttrList attrs;
    AstDoc *doc;          /* or NULL */
    Span span;
} Field;

struct Type {
    TypeKind kind;
    Span span;
    union {
        Path path;
        struct { Type *elem; Expr *len; } array;
        struct { Field **fields; uint32_t nfields; } anon_struct;
        Expr *typeof_expr;
        struct { Type **elems; uint32_t n; } tuple;
    } u;
};

/* ------------------------------------------------------------ expressions */

typedef enum ExprKind {
    EXPR_ERROR,
    EXPR_INT,             /* u.int_lit */
    EXPR_BOOL,            /* u.bool_val: true / false */
    EXPR_FILL,            /* u.fill_ones: all0 (0) / all1 (1) */
    EXPR_STRING,          /* u.string */
    EXPR_PATH,            /* u.path: a, T::WIDTH, Fifo<u8, 64>, std::x::f */
    EXPR_VARIANT,         /* u.variant: .Idle, .Write { addr: 1, data } */
    EXPR_STRUCT,          /* u.struct_lit: Stream { valid: true, data: x }, Cmd::Write { .. } */
    EXPR_FIELD,           /* u.field: x.f, x.f<8> (method generics, used by calls) */
    EXPR_INDEX,           /* u.index: x[i] */
    EXPR_SLICE,           /* u.slice: x[hi:lo] */
    EXPR_CALL,            /* u.call: f(a, b: c), Fifo<u8>(push: x) @ dom, x.fire() */
    EXPR_CAST,            /* u.cast: x as T */
    EXPR_UNARY,           /* u.unary */
    EXPR_BINARY,          /* u.binary (incl. `in` membership and temporal |-> |=> until) */
    EXPR_RANGE,           /* u.range: a..b, a..=b, ..b (lo NULL) */
    EXPR_IF,              /* u.if_expr */
    EXPR_MATCH,           /* u.match */
    EXPR_BLOCK,           /* u.block */
    EXPR_TUPLE,           /* u.list: (a, b) ; () is a tuple with n == 0 */
    EXPR_ARRAY,           /* u.list: [a, b, c] */
    EXPR_PAREN,           /* u.inner: (e) — kept for the formatter; semantically e */
    EXPR_SIM,             /* u.inner: sim M<..>(..) — inner is the EXPR_CALL */
    EXPR_TYPE,            /* u.type: a type in expression position ([u8; 4], struct {..}) */
    EXPR_TEMPORAL,        /* u.temporal: always e, never e, eventually[..n] e */
    EXPR_DELAY,           /* u.delay: a ##n b, a ##[lo:hi] b, a ##[1:$] b, ##1 b */
    EXPR_QPATH            /* u.qpath: a path on a type that is not a name:
                           * typeof(x)::WIDTH, [u8; 4]::WIDTH (§9.4) */
} ExprKind;

typedef enum UnaryOp {
    UN_NEG,               /* -x */
    UN_NOT,               /* !x, not x */
    UN_BITNOT,            /* ~x */
    UN_RED_AND,           /* &x */
    UN_RED_OR,            /* |x */
    UN_RED_XOR            /* ^x */
} UnaryOp;

typedef enum BinaryOp {
    BIN_MUL, BIN_DIV, BIN_MOD,
    BIN_ADD, BIN_SUB, BIN_ADD_WRAP, BIN_SUB_WRAP,   /* + - +% -% */
    BIN_SHL, BIN_SHR, BIN_SHRA,                     /* << >> >>> */
    BIN_BITAND, BIN_BITXOR, BIN_BITOR,              /* & ^ | */
    BIN_CONCAT,                                     /* ++ */
    BIN_EQ, BIN_NE, BIN_LT, BIN_LE, BIN_GT, BIN_GE,
    BIN_IN,                                         /* x in [a, b] (rhs usually EXPR_ARRAY) */
    BIN_AND, BIN_XOR, BIN_OR,                       /* and/&&, xor, or/|| */
    BIN_UNTIL,                                      /* temporal: a until b */
    BIN_IMPLIES,                                    /* temporal: a |-> b (same cycle) */
    BIN_IMPLIES_NEXT                                /* temporal: a |=> b (next cycle) */
} BinaryOp;

typedef enum TemporalOp {
    TEMP_ALWAYS,
    TEMP_NEVER,
    TEMP_EVENTUALLY       /* bound = n in eventually[..n], NULL when unbounded */
} TemporalOp;

typedef struct FieldInit {  /* in struct / variant literals */
    Ident name;
    Expr *value;          /* NULL for the shorthand `{ data }` (means `data: data`) */
    Span span;
} FieldInit;

typedef struct Arg {        /* call / instance argument */
    Ident name;           /* `push` in push: x; name.name == NULL when positional */
    Expr *value;
    Span span;
} Arg;

typedef struct MatchArm {
    Pattern *pat;         /* PAT_OR for `a | b` */
    Stmt *body;           /* `=> expr` is an STMT_EXPR (value arms read body->u.expr),
                           * `=> { .. }` an STMT_EXPR of an EXPR_BLOCK,
                           * `=> x = 1` an STMT_ASSIGN */
    Span span;
} MatchArm;

struct Block {
    Stmt **stmts;
    uint32_t nstmts;
    Expr *tail;           /* value of the block: == stmts[nstmts-1]->u.expr when the block ends
                           * with an expression statement without `;`, else NULL */
    Span span;            /* from `{` to `}` */
};

struct Expr {
    ExprKind kind;
    Span span;
    union {
        struct {
            const char *text;     /* digits as written, incl. 0x/0b/0o prefix and `_` (bn_parse(.., 0)) */
            uint64_t value;       /* the value when !overflow */
            int overflow;         /* value does not fit in 64 bits: use text */
            int base;             /* 2, 8, 10, 16 */
            char suffix;          /* 0, 'u' or 's' (typed literal 42u8 / 3s12 / 0xFFu<W>) */
            Expr *width;          /* typed literal width (EXPR_INT for 42u8, any expr for u<W>), else NULL */
        } int_lit;
        int bool_val;
        int fill_ones;
        struct { const char *value; uint32_t len; } string;   /* decoded, NUL-terminated */
        Path path;
        struct {
            Ident name;           /* without the dot */
            int has_fields;       /* `.Write { ... }` or `.Weighted<[3, 1, 2]>` */
            int angle;            /* payload written as generic-style arguments
                                   * `.Weighted<[3, 1, 2]>`: fields may be positional
                                   * (name.name == NULL), in payload field order */
            FieldInit *fields;
            uint32_t nfields;
        } variant;
        struct {
            Path path;            /* type or Type::Variant */
            FieldInit *fields;
            uint32_t nfields;
            int has_rest;         /* trailing `..` */
        } struct_lit;
        struct {
            Expr *base;
            Ident name;
            int has_args;         /* x.trunc<8>() */
            GenericArg *args;
            uint32_t nargs;
        } field;
        struct { Expr *base; Expr *index; } index;
        struct { Expr *base; Expr *hi; Expr *lo; } slice;
        struct {
            Expr *callee;         /* EXPR_PATH (fn or module, generic args in the path), EXPR_FIELD (method) */
            Arg *args;
            uint32_t nargs;
            Expr *domain;         /* `@ dom` after an instance, or NULL */
        } call;
        struct { Expr *expr; Type *type; } cast;
        struct { UnaryOp op; Expr *operand; } unary;
        struct { BinaryOp op; Expr *lhs; Expr *rhs; } binary;
        struct { Expr *lo; Expr *hi; int inclusive; } range;     /* lo may be NULL */
        struct {
            Expr *cond;
            Block *then_block;
            Expr *else_expr;      /* NULL, an EXPR_BLOCK, or an EXPR_IF (else if) */
        } if_expr;
        struct { Expr *scrutinee; MatchArm *arms; uint32_t narms; } match;
        Block *block;
        struct { Expr **items; uint32_t n; } list;
        Expr *inner;
        Type *type;
        struct { TemporalOp op; Expr *bound; Expr *body; } temporal;
        struct {
            Expr *lhs;            /* NULL for a leading delay `##1 b` */
            Expr *lo;             /* ##n: lo = n, hi = NULL; ##[a:b]: lo = a, hi = b */
            Expr *hi;
            int unbounded;        /* ##[a:$] */
            int ranged;           /* written with brackets */
            Expr *rhs;
        } delay;
        struct {
            Type *type;           /* TYPE_TYPEOF, TYPE_ARRAY, TYPE_STRUCT or TYPE_TUPLE */
            Path path;            /* the members after `::` (nsegs >= 1) */
        } qpath;
    } u;
};

/* --------------------------------------------------------------- patterns */

typedef enum PatternKind {
    PAT_ERROR,
    PAT_WILDCARD,         /* _ */
    PAT_BIND,             /* u.bind: x (a single identifier; sema may resolve it to a const) */
    PAT_LIT,              /* u.lit: 3, -1, 0xFFu8, true, "s" (an expression) */
    PAT_RANGE,            /* u.range: 0..9, 'a'..=b (bounds are expressions) */
    PAT_VARIANT,          /* u.variant: .Idle, .Write { addr, .. } */
    PAT_PATH,             /* u.path: Op::Add, consts::X (two or more segments) */
    PAT_STRUCT,           /* u.struct_pat: Fifo { pop, count }, Cmd::Write { addr: a, .. } */
    PAT_TUPLE,            /* u.list: (a, _, .Idle) */
    PAT_OR                /* u.list: p | q */
} PatternKind;

typedef struct FieldPat {
    Ident name;
    Pattern *pat;         /* NULL for the shorthand `{ addr }` (binds addr) */
    Span span;
} FieldPat;

struct Pattern {
    PatternKind kind;
    Span span;
    union {
        Ident bind;
        Expr *lit;
        struct { Expr *lo; Expr *hi; int inclusive; } range;
        struct {
            Ident name;
            int has_fields;
            FieldPat *fields;
            uint32_t nfields;
            int has_rest;         /* `..` */
        } variant;
        Path path;
        struct {
            Path path;
            FieldPat *fields;
            uint32_t nfields;
            int has_rest;
        } struct_pat;
        struct { Pattern **items; uint32_t n; } list;
    } u;
};

/* ------------------------------------------------------------- statements */

typedef enum StmtKind {
    STMT_ERROR,
    STMT_PORT,            /* u.port: in name: T [@ dom] */
    STMT_DOMAIN,          /* u.domain: domain sys = (clk, rst) */
    STMT_LET,             /* u.let: let pat [: T] = e [where c] */
    STMT_VAR,             /* u.var: var x [: T] [= e] [where c] */
    STMT_REG,             /* u.reg: reg x: T [= init] [@ dom] */
    STMT_MEM,             /* u.mem: mem m: [T; N] [= init] */
    STMT_ITEM,            /* u.item: nested const, fn, struct, enum, type, bundle, property, sequence */
    STMT_COMB,            /* u.comb: comb { ... } */
    STMT_ON,              /* u.on: on clk { ... } */
    STMT_ASSIGN,          /* u.assign: lhs = rhs [where c] */
    STMT_EXPR,            /* u.expr: if/match/block/call/... in statement position */
    STMT_FOR,             /* u.for_stmt: for pat in e { ... } */
    STMT_WHILE,           /* u.while_stmt: while c { ... } */
    STMT_ASSERT,          /* u.assert_stmt: assert c[, msg] / expect c[, msg] */
    STMT_STATIC_ASSERT,   /* u.sassert: static_assert(c[, msg]) */
    STMT_RETURN           /* u.ret: return [e] (value may be NULL) */
} StmtKind;

typedef enum PortDir { PORT_IN, PORT_OUT } PortDir;
typedef enum AssertKind { ASSERT_ASSERT, ASSERT_EXPECT } AssertKind;

struct Stmt {
    StmtKind kind;
    Span span;
    AttrList attrs;       /* #[...] written before (ports: also after the type) */
    AstDoc *doc;          /* or NULL */
    union {
        struct { PortDir dir; Ident name; Type *type; Expr *domain; } port;   /* domain may be NULL */
        struct { Ident name; Expr *clock; Expr *reset; } domain;               /* reset may be NULL */
        struct { Pattern *pat; Type *type; Expr *init; Expr *constraint; } let; /* type, constraint may be NULL */
        struct { Ident name; Type *type; Expr *init; Expr *constraint; } var;  /* all but name optional */
        struct { Ident name; Type *type; Expr *init; Expr *domain; } reg;      /* init, domain may be NULL */
        struct { Ident name; Type *type; Expr *init; } mem;                    /* init may be NULL */
        Item *item;
        Block *comb;
        struct { Expr *clock; Block *body; } on;          /* clock: clock port or domain name */
        struct { Expr *lhs; Expr *rhs; Expr *constraint; } assign;
        Expr *expr;
        struct { Pattern *pat; Expr *iter; Block *body; } for_stmt;
        struct { Expr *cond; Block *body; } while_stmt;
        struct { AssertKind kind; Expr *cond; Expr *msg; } assert_stmt;      /* msg may be NULL */
        struct { Expr *cond; Expr *msg; } sassert;                     /* msg may be NULL */
        Expr *ret;
    } u;
};

/* ------------------------------------------------------------------ items */

typedef enum ItemKind {
    ITEM_ERROR,
    ITEM_MODULE,          /* u.module */
    ITEM_EXTERN,          /* u.extern_mod */
    ITEM_BUNDLE,          /* u.bundle */
    ITEM_STRUCT,          /* u.struct_def */
    ITEM_ENUM,            /* u.enum_def */
    ITEM_CONST,           /* u.const_def */
    ITEM_FN,              /* u.fn */
    ITEM_TYPE,            /* u.type_alias */
    ITEM_REGMAP,          /* u.regmap */
    ITEM_TEST,            /* u.test */
    ITEM_PROPERTY,        /* u.property */
    ITEM_SEQUENCE,        /* u.sequence */
    ITEM_STATIC_ASSERT    /* u.sassert: static_assert(c, "msg") at file level */
} ItemKind;

typedef struct Variant {
    Ident name;
    int has_fields;       /* `Write { addr: u16, data: u32 }` or `Weighted<W: [uint; N]>` */
    int angle;            /* payload declared generic-style: `Weighted<W: [uint; N]>` */
    Field **fields;
    uint32_t nfields;
    AttrList attrs;
    AstDoc *doc;
    Span span;
} Variant;

typedef struct Param {
    Ident name;
    Type *type;
    Span span;
} Param;

typedef enum RegEntryKind {
    REG_SCALAR,           /* 0x08 count: rw u32 */
    REG_FIELDS,           /* 0x00 ctrl: rw { enable: bool @0, mode: Mode @[1:2] } */
    REG_STREAM            /* 0x10 fifo: wo u8 -> Stream<u8> */
} RegEntryKind;

typedef struct RegField {
    Ident name;
    Type *type;
    Expr *bit_hi;         /* @n: bit_hi = n, bit_lo = NULL; @[a:b]: bit_hi = a, bit_lo = b (as written) */
    Expr *bit_lo;
    AttrList attrs;
    AstDoc *doc;
    Span span;
} RegField;

typedef struct RegEntry {
    Expr *offset;
    Ident name;
    Ident access;         /* rw ro wo w1c rw1s (validated by the parser) */
    RegEntryKind kind;
    Type *type;           /* REG_SCALAR / REG_STREAM: the register's type; NULL for REG_FIELDS */
    Type *stream_type;    /* REG_STREAM: the type after `->` */
    RegField **fields;    /* REG_FIELDS */
    uint32_t nfields;
    Expr *reset;          /* `= value` (extension), or NULL */
    AttrList attrs;
    AstDoc *doc;
    Span span;
} RegEntry;

typedef enum PropertyKind {
    PROP_PLAIN,           /* property p { .. }  (asserted) */
    PROP_ASSERT,          /* assert property p { .. } */
    PROP_ASSUME,
    PROP_COVER
} PropertyKind;

typedef struct WhereList {
    Expr **items;
    uint32_t n;
} WhereList;

struct Item {
    ItemKind kind;
    Span span;            /* whole item, from its first attribute / `pub` / keyword */
    Ident name;           /* name.name == NULL for tests (see u.test.name) and static_assert */
    int is_pub;
    AttrList attrs;
    AstDoc *doc;          /* or NULL */
    union {
        struct { GenericParams generics; WhereList where; Stmt **body; uint32_t nbody; } module;
        struct {
            const char *file;     /* the SystemVerilog file ("" = vendor library) */
            Span file_span;
            GenericParams generics;
            WhereList where;
            Stmt **body;          /* ports (and whatever else was written) */
            uint32_t nbody;
        } extern_mod;
        /* fns: the `fn` and `const` items of the body (associated items, `T::NAME`) */
        struct { GenericParams generics; Field **fields; uint32_t nfields; Item **fns; uint32_t nfns; } bundle;
        struct { GenericParams generics; Field **fields; uint32_t nfields; Item **fns; uint32_t nfns; } struct_def;
        struct { GenericParams generics; Variant **variants; uint32_t nvariants; } enum_def;
        struct { Type *type; Expr *value; } const_def;   /* type may be NULL (extension) */
        struct {
            int is_const;
            GenericParams generics;
            Param *params;
            uint32_t nparams;
            Type *ret;            /* NULL when no `-> T` */
            WhereList where;
            Block *body;
        } fn;
        struct { GenericParams generics; Type *type; } type_alias;
        struct { RegEntry **entries; uint32_t nentries; } regmap;
        struct { const char *name; Span name_span; Block *body; } test;
        struct { PropertyKind kind; Expr *body; } property;
        struct { Expr *body; } sequence;
        struct { Expr *cond; Expr *msg; } sassert;
    } u;
};

/* ------------------------------------------------------------------- uses */

/* use a::b::C            path [a b C]
 * use a::b::*            path [a b], glob
 * use a::{X, y::Z}       path [a], group [{[X]}, {[y Z]}]
 * use a::B as C          path [a B], alias C (extension) */
typedef struct UseTree {
    Ident *path;
    uint32_t npath;
    int glob;
    int is_group;
    struct UseTree **group;
    uint32_t ngroup;
    Ident alias;          /* alias.name == NULL when absent */
    Span span;
} UseTree;

typedef struct UseDecl {
    int is_pub;           /* pub use (re-export) */
    UseTree *tree;
    AstDoc *doc;
    Span span;
} UseDecl;

/* ------------------------------------------------------------------- file */

typedef struct AstFile {
    uint32_t src;         /* source id (diag.h) */
    const char *path;     /* src_get(src)->path */
    Arena *arena;
    int has_package;
    Ident *package;       /* package std::stream -> [std, stream] */
    uint32_t npackage;
    Span package_span;
    UseDecl **uses;
    uint32_t nuses;
    Item **items;         /* in source order */
    uint32_t nitems;
    Comment *comments;    /* every comment, in source order (formatter, docs) */
    uint32_t ncomments;
    Token *tokens;        /* the token stream (last is TK_EOF) */
    uint32_t ntokens;
    int nerrors;          /* lexical + syntax errors reported for this file */
} AstFile;

const char *ast_unary_op_text(UnaryOp op);    /* "-", "!", "~", "&", "|", "^" */
const char *ast_binary_op_text(BinaryOp op);  /* "+", "and", "|->", ... */

#endif /* TERN_AST_H */
