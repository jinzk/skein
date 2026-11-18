'use strict';

const path = require('path');
const vscode = require('vscode');

let languageClient = null;
let languageClientStart = null;

function configuration() {
  return vscode.workspace.getConfiguration('skein');
}

function executablePath() {
  let executable = configuration().get('executablePath', 'skein');
  executable = typeof executable === 'string' ? executable.trim() : '';
  if (!executable) {
    executable = 'skein';
  }
  if (path.isAbsolute(executable)) {
    return executable;
  }
  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0 && /[\\/]/.test(executable)) {
    return path.resolve(folders[0].uri.fsPath, executable);
  }
  return executable;
}

function quote(value) {
  const text = String(value);
  if (process.platform === 'win32') {
    return '"' + text.replace(/"/g, '""') + '"';
  }
  return "'" + text.replace(/'/g, "'\\''") + "'";
}

function getTerminal(name) {
  const existing = vscode.window.terminals.find(
    (term) => term.name === name
  );
  return existing || vscode.window.createTerminal(name);
}

async function ensureSaved(document) {
  if (document.isUntitled) {
    return await document.save();
  }
  if (document.isDirty && configuration().get('saveBeforeRun', true)) {
    await document.save();
  }
  return true;
}

async function runFile() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'skein') {
    vscode.window.showErrorMessage('Skein: open a .skein file first.');
    return;
  }

  if (!(await ensureSaved(editor.document))) {
    return;
  }

  const file = editor.document.uri.fsPath;
  const configured = configuration().get('extraArgs', []);
  const extra = Array.isArray(configured) ? configured.map(String) : [];
  const command = [executablePath(), ...extra, file].map(quote).join(' ');
  const terminal = getTerminal('Skein');
  terminal.show(true);
  terminal.sendText(command, true);
}

function openRepl() {
  const terminal = getTerminal('Skein REPL');
  terminal.show(true);
  terminal.sendText(quote(executablePath()) + ' -i', true);
}

async function formatDocument() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'skein') {
    vscode.window.showErrorMessage('Skein: open a .skein file first.');
    return;
  }
  await vscode.commands.executeCommand('editor.action.formatDocument');
}

function serverArgs() {
  const args = ['lsp', '--stdio'];
  const configured = configuration().get('packagePaths', []);
  const paths = Array.isArray(configured) ? configured.map(String) : [];
  for (const entry of paths) {
    if (entry) {
      args.push('-I', entry);
    }
  }
  args.push('--typecheck=' + configuration().get('typecheck', 'warn'));
  return args;
}

function stopLanguageClient() {
  if (languageClientStart) {
    languageClientStart.dispose();
    languageClientStart = null;
  }
  languageClient = null;
}

function startLanguageClient() {
  if (!configuration().get('lsp.enabled', true)) {
    stopLanguageClient();
    return;
  }

  let clientModule;
  try {
    clientModule = require('vscode-languageclient/node');
  } catch (error) {
    vscode.window.showWarningMessage(
      'Skein: vscode-languageclient is not installed; LSP features are disabled.'
    );
    return;
  }

  const { LanguageClient, TransportKind } = clientModule;
  const serverOptions = {
    run: {
      command: executablePath(),
      args: serverArgs(),
      transport: TransportKind.stdio,
    },
    debug: {
      command: executablePath(),
      args: serverArgs(),
      transport: TransportKind.stdio,
    },
  };
  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'skein' },
      { scheme: 'untitled', language: 'skein' },
    ],
    synchronize: {
      configurationSection: 'skein',
      filePatterns: '**/*.skein',
    },
  };

  languageClient = new LanguageClient(
    'skein',
    'Skein Language Server',
    serverOptions,
    clientOptions
  );
  languageClientStart = languageClient.start();
}

function watchConfiguration(context) {
  const keys = [
    'skein.lsp.enabled',
    'skein.executablePath',
    'skein.typecheck',
    'skein.packagePaths',
  ];
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (keys.some((key) => event.affectsConfiguration(key))) {
        stopLanguageClient();
        startLanguageClient();
      }
    })
  );
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('skein.runFile', runFile),
    vscode.commands.registerCommand('skein.openRepl', openRepl),
    vscode.commands.registerCommand('skein.formatDocument', formatDocument),
    { dispose: stopLanguageClient }
  );
  startLanguageClient();
  watchConfiguration(context);
}

function deactivate() {
  stopLanguageClient();
}

module.exports = {
  activate,
  deactivate
};
