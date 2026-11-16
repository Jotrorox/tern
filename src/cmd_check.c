/* cmd_check.c — `tern check` (interim).
 *
 * Until the full front end (resolve, types, elaboration, checks) exists,
 * `tern check` lexes and parses its inputs and reports syntax errors, so the
 * golden `.err` fixtures for syntax errors already run. The real checker
 * replaces this file. */
#include "base/common.h"
#include "cmds.h"

int cmd_check(int argc, char **argv)
{
    return cmd_parse_files(argc, argv, "check", 0);
}
