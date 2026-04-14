# Bytecode Semantics Reference

## 1. Introduction and Scope
Kōtuku integrates a heavily modified LuaJIT 2.1 VM. This note captures the control-flow semantics of its bytecode so parser and emitter changes do not regress short-circuiting or extended-falsey behaviour. It answers questions such as "when does `BC_ISEQP` skip the next instruction?" and "how do `??` and `?` wire their jumps?" and is aimed at maintainers working on `IrEmitter`/`OperatorEmitter` or debugging logical and ternary operators.

## 2. Notation, Conventions, and Versioning
- Registers are shown as `R0`, `R1`, etc. Fields A/B/C/D follow LuaJIT encoding: `A` is usually a destination or base, `B`/`C` are sources, `D` is a constant or split field. `base` is the current stack frame start.
- Conditions are expressed as "condition true → skip next instruction; condition false → execute next instruction (normally a `JMP`)." "Next instruction" means the sequential `BCIns`; a taken `JMP` applies its offset from the following instruction.
- Version: LuaJIT 2.1 with extensive changes, assuming the `LJ_FR2` two-slot frame layout used by all supported platforms.
- **64-bit bytecode**: `BCIns` is now `uint64_t` (was `uint32_t`). Instructions occupy 8 bytes each. New extended formats (ABCP, ADP, AP) enable native 64-bit pointer storage for inline caching. See section 3.1 for format details.
- Keep this file aligned with changes in `src/tiri/jit/src/parser/*`, whenever bytecode emission patterns change.

## 3. Bytecode Overview
### 3.1 High-Level Structure
- Each instruction is a 64-bit `BCIns` packed with opcode and operand fields. Prototypes (`GCproto`) hold the instruction stream, constants, and line table.
- The opcode always occupies bits 0-7, ensuring dispatch logic remains consistent across all formats.
- PC increments by 8 bytes per instruction (was 4 bytes in the legacy 32-bit format).

**Standard Formats (lower 32 bits):**
- Format ABC: `[B:8][C:8][A:8][OP:8]` (bits 0-31)
- Format AD: `[D:16][A:8][OP:8]` (bits 0-31)

**Extended Formats (64-bit with upper half):**
```
Format ABCP (extended operand):
+----+----+----+----+----+----+----+----+
|        P (32-bit) | B  | C  | A  | OP |
+----+----+----+----+----+----+----+----+
bits 63          32  31              0

Format ADP (extended with D operand):
+----+----+----+----+----+----+----+----+
|        P (32-bit) |    D    | A  | OP |
+----+----+----+----+----+----+----+----+

Format AP (pointer with register):
+----+----+----+----+----+----+----+----+
|           PTR (48-bit)      | A  | OP |
+----+----+----+----+----+----+----+----+
bits 63                 16 15  8  7  0
```

**Design rationale:**
- Standard opcodes use the lower 32 bits; upper 32 bits are available for extended data (P field) or inline pointers.
- **AP format**: Stores a 48-bit pointer plus 8-bit A operand, enabling inline caching with a register reference.
- User-space pointers on x64 fit in 47 bits (low-half canonical range). Runtime asserts verify `ptr <= BC_PTR_USERSPACE_MAX` (0x00007FFFFFFFFFFF).
- Endian-independent: All field extraction uses bitmask/shift operations, not byte offsets.

### 3.2 Complete Bytecode Instruction Matrix

Operand suffixes: V=variable slot, S=string const, N=number const, P=primitive (~itype), B=byte literal, M=multiple args/results.

#### Comparison Ops (condition true → skip next; false → execute next)
| Opcode | Format | Description |
|--------|--------|-------------|
| `ISLT` | A D | R(A) < R(D) |
| `ISGE` | A D | R(A) >= R(D) |
| `ISLE` | A D | R(A) <= R(D) |
| `ISGT` | A D | R(A) > R(D) |
| `ISEQV` | A D | R(A) == R(D) |
| `ISNEV` | A D | R(A) != R(D) |
| `ISEQS` | A D | R(A) == str(D) |
| `ISNES` | A D | R(A) != str(D) |
| `ISEQN` | A D | R(A) == num(D) |
| `ISNEN` | A D | R(A) != num(D) |
| `ISEQP` | A D | R(A) == pri(D) (nil/false/true) |
| `ISNEP` | A D | R(A) != pri(D) |

#### Unary Test and Copy Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `ISTC` | A D | Copy R(D) to R(A) if truthy, else skip next |
| `ISFC` | A D | Copy R(D) to R(A) if falsey, else skip next |
| `IST` | D | Skip next if R(D) is truthy |
| `ISF` | D | Skip next if R(D) is falsey |
| `ISTYPE` | A D | Assert R(A) is type D (debug) |
| `ISNUM` | A D | Assert R(A) is number (debug) |
| `ISEMPTYARR` | A | R(A) is empty array → execute next JMP; else skip JMP |

#### Unary Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `MOV` | A D | R(A) = R(D) |
| `NOT` | A D | R(A) = not R(D) |
| `UNM` | A D | R(A) = -R(D) |
| `LEN` | A D | R(A) = #R(D) |

#### Binary Arithmetic Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `ADDVN` | A B C | R(A) = R(B) + num(C) |
| `SUBVN` | A B C | R(A) = R(B) - num(C) |
| `MULVN` | A B C | R(A) = R(B) * num(C) |
| `DIVVN` | A B C | R(A) = R(B) / num(C) |
| `MODVN` | A B C | R(A) = R(B) % num(C) |
| `ADDNV` | A B C | R(A) = num(C) + R(B) |
| `SUBNV` | A B C | R(A) = num(C) - R(B) |
| `MULNV` | A B C | R(A) = num(C) * R(B) |
| `DIVNV` | A B C | R(A) = num(C) / R(B) |
| `MODNV` | A B C | R(A) = num(C) % R(B) |
| `ADDVV` | A B C | R(A) = R(B) + R(C) |
| `SUBVV` | A B C | R(A) = R(B) - R(C) |
| `MULVV` | A B C | R(A) = R(B) * R(C) |
| `DIVVV` | A B C | R(A) = R(B) / R(C) |
| `MODVV` | A B C | R(A) = R(B) % R(C) |
| `POW` | A B C | R(A) = R(B) ^ R(C) |
| `CAT` | A B C | R(A) = R(B) .. ... .. R(C) (concatenate range) |

#### Constant Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `KSTR` | A D | R(A) = str(D) |
| `KCDATA` | A D | R(A) = cdata(D) (FFI) |
| `KSHORT` | A D | R(A) = signed 16-bit D |
| `KNUM` | A D | R(A) = num(D) |
| `KPRI` | A D | R(A) = pri(D) (nil/false/true) |
| `KNIL` | A D | R(A) ... R(D) = nil (range) |

#### Upvalue and Function Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `UGET` | A D | R(A) = upvalue(D) |
| `USETV` | A D | upvalue(A) = R(D) |
| `USETS` | A D | upvalue(A) = str(D) |
| `USETN` | A D | upvalue(A) = num(D) |
| `USETP` | A D | upvalue(A) = pri(D) |
| `UCLO` | A D | Close upvalues for R >= A; JMP to D |
| `FNEW` | A D | R(A) = new closure from proto(D) |

#### Table Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `TNEW` | A D | R(A) = new table (D encodes array/hash sizes) |
| `TDUP` | A D | R(A) = copy of template table(D) |
| `GGET` | A D | R(A) = _G[str(D)] |
| `GSET` | A D | _G[str(D)] = R(A) |
| `TGETV` | A B C | R(A) = R(B)[R(C)] |
| `TGETS` | A B C | R(A) = R(B)[str(C)] |
| `TGETB` | A B C | R(A) = R(B)[C] (byte index) |
| `TGETR` | A B C | R(A) = R(B)[R(C)] (raw, no metamethod) |
| `TSETV` | A B C | R(B)[R(C)] = R(A) |
| `TSETS` | A B C | R(B)[str(C)] = R(A) |
| `TSETB` | A B C | R(B)[C] = R(A) (byte index) |
| `TSETM` | A D | table[MULTRES...] = R(A)... (multi-set) |
| `TSETR` | A B C | R(B)[R(C)] = R(A) (raw, no metamethod) |

#### Array Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `AGETV` | A B C | R(A) = R(B)[R(C)] (native array get by variable) |
| `AGETB` | A B C | R(A) = R(B)[C] (native array get by byte literal) |
| `ASETV` | A B C | R(B)[R(C)] = R(A) (native array set by variable) |
| `ASETB` | A B C | R(B)[C] = R(A) (native array set by byte literal) |
| `ASGETV` | A B C | R(A) = R(B)?[R(C)] (safe array get - nil for out-of-bounds) |
| `ASGETB` | A B C | R(A) = R(B)?[C] (safe array get with literal - nil for out-of-bounds) |

#### Call and Vararg Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `CALLM` | A B C | R(A)...R(A+B-2) = R(A)(R(A+1)...R(A+C+MULTRES)) |
| `CALL` | A B C | R(A)...R(A+B-2) = R(A)(R(A+1)...R(A+C-1)) |
| `CALLMT` | A D | return R(A)(R(A+1)...R(A+D+MULTRES)) (tail) |
| `CALLT` | A D | return R(A)(R(A+1)...R(A+D-1)) (tail) |
| `ITERC` | A B C | Call iterator: R(A)...R(A+B-2) = R(A-3)(R(A-2), R(A-1)) |
| `ITERN` | A B C | Specialised next() iterator call |
| `VARG` | A B C | R(A)...R(A+B-2) = vararg (C=base offset) |
| `ISNEXT` | A D | Verify iterator is next(); JMP to D if not |

##### Array Iteration Fast Path (BC_ISARR, BC_ITERA)
- **BC_ISARR (A=D=base/jump)**: Guards array iteration setup. Checks that `R(A-2)` is a native array and `R(A-1)` is nil. On success, the opcode self-patches to `BC_ITERA` and falls through; on failure, it despecialises to `BC_JMP` and patches the following iterator to `BC_ITERC`.
- **BC_ITERA (A=base, B/C=lit)**: Specialised native array iterator (0-based). Control var is `R(A-1)`, index result is `R(A)`, value result is `R(A+1)`. Semantics:
  - If control var is nil, start at index 0; otherwise increment the control var by 1.
  - If `idx >= arr.len`: set `R(A)` to nil, proceed to `BC_ITERL` (loop exits).
  - Else: `R(A)` = index (integer tagged); `R(A-1)` updated to index; `R(A+1)` loaded via `lj_arr_getidx`.
  - JIT/hotcount entry exists as `lj_vm_IITERA`, mirroring `BC_ITERN`.

#### Return Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `RETM` | A D | return R(A)...R(A+D+MULTRES-1) |
| `RET` | A D | return R(A)...R(A+D-2) |
| `RET0` | A D | return (no values) |
| `RET1` | A D | return R(A) (single value) |

#### Type Fixing Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `TYPEFIX` | A D | Fix function return types at runtime (D = count of values) |

#### Loop and Branch Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `FORI` | A D | Numeric for init: R(A) = start, R(A+1) = limit, R(A+2) = step; JMP D |
| `JFORI` | A D | FORI with JIT trace linkage |
| `FORL` | A D | Numeric for loop: R(A) += R(A+2); if step>0 ? R(A)<=R(A+1) : R(A)>=R(A+1) then JMP D |
| `IFORL` | A D | FORL interpreter-only variant |
| `JFORL` | A D | FORL with JIT trace linkage |
| `ITERL` | A D | Iterator for loop: if R(A) != nil then R(A-1) = R(A); JMP D |
| `IITERL` | A D | ITERL interpreter-only variant |
| `JITERL` | A D | ITERL with JIT trace linkage |
| `LOOP` | A D | Loop hint (no-op in interpreter, JIT uses for tracing) |
| `ILOOP` | A D | LOOP interpreter-only variant |
| `JLOOP` | A D | LOOP with JIT trace linkage |
| `JMP` | A D | PC += D - 0x8000 (signed jump offset) |

#### Function Header Ops (internal, not emitted by parser)
| Opcode | Format | Description |
|--------|--------|-------------|
| `FUNCF` | A | Fixed-arg Lua function entry |
| `IFUNCF` | A | FUNCF interpreter-only variant |
| `JFUNCF` | A D | FUNCF with JIT trace linkage |
| `FUNCV` | A | Vararg Lua function entry |
| `IFUNCV` | A | FUNCV interpreter-only variant |
| `JFUNCV` | A D | FUNCV with JIT trace linkage |
| `FUNCC` | A | C function entry |
| `FUNCCW` | A | C function entry (with wrapper) |

#### Exception Handling Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `TRYENTER` | A D | Push exception frame (A=base, D=try_block_index) |
| `TRYLEAVE` | A D | Pop exception frame (A=base, D=0) |
| `CHECK` | A D | Check error code in R(A), raise if >= threshold |
| `RAISE` | A D | Raise exception with error code R(A) and message R(D) |

### 3.3 Resources and Cross-References
- Opcode definitions and metadata (including the `BCDEF` macro): `src/tiri/jit/src/bytecode/lj_bc.h`.
- Parser emission sites: `src/tiri/jit/src/parser/ir_emitter/operator_emitter.cpp` (operator lowering and bytecode emission), `src/tiri/jit/src/parser/ir_emitter/ir_emitter.cpp` (control-flow and expression emission).
- Exception handling emission: `src/tiri/jit/src/parser/ir_emitter/emit_try.cpp` (try-except-end statement bytecode generation).
- Behavioural context and patterns: `src/tiri/jit/src/parser/ir_emitter/operator_emitter.cpp`, `src/tiri/jit/src/parser/ir_emitter/ir_emitter.cpp`, and `src/tiri/jit/src/parser/parse_control_flow.cpp` (parser wiring and control-flow emission patterns).
- Native array implementation: `src/tiri/jit/src/runtime/lj_array.cpp`, `lj_array.h` (core array operations), `lj_vmarray.cpp` (bytecode handlers), `lib_array.cpp` (library functions).
- Array bytecode emission: `src/tiri/jit/src/parser/ir_emitter/emit_function.cpp`, `parse_regalloc.cpp` (array index expression discharge).
- VM implementations: `src/tiri/jit/src/jit/vm_x64.dasc` (x64), `vm_arm64.dasc` (ARM64), `vm_ppc.dasc` (PowerPC) - bytecode interpreter and JIT entry points.
- 64-bit bytecode implementation plan: `docs/plans/bcins-64bit.md` (detailed phase documentation).
- Tests exercising these paths live under `src/tiri/tests/`.

### 3.4 VM Assembly Considerations (64-bit Bytecode)

The VM assembly files (`vm_x64.dasc`, etc.) implement the bytecode interpreter and must correctly handle the 64-bit instruction format:

**Instruction Fetch and Dispatch:**
- `ins_NEXT` macro fetches a full 64-bit instruction and advances PC by 8 bytes.
- Opcode extraction: `movzx OP, RCL` (bits 0-7).
- A field extraction: `movzx RAd, RCH` (bits 8-15).
- D field extraction: `shr RC, 16` (bits 16-31, with upper bits preserved).

**PC-Relative Field Access:**
After PC advances past an instruction, field offsets from PC are:
```
PC_OP  = byte [PC-8]   ; Opcode (was PC-4)
PC_RA  = byte [PC-7]   ; A field (was PC-3)
PC_RC  = byte [PC-6]   ; C field (was PC-2)
PC_RB  = byte [PC-5]   ; B field (was PC-1)
PC_RD  = word [PC-6]   ; D field (was PC-2)
```

**Pointer Extraction:**
```asm
.macro ins_getp32, dst
  mov dst, dword [PC-4]  ; Upper 32 bits of instruction
.endmacro

.macro ins_getptr, dst
  mov dst, qword [PC-8]  ; Full 64-bit load
  shr dst, 16            ; Extract 48-bit pointer
.endmacro
```

**Forward Compatibility:**
- Use `shr RC, 16` (not `shr RCd, 16`) to preserve upper 32 bits for future opcodes that use the P field.
- The `ins_NEXT` macro already preserves upper bits in shifted form for P-field opcodes.

## 4. Conditional and Comparison Bytecodes
### 4.1 Conditional Bytecode Semantics
All comparison opcodes follow: **condition true → take the following JMP; condition false → fall through to next instruction**.

The comparison opcode and the following `JMP` form a single logical unit. When the condition is true, control branches to the JMP target; when false, execution continues sequentially.

### 4.2 Equality-with-Constant Opcodes (`BC_ISEQP`, `BC_ISEQN`, `BC_ISEQS`, `BC_ISNEP`, `BC_ISNEV`)
- `BC_ISEQP A, pri(D)`: compare register with primitive constant (nil/false/true). Equal → take JMP; not equal → fall through.
- `BC_ISEQN A, num(D)`: compare with numeric constant; same branch/fall-through behaviour.
- `BC_ISEQS A, str(D)`: compare with interned string constant; same branch/fall-through behaviour.
- `BC_ISNEP A, pri(D)` / `BC_ISNEV A, R(D)`: not-equal variants; not equal → take JMP; equal → fall through.
- Canonical branching: to branch on equality, place `JMP` immediately after the compare. Example (branch when value is nil):
  ```
  ISEQP   A, nil    ; nil → take JMP to target
  JMP     target    ; non-nil → fall through to next instruction
  ; non-nil path continues here
  ```
  For inverted sense (branch when not nil), use `ISNEP` with the same layout.

### 4.3 Generic Comparison Opcodes (`BC_ISLT`, `BC_ISGE`, `BC_ISLE`, `BC_ISGT`)
- These compare two registers (or a register and constant folded into D). Condition true takes the following JMP; condition false falls through.
- Pattern for `if a < b then ... else ... end`:
  ```
  ISLT    A, D      ; a < b → take JMP to else-branch
  JMP     else      ; a >= b → fall through to then-branch
  ; then-branch code here
  ```
- The parser uses the same idiom for numeric `for` bounds and relational operators in expressions.

### 4.4 Interaction with `BC_JMP` and Jump Lists
- Comparison + `JMP` encodes "if condition holds, branch to the jump target; otherwise fall through". A taken jump applies its signed offset from the instruction after the `JMP`.
- `jmp_patch` links jumps into singly linked lists so multiple comparisons can target a shared label. Patching a list fixes all pending offsets at once (e.g. chained falsey checks all landing on the RHS evaluation).
- Disassembly shows unpatched jumps as `----` offsets; when reading dumps from `--jit-options dump-bytecode`, identify compare/JMP pairs and the final patched destination to understand flow.

## 5. Native Array Operations

### 5.1 Overview

The native array type (`LJ_TARRAY`) provides a first-class VM type for fixed-size, homogeneous arrays. Unlike Lua tables, native arrays use dedicated bytecodes that bypass metamethod dispatch for element access, enabling better performance through direct memory access and JIT optimisation.

Native arrays support multiple element types: byte, int16, int32, int64, float, double, and pointer. All indexing uses 0-based conventions (Tiri standard).

### 5.2 Array Bytecode Semantics

Array bytecodes mirror the table access pattern but operate on `GCarray` objects:

| Bytecode | Operation | Bounds Check | Read-Only Check |
|----------|-----------|--------------|-----------------|
| `AGETV` | Load element by variable index | Yes (error) | No |
| `AGETB` | Load element by literal index | Yes (error) | No |
| `ASETV` | Store element by variable index | Yes (error) | Yes |
| `ASETB` | Store element by literal index | Yes (error) | Yes |
| `ASGETV` | Safe load by variable index | Yes (nil) | No |
| `ASGETB` | Safe load by literal index | Yes (nil) | No |

**Bounds checking**: Standard array access bytecodes (`AGETV`, `AGETB`, `ASETV`, `ASETB`) validate `0 <= index < array.len`. Out-of-bounds access raises `LJ_ERR_ARROB`.

**Safe bounds checking**: Safe array get bytecodes (`ASGETV`, `ASGETB`) return `nil` for out-of-bounds access instead of raising an error. These are used by the safe navigation operator (`?[]`) on arrays.

**Read-only checking**: Store operations (`ASETV`, `ASETB`) verify the `ARRAY_FLAG_READONLY` flag is not set. Writes to read-only arrays raise `LJ_ERR_ARRRO`.

### 5.3 Type Dispatch and Element Access

Array bytecodes must handle multiple element types. The interpreter dispatches based on `GCarray.elemtype`:

```
1. Type check: Verify operand is LJ_TARRAY
2. Index validation: Check 0 <= idx < len
3. Calculate element address: data + (idx * elemsize)
4. Load/store with type-appropriate width:
   - ARRAY_ELEM_BYTE:   8-bit load/store
   - ARRAY_ELEM_INT16:  16-bit load/store
   - ARRAY_ELEM_INT32:  32-bit load/store
   - ARRAY_ELEM_INT64:  64-bit load/store
   - ARRAY_ELEM_FLOAT:  32-bit float load/store
   - ARRAY_ELEM_DOUBLE: 64-bit double load/store
5. Box/unbox value for Lua stack
```

### 5.4 Fallback to Metamethods

When the base operand is not a native array, array bytecodes fall back to the standard table metamethod handlers (`vmeta_tgetv`, `vmeta_tsetv`). This enables:

- Graceful handling of userdata with `__index`/`__newindex`
- Mixed code that may operate on tables or arrays
- Forward compatibility with future array-like types

### 5.5 JIT Recording

Array bytecodes record to the JIT trace using call-based IR emission:

- `AGETV`/`AGETB` → `IRCALL_lj_arr_getidx`
- `ASETV`/`ASETB` → `IRCALL_lj_arr_setidx`
- `ASGETV`/`ASGETB` → `IRCALL_lj_arr_safe_getidx`

The recorder emits:
1. Type guard ensuring operand is array
2. Index narrowing (variable indices converted to integers)
3. Call to helper function with bounds checking

Future optimisation (Phase 5b) may inline element access for common types to eliminate function call overhead.

### 5.6 Parser Integration

When the parser can determine at compile time that an expression is an array (via type annotations or `array.new()` tracking), it emits array bytecodes directly:

- Known array + variable index → `BC_AGETV` / `BC_ASETV`
- Known array + literal index (0-255) → `BC_AGETB` / `BC_ASETB`
- Safe navigation (`?[]`) with variable index → `BC_ASGETV`
- Safe navigation (`?[]`) with literal index (0-255) → `BC_ASGETB`

When the base type is unknown, standard table bytecodes (`TGETV`, `TSETV`, etc.) are emitted, relying on runtime type dispatch.

### 5.7 Example: Array Access Pattern

```lua
local arr = array.new(100, "integer")
arr[0] = 42        -- BC_ASETB if type known, else BC_TSETB
local x = arr[i]   -- BC_AGETV if type known, else BC_TGETV
```

Disassembly for known array type:
```
ASETB   arr, 0, value    ; Store 42 at index 0
AGETV   dest, arr, i     ; Load element at variable index i
```

## 6. Control-Flow and Short-Circuit Patterns
### 6.1 Logical Operators (`and`, `or`)
- `a and b`: evaluate `a`; emit compare + `JMP` that branches past `b` when `a` is falsey. Truthy `a` falls through, so `b` is evaluated and its result replaces/occupies the same slot.
- `a or b`: evaluate `a`; emit compare + `JMP` that branches into `b` only when `a` is falsey. Truthy `a` falls through and becomes the result; `b` is untouched.
- Registers are normalised so the resulting value lives in the LHS register; `freereg` collapses after RHS evaluation to avoid leaks.

### 6.2 Ternary Operator (`cond ? true_val :> false_val`)
- Only one branch executes. The condition is evaluated in place; compare + `JMP` sends control to the false branch when the condition is extended-falsey.
- `IrEmitter::emit_ternary_expr` places both branches so the result lands in the condition register. Each branch frees temporaries before convergence, and `freereg` is patched back to the condition's base to guarantee a single-slot result.
- Example sketch:
  ```
  <eval cond in RA>
  ISEQP  RA, nil ; nil → branch to false
  JMP    false   ; non-nil → fall through to true branch
  ; true branch emits true_val into RA
  JMP    end
false:
  ; false branch emits false_val into RA
end:
  ```

### 6.3 Presence Operator (`x?`) – Extended Falsey Check
- Falsey set: `nil`, `false`, numeric zero, empty string, empty array. If operand is in this set, result is `false` and RHS (if any) is skipped.
- Emission: chain `BC_ISEQP`/`BC_ISEQN`/`BC_ISEQS`/`BC_ISEMPTYARR` comparisons, each followed by a `JMP` to the falsey path. A matching equality branches to that path; non-matching comparisons fall through to the next check. If all checks fall through (truthy value), execution continues past the chain.
- Jump lists patch the falsey exit after the chain; truthy fallthrough sets the result and collapses `freereg`.
- `BC_ISEMPTYARR` is used to check if the value is a native array with length zero. Semantics: if `R(A)` is an array with `len == 0`, execute the following `JMP` (falsey); otherwise skip the `JMP` (truthy - either not an array or non-empty array).

### 6.4 If-Empty Operator (`lhs ?? rhs`) – Short-Circuiting with Extended Falsey Semantics
- Evaluate `lhs`; if it is nil/false/0/""/empty array, evaluate `rhs` and return it; otherwise return `lhs` without touching `rhs`.
- Implemented in `IrEmitter::emit_if_empty_expr` plus helper routines in `operator_emitter.cpp`. The compare chain mirrors the presence operator: a matching falsey value branches to RHS evaluation; a truthy value falls through all checks and skips RHS entirely.
- The result register is the original `lhs` slot; RHS evaluation reuses it and collapses `freereg` afterward to avoid leaked arguments or vararg tails.
- Uses `BC_ISEMPTYARR` to check for empty arrays as part of the extended falsey semantics. If `R(A)` is a native array with `len == 0`, the following `JMP` is executed (falsey path); otherwise the `JMP` is skipped (truthy path).

## 7. Register Semantics and Multi-Value Behaviour
### 7.1 Register Lifetimes, `freereg`, and `nactvar`
- `nactvar` tracks active local slots; `freereg` is the first free slot above them. Temporaries are allocated above `nactvar` and must be reclaimed when no longer needed.
- Helpers such as `expr_free`, `RegisterGuard`, and `ir_collapse_freereg` ensure temporaries are released so following expressions do not see spurious stack entries.
- Failing to collapse `freereg` manifests as leaked arguments in calls or extra return values in vararg contexts.

### 7.2 `ExpKind::Call` and Multi-Return Semantics
- A freshly emitted `BC_CALL` yields an `ExpKind::Call` tied to its base register and may return multiple values (`BC_CALLM`) if left uncapped.
- Binary operators and control-flow constructs force single-value semantics: they convert call expressions to registers (`expr_toanyreg`), then free the call expression to cap results at one slot.
- Multi-return propagation is allowed only when explicitly constructing vararg lists; otherwise collapse to a single register before further comparisons or jumps.

### 7.3 Preventing Register Leaks in Chained Operations
- Reuse the LHS register when chaining operators at the same precedence; allocate new temporaries only when operands cannot share.
- After emitting a RHS, immediately free or collapse temporaries so the stack height matches `freereg` expectations before emitting the next operator.
- Always call `expr_free` before reserving new registers when an operand might still own a multi-return slot.

## 8. Common Emission Patterns and Anti-Patterns
### 8.1 Canonical Patterns (Do This)
- Compare + `JMP` for "branch on not-equal": `ISEQP A,const; JMP target` with true skipping the jump; see the relevant compare/jump emission logic in `operator_emitter.cpp`.
- Presence / if-empty chains: sequential `ISEQP/ISEQN/ISEQS/ISEMPTYARR` with shared jump list patched to the truthy or RHS path; see `operator_emitter.emit_presence_check` (called from `IrEmitter::emit_presence_expr`) and `IrEmitter::emit_if_empty_expr`.
- Logical short-circuit: `a or b` uses compare + `JMP` into RHS only when falsey; `a and b` jumps over RHS when falsey. Implemented via general binary operator emission in `operator_emitter.cpp` and `ir_emitter.cpp`.
- Ternary layout: condition in place, compare chain, then true/false blocks writing back into the same register, with end jump to merge; see `IrEmitter::emit_ternary_expr`.

### 8.2 Typical Mistakes (Do NOT Do This)
- Wiring compare + `JMP` backwards (treating "skip on equal" as "jump on equal"), which executes the wrong branch.
- Failing to collapse `freereg` after evaluating RHS, causing leaked stack slots that appear as extra arguments or returns.
- Allocating a new register for an operand without freeing the previous expression, allowing multi-return values to flow into subsequent operators.
- Regression tests that catch these issues: `src/tiri/tests/test_if_empty.tiri`, `test_presence.tiri`, logical operator suites, and ternary-focused cases; add new ones when patterns change.

## 9. Exception Handling and Type Fixing

### 9.1 Exception Handling Bytecodes (`BC_TRYENTER`, `BC_TRYLEAVE`, `BC_CHECK`, `BC_RAISE`)

Tiri's `try...except...end` statements are implemented using inline bytecode (not closures), allowing `return`, `break`, and `continue` to work correctly within try blocks.

**Bytecode structure:**
```
BC_TRYENTER  base, try_block_index    ; Push exception frame
<try body bytecode>                   ; Inline try body
BC_TRYLEAVE  base, 0                  ; Pop exception frame (normal exit)
JMP          exit_label               ; Jump over handlers
handler_1:                            ; Handler entry point
<handler1 bytecode>                   ; Inline handler body
JMP          exit_label
handler_2:
<handler2 bytecode>
JMP          exit_label
exit_label:
```

**Opcode semantics:**
- `BC_TRYENTER A, D`: Pushes an exception frame onto the exception handler stack. `A` is the base register, `D` is the try block index referencing metadata in `GCproto.try_blocks[]`.
- `BC_TRYLEAVE A, D`: Pops the exception frame (normal exit path). `A` is the base register, `D` is always 0.
- `BC_CHECK A, D`: Checks if the error code in `R(A)` is >= the error threshold. If so, raises an exception. Used for error code checking without explicit `raise` statements.
- `BC_RAISE A, D`: Raises an exception with error code in `R(A)` and optional message in `R(D)`. If `D` is 0xFF, no message is provided.

**Handler metadata:**
Handler metadata is stored in `GCproto.try_blocks[]` and `GCproto.try_handlers[]`. Each `TryBlockDesc` contains:
- `first_handler`: Index of the first handler in `try_handlers[]`
- `handler_count`: Number of handlers for this try block
- `entry_slots`: Register count at try block entry
- `flags`: `TRY_FLAG_TRACE` for debugging

Each `TryHandlerDesc` contains:
- `packed_filter`: Up to 4 16-bit error codes packed into 64 bits (0 = catch-all)
- `handler_pc`: Bytecode position of handler entry point
- `exception_reg`: Register holding exception table (0xFF = no variable)

**Implementation:** See [emit_try.cpp:30-240](src/tiri/jit/src/parser/ir_emitter/emit_try.cpp#L30-L240), [emit_try.cpp:248-291](src/tiri/jit/src/parser/ir_emitter/emit_try.cpp#L248-L291), [emit_try.cpp:299-320](src/tiri/jit/src/parser/ir_emitter/emit_try.cpp#L299-L320).

### 9.2 Runtime Type Fixing (`BC_TYPEFIX`)

`BC_TYPEFIX` enables runtime type inference for function return types when the function has no explicit return type annotations.

**Opcode semantics:**
- `BC_TYPEFIX A, D`: Fixes return types at runtime. `A` is the base register, `D` is the count of return values to process.
- Fast path: checks if `PROTO_TYPEFIX` flag is set in the function prototype. If not set, this is a no-op.
- Slow path: calls `lj_meta_typefix(L, base, count)` to infer and record types based on actual runtime values.

**When emitted:**
The parser sets the `PROTO_TYPEFIX` flag (defined in [lj_obj.h:623](src/tiri/jit/src/runtime/lj_obj.h#L623)) on function prototypes when:
1. The function has NO explicit return type annotations, AND
2. At least one return statement exists

**Purpose:**
Enables type inference for untyped functions, allowing the VM to optimize subsequent calls based on observed return types. The inferred types are stored in `GCproto.result_types[]` (up to `PROTO_MAX_RETURN_TYPES` positions).

**Implementation:** See [vm_x64.dasc:4901-4922](src/tiri/jit/src/jit/vm_x64.dasc#L4901-L4922), [lj_meta.cpp:664-685](src/tiri/jit/src/runtime/lj_meta.cpp#L664-L685), [parse_scope.cpp:1012-1024](src/tiri/jit/src/parser/parse_scope.cpp#L1012-L1024).

## 10. Testing, Debugging, and Tooling
### 10.1 Using Flute and Tiri Tests
- Run the Tiri regression tests under `src/tiri/tests/` (e.g. `test_if_empty.tiri`, `test_presence.tiri`, logical/ternary suites) to validate control-flow changes.
- When adding coverage, use side effects (counters, print hooks) on RHS expressions to prove short-circuiting, and capture varargs with `{...}` to detect leaked registers.

### 10.2 Disassembly and Bytecode Inspection
- Obtain bytecode via `mtDebugLog('disasm')` on a `tiri` object or run scripts with `--jit-options dump-bytecode,diagnose`.
- Map disassembly back to source by matching instruction order to expression evaluation order, then locate emission sites in `ir_emitter.cpp` or `operator_emitter.cpp`.
- Treat disassembly as the source of truth for branch direction when debugging control flow.

## 11. Maintenance Guidelines
- When touching conditional emission or short-circuit logic, update the opcode matrix and the relevant sections here.
- Add or adjust regression tests in `src/tiri/tests/` to cover new control-flow behaviours; rerun tests after installing a fresh build.
- Re-generate disassembly for representative snippets (logical ops, ternary, `??`, `?`) to verify register collapse and branch wiring.
- Reviewers should confirm emitted patterns match the documented skip/execute semantics and that test coverage exercises both true and false paths.

## 12. Glossary and Quick Reference

### Bytecode Types and Structures
- `BCIns`: 64-bit packed bytecode instruction (was 32-bit in legacy LuaJIT).
- `BCOp`: Opcode enum, always in bits 0-7 of the instruction.
- `BCPOS`: Bytecode position index within a prototype's instruction array.
- `BCREG`: Register number (8-bit, field A/B/C).

### Field Extraction Macros (defined in `lj_bc.h`)
- `bc_op(i)`: Extract opcode (bits 0-7).
- `bc_a(i)`: Extract A field (bits 8-15).
- `bc_b(i)`: Extract B field (bits 24-31 of lower word).
- `bc_c(i)`: Extract C field (bits 16-23 of lower word).
- `bc_d(i)`: Extract D field (bits 16-31 of lower word, unsigned).
- `bc_j(i)`: Extract D field as signed jump offset (`bc_d(i) - BCBIAS_J`).
- `bc_p32(i)`: Extract P field (upper 32 bits, for ABCP/ADP formats).
- `bc_ptr(i)`: Extract 48-bit pointer (bits 16-63, for AP format).

### Field Setter Macros
- `setbc_op(p, x)`: Set opcode in instruction at `p`.
- `setbc_a(p, x)`: Set A field.
- `setbc_b(p, x)`: Set B field.
- `setbc_c(p, x)`: Set C field.
- `setbc_d(p, x)`: Set D field.
- `setbc_p32(p, v)`: Set upper 32-bit P field.
- `setbc_ptr(p, ptr)`: Set 48-bit pointer in AP format.

### Instruction Construction Macros
- `BCINS_ABC(o, a, b, c)`: Build ABC-format instruction.
- `BCINS_AD(o, a, d)`: Build AD-format instruction.
- `BCINS_AJ(o, a, j)`: Build AD-format with jump bias.
- `BCINS_ABCP(o, a, b, c, p32)`: Build ABCP-format with 32-bit P field.
- `BCINS_ADP(o, a, d, p32)`: Build ADP-format with 32-bit P field.
- `BCINS_AP(o, a, ptr)`: Build AP-format with 48-bit pointer.

### Pointer Storage Constants
- `BC_PTR_USERSPACE_MAX`: Maximum valid user-space pointer (0x00007FFFFFFFFFFF on x64).

### Parser and Register Allocation
- `freereg`: first free stack slot above active locals; must be collapsed after temporaries.
- `nactvar`: count of active local variables.
- `ExpKind`: parser expression classification (`VNONRELOC`, `VRELOCABLE`, `VCALL`, etc.).

### Control Flow
- Extended falsey: `nil`, `false`, numeric zero, empty string, empty array.
- Short-circuit: using compare+`JMP` so RHS executes only when needed.
- Cheat sheet: `ISEQ*`/`ISNE*`/`IS*` comparisons — true skips next, false executes next; `and` skips RHS on falsey, `or` skips RHS on truthy; `x?` and `lhs ?? rhs` use chained equality tests with shared jump lists; ternary writes result back into the condition register with one branch skipped.

### Native Arrays
- `GCarray`: native array object with typed element storage.
- `LJ_TARRAY`: type tag for native arrays (value ~13).
- `ARRAY_FLAG_READONLY`: flag indicating array elements cannot be modified.
- `ARRAY_FLAG_EXTERNAL`: flag indicating array data is not owned by the GC.
- `AGETV`/`AGETB`/`ASETV`/`ASETB` provide direct array element access with bounds checking.
- `ASGETV`/`ASGETB` provide safe array access (returns nil for out-of-bounds) for the `?[]` operator.

### Exception Handling
- `PROTO_TYPEFIX`: flag indicating runtime type inference is enabled for function return types.
- `TryBlockDesc`: metadata for try blocks stored in `GCproto.try_blocks[]`.
- `TryHandlerDesc`: metadata for exception handlers stored in `GCproto.try_handlers[]`.
