# 6. Memories, state machines and register maps

Three things every design needs and SystemVerilog leaves to coding
conventions: RAMs that infer reliably, state machines that cannot miss a
state, and register maps that stay in sync with software (spec chapter 10).

## Memories

A `mem` is neither a `reg` nor an array of `reg`s. It has no reset and no
whole-value assignment, and it maps onto block RAM or LUT RAM:

```tern
package mems

const fn sine_table() -> [u8; 64] {
    var t: [u8; 64]
    for i in 0..64 {
        // a triangle as a stand-in for a real sine table
        t[i] = if i < 32 { (i * 8) as u8 } else { ((63 - i) * 8) as u8 }
    }
    t
}

#[top]
module Mems {
    in clk: clock
    in we: bool
    in wr_addr: uint<10>
    in wr_data: u32
    in rd_addr: uint<10>
    in idx: uint<6>
    out rd_q: u32
    out wave: u8

    mem buf: [u32; 1024]                              // block RAM if the target has it
    #[ram(distributed)] mem lut: [u8; 64] = sine_table()

    let rd = buf.read(rd_addr)      // synchronous read port: the value appears next cycle
    on clk {
        if we { buf[wr_addr] = wr_data }              // write port
    }
    rd_q = rd
    wave = lut[idx]                 // combinational read: distributed/LUT RAM only
}
```

* `m.read(addr)` is a synchronous read port: the data appears one cycle
  later. `m[addr]` outside an `on` block is a combinational read, which only
  LUT RAM supports.
* `m[addr] = data` inside `on clk` is a write port. `m.write(addr, data,
  mask)` writes with byte enables.
* Read-during-write behaviour is explicit: `m.read(addr, rdw: .OldData)` or
  `rdw: .NewData`. The default is `.Undefined`, which the simulator
  randomizes so that a test catches a design that depends on it.
* Initial contents come from a literal, a `const fn`, or
  `include_hex("file.hex")` / `include_bin(...)`. They are emitted as
  `$readmemh` plus an `initial` block.
* The compiler counts read and write ports. A `[u32; 1024]` with three
  readers is reported as `mem buf has 3 read ports; the target supports 2`,
  instead of quietly becoming ten thousand flip-flops.
* `#[ram(block)]` and `#[ram(distributed)]` choose the inference, and the
  target in `tern.toml` decides what is possible. The same source compiles
  everywhere.

## State machines

Tern has no FSM keyword. An `enum` plus a `match` inside `on clk` already
gives an exhaustive state machine whose states show up by name in
waveforms. Matching on a tuple writes a transition table directly:

```tern,test
package fsm

enum State { Idle, Run, Finish }

enum Event { None, Start, Done, Abort }

module Fsm {
    in clk: clock
    in rst: reset
    in event: Event
    out busy: bool
    out state_out: State

    reg state: State = .Idle
    on clk {
        state = match (state, event) {
            (.Idle, .Start) => .Run,
            (.Run, .Done) => .Finish,
            (.Run, .Abort) => .Idle,
            (.Finish, _) => .Idle,
            (s, _) => s,                  // stay
        }
    }
    busy = state in [.Run, .Finish]       // set membership
    state_out = state
}

test "start, finish, back to idle" {
    let dut = sim Fsm()
    dut.reset()
    dut.event = .Start
    dut.step()
    expect dut.busy and dut.state_out == .Run
    dut.event = .Done
    dut.step()
    expect dut.state_out == .Finish
    dut.event = .None
    dut.step()
    expect dut.state_out == .Idle and !dut.busy
}
```

Because `match` must be exhaustive, a new state added to the `enum` is a
compile error in every `match` that does not handle it. Tern has no
`default:` arm that silently swallows the new state. The `(s, _) => s` arm
above is an explicit decision to stay. `#[encoding(onehot)]` on the `enum`
picks one-hot state encoding.

## Register maps

A `regmap` declares a memory-mapped register block once. The compiler
generates the hardware, a C header, a CMSIS-SVD file and a Markdown table
from it:

```tern
package regs

enum Mode { Off, Slow, Fast, Turbo }

regmap Ctrl {
    0x00 ctrl: rw { enable: bool @0, mode: Mode @[2:1] },
    0x04 status: ro { busy: bool @0, err: bool @1 },
    0x08 count: rw u32,
    0x10 fifo: wo u8 -> Stream<u8>,     // a write-only register exposed as a stream
}

module Engine {
    in clk: clock
    in rst: reset
    in enable: bool
    in mode: Mode
    in data: Stream<u8>
    out busy: bool

    reg n: u8 = 0
    data.ready = enable
    on clk {
        if data.fire() { n = n +% 1 }
    }
    busy = n != 0 and mode != .Off
}

#[top]
module Soc {
    in clk: clock
    in rst: reset
    in axi: std::bus::Axi4Lite<32, 32>
    out busy: bool

    let regs = std::bus::RegisterFile<Ctrl>(bus: axi)
    let engine = Engine(enable: regs.ctrl.enable, mode: regs.ctrl.mode, data: regs.fifo)
    regs.status = Ctrl::Status { busy: engine.busy, err: false }
    busy = engine.busy
}
```

* Each line is `offset name: access type`. The access is `rw`, `ro`, `wo`,
  `w1c` (write one to clear) or `rw1s` (read/write, write one to set). The
  type is a scalar, a field list with bit positions (`@0`, `@[2:1]`), or
  `T -> Stream<T>` for a FIFO-style register.
* `Ctrl::Status` is the struct of one register's fields, named after the
  register in UpperCamel case.
* `RegisterFile<Map>` uses AXI4-Lite by default. The second generic argument
  is the bus type: `RegisterFile<Ctrl, Apb<32, 32>>` or
  `RegisterFile<Ctrl, Wishbone<32, 32>>`.
* `rw`, `wo`, `w1c` and `rw1s` registers are outputs of the register file
  (`regs.ctrl.enable`). `ro` registers are inputs that the design drives
  (`regs.status = ...`). A `w1c` register also gets an input `<name>_set`
  for the bits the hardware sets, and an `rw1s` register an input
  `<name>_clr`.
* Registers reset to zero unless the entry gives a reset value.
* An access to an offset that is not in the map answers with an error
  (SLVERR on AXI4-Lite).
* `tern build` writes `Ctrl.h`, `Ctrl.svd` and `Ctrl.md` next to the
  SystemVerilog.

Next: [Testing and verification](07-testing-and-verification.md).
