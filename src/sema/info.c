/* info.c — per-node semantic side table for tooling (hover, go-to-definition;
 * see sema.h). Records are keyed by span (file, offset, length): the
 * elaborator records each name use and declaration it binds; generic
 * modules elaborated several times keep the first instantiation's facts. */
#include "sema.h"
#include "../base/diag.h"

static uint64_t info_key(Span s)
{
    return hash_combine(hash_combine(hash_u64(s.file), hash_u64(s.off)), hash_u64(s.len)) | 1;
}

static SemInfo *info_find(Sema *S, Span s)
{
    uint64_t v;
    if (!s.file)
        return NULL;
    if (map_get(&S->info_index, info_key(s), &v)) {
        SemInfo *r = &S->info.data[v];
        if (r->span.file == s.file && r->span.off == s.off && r->span.len == s.len)
            return r;
    }
    return NULL;
}

void sema_info_record(Sema *S, const SemInfo *info)
{
    SemInfo *r;
    if (!S->record_info || !info->span.file)
        return;
    if (S->info_sorted) {
        /* rebuild the index after sorting */
        size_t i;
        map_clear(&S->info_index);
        for (i = 0; i < S->info.len; i++)
            map_put(&S->info_index, info_key(S->info.data[i].span), i);
        S->info_sorted = 0;
    }
    r = info_find(S, info->span);
    if (!r) {
        vec_push(S->info, *info);
        map_put(&S->info_index, info_key(info->span), S->info.len - 1);
        return;
    }
    if (!r->def.file)
        r->def = info->def;
    if (r->kind == SEM_NONE || r->kind == SEM_EXPR)
        r->kind = info->kind != SEM_NONE ? info->kind : r->kind;
    if (!r->name)
        r->name = info->name;
    if (!r->type)
        r->type = info->type;
    if (!r->width)
        r->width = info->width;
    if (!r->domain)
        r->domain = info->domain;
    if (!r->driver) {
        r->driver = info->driver;
        r->driver_span = info->driver_span;
    }
    if (!r->value)
        r->value = info->value;
}

void sema_info_set_driver(Sema *S, Span def, const char *driver, Span at)
{
    SemInfo *r = info_find(S, def);
    if (r && !r->driver) {
        r->driver = driver;
        r->driver_span = at;
    }
}

static int info_cmp(const void *a, const void *b)
{
    const SemInfo *x = (const SemInfo *)a, *y = (const SemInfo *)b;
    if (x->span.file != y->span.file)
        return x->span.file < y->span.file ? -1 : 1;
    if (x->span.off != y->span.off)
        return x->span.off < y->span.off ? -1 : 1;
    if (x->span.len != y->span.len)
        return x->span.len > y->span.len ? -1 : 1;
    return 0;
}

static void info_sort(Sema *S)
{
    size_t i;
    if (S->info_sorted)
        return;
    if (S->info.len)
        qsort(S->info.data, S->info.len, sizeof(SemInfo), info_cmp);
    map_clear(&S->info_index);
    for (i = 0; i < S->info.len; i++)
        map_put(&S->info_index, info_key(S->info.data[i].span), i);
    S->info_sorted = 1;
}

const SemInfo *sema_info_at(Sema *S, uint32_t file, uint32_t off)
{
    size_t i;
    const SemInfo *best = NULL;
    info_sort(S);
    for (i = 0; i < S->info.len; i++) {
        const SemInfo *r = &S->info.data[i];
        if (r->span.file != file)
            continue;
        if (r->span.off > off)
            break;
        if (off < r->span.off + (r->span.len ? r->span.len : 1)) {
            if (!best || r->span.len <= best->span.len)
                best = r;
        }
    }
    return best;
}

const SemInfo *sema_info_decl(Sema *S, Span def)
{
    if (S->info_sorted) {
        size_t i;
        map_clear(&S->info_index);
        for (i = 0; i < S->info.len; i++)
            map_put(&S->info_index, info_key(S->info.data[i].span), i);
        S->info_sorted = 0;
    }
    return info_find(S, def);
}

size_t sema_info_all(Sema *S, const SemInfo **out)
{
    info_sort(S);
    *out = S->info.data;
    return S->info.len;
}

const char *sema_kind_name(SemKind k)
{
    switch (k) {
    case SEM_PORT_IN: return "input port";
    case SEM_PORT_OUT: return "output port";
    case SEM_WIRE: return "let";
    case SEM_VAR: return "var";
    case SEM_REG: return "reg";
    case SEM_MEM: return "mem";
    case SEM_CONST: return "const";
    case SEM_GENERIC: return "generic";
    case SEM_PARAM: return "parameter";
    case SEM_LOCAL: return "local";
    case SEM_FN: return "fn";
    case SEM_MODULE: return "module";
    case SEM_TYPE: return "type";
    case SEM_INSTANCE: return "instance";
    case SEM_DOMAIN: return "domain";
    case SEM_PACKAGE: return "package";
    case SEM_FIELD: return "field";
    case SEM_VARIANT: return "variant";
    case SEM_EXPR: return "expression";
    default: return "";
    }
}

void sema_info_hover(Sema *S, const SemInfo *info, StrBuf *out)
{
    const SemInfo *decl = info->def.file ? sema_info_decl(S, info->def) : NULL;
    const char *driver = info->driver ? info->driver : decl ? decl->driver : NULL;
    Span dspan = info->driver ? info->driver_span : decl ? decl->driver_span : span_none();
    const char *dom = info->domain ? info->domain : decl ? decl->domain : NULL;
    sb_puts(out, "`");
    if (info->name)
        sb_puts(out, info->name);
    if (info->type) {
        sb_puts(out, info->name ? ": " : "");
        sb_puts(out, info->type);
    }
    if (dom)
        sb_printf(out, " @ %s", dom);
    if (info->value)
        sb_printf(out, " = %s", info->value);
    sb_puts(out, "`");
    if (info->kind != SEM_NONE)
        sb_printf(out, " — %s", sema_kind_name(info->kind));
    if (driver) {
        sb_printf(out, ", driven by %s", driver);
        if (dspan.file) {
            const SourceFile *sf = src_get(dspan.file);
            if (sf)
                sb_printf(out, " at %s:%u", sf->path, (unsigned)dspan.line);
        }
    }
}
