# Parser Implementation Notes

This file describes the C++20 Tiri parser embedded in Kōtuku.  Read the parent `src/tiri/jit/AGENTS.md` for JIT
build, test and runtime conventions that also apply here.

## Compilation Layout

Most parser implementation files form a unity build rooted at `parser.cpp`.  That file includes the AST, semantic
analysis and IR-emission implementation files directly.  `ast/builder.cpp` and `ir_emitter/ir_emitter.cpp` similarly
include their specialised implementation files at the end.

The lexer (`lexer.cpp`), imported-interface loader (`import_interface.cpp`), parser unit tests
(`parser_unit_tests.cpp`) and, when enabled, tips implementation (`parser_tips.cpp`) are separate CMake sources.  Do
not add an included implementation file to `TIRI_SOURCES`; doing so will compile it twice.

## Compilation Pipeline

The parser is organised around an AST boundary, but the complete pipeline has several passes:

1. `AstBuilder::parse_chunk()` constructs a typed `BlockStmt` tree.
2. `resolve_assignment_targets()` records assignment bindings.
3. `discover_static_bindings()` and `propagate_static_descriptors()` publish static value information.
4. `run_type_analysis()` performs optional semantic type analysis, followed by a second descriptor propagation pass.
5. `prepare_import_interfaces()` finalises imported-module interfaces and `collect_parser_symbols()` extracts tooling
   metadata.
6. `IrEmitter::emit_chunk()` lowers the AST to LuaJIT bytecode.

The builder may register enum constants and structures while parsing.  Keep its commit and rollback paths paired when
changing pipeline error handling.

## File Map

### Entry Points and Context

| File | Purpose |
|------|---------|
| `parser.cpp` / `parser.h` | `lj_parse()` entry point, pipeline orchestration and unity-build root |
| `parser_context.cpp` / `parser_context.h` | Parser configuration, diagnostics, token stream, import stack and shared static descriptors |
| `parser_diagnostics.cpp` / `parser_diagnostics.h` | Collected parser errors and warnings |
| `parser_profiler.h` | Parse, type-analysis and emission timing |
| `parser_symbols.cpp` / `parser_symbols.h` | Symbol and documentation metadata for tooling |
| `parser_tips.cpp` / `parser_tips.h` | Optional IDE tips, guarded by `INCLUDE_TIPS` |

### Lexer and Tokens

| File | Purpose |
|------|---------|
| `lexer.cpp` / `lexer.h` | Tokenisation and `LexState` |
| `lexer_types.h` | Token definitions, flags, source spans and lexer-side type declarations |
| `token_types.cpp` / `token_types.h` | Strongly typed `TokenKind` and `Token` adapters |
| `token_stream.cpp` / `token_stream.h` | Lookahead and token consumption for the AST builder |

### AST Construction

| File | Purpose |
|------|---------|
| `ast/nodes.cpp` / `ast/nodes.h` | AST payloads, ownership types and type-name conversion |
| `ast/builder.cpp` / `ast/builder.h` | `AstBuilder`, block parsing and registration transactions |
| `ast/expressions.cpp` | Primary, suffix, binary, unary and ternary expressions |
| `ast/literals.cpp` | Literals, tables and function expressions |
| `ast/statements.cpp` | Declarations, assignments, imports and other statements |
| `ast/loops.cpp` | Loop statements, `break` and `continue` |
| `ast/choose.cpp` | `choose` expressions |
| `ast/annotations.cpp` | Function annotation parsing |

### Semantic Analysis

| File | Purpose |
|------|---------|
| `assignment_target_resolution.*` | Assignment binding resolution before type analysis |
| `constant_evaluator.*` | Compile-time AST value evaluation |
| `field_type_lookup.*` | Static field and member type lookup |
| `static_type_descriptor.*` | Static value descriptors and catalogue storage |
| `static_descriptor_analysis.*` | Binding discovery and descriptor propagation |
| `table_ownership.*` | Ownership proof for contextual table designation |
| `type_checker.*` / `type_analysis.cpp` | Scope facts, inference and semantic type validation |
| `value_categories.*` | `ExprValue`, `RValue` and `LValue` handling |

### Imports

| File | Purpose |
|------|---------|
| `import_interface.*` | Translation of portable module interfaces into parser-owned descriptors |
| `import_interface_export.*` | Export of parser declarations to portable module interfaces |
| `import_module_validation.*` | Validation of imported-module state and cache inputs |

### IR Emission and Bytecode Support

| File | Purpose |
|------|---------|
| `ir_emitter/ir_emitter.*` | `IrEmitter` and general statement/expression lowering |
| `ir_emitter/operator_emitter.*` | Arithmetic, logical, comparison and bitwise operators |
| `ir_emitter/emit_*.cpp` | Specialised assignment, call, checkall, choose, function, global, import, table and try emission |
| `func_state.*` | `FuncState` helpers used during bytecode construction |
| `parse_regalloc.*` | Register allocation and RAII register handles |
| `parse_control_flow.*` | Jump chains and `ControlFlowEdge` patching |
| `parse_constants.cpp` | LuaJIT constant table and jump-list helpers |
| `parse_scope.cpp` | Runtime local, scope and upvalue handling during emission |
| `parse_expr.cpp` | Low-level `ExpDesc` and bytecode-expression helpers |

### Shared Headers and Tests

| File | Purpose |
|------|---------|
| `parse_types.h` | Strong index types, parser enums and expression descriptors |
| `parse_concepts.h` | C++20 concepts for parser and bytecode helper constraints |
| `parse_internal.h` | Internal bytecode and scope helper declarations |
| `parse_raii.h` | Scope and parser-state guards |
| `parse_value.h` | Compile-time parser value representation |
| `strong_index.h` | Strongly typed index wrapper |
| `parser_unit_tests.cpp` | Compiled parser unit tests, enabled by `UNIT_TESTS` |

## Main Types

### `ParserContext`

Owns the configuration, diagnostics and `TokenStreamAdapter`, and provides access to `LexState`, `FuncState` and
`lua_State`.  It also tracks nested imports and owns a shared `StaticDescriptorCatalogue`.  Use `ParserSession` for a
temporary configuration override.

### `AstBuilder`

Builds a `std::unique_ptr<BlockStmt>` through `parse_chunk()`.  It does not emit bytecode, but it does perform parser
registrations whose commit or rollback is controlled by the top-level pipeline.

### `IrEmitter`

Lowers a completed `BlockStmt` through `emit_chunk()`.  It owns the emission-time `RegisterAllocator` and
`ControlFlowGraph` and delegates operator lowering to `OperatorEmitter`.

### `RegisterAllocator`

Manages `FuncState::freereg`.  Prefer `acquire()` for one temporary register and `reserve_span()` for a strict LIFO
range.  `reserve()` and soft spans require explicit lifetime management.  Use the emitter's register-floor checks when
an emission path may disturb the enclosing expression floor.

### `TypeCheckScope`

Tracks parameters, locals, inferred types, const state and usage during semantic analysis.  The implementation-wide
analysis driver is `run_type_analysis()`; static descriptors are a separate, complementary data flow.

### `InstalledImportInterface`

Owns the parser-side translation of a finalised portable import interface.  Imported structures, callables and static
descriptors must remain tied to this object's lifetime.

## Implementation Rules

- Return recoverable parser failures with `ParserResult<T>`; the parser does not use C++ exceptions.
- AST children use `std::unique_ptr` and vectors of owning pointers.  Preserve ownership and assign a `SourceSpan` to
  every new node.
- `AstNodeKind` and `TiriType` are shared runtime enums in `src/tiri/jit/src/runtime/lj_obj.h`, not parser-local enums.
- Keep static descriptor, type-analysis and import-interface handling in sync when adding a construct that carries
  compile-time type or callable information.
- Use `ControlFlowEdge` for deferred jump patching and resolve or release every created edge before CFG finalisation.
- Follow the Kōtuku C++ rules from the repository guidance, including `and`, `or`, `IS`, three-space indentation and
  no C++ exceptions.

## Common Changes

- **Add an AST node:** Add the `AstNodeKind` entry in `runtime/lj_obj.h`, define its payload and variant membership in
  `ast/nodes.h`, construct it in the relevant `ast/*.cpp` file, update applicable semantic walkers, and add emission in
  `ir_emitter.cpp` or an `emit_*.cpp` file.
- **Add an operator:** Update the AST operator enum, lexer token metadata if new syntax is required, precedence/token
  mapping in `parser.cpp`, parsing in `ast/expressions.cpp`, semantic inference, and `operator_emitter.cpp`.
- **Change type behaviour:** Check `TiriType`, `FunctionReturnTypes`, `type_checker.*`, `type_analysis.cpp`, static
  descriptors and runtime contract consumers.  Source spellings are `any`, `nil`, `bool`, `num`, `str`, `table`,
  `array`, `func`, `struct`, `obj`, `range` and `userdata`.
- **Change imports:** Review AST parsing, module validation, portable interface export/installation, static descriptors,
  bytecode emission and cache tests as one path.
- **Add parser coverage:** Put low-level AST, semantic and emission tests in `parser_unit_tests.cpp`; put source-level
  behaviour tests under `src/tiri/tests/` and register them through the existing Tiri test list.

## References

- Bytecode reference: `src/tiri/jit/BYTECODE.md`
- Parser and register troubleshooting: `src/tiri/jit/src/TROUBLESHOOTING.md`
- Tiri integration tests: `src/tiri/tests/`
