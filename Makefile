# Makefile — POSIX make build of the Tern compiler.
#
#   make                  build ./tern (per-file objects in build/obj/)
#   make test             build and run the unit tests and the golden tests
#   make unity            one-command build: $(CC) ... -o tern src/unity.c
#   make check-warnings   -Werror builds of everything as C99 and as C11
#   make clean
#
# Variables: CC, STD (c11 | c99), OPT, WARN, EXTRA (appended to CFLAGS),
# CFLAGS (overrides all of the above), LDFLAGS, LDLIBS.
#   make CC=gcc STD=c99 EXTRA=-Werror
#   make EXTRA="-fsanitize=address,undefined -g" LDFLAGS="-fsanitize=address,undefined" test
#
# When adding a source file: add it to SRCS/OBJS, give it an object rule
# below, and add it to src/unity.c.

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

HDRS = \
	src/base/common.h src/base/arena.h src/base/str.h src/base/vec.h \
	src/base/map.h src/base/bignum.h src/base/json.h src/base/utf8.h \
	src/base/diag.h src/base/args.h \
	src/os/os.h \
	src/ir/ir.h \
	src/syntax/token.h src/syntax/lexer.h src/syntax/ast.h src/syntax/parser.h \
	src/cmds.h src/std_embed.h

BASE_SRCS = \
	src/base/common.c src/base/arena.c src/base/str.c src/base/map.c \
	src/base/bignum.c src/base/json.c src/base/utf8.c src/base/diag.c \
	src/base/args.c

OS_SRCS = src/os/os_posix.c src/os/os_win32.c src/os/os_common.c

SYNTAX_SRCS = src/syntax/lexer.c src/syntax/parser.c src/syntax/ast_dump.c

SRCS = $(OS_SRCS) $(BASE_SRCS) $(SYNTAX_SRCS) \
	src/std_embed.c src/cmd_parse.c src/cmd_check.c src/cmd_stubs.c src/main.c

OBJS = \
	$(OBJ)/os_posix.o $(OBJ)/os_win32.o $(OBJ)/os_common.o \
	$(OBJ)/base_common.o $(OBJ)/base_arena.o $(OBJ)/base_str.o \
	$(OBJ)/base_map.o $(OBJ)/base_bignum.o $(OBJ)/base_json.o \
	$(OBJ)/base_utf8.o $(OBJ)/base_diag.o $(OBJ)/base_args.o \
	$(OBJ)/syntax_lexer.o $(OBJ)/syntax_parser.o $(OBJ)/syntax_ast_dump.o \
	$(OBJ)/std_embed.o $(OBJ)/cmd_parse.o $(OBJ)/cmd_check.o \
	$(OBJ)/cmd_stubs.o $(OBJ)/main.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/.dir:
	mkdir -p $(OBJ)
	touch $@

# ------------------------------------------------------------- objects ---

$(OBJ)/os_posix.o: src/os/os_posix.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_posix.c -o $@
$(OBJ)/os_win32.o: src/os/os_win32.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_win32.c -o $@
$(OBJ)/os_common.o: src/os/os_common.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/os/os_common.c -o $@

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

$(OBJ)/syntax_lexer.o: src/syntax/lexer.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/lexer.c -o $@
$(OBJ)/syntax_parser.o: src/syntax/parser.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/parser.c -o $@
$(OBJ)/syntax_ast_dump.o: src/syntax/ast_dump.c $(HDRS) $(OBJ)/.dir
	$(CC) $(CFLAGS) -c src/syntax/ast_dump.c -o $@

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

# ------------------------------------------------------ embedded std lib ---
# tools/embed runs on every build but only rewrites src/std_embed.c when
# the std/ sources changed, so nothing is recompiled otherwise.

tools/embed: tools/embed.c src/base/common.c src/base/common.h $(OS_SRCS) src/os/os.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tools/embed.c $(LDLIBS)

src/std_embed.c: tools/embed FORCE
	./tools/embed std src/std_embed.c

FORCE:

# ---------------------------------------------------------------- tests ---
# The test programs are single translation units that include the base and
# os sources they need, so they build the same way everywhere.

TEST_DEPS = $(BASE_SRCS) $(OS_SRCS) $(HDRS)

tests/run: tests/run.c $(TEST_DEPS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/run.c $(LDLIBS)

build/test_base: tests/unit/test_base.c $(TEST_DEPS) $(OBJ)/.dir
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit/test_base.c $(LDLIBS)

test: $(BIN) tests/run build/test_base
	./build/test_base
	./$(BIN) parse std
	./tests/run --tern ./$(BIN)

# ---------------------------------------------------------------- other ---

unity:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BIN) src/unity.c $(LDLIBS)

check-warnings:
	for s in c99 c11; do \
	  $(MAKE) STD=$$s OBJ=build/obj-$$s BIN=build/tern-$$s EXTRA=-Werror build/tern-$$s || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/unity-$$s src/unity.c || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/run-$$s tests/run.c || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/test_base-$$s tests/unit/test_base.c || exit 1; \
	  $(CC) -std=$$s $(OPT) $(WARN) -Werror -o build/embed-$$s tools/embed.c || exit 1; \
	done
	@echo "check-warnings: OK (c99 and c11, -Werror)"

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/tern

clean:
	rm -rf build tests/out
	rm -f tern tern.exe tools/embed tools/embed.exe tests/run tests/run.exe

.PHONY: all test unity check-warnings install clean
