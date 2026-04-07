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

      target.safe_nav_skip.patch_to(BCPos(this->func_state.pc));

      RegisterAllocator allocator(&this->func_state);
      allocator.release(target.reserved);
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
               auto inferred = infer_expression_type_ext(*values[i.raw()]);
               if (inferred.type != TiriType::Unknown and inferred.type != TiriType::Any and inferred.type != TiriType::Nil) {
                  VarInfo* info = &this->func_state.var_get(base.raw() + i.raw());
                  info->fixed_type = inferred.type;
                  info->object_class_id = inferred.object_class_id;
               }
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
            allocator.release(target.reserved);
            continue;
         }

         if (target.needs_var_add and target.pending_symbol) {
            // Create new local for this undeclared variable
            this->lex_state.var_new(BCReg(0), target.pending_symbol, target.pending_line, target.pending_column);
            this->lex_state.var_add(BCReg(1));
            BCReg local_slot = BCReg(this->func_state.varmap.size() - 1);

            // Infer type from initialiser expression
            if (i < values.size()) {
               auto inferred = infer_expression_type_ext(*values[i]);
               if (inferred.type != TiriType::Unknown and inferred.type != TiriType::Any and inferred.type != TiriType::Nil) {
                  VarInfo* info = &this->func_state.var_get(local_slot.raw());
                  info->fixed_type = inferred.type;
                  info->object_class_id = inferred.object_class_id;
               }
            }

            // If the value isn't already at the local slot, move it
            if (value_slot.raw() != local_slot.raw()) {
               bcemit_AD(&this->func_state, BC_MOV, local_slot, value_slot);
            }
            this->update_local_binding(target.pending_symbol, local_slot);
         }
         else {
            // Existing target - copy value to it
            ExpDesc value_expr;
            value_expr.init(ExpKind::NonReloc, value_slot);
            bcemit_store(&this->func_state, &target.storage, &value_expr);
         }
         allocator.release(target.reserved);
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
         if (is_blank_target(first->storage)) continue;

         ExpDesc stack_value;
         stack_value.init(ExpKind::NonReloc, this->func_state.freereg - 1);
         bcemit_store(&this->func_state, &first->storage, &stack_value);
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
      if (targets.size() > 1) {
         auto begin = targets.rbegin();
         ++begin;
         assign_from_stack(begin, targets.rend());
      }
      for (PreparedAssignment& prepared : targets) {
         allocator.release(prepared.reserved);
      }
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);
   assign_from_stack(targets.rbegin(), targets.rend());
   for (PreparedAssignment& prepared : targets) {
      allocator.release(prepared.reserved);
   }
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

   ExpDesc rhs;
   if (mapped.value() IS BinOpr::Concat) {
      ExpDesc infix = working;
      // CONCAT compound assignment: use OperatorEmitter for BC_CAT chaining
      this->operator_emitter.prepare_concat(ExprValue(&infix));
      auto list = this->emit_expression_list(values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      if (count != 1) {
         const ExprNodePtr& node = values.front();
         SourceSpan span = node ? node->span : SourceSpan{};
         return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
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
         const ExprNodePtr& node = values.front();
         SourceSpan span = node ? node->span : SourceSpan{};
         return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
      }

      rhs = list.value_ref();
      ExpDesc infix = working;

      // Use OperatorEmitter for arithmetic compound assignments (+=, -=, *=, /=, %=)
      this->operator_emitter.emit_binary_arith(mapped.value(), ExprValue(&infix), rhs);

      bcemit_store(&this->func_state, &target.storage, &infix);
   }

   register_guard.release_to(register_guard.saved());
   allocator.release(target.reserved);
   allocator.release(copies.reserved);
   release_indexed_original(this->func_state, target.storage);
   this->func_state.reset_freereg();
   register_guard.adopt_saved(BCReg(this->func_state.freereg));
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for an if-empty assignment (??=), assigning only if the target is nil, false, 0, or empty string.

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

      // Finalize deferred local variable now that expression is evaluated

      if (target.needs_var_add and target.pending_symbol) {
         this->lex_state.var_new(BCReg(0), target.pending_symbol, target.pending_line, target.pending_column);
         this->lex_state.var_add(BCReg(1));
         BCReg slot = BCReg(this->func_state.varmap.size() - 1);
         // Update target.storage to point to the new local
         target.storage.init(ExpKind::Local, slot);
         target.storage.u.s.aux = this->func_state.varmap[slot.raw()];
         this->update_local_binding(target.pending_symbol, slot);
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

   ExpDesc nilv(ExpKind::Nil);
   ExpDesc falsev(ExpKind::False);
   ExpDesc zerov(0.0);
   ExpDesc emptyv(this->lex_state.intern_empty_string());

   bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, lhs_reg, const_pri(&nilv)));
   ControlFlowEdge check_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, lhs_reg, const_pri(&falsev)));
   ControlFlowEdge check_false = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQN, lhs_reg, const_num(&this->func_state, &zerov)));
   ControlFlowEdge check_zero = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQS, lhs_reg, const_str(&this->func_state, &emptyv)));
   ControlFlowEdge check_empty = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
   BCPos assign_pos = BCPos(this->func_state.pc);

   auto list = this->emit_expression_list(values, count);
   if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

   if (count != 1) {
      const ExprNodePtr& node = values.front();
      SourceSpan span = node ? node->span : SourceSpan{};
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
   }

   ExpDesc rhs = list.value_ref();
   bcemit_store(&this->func_state, &target.storage, &rhs);

   check_nil.patch_to(BCPos(assign_pos));
   check_false.patch_to(BCPos(assign_pos));
   check_zero.patch_to(BCPos(assign_pos));
   check_empty.patch_to(BCPos(assign_pos));
   skip_assign.patch_to(BCPos(this->func_state.pc));

   register_guard.release_to(register_guard.saved());
   allocator.release(target.reserved);
   allocator.release(copies.reserved);
   release_indexed_original(this->func_state, target.storage);
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

      // Finalize deferred local variable now that expression is evaluated

      if (target.needs_var_add and target.pending_symbol) {
         this->lex_state.var_new(BCReg(0), target.pending_symbol, target.pending_line, target.pending_column);
         this->lex_state.var_add(BCReg(1));
         BCReg slot = BCReg(this->func_state.varmap.size() - 1);
         // Update target.storage to point to the new local
         target.storage.init(ExpKind::Local, slot);
         target.storage.u.s.aux = this->func_state.varmap[slot.raw()];
         this->update_local_binding(target.pending_symbol, slot);
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

   // Only check for nil (simpler and faster than ??= which checks nil, false, 0, and empty string)
   ExpDesc nilv(ExpKind::Nil);

   bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, lhs_reg, const_pri(&nilv)));
   ControlFlowEdge check_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
   BCPos assign_pos = BCPos(this->func_state.pc);

   auto list = this->emit_expression_list(values, count);
   if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

   if (count != 1) {
      const ExprNodePtr& node = values.front();
      SourceSpan span = node ? node->span : SourceSpan{};
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
   }

   ExpDesc rhs = list.value_ref();
   bcemit_store(&this->func_state, &target.storage, &rhs);

   check_nil.patch_to(BCPos(assign_pos));
   skip_assign.patch_to(BCPos(this->func_state.pc));

   register_guard.release_to(register_guard.saved());
   allocator.release(target.reserved);
   allocator.release(copies.reserved);
   release_indexed_original(this->func_state, target.storage);
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

   auto prv = (prvTiri *)this->func_state.ls->L->script->ChildPrivate;
   bool trace_assignments = (prv->JitOptions & JOF::TRACE_ASSIGNMENTS) != JOF::NIL;

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
         pf::Log("Parser").msg("[%d] assignment: prepared %s target, duplicated %d registers (R%d..R%d)",
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
