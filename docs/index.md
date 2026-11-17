# Tern documentation

## Start here

* [README](../README.md): what Tern is, building, quickstart, status.
* [User guide](guide/README.md): the language and the toolchain, chapter by
  chapter, with examples that the test suite compiles.
  1. [Getting started](guide/01-getting-started.md)
  2. [Language basics](guide/02-language-basics.md)
  3. [Modules and generics](guide/03-modules-and-generics.md)
  4. [Bundles and clock domains](guide/04-bundles-and-clock-domains.md)
  5. [Compile-time evaluation](guide/05-compile-time.md)
  6. [Memories, state machines and register maps](guide/06-memories-fsms-regmaps.md)
  7. [Testing and verification](guide/07-testing-and-verification.md)
  8. [The standard library](guide/08-standard-library.md)
  9. [The toolchain](guide/09-toolchain.md)
  10. [Migrating from SystemVerilog](guide/10-migrating-from-systemverilog.md)

## Specification

* [tern-spec.pdf](tern-spec.pdf): the normative language, standard library
  and toolchain specification, version 0.1.
* [spec.txt](spec.txt): a plain-text extraction of the PDF, for searching
  (it renders `->` as `→`, `=>` as `⇒` and `<<`/`>>` as `«`/`»`).

## Reference

* [grammar.md](grammar.md): the concrete grammar the parser implements.
* [emitted-sv.md](emitted-sv.md): the SystemVerilog and Verilog-2005 that
  `tern build` writes.
* [ir-schema.md](ir-schema.md): the JSON form of the intermediate
  representation.
* [packages.md](packages.md): `tern.toml`, `tern.lock`, version requirements,
  and the package commands.
* [registry.md](registry.md): the git-based registry index.
* [style.md](style.md): the canonical style `tern fmt` produces, and the
  naming conventions.
* [migration.md](migration.md): `tern import` and `tern remap` in detail.
* [editors.md](editors.md): `tern lsp` and the setup for VS Code, Neovim,
  Helix and Emacs.

## Standard library

* `tern doc std --text` lists the packages, and `tern doc std::<pkg>::<Item>
  --text` shows one component's generics, ports, constants, properties and
  tests. `tern doc std -o build/doc` writes the same reference as HTML.
* [std/README.md](../std/README.md) and
  [std/README-part2.md](../std/README-part2.md): the package overview and the
  maintainers' notes on conventions and deviations from the spec.
* [Chapter 8 of the guide](guide/08-standard-library.md): a tour.

## Contributing

* [CONTRIBUTING.md](../CONTRIBUTING.md): building, testing, code rules,
  commits and CI.
* [ARCHITECTURE.md](ARCHITECTURE.md): the internal contract between the parts
  of the compiler.
* [CHANGELOG.md](../CHANGELOG.md): release notes.
