# LuaJIT Library System - Agent Guide

This document explains how the LuaJIT library system works in Kōtuku's Tiri scripting engine, including the LIB
macros, buildvm code generation, and the relationship between library functions, trace recording, and assembly
implementations.

## Overview

The LuaJIT library system uses a code generation pipeline where source annotations (`LJLIB_*` macros) are processed
by the `buildvm` tool at build time to generate dispatch tables, function enumerations, bytecode metadata and recording
hints for the JIT compiler.

```
┌─────────────────────────────────────────────────────────────┐
│ lib_*.cpp source files with LJLIB_* macro annotations       │
└────────────────────────────┬────────────────────────────────┘
                             │ buildvm (build-time tool)
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ Generated headers: lj_bcdef.h, lj_ffdef.h, lj_libdef.h,     │
│                    lj_recdef.h                              │
└────────────────────────────┬────────────────────────────────┘
                             │ compiled into
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ Runtime: VM dispatch, JIT recording, library registration   │
└─────────────────────────────────────────────────────────────┘
```

## LIB Macros Reference

The `LJLIB_*` macros are defined in `lib.h` and serve as annotations that `buildvm` parses.  Most expand to nothing in
the C++ compiler; `LJLIB_CF` and `LJLIB_ASM` also declare their implementation functions.

### Function Declaration Macros

| Macro | Purpose | Generated Function Name |
|-------|---------|------------------------|
| `LJLIB_CF(name)` | Pure C function implementation | `lj_cf_##name(lua_State *L)` |
| `LJLIB_ASM(name)` | Assembly fast-path with C fallback | `lj_ffh_##name(lua_State *L)` |
| `LJLIB_ASM_(name)` | Assembly-only (no C fallback) | None (asm only) |
| `LJLIB_LUA(name)` | Deprecated and rejected by `buildvm` | None |

Do not add `LJLIB_LUA`.  Convert embedded implementations to `LJLIB_CF`; the former precompiled-bytecode path is no
longer supported.

**Example:**
```cpp
// Pure C function - all logic in C
LJLIB_CF(rawset)
{
   lj_lib_checktab(L, 1);
   lj_lib_checkany(L, 2);
   // ... implementation
   return 1;
}

// Assembly with C fallback - common path in asm, edge cases in C
LJLIB_ASM(assert)
{
   // This C code only runs if the asm fast path fails
   if (tvisnil(L->base) or tvisfalse(L->base)) {
      // error handling
   }
   return FFH_RETRY;  // Tell VM to retry with corrected arguments
}
```

### Registration and Initialisation Macros

| Macro | Purpose |
|-------|---------|
| `LJLIB_PUSH(value)` | Push a value onto the stack during library init |
| `LJLIB_SET(name)` | Set the pushed value as a field in the library table |
| `LJLIB_NOREG` | Reserve the fast-function ID without creating or registering a closure |
| `LJLIB_NOREGUV` | Create an internal closure with upvalues but no public table entry |
| `LJLIB_INTRINSIC` | Register an internal compiler intrinsic without publishing a table entry |

Registration markers apply to the next generated-library function and are mutually exclusive.  Every generated
function receives a stable built-in callable identity.  Closures created for normal, `NOREGUV` and `INTRINSIC`
entries are rooted in the state-local built-in registry; `NOREG` only advances the ID.

**LJLIB_PUSH values:**
- String literals: `LJLIB_PUSH("string")`
- Numbers: `LJLIB_PUSH(3.14159)`
- Special values: `LJLIB_PUSH(lastcl)`, `LJLIB_PUSH(top-1)`, `LJLIB_PUSH(top-2)`

**Example:**
```cpp
LJLIB_PUSH("1.0") LJLIB_SET(_VERSION)   // Sets _VERSION = "1.0"
LJLIB_PUSH(lastcl)                       // Pushes the last created closure
```

### Recording Hints

| Macro | Purpose |
|-------|---------|
| `LJLIB_REC(handler data)` | Specifies JIT recording behaviour |

The `LJLIB_REC()` macro connects library functions to the JIT trace recorder. Format:
- `handler`: Name of the recording function (without `recff_` prefix)
- `data`: Auxiliary data passed to the recorder (often an IR opcode)
- `.`: Special marker meaning "use the function name unchanged"

**Examples:**
```cpp
LJLIB_REC(math_unary IRFPM_SQRT)    // Uses recff_math_unary with IRFPM_SQRT hint
LJLIB_REC(bit_shift IR_BSHL)        // Uses recff_bit_shift with IR_BSHL opcode
LJLIB_REC(.)                         // Uses recorder named after the function
LJLIB_REC(xpairs 1)                  // Uses recff_xpairs with data=1 for ipairs
```

### Module Declaration

```cpp
#define LJLIB_MODULE_math  // Declares this file implements the "math" library
```

## Buildvm Code Generation

The `buildvm` tool (source in `../host/buildvm_lib.cpp`) processes the ordered `LJLIB_C` source list in
`../../../CMakeLists.txt` and generates multiple headers.  The list order is significant because it defines fast
function IDs.

### Generation Commands

```bash
build/agents/src/tiri/jitlib-generated/buildvm -m ffdef -o <output>/lj_ffdef.h <ordered-LJLIB_C-inputs>
```

This illustrates the generated command only.  Use CMake (`cmake --build build/agents --config Debug --target tiri
--parallel`) rather than invoking `buildvm` manually: CMake supplies the complete ordered inputs and also runs the
`bcdef`, `libdef`, `recdef`, `folddef`, VM-definition and architecture-specific VM generation steps.

### Generated Files

**lj_ffdef.h** - Fast Function Definitions:
```cpp
// Generated enumeration of all fast functions
FFDEF(assert, "assert")
FFDEF(type, "type")
FFDEF(math_sqrt, "math.sqrt")
```

**lj_libdef.h** - Library Initialisation:
```cpp
// Function pointer arrays
static const lua_CFunction lj_lib_cf_base[] = {
   lj_ffh_assert,
   lj_ffh_type,
   // ...
};

// Binary-encoded initialisation data
static const uint8_t lj_lib_init_base[] = {
   2, 0, 22,                              // ffid, ffasmfunc, hash size
   70,97,115,115,101,114,116,195,        // "assert" + tag
   // ... encoded names and metadata
   255                                    // LIBINIT_END
};
```

**lj_recdef.h** - Recording Map:
```cpp
// Maps fast function IDs to recorder handlers
static const uint16_t recff_idmap[] = {
   0,                          // FF_C (no recorder)
   0x0100,                     // FF_assert → recff_assert
   // ... one entry per generated fast function
};
```

### Binary Encoding Format

Library initialisation data uses a compact binary format:

| Tag or marker | Meaning |
|---------------|---------|
| `0x00` | Pure C function (`LIBINIT_CF`) |
| `0x40` | ASM-backed function (`LIBINIT_ASM`) |
| `0x80` | ASM-only function (`LIBINIT_ASM_`) |
| `0xc0` | String constant (`LIBINIT_STRING`) |
| `0xf9` | Legacy Lua function (`LIBINIT_LUA`; no longer generated) |
| `0xfa` | Set table field (`LIBINIT_SET`) |
| `0xfb` | Numeric constant (`LIBINIT_NUMBER`) |
| `0xfc` | Copy from stack (`LIBINIT_COPY`) |
| `0xfd` | Use last closure (`LIBINIT_LASTCL`) |
| `0xfe` | Fast function ID (`LIBINIT_FFID`) |
| `0xff` | End marker (`LIBINIT_END`) |

## Two-Tier Function Implementation

`LJLIB_ASM` functions use a two-tier implementation strategy for optimal performance:

### Tier 1: Assembly Fast Path (vm_*.dasc)

The assembly implementations in `vm_x64.dasc` (or other architecture files) provide zero-overhead execution for common
cases:

```
┌─────────────────────────────────────────────────┐
│ VM Dispatch                                     │
│ 1. Check argument types in registers            │
│ 2. If types match: execute optimised path       │
│ 3. Return result directly                       │
└─────────────────────────────────────────────────┘
```

### Tier 2: C Fallback (lib_*.cpp)

When the assembly fast path fails (wrong types, edge cases), control falls back to C:

```cpp
LJLIB_ASM(math_sqrt) LJLIB_REC(math_unary IRFPM_SQRT)
{
   // Only reached if asm path failed (e.g., argument was a string)
   lj_lib_checknum(L, 1);   // Coerce string to number
   return FFH_RETRY;         // Retry the asm path with corrected argument
}
```

### FFH Return Codes

| Code | Meaning |
|------|---------|
| `FFH_RETRY` (0) | Retry fast path after argument correction |
| `FFH_RES(n)` | Success, returning n values |
| `FFH_TAILCALL` (-1) | Perform tail call to metamethod |
| `FFH_UNREACHABLE` | Alias for `FFH_RETRY`, used when the retried fast path is expected to complete |

### Execution Flow Example: math.sqrt("3.14")

```
1. VM calls asm fast path for math.sqrt
2. Asm checks: argument is string, not number → fails
3. Jumps to C fallback: lj_ffh_math_sqrt()
4. C code: lj_lib_checknum() coerces "3.14" → 3.14
5. C code: returns FFH_RETRY
6. VM retries asm path with numeric argument
7. Asm path succeeds, returns sqrt(3.14)
```

## JIT Recording System

The `LJLIB_REC()` annotations connect library functions to the JIT trace recorder in `lj_ffrecord.cpp`.

### Recording Architecture

```
┌────────────────────────────────────────────────────┐
│ LJLIB_REC(math_unary IRFPM_SQRT) in lib_math.cpp   │
└────────────────────────────┬───────────────────────┘
                             │ buildvm generates
                             ▼
┌────────────────────────────────────────────────────┐
│ recff_idmap[FF_math_sqrt] = (handler << 8)         │
│                                 + IRFPM_SQRT       │
└────────────────────────────┬───────────────────────┘
                             │ runtime lookup
                             ▼
┌────────────────────────────────────────────────────┐
│ recff_func[handler] = &recff_math_unary            │
│ Called with rd->data = IRFPM_SQRT                  │
└────────────────────────────┬───────────────────────┘
                             │ generates
                             ▼
┌────────────────────────────────────────────────────┐
│ IR_FPMATH node with IRFPM_SQRT parameter           │
│ Compiles to native SQRTSD instruction              │
└────────────────────────────────────────────────────┘
```

### RecordFFData Structure

```cpp
typedef struct RecordFFData {
   TValue* argv;                       // Runtime argument values
   ptrdiff_t result_count;             // Number of returned results (defaults to 1)
   uint32_t data;                      // Auxiliary data from LJLIB_REC()
   RecordFFDisposition disposition;    // Completed, stopped or pending call
} RecordFFData;
```

### Common Recorder Patterns

| Pattern | Example | Usage |
|---------|---------|-------|
| Unary math | `recff_math_abs` | Emits single-operand IR node |
| Binary operation | `recff_bit_nary` | Type checks + bitwise IR node selected by `rd->data` |
| Table access | `recff_rawget` | Uses RecordIndex helpers |
| Call emission | `recff_math_call` | Emits `lj_ir_call()` |
| Parameterised | `recff_xpairs` | Uses rd->data to distinguish ipairs/pairs |

## Relationship Between Components

### File Dependencies

```
lib_*.cpp (ordered sources from CMake's LJLIB_C list)
    │
    ├──→ buildvm ──→ lj_bcdef.h   (bytecode modes and fast-path offsets)
    │           ├──→ lj_ffdef.h   (FF_* enums and canonical names)
    │           ├──→ lj_libdef.h  (init data + function arrays)
    │           └──→ lj_recdef.h  (recording map)
    │
    ├──→ lib.cpp                  (library registration and built-in registry)
    │
    └──→ ../lj_ffrecord.cpp       (JIT recording)
             │
             └──→ ../jit/vm_*.dasc  (assembly implementations)
```

### Runtime Initialisation

1. `lj_lib_register()` reads binary init data from `lj_lib_init_*[]`
2. Decodes function names and metadata
3. Populates global library tables (`math`, `string`, etc.)
4. Registers each closure by built-in callable ID in the state-local built-in registry

### Call Dispatch

1. Lua code calls `math.sqrt(x)`
2. VM identifies target as fast function (ffid = FF_math_sqrt)
3. Dispatches to assembly fast path
4. On failure, falls back to C implementation
5. During JIT recording, `lj_ffrecord_func()` generates IR instead

## Adding a New Library Function

### Step 1: Declare the Function

```cpp
// In lib_mylib.cpp
#define LJLIB_MODULE_mylib

LJLIB_CF(mylib_myfunc)
{
   lua_Number arg = lj_lib_checknum(L, 1);
   lua_pushnumber(L, arg);
   return 1;
}

extern int luaopen_mylib(lua_State *L)
{
   LJ_LIB_REG(L, "mylib", mylib);
   return 1;
}
```

Add a new `lib_*.cpp` file to the explicit `LJLIB_C` list in `src/tiri/CMakeLists.txt`; the compilation source glob is
separate from the code-generation input list.  Register a new public library's `luaopen_*` function in `lib_init.cpp`.
Prefer `LJLIB_CF` unless the function has a real DynASM fast path.  When extending an existing library, add the
function to that library's source file and retain its existing `luaopen_*` registration.

### Step 2: Add Assembly Implementation (Optional)

Change the declaration to `LJLIB_ASM(mylib_myfunc)` and add a matching fast function to each supported architecture.
The C++ body then becomes the fallback used when the assembly path cannot handle the arguments.

```asm
// In vm_x64.dasc
|.ffunc mylib_myfunc
|  // Fast path implementation
|  // Fall through to C on failure
```

### Step 3: Add Recorder (If JIT Support Is Needed)

```cpp
// In lj_ffrecord.cpp
static void recff_mylib_myfunc(jit_State *J, RecordFFData *rd)
{
   // Validate the recorded arguments and emit the result into J->base[0].
   // Update rd->result_count or rd->disposition when the normal single-result path does not apply.
}
```

Add `LJLIB_REC(.)` to the function declaration only when the matching `recff_mylib_myfunc` handler exists.  A shared
recorder can instead be selected with `LJLIB_REC(handler data)`.

### Step 4: Preserve the Fast-Function ABI

Generated fast-function order is part of the private `BC_BFUNC` bytecode ABI.  Adding, removing or reordering an entry
requires a `BCDUMP_VERSION` bump in `../bytecode/lj_bcdump.h` and an update to the expected generated-order fingerprint
in `../debug/lj_ff.h`.  The fingerprint assertion deliberately fails the build when this maintenance is omitted.

### Step 5: Rebuild and Install

```bash
cmake --build build/agents --config Debug --target tiri --parallel
cmake --install build/agents --config Debug
```

In a static build, build `tiri origo_cmd` so that the executable is relinked.  Install before running integration or
Flute tests.

## Key Files Reference

| File | Purpose |
|------|---------|
| `lib.h` | LJLIB_* macro definitions |
| `lib_base.cpp` | Core Lua functions (type, assert, etc.) |
| `lib_math.cpp` | Math library |
| `lib_string.cpp` | String library |
| `lib_table.cpp` | Table library |
| `lib_bit.cpp` | Bitwise operations |
| `lib_array.cpp` | Typed array library |
| `lib_object.cpp` | Kōtuku object integration |
| `../host/buildvm_lib.cpp` | Buildvm library processor |
| `../lj_ffrecord.cpp` | JIT fast function recording |
| `lib.cpp` | Library registration and built-in callable registry |
| `../jit/vm_x64.dasc` | x64 assembly implementations |
| `../debug/lj_ff.h` | Generated fast-function IDs, names and ABI fingerprint |
| `../bytecode/lj_bcdump.h` | Bytecode dump version |
