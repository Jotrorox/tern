/* sv.c — IR -> SystemVerilog (IEEE 1800-2012) / Verilog-2005 emitter.
 *
 * One file per module, a package with the named struct/enum types and the
 * memory initialization files (see emit.h for the API and
 * docs/emitted-sv.md for the conventions).
 *
 * Exactness.  The emitted code computes exactly what the IR means under the
 * SystemVerilog sizing/signing rules.  The IR already gives every operand the
 * result width, so the emitter keeps one invariant: the self-determined width
 * of every emitted subexpression equals its IR width, and widths change only
 * through explicit, self-determining constructs (concatenations, size casts,
 * selects, temporaries).  Signedness matters only for / % >>> and the
 * relational operators; those get explicit $signed()/$unsigned() operands,
 * and a signed operation that sits in a context-determined position is
 * wrapped in $unsigned(...) so that the surrounding expression cannot turn
 * it unsigned (IEEE 1800-2017 11.8.2).
 */
#include "emit.h"
#include "svparam.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/map.h"
#include "../base/diag.h"
#include "../os/os.h"

/* ============================================================== output == */

void sv_options_init(SvOptions *o)
{
    memset(o, 0, sizeof *o);
    o->std = SV_STD_SV2012;
    o->target_kind = SV_TARGET_FPGA;
    o->line_markers = 1;
    o->constraints = EMIT_CONS_XDC | EMIT_CONS_SDC;
    o->regmaps = 1;
}

void sv_output_add(SvOutput *o, const char *name, SvFileKind kind, StrBuf *contents)
{
    size_t i;
    SvFile *f = NULL;
    for (i = 0; i < o->nfiles; i++)
        if (strcmp(o->files[i].name, name) == 0) {
            f = &o->files[i];
            free(f->data);
            break;
        }
    if (!f) {
        if (o->nfiles == o->cap)
            o->files = (SvFile *)tern_vec_grow(o->files, &o->cap, o->nfiles + 1, sizeof(SvFile));
        f = &o->files[o->nfiles++];
        f->name = xstrdup(name);
    }
    f->kind = kind;
    f->len = contents->len;
    f->data = sb_take(contents);
}

const SvFile *sv_output_find(const SvOutput *o, const char *name)
{
    size_t i;
    for (i = 0; i < o->nfiles; i++)
        if (strcmp(o->files[i].name, name) == 0)
            return &o->files[i];
    return NULL;
}

int sv_output_write_dir(const SvOutput *o, const char *dir)
{
    size_t i;
    if (os_mkdir_p(dir) != 0) {
        diag_error(span_none(), "cannot create output directory `%s`", dir);
        return -1;
    }
    for (i = 0; i < o->nfiles; i++) {
        char *p = os_path_join(dir, o->files[i].name);
        int rc = os_write_file_if_changed(p, o->files[i].data, o->files[i].len);
        if (rc < 0) {
            diag_error(span_none(), "cannot write `%s`", p);
            free(p);
            return -1;
        }
        free(p);
    }
    return 0;
}

void sv_output_concat(const SvOutput *o, StrBuf *out)
{
    size_t i;
    for (i = 0; i < o->nfiles; i++) {
        sb_printf(out, "// ---- file: %s ----\n", o->files[i].name);
        sb_append(out, o->files[i].data, o->files[i].len);
        if (o->files[i].len && o->files[i].data[o->files[i].len - 1] != '\n')
            sb_putc(out, '\n');
    }
}

void sv_output_free(SvOutput *o)
{
    size_t i;
    for (i = 0; i < o->nfiles; i++) {
        free(o->files[i].name);
        free(o->files[i].data);
    }
    free(o->files);
    for (i = 0; i < o->nextern; i++)
        free(o->extern_files[i]);
    free(o->extern_files);
    memset(o, 0, sizeof *o);
}

static void sv_output_add_extern(SvOutput *o, const char *path)
{
    size_t i;
    for (i = 0; i < o->nextern; i++)
        if (strcmp(o->extern_files[i], path) == 0)
            return;
    o->extern_files = (char **)xrealloc(o->extern_files, sizeof(char *) * (o->nextern + 1));
    o->extern_files[o->nextern++] = xstrdup(path);
}

int emit_design(const IrDesign *d, const SvOptions *o, SvOutput *out)
{
    SvOptions def;
    int errs;
    if (!o) {
        sv_options_init(&def);
        o = &def;
    }
    errs = sv_emit_design(d, o, out);
    if (o->constraints)
        errs += cons_emit(d, o, out);
    if (o->regmaps)
        errs += rmap_emit(d, out);
    return errs;
}

/* =============================================================== names == */

static const char *const sv_keywords[] = {
    "accept_on", "alias", "always", "always_comb", "always_ff", "always_latch", "and", "assert",
    "assign", "assume", "automatic", "before", "begin", "bind", "bins", "binsof", "bit", "break",
    "buf", "bufif0", "bufif1", "byte", "case", "casex", "casez", "cell", "chandle", "checker",
    "class", "clocking", "cmos", "config", "const", "constraint", "context", "continue", "cover",
    "covergroup", "coverpoint", "cross", "deassign", "default", "defparam", "design", "disable",
    "dist", "do", "edge", "else", "end", "endcase", "endchecker", "endclass", "endclocking",
    "endconfig", "endfunction", "endgenerate", "endgroup", "endinterface", "endmodule",
    "endpackage", "endprimitive", "endprogram", "endproperty", "endsequence", "endspecify",
    "endtable", "endtask", "enum", "event", "eventually", "expect", "export", "extends", "extern",
    "final", "first_match", "for", "force", "foreach", "forever", "fork", "forkjoin", "function",
    "generate", "genvar", "global", "highz0", "highz1", "if", "iff", "ifnone", "ignore_bins",
    "illegal_bins", "implements", "implies", "import", "incdir", "include", "initial", "inout",
    "input", "inside", "instance", "int", "integer", "interconnect", "interface", "intersect",
    "join", "join_any", "join_none", "large", "let", "liblist", "library", "local", "localparam",
    "logic", "longint", "macromodule", "matches", "medium", "modport", "module", "nand",
    "negedge", "nettype", "new", "nexttime", "nmos", "nor", "noshowcancelled", "not", "notif0",
    "notif1", "null", "or", "output", "package", "packed", "parameter", "pmos", "posedge",
    "primitive", "priority", "program", "property", "protected", "pull0", "pull1", "pulldown",
    "pullup", "pulsestyle_ondetect", "pulsestyle_onevent", "pure", "rand", "randc", "randcase",
    "randsequence", "rcmos", "real", "realtime", "ref", "reg", "reject_on", "release", "repeat",
    "restrict", "return", "rnmos", "rpmos", "rtran", "rtranif0", "rtranif1", "s_always",
    "s_eventually", "s_nexttime", "s_until", "s_until_with", "scalared", "sequence", "shortint",
    "shortreal", "showcancelled", "signed", "small", "soft", "solve", "specify", "specparam",
    "static", "string", "strong", "strong0", "strong1", "struct", "super", "supply0", "supply1",
    "sync_accept_on", "sync_reject_on", "table", "tagged", "task", "this", "throughout", "time",
    "timeprecision", "timeunit", "tran", "tranif0", "tranif1", "tri", "tri0", "tri1", "triand",
    "trior", "trireg", "type", "typedef", "union", "unique", "unique0", "unsigned", "until",
    "until_with", "untyped", "use", "uwire", "var", "vectored", "virtual", "void", "wait",
    "wait_order", "wand", "weak", "weak0", "weak1", "while", "wildcard", "wire", "with", "within",
    "wor", "xnor", "xor"};

int sv_is_keyword(const char *s)
{
    size_t lo = 0, hi = TERN_ARRAY_LEN(sv_keywords);
    if (!s)
        return 0;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(sv_keywords[mid], s);
        if (c == 0)
            return 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* A legal identifier for `s`: other characters become '_', a leading digit
 * gets a '_' prefix and a keyword a '_' suffix.  Interned. */
static const char *svn_sanitize(const char *s)
{
    StrBuf b = {0};
    const char *r;
    size_t i;
    if (!s || !*s)
        s = "_";
    if (tern_is_digit((unsigned char)s[0]))
        sb_putc(&b, '_');
    for (i = 0; s[i]; i++)
        sb_putc(&b, tern_is_ident((unsigned char)s[i]) ? s[i] : '_');
    if (sv_is_keyword(sb_cstr(&b)))
        sb_putc(&b, '_');
    r = intern(b.data, b.len);
    sb_free(&b);
    return r;
}

/* Claims `base` (interned) in `used`, appending '_' until it is free. */
static const char *svn_claim(Map *used, const char *base)
{
    const char *n = base;
    if (map_has(used, MAP_PTR(n))) {
        StrBuf b = {0};
        sb_puts(&b, base);
        do {
            sb_putc(&b, '_');
            n = intern(b.data, b.len);
        } while (map_has(used, MAP_PTR(n)));
        sb_free(&b);
    }
    map_put(used, MAP_PTR(n), 1);
    return n;
}

typedef struct SvModNames {
    const IrModule *m;
    const char *name;
    const char **sig;
    const char **inst;
    const char **param;
    const char **prop;
    Map used;
    uint32_t tmpn;
} SvModNames;

struct SvNames {
    const IrDesign *d;
    Arena arena;
    Map modidx;      /* IrModule* -> SvModNames* */
    VEC(SvModNames *) mods;
    Map modused;     /* module and package names */
    Map global;      /* package-level names: types and enum constants */
    Map tyname;      /* IrType* -> name of a registered named type */
    Map tagty;       /* payload enum IrType* -> synthetic tag enum IrType* */
    Map varname;     /* IrVariant* -> enum constant name */
    Map fieldname;   /* IrField* -> field name */
    Map grpname;     /* SvPGroup* -> module name (parameterized modules, one per group) */
    const char *pkg;
};

static int svn_has_payload(const IrType *t)
{
    uint32_t i;
    for (i = 0; i < t->nvariants; i++)
        if (t->variants[i].payload)
            return 1;
    return 0;
}

static void svn_register_type(SvNames *n, IrType *t)
{
    uint32_t i;
    StrBuf b = {0};
    if (!t || !t->name || map_has(&n->tyname, MAP_PTR(t)))
        return;
    if (t->kind == IRT_STRUCT) {
        Map local = {0};
        map_put(&n->tyname, MAP_PTR(t), MAP_PTR(svn_claim(&n->global, svn_sanitize(t->name))));
        for (i = 0; i < t->nfields; i++)
            map_put(&n->fieldname, MAP_PTR(&t->fields[i]),
                    MAP_PTR(svn_claim(&local, svn_sanitize(t->fields[i].name))));
        map_free(&local);
    } else if (t->kind == IRT_ENUM) {
        const char *tn = svn_claim(&n->global, svn_sanitize(t->name));
        IrType *tag = t;
        map_put(&n->tyname, MAP_PTR(t), MAP_PTR(tn));
        if (svn_has_payload(t)) {
            tag = ANEW(&n->arena, IrType);
            *tag = *t;
            tag->width = t->tag_width;
            tag->variants = ANEWN(&n->arena, IrVariant, t->nvariants ? t->nvariants : 1);
            for (i = 0; i < t->nvariants; i++) {
                tag->variants[i] = t->variants[i];
                tag->variants[i].payload = NULL;
            }
            sb_printf(&b, "%s_Tag", t->name);
            tag->name = intern(b.data, b.len);
            map_put(&n->tyname, MAP_PTR(tag), MAP_PTR(svn_claim(&n->global, svn_sanitize(tag->name))));
            map_put(&n->tagty, MAP_PTR(t), MAP_PTR(tag));
        }
        for (i = 0; i < t->nvariants; i++) {
            const char *vn;
            sb_reset(&b);
            sb_printf(&b, "%s_%s", t->name, t->variants[i].name ? t->variants[i].name : "v");
            vn = svn_claim(&n->global, svn_sanitize(sb_cstr(&b)));
            map_put(&n->varname, MAP_PTR(&t->variants[i]), MAP_PTR(vn));
            if (tag != t)
                map_put(&n->varname, MAP_PTR(&tag->variants[i]), MAP_PTR(vn));
        }
    }
    sb_free(&b);
}

SvNames *sv_names_new(const IrDesign *d)
{
    SvNames *n = (SvNames *)xcalloc(1, sizeof(SvNames));
    uint32_t i;
    StrBuf b = {0};
    n->d = d;
    for (i = 0; i < d->ntypes; i++)
        svn_register_type(n, d->types[i]);
    /* modules: externs keep their declared names (an extern module with
     * generics is one SV module, parameterized with #(...)); everything
     * else is made legal and unique */
    for (i = 0; i < d->nmodules; i++)
        if (d->modules[i]->is_extern)
            map_put(&n->modused,
                    MAP_PTR(intern_cstr(d->modules[i]->tern_name ? d->modules[i]->tern_name : d->modules[i]->name)), 1);
    sb_printf(&b, "%s_types_pkg", d->top ? d->top->name : "tern");
    n->pkg = svn_claim(&n->modused, svn_sanitize(sb_cstr(&b)));
    sb_free(&b);
    for (i = 0; i < d->nmodules; i++)
        sv_names_module(n, d->modules[i]);
    return n;
}

void sv_names_free(SvNames *n)
{
    size_t i;
    if (!n)
        return;
    for (i = 0; i < n->mods.len; i++) {
        SvModNames *mn = n->mods.data[i];
        free(mn->sig);
        free(mn->inst);
        free(mn->param);
        free(mn->prop);
        map_free(&mn->used);
        free(mn);
    }
    vec_free(n->mods);
    map_free(&n->modidx);
    map_free(&n->modused);
    map_free(&n->global);
    map_free(&n->tyname);
    map_free(&n->tagty);
    map_free(&n->varname);
    map_free(&n->fieldname);
    map_free(&n->grpname);
    arena_release(&n->arena);
    free(n);
}

static SvModNames *svn_mod(SvNames *n, const IrModule *m)
{
    uint64_t v;
    SvModNames *mn;
    uint32_t i;
    size_t it = 0;
    uint64_t k;
    if (map_get(&n->modidx, MAP_PTR(m), &v))
        return (SvModNames *)(uintptr_t)v;
    mn = (SvModNames *)xcalloc(1, sizeof(SvModNames));
    mn->m = m;
    mn->sig = (const char **)xcalloc(m->nsigs + 1, sizeof(char *));
    mn->inst = (const char **)xcalloc(m->ninsts + 1, sizeof(char *));
    mn->param = (const char **)xcalloc(m->nparams + 1, sizeof(char *));
    mn->prop = (const char **)xcalloc(m->nprops + 1, sizeof(char *));
    map_put(&n->modidx, MAP_PTR(m), MAP_PTR(mn));
    vec_push(n->mods, mn);
    if (m->is_extern) {
        mn->name = intern_cstr(m->tern_name ? m->tern_name : m->name);
        for (i = 0; i < m->nsigs; i++)
            mn->sig[i] = intern_cstr(m->sigs[i].name);
        for (i = 0; i < m->nparams; i++)
            mn->param[i] = intern_cstr(m->params[i].name);
        return mn;
    }
    {
        /* the members of a parameterized group share one module name */
        uint64_t pv;
        const SvPGroup *grp = NULL;
        if (n->d->sv_params && map_get(&n->d->sv_params->views, MAP_PTR(m), &pv))
            grp = ((const SvPView *)(uintptr_t)pv)->g;
        if (grp && map_get(&n->grpname, MAP_PTR(grp), &pv)) {
            mn->name = (const char *)(uintptr_t)pv;
        } else {
            mn->name = svn_claim(&n->modused, svn_sanitize(grp ? grp->name : m->name));
            if (grp)
                map_put(&n->grpname, MAP_PTR(grp), MAP_PTR(mn->name));
        }
    }
    while (map_next(&n->global, &it, &k, &v))
        map_put(&mn->used, k, 1);
    for (i = 0; i < m->nports; i++)
        if (m->ports[i] < m->nsigs && !mn->sig[m->ports[i]])
            mn->sig[m->ports[i]] = svn_claim(&mn->used, svn_sanitize(m->sigs[m->ports[i]].name));
    for (i = 0; i < m->nsigs; i++)
        if (!mn->sig[i])
            mn->sig[i] = svn_claim(&mn->used, svn_sanitize(m->sigs[i].name));
    for (i = 0; i < m->nparams; i++)
        mn->param[i] = svn_claim(&mn->used, svn_sanitize(m->params[i].name));
    for (i = 0; i < m->ninsts; i++)
        mn->inst[i] = svn_claim(&mn->used, svn_sanitize(m->insts[i].name));
    for (i = 0; i < m->nprops; i++)
        mn->prop[i] = svn_claim(&mn->used, svn_sanitize(m->props[i].name ? m->props[i].name : "prop"));
    return mn;
}

const char *sv_names_module(SvNames *n, const IrModule *m) { return svn_mod(n, m)->name; }

const char *sv_names_signal(SvNames *n, const IrModule *m, uint32_t sig)
{
    return sig < m->nsigs ? svn_mod(n, m)->sig[sig] : "?";
}

const char *sv_names_instance(SvNames *n, const IrModule *m, uint32_t inst)
{
    return inst < m->ninsts ? svn_mod(n, m)->inst[inst] : "?";
}

const char *sv_names_types_pkg(SvNames *n) { return n->pkg; }

/* A fresh module-scope name based on `base` (claimed). */
static const char *svn_fresh(SvModNames *mn, const char *base)
{
    return svn_claim(&mn->used, svn_sanitize(base));
}

static const char *svn_tmp(SvModNames *mn)
{
    char buf[32];
    const char *n;
    for (;;) {
        snprintf(buf, sizeof buf, "tmp_%u", mn->tmpn++);
        n = intern_cstr(buf);
        if (!map_has(&mn->used, MAP_PTR(n)))
            break;
    }
    map_put(&mn->used, MAP_PTR(n), 1);
    return n;
}

static const char *svn_tyname(SvNames *n, const IrType *t)
{
    uint64_t v;
    if (t && map_get(&n->tyname, MAP_PTR(t), &v))
        return (const char *)(uintptr_t)v;
    return NULL;
}

static const char *svn_varname(SvNames *n, const IrVariant *var)
{
    uint64_t v;
    if (map_get(&n->varname, MAP_PTR(var), &v))
        return (const char *)(uintptr_t)v;
    return NULL;
}

static const char *svn_fieldname(SvNames *n, const IrField *f)
{
    uint64_t v;
    if (map_get(&n->fieldname, MAP_PTR(f), &v))
        return (const char *)(uintptr_t)v;
    return svn_sanitize(f->name);
}

static IrType *svn_tagty(SvNames *n, const IrType *t)
{
    uint64_t v;
    if (map_get(&n->tagty, MAP_PTR(t), &v))
        return (IrType *)(uintptr_t)v;
    return NULL;
}

/* a registered enum without payloads (or the tag type of one with) */
static int svn_plain_enum(SvNames *n, const IrType *t)
{
    return t && t->kind == IRT_ENUM && svn_tyname(n, t) && !svn_has_payload(t);
}

static int svn_payload_enum(SvNames *n, const IrType *t)
{
    return t && t->kind == IRT_ENUM && svn_tyname(n, t) && svn_has_payload(t);
}

/* ============================================================= context == */

enum { SVD_NONE, SVD_IN, SVD_ASSIGN, SVD_COMB, SVD_CLOCKED, SVD_INST };

/* precedence levels of emitted expressions (higher binds tighter) */
enum {
    SVP_TERN = 1, SVP_LOR, SVP_LAND, SVP_BOR, SVP_BXOR, SVP_BAND, SVP_EQ, SVP_REL, SVP_SHIFT,
    SVP_ADD, SVP_MUL, SVP_UNARY, SVP_PRIM
};

typedef struct SvCtx {
    const IrDesign *d;
    const SvOptions *o;
    SvNames *n;
    SvOutput *out;
    SvOutput memout;      /* memory init files, appended after the modules */
    Arena scratch;
    int v2005;
    int errors;
    const char *pkg;
    /* parameterized modules: the plan (views of the members of accepted
     * groups); while a plan is being made, `validating` emits into `text`
     * without diagnostics and names group modules by SvPGroup.name */
    const struct SvParamPlan *plan;
    int validating;
    StrBuf *text;
} SvCtx;

typedef struct SvPConst {     /* Verilog-2005: localparam holding a parameter-dependent constant */
    const char *name;
    const SvXExpr *wx, *vx;   /* width / value expression (NULL: the number) */
    uint32_t w;
    uint64_t v;
} SvPConst;

typedef struct SvTemp {
    const char *name;
    uint32_t width;
    const IrType *ty;     /* SV declaration type (NULL: plain vector) */
    int local;            /* assigned inside a comb process (Verilog-2005 only) */
    const IrExpr *expr;
} SvTemp;

typedef struct SvGen {
    SvCtx *c;
    const IrModule *m;
    SvModNames *mn;
    Map types;            /* named IrTypes visible in this module */
    VEC(SvTemp) temps;
    StrBuf pre;           /* module-level temporaries pending before the current construct */
    StrBuf *spre;         /* statement-level assignments of process-local temporaries */
    int spre_ind;
    const Map *written;   /* signals written by the current comb process, or NULL */
    VEC(uint32_t) locals; /* temps local to the current comb process */
    Map defassigned;      /* comb: signals definitely assigned so far (V2005 case defaults) */
    Map poweron;          /* registers whose power-on value is their declaration's initializer */
    const char *rst_guard;/* reset-active condition for assertions outside a reset branch */
    int in_prop;
    int dedupe_marks;     /* inside a process: a marker repeating the previous line is omitted */
    uint32_t mark_file, mark_line;
    unsigned char *drv;
    unsigned char *memw;
    int uses_pkg;
    /* parameterized emission (NULL / empty otherwise) */
    const SvPView *pv;
    const SvXExpr *kwidth;    /* width of the constant printed next (reset values) */
    const SvXExpr *sel_w;     /* width / low bit of the slice printed next (sv_slice_of) */
    const SvXExpr *sel_lo;
    VEC(SvPConst) pconsts;
    Arena *xa;                /* scratch SvXExpr nodes */
} SvGen;

typedef struct SvEx {
    int prec;
    int sgn;              /* the emitted text is signed in SV */
    const IrType *ty;     /* SV type of the emitted text when it is a typed (struct/enum/array) value */
} SvEx;

static SvEx sv_ex(SvGen *g, const IrExpr *e, int self, StrBuf *out);

static void sv_error(SvGen *g, Span loc, const char *msg)
{
    if (!g->c->validating)
        diag_error(loc, "SystemVerilog emitter: %s (module %s)", msg, g->m->name);
    g->c->errors++;
}

/* --------------------------------------------------------------- misc -- */

static void sv_ind(StrBuf *out, int ind) { sb_indent(out, ind * 4); }

static void sv_mark(SvGen *g, Span loc, StrBuf *out)
{
    const SourceFile *f;
    if (!g->c->o->line_markers || !loc.file || !loc.line)
        return;
    f = src_get(loc.file);
    if (!f || !f->path)
        return;
    if (g->dedupe_marks && loc.file == g->mark_file && loc.line == g->mark_line)
        return;
    g->mark_file = loc.file;
    g->mark_line = loc.line;
    sb_printf(out, "  // tern: %s:%u", f->path, loc.line);
}

/* start of a process: markers inside it repeat no line of the previous one */
static void sv_marks_begin(SvGen *g, Span loc)
{
    g->dedupe_marks = 1;
    g->mark_file = loc.file;
    g->mark_line = loc.line;
}

static uint64_t sv_pow2_or_max(uint32_t w)
{
    return w >= 64 ? ~UINT64_C(0) : UINT64_C(1) << w;
}

/* "Verilog literal" of a constant: 1'b1, 3'd5, 8'h1f, 130'h3_..._f */
static void sv_lit(const IrBits *b, StrBuf *out)
{
    if (b->width == 1) {
        sb_puts(out, ir_bits_get(b, 0) ? "1'b1" : "1'b0");
        return;
    }
    if (b->width <= 64 && ir_bits_low64(b) <= 9) {
        sb_printf(out, "%u'd%u", b->width, (unsigned)ir_bits_low64(b));
        return;
    }
    ir_bits_print(b, out);
}

static void sv_lit_u64(uint32_t width, uint64_t v, StrBuf *out)
{
    uint64_t w[1];
    IrBits b;
    w[0] = width < 64 ? v & ((UINT64_C(1) << width) - 1u) : v;
    if (width > 64) {
        /* only small values are printed this way */
        sb_printf(out, "%u'h%llx", width, (unsigned long long)v);
        return;
    }
    b.width = width;
    b.w = w;
    sv_lit(&b, out);
}

static void sv_zero(uint32_t width, StrBuf *out)
{
    if (width == 1)
        sb_puts(out, "1'b0");
    else
        sb_printf(out, "%u'd0", width);
}

static void sv_str_lit(const char *s, StrBuf *out, int fmt)
{
    sb_putc(out, '"');
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            sb_printf(out, "\\%c", ch);
        else if (ch == '%' && fmt)
            sb_puts(out, "%%");
        else if (ch == '\n')
            sb_puts(out, "\\n");
        else if (ch < 32)
            sb_putc(out, ' ');
        else
            sb_putc(out, ch);
    }
    sb_putc(out, '"');
}

/* ----------------------------------------------- parameterized modules -- */

static const SvXExpr *svp_get(const SvGen *g, const void *p, int tag)
{
    uint64_t v;
    if (!g->pv || !p || !map_get(&g->pv->slots, SVP_KEY(p, tag), &v))
        return NULL;
    return (const SvXExpr *)(uintptr_t)v;
}

static const char *const *svp_pn(const SvGen *g) { return g->pv->g->pnames; }

static const SvXExpr *svp_k(SvGen *g, int64_t k)
{
    SvXExpr *x = ANEW(g->xa, SvXExpr);
    x->op = SVX_K;
    x->k = k;
    return x;
}

static const SvXExpr *svp_bin(SvGen *g, SvXOp op, const SvXExpr *a, const SvXExpr *b)
{
    SvXExpr *x = ANEW(g->xa, SvXExpr);
    x->op = op;
    x->a = (SvXExpr *)a;
    x->b = (SvXExpr *)b;
    return x;
}

/* "[W-1:0]" */
static void svp_range(SvGen *g, const SvXExpr *w, StrBuf *out)
{
    sb_putc(out, '[');
    svp_print_m1(w, svp_pn(g), out);
    sb_puts(out, ":0]");
}

/* A constant of width w (wx: expression or NULL) and value b (vx:
 * expression or NULL): SV-2012 `W'(V)`, Verilog-2005 a localparam. */
static void svp_const(SvGen *g, const SvXExpr *wx, const SvXExpr *vx, uint32_t width, const IrBits *b,
                      StrBuf *out)
{
    uint64_t v = b && b->width <= 64 ? ir_bits_low64(b) : 0;
    if (!g->c->v2005) {
        svp_print_castw(wx ? wx : svp_k(g, width), svp_pn(g), out);
        sb_puts(out, "'(");
        if (vx)
            svp_print(vx, svp_pn(g), SVX_PREC_COND, out);
        else if (!b || (b->width <= 64 && v < 0x80000000u))
            sb_printf(out, "%llu", (unsigned long long)v);
        else
            sv_lit(b, out);
        sb_putc(out, ')');
        return;
    }
    {
        size_t i;
        SvPConst pc;
        for (i = 0; i < g->pconsts.len; i++) {
            const SvPConst *q = &g->pconsts.data[i];
            if (q->wx == wx && q->vx == vx && (wx || q->w == width) && (vx || q->v == v)) {
                sb_puts(out, q->name);
                return;
            }
        }
        memset(&pc, 0, sizeof pc);
        {
            char buf[32];
            uint32_t n = (uint32_t)g->pconsts.len;
            for (;;) {
                snprintf(buf, sizeof buf, "pk_%u", n++);
                if (!map_has(&g->mn->used, MAP_PTR(intern_cstr(buf))))
                    break;
            }
            pc.name = svn_fresh(g->mn, buf);
        }
        pc.wx = wx;
        pc.vx = vx;
        pc.w = width;
        pc.v = v;
        vec_push(g->pconsts, pc);
        sb_puts(out, pc.name);
    }
}

/* the constant node e: a literal, or a parameter-dependent constant */
static void sv_klit(SvGen *g, const IrExpr *e, StrBuf *out)
{
    const SvXExpr *wx = svp_get(g, e, SVK_EXPRW), *vx = svp_get(g, e->bits.w, SVK_CONST);
    if (!wx)
        wx = g->kwidth;
    if (!wx && !vx) {
        sv_lit(&e->bits, out);
        return;
    }
    svp_const(g, wx, vx, e->width, &e->bits, out);
}

/* SV-2012: a zero constant of parameter-dependent width in a
 * context-determined position (an operand sized by its context, or the
 * value of an assignment) is the fill literal '0 */
static int sv_kfill0(SvGen *g, const IrExpr *e, StrBuf *out)
{
    if (g->c->v2005 || e->kind != IRE_CONST || !ir_bits_is_zero(&e->bits) ||
        svp_get(g, e->bits.w, SVK_CONST) || !(svp_get(g, e, SVK_EXPRW) || g->kwidth))
        return 0;
    sb_puts(out, "'0");
    return 1;
}

/* zero of e's width */
static void sv_zero_e(SvGen *g, const IrExpr *e, StrBuf *out)
{
    const SvXExpr *wx = svp_get(g, e, SVK_EXPRW);
    if (!wx) {
        sv_zero(e->width, out);
        return;
    }
    svp_const(g, wx, NULL, e->width, NULL, out);
}

static int sv_expr_eq(const IrExpr *a, const IrExpr *b)
{
    uint32_t i;
    if (a == b)
        return 1;
    if (!a || !b || a->kind != b->kind || a->width != b->width || a->nargs != b->nargs)
        return 0;
    switch (a->kind) {
    case IRE_CONST:
        if (!ir_bits_eq(&a->bits, &b->bits))
            return 0;
        break;
    case IRE_SIG:
    case IRE_MEMREAD:
        if (a->sig != b->sig)
            return 0;
        break;
    case IRE_SLICE:
    case IRE_PAST:
        if (a->lo != b->lo)
            return 0;
        break;
    case IRE_UNARY:
    case IRE_BINARY:
        if (a->op != b->op || a->is_signed != b->is_signed)
            return 0;
        break;
    case IRE_RESIZE:
        if (a->is_signed != b->is_signed)
            return 0;
        break;
    default: break;
    }
    for (i = 0; i < a->nargs; i++)
        if (!sv_expr_eq(a->args[i], b->args[i]))
            return 0;
    return 1;
}

static int sv_reads(const IrExpr *e, const Map *set)
{
    uint32_t i;
    if (!e || !set)
        return 0;
    if ((e->kind == IRE_SIG || e->kind == IRE_MEMREAD) && map_has(set, e->sig))
        return 1;
    for (i = 0; i < e->nargs; i++)
        if (sv_reads(e->args[i], set))
            return 1;
    return 0;
}

/* --------------------------------------------------------------- types -- */

/* Is `t` declared with structure (field / element selects) in this mode? */
static int sv_typed(SvGen *g, const IrType *t)
{
    if (!t || g->c->v2005)
        return 0;
    if (t->kind == IRT_ARRAY)
        return 1;
    if (t->kind == IRT_STRUCT || t->kind == IRT_ENUM)
        return svn_tyname(g->c->n, t) != NULL && map_has(&g->types, MAP_PTR(t));
    return 0;
}

static const IrType *sv_sigty(SvGen *g, uint32_t sig)
{
    const IrType *t = g->m->sigs[sig].type;
    return sv_typed(g, t) ? t : NULL;
}

static int sv_sig_signed(SvGen *g, uint32_t sig)
{
    return g->m->sigs[sig].type->kind == IRT_SINT;
}

/* Declaration type: "logic [7:0]", "logic signed [7:0]", "Pixel", "logic [3:0][7:0]",
 * "Pixel [3:0]"; Verilog-2005: "[7:0]", "signed [7:0]", "" (1 bit). */
static void sv_decl_type(SvGen *g, const IrType *t, StrBuf *out)
{
    uint32_t dims[64], nd = 0, i;
    const IrType *b = t;
    if (g->c->v2005) {
        if (t->kind == IRT_SINT)
            sb_puts(out, "signed ");
        if (t->width > 1)
            sb_printf(out, "[%u:0]", t->width - 1);
        else if (t->kind == IRT_SINT)
            sb_puts(out, "[0:0]");
        return;
    }
    while (b->kind == IRT_ARRAY && nd < 64) {
        dims[nd++] = b->len;
        b = b->elem;
    }
    if (b->kind == IRT_ARRAY) {   /* absurdly deep: flatten */
        sb_printf(out, "logic [%u:0]", t->width - 1);
        return;
    }
    if (sv_typed(g, b)) {
        sb_puts(out, svn_tyname(g->c->n, b));
        if (nd)
            sb_putc(out, ' ');
    } else {
        sb_puts(out, "logic");
        if (b->kind == IRT_SINT && !nd)
            sb_puts(out, " signed");
        if (nd || b->width > 1)
            sb_putc(out, ' ');
    }
    for (i = 0; i < nd; i++)
        sb_printf(out, "[%u:0]", dims[i] - 1);
    if (!sv_typed(g, b) && b->width > 1)
        sb_printf(out, "[%u:0]", b->width - 1);
}

/* Declaration type of a signal: a scalar whose width is a parameter
 * expression gets `logic [W-1:0]` (`[W-1:0]` in Verilog-2005). */
static void sv_sig_type(SvGen *g, uint32_t sig, StrBuf *out)
{
    const IrType *t = g->m->sigs[sig].type;
    const SvXExpr *wx = svp_get(g, &g->m->sigs[sig], SVK_SIGW);
    if (!wx || t->kind == IRT_ARRAY || t->kind == IRT_STRUCT || t->kind == IRT_ENUM) {
        sv_decl_type(g, t, out);
        return;
    }
    if (!g->c->v2005)
        sb_puts(out, t->kind == IRT_SINT ? "logic signed " : "logic ");
    else if (t->kind == IRT_SINT)
        sb_puts(out, "signed ");
    svp_range(g, wx, out);
}

/* 1 if the declaration type of sig is non-empty in Verilog-2005 */
static int sv_sig_has_range(SvGen *g, uint32_t sig)
{
    const IrType *t = g->m->sigs[sig].type;
    return t->width > 1 || t->kind == IRT_SINT || svp_get(g, &g->m->sigs[sig], SVK_SIGW) != NULL;
}

/* collect the named types reachable from t into g->types */
static void sv_use_type(SvGen *g, const IrType *t)
{
    uint32_t i;
    if (!t)
        return;
    if (t->name && svn_tyname(g->c->n, t)) {
        if (map_has(&g->types, MAP_PTR(t)))
            return;
        map_put(&g->types, MAP_PTR(t), 1);
        if (t->kind == IRT_ENUM && svn_tagty(g->c->n, t))
            map_put(&g->types, MAP_PTR(svn_tagty(g->c->n, t)), 1);
    }
    switch (t->kind) {
    case IRT_STRUCT:
        for (i = 0; i < t->nfields; i++)
            sv_use_type(g, t->fields[i].type);
        break;
    case IRT_ENUM:
        for (i = 0; i < t->nvariants; i++)
            sv_use_type(g, t->variants[i].payload);
        break;
    case IRT_ARRAY: sv_use_type(g, t->elem); break;
    default: break;
    }
}

static void sv_use_expr(SvGen *g, const IrExpr *e)
{
    uint32_t i;
    if (!e)
        return;
    if (e->type && e->type->kind == IRT_ENUM)
        sv_use_type(g, e->type);
    for (i = 0; i < e->nargs; i++)
        sv_use_expr(g, e->args[i]);
}

static void sv_use_block(SvGen *g, const IrBlock *b)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        sv_use_expr(g, s->rhs);
        sv_use_expr(g, s->cond);
        sv_use_expr(g, s->subject);
        sv_use_expr(g, s->addr);
        sv_use_expr(g, s->data);
        sv_use_expr(g, s->lhs.index);
        sv_use_block(g, &s->then_b);
        sv_use_block(g, &s->else_b);
        for (j = 0; j < s->narms; j++)
            sv_use_block(g, &s->arms[j].body);
    }
}

/* The exact sub-type of t at bits [lo +: w] (struct field, array element,
 * enum tag), or NULL. */
static const IrType *sv_type_at(SvGen *g, const IrType *t, uint32_t lo, uint32_t w)
{
    uint32_t i;
    while (t) {
        if (lo == 0 && w == t->width)
            return t;
        if (t->kind == IRT_STRUCT) {
            const IrType *nt = NULL;
            for (i = 0; i < t->nfields; i++) {
                const IrField *f = &t->fields[i];
                if (lo >= f->lsb && lo + w <= f->lsb + f->type->width) {
                    nt = f->type;
                    lo -= f->lsb;
                    break;
                }
            }
            t = nt;
        } else if (t->kind == IRT_ARRAY) {
            uint32_t ew = t->elem->width, k = lo / ew;
            if ((lo + w - 1) / ew != k)
                return NULL;
            lo -= k * ew;
            t = t->elem;
        } else if (t->kind == IRT_ENUM && svn_tagty(g->c->n, t)) {
            if (lo == t->width - t->tag_width && w == t->tag_width)
                return svn_tagty(g->c->n, t);
            return NULL;
        } else {
            return NULL;
        }
    }
    return NULL;
}

/* The enum this expression's value has (for naming constants), or NULL. */
static const IrType *sv_enum_of(SvGen *g, const IrExpr *e)
{
    const IrType *t = NULL;
    SvNames *n = g->c->n;
    switch (e->kind) {
    case IRE_SIG: t = g->m->sigs[e->sig].type; break;
    case IRE_MEMREAD: t = g->m->sigs[e->sig].type; break;
    case IRE_SLICE: {
        const IrExpr *x = e;
        uint32_t lo = 0;
        while (x->kind == IRE_SLICE) {
            lo += x->lo;
            x = x->args[0];
        }
        if (x->kind == IRE_SIG)
            t = sv_type_at(g, g->m->sigs[x->sig].type, lo, e->width);
        if (!t)
            t = e->type;
        break;
    }
    case IRE_MUX:
        t = sv_enum_of(g, e->args[1]);
        if (!t)
            t = sv_enum_of(g, e->args[2]);
        return t;
    default: t = e->type; break;
    }
    if (t && t->kind == IRT_ENUM && svn_tagty(n, t) == NULL && svn_plain_enum(n, t) &&
        map_has(&g->types, MAP_PTR(t)) && t->width == e->width)
        return t;
    return NULL;
}

/* Writes the constant name of `b` in plain enum `t` if there is one. */
static int sv_enum_name(SvGen *g, const IrType *t, const IrBits *b, StrBuf *out)
{
    uint32_t i;
    if (!t || !svn_plain_enum(g->c->n, t) || !map_has(&g->types, MAP_PTR(t)) || b->width != t->width)
        return 0;
    for (i = 0; i < t->nvariants; i++)
        if (ir_bits_eq(&t->variants[i].tag, b)) {
            const char *nm = svn_varname(g->c->n, &t->variants[i]);
            if (!nm)
                return 0;
            sb_puts(out, nm);
            return 1;
        }
    return 0;
}

/* ---------------------------------------------------------- temporaries -- */

static const char *sv_temp_ty(SvGen *g, const IrExpr *e, const IrType *ty, const char *base);

static const char *sv_temp(SvGen *g, const IrExpr *e)
{
    return sv_temp_ty(g, e, NULL, NULL);
}

static void sv_rhs(SvGen *g, const IrExpr *e, const IrType *ty, StrBuf *out);

/* A temporary holding e (declared with SV type `ty` or as a plain vector).
 * Module-level temporaries are continuous assignments placed before the
 * construct that uses them and reused for equal expressions; in a comb
 * process an expression that reads a signal the process writes needs a
 * process-local temporary (assigned right before the statement, with a
 * default at the top of the process so that no latch is inferred). */
static const char *sv_temp_ty(SvGen *g, const IrExpr *e, const IrType *ty, const char *base)
{
    SvTemp t;
    StrBuf val = {0};
    size_t i;
    int local = g->written && sv_reads(e, g->written);
    if (!local && !base)
        for (i = 0; i < g->temps.len; i++)
            if (!g->temps.data[i].local && g->temps.data[i].ty == ty &&
                sv_expr_eq(g->temps.data[i].expr, e))
                return g->temps.data[i].name;
    memset(&t, 0, sizeof t);
    t.name = base ? svn_fresh(g->mn, base) : svn_tmp(g->mn);
    t.width = e->width;
    t.ty = ty;
    t.local = local;
    t.expr = e;
    sv_rhs(g, e, ty, &val);
    if (local) {
        if (g->spre) {
            sv_ind(g->spre, g->spre_ind);
            sb_printf(g->spre, "%s = %s;\n", t.name, sb_cstr(&val));
        }
        vec_push(g->locals, (uint32_t)g->temps.len);
    } else {
        sb_printf(&g->pre, "    assign %s = %s;\n", t.name, sb_cstr(&val));
    }
    vec_push(g->temps, t);
    sb_free(&val);
    return t.name;
}

/* ------------------------------------------------------------- selects -- */

typedef struct SvSel {
    int sgn;
    const IrType *ty;     /* typed result (whole field / element), else NULL */
    int path;             /* identifier, field or element access: can be selected further */
} SvSel;

/* Writes bits [lo +: w] of `base` (a selectable primary of SV type t, NULL =
 * vector, width bw, signedness bsgn). */
static SvSel sv_select(SvGen *g, const char *base, const IrType *t, uint32_t bw, int bsgn, uint32_t lo,
                       uint32_t w, StrBuf *out)
{
    SvSel r;
    StrBuf b = {0};
    uint32_t i;
    /* parameter expressions for lo / w of a plain vector select (set by the caller) */
    const SvXExpr *lox = g->sel_lo, *wx = g->sel_w;
    g->sel_lo = g->sel_w = NULL;
    r.sgn = 0;
    r.ty = NULL;
    r.path = 1;
    if (lo == 0 && w == bw) {
        sb_puts(out, base);
        r.sgn = bsgn;
        r.ty = t;
        return r;
    }
    if (t && sv_typed(g, t)) {
        if (t->kind == IRT_ARRAY) {
            uint32_t ew = t->elem->width, k0 = lo / ew, k1 = (lo + w - 1) / ew, k;
            const IrType *et = sv_typed(g, t->elem) ? t->elem : NULL;
            if (k0 == k1) {
                sb_printf(&b, "%s[%u]", base, k0);
                r = sv_select(g, b.data, et, ew, 0, lo - k0 * ew, w, out);
                sb_free(&b);
                return r;
            }
            r.path = 0;
            if (lo % ew == 0 && w % ew == 0) {
                sb_printf(out, "%s[%u:%u]", base, k1, k0);
                return r;
            }
            sb_putc(out, '{');
            for (k = k1;; k--) {
                uint32_t plo = TERN_MAX(lo, k * ew), phi = TERN_MIN(lo + w, (k + 1) * ew);
                sb_reset(&b);
                sb_printf(&b, "%s[%u]", base, k);
                sv_select(g, b.data, et, ew, 0, plo - k * ew, phi - plo, out);
                if (k == k0)
                    break;
                sb_puts(out, ", ");
            }
            sb_putc(out, '}');
            sb_free(&b);
            return r;
        }
        if (t->kind == IRT_STRUCT) {
            for (i = 0; i < t->nfields; i++) {
                const IrField *f = &t->fields[i];
                if (lo >= f->lsb && lo + w <= f->lsb + f->type->width) {
                    sb_printf(&b, "%s.%s", base, svn_fieldname(g->c->n, f));
                    r = sv_select(g, b.data, sv_typed(g, f->type) ? f->type : NULL, f->type->width,
                                  f->type->kind == IRT_SINT, lo - f->lsb, w, out);
                    sb_free(&b);
                    return r;
                }
            }
        } else if (t->kind == IRT_ENUM && svn_tagty(g->c->n, t)) {
            uint32_t tl = bw - t->tag_width, pw = tl;
            if (lo >= tl) {
                const IrType *tt = svn_tagty(g->c->n, t);
                sb_printf(&b, "%s.tag", base);
                r = sv_select(g, b.data, tt, t->tag_width, 0, lo - tl, w, out);
                sb_free(&b);
                return r;
            }
            if (lo + w <= pw) {
                sb_printf(&b, "%s.payload", base);
                r = sv_select(g, b.data, NULL, pw, 0, lo, w, out);
                sb_free(&b);
                return r;
            }
        }
    }
    r.path = 0;
    if (lox || wx) {
        sb_printf(out, "%s[", base);
        if (!wx && w == 1) {
            svp_print(lox, svp_pn(g), SVX_PREC_COND, out);
        } else if (!lox && lo == 0) {
            svp_print_m1(wx, svp_pn(g), out);
            sb_puts(out, ":0");
        } else {
            svp_print(lox ? lox : svp_k(g, lo), svp_pn(g), SVX_PREC_COND, out);
            sb_puts(out, " +: ");
            svp_print(wx ? wx : svp_k(g, w), svp_pn(g), SVX_PREC_COND, out);
        }
        sb_putc(out, ']');
        return r;
    }
    if (w == 1)
        sb_printf(out, "%s[%u]", base, lo);
    else
        sb_printf(out, "%s[%u:%u]", base, lo + w - 1, lo);
    return r;
}

/* Resolves a chain of slices down to a signal: the signal and the absolute
 * low bit, or IR_NONE. */
static uint32_t sv_slice_base(const IrExpr *e, uint32_t *lo)
{
    uint32_t l = 0;
    while (e->kind == IRE_SLICE) {
        l += e->lo;
        e = e->args[0];
    }
    *lo = l;
    return e->kind == IRE_SIG ? e->sig : IR_NONE;
}

/* the address guard of a memory read: needed when the address can exceed the depth */
static int sv_mem_guard(SvGen *g, const IrExpr *e)
{
    uint32_t depth = g->m->sigs[e->sig].depth;
    /* parameterized depth or address width: always guarded (the guard is
     * true when the depth is a power of two) */
    if (svp_get(g, &g->m->sigs[e->sig], SVK_DEPTH) || svp_get(g, e->args[0], SVK_EXPRW))
        return 1;
    return sv_pow2_or_max(e->args[0]->width) > depth;
}

static void sv_index_text(SvGen *g, const IrExpr *idx, uint32_t ew, uint32_t lo, StrBuf *out)
{
    StrBuf t = {0};
    SvEx x = sv_ex(g, idx, 1, &t);
    int wrap = x.sgn;
    if (wrap)
        sb_printf(out, "$unsigned(%s)", sb_cstr(&t));
    else if (ew != 1 && x.prec < SVP_MUL)
        sb_printf(out, "(%s)", sb_cstr(&t));
    else
        sb_puts(out, sb_cstr(&t));
    if (ew != 1)
        sb_printf(out, " * %u", ew);
    if (lo)
        sb_printf(out, " + %u", lo);
    sb_free(&t);
}

/* bits [lo +: w] of expression x */
static SvEx sv_slice_of(SvGen *g, const IrExpr *x, uint32_t lo, uint32_t w, int self, StrBuf *out)
{
    SvEx r;
    uint32_t base_lo, s;
    const SvXExpr *wx = g->sel_w, *lox = g->sel_lo;
    g->sel_w = g->sel_lo = NULL;
    r.prec = SVP_PRIM;
    r.sgn = 0;
    r.ty = NULL;
    if (lo == 0 && w == x->width)
        return sv_ex(g, x, self, out);
    if (x->kind == IRE_CONST) {
        IrBits b = ir_bits_zero(&g->c->scratch, w);
        uint32_t i;
        for (i = 0; i < w; i++)
            ir_bits_set(&b, i, ir_bits_get(&x->bits, lo + i));
        sv_lit(&b, out);
        return r;
    }
    if (x->kind == IRE_SLICE) {
        const SvXExpr *xlo = svp_get(g, x, SVK_LO);
        if (xlo || lox)
            g->sel_lo = svp_bin(g, SVX_ADD, xlo ? xlo : svp_k(g, x->lo), lox ? lox : svp_k(g, lo));
        g->sel_w = wx;
        return sv_slice_of(g, x->args[0], x->lo + lo, w, self, out);
    }
    s = sv_slice_base(x, &base_lo);
    if (s != IR_NONE) {
        SvSel sel;
        g->sel_lo = lox;
        g->sel_w = wx;
        sel = sv_select(g, g->mn->sig[s], sv_sigty(g, s), g->m->sigs[s].type->width,
                              sv_sig_signed(g, s), base_lo + lo, w, out);
        r.sgn = sel.sgn;
        r.ty = sel.ty;
        return r;
    }
    if (x->kind == IRE_CONCAT) {
        /* parts from the LSB up: find the parts that hold [lo, lo+w) */
        uint32_t i, plo = 0, n = 0;
        StrBuf parts = {0};
        for (i = x->nargs; i-- > 0;) {
            const IrExpr *p = x->args[i];
            uint32_t phi = plo + p->width;
            if (phi > lo && plo < lo + w) {
                uint32_t a = TERN_MAX(lo, plo), b = TERN_MIN(lo + w, phi);
                StrBuf one = {0};
                sv_slice_of(g, p, a - plo, b - a, 1, &one);
                /* prepend (higher parts come later in this loop) */
                if (n)
                    sb_puts(&one, ", ");
                sb_append(&one, parts.data ? parts.data : "", parts.len);
                sb_free(&parts);
                parts = one;
                n++;
            }
            plo = phi;
        }
        if (n == 1)
            sb_append(out, parts.data, parts.len);
        else
            sb_printf(out, "{%s}", sb_cstr(&parts));
        sb_free(&parts);
        return r;
    }
    if (x->kind == IRE_RESIZE && x->args[0]->width < x->width) {
        const IrExpr *a = x->args[0];
        uint32_t aw = a->width;
        if (lo + w <= aw)
            return sv_slice_of(g, a, lo, w, self, out);
        if (!x->is_signed) {
            if (lo >= aw) {
                sv_zero(w, out);
                return r;
            }
            sb_putc(out, '{');
            sv_zero(lo + w - aw, out);
            sb_puts(out, ", ");
            sv_slice_of(g, a, lo, aw - lo, 1, out);
            sb_putc(out, '}');
            return r;
        }
    }
    if (x->kind == IRE_MEMREAD && !sv_mem_guard(g, x)) {
        StrBuf b = {0};
        sv_ex(g, x, 1, &b);
        g->sel_lo = lox;
        g->sel_w = wx;
        sv_select(g, sb_cstr(&b), NULL, x->width, 0, lo, w, out);
        sb_free(&b);
        return r;
    }
    if (!g->c->v2005 && (lo == 0 || (g->written && sv_reads(x, g->written)))) {
        /* size cast: W'(x) / W'(x >> lo), exact without a temporary */
        StrBuf t = {0}, ws = {0}, ls = {0};
        SvEx xe = sv_ex(g, x, 1, &t);
        if (wx)
            svp_print_castw(wx, svp_pn(g), &ws);
        else
            sb_printf(&ws, "%u", w);
        if (lox)
            svp_print(lox, svp_pn(g), SVX_PREC_ADD, &ls);
        else
            sb_printf(&ls, "%u", lo);
        if (lo == 0 && !lox) {
            sb_printf(out, "%s'(%s)", sb_cstr(&ws), sb_cstr(&t));
            r.sgn = xe.sgn;
        } else {
            if (xe.prec < SVP_UNARY)
                sb_printf(out, "%s'((%s) >> %s)", sb_cstr(&ws), sb_cstr(&t), sb_cstr(&ls));
            else
                sb_printf(out, "%s'(%s >> %s)", sb_cstr(&ws), sb_cstr(&t), sb_cstr(&ls));
        }
        sb_free(&t);
        sb_free(&ws);
        sb_free(&ls);
        return r;
    }
    {
        const char *tn = sv_temp(g, x);
        g->sel_lo = lox;
        g->sel_w = wx;
        sv_select(g, tn, NULL, x->width, 0, lo, w, out);
    }
    return r;
}

/* element idx (ew bits each) of array value arr */
static SvEx sv_index(SvGen *g, const IrExpr *e, StrBuf *out)
{
    const IrExpr *arr = e->args[0], *idx = e->args[1];
    uint32_t ew = e->width, aw = arr->width, n = aw / ew, lo, s;
    int guard = sv_pow2_or_max(idx->width) > n;
    StrBuf sel = {0}, it = {0};
    SvEx r;
    r.prec = SVP_PRIM;
    r.sgn = 0;
    r.ty = NULL;
    if (idx->kind == IRE_CONST && idx->bits.width <= 64) {
        uint64_t k = ir_bits_low64(&idx->bits);
        if (k < n)
            return sv_slice_of(g, arr, (uint32_t)k * ew, ew, 1, out);
        sv_zero(ew, out);
        return r;
    }
    s = sv_slice_base(arr, &lo);
    if (s != IR_NONE) {
        StrBuf rb = {0};
        const IrType *st = sv_sigty(g, s);
        SvSel rs = sv_select(g, g->mn->sig[s], st, g->m->sigs[s].type->width, 0, lo, aw, &rb);
        if (rs.path && rs.ty && rs.ty->kind == IRT_ARRAY && rs.ty->elem->width == ew) {
            sv_index_text(g, idx, 1, 0, &it);
            sb_printf(&sel, "%s[%s]", sb_cstr(&rb), sb_cstr(&it));
            r.ty = sv_typed(g, rs.ty->elem) ? rs.ty->elem : NULL;
        } else if (rs.path && (!rs.ty || rs.ty->kind != IRT_ARRAY)) {
            sv_index_text(g, idx, ew, 0, &it);
            if (ew == 1)
                sb_printf(&sel, "%s[%s]", sb_cstr(&rb), sb_cstr(&it));
            else
                sb_printf(&sel, "%s[%s +: %u]", sb_cstr(&rb), sb_cstr(&it), ew);
        } else if (!st) {
            sv_index_text(g, idx, ew, lo, &it);
            if (ew == 1)
                sb_printf(&sel, "%s[%s]", g->mn->sig[s], sb_cstr(&it));
            else
                sb_printf(&sel, "%s[%s +: %u]", g->mn->sig[s], sb_cstr(&it), ew);
        }
        sb_free(&rb);
    }
    if (!sel.len && arr->kind == IRE_MEMREAD && !sv_mem_guard(g, arr)) {
        StrBuf b = {0};
        sv_ex(g, arr, 1, &b);
        sb_reset(&it);
        sv_index_text(g, idx, ew, 0, &it);
        if (ew == 1)
            sb_printf(&sel, "%s[%s]", sb_cstr(&b), sb_cstr(&it));
        else
            sb_printf(&sel, "%s[%s +: %u]", sb_cstr(&b), sb_cstr(&it), ew);
        sb_free(&b);
    }
    if (!sel.len) {
        if (!g->c->v2005 && g->written && sv_reads(arr, g->written)) {
            /* W'(arr >> idx*ew): exact, no temporary */
            StrBuf t = {0};
            SvEx xe = sv_ex(g, arr, 1, &t);
            sb_reset(&it);
            sv_index_text(g, idx, ew, 0, &it);
            if (xe.prec < SVP_UNARY)
                sb_printf(&sel, "%u'((%s) >> (%s))", ew, sb_cstr(&t), sb_cstr(&it));
            else
                sb_printf(&sel, "%u'(%s >> (%s))", ew, sb_cstr(&t), sb_cstr(&it));
            sb_free(&t);
        } else {
            const char *tn = sv_temp(g, arr);
            sb_reset(&it);
            sv_index_text(g, idx, ew, 0, &it);
            if (ew == 1)
                sb_printf(&sel, "%s[%s]", tn, sb_cstr(&it));
            else
                sb_printf(&sel, "%s[%s +: %u]", tn, sb_cstr(&it), ew);
        }
    }
    if (guard) {
        StrBuf gi = {0};
        SvEx ix = sv_ex(g, idx, 1, &gi);
        if (ix.prec < SVP_ADD)
            sb_printf(out, "(%s) < ", sb_cstr(&gi));
        else
            sb_printf(out, "%s < ", sb_cstr(&gi));
        sv_lit_u64(idx->width, n, out);
        sb_printf(out, " ? %s : ", sb_cstr(&sel));
        sv_zero(ew, out);
        r.prec = SVP_TERN;
        r.ty = NULL;
        sb_free(&gi);
    } else {
        sb_append(out, sel.data, sel.len);
    }
    sb_free(&sel);
    sb_free(&it);
    return r;
}

/* ---------------------------------------------------------- operators -- */

static int sv_op_prec(IrOp op)
{
    switch (op) {
    case IROP_ADD:
    case IROP_SUB: return SVP_ADD;
    case IROP_MUL:
    case IROP_DIV:
    case IROP_MOD: return SVP_MUL;
    case IROP_AND: return SVP_BAND;
    case IROP_OR: return SVP_BOR;
    case IROP_XOR: return SVP_BXOR;
    case IROP_SHL:
    case IROP_SHR:
    case IROP_ASHR: return SVP_SHIFT;
    case IROP_EQ:
    case IROP_NE: return SVP_EQ;
    case IROP_LT:
    case IROP_LE:
    case IROP_GT:
    case IROP_GE: return SVP_REL;
    case IROP_LAND: return SVP_LAND;
    case IROP_LOR: return SVP_LOR;
    default: return SVP_UNARY;
    }
}

static const char *sv_op_sym(IrOp op)
{
    switch (op) {
    case IROP_ADD: return "+";
    case IROP_SUB: return "-";
    case IROP_MUL: return "*";
    case IROP_DIV: return "/";
    case IROP_MOD: return "%";
    case IROP_AND: return "&";
    case IROP_OR: return "|";
    case IROP_XOR: return "^";
    case IROP_SHL: return "<<";
    case IROP_SHR: return ">>";
    case IROP_ASHR: return ">>>";
    case IROP_EQ: return "==";
    case IROP_NE: return "!=";
    case IROP_LT: return "<";
    case IROP_LE: return "<=";
    case IROP_GT: return ">";
    case IROP_GE: return ">=";
    case IROP_LAND: return "&&";
    case IROP_LOR: return "||";
    case IROP_NOT: return "~";
    case IROP_NEG: return "-";
    case IROP_LNOT: return "!";
    case IROP_RAND: return "&";
    case IROP_ROR: return "|";
    case IROP_RXOR: return "^";
    }
    return "?";
}

/* Emits child e; parenthesized unless its precedence is >= need (or it is a
 * binary `same_op`, for associative chains). */
static SvEx sv_child(SvGen *g, const IrExpr *e, int self, int need, int same_op, StrBuf *out)
{
    StrBuf t = {0};
    SvEx x = sv_ex(g, e, self, &t);
    int ok = x.prec >= need;
    if (!ok && same_op >= 0 && e->kind == IRE_BINARY && (int)e->op == same_op &&
        x.prec == sv_op_prec(e->op))
        ok = 1;
    if (ok) {
        sb_append(out, t.data ? t.data : "", t.len);
    } else {
        sb_putc(out, '(');
        sb_append(out, t.data ? t.data : "", t.len);
        sb_putc(out, ')');
        x.prec = SVP_PRIM;
    }
    sb_free(&t);
    return x;
}

/* operand of a signedness-sensitive operator: $signed(x) / x / $unsigned(x) */
static void sv_sign_operand(SvGen *g, const IrExpr *e, int want_signed, int need, StrBuf *out)
{
    StrBuf t = {0};
    SvEx x = sv_ex(g, e, 1, &t);
    if (want_signed)
        sb_printf(out, "$signed(%s)", sb_cstr(&t));
    else if (x.sgn)
        sb_printf(out, "$unsigned(%s)", sb_cstr(&t));
    else if (x.prec < need)
        sb_printf(out, "(%s)", sb_cstr(&t));
    else
        sb_puts(out, sb_cstr(&t));
    sb_free(&t);
}

static int sv_is_past1_of(const IrExpr *p, const IrExpr *x)
{
    return p->kind == IRE_PAST && p->lo == 1 && sv_expr_eq(p->args[0], x);
}

/* $stable / $rose / $fell patterns inside properties */
static int sv_prop_pattern(SvGen *g, const IrExpr *e, StrBuf *out)
{
    const IrExpr *a, *b;
    StrBuf t = {0};
    if (!g->in_prop || e->kind != IRE_BINARY)
        return 0;
    a = e->args[0];
    b = e->args[1];
    if (e->op == IROP_EQ || e->op == IROP_NE) {
        const IrExpr *x = NULL;
        if (sv_is_past1_of(b, a))
            x = a;
        else if (sv_is_past1_of(a, b))
            x = b;
        if (!x)
            return 0;
        sv_ex(g, x, 1, &t);
        sb_printf(out, "%s$stable(%s)", e->op == IROP_NE ? "!" : "", sb_cstr(&t));
        sb_free(&t);
        return 1;
    }
    if (e->op == IROP_LAND && e->width == 1) {
        /* x && !$past(x) -> $rose(x); !x && $past(x) -> $fell(x) */
        if (b->kind == IRE_UNARY && b->op == IROP_LNOT && sv_is_past1_of(b->args[0], a) && a->width == 1) {
            sv_ex(g, a, 1, &t);
            sb_printf(out, "$rose(%s)", sb_cstr(&t));
            sb_free(&t);
            return 1;
        }
        if (a->kind == IRE_UNARY && a->op == IROP_LNOT && sv_is_past1_of(b, a->args[0]) &&
            b->width == 1) {
            sv_ex(g, a->args[0], 1, &t);
            sb_printf(out, "$fell(%s)", sb_cstr(&t));
            sb_free(&t);
            return 1;
        }
    }
    return 0;
}

/* shift amounts are self-determined and unsigned: a constant is written in
 * plain decimal */
static void sv_shift_amount(SvGen *g, const IrExpr *y, StrBuf *out)
{
    const SvXExpr *vx = y->kind == IRE_CONST ? svp_get(g, y->bits.w, SVK_CONST) : NULL;
    if (vx) {
        svp_print(vx, svp_pn(g), SVX_PREC_PRIM, out);
        return;
    }
    if (y->kind == IRE_CONST && y->bits.width <= 64 && ir_bits_low64(&y->bits) < 0x7FFFFFFFu &&
        (y->bits.width == 64 || (ir_bits_low64(&y->bits) >> y->bits.width) == 0)) {
        sb_printf(out, "%u", (unsigned)ir_bits_low64(&y->bits));
        return;
    }
    sv_child(g, y, 1, SVP_UNARY, -1, out);
}

static SvEx sv_binary(SvGen *g, const IrExpr *e, int self, StrBuf *out)
{
    SvEx r, a, b;
    IrOp op = e->op;
    int p = sv_op_prec(op);
    const IrExpr *x = e->args[0], *y = e->args[1];
    r.prec = p;
    r.sgn = 0;
    r.ty = NULL;
    if (sv_prop_pattern(g, e, out)) {
        r.prec = SVP_UNARY;
        return r;
    }
    switch (op) {
    /* the operands of these operators are context-determined (self = 0) */
    case IROP_ADD:
    case IROP_MUL:
        a = sv_child(g, x, 0, p, -1, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        b = sv_child(g, y, 0, p + 1, (int)op, out);
        r.sgn = a.sgn && b.sgn;
        return r;
    case IROP_SUB:
        a = sv_child(g, x, 0, p, -1, out);
        sb_puts(out, " - ");
        b = sv_child(g, y, 0, p + 1, -1, out);
        r.sgn = a.sgn && b.sgn;
        return r;
    case IROP_AND:
    case IROP_OR:
    case IROP_XOR:
        a = sv_child(g, x, 0, SVP_UNARY, (int)op, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        b = sv_child(g, y, 0, SVP_UNARY, (int)op, out);
        r.sgn = a.sgn && b.sgn;
        return r;
    case IROP_DIV:
    case IROP_MOD: {
        StrBuf q = {0}, yt = {0};
        int zero_safe = y->kind == IRE_CONST && !ir_bits_is_zero(&y->bits);
        int wrap = e->is_signed && (!self || !zero_safe);
        if (e->is_signed) {
            sv_sign_operand(g, x, 1, SVP_MUL, &q);
            sb_printf(&q, " %s ", sv_op_sym(op));
            sv_sign_operand(g, y, 1, SVP_MUL + 1, &q);
        } else {
            sv_sign_operand(g, x, 0, SVP_MUL, &q);
            sb_printf(&q, " %s ", sv_op_sym(op));
            sv_sign_operand(g, y, 0, SVP_MUL + 1, &q);
        }
        if (zero_safe) {
            if (wrap) {
                sb_printf(out, "$unsigned(%s)", sb_cstr(&q));
                r.prec = SVP_PRIM;
            } else {
                sb_append(out, q.data, q.len);
                r.sgn = e->is_signed;
            }
        } else {
            /* x / 0 == 0, x % 0 == x */
            sv_child(g, y, 0, SVP_ADD, -1, &yt);
            sb_printf(out, "%s != ", sb_cstr(&yt));
            sv_zero_e(g, y, out);
            if (wrap)
                sb_printf(out, " ? $unsigned(%s) : ", sb_cstr(&q));
            else
                sb_printf(out, " ? %s : ", sb_cstr(&q));
            if (op == IROP_DIV)
                sv_zero_e(g, e, out);
            else
                sv_child(g, x, 0, SVP_TERN + 1, -1, out);
            r.prec = SVP_TERN;
        }
        sb_free(&q);
        sb_free(&yt);
        return r;
    }
    case IROP_SHL:
    case IROP_SHR:
        a = sv_child(g, x, 0, SVP_UNARY, -1, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        sv_shift_amount(g, y, out);
        r.sgn = a.sgn;
        return r;
    case IROP_ASHR: {
        StrBuf q = {0};
        StrBuf xt = {0};
        sv_ex(g, x, 1, &xt);
        sb_printf(&q, "$signed(%s) >>> ", sb_cstr(&xt));
        sv_shift_amount(g, y, &q);
        if (self) {
            sb_append(out, q.data, q.len);
            r.sgn = 1;
        } else {
            sb_printf(out, "$unsigned(%s)", sb_cstr(&q));
            r.prec = SVP_PRIM;
        }
        sb_free(&q);
        sb_free(&xt);
        return r;
    }
    case IROP_EQ:
    case IROP_NE: {
        const IrType *ea = sv_enum_of(g, x), *eb = sv_enum_of(g, y);
        if (y->kind == IRE_CONST && ea) {
            sv_child(g, x, 0, SVP_ADD, -1, out);
            sb_printf(out, " %s ", sv_op_sym(op));
            if (!sv_enum_name(g, ea, &y->bits, out))
                sv_lit(&y->bits, out);
            return r;
        }
        if (x->kind == IRE_CONST && eb) {
            if (!sv_enum_name(g, eb, &x->bits, out))
                sv_lit(&x->bits, out);
            sb_printf(out, " %s ", sv_op_sym(op));
            sv_child(g, y, 0, SVP_ADD, -1, out);
            return r;
        }
        sv_child(g, x, 0, SVP_ADD, -1, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        sv_child(g, y, 0, SVP_ADD, -1, out);
        return r;
    }
    case IROP_LT:
    case IROP_LE:
    case IROP_GT:
    case IROP_GE:
        sv_sign_operand(g, x, e->is_signed, SVP_ADD, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        sv_sign_operand(g, y, e->is_signed, SVP_ADD, out);
        return r;
    case IROP_LAND:
    case IROP_LOR:
        sv_child(g, x, 1, SVP_EQ, (int)op, out);
        sb_printf(out, " %s ", sv_op_sym(op));
        sv_child(g, y, 1, SVP_EQ, (int)op, out);
        return r;
    default: break;
    }
    sv_error(g, e->loc, "unknown binary operator");
    sb_puts(out, "1'b0");
    return r;
}

static SvEx sv_resize(SvGen *g, const IrExpr *e, int self, StrBuf *out)
{
    const IrExpr *x = e->args[0];
    uint32_t w = e->width, xw = x->width, lo;
    SvEx r;
    r.prec = SVP_PRIM;
    r.sgn = 0;
    r.ty = NULL;
    const SvXExpr *wx = svp_get(g, e, SVK_EXPRW), *xwx = svp_get(g, x, SVK_EXPRW);
    if (w == xw)
        return sv_ex(g, x, self, out);
    if (w < xw) {
        g->sel_w = wx;
        return sv_slice_of(g, x, 0, w, self, out);
    }
    if (x->kind == IRE_CONST) {
        IrBits b = ir_bits_resize(&g->c->scratch, &x->bits, w, e->is_signed);
        const SvXExpr *vx = svp_get(g, x->bits.w, SVK_CONST);
        if ((wx || vx) && (!e->is_signed || !ir_bits_get(&x->bits, xw - 1)))
            svp_const(g, wx, vx, w, &b, out);
        else
            sv_lit(&b, out);
        return r;
    }
    if (!e->is_signed) {
        int64_t dk;
        if ((wx || xwx) && !svp_const_diff(wx ? wx : svp_k(g, w), xwx ? xwx : svp_k(g, xw), &dk)) {
            /* zero-extension to a parameter-dependent width */
            StrBuf t = {0};
            SvEx xe = sv_ex(g, x, 1, &t);
            if (!g->c->v2005) {
                svp_print_castw(wx ? wx : svp_k(g, w), svp_pn(g), out);
                if (xe.sgn)
                    sb_printf(out, "'($unsigned(%s))", sb_cstr(&t));
                else
                    sb_printf(out, "'(%s)", sb_cstr(&t));
            } else {
                sb_puts(out, "{{");
                svp_print(svp_bin(g, SVX_SUB, wx ? wx : svp_k(g, w), xwx ? xwx : svp_k(g, xw)), svp_pn(g),
                          SVX_PREC_PRIM, out);
                sb_printf(out, "{1'b0}}, %s}", sb_cstr(&t));
            }
            sb_free(&t);
            return r;
        }
        sb_putc(out, '{');
        sv_zero(w - xw, out);
        sb_puts(out, ", ");
        sv_ex(g, x, 1, out);
        sb_putc(out, '}');
        return r;
    }
    r.sgn = 1;
    if (!g->c->v2005) {
        StrBuf t = {0}, ws = {0};
        SvEx xe = sv_ex(g, x, 1, &t);
        int primary = x->kind == IRE_SIG || (x->kind == IRE_SLICE && sv_slice_base(x, &lo) != IR_NONE);
        if (wx)
            svp_print_castw(wx, svp_pn(g), &ws);
        else
            sb_printf(&ws, "%u", w);
        if (primary && xe.sgn)
            sb_printf(out, "%s'(%s)", sb_cstr(&ws), sb_cstr(&t));
        else
            sb_printf(out, "%s'($signed(%s))", sb_cstr(&ws), sb_cstr(&t));
        sb_free(&t);
        sb_free(&ws);
        return r;
    }
    /* Verilog-2005: {{n{x[msb]}}, x} on a selectable value */
    {
        StrBuf xt = {0}, sb = {0};
        uint32_t s = sv_slice_base(x, &lo);
        if (s != IR_NONE || (x->kind == IRE_MEMREAD && !sv_mem_guard(g, x))) {
            sv_ex(g, x, 1, &xt);
            sv_slice_of(g, x, xw - 1, 1, 1, &sb);
        } else {
            const char *tn = sv_temp(g, x);
            sb_puts(&xt, tn);
            sv_select(g, tn, NULL, xw, 0, xw - 1, 1, &sb);
        }
        if (xw == 1)
            sb_printf(out, "{%u{%s}}", w, sb_cstr(&sb));
        else
            sb_printf(out, "{{%u{%s}}, %s}", w - xw, sb_cstr(&sb), sb_cstr(&xt));
        sb_free(&xt);
        sb_free(&sb);
        r.sgn = 0;
    }
    return r;
}

static SvEx sv_ex(SvGen *g, const IrExpr *e, int self, StrBuf *out)
{
    SvEx r;
    uint32_t i;
    r.prec = SVP_PRIM;
    r.sgn = 0;
    r.ty = NULL;
    switch (e->kind) {
    case IRE_CONST:
        if (!self && sv_kfill0(g, e, out))
            return r;
        sv_klit(g, e, out);
        return r;
    case IRE_SIG:
        if (e->sig >= g->m->nsigs || g->m->sigs[e->sig].kind == IRS_MEM) {
            sv_error(g, e->loc, "invalid signal reference");
            sb_puts(out, "1'b0");
            return r;
        }
        sb_puts(out, g->mn->sig[e->sig]);
        r.sgn = sv_sig_signed(g, e->sig);
        r.ty = sv_sigty(g, e->sig);
        return r;
    case IRE_SLICE:
        g->sel_w = svp_get(g, e, SVK_EXPRW);
        g->sel_lo = svp_get(g, e, SVK_LO);
        return sv_slice_of(g, e->args[0], e->lo, e->width, self, out);
    case IRE_INDEX:
        return sv_index(g, e, out);
    case IRE_UNARY: {
        SvEx x;
        sb_puts(out, sv_op_sym(e->op));
        x = sv_child(g, e->args[0], e->op == IROP_NOT || e->op == IROP_NEG ? 0 : 1, SVP_PRIM, -1, out);
        r.prec = SVP_UNARY;
        if (e->op == IROP_NOT || e->op == IROP_NEG)
            r.sgn = x.sgn;
        return r;
    }
    case IRE_BINARY:
        return sv_binary(g, e, self, out);
    case IRE_CONCAT:
        if (e->nargs == 1)
            return sv_ex(g, e->args[0], 1, out);
        for (i = 1; i < e->nargs; i++)
            if (!sv_expr_eq(e->args[i], e->args[0]))
                break;
        if (i == e->nargs) {
            sb_printf(out, "{%u{", e->nargs);
            sv_ex(g, e->args[0], 1, out);
            sb_puts(out, "}}");
            return r;
        }
        sb_putc(out, '{');
        for (i = 0; i < e->nargs; i++) {
            if (i)
                sb_puts(out, ", ");
            sv_ex(g, e->args[i], 1, out);
        }
        sb_putc(out, '}');
        return r;
    case IRE_MUX: {
        SvEx a, b;
        sv_child(g, e->args[0], 1, SVP_LOR, -1, out);
        sb_puts(out, " ? ");
        a = sv_child(g, e->args[1], 0, SVP_TERN + 1, -1, out);
        sb_puts(out, " : ");
        b = sv_child(g, e->args[2], 0, SVP_TERN, -1, out);
        r.prec = SVP_TERN;
        r.sgn = a.sgn && b.sgn;
        r.ty = a.ty == b.ty ? a.ty : NULL;
        return r;
    }
    case IRE_RESIZE:
        return sv_resize(g, e, self, out);
    case IRE_MEMREAD: {
        StrBuf at = {0};
        SvEx a = sv_ex(g, e->args[0], 1, &at);
        const char *mem = g->mn->sig[e->sig];
        if (a.sgn) {
            StrBuf w = {0};
            sb_printf(&w, "$unsigned(%s)", sb_cstr(&at));
            sb_free(&at);
            at = w;
        }
        if (sv_mem_guard(g, e)) {
            if (a.prec < SVP_ADD && !a.sgn)
                sb_printf(out, "(%s) < ", sb_cstr(&at));
            else
                sb_printf(out, "%s < ", sb_cstr(&at));
            {
                const SvXExpr *dx = svp_get(g, &g->m->sigs[e->sig], SVK_DEPTH);
                const SvXExpr *awx = svp_get(g, e->args[0], SVK_EXPRW);
                if (dx || awx) {
                    /* unsized: the depth may not fit the address width */
                    if (dx)
                        svp_print(dx, svp_pn(g), SVX_PREC_ADD, out);
                    else
                        sb_printf(out, "%u", g->m->sigs[e->sig].depth);
                } else {
                    sv_lit_u64(e->args[0]->width, g->m->sigs[e->sig].depth, out);
                }
            }
            sb_printf(out, " ? %s[%s] : ", mem, sb_cstr(&at));
            sv_zero_e(g, e, out);
            r.prec = SVP_TERN;
        } else {
            sb_printf(out, "%s[%s]", mem, sb_cstr(&at));
        }
        sb_free(&at);
        return r;
    }
    case IRE_PAST:
        sb_puts(out, "$past(");
        sv_ex(g, e->args[0], 1, out);
        if (e->lo != 1)
            sb_printf(out, ", %u", e->lo);
        sb_putc(out, ')');
        return r;
    }
    sv_error(g, e->loc, "unknown expression kind");
    sb_puts(out, "1'b0");
    return r;
}

/* a ?: tree with enum constants by name (no casts) */
static void sv_enum_mux(SvGen *g, const IrExpr *e, const IrType *ty, StrBuf *out)
{
    if (e->kind == IRE_CONST) {
        if (!sv_enum_name(g, ty, &e->bits, out))
            sv_lit(&e->bits, out);
        return;
    }
    if (e->kind == IRE_MUX) {
        StrBuf t = {0};
        SvEx c = sv_ex(g, e->args[0], 1, &t);
        if (c.prec < SVP_LOR)
            sb_printf(out, "(%s) ? ", sb_cstr(&t));
        else
            sb_printf(out, "%s ? ", sb_cstr(&t));
        sb_free(&t);
        if (e->args[1]->kind == IRE_MUX) {
            sb_putc(out, '(');
            sv_enum_mux(g, e->args[1], ty, out);
            sb_putc(out, ')');
        } else {
            sv_enum_mux(g, e->args[1], ty, out);
        }
        sb_puts(out, " : ");
        sv_enum_mux(g, e->args[2], ty, out);
        return;
    }
    sv_child(g, e, 0, SVP_TERN + 1, -1, out);
}

/* Emits e as the value assigned to a target of SV type ty (enum constants by
 * name, casts to enum types in SV-2012). */
static void sv_rhs(SvGen *g, const IrExpr *e, const IrType *ty, StrBuf *out)
{
    SvNames *n = g->c->n;
    if (ty && ty->kind == IRT_ENUM && svn_plain_enum(n, ty) && map_has(&g->types, MAP_PTR(ty)) &&
        ty->width == e->width) {
        if (e->kind == IRE_CONST) {
            if (sv_enum_name(g, ty, &e->bits, out))
                return;
            if (!g->c->v2005) {
                sb_printf(out, "%s'(", svn_tyname(n, ty));
                sv_lit(&e->bits, out);
                sb_putc(out, ')');
            } else {
                sv_lit(&e->bits, out);
            }
            return;
        }
        if (e->kind == IRE_MUX) {
            /* SV-2012: the whole conditional is cast (a ?: of enum
             * constants is not of the enum type for every tool) */
            if (!g->c->v2005)
                sb_printf(out, "%s'(", svn_tyname(n, ty));
            sv_enum_mux(g, e, ty, out);
            if (!g->c->v2005)
                sb_putc(out, ')');
            return;
        }
        if (!g->c->v2005) {
            StrBuf t = {0};
            SvEx x = sv_ex(g, e, 1, &t);
            if (x.ty == ty)
                sb_append(out, t.data, t.len);
            else
                sb_printf(out, "%s'(%s)", svn_tyname(n, ty), sb_cstr(&t));
            sb_free(&t);
            return;
        }
    }
    if (ty && svn_payload_enum(n, ty) && map_has(&g->types, MAP_PTR(ty)) && e->kind == IRE_CONST &&
        ty->width == e->width && ty->width > ty->tag_width) {
        /* {Tag, payload} */
        IrType *tt = svn_tagty(n, ty);
        IrBits tag = ir_bits_zero(&g->c->scratch, ty->tag_width);
        IrBits pay = ir_bits_zero(&g->c->scratch, ty->width - ty->tag_width);
        uint32_t i;
        for (i = 0; i < pay.width; i++)
            ir_bits_set(&pay, i, ir_bits_get(&e->bits, i));
        for (i = 0; i < tag.width; i++)
            ir_bits_set(&tag, i, ir_bits_get(&e->bits, pay.width + i));
        sb_putc(out, '{');
        if (!sv_enum_name(g, tt, &tag, out))
            sv_lit(&tag, out);
        sb_puts(out, ", ");
        sv_lit(&pay, out);
        sb_putc(out, '}');
        return;
    }
    if (sv_kfill0(g, e, out))
        return;
    sv_ex(g, e, 1, out);
}

/* ------------------------------------------------------------- lvalues -- */

/* finds an array type at [lo, ...) with element width ew inside t */
static const IrType *sv_array_at(const IrType *t, uint32_t lo, uint32_t ew)
{
    uint32_t i;
    while (t) {
        if (lo == 0 && t->kind == IRT_ARRAY && t->elem->width == ew)
            return t;
        if (t->kind == IRT_STRUCT) {
            const IrType *nt = NULL;
            for (i = 0; i < t->nfields; i++) {
                const IrField *f = &t->fields[i];
                if (lo >= f->lsb && lo < f->lsb + f->type->width) {
                    nt = f->type;
                    lo -= f->lsb;
                    break;
                }
            }
            t = nt;
        } else if (t->kind == IRT_ARRAY) {
            uint32_t k = lo / t->elem->width;
            lo -= k * t->elem->width;
            t = t->elem;
        } else {
            return NULL;
        }
    }
    return NULL;
}

/* Writes the assignment target; returns its SV type (for sv_rhs).  *guard
 * receives the in-range condition of a dynamic index when it can be out of
 * range (then the assignment must be skipped). */
static const IrType *sv_lvalue(SvGen *g, const IrLvalue *lv, StrBuf *out, StrBuf *guard)
{
    const IrSignal *s;
    const IrType *st;
    if (lv->sig >= g->m->nsigs) {
        sv_error(g, span_none(), "invalid assignment target");
        sb_puts(out, "_invalid_");
        return NULL;
    }
    s = &g->m->sigs[lv->sig];
    st = sv_sigty(g, lv->sig);
    if (!lv->index) {
        SvSel r;
        g->sel_lo = svp_get(g, lv, SVK_LVLO);
        g->sel_w = svp_get(g, lv, SVK_LVW);
        r = sv_select(g, g->mn->sig[lv->sig], st, s->type->width, sv_sig_signed(g, lv->sig), lv->lo,
                      lv->width, out);
        const IrType *at = sv_type_at(g, s->type, lv->lo, lv->width);
        return at ? at : r.ty;
    }
    {
        const IrType *at = sv_array_at(s->type, lv->lo, lv->width);
        uint32_t ew = lv->width, rw = at ? at->width : s->type->width - lv->lo, n = lv->len ? lv->len : rw / ew;
        StrBuf rb = {0}, it = {0};
        const IrType *res = NULL;
        SvSel rs = sv_select(g, g->mn->sig[lv->sig], st, s->type->width, 0, lv->lo, rw, &rb);
        if (rs.path && rs.ty && rs.ty->kind == IRT_ARRAY && rs.ty->elem->width == ew) {
            sv_index_text(g, lv->index, 1, 0, &it);
            sb_printf(out, "%s[%s]", sb_cstr(&rb), sb_cstr(&it));
        } else if (rs.path && (!rs.ty || rs.ty->kind != IRT_ARRAY)) {
            sv_index_text(g, lv->index, ew, 0, &it);
            if (ew == 1)
                sb_printf(out, "%s[%s]", sb_cstr(&rb), sb_cstr(&it));
            else
                sb_printf(out, "%s[%s +: %u]", sb_cstr(&rb), sb_cstr(&it), ew);
        } else if (!st) {
            sv_index_text(g, lv->index, ew, lv->lo, &it);
            if (ew == 1)
                sb_printf(out, "%s[%s]", g->mn->sig[lv->sig], sb_cstr(&it));
            else
                sb_printf(out, "%s[%s +: %u]", g->mn->sig[lv->sig], sb_cstr(&it), ew);
        } else {
            sv_error(g, s->loc, "unsupported dynamic assignment target");
            sb_puts(out, g->mn->sig[lv->sig]);
        }
        if (at)
            res = at->elem;
        if (guard && sv_pow2_or_max(lv->index->width) > n) {
            StrBuf t = {0};
            SvEx x = sv_ex(g, lv->index, 1, &t);
            if (x.prec < SVP_ADD)
                sb_printf(guard, "(%s) < ", sb_cstr(&t));
            else
                sb_printf(guard, "%s < ", sb_cstr(&t));
            sv_lit_u64(lv->index->width, n, guard);
            sb_free(&t);
        }
        sb_free(&rb);
        sb_free(&it);
        return res;
    }
}

/* ---------------------------------------------------------- statements -- */

static void sv_block(SvGen *g, const IrBlock *b, int ind, int nb, StrBuf *out);
static void sv_stmt(SvGen *g, const IrStmt *s, int ind, int nb, StrBuf *out);

/* indents every line of text by `extra` levels */
static void sv_reindent(const char *text, size_t len, int extra, StrBuf *out)
{
    size_t i;
    int bol = 1;
    for (i = 0; i < len; i++) {
        if (bol && text[i] != '\n' && text[i] != '`')
            sv_ind(out, extra);
        bol = text[i] == '\n';
        sb_putc(out, text[i]);
    }
}

static void sv_cond_text(SvGen *g, const IrExpr *c, int negate, StrBuf *out)
{
    StrBuf t = {0};
    SvEx x = sv_ex(g, c, 1, &t);
    if (!negate)
        sb_append(out, t.data ? t.data : "", t.len);
    else if (c->kind == IRE_UNARY && c->op == IROP_LNOT) {
        sb_reset(&t);
        sv_ex(g, c->args[0], 1, &t);
        sb_append(out, t.data, t.len);
    } else if (x.prec >= SVP_PRIM)
        sb_printf(out, "!%s", sb_cstr(&t));
    else
        sb_printf(out, "!(%s)", sb_cstr(&t));
    sb_free(&t);
}

static void sv_if(SvGen *g, const IrStmt *s, int ind, int nb, StrBuf *out)
{
    StrBuf pre = {0}, head = {0};
    StrBuf *saved = g->spre;
    int saved_ind = g->spre_ind;
    const IrBlock *tb = &s->then_b, *eb = &s->else_b;
    int negate = 0;
    if (tb->n == 0 && eb->n == 0)
        return;
    if (tb->n == 0) {
        negate = 1;
        tb = &s->else_b;
        eb = &s->then_b;
    }
    g->spre = &pre;
    g->spre_ind = ind;
    sv_cond_text(g, s->cond, negate, &head);
    g->spre = saved;
    g->spre_ind = saved_ind;
    sb_append(out, pre.data ? pre.data : "", pre.len);
    sv_ind(out, ind);
    sb_printf(out, "if (%s) begin", sb_cstr(&head));
    sv_mark(g, s->loc, out);
    sb_putc(out, '\n');
    sv_block(g, tb, ind + 1, nb, out);
    sv_ind(out, ind);
    sb_puts(out, "end");
    if (eb->n) {
        StrBuf sub = {0};
        if (eb->n == 1 && eb->stmts[0]->kind == IRST_IF) {
            sv_stmt(g, eb->stmts[0], ind, nb, &sub);
            if (sub.len > (size_t)ind * 4 + 3 && memcmp(sub.data + ind * 4, "if (", 4) == 0) {
                sb_puts(out, " else ");
                sb_append(out, sub.data + ind * 4, sub.len - (size_t)ind * 4);
                sb_free(&sub);
                sb_free(&pre);
                sb_free(&head);
                return;
            }
            sb_puts(out, " else begin\n");
            sv_reindent(sub.data ? sub.data : "", sub.len, 1, out);
        } else {
            sb_puts(out, " else begin\n");
            sv_block(g, eb, ind + 1, nb, out);
        }
        sv_ind(out, ind);
        sb_puts(out, "end");
        sb_free(&sub);
    }
    sb_putc(out, '\n');
    sb_free(&pre);
    sb_free(&head);
}

static void sv_collect_targets(const IrBlock *b, Map *set)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN: map_put(set, s->lhs.sig, 1); break;
        case IRST_IF:
            sv_collect_targets(&s->then_b, set);
            sv_collect_targets(&s->else_b, set);
            break;
        case IRST_CASE:
            for (j = 0; j < s->narms; j++)
                sv_collect_targets(&s->arms[j].body, set);
            break;
        default: break;
        }
    }
}

static void sv_case(SvGen *g, const IrStmt *s, int ind, int nb, StrBuf *out)
{
    StrBuf pre = {0}, subj = {0};
    StrBuf *saved = g->spre;
    int saved_ind = g->spre_ind;
    const IrType *et = sv_enum_of(g, s->subject);
    uint32_t i, j, w = s->subject->width;
    int has_default = 0, full = 0;
    g->spre = &pre;
    g->spre_ind = ind;
    sv_ex(g, s->subject, 0, &subj);
    g->spre = saved;
    g->spre_ind = saved_ind;
    for (i = 0; i < s->narms; i++)
        if (s->arms[i].nvals == 0)
            has_default = 1;
    if (!has_default && w <= 16) {
        /* do the values cover every code? */
        Map seen = {0};
        for (i = 0; i < s->narms; i++)
            for (j = 0; j < s->arms[i].nvals; j++)
                map_put(&seen, ir_bits_low64(&s->arms[i].vals[j]), 1);
        full = map_len(&seen) == ((size_t)1 << w);
        map_free(&seen);
    }
    sb_append(out, pre.data ? pre.data : "", pre.len);
    sv_ind(out, ind);
    sb_printf(out, "%scase (%s)", g->c->v2005 ? "" : "unique ", sb_cstr(&subj));
    sv_mark(g, s->loc, out);
    sb_putc(out, '\n');
    for (i = 0; i < s->narms; i++) {
        const IrCaseArm *a = &s->arms[i];
        StrBuf body = {0};
        sv_ind(out, ind + 1);
        if (a->nvals == 0)
            sb_puts(out, "default");
        for (j = 0; j < a->nvals; j++) {
            if (j)
                sb_puts(out, ", ");
            if (!sv_enum_name(g, et, &a->vals[j], out)) {
                const SvXExpr *vx = svp_get(g, a->vals[j].w, SVK_CONST);
                const SvXExpr *swx = svp_get(g, s->subject, SVK_EXPRW);
                if (vx || swx)
                    svp_const(g, swx, vx, w, &a->vals[j], out);
                else
                    sv_lit(&a->vals[j], out);
            }
        }
        sb_puts(out, ":");
        if (a->body.n == 1) {
            sv_stmt(g, a->body.stmts[0], 0, nb, &body);
        }
        if (a->body.n == 0) {
            sb_puts(out, " ;\n");
        } else if (a->body.n == 1 && body.len && memchr(body.data, '\n', body.len - 1) == NULL &&
                   body.data[0] != ' ' && body.data[0] != '`' && strncmp(body.data, "if ", 3) != 0) {
            sb_putc(out, ' ');
            sb_append(out, body.data, body.len);
        } else {
            sb_puts(out, " begin\n");
            if (a->body.n == 1)
                sv_reindent(body.data, body.len, ind + 2, out);
            else
                sv_block(g, &a->body, ind + 2, nb, out);
            sv_ind(out, ind + 1);
            sb_puts(out, "end\n");
        }
        sb_free(&body);
    }
    if (!has_default && !full && g->c->v2005) {
        /* Verilog-2005 has no `unique`: an explicit default keeps synthesis
         * latch-free; values the IR rules out are don't-cares. */
        sv_ind(out, ind + 1);
        if (nb) {
            sb_puts(out, "default: ;\n");
        } else {
            Map tg = {0};
            uint32_t k, n = 0;
            for (i = 0; i < s->narms; i++)
                sv_collect_targets(&s->arms[i].body, &tg);
            for (k = 0; k < g->m->nsigs; k++)
                if (map_has(&tg, k) && !map_has(&g->defassigned, k))
                    n++;
            if (!n) {
                sb_puts(out, "default: ;\n");
            } else {
                sb_puts(out, "default: begin\n");
                for (k = 0; k < g->m->nsigs; k++)
                    if (map_has(&tg, k) && !map_has(&g->defassigned, k)) {
                        const SvXExpr *kx = svp_get(g, &g->m->sigs[k], SVK_SIGW);
                        sv_ind(out, ind + 2);
                        if (kx) {
                            sb_printf(out, "%s = {", g->mn->sig[k]);
                            svp_print_castw(kx, svp_pn(g), out);
                            sb_puts(out, "{1'bx}};\n");
                        } else {
                            sb_printf(out, "%s = %u'bx;\n", g->mn->sig[k], g->m->sigs[k].type->width);
                        }
                    }
                sv_ind(out, ind + 1);
                sb_puts(out, "end\n");
            }
            map_free(&tg);
        }
    }
    sv_ind(out, ind);
    sb_puts(out, "endcase\n");
    sb_free(&pre);
    sb_free(&subj);
}

static void sv_memwrite(SvGen *g, const IrStmt *s, int ind, StrBuf *out)
{
    StrBuf at = {0}, mem = {0};
    const IrSignal *ms = &g->m->sigs[s->mem];
    uint32_t w = ms->type->width, nbytes = (w + 7) / 8, i;
    SvEx a = sv_ex(g, s->addr, 1, &at);
    if (a.sgn) {
        StrBuf t = {0};
        sb_printf(&t, "$unsigned(%s)", sb_cstr(&at));
        sb_free(&at);
        at = t;
    }
    sb_printf(&mem, "%s[%s]", g->mn->sig[s->mem], sb_cstr(&at));
    if (!s->mask) {
        sv_ind(out, ind);
        sb_printf(out, "%s <= ", sb_cstr(&mem));
        sv_rhs(g, s->data, NULL, out);
        sb_putc(out, ';');
        sv_mark(g, s->loc, out);
        sb_putc(out, '\n');
    } else {
        for (i = 0; i < nbytes; i++) {
            uint32_t lo = 8 * i, bw = TERN_MIN(8u, w - lo);
            StrBuf lhs = {0}, rhs = {0};
            sv_select(g, sb_cstr(&mem), NULL, w, 0, lo, bw, &lhs);
            sv_slice_of(g, s->data, lo, bw, 1, &rhs);
            sv_ind(out, ind);
            if (s->mask->kind == IRE_CONST) {
                if (!ir_bits_get(&s->mask->bits, i)) {
                    sb_free(&lhs);
                    sb_free(&rhs);
                    continue;
                }
                sb_printf(out, "%s <= %s;", sb_cstr(&lhs), sb_cstr(&rhs));
            } else {
                StrBuf mk = {0};
                sv_slice_of(g, s->mask, i, 1, 1, &mk);
                sb_printf(out, "if (%s) %s <= %s;", sb_cstr(&mk), sb_cstr(&lhs), sb_cstr(&rhs));
                sb_free(&mk);
            }
            if (i == 0)
                sv_mark(g, s->loc, out);
            sb_putc(out, '\n');
            sb_free(&lhs);
            sb_free(&rhs);
        }
    }
    sb_free(&at);
    sb_free(&mem);
}

static void sv_assert(SvGen *g, const IrStmt *s, int ind, StrBuf *out)
{
    StrBuf c = {0}, msg = {0}, t = {0};
    const SourceFile *f = s->loc.file ? src_get(s->loc.file) : NULL;
    int v2005 = g->c->v2005;
    if (!g->c->o->emit_assertions || (v2005 && s->akind == IRA_COVER))
        return;
    if (s->msg)
        sb_puts(&msg, s->msg);
    else if (f && f->path)
        sb_printf(&msg, "assertion failed at %s:%u", f->path, s->loc.line);
    else
        sb_puts(&msg, "assertion failed");
    if (v2005 && !g->rst_guard) {
        /* if (!cond) $display(...) */
        sv_cond_text(g, s->cond, 1, &c);
    } else {
        SvEx x = sv_ex(g, s->cond, 1, &t);
        int par = x.prec < SVP_EQ;
        if (g->rst_guard && s->akind == IRA_COVER)
            sb_printf(&c, "!(%s) && %s%s%s", g->rst_guard, par ? "(" : "", sb_cstr(&t), par ? ")" : "");
        else if (g->rst_guard)
            sb_printf(&c, "%s%s || %s%s%s%s", v2005 ? "!(" : "", g->rst_guard, par ? "(" : "", sb_cstr(&t),
                      par ? ")" : "", v2005 ? ")" : "");
        else
            sb_append(&c, t.data, t.len);
    }
    sb_puts(out, "`ifndef SYNTHESIS\n");
    sv_ind(out, ind);
    if (v2005) {
        /* Verilog-2005 has no assertions: report like $error would */
        StrBuf m2 = {0};
        size_t i;
        sb_puts(&m2, "ERROR: %m: ");
        for (i = 0; i < msg.len; i++) {
            if (msg.data[i] == '%')
                sb_putc(&m2, '%');
            sb_putc(&m2, msg.data[i]);
        }
        sb_printf(out, "if (%s) $display(", sb_cstr(&c));
        sv_str_lit(sb_cstr(&m2), out, 0);
        sb_puts(out, ");");
        sb_free(&m2);
    } else if (s->akind == IRA_COVER) {
        sb_printf(out, "cover (%s);", sb_cstr(&c));
    } else {
        sb_printf(out, "%s (%s) else $error(", s->akind == IRA_ASSUME ? "assume" : "assert", sb_cstr(&c));
        sv_str_lit(sb_cstr(&msg), out, 1);
        sb_puts(out, ");");
    }
    sv_mark(g, s->loc, out);
    sb_puts(out, "\n`endif\n");
    sb_free(&c);
    sb_free(&msg);
    sb_free(&t);
}

static void sv_stmt(SvGen *g, const IrStmt *s, int ind, int nb, StrBuf *out)
{
    switch (s->kind) {
    case IRST_ASSIGN: {
        StrBuf pre = {0}, lhs = {0}, rhs = {0}, guard = {0};
        StrBuf *saved = g->spre;
        int saved_ind = g->spre_ind;
        const IrType *ty;
        g->spre = &pre;
        g->spre_ind = ind;
        ty = sv_lvalue(g, &s->lhs, &lhs, &guard);
        sv_rhs(g, s->rhs, ty, &rhs);
        g->spre = saved;
        g->spre_ind = saved_ind;
        sb_append(out, pre.data ? pre.data : "", pre.len);
        sv_ind(out, ind);
        if (guard.len)
            sb_printf(out, "if (%s) ", sb_cstr(&guard));
        sb_printf(out, "%s %s %s;", sb_cstr(&lhs), nb ? "<=" : "=", sb_cstr(&rhs));
        sv_mark(g, s->loc, out);
        sb_putc(out, '\n');
        sb_free(&pre);
        sb_free(&lhs);
        sb_free(&rhs);
        sb_free(&guard);
        break;
    }
    case IRST_IF: sv_if(g, s, ind, nb, out); break;
    case IRST_CASE: sv_case(g, s, ind, nb, out); break;
    case IRST_MEMWRITE:
        if (!nb)
            sv_error(g, s->loc, "memory write outside a clocked process");
        sv_memwrite(g, s, ind, out);
        break;
    case IRST_ASSERT: {
        StrBuf pre = {0}, body = {0};
        StrBuf *saved = g->spre;
        int saved_ind = g->spre_ind;
        g->spre = &pre;
        g->spre_ind = ind;
        sv_assert(g, s, ind, &body);
        g->spre = saved;
        g->spre_ind = saved_ind;
        sb_append(out, pre.data ? pre.data : "", pre.len);
        sb_append(out, body.data ? body.data : "", body.len);
        sb_free(&pre);
        sb_free(&body);
        break;
    }
    }
}

static void sv_block(SvGen *g, const IrBlock *b, int ind, int nb, StrBuf *out)
{
    uint32_t i;
    VEC(uint32_t) added = {0};
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        sv_stmt(g, s, ind, nb, out);
        if (!nb && s->kind == IRST_ASSIGN && !s->lhs.index && s->lhs.lo == 0 &&
            s->lhs.sig < g->m->nsigs && s->lhs.width == g->m->sigs[s->lhs.sig].type->width &&
            !map_has(&g->defassigned, s->lhs.sig)) {
            map_put(&g->defassigned, s->lhs.sig, 1);
            vec_push(added, s->lhs.sig);
        }
    }
    for (i = 0; i < added.len; i++)
        map_del(&g->defassigned, added.data[i]);
    vec_free(added);
}

/* ------------------------------------------------- statement projection -- */

/* Copy of block b keeping only assignments to signals in `keep` (and memory
 * writes / assertions when asked); empty ifs and cases are dropped. */
static IrBlock sv_project(SvGen *g, const IrBlock *b, const Map *keep, int mem, int asserts)
{
    IrStmtList l = {0};
    uint32_t i, j;
    Arena *a = &g->c->scratch;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN:
            if (map_has(keep, s->lhs.sig))
                ir_stmts_push(&l, (IrStmt *)s);
            break;
        case IRST_MEMWRITE:
            if (mem)
                ir_stmts_push(&l, (IrStmt *)s);
            break;
        case IRST_ASSERT:
            if (asserts && g->c->o->emit_assertions)
                ir_stmts_push(&l, (IrStmt *)s);
            break;
        case IRST_IF: {
            IrBlock t = sv_project(g, &s->then_b, keep, mem, asserts);
            IrBlock e = sv_project(g, &s->else_b, keep, mem, asserts);
            if (t.n || e.n) {
                IrStmt *c = ANEW(a, IrStmt);
                *c = *s;
                c->then_b = t;
                c->else_b = e;
                ir_stmts_push(&l, c);
            }
            break;
        }
        case IRST_CASE: {
            IrCaseArm *arms = ANEWN(a, IrCaseArm, s->narms ? s->narms : 1);
            int any = 0;
            for (j = 0; j < s->narms; j++) {
                arms[j] = s->arms[j];
                arms[j].body = sv_project(g, &s->arms[j].body, keep, mem, asserts);
                if (arms[j].body.n)
                    any = 1;
            }
            if (any) {
                IrStmt *c = ANEW(a, IrStmt);
                *c = *s;
                c->arms = arms;
                ir_stmts_push(&l, c);
            }
            break;
        }
        }
    }
    {
        IrBlock r = ir_block(a, (uint32_t)l.len, l.data);
        free(l.data);
        return r;
    }
}

/* ------------------------------------------------------------ processes -- */

static int sv_block_has_mem(const IrBlock *b)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (s->kind == IRST_MEMWRITE)
            return 1;
        if (s->kind == IRST_IF && (sv_block_has_mem(&s->then_b) || sv_block_has_mem(&s->else_b)))
            return 1;
        if (s->kind == IRST_CASE)
            for (j = 0; j < s->narms; j++)
                if (sv_block_has_mem(&s->arms[j].body))
                    return 1;
    }
    return 0;
}

typedef struct SvDom {
    const char *clk;
    int falling;
    const char *rst;     /* NULL if none */
    int async;
    int active_low;
} SvDom;

static SvDom sv_dom(SvGen *g, uint32_t d)
{
    SvDom r;
    const IrDomain *dm = &g->m->domains[d];
    memset(&r, 0, sizeof r);
    r.clk = g->mn->sig[dm->clock];
    r.falling = g->m->sigs[dm->clock].type->kind == IRT_CLOCK && g->m->sigs[dm->clock].type->falling;
    if (dm->reset != IR_NONE && dm->reset < g->m->nsigs) {
        const IrType *rt = g->m->sigs[dm->reset].type;
        r.rst = g->mn->sig[dm->reset];
        r.async = rt->kind == IRT_RESET && rt->async;
        r.active_low = rt->kind == IRT_RESET && rt->active_low;
    }
    return r;
}

static void sv_always_head(SvGen *g, const SvDom *dm, int with_async, Span loc, StrBuf *out)
{
    sb_printf(out, "    %s @(%s %s", g->c->v2005 ? "always" : "always_ff", dm->falling ? "negedge" : "posedge",
              dm->clk);
    if (with_async && dm->rst && dm->async)
        sb_printf(out, " or %s %s", dm->active_low ? "negedge" : "posedge", dm->rst);
    sb_puts(out, ") begin");
    g->dedupe_marks = 0;
    sv_mark(g, loc, out);
    sv_marks_begin(g, loc);
    sb_putc(out, '\n');
}

static void sv_reset_value(SvGen *g, uint32_t r, int ind, int nb, StrBuf *out)
{
    const IrSignal *s = &g->m->sigs[r];
    IrExpr k;
    memset(&k, 0, sizeof k);
    k.kind = IRE_CONST;
    k.width = s->type->width;
    k.bits = s->init;
    k.sig = IR_NONE;
    sv_ind(out, ind);
    sb_printf(out, "%s %s ", g->mn->sig[r], nb ? "<=" : "=");
    g->kwidth = svp_get(g, s, SVK_SIGW);
    sv_rhs(g, &k, s->type, out);
    g->kwidth = NULL;
    sb_puts(out, ";\n");
}

static void sv_clocked(SvGen *g, const IrProcess *p, StrBuf *out)
{
    SvDom dm;
    Map init = {0}, noinit = {0};
    uint32_t i, ninit = 0;
    StrBuf body = {0};
    if (p->domain >= g->m->ndomains) {
        sv_error(g, p->loc, "clocked process without a valid domain");
        return;
    }
    dm = sv_dom(g, p->domain);
    g->dedupe_marks = 0;
    for (i = 0; i < p->nregs; i++) {
        uint32_t r = p->regs[i];
        if (r >= g->m->nsigs)
            continue;
        if (g->m->sigs[r].has_init) {
            map_put(&init, r, 1);
            ninit++;
        } else {
            map_put(&noinit, r, 1);
        }
    }
    {   /* assignments to signals missing from regs: reset-less */
        Map all = {0};
        size_t it = 0;
        uint64_t k, v;
        sv_collect_targets(&p->body, &all);
        while (map_next(&all, &it, &k, &v))
            if (!map_has(&init, k))
                map_put(&noinit, k, 1);
        map_free(&all);
    }
    if (dm.rst && ninit) {
        IrBlock a = p->body, b = p->body;
        int split = map_len(&noinit) > 0 || sv_block_has_mem(&p->body);
        if (split) {
            a = sv_project(g, &p->body, &init, 0, 1);
            b = sv_project(g, &p->body, &noinit, 1, 0);
        }
        /* the reset process: if (rst) ... else body */
        {
            IrStmt rs;
            StrBuf rb = {0};
            memset(&rs, 0, sizeof rs);
            sv_always_head(g, &dm, 1, p->loc, &body);
            sb_printf(&rb, "        if (%s%s) begin\n", dm.active_low ? "!" : "", dm.rst);
            for (i = 0; i < p->nregs; i++)
                if (p->regs[i] < g->m->nsigs && map_has(&init, p->regs[i]))
                    sv_reset_value(g, p->regs[i], 3, 1, &rb);
            sb_puts(&rb, "        end");
            if (a.n) {
                StrBuf sub = {0};
                if (a.n == 1 && a.stmts[0]->kind == IRST_IF) {
                    sv_stmt(g, a.stmts[0], 2, 1, &sub);
                    if (sub.len > 12 && memcmp(sub.data + 8, "if (", 4) == 0) {
                        sb_puts(&rb, " else ");
                        sb_append(&rb, sub.data + 8, sub.len - 8);
                    } else {
                        sb_puts(&rb, " else begin\n");
                        sv_reindent(sub.data, sub.len, 1, &rb);
                        sb_puts(&rb, "        end\n");
                    }
                } else {
                    sb_puts(&rb, " else begin\n");
                    sv_block(g, &a, 3, 1, &rb);
                    sb_puts(&rb, "        end\n");
                }
                sb_free(&sub);
            } else {
                sb_putc(&rb, '\n');
            }
            sb_append(&body, rb.data, rb.len);
            sb_puts(&body, "    end\n");
            sb_free(&rb);
        }
        if (split && b.n) {
            const char *saved = g->rst_guard;
            sb_putc(&body, '\n');
            sv_always_head(g, &dm, 0, p->loc, &body);
            sv_block(g, &b, 2, 1, &body);
            sb_puts(&body, "    end\n");
            g->rst_guard = saved;
        }
    } else {
        StrBuf guard = {0};
        const char *saved = g->rst_guard;
        if (dm.rst) {
            sb_printf(&guard, "%s%s", dm.active_low ? "!" : "", dm.rst);
            g->rst_guard = sb_cstr(&guard);
        }
        {
            StrBuf head = {0}, blk = {0};
            sv_always_head(g, &dm, 0, p->loc, &head);
            sv_block(g, &p->body, 2, 1, &blk);
            if (blk.len) {
                sb_append(&body, head.data, head.len);
                sb_append(&body, blk.data, blk.len);
                sb_puts(&body, "    end\n");
            }
            sb_free(&head);
            sb_free(&blk);
        }
        g->rst_guard = saved;
        sb_free(&guard);
        if (!dm.rst && ninit) {
            /* power-on values (spec 6.4): FPGA targets initialize the
             * declaration (`logic [3:0] p = 4'd5;`, sv_decl_group), which
             * synthesis turns into the register's configuration value;
             * unlike an `initial` block it is not a second driver of a
             * variable written by always_ff (Verilator MULTIDRIVEN) */
            if (g->c->o->target_kind == SV_TARGET_FPGA) {
                for (i = 0; i < p->nregs; i++)
                    if (p->regs[i] < g->m->nsigs && map_has(&init, p->regs[i]))
                        map_put(&g->poweron, p->regs[i], 1);
            } else {
                for (i = 0; i < p->nregs; i++) {
                    uint32_t r = p->regs[i];
                    if (r < g->m->nsigs && map_has(&init, r)) {
                        const IrSignal *s = &g->m->sigs[r];
                        IrExpr k;
                        memset(&k, 0, sizeof k);
                        k.kind = IRE_CONST;
                        k.width = s->type->width;
                        k.bits = s->init;
                        sb_printf(&body, "    // WARNING: power-on value of %s (", g->mn->sig[r]);
                        g->kwidth = svp_get(g, s, SVK_SIGW);
                        sv_klit(g, &k, &body);
                        g->kwidth = NULL;
                        sb_puts(&body, ") dropped: ASIC target and the domain has no reset\n");
                        if (!g->c->validating)
                            diag_warning(s->loc,
                                     "register `%s` has a power-on value but its domain has no reset; "
                                     "the ASIC target ignores it",
                                     s->path);
                    }
                }
            }
        }
    }
    g->dedupe_marks = 0;
    /* module-level temporaries go before the process */
    if (body.len)
        sb_putc(out, '\n');
    sb_append(out, g->pre.data ? g->pre.data : "", g->pre.len);
    sb_reset(&g->pre);
    sb_append(out, body.data ? body.data : "", body.len);
    sb_free(&body);
    map_free(&init);
    map_free(&noinit);
}

static void sv_comb(SvGen *g, const IrProcess *p, StrBuf *out)
{
    Map written = {0};
    StrBuf body = {0};
    size_t i;
    sv_collect_targets(&p->body, &written);
    g->written = &written;
    vec_clear(g->locals);
    map_clear(&g->defassigned);
    sv_marks_begin(g, p->loc);
    sv_block(g, &p->body, 2, 0, &body);
    g->dedupe_marks = 0;
    g->written = NULL;
    if (!body.len && !g->locals.len) {   /* only stripped assertions */
        sb_append(out, g->pre.data ? g->pre.data : "", g->pre.len);
        sb_reset(&g->pre);
        sb_free(&body);
        map_free(&written);
        return;
    }
    sb_putc(out, '\n');
    sb_append(out, g->pre.data ? g->pre.data : "", g->pre.len);
    sb_reset(&g->pre);
    sb_printf(out, "    %s", g->c->v2005 ? "always @* begin" : "always_comb begin");
    sv_mark(g, p->loc, out);
    sb_putc(out, '\n');
    for (i = 0; i < g->locals.len; i++) {
        const SvTemp *t = &g->temps.data[g->locals.data[i]];
        sv_ind(out, 2);
        sb_printf(out, "%s = ", t->name);
        sv_zero_e(g, t->expr, out);
        sb_puts(out, ";\n");
    }
    sb_append(out, body.data ? body.data : "", body.len);
    sb_puts(out, "    end\n");
    vec_clear(g->locals);
    sb_free(&body);
    map_free(&written);
}

/* ---------------------------------------------------------- attributes -- */

static int sv_attr_text(const IrAttr *a, StrBuf *out)
{
    const char *n = a->name, *args = a->args;
    StrBuf v = {0};
    size_t i;
    int numeric = 1;
    if (!n)
        return 0;
    if (tern_streq(n, "test_port") || tern_streq(n, "clock") || tern_streq(n, "top") ||
        tern_streq(n, "cdc") || tern_streq(n, "doc") || tern_streq(n, "sv_name") ||
        tern_streq(n, "encoding") || tern_streq(n, "unsafe_cross") || tern_streq(n, "allow"))
        return 0;
    if (tern_streq(n, "async_reg")) {
        sb_puts(out, "ASYNC_REG = \"TRUE\"");
        return 1;
    }
    if (args)
        for (i = 0; args[i]; i++)
            if (args[i] != '"' && args[i] != ' ')
                sb_putc(&v, args[i]);
    if (tern_streq(n, "ram"))
        n = "ram_style";
    else if (tern_streq(n, "rom"))
        n = "rom_style";
    if (!v.len) {
        if (tern_streq(n, "keep") || tern_streq(n, "dont_touch") || tern_streq(n, "mark_debug"))
            sb_printf(out, "%s = \"true\"", svn_sanitize(n));
        else
            sb_puts(out, svn_sanitize(n));
        sb_free(&v);
        return 1;
    }
    for (i = 0; i < v.len; i++)
        if (!tern_is_digit((unsigned char)v.data[i]))
            numeric = 0;
    if (numeric)
        sb_printf(out, "%s = %s", svn_sanitize(n), v.data);
    else
        sb_printf(out, "%s = \"%s\"", svn_sanitize(n), v.data);
    sb_free(&v);
    return 1;
}

static void sv_attrs(uint32_t n, const IrAttr *a, StrBuf *out)
{
    uint32_t i;
    int any = 0;
    for (i = 0; i < n; i++) {
        StrBuf t = {0};
        if (sv_attr_text(&a[i], &t)) {
            sb_puts(out, any ? ", " : "(* ");
            sb_append(out, t.data, t.len);
            any = 1;
        }
        sb_free(&t);
    }
    if (any)
        sb_puts(out, " *) ");
}

static int sv_has_attr(const IrSignal *s, const char *name)
{
    uint32_t i;
    for (i = 0; i < s->nattrs; i++)
        if (tern_streq(s->attrs[i].name, name))
            return 1;
    return 0;
}

/* ------------------------------------------------------------ properties -- */

static int sv_seq_is_sequence(const IrSeq *q)
{
    switch (q->kind) {
    case IRQ_EXPR: return 1;
    case IRQ_DELAY: return sv_seq_is_sequence(q->a) && sv_seq_is_sequence(q->b);
    default: return 0;
    }
}

static void sv_seq(SvGen *g, const IrSeq *q, StrBuf *out);

static void sv_seq_operand(SvGen *g, const IrSeq *q, StrBuf *out)
{
    if (q->kind == IRQ_EXPR) {
        StrBuf t = {0};
        SvEx x = sv_ex(g, q->expr, 1, &t);
        if (x.prec >= SVP_UNARY)
            sb_append(out, t.data, t.len);
        else
            sb_printf(out, "(%s)", sb_cstr(&t));
        sb_free(&t);
        return;
    }
    sb_putc(out, '(');
    sv_seq(g, q, out);
    sb_putc(out, ')');
}

static void sv_seq(SvGen *g, const IrSeq *q, StrBuf *out)
{
    switch (q->kind) {
    case IRQ_EXPR: sv_seq_operand(g, q, out); return;
    case IRQ_DELAY:
        if (q->a->kind == IRQ_DELAY)
            sv_seq(g, q->a, out);
        else
            sv_seq_operand(g, q->a, out);
        if (q->hi == q->lo)
            sb_printf(out, " ##%u ", q->lo);
        else if (q->hi == IR_NONE)
            sb_printf(out, " ##[%u:$] ", q->lo);
        else
            sb_printf(out, " ##[%u:%u] ", q->lo, q->hi);
        sv_seq_operand(g, q->b, out);
        return;
    case IRQ_IMPL:
    case IRQ_IMPL_NEXT:
        if (sv_seq_is_sequence(q->a) && q->a->kind == IRQ_DELAY)
            sv_seq(g, q->a, out);
        else
            sv_seq_operand(g, q->a, out);
        sb_puts(out, q->kind == IRQ_IMPL ? " |-> " : " |=> ");
        if (q->b->kind == IRQ_IMPL || q->b->kind == IRQ_IMPL_NEXT || q->b->kind == IRQ_EVENTUALLY ||
            (q->b->kind == IRQ_DELAY && sv_seq_is_sequence(q->b)))
            sv_seq(g, q->b, out);
        else
            sv_seq_operand(g, q->b, out);
        return;
    case IRQ_ALWAYS:
        sb_puts(out, "always ");
        sv_seq_operand(g, q->a, out);
        return;
    case IRQ_NEVER:
        sb_puts(out, "always (not ");
        sv_seq_operand(g, q->a, out);
        sb_putc(out, ')');
        return;
    case IRQ_EVENTUALLY:
        if (q->hi == IR_NONE) {
            sb_puts(out, "s_eventually ");
            sv_seq_operand(g, q->a, out);
        } else if (sv_seq_is_sequence(q->a)) {
            sb_printf(out, "##[0:%u] ", q->hi);
            sv_seq_operand(g, q->a, out);
        } else {
            sb_printf(out, "s_eventually [0:%u] ", q->hi);
            sv_seq_operand(g, q->a, out);
        }
        return;
    case IRQ_UNTIL:
        sv_seq_operand(g, q->a, out);
        sb_puts(out, " until ");
        sv_seq_operand(g, q->b, out);
        return;
    case IRQ_AND:
    case IRQ_OR:
        sv_seq_operand(g, q->a, out);
        sb_puts(out, q->kind == IRQ_AND ? " and " : " or ");
        sv_seq_operand(g, q->b, out);
        return;
    case IRQ_NOT:
        sb_puts(out, "not ");
        sv_seq_operand(g, q->a, out);
        return;
    }
}

static void sv_properties(SvGen *g, StrBuf *out)
{
    uint32_t i;
    StrBuf body = {0};
    if (!g->m->nprops || !g->c->o->emit_assertions)
        return;
    if (g->c->v2005) {
        for (i = 0; i < g->m->nprops; i++) {
            sb_printf(out, "    // property %s: not emitted (Verilog-2005 has no concurrent assertions)",
                      g->mn->prop[i]);
            sv_mark(g, g->m->props[i].loc, out);
            sb_putc(out, '\n');
        }
        return;
    }
    g->in_prop = 1;
    for (i = 0; i < g->m->nprops; i++) {
        const IrProperty *p = &g->m->props[i];
        const IrSeq *q = p->seq;
        SvDom dm;
        StrBuf ev = {0}, first = {0};
        const char *kw = p->kind == IRA_ASSUME ? "assume" : p->kind == IRA_COVER ? "cover" : "assert";
        if (!q || p->domain >= g->m->ndomains) {
            sv_error(g, p->loc, "property without a sequence or domain");
            continue;
        }
        dm = sv_dom(g, p->domain);
        if (q->kind == IRQ_ALWAYS) {
            sv_seq(g, q->a, &ev);
        } else if (q->kind == IRQ_NEVER) {
            sb_puts(&ev, "not ");
            sv_seq_operand(g, q->a, &ev);
        } else if (p->kind == IRA_COVER) {
            sv_seq(g, q, &ev);
        } else {
            /* checked once, at the first clock edge out of reset */
            const char *fn;
            StrBuf nb = {0};
            sb_printf(&nb, "%s_first", g->mn->prop[i]);
            fn = svn_fresh(g->mn, sb_cstr(&nb));
            sb_free(&nb);
            sb_printf(&first, "    logic %s = 1'b1;\n    always_ff @(%s %s) %s <= ", fn,
                      dm.falling ? "negedge" : "posedge", dm.clk, fn);
            if (dm.rst)
                sb_printf(&first, "%s%s;\n", dm.active_low ? "!" : "", dm.rst);
            else
                sb_puts(&first, "1'b0;\n");
            sb_printf(&ev, "%s |-> ", fn);
            sv_seq_operand(g, q, &ev);
        }
        sb_append(&body, g->pre.data ? g->pre.data : "", g->pre.len);
        sb_reset(&g->pre);
        sb_append(&body, first.data ? first.data : "", first.len);
        sb_printf(&body, "    %s: %s property (@(%s %s)", g->mn->prop[i], kw, dm.falling ? "negedge" : "posedge",
                  dm.clk);
        if (dm.rst)
            sb_printf(&body, " disable iff (%s%s)", dm.active_low ? "!" : "", dm.rst);
        sb_printf(&body, " %s);", sb_cstr(&ev));
        sv_mark(g, p->loc, &body);
        sb_putc(&body, '\n');
        sb_free(&ev);
        sb_free(&first);
    }
    g->in_prop = 0;
    sb_puts(out, "    // properties (define TERN_NO_SVA for tools without concurrent assertions)\n");
    sb_puts(out, "`ifndef SYNTHESIS\n`ifndef TERN_NO_SVA\n");
    sb_append(out, body.data ? body.data : "", body.len);
    sb_puts(out, "`endif\n`endif\n");
    sb_free(&body);
}

/* ------------------------------------------------------------- instances -- */

static int sv_simple_conn(const IrExpr *e)
{
    uint32_t lo;
    return e->kind == IRE_CONST || e->kind == IRE_SIG ||
           (e->kind == IRE_SLICE && sv_slice_base(e, &lo) != IR_NONE);
}

/* ` #(.P(v), ...)` of an extern module instance (nothing without generics) */
static void sv_extern_params(const SvModNames *cn, const IrModule *cm, StrBuf *head)
{
    uint32_t i;
    if (!cm->nparams)
        return;
    sb_puts(head, " #(");
    for (i = 0; i < cm->nparams; i++) {
        const IrParam *pp = &cm->params[i];
        if (i)
            sb_puts(head, ", ");
        sb_printf(head, ".%s(", cn->param[i]);
        if (pp->value.width <= 64 && !pp->is_signed)
            sb_printf(head, "%llu", (unsigned long long)ir_bits_low64(&pp->value));
        else if (pp->value.width <= 64)
            sb_printf(head, "%lld",
                      (long long)(pp->value.width == 64 ? ir_bits_low64(&pp->value)
                                                         : (ir_bits_get(&pp->value, pp->value.width - 1)
                                                                ? ir_bits_low64(&pp->value) |
                                                                      (~UINT64_C(0) << pp->value.width)
                                                                : ir_bits_low64(&pp->value))));
        else
            ir_bits_print(&pp->value, head);
        sb_putc(head, ')');
    }
    sb_putc(head, ')');
}

static void sv_instance(SvGen *g, uint32_t ii, StrBuf *out)
{
    const IrInstance *in = &g->m->insts[ii];
    const IrModule *cm = in->module;
    SvModNames *cn = svn_mod(g->c->n, cm);
    StrBuf head = {0};
    VEC(char *) names = {0};
    VEC(char *) vals = {0};
    size_t maxn = 0, k;
    uint32_t i, j;
    for (i = 0; i < cm->nports; i++) {
        uint32_t p = cm->ports[i];
        const IrConn *c = NULL;
        StrBuf v = {0};
        for (j = 0; j < in->nconns; j++)
            if (in->conns[j].port == p)
                c = &in->conns[j];
        if (p >= cm->nsigs)
            continue;
        if (c && c->expr) {
            const IrSignal *ps = &cm->sigs[p];
            const IrType *pty = !cm->is_extern ? ps->type : NULL;
            if (ps->kind == IRS_OUT) {
                sv_ex(g, c->expr, 1, &v);
            } else if (sv_simple_conn(c->expr)) {
                sv_rhs(g, c->expr, pty, &v);
            } else {
                StrBuf base = {0};
                sb_printf(&base, "%s_%s", g->mn->inst[ii], cn->sig[p]);
                sb_puts(&v, sv_temp_ty(g, c->expr, pty, sb_cstr(&base)));
                sb_free(&base);
            }
        }
        vec_push(names, xstrdup(cn->sig[p]));
        vec_push(vals, sb_take(&v));
        if (strlen(cn->sig[p]) > maxn)
            maxn = strlen(cn->sig[p]);
    }
    sb_append(out, g->pre.data ? g->pre.data : "", g->pre.len);
    sb_reset(&g->pre);
    {
        const SvPGroup *cg = svp_group_of(g->c->plan, cm);
        if (cg && cg->accepted) {
            /* an instance of a parameterized module */
            sb_printf(&head, "    %s #(", g->c->validating ? cg->name : cn->name);
            for (i = 0; i < cg->nparams && i < cm->origin->ngenerics; i++) {
                uint64_t xv;
                const IrGeneric *ge = &cm->origin->generics[i];
                if (i)
                    sb_puts(&head, ", ");
                sb_printf(&head, ".%s(", cg->pnames[i]);
                if (g->pv && map_get(&g->pv->slots, SVP_KEYI(in, i, SVK_INSTP), &xv))
                    svp_print((const SvXExpr *)(uintptr_t)xv, svp_pn(g), SVX_PREC_COND, &head);
                else if (cg->pbool[i])
                    sb_printf(&head, "1'b%d", ge->value ? 1 : 0);
                else
                    sb_printf(&head, "%lld", (long long)ge->value);
                sb_putc(&head, ')');
            }
            sb_putc(&head, ')');
        } else {
            sb_printf(&head, "    %s", cn->name);
        }
    }
    if (cm->is_extern)
        sv_extern_params(cn, cm, &head);
    sb_printf(&head, " %s (", g->mn->inst[ii]);
    sb_append(out, head.data, head.len);
    sv_mark(g, in->loc, out);
    sb_putc(out, '\n');
    for (k = 0; k < names.len; k++) {
        sb_printf(out, "        .%s", names.data[k]);
        sb_indent(out, (int)(maxn - strlen(names.data[k])));
        sb_printf(out, " (%s)%s\n", vals.data[k], k + 1 < names.len ? "," : "");
        free(names.data[k]);
        free(vals.data[k]);
    }
    sb_puts(out, "    );\n");
    vec_free(names);
    vec_free(vals);
    sb_free(&head);
}

/* --------------------------------------------------------- memory files -- */

static void sv_hex_word(const IrBits *b, uint32_t width, StrBuf *out)
{
    uint32_t nd = (width + 3) / 4, i;
    for (i = nd; i-- > 0;) {
        uint32_t bit = i * 4, k, nib = 0;
        for (k = 0; k < 4; k++)
            if (bit + k < width && bit + k < b->width && ir_bits_get(b, bit + k))
                nib |= 1u << k;
        sb_putc(out, "0123456789abcdef"[nib]);
    }
}

static void sv_mem_files(SvGen *g, uint32_t mem, const char *fname)
{
    const IrSignal *s = &g->m->sigs[mem];
    uint32_t w = s->type->width, i;
    StrBuf f = {0};
    char stem[512];
    size_t sl = strlen(fname);
    snprintf(stem, sizeof stem, "%.*s", (int)(sl > 4 ? sl - 4 : sl), fname);
    sb_printf(&f, "// %s.%s: %u words of %u bits ($readmemh)\n", g->mn->name, g->mn->sig[mem], s->depth, w);
    for (i = 0; i < s->depth; i++) {
        sv_hex_word(&s->mem_init[i], w, &f);
        sb_putc(&f, '\n');
    }
    sv_output_add(&g->c->memout, fname, SV_FILE_MEMINIT, &f);
    if (g->c->o->mem_init_formats & SV_MEMINIT_MIF) {
        char nm[600];
        sb_printf(&f, "-- %s.%s\nWIDTH=%u;\nDEPTH=%u;\nADDRESS_RADIX=HEX;\nDATA_RADIX=HEX;\nCONTENT BEGIN\n",
                  g->mn->name, g->mn->sig[mem], w, s->depth);
        for (i = 0; i < s->depth; i++) {
            sb_printf(&f, "    %x : ", i);
            sv_hex_word(&s->mem_init[i], w, &f);
            sb_puts(&f, ";\n");
        }
        sb_puts(&f, "END;\n");
        snprintf(nm, sizeof nm, "%s.mif", stem);
        sv_output_add(&g->c->memout, nm, SV_FILE_MEMINIT, &f);
    }
    if (g->c->o->mem_init_formats & SV_MEMINIT_COE) {
        char nm[600];
        sb_printf(&f, "; %s.%s\nmemory_initialization_radix=16;\nmemory_initialization_vector=\n",
                  g->mn->name, g->mn->sig[mem]);
        for (i = 0; i < s->depth; i++) {
            sv_hex_word(&s->mem_init[i], w, &f);
            sb_puts(&f, i + 1 < s->depth ? ",\n" : ";\n");
        }
        snprintf(nm, sizeof nm, "%s.coe", stem);
        sv_output_add(&g->c->memout, nm, SV_FILE_MEMINIT, &f);
    }
    sb_free(&f);
}

static void sv_mem_init(SvGen *g, uint32_t mem, StrBuf *out)
{
    const IrSignal *s = &g->m->sigs[mem];
    const char *name = g->mn->sig[mem];
    uint32_t i;
    if (s->mem_init) {
        if (!g->memw[mem] && s->depth <= 16) {
            sb_printf(out, "    initial begin");
            sv_mark(g, s->loc, out);
            sb_putc(out, '\n');
            for (i = 0; i < s->depth; i++) {
                sb_printf(out, "        %s[%u] = ", name, i);
                sv_lit(&s->mem_init[i], out);
                sb_puts(out, ";\n");
            }
            sb_puts(out, "    end\n");
        } else {
            StrBuf fn = {0};
            sb_printf(&fn, "%s_%s.hex", g->mn->name, name);
            sb_printf(out, "    initial $readmemh(\"%s\", %s);", sb_cstr(&fn), name);
            sv_mark(g, s->loc, out);
            sb_putc(out, '\n');
            sv_mem_files(g, mem, sb_cstr(&fn));
            sb_free(&fn);
        }
    } else if (s->mem_init_file) {
        if (!s->mem_init_is_bin)
            sb_printf(out, "    initial $readmemh(\"%s\", %s);\n", os_path_basename(s->mem_init_file), name);
        else
            sb_printf(out, "    // %s: initialized from binary file %s (contents not available)\n", name,
                      s->mem_init_file);
    }
}

/* ---------------------------------------------------------------- module -- */

static void sv_note_memw(const IrBlock *b, unsigned char *memw, uint32_t nsigs)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        if (s->kind == IRST_MEMWRITE && s->mem < nsigs)
            memw[s->mem] = 1;
        sv_note_memw(&s->then_b, memw, nsigs);
        sv_note_memw(&s->else_b, memw, nsigs);
        for (j = 0; j < s->narms; j++)
            sv_note_memw(&s->arms[j].body, memw, nsigs);
    }
}

static void sv_note_drv(const IrBlock *b, unsigned char *drv, uint32_t nsigs, unsigned char kind)
{
    Map set = {0};
    size_t it = 0;
    uint64_t k, v;
    sv_collect_targets(b, &set);
    while (map_next(&set, &it, &k, &v))
        if (k < nsigs)
            drv[k] = kind;
    map_free(&set);
}

/* attributes, net/variable keyword (Verilog-2005) and type of a declaration */
static void sv_decl_prefix(SvGen *g, uint32_t sig, StrBuf *out)
{
    const IrSignal *s = &g->m->sigs[sig];
    size_t l;
    sv_attrs(s->nattrs, s->attrs, out);
    if (g->c->v2005) {
        int isreg = g->drv[sig] == SVD_COMB || g->drv[sig] == SVD_CLOCKED || s->kind == IRS_MEM;
        sb_puts(out, isreg ? "reg " : "wire ");
    }
    l = out->len;
    sv_sig_type(g, sig, out);
    if (out->len == l && out->len && out->data[out->len - 1] == ' ')
        sb_truncate(out, out->len - 1);
}

static void sv_decl_group(SvGen *g, IrSigKind kind, const char *title, StrBuf *out)
{
    uint32_t i;
    size_t width = 0;
    int any = 0;
    StrBuf t = {0};
    for (i = 0; i < g->m->nsigs; i++) {
        if (g->m->sigs[i].kind != kind)
            continue;
        sb_reset(&t);
        sv_decl_prefix(g, i, &t);
        if (t.len > width)
            width = t.len;
        any = 1;
    }
    if (!any) {
        sb_free(&t);
        return;
    }
    sb_printf(out, "\n    // %s\n", title);
    for (i = 0; i < g->m->nsigs; i++) {
        const IrSignal *s = &g->m->sigs[i];
        if (s->kind != kind)
            continue;
        sb_reset(&t);
        sv_decl_prefix(g, i, &t);
        sv_ind(out, 1);
        sb_append(out, t.data, t.len);
        sb_indent(out, (int)(width - t.len));
        sb_printf(out, " %s", g->mn->sig[i]);
        if (s->kind == IRS_MEM) {
            const SvXExpr *dx = svp_get(g, s, SVK_DEPTH);
            if (dx) {
                sb_puts(out, " [0:");
                svp_print_m1(dx, svp_pn(g), out);
                sb_putc(out, ']');
            } else {
                sb_printf(out, " [0:%u]", s->depth - 1);
            }
        }
        if (s->kind == IRS_REG && s->has_init && map_has(&g->poweron, i)) {
            IrExpr k;
            memset(&k, 0, sizeof k);
            k.kind = IRE_CONST;
            k.width = s->type->width;
            k.bits = s->init;
            k.sig = IR_NONE;
            sb_puts(out, " = ");
            g->kwidth = svp_get(g, s, SVK_SIGW);
            sv_rhs(g, &k, s->type, out);
            g->kwidth = NULL;
        }
        sb_putc(out, ';');
        if (sv_has_attr(s, "unsafe_cross"))
            sb_puts(out, "  // CDC: unchecked");
        sv_mark(g, s->loc, out);
        sb_putc(out, '\n');
    }
    sb_free(&t);
}

static void sv_port_list(SvGen *g, StrBuf *out)
{
    const IrModule *m = g->m;
    uint32_t i;
    size_t tw = 0;
    StrBuf t = {0};
    for (i = 0; i < m->nports; i++) {
        uint32_t p = m->ports[i];
        sb_reset(&t);
        if (g->c->v2005) {
            int isreg = g->drv[p] == SVD_COMB || g->drv[p] == SVD_CLOCKED;
            sb_puts(&t, m->sigs[p].kind == IRS_OUT && isreg ? "reg" : "wire");
            if (sv_sig_has_range(g, p))
                sb_putc(&t, ' ');
        }
        sv_sig_type(g, p, &t);
        if (t.len > tw)
            tw = t.len;
    }
    for (i = 0; i < m->nports; i++) {
        uint32_t p = m->ports[i];
        const IrSignal *s = &m->sigs[p];
        sb_reset(&t);
        if (g->c->v2005) {
            int isreg = g->drv[p] == SVD_COMB || g->drv[p] == SVD_CLOCKED;
            sb_puts(&t, s->kind == IRS_OUT && isreg ? "reg" : "wire");
            if (sv_sig_has_range(g, p))
                sb_putc(&t, ' ');
        }
        sv_sig_type(g, p, &t);
        sb_puts(out, "    ");
        sv_attrs(s->nattrs, s->attrs, out);
        sb_puts(out, s->kind == IRS_IN ? "input  " : "output ");
        sb_append(out, t.data, t.len);
        sb_indent(out, (int)(tw - t.len));
        sb_printf(out, " %s%s", g->mn->sig[p], i + 1 < m->nports ? "," : "");
        sv_mark(g, s->loc, out);
        sb_putc(out, '\n');
    }
    sb_free(&t);
}

static void sv_param_decls(SvGen *g, StrBuf *out)
{
    uint32_t i;
    const IrModule *m = g->m;
    if (!m->nparams)
        return;
    sb_puts(out, "\n    // generic parameters (informational: the module is monomorphized)\n");
    for (i = 0; i < m->nparams; i++) {
        const IrParam *p = &m->params[i];
        uint32_t w = p->value.width;
        uint64_t v = ir_bits_low64(&p->value);
        int neg = p->is_signed && w && ir_bits_get(&p->value, w - 1);
        if (w <= 32 && !g->c->v2005) {
            if (neg)
                sb_printf(out, "    localparam int %s = -%llu;\n", g->mn->param[i],
                          (unsigned long long)(((~v) + 1u) & ((UINT64_C(1) << w) - 1u)));
            else
                sb_printf(out, "    localparam int%s %s = %llu;\n", p->is_signed ? "" : " unsigned",
                          g->mn->param[i], (unsigned long long)v);
        } else if (w <= 31 && !neg) {
            sb_printf(out, "    localparam %s = %llu;\n", g->mn->param[i], (unsigned long long)v);
        } else {
            sb_printf(out, "    localparam %s[%u:0] %s = ", g->c->v2005 ? "" : "logic ", w - 1, g->mn->param[i]);
            ir_bits_print(&p->value, out);
            sb_puts(out, ";\n");
        }
    }
}

static void sv_enum_localparams(SvGen *g, StrBuf *out)
{
    uint32_t i, k;
    const IrDesign *d = g->c->d;
    int any = 0;
    /* in package (design) order; tag types right after their enum */
    for (k = 0; k < d->ntypes; k++) {
        const IrType *ts[2];
        uint32_t nt = 0, j;
        ts[nt++] = d->types[k];
        if (svn_tagty(g->c->n, d->types[k]))
            ts[nt++] = svn_tagty(g->c->n, d->types[k]);
        for (j = 0; j < nt; j++) {
            const IrType *t = ts[j];
            size_t nw = 0;
            if (!svn_plain_enum(g->c->n, t) || !map_has(&g->types, MAP_PTR(t)))
                continue;
            if (!any)
                sb_puts(out, "\n    // enum constants\n");
            any = 1;
            for (i = 0; i < t->nvariants; i++) {
                const char *vn = svn_varname(g->c->n, &t->variants[i]);
                if (vn && strlen(vn) > nw)
                    nw = strlen(vn);
            }
            sb_printf(out, "    // %s", svn_tyname(g->c->n, t));
            sv_mark(g, t->loc, out);
            sb_putc(out, '\n');
            for (i = 0; i < t->nvariants; i++) {
                const char *vn = svn_varname(g->c->n, &t->variants[i]);
                if (!vn)
                    continue;
                if (t->width > 1)
                    sb_printf(out, "    localparam [%u:0] %s", t->width - 1, vn);
                else
                    sb_printf(out, "    localparam %s", vn);
                sb_indent(out, (int)(nw - strlen(vn)));
                sb_puts(out, " = ");
                sv_lit(&t->variants[i].tag, out);
                sb_puts(out, ";\n");
            }
        }
    }
}

/* declaration type of a temporary */
static void sv_temp_decl(SvGen *g, const SvTemp *tp, StrBuf *t)
{
    const SvXExpr *wx = tp->ty && sv_typed(g, tp->ty) ? NULL : svp_get(g, tp->expr, SVK_EXPRW);
    if (g->c->v2005)
        sb_puts(t, tp->local ? "reg" : "wire");
    if (g->c->v2005 && (tp->width > 1 || wx))
        sb_putc(t, ' ');
    if (wx) {
        if (!g->c->v2005)
            sb_puts(t, tp->ty && tp->ty->kind == IRT_SINT ? "logic signed " : "logic ");
        svp_range(g, wx, t);
    } else if (tp->ty && !g->c->v2005)
        sv_decl_type(g, tp->ty, t);
    else if (!g->c->v2005)
        sb_printf(t, tp->width > 1 ? "logic [%u:0]" : "logic", tp->width - 1);
    else if (tp->width > 1)
        sb_printf(t, "[%u:0]", tp->width - 1);
}

/* generic parameters of a parameterized module, and its localparams */
static void svp_header_params(SvGen *g, StrBuf *out)
{
    const SvPGroup *grp = g->pv->g;
    uint32_t i;
    sb_puts(out, "#(\n");
    for (i = 0; i < grp->nparams; i++) {
        sb_puts(out, "    parameter ");
        if (!g->c->v2005)
            sb_puts(out, grp->pbool[i] ? "bit " : "int unsigned ");
        if (grp->pbool[i])
            sb_printf(out, "%s = 1'b%d", grp->pnames[i], grp->pdefault[i] ? 1 : 0);
        else
            sb_printf(out, "%s = %lld", grp->pnames[i], (long long)grp->pdefault[i]);
        sb_puts(out, i + 1 < grp->nparams ? ",\n" : "\n");
    }
    sb_putc(out, ')');
}

static void svp_local_decls(SvGen *g, StrBuf *out)
{
    const SvPGroup *grp = g->pv->g;
    size_t i;
    if (grp->locals.len) {
        sb_puts(out, "\n    // module constants\n");
        for (i = 0; i < grp->locals.len; i++) {
            const SvXExpr *x = grp->locals.data[i];
            sb_printf(out, "    localparam %s%s = ", g->c->v2005 ? "" : "int unsigned ", x->name);
            svp_print(x->a, svp_pn(g), SVX_PREC_COND, out);
            sb_puts(out, ";\n");
        }
    }
}

static void svp_pconst_decls(SvGen *g, StrBuf *out)
{
    size_t i;
    if (!g->pconsts.len)
        return;
    sb_puts(out, "\n    // parameter-dependent constants\n");
    for (i = 0; i < g->pconsts.len; i++) {
        const SvPConst *q = &g->pconsts.data[i];
        sb_puts(out, "    localparam ");
        if (q->wx)
            svp_range(g, q->wx, out);
        else
            sb_printf(out, "[%u:0]", q->w - 1);
        sb_printf(out, " %s = ", q->name);
        if (q->vx)
            svp_print(q->vx, svp_pn(g), SVX_PREC_COND, out);
        else
            sb_printf(out, "%llu", (unsigned long long)q->v);
        sb_puts(out, ";\n");
    }
}

static void sv_module_ex(SvCtx *c, const IrModule *m, const SvPView *pv)
{
    SvGen g;
    StrBuf body = {0}, decl = {0}, file = {0}, fname = {0};
    uint32_t i, j;
    int any;
    const char *mname;
    memset(&g, 0, sizeof g);
    g.c = c;
    g.m = m;
    g.mn = svn_mod(c->n, m);
    g.pv = pv;
    g.xa = &c->scratch;
    mname = pv && c->validating ? pv->g->name : g.mn->name;
    g.drv = (unsigned char *)xcalloc(m->nsigs + 1, 1);
    g.memw = (unsigned char *)xcalloc(m->nsigs + 1, 1);

    /* types used here */
    for (i = 0; i < m->nsigs; i++)
        sv_use_type(&g, m->sigs[i].type);
    for (i = 0; i < m->ninsts; i++) {
        const IrModule *cm = m->insts[i].module;
        if (cm->is_extern)
            continue;
        for (j = 0; j < cm->nports; j++)
            if (cm->ports[j] < cm->nsigs)
                sv_use_type(&g, cm->sigs[cm->ports[j]].type);
    }
    for (i = 0; i < m->nassigns; i++)
        sv_use_expr(&g, m->assigns[i].rhs);
    for (i = 0; i < m->nprocs; i++)
        sv_use_block(&g, &m->procs[i].body);
    g.uses_pkg = !c->v2005 && map_len(&g.types) > 0;

    /* drivers */
    for (i = 0; i < m->nsigs; i++)
        if (m->sigs[i].kind == IRS_IN)
            g.drv[i] = SVD_IN;
    for (i = 0; i < m->nassigns; i++)
        if (m->assigns[i].lhs.sig < m->nsigs)
            g.drv[m->assigns[i].lhs.sig] = SVD_ASSIGN;
    for (i = 0; i < m->ninsts; i++)
        for (j = 0; j < m->insts[i].nconns; j++) {
            const IrConn *cn = &m->insts[i].conns[j];
            const IrModule *cm = m->insts[i].module;
            if (cn->expr && cn->port < cm->nsigs && cm->sigs[cn->port].kind == IRS_OUT &&
                cn->expr->kind == IRE_SIG && cn->expr->sig < m->nsigs)
                g.drv[cn->expr->sig] = SVD_INST;
        }
    for (i = 0; i < m->nprocs; i++) {
        sv_note_drv(&m->procs[i].body, g.drv, m->nsigs, m->procs[i].kind == IRP_COMB ? SVD_COMB : SVD_CLOCKED);
        sv_note_memw(&m->procs[i].body, g.memw, m->nsigs);
    }
    for (i = 0; i < m->nsigs; i++)
        if (m->sigs[i].kind == IRS_REG && g.drv[i] == SVD_NONE && m->sigs[i].has_init)
            g.drv[i] = SVD_ASSIGN;   /* never assigned: holds its value forever */

    /* ---- body ---- */
    any = 0;
    for (i = 0; i < m->nsigs; i++)
        if (m->sigs[i].kind == IRS_MEM && (m->sigs[i].mem_init || m->sigs[i].mem_init_file)) {
            if (!any)
                sb_puts(&body, "\n    // memory contents\n");
            any = 1;
            sv_mem_init(&g, i, &body);
        }
    any = 0;
    for (i = 0; i < m->nassigns; i++) {
        const IrAssign *a = &m->assigns[i];
        StrBuf lhs = {0}, rhs = {0}, guard = {0};
        const IrType *ty;
        if (!any)
            sb_puts(&body, "\n    // continuous assignments\n");
        any = 1;
        ty = sv_lvalue(&g, &a->lhs, &lhs, &guard);
        if (a->lhs.index)
            sv_error(&g, a->loc, "dynamic index in a continuous assignment target");
        sv_rhs(&g, a->rhs, ty, &rhs);
        sb_append(&body, g.pre.data ? g.pre.data : "", g.pre.len);
        sb_reset(&g.pre);
        sb_printf(&body, "    assign %s = %s;", sb_cstr(&lhs), sb_cstr(&rhs));
        if (a->lhs.sig < m->nsigs && sv_has_attr(&m->sigs[a->lhs.sig], "unsafe_cross"))
            sb_puts(&body, "  // CDC: unchecked");
        sv_mark(&g, a->loc, &body);
        sb_putc(&body, '\n');
        sb_free(&lhs);
        sb_free(&rhs);
        sb_free(&guard);
    }
    for (i = 0; i < m->nsigs; i++)
        if (m->sigs[i].kind == IRS_REG && g.drv[i] == SVD_ASSIGN && m->sigs[i].has_init) {
            int driven = 0;
            for (j = 0; j < m->nassigns; j++)
                if (m->assigns[j].lhs.sig == i)
                    driven = 1;
            if (driven)
                continue;
            if (!any)
                sb_puts(&body, "\n    // continuous assignments\n");
            any = 1;
            sb_printf(&body, "    assign %s = ", g.mn->sig[i]);
            {
                IrExpr k;
                memset(&k, 0, sizeof k);
                k.kind = IRE_CONST;
                k.width = m->sigs[i].type->width;
                k.bits = m->sigs[i].init;
                g.kwidth = svp_get(&g, &m->sigs[i], SVK_SIGW);
                sv_rhs(&g, &k, m->sigs[i].type, &body);
                g.kwidth = NULL;
            }
            sb_puts(&body, ";  // never assigned: keeps its initial value\n");
        }
    for (i = 0; i < m->ninsts; i++) {
        sb_putc(&body, '\n');
        sv_instance(&g, i, &body);
    }
    for (i = 0; i < m->nprocs; i++)
        if (m->procs[i].kind == IRP_COMB)
            sv_comb(&g, &m->procs[i], &body);
    for (i = 0; i < m->nprocs; i++)
        if (m->procs[i].kind == IRP_CLOCKED)
            sv_clocked(&g, &m->procs[i], &body);
    if (m->nprops && c->o->emit_assertions) {
        sb_putc(&body, '\n');
        sv_properties(&g, &body);
    }

    /* ---- declarations ---- */
    if (pv) {
        svp_local_decls(&g, &decl);
        svp_pconst_decls(&g, &decl);
    } else {
        sv_param_decls(&g, &decl);
    }
    if (c->v2005)
        sv_enum_localparams(&g, &decl);
    sv_decl_group(&g, IRS_WIRE, "wires", &decl);
    sv_decl_group(&g, IRS_VAR, "combinational variables", &decl);
    sv_decl_group(&g, IRS_REG, "registers", &decl);
    sv_decl_group(&g, IRS_MEM, "memories", &decl);
    if (g.temps.len) {
        size_t width = 0;
        StrBuf t = {0};
        for (i = 0; i < g.temps.len; i++) {
            const SvTemp *tp = &g.temps.data[i];
            sb_reset(&t);
            sv_temp_decl(&g, tp, &t);
            if (t.len > width)
                width = t.len;
        }
        sb_puts(&decl, "\n    // temporaries\n");
        for (i = 0; i < g.temps.len; i++) {
            const SvTemp *tp = &g.temps.data[i];
            sb_reset(&t);
            sv_temp_decl(&g, tp, &t);
            sb_puts(&decl, "    ");
            sb_append(&decl, t.data, t.len);
            sb_indent(&decl, (int)(width - t.len));
            sb_printf(&decl, " %s;\n", tp->name);
        }
        sb_free(&t);
    }

    /* ---- file ---- */
    sb_printf(&file, "// %s.%s: generated by tern from %s", mname, c->v2005 ? "v" : "sv",
              pv ? pv->g->display : m->display ? m->display : m->name);
    if (m->package)
        sb_printf(&file, " (%s)", m->package);
    sb_puts(&file, ". Do not edit.\n");
    if (m->nattrs) {
        StrBuf at = {0};
        sv_attrs(m->nattrs, m->attrs, &at);
        if (at.len) {
            sb_truncate(&at, at.len - 1);
            sb_append(&file, at.data, at.len);
            sb_putc(&file, '\n');
        }
        sb_free(&at);
    }
    sb_printf(&file, "module %s", mname);
    if (g.uses_pkg) {
        sv_mark(&g, m->loc, &file);
        sb_printf(&file, "\n    import %s::*;\n", c->pkg);
        if (pv) {
            svp_header_params(&g, &file);
            sb_putc(&file, '\n');
        }
        sb_putc(&file, '(');
    } else {
        if (pv) {
            sb_putc(&file, ' ');
            svp_header_params(&g, &file);
        }
        sb_puts(&file, " (");
        sv_mark(&g, m->loc, &file);
    }
    sb_putc(&file, '\n');
    sv_port_list(&g, &file);
    sb_puts(&file, ");\n");
    sb_append(&file, decl.data ? decl.data : "", decl.len);
    sb_append(&file, body.data ? body.data : "", body.len);
    sb_puts(&file, "\nendmodule\n");
    sb_printf(&fname, "%s.%s", mname, c->v2005 ? "v" : "sv");
    if (c->text)
        sb_append(c->text, file.data ? file.data : "", file.len);
    else
        sv_output_add(c->out, sb_cstr(&fname), SV_FILE_MODULE, &file);

    sb_free(&fname);
    sb_free(&file);
    sb_free(&decl);
    sb_free(&body);
    sb_free(&g.pre);
    vec_free(g.temps);
    vec_free(g.locals);
    vec_free(g.pconsts);
    map_free(&g.types);
    map_free(&g.defassigned);
    map_free(&g.poweron);
    free(g.drv);
    free(g.memw);
}

/* --------------------------------------------------------------- package -- */

static void sv_package(SvCtx *c, StrBuf *out)
{
    const IrDesign *d = c->d;
    SvGen g;
    uint32_t i, k;
    memset(&g, 0, sizeof g);
    g.c = c;
    g.m = d->top;
    for (k = 0; k < d->ntypes; k++)
        sv_use_type(&g, d->types[k]);
    sb_printf(out, "// %s.sv: named types of the design, generated by tern. Do not edit.\n", c->pkg);
    sb_printf(out, "package %s;\n", c->pkg);
    for (k = 0; k < d->ntypes; k++) {
        const IrType *t = d->types[k];
        const char *tn = svn_tyname(c->n, t);
        if (!tn)
            continue;
        sb_putc(out, '\n');
        if (t->kind == IRT_ENUM) {
            const IrType *tt = svn_tagty(c->n, t) ? svn_tagty(c->n, t) : t;
            size_t nw = 0;
            for (i = 0; i < tt->nvariants; i++) {
                const char *vn = svn_varname(c->n, &tt->variants[i]);
                if (vn && strlen(vn) > nw)
                    nw = strlen(vn);
            }
            if (tt->width > 1)
                sb_printf(out, "    typedef enum logic [%u:0] {", tt->width - 1);
            else
                sb_puts(out, "    typedef enum logic {");
            sv_mark(&g, t->loc, out);
            sb_putc(out, '\n');
            for (i = 0; i < tt->nvariants; i++) {
                const char *vn = svn_varname(c->n, &tt->variants[i]);
                sb_printf(out, "        %s", vn ? vn : "?");
                sb_indent(out, (int)(nw - strlen(vn ? vn : "?")));
                sb_puts(out, " = ");
                sv_lit(&tt->variants[i].tag, out);
                sb_puts(out, i + 1 < tt->nvariants ? ",\n" : "\n");
            }
            sb_printf(out, "    } %s;\n", svn_tyname(c->n, tt));
            if (tt != t) {
                sb_printf(out, "\n    typedef struct packed {  // %s: tag ++ payload\n", t->name);
                {
                    char pt[48];
                    const char *tg = svn_tyname(c->n, tt);
                    int w1 = (int)strlen(tg), w2;
                    snprintf(pt, sizeof pt, "logic [%u:0]", t->width - t->tag_width - 1);
                    w2 = (int)strlen(pt);
                    sb_printf(out, "        %-*s tag;\n", w1 > w2 ? w1 : w2, tg);
                    sb_printf(out, "        %-*s payload;\n", w1 > w2 ? w1 : w2, pt);
                }
                sb_printf(out, "    } %s;\n", tn);
            }
        } else if (t->kind == IRT_STRUCT) {
            size_t tw = 0;
            StrBuf ft = {0};
            sb_puts(out, "    typedef struct packed {");
            sv_mark(&g, t->loc, out);
            sb_putc(out, '\n');
            for (i = 0; i < t->nfields; i++) {
                sb_reset(&ft);
                sv_decl_type(&g, t->fields[i].type, &ft);
                if (ft.len > tw)
                    tw = ft.len;
            }
            for (i = 0; i < t->nfields; i++) {
                sb_reset(&ft);
                sv_decl_type(&g, t->fields[i].type, &ft);
                sb_puts(out, "        ");
                sb_append(out, ft.data, ft.len);
                sb_indent(out, (int)(tw - ft.len));
                sb_printf(out, " %s;\n", svn_fieldname(c->n, &t->fields[i]));
            }
            sb_printf(out, "    } %s;\n", tn);
            sb_free(&ft);
        }
    }
    sb_puts(out, "\nendpackage\n");
    map_free(&g.types);
}

/* ---------------------------------------------------------------- design -- */

int sv_emit_design(const IrDesign *d, const SvOptions *o, SvOutput *out)
{
    SvCtx c;
    SvOptions def;
    uint32_t i;
    size_t k;
    if (!o) {
        sv_options_init(&def);
        o = &def;
    }
    memset(&c, 0, sizeof c);
    c.d = d;
    c.o = o;
    c.out = out;
    c.v2005 = o->std == SV_STD_V2005;
    c.n = sv_names_new(d);
    c.pkg = o->types_pkg ? o->types_pkg : sv_names_types_pkg(c.n);
    if (!c.v2005 && d->ntypes) {
        StrBuf pkg = {0}, fn = {0};
        int any = 0;
        for (i = 0; i < d->ntypes; i++)
            if (svn_tyname(c.n, d->types[i]))
                any = 1;
        if (any) {
            sv_package(&c, &pkg);
            sb_printf(&fn, "%s.sv", c.pkg);
            sv_output_add(out, sb_cstr(&fn), SV_FILE_PACKAGE, &pkg);
        }
        sb_free(&pkg);
        sb_free(&fn);
    }
    c.plan = d->sv_params;
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        uint64_t pv = 0;
        const SvPView *view = NULL;
        if (m->is_extern) {
            if (m->extern_file)
                sv_output_add_extern(out, m->extern_file);
            continue;
        }
        if (c.plan && map_get(&c.plan->views, MAP_PTR(m), &pv)) {
            view = (const SvPView *)(uintptr_t)pv;
            if (view->g->rep != m)
                continue;   /* emitted once, as the group's module */
        }
        sv_module_ex(&c, m, view);
    }
    for (k = 0; k < c.memout.nfiles; k++) {
        StrBuf t = {0};
        sb_append(&t, c.memout.files[k].data, c.memout.files[k].len);
        sv_output_add(out, c.memout.files[k].name, c.memout.files[k].kind, &t);
    }
    sv_output_free(&c.memout);
    sv_names_free(c.n);
    arena_release(&c.scratch);
    return c.errors;
}

/* ==================================================== parameter plan == */
/* sv_params_plan (emit.h): which generic modules become parameterized SV
 * modules.  The IR comparison and the expressions are in svparam.c; here
 * each candidate group is emitted once per elaboration and the texts are
 * compared. */

static int svp_eligible(const IrModule *m, const void *item, uint32_t ngen)
{
    uint32_t i;
    if (!m || !m->origin || m->origin->item != item || m->is_extern || m->is_cdc || !m->origin->ngenerics ||
        m->origin->ngenerics > 32 || (ngen && m->origin->ngenerics != ngen))
        return 0;
    for (i = 0; i < m->origin->ngenerics; i++) {
        const IrGeneric *g = &m->origin->generics[i];
        if (g->kind == IRG_BOOL)
            continue;
        if (g->kind != IRG_UINT || !g->fits || g->value < 0 || g->value >= (INT64_C(1) << 31))
            return 0;
    }
    return 1;
}

static SvXExpr *svp_copy(Arena *a, const SvXExpr *x)
{
    SvXExpr *r;
    if (!x)
        return NULL;
    r = ANEW(a, SvXExpr);
    *r = *x;
    r->a = svp_copy(a, x->a);
    r->b = svp_copy(a, x->b);
    r->c = svp_copy(a, x->c);
    return r;
}

/* one module's text, emitted for comparison */
static int svp_text(const IrDesign *d, SvNames *n, const struct SvParamPlan *plan, const IrModule *m,
                    const SvPView *pv, const SvOptions *o, StrBuf *out)
{
    SvCtx c;
    SvOutput dummy;
    memset(&c, 0, sizeof c);
    memset(&dummy, 0, sizeof dummy);
    c.d = d;
    c.o = o;
    c.n = n;
    c.out = &dummy;
    c.v2005 = o->std == SV_STD_V2005;
    c.pkg = "tern_types_pkg";
    c.plan = plan;
    c.validating = 1;
    c.text = out;
    sv_module_ex(&c, m, pv);
    sv_output_free(&c.memout);
    sv_output_free(&dummy);
    arena_release(&c.scratch);
    return c.errors;
}

static void svp_name_pair(Map *names, const char *from, const char *to)
{
    if (from && to && from != to)
        map_put(names, MAP_PTR(from), MAP_PTR(to));
}

/* names of elaboration k's named types -> the same types of elaboration 0 */
static void svp_type_names(SvNames *n0, SvNames *nk, const SvPWalk *w, uint32_t k, Map *names)
{
    size_t i;
    uint32_t j;
    for (i = 0; i < w->types.len; i++) {
        const SvPTypePair *p = &w->types.data[i];
        const IrType *t0 = p->t0, *tk = p->tk, *g0, *gk;
        if (p->k != k)
            continue;
        svp_name_pair(names, svn_tyname(nk, tk), svn_tyname(n0, t0));
        if (t0->kind != IRT_ENUM || t0->nvariants != tk->nvariants)
            continue;
        for (j = 0; j < t0->nvariants; j++)
            svp_name_pair(names, svn_varname(nk, &tk->variants[j]), svn_varname(n0, &t0->variants[j]));
        g0 = svn_tagty(n0, t0);
        gk = svn_tagty(nk, tk);
        if (g0 && gk) {
            svp_name_pair(names, svn_tyname(nk, gk), svn_tyname(n0, g0));
            for (j = 0; j < g0->nvariants && j < gk->nvariants; j++)
                svp_name_pair(names, svn_varname(nk, &gk->variants[j]), svn_varname(n0, &g0->variants[j]));
        }
    }
}

/* text with every identifier in `names` replaced and runs of spaces
 * collapsed (column alignment depends on the lengths of type names) */
static void svp_subst(const StrBuf *in, const Map *names, StrBuf *out)
{
    size_t i = 0, n = in->len;
    const char *s = in->data ? in->data : "";
    while (i < n) {
        if (s[i] == ' ') {
            sb_putc(out, ' ');
            while (i < n && s[i] == ' ')
                i++;
            continue;
        }
        if (tern_is_alpha((unsigned char)s[i]) || s[i] == '_') {
            size_t j = i;
            const char *id;
            uint64_t v;
            while (j < n && (tern_is_ident((unsigned char)s[j]) || s[j] == '$'))
                j++;
            id = intern(s + i, j - i);
            if (map_get(names, MAP_PTR(id), &v))
                sb_puts(out, (const char *)(uintptr_t)v);
            else
                sb_append(out, s + i, j - i);
            i = j;
        } else if (tern_is_digit((unsigned char)s[i]) || s[i] == '$') {
            size_t j = i;
            while (j < n && (tern_is_ident((unsigned char)s[j]) || s[j] == '$' || s[j] == '\''))
                j++;
            sb_append(out, s + i, j - i);
            i = j;
        } else {
            sb_putc(out, s[i++]);
        }
    }
}

/* emits every elaboration under every option combination; 1 if all texts agree */
static int svp_validate(const struct SvParamPlan *plan, const SvPWalk *w, const IrDesign *const *dk,
                        SvNames *const *nk, SvPView *const *views, const SvOptions *base)
{
    uint32_t combo, k;
    int ok = 1;
    for (combo = 0; combo < 8 && ok; combo++) {
        SvOptions o = *base;
        StrBuf t0 = {0}, tk0 = {0};
        Map none = {0};
        o.std = (combo & 1) ? SV_STD_V2005 : SV_STD_SV2012;
        o.emit_assertions = (combo & 2) ? 1 : 0;
        o.target_kind = (combo & 4) ? SV_TARGET_ASIC : SV_TARGET_FPGA;
        if (svp_text(dk[0], nk[0], plan, w->mods[0], views[0], &o, &tk0))
            ok = 0;
        svp_subst(&tk0, &none, &t0);
        for (k = 1; k < w->n && ok; k++) {
            StrBuf tk = {0}, ts = {0};
            Map names = {0};
            if (svp_text(dk[k], nk[k], plan, w->mods[k], views[k], &o, &tk))
                ok = 0;
            svp_type_names(nk[0], nk[k], w, k, &names);
            svp_subst(&tk, &names, &ts);
            if (ts.len != t0.len || (ts.len && memcmp(ts.data, t0.data, ts.len) != 0))
                ok = 0;
            map_free(&names);
            sb_free(&tk);
            sb_free(&ts);
        }
        sb_free(&t0);
        sb_free(&tk0);
    }
    return ok;
}

static void svp_view_free(SvPView *v)
{
    if (!v)
        return;
    map_free(&v->slots);
    free(v);
}

/* tries one group; 1 when accepted (the members' views are added to the plan) */
static int svp_try_group(struct SvParamPlan *plan, SvPGroup *grp, const IrDesign *d, const SvParamItem *it,
                         const SvOptions *o)
{
    VEC(const IrModule *) mods = {0};
    VEC(const IrDesign *) designs = {0};
    VEC(SvNames *) owned = {0};
    SvNames **nk = NULL;
    SvPView **views = NULL;
    int64_t **pvals = NULL;
    const SvXExpr **base = NULL;
    SvPWalk w;
    SvPCands cands;
    uint32_t i, k, nmembers, nbase = 0;
    int ok = 1;
    SvNames *nmain = sv_names_new(d);
    memset(&w, 0, sizeof w);
    memset(&cands, 0, sizeof cands);
    /* elaborations: the design's members (children first), then the probes */
    for (i = 0; i < d->nmodules; i++)
        if (svp_eligible(d->modules[i], grp->item, grp->nparams)) {
            vec_push(mods, d->modules[i]);
            vec_push(designs, d);
        }
    nmembers = (uint32_t)mods.len;
    for (i = 0; i < it->nprobes; i++) {
        const IrDesign *pd = it->probes[i];
        if (pd && svp_eligible(pd->top, grp->item, grp->nparams)) {
            vec_push(mods, pd->top);
            vec_push(designs, pd);
        }
    }
    if (!nmembers || mods.len < 2)
        ok = 0;
    /* every generic takes two values */
    for (i = 0; ok && i < grp->nparams; i++) {
        int two = 0;
        for (k = 1; k < mods.len; k++)
            if (mods.data[k]->origin->generics[i].value != mods.data[0]->origin->generics[i].value)
                two = 1;
        if (!two)
            ok = 0;
    }
    if (ok) {
        w.n = (uint32_t)mods.len;
        w.mods = mods.data;
        w.plan = plan;
        ok = svp_walk(&w);
    }
    if (ok) {
        pvals = (int64_t **)xcalloc(mods.len, sizeof(int64_t *));
        for (k = 0; k < mods.len; k++) {
            pvals[k] = (int64_t *)xcalloc(grp->nparams + 1, sizeof(int64_t));
            for (i = 0; i < grp->nparams; i++)
                pvals[k][i] = mods.data[k]->origin->generics[i].value;
        }
        base = (const SvXExpr **)xcalloc(grp->nparams + it->ncands + 1, sizeof(SvXExpr *));
        for (i = 0; i < grp->nparams; i++) {
            SvXExpr *x = ANEW(&plan->arena, SvXExpr);
            x->op = SVX_PARAM;
            x->param = i;
            base[nbase++] = x;
        }
        for (i = 0; i < it->ncands; i++)
            base[nbase++] = svp_copy(&plan->arena, it->cands[i]);
        svp_cands_init(&cands, base, nbase, (const int64_t *const *)pvals, (uint32_t)mods.len, &plan->arena);
        views = (SvPView **)xcalloc(mods.len, sizeof(SvPView *));
        for (k = 0; k < mods.len; k++) {
            views[k] = (SvPView *)xcalloc(1, sizeof(SvPView));
            views[k]->g = grp;
        }
        for (i = 0; ok && i < w.slots.len; i++) {
            const SvPSlot *sl = &w.slots.data[i];
            const int64_t *v = &w.vals.data[sl->at];
            const SvXExpr *x;
            for (k = 1; k < w.n; k++)
                if (v[k] != v[0])
                    break;
            if (k == w.n)
                continue;
            x = svp_match(&cands, v, &plan->arena);
            if (!x) {
                ok = 0;
                break;
            }
            for (k = 0; k < w.n; k++) {
                uint64_t old;
                uint64_t key = w.keys.data[sl->at + k];
                if (map_get(&views[k]->slots, key, &old) && old != MAP_PTR(x)) {
                    ok = 0;
                    break;
                }
                map_put(&views[k]->slots, key, MAP_PTR(x));
            }
        }
    }
    if (ok) {
        nk = (SvNames **)xcalloc(mods.len, sizeof(SvNames *));
        for (k = 0; k < mods.len; k++) {
            if (designs.data[k] == d) {
                nk[k] = nmain;
            } else {
                nk[k] = sv_names_new(designs.data[k]);
                vec_push(owned, nk[k]);
            }
        }
        grp->rep = mods.data[0];
        grp->pdefault = ANEWN(&plan->arena, int64_t, grp->nparams ? grp->nparams : 1);
        for (i = 0; i < grp->nparams; i++)
            grp->pdefault[i] = mods.data[0]->origin->generics[i].value;
        ok = svp_validate(plan, &w, designs.data, nk, views, o);
    }
    if (ok) {
        for (k = 0; k < nmembers; k++) {
            size_t mi = 0;
            uint64_t key, val;
            map_put(&plan->views, MAP_PTR(mods.data[k]), MAP_PTR(views[k]));
            vec_push(plan->allviews, views[k]);
            while (map_next(&views[k]->slots, &mi, &key, &val))
                svp_collect_locals((const SvXExpr *)(uintptr_t)val, grp);
            views[k] = NULL;
        }
        grp->accepted = 1;
    }
    for (k = 0; views && k < mods.len; k++)
        svp_view_free(views[k]);
    for (k = 0; pvals && k < mods.len; k++)
        free(pvals[k]);
    for (i = 0; i < owned.len; i++)
        sv_names_free(owned.data[i]);
    sv_names_free(nmain);
    vec_free(owned);
    free(nk);
    free(views);
    free(pvals);
    free(base);
    svp_cands_free(&cands);
    svp_walk_free(&w);
    vec_free(mods);
    vec_free(designs);
    return ok;
}

int sv_params_plan(IrDesign *d, const SvParamInput *in, const SvOptions *o)
{
    struct SvParamPlan *plan;
    VEC(SvPGroup *) order = {0};
    VEC(const SvParamItem *) items = {0};
    SvOptions def;
    uint32_t i, j;
    size_t gi;
    int accepted = 0;
    sv_params_free(d);
    if (!in || !in->n || !d->top)
        return 0;
    if (!o) {
        sv_options_init(&def);
        o = &def;
    }
    plan = (struct SvParamPlan *)xcalloc(1, sizeof *plan);
    /* groups, in the order of their first member (children first) */
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        const SvParamItem *it = NULL;
        SvPGroup *grp;
        StrBuf disp = {0};
        size_t k;
        if (!m->origin || map_has(&plan->groups, MAP_PTR(m->origin->item)))
            continue;
        for (k = 0; k < in->n; k++)
            if (in->items[k].item == m->origin->item)
                it = &in->items[k];
        if (!it || !svp_eligible(m, m->origin->item, 0))
            continue;
        grp = ANEW(&plan->arena, SvPGroup);
        grp->item = m->origin->item;
        grp->name = svn_sanitize(m->tern_name ? m->tern_name : m->name);
        grp->nparams = m->origin->ngenerics;
        grp->pnames = ANEWN(&plan->arena, const char *, grp->nparams);
        grp->pbool = ANEWN(&plan->arena, unsigned char, grp->nparams);
        sb_printf(&disp, "%s<", m->tern_name ? m->tern_name : m->name);
        for (j = 0; j < grp->nparams; j++) {
            grp->pnames[j] = svn_sanitize(m->origin->generics[j].name);
            grp->pbool[j] = m->origin->generics[j].kind == IRG_BOOL;
            sb_printf(&disp, "%s%s", j ? ", " : "", grp->pnames[j]);
        }
        sb_putc(&disp, '>');
        grp->display = arena_strdup(&plan->arena, sb_cstr(&disp));
        sb_free(&disp);
        map_put(&plan->groups, MAP_PTR(grp->item), MAP_PTR(grp));
        vec_push(order, grp);
        vec_push(items, it);
    }
    for (gi = 0; gi < order.len; gi++)
        if (svp_try_group(plan, order.data[gi], d, items.data[gi], o))
            accepted++;
    vec_free(order);
    vec_free(items);
    d->sv_params = plan;
    return accepted;
}

void sv_params_free(IrDesign *d)
{
    struct SvParamPlan *plan = d ? d->sv_params : NULL;
    size_t i = 0;
    uint64_t k, v;
    if (!plan)
        return;
    while (map_next(&plan->groups, &i, &k, &v))
        vec_free(((SvPGroup *)(uintptr_t)v)->locals);
    for (i = 0; i < plan->allviews.len; i++)
        svp_view_free(plan->allviews.data[i]);
    vec_free(plan->allviews);
    map_free(&plan->groups);
    map_free(&plan->views);
    arena_release(&plan->arena);
    free(plan);
    d->sv_params = NULL;
}

/* ============================================================ wrapper == */

void sv_emit_wrapper(const IrDesign *d, const SvOptions *o, const char *name, StrBuf *out)
{
    SvCtx c;
    SvGen g;
    SvOptions def;
    const IrModule *m = d->top;
    uint32_t i;
    size_t tw = 0, nw = 0;
    StrBuf t = {0};
    if (!o) {
        sv_options_init(&def);
        o = &def;
    }
    memset(&c, 0, sizeof c);
    memset(&g, 0, sizeof g);
    c.d = d;
    c.o = o;
    c.n = sv_names_new(d);
    c.pkg = o->types_pkg ? o->types_pkg : sv_names_types_pkg(c.n);
    c.v2005 = o->std == SV_STD_V2005;
    g.c = &c;
    g.m = m;
    g.mn = svn_mod(c.n, m);
    for (i = 0; i < m->nports; i++)
        sv_use_type(&g, m->sigs[m->ports[i]].type);
    sb_printf(out, "// %s.sv: test wrapper of %s, generated by tern. Do not edit.\n", name, g.mn->name);
    sb_puts(out, "// The inputs are driven through VPI.\n// verilator lint_off UNDRIVEN\n");
    sb_printf(out, "module %s", name);
    if (!c.v2005 && map_len(&g.types))
        sb_printf(out, "\n    import %s::*;\n", c.pkg);
    sb_puts(out, ";\n");
    for (i = 0; i < m->nports; i++) {
        sb_reset(&t);
        sv_decl_type(&g, m->sigs[m->ports[i]].type, &t);
        while (t.len && t.data[t.len - 1] == ' ')
            sb_truncate(&t, t.len - 1);
        if (t.len > tw)
            tw = t.len;
        if (strlen(g.mn->sig[m->ports[i]]) > nw)
            nw = strlen(g.mn->sig[m->ports[i]]);
    }
    for (i = 0; i < m->nports; i++) {
        uint32_t p = m->ports[i];
        sb_reset(&t);
        if (c.v2005)
            sb_puts(&t, m->sigs[p].kind == IRS_IN ? "reg " : "wire ");
        sv_decl_type(&g, m->sigs[p].type, &t);
        while (t.len && t.data[t.len - 1] == ' ')
            sb_truncate(&t, t.len - 1);
        sb_puts(out, "    ");
        sb_append(out, t.data ? t.data : "", t.len);
        sb_indent(out, (int)(tw + (c.v2005 ? 5 : 0) - t.len));
        sb_printf(out, " %s;\n", g.mn->sig[p]);
    }
    sb_printf(out, "\n    %s", g.mn->name);
    if (m->is_extern)
        sv_extern_params(g.mn, m, out);
    sb_puts(out, " dut (\n");
    for (i = 0; i < m->nports; i++) {
        const char *pn = g.mn->sig[m->ports[i]];
        sb_printf(out, "        .%s", pn);
        sb_indent(out, (int)(nw - strlen(pn)));
        sb_printf(out, " (%s)%s\n", pn, i + 1 < m->nports ? "," : "");
    }
    sb_puts(out, "    );\n\nendmodule\n");
    sb_free(&t);
    map_free(&g.types);
    sv_names_free(c.n);
    arena_release(&c.scratch);
}
