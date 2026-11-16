/* unity.c — the whole compiler as one translation unit:
 *
 *     cc -std=c11 -O2 -o tern src/unity.c
 *     cc -std=c99 -O2 -o tern src/unity.c
 *     cl /std:c11 /W4 /O2 src\unity.c /Fe:tern.exe
 *
 * Every .c file under src/ is listed here exactly once. Keep this list, the
 * Makefile (SRCS) and build.bat in sync when adding files. Because all
 * files share one translation unit, file-local (static) names and macros
 * must be unique across the tree: prefix them with the module name.
 */

/* Must stay first: it defines the POSIX feature-test macros, which have to
 * precede every system header. */
#include "os/os_posix.c"
#include "os/os_win32.c"
#include "os/os_common.c"

#include "base/common.c"
#include "base/arena.c"
#include "base/str.c"
#include "base/map.c"
#include "base/bignum.c"
#include "base/json.c"
#include "base/utf8.c"
#include "base/diag.c"
#include "base/args.c"

#include "syntax/lexer.c"
#include "syntax/parser.c"
#include "syntax/ast_dump.c"
#include "ir/ir_build.c"
#include "ir/ir_json.c"

#include "sema/resolve.c"
#include "sema/types.c"
#include "sema/info.c"
#include "elab/value.c"
#include "elab/consteval.c"
#include "elab/mono.c"
#include "elab/elab.c"
#include "check/check.c"

#include "sim/bitvec.c"
#include "sim/netlist.c"
#include "sim/eval.c"
#include "sim/vcd.c"
#include "emit/sv.c"
#include "emit/constraints.c"
#include "emit/regmap.c"
#include "emit/cmd_emit.c"
#include "pkg/toml.c"
#include "pkg/manifest.c"
#include "pkg/lock.c"
#include "pkg/git.c"
#include "pkg/cmd_pkg.c"
#include "lsp/rpc.c"
#include "lsp/lsp.c"
#include "lsp/lsp_nav.c"

#include "tools/import.c"
#include "tools/remap.c"
#include "tools/fmt.c"
#include "tools/doc.c"

#include "std_embed.c"
#include "driver.c"
#include "cmd_parse.c"
#include "cmd_check.c"
#include "cmd_stubs.c"
#include "main.c"
