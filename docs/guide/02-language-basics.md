# 2. Language basics

A Tern design is a tree of modules. A module is a set of typed signals (wires,
combinational variables and registers) connected by expressions and grouped
into blocks that give those expressions their meaning. This chapter covers
the parts every module uses: files, types and widths, the three signal
declarations, `comb` and `on` blocks, and the reset model (spec chapters 3 to
6).

## Files and packages

Source files end in `.tern` and are UTF-8. A file starts with its package:

```tern
package blinky::util

use std::fifo::Fifo // one name
use std::cdc::{Sync2, PulseSync} // several names
use std::math::* // everything public

pub const Width: uint = 8 // `pub` items are visible to other packages

const Hidden: uint = 3 // everything else is private to the package
```

* Packages map to directories: `package uart::rx` lives in `src/uart/rx/`.
  All files of one directory declare the same package.
* There is no `include` and no preprocessor. `use` imports names.
* `std::prelude` is imported into every file. It provides `Stream`, `Valid`,
  `u8`..`u64`, `s8`..`s64`, `clog2`, `count_ones` and friends.
* A statement ends at the end of its line. A semicolon separates two
  statements on one line. Braces delimit blocks, and Tern has no
  `begin`/`end`.
* Comments are `// ...` and `/* ... */`. `///` doc comments are picked up by
  `tern doc` and shown on hover in the editor.

## Types

Every signal has a static type with a known width and signedness.

| Type | Meaning |
|---|---|
| `bool` | one bit, logical; distinct from `uint<1>` |
| `bit<N>` | N raw bits, no arithmetic |
| `uint<N>` / `sint<N>` | unsigned / two's-complement integer of N bits |
| `uint` / `sint` | widthless, arbitrary-precision integers, compile time only |
| `clock`, `clock<falling>` | a clock |
| `reset`, `reset<async, low>`, ... | a reset; the type carries its style |
| `[T; N]` | array of N elements |
| `struct`, `enum`, `bundle` | user types (bundles are in [chapter 4](04-bundles-and-clock-domains.md)) |
| `tri<T>` | tri-state pad, only on a `#[top]` module |

`u8`, `u16`, `u32`, `u64` and `s8`..`s64` are prelude aliases for
`uint<8>`, `sint<8>` and so on.

Integer literals are untyped (`42`, `0xFF`, `0b1010_0011`, `0o17`,
`1_000_000`) and take their width from the context. A typed literal carries
its own: `42u8`, `-3s12`, `0xFFu<W>`. `all0` and `all1` fill any width, like
SystemVerilog's `'0` and `'1`.

### Width rules

Arithmetic is lossless: a result is always wide enough for every possible
value, so no operator overflows unless you ask it to wrap.

| Expression | Result |
|---|---|
| `uint<N> + uint<M>` | `uint<max(N,M)+1>` |
| `uint<N> +% uint<M>` | `uint<max(N,M)>`, wraps (`-%` likewise) |
| `uint<N> * uint<M>` | `uint<N+M>` |
| `sint<N> - sint<M>` | `sint<max(N,M)+1>` |
| `x / const` | width of `x`; division by a signal needs `std::math::Divider` |
| `x << k`, `x >> k` | width of `x`; bits fall off (`>>>` is arithmetic) |
| `a ++ b` | `bit<Na+Nb>` (SystemVerilog `{a, b}`) |
| `x[hi:lo]` | `bit<hi-lo+1>` |
| `&x`, `\|x`, `^x` | `bool` (reductions) |
| `x == y`, `<`, ... | `bool` |

```tern
package widths

#[top]
module Widths {
    in a: uint<5>
    in b: uint<7>
    in sa: sint<5>
    in sb: sint<7>
    out sum: uint<8>      // max(5, 7) + 1
    out wrap: uint<7>     // wraps, keeps the width
    out prod: uint<12>    // 5 + 7
    out diff: sint<8>
    out cat: bit<12>
    out top3: bit<3>
    out any: bool

    sum = a + b
    wrap = a +% b
    prod = a * b
    diff = sa - sb
    cat = a ++ b
    top3 = b[6:4]
    any = |a or ^b
}
```

Assigning a wider value to a narrower signal is an error, never a silent
truncation:

```tern,fail
module Counter {
    in clk: clock
    in rst: reset
    out q: uint<4>

    reg tick: uint<4> = 0
    on clk {
        tick = tick + 1       // uint<5> into uint<4>
    }
    q = tick
}
```

```
counter.tern:8:16: error: width mismatch: `tick + 1` is uint<5>, `tick` is uint<4>
  |
8 |         tick = tick + 1       // uint<5> into uint<4>
  |                ^^^^^^^^
  = help: use `+%` for a wrapping add, or truncate with `as uint<4>`
  in: Counter
```

### Conversions

Widening never needs a cast: `uint<N>` converts to any `uint<M>` with
`M >= N`, `sint<N>` to a wider `sint<M>` (sign-extended), an untyped literal
to any type that can hold it, and `bit<N>` to `uint<N>` and back. Everything
else is explicit:

| Conversion | Meaning |
|---|---|
| `x as uint<8>` | truncate or zero-extend |
| `x as sint<9>` | reinterpret or extend |
| `x as bit<N>` | forget signedness |
| `x as bool` | `x != 0` |
| `bits(x)` | any value (struct, enum, array) to its raw bits |
| `T::from_bits(b)` | the reverse |
| `std::math::widen_signed(x)` | `uint<N>` to `sint<N+1>`, lossless |

`bool` is not an integer, and `uint` and `sint` do not mix without a cast:

```tern,fail
module Mixed {
    in a: uint<8>
    in b: sint<8>
    in f: bool
    in g: bool
    out y: sint<10>
    out n: uint<2>
    y = a + b      // error: cannot mix uint<8> and sint<8> in `+`
    n = f + g      // error: bool is not an integer: `+` on bool
}
```

The fixes are `widen_signed(a) + b` and `f as uint<1> + g as uint<1>` (or
`count_ones(...)`).

```tern
package conversions

use std::math::widen_signed

#[top]
module Conversions {
    in a: uint<8>
    in b: sint<8>
    in f: bool
    in g: bool
    out y: sint<10>
    out n: uint<2>
    out low: uint<4>
    out nonzero: bool

    y = widen_signed(a) + b
    n = f as uint<1> + g as uint<1>
    low = a as uint<4>
    nonzero = a as bool
}
```

### Structs and enums

```tern
package types

struct Pixel { r: u8, g: u8, b: u8 }   // 24 bits, r is the MSB byte

enum Op { Nop, Add, Sub, Load }        // 2 bits, encoding chosen by the compiler

enum Cmd {                             // an enum with payloads: a tagged union
    Write { addr: u16, data: u32 },
    Read { addr: u16 },
    Idle,
}

#[encoding(onehot)]
enum Hot { A, B, C }

#[top]
module Types {
    in grad: u16
    in payload: u32
    in c: Cmd
    out grey: uint<10>
    out addr: u16
    out cmd: Cmd
    out raw: bit<24>

    let p = Pixel { r: 255, g: 0, b: grad[7:0] }
    grey = (p.r + p.g + p.b) / 3
    raw = bits(p)
    cmd = Cmd::Write { addr: 0x10, data: payload }
    addr = match c {
        .Write { addr, .. } | .Read { addr } => addr,
        .Idle => 0,
    }
}
```

* A struct is packed in declaration order, first field most significant.
* An enum variant is written `.Name` when the type is known from the context
  and `Type::Name` otherwise.
* Enums with payloads are packed as `tag ++ payload`. They replace the
  union-plus-manual-tag pattern.
* `#[encoding(binary)]`, `#[encoding(onehot)]` and `#[encoding(gray)]` choose
  the tag encoding.

### Arrays

`[u8; 16]` has 16 elements, indexed with `a[i]`. A constant index out of range
is a compile error. A dynamic index must be exactly wide enough that every
value is in range (`uint<4>` for 16 elements), or be an enum. `a.len`,
`a.map(f)`, `a.reduce(f)` and `a.zip(b)` work at elaboration time to build
logic.

```tern
package arrays

fn inc(x: u8) -> u8 { x +% 1 }

fn add(a: u8, b: u8) -> u8 { a +% b }

#[top]
module Arrays {
    in i: uint<4>
    in words: [u8; 4]
    out sel: u8
    out total: u8

    let table: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
    sel = table[i]
    total = words.map(inc).reduce(add)   // an adder tree, built at elaboration
    static_assert(table.len == 16)
}
```

## Operators

From highest to lowest precedence: field/index/slice/call/`as`; unary `-`
`!` `~` and the reductions `&` `|` `^`; `*`; `+ - +% -%`; `<< >> >>>`; `&`;
`^`; `|`; `++`; comparisons; `and` (`&&`); `or` (`||`). `not` and `xor` are
spelled as words too. There is no `? :`. `if` and `match` are expressions:

```tern
package exprs

#[top]
module Exprs {
    in sel: bool
    in a: u8
    in b: u8
    in code: uint<2>
    out y: u8
    out name: u8

    y = if sel { a } else { b }
    name = match code {
        0 => 0x41,
        1 => 0x42,
        _ => 0x3F,
    }
}
```

Equality is always 2-state, so Tern has no `===`.

## The three signal declarations

| Declaration | What it is | Where it is assigned |
|---|---|---|
| `let name = expr` | a named wire | once, at the declaration |
| `var name: T [= default]` | a combinational variable | in one `comb` block |
| `reg name: T [= init] [@ domain]` | a flip-flop | in one `on` block |

Output ports are assigned like a `let` (`out_port = expr` at module level) or
like a `var` (inside one `comb` block).

Each signal has exactly one driver. Driving a `var` from two `comb` blocks, a
`reg` from two `on` blocks, or an output from a block and a continuous
assignment is a compile error. So is leaving an output or a `var` undriven,
and Tern has no implicit nets.

```tern,fail
module TwoDrivers {
    in a: u8
    in b: u8
    out y: u8
    y = a
    comb { y = b }   // error: multiple drivers for `y`
}
```

## Combinational blocks

A `comb` block gives its `var`s and outputs values on every path through it.
The last write wins. A path that leaves a signal unassigned is an error
unless the `var` has a default, so a latch cannot be written by accident.

```tern
package alu

enum Op { Nop, Add, Sub, And }

#[top]
module Alu {
    in op: Op
    in a: u32
    in b: u32
    out y: u32
    out carry: bool

    var result: u32          // no default: every path below must assign it
    var c: bool = false      // default given: paths may skip it
    comb {
        match op {
            .Add => { result = (a + b) as u32; c = (a + b)[32] },
            .Sub => result = a -% b,
            .And => result = a & b,
            .Nop => result = 0,
        }
    }
    y = result
    carry = c
}
```

Without the `.Nop` arm, the `match` would not be exhaustive. With the arm
present but empty, the compiler names the path that misses the assignment:

```tern,fail
enum Op { Nop, Add }

module Alu {
    in op: Op
    in a: u32
    in b: u32
    out y: u32

    var result: u32
    comb {
        match op {
            .Add => result = a +% b,
            .Nop => {},
        }
    }
    y = result
}
```

```
alu.tern:11:9: error: `result` may be unassigned when op == Nop
   |
11 |         match op {
   |         ^^^^^^^^^^
   = help: assign it on every path, or give it a default: `var result: uint<32> = ...`
  in: Alu
```

Inside a `comb` block, `let` binds an intermediate value; `if`, `match` and
`for` over constant ranges structure the assignments.

## Clocked blocks

`on clk { ... }` describes the flip-flops of one clock domain.

```tern,test
package counter

module Counter {
    in clk: clock
    in rst: reset
    in load: bool
    in en: bool
    in din: u8
    out count: u8

    reg value: u8 = 0      // reset value 0
    on clk {
        if load { value = din } else if en { value = value +% 1 }
    }
    count = value
}

test "counts and loads" {
    let dut = sim Counter()
    dut.reset()
    dut.en = true
    dut.load = false
    dut.step(3)
    expect dut.count == 3
    dut.load = true
    dut.din = 200
    dut.step()
    expect dut.count == 200
}
```

* Inside `on clk`, reading a `reg` gives its value before the edge, and
  writing it sets the value after the edge. This is SystemVerilog's
  non-blocking assignment, and it is the only kind.
* A `reg` that is not assigned on some path keeps its value: Tern infers an
  enable. This is the one place where "not assigned" is not an error, because
  a register that holds its value is a flip-flop, not a latch.
* A `reg` may be assigned several times in one block. The last assignment in
  program order wins.
* Intermediate values inside the block are `let` bindings.
* A module with exactly one `clock` port (and at most one `reset`) has a
  default domain, so `reg` and `on` need no domain annotation. With several
  clocks, see [chapter 4](04-bundles-and-clock-domains.md).

## Reset

The reset style lives in the type of the reset port, not in the code:

| Port type | Style |
|---|---|
| `reset` | synchronous, active high |
| `reset<async>` | asynchronous, active high |
| `reset<low>` | synchronous, active low |
| `reset<async, low>` | asynchronous, active low |

`reg x: T = init` resets to `init`. `reg x: T` has no reset. A reset value
must be a compile-time constant. The compiler generates the right
sensitivity list and polarity, so no module writes `if (!rst_n)`.

```tern
package resets

#[top]
module Resets {
    in clk: clock
    in rst_n: reset<async, low>
    in d: u32
    out q: u32
    out valid: bool

    reg data: u32              // no reset: a data register
    reg seen: bool = false     // control state is reset
    on clk {
        data = d
        seen = true
    }
    q = data
    valid = seen
}
```

* A domain with a clock but no reset is allowed. Every `reg` in it is
  reset-less, and `= init` becomes a power-on value (an `initial` value on
  FPGA targets, a warning on ASIC targets).
* A reset is never data. `rst as bool` is an error. A design that needs to
  know whether it is in reset uses a `reg in_reset: bool = true` that it
  clears.
* In simulation, the initial value of a reset-less `reg` is pseudo-random
  per seed (`tern test --seed n`), so a design that relied on zeros fails
  deterministically instead of passing by luck. The spec also has
  `tern check` warn about a reset-less `reg` that is read before any write.
  This release does not implement that warning yet.

Next: [Modules and generics](03-modules-and-generics.md).
