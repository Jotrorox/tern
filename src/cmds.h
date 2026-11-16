/* cmds.h — every `tern <command>` entry point.
 *
 * A command is `int cmd_<name>(int argc, char **argv)` where argv[0] is the
 * command name ("build") and argv[1..argc-1] are its arguments, with the
 * global options (--color, --diag-format, --strict) already removed and
 * applied by main.c. Parse the rest with ArgParser (src/base/args.h).
 *
 * Exit codes: TERN_EXIT_OK on success, TERN_EXIT_FAIL when the input has
 * errors / tests fail, TERN_EXIT_USAGE for bad command lines and
 * unimplemented features. main.c calls diag_flush() after the command.
 *
 * Unimplemented commands live in cmd_stubs.c; to implement one, delete its
 * stub there and add the real function in its own file (keep src/unity.c,
 * the Makefile and build.bat in sync).
 */
#ifndef TERN_CMDS_H
#define TERN_CMDS_H

#define TERN_EXIT_OK 0
#define TERN_EXIT_FAIL 1
#define TERN_EXIT_USAGE 2

int cmd_new(int argc, char **argv);
int cmd_build(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_test(int argc, char **argv);
int cmd_sim(int argc, char **argv);
int cmd_prove(int argc, char **argv);
int cmd_synth(int argc, char **argv);
int cmd_fmt(int argc, char **argv);
int cmd_lsp(int argc, char **argv);
int cmd_doc(int argc, char **argv);
int cmd_add(int argc, char **argv);
int cmd_update(int argc, char **argv);
int cmd_vendor(int argc, char **argv);
int cmd_publish(int argc, char **argv);
int cmd_import(int argc, char **argv);
int cmd_remap(int argc, char **argv);

/* Internal (not listed in `tern help`): `tern parse --dump <file>` prints the
 * AST dump used by the golden tests (tests/fixtures/.../<name>.parse). */
int cmd_parse(int argc, char **argv);

/* Shared by `tern parse` and the interim `tern check` (cmd_parse.c): parses
 * the files/directories in argv[1..], reports syntax errors and, with
 * `--dump` (when allow_dump), prints each AST. Returns an exit status. */
int cmd_parse_files(int argc, char **argv, const char *cmd, int allow_dump);

/* Print the usage block of a command (from main.c's table) to stdout.
 * Returns 0 if the command exists. */
int cmd_print_usage(const char *name);

#endif /* TERN_CMDS_H */
