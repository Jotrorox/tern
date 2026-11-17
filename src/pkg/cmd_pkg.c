/* cmd_pkg.c — `tern new`, `tern add`, `tern update`, `tern vendor` and
 * `tern publish` (spec §13.2, §13.6). Documented in docs/packages.md. */
#include "pkg.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../cmds.h"

/* ------------------------------------------------------------ helpers -- */

typedef struct CpkgProject {
    char *root;              /* absolute project directory */
    char *manifest_path;
    char *lock_path;
    TernManifest m;
} CpkgProject;

static int cpkg_open(CpkgProject *p)
{
    memset(p, 0, sizeof *p);
    p->root = pkg_find_root(".");
    if (!p->root) {
        diag_error(span_none(), "no tern.toml in the current directory or any parent directory");
        diag_help("run `tern new <name>` to create a project, or `cd` into one");
        return -1;
    }
    p->manifest_path = os_path_join(p->root, PKG_MANIFEST_FILE);
    p->lock_path = os_path_join(p->root, PKG_LOCK_FILE);
    {
        /* diagnostics name the file relative to the current directory */
        char *cwd = os_getcwd(), *shown = cwd ? pkg_path_rel(p->manifest_path, cwd) : xstrdup(p->manifest_path);
        int r = pkg_manifest_load(shown, &p->m);
        free(shown);
        free(cwd);
        if (r != 0)
            return -1;
    }
    return 0;
}

static void cpkg_close(CpkgProject *p)
{
    pkg_manifest_free(&p->m);
    free(p->root);
    free(p->manifest_path);
    free(p->lock_path);
    memset(p, 0, sizeof *p);
}

static int cpkg_is_url(const char *s)
{
    const char *at = strchr(s, '@'), *colon = strchr(s, ':'), *slash = strchr(s, '/');
    if (strstr(s, "://"))
        return 1;
    if (at && colon && at < colon && (!slash || colon < slash))
        return 1; /* scp-like: user@host:path */
    return tern_ends_with(s, ".git");
}

/* "https://github.com/acme/tern-uart.git" -> "tern-uart" */
static char *cpkg_url_basename(const char *url)
{
    const char *e = url + strlen(url), *b;
    char *r, *p;
    while (e > url && e[-1] == '/')
        e--;
    if (e - url > 4 && memcmp(e - 4, ".git", 4) == 0)
        e -= 4;
    b = e;
    while (b > url && b[-1] != '/' && b[-1] != ':')
        b--;
    r = xstrndup(b, (size_t)(e - b));
    for (p = r; *p; p++) {
        if (!tern_is_ident((unsigned char)*p) && *p != '-')
            *p = '-';
    }
    if (!r[0] || !pkg_name_valid(r)) {
        free(r);
        r = xstrdup("dep");
    }
    return r;
}

static void cpkg_print_changes(const PkgLock *prev, const PkgLock *next)
{
    size_t i;
    int changes = 0;
    for (i = 0; i < next->pkgs.len; i++) {
        const PkgLocked *n = &next->pkgs.data[i];
        const PkgLocked *o = prev ? pkg_lock_find(prev, n->name) : NULL;
        if (!o) {
            pkg_status("Locking", "%s v%s", n->name, n->version);
            changes++;
        } else if (strcmp(o->version, n->version) != 0) {
            pkg_status("Updating", "%s v%s -> v%s", n->name, o->version, n->version);
            changes++;
        } else if (!(o->commit == NULL && n->commit == NULL) &&
                   !(o->commit && n->commit && strcmp(o->commit, n->commit) == 0)) {
            pkg_status("Updating", "%s v%s (%.10s -> %.10s)", n->name, n->version, o->commit ? o->commit : "-",
                       n->commit ? n->commit : "-");
            changes++;
        }
    }
    for (i = 0; prev && i < prev->pkgs.len; i++) {
        if (!pkg_lock_find(next, prev->pkgs.data[i].name)) {
            pkg_status("Removing", "%s v%s", prev->pkgs.data[i].name, prev->pkgs.data[i].version);
            changes++;
        }
    }
    if (!changes)
        pkg_status("Locked", "%lu package%s, no changes", (unsigned long)next->pkgs.len, next->pkgs.len == 1 ? "" : "s");
}

/* --------------------------------------------------------------- vendor -- */

static int cpkg_copy_tree(const char *from, const char *to)
{
    OsDirList dl;
    size_t i;
    int errs = 0;
    if (os_mkdir_p(to) != 0)
        return -1;
    if (os_list_dir(from, &dl) != 0)
        return -1;
    for (i = 0; i < dl.len; i++) {
        char *a = os_path_join(from, dl.items[i].name), *b = os_path_join(to, dl.items[i].name);
        if (dl.items[i].is_dir) {
            if (!dl.items[i].is_link && strcmp(dl.items[i].name, ".git") != 0)
                errs += cpkg_copy_tree(a, b) != 0;
        } else if (os_copy_file(a, b) != 0) {
            errs++;
        }
        free(a);
        free(b);
    }
    os_dir_list_free(&dl);
    return errs ? -1 : 0;
}

/* Copies tern.toml, src/, tests/, LICENSE*, README* of a package. */
static int cpkg_copy_package(const char *from, const char *to)
{
    OsDirList dl;
    size_t i;
    int errs = 0;
    if (os_mkdir_p(to) != 0 || os_list_dir(from, &dl) != 0)
        return -1;
    for (i = 0; i < dl.len; i++) {
        const char *nm = dl.items[i].name;
        char lower[16];
        size_t k;
        char *a, *b;
        for (k = 0; k + 1 < sizeof lower && nm[k]; k++)
            lower[k] = (char)tern_to_lower((unsigned char)nm[k]);
        lower[k] = 0;
        a = os_path_join(from, nm);
        b = os_path_join(to, nm);
        if (dl.items[i].is_dir) {
            if (strcmp(nm, "src") == 0 || strcmp(nm, "tests") == 0)
                errs += cpkg_copy_tree(a, b) != 0;
        } else if (strcmp(nm, PKG_MANIFEST_FILE) == 0 || tern_starts_with(lower, "license") ||
                   tern_starts_with(lower, "licence") || tern_starts_with(lower, "copying") ||
                   tern_starts_with(lower, "readme")) {
            errs += os_copy_file(a, b) != 0;
        }
        free(a);
        free(b);
    }
    os_dir_list_free(&dl);
    return errs ? -1 : 0;
}

static char *cpkg_marker(const char *dir)
{
    char *mp = os_path_join(dir, PKG_VENDOR_MARKER), *data = NULL, *p;
    size_t len;
    int r = os_read_file(mp, &data, &len);
    free(mp);
    if (r != 0)
        return NULL;
    for (p = data; *p && *p != '\n' && *p != '\r'; p++)
        ;
    *p = 0;
    return data;
}

/* Copies every locked git/registry package into vendor/<name>/ and removes
 * vendor directories of packages that are no longer locked. */
static int cpkg_vendor(CpkgProject *p, PkgLock *lock)
{
    char *vroot = os_path_join(p->root, PKG_VENDOR_DIR);
    size_t i, n = 0;
    int errs = 0;
    OsDirList dl;
    for (i = 0; i < lock->pkgs.len; i++) {
        const PkgLocked *l = &lock->pkgs.data[i];
        char *dest, *marker, *src, *tmp;
        StrBuf sb = {0};
        if (l->kind == PKG_SRC_PATH)
            continue;
        dest = os_path_join(vroot, l->name);
        marker = cpkg_marker(dest);
        if (marker && strcmp(marker, l->commit) == 0) {
            free(marker);
            free(dest);
            n++;
            continue; /* already vendored at this commit */
        }
        free(marker);
        src = pkg_cache_dir(l->name, l->commit);
        if (!os_is_dir(src)) {
            diag_error(span_none(), "dependency `%s` is not fetched", l->name);
            free(src);
            free(dest);
            errs++;
            continue;
        }
        os_mkdir_p(vroot);
        sb_printf(&sb, "%s/.%s.tmp", vroot, l->name);
        tmp = sb_take(&sb);
        os_remove_all(tmp);
        pkg_status("Vendoring", "%s v%s", l->name, l->version);
        if (cpkg_copy_package(src, tmp) != 0) {
            diag_error(span_none(), "cannot copy `%s` into %s", l->name, tmp);
            errs++;
        } else {
            char *mp = os_path_join(tmp, PKG_VENDOR_MARKER);
            StrBuf mk = {0};
            sb_printf(&mk, "%s\n", l->commit);
            if (os_write_file(mp, mk.data, mk.len) != 0 || os_remove_all(dest) != 0 || os_rename(tmp, dest) != 0) {
                diag_error(span_none(), "cannot write vendor/%s", l->name);
                errs++;
            } else {
                n++;
            }
            sb_free(&mk);
            free(mp);
        }
        os_remove_all(tmp);
        free(tmp);
        free(src);
        free(dest);
    }
    /* stale vendored packages */
    if (os_list_dir(vroot, &dl) == 0) {
        for (i = 0; i < dl.len; i++) {
            char *d, *marker;
            if (!dl.items[i].is_dir || pkg_lock_find(lock, dl.items[i].name))
                continue;
            d = os_path_join(vroot, dl.items[i].name);
            marker = cpkg_marker(d);
            if (marker) {
                pkg_status("Removing", "vendor/%s (no longer a dependency)", dl.items[i].name);
                os_remove_all(d);
            }
            free(marker);
            free(d);
        }
        os_dir_list_free(&dl);
    }
    free(vroot);
    if (errs)
        return -1;
    lock->vendored = 1;
    pkg_status("Vendored", "%lu package%s into vendor/", (unsigned long)n, n == 1 ? "" : "s");
    return 0;
}

/* ------------------------------------------------- resolve + lock + fetch -- */

/* Resolves `m` (with the previous lock as preference), fetches, writes
 * tern.lock (and refreshes vendor/ when vendored). */
static int cpkg_sync(CpkgProject *p, const TernManifest *m, const char *const *update, int update_all,
                     PkgGitProvider *gctx)
{
    PkgLock prev, next;
    PkgResolveOpts o;
    PkgProvider prov = pkg_git_provider(gctx);
    int pr = pkg_lock_read(p->lock_path, &prev);
    if (pr < 0)
        return -1;
    memset(&o, 0, sizeof o);
    o.prev = pr == 0 ? &prev : NULL;
    o.update = update;
    o.update_all = update_all;
    if (pkg_resolve(m, &o, &prov, &next) != 0) {
        pkg_lock_free(&prev);
        return -1;
    }
    cpkg_print_changes(pr == 0 ? &prev : NULL, &next);
    if (next.vendored && cpkg_vendor(p, &next) != 0) {
        pkg_lock_free(&prev);
        pkg_lock_free(&next);
        return -1;
    }
    if (pkg_lock_write(p->lock_path, &next) != 0) {
        pkg_lock_free(&prev);
        pkg_lock_free(&next);
        return -1;
    }
    pkg_lock_free(&prev);
    pkg_lock_free(&next);
    return 0;
}

static void cpkg_provider_init(PkgGitProvider *g, CpkgProject *p, char **index_url)
{
    memset(g, 0, sizeof *g);
    *index_url = pkg_index_url(&p->m);
    g->index_url = *index_url;
    g->project_root = p->root;
}

/* `tern add` without arguments: fetch what tern.lock says (resolving only
 * when tern.toml changed). */
static int cpkg_fetch_locked(CpkgProject *p)
{
    PkgLock lock;
    PkgGitProvider g;
    PkgProvider prov;
    char *iu;
    int r = pkg_lock_read(p->lock_path, &lock), rc;
    if (r < 0)
        return -1;
    cpkg_provider_init(&g, p, &iu);
    if (r == 0 && pkg_lock_satisfies(&p->m, &lock, 0)) {
        prov = pkg_git_provider(&g);
        rc = pkg_lock_fetch(&p->m, &lock, &prov);
        if (rc == 0 && lock.vendored)
            rc = cpkg_vendor(p, &lock);
        if (rc == 0)
            pkg_status("Ready", "%lu locked package%s", (unsigned long)lock.pkgs.len, lock.pkgs.len == 1 ? "" : "s");
    } else {
        rc = cpkg_sync(p, &p->m, NULL, 0, &g);
    }
    pkg_lock_free(&lock);
    free(iu);
    return rc;
}

/* ------------------------------------------------------------- tern add -- */

static int cpkg_add_usage(void)
{
    cmd_print_usage("add");
    return TERN_EXIT_OK;
}

int cmd_add(int argc, char **argv)
{
    ArgParser ap;
    const char *tag = NULL, *rev = NULL, *branch = NULL, *path = NULL, *git = NULL, *name_opt = NULL;
    char *git_owned = NULL;   /* git URL made from a local directory */
    char *hint_tag = NULL, *hint_commit = NULL;
    VEC(const char *) specs = {0};
    CpkgProject p;
    PkgGitProvider g;
    char *index_url = NULL;
    StrBuf text = {0};
    VEC(char *) names = {0};
    int rc = TERN_EXIT_FAIL;
    size_t i;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            vec_free(specs);
            return cpkg_add_usage();
        } else if (args_opt(&ap, "tag", &tag) || args_opt(&ap, "rev", &rev) || args_opt(&ap, "branch", &branch) ||
                   args_opt(&ap, "path", &path) || args_opt(&ap, "git", &git) || args_opt(&ap, "name", &name_opt)) {
            ;
        } else if (args_flag(&ap, "quiet|q")) {
            pkg_set_quiet(1);
        } else if (args_positional(&ap)) {
            vec_push(specs, ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (!git && !path && specs.len == 1 && cpkg_is_url(specs.data[0])) {
        git = specs.data[0];
        specs.len = 0;
    } else if (!git && !path && specs.len == 1 && os_is_dir(specs.data[0])) {
        /* a local directory: a git dependency on its repository (and tags) */
        char *abs = os_path_abs(specs.data[0]), *dotgit = os_path_join(abs, ".git");
        if (os_exists(dotgit)) {
            StrBuf u = {0};
            sb_puts(&u, abs[0] == '/' ? "file://" : "file:///");
            sb_puts(&u, abs);
            git_owned = sb_take(&u);
            git = git_owned;
            specs.len = 0;
        } else {
            args_error(&ap, "`%s` is a directory but not a git repository; for a path dependency use "
                            "`tern add --path %s`", specs.data[0], specs.data[0]);
        }
        free(dotgit);
        free(abs);
    }
    if ((tag != NULL) + (rev != NULL) + (branch != NULL) > 1)
        args_error(&ap, "use only one of --tag, --rev and --branch");
    if ((tag || rev || branch) && !git)
        args_error(&ap, "--tag/--rev/--branch need a git URL (`tern add <url> --tag v1.0.0`)");
    if ((git || path) && specs.len)
        args_error(&ap, "give either a git URL, --path or registry package names, not several kinds");
    if (git && path)
        args_error(&ap, "give either a git URL or --path, not both");
    if (name_opt && specs.len)
        args_error(&ap, "--name only applies to git URLs and --path");
    if (name_opt && !pkg_name_valid(name_opt))
        args_error(&ap, "invalid package name `%s`", name_opt);
    if (ap.errors) {
        vec_free(specs);
        free(git_owned);
        return TERN_EXIT_USAGE;
    }
    if (cpkg_open(&p) != 0) {
        vec_free(specs);
        free(git_owned);
        cpkg_close(&p);
        return TERN_EXIT_FAIL;
    }
    if (!git && !path && specs.len == 0) {
        rc = cpkg_fetch_locked(&p) == 0 ? TERN_EXIT_OK : TERN_EXIT_FAIL;
        cpkg_close(&p);
        vec_free(specs);
        return rc;
    }
    cpkg_provider_init(&g, &p, &index_url);
    {
        char *data;
        size_t len;
        if (os_read_file(p.manifest_path, &data, &len) != 0) {
            diag_error(span_none(), "cannot read `%s`", p.manifest_path);
            goto out;
        }
        sb_append(&text, data, len);
        free(data);
    }

    /* one textual edit per added dependency */
    if (path) {
        char *abs = os_path_abs(path), *mp = os_path_join(abs, PKG_MANIFEST_FILE), *rel;
        TernManifest dm;
        const char *nm = name_opt;
        StrBuf val = {0}, out = {0};
        char *err = NULL;
        if (!os_is_file(mp)) {
            diag_error(span_none(), "`%s` is not a Tern package (no tern.toml)", path);
            free(abs);
            free(mp);
            goto out;
        }
        if (pkg_manifest_load(mp, &dm) != 0) {
            free(abs);
            free(mp);
            pkg_manifest_free(&dm);
            goto out;
        }
        if (!nm)
            nm = dm.name;
        rel = pkg_path_rel(abs, p.root);
        sb_puts(&val, "{ path = ");
        toml_put_string(&val, rel);
        sb_puts(&val, " }");
        if (pkg_manifest_edit_dep(text.data, text.len, nm, sb_cstr(&val), &out, &err) != 0) {
            diag_error(span_none(), "%s", err);
            free(err);
            sb_free(&val);
            free(rel);
            free(abs);
            free(mp);
            pkg_manifest_free(&dm);
            goto out;
        }
        pkg_status("Adding", "%s (path %s) to tern.toml", nm, rel);
        vec_push(names, xstrdup(nm));
        sb_free(&text);
        text = out;
        sb_free(&val);
        free(rel);
        free(abs);
        free(mp);
        pkg_manifest_free(&dm);
    } else if (git) {
        /* fetch it once to learn its name (and newest tag) */
        Arena a = {0};
        PkgNode node;
        PkgProvider prov = pkg_git_provider(&g);
        char *tmpname = cpkg_url_basename(git), *mp;
        const char *nm = name_opt;
        TernManifest dm;
        StrBuf val = {0}, out = {0};
        char *err = NULL;
        PkgVersion node_version;
        int node_version_ok = 0;
        memset(&node, 0, sizeof node);
        memset(&node_version, 0, sizeof node_version);
        node.name = tmpname;
        node.kind = PKG_SRC_GIT;
        node.url = git;
        node.tag = tag;
        node.rev = rev;
        node.branch = branch;
        if (!tag && !rev && !branch) {
            PkgAvailList tags;
            size_t k;
            const PkgAvail *best = NULL;
            if (pkg_git_tags(git, &a, &tags) != 0) {
                free(tmpname);
                arena_release(&a);
                goto out;
            }
            for (k = 0; k < tags.items.len; k++) {
                const PkgAvail *t = &tags.items.data[k];
                if (t->version.pre[0])
                    continue;
                if (!best || pkg_version_cmp(&t->version, &best->version) > 0)
                    best = t;
            }
            if (best) {
                node.tag = best->tag;
                node.commit = best->commit;
                node_version = best->version;
                node_version_ok = 1;
            }
            vec_free(tags.items);
        }
        if (prov.fetch(prov.user, &node, &a) != 0) {
            free(tmpname);
            arena_release(&a);
            goto out;
        }
        /* the resolver below fetches the same commit again: point it at this one */
        if (node.tag && node.commit) {
            hint_tag = xstrdup(node.tag);
            hint_commit = xstrdup(node.commit);
            g.hint_url = git;
            g.hint_tag = hint_tag;
            g.hint_commit = hint_commit;
        }
        mp = os_path_join(node.root, PKG_MANIFEST_FILE);
        if (!os_is_file(mp)) {
            diag_error(span_none(), "%s is not a Tern package (no tern.toml at its root)", git);
            free(mp);
            free(tmpname);
            arena_release(&a);
            goto out;
        }
        if (pkg_manifest_load(mp, &dm) != 0) {
            free(mp);
            free(tmpname);
            pkg_manifest_free(&dm);
            arena_release(&a);
            goto out;
        }
        free(mp);
        if (!nm)
            nm = dm.name;
        if (strcmp(nm, tmpname) != 0) {
            /* keep the checkout under its real name in the cache */
            char *from = pkg_cache_dir(tmpname, node.commit), *to = pkg_cache_dir(nm, node.commit), *par;
            par = os_path_dirname(to);
            os_mkdir_p(par);
            if (!os_is_dir(to))
                os_rename(from, to);
            else
                os_remove_all(from);
            free(par);
            free(from);
            free(to);
        }
        sb_puts(&val, "{ git = ");
        toml_put_string(&val, git);
        if (node.tag && !tag && node_version_ok) {
            /* the newest release: a caret requirement, so `tern update`
             * moves to newer compatible tags (docs/packages.md) */
            char vb[PKG_VERSION_STRLEN];
            char *req = arena_printf(&a, "^%s", pkg_version_str(&node_version, vb));
            sb_puts(&val, ", version = ");
            toml_put_string(&val, req);
        } else if (node.tag) {
            sb_puts(&val, ", tag = ");
            toml_put_string(&val, node.tag);
        } else if (rev) {
            sb_puts(&val, ", rev = ");
            toml_put_string(&val, rev);
        } else if (branch) {
            sb_puts(&val, ", branch = ");
            toml_put_string(&val, branch);
        }
        sb_puts(&val, " }");
        if (pkg_manifest_edit_dep(text.data, text.len, nm, sb_cstr(&val), &out, &err) != 0) {
            diag_error(span_none(), "%s", err);
            free(err);
        } else {
            pkg_status("Adding", "%s = %s to tern.toml", nm, sb_cstr(&val));
            vec_push(names, xstrdup(nm));
            sb_free(&text);
            text = out;
        }
        sb_free(&val);
        pkg_manifest_free(&dm);
        free(tmpname);
        arena_release(&a);
        if (err)
            goto out;
    } else {
        PkgProvider prov = pkg_git_provider(&g);
        for (i = 0; i < specs.len; i++) {
            const char *spec = specs.data[i], *at = strchr(spec, '@');
            char *nm = at ? xstrndup(spec, (size_t)(at - spec)) : xstrdup(spec);
            char *req = NULL, *err = NULL;
            StrBuf val = {0}, out = {0};
            if (!pkg_name_valid(nm)) {
                diag_error(span_none(), "invalid package name `%s`", nm);
                diag_help("use `tern add <name>[@<version>]`, `tern add <git-url>` or `tern add --path <dir>`");
                free(nm);
                goto out;
            }
            if (at) {
                PkgReq r;
                char perr[160];
                if (pkg_req_parse(at + 1, &r, perr, sizeof perr) != 0) {
                    diag_error(span_none(), "invalid version requirement `%s`: %s", at + 1, perr);
                    diag_help("examples: axi@1.2, axi@=1.2.3, axi@~1.2");
                    free(nm);
                    goto out;
                }
                req = xstrdup(at + 1);
            } else {
                Arena a = {0};
                PkgAvailList av;
                int lr = prov.registry(prov.user, nm, &a, &av);
                const PkgAvail *best = NULL;
                size_t k;
                char vb[PKG_VERSION_STRLEN];
                if (lr == 1) {
                    diag_error(span_none(), "package `%s` is not in the registry index (%s)", nm, g.index_url);
                    diag_help("check the spelling, or add it by URL: `tern add https://.../%s`", nm);
                }
                if (lr != 0) {
                    vec_free(av.items);
                    arena_release(&a);
                    free(nm);
                    goto out;
                }
                for (k = 0; k < av.items.len; k++) {
                    const PkgAvail *t = &av.items.data[k];
                    if (t->yanked || t->version.pre[0])
                        continue;
                    if (!best || pkg_version_cmp(&t->version, &best->version) > 0)
                        best = t;
                }
                if (!best) {
                    diag_error(span_none(), "package `%s` has no published release", nm);
                    diag_help("depend on a pre-release explicitly: `tern add %s@=<version>`", nm);
                    vec_free(av.items);
                    arena_release(&a);
                    free(nm);
                    goto out;
                }
                req = xstrdup(pkg_version_str(&best->version, vb));
                vec_free(av.items);
                arena_release(&a);
            }
            toml_put_string(&val, req);
            if (pkg_manifest_edit_dep(text.data, text.len, nm, sb_cstr(&val), &out, &err) != 0) {
                diag_error(span_none(), "%s", err);
                free(err);
                sb_free(&val);
                free(req);
                free(nm);
                goto out;
            }
            pkg_status("Adding", "%s = %s to tern.toml", nm, sb_cstr(&val));
            sb_free(&text);
            text = out;
            sb_free(&val);
            free(req);
            vec_push(names, nm);
        }
    }

    /* validate the edited manifest, resolve, and only then write */
    {
        TernManifest nm;
        uint32_t id = src_add(p.m.path ? p.m.path : p.manifest_path, text.data, text.len);
        const char **upd = (const char **)xcalloc(names.len + 1, sizeof(char *));
        int ok;
        for (i = 0; i < names.len; i++)
            upd[i] = names.data[i];
        if (pkg_manifest_parse(id, p.root, &nm) != 0) {
            diag_help("tern.toml was not changed");
            pkg_manifest_free(&nm);
            free(upd);
            goto out;
        }
        ok = cpkg_sync(&p, &nm, upd, 0, &g) == 0;
        free(upd);
        pkg_manifest_free(&nm);
        if (!ok) {
            diag_help("tern.toml and tern.lock were not changed");
            goto out;
        }
        if (os_write_file_if_changed(p.manifest_path, text.data, text.len) < 0) {
            diag_error(span_none(), "cannot write `%s`", p.manifest_path);
            goto out;
        }
    }
    rc = TERN_EXIT_OK;
out:
    for (i = 0; i < names.len; i++)
        free(names.data[i]);
    vec_free(names);
    vec_free(specs);
    sb_free(&text);
    free(index_url);
    free(git_owned);
    free(hint_tag);
    free(hint_commit);
    cpkg_close(&p);
    return rc;
}

/* ---------------------------------------------------------- tern update -- */

int cmd_update(int argc, char **argv)
{
    ArgParser ap;
    VEC(const char *) names = {0};
    CpkgProject p;
    PkgGitProvider g;
    PkgLock prev;
    char *iu = NULL;
    int rc = TERN_EXIT_FAIL, pr;
    size_t i;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("update");
            vec_free(names);
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "quiet|q")) {
            pkg_set_quiet(1);
        } else if (args_positional(&ap)) {
            vec_push(names, ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors) {
        vec_free(names);
        return TERN_EXIT_USAGE;
    }
    if (cpkg_open(&p) != 0) {
        cpkg_close(&p);
        vec_free(names);
        return TERN_EXIT_FAIL;
    }
    pr = pkg_lock_read(p.lock_path, &prev);
    if (pr < 0)
        goto out;
    for (i = 0; i < names.len; i++) {
        if (!pkg_manifest_dep(&p.m, names.data[i]) && !(pr == 0 && pkg_lock_find(&prev, names.data[i]))) {
            diag_error(span_none(), "`%s` is not a dependency of %s", names.data[i], p.m.name ? p.m.name : "this project");
            if (p.m.ndeps) {
                StrBuf sb = {0};
                size_t k;
                for (k = 0; k < p.m.ndeps; k++)
                    sb_printf(&sb, "%s`%s`", k ? ", " : "", p.m.deps[k].name);
                diag_help("dependencies: %s", sb_cstr(&sb));
                sb_free(&sb);
            }
            goto out;
        }
    }
    cpkg_provider_init(&g, &p, &iu);
    vec_push(names, NULL);
    if (cpkg_sync(&p, &p.m, names.data, names.len == 1, &g) == 0)
        rc = TERN_EXIT_OK;
    /* git dependencies pinned by tag stay; say when a newer release exists */
    for (i = 0; rc == TERN_EXIT_OK && i < p.m.ndeps; i++) {
        const TernDep *d = &p.m.deps[i];
        Arena a = {0};
        PkgAvailList tags;
        PkgVersion pinned;
        const PkgAvail *best = NULL;
        size_t k, j;
        int named = names.len == 1;
        for (j = 0; j + 1 < names.len; j++)
            if (strcmp(names.data[j], d->name) == 0)
                named = 1;
        if (!named || d->kind != PKG_DEP_GIT || !d->tag ||
            pkg_version_parse(d->tag, &pinned, 1) != 0 ||
            pkg_git_tags(d->git, &a, &tags) != 0) {
            arena_release(&a);
            continue;
        }
        for (k = 0; k < tags.items.len; k++) {
            const PkgAvail *t = &tags.items.data[k];
            if (!t->version.pre[0] && pkg_version_cmp(&t->version, &pinned) > 0 &&
                (!best || pkg_version_cmp(&t->version, &best->version) > 0))
                best = t;
        }
        if (best)
            pkg_status("Note", "%s is pinned to tag %s; %s is available (edit its tag in tern.toml)", d->name,
                       d->tag, best->tag);
        vec_free(tags.items);
        arena_release(&a);
    }
out:
    if (pr >= 0)
        pkg_lock_free(&prev);
    free(iu);
    vec_free(names);
    cpkg_close(&p);
    return rc;
}

/* ---------------------------------------------------------- tern vendor -- */

int cmd_vendor(int argc, char **argv)
{
    ArgParser ap;
    CpkgProject p;
    PkgLock lock;
    int rc = TERN_EXIT_FAIL, remove = 0, r;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("vendor");
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "remove")) {
            remove = 1;
        } else if (args_flag(&ap, "quiet|q")) {
            pkg_set_quiet(1);
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors)
        return TERN_EXIT_USAGE;
    if (cpkg_open(&p) != 0) {
        cpkg_close(&p);
        return TERN_EXIT_FAIL;
    }
    if (remove) {
        r = pkg_lock_read(p.lock_path, &lock);
        if (r == 0) {
            size_t i;
            for (i = 0; i < lock.pkgs.len; i++) {
                StrBuf sb = {0};
                char *d, *marker;
                sb_printf(&sb, "%s/%s/%s", p.root, PKG_VENDOR_DIR, lock.pkgs.data[i].name);
                d = sb_take(&sb);
                marker = cpkg_marker(d);
                if (marker)
                    os_remove_all(d);
                free(marker);
                free(d);
            }
            lock.vendored = 0;
            if (pkg_lock_write(p.lock_path, &lock) == 0) {
                pkg_status("Unvendored", "builds read dependencies from the cache again");
                rc = TERN_EXIT_OK;
            }
        } else if (r == 1) {
            rc = TERN_EXIT_OK;
        }
        if (r >= 0)
            pkg_lock_free(&lock);
        cpkg_close(&p);
        return rc;
    }
    /* make sure tern.lock is current and everything is fetched */
    if (cpkg_fetch_locked(&p) != 0)
        goto out;
    r = pkg_lock_read(p.lock_path, &lock);
    if (r != 0) {
        if (r == 1) {
            pkg_status("Vendored", "nothing (no dependencies)");
            rc = TERN_EXIT_OK;
        }
        goto out;
    }
    if (cpkg_vendor(&p, &lock) == 0 && pkg_lock_write(p.lock_path, &lock) == 0)
        rc = TERN_EXIT_OK;
    pkg_lock_free(&lock);
out:
    cpkg_close(&p);
    return rc;
}

/* --------------------------------------------------------- tern publish -- */

/* "git@github.com:a/b.git" -> "https://github.com/a/b.git" for well-known hosts. */
static char *cpkg_public_url(const char *url)
{
    static const char *const hosts[] = {"github.com", "gitlab.com", "codeberg.org", "bitbucket.org"};
    size_t i;
    if (tern_starts_with(url, "git@")) {
        for (i = 0; i < TERN_ARRAY_LEN(hosts); i++) {
            size_t hl = strlen(hosts[i]);
            if (strncmp(url + 4, hosts[i], hl) == 0 && url[4 + hl] == ':') {
                StrBuf sb = {0};
                sb_printf(&sb, "https://%s/%s", hosts[i], url + 5 + hl);
                return sb_take(&sb);
            }
        }
    }
    return xstrdup(url);
}

/* "owner/repo" of a GitHub URL (https or scp-like), or NULL. */
static char *cpkg_github_slug(const char *url)
{
    const char *p = NULL, *e;
    char *r;
    if (tern_starts_with(url, "https://github.com/"))
        p = url + 19;
    else if (tern_starts_with(url, "http://github.com/"))
        p = url + 18;
    else if (tern_starts_with(url, "git@github.com:"))
        p = url + 15;
    else if (tern_starts_with(url, "ssh://git@github.com/"))
        p = url + 21;
    if (!p)
        return NULL;
    e = p + strlen(p);
    while (e > p && e[-1] == '/')
        e--;
    if (e - p > 4 && memcmp(e - 4, ".git", 4) == 0)
        e -= 4;
    r = xstrndup(p, (size_t)(e - p));
    if (!strchr(r, '/')) {
        free(r);
        return NULL;
    }
    return r;
}

static int cpkg_git_ok(const char *cwd, const char *const *args, const char *what)
{
    char *e = NULL;
    int rc = pkg_git(cwd, args, NULL, &e);
    if (rc != 0) {
        diag_error(span_none(), "%s failed", what);
        if (e && e[0])
            diag_note(span_none(), "git said:\n        %s", e);
    }
    free(e);
    return rc == 0 ? 0 : -1;
}

int cmd_publish(int argc, char **argv)
{
    ArgParser ap;
    CpkgProject p;
    int dry = 0, want_pr = 0, rc = TERN_EXIT_FAIL;
    const char *fork_opt = NULL;
    char *o = NULL, *e = NULL, *url = NULL, *head = NULL, *iu = NULL, *idx = NULL, *rel = NULL, *fork = NULL;
    char *slug = NULL, *branch = NULL, *tag = NULL, *fork_env = NULL;
    PkgIndexEntry entry;
    int have_entry = 0;
    StrBuf content = {0}, title = {0}, body = {0};
    size_t i;
    memset(&entry, 0, sizeof entry);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("publish");
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "dry-run|n")) {
            dry = 1;
        } else if (args_flag(&ap, "pr")) {
            want_pr = 1;
        } else if (args_opt(&ap, "fork", &fork_opt)) {
            ;
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors)
        return TERN_EXIT_USAGE;
    if (cpkg_open(&p) != 0) {
        cpkg_close(&p);
        return TERN_EXIT_FAIL;
    }
    if (!pkg_git_available()) {
        diag_error(span_none(), "`git` was not found in PATH; `tern publish` needs it");
        goto out;
    }
    for (i = 0; i < p.m.ndeps; i++) {
        if (p.m.deps[i].kind == PKG_DEP_PATH) {
            diag_error(p.m.deps[i].span, "path dependency `%s` cannot be published", p.m.deps[i].name);
            diag_help("publish `%s` first and depend on it by version or git URL", p.m.deps[i].name);
            goto out;
        }
    }
    if (!p.m.description)
        diag_warning(span_none(), "[package] has no `description`; the index entry will not have one");
    if (!p.m.license)
        diag_warning(span_none(), "[package] has no `license`; others may not be allowed to use the package");
    {
        const char *a[] = {"rev-parse", "--show-toplevel", NULL};
        if (pkg_git(p.root, a, NULL, NULL) != 0) {
            diag_error(span_none(), "%s is not inside a git repository", p.root);
            diag_help("a package is a git repository: run `git init`, commit, and add a remote");
            goto out;
        }
    }
    {
        const char *a[] = {"status", "--porcelain", NULL};
        if (pkg_git(p.root, a, &o, &e) != 0) {
            diag_error(span_none(), "`git status` failed: %s", e ? e : "");
            goto out;
        }
        if (o && o[0]) {
            diag_error(span_none(), "the working tree has uncommitted changes");
            diag_note(span_none(), "`git status --porcelain`:\n%s", o);
            diag_help("commit or stash them first; a published version must match a commit");
            goto out;
        }
        free(o);
        free(e);
        o = e = NULL;
    }
    tag = (char *)xmalloc(strlen(p.m.version) + 2);
    snprintf(tag, strlen(p.m.version) + 2, "v%s", p.m.version);
    {
        StrBuf ref = {0};
        const char *a[] = {"rev-parse", "-q", "--verify", NULL, NULL};
        int exists;
        sb_printf(&ref, "refs/tags/%s", tag);
        a[3] = ref.data;
        exists = pkg_git(p.root, a, NULL, NULL) == 0;
        sb_free(&ref);
        if (exists) {
            diag_error(span_none(), "version %s is already tagged (%s)", p.m.version, tag);
            diag_help("bump `version` in tern.toml, commit, and publish again");
            goto out;
        }
    }
    if (p.m.repository) {
        url = xstrdup(p.m.repository);
    } else {
        const char *a[] = {"remote", "get-url", "origin", NULL};
        char *raw = NULL;
        if (pkg_git(p.root, a, &raw, NULL) != 0 || !raw || !raw[0]) {
            diag_error(span_none(), "the repository has no remote `origin`");
            diag_help("add one (`git remote add origin <url>`) or set `repository = \"<url>\"` in [package]");
            free(raw);
            goto out;
        }
        url = cpkg_public_url(raw);
        free(raw);
    }
    {
        const char *a[] = {"rev-parse", "HEAD", NULL};
        if (pkg_git(p.root, a, &head, NULL) != 0 || !head || !head[0]) {
            diag_error(span_none(), "the repository has no commits yet");
            goto out;
        }
    }
    if (!dry) {
        StrBuf ref = {0};
        const char *a[] = {"ls-remote", "--tags", "origin", NULL, NULL};
        sb_printf(&ref, "refs/tags/%s", tag);
        a[3] = ref.data;
        if (pkg_git(p.root, a, &o, NULL) == 0 && o && o[0]) {
            diag_error(span_none(), "tag %s already exists on the remote", tag);
            diag_help("bump `version` in tern.toml, commit, and publish again");
            sb_free(&ref);
            goto out;
        }
        sb_free(&ref);
        free(o);
        o = NULL;
    }

    /* the index entry */
    iu = pkg_index_url(&p.m);
    idx = pkg_index_dir();
    rel = pkg_index_rel_path(p.m.name);
    pkg_status("Updating", "index %s", iu);
    if (pkg_index_sync(iu) != 0) {
        if (!dry)
            goto out;
        diag_warning(span_none(), "continuing the dry run without an up-to-date index");
    }
    {
        int r = pkg_index_read(idx, p.m.name, &entry);
        PkgAvail av;
        if (r < 0)
            goto out;
        have_entry = 1;
        if (r == 1) {
            entry.name = arena_strdup(&entry.arena, p.m.name);
            entry.git = arena_strdup(&entry.arena, url);
        } else if (strcmp(entry.git, url) != 0) {
            diag_error(span_none(), "the name `%s` is already registered in the index for %s", p.m.name, entry.git);
            diag_note(span_none(), "this repository is %s", url);
            diag_help("names are first come, first served: rename the package in tern.toml (or set [package] "
                      "repository if this is the same repository under another URL)");
            goto out;
        }
        for (i = 0; i < entry.versions.items.len; i++) {
            PkgVersion v;
            pkg_version_parse(p.m.version, &v, 0);
            if (pkg_version_cmp(&entry.versions.items.data[i].version, &v) == 0) {
                diag_error(span_none(), "%s %s is already in the index", p.m.name, p.m.version);
                diag_help("bump `version` in tern.toml");
                goto out;
            }
        }
        if (p.m.description)
            entry.description = arena_strdup(&entry.arena, p.m.description);
        memset(&av, 0, sizeof av);
        pkg_version_parse(p.m.version, &av.version, 0);
        av.tag = arena_strdup(&entry.arena, tag);
        av.commit = arena_strdup(&entry.arena, head);
        vec_push(entry.versions.items, av);
        entry.versions.git = entry.git;
        pkg_index_format(&entry, &content);
    }
    branch = (char *)xmalloc(strlen(p.m.name) + strlen(p.m.version) + 16);
    snprintf(branch, strlen(p.m.name) + strlen(p.m.version) + 16, "publish/%s-%s", p.m.name, p.m.version);
    sb_printf(&title, "Publish %s %s", p.m.name, p.m.version);
    sb_printf(&body, "Adds %s %s (%s, tag %s, commit %s) to the index.", p.m.name, p.m.version, url, tag, head);
    slug = cpkg_github_slug(iu);
    fork_env = os_getenv("TERN_INDEX_FORK");
    fork = fork_opt ? xstrdup(fork_opt)
                    : fork_env && fork_env[0] ? xstrdup(fork_env) : p.m.registry_fork ? xstrdup(p.m.registry_fork) : NULL;

    if (dry) {
        if (pkg_quiet()) {
            rc = TERN_EXIT_OK;
            goto out;
        }
        printf("Dry run: nothing was changed.\n\n");
        printf("Would tag %s at %s and push it to origin (%s).\n", tag, head, url);
        printf("Would write %s on branch %s of the index clone %s:\n\n", rel, branch, idx);
        printf("%s\n", sb_cstr(&content));
        if (fork)
            printf("Would push %s to %s%s.\n", branch, fork, want_pr ? " and open a pull request" : "");
        rc = TERN_EXIT_OK;
        goto out;
    }

    /* tag and push */
    {
        const char *a[] = {"tag", "-a", tag, "-m", title.data, NULL};
        const char *push[] = {"push", "-q", "origin", tag, NULL};
        pkg_status("Tagging", "%s at %.12s", tag, head);
        if (cpkg_git_ok(p.root, a, "`git tag`") != 0)
            goto out;
        pkg_status("Pushing", "%s to origin", tag);
        if (cpkg_git_ok(p.root, push, "`git push origin <tag>`") != 0) {
            const char *del[] = {"tag", "-d", tag, NULL};
            pkg_git(p.root, del, NULL, NULL);
            diag_help("the local tag was removed again; fix the problem and run `tern publish` again");
            goto out;
        }
    }
    /* index branch + commit */
    {
        const char *co[] = {"checkout", "-q", "-B", branch, NULL};
        const char *add[] = {"add", rel, NULL};
        const char *commit[] = {"commit", "-q", "-m", title.data, NULL};
        char *fp = os_path_join(idx, rel), *dir = os_path_dirname(fp);
        int ok;
        pkg_status("Writing", "index entry %s on branch %s", rel, branch);
        ok = cpkg_git_ok(idx, co, "`git checkout` in the index clone") == 0;
        if (ok) {
            os_mkdir_p(dir);
            ok = os_write_file(fp, content.data, content.len) == 0;
            if (!ok)
                diag_error(span_none(), "cannot write %s", fp);
        }
        ok = ok && cpkg_git_ok(idx, add, "`git add` in the index clone") == 0;
        ok = ok && cpkg_git_ok(idx, commit, "`git commit` in the index clone") == 0;
        free(fp);
        free(dir);
        if (!ok) {
            diag_note(span_none(), "the tag %s was pushed; the index entry can be written by hand: %s", tag, rel);
            goto out;
        }
    }
    if (fork) {
        const char *push[] = {"push", "-q", "-f", fork, branch, NULL};
        pkg_status("Pushing", "%s to %s", branch, fork);
        if (cpkg_git_ok(idx, push, "pushing the index branch to your fork") != 0) {
            diag_help("sync your fork with %s and run: git -C %s push %s %s", iu, idx, fork, branch);
            goto out;
        }
    }
    {
        char *gh = os_find_program("gh");
        char *fork_slug = fork ? cpkg_github_slug(fork) : NULL;
        StrBuf headref = {0};
        if (fork_slug) {
            char *slash = strchr(fork_slug, '/');
            if (slash)
                *slash = 0;
            sb_printf(&headref, "%s:%s", fork_slug, branch);
        } else if (want_pr && gh && slug) {
            sb_printf(&headref, "%s", branch);
        } else {
            sb_printf(&headref, "<your-github-user>:%s", branch);
        }
        if (want_pr && gh && slug) {
            char *argv_gh[16];
            int n = 0;
            OsSpawnOpts so;
            OsRunResult res;
            argv_gh[n++] = (char *)"gh";
            argv_gh[n++] = (char *)"pr";
            argv_gh[n++] = (char *)"create";
            argv_gh[n++] = (char *)"--repo";
            argv_gh[n++] = slug;
            argv_gh[n++] = (char *)"--head";
            argv_gh[n++] = headref.data;
            argv_gh[n++] = (char *)"--title";
            argv_gh[n++] = title.data;
            argv_gh[n++] = (char *)"--body";
            argv_gh[n++] = body.data;
            argv_gh[n] = NULL;
            memset(&so, 0, sizeof so);
            memset(&res, 0, sizeof res);
            so.cwd = idx;
            pkg_status("Opening", "pull request on %s", slug);
            fflush(stdout);
            fflush(stderr);
            if (os_run(argv_gh, &so, &res) != 0) {
                diag_error(span_none(), "`gh pr create` failed");
                diag_help("run it yourself from %s: gh pr create --repo %s --head %s --title \"%s\"", idx, slug,
                          headref.data, title.data);
                os_run_result_free(&res);
                free(gh);
                free(fork_slug);
                sb_free(&headref);
                goto out;
            }
            os_run_result_free(&res);
        } else if (!pkg_quiet()) {
            printf("\nPublished %s %s: tag %s is pushed and the index entry is committed on branch\n%s of %s.\n",
                   p.m.name, p.m.version, tag, branch, idx);
            printf("\nTo open the pull request against %s:\n", iu);
            if (!fork)
                printf("  git -C %s push <your-fork-of-the-index> %s\n", idx, branch);
            if (slug)
                printf("  gh pr create --repo %s --head %s --title \"%s\" --body \"%s\"\n", slug, headref.data,
                       title.data, body.data);
            else
                printf("  open a pull request from branch %s of your fork\n", branch);
            if (!want_pr && gh && slug)
                printf("\n`tern publish --pr` runs `gh pr create` for you.\n");
        }
        free(gh);
        free(fork_slug);
        sb_free(&headref);
    }
    rc = TERN_EXIT_OK;
out:
    if (have_entry)
        pkg_index_entry_free(&entry);
    sb_free(&content);
    sb_free(&title);
    sb_free(&body);
    free(o);
    free(e);
    free(url);
    free(head);
    free(iu);
    free(idx);
    free(rel);
    free(fork);
    free(fork_env);
    free(slug);
    free(branch);
    free(tag);
    cpkg_close(&p);
    return rc;
}

/* -------------------------------------------------------------- tern new -- */

static char *cpkg_camel(const char *ident)
{
    StrBuf sb = {0};
    int up = 1;
    const char *p;
    for (p = ident; *p; p++) {
        if (*p == '_' || *p == '-') {
            up = 1;
            continue;
        }
        sb_putc(&sb, up ? tern_to_upper((unsigned char)*p) : *p);
        up = 0;
    }
    if (!sb.len || !tern_is_alpha((unsigned char)sb.data[0])) {
        sb_free(&sb);
        sb_puts(&sb, "Top");
    }
    return sb_take(&sb);
}

static int cpkg_write(const char *dir, const char *rel, const char *text)
{
    char *p = os_path_join(dir, rel), *d = os_path_dirname(p);
    int r;
    os_mkdir_p(d);
    r = os_write_file(p, text, strlen(text));
    if (r != 0)
        diag_error(span_none(), "cannot write `%s`", p);
    free(p);
    free(d);
    return r;
}

int cmd_new(int argc, char **argv)
{
    ArgParser ap;
    const char *name = NULL;
    int no_git = 0, errs = 0, git = 0;
    char *ident, *mod, *author = NULL;
    StrBuf toml = {0}, src = {0}, test = {0}, readme = {0}, rel = {0};
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("new");
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "no-git")) {
            no_git = 1;
        } else if (args_positional(&ap)) {
            if (name)
                args_error(&ap, "unexpected argument `%s` (one project name)", ap.arg);
            name = ap.arg;
        } else {
            args_unknown(&ap);
        }
    }
    if (!name && !ap.errors)
        args_error(&ap, "missing project name: `tern new <name>`");
    if (ap.errors)
        return TERN_EXIT_USAGE;
    if (!pkg_name_valid(os_path_basename(name))) {
        diag_error(span_none(), "invalid project name `%s`", os_path_basename(name));
        diag_help("names start with a letter or `_` and contain only letters, digits, `_` and `-` (e.g. `blinky`)");
        return TERN_EXIT_USAGE;
    }
    if (os_exists(name)) {
        diag_error(span_none(), "destination `%s` already exists", name);
        diag_help("choose another name or remove the directory");
        return TERN_EXIT_FAIL;
    }
    ident = pkg_name_ident(os_path_basename(name));
    mod = cpkg_camel(ident);
    git = !no_git && pkg_git_available();
    if (git) {
        const char *un[] = {"config", "user.name", NULL}, *ue[] = {"config", "user.email", NULL};
        char *n = NULL, *e = NULL;
        if (pkg_git(NULL, un, &n, NULL) == 0 && n && n[0]) {
            StrBuf a = {0};
            sb_puts(&a, n);
            if (pkg_git(NULL, ue, &e, NULL) == 0 && e && e[0])
                sb_printf(&a, " <%s>", e);
            author = sb_take(&a);
        }
        free(n);
        free(e);
    }
    sb_printf(&toml,
              "[package]\n"
              "name = \"%s\"\n"
              "version = \"0.1.0\"\n"
              "description = \"\"\n",
              os_path_basename(name));
    if (author) {
        sb_puts(&toml, "authors = [");
        toml_put_string(&toml, author);
        sb_puts(&toml, "]\n");
    }
    sb_printf(&toml,
              "# license = \"MIT\"\n"
              "\n"
              "[dependencies]\n"
              "# axi = \"1.2\"                                         # from the registry index\n"
              "# uart = { git = \"https://github.com/acme/tern-uart\", tag = \"v2.0.1\" }\n"
              "\n"
              "[features]\n"
              "# debug_ila = false                                     # read as cfg::debug_ila\n"
              "\n"
              "[target.default]\n"
              "top = \"%s\"\n"
              "std = \"sv\"\n"
              "# device = \"xc7a35t\"\n",
              mod);
    sb_printf(&src,
              "package %s\n"
              "\n"
              "/// Blinks `led`: it toggles every `Period` clock cycles.\n"
              "#[top]\n"
              "pub module %s<Period: uint = 50_000_000>\n"
              "where Period >= 2\n"
              "{\n"
              "    in clk: clock\n"
              "    in rst: reset\n"
              "    /// The LED output.\n"
              "    out led: bool\n"
              "\n"
              "    reg count: uint<clog2(Period)> = 0\n"
              "    reg state: bool = false\n"
              "\n"
              "    led = state\n"
              "\n"
              "    on clk {\n"
              "        if count == Period - 1 {\n"
              "            count = 0\n"
              "            state = !state\n"
              "        } else {\n"
              "            count = count +%% 1\n"
              "        }\n"
              "    }\n"
              "}\n"
              "\n"
              "test \"led toggles every Period cycles\" {\n"
              "    let dut = sim %s<Period: 4>()\n"
              "    dut.reset()\n"
              "    expect !dut.led\n"
              "    dut.step(4)\n"
              "    expect dut.led\n"
              "    dut.step(4)\n"
              "    expect !dut.led\n"
              "}\n",
              ident, mod, mod);
    sb_printf(&test,
              "package %s_test\n"
              "\n"
              "use %s::%s\n"
              "\n"
              "test \"%s holds the LED for exactly Period cycles\" {\n"
              "    let dut = sim %s<Period: 8>()\n"
              "    dut.reset()\n"
              "    dut.step(7)\n"
              "    expect !dut.led\n"
              "    dut.step()\n"
              "    expect dut.led\n"
              "    dut.step(8)\n"
              "    expect !dut.led\n"
              "}\n",
              ident, ident, mod, mod, mod);
    sb_printf(&readme,
              "# %s\n"
              "\n"
              "A [Tern](https://tern-lang.com) hardware design.\n"
              "\n"
              "    tern test            # run the tests on the built-in simulator\n"
              "    tern build           # emit SystemVerilog into build/\n"
              "    tern add <package>   # add a dependency\n"
              "\n"
              "Layout: `tern.toml` (package, dependencies, features, targets), `src/` (the design),\n"
              "`tests/` (tests that are not next to the code they test).\n",
              os_path_basename(name));
    if (os_mkdir_p(name) != 0) {
        diag_error(span_none(), "cannot create directory `%s`", name);
        errs++;
    } else {
        errs += cpkg_write(name, PKG_MANIFEST_FILE, toml.data) != 0;
        sb_printf(&rel, "src/%s.tern", ident);
        errs += cpkg_write(name, rel.data, src.data) != 0;
        sb_reset(&rel);
        sb_printf(&rel, "tests/%s_test.tern", ident);
        errs += cpkg_write(name, rel.data, test.data) != 0;
        errs += cpkg_write(name, ".gitignore", "/build/\n") != 0;
        errs += cpkg_write(name, "README.md", readme.data) != 0;
    }
    if (!errs && git) {
        const char *inside[] = {"rev-parse", "--is-inside-work-tree", NULL};
        const char *init[] = {"init", "-q", NULL};
        char *o = NULL;
        int in_repo = pkg_git(name, inside, &o, NULL) == 0 && o && strcmp(o, "true") == 0;
        free(o);
        if (!in_repo && pkg_git(name, init, NULL, NULL) != 0)
            diag_warning(span_none(), "`git init` failed; the project was created without a repository");
    }
    if (!errs) {
        pkg_status("Created", "package `%s` in %s/", os_path_basename(name), name);
        if (!pkg_quiet())
            printf("Next:\n    cd %s\n    tern test\n", name);
    }
    sb_free(&toml);
    sb_free(&src);
    sb_free(&test);
    sb_free(&readme);
    sb_free(&rel);
    free(ident);
    free(mod);
    free(author);
    return errs ? TERN_EXIT_FAIL : TERN_EXIT_OK;
}
