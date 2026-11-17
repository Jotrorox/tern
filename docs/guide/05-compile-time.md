# 5. Compile-time evaluation

SystemVerilog spreads compile-time work over `parameter`, `localparam`,
`$clog2`, `generate` and the preprocessor. Tern does all of it with one
small, deterministic compile-time language: `const` values, `const fn`, and
ordinary `if`/`for` over them (spec chapter 9).

## Compile-time values

* `const Name: T = expr` declares a constant at package or module level. `T`
  is usually a widthless `uint`, `sint` or `bool`, but any type works.
* Generic parameters are constants inside the module.
* Widthless `uint` and `sint` have arbitrary precision at compile time, so
  parameter arithmetic cannot overflow at 32 bits.
* A value is compile-time if every input is: a literal, a `const`, a generic,
  the index of a loop over a constant range, or a `const fn` call on those.

```tern
package consts

const Big: uint = 1 << 100

const Huge: uint = Big * Big + 1          // no overflow at compile time

pub const ClkHz: uint = 100_000_000

pub const Baud: uint = 115_200

const Div: uint = ClkHz / Baud            // 868

#[top]
module Consts {
    out div: uint<10>
    static_assert(Huge > Big)
    static_assert(Div == 868, "integer division rounds down")
    div = Div
}
```

## `const fn`

A `const fn` runs at elaboration time. Its body may use `var`, `while`,
`for`, `if`, `match`, recursion, arrays and structs. It cannot read signals,
instantiate modules or do I/O.

```tern
package tables

const fn ceil_log2(n: uint) -> uint {
    var r = 0
    while (1 << r) < n { r = r + 1 }
    r
}

const fn crc_table(poly: uint<32>) -> [uint<32>; 256] {
    var t: [uint<32>; 256]
    for i in 0..256 {
        var c = i as uint<32>
        for _ in 0..8 { c = if c[0] { (c >> 1) ^ poly } else { c >> 1 } }
        t[i] = c
    }
    t
}

// computed once at compile time; becomes a ROM in hardware
const Crc32Table: [uint<32>; 256] = crc_table(0xEDB88320)

#[top]
module CrcRom {
    in index: u8
    out entry: u32

    static_assert(ceil_log2(1000) == 10)
    static_assert(Crc32Table[1] == 0x77073096)
    entry = Crc32Table[index]
}
```

Evaluation is bounded: `--const-eval-limit` (default 10,000,000 steps)
makes a runaway loop a compile error instead of a hang.

```tern,fail
const fn spin(n: uint) -> uint {
    var i = 0
    while i >= 0 { i = i + n }
    i
}

module Top {
    out y: u8
    const X: uint = spin(1)
    y = 0
}
```

```
spin.tern:3:24: error: compile-time evaluation exceeded the step limit (10000000 steps)
```

### One source, ROM or circuit

A `const fn` called with signal arguments is just a `fn`. The same source
produces a constant at compile time or a combinational circuit at run time.
`while` is rejected in the circuit case, because it has no bound.

```tern
package reverse

const fn reverse8(x: uint<8>) -> uint<8> {
    var r: uint<8> = 0
    for i in 0..8 { r[7 - i] = x[i] }
    r
}

#[top]
module Reverse {
    in x: u8
    out rev: u8

    const R: u8 = reverse8(0x01)   // evaluated by the compiler
    static_assert(R == 0x80)
    rev = reverse8(x)              // eight wires
}
```

## Static checks

* `static_assert(cond, "message")` works in module bodies, function bodies
  and elaboration-time `if`/`for` branches. It fires at elaboration and
  reports the instantiation path.
* `where` clauses on generics are sugar for `static_assert`s evaluated at
  the instantiation site.
* `const_log("...{}...", value)` prints during elaboration, which helps when
  debugging generic code.

```tern
package checks

pub module UartTx<ClkHz: uint, Baud: uint = 115_200> {
    in clk: clock
    out txd: bool

    const Div: uint = ClkHz / Baud
    static_assert(Div >= 16, "clock too slow for this baud rate")
    const_log("UartTx divider = {}", Div)
    txd = true
}

#[top]
module Top {
    in clk: clock
    out txd: bool
    let u = UartTx<ClkHz: 100_000_000>()
    txd = u.txd
}
```

With `ClkHz: 1_000_000`, the assertion fails and the error names the
instance:

```
checks.tern:8:5: error: static assertion failed: clock too slow for this baud rate
```

## Type-level computation

* `type` aliases may depend on constants: `type Addr = uint<clog2(Depth)>`.
* `typeof(expr)` names the type of an expression.
* `T::WIDTH` is the width of any type. `T::MAX` and `T::MIN` are the extreme
  values of an integer type.

```tern
package typelevel

module Rom<Depth: uint>
where Depth.is_pow2(), Depth <= 256
{
    in addr: uint<clog2(Depth)>
    out data: u16

    type Addr = uint<clog2(Depth)>
    let a: Addr = addr
    data = a * 3

    static_assert(typeof(a)::WIDTH == clog2(Depth))
    static_assert(typeof(data)::MAX == 0xFFFF)
    static_assert(s8::MIN == -128 and uint<4>::MAX == 15)
}

#[top]
module Top {
    in addr: uint<4>
    out data: u16
    let rom = Rom<16>(addr)
    data = rom.data
}
```

There is deliberately no type-level `if`. A module that needs two shapes is
two modules, or uses an `enum` payload.

## No preprocessor

| SystemVerilog | Tern |
|---|---|
| `` `define WIDTH 8 `` | `const Width: uint = 8` |
| `` `define MAX(a, b) ... `` | `const fn` or `fn` |
| `` `include "defs.svh" `` | `use pkg::*` |
| `` `ifdef DEBUG `` | a `bool` generic, or a feature flag `cfg::debug` |

Feature flags are declared in `tern.toml` with their default value:

```toml
[features]
debug_ila = false
```

They are read as `cfg::<name>`. A command line overrides them with
`--features debug_ila` or `--features -debug_ila`, and a build target can set
its own:

```tern,ignore
module Probe<Debug: bool = cfg::debug_ila> {
    in d: u8
    out q: u8
    if Debug { q = d ^ 0xFF } else { q = d }   // only one branch is elaborated
}
```

(This snippet is not checked on its own, because `cfg::debug_ila` only
exists in a project whose `tern.toml` declares it.)

The emitted SystemVerilog has all of this resolved, so downstream tools never
see a macro.

Next: [Memories, state machines and register maps](06-memories-fsms-regmaps.md).
