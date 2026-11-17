# 7. Testing and verification

Tests, assertions and properties are part of the language. They run with
`tern test` on a built-in cycle-based simulator and use the same types and
expressions as the design. Nothing inside a `test` or `property` is ever
synthesized (spec chapter 11).

## Unit tests

```tern,test
package fifo_test

use std::fifo::Fifo

test "fifo preserves order" {
    let dut = sim Fifo<u8, 4>()      // the harness owns clk/rst of the default domain
    dut.reset()                      // asserts reset for 2 cycles, then releases it
    for v in [1, 2, 3, 4] {
        dut.push = Stream { valid: true, data: v }
        dut.step()                   // one clock edge; combinational logic settles after it
    }
    dut.push.valid = false
    expect dut.count == 4
    dut.pop.ready = true
    for v in [1, 2, 3, 4] {
        expect dut.pop.valid and dut.pop.data == v
        dut.step()
    }
    expect !dut.pop.valid
}
```

* `sim Module<...>(inputs...)` builds a simulation handle. Inputs that are
  not given at construction are driven later by assignment
  (`dut.push = ...`, `dut.pop.ready = true`).
* Outputs, registers and any internal signal are readable by path:
  `dut.count`, `dut.rd_ptr`, `dut.rx_fifo.count`. Only inputs can be
  written, and Tern has no `force`. The spec exposes an internal signal to
  tests with `#[test_port]`. This release accepts the attribute, but driving
  such a signal from a test does not work yet.
* `dut.reset()` asserts reset for two cycles. `dut.step(n = 1)` advances
  `n` cycles of the default domain. `dut.settle()` evaluates combinational
  logic without a clock edge. `dut.step_until(cond, max: 1000)` steps until
  `cond` holds, returns the number of cycles it took, and fails at `max`.
  `dut.cycle()` is the current cycle number.
* `expect cond` records a failure and continues. `assert cond` stops the
  test. Both print the expression with the values of its operands and the
  cycle number.
* `log("...{}...", x)` prints. `dut.dump("waves.vcd")` records a VCD
  waveform, readable by GTKWave and Surfer.
* `var` and `let` work inside tests, and so do `for`, `if` and calls of
  `const fn`s.

A failing check looks like this:

```
test counts to four ... FAILED
    counter.tern:31:5: expect failed at cycle 5: dut.count == want
        dut.count = 3
        want = 4

FAILED: 0 passed, 1 failed
replay a failing test with: tern test --seed 1 "<name>"
```

## Table-driven and randomized stimulus

```tern,test
package alu_test

enum Op { Add, Sub, And }

module Alu<W: uint> {
    in op: Op
    in a: uint<W>
    in b: uint<W>
    out y: uint<W>

    y = match op {
        .Add => a +% b,
        .Sub => a -% b,
        .And => a & b,
    }
}

test "alu ops" {
    let dut = sim Alu<32>()
    for (op, a, b, want) in [(.Add, 1, 2, 3), (.Sub, 5, 7, 0xFFFF_FFFE), (.And, 0xF0, 0x3C, 0x30)] {
        dut.op = op; dut.a = a; dut.b = b
        dut.settle()
        expect dut.y == want
    }
}

test "random adds" {
    let dut = sim Alu<32>()
    for _ in 0..1000 {
        let a = rand<u32>()
        let b = rand<u32>() where b <= 0x1000   // rejection constraint
        dut.op = .Add; dut.a = a; dut.b = b
        dut.settle()
        expect dut.y == (a + b) as u32
    }
}
```

`rand<T>()` works for integers, `bool` and enums. It is seeded per test, and
the seed is printed on failure, so `tern test --seed <n> "<name>"` replays a
failing run exactly. The same seed also picks the pseudo-random power-on
values of reset-less registers.

## Assertions inside the design

Immediate assertions live in `comb` and `on` blocks. The simulator and formal
verification check them. They are stripped from synthesis output, or kept as
SystemVerilog `assert property` with `tern build --emit-assertions`.

```tern,ignore
on clk {
    assert !(push.fire() and count == Depth), "push into full fifo"
}
```

## Properties

A small subset of SVA, declared inside a module and checked on every cycle of
its domain, both while tests run and by `tern prove`:

```tern,test
package gate

module Gate {
    in clk: clock
    in rst: reset
    in push: Stream<u8>
    out pop: Stream<u8>
    out count: uint<3>

    reg n: uint<3> = 0
    reg held: bool = false
    reg data: u8 = 0

    push.ready = !held or pop.ready
    pop.valid = held
    pop.data = data
    count = n

    on clk {
        if pop.fire() { held = false }
        if push.fire() {
            held = true
            data = push.data
            n = n +% 1
        }
        assert n != 7, "counter about to wrap"
    }

    /// A stalled output beat is held unchanged.
    property data_stable {
        always (pop.valid and !pop.ready) |=> pop.valid and stable(pop.data)
    }
    /// The consumer never stalls for more than three cycles.
    property short_stalls {
        always (pop.valid and !pop.ready) |-> eventually[..3] pop.ready
    }
    assume property no_push_after_five { always count >= 5 |-> !push.valid }
    cover property two_in_a_row { push.fire() ##1 push.fire() }
}

test "a clean run" {
    let dut = sim Gate()
    dut.reset()
    dut.pop.ready = true
    for v in [1, 2, 3] {
        dut.push = Stream { valid: true, data: v }
        dut.step()
    }
    dut.push.valid = false
    dut.step(2)
    expect dut.count == 3
}
```

| Operator | Meaning |
|---|---|
| `always p`, `never p` | `p` holds (does not hold) on every cycle |
| `a \|-> b` | when `a` holds, `b` holds in the same cycle |
| `a \|=> b` | when `a` holds, `b` holds in the next cycle |
| `a ##n b`, `a ##[m:n] b`, `##[1:$]` | `b` follows `a` after n (m to n, any number of) cycles |
| `eventually[..n] p` | `p` holds within n cycles |
| `stable(x)`, `rose(x)`, `fell(x)`, `past(x, n)` | sampled-value functions |
| `p until q` | `p` holds until `q` does |

* `property` (or `assert property`) is checked. `assume property`
  constrains the inputs: a test reports a violated assumption and goes on,
  and formal verification treats it as given. `cover property` counts how
  often a sequence happens.
* `sequence name { ... }` names a sequence for reuse.
* Repetition (`[*n]`, `[->n]`) is reserved for a later version. Anything this
  subset cannot say can be written in SystemVerilog against the emitted code.

A property that fails stops the test and names the cycle. A test that pushes
one beat into `Gate` and then never sets `pop.ready` reports:

```
test a stalled consumer ... FAILED
    gate.tern:34:5: property `short_stalls` failed at cycle 6 (attempt started at cycle 3)
```

## Running tests

```sh
tern test                       # every test in src/ and tests/
tern test fifo                  # only tests whose name contains "fifo"
tern test src/uart.tern         # the tests of one file
tern test --list                # list the tests instead of running them
tern test --seed 42             # another base seed for rand<T>()
tern test --waves               # keep a VCD of each failing test in build/waves/
tern test --cover               # line, branch and cover-property coverage
tern test -j 4                  # four parallel worker processes
tern test std                   # the standard library's own tests
```

Tests run in parallel as separate worker processes, so a crashing test cannot
take down the run.

## Coverage

`tern test --cover` reports line coverage, branch coverage per `if` and per
`match` arm, and `cover property` hits. Full match-arm coverage is possible by
construction, because every arm must exist:

```
coverage:
  ticker.tern: lines 7/7 (100%), branches 4/5 (80%)
    branches not taken: 18:9 arm 3
  cover:
    property wraps (ticker.tern:34:5): 1 hit
    property counts_to_three (ticker.tern:35:5): 0 hits  <- never hit
```

Coverage is reported for your own files, not for the standard library.

## Formal verification

`tern prove` runs bounded model checking over every `assert`, `assume` and
`cover` in a module, plus its properties. It needs
[Yosys](https://github.com/YosysHQ/yosys),
[SymbiYosys](https://github.com/YosysHQ/sby) and an SMT solver (yices by
default) on your `PATH`. The [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build)
bundles all of them.

```sh
tern prove Gate                 # BMC, depth 20
tern prove Gate --depth 40
tern prove Gate --induct        # also try k-induction for an unbounded proof
tern prove Gate --no-run        # only write build/formal/Gate/ (Gate.sby, Verilog, wrapper)
```

A generic module is proved with its default generics, or else with the
arguments of its instantiation in the design's top.

`tern prove` emits Verilog-2005 plus generated checker logic, a wrapper that
assumes reset in the first cycle, and a `.sby` file. Without SymbiYosys it
stops after writing these files and says how to run them by hand.

When a check fails, the counterexample comes back as a waveform and as a
generated `test` that reproduces the trace, written to
`tests/formal_<module>_<check>.tern` (or printed with `--print-test`). A
formal failure becomes a regression test with one command.

## Waveforms

* `tern test --waves` keeps a VCD of every failing test in `build/waves/`.
* `dut.dump("file.vcd")` in a test records one explicitly.
* `tern sim <top> --vcd file.vcd` records an interactive session.

Names inside unrolled loops keep their iteration suffix (`r_0`, `r_1`), so
waveforms stay readable.

## Designs with extern modules

The built-in simulator cannot run the inside of an `extern module`. The spec
runs such tests through a Verilator backend (`tern test --backend
verilator`). That backend is not available in this release: `tern test`
accepts only `--backend builtin`. Test the Tern parts of a mixed design on
their own, or simulate the emitted SystemVerilog in your existing flow.

Next: [The standard library](08-standard-library.md).
