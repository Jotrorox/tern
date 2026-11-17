# Tern standard library

`std` is written in Tern, ships with the compiler (embedded by
`tools/embed.c`) and follows spec Chapter 12. Each package `std::<pkg>`
lives in `std/<pkg>/`; a package may be split over several `.tern` files in
that directory, all starting with `package std::<pkg>`.

## Layout

| Package | Files | Contents |
|---|---|---|
| `std::prelude` | `prelude/prelude.tern` | `Valid<T>`, `Stream<T>` (with `fire()`), `u8..u64`, `s8..s64`, `clog2`, `count_ones` |
| `std::stream` | `stream/stream.tern`, `route.tern`, `packet.tern` | `Beat<T>`, `Pair`, `Triple`, `Skid`, `Pipe`, `Throttle`, `Fork` (`.Sync`/`.Eager`), `Join`, `Join3`, `Arbiter` (`ArbPolicy`: `.RoundRobin`/`.Fixed`/`.Weighted<[uint; N]>`, + `arb_weights`), `Mux`, `Demux`, `Gearbox`, `GearboxLast`, `Packetizer`/`Depacketizer` (`Framing`: `.Count<Len>`/`.Delim<Value>`/`.Flag`), `DelimPacketizer`, `DelimDepacketizer` (runtime delimiter), `StreamCounter` |
| `std::fifo` | `fifo/fifo.tern`, `async.tern`, `packet.tern` | `Fifo` (block RAM, look-ahead read), `FifoFwft` (LUT RAM), `ShiftFifo`, `Lifo`, `AsyncFifo`, `PacketFifo` |
| `std::cdc` | `cdc/cdc.tern` | `Sync2`, `Sync3`, `PulseSync`, `HandshakeSync`, `ResetSync`, `ResetBridge`, `GrayCounter`, `AsyncFifo` (re-export) |
| `std::math` | `math/math.tern`, `arith.tern`, `cordic.tern`, `fixed.tern`, `lfsr.tern` | functions (`flog2`, `is_pow2`, `gray`/`ungray`, `leading_zeros`, `trailing_zeros`, `priority_encode`, `onehot`/`onehot_to_bin`, `reverse_bits`, `min`/`max`/`smin`/`smax`/`abs`, `sat_add`/`sat_sub`(`_s`), `widen_signed`, `round_half_up`, `round_even`), `Operands`, `Multiplier`, `SignedMultiplier`, `Divider`, `Sqrt`, `Mac`, `Cordic` (`.Rotate`/`.Vector`/`.Hyperbolic`), `Fixed<I, F>` + `fx_*` functions + `FixedDivider`, `Lfsr` + `lfsr_taps` |
| `std::mem` | `mem/mem.tern` | `RomTable`, `SinglePortRam`, `SimpleDualPortRam`, `TrueDualPortRam`, `RegFile`, `Cam`, `ShiftReg`, `DelayLine`, `Lut` |
| `std::crc` | `crc/crc.tern` | `crc_update`, `Crc` (bit/byte/word-parallel through the payload type), presets `Crc8`, `Crc16Ccitt`, `Crc16Modbus`, `Crc32`, `Crc32C` (`clear` defaults to false), `Scrambler<Poly, T>`, `Lfsr` (re-export) |
| `std::encoding` | `encoding/encoding.tern`, `line.tern`, `ecc.tern` | `Enc8b10b`/`Dec8b10b` (+ `encode_8b10b`/`decode_8b10b`), `ManchesterEnc`/`ManchesterDec`, `Nrzi`/`NrziDec`, `HammingEnc`/`HammingDec` (SECDED, + `hamming_encode`/`hamming_decode`), `Parity`/`ParityCheck`, `PriorityMux`, `base64_char`/`base64_value` |
| `std::clk` | `clk/clk.tern` | `Divider`, `Enable`, `Gate`, `Strobe<Hz>`, `Watchdog` |
| `std::timer` | `timer/timer.tern` | `Timer`, `Prescaler`, `Pulse`, `Stopwatch`, `Rtc`, `Watchdog` (re-export) |
| `std::debug` | `debug/debug.tern` | `Ila`, `IlaUart`, `IlaAxiLite` (+ the compiler's `#[probe]`), `Heartbeat`, `ErrorLatch` |

`std::io`, `std::bus`, `std::dsp`, `std::video`, `std::sim` and
`std::target` are maintained separately.

Package dependencies (no cycles): `fifo -> stream, math`; `cdc -> fifo`;
`mem, crc, encoding -> math`; `timer -> clk`; `debug -> stream, math, io,
bus`.

## Conventions

* Every component is generic over its payload (`T: type` first) and sized by
  `uint` generics constrained with `where`. Exceptions follow the spec's
  own tables: `Crc<W, Poly, Init, RefIn, RefOut, XorOut, T>`,
  `Scrambler<Poly, T>`, `Cic<Stages, R, T>` and `Fft<N, T>`.
* Components that need their clock's frequency take it as a generic
  `ClkHz: uint = clock_hz(clk)` (spec §7.5): the top's `#[clock(hz: ..)]`
  is passed down through the instances, and an explicit `ClkHz` overrides
  it (tests do that when they simulate the component on its own).
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

## Spec §12 audit

Every package and component of spec §12.2 (and the rules of §12.1 and the
composition of §12.3), checked against this tree. Status: **ok** = present
with the spec's name and generic signature (extra generics after the
spec's ones have defaults unless noted); **deviation** = present under
another shape, with the reason. Part 2 (io, bus, dsp, video, sim, target)
is audited in [README-part2.md](README-part2.md).

| Spec | std | Status |
|---|---|---|
| §12.1 rule 1: payload type first, `uint` generics with `where` | all components | ok; exceptions where the spec's table orders differently (Crc, Scrambler, Cic, Fft) |
| §12.1 rule 2: `Stream<T>` / `Valid<T>` between components | all components | ok |
| §12.1 rule 3: published `LATENCY` | all components of part 1 | ok (`RegisterFile` gets the front-end's from the compiler) |
| §12.1 rule 4: a test per behaviour, a property per invariant | `tern test std` | ok |
| §12.1 rule 5/6/7: attributes, vendor-free, naming | all | ok |
| **std::prelude** `Valid<T>`, `Stream<T>` (`fire`, `map(f)`, `skid()`) | prelude, compiler | ok (`map`/`skid` are compiler builtins on `Stream`) |
| `u8..u64`, `s8..s64`, `clog2`, `count_ones` | prelude | ok |
| `bits`, `all0`/`all1`, `static_assert`, `typeof` | compiler builtins | ok |
| **std::stream** `Skid<T>` (LATENCY = 1) | `Skid<T>` | ok |
| `Pipe<T, Stages>` | `Pipe<T, Stages = 1>` | ok |
| `Fork<T, N>`, policy `.Eager` | `Fork<T, N, Policy: ForkPolicy = .Sync>` | ok |
| `Join<A, B>` / `Join3` | `Join<A, B>`, `Join3<A, B, C>` | ok |
| `Arbiter<T, N, Policy>`: `.RoundRobin` (default), `.Fixed`, `.Weighted<[uint; N]>` | `Arbiter<T, N, Policy: ArbPolicy<N> = .RoundRobin>`, `Arbiter<u8, 3, .Weighted<[3, 1, 2]>>` | ok (weighted round-robin; payload variants as generic values are a compiler feature) |
| `Mux<T, N>` / `Demux<T, N>` | same | ok |
| `Gearbox<In, Out>` with `last` support | `Gearbox<In, Out>` and `GearboxLast<In, Out>` (on `Stream<Beat<..>>`) | deviation: the last-aware variant is a separate module, because a port type cannot depend on a generic test of the payload type |
| `Throttle` | `Throttle<T, N>` | ok |
| `Packetizer<T>` / `Depacketizer<T>`, by count or delimiter | `Packetizer<T, By: Framing<T> = .Delim<all0>>`, `Depacketizer<T, By = .Flag>` with `.Count<Len>` / `.Delim<Value>`; `DelimPacketizer` / `DelimDepacketizer` take the delimiter as a port | ok |
| `StreamCounter` (beats and packets as `Valid`) | `StreamCounter<T, W = 32>` | ok |
| **std::fifo** `Fifo<T, Depth>` (registered output, `count`, `almost_full<N>`) | `Fifo<T, Depth = 16, AlmostFull = Depth - 1>` | ok |
| `FifoFwft`, `AsyncFifo`, `Lifo`, `PacketFifo` `<T, Depth>` | same | ok (`AsyncFifo`'s resets default to the domains of `src` / `dst`) |
| `ShiftFifo<T, Depth>`, depth <= 8 | `where Depth <= 8` | ok |
| **std::cdc** `Sync2<T>`, `Sync3<T>`, `PulseSync`, `HandshakeSync<T>`, `ResetSync`, `ResetBridge`, `GrayCounter<W>`, `AsyncFifo` (re-export), `unsafe_cross` | same (`unsafe_cross` is a compiler builtin) | ok |
| **std::math** functions `clog2` ... `round_half_up`/`round_even` | `math.tern` | ok (plus `reverse_bits`, `smin`/`smax`, `sat_add_s`/`sat_sub_s`) |
| `Multiplier<N, M, Stages>` | same (+ `SignedMultiplier`) | ok |
| `Divider<N, Stages>`, `Sqrt<N>`, `Mac<N, M, Acc>` | same (`Sqrt<N, Pipelined>`, `Mac<.., Saturate>`) | ok |
| `Cordic<W, Mode>`: `.Rotate`, `.Vector`, `.Hyperbolic` | same | ok |
| `Fixed<I, F>` with `+ - * /`, rounding modes, conversions | `Fixed<I, F>`, `fx_add`/`fx_sub`/`fx_neg`/`fx_mul`, `FixedDivider`, `fx_convert<I2, F2, Round, Sat>` | ok (the language has no operator overloading, so the operators are functions; `/` takes several cycles and is a module) |
| `Lfsr<W, Poly>`, Fibonacci/Galois, maximal-length presets | `Lfsr<W, Poly = lfsr_taps(W), Galois, Seed>` | ok |
| **std::mem** `RomTable<T, N>`, `SinglePortRam`, `SimpleDualPortRam`, `TrueDualPortRam`, `RegFile<T, N, Rd, Wr>`, `Cam<T, N>`, `ShiftReg<T, N>`, `DelayLine<T, N>` | same (`RomTable<T, N, Data>`) | ok |
| `Lut<In, Out>` from a const fn | `Lut<In, Out, Table>`, `Table` the result of a const fn call (`Lut<u4, u8, square_table()>`) | deviation: generics cannot be functions, so the const fn is called in the argument |
| **std::crc** `Crc<W, Poly, Init, RefIn, RefOut, XorOut>`, byte- and word-parallel | `Crc<W, Poly, Init, RefIn, RefOut, XorOut, T: type = u8>` (parallelism through `T`) | ok |
| presets `Crc8`, `Crc16Ccitt`, `Crc16Modbus`, `Crc32`, `Crc32C` | same, `<T = u8>`, `T` inferred from `in` | ok (§12.3's `Crc32(in: xfifo.pop)` compiles as written: `clear` defaults to false) |
| `Scrambler<Poly>`, `Lfsr` (re-export) | `Scrambler<Poly = x^58 + x^39 + 1, T = u8, Descramble = false>` | ok |
| **std::encoding** `Enc8b10b` / `Dec8b10b` | same | ok |
| `Manchester`, `Nrzi` | `ManchesterEnc`/`ManchesterDec`, `Nrzi`/`NrziDec` | deviation: one module per direction, as for 8b/10b (the two have different port types) |
| `Hamming<K>` (SECDED encoder/decoder) | `HammingEnc<K>` / `HammingDec<K>` (+ `hamming_encode`/`hamming_decode`) | deviation: an encoder/decoder pair, same reason |
| `Parity` | `Parity<T, Odd>` / `ParityCheck<T, Odd>` | ok |
| `PriorityMux`, `Base64` (test tooling) | `PriorityMux<T, N>`, `base64_char`/`base64_value` | ok |
| **std::clk** `Divider<N>`, `Enable`, `Gate`, `Strobe<Hz>`, `Watchdog<W>` | same; `Strobe<Hz, ClkHz = clock_hz(clk)>` | ok (the input rate comes from the clock's `#[clock(hz)]`; `Watchdog` moved here from std::timer, which re-exports it) |
| **std::timer** `Timer<W>`, `Prescaler`, `Pulse<Cycles>`, `Stopwatch`, `Rtc` | same; `Rtc<ClkHz = clock_hz(clk)>` | ok |
| **std::debug** `Ila<Probes, Depth>`, read out over UART or AXI-Lite | `Ila<Probes, Depth, Pre>`, `IlaUart<Probes, Depth, Pre, Baud, ClkHz>`, `IlaAxiLite<Probes, Depth, Pre, AW, DW>` | ok |
| `#[probe]` attribute | compiler: collects a module's probed signals into the type `Probes` and the value `probes` (`Ila<Probes>(probes: probes, ..)`), emitted with `mark_debug` | ok |
| `Heartbeat`, `ErrorLatch<N>` | `Heartbeat<Hz = 1, ClkHz = clock_hz(clk)>`, `ErrorLatch<N>` | ok |
| **§12.3** composition, four lines | `tests/fixtures/spec/s12_3_composition.tern` | ok, as written |

The notes this file used to carry for the compiler are all resolved:
generic inference from port connections (`Crc32(in: ..)`), `AsyncFifo`'s
resets from the `src`/`dst` domains, `Stream::map(f)` / `Stream::skid()`,
`Arbiter`'s `.Weighted` policy (enum variants with a compile-time payload
as generic values) and the clock frequency (`clock_hz`). What remains are
the deviations marked above.

## Language features used

Syntax and semantics std relies on beyond the plain spec examples (all
implemented by the compiler; docs/grammar.md describes the syntax):

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
30. Enum variants with a compile-time payload declared generic-style
    (`Weighted<Weights: [uint; N]>`), written `.Weighted<[3, 1, 2]>` as a
    generic argument, matched in const fns (`.Weighted { Weights } => ..`).
31. `clock_hz(clk)` in generic defaults (`ClkHz: uint = clock_hz(clk)`).
32. Input port defaults (`in clear: bool = false`).
33. `#[probe]` and the generated `Probes` / `probes`.
34. `dut.expect_eventually(cond, max: n)` in tests.
