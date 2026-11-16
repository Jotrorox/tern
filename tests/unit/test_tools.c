/* test_tools.c — tests for `tern import` (golden files) and `tern remap`.
 *
 *     cc -o build/test_tools tests/unit/test_tools.c && build/test_tools   (from the repository root)
 *
 * Import goldens: every tests/fixtures/import/<stem>.sv / .v is imported with
 * the extern path "<file>" and package "<stem>"; the result must equal
 * <stem>.expected.tern and the warnings (short format) <stem>.expected.log.
 * `UPDATE=1` rewrites the expectations. (They are not named <stem>.tern /
 * <stem>.err so the golden runner, tests/run.c, does not treat them as
 * compiler fixtures.) */
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
#include "../../src/tools/import.c"
#include "../../src/tools/remap.c"

int cmd_print_usage(const char *name)
{
    TERN_UNUSED(name);
    return 0;
}

static int tt_checks, tt_fails, tt_update;
static StrBuf tt_diags;

#define TT_CHECK(c) tt_check((c) ? 1 : 0, #c, __FILE__, __LINE__)

static void tt_check(int ok, const char *expr, const char *file, int line)
{
    tt_checks++;
    if (!ok) {
        tt_fails++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

static void tt_sink(const Diagnostic *d, void *user)
{
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_SHORT, 0, &tt_diags);
}

/* trailing whitespace per line and trailing blank lines do not matter */
static void tt_normalize(const char *s, StrBuf *out)
{
    size_t i = 0, n = strlen(s);
    while (i < n) {
        size_t e = i, t;
        while (e < n && s[e] != '\n')
            e++;
        t = e;
        while (t > i && (s[t - 1] == ' ' || s[t - 1] == '\t' || s[t - 1] == '\r'))
            t--;
        sb_append(out, s + i, t - i);
        sb_putc(out, '\n');
        i = e + 1;
    }
    while (out->len >= 2 && out->data[out->len - 1] == '\n' && out->data[out->len - 2] == '\n')
        sb_truncate(out, out->len - 1);
}

static void tt_compare(const char *path, const char *got)
{
    char *want = NULL;
    size_t len;
    StrBuf a = {0}, b = {0};
    if (os_read_file(path, &want, &len) != 0)
        want = NULL;
    tt_normalize(got, &a);
    if (want)
        tt_normalize(want, &b);
    tt_checks++;
    if (!want || strcmp(sb_cstr(&a), sb_cstr(&b)) != 0) {
        if (tt_update) {
            os_write_file(path, a.data, a.len);
            printf("updated %s\n", path);
        } else {
            tt_fails++;
            fprintf(stderr, "golden mismatch: %s\n--- expected\n%s--- got\n%s---\n", path, want ? sb_cstr(&b) : "(missing)\n",
                    sb_cstr(&a));
        }
    }
    free(want);
    sb_free(&a);
    sb_free(&b);
}

static void test_import_goldens(void)
{
    const char *dir = "tests/fixtures/import";
    OsDirList dl;
    size_t i;
    int n = 0;
    if (os_list_dir(dir, &dl) != 0) {
        fprintf(stderr, "cannot list %s (run from the repository root)\n", dir);
        tt_fails++;
        return;
    }
    for (i = 0; i < dl.len; i++) {
        const char *nm = dl.items[i].name, *ext = os_path_ext(nm);
        char *path, *data, *pkg;
        size_t len;
        uint32_t id;
        StrBuf out = {0}, exp = {0};
        if (dl.items[i].is_dir || (strcmp(ext, ".sv") != 0 && strcmp(ext, ".v") != 0))
            continue;
        path = os_path_join(dir, nm);
        if (os_read_file(path, &data, &len) != 0) {
            free(path);
            continue;
        }
        diag_flush();
        sb_reset(&tt_diags);
        id = src_add(nm, data, len);
        free(data);
        pkg = imp_sanitize_pkg(nm, (size_t)(ext - nm));
        TT_CHECK(imp_source(id, nm, pkg, &out) >= 0);
        diag_flush();
        sb_printf(&exp, "%s/%.*s.expected.tern", dir, (int)(ext - nm), nm);
        tt_compare(exp.data, sb_cstr(&out));
        sb_reset(&exp);
        sb_printf(&exp, "%s/%.*s.expected.log", dir, (int)(ext - nm), nm);
        tt_compare(exp.data, sb_cstr(&tt_diags));
        sb_free(&out);
        sb_free(&exp);
        free(pkg);
        free(path);
        n++;
    }
    os_dir_list_free(&dl);
    TT_CHECK(n >= 3);
    sb_reset(&tt_diags);
}

/* Small inline cases: exact output of one module. */
static void tt_import_case(const char *sv, const char *expect_body)
{
    uint32_t id = src_add("case.sv", sv, strlen(sv));
    StrBuf out = {0};
    const char *body;
    TT_CHECK(imp_source(id, "case.sv", "p", &out) >= 0);
    body = strstr(sb_cstr(&out), "extern module");
    tt_checks++;
    if (!body || strcmp(body, expect_body) != 0) {
        tt_fails++;
        fprintf(stderr, "import case mismatch for:\n%s\n--- expected\n%s--- got\n%s---\n", sv, expect_body,
                body ? body : sb_cstr(&out));
    }
    sb_free(&out);
}

static void test_import_cases(void)
{
    diag_flush();
    sb_reset(&tt_diags);
    tt_import_case("module m(input a, output [W*2-1:0] y); endmodule",
                   "extern module m(\"case.sv\") {\n    in a: bool\n    out y: uint<W * 2>\n}\n");
    tt_import_case("module m #(parameter N = 4, localparam M = N + 1)(input [M:0] x, input [N-1:0][3:0] arr); endmodule",
                   "extern module m(\"case.sv\") <N: uint = 4> {\n    in x: uint<(N + 1) + 1>\n    in arr: [uint<4>; N]\n}\n");
    tt_import_case("module m(input logic [15:8] hi, input logic [1:4] asc, output logic [A:B] odd); endmodule",
                   "extern module m(\"case.sv\") {\n    in hi: uint<8>\n    in asc: uint<4> // ascending range in SystemVerilog\n"
                   "    out odd: uint<(A) - (B) + 1>\n}\n");
    tt_import_case("module m(input wire rst_b, input nreset, input i_clk_i, input clk_sel, input byte b); endmodule",
                   "extern module m(\"case.sv\") {\n"
                   "    in rst_b: reset<sync, low> // TODO: confirm: reset (active-low from the name; synchronous assumed)\n"
                   "    in nreset: reset<sync, low> // TODO: confirm: reset (active-low from the name; synchronous assumed)\n"
                   "    in i_clk_i: clock // TODO: confirm: clock (from the name)\n"
                   "    in clk_sel: bool\n"
                   "    in b: sint<8>\n}\n");
    tt_import_case("module m #(parameter W = 8'd16, parameter X = 'h1F, parameter Y = A > B ? A : B) (input [$clog2(W)-1:0] a); endmodule",
                   "extern module m(\"case.sv\") <W: uint = 16, X: uint = 0x1F, Y: uint = A > B ? A : B> {\n"
                   "    in a: uint<clog2(W)>\n}\n");
    tt_import_case("module m(a, b); input a; output b; reg [3:0] b; endmodule",
                   "extern module m(\"case.sv\") {\n    in a: bool\n    out b: uint<4>\n}\n");
    tt_import_case("module type(input logic match, output logic \\weird.name ); endmodule",
                   "extern module type_(\"case.sv\") {\n    in match_: bool #[sv_name(\"match\")]\n"
                   "    out weird_name: bool #[sv_name(\"\\\\weird.name\")]\n}\n");
    /* errors are reported with locations */
    diag_flush();
    sb_reset(&tt_diags);
    {
        StrBuf out = {0};
        uint32_t id = src_add("bad.sv", "module m(a, b); input a; endmodule\nmodule n(input x) endmodule\n", 61);
        TT_CHECK(imp_source(id, "bad.sv", "p", &out) < 0);
        diag_flush();
        TT_CHECK(strstr(sb_cstr(&tt_diags), "bad.sv:1:13: error: port `b` has no input/output/inout declaration") != NULL);
        TT_CHECK(strstr(sb_cstr(&tt_diags), "bad.sv:2:19: error: expected `;` after the header of module `n`") != NULL);
        if (!strstr(sb_cstr(&tt_diags), "port `b`"))
            fprintf(stderr, "%s", sb_cstr(&tt_diags));
        sb_free(&out);
    }
    sb_reset(&tt_diags);
    diag_reset();
}

static char *tt_tmp;

static void tt_write(const char *rel, const char *text)
{
    char *p = os_path_join(tt_tmp, rel), *d = os_path_dirname(p);
    os_mkdir_p(d);
    os_write_file(p, text, strlen(text));
    free(p);
    free(d);
}

static int tt_file_has(const char *rel, const char *needle)
{
    char *p = os_path_join(tt_tmp, rel), *data = NULL;
    size_t len;
    int ok = os_read_file(p, &data, &len) == 0 && strstr(data, needle) != NULL;
    if (!ok)
        fprintf(stderr, "  %s lacks \"%s\":\n%s\n", rel, needle, data ? data : "(missing)");
    free(p);
    free(data);
    return ok;
}

static void test_import_dir(void)
{
    char *out = os_path_join(tt_tmp, "imported");
    char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)"import";
    argv[argc++] = (char *)"tests/fixtures/import/rtl";
    argv[argc++] = (char *)"-o";
    argv[argc++] = out;
    argv[argc++] = (char *)"--root";
    argv[argc++] = (char *)"tests/fixtures/import";
    argv[argc] = NULL;
    fflush(stdout);
    TT_CHECK(cmd_import(argc, argv) == TERN_EXIT_OK);
    TT_CHECK(tt_file_has("imported/core.tern", "package imported\n"));
    TT_CHECK(tt_file_has("imported/core.tern", "extern module core(\"rtl/core.sv\")"));
    TT_CHECK(tt_file_has("imported/periph/gpio.tern", "package imported::periph\n"));
    TT_CHECK(tt_file_has("imported/periph/gpio.tern", "extern module gpio(\"rtl/periph/gpio.v\")"));
    /* one file to a named output with a package */
    argc = 0;
    argv[argc++] = (char *)"import";
    argv[argc++] = (char *)"tests/fixtures/import/uart_tx.sv";
    argv[argc++] = (char *)"-o";
    {
        char *one = os_path_join(tt_tmp, "src/uart.tern");
        argv[argc++] = one;
        argv[argc++] = (char *)"--package";
        argv[argc++] = (char *)"legacy::uart";
        argv[argc] = NULL;
        TT_CHECK(cmd_import(argc, argv) == TERN_EXIT_OK);
        TT_CHECK(tt_file_has("src/uart.tern", "package legacy::uart\n"));
        free(one);
    }
    diag_flush();
    sb_reset(&tt_diags);
    diag_reset();
    free(out);
}

static void test_remap(void)
{
    RemapMap m;
    StrBuf out = {0};
    char *build = os_path_join(tt_tmp, "build");
    const char *log =
        "%Warning-WIDTH: build/default/Top.sv:12:7: Operator ASSIGN expects 8 bits\n"
        "ERROR: [Synth 8-439] module 'x' not found [/abs/path/build/default/Top.sv:4]\n"
        "** Error: Top.sv(13): (vlog-2730) Undefined variable\n"
        "Error-[IND] Identifier not declared \"build/default/Top.sv\", 20\n"
        "Warning: File: /x/Top.sv Line: 21 something\n"
        "Fifo.sv, line 3: before any marker stays\n"
        "Fifo.sv, line 9: mapped\n"
        "Other.sv:5: unknown file stays\n"
        "plain text Top.svh and Top.sv without line stay\n";
    tt_write("build/default/Top.sv",
             "// Generated by tern\n"                    /* 1 */
             "// tern: src/top.tern:3\n"                 /* 2 */
             "module Top (\n"                            /* 3 */
             "  input logic clk\n"                       /* 4 */
             ");\n"                                      /* 5 */
             "  // tern: src/top.tern:10:5\n"            /* 6 */
             "  logic [7:0] a;\n"                        /* 7 */
             "  logic b;\n"                              /* 8 */
             "  // tern: src/top.tern:12\n"              /* 9 */
             "  always_comb begin\n"                     /* 10 */
             "  end\n"                                   /* 11 */
             "  assign a = b;\n"                         /* 12 */
             "  // tern: C:/proj/src/top.tern:40\n"      /* 13 */
             "endmodule\n");
    tt_write("build/default/Fifo.sv", "module Fifo;\n\n\n\n// tern: src/fifo.tern:7\n\n\n\n\nendmodule\n");
    tt_write("build/notes.txt", "// tern: ignored.tern:1\n");
    memset(&m, 0, sizeof m);
    remap_scan(&m, build, build);
    TT_CHECK(m.files.len == 2);
    TT_CHECK(m.nmarks == 5);
    remap_text(&m, log, strlen(log), &out);
    {
        const char *want =
            "%Warning-WIDTH: src/top.tern:12 (build/default/Top.sv:12:7): Operator ASSIGN expects 8 bits\n"
            "ERROR: [Synth 8-439] module 'x' not found [src/top.tern:3 (/abs/path/build/default/Top.sv:4)]\n"
            "** Error: C:/proj/src/top.tern:40 (Top.sv(13)): (vlog-2730) Undefined variable\n"
            "Error-[IND] Identifier not declared C:/proj/src/top.tern:40 (\"build/default/Top.sv\", 20)\n"
            "Warning: File: C:/proj/src/top.tern:40 (/x/Top.sv Line: 21) something\n"
            "Fifo.sv, line 3: before any marker stays\n"
            "src/fifo.tern:7 (Fifo.sv, line 9): mapped\n"
            "Other.sv:5: unknown file stays\n"
            "plain text Top.svh and Top.sv without line stay\n";
        tt_checks++;
        if (strcmp(sb_cstr(&out), want) != 0) {
            tt_fails++;
            fprintf(stderr, "remap mismatch\n--- expected\n%s--- got\n%s---\n", want, sb_cstr(&out));
        }
    }
    /* the lookup itself */
    TT_CHECK(m.files.len == 2 && remap_lookup(&m.files.data[1], 7) && strcmp(remap_lookup(&m.files.data[1], 7), "src/top.tern:10") == 0);
    TT_CHECK(m.files.len == 2 && remap_lookup(&m.files.data[1], 1) == NULL);
    /* the command, from a log file to an output file */
    {
        char *logp = os_path_join(tt_tmp, "tool.log"), *outp = os_path_join(tt_tmp, "tool.remapped");
        char *argv[6];
        tt_write("tool.log", "Top.sv:7: x\n");
        argv[0] = (char *)"remap";
        argv[1] = build;
        argv[2] = logp;
        argv[3] = (char *)"-o";
        argv[4] = outp;
        argv[5] = NULL;
        TT_CHECK(cmd_remap(5, argv) == TERN_EXIT_OK);
        TT_CHECK(tt_file_has("tool.remapped", "src/top.tern:10 (Top.sv:7): x\n"));
        free(logp);
        free(outp);
    }
    remap_free(&m);
    sb_free(&out);
    free(build);
}

int main(void)
{
    char *t = os_temp_dir(), *upd = os_getenv("UPDATE");
    StrBuf sb = {0};
    tt_update = upd && upd[0] && strcmp(upd, "0") != 0;
    free(upd);
    sb_printf(&sb, "%s/tern-test-tools-%llu", t ? t : ".", (unsigned long long)os_time_ns());
    free(t);
    tt_tmp = sb_take(&sb);
    os_mkdir_p(tt_tmp);
    diag_set_sink(tt_sink, NULL);
    test_import_goldens();
    test_import_cases();
    test_import_dir();
    test_remap();
    diag_flush();
    if (!tt_fails)
        os_remove_all(tt_tmp);
    else
        fprintf(stderr, "test files kept in %s\n", tt_tmp);
    free(tt_tmp);
    sb_free(&tt_diags);
    src_free_all();
    intern_free_all();
    if (tt_fails) {
        printf("tools tests: %d of %d checks FAILED\n", tt_fails, tt_checks);
        return 1;
    }
    printf("tools tests: all %d checks passed\n", tt_checks);
    return 0;
}
