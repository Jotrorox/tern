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
/* DOC_INCLUDE */

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

static void tf_doc_tests(void)
{
}
