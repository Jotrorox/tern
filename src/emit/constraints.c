/* constraints.c — timing-constraint fragments for the design's top (spec
 * §8.3): create_clock for top clock ports with a known frequency, and for
 * every instance of a std::cdc primitive in the hierarchy the synchronizer
 * constraints (false paths into the first synchronizer stage, datapath-only
 * max delays for Gray pointers and handshake data, ASYNC_REG cells), plus
 * the CDC report as a comment block.
 *
 * Cell names follow the synthesis tools' conventions: Vivado names the flop
 * of register `r` in instance path a/b `a/b/r_reg` (`r_reg[i]` for vectors);
 * Quartus names it `a|b|r` (`r[i]`). */
#include "emit.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/map.h"

typedef struct ConsCtx {
    const IrDesign *d;
    SvNames *n;
    int sdc;             /* 0: Vivado .xdc, 1: Quartus .sdc */
    StrBuf *out;
} ConsCtx;

typedef struct ConsFrame {     /* one level of the instance path */
    const IrModule *parent;
    uint32_t inst;
} ConsFrame;

typedef VEC(ConsFrame) ConsFrameVec;

static void cons_hdr(ConsCtx *c, const char *title)
{
    sb_printf(c->out, "\n# %s\n", title);
}

/* cell pattern(s) of register `sig` of module m at instance path `path` */
static void cons_cells(ConsCtx *c, const char *path, const IrModule *m, uint32_t sig, StrBuf *out)
{
    const char *nm = sv_names_signal(c->n, m, sig);
    int vec = m->sigs[sig].type->width > 1;
    if (m->sigs[sig].kind == IRS_MEM) {   /* RAM cells: mem_reg_0_3_0_5 (Vivado), mem_rtl_0 / mem[i][j] */
        sb_printf(out, "%s%s%s%s*", path, *path ? (c->sdc ? "|" : "/") : "", nm, c->sdc ? "" : "_reg");
        return;
    }
    if (c->sdc)
        sb_printf(out, "%s%s%s%s", path, *path ? "|" : "", nm, vec ? "[*]" : "");
    else
        sb_printf(out, "%s%s%s_reg%s", path, *path ? "/" : "", nm, vec ? "[*]" : "");
}

static void cons_get(ConsCtx *c, const char *pats, StrBuf *out)
{
    sb_printf(out, "[%s {%s}]", c->sdc ? "get_registers" : "get_cells", pats);
}

/* ---------------------------------------------------- clock resolution -- */

/* hz of the clock signal `sig` of the module at the end of `stack` (0: unknown) */
static uint64_t cons_clock_hz(ConsCtx *c, const ConsFrame *stack, size_t depth, const IrModule *m, uint32_t sig)
{
    uint32_t i;
    TERN_UNUSED(c);
    while (depth > 0) {
        const ConsFrame *f = &stack[depth - 1];
        const IrInstance *in = &f->parent->insts[f->inst];
        const IrExpr *e = NULL;
        for (i = 0; i < in->nconns; i++)
            if (in->conns[i].port == sig)
                e = in->conns[i].expr;
        if (!e || e->kind != IRE_SIG)
            return 0;
        m = f->parent;
        sig = e->sig;
        depth--;
    }
    for (i = 0; i < m->ndomains; i++)
        if (m->domains[i].clock == sig && m->domains[i].hz)
            return m->domains[i].hz;
    return 0;
}

/* --------------------------------------------------- source analysis -- */

static int cons_has_attr(const IrSignal *s, const char *name)
{
    uint32_t i;
    for (i = 0; i < s->nattrs; i++)
        if (tern_streq(s->attrs[i].name, name))
            return 1;
    return 0;
}

static uint32_t cons_mem_domain(const IrModule *m, uint32_t mem);

static void cons_expr_reads(const IrExpr *e, Map *set)
{
    uint32_t i;
    if (!e)
        return;
    if (e->kind == IRE_SIG || e->kind == IRE_MEMREAD)
        map_put(set, e->sig, 1);
    for (i = 0; i < e->nargs; i++)
        cons_expr_reads(e->args[i], set);
}

static void cons_block_reads_for(const IrBlock *b, uint32_t target, Map *set, int *found)
{
    uint32_t i, j;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN:
            if (s->lhs.sig == target) {
                cons_expr_reads(s->rhs, set);
                *found = 1;
            }
            break;
        case IRST_IF:
            cons_block_reads_for(&s->then_b, target, set, found);
            cons_block_reads_for(&s->else_b, target, set, found);
            break;
        case IRST_CASE:
            for (j = 0; j < s->narms; j++)
                cons_block_reads_for(&s->arms[j].body, target, set, found);
            break;
        case IRST_MEMWRITE:
            if (s->mem == target) {
                cons_expr_reads(s->data, set);
                *found = 1;
            }
            break;
        default: break;
        }
    }
}

/* Leaves (registers, memories, inputs) whose values reach `sig`'s next value,
 * looking through continuous assignments and comb processes. */
static void cons_sources(const IrModule *m, uint32_t sig, Map *leaves, Map *seen, int depth)
{
    Map direct = {0};
    size_t it = 0;
    uint64_t k, v;
    uint32_t i;
    int found = 0;
    for (i = 0; i < m->nprocs; i++)
        cons_block_reads_for(&m->procs[i].body, sig, &direct, &found);
    for (i = 0; i < m->nassigns; i++)
        if (m->assigns[i].lhs.sig == sig)
            cons_expr_reads(m->assigns[i].rhs, &direct);
    while (map_next(&direct, &it, &k, &v)) {
        const IrSignal *s;
        if (k >= m->nsigs || map_has(seen, k))
            continue;
        map_put(seen, k, 1);
        s = &m->sigs[k];
        if (s->kind == IRS_REG || s->kind == IRS_MEM || s->kind == IRS_IN || depth > 32)
            map_put(leaves, k, 1);
        else
            cons_sources(m, (uint32_t)k, leaves, seen, depth + 1);
    }
    map_free(&direct);
}

static uint32_t cons_mem_domain(const IrModule *m, uint32_t mem)
{
    uint32_t i;
    for (i = 0; i < m->nprocs; i++) {
        Map set = {0};
        int found = 0;
        if (m->procs[i].kind != IRP_CLOCKED)
            continue;
        cons_block_reads_for(&m->procs[i].body, mem, &set, &found);
        map_free(&set);
        if (found)
            return m->procs[i].domain;
    }
    return IR_NONE;
}

static uint32_t cons_sig_domain(const IrModule *m, uint32_t s)
{
    if (m->sigs[s].kind == IRS_MEM)
        return cons_mem_domain(m, s);
    if (m->sigs[s].domain != IR_NONE)
        return m->sigs[s].domain;
    if (m->sigs[s].kind == IRS_REG) {
        uint32_t i, k;
        for (i = 0; i < m->nprocs; i++)
            for (k = 0; k < m->procs[i].nregs; k++)
                if (m->procs[i].regs[k] == s)
                    return m->procs[i].domain;
    }
    return IR_NONE;
}

/* ------------------------------------------------------- one CDC inst -- */

static void cons_cdc_instance(ConsCtx *c, const ConsFrame *stack, size_t depth, const char *path,
                              const IrModule *cm, const char *inst_display)
{
    uint32_t r;
    StrBuf ar = {0}, line = {0};
    int reset_kind = cm->cdc_kind == IRC_RESET_SYNC || cm->cdc_kind == IRC_RESET_BRIDGE;
    sb_printf(&line, "CDC %s: %s (%s)", inst_display, cm->display ? cm->display : cm->name,
              ir_cdc_kind_name(cm->cdc_kind));
    cons_hdr(c, sb_cstr(&line));
    /* ASYNC_REG cells */
    for (r = 0; r < cm->nsigs; r++)
        if (cm->sigs[r].kind == IRS_REG && cons_has_attr(&cm->sigs[r], "async_reg")) {
            if (ar.len)
                sb_putc(&ar, ' ');
            cons_cells(c, path, cm, r, &ar);
        }
    if (ar.len) {
        if (c->sdc) {
            sb_printf(c->out, "# synchronizer registers (Quartus: SYNCHRONIZER_IDENTIFICATION in the .qsf): %s\n",
                      sb_cstr(&ar));
        } else {
            sb_puts(c->out, "set_property ASYNC_REG TRUE ");
            cons_get(c, sb_cstr(&ar), c->out);
            sb_putc(c->out, '\n');
        }
    }
    /* every register fed from another domain */
    for (r = 0; r < cm->nsigs; r++) {
        Map leaves = {0}, seen = {0};
        size_t it = 0;
        uint64_t k, v;
        uint32_t rd;
        int is_async = cons_has_attr(&cm->sigs[r], "async_reg");
        StrBuf to = {0}, from = {0};
        int foreign_in = 0, nfrom = 0;
        if (cm->sigs[r].kind != IRS_REG)
            continue;
        rd = cons_sig_domain(cm, r);
        cons_sources(cm, r, &leaves, &seen, 0);
        while (map_next(&leaves, &it, &k, &v)) {
            uint32_t ld;
            if (k == r)
                continue;
            ld = cons_sig_domain(cm, (uint32_t)k);
            if (cm->sigs[k].kind == IRS_IN) {
                if (cm->sigs[k].type->kind == IRT_CLOCK || cm->sigs[k].type->kind == IRT_RESET)
                    continue;
                if ((ld != IR_NONE && ld != rd) || (ld == IR_NONE && is_async))
                    foreign_in = 1;
                continue;
            }
            if (ld == IR_NONE || ld == rd)
                continue;
            if (cm->sigs[k].kind == IRS_REG && cons_has_attr(&cm->sigs[k], "async_reg") &&
                cons_sig_domain(cm, (uint32_t)k) == rd)
                continue;
            if (from.len)
                sb_putc(&from, ' ');
            cons_cells(c, path, cm, (uint32_t)k, &from);
            nfrom++;
        }
        cons_cells(c, path, cm, r, &to);
        if (reset_kind && is_async) {
            if (c->sdc) {
                sb_puts(c->out, "set_false_path -to ");
                cons_get(c, sb_cstr(&to), c->out);
                sb_puts(c->out, "  ;# asynchronous assertion\n");
            } else {
                sb_printf(c->out,
                          "set_false_path -to [get_pins -of_objects [get_cells {%s}] -filter {IS_PRESET || IS_CLEAR}]\n",
                          sb_cstr(&to));
            }
        } else if (nfrom || foreign_in) {
            int maxdelay = nfrom && (cm->cdc_kind == IRC_ASYNC_FIFO || !is_async);
            if (maxdelay) {
                uint64_t hz = 0;
                double ns;
                if (rd != IR_NONE && rd < cm->ndomains)
                    hz = cons_clock_hz(c, stack, depth, cm, cm->domains[rd].clock);
                ns = hz ? 1e9 / (double)hz : 10.0;
                sb_printf(c->out, "set_max_delay%s -from ", c->sdc ? "" : " -datapath_only");
                cons_get(c, sb_cstr(&from), c->out);
                sb_puts(c->out, " -to ");
                cons_get(c, sb_cstr(&to), c->out);
                sb_printf(c->out, " %.3f%s\n", ns, hz ? "" : "  ;# destination clock frequency unknown: assumed 100 MHz");
            } else {
                sb_puts(c->out, "set_false_path -to ");
                cons_get(c, sb_cstr(&to), c->out);
                sb_putc(c->out, '\n');
            }
        }
        sb_free(&to);
        sb_free(&from);
        map_free(&leaves);
        map_free(&seen);
    }
    sb_free(&ar);
    sb_free(&line);
}

static void cons_walk(ConsCtx *c, const IrModule *m, ConsFrameVec *stack, const char *path, const char *disp)
{
    uint32_t i;
    for (i = 0; i < m->ninsts; i++) {
        const IrModule *cm = m->insts[i].module;
        StrBuf p = {0}, dp = {0};
        ConsFrame f;
        sb_printf(&p, "%s%s%s", path, *path ? (c->sdc ? "|" : "/") : "", sv_names_instance(c->n, m, i));
        sb_printf(&dp, "%s > %s", disp, m->insts[i].name);
        f.parent = m;
        f.inst = i;
        vec_push(stack[0], f);
        if (cm->is_cdc)
            cons_cdc_instance(c, stack->data, stack->len, sb_cstr(&p), cm, sb_cstr(&dp));
        else if (!cm->is_extern)
            cons_walk(c, cm, stack, sb_cstr(&p), sb_cstr(&dp));
        stack->len--;
        sb_free(&p);
        sb_free(&dp);
    }
}

static void cons_emit_one(const IrDesign *d, int sdc, StrBuf *out)
{
    ConsCtx c;
    const IrModule *top = d->top;
    uint32_t i, j;
    ConsFrameVec stack = {0};
    c.d = d;
    c.n = sv_names_new(d);
    c.sdc = sdc;
    c.out = out;
    sb_printf(out, "# %s.%s: timing constraints for %s, generated by tern. Do not edit.\n",
              top ? sv_names_module(c.n, top) : "design", sdc ? "sdc" : "xdc", top ? top->name : "?");
    if (!top) {
        sv_names_free(c.n);
        return;
    }
    cons_hdr(&c, "clocks");
    for (i = 0; i < top->ndomains; i++) {
        const IrDomain *dm = &top->domains[i];
        int dup = 0;
        if (dm->clock >= top->nsigs || top->sigs[dm->clock].kind != IRS_IN)
            continue;
        for (j = 0; j < i; j++)
            if (top->domains[j].clock == dm->clock)
                dup = 1;
        if (dup)
            continue;
        if (dm->hz) {
            const char *cn = sv_names_signal(c.n, top, dm->clock);
            sb_printf(out, "create_clock -name %s -period %.3f [get_ports {%s}]\n", cn, 1e9 / (double)dm->hz, cn);
        } else {
            sb_printf(out, "# %s: frequency unknown (declare it with #[clock(hz: N)])\n",
                      sv_names_signal(c.n, top, dm->clock));
        }
    }
    cons_walk(&c, top, &stack, "", top->name);
    vec_free(stack);
    cons_hdr(&c, "CDC report");
    if (!d->ncrossings)
        sb_puts(out, "#   (no clock-domain crossings)\n");
    for (i = 0; i < d->ncrossings; i++) {
        const IrCrossing *x = &d->crossings[i];
        const SourceFile *f = x->loc.file ? src_get(x->loc.file) : NULL;
        sb_printf(out, "#   %s: %s, %s -> %s", x->path ? x->path : "?", ir_cdc_kind_name(x->kind),
                  x->from_domain && *x->from_domain ? x->from_domain : "(default)",
                  x->to_domain && *x->to_domain ? x->to_domain : "(default)");
        if (x->signal)
            sb_printf(out, ", signal %s", x->signal);
        if (f && f->path)
            sb_printf(out, " (%s:%u)", f->path, x->loc.line);
        sb_putc(out, '\n');
    }
    sv_names_free(c.n);
}

void cons_emit_xdc(const IrDesign *d, StrBuf *out) { cons_emit_one(d, 0, out); }
void cons_emit_sdc(const IrDesign *d, StrBuf *out) { cons_emit_one(d, 1, out); }

int cons_emit(const IrDesign *d, const SvOptions *o, SvOutput *out)
{
    unsigned which = o ? o->constraints : (EMIT_CONS_XDC | EMIT_CONS_SDC);
    SvNames *n;
    StrBuf sb = {0}, name = {0};
    if (!d->top)
        return 0;
    n = sv_names_new(d);
    if (which & EMIT_CONS_XDC) {
        cons_emit_xdc(d, &sb);
        sb_printf(&name, "%s.xdc", sv_names_module(n, d->top));
        sv_output_add(out, sb_cstr(&name), SV_FILE_XDC, &sb);
    }
    if (which & EMIT_CONS_SDC) {
        sb_reset(&name);
        cons_emit_sdc(d, &sb);
        sb_printf(&name, "%s.sdc", sv_names_module(n, d->top));
        sv_output_add(out, sb_cstr(&name), SV_FILE_SDC, &sb);
    }
    sb_free(&sb);
    sb_free(&name);
    sv_names_free(n);
    return 0;
}
