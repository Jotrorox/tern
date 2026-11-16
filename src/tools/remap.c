/* remap.c — `tern remap`: rewrite SystemVerilog locations in tool logs
 * (synthesis, lint, simulation) to the Tern source they came from (spec
 * §13.3; docs/migration.md).
 *
 * The emitter writes a line marker `// tern: path/to/file.tern:42` before
 * each construct. remap scans every .sv/.v/.svh file below the build
 * directory, maps each SystemVerilog line to the nearest preceding marker,
 * and rewrites `file.sv:123`, `file.sv(123)`, `file.sv, line 123`,
 * `"file.sv", 123` and `file.sv Line: 123` in the log to
 * `file.tern:42 (file.sv:123)`. */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/diag.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../cmds.h"

typedef struct RemapMark {
    uint32_t sv_line;          /* line of the marker in the .sv file */
    const char *tern;          /* "path/to/file.tern:42" (the location text, without a column) */
} RemapMark;

typedef VEC(RemapMark) RemapMarkVec;

typedef struct RemapFile {
    const char *abs;           /* absolute, normalized */
    const char *rel;           /* relative to the build directory */
    const char *base;          /* basename */
    RemapMarkVec marks;        /* ascending sv_line */
} RemapFile;

typedef VEC(RemapFile) RemapFileVec;

typedef struct RemapMap {
    RemapFileVec files;
    size_t nmarks;
    Arena arena;
} RemapMap;

/* Parses the markers of one SystemVerilog text. */
static void remap_add_text(RemapMap *m, const char *abs, const char *rel, const char *text, size_t len)
{
    RemapFile f;
    size_t p = 0;
    uint32_t line = 1;
    memset(&f, 0, sizeof f);
    f.abs = arena_strdup(&m->arena, abs);
    f.rel = arena_strdup(&m->arena, rel);
    f.base = os_path_basename(f.abs);
    while (p < len) {
        size_t e = p;
        const char *hit;
        while (e < len && text[e] != '\n')
            e++;
        /* find "// tern:" in [p, e) */
        hit = NULL;
        {
            size_t q;
            for (q = p; q + 8 <= e; q++) {
                if (text[q] == '/' && memcmp(text + q, "// tern:", 8) == 0) {
                    hit = text + q + 8;
                    break;
                }
            }
        }
        if (hit) {
            const char *s = hit, *t, *end = text + e;
            while (s < end && (*s == ' ' || *s == '\t'))
                s++;
            t = s;
            while (t < end && *t != ' ' && *t != '\t' && *t != '\r')
                t++;
            /* t..s is "path:line[:col]" */
            if (t > s) {
                const char *d = t, *colon1 = NULL, *colon2 = NULL;
                while (d > s && tern_is_digit((unsigned char)d[-1]))
                    d--;
                if (d < t && d > s && d[-1] == ':') {
                    colon1 = d - 1;
                    d = colon1;
                    {
                        const char *d2 = d;
                        while (d2 > s && tern_is_digit((unsigned char)d2[-1]))
                            d2--;
                        if (d2 < d && d2 > s && d2[-1] == ':')
                            colon2 = d2 - 1;
                    }
                    {
                        RemapMark mk;
                        const char *loc_end = colon2 ? colon1 : t; /* drop a column */
                        mk.sv_line = line;
                        mk.tern = arena_strndup(&m->arena, s, (size_t)(loc_end - s));
                        vec_push(f.marks, mk);
                        m->nmarks++;
                    }
                }
            }
        }
        line++;
        p = e + 1;
    }
    vec_push(m->files, f);
}

static int remap_is_sv(const char *name)
{
    return tern_ends_with(name, ".sv") || tern_ends_with(name, ".v") || tern_ends_with(name, ".svh");
}

static void remap_scan(RemapMap *m, const char *dir, const char *root)
{
    OsDirList dl;
    size_t k;
    if (os_list_dir(dir, &dl) != 0)
        return;
    for (k = 0; k < dl.len; k++) {
        char *p = os_path_join(dir, dl.items[k].name);
        if (dl.items[k].is_dir) {
            if (!dl.items[k].is_link && dl.items[k].name[0] != '.')
                remap_scan(m, p, root);
        } else if (remap_is_sv(dl.items[k].name)) {
            char *data, *abs = os_path_abs(p), *rel = os_path_relative(abs, root);
            size_t len;
            if (os_read_file(p, &data, &len) == 0) {
                remap_add_text(m, abs, rel, data, len);
                free(data);
            }
            free(abs);
            free(rel);
        }
        free(p);
    }
    os_dir_list_free(&dl);
}

static void remap_free(RemapMap *m)
{
    size_t i;
    for (i = 0; i < m->files.len; i++)
        vec_free(m->files.data[i].marks);
    vec_free(m->files);
    arena_release(&m->arena);
    memset(m, 0, sizeof m[0]);
}

/* The file a log path refers to: exact, longest relative-path suffix, or a
 * unique basename. */
static const RemapFile *remap_find(const RemapMap *m, const char *path, size_t n)
{
    char buf[1024];
    size_t i, k, best_len = 0;
    const RemapFile *best = NULL, *by_base = NULL;
    int nbase = 0;
    const char *base;
    if (n >= sizeof buf)
        return NULL;
    for (k = 0; k < n; k++)
        buf[k] = path[k] == '\\' ? '/' : path[k];
    buf[n] = 0;
    base = os_path_basename(buf);
    for (i = 0; i < m->files.len; i++) {
        const RemapFile *f = &m->files.data[i];
        size_t rl = strlen(f->rel);
        if (strcmp(buf, f->abs) == 0)
            return f;
        if ((n == rl && strcmp(buf, f->rel) == 0) || (n > rl && buf[n - rl - 1] == '/' && strcmp(buf + n - rl, f->rel) == 0)) {
            if (rl > best_len) {
                best_len = rl;
                best = f;
            }
        }
        if (strcmp(base, f->base) == 0) {
            by_base = f;
            nbase++;
        }
    }
    if (best)
        return best;
    return nbase == 1 ? by_base : NULL;
}

static const char *remap_lookup(const RemapFile *f, uint32_t line)
{
    size_t lo = 0, hi = f->marks.len;
    /* last mark with sv_line <= line */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f->marks.data[mid].sv_line <= line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo ? f->marks.data[lo - 1].tern : NULL;
}

static int remap_path_char(int c)
{
    return c > ' ' && c != '"' && c != '\'' && c != '(' && c != ')' && c != '[' && c != ']' && c != '<' &&
           c != '>' && c != ',' && c != ';' && c != '|' && c != '`' && c != '{' && c != '}' && c != '=';
}

/* Reads decimal digits at s[*p]; returns 1 and the value. */
static int remap_num(const char *s, size_t len, size_t *p, uint32_t *v)
{
    size_t q = *p;
    uint64_t x = 0;
    while (q < len && tern_is_digit((unsigned char)s[q]) && x < 0xFFFFFFFFu) {
        x = x * 10 + (uint64_t)(s[q] - '0');
        q++;
    }
    if (q == *p || x > 0xFFFFFFFFu)
        return 0;
    *p = q;
    *v = (uint32_t)x;
    return 1;
}

static int remap_lit(const char *s, size_t len, size_t p, const char *lit)
{
    size_t n = strlen(lit);
    return p + n <= len && memcmp(s + p, lit, n) == 0;
}

static void remap_text(const RemapMap *m, const char *s, size_t len, StrBuf *out)
{
    size_t p = 0, copied = 0;
    while (p < len) {
        size_t extlen = 0;
        if (s[p] == '.') {
            if (remap_lit(s, len, p, ".svh"))
                extlen = 4;
            else if (remap_lit(s, len, p, ".sv"))
                extlen = 3;
            else if (remap_lit(s, len, p, ".v"))
                extlen = 2;
            if (extlen && p + extlen < len && tern_is_ident((unsigned char)s[p + extlen]))
                extlen = 0;
        }
        if (extlen) {
            size_t start = p, end = p + extlen, q;
            uint32_t line = 0;
            int ok = 0, quoted;
            while (start > copied && remap_path_char((unsigned char)s[start - 1]))
                start--;
            quoted = start > 0 && s[start - 1] == '"' && end < len && s[end] == '"';
            q = end + (quoted ? 1 : 0);
            {
                size_t r = q + 1;
                if (q < len && s[q] == ':' && remap_num(s, len, &r, &line)) {
                    ok = 1;
                    q = r;
                    /* :line:col — the column stays in the original text */
                    if (q + 1 < len && s[q] == ':' && tern_is_digit((unsigned char)s[q + 1])) {
                        uint32_t col;
                        size_t q2 = q + 1;
                        if (remap_num(s, len, &q2, &col))
                            q = q2;
                    }
                } else if (q < len && s[q] == '(' && remap_num(s, len, &r, &line)) {
                    if (r < len && s[r] == ')') {
                        q = r + 1;
                        ok = 1;
                    } else if (r < len && s[r] == ',') { /* file.sv(12,5) */
                        while (r < len && s[r] != ')' && s[r] != '\n')
                            r++;
                        if (r < len && s[r] == ')') {
                            q = r + 1;
                            ok = 1;
                        }
                    }
                } else {
                    static const char *const seps[] = {", line ", " line ", " Line: ", ", "};
                    size_t k;
                    for (k = 0; k < TERN_ARRAY_LEN(seps) && !ok; k++) {
                        if (k == 3 && !quoted)
                            break;
                        if (remap_lit(s, len, q, seps[k])) {
                            r = q + strlen(seps[k]);
                            if (remap_num(s, len, &r, &line)) {
                                ok = 1;
                                q = r;
                            }
                        }
                    }
                }
            }
            if (ok) {
                const RemapFile *f = remap_find(m, s + start, end - start);
                const char *loc = f ? remap_lookup(f, line) : NULL;
                if (loc) {
                    size_t ostart = quoted ? start - 1 : start;
                    sb_append(out, s + copied, ostart - copied);
                    sb_printf(out, "%s (", loc);
                    sb_append(out, s + ostart, q - ostart);
                    sb_putc(out, ')');
                    copied = q;
                    p = q;
                    continue;
                }
            }
            p = end;
            continue;
        }
        p++;
    }
    sb_append(out, s + copied, len - copied);
}

static int remap_read_stream(FILE *f, StrBuf *out)
{
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sb_append(out, buf, n);
    return ferror(f) ? -1 : 0;
}

int cmd_remap(int argc, char **argv)
{
    ArgParser ap;
    const char *out_path = NULL, *build_dir = NULL, *log_path = NULL;
    RemapMap m;
    StrBuf in = {0}, out = {0};
    char *root;
    int rc = TERN_EXIT_OK;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("remap");
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "output|o", &out_path) || args_opt(&ap, "build-dir|b", &build_dir)) {
            ;
        } else if (args_positional(&ap)) {
            if (strcmp(ap.arg, "-") != 0 && os_is_dir(ap.arg) && !build_dir)
                build_dir = ap.arg;
            else if (!log_path)
                log_path = ap.arg;
            else
                args_error(&ap, "unexpected argument `%s`", ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors)
        return TERN_EXIT_USAGE;
    if (!build_dir)
        build_dir = "build";
    if (!os_is_dir(build_dir)) {
        diag_error(span_none(), "build directory `%s` not found", build_dir);
        diag_help("run `tern build` first, or pass the directory: `tern remap <build-dir> <log>`");
        return TERN_EXIT_FAIL;
    }
    memset(&m, 0, sizeof m);
    root = os_path_abs(build_dir);
    remap_scan(&m, root, root);
    free(root);
    if (!m.files.len)
        diag_warning(span_none(), "no .sv or .v files below `%s`", build_dir);
    else if (!m.nmarks)
        diag_warning(span_none(), "no `// tern:` line markers in the files below `%s`", build_dir);
    if (log_path && strcmp(log_path, "-") != 0) {
        char *data;
        size_t len;
        if (os_read_file(log_path, &data, &len) != 0) {
            diag_error(span_none(), "cannot read `%s`", log_path);
            remap_free(&m);
            return TERN_EXIT_FAIL;
        }
        sb_append(&in, data, len);
        free(data);
    } else if (remap_read_stream(stdin, &in) != 0) {
        diag_error(span_none(), "cannot read the log from standard input");
        remap_free(&m);
        return TERN_EXIT_FAIL;
    }
    remap_text(&m, in.data ? in.data : "", in.len, &out);
    if (out_path) {
        if (os_write_file(out_path, out.data ? out.data : "", out.len) != 0) {
            diag_error(span_none(), "cannot write `%s`", out_path);
            rc = TERN_EXIT_FAIL;
        }
    } else {
        fwrite(out.data ? out.data : "", 1, out.len, stdout);
    }
    sb_free(&in);
    sb_free(&out);
    remap_free(&m);
    return rc;
}
