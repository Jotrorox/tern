# Contributing to Tern

Thanks for helping. This file covers building, testing, the code rules and how
CI checks a change. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the
internal contract between the parts of the compiler. Read the sections for the
directories you touch before changing them.

## Building

```sh
make -j8                        # ./tern from per-file objects in build/obj/
cc -O2 -o tern src/unity.c      # the same compiler as one translation unit
build.bat                       # MSVC (Developer Command Prompt), writes tern.exe
```

Useful variables: `make CC=gcc STD=c99 EXTRA=-Werror`, `OPT=-O0 EXTRA=-g`
for debugging, and
`make EXTRA="-fsanitize=address,undefined -g" LDFLAGS="-fsanitize=address,undefined" test`
for a sanitizer run.

The standard library (`std/**/*.tern`) is embedded into the binary by
`tools/embed.c`. `make` runs it on every build, but it rewrites
`src/std_embed.c` only when a std source changed. **`src/std_embed.c` is
committed**, so the one-command and MSVC builds need no generator step.
Commit it together with your std changes, because CI fails if it is stale.
During development, `tern` reads std from `$TERN_STD`, or from a `std/`
directory next to the binary, so std edits take effect without a rebuild.

## Testing

```sh
make test                       # everything below, about a minute on a laptop
make test-std                   # only `tern test std`
make test-docs                  # only the code blocks of README.md and docs/guide/
make check-warnings             # -Werror builds of everything as C99 and C11
```

`make test` runs, in order:

1. **Unit tests**, `build/test_*` from `tests/unit/test_*.c`. Each one is a
   single translation unit that includes the sources it tests, so it builds
   without the object files. `test_emit` also compiles every emitted file set
   with Icarus Verilog (when `iverilog` is installed) and cross-checks it
   against the built-in simulator.
2. `tern parse std` and `tern fmt --check std`. The standard library must
   parse and be formatted canonically.
3. **Golden tests**, `tests/run`. It walks `tests/fixtures/`, runs `tern` on
   each `foo.tern` once per sibling expectation file, and compares the output.
   It also runs `tern test std`.
4. **Documentation snippets**, `build/doc_snippets`. It extracts every
   ` ```tern ` block from `README.md` and `docs/guide/*.md` and runs
   `tern check` on it (see [Documentation](#documentation)).

### Golden tests

| Expectation file | Command (run in the fixture's directory) |
|---|---|
| `foo.parse` | `tern parse --dump foo.tern` (stdout) |
| `foo.err` | `tern check --diag-format=short foo.tern` (stderr) |
| `foo.sv` | `tern build --stdout --diag-format=short foo.tern` |
| `foo.test` | `tern test --diag-format=short foo.tern` |
| `foo.fmt` | `tern fmt --stdout foo.tern` |
| `foo.ir` | `tern ir --diag-format=short foo.tern` |
| `foo.unused` | `tern check --unused --diag-format=short foo.tern` (stderr) |
| `foo.cdc` | `tern check --cdc-report --diag-format=short foo.tern` |

A first line `// skip` skips a fixture. A first line `// args: --cover` adds
arguments before the file name. Output is compared after normalizing line
endings and trailing whitespace, and a crash fails the case even when the
output matches.

```sh
./tests/run                     # all fixtures (+ tern test std)
./tests/run check/latch         # only fixtures whose path contains the filter
./tests/run -v -j 1             # verbose, sequential
UPDATE=1 ./tests/run check      # rewrite mismatching expectations
UPDATE=1 make test              # rewrite every golden file, including the
                                # unit tests' goldens (test_emit, test_fmt)
```

To add a regression test, put a `.tern` file in the right
`tests/fixtures/<area>/` directory, create an empty expectation file next to
it, run `UPDATE=1 ./tests/run <name>`, and **read the generated expectation
before committing it**. A golden file records what the compiler does, which
is not necessarily what it should do.

### Unit tests

To add a unit test program, add it to `UNIT_TESTS` in the `Makefile` with its
own rule, and add it to `build.bat`. Individual cases go into the existing
`tests/unit/test_<area>.c` files.

### Fuzzing

`make fuzz` builds a libFuzzer harness for the parser (`build/fuzz_parser`,
needs clang). On macOS use Homebrew's LLVM:
`make fuzz FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang`.

## Code rules

The complete list is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) under
*Ground rules*. In short:

* **Portable C.** C11 that also compiles as C99, with no compiler extensions,
  no `_Generic`, no `<threads.h>`/`<stdatomic.h>`, no VLAs, and no anonymous
  struct or union members. Warning-free with `-Wall -Wextra -Wpedantic
  -Werror` on GCC and Clang, C99 and C11. `make check-warnings` checks all of
  that.
* **One platform layer.** Only `src/os/*` includes OS headers. Everything else
  uses `<stddef.h> <stdint.h> <stdarg.h> <stdio.h> <stdlib.h> <string.h>
  <limits.h>`. `<ctype.h>`, `<math.h>`, `<errno.h>` and `<assert.h>` are not
  allowed; the helpers live in `src/base/`.
* **Zero third-party dependencies.** Yosys, SymbiYosys, Verilator, git and gh
  are optional programs run as subprocesses, never linked. The test suite is
  C too, so no Python, shell or other interpreter is needed for `make test`.
* **Unity-build safe.** `src/unity.c` includes every `.c` file, so
  file-local helpers need unique, module-prefixed names (`lex_`, `elab_`,
  `sv_`, ...), and local macros are `#undef`ined at the end of the file.
  When you add a source file, add it to its directory's section of the
  `Makefile`, to `src/unity.c` and, for test programs, to `build.bat`.
* **Arenas.** Memory is allocated in per-phase arenas, and hot paths never
  call `free`.
* **Diagnostics are the product.** Every error has a span and the elaboration
  path, and a one-line fix (`= help: ...`) where possible. Never stop at the
  first error.
* **Standard library conventions.** See [std/README.md](std/README.md): the
  payload type is the first generic, handshakes are `Stream`/`Valid`, every
  module has `pub const LATENCY`, every pub item and port has a `///`
  comment, every documented behaviour has a `test`, and every sequential
  component has a `property`.

Tern code (std, fixtures, docs) is formatted with `tern fmt`.

## Documentation

User documentation lives in `README.md` and `docs/guide/`. The reference
documents are in `docs/`, and [docs/index.md](docs/index.md) links them all.

Every ` ```tern ` code block in `README.md` and `docs/guide/*.md` is
compiled by `make test`. The info string selects the check:

| Fence | Check |
|---|---|
| ` ```tern ` | `tern check` must pass |
| ` ```tern,test ` | `tern check` and `tern test` must pass |
| ` ```tern,fail ` | `tern check` must report an error (exit status 1) |
| ` ```tern,ignore ` | not checked; use it only for intentional fragments |

Write examples as complete files (with a `package` line) so readers can copy
them. A generic module is only elaborated when something instantiates it, so
add a `#[top]` module or a `test` that uses it. Otherwise only its syntax and
names are checked. `build/doc_snippets -v docs/guide/03-modules-and-generics.md`
checks one file, and failing snippets are kept in `build/doc-snippets/` for
inspection.

When a change alters user-visible behaviour (a command, an option, a
diagnostic, a language rule), update the guide and the relevant reference
document in the same change, and add an entry to `CHANGELOG.md`.

## Commits

* Keep each commit focused, and make sure `make test` passes at every commit.
* The subject line names the area, then says what changed, in the present
  tense, without a trailing period:
  `check: multiple/missing drivers, full assignment in comb blocks`,
  `tern lsp: usage text, upper-case drive letters from file URIs`,
  `docs: grammar for associated items and type-level paths`.
  Common areas are the directory or command names: `syntax`, `sema`, `elab`,
  `check`, `emit`, `sim`, `backend`, `pkg`, `std`, `tests`, `docs`,
  `tern <command>`.
* Use the body for the why and for a list of the changed parts, wrapped at
  about 72 columns.
* Fixes for spec conformance should cite the section (`spec §8.2`).
* Golden-file updates belong in the same commit as the change that causes
  them. Mention in the message when a golden file changed on purpose.

## Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push to
`main` and on every pull request:

| Job | What it checks |
|---|---|
| Linux gcc/clang x c99/c11 | `make test` with `-Werror` (per-file objects), the one-command unity build, and that `src/std_embed.c` is up to date |
| Emitted HDL | `make test` with Icarus Verilog installed (emitted SV cross-checked against the simulator), `tests/lint_emitted.sh` (Verilator lint, Yosys synthesis of the emitted fixtures), and `tern new`/`build --lint`/`synth`/`prove --no-run` on a sample project |
| check-warnings | `-Werror` builds of everything as C99 and C11, with GCC and Clang |
| macOS clang | `make test`, `check-warnings`, unity builds in C11 and C99 |
| Unity build | `cc -std=c11` and `cc -std=c99` on `src/unity.c` |
| Windows MSVC | `build.bat test` |
| TCC | smoke build of `tern`, `test_base` and `tests/run`, then the golden tests |
| ASan + UBSan | `make test` under AddressSanitizer and UndefinedBehaviorSanitizer |

[`.github/workflows/release.yml`](.github/workflows/release.yml) builds the
release binaries for Linux, macOS and Windows when a `v*` tag is pushed.

Before opening a pull request, run at least `make test` and
`make check-warnings` locally. If you can, also run
`make CC=clang OPT=-O1 EXTRA="-g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" test`.

## License

By contributing you agree that your contributions are licensed under the
[BSD 3-Clause License](LICENSE).
