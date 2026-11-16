# Tern canonical style (`tern fmt`)

`tern fmt` prints every file in one canonical layout. It has no options: the
rules below are the whole configuration. The standard library is kept in this
style (`make test` runs `tern fmt --check std`).

```
tern fmt                     format the project's src/ and tests/ (or the .tern files here)
tern fmt <file|dir>...       format these files; directories recurse
tern fmt --check [paths]     change nothing; list the files that would change, exit 1 if any
tern fmt --stdout <file>     print the formatted file instead of writing it
```

The project root is the nearest directory upwards that holds `tern.toml`.
Files are written only when their content changes. A file with syntax errors
is reported and left alone (exit status 1). Before anything is written, the
result is parsed again and compared with the original: same syntax tree
(ignoring positions), same comments in the same order. If that ever fails
the file is left unchanged and an internal error is reported.

## What the formatter guarantees

* **Meaning is preserved.** Only layout changes. Literals are reproduced
  exactly as written (`0x00FF_FFFF` stays, never re-radixed; strings keep
  their escapes); parentheses are kept.
* **No comment is lost.** Count, text and order of all comments are kept.
* **Idempotent.** Formatting formatted code changes nothing.

## Layout rules

### Indentation and spacing

* 4 spaces per level, no tabs, no trailing whitespace, `\n` line endings,
  exactly one newline at the end of the file.
* Spaces around binary operators (`a + b`, `x == y`, `a ++ b`, `p |=> q`)
  and around `=`, `=>`, `->` and `@` (`reg r: u8 = 0 @ io`).
* No space before a colon, one after it: `a: T`, `push: x`, `Depth: 64`.
* No space inside brackets of any kind: `f(a, b)`, `[1, 2]`, `x[7:4]`,
  `Fifo<u8, 64>`, `use a::{B, C}`. One space after each comma.
* Struct literals, struct/enum/bundle bodies, field patterns and
  one-line blocks have inner spaces: `Stream { valid: true, data: x }`,
  `if x { y }`, `match s { .A => 1, .B => 2 }`.
* Ranges and slices are tight: `0..8`, `0..=N`, `x[hi:lo]`, `##[1:$]`.
* Prefix operators are tight: `-x`, `!x`, `~x`, `&x` (`& &x` keeps its space
  so it does not turn into `&&`).
* `extern module Name("file.sv") <W: uint> { ... }`: one space between the
  file and the generic parameters.

### Operators and keywords

* `&&` and `||` are written `and` and `or`. `!x` and `not x` are kept as
  written.

### Items and blank lines

* `package` comes first, then one blank line, then the `use` lines in the
  order the author wrote them (they are neither sorted nor merged; groups
  are printed `use a::{B, C}`). Blank lines between consecutive `use` lines
  are kept (at most one).
* Exactly one blank line between top-level items, and between the `use`
  block and the first item. A comment that belongs to the next item (doc or
  ordinary) stays attached to it; the blank line goes before the comment.
* Inside bodies (modules, blocks, functions, tests, lists) blank lines the
  author wrote are kept, collapsed to at most one, and removed directly after
  an opening and before a closing bracket.
* Attributes of items go on their own line above the item. Attributes of
  statements, fields and register entries stay where the author put them:
  `#[ram(block)] mem buf: [T; N]` on one line, or `#[async_reg]` on the line
  above `reg s1: T`. Trailing port attributes stay trailing:
  `in clk: clock #[clock(hz: 100_000_000)]`.
* A `where` clause goes on its own line, followed by the body's `{` on its
  own line:

  ```
  pub module Fifo<T: type, Depth: uint = 16>
  where Depth.is_pow2(), Depth >= 2
  {
  ```

  Long `where` lists continue after a comma, aligned under the first
  constraint, where the author broke them or where the next constraint
  would exceed the width.

### Statements and blocks

* One statement per line. Statements the author joined with `;` on one line
  stay joined (`busy = true; sh = 0xFF`).
* A block stays on one line (`if x { y }`, `on clk { q = d }`,
  `fn f() -> u8 { 1 }`) when the author wrote it on one line, it has no
  comments and it fits; otherwise it is one statement per line. An empty
  block is `{}`.
* `if`/`else` chains are all on one line or all broken (`} else {`). An
  `else` that the author put on its own line after a one-line block stays
  there, lined up with the `if`:

  ```
  if pfd_hz < 17_000_000 { 1 }
  else if pfd_hz < 26_000_000 { 2 }
  else { 3 }
  ```

* Property and sequence bodies are `{ expr }` on one line when they were
  written that way and fit, else the expression goes on its own line.

### Line width and breaking

* The target width is 100 columns.
* Argument lists, generic argument and parameter lists, array and tuple
  literals, struct literals, `static_assert(...)`, struct/enum/bundle/regmap
  bodies and match arms are written on one line when they fit; otherwise
  one element per line, indented one level, with a trailing comma, and the
  closing bracket on its own line:

  ```
  let fifo = std::fifo::Fifo<uint<FirstParameter>, SecondParameter, SecondParameter - 1>(
      push: data_in,
      pop: data_out,
  )
  ```

* A list the author broke right after its opening bracket stays broken,
  even if it would fit. A list with a comment inside is always broken.
* An argument, generic-argument, array or tuple list with a single element
  is never broken around that element; the element breaks inside if it must
  (`foo(Bar {` ... `})`).
* Array literals whose elements are all literals, names, variants or tuples
  of those are laid out as tables: as many elements per line as fit, and a
  new line wherever the author started one.
* A function signature that does not fit breaks its parameter list (not the
  return type).
* Binary expressions are never broken automatically. A line break the author
  put after a binary operator (or after `=`) is kept, and the continuation is
  indented one level. In property bodies a break before `|->`, `|=>`, `and`,
  `##`, ... is kept before the operator.
* Lines that cannot be broken by these rules may exceed the width.

### Lists, commas and match arms

* Broken lists always end with a trailing comma: match arms (including arms
  whose body is a block), struct and anonymous-struct fields, bundle fields
  (but not after a bundle `fn`), enum variants, regmap entries and fields,
  struct literals, argument and generic lists. One-line lists have none
  (except one-element tuples, `(a,)`).

### Comments

* Comments stay where they are relative to the code. A comment that follows
  code on its line stays trailing; it keeps its column when its line starts
  with the same code as in the source and the code still ends to the left of
  it (so aligned trailing comments stay aligned), otherwise it is separated
  by one space.
* Comments on their own line are indented like the code that follows.
* `///` doc comments are copied verbatim and stay attached above their item,
  port, field, variant or register.
* Block comments are copied verbatim (a multi-line block comment keeps its
  inner lines unchanged).

## Naming conventions (warnings)

The formatter warns (it never renames) when a declaration does not follow the
naming scheme of spec §3.2; the help line suggests the converted name. The
compiler itself does not care. With `--strict` the warnings are errors.

| Declaration | Convention |
|---|---|
| modules, structs, enums, bundles, regmaps, type aliases | `UpperCamelCase` |
| enum variants | `UpperCamelCase` |
| constants | `UpperCamelCase` or `SCREAMING_CASE` (`Crc32Table`, `LATENCY`) |
| ports, `let`/`var`/`reg`/`mem` names, fields, domains | `snake_case` |
| functions and their parameters, properties, sequences, registers of a regmap | `snake_case` |

Exempt: `extern module` declarations (they carry SystemVerilog names) and the
prelude's primitive aliases (`u8` ... `s64`). Generic parameters are not
checked. The check is `fmt_check_naming()` in `src/tools/fmt.c` (declared in
`src/tools/tools.h`), so other commands can run it too.
