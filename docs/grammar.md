# Tern concrete grammar (as implemented)

This is the grammar `src/syntax/` accepts: the lexer (`lexer.c`), the
recursive-descent parser with Pratt expressions (`parser.c`) and the AST it
builds (`ast.h`). It follows Appendix A of the spec and records every rule
the appendix leaves open: newlines, precedence, disambiguation, and the
extensions the standard library needs. When this file and the parser
disagree, the parser is the reference and this file has a bug.

## 1. Lexical structure

* Source is UTF-8 (a leading BOM is skipped). Identifiers are ASCII
  `[A-Za-z_][A-Za-z0-9_]*`. Unicode is allowed only in strings and comments.
  Any other non-ASCII byte is an error.
* **Line breaks are not tokens.** Each token records whether a line break
  separates it from the previous token (`Token.nl_before`). §2 says where
  that matters.
* **Comments:** `// ...`, `/* ... */` (nestable), and `/// ...` doc comments.
  All of them go into `AstFile.comments` with spans, for the formatter.
  A run of consecutive `///` lines forms one doc block. The block attaches
  to the next token, and the parser moves it onto the item, port, field,
  variant, regmap entry, `use` or statement that the token starts
  (`AstDoc`). `////` is an ordinary comment.
* **Keywords (38):** `package use pub module extern bundle struct enum const
  fn type regmap domain where test property sequence in out flip let var reg
  mem on comb match if else for while return as sim expect assert assume
  cover`.
* **Word operators and literals:** `and or not xor true false all0 all1`.
  `&&`, `||` and `!` lex as `and`, `or` and `not`.
* **Contextual words:** `static_assert always never eventually until` are
  identifiers except in their constructs (§4.6, §4.12).
* **Reserved words:** `impl trait async yield generate initial wire logic`.
  Each one is rejected with a dedicated message. For the last four the
  message is "`x` is SystemVerilog, not Tern", plus the Tern equivalent as a
  help line. The one exception is `async` as a generic argument, as in
  `reset<async, low>`.
* **Integer literals:** decimal `42`, hex `0xFF`, binary `0b1010_0011`,
  octal `0o17`; `_` separators are ignored. There are no floats: `1.5` is an
  error, and `1..5` is a range.
* **Typed literals:** a `u`/`s` suffix with a decimal width (`42u8`,
  `0b110u3`), or with a const expression in angle brackets directly after
  the `u`/`s` (`0xFFu<W>`, `1u<W + 1>`). In `-3s12` the minus is the unary
  operator. AST: `EXPR_INT` with `suffix` and `width`. The literal text is
  kept as written in `int_lit.text`, so values wider than 64 bits stay
  exact.
* **Strings:** `"..."` on one line. Escapes: `\n \t \r \0 \\ \" \' \{ \}
  \xHH` (00-7F) and `\u{X..}`.
* **Attributes:** `#[name]` or `#[name(args)]`. Each argument is an
  expression, optionally labelled `label: expr`, as in
  `#[clock(hz: 100_000_000)]` and `#[ram(block)]`.
* **Punctuation and operators:** `( ) [ ] { } , ; : :: . .. ..= = == != < <= >
  >= + - * / % +% -% << >> >>> & | ^ ~ ++ -> => |-> |=> ## @ $ #[`.
* **Errors the lexer reports itself, and then continues past:**
  * `===` and `!==` (2-state equality only); lexed as `==` and `!=`.
  * `?` ("`?:` does not exist").
  * `+= -= *= /= %= &= |= ^=` (no compound assignment); lexed as `=`.
  * `**`.
  * SystemVerilog literals `8'hFF` and `'0`.
  * Backtick directives (there is no preprocessor).
  * `#10` delays.
  * Bad suffixes `5us` and `42u`, invalid digits, unterminated strings and
    comments, unknown escapes.

  Lexical errors are reported in source order, interleaved with syntax
  errors.

## 2. Newlines

A statement ends at the end of its line; `;` separates statements on one
line. A line break is **insignificant**:

* inside `( ... )`, `[ ... ]` and generic lists `< ... >`;
* inside a `property`/`sequence` body, so a property can continue on the
  next line with `|=>`;
* after any binary operator, `=`, `=>`, `->`, `,`, `:` and `@`. The parser
  only checks for a line break at points where the construct could
  continue: a binary operator, a postfix operator, an optional trailing
  part. So an operand always continues onto the next line;
* before `else`, before `where` (on items), and before the `{` of item
  bodies, blocks and `if`/`for`/`while` heads;
* between entries of brace-delimited lists (struct/enum/bundle bodies,
  struct literals, regmaps, match arms). Entries are separated by `,`, `;`
  or a line break.

Inside `{ ... }` blocks line breaks are significant again, even when the
block is nested in parentheses. A line break **ends** an expression before:

* a binary operator: `x` followed by `-y` on the next line is two
  statements;
* a postfix operator `.f`, `[i]`, `(args)`, `as T`, or `{` starting a
  struct literal: a line that starts with `.method()` is a new statement;
* `@ dom` (instance domain, `reg` domain, port domain), a trailing port
  attribute, and the `where` of `rand<T>() where c`.

After a statement the parser requires `;`, `}`, end of file or a line break,
otherwise: "expected a new line or `;` after the statement".

## 3. Precedence (Table 3.2, extended)

Lowest to highest. All binary operators are left-associative except
implication, which is right-associative.

| bp | operators | AST |
|---|---|---|
| 2 | `a..b`, `a..=b`, `..b` | `EXPR_RANGE` |
| 4 | `\|->` `\|=>` (properties only) | `BIN_IMPLIES`, `BIN_IMPLIES_NEXT` |
| 6 | `until` (properties only) | `BIN_UNTIL` |
| 8 | `a ##n b`, `a ##[lo:hi] b`, `a ##[lo:$] b` | `EXPR_DELAY` |
| 10 | `or` `\|\|` | `BIN_OR` |
| 12 | `xor` | `BIN_XOR` |
| 14 | `and` `&&` | `BIN_AND` |
| 16 | `== != < <= > >=`, `in` (set membership) | `BIN_EQ`.., `BIN_IN` |
| 18 | `++` | `BIN_CONCAT` |
| 20 | `\|` | `BIN_BITOR` |
| 22 | `^` | `BIN_BITXOR` |
| 24 | `&` | `BIN_BITAND` |
| 26 | `<< >> >>>` | `BIN_SHL`.. |
| 28 | `+ - +% -%` | `BIN_ADD`.. |
| 30 | `* / %` | `BIN_MUL`.. |
| 32 | prefix `- ! not ~ & \| ^` | `EXPR_UNARY` (`&x \|x ^x` are reductions) |
| — | postfix `.f`, `.f<..>`, `[i]`, `[hi:lo]`, `(args)`, `as T`, `Path { .. }`, `@ dom` | |

The spec leaves out `xor`; here it sits between `and` and `or`. Inside
properties, the prefixes `always e`/`never e` take the whole rest of the
expression (bp 3). `eventually[..n] e` takes an operand at bp 5, so
`a |-> eventually[..N] b == 0` reads as `a |-> eventually(b == 0)`. A
leading `##n e` is a delay with no left side.

## 4. Grammar

EBNF. `NL` is the statement end from §2. `name` is an identifier; where a
keyword reading is impossible, `in` and `out` are accepted too (§5.3).

### 4.1 File

```
file      = [ "package" word { "::" word } NL ] { use | item } ;
use       = [ "pub" ] "use" use_tree NL ;
use_tree  = word { "::" word } [ "::" ( "*" | "{" use_tree { "," use_tree } [","] "}" ) ]
            [ "as" name ] ;
item      = attrs [ "pub" ] ( module | extern_mod | bundle | struct | enum | const | fn
          | type_alias | regmap | test | property | sequence )
          | static_assert ;
```

* `package` is optional: fixtures and single files have none.
* `word` includes keywords, because the spec's own package names include
  `std::mem` and `std::sim`.
* `use` may appear anywhere at file level. `pub use` re-exports, and
  `use a::B as C` is accepted.
* At file level the parser reports `reg`/`let`/`in`/... "outside a module",
  SystemVerilog `endmodule`/`interface`/`typedef`/`function`/`parameter`/
  `import`/`include`, and reserved words.

### 4.2 Items

```
module     = "module" name [ generics ] [ where ] "{" { stmt } "}" ;
extern_mod = "extern" "module" name "(" string ")" [ generics ] [ where ] "{" { stmt } "}" ;
generics   = "<" [ generic { "," generic } [","] ] ">" ;
generic    = name ":" ( "type" [ "=" type ] | type [ "=" expr ] ) ;
where      = "where" expr { "," expr } ;
bundle     = "bundle" name [ generics ] "{" { attrs ( [ "flip" ] name ":" type | [ "pub" ] ( const | fn ) ) SEP } "}" ;
struct     = "struct" name [ generics ] "{" { attrs ( field | [ "pub" ] ( const | fn ) ) SEP } "}" ;
field      = attrs name ":" type ;
enum       = "enum" name [ generics ] "{" { attrs name [ "{" { field SEP } "}" | payload ] SEP } "}" ;
payload    = "<" name ":" type { "," name ":" type } [","] ">" ;   (* Weighted<Weights: [uint; N]> *)
const      = "const" name [ ":" type ] "=" expr ;
fn         = [ "const" ] "fn" name [ generics ] "(" [ param { "," param } [","] ] ")"
             [ "->" type ] [ where ] block ;
param      = name ":" type ;
type_alias = "type" name [ generics ] "=" type ;
regmap     = "regmap" name "{" { attrs reg_entry SEP } "}" ;
reg_entry  = expr name ":" access ( "{" { name ":" type "@" bitpos SEP } "}"
                                  | type [ "->" type ] ) [ "=" expr ] ;
access     = "rw" | "ro" | "wo" | "w1c" | "rw1s" ;
bitpos     = expr | "[" expr ":" expr "]" ;
test       = "test" string block ;
property   = [ "assert" | "assume" | "cover" ] "property" name "{" temporal "}" ;
sequence   = "sequence" name "{" temporal "}" ;
SEP        = "," | ";" | line break ;
```

Notes:

* Generic defaults may refer to earlier generics and may be any expression:
  `.RoundRobin`, an array literal, an `if` expression or a const fn call.
  Generic parameter types may be enums, structs or arrays (`Data: [T; N]`).
  The generic list may span lines, and its `>` may sit on its own line.
* A module's generic default may be `clock_hz(port)`, the nominal frequency
  of the clock that the instantiation connects to that clock port (spec
  §7.5; §4.8). The generic then *is* that clock's frequency inside the
  module, whether it took the default or was given explicitly.
* An enum variant may carry a payload declared like generic parameters,
  `Weighted<Weights: [uint; N]>` (spec §12.2, `Arbiter`'s
  `.Weighted<[uint; N]>`). It is stored exactly like a `{ .. }` payload
  (`Variant.angle` records the spelling) and is written generic-style:
  `.Weighted<[3, 1, 2]>` (positional, in declaration order) or
  `.Weighted<Weights: [3, 1, 2]>`, also `ArbPolicy::Weighted<..>`; any
  payload variant accepts this form, and patterns use the brace form
  (`.Weighted { Weights }`). Payload names are UpperCamel like generic
  parameters. A payload without a bit representation (widthless `uint`,
  arrays of it) makes the enum compile-time only: usable as a generic
  parameter, const or const fn value, not as a signal. Such values compare
  with `==`, take part in `match`, and appear in monomorphized names
  (`Arbiter_u8_3_Weighted_3_1_2`).
* A `where` clause may follow a line break. Its expressions do not allow
  struct literals (§5.2).
* An extern's string is the SystemVerilog file; `""` means a vendor-library
  cell.
* `const` without a type and `fn` without `-> T` are accepted; sema decides
  whether they are allowed.
* Regmap fields write bit ranges as `@[hi:lo]` or `@[lo:hi]`, stored as
  written. `= value` gives a reset value; this is an extension used by the
  `RegisterFile` documentation.
* Struct and bundle bodies may contain associated `const` and `fn` items
  (`Flags::NONE`; an extension the spec's §6.2 example needs), stored in
  `struct_def.fns` / `bundle.fns`. `::` after an expression that reads as a
  type but is not a path (`typeof(x)`, `[T; N]`, an anonymous struct or a
  tuple type) starts an `EXPR_QPATH` (§9.4).
* A bundle may contain `fn` items. Inside them, `self` is an ordinary
  identifier.
* A doc comment may come before or after the attributes of a declaration.

### 4.3 Statements

Module bodies, blocks, `fn`/`test` bodies, `on`/`comb` bodies and the
bodies of elaboration-time `if`/`for` all use one statement grammar. Sema
decides which statements are legal where.

```
stmt   = attrs (
           ( "in" | "out" ) name ":" type [ "@" postfix ] { attr }    (* port *)
         | "domain" name "=" "(" expr [ "," expr ] ")"
         | "let" pattern [ ":" type ] "=" expr [ "where" expr ]
         | "var" name [ ":" type ] [ "=" expr [ "where" expr ] ]
         | "reg" name ":" type [ "=" expr ] [ "@" postfix ]
         | "mem" name ":" type [ "=" expr ]
         | [ "pub" ] ( const | fn | struct | enum | type_alias | bundle | property | sequence )
         | "comb" block
         | "on" postfix block
         | "for" pattern "in" expr block
         | "while" expr block
         | ( "assert" | "expect" ) expr [ "," expr ]
         | "static_assert" "(" expr [ "," expr ] [","] ")"
         | "return" [ expr ]
         | expr [ "=" expr [ "where" expr ] ]                         (* assignment / expression *)
         ) ;
block  = "{" { stmt NL } "}" ;
```

* **Port vs. expression:** `in`/`out` followed by a name (or keyword) and `:`
  starts a port. Anything else is an expression, so `in.ready = ...`,
  `out[i].valid = ...` and `in = x` are assignments.
* **Port extras:** a port may carry a domain (`in d: T @ src_d`) and
  trailing attributes on the same line (`in clk: clock #[clock(hz: N)]`).
* **Domains:** a `reg` or `on` may name a clock port or a domain.
  `domain d = (clk, rst)` takes expressions, so a converted `let` works.
* **`reg` with domain:** the domain is not part of the initializer
  expression. `reg s1: T = T::from_bits(all0) @ dst` is init
  `T::from_bits(all0)`, domain `dst`.
* **`if` and `match` are expressions.** In statement position they are
  `STMT_EXPR`, and this includes the module-level elaboration
  `if Fast { ... }`. A block's value (`Block.tail`) is its last expression
  statement when no `;` follows it. `Block.stmts` still contains that
  statement.
* **`<=` at statement level:** `x <= y` gets "`<=` non-blocking assignment
  does not exist; use `=`".
* **`where` constraint:** `let b = rand<u32>() where b <= 0x1000` (§11.2)
  is a constraint on `let`/`var`/assignment, written on the same line.
* **Nested items:** a module may contain `pub const`, and module-local
  `fn`/`struct`/`enum`/`type`/`bundle`/`property`/`sequence` (`STMT_ITEM`).
* **SystemVerilog keywords** at the start of a statement (`begin`, `end`,
  `always_ff`, `always_comb`, `assign`, `input`, `output`, `inout`,
  `parameter`, `localparam`, `typedef`, `function`, `integer`, ...) get a
  dedicated message when they cannot be the start of a Tern statement.

### 4.4 Expressions

```
primary = int | typed_int | "true" | "false" | "all0" | "all1" | string
        | path                                (* a, T::WIDTH, Fifo<u8, 64>, std::bus::X<..> *)
        | "." name [ "{" field_inits "}" | generic_args ]
                                              (* .Idle, .Write { addr, data: d }, .Weighted<[3, 1]> *)
        | "(" ")" | "(" expr ")" | "(" expr "," [ expr { "," expr } [","] ] ")"
        | "[" [ expr { "," expr } [","] ] "]" (* array literal *)
        | "[" type ";" expr "]"               (* array type in generic-argument position *)
        | "struct" "{" fields "}"             (* anonymous struct type, same *)
        | block | if_expr | match_expr
        | "sim" postfix                       (* sim Fifo<u8, 4>() *)
        | ".." expr ;
if_expr    = "if" expr block [ "else" ( if_expr | block ) ] ;
match_expr = "match" expr "{" { pattern "=>" ( block | stmt ) SEP } "}" ;
postfix    = primary { "." word [ generic_args ] | "[" expr [ ":" expr ] "]"
                     | "(" [ arg { "," arg } [","] ] ")" [ "@" postfix ]
                     | "as" type | path "{" field_inits "}"
                     | "::" path } ;   (* typeof(x)::WIDTH, [u8; 4]::WIDTH: after a non-path type *)
arg        = [ name ":" ] expr ;                      (* push: x, in: rx.out, max: 1000 *)
field_inits= { name [ ":" expr ] SEP } [ ".." ] ;
path       = name [ generic_args ] { "::" word [ generic_args ] } ;
generic_args = "<" [ garg { "," garg } [","] ] ">" ;
garg       = [ name ":" ] expr ;                      (* types and values share one list *)
```

* A generic argument is parsed as an expression. When it also reads as a
  type, `GenericArg.type` is set too. That covers paths (with arguments),
  `[T; N]`, `struct {..}`, `typeof(e)`, and tuples or parentheses of these.
  A type generic uses `.type`, a value generic `.expr`. Arguments may be
  named (`Baud: 1`) and positional and named arguments may be mixed.
* A match arm body is an expression, a block, or any statement such as
  `.Sub => alu_out = a -% b`. It is stored as a `Stmt`: value arms are
  `STMT_EXPR`.
* Instances and function calls are both `EXPR_CALL`. `Fifo<u8>(push)` (a
  bare name as argument) is a positional argument; sema treats it as the
  shorthand connection.
* Struct literals take a path: `Stream { .. }`, `Ctrl::Status { .. }`,
  `Stream<u8> { .. }`, `Cmd::Write { .. }`. Shorthand fields (`{ data }`)
  have `value == NULL`.
* `x.trunc<8>()`: method generic arguments are accepted when `(` follows.
* `.Name<..>` is a variant with its payload given generic-style
  (`EXPR_VARIANT` with `angle` set; the arguments become `FieldInit`s whose
  `name` is empty for positional ones). Inside a generic list it is always
  parsed; elsewhere speculatively, and it is kept when the `>` closes (a
  comparison with a variant on the left is meaningless).

### 4.5 Patterns

```
pattern  = alt { "|" alt } ;
alt      = "_" | name | literal [ ( ".." | "..=" ) literal ]
         | name ( ".." | "..=" ) literal
         | "." name [ "{" field_pats "}" ]
         | path                                   (* two or more segments: Op::Add *)
         | path "{" field_pats "}"                (* Fifo { pop, count }, Cmd::Write { addr, .. } *)
         | "(" pattern { "," pattern } [","] ")" ;
literal  = unary-level expression starting with an integer, `-`, true, false, string, all0/all1 ;
field_pats = { name [ ":" pattern ] SEP } [ ".." ] ;
```

A single identifier is `PAT_BIND`; sema may resolve it to a const. Range
bounds are expressions (`LO..=HI`, `3..=9`).

### 4.6 Types

```
type = path                       (* uint<8>, sint<9>, bit<N>, bool, clock<falling>,
                                     reset<async, low>, tri<bool>, Stream<T>, Ctrl::Status, u8 *)
     | "[" type ";" expr "]"
     | "struct" "{" { field SEP } "}"
     | "(" type { "," type } [","] ")"
     | "typeof" "(" expr ")" ;
```

`typeof` is contextual: it is a type only when followed by `(`. Built-in
type names are ordinary paths.

### 4.7 Temporal expressions (property and sequence bodies)

```
temporal = expr ;   (* with the extra operators of §3 enabled *)
```

Inside a `property`/`sequence` body:

* `always e` and `never e`;
* `eventually e` and `eventually[..n] e`;
* `a until b`;
* `a ##n b`, `a ##[lo:hi] b`, `a ##[lo:$] b`, and a leading `##n b`;
* `a |-> b` and `a |=> b`.

The cycle count after `##` is a literal, a name or a parenthesized
expression, so `a ##1 (b)` is a delay and not a call. `stable(x)`,
`rose(x)`, `fell(x)` and `past(x, n)` are ordinary calls. Outside
properties, `|->`, `|=>` and `##` are reported, and `always` and the other
words are plain identifiers. A property may sit inside an elaboration
`if`/`for`.

### 4.8 Builtin functions

Besides the prelude, these names are handled by the elaborator (and in
tests by the interpreter) when nothing in scope shadows them: `bits(x)`,
`static_assert(c, "msg")`, `unsafe_cross(x)`, `include_hex(f)` /
`include_bin(f)`, `const_log(..)`, `stable`/`rose`/`fell`/`past` (properties)
and:

* `clock_hz(clk)` — the nominal frequency in Hz (a widthless `uint`) of a
  clock port or domain of the module being elaborated (spec §7.5). It is
  known when the clock port carries `#[clock(hz: N)]`, or when a generic of
  the module is declared `= clock_hz(port)`: in that default it reads the
  frequency of the clock the instantiation connects (by name, through `@
  dom`, or the parent's default domain), so the frequency travels down the
  hierarchy one generic at a time, `ClkHz: uint = clock_hz(clk)`, and
  instances under different clocks are different modules
  (`Strobe<10, 100000000>`). Anywhere else an unknown frequency is an
  error with the fix. A module whose generic defaults to `clock_hz(..)` is
  not elaborated on its own as a default root (`tern check` without tops).
  The known frequency also fills `IrDomain.hz` of the module's domains (the
  simulator's time base).

Attributes with compiler support on declarations: `#[clock(hz: N)]`,
`#[test_port]`, `#[keep]`, `#[async_reg]`, `#[ram(..)]`, and
`#[probe]` (std::debug): the probed ports and module-level `let`/`var`/`reg`
signals of a module become the struct type `Probes` (one field per
signal, named after it, in declaration order) and the module-level value
`probes` of that type, so `Ila<Probes>(probes: probes, ..)` captures them
all. Probed signals are emitted with `(* mark_debug = "true" *)`.
* `x[i +: w]` and `x[i -: w]` are rejected with a message.

## 5. Disambiguation rules

### 5.1 Generic arguments vs. `<`

* **In type position** (after `:`, `as`, `->`, inside generic lists, after
  `type X =`), `<` after a path segment always opens generic arguments.
* **In expressions**, the parser parses `<...>` speculatively. Errors are
  suppressed and the arena is rewound on failure. It keeps the generic
  reading only if the closing `>` is followed, on the same line, by `(`
  (call or instance), `{` (struct literal, where allowed) or `::`.
  Otherwise it backtracks and `<` is a comparison. So `a < b and c > d` is
  two comparisons, `f<8>(x)` is a call, and `x < y >> 2` is
  `x < (y >> 2)`.
* **Inside generic lists**, `>`, `>=`, `>>` and `>>>` close the list rather
  than act as operators. Write `Foo<(A > B)>` for a comparison.
* **Splitting:** `>>`, `>>>` and `>=` are split when they close a generic
  list (`Stream<Stream<u8>>`, `A<B<C<u8>>>`, `let x: uint<8>= 3`). The split
  state is part of the backtracking state.

### 5.2 Struct literals in condition heads

In the head of `if`, `match`, `while` and `for`, and in `where` clauses,
`Path {` is not a struct literal: `if tx.valid { ... }` is a block. Inside
parentheses, brackets or call arguments the rule is lifted again:
`if (Foo { a: 1 }) == y { }` and `if f(Foo { a: 1 }) { }` work.

### 5.3 `in` and `out` as names

Both are keywords, but std and the spec use them as port names. They are
names:

* as a port name after `in`/`out` (`in in: Stream<T>`, `out out: ...`);
* as a primary expression (`in.ready`, `out[i].valid`, `in`, `dut.in`);
* as a field after `.`;
* as an argument or field label (`Gearbox<..>(in: rx.out)`);
* as `fn` parameter, `var`/`reg`/`mem` and field names.

`in` after a complete operand is the membership operator (`state in [.A,
.B]`). In `for pattern in expr` the pattern parser never consumes `in`.

### 5.4 Keywords after `::`

A path segment after `::` may be any word, keyword or not (`std::sim::X`,
`std::mem::RomTable`). The same holds for every segment of a `package` or
`use` path.

### 5.5 `@`

After an instance, `@ dom` sets the instance domain
(`Fifo<u8>(push: s) @ io`). In a `reg` initializer this is switched off, so
the `@` belongs to the `reg`.

## 6. Extensions beyond Appendix A used by std

Each of these is required by `std/**/*.tern` or by the spec text outside
Appendix A:

1. `in`/`out` as port names and expression roots (§5.3).
2. `pub use` re-exports, grouped uses with nested paths
   (`use std::{io::UartRx, ...}`), and keywords in package paths
   (`package std::mem`).
3. `pub const` and module-local `fn`/`struct`/`enum`/`type` in module
   bodies.
4. Port domains `in d: T @ src`; `on <domain>`; `domain` with any
   expressions; trailing port attributes.
5. `var` without a type (`var r = 0`), and `const` without a type.
6. Generics:
   * generic lists over several lines, with the `>` on its own line;
   * defaults that are arbitrary expressions;
   * named and positional arguments mixed;
   * array, struct, enum-variant, `if` and negative-literal arguments;
   * types inside argument lists (`[T; N]`, `struct {..}`).
7. `where` clauses with several comma-separated constraints, continued on
   following lines.
8. `T::from_bits(..)`, `Module<args>::CONST` and `Map::Status { .. }` paths.
9. Brace-list separators: `,` or line break, with trailing commas.
10. Doc comments on fields, variants, ports, properties and regmap entries.
11. Match arms that are statements (`x = 1`) or empty blocks (`=> {}`), and
    nested matches.
12. Properties:
    * multi-line bodies;
    * declared inside elaboration `if`/`for`;
    * `never`, `until` and leading `##`;
    * `##[1:$]`;
    * sequences on the left of an implication.
13. `rand<T>() where cond` constraints on `let`/`var`/assignment.
14. `static_assert(...)` with arguments over several lines and a trailing
    comma; `static_assert` at file level.
15. Typed literals with an expression width (`1u<W>`).
16. Regmaps:
    * `@[hi:lo]` bit ranges in either order;
    * `= reset` values;
    * docs on entries and fields.
17. Tests:
    * named arguments (`step_until(c, max: 300)`, `run(ns: 500)`,
      `expect_eventually(c, max: 20)`);
    * several statements per line with `;`.
18. Enum variant payloads declared and written generic-style
    (`Weighted<Weights: [uint; N]>`, `.Weighted<[3, 1, 2]>`), and
    `clock_hz(clk)` (§4.2, §4.8).

## 7. Error recovery

* The first error in a statement puts the parser into panic mode. Further
  errors stay quiet until it resynchronizes:
  * **In blocks and module bodies:** at the next line break, `;` or `}` at
    the same nesting level, or at a declaration keyword that starts the
    line where the error happened.
  * **In brace lists:** at the next `,` or line.
  * **At file level:** at the next line-initial item keyword, `#[`, a
    reserved word or `endmodule`.
* Speculation never reports.
* At end of file only the innermost unclosed `{` is reported, with a note
  at its position.
* When a line break cut a construct short, "expected X, found Y" points at
  the end of that line ("found end of line").
* Nesting deeper than about 200 levels (parentheses, blocks, types,
  unary chains) is reported as "nesting is too deep" instead of exhausting
  the C stack. Speculative generic arguments nest at most two deep, which
  bounds backtracking.
* Positions are exact and cheap: the lexer computes line and code-point
  column for every token in one forward pass (`Token.line`/`col`), and node
  spans start at token starts. `tests/fuzz/fuzz_parser.c` checks them
  against `src_linecol`.
* The parser never stops at the first error. `tern parse` and the interim
  `tern check` exit with status 1 when any file has errors.

## 8. Dump format (`tern parse --dump`)

The golden tests (`tests/fixtures/**/<name>.parse`) use an S-expression
dump:

* Items and statements start a line with `(<kind>@line:col ...`, followed
  by attributes (`#[..]`) and docs (`(doc "...")`).
* Expressions are inline:
  * paths print as written, generic arguments included;
  * binary operators as `(op lhs rhs)`;
  * calls as `(call callee args.. [@ dom])`;
  * fields as `(. base name)`.
* Blocks and match arms break lines. A block's value prints as
  `(tail e)`.

The format is for tests only; nothing reads it back.
