// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: try...except...end statement emission (bytecode-level)
// This file is #included from ir_emitter.cpp
//
// This implements bytecode-level exception handling that emits try body and handlers inline (not in closures),
// allowing return/break/continue to work correctly.
//
// Bytecode structure:
//   BC_TRYENTER  base, try_block_index    ; Push exception frame
//   <try body bytecode>                   ; Inline try body
//   BC_TRYLEAVE  base, 0                  ; Pop exception frame (normal exit)
//   JMP          exit_label               ; Jump over handlers
//   handler_1:                            ; Handler entry point (recorded in TryHandlerDesc)
//   <handler1 bytecode>                   ; Inline handler body
//   JMP          exit_label
//   handler_2:
//   <handler2 bytecode>
//   JMP          exit_label
//   exit_label:
//
// Handler metadata (TryBlockDesc, TryHandlerDesc) is stored in the FuncState during compilation and copied to the
// GCproto during fs_finish().

//********************************************************************************************************************
// Emit bytecode for try...except...end exception handling blocks.
//
// This is the bytecode-level implementation that emits try body and handlers inline,
// allowing return/break/continue to correctly affect the enclosing function/loop.

ParserResult<IrEmitUnit> IrEmitter::emit_try_except_stmt(const TryExceptPayload &Payload)
{
   FuncState *fs = &this->func_state;
   BCReg base_reg = BCReg(fs->freereg);

   if (not Payload.try_block) return this->unsupported_stmt(AstNodeKind::TryExceptStmt, SourceSpan{});

   // Reserve a slot in try_blocks for this try block. We'll fill in the details later after emitting the try body
   // (which may contain nested try blocks with their own handlers).  This ensures first_handler_index is correct
   // after nested try blocks have added their handlers.

   uint16_t try_block_index = uint16_t(fs->try_blocks.size());

   // Validate limits

   if (try_block_index >= 0xFFFF) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "too many try blocks in function", SourceSpan{}));
   }

   // Determine handler count - if no except clauses, we'll emit a synthetic catch-all handler

   uint8_t handler_count = Payload.except_clauses.empty() ? 1 : uint8_t(Payload.except_clauses.size());

   // Push a placeholder TryBlockDesc - first_handler will be updated after emitting try body

   uint8_t entry_slots = uint8_t(base_reg.raw());
   uint8_t flags = 0;
   if (Payload.enable_trace) flags |= TRY_FLAG_TRACE;

   fs->try_blocks.push_back(TryBlockDesc{
      0,             // Place-holder; set correctly after try body
      handler_count,
      entry_slots,
      flags
   });

   // Track try depth for break/continue cleanup

   uint8_t saved_try_depth = fs->try_depth;
   fs->try_depth++;
   fs->runtime_scopes.push_back(RuntimeScope{ RuntimeScopeKind::Try, base_reg });

   // Emit BC_TRYENTER with try block index
   bcemit_AD(fs, BC_TRYENTER, base_reg, BCReg(try_block_index));

   // Emit the try body inline (not in a closure). Nested try blocks add their handlers during this phase.
   // We manually manage the scope here so we can emit BC_TRYLEAVE before defer execution.
   // The ScopeGuard destructor will call fscope_end() which executes defers AFTER BC_TRYLEAVE.
   {
      FuncScope try_scope;
      ScopeGuard try_guard(fs, &try_scope, FuncScopeFlag::None);
      LocalBindingScope binding_scope(this->binding_table);

      // Emit all statements in the try body
      for (const StmtNode& stmt : Payload.try_block->view()) {
         auto status = this->emit_statement(stmt);
         if (not status.ok()) {
            fs->try_depth = saved_try_depth;
            return status;
         }
         this->ensure_register_balance(describe_node_kind(stmt.kind));
      }

      // Emit BC_TRYLEAVE BEFORE the scope ends (before defers are executed)
      // This ensures defers run OUTSIDE the try block's exception protection
      bcemit_AD(fs, BC_TRYLEAVE, base_reg, BCReg(0));

      // ScopeGuard destructor runs here, calling fscope_end() which executes defers
   }

   lj_assertX(not fs->runtime_scopes.empty() and fs->runtime_scopes.back().kind IS RuntimeScopeKind::Try,
      "try runtime scope stack is unbalanced");
   fs->runtime_scopes.pop_back();

   fs->try_depth = saved_try_depth;

   // Emit success block (if present) - runs after defers, before jump over handlers
   if (Payload.success_block) {
      auto success_result = this->emit_block(*Payload.success_block, FuncScopeFlag::None);
      if (not success_result.ok()) {
         fs->try_depth = saved_try_depth;
         return success_result;
      }
   }

   // Jump over handlers (successful completion)
   ControlFlowEdge exit_jmp = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

   // Correctly determine first_handler_index - nested try blocks have added their handlers
   size_t first_handler_index = fs->try_handlers.size();

   // Update the placeholder TryBlockDesc with the correct first_handler
   fs->try_blocks[try_block_index].first_handler = uint16_t(first_handler_index);

   if (first_handler_index + Payload.except_clauses.size() >= 0xFFFF) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "too many exception handlers in function", SourceSpan{}));
   }

   // Emit handlers inline and record metadata

   std::vector<ControlFlowEdge> handler_exits;
   // Reserve a contiguous descriptor range before nested handler bodies append their own handlers.
   fs->try_handlers.resize(first_handler_index + Payload.except_clauses.size());
   size_t handler_index = first_handler_index;

   for (const auto &clause : Payload.except_clauses) {
      if (clause.filter_codes.size() > MAX_EXCEPTION_FILTER_CODES) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "too many error-code filters in try block", SourceSpan{}));
      }

      // Pack filter codes inline (up to MAX_EXCEPTION_FILTER_CODES 16-bit error codes into a 64-bit integer)
      uint64_t packed_filter = 0;
      if (not clause.filter_codes.empty()) {
         int shift = 0;
         for (const auto &code_expr : clause.filter_codes) {
            if (not code_expr) continue;

            // Try to evaluate the expression as a constant
            auto code_result = this->emit_expression(*code_expr);
            if (not code_result.ok()) {
               fs->try_depth = saved_try_depth;
               fs->reset_freereg();
               return ParserResult<IrEmitUnit>::failure(code_result.error_ref());
            }

            ExpDesc code = code_result.value_ref();

            // We need the numeric value - if it's a constant, extract it
            if (code.k IS ExpKind::Num) {
               uint16_t code_val = uint16_t(code.number_value());
               packed_filter |= (uint64_t(code_val) << shift);
               shift += 16;
            }
            else { // Non-numeric codes are an error
               expr_free(fs, &code);
               fs->try_depth = saved_try_depth;
               fs->reset_freereg();
               return ParserResult<IrEmitUnit>::failure(this->make_error(
                  ParserErrorCode::InternalInvariant, "Non-numeric filter in try block", SourceSpan{}));
            }
         }
      }

      BCREG saved_nactvar = fs->varmap.size();
      BCReg saved_freereg = BCReg(fs->freereg);
      BCPOS handler_pc = fs->pc;
      BCREG exception_reg = saved_nactvar;
      bcreg_reserve(fs, BCReg(1));
      this->lex_state.var_new_fixed(BCReg(0), VARNAME_EXCEPTION);
      this->lex_state.var_add(BCReg(1));
      this->handler_exceptions.push_back(BCReg(exception_reg));

      LocalBindingScope binding_scope(this->binding_table);
      if (clause.exception_var.has_value() and clause.exception_var->symbol) {
         BCReg visible_reg = BCReg(fs->varmap.size());
         bcreg_reserve(fs, BCReg(1));
         this->lex_state.var_new(BCReg(0), clause.exception_var->symbol,
            clause.exception_var->span.line, clause.exception_var->span.column);
         this->lex_state.var_add(BCReg(1));
         fs->var_get(visible_reg).binding_id = clause.exception_var->binding_id;
         this->update_local_binding(clause.exception_var->symbol, visible_reg);
         bcemit_AD(fs, BC_MOV, visible_reg, BCReg(exception_reg));
      }

      if (clause.block) {
         auto handler_result = this->emit_block(*clause.block, FuncScopeFlag::None);
         if (not handler_result.ok()) {
            this->handler_exceptions.pop_back();
            fs->try_depth = saved_try_depth;
            return handler_result;
         }
      }
      this->handler_exceptions.pop_back();
      this->lex_state.var_remove(saved_nactvar);
      fs->freereg = saved_freereg.raw();

      fs->try_handlers[handler_index++] = TryHandlerDesc{ packed_filter, handler_pc, exception_reg };

      // Jump to exit after handler
      handler_exits.push_back(this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs))));
   }

   // If no except clauses were provided, emit a synthetic catch-all handler that silently swallows exceptions.
   // This handler has no body - it just jumps to the exit, effectively discarding the exception.

   if (Payload.except_clauses.empty()) {
      BCPOS handler_pc = fs->pc;  // Record handler entry PC

      // Record handler metadata: packed_filter=0 (catch-all), exception_reg=0xFF (no variable)
      fs->try_handlers.push_back(TryHandlerDesc{
         0,           // packed_filter = 0 means catch-all
         handler_pc,
         0xFF         // exception_reg = 0xFF means no exception variable
      });

      // Jump to exit (this is the "body" of the synthetic handler - just falls through to exit)
      handler_exits.push_back(this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs))));
   }

   // Patch all exits to point to after the try-except block
   exit_jmp.patch_here();
   for (ControlFlowEdge &handler_exit : handler_exits) {
      handler_exit.patch_here();
   }

   fs->try_depth = saved_try_depth; // Restore try depth

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for raise statement:
//   raise error_code
//   raise error_code, message
//   raise message
//
// Bytecode structure:
//   BC_RAISE  A=error_reg, D=msg_reg

ParserResult<IrEmitUnit> IrEmitter::emit_raise_payload(const RaisePayload &Payload, const SourceSpan &Span)
{
   FuncState *fs = &this->func_state;
   RegisterGuard register_guard(fs);

   if (Payload.rethrow) {
      if (this->handler_exceptions.empty()) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "rethrow has no active handler register", Span));
      }
      bcemit_AD(fs, BC_RETHROW, this->handler_exceptions.back(), BCReg(0));
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   if (not Payload.error_code) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "raise statement requires error code expression", Span));
   }

   auto code_result = this->emit_expression(*Payload.error_code);
   if (not code_result.ok()) return ParserResult<IrEmitUnit>::failure(code_result.error_ref());

   ExpDesc code_expr = code_result.value_ref();
   if (code_expr.is_unreachable()) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   // Snapshot a local code before evaluating a message that may mutate that local.
   if (Payload.message) expr_tonextreg(fs, &code_expr);
   else expr_toanyreg(fs, &code_expr);
   auto error_reg = BCReg(code_expr.u.s.info);
   BCReg msg_reg = BCReg(0);
   bool release_msg_reg = false;

   // Evaluate optional message expression.  If there is no explicit comma message, copy the first expression to a
   // separate message slot so that `raise "message"` defaults the code to ERR::Exception and keeps the custom text.

   if (Payload.message) {
      auto msg_result = this->emit_expression(*Payload.message);
      if (not msg_result.ok()) {
         expr_free(fs, &code_expr);
         return ParserResult<IrEmitUnit>::failure(msg_result.error_ref());
      }
      ExpDesc msg_expr = msg_result.value_ref();
      if (msg_expr.is_unreachable()) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
      expr_toanyreg(fs, &msg_expr);
      msg_reg = BCReg(msg_expr.u.s.info);
      expr_free(fs, &msg_expr);
   }
   else {
      msg_reg = BCReg(fs->freereg);
      bcreg_reserve(fs, 1);
      release_msg_reg = true;
      bcemit_AD(fs, BC_MOV, msg_reg, error_reg);
   }

   // Payload evaluation must not replace the source location of the raise.
   this->lex_state.lastline = Span.line;

   // Emit BC_RAISE: A=error_reg, D=msg_reg
   bcemit_AD(fs, BC_RAISE, error_reg, msg_reg);
   if (release_msg_reg) fs->freereg--;
   expr_free(fs, &code_expr);

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for check statement: check expression
//
// Bytecode structure:
//   BC_CHECK  A=error_reg, D=source_column

ParserResult<IrEmitUnit> IrEmitter::emit_check_stmt(const CheckStmtPayload &Payload, const SourceSpan &Span)
{
   FuncState *fs = &this->func_state;

   if (not Payload.error_code) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "check statement requires error code expression", Span));
   }

   // Native calls must retain their call-specific diagnostic. A transient checkall scope lets supported direct native
   // boundaries promote before BC_CHECK, while ordinary expressions still fall through to the explicit check opcode.

   BCReg checkall_base = BCReg(fs->freereg);
   bcemit_AD(fs, BC_CHECKALLENTER, checkall_base, BCReg(0));
   fs->runtime_scopes.push_back(RuntimeScope{ RuntimeScopeKind::Checkall, checkall_base });

   auto code_result = this->emit_expression(*Payload.error_code);
   if (not code_result.ok()) return ParserResult<IrEmitUnit>::failure(code_result.error_ref());

   ExpDesc code_expr = code_result.value_ref();
   expr_toanyreg(fs, &code_expr);
   bcemit_AD(fs, BC_CHECKALLLEAVE, checkall_base, BCReg(0));
   fs->runtime_scopes.pop_back();

   // Preserve the expression column for assert-style error formatting.  BC_CHECK's D operand is otherwise unused.

   int32_t raw_column = Payload.error_code->span.column.lineNumber();
   BCReg source_column = BCReg(raw_column > int32_t(BCMAX_D) ? BCMAX_D : raw_column);
   bcemit_AD(fs, BC_CHECK, BCReg(code_expr.u.s.info), source_column);
   expr_free(fs, &code_expr);

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
