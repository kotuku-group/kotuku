
## Troubleshooting Register Allocation

When changing emission logic:

- Never reduce `fs->freereg` below `fs->varmap.size()`; locals are stored there.
- Ensure every path that creates a `VCALL` either converts it to `VNONRELOC` or signals to assignment helpers how many results the call should return.
- The helper `expr_discharge()` is frequently used to normalise expressions before storage; inspect current usage before inventing new patterns.

### Register Management for Unary/Postfix Operators

When implementing operators that transform a value in-place (like unary `-`, `not`, or custom postfix operators):

1. **Modifying the Same Register (In-Place):**
   ```c
   BCREG reg = expr_toanyreg(fs, e);
   // Emit bytecode that modifies reg in-place
   bcemit_AD(fs, BC_UNM, reg, reg);  // Example: negate the value
   // Expression still points to same register
   ```
   **Issue:** If the original variable is referenced again later in the same expression, it will get the modified value, not the original. Example: `x? and x` would see boolean for both uses of `x`.

2. **Allocating a New Register (Recommended for Value-Transforming Operators):**
   ```c
   BCREG src_reg = expr_toanyreg(fs, e);
   expr_free(fs, e);              // CRITICAL: Free source register first
   BCREG dest_reg = fs->freereg;
   bcreg_reserve(fs, 1);          // Allocate new register for result
   // Emit bytecode that reads src_reg, writes to dest_reg
   bcemit_AD(fs, BC_KPRI, dest_reg, VKTRUE);  // Example
   expr_init(e, VNONRELOC, dest_reg);
   ```
   **Benefits:** Original value remains accessible; no multi-value return issues.

3. **Using VRELOCABLE (For Simple Transformations):**
   ```c
   expr_toanyreg(fs, e);
   expr_free(fs, e);                            // Free old register
   e->u.s.info = bcemit_AD(fs, op, 0, e->u.s.info);  // Emit instruction
   e->k = VRELOCABLE;                           // Mark as relocatable
   ```
   This pattern (from `bcemit_unop()`) defers register allocation until later.

**Common Pitfall:** Forgetting `expr_free()` before allocating a new register causes the function to return multiple values (the original value AND the transformed value), which manifests as extra arguments in function calls or assignment contexts.

### CALL Instructions and Base Registers

- After a `BC_CALL` instruction executes, the result(s) are placed starting at the base register, **overwriting the function** that was there.
- When emitting multiple calls that reuse the same base register, always move values to their destination positions **before** loading the next function into the base register. Otherwise, the previous result will be overwritten.
- Example pattern for chained operations:
  ```c
  // CORRECT: Move result to argument position before reloading function
  expr_toreg(fs, previous_result, arg_position);
  expr_toreg(fs, new_function, base_register);

  // INCORRECT: Loading function first overwrites the result
  expr_toreg(fs, new_function, base_register);  // Overwrites result!
  expr_toreg(fs, previous_result, arg_position);  // Too late
  ```

### Controlling VCALL Result Counts

- By default, `VCALL` expressions can return multiple values, which may leak into assignment contexts. To restrict a call to a single result:
  - Set `VCALL_SINGLE_RESULT_FLAG` in `expr->u.s.aux` (bitwise OR with base reg)
  - Call `expr_discharge()` to convert the `VCALL` to `VNONRELOC`
  - The flag is automatically handled in `expr_discharge()` and `assign_adjust()`
- This pattern ensures chained operations don't expose multi-value semantics to the assignment machinery.

### Multi-Return Function Calls as Binary Operator Operands

When a function returning multiple values is used as an operand in a binary operator (e.g., `1 & returns_two_values()`), standard Lua semantics dictate that only the first return value should be used. However, this requires careful handling in the parser.

**The Problem:**

For operators implemented via library calls (like bitwise operators calling `bit.band`), the base register selection logic checks if operands are at the top of the stack:

```cpp
if (rhs->k IS ExpKind::NonReloc and rhs->u.s.info >= fs->varmap.size() and rhs->u.s.info + 1 IS fs->freereg) {
   base = rhs->u.s.info;  // Reuse RHS register
}
```

This check **fails** when RHS is still `ExpKind::Call` (not yet discharged to `NonReloc`), causing:
1. A new base register to be allocated at `fs->freereg`
2. The library call result to go to the wrong register
3. The local variable assignment to use the wrong value (the raw call result instead of the operator result)

**Example Bug Scenario:**

```lua
local function returns6_99()
   return 6, 99
end

local x = 1 & returns6_99()  -- Expected: bit.band(1, 6) = 0, Got: 6
```

The bytecode would show:
```
CALL       A=R2 B=#2 C=#1    -- returns6_99() result (6) in R2
KSHORT     A=R5 D=#1         -- Load 1 to R5
MOV        A=R6 D=R2         -- Copy 6 to R6
TGETS      A=R3 ... "band"   -- Load bit.band to R3 (not R2!)
CALL       A=R3 B=#2 C=#3    -- bit.band result (0) goes to R3
GGET       A=R3 D="assert"   -- R3 immediately overwritten!
ISEQV      A=R2 D=R0         -- Compares R2 (still 6!) with expected
```

**The Solution:**

Discharge `Call` expressions to `NonReloc` **before** base register calculation:

```cpp
static void bcemit_bit_call(FuncState *fs, std::string_view FName, ExpDesc *LHS, ExpDesc *RHS)
{
   RegisterAllocator allocator(fs);

   // Discharge Call expressions to NonReloc first. This ensures that function calls
   // returning multiple values are properly truncated to single values before being
   // used as operands, matching Lua's standard semantics for binary operators.
   if (LHS->k IS ExpKind::Call) {
      ExpressionValue lhs_discharge(fs, *LHS);
      lhs_discharge.discharge();
      *LHS = lhs_discharge.legacy();
   }
   if (RHS->k IS ExpKind::Call) {
      ExpressionValue rhs_discharge(fs, *RHS);
      rhs_discharge.discharge();
      *RHS = rhs_discharge.legacy();
   }

   // Now the base register check will correctly identify NonReloc operands
   BCREG base;
   if (RHS->k IS ExpKind::NonReloc and ...) { ... }
}
```

**Key Insights:**

1. **Discharge timing matters**: The `expr_discharge()` function converts `ExpKind::Call` to `ExpKind::NonReloc` by setting `e->u.s.info = e->u.s.aux` (the call's base register). This must happen before any logic that depends on the expression kind.

2. **Runtime truncation is automatic**: The `BC_CALL` instruction's `B` field (result count) handles runtime truncation. When `bc_b=2` (expect 1 result), extra return values are discarded by the VM. The parser issue is about correctly tracking *which register* holds the result.

3. **This pattern applies to any operator implemented via function calls**: Not just bitwise operators, but any custom operator that emits library calls should discharge Call operands first.

4. **Test with multi-return functions**: Always include tests like `op(literal, multi_return_func())` to catch these issues. The test file `test_bitwise.tiri` includes `testMultipleReturnRHS` for this purpose.

### Preventing Orphaned Registers in Chained Operations

When implementing operators that can chain across precedence boundaries (e.g., operators with C-style precedence), be careful to avoid orphaning intermediate results on the register stack, which manifests as expressions returning multiple values instead of one.

**The Problem Pattern:**
1. First operation completes, stores result in register N, sets `freereg = N+1`
2. Control returns to the expression parser to handle the next operator
3. Parser allocates a NEW base register (often `freereg`) for the next operation
4. Register N is left orphaned on the stack, becoming an extra return value

**The Solution:**
Before allocating a base register for an operation, check if the LHS operand (which may be the previous operation's result) is already at the top of the stack. The check pattern is:
```c
if (lhs->k IS VNONRELOC and lhs->u.s.info >= fs->varmap.size() and lhs->u.s.info + 1 IS fs->freereg) {
   // LHS is at the top - reuse its register to avoid orphaning
   base_reg = lhs->u.s.info;
}
```

This commonly occurs when chaining across precedence levels where the parser returns control between operations rather than handling the entire chain in one function call.

**Debugging Orphaned Registers:**
- Symptom: Expressions return multiple values when they should return one
- Use printf debugging to trace `lhs->k`, `lhs->u.s.info`, `freereg`, and
  `nactvar` through operation sequences
- Check that `fs->freereg` is correctly adjusted after each operation completes
- Verify that base register reuse logic considers all stack-top scenarios

### Presence Operator Register Discipline

Investigations into the optional `?` operator highlighted several register-handling
rules that apply to any parser feature borrowing temporary slots:

1. **Surface the final value from the RHS register.**
   - After emitting the fallback value, update the expression descriptor to point at
     the RHS slot (e.g. `expr_init(e1, VNONRELOC, rhs_reg);`).
   - This mirrors the ternary operator contract and keeps concatenation chains over
     contiguous registers.

2. **Clamp `fs->freereg` using the borrowed register, not the assignment target.**
   - Collapse with `fs->freereg = rhs_reg + 1;` (or via `bcreg_free`) once the borrowed
     slot is written back.
   - Deriving the collapse point from the destination register leaves the borrowed
     slot marked live, causing `freereg` to drift upwards across chained operators.

3. **Maintain CAT chain contiguity.**
   - `BC_CAT` expects source registers to be consecutive. If an operator copies a
     fallback result into a non-contiguous slot, concatenations may emit duplicate
     values or raise "function or expression too complex" during parsing.
   - When debugging, dump the bytecode to verify CAT operands stay compact (for
     example `R3`, `R4`, `R5`).

4. **Use logical operators as a template.**
   - The `and` / `or` implementations demonstrate how to preserve operand slots across
     chained expressions with short-circuit semantics.

### Implementing Binary Logical Operators with Short-Circuiting

When implementing binary logical operators (like `or`, `and`, or custom variants like the `?`
if-empty operator), follow a two-phase pattern: setup in `bcemit_binop_left()` and completion in `bcemit_binop()`.

**The Pattern:**
1. **In `bcemit_binop_left()`**: Set up jumps in `e->t` for truthy LHS (to skip RHS) or
   `NO_JMP` for falsey LHS (to evaluate RHS). For truthy constants, you must allocate a
   register before loading them:
   ```c
   if (e->k IS VKSTR or e->k IS VKNUM or e->k IS VKTRUE) {
     bcreg_reserve(fs, 1);  // CRITICAL: Allocate register first
     expr_toreg_nobranch(fs, e, fs->freereg-1);
     pc = bcemit_jmp(fs);  // Jump to skip RHS
   }
   ```
   **Never use `NO_REG` as a register number** - it causes runtime crashes in `lj_BC_MOV`.

2. **In `bcemit_binop()`**: Check if `e1->t != NO_JMP` to determine if LHS is truthy:
   ```c
   if (e1->t != NO_JMP) {
     // LHS is truthy - patch jumps to skip RHS, return LHS
     jmp_patch(fs, e1->t, fs->pc);
     e1->t = NO_JMP;
     // Ensure LHS is in a register (already done in bcemit_binop_left() for constants)
   } else {
     // LHS is falsey - evaluate RHS
   }
   ```

**Critical Gotcha:**
- After `expr_toreg_nobranch()` with a proper register, the expression becomes `VNONRELOC`
- Check `e1->k IS VNONRELOC` before trying to load it again in `bcemit_binop()`
- If the constant was already loaded in `bcemit_binop_left()`, it's already in a register

**Pattern for Extended Falsey Checks** (as used in `?` if-empty operator and `??` presence check):

The pattern chains multiple equality checks, where each check's JMP is patched to the same "falsey branch". The logic works as follows:

```c
// Check for nil
bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&nilv)));
check_nil = bcemit_jmp(fs);  // Skipped when reg IS nil
// Check for false
bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&falsev)));
check_false = bcemit_jmp(fs);  // Skipped when reg IS false
// Check for zero
bcemit_INS(fs, BCINS_AD(BC_ISEQN, reg, const_num(fs, &zerov)));
check_zero = bcemit_jmp(fs);  // Skipped when reg IS 0
// Check for empty string
bcemit_INS(fs, BCINS_AD(BC_ISEQS, reg, const_str(fs, &emptyv)));
check_empty = bcemit_jmp(fs);  // Skipped when reg IS ""

// Patch all JMPs to the falsey branch (e.g., RHS evaluation)
jmp_patch(fs, check_nil, falsey_branch_pc);
jmp_patch(fs, check_false, falsey_branch_pc);
jmp_patch(fs, check_zero, falsey_branch_pc);
jmp_patch(fs, check_empty, falsey_branch_pc);
```

**How This Works**:
- **When value is falsey** (e.g., `reg IS nil`): The matching `BC_ISEQP` (e.g., `BC_ISEQP reg, VKNIL`) finds equality → skips its JMP → execution continues past all checks → eventually reaches code that handles the falsey case (e.g., load false, evaluate RHS)
- **When value is truthy** (e.g., `reg IS "hello"`): ALL `BC_ISEQP` checks find inequality (reg != nil, reg != false, reg != 0, reg != "") → NONE skip their JMPs → the first JMP executes and jumps to the falsey branch → result is correct (e.g., load true, skip RHS)

**Note**: This pattern works because we chain multiple checks and patch all JMPs to the same location. If the value matches any falsey check, that check's JMP is skipped and execution continues. If the value matches none of the checks (is truthy), all JMPs execute and the first one jumps to the falsey branch. The exact behavior depends on how the falsey branch is structured.

### Implementing Operators with Extended Falsey Semantics

When implementing operators with custom falsey semantics (e.g., treating `0` and `""` as falsey
in addition to `nil` and `false`):

1. **Handle compile-time constants separately** in `bcemit_binop()`:
   ```c
   if (e1->k IS VKNIL or e1->k IS VKFALSE) {
     // Definitely falsey - evaluate RHS
   } else if (e1->k IS VKNUM and expr_numiszero(e1)) {
     // Zero is falsey - evaluate RHS
   } else if (e1->k IS VKSTR and e1->u.sval and e1->u.sval->len IS 0) {
     // Empty string is falsey - evaluate RHS
   }
   ```

2. **For runtime values**, emit a chain of comparison instructions:
   ```cpp
   // Check for nil
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&nilv)));
   check_nil = bcemit_jmp(fs);
   // Check for false
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, reg, const_pri(&falsev)));
   check_false = bcemit_jmp(fs);
   // Check for zero
   bcemit_INS(fs, BCINS_AD(BC_ISEQN, reg, const_num(fs, &zerov)));
   check_zero = bcemit_jmp(fs);
   // Check for empty string
   bcemit_INS(fs, BCINS_AD(BC_ISEQS, reg, const_str(fs, &emptyv)));
   check_empty = bcemit_jmp(fs);

   // All falsey checks jump to RHS evaluation
   jmp_patch(fs, check_nil, fs->pc);
   jmp_patch(fs, check_false, fs->pc);
   // ... etc
   ```

   **How This Works**:
   - When `reg IS nil`: `BC_ISEQP` skips the `JMP` → execution continues past all checks
   - When `reg != nil`: `BC_ISEQP` doesn't skip → `JMP` executes → jumps to RHS evaluation
   - Since we want to evaluate RHS when ANY falsey value is found, we patch all jumps to the RHS evaluation point
   - If the value is truthy, ALL checks skip their jumps, and execution continues (returning the truthy value)

3. **Use `lj_parse_keepstr()` for string constants** to ensure they're anchored and not GC'd:
   ```cpp
   emptyv.u.sval = lj_parse_keepstr(fs->ls, "", 0);
   ```

### Single-Character Token Recognition

When adding single-character operators (like `?`):

1. **Add the token** to `TKDEF` in `lj_lex.h` using the `T2` macro:
   ```cpp
   __(if_empty, ?)
   ```

2. **Handle recognition in the lexer** (`lj_lex.cpp`) in the switch statement:
   ```cpp
   case '?':
     lex_next(ls);
     return TK_if_empty;
   ```

### Multi-Character Token Recognition

When adding operators that extend reserved words:

1. **Add the token** to `TKDEF` in `lj_lex.h` using the `T2` macro.
2. **Handle recognition in the lexer** (`lj_lex.cpp`) after identifying a reserved word by checking the next character.
   **Important**: Check the next character (`ls->c`) **after** recognising the reserved word, not in the character switch statement.
3. **Map the token to an operator** in `token2binop()` in `parser.cpp`

## Debugging Parser Issues

When adding new operators or modifying expression parsing, use these strategies:

### Printf Debugging for Token Flow
Add temporary printf statements to trace token values through parsing:
```cpp
// In expr_primary() suffix loop or similar:
printf("[DEBUG] tok=%d ('%c' if printable)\n", ls->tok, (ls->tok >= 32 and ls->tok < 127) ? ls->tok : '?');
```
Remember to remove these before committing. Token values < 256 are ASCII characters;
values >= 256 are token type constants from `lj_lex.h`.

### Disassembling Generated Bytecode

The simplest way to debug generated bytecode is to create an isolated `.tiri` testing file and then run `origo` with the `--jit-options dump-bytecode` option.

### Diagnosing Multi-Value Return Issues

If an operator returns multiple values instead of one:

- **Symptom**: Expressions like `(x + y)?` produce two values in varargs contexts
- **Common cause**: Allocating a new register without calling `expr_free()` first
- **Fix pattern**:
  ```cpp
  BCREG src_reg = expr_toanyreg(fs, e);
  expr_free(fs, e);           // Free the source register
  BCREG dest_reg = fs->freereg;
  bcreg_reserve(fs, 1);       // Allocate result register
  ```
- **Test with**: Create a function that counts return values using `{...}` and `#`

### Incremental Testing Strategy
When implementing new operators:
1. Start with compile-time constants (`5`, `nil`) - test constant folding path
2. Test simple variables (`x`) - test basic runtime path
3. Test field access (`t.field`) - test suffix loop integration
4. Test parenthesised expressions (`(x + y)`) - test complex expressions
5. Test function calls that return a single result and those that return multiple results
   (`f(x)`) - test VCALL handling
6. Test in various contexts (assignments, function arguments, conditionals)
7. If issues arise, use `--jit-options dump-bytecode` as a source of truth rather than guessing the logic of emitted bytecode.
