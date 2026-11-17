/* remote.c — a SimModel whose values live in another simulator (spec
 * §13.4: `tern test --backend verilator`).
 *
 * The model is built from the IR exactly like a built-in one (sim_build), so
 * handles, paths, types, widths, domains and writability are the same and
 * the test interpreter cannot tell the difference; only the value
 * operations (poke, peek, settle, step, run, reset, waves) are forwarded,
 * as text lines, to a harness process (src/backend/vlsim.c builds and
 * starts a Verilator one). Cycle counts and time are kept here with the
 * built-in simulator's clock algorithm, which the harness repeats.
 *
 * Protocol (requests; every response line starts with '@', other lines are
 * the harness's own output and are kept for failure reports):
 *   C <i> <half_ps> <falling> <path>   declare top clock i       (no reply)
 *   R <active_low> <path>              declare a top reset       (no reply)
 *   I                                  initial evaluation        @ok
 *   P <path> <hex>                     drive                     @ok | @err <msg>
 *   G <path>                           read                      @v <hex> | @err <msg>
 *   M <path> <addr>                    read a memory word        @v <hex> | @err <msg>
 *   E                                  settle                    @ok
 *   S <i> <n>                          n cycles of clock i       @ok
 *   N <ns>                             all clocks for ns         @ok
 *   X <i>...                           resets on, 2 cycles of each clock i, off  @ok
 *   D <path>                           VCD waves                 @ok | @err <msg>
 *   Q                                  quit                      @bye
 * Any request may be answered with @stop: the model stopped (a failed
 * assertion or $finish); it is reported once through the fail handler with
 * the harness output that preceded it. */
#include "sim_impl.h"
#include "../base/diag.h"
#include "../base/str.h"
#include "../os/os.h"

struct SimRemote {
    SimRemoteIo io;
    const char **paths;       /* per handle, cached */
    StrBuf log;               /* harness output since the last response */
    int dead;                 /* stopped or gone: no more requests */
    int reported;
};

/* ------------------------------------------------------------ values -- */

void sim_remote_hex(const IrBits *b, uint32_t width, StrBuf *out)
{
    uint32_t nd = width ? (width + 3) / 4 : 1, i;
    for (i = nd; i-- > 0;) {
        uint32_t bit = i * 4, k, nib = 0;
        for (k = 0; k < 4; k++)
            if (bit + k < width && bit + k < b->width && ir_bits_get(b, bit + k))
                nib |= 1u << k;
        sb_putc(out, "0123456789abcdef"[nib]);
    }
}

int sim_remote_unhex(const char *s, IrBits *out)
{
    size_t n = 0, i;
    uint32_t bit = 0, k;
    if (!s)
        return 0;
    while (*s == ' ')
        s++;
    while (s[n] && s[n] != ' ')
        n++;
    memset(out->w, 0, sizeof(uint64_t) * IR_LIMBS(out->width));
    for (i = n; i-- > 0 && bit < out->width; bit += 4) {
        char ch = s[i];
        int v;
        if (ch >= '0' && ch <= '9')
            v = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            v = ch - 'A' + 10;
        else if (ch == 'x' || ch == 'X' || ch == 'z' || ch == 'Z' || ch == '?')
            v = 0; /* 2-state: unknown bits read as 0 */
        else
            return 0;
        for (k = 0; k < 4 && bit + k < out->width; k++)
            if (v & (1 << k))
                ir_bits_set(out, bit + k, 1);
    }
    return 1;
}

/* ---------------------------------------------------------- requests -- */

static void simr_fail(SimModel *m, const char *what)
{
    SimRemote *r = m->remote;
    SimFailure f;
    if (r->reported)
        return;
    r->reported = 1;
    memset(&f, 0, sizeof f);
    f.kind = SIM_FAIL_ASSERT;
    f.what = what;
    if (r->io.locate && !r->io.locate(r->io.user, what, &f.loc))
        memset(&f.loc, 0, sizeof f.loc);
    f.inst_path = "";
    f.time_ps = m->now_ps;
    f.cycle = m->default_clock != IR_NONE ? m->clocks.data[m->default_clock].cycles : 0;
    m->nfail++;
    if (m->fail_fn)
        m->fail_fn(m->fail_user, &f);
    else
        fprintf(stderr, "%s\n", what);
}

/* the first line of the harness output since the last response, without
 * the simulator's "[time] %Error: " prefix */
static const char *simr_log_text(SimRemote *r)
{
    char *s = r->log.data ? r->log.data : (char *)"", *e;
    if (*s == '[') {
        char *q = strchr(s, ']');
        if (q)
            s = q + 1;
    }
    while (*s == ' ')
        s++;
    if (strncmp(s, "%Error: ", 8) == 0)
        s += 8;
    e = strchr(s, '\n');
    if (e)
        *e = 0;
    return s;
}

/* Sends a request and reads its response line (heap; NULL when the model
 * stopped or is gone, after reporting it once). want = 0: no response. */
static char *simr_request(SimModel *m, const char *line, int want)
{
    SimRemote *r = m->remote;
    char *resp;
    if (r->dead)
        return NULL;
    if (r->io.write(r->io.user, line) != 0) {
        r->dead = 1;
        simr_fail(m, "verilator model exited unexpectedly");
        return NULL;
    }
    if (!want)
        return NULL;
    sb_reset(&r->log);
    for (;;) {
        resp = r->io.read_line(r->io.user);
        if (!resp) {
            StrBuf w = {0};
            r->dead = 1;
            sb_printf(&w, "verilator model exited unexpectedly%s%s", r->log.len ? ": " : "", simr_log_text(r));
            simr_fail(m, arena_strdup(&m->arena, sb_cstr(&w)));
            sb_free(&w);
            return NULL;
        }
        if (resp[0] == '@')
            break;
        sb_printf(&r->log, "%s\n", resp);
        free(resp);
    }
    if (strncmp(resp, "@stop", 5) == 0) {
        StrBuf w = {0};
        r->dead = 1;
        if (r->log.len)
            sb_printf(&w, "verilator: %s", simr_log_text(r));
        else
            sb_puts(&w, "verilator stopped the simulation");
        simr_fail(m, arena_strdup(&m->arena, sb_cstr(&w)));
        sb_free(&w);
        free(resp);
        return NULL;
    }
    if (strncmp(resp, "@err", 4) == 0) {
        diag_error(span_none(), "verilator backend: %s", resp[4] ? resp + 5 : "error");
        free(resp);
        return NULL;
    }
    return resp;
}

static void simr_simple(SimModel *m, const char *line)
{
    free(simr_request(m, line, 1));
}

static const char *simr_path(SimModel *m, int h)
{
    SimRemote *r = m->remote;
    if (!r->paths[h])
        r->paths[h] = r->io.path(r->io.user, m, h);
    return r->paths[h];
}

/* --------------------------------------------------------- operations -- */

static int simr_poke(SimModel *m, int h, const IrBits *v)
{
    StrBuf l = {0};
    const SimSig *s = &m->sigs.data[h];
    char *resp;
    /* keep the local copy too (the VCD-less metadata paths read it) */
    if (v->width == 0)
        bv_zero(m->v + s->off, s->width);
    else
        bv_resize(m->v + s->off, s->width, v->w, v->width, 0);
    sb_printf(&l, "P %s ", simr_path(m, h));
    sim_remote_hex(v, s->width, &l);
    sb_putc(&l, '\n');
    resp = simr_request(m, sb_cstr(&l), 1);
    sb_free(&l);
    m->dirty = 1;
    if (!resp)
        return SIM_ERR_IO;
    free(resp);
    return SIM_OK;
}

static int simr_value(SimModel *m, const char *req, IrBits *out)
{
    char *resp = simr_request(m, req, 1);
    int ok = resp && strncmp(resp, "@v ", 3) == 0 && sim_remote_unhex(resp + 3, out);
    if (!ok)
        memset(out->w, 0, sizeof(uint64_t) * IR_LIMBS(out->width));
    free(resp);
    return ok ? SIM_OK : SIM_ERR_IO;
}

static int simr_peek(SimModel *m, int h, IrBits *out)
{
    StrBuf l = {0};
    int rc;
    out->width = m->sigs.data[h].width;
    sb_printf(&l, "G %s\n", simr_path(m, h));
    rc = simr_value(m, sb_cstr(&l), out);
    sb_free(&l);
    return rc;
}

static int simr_mem_peek(SimModel *m, int h, uint64_t addr, IrBits *out)
{
    StrBuf l = {0};
    int rc;
    out->width = m->mems.data[m->sigs.data[h].mem].width;
    sb_printf(&l, "M %s %llu\n", simr_path(m, h), (unsigned long long)addr);
    rc = simr_value(m, sb_cstr(&l), out);
    sb_free(&l);
    return rc;
}

static void simr_settle(SimModel *m)
{
    simr_simple(m, "E\n");
    m->dirty = 0;
}

/* the clock bookkeeping of eval.c's ev_step_clock, without evaluation */
static void simr_count_steps(SimModel *m, uint32_t ci, uint64_t n)
{
    SimClock *c = &m->clocks.data[ci];
    m->now_ps += 2u * n * c->half_ps;
    c->cycles += n;
    c->next_toggle = m->now_ps + c->half_ps;
}

static void simr_step(SimModel *m, uint32_t ci, uint32_t n)
{
    char l[64];
    snprintf(l, sizeof l, "S %u %u\n", (unsigned)ci, (unsigned)n);
    simr_simple(m, l);
    simr_count_steps(m, ci, n);
    m->dirty = 0;
}

static void simr_run_ns(SimModel *m, uint64_t ns)
{
    char l[64];
    uint64_t target = m->now_ps + ns * 1000u;
    snprintf(l, sizeof l, "N %llu\n", (unsigned long long)ns);
    simr_simple(m, l);
    /* eval.c's sim_run_ns, counting active edges */
    for (;;) {
        uint64_t t = ~UINT64_C(0);
        uint32_t i;
        for (i = 0; i < m->clocks.len; i++) {
            uint64_t nt = TERN_MAX(m->clocks.data[i].next_toggle, m->now_ps);
            if (nt < t)
                t = nt;
        }
        if (t > target || !m->clocks.len)
            break;
        m->now_ps = t;
        for (i = 0; i < m->clocks.len; i++) {
            SimClock *c = &m->clocks.data[i];
            if (TERN_MAX(c->next_toggle, m->now_ps) != t)
                continue;
            c->level = !c->level;
            if (c->level == (c->falling ? 0 : 1))
                c->cycles++;
            c->next_toggle = t + c->half_ps;
        }
    }
    m->now_ps = target;
    m->dirty = 0;
}

static void simr_reset(SimModel *m)
{
    StrBuf l = {0};
    VEC(uint32_t) clks = {0};
    const IrModule *top = m->top;
    uint32_t i, j;
    /* the clocks eval.c's sim_reset steps */
    for (i = 0; i < top->ndomains; i++) {
        uint32_t c = m->topdoms.data[i].clock;
        if (top->domains[i].reset == IR_NONE)
            continue;
        for (j = 0; j < clks.len; j++)
            if (clks.data[j] == c)
                break;
        if (j == clks.len)
            vec_push(clks, c);
    }
    if (!clks.len)
        for (i = 0; i < m->clocks.len; i++)
            vec_push(clks, i);
    sb_puts(&l, "X");
    for (i = 0; i < clks.len; i++)
        sb_printf(&l, " %u", (unsigned)clks.data[i]);
    sb_putc(&l, '\n');
    simr_simple(m, sb_cstr(&l));
    for (i = 0; i < clks.len; i++)
        simr_count_steps(m, clks.data[i], 2);
    vec_free(clks);
    sb_free(&l);
    m->dirty = 0;
}

static int simr_vcd_open(SimModel *m, const char *path)
{
    StrBuf l = {0};
    char *resp, *abs = os_path_abs(path); /* the model runs in its build directory */
    sb_printf(&l, "D %s\n", abs ? abs : path);
    free(abs);
    resp = simr_request(m, sb_cstr(&l), 1);
    sb_free(&l);
    if (!resp)
        return SIM_ERR_IO;
    free(resp);
    return SIM_OK;
}

static void simr_destroy(SimModel *m)
{
    SimRemote *r = m->remote;
    if (!r)
        return;
    if (!r->dead)
        free(simr_request(m, "Q\n", 1));
    if (r->io.close)
        r->io.close(r->io.user);
    free(r->paths);
    sb_free(&r->log);
    free(r);
    m->remote = NULL;
}

static const SimRemoteOps simr_ops = {
    simr_poke, simr_peek, simr_mem_peek, simr_settle, simr_step, simr_run_ns, simr_reset, simr_vcd_open,
    simr_destroy,
};

/* ---------------------------------------------------------- lifecycle -- */

static void simr_init_fail(void *user, const SimFailure *f)
{
    TERN_UNUSED(user);
    diag_error(span_none(), "%s", f->what);
}

SimModel *sim_new_remote(const IrDesign *d, const IrModule *top, uint64_t seed, const SimRemoteIo *io)
{
    SimModel *m;
    SimRemote *r;
    StrBuf l = {0};
    uint32_t i;
    char *resp;
    if (!top)
        top = d ? d->top : NULL;
    if (!top) {
        diag_error(span_none(), "simulation: no top module");
        if (io->close)
            io->close(io->user);
        return NULL;
    }
    m = (SimModel *)xcalloc(1, sizeof(SimModel));
    r = (SimRemote *)xcalloc(1, sizeof(SimRemote));
    r->io = *io;
    m->design = d;
    m->top = top;
    m->seed = seed;
    m->rops = &simr_ops;
    m->remote = r;
    if (!sim_build(m)) {
        sim_free(m);
        return NULL;
    }
    r->paths = (const char **)xcalloc(m->sigs.len + 1, sizeof(char *));
    /* clocks and resets of the top, then the initial evaluation */
    for (i = 0; i < m->clocks.len; i++) {
        const SimClock *c = &m->clocks.data[i];
        sb_reset(&l);
        sb_printf(&l, "C %u %llu %d %s\n", (unsigned)i, (unsigned long long)c->half_ps, c->falling,
                  simr_path(m, (int)c->sig));
        simr_request(m, sb_cstr(&l), 0);
    }
    for (i = 0; i < top->nsigs; i++) {
        const IrSignal *s = &top->sigs[i];
        if (s->kind != IRS_IN || s->type->kind != IRT_RESET)
            continue;
        sb_reset(&l);
        sb_printf(&l, "R %d %s\n", s->type->active_low ? 1 : 0, simr_path(m, (int)i));
        simr_request(m, sb_cstr(&l), 0);
    }
    sb_free(&l);
    m->fail_fn = simr_init_fail;
    resp = simr_request(m, "I\n", 1);
    m->fail_fn = NULL;
    if (!resp) {
        if (!r->reported)
            diag_error(span_none(), "verilator backend: the model did not start");
        sim_free(m);
        return NULL;
    }
    free(resp);
    m->dirty = 0;
    return m;
}

int sim_is_remote(const SimModel *m)
{
    return m && m->rops != NULL;
}
