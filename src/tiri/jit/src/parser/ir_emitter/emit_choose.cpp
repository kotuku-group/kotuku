// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: choose expression emission
// #included from ir_emitter.cpp

//********************************************************************************************************************
// Emit bytecode for a choose expression: choose scrutinee from pattern -> result ... end
// Generates if/elseif/else chain for pattern matching with equality tests.
// Supports tuple scrutinee: choose (expr1, expr2, ...) from (pattern1, pattern2, ...) -> result end

ParserResult<ExpDesc> IrEmitter::emit_choose_expr(const ChooseExprPayload &Payload)
{
   // Validate: must have either single scrutinee or tuple scrutinee, and at least one case
   if ((not Payload.scrutinee and not Payload.is_tuple_scrutinee()) or Payload.cases.empty()) {
      return this->unsupported_expr(AstNodeKind::ChooseExpr, SourceSpan{});
   }

   FuncState *fs = &this->func_state;
   RegisterGuard register_guard(fs);
   RegisterAllocator allocator(fs);

   // Determine if tuple or single scrutinee

   bool is_tuple = Payload.is_tuple_scrutinee();
   size_t tuple_arity = Payload.tuple_arity();
   std::vector<BCReg> scrutinee_regs;  // Registers holding scrutinee values (1 for single, N for tuple)
   BCReg result_reg;

   if (is_tuple) {
      // JIT safety: For local variables (like function parameters), compare directly against
      // their original registers (like single-scrutinee does). For non-locals (constants,
      // expressions), load them into new registers.
      //
      // A JIT bug occurs when we copy a parameter to a new register (MOV R3, R0) and then
      // the JIT trace records this aliasing relationship incorrectly. By comparing directly
      // against the original parameter registers (R0, R1), we avoid the aliasing issue.
      //
      // Result register is allocated FIRST to ensure it doesn't conflict with any
      // scrutinee registers.

      result_reg = BCReg(fs->freereg);
      allocator.reserve(BCReg(1));

      for (size_t i = 0; i < tuple_arity; ++i) {
         const ExprNodePtr& elem = Payload.scrutinee_tuple[i];
         auto elem_result = this->emit_expression(*elem);
         if (not elem_result.ok()) return elem_result;

         // Check if this element is a local variable BEFORE discharging
         bool elem_is_local = elem_result.value_ref().is_local();

         ExpressionValue elem_value(fs, elem_result.value_ref());
         BCReg reg = elem_value.discharge_to_any_reg(allocator);

         if (elem_is_local) {
            // For locals (params), use the original register directly - no copy
            // This avoids creating MOV R3, R0 which confuses the JIT tracer
            scrutinee_regs.push_back(reg);
         }
         else { // For non-locals (constants, expressions), allocate a dedicated register
            BCReg dest = fs->free_reg();
            if (reg.raw() != dest.raw()) {
               bcemit_AD(fs, BC_MOV, dest, reg);
               allocator.collapse_freereg(reg);
            }
            allocator.reserve(BCReg(1));
            scrutinee_regs.push_back(dest);
         }
      }
   }
   else if (Payload.has_inferred_arity()) {
      // Function call returning multiple values - arity inferred from first tuple pattern

      size_t arity = Payload.inferred_tuple_arity;
      BCReg base_reg = BCReg(fs->freereg);

      // Emit the scrutinee expression (should be a function call)

      auto scrutinee_result = this->emit_expression(*Payload.scrutinee);
      if (not scrutinee_result.ok()) return scrutinee_result;

      ExpDesc scrutinee_expr = scrutinee_result.value_ref();

      // If it's a call, adjust to capture N return values

      if (scrutinee_expr.k IS ExpKind::Call) {
         // Use setbc_b to request exactly 'arity' results
         // B = arity + 1 means "expect arity results"

         setbc_b(ir_bcptr(fs, &scrutinee_expr), int(arity) + 1);

         // Reserve registers for all return values

         if (arity > 1) allocator.reserve(BCReg(arity - 1));

         // Fill scrutinee_regs with consecutive registers

         for (size_t i = 0; i < arity; ++i) {
            scrutinee_regs.push_back(BCReg(base_reg.raw() + int(i)));
         }

         result_reg = base_reg;  // Result goes into first register
      }
      else {
         // Not a call - cannot match a single value against tuple patterns.
         // Emit an error to prevent out-of-bounds access when iterating tuple patterns.
         return ParserResult<ExpDesc>::failure(this->make_error(
            ParserErrorCode::UnexpectedToken,
            "tuple patterns require a function call that returns multiple values, not a single expression"
         ));
      }
   }
   else {
      // Evaluate single scrutinee into a temporary register

      auto scrutinee_result = this->emit_expression(*Payload.scrutinee);
      if (not scrutinee_result.ok()) return scrutinee_result;

      // Check if scrutinee is a local variable BEFORE discharging
      // (discharge changes Local to NonReloc, losing this information)

      bool scrutinee_is_local = scrutinee_result.value_ref().is_local();

      ExpressionValue scrutinee_value(fs, scrutinee_result.value_ref());
      BCReg scrutinee_reg = scrutinee_value.discharge_to_any_reg(allocator);
      scrutinee_regs.push_back(scrutinee_reg);

      // Determine result register allocation strategy:
      // - If scrutinee is a local variable (e.g., loop variable), allocate a SEPARATE result register
      //   to avoid clobbering the live variable.
      // - If scrutinee is a constant/temporary, reuse the same register for efficiency and correct
      //   semantics (assignment expects result in that register).
      //
      // Note: Reserve the scrutinee register in all cases to prevent pattern expressions from overwriting
      // it during case evaluation.

      if (scrutinee_is_local) {
         result_reg = fs->free_reg();
         allocator.reserve(BCReg(1));
      }
      else {
         result_reg = scrutinee_reg;
         allocator.reserve(BCReg(1));  // Reserve scrutinee_reg to protect it from reuse
      }
   }

   // For single scrutinee, use the first (only) element

   BCReg scrutinee_reg = scrutinee_regs.empty() ? BCReg(0) : scrutinee_regs[0];

   // Create escape list for jumps to end of choose expression

   ControlFlowEdge escapelist = this->control_flow.make_unconditional();

   // Prepare constant ExpDescs for primitive comparisons

   ExpDesc nilv(ExpKind::Nil);
   ExpDesc truev(ExpKind::True);
   ExpDesc falsev(ExpKind::False);

   // Check if there's an else clause or wildcard - if not, we need to emit nil for no-match
   // Also check if any case has a statement result

   bool has_else = false;
   bool has_statement_results = false;
   for (const auto& arm : Payload.cases) {
      if (arm.is_else or arm.is_wildcard) { has_else = true; }
      if (arm.has_statement_result) { has_statement_results = true; }
   }

   // Generate if/elseif chain for each case

   for (size_t i = 0; i < Payload.cases.size(); ++i) {
      const ChooseCase& case_arm = Payload.cases[i];
      bool has_next = (i + 1) < Payload.cases.size();

      if (case_arm.is_else or case_arm.is_wildcard) {
         // Else/wildcard branch - just emit result directly (no comparison)
         if (not case_arm.result and not case_arm.has_statement_result) {
            return this->unsupported_expr(AstNodeKind::ChooseExpr, case_arm.span);
         }

         ControlFlowEdge guard_jump;

         // Emit guard condition check if present - wildcards can have guards too

         if (case_arm.guard) {
            auto guard_result = this->emit_expression(*case_arm.guard);
            if (not guard_result.ok()) return guard_result;

            ExpressionValue guard_value(fs, guard_result.value_ref());
            BCReg guard_reg = guard_value.discharge_to_any_reg(allocator);

            // Jump to next case if guard is falsey (BC_ISF = jump if false)

            bcemit_INS(fs, BCINS_AD(BC_ISF, 0, guard_reg));
            guard_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

            allocator.collapse_freereg(guard_reg);
         }

         // Emit result - either expression or statement

         if (case_arm.has_statement_result) {
            auto stmt_result = this->emit_statement(*case_arm.result_stmt);
            if (not stmt_result.ok()) return ParserResult<ExpDesc>::failure(stmt_result.error_ref());
         }
         else {
            auto result = this->emit_expression(*case_arm.result);
            if (not result.ok()) return result;
            ExpressionValue result_value(fs, result.value_ref());
            result_value.discharge();
            this->materialise_to_reg(result_value.legacy(), result_reg, case_arm.is_wildcard ? "choose wildcard branch" : "choose else branch");
            // Note: Do not collapse result_reg here - it may be the same as scrutinee_reg.
         }

         if (has_next or guard_jump.valid()) {
            // Jump to end after wildcard/else (in case there are more branches or guard can fail)
            escapelist.append(BCPos(bcemit_jmp(fs)));
         }

         // Patch guard failure jump to after this case's result

         if (guard_jump.valid()) guard_jump.patch_here();
      }
      else if (case_arm.is_tuple_pattern) {
         // Tuple pattern match: compare each scrutinee position with corresponding pattern

         if (not case_arm.result and not case_arm.has_statement_result) {
            return this->unsupported_expr(AstNodeKind::ChooseExpr, case_arm.span);
         }

         ControlFlowEdge false_jump = this->control_flow.make_unconditional();

         // For each tuple position, emit comparison if not a wildcard
         for (size_t pos = 0; pos < case_arm.tuple_patterns.size(); ++pos) {
            // Skip wildcard positions
            if (case_arm.tuple_wildcards[pos]) continue;

            const ExprNodePtr& pat = case_arm.tuple_patterns[pos];
            if (not pat) continue;  // Null indicates wildcard placeholder

            auto pat_result = this->emit_expression(*pat);
            if (not pat_result.ok()) return pat_result;
            ExpDesc pattern_expr = pat_result.value_ref();

            BCReg scr_reg = scrutinee_regs[pos];

            // Generate ISNE* comparison for this position (same logic as single-value patterns)

            if (pattern_expr.k IS ExpKind::Nil) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scr_reg, const_pri(&nilv)));
            }
            else if (pattern_expr.k IS ExpKind::True) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scr_reg, const_pri(&truev)));
            }
            else if (pattern_expr.k IS ExpKind::False) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scr_reg, const_pri(&falsev)));
            }
            else if (pattern_expr.k IS ExpKind::Num) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEN, scr_reg, const_num(fs, &pattern_expr)));
            }
            else if (pattern_expr.k IS ExpKind::Str) {
               bcemit_INS(fs, BCINS_AD(BC_ISNES, scr_reg, const_str(fs, &pattern_expr)));
            }
            else { // Non-constant value - materialise to register and use ISNEV
               ExpressionValue pat_val(fs, pattern_expr);
               BCReg pat_reg = pat_val.discharge_to_any_reg(allocator);
               bcemit_INS(fs, BCINS_AD(BC_ISNEV, scr_reg, pat_reg));
               allocator.collapse_freereg(pat_reg);
            }

            // Jump to next case if this position doesn't match (conjunctive AND)
            false_jump.append(BCPos(bcemit_jmp(fs)));
         }

         // Emit guard condition check if present

         if (case_arm.guard) {
            auto guard_result = this->emit_expression(*case_arm.guard);
            if (not guard_result.ok()) return guard_result;

            ExpressionValue guard_value(fs, guard_result.value_ref());
            BCReg guard_reg = guard_value.discharge_to_any_reg(allocator);

            // Jump to next case if guard is falsey (BC_ISF = jump if false)

            bcemit_INS(fs, BCINS_AD(BC_ISF, 0, guard_reg));
            false_jump.append(BCPos(bcemit_jmp(fs)));

            allocator.collapse_freereg(guard_reg);
         }

         // Emit result - either expression or statement

         if (case_arm.has_statement_result) {
            auto stmt_result = this->emit_statement(*case_arm.result_stmt);
            if (not stmt_result.ok()) return ParserResult<ExpDesc>::failure(stmt_result.error_ref());
         }
         else {
            auto result = this->emit_expression(*case_arm.result);
            if (not result.ok()) return result;
            ExpressionValue result_value(fs, result.value_ref());
            result_value.discharge();
            this->materialise_to_reg(result_value.legacy(), result_reg, "choose tuple case result");
            // Note: Do not collapse result_reg here - it may overlap with scrutinee registers.
         }

         // Jump to end after this case

         if (has_next or not has_else) escapelist.append(BCPos(bcemit_jmp(fs)));

         // Patch false jump to next case

         false_jump.patch_here();
      }
      else {
         // Single-value pattern match: compare scrutinee_reg with pattern value

         if (not case_arm.pattern or (not case_arm.result and not case_arm.has_statement_result)) {
            return this->unsupported_expr(AstNodeKind::ChooseExpr, case_arm.span);
         }

         ControlFlowEdge false_jump;

         // Check for table pattern { key = value, ... }
         // Table patterns are handled specially - we extract the payload directly from the AST
         // and emit type checking + field comparison bytecode. We must NOT call emit_expression
         // for table patterns, as that would emit TDUP bytecode that gets overwritten by the
         // type() call, creating dead code that confuses the JIT's slot tracking.

         if (case_arm.is_table_pattern) {
            // Table pattern: { key1 = value1, key2 = value2, ... }

            const auto *table_payload = std::get_if<TableExprPayload>(&case_arm.pattern->data);
            if (not table_payload) return this->unsupported_expr(AstNodeKind::ChooseExpr, case_arm.span);

            lua_State *L = this->lex_state.L;

            // Helper to get constant string index
            auto str_const = [fs](GCstr* s) -> BCReg {
               return BCReg(const_gc(fs, obj2gco(s), LJ_TSTR));
            };

            // Type check - scrutinee must be a table
            // Call type(scrutinee) and compare result with "table"

            BCREG temp_base = fs->freereg;
            allocator.reserve(BCReg(2 + LJ_FR2));  // function slot, frame link, arg

            // Load 'type' global function -> temp_base

            bcemit_AD(fs, BC_GGET, temp_base, str_const(lj_str_newlit(L, "type")));

            // Copy scrutinee as argument -> temp_base + 1 + LJ_FR2

            bcemit_AD(fs, BC_MOV, temp_base + 1 + LJ_FR2, scrutinee_reg);

            // Call type(scrutinee) -> result in temp_base
            // BC_CALL A=base, B=2 (expect 1 result), C=2 (1 arg + 1)

            bcemit_ABC(fs, BC_CALL, temp_base, 2, 2);

            // Compare result with "table" string - jump if NOT equal

            bcemit_INS(fs, BCINS_AD(BC_ISNES, temp_base, str_const(lj_str_newlit(L, "table"))));
            false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

            allocator.collapse_freereg(BCReg(temp_base));

            // For each field in the pattern, check existence and value

            for (const TableField &field : table_payload->fields) {
               if (field.kind != TableFieldKind::Record or not field.name.has_value()) {
                  continue;  // Skip non-record fields (should have been caught by parser)
               }

               // Get field value: TGETS field_reg, scrutinee_reg, "key"

               BCREG field_reg = fs->freereg;
               allocator.reserve(BCReg(1));
               bcemit_tgets(fs, field_reg, scrutinee_reg.raw(), str_const(field.name->symbol).raw());

               // Emit expected value expression and compare

               auto value_result = this->emit_expression(*field.value);
               if (not value_result.ok()) return value_result;
               ExpDesc val = value_result.value_ref();

               // Generate ISNE* comparison based on value type - jump if NOT equal

               if (val.k IS ExpKind::Nil)        bcemit_INS(fs, BCINS_AD(BC_ISNEP, field_reg, const_pri(&nilv)));
               else if (val.k IS ExpKind::True)  bcemit_INS(fs, BCINS_AD(BC_ISNEP, field_reg, const_pri(&truev)));
               else if (val.k IS ExpKind::False) bcemit_INS(fs, BCINS_AD(BC_ISNEP, field_reg, const_pri(&falsev)));
               else if (val.k IS ExpKind::Num)   bcemit_INS(fs, BCINS_AD(BC_ISNEN, field_reg, const_num(fs, &val)));
               else if (val.k IS ExpKind::Str)   bcemit_INS(fs, BCINS_AD(BC_ISNES, field_reg, const_str(fs, &val)));
               else { // Non-constant value - materialise to register and use ISNEV
                  ExpressionValue val_expr(fs, val);
                  BCReg val_reg = val_expr.discharge_to_any_reg(allocator);
                  bcemit_INS(fs, BCINS_AD(BC_ISNEV, field_reg, val_reg));
                  allocator.collapse_freereg(val_reg);
               }
               false_jump.append(BCPos(bcemit_jmp(fs)));
               allocator.collapse_freereg(BCReg(field_reg));
            }
         }
         else {
            // Non-table pattern: emit the pattern expression and compare with scrutinee

            // Temporarily ensure freereg is above scrutinee_reg to prevent pattern expressions
            // from clobbering the scrutinee. Save and restore freereg to avoid affecting
            // code after the choose expression.

            BCREG saved_freereg = fs->freereg;
            if (fs->freereg <= scrutinee_reg.raw()) fs->freereg = BCREG(scrutinee_reg.raw() + 1);

            auto pattern_result = this->emit_expression(*case_arm.pattern);

            // Restore freereg to its saved value (but not below result_reg + 1 to preserve result)

            if (saved_freereg > BCREG(result_reg + 1)) fs->freereg = saved_freereg;
            else fs->freereg = BCREG(result_reg + 1);

            if (not pattern_result.ok()) return pattern_result;
            ExpDesc pattern_expr = pattern_result.value_ref();

            // Check for relational pattern (< <= > >=)
            if (case_arm.relational_op != ChooseRelationalOp::None) {
               // Relational patterns require both operands in registers
               // Generate: jump if condition is NOT satisfied (inverted logic)
               // For < pattern: jump if scrutinee >= pattern (use BC_ISGE)
               // For <= pattern: jump if scrutinee > pattern (use BC_ISGT)
               // For > pattern: jump if scrutinee <= pattern (use BC_ISLE)
               // For >= pattern: jump if scrutinee < pattern (use BC_ISLT)

               ExpressionValue pattern_value(fs, pattern_expr);
               BCReg pattern_reg = pattern_value.discharge_to_any_reg(allocator);

               BCOp bc_op;
               switch (case_arm.relational_op) {
                  case ChooseRelationalOp::LessThan:     bc_op = BC_ISGE; break; // jump if NOT <
                  case ChooseRelationalOp::LessEqual:    bc_op = BC_ISGT; break; // jump if NOT <=
                  case ChooseRelationalOp::GreaterThan:  bc_op = BC_ISLE; break; // jump if NOT >
                  case ChooseRelationalOp::GreaterEqual: bc_op = BC_ISLT; break; // jump if NOT >=
                  default: bc_op = BC_ISGE; break; // Shouldn't reach here
               }

               bcemit_INS(fs, BCINS_AD(bc_op, scrutinee_reg, pattern_reg));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
               allocator.collapse_freereg(pattern_reg);
            }
            // Equality pattern (default)
            else if (pattern_expr.k IS ExpKind::Nil) {
               // Generate: if scrutinee_reg != pattern_value then jump to next case
               // Use ISNE* bytecode for "not equal" comparison (jump if not equal)
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scrutinee_reg, const_pri(&nilv)));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
            }
            else if (pattern_expr.k IS ExpKind::True) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scrutinee_reg, const_pri(&truev)));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
            }
            else if (pattern_expr.k IS ExpKind::False) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEP, scrutinee_reg, const_pri(&falsev)));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
            }
            else if (pattern_expr.k IS ExpKind::Num) {
               bcemit_INS(fs, BCINS_AD(BC_ISNEN, scrutinee_reg, const_num(fs, &pattern_expr)));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
            }
            else if (pattern_expr.k IS ExpKind::Str) {
               bcemit_INS(fs, BCINS_AD(BC_ISNES, scrutinee_reg, const_str(fs, &pattern_expr)));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
            }
            else {
               // Non-constant pattern - need to materialise and use ISNEV
               ExpressionValue pattern_value(fs, pattern_expr);
               BCReg pattern_reg = pattern_value.discharge_to_any_reg(allocator);
               bcemit_INS(fs, BCINS_AD(BC_ISNEV, scrutinee_reg, pattern_reg));
               false_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
               allocator.collapse_freereg(pattern_reg);
            }
         }  // End of non-table pattern else block

         // Emit guard condition check if present

         if (case_arm.guard) {
            auto guard_result = this->emit_expression(*case_arm.guard);
            if (not guard_result.ok()) return guard_result;

            ExpressionValue guard_value(fs, guard_result.value_ref());
            BCReg guard_reg = guard_value.discharge_to_any_reg(allocator);

            // Jump to next case if guard is falsey (BC_ISF = jump if false)
            bcemit_INS(fs, BCINS_AD(BC_ISF, 0, guard_reg));
            false_jump.append(BCPos(bcemit_jmp(fs)));

            allocator.collapse_freereg(guard_reg);
         }

         // Emit result - either expression or statement

         if (case_arm.has_statement_result) {
            auto stmt_result = this->emit_statement(*case_arm.result_stmt);
            if (not stmt_result.ok()) return ParserResult<ExpDesc>::failure(stmt_result.error_ref());
         }
         else {
            auto result = this->emit_expression(*case_arm.result);
            if (not result.ok()) return result;
            ExpressionValue result_value(fs, result.value_ref());
            result_value.discharge();
            this->materialise_to_reg(result_value.legacy(), result_reg, "choose case result");
            // Note: Do not collapse result_reg here - it may be the same as scrutinee_reg,
            // and we need to preserve scrutinee_reg for subsequent case comparisons.
         }

         // Jump to end after this case (needed if there are more cases OR if there's no else)

         if (has_next or not has_else) escapelist.append(BCPos(bcemit_jmp(fs)));

         // Patch false jump to next case
         false_jump.patch_here();
      }
   }

   // If there's no else clause and we're in expression mode, emit nil as the fallback value
   // Skip nil fallback for statement-only choose expressions

   if (not has_else and not has_statement_results) {
      this->materialise_to_reg(nilv, result_reg, "choose no-match fallback");
   }

   // Patch all escape jumps to current position

   escapelist.patch_here();

   // For statement-mode choose, return nil since there's no meaningful result

   if (has_statement_results) {
      ExpDesc result(ExpKind::Nil);
      return ParserResult<ExpDesc>::success(result);
   }

   // Ensure freereg is exactly result_reg + 1 so that subsequent code doesn't think
   // there are intermediate values between the result and whatever comes next.
   // This is critical for expressions like concatenation that depend on consecutive registers.

   fs->freereg = BCREG(result_reg + 1);
   register_guard.disarm();  // Don't let guard restore freereg, we've set it correctly

   ExpDesc result;
   result.init(ExpKind::NonReloc, result_reg);
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a list of expressions, materialising intermediate results and returning the last expression.

ParserResult<ExpDesc> IrEmitter::emit_expression_list(const ExprNodeList &expressions, BCReg &count)
{
   count = BCReg(0);
   if (expressions.empty()) return ParserResult<ExpDesc>::success(ExpDesc(ExpKind::Void));

   ExpDesc last(ExpKind::Void);
   bool first = true;
   for (const ExprNodePtr &node : expressions) {
      if (not node) return this->unsupported_expr(AstNodeKind::ExpressionStmt, SourceSpan{});
      if (not first) this->materialise_to_next_reg(last, "expression list baton");
      auto value = this->emit_expression(*node);
      if (not value.ok()) return value;
      ExpDesc expr = value.value_ref();
      ++count;
      last = expr;
      first = false;
   }

   return ParserResult<ExpDesc>::success(last);
}
