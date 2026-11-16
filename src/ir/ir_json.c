/* ir_json.c — IR <-> JSON (schema: docs/ir-schema.md).
 *
 * The writer is deterministic: the same design always produces the same
 * text, so `write -> read -> write` is the identity on the text.  Types are
 * written once, in a table, children before parents, and referenced by
 * index everywhere else.
 */
#include "ir.h"
#include "../base/str.h"
#include "../base/map.h"
#include "../base/json.h"

uint32_t irb_cap(uint32_t n);
void *irb_grow(Arena *a, void *arr, uint32_t n, size_t elem);

#define IRJ_FORMAT "tern-ir"
#define IRJ_VERSION 1

/* ============================================================== writer == */

typedef struct IrjWriter {
    JsonWriter w;
    Map type_ids;          /* IrType* -> id */
    IrType **types;        /* in id order */
    uint32_t ntypes, cap;
    StrBuf tmp;
} IrjWriter;

static void irj_collect_type(IrjWriter *jw, IrType *t)
{
    uint32_t i;
    if (!t || map_has(&jw->type_ids, MAP_PTR(t)))
        return;
    /* children first */
    if (t->elem)
        irj_collect_type(jw, t->elem);
    for (i = 0; i < t->nfields; i++)
        irj_collect_type(jw, t->fields[i].type);
    for (i = 0; i < t->nvariants; i++)
        irj_collect_type(jw, t->variants[i].payload);
    if (map_has(&jw->type_ids, MAP_PTR(t)))
        return;
    if (jw->ntypes == jw->cap) {
        jw->cap = jw->cap ? jw->cap * 2 : 32;
        jw->types = (IrType **)xrealloc(jw->types, sizeof(IrType *) * jw->cap);
    }
    map_put(&jw->type_ids, MAP_PTR(t), jw->ntypes);
    jw->types[jw->ntypes++] = t;
}

static void irj_collect_expr(IrjWriter *jw, const IrExpr *e)
{
    uint32_t i;
    if (!e)
        return;
    irj_collect_type(jw, e->type);
    for (i = 0; i < e->nargs; i++)
        irj_collect_expr(jw, e->args[i]);
}

static void irj_collect_block(IrjWriter *jw, const IrBlock *b)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        irj_collect_expr(jw, s->lhs.index);
        irj_collect_expr(jw, s->rhs);
        irj_collect_expr(jw, s->cond);
        irj_collect_block(jw, &s->then_b);
        irj_collect_block(jw, &s->else_b);
        irj_collect_expr(jw, s->subject);
        for (j = 0; j < s->narms; j++)
            irj_collect_block(jw, &s->arms[j].body);
        irj_collect_expr(jw, s->addr);
        irj_collect_expr(jw, s->data);
        irj_collect_expr(jw, s->mask);
    }
}

static void irj_collect_seq(IrjWriter *jw, const IrSeq *q)
{
    if (!q)
        return;
    irj_collect_expr(jw, q->expr);
    irj_collect_seq(jw, q->a);
    irj_collect_seq(jw, q->b);
}

static void irj_collect(IrjWriter *jw, const IrDesign *d)
{
    uint32_t i, j, k;
    for (i = 0; i < d->ntypes; i++)
        irj_collect_type(jw, d->types[i]);
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        for (j = 0; j < m->nsigs; j++)
            irj_collect_type(jw, m->sigs[j].type);
        for (j = 0; j < m->nassigns; j++) {
            irj_collect_expr(jw, m->assigns[j].lhs.index);
            irj_collect_expr(jw, m->assigns[j].rhs);
        }
        for (j = 0; j < m->nprocs; j++)
            irj_collect_block(jw, &m->procs[j].body);
        for (j = 0; j < m->ninsts; j++)
            for (k = 0; k < m->insts[j].nconns; k++)
                irj_collect_expr(jw, m->insts[j].conns[k].expr);
        for (j = 0; j < m->nprops; j++)
            irj_collect_seq(jw, m->props[j].seq);
    }
    for (i = 0; i < d->nregmaps; i++)
        for (j = 0; j < d->regmaps[i].nregs; j++)
            for (k = 0; k < d->regmaps[i].regs[j].nfields; k++)
                irj_collect_type(jw, d->regmaps[i].regs[j].fields[k].type);
}

static uint32_t irj_type_id(IrjWriter *jw, const IrType *t)
{
    uint64_t id = 0;
    if (!map_get(&jw->type_ids, MAP_PTR(t), &id))
        tern_fatal("ir_to_json: uncollected type");
    return (uint32_t)id;
}

static void irj_kv_type(IrjWriter *jw, const char *key, const IrType *t)
{
    jw_key(&jw->w, key);
    if (t)
        jw_uint(&jw->w, irj_type_id(jw, t));
    else
        jw_null(&jw->w);
}

static void irj_bits(IrjWriter *jw, const IrBits *b)
{
    sb_reset(&jw->tmp);
    ir_bits_print(b, &jw->tmp);
    jw_str(&jw->w, sb_cstr(&jw->tmp));
}

static void irj_kv_bits(IrjWriter *jw, const char *key, const IrBits *b)
{
    jw_key(&jw->w, key);
    irj_bits(jw, b);
}

static void irj_kv_idx(IrjWriter *jw, const char *key, uint32_t v)
{
    jw_key(&jw->w, key);
    if (v == IR_NONE)
        jw_null(&jw->w);
    else
        jw_uint(&jw->w, v);
}

static void irj_span(IrjWriter *jw, Span s)
{
    const SourceFile *f;
    if (s.file == 0)
        return;
    f = src_get(s.file);
    jw_key(&jw->w, "loc");
    jw_object_begin(&jw->w);
    jw_kv_str(&jw->w, "file", f ? f->path : "");
    jw_kv_uint(&jw->w, "line", s.line);
    jw_kv_uint(&jw->w, "col", s.col);
    jw_kv_uint(&jw->w, "off", s.off);
    jw_kv_uint(&jw->w, "len", s.len);
    jw_object_end(&jw->w);
}

static void irj_attrs(IrjWriter *jw, uint32_t n, const IrAttr *a)
{
    uint32_t i;
    if (!n)
        return;
    jw_key(&jw->w, "attrs");
    jw_array_begin(&jw->w);
    for (i = 0; i < n; i++) {
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", a[i].name);
        jw_kv_str(&jw->w, "args", a[i].args);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);
}

static void irj_expr(IrjWriter *jw, const IrExpr *e)
{
    uint32_t i;
    if (!e) {
        jw_null(&jw->w);
        return;
    }
    jw_object_begin(&jw->w);
    jw_kv_str(&jw->w, "k", ir_expr_kind_name(e->kind));
    jw_kv_uint(&jw->w, "w", e->width);
    if (e->kind == IRE_UNARY || e->kind == IRE_BINARY)
        jw_kv_str(&jw->w, "op", ir_op_name(e->op));
    if (e->is_signed)
        jw_kv_bool(&jw->w, "signed", 1);
    if (e->type)
        irj_kv_type(jw, "type", e->type);
    switch (e->kind) {
    case IRE_CONST: irj_kv_bits(jw, "v", &e->bits); break;
    case IRE_SIG:
    case IRE_MEMREAD: jw_kv_uint(&jw->w, "sig", e->sig); break;
    case IRE_SLICE:
        jw_kv_uint(&jw->w, "lo", e->lo);
        if (e->field)
            jw_kv_str(&jw->w, "field", e->field);
        break;
    case IRE_PAST: jw_kv_uint(&jw->w, "lo", e->lo); break;
    default: break;
    }
    if (e->nargs) {
        jw_key(&jw->w, "args");
        jw_array_begin(&jw->w);
        for (i = 0; i < e->nargs; i++)
            irj_expr(jw, e->args[i]);
        jw_array_end(&jw->w);
    }
    irj_span(jw, e->loc);
    jw_object_end(&jw->w);
}

static void irj_lvalue(IrjWriter *jw, const IrLvalue *lv)
{
    jw_object_begin(&jw->w);
    jw_kv_uint(&jw->w, "sig", lv->sig);
    jw_kv_uint(&jw->w, "lo", lv->lo);
    jw_kv_uint(&jw->w, "w", lv->width);
    if (lv->index) {
        jw_key(&jw->w, "index");
        irj_expr(jw, lv->index);
    }
    if (lv->field)
        jw_kv_str(&jw->w, "field", lv->field);
    jw_object_end(&jw->w);
}

static void irj_block(IrjWriter *jw, const IrBlock *b);

static void irj_stmt(IrjWriter *jw, const IrStmt *s)
{
    uint32_t i, j;
    jw_object_begin(&jw->w);
    switch (s->kind) {
    case IRST_ASSIGN:
        jw_kv_str(&jw->w, "s", "assign");
        jw_key(&jw->w, "lhs");
        irj_lvalue(jw, &s->lhs);
        jw_key(&jw->w, "rhs");
        irj_expr(jw, s->rhs);
        break;
    case IRST_IF:
        jw_kv_str(&jw->w, "s", "if");
        jw_key(&jw->w, "cond");
        irj_expr(jw, s->cond);
        jw_key(&jw->w, "then");
        irj_block(jw, &s->then_b);
        jw_key(&jw->w, "else");
        irj_block(jw, &s->else_b);
        break;
    case IRST_CASE:
        jw_kv_str(&jw->w, "s", "case");
        jw_key(&jw->w, "subject");
        irj_expr(jw, s->subject);
        jw_key(&jw->w, "arms");
        jw_array_begin(&jw->w);
        for (i = 0; i < s->narms; i++) {
            jw_object_begin(&jw->w);
            jw_key(&jw->w, "vals");
            jw_array_begin(&jw->w);
            for (j = 0; j < s->arms[i].nvals; j++)
                irj_bits(jw, &s->arms[i].vals[j]);
            jw_array_end(&jw->w);
            jw_key(&jw->w, "body");
            irj_block(jw, &s->arms[i].body);
            jw_object_end(&jw->w);
        }
        jw_array_end(&jw->w);
        break;
    case IRST_MEMWRITE:
        jw_kv_str(&jw->w, "s", "memwrite");
        jw_kv_uint(&jw->w, "mem", s->mem);
        jw_key(&jw->w, "addr");
        irj_expr(jw, s->addr);
        jw_key(&jw->w, "data");
        irj_expr(jw, s->data);
        jw_key(&jw->w, "mask");
        irj_expr(jw, s->mask);
        break;
    case IRST_ASSERT:
        jw_kv_str(&jw->w, "s", "assert");
        jw_kv_str(&jw->w, "kind", ir_assert_kind_name(s->akind));
        jw_key(&jw->w, "cond");
        irj_expr(jw, s->cond);
        jw_kv_str(&jw->w, "msg", s->msg);
        break;
    }
    irj_span(jw, s->loc);
    jw_object_end(&jw->w);
}

static void irj_block(IrjWriter *jw, const IrBlock *b)
{
    uint32_t i;
    jw_array_begin(&jw->w);
    for (i = 0; i < b->n; i++)
        irj_stmt(jw, b->stmts[i]);
    jw_array_end(&jw->w);
}

static void irj_seq(IrjWriter *jw, const IrSeq *q)
{
    if (!q) {
        jw_null(&jw->w);
        return;
    }
    jw_object_begin(&jw->w);
    jw_kv_str(&jw->w, "q", ir_seq_kind_name(q->kind));
    switch (q->kind) {
    case IRQ_EXPR:
        jw_key(&jw->w, "expr");
        irj_expr(jw, q->expr);
        break;
    case IRQ_DELAY:
        jw_kv_uint(&jw->w, "lo", q->lo);
        irj_kv_idx(jw, "hi", q->hi);
        break;
    case IRQ_EVENTUALLY: irj_kv_idx(jw, "hi", q->hi); break;
    default: break;
    }
    if (q->a) {
        jw_key(&jw->w, "a");
        irj_seq(jw, q->a);
    }
    if (q->b) {
        jw_key(&jw->w, "b");
        irj_seq(jw, q->b);
    }
    jw_object_end(&jw->w);
}

static void irj_type(IrjWriter *jw, const IrType *t, uint32_t id)
{
    static const char *const kinds[] = {"bool", "bits",   "uint",  "sint", "enum",
                                        "struct", "array", "clock", "reset"};
    uint32_t i;
    jw_object_begin(&jw->w);
    jw_kv_uint(&jw->w, "id", id);
    jw_kv_str(&jw->w, "kind", kinds[t->kind]);
    jw_kv_uint(&jw->w, "width", t->width);
    if (t->name)
        jw_kv_str(&jw->w, "name", t->name);
    switch (t->kind) {
    case IRT_STRUCT:
        jw_key(&jw->w, "fields");
        jw_array_begin(&jw->w);
        for (i = 0; i < t->nfields; i++) {
            jw_object_begin(&jw->w);
            jw_kv_str(&jw->w, "name", t->fields[i].name);
            irj_kv_type(jw, "type", t->fields[i].type);
            jw_kv_uint(&jw->w, "lsb", t->fields[i].lsb);
            jw_object_end(&jw->w);
        }
        jw_array_end(&jw->w);
        break;
    case IRT_ENUM:
        jw_kv_uint(&jw->w, "tag_width", t->tag_width);
        jw_kv_str(&jw->w, "encoding", ir_encoding_name(t->encoding));
        jw_key(&jw->w, "variants");
        jw_array_begin(&jw->w);
        for (i = 0; i < t->nvariants; i++) {
            jw_object_begin(&jw->w);
            jw_kv_str(&jw->w, "name", t->variants[i].name);
            irj_kv_bits(jw, "tag", &t->variants[i].tag);
            irj_kv_type(jw, "payload", t->variants[i].payload);
            jw_object_end(&jw->w);
        }
        jw_array_end(&jw->w);
        break;
    case IRT_ARRAY:
        irj_kv_type(jw, "elem", t->elem);
        jw_kv_uint(&jw->w, "len", t->len);
        break;
    case IRT_CLOCK: jw_kv_bool(&jw->w, "falling", t->falling); break;
    case IRT_RESET:
        jw_kv_bool(&jw->w, "async", t->async);
        jw_kv_bool(&jw->w, "active_low", t->active_low);
        break;
    default: break;
    }
    irj_span(jw, t->loc);
    jw_object_end(&jw->w);
}

static void irj_module(IrjWriter *jw, const IrModule *m)
{
    uint32_t i, j;
    jw_object_begin(&jw->w);
    jw_kv_str(&jw->w, "name", m->name);
    jw_kv_str(&jw->w, "tern_name", m->tern_name);
    jw_kv_str(&jw->w, "display", m->display);
    jw_kv_str(&jw->w, "package", m->package);
    jw_kv_bool(&jw->w, "is_top", m->is_top);
    jw_kv_bool(&jw->w, "is_extern", m->is_extern);
    jw_kv_str(&jw->w, "extern_file", m->extern_file);
    jw_kv_bool(&jw->w, "is_cdc", m->is_cdc);
    if (m->is_cdc)
        jw_kv_str(&jw->w, "cdc_kind", ir_cdc_kind_name(m->cdc_kind));
    irj_attrs(jw, m->nattrs, m->attrs);
    jw_key(&jw->w, "params");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nparams; i++) {
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", m->params[i].name);
        irj_kv_bits(jw, "value", &m->params[i].value);
        jw_kv_bool(&jw->w, "signed", m->params[i].is_signed);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);

    jw_key(&jw->w, "signals");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", s->name);
        jw_kv_str(&jw->w, "path", s->path);
        jw_kv_str(&jw->w, "kind", ir_sig_kind_name(s->kind));
        irj_kv_type(jw, "type", s->type);
        irj_kv_idx(jw, "domain", s->domain);
        jw_key(&jw->w, "init");
        if (s->has_init)
            irj_bits(jw, &s->init);
        else
            jw_null(&jw->w);
        if (s->test_port)
            jw_kv_bool(&jw->w, "test_port", 1);
        if (s->flipped)
            jw_kv_bool(&jw->w, "flipped", 1);
        if (s->kind == IRS_MEM) {
            jw_kv_uint(&jw->w, "depth", s->depth);
            jw_key(&jw->w, "mem_init");
            if (s->mem_init) {
                jw_array_begin(&jw->w);
                for (j = 0; j < s->depth; j++)
                    irj_bits(jw, &s->mem_init[j]);
                jw_array_end(&jw->w);
            } else {
                jw_null(&jw->w);
            }
            jw_kv_str(&jw->w, "mem_init_file", s->mem_init_file);
            jw_kv_bool(&jw->w, "mem_init_is_bin", s->mem_init_is_bin);
        }
        irj_attrs(jw, s->nattrs, s->attrs);
        irj_span(jw, s->loc);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);

    jw_key(&jw->w, "ports");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nports; i++)
        jw_uint(&jw->w, m->ports[i]);
    jw_array_end(&jw->w);

    jw_key(&jw->w, "domains");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->ndomains; i++) {
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", m->domains[i].name ? m->domains[i].name : "");
        jw_kv_uint(&jw->w, "clock", m->domains[i].clock);
        irj_kv_idx(jw, "reset", m->domains[i].reset);
        jw_kv_uint(&jw->w, "hz", m->domains[i].hz);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);
    irj_kv_idx(jw, "default_domain", m->default_domain);

    jw_key(&jw->w, "assigns");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nassigns; i++) {
        jw_object_begin(&jw->w);
        jw_key(&jw->w, "lhs");
        irj_lvalue(jw, &m->assigns[i].lhs);
        jw_key(&jw->w, "rhs");
        irj_expr(jw, m->assigns[i].rhs);
        irj_span(jw, m->assigns[i].loc);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);

    jw_key(&jw->w, "processes");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nprocs; i++) {
        const IrProcess *p = &m->procs[i];
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "kind", p->kind == IRP_COMB ? "comb" : "clocked");
        irj_kv_idx(jw, "domain", p->domain);
        jw_key(&jw->w, "regs");
        jw_array_begin(&jw->w);
        for (j = 0; j < p->nregs; j++)
            jw_uint(&jw->w, p->regs[j]);
        jw_array_end(&jw->w);
        jw_key(&jw->w, "body");
        irj_block(jw, &p->body);
        irj_span(jw, p->loc);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);

    jw_key(&jw->w, "instances");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", in->name);
        jw_kv_str(&jw->w, "module", in->module ? in->module->name : NULL);
        jw_key(&jw->w, "conns");
        jw_array_begin(&jw->w);
        for (j = 0; j < in->nconns; j++) {
            jw_object_begin(&jw->w);
            jw_kv_uint(&jw->w, "port", in->conns[j].port);
            jw_key(&jw->w, "expr");
            irj_expr(jw, in->conns[j].expr);
            jw_object_end(&jw->w);
        }
        jw_array_end(&jw->w);
        irj_span(jw, in->loc);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);

    jw_key(&jw->w, "properties");
    jw_array_begin(&jw->w);
    for (i = 0; i < m->nprops; i++) {
        const IrProperty *p = &m->props[i];
        jw_object_begin(&jw->w);
        jw_kv_str(&jw->w, "name", p->name);
        jw_kv_str(&jw->w, "kind", ir_assert_kind_name(p->kind));
        irj_kv_idx(jw, "domain", p->domain);
        jw_key(&jw->w, "seq");
        irj_seq(jw, p->seq);
        irj_span(jw, p->loc);
        jw_object_end(&jw->w);
    }
    jw_array_end(&jw->w);
    irj_span(jw, m->loc);
    jw_object_end(&jw->w);
}

void ir_to_json(const IrDesign *d, StrBuf *out)
{
    IrjWriter jw;
    uint32_t i, j, k;
    memset(&jw, 0, sizeof jw);
    jw_init(&jw.w, out, 1);
    irj_collect(&jw, d);

    jw_object_begin(&jw.w);
    jw_kv_str(&jw.w, "format", IRJ_FORMAT);
    jw_kv_int(&jw.w, "version", IRJ_VERSION);
    jw_kv_str(&jw.w, "top", d->top ? d->top->name : NULL);
    jw_key(&jw.w, "types");
    jw_array_begin(&jw.w);
    for (i = 0; i < jw.ntypes; i++)
        irj_type(&jw, jw.types[i], i);
    jw_array_end(&jw.w);
    jw_key(&jw.w, "named_types");
    jw_array_begin(&jw.w);
    for (i = 0; i < d->ntypes; i++)
        jw_uint(&jw.w, irj_type_id(&jw, d->types[i]));
    jw_array_end(&jw.w);
    jw_key(&jw.w, "modules");
    jw_array_begin(&jw.w);
    for (i = 0; i < d->nmodules; i++)
        irj_module(&jw, d->modules[i]);
    jw_array_end(&jw.w);

    jw_key(&jw.w, "regmaps");
    jw_array_begin(&jw.w);
    for (i = 0; i < d->nregmaps; i++) {
        const IrRegmap *r = &d->regmaps[i];
        jw_object_begin(&jw.w);
        jw_kv_str(&jw.w, "name", r->name);
        jw_kv_str(&jw.w, "package", r->package);
        jw_key(&jw.w, "regs");
        jw_array_begin(&jw.w);
        for (j = 0; j < r->nregs; j++) {
            const IrReg *g = &r->regs[j];
            jw_object_begin(&jw.w);
            jw_kv_uint(&jw.w, "offset", g->offset);
            jw_kv_str(&jw.w, "name", g->name);
            jw_kv_str(&jw.w, "access", ir_access_name(g->access));
            jw_kv_uint(&jw.w, "width", g->width);
            jw_kv_bool(&jw.w, "is_stream", g->is_stream);
            jw_kv_str(&jw.w, "doc", g->doc);
            jw_key(&jw.w, "fields");
            jw_array_begin(&jw.w);
            for (k = 0; k < g->nfields; k++) {
                jw_object_begin(&jw.w);
                jw_kv_str(&jw.w, "name", g->fields[k].name);
                jw_kv_uint(&jw.w, "lo", g->fields[k].lo);
                jw_kv_uint(&jw.w, "width", g->fields[k].width);
                irj_kv_type(&jw, "type", g->fields[k].type);
                jw_object_end(&jw.w);
            }
            jw_array_end(&jw.w);
            jw_object_end(&jw.w);
        }
        jw_array_end(&jw.w);
        irj_span(&jw, r->loc);
        jw_object_end(&jw.w);
    }
    jw_array_end(&jw.w);

    jw_key(&jw.w, "crossings");
    jw_array_begin(&jw.w);
    for (i = 0; i < d->ncrossings; i++) {
        const IrCrossing *c = &d->crossings[i];
        jw_object_begin(&jw.w);
        jw_kv_str(&jw.w, "path", c->path);
        jw_kv_str(&jw.w, "kind", ir_cdc_kind_name(c->kind));
        jw_kv_str(&jw.w, "from_domain", c->from_domain);
        jw_kv_str(&jw.w, "to_domain", c->to_domain);
        jw_kv_str(&jw.w, "signal", c->signal);
        irj_span(&jw, c->loc);
        jw_object_end(&jw.w);
    }
    jw_array_end(&jw.w);
    jw_object_end(&jw.w);
    sb_putc(out, '\n');

    map_free(&jw.type_ids);
    free(jw.types);
    sb_free(&jw.tmp);
}

/* ============================================================== reader == */

typedef struct IrjReader {
    Arena *a;
    IrDesign *d;
    IrType **types;
    uint32_t ntypes;
    const IrModule *cur;   /* module being read (signal range checks) */
    int failed;
    char err[160];
} IrjReader;

static void irj_fail(IrjReader *r, const char *what)
{
    if (!r->failed) {
        r->failed = 1;
        snprintf(r->err, sizeof r->err, "%s", what);
    }
}

static const char *irj_str(IrjReader *r, const JsonValue *v)
{
    if (!v || v->kind == JSON_NULL)
        return NULL;
    if (v->kind != JSON_STRING) {
        irj_fail(r, "expected a string");
        return NULL;
    }
    return intern(v->str, v->len);
}

static const char *irj_text(IrjReader *r, const JsonValue *v)   /* arena copy, not interned */
{
    if (!v || v->kind == JSON_NULL)
        return NULL;
    if (v->kind != JSON_STRING) {
        irj_fail(r, "expected a string");
        return NULL;
    }
    return arena_strndup(r->a, v->str, v->len);
}

static uint64_t irj_u64(IrjReader *r, const JsonValue *v)
{
    if (!v || v->kind != JSON_NUMBER || !v->is_int || v->integer < 0) {
        irj_fail(r, "expected a non-negative integer");
        return 0;
    }
    return (uint64_t)v->integer;
}

static uint32_t irj_u32(IrjReader *r, const JsonValue *v)
{
    uint64_t x = irj_u64(r, v);
    if (x > 0xFFFFFFFFu)
        irj_fail(r, "integer out of range");
    return (uint32_t)x;
}

static uint32_t irj_idx(IrjReader *r, const JsonValue *v)   /* null -> IR_NONE */
{
    if (!v || v->kind == JSON_NULL)
        return IR_NONE;
    return irj_u32(r, v);
}

static IrBits irj_bitsv(IrjReader *r, const JsonValue *v)
{
    IrBits b;
    b.width = 1;
    b.w = ANEWN(r->a, uint64_t, 1);
    if (!v || v->kind != JSON_STRING || !ir_bits_parse(r->a, v->str, v->len, &b))
        irj_fail(r, "malformed bit literal");
    return b;
}

static IrType *irj_typeref(IrjReader *r, const JsonValue *v)
{
    uint32_t id;
    if (!v || v->kind == JSON_NULL)
        return NULL;
    id = irj_u32(r, v);
    if (id >= r->ntypes) {
        irj_fail(r, "type id out of range");
        return NULL;
    }
    return r->types[id];
}

static int irj_lookup(const char *name, const char *const *names, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (tern_streq(name, names[i]))
            return i;
    return -1;
}

static int irj_enum(IrjReader *r, const JsonValue *v, const char *(*namefn)(int), int n)
{
    const char *s = json_as_str(v, NULL);
    int i;
    for (i = 0; i < n && s; i++)
        if (tern_streq(s, namefn(i)))
            return i;
    irj_fail(r, "unknown enumerator");
    return 0;
}

static const char *irj_n_sigkind(int i) { return ir_sig_kind_name((IrSigKind)i); }
static const char *irj_n_op(int i) { return ir_op_name((IrOp)i); }
static const char *irj_n_ekind(int i) { return ir_expr_kind_name((IrExprKind)i); }
static const char *irj_n_qkind(int i) { return ir_seq_kind_name((IrSeqKind)i); }
static const char *irj_n_akind(int i) { return ir_assert_kind_name((IrAssertKind)i); }
static const char *irj_n_cdc(int i) { return ir_cdc_kind_name((IrCdcKind)i); }
static const char *irj_n_access(int i) { return ir_access_name((IrAccess)i); }
static const char *irj_n_enc(int i) { return ir_encoding_name((IrEncoding)i); }

static Span irj_span_read(IrjReader *r, const JsonValue *obj)
{
    const JsonValue *l = json_get(obj, "loc");
    Span s;
    const char *path;
    memset(&s, 0, sizeof s);
    if (!l || l->kind != JSON_OBJECT)
        return s;
    path = json_as_str(json_get(l, "file"), "");
    if (*path) {
        s.file = src_find(path);
        if (!s.file)
            s.file = src_add(path, "", 0);
    }
    s.line = (uint32_t)json_as_int(json_get(l, "line"), 0);
    s.col = (uint32_t)json_as_int(json_get(l, "col"), 0);
    s.off = (uint32_t)json_as_int(json_get(l, "off"), 0);
    s.len = (uint32_t)json_as_int(json_get(l, "len"), 0);
    TERN_UNUSED(r);
    return s;
}

/* arena array with ir_build-compatible capacity */
static void *irj_array(IrjReader *r, uint32_t n, size_t elem)
{
    return n ? arena_zalloc(r->a, elem * irb_cap(n)) : NULL;
}

static void irj_attrs_read(IrjReader *r, const JsonValue *v, uint32_t *n, IrAttr **out)
{
    uint32_t i, cnt = (uint32_t)json_len(v);
    *n = cnt;
    *out = (IrAttr *)irj_array(r, cnt, sizeof(IrAttr));
    for (i = 0; i < cnt; i++) {
        (*out)[i].name = irj_str(r, json_get(json_at(v, i), "name"));
        (*out)[i].args = irj_text(r, json_get(json_at(v, i), "args"));
    }
}

static uint32_t irj_sigref(IrjReader *r, const JsonValue *v)
{
    uint32_t s = irj_u32(r, v);
    if (r->cur && s >= r->cur->nsigs)
        irj_fail(r, "signal id out of range");
    return s;
}

static IrExpr *irj_expr_read(IrjReader *r, const JsonValue *v, int depth)
{
    IrExpr *e;
    const JsonValue *args;
    uint32_t i;
    if (!v || v->kind == JSON_NULL || r->failed)
        return NULL;
    if (v->kind != JSON_OBJECT || depth > 10000) {
        irj_fail(r, "expected an expression object");
        return NULL;
    }
    e = ANEW(r->a, IrExpr);
    e->kind = (IrExprKind)irj_enum(r, json_get(v, "k"), irj_n_ekind, IRE_PAST + 1);
    e->width = irj_u32(r, json_get(v, "w"));
    if (e->width == 0)
        irj_fail(r, "zero-width expression");
    e->sig = IR_NONE;
    if (e->kind == IRE_UNARY || e->kind == IRE_BINARY)
        e->op = (IrOp)irj_enum(r, json_get(v, "op"), irj_n_op, IROP_LOR + 1);
    e->is_signed = json_as_bool(json_get(v, "signed"), 0);
    e->type = irj_typeref(r, json_get(v, "type"));
    switch (e->kind) {
    case IRE_CONST: e->bits = irj_bitsv(r, json_get(v, "v")); break;
    case IRE_SIG:
    case IRE_MEMREAD: e->sig = irj_sigref(r, json_get(v, "sig")); break;
    case IRE_SLICE:
        e->lo = irj_u32(r, json_get(v, "lo"));
        e->field = irj_str(r, json_get(v, "field"));
        break;
    case IRE_PAST: e->lo = irj_u32(r, json_get(v, "lo")); break;
    default: break;
    }
    args = json_get(v, "args");
    e->nargs = (uint32_t)json_len(args);
    if (e->nargs) {
        e->args = ANEWN(r->a, IrExpr *, e->nargs);
        for (i = 0; i < e->nargs; i++) {
            e->args[i] = irj_expr_read(r, json_at(args, i), depth + 1);
            if (!e->args[i])
                irj_fail(r, "missing expression operand");
        }
    }
    e->loc = irj_span_read(r, v);
    return e;
}

static IrLvalue irj_lvalue_read(IrjReader *r, const JsonValue *v)
{
    IrLvalue lv;
    memset(&lv, 0, sizeof lv);
    lv.sig = irj_sigref(r, json_get(v, "sig"));
    lv.lo = irj_u32(r, json_get(v, "lo"));
    lv.width = irj_u32(r, json_get(v, "w"));
    lv.index = irj_expr_read(r, json_get(v, "index"), 0);
    lv.field = irj_str(r, json_get(v, "field"));
    return lv;
}

static IrBlock irj_block_read(IrjReader *r, const JsonValue *v, int depth);

static IrStmt *irj_stmt_read(IrjReader *r, const JsonValue *v, int depth)
{
    IrStmt *s = ANEW(r->a, IrStmt);
    const char *k = json_as_str(json_get(v, "s"), "");
    uint32_t i, j;
    s->mem = IR_NONE;
    s->lhs.sig = IR_NONE;
    if (tern_streq(k, "assign")) {
        s->kind = IRST_ASSIGN;
        s->lhs = irj_lvalue_read(r, json_get(v, "lhs"));
        s->rhs = irj_expr_read(r, json_get(v, "rhs"), 0);
        if (!s->rhs)
            irj_fail(r, "assign without rhs");
    } else if (tern_streq(k, "if")) {
        s->kind = IRST_IF;
        s->cond = irj_expr_read(r, json_get(v, "cond"), 0);
        s->then_b = irj_block_read(r, json_get(v, "then"), depth + 1);
        s->else_b = irj_block_read(r, json_get(v, "else"), depth + 1);
        if (!s->cond)
            irj_fail(r, "if without condition");
    } else if (tern_streq(k, "case")) {
        const JsonValue *arms = json_get(v, "arms");
        s->kind = IRST_CASE;
        s->subject = irj_expr_read(r, json_get(v, "subject"), 0);
        if (!s->subject)
            irj_fail(r, "case without subject");
        s->narms = (uint32_t)json_len(arms);
        s->arms = ANEWN(r->a, IrCaseArm, s->narms ? s->narms : 1);
        for (i = 0; i < s->narms; i++) {
            const JsonValue *arm = json_at(arms, i), *vals = json_get(arm, "vals");
            s->arms[i].nvals = (uint32_t)json_len(vals);
            s->arms[i].vals = s->arms[i].nvals ? ANEWN(r->a, IrBits, s->arms[i].nvals) : NULL;
            for (j = 0; j < s->arms[i].nvals; j++)
                s->arms[i].vals[j] = irj_bitsv(r, json_at(vals, j));
            s->arms[i].body = irj_block_read(r, json_get(arm, "body"), depth + 1);
        }
    } else if (tern_streq(k, "memwrite")) {
        s->kind = IRST_MEMWRITE;
        s->mem = irj_sigref(r, json_get(v, "mem"));
        s->addr = irj_expr_read(r, json_get(v, "addr"), 0);
        s->data = irj_expr_read(r, json_get(v, "data"), 0);
        s->mask = irj_expr_read(r, json_get(v, "mask"), 0);
        if (!s->addr || !s->data)
            irj_fail(r, "memwrite without address or data");
    } else if (tern_streq(k, "assert")) {
        s->kind = IRST_ASSERT;
        s->akind = (IrAssertKind)irj_enum(r, json_get(v, "kind"), irj_n_akind, IRA_EXPECT + 1);
        s->cond = irj_expr_read(r, json_get(v, "cond"), 0);
        s->msg = irj_text(r, json_get(v, "msg"));
        if (!s->cond)
            irj_fail(r, "assert without condition");
    } else {
        irj_fail(r, "unknown statement kind");
    }
    s->loc = irj_span_read(r, v);
    return s;
}

static IrBlock irj_block_read(IrjReader *r, const JsonValue *v, int depth)
{
    IrBlock b;
    uint32_t i;
    b.n = (uint32_t)json_len(v);
    b.stmts = NULL;
    if (depth > 10000) {
        irj_fail(r, "statements nested too deeply");
        b.n = 0;
        return b;
    }
    if (b.n) {
        b.stmts = ANEWN(r->a, IrStmt *, b.n);
        for (i = 0; i < b.n; i++)
            b.stmts[i] = irj_stmt_read(r, json_at(v, i), depth);
    }
    return b;
}

static IrSeq *irj_seq_read(IrjReader *r, const JsonValue *v, int depth)
{
    IrSeq *q;
    if (!v || v->kind == JSON_NULL || r->failed)
        return NULL;
    if (depth > 10000) {
        irj_fail(r, "sequence nested too deeply");
        return NULL;
    }
    q = ANEW(r->a, IrSeq);
    q->kind = (IrSeqKind)irj_enum(r, json_get(v, "q"), irj_n_qkind, IRQ_NOT + 1);
    q->expr = irj_expr_read(r, json_get(v, "expr"), 0);
    if (q->kind == IRQ_DELAY)
        q->lo = irj_u32(r, json_get(v, "lo"));
    if (q->kind == IRQ_DELAY || q->kind == IRQ_EVENTUALLY)
        q->hi = irj_idx(r, json_get(v, "hi"));
    q->a = irj_seq_read(r, json_get(v, "a"), depth + 1);
    q->b = irj_seq_read(r, json_get(v, "b"), depth + 1);
    if (q->kind == IRQ_EXPR && !q->expr)
        irj_fail(r, "sequence expression missing");
    return q;
}

static void irj_types_read(IrjReader *r, const JsonValue *types)
{
    static const char *const kinds[] = {"bool", "bits",   "uint",  "sint", "enum",
                                        "struct", "array", "clock", "reset"};
    uint32_t i, j, n = (uint32_t)json_len(types);
    r->types = ANEWN(r->a, IrType *, n ? n : 1);
    for (i = 0; i < n && !r->failed; i++) {
        const JsonValue *tv = json_at(types, i);
        IrType *t = ANEW(r->a, IrType);
        int k = irj_lookup(json_as_str(json_get(tv, "kind"), ""), kinds, 9);
        r->types[i] = t;
        r->ntypes = i; /* only earlier types may be referenced */
        if (k < 0) {
            irj_fail(r, "unknown type kind");
            break;
        }
        t->kind = (IrTypeKind)k;
        t->width = irj_u32(r, json_get(tv, "width"));
        t->name = irj_str(r, json_get(tv, "name"));
        t->loc = irj_span_read(r, tv);
        if (t->kind == IRT_STRUCT) {
            const JsonValue *fs = json_get(tv, "fields");
            t->nfields = (uint32_t)json_len(fs);
            t->fields = ANEWN(r->a, IrField, t->nfields ? t->nfields : 1);
            for (j = 0; j < t->nfields; j++) {
                const JsonValue *f = json_at(fs, j);
                t->fields[j].name = irj_str(r, json_get(f, "name"));
                t->fields[j].type = irj_typeref(r, json_get(f, "type"));
                t->fields[j].lsb = irj_u32(r, json_get(f, "lsb"));
                if (!t->fields[j].type)
                    irj_fail(r, "struct field without type");
            }
        } else if (t->kind == IRT_ENUM) {
            const JsonValue *vs = json_get(tv, "variants");
            t->tag_width = irj_u32(r, json_get(tv, "tag_width"));
            t->encoding = (IrEncoding)irj_enum(r, json_get(tv, "encoding"), irj_n_enc, IR_ENC_GRAY + 1);
            t->nvariants = (uint32_t)json_len(vs);
            t->variants = ANEWN(r->a, IrVariant, t->nvariants ? t->nvariants : 1);
            for (j = 0; j < t->nvariants; j++) {
                const JsonValue *vv = json_at(vs, j);
                t->variants[j].name = irj_str(r, json_get(vv, "name"));
                t->variants[j].tag = irj_bitsv(r, json_get(vv, "tag"));
                t->variants[j].payload = irj_typeref(r, json_get(vv, "payload"));
            }
        } else if (t->kind == IRT_ARRAY) {
            t->elem = irj_typeref(r, json_get(tv, "elem"));
            t->len = irj_u32(r, json_get(tv, "len"));
            if (!t->elem)
                irj_fail(r, "array without element type");
        } else if (t->kind == IRT_CLOCK) {
            t->falling = json_as_bool(json_get(tv, "falling"), 0);
        } else if (t->kind == IRT_RESET) {
            t->async = json_as_bool(json_get(tv, "async"), 0);
            t->active_low = json_as_bool(json_get(tv, "active_low"), 0);
        }
        if (t->width == 0)
            irj_fail(r, "zero-width type");
        r->ntypes = i + 1;
    }
    if (r->failed)
        r->ntypes = 0;
}

static IrModule *irj_module_read(IrjReader *r, const JsonValue *mv)
{
    IrModule *m = ANEW(r->a, IrModule);
    const JsonValue *v;
    uint32_t i, j;
    m->name = irj_str(r, json_get(mv, "name"));
    m->tern_name = irj_str(r, json_get(mv, "tern_name"));
    m->display = irj_str(r, json_get(mv, "display"));
    m->package = irj_str(r, json_get(mv, "package"));
    m->is_top = json_as_bool(json_get(mv, "is_top"), 0);
    m->is_extern = json_as_bool(json_get(mv, "is_extern"), 0);
    m->extern_file = irj_text(r, json_get(mv, "extern_file"));
    m->is_cdc = json_as_bool(json_get(mv, "is_cdc"), 0);
    if (m->is_cdc)
        m->cdc_kind = (IrCdcKind)irj_enum(r, json_get(mv, "cdc_kind"), irj_n_cdc, IRC_UNSAFE + 1);
    if (!m->name)
        irj_fail(r, "module without name");
    irj_attrs_read(r, json_get(mv, "attrs"), &m->nattrs, &m->attrs);
    m->loc = irj_span_read(r, mv);
    r->cur = m;

    v = json_get(mv, "params");
    m->nparams = (uint32_t)json_len(v);
    m->params = (IrParam *)irj_array(r, m->nparams, sizeof(IrParam));
    for (i = 0; i < m->nparams; i++) {
        const JsonValue *p = json_at(v, i);
        m->params[i].name = irj_str(r, json_get(p, "name"));
        m->params[i].value = irj_bitsv(r, json_get(p, "value"));
        m->params[i].is_signed = json_as_bool(json_get(p, "signed"), 0);
    }

    v = json_get(mv, "signals");
    m->nsigs = (uint32_t)json_len(v);
    m->sigs = (IrSignal *)irj_array(r, m->nsigs, sizeof(IrSignal));
    for (i = 0; i < m->nsigs && !r->failed; i++) {
        const JsonValue *sv = json_at(v, i), *init;
        IrSignal *s = &m->sigs[i];
        s->name = irj_str(r, json_get(sv, "name"));
        s->path = irj_str(r, json_get(sv, "path"));
        s->kind = (IrSigKind)irj_enum(r, json_get(sv, "kind"), irj_n_sigkind, IRS_MEM + 1);
        s->type = irj_typeref(r, json_get(sv, "type"));
        if (!s->type || !s->name)
            irj_fail(r, "signal without name or type");
        s->domain = irj_idx(r, json_get(sv, "domain"));
        init = json_get(sv, "init");
        if (init && init->kind != JSON_NULL) {
            s->has_init = 1;
            s->init = irj_bitsv(r, init);
        }
        s->test_port = json_as_bool(json_get(sv, "test_port"), 0);
        s->flipped = json_as_bool(json_get(sv, "flipped"), 0);
        if (s->kind == IRS_MEM) {
            const JsonValue *mi = json_get(sv, "mem_init");
            s->depth = irj_u32(r, json_get(sv, "depth"));
            if (mi && mi->kind == JSON_ARRAY) {
                if (json_len(mi) != s->depth)
                    irj_fail(r, "mem_init length differs from depth");
                else {
                    s->mem_init = ANEWN(r->a, IrBits, s->depth ? s->depth : 1);
                    for (j = 0; j < s->depth; j++)
                        s->mem_init[j] = irj_bitsv(r, json_at(mi, j));
                }
            }
            s->mem_init_file = irj_text(r, json_get(sv, "mem_init_file"));
            s->mem_init_is_bin = json_as_bool(json_get(sv, "mem_init_is_bin"), 0);
        }
        irj_attrs_read(r, json_get(sv, "attrs"), &s->nattrs, &s->attrs);
        s->loc = irj_span_read(r, sv);
    }

    v = json_get(mv, "ports");
    m->nports = (uint32_t)json_len(v);
    m->ports = (uint32_t *)irj_array(r, m->nports, sizeof(uint32_t));
    for (i = 0; i < m->nports; i++)
        m->ports[i] = irj_sigref(r, json_at(v, i));

    v = json_get(mv, "domains");
    m->ndomains = (uint32_t)json_len(v);
    m->domains = (IrDomain *)irj_array(r, m->ndomains, sizeof(IrDomain));
    for (i = 0; i < m->ndomains; i++) {
        const JsonValue *dv = json_at(v, i);
        m->domains[i].name = irj_str(r, json_get(dv, "name"));
        if (!m->domains[i].name)
            m->domains[i].name = intern_cstr("");
        m->domains[i].clock = irj_sigref(r, json_get(dv, "clock"));
        m->domains[i].reset = irj_idx(r, json_get(dv, "reset"));
        if (m->domains[i].reset != IR_NONE && m->domains[i].reset >= m->nsigs)
            irj_fail(r, "reset signal out of range");
        m->domains[i].hz = irj_u64(r, json_get(dv, "hz"));
    }
    m->default_domain = irj_idx(r, json_get(mv, "default_domain"));

    v = json_get(mv, "assigns");
    m->nassigns = (uint32_t)json_len(v);
    m->assigns = (IrAssign *)irj_array(r, m->nassigns, sizeof(IrAssign));
    for (i = 0; i < m->nassigns; i++) {
        const JsonValue *av = json_at(v, i);
        m->assigns[i].lhs = irj_lvalue_read(r, json_get(av, "lhs"));
        m->assigns[i].rhs = irj_expr_read(r, json_get(av, "rhs"), 0);
        if (!m->assigns[i].rhs)
            irj_fail(r, "assign without rhs");
        m->assigns[i].loc = irj_span_read(r, av);
    }

    v = json_get(mv, "processes");
    m->nprocs = (uint32_t)json_len(v);
    m->procs = (IrProcess *)irj_array(r, m->nprocs, sizeof(IrProcess));
    for (i = 0; i < m->nprocs; i++) {
        const JsonValue *pv = json_at(v, i), *regs = json_get(pv, "regs");
        IrProcess *p = &m->procs[i];
        p->kind = tern_streq(json_as_str(json_get(pv, "kind"), ""), "clocked") ? IRP_CLOCKED : IRP_COMB;
        p->domain = irj_idx(r, json_get(pv, "domain"));
        if (p->kind == IRP_CLOCKED && p->domain >= m->ndomains)
            irj_fail(r, "clocked process without a valid domain");
        p->nregs = (uint32_t)json_len(regs);
        p->regs = p->nregs ? ANEWN(r->a, uint32_t, p->nregs) : NULL;
        for (j = 0; j < p->nregs; j++)
            p->regs[j] = irj_sigref(r, json_at(regs, j));
        p->body = irj_block_read(r, json_get(pv, "body"), 0);
        p->loc = irj_span_read(r, pv);
    }

    v = json_get(mv, "instances");
    m->ninsts = (uint32_t)json_len(v);
    m->insts = (IrInstance *)irj_array(r, m->ninsts, sizeof(IrInstance));
    for (i = 0; i < m->ninsts && !r->failed; i++) {
        const JsonValue *iv = json_at(v, i), *conns = json_get(iv, "conns");
        IrInstance *in = &m->insts[i];
        in->name = irj_str(r, json_get(iv, "name"));
        in->module = ir_design_find_module(r->d, json_as_str(json_get(iv, "module"), ""));
        if (!in->module) {
            irj_fail(r, "instance of an unknown module (modules must precede their parents)");
            break;
        }
        in->nconns = (uint32_t)json_len(conns);
        in->conns = (IrConn *)irj_array(r, in->nconns, sizeof(IrConn));
        for (j = 0; j < in->nconns; j++) {
            const JsonValue *cv = json_at(conns, j);
            in->conns[j].port = irj_u32(r, json_get(cv, "port"));
            if (in->conns[j].port >= in->module->nsigs)
                irj_fail(r, "port id out of range");
            in->conns[j].expr = irj_expr_read(r, json_get(cv, "expr"), 0);
        }
        in->loc = irj_span_read(r, iv);
    }

    v = json_get(mv, "properties");
    m->nprops = (uint32_t)json_len(v);
    m->props = (IrProperty *)irj_array(r, m->nprops, sizeof(IrProperty));
    for (i = 0; i < m->nprops; i++) {
        const JsonValue *pv = json_at(v, i);
        IrProperty *p = &m->props[i];
        p->name = irj_str(r, json_get(pv, "name"));
        p->kind = (IrAssertKind)irj_enum(r, json_get(pv, "kind"), irj_n_akind, IRA_EXPECT + 1);
        p->domain = irj_idx(r, json_get(pv, "domain"));
        p->seq = irj_seq_read(r, json_get(pv, "seq"), 0);
        if (!p->seq)
            irj_fail(r, "property without sequence");
        p->loc = irj_span_read(r, pv);
    }
    r->cur = NULL;
    return m;
}

IrDesign *ir_from_json(Arena *a, const char *text, size_t len)
{
    JsonError jerr;
    JsonValue *root = json_parse(a, text, len, &jerr);
    IrjReader r;
    const JsonValue *v;
    IrDesign *d;
    uint32_t i, j, k;
    if (!root) {
        diag_error(span_none(), "invalid IR JSON at %u:%u: %s", jerr.line, jerr.col, jerr.msg);
        return NULL;
    }
    memset(&r, 0, sizeof r);
    r.a = a;
    if (!tern_streq(json_as_str(json_get(root, "format"), ""), IRJ_FORMAT) ||
        json_as_int(json_get(root, "version"), 0) != IRJ_VERSION) {
        diag_error(span_none(), "not a tern IR document (expected format \"%s\", version %d)",
                   IRJ_FORMAT, IRJ_VERSION);
        return NULL;
    }
    d = ir_design_new(a);
    r.d = d;
    irj_types_read(&r, json_get(root, "types"));

    v = json_get(root, "named_types");
    d->ntypes = (uint32_t)json_len(v);
    d->types = (IrType **)irj_array(&r, d->ntypes, sizeof(IrType *));
    for (i = 0; i < d->ntypes; i++) {
        d->types[i] = irj_typeref(&r, json_at(v, i));
        if (!d->types[i])
            irj_fail(&r, "bad named type reference");
    }

    v = json_get(root, "modules");
    for (i = 0; i < json_len(v) && !r.failed; i++) {
        IrModule *m = irj_module_read(&r, json_at(v, i));
        uint32_t n = d->nmodules;
        d->modules = (IrModule **)irb_grow(a, d->modules, n, sizeof(IrModule *));
        d->modules[d->nmodules++] = m;
        if (m->is_top)
            d->top = m;
    }
    if (!d->top && d->nmodules) {
        const char *top = json_as_str(json_get(root, "top"), NULL);
        d->top = top ? ir_design_find_module(d, top) : NULL;
    }

    v = json_get(root, "regmaps");
    d->nregmaps = (uint32_t)json_len(v);
    d->regmaps = (IrRegmap *)irj_array(&r, d->nregmaps, sizeof(IrRegmap));
    for (i = 0; i < d->nregmaps; i++) {
        const JsonValue *rv = json_at(v, i), *regs = json_get(rv, "regs");
        IrRegmap *rm = &d->regmaps[i];
        rm->name = irj_str(&r, json_get(rv, "name"));
        rm->package = irj_str(&r, json_get(rv, "package"));
        rm->nregs = (uint32_t)json_len(regs);
        rm->regs = rm->nregs ? ANEWN(a, IrReg, rm->nregs) : NULL;
        for (j = 0; j < rm->nregs; j++) {
            const JsonValue *gv = json_at(regs, j), *fs = json_get(gv, "fields");
            IrReg *g = &rm->regs[j];
            g->offset = irj_u64(&r, json_get(gv, "offset"));
            g->name = irj_str(&r, json_get(gv, "name"));
            g->access = (IrAccess)irj_enum(&r, json_get(gv, "access"), irj_n_access, IR_RW1S + 1);
            g->width = irj_u32(&r, json_get(gv, "width"));
            g->is_stream = json_as_bool(json_get(gv, "is_stream"), 0);
            g->doc = irj_text(&r, json_get(gv, "doc"));
            g->nfields = (uint32_t)json_len(fs);
            g->fields = g->nfields ? ANEWN(a, IrRegField, g->nfields) : NULL;
            for (k = 0; k < g->nfields; k++) {
                const JsonValue *fv = json_at(fs, k);
                g->fields[k].name = irj_str(&r, json_get(fv, "name"));
                g->fields[k].lo = irj_u32(&r, json_get(fv, "lo"));
                g->fields[k].width = irj_u32(&r, json_get(fv, "width"));
                g->fields[k].type = irj_typeref(&r, json_get(fv, "type"));
            }
        }
        rm->loc = irj_span_read(&r, rv);
    }

    v = json_get(root, "crossings");
    d->ncrossings = (uint32_t)json_len(v);
    d->crossings = (IrCrossing *)irj_array(&r, d->ncrossings, sizeof(IrCrossing));
    for (i = 0; i < d->ncrossings; i++) {
        const JsonValue *cv = json_at(v, i);
        IrCrossing *c = &d->crossings[i];
        c->path = irj_str(&r, json_get(cv, "path"));
        c->kind = (IrCdcKind)irj_enum(&r, json_get(cv, "kind"), irj_n_cdc, IRC_UNSAFE + 1);
        c->from_domain = irj_str(&r, json_get(cv, "from_domain"));
        c->to_domain = irj_str(&r, json_get(cv, "to_domain"));
        c->signal = irj_str(&r, json_get(cv, "signal"));
        c->loc = irj_span_read(&r, cv);
    }

    if (r.failed) {
        diag_error(span_none(), "invalid IR JSON: %s", r.err);
        return NULL;
    }
    return d;
}

#undef IRJ_FORMAT
#undef IRJ_VERSION
