/* vcdread.c — a minimal Value Change Dump reader (IEEE 1364 §18) for the
 * counterexample traces SymbiYosys writes (and the simulator's own VCDs).
 *
 * Understands $scope/$upscope, $var (aliases sharing an identifier code
 * included), $timescale, #time, scalar (0!/1!/x!/z!) and vector (b1010 !)
 * changes; real values, $comment and the other sections are skipped. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/map.h"
#include "../base/vec.h"
#include "backend.h"

typedef struct VcdrLex {
    const char *p, *end;
} VcdrLex;

/* next whitespace-separated token; returns its length (0 at the end) */
static size_t vcdr_tok(VcdrLex *lx, const char **out)
{
    while (lx->p < lx->end && tern_is_space((unsigned char)*lx->p))
        lx->p++;
    *out = lx->p;
    while (lx->p < lx->end && !tern_is_space((unsigned char)*lx->p))
        lx->p++;
    return (size_t)(lx->p - *out);
}

static int vcdr_is(const char *t, size_t n, const char *kw)
{
    return strlen(kw) == n && memcmp(t, kw, n) == 0;
}

/* skips to the matching $end */
static void vcdr_skip_section(VcdrLex *lx)
{
    const char *t;
    size_t n;
    while ((n = vcdr_tok(lx, &t)) != 0)
        if (vcdr_is(t, n, "$end"))
            return;
}

typedef struct VcdrTmpChange {
    uint64_t time;
    uint32_t code;
    const char *value;
} VcdrTmpChange;

typedef VEC(VcdrVar) VcdrVarVec;
typedef VEC(VcdrTmpChange) VcdrChangeVec;
typedef VEC(uint32_t) VcdrU32Vec;
typedef VEC(const char *) VcdrStrVec;

static const char *vcdr_norm_value(Arena *a, const char *v, size_t n, uint32_t width)
{
    char *r;
    size_t i;
    char fill;
    if (width == 0)
        width = 1;
    r = (char *)arena_alloc(a, width + 1);
    fill = n && (tern_to_lower((unsigned char)v[0]) == 'x' || tern_to_lower((unsigned char)v[0]) == 'z')
               ? (char)tern_to_lower((unsigned char)v[0])
               : '0';
    if (n >= width) {
        for (i = 0; i < width; i++)
            r[i] = (char)tern_to_lower((unsigned char)v[n - width + i]);
    } else {
        for (i = 0; i < width - n; i++)
            r[i] = fill;
        for (i = 0; i < n; i++)
            r[width - n + i] = (char)tern_to_lower((unsigned char)v[i]);
    }
    r[width] = 0;
    return r;
}

static char *vcdr_errf(const char *msg, const char *tok, size_t n)
{
    StrBuf sb = {0};
    sb_printf(&sb, "%s `%.*s`", msg, (int)(n > 40 ? 40 : n), tok);
    return sb_take(&sb);
}

int vcdr_parse(const char *text, size_t len, VcdrTrace *t, char **err)
{
    VcdrLex lx;
    VcdrVarVec vars = {0};
    VcdrChangeVec changes = {0};
    VcdrU32Vec widths = {0};
    VcdrStrVec scopes = {0};
    Map codes = {0};
    uint64_t now = 0;
    int in_defs = 1, rc = 0;
    const char *tk;
    size_t n, i;
    memset(t, 0, sizeof *t);
    *err = NULL;
    lx.p = text;
    lx.end = text + len;
    while ((n = vcdr_tok(&lx, &tk)) != 0) {
        if (tk[0] == '$') {
            if (vcdr_is(tk, n, "$scope")) {
                const char *k, *nm;
                size_t kn = vcdr_tok(&lx, &k), nn = vcdr_tok(&lx, &nm);
                TERN_UNUSED(kn);
                vec_push(scopes, arena_strndup(&t->arena, nm, nn));
                vcdr_skip_section(&lx);
            } else if (vcdr_is(tk, n, "$upscope")) {
                if (scopes.len)
                    scopes.len--;
                vcdr_skip_section(&lx);
            } else if (vcdr_is(tk, n, "$var")) {
                const char *ty, *w, *id, *ref, *x;
                size_t tyn, wn, idn, refn, xn;
                VcdrVar v;
                uint64_t width = 0, *slot;
                int inserted = 0;
                StrBuf sc = {0};
                tyn = vcdr_tok(&lx, &ty);
                wn = vcdr_tok(&lx, &w);
                idn = vcdr_tok(&lx, &id);
                refn = vcdr_tok(&lx, &ref);
                TERN_UNUSED(tyn);
                if (!wn || !idn || !refn) {
                    *err = xstrdup("truncated $var");
                    rc = -1;
                    break;
                }
                for (i = 0; i < wn; i++) {
                    if (!tern_is_digit((unsigned char)w[i])) {
                        *err = vcdr_errf("bad $var width", w, wn);
                        rc = -1;
                        break;
                    }
                    width = width * 10 + (uint64_t)(w[i] - '0');
                }
                if (rc)
                    break;
                /* a bit range written as a separate token is dropped with $end */
                while ((xn = vcdr_tok(&lx, &x)) != 0 && !vcdr_is(x, xn, "$end"))
                    ;
                memset(&v, 0, sizeof v);
                v.id = intern(id, idn);
                for (i = 0; i < refn; i++)
                    if (ref[i] == '[' && i > 0)
                        break;
                v.name = arena_strndup(&t->arena, ref, i);
                for (i = 0; i < scopes.len; i++) {
                    if (i)
                        sb_putc(&sc, '.');
                    sb_puts(&sc, scopes.data[i]);
                }
                v.scope = arena_strdup(&t->arena, sb_cstr(&sc));
                if (sc.len)
                    sb_putc(&sc, '.');
                sb_puts(&sc, v.name);
                v.full = arena_strdup(&t->arena, sb_cstr(&sc));
                sb_free(&sc);
                v.width = width ? (uint32_t)width : 1;
                slot = map_ref(&codes, MAP_PTR(v.id), &inserted);
                if (inserted) {
                    *slot = widths.len;
                    vec_push(widths, v.width);
                }
                v.code = (uint32_t)*slot;
                vec_push(vars, v);
            } else if (vcdr_is(tk, n, "$timescale")) {
                StrBuf sb = {0};
                const char *x;
                size_t xn;
                while ((xn = vcdr_tok(&lx, &x)) != 0 && !vcdr_is(x, xn, "$end"))
                    sb_append(&sb, x, xn);
                t->timescale = arena_strdup(&t->arena, sb_cstr(&sb));
                sb_free(&sb);
            } else if (vcdr_is(tk, n, "$enddefinitions")) {
                in_defs = 0;
                vcdr_skip_section(&lx);
            } else if (vcdr_is(tk, n, "$dumpvars") || vcdr_is(tk, n, "$dumpall") || vcdr_is(tk, n, "$dumpon") ||
                       vcdr_is(tk, n, "$dumpoff") || vcdr_is(tk, n, "$end")) {
                /* value sections: their contents are ordinary changes */
            } else {
                vcdr_skip_section(&lx); /* $date, $version, $comment, ... */
            }
            continue;
        }
        if (in_defs) {
            *err = vcdr_errf("unexpected token in the header:", tk, n);
            rc = -1;
            break;
        }
        if (tk[0] == '#') {
            uint64_t v = 0;
            for (i = 1; i < n; i++) {
                if (!tern_is_digit((unsigned char)tk[i])) {
                    *err = vcdr_errf("bad timestamp", tk, n);
                    rc = -1;
                    break;
                }
                v = v * 10 + (uint64_t)(tk[i] - '0');
            }
            if (rc)
                break;
            now = v;
            continue;
        }
        {
            const char *val = tk, *id;
            size_t vn, idn;
            uint64_t code;
            VcdrTmpChange c;
            char k = (char)tern_to_lower((unsigned char)tk[0]);
            if (k == 'b' || k == 'r') {
                val = tk + 1;
                vn = n - 1;
                idn = vcdr_tok(&lx, &id);
                if (!idn) {
                    *err = xstrdup("value without identifier at the end of the file");
                    rc = -1;
                    break;
                }
                if (k == 'r')
                    continue; /* real values are not used */
            } else if (k == '0' || k == '1' || k == 'x' || k == 'z') {
                vn = 1;
                id = tk + 1;
                idn = n - 1;
            } else {
                *err = vcdr_errf("unexpected token", tk, n);
                rc = -1;
                break;
            }
            if (!map_get(&codes, MAP_PTR(intern(id, idn)), &code))
                continue; /* undeclared identifier: ignore */
            c.time = now;
            c.code = (uint32_t)code;
            c.value = vcdr_norm_value(&t->arena, val, vn, widths.data[code]);
            vec_push(changes, c);
        }
    }
    if (rc == 0) {
        size_t k;
        VcdrU32Vec counts = {0};
        t->nvars = vars.len;
        t->vars = ANEWN(&t->arena, VcdrVar, vars.len ? vars.len : 1);
        if (vars.len)
            memcpy(t->vars, vars.data, vars.len * sizeof(VcdrVar));
        t->nseries = widths.len;
        t->series = ANEWN(&t->arena, VcdrSeries, widths.len ? widths.len : 1);
        vec_resize(counts, widths.len);
        for (i = 0; i < changes.len; i++)
            counts.data[changes.data[i].code]++;
        for (k = 0; k < widths.len; k++) {
            char *x = (char *)arena_alloc(&t->arena, widths.data[k] + 1);
            memset(x, 'x', widths.data[k]);
            x[widths.data[k]] = 0;
            t->series[k].width = widths.data[k];
            t->series[k].unknown = x;
            t->series[k].times = ANEWN(&t->arena, uint64_t, counts.data[k] ? counts.data[k] : 1);
            t->series[k].values = ANEWN(&t->arena, const char *, counts.data[k] ? counts.data[k] : 1);
        }
        for (i = 0; i < changes.len; i++) {
            VcdrSeries *s = &t->series[changes.data[i].code];
            /* a second change at the same time replaces the first */
            if (s->n && s->times[s->n - 1] == changes.data[i].time) {
                s->values[s->n - 1] = changes.data[i].value;
                continue;
            }
            if (s->n && s->times[s->n - 1] > changes.data[i].time)
                continue; /* time going backwards: ignore */
            s->times[s->n] = changes.data[i].time;
            s->values[s->n] = changes.data[i].value;
            s->n++;
        }
        t->times = ANEWN(&t->arena, uint64_t, changes.len ? changes.len : 1);
        for (i = 0; i < changes.len; i++)
            if (!t->ntimes || t->times[t->ntimes - 1] < changes.data[i].time)
                t->times[t->ntimes++] = changes.data[i].time;
        vec_free(counts);
    } else {
        arena_release(&t->arena);
        memset(t, 0, sizeof *t);
    }
    vec_free(vars);
    vec_free(changes);
    vec_free(widths);
    vec_free(scopes);
    map_free(&codes);
    return rc;
}

void vcdr_free(VcdrTrace *t)
{
    arena_release(&t->arena);
    memset(t, 0, sizeof *t);
}

int vcdr_find(const VcdrTrace *t, const char *name)
{
    size_t i, nl = strlen(name);
    int best = -1;
    size_t best_depth = (size_t)-1;
    for (i = 0; i < t->nvars; i++)
        if (strcmp(t->vars[i].full, name) == 0)
            return (int)i;
    for (i = 0; i < t->nvars; i++) {
        const VcdrVar *v = &t->vars[i];
        size_t fl = strlen(v->full), depth = 0;
        const char *p;
        if (fl <= nl || v->full[fl - nl - 1] != '.' || strcmp(v->full + fl - nl, name) != 0)
            continue;
        for (p = v->full; *p; p++)
            depth += *p == '.';
        if (depth < best_depth) {
            best_depth = depth;
            best = (int)i;
        }
    }
    return best;
}

const char *vcdr_value_at(const VcdrTrace *t, int var, uint64_t time)
{
    const VcdrSeries *s;
    size_t lo = 0, hi;
    if (var < 0 || (size_t)var >= t->nvars)
        return "";
    s = &t->series[t->vars[var].code];
    if (!s->n || s->times[0] > time)
        return s->unknown;
    hi = s->n;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (s->times[mid] <= time)
            lo = mid;
        else
            hi = mid;
    }
    return s->values[lo];
}

size_t vcdr_rising_edges(const VcdrTrace *t, int var, uint64_t **out)
{
    const VcdrSeries *s;
    size_t i, n = 0;
    char prev = 'x';
    *out = NULL;
    if (var < 0 || (size_t)var >= t->nvars)
        return 0;
    s = &t->series[t->vars[var].code];
    *out = (uint64_t *)xcalloc(s->n ? s->n : 1, sizeof(uint64_t));
    for (i = 0; i < s->n; i++) {
        char c = s->values[i][s->width - 1];
        if (c == '1' && prev != '1')
            (*out)[n++] = s->times[i];
        prev = c;
    }
    return n;
}
