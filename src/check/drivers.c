/* drivers.c — multiple and missing drivers (spec §6.1: "Each signal has
 * exactly one driver block").
 *
 * A driver is a node of the module model (check_impl.h): one continuous
 * assignment, one comb or `on` block (for the signals it assigns), or one
 * output of an instance. Drivers are tracked per bit range, so element-wise
 * assignments of an array `var` from several continuous assignments are
 * fine while two drivers of the same bits are an error that points at both.
 * Memories may be written from several `on` blocks (multi-port RAMs) and
 * are not checked here.
 *
 * Every output, wire, var and register must be driven (all of its bits);
 * #[test_port] signals are driven by tests and exempt. Assigning an input
 * is rejected by the elaborator; the IR-level check here is a safety net. */
#include "check_impl.h"

static const char *drv_desc(const ChkMod *cm, const ChkNode *n)
{
    switch (n->kind) {
    case CHK_N_ASSIGN: return "a continuous assignment";
    case CHK_N_COMB: return "a `comb` block";
    case CHK_N_CLOCKED: return "an `on` block";
    case CHK_N_INST_OUT: {
        const IrInstance *in = &cm->m->insts[n->idx];
        const IrModule *c = in->module;
        const char *port = c && n->aux < c->nsigs ? c->sigs[n->aux].path : "?";
        return arena_printf(&cm->cx->arena, "output `%s` of instance `%s`", port, in->name);
    }
    default: break;
    }
    return "another driver";
}

static void drv_multiple(ChkMod *cm, uint32_t sig, const ChkRange *first, const ChkRange *second, uint32_t lo,
                         uint32_t hi)
{
    ChkCtx *cx = cm->cx;
    const ChkNode *a = &cm->nodes.data[first->node], *b = &cm->nodes.data[second->node];
    const char *what = chk_bits_name(cx, cm->m, sig, lo, hi);
    int ok;
    if (a->kind == b->kind && a->kind != CHK_N_INST_OUT)
        ok = chk_error(cx, second->loc, "multiple drivers for %s: driven by two %s", what,
                       a->kind == CHK_N_ASSIGN ? "continuous assignments"
                       : a->kind == CHK_N_COMB ? "`comb` blocks"
                                               : "`on` blocks");
    else
        ok = chk_error(cx, second->loc, "multiple drivers for %s: driven by %s and %s", what, drv_desc(cm, b),
                       drv_desc(cm, a));
    if (!ok)
        return;
    diag_note(first->loc, "%s is also driven here", what);
    if (a->kind == CHK_N_INST_OUT || b->kind == CHK_N_INST_OUT)
        diag_help("an instance output already drives this signal; read it instead of assigning it");
    else
        diag_help("a signal has exactly one driver block: move all its assignments into one block");
}

static void drv_undriven(ChkMod *cm, uint32_t sig, uint32_t lo, uint32_t hi, int none)
{
    ChkCtx *cx = cm->cx;
    const IrSignal *s = &cm->m->sigs[sig];
    const char *kw = chk_kind_word(s->kind);
    int r;
    if (none) {
        if (s->kind == IRS_VAR || s->kind == IRS_REG)
            r = chk_error(cx, s->loc, "%s `%s` is never assigned", kw, s->path);
        else
            r = chk_error(cx, s->loc, "%s `%s` is never driven", kw, s->path);
    } else {
        const char *verb = s->kind == IRS_VAR || s->kind == IRS_REG ? "assigned" : "driven";
        if (hi == lo + 1)
            r = chk_error(cx, s->loc, "bit %u of %s `%s` is never %s", (unsigned)lo, kw, s->path, verb);
        else
            r = chk_error(cx, s->loc, "bits [%u:%u] of %s `%s` are never %s", (unsigned)(hi - 1), (unsigned)lo, kw,
                          s->path, verb);
    }
    if (!r)
        return;
    switch (s->kind) {
    case IRS_OUT: diag_help("assign it at module level (`%s = ...`) or in a `comb` block", s->path); break;
    case IRS_VAR: {
        StrBuf ty = {0};
        if (s->type)
            ir_type_print(s->type, &ty);
        diag_help("assign it in a `comb` block, or give it a default (`var %s: %s = ...`)", s->path,
                  ty.len ? sb_cstr(&ty) : "T");
        sb_free(&ty);
        break;
    }
    case IRS_REG: diag_help("assign it in an `on` block, or use a `const` if it never changes"); break;
    default: break;
    }
}

void chk_drivers(ChkMod *cm)
{
    IrModule *m = cm->m;
    uint32_t s;
    for (s = 0; s < m->nsigs; s++) {
        const IrSignal *sg = &m->sigs[s];
        uint32_t f, e, i, cov, covered, w;
        int reported = 0;
        chk_ranges_of(cm, s, &f, &e);
        if (sg->kind == IRS_IN) {
            if (f < e)
                chk_error(cm->cx, cm->ranges.data[f].loc, "cannot assign to input port `%s`", sg->path);
            continue;
        }
        if (sg->kind == IRS_MEM)
            continue;
        /* the block kind must match the signal kind (the elaborator rejects
           this in source; designs read from IR JSON are checked here) */
        for (i = f; i < e; i++) {
            const ChkNode *n = &cm->nodes.data[cm->ranges.data[i].node];
            if (sg->kind == IRS_REG && n->kind != CHK_N_CLOCKED) {
                chk_error(cm->cx, cm->ranges.data[i].loc, "register `%s` can only be assigned in an `on` block",
                          sg->path);
                break;
            }
            if (sg->kind != IRS_REG && n->kind == CHK_N_CLOCKED) {
                chk_error(cm->cx, cm->ranges.data[i].loc,
                          "`%s` is not a register: assign it in a `comb` block or at module level", sg->path);
                break;
            }
        }
        /* overlapping ranges of different drivers */
        if (f < e) {
            cov = f;
            for (i = f + 1; i < e && !reported; i++) {
                const ChkRange *r = &cm->ranges.data[i], *c = &cm->ranges.data[cov];
                if (r->lo < c->hi && r->node != c->node) {
                    drv_multiple(cm, s, c, r, r->lo, r->hi < c->hi ? r->hi : c->hi);
                    reported = 1;
                }
                if (r->hi > c->hi)
                    cov = i;
            }
        }
        /* undriven bits */
        if (sg->test_port || !sg->type)
            continue;
        w = sg->type->width;
        if (f == e) {
            drv_undriven(cm, s, 0, w, 1);
            continue;
        }
        covered = 0;
        for (i = f; i < e; i++) {
            const ChkRange *r = &cm->ranges.data[i];
            if (r->lo > covered)
                break;
            if (r->hi > covered)
                covered = r->hi;
        }
        if (covered < w) {
            uint32_t hi = w;
            if (i < e && cm->ranges.data[i].lo < hi)
                hi = cm->ranges.data[i].lo;
            drv_undriven(cm, s, covered, hi, 0);
        }
    }
}
