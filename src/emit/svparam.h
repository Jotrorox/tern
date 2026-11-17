/* svparam.h — private to src/emit: parameterized SystemVerilog modules
 * (spec §13.3; the public API and the method are described in emit.h at
 * sv_params_plan).  svparam.c has the parts that only look at the IR: the
 * lockstep walk over the elaborations of one declaration, candidate
 * matching and printing of SvXExpr trees.  sv.c drives the plan (it needs
 * the emitter to compare the emitted texts) and prints the views. */
#ifndef TERN_SVPARAM_H
#define TERN_SVPARAM_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "emit.h"

/* Slot kinds: places of the IR whose number may be a parameter expression.
 * A slot is keyed by the address of the IR object holding the number. */
enum {
    SVK_SIGW = 1, /* IrSignal: width of a scalar signal (bool/bits/uint/sint) */
    SVK_DEPTH,    /* IrSignal: memory depth */
    SVK_EXPRW,    /* IrExpr: result width */
    SVK_LO,       /* IrExpr: IRE_SLICE low bit */
    SVK_CONST,    /* IrBits limbs (IrBits.w): value of a constant / reset value / case label */
    SVK_INSTP,    /* IrInstance address + generic index: child generic argument */
    SVK_LVW,      /* IrLvalue: width */
    SVK_LVLO      /* IrLvalue: low bit */
};

#define SVP_KEY(p, tag) (((uint64_t)(uintptr_t)(p) << 4) | (uint64_t)(tag))
#define SVP_KEYI(p, i, tag) ((((uint64_t)(uintptr_t)(p) + (uint64_t)(i)) << 4) | (uint64_t)(tag))

/* one SystemVerilog module shared by the elaborations of a declaration */
typedef struct SvPGroup {
    const void *item;         /* IrOrigin.item */
    const char *name;         /* SV module name (sanitized declaration name) */
    const char *display;      /* "Blinky<CLK_HZ>" */
    uint32_t nparams;
    const char **pnames;      /* SV names of the generics */
    unsigned char *pbool;     /* 1: bool generic */
    int64_t *pdefault;        /* parameter defaults: the representative's values */
    const IrModule *rep;      /* the elaboration that is emitted */
    VEC(const SvXExpr *) locals; /* module consts used by the expressions (dependency order) */
    int accepted;
} SvPGroup;

/* how one IrModule is printed as a member of a group */
typedef struct SvPView {
    SvPGroup *g;
    Map slots;                /* SVP_KEY -> const SvXExpr* */
} SvPView;

struct SvParamPlan {
    Arena arena;
    Map groups;               /* item -> SvPGroup* (every eligible declaration) */
    Map views;                /* IrModule* (accepted members of the design) -> SvPView* */
    VEC(SvPView *) allviews;  /* for freeing */
};

/* --- svparam.c ---------------------------------------------------------- */

typedef struct SvPSlot {
    int tag;
    uint32_t at;              /* first index into SvPWalk.keys / vals (n entries) */
} SvPSlot;

typedef struct SvPTypePair {  /* named types at the same place in elaboration 0 and k */
    uint32_t k;
    const IrType *t0, *tk;
} SvPTypePair;

typedef struct SvPWalk {
    uint32_t n;               /* elaborations */
    const IrModule **mods;    /* mods[0] is the representative */
    const struct SvParamPlan *plan; /* accepted child groups */
    VEC(SvPSlot) slots;
    VEC(uint64_t) keys;       /* per slot: n keys */
    VEC(int64_t) vals;        /* per slot: n values */
    VEC(SvPTypePair) types;
    const char *why;          /* set when the elaborations differ structurally */
} SvPWalk;

/* Compares every elaboration with mods[0]; 1 if they differ only in slots. */
int svp_walk(SvPWalk *w);
void svp_walk_free(SvPWalk *w);

/* The group of a module (accepted or not), or NULL. */
SvPGroup *svp_group_of(const struct SvParamPlan *plan, const IrModule *m);

/* candidates with their value in each elaboration */
typedef struct SvPCands {
    uint32_t n, nelab;
    const SvXExpr **x;
    int64_t *vals;            /* n * nelab */
    unsigned char *ok;        /* n: defined in every elaboration */
} SvPCands;

void svp_cands_init(SvPCands *c, const SvXExpr *const *base, uint32_t nbase, const int64_t *const *pvals,
                    uint32_t nelab, Arena *a);
void svp_cands_free(SvPCands *c);
/* The first candidate (or derived form, allocated in a) equal to v in every elaboration. */
const SvXExpr *svp_match(const SvPCands *c, const int64_t *v, Arena *a);

/* Printing (pnames: the group's parameter names).  prec: the precedence the
 * context needs (SVX_PREC_*); the text is parenthesized when it binds less. */
enum { SVX_PREC_COND = 1, SVX_PREC_LOR, SVX_PREC_LAND, SVX_PREC_BOR, SVX_PREC_BXOR, SVX_PREC_BAND, SVX_PREC_EQ,
       SVX_PREC_REL, SVX_PREC_SHIFT, SVX_PREC_ADD, SVX_PREC_MUL, SVX_PREC_PRIM };
void svp_print(const SvXExpr *x, const char *const *pnames, int prec, StrBuf *out);
/* "W-1" for a range bound: folds `e + 1` to `e` */
void svp_print_m1(const SvXExpr *x, const char *const *pnames, StrBuf *out);
/* a primary usable as a cast width: `W`, `8`, `($clog2(N))` */
void svp_print_castw(const SvXExpr *x, const char *const *pnames, StrBuf *out);
int svp_equal(const SvXExpr *a, const SvXExpr *b);
/* a - b when that is a constant (`W + 1` - `W`): 1 and *out */
int svp_const_diff(const SvXExpr *a, const SvXExpr *b, int64_t *out);
/* adds the SVX_LOCAL nodes reachable from x to out (definitions first) */
void svp_collect_locals(const SvXExpr *x, SvPGroup *g);

#endif /* TERN_SVPARAM_H */
