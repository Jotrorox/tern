# 8. The standard library

`std` ships with the compiler and is written entirely in Tern. Every module in
it is generic, carries tests and properties, publishes its latency as a
constant, and speaks the same `Stream` handshake. A design is assembled from
parts that fit without adapters (spec chapter 12).

The reference is generated from the sources:

```sh
tern doc std --text                  # the package list
tern doc std::fifo --text            # one package
tern doc std::fifo::Fifo --text      # one item: generics, ports, constants, properties, tests
tern doc std -o build/doc --open     # the whole library as a static HTML site
tern test std                        # run the library's own tests (363 of them)
```

## Rules every component follows

1. Generic over the payload type (`T: type` comes first) and sized by `uint`
   generics with `where` constraints. No widths are hard-coded.
2. One handshake: `Stream<T>` (valid/ready/data) between components,
   `Valid<T>` (no back-pressure) where that is enough. Width and rate changes
   are explicit components, never hidden in a port.
3. Latency is a published constant: `Fifo<u8, 16>::LATENCY`,
   `Multiplier<16>::LATENCY`.
4. Every documented behaviour has a `test`, and every sequential component
   has at least one `property` for its invariant.
5. Single-clock modules name their ports `clk` and `rst`. An input stream is
   `in` or `push`, an output stream `out` or `pop`.
6. Data registers are reset-less; only control state is reset.
7. Nothing is vendor-specific except `std::target::*`.

## Packages

| Package | Highlights |
|---|---|
| `std::prelude` (auto-imported) | `Valid<T>`, `Stream<T>`, `u8`..`u64`, `s8`..`s64`, `clog2`, `count_ones` |
| `std::stream` | `Skid`, `Pipe`, `Fork`, `Join`/`Join3`, `Arbiter` (`.RoundRobin`, `.Fixed`), `Mux`, `Demux`, `Gearbox`, `GearboxLast`, `Throttle`, `Packetizer`/`Depacketizer`, `StreamCounter`, `Beat<T>` for packets |
| `std::fifo` | `Fifo` (block RAM), `FifoFwft` (first-word fall-through), `ShiftFifo`, `Lifo`, `AsyncFifo`, `PacketFifo` |
| `std::cdc` | `Sync2`, `Sync3`, `PulseSync`, `HandshakeSync`, `ResetSync`, `ResetBridge`, `GrayCounter`, `AsyncFifo` |
| `std::math` | `flog2`, `is_pow2`, `gray`/`ungray`, `leading_zeros`, `priority_encode`, `onehot`, `min`/`max`/`abs`, `sat_add`/`sat_sub`, `widen_signed`, rounding; `Multiplier`, `Divider`, `Sqrt`, `Mac`, `Cordic`, `Fixed<I, F>`, `Lfsr` |
| `std::mem` | `RomTable`, `SinglePortRam`, `SimpleDualPortRam`, `TrueDualPortRam`, `RegFile`, `Cam`, `ShiftReg`, `DelayLine`, `Lut` |
| `std::crc` | `Crc` (bit-, byte- and word-parallel), presets `Crc8`, `Crc16Ccitt`, `Crc16Modbus`, `Crc32`, `Crc32C`; `Scrambler` |
| `std::encoding` | `Enc8b10b`/`Dec8b10b`, Manchester, NRZI, Hamming SECDED, `Parity`, `PriorityMux`, Base64 helpers |
| `std::io` | `UartTx`, `UartRx`, `Uart`, `SpiMaster`, `SpiSlave`, `QspiMaster`, `I2cMaster`, `I2cSlave`, `Pwm`, `Debounce`, `EdgeDetect`, `QuadratureDecoder`, `Ws2812`, `SevenSegment`, `Ps2`, `OneWire`, `Sdio` (SPI-mode SD card) |
| `std::bus` | bundles `Axi4`, `Axi4Lite`, `Axi4Stream`, `Apb`, `Ahb`, `Wishbone`, `TileLinkUL`; `RegisterFile`; bridges; `Axi4Crossbar`, `Axi4Interconnect`, `AxiDma`, `WishboneArbiter` |
| `std::dsp` | `Fir`, `IirBiquad`, `Cic`, `Nco`, `MovingAverage`, `Accumulator`, `Fft` |
| `std::clk`, `std::timer` | `Divider`, `Enable`, `Gate` (the one legal clock gate), `Strobe`; `Timer`, `Prescaler`, `Pulse`, `Stopwatch`, `Rtc`, `Watchdog` |
| `std::video` | `VgaTiming` with presets from 640x480 to 1920x1080, `Tmds` (DVI/HDMI), `Framebuffer`, `TextMode`, `Rgb565`/`Rgb888` |
| `std::debug` | `Ila` (trigger and capture), `Heartbeat`, `ErrorLatch` |
| `std::sim` (tests only) | `StreamDriver`, `StreamMonitor`, `Scoreboard`, `ExpectEventually`, `ClockModel`, `Axi4LiteDriver`, `Axi4StreamDriver`, `UartModel`, `SpiModel`, `I2cModel` |
| `std::target` | `xilinx` (`Mmcm`, `Pll`, `Bufg`, `Iddr`/`Oddr`, `Iobuf`, ...), `lattice` (`SbPll`, `SbIo`, `EcpPll`, ...), `intel` (`Pll`, `AltIobuf`, ...), and `generic` behavioural fallbacks so every design also simulates |

The maintainers' notes in [std/README.md](../../std/README.md) and
[std/README-part2.md](../../std/README-part2.md) list where `std` deviates
from the spec's tables (for example, `Arbiter`'s `.Weighted` policy is not
implemented, and `Strobe`, `Rtc` and `Heartbeat` take `ClkHz` as a generic).

## Using components

A checksum unit, tested against the standard CRC-32 check value of
`"123456789"`:

```tern,test
package checksum

use std::crc::Crc32

test "crc-32 of 123456789" {
    let dut = sim Crc32()
    dut.reset()
    dut.clear = false
    for c in [0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39] {
        dut.in = Stream { valid: true, data: c }
        dut.step()
    }
    dut.in.valid = false
    dut.settle()
    expect dut.crc == Crc32<u8>::CHECK
}
```

A debounced button that toggles an LED, with the PWM brightness set by a
counter. `ClkHz` is a project constant, and the debounce window is
computed from it:

```tern
package panel

use std::io::{Debounce, Pwm}

const ClkHz: uint = 12_000_000

#[top]
module Panel {
    in clk: clock #[clock(hz: 12_000_000)]
    in rst: reset
    in button: bool
    out led: bool
    out dim: bool

    let btn = Debounce<Cycles: ClkHz / 100>(d: button)   // 10 ms
    reg lit: bool = false
    reg level: u8 = 0
    on clk {
        if btn.rose {
            lit = !lit
            level = level +% 32
        }
    }
    led = lit
    let pwm = Pwm<8>(duty: level, en: lit)
    dim = pwm.pwm
}
```

The composition example from spec §12.3: a UART receiver in one clock domain,
a width converter, a clock-crossing FIFO and a CRC in another, with no glue
signals:

```tern
package soc

use std::{io::UartRx, fifo::AsyncFifo, stream::Gearbox, crc::Crc32}

#[top]
module Soc {
    in io_clk: clock
    in io_rst: reset
    in sys_clk: clock
    in sys_rst: reset
    in rxd: bool
    out crc_value: u32

    domain io = (io_clk, io_rst)
    domain sys = (sys_clk, sys_rst)

    let rx = UartRx<ClkHz: 100_000_000, Baud: 3_000_000>(rxd) @ io
    let words = Gearbox<u8, u32>(in: rx.out) @ io
    let xfifo = AsyncFifo<u32, 512>(
        push: words.out,
        src: io.clock,
        dst: sys.clock,
        src_rst: io.reset,
        dst_rst: sys.reset,
    )
    let crc = Crc32(in: xfifo.pop, clear: false) @ sys
    crc_value = crc.crc
}
```

Compared with the spec's four-line version, `AsyncFifo` needs its two resets
connected explicitly and `Crc32` its `clear` input. `Crc32`'s payload type
(`u32`) is inferred from the stream connected to `in`.

## Math helpers

Every function in `std::math` is a `const fn`, so it works on constants at
elaboration time and on signals as logic:

```tern
package mathdemo

use std::math::{is_pow2, gray, ungray, sat_add, leading_zeros, max}

#[top]
module MathDemo {
    in a: u8
    in b: u8
    out g: bit<8>
    out back: u8
    out sum: u8
    out lz: uint<4>
    out big: u8
    out pow2: bool

    static_assert(is_pow2(64u8) and !is_pow2(48u8))
    g = gray(a)
    back = ungray(g)
    sum = sat_add(a, b)          // clamps at 255 instead of wrapping
    lz = leading_zeros(bits(a))
    big = max(a, b)
    pow2 = is_pow2(a)
}
```

## Test helpers

`std::sim` holds components that only make sense in tests: stream drivers and
monitors, a scoreboard, bus drivers and models of UART, SPI and I2C devices.
They are ordinary Tern modules, so a test bench is a module that wires them to
the design under test, and the `test` block simulates that bench.

Next: [The toolchain](09-toolchain.md).
