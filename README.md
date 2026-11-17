# Tern

[![CI](https://github.com/Jotrorox/tern/actions/workflows/ci.yml/badge.svg)](https://github.com/Jotrorox/tern/actions/workflows/ci.yml)

Tern is a synthesis-first hardware description language for synchronous
digital hardware. Everything that compiles is hardware, every signal is typed
with a width and a clock domain, tests live beside the design, and the
compiler emits clean SystemVerilog, so Tern drops into any existing flow.

This repository is the reference implementation: one portable C11 program,
`tern`, that also compiles as C99 and has no third-party dependencies. It
contains the compiler, the simulator, the test runner, the formatter, the
language server, the package manager and a standard library written in Tern.

The language is specified in [docs/tern-spec.pdf](docs/tern-spec.pdf)
(version 0.1). The [user guide](docs/guide/README.md) teaches it chapter by
chapter.

## A taste

An 8N1 UART transmitter and its test, from the spec's language tour:

```tern,test
package uart

use std::stream::Stream

/// Sends one byte per `tx` handshake, 8 data bits, no parity, 1 stop bit.
module UartTx<ClkHz: uint, Baud: uint = 115_200> {
    in clk: clock
    in rst: reset
    in tx: Stream<uint<8>> // valid + data in, ready out
    out txd: bool

    const Div: uint = ClkHz / Baud
    static_assert(Div >= 16, "clock too slow for this baud rate")

    enum State { Idle, Start, Data, Stop }
    reg state: State = .Idle
    reg tick: uint<clog2(Div)> = 0
    reg bit: uint<3> = 0
    reg shift: uint<8> = 0

    let baud_tick = tick == Div - 1
    tx.ready = state == .Idle
    txd = match state {
        .Idle | .Stop => true,
        .Start => false,
        .Data => shift[0],
    }

    on clk {
        tick = if baud_tick or state == .Idle { 0 } else { tick +% 1 }
        match state {
            .Idle => if tx.valid { shift = tx.data; state = .Start },
            .Start => if baud_tick { state = .Data },
            .Data => if baud_tick {
                shift = shift >> 1
                bit = bit +% 1
                if bit == 7 { state = .Stop }
            },
            .Stop => if baud_tick { state = .Idle },
        }
    }
}

test "sends 0x55 LSB first" {
    let dut = sim UartTx<ClkHz: 16, Baud: 1>() // Div = 16, clk/rst driven by the harness
    dut.reset()
    dut.tx = Stream { valid: true, data: 0x55 }
    dut.step()
    expect !dut.tx.ready
    let line = [false, true, false, true, false, true, false, true, false, true]
    for b in line {
        dut.step(8) // sample mid-bit
        expect dut.txd == b
        dut.step(8)
    }
}
```

This version cannot contain a latch, a race, a width bug or a missed reset:

* **One assignment operator.** Inside `on clk`, `state = .Start` sets the
  register's next value. At module level, `txd = ...` is a continuous
  assignment.
* **No hand-written reset.** `reg state: State = .Idle` gives the reset
  value. The `reset` port type says whether the reset is synchronous or
  asynchronous, and which polarity.
* **Exhaustive `match`.** Every `State` must be covered, so there is no
  `default` to forget and no latch.
* **Lossless arithmetic.** `tick +% 1` is a wrapping add that keeps the
  width. A plain `tick + 1` is one bit wider, and assigning it back is a
  compile error.
* **Bundles.** `Stream<uint<8>>` carries `valid` and `data` in and `ready`
  out, because the port is `in`.
* **Tests in the same file.** `tern test` runs this test on the built-in
  simulator.

## Features

* **A small language.** Rust/Go-flavoured syntax, 38 keywords, one assignment
  operator, no `begin`/`end`. Three signal kinds: `let` (wire), `var`
  (combinational variable) and `reg` (flip-flop).
* **Correct by construction.** Latches, width surprises, unconnected inputs,
  multiple drivers and implicit clock-domain crossings are compile errors.
  Every error has a source span, the elaboration path, and a suggested fix
  where one exists.
* **Typed clock domains.** A register's domain is part of its type. Crossing
  domains requires a `std::cdc` primitive, and `tern check` prints the CDC
  report.
* **Generics and compile-time code.** Type and value generics with `where`
  constraints, `const fn` with arbitrary-precision integers, and ordinary
  `for`/`if` instead of `generate` and the preprocessor.
* **Bundles** with `flip` instead of interfaces and modports. **Memories**
  with counted ports and explicit read-during-write behaviour. **Register
  maps** that generate the hardware, a C header, CMSIS-SVD and Markdown.
* **Verification built in.** `test` blocks, immediate assertions, a temporal
  property subset (`|->`, `|=>`, `##n`, `eventually`, `stable`, ...), table
  and randomized stimulus, coverage, VCD waveforms, and an interactive
  simulator REPL.
* **Readable output.** One SystemVerilog file per module, with
  `always_ff`/`always_comb`, packed structs and enums, and `// tern: file:line`
  markers, or Verilog-2005. `tern remap` maps tool logs back to Tern lines.
* **Interop.** `extern module` declares SystemVerilog as a typed black box,
  and `tern import` writes those declarations from existing code.
* **Batteries included.** One CLI for build, check, test, sim, formal, synth,
  format, docs, packages and the language server. The standard library has
  FIFOs, CDC, streams, math, CRC, encoders, UART/SPI/I2C, buses, DSP, video,
  debug cores and test helpers.
* **Portable and dependency-free.** Builds with GCC, Clang, MSVC and TCC on
  Linux, macOS and Windows.

## Building

Any C11 (or C99) compiler builds `tern`:

```sh
make -j8                        # POSIX make; per-file objects in build/obj/
make install                    # copies ./tern to /usr/local/bin (PREFIX=... to change)
```

The whole compiler is also one translation unit, so a single command works
too, with no make and no configure step:

```sh
cc -O2 -o tern src/unity.c
```

On Windows, run `build.bat` from a Developer Command Prompt for Visual Studio
2019 16.8 or newer. It writes `tern.exe`, and `build.bat test` also builds
and runs the test suite.

The standard library is embedded in the binary. Nothing else needs to be
installed for `check`, `build`, `test`, `sim`, `fmt`, `doc` and `lsp`. Some
commands drive external tools, which must be on your `PATH`:

| Command | Needs |
|---|---|
| `tern prove` | [Yosys](https://github.com/YosysHQ/yosys), [SymbiYosys](https://github.com/YosysHQ/sby) and an SMT solver (yices by default) |
| `tern synth` | Yosys, Vivado or Quartus, depending on the target |
| `tern build --lint` | Verilator |
| `tern new`, `add`, `update`, `vendor`, `publish` | `git` (and optionally `gh` for `publish --pr`) |

The [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build) bundles
Yosys, SymbiYosys, the solvers and Verilator.

## Quickstart

```sh
tern new blinky                 # tern.toml, src/blinky.tern, tests/blinky_test.tern
cd blinky
tern check                      # all compile-time checks
tern test                       # run the test blocks
tern build                      # SystemVerilog in build/default/
tern sim 'Blinky<Period: 4>'    # poke at the design interactively
```

```
$ tern test
running 2 tests
test led toggles every Period cycles ... ok
test Blinky holds the LED for exactly Period cycles ... ok

ok: 2 passed, 0 failed
```

Every command also works on loose files (`tern check foo.tern`,
`tern test foo.tern`, `tern build --stdout foo.tern`).
[Getting started](docs/guide/01-getting-started.md) walks through it in more
detail.

## Commands

| Command | Does |
|---|---|
| `tern new <name>` | project skeleton with `tern.toml`, `src/`, `tests/` |
| `tern build [--target t]` | elaborate, check, and write `build/<target>/`: `.sv`, constraints, memory init files, regmap headers |
| `tern check` | every compile-time check without writing files; CDC report; unused signals |
| `tern test [filter] [--waves] [--seed n] [--cover]` | run `test` blocks on the built-in simulator, in parallel |
| `tern sim <top> [--vcd f]` | interactive stimulus REPL, or a stimulus script |
| `tern prove <module> [--depth n] [--induct]` | formal verification via Yosys and SymbiYosys |
| `tern synth [--target t]` | drive Yosys, Vivado or Quartus; utilization and timing summary |
| `tern fmt` | canonical formatting, no options |
| `tern doc [pkg]` | HTML or terminal documentation from `///` comments |
| `tern lsp` | language server: diagnostics, go-to-definition, hover with width/domain/driver, instance tree |
| `tern add <pkg>[@ver]` | add a dependency by index name or git URL; writes `tern.lock` |
| `tern update` / `tern vendor` | newer compatible versions / copy dependencies into `vendor/` |
| `tern publish` | push the version tag and prepare the registry index entry |
| `tern import <file.sv>` | generate `extern module` declarations from SystemVerilog |
| `tern remap` | translate synthesis and simulator logs to Tern source locations |

`tern help <command>` lists the options of each command.

## Documentation

* [User guide](docs/guide/README.md): getting started, language basics,
  modules and generics, bundles and clock domains, compile-time evaluation,
  memories/FSMs/register maps, testing and verification, the standard
  library, the toolchain, and migrating from SystemVerilog.
* [Language specification](docs/tern-spec.pdf), the normative definition.
* [Documentation index](docs/index.md), including the reference documents:
  grammar, emitted SystemVerilog, IR schema, packages, registry, formatter
  style, and `tern import`/`tern remap`.
* Standard library: `tern doc std --text` offline, or `tern doc std -o
  build/doc` for HTML. [std/README.md](std/README.md) and
  [std/README-part2.md](std/README-part2.md) give the package overview.
* Editor support: [docs/editors.md](docs/editors.md) covers VS Code (extension
  in [editors/vscode/](editors/vscode)), Neovim, Helix and Emacs.

## Status

Tern 0.1.0 implements the language of spec version 0.1 and every command of
spec §13.2. The standard library passes its own 363 tests (`tern test std`).
CI builds with GCC and Clang (C99 and C11, `-Werror`), MSVC and TCC, runs
the test suite under AddressSanitizer and UndefinedBehaviorSanitizer,
cross-checks emitted SystemVerilog against the simulator with Icarus Verilog,
and lints and synthesizes it with Verilator and Yosys.

This is an early release. Known limitations:

* **The compiled simulator needs a host C compiler.** Long tests switch from
  the interpreter to a model compiled with `$CC`/`cc`/`cl` (`--sim auto`);
  without a compiler they stay interpreted, which is slow on large designs.
* **No Verilator test backend.** Tests that instantiate an `extern module`
  cannot run, because `tern test` only accepts `--backend builtin`.
* **External tools for formal and synthesis.** `tern prove` and `tern synth`
  generate scripts, run the tools and parse their results, but they need
  Yosys/SymbiYosys or the vendor tools installed. CI exercises the generated
  formal files with Yosys, but it does not run SymbiYosys.
* **The registry.** `tern add <name>` resolves names through a git index
  (default `https://github.com/tern-lang/registry`, configurable with
  `TERN_INDEX` or `[registry] index`). Git URLs and path dependencies work
  without it.
* **No stable IR yet.** The IR JSON schema ([docs/ir-schema.md](docs/ir-schema.md))
  is version 1, and the spec makes it stable only from Tern 1.0.
* **Unfinished spec items.** The `tern check` warning for a reset-less
  register read before any write (spec §6.5) is not implemented, and tests
  cannot yet drive a signal marked `#[test_port]` (spec §11.1).
* **Small deviations in std** from the spec's tables are listed in the std
  READMEs. For example, `Arbiter` has no `.Weighted` policy.

See [CHANGELOG.md](CHANGELOG.md) for what is in each release, and
[CONTRIBUTING.md](CONTRIBUTING.md) to help.

## License

BSD 3-Clause, see [LICENSE](LICENSE).
