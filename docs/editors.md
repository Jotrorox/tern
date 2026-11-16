# Editor support

`tern lsp` is a Language Server Protocol server that talks JSON-RPC over
stdin/stdout. Every editor with an LSP client can use it; there is nothing to
install besides the `tern` binary.

```
tern lsp [--log FILE] [--debounce MS]
```

* `--log FILE` writes every message and analysis timing to FILE (nothing is
  ever written to stderr otherwise).
* `--debounce MS` (default 150) waits that long after the last edit before
  re-analysing; requests such as hover always see the latest text.
* `--stdio` is accepted and ignored (stdio is the only transport).

## What it does

| Feature | Notes |
|---|---|
| Diagnostics | the whole `tern check` pipeline (parse, resolve, elaborate, IR checks) on every change, for the project the file belongs to; help lines and the elaboration path are part of the message, notes become related locations |
| Go to definition | locals, signals, ports, generics, fields, instance ports, types, modules, functions, constants and `use` paths, across files and into std |
| Hover | type, width in bits, clock domain, driver (`on clk`, `comb`, `continuous`, ...), constant value, the item signature and its `///` doc comment |
| Find references / highlight | uses the elaborator saw, plus `use` imports |
| Document symbols | modules with their ports, signals, instances, domains, constants; structs/bundles with fields; enums with variants |
| Workspace symbols | top-level items of the open projects |
| Formatting | `tern fmt`; no edit when the file has syntax errors |
| Completion | keywords, built-in types, names in scope, `pkg::` members, fields / ports / methods after `.` |
| `tern/instanceTree` | the elaborated hierarchy (custom request, below) |

A file belongs to the project of the nearest `tern.toml` when it lies in
that project's `src/` or `tests/`; such files are analysed together (open
editor buffers replace the files on disk). Any other file is analysed on its
own, like `tern check file.tern`. Elaboration-based features (types, widths,
drivers, references) cover the modules `tern check` elaborates: `#[top]`
modules, else every module whose generics all have defaults, plus the
modules the tests instantiate. Inside other generic modules navigation falls
back to the syntax tree.

std is shown from a `std/` directory when one is used (`$TERN_STD`, or
`std/` next to the `tern` binary, or the workspace's `std/`); definitions in
the copy embedded in the binary have no file to jump to.

### `tern/instanceTree`

Request parameters: `{ "textDocument": { "uri": ... } }` (optional; without
it every open analysis is returned). Result: an array of root nodes:

```json
{
  "name": "f",                       // instance name (the module name for roots)
  "module": "Fifo",                  // Tern module name
  "display": "Fifo<uint<8>, 16>",    // with generic arguments
  "emitted": "Fifo_u8_16",           // emitted SystemVerilog module
  "package": "std::fifo",
  "params": [{ "name": "Depth", "value": "16" }],
  "location": { "uri": "...", "range": { ... } },     // the instance
  "definition": { "uri": "...", "range": { ... } },   // the module
  "children": [ ... ]
}
```

The server announces it as `capabilities.experimental.instanceTreeProvider`.

## VS Code

A minimal extension lives in [`editors/vscode/`](../editors/vscode/): syntax
highlighting (TextMate grammar: keywords, comments, numbers with width
suffixes, attributes), bracket/comment configuration, and a
`vscode-languageclient` client that starts `tern lsp`. It also adds the
commands *Tern: Show Instance Tree* and *Tern: Restart Language Server*.

```sh
cd editors/vscode
npm install                    # vscode-languageclient
code --extensionDevelopmentPath="$PWD"      # try it, or:
npx @vscode/vsce package && code --install-extension tern-0.1.0.vsix
```

Settings: `tern.path` (the executable, default `tern` from `PATH`) and
`tern.lsp.log` (a log file for `--log`).

## Neovim

With [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig), register a
custom server:

```lua
vim.filetype.add({ extension = { tern = 'tern' } })

local configs = require('lspconfig.configs')
if not configs.tern then
  configs.tern = {
    default_config = {
      cmd = { 'tern', 'lsp' },
      filetypes = { 'tern' },
      root_dir = require('lspconfig.util').root_pattern('tern.toml', '.git'),
      single_file_support = true,
    },
  }
end
require('lspconfig').tern.setup({})
```

Neovim 0.11 and later need no plugin:

```lua
vim.filetype.add({ extension = { tern = 'tern' } })
vim.lsp.config('tern', {
  cmd = { 'tern', 'lsp' },
  filetypes = { 'tern' },
  root_markers = { 'tern.toml', '.git' },
})
vim.lsp.enable('tern')
```

Format on save: `vim.lsp.buf.format()`; the instance tree:
`vim.lsp.buf_request(0, 'tern/instanceTree', { textDocument = vim.lsp.util.make_text_document_params() }, function(_, r) vim.print(r) end)`.

## Helix

In `~/.config/helix/languages.toml`:

```toml
[language-server.tern]
command = "tern"
args = ["lsp"]

[[language]]
name = "tern"
scope = "source.tern"
file-types = ["tern"]
roots = ["tern.toml"]
comment-token = "//"
block-comment-tokens = { start = "/*", end = "*/" }
indent = { tab-width = 4, unit = "    " }
language-servers = ["tern"]
auto-format = true
```

## Emacs (eglot)

```elisp
(define-derived-mode tern-mode prog-mode "Tern"
  "Major mode for Tern hardware descriptions."
  (setq-local comment-start "// ")
  (setq-local comment-end ""))
(add-to-list 'auto-mode-alist '("\\.tern\\'" . tern-mode))

(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs '(tern-mode . ("tern" "lsp"))))
(add-hook 'tern-mode-hook #'eglot-ensure)
```

`M-x eglot-format-buffer` formats with `tern fmt`.

## Other clients

Any client works with the command `tern lsp` over stdio, language id `tern`
and the file extension `.tern`. Positions are UTF-16 code units (the LSP
default); text synchronization is incremental.
