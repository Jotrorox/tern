# Makefile — POSIX make build of the Tern compiler.
#
#   make                  build ./tern (per-file objects in build/obj/)
#   make test             build and run the unit tests and the golden tests
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
	src/emit/emit.h \
	src/sim/sim.h src/sim/sim_impl.h \
	src/pkg/pkg.h \
	src/tools/tools.h \
	src/cmds.h src/std_embed.h

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
SEMA_SRCS =
SEMA_OBJS =

# --- elab/
ELAB_SRCS =
ELAB_OBJS =

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
CHECK_SRCS =
CHECK_OBJS =

# --- emit/
EMIT_SRCS = \
	src/emit/sv.c \
	src/emit/constraints.c \
	src/emit/regmap.c \
	src/emit/cmd_emit.c
EMIT_OBJS = \
	$(OBJ)/emit_sv.o \
	$(OBJ)/emit_constraints.o \
	$(OBJ)/emit_regmap.o \
	$(OBJ)/emit_cmd_emit.o
$(OBJ)/emit_sv.o: src/emit/sv.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/emit/sv.c -o $@
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
	src/sim/vcd.c
SIM_OBJS = \
	$(OBJ)/sim_bitvec.o \
	$(OBJ)/sim_netlist.o \
	$(OBJ)/sim_eval.o \
	$(OBJ)/sim_vcd.o
$(OBJ)/sim_bitvec.o: src/sim/bitvec.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/bitvec.c -o $@
$(OBJ)/sim_netlist.o: src/sim/netlist.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/netlist.c -o $@
$(OBJ)/sim_eval.o: src/sim/eval.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/eval.c -o $@
$(OBJ)/sim_vcd.o: src/sim/vcd.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/sim/vcd.c -o $@

# --- backend/
BACKEND_SRCS =
BACKEND_OBJS =

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
LSP_SRCS =
LSP_OBJS =

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
	src/cmd_parse.c \
	src/cmd_check.c \
	src/cmd_stubs.c \
	src/main.c
MAIN_OBJS = \
	$(OBJ)/std_embed.o \
	$(OBJ)/cmd_parse.o \
	$(OBJ)/cmd_check.o \
	$(OBJ)/cmd_stubs.o \
	$(OBJ)/main.o
$(OBJ)/std_embed.o: src/std_embed.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/std_embed.c -o $@
$(OBJ)/cmd_parse.o: src/cmd_parse.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/cmd_parse.c -o $@
$(OBJ)/cmd_check.o: src/cmd_check.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/cmd_check.c -o $@
$(OBJ)/cmd_stubs.o: src/cmd_stubs.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/cmd_stubs.c -o $@
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
	build/test_emit

tests/run: tests/run.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/run.c $(LDLIBS)

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

build/test_emit: tests/unit/test_emit.c $(TEST_DEPS) $(IR_SRCS) $(SIM_SRCS) $(EMIT_SRCS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_emit.c $(LDLIBS)

test: $(BIN) tests/run $(UNIT_TESTS)
	for t in $(UNIT_TESTS); do ./$$t || exit 1; done
	./$(BIN) parse std
	./$(BIN) fmt --check std
	./tests/run --tern ./$(BIN)

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

.PHONY: all test unity check-warnings fuzz install clean FORCE
