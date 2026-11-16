/* import.c — `tern import`: SystemVerilog / Verilog-2005 module headers to
 * Tern `extern module` declarations (spec §13.4, §13.5; docs/migration.md).
 *
 * This is a header parser, not a SystemVerilog front end: it lexes the file
 * (comments, attributes `(* *)` and compiler directives are skipped, the
 * latter with a warning), finds every `module`, and reads its parameter
 * list and its ports, both ANSI (`input logic [7:0] a, b`) and non-ANSI
 * (names in the header, `input`/`output` declarations in the body). Module
 * bodies are otherwise skipped. Everything Tern cannot express directly
 * (interfaces, unpacked array ports, user-defined types, string/real
 * parameters) is emitted as a commented-out line with a TODO, and clock /
 * reset types guessed from port names carry a "TODO: confirm" comment. */
#include "../base/common.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/diag.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../cmds.h"

/* ------------------------------------------------------------- lexer --- */

typedef enum ImpTokKind { IMPT_EOF, IMPT_ID, IMPT_NUM, IMPT_STR, IMPT_OP, IMPT_MACRO } ImpTokKind;

typedef struct ImpTok {
    ImpTokKind k;
    uint32_t off, len;
} ImpTok;

typedef VEC(ImpTok) ImpTokVec;

typedef struct ImpLexer {
    uint32_t file;
    const char *s;
    size_t len, p;
    ImpTokVec toks;
    char warned[24][24];      /* directive names already warned about */
    int nwarned;
    int errors;
} ImpLexer;

static const char *const imp_ops[] = {
    "<<<=", ">>>=", "<<<", ">>>", "===", "!==", "==?", "!=?", "<<=", ">>=", "**", "::", "==", "!=", "<=",
    ">=", "&&", "||", "<<", ">>", "+:", "-:", "->", "++", "--", "+=", "-=", "##", ".*", "'{",
};

static int imp_id_start(int c)
{
    return tern_is_alpha(c) || c == '_';
}

static int imp_id_char(int c)
{
    return tern_is_ident(c) || c == '$';
}

static void imp_push(ImpLexer *L, ImpTokKind k, size_t start)
{
    ImpTok t;
    t.k = k;
    t.off = (uint32_t)start;
    t.len = (uint32_t)(L->p - start);
    vec_push(L->toks, t);
}

static void imp_warn_directive(ImpLexer *L, size_t start, const char *name, size_t nlen, const char *shown)
{
    int i;
    char nm[24];
    if (nlen >= sizeof nm)
        nlen = sizeof nm - 1;
    memcpy(nm, name, nlen);
    nm[nlen] = 0;
    for (i = 0; i < L->nwarned; i++) {
        if (strcmp(L->warned[i], nm) == 0)
            return;
    }
    if (L->nwarned < (int)TERN_ARRAY_LEN(L->warned))
        memcpy(L->warned[L->nwarned++], nm, nlen + 1);
    {
        Span sp = span_make(L->file, (uint32_t)start, (uint32_t)(strlen(shown) + 1));
        if (strcmp(nm, "timescale") == 0)
            diag_warning(sp, "`timescale ignored: Tern designs have no time scale");
        else if (strcmp(nm, "define") == 0)
            diag_warning(sp, "macro definitions (`define) are ignored; macro uses are kept with a TODO");
        else if (strcmp(nm, "include") == 0)
            diag_warning(sp, "`include is not followed; import the included file separately if it declares modules");
        else if (strcmp(nm, "ifdef") == 0 || strcmp(nm, "ifndef") == 0 || strcmp(nm, "elsif") == 0 ||
                 strcmp(nm, "else") == 0)
            diag_warning(sp, "conditional compilation (`%s) is not evaluated: declarations of every branch are imported",
                         shown);
        else
            diag_warning(sp, "compiler directive `%s ignored", nm);
        if (strcmp(nm, "ifdef") == 0 || strcmp(nm, "ifndef") == 0)
            diag_help("Tern has no preprocessor: use a bool generic or a [features] flag (cfg::name)");
    }
}

static void imp_skip_to_eol(ImpLexer *L, int continuation)
{
    while (L->p < L->len && L->s[L->p] != '\n') {
        if (continuation && L->s[L->p] == '\\' && L->p + 1 < L->len &&
            (L->s[L->p + 1] == '\n' || (L->s[L->p + 1] == '\r' && L->p + 2 < L->len && L->s[L->p + 2] == '\n'))) {
            L->p += L->s[L->p + 1] == '\r' ? 3 : 2;
            continue;
        }
        L->p++;
    }
}

static void imp_lex(ImpLexer *L)
{
    while (L->p < L->len) {
        int c = (unsigned char)L->s[L->p];
        size_t start = L->p;
        if (tern_is_space(c)) {
            L->p++;
            continue;
        }
        if (c == '/' && L->p + 1 < L->len && L->s[L->p + 1] == '/') {
            imp_skip_to_eol(L, 0);
            continue;
        }
        if (c == '/' && L->p + 1 < L->len && L->s[L->p + 1] == '*') {
            const char *e = NULL;
            size_t q;
            for (q = L->p + 2; q + 1 < L->len; q++) {
                if (L->s[q] == '*' && L->s[q + 1] == '/') {
                    e = L->s + q;
                    break;
                }
            }
            if (!e) {
                diag_error(span_make(L->file, (uint32_t)start, 2), "unterminated /* comment");
                L->errors++;
                L->p = L->len;
                break;
            }
            L->p = (size_t)(e - L->s) + 2;
            continue;
        }
        if (c == '(' && L->p + 1 < L->len && L->s[L->p + 1] == '*') {
            /* attribute (* ... *), but not @(*) */
            size_t q = L->p + 2;
            while (q < L->len && (L->s[q] == ' ' || L->s[q] == '\t'))
                q++;
            if (q < L->len && L->s[q] != ')') {
                for (q = L->p + 2; q + 1 < L->len; q++) {
                    if (L->s[q] == '*' && L->s[q + 1] == ')')
                        break;
                }
                L->p = q + 2 <= L->len ? q + 2 : L->len;
                continue;
            }
        }
        if (c == '`') {
            static const char *const dirs[] = {
                "timescale", "define", "undef", "undefineall", "include", "ifdef", "ifndef", "elsif", "else",
                "endif", "default_nettype", "resetall", "celldefine", "endcelldefine", "line", "pragma",
                "begin_keywords", "end_keywords", "unconnected_drive", "nounconnected_drive", "__FILE__", "__LINE__",
            };
            size_t q = L->p + 1, i, nlen;
            while (q < L->len && imp_id_char((unsigned char)L->s[q]))
                q++;
            nlen = q - (L->p + 1);
            for (i = 0; i < TERN_ARRAY_LEN(dirs); i++) {
                if (strlen(dirs[i]) == nlen && memcmp(dirs[i], L->s + L->p + 1, nlen) == 0)
                    break;
            }
            if (i < TERN_ARRAY_LEN(dirs) && i < 20) {
                const char *nm = dirs[i];
                int cond = strcmp(nm, "ifdef") == 0 || strcmp(nm, "ifndef") == 0 || strcmp(nm, "elsif") == 0 ||
                           strcmp(nm, "else") == 0;
                if (cond)
                    imp_warn_directive(L, start, "ifdef", 5, nm);
                else if (strcmp(nm, "endif") != 0 && strcmp(nm, "celldefine") != 0 &&
                         strcmp(nm, "endcelldefine") != 0 && strcmp(nm, "resetall") != 0 &&
                         strcmp(nm, "end_keywords") != 0 && strcmp(nm, "nounconnected_drive") != 0 &&
                         strcmp(nm, "undefineall") != 0)
                    imp_warn_directive(L, start, nm, nlen, nm);
                L->p = q;
                if (strcmp(nm, "ifdef") == 0 || strcmp(nm, "ifndef") == 0 || strcmp(nm, "elsif") == 0 ||
                    strcmp(nm, "undef") == 0) {
                    while (L->p < L->len && (L->s[L->p] == ' ' || L->s[L->p] == '\t'))
                        L->p++;
                    while (L->p < L->len && imp_id_char((unsigned char)L->s[L->p]))
                        L->p++;
                } else if (strcmp(nm, "else") == 0 || strcmp(nm, "endif") == 0 || strcmp(nm, "celldefine") == 0 ||
                           strcmp(nm, "endcelldefine") == 0 || strcmp(nm, "resetall") == 0 ||
                           strcmp(nm, "end_keywords") == 0 || strcmp(nm, "nounconnected_drive") == 0 ||
                           strcmp(nm, "undefineall") == 0) {
                    /* no arguments */
                } else {
                    imp_skip_to_eol(L, strcmp(nm, "define") == 0);
                }
                continue;
            }
            L->p = q;
            imp_push(L, IMPT_MACRO, start);
            continue;
        }
        if (imp_id_start(c) || c == '$') {
            L->p++;
            while (L->p < L->len && imp_id_char((unsigned char)L->s[L->p]))
                L->p++;
            imp_push(L, IMPT_ID, start);
            continue;
        }
        if (c == '\\') {
            /* escaped identifier: up to white space */
            while (L->p < L->len && !tern_is_space((unsigned char)L->s[L->p]))
                L->p++;
            imp_push(L, IMPT_ID, start);
            continue;
        }
        if (tern_is_digit(c) || (c == '\'' && L->p + 1 < L->len && L->s[L->p + 1] &&
                                 strchr("01xXzZsSbBoOdDhH", L->s[L->p + 1]))) {
            size_t q;
            while (L->p < L->len && (tern_is_digit((unsigned char)L->s[L->p]) || L->s[L->p] == '_'))
                L->p++;
            if (L->p < L->len && L->s[L->p] == '.' && L->p + 1 < L->len && tern_is_digit((unsigned char)L->s[L->p + 1])) {
                L->p++;
                while (L->p < L->len && (tern_is_digit((unsigned char)L->s[L->p]) || L->s[L->p] == '_'))
                    L->p++;
            }
            if (L->p < L->len && (L->s[L->p] == 'e' || L->s[L->p] == 'E') && L->p > start &&
                L->p + 1 < L->len && (tern_is_digit((unsigned char)L->s[L->p + 1]) || L->s[L->p + 1] == '-' || L->s[L->p + 1] == '+')) {
                L->p += 2;
                while (L->p < L->len && tern_is_digit((unsigned char)L->s[L->p]))
                    L->p++;
            }
            q = L->p;
            while (q < L->len && (L->s[q] == ' ' || L->s[q] == '\t'))
                q++;
            if (q < L->len && L->s[q] == '\'' && q + 1 < L->len && L->s[q + 1] != '{') {
                /* based literal: [size] '[s]base digits */
                L->p = q + 1;
                if (L->p < L->len && (L->s[L->p] == 's' || L->s[L->p] == 'S'))
                    L->p++;
                if (L->p < L->len && strchr("bBoOdDhH", L->s[L->p]) && L->s[L->p]) {
                    L->p++;
                    while (L->p < L->len && (L->s[L->p] == ' ' || L->s[L->p] == '\t'))
                        L->p++;
                }
                while (L->p < L->len && (tern_is_xdigit((unsigned char)L->s[L->p]) || L->s[L->p] == '_' ||
                                         strchr("xXzZ?", L->s[L->p])) && L->s[L->p])
                    L->p++;
            }
            imp_push(L, IMPT_NUM, start);
            continue;
        }
        if (c == '"') {
            L->p++;
            while (L->p < L->len && L->s[L->p] != '"' && L->s[L->p] != '\n') {
                if (L->s[L->p] == '\\')
                    L->p++;
                L->p++;
            }
            if (L->p < L->len && L->s[L->p] == '"')
                L->p++;
            imp_push(L, IMPT_STR, start);
            continue;
        }
        {
            size_t i;
            for (i = 0; i < TERN_ARRAY_LEN(imp_ops); i++) {
                size_t n = strlen(imp_ops[i]);
                if (L->p + n <= L->len && memcmp(L->s + L->p, imp_ops[i], n) == 0)
                    break;
            }
            L->p += i < TERN_ARRAY_LEN(imp_ops) ? strlen(imp_ops[i]) : 1;
            imp_push(L, IMPT_OP, start);
        }
    }
    {
        ImpTok t;
        t.k = IMPT_EOF;
        t.off = (uint32_t)L->len;
        t.len = 0;
        vec_push(L->toks, t);
    }
}

/* ------------------------------------------------------------ parser --- */

typedef struct ImpRange {
    size_t a, b;               /* token indices [a, b) */
} ImpRange;

typedef VEC(ImpRange) ImpRangeVec;

typedef struct ImpDecl {       /* the type part of a port declaration */
    int has_dir;
    int dir;                   /* 0 input, 1 output, 2 inout, 3 ref */
    int has_type;              /* any type keyword, signing or packed dimension */
    const char *kw;            /* logic, reg, wire, bit, int, integer, ... or NULL */
    int is_signed, is_unsigned;
    ImpRange dims[8];          /* packed dimensions: tokens inside [ ] */
    int ndims;
    int user_type;             /* an identifier used as a type */
    size_t user_tok, user_end; /* its tokens [user_tok, user_end) */
    int interface;             /* interface port `iface[.modport] name` */
    int unsupported;           /* real, string, chandle, event, ... */
    const char *unsupported_kw;
} ImpDecl;

typedef struct ImpPort {
    size_t name_tok;
    ImpDecl d;
    int has_decl;              /* non-ANSI: a direction declaration was found */
    int unpacked;              /* unpacked dimensions after the name */
    ImpRange unpacked_dims;
    int dup;
} ImpPort;

typedef struct ImpParam {
    size_t name_tok;
    int local;
    int is_type;
    ImpDecl d;
    ImpRange def;              /* default expression tokens; a == b if none */
} ImpParam;

typedef VEC(ImpPort) ImpPortVec;
typedef VEC(ImpParam) ImpParamVec;

typedef struct ImpModule {
    size_t kw_tok, name_tok;
    ImpPortVec ports;
    ImpParamVec params;
    int bad;
    int star_ports;            /* module m(.*) */
} ImpModule;

typedef VEC(ImpModule) ImpModuleVec;

typedef struct Imp {
    ImpLexer L;
    const ImpTok *t;
    size_t n;
    StrBuf todo;               /* scratch */
    int errors;
} Imp;

static int imp_eq(const Imp *I, size_t i, const char *s)
{
    size_t n = strlen(s);
    return i < I->n && I->t[i].len == n && memcmp(I->L.s + I->t[i].off, s, n) == 0;
}

static int imp_is_id(const Imp *I, size_t i)
{
    return i < I->n && I->t[i].k == IMPT_ID;
}

static Span imp_span(const Imp *I, size_t i)
{
    if (i >= I->n)
        i = I->n - 1;
    return span_make(I->L.file, I->t[i].off, I->t[i].len);
}

/* index of the bracket matching the opener at i, or n */
static size_t imp_match(const Imp *I, size_t i)
{
    int depth = 0;
    size_t j;
    for (j = i; j < I->n && I->t[j].k != IMPT_EOF; j++) {
        if (I->t[j].k != IMPT_OP)
            continue;
        if (imp_eq(I, j, "(") || imp_eq(I, j, "[") || imp_eq(I, j, "{") || imp_eq(I, j, "'{"))
            depth++;
        else if (imp_eq(I, j, ")") || imp_eq(I, j, "]") || imp_eq(I, j, "}")) {
            if (--depth == 0)
                return j;
        }
    }
    return I->n;
}

/* split [a, b) at top-level commas */
static void imp_split(const Imp *I, size_t a, size_t b, ImpRangeVec *out)
{
    size_t j, start = a;
    int depth = 0;
    for (j = a; j < b; j++) {
        if (I->t[j].k == IMPT_OP) {
            if (imp_eq(I, j, "(") || imp_eq(I, j, "[") || imp_eq(I, j, "{") || imp_eq(I, j, "'{"))
                depth++;
            else if (imp_eq(I, j, ")") || imp_eq(I, j, "]") || imp_eq(I, j, "}"))
                depth--;
            else if (depth == 0 && imp_eq(I, j, ",")) {
                ImpRange r;
                r.a = start;
                r.b = j;
                vec_push(*out, r);
                start = j + 1;
            }
        }
    }
    if (b > start || (b == start && b > a)) {
        ImpRange r;
        r.a = start;
        r.b = b;
        vec_push(*out, r);
    }
}

static const char *const imp_data_kws[] = {
    "logic", "reg", "bit", "wire", "tri", "wand", "wor", "triand", "trior", "tri0", "tri1", "supply0", "supply1",
    "uwire", "var", "int", "integer", "byte", "shortint", "longint", "time",
};
static const char *const imp_unsup_kws[] = {"real", "realtime", "shortreal", "string", "chandle", "event"};

static const char *imp_kw_of(const Imp *I, size_t i, const char *const *list, size_t n)
{
    size_t k;
    if (!imp_is_id(I, i))
        return NULL;
    for (k = 0; k < n; k++) {
        if (imp_eq(I, i, list[k]))
            return list[k];
    }
    return NULL;
}

/* Parses `[dir] [net/var] [type] [signed] {[dims]}` from i; returns the index
 * after it. Stops at the first token that is not part of the type. */
static size_t imp_parse_decl(Imp *I, size_t i, size_t end, ImpDecl *d)
{
    memset(d, 0, sizeof *d);
    if (imp_eq(I, i, "input") || imp_eq(I, i, "output") || imp_eq(I, i, "inout") || imp_eq(I, i, "ref")) {
        d->has_dir = 1;
        d->dir = imp_eq(I, i, "input") ? 0 : imp_eq(I, i, "output") ? 1 : imp_eq(I, i, "inout") ? 2 : 3;
        i++;
    }
    for (; i < end; i++) {
        const char *kw = imp_kw_of(I, i, imp_data_kws, TERN_ARRAY_LEN(imp_data_kws));
        const char *uk = imp_kw_of(I, i, imp_unsup_kws, TERN_ARRAY_LEN(imp_unsup_kws));
        if (kw) {
            d->has_type = 1;
            /* `wire logic`, `var logic`: the data type wins */
            if (!d->kw || strcmp(d->kw, "wire") == 0 || strcmp(d->kw, "var") == 0 || strcmp(d->kw, "tri") == 0)
                d->kw = kw;
            continue;
        }
        if (uk) {
            d->has_type = 1;
            d->unsupported = 1;
            d->unsupported_kw = uk;
            continue;
        }
        if (imp_eq(I, i, "signed")) {
            d->has_type = 1;
            d->is_signed = 1;
            continue;
        }
        if (imp_eq(I, i, "unsigned")) {
            d->has_type = 1;
            d->is_unsigned = 1;
            continue;
        }
        if (imp_eq(I, i, "[")) {
            size_t m = imp_match(I, i);
            if (m >= end)
                break;
            if (d->ndims < (int)TERN_ARRAY_LEN(d->dims)) {
                d->dims[d->ndims].a = i + 1;
                d->dims[d->ndims].b = m;
                d->ndims++;
            }
            d->has_type = 1;
            i = m;
            continue;
        }
        if (imp_is_id(I, i) && !d->kw && !d->user_type && !d->interface && i + 1 < end &&
            (imp_is_id(I, i + 1) || imp_eq(I, i + 1, ".") || imp_eq(I, i + 1, "::") || imp_eq(I, i + 1, "["))) {
            /* `my_t x`, `pkg::my_t x`, `axi_if.master m`, `my_t [3:0] x` */
            size_t j = i;
            if (imp_eq(I, i + 1, "[")) {
                /* `name [7:0]` alone is a name with unpacked dims; a type needs a name after the dims */
                size_t m = imp_match(I, i + 1);
                if (m + 1 >= end || !imp_is_id(I, m + 1))
                    break;
            }
            if (imp_eq(I, i + 1, "::") && imp_is_id(I, i + 2))
                j = i + 2;
            if (imp_eq(I, j + 1, ".") && imp_is_id(I, j + 2) && !d->has_dir) {
                d->interface = 1;
                d->has_type = 1;
                d->user_tok = i;
                d->user_end = j + 3;
                i = j + 2;
                continue;
            }
            if (!imp_is_id(I, j + 1) && !imp_eq(I, j + 1, "["))
                break;
            if (!d->has_dir && !d->has_type && imp_is_id(I, j + 1) && j == i) {
                /* `iface name` without direction: an interface port */
                d->interface = 1;
            } else {
                d->user_type = 1;
            }
            d->user_tok = i;
            d->user_end = j + 1;
            d->has_type = 1;
            i = j;
            continue;
        }
        break;
    }
    return i;
}

static void imp_add_port(ImpModule *m, const ImpPort *p, Imp *I)
{
    size_t k;
    ImpPort np = *p;
    for (k = 0; k < m->ports.len; k++) {
        size_t a = m->ports.data[k].name_tok, b = p->name_tok;
        if (I->t[a].len == I->t[b].len && memcmp(I->L.s + I->t[a].off, I->L.s + I->t[b].off, I->t[a].len) == 0) {
            diag_warning(imp_span(I, b), "port `%.*s` is declared again (an `ifdef branch?); the first declaration is used",
                         (int)I->t[b].len, I->L.s + I->t[b].off);
            diag_note(imp_span(I, a), "first declared here");
            return;
        }
    }
    vec_push(m->ports, np);
}

/* ANSI port list item(s) */
static void imp_ansi_ports(Imp *I, ImpModule *m, size_t a, size_t b)
{
    ImpRangeVec items = {0};
    ImpDecl prev;
    size_t k;
    int have_prev = 0;
    memset(&prev, 0, sizeof prev);
    imp_split(I, a, b, &items);
    for (k = 0; k < items.len; k++) {
        ImpRange r = items.data[k];
        ImpDecl d;
        ImpPort p;
        size_t i;
        if (r.a == r.b)
            continue;
        i = imp_parse_decl(I, r.a, r.b, &d);
        memset(&p, 0, sizeof p);
        if (i >= r.b || !imp_is_id(I, i)) {
            if (imp_eq(I, r.a, ".")) {
                diag_warning(imp_span(I, r.a), "explicit port expressions (.name(expr)) are not supported; port skipped");
            } else {
                diag_error(imp_span(I, i < r.b ? i : r.a), "expected a port name");
                I->errors++;
            }
            continue;
        }
        if (!d.has_dir && !d.has_type && have_prev) {
            d = prev; /* `input [7:0] a, b`: b inherits everything */
        } else if (!d.has_dir) {
            d.has_dir = 1;
            d.dir = have_prev ? prev.dir : 2;
            if (!have_prev && !d.interface)
                diag_warning(imp_span(I, r.a), "port without a direction: SystemVerilog makes it `inout`");
        }
        p.d = d;
        p.name_tok = i;
        p.has_decl = 1;
        i++;
        if (i < r.b && imp_eq(I, i, "[")) {
            size_t mm = imp_match(I, i);
            p.unpacked = 1;
            p.unpacked_dims.a = i;
            p.unpacked_dims.b = mm < r.b ? mm + 1 : r.b;
        }
        prev = d;
        have_prev = 1;
        imp_add_port(m, &p, I);
    }
    vec_free(items);
}

static int imp_skip_block(Imp *I, size_t *i, const char *open, const char *close)
{
    int depth = 0;
    size_t j;
    for (j = *i; j < I->n && I->t[j].k != IMPT_EOF; j++) {
        if (imp_eq(I, j, open))
            depth++;
        else if (imp_eq(I, j, close) && --depth == 0) {
            *i = j;
            return 0;
        }
    }
    *i = j;
    return -1;
}

/* Parameter declarations: `parameter [type] A = 1, B = 2` (one item list). */
static void imp_params(Imp *I, ImpModule *m, size_t a, size_t b, int in_header, int force_local)
{
    ImpRangeVec items = {0};
    size_t k;
    int local = force_local;
    ImpDecl cur;
    int is_type = 0;
    memset(&cur, 0, sizeof cur);
    imp_split(I, a, b, &items);
    for (k = 0; k < items.len; k++) {
        ImpRange r = items.data[k];
        size_t i = r.a;
        ImpParam p;
        ImpDecl d;
        if (r.a == r.b)
            continue;
        if (imp_eq(I, i, "parameter") || imp_eq(I, i, "localparam")) {
            local = force_local || imp_eq(I, i, "localparam");
            i++;
            is_type = 0;
            memset(&cur, 0, sizeof cur);
            if (imp_eq(I, i, "type")) {
                is_type = 1;
                i++;
            } else {
                size_t j = imp_parse_decl(I, i, r.b, &d);
                if (j < r.b && imp_is_id(I, j) && (j + 1 >= r.b || imp_eq(I, j + 1, "=") || imp_eq(I, j + 1, "["))) {
                    cur = d;
                    i = j;
                }
            }
        } else if (imp_eq(I, i, "type")) {
            is_type = 1;
            i++;
        } else if (in_header) {
            /* `#(int W = 8)`: a type without the `parameter` keyword */
            size_t j = imp_parse_decl(I, i, r.b, &d);
            if (j > i && j < r.b && imp_is_id(I, j)) {
                cur = d;
                i = j;
            }
        }
        if (i >= r.b || !imp_is_id(I, i)) {
            diag_warning(imp_span(I, r.a), "cannot read this parameter declaration; skipped");
            continue;
        }
        memset(&p, 0, sizeof p);
        p.name_tok = i;
        p.local = local;
        p.is_type = is_type;
        p.d = cur;
        i++;
        while (i < r.b && imp_eq(I, i, "[")) /* unpacked dims on a parameter */
            i = imp_match(I, i) + 1;
        if (i < r.b && imp_eq(I, i, "=")) {
            p.def.a = i + 1;
            p.def.b = r.b;
        }
        vec_push(m->params, p);
    }
    vec_free(items);
}

/* Scans a module body for non-ANSI port declarations, net declarations
 * that refine them, and (when the header had no #()) parameters. */
static void imp_body(Imp *I, ImpModule *m, size_t a, size_t b, int header_params)
{
    size_t i = a;
    while (i < b) {
        size_t stmt_end;
        if (imp_eq(I, i, "function") || imp_eq(I, i, "task")) {
            int proto = i > a && (imp_eq(I, i - 1, "import") || imp_eq(I, i - 1, "export") || imp_eq(I, i - 1, "extern") ||
                                  imp_eq(I, i - 1, "virtual") || (i > a + 1 && imp_eq(I, i - 2, "import")));
            if (!proto) {
                const char *close = imp_eq(I, i, "function") ? "endfunction" : "endtask";
                while (i < b && !imp_eq(I, i, close))
                    i++;
                i++;
                continue;
            }
        }
        {
            static const char *const blocks[][2] = {
                {"clocking", "endclocking"}, {"covergroup", "endgroup"}, {"property", "endproperty"},
                {"sequence", "endsequence"}, {"specify", "endspecify"}, {"class", "endclass"},
                {"checker", "endchecker"}, {"program", "endprogram"}, {"interface", "endinterface"},
            };
            size_t k;
            int skipped = 0;
            for (k = 0; k < TERN_ARRAY_LEN(blocks); k++) {
                if (imp_eq(I, i, blocks[k][0]) && !(i > a && imp_eq(I, i - 1, "default"))) {
                    while (i < b && !imp_eq(I, i, blocks[k][1]))
                        i++;
                    i++;
                    skipped = 1;
                    break;
                }
            }
            if (skipped)
                continue;
        }
        if (imp_eq(I, i, "module")) {
            size_t j = i;
            imp_skip_block(I, &j, "module", "endmodule");
            i = j + 1;
            continue;
        }
        /* a statement up to `;` at depth 0 */
        {
            int depth = 0;
            for (stmt_end = i; stmt_end < b; stmt_end++) {
                if (imp_eq(I, stmt_end, "(") || imp_eq(I, stmt_end, "[") || imp_eq(I, stmt_end, "{") ||
                    imp_eq(I, stmt_end, "'{"))
                    depth++;
                else if (imp_eq(I, stmt_end, ")") || imp_eq(I, stmt_end, "]") || imp_eq(I, stmt_end, "}"))
                    depth--;
                else if (depth <= 0 && imp_eq(I, stmt_end, ";"))
                    break;
                else if (depth <= 0 && (imp_eq(I, stmt_end, "begin") || imp_eq(I, stmt_end, "end") ||
                                        imp_eq(I, stmt_end, "generate") || imp_eq(I, stmt_end, "endgenerate")) &&
                         stmt_end > i)
                    break;
            }
        }
        if (imp_eq(I, i, "input") || imp_eq(I, i, "output") || imp_eq(I, i, "inout")) {
            ImpDecl d;
            size_t j = imp_parse_decl(I, i, stmt_end, &d);
            ImpRangeVec names = {0};
            size_t k;
            imp_split(I, j, stmt_end, &names);
            for (k = 0; k < names.len; k++) {
                size_t nt = names.data[k].a, q;
                if (!imp_is_id(I, nt))
                    continue;
                for (q = 0; q < m->ports.len; q++) {
                    ImpPort *p = &m->ports.data[q];
                    if (I->t[p->name_tok].len == I->t[nt].len &&
                        memcmp(I->L.s + I->t[p->name_tok].off, I->L.s + I->t[nt].off, I->t[nt].len) == 0) {
                        if (p->has_decl) {
                            diag_warning(imp_span(I, nt), "port `%.*s` is declared again; the first declaration is used",
                                         (int)I->t[nt].len, I->L.s + I->t[nt].off);
                        } else {
                            p->d = d;
                            p->has_decl = 1;
                            if (nt + 1 < names.data[k].b && imp_eq(I, nt + 1, "[")) {
                                p->unpacked = 1;
                                p->unpacked_dims.a = nt + 1;
                                p->unpacked_dims.b = names.data[k].b;
                            }
                        }
                        break;
                    }
                }
                if (q == m->ports.len)
                    diag_warning(imp_span(I, nt), "`%.*s` is declared as a port but is not in the port list; ignored",
                                 (int)I->t[nt].len, I->L.s + I->t[nt].off);
            }
            vec_free(names);
        } else if (imp_kw_of(I, i, imp_data_kws, TERN_ARRAY_LEN(imp_data_kws))) {
            /* `reg [3:0] q;` refines a port declared as `output q;` */
            ImpDecl d;
            size_t j = imp_parse_decl(I, i, stmt_end, &d);
            ImpRangeVec names = {0};
            size_t k;
            imp_split(I, j, stmt_end, &names);
            for (k = 0; k < names.len; k++) {
                size_t nt = names.data[k].a, q;
                if (!imp_is_id(I, nt))
                    continue;
                for (q = 0; q < m->ports.len; q++) {
                    ImpPort *p = &m->ports.data[q];
                    if (p->has_decl && I->t[p->name_tok].len == I->t[nt].len &&
                        memcmp(I->L.s + I->t[p->name_tok].off, I->L.s + I->t[nt].off, I->t[nt].len) == 0) {
                        if (!p->d.ndims && d.ndims) {
                            memcpy(p->d.dims, d.dims, sizeof d.dims);
                            p->d.ndims = d.ndims;
                        }
                        if (d.is_signed)
                            p->d.is_signed = 1;
                        if (d.kw && (!p->d.kw || strcmp(p->d.kw, "wire") == 0))
                            p->d.kw = d.kw;
                        p->d.has_type = 1;
                    }
                }
            }
            vec_free(names);
        } else if (imp_eq(I, i, "parameter")) {
            imp_params(I, m, i, stmt_end, 0, header_params);
        }
        i = stmt_end + 1;
    }
}

static void imp_module(Imp *I, size_t *pi, ImpModuleVec *mods)
{
    ImpModule m;
    size_t i = *pi + 1, body, end;
    int header_params = 0;
    memset(&m, 0, sizeof m);
    m.kw_tok = *pi;
    if (imp_eq(I, i, "static") || imp_eq(I, i, "automatic"))
        i++;
    if (!imp_is_id(I, i)) {
        diag_error(imp_span(I, i), "expected a module name after `module`");
        I->errors++;
        *pi = i;
        return;
    }
    m.name_tok = i++;
    while (imp_eq(I, i, "import")) {
        while (i < I->n && !imp_eq(I, i, ";") && I->t[i].k != IMPT_EOF)
            i++;
        i++;
    }
    if (imp_eq(I, i, "#") && imp_eq(I, i + 1, "(")) {
        size_t close = imp_match(I, i + 1);
        if (close >= I->n) {
            diag_error(imp_span(I, i + 1), "unterminated parameter list");
            I->errors++;
            *pi = I->n - 1;
            return;
        }
        imp_params(I, &m, i + 2, close, 1, 0);
        header_params = 1;
        i = close + 1;
    }
    if (imp_eq(I, i, "(")) {
        size_t close = imp_match(I, i), k;
        int ansi = 0;
        if (close >= I->n) {
            diag_error(imp_span(I, i), "unterminated port list");
            I->errors++;
            *pi = I->n - 1;
            return;
        }
        if (imp_eq(I, i + 1, ".*")) {
            m.star_ports = 1;
        } else {
            /* ANSI if any item starts with something other than a bare name */
            ImpRangeVec items = {0};
            imp_split(I, i + 1, close, &items);
            for (k = 0; k < items.len; k++) {
                if (items.data[k].b - items.data[k].a > 1 ||
                    (items.data[k].b > items.data[k].a &&
                     (imp_eq(I, items.data[k].a, "input") || imp_eq(I, items.data[k].a, "output") ||
                      imp_eq(I, items.data[k].a, "inout"))))
                    ansi = 1;
            }
            if (ansi) {
                imp_ansi_ports(I, &m, i + 1, close);
            } else {
                for (k = 0; k < items.len; k++) {
                    ImpPort p;
                    if (items.data[k].a == items.data[k].b || !imp_is_id(I, items.data[k].a))
                        continue;
                    memset(&p, 0, sizeof p);
                    p.name_tok = items.data[k].a;
                    imp_add_port(&m, &p, I);
                }
            }
            vec_free(items);
        }
        i = close + 1;
    }
    if (!imp_eq(I, i, ";")) {
        diag_error(imp_span(I, i), "expected `;` after the header of module `%.*s`", (int)I->t[m.name_tok].len,
                   I->L.s + I->t[m.name_tok].off);
        I->errors++;
        m.bad = 1;
    }
    body = i + 1;
    end = *pi;
    if (imp_skip_block(I, &end, "module", "endmodule") != 0 && imp_skip_block(I, &end, "macromodule", "endmodule") != 0) {
        diag_error(imp_span(I, m.name_tok), "module `%.*s` has no `endmodule`", (int)I->t[m.name_tok].len,
                   I->L.s + I->t[m.name_tok].off);
        I->errors++;
        m.bad = 1;
        end = I->n - 1;
    }
    if (!m.bad)
        imp_body(I, &m, body, end, header_params);
    if (!m.bad) {
        size_t k;
        for (k = 0; k < m.ports.len; k++) {
            if (!m.ports.data[k].has_decl) {
                diag_error(imp_span(I, m.ports.data[k].name_tok), "port `%.*s` has no input/output/inout declaration",
                           (int)I->t[m.ports.data[k].name_tok].len, I->L.s + I->t[m.ports.data[k].name_tok].off);
                I->errors++;
            }
        }
        vec_push(*mods, m);
    } else {
        vec_free(m.ports);
        vec_free(m.params);
    }
    *pi = end;
}

/* ------------------------------------------------------------ output --- */

static const char *const imp_tern_keywords[] = {
    "package", "use", "pub", "module", "extern", "bundle", "struct", "enum", "const", "fn", "type", "regmap",
    "domain", "where", "test", "property", "sequence", "in", "out", "flip", "let", "var", "reg", "mem", "on",
    "comb", "match", "if", "else", "for", "while", "return", "as", "sim", "expect", "assert", "assume", "cover",
    "true", "false", "and", "or", "not", "self", "std", "cfg", "bool", "uint", "sint", "bit", "clock", "reset",
    "tri",
};

static int imp_is_keyword(const char *s, size_t n)
{
    size_t k;
    for (k = 0; k < TERN_ARRAY_LEN(imp_tern_keywords); k++) {
        if (strlen(imp_tern_keywords[k]) == n && memcmp(imp_tern_keywords[k], s, n) == 0)
            return 1;
    }
    return 0;
}

/* Writes a Tern identifier for SV name s[0..n); returns 1 if it differs
 * (the caller adds #[sv_name("...")]). */
static int imp_put_ident(StrBuf *sb, const char *s, size_t n)
{
    size_t k;
    int changed = 0;
    if (n && s[0] == '\\') {
        s++;
        n--;
        changed = 1;
    }
    if (!n || !(tern_is_alpha((unsigned char)s[0]) || s[0] == '_')) {
        sb_putc(sb, '_');
        changed = 1;
    }
    for (k = 0; k < n; k++) {
        if (tern_is_ident((unsigned char)s[k]))
            sb_putc(sb, s[k]);
        else {
            sb_putc(sb, '_');
            changed = 1;
        }
    }
    if (!changed && imp_is_keyword(s, n)) {
        sb_putc(sb, '_');
        changed = 1;
    }
    return changed;
}

/* A Tern string literal for s[0..n). */
static void imp_put_str(StrBuf *sb, const char *s, size_t n)
{
    size_t k;
    sb_putc(sb, '"');
    for (k = 0; k < n; k++) {
        if (s[k] == '"' || s[k] == '\\')
            sb_putc(sb, '\\');
        sb_putc(sb, s[k]);
    }
    sb_putc(sb, '"');
}

static int imp_tok_is_number(const Imp *I, size_t i, int64_t *v)
{
    uint64_t u;
    char buf[32];
    if (i >= I->n || I->t[i].k != IMPT_NUM || I->t[i].len >= sizeof buf)
        return 0;
    memcpy(buf, I->L.s + I->t[i].off, I->t[i].len);
    buf[I->t[i].len] = 0;
    if (!tern_parse_u64(buf, &u) || u > (uint64_t)INT64_MAX)
        return 0;
    *v = (int64_t)u;
    return 1;
}

typedef struct ImpCtx {
    Imp *I;
    const ImpModule *m;
    int todo;                  /* the expression needs review */
    const char *todo_why;
    int depth;
} ImpCtx;

static void imp_expr(ImpCtx *C, size_t a, size_t b, StrBuf *out);

/* A converted number literal. */
static void imp_number(ImpCtx *C, size_t i, StrBuf *out)
{
    const char *s = C->I->L.s + C->I->t[i].off;
    size_t n = C->I->t[i].len, q = 0, k;
    const char *tick = memchr(s, '\'', n);
    char base;
    StrBuf digits = {0};
    if (!tick) {
        int real = 0;
        for (k = 0; k < n; k++) {
            if (s[k] == '.' || s[k] == 'e' || s[k] == 'E')
                real = 1;
        }
        sb_append(out, s, n);
        if (real) {
            C->todo = 1;
            C->todo_why = "real number";
        }
        return;
    }
    q = (size_t)(tick - s) + 1;
    if (q < n && (s[q] == 's' || s[q] == 'S'))
        q++;
    if (q >= n) {
        sb_append(out, s, n);
        C->todo = 1;
        return;
    }
    base = (char)tern_to_lower((unsigned char)s[q]);
    if (base != 'b' && base != 'o' && base != 'd' && base != 'h') {
        /* '0 '1 'x 'z */
        if (s[q] == '0') {
            sb_putc(out, '0');
        } else {
            sb_append(out, s, n);
            C->todo = 1;
            C->todo_why = "unsized fill literal";
        }
        return;
    }
    q++;
    for (; q < n; q++) {
        if (s[q] == ' ' || s[q] == '\t')
            continue;
        if (strchr("xXzZ?", s[q])) {
            C->todo = 1;
            C->todo_why = "x/z digits";
        }
        sb_putc(&digits, s[q]);
    }
    if (base == 'h')
        sb_puts(out, "0x");
    else if (base == 'o')
        sb_puts(out, "0o");
    else if (base == 'b')
        sb_puts(out, "0b");
    sb_puts(out, digits.len ? digits.data : "0");
    sb_free(&digits);
}

static const ImpParam *imp_find_param(const ImpCtx *C, size_t tok)
{
    size_t k;
    const Imp *I = C->I;
    for (k = 0; C->m && k < C->m->params.len; k++) {
        const ImpParam *p = &C->m->params.data[k];
        if (I->t[p->name_tok].len == I->t[tok].len &&
            memcmp(I->L.s + I->t[p->name_tok].off, I->L.s + I->t[tok].off, I->t[tok].len) == 0)
            return p;
    }
    return NULL;
}

static void imp_expr(ImpCtx *C, size_t a, size_t b, StrBuf *out)
{
    Imp *I = C->I;
    size_t i;
    int prev_operand = 0; /* the previous token ends an operand */
    int need_space = 0;
    C->depth++;
    for (i = a; i < b; i++) {
        const ImpTok *t = &I->t[i];
        const char *s = I->L.s + t->off;
        int is_op = t->k == IMPT_OP;
        if (is_op && (imp_eq(I, i, ")") || imp_eq(I, i, "]") || imp_eq(I, i, ","))) {
            sb_append(out, s, t->len);
            need_space = 1;
            prev_operand = !imp_eq(I, i, ",");
            continue;
        }
        if (need_space && !(is_op && (imp_eq(I, i, "(") || imp_eq(I, i, "[")) && prev_operand))
            sb_putc(out, ' ');
        need_space = 1;
        if (t->k == IMPT_NUM) {
            imp_number(C, i, out);
            prev_operand = 1;
        } else if (t->k == IMPT_ID) {
            const ImpParam *p;
            if (t->len == 6 && memcmp(s, "$clog2", 6) == 0) {
                sb_puts(out, "clog2");
            } else if (s[0] == '$') {
                sb_append(out, s, t->len);
                C->todo = 1;
                C->todo_why = "system function";
            } else if ((p = imp_find_param(C, i)) != NULL && p->local && p->def.a < p->def.b && C->depth < 8) {
                /* localparams are not generics: substitute their value */
                int primary = p->def.b - p->def.a == 1 ||
                              (imp_is_id(I, p->def.a) && imp_eq(I, p->def.a + 1, "(") &&
                               imp_match(I, p->def.a + 1) == p->def.b - 1);
                if (!primary)
                    sb_putc(out, '(');
                imp_expr(C, p->def.a, p->def.b, out);
                if (!primary)
                    sb_putc(out, ')');
            } else {
                if (i + 1 < b && imp_eq(I, i + 1, "::")) {
                    C->todo = 1;
                    C->todo_why = "package-scoped name";
                }
                imp_put_ident(out, s, t->len);
            }
            prev_operand = 1;
        } else if (t->k == IMPT_MACRO) {
            imp_put_ident(out, s + 1, t->len - 1);
            C->todo = 1;
            C->todo_why = "macro";
            prev_operand = 1;
        } else if (t->k == IMPT_STR) {
            sb_append(out, s, t->len);
            prev_operand = 1;
        } else {
            int unary = !prev_operand && (imp_eq(I, i, "-") || imp_eq(I, i, "+") || imp_eq(I, i, "~") ||
                                          imp_eq(I, i, "!"));
            if (imp_eq(I, i, "&&"))
                sb_puts(out, "and");
            else if (imp_eq(I, i, "||"))
                sb_puts(out, "or");
            else {
                if (imp_eq(I, i, "?") || imp_eq(I, i, "**") || imp_eq(I, i, "{") || imp_eq(I, i, "'{") ||
                    imp_eq(I, i, "::")) {
                    C->todo = 1;
                    C->todo_why = imp_eq(I, i, "?") ? "conditional operator"
                                  : imp_eq(I, i, "**") ? "power operator"
                                                       : "SystemVerilog-only syntax";
                }
                sb_append(out, s, t->len);
            }
            prev_operand = 0;
            if (unary || imp_eq(I, i, "(") || imp_eq(I, i, "[") || imp_eq(I, i, "::"))
                need_space = 0;
        }
    }
    C->depth--;
}

/* Width of one packed dimension [msb:lsb] as a Tern expression; *lit gets
 * the value when both bounds are literals. Returns 0 if the range is odd. */
static int imp_dim_width(ImpCtx *C, ImpRange r, StrBuf *out, int64_t *lit, int *ascending)
{
    Imp *I = C->I;
    size_t colon = r.b, k;
    int depth = 0;
    int64_t hi, lo;
    *lit = -1;
    *ascending = 0;
    for (k = r.a; k < r.b; k++) {
        if (imp_eq(I, k, "(") || imp_eq(I, k, "["))
            depth++;
        else if (imp_eq(I, k, ")") || imp_eq(I, k, "]"))
            depth--;
        else if (depth == 0 && imp_eq(I, k, ":")) {
            colon = k;
            break;
        } else if (depth == 0 && (imp_eq(I, k, "+:") || imp_eq(I, k, "-:"))) {
            return 0;
        }
    }
    if (colon == r.b) {
        /* [N]: SystemVerilog shorthand for [N-1:0] in unpacked dims; in a packed context it is N bits */
        if (r.b - r.a == 1 && imp_tok_is_number(I, r.a, &hi)) {
            *lit = hi;
            sb_printf(out, "%lld", (long long)hi);
            return 1;
        }
        imp_expr(C, r.a, r.b, out);
        return 1;
    }
    if (colon - r.a == 1 && r.b - colon - 1 == 1 && imp_tok_is_number(I, r.a, &hi) &&
        imp_tok_is_number(I, colon + 1, &lo)) {
        int64_t w = hi >= lo ? hi - lo + 1 : lo - hi + 1;
        *ascending = hi < lo;
        *lit = w;
        sb_printf(out, "%lld", (long long)w);
        return 1;
    }
    /* [X-1:0] -> X ; [X:0] -> X + 1 ; [0:X-1] -> X (ascending) */
    if (r.b - colon - 1 == 1 && imp_tok_is_number(I, colon + 1, &lo) && lo == 0) {
        if (colon - r.a >= 3 && imp_eq(I, colon - 2, "-") && imp_tok_is_number(I, colon - 1, &hi) && hi == 1) {
            imp_expr(C, r.a, colon - 2, out);
            return 1;
        }
        if (colon - r.a == 1 && imp_tok_is_number(I, r.a, &hi)) {
            *lit = hi + 1;
            sb_printf(out, "%lld", (long long)(hi + 1));
            return 1;
        }
        imp_expr(C, r.a, colon, out);
        sb_puts(out, " + 1");
        return 1;
    }
    if (colon - r.a == 1 && imp_tok_is_number(I, r.a, &hi) && hi == 0) {
        *ascending = 1;
        if (r.b - colon - 1 >= 3 && imp_eq(I, r.b - 2, "-") && imp_tok_is_number(I, r.b - 1, &lo) && lo == 1) {
            imp_expr(C, colon + 1, r.b - 2, out);
            return 1;
        }
        imp_expr(C, colon + 1, r.b, out);
        sb_puts(out, " + 1");
        return 1;
    }
    sb_putc(out, '(');
    imp_expr(C, r.a, colon, out);
    sb_puts(out, ") - (");
    imp_expr(C, colon + 1, r.b, out);
    sb_puts(out, ") + 1");
    return 1;
}

/* Renders the Tern type of a declaration. Returns 1 for a supported type,
 * 0 (with *why) otherwise. *is_bool: plain 1-bit unsigned. */
static int imp_type(ImpCtx *C, const ImpDecl *d, StrBuf *out, int *is_bool, const char **why, StrBuf *note)
{
    Imp *I = C->I;
    int64_t base_w = 1;
    int sgn = d->is_signed;
    int k;
    *is_bool = 0;
    *why = NULL;
    if (d->interface) {
        *why = "interface port: model it as a bundle and connect it with #[sv_name]";
        sb_append(out, I->L.s + I->t[d->user_tok].off, I->t[d->user_end - 1].off + I->t[d->user_end - 1].len - I->t[d->user_tok].off);
        return 0;
    }
    if (d->unsupported) {
        *why = "type has no Tern equivalent";
        sb_puts(out, d->unsupported_kw);
        return 0;
    }
    if (d->user_type) {
        *why = "user-defined type: write the matching Tern struct/enum type";
        sb_append(out, I->L.s + I->t[d->user_tok].off, I->t[d->user_end - 1].off + I->t[d->user_end - 1].len - I->t[d->user_tok].off);
        return 0;
    }
    if (d->kw) {
        if (strcmp(d->kw, "int") == 0 || strcmp(d->kw, "integer") == 0) {
            base_w = 32;
            sgn = !d->is_unsigned;
        } else if (strcmp(d->kw, "byte") == 0) {
            base_w = 8;
            sgn = !d->is_unsigned;
        } else if (strcmp(d->kw, "shortint") == 0) {
            base_w = 16;
            sgn = !d->is_unsigned;
        } else if (strcmp(d->kw, "longint") == 0) {
            base_w = 64;
            sgn = !d->is_unsigned;
        } else if (strcmp(d->kw, "time") == 0) {
            base_w = 64;
        }
    }
    if (base_w != 1 && d->ndims) {
        *why = "packed dimensions on an integer type";
        sb_puts(out, d->kw);
        return 0;
    }
    if (d->ndims == 0) {
        if (base_w == 1 && !sgn) {
            sb_puts(out, "bool");
            *is_bool = 1;
        } else {
            sb_printf(out, "%s<%lld>", sgn ? "sint" : "uint", (long long)base_w);
        }
        return 1;
    }
    {
        /* element = last dimension; outer dimensions become arrays */
        StrBuf w = {0}, inner = {0};
        int64_t lit;
        int asc, ok;
        ok = imp_dim_width(C, d->dims[d->ndims - 1], &w, &lit, &asc);
        if (!ok) {
            *why = "indexed part-select range in a declaration";
            sb_free(&w);
            return 0;
        }
        if (asc)
            sb_printf(note, "%sascending range in SystemVerilog", note->len ? "; " : "");
        if (lit == 1 && !sgn && d->ndims == 1) {
            sb_puts(&inner, "bool");
            *is_bool = 1;
        } else if (lit == 0 || lit < -1) {
            *why = "zero or negative width";
            sb_free(&w);
            return 0;
        } else {
            sb_printf(&inner, "%s<%s>", sgn && d->ndims == 1 ? "sint" : "uint", w.data);
        }
        if (sgn && d->ndims > 1)
            sb_printf(note, "%ssigned packed array: elements imported as unsigned", note->len ? "; " : "");
        for (k = d->ndims - 2; k >= 0; k--) {
            StrBuf dw = {0}, t = {0};
            if (!imp_dim_width(C, d->dims[k], &dw, &lit, &asc)) {
                *why = "indexed part-select range in a declaration";
                sb_free(&dw);
                sb_free(&w);
                sb_free(&inner);
                return 0;
            }
            sb_printf(&t, "[%s; %s]", inner.data, dw.data);
            sb_free(&inner);
            inner = t;
            sb_free(&dw);
        }
        sb_puts(out, inner.data);
        sb_free(&inner);
        sb_free(&w);
    }
    return 1;
}

/* lower-case copy of a port name with i_/o_/_i/_o affixes removed */
static void imp_norm_name(const char *s, size_t n, char *out, size_t outsz)
{
    size_t k, o = 0;
    char buf[128];
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    for (k = 0; k < n; k++)
        buf[k] = (char)tern_to_lower((unsigned char)s[k]);
    buf[n] = 0;
    s = buf;
    if (n > 2 && (tern_starts_with(s, "i_") || tern_starts_with(s, "o_"))) {
        s += 2;
        n -= 2;
    }
    if (n > 2 && (tern_ends_with(s, "_i") || tern_ends_with(s, "_o")))
        n -= 2;
    else if (n > 3 && tern_ends_with(s, "_in"))
        n -= 3;
    else if (n > 4 && tern_ends_with(s, "_out"))
        n -= 4;
    for (k = 0; k < n && o + 1 < outsz; k++)
        out[o++] = s[k];
    out[o] = 0;
}

static int imp_has_token(const char *name, const char *tok)
{
    size_t tl = strlen(tok);
    const char *p = name;
    while ((p = strstr(p, tok)) != NULL) {
        int left = p == name || p[-1] == '_';
        int right = p[tl] == 0 || p[tl] == '_';
        if (left && right)
            return 1;
        p += tl;
    }
    return 0;
}

static int imp_is_clock(const char *n)
{
    static const char *const not_clock[] = {"en", "ena", "enable", "sel", "select", "div", "cnt", "count", "gate",
                                            "gated", "req", "ack", "valid", "ok", "stable", "good", "lost",
                                            "fail", "err", "sw", "mux", "locked", "lock", "freq", "out_en"};
    size_t k;
    for (k = 0; k < TERN_ARRAY_LEN(not_clock); k++) {
        if (imp_has_token(n, not_clock[k]))
            return 0;
    }
    if (strcmp(n, "clk") == 0 || strcmp(n, "clock") == 0 || imp_has_token(n, "clk") || imp_has_token(n, "clock"))
        return 1;
    /* aclk, hclk, pclk, sclk, clk100, clkin */
    if (strlen(n) <= 6 && (tern_ends_with(n, "clk") || tern_starts_with(n, "clk")))
        return 1;
    return 0;
}

/* 0 = not a reset; else 1 and *low / *async from the name */
static int imp_is_reset(const char *n, int *low, int *async)
{
    static const char *const not_reset[] = {"done", "req", "ack", "cnt", "count", "busy", "status", "ok", "val",
                                            "value", "vector", "addr", "en"};
    static const char *const resets[] = {"rst", "reset", "rstn", "resetn", "rst_n", "reset_n", "rstb", "rst_b",
                                         "arst", "arstn", "arst_n", "areset", "aresetn", "areset_n", "nrst",
                                         "nreset", "srst", "srstn", "srst_n", "por", "rst_l", "reset_l"};
    size_t k;
    int found = 0;
    *low = 0;
    *async = 0;
    for (k = 0; k < TERN_ARRAY_LEN(not_reset); k++) {
        if (imp_has_token(n, not_reset[k]))
            return 0;
    }
    for (k = 0; k < TERN_ARRAY_LEN(resets); k++) {
        if (imp_has_token(n, resets[k]))
            found = 1;
    }
    if (!found)
        return 0;
    *low = tern_ends_with(n, "_n") || tern_ends_with(n, "rstn") || tern_ends_with(n, "resetn") ||
           tern_ends_with(n, "_b") || tern_ends_with(n, "rstb") || tern_ends_with(n, "_l") || tern_ends_with(n, "_ni") ||
           tern_starts_with(n, "nrst") || tern_starts_with(n, "nreset");
    *async = strstr(n, "arst") != NULL || strstr(n, "areset") != NULL || strstr(n, "async") != NULL;
    return 1;
}

static void imp_emit_module(Imp *I, const ImpModule *m, const char *extern_path, StrBuf *out)
{
    ImpCtx C;
    StrBuf name = {0}, generics = {0}, pre = {0}, body = {0};
    size_t k;
    int renamed, ngen = 0;
    uint32_t line, col;
    memset(&C, 0, sizeof C);
    C.I = I;
    C.m = m;
    renamed = imp_put_ident(&name, I->L.s + I->t[m->name_tok].off, I->t[m->name_tok].len);
    src_linecol(I->L.file, I->t[m->kw_tok].off, &line, &col);
    /* generics */
    for (k = 0; k < m->params.len; k++) {
        const ImpParam *p = &m->params.data[k];
        StrBuf ty = {0}, def = {0}, pname = {0}, note = {0};
        int is_bool = 0, ok = 1;
        const char *why = NULL;
        if (p->local)
            continue;
        imp_put_ident(&pname, I->L.s + I->t[p->name_tok].off, I->t[p->name_tok].len);
        C.todo = 0;
        C.todo_why = NULL;
        if (p->is_type) {
            sb_printf(&pre, "// TODO: type parameter `%s` is not imported (extern modules take value generics only)\n",
                      pname.data);
            sb_free(&pname);
            continue;
        }
        if (p->d.unsupported || p->d.user_type) {
            sb_printf(&pre, "// TODO: parameter `%s` (%s) is not imported; it keeps its SystemVerilog default\n",
                      pname.data, p->d.unsupported ? p->d.unsupported_kw : "user-defined type");
            sb_free(&pname);
            continue;
        }
        if (p->def.a < p->def.b && I->t[p->def.a].k == IMPT_STR) {
            sb_printf(&pre, "// TODO: string parameter `%s` is not imported; it keeps its SystemVerilog default\n",
                      pname.data);
            sb_free(&pname);
            continue;
        }
        if (p->d.ndims || (p->d.kw && strcmp(p->d.kw, "int") != 0 && strcmp(p->d.kw, "integer") != 0)) {
            ok = imp_type(&C, &p->d, &ty, &is_bool, &why, &note);
            if (!ok) {
                sb_printf(&pre, "// TODO: parameter `%s` is not imported: %s\n", pname.data, why);
                sb_free(&ty);
                sb_free(&pname);
                sb_free(&note);
                continue;
            }
        } else {
            int neg = p->def.a < p->def.b && imp_eq(I, p->def.a, "-");
            sb_puts(&ty, (p->d.is_signed || neg) && !p->d.is_unsigned ? "sint" : "uint");
        }
        if (p->def.a < p->def.b) {
            if (is_bool && p->def.b - p->def.a == 1 && I->t[p->def.a].k == IMPT_NUM) {
                const char *s = I->L.s + I->t[p->def.a].off;
                size_t n = I->t[p->def.a].len;
                sb_puts(&def, s[n - 1] == '1' ? "true" : s[n - 1] == '0' ? "false" : "false");
            } else {
                imp_expr(&C, p->def.a, p->def.b, &def);
            }
        }
        if (C.todo)
            sb_printf(&pre, "// TODO: check the default of `%s` (%s)\n", pname.data, C.todo_why ? C.todo_why : "review");
        if (ngen++)
            sb_puts(&generics, ", ");
        sb_printf(&generics, "%s: %s", pname.data, ty.data);
        if (def.len)
            sb_printf(&generics, " = %s", def.data);
        sb_free(&ty);
        sb_free(&def);
        sb_free(&pname);
        sb_free(&note);
    }
    /* ports */
    if (m->star_ports)
        sb_puts(&body, "    // TODO: `.*` port list: declare the ports by hand\n");
    for (k = 0; k < m->ports.len; k++) {
        const ImpPort *p = &m->ports.data[k];
        const char *pn = I->L.s + I->t[p->name_tok].off;
        size_t pl = I->t[p->name_tok].len;
        StrBuf ty = {0}, ident = {0}, note = {0}, line_sb = {0};
        int is_bool = 0, ok, prenamed;
        const char *why = NULL;
        const char *dir = p->d.dir == 1 ? "out" : "in";
        char norm[128];
        int low, async;
        if (!p->has_decl)
            continue;
        C.todo = 0;
        C.todo_why = NULL;
        prenamed = imp_put_ident(&ident, pn, pl);
        ok = imp_type(&C, &p->d, &ty, &is_bool, &why, &note);
        if (ok && p->unpacked) {
            ok = 0;
            why = "unpacked array port: flatten it (or wrap the module) in SystemVerilog";
        }
        if (ok && p->d.dir == 3) {
            ok = 0;
            why = "`ref` port";
        }
        imp_norm_name(pn, pl, norm, sizeof norm);
        if (ok && is_bool && p->d.dir != 2 && imp_is_clock(norm)) {
            sb_reset(&ty);
            sb_puts(&ty, "clock");
            if (tern_ends_with(norm, "_n") || tern_ends_with(norm, "_b"))
                sb_printf(&note, "%sTODO: confirm: clock (from the name; `_n`: negative edge? use clock<falling>)",
                          note.len ? "; " : "");
            else
                sb_printf(&note, "%sTODO: confirm: clock (from the name)", note.len ? "; " : "");
        } else if (ok && is_bool && p->d.dir != 2 && imp_is_reset(norm, &low, &async)) {
            sb_reset(&ty);
            if (!low && !async)
                sb_puts(&ty, "reset");
            else
                sb_printf(&ty, "reset<%s, %s>", async ? "async" : "sync", low ? "low" : "high");
            sb_printf(&note, "%sTODO: confirm: reset (%s from the name; %s)", note.len ? "; " : "",
                      low ? "active-low" : "active-high", async ? "asynchronous from the name" : "synchronous assumed");
        }
        if (C.todo)
            sb_printf(&note, "%sTODO: check the width (%s)", note.len ? "; " : "", C.todo_why ? C.todo_why : "review");
        if (ok && p->d.dir == 2) {
            StrBuf t2 = {0};
            sb_printf(&t2, "tri<%s>", ty.data);
            sb_free(&ty);
            ty = t2;
            sb_printf(&note, "%sinout: tri<> ports are only allowed on a #[top] module", note.len ? "; " : "");
        }
        if (!ok) {
            sb_printf(&line_sb, "    // %s %s: %s // TODO: %s", dir, ident.data, ty.len ? ty.data : "?", why);
        } else {
            sb_printf(&line_sb, "    %s %s: %s", dir, ident.data, ty.data);
            if (prenamed) {
                sb_puts(&line_sb, " #[sv_name(");
                imp_put_str(&line_sb, pn, pl);
                sb_puts(&line_sb, ")]");
            }
            if (note.len)
                sb_printf(&line_sb, " // %s", note.data);
        }
        sb_puts(&body, line_sb.data);
        sb_putc(&body, '\n');
        sb_free(&ty);
        sb_free(&ident);
        sb_free(&note);
        sb_free(&line_sb);
    }
    sb_printf(out, "\n/// `%.*s` from %s:%u.\n", (int)I->t[m->name_tok].len, I->L.s + I->t[m->name_tok].off,
              extern_path, (unsigned)line);
    sb_puts(out, pre.len ? pre.data : "");
    if (renamed) {
        sb_puts(out, "#[sv_name(");
        imp_put_str(out, I->L.s + I->t[m->name_tok].off, I->t[m->name_tok].len);
        sb_puts(out, ")]\n");
    }
    sb_printf(out, "extern module %s(", name.data);
    imp_put_str(out, extern_path, strlen(extern_path));
    sb_putc(out, ')');
    if (ngen)
        sb_printf(out, " <%s>", generics.data);
    sb_puts(out, " {\n");
    sb_puts(out, body.len ? body.data : "");
    sb_puts(out, "}\n");
    sb_free(&name);
    sb_free(&generics);
    sb_free(&pre);
    sb_free(&body);
}

/* Imports registered source `src` into `out`. Returns the number of modules
 * (or -1 on errors). */
static int imp_source(uint32_t src, const char *extern_path, const char *package, StrBuf *out)
{
    Imp I;
    const SourceFile *f = src_get(src);
    ImpModuleVec mods = {0};
    size_t i, k;
    int nmods;
    memset(&I, 0, sizeof I);
    I.L.file = src;
    I.L.s = f->text;
    I.L.len = f->len;
    imp_lex(&I.L);
    I.t = I.L.toks.data;
    I.n = I.L.toks.len;
    I.errors = I.L.errors;
    for (i = 0; i < I.n && I.t[i].k != IMPT_EOF; i++) {
        if (imp_eq(&I, i, "module") || imp_eq(&I, i, "macromodule")) {
            imp_module(&I, &i, &mods);
        } else if (imp_eq(&I, i, "interface") && !(i > 0 && imp_eq(&I, i - 1, "virtual"))) {
            size_t nm = i + 1;
            if (imp_eq(&I, nm, "static") || imp_eq(&I, nm, "automatic"))
                nm++;
            diag_warning(imp_span(&I, i), "interface `%.*s` is not imported", imp_is_id(&I, nm) ? (int)I.t[nm].len : 1,
                         imp_is_id(&I, nm) ? I.L.s + I.t[nm].off : "?");
            diag_help("model it as a Tern bundle (spec §8.1) and connect it through #[sv_name]");
            while (i < I.n && !imp_eq(&I, i, "endinterface") && I.t[i].k != IMPT_EOF)
                i++;
        } else {
            static const char *const skip[][2] = {
                {"package", "endpackage"}, {"class", "endclass"},       {"program", "endprogram"},
                {"primitive", "endprimitive"}, {"config", "endconfig"}, {"checker", "endchecker"},
            };
            for (k = 0; k < TERN_ARRAY_LEN(skip); k++) {
                if (imp_eq(&I, i, skip[k][0])) {
                    if (strcmp(skip[k][0], "primitive") == 0)
                        diag_warning(imp_span(&I, i), "user-defined primitives (UDPs) are not imported");
                    while (i < I.n && !imp_eq(&I, i, skip[k][1]) && I.t[i].k != IMPT_EOF)
                        i++;
                    break;
                }
            }
        }
    }
    sb_printf(out, "// Generated by `tern import` from %s; review every TODO.\npackage %s\n", extern_path, package);
    for (k = 0; k < mods.len; k++)
        imp_emit_module(&I, &mods.data[k], extern_path, out);
    nmods = (int)mods.len;
    for (k = 0; k < mods.len; k++) {
        vec_free(mods.data[k].ports);
        vec_free(mods.data[k].params);
    }
    vec_free(mods);
    vec_free(I.L.toks);
    sb_free(&I.todo);
    return I.errors ? -1 : nmods;
}

/* ----------------------------------------------------------- command --- */

static char *imp_sanitize_pkg(const char *s, size_t n)
{
    StrBuf sb = {0};
    size_t k;
    for (k = 0; k < n; k++) {
        char c = s[k];
        if (tern_is_alnum((unsigned char)c) || c == '_')
            sb_putc(&sb, tern_to_lower((unsigned char)c));
        else if (sb.len && sb.data[sb.len - 1] != '_')
            sb_putc(&sb, '_');
    }
    while (sb.len && sb.data[sb.len - 1] == '_')
        sb_truncate(&sb, sb.len - 1);
    if (!sb.len || !tern_is_alpha((unsigned char)sb.data[0])) {
        StrBuf t = {0};
        sb_printf(&t, "sv_%s", sb_cstr(&sb));
        sb_free(&sb);
        sb = t;
    }
    if (imp_is_keyword(sb.data, sb.len))
        sb_putc(&sb, '_');
    return sb_take(&sb);
}

static char *imp_stem_pkg(const char *path)
{
    const char *b = os_path_basename(path), *e = os_path_ext(b);
    return imp_sanitize_pkg(b, (size_t)(e - b));
}

static char *imp_find_project_root(void)
{
    char *dir = os_getcwd();
    while (dir) {
        char *p = os_path_join(dir, "tern.toml"), *up;
        int found = os_is_file(p);
        free(p);
        if (found)
            return dir;
        up = os_path_dirname(dir);
        if (strcmp(up, dir) == 0 || strcmp(up, ".") == 0) {
            free(up);
            break;
        }
        free(dir);
        dir = up;
    }
    free(dir);
    return NULL;
}

/* Lexical relative path, using ".." when needed. */
static char *imp_rel(const char *path, const char *base)
{
    char *p = os_path_abs(path), *b = os_path_abs(base);
    size_t i = 0, common = 0;
    StrBuf sb = {0};
    const char *q;
    while (p[i] && b[i] && p[i] == b[i]) {
        i++;
        if ((p[i] == '/' || !p[i]) && (b[i] == '/' || !b[i]))
            common = i;
    }
    if (common == 0) {
        free(b);
        return p;
    }
    for (q = b + common; *q;) {
        if (*q == '/')
            q++;
        if (!*q)
            break;
        sb_puts(&sb, "../");
        while (*q && *q != '/')
            q++;
    }
    q = p + common;
    if (*q == '/')
        q++;
    sb_puts(&sb, q);
    free(p);
    free(b);
    if (!sb.len)
        sb_puts(&sb, ".");
    return sb_take(&sb);
}

static int imp_one(const char *in, const char *root, const char *package, const char *out_path, int to_stdout,
                   int *nmods_out)
{
    char *data, *ext_path;
    size_t len;
    uint32_t id;
    StrBuf out = {0};
    int n;
    *nmods_out = 0;
    if (os_read_file(in, &data, &len) != 0) {
        diag_error(span_none(), "cannot read `%s`", in);
        return -1;
    }
    id = src_add(in, data, len);
    free(data);
    ext_path = root ? imp_rel(in, root) : os_path_normalize(in);
    n = imp_source(id, ext_path, package, &out);
    free(ext_path);
    fflush(stdout);
    diag_flush(); /* this file's warnings before the next progress line */
    if (n < 0) {
        sb_free(&out);
        return -1;
    }
    *nmods_out = n;
    if (n == 0) {
        diag_warning(span_make(id, 0, 0), "no module declarations found in `%s`", in);
        sb_free(&out);
        return 0;
    }
    if (to_stdout) {
        fwrite(out.data, 1, out.len, stdout);
    } else {
        char *dir = os_path_dirname(out_path);
        os_mkdir_p(dir);
        free(dir);
        if (os_write_file(out_path, out.data, out.len) != 0) {
            diag_error(span_none(), "cannot write `%s`", out_path);
            sb_free(&out);
            return -1;
        }
    }
    sb_free(&out);
    return 0;
}

typedef VEC(char *) ImpPathVec;

static void imp_collect(const char *dir, const char *skip_dir, ImpPathVec *out)
{
    OsDirList dl;
    size_t k;
    if (os_list_dir(dir, &dl) != 0)
        return;
    for (k = 0; k < dl.len; k++) {
        const char *nm = dl.items[k].name;
        char *p;
        if (nm[0] == '.')
            continue;
        p = os_path_join(dir, nm);
        if (dl.items[k].is_dir) {
            char *ap = os_path_abs(p);
            if (!dl.items[k].is_link && !(skip_dir && strcmp(ap, skip_dir) == 0))
                imp_collect(p, skip_dir, out);
            free(ap);
            free(p);
        } else if (tern_ends_with(nm, ".sv") || tern_ends_with(nm, ".v")) {
            vec_push(*out, p);
        } else {
            free(p);
        }
    }
    os_dir_list_free(&dl);
}

int cmd_import(int argc, char **argv)
{
    ArgParser ap;
    const char *out_opt = NULL, *pkg_opt = NULL, *root_opt = NULL;
    ImpPathVec inputs = {0};
    char *root = NULL;
    int errs = 0, total = 0, files = 0;
    size_t k;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("import");
            vec_free(inputs);
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "output|o", &out_opt) || args_opt(&ap, "package", &pkg_opt) ||
                   args_opt(&ap, "root", &root_opt)) {
            ;
        } else if (args_positional(&ap)) {
            vec_push(inputs, (char *)ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (!inputs.len && !ap.errors)
        args_error(&ap, "missing input: `tern import <file.sv|dir>...`");
    if (pkg_opt) {
        const char *p;
        for (p = pkg_opt; *p; p++) {
            if (!tern_is_ident((unsigned char)*p) && !(p[0] == ':' && p[1] == ':') && !(p > pkg_opt && p[-1] == ':' && p[0] == ':'))
                break;
        }
        if (*p || !tern_is_alpha((unsigned char)pkg_opt[0]))
            args_error(&ap, "invalid package name `%s`", pkg_opt);
    }
    if (ap.errors) {
        vec_free(inputs);
        return TERN_EXIT_USAGE;
    }
    root = root_opt ? os_path_abs(root_opt) : imp_find_project_root();
    for (k = 0; k < inputs.len; k++) {
        const char *in = inputs.data[k];
        if (os_is_dir(in)) {
            ImpPathVec svs = {0};
            char *outdir, *aout, *ain = os_path_abs(in);
            size_t j;
            if (out_opt)
                outdir = xstrdup(out_opt);
            else if (root)
                outdir = os_path_join(root, "src/imported");
            else
                outdir = xstrdup("imported");
            aout = os_path_abs(outdir);
            imp_collect(in, aout, &svs);
            if (!svs.len)
                diag_warning(span_none(), "no .sv or .v files found in `%s`", in);
            for (j = 0; j < svs.len; j++) {
                char *rel = imp_rel(svs.data[j], ain), *reldir = os_path_dirname(rel), *pkg, *outp;
                const char *b = os_path_basename(rel), *e = os_path_ext(b);
                StrBuf op = {0}, pk = {0};
                int n;
                /* package: --package (or "imported") plus the sub-directories */
                sb_puts(&pk, pkg_opt ? pkg_opt : "imported");
                if (strcmp(reldir, ".") != 0) {
                    char *seg = reldir;
                    while (*seg) {
                        char *slash = strchr(seg, '/');
                        size_t sl = slash ? (size_t)(slash - seg) : strlen(seg);
                        char *s = imp_sanitize_pkg(seg, sl);
                        sb_printf(&pk, "::%s", s);
                        free(s);
                        seg += sl + (slash ? 1 : 0);
                    }
                }
                pkg = sb_take(&pk);
                sb_printf(&op, "%s/%s%s%.*s", outdir, strcmp(reldir, ".") == 0 ? "" : reldir,
                          strcmp(reldir, ".") == 0 ? "" : "/", (int)(e - b), b);
                {
                    /* a.sv and a.v in one directory: keep both */
                    char *cand = NULL;
                    StrBuf t = {0};
                    sb_printf(&t, "%s.tern", op.data);
                    cand = sb_take(&t);
                    if (os_exists(cand) && tern_ends_with(b, ".v")) {
                        free(cand);
                        sb_printf(&t, "%s_v.tern", op.data);
                        cand = sb_take(&t);
                    }
                    outp = cand;
                }
                if (imp_one(svs.data[j], root, pkg, outp, 0, &n) != 0)
                    errs++;
                else if (n) {
                    printf("  %s -> %s (%d module%s)\n", svs.data[j], outp, n, n == 1 ? "" : "s");
                    total += n;
                    files++;
                }
                sb_free(&op);
                free(outp);
                free(pkg);
                free(rel);
                free(reldir);
                free(svs.data[j]);
            }
            vec_free(svs);
            free(outdir);
            free(aout);
            free(ain);
        } else {
            char *pkg = pkg_opt ? xstrdup(pkg_opt) : imp_stem_pkg(out_opt ? out_opt : in);
            int n;
            int to_stdout = !out_opt || strcmp(out_opt, "-") == 0;
            if (out_opt && inputs.len > 1 && !to_stdout && os_is_dir(out_opt)) {
                /* several files into a directory */
                StrBuf op = {0};
                const char *b = os_path_basename(in), *e = os_path_ext(b);
                free(pkg);
                pkg = pkg_opt ? xstrdup(pkg_opt) : imp_stem_pkg(in);
                sb_printf(&op, "%s/%.*s.tern", out_opt, (int)(e - b), b);
                if (imp_one(in, root, pkg, op.data, 0, &n) != 0)
                    errs++;
                sb_free(&op);
            } else if (imp_one(in, root, pkg, out_opt, to_stdout, &n) != 0) {
                errs++;
            }
            total += n;
            files += n > 0;
            free(pkg);
        }
    }
    if (!errs && files > 1)
        printf("Imported %d module%s from %d file%s.\n", total, total == 1 ? "" : "s", files, files == 1 ? "" : "s");
    free(root);
    vec_free(inputs);
    return errs || diag_error_count() ? TERN_EXIT_FAIL : TERN_EXIT_OK;
}
