/* pkg.h — tern.toml, tern.lock, semantic versions, dependency resolution,
 * the on-disk package layout and the git plumbing behind `tern add`,
 * `tern update`, `tern vendor` and `tern publish` (spec §4, §9.5, §13.6).
 *
 * For the front-end driver (build/check/test/lsp) only three calls matter,
 * and none of them touches the network or runs git:
 *
 *     PkgProject prj;
 *     int r = pkg_project_open(".", &prj);    // finds tern.toml upwards
 *     if (r < 0) return TERN_EXIT_FAIL;        // errors already reported
 *     if (r == 0) { ...single-file mode, no manifest... }
 *     // prj.manifest.name / .version / .targets / .features
 *     // prj.deps.items[i].ident  -> `use` root of a dependency (`uart_ip`)
 *     // prj.deps.items[i].src    -> its src/ directory
 *     int dbg = 0; pkg_manifest_feature(&prj.manifest, "debug_ila", &dbg); // cfg::debug_ila
 *     const TernTarget *t = pkg_manifest_target(&prj.manifest, "arty");
 *     pkg_project_close(&prj);
 *
 * pkg_project_open = pkg_find_root + pkg_manifest_load + pkg_dep_roots.
 * `--features a,-b` on the command line maps to pkg_features_apply().
 *
 * Errors are reported through diag.h (with tern.toml / tern.lock spans when
 * there is one) and every function returns -1 after reporting.
 *
 * Layout on disk:
 *     $TERN_HOME (default ~/.tern)
 *         index/                        shallow clone of the registry index
 *         cache/<name>/<commit>/        package checkouts (immutable)
 *     <project>/vendor/<name>/          after `tern vendor`
 */
#ifndef TERN_PKG_H
#define TERN_PKG_H

#include "../base/common.h"
#include "../base/arena.h"
#include "../base/diag.h"
#include "../base/str.h"
#include "../base/vec.h"

#define PKG_MANIFEST_FILE "tern.toml"
#define PKG_LOCK_FILE "tern.lock"
#define PKG_DEFAULT_INDEX "https://github.com/tern-lang/registry"
#define PKG_VENDOR_DIR "vendor"
#define PKG_VENDOR_MARKER ".tern-vendor"   /* vendor/<name>/.tern-vendor holds the commit */

/* ================================================================ TOML === */
/* A TOML 1.0 subset: tables, dotted headers, arrays of tables, dotted keys,
 * bare/quoted keys, basic/literal (multi-line) strings, integers (dec, 0x,
 * 0o, 0b, `_`), booleans, (multi-line) arrays and inline tables. Floats and
 * dates are rejected with an explanation. */

typedef enum TomlKind { TOML_STRING, TOML_INT, TOML_BOOL, TOML_ARRAY, TOML_TABLE } TomlKind;

typedef struct TomlValue TomlValue;
struct TomlValue {
    TomlKind kind;
    Span span;              /* the value; a table defined by a header: the header */
    const char *key;        /* member name when inside a table, else NULL */
    Span key_span;
    const char *str;        /* TOML_STRING: NUL-terminated (may contain NULs: see str_len) */
    size_t str_len;
    int64_t num;            /* TOML_INT */
    int bval;               /* TOML_BOOL */
    TomlValue *first, *last; /* children: table members / array items, in order */
    TomlValue *next;        /* next sibling */
    size_t count;           /* number of children */
    unsigned flags;         /* parser bookkeeping */
};

/* Parse the registered source `src_id`. Returns the root table (allocated in
 * `a`) or NULL if there were errors (all reported). */
TomlValue *toml_parse(Arena *a, uint32_t src_id);
/* Read, register and parse a file. A missing/unreadable file is reported. */
TomlValue *toml_parse_file(Arena *a, const char *path, uint32_t *src_id_out);
const TomlValue *toml_get(const TomlValue *table, const char *key);   /* NULL-safe */
const char *toml_kind_name(TomlKind k);       /* "string", "integer", ... */
/* Writer helpers: a quoted basic string with escapes; a key (bare if possible). */
void toml_put_string(StrBuf *sb, const char *s);
void toml_put_key(StrBuf *sb, const char *key);

/* ============================================================ versions === */

typedef struct PkgVersion {
    uint64_t major, minor, patch;
    char pre[48];           /* pre-release ("rc.1"), "" for a release; build metadata dropped */
} PkgVersion;

/* "1.2.3", "1.2.3-rc.1", "1.2.3+build"; with allow_v also "v1.2.3" (tags). */
int pkg_version_parse(const char *s, PkgVersion *v, int allow_v);
int pkg_version_cmp(const PkgVersion *a, const PkgVersion *b);   /* semver precedence */
#define PKG_VERSION_STRLEN 128                                   /* buffer size for pkg_version_str */
char *pkg_version_str(const PkgVersion *v, char buf[PKG_VERSION_STRLEN]); /* returns buf */

typedef enum PkgOp { PKG_OP_EQ, PKG_OP_GT, PKG_OP_GE, PKG_OP_LT, PKG_OP_LE } PkgOp;

#define PKG_REQ_MAX 16
typedef struct PkgReq {
    int n;
    struct PkgReqCmp {
        PkgOp op;
        PkgVersion v;
    } cmp[PKG_REQ_MAX];     /* all must hold */
} PkgReq;

/* Requirement syntax (Cargo-like): "1.2" and "^1.2" (>=1.2.0, <2.0.0; for
 * 0.x the minor is the breaking component), "~1.2" (>=1.2.0, <1.3.0),
 * "=1.2.3", ">=1.0, <2", ">1", "<=1.4", "*". Comma separates comparators that
 * must all hold. On error returns -1 and writes a message to err. */
int pkg_req_parse(const char *s, PkgReq *r, char *err, size_t errlen);
/* Pre-release versions only match when a comparator names the same
 * major.minor.patch with a pre-release. */
int pkg_req_matches(const PkgReq *r, const PkgVersion *v);
int pkg_req_matches_str(const char *req, const char *version);   /* convenience; 0 on parse errors */

/* ============================================================ manifest === */

typedef enum PkgDepKind { PKG_DEP_REGISTRY, PKG_DEP_GIT, PKG_DEP_PATH } PkgDepKind;

typedef struct TernDep {
    const char *name;       /* key in [dependencies]; also the `use` root (with '-' -> '_') */
    PkgDepKind kind;
    const char *req;        /* version requirement; registry: always set ("*" if omitted); git: optional */
    const char *git;        /* git URL */
    const char *tag, *rev, *branch;  /* git pin: at most one (none: newest semver tag, else HEAD) */
    const char *path;       /* path dependency as written (relative to the manifest's directory) */
    Span span;
} TernDep;

typedef struct TernFeature {
    const char *name;
    int default_on;         /* value in [features] */
    int enabled;            /* default_on unless changed by pkg_features_apply */
    Span span;
} TernFeature;

typedef struct TernTargetKV {
    const char *key;
    const TomlValue *value; /* any TOML value */
} TernTargetKV;

typedef struct TernTarget {
    const char *name;       /* [target.<name>] */
    const char *device;     /* NULL if absent */
    const char *top;
    const char *backend;
    const char *std;        /* "sv" (default) or "verilog2005" */
    TernTargetKV *extra;    /* every other key, in file order */
    size_t nextra;
    Span span;
} TernTarget;

typedef struct TernManifest {
    const char *path;       /* the tern.toml path as given */
    const char *root;       /* absolute, normalized project directory */
    uint32_t src;           /* source id of tern.toml */
    const char *name, *version, *description, *license, *repository;
    const char **authors;
    size_t nauthors;
    const char *registry_index;   /* [registry] index, or NULL */
    const char *registry_fork;    /* [registry] fork (push URL for `tern publish`), or NULL */
    TernDep *deps;
    size_t ndeps;
    TernFeature *features;
    size_t nfeatures;
    TernTarget *targets;
    size_t ntargets;
    const TomlValue *toml;  /* the whole document */
    Arena arena;            /* owns everything above */
} TernManifest;

/* Load and validate a manifest. 0 on success, -1 on errors (reported). */
int pkg_manifest_load(const char *path, TernManifest *m);
/* Same, from text already registered with src_add (`root` = project dir). */
int pkg_manifest_parse(uint32_t src_id, const char *root, TernManifest *m);
void pkg_manifest_free(TernManifest *m);
/* Nearest directory at or above start_dir containing tern.toml (absolute,
 * heap), or NULL. */
char *pkg_find_root(const char *start_dir);
const TernDep *pkg_manifest_dep(const TernManifest *m, const char *name);
/* Target by name; name NULL: the only target, or the one named "default". */
const TernTarget *pkg_manifest_target(const TernManifest *m, const char *name);
/* Value of cfg::<name>: returns 1 and sets *value if the feature is declared. */
int pkg_manifest_feature(const TernManifest *m, const char *name, int *value);
/* Apply a command-line feature list "a,b,-c" (or "no-c"): enable a and b,
 * disable c. Unknown names are errors. 0 / -1. */
int pkg_features_apply(TernManifest *m, const char *list);
int pkg_name_valid(const char *name);   /* [A-Za-z_][A-Za-z0-9_-]* */
/* The `use` root of a package name: '-' -> '_' (heap). */
char *pkg_name_ident(const char *name);

/* Textual edit of tern.toml: set `name = <value_toml>` in [dependencies],
 * replacing an existing entry line or appending one (creating the section
 * at the end of the file if needed); the rest of the file is preserved
 * byte for byte. Returns 0, or -1 with a heap message in *err. */
int pkg_manifest_edit_dep(const char *text, size_t len, const char *name, const char *value_toml,
                          StrBuf *out, char **err);

/* ================================================================ lock === */

typedef enum PkgSourceKind { PKG_SRC_REGISTRY, PKG_SRC_GIT, PKG_SRC_PATH } PkgSourceKind;

typedef struct PkgLocked {
    const char *name;
    const char *version;    /* from the package's tern.toml / the tag */
    PkgSourceKind kind;
    const char *url;        /* git URL; path deps: path relative to the project root */
    const char *tag, *rev, *branch;   /* the pin (registry packages: the tag) */
    const char *commit;     /* full hash; NULL for path deps */
    const char **deps;      /* names of its dependencies */
    size_t ndeps;
} PkgLocked;

typedef VEC(PkgLocked) PkgLockedVec;

typedef struct PkgLock {
    PkgLockedVec pkgs;      /* sorted by name when written */
    int vendored;           /* `tern vendor` ran: builds read vendor/<name>/ */
    Arena arena;            /* strings */
} PkgLock;

/* 0 = read, 1 = file absent (lock is empty), -1 = errors (reported). */
int pkg_lock_read(const char *path, PkgLock *lock);
void pkg_lock_format(const PkgLock *lock, StrBuf *out);
int pkg_lock_write(const char *path, const PkgLock *lock);   /* only if changed */
void pkg_lock_free(PkgLock *lock);
const PkgLocked *pkg_lock_find(const PkgLock *lock, const char *name);
const char *pkg_source_prefix(PkgSourceKind k);   /* "registry+", "git+", "path+" */
/* 1 if every [dependencies] entry of m is in `lock` with the same source,
 * pin and a matching version, else 0 (with report: one error per entry). */
int pkg_lock_satisfies(const TernManifest *m, const PkgLock *lock, int report);

/* ========================================================== resolution === */

typedef struct PkgAvail {
    PkgVersion version;
    const char *tag;        /* git tag ("v1.2.0") */
    const char *commit;     /* commit recorded by the index / ls-remote, or NULL */
    int yanked;
} PkgAvail;

typedef VEC(PkgAvail) PkgAvailVec;

typedef struct PkgAvailList {
    const char *git;        /* registry: the package's git URL */
    const char *description;
    PkgAvailVec items;      /* any order */
} PkgAvailList;

typedef struct PkgNode {
    const char *name;
    PkgSourceKind kind;
    const char *url;        /* git URL; path deps: absolute directory */
    const char *tag, *rev, *branch;
    const char *commit;     /* expected commit (index/lock) on input; actual after fetch */
    const char *root;       /* set by fetch: directory holding its tern.toml */
    const char *version;    /* chosen version (registry/git tags) or NULL; fetch may fill it */
} PkgNode;

/* Where packages come from; lets the unit tests resolve without git.
 * Strings handed back must live in `a` (or longer). */
typedef struct PkgProvider {
    void *user;
    /* Versions and git URL of registry package `name`: 0, 1 = unknown package
     * (resolver reports it), -1 = error (reported). */
    int (*registry)(void *user, const char *name, Arena *a, PkgAvailList *out);
    /* Semver tags of a git repository: 0 / -1. */
    int (*tags)(void *user, const char *url, Arena *a, PkgAvailList *out);
    /* Make the package available locally: set node->root (and node->commit). */
    int (*fetch)(void *user, PkgNode *node, Arena *a);
} PkgProvider;

typedef struct PkgResolveOpts {
    const PkgLock *prev;            /* previous lock: keep its choices when still valid */
    const char *const *update;      /* names allowed to move (NULL-terminated), or NULL */
    int update_all;                 /* ignore prev for every package */
} PkgResolveOpts;

/* Resolve m's dependency graph (transitively) into `out` (must be zeroed).
 * Newest matching version wins unless the previous lock already satisfies
 * every requirement; conflicts are explained. 0 / -1. */
int pkg_resolve(const TernManifest *m, const PkgResolveOpts *o, const PkgProvider *p, PkgLock *out);
/* Make every locked package available locally (cache or vendor/), fetching
 * the missing ones through p at their locked commits. 0 / -1. */
int pkg_lock_fetch(const TernManifest *m, const PkgLock *lock, const PkgProvider *p);

/* ============================================================== layout === */

char *pkg_home_dir(void);                         /* $TERN_HOME or ~/.tern */
char *pkg_index_dir(void);                        /* <home>/index */
char *pkg_cache_dir(const char *name, const char *commit);   /* <home>/cache/<name>/<commit> */
/* $TERN_INDEX, else m's [registry] index, else PKG_DEFAULT_INDEX (heap). */
char *pkg_index_url(const TernManifest *m);
/* Lexical relative path from directory base to path, with ".." (heap). */
char *pkg_path_rel(const char *path, const char *base);

typedef struct PkgDepRoot {
    const char *name;       /* package name as in tern.toml/tern.lock */
    const char *ident;      /* `use` root: name with '-' -> '_' */
    const char *version;
    const char *root;       /* absolute directory with its tern.toml */
    const char *src;        /* root/src */
    int vendored;           /* read from vendor/ */
    int direct;             /* listed in the project's own [dependencies] */
} PkgDepRoot;

typedef struct PkgDepRoots {
    PkgDepRoot *items;
    size_t len;
    Arena arena;
} PkgDepRoots;

/* Source roots of every locked dependency, from tern.lock + the cache or
 * vendor/; no network. Errors (reported) when tern.lock is missing or stale
 * or a package is not fetched ("run `tern add`"). 0 / -1. */
int pkg_dep_roots(const TernManifest *m, PkgDepRoots *out);
void pkg_dep_roots_free(PkgDepRoots *r);

typedef struct PkgProject {
    TernManifest manifest;
    PkgDepRoots deps;
} PkgProject;

/* 1 = project loaded, 0 = no tern.toml at or above start_dir, -1 = errors. */
int pkg_project_open(const char *start_dir, PkgProject *p);
void pkg_project_close(PkgProject *p);

/* ================================================================= git === */

int pkg_git_available(void);
/* Run `git <args...>` (NULL-terminated) in cwd (NULL = current). Captures
 * stdout into *out (heap, trailing newline trimmed; may be NULL) and stderr
 * into *err. Returns git's exit code, -1 if git could not be started. */
int pkg_git(const char *cwd, const char *const *args, char **out, char **err);
/* Semver tags of `url` via `git ls-remote --tags` (peeled commits). */
int pkg_git_tags(const char *url, Arena *a, PkgAvailList *out);
/* Check out url at tag/branch/rev (at most one; none = default branch) into
 * the directory `dest` (must not exist), without .git; *commit_out = HEAD. */
int pkg_git_fetch_into(const char *url, const char *tag, const char *branch, const char *rev,
                       const char *dest, char **commit_out);
/* Make sure <home>/index is a fresh shallow clone of index_url. */
int pkg_index_sync(const char *index_url);

/* Registry index entry: <dir>/<first-two-letters>/<name>.toml. */
typedef struct PkgIndexEntry {
    const char *name, *git, *description;
    PkgAvailList versions;  /* versions.git == git */
    Arena arena;
} PkgIndexEntry;

char *pkg_index_rel_path(const char *name);   /* "ax/axi.toml" (heap) */
/* 0 = read, 1 = no such package, -1 = malformed (reported). */
int pkg_index_read(const char *index_dir, const char *name, PkgIndexEntry *e);
void pkg_index_format(const PkgIndexEntry *e, StrBuf *out);
void pkg_index_entry_free(PkgIndexEntry *e);

/* The provider used by the commands: registry lookups in the synced index,
 * tags via ls-remote, checkouts into the cache (vendor/ is used when it
 * holds the wanted commit). */
typedef struct PkgGitProvider {
    const char *index_url;
    const char *project_root;
    int index_synced;
    int offline;            /* never sync the index (it must exist) */
    int quiet;
    /* a fetch already made in this run (`tern add` learns a git dependency's
     * name first): a node for the same url and tag reuses its commit */
    const char *hint_url, *hint_tag, *hint_commit;
} PkgGitProvider;

PkgProvider pkg_git_provider(PkgGitProvider *ctx);

/* Progress line "    Fetching axi v1.2.0" on stderr (verb right-aligned). */
void pkg_status(const char *verb, const char *fmt, ...) TERN_PRINTF(2, 3);
void pkg_set_quiet(int quiet);
int pkg_quiet(void);

#endif /* TERN_PKG_H */
