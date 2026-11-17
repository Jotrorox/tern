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
| `src/sema/` | `resolve.c types.c info.c syninfo.c` (+ `sema.h`) | name binding, type/width rules, semantic info for tooling |
| `src/elab/` | `value.[ch] consteval.c elab.c mono.c regfile.c` (+ `elab.h`) | const interpreter, generics, unrolling, AST→IR, generated RegisterFile bodies |
| `src/ir/` | `ir.h ir_build.c ir_json.c` | typed IR (contract below) and JSON form |
| `src/check/` | `check.c` | multiple/missing drivers, no-latch proof, clock-domain mixing |
| `src/emit/` | `sv.c constraints.c regmap.c` | SystemVerilog/Verilog-2005, .xdc/.sdc, regmap .h/.svd/.md |
| `src/sim/` | `bitvec.c netlist.c eval.c testrun.c vcd.c` (+ `sim.h`, private `sim_impl.h`) | cycle-based simulator and `test` interpreter |
| `src/backend/` | `target.c build.c verilator.c vcdread.c formal.c prove.c synth.c cmd_synth.c` (+ `backend.h`) | `tern build`, `tern prove`, `tern synth`: targets, emitted-file output, SymbiYosys / Yosys / Vivado / Quartus scripts and result parsers |
| `src/pkg/` | `toml.c manifest.c lock.c git.c cmd_pkg.c` (+ `pkg.h`) | tern.toml, tern.lock, semver, dependency resolution, git; `tern new/add/update/vendor/publish` (docs/packages.md, docs/registry.md) |
| `src/lsp/` | `rpc.c lsp.c lsp_nav.c` (+ `lsp.h`, private `lsp_impl.h`) | JSON-RPC language server over stdio (`tern lsp`, docs/editors.md) |
| `src/tools/` | `fmt.c doc.c import.c remap.c` | formatter, HTML docs, `tern import` (SV → extern), `tern remap` (docs/migration.md) |
| `src/` | `main.c cmds.h cmd_parse.c cmd_check.c driver.c unity.c std_embed.[ch]` | CLI entry + help, command prototypes, `tern parse`, `tern check`, shared front-end driver, embedded std |
| `std/` | `*.tern` | standard library, written in Tern (package `std::<pkg>` lives in `std/<pkg>/`) |
| `tools/` | `embed.c` | embeds `std/**/*.tern` into `src/std_embed.c` (committed) |
| `tests/` | `run.c`, `doc_snippets.c`, `unit/test_{base,sim,pkg,tools,fmt,emit,lsp}.c`, `fixtures/**`, `lsp/*.session`, `fuzz/fuzz_parser.c` | golden-file test runner, documentation snippet checker (`make test-docs`), unit tests, fixtures, libFuzzer parser harness (`make fuzz`) |

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
  `diag_set_color`, `diag_set_output(FILE*)`, `diag_set_sink(fn,user)` / `diag_get_sink` (save and restore) so the
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
  `os_isatty`/`os_color_enabled`, `os_cpu_count`, `os_set_binary_stdio`,
  `os_read_stdin`/`os_stdin_wait` (unbuffered stdin with a timeout, for the
  LSP), and processes:
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
  become `IRE_MUX` chains. A statement `match` in a `comb` block may leave
  values out (§6.2); its `IRST_CASE` then ends in an empty default arm.
* Dynamic indices may be out of range when an array length is not a power of
  two (`[u8; 3]` with a `uint<2>` index): reads give 0 and writes are skipped
  (2-state). `IRE_INDEX` holds `args[0].width / width` elements; an indexed
  `IrLvalue` names its element count in `len` (JSON `"len"`), so the
  simulator, the emitter and the verifier agree even when the array shares
  its signal with other struct fields.

## Front end: elaboration details and IR checks

Beyond the parts described in `elab.h`, `sema.h` and `check.h`:

* **Type-level paths** (§9.4): `T::WIDTH`, `T::MAX`, `T::MIN` work on every
  type expression. Paths on types that are not names — `typeof(x)::WIDTH`,
  `[u8; 4]::WIDTH` — are `EXPR_QPATH` nodes (`u.qpath.type` + the member
  path), parsed as a postfix `::` after an expression that reads as a type.
  Struct and bundle bodies may hold associated `const` and `fn` items
  (`Flags::NONE`, in `struct_def.fns` / `bundle.fns`).
* **Places** (`elab.h`): besides signal, aggregate and local places there is
  `PL_DYN` — the element of an array of bundles chosen by a signal index.
  Reading it gives a mux per field (an `IRE_INDEX` into the concatenation of
  the elements), assigning it emits an `IRST_CASE` decode over the elements;
  only inside `comb`/`on` blocks.
* **`return` under conditions on signals**: inside an inlined fn,
  `Elab.fn_path` is the condition of the arm being run (relative to the fn's
  entry) and `fn_ret_cond`/`fn_ret_val` accumulate the returns seen so far;
  the result is a mux over the return points, earlier ones first, falling
  back to the tail value. Fns have no other side effects, so the code after
  a conditional return simply keeps running for the other paths.
* **Generic inference for modules**: a module instantiated without `<..>`
  gets type and width generics from the types of its connected inputs
  (`Crc32(in: words)`, §12.3); the rest keep their defaults.
* **std::bus::RegisterFile** (`src/elab/regfile.c`, §10.3): elaborating
  `RegisterFile<Map, Bus>` appends a generated body to the declared one: the
  generated Tern source (registered as `<RegisterFile<..>>`) instantiates
  the front-end for `Bus` (Axi4LiteRegPort / ApbRegPort / WishboneRegPort
  from std/bus/regfile.tern) and implements the decode, storage and
  read-back. A regmap used as a type is its `Map::Regs` bundle (the type's
  `def` is the regmap item, so `Map::Status` etc. resolve on it). Ports, one
  per field of `Map::Regs`: `rw`/`wo` registers are outputs, `ro` inputs,
  `wo T -> Stream<T>` output streams (one beat per write; a write while the
  previous beat is pending answers `err`), `w1c` registers are outputs plus
  an input `<name>_set`, `rw1s` registers outputs plus `<name>_clr`. Offsets
  are word-aligned byte offsets; registers wider than the bus take
  consecutive words; fields keep their `@` bit positions on the bus; `= v`
  gives a reset value; unmapped offsets answer `err`.
* **Memory ports** (§10.1): a `mem` read through more than two ports (distinct
  `IRE_MEMREAD`s) is an error unless it is `#[ram(distributed)]`.
* **Duplicate declarations** of a name in one module scope are errors.
* **Semantic info without elaboration** (`src/sema/syninfo.c`): after
  elaborating, the driver calls `sema_info_syntactic(S)`, which walks every
  user file with lexical scopes and records declarations (with their declared
  types as written), and the definitions of name uses. Elaborated records win,
  so this only fills in code that was never elaborated (generic modules
  without instances). `sema_info_*` is otherwise unchanged.
* **IR checks** (`src/check/check.c`, run by `tern check` when elaboration
  succeeded): multiple drivers (two blocks, or a block and a continuous
  assignment, or continuous assignments to shared bits) and undriven
  outputs/vars (§6.1); full assignment of every var/output a `comb` block
  assigns, reported with the first path that misses it ("`alu_out` may be
  unassigned when `op == Nop`", §6.2); clock-domain mixing (§5.6, §8.2):
  wires, vars and outputs take the domain of what drives them, a clocked
  process may only read its own domain, and an expression may not mix two
  ("pix-domain signal `line` used in sys-domain logic; cross with std::cdc").
  Domains over the same clock (or a clock derived by a cast) are one domain;
  `#[cdc(..)]`, `#[clock_gate]` and `#[clock_gen]` modules and `unsafe_cross`
  wires are exempt.

## Design checks (`src/check/`)

`check_design(IrDesign*, const CheckOptions*)` (`check.h`) runs after
elaboration when there were no errors (`tern check`; `tern build` and the LSP
should call it too; `CheckOptions.cdc_report` = 0, the zero default, prints
nothing, which the LSP needs since stdout carries JSON-RPC). It fills `IrDesign.crossings` (the emitter prints them in the
constraint files' CDC report comment) and returns the error count.

* `check.c` builds a model per module (`check_impl.h`): *nodes* are the units
  that drive signals — one per continuous assign, one per (comb or `on`
  block, target signal), one per instance output, plus read-only nodes for
  instance inputs, assertions and properties. Each node has the bit ranges it
  drives and the signal bits it reads (conditions included). Drivers are
  sorted per signal, so lookups are binary searches; the combinational edges
  (driver node → reader node, by overlapping bits) give strongly connected
  components in topological order (iterative Tarjan). Modules are processed
  children first, and each records a *port summary* (which inputs every
  output depends on combinationally) that its parents use for instance
  outputs. Extern modules have no combinational paths.
* `drivers.c`: two nodes driving overlapping bits ("multiple drivers", both
  sites, bit-accurate so element-wise array assigns are fine; memories are
  exempt: multi-port RAMs), and outputs/wires/vars/registers with undriven
  bits (#[test_port] exempt).
* `assign.c`: definite assignment over each comb block with one bitset per
  block (every bit of every target on every path; a `case` without default
  whose values do not cover the subject has an empty path); the message names
  one failing path (`` `alu_out` may be unassigned when op == Nop ``). Reads
  of bits the block drives before they are assigned are errors. SCCs of size
  > 1 (or self-reading assigns) are combinational loops, reported with the
  cycle (`combinational loop: a -> b -> a`). The elaborator lowers the last
  arm of an exhaustive `match` to the final `else`.
* `cdc.c`: a domain is identified by its clock signal (through `let`
  aliases). Signals with `IrSignal.domain` (or instance wires whose child
  port has a domain) are fixed; others take the domain of the first domain
  they read, in topological order. Reads of another domain in an `on` block,
  in logic driving a fixed-domain signal, or mixed with another domain are
  errors (`pix-domain signal `line` used in sys-domain logic; cross with
  std::cdc`), except inside `#[cdc]` modules; `unsafe_cross` wires are
  domain-free. A domain (or an instance's reset port) whose reset comes from
  another domain warns. Crossings (every `#[cdc]` instance and
  `unsafe_cross`) are expanded along the instance paths from the roots.
* `unused.c` (`--unused`): unread inputs (a bundle port counts as read when
  any field is), wires, vars, registers (self-feeding only counts as
  unread), memories, and instance outputs (one warning per instance).
  Aggregated over all elaborations of a declaration; generic modules
  elaborated only as default roots are skipped; `_`-prefixed names,
  #[test_port], #[keep] and non-user files (`CheckOptions.is_user_file`) are
  exempt.

Diagnostics go through `chk_error`/`chk_warning`, which drop duplicates (the
same message at the same span from monomorphized copies of a module).

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

## Language server (`src/lsp/`, spec §13.2)

`tern lsp` (docs/editors.md for clients and features) is three files:
`rpc.c` (Content-Length framing over an `LspIo` byte stream: stdin/stdout via
`os_read_stdin`/`os_stdin_wait` in binary mode, or memory in the tests),
`lsp.c` (lifecycle, documents, analyses, diagnostics, formatting, dispatch)
and `lsp_nav.c` (definition, hover, references, symbols, completion,
`tern/instanceTree`). `lsp_run(io, opts)` is the entry point.

* Documents are overlays: `DriverOptions.overlay` hands the driver the
  editor buffer instead of the file on disk, `DriverOptions.root` says where
  to look for tern.toml. Each open document belongs to an analysis group: the
  project (files under its `src/` or `tests/`; `include_tests` is on) or the
  file alone (`tern check file.tern`, in its project's context if any).
* An analysis is exactly `tern check`: `driver_open` + `driver_elaborate_all`
  + `check_design` (elaboration only without syntax errors), with
  diagnostics captured through `diag_set_sink`. After a change the groups
  that read the file are marked dirty and re-analysed once no input arrived
  for `--debounce` ms (requests analyse at once). Each analysis owns its
  `Driver` and IR arena and frees the previous one; the driver registers
  each path once (`src_find` + `src_update`), so the source registry stays
  bounded across edits (tested).
* Diagnostics are published per file (including files that are not open,
  and tern.toml), with an empty list for files that no longer have any.
  Positions are UTF-16 code units, converted from byte offsets with the
  `utf8_*` helpers.
* Navigation uses the elaborator's `SemInfo` side table (`sema_info_at`,
  which also records `x.field` / `inst.port` uses and hardware widths) and
  falls back to the AST (`lsp_collect_decls`) plus `sema_lookup` for code
  that was not elaborated, item names and `use` paths.
* Never crash on input: malformed JSON or headers get JSON-RPC errors,
  unknown documents get empty answers, and nothing but protocol messages is
  written to stdout (`--log FILE` for tracing).

## Test interpreter, `tern test`, `tern sim` (`src/sim/testrun.[ch] cmd_test.c cmd_sim.c`)

* `testrun_run(dr, test, opts, cov, report)` runs one `test` body in the
  const evaluator with `ElabHooks`: `sim M<..>(inputs)` re-binds the module in
  a fresh `Elab` + `IrDesign` (kept alive with the handle for its `Ty`→`IrType`
  cache, inverted to read struct/enum signals back as typed values) and
  returns a `VAL_OPAQUE` handle (`TrNode`: root, bundle/instance path, clock
  domain, memory). Reads settle combinational logic first; writes go through
  `sim_poke` (top inputs and `#[test_port]` only — internal signals, outputs
  and the harness-owned clocks/resets are errors with a fix). A bundle can be
  assigned a literal or another handle's bundle (`dec.in = enc.out`); bundle
  methods (`dut.out.fire()`) run on the bundle value read from the handle.
  Methods: `reset()`, `step(n)`, `step_until(cond, max: n)` (the condition is
  re-evaluated each cycle through `Elab.opq_call`), `settle()`,
  `run(ns: n)`, `dump(file)`, `cycle()`, `time_ns()`; `dut.<domain or clock
  port>.step(n)` steps one domain.
* `expect`/`assert` failures print `file:line:col: expect failed at cycle N:
  <expr>` plus `operand = value` lines (operands are re-evaluated quietly;
  impure calls are skipped). `assert`, a failing design assertion or assert
  property, and a `step_until` timeout stop the test (`el->limit_hit`);
  `expect`, assumption violations and still-pending `eventually` obligations
  (at the end) are recorded and the test continues. Elaboration/evaluation
  errors are captured with a diag sink into the test's report.
* `rand<T>()`: per-test splitmix64 seeded from `--seed` (default 1) and the
  test name; integers are half uniform, half biased (random bit length, edge
  values) so `where` rejection sampling converges. `.Variant` without a known
  enum type is a deferred value (`VAL_F_VARIANT`) typed by `elab_convert`.
* `tern test` runs tests in worker processes (`tern test --worker i,j,..`,
  chunks of the test list, results printed in source order, crashed workers
  reported and the rest of their chunk retried); fewer than 4 tests run in
  process. `--cover` merges `sim_coverage` over designs, tests and workers
  (`testcov_*`). The report format is fixed by the goldens in
  `tests/fixtures/test/` (a fixture's first line `// args: --cover` adds
  arguments). Timing is printed only on a terminal.
* `tern sim` is a session (`testrun_session_*`) with the design bound to
  `dut`; command lines are rewritten to Tern statements and parsed as a test
  body.
* `std::bus::RegisterFile<Map, Bus>` is generated in `src/elab/regfile.c`:
  Tern statements (ports per regmap entry, front-end instance, decode) are
  appended to the std module's body before it is elaborated. A regmap used
  as a type is its `Map::Regs` bundle (`Ty.def` = the regmap item), and
  `Map::<Name>` works on it.

## Testing the compiler

`make test` builds and runs `build/test_base` (unit tests for `src/base` and
`src/os`, `tests/unit/test_base.c`), `build/test_pkg` (`src/pkg`, including an
end-to-end run of the package commands against local bare git repositories,
skipped without git), `build/test_tools` (`tern import` goldens in
`tests/fixtures/import/<stem>.expected.tern|.log`, `tern remap`),
`build/test_fmt` (`tern fmt` is faithful and idempotent on every std file and
fixture; `tern doc` text goldens in `tests/fixtures/doc/` and an HTML smoke
test over std), `build/test_backend` (see "Back ends"), `build/test_lsp` (the language server in-process over a
memory stream, plus the scripted sessions `tests/lsp/*.session`) and
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
* `foo.tern` + `foo.ir` — expected elaborated IR: `tern ir foo.tern` (stdout)
* `foo.tern` + `foo.unused` — expected `--unused` warnings:
  `tern check --unused --diag-format=short foo.tern` (stderr)
* `foo.tern` + `foo.cdc` — expected CDC report:
  `tern check --cdc-report --diag-format=short foo.tern` (stdout)

`tests/fixtures/spec/` holds every code example of the spec, completed where
the spec elides parts, as `.ir` goldens (the designs) and `.err` goldens
(`spec/errors/`, the diagnostics the spec quotes); `spec/cfg/` is a project
with `[features]` for `cfg::name`. `build/test_elab` (`tests/unit/test_elab.c`)
elaborates the designs in `tests/fixtures/elab/` with `tern ir --json`, reads
them back with `ir_from_json` and drives them in the simulator: register-file
bus transactions on AXI4-Lite, APB and Wishbone, out-of-range indices,
indices into arrays of bundles, `return` in branches, and hover output for
code that was never elaborated.

So these commands must accept a single `.tern` file outside a project, and
`build` must support `--stdout` (all emitted modules to stdout). Output and
expectation are compared after normalizing line endings, trailing whitespace
and trailing blank lines; the exit status is ignored unless the process
crashed. A first line starting with `// skip` marks a fixture as skipped.
Usage: `tests/run [-v] [-j N] [--tern path] [--no-std] [<filter>...]`.
`UPDATE=1 make test` rewrites mismatching expectations.

`make test` also checks that std is canonically formatted
(`tern fmt --check std`, docs/style.md) and runs `tern test --diag-format=short std` from the repository
root and requires exit status 0 (`make test-std` runs only that). A fixture
whose first line is `// args: <a> <b>` gets those arguments before its file
name (`// args: --cover`).

## Back ends (`src/backend/`, `backend.h`)

**Targets** (`target.c`). `bk_target_resolve` turns `[target.<name>]` of
tern.toml into a `BkTarget`: `device` picks the family (`xc*` Xilinx,
`5C*`/`10M*`/`EP4C*` Intel, `ice40`/`up5k`/`hx8k` iCE40, `LFE5U*` ECP5,
`GW1N*` Gowin, `sky130`/`gf180` ASIC), which sets the defaults for the
synthesis tool, the constraint format (`.xdc` for Xilinx, `.sdc` otherwise,
both for unknown devices) and memory-init formats (`.mif` for Intel). Extra
keys override them: `vendor`, `kind = "fpga"|"asic"`, `constraints`,
`mem_init`, `features` (applied before `--features`, via
`driver_apply_features`), `clock_hz` (synthesis timing goal; default: the
top's `#[clock(hz)]`), `package` (nextpnr), `part` (full Vivado part),
`family` (Quartus FAMILY). `backend = "yosys"|"vivado"|"quartus"` is the
synthesis tool.

**`tern build`** (`build.c`). `bk_build_run`: driver -> the tops (`--top`,
else the target's `top`, else every `#[top]` of the user files, else the
first module without required generics), each elaborated into its own
design -> `check_design` (errors fail the build) -> `emit_design` with the
target's options -> one merged `SvOutput`. Absolute source paths in line
markers are rewritten to project-relative ones (`bk_rewrite_paths`; the
development `std/` directory becomes `std/...`), so builds are
reproducible across checkouts. Extern module sources are found relative to
the project root (or the declaring file) and copied into the output.
`bk_build_write` writes `build/<target>/` (or `build/` without a project;
`-o` overrides) with `os_write_file_if_changed` and prints a summary;
`--stdout` prints the `// ---- file: X ----` concatenation (the `.sv`
golden fixtures in `tests/fixtures/build/` use it); `--lint` runs
`verilator --lint-only` (`verilator.c`, the waivers of
`tests/lint_emitted.sh`).

**`tern prove`** (`formal.c`, `prove.c`). `fv_generate` restricts the
design to the module and its descendants and emits it as Verilog-2005
(what open-source Yosys reads reliably), then appends a checker block under
`` `ifdef FORMAL `` before each module's `endmodule`; the checks refer to
the module's own signals through `SvNames`. Every IR node of a checked
expression becomes a `wire` of its exact IR width, so no Verilog width rule
can change a value. Immediate assertions (`assert`/`assume`/`cover`/
`expect`) become `always @* if (<not in reset> && <path>) assert (c);`
where the path condition is the conjunction of the `if`/`case` branches
leading to the statement (comb-block assertions that read a variable the
same block assigns earlier are not translated). Properties are compiled to
monitors with the simulator's semantics (sim.h): end-match signals for
antecedent sequences (`##n`, `##[a:b]`, `##[a:$]` via history and sticky
registers), obligation vectors for ranged consequents and bounded
`eventually`, a pending flag for `until`, a first-cycle flag for properties
without `always`; monitor registers clear in reset. Unbounded obligations
are liveness and are reported as not translated. The wrapper
`tern_formal_top` instantiates the module with free inputs and assumes each
top reset for the first two cycles of its domain and released afterwards,
which is exactly what `dut.reset()` does in a test. `<Mod>.sby` has tasks
`bmc`, `prove` (`--induct`) and `cover`; `multiclock on` with several top
clocks. `prove.c` writes `build/formal/<Mod>/`, runs `sby -f` when it is on
PATH (otherwise it says what to install and exits 2), and
`fv_parse_sby_log` maps `Assert failed in ...: <file>:<line>`, `Reached /
Unreached cover statement at ...` and `Writing trace to VCD file` back to
the checks by generated file and line. For the first failure,
`vcdr_parse` (`vcdread.c`, a minimal VCD reader) reads the trace and
`fv_cex_to_test` writes a Tern test: `sim M<..>()`, `dut.reset()` for the
reset cycles, then the changed top inputs (clocks and resets excluded) and
`dut.step()` per clock edge up to the failing step; the simulator reports
the failing check during the last step. It goes to
`tests/formal_<module>_<check>.tern` (with the package of the existing
test files and a `use` of the module), or to stdout with `--print-test`.

**`tern synth`** (`synth.c`, `cmd_synth.c`). Runs the build, then writes
`build/<target>/synth/`: for Yosys a Verilog-2005 emission of the same
designs plus `synth.ys` (`synth_ice40` / `synth_ecp5` / `synth_xilinx` /
`synth_gowin` / `synth_intel_alm` / `synth`, then `tee -o utilization.txt
stat`; nextpnr-ice40/-ecp5 for Fmax when installed); for Vivado
`synth.tcl` (non-project mode: `read_verilog -sv`, `read_xdc`,
`synth_design`, `opt_design`, `place_design`, `route_design`,
`report_utilization`, `report_timing_summary`); for Quartus
`synth_quartus.tcl` (`project_new`, sources, SDC, `execute_flow
-compile`). A tool on PATH is run and its reports are parsed
(`syn_parse_yosys_stat` for both `stat` formats, `syn_parse_nextpnr`,
`syn_parse_vivado_util/timing`, `syn_parse_quartus_fit/sta`) into
LUTs/FFs/BRAM/DSP and Fmax/WNS against the target clock (exit 1 when the
goal is missed). Without the tool the script is written and the command
line to run it is printed (exit 0).

`build/test_backend` (`tests/unit/test_backend.c`) tests target
resolution, path rewriting, the formal generator (and compiles its output
with Icarus Verilog when installed), the sby log parser on
`tests/fixtures/formal/sby_bmc_fail.log`, the VCD reader and the
counterexample test on `tests/fixtures/formal/counter_trace.vcd`, the
synthesis scripts, the report parsers on `tests/fixtures/synth/`, and
compiles every `tests/fixtures/build/*.sv` golden with Icarus Verilog. No
test needs Yosys, SymbiYosys, Vivado or Quartus.

## Conventions

* Commit early and often with descriptive messages; never commit a tree
  that fails `make` or `make test` (golden tests for not-yet-implemented
  features are marked by a `// skip` first line).
* Keep `src/unity.c`, `Makefile` and `build.bat` in sync when adding files
  (`build.bat` only builds `src/unity.c`; the Makefile needs the file in
  `SRCS`/`OBJS`, any new header in `HDRS`, and an object rule).
* `src/std_embed.c` is generated from `std/` by `tools/embed` (run by every
  `make`; it rewrites the file only when something changed) and committed.
* Every command-line command is implemented as `int cmd_<name>(int argc, char **argv)`
  declared in `src/cmds.h`.
