# Migrating from SystemVerilog: `tern import` and `tern remap`

The migration path (spec §13.5):

1. `tern new`, then `tern import` the existing tree: every SystemVerilog
   module becomes an `extern module`, and the build works on day one.
2. Rewrite leaf modules in Tern one at a time, each with a `test`; the emitted
   SystemVerilog replaces the original in the same flow.
3. Move interfaces to bundles as the modules on both sides are converted.
4. Delete `extern` declarations as they empty out.

## `tern import`

```
tern import rtl/uart_tx.sv                     # to stdout
tern import rtl/uart_tx.sv -o src/uart.tern --package uart
tern import rtl/                               # every .sv/.v below rtl/
tern import rtl/ -o src/legacy --package legacy
```

* One file: output to `-o <file>` (default stdout); the package is
  `--package` or the output/input file name.
* A directory: every `*.sv` / `*.v` below it (hidden directories and the output
  directory are skipped) becomes one `.tern` file in the output directory
  (`-o`, default `src/imported` in a project, else `imported/`), mirroring the
  sub-directories. Files of one output directory share a package:
  `--package` (default `imported`) plus the sub-directory names
  (`imported::periph`). Files without modules are skipped.
* The path in `extern module name("path")` is relative to the project root
  (the directory with `tern.toml`), or to `--root <dir>`.

It is a header parser, not a full SystemVerilog front end. Comments and
attributes `(* ... *)` are skipped; compiler directives are skipped with a
warning (`` `timescale ``, `` `define `` — macro *uses* are kept with a TODO,
`` `include `` is not followed, and `` `ifdef `` branches are not evaluated:
declarations of every branch are read, a repeated port keeps its first
declaration). Interfaces, packages, classes and UDPs are skipped (interfaces
with a warning). Module bodies are only scanned for non-ANSI port declarations
(and the net declarations that refine them, `output q; reg [3:0] q;`) and
Verilog-2001 body `parameter`s; functions, tasks, clocking blocks and similar
are skipped so their `input` arguments are not mistaken for ports.

### Mapping

| SystemVerilog | Tern |
|---|---|
| `parameter int W = 8`, untyped, `integer`, `int unsigned` | `W: uint = 8` (`sint` when `signed` or the default is negative) |
| `parameter logic [7:0] INIT = 8'hA5`, `parameter bit EN = 1'b1` | `INIT: uint<8> = 0xA5`, `EN: bool = true` |
| `localparam` | not a generic; uses in widths are substituted by its value |
| `parameter type`, `string`, `real` | `// TODO` comment above the declaration (keeps its SV default) |
| 1-bit `logic`/`wire`/`reg`/`bit` port | `bool` |
| `[7:0]`, `[15:8]` | `uint<8>` |
| `[W-1:0]`, `[W:0]`, `[$clog2(D)-1:0]` | `uint<W>`, `uint<W + 1>`, `uint<clog2(D)>` |
| `[0:7]` (ascending) | `uint<8>` with a comment |
| `signed [W-1:0]` | `sint<W>` |
| `[3:0][7:0]` (packed array) | `[uint<8>; 4]` |
| `int`, `byte`, `shortint`, `longint`, `int unsigned` | `sint<32>`, `sint<8>`, `sint<16>`, `sint<64>`, `uint<32>` |
| `clk`, `clock`, `*_clk`, `aclk`, `clk_i`, ... (1 bit) | `clock` `// TODO: confirm` (not `clk_en`, `clk_sel`, ...) |
| `rst`, `reset`, `*_rst` (1 bit) | `reset` `// TODO: confirm` |
| `rst_n`, `rstn`, `*_n`, `*_b`, `nrst` resets | `reset<sync, low>` `// TODO: confirm` |
| `arst*`, `areset*` resets | `reset<async, high/low>` `// TODO: confirm` |
| `inout` | `in x: tri<T>` with a comment: only a `#[top]` module may have `tri<>` ports |
| interface port, unpacked array port, user-defined / package type, `ref` | commented-out port line with a `// TODO` |
| port or module named like a Tern keyword (`on`, `type`, ...), escaped identifiers | renamed (`on_`) with `#[sv_name("on")]` |
| sized literals `8'hFF`, `'d10`, `4'b1010`, `'0` | `0xFF`, `10`, `0b1010`, `0` |
| `&&`, `\|\|`, `$clog2` | `and`, `or`, `clog2` |
| `?:`, `**`, macros, system functions, x/z digits | kept, with `// TODO: check ...` |

Example (spec §2):

```systemverilog
module uart_tx #(parameter int CLK_HZ = 50_000_000, parameter int BAUD = 115_200) (
    input logic clk, input logic rst,
    input logic tx_valid, input logic [7:0] tx_data,
    output logic tx_ready, output logic txd);
```

becomes

```tern
/// `uart_tx` from rtl/uart_tx.sv:1.
extern module uart_tx("rtl/uart_tx.sv") <CLK_HZ: uint = 50_000_000, BAUD: uint = 115_200> {
    in clk: clock // TODO: confirm: clock (from the name)
    in rst: reset // TODO: confirm: reset (active-high from the name; synchronous assumed)
    in tx_valid: bool
    in tx_data: uint<8>
    out tx_ready: bool
    out txd: bool
}
```

The engineer then adds what SystemVerilog cannot express: confirm the clock and
reset types, group `tx_valid`/`tx_data`/`tx_ready` into a `Stream<uint<8>>`
bundle port (mapped by convention, `#[sv_name]` to override), and replace
user-defined types with Tern structs/enums.

Golden examples live in `tests/fixtures/import/` (`<stem>.sv` → `<stem>.expected.tern`
plus the warnings in `<stem>.expected.log`); `make test` checks them and
`UPDATE=1 make test` rewrites them.

## `tern remap`

The emitted SystemVerilog carries a line marker before each construct:

```systemverilog
// tern: src/uart.tern:42
always_ff @(posedge clk) begin
```

(`path:line`, optionally `path:line:col`; the marker may be indented or follow
code on the same line). `tern remap` reads every `.sv`, `.v` and `.svh` file
below the build directory, maps each SystemVerilog line to the nearest
preceding marker, and rewrites the tool log:

```
tern remap < vivado.log                 # build dir: build/
tern remap build/arty synth.log -o synth.tern.log
verilator --lint-only ... 2>&1 | tern remap
```

```
%Warning-WIDTH: build/arty/Uart.sv:57:9: ...   ->  %Warning-WIDTH: src/uart.tern:42 (build/arty/Uart.sv:57:9): ...
```

Recognized locations: `file.sv:N` and `file.sv:N:C` (Verilator, Yosys, Vivado
`[file.sv:N]`), `file.sv(N)` and `file.sv(N,C)` (Questa), `file.sv, line N`,
`"file.sv", N` (VCS), `file.sv line N`, `file.sv Line: N` (Quartus). A log path
matches a file by its absolute path, by the longest path suffix relative to the
build directory, or by a unique base name. Locations before the first marker
of a file, and files that are not in the build directory, are left unchanged.
