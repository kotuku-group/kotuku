# Tiri Language Support for VS Code

Language support for Tiri scripting (Kōtuku), including syntax highlighting and LSP integration.

## Setup

### 1. Install Dependencies

```bash
cd tools/tiri_lsp/vscode_plugin
npm install
```

### 2. LSP Server Startup

By default, the extension looks for an existing Tiri LSP server on port 5007.  This commandline starts the server manually with the default port:

```bash
origo tools/tiri_lsp/server.tiri port=5007
```

If `tiri.lsp.autoStart` is enabled, the plugin will start the server on the defined `port`.  If `port=0`, the server runs in stdio mode.

### 3. Install the Extension

**Option A: Install via VS Code Command Palette (Recommended)**

1. Package the extension:
   ```bash
   cd tools/tiri_lsp/vscode_plugin
   npx vsce package
   ```

2. In VS Code, press `Ctrl+Shift+P`
3. Type **"Extensions: Install from VSIX"** and select it
4. Navigate to `tools/tiri_lsp/vscode_plugin/tiri-language-0.1.0.vsix`
5. Click Install

**Option B: Install From Command Line**

```bash
# Package the extension
npx vsce package

# Install the .vsix file (requires 'code' in PATH)
code --install-extension tiri-language-0.1.0.vsix
```

## Troubleshooting

### LSP Server Auto-Start

If `port=0` or `tiri.lsp.autoStart` is enabled, the server script is started automatically.  Configure `tiri.lsp.origoPath` if `origo` is not in PATH, or `tiri.lsp.serverScript` if the server script is in another location.

### "Could not connect to server"

1. For TCP mode, ensure the LSP server is running.
2. Check the port matches your VS Code settings.
3. View the Tiri LSP output channel for connection details:
   - View > Output > Select "Tiri LSP" from dropdown

### No Syntax Highlighting

Ensure the opened file has a `.tiri` extension.
In the bottom right corner, ensure that the Language Mode is set to Tiri.
