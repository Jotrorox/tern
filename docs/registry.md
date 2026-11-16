# The registry index

The index is a plain git repository — by default
`https://github.com/tern-lang/registry`, mirrored at tern-lang.com/registry —
that maps package names to git URLs and published versions. It holds no code.
`tern add` / `tern update` keep a shallow clone of it in `$TERN_HOME/index`
(default `~/.tern/index`). Another index can be used with the `TERN_INDEX`
environment variable or `[registry] index = "<url>"` in `tern.toml`.

## Layout

One file per package:

```
<first two letters of the name, lower-case>/<name>.toml
```

so `axi` is `ax/axi.toml` and `uart-ip` is `ua/uart-ip.toml`. Names of one
character live in `1/` (`1/x.toml`). Other files (README, CI) are ignored.

## Entry format

```toml
name = "axi"
git = "https://github.com/acme/tern-axi"
description = "AXI4 and AXI4-Lite bundles, interconnect and adapters"

[[version]]
version = "1.2.0"
tag = "v1.2.0"
commit = "4f38a495a19ff63c6b73c8088e19b1c6f67e787c"

[[version]]
version = "1.3.0"
tag = "v1.3.0"
commit = "6835692331ddb014cd394ecdd9444618c091dc1d"
yanked = true        # optional: never chosen for new resolutions
```

| Key | Meaning |
|---|---|
| `name` | the package name; must equal the file name |
| `git` | the repository URL, the package's identity |
| `description` | one line, optional |
| `[[version]] version` | semver version |
| `[[version]] tag` | the git tag (default `v<version>`) |
| `[[version]] commit` | the full commit hash the tag pointed to when published; a client that finds the tag elsewhere refuses it |
| `[[version]] yanked` | optional; yanked versions stay usable from an existing `tern.lock` |

Versions are kept in ascending order. The file uses the same TOML subset as
`tern.toml` (see [packages.md](packages.md)).

## Rules

* Names are first come, first served; the git URL is the identity. A later
  pull request for an existing name must use the same `git` URL.
* An entry is only ever extended: versions are added (or marked `yanked`),
  never rewritten.
* `std` is versioned with the compiler and is not in the index.

## Publishing

`tern publish` tags `v<version>`, pushes the tag, and writes the entry on a
branch `publish/<name>-<version>` of the local index clone; the change reaches
the index as a pull request (one `git push` to your fork plus `gh pr create`,
which `tern publish --fork <url> --pr` does for you). Reviewers check that the
`git` URL serves the tag at the recorded commit and that the repository has a
`tern.toml` whose name and version match.
