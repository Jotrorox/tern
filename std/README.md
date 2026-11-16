# Tern standard library

`std` is written in Tern, ships with the compiler (embedded by
`tools/embed.c`) and follows spec Chapter 12. Each package `std::<pkg>`
lives in `std/<pkg>/`; a package may be split over several `.tern` files in
that directory, all starting with `package std::<pkg>`.

## Layout

| Package | Files | Contents |
|---|---|---|
| `std::prelude` | `prelude/prelude.tern` | `Valid<T>`, `Stream<T>` (with `fire()`), `u8..u64`, `s8..s64`, `clog2`, `count_ones` |
| `std::stream` | `stream/stream.tern`, `route.tern`, `packet.tern` | `Beat<T>`, `Pair`, `Triple`, `Skid`, `Pipe`, `Throttle`, `Fork` (`.Sync`/`.Eager`), `Join`, `Join3`, `Arbiter` (`.RoundRobin`/`.Fixed`), `Mux`, `Demux`, `Gearbox`, `GearboxLast`, `Packetizer`, `DelimPacketizer`, `Depacketizer`, `DelimDepacketizer`, `StreamCounter` |
| `std::fifo` | `fifo/fifo.tern`, `async.tern`, `packet.tern` | `Fifo` (block RAM, look-ahead read), `FifoFwft` (LUT RAM), `ShiftFifo`, `Lifo`, `AsyncFifo`, `PacketFifo` |
| `std::cdc` | `cdc/cdc.tern` | `Sync2`, `Sync3`, `PulseSync`, `HandshakeSync`, `ResetSync`, `ResetBridge`, `GrayCounter`, `AsyncFifo` (re-export) |
| `std::math` | `math/math.tern`, `arith.tern`, `cordic.tern`, `fixed.tern`, `lfsr.tern` | functions (`flog2`, `is_pow2`, `gray`/`ungray`, `leading_zeros`, `trailing_zeros`, `priority_encode`, `onehot`/`onehot_to_bin`, `reverse_bits`, `min`/`max`/`smin`/`smax`/`abs`, `sat_add`/`sat_sub`(`_s`), `widen_signed`, `round_half_up`, `round_even`), `Operands`, `Multiplier`, `SignedMultiplier`, `Divider`, `Sqrt`, `Mac`, `Cordic` (`.Rotate`/`.Vector`/`.Hyperbolic`), `Fixed<I, F>` + `fx_*` functions + `FixedDivider`, `Lfsr` + `lfsr_taps` |
| `std::mem` | `mem/mem.tern` | `RomTable`, `SinglePortRam`, `SimpleDualPortRam`, `TrueDualPortRam`, `RegFile`, `Cam`, `ShiftReg`, `DelayLine`, `Lut` |
| `std::crc` | `crc/crc.tern` | `crc_update`, `Crc` (bit/byte/word-parallel through the payload type), presets `Crc8`, `Crc16Ccitt`, `Crc16Modbus`, `Crc32`, `Crc32C`, `Scrambler`, `Lfsr` (re-export) |
| `std::encoding` | `encoding/encoding.tern`, `line.tern`, `ecc.tern` | `Enc8b10b`/`Dec8b10b` (+ `encode_8b10b`/`decode_8b10b`), `ManchesterEnc`/`ManchesterDec`, `Nrzi`/`NrziDec`, `HammingEnc`/`HammingDec` (SECDED, + `hamming_encode`/`hamming_decode`), `Parity`/`ParityCheck`, `PriorityMux`, `base64_char`/`base64_value` |
| `std::clk` | `clk/clk.tern` | `Divider`, `Enable`, `Gate`, `Strobe` |
| `std::timer` | `timer/timer.tern` | `Timer`, `Prescaler`, `Pulse`, `Stopwatch`, `Rtc`, `Watchdog` |
| `std::debug` | `debug/debug.tern` | `Ila`, `Heartbeat`, `ErrorLatch` |

`std::io`, `std::bus`, `std::dsp`, `std::video`, `std::sim` and
`std::target` are maintained separately.

Package dependencies (no cycles): `fifo -> stream, math`; `cdc -> fifo`;
`mem, crc, encoding -> math`; `debug -> stream, math`.

## Conventions

* Every component is generic over its payload (`T: type` first) and sized by
  `uint` generics constrained with `where`.
* Handshakes are `Stream<T>` (valid/ready/data) or `Valid<T>`. Input streams
  are named `in` or `push`, output streams `out` or `pop`. A producer holds
  `valid`/`data` until the beat fires, and no `valid` output depends
  combinationally on a `ready` input of the same port; most stateful
  components have a `property` saying so (`out_stable`, `pop_stable`).
* Every module publishes `pub const LATENCY: uint` (some also `CHECK`,
  `GAIN_COMP`, `ITERS`).
* Every pub item and every port has a `///` doc comment. Every documented
  behaviour has a `test`; every sequential component has at least one
  `property` for its main invariant.
* Single-clock modules name their ports `clk: clock` and `rst: reset`.
  Memories and synchronizers have no reset port. Multi-clock modules use
  `src`/`src_rst` and `dst`/`dst_rst` with `domain src_d = (src, src_rst)`
  and `domain dst_d = (dst, dst_rst)`.
* Data registers are deliberately reset-less (only control state is reset);
  their contents are only observed while qualified by a valid bit.
* Packet streams are `Stream<Beat<T>>` (`Beat { data, last }`), defined in
  `std::stream`.
* Width conversion is little-endian: the first narrow beat is the least
  significant lane of the wide word (Gearbox, Crc word-parallel input).

## Notes for the compiler team (deviations / open points)

* `AsyncFifo` has explicit `src_rst`/`dst_rst` ports, so the §12.3 example
  `AsyncFifo<u32, 512>(push: .., src: io.clock, dst: sys.clock)` would also
  need them connected (or a rule that fills them from the named domains).
* CRC presets default to `T = u8`; §12.3's `Crc32(in: xfifo.pop)` on a
  `Stream<u32>` needs `Crc32<u32>(...)` unless generic arguments are inferred
  from port connections.
* `Stream::map(f)` / `Stream::skid()` (§8.1) are not provided: they need a
  higher-order fn and instantiation from a bundle fn. `Arbiter`'s
  `.Weighted` policy is not implemented.
* `Strobe`, `Rtc` and `Heartbeat` take `ClkHz` as an explicit generic: there
  is no syntax yet to read a clock port's `#[clock(hz: ..)]`.
* `Lut<In, Out, Table>` takes the table as an array generic, typically the
  result of a const fn call, since fn-valued generics do not exist.
* Gearbox "last support" is the separate module `GearboxLast`.

## Language features used

Syntax and semantics std relies on beyond the plain spec examples (please
check each is supported or tell us the intended spelling):

1. `in` / `out` as **port names** (`in in: Stream<T>`, `out out: Stream<T>`),
   and therefore as the root of expressions and assignment targets
   (`in.ready = ..`, `out.valid = ..`, `in[i].valid`, `out[i].ready`), as an
   argument value (`Crc<..>(in: in, clear: clear)`) and in tests
   (`dut.in = ..`, `dut.out.ready`). A statement starting with `in`/`out`
   followed by `.`, `[` or `=` is an assignment, not a port declaration.
2. `pub const` inside a module body (`pub const LATENCY: uint = ..`), and
   reading it as `Module<args>::NAME` (`Cordic<16>::GAIN_COMP`).
3. Arrays of bundles and of scalars as ports: `in in: [Stream<T>; N]`,
   `out out: [Stream<T>; N]`, `in in: [Valid<T>; N]`,
   `in raddr: [uint<clog2(N)>; Rd]`.
4. Generic parameters of **enum type** with variant defaults
   (`Policy: ArbPolicy = .RoundRobin`, `R: Round = .Truncate`), compared at
   elaboration (`if Policy == .RoundRobin`) or matched (`match R { .. }`).
5. Generic parameters of **array type** (`Data: [T; N]`,
   `Table: [Out; 1 << In::WIDTH]`) with array literals or const fn calls as
   arguments (`RomTable<u8, 4, [10, 20, 30, 40]>`, `Lut<uint<4>, u8, square_table()>`).
6. Generic defaults that refer to earlier generics or call const fns
   (`AlmostFull: uint = Depth - 1`, `M: uint = N`, `Pre: uint = Depth / 2`,
   `Poly: uint = lfsr_taps(W)`), and named generic arguments that skip
   defaulted ones (`Lfsr<8, Galois: true>`).
7. Generic structs, including `uint` generics (`Beat<T>`, `Operands<A, B>`,
   `DivResult<N: uint>`, `Fixed<I: uint, F: uint>`, `HammingResult<K>`),
   and struct literals whose generic arguments are inferred from context
   (`Fixed { raw: x }`, `Beat { data: d, last: l }`).
8. `///` doc comments on struct fields and enum variants; trailing commas in
   struct and enum bodies (one field / variant per line).
9. Module-local `struct`, `enum`, `type`, `const` and `fn` items that use the
   module's generics (`struct St` and `fn step` in `Sqrt`, `fn run` in
   `Scrambler`, `enum State` in `Ila`, `type Ptr = uint<AW + 1>`).
10. Functions with explicit generics plus inferred width generics:
    `round_even<F>(x)`, `crc_update<Poly, RefIn>(c, d)`,
    `hamming_decode<K>(c)`, `fx_convert<I2, F2, .HalfEven, true>(a)`
    (fn generics may have defaults); return types computed from generics
    (`bit<hamming_n(K)>`, `bit<1 << N>`, `Fixed<IA + IB, FA + FB>`).
11. `var` plus `for` accumulation inside `const fn`s that are also used on
    signals (and inside the prelude's plain `fn count_ones`, as agreed).
12. Elaboration-time `if` on generics inside `fn` bodies and inside
    `on`/`comb` blocks, with only the taken branch elaborated (branches may
    contain widths that are invalid for other parameter values, e.g.
    `uint<F - F2>` in `fx_convert`). `static_assert` inside fn bodies and
    inside elaboration `if` branches.
13. Elaboration `if`/`for` at module level containing `reg`, `var`, `const`,
    `on`, `comb` and `property` (Gearbox declares `const R` in two branches;
    Pipe/Multiplier/Cordic declare a `reg` and an `on` block per iteration,
    as in the spec's `Pipeline` example, chained through `var` arrays
    assigned element-wise at module level).
14. Bit-index assignment (`done[i] = true` on a `reg bit<N>`, `r[k] = ..` on
    a `var bit<N>`), struct-field assignment on registers (`t.seconds = 0`
    inside `on`), constant-index assignment of `reg` array elements
    (`q[i] = ..`, `lanes[i] = ..`) and dynamic-index reads of `reg` arrays
    (`regs[raddr[r]]`).
15. `mem` initialised from a generic or const (`mem rom: [T; N] = Data`),
    `#[ram(distributed)]` on the line before `mem`, `m.read(addr, rdw: .OldData)`
    / `.NewData`, mem indices obtained by truncating a wider pointer
    (`buf[wr_ptr as Addr]`).
16. Casts: `x as uint<N>` to truncate pointers; `bit<N> as sint<N>`
    reinterpretation; compile-time integers to bits (`Poly as bit<W>`,
    `(1 << k) as bit<W>`); `b as uint<1>` to concatenate a bool;
    widthless `(1 << F) as sint` in const fns.
17. Reductions on arbitrary expressions: `^(s & P)`, `&bits(cnt)`, `|x`.
18. `T::from_bits(all0)` as the reset / power-on value of a generic-typed
    register; struct literals as reset values.
19. Multi-clock modules: `domain name = (clock, reset)` declarations, ports
    annotated with a **domain name** (`in push: Stream<T> @ src_d`),
    `reg .. @ src_d`, and **`on <domain>`** (an `on` block naming a domain
    rather than a clock port). Reset-less synchronizers use a clock port name
    directly (`reg s1: T @ dst`, `on dst`, `in d: T @ src`).
20. `#[cdc(sync2|sync3|pulse|handshake|async_fifo|reset_sync|reset_bridge|gray)]`
    on modules and `#[async_reg]` on the line before a `reg`.
21. Clock/reset conversions, used only inside recognised primitives:
    `bool as reset<async>` and `reset as reset<async>` (in
    `#[cdc(reset_sync)]` / `#[cdc(reset_bridge)]`), `clock as clock<falling>`,
    `clock as bool`, `bool as clock` (in the new attributes
    `#[clock_gen]` (`std::clk::Divider`) and `#[clock_gate]`
    (`std::clk::Gate`)). `domain` declarations whose clock or reset is such a
    converted `let` (`domain fall = (clk_n)`, `domain d = (dst, src_async)`).
22. Output ports of type `reset<async>` and `clock`.
23. Re-exports: `pub use std::fifo::AsyncFifo`,
    `pub use std::prelude::{clog2, count_ones}`.
24. Properties: inside elaboration `if`; `never expr`; `past(x, n)` with a
    constant `n`; `!stable(x)`; sequences on the left of an implication
    (`a ##1 b |=> c`); `assume property` and `cover property` with
    `##[1:$]`; doc comments on properties.
25. Typed literals with arbitrary suffix widths (`64u8`, `-3s8`, `255s9`,
    `2u3`).
26. `const fn` details: `match` on a widthless integer with literal arms and
    `_`; widthless `sint` parameters; `let` bindings of compile-time values
    used in later widths (`let r = hamming_r(K)`); `for` over a range whose
    bound is a parameter; `x.is_pow2()` on a loop index.
27. `if` expressions whose arms mix an untyped literal or `all0` with a typed
    value; typed `let` (`let top: uint<N> = all1`).
28. Ordinary identifiers that are words elsewhere: `wait`, `left`, `first`,
    `last`, `done`, `gray`, `bin`, `line`, `parity_bit`.
29. Tests: several `sim` handles in one test (encoder feeding decoder);
    `dut.<clock>.step()` for multi-clock modules; `step(n)`;
    `step_until(cond, max)`; `rand<T>() where cond`; tuple destructuring in
    `for (a, b, c) in [..]`; indexing test-level arrays with test-time
    integers; `var` in tests; reading internal registers by name
    (`dut.q`, `dut.en_l`); copying a sim's output bundle into another sim's
    input (`dec.in = enc.out`); nested field reads (`dut.out.data.sym.k`);
    assigning to elements of array ports (`dut.in[i] = Stream { .. }`);
    calling const fns directly in tests; `dut.reset()` on multi-domain
    modules resets every domain.
