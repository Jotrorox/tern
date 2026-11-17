/* fuzz_toml.c — libFuzzer harness for the TOML reader and the tern.toml
 * validation (src/pkg/toml.c, src/pkg/manifest.c), plus the textual
 * [dependencies] editor that `tern add` uses.
 *
 *     make fuzz && ./build/fuzz_toml -max_total_time=60 corpus-dir
 *
 * Built with -DFUZZ_STANDALONE it replays files and directories
 * (tests/fuzz/fuzz_main.h; `make test` replays tests/fuzz/corpus/toml).
 */
#define FUZZ_NAME "fuzz_toml"
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
#include "../../src/pkg/toml.c"
#include "../../src/pkg/manifest.c"
#include "../../src/pkg/lock.c"
#include "../../src/pkg/git.c"

static uint32_t fuzz_toml_src;

static void fuzz_toml_sink(const Diagnostic *d, void *user)
{
    StrBuf sb = {0};
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_HUMAN, 0, &sb);
    sb_free(&sb);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    TernManifest m;
    StrBuf out = {0};
    char *err = NULL;
    if (size > 1u << 20)
        return 0;
    if (!fuzz_toml_src) {
        diag_set_sink(fuzz_toml_sink, NULL);
        fuzz_toml_src = src_add("tern.toml", (const char *)data, size);
    } else {
        src_update(fuzz_toml_src, (const char *)data, size);
    }
    if (pkg_manifest_parse(fuzz_toml_src, "/tern-fuzz-project", &m) == 0) {
        (void)pkg_manifest_target(&m, NULL);
        (void)pkg_manifest_dep(&m, "x");
        (void)pkg_features_apply(&m, "a,-b");
    }
    pkg_manifest_free(&m);
    if (pkg_manifest_edit_dep((const char *)data, size, "fuzz-dep", "\"1.2\"", &out, &err) != 0)
        free(err);
    sb_free(&out);
    diag_flush();
    diag_reset();
    return 0;
}

#include "fuzz_main.h"
