# std, part 2: io, bus, dsp, video, sim, target

These packages are written in Tern against spec v0.1 and the conventions agreed
with the compiler team. Nothing here has been compiled yet. Every package has
`test` blocks, and every sequential component has at least one `property`.

| Package | Files | Components |
|---|---|---|
| `std::io` | `std/io/*.tern` | UartTxCore, UartRxCore, UartTx, UartRx, Uart; SpiMaster, SpiSlave, QspiMaster (+QspiCmd); I2cMaster (+I2cCmd, I2cResult), I2cSlave; Pwm; Debounce; EdgeDetect; QuadratureDecoder; Ws2812; SevenSegment (+hex_to_segments); Ps2; OneWire (+OneWireCmd, OneWireResult); Sdio, an SPI-mode SD card engine (+SdCmd, SdError, sd_crc7, sd_frame) |
| `std::bus` | `std/bus/*.tern` | bundles Axi4 (+Axi4Addr/W/B/R), Axi4Lite, Axi4Stream, Apb, Ahb, Wishbone, TileLinkUL (+TileLinkA/D); RegReq/RegRsp, Axi4LiteSlave, Axi4LiteMaster; RegBus, Axi4LiteRegPort, ApbRegPort, WishboneRegPort, RegisterFile; Axi4LiteToApb, Axi4ToAxi4Lite, ApbBridge, WishboneToAxi4Lite; Axi4Crossbar, Axi4Interconnect, WishboneArbiter; AxiDma (+DmaCmd); axi_next_addr |
| `std::dsp` | `std/dsp/*.tern` | Fir, IirBiquad, Cic, Nco, MovingAverage, Accumulator, Fft, Complex, sin/cos const fns, bit_reverse, `pub use std::math::Fixed` |
| `std::video` | `std/video/*.tern` | VideoMode and its presets (Vga640x480, Svga800x600, Xga1024x768, Hd1280x720, Hd1920x1080), VgaTiming, Tmds (+tmds_transition, tmds_decode, tmds_is_control), Framebuffer, TextMode, Rgb888/Rgb565 and conversions |
| `std::sim` | `std/sim/*.tern` | StreamDriver, StreamMonitor, Scoreboard, ExpectEventually, ClockModel, lfsr16_next, Axi4LiteDriver (+AxiLiteOp, axil_write, axil_read), Axi4StreamDriver, UartModel, SpiModel, I2cModel |
| `std::target` | `std/target/target.tern` | exact_period_ns, period_ps |
| `std::target::xilinx` | `std/target/xilinx/xilinx.tern` | Mmcm, Pll, Bufg, Iddr, Oddr, Iobuf, Idelay, IdelayCtrl, Startup; externs MMCME2_BASE, PLLE2_BASE, BUFG, IDDR, ODDR, IOBUF, IDELAYE2, IDELAYCTRL, STARTUPE2 |
| `std::target::lattice` | `std/target/lattice/lattice.tern` | SbPll, SbIo, EcpPll, EcpIo, ice40_filter_range; externs SB_PLL40_CORE, SB_IO, EHXPLLL, BB |
| `std::target::intel` | `std/target/intel/intel.tern` | Pll, AltIobuf, DdioIn, DdioOut; externs altpll, ALT_IOBUF, altddio_in, altddio_out |
| `std::target::generic` | `std/target/generic/generic.tern` | behavioural fallbacks with the same names: Pll, Mmcm, SbPll, EcpPll, Bufg, Iddr, Oddr, Iobuf, SbIo, EcpIo, AltIobuf, DdioIn, DdioOut, Idelay, IdelayCtrl, Startup |

## Notes for the compiler team

* **`std::bus::RegisterFile<Map, Bus>`** is compiler-assisted. Only its
  common ports (clk, rst, bus) are declared. The compiler has to:
  * pick the bus front-end that matches `Bus` (Axi4LiteRegPort, ApbRegPort or
    WishboneRegPort, all written in Tern and producing a `RegBus`);
  * generate the per-register ports and the decode from `Map`.

  The tests in `std/bus/regfile.tern` use a demo `regmap` to show the
  expected behaviour.
* **Rule exceptions:**
  * `Cic<Stages, R>` and `Fft<N, T>` keep the generic order the spec gives
    them, so the payload type is not their first generic.
  * Fir, IirBiquad and the other dsp modules compute on
    `bits(x) as sint<T::WIDTH>` and document this. They do this because
    arithmetic on an arbitrary `T: type` is not defined.
* **`step_until` timing:** some tests assume `dut.step_until(cond, max: n)`
  returns without stepping when `cond` already holds.
* **Vendor externs:** these use `extern module NAME("")`.
  * SystemVerilog parameters that are real-valued get integer values.
  * String parameters are not declared and stay at their library defaults.
  * Vendor ports that are not needed are left out, so they stay unconnected
    in the emitted instance.

## Language features used

This list covers syntax and semantics these packages rely on beyond the plain
spec examples. Items marked (spec) are named in the spec but not shown in an
example.

### Names, items, packages
- A port named with the keyword `in`, both as declaration and as a named argument: `in in: Stream<T>`, `in.valid`, `in.fire()`, `M(in: x)`. (spec §12.1/§12.3 use it.)
- `pub use std::math::Fixed` re-export (not in the grammar).
- Subpackages: `std::target::xilinx` lives in `std/target/xilinx/`, and the parent `std::target` lives in `std/target/target.tern`. Items of sibling std packages are imported with `use std::bus::{Axi4Lite, RegReq}` and `use std::target::exact_period_ns`.
- Private, non-`pub` modules and regmaps in std files that exist only as test benches (`UartDecodeBench`, `SpiPair<Mode>`, `SdBench`, `regmap DemoRegs`).
- Lower-case and all-caps module, generic and port names on vendor externs (`altpll`, `MMCME2_BASE`, `CLKIN1`) so they match the vendor library.
- Doc comments (`///`) on ports, struct fields, enum variants, properties, consts and extern ports. Non-ASCII characters (em dash, §, ±, µ) appear only in comments.

### Generics and compile-time code
- `where` clauses with several comma-separated constraints, using `and`, `or`, arithmetic and `.is_pow2()`. Examples: `where DW == 32 or DW == 64`, `(Base / Window) * Window == Base`, `Base + N * Window <= (1 << AW)`.
- Value generics of these kinds:
  - `bool`: `Strict: bool = true`, `SegActiveLow: bool = true`
  - a sized integer: `Addr: uint<7>`
  - widthless `sint`
  - an enum, with a default: `Mode: CicMode = .Decimate`
  - a struct: `Mode: VideoMode = Vga640x480`, with field access `Mode.h_active`
  - an array: `Items: [T; N]`, `Ops: [AxiLiteOp<AW, DW>; N]`, `Coeffs: [sint<CW>; Taps]`, `Bases: [uint; Slaves]`
- Generic arguments: array literals (`StreamDriver<u8, 4, [10, 20, 30, 40]>`, `Fir<s16, 3, 4, [-1, 2, -1]>`), negative literals, enum variants (`Cic<2, 4, s16, .Interpolate>`), named consts, `if` expressions (`PULLUP: if PullUp { 1 } else { 0 }`), and positional and named arguments mixed in one list.
- A generic whose type or default refers to an earlier generic; `Bus: type = Axi4Lite<32, 32>`.
- Module consts read from outside with generic arguments: `Mmcm<100_000_000, 10, 1, 5>::CLK0_HZ` (spec §12.1 mentions `Multiplier<..>::LATENCY`).
- Generic structs (`Axi4Addr<AW, IDW>`, `Complex<T>`, `AxiLiteOp<AW, DW>`). Their literals either infer the generics (`RegReq { ... }`, `Complex { ... }`) or go through a type alias (`C { re: .., im: .. }`).
- A package-level struct const (`pub const Vga640x480: VideoMode = VideoMode { ... }`) with widthless `uint`/`bool` fields.
- A package-level const array of structs built by `const fn` calls (`const TestOps: [AxiLiteOp<32, 32>; 3] = [axil_write<32, 32>(..), ..]`).
- Generic `fn` and `const fn`, called either with explicit generic arguments (`axil_write<32, 32>(a, d)`, `quarter_sine<QN, OW>()`) or with the generics inferred from the arguments (`axi_next_addr(addr, ..)`, `bit_reverse(cnt)`).
- A `const fn` that returns a struct or an array, uses `var`, `for` and `if`, and calls other const fns.
- A `const fn` called on signals (`sd_crc7` via `sd_frame`).
- `fn`, `type` aliases, `enum`s (several per module, each with its own `Idle`, resolved by context) and non-pub `const`s inside a module body.
- `const` initialised by an `if` expression: `const SW: uint = if Digits > 1 { clog2(Digits) } else { 1 }`. `const CPOL: bool = Mode >= 2`.
- `static_assert` in module bodies, inside elaboration loops, and with arguments spread over two lines.
- `T::WIDTH`, `S::MAX`/`S::MIN` through an alias, `bits(x) as sint<N>`, `T::from_bits(slice)`.
- `/` on compile-time values (spec Table 5.2) at the same precedence as `*`. `%` is not used anywhere.

### Signals, types and expressions
- `= all0` / `= all1` as the reset value or default of array, struct and `bit<N>` regs and vars, as a struct-literal field value, and as a value in expressions (`strb: all1`, `keep = all1`).
- Typed literals with constant widths: `1u1`, `0u1`, `0b110u3`, `4096u13`, `1u<W>`.
- `b as uint<1>` to turn a bool into a 1-bit integer inside `++` and `|`: `(line as uint<1>) ++ shift[7:1]`, `(rx << 1) | (mosi as uint<1>)`.
- Implicit widening across mixed widths: `|` and `<` between `uint<1>`/`bit<4>` and wider values or untyped consts (`lfsr[3:0] < Throttle`), and comparing `bit<N>` with `uint<N>`.
- `xor` on bools; a bool compared with a compile-time bool (`leading != CPHA`).
- Reduction xor on a slice (`^frame[8:1]`).
- Slice bounds given by expressions (`rx[W - 2:0]`).
- Dynamic bit index into a vector (`font_byte[~xbit]`).
- `+%`/`-%` on `sint` and between operands of different widths.
- `x as uint<N>` / `x as sint<N>` to narrow.
- `if` and `match` expressions with compile-time conditions that choose between signal expressions (`seg = if SegActiveLow { ~seg_hi } else { seg_hi }`).
- Typed `let` from an `if` over consts: `let high_time: uint<clog2(T_BIT)> = if shift[23] { T1H } else { T0H }`.
- `match` patterns and arms:
  - Integer or `bit<N>` literal arms that cover every value count as exhaustive with no `_`: 16 arms on `uint<4>` in `hex_to_segments`, 4 arms on `bit<2>`.
  - Literal arms followed by `_`.
  - Payload patterns that bind a field (`.Write { byte } => ...`).
  - Arms that are an empty block (`.Idle => {},`) or a nested `match`.
  - Alternatives with `|`.
- Enum payload variants used as Stream data: `I2cCmd::Write { byte: 0x84 }`.
- Set membership `state in [.A, .B]` (spec §10.2).
- Several statements on one line, separated by `;`.

### Module structure
- Several `on clk` blocks in one module, each driving its own registers.
- `let` bindings inside `on` blocks and inside match-arm blocks.
- Elaboration-time `for` inside `on` and `comb` blocks, with compile-time `if` inside: `for k in 0..10 { if t == Div / 2 + k * Div { if k == 0 { ... } } }`.
- A runtime comparison with a loop constant used to select an element of a const generic array: `for i in 0..N { if idx == i { item = Items[i] } }`.
- Elaboration-time `if` and `for` at module level that declare `reg`, `var`, `comb`, `on`, `property` and `static_assert`. These loops can nest, and a loop constant can be assigned to a register.
- `reg` arrays written at a dynamic, exactly-wide index inside `on` (`beats[wr] = data`). 2-D arrays `[[bool; S]; M]`.
- Bit-select as an assignment target in `comb` (`an_hi[i] = sel == i and enable[i]`). Single elements of an array `var` assigned in separate elaboration loops.
- `mem` initialised from a generic `const fn`, `mem` of struct elements, `.read(addr, rdw: .OldData)` (spec), `#[ram(block)]`.
- Out-port fields given a default inside `comb` and then read back in the same block.

### Instances and bundles
- A bundle inside a bundle (`RegBus.req: Valid<...>`), and bundle fns that use `self` (`fn fire() -> bool { self.valid and self.ready }`, `self.htrans[1]`).
- Arrays of bundle ports (`in s: [Axi4Lite<AW, DW>; N]`), indexed with loop constants in designs and in tests (`dut.m[1].pready`), with an array element passed as a connection (`Axi4LiteSlave(s: s[i])`).
- Instances created with some inputs missing, which are assigned afterwards. This includes the flipped fields of an instance's output bundles and the instance's own outputs fed back to it: `master.req.valid = ...`, `front.req.ready = true`, `mmcm.CLKFBIN = mmcm.CLKFBOUT`, `pll.CLKFB = pll.CLKOP`.
- Reading a flipped field of an instance's input bundle (`back.req.ready`).
- Field access directly on an instance expression: `clk0 = BUFG(I: mmcm.CLKOUT0).O`.
- Connections given as literals or consts:
  - a struct or Stream literal: `tx: Stream { valid: false, data: 0 }`
  - a const or literal: `divisor: DIV`, `PWRDWN: false`, `CNTVALUEIN: 0`
  - an array literal of clocks: `inclk: [clk_in, clk_in]`
- Whole-bundle assignment from an instance's output (`out = rx_fifo.pop`, `m[j] = back.m`), and assigning one instance's whole bundle to another instance's port.

### Clocks, resets, pads, externs
- `reset<async>` / `reset<async, low>` on wrapper ports. A wrapper's reset is passed to an extern port whose polarity type matches; no inversion is ever needed.
- Clock outputs that are a clock input passed through (`clk0 = clk_in` in the generic PLLs). Clocks as extern outputs. Arrays of clocks on extern ports (`out clk: [clock; width_clock]`) indexed with `pll.clk[0]`.
- `x as clock` (bool to clock) in `std::sim::ClockModel`, which only test benches use, to generate a test clock from logic.
- `tri<bool>` ports on extern pad primitives (`IOBUF.IO`, `SB_IO.PACKAGE_PIN`, `BB.B`, `ALT_IOBUF.io`) and on the non-top pad wrappers (`Iobuf`, `SbIo`, `EcpIo`, `AltIobuf`). A wrapper only passes its `pad` through to the primitive. These ports are declared with `in`, since direction has no meaning for `tri`.
- Externs written as `extern module NAME("")`:
  - The empty path means the module comes from the vendor library.
  - Generics can be listed over several lines.
  - `uint` generics feed real and integer SystemVerilog parameters; `bit<N>` generics feed vector parameters (`PIN_TYPE: bit<6>`).
  - A port's width can depend on a generic.
  - Only some of the vendor ports are declared.

### Verification
- Immediate `assert cond, "msg"` inside `on` and `comb` blocks, gated by a compile-time bool generic: `assert !(Strict and mismatch), "..."`. A `comb` block may contain nothing but an `assert`.
- Property bodies:
  - `never expr`
  - `past(x, n)` on expressions, and `past(x) == .Variant`
  - `stable(...)` on struct-valued signals
  - `rose`, `fell`, `eventually[..n]`
  - multi-line bodies
- Where properties are declared: inside elaboration loops and `if`s, and at module level in modules that only instantiate others.
- Tests:
  - Variables and loops:
    - untyped and typed `var`, reassigned (`var t = 0; t = t + 1`, `var d: sint<16> = 0`), including array vars with element and whole-array assignment
    - tuple-destructuring `for` loops, including tuples that contain arrays: `for (bytes, read) in [([..], false), ..]`
    - ranges whose bounds are only known when the test runs (`for _ in 0..(10 * div)`, `for k in 0..n`)
    - indexing and bit-indexing test values with test variables (`items[got]`, `v[23 - i]`, `reply[k as uint<3>]`)
    - `if` statements and `if` expressions
  - Simulation handles:
    - several `sim` handles in one test, stepped together
    - assigning a struct, bundle or array literal to a port: `dut.expected = Valid { .. }`, `dut.s2mm = Axi4Stream { .. }`, `dut.digits = [..]`, `dut.m.b = Stream { .. }`
    - reading internal registers, arrays and nested instance signals by path (`dut.mem_r[3]`, `dut.beats[0]`, `dut.timing.frame_start`, `dut.cnt`), and reading struct fields of ports (`dut.out.data.presence`)
    - simulating a module with no clock (purely combinational) using only `settle()`
  - Other:
    - calling package `fn`s and module consts inside `expect`
    - `rand<bool>()`
- Regmap:
  - A field bit range written high to low: `mode: uint<2> @[2:1]`. The spec's own example writes `@[1:2]`.
  - `Map::Status` used as a struct literal in a test.
