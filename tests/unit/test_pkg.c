/* test_pkg.c — unit tests for src/pkg (TOML, manifest, semver, resolution,
 * tern.lock, textual manifest edits, dependency roots) and an end-to-end
 * test of `tern new/add/update/vendor/publish` against local bare git
 * repositories (skipped when git is not installed).
 *
 *     cc -o build/test_pkg tests/unit/test_pkg.c && build/test_pkg
 *
 * A single translation unit that includes the sources under test. */
#include "../../src/os/os_posix.c"
#include "../../src/os/os_win32.c"
#include "../../src/os/os_common.c"
#include "../../src/base/common.c"
#include "../../src/base/arena.c"
#include "../../src/base/str.c"
#include "../../src/base/map.c"
#include "../../src/base/bignum.c"
#include "../../src/base/json.c"
#include "../../src/base/utf8.c"
#include "../../src/base/diag.c"
#include "../../src/base/args.c"
#include "../../src/pkg/toml.c"
#include "../../src/pkg/manifest.c"
#include "../../src/pkg/lock.c"
#include "../../src/pkg/git.c"
#include "../../src/pkg/cmd_pkg.c"

int cmd_print_usage(const char *name)
{
    TERN_UNUSED(name);
    return 0;
}

/* ------------------------------------------------------------ framework */

static int tp_checks, tp_fails;
static StrBuf tp_diags; /* captured diagnostics (short format) */

static void tp_check(int ok, const char *expr, const char *file, int line)
{
    tp_checks++;
    if (!ok) {
        diag_flush();
        tp_fails++;
        if (tp_fails <= 60) {
            fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
            if (tp_diags.len)
                fprintf(stderr, "  diagnostics so far:\n%s", sb_cstr(&tp_diags));
        }
    }
}

#define TP_CHECK(c) tp_check((c) ? 1 : 0, #c, __FILE__, __LINE__)
#define TP_STREQ(a, b) tp_streq((a), (b), #a, __FILE__, __LINE__)
#define TP_HAS(needle) tp_has((needle), __FILE__, __LINE__)

static void tp_streq(const char *a, const char *b, const char *expr, const char *file, int line)
{
    int ok = a && b && strcmp(a, b) == 0;
    tp_checks++;
    if (!ok) {
        tp_fails++;
        if (tp_fails <= 60)
            fprintf(stderr, "%s:%d: %s\n    got:      \"%s\"\n    expected: \"%s\"\n", file, line, expr,
                    a ? a : "(null)", b ? b : "(null)");
    }
}

/* the captured diagnostics contain `needle` */
static void tp_has(const char *needle, const char *file, int line)
{
    diag_flush();
    tp_checks++;
    if (!strstr(sb_cstr(&tp_diags), needle)) {
        tp_fails++;
        if (tp_fails <= 60)
            fprintf(stderr, "%s:%d: diagnostics lack \"%s\"; got:\n%s\n", file, line, needle, sb_cstr(&tp_diags));
    }
}

static void tp_sink(const Diagnostic *d, void *user)
{
    uint32_t i;
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_SHORT, 0, &tp_diags);
    for (i = 0; i < d->nsubs; i++)
        sb_printf(&tp_diags, "  %s: %s\n", diag_level_name(d->subs[i].level), d->subs[i].msg);
}

static void tp_reset_diags(void)
{
    diag_flush();
    sb_reset(&tp_diags);
    diag_reset();
}

static uint32_t tp_src(const char *name, const char *text)
{
    return src_add(name, text, strlen(text));
}

static char *tp_tmp;   /* scratch directory for this run */

static char *tp_path(const char *rel)
{
    return os_path_join(tp_tmp, rel);
}

static void tp_write(const char *rel, const char *text)
{
    char *p = tp_path(rel), *d = os_path_dirname(p);
    os_mkdir_p(d);
    if (os_write_file(p, text, strlen(text)) != 0)
        fprintf(stderr, "cannot write %s\n", p);
    free(p);
    free(d);
}

/* ------------------------------------------------------------------ TOML */

static void test_toml_ok(void)
{
    Arena a = {0};
    const char *text =
        "# comment\n"
        "title = \"tern\" # trailing\n"
        "'quoted key' = 'lit\\eral'\n"
        "\"esc\" = \"tab\\tnl\\nq\\\"u\\u00e9\"\n"
        "n = -42\nh = 0xFF\no = 0o17\nb = 0b101\nbig = 1_000_000\n"
        "yes = true\nno = false\n"
        "arr = [ 1, 2,\n  3, # c\n]\n"
        "nested = [[1], [\"a\", \"b\"]]\n"
        "dotted.key = 5\n"
        "ml = \"\"\"\nline1\nline2\\\n   joined\"\"\"\n"
        "\n[package]\nname = \"x\"\n"
        "[target.arty]\ndevice = \"xc7a35t\"\n"
        "[target.icebreaker]\ndevice = \"ice40\"\n"
        "[dependencies]\nuart = { git = \"https://g/u\", tag = \"v2.0.1\" }\nempty = {}\n"
        "[[package2]]\nname = \"a\"\n[[package2]]\nname = \"b\"\n[package2.sub]\nk = 1\n"
        "[x.y.z]\nv = 1\n[x]\nw = 2\n";
    uint32_t id = tp_src("ok.toml", text);
    const TomlValue *root = toml_parse(&a, id), *v, *t;
    TP_CHECK(root != NULL);
    if (!root)
        return;
    TP_STREQ(toml_get(root, "title")->str, "tern");
    TP_STREQ(toml_get(root, "quoted key")->str, "lit\\eral");
    TP_STREQ(toml_get(root, "esc")->str, "tab\tnl\nq\"u\xc3\xa9");
    TP_CHECK(toml_get(root, "n")->num == -42);
    TP_CHECK(toml_get(root, "h")->num == 255);
    TP_CHECK(toml_get(root, "o")->num == 15);
    TP_CHECK(toml_get(root, "b")->num == 5);
    TP_CHECK(toml_get(root, "big")->num == 1000000);
    TP_CHECK(toml_get(root, "yes")->bval == 1 && toml_get(root, "no")->bval == 0);
    v = toml_get(root, "arr");
    TP_CHECK(v && v->kind == TOML_ARRAY && v->count == 3 && v->last->num == 3);
    v = toml_get(root, "nested");
    TP_CHECK(v && v->count == 2 && v->last->count == 2 && strcmp(v->last->last->str, "b") == 0);
    v = toml_get(toml_get(root, "dotted"), "key");
    TP_CHECK(v && v->num == 5);
    TP_STREQ(toml_get(root, "ml")->str, "line1\nline2joined");
    t = toml_get(root, "target");
    TP_CHECK(t && t->count == 2);
    TP_STREQ(toml_get(toml_get(t, "arty"), "device")->str, "xc7a35t");
    v = toml_get(toml_get(root, "dependencies"), "uart");
    TP_CHECK(v && v->kind == TOML_TABLE && strcmp(toml_get(v, "tag")->str, "v2.0.1") == 0);
    TP_CHECK(v && v->span.line == 29 && v->span.col == 8);
    v = toml_get(root, "package2");
    TP_CHECK(v && v->kind == TOML_ARRAY && v->count == 2);
    TP_STREQ(toml_get(v->last, "name")->str, "b");
    TP_CHECK(toml_get(toml_get(v->last, "sub"), "k")->num == 1);
    TP_CHECK(toml_get(toml_get(root, "x"), "w")->num == 2);
    TP_CHECK(toml_get(toml_get(toml_get(toml_get(root, "x"), "y"), "z"), "v")->num == 1);
    arena_release(&a);
}

static void tp_toml_err(const char *text, const char *expect, int line, int col)
{
    Arena a = {0};
    char loc[64];
    uint32_t id;
    tp_reset_diags();
    id = tp_src("bad.toml", text);
    TP_CHECK(toml_parse(&a, id) == NULL);
    TP_HAS(expect);
    snprintf(loc, sizeof loc, "bad.toml:%d:%d:", line, col);
    TP_HAS(loc);
    arena_release(&a);
}

static void test_toml_errors(void)
{
    tp_toml_err("a = 1\na = 2\n", "duplicate key `a`", 2, 1);
    tp_toml_err("[t]\nx=1\n[t]\n", "table [t] is defined more than once", 3, 1);
    tp_toml_err("x = \"abc\n", "unterminated string", 1, 5);
    tp_toml_err("x = 1.5\n", "floating-point values are not supported", 1, 5);
    tp_toml_err("x = 1979-05-27\n", "dates and times are not supported", 1, 5);
    tp_toml_err("x = [1, 2\ny = 3\n", "expected `,` or `]` in array", 2, 1);
    tp_toml_err("x = { a = 1,\n", "inline tables must be written on one line", 1, 5);
    tp_toml_err("x = hello\n", "strings must be quoted", 1, 5);
    tp_toml_err("x = True\n", "booleans are lowercase", 1, 5);
    tp_toml_err("x\n", "expected `=` after key `x`", 1, 2);
    tp_toml_err("x = 1 y = 2\n", "expected end of line after the value", 1, 7);
    tp_toml_err("x = 012\n", "leading zeros", 1, 5);
    tp_toml_err("x = \"\\q\"\n", "invalid escape sequence", 1, 6);
    tp_toml_err("x = 99999999999999999999\n", "does not fit in 64 bits", 1, 5);
    tp_toml_err("[a\n", "expected `]` to close the table header", 1, 3);
    tp_toml_err("a = 1\n[a.b]\n", "`a` is already defined as integer", 2, 2);
    tp_toml_err("p = {x = 1}\np.y = 2\n", "cannot add keys to `p`", 2, 1);
    tp_toml_err("x = \n", "expected a value after `=`", 1, 5);
    /* several errors in one file are all reported */
    tp_toml_err("a = \nb = 1.0\nc = ok\n", "floating-point", 2, 5);
    TP_HAS("strings must be quoted");
    TP_CHECK(diag_error_count() == 3);
    tp_reset_diags();
}

static void test_toml_writer(void)
{
    StrBuf sb = {0};
    toml_put_string(&sb, "a\"b\\c\nd\x01");
    TP_STREQ(sb_cstr(&sb), "\"a\\\"b\\\\c\\nd\\u0001\"");
    sb_reset(&sb);
    toml_put_key(&sb, "uart-ip");
    sb_putc(&sb, ' ');
    toml_put_key(&sb, "has space");
    TP_STREQ(sb_cstr(&sb), "uart-ip \"has space\"");
    sb_free(&sb);
}

/* -------------------------------------------------------------- manifest */

static const char *tp_spec_manifest =
    "# tern.toml\n"
    "[package]\n"
    "name = \"my-soc\"\n"
    "version = \"0.3.0\"\n"
    "description = \"A small SoC\"\n"
    "authors = [\"A <a@x>\", \"B\"]\n"
    "license = \"MIT\"\n"
    "\n"
    "[dependencies]\n"
    "axi = \"1.2\" # short name, resolved through the index\n"
    "uart-ip = { git = \"https://github.com/acme/tern-uart\", tag = \"v2.0.1\" }\n"
    "vendor = { path = \"../vendor-ip\" }\n"
    "\n"
    "[features]\n"
    "debug_ila = false\n"
    "fast_sim = true\n"
    "\n"
    "[target.arty]\n"
    "device = \"xc7a35t\"\n"
    "top = \"Top\"\n"
    "backend = \"vivado\"\n"
    "clock_hz = 100_000_000\n"
    "pins = \"arty.xdc\"\n"
    "\n"
    "[target.sim]\n"
    "top = \"Top\"\n"
    "std = \"verilog2005\"\n"
    "\n"
    "[registry]\n"
    "index = \"https://example.com/index\"\n";

static void test_manifest(void)
{
    TernManifest m;
    const TernTarget *t;
    int v = -1;
    uint32_t id;
    tp_reset_diags();
    id = tp_src("proj/tern.toml", tp_spec_manifest);
    TP_CHECK(pkg_manifest_parse(id, "proj", &m) == 0);
    TP_STREQ(m.name, "my-soc");
    TP_STREQ(m.version, "0.3.0");
    TP_STREQ(m.description, "A small SoC");
    TP_CHECK(m.nauthors == 2 && strcmp(m.authors[1], "B") == 0);
    TP_STREQ(m.license, "MIT");
    TP_CHECK(m.ndeps == 3);
    if (m.ndeps == 3) {
        TP_CHECK(m.deps[0].kind == PKG_DEP_REGISTRY);
        TP_STREQ(m.deps[0].req, "1.2");
        TP_CHECK(m.deps[1].kind == PKG_DEP_GIT);
        TP_STREQ(m.deps[1].name, "uart-ip");
        TP_STREQ(m.deps[1].tag, "v2.0.1");
        TP_STREQ(m.deps[1].git, "https://github.com/acme/tern-uart");
        TP_CHECK(m.deps[2].kind == PKG_DEP_PATH);
        TP_STREQ(m.deps[2].path, "../vendor-ip");
    }
    TP_CHECK(pkg_manifest_feature(&m, "debug_ila", &v) == 1 && v == 0);
    TP_CHECK(pkg_manifest_feature(&m, "fast_sim", &v) == 1 && v == 1);
    TP_CHECK(pkg_manifest_feature(&m, "nope", &v) == 0);
    TP_CHECK(pkg_features_apply(&m, "debug_ila,-fast_sim") == 0);
    TP_CHECK(pkg_manifest_feature(&m, "debug_ila", &v) == 1 && v == 1);
    TP_CHECK(pkg_manifest_feature(&m, "fast_sim", &v) == 1 && v == 0);
    TP_CHECK(pkg_features_apply(&m, "no-debug_ila") == 0 && pkg_manifest_feature(&m, "debug_ila", &v) && v == 0);
    TP_CHECK(pkg_features_apply(&m, "bogus") != 0);
    TP_HAS("unknown feature `bogus`");
    TP_CHECK(m.ntargets == 2);
    t = pkg_manifest_target(&m, "arty");
    TP_CHECK(t && strcmp(t->device, "xc7a35t") == 0 && strcmp(t->top, "Top") == 0 && strcmp(t->std, "sv") == 0);
    TP_CHECK(t && strcmp(t->backend, "vivado") == 0 && t->nextra == 2);
    TP_CHECK(t && strcmp(t->extra[0].key, "clock_hz") == 0 && t->extra[0].value->num == 100000000);
    t = pkg_manifest_target(&m, "sim");
    TP_CHECK(t && strcmp(t->std, "verilog2005") == 0 && !t->device);
    TP_CHECK(pkg_manifest_target(&m, NULL) == NULL); /* two targets, none named default */
    TP_STREQ(m.registry_index, "https://example.com/index");
    TP_CHECK(tern_ends_with(m.root, "proj"));
    pkg_manifest_free(&m);
    tp_reset_diags();
}

static void tp_manifest_err(const char *text, const char *expect)
{
    TernManifest m;
    uint32_t id;
    tp_reset_diags();
    id = tp_src("tern.toml", text);
    TP_CHECK(pkg_manifest_parse(id, ".", &m) != 0);
    TP_HAS(expect);
    pkg_manifest_free(&m);
}

static void test_manifest_errors(void)
{
    const char *pk = "[package]\nname = \"x\"\nversion = \"1.0.0\"\n";
    StrBuf sb = {0};
    tp_manifest_err("[dependencies]\n", "tern.toml has no [package] section");
    tp_manifest_err("[package]\nversion = \"1.0.0\"\n", "[package] has no `name`");
    tp_manifest_err("[package]\nname = \"x\"\nversion = \"1.0\"\n", "invalid version `1.0`");
    tp_manifest_err("[package]\nname = \"9x\"\nversion = \"1.0.0\"\n", "invalid package name `9x`");
    tp_manifest_err("[package]\nname = \"module\"\nversion = \"1.0.0\"\n", "reserved word");
    tp_manifest_err("[package]\nname = 5\nversion = \"1.0.0\"\n", "package.name must be a string");
#define TP_WITH(extra, expect) \
    do { \
        sb_reset(&sb); \
        sb_puts(&sb, pk); \
        sb_puts(&sb, extra); \
        tp_manifest_err(sb_cstr(&sb), expect); \
    } while (0)
    TP_WITH("[dependencies]\na = { tag = \"v1\" }\n", "`tag`, `rev` and `branch` require `git`");
    TP_WITH("[dependencies]\na = { git = \"u\", tag = \"v1\", rev = \"abc\" }\n", "only one of `tag`, `rev` and `branch`");
    TP_WITH("[dependencies]\na = { git = \"u\", path = \"p\" }\n", "has both `git` and `path`");
    TP_WITH("[dependencies]\na = \"^^1\"\n", "invalid version requirement \"^^1\"");
    TP_WITH("[dependencies]\na = { gti = \"u\" }\n", "unknown key `gti` in dependency `a`");
    TP_WITH("[dependencies]\na = 3\n", "dependencies.a must be a version string or an inline table");
    TP_WITH("[features]\ndebug = \"yes\"\n", "feature `debug` must be a boolean");
    TP_WITH("[features]\ndebug-ila = true\n", "feature name `debug-ila` is not a valid identifier");
    TP_WITH("[target.arty]\nstd = \"vhdl\"\n", "invalid output dialect \"vhdl\"");
    TP_WITH("[target]\narty = 1\n", "target.arty must be a table");
#undef TP_WITH
    /* unknown sections/keys are warnings, not errors */
    {
        TernManifest m;
        uint32_t id;
        tp_reset_diags();
        sb_reset(&sb);
        sb_puts(&sb, "[package]\nname = \"x\"\nversion = \"1.0.0\"\nlicence = \"MIT\"\n[dependency]\n");
        id = tp_src("tern.toml", sb_cstr(&sb));
        TP_CHECK(pkg_manifest_parse(id, ".", &m) == 0);
        TP_HAS("unknown key `licence` in [package]");
        TP_HAS("did you mean `license`?");
        TP_HAS("unknown section `dependency`");
        TP_HAS("did you mean [dependencies]?");
        pkg_manifest_free(&m);
    }
    sb_free(&sb);
    tp_reset_diags();
}

/* ---------------------------------------------------------- manifest edit */

static void tp_edit(const char *in, const char *name, const char *val, const char *expect)
{
    StrBuf out = {0};
    char *err = NULL;
    int r = pkg_manifest_edit_dep(in, strlen(in), name, val, &out, &err);
    TP_CHECK(r == 0);
    if (r != 0)
        fprintf(stderr, "  edit error: %s\n", err ? err : "?");
    TP_STREQ(sb_cstr(&out), expect);
    free(err);
    sb_free(&out);
}

static void test_manifest_edit(void)
{
    StrBuf out = {0};
    char *err = NULL;
    /* append to an existing section, keeping comments and other sections */
    tp_edit("[package]\nname = \"x\"\n\n[dependencies]\n# deps\naxi = \"1.2\"\n\n[features]\na = true\n", "uart",
            "\"2.0.0\"",
            "[package]\nname = \"x\"\n\n[dependencies]\n# deps\naxi = \"1.2\"\nuart = \"2.0.0\"\n\n[features]\na = true\n");
    /* replace an entry in place, keeping its trailing comment */
    tp_edit("[dependencies]\n  axi = \"1.2\"   # the bus\nb = \"1\"\n", "axi", "\"1.3.0\"",
            "[dependencies]\n  axi = \"1.3.0\" # the bus\nb = \"1\"\n");
    /* quoted key, and a key that merely shares a prefix */
    tp_edit("[dependencies]\naxi-lite = \"1\"\n\"axi\" = \"1\"\n", "axi", "\"2\"",
            "[dependencies]\naxi-lite = \"1\"\naxi = \"2\"\n");
    /* create the section */
    tp_edit("[package]\nname = \"x\"", "axi", "\"1\"", "[package]\nname = \"x\"\n\n[dependencies]\naxi = \"1\"\n");
    tp_edit("", "axi", "\"1\"", "[dependencies]\naxi = \"1\"\n");
    /* empty section at the end of the file without a newline */
    tp_edit("[dependencies]", "axi", "\"1\"", "[dependencies]\naxi = \"1\"\n");
    /* CRLF files stay CRLF */
    tp_edit("[package]\r\nname = \"x\"\r\n[dependencies]\r\n", "axi", "{ path = \"../a\" }",
            "[package]\r\nname = \"x\"\r\n[dependencies]\r\naxi = { path = \"../a\" }\r\n");
    /* names needing quotes */
    tp_edit("[dependencies]\n", "odd.name", "\"1\"", "[dependencies]\n\"odd.name\" = \"1\"\n");
    TP_CHECK(pkg_manifest_edit_dep("[dependencies.axi]\nversion = \"1\"\n", 31, "axi", "\"2\"", &out, &err) != 0);
    TP_CHECK(err && strstr(err, "edit tern.toml by hand"));
    free(err);
    sb_free(&out);
}

/* -------------------------------------------------------------- versions */

static void test_semver(void)
{
    static const char *const ordered[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
        "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "1.10.0", "2.0.0",
    };
    PkgVersion a, b;
    char buf[PKG_VERSION_STRLEN];
    size_t i;
    for (i = 0; i + 1 < TERN_ARRAY_LEN(ordered); i++) {
        TP_CHECK(pkg_version_parse(ordered[i], &a, 0) == 0);
        TP_CHECK(pkg_version_parse(ordered[i + 1], &b, 0) == 0);
        TP_CHECK(pkg_version_cmp(&a, &b) < 0);
        TP_CHECK(pkg_version_cmp(&b, &a) > 0);
        TP_CHECK(pkg_version_cmp(&a, &a) == 0);
    }
    TP_CHECK(pkg_version_parse("1.2.3+build.5", &a, 0) == 0);
    TP_STREQ(pkg_version_str(&a, buf), "1.2.3");
    TP_CHECK(pkg_version_parse("v1.2.3", &a, 0) != 0);
    TP_CHECK(pkg_version_parse("v1.2.3", &a, 1) == 0 && a.major == 1 && a.patch == 3);
    TP_CHECK(pkg_version_parse("1.2", &a, 0) != 0);
    TP_CHECK(pkg_version_parse("01.2.3", &a, 0) != 0);
    TP_CHECK(pkg_version_parse("1.2.3-", &a, 0) != 0);
    TP_CHECK(pkg_version_parse("1.2.3-01", &a, 0) != 0);
    TP_CHECK(pkg_version_parse("1.2.3 ", &a, 0) != 0);
}

static void test_requirements(void)
{
    static const struct {
        const char *req, *ver;
        int ok;
    } cases[] = {
        {"1.2", "1.2.0", 1}, {"1.2", "1.9.3", 1}, {"1.2", "2.0.0", 0}, {"1.2", "1.1.9", 0},
        {"^1.2.3", "1.2.3", 1}, {"^1.2.3", "1.2.2", 0}, {"^1.2.3", "1.99.0", 1},
        {"0.2", "0.2.9", 1}, {"0.2", "0.3.0", 0}, {"^0.2.3", "0.2.2", 0}, {"^0.0.3", "0.0.3", 1},
        {"^0.0.3", "0.0.4", 0}, {"^0", "0.9.9", 1}, {"^0", "1.0.0", 0}, {"1", "1.9.9", 1}, {"1", "2.0.0", 0},
        {"=1.2.3", "1.2.3", 1}, {"=1.2.3", "1.2.4", 0}, {"=1.2", "1.2.7", 1}, {"=1.2", "1.3.0", 0},
        {"~1.2", "1.2.9", 1}, {"~1.2", "1.3.0", 0}, {"~1.2.3", "1.2.2", 0}, {"~1", "1.9.0", 1},
        {">=1.0, <2", "1.5.0", 1}, {">=1.0, <2", "2.0.0", 0}, {">=1.0, <2", "0.9.0", 0},
        {">1.2", "1.2.9", 0}, {">1.2", "1.3.0", 1}, {">1.2.3", "1.2.4", 1}, {"<=1.4", "1.4.9", 1},
        {"<=1.4", "1.5.0", 0}, {"*", "7.0.0", 1}, {"1.*", "1.4.0", 1}, {"1.*", "2.0.0", 0},
        {"1.2", "1.3.0-rc.1", 0},          /* pre-releases need an explicit opt-in */
        {">=1.0, <2", "2.0.0-alpha", 0},
        {"^1.3.0-rc.1", "1.3.0-rc.2", 1}, {"^1.3.0-rc.1", "1.3.0", 1}, {"^1.3.0-rc.1", "1.4.0-rc.1", 0},
        {"=1.0.0-beta", "1.0.0-beta", 1}, {"v1.2", "1.2.5", 1},
    };
    static const char *const bad[] = {"", "^^1", "1.2.3.4", ">=", "1.x.3-rc", "1,", "abc", "1.2 3", "1.2-rc"};
    PkgReq r;
    char err[128];
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(cases); i++) {
        int got = pkg_req_matches_str(cases[i].req, cases[i].ver);
        tp_check(got == cases[i].ok, cases[i].req, __FILE__, __LINE__);
        if (got != cases[i].ok)
            fprintf(stderr, "  requirement \"%s\" vs %s: expected %d\n", cases[i].req, cases[i].ver, cases[i].ok);
    }
    for (i = 0; i < TERN_ARRAY_LEN(bad); i++) {
        int rc = pkg_req_parse(bad[i], &r, err, sizeof err);
        tp_check(rc != 0 && err[0], bad[i], __FILE__, __LINE__);
    }
}

/* ------------------------------------------------------------ resolution */

/* A fake registry: packages are directories under <tmp>/fake/<name>/<ver>/
 * holding a tern.toml; versions and their "commits" are listed here. */
typedef struct FakePkg {
    const char *name, *version, *deps; /* deps: lines for [dependencies] */
    int yanked;
} FakePkg;

static const FakePkg tp_fake[] = {
    {"stream", "1.0.0", "", 0},
    {"stream", "1.1.0", "", 0},
    {"stream", "1.2.0", "", 1},     /* yanked */
    {"stream", "2.0.0", "", 0},
    {"stream", "2.1.0-rc.1", "", 0},
    {"axi", "1.0.0", "stream = \"1.0\"\n", 0},
    {"axi", "1.1.0", "stream = \"1\"\n", 0},
    {"axi", "2.0.0", "stream = \"2\"\n", 0},
    {"dma", "1.0.0", "axi = \"1\"\nstream = \"~1.0\"\n", 0},
    {"needs2", "1.0.0", "stream = \">=2.0\"\n", 0},
};

static int tp_fake_registry(void *user, const char *name, Arena *a, PkgAvailList *out)
{
    size_t i;
    int found = 0;
    TERN_UNUSED(user);
    memset(out, 0, sizeof *out);
    for (i = 0; i < TERN_ARRAY_LEN(tp_fake); i++) {
        PkgAvail av;
        if (strcmp(tp_fake[i].name, name) != 0)
            continue;
        found = 1;
        memset(&av, 0, sizeof av);
        pkg_version_parse(tp_fake[i].version, &av.version, 0);
        av.tag = arena_printf(a, "v%s", tp_fake[i].version);
        av.commit = arena_printf(a, "c-%s-%s", name, tp_fake[i].version);
        av.yanked = tp_fake[i].yanked;
        vec_push(out->items, av);
    }
    out->git = arena_printf(a, "https://example.com/%s", name);
    return found ? 0 : 1;
}

static int tp_fake_tags(void *user, const char *url, Arena *a, PkgAvailList *out)
{
    PkgAvail av;
    TERN_UNUSED(user);
    memset(out, 0, sizeof *out);
    memset(&av, 0, sizeof av);
    pkg_version_parse("3.0.0", &av.version, 0);
    av.tag = "v3.0.0";
    av.commit = arena_printf(a, "c-tag-%s", url);
    vec_push(out->items, av);
    return 0;
}

static int tp_fake_fetch(void *user, PkgNode *node, Arena *a)
{
    StrBuf sb = {0};
    char *dir;
    size_t i;
    TERN_UNUSED(user);
    if (node->kind == PKG_SRC_PATH) {
        node->root = node->url;
        return 0;
    }
    if (node->kind == PKG_SRC_GIT) {
        sb_printf(&sb, "fake/git/%s", node->name);
        dir = tp_path(sb_cstr(&sb));
        sb_reset(&sb);
        sb_printf(&sb, "fake/git/%s/tern.toml", node->name);
        {
            StrBuf t = {0};
            sb_printf(&t, "[package]\nname = \"%s\"\nversion = \"3.0.0\"\n", node->name);
            tp_write(sb_cstr(&sb), sb_cstr(&t));
            sb_free(&t);
        }
        node->root = arena_strdup(a, dir);
        if (!node->commit)
            node->commit = arena_printf(a, "c-git-%s", node->tag ? node->tag : node->branch ? node->branch : "head");
        free(dir);
        sb_free(&sb);
        return 0;
    }
    for (i = 0; i < TERN_ARRAY_LEN(tp_fake); i++) {
        if (strcmp(tp_fake[i].name, node->name) == 0 && strcmp(tp_fake[i].version, node->version) == 0) {
            StrBuf t = {0};
            sb_printf(&sb, "fake/%s/%s/tern.toml", node->name, node->version);
            sb_printf(&t, "[package]\nname = \"%s\"\nversion = \"%s\"\n[dependencies]\n%s", node->name,
                      node->version, tp_fake[i].deps);
            tp_write(sb_cstr(&sb), sb_cstr(&t));
            sb_free(&t);
            sb_reset(&sb);
            sb_printf(&sb, "fake/%s/%s", node->name, node->version);
            dir = tp_path(sb_cstr(&sb));
            node->root = arena_strdup(a, dir);
            free(dir);
            sb_free(&sb);
            return 0;
        }
    }
    diag_error(span_none(), "fake: no %s %s", node->name, node->version);
    sb_free(&sb);
    return -1;
}

static const PkgProvider tp_fake_provider = {NULL, tp_fake_registry, tp_fake_tags, tp_fake_fetch};

static int tp_resolve(const char *deps, const PkgResolveOpts *o, PkgLock *out)
{
    TernManifest m;
    StrBuf t = {0};
    uint32_t id;
    int r;
    sb_printf(&t, "[package]\nname = \"root\"\nversion = \"0.1.0\"\n[dependencies]\n%s", deps);
    id = tp_src("root/tern.toml", sb_cstr(&t));
    sb_free(&t);
    if (pkg_manifest_parse(id, tp_tmp, &m) != 0) {
        pkg_manifest_free(&m);
        memset(out, 0, sizeof *out);
        return -1;
    }
    r = pkg_resolve(&m, o, &tp_fake_provider, out);
    pkg_manifest_free(&m);
    return r;
}

static const char *tp_locked_version(const PkgLock *l, const char *name)
{
    const PkgLocked *p = pkg_lock_find(l, name);
    return p ? p->version : "(absent)";
}

static void test_resolve(void)
{
    PkgLock l, l2;
    PkgResolveOpts o;
    const char *upd[] = {"stream", NULL};
    tp_reset_diags();
    /* newest matching, transitive, yanked and pre-releases skipped */
    TP_CHECK(tp_resolve("axi = \"1\"\n", NULL, &l) == 0);
    TP_STREQ(tp_locked_version(&l, "axi"), "1.1.0");
    TP_STREQ(tp_locked_version(&l, "stream"), "1.1.0");
    TP_CHECK(l.pkgs.len == 2);
    TP_CHECK(pkg_lock_find(&l, "axi") && pkg_lock_find(&l, "axi")->ndeps == 1);
    TP_STREQ(pkg_lock_find(&l, "axi")->commit, "c-axi-1.1.0");
    TP_STREQ(pkg_lock_find(&l, "axi")->tag, "v1.1.0");
    TP_STREQ(pkg_lock_find(&l, "stream")->url, "https://example.com/stream");
    pkg_lock_free(&l);
    /* diamond: dma wants stream ~1.0 and axi 1 -> axi 1.1.0 is fine, stream 1.0.0 */
    TP_CHECK(tp_resolve("dma = \"1\"\n", NULL, &l) == 0);
    TP_STREQ(tp_locked_version(&l, "stream"), "1.0.0");
    TP_STREQ(tp_locked_version(&l, "axi"), "1.1.0");
    pkg_lock_free(&l);
    /* the root's own requirement narrows the transitive one */
    TP_CHECK(tp_resolve("axi = \"1\"\nstream = \"=1.0.0\"\n", NULL, &l) == 0);
    TP_STREQ(tp_locked_version(&l, "stream"), "1.0.0");
    pkg_lock_free(&l);
    /* explicit pre-release */
    TP_CHECK(tp_resolve("stream = \"=2.1.0-rc.1\"\n", NULL, &l) == 0);
    TP_STREQ(tp_locked_version(&l, "stream"), "2.1.0-rc.1");
    pkg_lock_free(&l);
    /* conflict: axi 1.0 needs stream 1.0, needs2 needs stream >=2 */
    tp_reset_diags();
    TP_CHECK(tp_resolve("axi = \"=1.0.0\"\nneeds2 = \"1\"\n", NULL, &l) != 0);
    TP_HAS("cannot resolve `stream`: no version satisfies every requirement");
    TP_HAS("`axi` requires `stream` \"1.0\" from the index");
    TP_HAS("`needs2` requires `stream` \">=2.0\" from the index");
    TP_HAS("available versions of `stream`: 1.0.0, 1.1.0, 2.0.0, 2.1.0-rc.1");
    /* unknown package / no matching version */
    tp_reset_diags();
    TP_CHECK(tp_resolve("nosuch = \"1\"\n", NULL, &l) != 0);
    TP_HAS("package `nosuch` is not in the registry index");
    tp_reset_diags();
    TP_CHECK(tp_resolve("axi = \"5\"\n", NULL, &l) != 0);
    TP_HAS("no version of `axi` matches \"5\"");
    /* source conflict */
    tp_reset_diags();
    TP_CHECK(tp_resolve("dma = \"1\"\naxi = { git = \"https://other/axi\" }\n", NULL, &l) != 0);
    TP_HAS("`axi` is required from different sources");
    tp_reset_diags();

    /* the previous lock is kept unless asked to move */
    TP_CHECK(tp_resolve("axi = \"=1.0.0\"\nstream = \"=1.0.0\"\n", NULL, &l) == 0);
    TP_STREQ(tp_locked_version(&l, "stream"), "1.0.0");
    memset(&o, 0, sizeof o);
    o.prev = &l;
    TP_CHECK(tp_resolve("axi = \"1\"\n", &o, &l2) == 0);
    TP_STREQ(tp_locked_version(&l2, "axi"), "1.0.0");      /* kept */
    TP_STREQ(tp_locked_version(&l2, "stream"), "1.0.0");   /* kept (axi 1.0.0 wants ^1.0) */
    pkg_lock_free(&l2);
    o.update = upd;
    TP_CHECK(tp_resolve("axi = \"1\"\n", &o, &l2) == 0);
    TP_STREQ(tp_locked_version(&l2, "axi"), "1.0.0");      /* not in the update list */
    TP_STREQ(tp_locked_version(&l2, "stream"), "1.1.0");   /* moved */
    pkg_lock_free(&l2);
    o.update = NULL;
    o.update_all = 1;
    TP_CHECK(tp_resolve("axi = \"1\"\n", &o, &l2) == 0);
    TP_STREQ(tp_locked_version(&l2, "axi"), "1.1.0");
    TP_STREQ(tp_locked_version(&l2, "stream"), "1.1.0");
    pkg_lock_free(&l2);
    /* a lock entry that no longer matches moves anyway */
    o.update_all = 0;
    TP_CHECK(tp_resolve("axi = \"2\"\n", &o, &l2) == 0);
    TP_STREQ(tp_locked_version(&l2, "axi"), "2.0.0");
    TP_STREQ(tp_locked_version(&l2, "stream"), "2.0.0");
    pkg_lock_free(&l2);
    pkg_lock_free(&l);

    /* git deps: pinned, and newest tag when unpinned; path deps */
    tp_write("local/tern.toml", "[package]\nname = \"local\"\nversion = \"0.5.0\"\n[dependencies]\nstream = \"2\"\n");
    TP_CHECK(tp_resolve("uart = { git = \"https://g/uart\", tag = \"v1.0.0\" }\n"
                        "spi = { git = \"https://g/spi\" }\n"
                        "local = { path = \"local\" }\n",
                        NULL, &l) == 0);
    if (pkg_lock_find(&l, "uart") && pkg_lock_find(&l, "spi") && pkg_lock_find(&l, "local")) {
        TP_CHECK(pkg_lock_find(&l, "uart")->kind == PKG_SRC_GIT);
        TP_STREQ(pkg_lock_find(&l, "uart")->tag, "v1.0.0");
        TP_STREQ(pkg_lock_find(&l, "uart")->version, "3.0.0");   /* from its tern.toml */
        TP_STREQ(pkg_lock_find(&l, "spi")->tag, "v3.0.0");       /* newest tag */
        TP_STREQ(pkg_lock_find(&l, "spi")->commit, "c-tag-https://g/spi");
        TP_CHECK(pkg_lock_find(&l, "local")->kind == PKG_SRC_PATH);
        TP_STREQ(pkg_lock_find(&l, "local")->url, "local");
        TP_STREQ(pkg_lock_find(&l, "local")->version, "0.5.0");
        TP_STREQ(tp_locked_version(&l, "stream"), "2.0.0");      /* via the path dep */
        TP_CHECK(pkg_lock_find(&l, "local")->commit == NULL);
    } else {
        TP_CHECK(!"git/path packages missing from the lock");
    }
    pkg_lock_free(&l);
    tp_reset_diags();
}

/* ------------------------------------------------------------------ lock */

static void test_lock_roundtrip(void)
{
    PkgLock l, r;
    PkgLocked a, b;
    const char *deps[] = {"b"};
    StrBuf s1 = {0}, s2 = {0};
    char *path = tp_path("lock/tern.lock"), *dir = tp_path("lock");
    os_mkdir_p(dir);
    free(dir);
    memset(&l, 0, sizeof l);
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.name = "a";
    a.version = "1.2.3";
    a.kind = PKG_SRC_REGISTRY;
    a.url = "https://example.com/a";
    a.tag = "v1.2.3";
    a.commit = "0123456789abcdef0123456789abcdef01234567";
    a.deps = deps;
    a.ndeps = 1;
    b.name = "b";
    b.version = "0.1.0";
    b.kind = PKG_SRC_PATH;
    b.url = "../b dir";
    vec_push(l.pkgs, b);
    vec_push(l.pkgs, a);
    l.vendored = 1;
    pkg_lock_format(&l, &s1);
    TP_CHECK(strstr(s1.data, "source = \"registry+https://example.com/a\"") != NULL);
    TP_CHECK(strstr(s1.data, "vendored = true") != NULL);
    TP_CHECK(strstr(s1.data, "name = \"a\"") < strstr(s1.data, "name = \"b\"")); /* sorted */
    TP_CHECK(pkg_lock_write(path, &l) == 0);
    TP_CHECK(pkg_lock_read(path, &r) == 0);
    TP_CHECK(r.pkgs.len == 2 && r.vendored == 1);
    TP_CHECK(pkg_lock_find(&r, "a") && pkg_lock_find(&r, "a")->ndeps == 1);
    TP_STREQ(pkg_lock_find(&r, "b")->url, "../b dir");
    TP_CHECK(pkg_lock_find(&r, "b")->kind == PKG_SRC_PATH);
    pkg_lock_format(&r, &s2);
    TP_STREQ(sb_cstr(&s2), sb_cstr(&s1));
    pkg_lock_free(&r);
    vec_free(l.pkgs);
    {
        char *missing = tp_path("lock/none.lock");
        TP_CHECK(pkg_lock_read(missing, &r) == 1);
        pkg_lock_free(&r);
        free(missing);
    }
    tp_reset_diags();
    tp_write("lock/bad.lock", "version = 1\n[[package]]\nname = \"x\"\nversion = \"1.0.0\"\nsource = \"ftp+x\"\ncommit = \"c\"\n");
    {
        char *bad = tp_path("lock/bad.lock");
        TP_CHECK(pkg_lock_read(bad, &r) == -1);
        TP_HAS("unknown source \"ftp+x\" in tern.lock");
        pkg_lock_free(&r);
        free(bad);
    }
    tp_reset_diags();
    sb_free(&s1);
    sb_free(&s2);
    free(path);
}

static void test_paths(void)
{
    char *r;
    r = pkg_path_rel("/a/b/c", "/a/b");
    TP_STREQ(r, "c");
    free(r);
    r = pkg_path_rel("/a/x/y", "/a/b/c");
    TP_STREQ(r, "../../x/y");
    free(r);
    r = pkg_path_rel("/a/b", "/a/b");
    TP_STREQ(r, ".");
    free(r);
    r = pkg_path_rel("/ab/c", "/a");
    TP_STREQ(r, "../ab/c");
    free(r);
    r = pkg_index_rel_path("axi");
    TP_STREQ(r, "ax/axi.toml");
    free(r);
    r = pkg_index_rel_path("X");
    TP_STREQ(r, "1/X.toml");
    free(r);
    r = pkg_index_rel_path("Uart-ip");
    TP_STREQ(r, "ua/Uart-ip.toml");
    free(r);
    r = pkg_name_ident("uart-ip");
    TP_STREQ(r, "uart_ip");
    free(r);
    TP_CHECK(cpkg_is_url("https://github.com/a/b"));
    TP_CHECK(cpkg_is_url("git@github.com:a/b.git"));
    TP_CHECK(cpkg_is_url("file:///tmp/x"));
    TP_CHECK(!cpkg_is_url("axi@1.2"));
    TP_CHECK(!cpkg_is_url("axi"));
    r = cpkg_url_basename("https://github.com/acme/tern-uart.git/");
    TP_STREQ(r, "tern-uart");
    free(r);
    r = cpkg_public_url("git@github.com:acme/x.git");
    TP_STREQ(r, "https://github.com/acme/x.git");
    free(r);
    r = cpkg_github_slug("https://github.com/tern-lang/registry");
    TP_STREQ(r, "tern-lang/registry");
    free(r);
    r = cpkg_github_slug("git@github.com:me/registry.git");
    TP_STREQ(r, "me/registry");
    free(r);
    TP_CHECK(cpkg_github_slug("https://gitlab.com/a/b") == NULL);
}

/* ------------------------------------------------------ dependency roots */

static void test_dep_roots(void)
{
    TernManifest m;
    PkgDepRoots roots;
    char *proj = tp_path("roots"), *mp = tp_path("roots/tern.toml"), *home = tp_path("roots-home");
    const char *commit = "1111111111111111111111111111111111111111";
    StrBuf sb = {0};
    tp_reset_diags();
    os_setenv("TERN_HOME", home);
    tp_write("roots/tern.toml", "[package]\nname = \"app\"\nversion = \"1.0.0\"\n[dependencies]\n"
                                "uart-ip = \"1\"\nlocal = { path = \"../roots-local\" }\n");
    tp_write("roots-local/tern.toml", "[package]\nname = \"local\"\nversion = \"0.1.0\"\n");
    TP_CHECK(pkg_manifest_load(mp, &m) == 0);
    /* no lock */
    TP_CHECK(pkg_dep_roots(&m, &roots) != 0);
    TP_HAS("there is no tern.lock");
    TP_HAS("run `tern add`");
    tp_reset_diags();
    sb_printf(&sb,
              "version = 1\n[[package]]\nname = \"local\"\nversion = \"0.1.0\"\nsource = \"path+../roots-local\"\n"
              "[[package]]\nname = \"uart-ip\"\nversion = \"1.4.0\"\nsource = \"registry+https://x/uart\"\n"
              "tag = \"v1.4.0\"\ncommit = \"%s\"\n",
              commit);
    tp_write("roots/tern.lock", sb_cstr(&sb));
    /* locked but not fetched */
    TP_CHECK(pkg_dep_roots(&m, &roots) != 0);
    TP_HAS("dependency `uart-ip` (1.4.0, commit 1111111111) is not fetched");
    tp_reset_diags();
    sb_reset(&sb);
    sb_printf(&sb, "roots-home/cache/uart-ip/%s/tern.toml", commit);
    tp_write(sb_cstr(&sb), "[package]\nname = \"uart-ip\"\nversion = \"1.4.0\"\n");
    TP_CHECK(pkg_dep_roots(&m, &roots) == 0);
    TP_CHECK(roots.len == 2);
    if (roots.len == 2) {
        TP_STREQ(roots.items[1].ident, "uart_ip");
        TP_CHECK(tern_ends_with(roots.items[1].src, "/cache/uart-ip/1111111111111111111111111111111111111111/src"));
        TP_CHECK(roots.items[1].direct && !roots.items[1].vendored);
        TP_CHECK(tern_ends_with(roots.items[0].root, "/roots-local"));
    }
    pkg_dep_roots_free(&roots);
    /* vendored copy wins when its marker matches */
    sb_reset(&sb);
    sb_printf(&sb, "%s\n", commit);
    tp_write("roots/vendor/uart-ip/.tern-vendor", sb_cstr(&sb));
    TP_CHECK(pkg_dep_roots(&m, &roots) == 0);
    TP_CHECK(roots.len == 2 && roots.items[1].vendored && tern_ends_with(roots.items[1].root, "roots/vendor/uart-ip"));
    pkg_dep_roots_free(&roots);
    /* stale lock: the requirement no longer matches */
    pkg_manifest_free(&m);
    tp_write("roots/tern.toml", "[package]\nname = \"app\"\nversion = \"1.0.0\"\n[dependencies]\n"
                                "uart-ip = \"2\"\nlocal = { path = \"../roots-local\" }\n");
    TP_CHECK(pkg_manifest_load(mp, &m) == 0);
    TP_CHECK(pkg_dep_roots(&m, &roots) != 0);
    TP_HAS("tern.lock is out of date for `uart-ip`");
    TP_HAS("run `tern update uart-ip`");
    pkg_manifest_free(&m);
    /* pkg_project_open: no project above a bare directory */
    {
        PkgProject prj;
        char *bare = tp_path("roots-local/src");
        os_mkdir_p(bare);
        TP_CHECK(pkg_project_open(bare, &prj) == 1); /* finds roots-local/tern.toml */
        TP_STREQ(prj.manifest.name, "local");
        TP_CHECK(prj.deps.len == 0);
        pkg_project_close(&prj);
        free(bare);
    }
    os_setenv("TERN_HOME", NULL);
    tp_reset_diags();
    sb_free(&sb);
    free(proj);
    free(mp);
    free(home);
}

/* ------------------------------------------------- end to end, with git */

static int tp_git(const char *cwd, const char *a0, const char *a1, const char *a2, const char *a3, const char *a4)
{
    const char *args[6];
    char *err = NULL;
    int rc;
    args[0] = a0;
    args[1] = a1;
    args[2] = a2;
    args[3] = a3;
    args[4] = a4;
    args[5] = NULL;
    rc = pkg_git(cwd, args, NULL, &err);
    if (rc != 0)
        fprintf(stderr, "git %s %s %s failed in %s: %s\n", a0, a1 ? a1 : "", a2 ? a2 : "", cwd ? cwd : ".",
                err ? err : "");
    free(err);
    return rc;
}

static char *tp_url(const char *rel)
{
    StrBuf sb = {0};
    char *p = tp_path(rel);
    sb_printf(&sb, "file://%s%s", p[0] == '/' ? "" : "/", p);
    free(p);
    return sb_take(&sb);
}

/* Creates <tmp>/work/<name> with a package at `version`, commits, tags and
 * pushes to <tmp>/remote/<name>.git (created on first use). */
static void tp_make_release(const char *name, const char *version, const char *deps)
{
    StrBuf rel = {0}, text = {0}, tag = {0};
    char *work, *bare, *bare_url;
    sb_printf(&rel, "work/%s", name);
    work = tp_path(sb_cstr(&rel));
    sb_reset(&rel);
    sb_printf(&rel, "remote/%s.git", name);
    bare = tp_path(sb_cstr(&rel));
    bare_url = tp_url(sb_cstr(&rel));
    if (!os_is_dir(bare)) {
        os_mkdir_p(bare);
        tp_git(bare, "init", "-q", "--bare", NULL, NULL);
        os_mkdir_p(work);
        tp_git(work, "init", "-q", NULL, NULL, NULL);
        tp_git(work, "remote", "add", "origin", bare_url, NULL);
    }
    sb_reset(&rel);
    sb_printf(&rel, "work/%s/tern.toml", name);
    sb_printf(&text, "[package]\nname = \"%s\"\nversion = \"%s\"\nlicense = \"MIT\"\n[dependencies]\n%s", name, version,
              deps);
    tp_write(sb_cstr(&rel), sb_cstr(&text));
    sb_reset(&rel);
    sb_reset(&text);
    sb_printf(&rel, "work/%s/src/%s.tern", name, name);
    sb_printf(&text, "package %s\n// version %s\n", name, version);
    tp_write(sb_cstr(&rel), sb_cstr(&text));
    sb_reset(&rel);
    sb_printf(&rel, "work/%s/tests/t.tern", name);
    tp_write(sb_cstr(&rel), "// tests\n");
    sb_reset(&rel);
    sb_printf(&rel, "work/%s/README.md", name);
    tp_write(sb_cstr(&rel), "readme\n");
    sb_reset(&rel);
    sb_printf(&rel, "work/%s/LICENSE", name);
    tp_write(sb_cstr(&rel), "MIT\n");
    sb_reset(&rel);
    sb_printf(&rel, "work/%s/docs/big.txt", name);
    tp_write(sb_cstr(&rel), "not vendored\n");
    sb_printf(&tag, "v%s", version);
    tp_git(work, "add", "-A", NULL, NULL, NULL);
    tp_git(work, "commit", "-q", "-m", sb_cstr(&tag), NULL);
    tp_git(work, "tag", sb_cstr(&tag), NULL, NULL, NULL);
    tp_git(work, "push", "-q", "origin", "HEAD:refs/heads/main", NULL);
    tp_git(work, "push", "-q", "origin", sb_cstr(&tag), NULL);
    sb_free(&rel);
    sb_free(&text);
    sb_free(&tag);
    free(work);
    free(bare);
    free(bare_url);
}

/* Writes the index entry for `name` with every tag of its bare repo and
 * pushes the index. */
static void tp_index_publish(const char *name)
{
    Arena a = {0};
    PkgAvailList tags;
    PkgIndexEntry e;
    StrBuf rel = {0}, text = {0};
    char *url, *irel, *iwork;
    sb_printf(&rel, "remote/%s.git", name);
    url = tp_url(sb_cstr(&rel));
    TP_CHECK(pkg_git_tags(url, &a, &tags) == 0);
    memset(&e, 0, sizeof e);
    e.name = name;
    e.git = url;
    e.description = "test package";
    e.versions = tags;
    pkg_index_format(&e, &text);
    irel = pkg_index_rel_path(name);
    sb_reset(&rel);
    sb_printf(&rel, "work/index/%s", irel);
    tp_write(sb_cstr(&rel), sb_cstr(&text));
    iwork = tp_path("work/index");
    tp_git(iwork, "add", "-A", NULL, NULL, NULL);
    tp_git(iwork, "commit", "-q", "-m", name, NULL);
    tp_git(iwork, "push", "-q", "origin", "HEAD:refs/heads/main", NULL);
    vec_free(tags.items);
    arena_release(&a);
    sb_free(&rel);
    sb_free(&text);
    free(url);
    free(irel);
    free(iwork);
}

static int tp_run(int (*fn)(int, char **), const char *a0, const char *a1, const char *a2, const char *a3)
{
    char *argv[5];
    int n = 0;
    argv[n++] = (char *)a0;
    if (a1)
        argv[n++] = (char *)a1;
    if (a2)
        argv[n++] = (char *)a2;
    if (a3)
        argv[n++] = (char *)a3;
    argv[n] = NULL;
    return fn(n, argv);
}

static char *tp_read(const char *rel)
{
    char *p = tp_path(rel), *data = NULL;
    size_t len;
    if (os_read_file(p, &data, &len) != 0)
        data = NULL;
    free(p);
    return data;
}

static int tp_contains(const char *rel, const char *needle)
{
    char *d = tp_read(rel);
    int ok = d && strstr(d, needle) != NULL;
    if (!ok)
        fprintf(stderr, "  %s does not contain \"%s\":\n%s\n", rel, needle, d ? d : "(missing)");
    free(d);
    return ok;
}

static int tp_exists(const char *rel)
{
    char *p = tp_path(rel);
    int ok = os_exists(p);
    free(p);
    return ok;
}

static void test_end_to_end(void)
{
    char *cwd = os_getcwd(), *home, *gitcfg, *index_url, *idxwork, *proj, *uart_url;
    StrBuf sb = {0};
    if (!pkg_git_available()) {
        printf("skip: end-to-end package tests (git not found)\n");
        free(cwd);
        return;
    }
    tp_reset_diags();
    /* an isolated git and tern environment */
    home = tp_path("home");
    gitcfg = tp_path("home/.gitconfig");
    tp_write("home/.gitconfig", "[user]\n\tname = Tern Test\n\temail = test@example.com\n"
                                "[init]\n\tdefaultBranch = main\n[commit]\n\tgpgSign = false\n"
                                "[tag]\n\tgpgSign = false\n[protocol \"file\"]\n\tallow = always\n");
    os_setenv("HOME", home);
    os_setenv("GIT_CONFIG_GLOBAL", gitcfg);
    os_setenv("GIT_CONFIG_NOSYSTEM", "1");
    os_setenv("GIT_TERMINAL_PROMPT", "0");
    {
        char *th = tp_path("tern-home");
        os_setenv("TERN_HOME", th);
        free(th);
    }
    /* packages and the index */
    tp_make_release("stream-utils", "1.0.0", "");
    tp_make_release("stream-utils", "1.1.0", "");
    tp_make_release("stream-utils", "2.0.0", "");
    tp_make_release("axi", "1.0.0", "stream-utils = \"1.0\"\n");
    tp_make_release("axi", "1.2.0", "stream-utils = \"1\"\n");
    tp_make_release("uart", "0.9.0", "");
    tp_make_release("uart", "1.0.0", "");
    idxwork = tp_path("work/index");
    {
        char *bare = tp_path("remote/index.git");
        os_mkdir_p(bare);
        tp_git(bare, "init", "-q", "--bare", NULL, NULL);
        free(bare);
    }
    index_url = tp_url("remote/index.git");
    os_mkdir_p(idxwork);
    tp_git(idxwork, "init", "-q", NULL, NULL, NULL);
    tp_git(idxwork, "remote", "add", "origin", index_url, NULL);
    tp_write("work/index/README.md", "test index\n");
    tp_index_publish("stream-utils");
    tp_index_publish("axi");
    os_setenv("TERN_INDEX", index_url);

    /* tern new */
    TP_CHECK(os_chdir(tp_tmp) == 0);
    TP_CHECK(tp_run(cmd_new, "new", "proj", NULL, NULL) == 0);
    TP_CHECK(tp_exists("proj/tern.toml") && tp_exists("proj/src/proj.tern") && tp_exists("proj/tests/proj_test.tern"));
    TP_CHECK(tp_exists("proj/.gitignore") && tp_exists("proj/README.md") && tp_exists("proj/.git"));
    TP_CHECK(tp_contains("proj/src/proj.tern", "pub module Proj<Period: uint = 50_000_000>"));
    TP_CHECK(tp_contains("proj/src/proj.tern", "count = count +% 1"));
    TP_CHECK(tp_contains("proj/tests/proj_test.tern", "use proj::Proj"));
    TP_CHECK(tp_contains("proj/tern.toml", "authors = [\"Tern Test <test@example.com>\"]"));
    TP_CHECK(tp_run(cmd_new, "new", "proj", NULL, NULL) != 0); /* exists */
    TP_HAS("destination `proj` already exists");
    tp_reset_diags();
    TP_CHECK(tp_run(cmd_new, "new", "9bad", NULL, NULL) == TERN_EXIT_USAGE);
    tp_reset_diags();
    proj = tp_path("proj");
    TP_CHECK(os_chdir(proj) == 0);
    {
        /* the generated manifest is valid */
        TernManifest m;
        TP_CHECK(pkg_manifest_load("tern.toml", &m) == 0);
        TP_CHECK(m.ntargets == 1 && pkg_manifest_target(&m, NULL) && strcmp(pkg_manifest_target(&m, NULL)->top, "Proj") == 0);
        pkg_manifest_free(&m);
    }

    /* tern add <registry name>: newest release, transitive deps */
    TP_CHECK(tp_run(cmd_add, "add", "axi", NULL, NULL) == 0);
    TP_CHECK(tp_contains("proj/tern.toml", "tag = \"v2.0.1\" }\naxi = \"1.2.0\"\n\n[features]"));
    TP_CHECK(tp_contains("proj/tern.toml", "axi = \"1.2.0\"\n"));
    TP_CHECK(tp_contains("proj/tern.toml", "[features]")); /* rest preserved */
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"axi\"\nversion = \"1.2.0\""));
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"stream-utils\"\nversion = \"1.1.0\""));
    TP_CHECK(tp_contains("proj/tern.lock", "dependencies = [\"stream-utils\"]"));
    TP_CHECK(tp_exists("tern-home/index/ax/axi.toml"));
    {
        PkgProject prj;
        TP_CHECK(pkg_project_open(".", &prj) == 1);
        TP_CHECK(prj.deps.len == 2);
        if (prj.deps.len == 2) {
            char *f = os_path_join(prj.deps.items[0].src, "axi.tern");
            TP_STREQ(prj.deps.items[0].name, "axi");
            TP_CHECK(os_is_file(f));
            TP_STREQ(prj.deps.items[1].ident, "stream_utils");
            TP_CHECK(!prj.deps.items[1].direct);
            free(f);
        }
        pkg_project_close(&prj);
    }

    /* tern add name@req (replaces the line) */
    TP_CHECK(tp_run(cmd_add, "add", "axi@=1.0.0", NULL, NULL) == 0);
    TP_CHECK(tp_contains("proj/tern.toml", "axi = \"=1.0.0\"\n"));
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"axi\"\nversion = \"1.0.0\""));
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"stream-utils\"\nversion = \"1.1.0\"")); /* kept, still ^1.0 */

    /* a conflicting requirement leaves both files untouched */
    tp_reset_diags();
    TP_CHECK(tp_run(cmd_add, "add", "stream-utils@2", NULL, NULL) != 0);
    TP_HAS("cannot resolve `stream-utils`");
    TP_CHECK(tp_contains("proj/tern.toml", "axi = \"=1.0.0\"\n"));
    {
        char *t = tp_read("proj/tern.toml");
        TP_CHECK(t && !strstr(t, "stream-utils"));
        free(t);
    }
    tp_reset_diags();
    TP_CHECK(tp_run(cmd_add, "add", "nosuchpkg", NULL, NULL) != 0);
    TP_HAS("package `nosuchpkg` is not in the registry index");
    tp_reset_diags();

    /* tern add <git url> (newest tag, name from its tern.toml) */
    uart_url = tp_url("remote/uart.git");
    TP_CHECK(tp_run(cmd_add, "add", uart_url, NULL, NULL) == 0);
    sb_printf(&sb, "uart = { git = \"%s\", tag = \"v1.0.0\" }", uart_url);
    TP_CHECK(tp_contains("proj/tern.toml", sb_cstr(&sb)));
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"uart\"\nversion = \"1.0.0\""));
    /* ... and with an explicit tag */
    TP_CHECK(tp_run(cmd_add, "add", uart_url, "--tag", "v0.9.0") == 0);
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"uart\"\nversion = \"0.9.0\""));
    TP_CHECK(tp_contains("proj/tern.lock", "tag = \"v0.9.0\""));

    /* tern add --path */
    tp_write("libs/mylib/tern.toml", "[package]\nname = \"mylib\"\nversion = \"0.2.0\"\n");
    TP_CHECK(tp_run(cmd_add, "add", "--path", "../libs/mylib", NULL) == 0);
    TP_CHECK(tp_contains("proj/tern.toml", "mylib = { path = \"../libs/mylib\" }"));
    TP_CHECK(tp_contains("proj/tern.lock", "source = \"path+../libs/mylib\""));

    /* a new release + tern update */
    tp_make_release("axi", "1.3.0", "stream-utils = \"1\"\n");
    tp_index_publish("axi");
    TP_CHECK(tp_run(cmd_add, "add", "axi@1", NULL, NULL) == 0);   /* fresh index: picks 1.3.0 */
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"axi\"\nversion = \"1.3.0\""));
    tp_make_release("stream-utils", "1.5.0", "");
    tp_index_publish("stream-utils");
    TP_CHECK(tp_run(cmd_update, "update", "axi", NULL, NULL) == 0);  /* only axi may move */
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"stream-utils\"\nversion = \"1.1.0\""));
    TP_CHECK(tp_run(cmd_update, "update", NULL, NULL, NULL) == 0);
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"stream-utils\"\nversion = \"1.5.0\""));
    TP_CHECK(tp_contains("proj/tern.lock", "name = \"uart\"\nversion = \"0.9.0\"")); /* pinned tag */
    tp_reset_diags();
    TP_CHECK(tp_run(cmd_update, "update", "bogus", NULL, NULL) != 0);
    TP_HAS("`bogus` is not a dependency of proj");
    tp_reset_diags();

    /* tern vendor */
    TP_CHECK(tp_run(cmd_vendor, "vendor", NULL, NULL, NULL) == 0);
    TP_CHECK(tp_exists("proj/vendor/axi/tern.toml") && tp_exists("proj/vendor/axi/src/axi.tern"));
    TP_CHECK(tp_exists("proj/vendor/axi/tests/t.tern") && tp_exists("proj/vendor/axi/LICENSE"));
    TP_CHECK(tp_exists("proj/vendor/axi/README.md") && tp_exists("proj/vendor/axi/.tern-vendor"));
    TP_CHECK(!tp_exists("proj/vendor/axi/docs") && !tp_exists("proj/vendor/axi/.git"));
    TP_CHECK(tp_exists("proj/vendor/uart/tern.toml") && tp_exists("proj/vendor/stream-utils/tern.toml"));
    TP_CHECK(!tp_exists("proj/vendor/mylib")); /* path deps stay where they are */
    TP_CHECK(tp_contains("proj/tern.lock", "vendored = true"));
    {
        /* builds now read vendor/, even with the cache gone */
        PkgProject prj;
        size_t i, nv = 0;
        char *cache = tp_path("tern-home/cache");
        os_remove_all(cache);
        free(cache);
        TP_CHECK(pkg_project_open(".", &prj) == 1);
        for (i = 0; i < prj.deps.len; i++)
            nv += (size_t)prj.deps.items[i].vendored;
        TP_CHECK(prj.deps.len == 4 && nv == 3);
        pkg_project_close(&prj);
    }
    /* `tern add` (no args) on a fresh machine fetches the locked versions */
    TP_CHECK(tp_run(cmd_vendor, "vendor", "--remove", NULL, NULL) == 0);
    TP_CHECK(!tp_exists("proj/vendor/axi"));
    {
        PkgProject prj;
        TP_CHECK(pkg_project_open(".", &prj) == -1);
        TP_HAS("is not fetched");
        tp_reset_diags();
    }
    TP_CHECK(tp_run(cmd_add, "add", NULL, NULL, NULL) == 0);
    {
        PkgProject prj;
        TP_CHECK(pkg_project_open(".", &prj) == 1);
        TP_CHECK(prj.deps.len == 4);
        pkg_project_close(&prj);
    }

    /* tern publish: dry run, then for real against a fork of the index */
    {
        char *uwork = tp_path("work/uart"), *fork = tp_path("remote/index-fork.git"), *fork_url;
        char *o = NULL;
        const char *show[] = {"ls-remote", "--heads", NULL, NULL};
        const char *tags[] = {"ls-remote", "--tags", NULL, NULL};
        TP_CHECK(os_chdir(uwork) == 0);
        tp_write("work/uart/tern.toml", "[package]\nname = \"uart\"\nversion = \"1.1.0\"\ndescription = \"UART\"\n"
                                         "license = \"MIT\"\n");
        TP_CHECK(tp_run(cmd_publish, "publish", "--dry-run", NULL, NULL) != 0); /* dirty tree */
        TP_HAS("the working tree has uncommitted changes");
        tp_reset_diags();
        tp_git(uwork, "commit", "-q", "-am", "1.1.0", NULL);
        TP_CHECK(tp_run(cmd_publish, "publish", "--dry-run", NULL, NULL) == 0);
        os_mkdir_p(fork);
        tp_git(fork, "init", "-q", "--bare", NULL, NULL);
        fork_url = tp_url("remote/index-fork.git");
        tp_git(idxwork, "push", "-q", fork_url, "HEAD:refs/heads/main", NULL);
        TP_CHECK(tp_run(cmd_publish, "publish", "--fork", fork_url, NULL) == 0);
        show[2] = fork_url;
        TP_CHECK(pkg_git(NULL, show, &o, NULL) == 0 && o && strstr(o, "refs/heads/publish/uart-1.1.0"));
        free(o);
        o = NULL;
        tags[2] = uart_url;
        TP_CHECK(pkg_git(NULL, tags, &o, NULL) == 0 && o && strstr(o, "refs/tags/v1.1.0"));
        free(o);
        TP_CHECK(tp_contains("tern-home/index/ua/uart.toml", "version = \"1.1.0\"\ntag = \"v1.1.0\""));
        TP_CHECK(tp_contains("tern-home/index/ua/uart.toml", "description = \"UART\""));
        /* the same version again is refused */
        TP_CHECK(tp_run(cmd_publish, "publish", NULL, NULL, NULL) != 0);
        TP_HAS("version 1.1.0 is already tagged (v1.1.0)");
        tp_reset_diags();
        free(uwork);
        free(fork);
        free(fork_url);
    }

    os_chdir(cwd);
    os_setenv("TERN_INDEX", NULL);
    os_setenv("TERN_HOME", NULL);
    sb_free(&sb);
    free(cwd);
    free(home);
    free(gitcfg);
    free(index_url);
    free(idxwork);
    free(proj);
    free(uart_url);
    tp_reset_diags();
}

int main(void)
{
    char *t = os_temp_dir();
    StrBuf sb = {0};
    sb_printf(&sb, "%s/tern-test-pkg-%llu", t ? t : ".", (unsigned long long)os_time_ns());
    free(t);
    tp_tmp = sb_take(&sb);
    os_mkdir_p(tp_tmp);
    {
        /* resolve symlinks such as /tmp -> /private/tmp so path checks are stable */
        char *cwd = os_getcwd(), *real;
        os_chdir(tp_tmp);
        real = os_getcwd();
        os_chdir(cwd);
        free(cwd);
        if (real) {
            free(tp_tmp);
            tp_tmp = real;
        }
    }
    diag_set_sink(tp_sink, NULL);
    pkg_set_quiet(1);
    test_toml_ok();
    test_toml_errors();
    test_toml_writer();
    test_manifest();
    test_manifest_errors();
    test_manifest_edit();
    test_semver();
    test_requirements();
    test_resolve();
    test_lock_roundtrip();
    test_paths();
    test_dep_roots();
    test_end_to_end();
    diag_flush();
    if (!tp_fails)
        os_remove_all(tp_tmp);
    else
        fprintf(stderr, "test files kept in %s\n", tp_tmp);
    free(tp_tmp);
    sb_free(&tp_diags);
    src_free_all();
    intern_free_all();
    if (tp_fails) {
        printf("pkg tests: %d of %d checks FAILED\n", tp_fails, tp_checks);
        return 1;
    }
    printf("pkg tests: all %d checks passed\n", tp_checks);
    return 0;
}
