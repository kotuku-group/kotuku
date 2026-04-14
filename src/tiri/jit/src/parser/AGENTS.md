# Parser Implementation Notes

This file captures parser-specific practices and gotchas for the Tiri JIT parser that ships inside Kōtuku. The parser
has been modernised to C++20 and implements a two-phase architecture: AST construction followed by IR emission.

The parser is built as an amalgamated build, with all source files included in a single compilation unit at
`parser.cpp`. This simplifies dependency management and ensures consistent compiler settings across the entire parser
codebase.

## Architecture Overview

The parser follows a classic two-phase compilation model:

1. **AST Building Phase** (`ast/`) - Parses source tokens into a typed Abstract Syntax Tree
2. **IR Emission Phase** (`ir_emitter/`) - Lowers the AST to LuaJIT bytecode

This separation allows the AST and bytecode emission to evolve independently while sharing a single AST boundary.

## Parser File Structure

### Core Infrastructure

| File | Purpose |
|------|---------|
| `parse_types.h` | Type definitions, enums, constexpr helper functions for expressions and scopes |
| `parse_concepts.h` | C++20 concepts for compile-time type validation (15+ concepts) |
| `parse_raii.h` | RAII guards (ScopeGuard, RegisterGuard, VStackGuard) with move semantics |
| `parse_internal.h` | Internal function declarations and template helpers for bytecode emission |
| `parse_value.h` | Value type definitions |

### Lexer

| File | Purpose |
|------|---------|
| `lexer.h` / `lexer.cpp` | Tokenisation and lexical analysis |
| `token_types.h` / `token_types.cpp` | Strongly typed `TokenKind` enum and `Token` class with payloads |
| `token_stream.h` / `token_stream.cpp` | Token stream adapter for lookahead and consumption |

### Parser Context and Configuration

| File | Purpose |
|------|---------|
| `parser.h` / `parser.cpp` | Main parser entry point (`lj_parse`) |
| `parser_context.h` / `parser_context.cpp` | `ParserContext` class managing lexer state, diagnostics, configuration, and `ParserConfig` struct |
| `parser_diagnostics.h` / `parser_diagnostics.cpp` | `ParserDiagnostics` for error and warning collection |
| `parser_tips.h` / `parser_tips.cpp` | Optional tips system for IDE integration |
| `parser_profiler.h` | Profiling instrumentation for parser performance analysis |

### AST Building (`ast/` subdirectory)

| File | Purpose |
|------|---------|
| `nodes.h` / `nodes.cpp` | Complete AST node schema with 30+ node types |
| `builder.h` / `builder.cpp` | `AstBuilder` class entry point and core infrastructure |
| `expressions.cpp` | Expression parsing (primary, suffix, binary, unary, ternary) |
| `literals.cpp` | Literal parsing (numbers, strings, tables, functions) |
| `statements.cpp` | Statement parsing (assignments, declarations, returns) |
| `loops.cpp` | Loop parsing (for, while, repeat, break, continue) |
| `choose.cpp` | Pattern-matching `choose` expression parsing |
| `annotations.cpp` | Annotation (`@`) parsing for function metadata |

### IR Emission (`ir_emitter/` subdirectory)

| File | Purpose |
|------|---------|
| `ir_emitter.h` / `ir_emitter.cpp` | `IrEmitter` class that lowers AST to bytecode |
| `operator_emitter.h` / `operator_emitter.cpp` | Operator-specific bytecode emission (arithmetic, logical, bitwise) |
| `emit_assignment.cpp` | Assignment statement emission (plain, compound, conditional) |
| `emit_call.cpp` | Function call emission |
| `emit_choose.cpp` | `choose` expression bytecode generation |
| `emit_function.cpp` | Function literal and closure emission |
| `emit_global.cpp` | Global declaration emission with const tracking |
| `emit_table.cpp` | Table constructor emission |
| `emit_try.cpp` | Try-except exception handling bytecode generation |

### Register and Control Flow Management

| File | Purpose |
|------|---------|
| `parse_regalloc.h` / `parse_regalloc.cpp` | `RegisterAllocator` class and expression discharge logic |
| `parse_control_flow.h` / `parse_control_flow.cpp` | `ControlFlowGraph` for jump patching and branching |
| `parse_scope.cpp` | Scope management, variable resolution, and upvalue tracking |
| `parse_constants.cpp` | Constant table management and jump list handling |

### Type System

| File | Purpose |
|------|---------|
| `type_checker.h` / `type_checker.cpp` | `TypeCheckScope` for static type validation |
| `type_analysis.cpp` | Static type analysis pass over the AST |
| `value_categories.h` / `value_categories.cpp` | `ExprValue`, `RValue`, `LValue` hierarchy |

### Legacy/Support Files

| File | Purpose |
|------|---------|
| `parse_expr.cpp` | Legacy expression parsing utilities |
| `parser_unit_tests.cpp` | Unit tests for parser components |

## Key Classes

### `ParserContext`
Central context object that bundles:
- `LexState` reference for tokenisation
- `FuncState` reference for bytecode state
- `ParserDiagnostics` for error collection
- `TokenStreamAdapter` for token consumption
- `ParserConfig` for behaviour options

### `AstBuilder`
Constructs a typed AST from the token stream. Entry point is `parse_chunk()` which returns a `BlockStmt` root node.
The builder is stateless between chunks and does not touch `FuncState` or emit bytecode.

### `IrEmitter`
Lowers AST nodes to LuaJIT bytecode. Entry point is `emit_chunk()` which consumes a `BlockStmt` and populates
`FuncState` with bytecode instructions. Uses `RegisterAllocator` and `ControlFlowGraph` for resource management.

### `RegisterAllocator`
Manages register allocation with RAII-based `RegisterSpan` for automatic cleanup. Tracks the register floor and
provides debugging assertions for register balance.

### `ControlFlowGraph`
Manages jump patching for branching constructs. Provides `ControlFlowEdge` for forward jumps that are resolved when
targets become known.

## Key Implementation Patterns

### Type Safety with Concepts
```cpp
// Use concepts for compile-time validation
template<BytecodeOpcode Op>
static inline BCPOS bcemit_ABC(FuncState *, Op, BCREG, BCREG, BCREG);
```

### RAII for Resource Management
```cpp
// Automatic scope cleanup
FuncScope bl;
ScopeGuard scope_guard(fs, &bl, FuncScopeFlag::None);
// ... parse statements ...
// Automatic cleanup on scope exit
```

### Result Types for Error Handling
```cpp
// ParserResult<T> wraps success/failure without exceptions
ParserResult<ExprNodePtr> result = parse_expression();
if (not result.ok()) return ParserResult<StmtNodePtr>::failure(result.error_ref());
```

### AST Node Ownership
```cpp
// Nodes own children through unique_ptr
using ExprNodePtr = std::unique_ptr<ExprNode>;
using StmtNodePtr = std::unique_ptr<StmtNode>;
using ExprNodeList = std::vector<ExprNodePtr>;
```

### Modern Container Usage
```cpp
// Prefer std::span for array access
auto uvmap_range = std::span(fs->uvmap.data(), fs->nuv);
for (auto uv_idx : uvmap_range) { ... }

// Use std::string_view for string parameters
static GCstr * keepstr(std::string_view str);
```

## Quick Reference

### File Locations
- Parser source: `src/tiri/jit/src/parser/`
- AST definitions: `src/tiri/jit/src/parser/ast/`
- IR emission: `src/tiri/jit/src/parser/ir_emitter/`
- Bytecode reference: `src/tiri/jit/BYTECODE.md`
- Tiri tests: `src/tiri/tests/`
- Troubleshooting Guide: `src/tiri/jit/src/parser/TROUBLESHOOTING.md`

### Common Tasks
- **Adding an AST node**: Define struct in `nodes.h`, add `AstNodeKind` enum entry, implement parsing in
  `builder_*.cpp`, implement emission in `ir_emitter/emit_*.cpp`
- **Adding an operator**: Add to `AstBinaryOperator`/`AstUnaryOperator` enum, handle in `expressions.cpp`,
  emit in `operator_emitter.cpp`
- **Register allocation**: Use `RegisterAllocator::reserve()` for temporary registers, ensure balance with
  `ensure_register_floor()`
- **Understanding concepts**: See `parse_concepts.h` for type constraints
- **Expression builders**: See `parse_types.h` for constexpr factories

### Key Headers
| Header | Purpose |
|--------|---------|
| `ast/nodes.h` | AST node types and payloads |
| `ast/builder.h` | AST construction interface |
| `ir_emitter/ir_emitter.h` | Bytecode emission interface |
| `parser_context.h` | Central parser context |
| `parse_types.h` | Core types and expression builders |
| `parse_concepts.h` | C++20 type constraints |
| `parse_raii.h` | RAII guards |
| `token_types.h` | Token representation |

## AST Node Hierarchy

The AST uses a discriminated union pattern:

```
ExprNode (AstNodeKind discriminator)
├── Literals: Nil, Boolean, Number, String
├── References: Identifier, Vararg
├── Operations: Unary, Binary, Ternary, Update, Presence
├── Access: Member, Index, SafeMember, SafeIndex
├── Calls: Call, SafeCall, Pipe
├── Constructors: Table, Function, Range
└── Extensions: Choose, ResultFilter

StmtNode (AstNodeKind discriminator)
├── Declarations: LocalDecl, GlobalDecl, LocalFunction, Function
├── Control Flow: If, While, Repeat, NumericFor, GenericFor
├── Jumps: Break, Continue, Return
├── Blocks: Block, Defer
└── Expressions: ExpressionStmt, Assignment, ConditionalShorthand
```

## Type System Overview

The parser includes optional static type analysis:

- `TiriType` enum defines supported types (Nil, Boolean, Number, String, Table, Function, etc.)
- `FunctionReturnTypes` captures declared return type annotations
- `TypeCheckScope` tracks variable types within scopes
- Type mismatches can be configured as errors or warnings via `ParserConfig`

Type annotations use the syntax `local x: number = 42` and function return types use `: <type1, type2>`.
