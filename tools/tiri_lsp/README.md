# Tiri LSP

A Language Server Protocol (LSP) implementation for Tiri scripting, providing IDE integration features for Kōtuku.

## Table of Contents

1. [Overview](#overview)
2. [Features](#features)
   - [Diagnostics](#diagnostics)
   - [Semantic Highlighting](#semantic-highlighting)
   - [Hover Information](#hover-information)
   - [Document Symbols](#document-symbols)
   - [Code Folding](#code-folding)
   - [Code Hints](#code-hints)
3. [Getting Started](#getting-started)
   - [Prerequisites](#prerequisites)
   - [Starting the Server](#starting-the-server)
   - [Configuration Options](#configuration-options)
4. [VSCode Extension](#vscode-extension)
   - [Installation](#installation)
   - [Extension Settings](#extension-settings)
5. [Architecture](#architecture)
   - [File Structure](#file-structure)
   - [LSP Message Flow](#lsp-message-flow)
   - [Documentation Cache](#documentation-cache)
6. [Supported LSP Methods](#supported-lsp-methods)
7. [Extending the Server](#extending-the-server)
8. [Testing](#testing)
9. [Troubleshooting](#troubleshooting)

## Overview

The Tiri LSP server provides language intelligence for Tiri files in any LSP-compatible editor. It communicates over TCP and offers real-time feedback as you write Tiri code.

## Features

### Diagnostics

Real-time syntax error detection with precise location information. Errors, warnings, and informational messages are
reported as you type.

### Semantic Highlighting

Full semantic token support for accurate syntax highlighting:
- Keywords and operators
- Functions and parameters
- Variables and properties
- Classes, types, and namespaces
- Strings, numbers, and comments
- Built-in functions (marked with `defaultLibrary` modifier)

### Hover Information

Context-aware documentation on hover:
- Tiri keywords with descriptions
- Built-in functions with prototypes
- Kōtuku API classes, methods, fields, and actions
- Module functions (e.g., `mSys.Sleep`, `mNet.AddressToStr`)
- Links to online documentation

### Document Symbols

Outline view showing:
- Global and local functions
- Function scope (global/local/thunk)
- Nested function support

### Code Folding

Intelligent folding for:
- Functions
- Control structures (`if`, `for`, `while`, `repeat`)
- Multi-line comments
- `defer` blocks

### Code Hints

Parser-generated tips for code improvement (LSP severity 4), including:
- Type safety suggestions
- Performance recommendations
- Code quality improvements
- Best practice hints
- Style suggestions

## Getting Started

### Prerequisites

- Kōtuku installed and built
- `origo` executable available in PATH

### Starting the Server

```bash
origo tools/tiri_lsp/server.tiri port=5007
```

### Configuration Options

| Parameter     | Default                            | Description                          |
|---------------|------------------------------------|------------------------------------- |
| `port`        | 5007                               | TCP port to listen on                |
| `verbose`     | false                              | Enable debug logging                 |
| `config`      | `user:config/lsp_server_cfg.tiri` | Path to configuration file           |
| `request-log` | (none)                             | Path for request/response logging    |

Example with options:
```bash
origo server.tiri port=5007 verbose=true request-log=debug.log
```

## VSCode Extension

### Installation

1. Open VSCode
2. Go to Extensions (Ctrl+Shift+X)
3. Click "..." menu → "Install from VSIX..."
4. Select `tools/tiri_lsp/vscode_plugin/tiri-language-0.1.0.vsix`

Or build from source:
```bash
cd tools/tiri_lsp/vscode_plugin
npm install
npx vsce package
```

### Extension Settings

Configure in VSCode settings:

|Setting|Default|Description|
|-|-|-|
|`tiri.lsp.enable`|true|Enable LSP server connection|
|`tiri.lsp.host`|"127.0.0.1"|LSP server host address|
|`tiri.lsp.port`|5007|LSP server TCP port|

## Architecture

### File Structure

```
tools/tiri_lsp/
├── server.tiri      # Main server entry point
├── lsp_lib.tiri         # Core LSP implementation
├── lsp_defs.tiri        # Keyword/builtin definitions
├── README.md             # This file
└── vscode_plugin/        # VSCode extension
    ├── extension.js      # Extension entry point
    ├── package.json      # Extension manifest
    ├── syntaxes/         # TextMate grammar
    └── *.vsix            # Packaged extension
```

### LSP Message Flow

1. Client connects via TCP
2. `initialize` handshake establishes capabilities
3. `textDocument/didOpen` begins document tracking
4. `textDocument/didChange` triggers re-parsing and diagnostics
5. Feature requests (hover, symbols, etc.) are processed on demand
6. `shutdown`/`exit` terminates the session

### Documentation Cache

API documentation is parsed from `docs/xml/` and cached at `user:config/lsp_doc_cache.tiri` for faster startup.
The cache is automatically rebuilt when source XML files change.

## Supported LSP Methods

|Method|Description|
|-|-|
|`initialize`| Capability negotiation|
|`initialized`| Post-init notification|
|`shutdown`| Graceful shutdown request|
|`exit`| Server termination|
|`$/cancelRequest`| Cancel pending request|
|`textDocument/didOpen`|Document opened|
|`textDocument/didChange`|Document modified|
|`textDocument/didClose`|Document closed|
|`textDocument/hover`|Hover information|
|`textDocument/documentSymbol`|Document outline|
|`textDocument/foldingRange`|Folding ranges|
|`textDocument/semanticTokens/full`|Full semantic tokens|
|`textDocument/semanticTokens/full/delta`|Incremental semantic tokens|
|`textDocument/publishDiagnostics`|Error/warning/hint reporting|

## Extending the Server

Register custom handlers for additional LSP methods:

```lua
server = lspStart({ port = 5007 })

server.registerHandler('textDocument/completion', function(msg, state)
   -- Return completion items
   return {
      isIncomplete = false,
      items = {
         { label = 'example', kind = 1 }
      }
   }
end)
```

## Testing

Test with netcat or telnet:

```bash
# Start server
origo server.tiri port=5007

# In another terminal
nc localhost 5007

# Send initialize request
Content-Length: 110

{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":1,"capabilities":{},"rootUri":null}}
```

## Troubleshooting

**Server won't start**
- Ensure port is not already in use
- Check that `origo` is accessible
- Verify the Kōtuku SDK path can be resolved

**No diagnostics appearing**
- Check that the LSP client is connected (look for server logs)
- Verify `tiri.lsp.enable` is true in VSCode settings
- Ensure the server and client are using the same port

**Documentation not showing on hover**
- The doc cache may need rebuilding; delete `user:config/lsp_doc_cache.tiri`
- Ensure `docs/xml/` contains the API documentation files

**Verbose debugging**
- Start server with `verbose=true` for detailed logging
- Use `request-log=path.log` to capture all LSP messages
