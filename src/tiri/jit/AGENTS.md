# LuaJIT 2.1 Integration Notes

This file captures practices and gotchas observed while maintaining the LuaJIT 2.1 sources that ship inside Kōtuku. Use it as a quick orientation before diving into changes.

## Related Documentation

|Document|Purpose|
|-|-|
| [`src/parser/AGENTS.md`](src/parser/AGENTS.md) | Parser architecture, AST nodes, IR emission, register allocation, debugging strategies |
| [`src/lib/AGENTS.md`](src/lib/AGENTS.md) | LJLIB_* macros, buildvm code generation, fast function implementation, JIT recording |
| [`src/jit/AGENTS.md`](src/jit/AGENTS.md) | x64 VM assembly, register aliasing, Windows calling conventions |
| [`src/TROUBLESHOOTING.md`](src/TROUBLESHOOTING.md) | Register allocation troubleshooting, operator implementation patterns |
| [`BYTECODE.md`](BYTECODE.md) | Complete bytecode instruction reference with control-flow semantics |

## Repository Layout

```
src/
├── parser/              # C++20 refactored parser (two-phase: AST + IR emission)
│   ├── ast/             # AST node definitions and builder
│   └── ir_emitter/      # Bytecode emission from AST
├── lib/                 # Library implementations (LJLIB_* macros)
├── jit/                 # JIT Lua scripts and x64 VM assembly notes
├── debug/               # Error guard documentation
├── host/                # Build tools (buildvm, minilua)

build/agents/src/tiri/jitlib-generated/  # Generated headers, VM object, host helpers
build/agents/jit/lib/              # Final static library
```


## Integration & Build Tips
- Always rebuild via CMake (e.g. `cmake --build build/agents --config <BuildType>`) after touching LuaJIT or Tiri sources so the static library target is regenerated and relinked into the Tiri module.
- CMake drives three build strategies, matching the logic in `src/tiri/CMakeLists.txt`:
  - **MSVC**: `msvcbuild_codegen.bat` produces generated headers and `lj_vm.obj`, and CMake links `lua51.lib` next to the upstream sources.
  - **Unix-like toolchains**: CMake builds the host tools (`minilua` and `buildvm`), generates assembly with DynASM, then archives `lj_vm.o` + `ljamalg.o` into `libluajit-5.1.a`.
- Install (`cmake --install build/agents --config <BuildType>`) before running tests so the freshly built `origo` binary (or `origo.exe` on Windows) and scripts land in `build/agents-install/`.

## Error Handling Configuration
- **Windows (MSVC)**: Must NOT define `LUAJIT_NO_UNWIND`. MSVC always uses Structured Exception Handling (SEH) via `RaiseException()` and `lj_err_unwind_win()`.  There is no "internal unwinding" implementation for MSVC - SEH is the only viable mechanism. Setting `LJ_NO_UNWIND` for MSVC breaks exception handling and causes catch() tests to fail with "attempt to call a nil value" errors.
- The `LJ_NO_UNWIND` flag results in broken code that corrupts memory if used in GCC builds.

## Testing

### Running Tests
```bash
# Run all tests
ctest --build-config <BuildType> --test-dir build/agents

# Run specific test by label
ctest --build-config <BuildType> --test-dir build/agents -R <label>
```

### Manual Testing
```bash
# Quick checks with log output
origo --no-crash-handler --log-warning your_script.tiri
```

### After Modifying LuaJIT Sources

**Critical**: Rebuild and reinstall after touching LuaJIT C sources:
```bash
cmake --build build/agents --config <BuildType> --parallel
cmake --install build/agents --config <BuildType>
```

### JIT Debugging Options

Run `origo` with `--jit-options` to pass JIT engine flags as a CSV list:

|Option|Purpose|
|-|-|
|`off`|Disable the JIT compiler|
|`trace-tokens`|Trace tokenisation|
|`trace-expect`|Trace parser expectations|
|`trace-boundary`|Trace boundary crossings between interpreted and JIT code|
|`trace-operators`|Trace operator emission|
|`trace-registers`|Trace register allocation|
|`trace-cfg`|Trace control flow graph operations|
|`trace-assignments`|Trace assignment emission|
|`trace-value-category`|Trace value category analysis|
|`trace-types`|Trace type analysis|
|`trace`|Enable all trace messages|
|`dump-bytecode`|Dump disassembled bytecode at the end of parsing|
|`diagnose`|Disable abort-on-error for full script parsing|
|`profile`|Profile JIT parsing and runtime|
|`tips`|Enable parser tips|
|`top-tips`|Enable top-level tips only|
|`all-tips`|Enable all tips|

Example: `--jit-options dump-bytecode,trace-registers`

### Per-Script JIT Options
```lua
local script = obj.new('tiri', {
   statement = [[
      function test()
         return 42
      end
   ]],
   jitOptions = 'dump-bytecode,diagnose'
})
script.acActivate()
```

## Annotation System Internals

Function annotations are parsed in the C++ parser and can follow two different paths:

- AST annotation parsing: `src/parser/ast/annotations.cpp` and `src/parser/ast/nodes.h` parse `@Name(...)`
  syntax into `AnnotationEntry` values and attach them to `FunctionExprPayload::annotations`.
- Runtime registration: `src/parser/ir_emitter/emit_function.cpp` and `src/lib/lib_debug.cpp` emit
  `debug.anno.set(func, "...", source, name)` for annotated functions so runtime code can inspect annotations.
- Parser metadata extraction: `src/parser/parser_symbols.cpp`, `src/parser/parser_symbols.h` and
  `src/lib/lib_debug.cpp` build the `debug.validate(Source, "symbols").symbols` table for LSP and documentation
  tooling.

### Runtime Annotation Registration

`IrEmitter::emit_annotation_registration()` serialises parsed `AnnotationEntry` values back to annotation syntax and
emits bytecode that calls `debug.anno.set()`.  String annotation values are escaped by
`append_annotation_string_literal()` before being embedded in the generated annotation string; keep this escaping path
in mind when adding new annotation value types.

`@Doc` is special because documentation text can be large.  The emitter checks:

```cpp
L->script and ((L->script->Flags & SCF::PROCESS_DOC) != SCF::NIL)
```

If `SCF::PROCESS_DOC` is not set, a `@Doc` annotation is still registered as an annotation marker, but its arguments are
omitted from the runtime annotation string.  This prevents normal script execution from retaining large documentation
payloads in `debug.anno`.  Tooling paths that need the payload must enable `SCF::PROCESS_DOC`.

### Parser Symbol Extraction

`debug.validate(Source, "symbols")` is the public extraction path for parser-provided symbol and documentation metadata.
In `LJLIB_CF(debug_validate)`:

1. The `"symbols"` flag temporarily sets `L->script->Flags |= SCF::PROCESS_DOC`.
2. `lua_load()` parses the source in diagnose mode.
3. `collect_parser_symbols()` is called after AST construction from `parser.cpp`.
4. `push_symbol_metadata()` converts `L->parser_symbols` into a Tiri table named `symbols`.
5. The original script flags are restored and `L->parser_symbols` is deleted after the table has been pushed.

`collect_parser_symbols()` is deliberately gated on `SCF::PROCESS_DOC`; without the flag it deletes any stale
`Lua.parser_symbols` value and returns without walking the AST.

The collector walks function declarations and function expressions assigned to variables or members.  For every function
it builds a `ParserSymbolMetadata` record:

```cpp
struct ParserSymbolMetadata {
   std::string name;
   std::string kind;       // "function" or "thunk"
   std::string signature;  // e.g. "greet(Name: str):str"
   SourceSpan span;
   SourceSpan end_span;
   std::vector<ParserAnnotationMetadata> annotations;
   ParserDocBlockMetadata doc;
   std::vector<ParserDocParamMetadata> params;
   std::vector<ParserDocReturnMetadata> results;
   std::vector<ParserDocErrorMetadata> errors;
};
```

The table exposed to Tiri is zero-indexed and has this shape:

```lua
{
   symbols = {
      [0] = {
         name = "greet",
         kind = "function",
         signature = "greet(Name: str):str",
         line = 0,
         column = 0,
         endLine = 6,
         endColumn = 1,
         annotations = {
            [0] = { name = "Doc", args = { text = "..." } }
         },
         doc = {
            summary = "Returns a greeting.",
            body = "Returns a greeting.",
            raw = "...original annotation text...",
            examples = {}
         },
         params = {
            [0] = { name = "Name", type = "str", doc = "Person to greet", inferred = false }
         },
         results = {
            [0] = { type = "str", doc = "Greeting text", inferred = false }
         },
         errors = {
            [0] = { code = "Args", doc = "If the name is invalid", inferred = false }
         }
      }
   }
}
```

The parser exposes result values as `results`, not `returns`.  This matches the field name used by
`ParserSymbolMetadata` and by the Tiri tests.

### `@Doc(text=...)` Parsing

`parser_symbols.cpp` looks for an annotation named exactly `Doc` and an argument named exactly `text`.  The text
argument must be a string; other argument types are ignored for documentation parsing.

`parse_doc_text()` first calls `normalise_doc_lines()`:

- Split on `\n`.
- Drop a trailing `\r` from each line.
- Drop leading whitespace from every line.

This means indented documentation blocks are valid and marker recognition is independent of indentation:

```lua
@Doc(text=[[
   Returns a greeting.

   -INPUT-
   Name: Person to greet
]])
```

After normalisation, the parser chooses between long form and compact form.  If the first non-empty, trimmed line starts
with `@`, compact form is used; otherwise marker form is used.

### Marker Form

Marker form uses exact marker names:

|Marker|Target metadata|Line parser|
|-|-|-|
|Description text|`doc.summary`, `doc.body`|First non-empty line is `summary`; joined block is `body`.|
|`-INPUT-`|`params[].doc`|Parsed as `Name: Description`, then matched to signature parameters by name.|
|`-RESULTS-`|`results[].doc`|Each non-empty line is parsed as `type: Description` and matched to result positions.|
|`-ERRORS-`|`errors[]`|Each non-empty line is parsed as `Code: Description`.|
|`-EXAMPLE-`|`doc.examples[]`|All lines until the next marker or end of block are joined and appended as one example.|

`parse_name_doc_line()` is used for input, result and error lines.  If no colon is present, the whole line becomes the
name/type/code and the doc string is empty.

### Compact Form

Compact form is intended for terse one-line documentation.  Tags are exact and lower-case:

|Tag|Target metadata|Line parser|
|-|-|-|
|`@desc`|`doc.summary`, `doc.body`|First `@desc` sets `summary`; each non-empty payload is appended to `body`.|
|`@input`|`params[].doc`|Payload is parsed as `Name: Description`.|
|`@result`|`results[].doc`|Payload is parsed as `type: Description`.|
|`@error`|`errors[]`|Payload is parsed as `Code: Description`.|

Compact entries do not support continuation lines.  Unknown lines are ignored by the compact parser.

Example:

```lua
@Doc(text=[[
   @desc Returns a greeting.
   @input Name: Person to greet
   @result str: Greeting text
   @result str: Additional argument
   @error Args: If the name is invalid
   @error Failed: Random failure
]])
function greetCompact(Name: str):<str, str>
   return "Hello " .. Name, "extra"
end
```

### Signature Merging Rules

Documentation text augments parser-derived signature metadata; it does not replace it.

- Parameters always come from the function signature, excluding implicit `self`.
- Parameter documentation is matched by parameter name.  `params[].inferred` is `true` when no matching doc line was
  found.
- Result entries use the larger of the declared result count and documented result count.
- Declared result types take precedence over documented result types.  A documented result type is used only when no
  declared type exists at that position.
- `results[].inferred` is `true` only for documented result entries beyond the declared result count.
- Error entries currently come only from explicit documentation.  Error-code inference is not implemented in the
  collector yet, so `errors[].inferred` is currently `false`.

### Unit Tests
- Unit tests are managed by `MODTests()` in `src/tiri/tiri.cpp`
- Run compiled-in unit tests: `src/tiri/tests/test_unit_tests.tiri` with `--log-api`

## Common Gotchas

- **Naming collisions**: Check that new compile-time constants or flags don't collide with upstream naming; we will eventually rebase to newer LuaJIT drops.
- **Build artefacts**: Generated outputs under `build/agents/` can be removed safely; do not store investigation artefacts there long-term.
- **Subtle regressions**: Tiri tests often surface LuaJIT semantic changes as script regressions rather than crashes.

## VM Assembly and buildvm Dependencies

**Critical Build Dependency**: The `lj_obj.h` file contains the `MMDEF` macro which defines the metamethod table. When modifying `MMDEF` (e.g., adding new metamethods like `__close`), both `buildvm` and `lj_vm.obj` must be regenerated.

**Why This Matters:**
- `buildvm` generates `lj_vm.obj` with hardcoded metamethod offsets derived from `MMDEF`
- If `lj_obj.h` changes but `lj_vm.obj` is not regenerated, the VM assembly will have stale offsets
- This causes cryptic runtime failures like `PANIC: unprotected error in call to Lua API ()` affecting *all* Tiri scripts

### Adding New Metamethods

When adding entries to `MMDEF` in `lj_obj.h`:

1. **Position matters**: The `MMDEF` macro generates an enum (`MM_*`) with sequential values. The first 6-8 metamethods are "fast" (negative cached). Add new metamethods at the end, after `_(tostring)`, to avoid shifting existing indices.

2. **Force rebuild**: After modifying `MMDEF`, delete the generated VM files:
   ```bash
   # Windows
   del build\agents\src\tiri\jitlib-generated\buildvm.exe
   del build\agents\src\tiri\jitlib-generated\lj_vm.obj

   # Unix
   rm build/agents/src/tiri/jitlib-generated/buildvm
   rm build/agents/src/tiri/jitlib-generated/lj_vm.o
   ```

3. **Full rebuild**: Run `cmake --build build/agents --config <BuildType> --parallel`

**Note**: CMake includes `lj_obj.h` in the `DEPENDS` clause for both buildvm compilation and VM generation, ensuring automatic regeneration when `lj_obj.h` changes.

---

## Quick Reference

|Resource|Location|
|-|-|
|Parser source|`src/tiri/jit/src/parser/`|
|AST definitions|`src/tiri/jit/src/parser/ast/`|
|IR emission|`src/tiri/jit/src/parser/ir_emitter/`|
|Bytecode reference|`src/tiri/jit/BYTECODE.md`|
|JIT assembly notes|`src/tiri/jit/src/jit/AGENTS.md`|
|Tiri tests|`src/tiri/tests/`|
