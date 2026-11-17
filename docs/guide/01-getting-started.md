# 1. Getting started

This chapter builds the compiler, creates a project, and takes it through the
everyday loop: check, test, simulate, build.

## Build `tern`

`tern` is one C program with no dependencies. Any C11 compiler builds it:

```sh
git clone https://github.com/Jotrorox/tern
cd tern
make -j8                      # POSIX make, per-file objects in build/obj/
# or, without make:
cc -O2 -o tern src/unity.c    # the whole compiler as one translation unit
```

On Windows, open a Developer Command Prompt for Visual Studio 2019 16.8 or
newer and run `build.bat`. It writes `tern.exe`.

Copy the binary somewhere on your `PATH`. `make install` copies it to
`/usr/local/bin`, and `make install PREFIX=$HOME/.local` puts it under your
home directory. The standard library is compiled into the binary, so nothing
else needs to be installed.

```sh
tern --version
tern help            # all commands
tern help test       # the options of one command
```

Optional external tools are only needed for some commands. `tern` looks for
them on your `PATH`:

| Tool | Used by |
|---|---|
| `git` | `tern new` (runs `git init`), `tern add`, `tern update`, `tern vendor`, `tern publish` |
| Yosys + SymbiYosys (+ an SMT solver such as yices) | `tern prove` |
| Yosys, Vivado or Quartus | `tern synth` |
| Verilator | `tern build --lint` |

`check`, `build`, `test`, `sim`, `fmt`, `doc` and `lsp` need nothing but
`tern`.

## A first project

```sh
tern new blinky
cd blinky
```

`tern new` creates:

```
blinky/
  tern.toml              package name, version, dependencies, build targets
  src/blinky.tern        a blinking-LED module with a test
  tests/blinky_test.tern a second test, in its own package
  README.md
  .gitignore             ignores build/
```

It also runs `git init` when `git` is installed. Pass `--no-git` to skip that.

`src/blinky.tern` looks like this:

```tern,test
package blinky

/// Blinks `led`: it toggles every `Period` clock cycles.
#[top]
pub module Blinky<Period: uint = 50_000_000>
where Period >= 2
{
    in clk: clock
    in rst: reset
    /// The LED output.
    out led: bool

    reg count: uint<clog2(Period)> = 0
    reg state: bool = false

    led = state

    on clk {
        if count == Period - 1 {
            count = 0
            state = !state
        } else {
            count = count +% 1
        }
    }
}

test "led toggles every Period cycles" {
    let dut = sim Blinky<Period: 4>()
    dut.reset()
    expect !dut.led
    dut.step(4)
    expect dut.led
    dut.step(4)
    expect !dut.led
}
```

Several things already differ from SystemVerilog:

* `Period` is a generic with a default value. A `where` clause constrains it.
  The compiler checks the clause wherever the module is used.
* `reg count: uint<clog2(Period)> = 0` declares a flip-flop. Its width comes
  from the generic, and `= 0` is its reset value. No module ever writes a
  reset branch by hand.
* `on clk { ... }` is the clocked block. Plain `=` sets a register's next
  value there, so Tern needs no `<=`.
* `count +% 1` is a wrapping add. A plain `count + 1` is one bit wider than
  `count`, and assigning it back to `count` is a compile error.
* The test sits next to the design and uses the same types.

## The everyday loop

```sh
tern check           # every compile-time check, no output files
tern test            # run all test blocks in src/ and tests/
tern build           # write SystemVerilog to build/default/
tern fmt             # format the sources canonically
```

`tern test` runs the tests in parallel and prints one line per test:

```
running 2 tests
test led toggles every Period cycles ... ok
test Blinky holds the LED for exactly Period cycles ... ok

ok: 2 passed, 0 failed
```

`tern build` elaborates every target in `tern.toml` and writes one `.sv` file
per module, plus constraint fragments:

```
tern build: Blinky<50000000> -> build/default/ (target default, SystemVerilog)
  modules      Blinky_50000000.sv
  constraints  Blinky_50000000.xdc Blinky_50000000.sdc
  3 files, 0 changed
```

Tern monomorphizes generics into the module name (`Blinky_50000000`), and the
emitted code is ordinary, readable SystemVerilog with `// tern: file:line`
markers that point back to your source.
[The toolchain](09-toolchain.md) describes the output in detail.

## Poking at a design interactively

`tern sim` opens a REPL on a module. The design is called `dut`:

```sh
tern sim 'Blinky<Period: 4>'
```

```
simulating Blinky<Period: 4>; `help` lists the commands
tern> reset
tern> step 4
tern> peek led
led = true
tern> signals
  clk                      harness  bool
  rst                      harness  bool
  led                      out      bool
  count                    reg      uint<2>
  state                    reg      bool
  domain (default) (100000000 Hz)
tern> quit
```

Any line that is not a REPL command runs as a statement of a test body, so
`expect dut.led` and `dut.step_until(dut.led, max: 100)` work too. With a file
argument, `tern sim` runs that file as a script and stops at the first failing
line. `--vcd waves.vcd` records a waveform you can open in GTKWave or Surfer.

## Single files

Every command also works on loose files outside a project. This is handy for
experiments and is how the examples in this guide are checked:

```sh
tern check scratch.tern
tern test scratch.tern
tern build --stdout scratch.tern
```

## Editor support

`tern lsp` is a language server. It gives diagnostics as you type,
go-to-definition, and hover with each signal's type, width, clock domain and
driver. [docs/editors.md](../editors.md) has setup instructions for VS Code
(the extension lives in `editors/vscode/`), Neovim, Helix and Emacs.

Next: [Language basics](02-language-basics.md).
