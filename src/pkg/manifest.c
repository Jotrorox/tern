/* manifest.c — tern.toml: loading and validation into TernManifest,
 * features (cfg::name), targets, and the textual dependency edit used by
 * `tern add` (which must preserve the rest of the user's file). */
#include "pkg.h"
#include "../os/os.h"

static const char *const man_tern_keywords[] = {
    "package", "use", "pub", "module", "extern", "bundle", "struct", "enum", "const", "fn",
    "type", "regmap", "domain", "where", "test", "property", "sequence", "in", "out", "flip",
    "let", "var", "reg", "mem", "on", "comb", "match", "if", "else", "for", "while", "return",
    "as", "sim", "expect", "assert", "assume", "cover", "true", "false", "std", "cfg",
};

int pkg_name_valid(const char *name)
{
    const char *p;
    if (!name || !(tern_is_alpha((unsigned char)name[0]) || name[0] == '_'))
        return 0;
    for (p = name + 1; *p; p++) {
        if (!tern_is_ident((unsigned char)*p) && *p != '-')
            return 0;
    }
    return 1;
}

char *pkg_name_ident(const char *name)
{
    char *r = xstrdup(name), *p;
    for (p = r; *p; p++) {
        if (*p == '-')
            *p = '_';
    }
    return r;
}

static int man_is_ident(const char *s)
{
    const char *p;
    if (!s || !(tern_is_alpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (p = s + 1; *p; p++) {
        if (!tern_is_ident((unsigned char)*p))
            return 0;
    }
    return 1;
}

static int man_is_keyword(const char *s)
{
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(man_tern_keywords); i++) {
        if (strcmp(man_tern_keywords[i], s) == 0)
            return 1;
    }
    return 0;
}

/* Reports `key` with the wrong type. */
static void man_type_error(const TomlValue *v, const char *where, const char *want)
{
    diag_error(v->span, "%s must be %s, found %s", where, want,
               v->kind == TOML_INT ? "an integer" : v->kind == TOML_BOOL ? "a boolean" :
               v->kind == TOML_ARRAY ? "an array" : v->kind == TOML_TABLE ? "a table" : "a string");
}

/* Optional string member; reports a type error. Returns -1 on error. */
static int man_opt_str(const TomlValue *t, const char *key, const char *where, const char **out)
{
    const TomlValue *v = toml_get(t, key);
    *out = NULL;
    if (!v)
        return 0;
    if (v->kind != TOML_STRING) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s.%s", where, key);
        man_type_error(v, buf, "a string");
        return -1;
    }
    *out = v->str;
    return 0;
}

/* Levenshtein distance (short strings only; larger ones count as far). */
static size_t man_edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i, j;
    size_t row[40];
    if (lb >= TERN_ARRAY_LEN(row))
        return (size_t)-1;
    for (j = 0; j <= lb; j++)
        row[j] = j;
    for (i = 1; i <= la; i++) {
        size_t prev = row[0];
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t cur = row[j];
            size_t sub = prev + (tern_to_lower((unsigned char)a[i - 1]) == tern_to_lower((unsigned char)b[j - 1]) ? 0 : 1);
            size_t del = row[j] + 1, ins = row[j - 1] + 1;
            row[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
            prev = cur;
        }
    }
    return row[lb];
}

static void man_unknown_key(const TomlValue *v, const char *section, const char *const *known, size_t nknown)
{
    size_t i;
    diag_warning(v->key_span, "unknown key `%s` in [%s]", v->key, section);
    for (i = 0; i < nknown; i++) {
        if (man_edit_distance(known[i], v->key) <= 2 || (strlen(v->key) > 2 && strstr(known[i], v->key))) {
            diag_help("did you mean `%s`?", known[i]);
            return;
        }
    }
    {
        StrBuf sb = {0};
        for (i = 0; i < nknown; i++)
            sb_printf(&sb, "%s`%s`", i ? ", " : "", known[i]);
        diag_help("known keys: %s", sb_cstr(&sb));
        sb_free(&sb);
    }
}

static int man_package(TernManifest *m, const TomlValue *pkg)
{
    static const char *const known[] = {"name", "version", "description", "authors", "license", "repository"};
    const TomlValue *v;
    int errs = 0;
    size_t i;
    if (!pkg) {
        diag_error(span_make(m->src, 0, 0), "tern.toml has no [package] section");
        diag_help("add:\n    [package]\n    name = \"my-design\"\n    version = \"0.1.0\"");
        return -1;
    }
    if (pkg->kind != TOML_TABLE) {
        man_type_error(pkg, "`package`", "a table ([package])");
        return -1;
    }
    errs += man_opt_str(pkg, "name", "package", &m->name) != 0;
    errs += man_opt_str(pkg, "version", "package", &m->version) != 0;
    errs += man_opt_str(pkg, "description", "package", &m->description) != 0;
    errs += man_opt_str(pkg, "license", "package", &m->license) != 0;
    errs += man_opt_str(pkg, "repository", "package", &m->repository) != 0;
    if (!m->name && !toml_get(pkg, "name")) {
        diag_error(pkg->span, "[package] has no `name`");
        diag_help("add `name = \"my-design\"`");
        errs++;
    } else if (m->name && !pkg_name_valid(m->name)) {
        diag_error(toml_get(pkg, "name")->span, "invalid package name `%s`", m->name);
        diag_help("package names start with a letter or `_` and contain only letters, digits, `_` and `-`");
        errs++;
    } else if (m->name) {
        char *id = pkg_name_ident(m->name);
        if (man_is_keyword(id)) {
            diag_error(toml_get(pkg, "name")->span, "package name `%s` is a reserved word", m->name);
            diag_help("it would be imported as `use %s::...`; pick another name", id);
            errs++;
        }
        free(id);
    }
    if (!m->version && !toml_get(pkg, "version")) {
        diag_error(pkg->span, "[package] has no `version`");
        diag_help("add `version = \"0.1.0\"` (semantic versioning)");
        errs++;
    } else if (m->version) {
        PkgVersion ver;
        if (pkg_version_parse(m->version, &ver, 0) != 0) {
            diag_error(toml_get(pkg, "version")->span, "invalid version `%s`", m->version);
            diag_help("versions are semantic versions: MAJOR.MINOR.PATCH, e.g. \"1.2.0\"");
            errs++;
        }
    }
    v = toml_get(pkg, "authors");
    if (v) {
        if (v->kind == TOML_STRING) {
            m->authors = ANEWN(&m->arena, const char *, 1);
            m->authors[0] = v->str;
            m->nauthors = 1;
        } else if (v->kind != TOML_ARRAY) {
            man_type_error(v, "package.authors", "an array of strings");
            errs++;
        } else {
            const TomlValue *a;
            m->authors = ANEWN(&m->arena, const char *, v->count + 1);
            for (a = v->first; a; a = a->next) {
                if (a->kind != TOML_STRING) {
                    man_type_error(a, "each entry of package.authors", "a string");
                    errs++;
                    continue;
                }
                m->authors[m->nauthors++] = a->str;
            }
        }
    }
    for (v = pkg->first; v; v = v->next) {
        for (i = 0; i < TERN_ARRAY_LEN(known); i++) {
            if (strcmp(known[i], v->key) == 0)
                break;
        }
        if (i == TERN_ARRAY_LEN(known))
            man_unknown_key(v, "package", known, TERN_ARRAY_LEN(known));
    }
    return errs ? -1 : 0;
}

static int man_dep(TernManifest *m, const TomlValue *v, TernDep *d)
{
    static const char *const known[] = {"version", "git", "tag", "rev", "branch", "path"};
    const TomlValue *e;
    int errs = 0, npin = 0;
    char where[160];
    memset(d, 0, sizeof *d);
    d->name = v->key;
    d->span = v->key_span;
    TERN_UNUSED(m);
    if (!pkg_name_valid(v->key)) {
        diag_error(v->key_span, "invalid dependency name `%s`", v->key);
        diag_help("package names start with a letter or `_` and contain only letters, digits, `_` and `-`");
        return -1;
    }
    snprintf(where, sizeof where, "dependencies.%s", v->key);
    if (v->kind == TOML_STRING) {
        d->kind = PKG_DEP_REGISTRY;
        d->req = v->str;
    } else if (v->kind == TOML_TABLE) {
        errs += man_opt_str(v, "version", where, &d->req) != 0;
        errs += man_opt_str(v, "git", where, &d->git) != 0;
        errs += man_opt_str(v, "tag", where, &d->tag) != 0;
        errs += man_opt_str(v, "rev", where, &d->rev) != 0;
        errs += man_opt_str(v, "branch", where, &d->branch) != 0;
        errs += man_opt_str(v, "path", where, &d->path) != 0;
        for (e = v->first; e; e = e->next) {
            size_t i;
            for (i = 0; i < TERN_ARRAY_LEN(known); i++) {
                if (strcmp(known[i], e->key) == 0)
                    break;
            }
            if (i == TERN_ARRAY_LEN(known)) {
                diag_error(e->key_span, "unknown key `%s` in dependency `%s`", e->key, v->key);
                diag_help("a dependency is \"<version>\", { git = \"<url>\", tag/rev/branch = \"...\" } or { path = \"<dir>\" }");
                errs++;
            }
        }
        if (errs)
            return -1;
        npin = (d->tag != NULL) + (d->rev != NULL) + (d->branch != NULL);
        if (d->git && d->path) {
            diag_error(v->span, "dependency `%s` has both `git` and `path`", v->key);
            diag_help("use one source: { git = \"...\" } or { path = \"...\" }");
            return -1;
        }
        if (npin && !d->git) {
            diag_error(v->span, "`tag`, `rev` and `branch` require `git` (dependency `%s`)", v->key);
            return -1;
        }
        if (npin > 1) {
            diag_error(v->span, "dependency `%s` may specify only one of `tag`, `rev` and `branch`", v->key);
            return -1;
        }
        if (d->git) {
            d->kind = PKG_DEP_GIT;
            if (!d->git[0]) {
                diag_error(toml_get(v, "git")->span, "empty git URL for dependency `%s`", v->key);
                return -1;
            }
        } else if (d->path) {
            d->kind = PKG_DEP_PATH;
        } else {
            d->kind = PKG_DEP_REGISTRY;
            if (!d->req) {
                diag_error(v->span, "dependency `%s` has no source", v->key);
                diag_help("add `version = \"1.0\"` (registry), `git = \"<url>\"` or `path = \"<dir>\"`");
                return -1;
            }
        }
    } else {
        man_type_error(v, where, "a version string or an inline table");
        diag_help("e.g. `%s = \"1.2\"` or `%s = { git = \"https://...\", tag = \"v1.2.0\" }`", v->key, v->key);
        return -1;
    }
    if (d->req) {
        PkgReq r;
        char err[160];
        if (pkg_req_parse(d->req, &r, err, sizeof err) != 0) {
            const TomlValue *rv = v->kind == TOML_STRING ? v : toml_get(v, "version");
            diag_error(rv->span, "invalid version requirement \"%s\" for `%s`: %s", d->req, v->key, err);
            diag_help("examples: \"1.2\" (= ^1.2), \"=1.2.3\", \"~1.2\", \">=1.0, <2\"");
            return -1;
        }
    } else if (d->kind == PKG_DEP_REGISTRY) {
        d->req = "*";
    }
    return 0;
}

static int man_target(TernManifest *m, const TomlValue *v, TernTarget *t)
{
    const TomlValue *e;
    int errs = 0;
    char where[160];
    memset(t, 0, sizeof *t);
    t->name = v->key;
    t->span = v->key_span.file ? v->key_span : v->span;
    snprintf(where, sizeof where, "target.%s", v->key);
    if (v->kind != TOML_TABLE) {
        man_type_error(v, where, "a table ([target.<name>])");
        return -1;
    }
    errs += man_opt_str(v, "device", where, &t->device) != 0;
    errs += man_opt_str(v, "top", where, &t->top) != 0;
    errs += man_opt_str(v, "backend", where, &t->backend) != 0;
    errs += man_opt_str(v, "std", where, &t->std) != 0;
    if (t->std && strcmp(t->std, "sv") != 0 && strcmp(t->std, "verilog2005") != 0) {
        diag_error(toml_get(v, "std")->span, "invalid output dialect \"%s\" in [%s]", t->std, where);
        diag_help("use \"sv\" (SystemVerilog, the default) or \"verilog2005\"");
        errs++;
    }
    if (!t->std)
        t->std = "sv";
    if (t->top && !man_is_ident(t->top) && !strstr(t->top, "::")) {
        diag_error(toml_get(v, "top")->span, "`top` must name a module, found \"%s\"", t->top);
        errs++;
    }
    t->extra = ANEWN(&m->arena, TernTargetKV, v->count + 1);
    for (e = v->first; e; e = e->next) {
        if (strcmp(e->key, "device") == 0 || strcmp(e->key, "top") == 0 || strcmp(e->key, "backend") == 0 ||
            strcmp(e->key, "std") == 0)
            continue;
        t->extra[t->nextra].key = e->key;
        t->extra[t->nextra].value = e;
        t->nextra++;
    }
    return errs ? -1 : 0;
}

int pkg_manifest_parse(uint32_t src_id, const char *root, TernManifest *m)
{
    const SourceFile *f = src_get(src_id);
    const TomlValue *doc, *v, *e;
    int errs = 0;
    char *nroot;
    memset(m, 0, sizeof *m);
    if (!f)
        return -1;
    m->src = src_id;
    m->path = arena_strdup(&m->arena, f->path);
    nroot = os_path_abs(root ? root : ".");
    m->root = arena_strdup(&m->arena, nroot);
    free(nroot);
    doc = toml_parse(&m->arena, src_id);
    if (!doc)
        return -1;
    m->toml = doc;
    for (v = doc->first; v; v = v->next) {
        static const char *const known[] = {"package", "dependencies", "features", "target", "registry"};
        size_t i;
        for (i = 0; i < TERN_ARRAY_LEN(known); i++) {
            if (strcmp(known[i], v->key) == 0)
                break;
        }
        if (i == TERN_ARRAY_LEN(known)) {
            diag_warning(v->key_span, "unknown %s `%s` in tern.toml (ignored)",
                         v->kind == TOML_TABLE ? "section" : "key", v->key);
            if (strcmp(v->key, "dependency") == 0 || strcmp(v->key, "deps") == 0)
                diag_help("did you mean [dependencies]?");
            else if (strcmp(v->key, "targets") == 0 || strcmp(v->key, "feature") == 0)
                diag_help("did you mean [%s]?", strcmp(v->key, "targets") == 0 ? "target.<name>" : "features");
        }
    }
    errs += man_package(m, toml_get(doc, "package")) != 0;

    v = toml_get(doc, "dependencies");
    if (v && v->kind != TOML_TABLE) {
        man_type_error(v, "`dependencies`", "a table ([dependencies])");
        errs++;
    } else if (v) {
        m->deps = ANEWN(&m->arena, TernDep, v->count + 1);
        for (e = v->first; e; e = e->next) {
            if (man_dep(m, e, &m->deps[m->ndeps]) != 0)
                errs++;
            else
                m->ndeps++;
        }
    }

    v = toml_get(doc, "features");
    if (v && v->kind != TOML_TABLE) {
        man_type_error(v, "`features`", "a table ([features])");
        errs++;
    } else if (v) {
        m->features = ANEWN(&m->arena, TernFeature, v->count + 1);
        for (e = v->first; e; e = e->next) {
            TernFeature *ft = &m->features[m->nfeatures];
            if (!man_is_ident(e->key)) {
                diag_error(e->key_span, "feature name `%s` is not a valid identifier", e->key);
                diag_help("features are read as `cfg::<name>`; use letters, digits and `_` (e.g. `%s`)",
                          e->key);
                errs++;
                continue;
            }
            if (man_is_keyword(e->key)) {
                diag_error(e->key_span, "feature name `%s` is a reserved word", e->key);
                errs++;
                continue;
            }
            if (e->kind != TOML_BOOL) {
                char buf[96];
                snprintf(buf, sizeof buf, "feature `%s`", e->key);
                man_type_error(e, buf, "a boolean (its default)");
                diag_help("write `%s = false` or `%s = true`", e->key, e->key);
                errs++;
                continue;
            }
            ft->name = e->key;
            ft->default_on = e->boolean;
            ft->enabled = e->boolean;
            ft->span = e->key_span;
            m->nfeatures++;
        }
    }

    v = toml_get(doc, "target");
    if (v && v->kind != TOML_TABLE) {
        man_type_error(v, "`target`", "a table of targets ([target.<name>])");
        errs++;
    } else if (v) {
        m->targets = ANEWN(&m->arena, TernTarget, v->count + 1);
        for (e = v->first; e; e = e->next) {
            if (man_target(m, e, &m->targets[m->ntargets]) != 0)
                errs++;
            else
                m->ntargets++;
        }
    }

    v = toml_get(doc, "registry");
    if (v && v->kind != TOML_TABLE) {
        man_type_error(v, "`registry`", "a table ([registry])");
        errs++;
    } else if (v) {
        errs += man_opt_str(v, "index", "registry", &m->registry_index) != 0;
        errs += man_opt_str(v, "fork", "registry", &m->registry_fork) != 0;
        for (e = v->first; e; e = e->next) {
            static const char *const known[] = {"index", "fork"};
            if (strcmp(e->key, "index") != 0 && strcmp(e->key, "fork") != 0)
                man_unknown_key(e, "registry", known, 2);
        }
    }
    return errs ? -1 : 0;
}

int pkg_manifest_load(const char *path, TernManifest *m)
{
    char *data, *dir;
    size_t len;
    uint32_t id;
    int r;
    if (os_read_file(path, &data, &len) != 0) {
        memset(m, 0, sizeof *m);
        diag_error(span_none(), "cannot read `%s`", path);
        if (!os_exists(path))
            diag_help("run `tern new <name>` to create a project, or run the command inside one");
        return -1;
    }
    id = src_find(path);
    if (id)
        src_update(id, data, len);
    else
        id = src_add(path, data, len);
    free(data);
    dir = os_path_dirname(path);
    r = pkg_manifest_parse(id, dir, m);
    free(dir);
    return r;
}

void pkg_manifest_free(TernManifest *m)
{
    arena_release(&m->arena);
    memset(m, 0, sizeof *m);
}

char *pkg_find_root(const char *start_dir)
{
    char *dir = os_path_abs(start_dir ? start_dir : ".");
    for (;;) {
        char *p = os_path_join(dir, PKG_MANIFEST_FILE);
        char *up;
        int found = os_is_file(p);
        free(p);
        if (found)
            return dir;
        up = os_path_dirname(dir);
        if (strcmp(up, dir) == 0 || !up[0] || strcmp(up, ".") == 0) {
            free(up);
            free(dir);
            return NULL;
        }
        free(dir);
        dir = up;
    }
}

const TernDep *pkg_manifest_dep(const TernManifest *m, const char *name)
{
    size_t i;
    for (i = 0; i < m->ndeps; i++) {
        if (strcmp(m->deps[i].name, name) == 0)
            return &m->deps[i];
    }
    return NULL;
}

const TernTarget *pkg_manifest_target(const TernManifest *m, const char *name)
{
    size_t i;
    if (!name) {
        if (m->ntargets == 1)
            return &m->targets[0];
        name = "default";
    }
    for (i = 0; i < m->ntargets; i++) {
        if (strcmp(m->targets[i].name, name) == 0)
            return &m->targets[i];
    }
    return NULL;
}

int pkg_manifest_feature(const TernManifest *m, const char *name, int *value)
{
    size_t i;
    for (i = 0; i < m->nfeatures; i++) {
        if (strcmp(m->features[i].name, name) == 0) {
            if (value)
                *value = m->features[i].enabled;
            return 1;
        }
    }
    return 0;
}

int pkg_features_apply(TernManifest *m, const char *list)
{
    const char *p = list;
    int errs = 0;
    while (p && *p) {
        const char *e = p;
        char name[128];
        size_t n, i;
        int on = 1;
        while (*e && *e != ',' && *e != ' ')
            e++;
        n = (size_t)(e - p);
        if (n && (*p == '-' || *p == '!')) {
            on = 0;
            p++;
            n--;
        } else if (n > 3 && strncmp(p, "no-", 3) == 0) {
            on = 0;
            p += 3;
            n -= 3;
        }
        if (n >= sizeof name)
            n = sizeof name - 1;
        memcpy(name, p, n);
        name[n] = 0;
        if (n) {
            for (i = 0; i < m->nfeatures; i++) {
                if (strcmp(m->features[i].name, name) == 0) {
                    m->features[i].enabled = on;
                    break;
                }
            }
            if (i == m->nfeatures) {
                StrBuf sb = {0};
                diag_error(span_none(), "unknown feature `%s`", name);
                for (i = 0; i < m->nfeatures; i++)
                    sb_printf(&sb, "%s`%s`", i ? ", " : "", m->features[i].name);
                if (m->nfeatures)
                    diag_help("features declared in [features]: %s", sb_cstr(&sb));
                else
                    diag_help("declare it in tern.toml: [features] %s = false", name);
                sb_free(&sb);
                errs++;
            }
        }
        p = e;
        while (*p == ',' || *p == ' ')
            p++;
    }
    return errs ? -1 : 0;
}

/* --------------------------------------------------- textual dep edits -- */

typedef struct ManLine {
    size_t start, end, next;   /* [start,end) is the text without the line terminator */
} ManLine;

/* Normalized header name of a line ("dependencies", "target.arty"), or 0
 * if the line is not a table header. Array-of-table headers give "[name". */
static int man_header_name(const char *s, size_t start, size_t end, char *out, size_t outsz)
{
    size_t p = start, o = 0;
    int aot = 0;
    while (p < end && (s[p] == ' ' || s[p] == '\t'))
        p++;
    if (p >= end || s[p] != '[')
        return 0;
    p++;
    if (p < end && s[p] == '[') {
        aot = 1;
        p++;
        if (o + 1 < outsz)
            out[o++] = '[';
    }
    while (p < end && s[p] != ']') {
        char c = s[p++];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'')
            continue;
        if (o + 1 < outsz)
            out[o++] = c;
    }
    out[o] = 0;
    TERN_UNUSED(aot);
    return p < end;
}

/* Does the line define key `name` (`name =`, `"name" =`)? */
static int man_line_defines(const char *s, size_t start, size_t end, const char *name)
{
    size_t p = start, n = strlen(name);
    char q = 0;
    while (p < end && (s[p] == ' ' || s[p] == '\t'))
        p++;
    if (p < end && (s[p] == '"' || s[p] == '\''))
        q = s[p++];
    if (p + n > end || memcmp(s + p, name, n) != 0)
        return 0;
    p += n;
    if (q) {
        if (p >= end || s[p] != q)
            return 0;
        p++;
    } else if (p < end && (tern_is_ident((unsigned char)s[p]) || s[p] == '-' || s[p] == '.')) {
        return 0;
    }
    while (p < end && (s[p] == ' ' || s[p] == '\t'))
        p++;
    return p < end && s[p] == '=';
}

int pkg_manifest_edit_dep(const char *text, size_t len, const char *name, const char *value_toml, StrBuf *out,
                          char **err)
{
    VEC(ManLine) lines = {0};
    size_t i, p = 0, sec = (size_t)-1, sec_end = 0, found = (size_t)-1, insert_after;
    const char *nl = "\n";
    StrBuf line = {0};
    char hdr[256], sub[300];
    *err = NULL;
    while (p < len) {
        ManLine l;
        l.start = p;
        while (p < len && text[p] != '\n')
            p++;
        l.end = p > l.start && text[p - 1] == '\r' ? p - 1 : p;
        if (p < len) {
            if (l.end < p)
                nl = "\r\n";
            p++;
        }
        l.next = p;
        vec_push(lines, l);
    }
    snprintf(sub, sizeof sub, "dependencies.%s", name);
    for (i = 0; i < lines.len; i++) {
        if (!man_header_name(text, lines.data[i].start, lines.data[i].end, hdr, sizeof hdr))
            continue;
        if (sec != (size_t)-1 && sec_end == 0)
            sec_end = i;
        if (strcmp(hdr, "dependencies") == 0) {
            sec = i;
            sec_end = 0;
        } else if (strcmp(hdr, sub) == 0) {
            StrBuf m = {0};
            sb_printf(&m, "`%s` is declared as a [%s] table on line %u; edit tern.toml by hand", name, sub,
                      (unsigned)(i + 1));
            *err = sb_take(&m);
            vec_free(lines);
            return -1;
        }
    }
    if (sec != (size_t)-1 && sec_end == 0)
        sec_end = lines.len;
    toml_put_key(&line, name);
    sb_puts(&line, " = ");
    sb_puts(&line, value_toml);
    if (sec == (size_t)-1) {
        sb_append(out, text, len);
        if (len && text[len - 1] != '\n')
            sb_puts(out, nl);
        if (len && !(len >= 2 && text[len - 1] == '\n' && (text[len - 2] == '\n' ||
                                                           (len >= 3 && text[len - 2] == '\r' && text[len - 3] == '\n'))))
            sb_puts(out, nl);
        sb_printf(out, "[dependencies]%s%s%s", nl, sb_cstr(&line), nl);
        sb_free(&line);
        vec_free(lines);
        return 0;
    }
    for (i = sec + 1; i < sec_end; i++) {
        if (man_line_defines(text, lines.data[i].start, lines.data[i].end, name)) {
            found = i;
            break;
        }
    }
    if (found != (size_t)-1) {
        const ManLine *l = &lines.data[found];
        size_t ind = l->start;
        while (ind < l->end && (text[ind] == ' ' || text[ind] == '\t'))
            ind++;
        sb_append(out, text, ind);
        sb_puts(out, sb_cstr(&line));
        /* keep a trailing comment */
        {
            size_t q = ind;
            int instr = 0;
            char qc = 0;
            for (; q < l->end; q++) {
                char c = text[q];
                if (instr) {
                    if (c == '\\' && qc == '"')
                        q++;
                    else if (c == qc)
                        instr = 0;
                } else if (c == '"' || c == '\'') {
                    instr = 1;
                    qc = c;
                } else if (c == '#') {
                    sb_puts(out, " ");
                    sb_append(out, text + q, l->end - q);
                    break;
                }
            }
        }
        sb_append(out, text + l->end, len - l->end);
        sb_free(&line);
        vec_free(lines);
        return 0;
    }
    insert_after = sec;
    for (i = sec + 1; i < sec_end; i++) {
        size_t q = lines.data[i].start;
        while (q < lines.data[i].end && (text[q] == ' ' || text[q] == '\t'))
            q++;
        if (q < lines.data[i].end) /* after the last entry or comment of the section */
            insert_after = i;
    }
    {
        const ManLine *l = &lines.data[insert_after];
        sb_append(out, text, l->next);
        if (l->next == len && (len == 0 || text[len - 1] != '\n'))
            sb_puts(out, nl);
        sb_puts(out, sb_cstr(&line));
        sb_puts(out, nl);
        sb_append(out, text + l->next, len - l->next);
    }
    sb_free(&line);
    vec_free(lines);
    return 0;
}
