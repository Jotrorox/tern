/* elab.c — elaboration of modules into the IR (see elab.h): ports and
 * bundle flattening, domains, signal declarations (let/var/reg/mem),
 * continuous assignments, comb and clocked processes, instances with
 * inherited clocks/resets, memories, properties, regmaps, and the module
 * cache. Expressions and statements are evaluated by consteval.c.
 *
 * Naming (also documented for the back ends and the test interpreter):
 *   - a port `tx: Stream<u8>` becomes signals tx_valid, tx_data, tx_ready
 *     with paths "tx.valid" ...; arrays of bundles: in_0_valid / "in[0].valid";
 *   - declarations inside unrolled loops get "_<i>" ("_<i>_<j>" nested);
 *   - every port of an instance `x` gets a parent wire x_<port> (path
 *     "x.<port path>"): outputs are driven by the instance, data inputs are
 *     assigned from the connection (or later by `x.port = e`); clock and
 *     reset inputs are connected directly to the parent's signal.
 */
#include "elab_impl.h"

/* deepest chain of module instances (stack depth of elab_module) */
#define EL_MAX_MOD_DEPTH 128

/* ------------------------------------------------------------- helpers */

static const char *elb_attr_args(Elab *el, Attr *a)
{
    Span s;
    TERN_UNUSED(el);
    if (!a->nargs)
        return NULL;
    s = span_join(a->args[0].span, a->args[a->nargs - 1].span);
    return el_span_text(s);
}

static Attr *elb_find_attr(AttrList *l, const char *name)
{
    uint32_t i;
    for (i = 0; i < l->n; i++)
        if (l->items[i].name.name && strcmp(l->items[i].name.name, name) == 0)
            return &l->items[i];
    return NULL;
}

static void elb_sig_attrs(Elab *el, uint32_t sig, AttrList *l)
{
    uint32_t i;
    IrModule *m = el->mod->ir;
    for (i = 0; i < l->n; i++) {
        Attr *a = &l->items[i];
        const char *n = a->name.name;
        if (!n)
            continue;
        if (strcmp(n, "test_port") == 0) {
            m->sigs[sig].test_port = 1;
            continue;
        }
        if (strcmp(n, "clock") == 0)
            continue; /* recorded in the domain (hz) */
        ir_signal_add_attr(el->design, m, sig, n, elb_attr_args(el, a));
    }
}

static const char *elb_name(Elab *el, Env *env, const char *base)
{
    const char *suf = env && env->suffix ? env->suffix : "";
    if (!suf[0])
        return base;
    return intern_cstr(arena_printf(&el->arena, "%s%s", base, suf));
}

static const char *elb_domain_name(Elab *el, uint32_t d)
{
    if (d == IR_NONE || d >= el->mod->ir->ndomains)
        return NULL;
    if (el->mod->ir->domains[d].name && el->mod->ir->domains[d].name[0])
        return el->mod->ir->domains[d].name;
    return el->mod->ir->sigs[el->mod->ir->domains[d].clock].name;
}

/* Declares the IR signals of a (possibly bundle) type and returns the place.
 * dir: 0 none, 1 in, 2 out (for ports; flip toggles). */
static Place *elb_declare_tree(Elab *el, const char *name, const char *path, Ty *t, IrSigKind kind, int flipped,
                               uint32_t domain, AttrList *attrs, Span loc)
{
    IrModule *m = el->mod->ir;
    Place *p = ANEW(&el->arena, Place);
    uint32_t i;
    p->ty = t;
    if (t->kind == TY_BUNDLE || (t->kind == TY_ARRAY && ty_has_bundle(t))) {
        uint32_t n = t->kind == TY_BUNDLE ? t->nfields : t->len;
        p->kind = PL_AGG;
        p->n = n;
        p->subs = ANEWN(&el->arena, Place *, n ? n : 1);
        for (i = 0; i < n; i++) {
            const char *cn, *cp;
            Ty *ct;
            int f = flipped;
            if (t->kind == TY_BUNDLE) {
                cn = arena_printf(&el->arena, "%s_%s", name, t->fields[i].name);
                cp = arena_printf(&el->arena, "%s.%s", path, t->fields[i].name);
                ct = t->fields[i].ty;
                f ^= t->fields[i].flip;
            } else {
                cn = arena_printf(&el->arena, "%s_%u", name, (unsigned)i);
                cp = arena_printf(&el->arena, "%s[%u]", path, (unsigned)i);
                ct = t->elem;
            }
            p->subs[i] = elb_declare_tree(el, cn, cp, ct, kind, f, domain, attrs, loc);
        }
        return p;
    }
    {
        IrSigKind k = kind;
        uint32_t sig;
        IrType *irt = el_irty(el, t);
        if (!irt) {
            el_error(el, loc, "%s is not a hardware type", ty_str(t));
            irt = ir_type_bool(el->design);
        }
        if ((kind == IRS_IN || kind == IRS_OUT) && flipped)
            k = kind == IRS_IN ? IRS_OUT : IRS_IN;
        sig = ir_add_signal(el->design, m, intern_cstr(name), intern_cstr(path), k, irt, loc);
        m->sigs[sig].domain = domain;
        m->sigs[sig].flipped = flipped;
        if (t->kind == TY_TRI)
            ir_signal_add_attr(el->design, m, sig, "tri", NULL);
        if (attrs)
            elb_sig_attrs(el, sig, attrs);
        p->kind = PL_SIG;
        p->sig = sig;
        p->lo = 0;
        p->width = irt->width;
        return p;
    }
}

int el_is_signal_place(const Place *p)
{
    return p && (p->kind == PL_SIG || p->kind == PL_AGG || p->kind == PL_DYN);
}

static void elb_mark_driven(Elab *el, uint32_t sig, Span at, const char *how)
{
    ModCtx *mc = el->mod;
    map_put(&mc->sig_driven, sig, 1);
    if (map_has(&mc->pending, sig)) {
        uint64_t k;
        map_get(&mc->pending, sig, &k);
        mc->pendings.data[k - 1].assigned = 1;
    }
    sema_info_set_driver(el->S, mc->ir->sigs[sig].loc, intern_cstr(how), at);
}

/* a module-level name declared twice in one scope */
static int elb_redeclared(Elab *el, Env *env, const char *nm, Span def)
{
    Binding *old = nm ? ce_lookup_local_binding(env, nm) : NULL;
    if (!old || (old->def.file == def.file && old->def.off == def.off))
        return 0;
    if (old->def.file == def.file && old->def.off > def.off) {
        /* report the later one in source order */
        el_error(el, old->def, "`%s` is declared twice in this module", nm);
        el_note(el, def, "first declared here");
    } else {
        el_error(el, def, "`%s` is declared twice in this module", nm);
        if (old->def.file)
            el_note(el, old->def, "first declared here");
    }
    return 1;
}

/* --------------------------------------------------------- assignment -- */

static void elb_assign_leaf(Elab *el, Place *p, Value v, Expr *src, Expr *lhs, Span at)
{
    ModCtx *mc = el->mod;
    IrModule *m;
    IrSignal *s;
    IrExpr *rhs;
    IrLvalue lv;
    const char *what;
    if (!mc || p->sig == IR_NONE)
        return;
    m = mc->ir;
    s = &m->sigs[p->sig];
    what = lhs ? arena_printf(&el->arena, "`%s`", el_text(lhs))
               : arena_printf(&el->arena, "`%s`", s->path);
    v = elab_convert(el, v, p->ty, src, what, src ? src->span : at);
    if (v.kind == VAL_ERROR)
        return;
    if (el->in_fn) {
        el_error(el, at, "a function cannot assign signals");
        return;
    }
    if (s->kind == IRS_MEM) {
        if (!el->blk || !el->blk->clocked) {
            el_error(el, at, "memory `%s` can only be written inside an `on` block", s->name);
            return;
        }
        if (!p->index) {
            el_error(el, at, "a memory has no whole-value assignment; write one entry: `%s[addr] = ..`", s->name);
            return;
        }
        rhs = el_to_ir(el, &v, at);
        ir_stmts_push(el->blk->out, ir_stmt_memwrite(el->design->arena, m, p->sig, p->index, rhs, NULL, at));
        return;
    }
    if (s->kind == IRS_IN) {
        el_error(el, at, "cannot assign to input port `%s`", s->path);
        if (s->path && strchr(s->path, '.'))
            el_help(el, "`%s` flows into this module: on an `in` bundle port only the `flip` fields are outputs",
                    s->path);
        else
            el_help(el, "inputs are driven by the instantiating module; for a value computed here use an `out` "
                        "port or a `let`/`var`");
        return;
    }
    if (map_has(&mc->inst_out, p->sig)) {
        el_error(el, at, "cannot assign to `%s`: it is an output of the instance", s->path);
        return;
    }
    if (map_has(&mc->let_wire, p->sig)) {
        el_error(el, at, "cannot assign to `%s`: a `let` is assigned once, at its declaration", s->path);
        el_help(el, "for a value assigned in a block, declare `var %s: ...` (comb) or `reg %s: ...` (on)", s->name,
                s->name);
        return;
    }
    rhs = el_to_ir(el, &v, at);
    if (p->index) {
        lv = ir_lv_index(m, p->sig, p->lo, p->elem_width, p->index);
        lv.len = p->len;
    } else
        lv = ir_lv_slice(m, p->sig, p->lo, p->width);
    lv.field = p->field;
    if (el->blk) {
        if (el->blk->clocked) {
            if (s->kind != IRS_REG) {
                el_error(el, at, "`%s` is not a register: assign it in a `comb` block or at module level", s->path);
                if (s->kind == IRS_VAR || s->kind == IRS_OUT)
                    el_help(el, "inside `on`, only `reg`s can be assigned; declare `reg %s` for a flip-flop", s->name);
                return;
            }
            if (s->domain != el->blk->domain && s->domain != IR_NONE) {
                el_error(el, at, "register `%s` belongs to domain `%s` but is assigned in `on %s`", s->path,
                         elb_domain_name(el, s->domain), elb_domain_name(el, el->blk->domain));
                return;
            }
        } else if (s->kind == IRS_REG) {
            el_error(el, at, "register `%s` can only be assigned in an `on` block", s->path);
            el_help(el, "a `comb` block assigns `var`s and outputs: declare `var %s: ...` for a combinational "
                        "value, or assign the register in `on`",
                    s->name);
            return;
        }
        ir_stmts_push(el->blk->out, ir_stmt_assign(el->design->arena, lv, rhs, at));
        vec_push(el->blk->assigned, p->sig);
        elb_mark_driven(el, p->sig, at, el->blk->clocked ? arena_printf(&el->arena, "on %s",
                                                                       elb_domain_name(el, el->blk->domain))
                                                         : "comb");
        return;
    }
    if (s->kind == IRS_REG) {
        el_error(el, at, "register `%s` can only be assigned in an `on` block", s->path);
        el_help(el, "write `on %s { %s = ... }`", elb_domain_name(el, s->domain) ? elb_domain_name(el, s->domain) : "clk",
                s->name);
        return;
    }
    ir_add_assign(el->design, m, lv, rhs, at);
    elb_mark_driven(el, p->sig, at, "continuous");
}

void el_assign(Elab *el, Place *p, Value v, Expr *src, Expr *lhs, Span at)
{
    uint32_t i;
    if (!p || v.kind == VAL_ERROR)
        return;
    if (p->kind == PL_LOCAL) {
        el_assign_local(el, p, v, at);
        return;
    }
    if (p->kind == PL_DYN) {
        /* write decode: assign the element the index selects */
        IrStmtList *saved;
        IrCaseArm *arms;
        uint32_t k, na = 0, iw = p->index->width;
        if (!el->blk) {
            el_error(el, at, "an assignment through a signal index into an array of bundles must be inside a `comb` or `on` block");
            return;
        }
        saved = el->blk->out;
        arms = (IrCaseArm *)xcalloc(p->n + 1, sizeof(IrCaseArm));
        for (k = 0; k < p->n; k++) {
            IrStmtList l = {0};
            if (iw < 32 && k >= (1u << iw))
                break;
            el->blk->out = &l;
            el_assign(el, p->subs[k], v, src, lhs, at);
            el->blk->out = saved;
            arms[na].nvals = 1;
            arms[na].vals = ANEWN(el->design->arena, IrBits, 1);
            arms[na].vals[0] = ir_bits_u64(el->design->arena, iw, k);
            arms[na].body = ir_stmts_finish(el->design->arena, &l);
            na++;
        }
        if (iw >= 32 || na < (1u << iw))
            na++; /* default: an index past the array writes nothing */
        ir_stmts_push(el->blk->out, ir_stmt_case(el->design->arena, p->index, na, arms, at));
        free(arms);
        return;
    }
    if (p->kind == PL_AGG) {
        Ty *t = p->ty;
        if (v.kind != VAL_BUNDLE && v.kind != VAL_ARRAY) {
            el_error(el, at, "cannot assign a %s to %s", v.ty ? ty_str(v.ty) : val_kind_name((ValKind)v.kind),
                     ty_str(t));
            return;
        }
        if (v.ty != t && !(v.kind == VAL_ARRAY && v.u.list->n == p->n)) {
            el_error(el, at, "type mismatch: %s%s%s is %s, %s is %s", src ? "`" : "", src ? el_text(src) : "value",
                     src ? "`" : "", v.ty ? ty_str(v.ty) : "?", lhs ? el_text(lhs) : "the target", ty_str(t));
            return;
        }
        for (i = 0; i < p->n && i < v.u.list->n; i++) {
            Value fv = v.u.list->v[i];
            int flip = t->kind == TY_BUNDLE && t->fields[i].flip;
            if (fv.kind == VAL_VOID)
                continue;
            if (!flip) {
                el_assign(el, p->subs[i], fv, NULL, NULL, at);
            } else if (fv.place) {
                /* flipped field flows the other way: rhs.f = lhs.f */
                el_assign(el, fv.place, el_read_place(el, p->subs[i]), NULL, NULL, at);
            }
        }
        return;
    }
    elb_assign_leaf(el, p, v, src, lhs, at);
}

/* ------------------------------------------------------------- domains -- */

static uint32_t elb_domain_for_clock(Elab *el, uint32_t clk, int create)
{
    IrModule *m = el->mod->ir;
    uint32_t i;
    if (m->default_domain != IR_NONE && m->domains[m->default_domain].clock == clk)
        return m->default_domain;
    for (i = 0; i < m->ndomains; i++)
        if (m->domains[i].clock == clk)
            return i;
    if (!create)
        return IR_NONE;
    /* an implicit, reset-less domain named after its clock */
    return ir_add_domain(el->design, m, m->sigs[clk].name, clk, IR_NONE, 0);
}

uint32_t el_domain_of(Elab *el, Expr *e, Env *env, Span at)
{
    Value v = elab_eval(el, e, env, NULL);
    IrModule *m = el->mod->ir;
    if (v.kind == VAL_ERROR)
        return IR_NONE;
    if (v.kind == VAL_DOMAIN)
        return v.u.dom->idx;
    if (v.kind == VAL_HW && v.ty && v.ty->kind == TY_CLOCK && v.u.e->kind == IRE_SIG)
        return elb_domain_for_clock(el, v.u.e->sig, 1);
    if (v.kind == VAL_HW && v.ty && v.ty->kind == TY_CLOCK) {
        /* a clock expression: give it a wire */
        uint32_t w = ir_add_signal(el->design, m, elb_name(el, env, "clk_expr"), NULL, IRS_WIRE,
                                   el_irty(el, v.ty), at);
        ir_add_assign(el->design, m, ir_lv_sig(m, w), v.u.e, at);
        return elb_domain_for_clock(el, w, 1);
    }
    el_error(el, at, "`%s` is not a clock or a domain", el_text(e));
    return IR_NONE;
}

Value el_domain_field(Elab *el, Value dom, const char *name, Span at)
{
    IrModule *m = el->mod ? el->mod->ir : NULL;
    IrDomain *d;
    uint32_t sig;
    Value v;
    if (!m)
        return val_error();
    d = &m->domains[dom.u.dom->idx];
    if (strcmp(name, "clock") == 0)
        sig = d->clock;
    else if (strcmp(name, "reset") == 0) {
        sig = d->reset;
        if (sig == IR_NONE) {
            el_error(el, at, "domain `%s` has no reset", d->name);
            return val_error();
        }
    } else {
        el_error(el, at, "a domain has the fields `clock` and `reset`, not `%s`", name);
        return val_error();
    }
    {
        IrType *it = m->sigs[sig].type;
        Ty *t = it->kind == IRT_CLOCK ? ty_clock(el->S, it->falling) : ty_reset(el->S, it->async, it->active_low);
        v = el_hw(el, t, ir_sig(el->design->arena, m, sig, at));
    }
    return v;
}

static uint32_t elb_default_domain(Elab *el, Span at, const char *what)
{
    IrModule *m = el->mod->ir;
    if (m->default_domain != IR_NONE)
        return m->default_domain;
    if (m->ndomains == 0 && el->mod->clock_ports.len == 0) {
        el_error(el, at, "%s needs a clock, but module `%s` has no clock port", what, m->tern_name);
        el_help(el, "add `in clk: clock` (and `in rst: reset`)");
    } else {
        el_error(el, at, "%s needs a domain: module `%s` has %u clock ports", what, m->tern_name,
                 (unsigned)el->mod->clock_ports.len);
        el_help(el, "say which with `@ <domain or clock>`");
    }
    return IR_NONE;
}

/* --------------------------------------------------------------- ports -- */

static void elb_declare_port(Elab *el, Stmt *s, Env *env)
{
    ModCtx *mc = el->mod;
    Ty *t = elab_eval_type(el, s->u.port.type, env);
    const char *name = s->u.port.name.name;
    Place *p;
    Binding *b;
    ModPort mp;
    if (!name || elb_redeclared(el, env, name, s->u.port.name.span))
        return;
    if (t->kind != TY_ERROR && !t->hw && t->kind != TY_BUNDLE && !(t->kind == TY_ARRAY && ty_has_bundle(t))) {
        el_error(el, s->u.port.type->span, "port `%s` has type %s, which is not a hardware type", name, ty_str(t));
        t = el->S->t_error;
    }
    if (t->kind == TY_ERROR) {
        b = elab_env_bind(el, env, name, val_error(), s->u.port.name.span);
        b->sem = s->u.port.dir == PORT_IN ? SEM_PORT_IN : SEM_PORT_OUT;
        return;
    }
    if (t->kind == TY_TRI && !mc->is_top && !mc->is_extern && !elb_find_attr(&mc->item->attrs, "pad")) {
        el_error(el, s->u.port.type->span, "tri-state port `%s` on a module that is not #[top]", name);
        el_help(el, "tri<T> ports exist only on #[top] modules (and extern pad primitives); internal tri-states are forbidden");
    }
    p = elb_declare_tree(el, name, name, t, s->u.port.dir == PORT_IN ? IRS_IN : IRS_OUT, 0, IR_NONE, &s->attrs,
                         s->u.port.name.span);
    b = elab_env_bind(el, env, name, el_read_place(el, p), s->u.port.name.span);
    b->sem = s->u.port.dir == PORT_IN ? SEM_PORT_IN : SEM_PORT_OUT;
    if (p->kind == PL_SIG && s->u.port.dir == PORT_IN) {
        if (t->kind == TY_CLOCK)
            vec_push(mc->clock_ports, p->sig);
        else if (t->kind == TY_RESET)
            vec_push(mc->reset_ports, p->sig);
    }
    memset(&mp, 0, sizeof mp);
    mp.name = name;
    mp.ty = t;
    mp.dir = s->u.port.dir;
    mp.place = p;
    mp.domain = IR_NONE;
    mp.is_clock = t->kind == TY_CLOCK;
    mp.is_reset = t->kind == TY_RESET;
    mp.loc = s->u.port.name.span;
    {
        ModInfo *mi = mc->mi;
        ModPort *np = ANEWN(&el->arena, ModPort, mi->nports + 1);
        if (mi->nports)
            memcpy(np, mi->ports, mi->nports * sizeof(ModPort));
        np[mi->nports++] = mp;
        mi->ports = np;
    }
    el_record_decl(el, s->u.port.name.span, name, b->sem, t, NULL, NULL);
}

static void elb_set_domain_tree(IrModule *m, Place *p, uint32_t d)
{
    uint32_t i;
    if (!p)
        return;
    if (p->kind == PL_SIG) {
        IrType *t = m->sigs[p->sig].type;
        if (t->kind != IRT_CLOCK)
            m->sigs[p->sig].domain = d;
        return;
    }
    for (i = 0; i < p->n; i++)
        elb_set_domain_tree(m, p->subs[i], d);
}

static uint64_t elb_clock_hz(Elab *el, uint32_t sig, Stmt **body, uint32_t n)
{
    uint32_t i;
    IrModule *m = el->mod->ir;
    for (i = 0; i < n; i++) {
        Stmt *s = body[i];
        Attr *a;
        if (s->kind != STMT_PORT || s->u.port.name.name != m->sigs[sig].name)
            continue;
        a = elb_find_attr(&s->attrs, "clock");
        if (a) {
            uint32_t k;
            for (k = 0; k < a->nargs; k++)
                if (a->args[k].name.name && strcmp(a->args[k].name.name, "hz") == 0) {
                    Value v = elab_eval_const(el, a->args[k].value, el->mod->scope, el->S->t_int, "a frequency");
                    uint64_t hz = 0;
                    if (val_get_u64(&v, &hz))
                        return hz;
                }
        }
    }
    return 0;
}

/* ------------------------------------------------------- declarations -- */

/* a `flip` field anywhere in a (nested) bundle type: such a bundle has
 * parts flowing both ways and cannot be registered */
static const char *elb_flip_field(const Ty *t)
{
    uint32_t i;
    if (t->kind == TY_ARRAY)
        return elb_flip_field(t->elem);
    if (t->kind != TY_BUNDLE)
        return NULL;
    for (i = 0; i < t->nfields; i++) {
        const char *f;
        if (t->fields[i].flip)
            return t->fields[i].name;
        if ((f = elb_flip_field(t->fields[i].ty)) != NULL)
            return f;
    }
    return NULL;
}

/* reset values of a register of bundle type, field by field */
static int elb_init_tree(Elab *el, Place *p, Value v)
{
    uint32_t i;
    int ok = 1;
    if (v.kind == VAL_ERROR || v.kind == VAL_VOID)
        return 1;
    if (p->kind == PL_SIG) {
        if (!val_is_const(&v))
            return 0;
        ir_set_init(el->design, el->mod->ir, p->sig, elab_const_bits(el, &v, p->ty));
        return 1;
    }
    if (p->kind != PL_AGG)
        return 1;
    for (i = 0; i < p->n; i++) {
        Value fv;
        if (p->ty->kind == TY_BUNDLE) {
            if ((v.kind != VAL_BUNDLE && v.kind != VAL_STRUCT) || !v.u.list || i >= v.u.list->n)
                return ok;
            fv = v.u.list->v[i];
        } else {
            fv = el_index(el, v, val_int(el->S->t_int, (int64_t)i), span_none(), NULL);
        }
        ok &= elb_init_tree(el, p->subs[i], fv);
    }
    return ok;
}

static Value elb_force_reg(Elab *el, Binding *b, Stmt *s, Env *env)
{
    IrModule *m = el->mod->ir;
    Ty *t = elab_eval_type(el, s->u.reg.type, env);
    uint32_t dom;
    const char *name = elb_name(el, env, s->u.reg.name.name);
    Place *p;
    Value v;
    if (t->kind == TY_ERROR)
        return val_error();
    if (ty_has_bundle(t) && elb_flip_field(t)) {
        /* §8.1: a bundle whose fields all flow one way is a group of
         * registers; a `flip` field flows against the others */
        el_error(el, s->u.reg.type->span, "register `%s` cannot hold %s: its field `%s` is `flip`", s->u.reg.name.name,
                 ty_str(t), elb_flip_field(t));
        el_help(el, "register the forward fields (a bundle without `flip` fields, e.g. Valid<T>, or a struct); "
                    "the flipped ones flow the other way");
        return val_error();
    }
    if (ty_has_bundle(t) && t->kind != TY_BUNDLE && t->kind != TY_ARRAY) {
        el_error(el, s->u.reg.type->span, "register `%s` cannot hold %s", s->u.reg.name.name, ty_str(t));
        return val_error();
    }
    if (!t->hw && !ty_has_bundle(t)) {
        el_error(el, s->u.reg.type->span, "register `%s` has type %s, which is not a hardware type", s->u.reg.name.name,
                 ty_str(t));
        return val_error();
    }
    if (t->kind == TY_CLOCK || t->kind == TY_RESET) {
        el_error(el, s->u.reg.type->span, "a register cannot hold a %s", ty_str(t));
        return val_error();
    }
    dom = s->u.reg.domain ? el_domain_of(el, s->u.reg.domain, env, s->u.reg.domain->span)
                          : elb_default_domain(el, s->span, arena_printf(&el->arena, "register `%s`", s->u.reg.name.name));
    p = elb_declare_tree(el, name, name, t, IRS_REG, 0, dom, &s->attrs, s->u.reg.name.span);
    if (s->u.reg.init && p->kind == PL_AGG) {
        /* one register per field; the literal gives each its reset value */
        Value iv = elab_eval(el, s->u.reg.init, env, t);
        if (iv.kind != VAL_ERROR)
            iv = elab_convert(el, iv, t, s->u.reg.init,
                              arena_printf(&el->arena, "register `%s`", s->u.reg.name.name), s->u.reg.init->span);
        if (iv.kind != VAL_ERROR && !elb_init_tree(el, p, iv)) {
            el_error(el, s->u.reg.init->span, "the reset value of `%s` must be a compile-time constant",
                     s->u.reg.name.name);
            el_help(el, "a reset value is a constant; to load a signal, assign it in the `on` block");
        }
    } else if (s->u.reg.init && p->kind == PL_SIG) {
        Value iv = elab_eval(el, s->u.reg.init, env, t);
        if (iv.kind != VAL_ERROR) {
            iv = elab_convert(el, iv, t, s->u.reg.init,
                              arena_printf(&el->arena, "register `%s`", s->u.reg.name.name), s->u.reg.init->span);
            if (iv.kind != VAL_ERROR && !val_is_const(&iv)) {
                el_error(el, s->u.reg.init->span, "the reset value of `%s` must be a compile-time constant",
                         s->u.reg.name.name);
                el_help(el, "a reset value is a constant; to load a signal, assign it in the `on` block");
            } else if (iv.kind != VAL_ERROR) {
                ir_set_init(el->design, m, p->sig, elab_const_bits(el, &iv, t));
            }
        }
    }
    v = el_read_place(el, p);
    b->sem = SEM_REG;
    el_record_decl(el, s->u.reg.name.span, s->u.reg.name.name, SEM_REG, t, elb_domain_name(el, dom), NULL);
    return v;
}

static Value elb_force_var(Elab *el, Binding *b, Stmt *s, Env *env)
{
    Ty *t = s->u.var.type ? elab_eval_type(el, s->u.var.type, env) : NULL;
    const char *name = elb_name(el, env, s->u.var.name.name);
    Value init = val_error();
    Place *p;
    if (s->u.var.init) {
        init = elab_eval(el, s->u.var.init, env, t && t->kind != TY_ERROR ? t : NULL);
        if (!t && init.kind != VAL_ERROR)
            t = init.ty;
    }
    if (!t || t->kind == TY_ERROR) {
        if (!t)
            el_error(el, s->span, "`var %s` needs a type", s->u.var.name.name);
        return val_error();
    }
    if (t->kind == TY_INT) {
        el_error(el, s->span, "`var %s` needs a sized type, not a widthless integer", s->u.var.name.name);
        return val_error();
    }
    p = elb_declare_tree(el, name, name, t, IRS_VAR, 0, IR_NONE, &s->attrs, s->u.var.name.span);
    if (s->u.var.init && init.kind != VAL_ERROR) {
        init = elab_convert(el, init, t, s->u.var.init, arena_printf(&el->arena, "`%s`", s->u.var.name.name),
                            s->u.var.init->span);
        if (init.kind != VAL_ERROR && p->kind == PL_SIG) {
            VarDefault vd;
            vd.sig = p->sig;
            vd.val = init;
            vd.loc = s->u.var.init->span;
            vec_push(el->mod->var_defaults, vd);
        }
    }
    b->sem = SEM_VAR;
    el_record_decl(el, s->u.var.name.span, s->u.var.name.name, SEM_VAR, t, NULL, NULL);
    return el_read_place(el, p);
}

static Value elb_force_mem(Elab *el, Binding *b, Stmt *s, Env *env)
{
    IrModule *m = el->mod->ir;
    Ty *t = elab_eval_type(el, s->u.mem.type, env);
    const char *name = elb_name(el, env, s->u.mem.name.name);
    uint32_t sig;
    Value v;
    if (t->kind == TY_ERROR)
        return val_error();
    if (t->kind != TY_ARRAY || !t->elem->hw) {
        el_error(el, s->u.mem.type->span, "a memory needs an array type `[T; N]`, found %s", ty_str(t));
        return val_error();
    }
    sig = ir_add_memory(el->design, m, name, el_irty(el, t->elem), t->len, s->u.mem.name.span);
    elb_sig_attrs(el, sig, &s->attrs);
    if (s->u.mem.init) {
        Expr *ie = s->u.mem.init;
        Value iv = elab_eval(el, ie, env, t);
        if (iv.kind != VAL_ERROR)
            iv = elab_convert(el, iv, t, ie, arena_printf(&el->arena, "memory `%s`", s->u.mem.name.name), ie->span);
        if (iv.kind != VAL_ERROR && !val_is_const(&iv)) {
            el_error(el, ie->span, "the initial contents of `%s` must be compile-time constants", s->u.mem.name.name);
        } else if (iv.kind == VAL_ARRAY) {
            uint32_t i;
            IrBits *init = ANEWN(el->design->arena, IrBits, t->len);
            for (i = 0; i < t->len; i++)
                init[i] = elab_const_bits(el, &iv.u.list->v[i], t->elem);
            m->sigs[sig].mem_init = init;
        }
        if (ie->kind == EXPR_CALL && ie->u.call.callee->kind == EXPR_PATH && ie->u.call.nargs == 1 &&
            ie->u.call.args[0].value->kind == EXPR_STRING) {
            const char *fn = ie->u.call.callee->u.path.segs[0].name.name;
            if (fn && (strcmp(fn, "include_hex") == 0 || strcmp(fn, "include_bin") == 0)) {
                m->sigs[sig].mem_init_file = arena_strdup(el->design->arena, ie->u.call.args[0].value->u.string.value);
                m->sigs[sig].mem_init_is_bin = fn[8] == 'b';
            }
        }
    }
    memset(&v, 0, sizeof v);
    v.kind = VAL_MEM;
    v.ty = t;
    v.u.mem = ANEW(&el->arena, ValMem);
    v.u.mem->sig = sig;
    v.u.mem->elem = t->elem;
    v.u.mem->depth = t->len;
    v.u.mem->loc = s->u.mem.name.span;
    b->sem = SEM_MEM;
    el_record_decl(el, s->u.mem.name.span, s->u.mem.name.name, SEM_MEM, t, NULL, NULL);
    return v;
}

static Value elb_force_domain(Elab *el, Binding *b, Stmt *s, Env *env)
{
    IrModule *m = el->mod->ir;
    Value c = elab_eval(el, s->u.domain.clock, env, NULL);
    Value r = s->u.domain.reset ? elab_eval(el, s->u.domain.reset, env, NULL) : val_void(el->S);
    uint32_t csig = IR_NONE, rsig = IR_NONE, idx;
    Value v;
    if (c.kind == VAL_ERROR || r.kind == VAL_ERROR)
        return val_error();
    if (c.kind != VAL_HW || !c.ty || c.ty->kind != TY_CLOCK) {
        el_error(el, s->u.domain.clock->span, "the first element of a domain must be a clock, found %s",
                 c.ty ? ty_str(c.ty) : val_kind_name((ValKind)c.kind));
        return val_error();
    }
    if (c.u.e->kind == IRE_SIG)
        csig = c.u.e->sig;
    else {
        csig = ir_add_signal(el->design, m, elb_name(el, env, arena_printf(&el->arena, "%s_clock", s->u.domain.name.name)),
                             NULL, IRS_WIRE, el_irty(el, c.ty), s->span);
        ir_add_assign(el->design, m, ir_lv_sig(m, csig), c.u.e, s->span);
    }
    if (s->u.domain.reset) {
        if (r.kind != VAL_HW || !r.ty || r.ty->kind != TY_RESET) {
            el_error(el, s->u.domain.reset->span, "the second element of a domain must be a reset, found %s",
                     r.ty ? ty_str(r.ty) : val_kind_name((ValKind)r.kind));
            return val_error();
        }
        if (r.u.e->kind == IRE_SIG && m->sigs[r.u.e->sig].type->async == r.ty->async &&
            m->sigs[r.u.e->sig].type->active_low == r.ty->active_low)
            rsig = r.u.e->sig;
        else {
            /* an expression, or a signal converted to another style
             * (`rst_out as reset`): a wire of the domain's reset type */
            rsig = ir_add_signal(el->design, m,
                                 elb_name(el, env, arena_printf(&el->arena, "%s_reset", s->u.domain.name.name)), NULL,
                                 IRS_WIRE, el_irty(el, r.ty), s->span);
            ir_add_assign(el->design, m, ir_lv_sig(m, rsig), r.u.e, s->span);
        }
    }
    idx = ir_add_domain(el->design, m, elb_name(el, env, s->u.domain.name.name), csig, rsig,
                        elb_clock_hz(el, csig, el->mod->item->kind == ITEM_MODULE ? el->mod->item->u.module.body : NULL,
                                     el->mod->item->kind == ITEM_MODULE ? el->mod->item->u.module.nbody : 0));
    memset(&v, 0, sizeof v);
    v.kind = VAL_DOMAIN;
    v.u.dom = ANEW(&el->arena, ValDomain);
    v.u.dom->idx = idx;
    b->sem = SEM_DOMAIN;
    el_record_decl(el, s->u.domain.name.span, s->u.domain.name.name, SEM_DOMAIN, NULL, NULL, NULL);
    return v;
}

static int elb_is_instance_init(Elab *el, Expr *init, Env *env, Value *callee_out)
{
    Expr *callee;
    Value cv;
    int q;
    if (init->kind != EXPR_CALL)
        return 0;
    callee = init->u.call.callee;
    if (callee->kind != EXPR_PATH)
        return 0;
    q = el->quiet;
    el->quiet = 1;
    cv = el_eval_path(el, &callee->u.path, env, NULL, 0);
    el->quiet = q;
    el->quiet_errors = 0;
    if (cv.kind == VAL_MODULE ||
        (cv.kind == VAL_ITEM && (cv.u.fn->item->kind == ITEM_MODULE || cv.u.fn->item->kind == ITEM_EXTERN))) {
        /* evaluate again with diagnostics */
        *callee_out = el_eval_path(el, &callee->u.path, env, NULL, 0);
        return callee_out->kind != VAL_ERROR;
    }
    return 0;
}

static Value elb_force_let(Elab *el, Binding *b, Stmt *s, Env *env)
{
    IrModule *m = el->mod->ir;
    Pattern *pat = s->u.let.pat;
    Ty *t = s->u.let.type ? elab_eval_type(el, s->u.let.type, env) : NULL;
    Value callee, v;
    if (t && t->kind == TY_ERROR)
        t = NULL;
    if (s->u.let.constraint)
        el_error(el, s->u.let.constraint->span, "`where` on `let` is only allowed in tests");
    if (elb_is_instance_init(el, s->u.let.init, env, &callee)) {
        v = el_instance(el, s->u.let.init, env, pat->kind == PAT_BIND ? elb_name(el, env, pat->u.bind.name) : NULL,
                        s->span, callee);
        b->sem = SEM_INSTANCE;
        if (pat->kind == PAT_STRUCT) {
            uint32_t k;
            for (k = 0; k < pat->u.struct_pat.nfields; k++) {
                FieldPat *fp = &pat->u.struct_pat.fields[k];
                Binding *fb = ce_lookup_local_binding(env, fp->pat && fp->pat->kind == PAT_BIND ? fp->pat->u.bind.name
                                                                                              : fp->name.name);
                Value fv = v.kind == VAL_ERROR ? v : el_inst_field(el, v, fp->name.name, fp->name.span);
                if (fb) {
                    fb->val = fv;
                    fb->state = 2;
                    fb->sem = SEM_WIRE;
                }
            }
            return b->val.kind == VAL_VOID ? v : b->val;
        }
        return v;
    }
    v = elab_eval(el, s->u.let.init, env, t);
    if (t && v.kind != VAL_ERROR)
        v = elab_convert(el, v, t, s->u.let.init,
                         pat->kind == PAT_BIND ? arena_printf(&el->arena, "`%s`", pat->u.bind.name) : NULL,
                         s->u.let.init->span);
    if (pat->kind != PAT_BIND) {
        /* destructuring: bind the parts in the declaring scope */
        Env *tmp = elab_env_new(el, NULL, env->file);
        uint32_t k;
        el_bind_pattern(el, pat, v, tmp, 0);
        for (k = 0; k < tmp->n; k++) {
            Binding *fb = ce_lookup_local_binding(env, tmp->b[k].name);
            if (fb) {
                fb->val = tmp->b[k].val;
                fb->state = 2;
            }
        }
        elab_env_free(tmp);
        return b->val;
    }
    if (v.kind == VAL_HW && v.ty && v.ty->hw) {
        const char *name = elb_name(el, env, pat->u.bind.name);
        uint32_t w = ir_add_signal(el->design, m, name, NULL, IRS_WIRE, el_irty(el, v.ty), pat->u.bind.span);
        Value wv;
        Place *p;
        elb_sig_attrs(el, w, &s->attrs);
        ir_add_assign(el->design, m, ir_lv_sig(m, w), v.u.e, s->u.let.init->span);
        map_put(&el->mod->sig_driven, w, 1);
        map_put(&el->mod->let_wire, w, 1);
        p = ANEW(&el->arena, Place);
        p->kind = PL_SIG;
        p->sig = w;
        p->width = m->sigs[w].type->width;
        p->ty = v.ty;
        wv = el_read_place(el, p);
        b->sem = SEM_WIRE;
        el_record_decl(el, pat->u.bind.span, pat->u.bind.name, SEM_WIRE, v.ty, NULL, NULL);
        sema_info_set_driver(el->S, pat->u.bind.span, intern_cstr("continuous"), s->span);
        return wv;
    }
    b->sem = val_is_const(&v) ? SEM_CONST : SEM_WIRE;
    el_record_decl(el, pat->u.bind.span, pat->u.bind.name, (SemKind)b->sem, v.ty, NULL, &v);
    return v;
}

Value el_force_decl(Elab *el, Binding *b)
{
    Stmt *s = b->decl;
    Env *env = b->decl_env;
    const char *nm = b->name;
    struct BlkCtx *saved_blk = el->blk;
    int saved_fn = el->in_fn, saved_undo = (int)el->undo_depth;
    Value v;
    if (!s || !el->mod)
        return val_error();
    b->state = 1;
    el->blk = NULL;
    el->in_fn = 0;
    el->undo_depth = 0;
    {
        struct BlkCtx *saved_tb = el->temp_blk;
        el->temp_blk = NULL;
        switch (s->kind) {
        case STMT_REG: v = elb_force_reg(el, b, s, env); break;
        case STMT_VAR: v = elb_force_var(el, b, s, env); break;
        case STMT_MEM: v = elb_force_mem(el, b, s, env); break;
        case STMT_DOMAIN: v = elb_force_domain(el, b, s, env); break;
        case STMT_LET: v = elb_force_let(el, b, s, env); break;
        default: v = val_error(); break;
        }
        el->temp_blk = saved_tb;
    }
    el->blk = saved_blk;
    el->in_fn = saved_fn;
    el->undo_depth = (uint32_t)saved_undo;
    /* the binding array may have grown */
    b = ce_lookup_local_binding(env, nm);
    if (b) {
        uint8_t sem = b->sem;
        if (b->state != 2 || s->kind != STMT_LET || b->val.kind == VAL_VOID)
            b->val = v;
        b->state = 2;
        b->sem = sem;
        return b->val;
    }
    return v;
}

/* ----------------------------------------------------------- instances -- */

typedef struct ElbConnCtx {
    ValInst *vi;
    IrModule *child;
    uint32_t inst;
} ElbConnCtx;

/* Mirrors the child's place tree in the parent: one wire per leaf. Clock and
 * reset inputs given in `direct` are connected directly. */
static Place *elb_inst_tree(Elab *el, ElbConnCtx *cc, Place *cp, Ty *t, const char *pname, const char *ppath,
                            uint32_t domain, Span loc)
{
    IrModule *m = el->mod->ir;
    Place *p = ANEW(&el->arena, Place);
    uint32_t i;
    p->ty = t;
    if (cp->kind == PL_AGG) {
        p->kind = PL_AGG;
        p->n = cp->n;
        p->subs = ANEWN(&el->arena, Place *, cp->n ? cp->n : 1);
        for (i = 0; i < cp->n; i++) {
            const char *cn, *cpth;
            Ty *ct;
            if (t->kind == TY_BUNDLE) {
                cn = arena_printf(&el->arena, "%s_%s", pname, t->fields[i].name);
                cpth = arena_printf(&el->arena, "%s.%s", ppath, t->fields[i].name);
                ct = t->fields[i].ty;
            } else {
                cn = arena_printf(&el->arena, "%s_%u", pname, (unsigned)i);
                cpth = arena_printf(&el->arena, "%s[%u]", ppath, (unsigned)i);
                ct = t->elem;
            }
            p->subs[i] = elb_inst_tree(el, cc, cp->subs[i], ct, cn, cpth, domain, loc);
        }
        return p;
    }
    {
        IrSignal *cs = &cc->child->sigs[cp->sig];
        uint32_t w = ir_add_signal(el->design, m, intern_cstr(pname), intern_cstr(ppath), IRS_WIRE, cs->type, loc);
        m->sigs[w].domain = cs->type->kind == IRT_CLOCK ? IR_NONE : domain;
        p->kind = PL_SIG;
        p->sig = w;
        p->width = cs->type->width;
        if (cs->kind == IRS_OUT) {
            ir_connect(el->design, m, cc->inst, cp->sig, ir_sig(el->design->arena, m, w, loc));
            map_put(&el->mod->inst_out, w, 1);
            map_put(&el->mod->sig_driven, w, 1);
        } else {
            PendingIn pi;
            ir_connect(el->design, m, cc->inst, cp->sig, ir_sig(el->design->arena, m, w, loc));
            pi.sig = w;
            pi.inst = cc->vi;
            pi.port = cs->path;
            pi.assigned = 0;
            vec_push(el->mod->pendings, pi);
            map_put(&el->mod->pending, w, el->mod->pendings.len);
        }
        return p;
    }
}

static void elb_mark_tree_assigned(Elab *el, Place *p)
{
    uint32_t i;
    uint64_t k;
    if (!p)
        return;
    if (p->kind == PL_SIG) {
        if (map_get(&el->mod->pending, p->sig, &k))
            el->mod->pendings.data[k - 1].assigned = 1;
        return;
    }
    for (i = 0; i < p->n; i++)
        elb_mark_tree_assigned(el, p->subs[i]);
}

static int elb_find_port(ModInfo *mi, const char *name)
{
    uint32_t i;
    for (i = 0; i < mi->nports; i++)
        if (mi->ports[i].name == name)
            return (int)i;
    return -1;
}

/* help line for an unknown port: the closest port name, else the ports */
static void elb_port_help(Elab *el, ModInfo *mi, const char *name)
{
    const char *names[128] = {0};
    size_t n = 0;
    uint32_t i;
    const char *best;
    StrBuf sb = {0};
    for (i = 0; i < mi->nports && n < TERN_ARRAY_LEN(names); i++)
        names[n++] = mi->ports[i].name;
    best = strlen(name) > 2 ? el_closest(name, names, n) : NULL; /* `c` is "close" to every short name */
    if (best) {
        el_help(el, "did you mean `%s`?", best);
        return;
    }
    for (i = 0; i < n && i < 12; i++)
        sb_printf(&sb, "%s`%s`", i ? ", " : "", names[i]);
    if (n > 12)
        sb_puts(&sb, ", ...");
    if (n)
        el_help(el, "its ports are %s", sb.data);
    sb_free(&sb);
}

/* the parent domain whose clock drives the clock port of child domain cd */
static uint32_t elb_map_child_domain(Elab *el, ModInfo *mi, IrExpr **clk_conn, uint32_t cd, uint32_t fallback)
{
    IrModule *c = mi->ir;
    uint32_t ck, i;
    if (cd == IR_NONE || cd >= c->ndomains)
        return fallback;
    ck = c->domains[cd].clock;
    for (i = 0; i < mi->nports; i++) {
        Place *pp = mi->ports[i].place;
        if (pp && pp->kind == PL_SIG && pp->sig == ck && clk_conn[i] && clk_conn[i]->kind == IRE_SIG) {
            IrModule *m = el->mod->ir;
            uint32_t d;
            /* the instance's own domain when it has this clock: two domains
             * may share a clock with different resets (`@ d`) */
            if (fallback != IR_NONE && fallback < m->ndomains && m->domains[fallback].clock == clk_conn[i]->sig)
                return fallback;
            d = elb_domain_for_clock(el, clk_conn[i]->sig, 0);
            return d != IR_NONE ? d : fallback;
        }
    }
    return fallback;
}

static int elb_pure_expr(const Expr *e)
{
    while (e && (e->kind == EXPR_FIELD || e->kind == EXPR_INDEX || e->kind == EXPR_PAREN))
        e = e->kind == EXPR_FIELD ? e->u.field.base : e->kind == EXPR_INDEX ? e->u.index.base : e->u.inner;
    return e && e->kind == EXPR_PATH;
}

/* Generic arguments of a module instantiated without `<..>`, inferred from
 * the types of the connected inputs (`Crc32(in: words)` with
 * `in in: Stream<T>`); generics that cannot be inferred keep their defaults.
 * Returns the number of inferred arguments. */
static uint32_t elb_infer_generics(Elab *el, Item *it, FileUnit *file, Expr *call, Env *env, ElabArgs *out)
{
    GenericParams *gp = it->kind == ITEM_MODULE ? &it->u.module.generics : &it->u.extern_mod.generics;
    Stmt **body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
    uint32_t nbody = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody;
    uint32_t i, k, n = 0;
    Env *genv;
    memset(out, 0, sizeof *out);
    if (!gp->n || !call->u.call.nargs)
        return 0;
    if (!file)
        file = sema_file_of_item(el->S, it);
    genv = elab_env_new(el, NULL, file);
    for (i = 0; i < gp->n; i++) {
        Value ph = val_void(el->S);
        ph.aux = 1;
        elab_env_bind(el, genv, gp->items[i].name.name, ph, gp->items[i].name.span)->sem = SEM_GENERIC;
    }
    for (i = 0; i < call->u.call.nargs; i++) {
        Arg *a = &call->u.call.args[i];
        const char *pn = a->name.name;
        Stmt *port = NULL;
        Value v;
        int q;
        if (!pn && a->value->kind == EXPR_PATH && a->value->u.path.nsegs == 1)
            pn = a->value->u.path.segs[0].name.name;
        if (!pn || !elb_pure_expr(a->value))
            continue;
        for (k = 0; k < nbody; k++)
            if (body[k]->kind == STMT_PORT && body[k]->u.port.dir == PORT_IN && body[k]->u.port.name.name == pn)
                port = body[k];
        if (!port)
            continue;
        q = el->quiet;
        el->quiet = 1;
        v = elab_eval(el, a->value, env, NULL);
        el->quiet = q;
        el->quiet_errors = 0;
        if (v.kind != VAL_ERROR && v.ty)
            el_unify_type(el, port->u.port.type, v.ty, genv);
    }
    out->items = ANEWN(&el->arena, ElabArg, gp->n);
    for (i = 0; i < gp->n; i++) {
        Binding *b = ce_lookup_local_binding(genv, gp->items[i].name.name);
        if (!b || (b->val.kind == VAL_VOID && b->val.aux == 1) || b->val.kind == VAL_ERROR)
            continue;
        out->items[n].name = gp->items[i].name.name;
        out->items[n].val = b->val;
        out->items[n].span = call->span;
        n++;
    }
    out->n = n;
    elab_env_free(genv);
    return n;
}

Value el_instance(Elab *el, Expr *call, Env *env, const char *name, Span at, Value calleev)
{
    ModCtx *mc = el->mod;
    IrModule *m;
    ValModule *vm;
    ModInfo *mi;
    ValInst *vi;
    Value v;
    uint32_t i, inst, dom;
    IrExpr **conn;          /* direct connections of clock/reset ports */
    int *given;             /* port index -> arg index + 1 */
    Arg *args = call->u.call.args;
    uint32_t nargs = call->u.call.nargs;
    ElbConnCtx cc;
    char *frame;
    if (!mc || el->in_fn || el->blk || el->undo_depth) {
        el_error(el, at, "modules can only be instantiated at module level");
        return val_error();
    }
    m = mc->ir;
    if (calleev.kind == VAL_ERROR)
        return calleev;
    if (calleev.kind == VAL_MODULE)
        vm = calleev.u.mod;
    else if (calleev.kind == VAL_ITEM) {
        ElabArgs inferred;
        if (elb_infer_generics(el, calleev.u.fn->item, calleev.u.fn->file, call, env, &inferred))
            vm = elab_bind_module(el, calleev.u.fn->item, calleev.u.fn->file, &inferred, call->u.call.callee->span);
        else
            vm = elab_bind_module(el, calleev.u.fn->item, calleev.u.fn->file, NULL, call->u.call.callee->span);
    }
    else {
        el_error(el, call->u.call.callee->span, "`%s` is not a module", el_text(call->u.call.callee));
        return val_error();
    }
    if (!vm)
        return val_error();
    if (!name) {
        char buf[128];
        size_t k;
        snprintf(buf, sizeof buf, "%s_%u", vm->item->name.name, (unsigned)mc->ninline++);
        for (k = 0; buf[k]; k++)
            buf[k] = (char)tern_to_lower((unsigned char)buf[k]);
        name = elb_name(el, env, intern_cstr(buf));
    }
    /* the arguments as written (`rx_fifo: Fifo<u8, 64>`); inside a generic
     * module they may name its generics, so the evaluated ones (`M<3>`) */
    frame = arena_printf(&el->arena, "%s: %s", name,
                         el->mod && el->mod->vm && el->mod->vm->args.n ? vm->display
                                                                       : el_text(call->u.call.callee));
    diag_push_path(frame);
    mi = elab_module(el, vm, call->u.call.callee->span);
    diag_pop_path();
    if (!mi || !mi->ir)
        return val_error();
    /* the instance's domain */
    if (call->u.call.domain)
        dom = el_domain_of(el, call->u.call.domain, env, call->u.call.domain->span);
    else
        dom = m->default_domain;
    conn = (IrExpr **)xcalloc(mi->nports ? mi->nports : 1, sizeof(IrExpr *));
    given = (int *)xcalloc(mi->nports ? mi->nports : 1, sizeof(int));
    /* map arguments to ports */
    for (i = 0; i < nargs; i++) {
        const char *pn = args[i].name.name;
        int k;
        if (!pn) {
            Expr *a = args[i].value;
            if (a->kind == EXPR_PATH && a->u.path.nsegs == 1 && !a->u.path.segs[0].has_args)
                pn = a->u.path.segs[0].name.name;
            else {
                el_error(el, args[i].span, "instance connections are by name: `port: value`");
                continue;
            }
        }
        k = elb_find_port(mi, pn);
        if (k < 0) {
            el_error(el, args[i].name.name ? args[i].name.span : args[i].span, "module `%s` has no port `%s`",
                     vm->display, pn);
            elb_port_help(el, mi, pn);
            continue;
        }
        if (given[k]) {
            el_error(el, args[i].span, "port `%s` is connected twice", pn);
            continue;
        }
        if (mi->ports[k].dir == PORT_OUT) {
            el_error(el, args[i].span, "`%s` is an output of `%s`; read it as `%s.%s`", pn, vm->display, name, pn);
            continue;
        }
        given[k] = (int)i + 1;
    }
    /* clocks first, then resets (which may follow a connected clock's domain) */
    for (i = 0; i < mi->nports; i++) {
        ModPort *pp = &mi->ports[i];
        if (!pp->is_clock || pp->place->kind != PL_SIG || pp->dir != PORT_IN)
            continue;
        if (given[i]) {
            Value cv = elab_eval(el, args[given[i] - 1].value, env, pp->ty);
            if (cv.kind == VAL_HW && cv.ty && cv.ty->kind == TY_CLOCK)
                conn[i] = cv.u.e;
            else if (cv.kind != VAL_ERROR)
                el_error(el, args[given[i] - 1].value->span, "port `%s` needs a clock, found %s", pp->name,
                         cv.ty ? ty_str(cv.ty) : val_kind_name((ValKind)cv.kind));
        } else if (dom != IR_NONE) {
            conn[i] = ir_sig(el->design->arena, m, m->domains[dom].clock, at);
        }
    }
    for (i = 0; i < mi->nports; i++) {
        ModPort *pp = &mi->ports[i];
        if (!pp->is_reset || pp->place->kind != PL_SIG || pp->dir != PORT_IN)
            continue;
        if (given[i]) {
            Value rv = elab_eval(el, args[given[i] - 1].value, env, pp->ty);
            if (rv.kind == VAL_HW && rv.ty && rv.ty->kind == TY_RESET) {
                if (rv.ty != pp->ty) {
                    el_error(el, args[given[i] - 1].value->span, "reset style mismatch: `%s` is %s, port `%s` is %s",
                             el_text(args[given[i] - 1].value), ty_str(rv.ty), pp->name, ty_str(pp->ty));
                    el_help(el, "convert explicitly: `%s as %s`", el_text(args[given[i] - 1].value), ty_str(pp->ty));
                    conn[i] = rv.u.e; /* no follow-up errors */
                } else {
                    conn[i] = rv.u.e;
                }
            } else if (rv.kind != VAL_ERROR) {
                el_error(el, args[given[i] - 1].value->span, "port `%s` needs a reset, found %s", pp->name,
                         rv.ty ? ty_str(rv.ty) : val_kind_name((ValKind)rv.kind));
            }
        } else {
            /* the parent domain of the child's domain using this reset */
            uint32_t pd = dom, cd;
            IrModule *c = mi->ir;
            for (cd = 0; cd < c->ndomains; cd++)
                if (c->domains[cd].reset == pp->place->sig) {
                    pd = elb_map_child_domain(el, mi, conn, cd, dom);
                    break;
                }
            if (pd != IR_NONE && m->domains[pd].reset != IR_NONE) {
                IrSignal *rs = &m->sigs[m->domains[pd].reset];
                Ty *rt = ty_reset(el->S, rs->type->async, rs->type->active_low);
                if (rt != pp->ty) {
                    el_error(el, at, "instance `%s`: reset port `%s` is %s, but the inherited reset `%s` is %s", name,
                             pp->name, ty_str(pp->ty), rs->path ? rs->path : rs->name, ty_str(rt));
                    el_help(el, "connect it explicitly: `%s: %s as %s`", pp->name, rs->path ? rs->path : rs->name,
                            ty_str(pp->ty));
                    conn[i] = ir_sig(el->design->arena, m, m->domains[pd].reset, at); /* no follow-up errors */
                } else {
                    conn[i] = ir_sig(el->design->arena, m, m->domains[pd].reset, at);
                }
            }
        }
    }
    inst = ir_add_instance(el->design, m, name, mi->ir, at);
    vi = ANEW(&el->arena, ValInst);
    vi->name = name;
    vi->mi = mi;
    vi->inst = inst;
    vi->domain = dom;
    vi->loc = at;
    vi->ports = ANEWN(&el->arena, Value, mi->nports ? mi->nports : 1);
    cc.vi = vi;
    cc.child = mi->ir;
    cc.inst = inst;
    for (i = 0; i < mi->nports; i++) {
        ModPort *pp = &mi->ports[i];
        uint32_t pdom = mi->ir->ndomains ? elb_map_child_domain(el, mi, conn, pp->domain, dom) : IR_NONE;
        if ((pp->is_clock || pp->is_reset) && pp->dir == PORT_IN && pp->place->kind == PL_SIG && conn[i]) {
            ir_connect(el->design, m, inst, pp->place->sig, conn[i]);
            vi->ports[i] = el_hw(el, pp->ty, conn[i]);
            continue;
        }
        {
            Place *p = elb_inst_tree(el, &cc, pp->place, pp->ty, arena_printf(&el->arena, "%s_%s", name, pp->name),
                                     arena_printf(&el->arena, "%s.%s", name, pp->name), pdom, at);
            vi->ports[i] = el_read_place(el, p);
        }
    }
    memset(&v, 0, sizeof v);
    v.kind = VAL_INST;
    v.u.inst = vi;
    /* data connections */
    for (i = 0; i < mi->nports; i++) {
        ModPort *pp = &mi->ports[i];
        Value av;
        Expr *ae;
        if (!given[i] || ((pp->is_clock || pp->is_reset) && pp->dir == PORT_IN && pp->place->kind == PL_SIG))
            continue;
        ae = args[given[i] - 1].value;
        av = elab_eval(el, ae, env, pp->ty);
        if (av.kind != VAL_ERROR && vi->ports[i].place)
            el_assign(el, vi->ports[i].place, av, ae, NULL, args[given[i] - 1].span);
        /* connected (even when it failed): not reported as unconnected */
        elb_mark_tree_assigned(el, vi->ports[i].place);
    }
    free(conn);
    free(given);
    el_record_decl(el, at, name, SEM_INSTANCE, NULL, NULL, NULL);
    return v;
}

Value el_inst_field(Elab *el, Value inst, const char *name, Span at)
{
    ValInst *vi = inst.u.inst;
    int k = elb_find_port(vi->mi, name);
    if (k < 0) {
        el_error(el, at, "module `%s` has no port `%s`", vi->mi->display, name);
        elb_port_help(el, vi->mi, name);
        return val_error();
    }
    return vi->ports[k];
}

/* ------------------------------------------------------------- memories */

static IrExpr *elb_mem_addr(Elab *el, ValMem *mm, Value a, Span at)
{
    uint32_t need = ty_clog2(mm->depth);
    Ty *t = a.ty;
    if (need == 0)
        need = 1;
    if (a.kind == VAL_ERROR)
        return NULL;
    if (a.kind == VAL_INT && t && t->kind == TY_INT) {
        a = elab_convert(el, a, ty_uint(el->S, need), NULL, "the address", at);
        if (a.kind == VAL_ERROR)
            return NULL;
        return el_to_ir(el, &a, at);
    }
    if (!t || (!ty_is_sized_int(t) && t->kind != TY_ENUM) || t->kind == TY_SINT) {
        el_error(el, at, "memory address must be an unsigned integer, found %s",
                 t ? ty_str(t) : val_kind_name((ValKind)a.kind));
        return NULL;
    }
    if (t->width != need) {
        el_error(el, at, "memory address is %s, but `%u` entries need exactly uint<%u>", ty_str(t),
                 (unsigned)mm->depth, (unsigned)need);
        el_help(el, "convert with `as uint<%u>`", (unsigned)need);
        return NULL;
    }
    return el_to_ir(el, &a, at);
}

Value el_mem_index(Elab *el, Value mem, Value idx, Span at)
{
    ValMem *mm = mem.u.mem;
    IrExpr *addr = elb_mem_addr(el, mm, idx, at);
    Value v;
    Place *p;
    if (!addr)
        return val_error();
    v = el_hw(el, mm->elem, ir_memread(el->design->arena, el->mod->ir, mm->sig, addr, at));
    p = ANEW(&el->arena, Place);
    p->kind = PL_SIG;
    p->sig = mm->sig;
    p->index = addr;
    p->elem_width = mm->elem->width;
    p->width = mm->elem->width;
    p->ty = mm->elem;
    v.place = p;
    return v;
}

Value el_mem_method(Elab *el, Value mem, const char *name, Expr *call, Env *env)
{
    ValMem *mm = mem.u.mem;
    IrModule *m = el->mod ? el->mod->ir : NULL;
    Arg *args = call->u.call.args;
    uint32_t n = call->u.call.nargs, i;
    Span at = call->span;
    if (!m)
        return val_error();
    if (strcmp(name, "read") == 0) {
        Value a = val_error();
        int newdata = 0;
        uint32_t dom, reg;
        IrExpr *addr;
        IrStmtList body = {0};
        const char *rn;
        for (i = 0; i < n; i++) {
            if (args[i].name.name && strcmp(args[i].name.name, "rdw") == 0) {
                Expr *x = args[i].value;
                const char *vn = x->kind == EXPR_VARIANT ? x->u.variant.name.name
                               : x->kind == EXPR_PATH ? x->u.path.segs[x->u.path.nsegs - 1].name.name : NULL;
                if (vn && strcmp(vn, "NewData") == 0)
                    newdata = 1;
                else if (!vn || (strcmp(vn, "OldData") != 0 && strcmp(vn, "Undefined") != 0))
                    el_error(el, x->span, "`rdw` is `.OldData`, `.NewData` or `.Undefined`");
            } else if (args[i].name.name && strcmp(args[i].name.name, "addr") != 0) {
                el_error(el, args[i].span, "`read` has no argument `%s`", args[i].name.name);
            } else {
                a = elab_eval(el, args[i].value, env, NULL);
            }
        }
        addr = elb_mem_addr(el, mm, a, at);
        if (!addr)
            return val_error();
        if (el->blk && el->blk->clocked)
            dom = el->blk->domain;
        else
            dom = elb_default_domain(el, at, "a synchronous memory read");
        if (dom == IR_NONE)
            return val_error();
        rn = elb_name(el, env, arena_printf(&el->arena, "%s_rd%u", m->sigs[mm->sig].name, (unsigned)mm->nreads++));
        reg = ir_add_signal(el->design, m, rn, NULL, IRS_REG, m->sigs[mm->sig].type, at);
        m->sigs[reg].domain = dom;
        ir_stmts_push(&body, ir_stmt_assign(el->design->arena, ir_lv_sig(m, reg),
                                            ir_memread(el->design->arena, m, mm->sig, addr, at), at));
        {
            uint32_t pr = ir_add_process(el->design, m, IRP_CLOCKED, dom, ir_stmts_finish(el->design->arena, &body), at);
            if (newdata) {
                MemReadNew rn2;
                rn2.mem = mm->sig;
                rn2.reg = reg;
                rn2.addr = addr;
                rn2.proc = pr;
                vec_push(el->mod->memreads_new, rn2);
            }
        }
        map_put(&el->mod->sig_driven, reg, 1);
        return el_hw(el, mm->elem, ir_sig(el->design->arena, m, reg, at));
    }
    if (strcmp(name, "write") == 0) {
        Value a, d, k = val_error();
        IrExpr *addr, *mask = NULL;
        if (n < 2 || n > 3) {
            el_error(el, at, "`write(addr, data[, mask])` takes 2 or 3 arguments");
            return val_error();
        }
        if (!el->blk || !el->blk->clocked) {
            el_error(el, at, "memory `%s` can only be written inside an `on` block", m->sigs[mm->sig].name);
            return val_error();
        }
        a = elab_eval(el, args[0].value, env, NULL);
        d = elab_eval(el, args[1].value, env, mm->elem);
        d = elab_convert(el, d, mm->elem, args[1].value, "the memory data", args[1].value->span);
        addr = elb_mem_addr(el, mm, a, args[0].value->span);
        if (n == 3) {
            uint32_t mw = (mm->elem->width + 7) / 8;
            k = elab_eval(el, args[2].value, env, ty_bits(el->S, mw));
            k = elab_convert(el, k, ty_bits(el->S, mw), args[2].value, "the byte mask", args[2].value->span);
            if (k.kind == VAL_ERROR)
                return val_error();
            mask = el_to_ir(el, &k, at);
        }
        if (!addr || d.kind == VAL_ERROR)
            return val_error();
        ir_stmts_push(el->blk->out, ir_stmt_memwrite(el->design->arena, m, mm->sig, addr, el_to_ir(el, &d, at), mask, at));
        return val_void(el->S);
    }
    el_error(el, call->u.call.callee->span, "a memory has the methods `read` and `write`, not `%s`", name);
    return val_error();
}

/* -------------------------------------------------------------- temps -- */

static uint32_t elb_tree_size(const IrExpr *e, uint32_t cap)
{
    uint32_t n = 1, i;
    for (i = 0; i < e->nargs && n <= cap; i++)
        n += elb_tree_size(e->args[i], cap - n);
    return n;
}

/* Gives a large hardware value an intermediate signal so that repeated uses
 * of locals (fn bodies, loops) do not grow expression trees exponentially:
 * in a comb block a var assigned at this point (default 0 at the top of the
 * process), elsewhere a wire. */
Value el_maybe_temp(Elab *el, Value v, Span at)
{
    ModCtx *mc = el->mod;
    IrModule *m;
    uint32_t sig;
    BlkCtx *b = el->temp_blk;
    const char *nm;
    Value r;
    if (v.kind != VAL_HW || !el->design || !mc || !v.ty || !v.ty->hw || v.place || mc->in_property)
        return v;
    if (elb_tree_size(v.u.e, 40) <= 16)
        return v;
    m = mc->ir;
    nm = intern_cstr(arena_printf(&el->arena, "_t%u", (unsigned)mc->ntemps++));
    if (b && !b->clocked && b->out) {
        VarDefault vd;
        sig = ir_add_signal(el->design, m, nm, NULL, IRS_VAR, el_irty(el, v.ty), at);
        ir_stmts_push(b->out, ir_stmt_assign(el->design->arena, ir_lv_sig(m, sig), v.u.e, at));
        vec_push(b->assigned, sig);
        vd.sig = sig;
        vd.val = el_zero(el, v.ty);
        vd.loc = at;
        vec_push(mc->var_defaults, vd);
    } else {
        sig = ir_add_signal(el->design, m, nm, NULL, IRS_WIRE, el_irty(el, v.ty), at);
        ir_add_assign(el->design, m, ir_lv_sig(m, sig), v.u.e, at);
        map_put(&mc->let_wire, sig, 1);
    }
    map_put(&mc->sig_driven, sig, 1);
    r = el_hw(el, v.ty, ir_sig(el->design->arena, m, sig, at));
    return r;
}

/* ------------------------------------------------------- unsafe_cross -- */

Value el_unsafe_cross(Elab *el, Value v, Span at)
{
    IrModule *m;
    uint32_t w;
    Value r;
    if (v.kind == VAL_ERROR || !el->mod)
        return v;
    if (v.kind != VAL_HW) {
        if (val_is_const(&v))
            return v;
        el_error(el, at, "`unsafe_cross` takes a signal");
        return val_error();
    }
    m = el->mod->ir;
    w = ir_add_signal(el->design, m, intern_cstr(arena_printf(&el->arena, "unsafe_cross_%u", (unsigned)el->mod->nunsafe++)),
                      NULL, IRS_WIRE, el_irty(el, v.ty), at);
    ir_signal_add_attr(el->design, m, w, "unsafe_cross", NULL);
    ir_add_assign(el->design, m, ir_lv_sig(m, w), v.u.e, at);
    map_put(&el->mod->let_wire, w, 1);
    el_warning(el, at, "unchecked clock-domain crossing (`unsafe_cross`)");
    r = el_hw(el, v.ty, ir_sig(el->design->arena, m, w, at));
    return r;
}

/* ----------------------------------------------------------- properties */

Value el_temporal_call(Elab *el, const char *name, Expr *call, Env *env)
{
    Arg *args = call->u.call.args;
    uint32_t n = call->u.call.nargs, cycles = 1;
    Value x, p;
    Span at = call->span;
    if (!el->mod || !el->mod->in_property) {
        el_error(el, at, "`%s` is only valid inside a property", name);
        return val_error();
    }
    if (n < 1 || n > (strcmp(name, "past") == 0 ? 2u : 1u)) {
        el_error(el, at, "wrong number of arguments to `%s`", name);
        return val_error();
    }
    x = elab_eval(el, args[0].value, env, NULL);
    if (x.kind == VAL_ERROR)
        return x;
    if (n == 2) {
        Value c = elab_eval_const(el, args[1].value, env, el->S->t_int, "a cycle count");
        uint64_t k;
        if (!val_get_u64(&c, &k) || k == 0 || k > 4096) {
            el_error(el, args[1].value->span, "`past` needs a cycle count 1 ..= 4096");
            return val_error();
        }
        cycles = (uint32_t)k;
    }
    if (!x.ty || !x.ty->hw) {
        el_error(el, at, "`%s` needs a signal", name);
        return val_error();
    }
    if (x.kind != VAL_HW)
        return strcmp(name, "past") == 0 ? x : strcmp(name, "stable") == 0 ? el_mk_bool(el, 1) : el_mk_bool(el, 0);
    p = el_hw(el, x.ty, ir_past(el->design->arena, x.u.e, cycles, at));
    if (strcmp(name, "past") == 0)
        return p;
    if (strcmp(name, "stable") == 0)
        return el_binary(el, BIN_EQ, x, p, at, NULL, NULL);
    if (x.ty->kind != TY_BOOL && x.ty->width != 1) {
        el_error(el, at, "`%s` needs a bool", name);
        return val_error();
    }
    if (x.ty->kind != TY_BOOL) {
        x = el_cast(el, x, el->S->t_bool, at, NULL);
        p = el_cast(el, p, el->S->t_bool, at, NULL);
    }
    if (strcmp(name, "rose") == 0)
        return el_and(el, x, el_not(el, p, at), at);
    return el_and(el, el_not(el, x, at), p, at);
}

static int elb_has_temporal(Expr *e)
{
    if (!e)
        return 0;
    switch (e->kind) {
    case EXPR_TEMPORAL: case EXPR_DELAY: return 1;
    case EXPR_BINARY:
        if (e->u.binary.op == BIN_IMPLIES || e->u.binary.op == BIN_IMPLIES_NEXT || e->u.binary.op == BIN_UNTIL)
            return 1;
        return elb_has_temporal(e->u.binary.lhs) || elb_has_temporal(e->u.binary.rhs);
    case EXPR_UNARY: return elb_has_temporal(e->u.unary.operand);
    case EXPR_PAREN: return elb_has_temporal(e->u.inner);
    default: return 0;
    }
}

static uint32_t elb_cycles(Elab *el, Expr *e, Env *env)
{
    Value v = elab_eval_const(el, e, env, el->S->t_int, "a cycle count");
    uint64_t k = 0;
    if (v.kind != VAL_ERROR && (!val_get_u64(&v, &k) || k > 1000000)) {
        el_error(el, e->span, "invalid cycle count");
        k = 0;
    }
    return (uint32_t)k;
}

static IrSeq *elb_seq(Elab *el, Expr *e, Env *env, int depth)
{
    Arena *a = el->design->arena;
    if (depth > 64) {
        el_error(el, e->span, "sequence nested too deeply (recursive sequence?)");
        return NULL;
    }
    switch (e->kind) {
    case EXPR_PAREN:
        return elb_seq(el, e->u.inner, env, depth);
    case EXPR_TEMPORAL: {
        IrSeq *x = elb_seq(el, e->u.temporal.body, env, depth + 1);
        if (!x)
            return NULL;
        if (e->u.temporal.op == TEMP_ALWAYS)
            return ir_seq_unary(a, IRQ_ALWAYS, x);
        if (e->u.temporal.op == TEMP_NEVER)
            return ir_seq_unary(a, IRQ_NEVER, x);
        return ir_seq_eventually(a, x, e->u.temporal.bound ? elb_cycles(el, e->u.temporal.bound, env) : IR_NONE);
    }
    case EXPR_DELAY: {
        IrSeq *l = e->u.delay.lhs ? elb_seq(el, e->u.delay.lhs, env, depth + 1)
                                  : ir_seq_expr(a, ir_const_u64(a, 1, 1, e->span));
        IrSeq *r = elb_seq(el, e->u.delay.rhs, env, depth + 1);
        uint32_t lo = elb_cycles(el, e->u.delay.lo, env);
        uint32_t hi = e->u.delay.unbounded ? IR_NONE : e->u.delay.hi ? elb_cycles(el, e->u.delay.hi, env) : lo;
        if (!l || !r)
            return NULL;
        return ir_seq_delay(a, l, lo, hi, r);
    }
    case EXPR_BINARY:
        if (e->u.binary.op == BIN_IMPLIES || e->u.binary.op == BIN_IMPLIES_NEXT || e->u.binary.op == BIN_UNTIL ||
            ((e->u.binary.op == BIN_AND || e->u.binary.op == BIN_OR) && elb_has_temporal(e))) {
            IrSeq *l = elb_seq(el, e->u.binary.lhs, env, depth + 1);
            IrSeq *r = elb_seq(el, e->u.binary.rhs, env, depth + 1);
            IrSeqKind k = e->u.binary.op == BIN_IMPLIES ? IRQ_IMPL : e->u.binary.op == BIN_IMPLIES_NEXT ? IRQ_IMPL_NEXT
                        : e->u.binary.op == BIN_UNTIL ? IRQ_UNTIL : e->u.binary.op == BIN_AND ? IRQ_AND : IRQ_OR;
            if (!l || !r)
                return NULL;
            return ir_seq_binary(a, k, l, r);
        }
        break;
    case EXPR_UNARY:
        if (e->u.unary.op == UN_NOT && elb_has_temporal(e->u.unary.operand)) {
            IrSeq *x = elb_seq(el, e->u.unary.operand, env, depth + 1);
            return x ? ir_seq_unary(a, IRQ_NOT, x) : NULL;
        }
        break;
    case EXPR_PATH:
        if (e->u.path.nsegs == 1) {
            Binding *b = elab_env_find(env, e->u.path.segs[0].name.name);
            if (b && b->item && b->item->kind == ITEM_SEQUENCE)
                return elb_seq(el, b->item->u.sequence.body, env, depth + 1);
            if (!b) {
                Symbol sym;
                if (sema_lookup(el->S, env->file, e->u.path.segs[0].name.name, &sym) && sym.kind == SYM_ITEM &&
                    sym.item->kind == ITEM_SEQUENCE)
                    return elb_seq(el, sym.item->u.sequence.body, env, depth + 1);
            }
        }
        break;
    default:
        break;
    }
    {
        Value v = el_prop_reset(el, elab_eval(el, e, env, el->S->t_bool), e->span);
        if (v.kind == VAL_ERROR)
            return NULL;
        if (!v.ty || v.ty->kind != TY_BOOL) {
            el_error(el, e->span, "a property needs bool expressions, found %s",
                     v.ty ? ty_str(v.ty) : val_kind_name((ValKind)v.kind));
            return NULL;
        }
        return ir_seq_expr(a, el_to_ir(el, &v, e->span));
    }
}

/* domains of the signals a property reads: *dom gets the only one, or
 * IR_NONE + 1 once two different domains were seen */
static void elb_expr_domain(const IrModule *m, const IrExpr *e, uint32_t *dom)
{
    uint32_t i;
    if (!e)
        return;
    if ((e->kind == IRE_SIG || e->kind == IRE_MEMREAD) && e->sig < m->nsigs) {
        uint32_t d = m->sigs[e->sig].domain;
        if (d != IR_NONE && m->sigs[e->sig].type->kind != IRT_CLOCK && m->sigs[e->sig].type->kind != IRT_RESET) {
            if (*dom == IR_NONE)
                *dom = d;
            else if (*dom != d)
                *dom = IR_NONE - 1;
        }
    }
    for (i = 0; i < e->nargs; i++)
        elb_expr_domain(m, e->args[i], dom);
}

static void elb_seq_domain(const IrModule *m, const IrSeq *q, uint32_t *dom)
{
    if (!q)
        return;
    elb_expr_domain(m, q->expr, dom);
    elb_seq_domain(m, q->a, dom);
    elb_seq_domain(m, q->b, dom);
}

static void elb_property(Elab *el, Item *it, Env *env)
{
    IrModule *m = el->mod->ir;
    IrSeq *seq;
    IrAssertKind k = it->u.property.kind == PROP_ASSUME ? IRA_ASSUME
                   : it->u.property.kind == PROP_COVER ? IRA_COVER : IRA_ASSERT;
    uint32_t dom = m->default_domain != IR_NONE ? m->default_domain : m->ndomains ? 0 : IR_NONE;
    el->mod->in_property = 1;
    seq = elb_seq(el, it->u.property.body, env, 0);
    el->mod->in_property = 0;
    if (seq)
        ir_add_property(el->design, m, elb_name(el, env, it->name.name ? it->name.name : "property"), k, dom, seq,
                        it->span);
}

/* --------------------------------------------------------------- blocks -- */

static void elb_block(Elab *el, Block *body, Env *env, int clocked, uint32_t dom, Span at)
{
    ModCtx *mc = el->mod;
    IrStmtList list = {0};
    BlkCtx blk;
    Env *benv = elab_env_new(el, env, NULL);
    uint32_t pr;
    size_t i;
    memset(&blk, 0, sizeof blk);
    blk.clocked = clocked;
    blk.domain = dom;
    blk.out = &list;
    blk.loc = at;
    el->blk = &blk;
    el->temp_blk = &blk;
    el_exec_stmts(el, body->stmts, body->nstmts, benv, NULL, NULL);
    el->blk = NULL;
    el->temp_blk = NULL;
    elab_env_free(benv);
    pr = ir_add_process(el->design, mc->ir, clocked ? IRP_CLOCKED : IRP_COMB, clocked ? dom : IR_NONE,
                        ir_stmts_finish(el->design->arena, &list), at);
    if (!clocked)
        for (i = 0; i < blk.assigned.len; i++)
            if (!map_has(&mc->sig_comb, blk.assigned.data[i]))
                map_put(&mc->sig_comb, blk.assigned.data[i], pr + 1);
    vec_free(blk.assigned);
}

/* ------------------------------------------------------- module bodies -- */

static void elb_bind_decls(Elab *el, Stmt **stmts, uint32_t n, Env *env)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        Stmt *s = stmts[i];
        const char *nm = NULL;
        Span def = span_none();
        Binding *b;
        switch (s->kind) {
        case STMT_DOMAIN: nm = s->u.domain.name.name; def = s->u.domain.name.span; break;
        case STMT_VAR: nm = s->u.var.name.name; def = s->u.var.name.span; break;
        case STMT_REG: nm = s->u.reg.name.name; def = s->u.reg.name.span; break;
        case STMT_MEM: nm = s->u.mem.name.name; def = s->u.mem.name.span; break;
        case STMT_LET:
            if (s->u.let.pat->kind == PAT_BIND) {
                nm = s->u.let.pat->u.bind.name;
                def = s->u.let.pat->u.bind.span;
            } else {
                /* destructuring: every bound name forces the declaration */
                Env *tmp = elab_env_new(el, NULL, env->file);
                uint32_t k;
                Pattern *p = s->u.let.pat;
                if (p->kind == PAT_STRUCT) {
                    for (k = 0; k < p->u.struct_pat.nfields; k++) {
                        FieldPat *fp = &p->u.struct_pat.fields[k];
                        const char *bn = fp->pat && fp->pat->kind == PAT_BIND ? fp->pat->u.bind.name : fp->name.name;
                        b = elab_env_bind(el, env, bn, val_void(el->S), fp->name.span);
                        b->lazy = 1;
                        b->decl = s;
                        b->decl_env = env;
                        b->sem = SEM_WIRE;
                    }
                } else if (p->kind == PAT_TUPLE) {
                    for (k = 0; k < p->u.list.n; k++)
                        if (p->u.list.items[k]->kind == PAT_BIND) {
                            b = elab_env_bind(el, env, p->u.list.items[k]->u.bind.name, val_void(el->S),
                                              p->u.list.items[k]->u.bind.span);
                            b->lazy = 1;
                            b->decl = s;
                            b->decl_env = env;
                        }
                }
                elab_env_free(tmp);
            }
            break;
        case STMT_ITEM:
            if (s->u.item->name.name && s->u.item->kind != ITEM_PROPERTY) {
                b = elab_env_bind(el, env, s->u.item->name.name, val_void(el->S), s->u.item->name.span);
                b->lazy = 1;
                b->item = s->u.item;
                b->decl_env = env;
                b->sem = s->u.item->kind == ITEM_CONST ? SEM_CONST : s->u.item->kind == ITEM_FN ? SEM_FN : SEM_TYPE;
            }
            break;
        default:
            break;
        }
        if (nm && !elb_redeclared(el, env, nm, def)) {
            b = elab_env_bind(el, env, nm, val_void(el->S), def);
            b->lazy = 1;
            b->decl = s;
            b->decl_env = env;
        }
    }
}

static void elb_run_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env);

static void elb_force_name(Elab *el, Env *env, const char *nm)
{
    Binding *b = nm ? ce_lookup_local_binding(env, nm) : NULL;
    if (b && b->lazy && b->state == 0)
        el_force(el, b);
}

void el_mod_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env)
{
    elb_bind_decls(el, stmts, n, env);
    elb_run_stmts(el, stmts, n, env);
}

static void elb_run_stmts(Elab *el, Stmt **stmts, uint32_t n, Env *env)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        Stmt *s = stmts[i];
        if (el->limit_hit)
            return;
        switch (s->kind) {
        case STMT_ERROR:
        case STMT_PORT:
            break;
        case STMT_DOMAIN: elb_force_name(el, env, s->u.domain.name.name); break;
        case STMT_VAR: elb_force_name(el, env, s->u.var.name.name); break;
        case STMT_REG: elb_force_name(el, env, s->u.reg.name.name); break;
        case STMT_MEM: elb_force_name(el, env, s->u.mem.name.name); break;
        case STMT_LET:
            if (s->u.let.pat->kind == PAT_BIND)
                elb_force_name(el, env, s->u.let.pat->u.bind.name);
            else if (s->u.let.pat->kind == PAT_STRUCT && s->u.let.pat->u.struct_pat.nfields) {
                FieldPat *fp = &s->u.let.pat->u.struct_pat.fields[0];
                elb_force_name(el, env, fp->pat && fp->pat->kind == PAT_BIND ? fp->pat->u.bind.name : fp->name.name);
            } else if (s->u.let.pat->kind == PAT_TUPLE) {
                uint32_t k;
                for (k = 0; k < s->u.let.pat->u.list.n; k++)
                    if (s->u.let.pat->u.list.items[k]->kind == PAT_BIND) {
                        elb_force_name(el, env, s->u.let.pat->u.list.items[k]->u.bind.name);
                        break;
                    }
            }
            break;
        case STMT_ITEM:
            if (s->u.item->kind == ITEM_PROPERTY)
                elb_property(el, s->u.item, env);
            else if (s->u.item->kind == ITEM_CONST)
                elb_force_name(el, env, s->u.item->name.name);
            break;
        case STMT_ASSIGN: {
            Place *p;
            Value v;
            if (s->u.assign.constraint)
                el_error(el, s->u.assign.constraint->span, "`where` on an assignment is only allowed in tests");
            p = el_place_of(el, s->u.assign.lhs, env);
            if (!p)
                break;
            v = elab_eval(el, s->u.assign.rhs, env, p->kind == PL_LOCAL ? NULL : p->ty);
            el_assign(el, p, v, s->u.assign.rhs, s->u.assign.lhs, s->span);
            break;
        }
        case STMT_COMB:
            elb_block(el, s->u.comb, env, 0, IR_NONE, s->span);
            break;
        case STMT_ON: {
            uint32_t d = el_domain_of(el, s->u.on.clock, env, s->u.on.clock->span);
            if (d != IR_NONE)
                elb_block(el, s->u.on.body, env, 1, d, s->span);
            break;
        }
        case STMT_EXPR: {
            Expr *e = s->u.expr;
            if (e->kind == EXPR_IF) {
                /* elaboration-time if */
                for (;;) {
                    Value c = elab_eval_const(el, e->u.if_expr.cond, env, el->S->t_bool, "a module-level `if` condition");
                    if (c.kind == VAL_ERROR)
                        break;
                    if (c.kind != VAL_BOOL) {
                        el_error(el, e->u.if_expr.cond->span, "`if` condition must be bool");
                        break;
                    }
                    if (c.u.i) {
                        Env *ie = elab_env_new(el, env, NULL);
                        el_env_keep(el, ie);
                        el_mod_stmts(el, e->u.if_expr.then_block->stmts, e->u.if_expr.then_block->nstmts, ie);
                        break;
                    }
                    if (!e->u.if_expr.else_expr)
                        break;
                    if (e->u.if_expr.else_expr->kind == EXPR_IF) {
                        e = e->u.if_expr.else_expr;
                        continue;
                    }
                    if (e->u.if_expr.else_expr->kind == EXPR_BLOCK) {
                        Env *ie = elab_env_new(el, env, NULL);
                        el_env_keep(el, ie);
                        el_mod_stmts(el, e->u.if_expr.else_expr->u.block->stmts, e->u.if_expr.else_expr->u.block->nstmts,
                                     ie);
                    }
                    break;
                }
            } else if (e->kind == EXPR_CALL) {
                elab_eval(el, e, env, NULL);
            } else if (e->kind == EXPR_MATCH) {
                el_error(el, e->span, "a module-level `match` must be inside a `comb` or `on` block");
            } else if (e->kind != EXPR_ERROR) {
                el_error(el, e->span, "this expression has no effect at module level");
            }
            break;
        }
        case STMT_FOR: {
            Value it = elab_eval_const(el, s->u.for_stmt.iter, env, NULL, "a module-level `for` range");
            ValList vals;
            uint32_t k;
            if (it.kind == VAL_ERROR)
                break;
            if (it.kind == VAL_RANGE) {
                int64_t lo = 0, hi = 0;
                val_get_i64(&it.u.list->v[0], &lo);
                val_get_i64(&it.u.list->v[1], &hi);
                if (it.aux)
                    hi++;
                if (hi < lo)
                    hi = lo;
                vals.n = (uint32_t)(hi - lo);
                vals.v = ANEWN(&el->arena, Value, vals.n ? vals.n : 1);
                for (k = 0; k < vals.n; k++)
                    vals.v[k] = val_int(el->S->t_int, lo + (int64_t)k);
            } else if (it.kind == VAL_ARRAY || it.kind == VAL_TUPLE) {
                vals = *it.u.list;
            } else {
                el_error(el, s->u.for_stmt.iter->span, "`for` needs a compile-time range or array");
                break;
            }
            for (k = 0; k < vals.n && !el->limit_hit; k++) {
                Env *ie = elab_env_new(el, env, NULL);
                el_env_keep(el, ie);
                ie->suffix = intern_cstr(arena_printf(&el->arena, "%s_%u", env->suffix ? env->suffix : "", (unsigned)k));
                el_bind_pattern(el, s->u.for_stmt.pat, vals.v[k], ie, 0);
                {
                    uint32_t q;
                    for (q = 0; q < ie->n; q++)
                        ie->b[q].sem = SEM_CONST;
                }
                el_mod_stmts(el, s->u.for_stmt.body->stmts, s->u.for_stmt.body->nstmts, ie);
            }
            break;
        }
        case STMT_STATIC_ASSERT:
        case STMT_ASSERT:
            if (s->kind == STMT_ASSERT) {
                el_error(el, s->span, "immediate assertions belong in a `comb` or `on` block");
                break;
            }
            el_exec_stmt(el, s, env);
            break;
        case STMT_WHILE:
            el_error(el, s->span, "`while` is only allowed in const fns");
            break;
        case STMT_RETURN:
            el_error(el, s->span, "`return` outside a function");
            break;
        }
    }
}

/* ------------------------------------------------------ module driver -- */

static IrCdcKind elb_cdc_kind(const char *n, int *ok)
{
    static const char *const names[] = {"sync2", "sync3", "pulse", "handshake", "async_fifo",
                                        "reset_sync", "reset_bridge", "gray", "unsafe"};
    uint32_t i;
    *ok = 1;
    for (i = 0; i < TERN_ARRAY_LEN(names); i++)
        if (strcmp(n, names[i]) == 0)
            return (IrCdcKind)i;
    *ok = 0;
    return IRC_UNSAFE;
}

static void elb_module_attrs(Elab *el, ModCtx *mc)
{
    Item *it = mc->item;
    uint32_t i;
    for (i = 0; i < it->attrs.n; i++) {
        Attr *a = &it->attrs.items[i];
        const char *n = a->name.name;
        if (!n)
            continue;
        if (strcmp(n, "top") == 0)
            continue;
        if (strcmp(n, "cdc") == 0) {
            int ok = 0;
            mc->attr_cdc = 1;
            mc->ir->is_cdc = 1;
            if (a->nargs == 1 && a->args[0].value->kind == EXPR_PATH)
                mc->ir->cdc_kind = elb_cdc_kind(a->args[0].value->u.path.segs[0].name.name, &ok);
            if (!ok)
                el_error(el, a->span, "unknown #[cdc(..)] kind (sync2, sync3, pulse, handshake, async_fifo, "
                                      "reset_sync, reset_bridge, gray)");
            continue;
        }
        if (strcmp(n, "clock_gen") == 0 || strcmp(n, "clock_gate") == 0)
            mc->attr_clock_gen = 1;
        ir_module_add_attr(el->design, mc->ir, n, elb_attr_args(el, a));
    }
}

/* IrModule.origin: the declaration and its generic arguments (for back ends
 * that re-elaborate it, e.g. parameterized SystemVerilog) */
static void elb_origin(Elab *el, ModCtx *mc)
{
    uint32_t i;
    ValModule *vm = mc->vm;
    Arena *a = el->design->arena;
    IrOrigin *o = ANEW(a, IrOrigin);
    GenericParams *gp = vm->item->kind == ITEM_MODULE ? &vm->item->u.module.generics
                                                      : &vm->item->u.extern_mod.generics;
    o->item = vm->item;
    o->file = vm->file;
    o->ngenerics = vm->args.n;
    o->generics = ANEWN(a, IrGeneric, vm->args.n ? vm->args.n : 1);
    for (i = 0; i < vm->args.n; i++) {
        IrGeneric *g = &o->generics[i];
        const Value *v = &vm->args.items[i].val;
        g->name = vm->args.items[i].name;
        g->kind = IRG_OTHER;
        if (i < gp->n && gp->items[i].kind == GENERIC_TYPE)
            continue;
        if (v->kind == VAL_INT) {
            int64_t x = 0;
            g->fits = val_get_i64(v, &x);
            g->value = x;
            g->kind = (v->ty && v->ty->kind == TY_SINT) || (g->fits && x < 0) ? IRG_SINT : IRG_UINT;
        } else if (v->kind == VAL_BOOL) {
            g->kind = IRG_BOOL;
            g->fits = 1;
            g->value = v->u.i ? 1 : 0;
        }
    }
    mc->ir->origin = o;
}

static void elb_params(Elab *el, ModCtx *mc)
{
    uint32_t i;
    ValModule *vm = mc->vm;
    elb_origin(el, mc);
    for (i = 0; i < vm->args.n; i++) {
        Value *v = &vm->args.items[i].val;
        if (v->kind == VAL_INT) {
            BigInt b = {0};
            uint32_t w;
            int sg;
            IrBits bits;
            val_get_bn(v, &b);
            sg = bn_is_neg(&b) || (v->ty && v->ty->kind == TY_SINT);
            w = v->ty && v->ty->kind != TY_INT ? v->ty->width
                : (uint32_t)(sg ? bn_signed_width(&b) : bn_bitlen(&b));
            if (w < 32 && v->ty && v->ty->kind == TY_INT)
                w = 32;
            if (w == 0)
                w = 1;
            bits.width = w;
            bits.w = ANEWN(el->design->arena, uint64_t, IR_LIMBS(w));
            bn_to_limbs(&b, bits.w, w);
            bn_free(&b);
            ir_add_param(el->design, mc->ir, vm->args.items[i].name, bits, sg);
        } else if (v->kind == VAL_BOOL) {
            ir_add_param(el->design, mc->ir, vm->args.items[i].name, ir_bits_u64(el->design->arena, 1, (uint64_t)v->u.i), 0);
        }
    }
}

/* forwarding for `.read(addr, rdw: .NewData)`: path condition of every
 * write to the memory in the clocked processes of the same domain */
typedef struct ElbFwd {
    Elab *el;
    uint32_t mem;
    IrExpr *addr;
    IrExpr *result;
    uint32_t width;
} ElbFwd;

static void elb_fwd_block(ElbFwd *f, const IrBlock *b, IrExpr *cond);

static IrExpr *elb_and(Elab *el, IrExpr *a, IrExpr *b)
{
    if (!a)
        return b;
    return ir_binary(el->design->arena, IROP_LAND, a, b, 0, b->loc);
}

static void elb_fwd_stmt(ElbFwd *f, const IrStmt *s, IrExpr *cond)
{
    Arena *a = f->el->design->arena;
    uint32_t i, k;
    switch (s->kind) {
    case IRST_IF:
        elb_fwd_block(f, &s->then_b, elb_and(f->el, cond, s->cond));
        elb_fwd_block(f, &s->else_b, elb_and(f->el, cond, ir_unary(a, IROP_LNOT, s->cond, s->loc)));
        break;
    case IRST_CASE:
        for (i = 0; i < s->narms; i++) {
            IrExpr *c = NULL;
            for (k = 0; k < s->arms[i].nvals; k++) {
                IrExpr *eq = ir_binary(a, IROP_EQ, s->subject, ir_const(a, s->arms[i].vals[k], s->loc), 0, s->loc);
                c = c ? ir_binary(a, IROP_LOR, c, eq, 0, s->loc) : eq;
            }
            elb_fwd_block(f, &s->arms[i].body, c ? elb_and(f->el, cond, c) : cond);
        }
        break;
    case IRST_MEMWRITE:
        if (s->mem == f->mem && !s->mask) {
            IrExpr *hit = ir_binary(a, IROP_EQ, s->addr, f->addr, 0, s->loc);
            IrExpr *c = elb_and(f->el, cond, hit);
            f->result = ir_mux(a, c, s->data, f->result, s->loc);
        }
        break;
    default:
        break;
    }
}

static void elb_fwd_block(ElbFwd *f, const IrBlock *b, IrExpr *cond)
{
    uint32_t i;
    for (i = 0; i < b->n; i++)
        elb_fwd_stmt(f, b->stmts[i], cond);
}

/* read ports of the memories (distinct IRE_MEMREAD nodes, §10.1) */
typedef struct ElbPorts {
    Map seen;
    uint32_t *count;          /* per signal */
} ElbPorts;

static void elb_ports_expr(ElbPorts *pc, const IrExpr *e)
{
    uint32_t i;
    if (!e || map_has(&pc->seen, MAP_PTR(e)))
        return;
    map_put(&pc->seen, MAP_PTR(e), 1);
    if (e->kind == IRE_MEMREAD)
        pc->count[e->sig]++;
    for (i = 0; i < e->nargs; i++)
        elb_ports_expr(pc, e->args[i]);
}

static void elb_ports_block(ElbPorts *pc, const IrBlock *b)
{
    uint32_t i, k;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        elb_ports_expr(pc, s->rhs);
        elb_ports_expr(pc, s->cond);
        elb_ports_expr(pc, s->subject);
        elb_ports_expr(pc, s->addr);
        elb_ports_expr(pc, s->data);
        elb_ports_expr(pc, s->lhs.index);
        elb_ports_block(pc, &s->then_b);
        elb_ports_block(pc, &s->else_b);
        for (k = 0; k < s->narms; k++)
            elb_ports_block(pc, &s->arms[k].body);
    }
}

static void elb_check_mem_ports(Elab *el, ModCtx *mc)
{
    IrModule *m = mc->ir;
    ElbPorts pc;
    uint32_t i, k;
    int any = 0;
    for (i = 0; i < m->nsigs && !any; i++)
        any = m->sigs[i].kind == IRS_MEM;
    if (!any)
        return;
    memset(&pc, 0, sizeof pc);
    pc.count = (uint32_t *)xcalloc(m->nsigs, sizeof(uint32_t));
    for (i = 0; i < m->nassigns; i++) {
        elb_ports_expr(&pc, m->assigns[i].rhs);
        elb_ports_expr(&pc, m->assigns[i].lhs.index);
    }
    for (i = 0; i < m->nprocs; i++)
        elb_ports_block(&pc, &m->procs[i].body);
    for (i = 0; i < m->nsigs; i++) {
        int distributed = 0;
        if (m->sigs[i].kind != IRS_MEM || pc.count[i] <= 2)
            continue;
        for (k = 0; k < m->sigs[i].nattrs; k++)
            if (strcmp(m->sigs[i].attrs[k].name, "ram") == 0 && m->sigs[i].attrs[k].args &&
                (strstr(m->sigs[i].attrs[k].args, "distributed") || strstr(m->sigs[i].attrs[k].args, "registers")))
                distributed = 1;
        if (distributed)
            continue;
        el_error(el, m->sigs[i].loc, "mem `%s` has %u read ports; the target supports 2", m->sigs[i].name,
                 (unsigned)pc.count[i]);
        el_help(el, "share a port between the readers, duplicate the memory, or mark it #[ram(distributed)]");
    }
    free(pc.count);
    map_free(&pc.seen);
}

static void elb_finish(Elab *el, ModCtx *mc)
{
    IrModule *m = mc->ir;
    size_t i, j;
    uint32_t k;
    /* var defaults: first statement of the comb process that drives the var */
    for (i = 0; i < mc->var_defaults.len; i++) {
        VarDefault *vd = &mc->var_defaults.data[i];
        uint64_t pr;
        IrExpr *rhs = el_to_ir(el, &vd->val, vd->loc);
        if (map_get(&mc->sig_comb, vd->sig, &pr)) {
            IrProcess *p = &m->procs[pr - 1];
            IrStmt **ns = ANEWN(el->design->arena, IrStmt *, p->body.n + 1);
            ns[0] = ir_stmt_assign(el->design->arena, ir_lv_sig(m, vd->sig), rhs, vd->loc);
            if (p->body.n)
                memcpy(ns + 1, p->body.stmts, p->body.n * sizeof(IrStmt *));
            p->body.stmts = ns;
            p->body.n++;
        } else if (!map_has(&mc->sig_driven, vd->sig)) {
            ir_add_assign(el->design, m, ir_lv_sig(m, vd->sig), rhs, vd->loc);
        }
    }
    /* read-during-write forwarding */
    for (i = 0; i < mc->memreads_new.len; i++) {
        MemReadNew *r = &mc->memreads_new.data[i];
        IrProcess *rp = &m->procs[r->proc];
        ElbFwd f;
        f.el = el;
        f.mem = r->mem;
        f.addr = r->addr;
        f.result = rp->body.stmts[0]->rhs;
        f.width = f.result->width;
        for (j = 0; j < m->nprocs; j++)
            if (j != r->proc && m->procs[j].kind == IRP_CLOCKED && m->procs[j].domain == rp->domain)
                elb_fwd_block(&f, &m->procs[j].body, NULL);
        rp->body.stmts[0]->rhs = f.result;
    }
    elb_check_mem_ports(el, mc);
    /* unconnected instance inputs */
    for (i = 0; i < mc->pendings.len; i++) {
        PendingIn *pi = &mc->pendings.data[i];
        StrBuf sb = {0};
        int count = 0;
        const char *clock_port = NULL;
        ValInst *vi = pi->inst;
        if (pi->assigned || !vi)
            continue;
        for (j = i; j < mc->pendings.len; j++) {
            PendingIn *q = &mc->pendings.data[j];
            const char *nm;
            size_t len;
            if (q->inst != vi || q->assigned)
                continue;
            q->assigned = 1;
            nm = q->port;
            len = strcspn(nm, ".[");
            if (sb.len) {
                /* skip duplicates of the same top-level port */
                char key[160];
                snprintf(key, sizeof key, "`%.*s`", (int)len, nm);
                if (strstr(sb_cstr(&sb), key))
                    continue;
                sb_puts(&sb, ", ");
            }
            sb_printf(&sb, "`%.*s`", (int)len, nm);
            count++;
            for (k = 0; k < vi->mi->nports && !clock_port; k++)
                if ((vi->mi->ports[k].is_clock || vi->mi->ports[k].is_reset) &&
                    strlen(vi->mi->ports[k].name) == len && strncmp(vi->mi->ports[k].name, nm, len) == 0)
                    clock_port = vi->mi->ports[k].name;
        }
        el_error(el, vi->loc, "instance `%s` of `%s` has unconnected input%s %s", vi->name, vi->mi->display,
                 count == 1 ? "" : "s", sb_cstr(&sb));
        if (clock_port)
            el_help(el, "clock and reset ports are inherited only from a default domain (a module with one "
                        "clock); connect `%s: ...` or put the instance in a domain: `%s(..) @ dom`",
                    clock_port, vi->mi->display);
        else
            el_help(el, "connect %s in the instantiation (`port: value`) or assign it later (`%s.port = value`)",
                    count == 1 ? "it" : "them", vi->name);
        sb_free(&sb);
    }
}

static void elb_free_ctx(ModCtx *mc)
{
    vec_free(mc->clock_ports);
    vec_free(mc->reset_ports);
    map_free(&mc->sig_comb);
    map_free(&mc->sig_driven);
    vec_free(mc->var_defaults);
    map_free(&mc->pending);
    vec_free(mc->pendings);
    vec_free(mc->memwrites);
    vec_free(mc->memreads_new);
    map_free(&mc->inst_out);
    map_free(&mc->let_wire);
}

static int elb_name_taken(Elab *el, const char *nm)
{
    return map_has(&el->modtypes, MAP_PTR(nm) ^ 2u);
}

ModInfo *elab_module(Elab *el, ValModule *vm, Span at)
{
    uint64_t v;
    ModInfo *mi;
    ModCtx mc;
    ModCtx *saved_mod = el->mod;
    struct BlkCtx *saved_blk = el->blk;
    int saved_fn = el->in_fn;
    uint32_t saved_undo = el->undo_depth;
    Item *it = vm->item;
    Stmt **body;
    uint32_t nbody, i;
    Env *scope;
    int errs0 = diag_error_count();
    if (map_get(&el->modules, MAP_PTR(vm->cache_key), &v)) {
        mi = (ModInfo *)(uintptr_t)v;
        if (mi->state == 1) {
            el_error(el, at, "module `%s` instantiates itself", vm->display);
            return NULL;
        }
        return mi;
    }
    if (!el->design) {
        el_error(el, at, "modules can only be elaborated into a design");
        return NULL;
    }
    if (el->mod_depth >= EL_MAX_MOD_DEPTH) {
        /* M<N> instantiating M<N + 1> forever: every key is new, so the
         * self-instantiation check above never fires */
        el_error(el, at, "module instances nested too deeply (more than %u levels) at `%s`",
                 (unsigned)EL_MAX_MOD_DEPTH, vm->display);
        diag_help("a module that instantiates itself needs a base case: `if N > 1 { ... }`");
        return NULL;
    }
    el->mod_depth++;
    mi = ANEW(&el->arena, ModInfo);
    mi->item = it;
    mi->file = vm->file;
    mi->key = vm->key;
    mi->display = vm->display;
    mi->state = 1;
    map_put(&el->modules, MAP_PTR(vm->cache_key), MAP_PTR(mi));
    {
        /* emitted names are unique in the design */
        const char *nm = vm->key;
        int k = 2;
        while (ir_design_find_module(el->design, nm) || elb_name_taken(el, nm))
            nm = intern_cstr(arena_printf(&el->arena, "%s_%d", vm->key, k++));
        map_put(&el->modtypes, MAP_PTR(nm) ^ 2u, 1);
        mi->ir = ir_module_new(el->design, nm, it->name.name, vm->display, it->name.span);
    }
    mi->ir->package = vm->file && vm->file->pkg->path[0] ? vm->file->pkg->path : NULL;
    mi->ir->default_domain = IR_NONE;
    memset(&mc, 0, sizeof mc);
    mc.mi = mi;
    mc.vm = vm;
    mc.ir = mi->ir;
    mc.file = vm->file;
    mc.item = it;
    mc.is_top = elb_find_attr(&it->attrs, "top") != NULL;
    mc.is_extern = it->kind == ITEM_EXTERN;
    mi->ir->is_top = mc.is_top;
    mi->ir->is_extern = mc.is_extern;
    if (mc.is_extern)
        mi->ir->extern_file = it->u.extern_mod.file;
    scope = elab_env_new(el, vm->genv, vm->file);
    el_env_keep(el, scope);
    scope->mod = &mc;
    scope->suffix = "";
    mc.scope = scope;
    mi->scope = scope;
    el->mod = &mc;
    el->blk = NULL;
    el->in_fn = 0;
    el->undo_depth = 0;
    body = it->kind == ITEM_MODULE ? it->u.module.body : it->u.extern_mod.body;
    nbody = it->kind == ITEM_MODULE ? it->u.module.nbody : it->u.extern_mod.nbody;
    if (el_regfile_is(it, vm->file)) {
        /* compiler-generated register file (regfile.c) */
        Stmt **gen = NULL;
        uint32_t ngen = 0;
        if (el_regfile_body(el, vm, &gen, &ngen)) {
            Stmt **all = ANEWN(&el->arena, Stmt *, nbody + ngen);
            if (nbody)
                memcpy(all, body, nbody * sizeof(Stmt *));
            memcpy(all + nbody, gen, ngen * sizeof(Stmt *));
            body = all;
            nbody += ngen;
        } else {
            mi->failed = 1;
        }
    }
    elb_module_attrs(el, &mc);
    elb_params(el, &mc);
    if (!mc.is_extern)
        elb_bind_decls(el, body, nbody, scope);
    /* ports first: they define the default domain */
    for (i = 0; i < nbody; i++)
        if (body[i]->kind == STMT_PORT)
            elb_declare_port(el, body[i], scope);
    if (mc.clock_ports.len == 1) {
        uint32_t ck = mc.clock_ports.data[0];
        uint32_t rs = mc.reset_ports.len == 1 ? mc.reset_ports.data[0] : IR_NONE;
        uint32_t d = ir_add_domain(el->design, mi->ir, "", ck, rs, elb_clock_hz(el, ck, body, nbody));
        mi->ir->default_domain = d;
    }
    mi->nclocks = (uint32_t)mc.clock_ports.len;
    if (mc.is_extern) {
        for (i = 0; i < nbody; i++)
            if (body[i]->kind != STMT_PORT && body[i]->kind != STMT_ITEM && body[i]->kind != STMT_ERROR &&
                body[i]->kind != STMT_STATIC_ASSERT)
                el_error(el, body[i]->span, "an extern module declares only ports");
    } else {
        elb_run_stmts(el, body, nbody, scope);
    }
    /* port domains (`@ dom` or the default domain) */
    for (i = 0; i < mi->nports; i++) {
        Stmt *ps = NULL;
        uint32_t k, d = mi->ir->default_domain;
        for (k = 0; k < nbody; k++)
            if (body[k]->kind == STMT_PORT && body[k]->u.port.name.name == mi->ports[i].name)
                ps = body[k];
        if (ps && ps->u.port.domain) {
            if (mc.is_extern) {
                /* extern modules only have clock ports as domains */
                Value dv = elab_eval(el, ps->u.port.domain, scope, NULL);
                if (dv.kind == VAL_HW && dv.ty && dv.ty->kind == TY_CLOCK && dv.u.e->kind == IRE_SIG)
                    d = elb_domain_for_clock(el, dv.u.e->sig, 1);
                else if (dv.kind == VAL_DOMAIN)
                    d = dv.u.dom->idx;
            } else {
                d = el_domain_of(el, ps->u.port.domain, scope, ps->u.port.domain->span);
            }
        }
        mi->ports[i].domain = d;
        if (!mi->ports[i].is_clock)
            elb_set_domain_tree(mi->ir, mi->ports[i].place, d);
    }
    /* a property is checked in the domain of the signals it reads (when they
     * agree), not always in the default domain */
    if (mi->ir->ndomains > 1)
        for (i = 0; i < mi->ir->nprops; i++) {
            uint32_t d = IR_NONE;
            elb_seq_domain(mi->ir, mi->ir->props[i].seq, &d);
            if (d < mi->ir->ndomains)
                mi->ir->props[i].domain = d;
        }
    elb_finish(el, &mc);
    elb_free_ctx(&mc);
    scope->mod = NULL;
    el->mod = saved_mod;
    el->blk = saved_blk;
    el->in_fn = saved_fn;
    el->undo_depth = saved_undo;
    ir_design_add_module(el->design, mi->ir);
    /* errors here or in the instances elaborated meanwhile: the checks
     * skip the module (its IR is incomplete) but still run elsewhere */
    if (mi->failed || diag_error_count() != errs0) {
        mi->failed = 1;
        mi->ir->elab_failed = 1;
    }
    mi->state = 2;
    el->mod_depth--;
    return mi;
}

/* --------------------------------------------------------------- regmaps */

void elab_add_regmap(Elab *el, Item *rm, FileUnit *file)
{
    IrRegmap r;
    uint32_t i, k;
    Env *env;
    if (!el->design)
        return;
    env = elab_env_new(el, NULL, file);
    memset(&r, 0, sizeof r);
    r.name = rm->name.name;
    r.package = file && file->pkg->path[0] ? file->pkg->path : NULL;
    r.loc = rm->span;
    r.nregs = rm->u.regmap.nentries;
    r.regs = (IrReg *)xcalloc(r.nregs ? r.nregs : 1, sizeof(IrReg));
    for (i = 0; i < r.nregs; i++) {
        RegEntry *e = rm->u.regmap.entries[i];
        IrReg *g = &r.regs[i];
        Value off = elab_eval_const(el, e->offset, env, el->S->t_int, "a register offset");
        const char *acc = e->access.name ? e->access.name : "rw";
        uint64_t o = 0;
        val_get_u64(&off, &o);
        g->offset = o;
        g->name = e->name.name;
        g->access = strcmp(acc, "ro") == 0 ? IR_RO : strcmp(acc, "wo") == 0 ? IR_WO : strcmp(acc, "w1c") == 0 ? IR_W1C
                  : strcmp(acc, "rw1s") == 0 ? IR_RW1S : IR_RW;
        g->is_stream = e->kind == REG_STREAM;
        g->doc = e->doc ? e->doc->text : NULL;
        if (e->kind == REG_FIELDS) {
            uint32_t maxbit = 0;
            g->nfields = e->nfields;
            g->fields = (IrRegField *)xcalloc(e->nfields ? e->nfields : 1, sizeof(IrRegField));
            for (k = 0; k < e->nfields; k++) {
                RegField *f = e->fields[k];
                Value hi = elab_eval_const(el, f->bit_hi, env, el->S->t_int, "a bit position");
                Value lo = f->bit_lo ? elab_eval_const(el, f->bit_lo, env, el->S->t_int, "a bit position") : hi;
                int64_t h = 0, l = 0;
                Ty *ft = elab_eval_type(el, f->type, env);
                val_get_i64(&hi, &h);
                val_get_i64(&lo, &l);
                if (h < l) {
                    int64_t x = h;
                    h = l;
                    l = x;
                }
                g->fields[k].name = f->name.name;
                g->fields[k].lo = (uint32_t)l;
                g->fields[k].width = (uint32_t)(h - l + 1);
                g->fields[k].type = el_irty(el, ft);
                if ((uint32_t)h + 1 > maxbit)
                    maxbit = (uint32_t)h + 1;
            }
            g->width = maxbit <= 8 ? 8 : maxbit <= 16 ? 16 : maxbit <= 32 ? 32 : 64;
        } else {
            Ty *t = elab_eval_type(el, e->type, env);
            g->width = t->hw ? t->width : 32;
        }
    }
    ir_design_add_regmap(el->design, &r);
    for (i = 0; i < r.nregs; i++)
        free(r.regs[i].fields);
    free(r.regs);
    elab_env_free(env);
}

/* ------------------------------------------------------------- verifier */

static char elb_verify_what[256];
static Span elb_verify_at;

static int elb_vbad(const IrExpr *e, const char *what)
{
    if (!elb_verify_what[0]) {
        snprintf(elb_verify_what, sizeof elb_verify_what, "%s (%s, width %u)", what, ir_expr_kind_name(e->kind),
                 (unsigned)e->width);
        elb_verify_at = e->loc;
    }
    return 1;
}

static Map elb_verify_seen;

static int elb_verify_expr(const IrModule *m, const IrExpr *e, int in_prop)
{
    uint32_t i;
    int bad = 0;
    if (!e) {
        if (!elb_verify_what[0])
            snprintf(elb_verify_what, sizeof elb_verify_what, "missing expression");
        return 1;
    }
    if (map_has(&elb_verify_seen, MAP_PTR(e)))
        return 0;
    map_put(&elb_verify_seen, MAP_PTR(e), 1);
    if (e->width == 0)
        bad += elb_vbad(e, "zero width");
    for (i = 0; i < e->nargs; i++)
        bad += elb_verify_expr(m, e->args[i], in_prop);
    switch (e->kind) {
    case IRE_SIG:
        if (e->sig >= m->nsigs || m->sigs[e->sig].type->width != e->width)
            bad += elb_vbad(e, "signal width");
        break;
    case IRE_SLICE:
        if (e->lo + e->width > e->args[0]->width)
            bad += elb_vbad(e, "slice out of range");
        break;
    case IRE_BINARY:
        if (e->op >= IROP_SHL && e->op <= IROP_ASHR) {
            if (e->args[0]->width != e->width)
                bad += elb_vbad(e, "shift width");
        } else if (e->op >= IROP_EQ && e->op <= IROP_GE) {
            if (e->args[0]->width != e->args[1]->width || e->width != 1)
                bad += elb_vbad(e, "comparison operand widths");
        } else if (e->args[0]->width != e->width || e->args[1]->width != e->width) {
            bad += elb_vbad(e, "binary operand widths");
        }
        break;
    case IRE_INDEX:
        /* may be out of range (reads 0), but the base holds at least one element */
        if (e->args[0]->width < e->width)
            bad += elb_vbad(e, "index element wider than the array");
        break;
    case IRE_MUX:
        if (e->args[0]->width != 1 || e->args[1]->width != e->width || e->args[2]->width != e->width)
            bad += elb_vbad(e, "mux widths");
        break;
    case IRE_MEMREAD:
        if (e->sig >= m->nsigs || m->sigs[e->sig].kind != IRS_MEM)
            bad += elb_vbad(e, "memread of a non-memory");
        break;
    case IRE_PAST:
        if (!in_prop)
            bad += elb_vbad(e, "past outside a property");
        break;
    default:
        break;
    }
    return bad;
}

static int elb_verify_lvalue(const IrModule *m, const IrLvalue *lv, const IrExpr *rhs)
{
    uint64_t sw;
    int bad = 0;
    if (lv->sig >= m->nsigs)
        return elb_vbad(rhs, "assignment to an unknown signal");
    sw = m->sigs[lv->sig].type->width;
    if (lv->index) {
        bad += elb_verify_expr(m, lv->index, 0);
        if ((uint64_t)lv->lo + (uint64_t)(lv->len ? lv->len : 1) * lv->width > sw)
            bad += elb_vbad(rhs, "indexed assignment past the end of the signal");
    } else if ((uint64_t)lv->lo + lv->width > sw) {
        bad += elb_vbad(rhs, "assignment slice out of range");
    }
    return bad;
}

static int elb_verify_block(const IrModule *m, const IrBlock *b)
{
    uint32_t i, k;
    int bad = 0;
    for (i = 0; i < b->n; i++) {
        const IrStmt *s = b->stmts[i];
        switch (s->kind) {
        case IRST_ASSIGN:
            bad += elb_verify_expr(m, s->rhs, 0);
            if (s->rhs)
                bad += elb_verify_lvalue(m, &s->lhs, s->rhs);
            if (s->rhs && s->rhs->width != s->lhs.width)
                bad += elb_vbad(s->rhs, "assignment width");
            break;
        case IRST_IF:
            bad += elb_verify_expr(m, s->cond, 0) + elb_verify_block(m, &s->then_b) + elb_verify_block(m, &s->else_b);
            if (s->cond && s->cond->width != 1)
                bad += elb_vbad(s->cond, "condition width");
            break;
        case IRST_CASE:
            bad += elb_verify_expr(m, s->subject, 0);
            for (k = 0; k < s->narms; k++)
                bad += elb_verify_block(m, &s->arms[k].body);
            break;
        case IRST_MEMWRITE:
            bad += elb_verify_expr(m, s->addr, 0) + elb_verify_expr(m, s->data, 0);
            break;
        case IRST_ASSERT:
            bad += elb_verify_expr(m, s->cond, 0);
            break;
        }
    }
    return bad;
}

static int elb_verify_seq(const IrModule *m, const IrSeq *q)
{
    if (!q)
        return 0;
    return (q->kind == IRQ_EXPR ? elb_verify_expr(m, q->expr, 1) : 0) + elb_verify_seq(m, q->a) +
           elb_verify_seq(m, q->b);
}

int elab_ir_verify(const IrDesign *d)
{
    uint32_t mi, i;
    int total = 0;
    for (mi = 0; mi < d->nmodules; mi++) {
        const IrModule *m = d->modules[mi];
        int bad = 0;
        elb_verify_what[0] = 0;
        for (i = 0; i < m->nassigns; i++) {
            bad += elb_verify_expr(m, m->assigns[i].rhs, 0);
            bad += elb_verify_lvalue(m, &m->assigns[i].lhs, m->assigns[i].rhs);
            if (m->assigns[i].rhs->width != m->assigns[i].lhs.width)
                bad += elb_vbad(m->assigns[i].rhs, "continuous assignment width");
        }
        for (i = 0; i < m->nprocs; i++)
            bad += elb_verify_block(m, &m->procs[i].body);
        for (i = 0; i < m->nprops; i++)
            bad += elb_verify_seq(m, m->props[i].seq);
        for (i = 0; i < m->ninsts; i++) {
            uint32_t k;
            for (k = 0; k < m->insts[i].nconns; k++) {
                const IrConn *c = &m->insts[i].conns[k];
                if (c->expr && c->expr->width != m->insts[i].module->sigs[c->port].type->width)
                    bad += elb_vbad(c->expr, "connection width");
            }
        }
        map_clear(&elb_verify_seen);
        if (bad) {
            diag_error(m->loc, "internal error: %d IR invariant violation%s in module `%s`", bad, bad == 1 ? "" : "s",
                       m->name);
            diag_note(elb_verify_at, "first: %s", elb_verify_what);
        }
        total += bad;
    }
    map_free(&elb_verify_seen);
    return total;
}

#undef EL_MAX_MOD_DEPTH
