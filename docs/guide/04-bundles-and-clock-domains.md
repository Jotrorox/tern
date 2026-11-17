# 4. Bundles, clock domains and CDC

A bundle is a struct whose fields have directions. Declaring a port `in` or
`out` fixes every field's direction at once, so one keyword, `flip`, replaces
SystemVerilog interfaces and modports. Clock domains are part of a signal's
type, which makes a clock-domain crossing without a synchronizer a compile
error (spec chapter 8).

## Bundles

The prelude defines the two handshakes the whole standard library speaks:

```tern,ignore
pub bundle Valid<T: type> {
    valid: bool
    data: T
}

pub bundle Stream<T: type> {
    valid: bool
    data: T
    flip ready: bool      // flows against the bundle's direction
}
```

On an `in` port, plain fields are inputs and `flip` fields are outputs. On an
`out` port it is the other way round. Nesting composes:

```tern
package bus

pub bundle Axi4Lite<AW: uint = 32, DW: uint = 32> {
    aw: Stream<uint<AW>>,
    w: Stream<struct { data: uint<DW>, strb: bit<DW / 8> }>,
    flip b: Stream<bit<2>>,
    ar: Stream<uint<AW>>,
    flip r: Stream<struct { data: uint<DW>, resp: bit<2> }>,
}

/// Accepts every write and answers OKAY; reads are never accepted.
#[top]
module WriteSink {
    in clk: clock
    in rst: reset
    in bus: Axi4Lite

    // `flip b` on an `in` port: b.valid and b.data are outputs, b.ready an input
    bus.aw.ready = true
    bus.w.ready = true
    bus.b = Stream { valid: bus.aw.valid and bus.w.valid, data: 0 }
    bus.ar.ready = false
    bus.r.valid = false
    bus.r.data.data = 0
    bus.r.data.resp = 0
}
```

A bundle value can be assigned three ways:

* whole, `tx = rx`, which connects both directions in one line;
* field by field, `tx.ready = ...`;
* with a literal, `Stream { valid: true, data: x }`. A literal lists only the
  fields that flow in the bundle's direction, so `ready` is not in it.

`fn` items inside a bundle body see the bundle as `self` (no parameter is
declared for it):

```tern,test
package req

pub bundle Req<T: type> {
    valid: bool,
    data: T,
    flip ack: bool,

    /// True in a cycle in which a request is accepted.
    fn done() -> bool { self.valid and self.ack }
}

module Server {
    in clk: clock
    in rst: reset
    in req: Req<u8>
    out count: u8

    reg n: u8 = 0
    req.ack = true
    on clk {
        if req.done() { n = n +% 1 }
    }
    count = n
}

test "counts accepted requests" {
    let dut = sim Server()
    dut.reset()
    dut.req = Req { valid: true, data: 1 }
    dut.step(3)
    expect dut.count == 3
}
```

`Stream` has `fire()` (valid and ready), `map(f)` and `skid()`:

```tern,test
package streams

fn double(x: u8) -> u8 { x +% x }

module Stage {
    in clk: clock
    in rst: reset
    in rx: Stream<u8>
    out tx: Stream<u8>
    out doubled: Stream<u8>
    in t: Stream<u8>
    in u: Stream<u8>
    out sk: Stream<u8>
    out fired: bool

    tx = rx                    // both directions in one line
    doubled = t.map(double)    // ready flows back to t
    sk = u.skid()              // a one-entry register slice (std::stream::Skid)
    fired = rx.fire()
}

test "map doubles the payload" {
    let dut = sim Stage()
    dut.reset()
    dut.t = Stream { valid: true, data: 21 }
    dut.doubled.ready = true
    dut.settle()
    expect dut.doubled.valid and dut.doubled.data == 42
    expect dut.t.ready
}
```

## Clock domains

A domain is a clock plus, optionally, a reset; the two travel together. A
module with one `clock` port has an implicit default domain. A module with
several must name them, and every `reg`, `on` block and instance says which
domain it belongs to:

```tern
package domains

module Counter {
    in clk: clock
    in rst: reset
    out count: u8
    reg c: u8 = 0
    on clk { c = c +% 1 }
    count = c
}

#[top]
module Top {
    in sys_clk: clock
    in sys_rst: reset
    in pix_clk: clock
    in pix_rst: reset
    in go: bool @ sys
    out sys_count: u8 @ sys
    out pix_count: u8 @ pix
    out busy: bool @ sys

    domain sys = (sys_clk, sys_rst)
    domain pix = (pix_clk, pix_rst)

    let a = Counter() @ sys       // clk/rst of the instance come from `sys`
    let b = Counter() @ pix
    sys_count = a.count
    pix_count = b.count

    reg running: bool = false @ sys
    on sys {
        if go { running = true }
    }
    busy = running
}
```

* `domain name = (clock, reset)` or `domain name = (clock)` (no reset).
* `reg x: T = init @ name` and `on name { ... }` place registers in a
  domain. `@ clk_port` works too for a domain without a reset.
* Ports can carry a domain (`in go: bool @ sys`).
* `name.clock` and `name.reset` are the domain's signals, for connecting
  modules that take several clocks.
* An instance's `@ name` connects every clock and reset port it does not name
  explicitly. The reset style must match: connecting a `reset<async, low>`
  domain to a module whose port is a plain `reset` is an error.

Combinational logic has the domain of the signals it reads, and constant logic
belongs to no domain. Mixing two domains is an error at the point where they
mix:

```tern,fail
module Mixed {
    in sys_clk: clock
    in pix_clk: clock
    out y: u8 @ sys
    domain sys = (sys_clk)
    domain pix = (pix_clk)

    reg line: u8 = 0 @ pix
    reg count: u8 = 0 @ sys
    on pix { line = line +% 1 }
    on sys { count = count +% 1 }
    y = count ^ line
}
```

```
mixed.tern:12:17: error: pix-domain signal `line` used in sys-domain logic; cross with std::cdc
   |
12 |     y = count ^ line
   |                 ^^^^
   = help: use std::cdc::Sync2 for a level, PulseSync for a pulse, HandshakeSync or AsyncFifo for data, or unsafe_cross(x) for a quasi-static value
  in: Mixed
```

## Crossing domains

The only legal way across a domain boundary is a `std::cdc` module (or the
`unsafe_cross` escape hatch). The compiler recognises these primitives, types
their outputs in the destination domain, and carries the right synthesis
attributes (`ASYNC_REG`, false paths) into the emitted SystemVerilog and an
`.xdc`/`.sdc` constraints fragment.

| Primitive | Use |
|---|---|
| `Sync2<T = bool>`, `Sync3<T>` | a level signal, 2 or 3 flops; `T` may be a bundle of independent bits |
| `PulseSync` | a single-cycle pulse, toggle-based |
| `HandshakeSync<T>` | a multi-bit value with req/ack; slow but safe |
| `AsyncFifo<T, Depth>` | streaming data, Gray-coded pointers |
| `ResetSync` | asynchronous assert, synchronous de-assert of a reset into a domain |
| `ResetBridge` | derives a reset in one domain from a reset in another |
| `GrayCounter<W>` | a counter whose value is read in another domain |
| `unsafe_cross(x)` | escape hatch for quasi-static configuration; a warning and a `// CDC: unchecked` marker |

```tern
package cdc

use std::cdc::{Sync2, PulseSync, AsyncFifo}

#[top]
module Bridge {
    in a_clk: clock
    in a_rst: reset
    in b_clk: clock
    in b_rst: reset
    in push: Stream<u8> @ a
    in flag: bool @ a
    in kick: bool @ a
    in mode: uint<2> @ a
    out pop: Stream<u8> @ b
    out flag_b: bool @ b
    out kick_b: bool @ b
    out mode_b: uint<2> @ b

    domain a = (a_clk, a_rst)
    domain b = (b_clk, b_rst)

    // a stream of data
    let fifo = AsyncFifo<u8, 16>(
        src: a.clock,
        src_rst: a.reset,
        dst: b.clock,
        dst_rst: b.reset,
        push,
    )
    pop = fifo.pop

    // a level and a pulse
    let s = Sync2(src: a.clock, dst: b.clock, d: flag)
    flag_b = s.q
    let p = PulseSync(src: a.clock, src_rst: a.reset, dst: b.clock, dst_rst: b.reset, d: kick)
    kick_b = p.q

    // a configuration value that only changes while the design is idle
    reg m: uint<2> = 0 @ b
    on b { m = unsafe_cross(mode) }
    mode_b = m
}
```

A raw asynchronous reset is brought into each domain with `ResetSync`
(asynchronous assertion, synchronous release). Its output can then be the
reset of a domain:

```tern
package resetsync

use std::cdc::ResetSync

#[top]
module Resets {
    in sys_clk: clock
    in pix_clk: clock
    in ext_rst: reset<async>
    out a: u8 @ sys
    out b: u8 @ pix

    let rs = ResetSync(clk: sys_clk, rst_in: ext_rst)
    domain sys = (sys_clk, rs.rst_out)
    let rp = ResetSync(clk: pix_clk, rst_in: ext_rst)
    domain pix = (pix_clk, rp.rst_out)

    reg ca: u8 = 0 @ sys
    on sys { ca = ca +% 1 }
    reg cb: u8 = 0 @ pix
    on pix { cb = cb +% 1 }
    a = ca
    b = cb
}
```

`tern check` lists every crossing of the `Bridge` example above whenever the
design has any (with `--cdc-report` also when it has none). This is the CDC
report most teams write by hand. `unsafe_cross` adds a warning, which
`--strict` turns into an error:

```
bridge.tern:41:16: warning: unchecked clock-domain crossing (`unsafe_cross`)
   |
41 |     on b { m = unsafe_cross(mode) }
   |                ^^^^^^^^^^^^^^^^^^
  in: Bridge

CDC report: 4 crossings
  Bridge > fifo: async_fifo, a -> b, signal push.valid (bridge.tern:24)
  Bridge > s: sync2, a -> b, signal flag (bridge.tern:34)
  Bridge > p: pulse, a -> b, signal kick (bridge.tern:36)
  Bridge: unsafe, a -> b, signal mode (bridge.tern:41)
```

## Testing multi-clock designs

In a test, a module with several domains is stepped per domain, by the name
of the domain or of its clock port (`dut.a.step()` or `dut.a_clk.step()`).
`dut.reset()` resets every domain. `dut.run(ns: 500)`
advances all domains together, using the periods from `#[clock(hz: N)]`:

```tern,test
package twoclocks

use std::cdc::Sync2

module Crossing {
    in a_clk: clock
    in b_clk: clock
    in d: bool @ a
    out q: bool @ b
    domain a = (a_clk)
    domain b = (b_clk)
    let s = Sync2(src: a.clock, dst: b.clock, d)
    q = s.q
}

test "a level arrives after two destination clocks" {
    let dut = sim Crossing()
    dut.d = true
    dut.a_clk.step()
    dut.b_clk.step()
    expect !dut.q
    dut.b_clk.step()
    expect dut.q
}
```

Next: [Compile-time evaluation](05-compile-time.md).
