// Tern for VS Code: starts `tern lsp` over stdio (see docs/editors.md).
'use strict';

const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;

function serverOptions() {
  const cfg = vscode.workspace.getConfiguration('tern');
  const args = ['lsp'];
  const log = cfg.get('lsp.log');
  if (log) args.push('--log', log);
  return { command: cfg.get('path') || 'tern', args };
}

async function start() {
  client = new LanguageClient('tern', 'Tern', serverOptions(), {
    documentSelector: [{ scheme: 'file', language: 'tern' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/{*.tern,tern.toml}'),
    },
  });
  await client.start();
}

function printTree(nodes, depth, out) {
  for (const n of nodes) {
    const params = n.params.map((p) => `${p.name}=${p.value}`).join(', ');
    const label = depth === 0 ? n.display : `${n.name}: ${n.display}`;
    out.push(`${'  '.repeat(depth)}${label}${params ? `  (${params})` : ''}`);
    printTree(n.children, depth + 1, out);
  }
}

async function showInstanceTree() {
  if (!client) return;
  const editor = vscode.window.activeTextEditor;
  const params = editor && editor.document.languageId === 'tern'
    ? { textDocument: { uri: editor.document.uri.toString() } }
    : {};
  const tree = await client.sendRequest('tern/instanceTree', params);
  const lines = [];
  printTree(tree, 0, lines);
  const doc = await vscode.workspace.openTextDocument({
    content: lines.join('\n') || '(nothing elaborated)',
    language: 'plaintext',
  });
  await vscode.window.showTextDocument(doc, { preview: true });
}

async function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('tern.instanceTree', showInstanceTree),
    vscode.commands.registerCommand('tern.restartServer', async () => {
      if (client) await client.stop();
      await start();
    }),
  );
  await start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
