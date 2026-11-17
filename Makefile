# Makefile — POSIX make build of the Tern compiler.
#
#   make                  build ./tern (per-file objects in build/obj/)
#   make test             build and run the unit tests and the golden tests
#                         (including `tern test std`, about 3 s)
#   make test-std         only `tern test std`
#   make examples         check, test and build the projects in examples/ (part of `make test`)
#   make test-docs        only the ```tern snippets of README.md and docs/guide/
#   make unity            one-command build: $(CC) ... -o tern src/unity.c
#   make check-warnings   -Werror builds of everything as C99 and as C11
#   make fuzz             libFuzzer parser harness build/fuzz_parser (clang;
#                         on macOS: FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang)
#   make clean
#
# Variables: CC, STD (c11 | c99), OPT, WARN, EXTRA (appended to CFLAGS),
# CFLAGS (overrides all of the above), LDFLAGS, LDLIBS.
#   make CC=gcc STD=c99 EXTRA=-Werror
#   make EXTRA="-fsanitize=address,undefined -g" LDFLAGS="-fsanitize=address,undefined" test
#
# When adding a source file: add it to its directory's *_SRCS and *_OBJS
# lists, give it an object rule in that directory's section, and add it to
# src/unity.c.  Each directory has its own section so that parallel work in
# different directories merges cleanly.

.POSIX:
.SUFFIXES:

CC      = cc
STD     = c11
OPT     = -O2
WARN    = -Wall -Wextra -Wpedantic
EXTRA   =
CFLAGS  = -std=$(STD) $(OPT) $(WARN) $(EXTRA)
LDFLAGS =
LDLIBS  =
OBJ     = build/obj
BIN     = tern
PREFIX  = /usr/local

all: $(BIN)

# Every header; any change rebuilds everything (simple and always correct).
HDRS = \
	src/base/common.h src/base/arena.h src/base/str.h src/base/vec.h \
	src/base/map.h src/base/bignum.h src/base/json.h src/base/utf8.h \
	src/base/diag.h src/base/args.h \
	src/os/os.h \
	src/syntax/token.h src/syntax/lexer.h src/syntax/ast.h src/syntax/parser.h \
	src/ir/ir.h \
	src/emit/emit.h src/emit/svparam.h \
	src/sim/sim.h src/sim/sim_impl.h src/sim/testrun.h \
	src/pkg/pkg.h \
	src/lsp/lsp.h src/lsp/lsp_impl.h \
	src/tools/tools.h \
	src/sema/sema.h \
	src/elab/value.h src/elab/elab.h src/elab/elab_impl.h \
	src/check/check.h src/check/check_impl.h \
	src/backend/backend.h \
	src/cmds.h src/std_embed.h src/driver.h

# --- os/
OS_SRCS = \
	src/os/os_posix.c \
	src/os/os_win32.c \
	src/os/os_common.c
OS_OBJS = \
	$(OBJ)/os_posix.o \
	$(OBJ)/os_win32.o \
	$(OBJ)/os_common.o
$(OBJ)/os_posix.o: src/os/os_posix.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_posix.c -o $@
$(OBJ)/os_win32.o: src/os/os_win32.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_win32.c -o $@
$(OBJ)/os_common.o: src/os/os_common.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_common.c -o $@

# --- base/
BASE_SRCS = \
	src/base/common.c \
	src/base/arena.c \
	src/base/str.c \
	src/base/map.c \
	src/base/bignum.c \
	src/base/json.c \
	src/base/utf8.c \
	src/base/diag.c \
	src/base/args.c
BASE_OBJS = \
	$(OBJ)/base_common.o \
	$(OBJ)/base_arena.o \
	$(OBJ)/base_str.o \
	$(OBJ)/base_map.o \
	$(OBJ)/base_bignum.o \
	$(OBJ)/base_json.o \
	$(OBJ)/base_utf8.o \
	$(OBJ)/base_diag.o \
	$(OBJ)/base_args.o
$(OBJ)/base_common.o: src/base/common.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/common.c -o $@
$(OBJ)/base_arena.o: src/base/arena.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/arena.c -o $@
$(OBJ)/base_str.o: src/base/str.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/str.c -o $@
$(OBJ)/base_map.o: src/base/map.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/map.c -o $@
$(OBJ)/base_bignum.o: src/base/bignum.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/bignum.c -o $@
$(OBJ)/base_json.o: src/base/json.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/json.c -o $@
$(OBJ)/base_utf8.o: src/base/utf8.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/utf8.c -o $@
$(OBJ)/base_diag.o: src/base/diag.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/diag.c -o $@
$(OBJ)/base_args.o: src/base/args.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/base/args.c -o $@

# --- syntax/
SYNTAX_SRCS = \
	src/syntax/lexer.c \
	src/syntax/parser.c \
	src/syntax/ast_dump.c
SYNTAX_OBJS = \
	$(OBJ)/syntax_lexer.o \
	$(OBJ)/syntax_parser.o \
	$(OBJ)/syntax_ast_dump.o
$(OBJ)/syntax_lexer.o: src/syntax/lexer.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/lexer.c -o $@
$(OBJ)/syntax_parser.o: src/syntax/parser.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/parser.c -o $@
$(OBJ)/syntax_ast_dump.o: src/syntax/ast_dump.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/ast_dump.c -o $@

# --- sema/
SEMA_SRCS = \
	src/sema/resolve.c \
	src/sema/types.c \
	src/sema/info.c \
	src/sema/syninfo.c
SEMA_OBJS = \
	$(OBJ)/sema_resolve.o \
	$(OBJ)/sema_types.o \
	$(OBJ)/sema_info.o \
	$(OBJ)/sema_syninfo.o
$(OBJ)/sema_resolve.o: src/sema/resolve.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sema/resolve.c -o $@
$(OBJ)/sema_types.o: src/sema/types.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sema/types.c -o $@
$(OBJ)/sema_info.o: src/sema/info.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sema/info.c -o $@
$(OBJ)/sema_syninfo.o: src/sema/syninfo.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sema/syninfo.c -o $@

# --- elab/
ELAB_SRCS = \
	src/elab/value.c \
	src/elab/consteval.c \
	src/elab/mono.c \
	src/elab/elab.c \
	src/elab/regfile.c
ELAB_OBJS = \
	$(OBJ)/elab_value.o \
	$(OBJ)/elab_consteval.o \
	$(OBJ)/elab_mono.o \
	$(OBJ)/elab_elab.o \
	$(OBJ)/elab_regfile.o
$(OBJ)/elab_value.o: src/elab/value.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/elab/value.c -o $@
$(OBJ)/elab_consteval.o: src/elab/consteval.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/elab/consteval.c -o $@
$(OBJ)/elab_mono.o: src/elab/mono.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/elab/mono.c -o $@
$(OBJ)/elab_elab.o: src/elab/elab.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/elab/elab.c -o $@
$(OBJ)/elab_regfile.o: src/elab/regfile.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/elab/regfile.c -o $@

# --- ir/
IR_SRCS = \
	src/ir/ir_build.c \
	src/ir/ir_json.c
IR_OBJS = \
	$(OBJ)/ir_build.o \
	$(OBJ)/ir_json.o
$(OBJ)/ir_build.o: src/ir/ir_build.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/ir/ir_build.c -o $@
$(OBJ)/ir_json.o: src/ir/ir_json.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/ir/ir_json.c -o $@

# --- check/
CHECK_SRCS = \
	src/check/check.c \
	src/check/drivers.c \
	src/check/assign.c \
	src/check/cdc.c \
	src/check/unused.c
CHECK_OBJS = \
	$(OBJ)/check_check.o \
	$(OBJ)/check_drivers.o \
	$(OBJ)/check_assign.o \
	$(OBJ)/check_cdc.o \
	$(OBJ)/check_unused.o
$(OBJ)/check_check.o: src/check/check.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/check/check.c -o $@
$(OBJ)/check_drivers.o: src/check/drivers.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/check/drivers.c -o $@
$(OBJ)/check_assign.o: src/check/assign.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/check/assign.c -o $@
$(OBJ)/check_cdc.o: src/check/cdc.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/check/cdc.c -o $@
$(OBJ)/check_unused.o: src/check/unused.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/check/unused.c -o $@

# --- emit/
EMIT_SRCS = \
	src/emit/sv.c \
	src/emit/svparam.c \
	src/emit/constraints.c \
	src/emit/regmap.c \
	src/emit/cmd_emit.c
EMIT_OBJS = \
	$(OBJ)/emit_sv.o \
	$(OBJ)/emit_svparam.o \
	$(OBJ)/emit_constraints.o \
	$(OBJ)/emit_regmap.o \
	$(OBJ)/emit_cmd_emit.o
$(OBJ)/emit_sv.o: src/emit/sv.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/sv.c -o $@
$(OBJ)/emit_svparam.o: src/emit/svparam.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/svparam.c -o $@
$(OBJ)/emit_constraints.o: src/emit/constraints.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/constraints.c -o $@
$(OBJ)/emit_regmap.o: src/emit/regmap.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/regmap.c -o $@
$(OBJ)/emit_cmd_emit.o: src/emit/cmd_emit.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/cmd_emit.c -o $@

# --- sim/
SIM_SRCS = \
	src/sim/bitvec.c \
	src/sim/netlist.c \
	src/sim/eval.c \
	src/sim/vcd.c \
	src/sim/remote.c \
	src/sim/testrun.c \
	src/sim/cmd_test.c \
	src/sim/cmd_sim.c
SIM_OBJS = \
	$(OBJ)/sim_bitvec.o \
	$(OBJ)/sim_netlist.o \
	$(OBJ)/sim_eval.o \
	$(OBJ)/sim_vcd.o \
	$(OBJ)/sim_remote.o \
	$(OBJ)/sim_testrun.o \
	$(OBJ)/sim_cmd_test.o \
	$(OBJ)/sim_cmd_sim.o
$(OBJ)/sim_bitvec.o: src/sim/bitvec.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/bitvec.c -o $@
$(OBJ)/sim_netlist.o: src/sim/netlist.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/netlist.c -o $@
$(OBJ)/sim_eval.o: src/sim/eval.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/eval.c -o $@
$(OBJ)/sim_vcd.o: src/sim/vcd.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/vcd.c -o $@
$(OBJ)/sim_remote.o: src/sim/remote.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/remote.c -o $@
$(OBJ)/sim_testrun.o: src/sim/testrun.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/testrun.c -o $@
$(OBJ)/sim_cmd_test.o: src/sim/cmd_test.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/cmd_test.c -o $@
$(OBJ)/sim_cmd_sim.o: src/sim/cmd_sim.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/cmd_sim.c -o $@

# --- backend/
BACKEND_SRCS = \
	src/backend/target.c \
	src/backend/build.c \
	src/backend/params.c \
	src/backend/vlsim.c \
	src/backend/vlharness.c \
	src/backend/verilator.c \
	src/backend/vcdread.c \
	src/backend/formal.c \
	src/backend/prove.c \
	src/backend/synth.c \
	src/backend/cmd_synth.c
BACKEND_OBJS = \
	$(OBJ)/backend_target.o \
	$(OBJ)/backend_build.o \
	$(OBJ)/backend_params.o \
	$(OBJ)/backend_vlsim.o \
	$(OBJ)/backend_vlharness.o \
	$(OBJ)/backend_verilator.o \
	$(OBJ)/backend_vcdread.o \
	$(OBJ)/backend_formal.o \
	$(OBJ)/backend_prove.o \
	$(OBJ)/backend_synth.o \
	$(OBJ)/backend_cmd_synth.o
$(OBJ)/backend_target.o: src/backend/target.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/target.c -o $@
$(OBJ)/backend_build.o: src/backend/build.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/build.c -o $@
$(OBJ)/backend_params.o: src/backend/params.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/params.c -o $@
$(OBJ)/backend_vlsim.o: src/backend/vlsim.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/vlsim.c -o $@
$(OBJ)/backend_vlharness.o: src/backend/vlharness.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/vlharness.c -o $@
$(OBJ)/backend_verilator.o: src/backend/verilator.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/verilator.c -o $@
$(OBJ)/backend_vcdread.o: src/backend/vcdread.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/vcdread.c -o $@
$(OBJ)/backend_formal.o: src/backend/formal.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/formal.c -o $@
$(OBJ)/backend_prove.o: src/backend/prove.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/prove.c -o $@
$(OBJ)/backend_synth.o: src/backend/synth.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/synth.c -o $@
$(OBJ)/backend_cmd_synth.o: src/backend/cmd_synth.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/backend/cmd_synth.c -o $@

# --- pkg/
PKG_SRCS = \
	src/pkg/toml.c \
	src/pkg/manifest.c \
	src/pkg/lock.c \
	src/pkg/git.c \
	src/pkg/cmd_pkg.c
PKG_OBJS = \
	$(OBJ)/pkg_toml.o \
	$(OBJ)/pkg_manifest.o \
	$(OBJ)/pkg_lock.o \
	$(OBJ)/pkg_git.o \
	$(OBJ)/pkg_cmd_pkg.o
$(OBJ)/pkg_toml.o: src/pkg/toml.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/pkg/toml.c -o $@
$(OBJ)/pkg_manifest.o: src/pkg/manifest.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/pkg/manifest.c -o $@
$(OBJ)/pkg_lock.o: src/pkg/lock.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/pkg/lock.c -o $@
$(OBJ)/pkg_git.o: src/pkg/git.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/pkg/git.c -o $@
$(OBJ)/pkg_cmd_pkg.o: src/pkg/cmd_pkg.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/pkg/cmd_pkg.c -o $@

# --- lsp/
LSP_SRCS = \
	src/lsp/rpc.c \
	src/lsp/lsp.c \
	src/lsp/lsp_nav.c
LSP_OBJS = \
	$(OBJ)/lsp_rpc.o \
	$(OBJ)/lsp_lsp.o \
	$(OBJ)/lsp_nav.o
$(OBJ)/lsp_rpc.o: src/lsp/rpc.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/lsp/rpc.c -o $@
$(OBJ)/lsp_lsp.o: src/lsp/lsp.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/lsp/lsp.c -o $@
$(OBJ)/lsp_nav.o: src/lsp/lsp_nav.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/lsp/lsp_nav.c -o $@

# --- tools/
TOOLS_SRCS = \
	src/tools/import.c \
	src/tools/remap.c \
	src/tools/fmt.c \
	src/tools/doc.c
TOOLS_OBJS = \
	$(OBJ)/tools_import.o \
	$(OBJ)/tools_remap.o \
	$(OBJ)/tools_fmt.o \
	$(OBJ)/tools_doc.o
$(OBJ)/tools_import.o: src/tools/import.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/tools/import.c -o $@
$(OBJ)/tools_remap.o: src/tools/remap.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/tools/remap.c -o $@
$(OBJ)/tools_fmt.o: src/tools/fmt.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/tools/fmt.c -o $@
$(OBJ)/tools_doc.o: src/tools/doc.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/tools/doc.c -o $@

# --- src/
MAIN_SRCS = \
	src/std_embed.c \
	src/driver.c \
	src/cmd_parse.c \
	src/cmd_check.c \
	src/main.c
MAIN_OBJS = \
	$(OBJ)/std_embed.o \
	$(OBJ)/driver.o \
	$(OBJ)/cmd_parse.o \
	$(OBJ)/cmd_check.o \
	$(OBJ)/main.o
$(OBJ)/std_embed.o: src/std_embed.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/std_embed.c -o $@
$(OBJ)/driver.o: src/driver.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/driver.c -o $@
$(OBJ)/cmd_parse.o: src/cmd_parse.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/cmd_parse.c -o $@
$(OBJ)/cmd_check.o: src/cmd_check.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/cmd_check.c -o $@
$(OBJ)/main.o: src/main.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/main.c -o $@

SRCS = \
	$(OS_SRCS) \
	$(BASE_SRCS) \
	$(SYNTAX_SRCS) \
	$(SEMA_SRCS) \
	$(ELAB_SRCS) \
	$(IR_SRCS) \
	$(CHECK_SRCS) \
	$(EMIT_SRCS) \
	$(SIM_SRCS) \
	$(BACKEND_SRCS) \
	$(PKG_SRCS) \
	$(LSP_SRCS) \
	$(TOOLS_SRCS) \
	$(MAIN_SRCS)
OBJS = \
	$(OS_OBJS) \
	$(BASE_OBJS) \
	$(SYNTAX_OBJS) \
	$(SEMA_OBJS) \
	$(ELAB_OBJS) \
	$(IR_OBJS) \
	$(CHECK_OBJS) \
	$(EMIT_OBJS) \
	$(SIM_OBJS) \
	$(BACKEND_OBJS) \
	$(PKG_OBJS) \
	$(LSP_OBJS) \
	$(TOOLS_OBJS) \
	$(MAIN_OBJS)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/.dir:
	mkdir -p $(OBJ)
	touch $@

# ------------------------------------------------------ embedded std lib ---
# tools/embed runs on every build but only rewrites src/std_embed.c when
# the std/ sources changed, so nothing is recompiled otherwise.

tools/embed: tools/embed.c src/base/common.c src/base/common.h $(OS_SRCS) src/os/os.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/embed.c $(LDLIBS)

src/std_embed.c: tools/embed FORCE
	./tools/embed std src/std_embed.c

FORCE:

# ---------------------------------------------------------------- tests ---
# The test programs are single translation units that include the sources
# they need.  To add one: add it to UNIT_TESTS (one per line), give it a
# rule, and add it to build.bat.

TEST_DEPS = $(BASE_SRCS) $(OS_SRCS) $(HDRS)

UNIT_TESTS = \
	build/test_base \
	build/test_sim \
	build/test_pkg \
	build/test_tools \
	build/test_fmt \
	build/test_emit \
	build/test_backend \
	build/test_lsp \
	build/test_elab

tests/run: tests/run.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/run.c $(LDLIBS)

# Extracts the ```tern code blocks of README.md and docs/guide/*.md and runs
# `tern check` (and `tern test` for ```tern,test) on each one.
build/doc_snippets: tests/doc_snippets.c $(TEST_DEPS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/doc_snippets.c $(LDLIBS)

build/test_base: tests/unit/test_base.c $(TEST_DEPS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_base.c $(LDLIBS)

build/test_sim: tests/unit/test_sim.c $(TEST_DEPS) $(IR_SRCS) $(SIM_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_sim.c $(LDLIBS)

build/test_pkg: tests/unit/test_pkg.c $(TEST_DEPS) $(PKG_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_pkg.c $(LDLIBS)

build/test_tools: tests/unit/test_tools.c $(TEST_DEPS) $(TOOLS_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_tools.c $(LDLIBS)

build/test_fmt: tests/unit/test_fmt.c $(TEST_DEPS) $(SYNTAX_SRCS) $(TOOLS_SRCS) src/std_embed.c $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_fmt.c $(LDLIBS)

build/test_backend: tests/unit/test_backend.c $(TEST_DEPS) $(IR_SRCS) $(EMIT_SRCS) $(BACKEND_SRCS) src/pkg/toml.c $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_backend.c $(LDLIBS)

build/test_emit: tests/unit/test_emit.c $(TEST_DEPS) $(IR_SRCS) $(SIM_SRCS) $(EMIT_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_emit.c $(LDLIBS)

build/test_lsp: tests/unit/test_lsp.c $(TEST_DEPS) $(SYNTAX_SRCS) $(IR_SRCS) $(SEMA_SRCS) $(ELAB_SRCS) $(CHECK_SRCS) $(PKG_SRCS) $(LSP_SRCS) src/tools/fmt.c src/std_embed.c src/driver.c $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_lsp.c $(LDLIBS)

build/test_elab: tests/unit/test_elab.c $(TEST_DEPS) $(IR_SRCS) $(SIM_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_elab.c $(LDLIBS)

test: $(BIN) tests/run build/doc_snippets $(UNIT_TESTS)
	for t in $(UNIT_TESTS); do ./$$t || exit 1; done
	./$(BIN) parse std
	./$(BIN) fmt --check std
	./tests/run --tern ./$(BIN)
	$(MAKE) examples
	$(MAKE) test-verilator
	./build/doc_snippets --tern ./$(BIN)

# The example projects (about a second): tern check, test, build (SV and
# Verilog-2005) and fmt --check in each; with iverilog on PATH, also their
# sim/tb_*.sv benches against the emitted RTL. Output only on failure.
EXAMPLES = uart_loopback sensor_bridge smoother
# `tern test --backend verilator` on tests/fixtures/verilator (an extern
# SystemVerilog module), and without --backend (extern designs pick
# Verilator); skipped when verilator is not installed
test-verilator: $(BIN)
	@if command -v verilator >/dev/null 2>&1; then \
	  cd tests/fixtures/verilator && ../../../$(BIN) test --backend verilator && \
	  ../../../$(BIN) test; \
	else echo "test-verilator: verilator not installed, skipped"; fi

examples: $(BIN)
	@for e in $(EXAMPLES); do \
	  d=examples/$$e; t=../../$(BIN); \
	  out=`cd $$d && $$t check --diag-format=short 2>&1 >/dev/null && \
	    $$t test --diag-format=short 2>&1 && $$t build -q 2>&1 && \
	    $$t build -q --target v2005 2>&1 && $$t fmt --check 2>&1` || \
	    { echo "$$out"; echo "$$d: FAILED"; exit 1; }; \
	  if command -v iverilog >/dev/null 2>&1; then \
	    for tb in $$d/sim/tb_*.sv; do \
	      [ -f "$$tb" ] || continue; \
	      out=`cd $$d && { iverilog -g2012 -o build/tb.vvp -c build/default/*.f sim/\`basename $$tb\` \
	        2>build/iverilog.log && vvp -n build/tb.vvp 2>&1 || cat build/iverilog.log; }`; \
	      case "$$out" in *PASS*) ;; *) echo "$$out"; echo "$$tb: FAILED"; exit 1;; esac; \
	    done; \
	  fi; \
	  echo "examples/$$e: ok"; \
	done

# the documentation snippets only
test-docs: $(BIN) build/doc_snippets
	./build/doc_snippets --tern ./$(BIN)

# `tern test std` (also run by tests/run as part of `make test`)
test-std: $(BIN)
	./$(BIN) test --diag-format=short std

# ----------------------------------------------------------------- fuzz ---
# Not part of `all`/`test`: needs clang with libFuzzer. Run it with e.g.
#   ./build/fuzz_parser -max_total_time=120 build/fuzz-corpus tests/fixtures/parse std

FUZZ_CC = clang
FUZZ_FLAGS = -g -O1 -fsanitize=fuzzer,address,undefined

fuzz: $(OBJ)/.dir
	mkdir -p build/fuzz-corpus
	$(FUZZ_CC) -std=c11 $(FUZZ_FLAGS) $(WARN) -o build/fuzz_parser tests/fuzz/fuzz_parser.c

# ---------------------------------------------------------------- other ---

unity:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BIN) src/unity.c $(LDLIBS)

check-warnings:
	for s in c99 c11; do \
	  $(MAKE) STD=$$s OBJ=build/obj-$$s BIN=build/tern-$$s EXTRA=-Werror build/tern-$$s || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/unity-$$s src/unity.c || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/run-$$s tests/run.c || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/doc_snippets-$$s tests/doc_snippets.c || exit 1; \
	  for t in $(UNIT_TESTS); do n=`basename $$t`; \
	    $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/$$n-$$s tests/unit/$$n.c || exit 1; done; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/embed-$$s tools/embed.c || exit 1; \
	done
	@echo "check-warnings: OK (c99 and c11, -Werror)"

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/tern

clean:
	rm -rf build tests/out
	rm -f tern tern.exe tools/embed tools/embed.exe tests/run tests/run.exe

.PHONY: all test test-std test-verilator test-docs examples unity check-warnings fuzz install clean FORCE
