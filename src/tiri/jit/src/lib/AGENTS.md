# LuaJIT Library System - Agent Guide

This document explains how the LuaJIT library system works in Kōtuku's Tiri scripting engine, including the LIB
macros, buildvm code generation, and the relationship between library functions, trace recording, and assembly
implementations.

## Overview

The LuaJIT library system uses a sophisticated code generation pipeline where source annotations (LJLIB_* macros) are
processed by the `buildvm` tool at build time to generate dispatch tables, function enumerations, and recording hints
for the JIT compiler.

```
┌─────────────────────────────────────────────────────────────┐
│ lib_*.cpp source files with LJLIB_* macro annotations       │
└────────────────────────────┬────────────────────────────────┘
                             │ buildvm (build-time tool)
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ Generated headers: lj_ffdef.h, lj_libdef.h, lj_recdef.h     │
└────────────────────────────┬────────────────────────────────┘
                             │ compiled into
                             ▼
┌─────────────────────────────────────────────────────────────┐
│ Runtime: VM dispatch, JIT recording, library registration   │
└─────────────────────────────────────────────────────────────┘
```

## LIB Macros Reference

The LJLIB_* macros are defined in `lib.h` and serve as annotations that buildvm parses. They do not generate code
directly - instead, buildvm extracts the information and produces header files.

### Function Declaration Macros

| Macro | Purpose | Generated Function Name |
|-------|---------|------------------------|
| `LJLIB_CF(name)` | Pure C function implementation | `lj_cf_##name(lua_State *L)` |
| `LJLIB_ASM(name)` | Assembly fast-path with C fallback | `lj_ffh_##name(lua_State *L)` |
| `LJLIB_ASM_(name)` | Assembly-only (no C fallback) | None (asm only) |
| `LJLIB_LUA(name)` | Lua bytecode implementation | Embedded bytecode |

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
| `LJLIB_NOREG` | Do not register the function in the library table |
| `LJLIB_NOREGUV` | Do not register but support upvalues |

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

The `buildvm` tool (source in `../host/buildvm_lib.cpp`) processes library source files and generates multiple headers.

### Build Commands

```bash
buildvm -m ffdef  -o lj_ffdef.h   lib_*.cpp   # Fast function enumeration
buildvm -m libdef -o lj_libdef.h  lib_*.cpp   # Library initialisation data
buildvm -m recdef -o lj_recdef.h  lib_*.cpp   # JIT recording dispatch map
buildvm -m bcdef  -o lj_bcdef.h   lib_*.cpp   # Bytecode definitions
```

### Generated Files

**lj_ffdef.h** - Fast Function Definitions:
```cpp
// Generated enumeration of all fast functions
FFDEF(assert)       // FF_assert = 1
FFDEF(type)         // FF_type = 2
FFDEF(math_sqrt)    // FF_math_sqrt = 40
// ... ~147 total functions
```

**lj_libdef.h** - Library Initialisation:
```cpp
// Function pointer arrays
static const lua_CFunction lj_lib_cf_base[] = {
   lj_cf_assert,
   lj_ffh_type,
   // ...
};

// Binary-encoded initialisation data
static const uint8_t lj_lib_init_base[] = {
   2, 0, 26,                              // ffid, ffasmfunc, hash size
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
   0x0100,                     // FF_assert → recff_func[1]
   // ...
   0x1400+(IRFPM_SQRT),        // FF_math_sqrt → recff_math_unary
};
```

### Binary Encoding Format

Library initialisation data uses a compact binary format:

| Tag (upper 2 bits) | Meaning |
|-------------------|---------|
| `0x00` | Pure C function (`LIBINIT_CF`) |
| `0x40` | ASM-backed function (`LIBINIT_ASM`) |
| `0x80` | ASM-only function (`LIBINIT_ASM_`) |
| `0xc0` | String constant (`LIBINIT_STRING`) |
| `0xf9` | Lua function (`LIBINIT_LUA`) |
| `0xfa` | Set table field (`LIBINIT_SET`) |
| `0xfb` | Numeric constant (`LIBINIT_NUMBER`) |
| `0xfc` | Copy from stack (`LIBINIT_COPY`) |
| `0xfd` | Use last closure (`LIBINIT_LASTCL`) |
| `0xfe` | Fast function ID (`LIBINIT_FFID`) |
| `0xff` | End marker (`LIBINIT_END`) |

## Two-Tier Function Implementation

Fast functions use a two-tier implementation strategy for optimal performance:

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
| `FFH_UNREACHABLE` | Should never be reached |

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
│ recff_idmap[FF_math_sqrt] = 0x1400 + IRFPM_SQRT    │
└────────────────────────────┬───────────────────────┘
                             │ runtime lookup
                             ▼
┌────────────────────────────────────────────────────┐
│ recff_func[0x14] = &recff_math_unary               │
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
   TValue* argv;        // Argument values from stack
   ptrdiff_t nres;      // Number of results
   uint32_t data;       // Auxiliary data from LJLIB_REC()
} RecordFFData;
```

### Common Recorder Patterns

| Pattern | Example | Usage |
|---------|---------|-------|
| Unary math | `recff_math_abs` | Emits single-operand IR node |
| Binary operation | `recff_bit_band` | Type checks + IR_BAND node |
| Table access | `recff_rawget` | Uses RecordIndex helpers |
| Call emission | `recff_math_call` | Emits `lj_ir_call()` |
| Parameterised | `recff_xpairs` | Uses rd->data to distinguish ipairs/pairs |

## Relationship Between Components

### File Dependencies

```
lib_*.cpp (source)
    │
    ├──→ buildvm ──→ lj_ffdef.h   (FF_* enums)
    │           ├──→ lj_libdef.h  (init data + function arrays)
    │           └──→ lj_recdef.h  (recording map)
    │
    ├──→ lj_lib.cpp               (library registration)
    │
    └──→ lj_ffrecord.cpp          (JIT recording)
             │
             └──→ vm_*.dasc       (assembly implementations)
```

### Runtime Initialisation

1. `lj_lib_register()` reads binary init data from `lj_lib_init_*[]`
2. Decodes function names and metadata
3. Populates global library tables (`math`, `string`, etc.)
4. Associates fast function IDs with function closures

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

LJLIB_ASM(mylib_myfunc) LJLIB_REC(.)   // Use default recorder
{
   // C fallback implementation
   double arg = lj_lib_checknum(L, 1);
   // ... handle edge cases
   return FFH_RETRY;
}
```

### Step 2: Add Assembly Implementation (Optional)

```asm
// In vm_x64.dasc
|.ffunc mylib_myfunc
|  // Fast path implementation
|  // Fall through to C on failure
```

### Step 3: Add Recorder (If JIT Support Needed)

```cpp
// In lj_ffrecord.cpp
static void recff_mylib_myfunc(jit_State *J, RecordFFData *rd)
{
   TRef tr = recff_argc_check(J, rd, 1);
   // Generate IR for the operation
   J->base[0] = emitir(/* IR nodes */);
}
```

### Step 4: Rebuild

```bash
cmake --build build/agents --target tiri --parallel
```

Buildvm automatically processes the new function and updates all generated headers.

## Key Files Reference

| File | Purpose |
|------|---------|
| `lib.h` | LJLIB_* macro definitions |
| `lib_base.cpp` | Core Lua functions (type, assert, etc.) |
| `lib_math.cpp` | Math library |
| `lib_string.cpp` | String library |
| `lib_table.cpp` | Table library |
| `lib_bit.cpp` | Bitwise operations |
| `lib_ffi.cpp` | FFI library |
| `../host/buildvm_lib.cpp` | Buildvm library processor |
| `../lj_ffrecord.cpp` | JIT fast function recording |
| `../lj_lib.cpp` | Library registration |
| `../vm_x64.dasc` | x64 assembly implementations |
