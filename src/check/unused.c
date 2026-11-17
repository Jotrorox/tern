/* unused.c — unused signals (`tern check --unused`, spec §7.2, §13.2).
 *
 * Warnings (errors under --strict) for
 *   - inputs the module never reads (a bundle port counts as read when
 *     any of its fields is: a module may ignore `user` of a stream),
 *   - wires (`let`) never read,
 *   - vars and registers assigned but never read (a register that only
 *     feeds itself counts as unread),
 *   - memories written but never read,
 *   - instance outputs the parent never reads ("an unused output is fine",
 *     so this is only reported with --unused), one warning per instance
 *     listing its unread output ports.
 * A signal counts as read when an assignment, a block (including its
 * conditions), an instance connection, a domain (clock/reset) or a
 * property reads any of its bits. Exempt: names with a `_`-prefixed
 * segment (`_spare`, an instance `_u`), #[test_port] and #[keep] signals,
 * unsafe_cross wires, output ports (read by the parent), and signals
 * declared outside the user's files (CheckOptions.is_user_file).
 *
 * Generic modules depend on their arguments (an elaboration-time `if` may
 * leave a `let` unread in one configuration), so a declaration is reported
 * only when it is unread in every elaboration of its module, and modules
 * elaborated only as roots with default generic arguments (`tern check` of
 * a library) are not reported at all. */
#include "check_impl.h"

/* records one elaboration of a candidate; msg (printf-formatted) is only
   used when unread */
static void unu_candidate(ChkCtx *cx, uint64_t key, Span loc, int unread, const char *help, const char *fmt, ...)
    TERN_PRINTF(6, 7);

static void unu_candidate(ChkCtx *cx, uint64_t key, Span loc, int unread, const char *help, const char *fmt, ...)
{
    uint64_t v;
    ChkUnused *u;
    if (!map_get(&cx->unused_idx, key, &v)) {
        ChkUnused n;
        memset(&n, 0, sizeof n);
        n.loc = loc;
        vec_push(cx->unused, n);
        v = cx->unused.len;
        map_put(&cx->unused_idx, key, v);
    }
    u = &cx->unused.data[v - 1];
    u->seen++;
    if (!unread)
        return;
    u->unread++;
    if (!u->msg) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        u->msg = arena_strdup(&cx->arena, buf);
        u->help = help ? arena_strdup(&cx->arena, help) : NULL;
    }
}

static uint64_t unu_key(Span loc, const char *what)
{
    return hash_combine(hash_combine(hash_u64(loc.file), hash_u64(loc.off)), (uint64_t)(uintptr_t)what) | 1;
}

void chk_unused_report(ChkCtx *cx)
{
    size_t i;
    for (i = 0; i < cx->unused.len; i++) {
        const ChkUnused *u = &cx->unused.data[i];
        if (u->msg && u->unread == u->seen && chk_warning(cx, u->loc, "%s", u->msg) && u->help)
            diag_help("%s", u->help);
    }
}

static int unu_exempt_name(const char *p)
{
    int start = 1;
    for (; p && *p; p++) {
        if (start && *p == '_')
            return 1;
        start = *p == '.' || *p == '[';
    }
    return 0;
}

/* the first path segment after `skip` characters ("tx" of "tx.valid") */
static const char *unu_port_of(const char *path, size_t skip)
{
    const char *p = path + skip, *e = p;
    while (*e && *e != '.' && *e != '[')
        e++;
    return intern(p, (size_t)(e - p));
}

static size_t unu_inst_prefix(const IrInstance *in, const char *path)
{
    size_t nl = strlen(in->name);
    if (strncmp(path, in->name, nl) == 0 && path[nl] == '.')
        return nl + 1;
    return 0;
}

void chk_unused(ChkMod *cm)
{
    IrModule *m = cm->m;
    ChkCtx *cx = cm->cx;
    const CheckOptions *o = cx->opt;
    uint8_t *rd;
    uint32_t i, r;
    Map port_read = {0};   /* hash(instance + 1, interned port) -> 1 if any leaf is read */
    Map port_done = {0};
    if (m->is_extern)
        return;
    /* a library module elaborated on its own with default generics */
    if (!map_has(&cx->instantiated, MAP_PTR(m)) && !m->is_top &&
        (m->nparams || (m->display && m->tern_name && strcmp(m->display, m->tern_name) != 0)))
        return;
    rd = (uint8_t *)xcalloc(m->nsigs + 1, 1);
    for (i = 0; i < cm->nodes.len; i++) {
        const ChkNode *n = &cm->nodes.data[i];
        for (r = n->rd; r < n->rd + n->nrd; r++) {
            uint32_t s = cm->reads.data[r].sig;
            if (s < m->nsigs && s != n->sig)
                rd[s] = 1;
        }
    }
    for (i = 0; i < m->ndomains; i++) {
        if (m->domains[i].clock < m->nsigs)
            rd[m->domains[i].clock] = 1;
        if (m->domains[i].reset < m->nsigs)
            rd[m->domains[i].reset] = 1;
    }
    /* which (bundle) ports have a read leaf */
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        uint32_t ii = cm->inst_wire_inst[i];
        size_t skip = ii ? unu_inst_prefix(&m->insts[ii - 1], s->path) : 0;
        if (!rd[i] || (s->kind != IRS_IN && !ii))
            continue;
        map_put(&port_read, hash_combine(hash_u64(ii), (uint64_t)(uintptr_t)unu_port_of(s->path, skip)), 1);
    }
    for (i = 0; i < m->nsigs; i++) {
        const IrSignal *s = &m->sigs[i];
        int un = !rd[i];
        if (s->kind == IRS_OUT || s->test_port || unu_exempt_name(s->path) || unu_exempt_name(s->name) ||
            chk_sig_attr(s, "keep") || chk_sig_attr(s, "unsafe_cross") || cm->inst_wire_inst[i])
            continue;
        if (o->is_user_file && !o->is_user_file(s->loc.file, o->user))
            continue;
        switch (s->kind) {
        case IRS_IN: {
            const char *port = unu_port_of(s->path, 0);
            uint64_t key = (uint64_t)(uintptr_t)port;
            if (map_has(&port_done, key))
                break;
            map_put(&port_done, key, 1);
            un = !map_has(&port_read, hash_combine(hash_u64(0), key));
            if (s->type && s->type->kind == IRT_CLOCK)
                unu_candidate(cx, unu_key(s->loc, port), s->loc, un, NULL, "clock input `%s` is never used", port);
            else
                unu_candidate(cx, unu_key(s->loc, port), s->loc, un, NULL, "input `%s` is never read", port);
            break;
        }
        case IRS_WIRE: unu_candidate(cx, unu_key(s->loc, s->path), s->loc, un, NULL, "`%s` is never read", s->path); break;
        case IRS_VAR:
            unu_candidate(cx, unu_key(s->loc, s->path), s->loc, un, NULL, "var `%s` is assigned but never read", s->path);
            break;
        case IRS_REG:
            unu_candidate(cx, unu_key(s->loc, s->path), s->loc, un, NULL, "register `%s` is assigned but never read",
                          s->path);
            break;
        case IRS_MEM:
            unu_candidate(cx, unu_key(s->loc, s->path), s->loc, un, NULL, "memory `%s` is written but never read",
                          s->path);
            break;
        default: break;
        }
    }
    /* instance outputs, one warning per instance */
    for (i = 0; i < m->ninsts; i++) {
        const IrInstance *in = &m->insts[i];
        StrBuf sb = {0};
        Map seen = {0};
        uint32_t k, n = 0;
        if (unu_exempt_name(in->name) || (o->is_user_file && !o->is_user_file(in->loc.file, o->user)))
            continue;
        for (k = 0; k < in->nconns; k++) {
            const IrConn *cn = &in->conns[k];
            const char *port;
            uint32_t w;
            if (!cn->expr || cn->expr->kind != IRE_SIG || !in->module || cn->port >= in->module->nsigs ||
                in->module->sigs[cn->port].kind != IRS_OUT)
                continue;
            w = cn->expr->sig;
            if (w >= m->nsigs || rd[w])
                continue;
            port = unu_port_of(m->sigs[w].path, unu_inst_prefix(in, m->sigs[w].path));
            if (map_has(&port_read, hash_combine(hash_u64(i + 1), (uint64_t)(uintptr_t)port)) ||
                map_has(&seen, (uint64_t)(uintptr_t)port) || unu_exempt_name(port))
                continue;
            map_put(&seen, (uint64_t)(uintptr_t)port, 1);
            sb_printf(&sb, "%s`%s`", n ? ", " : "", port);
            n++;
        }
        {
            char help[160];
            snprintf(help, sizeof help, "name the instance `_%s` if its unused outputs are intended", in->name);
            unu_candidate(cx, unu_key(in->loc, intern_cstr(sb_cstr(&sb))), in->loc, n != 0, help,
                          "%s %s of instance `%s` %s never read", n == 1 ? "output" : "outputs", sb_cstr(&sb), in->name,
                          n == 1 ? "is" : "are");
        }
        sb_free(&sb);
        map_free(&seen);
    }
    map_free(&port_read);
    map_free(&port_done);
    free(rd);
}
