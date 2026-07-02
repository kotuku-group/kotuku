// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: assignment emission
// #included from ir_emitter.cpp

static bool contains_safe_nav_target(const ExprNode& Expr)
{
   switch (Expr.kind) {
      case AstNodeKind::SafeMemberExpr:
      case AstNodeKind::SafeIndexExpr:
         return true;

      case AstNodeKind::MemberExpr: {
         const auto& payload = std::get<MemberExprPayload>(Expr.data);
         return payload.table and contains_safe_nav_target(*payload.table);
      }

      case AstNodeKind::IndexExpr: {
         const auto& payload = std::get<IndexExprPayload>(Expr.data);
         return payload.table and contains_safe_nav_target(*payload.table);
      }

      default:
         return false;
   }
}

static void patch_safe_nav_skip_here(PreparedAssignment& Target, FuncState& State)
{
   if (Target.safe_nav_skip.valid()) Target.safe_nav_skip.patch_to(BCPos(State.pc));
}

static void release_prepared_assignment(RegisterAllocator& Allocator, FuncState& State, PreparedAssignment& Target)
{
   patch_safe_nav_skip_here(Target, State);
   Allocator.release(Target.reserved);
   release_indexed_original(State, Target.storage);
}

static ParserResult<IrEmitUnit> assignment_value_count_error(
   IrEmitter* Emitter, const ExprNodeList& Values, std::string_view Message)
{
   const ExprNode* raw = nullptr;
   if (not Values.empty()) {
      const ExprNodePtr& first = Values.front();
      raw = first ? first.get() : nullptr;
   }
   SourceSpan span = raw ? raw->span : SourceSpan{};
   return ParserResult<IrEmitUnit>::failure(
      ParserError(ParserErrorCode::InternalInvariant, Token::from_span(span, TokenKind::Unknown), Message));
}

//********************************************************************************************************************
// Emit bytecode for a plain assignment, storing values into one or more target lvalues.  NB: This generic function
// exists over the local/global specific implementations because of the need to handle complex scenarios
// involving mixed scope variables on the LHS.

ParserResult<IrEmitUnit> IrEmitter::emit_plain_assignment(std::vector<PreparedAssignment> targets, const ExprNodeList& values)
{
   auto nvars = BCReg(BCREG(targets.size()));
   if (not nvars) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   if (nvars IS BCReg(1) and targets.front().safe_nav_skip.valid()) {
      PreparedAssignment& target = targets.front();
      // Single-target safe-nav assignment keeps the stronger short-circuit semantics: if the guarded
      // target failed during preparation, execution jumps past both RHS evaluation and the final store.
      ExpDesc tail(ExpKind::Void);
      auto nexps = BCReg(0);
      if (not values.empty()) {
         auto list = this->emit_expression_list(values, nexps);
         if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());
         tail = list.value_ref();
      }

      if (nexps IS BCReg(1)) {
         if (tail.k IS ExpKind::Call) {
            if (bc_op(*ir_bcptr(&this->func_state, &tail)) IS BC_VARG) {
               this->func_state.freereg--;
               tail.k = ExpKind::Relocable;
            }
            else {
               tail.u.s.info = tail.u.s.aux;
               tail.k = ExpKind::NonReloc;
            }
         }

         if (not is_blank_target(target.storage)) {
            bcemit_store(&this->func_state, &target.storage, &tail);
         }
      }
      else {
         this->lex_state.assign_adjust(1, nexps.raw(), &tail);
         if (not is_blank_target(target.storage)) {
            ExpDesc stack_value;
            stack_value.init(ExpKind::NonReloc, this->func_state.freereg - 1);
            bcemit_store(&this->func_state, &target.storage, &stack_value);
         }
      }

      RegisterAllocator allocator(&this->func_state);
      release_prepared_assignment(allocator, this->func_state, target);
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Count pending locals that need to be created after expression evaluation
   BCReg pending_locals = BCReg(0);
   for (const PreparedAssignment& target : targets) {
      if (target.needs_var_add) ++pending_locals;
   }

   // If ALL targets are new locals (undeclared), use a simpler approach similar to local declarations
   if (pending_locals IS nvars) {
      // Register all new variable names with var_new
      BCReg idx = BCReg(0);
      for (PreparedAssignment& target : targets) {
         if (target.pending_symbol) {
            this->lex_state.var_new(idx, target.pending_symbol, target.pending_line, target.pending_column);
            ++idx;
         }
      }

      // Evaluate expressions
      ExpDesc tail(ExpKind::Void);
      auto nexps = BCReg(0);
      if (not values.empty()) {
         auto list = this->emit_expression_list(values, nexps);
         if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());
         tail = list.value_ref();
      }

      // Use assign_adjust to place values correctly and handle multi-return
      this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);
      this->lex_state.var_add(nvars);

      // Update binding table and set fixed_type from initialisers
      BCReg base = BCReg(this->func_state.varmap.size() - nvars.raw());
      for (BCReg i = BCReg(0); i < nvars; ++i) {
         PreparedAssignment& target = targets[i.raw()];
         if (target.pending_symbol) {
            this->update_local_binding(target.pending_symbol, base + i);

            // Infer type from initialiser expression (same logic as emit_local_decl_stmt)
            if (i.raw() < values.size()) {
               this->apply_inferred_local_type(base + i, *values[i.raw()]);
            }
         }
      }

      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Mixed case or all-existing case: evaluate expressions first, then assign
   ExpDesc tail(ExpKind::Void);
   auto nexps = BCReg(0);
   if (not values.empty()) {
      auto list = this->emit_expression_list(values, nexps);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());
      tail = list.value_ref();
   }

   RegisterAllocator allocator(&this->func_state);

   // For mixed assignments with pending locals, we need special handling
   // regardless of whether nexps == nvars (for multi-return calls, nexps=1 but nvars>1)
   if (pending_locals > BCReg(0)) {
      // Handle call results - adjust for multi-return
      if (tail.k IS ExpKind::Call) {
         if (bc_op(*ir_bcptr(&this->func_state, &tail)) IS BC_VARG) {
            setbc_b(ir_bcptr(&this->func_state, &tail), nvars.raw() + 1);
            this->func_state.freereg--;
            allocator.reserve(BCReg(nvars.raw() - 1));
         }
         else {
            // Fixup call result count
            setbc_b(ir_bcptr(&this->func_state, &tail), nvars.raw() + 1);
            if (nvars > BCReg(1)) {
               allocator.reserve(BCReg(nvars.raw() - 1));
            }
         }
      }
      else {
         // Non-call: use assign_adjust to pad with nils if needed
         this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);
      }

      // Values are in consecutive registers starting at (freereg - nvars) for non-calls
      // For calls, tail.u.s.aux contains the base register
      BCReg value_base = (tail.k IS ExpKind::Call) ? BCReg(tail.u.s.aux)
                                                   : BCReg(this->func_state.freereg - nvars.raw());

      // Process targets from first to last
      for (size_t i = 0; i < targets.size(); ++i) {
         PreparedAssignment& target = targets[i];
         BCReg value_slot = value_base + BCReg(i);

         // Skip blank identifiers - the value is discarded
         if (is_blank_target(target.storage)) {
            release_prepared_assignment(allocator, this->func_state, target);
            continue;
         }

         if (target.needs_var_add and target.pending_symbol) {
            // Create new local for this undeclared variable
            BCReg local_slot = this->finalise_pending_local_assignment(target);

            // Infer type from initialiser expression
            if (i < values.size()) {
               this->apply_inferred_local_type(local_slot, *values[i]);
            }

            // If the value isn't already at the local slot, move it
            if (value_slot.raw() != local_slot.raw()) {
               bcemit_AD(&this->func_state, BC_MOV, local_slot, value_slot);
            }
         }
         else {
            // Existing target - copy value to it
            ExpDesc value_expr;
            value_expr.init(ExpKind::NonReloc, value_slot);
            bcemit_store(&this->func_state, &target.storage, &value_expr);
         }
         release_prepared_assignment(allocator, this->func_state, target);
      }

      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // No pending locals - use the original assignment logic
   auto assign_from_stack = [&](std::vector<PreparedAssignment>::reverse_iterator first,
      std::vector<PreparedAssignment>::reverse_iterator last)
   {
      for (; first != last; ++first) {
         // Skip blank identifiers - the value is discarded
         if (not is_blank_target(first->storage)) {
            ExpDesc stack_value;
            stack_value.init(ExpKind::NonReloc, this->func_state.freereg - 1);
            bcemit_store(&this->func_state, &first->storage, &stack_value);
         }
         release_prepared_assignment(allocator, this->func_state, *first);
      }
   };

   if (nexps IS nvars) {
      if (tail.k IS ExpKind::Call) {
         if (bc_op(*ir_bcptr(&this->func_state, &tail)) IS BC_VARG) {
            this->func_state.freereg--;
            tail.k = ExpKind::Relocable;
         }
         else {
            tail.u.s.info = tail.u.s.aux;
            tail.k = ExpKind::NonReloc;
         }
      }
      // Skip blank identifiers for the last target
      if (not is_blank_target(targets.back().storage)) {
         bcemit_store(&this->func_state, &targets.back().storage, &tail);
      }
      release_prepared_assignment(allocator, this->func_state, targets.back());
      if (targets.size() > 1) {
         auto begin = targets.rbegin();
         ++begin;
         assign_from_stack(begin, targets.rend());
      }
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);
   assign_from_stack(targets.rbegin(), targets.rend());
   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a compound assignment (+=, -=, etc.), combining a binary operation with storage.

ParserResult<IrEmitUnit> IrEmitter::emit_compound_assignment(AssignmentOperator op, PreparedAssignment target, const ExprNodeList &values)
{
   auto mapped = map_assignment_operator(op);
   if (not mapped.has_value()) {
      const ExprNode* raw = nullptr;
      if (not values.empty()) {
         const ExprNodePtr &first = values.front();
         raw = first ? first.get() : nullptr;
      }
      SourceSpan span = raw ? raw->span : SourceSpan{};
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
   }

   if (values.empty()) return this->unsupported_stmt(AstNodeKind::AssignmentStmt, SourceSpan{});

   auto count = BCReg(0);
   RegisterGuard register_guard(&this->func_state);

   RegisterAllocator allocator(&this->func_state);
   TableOperandCopies copies = allocator.duplicate_table_operands(target.storage);
   ExpDesc working = copies.duplicated;

   auto finish_compound_assignment = [&]() {
      register_guard.release_to(register_guard.saved());
      allocator.release(copies.reserved);
      release_prepared_assignment(allocator, this->func_state, target);
      this->func_state.reset_freereg();
      register_guard.adopt_saved(BCReg(this->func_state.freereg));
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   };

   ExpDesc rhs;
   const bool direct_known_non_array = (target.storage.k IS ExpKind::Local or target.storage.k IS ExpKind::Upval) and
      not (target.storage.result_type IS TiriType::Array or target.storage.result_type IS TiriType::Unknown or
         target.storage.result_type IS TiriType::Any);
   const bool use_append_helper = mapped.value() IS BinOpr::Concat and not direct_known_non_array;

   if (use_append_helper) {
      ExpDesc append_func;
      append_func.init(ExpKind::Global, 0);
      append_func.u.sval = this->func_state.ls->keepstr("array");
      this->materialise_to_next_reg(append_func, "array append interface");

      ExpDesc append_key(this->func_state.ls->keepstr("append"));
      expr_index(&this->func_state, &append_func, &append_key);
      this->materialise_to_next_reg(append_func, "array append helper");

      RegisterAllocator call_allocator(&this->func_state);
      call_allocator.reserve(BCReg(1));
      auto call_base = BCReg(append_func.u.s.info);

      this->materialise_to_next_reg(working, "array append compound receiver");

      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         return assignment_value_count_error(this, values,
            "compound assignment expects exactly one RHS value");
      }

      rhs = list.value_ref();
      this->materialise_to_next_reg(rhs, "array compound append argument");

      ExpDesc result;
      result.init(ExpKind::Call, bcemit_ABC(&this->func_state, BC_CALL, call_base, 2,
         this->func_state.freereg - call_base - 1));
      result.u.s.aux = call_base;
      if (target.storage.result_type IS TiriType::Array) result.result_type = TiriType::Array;

      bcemit_store(&this->func_state, &target.storage, &result);
   }
   else if (mapped.value() IS BinOpr::Concat) {
      ExpDesc infix = working;
      // CONCAT compound assignment: use OperatorEmitter for BC_CAT chaining
      this->operator_emitter.prepare_concat(ExprValue(&infix));
      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         return assignment_value_count_error(this, values,
            "compound assignment expects exactly one RHS value");
      }

      rhs = list.value_ref();
      this->operator_emitter.complete_concat(ExprValue(&infix), rhs);
      bcemit_store(&this->func_state, &target.storage, &infix);
   }
   else {
      this->materialise_to_next_reg(working, "compound assignment base");
      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         return assignment_value_count_error(this, values,
            "compound assignment expects exactly one RHS value");
      }

      rhs = list.value_ref();
      ExpDesc infix = working;

      // Use OperatorEmitter for arithmetic compound assignments (+=, -=, *=, /=, %=)
      this->operator_emitter.emit_binary_arith(mapped.value(), ExprValue(&infix), rhs);

      bcemit_store(&this->func_state, &target.storage, &infix);
   }

   return finish_compound_assignment();
}

//********************************************************************************************************************
// Emit bytecode for an if-empty assignment (??=), assigning only if the target is nil, false, 0, empty string, or
// an empty collection.

ParserResult<IrEmitUnit> IrEmitter::emit_if_empty_assignment(PreparedAssignment target, const ExprNodeList& values)
{
   if (values.empty() or not vkisvar(target.storage.k)) {
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, SourceSpan{});
   }

   // If the target is a newly created local (from an undeclared variable), skip the emptiness
   // checks and go straight to assignment. The variable is undefined, which is semantically empty.

   if (target.newly_created) {
      auto count = BCReg(0);
      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         const ExprNodePtr& node = values.front();
         SourceSpan span = node ? node->span : SourceSpan{};
         return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
      }

      // Finalise deferred local variable now that expression is evaluated

      if (target.needs_var_add and target.pending_symbol) {
         this->finalise_pending_local_assignment(target);
      }

      ExpDesc rhs = list.value_ref();
      bcemit_store(&this->func_state, &target.storage, &rhs);
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   auto count = BCReg(0);
   RegisterGuard register_guard(&this->func_state);

   // Use RegisterAllocator::duplicate_table_operands()
   RegisterAllocator allocator(&this->func_state);
   TableOperandCopies copies = allocator.duplicate_table_operands(target.storage);
   ExpDesc working = copies.duplicated;

   // Use ExpressionValue for discharge operations
   ExpressionValue lhs_value(&this->func_state, working);
   auto lhs_reg = lhs_value.discharge_to_any_reg(allocator);

   FalseyJumpOptions options;
   options.include_empty_array = true;
   ControlFlowEdge falsey_edge = emit_falsey_jumps(
      this->func_state, this->lex_state, this->control_flow, lhs_reg, options);

   // Safe-nav targets may already carry a skip edge from target preparation. Those jumps bypass this
   // whole conditional-assignment block. The checks below are only for the terminal lvalue once the
   // guarded chain has resolved successfully.
   ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
   BCPos assign_pos = BCPos(this->func_state.pc);

   auto list = this->emit_expression_list(values, count);
   if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

   if (count != 1) {
      return assignment_value_count_error(this, values,
         "?" "?= assignment expects exactly one RHS value");
   }

   ExpDesc rhs = list.value_ref();
   bcemit_store(&this->func_state, &target.storage, &rhs);

   falsey_edge.patch_to(BCPos(assign_pos));
   skip_assign.patch_to(BCPos(this->func_state.pc));

   register_guard.release_to(register_guard.saved());
   allocator.release(copies.reserved);
   release_prepared_assignment(allocator, this->func_state, target);
   this->func_state.reset_freereg();
   register_guard.adopt_saved(BCReg(this->func_state.freereg));
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for an if-nil assignment (?=), assigning only if the target is nil.
// This is a faster alternative to ??= when only nil checks are needed.

ParserResult<IrEmitUnit> IrEmitter::emit_if_nil_assignment(PreparedAssignment target, const ExprNodeList& values)
{
   if (values.empty() or not vkisvar(target.storage.k)) {
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, SourceSpan{});
   }

   // If the target is a newly created local (from an undeclared variable), skip the nil
   // check and go straight to assignment. The variable is undefined, which is nil.

   if (target.newly_created) {
      auto count = BCReg(0);
      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         const ExprNodePtr& node = values.front();
         SourceSpan span = node ? node->span : SourceSpan{};
         return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
      }

      // Finalise deferred local variable now that expression is evaluated

      if (target.needs_var_add and target.pending_symbol) {
         this->finalise_pending_local_assignment(target);
      }

      ExpDesc rhs = list.value_ref();
      bcemit_store(&this->func_state, &target.storage, &rhs);
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   auto count = BCReg(0);
   RegisterGuard register_guard(&this->func_state);

   // Use RegisterAllocator::duplicate_table_operands()
   RegisterAllocator allocator(&this->func_state);
   TableOperandCopies copies = allocator.duplicate_table_operands(target.storage);
   ExpDesc working = copies.duplicated;

   // Use ExpressionValue for discharge operations
   ExpressionValue lhs_value(&this->func_state, working);
   auto lhs_reg = lhs_value.discharge_to_any_reg(allocator);

   // Only check for nil (simpler and faster than ??= which checks nil, false, 0, empty strings, and empty
   // collections).
   FalseyJumpOptions nil_only;
   nil_only.include_false = false;
   nil_only.include_zero = false;
   nil_only.include_empty_string = false;
   ControlFlowEdge check_nil = emit_falsey_jumps(
      this->func_state, this->lex_state, this->control_flow, lhs_reg, nil_only);

   // As with ??= above, any safe-nav skip edge lands after this emitter's store path. The nil check here
   // runs only when target preparation proved that the guarded chain itself was non-nil.
   ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
   BCPos assign_pos = BCPos(this->func_state.pc);

   auto list = this->emit_expression_list(values, count);
   if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

   if (count != 1) {
      return assignment_value_count_error(this, values,
         "?= assignment expects exactly one RHS value");
   }

   ExpDesc rhs = list.value_ref();
   bcemit_store(&this->func_state, &target.storage, &rhs);

   check_nil.patch_to(BCPos(assign_pos));
   skip_assign.patch_to(BCPos(this->func_state.pc));

   register_guard.release_to(register_guard.saved());
   allocator.release(copies.reserved);
   release_prepared_assignment(allocator, this->func_state, target);
   this->func_state.reset_freereg();
   register_guard.adopt_saved(BCReg(this->func_state.freereg));
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Prepare assignment targets by resolving lvalues and duplicating table operands to prevent register clobbering.
// When AllocNewLocal is false, unscoped variables will NOT create new locals even when protected_globals
// is true. This is used for compound assignments (+=, -=) and if-empty assignments (??=) where the variable
// must already exist - we should modify the existing storage, not create a new local.

ParserResult<std::vector<PreparedAssignment>> IrEmitter::prepare_assignment_targets(
   const ExprNodeList &Targets, bool AllocNewLocal, bool AllowSafeNav)
{
   std::vector<PreparedAssignment> lhs;
   lhs.reserve(Targets.size());
   RegisterAllocator allocator(&this->func_state);

   auto script = this->func_state.ls->L->script;
   bool trace_assignments = (script->JitOptions & JOF::TRACE_ASSIGNMENTS) != JOF::NIL;

   for (const ExprNodePtr &node : Targets) {
      if (not node) {
         return ParserResult<std::vector<PreparedAssignment>>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "assignment target missing"));
      }

      PreparedAssignment prepared;
      auto lvalue = this->emit_lvalue_expr(*node, AllocNewLocal, AllowSafeNav ? &prepared.safe_nav_skip : nullptr);
      if (not lvalue.ok()) return ParserResult<std::vector<PreparedAssignment>>::failure(lvalue.error_ref());

      ExpDesc slot = lvalue.value_ref();

      // Check if this is an Unscoped variable that needs a new local
      // Keep it as Unscoped and defer local creation until after expression evaluation

      if (slot.k IS ExpKind::Unscoped) {
         prepared.needs_var_add  = true;
         prepared.newly_created  = true;
         prepared.pending_symbol = slot.u.sval;
         prepared.pending_line   = node->span.line;
         prepared.pending_column = node->span.column;
         // Don't convert to Local yet - keep as Unscoped for now
         // The actual local slot will be determined later in emit_plain_assignment
      }

      TableOperandCopies copies = allocator.duplicate_table_operands(slot);
      prepared.storage = copies.duplicated;
      prepared.reserved = std::move(copies.reserved);
      prepared.target = LValue::from_expdesc(&prepared.storage);

      if (trace_assignments and prepared.reserved.count().raw() > 0) {
         auto target_kind = prepared.target.is_indexed() ? "indexed" : "member";
         kt::Log("Parser").msg("[%d] assignment: prepared %s target, duplicated %d registers (R%d..R%d)",
            this->func_state.ls->linenumber.lineNumber(), target_kind,
            unsigned(prepared.reserved.count().raw()), unsigned(prepared.reserved.start().raw()),
            unsigned(prepared.reserved.start().raw() + prepared.reserved.count().raw() - 1));
      }

      if (prepared.target.is_local()) {
         for (PreparedAssignment& existing : lhs) {
            bool refresh_table = existing.target.is_indexed()
               and existing.target.get_table_reg() IS prepared.target.get_local_reg();

            bool refresh_key = existing.target.is_indexed() and is_register_key(existing.storage.u.s.aux)
               and existing.target.get_key_reg() IS prepared.target.get_local_reg();

            bool refresh_member = existing.target.is_member()
               and existing.target.get_table_reg() IS prepared.target.get_local_reg();

            if (refresh_table or refresh_key or refresh_member) {
               TableOperandCopies refreshed = allocator.duplicate_table_operands(existing.storage);
               existing.storage = refreshed.duplicated;
               existing.reserved = std::move(refreshed.reserved);
               existing.target = LValue::from_expdesc(&existing.storage);
            }
         }
      }

      lhs.push_back(std::move(prepared));
   }

   return ParserResult<std::vector<PreparedAssignment>>::success(std::move(lhs));
}
