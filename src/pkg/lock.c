/* lock.c — semantic versions and requirements, tern.lock, dependency
 * resolution, the on-disk layout ($TERN_HOME, cache, vendor/) and the
 * network-free dependency-root lookup used by the front-end driver. */
#include "pkg.h"
#include "../os/os.h"

/* ============================================================ versions === */

/* Parses a decimal component without leading zeros. Returns chars used, 0 on error. */
static size_t lock_num(const char *s, uint64_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    if (!tern_is_digit((unsigned char)s[0]))
        return 0;
    if (s[0] == '0' && tern_is_digit((unsigned char)s[1]))
        return 0;
    while (tern_is_digit((unsigned char)s[i])) {
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return 0;
        v = v * 10 + d;
        i++;
    }
    *out = v;
    return i;
}

/* Pre-release identifiers: [0-9A-Za-z-]+ separated by '.', numeric ones
 * without leading zeros. Returns chars used, 0 on error. */
static size_t lock_pre(const char *s, char *out, size_t outsz)
{
    size_t i = 0, idstart = 0;
    int numeric = 1;
    for (;;) {
        char c = s[i];
        if (tern_is_alnum((unsigned char)c) || c == '-') {
            if (!tern_is_digit((unsigned char)c))
                numeric = 0;
            i++;
            continue;
        }
        if (i == idstart)
            return 0; /* empty identifier */
        if (numeric && i - idstart > 1 && s[idstart] == '0')
            return 0;
        if (c != '.')
            break;
        i++;
        idstart = i;
        numeric = 1;
    }
    if (i + 1 > outsz)
        return 0;
    memcpy(out, s, i);
    out[i] = 0;
    return i;
}

int pkg_version_parse(const char *s, PkgVersion *v, int allow_v)
{
    size_t n;
    memset(v, 0, sizeof *v);
    if (!s)
        return -1;
    if (allow_v && (s[0] == 'v' || s[0] == 'V'))
        s++;
    if (!(n = lock_num(s, &v->major)) || s[n] != '.')
        return -1;
    s += n + 1;
    if (!(n = lock_num(s, &v->minor)) || s[n] != '.')
        return -1;
    s += n + 1;
    if (!(n = lock_num(s, &v->patch)))
        return -1;
    s += n;
    if (*s == '-') {
        if (!(n = lock_pre(s + 1, v->pre, sizeof v->pre)))
            return -1;
        s += n + 1;
    }
    if (*s == '+') {
        s++;
        if (!*s)
            return -1;
        while (tern_is_alnum((unsigned char)*s) || *s == '-' || *s == '.')
            s++;
    }
    return *s ? -1 : 0;
}

static int lock_pre_cmp(const char *a, const char *b)
{
    /* release (empty) > any pre-release */
    if (!a[0] || !b[0])
        return a[0] ? -1 : b[0] ? 1 : 0;
    for (;;) {
        size_t la = 0, lb = 0, i;
        int na = 1, nb = 1, c;
        while (a[la] && a[la] != '.') {
            if (!tern_is_digit((unsigned char)a[la]))
                na = 0;
            la++;
        }
        while (b[lb] && b[lb] != '.') {
            if (!tern_is_digit((unsigned char)b[lb]))
                nb = 0;
            lb++;
        }
        if (na && nb) {
            if (la != lb)
                return la < lb ? -1 : 1;
            c = memcmp(a, b, la);
            if (c)
                return c < 0 ? -1 : 1;
        } else if (na != nb) {
            return na ? -1 : 1; /* numeric identifiers sort first */
        } else {
            for (i = 0; i < la && i < lb; i++) {
                if (a[i] != b[i])
                    return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
            }
            if (la != lb)
                return la < lb ? -1 : 1;
        }
        a += la;
        b += lb;
        if (!*a || !*b)
            return *a ? 1 : *b ? -1 : 0;
        a++;
        b++;
    }
}

int pkg_version_cmp(const PkgVersion *a, const PkgVersion *b)
{
    if (a->major != b->major)
        return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor)
        return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch)
        return a->patch < b->patch ? -1 : 1;
    return lock_pre_cmp(a->pre, b->pre);
}

char *pkg_version_str(const PkgVersion *v, char buf[PKG_VERSION_STRLEN])
{
    snprintf(buf, PKG_VERSION_STRLEN, "%llu.%llu.%llu%s%s", (unsigned long long)v->major, (unsigned long long)v->minor,
             (unsigned long long)v->patch, v->pre[0] ? "-" : "", v->pre);
    return buf;
}

/* --------------------------------------------------------- requirements -- */

static void lock_req_add(PkgReq *r, PkgOp op, uint64_t ma, uint64_t mi, uint64_t pa, const char *pre)
{
    struct PkgReqCmp *c;
    if (r->n >= PKG_REQ_MAX)
        return;
    c = &r->cmp[r->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    c->v.major = ma;
    c->v.minor = mi;
    c->v.patch = pa;
    if (pre)
        snprintf(c->v.pre, sizeof c->v.pre, "%s", pre);
}

int pkg_req_parse(const char *s, PkgReq *r, char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    if (err && errlen)
        err[0] = 0;
    if (!s) {
        snprintf(err, errlen, "empty requirement");
        return -1;
    }
    for (;;) {
        const char *p = s;
        char op[3] = {0};
        uint64_t part[3] = {0, 0, 0};
        int nparts = 0;
        char pre[48] = {0};
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '*' || ((*p == 'x' || *p == 'X') && (p[1] == 0 || p[1] == ',' || p[1] == ' '))) {
            p++;
        } else {
            if (*p == '>' || *p == '<') {
                op[0] = *p++;
                if (*p == '=')
                    op[1] = *p++;
            } else if (*p == '=' || *p == '^' || *p == '~') {
                op[0] = *p++;
            }
            while (*p == ' ')
                p++;
            if (*p == 'v')
                p++;
            while (nparts < 3) {
                size_t n;
                if (*p == '*' || *p == 'x' || *p == 'X') {
                    p++;
                    /* wildcard: stop counting parts; skip any remaining .x */
                    while (*p == '.' && (p[1] == '*' || p[1] == 'x' || p[1] == 'X'))
                        p += 2;
                    break;
                }
                n = lock_num(p, &part[nparts]);
                if (!n) {
                    snprintf(err, errlen, "expected a version number at `%s`", *p ? p : "end");
                    return -1;
                }
                p += n;
                nparts++;
                if (*p != '.')
                    break;
                p++;
            }
            if (nparts == 0) {
                if (op[0]) {
                    snprintf(err, errlen, "`%s` needs a version after it", op);
                    return -1;
                }
                /* bare wildcard */
            }
            if (*p == '-') {
                size_t n;
                if (nparts != 3) {
                    snprintf(err, errlen, "a pre-release needs a full MAJOR.MINOR.PATCH version");
                    return -1;
                }
                n = lock_pre(p + 1, pre, sizeof pre);
                if (!n) {
                    snprintf(err, errlen, "invalid pre-release `%s`", p + 1);
                    return -1;
                }
                p += n + 1;
            }
            if (*p == '+') {
                p++;
                while (tern_is_alnum((unsigned char)*p) || *p == '-' || *p == '.')
                    p++;
            }
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && *p != ',') {
            snprintf(err, errlen, "unexpected `%c`", *p);
            return -1;
        }
        if (r->n + 2 > PKG_REQ_MAX) {
            snprintf(err, errlen, "too many comparators");
            return -1;
        }
        if (nparts > 0) {
            uint64_t ma = part[0], mi = part[1], pa = part[2];
            const char *o = op;
            if (!o[0] || strcmp(o, "^") == 0) {
                lock_req_add(r, PKG_OP_GE, ma, mi, pa, pre);
                if (ma > 0 || nparts == 1)
                    lock_req_add(r, PKG_OP_LT, ma + 1, 0, 0, NULL);
                else if (mi > 0 || nparts == 2)
                    lock_req_add(r, PKG_OP_LT, 0, mi + 1, 0, NULL);
                else
                    lock_req_add(r, PKG_OP_LT, 0, 0, pa + 1, NULL);
            } else if (strcmp(o, "~") == 0) {
                lock_req_add(r, PKG_OP_GE, ma, mi, pa, pre);
                if (nparts == 1)
                    lock_req_add(r, PKG_OP_LT, ma + 1, 0, 0, NULL);
                else
                    lock_req_add(r, PKG_OP_LT, ma, mi + 1, 0, NULL);
            } else if (strcmp(o, "=") == 0) {
                if (nparts == 3) {
                    lock_req_add(r, PKG_OP_EQ, ma, mi, pa, pre);
                } else {
                    lock_req_add(r, PKG_OP_GE, ma, mi, 0, NULL);
                    if (nparts == 1)
                        lock_req_add(r, PKG_OP_LT, ma + 1, 0, 0, NULL);
                    else
                        lock_req_add(r, PKG_OP_LT, ma, mi + 1, 0, NULL);
                }
            } else if (strcmp(o, ">") == 0) {
                if (nparts == 3)
                    lock_req_add(r, PKG_OP_GT, ma, mi, pa, pre);
                else if (nparts == 2)
                    lock_req_add(r, PKG_OP_GE, ma, mi + 1, 0, NULL);
                else
                    lock_req_add(r, PKG_OP_GE, ma + 1, 0, 0, NULL);
            } else if (strcmp(o, ">=") == 0) {
                lock_req_add(r, PKG_OP_GE, ma, mi, pa, pre);
            } else if (strcmp(o, "<") == 0) {
                lock_req_add(r, PKG_OP_LT, ma, mi, pa, pre);
            } else { /* <= */
                if (nparts == 3)
                    lock_req_add(r, PKG_OP_LE, ma, mi, pa, pre);
                else if (nparts == 2)
                    lock_req_add(r, PKG_OP_LT, ma, mi + 1, 0, NULL);
                else
                    lock_req_add(r, PKG_OP_LT, ma + 1, 0, 0, NULL);
            }
        }
        if (!*p)
            break;
        s = p + 1;
        while (*s == ' ')
            s++;
        if (!*s) {
            snprintf(err, errlen, "trailing `,`");
            return -1;
        }
    }
    return 0;
}

int pkg_req_matches(const PkgReq *r, const PkgVersion *v)
{
    int i, pre_ok = v->pre[0] == 0;
    for (i = 0; i < r->n; i++) {
        const struct PkgReqCmp *c = &r->cmp[i];
        int k = pkg_version_cmp(v, &c->v);
        int ok = 0;
        switch (c->op) {
        case PKG_OP_EQ: ok = k == 0; break;
        case PKG_OP_GT: ok = k > 0; break;
        case PKG_OP_GE: ok = k >= 0; break;
        case PKG_OP_LT: ok = k < 0; break;
        case PKG_OP_LE: ok = k <= 0; break;
        }
        if (!ok)
            return 0;
        if (c->v.pre[0] && c->v.major == v->major && c->v.minor == v->minor && c->v.patch == v->patch)
            pre_ok = 1;
    }
    return pre_ok;
}

int pkg_req_matches_str(const char *req, const char *version)
{
    PkgReq r;
    PkgVersion v;
    char err[128];
    if (pkg_req_parse(req, &r, err, sizeof err) != 0 || pkg_version_parse(version, &v, 1) != 0)
        return 0;
    return pkg_req_matches(&r, &v);
}

/* ================================================================ lock === */

const char *pkg_source_prefix(PkgSourceKind k)
{
    return k == PKG_SRC_REGISTRY ? "registry+" : k == PKG_SRC_GIT ? "git+" : "path+";
}

const PkgLocked *pkg_lock_find(const PkgLock *lock, const char *name)
{
    size_t i;
    for (i = 0; i < lock->pkgs.len; i++) {
        if (strcmp(lock->pkgs.data[i].name, name) == 0)
            return &lock->pkgs.data[i];
    }
    return NULL;
}

static const char *lock_opt_str(PkgLock *lock, const TomlValue *t, const char *key, int *errs)
{
    const TomlValue *v = toml_get(t, key);
    if (!v)
        return NULL;
    if (v->kind != TOML_STRING) {
        diag_error(v->span, "`%s` in tern.lock must be a string", key);
        (*errs)++;
        return NULL;
    }
    return arena_strdup(&lock->arena, v->str);
}

int pkg_lock_read(const char *path, PkgLock *lock)
{
    Arena tmp = {0};
    const TomlValue *doc, *v, *e;
    int errs = 0;
    memset(lock, 0, sizeof *lock);
    if (!os_exists(path))
        return 1;
    doc = toml_parse_file(&tmp, path, NULL);
    if (!doc) {
        diag_help("tern.lock is generated; delete it and run `tern update` to regenerate it");
        arena_release(&tmp);
        return -1;
    }
    v = toml_get(doc, "version");
    if (v && (v->kind != TOML_INT || v->num != 1)) {
        diag_error(v->span, "unsupported tern.lock format version");
        diag_help("this tern.lock was written by a newer tern; upgrade tern or run `tern update`");
        arena_release(&tmp);
        return -1;
    }
    v = toml_get(doc, "vendored");
    if (v)
        lock->vendored = v->kind == TOML_BOOL && v->boolean;
    v = toml_get(doc, "package");
    if (v && v->kind != TOML_ARRAY) {
        diag_error(v->span, "`package` in tern.lock must be written as [[package]] tables");
        errs++;
        v = NULL;
    }
    for (e = v ? v->first : NULL; e; e = e->next) {
        PkgLocked pk;
        const char *src;
        const TomlValue *deps;
        memset(&pk, 0, sizeof pk);
        if (e->kind != TOML_TABLE) {
            diag_error(e->span, "each [[package]] in tern.lock must be a table");
            errs++;
            continue;
        }
        pk.name = lock_opt_str(lock, e, "name", &errs);
        pk.version = lock_opt_str(lock, e, "version", &errs);
        src = lock_opt_str(lock, e, "source", &errs);
        pk.tag = lock_opt_str(lock, e, "tag", &errs);
        pk.rev = lock_opt_str(lock, e, "rev", &errs);
        pk.branch = lock_opt_str(lock, e, "branch", &errs);
        pk.commit = lock_opt_str(lock, e, "commit", &errs);
        if (!pk.name || !pk.version || !src) {
            diag_error(e->span, "[[package]] in tern.lock needs `name`, `version` and `source`");
            diag_help("tern.lock is generated; delete it and run `tern update` to regenerate it");
            errs++;
            continue;
        }
        if (tern_starts_with(src, "registry+")) {
            pk.kind = PKG_SRC_REGISTRY;
            pk.url = src + 9;
        } else if (tern_starts_with(src, "git+")) {
            pk.kind = PKG_SRC_GIT;
            pk.url = src + 4;
        } else if (tern_starts_with(src, "path+")) {
            pk.kind = PKG_SRC_PATH;
            pk.url = src + 5;
        } else {
            diag_error(toml_get(e, "source")->span, "unknown source \"%s\" in tern.lock", src);
            diag_help("sources are \"registry+<git url>\", \"git+<git url>\" or \"path+<dir>\"");
            errs++;
            continue;
        }
        if (pk.kind != PKG_SRC_PATH && !pk.commit) {
            diag_error(e->span, "[[package]] `%s` in tern.lock has no `commit`", pk.name);
            errs++;
            continue;
        }
        deps = toml_get(e, "dependencies");
        if (deps && deps->kind == TOML_ARRAY) {
            const TomlValue *d;
            pk.deps = ANEWN(&lock->arena, const char *, deps->count + 1);
            for (d = deps->first; d; d = d->next) {
                if (d->kind == TOML_STRING)
                    pk.deps[pk.ndeps++] = arena_strdup(&lock->arena, d->str);
            }
        }
        if (pkg_lock_find(lock, pk.name)) {
            diag_error(e->span, "package `%s` appears twice in tern.lock", pk.name);
            errs++;
            continue;
        }
        vec_push(lock->pkgs, pk);
    }
    arena_release(&tmp);
    return errs ? -1 : 0;
}

static int lock_cmp_name(const void *a, const void *b)
{
    return strcmp(((const PkgLocked *)a)->name, ((const PkgLocked *)b)->name);
}

void pkg_lock_format(const PkgLock *lock, StrBuf *out)
{
    size_t i, j;
    PkgLocked *sorted = (PkgLocked *)xmalloc(sizeof(PkgLocked) * (lock->pkgs.len + 1));
    if (lock->pkgs.len)
        memcpy(sorted, lock->pkgs.data, sizeof(PkgLocked) * lock->pkgs.len);
    qsort(sorted, lock->pkgs.len, sizeof(PkgLocked), lock_cmp_name);
    sb_puts(out, "# tern.lock: generated by `tern add` / `tern update`; do not edit by hand.\n"
                 "# Commit it so builds are reproducible.\n"
                 "version = 1\n");
    if (lock->vendored)
        sb_puts(out, "vendored = true\n");
    for (i = 0; i < lock->pkgs.len; i++) {
        const PkgLocked *p = &sorted[i];
        sb_puts(out, "\n[[package]]\nname = ");
        toml_put_string(out, p->name);
        sb_puts(out, "\nversion = ");
        toml_put_string(out, p->version);
        sb_puts(out, "\nsource = ");
        {
            StrBuf s = {0};
            sb_puts(&s, pkg_source_prefix(p->kind));
            sb_puts(&s, p->url);
            toml_put_string(out, sb_cstr(&s));
            sb_free(&s);
        }
        if (p->tag) {
            sb_puts(out, "\ntag = ");
            toml_put_string(out, p->tag);
        }
        if (p->rev) {
            sb_puts(out, "\nrev = ");
            toml_put_string(out, p->rev);
        }
        if (p->branch) {
            sb_puts(out, "\nbranch = ");
            toml_put_string(out, p->branch);
        }
        if (p->commit) {
            sb_puts(out, "\ncommit = ");
            toml_put_string(out, p->commit);
        }
        if (p->ndeps) {
            sb_puts(out, "\ndependencies = [");
            for (j = 0; j < p->ndeps; j++) {
                if (j)
                    sb_puts(out, ", ");
                toml_put_string(out, p->deps[j]);
            }
            sb_puts(out, "]");
        }
        sb_puts(out, "\n");
    }
    free(sorted);
}

int pkg_lock_write(const char *path, const PkgLock *lock)
{
    StrBuf sb = {0};
    int r;
    pkg_lock_format(lock, &sb);
    r = os_write_file_if_changed(path, sb.data, sb.len);
    sb_free(&sb);
    if (r < 0) {
        diag_error(span_none(), "cannot write `%s`", path);
        return -1;
    }
    return 0;
}

void pkg_lock_free(PkgLock *lock)
{
    vec_free(lock->pkgs);
    arena_release(&lock->arena);
    memset(lock, 0, sizeof *lock);
}

/* ============================================================== layout === */

char *pkg_home_dir(void)
{
    char *h = os_getenv("TERN_HOME"), *home, *r;
    if (h && h[0]) {
        r = os_path_abs(h);
        free(h);
        return r;
    }
    free(h);
    home = os_home_dir();
    if (!home)
        return os_path_abs(".tern");
    r = os_path_join(home, ".tern");
    free(home);
    return r;
}

char *pkg_index_dir(void)
{
    char *h = pkg_home_dir(), *r = os_path_join(h, "index");
    free(h);
    return r;
}

char *pkg_cache_dir(const char *name, const char *commit)
{
    char *h = pkg_home_dir(), *r;
    StrBuf sb = {0};
    sb_printf(&sb, "%s/cache/%s/%s", h, name, commit);
    free(h);
    r = sb_take(&sb);
    return r;
}

char *pkg_index_url(const TernManifest *m)
{
    char *e = os_getenv("TERN_INDEX");
    if (e && e[0])
        return e;
    free(e);
    if (m && m->registry_index && m->registry_index[0])
        return xstrdup(m->registry_index);
    return xstrdup(PKG_DEFAULT_INDEX);
}

char *pkg_path_rel(const char *path, const char *base)
{
    char *p = os_path_abs(path), *b = os_path_abs(base);
    size_t i = 0, common = 0;
    StrBuf sb = {0};
    const char *rest;
    /* longest common prefix ending at a component boundary */
    while (p[i] && b[i] && p[i] == b[i]) {
        i++;
        if ((p[i] == '/' || p[i] == 0) && (b[i] == '/' || b[i] == 0))
            common = i;
    }
    if (common == 0 && !(p[0] == '/' && b[0] == '/')) {
        free(b);
        return p; /* different roots (drives): keep absolute */
    }
    if (b[0] == '/' && b[1] == 0)
        common = 0;
    rest = p + common;
    if (*rest == '/')
        rest++;
    {
        const char *q = b + common;
        if (*q == '/')
            q++;
        while (*q) {
            sb_puts(&sb, "../");
            while (*q && *q != '/')
                q++;
            if (*q == '/')
                q++;
        }
    }
    sb_puts(&sb, rest);
    if (sb.len == 0)
        sb_puts(&sb, ".");
    else if (sb.data[sb.len - 1] == '/')
        sb_truncate(&sb, sb.len - 1);
    free(p);
    free(b);
    return sb_take(&sb);
}

/* ========================================================== resolution === */

typedef struct ResReq {
    const char *requirer;      /* "my-soc" */
    const TernDep *dep;
    const char *base_dir;      /* directory of the requirer (path deps) */
} ResReq;

typedef VEC(ResReq) ResReqVec;

typedef struct ResPkg {
    PkgNode node;
    TernManifest man;
    int has_man;
    int keep;
} ResPkg;

typedef VEC(ResPkg *) ResPkgVec;

typedef struct ResCacheEntry {
    const char *key;           /* "reg:<name>" or "git:<url>" */
    int status;                /* 0 ok, 1 unknown, -1 error */
    PkgAvailList list;
} ResCacheEntry;

typedef VEC(ResCacheEntry) ResCacheVec;

typedef struct Resolver {
    const TernManifest *root;
    const PkgResolveOpts *opts;
    const PkgProvider *prov;
    Arena arena;
    ResPkgVec sel;
    ResCacheVec cache;
    int errors;
} Resolver;

static int res_may_move(const Resolver *R, const char *name)
{
    const char *const *u;
    if (!R->opts || !R->opts->prev)
        return 1;
    if (R->opts->update_all)
        return 1;
    for (u = R->opts->update; u && *u; u++) {
        if (strcmp(*u, name) == 0)
            return 1;
    }
    return 0;
}

static const PkgLocked *res_prev(const Resolver *R, const char *name)
{
    if (!R->opts || !R->opts->prev || res_may_move(R, name))
        return NULL;
    return pkg_lock_find(R->opts->prev, name);
}

static void res_avail_free(PkgAvailList *l)
{
    vec_free(l->items);
}

static ResCacheEntry *res_lookup(Resolver *R, int registry, const char *what)
{
    size_t i;
    ResCacheEntry e;
    char *key = arena_printf(&R->arena, "%s:%s", registry ? "reg" : "git", what);
    for (i = 0; i < R->cache.len; i++) {
        if (strcmp(R->cache.data[i].key, key) == 0)
            return &R->cache.data[i];
    }
    memset(&e, 0, sizeof e);
    e.key = key;
    if (registry)
        e.status = R->prov->registry ? R->prov->registry(R->prov->user, what, &R->arena, &e.list) : 1;
    else
        e.status = R->prov->tags ? R->prov->tags(R->prov->user, what, &R->arena, &e.list) : -1;
    vec_push(R->cache, e);
    return &vec_last(R->cache);
}

static const char *res_dep_desc(Arena *a, const TernDep *d)
{
    switch (d->kind) {
    case PKG_DEP_REGISTRY: return arena_printf(a, "\"%s\" from the index", d->req);
    case PKG_DEP_GIT:
        if (d->tag)
            return arena_printf(a, "%s (tag %s)", d->git, d->tag);
        if (d->rev)
            return arena_printf(a, "%s (rev %s)", d->git, d->rev);
        if (d->branch)
            return arena_printf(a, "%s (branch %s)", d->git, d->branch);
        if (d->req)
            return arena_printf(a, "%s (version \"%s\")", d->git, d->req);
        return arena_printf(a, "%s", d->git);
    case PKG_DEP_PATH: return arena_printf(a, "path \"%s\"", d->path);
    }
    return "";
}

static void res_note_reqs(Resolver *R, const ResReq *reqs, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        diag_note(reqs[i].dep->span, "`%s` requires `%s` %s", reqs[i].requirer, reqs[i].dep->name,
                  res_dep_desc(&R->arena, reqs[i].dep));
}

static int res_same_str(const char *a, const char *b)
{
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

/* Picks the package for the requirements reqs[0..n) on one name.
 * Returns 0 and fills *node, or -1 after reporting. */
static int res_choose(Resolver *R, const ResReq *reqs, size_t n, PkgNode *node)
{
    const char *name = reqs[0].dep->name;
    const TernDep *d0 = reqs[0].dep;
    PkgSourceKind kind = d0->kind == PKG_DEP_REGISTRY ? PKG_SRC_REGISTRY : d0->kind == PKG_DEP_GIT ? PKG_SRC_GIT : PKG_SRC_PATH;
    const char *url = NULL, *tag = NULL, *rev = NULL, *branch = NULL;
    const PkgLocked *prev = res_prev(R, name);
    ResCacheEntry *ce = NULL;
    size_t i;
    memset(node, 0, sizeof *node);
    node->name = name;
    node->kind = kind;
    /* every requirement must name the same source */
    for (i = 0; i < n; i++) {
        const TernDep *d = reqs[i].dep;
        const char *u = NULL;
        if (d->kind != d0->kind)
            goto source_conflict;
        if (d->kind == PKG_DEP_GIT)
            u = d->git;
        else if (d->kind == PKG_DEP_PATH) {
            char *j = os_path_join(reqs[i].base_dir, d->path);
            char *a = os_path_abs(j);
            u = arena_strdup(&R->arena, a);
            free(j);
            free(a);
        }
        if (i == 0)
            url = u;
        else if (!res_same_str(url, u))
            goto source_conflict;
        if (d->kind == PKG_DEP_GIT && (d->tag || d->rev || d->branch)) {
            if ((tag || rev || branch) && !(res_same_str(tag, d->tag) && res_same_str(rev, d->rev) &&
                                            res_same_str(branch, d->branch))) {
                diag_error(span_none(), "conflicting git pins for `%s`", name);
                res_note_reqs(R, reqs, n);
                diag_help("make every package that depends on `%s` use the same tag/rev/branch", name);
                return -1;
            }
            tag = d->tag;
            rev = d->rev;
            branch = d->branch;
        }
    }
    if (kind == PKG_SRC_PATH) {
        node->url = url;
        return 0;
    }
    if (kind == PKG_SRC_REGISTRY) {
        ce = res_lookup(R, 1, name);
        if (ce->status == 1) {
            diag_error(reqs[0].dep->span, "package `%s` is not in the registry index", name);
            diag_help("check the spelling, or depend on it by URL: %s = { git = \"https://...\" }", name);
            return -1;
        }
        if (ce->status < 0)
            return -1;
        node->url = ce->list.git;
    } else {
        node->url = url;
        node->tag = tag;
        node->rev = rev;
        node->branch = branch;
        if (tag || rev || branch) {
            if (prev && prev->kind == PKG_SRC_GIT && res_same_str(prev->url, url) && res_same_str(prev->tag, tag) &&
                res_same_str(prev->rev, rev) && res_same_str(prev->branch, branch))
                node->commit = prev->commit;
            return 0;
        }
        ce = res_lookup(R, 0, url);
        if (ce->status < 0)
            return -1;
    }
    /* version selection over ce->list */
    {
        PkgReq rq[64];
        size_t nrq = 0;
        const PkgAvail *best = NULL, *locked = NULL;
        for (i = 0; i < n && nrq < TERN_ARRAY_LEN(rq); i++) {
            char err[128];
            if (reqs[i].dep->req && pkg_req_parse(reqs[i].dep->req, &rq[nrq], err, sizeof err) == 0)
                nrq++;
        }
        for (i = 0; i < ce->list.items.len; i++) {
            const PkgAvail *a = &ce->list.items.data[i];
            size_t k;
            int ok = 1;
            for (k = 0; k < nrq && ok; k++)
                ok = pkg_req_matches(&rq[k], &a->version);
            if (!ok)
                continue;
            if (nrq == 0 && a->version.pre[0])
                continue; /* no requirement: releases only */
            if (prev && prev->kind == kind && res_same_str(prev->url, node->url)) {
                PkgVersion pv;
                if (pkg_version_parse(prev->version, &pv, 1) == 0 && pkg_version_cmp(&pv, &a->version) == 0)
                    locked = a;
            }
            if (a->yanked)
                continue;
            if (!best || pkg_version_cmp(&a->version, &best->version) > 0)
                best = a;
        }
        if (locked)
            best = locked;
        if (!best) {
            if (kind == PKG_SRC_GIT && ce->list.items.len == 0 && nrq == 0) {
                /* a repository without semver tags: follow its default branch */
                if (prev && prev->kind == PKG_SRC_GIT && res_same_str(prev->url, url) && !prev->tag && !prev->rev &&
                    !prev->branch)
                    node->commit = prev->commit;
                return 0;
            }
            {
                StrBuf av = {0};
                size_t shown = 0;
                for (i = 0; i < ce->list.items.len; i++) {
                    char vb[PKG_VERSION_STRLEN];
                    if (ce->list.items.data[i].yanked)
                        continue;
                    if (shown++ == 12) {
                        sb_puts(&av, ", ...");
                        break;
                    }
                    sb_printf(&av, "%s%s", av.len ? ", " : "", pkg_version_str(&ce->list.items.data[i].version, vb));
                }
                if (n == 1)
                    diag_error(reqs[0].dep->span, "no version of `%s` matches \"%s\"", name,
                               reqs[0].dep->req ? reqs[0].dep->req : "*");
                else
                    diag_error(span_none(), "cannot resolve `%s`: no version satisfies every requirement", name);
                res_note_reqs(R, reqs, n);
                if (av.len)
                    diag_note(span_none(), "available versions of `%s`: %s", name, sb_cstr(&av));
                else
                    diag_note(span_none(), "`%s` has no published versions%s", name,
                              kind == PKG_SRC_GIT ? " (no vMAJOR.MINOR.PATCH tags)" : "");
                if (n > 1)
                    diag_help("relax one of the requirements so that a single version satisfies all of them");
                else
                    diag_help("change the requirement in tern.toml to one of the available versions");
                sb_free(&av);
            }
            return -1;
        }
        {
            char vb[PKG_VERSION_STRLEN];
            node->version = arena_strdup(&R->arena, pkg_version_str(&best->version, vb));
        }
        node->tag = best->tag;
        node->commit = best->commit;
        if (prev && locked == best && prev->commit)
            node->commit = prev->commit;
    }
    return 0;

source_conflict:
    diag_error(span_none(), "`%s` is required from different sources", name);
    res_note_reqs(R, reqs, n);
    diag_help("a package name can only refer to one source; make the requirements agree (or rename one dependency)");
    return -1;
}

static int res_node_same(const PkgNode *a, const PkgNode *b)
{
    return a->kind == b->kind && res_same_str(a->url, b->url) && res_same_str(a->tag, b->tag) &&
           res_same_str(a->rev, b->rev) && res_same_str(a->branch, b->branch) &&
           (!a->version || !b->version || strcmp(a->version, b->version) == 0) &&
           (!a->commit || !b->commit || strcmp(a->commit, b->commit) == 0);
}

static ResPkg *res_find_sel(Resolver *R, const char *name)
{
    size_t i;
    for (i = 0; i < R->sel.len; i++) {
        if (strcmp(R->sel.data[i]->node.name, name) == 0)
            return R->sel.data[i];
    }
    return NULL;
}

static void res_pkg_free(ResPkg *p)
{
    if (p->has_man)
        pkg_manifest_free(&p->man);
    free(p);
}

static int res_fetch(Resolver *R, ResPkg *p)
{
    char *mpath;
    int r;
    if (R->prov->fetch(R->prov->user, &p->node, &R->arena) != 0)
        return -1;
    if (!p->node.root) {
        diag_error(span_none(), "internal: fetching `%s` gave no directory", p->node.name);
        return -1;
    }
    mpath = os_path_join(p->node.root, PKG_MANIFEST_FILE);
    if (!os_is_file(mpath)) {
        diag_error(span_none(), "dependency `%s` (%s) has no tern.toml", p->node.name, p->node.url);
        diag_help("a Tern package is a git repository with tern.toml, src/ and tests/ at its root");
        free(mpath);
        return -1;
    }
    r = pkg_manifest_load(mpath, &p->man);
    free(mpath);
    p->has_man = 1;
    if (r != 0) {
        diag_error(span_none(), "the tern.toml of dependency `%s` is invalid (see above)", p->node.name);
        return -1;
    }
    if (p->node.kind == PKG_SRC_REGISTRY && p->node.version && p->man.version &&
        strcmp(p->node.version, p->man.version) != 0) {
        diag_warning(span_none(), "`%s` %s: its tern.toml says version %s", p->node.name, p->node.version,
                     p->man.version);
    }
    if (!p->node.version)
        p->node.version = arena_strdup(&R->arena, p->man.version ? p->man.version : "0.0.0");
    return 0;
}

int pkg_resolve(const TernManifest *m, const PkgResolveOpts *o, const PkgProvider *p, PkgLock *out)
{
    Resolver R;
    int iter, rc = -1;
    size_t i, j;
    memset(&R, 0, sizeof R);
    R.root = m;
    R.opts = o;
    R.prov = p;
    memset(out, 0, sizeof *out);
    for (iter = 0; iter < 64; iter++) {
        ResReqVec reqs = {0};
        VEC(const char *) names = {0};
        ResPkgVec next = {0};
        int changed = 0, bad = 0;
        size_t qi;
        /* 1. gather requirements breadth-first from the root through the
         *    packages selected so far */
        for (i = 0; i < m->ndeps; i++) {
            ResReq rq;
            rq.requirer = m->name ? m->name : "the project";
            rq.dep = &m->deps[i];
            rq.base_dir = m->root;
            vec_push(reqs, rq);
        }
        for (qi = 0; qi < reqs.len; qi++) {
            const char *nm = reqs.data[qi].dep->name;
            ResPkg *sp;
            for (j = 0; j < names.len; j++) {
                if (strcmp(names.data[j], nm) == 0)
                    break;
            }
            if (j < names.len)
                continue;
            vec_push(names, nm);
            sp = res_find_sel(&R, nm);
            if (sp && sp->has_man) {
                for (j = 0; j < sp->man.ndeps; j++) {
                    ResReq rq;
                    rq.requirer = sp->node.name;
                    rq.dep = &sp->man.deps[j];
                    rq.base_dir = sp->man.root;
                    vec_push(reqs, rq);
                }
            }
        }
        /* 2. choose per name */
        for (j = 0; j < names.len; j++) {
            ResReqVec group = {0};
            PkgNode node;
            ResPkg *cur;
            for (qi = 0; qi < reqs.len; qi++) {
                if (strcmp(reqs.data[qi].dep->name, names.data[j]) == 0)
                    vec_push(group, reqs.data[qi]);
            }
            if (res_choose(&R, group.data, group.len, &node) != 0) {
                bad = 1;
                vec_free(group);
                continue;
            }
            vec_free(group);
            cur = res_find_sel(&R, node.name);
            if (cur && res_node_same(&cur->node, &node)) {
                cur->keep = 1;
                vec_push(next, cur);
            } else {
                ResPkg *np = (ResPkg *)xcalloc(1, sizeof(ResPkg));
                np->node = node;
                np->keep = 1;
                vec_push(next, np);
                changed = 1;
            }
        }
        if (next.len != R.sel.len)
            changed = 1;
        vec_free(reqs);
        vec_free(names);
        /* drop packages that are no longer selected */
        for (i = 0; i < R.sel.len; i++) {
            if (!R.sel.data[i]->keep)
                res_pkg_free(R.sel.data[i]);
        }
        vec_free(R.sel);
        R.sel = next;
        for (i = 0; i < R.sel.len; i++)
            R.sel.data[i]->keep = 0;
        if (bad)
            goto done;
        /* 3. fetch new selections (their manifests add requirements) */
        for (i = 0; i < R.sel.len; i++) {
            if (!R.sel.data[i]->has_man && res_fetch(&R, R.sel.data[i]) != 0)
                goto done;
        }
        if (!changed)
            break;
    }
    if (iter == 64) {
        diag_error(span_none(), "dependency resolution did not converge");
        diag_help("pin the packages involved to exact versions (\"=1.2.3\") in tern.toml");
        goto done;
    }
    /* 4. the lock */
    for (i = 0; i < R.sel.len; i++) {
        const ResPkg *sp = R.sel.data[i];
        PkgLocked pk;
        memset(&pk, 0, sizeof pk);
        pk.name = arena_strdup(&out->arena, sp->node.name);
        pk.version = arena_strdup(&out->arena, sp->node.version ? sp->node.version : "0.0.0");
        pk.kind = sp->node.kind;
        if (sp->node.kind == PKG_SRC_PATH) {
            char *rel = pkg_path_rel(sp->node.url, m->root);
            pk.url = arena_strdup(&out->arena, rel);
            free(rel);
        } else {
            pk.url = arena_strdup(&out->arena, sp->node.url);
            pk.commit = sp->node.commit ? arena_strdup(&out->arena, sp->node.commit) : NULL;
        }
        pk.tag = sp->node.tag ? arena_strdup(&out->arena, sp->node.tag) : NULL;
        pk.rev = sp->node.rev ? arena_strdup(&out->arena, sp->node.rev) : NULL;
        pk.branch = sp->node.branch ? arena_strdup(&out->arena, sp->node.branch) : NULL;
        if (sp->has_man && sp->man.ndeps) {
            pk.deps = ANEWN(&out->arena, const char *, sp->man.ndeps);
            for (j = 0; j < sp->man.ndeps; j++)
                pk.deps[pk.ndeps++] = arena_strdup(&out->arena, sp->man.deps[j].name);
        }
        vec_push(out->pkgs, pk);
    }
    qsort(out->pkgs.data, out->pkgs.len, sizeof(PkgLocked), lock_cmp_name);
    if (o && o->prev)
        out->vendored = o->prev->vendored;
    rc = 0;
done:
    for (i = 0; i < R.sel.len; i++)
        res_pkg_free(R.sel.data[i]);
    vec_free(R.sel);
    for (i = 0; i < R.cache.len; i++)
        res_avail_free(&R.cache.data[i].list);
    vec_free(R.cache);
    arena_release(&R.arena);
    if (rc != 0)
        pkg_lock_free(out);
    return rc;
}

/* =================================================== dependency roots === */

static int lock_dep_matches(const TernManifest *m, const TernDep *d, const PkgLocked *l, const char **why)
{
    *why = NULL;
    switch (d->kind) {
    case PKG_DEP_REGISTRY:
        if (l->kind != PKG_SRC_REGISTRY) {
            *why = "it is locked from a different source";
            return 0;
        }
        break;
    case PKG_DEP_GIT:
        if (l->kind != PKG_SRC_GIT || strcmp(l->url, d->git) != 0) {
            *why = "its git URL changed";
            return 0;
        }
        if ((d->tag || d->rev || d->branch) &&
            !(res_same_str(d->tag, l->tag) && res_same_str(d->rev, l->rev) && res_same_str(d->branch, l->branch))) {
            *why = "its tag/rev/branch changed";
            return 0;
        }
        break;
    case PKG_DEP_PATH: {
        char *j, *a, *b, *lj;
        int same;
        if (l->kind != PKG_SRC_PATH) {
            *why = "it is locked from a different source";
            return 0;
        }
        j = os_path_join(m->root, d->path);
        a = os_path_abs(j);
        lj = os_path_join(m->root, l->url);
        b = os_path_abs(lj);
        same = strcmp(a, b) == 0;
        free(j);
        free(a);
        free(lj);
        free(b);
        if (!same) {
            *why = "its path changed";
            return 0;
        }
        return 1;
    }
    }
    if (d->req && !pkg_req_matches_str(d->req, l->version)) {
        *why = "the locked version does not match the requirement";
        return 0;
    }
    return 1;
}

int pkg_lock_satisfies(const TernManifest *m, const PkgLock *lock, int report)
{
    size_t i;
    int ok = 1;
    for (i = 0; i < m->ndeps; i++) {
        const TernDep *d = &m->deps[i];
        const PkgLocked *l = pkg_lock_find(lock, d->name);
        const char *why;
        if (!l) {
            ok = 0;
            if (report) {
                diag_error(d->span, "dependency `%s` is not in tern.lock", d->name);
                diag_help("run `tern add` to resolve and fetch it");
            }
        } else if (!lock_dep_matches(m, d, l, &why)) {
            ok = 0;
            if (report) {
                diag_error(d->span, "tern.lock is out of date for `%s`: %s (locked: %s)", d->name, why, l->version);
                diag_help("run `tern update %s`", d->name);
            }
        }
    }
    return ok;
}

int pkg_lock_fetch(const TernManifest *m, const PkgLock *lock, const PkgProvider *p)
{
    Arena a = {0};
    size_t i;
    int errs = 0;
    for (i = 0; i < lock->pkgs.len; i++) {
        const PkgLocked *l = &lock->pkgs.data[i];
        PkgNode n;
        memset(&n, 0, sizeof n);
        n.name = l->name;
        n.kind = l->kind;
        n.tag = l->tag;
        n.rev = l->rev;
        n.branch = l->branch;
        n.commit = l->commit;
        n.version = l->version;
        if (l->kind == PKG_SRC_PATH) {
            char *j = os_path_join(m->root, l->url), *ab = os_path_abs(j);
            n.url = arena_strdup(&a, ab);
            free(j);
            free(ab);
        } else {
            n.url = l->url;
        }
        if (p->fetch(p->user, &n, &a) != 0)
            errs++;
    }
    arena_release(&a);
    return errs ? -1 : 0;
}

static char *lock_read_marker(const char *dir)
{
    char *p = os_path_join(dir, PKG_VENDOR_MARKER), *data = NULL, *e;
    size_t len;
    if (os_read_file(p, &data, &len) != 0) {
        free(p);
        return NULL;
    }
    free(p);
    for (e = data; *e && *e != '\n' && *e != '\r'; e++)
        ;
    *e = 0;
    return data;
}

int pkg_dep_roots(const TernManifest *m, PkgDepRoots *out)
{
    PkgLock lock;
    char *lock_path;
    int r, errs = 0;
    size_t i;
    memset(out, 0, sizeof *out);
    if (m->ndeps == 0)
        return 0;
    lock_path = os_path_join(m->root, PKG_LOCK_FILE);
    r = pkg_lock_read(lock_path, &lock);
    free(lock_path);
    if (r < 0)
        return -1;
    if (r == 1) {
        diag_error(m->deps[0].span, "tern.toml has dependencies but there is no tern.lock");
        diag_help("run `tern add` to resolve and fetch the dependencies (it writes tern.lock)");
        pkg_lock_free(&lock);
        return -1;
    }
    if (!pkg_lock_satisfies(m, &lock, 1)) {
        pkg_lock_free(&lock);
        return -1;
    }
    out->items = ANEWN(&out->arena, PkgDepRoot, lock.pkgs.len + 1);
    for (i = 0; i < lock.pkgs.len; i++) {
        const PkgLocked *l = &lock.pkgs.data[i];
        PkgDepRoot *dr = &out->items[out->len];
        char *root = NULL, *id, *srcp;
        memset(dr, 0, sizeof *dr);
        if (l->kind == PKG_SRC_PATH) {
            char *j = os_path_join(m->root, l->url);
            root = os_path_abs(j);
            free(j);
            if (!os_is_dir(root)) {
                diag_error(span_none(), "path dependency `%s` not found at `%s`", l->name, root);
                errs++;
                free(root);
                continue;
            }
        } else {
            char *vd, *marker;
            StrBuf sb = {0};
            sb_printf(&sb, "%s/%s/%s", m->root, PKG_VENDOR_DIR, l->name);
            vd = sb_take(&sb);
            marker = lock_read_marker(vd);
            if (marker && strcmp(marker, l->commit) == 0) {
                root = vd;
                dr->vendored = 1;
            } else if (lock.vendored) {
                diag_error(span_none(), "dependency `%s` is not vendored (or vendor/%s is out of date)", l->name,
                           l->name);
                diag_help("run `tern vendor` to copy the locked dependencies into vendor/");
                errs++;
                free(vd);
            } else {
                char *cd = pkg_cache_dir(l->name, l->commit);
                free(vd);
                if (os_is_dir(cd)) {
                    root = cd;
                } else {
                    diag_error(span_none(), "dependency `%s` (%s, commit %.10s) is not fetched", l->name, l->version,
                               l->commit);
                    diag_help("run `tern add` (fetches the versions in tern.lock) or `tern update`");
                    errs++;
                    free(cd);
                }
            }
            free(marker);
            if (!root)
                continue;
        }
        id = pkg_name_ident(l->name);
        srcp = os_path_join(root, "src");
        dr->name = l->name ? arena_strdup(&out->arena, l->name) : "";
        dr->ident = arena_strdup(&out->arena, id);
        dr->version = arena_strdup(&out->arena, l->version);
        dr->root = arena_strdup(&out->arena, root);
        dr->src = arena_strdup(&out->arena, srcp);
        dr->direct = pkg_manifest_dep(m, l->name) != NULL;
        free(id);
        free(srcp);
        free(root);
        out->len++;
    }
    pkg_lock_free(&lock);
    if (errs) {
        pkg_dep_roots_free(out);
        return -1;
    }
    return 0;
}

void pkg_dep_roots_free(PkgDepRoots *r)
{
    arena_release(&r->arena);
    memset(r, 0, sizeof *r);
}

int pkg_project_open(const char *start_dir, PkgProject *p)
{
    char *root = pkg_find_root(start_dir), *mp;
    memset(p, 0, sizeof *p);
    if (!root)
        return 0;
    mp = os_path_join(root, PKG_MANIFEST_FILE);
    free(root);
    if (pkg_manifest_load(mp, &p->manifest) != 0) {
        free(mp);
        pkg_manifest_free(&p->manifest);
        return -1;
    }
    free(mp);
    if (pkg_dep_roots(&p->manifest, &p->deps) != 0) {
        pkg_manifest_free(&p->manifest);
        return -1;
    }
    return 1;
}

void pkg_project_close(PkgProject *p)
{
    pkg_dep_roots_free(&p->deps);
    pkg_manifest_free(&p->manifest);
}
