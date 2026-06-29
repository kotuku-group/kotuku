// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: global declaration emission
// #included from ir_emitter.cpp

//********************************************************************************************************************
// Emit bytecode for a global variable declaration statement, explicitly storing values in the global table.
// Handles multi-value returns from function calls (e.g., global a, b, c = f())

ParserResult<IrEmitUnit> IrEmitter::emit_global_decl_stmt(const GlobalDeclStmtPayload &Payload)
{
   auto nvars = BCReg(BCREG(Payload.names.size()));
   if (nvars IS 0) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   // Register all declared global names so nested functions can recognise them

   for (size_t i = 0; i < Payload.names.size(); ++i) {
      const Identifier& identifier = Payload.names[i];
      if (is_blank_symbol(identifier)) continue;
      if (GCstr *name = identifier.symbol) {
         this->func_state.declared_globals.insert(name);

         if (identifier.has_const) {
            // Validate: const requires initialiser
            if (i >= Payload.values.size()) {
               return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::ConstRequiresInitialiser,
                  std::format("const global '{}' requires an initialiser",
                     std::string_view(strdata(name), name->len))));
            }
            // Track this global as const for reassignment checking
            this->func_state.const_globals.insert(name);
         }
      }
   }

   // Handle conditional assignment (??=) for global declarations
   // The ??= operator only supports a single target variable

   if (Payload.op IS AssignmentOperator::IfEmpty) {
      if (nvars != 1) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant,
            "conditional assignment (?\?=) only supports a single target variable"));
      }

      const Identifier& identifier = Payload.names[0];
      if (is_blank_symbol(identifier) or not identifier.symbol) {
         this->func_state.reset_freereg();
         return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
      }

      GCstr *name = identifier.symbol;
      RegisterGuard register_guard(&this->func_state);
      RegisterAllocator allocator(&this->func_state);

      // Load current global value into a register

      ExpDesc global_var;
      global_var.init(ExpKind::Global, 0);
      global_var.u.sval = name;

      ExpressionValue lhs_value(&this->func_state, global_var);
      auto lhs_reg = lhs_value.discharge_to_any_reg(allocator);

      // Emit checks for empty values: nil, false, 0, "", and empty collections.

      FalseyJumpOptions options;
      options.include_empty_array = true;
      ControlFlowEdge falsey_edge = emit_falsey_jumps(
         this->func_state, this->lex_state, this->control_flow, lhs_reg, options);

      // Skip assignment if not empty

      ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
      BCPos assign_pos = BCPos(this->func_state.pc);

      // Emit the value expression(s) - if multiple values from RHS (e.g. function returning
      // multiple values), we take only the first value and discard the rest

      auto count = BCReg(0);
      auto list = this->emit_expression_list(Payload.values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      ExpDesc rhs = list.value_ref();

      // If we got multiple values (e.g. from a function call), adjust to take only the first one

      if (count > 1 or rhs.k IS ExpKind::Call) {
         this->lex_state.assign_adjust(1, count.raw(), &rhs);
      }

      ExpDesc target;
      target.init(ExpKind::Global, 0);
      target.u.sval = name;
      bcemit_store(&this->func_state, &target, &rhs);

      // Patch jumps
      falsey_edge.patch_to(assign_pos);
      skip_assign.patch_to(BCPos(this->func_state.pc));

      register_guard.release_to(register_guard.saved());
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Handle conditional assignment (?=) for global declarations - only checks for nil
   // The ?= operator only supports a single target variable

   if (Payload.op IS AssignmentOperator::IfNil) {
      if (nvars != 1) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant,
            "conditional assignment (?=) only supports a single target variable"));
      }

      const Identifier& identifier = Payload.names[0];
      if (is_blank_symbol(identifier) or not identifier.symbol) {
         this->func_state.reset_freereg();
         return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
      }

      GCstr* name = identifier.symbol;
      RegisterGuard register_guard(&this->func_state);
      RegisterAllocator allocator(&this->func_state);

      // Load current global value into a register

      ExpDesc global_var;
      global_var.init(ExpKind::Global, 0);
      global_var.u.sval = name;

      ExpressionValue lhs_value(&this->func_state, global_var);
      auto lhs_reg = lhs_value.discharge_to_any_reg(allocator);

      // Only check for nil (simpler and faster than ??=)

      FalseyJumpOptions nil_only;
      nil_only.include_false = false;
      nil_only.include_zero = false;
      nil_only.include_empty_string = false;
      ControlFlowEdge check_nil = emit_falsey_jumps(
         this->func_state, this->lex_state, this->control_flow, lhs_reg, nil_only);

      // Skip assignment if not nil

      ControlFlowEdge skip_assign = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
      BCPos assign_pos = BCPos(this->func_state.pc);

      // Emit the value expression(s) - if multiple values from RHS (e.g. function returning
      // multiple values), we take only the first value and discard the rest

      auto count = BCReg(0);
      auto list = this->emit_expression_list(Payload.values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      ExpDesc rhs = list.value_ref();

      // If we got multiple values (e.g. from a function call), adjust to take only the first one

      if (count > 1 or rhs.k IS ExpKind::Call) {
         this->lex_state.assign_adjust(1, count.raw(), &rhs);
      }

      ExpDesc target;
      target.init(ExpKind::Global, 0);
      target.u.sval = name;
      bcemit_store(&this->func_state, &target, &rhs);

      // Patch jumps
      check_nil.patch_to(assign_pos);
      skip_assign.patch_to(BCPos(this->func_state.pc));

      register_guard.release_to(register_guard.saved());
      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Use emit_expression_list to get all values into consecutive registers
   // This properly handles multi-value returns from function calls

   ExpDesc tail(ExpKind::Void);
   auto nexps = BCReg(0);
   if (not Payload.values.empty()) {
      auto list = this->emit_expression_list(Payload.values, nexps);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());
      tail = list.value_ref();
   }

   // Use assign_adjust to handle multi-value returns - this places values in consecutive registers
   // and pads with nil if there are more variables than expressions

   this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);

   // Now values are in consecutive registers starting at (freereg - nvars)

   BCReg value_base = BCReg(this->func_state.freereg - nvars.raw());

   // Store each value to its corresponding global variable

   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];

      if (is_blank_symbol(identifier)) continue; // Skip blank identifiers - value is discarded

      GCstr* name = identifier.symbol;
      if (not name) continue;

      // Create global variable target

      ExpDesc var;
      var.init(ExpKind::Global, 0);
      var.u.sval = name;

      // Create source expression from the value register

      ExpDesc value_expr;
      value_expr.init(ExpKind::NonReloc, value_base + i);

      bcemit_store(&this->func_state, &var, &value_expr); // Store to global
   }

   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
