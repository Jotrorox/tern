# Changelog

All notable changes to Tern are listed here. Versions follow
[semantic versioning](https://semver.org/). Before 1.0, a minor version may
contain breaking changes.

## 0.1.0 (unreleased)

The first release of the reference implementation. It implements the language
of spec version 0.1 (`docs/tern-spec.pdf`) and every command of spec §13.2.

### Compiler

* Hand-written lexer and recursive-descent/Pratt parser for the full grammar
  of spec appendix A, with dedicated errors for SystemVerilog words (`wire`,
  `logic`, `initial`, `generate`) and reserved keywords.
* Name resolution across packages and files: `use` with single names, lists
  and `*`, `pub` visibility, the auto-imported `std::prelude`, and
  re-exports.
* Types and width rules: `bool`, `bit<N>`, `uint<N>`, `sint<N>`, widthless
  compile-time integers, `clock`/`reset` types with style parameters, arrays,
  packed structs, enums with payloads and `#[encoding(...)]`, bundles with
  `flip`, `tri<T>` on top-level ports. Lossless arithmetic with wrapping
  `+%`/`-%`, explicit conversions (`as`, `bits`, `from_bits`), and errors for
  silent truncation and signed/unsigned mixing.
* Elaboration: type and value generics with defaults and `where` clauses,
  generic arguments inferred from connections, a step-bounded `const fn`
  interpreter with arbitrary-precision integers, elaboration-time `for`/`if`,
  monomorphization, `static_assert`, `const_log`, `typeof`, `T::WIDTH`,
  `T::MAX`/`T::MIN`, module-level `pub const` read as `Module<..>::NAME`, and
  feature flags (`cfg::name`).
* Design checks: multiple and missing drivers, the full-assignment proof for
  `comb` blocks (no latches, with the failing path in the message),
  exhaustive `match`, combinational loops, clock-domain typing with a CDC
  report, unconnected inputs, and unused signals (`--unused`).
* Memories with counted read and write ports, synchronous and combinational
  reads, byte-enable writes, explicit read-during-write behaviour, and
  initial contents from literals, `const fn` or `include_hex`/`include_bin`.
* Register maps (`regmap`) with `rw`, `ro`, `wo`, `w1c`, `rw1s` and stream
  registers, and a compiler-generated `std::bus::RegisterFile` with AXI4-Lite,
  APB and Wishbone front-ends.
* Diagnostics with source excerpts, the elaboration path and suggested fixes,
  in human, short and JSON formats. `--strict` turns warnings into errors.

### Back ends

* SystemVerilog 2012 and Verilog-2005 emitter: one file per module,
  monomorphized names, packed structs and enums in a types package,
  `always_ff`/`always_comb`, `// tern: file:line` markers, optional
  assertions (`--emit-assertions`), `ASYNC_REG` attributes, and `.xdc`/`.sdc`
  constraint fragments for crossings.
* Regmap outputs: C header, CMSIS-SVD and Markdown.
* `tern build` with targets from `tern.toml` (device family detection,
  constraint and memory-init formats, features per target), extern-file
  copies, change-only writes and optional Verilator lint.
* `tern prove`: Verilog-2005 plus generated formal checkers and a SymbiYosys
  project. It runs BMC or k-induction and turns counterexamples into Tern
  tests. Needs Yosys and SymbiYosys.
* `tern synth`: Yosys, Vivado and Quartus scripts by device family, tool runs,
  and utilization and timing report parsing. Needs the respective tool.
* The IR as JSON (schema version 1, `docs/ir-schema.md`).

### Simulation and verification

* Built-in cycle-based, 2-state simulator (an interpreter over a flattened
  netlist), with VCD output.
* `test` blocks: `sim` handles, `reset`/`step`/`step_until`/`settle`/`run`,
  per-domain stepping, `expect` and `assert` with operand values, `log`,
  `rand<T>()` with rejection constraints and per-test seeds, table-driven
  loops, and pseudo-random power-on values for reset-less registers.
* Immediate assertions and the temporal property subset of spec §11.4
  (`always`, `never`, `|->`, `|=>`, `##n`, `##[a:b]`, `eventually`, `stable`,
  `rose`, `fell`, `past`, `until`), plus `assume property` and
  `cover property`.
* `tern test`: parallel worker processes, name filters, `--seed`,
  `--waves`, `--cover` (line, branch and cover-property coverage) and
  `--list`.
* `tern sim`: an interactive REPL and a script runner.

### Tools

* `tern new`, `tern check`, `tern fmt` (canonical formatter, `--check`),
  `tern doc` (static HTML and terminal reference from `///` comments).
* `tern lsp`: diagnostics, go-to-definition, hover with type, width, domain
  and driver, references, document and workspace symbols, completion,
  formatting, and an instance-tree request. A VS Code extension is in
  `editors/vscode/`, and `docs/editors.md` has setups for Neovim, Helix and
  Emacs.
* Packages: `tern.toml` and `tern.lock`, semver requirements, transitive
  resolution, a git-based registry index, and `tern add`, `tern update`,
  `tern vendor`, `tern publish`.
* Interop: `tern import` (SystemVerilog headers to `extern module`
  declarations) and `tern remap` (tool logs back to Tern locations).

### Standard library

* Written in Tern and embedded in the binary: `prelude`, `stream`, `fifo`,
  `cdc`, `math`, `mem`, `crc`, `encoding`, `io`, `bus`, `dsp`, `clk`,
  `timer`, `video`, `debug`, `sim` and `target` (Xilinx, Lattice, Intel and
  generic fallbacks). 363 tests run with `tern test std`.

### Build and quality

* Portable C11/C99 with no dependencies: `make`, `build.bat` (MSVC) and the
  one-command `cc -O2 -o tern src/unity.c`.
* Test suite in C: unit tests, golden-file tests, an Icarus Verilog
  cross-check of the emitted SystemVerilog, a libFuzzer parser harness, and a
  check that every code block in the user documentation compiles.
* CI on Linux (GCC, Clang, C99, C11, `-Werror`), macOS, Windows (MSVC) and
  TCC, with sanitizers, Verilator lint and Yosys synthesis of emitted code.
* Documentation: README, a ten-chapter user guide, reference documents and a
  contributing guide.

### Known limitations

* The simulator is an interpreter. The compile-to-C mode of spec §13.7 is not
  implemented.
* `tern test --backend verilator` (spec §13.4) is not available, so tests of
  designs with extern modules cannot run.
* The `tern check` warning for reset-less registers that are read before
  any write (spec §6.5) is not implemented. The simulator's per-seed
  randomization of power-on values is.
* `#[test_port]` (spec §11.1) is accepted, but tests cannot yet drive a
  signal marked with it.
* `Arbiter`'s `.Weighted` policy and a few other std details differ from the
  spec's tables. See `std/README.md` and `std/README-part2.md`.
