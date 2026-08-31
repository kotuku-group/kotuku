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

   // Emit try body inline (not in closure!).  Nested try blocks will add their handlers to try_handlers during this phase.
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

   for (const auto &clause : Payload.except_clauses) {
      // Pack filter codes inline (up to 4 16-bit error codes into 64-bit integer)
      uint64_t packed_filter = 0;
      if (not clause.filter_codes.empty()) {
         int shift = 0;
         for (const auto &code_expr : clause.filter_codes) {
            if (not code_expr or shift >= 64) break;

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

      // Determine exception register

      BCREG exception_reg = 0xFF;  // No exception variable by default
      BCReg saved_freereg = BCReg(fs->freereg);
      BCPOS handler_pc = fs->pc;  // Record handler entry PC BEFORE emitting any handler code

      // If there's an exception variable, allocate a register for it.
      // The runtime will place the exception table in this register

      if (clause.exception_var.has_value() and clause.exception_var->symbol) {
         BCREG saved_nactvar = fs->varmap.size();  // Save varmap.size() before adding the exception variable

         // Reserve register space first, then create the variable var_new takes an offset from varmap.size(), not an
         // absolute register.

         fs->freereg++;
         this->lex_state.var_new(BCReg(0), clause.exception_var->symbol, clause.exception_var->span.line, clause.exception_var->span.column);
         this->lex_state.var_add(BCReg(1));

         exception_reg = saved_nactvar; // The exception register is the slot we just added
         fs->var_get(exception_reg).binding_id = clause.exception_var->binding_id;

         // Update local binding so the variable can be referenced
         this->update_local_binding(clause.exception_var->symbol, BCReg(exception_reg));

         // Emit handler body
         if (clause.block) {
            auto handler_result = this->emit_block(*clause.block, FuncScopeFlag::None);
            if (not handler_result.ok()) {
               fs->try_depth = saved_try_depth;
               return handler_result;
            }
         }

         // Clean up: remove the exception variable and restore freereg
         this->lex_state.var_remove(saved_nactvar);
         fs->freereg = saved_freereg.raw();
      }
      else {
         // No exception variable - emit handler body without variable binding
         if (clause.block) {
            auto handler_result = this->emit_block(*clause.block, FuncScopeFlag::None);
            if (not handler_result.ok()) {
               fs->try_depth = saved_try_depth;
               return handler_result;
            }
         }
         fs->freereg = saved_freereg.raw();
      }

      // Record handler metadata
      fs->try_handlers.push_back(TryHandlerDesc{
         packed_filter,
         handler_pc,
         exception_reg
      });

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

ParserResult<IrEmitUnit> IrEmitter::emit_raise_stmt(const RaiseStmtPayload &Payload, const SourceSpan &Span)
{
   FuncState *fs = &this->func_state;

   if (not Payload.error_code) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "raise statement requires error code expression", Span));
   }

   auto code_result = this->emit_expression(*Payload.error_code);
   if (not code_result.ok()) return ParserResult<IrEmitUnit>::failure(code_result.error_ref());

   ExpDesc code_expr = code_result.value_ref();
   expr_toanyreg(fs, &code_expr);
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
      expr_toanyreg(fs, &msg_expr);
      msg_reg = BCReg(msg_expr.u.s.info);
      expr_free(fs, &msg_expr);
   }
   else {
      msg_reg = BCReg(fs->freereg++);
      release_msg_reg = true;
      bcemit_AD(fs, BC_MOV, msg_reg, error_reg);
   }

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

//********************************************************************************************************************
// Emit bytecode for one import entry.
//
// The import entry inlines the content of the referenced file at compile time.
// The inlined_body block is emitted as if its statements were written directly at the import location.
// This creates a new scope for the imported content to provide some isolation.
//
// When namespace_name is set (from 'as alias' or module's default namespace), emits:
//   local <namespace_name> <const> = _LIB['<default_namespace>']

void IrEmitter::emit_namespace_registry_load(std::string_view Name, BCReg Destination)
{
   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   auto str_const = [fs](GCstr *String) -> BCREG {
      return const_gc(fs, obj2gco(String), LJ_TSTR);
   };

   bcemit_AD(fs, BC_GGET, Destination.raw(), str_const(lj_str_newlit(L, "_LIB")));
   GCstr *namespace_name = lj_str_new(L, Name.data(), Name.size());
   bcemit_tgets(fs, Destination.raw(), Destination.raw(), str_const(namespace_name));
}

void IrEmitter::emit_namespace_missing_guard(std::string_view Name, BCReg Value)
{
   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   ExpDesc nil_value(ExpKind::Nil);
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, Value.raw(), const_pri(&nil_value)));
   ControlFlowEdge missing = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
   ControlFlowEdge present = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

   missing.patch_here();
   BCReg saved_freereg = fs->free_reg();
   BCReg call_base = saved_freereg;
   bcreg_reserve(fs, BCReg(2 + LJ_FR2));
   auto str_const = [fs](GCstr *String) -> BCREG {
      return const_gc(fs, obj2gco(String), LJ_TSTR);
   };
   bcemit_AD(fs, BC_GGET, call_base.raw(), str_const(lj_str_newlit(L, "error")));
   std::string message = std::format("Cannot join namespace '{}': registry entry does not exist", Name);
   bcemit_AD(fs, BC_KSTR, call_base.raw() + 1 + LJ_FR2,
      str_const(lj_str_new(L, message.c_str(), message.size())));
   bcemit_ABC(fs, BC_CALL, call_base.raw(), 2, 2);
   fs->freereg = saved_freereg.raw();
   present.patch_here();
}

void IrEmitter::publish_namespace_local(const Identifier &Name, BCReg Slot)
{
   FuncState *fs = &this->func_state;
   this->lex_state.var_new(BCReg(0), Name.symbol, Name.span.line, Name.span.column);
   this->lex_state.var_add(BCReg(1));

   VarInfo *info = &fs->var_get(fs->varmap.size() - 1);
   info->info |= VarInfoFlag::Const;
   info->binding_id = Name.binding_id;
   info->static_value = Name.static_value;
   if (Name.binding_id) {
      const auto &binding = this->ctx.descriptors().binding(Name.binding_id);
      info->static_callable = binding.callable;
      if (binding.callable) info->static_results = this->ctx.descriptors().callable(binding.callable).results;
      this->apply_analysed_local_type(Slot, Name.binding_id);
      this->assert_analysed_local_type(Slot, Name.binding_id);
   }

   this->update_local_binding(Name.symbol, Slot);
   fs->reset_freereg();
}

ParserResult<IrEmitUnit> IrEmitter::emit_import_entry(const ImportEntryPayload &Entry)
{
   FuncState *fs = &this->func_state;

   // If there's a body, emit the inlined content first
   if (Entry.inlined_body) {
      // Temporarily switch to the imported file's FileSource index
      // so that prototypes created for functions in the import get the correct file_source_idx
      uint8_t saved_file_index = this->lex_state.current_file_index;
      this->lex_state.current_file_index = Entry.file_source_idx;

      auto result = this->emit_block(*Entry.inlined_body, FuncScopeFlag::None);

      // Restore the parent file's index
      this->lex_state.current_file_index = saved_file_index;

      if (not result.ok()) return result;
   }

   // If namespace_name is set, emit: local <name> <const> = _LIB['<default_namespace>']
   if (Entry.namespace_name) {
      const Identifier &ns_id = *Entry.namespace_name;
      const std::string &default_ns = Entry.default_namespace;

      if (default_ns.empty()) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant,
            "import namespace_name is set but default_namespace is empty"));
      }

      // Allocate a register for the namespace local variable
      BCReg dest = BCReg(fs->freereg);
      bcreg_reserve(fs, BCReg(1));
      this->emit_namespace_registry_load(default_ns, dest);
      this->publish_namespace_local(ns_id, dest);
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit a namespace declaration.  Creation publishes its const local before evaluating the literal so closures can
// capture their owning namespace, then stores the completed value in the registry.  A join loads the registry value
// and rejects a missing entry before publishing the local.  Joins following an import already have the correct
// binding and only validate that its registry value is present.

ParserResult<IrEmitUnit> IrEmitter::emit_namespace_stmt(const NamespaceStmtPayload &Payload)
{
   std::string_view namespace_name(strdata(Payload.name.symbol), Payload.name.symbol->len);
   if (Payload.reuses_import_binding) {
      std::optional<BCReg> existing = this->resolve_local(Payload.name.symbol);
      if (not existing) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "Reused namespace import binding is unavailable"));
      }
      this->emit_namespace_missing_guard(namespace_name, *existing);
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   BCReg value_reg = fs->free_reg();

   if (Payload.mode IS NamespaceDeclarationMode::Create) {
      if (not Payload.initialiser) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "Creating namespace has no initialiser"));
      }

      bcreg_reserve(fs, BCReg(1));
      this->publish_namespace_local(Payload.name, value_reg);

      auto emitted = this->emit_expression(*Payload.initialiser);
      if (not emitted.ok()) return ParserResult<IrEmitUnit>::failure(emitted.error_ref());
      ExpDesc value = emitted.value_ref();
      this->materialise_to_reg(value, value_reg, "namespace initialiser");
      fs->reset_freereg();

      BCReg registry_reg = fs->free_reg();
      bcreg_reserve(fs, BCReg(1));
      auto str_const = [fs](GCstr *String) -> BCREG {
         return const_gc(fs, obj2gco(String), LJ_TSTR);
      };
      bcemit_AD(fs, BC_GGET, registry_reg.raw(), str_const(lj_str_newlit(L, "_LIB")));
      bcemit_tsets(fs, value_reg.raw(), registry_reg.raw(), str_const(Payload.name.symbol));
      fs->reset_freereg();
   }
   else {
      bcreg_reserve(fs, BCReg(1));
      this->emit_namespace_registry_load(namespace_name, value_reg);
      this->emit_namespace_missing_guard(namespace_name, value_reg);
      this->publish_namespace_local(Payload.name, value_reg);
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for import statement: import 'path' [, 'path'...]

ParserResult<IrEmitUnit> IrEmitter::emit_import_stmt(const ImportStmtPayload &Payload)
{
   for (const ImportEntryPayload &entry : Payload.entries) {
      auto result = this->emit_import_entry(entry);
      if (not result.ok()) return result;
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
