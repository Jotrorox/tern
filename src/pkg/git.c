/* git.c — every git operation, done by running the system `git` (spec
 * §13.6: no git library): tag listing, checkouts into the package cache,
 * the shallow index clone, index entries, and the PkgProvider used by the
 * package commands. */
#include "pkg.h"
#include "../os/os.h"

static int git_quiet;

void pkg_set_quiet(int quiet)
{
    git_quiet = quiet;
}

int pkg_quiet(void)
{
    return git_quiet;
}

void pkg_status(const char *verb, const char *fmt, ...)
{
    va_list ap;
    int color;
    if (git_quiet)
        return;
    diag_flush();
    fflush(stdout);
    color = os_color_enabled(2);
    fprintf(stderr, "%s%12s%s ", color ? "\033[1;32m" : "", verb, color ? "\033[0m" : "");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

int pkg_git_available(void)
{
    char *p = os_find_program("git");
    int ok = p != NULL;
    free(p);
    return ok;
}

static void git_trim(char *s)
{
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = 0;
}

int pkg_git(const char *cwd, const char *const *args, char **out, char **err)
{
    char *argv[48];
    size_t n = 0;
    OsSpawnOpts o;
    OsRunResult res;
    int rc;
    if (out)
        *out = NULL;
    if (err)
        *err = NULL;
    argv[n++] = (char *)"git";
    while (args[n - 1] && n < TERN_ARRAY_LEN(argv) - 1) {
        argv[n] = (char *)args[n - 1];
        n++;
    }
    argv[n] = NULL;
    memset(&o, 0, sizeof o);
    memset(&res, 0, sizeof res);
    o.cwd = cwd;
    o.stdin_null = 1;
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    rc = os_run(argv, &o, &res);
    if (rc < 0) {
        if (err)
            *err = xstrdup("cannot run `git`: is it installed and in PATH?");
        os_run_result_free(&res);
        return -1;
    }
    if (out) {
        *out = res.out ? res.out : xstrdup("");
        res.out = NULL;
        git_trim(*out);
    }
    if (err) {
        *err = res.err ? res.err : xstrdup("");
        res.err = NULL;
        git_trim(*err);
    }
    os_run_result_free(&res);
    return rc;
}

/* Reports a failed git command with its stderr. */
static void git_report(const char *what, const char *errtext)
{
    diag_error(span_none(), "%s", what);
    if (errtext && errtext[0]) {
        /* indent git's output as a note */
        diag_note(span_none(), "git said:\n        %s", errtext);
    }
}

static int git_require(void)
{
    if (pkg_git_available())
        return 0;
    diag_error(span_none(), "`git` was not found in PATH");
    diag_help("install git (https://git-scm.com); only `tern add`, `update`, `vendor` and `publish` need it");
    return -1;
}

int pkg_git_tags(const char *url, Arena *a, PkgAvailList *out)
{
    const char *args[] = {"ls-remote", "--tags", url, NULL};
    char *o = NULL, *e = NULL, *line;
    int rc;
    memset(out, 0, sizeof *out);
    if (git_require() != 0)
        return -1;
    rc = pkg_git(NULL, args, &o, &e);
    if (rc != 0) {
        StrBuf m = {0};
        sb_printf(&m, "cannot list the tags of %s", url);
        git_report(sb_cstr(&m), e);
        diag_help("check the URL and your network connection / credentials");
        sb_free(&m);
        free(o);
        free(e);
        return -1;
    }
    line = o;
    while (line && *line) {
        char *nl = strchr(line, '\n'), *tab, *ref;
        size_t rlen;
        int peeled = 0;
        if (nl)
            *nl = 0;
        tab = strchr(line, '\t');
        if (tab && tern_starts_with(tab + 1, "refs/tags/")) {
            PkgVersion v;
            ref = tab + 11;
            rlen = strlen(ref);
            if (rlen > 3 && strcmp(ref + rlen - 3, "^{}") == 0) {
                ref[rlen - 3] = 0;
                peeled = 1;
            }
            if (pkg_version_parse(ref, &v, 1) == 0) {
                size_t i;
                *tab = 0;
                for (i = 0; i < out->items.len; i++) {
                    if (pkg_version_cmp(&out->items.data[i].version, &v) == 0)
                        break;
                }
                if (i < out->items.len) {
                    if (peeled && strcmp(out->items.data[i].tag, ref) == 0)
                        out->items.data[i].commit = arena_strdup(a, line);
                } else {
                    PkgAvail av;
                    memset(&av, 0, sizeof av);
                    av.version = v;
                    av.tag = arena_strdup(a, ref);
                    av.commit = arena_strdup(a, line);
                    vec_push(out->items, av);
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    out->git = arena_strdup(a, url);
    free(o);
    free(e);
    return 0;
}

int pkg_git_fetch_into(const char *url, const char *tag, const char *branch, const char *rev, const char *dest,
                       char **commit_out)
{
    char *o = NULL, *e = NULL, *parent, *gitdir;
    int rc;
    *commit_out = NULL;
    if (git_require() != 0)
        return -1;
    os_remove_all(dest);
    parent = os_path_dirname(dest);
    os_mkdir_p(parent);
    free(parent);
    if (tag || branch) {
        const char *args[] = {"-c", "advice.detachedHead=false", "clone", "-q", "--depth", "1", "--branch",
                              tag ? tag : branch, url, dest, NULL};
        rc = pkg_git(NULL, args, NULL, &e);
    } else if (rev) {
        const char *init[] = {"init", "-q", dest, NULL};
        const char *fetch[] = {"fetch", "-q", "--depth", "1", url, rev, NULL};
        const char *co[] = {"-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD", NULL};
        rc = pkg_git(NULL, init, NULL, &e);
        free(e);
        e = NULL;
        if (rc == 0)
            rc = pkg_git(dest, fetch, NULL, &e);
        if (rc == 0) {
            free(e);
            e = NULL;
            rc = pkg_git(dest, co, NULL, &e);
        } else {
            /* the server does not serve arbitrary commits: full clone */
            const char *clone[] = {"clone", "-q", "--no-checkout", url, dest, NULL};
            const char *co2[] = {"-c", "advice.detachedHead=false", "checkout", "-q", rev, NULL};
            os_remove_all(dest);
            free(e);
            e = NULL;
            rc = pkg_git(NULL, clone, NULL, &e);
            if (rc == 0) {
                free(e);
                e = NULL;
                rc = pkg_git(dest, co2, NULL, &e);
            }
        }
    } else {
        const char *args[] = {"clone", "-q", "--depth", "1", url, dest, NULL};
        rc = pkg_git(NULL, args, NULL, &e);
    }
    if (rc != 0) {
        StrBuf m = {0};
        sb_printf(&m, "cannot fetch %s", url);
        if (tag)
            sb_printf(&m, " at tag %s", tag);
        else if (branch)
            sb_printf(&m, " at branch %s", branch);
        else if (rev)
            sb_printf(&m, " at commit %s", rev);
        git_report(sb_cstr(&m), e);
        sb_free(&m);
        free(e);
        os_remove_all(dest);
        return -1;
    }
    free(e);
    e = NULL;
    {
        const char *args[] = {"rev-parse", "HEAD", NULL};
        rc = pkg_git(dest, args, &o, &e);
    }
    if (rc != 0 || !o || !o[0]) {
        git_report("cannot read the checked-out commit", e);
        free(o);
        free(e);
        os_remove_all(dest);
        return -1;
    }
    free(e);
    gitdir = os_path_join(dest, ".git");
    os_remove_all(gitdir);
    free(gitdir);
    *commit_out = o;
    return 0;
}

int pkg_index_sync(const char *index_url)
{
    char *dir = pkg_index_dir(), *gitdir = os_path_join(dir, ".git"), *o = NULL, *e = NULL;
    int rc;
    if (git_require() != 0) {
        free(dir);
        free(gitdir);
        return -1;
    }
    if (os_is_dir(gitdir)) {
        const char *geturl[] = {"remote", "get-url", "origin", NULL};
        const char *fetch[] = {"fetch", "-q", "--depth", "1", "origin", "HEAD", NULL};
        const char *co[] = {"-c", "advice.detachedHead=false", "checkout", "-q", "--force", "--detach", "FETCH_HEAD",
                            NULL};
        rc = pkg_git(dir, geturl, &o, NULL);
        if (rc != 0 || strcmp(o, index_url) != 0) {
            const char *seturl[] = {"remote", "set-url", "origin", index_url, NULL};
            const char *addurl[] = {"remote", "add", "origin", index_url, NULL};
            if (pkg_git(dir, rc == 0 ? seturl : addurl, NULL, NULL) != 0) {
                diag_error(span_none(), "cannot point the index clone at %s", index_url);
                free(o);
                free(dir);
                free(gitdir);
                return -1;
            }
        }
        free(o);
        rc = pkg_git(dir, fetch, NULL, &e);
        if (rc == 0) {
            free(e);
            e = NULL;
            rc = pkg_git(dir, co, NULL, &e);
        }
        if (rc != 0) {
            StrBuf m = {0};
            sb_printf(&m, "cannot update the registry index from %s", index_url);
            git_report(sb_cstr(&m), e);
            diag_help("check your network connection, or set TERN_INDEX / [registry] index to a reachable index; "
                      "deleting %s forces a fresh clone", dir);
            sb_free(&m);
            free(e);
            free(dir);
            free(gitdir);
            return -1;
        }
    } else {
        const char *clone[] = {"clone", "-q", "--depth", "1", index_url, dir, NULL};
        char *home = pkg_home_dir();
        os_mkdir_p(home);
        free(home);
        os_remove_all(dir);
        rc = pkg_git(NULL, clone, NULL, &e);
        if (rc != 0) {
            StrBuf m = {0};
            sb_printf(&m, "cannot clone the registry index %s", index_url);
            git_report(sb_cstr(&m), e);
            diag_help("check your network connection, or set TERN_INDEX / [registry] index to a reachable index");
            sb_free(&m);
            free(e);
            free(dir);
            free(gitdir);
            return -1;
        }
    }
    free(e);
    free(dir);
    free(gitdir);
    return 0;
}

/* --------------------------------------------------------------- index -- */

char *pkg_index_rel_path(const char *name)
{
    StrBuf sb = {0};
    if (strlen(name) < 2)
        sb_printf(&sb, "1/%s.toml", name);
    else
        sb_printf(&sb, "%c%c/%s.toml", tern_to_lower((unsigned char)name[0]), tern_to_lower((unsigned char)name[1]),
                  name);
    return sb_take(&sb);
}

int pkg_index_read(const char *index_dir, const char *name, PkgIndexEntry *e)
{
    char *rel = pkg_index_rel_path(name), *path = os_path_join(index_dir, rel);
    Arena tmp = {0};
    const TomlValue *doc, *v, *it;
    int errs = 0;
    memset(e, 0, sizeof *e);
    free(rel);
    if (!os_is_file(path)) {
        free(path);
        return 1;
    }
    doc = toml_parse_file(&tmp, path, NULL);
    if (!doc) {
        free(path);
        arena_release(&tmp);
        return -1;
    }
    v = toml_get(doc, "name");
    if (!v || v->kind != TOML_STRING || strcmp(v->str, name) != 0) {
        diag_error(v ? v->span : span_make(src_find(path), 0, 0), "index entry `%s` must have name = \"%s\"", path,
                   name);
        errs++;
    } else {
        e->name = arena_strdup(&e->arena, v->str);
    }
    v = toml_get(doc, "git");
    if (!v || v->kind != TOML_STRING || !v->str[0]) {
        diag_error(v ? v->span : span_make(src_find(path), 0, 0), "index entry `%s` has no `git` URL", path);
        errs++;
    } else {
        e->git = arena_strdup(&e->arena, v->str);
    }
    v = toml_get(doc, "description");
    if (v && v->kind == TOML_STRING)
        e->description = arena_strdup(&e->arena, v->str);
    v = toml_get(doc, "version");
    for (it = v && v->kind == TOML_ARRAY ? v->first : NULL; it; it = it->next) {
        const TomlValue *ver = toml_get(it, "version"), *tag = toml_get(it, "tag"), *commit = toml_get(it, "commit"),
                        *yanked = toml_get(it, "yanked");
        PkgAvail av;
        memset(&av, 0, sizeof av);
        if (!ver || ver->kind != TOML_STRING || pkg_version_parse(ver->str, &av.version, 0) != 0) {
            diag_error(ver ? ver->span : it->span, "invalid `version` in index entry `%s`", path);
            errs++;
            continue;
        }
        if (tag && tag->kind == TOML_STRING)
            av.tag = arena_strdup(&e->arena, tag->str);
        else
            av.tag = arena_printf(&e->arena, "v%s", ver->str);
        if (commit && commit->kind == TOML_STRING && commit->str[0])
            av.commit = arena_strdup(&e->arena, commit->str);
        av.yanked = yanked && yanked->kind == TOML_BOOL && yanked->bval;
        vec_push(e->versions.items, av);
    }
    e->versions.git = e->git;
    e->versions.description = e->description;
    free(path);
    arena_release(&tmp);
    if (errs) {
        pkg_index_entry_free(e);
        return -1;
    }
    return 0;
}

static int git_avail_cmp(const void *a, const void *b)
{
    return pkg_version_cmp(&((const PkgAvail *)a)->version, &((const PkgAvail *)b)->version);
}

void pkg_index_format(const PkgIndexEntry *e, StrBuf *out)
{
    size_t i;
    PkgAvail *sorted = (PkgAvail *)xmalloc(sizeof(PkgAvail) * (e->versions.items.len + 1));
    if (e->versions.items.len)
        memcpy(sorted, e->versions.items.data, sizeof(PkgAvail) * e->versions.items.len);
    qsort(sorted, e->versions.items.len, sizeof(PkgAvail), git_avail_cmp);
    sb_puts(out, "name = ");
    toml_put_string(out, e->name);
    sb_puts(out, "\ngit = ");
    toml_put_string(out, e->git);
    if (e->description) {
        sb_puts(out, "\ndescription = ");
        toml_put_string(out, e->description);
    }
    sb_puts(out, "\n");
    for (i = 0; i < e->versions.items.len; i++) {
        char vb[PKG_VERSION_STRLEN];
        sb_puts(out, "\n[[version]]\nversion = ");
        toml_put_string(out, pkg_version_str(&sorted[i].version, vb));
        sb_puts(out, "\ntag = ");
        toml_put_string(out, sorted[i].tag);
        if (sorted[i].commit) {
            sb_puts(out, "\ncommit = ");
            toml_put_string(out, sorted[i].commit);
        }
        if (sorted[i].yanked)
            sb_puts(out, "\nyanked = true");
        sb_puts(out, "\n");
    }
    free(sorted);
}

void pkg_index_entry_free(PkgIndexEntry *e)
{
    vec_free(e->versions.items);
    arena_release(&e->arena);
    memset(e, 0, sizeof *e);
}

/* ------------------------------------------------------------ provider -- */

static int git_prov_registry(void *user, const char *name, Arena *a, PkgAvailList *out)
{
    PkgGitProvider *ctx = (PkgGitProvider *)user;
    PkgIndexEntry e;
    char *dir;
    int r;
    size_t i;
    memset(out, 0, sizeof *out);
    if (!ctx->index_synced) {
        if (ctx->offline) {
            char *d = pkg_index_dir();
            int ok = os_is_dir(d);
            free(d);
            if (!ok) {
                diag_error(span_none(), "the registry index has not been fetched yet");
                diag_help("run `tern update` with network access");
                return -1;
            }
        } else {
            pkg_status("Updating", "index %s", ctx->index_url);
            if (pkg_index_sync(ctx->index_url) != 0)
                return -1;
        }
        ctx->index_synced = 1;
    }
    dir = pkg_index_dir();
    r = pkg_index_read(dir, name, &e);
    free(dir);
    if (r != 0)
        return r;
    out->git = arena_strdup(a, e.git);
    out->description = e.description ? arena_strdup(a, e.description) : NULL;
    for (i = 0; i < e.versions.items.len; i++) {
        PkgAvail av = e.versions.items.data[i];
        av.tag = arena_strdup(a, av.tag);
        av.commit = av.commit ? arena_strdup(a, av.commit) : NULL;
        vec_push(out->items, av);
    }
    pkg_index_entry_free(&e);
    return 0;
}

static int git_prov_tags(void *user, const char *url, Arena *a, PkgAvailList *out)
{
    TERN_UNUSED(user);
    return pkg_git_tags(url, a, out);
}

static char *git_vendor_marker(const char *project_root, const char *name, char **dir_out)
{
    StrBuf sb = {0};
    char *dir, *mp, *data = NULL, *p;
    size_t len;
    sb_printf(&sb, "%s/%s/%s", project_root, PKG_VENDOR_DIR, name);
    dir = sb_take(&sb);
    mp = os_path_join(dir, PKG_VENDOR_MARKER);
    if (os_read_file(mp, &data, &len) == 0) {
        for (p = data; *p && *p != '\n' && *p != '\r'; p++)
            ;
        *p = 0;
    }
    free(mp);
    *dir_out = dir;
    return data;
}

static int git_prov_fetch(void *user, PkgNode *node, Arena *a)
{
    PkgGitProvider *ctx = (PkgGitProvider *)user;
    char *commit = NULL, *tmp, *final, *home;
    const char *tag = node->tag, *branch = node->branch, *rev = node->rev;
    StrBuf sb = {0};
    if (node->kind == PKG_SRC_PATH) {
        if (!os_is_dir(node->url)) {
            diag_error(span_none(), "path dependency `%s` not found: `%s` is not a directory", node->name, node->url);
            return -1;
        }
        node->root = node->url;
        return 0;
    }
    if (!node->commit && ctx->hint_commit && tag && ctx->hint_tag && node->url && ctx->hint_url &&
        strcmp(node->url, ctx->hint_url) == 0 && strcmp(tag, ctx->hint_tag) == 0)
        node->commit = arena_strdup(a, ctx->hint_commit);
    if (node->commit) {
        char *cd = pkg_cache_dir(node->name, node->commit);
        if (os_is_dir(cd)) {
            node->root = arena_strdup(a, cd);
            free(cd);
            return 0;
        }
        free(cd);
        if (ctx->project_root) {
            char *vdir, *marker = git_vendor_marker(ctx->project_root, node->name, &vdir);
            int hit = marker && strcmp(marker, node->commit) == 0;
            free(marker);
            if (hit) {
                node->root = arena_strdup(a, vdir);
                free(vdir);
                return 0;
            }
            free(vdir);
        }
        if (!tag && !branch)
            rev = node->commit;
        else if (branch) {
            /* keep the locked commit of a branch pin */
            branch = NULL;
            rev = node->commit;
        }
    }
    if (node->version)
        pkg_status("Fetching", "%s v%s (%s)", node->name, node->version, node->url);
    else
        pkg_status("Fetching", "%s (%s%s%s)", node->name, node->url, tag ? " " : branch ? " branch " : rev ? " " : "",
                   tag ? tag : branch ? branch : rev ? rev : "");
    home = pkg_home_dir();
    sb_printf(&sb, "%s/cache/%s/.tmp-%llu", home, node->name, (unsigned long long)os_time_ns());
    free(home);
    tmp = sb_take(&sb);
    if (pkg_git_fetch_into(node->url, tag, branch, rev, tmp, &commit) != 0) {
        free(tmp);
        return -1;
    }
    if (node->commit && strcmp(node->commit, commit) != 0) {
        diag_error(span_none(), "%s %s of `%s` now points to commit %.12s, but %.12s is recorded", tag ? "tag" : "ref",
                   tag ? tag : rev, node->name, commit, node->commit);
        diag_note(span_none(), "the tag was moved after it was %s", node->kind == PKG_SRC_REGISTRY ? "published to the index" : "locked");
        if (node->kind == PKG_SRC_REGISTRY)
            diag_help("ask the maintainer to publish a new version instead of moving tags");
        else
            diag_help("run `tern update %s` to accept the new commit", node->name);
        os_remove_all(tmp);
        free(tmp);
        free(commit);
        return -1;
    }
    final = pkg_cache_dir(node->name, commit);
    if (os_is_dir(final)) {
        os_remove_all(tmp);
    } else if (os_rename(tmp, final) != 0) {
        diag_error(span_none(), "cannot move the checkout of `%s` into %s", node->name, final);
        os_remove_all(tmp);
        free(tmp);
        free(final);
        free(commit);
        return -1;
    }
    node->root = arena_strdup(a, final);
    node->commit = arena_strdup(a, commit);
    free(tmp);
    free(final);
    free(commit);
    return 0;
}

PkgProvider pkg_git_provider(PkgGitProvider *ctx)
{
    PkgProvider p;
    p.user = ctx;
    p.registry = git_prov_registry;
    p.tags = git_prov_tags;
    p.fetch = git_prov_fetch;
    return p;
}
