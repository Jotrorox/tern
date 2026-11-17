# 9. The toolchain

One binary, `tern`, does everything from project creation to formal proof.
Its only required output is readable SystemVerilog, so Tern is usable without
vendor support (spec chapter 13).

## Pipeline

Every command runs the same front half: parse, resolve names, elaborate
(generics, `const fn`, unrolling) and check (types, widths, drivers, latches,
clock domains) into a typed intermediate representation (IR). The SystemVerilog
emitter, the simulator, formal verification and the language server all work
from that IR, so they cannot disagree about a design. `tern ir` prints it, and
its JSON form is documented in [ir-schema.md](../ir-schema.md).

## Commands

| Command | Does |
|---|---|
| `tern new <name> [--no-git]` | project skeleton with `tern.toml`, `src/`, `tests/` |
| `tern check [files] [--unused] [--cdc-report]` | every compile-time check without writing files; CDC report; unused signals |
| `tern build [--target t] [--top m] [-o dir \| --stdout]` | elaborate, check and write `build/<target>/`: `.sv` files, constraint fragments, memory init files, regmap headers |
| `tern test [paths] [filter] [--waves] [--seed n] [--cover] [-j n] [--list]` | run `test` blocks on the built-in simulator |
| `tern sim <top> [--vcd f] [script]` | interactive stimulus REPL, or a stimulus script |
| `tern prove <module> [--depth n] [--induct]` | formal verification via Yosys and SymbiYosys |
| `tern synth [--target t] [--tool yosys\|vivado\|quartus]` | run synthesis with a generated script; utilization and timing summary |
| `tern fmt [--check] [files]` | canonical formatting, no options |
| `tern doc [pkg\|item] [--text \| -o dir] [--open]` | documentation from `///` comments, as HTML or terminal text |
| `tern lsp` | language server on stdio |
| `tern add <pkg>[@ver] \| <git-url>` | add a dependency, write `tern.lock` |
| `tern update [pkg]` / `tern vendor` | move to newer compatible tags / copy dependencies into `vendor/` |
| `tern publish [--dry-run]` | push the version tag and prepare the index entry |
| `tern import <file.sv \| dir>` | generate `extern module` declarations from SystemVerilog |
| `tern remap [build-dir] [log]` | translate tool logs back to Tern source locations |

`tern help <command>` prints every option of a command.

Options accepted by every command:

| Option | Effect |
|---|---|
| `--diag-format=human\|short\|json` | diagnostics with source excerpts (default), one line each, or JSON |
| `--color=auto\|always\|never` | coloured diagnostics |
| `--strict` | warnings become errors |
| `--features a,-b` | turn features on or off (commands that elaborate) |

Exit status: 0 on success, 1 when the input has errors or tests fail, 2 for a
bad command line or a missing external tool.

## `tern.toml`

```toml
[package]
name = "my-soc"
version = "0.3.0"
description = "A small SoC"
license = "MIT"

[dependencies]
axi = "1.2"                                                     # through the registry index
uart-ip = { git = "https://github.com/acme/tern-uart", tag = "v2.0.1" }
vendor-ip = { path = "../vendor-ip" }

[features]
debug_ila = false                  # read as cfg::debug_ila

[target.arty]
device = "xc7a35t"
top = "Top"
clock_hz = 100_000_000

[target.icebreaker]
device = "up5k"
top = "Top"
std = "verilog2005"
features = "debug_ila"
```

`tern build` compiles every package under `src/`, and `tern test` also
compiles `tests/`. [packages.md](../packages.md) is the complete reference for
the manifest, the lock file, version requirements and dependency resolution.

## Targets

A `[target.<name>]` section describes one build:

| Key | Meaning |
|---|---|
| `top` | the top module (default: every `#[top]`) |
| `device` | the part; its prefix picks the family (`xc...` Xilinx, `up5k`/`ice40...` iCE40, `lfe5...` ECP5, `gw...` Gowin, `5c...`/`10m...` Intel, `sky130`/`gf180` ASIC) |
| `vendor` | the family explicitly: `xilinx`, `intel`, `lattice`, `ice40`, `ecp5`, `gowin`, `asic`, `generic` |
| `kind` | `fpga` or `asic` (power-on values are `initial` blocks on FPGAs and a warning on ASICs) |
| `backend` | synthesis tool for `tern synth`: `yosys`, `vivado`, `quartus` |
| `std` | `sv` (SystemVerilog 2012, default) or `verilog2005` |
| `constraints` | `xdc`, `sdc` or `none` (default by family) |
| `mem_init` | memory initialisation files: `hex`, `mif`, `coe` |
| `clock_hz` | the main clock frequency, for the timing summary |
| `features` | features enabled for this target |

`tern build --target arty` builds one target. Without `--target`, the only
target (or `default`) is built. Outside a project, `tern build file.tern`
writes to `build/`.

## What `tern build` writes

* One `.sv` file per module, with names preserved. Generic modules are
  monomorphized (`Fifo<u8, 64>` becomes `Fifo_u8_64`). A `_types_pkg.sv`
  holds the packed structs and enums.
* Idiomatic SystemVerilog: `always_ff`/`always_comb`, `logic`, packed
  structs and enums, `unique case`. No tool-specific constructs.
  `--std verilog2005` (or `std = "verilog2005"` in the target) writes plain
  Verilog-2005.
* `// tern: file.tern:42` line markers before each construct.
  `--no-line-markers` leaves them out.
* Constraint fragments (`.xdc`, `.sdc`) for clock-domain crossings, and
  `ASYNC_REG` attributes on synchronizer flops.
* Memory initialisation files (`$readmemh` data, or `.mif`/`.coe`).
* For each `regmap`: `<Map>.h` (C header), `<Map>.svd` (CMSIS-SVD) and
  `<Map>.md`.
* Extern modules' SystemVerilog files, copied next to the output.

Only changed files are rewritten, so downstream tools see stable timestamps.
`.tern-build` in the output directory records what the build wrote; the
next build deletes the files from that list it no longer generates (a
renamed module's old `.sv`), and leaves every other file alone.
`tern build --lint` runs `verilator --lint-only` on the result when Verilator
is installed. [emitted-sv.md](../emitted-sv.md) describes the output in
detail.

## Synthesis

`tern synth` writes a synthesis script for the target's tool (a Yosys `.ys`
script for open-source flows, a Tcl script for Vivado or Quartus), runs the
tool, and prints a utilization and timing summary. The tool must be installed
and on your `PATH`. `--no-run` only writes the script to
`build/<target>/synth/`, for running it yourself.

```sh
tern synth                       # the default target, tool by device family
tern synth --target arty         # Vivado for an xc7 part
tern synth --tool yosys          # force Yosys
tern synth --no-run              # only write the script
```

## Logs back to source

Synthesis and simulator messages refer to the emitted `.sv` files.
`tern remap` translates them back to Tern locations, using the line markers:

```sh
vivado -mode batch -source build/arty/synth/synth.tcl | tern remap build/arty
verilator --lint-only build/default/*.sv 2>&1 | tern remap
```

```
%Warning-WIDTH: build/arty/Uart.sv:57:9: ...   ->   %Warning-WIDTH: src/uart.tern:42 (build/arty/Uart.sv:57:9): ...
```

## Formatting and documentation

* `tern fmt` formats the project's sources. It has no options, so every Tern
  project looks the same. `tern fmt --check` lists files that would change and
  exits 1, which suits CI. The rules are in [style.md](../style.md).
* `tern doc` builds a static HTML site from `///` comments, with port tables,
  generics, constants, properties and the names of the tests. `tern doc
  std::fifo::Fifo --text` prints the same information in the terminal.

## Packages

A package is a git repository with `tern.toml`, `src/` and `tests/`. Versions
are semver tags (`v1.2.0`). A public index, itself a git repository, maps
short names to git URLs, so there is no upload step and no server holding
code.

```sh
tern add axi                  # newest release, recorded as axi = "1.2.3"
tern add axi@1.2 dma@~0.4     # explicit requirements
tern add https://github.com/acme/tern-uart --tag v2.0.1
tern add --path ../vendor-ip
tern update                   # newer compatible versions
tern vendor                   # copy everything into vendor/ for offline builds
tern publish --dry-run        # check what publishing would do
```

* Only `add`, `update`, `vendor` and `publish` run `git` and touch the
  network. `build`, `check`, `test` and the language server read dependencies
  from `~/.tern/cache/` or `vendor/`, as recorded in `tern.lock`.
* Commit `tern.lock`, because builds are reproducible from it.
* A dependency `uart-ip` is imported as `use uart_ip::...`.
* `std` is versioned with the compiler and is never a dependency.

[packages.md](../packages.md) and [registry.md](../registry.md) cover the
details, including the index format and the publishing flow.

## Editor support

`tern lsp` speaks the Language Server Protocol on stdin/stdout. It provides
diagnostics on every change, go-to-definition (into std too), hover with type,
width, clock domain and driver, find references, document and workspace
symbols, completion, formatting, and a custom `tern/instanceTree` request for
the elaborated hierarchy. [editors.md](../editors.md) has setup instructions
for VS Code, Neovim, Helix and Emacs.

Next: [Migrating from SystemVerilog](10-migrating-from-systemverilog.md).
