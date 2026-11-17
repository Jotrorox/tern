/* simc.c — the compiled simulator (spec §13.7): the instruction list that
 * netlist.c builds is translated to C, compiled with the host C compiler
 * into a shared library and loaded; the engine (eval.c) then calls the
 * compiled code instead of interpreting the comb / clocked / property
 * regions.  Everything else (state, pokes, peeks, clocks, resets, VCD,
 * properties, coverage counters, test_port overrides) stays in eval.c, so
 * the compiled model behaves exactly like the interpreter.
 *
 * Generated code
 *   One C statement per instruction over the same value array (`v`), with
 *   the same masks and the same evaluation order; constants are inlined;
 *   jumps become gotos and case tables C switches.  Wide operations whose
 *   C is short (copy, mux, bitwise, add/sub, eq/ne, constant slices and
 *   deposits, zero-extension) are inlined limb by limb; the rest (wide
 *   mul/div/shift, memory writes, assertions' bookkeeping, #[test_port]
 *   overrides, ...) call back into the interpreter for that one
 *   instruction.  Regions are split into functions of about SC_CHUNK
 *   instructions at points no jump crosses (keeps compile time linear).
 *
 * ABI: the library exports tern_abi() (SC_ABI), tern_id() (the hash of
 * the generated source) and tern_exec(rt, region) with region 0 = comb,
 * 1 + i = clocked code of flattened domain i, 1 + ndoms + g = property
 * leaves of edge group g.
 *
 * Host compiler: $CC (split at spaces), else cc, gcc, clang, cl (MSVC,
 * Windows), whichever is found first on PATH.  Flags: -O1 -shared -fPIC
 * (cl: /O1 /LD).  Models are cached as <cache>/<hash><ext>, the hash
 * covering the source, the compiler command and the flags; a compile
 * writes unique temporary names and renames the library into place, so
 * parallel test workers never see a partial file.
 *
 * Engine selection (sim_set_engine_defaults / sim_compile):
 *   interp    never compile
 *   compiled  compile (or load from the cache) in sim_new; a failure is an
 *             error (sim_new returns NULL)
 *   auto      interpret; after SC_AUTO_WORK interpreted instructions
 *             (about a quarter of a second) generate the C: a cached
 *             model is loaded at once, otherwise the compiler runs in the
 *             background and the model switches to compiled code when it
 *             has finished.  No compiler, or a failed compile: the
 *             interpreter carries on silently.  The switch is invisible
 *             (both engines produce identical values), so the results do
 *             not depend on when it happens. */
#include "sim_impl.h"
#include "../os/os.h"

#define SC_ABI 1u
#define SC_CHUNK 2000u
#define SC_AUTO_WORK (UINT64_C(1) << 26)
#define SC_POLL_WORK (UINT64_C(1) << 21)
#define SC_LIMBS(w) (((w) + 63u) / 64u)

/* mirrors `TernRt` in the generated prelude */
typedef struct SimCRt {
    uint64_t *v;
    uint64_t *cov;
    uint64_t **mem;
    const uint32_t *nforced;
    void *m;
    void (*insn)(void *m, unsigned pc);
    void (*assert_hit)(void *m, unsigned id, unsigned long long val);
} SimCRt;

typedef void (*SimCExecFn)(SimCRt *rt, unsigned region);
typedef unsigned (*SimCAbiFn)(void);
typedef const char *(*SimCIdFn)(void);

struct SimC {
    void *dl;
    SimCExecFn exec;
    SimCRt rt;
    uint64_t **mems;
    /* compile in progress */
    OsProc *proc;
    char *src_path;           /* temporary .c */
    char *tmp_out;            /* temporary library */
    char *final_path;         /* cache entry */
    char *id;                 /* expected tern_id() */
    int failed;
};

/* --------------------------------------------------- process defaults -- */

static SimEngine sc_default_engine = SIM_ENGINE_AUTO;
static char *sc_default_cache;
static int sc_cc_probed;
static VEC(char *) sc_cc_argv;
static int sc_cc_msvc;
static char *sc_cc_display;
static uint32_t sc_counter;

void sim_set_engine_defaults(SimEngine e, const char *cache_dir)
{
    sc_default_engine = e;
    free(sc_default_cache);
    sc_default_cache = cache_dir ? xstrdup(cache_dir) : NULL;
}

SimEngine sim_engine_default(void)
{
    return sc_default_engine;
}

int sim_parse_engine(const char *s, SimEngine *out)
{
    if (!s)
        return 0;
    if (strcmp(s, "auto") == 0)
        *out = SIM_ENGINE_AUTO;
    else if (strcmp(s, "interp") == 0)
        *out = SIM_ENGINE_INTERP;
    else if (strcmp(s, "compiled") == 0)
        *out = SIM_ENGINE_COMPILED;
    else
        return 0;
    return 1;
}

static int sc_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* "cl", "cl.exe", ".../cl.exe", "clang-cl" (case-insensitive) */
static int sc_name_is_msvc(const char *path)
{
    const char *b = os_path_basename(path);
    char low[32];
    size_t i, n = strlen(b);
    if (n >= sizeof low)
        return 0;
    for (i = 0; i <= n; i++)
        low[i] = (char)(b[i] >= 'A' && b[i] <= 'Z' ? b[i] - 'A' + 'a' : b[i]);
    return strcmp(low, "cl") == 0 || strcmp(low, "cl.exe") == 0 || strcmp(low, "clang-cl") == 0 ||
           strcmp(low, "clang-cl.exe") == 0;
}

static int sc_try_cc(const char *cmd)
{
    const char *p = cmd;
    char *prog;
    VEC(char *) argv = {0};
    while (*p) {
        const char *s;
        while (sc_is_space(*p))
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && !sc_is_space(*p))
            p++;
        vec_push(argv, xstrndup(s, (size_t)(p - s)));
    }
    if (!argv.len) {
        vec_free(argv);
        return 0;
    }
    prog = os_find_program(argv.data[0]);
    if (!prog && os_is_file(argv.data[0]))
        prog = xstrdup(argv.data[0]);
    if (!prog) {
        size_t i;
        for (i = 0; i < argv.len; i++)
            free(argv.data[i]);
        vec_free(argv);
        return 0;
    }
    free(argv.data[0]);
    argv.data[0] = prog;
    sc_cc_argv.len = 0;
    {
        size_t i;
        for (i = 0; i < argv.len; i++)
            vec_push(sc_cc_argv, argv.data[i]);
    }
    vec_free(argv);
    sc_cc_msvc = sc_name_is_msvc(prog);
    sc_cc_display = xstrdup(cmd);
    return 1;
}

const char *sim_host_cc(void)
{
    if (!sc_cc_probed) {
        char *env = os_getenv("CC");
        static const char *const fallbacks[] = {"cc", "gcc", "clang", "cl", "tcc"};
        size_t i;
        sc_cc_probed = 1;
        if (env && *env && sc_try_cc(env)) {
            free(env);
            return sc_cc_display;
        }
        free(env);
        for (i = 0; i < TERN_ARRAY_LEN(fallbacks); i++)
            if (sc_try_cc(fallbacks[i]))
                break;
    }
    return sc_cc_display;
}

static char *sc_cache_dir(void)
{
    char *env;
    if (sc_default_cache)
        return xstrdup(sc_default_cache);
    env = os_getenv("TERN_SIMCACHE");
    if (env && *env)
        return env;
    free(env);
    {
        char *tmp = os_temp_dir();
        char *dir = os_path_join(tmp ? tmp : ".", "tern-simcache");
        free(tmp);
        return dir;
    }
}

/* -------------------------------------------------------- generation -- */

typedef struct ScGen {
    SimModel *m;
    StrBuf *o;
    int inline_consts;
    uint8_t *label;           /* per code position (0..code.len): jump target */
    uint8_t *cb;              /* per code position: emitted as an interpreter callback */
    uint8_t *tbad;            /* per temporary: live into some chunk, must stay in v[] */
    uint8_t *cbad;            /* per temporary: wide or callback operand in the current chunk */
    int locals;               /* use C locals for the temporaries that may */
    char nm[4][24];
    int nmi;
} ScGen;

static int sc_is_const(const ScGen *g, uint32_t x)
{
    return g->inline_consts && x >= g->m->const_base && x < g->m->temp_base;
}

static int sc_is_local(const ScGen *g, uint32_t x)
{
    return g->locals && x >= g->m->temp_base && x < g->m->misc_base && !g->tbad[x - g->m->temp_base] &&
           !g->cbad[x - g->m->temp_base];
}

/* the C lvalue of a narrow value: v[x], or the local t<x> of a temporary */
static const char *sc_nm(ScGen *g, uint32_t x)
{
    char *b = g->nm[g->nmi++ & 3];
    snprintf(b, sizeof g->nm[0], sc_is_local(g, x) ? "t%u" : "v[%u]", x);
    return b;
}

/* a narrow operand */
static void sc_rd(ScGen *g, uint32_t x)
{
    if (sc_is_const(g, x))
        sb_printf(g->o, "0x%llxULL", (unsigned long long)g->m->v[x]);
    else
        sb_puts(g->o, sc_nm(g, x));
}

static void sc_lit(ScGen *g, uint64_t x)
{
    sb_printf(g->o, "0x%llxULL", (unsigned long long)x);
}

static uint64_t sc_top_mask(uint32_t w)
{
    return (w % 64) ? ((UINT64_C(1) << (w % 64)) - 1u) : ~UINT64_C(0);
}

/* expression for bv_get64(v + a, aw, lo, n) with constant lo, n (n <= 64) */
static void sc_get64(ScGen *g, uint32_t a, uint32_t aw, uint64_t lo, uint32_t n)
{
    uint32_t limb, off, na = SC_LIMBS(aw);
    if (lo >= aw || n == 0) {
        sb_puts(g->o, "0");
        return;
    }
    limb = (uint32_t)(lo / 64);
    off = (uint32_t)(lo % 64);
    sb_puts(g->o, "((");
    if (off)
        sb_printf(g->o, "(v[%u] >> %u)", a + limb, off);
    else
        sb_printf(g->o, "v[%u]", a + limb);
    if (off && limb + 1 < na)
        sb_printf(g->o, " | (v[%u] << %u)", a + limb + 1, 64 - off);
    sb_puts(g->o, ")");
    if (n < 64) {
        sb_puts(g->o, " & ");
        sc_lit(g, (UINT64_C(1) << n) - 1u);
    }
    sb_puts(g->o, ")");
}

/* statements for bv_put_bits(v + d, dw, p, <expr in variable `t`>, nbits) */
static void sc_put_bits(ScGen *g, uint32_t d, uint32_t dw, uint64_t p, uint32_t nbits)
{
    uint32_t limb, off;
    uint64_t mk;
    if (p >= dw || nbits == 0)
        return;
    if (p + nbits > dw)
        nbits = (uint32_t)(dw - p);
    mk = nbits >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << nbits) - 1u);
    limb = (uint32_t)(p / 64);
    off = (uint32_t)(p % 64);
    sb_puts(g->o, " t &= ");
    sc_lit(g, mk);
    sb_printf(g->o, "; v[%u] = (v[%u] & ", d + limb, d + limb);
    sc_lit(g, ~(mk << off));
    if (off)
        sb_printf(g->o, ") | (t << %u);", off);
    else
        sb_puts(g->o, ") | t;");
    if (off && off + nbits > 64) {
        uint32_t rest = off + nbits - 64;
        uint64_t m2 = (UINT64_C(1) << rest) - 1u;
        sb_printf(g->o, " v[%u] = (v[%u] & ", d + limb + 1, d + limb + 1);
        sc_lit(g, ~m2);
        sb_printf(g->o, ") | (t >> %u);", 64 - off);
    }
}

static void sc_callback(ScGen *g, uint32_t pc)
{
    g->cb[pc] = 1;
    sb_printf(g->o, "rt->insn(rt->m, %uu);", pc);
}

/* one instruction; returns 0 if it cannot be translated */
static int sc_insn(ScGen *g, uint32_t pc)
{
    SimModel *m = g->m;
    const SimInsn *in = &m->code.data[pc];
    StrBuf *o = g->o;
    uint32_t i, n;
    sb_puts(o, "  ");
    switch ((SimOp)in->op) {
    case SOP_COPY:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, ";");
        break;
    case SOP_SLICE:
        if (in->p >= 64) {
            sc_callback(g, pc);
            break;
        }
        sb_printf(o, "%s = (", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_printf(o, " >> %u) & ", in->p);
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_XSLICE:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_get64(g, in->a, in->aw, in->p, in->w);
        sb_puts(o, ";");
        break;
    case SOP_INDEX:
        if (!in->w) {
            sc_callback(g, pc);
            break;
        }
        sb_puts(o, "{ tu64 i = ");
        sc_rd(g, in->b);
        sb_printf(o, "; %s = i < %uu ? (", sc_nm(g, in->d), in->aw / in->w);
        sc_rd(g, in->a);
        sb_printf(o, " >> (i * %uu)) & ", in->w);
        sc_lit(g, in->mask);
        sb_puts(o, " : 0; }");
        break;
    case SOP_NOT:
        sb_printf(o, "%s = ~", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " & ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_NEG:
        sb_printf(o, "%s = (0 - ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, ") & ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_LNOT:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " == 0;");
        break;
    case SOP_RAND:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " == ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_ROR:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " != 0;");
        break;
    case SOP_RXOR:
        sb_printf(o, "%s = tern_parity(", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, ");");
        break;
    case SOP_ADD:
    case SOP_SUB:
    case SOP_MUL:
        sb_printf(o, "%s = (", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, in->op == SOP_ADD ? " + " : in->op == SOP_SUB ? " - " : " * ");
        sc_rd(g, in->b);
        sb_puts(o, ") & ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_DIVU:
    case SOP_MODU:
        sb_puts(o, "{ tu64 x = ");
        sc_rd(g, in->a);
        sb_puts(o, ", y = ");
        sc_rd(g, in->b);
        if (in->op == SOP_DIVU)
            sb_printf(o, "; %s = y ? x / y : 0; }", sc_nm(g, in->d));
        else
            sb_printf(o, "; %s = y ? x %% y : x; }", sc_nm(g, in->d));
        break;
    case SOP_AND:
    case SOP_OR:
    case SOP_XOR:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, in->op == SOP_AND ? " & " : in->op == SOP_OR ? " | " : " ^ ");
        sc_rd(g, in->b);
        sb_puts(o, ";");
        break;
    case SOP_SHL:
    case SOP_SHR:
        sb_puts(o, "{ tu64 s = ");
        sc_rd(g, in->b);
        sb_printf(o, "; %s = s >= %uu ? 0 : ", sc_nm(g, in->d), in->w);
        if (in->op == SOP_SHL) {
            sb_puts(o, "(");
            sc_rd(g, in->a);
            sb_puts(o, " << s) & ");
            sc_lit(g, in->mask);
        } else {
            sc_rd(g, in->a);
            sb_puts(o, " >> s");
        }
        sb_puts(o, "; }");
        break;
    case SOP_ASHR:
        if (!in->w || in->w > 64) {
            sc_callback(g, pc);
            break;
        }
        sb_puts(o, "{ tu64 s = ");
        sc_rd(g, in->b);
        sb_puts(o, ", x = ");
        sc_rd(g, in->a);
        sb_printf(o, ", M = ");
        sc_lit(g, in->mask);
        sb_printf(o, "; int sg = (int)((x >> %u) & 1u); %s = s >= %uu ? (sg ? M : 0) : "
                     "(x >> s) | (sg ? (M & ~(M >> s)) : 0); }",
                  in->w - 1, sc_nm(g, in->d), in->w);
        break;
    case SOP_EQ:
    case SOP_NE:
    case SOP_LTU:
    case SOP_LEU:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, in->op == SOP_EQ ? " == " : in->op == SOP_NE ? " != " : in->op == SOP_LTU ? " < " : " <= ");
        sc_rd(g, in->b);
        sb_puts(o, ";");
        break;
    case SOP_LTS:
    case SOP_LES: {
        uint64_t sb;
        if (!in->aw || in->aw > 64) {
            sc_callback(g, pc);
            break;
        }
        sb = UINT64_C(1) << (in->aw - 1);
        sb_printf(o, "%s = (", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " ^ ");
        sc_lit(g, sb);
        sb_puts(o, in->op == SOP_LTS ? ") < (" : ") <= (");
        sc_rd(g, in->b);
        sb_puts(o, " ^ ");
        sc_lit(g, sb);
        sb_puts(o, ");");
        break;
    }
    case SOP_LAND:
    case SOP_LOR:
        sb_printf(o, "%s = (", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, in->op == SOP_LAND ? " & " : " | ");
        sc_rd(g, in->b);
        sb_puts(o, ") & 1u;");
        break;
    case SOP_SHLOR:
        sb_printf(o, "%s = (", sc_nm(g, in->d));
        if (in->p >= 64) {
            sb_puts(o, "0");
        } else {
            sb_puts(o, "(");
            sc_rd(g, in->a);
            sb_printf(o, " << %u)", in->p);
        }
        sb_puts(o, " | ");
        sc_rd(g, in->b);
        sb_puts(o, ") & ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    case SOP_MUX:
        sb_printf(o, "%s = ", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " ? ");
        sc_rd(g, in->b);
        sb_puts(o, " : ");
        sc_rd(g, in->c);
        sb_puts(o, ";");
        break;
    case SOP_SEXT: {
        uint64_t sb;
        if (!in->aw || in->aw > 64) {
            sc_callback(g, pc);
            break;
        }
        sb = UINT64_C(1) << (in->aw - 1);
        sb_printf(o, "%s = ((", sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " ^ ");
        sc_lit(g, sb);
        sb_puts(o, ") - ");
        sc_lit(g, sb);
        sb_puts(o, ") & ");
        sc_lit(g, in->mask);
        sb_puts(o, ";");
        break;
    }
    case SOP_DEPOSIT: {
        uint64_t mk;
        if (in->p >= 64) {
            sc_callback(g, pc);
            break;
        }
        mk = in->mask << in->p;
        sb_printf(o, "%s = (%s & ", sc_nm(g, in->d), sc_nm(g, in->d));
        sc_lit(g, ~mk);
        sb_puts(o, ") | ((");
        sc_rd(g, in->a);
        sb_printf(o, " << %u) & ", in->p);
        sc_lit(g, mk);
        sb_puts(o, ");");
        break;
    }
    case SOP_DEPOSIT_DYN: {
        uint64_t lim;
        if (!in->aw || in->aw > 64) {
            sc_callback(g, pc);
            break;
        }
        if (in->p + in->aw > in->w) /* never in range */
            break;
        lim = (uint64_t)(in->w - in->p - in->aw) / in->aw;
        sb_puts(o, "{ tu64 idx = ");
        sc_rd(g, in->b);
        sb_printf(o, "; if (idx < %uu && idx <= ", in->c);
        sc_lit(g, lim);
        sb_printf(o, ") { tu64 lo = %uu + idx * %uu, mk = ", in->p, in->aw);
        sc_lit(g, in->mask);
        sb_printf(o, " << lo; %s = (%s & ~mk) | ((", sc_nm(g, in->d), sc_nm(g, in->d));
        sc_rd(g, in->a);
        sb_puts(o, " << lo) & mk); } }");
        break;
    }
    case SOP_WCOPY:
        n = SC_LIMBS(in->w);
        if (in->d == in->a)
            break;
        for (i = 0; i < n; i++)
            sb_printf(o, "v[%u] = v[%u]; ", in->d + i, in->a + i);
        break;
    case SOP_WZERO:
        n = SC_LIMBS(in->w);
        for (i = 0; i < n; i++)
            sb_printf(o, "v[%u] = 0; ", in->d + i);
        break;
    case SOP_WMUX:
        n = SC_LIMBS(in->w);
        sb_puts(o, "if (");
        sc_rd(g, in->a);
        sb_puts(o, ") {");
        if (in->d != in->b)
            for (i = 0; i < n; i++)
                sb_printf(o, " v[%u] = v[%u];", in->d + i, in->b + i);
        sb_puts(o, " } else {");
        if (in->d != in->c)
            for (i = 0; i < n; i++)
                sb_printf(o, " v[%u] = v[%u];", in->d + i, in->c + i);
        sb_puts(o, " }");
        break;
    case SOP_WUN:
        n = SC_LIMBS(in->w);
        if (in->sub == IROP_NOT && n) {
            for (i = 0; i < n; i++)
                sb_printf(o, "v[%u] = ~v[%u]; ", in->d + i, in->a + i);
            if (in->w % 64) {
                sb_printf(o, "v[%u] &= ", in->d + n - 1);
                sc_lit(g, sc_top_mask(in->w));
                sb_puts(o, ";");
            }
        } else if (in->sub == IROP_ROR && in->aw) {
            uint32_t na = SC_LIMBS(in->aw);
            sb_printf(o, "v[%u] = (", in->d);
            for (i = 0; i < na; i++)
                sb_printf(o, "%sv[%u]", i ? " | " : "", in->a + i);
            sb_puts(o, ") != 0;");
        } else {
            sc_callback(g, pc);
        }
        break;
    case SOP_WBIN:
        n = SC_LIMBS(in->w);
        switch ((IrOp)in->sub) {
        case IROP_AND:
        case IROP_OR:
        case IROP_XOR: {
            const char *op = in->sub == IROP_AND ? "&" : in->sub == IROP_OR ? "|" : "^";
            for (i = 0; i < n; i++)
                sb_printf(o, "v[%u] = v[%u] %s v[%u]; ", in->d + i, in->a + i, op, in->b + i);
            break;
        }
        case IROP_ADD:
        case IROP_SUB:
            if (!n) {
                sc_callback(g, pc);
                break;
            }
            sb_puts(o, "{ tu64 c = 0, x, y, r, r2;");
            for (i = 0; i < n; i++) {
                if (in->sub == IROP_ADD)
                    sb_printf(o, " x = v[%u]; y = v[%u]; r = x + y; r2 = r + c; c = (tu64)(r < x) | (tu64)(r2 < r); "
                                 "v[%u] = r2;",
                              in->a + i, in->b + i, in->d + i);
                else
                    sb_printf(o, " x = v[%u]; y = v[%u]; r = x - y; r2 = r - c; c = (tu64)(x < y) | (tu64)(r < c); "
                                 "v[%u] = r2;",
                              in->a + i, in->b + i, in->d + i);
            }
            sb_puts(o, " (void)c;");
            if (in->w % 64) {
                sb_printf(o, " v[%u] &= ", in->d + n - 1);
                sc_lit(g, sc_top_mask(in->w));
                sb_puts(o, ";");
            }
            sb_puts(o, " }");
            break;
        case IROP_LAND:
        case IROP_LOR:
            sb_printf(o, "v[%u] = (v[%u] %s v[%u]) & 1u;", in->d, in->a, in->sub == IROP_LAND ? "&" : "|", in->b);
            break;
        default: sc_callback(g, pc); break;
        }
        break;
    case SOP_WCMP:
        if ((in->sub == IROP_EQ || in->sub == IROP_NE) && in->aw) {
            uint32_t na = SC_LIMBS(in->aw);
            sb_printf(o, "v[%u] = %s(", in->d, in->sub == IROP_NE ? "!" : "");
            for (i = 0; i < na; i++)
                sb_printf(o, "%sv[%u] == v[%u]", i ? " && " : "", in->a + i, in->b + i);
            sb_puts(o, ");");
        } else {
            sc_callback(g, pc);
        }
        break;
    case SOP_WEXTRACT:
        n = SC_LIMBS(in->w);
        if (in->d < in->a + SC_LIMBS(in->aw) && in->a < in->d + n) { /* overlap: keep the helper's order */
            sc_callback(g, pc);
            break;
        }
        for (i = 0; i < n; i++) {
            uint32_t bits = in->w - 64 * i;
            sb_printf(o, "v[%u] = ", in->d + i);
            sc_get64(g, in->a, in->aw, (uint64_t)in->p + 64u * i, bits > 64 ? 64 : bits);
            sb_puts(o, "; ");
        }
        break;
    case SOP_WRESIZE: {
        uint32_t nd = SC_LIMBS(in->w), na = SC_LIMBS(in->aw);
        if (in->sgn || !in->w) {
            sc_callback(g, pc);
            break;
        }
        for (i = 0; i < nd; i++) {
            if (in->w <= in->aw || i < na) {
                if (in->d + i != in->a + i)
                    sb_printf(o, "v[%u] = v[%u]; ", in->d + i, in->a + i);
            } else {
                sb_printf(o, "v[%u] = 0; ", in->d + i);
            }
        }
        if (in->w % 64) {
            sb_printf(o, "v[%u] &= ", in->d + nd - 1);
            sc_lit(g, sc_top_mask(in->w));
            sb_puts(o, ";");
        }
        break;
    }
    case SOP_WDEPOSIT: {
        uint32_t na = SC_LIMBS(in->aw);
        if (in->d < in->a + na && in->a < in->d + SC_LIMBS(in->w)) {
            sc_callback(g, pc);
            break;
        }
        sb_puts(o, "{ tu64 t;");
        for (i = 0; i < na; i++) {
            uint32_t bits = in->aw - 64 * i;
            uint64_t p = (uint64_t)in->p + 64u * i;
            if (p >= in->w)
                break;
            sb_printf(o, " t = v[%u];", in->a + i);
            sc_put_bits(g, in->d, in->w, p, bits > 64 ? 64 : bits);
        }
        sb_puts(o, " (void)t; }");
        break;
    }
    case SOP_MEMREAD: {
        const SimMem *mm = &m->mems.data[in->p];
        if (mm->limbs != 1 || in->aw > 64 || in->w > 64) {
            sc_callback(g, pc);
            break;
        }
        sb_puts(o, "{ tu64 ad = ");
        sc_rd(g, in->a);
        sb_printf(o, "; %s = ad < %uu ? rt->mem[%u][ad] : 0; }", sc_nm(g, in->d), mm->depth, in->p);
        break;
    }
    case SOP_JMP: sb_printf(o, "goto L%u;", in->p); break;
    case SOP_JZ:
        sb_puts(o, "if (!");
        sc_rd(g, in->a);
        sb_printf(o, ") goto L%u;", in->p);
        break;
    case SOP_SWITCH: {
        const SimSwitch *t = &m->switches.data[in->p];
        sb_puts(o, "switch (");
        sc_rd(g, in->a);
        sb_puts(o, ") {");
        for (i = 0; i < t->n; i++) {
            if (i && t->vals[i] == t->vals[i - 1])
                continue;
            sb_puts(o, " case ");
            sc_lit(g, t->vals[i]);
            sb_printf(o, ": goto L%u;", t->targets[i]);
        }
        sb_printf(o, " default: goto L%u; }", t->dflt);
        break;
    }
    case SOP_ASSERT:
        sb_printf(o, "rt->assert_hit(rt->m, %uu, ", in->p);
        sc_rd(g, in->a);
        sb_puts(o, ");");
        break;
    case SOP_COV: sb_printf(o, "cov[%u]++;", in->p); break;
    case SOP_FORCE:
        sb_puts(o, "if (*rt->nforced) ");
        sc_callback(g, pc);
        break;
    default:
        /* XINDEX, DIVS/MODS, WEXTRACT/WINDEX/WDEPOSIT_DYN, MEMWRITE, ... */
        sc_callback(g, pc);
        break;
    }
    sb_putc(o, '\n');
    return 1;
}

static int sc_targets(const SimModel *m, uint32_t pc, uint32_t *out, uint32_t max)
{
    const SimInsn *in = &m->code.data[pc];
    uint32_t n = 0, i;
    if (in->op == SOP_JMP || in->op == SOP_JZ) {
        out[n++] = in->p;
    } else if (in->op == SOP_SWITCH) {
        const SimSwitch *t = &m->switches.data[in->p];
        if (t->n + 1 > max)
            return 0;
        for (i = 0; i < t->n; i++)
            out[n++] = t->targets[i];
        out[n++] = t->dflt;
    }
    return (int)n;
}

static uint32_t sc_max_targets(const SimModel *m)
{
    uint32_t i, n = 2;
    for (i = 0; i < m->switches.len; i++)
        if (m->switches.data[i].n + 1 > n)
            n = m->switches.data[i].n + 1;
    return n;
}

/* the operands of an instruction as limb ranges: reads may be
 * over-approximated, writes must not be (both only make temporaries stay
 * in v[] more often) */
typedef struct ScOpnd {
    uint32_t off, limbs;
    int write;
} ScOpnd;

static uint32_t sc_maxl(uint32_t a, uint32_t b)
{
    a = SC_LIMBS(a);
    b = SC_LIMBS(b);
    a = a > b ? a : b;
    return a ? a : 1;
}

static int sc_operands(const SimInsn *in, ScOpnd *o)
{
    int n = 0;
#define SC_R(x, l) (o[n].off = (x), o[n].limbs = (l), o[n].write = 0, n++)
#define SC_W(x, l) (o[n].off = (x), o[n].limbs = (l), o[n].write = 1, n++)
    switch ((SimOp)in->op) {
    case SOP_JMP:
    case SOP_COV: break;
    case SOP_JZ:
    case SOP_SWITCH:
    case SOP_ASSERT: SC_R(in->a, 1); break;
    case SOP_COPY:
    case SOP_SLICE:
    case SOP_NOT:
    case SOP_NEG:
    case SOP_LNOT:
    case SOP_RAND:
    case SOP_ROR:
    case SOP_RXOR:
    case SOP_SEXT:
        SC_R(in->a, 1);
        SC_W(in->d, 1);
        break;
    case SOP_XSLICE:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_W(in->d, 1);
        break;
    case SOP_XINDEX:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->b, 1);
        SC_W(in->d, 1);
        break;
    case SOP_INDEX:
    case SOP_ADD:
    case SOP_SUB:
    case SOP_MUL:
    case SOP_DIVU:
    case SOP_DIVS:
    case SOP_MODU:
    case SOP_MODS:
    case SOP_AND:
    case SOP_OR:
    case SOP_XOR:
    case SOP_SHL:
    case SOP_SHR:
    case SOP_ASHR:
    case SOP_EQ:
    case SOP_NE:
    case SOP_LTU:
    case SOP_LTS:
    case SOP_LEU:
    case SOP_LES:
    case SOP_LAND:
    case SOP_LOR:
    case SOP_SHLOR:
        SC_R(in->a, 1);
        SC_R(in->b, 1);
        SC_W(in->d, 1);
        break;
    case SOP_MUX:
        SC_R(in->a, 1);
        SC_R(in->b, 1);
        SC_R(in->c, 1);
        SC_W(in->d, 1);
        break;
    case SOP_DEPOSIT:
        SC_R(in->a, 1);
        SC_R(in->d, 1);
        SC_W(in->d, 1);
        break;
    case SOP_DEPOSIT_DYN:
        SC_R(in->a, 1);
        SC_R(in->b, 1);
        SC_R(in->d, 1);
        SC_W(in->d, 1);
        break;
    case SOP_FORCE: SC_W(in->d, sc_maxl(in->w, 1)); break;
    case SOP_MEMREAD:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_MEMWRITE:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->b, sc_maxl(in->w, 1));
        if (in->sub)
            SC_R(in->c, sc_maxl(in->bw, 1));
        break;
    case SOP_WCOPY:
        SC_R(in->a, sc_maxl(in->w, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WZERO: SC_W(in->d, sc_maxl(in->w, 1)); break;
    case SOP_WEXTRACT:
    case SOP_WRESIZE:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WINDEX:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->b, sc_maxl(in->bw, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WUN:
        SC_R(in->a, sc_maxl(in->w, in->aw));
        SC_W(in->d, in->sub == IROP_NOT || in->sub == IROP_NEG ? sc_maxl(in->w, 1) : 1);
        break;
    case SOP_WBIN:
        SC_R(in->a, sc_maxl(in->w, 1));
        SC_R(in->b, sc_maxl(in->w, in->bw));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WCMP:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->b, sc_maxl(in->aw, 1));
        SC_W(in->d, 1);
        break;
    case SOP_WMUX:
        SC_R(in->a, 1);
        SC_R(in->b, sc_maxl(in->w, 1));
        SC_R(in->c, sc_maxl(in->w, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WDEPOSIT:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->d, sc_maxl(in->w, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    case SOP_WDEPOSIT_DYN:
        SC_R(in->a, sc_maxl(in->aw, 1));
        SC_R(in->b, sc_maxl(in->bw, 1));
        SC_R(in->d, sc_maxl(in->w, 1));
        SC_W(in->d, sc_maxl(in->w, 1));
        break;
    }
#undef SC_R
#undef SC_W
    return n;
}

static int sc_is_wide_op(const SimInsn *in)
{
    return in->op >= SOP_WCOPY && in->op <= SOP_WDEPOSIT_DYN;
}

static int sc_is_temp(const SimModel *m, uint32_t x)
{
    return x >= m->temp_base && x < m->misc_base;
}

/* chunk [s, e) of a region */
typedef struct ScChunk {
    uint32_t region, s, e;
} ScChunk;
typedef VEC(ScChunk) ScChunkVec;

/* splits [lo, hi) into chunks no jump crosses and marks the labels;
 * 0 if a jump leaves the region */
static int sc_chunks(ScGen *g, uint32_t region, uint32_t lo, uint32_t hi, uint32_t *tg, uint32_t tgcap,
                     ScChunkVec *out)
{
    SimModel *m = g->m;
    uint32_t len = hi - lo, pc, start, k, ntemp = m->misc_base - m->temp_base;
    int32_t *cover;
    uint32_t *lastw;
    ScOpnd ops[8];
    if (hi <= lo)
        return 1;
    cover = (int32_t *)xcalloc((size_t)len + 2, sizeof(int32_t));
    lastw = (uint32_t *)xmalloc(sizeof(uint32_t) * ((size_t)ntemp + 1));
    memset(lastw, 0xFF, sizeof(uint32_t) * ((size_t)ntemp + 1));
    for (pc = lo; pc < hi; pc++) {
        int no = sc_operands(&m->code.data[pc], ops), kk;
        uint32_t l;
        /* a temporary's value flows from its write to its reads: keep them together */
        for (kk = 0; kk < no; kk++)
            for (l = 0; l < ops[kk].limbs && !ops[kk].write; l++) {
                uint32_t x = ops[kk].off + l;
                if (sc_is_temp(m, x) && lastw[x - m->temp_base] != 0xFFFFFFFFu) {
                    cover[lastw[x - m->temp_base] + 1 - lo]++;
                    cover[pc - lo + 1]--;
                }
            }
        for (kk = 0; kk < no; kk++)
            for (l = 0; l < ops[kk].limbs && ops[kk].write; l++)
                if (sc_is_temp(m, ops[kk].off + l))
                    lastw[ops[kk].off + l - m->temp_base] = pc;
    }
    free(lastw);
    for (pc = lo; pc < hi; pc++) {
        int nt = sc_targets(m, pc, tg, tgcap), j;
        for (j = 0; j < nt; j++) {
            uint32_t t = tg[j], a, b;
            if (t < lo || t > hi) {
                free(cover);
                return 0;
            }
            g->label[t] = 1;
            /* boundary k (between k-1 and k) may not separate a jump from its label */
            if (t > pc) {
                a = pc + 1;
                b = t - 1;
            } else {
                a = t + 1;
                b = pc;
            }
            if (a <= b) {
                cover[a - lo]++;
                cover[b - lo + 1]--;
            }
        }
    }
    for (k = 1; k <= len; k++)
        cover[k] += cover[k - 1];
    start = lo;
    while (start < hi) {
        ScChunk c;
        uint32_t end = start + SC_CHUNK < hi ? start + SC_CHUNK : hi;
        while (end < hi && cover[end - lo])
            end++;
        c.region = region;
        c.s = start;
        c.e = end;
        vec_push(*out, c);
        start = end;
    }
    free(cover);
    return 1;
}

static void sc_mark(const SimModel *m, uint8_t *bad, const ScOpnd *op, uint8_t val)
{
    uint32_t l;
    for (l = 0; l < op->limbs; l++)
        if (sc_is_temp(m, op->off + l))
            bad[op->off + l - m->temp_base] = val;
}

/* A temporary can be a C local of a chunk function when no chunk reads it
 * before writing it on some path (then no value ever flows between chunks,
 * or to the interpreter, through v[]) and, in that chunk, it is only used
 * by inlined narrow instructions.  This marks the first kind (tbad). */
static void sc_liveness(ScGen *g, const ScChunkVec *chunks, uint32_t *tg, uint32_t tgcap)
{
    SimModel *m = g->m;
    uint32_t nt = m->misc_base - m->temp_base, words = (nt + 31) / 32, ci, pc, j;
    uint32_t *cur;
    ScOpnd ops[8];
    if (!nt)
        return;
    cur = (uint32_t *)xcalloc((size_t)words, sizeof(uint32_t));
    /* reads before writes, per chunk (forward jumps only) */
    for (ci = 0; ci < chunks->len; ci++) {
        const ScChunk *c = &chunks->data[ci];
        uint32_t len = c->e - c->s;
        uint32_t **pend = (uint32_t **)xcalloc((size_t)len + 1, sizeof(uint32_t *));
        int reach = 1, backward = 0;
        memset(cur, 0, sizeof(uint32_t) * words);
        for (pc = c->s; pc < c->e; pc++) {
            const SimInsn *in = &m->code.data[pc];
            uint32_t *pd = pend[pc - c->s];
            int no, k, ntg;
            if (pd) {
                if (reach)
                    for (j = 0; j < words; j++)
                        cur[j] &= pd[j];
                else
                    memcpy(cur, pd, sizeof(uint32_t) * words);
                reach = 1;
            } else if (!reach) {
                for (j = 0; j < words; j++)
                    cur[j] = ~0u; /* unreachable code reads nothing */
                reach = 1;
            }
            no = sc_operands(in, ops);
            for (k = 0; k < no; k++) {
                uint32_t l;
                if (ops[k].write)
                    continue;
                for (l = 0; l < ops[k].limbs; l++) {
                    uint32_t x = ops[k].off + l - m->temp_base;
                    if (sc_is_temp(m, ops[k].off + l) && !(cur[x / 32] & (1u << (x % 32))))
                        g->tbad[x] = 1;
                }
            }
            for (k = 0; k < no; k++) {
                uint32_t l;
                if (!ops[k].write)
                    continue;
                for (l = 0; l < ops[k].limbs; l++) {
                    uint32_t x = ops[k].off + l - m->temp_base;
                    if (sc_is_temp(m, ops[k].off + l))
                        cur[x / 32] |= 1u << (x % 32);
                }
            }
            ntg = sc_targets(m, pc, tg, tgcap);
            for (k = 0; k < ntg; k++) {
                uint32_t t = tg[k];
                if (t <= pc) {
                    backward = 1;
                    continue;
                }
                if (t >= c->e)
                    continue;
                if (!pend[t - c->s]) {
                    pend[t - c->s] = (uint32_t *)xmalloc(sizeof(uint32_t) * words);
                    memcpy(pend[t - c->s], cur, sizeof(uint32_t) * words);
                } else {
                    for (j = 0; j < words; j++)
                        pend[t - c->s][j] &= cur[j];
                }
            }
            if (in->op == SOP_JMP || in->op == SOP_SWITCH)
                reach = 0;
        }
        if (backward) { /* a loop: keep every temporary of the chunk in v[] */
            for (pc = c->s; pc < c->e; pc++) {
                int no = sc_operands(&m->code.data[pc], ops), k;
                for (k = 0; k < no; k++)
                    sc_mark(m, g->tbad, &ops[k], 1);
            }
        }
        for (j = 0; j <= len; j++)
            free(pend[j]);
        free(pend);
    }
    free(cur);
}

static void sc_chunk_fn(ScGen *g, const ScChunk *c, uint32_t fn)
{
    SimModel *m = g->m;
    uint32_t pc, nt = m->misc_base - m->temp_base;
    int calls = 0;
    ScOpnd ops[8];
    StrBuf body = {0};
    StrBuf *save = g->o;
    /* wide and callback operands stay in v[] in this chunk */
    for (pc = c->s; pc < c->e; pc++) {
        const SimInsn *in = &m->code.data[pc];
        int no = sc_operands(in, ops), k;
        for (k = 0; k < no; k++)
            if (ops[k].limbs != 1 || g->cb[pc] || sc_is_wide_op(in))
                sc_mark(m, g->cbad, &ops[k], 1);
    }
    g->o = &body;
    for (pc = c->s; pc < c->e; pc++) {
        if (g->label[pc])
            sb_printf(&body, "L%u:;\n", pc);
        sc_insn(g, pc);
        if (g->cb[pc] || m->code.data[pc].op == SOP_ASSERT)
            calls = 1;
    }
    if (g->label[c->e])
        sb_printf(&body, "L%u:;\n", c->e);
    g->o = save;
    /* restrict lets the C compiler keep values in registers; it is only
     * valid while nothing else (a callback) reads or writes v[] and cov[] */
    sb_printf(g->o, "static void tern_f%u(TernRt *rt)\n{\n", fn);
    sb_printf(g->o, "  tu64 *%sv = rt->v;\n  tu64 *%scov = rt->cov;\n", calls ? "" : "TERN_RESTRICT ",
              calls ? "" : "TERN_RESTRICT ");
    if (nt) {
        uint8_t *seen = (uint8_t *)xcalloc(nt, 1);
        for (pc = c->s; pc < c->e; pc++) {
            int no = sc_operands(&m->code.data[pc], ops), k;
            for (k = 0; k < no; k++) {
                uint32_t x = ops[k].off;
                if (!sc_is_local(g, x) || seen[x - m->temp_base])
                    continue;
                seen[x - m->temp_base] = 1;
                sb_printf(g->o, "  tu64 t%u = 0;\n", x);

            }
        }
        free(seen);
    }
    sb_append(g->o, body.data ? body.data : "", body.len);
    sb_puts(g->o, "  (void)v; (void)cov;\n}\n\n");
    for (pc = c->s; pc < c->e; pc++) {
        int no = sc_operands(&m->code.data[pc], ops), k;
        for (k = 0; k < no; k++)
            sc_mark(m, g->cbad, &ops[k], 0);
    }
    sb_free(&body);
}

static const char sc_prelude[] =
    "/* generated by tern: compiled simulation model (do not edit) */\n"
    "typedef unsigned long long tu64;\n"
    "typedef struct TernRt {\n"
    "  tu64 *v;\n"
    "  tu64 *cov;\n"
    "  tu64 **mem;\n"
    "  const unsigned *nforced;\n"
    "  void *m;\n"
    "  void (*insn)(void *m, unsigned pc);\n"
    "  void (*assert_hit)(void *m, unsigned id, tu64 val);\n"
    "} TernRt;\n"
    "#if defined(_WIN32)\n"
    "#define TERN_EXPORT __declspec(dllexport)\n"
    "#else\n"
    "#define TERN_EXPORT\n"
    "#endif\n"
    "#if defined(_MSC_VER)\n"
    "#define TERN_RESTRICT __restrict\n"
    "#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L\n"
    "#define TERN_RESTRICT restrict\n"
    "#else\n"
    "#define TERN_RESTRICT\n"
    "#endif\n"
    "static tu64 tern_parity(tu64 x)\n"
    "{\n"
    "  x ^= x >> 32; x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;\n"
    "  return x & 1u;\n"
    "}\n\n";

/* the C source of m's model (without the trailing tern_id()); 0 if the
 * instruction list cannot be translated */
static int sc_generate(SimModel *m, StrBuf *o)
{
    ScGen g;
    StrBuf scratch = {0};
    StrBuf exec = {0};
    ScChunkVec chunks = {0};
    uint32_t i, r, tgcap = sc_max_targets(m);
    uint32_t *tg = (uint32_t *)xcalloc(tgcap, sizeof(uint32_t));
    uint32_t nt = m->misc_base - m->temp_base;
    int ok = 1;
    memset(&g, 0, sizeof g);
    g.m = m;
    g.inline_consts = 1;
    g.label = (uint8_t *)xcalloc(m->code.len + 1, 1);
    g.cb = (uint8_t *)xcalloc(m->code.len + 1, 1);
    g.tbad = (uint8_t *)xcalloc((size_t)nt + 1, 1);
    g.cbad = (uint8_t *)xcalloc((size_t)nt + 1, 1);
    for (i = 0; i < m->code.len; i++) {
        uint32_t d = m->code.data[i].d;
        if (m->code.data[i].op != SOP_JMP && m->code.data[i].op != SOP_JZ && d >= m->const_base &&
            d < m->temp_base)
            g.inline_consts = 0;
    }
    for (r = 0; r < 1 + m->doms.len + m->groups.len && ok; r++) {
        uint32_t lo, hi;
        if (r == 0) {
            lo = 0;
            hi = m->comb_end;
        } else if (r <= m->doms.len) {
            lo = m->doms.data[r - 1].code_lo;
            hi = m->doms.data[r - 1].code_hi;
        } else {
            lo = m->groups.data[r - 1 - m->doms.len].pcode_lo;
            hi = m->groups.data[r - 1 - m->doms.len].pcode_hi;
        }
        ok = sc_chunks(&g, r, lo, hi, tg, tgcap, &chunks);
    }
    if (ok) {
        /* a dry run finds the instructions that become callbacks */
        g.o = &scratch;
        for (i = 0; i < m->code.len; i++) {
            sc_insn(&g, i);
            sb_reset(&scratch);
        }
        sc_liveness(&g, &chunks, tg, tgcap);
        g.locals = 1;
        g.o = o;
        sb_puts(o, sc_prelude);
        for (i = 0; i < chunks.len; i++)
            sc_chunk_fn(&g, &chunks.data[i], i);
        sb_puts(&exec, "TERN_EXPORT void tern_exec(TernRt *rt, unsigned region)\n{\n  switch (region) {\n");
        for (i = 0; i < chunks.len; i++) {
            if (!i || chunks.data[i].region != chunks.data[i - 1].region)
                sb_printf(&exec, "  case %u:\n", chunks.data[i].region);
            sb_printf(&exec, "    tern_f%u(rt);\n", i);
            if (i + 1 == chunks.len || chunks.data[i + 1].region != chunks.data[i].region)
                sb_puts(&exec, "    break;\n");
        }
        sb_puts(&exec, "  default: break;\n  }\n}\n\n");
        sb_printf(&exec, "TERN_EXPORT unsigned tern_abi(void)\n{\n  return %uu;\n}\n\n", SC_ABI);
        sb_append(o, exec.data ? exec.data : "", exec.len);
    }
    free(tg);
    free(g.label);
    free(g.cb);
    free(g.tbad);
    free(g.cbad);
    vec_free(chunks);
    sb_free(&scratch);
    sb_free(&exec);
    return ok;
}

char *sim_c_source(SimModel *m)
{
    StrBuf o = {0};
    if (!m || m->rops || !sc_generate(m, &o)) {
        sb_free(&o);
        return NULL;
    }
    return sb_take(&o);
}

/* ---------------------------------------------------------- runtime -- */

static void sc_rt_insn(void *m, unsigned pc)
{
    sim_exec((SimModel *)m, pc, pc + 1);
}

static void sc_rt_assert(void *m, unsigned id, unsigned long long val)
{
    sim_assert_hit((SimModel *)m, id, (uint64_t)val);
}

static SimC *sc_get(SimModel *m)
{
    if (!m->simc)
        m->simc = (SimC *)xcalloc(1, sizeof(SimC));
    return m->simc;
}

/* cl leaves an import library, an export file and the object next to the DLL */
static void sc_remove_msvc_extras(const char *out)
{
    static const char *const exts[] = {".lib", ".exp", ".obj"};
    size_t i, n;
    if (!sc_cc_msvc || !out)
        return;
    n = strlen(out) - strlen(os_dl_ext());
    for (i = 0; i < TERN_ARRAY_LEN(exts); i++) {
        StrBuf p = {0};
        sb_append(&p, out, n);
        sb_puts(&p, exts[i]);
        os_remove(sb_cstr(&p));
        sb_free(&p);
    }
}

static void sc_clear_job(SimC *c)
{
    if (c->proc) {
        os_proc_free(c->proc);
        c->proc = NULL;
    }
    if (c->src_path) {
        char *keep = os_getenv("TERN_SIMC_KEEP");
        if (keep && *keep && c->final_path) {
            /* debugging: keep the source as <cache>/<hash>.c */
            StrBuf p = {0};
            sb_append(&p, c->final_path, strlen(c->final_path) - strlen(os_dl_ext()));
            sb_puts(&p, ".c");
            os_rename(c->src_path, sb_cstr(&p));
            sb_free(&p);
        }
        free(keep);
        os_remove(c->src_path);
        free(c->src_path);
        c->src_path = NULL;
    }
    if (c->tmp_out) {
        os_remove(c->tmp_out);
        sc_remove_msvc_extras(c->tmp_out);
        free(c->tmp_out);
        c->tmp_out = NULL;
    }
}

/* load a library and check it is the model we expect; 1 on success */
static int sc_load(SimModel *m, const char *path, const char *id, char **err)
{
    SimC *c = sc_get(m);
    void *dl = os_dl_open(path, err);
    SimCAbiFn abi;
    SimCIdFn idf;
    SimCExecFn ex;
    uint32_t i;
    if (!dl)
        return 0;
    abi = (SimCAbiFn)os_dl_sym(dl, "tern_abi");
    idf = (SimCIdFn)os_dl_sym(dl, "tern_id");
    ex = (SimCExecFn)os_dl_sym(dl, "tern_exec");
    if (!abi || !idf || !ex || abi() != SC_ABI || strcmp(idf(), id) != 0) {
        if (err)
            *err = xstrdup("the library is not this model (stale cache entry)");
        os_dl_close(dl);
        return 0;
    }
    c->dl = dl;
    c->exec = ex;
    c->mems = (uint64_t **)xcalloc(m->mems.len + 1, sizeof(uint64_t *));
    for (i = 0; i < m->mems.len; i++)
        c->mems[i] = m->mems.data[i].data;
    c->rt.v = m->v;
    c->rt.cov = m->cov;
    c->rt.mem = c->mems;
    c->rt.nforced = &m->nforced;
    c->rt.m = m;
    c->rt.insn = sc_rt_insn;
    c->rt.assert_hit = sc_rt_assert;
    return 1;
}

static char *sc_hex64(uint64_t h)
{
    char buf[17];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return xstrdup(buf);
}

/* generate, look up the cache, and start the compiler when needed.
 * Returns 1 when the model is loaded already, 2 when a compile is running,
 * 0 on failure (*err set). */
static int sc_begin(SimModel *m, char **err)
{
    SimC *c = sc_get(m);
    StrBuf src = {0}, key = {0}, p = {0};
    char *dir, *hex, *idstr;
    uint64_t h;
    size_t i;
    OsSpawnOpts so;
    VEC(char *) argv = {0};
    const char *ext = os_dl_ext();
    if (!sim_host_cc()) {
        *err = xstrdup("no host C compiler found ($CC, cc, gcc, clang, cl)");
        return 0;
    }
    if (!sc_generate(m, &src)) {
        sb_free(&src);
        *err = xstrdup("the design's instruction list cannot be translated to C");
        return 0;
    }
    /* key: source, compiler, flags, ABI */
    for (i = 0; i < sc_cc_argv.len; i++)
        sb_printf(&key, "%s\n", sc_cc_argv.data[i]);
    sb_printf(&key, "msvc=%d abi=%u -O1\n", sc_cc_msvc, SC_ABI);
    h = hash_combine(hash_bytes(src.data, src.len), hash_bytes(key.data, key.len));
    h = hash_combine(h, (uint64_t)src.len);
    hex = sc_hex64(h);
    {
        StrBuf id = {0};
        sb_printf(&id, "tern-model-%s-%llu", hex, (unsigned long long)src.len);
        idstr = sb_take(&id);
    }
    sb_printf(&src, "TERN_EXPORT const char *tern_id(void)\n{\n  return \"%s\";\n}\n", idstr);
    dir = sc_cache_dir();
    os_mkdir_p(dir);
    sb_printf(&p, "%s/%s%s", dir, hex, ext);
    c->final_path = sb_take(&p);
    c->id = idstr;
    if (os_is_file(c->final_path)) {
        char *e = NULL;
        if (sc_load(m, c->final_path, c->id, &e)) {
            free(e);
            sb_free(&src);
            sb_free(&key);
            free(dir);
            free(hex);
            return 1;
        }
        free(e);
        os_remove(c->final_path); /* stale or damaged: rebuild it */
    }
    sc_counter++;
    sb_printf(&p, "%s/%s-%llu-%u.c", dir, hex, (unsigned long long)os_process_id(), sc_counter);
    c->src_path = sb_take(&p);
    sb_printf(&p, "%s/%s-%llu-%u%s", dir, hex, (unsigned long long)os_process_id(), sc_counter, ext);
    c->tmp_out = sb_take(&p);
    free(dir);
    free(hex);
    sb_free(&key);
    if (os_write_file(c->src_path, src.data, src.len) != 0) {
        *err = xstrdup("cannot write the generated model to the simulation cache");
        sb_free(&src);
        sc_clear_job(c);
        return 0;
    }
    sb_free(&src);
    for (i = 0; i < sc_cc_argv.len; i++)
        vec_push(argv, xstrdup(sc_cc_argv.data[i]));
    if (sc_cc_msvc) {
        size_t n = strlen(c->tmp_out) - strlen(ext);
        vec_push(argv, xstrdup("/nologo"));
        vec_push(argv, xstrdup("/O1"));
        vec_push(argv, xstrdup("/LD"));
        sb_printf(&p, "/Fe%s", c->tmp_out);
        vec_push(argv, sb_take(&p));
        sb_puts(&p, "/Fo");
        sb_append(&p, c->tmp_out, n);
        sb_puts(&p, ".obj");
        vec_push(argv, sb_take(&p));
        vec_push(argv, xstrdup(c->src_path));
    } else {
        vec_push(argv, xstrdup("-O1"));
        vec_push(argv, xstrdup("-shared"));
        vec_push(argv, xstrdup("-fPIC"));
        vec_push(argv, xstrdup("-o"));
        vec_push(argv, xstrdup(c->tmp_out));
        vec_push(argv, xstrdup(c->src_path));
    }
    vec_push(argv, NULL);
    memset(&so, 0, sizeof so);
    so.capture_stdout = 1;
    so.capture_stderr = 1;
    so.stdin_null = 1;
    c->proc = os_spawn(argv.data, &so, err);
    for (i = 0; i < argv.len; i++)
        free(argv.data[i]);
    vec_free(argv);
    if (!c->proc) {
        if (!*err)
            *err = xstrdup("cannot start the host C compiler");
        sc_clear_job(c);
        return 0;
    }
    return 2;
}

/* the compile has finished: install the library and load it */
static int sc_finish(SimModel *m, char **err)
{
    SimC *c = sc_get(m);
    int code = os_proc_exit_code(c->proc);
    int ok = 0;
    if (code != 0 || !os_is_file(c->tmp_out)) {
        size_t elen, olen;
        const char *e = os_proc_stderr(c->proc, &elen);
        const char *out = os_proc_stdout(c->proc, &olen);
        StrBuf sb = {0};
        sb_printf(&sb, "the host C compiler (%s) failed with status %d", sc_cc_display, code);
        if (elen || olen) {
            const char *s = elen ? e : out;
            size_t n = elen ? elen : olen;
            sb_puts(&sb, ":\n");
            sb_append(&sb, s, n > 1500 ? 1500 : n);
        }
        *err = sb_take(&sb);
        sc_clear_job(c);
        return 0;
    }
    sc_remove_msvc_extras(c->tmp_out);
    if (os_rename(c->tmp_out, c->final_path) == 0) {
        free(c->tmp_out);
        c->tmp_out = NULL;
        ok = sc_load(m, c->final_path, c->id, err);
    } else if (os_is_file(c->final_path)) {
        /* another worker installed it first (Windows cannot replace a loaded DLL) */
        ok = sc_load(m, c->final_path, c->id, err);
    } else {
        ok = sc_load(m, c->tmp_out, c->id, err);
        /* keep tmp_out until the model is freed (it is loaded) */
        if (ok) {
            os_proc_free(c->proc);
            c->proc = NULL;
            os_remove(c->src_path);
            free(c->src_path);
            c->src_path = NULL;
            return 1;
        }
    }
    sc_clear_job(c);
    return ok;
}

int sim_compile_start(SimModel *m)
{
    char *err = NULL;
    SimC *c;
    int r;
    if (!m || m->rops)
        return -1;
    c = sc_get(m);
    if (c->exec || c->proc)
        return 0;
    r = sc_begin(m, &err);
    if (r == 0) {
        diag_error(span_none(), "compiled simulation: %s", err ? err : "failed");
        free(err);
        c->failed = 1;
        return -1;
    }
    return 0;
}

int sim_compile(SimModel *m)
{
    char *err = NULL;
    SimC *c;
    if (sim_compile_start(m) != 0)
        return -1;
    c = m->simc;
    if (c->exec)
        return 0;
    os_proc_wait(c->proc);
    if (!sc_finish(m, &err)) {
        diag_error(span_none(), "compiled simulation: %s", err ? err : "failed");
        free(err);
        c->failed = 1;
        return -1;
    }
    m->work_next = ~UINT64_C(0);
    return 0;
}

int sim_is_compiled(const SimModel *m)
{
    return m && m->simc && m->simc->exec;
}

/* ---------------------------------------------------- engine hookup -- */

void sim_exec_region(SimModel *m, uint32_t region, uint32_t lo, uint32_t hi)
{
    SimC *c = m->simc;
    if (c && c->exec) {
        c->exec(&c->rt, region);
        return;
    }
    sim_exec(m, lo, hi);
    m->work += (uint64_t)(hi - lo) + 1u;
    if (m->work >= m->work_next)
        simc_tick(m);
}

void simc_tick(SimModel *m)
{
    SimC *c = sc_get(m);
    char *err = NULL;
    if (c->exec || c->failed) {
        m->work_next = ~UINT64_C(0);
        return;
    }
    m->work_next = m->work + SC_POLL_WORK;
    if (!c->proc) {
        int r = sc_begin(m, &err);
        free(err);
        if (r != 2) {
            if (r == 0)
                c->failed = 1;
            m->work_next = ~UINT64_C(0);
        }
        return;
    }
    {
        OsProc *one = c->proc;
        if (os_proc_wait_any(&one, 1, 0) != 0)
            return; /* still compiling */
    }
    if (!sc_finish(m, &err))
        c->failed = 1;
    free(err);
    m->work_next = ~UINT64_C(0);
}

int simc_setup(SimModel *m)
{
    SimEngine e = sc_default_engine;
    m->work_next = ~UINT64_C(0);
    if (m->rops)
        return 1;
    if (e == SIM_ENGINE_COMPILED)
        return sim_compile(m) == 0;
    if (e == SIM_ENGINE_AUTO && sim_host_cc())
        m->work_next = SC_AUTO_WORK;
    return 1;
}

void simc_free(SimModel *m)
{
    SimC *c = m->simc;
    if (!c)
        return;
    if (c->dl)
        os_dl_close(c->dl);
    sc_clear_job(c);
    free(c->mems);
    free(c->final_path);
    free(c->id);
    free(c);
    m->simc = NULL;
}

#undef SC_ABI
#undef SC_CHUNK
#undef SC_AUTO_WORK
#undef SC_POLL_WORK
#undef SC_LIMBS
