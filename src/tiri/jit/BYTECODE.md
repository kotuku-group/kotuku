# Bytecode Semantics Reference

## 1. Introduction and Scope

Kōtuku integrates a heavily modified LuaJIT 2.1 VM.  This note captures the control-flow and specialised runtime
semantics of its bytecode so parser, emitter and VM changes remain aligned.  It answers questions such as "when does
`BC_ISEQP` skip the next instruction?", "how do `??` and `?` wire their jumps?" and "how does `BC_BFUNC` load a
canonical built-in callable?"  It is aimed at maintainers working on the parser, interpreter, JIT or bytecode tooling.

## 2. Notation, Conventions, and Versioning

- Registers are shown as `R0`, `R1`, etc. Fields A/B/C/D follow LuaJIT encoding: `A` is usually a destination or base, `B`/`C` are sources, `D` is a constant or split field. `base` is the current stack frame start.
- Conditions are expressed as "condition true → skip next instruction; condition false → execute next instruction (normally a `JMP`)." "Next instruction" means the sequential `BCIns`; a taken `JMP` applies its offset from the following instruction.
- Version: LuaJIT 2.1 with extensive changes, assuming the `LJ_FR2` two-slot frame layout used by all supported platforms.
- Serialised bytecode format: `0x8b`.  This version extends `BMETH` dispatch to method-compatible userdata
  metatables.  Earlier formats are rejected because they cannot safely select the new call-frame semantics.
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
| `ISFALSEY` | A D | Execute next JMP if R(A) is nil or matches falsey mask D; otherwise skip JMP |

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
| `BFUNC` | A D | R(A) = canonical built-in callable(D) |
| `BMETH` | A J P | Runtime method dispatch for member string(P); ordinary field-call target J |

##### Built-in Callable Loads (`BFUNC`)

`BFUNC` loads a canonical native closure from the current Tiri state's built-in callable registry.  Operand `A` is
the destination register and `D` is a `BuiltinCallableID`, generated from the fast-function definition order.  The
registry contains the exact closures published during library initialisation, including their environments and
upvalues, and the garbage collector roots every populated slot independently of the public module tables.

The instruction is a pure, non-allocating load.  It performs no global or table lookup, invokes no metamethod or user
code, and does not recreate a closure.  Rebinding a public entry such as `table.insert` therefore does not change the
callable loaded by an existing `BFUNC`.  The resulting value is an ordinary function and is invoked by the normal
`CALL`, `CALLM`, `CALLT` or `CALLMT` instructions.

The bytecode reader rejects an ID outside the generated range and rejects a missing or non-function registry slot.
It never installs `nil` as a fallback.  Interpreter backends load the state-local slot directly, while the JIT records
the same closure as a stable function constant without a runtime equality guard.

Dumps serialise only the numeric `BuiltinCallableID`; they never contain a closure or process-local address.  The
generated fast-function order is therefore a private bytecode ABI.  Adding, removing or reordering an entry requires
both a dump-version bump and an update to the generated-order fingerprint in `lj_ff.h`.  Debug disassembly resolves
the ID to its canonical `interface.function` name.

##### Runtime Method Dispatch (`BMETH`)

`BMETH` precedes two generated call blocks.  The fall-through block includes the receiver as native argument zero;
the jump target uses ordinary field-call arguments.  Dispatch first selects a registered canonical built-in.  If no
built-in matches, full userdata whose metatable carries the private method-compatible mark resolves the member
normally and uses the receiver block.  Every other receiver resolves the same member and jumps to the field block.
Both lookup paths preserve `__index` table and function behaviour.

The JIT guards the observed userdata metatable compatibility value before recording normal indexed lookup.  A trace
therefore side-exits when the same dynamic call site changes between compatible and ordinary userdata.

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

#### Object and Struct Field Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `OBGETF` | A B C P | R(A) = object field R(B)[str(C)]; P caches the object read-table index |
| `OBSETF` | A B C P | object field R(B)[str(C)] = R(A); P caches the object write-table index |
| `OBCALL` | A B C | Reserved object action/method call opcode |
| `STGETF` | A B C P | R(A) = struct field R(B)[str(C)]; P caches the struct field index |
| `STSETF` | A B C P | struct field R(B)[str(C)] = R(A); P caches the struct field index |

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
  - A statically proved bare-array loop stores the array in `R(A-2)`, initialises `R(A-1)` to nil and emits `ITERA`
    directly. `R(A-3)` is unused by this instruction.
  - If control var is nil, start at index 0; otherwise increment the control var by 1.
  - If `idx >= arr.len`: set `R(A)` to nil, proceed to `BC_ITERL` (loop exits).
  - Else: `R(A)` = index (integer tagged); `R(A-1)` updated to index; `R(A+1)` loaded via `lj_arr_getidx`.
  - JIT/hotcount entry exists as `lj_vm_IITERA`, mirroring `BC_ITERN`.
  - `ISARR` remains available for guarded legacy layouts, but generic-for lowering no longer infers array identity from
    the shape of a variable-load instruction.

#### Return Ops
| Opcode | Format | Description |
|--------|--------|-------------|
| `RETM` | A D | return R(A)...R(A+D+MULTRES-1) |
| `RET` | A D | return R(A)...R(A+D-2) |
| `RET0` | A D | return (no values) |
| `RET1` | A D | return R(A) (single value) |

#### Module Dependency Activation

| Opcode | Format | Description |
|--------|--------|-------------|
| `MODACT` | A D | Resolve dependency descriptor D and materialise its callable closures from R(A) onward |

`MODACT` is compiler-private and executes at the source declaration's activation position.  The descriptor provides
the canonical module name and ordered function names; no module or function name is repeated in the instruction
stream.  `A` is the first destination register and `D` is the descriptor ordinal.  A dependency with no referenced
functions still executes the opcode but writes no register.  Direct and extracted module calls both read the resulting
hidden locals and enter the shared native marshaller.  Activation materialises closures and may load a module, so the
JIT exits to the interpreter at this opcode rather than recording it in a trace.

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
| `RANGEPREP` | A D | Prepare the direct range at base R(A); D is a `RANGE_PREP_*` flag mask |
| `RANGEVAL` | A D | Generate the visible range value at R(A+7) from the prepared ordinal state; D is unused |

##### Direct Range Loops (`RANGEPREP`, `RANGEVAL`)

Direct ranges that require runtime preparation use `RANGEPREP` with the existing numeric `FORI`/`FORL` instructions.
The numeric loop advances an ordinal, while `RANGEVAL` derives the visible value from the original start and step
without accumulating floating-point error.  Safe compile-time integer ranges outside protected regions can emit
`FORI`/`FORL` directly and omit both range-specific instructions.

`RANGEPREP` uses the AD format:

- `A` is the first slot in the range-loop frame.
- `D` is a bit mask containing:
  - `RANGE_PREP_INCLUSIVE` (`1 << 0`): include an aligned stop value.
  - `RANGE_PREP_HAS_STEP` (`1 << 1`): read the explicit step from `R(A+2)`; otherwise infer `1` or `-1` from the
    start and stop.
  - `RANGE_PREP_DIRECT_INTEGER` (`1 << 2`): prepare the compact four-slot integer layout.
- On entry, `R(A)` is the start and `R(A+1)` is the stop.  `R(A+2)` is the explicit step when
  `RANGE_PREP_HAS_STEP` is set.
- Preparation validates finite bounds, a finite non-zero step, the range count, and representable progress.  Invalid
  input raises the numeric-range error instead of entering the loop.
- The instruction otherwise falls through to the following `FORI`; it has no bytecode jump operand.

The stack effects depend on `RANGE_PREP_DIRECT_INTEGER`:

| Relative slot | Compact integer layout | Full ordinal layout |
|---------------|------------------------|---------------------|
| `R(A+0)` | Integer start/index | Ordinal index, initialised to `0` |
| `R(A+1)` | Integer loop limit, adjusted for an exclusive stop | Final ordinal, `count - 1` |
| `R(A+2)` | Integer step | Ordinal step, always `1` |
| `R(A+3)` | Current integer value, initialised to nil and maintained by `FORI`/`FORL` | Current ordinal, initialised to nil and maintained by `FORI`/`FORL` |
| `R(A+4)` | Not part of the compact frame | Original start |
| `R(A+5)` | Not part of the compact frame | Prepared value step |
| `R(A+6)` | Not part of the compact frame | Integer-result flag (`1` for signed 32-bit integral values, otherwise `0`) |
| `R(A+7)` | Not part of the compact frame | Visible loop value, initialised to nil |

The compact layout occupies four slots and does not use `RANGEVAL`; the visible control variable is `R(A+3)`.  The
full layout occupies eight slots and uses `R(A+7)` as the visible control variable.

`RANGEVAL` also uses the AD format, but only `A` is meaningful.  It reads the current ordinal from `R(A+3)`, the
original start from `R(A+4)`, the value step from `R(A+5)`, and the integer-result flag from `R(A+6)`.  It writes
`R(A+7)` as `fma(ordinal, step, start)`, tagging the result as an integer when the flag is set and as a number
otherwise.  It then proceeds to the next instruction without branching.

The emitted control-flow pattern for the full layout is:

```text
RANGEPREP A, flags
FORI      A, loop_exit
RANGEVAL  A
loop_body
FORL      A, RANGEVAL
loop_exit:
```

`FORI` checks the prepared ordinal range.  An empty or initially out-of-bounds range transfers control to
`loop_exit`; otherwise it sets `R(A+3)` to the first ordinal and falls through to `RANGEVAL`.  `FORL` advances the
ordinal in `R(A)` and `R(A+3)` and jumps back to `RANGEVAL` while the ordinal remains within `R(A+1)`.  A `continue`
targets `FORL`, while a `break` targets `loop_exit`.  This keeps range preparation outside the loop and executes
`RANGEVAL` exactly once for every body iteration.

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

Ordinary array bytecodes record bounds guards and inline loads or stores for supported numeric and garbage-collected
element types.  Other element types use the corresponding helper after the guard:

- `AGETV`/`AGETB` fall back to `IRCALL_lj_arr_getidx`.
- `ASETV`/`ASETB` fall back to `IRCALL_lj_arr_setidx`.

Safe array bytecodes use observation-specific recording:

- A non-array receiver, or an array with a non-integer key, records the ordinary indexed lookup and metamethod path.
- An in-bounds `ASGETV` array access narrows the variable index, guards it against the current array length, and uses
  the same inline load or `lj_arr_getidx` fallback as `AGETV`.
- An observed out-of-bounds `ASGETV` access terminates the trace with an interpreter link before the bytecode.  The
  interpreter then writes `nil` to the destination.
- A native-array `ASGETB` observation disables JIT recording for that prototype.  This prevents installation of a
  partial trace which can restore a stale destination slot at a later safe-navigation join.

The recorder does not emit `IRCALL_lj_arr_safe_getidx`.  That helper is used by the interpreter implementations.

### 5.6 Parser Integration

When the parser can determine at compile time that an expression is an array (via type annotations or `array.new()` tracking), it emits array bytecodes directly:

- Known array + variable index → `BC_AGETV` / `BC_ASETV`
- Known array + literal index (0-255) → `BC_AGETB` / `BC_ASETB`
- Safe navigation on a known array with a variable numeric-capable index → `BC_ASGETV`
- Safe navigation on a known array with a literal numeric index (0-255) → `BC_ASGETB`

Safe indexing on a statically proved table uses ordinary `TGETV`, `TGETB` or `TGETS` bytecodes.  A proved string key
also uses ordinary table indexing.  When the receiver type is unresolved and the key may be numeric, the parser
retains `ASGETV` or `ASGETB` so a native-array value can preserve safe out-of-bounds semantics at run time.

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

## 6. Native Struct Field Operations

### 6.1 Overview

`GCstruct` field reads and writes use `STGETF` and `STSETF` when the parser knows that the base expression has the
`struct` type. These opcodes bypass the generic base-metatable lookup and C-closure call frame while preserving the
same field conversion, lifecycle, null-payload and error behaviour as `__index` and `__newindex`.

### 6.2 Encoding and Inline Cache

Both instructions use ABCP format. B identifies the struct register, C is the negated string-constant index used by
`TGETS`, and A is the destination for `STGETF` or source value for `STSETF`. P starts at `0xFFFFFFFF` and caches the
index into `GCstruct.def->Fields`.

Every cache hit validates both the index and the field's case-insensitive `strihash`. A miss performs a linear lookup
and replaces P, so a single polymorphic instruction site can safely alternate between different struct definitions.

### 6.3 Interpreter Semantics

| Bytecode | Operation | Fast-type failure | Cached value |
|----------|-----------|-------------------|--------------|
| `STGETF` | Load a named field or the bound `structSize` closure | `vmeta_tgets` | Field index |
| `STSETF` | Store a named writable field | `vmeta_tsets` | Field index |

The x64, ARM64 and PowerPC handlers call `bc_struct_getfield` and `bc_struct_setfield`. The helpers synchronise the Lua
frame before any operation that may allocate, protect setter values from garbage collection and restore the
interpreter stack after the shared struct field core completes. A non-struct value at a statically struct-typed access
site falls back to the ordinary metamethod path.

### 6.4 Parser and Platform Status

`ExpKind::IndexedStruct` lowers constant string member access to `STGETF`/`STSETF`. A struct member used as a callee is
downgraded to normal indexed access so helpers such as `value.structSize()` still resolve through `__index`.

The interpreter implementation covers x64, ARM64 and PowerPC. The JIT can record these opcodes: scalar numeric fields may lower to guarded XLOAD/XSTORE, while complex or lifecycle-bound fields fall back to the C helpers.

## 7. Control-Flow and Short-Circuit Patterns
### 7.1 Logical Operators (`and`, `or`)
- `a and b`: evaluate `a`; emit compare + `JMP` that branches past `b` when `a` is falsey. Truthy `a` falls through, so `b` is evaluated and its result replaces/occupies the same slot.
- `a or b`: evaluate `a`; emit compare + `JMP` that branches into `b` only when `a` is falsey. Truthy `a` falls through and becomes the result; `b` is untouched.
- Registers are normalised so the resulting value lives in the LHS register; `freereg` collapses after RHS evaluation to avoid leaks.

### 7.2 Ternary Operators (`cond ? true_val :> false_val`, `cond ?? true_val :> false_val`)
- Only one branch executes. The standard `? :>` form matches `if` falsey semantics: only `nil` and `false` branch to the false value.
- The extended `?? :>` form uses the same extended falsey set as `??`: `nil`, `false`, numeric zero, empty string, and empty collections.
- `IrEmitter::emit_ternary_expr` places both branches so the selected branch materialises into a single result register. Each branch frees temporaries before convergence, and `freereg` is patched back to guarantee a single-slot result.
- Example sketch:
  ```
  <eval cond in RA>
  ISFC   RA      ; standard form branches to false for nil/false
  JMP    false
  ; true branch emits true_val into RA
  JMP    end
false:
  ; false branch emits false_val into RA
end:
  ```

### 7.3 Presence Operator (`x?`) – Extended Falsey Check
- Falsey set: `nil`, `false`, numeric zero, empty string, empty arrays, and empty tables. If operand is in this set, result is `false` and RHS (if any) is skipped.
- Emission: one `BC_ISFALSEY` followed by a `JMP` to the falsey path. Nil is always checked. Its D operand is a
  mask containing `ISFALSEY_FALSE` (0x1), `ISFALSEY_ZERO` (0x2), `ISFALSEY_EMPTY_STR` (0x4), and
  `ISFALSEY_EMPTY_COLL` (0x8).
- Known operand types narrow the mask to the one applicable value check. Object, function, and struct operands
  degrade to the existing nil-only `BC_ISEQP` check.
- The JIT recorder specialises on the observed runtime type and records at most one value guard. Empty tables retain
  the previous NYI behaviour because determining table emptiness requires traversal.
- The falsey jump is patched after the truthy result path; truthy fallthrough sets the result and collapses `freereg`.

### 7.4 If-Empty Operator (`lhs ?? rhs`) – Short-Circuiting with Extended Falsey Semantics
- Evaluate `lhs`; if it is nil/false/0/""/empty array/empty table, evaluate `rhs` and return it; otherwise return `lhs` without touching `rhs`.
- Implemented in `IrEmitter::emit_if_empty_expr` plus helper routines in `operator_emitter.cpp`. `BC_ISFALSEY`
  branches directly to RHS evaluation for a selected falsey value; a truthy value skips the following `JMP`.
- The result register is the original `lhs` slot; RHS evaluation reuses it and collapses `freereg` afterward to avoid leaked arguments or vararg tails.

## 8. Register Semantics and Multi-Value Behaviour
### 8.1 Register Lifetimes, `freereg`, and `nactvar`
- `nactvar` tracks active local slots; `freereg` is the first free slot above them. Temporaries are allocated above `nactvar` and must be reclaimed when no longer needed.
- Helpers such as `expr_free`, `RegisterGuard`, and `ir_collapse_freereg` ensure temporaries are released so following expressions do not see spurious stack entries.
- Failing to collapse `freereg` manifests as leaked arguments in calls or extra return values in vararg contexts.

### 8.2 `ExpKind::Call` and Multi-Return Semantics
- A freshly emitted `BC_CALL` yields an `ExpKind::Call` tied to its base register and may return multiple values (`BC_CALLM`) if left uncapped.
- Binary operators and control-flow constructs force single-value semantics: they convert call expressions to registers (`expr_toanyreg`), then free the call expression to cap results at one slot.
- Multi-return propagation is allowed only when explicitly constructing vararg lists; otherwise collapse to a single register before further comparisons or jumps.

### 8.3 Preventing Register Leaks in Chained Operations
- Reuse the LHS register when chaining operators at the same precedence; allocate new temporaries only when operands cannot share.
- After emitting a RHS, immediately free or collapse temporaries so the stack height matches `freereg` expectations before emitting the next operator.
- Always call `expr_free` before reserving new registers when an operand might still own a multi-return slot.

## 9. Common Emission Patterns and Anti-Patterns
### 9.1 Canonical Patterns (Do This)
- Compare + `JMP` for "branch on not-equal": `ISEQP A,const; JMP target` with true skipping the jump; see the relevant compare/jump emission logic in `operator_emitter.cpp`.
- Presence / if-empty checks: `ISFALSEY A,mask; JMP target`, with nil-only cases using
  `ISEQP A,nil; JMP target`; see `operator_emitter.emit_presence_check` (called from
  `IrEmitter::emit_presence_expr`) and `IrEmitter::emit_if_empty_expr`.
- Logical short-circuit: `a or b` uses compare + `JMP` into RHS only when falsey; `a and b` jumps over RHS when falsey. Implemented via general binary operator emission in `operator_emitter.cpp` and `ir_emitter.cpp`.
- Ternary layout: condition in place, compare chain, then true/false blocks writing back into the same register, with end jump to merge; see `IrEmitter::emit_ternary_expr`.

### 9.2 Typical Mistakes (Do NOT Do This)
- Wiring compare + `JMP` backwards (treating "skip on equal" as "jump on equal"), which executes the wrong branch.
- Failing to collapse `freereg` after evaluating RHS, causing leaked stack slots that appear as extra arguments or returns.
- Allocating a new register for an operand without freeing the previous expression, allowing multi-return values to flow into subsequent operators.
- Regression tests that catch these issues: `src/tiri/tests/test_if_empty.tiri`, `test_presence.tiri`, logical operator suites, and ternary-focused cases; add new ones when patterns change.

## 10. Exception Handling and Type Fixing

### 10.1 Exception Handling Bytecodes (`BC_TRYENTER`, `BC_TRYLEAVE`, `BC_CHECK`, `BC_RAISE`)

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

### 10.2 Dynamic Result Classification

Return signatures have one static authority.  Explicit declarations produce checked entries and validated return
analysis produces trusted inferred entries.  When analysis cannot validate a function's results, the prototype sets
`DynamicResults` with a result count and stored-entry count of zero.  Execution never observes results to refine that
metadata, so a prototype signature is immutable after compilation or loading.

The JIT uses the flag itself to select its conservative prototype-specialisation policy.  Dynamic functions keep
normal language behaviour: different calls may return different categories and arities without acquiring a result
contract.

### 10.3 Prototype Signatures

Every typed function prototype carries one versioned signature containing:

- complete fixed parameter and semantic result counts;
- stored positional parameter and result entries;
- parameter/result variadic, explicit-result and dynamic-result flags;
- each entry's nominal type, portable structure or class constraint, nullability, required state, provenance and
  strength.

Constraints use stable 32-bit structure keys or class identifiers, never process-local pointers. Declared non-`any`
contracts have checked strength and statically validated inferred results have trusted strength. Explicit `any`
entries remain advisory, while dynamic results have no positional entries.

The signature is stored in the prototype allocation between upvalue descriptors and debug metadata. Its fields are
written explicitly because bytecode-loaded prototypes are allocated as raw GC memory. Both stripped and unstripped
bytecode dumps retain signatures, and nested prototypes are handled recursively.

Debug bytecode output prints a deterministic signature header and one line for every stored parameter and result
entry. It resolves known constraint names when possible and otherwise prints the stable identifier.

### 10.4 Module Dependency Descriptors

A compilation unit that declares `module x as mX` records its dependencies as portable descriptors on the unit's
prototype.  Module declarations are unit-scoped, so only the prototype that declared them carries a descriptor block;
ordinary functions carry none and pay nothing.

Each block holds a version, a dependency array and a function array:

- a dependency stores the canonical module name and the contiguous range of functions the unit references from it;
- a function stores the canonical function name and its owning dependency index.

Only names are persisted.  A native address or an export-list index is a process identity: it could not survive
serialisation, and a stored index would select the wrong function if the module's export list were reordered or
extended between writing and loading.  Names resolve against the global module registry in any process.

Descriptors are deduplicated within each compilation unit.  Aliases of one canonical module share one dependency, and
repeated references to one function share one entry.  Imported units are parsed independently but emit into the root
prototype, so two imported units may contribute distinct descriptors for the same canonical module.  Their ordinals
and activation positions remain separate.  A declaration that references no function still produces a dependency,
because the module must be resolved and retained regardless.

The block is stored in the prototype allocation after the signature, naturally aligned because its entries hold
`GCRef` fields.  Names are anchored as GC constants when the parser builds the prototype; a prototype rebuilt from a
dump has no such constants, so the collector marks the descriptor names directly.

Resolution produces a prototype-owned sidecar of non-owning pointers to the registry's callable records plus one
activation byte per descriptor.  The Lua state allocates this sidecar on first activation, the collector frees it with
the prototype, and it is never stored in a global collection.  Registry records remain stable until expunge, which
runs only once no Tiri state can execute, so a resolved pointer cannot dangle.  Descriptors resolve independently at
activation rather than at load, which preserves failure timing: a missing module or function is reported when its
dependency declaration executes.

The reader validates the block structurally before installing it: bounded counts, contiguous non-overlapping function
ranges, an owning dependency that actually contains each function, non-empty names and exact consumption of the
declared length.  It then requires exactly one `MODACT` for every descriptor, rejects unknown or repeated descriptor
ordinals, and verifies that each destination range fits the prototype frame.  Repeated canonical module names are
valid because imported compilation units can contribute independent descriptors.  Structural validity is not export
validity, so every name is revalidated against the registry at resolution.

The process-wide ownership, publication and lock contract is documented in
[MODULE_REGISTRY.md](../MODULE_REGISTRY.md).

#### Current Table Context

| Opcode | Format | Description |
|--------|--------|-------------|
| `CTXGET` | A | Load the state-local current table context into R(A) |
| `CTXENTER` | A | Enter the table receiver retained in R(A-1), if present, before argument evaluation |
| `CTXCALL` | ABC | Invoke a contextual fixed-argument call with the ordinary `CALL` B/C layout |
| `CTXCALLM` | ABC | Invoke a contextual multiple-argument call with the ordinary `CALLM` B/C layout |
| `CTXLEAVE` | A | Restore the inherited context and normalise results after a contextual call |
| `CTXCALLT` | AD | Transfer context ownership and invoke a fixed-argument tail call |

`CTXGET` is appended to the opcode set so existing opcode numbers remain stable. An empty physical context stack is
the permanent root sentinel and resolves dynamically through `L->env`; consequently a supported environment
replacement is visible to the next `CTXGET`.  The standalone `&&` current-context expression lowers directly to
`CTXGET`.  `&name` and the equivalent `&&.name` access lower to `CTXGET` followed by ordinary `TGET*` or `TSET*`
operations. Root writes therefore cross the same marked-environment mutation boundary as `_G` writes and cannot
bypass protected-global, const or sticky-type contracts.

Phase 2 deliberately stops recording a trace at `CTXGET`; Phase 5 will model context in recorder state and snapshots.

##### Context Ownership Layout (Gate B)

Each `lua_State` owns an explicit vector of non-root overrides. An entry stores a `GCRef` to a table and the owning
activation base as a stack-relative byte offset. Current-context lookup is constant time from the final entry. The
relative owner survives stack relocation, the GC traverses all entries, and state destruction releases the vector.
The root is never stored or popped and is always resolved from `L->env`.

This layout was selected over frame annotations because it does not change the architecture-specific frame layout,
keeps root environment replacement dynamic, and gives later error unwinding and tail transfer an explicit ownership
boundary.

##### Contextual Call Layout (Gate C)

Current-context access uses the dedicated `CTXGET` opcode. Table member calls use appended contextual variants of the
existing call family. Their register layout reserves the slot immediately before the ordinary call base for the
original receiver; the callable and written arguments otherwise retain the `CALL`/`CALLM` layout. `CTXENTER` performs
the runtime table gate after member lookup and before argument evaluation. `CTXCALL` repeats the gate idempotently at
the activation boundary, and `CTXLEAVE` restores the inherited context after a normal return. It shifts returned
values down over the retained receiver slot so expression result allocation remains identical to an ordinary member
call.

Named and computed lookup continue to use the appropriate existing `TGET*` operation while retaining the original
receiver. Safe calls branch around lookup, entry and argument evaluation on their `nil` path. Statically proved
strings, arrays, ranges, structures and objects retain their specialised receiver dispatch without emitting context
operations. Dynamically typed receivers use the contextual variants, whose runtime gate leaves non-table values and
their visible argument layout unchanged. `CTXCALLT` transfers a prepared receiver from the discarded activation to
the reused tail frame, replacing an override owned by that frame where necessary. Direct tail calls naturally retain
their existing frame owner. Variable-argument contextual calls retain the ordinary call-and-return form so their
multiple-result state does not cross a context helper boundary. Direct calls and compiler-managed module namespace
calls retain their existing bytecode and overhead.

Dedicated contextual call variants were selected because lowering through an ordinary call frame would lose the
receiver association before activation ownership can be established. The Phase 3 opcodes occupy values 122 through
125 after `CTXGET`; Phase 4 appends `CTXCALLT` at value 126, preserving all earlier opcode numbers.

##### Restoration and Tail Ownership (Gate D)

Fixed, zero and fixed-multiple returns release an override owned by the returning VM frame. `CTXLEAVE` remains
responsible for result normalisation and is idempotent with that release. Error unwinding prunes overrides above the
surviving activation and restores each owning frame before its `<close>` handlers run. Protected calls therefore
retain their caller context while abandoned callees cannot leak a receiver into a handler.

Asynchronous callback entry may suspend the interrupted context stack and expose the dynamic `L->env` root for the
callback's duration. Synchronous `lua_call()` and `lua_pcall()` re-entry does not use this boundary and inherits the
active context. Fixed-argument contextual tail calls use `CTXCALLT`; direct and variable-argument calls preserve their
existing tail or call-and-return paths respectively.

## 11. Testing, Debugging, and Tooling

### 11.1 Using Flute and Tiri Tests
- Run the Tiri regression tests under `src/tiri/tests/` (e.g. `test_if_empty.tiri`, `test_presence.tiri`, logical/ternary suites) to validate control-flow changes.
- When adding coverage, use side effects (counters, print hooks) on RHS expressions to prove short-circuiting, and capture varargs with `{...}` to detect leaked registers.

### 11.2 Disassembly and Bytecode Inspection
- Obtain bytecode via `mtDebugLog('disasm')` on a `tiri` object or run scripts with `--jit-options dump-bytecode,diagnose`.
- Map disassembly back to source by matching instruction order to expression evaluation order, then locate emission sites in `ir_emitter.cpp` or `operator_emitter.cpp`.
- Treat disassembly as the source of truth for branch direction when debugging control flow.
- For `BFUNC`, confirm that disassembly shows the canonical built-in name rather than only its numeric ID.
- Until source-level lowering emits `BFUNC`, use the focused parser unit tests to cover execution, dump round trips,
  malformed IDs, mutable public bindings, closure lifetime and JIT recording.

## 12. Maintenance Guidelines

- When touching conditional emission or short-circuit logic, update the opcode matrix and the relevant sections here.
- Add or adjust regression tests in `src/tiri/tests/` to cover new control-flow behaviours; rerun tests after installing a fresh build.
- Re-generate disassembly for representative snippets (logical ops, ternary, `??`, `?`) to verify register collapse and branch wiring.
- Reviewers should confirm emitted patterns match the documented skip/execute semantics and that test coverage exercises both true and false paths.
- Treat `BuiltinCallableID` values as serialised ABI values.  Any generated fast-function insertion, removal or
  reorder must update `BCDUMP_VERSION` and the fingerprint in `lj_ff.h`.
- Keep the state-local built-in callable registry immutable after library initialisation and retain its independent GC
  roots.  `BFUNC` must remain a direct load rather than a public table lookup.

## 13. Glossary and Quick Reference

### Bytecode Types and Structures
- `BCIns`: 64-bit packed bytecode instruction (was 32-bit in legacy LuaJIT).
- `BCOp`: Opcode enum, always in bits 0-7 of the instruction.
- `BCPOS`: Bytecode position index within a prototype's instruction array.
- `BCREG`: Register number (8-bit, field A/B/C).
- `BuiltinCallableID`: Stable-for-one-bytecode-version numeric ID generated from the fast-function definition order.

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
- Extended falsey: `nil`, `false`, numeric zero, empty string, empty arrays, empty tables.
- Short-circuit: using compare+`JMP` so RHS executes only when needed.
- Cheat sheet: `ISEQ*`/`ISNE*`/`IS*` comparisons — true skips next, false executes next; `and` skips RHS on falsey, `or` skips RHS on truthy; `x?` and `lhs ?? rhs` use chained equality tests with shared jump lists; ternary writes result back into the condition register with one branch skipped.

### Native Arrays
- `GCarray`: native array object with typed element storage.
- `LJ_TARRAY`: type tag for native arrays (value ~13).
- `ARRAY_FLAG_READONLY`: flag indicating array elements cannot be modified.
- `ARRAY_FLAG_EXTERNAL`: flag indicating array data is not owned by the GC.
- `AGETV`/`AGETB`/`ASETV`/`ASETB` provide direct array element access with bounds checking.
- `ASGETV`/`ASGETB` provide safe array access (returns nil for out-of-bounds) for the `?[]` operator.

### Native Structs
- `GCstruct`: native struct header referencing an inline or external payload and its stable `struct_record` definition.
- `LJ_TSTRUCT`: type tag for native structs.
- `STGETF`/`STSETF`: named field access with a P32 field-index cache and generic metamethod fallback.

### Exception Handling
- `TryBlockDesc`: metadata for try blocks stored in `GCproto.try_blocks[]`.
- `TryHandlerDesc`: metadata for exception handlers stored in `GCproto.try_handlers[]`.

### Runtime Type Contracts
- `CONTRACT A, D`: validates values starting at register `A` using the portable descriptor string at constant `D`.
  Descriptors encode the boundary, exact Tiri types, nullable/required flags, labels, named-structure identity and
  fixed or dynamic result shape.
- `MRSAVE A` / `MRRESTORE A`: preserve the VM multi-result count and returned values in a state-owned nested save stack
  while return-path `<close>` and `defer` handlers execute. Restored values begin at `R(A+1)`.
- Basic tag contracts use trace slot specialisation. Prototypes needing structure identity, range metatable,
  callable-value or dynamic-result predicates remain interpreter-only until those predicates have dedicated trace IR
  guards.
- `BFUNC A, D`: loads the canonical built-in callable identified by `BuiltinCallableID(D)` into `R(A)`.
- The private bytecode dump version is `0x8b`.  It includes a length-delimited, schema-versioned signature and a
  length-delimited module dependency block before each prototype's bytecode, and preserves constant-table
  classification flags.  Version `0x87` replaced the compiler-private `mod['\31dependency']` call emitted by `0x86`
  with `MODACT`; version `0x88` adds `BFUNC` and its generated-ID ABI; version `0x8b` adds method-compatible userdata
  dispatch to `BMETH`.  All older versions are rejected at the header rather than retaining compatibility shims.
  Discard old dumps and rebuild them from source.
