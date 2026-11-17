/* lsp.c — the language server: lifecycle, documents, analyses,
 * diagnostics, formatting and request dispatch (see lsp.h). The
 * navigation features live in lsp_nav.c. */
#include "lsp_impl.h"
#include "../base/args.h"
#include "../base/utf8.h"
#include "../os/os.h"
#include "../check/check.h"
#include "../tools/tools.h"
#include "../cmds.h"

/* ------------------------------------------------------------- logging --- */

void lsp_log(LspServer *srv, const char *fmt, ...)
{
    va_list ap;
    if (!srv->opt.log)
        return;
    va_start(ap, fmt);
    vfprintf(srv->opt.log, fmt, ap);
    va_end(ap);
    fputc('\n', srv->opt.log);
    fflush(srv->opt.log);
}

/* ---------------------------------------------------------------- URIs --- */

static int lsp_hexval(int c)
{
    return tern_is_xdigit(c) ? tern_hex_value(c) : -1;
}

char *lsp_uri_to_path(const char *uri)
{
    StrBuf sb = {0};
    const char *p;
    char *norm;
    if (!uri || strncmp(uri, "file://", 7) != 0)
        return NULL;
    p = uri + 7;
    if (*p != '/') {
        /* file://host/share/path: a UNC path */
        const char *slash = strchr(p, '/');
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (!(hl == 9 && strncmp(p, "localhost", 9) == 0)) {
            sb_puts(&sb, "//");
            sb_append(&sb, p, hl);
        }
        p += hl;
    }
    for (; *p && *p != '?' && *p != '#'; p++) {
        if (*p == '%' && lsp_hexval((unsigned char)p[1]) >= 0 && lsp_hexval((unsigned char)p[2]) >= 0) {
            sb_putc(&sb, (char)(lsp_hexval((unsigned char)p[1]) * 16 + lsp_hexval((unsigned char)p[2])));
            p += 2;
        } else {
            sb_putc(&sb, *p);
        }
    }
    /* /c:/x -> C:/x (drive letters as the OS spells them) */
    if (sb.len >= 3 && sb.data[0] == '/' && tern_is_alpha((unsigned char)sb.data[1]) && sb.data[2] == ':')
        memmove(sb.data, sb.data + 1, sb.len--);
    if (sb.len >= 2 && tern_is_alpha((unsigned char)sb.data[0]) && sb.data[1] == ':')
        sb.data[0] = (char)tern_to_upper((unsigned char)sb.data[0]);
    norm = os_path_normalize(sb_cstr(&sb));
    sb_free(&sb);
    return norm;
}

char *lsp_path_to_uri(const char *path)
{
    StrBuf sb = {0};
    const char *p;
    char *norm = os_path_normalize(path);
    sb_puts(&sb, "file://");
    if (tern_is_alpha((unsigned char)norm[0]) && norm[1] == ':')
        sb_putc(&sb, '/');
    for (p = norm; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (tern_is_alnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~' ||
            (c == ':' && p == norm + 1))
            sb_putc(&sb, (char)c);
        else
            sb_printf(&sb, "%%%02X", (unsigned)c);
    }
    free(norm);
    return sb_take(&sb);
}

/* ----------------------------------------------------------- positions --- */

int lsp_span_valid(Span s)
{
    return s.file != 0 && src_get(s.file) != NULL;
}

const char *lsp_span_text(Span s, size_t *len)
{
    const SourceFile *sf = src_get(s.file);
    if (!sf || s.off > sf->len) {
        *len = 0;
        return "";
    }
    *len = s.off + s.len <= sf->len ? s.len : sf->len - s.off;
    return sf->text + s.off;
}

uint32_t lsp_offset_of(const char *text, size_t len, int64_t line, int64_t character)
{
    size_t i = 0, end;
    int64_t l = 0;
    if (line < 0)
        return 0;
    while (l < line) {
        while (i < len && text[i] != '\n')
            i++;
        if (i >= len)
            return (uint32_t)len;
        i++;
        l++;
    }
    end = i;
    while (end < len && text[end] != '\n')
        end++;
    if (end > i && text[end - 1] == '\r')
        end--;
    if (character < 0)
        character = 0;
    return (uint32_t)(i + utf8_offset_of_utf16(text + i, end - i, (size_t)character));
}

void lsp_pos_of(uint32_t src, uint32_t off, uint32_t *line, uint32_t *character)
{
    const SourceFile *sf = src_get(src);
    uint32_t lo, hi;
    if (!sf || !sf->nlines) {
        *line = *character = 0;
        return;
    }
    if (off > sf->len)
        off = (uint32_t)sf->len;
    lo = 0;
    hi = sf->nlines;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (sf->lines[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    *line = lo;
    *character = (uint32_t)utf8_utf16_len(sf->text + sf->lines[lo], off - sf->lines[lo]);
}

static void lsp_w_pos(JsonWriter *w, uint32_t src, uint32_t off)
{
    uint32_t l, c;
    lsp_pos_of(src, off, &l, &c);
    jw_object_begin(w);
    jw_kv_uint(w, "line", l);
    jw_kv_uint(w, "character", c);
    jw_object_end(w);
}

void lsp_w_range(JsonWriter *w, Span s)
{
    jw_object_begin(w);
    jw_key(w, "start");
    lsp_w_pos(w, s.file, s.off);
    jw_key(w, "end");
    lsp_w_pos(w, s.file, s.off + s.len);
    jw_object_end(w);
}

LspDoc *lsp_find_doc(LspServer *srv, const char *path)
{
    size_t i;
    for (i = 0; i < srv->docs.len; i++)
        if (strcmp(srv->docs.data[i]->path, path) == 0)
            return srv->docs.data[i];
    return NULL;
}

const char *lsp_src_path(LspServer *srv, uint32_t src)
{
    uint64_t v;
    const SourceFile *sf;
    char *abs = NULL;
    if (!src)
        return NULL;
    if (map_get(&srv->srcpath, src, &v)) {
        const char *p = (const char *)(uintptr_t)v;
        return p[0] ? p : NULL;
    }
    sf = src_get(src);
    if (!sf)
        return NULL;
    if (os_path_is_abs(sf->path)) {
        abs = os_path_normalize(sf->path);
    } else {
        abs = os_path_abs(sf->path);
        if (!lsp_find_doc(srv, abs) && !os_is_file(abs) && srv->root) {
            /* embedded std ("std/fifo/fifo.tern"): a copy in the workspace */
            char *alt = os_path_join(srv->root, sf->path);
            char *n = os_path_normalize(alt);
            free(alt);
            free(abs);
            abs = n;
        }
    }
    if (!lsp_find_doc(srv, abs) && !os_is_file(abs)) {
        free(abs);
        abs = xstrdup("");
    }
    map_put(&srv->srcpath, src, MAP_PTR(abs));
    return abs[0] ? abs : NULL;
}

static void lsp_srcpath_clear(LspServer *srv)
{
    size_t it = 0;
    uint64_t k, v;
    while (map_next(&srv->srcpath, &it, &k, &v))
        free((void *)(uintptr_t)v);
    map_clear(&srv->srcpath);
}

static char *lsp_uri_for_path(LspServer *srv, const char *path)
{
    LspDoc *d = lsp_find_doc(srv, path);
    return d ? xstrdup(d->uri) : lsp_path_to_uri(path);
}

int lsp_w_location(LspServer *srv, JsonWriter *w, Span s)
{
    const char *path;
    char *uri;
    if (!lsp_span_valid(s) || !(path = lsp_src_path(srv, s.file)))
        return 0;
    uri = lsp_uri_for_path(srv, path);
    jw_object_begin(w);
    jw_kv_str(w, "uri", uri);
    jw_key(w, "range");
    lsp_w_range(w, s);
    jw_object_end(w);
    free(uri);
    return 1;
}

/* ------------------------------------------------------------ analyses --- */

static void lsp_diags_clear(LspDiagVec *v)
{
    size_t i;
    uint32_t k;
    for (i = 0; i < v->len; i++) {
        free(v->data[i].msg);
        for (k = 0; k < v->data[i].nnotes; k++)
            free(v->data[i].notes[k].msg);
        free(v->data[i].notes);
    }
    vec_clear(*v);
}

static void lsp_strvec_free(LspStrVec *v)
{
    size_t i;
    for (i = 0; i < v->len; i++)
        free(v->data[i]);
    vec_free(*v);
}

static void lsp_group_release(LspGroup *g)
{
    size_t i;
    driver_free(g->dr);
    g->dr = NULL;
    arena_release(&g->ir);
    g->design = NULL;
    lsp_diags_clear(&g->diags);
    for (i = 0; i < g->files.len; i++)
        free(g->files.data[i].path);
    vec_clear(g->files);
    g->analyzed = 0;
}

static void lsp_group_free(LspGroup *g)
{
    lsp_group_release(g);
    vec_free(g->diags);
    vec_free(g->files);
    lsp_strvec_free(&g->published);
    free(g->root);
    free(g->file);
    free(g);
}

LspFile *lsp_group_file(LspGroup *g, const char *path)
{
    size_t i;
    for (i = 0; i < g->files.len; i++)
        if (g->files.data[i].path && strcmp(g->files.data[i].path, path) == 0)
            return &g->files.data[i];
    return NULL;
}

LspFile *lsp_group_file_by_src(LspGroup *g, uint32_t src)
{
    size_t i;
    for (i = 0; i < g->files.len; i++)
        if (g->files.data[i].src == src)
            return &g->files.data[i];
    return NULL;
}

static void lsp_capture_sink(const Diagnostic *d, void *user)
{
    LspServer *srv = (LspServer *)user;
    LspDiag ld;
    StrBuf msg = {0};
    VEC(LspNote) notes = {0};
    uint32_t i;
    if (!srv->capture)
        return;
    memset(&ld, 0, sizeof ld);
    ld.level = d->level;
    ld.span = d->span;
    sb_puts(&msg, d->msg ? d->msg : "");
    for (i = 0; i < d->nsubs; i++) {
        const DiagSub *s = &d->subs[i];
        if (s->level == DIAG_NOTE && s->span.file) {
            LspNote n;
            n.span = s->span;
            n.msg = xstrdup(s->msg ? s->msg : "");
            vec_push(notes, n);
        } else {
            sb_printf(&msg, "\n%s: %s", s->level == DIAG_HELP ? "help" : "note", s->msg ? s->msg : "");
        }
    }
    if (d->path)
        sb_printf(&msg, "\nin: %s", d->path);
    ld.msg = sb_take(&msg);
    ld.notes = notes.data;
    ld.nnotes = (uint32_t)notes.len;
    vec_push(*srv->capture, ld);
}

static void lsp_quiet_sink(const Diagnostic *d, void *user)
{
    LspServer *srv = (LspServer *)user;
    if (srv->opt.log && d->msg)
        lsp_log(srv, "diagnostic outside an analysis: %s", d->msg);
}

static const char *lsp_overlay(void *user, const char *path, size_t *len)
{
    LspServer *srv = (LspServer *)user;
    char *abs;
    LspDoc *d;
    if (!srv->docs.len)
        return NULL;
    abs = os_path_abs(path);
    d = lsp_find_doc(srv, abs);
    free(abs);
    if (!d)
        return NULL;
    *len = d->len;
    return d->text;
}

static void lsp_analyze(LspServer *srv, LspGroup *g)
{
    DriverOptions o;
    CheckOptions co;
    const char *paths[1];
    char *dir = NULL;
    uint64_t t0 = os_time_ns();
    size_t i;
    lsp_group_release(g);
    memset(&o, 0, sizeof o);
    memset(&co, 0, sizeof co);
    if (g->file) {
        paths[0] = g->file;
        o.paths = paths;
        o.npaths = 1;
    }
    if (g->root) {
        o.root = g->root;
    } else {
        dir = os_path_dirname(g->file);
        o.root = dir;
    }
    o.include_tests = 1;
    o.cmd = "lsp";
    o.overlay = lsp_overlay;
    o.overlay_user = srv;
    diag_flush();
    diag_reset();
    srv->capture = &g->diags;
    diag_set_sink(lsp_capture_sink, srv);
    g->dr = driver_open(&o);
    if (g->dr && !driver_parse_errors(g->dr)) {
        g->design = driver_elaborate_all(g->dr, NULL, &g->ir);
        if (diag_error_count() == 0)
            check_design(g->design, &co);
    }
    diag_flush();
    diag_set_sink(lsp_quiet_sink, srv);
    srv->capture = NULL;
    diag_reset();
    free(dir);
    if (g->dr) {
        Sema *S = driver_sema(g->dr);
        for (i = 0; i < S->files.len; i++) {
            LspFile lf;
            const char *p;
            lf.fu = S->files.data[i];
            lf.src = lf.fu->ast->src;
            p = lsp_src_path(srv, lf.src);
            lf.path = p ? xstrdup(p) : NULL;
            vec_push(g->files, lf);
        }
    }
    g->dirty = 0;
    g->analyzed = 1;
    lsp_log(srv, "analysed %s: %u files, %u diagnostics, %.1f ms", g->file ? g->file : g->root,
            (unsigned)g->files.len, (unsigned)g->diags.len, (double)(os_time_ns() - t0) / 1e6);
}

/* ---------------------------------------------------------- diagnostics --- */

static int lsp_strvec_has(const LspStrVec *v, const char *s)
{
    size_t i;
    for (i = 0; i < v->len; i++)
        if (strcmp(v->data[i], s) == 0)
            return 1;
    return 0;
}

/* Diagnostics without a location go to the group's first open document. */
static const char *lsp_group_primary(LspServer *srv, LspGroup *g)
{
    size_t i;
    if (g->file)
        return g->file;
    for (i = 0; i < srv->docs.len; i++)
        if (srv->docs.data[i]->group == g)
            return srv->docs.data[i]->path;
    return NULL;
}

static void lsp_w_diag_range(JsonWriter *w, LspServer *srv, Span s, const char *path)
{
    if (s.file && lsp_span_valid(s)) {
        lsp_w_range(w, s);
        return;
    }
    TERN_UNUSED(srv);
    TERN_UNUSED(path);
    jw_raw(w, "{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}");
}

static void lsp_publish_file(LspServer *srv, LspGroup *g, const char *path)
{
    StrBuf sb = {0};
    JsonWriter w;
    char *uri = lsp_uri_for_path(srv, path);
    LspDoc *doc = lsp_find_doc(srv, path);
    const char *primary = lsp_group_primary(srv, g);
    size_t i;
    uint32_t k;
    jw_init(&w, &sb, 0);
    jw_object_begin(&w);
    jw_kv_str(&w, "uri", uri);
    if (doc)
        jw_kv_int(&w, "version", doc->version);
    jw_key(&w, "diagnostics");
    jw_array_begin(&w);
    for (i = 0; i < g->diags.len; i++) {
        const LspDiag *d = &g->diags.data[i];
        const char *dp = d->span.file ? lsp_src_path(srv, d->span.file) : primary;
        if (!dp || strcmp(dp, path) != 0)
            continue;
        jw_object_begin(&w);
        jw_key(&w, "range");
        lsp_w_diag_range(&w, srv, d->span, path);
        jw_kv_int(&w, "severity", d->level == DIAG_ERROR ? 1 : d->level == DIAG_WARNING ? 2 : 3);
        jw_kv_str(&w, "source", "tern");
        jw_kv_str(&w, "message", d->msg);
        if (d->nnotes) {
            jw_key(&w, "relatedInformation");
            jw_array_begin(&w);
            for (k = 0; k < d->nnotes; k++) {
                StrBuf loc = {0};
                JsonWriter lw;
                jw_init(&lw, &loc, 0);
                if (lsp_w_location(srv, &lw, d->notes[k].span)) {
                    jw_object_begin(&w);
                    jw_key(&w, "location");
                    jw_raw(&w, sb_cstr(&loc));
                    jw_kv_str(&w, "message", d->notes[k].msg);
                    jw_object_end(&w);
                }
                sb_free(&loc);
            }
            jw_array_end(&w);
        }
        jw_object_end(&w);
    }
    jw_array_end(&w);
    jw_object_end(&w);
    rpc_send_notification(srv->io, "textDocument/publishDiagnostics", sb_cstr(&sb));
    sb_free(&sb);
    free(uri);
}

/* Publishes every file of the group with diagnostics and clears the files
 * that had some last time but no longer do. */
static void lsp_publish(LspServer *srv, LspGroup *g)
{
    LspStrVec now = {0};
    const char *primary = lsp_group_primary(srv, g);
    size_t i;
    for (i = 0; i < g->diags.len; i++) {
        const char *p = g->diags.data[i].span.file ? lsp_src_path(srv, g->diags.data[i].span.file) : primary;
        if (p && !lsp_strvec_has(&now, p))
            vec_push(now, xstrdup(p));
    }
    /* open documents of the group always get a (possibly empty) list */
    for (i = 0; i < srv->docs.len; i++)
        if (srv->docs.data[i]->group == g && !lsp_strvec_has(&now, srv->docs.data[i]->path))
            vec_push(now, xstrdup(srv->docs.data[i]->path));
    for (i = 0; i < now.len; i++)
        lsp_publish_file(srv, g, now.data[i]);
    for (i = 0; i < g->published.len; i++)
        if (!lsp_strvec_has(&now, g->published.data[i])) {
            StrBuf sb = {0};
            JsonWriter w;
            char *uri = lsp_uri_for_path(srv, g->published.data[i]);
            jw_init(&w, &sb, 0);
            jw_object_begin(&w);
            jw_kv_str(&w, "uri", uri);
            jw_key(&w, "diagnostics");
            jw_raw(&w, "[]");
            jw_object_end(&w);
            rpc_send_notification(srv->io, "textDocument/publishDiagnostics", sb_cstr(&sb));
            sb_free(&sb);
            free(uri);
        }
    lsp_strvec_free(&g->published);
    g->published = now;
}

static void lsp_clear_published(LspServer *srv, LspGroup *g)
{
    lsp_diags_clear(&g->diags);
    lsp_publish(srv, g);
}

static void lsp_ensure_analyzed(LspServer *srv, LspGroup *g)
{
    if (g && (g->dirty || !g->analyzed)) {
        lsp_analyze(srv, g);
        lsp_publish(srv, g);
    }
}

static void lsp_analyze_dirty(LspServer *srv)
{
    size_t i;
    for (i = 0; i < srv->groups.len; i++)
        if (srv->groups.data[i]->dirty)
            lsp_ensure_analyzed(srv, srv->groups.data[i]);
}

static int lsp_any_dirty(LspServer *srv)
{
    size_t i;
    for (i = 0; i < srv->groups.len; i++)
        if (srv->groups.data[i]->dirty)
            return 1;
    return 0;
}

/* --------------------------------------------------------------- groups --- */

static int lsp_streq_opt(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static LspGroup *lsp_group_for(LspServer *srv, const char *path)
{
    char *dir = os_path_dirname(path);
    char *root = pkg_find_root(dir);
    char *file = NULL;
    LspGroup *g;
    size_t i;
    free(dir);
    if (root) {
        char *rel = os_path_relative(path, root);
        int in_project = strncmp(rel, "src/", 4) == 0 || strncmp(rel, "tests/", 6) == 0;
        free(rel);
        if (!in_project)
            file = xstrdup(path);
    } else {
        file = xstrdup(path);
    }
    for (i = 0; i < srv->groups.len; i++) {
        g = srv->groups.data[i];
        if (lsp_streq_opt(g->root, root) && lsp_streq_opt(g->file, file)) {
            free(root);
            free(file);
            return g;
        }
    }
    g = (LspGroup *)xcalloc(1, sizeof *g);
    g->root = root;
    g->file = file;
    g->dirty = 1;
    vec_push(srv->groups, g);
    return g;
}

/* After `path` changed: its group and every group that reads it. */
static void lsp_mark_dirty(LspServer *srv, const char *path, LspGroup *own)
{
    size_t i;
    if (own)
        own->dirty = 1;
    for (i = 0; i < srv->groups.len; i++)
        if (path == NULL || lsp_group_file(srv->groups.data[i], path))
            srv->groups.data[i]->dirty = 1;
}

static void lsp_doc_free(LspDoc *d)
{
    free(d->uri);
    free(d->path);
    free(d->text);
    free(d);
}

static void lsp_drop_unused_groups(LspServer *srv)
{
    size_t i, k;
    for (i = 0; i < srv->groups.len;) {
        LspGroup *g = srv->groups.data[i];
        int used = 0;
        for (k = 0; k < srv->docs.len; k++)
            if (srv->docs.data[k]->group == g)
                used = 1;
        if (used) {
            i++;
            continue;
        }
        lsp_clear_published(srv, g);
        lsp_group_free(g);
        vec_remove(srv->groups, i);
    }
}

/* ------------------------------------------------------------ documents --- */

static LspDoc *lsp_doc_of(LspServer *srv, const JsonValue *params)
{
    const char *uri = json_as_str(json_get(json_get(params, "textDocument"), "uri"), NULL);
    char *path;
    LspDoc *d;
    if (!uri)
        return NULL;
    path = lsp_uri_to_path(uri);
    if (!path)
        return NULL;
    d = lsp_find_doc(srv, path);
    free(path);
    return d;
}

static void lsp_did_open(LspServer *srv, const JsonValue *params)
{
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_as_str(json_get(td, "uri"), NULL);
    const JsonValue *text = json_get(td, "text");
    char *path;
    LspDoc *d;
    if (!uri || !json_is(text, JSON_STRING))
        return;
    path = lsp_uri_to_path(uri);
    if (!path)
        return;
    d = lsp_find_doc(srv, path);
    if (!d) {
        d = (LspDoc *)xcalloc(1, sizeof *d);
        d->path = path;
        d->uri = xstrdup(uri);
        vec_push(srv->docs, d);
    } else {
        free(path);
        free(d->text);
    }
    d->text = xmemdup(text->str, text->len + 1);
    d->len = text->len;
    d->version = json_as_int(json_get(td, "version"), 0);
    lsp_srcpath_clear(srv); /* a new buffer can give a virtual path a home */
    d->group = lsp_group_for(srv, d->path);
    lsp_drop_unused_groups(srv); /* a reopened document may change groups */
    lsp_mark_dirty(srv, d->path, d->group);
}

static void lsp_apply_change(LspDoc *d, const JsonValue *ch)
{
    const JsonValue *range = json_get(ch, "range");
    const JsonValue *text = json_get(ch, "text");
    if (!json_is(text, JSON_STRING))
        return;
    if (!range) {
        free(d->text);
        d->text = xmemdup(text->str, text->len + 1);
        d->len = text->len;
    } else {
        const JsonValue *s = json_get(range, "start"), *e = json_get(range, "end");
        uint32_t a = lsp_offset_of(d->text, d->len, json_as_int(json_get(s, "line"), 0),
                                   json_as_int(json_get(s, "character"), 0));
        uint32_t b = lsp_offset_of(d->text, d->len, json_as_int(json_get(e, "line"), 0),
                                   json_as_int(json_get(e, "character"), 0));
        char *nt;
        size_t nl;
        if (b < a) {
            uint32_t t = a;
            a = b;
            b = t;
        }
        nl = d->len - (b - a) + text->len;
        nt = (char *)xmalloc(nl + 1);
        memcpy(nt, d->text, a);
        memcpy(nt + a, text->str, text->len);
        memcpy(nt + a + text->len, d->text + b, d->len - b);
        nt[nl] = '\0';
        free(d->text);
        d->text = nt;
        d->len = nl;
    }
}

static void lsp_did_change(LspServer *srv, const JsonValue *params)
{
    LspDoc *d = lsp_doc_of(srv, params);
    const JsonValue *changes = json_get(params, "contentChanges");
    size_t i;
    if (!d)
        return;
    for (i = 0; i < json_len(changes); i++)
        lsp_apply_change(d, json_at(changes, i));
    d->version = json_as_int(json_get(json_get(params, "textDocument"), "version"), d->version);
    lsp_mark_dirty(srv, d->path, d->group);
}

static void lsp_did_close(LspServer *srv, const JsonValue *params)
{
    LspDoc *d = lsp_doc_of(srv, params);
    size_t i;
    char *path;
    if (!d)
        return;
    path = xstrdup(d->path);
    for (i = 0; i < srv->docs.len; i++)
        if (srv->docs.data[i] == d) {
            vec_remove(srv->docs, i);
            break;
        }
    lsp_doc_free(d);
    lsp_srcpath_clear(srv);
    lsp_drop_unused_groups(srv);
    lsp_mark_dirty(srv, path, NULL); /* the file on disk replaces the buffer */
    free(path);
}

/* ----------------------------------------------------------- formatting --- */

static void lsp_formatting(LspServer *srv, LspDoc *d, LspFile *lf, StrBuf *out)
{
    StrBuf text = {0}, why = {0};
    JsonWriter w;
    uint32_t el, ec;
    if (!lf || lf->fu->ast->nerrors) {
        sb_puts(out, "[]");
        return;
    }
    fmt_format(lf->fu->ast, &text);
    if (!fmt_verify(lf->fu->ast, sb_cstr(&text), text.len, &why)) {
        lsp_log(srv, "formatting refused: %s", sb_cstr(&why));
        sb_puts(out, "[]");
    } else if (text.len == d->len && memcmp(text.data, d->text, d->len) == 0) {
        sb_puts(out, "[]");
    } else {
        lsp_pos_of(lf->src, (uint32_t)d->len, &el, &ec);
        jw_init(&w, out, 0);
        jw_array_begin(&w);
        jw_object_begin(&w);
        jw_key(&w, "range");
        jw_object_begin(&w);
        jw_key(&w, "start");
        jw_raw(&w, "{\"line\":0,\"character\":0}");
        jw_key(&w, "end");
        jw_object_begin(&w);
        jw_kv_uint(&w, "line", el);
        jw_kv_uint(&w, "character", ec);
        jw_object_end(&w);
        jw_object_end(&w);
        jw_key(&w, "newText");
        jw_strn(&w, text.data, text.len);
        jw_object_end(&w);
        jw_array_end(&w);
    }
    sb_free(&text);
    sb_free(&why);
}

/* ------------------------------------------------------------- dispatch --- */

static void lsp_initialize(LspServer *srv, const JsonValue *params, StrBuf *out)
{
    const char *root_uri = json_as_str(json_get(params, "rootUri"), NULL);
    const JsonValue *folders = json_get(params, "workspaceFolders");
    JsonWriter w;
    if (!root_uri && json_len(folders))
        root_uri = json_as_str(json_get(json_at(folders, 0), "uri"), NULL);
    free(srv->root);
    srv->root = root_uri ? lsp_uri_to_path(root_uri) : NULL;
    if (!srv->root && json_as_str(json_get(params, "rootPath"), NULL))
        srv->root = os_path_abs(json_as_str(json_get(params, "rootPath"), "."));
    srv->initialized = 1;
    jw_init(&w, out, 0);
    jw_object_begin(&w);
    jw_key(&w, "capabilities");
    jw_object_begin(&w);
    jw_key(&w, "textDocumentSync");
    jw_raw(&w, "{\"openClose\":true,\"change\":2,\"save\":{\"includeText\":false}}");
    jw_kv_bool(&w, "definitionProvider", 1);
    jw_kv_bool(&w, "hoverProvider", 1);
    jw_kv_bool(&w, "documentSymbolProvider", 1);
    jw_kv_bool(&w, "documentFormattingProvider", 1);
    jw_kv_bool(&w, "referencesProvider", 1);
    jw_kv_bool(&w, "documentHighlightProvider", 1);
    jw_kv_bool(&w, "workspaceSymbolProvider", 1);
    jw_key(&w, "completionProvider");
    jw_raw(&w, "{\"triggerCharacters\":[\".\",\":\"],\"resolveProvider\":false}");
    jw_key(&w, "experimental");
    jw_raw(&w, "{\"instanceTreeProvider\":true}");
    jw_object_end(&w);
    jw_key(&w, "serverInfo");
    jw_object_begin(&w);
    jw_kv_str(&w, "name", "tern");
    jw_kv_str(&w, "version", TERN_VERSION);
    jw_object_end(&w);
    jw_object_end(&w);
}

/* Resolves the document of a request, analyses it if needed and returns
 * the byte offset of params.position. */
static LspDoc *lsp_request_doc(LspServer *srv, const JsonValue *params, LspFile **lf, uint32_t *off)
{
    LspDoc *d = lsp_doc_of(srv, params);
    const JsonValue *pos = json_get(params, "position");
    *lf = NULL;
    if (off)
        *off = 0;
    if (!d)
        return NULL;
    lsp_ensure_analyzed(srv, d->group);
    *lf = lsp_group_file(d->group, d->path);
    if (off && pos)
        *off = lsp_offset_of(d->text, d->len, json_as_int(json_get(pos, "line"), 0),
                             json_as_int(json_get(pos, "character"), 0));
    return d;
}

/* Handles a request; returns 1 and fills `out` with the result, or 0 and
 * sets *code / *msg for an error. */
static int lsp_request(LspServer *srv, const char *method, const JsonValue *params, StrBuf *out, int *code,
                       const char **msg)
{
    LspDoc *d;
    LspFile *lf;
    uint32_t off;
    if (strcmp(method, "initialize") == 0) {
        lsp_initialize(srv, params, out);
        return 1;
    }
    if (!srv->initialized) {
        *code = RPC_SERVER_NOT_INITIALIZED;
        *msg = "the server is not initialized";
        return 0;
    }
    if (srv->shutdown) {
        *code = RPC_INVALID_REQUEST;
        *msg = "the server is shutting down";
        return 0;
    }
    if (strcmp(method, "shutdown") == 0) {
        srv->shutdown = 1;
        sb_puts(out, "null");
        return 1;
    }
    if (strcmp(method, "workspace/symbol") == 0) {
        size_t i;
        for (i = 0; i < srv->groups.len; i++)
            lsp_ensure_analyzed(srv, srv->groups.data[i]);
        lsp_nav_workspace_symbols(srv, json_as_str(json_get(params, "query"), ""), out);
        return 1;
    }
    if (strcmp(method, "tern/instanceTree") == 0) {
        LspGroup *only = NULL;
        size_t i;
        if (json_get(params, "textDocument")) {
            d = lsp_doc_of(srv, params);
            if (!d) {
                sb_puts(out, "[]");
                return 1;
            }
            only = d->group;
        }
        for (i = 0; i < srv->groups.len; i++)
            if (!only || srv->groups.data[i] == only)
                lsp_ensure_analyzed(srv, srv->groups.data[i]);
        lsp_nav_instance_tree(srv, only, out);
        return 1;
    }
    if (strncmp(method, "textDocument/", 13) != 0) {
        *code = RPC_METHOD_NOT_FOUND;
        *msg = "method not found";
        return 0;
    }
    method += 13;
    if (strcmp(method, "definition") != 0 && strcmp(method, "hover") != 0 &&
        strcmp(method, "documentSymbol") != 0 && strcmp(method, "formatting") != 0 &&
        strcmp(method, "completion") != 0 && strcmp(method, "references") != 0 &&
        strcmp(method, "documentHighlight") != 0) {
        *code = RPC_METHOD_NOT_FOUND;
        *msg = "method not found";
        return 0;
    }
    d = lsp_request_doc(srv, params, &lf, &off);
    if (!d) {
        /* unknown document: an empty answer */
        sb_puts(out, strcmp(method, "documentSymbol") == 0 || strcmp(method, "formatting") == 0 ||
                             strcmp(method, "references") == 0 || strcmp(method, "documentHighlight") == 0
                         ? "[]"
                         : "null");
        return 1;
    }
    if (strcmp(method, "definition") == 0)
        lsp_nav_definition(srv, d, lf, off, out);
    else if (strcmp(method, "hover") == 0)
        lsp_nav_hover(srv, d, lf, off, out);
    else if (strcmp(method, "documentSymbol") == 0)
        lsp_nav_document_symbols(srv, d, lf, out);
    else if (strcmp(method, "formatting") == 0)
        lsp_formatting(srv, d, lf, out);
    else if (strcmp(method, "completion") == 0)
        lsp_nav_completion(srv, d, lf, off, out);
    else if (strcmp(method, "references") == 0)
        lsp_nav_references(srv, d, lf, off,
                           json_as_bool(json_get(json_get(params, "context"), "includeDeclaration"), 1), 0, out);
    else
        lsp_nav_references(srv, d, lf, off, 1, 1, out);
    return 1;
}

static void lsp_notification(LspServer *srv, const char *method, const JsonValue *params)
{
    if (strcmp(method, "exit") == 0) {
        srv->exit_requested = 1;
        return;
    }
    if (!srv->initialized || srv->shutdown)
        return;
    if (strcmp(method, "textDocument/didOpen") == 0)
        lsp_did_open(srv, params);
    else if (strcmp(method, "textDocument/didChange") == 0)
        lsp_did_change(srv, params);
    else if (strcmp(method, "textDocument/didClose") == 0)
        lsp_did_close(srv, params);
    else if (strcmp(method, "textDocument/didSave") == 0) {
        LspDoc *d = lsp_doc_of(srv, params);
        if (d)
            lsp_mark_dirty(srv, d->path, d->group);
    } else if (strcmp(method, "workspace/didChangeWatchedFiles") == 0) {
        lsp_srcpath_clear(srv);
        lsp_mark_dirty(srv, NULL, NULL);
    }
    /* initialized, $/cancelRequest, $/setTrace, workspace/didChangeConfiguration: nothing to do */
}

static void lsp_handle(LspServer *srv, const char *body, size_t len)
{
    Arena a = {0};
    JsonError err;
    JsonValue *msg = json_parse(&a, body, len, &err);
    const JsonValue *id, *mv;
    const char *method;
    if (!msg) {
        lsp_log(srv, "<-- malformed JSON (%s)", err.msg);
        rpc_send_error(srv->io, NULL, RPC_PARSE_ERROR, "parse error");
        arena_release(&a);
        return;
    }
    if (srv->opt.log)
        lsp_log(srv, "<-- %.*s", (int)(len > 2000 ? 2000 : len), body);
    id = json_get(msg, "id");
    mv = json_get(msg, "method");
    if (!json_is(msg, JSON_OBJECT) || (mv && !json_is(mv, JSON_STRING))) {
        rpc_send_error(srv->io, id, RPC_INVALID_REQUEST, "invalid request");
        arena_release(&a);
        return;
    }
    if (!mv) {
        /* a response to a server request (none are sent): ignore it */
        if (!json_get(msg, "result") && !json_get(msg, "error"))
            rpc_send_error(srv->io, id, RPC_INVALID_REQUEST, "invalid request");
        arena_release(&a);
        return;
    }
    method = mv->str;
    if (id) {
        StrBuf out = {0};
        int code = 0;
        const char *emsg = NULL;
        if (lsp_request(srv, method, json_get(msg, "params"), &out, &code, &emsg)) {
            rpc_send_result(srv->io, id, out.len ? sb_cstr(&out) : "null");
            if (srv->opt.log)
                lsp_log(srv, "--> result %.*s", (int)(out.len > 2000 ? 2000 : out.len), sb_cstr(&out));
        } else {
            rpc_send_error(srv->io, id, code, emsg);
            lsp_log(srv, "--> error %d %s", code, emsg);
        }
        sb_free(&out);
    } else {
        lsp_notification(srv, method, json_get(msg, "params"));
    }
    arena_release(&a);
}

int lsp_run(const LspIo *io, const LspOptions *opt)
{
    LspServer srv;
    StrBuf body = {0};
    DiagSink old_sink;
    void *old_user;
    size_t i;
    int rc;
    memset(&srv, 0, sizeof srv);
    srv.io = io;
    if (opt)
        srv.opt = *opt;
    rpc_reader_init(&srv.rd, io);
    diag_flush();
    diag_get_sink(&old_sink, &old_user);
    diag_set_sink(lsp_quiet_sink, &srv);
    lsp_log(&srv, "tern lsp %s started", TERN_VERSION);
    while (!srv.exit_requested) {
        int r;
        if (lsp_any_dirty(&srv) &&
            (srv.opt.debounce_ms <= 0 || !rpc_input_pending(&srv.rd, srv.opt.debounce_ms))) {
            lsp_analyze_dirty(&srv);
            continue;
        }
        r = rpc_read(&srv.rd, &body);
        if (r == 0)
            break;
        if (r < 0) {
            lsp_log(&srv, "<-- malformed header");
            continue;
        }
        lsp_handle(&srv, body.data ? body.data : "", body.len);
    }
    rc = srv.exit_requested && srv.shutdown ? 0 : 1;
    lsp_log(&srv, "tern lsp exiting with %d", rc);
    for (i = 0; i < srv.docs.len; i++)
        lsp_doc_free(srv.docs.data[i]);
    vec_free(srv.docs);
    for (i = 0; i < srv.groups.len; i++)
        lsp_group_free(srv.groups.data[i]);
    vec_free(srv.groups);
    lsp_srcpath_clear(&srv);
    map_free(&srv.srcpath);
    free(srv.root);
    sb_free(&body);
    rpc_reader_free(&srv.rd);
    diag_set_sink(old_sink, old_user);
    return rc;
}

/* ------------------------------------------------------------- command --- */

int cmd_lsp(int argc, char **argv)
{
    ArgParser ap;
    const char *log_path = NULL;
    uint64_t debounce = 150;
    LspOptions o;
    LspIo io;
    int rc;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("lsp");
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "log", &log_path))
            ;
        else if (args_opt_u64(&ap, "debounce", &debounce))
            ;
        else if (args_flag(&ap, "stdio"))
            ; /* the only transport; accepted because clients pass it */
        else
            args_unknown(&ap);
    }
    if (ap.errors)
        return TERN_EXIT_USAGE;
    memset(&o, 0, sizeof o);
    o.debounce_ms = debounce > 10000 ? 10000 : (int)debounce;
    if (log_path) {
        o.log = fopen(log_path, "w");
        if (!o.log) {
            fprintf(stderr, "tern lsp: error: cannot write %s\n", log_path);
            return TERN_EXIT_USAGE;
        }
    }
    io = lsp_stdio_io();
    rc = lsp_run(&io, &o);
    if (o.log)
        fclose(o.log);
    return rc;
}
