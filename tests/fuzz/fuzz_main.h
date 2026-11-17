/* fuzz_main.h — the replay driver shared by the fuzz harnesses.
 *
 * Every harness (tests/fuzz/fuzz_*.c) defines LLVMFuzzerTestOneInput and
 * includes this file last. Built with libFuzzer (-fsanitize=fuzzer) this
 * file adds nothing; built with -DFUZZ_STANDALONE it adds a main() that runs
 * each file named on the command line, and every file below each directory
 * named there, through the harness once:
 *
 *     cc -DFUZZ_STANDALONE -g -fsanitize=address,undefined tests/fuzz/fuzz_parser.c -o fz
 *     ./fz crash-1234                      # reproduce a saved input
 *     ./fz tests/fuzz/corpus/parser        # replay a regression corpus (`make test`)
 *
 * The harness must already include src/os/os_posix.c (first), os_win32.c
 * and os_common.c, plus common.c.
 */
#ifdef FUZZ_STANDALONE

static int fuzz_quiet;

static int fuzz_run_path(const char *path, unsigned *count)
{
    char *buf = NULL;
    size_t n = 0;
    if (os_is_dir(path)) {
        OsDirList list;
        size_t i;
        int rc = 0;
        if (os_list_dir(path, &list) != 0) {
            fprintf(stderr, "fuzz: cannot list %s\n", path);
            return 1;
        }
        for (i = 0; i < list.len && rc == 0; i++) {
            char *child;
            if (list.items[i].name[0] == '.')
                continue;
            child = os_path_join(path, list.items[i].name);
            rc = fuzz_run_path(child, count);
            free(child);
        }
        os_dir_list_free(&list);
        return rc;
    }
    if (os_read_file(path, &buf, &n) != 0) {
        fprintf(stderr, "fuzz: cannot read %s\n", path);
        return 1;
    }
    if (!fuzz_quiet)
        fprintf(stderr, "fuzz: %s\n", path);
    LLVMFuzzerTestOneInput((const uint8_t *)buf, n);
    free(buf);
    (*count)++;
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    unsigned count = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            fuzz_quiet = 1;
            continue;
        }
        if (fuzz_run_path(argv[i], &count) != 0)
            return 2;
    }
    printf("%s: %u inputs ok\n", FUZZ_NAME, count);
    return 0;
}

#endif /* FUZZ_STANDALONE */
