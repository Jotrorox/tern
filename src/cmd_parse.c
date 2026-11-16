/* cmd_parse.c — `tern parse [--dump] <file.tern | dir>...` (internal).
 *
 * Lexes and parses every file (directories are searched recursively for
 * `*.tern`, in sorted order), reports syntax errors, and with --dump prints
 * the AST dump used by the golden tests (tests/fixtures/.../<name>.parse).
 * Exit status 1 when any file has a syntax error. */
#include "base/common.h"
#include "base/args.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/vec.h"
#include "base/diag.h"
#include "os/os.h"
#include "syntax/parser.h"
#include "cmds.h"

typedef VEC(char *) CmdParsePaths;

static void cmd_parse_collect(const char *path, CmdParsePaths *out, int top)
{
    OsDirList list;
    size_t i;
    if (!os_is_dir(path)) {
        if (top || strcmp(os_path_ext(path), ".tern") == 0)
            vec_push(*out, xstrdup(path));
        return;
    }
    if (os_list_dir(path, &list) != 0)
        return;
    for (i = 0; i < list.len; i++) {
        char *child;
        if (list.items[i].name[0] == '.')
            continue;
        child = os_path_join(path, list.items[i].name);
        if (list.items[i].is_dir || strcmp(os_path_ext(child), ".tern") == 0)
            cmd_parse_collect(child, out, 0);
        free(child);
    }
    os_dir_list_free(&list);
}

static void cmd_parse_free_paths(CmdParsePaths *paths)
{
    size_t i;
    for (i = 0; i < paths->len; i++)
        free(paths->data[i]);
    vec_free(*paths);
}

int cmd_parse_files(int argc, char **argv, const char *cmd, int allow_dump)
{
    ArgParser ap;
    CmdParsePaths paths = {0};
    int dump = 0, verbose = 0, failed = 0, nfiles = 0;
    size_t i;
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (allow_dump && args_flag(&ap, "dump"))
            dump = 1;
        else if (args_flag(&ap, "verbose|v"))
            verbose = 1;
        else if (args_flag(&ap, "help|h")) {
            cmd_print_usage(cmd);
            cmd_parse_free_paths(&paths);
            return TERN_EXIT_OK;
        } else if (args_positional(&ap))
            cmd_parse_collect(ap.arg, &paths, 1);
        else
            args_unknown(&ap);
    }
    if (ap.errors || paths.len == 0) {
        if (!ap.errors)
            fprintf(stderr, "tern %s: error: no input files\n", cmd);
        cmd_parse_free_paths(&paths);
        return TERN_EXIT_USAGE;
    }
    for (i = 0; i < paths.len; i++) {
        char *text = NULL;
        size_t len = 0;
        uint32_t id;
        Arena arena = {0};
        AstFile *f;
        if (os_read_file(paths.data[i], &text, &len) != 0) {
            fprintf(stderr, "tern %s: error: cannot read %s\n", cmd, paths.data[i]);
            failed = 1;
            continue;
        }
        id = src_add(paths.data[i], text, len);
        free(text);
        f = parse_file(&arena, id);
        nfiles++;
        if (f->nerrors)
            failed = 1;
        if (dump) {
            StrBuf sb = {0};
            ast_dump(f, &sb);
            fwrite(sb.data, 1, sb.len, stdout);
            sb_free(&sb);
        }
        arena_release(&arena);
    }
    if (verbose)
        fprintf(stderr, "tern %s: %d file%s, %d error%s\n", cmd, nfiles, nfiles == 1 ? "" : "s",
                diag_error_count(), diag_error_count() == 1 ? "" : "s");
    cmd_parse_free_paths(&paths);
    return failed ? TERN_EXIT_FAIL : TERN_EXIT_OK;
}

int cmd_parse(int argc, char **argv)
{
    return cmd_parse_files(argc, argv, "parse", 1);
}
