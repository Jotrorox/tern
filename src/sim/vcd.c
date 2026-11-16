/* vcd.c — Value Change Dump output (IEEE 1364 §18), timescale 1ps.
 * One scope per instance (named after the instance, the top after its
 * module), every non-memory signal under its SV name. */
#include "sim_impl.h"

static void vcd_id(char *buf, uint32_t n)
{
    int i = 0;
    do {
        buf[i++] = (char)(33 + n % 94);
        n /= 94;
    } while (n && i < 7);
    buf[i] = 0;
}

static void vcd_value(SimModel *m, const SimSig *s, StrBuf *sb)
{
    const uint64_t *v = m->v + s->off;
    if (s->width == 1) {
        sb_putc(sb, (v[0] & 1u) ? '1' : '0');
        sb_puts(sb, s->vcd_id);
        sb_putc(sb, '\n');
        return;
    }
    sb_putc(sb, 'b');
    bv_print_bin(sb, v, s->width, 1);
    sb_putc(sb, ' ');
    sb_puts(sb, s->vcd_id);
    sb_putc(sb, '\n');
}

static void vcd_scope(SimModel *m, uint32_t ii, StrBuf *sb)
{
    const SimInst *in = &m->insts.data[ii];
    uint32_t i;
    sb_printf(sb, "$scope module %s $end\n", ii == 0 ? in->mod->name : in->name);
    for (i = 0; i < in->mod->nsigs; i++) {
        const SimSig *s = &m->sigs.data[in->sig_base + i];
        if (s->mem != IR_NONE)
            continue;
        sb_printf(sb, "$var %s %u %s %s", s->is_state ? "reg" : "wire", s->width, s->vcd_id,
                  s->ir->name);
        if (s->width > 1)
            sb_printf(sb, " [%u:0]", s->width - 1);
        sb_puts(sb, " $end\n");
    }
    for (i = 0; i < in->nchildren; i++)
        if (in->children[i] < m->insts.len && m->insts.data[in->children[i]].parent == ii)
            vcd_scope(m, in->children[i], sb);
    sb_puts(sb, "$upscope $end\n");
}

int sim_vcd_open(SimModel *m, const char *path)
{
    StrBuf sb = {0};
    uint32_t i, n = 0;
    char id[8];
    sim_vcd_close(m);
    m->vcd = fopen(path, "wb");
    if (!m->vcd)
        return SIM_ERR_IO;
    for (i = 0; i < m->sigs.len; i++) {
        SimSig *s = &m->sigs.data[i];
        if (s->mem != IR_NONE)
            continue;
        vcd_id(id, n++);
        s->vcd_id = arena_strdup(&m->arena, id);
    }
    sb_printf(&sb, "$version tern %s $end\n$timescale 1ps $end\n", TERN_VERSION);
    if (m->insts.len)
        vcd_scope(m, 0, &sb);
    sb_puts(&sb, "$enddefinitions $end\n");
    sb_printf(&sb, "#%llu\n$dumpvars\n", (unsigned long long)m->now_ps);
    for (i = 0; i < m->sigs.len; i++)
        if (m->sigs.data[i].mem == IR_NONE)
            vcd_value(m, &m->sigs.data[i], &sb);
    sb_puts(&sb, "$end\n");
    fwrite(sb.data, 1, sb.len, m->vcd);
    sb_free(&sb);
    free(m->vcd_last);
    m->vcd_last = (uint64_t *)xmalloc(sizeof(uint64_t) * (m->sig_limbs ? m->sig_limbs : 1));
    memcpy(m->vcd_last, m->v, sizeof(uint64_t) * m->sig_limbs);
    m->vcd_time = m->now_ps;
    m->vcd_time_valid = 1;
    fflush(m->vcd);
    return SIM_OK;
}

void sim_vcd_sample(SimModel *m)
{
    StrBuf sb = {0};
    uint32_t i;
    if (!m->vcd)
        return;
    for (i = 0; i < m->sigs.len; i++) {
        const SimSig *s = &m->sigs.data[i];
        if (s->mem != IR_NONE)
            continue;
        if (memcmp(m->vcd_last + s->off, m->v + s->off, sizeof(uint64_t) * s->limbs) == 0)
            continue;
        if (!m->vcd_time_valid || m->vcd_time != m->now_ps) {
            sb_printf(&sb, "#%llu\n", (unsigned long long)m->now_ps);
            m->vcd_time = m->now_ps;
            m->vcd_time_valid = 1;
        }
        memcpy(m->vcd_last + s->off, m->v + s->off, sizeof(uint64_t) * s->limbs);
        vcd_value(m, s, &sb);
    }
    if (sb.len)
        fwrite(sb.data, 1, sb.len, m->vcd);
    sb_free(&sb);
}

void sim_vcd_flush(SimModel *m)
{
    if (m->vcd)
        fflush(m->vcd);
}

void sim_vcd_close(SimModel *m)
{
    if (!m || !m->vcd)
        return;
    if (m->vcd_time_valid && m->now_ps > m->vcd_time)
        fprintf(m->vcd, "#%llu\n", (unsigned long long)m->now_ps);
    fclose(m->vcd);
    m->vcd = NULL;
    free(m->vcd_last);
    m->vcd_last = NULL;
}
