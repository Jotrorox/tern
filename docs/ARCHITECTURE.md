# Tern reference implementation — architecture

This document is the internal contract between the parts of the compiler. The
normative language definition is the spec (`docs/tern-spec.pdf`, plain-text
extraction in `docs/spec.txt`; note that the extraction renders `->` as `→`,
`=>` as `⇒`, `|->` as `|→`, `|=>` as `|⇒`, `<<`/`>>` as `«`/`»`, and drops some
spaces — the real syntax is ASCII).

## Ground rules (spec §13.7)

* Portable C11 that also compiles with `-std=c99`. No compiler extensions, no
  `_Generic`, no `<threads.h>`, no `<stdatomic.h>`, no VLAs, no anonymous
  structs/unions (an untagged struct in a `typedef` is fine; an unnamed struct
  *member* is not). Must build warning-free with
  `-Wall -Wextra -Wpedantic -Werror` on GCC and Clang, C11 and C99.
* Only `src/os/*` may include OS headers. Everything else uses only
  `<stddef.h> <stdint.h> <stdarg.h> <stdio.h> <stdlib.h> <string.h> <limits.h>`
  (and `<ctype.h>`/`<math.h>`/`<errno.h>`/`<assert.h>` are NOT on the list — do
  not use them; write the tiny helpers in `src/base/`).
* Zero third-party dependencies. Yosys, SymbiYosys, Verilator, git, gh are
  optional external programs run as subprocesses via `os.h`.
* Build: `cc -std=c11 -O2 -o tern src/unity.c` must always work (unity.c
  `#include`s every `.c` file), as must `make` (POSIX make, per-file objects)
  and `build.bat` (MSVC). Every `.c` file must therefore be safe to include
  into one translation unit: **file-local helpers must have unique names**
  (prefix static functions/macros with the module name, e.g. `lex_`, `elab_`,
  `sv_`), and `#undef` any local macros at the end of the file.
* Memory: arenas per phase (AST per file, elaboration, IR). Hot paths never
  call `free`. Growable arrays (`vec.h`) use the heap and are freed in bulk.
* Diagnostics are the product: every error has a span, the elaboration path
  (`Top > rx_fifo: Fifo<u8, 64>`), and a one-line fix where possible. Never
  stop at the first error.

## Source layout

| Dir | Files | Owner/role |
|---|---|---|
| `src/base/` | `common.h arena.[ch] str.[ch] map.[ch] vec.h bignum.[ch] json.[ch] utf8.[ch] diag.[ch]` | utilities |
| `src/os/` | `os.h os_posix.c os_win32.c` | the only OS-touching code |
| `src/syntax/` | `token.h lexer.[ch] ast.h parser.[ch] ast_dump.c` | lexer, Pratt/recursive-descent parser, AST |
| `src/sema/` | `resolve.c types.c domains.c` (+ `sema.h`) | name binding, type/width rules, clock domains |
| `src/elab/` | `value.[ch] consteval.c elab.c mono.c` (+ `elab.h`) | const interpreter, generics, unrolling, AST→IR |
| `src/ir/` | `ir.h ir_build.c ir_json.c` | typed IR (contract below) and JSON form |
| `src/check/` | `drivers.c assign.c cdc.c unused.c` | multiple/missing drivers, no-latch proof, CDC report, unused |
| `src/emit/` | `sv.c constraints.c regmap.c` | SystemVerilog/Verilog-2005, .xdc/.sdc, regmap .h/.svd/.md |
| `src/sim/` | `bitvec.c netlist.c eval.c testrun.c vcd.c` (+ `sim.h`) | cycle-based simulator and `test` interpreter |
| `src/backend/` | `verilator.c formal.c synth.c` | scripts for external tools |
| `src/pkg/` | `toml.c lock.c git.c` (+ `pkg.h`) | tern.toml, tern.lock, dependency resolution, git |
| `src/lsp/` | `rpc.c lsp.c` | JSON-RPC language server over stdio |
| `src/tools/` | `fmt.c doc.c import.c` | formatter, HTML docs, `tern import` (SV → extern) |
| `src/` | `main.c driver.c unity.c std_embed.c` | CLI entry, shared front-end driver, embedded std |
| `std/` | `*.tern` | standard library, written in Tern (package `std::<pkg>` lives in `std/<pkg>.tern` or `std/<pkg>/`) |
| `tools/` | `embed.c` | embeds `std/**/*.tern` into `src/std_embed.c` (committed) |
| `tests/` | `run.c`, `fixtures/**` | golden-file test runner + fixtures |

## Pipeline

```
files ─lex/parse─▶ AST (per-file arena)
      ─resolve─▶ packages, scopes, items bound
      ─elaborate(top, generics)─▶ consteval + type/width check + unroll + mono
      ─▶ IR (IrDesign)  ─checks─▶ drivers / latches / CDC / unused
      ─▶ back ends: emit/sv.c, sim/*, backend/*, ir_json.c, lsp
```

`src/driver.c` owns the shared front half: discover project (`tern.toml`,
`src/`, `tests/`), load std from `std_embed.c` (or from `$TERN_STD` / a `std/`
directory next to the binary when present, for development), parse, resolve,
and expose `driver_elaborate(...)`. Every command uses it, so the emitter, the
simulator and the LSP cannot disagree.

Test blocks are **not** in the IR. `tern test` interprets their AST with the
same const-evaluator (`src/elab/consteval.c`) extended with simulation-handle
values (`src/sim/testrun.c`); `sim M<..>()` elaborates `M` into an `IrDesign`
and builds a simulator model from it.

## Base API (fixed names — other modules rely on these)

* `common.h`: includes the allowed std headers; `TERN_STATIC_ASSERT(cond,name)`,
  `TERN_UNUSED(x)`, `TERN_ARRAY_LEN(a)`, `xmalloc/xcalloc/xrealloc/xstrdup`
  (abort on OOM), `tern_is_digit/is_alpha/is_alnum/is_space/to_lower`
  (replacing `<ctype.h>`), `TERN_VERSION` string.
* `arena.h`: `Arena` (value type, zero-init is valid), `arena_alloc(Arena*,size)`
  (8/16-byte aligned), `arena_zalloc`, `arena_strndup(Arena*,const char*,size_t)`,
  `arena_release(Arena*)`, macros `ANEW(arena,T)` (zeroed) and `ANEWN(arena,T,n)`.
* `str.h`: global intern table — `const char *intern(const char*,size_t)`,
  `intern_cstr(const char*)`; interned pointers compare with `==`.
  `StrBuf` {`char *data; size_t len, cap;`} with `sb_putc, sb_puts, sb_append(sb,p,n),
  sb_printf(sb,fmt,...)`, `sb_cstr` (NUL-terminated view), `sb_reset`, `sb_free`.
* `vec.h`: `#define VEC(T) struct { T *data; size_t len, cap; }` used as
  `typedef VEC(Foo*) FooVec;`; `vec_push(v,x)`, `vec_pop(v)`, `vec_last(v)`,
  `vec_reserve(v,n)`, `vec_clear(v)`, `vec_free(v)` (macros, heap-backed).
* `map.h`: open-addressing `Map` from `uint64_t` key to `uint64_t` value
  (pointer keys/values via `(uint64_t)(uintptr_t)`): `map_get(Map*,key,uint64_t*out)->int`,
  `map_put`, `map_has`, `map_del`, `map_clear`, `map_free`, iteration helpers;
  plus `hash_bytes`/`hash_u64`.
* `bignum.h`: `BigInt` arbitrary-precision signed integers (compile-time
  arithmetic): parse/format in bases 2/8/10/16, add/sub/mul/divmod, shifts,
  two's-complement infinite-width and/or/xor/not, compare, bit length,
  get/set bit, conversion to/from int64/uint64 and to/from little-endian
  64-bit limb arrays (to interoperate with `IrBits`).
* `json.h`: DOM reader (`json_parse`) and streaming writer.
* `utf8.h`: decode/validate.
* `diag.h`:
  ```c
  typedef struct Span { uint32_t file, off, len, line, col; } Span; /* line/col 1-based; file = source id */
  uint32_t src_add(const char *path, const char *text, size_t len); /* registers a source */
  const SourceFile *src_get(uint32_t id);  /* path, text, len, line table */
  void diag_error(Span, const char *fmt, ...);   void diag_warning(Span, const char *fmt, ...);
  void diag_note(Span, const char *fmt, ...);    /* attaches to the previous diagnostic */
  void diag_help(const char *fmt, ...);          /* one-line fix for the previous diagnostic */
  void diag_push_path(const char *frame); void diag_pop_path(void); /* elaboration path */
  ```
  plus error/warning counters, `--strict` (warnings → errors), human
  rendering with source excerpt and caret, a JSON rendering, and a sink hook
  so the LSP can collect diagnostics instead of printing them.
* `os.h`: read/write whole file, mkdir -p, list directory, stat (exists,
  is_dir, mtime), remove, cwd, home dir, path join/normalize, executable
  path, env vars, monotonic clock, isatty/colour detection, spawn a child
  process with argv (optionally capturing stdout/stderr, optionally with
  stdin from a buffer), wait/poll for any child (used by the parallel test
  runner).

## IR contract

See `src/ir/ir.h` — it is the normative definition, with invariants in the
comments. Key points:

* All widths are concrete. Every IR expression has an exact result width;
  the elaborator inserts explicit `IRE_RESIZE` so arithmetic operands already
  have the result width (lossless Tern widening happens *before* the IR).
  Back ends never re-derive Tern width rules.
* Bundles are flattened at module boundaries: a port `tx: Stream<u8>` becomes
  port signals `tx_valid`, `tx_data`, `tx_ready` whose `path` is `tx.valid`
  etc. Structs and enums stay typed (`IrType`) so the emitter can produce
  packed structs/enums; arrays are values whose element `i` occupies bits
  `[i*ew +: ew]`.
* Value generics are resolved; every distinct instantiation is its own
  `IrModule` (monomorphized, mangled name `Fifo_u8_64`; modules without
  generics keep their name). Value generics are recorded as `params` so the
  emitter can print them as `localparam`s.
* Unrolled-loop names get the iteration appended (`r_0`, `r_1`).
* Processes are `comb` or clocked (by domain). Reset is not in the statements:
  the registers a clocked process drives are listed in `regs`, and each reg's
  `has_init/init` plus the domain's reset type determine the reset branch.
* Sync memory reads (`m.read(a)`) are lowered to a reset-less `IRS_REG` assigned
  `IRE_MEMREAD` inside the domain's clocked process.
* Match on enums/ints with constant patterns becomes `IRST_CASE`; matches with
  bindings/tuples/wildcards are lowered to `if` chains; match/if *expressions*
  become `IRE_MUX` chains.

## Simulator contract (`src/sim/sim.h`, defined by the simulator)

```c
SimModel *sim_new(const IrDesign *d, const IrModule *top, uint64_t seed);
int  sim_find(SimModel*, const char *path);  /* "tx.valid", "rx_fifo.count", "state" → handle or -1 */
void sim_poke(SimModel*, int h, const IrBits *v);   /* inputs and #[test_port] only */
void sim_peek(SimModel*, int h, IrBits *out);
void sim_settle(SimModel*);                      /* combinational settle */
void sim_step(SimModel*, uint32_t domain, uint32_t n); /* n edges of a domain of the top */
void sim_reset(SimModel*);                        /* assert all resets 2 cycles, release */
```
plus VCD, in-design assertion failure reporting, property monitors and
coverage counters.

## Testing the compiler

`tests/run.c` (built by `make test`) walks `tests/fixtures/`. Each fixture is a
`.tern` file with a sibling expectation:

* `foo.tern` + `foo.parse` — expected AST dump (`tern parse --dump`, internal)
* `foo.tern` + `foo.err` — expected diagnostics (one per line:
  `file:line:col: error: message`)
* `foo.tern` + `foo.sv` — expected emitted SystemVerilog
* `foo.tern` + `foo.test` — expected `tern test` summary

`make test` also runs `tern test` over `std` (`tern test std`).
`UPDATE=1 make test` rewrites expectations.

## Conventions

* Commit early and often with descriptive messages; never commit a tree
  that fails `make` or `make test` (golden tests for not-yet-implemented
  features are marked by a `// skip` first line).
* Keep `src/unity.c`, `Makefile` and `build.bat` in sync when adding files.
* Every command-line command is implemented as `int cmd_<name>(int argc, char **argv)`
  declared in `src/cmds.h`.
