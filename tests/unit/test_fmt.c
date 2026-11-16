/* test_fmt.c — tests for `tern fmt` and `tern doc` (src/tools/fmt.c, doc.c).
 *
 *     cc -o build/test_fmt tests/unit/test_fmt.c && build/test_fmt   (from the repository root)
 *
 * Formatter: every .tern file under std/ and tests/fixtures/ without
 * syntax errors is formatted; the result must parse to the same AST
 * (ignoring positions) with the same comments (fmt_verify), and formatting
 * it again must not change it (idempotence). Naming warnings are checked on
 * a small source.
 *
 * Doc generator: text renderings of a few std items must equal the goldens
 * in tests/fixtures/doc/<name>.txt (`UPDATE=1` rewrites them), and the HTML
 * site for all of std is generated into a temporary directory and checked
 * (every package page exists, tags are balanced). */
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
#include "../../src/syntax/lexer.c"
#include "../../src/syntax/parser.c"
#include "../../src/syntax/ast_dump.c"
#include "../../src/std_embed.c"
#include "../../src/tools/fmt.c"
#include "../../src/tools/doc.c"

int cmd_print_usage(const char *name)
{
    TERN_UNUSED(name);
    return 0;
}

static int tf_checks, tf_fails, tf_update;

#define TF_CHECK(c) tf_check((c) ? 1 : 0, #c, __FILE__, __LINE__)

static void tf_check(int ok, const char *expr, const char *file, int line)
{
    tf_checks++;
    if (!ok) {
        tf_fails++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

typedef VEC(char *) TfPaths;

static void tf_collect(const char *dir, TfPaths *out)
{
    OsDirList list;
    size_t i;
    if (os_list_dir(dir, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        char *p = os_path_join(dir, list.items[i].name);
        if (list.items[i].name[0] == '.') {
            free(p);
            continue;
        }
        if (list.items[i].is_dir)
            tf_collect(p, out);
        else if (strcmp(os_path_ext(p), ".tern") == 0) {
            vec_push(*out, p);
            continue;
        }
        free(p);
    }
    os_dir_list_free(&list);
}

/* ------------------------------------------------------------- formatter */

static int tf_nformatted, tf_nskipped;

static void tf_fmt_file(const char *path)
{
    char *text = NULL;
    size_t len = 0;
    Arena a = {0}, b = {0};
    AstFile *f, *g;
    StrBuf out1 = {0}, out2 = {0}, why = {0};
    uint32_t id;
    if (os_read_file(path, &text, &len) != 0) {
        fprintf(stderr, "cannot read %s\n", path);
        tf_fails++;
        return;
    }
    diag_set_sink(fmt_null_sink, NULL);
    id = src_add(path, text, len);
    f = parse_file(&a, id);
    diag_flush();
    diag_set_sink(NULL, NULL);
    if (f->nerrors) {
        tf_nskipped++;
    } else {
        tf_nformatted++;
        fmt_format(f, &out1);
        if (!fmt_verify(f, out1.data, out1.len, &why)) {
            fprintf(stderr, "%s: formatting is not faithful: %s\n", path, why.data);
            tf_fails++;
        } else {
            uint32_t id2 = src_add(path, out1.data, out1.len);
            g = parse_file(&b, id2);
            fmt_format(g, &out2);
            if (out1.len != out2.len || memcmp(out1.data, out2.data, out1.len) != 0) {
                fprintf(stderr, "%s: formatting is not idempotent\n", path);
                tf_fails++;
            }
        }
        tf_checks++;
    }
    sb_free(&out1);
    sb_free(&out2);
    sb_free(&why);
    arena_release(&a);
    arena_release(&b);
    free(text);
}

static void tf_fmt_corpus(void)
{
    TfPaths paths = {0};
    size_t i;
    tf_collect("std", &paths);
    tf_collect("tests/fixtures", &paths);
    TF_CHECK(paths.len > 50);
    for (i = 0; i < paths.len; i++) {
        tf_fmt_file(paths.data[i]);
        free(paths.data[i]);
    }
    vec_free(paths);
    printf("test_fmt: %d files formatted (faithful, idempotent), %d skipped (syntax errors)\n", tf_nformatted,
           tf_nskipped);
}

/* fmt of a string */
static char *tf_fmt_string(const char *src)
{
    Arena a = {0};
    StrBuf out = {0};
    uint32_t id = src_add("<test>", src, strlen(src));
    AstFile *f = parse_file(&a, id);
    TF_CHECK(f->nerrors == 0);
    fmt_format(f, &out);
    TF_CHECK(fmt_verify(f, out.data, out.len, NULL));
    arena_release(&a);
    sb_cstr(&out);
    return sb_take(&out);
}

static void tf_fmt_cases(void)
{
    static const struct {
        const char *in, *want;
    } cases[] = {
        {"module M{in a:u8\nout y:u8\ny=a&&b||c}", "module M {\n    in a: u8\n    out y: u8\n    y = a and b or c\n}\n"},
        {"const X = 0x00_ff\n", "const X = 0x00_ff\n"},
        {"fn f() -> u8 {\n\n\n    1\n}\n", "fn f() -> u8 {\n    1\n}\n"},
        {"module M {\n    a = 1\n\n\n\n    b = 2\n}\n", "module M {\n    a = 1\n\n    b = 2\n}\n"},
        {"module M {\n    let x = & &y\n}\n", "module M {\n    let x = & &y\n}\n"},
        {"module M { x = 1 }\n", "module M { x = 1 }\n"},
        {"", ""},
    };
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(cases); i++) {
        char *got = tf_fmt_string(cases[i].in);
        if (strcmp(got, cases[i].want) != 0) {
            fprintf(stderr, "fmt case %u:\n--- want\n%s--- got\n%s---\n", (unsigned)i, cases[i].want, got);
            tf_fails++;
        }
        tf_checks++;
        free(got);
    }
}

static StrBuf tf_diags;

static void tf_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_SHORT, 0, &tf_diags);
    if (d->nsubs && d->subs[0].level == DIAG_HELP) {
        sb_puts(&tf_diags, d->subs[0].msg);
        sb_putc(&tf_diags, 10);
    }
}

static void tf_naming(void)
{
    static const char src[] = "module foo_bar {\n"
                              "    in ClkIn: clock\n"
                              "    reg MyReg: u8 = 0\n"
                              "}\n"
                              "fn DoThing() {}\n"
                              "const bad_const = 1\n"
                              "const GoodConst = 1\n"
                              "const GOOD_CONST = 1\n"
                              "enum Mode { idle, Busy }\n"
                              "struct HTTPRequest { RawData: u8 }\n"
                              "pub type u8 = uint<8>\n"
                              "extern module SB_IO(\"\") {}\n";
    Arena a = {0};
    uint32_t id = src_add("naming.tern", src, sizeof src - 1);
    AstFile *f = parse_file(&a, id);
    int n;
    TF_CHECK(f->nerrors == 0);
    diag_set_sink(tf_sink, NULL);
    n = fmt_check_naming(f);
    diag_flush();
    diag_set_sink(NULL, NULL);
    TF_CHECK(n == 7);
    TF_CHECK(strstr(tf_diags.data, "module `foo_bar` should be UpperCamelCase") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `FooBar`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `clk_in`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `my_reg`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `do_thing`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `BadConst`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `Idle`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "rename it to `raw_data`") != NULL);
    TF_CHECK(strstr(tf_diags.data, "HTTPRequest") == NULL);
    TF_CHECK(strstr(tf_diags.data, "SB_IO") == NULL);
    sb_free(&tf_diags);
    arena_release(&a);
}

/* ------------------------------------------------------------------- doc */

static void tf_doc_tests(void);

int main(void)
{
    char *up = os_getenv("UPDATE");
    tf_update = up && up[0] && strcmp(up, "0") != 0;
    free(up);
    tf_fmt_cases();
    tf_naming();
    tf_fmt_corpus();
    tf_doc_tests();
    if (tf_fails) {
        fprintf(stderr, "test_fmt: %d of %d checks failed\n", tf_fails, tf_checks);
        return 1;
    }
    printf("test_fmt: %d checks passed\n", tf_checks);
    return 0;
}

/* Compares `got` with the golden file (rewritten with UPDATE=1). */
static void tf_golden(const char *path, const char *got)
{
    char *want = NULL;
    size_t len = 0;
    if (os_read_file(path, &want, &len) != 0)
        want = NULL;
    if (!want || strcmp(want, got) != 0) {
        if (tf_update) {
            os_write_file(path, got, strlen(got));
            printf("test_fmt: updated %s\n", path);
        } else {
            fprintf(stderr, "%s: output differs (UPDATE=1 rewrites it); got:\n%s\n", path, got);
            tf_fails++;
        }
    }
    tf_checks++;
    free(want);
}

static const char *memmem_simple(const char *h, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    for (i = 0; i + k <= n; i++) {
        if (memcmp(h + i, needle, k) == 0)
            return h + i;
    }
    return NULL;
}

static const char *const tf_void_tags[] = {"meta", "link", "input", "br", "hr", "img"};

/* Every element is closed in order; no external resources. */
static int tf_html_balanced(const char *html, StrBuf *why)
{
    const char *stack[256];
    size_t depth = 0, i;
    const char *p = html;
    while ((p = strchr(p, '<')) != NULL) {
        const char *q = p + 1;
        int closing = 0;
        char name[32];
        size_t n = 0;
        if (strncmp(p, "<!--", 4) == 0) {
            const char *e = strstr(p, "-->");
            p = e ? e + 3 : p + 4;
            continue;
        }
        if (q[0] == '!') {
            p = q;
            continue;
        }
        if (q[0] == '/') {
            closing = 1;
            q++;
        }
        while (tern_is_alnum((unsigned char)*q) && n + 1 < sizeof name)
            name[n++] = *q++;
        name[n] = '\0';
        if (n == 0) {
            sb_printf(why, "stray `<` at offset %u", (unsigned)(p - html));
            return 0;
        }
        if (!closing && (strcmp(name, "script") == 0 || strcmp(name, "link") == 0)) {
            const char *e = strchr(q, '>');
            if (e && (memmem_simple(q, (size_t)(e - q), "http") != NULL)) {
                sb_printf(why, "external resource in <%s>", name);
                return 0;
            }
        }
        for (i = 0; i < TERN_ARRAY_LEN(tf_void_tags); i++) {
            if (strcmp(name, tf_void_tags[i]) == 0)
                break;
        }
        if (i == TERN_ARRAY_LEN(tf_void_tags)) {
            if (closing) {
                if (depth == 0 || strcmp(stack[depth - 1], name) != 0) {
                    sb_printf(why, "</%s> does not close <%s>", name, depth ? stack[depth - 1] : "(nothing)");
                    return 0;
                }
                depth--;
            } else if (depth < TERN_ARRAY_LEN(stack)) {
                stack[depth++] = intern_cstr(name);
            }
        }
        p = strchr(q, '>');
        if (!p) {
            sb_puts(why, "unterminated tag");
            return 0;
        }
    }
    if (depth) {
        sb_printf(why, "<%s> is not closed", stack[depth - 1]);
        return 0;
    }
    return 1;
}

static void tf_doc_tests(void)
{
    static const struct {
        const char *what, *golden;
    } goldens[] = {
        {"std::fifo::Fifo", "tests/fixtures/doc/std_fifo_Fifo.txt"},
        {"std::prelude::Stream", "tests/fixtures/doc/std_prelude_Stream.txt"},
        {"std::math::Round", "tests/fixtures/doc/std_math_Round.txt"},
        {"std::fifo", "tests/fixtures/doc/std_fifo.txt"},
    };
    DocSet *ds = doc_set_new();
    size_t i;
    StrBuf out = {0}, why = {0};
    char *tmp, *dir;
    int npages = 0;
    doc_set_add_std(ds);
    TF_CHECK(ds->nerrors == 0);
    for (i = 0; i < TERN_ARRAY_LEN(goldens); i++) {
        sb_reset(&out);
        TF_CHECK(doc_render_text(ds, goldens[i].what, 80, &out) == 0);
        tf_golden(goldens[i].golden, sb_cstr(&out));
    }
    sb_reset(&out);
    TF_CHECK(doc_render_text(ds, "std::fifo::Fifx", 80, &out) == -1);
    TF_CHECK(strstr(sb_cstr(&out), "did you mean `std::fifo::Fifo`?") != NULL);
    sb_reset(&out);
    TF_CHECK(doc_render_text(ds, "std", 80, &out) == 0);
    TF_CHECK(strstr(sb_cstr(&out), "std::stream") != NULL);
    /* every package and item renders (no crash, sanitizers) */
    {
        size_t j, nitems = 0;
        for (i = 0; i < ds->pkgs.len; i++) {
            sb_reset(&out);
            TF_CHECK(doc_render_text(ds, ds->pkgs.data[i]->name, 60, &out) == 0);
            for (j = 0; j < ds->pkgs.data[i]->items.len; j++) {
                sb_reset(&out);
                TF_CHECK(doc_render_text(ds, ds->pkgs.data[i]->items.data[j]->full, 80, &out) == 0);
                nitems++;
            }
        }
        TF_CHECK(nitems > 200);
        printf("test_fmt: doc: %u std items rendered as text\n", (unsigned)nitems);
    }

    /* the HTML site for all of std */
    tmp = os_temp_dir();
    sb_reset(&out);
    sb_printf(&out, "%s/tern-test-doc-%llu", tmp ? tmp : ".", (unsigned long long)os_time_ns());
    dir = sb_take(&out);
    free(tmp);
    TF_CHECK(doc_write_html(ds, dir, &why) == 0);
    for (i = 0; i < ds->pkgs.len; i++) {
        char *path = os_path_join(dir, ds->pkgs.data[i]->page);
        char *html = NULL;
        size_t len = 0;
        if (os_read_file(path, &html, &len) != 0) {
            fprintf(stderr, "doc: page %s was not written\n", path);
            tf_fails++;
        } else {
            sb_reset(&why);
            npages++;
            if (!tf_html_balanced(html, &why)) {
                fprintf(stderr, "doc: %s: %s\n", path, why.data);
                tf_fails++;
            }
            TF_CHECK(strstr(html, "<title>") != NULL);
        }
        tf_checks++;
        free(html);
        free(path);
    }
    {
        static const char *const extra[] = {"index.html", "style.css", "search.js", "search-index.js"};
        for (i = 0; i < TERN_ARRAY_LEN(extra); i++) {
            char *path = os_path_join(dir, extra[i]);
            char *text = NULL;
            size_t len = 0;
            TF_CHECK(os_read_file(path, &text, &len) == 0 && len > 100);
            if (text && i == 0) {
                sb_reset(&why);
                if (!tf_html_balanced(text, &why)) {
                    fprintf(stderr, "doc: index.html: %s\n", why.data);
                    tf_fails++;
                }
            }
            if (text && i == 3)
                TF_CHECK(strstr(text, "\"std::fifo::Fifo\",\"module\",\"std.fifo.html#module.Fifo\"") != NULL);
            if (text && i == 1)
                TF_CHECK(strstr(text, "prefers-color-scheme:dark") != NULL);
            free(text);
            free(path);
        }
    }
    TF_CHECK(npages >= 15);
    printf("test_fmt: doc: %d std package pages written and checked\n", npages);
    os_remove_all(dir);
    free(dir);
    sb_free(&out);
    sb_free(&why);
    doc_set_free(ds);
}
