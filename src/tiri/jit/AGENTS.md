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
