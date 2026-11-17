# The Tern user guide

This guide teaches Tern through examples. Its chapters follow the
[language specification](../tern-spec.pdf), which remains the normative
definition.

1. [Getting started](01-getting-started.md): building `tern`, a first
   project, the check/test/build loop, the simulator REPL.
2. [Language basics](02-language-basics.md): files and packages, types and
   widths, conversions, `let`/`var`/`reg`, `comb` and `on` blocks, reset.
3. [Modules and generics](03-modules-and-generics.md): declaring and
   instantiating modules, `where`, elaboration-time `for`/`if`, functions,
   the top level, extern modules.
4. [Bundles and clock domains](04-bundles-and-clock-domains.md): `bundle` and
   `flip`, named domains, crossing domains with `std::cdc`, the CDC report.
5. [Compile-time evaluation](05-compile-time.md): `const`, `const fn`,
   `static_assert`, type-level computation, feature flags.
6. [Memories, state machines and register maps](06-memories-fsms-regmaps.md):
   `mem`, enum-and-match FSMs, `regmap` and `RegisterFile`.
7. [Testing and verification](07-testing-and-verification.md): `test`
   blocks, stimulus, assertions, properties, coverage, formal verification.
8. [The standard library](08-standard-library.md): conventions, the packages,
   composing components.
9. [The toolchain](09-toolchain.md): every command, `tern.toml`, targets,
   emitted files, synthesis, packages, editors.
10. [Migrating from SystemVerilog](10-migrating-from-systemverilog.md):
    `tern import`, extern modules, a cheat sheet, a worked example.

## Conventions

Every `tern` code block in this guide is compiled by the test suite
(`make test-docs`, which `make test` includes), so the examples stay valid as
the compiler changes:

* a plain `tern` block must pass `tern check`;
* a `tern,test` block must also pass `tern test`;
* a `tern,fail` block shows a mistake and must be rejected by `tern check`;
* a `tern,ignore` block is an intentional fragment and is not checked.

To try an example, save it as `example.tern` and run `tern check example.tern`
or `tern test example.tern`. No project is needed.
