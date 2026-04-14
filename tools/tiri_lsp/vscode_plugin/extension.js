// Tiri Language Support for VS Code
// Connects to the Tiri LSP server over TCP

const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');
const net = require('net');

let client;
let outputChannel;

function activate(context) {
   outputChannel = vscode.window.createOutputChannel('Tiri LSP');
   outputChannel.appendLine('Tiri extension activated');

   const config = vscode.workspace.getConfiguration('tiri.lsp');
   const enabled = config.get('enable', true);

   if (!enabled) {
      outputChannel.appendLine('LSP connection disabled in settings');
      return;
   }

   const host = config.get('host', '127.0.0.1');
   const port = config.get('port', 5007);

   outputChannel.appendLine(`Connecting to LSP server at ${host}:${port}`);

   // Server options: connect via TCP socket
   const serverOptions = () => {
      return new Promise((resolve, reject) => {
         const socket = net.connect({ port: port, host: host });

         socket.on('connect', () => {
            outputChannel.appendLine('Connected to LSP server');
            resolve({
               reader: socket,
               writer: socket
            });
         });

         socket.on('error', (err) => {
            outputChannel.appendLine(`Connection error: ${err.message}`);
            vscode.window.showWarningMessage(
               `Tiri LSP: Could not connect to server at ${host}:${port}. ` +
               `Start the server with: origo tools/lsp_server.tiri port=${port}`
            );
            reject(err);
         });
      });
   };

   // Client options
   const clientOptions = {
      documentSelector: [
         { scheme: 'file', language: 'tiri' }
      ],
      outputChannel: outputChannel,
      synchronize: {
         fileEvents: vscode.workspace.createFileSystemWatcher('**/*.tiri')
      }
   };

   // Create and start the language client
   client = new LanguageClient(
      'tiri-lsp',
      'Tiri Language Server',
      serverOptions,
      clientOptions
   );

   // Start the client (also launches the server if needed)
   client.start().then(() => {
      outputChannel.appendLine('LSP client started successfully');
   }).catch((err) => {
      outputChannel.appendLine(`Failed to start LSP client: ${err.message}`);
   });

   // Register command to restart LSP connection
   const restartCommand = vscode.commands.registerCommand('tiri.restartLsp', async () => {
      outputChannel.appendLine('Restarting LSP connection...');
      if (client) {
         await client.stop();
      }
      client.start();
   });

   context.subscriptions.push(restartCommand);
   context.subscriptions.push(outputChannel);
}

function deactivate() {
   if (outputChannel) {
      outputChannel.appendLine('Tiri extension deactivating');
   }
   if (client) {
      return client.stop();
   }
   return undefined;
}

module.exports = { activate, deactivate };
