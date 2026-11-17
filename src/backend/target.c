/* target.c — build targets ([target.<name>] of tern.toml, docs/packages.md)
 * and helpers shared by build, synth and prove: device-family detection,
 * source-path rewriting in emitted files, HDL compile order. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/diag.h"
#include "../os/os.h"
#include "../pkg/pkg.h"
#include "backend.h"

/* ============================================================== targets */

static int bk_istarts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tern_to_lower((unsigned char)*s) != tern_to_lower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int bk_icontains(const char *s, const char *needle)
{
    for (; *s; s++)
        if (bk_istarts(s, needle))
            return 1;
    return 0;
}

BkFamily bk_family_from_device(const char *dev)
{
    if (!dev || !*dev)
        return BK_FAM_GENERIC;
    if (bk_istarts(dev, "xc") || bk_istarts(dev, "xa7") || bk_istarts(dev, "xq"))
        return BK_FAM_XILINX;
    if (bk_istarts(dev, "ice40") || bk_istarts(dev, "ice5") || bk_istarts(dev, "hx") || bk_istarts(dev, "lp") ||
        bk_istarts(dev, "up5k") || bk_istarts(dev, "up3k") || bk_istarts(dev, "u4k") || bk_istarts(dev, "ice"))
        return BK_FAM_ICE40;
    if (bk_istarts(dev, "lfe5") || bk_istarts(dev, "ecp5") || bk_istarts(dev, "25k") || bk_istarts(dev, "45k") ||
        bk_istarts(dev, "85k") || bk_istarts(dev, "um5g") || bk_istarts(dev, "um-"))
        return BK_FAM_ECP5;
    if (bk_istarts(dev, "gw1n") || bk_istarts(dev, "gw2a") || bk_istarts(dev, "gw5a"))
        return BK_FAM_GOWIN;
    if (bk_istarts(dev, "5c") || bk_istarts(dev, "5s") || bk_istarts(dev, "5a") || bk_istarts(dev, "10m") ||
        bk_istarts(dev, "10c") || bk_istarts(dev, "10a") || bk_istarts(dev, "ep4") || bk_istarts(dev, "ep3") ||
        bk_istarts(dev, "1s") || bk_istarts(dev, "agf") || bk_istarts(dev, "ag") || bk_istarts(dev, "a5e"))
        return BK_FAM_INTEL;
    if (bk_icontains(dev, "sky130") || bk_icontains(dev, "gf180") || bk_icontains(dev, "ihp") ||
        bk_icontains(dev, "asic"))
        return BK_FAM_ASIC;
    return BK_FAM_GENERIC;
}

const char *bk_family_name(BkFamily f)
{
    switch (f) {
    case BK_FAM_XILINX: return "xilinx";
    case BK_FAM_INTEL: return "intel";
    case BK_FAM_ICE40: return "ice40";
    case BK_FAM_ECP5: return "ecp5";
    case BK_FAM_GOWIN: return "gowin";
    case BK_FAM_ASIC: return "asic";
    case BK_FAM_GENERIC: break;
    }
    return "generic";
}

const char *bk_tool_name(BkTool t)
{
    switch (t) {
    case BK_TOOL_YOSYS: return "yosys";
    case BK_TOOL_VIVADO: return "vivado";
    case BK_TOOL_QUARTUS: return "quartus";
    case BK_TOOL_NONE: break;
    }
    return "none";
}

int bk_tool_parse(const char *s, BkTool *out)
{
    if (tern_strcasecmp(s, "yosys") == 0)
        *out = BK_TOOL_YOSYS;
    else if (tern_strcasecmp(s, "vivado") == 0)
        *out = BK_TOOL_VIVADO;
    else if (tern_strcasecmp(s, "quartus") == 0)
        *out = BK_TOOL_QUARTUS;
    else
        return -1;
    return 0;
}

BkTool bk_family_default_tool(BkFamily f)
{
    if (f == BK_FAM_XILINX)
        return BK_TOOL_VIVADO;
    if (f == BK_FAM_INTEL)
        return BK_TOOL_QUARTUS;
    return BK_TOOL_YOSYS;
}

static const TomlValue *bk_extra(const TernTarget *tt, const char *key)
{
    size_t i;
    for (i = 0; tt && i < tt->nextra; i++)
        if (strcmp(tt->extra[i].key, key) == 0)
            return tt->extra[i].value;
    return NULL;
}

/* a string or an array of strings, as "a,b,c" */
static const char *bk_list_value(const TomlValue *v, const char *key, const char *where, Arena *a, int *errs)
{
    StrBuf sb = {0};
    const TomlValue *it;
    const char *r;
    if (!v)
        return NULL;
    if (v->kind == TOML_STRING)
        return arena_strdup(a, v->str);
    if (v->kind != TOML_ARRAY) {
        diag_error(v->span, "`%s` in [%s] must be a string or an array of strings", key, where);
        (*errs)++;
        return NULL;
    }
    for (it = v->first; it; it = it->next) {
        if (it->kind != TOML_STRING) {
            diag_error(it->span, "`%s` in [%s] must contain strings", key, where);
            (*errs)++;
            continue;
        }
        if (sb.len)
            sb_putc(&sb, ',');
        sb_puts(&sb, it->str);
    }
    r = arena_strdup(a, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static const char *bk_str_value(const TomlValue *v, const char *key, const char *where, Arena *a, int *errs)
{
    if (!v)
        return NULL;
    if (v->kind != TOML_STRING) {
        diag_error(v->span, "`%s` in [%s] must be a string", key, where);
        (*errs)++;
        return NULL;
    }
    return arena_strdup(a, v->str);
}

/* "xdc,sdc" -> bits; returns -1 on an unknown word (reported at span) */
static int bk_parse_bits(const char *list, const char *const *names, const unsigned *bits, size_t n, unsigned *out,
                         Span span, const char *what)
{
    const char *p = list;
    *out = 0;
    while (*p) {
        const char *e = p;
        size_t i, len;
        int ok = 0;
        while (*e && *e != ',' && *e != ' ')
            e++;
        len = (size_t)(e - p);
        for (i = 0; i < n; i++)
            if (len && strlen(names[i]) == len && strncmp(names[i], p, len) == 0) {
                *out |= bits[i];
                ok = 1;
            }
        if (len && !ok) {
            diag_error(span, "unknown %s `%.*s`", what, (int)len, p);
            return -1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

int bk_target_resolve(const TernTarget *tt, const char *name, BkTarget *t, Arena *a)
{
    static const char *const cons_names[] = {"none", "xdc", "sdc"};
    static const unsigned cons_bits[] = {0, EMIT_CONS_XDC, EMIT_CONS_SDC};
    static const char *const mem_names[] = {"hex", "mif", "coe"};
    static const unsigned mem_bits[] = {0, SV_MEMINIT_MIF, SV_MEMINIT_COE};
    const char *where, *s;
    const TomlValue *v;
    int errs = 0;
    memset(t, 0, sizeof *t);
    t->name = arena_strdup(a, tt ? tt->name : (name ? name : "default"));
    t->std = SV_STD_SV2012;
    t->kind = SV_TARGET_FPGA;
    t->constraints = EMIT_CONS_XDC | EMIT_CONS_SDC;
    if (!tt)
        return 0;
    t->from_manifest = 1;
    where = arena_printf(a, "target.%s", tt->name);
    t->device = tt->device ? arena_strdup(a, tt->device) : NULL;
    t->top = tt->top ? arena_strdup(a, tt->top) : NULL;
    if (tt->std && strcmp(tt->std, "verilog2005") == 0)
        t->std = SV_STD_V2005;
    t->family = bk_family_from_device(t->device);
    s = bk_str_value(bk_extra(tt, "vendor"), "vendor", where, a, &errs);
    if (s) {
        if (tern_strcasecmp(s, "xilinx") == 0 || tern_strcasecmp(s, "amd") == 0)
            t->family = BK_FAM_XILINX;
        else if (tern_strcasecmp(s, "intel") == 0 || tern_strcasecmp(s, "altera") == 0)
            t->family = BK_FAM_INTEL;
        else if (tern_strcasecmp(s, "ice40") == 0 ||
                 (tern_strcasecmp(s, "lattice") == 0 && bk_family_from_device(t->device) != BK_FAM_ECP5))
            t->family = BK_FAM_ICE40;
        else if (tern_strcasecmp(s, "ecp5") == 0 || tern_strcasecmp(s, "lattice") == 0)
            t->family = BK_FAM_ECP5;
        else if (tern_strcasecmp(s, "gowin") == 0)
            t->family = BK_FAM_GOWIN;
        else if (tern_strcasecmp(s, "asic") == 0)
            t->family = BK_FAM_ASIC;
        else if (tern_strcasecmp(s, "generic") == 0)
            t->family = BK_FAM_GENERIC;
        else {
            diag_error(bk_extra(tt, "vendor")->span, "unknown vendor \"%s\" in [%s]", s, where);
            diag_help("use one of xilinx, intel, lattice, ice40, ecp5, gowin, asic, generic");
            errs++;
        }
    }
    if (t->family == BK_FAM_ASIC)
        t->kind = SV_TARGET_ASIC;
    s = bk_str_value((v = bk_extra(tt, "kind")) ? v : bk_extra(tt, "target_kind"), "kind", where, a, &errs);
    if (s) {
        if (strcmp(s, "fpga") == 0)
            t->kind = SV_TARGET_FPGA;
        else if (strcmp(s, "asic") == 0)
            t->kind = SV_TARGET_ASIC;
        else {
            diag_error(v->span, "invalid kind \"%s\" in [%s] (fpga or asic)", s, where);
            errs++;
        }
    }
    /* defaults by family */
    switch (t->family) {
    case BK_FAM_XILINX: t->constraints = EMIT_CONS_XDC; break;
    case BK_FAM_INTEL:
        t->constraints = EMIT_CONS_SDC;
        t->mem_init = SV_MEMINIT_MIF;
        break;
    case BK_FAM_ICE40:
    case BK_FAM_ECP5:
    case BK_FAM_GOWIN:
    case BK_FAM_ASIC: t->constraints = EMIT_CONS_SDC; break;
    case BK_FAM_GENERIC: break;
    }
    v = bk_extra(tt, "constraints");
    s = bk_list_value(v, "constraints", where, a, &errs);
    if (s && bk_parse_bits(s, cons_names, cons_bits, 3, &t->constraints, v->span, "constraint format") != 0)
        errs++;
    v = bk_extra(tt, "mem_init");
    s = bk_list_value(v, "mem_init", where, a, &errs);
    if (s && bk_parse_bits(s, mem_names, mem_bits, 3, &t->mem_init, v->span, "memory-init format") != 0)
        errs++;
    t->features = bk_list_value(bk_extra(tt, "features"), "features", where, a, &errs);
    t->package = bk_str_value(bk_extra(tt, "package"), "package", where, a, &errs);
    t->part = bk_str_value(bk_extra(tt, "part"), "part", where, a, &errs);
    t->family_name = bk_str_value(bk_extra(tt, "family"), "family", where, a, &errs);
    v = bk_extra(tt, "clock_hz");
    if (v) {
        if (v->kind != TOML_INT || v->num <= 0) {
            diag_error(v->span, "`clock_hz` in [%s] must be a positive integer", where);
            errs++;
        } else {
            t->clock_hz = (uint64_t)v->num;
        }
    }
    if (tt->backend && bk_tool_parse(tt->backend, &t->tool) != 0) {
        diag_error(tt->span, "unknown backend \"%s\" in [%s]", tt->backend, where);
        diag_help("the synthesis tool: yosys, vivado or quartus");
        errs++;
    }
    return errs;
}

/* ============================================================= helpers */

void bk_rewrite_paths(SvOutput *out, const char *const *from, const char *const *to, size_t n)
{
    size_t f, k;
    for (f = 0; f < out->nfiles; f++) {
        SvFile *file = &out->files[f];
        for (k = 0; k < n; k++) {
            StrBuf sb = {0};
            size_t flen;
            const char *p, *hit;
            if (!from[k] || !*from[k])
                continue;
            flen = strlen(from[k]);
            p = file->data;
            if (!strstr(p, from[k]))
                continue;
            while ((hit = strstr(p, from[k])) != NULL) {
                if (hit[flen] != '/') {
                    sb_append(&sb, p, (size_t)(hit - p) + flen);
                    p = hit + flen;
                    continue;
                }
                sb_append(&sb, p, (size_t)(hit - p));
                sb_puts(&sb, to[k]);
                p = hit + flen + 1;
            }
            sb_puts(&sb, p);
            free(file->data);
            file->len = sb.len;
            file->data = sb_take(&sb);
        }
    }
}

int bk_is_extern_file(const SvOutput *out, const char *name)
{
    size_t i;
    for (i = 0; i < out->nextern; i++)
        if (strcmp(os_path_basename(out->extern_files[i]), name) == 0)
            return 1;
    return 0;
}

void bk_hdl_files(const SvOutput *out, BkPathVec *files)
{
    size_t i;
    /* the package first (the modules import it), then extern sources, then
     * the emitted modules children-first */
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_PACKAGE)
            vec_push(*files, out->files[i].name);
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_MODULE && bk_is_extern_file(out, out->files[i].name))
            vec_push(*files, out->files[i].name);
    for (i = 0; i < out->nfiles; i++)
        if (out->files[i].kind == SV_FILE_MODULE && !bk_is_extern_file(out, out->files[i].name))
            vec_push(*files, out->files[i].name);
}


