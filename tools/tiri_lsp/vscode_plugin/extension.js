// Tiri Language Support for VS Code
// Connects to the Tiri LSP server over stdio or TCP

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
      port: config.get('port', 0),
      autoStart: config.get('autoStart', false),
      origoPath: config.get('origoPath', 'origo'),
      serverScript: config.get('serverScript', 'tools/tiri_lsp/server.tiri'),
      logApi: config.get('logApi', false),
      logFile: config.get('logFile', '')
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

function getLogFilePath(settings, port) {
   const configuredPath = settings.logFile && settings.logFile.trim();
   if (configuredPath) {
      return configuredPath;
   }

   if (port === 0 && settings.logApi) {
      const workspacePath = getWorkspacePath();
      return path.join(workspacePath || process.cwd(), 'tiri-lsp-core.log');
   }

   return '';
}

function getServerArgs(settings, serverScript, port) {
   const args = [];
   const logFile = getLogFilePath(settings, port);

   if (settings.logApi) {
      args.push('--log-api');
   }

   if (logFile) {
      args.push('--log-file', logFile);
   }

   args.push(serverScript, `port=${port}`);
   return args;
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

   const args = getServerArgs(settings, serverScript, settings.port);
   outputChannel.appendLine(`Starting Tiri LSP server: ${settings.origoPath} ${args.join(' ')}`);

   serverProcess = spawn(settings.origoPath, args, {
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

function createStdioServerOptions(settings) {
   const serverScript = resolveServerScript(settings.serverScript);
   if (!serverScript) {
      outputChannel.appendLine('Cannot start Tiri LSP server: server script is relative and no workspace is open');
      vscode.window.showWarningMessage(
         'Tiri LSP: Set tiri.lsp.serverScript to an absolute path, or open the Kōtuku workspace.'
      );
      throw new Error('Unable to resolve Tiri LSP server script');
   }

   const args = getServerArgs(settings, serverScript, 0);
   outputChannel.appendLine(`Starting LSP server in stdio mode: ${settings.origoPath} ${args.join(' ')}`);

   return {
      command: settings.origoPath,
      args: args,
      options: {
         cwd: getWorkspacePath(),
         windowsHide: true
      }
   };
}

async function stopLanguageClient() {
   if (client) {
      const oldClient = client;
      client = undefined;
      await oldClient.stop();
   }
}

function createLanguageClient(settings) {
   const serverOptions = settings.port === 0
      ? createStdioServerOptions(settings)
      : () => connectToServer(settings);

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

   if (settings.port === 0) {
      outputChannel.appendLine('Using stdio mode for LSP server');
   }
   else {
      outputChannel.appendLine(`Connecting to LSP server at ${settings.host}:${settings.port}`);
   }

   let newClient;

   try {
      newClient = createLanguageClient(settings);
   }
   catch (err) {
      outputChannel.appendLine(`Failed to create LSP client: ${err.message}`);
      return;
   }

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
