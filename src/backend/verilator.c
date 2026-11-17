/* verilator.c — `tern build --lint`: Verilator lint over the emitted files.
 *
 * Style-only warnings that generated code legitimately triggers are waived
 * (the same set as tests/lint_emitted.sh); concurrent assertions are
 * hidden with TERN_NO_SVA. */
#include "../base/common.h"
#include "../base/str.h"
#include "../os/os.h"
#include "backend.h"

int vl_lint(const char *dir, const SvOutput *out, const char *top, SvStd std)
{
    static const char *const vl_flags[] = {
        "--lint-only", "-Wall", "-Wno-DECLFILENAME", "-Wno-UNUSED", "-Wno-WIDTH", "-Wno-PINCONNECTEMPTY",
        "-Wno-CASEINCOMPLETE", "-Wno-MULTITOP", "-DTERN_NO_SVA"};
    char *exe = os_find_program("verilator");
    BkPathVec files = {0};
    VEC(char *) argv = {0};
    OsSpawnOpts so;
    OsRunResult res;
    size_t i;
    int r;
    if (!exe) {
        fflush(stdout);
        fprintf(stderr, "tern build: --lint needs Verilator, which is not on PATH\n"
                        "  install it (apt install verilator / brew install verilator) and run again\n");
        return -1;
    }
    bk_hdl_files(out, &files);
    vec_push(argv, exe);
    for (i = 0; i < TERN_ARRAY_LEN(vl_flags); i++)
        vec_push(argv, (char *)vl_flags[i]);
    vec_push(argv, (char *)"--default-language");
    vec_push(argv, (char *)(std == SV_STD_V2005 ? "1364-2005" : "1800-2012"));
    if (top) {
        vec_push(argv, (char *)"--top-module");
        vec_push(argv, (char *)top);
    }
    for (i = 0; i < files.len; i++)
        vec_push(argv, (char *)files.data[i]);
    vec_push(argv, NULL);
    memset(&so, 0, sizeof so);
    memset(&res, 0, sizeof res);
    so.cwd = dir;
    so.stdin_null = 1;
    so.capture_stdout = 1;
    so.merge_stderr = 1;
    r = os_run(argv.data, &so, &res);
    if (res.out && res.out_len)
        fwrite(res.out, 1, res.out_len, stdout);
    if (r != 0)
        printf("tern build: verilator lint failed (exit %d)\n", r);
    else
        printf("tern build: verilator lint clean (%u file%s)\n", (unsigned)files.len, files.len == 1 ? "" : "s");
    os_run_result_free(&res);
    vec_free(argv);
    vec_free(files);
    free(exe);
    return r == 0 ? 0 : 1;
}
