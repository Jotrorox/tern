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
  `-Wall -Wextra -Wpedantic -Werror` on GCC and Clang, C11 and C99
  (`make check-warnings` checks all of it).
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
  `os/os_posix.c` must stay the first file in `unity.c` (it defines the POSIX
  feature-test macros, which must precede every system header).
  `os_win32.c` includes `<windows.h>` (with `WIN32_LEAN_AND_MEAN`, `NOMINMAX`,
  `NOGDI`, `NOUSER`) and `#undef`s common-word macros (`near`, `far`, `IN`,
  `OUT`, `ERROR`, `DELETE`, `interface`, `small`, ...) at its end, but avoid
  such identifiers anyway: in the Windows unity build every file comes after it.
* Memory: arenas per phase (AST per file, elaboration, IR). Hot paths never
  call `free`. Growable arrays (`vec.h`) use the heap and are freed in bulk.
* Diagnostics are the product: every error has a span, the elaboration path
  (`Top > rx_fifo: Fifo<u8, 64>`), and a one-line fix where possible. Never
  stop at the first error.

## Source layout

| Dir | Files | Owner/role |
|---|---|---|
| `src/base/` | `common.[ch] arena.[ch] str.[ch] map.[ch] vec.h bignum.[ch] json.[ch] utf8.[ch] diag.[ch] args.[ch]` | utilities |
| `src/os/` | `os.h os_posix.c os_win32.c os_common.c` | the only OS-touching code (`os_common.c` is the portable half — paths, helpers — and includes no OS header) |
| `src/syntax/` | `token.h lexer.[ch] ast.h parser.[ch] ast_dump.c` | lexer, Pratt/recursive-descent parser, AST (`ast.h` is the contract; concrete grammar in `docs/grammar.md`) |
| `src/sema/` | `resolve.c types.c domains.c` (+ `sema.h`) | name binding, type/width rules, clock domains |
| `src/elab/` | `value.[ch] consteval.c elab.c mono.c` (+ `elab.h`) | const interpreter, generics, unrolling, AST→IR |
| `src/ir/` | `ir.h ir_build.c ir_json.c` | typed IR (contract below) and JSON form |
| `src/check/` | `drivers.c assign.c cdc.c unused.c` | multiple/missing drivers, no-latch proof, CDC report, unused |
| `src/emit/` | `sv.c constraints.c regmap.c` | SystemVerilog/Verilog-2005, .xdc/.sdc, regmap .h/.svd/.md |
| `src/sim/` | `bitvec.c netlist.c eval.c testrun.c vcd.c` (+ `sim.h`, private `sim_impl.h`) | cycle-based simulator and `test` interpreter |
| `src/backend/` | `verilator.c formal.c synth.c` | scripts for external tools |
| `src/pkg/` | `toml.c manifest.c lock.c git.c cmd_pkg.c` (+ `pkg.h`) | tern.toml, tern.lock, semver, dependency resolution, git; `tern new/add/update/vendor/publish` (docs/packages.md, docs/registry.md) |
| `src/lsp/` | `rpc.c lsp.c` | JSON-RPC language server over stdio |
| `src/tools/` | `fmt.c doc.c import.c remap.c` | formatter, HTML docs, `tern import` (SV → extern), `tern remap` (docs/migration.md) |
| `src/` | `main.c cmds.h cmd_stubs.c cmd_parse.c cmd_check.c driver.c unity.c std_embed.[ch]` | CLI entry + help, command prototypes, stubs for unimplemented commands, `tern parse` and the interim parse-only `tern check`, shared front-end driver, embedded std |
| `std/` | `*.tern` | standard library, written in Tern (package `std::<pkg>` lives in `std/<pkg>/`) |
| `tools/` | `embed.c` | embeds `std/**/*.tern` into `src/std_embed.c` (committed) |
| `tests/` | `run.c`, `unit/test_{base,sim,pkg,tools,fmt}.c`, `fixtures/**`, `fuzz/fuzz_parser.c` | golden-file test runner, unit tests, fixtures, libFuzzer parser harness (`make fuzz`) |

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

The headers are the reference (each starts with a usage comment); summary:

* `common.h`: includes the allowed std headers; `TERN_STATIC_ASSERT(cond,name)`,
  `TERN_UNUSED(x)`, `TERN_ARRAY_LEN(a)`, `TERN_MIN/MAX`, `TERN_ASSERT(cond)`
  (always on), `TERN_PRINTF(fmt,args)` (format-checking attribute on GCC/Clang
  only), `xmalloc/xcalloc/xrealloc/xstrdup/xstrndup/xmemdup` (abort on OOM),
  `tern_fatal(fmt,...)` (internal errors only),
  `tern_is_digit/is_xdigit/is_alpha/is_alnum/is_ident/is_space/is_upper/is_lower/
  to_lower/to_upper/hex_value` (replacing `<ctype.h>`),
  `tern_streq/starts_with/ends_with/strcasecmp`, `tern_parse_u64`,
  `TERN_VERSION` string. Implemented in `common.c`.
* `arena.h`: `Arena` (value type, zero-init is valid), `arena_alloc(Arena*,size)`
  (16-byte aligned, never NULL), `arena_zalloc`, `arena_memdup`,
  `arena_strndup(Arena*,const char*,size_t)`, `arena_strdup`, `arena_printf`,
  `arena_release(Arena*)`, `arena_mark`/`arena_rewind`, macros `ANEW(arena,T)`
  (zeroed) and `ANEWN(arena,T,n)` (zeroed).
* `str.h`: global intern table — `const char *intern(const char*,size_t)`,
  `intern_cstr(const char*)`, `intern_len`; interned pointers compare with `==`.
  `StrBuf` {`char *data; size_t len, cap;`} (zero-init valid; `data` is
  NUL-terminated once written) with `sb_putc, sb_puts, sb_append(sb,p,n),
  sb_printf(sb,fmt,...)`, `sb_vprintf`, `sb_indent(sb,n)`, `sb_reserve`,
  `sb_cstr` (NUL-terminated view, "" when empty), `sb_reset`, `sb_truncate`,
  `sb_take` (hands the heap string to the caller), `sb_free`.
* `vec.h`: `#define VEC(T) struct { T *data; size_t len, cap; }` used as
  `typedef VEC(Foo*) FooVec;` (zero-init valid). The macros take the vector
  **lvalue itself**, not a pointer: `vec_push(v,x)`, `vec_pop(v)`, `vec_last(v)`,
  `vec_reserve(v,n)` (total capacity >= n), `vec_resize(v,n)` (zero-fills),
  `vec_remove(v,i)` (order-preserving), `vec_clear(v)`, `vec_free(v)`.
  Through a pointer: `vec_push(p->items, x)`.
* `map.h`: open-addressing `Map` from `uint64_t` key to `uint64_t` value
  (zero-init valid; pointer keys/values via `MAP_PTR(p)` =
  `(uint64_t)(uintptr_t)p`): `map_get(const Map*,key,uint64_t*out)->int`,
  `map_put`, `map_has`, `map_del`, `map_ref(m,key,&inserted)` (get-or-insert,
  returns the value slot), `map_len`, `map_clear`, `map_free`, iteration
  `size_t it = 0; while (map_next(m, &it, &k, &v)) ...`; plus
  `hash_bytes`/`hash_u64`/`hash_combine`.
* `bignum.h`: `BigInt` arbitrary-precision signed integers (compile-time
  arithmetic), sign-magnitude with 32-bit limbs. Zero-init is the value 0;
  release with `bn_free`. Every operation writes its first argument, which may
  alias any input: `bn_copy/move/swap`, `bn_set_i64/u64`, `bn_get_i64/u64`
  (return 1 if it fits), `bn_low_u64` (mod 2^64), `bn_parse(r,s,n,base)`
  (bases 2/8/10/16, 0 = detect `0x/0o/0b`; `_` separators; optional sign),
  `bn_to_str(a,base)` / `bn_to_sb`, `bn_add/sub/mul/neg/abs/pow`,
  `bn_add_i64/mul_i64`, `bn_divmod_trunc` (C semantics) / `bn_divmod_floor`
  (Python semantics), both returning 0 on division by zero, `bn_shl` /
  `bn_shr` (arithmetic, i.e. floor), infinite-width two's complement
  `bn_and/or/xor/not`, `bn_cmp/cmp_i64/cmp_abs`, `bn_sign/is_zero/is_neg`,
  `bn_bitlen` (bits of |a|), `bn_signed_width` (minimum two's complement
  width), `bn_get_bit/set_bit` (two's complement), `bn_fits_width(a,w,signed)`,
  `bn_hash`, and the `IrBits` bridge `bn_to_limbs(a,w,width)` (wraps mod
  2^width, clears bits >= width) / `bn_from_limbs(r,w,width,is_signed)`.
* `json.h`: DOM reader `JsonValue *json_parse(Arena*, text, len, JsonError*)`
  (nodes live in the arena; the `json_get/at/len/is/as_str/as_int/as_num/as_bool`
  accessors are NULL-safe and take defaults) and a streaming writer
  `JsonWriter` (`jw_init(&w,&sb,pretty)`, `jw_object_begin/end`,
  `jw_array_begin/end`, `jw_key`, `jw_str/strn/int/uint/num/bool/null/raw/value`,
  `jw_kv_str/int/uint/bool/num`) with automatic commas and indentation;
  `json_escape(sb,s,n)` writes one quoted string literal.
* `utf8.h`: `utf8_decode` (strict RFC 3629), `utf8_encode`, `utf8_valid`,
  `utf8_count` (code points), `utf8_utf16_len` / `utf8_offset_of_utf16` (LSP).
* `diag.h`:
  ```c
  typedef struct Span { uint32_t file, off, len, line, col; } Span; /* line/col 1-based; file = source id */
  uint32_t src_add(const char *path, const char *text, size_t len); /* registers a source (text is copied) */
  const SourceFile *src_get(uint32_t id);  /* path, text, len, line table */
  void diag_error(Span, const char *fmt, ...);   void diag_warning(Span, const char *fmt, ...);
  void diag_note(Span, const char *fmt, ...);    /* attaches to the previous diagnostic */
  void diag_help(const char *fmt, ...);          /* one-line fix for the previous diagnostic */
  void diag_push_path(const char *frame); void diag_pop_path(void); /* elaboration path */
  ```
  Source ids start at 1; **file 0 means "no location"**, so a zero-initialized
  `Span` (or `span_none()`) is valid. Lex from `src_get(id)->text`. `col`
  counts code points from the line start (a tab is one); `span_make(file,off,len)`
  computes line/col from the offset, `span_join` merges two spans, `src_line`
  returns a line's text, `src_update` replaces a source's text (LSP edits). A
  diagnostic is emitted when the next one starts or on `diag_flush()` (main.c
  calls it after every command). Also: `diag_error_count`/`diag_warning_count`,
  `diag_set_strict` (`--strict`: warnings become errors),
  `diag_set_format(DIAG_FMT_HUMAN | DIAG_FMT_SHORT | DIAG_FMT_JSON)`,
  `diag_set_color`, `diag_set_output(FILE*)`, `diag_set_sink(fn,user)` so the
  LSP can collect `Diagnostic`s instead of printing them,
  `diag_path_depth`/`diag_path_truncate` (unwinding), and
  `diag_render(d,fmt,color,sb)` / `diag_render_json(d,jw)`. Human format:
  ```
  top.tern:2:10: error: width mismatch: expected uint<8>, found uint<9>
    |
  2 |     let x = a + b
    |             ^^^^^
    = help: truncate explicitly with `(a + b).trunc<8>()`
    in: Top > rx_fifo: Fifo<u8, 64>
  ```
  Notes that carry their own span get a `file:line:col: note:` block with an
  excerpt. Short format: `file:line:col: error: message`, one line per
  diagnostic (notes and help omitted). JSON: one object per line.
* `args.h`: `ArgParser` shared by all commands — `args_init(&ap,argc,argv)`;
  `while (args_next(&ap)) { if (args_flag(&ap,"waves")) ...; else if
  (args_opt(&ap,"seed|s",&val)) ...; else if (args_positional(&ap)) ...; else
  args_unknown(&ap); }`, `args_opt_u64`, `ap.errors`; supports `--flag`,
  `--opt value`, `--opt=value`, `-o value`, `--`.
* `os.h`: read/write whole file (also `os_write_file_if_changed`), copy,
  rename, remove, `os_remove_all`, `os_mkdir`/`os_mkdir_p`, `os_list_dir`
  (sorted), `os_stat` (exists, is_dir, is_file, is_link, size, mtime_ns), cwd,
  chdir, home dir, temp dir, path join/normalize/abs/dirname/basename/ext/
  relative (paths are UTF-8 and use `/`), executable path, env vars
  (`os_getenv`: use it instead of `getenv` so Windows sees UTF-8 and
  `os_setenv` changes), `os_find_program` (PATH, PATHEXT), monotonic clock,
  `os_isatty`/`os_color_enabled`, `os_cpu_count`, and processes:
  `os_spawn(argv, &OsSpawnOpts{cwd, stdin_data/stdin_len, stdin_null,
  capture_stdout, capture_stderr, merge_stderr}, &err)`, `os_proc_wait`,
  `os_proc_wait_any(procs, n, timeout_ms)` (pumps all captured output while
  waiting; used by the parallel test runner), `os_proc_exit_code/stdout/stderr/
  kill/free`, and the one-shot `os_run`.
* `std_embed.h`: `tern_std_files[]` (`{path, chunks, nchunks, len}`, sorted by
  path), `tern_std_file_count`, `tern_std_text(i)` (contiguous contents, cached).

## Command-line interface

`src/main.c` owns the command table and help text (`tern help`,
`tern help <cmd>`, `cmd_print_usage(name)`) and applies the global options,
accepted anywhere before `--`: `--color[=auto|always|never]`, `--no-color`,
`--diag-format=human|short|json`, `--strict`. A command receives
`argv[0] = "<name>"` plus its remaining arguments. Exit codes: 0 success,
1 errors in the input / failing tests, 2 usage errors and unimplemented
commands (`TERN_EXIT_*` in `cmds.h`). `tern parse --dump <file>` is an
internal command (not in `tern help`) used by the golden tests.

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

`sim.h` is the reference (its header comment states the semantics: two-phase
evaluation, reset and power-on model, assertion/property/coverage rules).
Summary:

```c
SimModel *sim_new(const IrDesign *d, const IrModule *top, uint64_t seed); /* NULL + diag_error (comb loop) */
void sim_free(SimModel*);
int  sim_find(SimModel*, const char *path);  /* "tx.valid", "rx_fifo.count", "rx_fifo.mem" -> handle or -1 */
int  sim_find_prefix(SimModel*, const char *prefix, int *out, int max); /* flattened bundle fields */
int  sim_signal_count(m); sim_signal_path/name/instance/ir(m, h); sim_type(m, h); sim_width(m, h);
int  sim_poke(SimModel*, int h, const IrBits *v);   /* top inputs and #[test_port] only, else SIM_ERR_* */
int  sim_peek(SimModel*, int h, IrBits *out);       /* + sim_poke_u64/sim_peek_u64/sim_peek_bits */
int  sim_mem_peek/sim_mem_poke(SimModel*, int h, uint64_t addr, ...);
void sim_settle(SimModel*);                         /* combinational settle */
void sim_step(SimModel*, uint32_t domain, uint32_t n); /* n cycles of a top domain */
void sim_run_ns(SimModel*, uint64_t ns);            /* all top clocks by hz (default 100 MHz) */
void sim_reset(SimModel*);                          /* assert all top resets 2 cycles, release */
uint64_t sim_cycle(m, domain); uint64_t sim_time_ps(m);
int  sim_domain_count/sim_default_domain/sim_find_domain(m, name);
void sim_set_fail_handler(SimModel*, SimFailFn, void *user); uint32_t sim_finish(SimModel*);
size_t sim_coverage(SimModel*, const SimCovPoint **out);
int  sim_vcd_open(SimModel*, const char *path); void sim_vcd_close(SimModel*);
```

## Testing the compiler

`make test` builds and runs `build/test_base` (unit tests for `src/base` and
`src/os`, `tests/unit/test_base.c`), `build/test_pkg` (`src/pkg`, including an
end-to-end run of the package commands against local bare git repositories,
skipped without git), `build/test_tools` (`tern import` goldens in
`tests/fixtures/import/<stem>.expected.tern|.log`, `tern remap`),
`build/test_fmt` (`tern fmt` is faithful and idempotent on every std file and
fixture; `tern doc` text goldens in `tests/fixtures/doc/` and an HTML smoke
test over std) and
`tests/run` (the golden-file runner, `tests/run.c`); all are single
translation units that `#include` the sources they need. The runner walks `tests/fixtures/`. Each fixture is a
`.tern` file with sibling expectation files; for each expectation, `tern`
runs (in parallel worker processes) with the fixture's directory as working
directory and the bare file name as argument:

* `foo.tern` + `foo.parse` — expected AST dump:
  `tern parse --dump foo.tern` (stdout)
* `foo.tern` + `foo.err` — expected diagnostics:
  `tern check --diag-format=short foo.tern` (stderr; one per line,
  `foo.tern:line:col: error: message`)
* `foo.tern` + `foo.sv` — expected emitted SystemVerilog:
  `tern build --stdout --diag-format=short foo.tern` (stdout)
* `foo.tern` + `foo.test` — expected `tern test` summary:
  `tern test --diag-format=short foo.tern` (stdout)
* `foo.tern` + `foo.fmt` — expected canonical formatting (docs/style.md):
  `tern fmt --stdout foo.tern` (stdout)

So these commands must accept a single `.tern` file outside a project, and
`build` must support `--stdout` (all emitted modules to stdout). Output and
expectation are compared after normalizing line endings, trailing whitespace
and trailing blank lines; the exit status is ignored unless the process
crashed. A first line starting with `// skip` marks a fixture as skipped.
Usage: `tests/run [-v] [-j N] [--tern path] [--no-std] [<filter>...]`.
`UPDATE=1 make test` rewrites mismatching expectations.

`make test` also runs `tern test --diag-format=short std` from the repository
root and requires exit status 0; while `tern test` is still the stub ("not
implemented yet", exit 2) that case is reported as skipped.

## Conventions

* Commit early and often with descriptive messages; never commit a tree
  that fails `make` or `make test` (golden tests for not-yet-implemented
  features are marked by a `// skip` first line).
* Keep `src/unity.c`, `Makefile` and `build.bat` in sync when adding files
  (`build.bat` only builds `src/unity.c`; the Makefile needs the file in
  `SRCS`/`OBJS`, any new header in `HDRS`, and an object rule).
* To implement a command, delete its stub from `src/cmd_stubs.c` and add the
  real `cmd_<name>` in its own file.
* `src/std_embed.c` is generated from `std/` by `tools/embed` (run by every
  `make`; it rewrites the file only when something changed) and committed.
* Every command-line command is implemented as `int cmd_<name>(int argc, char **argv)`
  declared in `src/cmds.h`.
