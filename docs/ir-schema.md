# Tern IR JSON schema (version 1)

`ir_to_json()` (src/ir/ir_json.c) serializes an elaborated design
(`IrDesign`, defined in `src/ir/ir.h`) to JSON; `ir_from_json()` reads it
back. The format is the stable contract for third-party tools (visualizers,
linters, alternative back ends). `ir.h` is the normative definition of every
field's meaning; this document describes the encoding.

Properties of the encoding:

* **Deterministic.** The same design always produces the same text, so
  `write → read → write` reproduces the text byte for byte. Output is
  pretty-printed with two-space indentation and ends with a newline.
* **Indices, not pointers.** Types are written once in a table and referenced
  by `id`; signals are referenced by their index in the module's `signals`
  array; modules are referenced by `name` (modules appear children first, so a
  module only references earlier ones).
* **Bit vectors** are strings in Verilog literal syntax: `"<width>'h<hex>"`,
  lower case, no leading zeros (`"8'h1f"`, `"130'h0"`). Readers also accept
  `'d` and `'b` and `_` separators. Widths are always explicit and > 0.
* **Absent values** are `null` (e.g. `"reset": null` where `ir.h` uses
  `IR_NONE`, `"init": null` for a register without reset value). Optional
  boolean flags that are false may be omitted (`signed`, `test_port`,
  `flipped`); readers must treat a missing flag as false.
* **Spans** are written as `"loc": {"file", "line", "col", "off", "len"}` —
  `file` is the source path as given to the compiler, `line`/`col` are
  1-based (`col` counts code points), `off`/`len` are byte offsets. `file`,
  `line` and `col` are the guaranteed part; `"loc"` is omitted for objects
  without a source location.
* Unknown members must be ignored by readers (forward compatibility within a
  major version). A change that alters the meaning of existing members bumps
  `version`.

## Top level

```json
{
  "format": "tern-ir",
  "version": 1,
  "top": "Top",
  "types": [ Type, ... ],
  "named_types": [ 3, 5 ],
  "modules": [ Module, ... ],
  "regmaps": [ Regmap, ... ],
  "crossings": [ Crossing, ... ]
}
```

| member | meaning |
|---|---|
| `top` | name of the top module (also flagged `is_top` in its module), or `null` |
| `types` | every type used anywhere in the design; element `i` has `"id": i`; children precede parents |
| `named_types` | ids of the named struct/enum types (`IrDesign.types`), in dependency order |
| `modules` | every module, children before parents; the top is last |

## Type

Common members: `id`, `kind`, `width` (total bits), `name` (struct/enum
types with an emitted name; omitted when anonymous), `loc`.

| `kind` | extra members |
|---|---|
| `"bool"`, `"bits"`, `"uint"`, `"sint"` | — |
| `"struct"` | `fields`: `[{"name", "type": id, "lsb"}]`, `fields[0]` is the most significant; `lsb` is the bit offset of the field |
| `"enum"` | `tag_width`, `encoding` (`"binary"`, `"onehot"`, `"gray"`), `variants`: `[{"name", "tag": bits, "payload": id or null}]`; the tag occupies the MSBs, payloads (structs) the LSBs |
| `"array"` | `elem`: id, `len`; element `i` occupies bits `[i*elem.width +: elem.width]` |
| `"clock"` | `falling`: bool |
| `"reset"` | `async`: bool, `active_low`: bool |

Anonymous types are structurally unique within a design (the builder interns
them), so two type ids with identical contents do not occur in compiler
output — but readers must not rely on it.

## Module

```json
{
  "name": "Fifo_u8_4", "tern_name": "Fifo", "display": "Fifo<u8, 4>",
  "package": "std::fifo",
  "is_top": false, "is_extern": false, "extern_file": null,
  "is_cdc": false, "cdc_kind": "sync2",
  "attrs": [ {"name": "keep", "args": null} ],
  "params": [ {"name": "Depth", "value": "32'h4", "signed": false} ],
  "signals": [ Signal, ... ],
  "ports": [ 0, 1, 2 ],
  "domains": [ {"name": "", "clock": 0, "reset": 1, "hz": 100000000} ],
  "default_domain": 0,
  "assigns": [ {"lhs": Lvalue, "rhs": Expr, "loc": ...} ],
  "processes": [ Process, ... ],
  "instances": [ Instance, ... ],
  "properties": [ Property, ... ],
  "loc": Span
}
```

* `cdc_kind` is present only when `is_cdc` is true: one of `"sync2"`,
  `"sync3"`, `"pulse"`, `"handshake"`, `"async_fifo"`, `"reset_sync"`,
  `"reset_bridge"`, `"gray"`, `"unsafe"`.
* `attrs` is omitted when empty.
* `ports` lists signal indices in declaration order.
* `hz` is `0` when unknown; `default_domain` is a domain index or `null`.

### Signal

```json
{
  "name": "push_valid", "path": "push.valid", "kind": "in", "type": 4,
  "domain": null, "init": null,
  "test_port": true, "flipped": true,
  "depth": 1024, "mem_init": ["8'h1", ...], "mem_init_file": "sine.hex", "mem_init_is_bin": false,
  "attrs": [ {"name": "ram", "args": "distributed"} ],
  "loc": Span
}
```

* `kind`: `"in"`, `"out"`, `"wire"`, `"var"`, `"reg"`, `"mem"`.
* `name` is the SV identifier, unique in the module; `path` the Tern-level
  path used by tests (`"tx.valid"` for a flattened bundle field).
* `domain`: domain index or `null` (domain-free).
* `init`: reset value (or power-on value in a domain without reset) for
  registers; `null` when there is none.
* For `"mem"`: `type` is the element type, `depth` the entry count,
  `mem_init` either `null` or exactly `depth` bit strings,
  `mem_init_file`/`mem_init_is_bin` the `include_hex`/`include_bin` source.
  These members are only written for memories.

### Expr

Every expression is an object with `k` (kind) and `w` (result width), plus:

| member | when |
|---|---|
| `op` | `unary`/`binary`: operator name (below) |
| `signed` | `true` when `is_signed` is set (signed division/comparison, sign extension, `ashr`, or a signed result) |
| `type` | optional semantic type id of the result (enum/struct/...) |
| `args` | operands (omitted when there are none) |
| `loc` | span |

| `k` | extra members | meaning |
|---|---|---|
| `"const"` | `v`: bits | constant |
| `"sig"` | `sig` | signal (any kind except `mem`) |
| `"slice"` | `lo`, optional `field` | `args[0][lo +: w]`; `field` names a struct field for readable output |
| `"index"` | — | element `args[1]` of `args[0]`, element `i` at `[i*w +: w]`; an index past the last whole element reads 0 |
| `"unary"` | `op` | `not neg lnot rand ror rxor` |
| `"binary"` | `op` | `add sub mul div mod and or xor shl shr ashr eq ne lt le gt ge land lor` |
| `"concat"` | — | `args[0]` is the most significant |
| `"mux"` | — | `args[0] ? args[1] : args[2]` |
| `"resize"` | — | zero-extend (`signed` false) / sign-extend (`signed` true) / truncate `args[0]` to `w` |
| `"memread"` | `sig` | combinational read of memory `sig` at address `args[0]` |
| `"past"` | `lo` | value of `args[0]` `lo` cycles ago (properties only) |

Width invariants (from `ir.h`): arithmetic and bitwise operands already
have the result width; shift amounts are unsigned of any width; comparison
results and reductions are 1 bit; `land`/`lor`/`lnot` operate on 1-bit values.

### Lvalue

```json
{"sig": 3, "lo": 0, "w": 8, "index": Expr, "len": 3, "field": "carry"}
```

Bits `[lo +: w]` of signal `sig`; with `index`, element `index` (each `w`
bits) of the array of `len` elements starting at bit `lo`; an assignment
whose index is `len` or more writes nothing (array lengths that are not a
power of two). `index`, `len` and `field` are omitted when absent; a missing
`len` means as many whole elements as fit from `lo` to the end of the
signal. A whole-signal assignment has `lo` 0 and `w` equal to the signal
width.

### Process and statements

```json
{"kind": "clocked", "domain": 0, "regs": [5, 6], "body": [ Stmt, ... ], "loc": Span}
```

`kind` is `"comb"` (then `domain` is `null` and `regs` empty) or
`"clocked"`. `regs` lists the registers the process drives; their reset
behaviour follows from each register's `init` and the domain's reset type.

Statements carry `s` (kind) and `loc`:

| `s` | members |
|---|---|
| `"assign"` | `lhs`: Lvalue, `rhs`: Expr (blocking in comb, next-value in clocked) |
| `"if"` | `cond`, `then`: [Stmt], `else`: [Stmt] |
| `"case"` | `subject`, `arms`: `[{"vals": [bits...], "body": [Stmt]}]`; an arm with empty `vals` is `default` |
| `"memwrite"` | `mem` (signal index), `addr`, `data`, `mask` (Expr with one bit per data byte, or `null`) |
| `"assert"` | `kind` (`"assert"`, `"assume"`, `"cover"`), `cond`, `msg` (string or `null`) |

### Instance

```json
{"name": "rx_fifo", "module": "Fifo_u8_64",
 "conns": [ {"port": 2, "expr": Expr}, {"port": 5, "expr": null} ], "loc": Span}
```

`port` is a signal index in the child module. For an input port `expr` is
evaluated in the parent; for an output port it is a `sig` expression naming
the parent wire the output drives, or `null` when unused.

### Property

```json
{"name": "valid_holds", "kind": "assert", "domain": 0, "seq": Seq, "loc": Span}
```

`kind` is `"assert"`, `"assume"` or `"cover"`. A sequence/property node has
`q` (kind) and:

| `q` | members | meaning |
|---|---|---|
| `"expr"` | `expr` | boolean expression in one cycle |
| `"delay"` | `a`, `b`, `lo`, `hi` | `a ##[lo:hi] b`; `hi` `null` means `$` |
| `"impl"` / `"impl_next"` | `a`, `b` | `a \|-> b` / `a \|=> b` |
| `"always"`, `"never"`, `"not"` | `a` | |
| `"eventually"` | `a`, `hi` | `eventually[..hi] a`; `hi` `null` = unbounded |
| `"until"`, `"and"`, `"or"` | `a`, `b` | |

## Regmap

```json
{"name": "Ctrl", "package": null,
 "regs": [ {"offset": 0, "name": "ctrl", "access": "rw", "width": 32, "is_stream": false,
            "doc": null,
            "fields": [ {"name": "enable", "lo": 0, "width": 1, "type": 0} ]} ],
 "loc": Span}
```

`access` is one of `"rw"`, `"ro"`, `"wo"`, `"w1c"`, `"rw1s"`; `fields` is
empty for a scalar register.

## Crossing

One entry of the CDC report:

```json
{"path": "Top > xfifo", "kind": "async_fifo", "from_domain": "sys", "to_domain": "pix",
 "signal": "flag", "loc": Span}
```

## Reading

`IrDesign *ir_from_json(Arena *a, const char *text, size_t len)` rebuilds
the design in `a` (strings are interned; source paths named in spans are
registered with `src_add()` if not known, with empty text). It validates
indices and the structure listed here and returns `NULL` after a
`diag_error()` on malformed input. Arrays are allocated so that the
`ir_add_*` builder helpers can keep appending to a loaded design.
