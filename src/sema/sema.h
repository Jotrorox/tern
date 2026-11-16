/* sema.h — packages and name resolution (resolve.c), the Tern type system
 * (types.c) and the per-node semantic side table for tooling (info.c).
 *
 * Usage (the driver does this; see src/driver.h):
 *
 *     Sema *S = sema_new();
 *     FileUnit *f = sema_add_file(S, ast, is_user, is_std);   // per parsed file
 *     sema_resolve(S);            // packages, items, `use`, re-exports, prelude
 *     Symbol sym;
 *     if (sema_lookup(S, f, name, &sym)) ...                  // file-level lookup
 *     Ty *t = ty_uint(S, 8);                                  // interned types
 *     sema_free(S);
 *
 * Name resolution is two-level. Package-level names (items, `use`d names,
 * globs, std::prelude, package roots such as `std`) are resolved here.
 * Everything that depends on generic arguments or on local declarations
 * (generic parameters, ports, let/var/reg, fn parameters, module-local
 * items) is bound by the elaborator's environments (src/elab/elab.h), which
 * fall back to sema_lookup() for names they do not know.
 *
 * Types (`Ty`) are concrete: they only exist after generic arguments are
 * known, so the type checker runs during elaboration, on each
 * instantiation. Types are hash-consed: two types are equal iff their
 * pointers are equal. Named types (struct/enum/bundle) are keyed by their
 * declaring item, their module context (module-local items) and their
 * generic arguments.
 */
#ifndef TERN_SEMA_H
#define TERN_SEMA_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../syntax/ast.h"
#include "../ir/ir.h"

typedef struct Sema Sema;
typedef struct Package Package;
typedef struct FileUnit FileUnit;
typedef struct Ty Ty;
typedef struct UseEntry UseEntry;

/* ============================================================ packages === */

typedef enum SymKind { SYM_NONE, SYM_ITEM, SYM_PACKAGE } SymKind;

typedef struct Symbol {
    SymKind kind;
    Item *item;           /* SYM_ITEM */
    FileUnit *file;       /* SYM_ITEM: the defining file */
    Package *pkg;         /* SYM_ITEM: the owning package; SYM_PACKAGE: the package */
} Symbol;

/* One `use` path (a group `use a::{B, c::D}` yields one entry per leaf). */
struct UseEntry {
    const char *name;     /* bound name (alias or last segment); NULL for globs */
    const char **segs;    /* interned path segments */
    Span *seg_spans;
    uint32_t nsegs;
    int glob;             /* `use a::b::*`: segs name the package */
    int is_pub;           /* `pub use` (re-export) */
    Span span;
    FileUnit *file;
    int state;            /* 0 unresolved, 1 resolving, 2 resolved, 3 failed */
    Symbol sym;           /* resolved target (a package for globs) */
};

typedef VEC(UseEntry *) UseEntryVec;
typedef VEC(FileUnit *) FileUnitVec;

struct FileUnit {
    AstFile *ast;
    Package *pkg;
    const char *dir;      /* directory of the file ("" for embedded std paths) */
    int is_user;          /* given on the command line / project source: checked and elaborated */
    int is_std;           /* part of the standard library */
    int is_test_root;     /* from tests/ (only compiled by `tern test`) */
    Map uses;             /* interned name -> UseEntry* (explicit uses) */
    UseEntryVec globs;
    UseEntryVec all_uses;
};

struct Package {
    const char *path;     /* "std::fifo" (interned); "" for the root namespace and unnamed files */
    const char *name;     /* last segment */
    Package *parent;
    Map children;         /* interned segment -> Package* */
    FileUnitVec files;
    Map items;            /* interned name -> Symbol* (every named item, pub or not) */
    Map reexports;        /* interned name -> UseEntry* (`pub use`) */
    UseEntryVec reexport_globs;
    int is_std;
    const char *dir;      /* directory of the first file (agreement check) */
};

/* ============================================================== types === */

typedef enum TyKind {
    TY_ERROR,             /* poisoned: an error was already reported */
    TY_VOID,              /* () */
    TY_BOOL,
    TY_BIT,               /* bit<N> */
    TY_UINT,              /* uint<N> */
    TY_SINT,              /* sint<N> */
    TY_INT,               /* widthless uint / sint (compile time only); is_signed */
    TY_CLOCK,             /* falling */
    TY_RESET,             /* async, active_low */
    TY_ARRAY,             /* [elem; len] */
    TY_STRUCT,            /* packed, fields[0] is the MSB */
    TY_ENUM,              /* tag ++ payload */
    TY_BUNDLE,            /* fields with flip; never a single signal */
    TY_TUPLE,             /* compile time and match subjects; fields named "0", "1", ... */
    TY_TRI,               /* tri<elem>, #[top] / extern ports only */
    TY_STRING,            /* compile time only */
    TY_TYPE               /* the type of types (`T: type` generics) */
} TyKind;

typedef struct TyField {
    const char *name;     /* interned */
    Ty *ty;
    int flip;             /* bundle fields */
    uint32_t lsb;         /* struct fields: bit offset of the field's LSB */
    Span loc;
    AstDoc *doc;
} TyField;

typedef struct TyVariant {
    const char *name;     /* interned */
    Ty *payload;          /* anonymous TY_STRUCT of the payload fields, or NULL */
    uint64_t tag;         /* encoded tag value (binary index, one-hot bit, gray code) */
    Span loc;
} TyVariant;

struct Ty {
    TyKind kind;
    uint32_t width;       /* bits of a hardware value; 0 for compile-time-only and bundle types */
    int is_signed;        /* SINT, signed widthless INT */
    int hw;               /* can be the type of one IR signal */
    int falling;          /* CLOCK */
    int async, active_low;/* RESET */
    Ty *elem;             /* ARRAY, TRI */
    uint32_t len;         /* ARRAY */
    uint32_t nfields;     /* STRUCT, BUNDLE, TUPLE */
    TyField *fields;
    uint32_t nvariants;   /* ENUM */
    TyVariant *variants;
    uint32_t tag_width;
    IrEncoding enc;
    uint32_t payload_width;
    /* named types (STRUCT/ENUM/BUNDLE) */
    Item *def;            /* declaring item, or NULL (anonymous struct, regmap-generated) */
    FileUnit *file;       /* file of the declaring item */
    void *genv;           /* elaborator environment holding the type's generic bindings
                             (struct Env *, see elab.h): used for bundle methods */
    void *gvals;          /* generic argument values (struct Value *, see value.h) */
    uint32_t ngvals;
    const char *name;     /* display name: "uint<8>", "Beat<uint<8>>", "UartTx::State" */
    const char *emit_name;/* emitted SV type name for named types ("Beat_u8", "UartTx_16_1_State") */
    const char *mangle;   /* name fragment used in mangled module names ("u8", "Beat_u8") */
    const char *key;      /* interning key (interned) */
    Span loc;
};

typedef struct TyIrCache {    /* Ty -> IrType conversion state, one per IrDesign */
    Map map;                  /* Ty* -> IrType* */
    Map names;                /* interned emitted type names already used */
} TyIrCache;

/* ============================================================ semantic info */

typedef enum SemKind {
    SEM_NONE, SEM_PORT_IN, SEM_PORT_OUT, SEM_WIRE, SEM_VAR, SEM_REG, SEM_MEM, SEM_CONST,
    SEM_GENERIC, SEM_PARAM, SEM_LOCAL, SEM_FN, SEM_MODULE, SEM_TYPE, SEM_INSTANCE,
    SEM_DOMAIN, SEM_PACKAGE, SEM_FIELD, SEM_VARIANT, SEM_EXPR
} SemKind;

/* One record per AST node (a use of a name, a declaration or an
 * expression). Strings are interned. Fields that are unknown are NULL /
 * a zero span. */
typedef struct SemInfo {
    Span span;            /* the node */
    Span def;             /* declaration (go-to-definition); file 0 if unknown */
    SemKind kind;
    const char *name;
    const char *type;     /* printed type, "uint<5>" */
    const char *domain;   /* clock domain name ("clk", "sys") */
    const char *driver;   /* "on clk", "comb", "continuous", "instance output", "input" */
    Span driver_span;     /* first assignment */
    const char *value;    /* compile-time value of consts and generics */
} SemInfo;

/* ============================================================== Sema === */

struct Sema {
    Arena arena;          /* packages, symbols, types */
    Package *root;        /* namespace root */
    FileUnitVec files;
    Package *prelude;     /* std::prelude, or NULL */
    Map types;            /* interned key -> Ty* */
    Ty *t_error, *t_void, *t_bool, *t_int, *t_sint, *t_string, *t_type;
    /* [features] of tern.toml: cfg::name */
    const char **feature_names;
    int *feature_values;
    size_t nfeatures;
    int has_manifest;
    /* semantic info */
    Map info_index;       /* span key -> index into info */
    VEC(SemInfo) info;
    int info_sorted;
    int record_info;      /* 0 disables recording (default 1) */
    /* memory of finished elaborators that types may still point into
     * (generic environments, generic argument values): freed by sema_free */
    VEC(Arena *) retired_arenas;
    VEC(void *) retired_blocks;
};

/* ------------------------------------------------------------ resolve.c */

Sema *sema_new(void);
void sema_free(Sema *S);
/* Registers a parsed file under its declared package (files without
 * `package` go to the root package ""). */
FileUnit *sema_add_file(Sema *S, AstFile *ast, int is_user, int is_std);
/* Builds item tables (duplicate definitions are errors), checks that files in
 * one directory agree on their package, resolves every `use` (errors for
 * unknown paths and private items) and finds std::prelude. */
void sema_resolve(Sema *S);
/* File-level lookup of a single name: own package's items, explicit uses,
 * glob imports, std::prelude, package roots. Returns 1 if found. */
int sema_lookup(Sema *S, FileUnit *from, const char *name, Symbol *out);
/* `pkg::name`: item, re-export or sub-package. *private_out (may be NULL)
 * is set when the name exists but is not visible from `from`. */
int sema_lookup_member(Sema *S, FileUnit *from, Package *pkg, const char *name, Symbol *out,
                       int *private_out);
Package *sema_find_package(Sema *S, const char *path);   /* "std::fifo" or NULL */
Package *sema_root_package(Sema *S, const char *name);   /* namespace root by first segment */
/* Reports a "did you mean" help line for an unknown `name` (call right after
 * the diag_error): the closest visible name (edit distance) among `extra`
 * (local names) and the file's scope, else a std/package item with exactly
 * this name ("did you mean to `use std::fifo::Fifo`?"). */
void sema_suggest(Sema *S, FileUnit *from, const char *name, const char *const *extra, size_t nextra);
/* Levenshtein distance (ASCII), capped at `cap` + 1. */
uint32_t sema_edit_distance(const char *a, const char *b, uint32_t cap);
/* Package of a module item / its source file. */
FileUnit *sema_file_of_item(Sema *S, const Item *it);
int sema_feature(Sema *S, const char *name, int *value);  /* cfg::name */

/* -------------------------------------------------------------- types.c */

Ty *ty_scalar(Sema *S, TyKind kind, uint32_t width);   /* BOOL / BIT / UINT / SINT */
Ty *ty_bool(Sema *S);
Ty *ty_bits(Sema *S, uint32_t width);
Ty *ty_uint(Sema *S, uint32_t width);
Ty *ty_sint(Sema *S, uint32_t width);
Ty *ty_int(Sema *S, int is_signed);                     /* widthless */
Ty *ty_clock(Sema *S, int falling);
Ty *ty_reset(Sema *S, int async, int active_low);
Ty *ty_array(Sema *S, Ty *elem, uint32_t len);
Ty *ty_tuple(Sema *S, uint32_t n, Ty *const *elems);
Ty *ty_tri(Sema *S, Ty *elem);
/* Named struct / bundle: `key` identifies it (def item + context + generic
 * args); an existing type with that key is returned unchanged (fields are
 * then ignored). fields[0] is the MSB; lsb offsets are computed. */
Ty *ty_struct(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
              uint32_t nfields, const TyField *fields, Span loc);
Ty *ty_bundle(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
              uint32_t nfields, const TyField *fields, Span loc);
/* variants' tags are assigned from `enc`. */
Ty *ty_enum(Sema *S, const char *key, const char *name, const char *emit_name, Item *def,
            uint32_t nvariants, const TyVariant *variants, IrEncoding enc, Span loc);
Ty *ty_find(Sema *S, const char *key);                  /* by interning key, or NULL */

void ty_print(const Ty *t, StrBuf *out);
const char *ty_str(const Ty *t);                        /* interned display name */
int ty_is_int(const Ty *t);         /* BIT, UINT, SINT, INT */
int ty_is_sized_int(const Ty *t);   /* BIT, UINT, SINT */
int ty_is_hw(const Ty *t);          /* can be one IR signal */
int ty_has_bundle(const Ty *t);     /* bundle, or array/tuple of bundles */
int ty_field_index(const Ty *t, const char *name);      /* struct/bundle/tuple field or -1 */
int ty_variant_index(const Ty *t, const char *name);    /* enum variant or -1 */
uint32_t ty_clog2(uint64_t n);
/* Converts a hardware type to the IR (named types get unique emitted names
 * per design). Returns NULL for non-hardware types. */
IrType *ty_to_ir(IrDesign *d, TyIrCache *cache, const Ty *t);
void ty_ir_cache_free(TyIrCache *c);

/* --------------------------------------------------------------- info.c */

/* Records info for a node. The first record for a span wins; later records
 * for the same span only fill fields that are still unknown. */
void sema_info_record(Sema *S, const SemInfo *info);
/* Sets the driver of the declaration at `def` (first driver wins). */
void sema_info_set_driver(Sema *S, Span def, const char *driver, Span at);
/* Innermost record containing byte offset `off` of source `file`, or NULL. */
const SemInfo *sema_info_at(Sema *S, uint32_t file, uint32_t off);
/* Record of a declaration, by its span (for drivers / hover of uses). */
const SemInfo *sema_info_decl(Sema *S, Span def);
/* Hover text: "`count: uint<5> @ clk` — reg, driven by on clk at fifo.tern:40". */
void sema_info_hover(Sema *S, const SemInfo *info, StrBuf *out);
/* All records (sorted by file, offset) — for references / document symbols. */
size_t sema_info_all(Sema *S, const SemInfo **out);
const char *sema_kind_name(SemKind k);

#endif /* TERN_SEMA_H */
