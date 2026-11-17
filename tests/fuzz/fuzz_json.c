/* fuzz_json.c — libFuzzer harness for the JSON reader and writer
 * (src/base/json.c; the LSP, `tern sim` remote models and the IR JSON
 * tests read JSON with it).
 *
 *     make fuzz && ./build/fuzz_json -max_total_time=60 corpus-dir
 *
 * Every document that parses must serialize to JSON that parses again;
 * serializing that is a fixed point (compact and pretty). Built with
 * -DFUZZ_STANDALONE it replays files and directories (tests/fuzz/fuzz_main.h;
 * `make test` replays tests/fuzz/corpus/json).
 */
#define FUZZ_NAME "fuzz_json"
#include "../../src/os/os_posix.c"
#include "../../src/os/os_win32.c"
#include "../../src/os/os_common.c"
#include "../../src/base/common.c"
#include "../../src/base/arena.c"
#include "../../src/base/str.c"
#include "../../src/base/map.c"
#include "../../src/base/utf8.c"
#include "../../src/base/json.c"

/* serializes v, parses the text back and serializes that into out */
static void fuzz_json_reparse(Arena *a, const StrBuf *text, StrBuf *out, int pretty)
{
    JsonError err;
    JsonValue *v = json_parse(a, text->data ? text->data : "", text->len, &err);
    if (!v) {
        fprintf(stderr, "fuzz_json: own output does not parse (%s at %u:%u):\n%s\n", err.msg, (unsigned)err.line,
                (unsigned)err.col, text->data ? text->data : "");
        abort();
    }
    json_to_sb(out, v, pretty);
}

/* Invalid UTF-8 in a string is written as \ufffd, which reads back as a
 * valid U+FFFD and is then written as is: the second serialization must
 * be a fixed point. */
static void fuzz_json_roundtrip(const JsonValue *v, int pretty)
{
    Arena a = {0};
    StrBuf s1 = {0}, s2 = {0}, s3 = {0};
    json_to_sb(&s1, v, pretty);
    fuzz_json_reparse(&a, &s1, &s2, pretty);
    fuzz_json_reparse(&a, &s2, &s3, pretty);
    if (s2.len != s3.len || (s2.len && memcmp(s2.data, s3.data, s2.len) != 0)) {
        fprintf(stderr, "fuzz_json: round trip changed the text:\n%s\n---\n%s\n", sb_cstr(&s2), sb_cstr(&s3));
        abort();
    }
    sb_free(&s1);
    sb_free(&s2);
    sb_free(&s3);
    arena_release(&a);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Arena a = {0};
    JsonError err;
    JsonValue *v = json_parse(&a, (const char *)data, size, &err);
    if (v) {
        fuzz_json_roundtrip(v, 0);
        fuzz_json_roundtrip(v, 1);
        (void)json_as_str(json_get(v, "method"), "");
        (void)json_as_int(json_at(v, 0), 0);
    } else if (err.offset > size) {
        fprintf(stderr, "fuzz_json: error offset %lu past the end (%lu)\n", (unsigned long)err.offset,
                (unsigned long)size);
        abort();
    }
    arena_release(&a);
    return 0;
}

#include "fuzz_main.h"
