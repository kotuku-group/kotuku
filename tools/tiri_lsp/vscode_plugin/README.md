# Tiri Language Support for VS Code

Language support for Tiri scripting (Kōtuku), including syntax highlighting and LSP integration.

## Setup

### 1. Install Dependencies

```bash
cd tools/tiri_lsp/vscode_plugin
npm install
```

### 2. Start the LSP Server

By default, the extension starts the Tiri LSP server automatically for local connections. To start it manually instead, set `tiri.lsp.autoStart` to `false` and run:

```bash
origo tools/tiri_lsp/server.tiri port=5007
```

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

The extension auto-starts the server with `origo tools/tiri_lsp/server.tiri port=5007`. Configure `tiri.lsp.origoPath` if `origo` is not in PATH, or `tiri.lsp.serverScript` if the server script is in another location.

### "Could not connect to server"

1. Ensure the LSP server is running.
2. Check the port matches your VS Code settings
3. View the Tiri LSP output channel for connection details:
   - View > Output > Select "Tiri LSP" from dropdown

### No Syntax Highlighting

Ensure the opened file has a `.tiri` extension.
In the bottom right corner, ensure that the Language Mode is set to Tiri.
