/* fuzz_check.c — libFuzzer harness for `tern check` on one file: parse,
 * resolve (against the embedded std), elaborate every root and run the IR
 * checks, rendering every diagnostic.
 *
 *     make fuzz                    # also builds build/fuzz_check
 *     ./build/fuzz_check -max_total_time=120 -rss_limit_mb=1024 -timeout=10 corpus-dir seeds...
 *
 * The const-evaluator step limit is lowered (FUZZ_CHECK_STEPS) so that
 * loops and recursion in the input end quickly; run it with a small
 * -rss_limit_mb / -malloc_limit_mb. Built with -DFUZZ_STANDALONE it replays
 * the files and directories named on the command line (tests/fuzz/fuzz_main.h,
 * `make test` replays tests/fuzz/corpus/check).
 *
 * Single translation unit, like tests/unit/test_lsp.c.
 */
#define FUZZ_NAME "fuzz_check"
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
#include "../../src/ir/ir_build.c"
#include "../../src/ir/ir_json.c"
#include "../../src/sema/resolve.c"
#include "../../src/sema/types.c"
#include "../../src/sema/info.c"
#include "../../src/sema/syninfo.c"
#include "../../src/elab/value.c"
#include "../../src/elab/consteval.c"
#include "../../src/elab/mono.c"
#include "../../src/elab/regfile.c"
#include "../../src/elab/elab.c"
#include "../../src/check/check.c"
#include "../../src/check/drivers.c"
#include "../../src/check/assign.c"
#include "../../src/check/cdc.c"
#include "../../src/check/unused.c"
#include "../../src/check/poweron.c"
#include "../../src/pkg/toml.c"
#include "../../src/pkg/manifest.c"
#include "../../src/pkg/lock.c"
#include "../../src/pkg/git.c"
#include "../../src/std_embed.c"

/* Parsing and resolving all of std costs ~20 ms per input, which leaves
 * libFuzzer a few dozen runs a second; the harness loads only the prelude
 * and the packages that the small designs in the seeds use. driver.c sees
 * the filtered table through these macros. */
static const char *const fuzz_std_keep[] = {
    "std/prelude/", "std/fifo/", "std/cdc/", "std/clk/", "std/mem/", "std/stream/stream.tern",
    "std/target/", "std/math/math.tern", "std/timer/",
};
static StdEmbedFile fuzz_std_files[256];
static size_t fuzz_std_index[256];
static size_t fuzz_std_count;

static void fuzz_std_init(void)
{
    size_t i, k;
    for (i = 0; i < tern_std_file_count && fuzz_std_count < TERN_ARRAY_LEN(fuzz_std_files); i++)
        for (k = 0; k < TERN_ARRAY_LEN(fuzz_std_keep); k++)
            if (tern_starts_with(tern_std_files[i].path, fuzz_std_keep[k])) {
                fuzz_std_index[fuzz_std_count] = i;
                fuzz_std_files[fuzz_std_count++] = tern_std_files[i];
                break;
            }
}

static const char *fuzz_std_text(size_t i)
{
    return tern_std_text(fuzz_std_index[i]);
}

#define tern_std_files fuzz_std_files
#define tern_std_file_count fuzz_std_count
#define tern_std_text fuzz_std_text
#include "../../src/driver.c"
#undef tern_std_files
#undef tern_std_file_count
#undef tern_std_text

#define FUZZ_CHECK_STEPS 200000u
#define FUZZ_CHECK_PATH "fuzz_input.tern"
/* a directory with no tern.toml at or above it: single-file mode */
#define FUZZ_ROOT_DIR "/tern-fuzz-no-project"
#ifdef _WIN32
#define FUZZ_NULL_DEVICE "NUL"
#else
#define FUZZ_NULL_DEVICE "/dev/null"
#endif

static const char *fuzz_check_text;
static size_t fuzz_check_len;
static FILE *fuzz_check_null;

static const char *fuzz_check_overlay(void *user, const char *path, size_t *len)
{
    TERN_UNUSED(user);
    if (strcmp(path, FUZZ_CHECK_PATH) != 0)
        return NULL;
    *len = fuzz_check_len;
    return fuzz_check_text;
}

static void fuzz_check_sink(const Diagnostic *d, void *user)
{
    StrBuf sb = {0};
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_HUMAN, 0, &sb);
    sb_free(&sb);
}

static int fuzz_check_is_user(uint32_t file, void *user)
{
    Sema *S = driver_sema((Driver *)user);
    size_t i;
    for (i = 0; i < S->files.len; i++)
        if (S->files.data[i]->ast && S->files.data[i]->ast->src == file)
            return S->files.data[i]->is_user;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *const paths[] = {FUZZ_CHECK_PATH};
    DriverOptions o;
    Driver *dr;
    Arena ira = {0};
    if (size > 64u * 1024u)
        return 0;
    if (!fuzz_check_null) {
        /* the CDC report and the like go to stdout */
        fuzz_check_null = fopen(FUZZ_NULL_DEVICE, "w");
        fuzz_std_init();
        diag_set_sink(fuzz_check_sink, NULL);
    }
    fuzz_check_text = (const char *)data;
    fuzz_check_len = size;
    memset(&o, 0, sizeof o);
    o.paths = paths;
    o.npaths = 1;
    o.cmd = "check";
    o.root = FUZZ_ROOT_DIR;
    o.const_eval_limit = FUZZ_CHECK_STEPS;
    o.overlay = fuzz_check_overlay;
    dr = driver_open(&o);
    if (dr) {
        if (!driver_parse_errors(dr)) {
            CheckOptions co;
            IrDesign *d = driver_elaborate_all(dr, NULL, &ira);
            memset(&co, 0, sizeof co);
            co.cdc_report = 1;
            co.unused = 1;
            co.report_out = fuzz_check_null;
            co.is_user_file = fuzz_check_is_user;
            co.user = dr;
            if (diag_error_count() == 0)
                check_design(d, &co);
        }
        driver_free(dr);
    }
    arena_release(&ira);
    diag_flush();
    diag_reset();
    return 0;
}

#include "fuzz_main.h"
