/* fuzz_parser.c — libFuzzer harness for the lexer, parser and AST dump.
 *
 *     make fuzz                                  # builds build/fuzz_parser with $(FUZZ_CC)
 *     make fuzz FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang   # macOS: Apple clang has no libFuzzer
 *     ./build/fuzz_parser -max_total_time=120 build/fuzz-corpus tests/fixtures/parse std
 *
 * Built with -DFUZZ_STANDALONE (no libFuzzer) it runs the inputs named on
 * the command line once each, which reproduces a crash from a saved input:
 *     cc -DFUZZ_STANDALONE -g -fsanitize=address,undefined tests/fuzz/fuzz_parser.c -o fuzz1
 *     ./fuzz1 crash-1234
 *
 * Single translation unit: it includes the sources it needs, like tests/run.c.
 */
#include "../../src/base/common.c"
#include "../../src/base/arena.c"
#include "../../src/base/str.c"
#include "../../src/base/map.c"
#include "../../src/base/utf8.c"
#include "../../src/base/json.c"
#include "../../src/base/diag.c"
#include "../../src/syntax/lexer.c"
#include "../../src/syntax/parser.c"
#include "../../src/syntax/ast_dump.c"

static uint32_t fuzz_src;

static void fuzz_sink(const Diagnostic *d, void *user)
{
    /* render anyway: the excerpt code must cope with every span we produce */
    StrBuf sb = {0};
    TERN_UNUSED(user);
    diag_render(d, DIAG_FMT_HUMAN, 0, &sb);
    sb_free(&sb);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Arena a = {0};
    AstFile *f;
    StrBuf sb = {0};
    if (size > 1u << 20)
        return 0;
    if (!fuzz_src) {
        diag_set_sink(fuzz_sink, NULL);
        fuzz_src = src_add("fuzz.tern", (const char *)data, size);
    } else {
        src_update(fuzz_src, (const char *)data, size);
    }
    f = parse_file(&a, fuzz_src);
    {
        /* positions computed incrementally by the lexer must match diag.h */
        uint32_t i, line, col;
        for (i = 0; i < f->ntokens; i++) {
            src_linecol(fuzz_src, f->tokens[i].off, &line, &col);
            if (line != f->tokens[i].line || col != f->tokens[i].col) {
                fprintf(stderr, "token %u at offset %u: lexer says %u:%u, src_linecol %u:%u\n", (unsigned)i,
                        (unsigned)f->tokens[i].off, (unsigned)f->tokens[i].line, (unsigned)f->tokens[i].col,
                        (unsigned)line, (unsigned)col);
                abort();
            }
        }
    }
    ast_dump(f, &sb);
    diag_flush();
    diag_reset();
    sb_free(&sb);
    arena_release(&a);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        char *buf;
        long n;
        if (!fp) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        buf = (char *)xmalloc((size_t)(n > 0 ? n : 1));
        if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
            fclose(fp);
            free(buf);
            return 2;
        }
        fclose(fp);
        LLVMFuzzerTestOneInput((const uint8_t *)buf, (size_t)(n > 0 ? n : 0));
        free(buf);
        printf("ok %s\n", argv[i]);
    }
    return 0;
}
#endif
