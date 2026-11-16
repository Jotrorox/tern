/* ast_dump.c — deterministic S-expression dump of the AST (golden tests).
 *
 * Items and statements start a line and carry their position (`@line:col`);
 * expressions, types and patterns print inline, except that blocks and match
 * arms break lines. The format is only for tests and debugging; nothing
 * parses it back.
 */
#include "parser.h"
#include "../base/str.h"

typedef struct AdState {
    StrBuf *sb;
    int indent;
} AdState;

static void ad_expr(AdState *d, const Expr *e);
static void ad_type(AdState *d, const Type *t);
static void ad_pat(AdState *d, const Pattern *p);
static void ad_stmt(AdState *d, const Stmt *s);
static void ad_item(AdState *d, const Item *it);
static void ad_block(AdState *d, const Block *b);

static void ad_nl(AdState *d)
{
    sb_putc(d->sb, '\n');
    sb_indent(d->sb, d->indent * 2);
}

static void ad_str(AdState *d, const char *s, size_t n)
{
    size_t i;
    sb_putc(d->sb, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            sb_putc(d->sb, '\\');
            sb_putc(d->sb, c);
        } else if (c == '\n') {
            sb_puts(d->sb, "\\n");
        } else if (c == '\t') {
            sb_puts(d->sb, "\\t");
        } else if (c < 0x20 || c == 0x7F) {
            sb_printf(d->sb, "\\x%02X", c);
        } else {
            sb_putc(d->sb, c);
        }
    }
    sb_putc(d->sb, '"');
}

static void ad_pos(AdState *d, Span sp)
{
    sb_printf(d->sb, "@%u:%u", (unsigned)sp.line, (unsigned)sp.col);
}

static void ad_name(AdState *d, Ident id)
{
    sb_puts(d->sb, id.name ? id.name : "<error>");
}

static void ad_doc(AdState *d, const AstDoc *doc)
{
    if (!doc)
        return;
    sb_puts(d->sb, " (doc ");
    ad_str(d, doc->text, strlen(doc->text));
    sb_putc(d->sb, ')');
}

static void ad_attrs(AdState *d, AttrList a)
{
    uint32_t i, j;
    for (i = 0; i < a.n; i++) {
        const Attr *at = &a.items[i];
        sb_puts(d->sb, " #[");
        ad_name(d, at->name);
        if (at->has_args) {
            sb_putc(d->sb, '(');
            for (j = 0; j < at->nargs; j++) {
                if (j)
                    sb_puts(d->sb, ", ");
                if (at->args[j].name.name) {
                    ad_name(d, at->args[j].name);
                    sb_puts(d->sb, ": ");
                }
                ad_expr(d, at->args[j].value);
            }
            sb_putc(d->sb, ')');
        }
        sb_putc(d->sb, ']');
    }
}

static void ad_gargs(AdState *d, const GenericArg *args, uint32_t n)
{
    uint32_t i;
    sb_putc(d->sb, '<');
    for (i = 0; i < n; i++) {
        if (i)
            sb_puts(d->sb, ", ");
        if (args[i].name.name) {
            ad_name(d, args[i].name);
            sb_puts(d->sb, ": ");
        }
        ad_expr(d, args[i].expr);
    }
    sb_putc(d->sb, '>');
}

static void ad_path(AdState *d, const Path *p)
{
    uint32_t i;
    for (i = 0; i < p->nsegs; i++) {
        if (i)
            sb_puts(d->sb, "::");
        ad_name(d, p->segs[i].name);
        if (p->segs[i].has_args)
            ad_gargs(d, p->segs[i].args, p->segs[i].nargs);
    }
}

static void ad_opt(AdState *d, const char *label, const Expr *e)
{
    if (!e)
        return;
    sb_printf(d->sb, " %s ", label);
    ad_expr(d, e);
}

/* ------------------------------------------------------------ types */

static void ad_field(AdState *d, const Field *f)
{
    sb_puts(d->sb, f->flip ? "(flip " : "(field ");
    ad_name(d, f->name);
    sb_putc(d->sb, ' ');
    ad_type(d, f->type);
    ad_attrs(d, f->attrs);
    ad_doc(d, f->doc);
    sb_putc(d->sb, ')');
}

static void ad_fields(AdState *d, Field *const *fields, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        sb_putc(d->sb, ' ');
        ad_field(d, fields[i]);
    }
}

static void ad_type(AdState *d, const Type *t)
{
    uint32_t i;
    if (!t) {
        sb_puts(d->sb, "<null>");
        return;
    }
    switch (t->kind) {
    case TYPE_ERROR:
        sb_puts(d->sb, "(error-type)");
        break;
    case TYPE_PATH:
        ad_path(d, &t->u.path);
        break;
    case TYPE_ARRAY:
        sb_puts(d->sb, "[");
        ad_type(d, t->u.array.elem);
        sb_puts(d->sb, "; ");
        ad_expr(d, t->u.array.len);
        sb_puts(d->sb, "]");
        break;
    case TYPE_STRUCT:
        sb_puts(d->sb, "(struct-type");
        ad_fields(d, t->u.anon_struct.fields, t->u.anon_struct.nfields);
        sb_putc(d->sb, ')');
        break;
    case TYPE_TYPEOF:
        sb_puts(d->sb, "(typeof ");
        ad_expr(d, t->u.typeof_expr);
        sb_putc(d->sb, ')');
        break;
    case TYPE_TUPLE:
        sb_puts(d->sb, "(tuple-type");
        for (i = 0; i < t->u.tuple.n; i++) {
            sb_putc(d->sb, ' ');
            ad_type(d, t->u.tuple.elems[i]);
        }
        sb_putc(d->sb, ')');
        break;
    }
}

/* ------------------------------------------------------------ expressions */

static void ad_field_inits(AdState *d, const FieldInit *f, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        sb_putc(d->sb, ' ');
        if (f[i].value) {
            sb_putc(d->sb, '(');
            ad_name(d, f[i].name);
            sb_putc(d->sb, ' ');
            ad_expr(d, f[i].value);
            sb_putc(d->sb, ')');
        } else {
            ad_name(d, f[i].name);
        }
    }
}

static void ad_expr(AdState *d, const Expr *e)
{
    uint32_t i;
    if (!e) {
        sb_puts(d->sb, "<null>");
        return;
    }
    switch (e->kind) {
    case EXPR_ERROR:
        sb_puts(d->sb, "(error)");
        break;
    case EXPR_INT:
        if (e->u.int_lit.suffix) {
            sb_printf(d->sb, "(int %s %c ", e->u.int_lit.text, e->u.int_lit.suffix);
            ad_expr(d, e->u.int_lit.width);
            sb_putc(d->sb, ')');
        } else {
            sb_puts(d->sb, e->u.int_lit.text);
        }
        break;
    case EXPR_BOOL:
        sb_puts(d->sb, e->u.bool_val ? "true" : "false");
        break;
    case EXPR_FILL:
        sb_puts(d->sb, e->u.fill_ones ? "all1" : "all0");
        break;
    case EXPR_STRING:
        ad_str(d, e->u.string.value, e->u.string.len);
        break;
    case EXPR_PATH:
        ad_path(d, &e->u.path);
        break;
    case EXPR_VARIANT:
        if (e->u.variant.has_fields) {
            sb_puts(d->sb, "(.");
            ad_name(d, e->u.variant.name);
            ad_field_inits(d, e->u.variant.fields, e->u.variant.nfields);
            sb_putc(d->sb, ')');
        } else {
            sb_putc(d->sb, '.');
            ad_name(d, e->u.variant.name);
        }
        break;
    case EXPR_STRUCT:
        sb_puts(d->sb, "(struct ");
        ad_path(d, &e->u.struct_lit.path);
        ad_field_inits(d, e->u.struct_lit.fields, e->u.struct_lit.nfields);
        if (e->u.struct_lit.has_rest)
            sb_puts(d->sb, " ..");
        sb_putc(d->sb, ')');
        break;
    case EXPR_FIELD:
        sb_puts(d->sb, "(. ");
        ad_expr(d, e->u.field.base);
        sb_putc(d->sb, ' ');
        ad_name(d, e->u.field.name);
        if (e->u.field.has_args)
            ad_gargs(d, e->u.field.args, e->u.field.nargs);
        sb_putc(d->sb, ')');
        break;
    case EXPR_INDEX:
        sb_puts(d->sb, "(index ");
        ad_expr(d, e->u.index.base);
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.index.index);
        sb_putc(d->sb, ')');
        break;
    case EXPR_SLICE:
        sb_puts(d->sb, "(slice ");
        ad_expr(d, e->u.slice.base);
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.slice.hi);
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.slice.lo);
        sb_putc(d->sb, ')');
        break;
    case EXPR_CALL:
        sb_puts(d->sb, "(call ");
        ad_expr(d, e->u.call.callee);
        for (i = 0; i < e->u.call.nargs; i++) {
            sb_putc(d->sb, ' ');
            if (e->u.call.args[i].name.name) {
                ad_name(d, e->u.call.args[i].name);
                sb_puts(d->sb, ": ");
            }
            ad_expr(d, e->u.call.args[i].value);
        }
        ad_opt(d, "@", e->u.call.domain);
        sb_putc(d->sb, ')');
        break;
    case EXPR_CAST:
        sb_puts(d->sb, "(as ");
        ad_expr(d, e->u.cast.expr);
        sb_putc(d->sb, ' ');
        ad_type(d, e->u.cast.type);
        sb_putc(d->sb, ')');
        break;
    case EXPR_UNARY: {
        static const char *const names[] = {"neg", "not", "~", "red&", "red|", "red^"};
        sb_printf(d->sb, "(%s ", names[e->u.unary.op]);
        ad_expr(d, e->u.unary.operand);
        sb_putc(d->sb, ')');
        break;
    }
    case EXPR_BINARY:
        sb_printf(d->sb, "(%s ", ast_binary_op_text(e->u.binary.op));
        ad_expr(d, e->u.binary.lhs);
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.binary.rhs);
        sb_putc(d->sb, ')');
        break;
    case EXPR_RANGE:
        sb_puts(d->sb, e->u.range.inclusive ? "(..= " : "(.. ");
        if (e->u.range.lo)
            ad_expr(d, e->u.range.lo);
        else
            sb_putc(d->sb, '_');
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.range.hi);
        sb_putc(d->sb, ')');
        break;
    case EXPR_IF:
        sb_puts(d->sb, "(if ");
        ad_expr(d, e->u.if_expr.cond);
        sb_putc(d->sb, ' ');
        ad_block(d, e->u.if_expr.then_block);
        if (e->u.if_expr.else_expr) {
            sb_puts(d->sb, " else ");
            ad_expr(d, e->u.if_expr.else_expr);
        }
        sb_putc(d->sb, ')');
        break;
    case EXPR_MATCH:
        sb_puts(d->sb, "(match ");
        ad_expr(d, e->u.match.scrutinee);
        d->indent++;
        for (i = 0; i < e->u.match.narms; i++) {
            const MatchArm *a = &e->u.match.arms[i];
            ad_nl(d);
            sb_puts(d->sb, "(arm ");
            ad_pat(d, a->pat);
            sb_puts(d->sb, " =>");
            if (a->body->kind == STMT_EXPR) {
                sb_putc(d->sb, ' ');
                ad_expr(d, a->body->u.expr);
            } else {
                d->indent++;
                ad_nl(d);
                ad_stmt(d, a->body);
                d->indent--;
            }
            sb_putc(d->sb, ')');
        }
        d->indent--;
        sb_putc(d->sb, ')');
        break;
    case EXPR_BLOCK:
        ad_block(d, e->u.block);
        break;
    case EXPR_TUPLE:
    case EXPR_ARRAY:
        sb_puts(d->sb, e->kind == EXPR_TUPLE ? "(tuple" : "(array");
        for (i = 0; i < e->u.list.n; i++) {
            sb_putc(d->sb, ' ');
            ad_expr(d, e->u.list.items[i]);
        }
        sb_putc(d->sb, ')');
        break;
    case EXPR_PAREN:
        sb_puts(d->sb, "(paren ");
        ad_expr(d, e->u.inner);
        sb_putc(d->sb, ')');
        break;
    case EXPR_SIM:
        sb_puts(d->sb, "(sim ");
        ad_expr(d, e->u.inner);
        sb_putc(d->sb, ')');
        break;
    case EXPR_TYPE:
        sb_puts(d->sb, "(type ");
        ad_type(d, e->u.type);
        sb_putc(d->sb, ')');
        break;
    case EXPR_TEMPORAL: {
        static const char *const names[] = {"always", "never", "eventually"};
        sb_printf(d->sb, "(%s ", names[e->u.temporal.op]);
        if (e->u.temporal.bound) {
            sb_puts(d->sb, "..");
            ad_expr(d, e->u.temporal.bound);
            sb_putc(d->sb, ' ');
        }
        ad_expr(d, e->u.temporal.body);
        sb_putc(d->sb, ')');
        break;
    }
    case EXPR_DELAY:
        if (e->u.delay.ranged) {
            sb_puts(d->sb, "(##[");
            ad_expr(d, e->u.delay.lo);
            sb_putc(d->sb, ':');
            if (e->u.delay.unbounded)
                sb_putc(d->sb, '$');
            else
                ad_expr(d, e->u.delay.hi);
            sb_puts(d->sb, "] ");
        } else {
            sb_puts(d->sb, "(## ");
            ad_expr(d, e->u.delay.lo);
            sb_putc(d->sb, ' ');
        }
        if (e->u.delay.lhs)
            ad_expr(d, e->u.delay.lhs);
        else
            sb_putc(d->sb, '_');
        sb_putc(d->sb, ' ');
        ad_expr(d, e->u.delay.rhs);
        sb_putc(d->sb, ')');
        break;
    }
}

/* ------------------------------------------------------------ patterns */

static void ad_field_pats(AdState *d, const FieldPat *f, uint32_t n, int rest)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        sb_putc(d->sb, ' ');
        if (f[i].pat) {
            sb_putc(d->sb, '(');
            ad_name(d, f[i].name);
            sb_putc(d->sb, ' ');
            ad_pat(d, f[i].pat);
            sb_putc(d->sb, ')');
        } else {
            ad_name(d, f[i].name);
        }
    }
    if (rest)
        sb_puts(d->sb, " ..");
}

static void ad_pat(AdState *d, const Pattern *p)
{
    uint32_t i;
    if (!p) {
        sb_puts(d->sb, "<null>");
        return;
    }
    switch (p->kind) {
    case PAT_ERROR:
        sb_puts(d->sb, "(error-pat)");
        break;
    case PAT_WILDCARD:
        sb_putc(d->sb, '_');
        break;
    case PAT_BIND:
        ad_name(d, p->u.bind);
        break;
    case PAT_LIT:
        ad_expr(d, p->u.lit);
        break;
    case PAT_RANGE:
        sb_puts(d->sb, p->u.range.inclusive ? "(..= " : "(.. ");
        ad_expr(d, p->u.range.lo);
        sb_putc(d->sb, ' ');
        ad_expr(d, p->u.range.hi);
        sb_putc(d->sb, ')');
        break;
    case PAT_VARIANT:
        if (p->u.variant.has_fields) {
            sb_puts(d->sb, "(.");
            ad_name(d, p->u.variant.name);
            ad_field_pats(d, p->u.variant.fields, p->u.variant.nfields, p->u.variant.has_rest);
            sb_putc(d->sb, ')');
        } else {
            sb_putc(d->sb, '.');
            ad_name(d, p->u.variant.name);
        }
        break;
    case PAT_PATH:
        ad_path(d, &p->u.path);
        break;
    case PAT_STRUCT:
        sb_puts(d->sb, "(struct-pat ");
        ad_path(d, &p->u.struct_pat.path);
        ad_field_pats(d, p->u.struct_pat.fields, p->u.struct_pat.nfields, p->u.struct_pat.has_rest);
        sb_putc(d->sb, ')');
        break;
    case PAT_TUPLE:
    case PAT_OR:
        sb_puts(d->sb, p->kind == PAT_TUPLE ? "(tuple-pat" : "(|");
        for (i = 0; i < p->u.list.n; i++) {
            sb_putc(d->sb, ' ');
            ad_pat(d, p->u.list.items[i]);
        }
        sb_putc(d->sb, ')');
        break;
    }
}

/* ------------------------------------------------------------ statements */

static void ad_block(AdState *d, const Block *b)
{
    uint32_t i;
    if (!b) {
        sb_puts(d->sb, "<null>");
        return;
    }
    sb_puts(d->sb, "(block");
    d->indent++;
    for (i = 0; i < b->nstmts; i++) {
        ad_nl(d);
        if (b->tail && i + 1 == b->nstmts) {
            sb_puts(d->sb, "(tail ");
            ad_expr(d, b->tail);
            sb_putc(d->sb, ')');
        } else {
            ad_stmt(d, b->stmts[i]);
        }
    }
    d->indent--;
    sb_putc(d->sb, ')');
}

static void ad_stmt_head(AdState *d, const char *head, const Stmt *s)
{
    sb_printf(d->sb, "(%s", head);
    ad_pos(d, s->span);
    ad_attrs(d, s->attrs);
    if (s->kind != STMT_ITEM)
        ad_doc(d, s->doc);
}

static void ad_stmt(AdState *d, const Stmt *s)
{
    switch (s->kind) {
    case STMT_ERROR:
        ad_stmt_head(d, "error-stmt", s);
        break;
    case STMT_PORT:
        ad_stmt_head(d, s->u.port.dir == PORT_IN ? "in" : "out", s);
        sb_putc(d->sb, ' ');
        ad_name(d, s->u.port.name);
        sb_putc(d->sb, ' ');
        ad_type(d, s->u.port.type);
        ad_opt(d, "@", s->u.port.domain);
        break;
    case STMT_DOMAIN:
        ad_stmt_head(d, "domain", s);
        sb_putc(d->sb, ' ');
        ad_name(d, s->u.domain.name);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.domain.clock);
        if (s->u.domain.reset) {
            sb_putc(d->sb, ' ');
            ad_expr(d, s->u.domain.reset);
        }
        break;
    case STMT_LET:
        ad_stmt_head(d, "let", s);
        sb_putc(d->sb, ' ');
        ad_pat(d, s->u.let.pat);
        if (s->u.let.type) {
            sb_puts(d->sb, " : ");
            ad_type(d, s->u.let.type);
        }
        ad_opt(d, "=", s->u.let.init);
        ad_opt(d, "where", s->u.let.constraint);
        break;
    case STMT_VAR:
        ad_stmt_head(d, "var", s);
        sb_putc(d->sb, ' ');
        ad_name(d, s->u.var.name);
        if (s->u.var.type) {
            sb_puts(d->sb, " : ");
            ad_type(d, s->u.var.type);
        }
        ad_opt(d, "=", s->u.var.init);
        ad_opt(d, "where", s->u.var.constraint);
        break;
    case STMT_REG:
        ad_stmt_head(d, "reg", s);
        sb_putc(d->sb, ' ');
        ad_name(d, s->u.reg.name);
        sb_puts(d->sb, " : ");
        ad_type(d, s->u.reg.type);
        ad_opt(d, "=", s->u.reg.init);
        ad_opt(d, "@", s->u.reg.domain);
        break;
    case STMT_MEM:
        ad_stmt_head(d, "mem", s);
        sb_putc(d->sb, ' ');
        ad_name(d, s->u.mem.name);
        sb_puts(d->sb, " : ");
        ad_type(d, s->u.mem.type);
        ad_opt(d, "=", s->u.mem.init);
        break;
    case STMT_ITEM:
        ad_item(d, s->u.item);
        return;
    case STMT_COMB:
        ad_stmt_head(d, "comb", s);
        sb_putc(d->sb, ' ');
        ad_block(d, s->u.comb);
        break;
    case STMT_ON:
        ad_stmt_head(d, "on", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.on.clock);
        sb_putc(d->sb, ' ');
        ad_block(d, s->u.on.body);
        break;
    case STMT_ASSIGN:
        ad_stmt_head(d, "=", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.assign.lhs);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.assign.rhs);
        ad_opt(d, "where", s->u.assign.constraint);
        break;
    case STMT_EXPR:
        ad_stmt_head(d, "expr", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.expr);
        break;
    case STMT_FOR:
        ad_stmt_head(d, "for", s);
        sb_putc(d->sb, ' ');
        ad_pat(d, s->u.for_stmt.pat);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.for_stmt.iter);
        sb_putc(d->sb, ' ');
        ad_block(d, s->u.for_stmt.body);
        break;
    case STMT_WHILE:
        ad_stmt_head(d, "while", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.while_stmt.cond);
        sb_putc(d->sb, ' ');
        ad_block(d, s->u.while_stmt.body);
        break;
    case STMT_ASSERT:
        ad_stmt_head(d, s->u.assert_stmt.kind == ASSERT_EXPECT ? "expect" : "assert", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.assert_stmt.cond);
        if (s->u.assert_stmt.msg) {
            sb_putc(d->sb, ' ');
            ad_expr(d, s->u.assert_stmt.msg);
        }
        break;
    case STMT_STATIC_ASSERT:
        ad_stmt_head(d, "static_assert", s);
        sb_putc(d->sb, ' ');
        ad_expr(d, s->u.static_assert.cond);
        if (s->u.static_assert.msg) {
            sb_putc(d->sb, ' ');
            ad_expr(d, s->u.static_assert.msg);
        }
        break;
    case STMT_RETURN:
        ad_stmt_head(d, "return", s);
        if (s->u.ret) {
            sb_putc(d->sb, ' ');
            ad_expr(d, s->u.ret);
        }
        break;
    }
    sb_putc(d->sb, ')');
}

/* ------------------------------------------------------------ items */

static void ad_generics(AdState *d, const GenericParams *g)
{
    uint32_t i;
    if (!g->present)
        return;
    sb_puts(d->sb, " (generics");
    for (i = 0; i < g->n; i++) {
        const GenericParam *gp = &g->items[i];
        sb_puts(d->sb, " (");
        ad_name(d, gp->name);
        if (gp->kind == GENERIC_TYPE) {
            sb_puts(d->sb, " type");
            if (gp->default_type) {
                sb_puts(d->sb, " = ");
                ad_type(d, gp->default_type);
            }
        } else {
            sb_putc(d->sb, ' ');
            ad_type(d, gp->type);
            ad_opt(d, "=", gp->default_value);
        }
        sb_putc(d->sb, ')');
    }
    sb_putc(d->sb, ')');
}

static void ad_where(AdState *d, const WhereList *w)
{
    uint32_t i;
    if (!w->n)
        return;
    sb_puts(d->sb, " (where");
    for (i = 0; i < w->n; i++) {
        sb_putc(d->sb, ' ');
        ad_expr(d, w->items[i]);
    }
    sb_putc(d->sb, ')');
}

static void ad_body(AdState *d, Stmt *const *body, uint32_t n)
{
    uint32_t i;
    d->indent++;
    for (i = 0; i < n; i++) {
        ad_nl(d);
        ad_stmt(d, body[i]);
    }
    d->indent--;
}

static void ad_item(AdState *d, const Item *it)
{
    static const char *const heads[] = {"error-item", "module", "extern", "bundle", "struct", "enum", "const",
                                        "fn", "type", "regmap", "test", "property", "sequence", "static_assert"};
    uint32_t i;
    sb_printf(d->sb, "(%s", heads[it->kind]);
    ad_pos(d, it->span);
    if (it->name.name) {
        sb_putc(d->sb, ' ');
        ad_name(d, it->name);
    }
    if (it->is_pub)
        sb_puts(d->sb, " pub");
    ad_attrs(d, it->attrs);
    ad_doc(d, it->doc);
    switch (it->kind) {
    case ITEM_ERROR:
        break;
    case ITEM_MODULE:
        ad_generics(d, &it->u.module.generics);
        ad_where(d, &it->u.module.where);
        ad_body(d, it->u.module.body, it->u.module.nbody);
        break;
    case ITEM_EXTERN:
        sb_putc(d->sb, ' ');
        ad_str(d, it->u.extern_mod.file ? it->u.extern_mod.file : "", it->u.extern_mod.file ? strlen(it->u.extern_mod.file) : 0);
        ad_generics(d, &it->u.extern_mod.generics);
        ad_where(d, &it->u.extern_mod.where);
        ad_body(d, it->u.extern_mod.body, it->u.extern_mod.nbody);
        break;
    case ITEM_BUNDLE:
        ad_generics(d, &it->u.bundle.generics);
        d->indent++;
        for (i = 0; i < it->u.bundle.nfields; i++) {
            ad_nl(d);
            ad_field(d, it->u.bundle.fields[i]);
        }
        for (i = 0; i < it->u.bundle.nfns; i++) {
            ad_nl(d);
            ad_item(d, it->u.bundle.fns[i]);
        }
        d->indent--;
        break;
    case ITEM_STRUCT:
        ad_generics(d, &it->u.struct_def.generics);
        d->indent++;
        for (i = 0; i < it->u.struct_def.nfields; i++) {
            ad_nl(d);
            ad_field(d, it->u.struct_def.fields[i]);
        }
        d->indent--;
        break;
    case ITEM_ENUM:
        ad_generics(d, &it->u.enum_def.generics);
        d->indent++;
        for (i = 0; i < it->u.enum_def.nvariants; i++) {
            const Variant *v = it->u.enum_def.variants[i];
            ad_nl(d);
            sb_puts(d->sb, "(variant ");
            ad_name(d, v->name);
            ad_attrs(d, v->attrs);
            ad_doc(d, v->doc);
            if (v->has_fields) {
                sb_puts(d->sb, " {");
                ad_fields(d, v->fields, v->nfields);
                sb_puts(d->sb, " }");
            }
            sb_putc(d->sb, ')');
        }
        d->indent--;
        break;
    case ITEM_CONST:
        if (it->u.const_def.type) {
            sb_puts(d->sb, " : ");
            ad_type(d, it->u.const_def.type);
        }
        ad_opt(d, "=", it->u.const_def.value);
        break;
    case ITEM_FN:
        if (it->u.fn.is_const)
            sb_puts(d->sb, " const");
        ad_generics(d, &it->u.fn.generics);
        sb_puts(d->sb, " (params");
        for (i = 0; i < it->u.fn.nparams; i++) {
            sb_puts(d->sb, " (");
            ad_name(d, it->u.fn.params[i].name);
            sb_putc(d->sb, ' ');
            ad_type(d, it->u.fn.params[i].type);
            sb_putc(d->sb, ')');
        }
        sb_putc(d->sb, ')');
        if (it->u.fn.ret) {
            sb_puts(d->sb, " -> ");
            ad_type(d, it->u.fn.ret);
        }
        ad_where(d, &it->u.fn.where);
        d->indent++;
        ad_nl(d);
        ad_block(d, it->u.fn.body);
        d->indent--;
        break;
    case ITEM_TYPE:
        ad_generics(d, &it->u.type_alias.generics);
        sb_puts(d->sb, " = ");
        ad_type(d, it->u.type_alias.type);
        break;
    case ITEM_REGMAP:
        d->indent++;
        for (i = 0; i < it->u.regmap.nentries; i++) {
            const RegEntry *r = it->u.regmap.entries[i];
            uint32_t j;
            ad_nl(d);
            sb_puts(d->sb, "(reg");
            ad_pos(d, r->span);
            ad_attrs(d, r->attrs);
            ad_doc(d, r->doc);
            sb_putc(d->sb, ' ');
            ad_expr(d, r->offset);
            sb_putc(d->sb, ' ');
            ad_name(d, r->name);
            sb_putc(d->sb, ' ');
            ad_name(d, r->access);
            if (r->kind == REG_FIELDS) {
                for (j = 0; j < r->nfields; j++) {
                    const RegField *f = r->fields[j];
                    sb_puts(d->sb, " (");
                    ad_name(d, f->name);
                    sb_putc(d->sb, ' ');
                    ad_type(d, f->type);
                    ad_opt(d, "@", f->bit_hi);
                    if (f->bit_lo) {
                        sb_putc(d->sb, ' ');
                        ad_expr(d, f->bit_lo);
                    }
                    ad_attrs(d, f->attrs);
                    ad_doc(d, f->doc);
                    sb_putc(d->sb, ')');
                }
            } else {
                sb_putc(d->sb, ' ');
                ad_type(d, r->type);
                if (r->kind == REG_STREAM) {
                    sb_puts(d->sb, " -> ");
                    ad_type(d, r->stream_type);
                }
            }
            ad_opt(d, "=", r->reset);
            sb_putc(d->sb, ')');
        }
        d->indent--;
        break;
    case ITEM_TEST:
        sb_putc(d->sb, ' ');
        if (it->u.test.name)
            ad_str(d, it->u.test.name, strlen(it->u.test.name));
        if (it->u.test.body) {
            d->indent++;
            ad_nl(d);
            ad_block(d, it->u.test.body);
            d->indent--;
        }
        break;
    case ITEM_PROPERTY: {
        static const char *const kinds[] = {"", " assert", " assume", " cover"};
        sb_puts(d->sb, kinds[it->u.property.kind]);
        sb_putc(d->sb, ' ');
        ad_expr(d, it->u.property.body);
        break;
    }
    case ITEM_SEQUENCE:
        sb_putc(d->sb, ' ');
        ad_expr(d, it->u.sequence.body);
        break;
    case ITEM_STATIC_ASSERT:
        sb_putc(d->sb, ' ');
        ad_expr(d, it->u.static_assert.cond);
        if (it->u.static_assert.msg) {
            sb_putc(d->sb, ' ');
            ad_expr(d, it->u.static_assert.msg);
        }
        break;
    }
    sb_putc(d->sb, ')');
}

static void ad_use_tree(AdState *d, const UseTree *u)
{
    uint32_t i;
    for (i = 0; i < u->npath; i++) {
        if (i)
            sb_puts(d->sb, "::");
        ad_name(d, u->path[i]);
    }
    if (u->glob)
        sb_puts(d->sb, "::*");
    if (u->is_group) {
        sb_puts(d->sb, "::{");
        for (i = 0; i < u->ngroup; i++) {
            if (i)
                sb_puts(d->sb, ", ");
            ad_use_tree(d, u->group[i]);
        }
        sb_putc(d->sb, '}');
    }
    if (u->alias.name) {
        sb_puts(d->sb, " as ");
        ad_name(d, u->alias);
    }
}

void ast_dump(const AstFile *f, StrBuf *out)
{
    AdState d;
    uint32_t i;
    d.sb = out;
    d.indent = 1;
    sb_puts(out, "(file");
    if (f->has_package) {
        ad_nl(&d);
        sb_puts(out, "(package ");
        for (i = 0; i < f->npackage; i++) {
            if (i)
                sb_puts(out, "::");
            ad_name(&d, f->package[i]);
        }
        sb_putc(out, ')');
    }
    for (i = 0; i < f->nuses; i++) {
        const UseDecl *u = f->uses[i];
        ad_nl(&d);
        sb_puts(out, "(use");
        ad_pos(&d, u->span);
        if (u->is_pub)
            sb_puts(out, " pub");
        ad_doc(&d, u->doc);
        sb_putc(out, ' ');
        ad_use_tree(&d, u->tree);
        sb_putc(out, ')');
    }
    for (i = 0; i < f->nitems; i++) {
        ad_nl(&d);
        ad_item(&d, f->items[i]);
    }
    ad_nl(&d);
    sb_printf(out, "(comments %u))\n", (unsigned)f->ncomments);
}

void ast_dump_expr(const Expr *e, StrBuf *out)
{
    AdState d;
    d.sb = out;
    d.indent = 0;
    ad_expr(&d, e);
}

void ast_dump_type(const Type *t, StrBuf *out)
{
    AdState d;
    d.sb = out;
    d.indent = 0;
    ad_type(&d, t);
}
