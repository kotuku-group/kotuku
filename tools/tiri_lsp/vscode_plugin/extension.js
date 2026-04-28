// Tiri Language Support for VS Code
// Connects to the Tiri LSP server over TCP

const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');
const { spawn } = require('child_process');
const net = require('net');
const path = require('path');

let client;
let outputChannel;
let serverProcess;

function getSettings() {
   const config = vscode.workspace.getConfiguration('tiri.lsp');
   return {
      enabled: config.get('enable', true),
      host: config.get('host', '127.0.0.1'),
      port: config.get('port', 5007),
      autoStart: config.get('autoStart', false),
      origoPath: config.get('origoPath', 'origo'),
      serverScript: config.get('serverScript', 'tools/tiri_lsp/server.tiri')
   };
}

function isLocalHost(host) {
   return host === '127.0.0.1' || host === 'localhost' || host === '::1';
}

function getWorkspacePath() {
   const folders = vscode.workspace.workspaceFolders;
   if (folders && folders.length > 0) {
      return folders[0].uri.fsPath;
   }
   return undefined;
}

function resolveServerScript(serverScript) {
   if (path.isAbsolute(serverScript)) {
      return serverScript;
   }

   const workspacePath = getWorkspacePath();
   if (workspacePath) {
      return path.join(workspacePath, serverScript);
   }

   return undefined;
}

function wait(ms) {
   return new Promise((resolve) => setTimeout(resolve, ms));
}

function openSocket(settings) {
   return new Promise((resolve, reject) => {
      const socket = net.connect({ port: settings.port, host: settings.host });

      socket.on('connect', () => {
         outputChannel.appendLine('Connected to LSP server');
         resolve({
            reader: socket,
            writer: socket
         });
      });

      socket.on('error', (err) => {
         socket.destroy();
         reject(err);
      });
   });
}

function startServerProcess(settings) {
   if (!settings.autoStart || !isLocalHost(settings.host) || serverProcess) {
      return;
   }

   const serverScript = resolveServerScript(settings.serverScript);
   if (!serverScript) {
      outputChannel.appendLine('Cannot auto-start Tiri LSP server: server script is relative and no workspace is open');
      vscode.window.showWarningMessage(
         'Tiri LSP: Set tiri.lsp.serverScript to an absolute path, or open the Kōtuku workspace.'
      );
      return;
   }

   const cwd = getWorkspacePath();

   outputChannel.appendLine(`Starting Tiri LSP server: ${settings.origoPath} ${serverScript} port=${settings.port}`);

   serverProcess = spawn(settings.origoPath, [serverScript, `port=${settings.port}`], {
      cwd: cwd,
      windowsHide: true
   });

   serverProcess.stdout.on('data', (data) => {
      outputChannel.append(data.toString());
   });

   serverProcess.stderr.on('data', (data) => {
      outputChannel.append(data.toString());
   });

   serverProcess.on('error', (err) => {
      outputChannel.appendLine(`Failed to start Tiri LSP server: ${err.message}`);
      vscode.window.showWarningMessage(`Tiri LSP: Could not start server with ${settings.origoPath}.`);
      serverProcess = undefined;
   });

   serverProcess.on('exit', (code, signal) => {
      outputChannel.appendLine(`Tiri LSP server exited with code ${code}, signal ${signal}`);
      serverProcess = undefined;
   });
}

function stopServerProcess() {
   if (serverProcess) {
      outputChannel.appendLine('Stopping Tiri LSP server process');
      serverProcess.kill();
      serverProcess = undefined;
   }
}

async function connectToServer(settings) {
   const attempts = settings.autoStart && isLocalHost(settings.host) ? 10 : 1;
   let lastError;

   for (let attempt = 0; attempt < attempts; attempt++) {
      try {
         return await openSocket(settings);
      }
      catch (err) {
         lastError = err;

         if (attempt === 0) {
            outputChannel.appendLine(`Connection error: ${err.message}`);
            startServerProcess(settings);
         }

         if (attempt + 1 < attempts) {
            await wait(500);
         }
      }
   }

   vscode.window.showWarningMessage(
      `Tiri LSP: Could not connect to server at ${settings.host}:${settings.port}. ` +
      `Start the server with: origo tools/tiri_lsp/server.tiri port=${settings.port}`
   );

   throw lastError;
}

async function stopLanguageClient() {
   if (client) {
      const oldClient = client;
      client = undefined;
      await oldClient.stop();
   }
}

function createLanguageClient(settings) {
   const serverOptions = () => connectToServer(settings);

   const clientOptions = {
      documentSelector: [
         { scheme: 'file', language: 'tiri' }
      ],
      outputChannel: outputChannel,
      synchronize: {
         fileEvents: vscode.workspace.createFileSystemWatcher('**/*.tiri')
      }
   };

   return new LanguageClient(
      'tiri-lsp',
      'Tiri Language Server',
      serverOptions,
      clientOptions
   );
}

async function startLanguageClient() {
   const settings = getSettings();

   if (!settings.enabled) {
      outputChannel.appendLine('LSP connection disabled in settings');
      return;
   }

   outputChannel.appendLine(`Connecting to LSP server at ${settings.host}:${settings.port}`);

   const newClient = createLanguageClient(settings);
   client = newClient;

   try {
      await newClient.start();
      outputChannel.appendLine('LSP client started successfully');
   }
   catch (err) {
      outputChannel.appendLine(`Failed to start LSP client: ${err.message}`);
      if (client === newClient) {
         client = undefined;
      }
   }
}

async function restartLanguageClient() {
   outputChannel.appendLine('Restarting LSP connection...');
   await stopLanguageClient();
   stopServerProcess();
   await startLanguageClient();
}

function activate(context) {
   outputChannel = vscode.window.createOutputChannel('Tiri LSP');
   outputChannel.appendLine('Tiri extension activated');

   startLanguageClient();

   const restartCommand = vscode.commands.registerCommand('tiri.restartLsp', async () => {
      await restartLanguageClient();
   });

   const configWatcher = vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (event.affectsConfiguration('tiri.lsp')) {
         await restartLanguageClient();
      }
   });

   context.subscriptions.push(restartCommand);
   context.subscriptions.push(configWatcher);
   context.subscriptions.push(outputChannel);
}

async function deactivate() {
   if (outputChannel) {
      outputChannel.appendLine('Tiri extension deactivating');
   }
   await stopLanguageClient();
   stopServerProcess();
}

module.exports = { activate, deactivate };
