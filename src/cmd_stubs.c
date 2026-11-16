/* cmd_stubs.c — placeholders for commands that are not implemented yet.
 *
 * To implement a command: delete its one-line stub below and add the real
 * cmd_<name> in its own file. Delete this file (and its entries in
 * src/unity.c, the Makefile and build.bat) once the last stub is gone. */
#include "base/common.h"
#include "base/args.h"
#include "cmds.h"

static int stub_not_implemented(const char *name, int argc, char **argv)
{
    if (args_wants_help(argc, argv)) {
        cmd_print_usage(name);
        return TERN_EXIT_OK;
    }
    fflush(stdout);
    fprintf(stderr, "tern %s: not implemented yet\n", name);
    return TERN_EXIT_USAGE;
}

int cmd_build(int argc, char **argv) { return stub_not_implemented("build", argc, argv); }
int cmd_test(int argc, char **argv) { return stub_not_implemented("test", argc, argv); }
int cmd_sim(int argc, char **argv) { return stub_not_implemented("sim", argc, argv); }
int cmd_prove(int argc, char **argv) { return stub_not_implemented("prove", argc, argv); }
int cmd_synth(int argc, char **argv) { return stub_not_implemented("synth", argc, argv); }
