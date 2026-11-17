# 3. Modules and generics

A module instance is an expression. Inputs are passed by name, outputs are
read as fields, and clocks propagate implicitly, so an instantiation takes one
line instead of a port map (spec chapter 7).

## Declaring a module

```tern,test
package fifo

/// A small synchronous FIFO.
pub module Fifo<T: type, Depth: uint = 16>
where Depth.is_pow2()
{
    in clk: clock
    in rst: reset
    in push: Stream<T>
    out pop: Stream<T>
    out count: uint<clog2(Depth) + 1>

    #[ram(distributed)] mem buf: [T; Depth]     // read combinationally below
    reg wr: uint<clog2(Depth)> = 0
    reg rd: uint<clog2(Depth)> = 0
    reg n: uint<clog2(Depth) + 1> = 0

    push.ready = n != Depth
    pop.valid = n != 0
    pop.data = buf[rd]
    count = n

    on clk {
        if push.fire() { buf[wr] = push.data; wr = wr +% 1 }
        if pop.fire() { rd = rd +% 1 }
        n = n +% (push.fire() as uint<1>) -% (pop.fire() as uint<1>)
    }
}

test "keeps the order" {
    let dut = sim Fifo<u8, 4>()
    dut.reset()
    dut.pop.ready = false
    for v in [7, 8, 9] {
        dut.push = Stream { valid: true, data: v }
        dut.step()
    }
    dut.push.valid = false
    expect dut.count == 3
    dut.pop.ready = true
    for v in [7, 8, 9] {
        expect dut.pop.valid and dut.pop.data == v
        dut.step()
    }
}
```

* Generics go in `<>`. `T: type` is a type parameter. `N: uint` and
  `B: bool` are value parameters (enums, structs and arrays work too). Each
  may have a default, and a default may refer to earlier generics
  (`AlmostFull: uint = Depth - 1`).
* A `where` clause states constraints. They are checked at elaboration, and
  the error points at the instantiation that broke them.
* Ports are declared in the body with `in` and `out`. There is no `inout`;
  a top-level pad uses `tri<T>`.
* Port order does not matter, because connections are always made by name.
* Generics are compile-time constants inside the module, so they can size
  ports, registers and memories.

## Instantiating

```tern
package top

use std::fifo::Fifo

module Producer {
    out data: Stream<u8>
    data.valid = true
    data.data = 0x55
}

#[top]
module Top {
    in clk: clock
    in rst: reset
    in push: Stream<u8>
    out level: uint<7>
    out full: bool
    out first: u8

    let src = Producer()
    let a = Fifo<u8, 64>(push: src.data)   // clk and rst are inherited
    a.pop.ready = true
    level = a.count
    full = a.count == 64

    let b = Fifo<u8>(push)                 // shorthand for push: push
    b.pop.ready = true
    first = b.pop.data
}
```

* Every `in` port must be connected. The exceptions are `clock` and `reset`
  ports that match the parent's default domain, which are inherited, and
  data inputs that declare a default (`in clear: bool = false`), which take
  it when left open. An unconnected input is an error. An unused output is
  fine, and `tern check --unused` reports it.
* Generic arguments can be positional or named (`Fifo<u8, Depth: 64>`), with
  types and values in one list. Generic arguments can also be inferred from
  the connected inputs.
* The instance is a value whose fields are the module's `out` ports
  (`a.count`). The flipped fields of an output bundle flow back into the
  instance, so the parent drives them by assignment (`a.pop.ready = true`).
* Shorthand: `Fifo<u8>(push)` connects the parent's `push` to the port of the
  same name.
* An instance can be destructured:
  `let Fifo { pop, count } = Fifo<u8>(push: s)`.
* `@ dom` after the argument list picks the clock domain for all clock and
  reset ports that are not named explicitly. Chapter 4 covers domains.

A missing connection or a violated `where` clause is reported at the
instantiation:

```tern,fail
module Fifo<Depth: uint = 16>
where Depth.is_pow2(), Depth >= 2
{
    in clk: clock
    in flush: bool
    out full: bool
    full = flush
}

#[top]
module Top {
    in clk: clock
    out f: bool
    let x = Fifo<Depth: 6>()
    f = x.full
}
```

```
top.tern:14:13: error: `where` constraint `Depth.is_pow2()` of `Fifo<6>` is not satisfied
   |
14 |     let x = Fifo<Depth: 6>()
   |             ^^^^^^^^^^^^^^
  in: Top
top.tern:2:7: note: constraint declared here (with Depth = 6)
   |
 2 | where Depth.is_pow2(), Depth >= 2
   |       ^^^^^^^^^^^^^^^
```

With `Depth: 8`, the next error would be
``instance `x` of `Fifo<8>` has unconnected input `flush` ``.

### Enum generics with a payload

A generic of enum type selects a variant (`Policy: ArbPolicy = .Fixed`). A
variant may also carry compile-time data, declared like generic
parameters and written like generic arguments, so a policy can come with
its parameters:

```tern
package policy

pub enum Mode<N: uint> {
    Plain,
    Scaled<Gains: [uint; N]>,
}

const fn gain<N: uint>(m: Mode<N>, i: uint) -> uint {
    match m {
        .Scaled { Gains } => Gains[i],
        _ => 1,
    }
}

module Scale<N: uint, M: Mode<N> = .Plain> {
    in x: [u8; N]
    out y: [u16; N]
    for i in 0..N {
        y[i] = x[i] * gain<N>(M, i)
    }
}

#[top]
module Top {
    in a: [u8; 3]
    out b: [u16; 3]
    out c: [u16; 3]
    b = Scale<3>(x: a).y
    c = Scale<3, .Scaled<[1, 2, 4]>>(x: a).y   // or .Scaled<Gains: [1, 2, 4]>
}
```

The payload takes part in comparisons (`M == .Scaled<[1, 2, 4]>`), in
`match` and in the module's emitted name (`Scale_3_Scaled_1_2_4`). A
payload of widthless `uint`s has no bit representation, so such an enum is
a compile-time value only. std's `Arbiter<T, N, .Weighted<[3, 1, 2]>>` is
built this way.

## Elaboration-time code

A `for`, `if` or `let` whose range or condition is a compile-time value is
unrolled at elaboration. Tern has no separate `generate` syntax.

```tern
package pipeline

/// `Stages` register stages; one `reg` and one `on` block per iteration.
pub module Pipeline<T: type, Stages: uint> {
    in clk: clock
    in d: T
    out q: T

    var stage: [T; Stages + 1]
    stage[0] = d
    for i in 0..Stages {
        reg r: T @ clk          // emitted as r_0, r_1, ...
        on clk { r = stage[i] }
        stage[i + 1] = r
    }
    q = stage[Stages]
}

pub module Adder<W: uint, Fast: bool = false> {
    in a: uint<W>
    in b: uint<W>
    out y: uint<W + 1>

    if Fast {
        y = a + b               // only one branch is elaborated
    } else {
        var c: bool
        var s: uint<W>
        comb {
            c = false
            s = 0
            for i in 0..W {
                s[i] = a[i] ^ b[i] ^ c
                c = (a[i] and b[i]) or (c and (a[i] ^ b[i]))
            }
        }
        y = c as uint<1> ++ s
    }
}

#[top]
module Top {
    in clk: clock
    in d: u8
    out q: u8
    out sum: uint<9>

    let p = Pipeline<u8, 3>(d)
    q = p.q
    let add = Adder<8>(a: d, b: 3)
    sum = add.y
}
```

Names declared inside an unrolled loop get the iteration appended in the
emitted SystemVerilog (`r_0`, `r_1`), so waveforms stay readable.

## Functions

`fn` declares a pure combinational function. It takes values and returns a
value, and its body may contain `let`, `if`, `match` and `for` over constant
ranges. It is inlined wherever it is called. A function cannot contain a
`reg` or instantiate a module. Width generics are inferred from the
arguments:

```tern
package funcs

fn parity(x: bit<N>) -> bool { ^x }       // N inferred from the argument

fn saturate(x: sint<W>, lo: sint<W>, hi: sint<W>) -> sint<W> {
    if x < lo { lo } else if x > hi { hi } else { x }
}

#[top]
module Funcs {
    in d: u8
    in x: sint<6>
    out par: bool
    out sat: sint<6>

    par = parity(d)
    sat = saturate(x, -8, 7)
}
```

`const fn` is the subset that also runs at elaboration time, covered in
[chapter 5](05-compile-time.md). Every `const fn` can be used as an ordinary
`fn` on signals.

## Module-local items and published constants

A module body may declare its own `const`, `type`, `enum`, `struct` and `fn`
items, which can use the module's generics. A `pub const` is readable from
outside as `Module<args>::NAME`. The standard library uses this convention to
publish every component's latency:

```tern
package latency

use std::fifo::Fifo

pub module Delay<T: type, N: uint> {
    in clk: clock
    in d: T
    out q: T

    pub const LATENCY: uint = N
    type Word = T

    var stage: [Word; N + 1]
    stage[0] = d
    for i in 0..N {
        reg r: Word @ clk
        on clk { r = stage[i] }
        stage[i + 1] = r
    }
    q = stage[N]
}

#[top]
module Top {
    in clk: clock
    in d: u8
    out q: u8

    static_assert(Delay<u8, 4>::LATENCY == 4)
    static_assert(Fifo<u8, 16>::LATENCY == 2)
    let delay = Delay<u8, 4>(d)
    q = delay.q
}
```

## The top level

`#[top]` marks a module as a synthesis root. A project may have several, one
per build target in `tern.toml`. Only a top may have `tri<T>` ports. On a
clock port, `#[clock(hz: N)]` records the nominal frequency. `tern build`
turns it into a `create_clock` constraint, and the simulator uses it as the
time base for `dut.run(ns: ...)`.

```tern
package board

#[top]
module Board {
    in clk100: clock #[clock(hz: 100_000_000)]
    in rst: reset
    in btn: bool
    out led: bool

    reg on_: bool = false
    on clk100 {
        if btn { on_ = !on_ }
    }
    led = on_
}
```

`clock_hz(clk)` reads that frequency at elaboration. Below the top, a
module learns its clock's frequency through a generic declared
`ClkHz: uint = clock_hz(clk)`: the default takes the frequency of whatever
clock the instantiation connects, and inside the module `clk`'s frequency
is then `ClkHz` (chapter 8 shows std's `Strobe<Hz>` and `Heartbeat` using
it).

Without a `#[top]`, `tern check` and `tern build` elaborate every module
whose generics all have defaults (except defaults read from the clock),
plus the modules the tests simulate.

## Extern modules

An `extern module` declares a SystemVerilog module as a typed black box.
Tern checks the connections, trusts the inside, and passes the file through
to the build:

```tern
package ip

extern module pll_wrapper("ip/pll_wrapper.sv") {
    in refclk: clock
    in rst: reset<async, high>
    out outclk: clock
    out locked: bool
}
```

`tern import` writes these declarations from existing SystemVerilog.
[Migrating from SystemVerilog](10-migrating-from-systemverilog.md) shows the
workflow.

Next: [Bundles and clock domains](04-bundles-and-clock-domains.md).
