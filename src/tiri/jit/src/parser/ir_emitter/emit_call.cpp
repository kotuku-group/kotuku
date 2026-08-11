// Copyright (C) 2025-2026 Paul Manias
// IR emitter implementation: call expression emission
//
// #included from ir_emitter.cpp

constexpr auto HASH_ASSERT  = kt::strhash("assert");
constexpr auto HASH_MSG     = kt::strhash("msg");

// Known C library interface hashes - warnings for missing prototypes only apply to these

static constexpr uint32_t KNOWN_C_INTERFACES[] = {
   kt::strhash("obj"),
   kt::strhash("string"),
   kt::strhash("math"),
   kt::strhash("table"),
   kt::strhash("bit"),
   kt::strhash("jit"),
   kt::strhash("debug"),
   kt::strhash("array"),
   kt::strhash("range")
};

static bool is_known_c_interface(uint32_t Hash) {
   for (auto h : KNOWN_C_INTERFACES) {
      if (h IS Hash) return true;
   }
   return false;
}

// Check if a name in the global table is a C function

static bool is_global_cfunction(lua_State *L, CSTRING Name)
{
   lua_getglobal(L, Name);
   bool is_cfunc = lua_iscfunction(L, -1) != 0;
   lua_pop(L, 1);
   return is_cfunc;
}

// Return the receiver selected by static built-in method classification.  Only direct named dot calls can acquire the
// annotation, so reaching emission with any other target is an internal consistency failure.

static const ExprNode * builtin_method_receiver(const CallExprPayload &Payload)
{
   const auto *direct = std::get_if<DirectCallTarget>(&Payload.target);
   if (not direct or not direct->callable) return nullptr;
   if (direct->callable->kind IS AstNodeKind::MemberExpr) {
      return std::get<MemberExprPayload>(direct->callable->data).table.get();
   }
   if (direct->callable->kind IS AstNodeKind::SafeMemberExpr) {
      return std::get<SafeMemberExprPayload>(direct->callable->data).table.get();
   }
   return nullptr;
}

static BCReg prepare_builtin_method_frame(
   FuncState *State, BuiltinCallableID Callable, BCReg Receiver, BCReg CallBase)
{
   RegisterAllocator allocator(State);
   BCREG required_top = CallBase.raw() + 2 + LJ_FR2;
   if (State->freereg < required_top) allocator.reserve(BCReg(required_top - State->freereg));
   bcemit_AD(State, BC_MOV, CallBase.raw() + 1 + LJ_FR2, Receiver.raw());
   bcemit_builtin_callable(State, Callable, CallBase.raw());
   return CallBase;
}

//********************************************************************************************************************
// Emit an unresolved built-in method used as a pipe destination.  The selected branch owns the pipe operand and the
// written arguments, so their expressions execute once with the call-frame layout selected at runtime.

ParserResult<ExpDesc> IrEmitter::emit_runtime_builtin_method_pipe(
   const PipeExprPayload &Payload, const CallExprPayload &Call)
{
   if (not Call.runtime_builtin_method or not Call.runtime_builtin_method->member) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "invalid piped runtime built-in method annotation"));
   }

   const ExprNode *receiver_node = builtin_method_receiver(Call);
   if (not receiver_node) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "piped runtime built-in method annotation has no direct receiver"));
   }

   FuncState *state = &this->func_state;
   BCLine call_line = this->lex_state.lastline;
   BCReg call_base = state->free_reg();
   auto receiver_result = this->emit_expression(*receiver_node);
   if (not receiver_result.ok()) return receiver_result;

   std::unique_ptr<NilShortCircuitGuard> nil_guard;
   ExpDesc receiver = receiver_result.value_ref();
   RegisterAllocator allocator(state);
   BCReg receiver_reg(0);
   if (Call.runtime_builtin_method->safe) {
      nil_guard = std::make_unique<NilShortCircuitGuard>(this, receiver);
      if (not nil_guard->ok()) return nil_guard->error<ExpDesc>();
      receiver = nil_guard->base_expression();
      receiver_reg = BCReg(receiver.u.s.info);
   }
   else {
      ExpressionValue receiver_value(state, receiver);
      receiver_reg = receiver_value.discharge_to_any_reg(allocator);
   }

   BCREG receiver_slot = call_base.raw() + 1 + LJ_FR2;
   BCREG required_top = receiver_slot + 1;
   if (state->freereg < required_top) allocator.reserve(BCReg(required_top - state->freereg));
   bcemit_AD(state, BC_MOV, receiver_slot, receiver_reg.raw());

   BCREG member_constant = const_gc(
      state, obj2gco(Call.runtime_builtin_method->member), LJ_TSTR);
   BCPos dispatch_pc = BCPos(bcemit_INS(
      state, BCINS_AJP(BC_BMETH, call_base.raw(), NO_JMP, member_constant)));

   auto emit_branch = [&](bool BuiltinBranch) -> ParserResult<BCPos> {
      state->freereg = call_base.raw() + 1 + LJ_FR2 + (BuiltinBranch ? 1 : 0);
      auto lhs_result = this->emit_expression(*Payload.lhs);
      if (not lhs_result.ok()) return ParserResult<BCPos>::failure(lhs_result.error_ref());
      ExpDesc lhs = lhs_result.value_ref();
      bool forward_multret = false;
      if (lhs.k IS ExpKind::Call) {
         if (Payload.limit > 0) {
            set_call_result_count(state, lhs, Payload.limit + 1);
            state->freereg = lhs.u.s.aux + Payload.limit;
         }
         else {
            set_call_result_count(state, lhs, 0);
            forward_multret = true;
         }
      }
      else this->materialise_to_next_reg(lhs, "runtime method pipe operand");

      BCReg argument_count(0);
      ExpDesc arguments(ExpKind::Void);
      if (not Call.arguments.empty()) {
         auto arguments_result = this->emit_expression_list(Call.arguments, argument_count);
         if (not arguments_result.ok()) return ParserResult<BCPos>::failure(arguments_result.error_ref());
         arguments = arguments_result.value_ref();
      }

      BCIns instruction;
      if (forward_multret and Call.arguments.empty()) {
         instruction = BCINS_ABC(BC_CALLM, call_base.raw(), 2,
            lhs.u.s.aux - call_base.raw() - 1 - LJ_FR2);
      }
      else {
         if (arguments.k != ExpKind::Void) {
            this->materialise_to_next_reg(arguments, "runtime method pipe arguments");
         }
         instruction = BCINS_ABC(BC_CALL, call_base.raw(), 2,
            state->freereg - call_base.raw() - 1);
      }
      this->lex_state.lastline = call_line;
      return ParserResult<BCPos>::success(BCPos(bcemit_INS(state, instruction)));
   };

   auto builtin_result = emit_branch(true);
   if (not builtin_result.ok()) return ParserResult<ExpDesc>::failure(builtin_result.error_ref());
   BCPos builtin_call = builtin_result.value_ref();
   ControlFlowEdge skip_field = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));

   bcemit_AD(state, BC_MOV, call_base.raw(), call_base.raw());
   this->control_flow.make_unconditional(dispatch_pc).patch_to(state->current_pc());

   // BC_BMETH leaves the ordinary field value in call_base on its fallback path.  A safe destination must also
   // short-circuit when that field is nil, not just when the receiver is nil.  Emit this check before emit_branch()
   // so neither the pipe operand nor its written arguments are evaluated for a missing method.
   ControlFlowEdge field_nil_jump;
   if (Call.runtime_builtin_method->safe) {
      ExpDesc nil_value(ExpKind::Nil);
      bcemit_INS(state, BCINS_AD(BC_ISEQP, call_base, const_pri(&nil_value)));
      field_nil_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));
   }

   auto field_result = emit_branch(false);
   if (not field_result.ok()) return ParserResult<ExpDesc>::failure(field_result.error_ref());
   BCPos field_call = field_result.value_ref();
   skip_field.patch_here();

   if (Call.runtime_builtin_method->safe) {
      // Successful built-in and field calls converge above and must preserve their result.  Only field_nil_jump enters
      // the synthetic nil-result block; skip_nil carries both successful paths around it to the common continuation.
      ControlFlowEdge skip_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));
      field_nil_jump.patch_to(state->current_pc());
      bcemit_nil(state, call_base.raw(), 1);
      skip_nil.patch_here();
   }

   ParserResult<ExpDesc> emitted = nil_guard ? nil_guard->complete_call(call_base, field_call) :
      ParserResult<ExpDesc>::success(ExpDesc(ExpKind::Call, field_call.raw()));
   if (not emitted.ok()) return emitted;
   ExpDesc result = emitted.value_ref();
   result.alternate_call = builtin_call.raw();
   result.u.s.aux = call_base.raw();
   result.static_results = Call.results;
   if (Call.results) {
      const auto &descriptor = this->ctx.descriptors().results(Call.results).value_at(0);
      result.result_type = descriptor.primary;
      result.object_class_id = descriptor.object_class_id;
      result.struct_def = descriptor.struct_def;
   }
   state->freereg = call_base.raw() + 1;
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Pipe expression: lhs |> rhs_call()
// Prepends the LHS result(s) as argument(s) to the RHS function call.  The RHS must be a CallExpr node.
//
// Register layout for calls:
//   R(base)   = function
//   R(base+1) = frame link (FR2, 64-bit mode)
//   R(base+2) = first argument (LHS piped value)
//   R(base+3...) = remaining arguments from RHS call

ParserResult<ExpDesc> IrEmitter::emit_pipe_expr(const PipeExprPayload &Payload)
{
   if (not Payload.lhs or not Payload.rhs_call) return this->unsupported_expr(AstNodeKind::PipeExpr, SourceSpan{});

   // Deferred iteration: the parser couldn't determine the LHS type at AST time.
   // Now that we can resolve variable types, select the canonical array/range each() callable.

   if (Payload.deferred_iteration) {
      BCReg call_base = this->func_state.free_reg();
      auto lhs_result = this->emit_expression(*Payload.lhs);
      if (not lhs_result.ok()) return lhs_result;
      ExpDesc callee = lhs_result.value_ref();

      const fprototype *prototype = get_method_prototype(callee.result_type, "each");
      if (not prototype or (callee.result_type != TiriType::Array and callee.result_type != TiriType::Range)) {
         return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::UnexpectedToken,
            "pipe operator requires function call on right-hand side (or an array/range on the left)",
            Payload.lhs->span));
      }

      this->materialise_to_next_reg(callee, "deferred pipe array receiver");
      BCReg receiver_reg = BCReg(callee.u.s.info);
      prepare_builtin_method_frame(
         &this->func_state, prototype->builtin_callable_id, receiver_reg, call_base);

      // Emit the function argument

      auto rhs_result = this->emit_expression(*Payload.rhs_call);
      if (not rhs_result.ok()) return rhs_result;
      ExpDesc rhs = rhs_result.value_ref();
      this->materialise_to_next_reg(rhs, "deferred pipe iteration callback");

      BCIns ins = BCINS_ABC(BC_CALL, call_base, 2, this->func_state.freereg - call_base - 1);
      BCLine call_line = this->lex_state.lastline;
      this->lex_state.lastline = call_line;

      ExpDesc result;
      result.init(ExpKind::Call, bcemit_INS(&this->func_state, ins));
      result.u.s.aux = call_base;
      result.result_type = callee.result_type;
      this->func_state.freereg = call_base + 1;
      return ParserResult<ExpDesc>::success(result);
   }

   // The RHS must be a call expression - this was validated in the parser
   if (Payload.rhs_call->kind != AstNodeKind::CallExpr and Payload.rhs_call->kind != AstNodeKind::SafeCallExpr) {
      return this->unsupported_expr(AstNodeKind::PipeExpr, Payload.rhs_call->span);
   }

   BCLine call_line = this->lex_state.lastline;
   FuncState *fs = &this->func_state;

   const CallExprPayload &call_payload = std::get<CallExprPayload>(Payload.rhs_call->data);
   if (call_payload.runtime_builtin_method) {
      return this->emit_runtime_builtin_method_pipe(Payload, call_payload);
   }

   // Emit the callee (function) FIRST to establish base register

   ExpDesc callee;
   BCReg base(0);
   std::unique_ptr<NilShortCircuitGuard> nil_guard;
   if (call_payload.builtin_method) {
      base = fs->free_reg();
      const ExprNode *receiver_node = builtin_method_receiver(call_payload);
      if (not receiver_node or not builtin_callable_valid(call_payload.builtin_method->callable)) {
         return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
            "invalid piped built-in method annotation"));
      }

      auto receiver_result = this->emit_expression(*receiver_node);
      if (not receiver_result.ok()) return receiver_result;
      ExpDesc receiver = receiver_result.value_ref();
      RegisterAllocator allocator(fs);
      BCReg receiver_reg(0);
      if (call_payload.builtin_method->safe) {
         nil_guard = std::make_unique<NilShortCircuitGuard>(this, receiver);
         if (not nil_guard->ok()) return nil_guard->error<ExpDesc>();
         receiver = nil_guard->base_expression();
         receiver_reg = BCReg(receiver.u.s.info);
      }
      else {
         ExpressionValue receiver_value(fs, receiver);
         receiver_reg = receiver_value.discharge_to_any_reg(allocator);
      }

      prepare_builtin_method_frame(fs, call_payload.builtin_method->callable, receiver_reg, base);
   }
   else if (const auto* direct = std::get_if<DirectCallTarget>(&call_payload.target)) {
      if (not direct->callable) return this->unsupported_expr(AstNodeKind::PipeExpr, SourceSpan{});
      auto callee_result = this->emit_expression(*direct->callable);
      if (not callee_result.ok()) return callee_result;
      callee = callee_result.value_ref();
      this->materialise_to_next_reg(callee, "pipe call callee");
      RegisterAllocator allocator(fs);
      allocator.reserve(BCReg(1)); // Frame link (FR2)
      base = BCReg(callee.u.s.info);
   }
   else return this->unsupported_expr(AstNodeKind::PipeExpr, SourceSpan{});

   // Emit LHS expression as the first argument(s)

   auto lhs_result = this->emit_expression(*Payload.lhs);
   if (not lhs_result.ok()) return lhs_result;
   ExpDesc lhs = lhs_result.value_ref();

   // Determine if LHS is a multi-value expression (function call)

   bool lhs_is_call = lhs.k IS ExpKind::Call;
   bool forward_multret = false;

   if (lhs_is_call) {
      // Multi-value case: discharge the call result to registers
      // If limit > 0, we want exactly 'limit' results
      // If limit == 0, we want all results (multi-return)

      if (Payload.limit > 0) {
         // Set BC_CALL B field to request exactly 'limit' return values
         // B = limit + 1 means "expect limit results"

         set_call_result_count(fs, lhs, Payload.limit + 1);

         // The call results are placed starting at lhs.u.s.aux (the call base)
         // Update freereg to reflect the limited number of results

         fs->freereg = lhs.u.s.aux + Payload.limit;
      }
      else { // Forward all return values - keep B=0 for CALLM pattern
         set_call_result_count(fs, lhs, 0);
         forward_multret = true;
      }
   }
   else { // Single value case - materialize to next register
      this->materialise_to_next_reg(lhs, "pipe LHS value");
   }

   // Emit remaining RHS arguments

   BCReg arg_count(0);
   ExpDesc args(ExpKind::Void);
   if (not call_payload.arguments.empty()) {
      auto args_result = this->emit_expression_list(call_payload.arguments, arg_count);
      if (not args_result.ok()) return ParserResult<ExpDesc>::failure(args_result.error_ref());
      args = args_result.value_ref();
   }

   // Emit the call instruction

   BCIns ins;
   if (forward_multret and call_payload.arguments.empty()) {
      // Use CALLM to forward all LHS return values as arguments (no additional args)
      // C field = number of fixed args before the vararg (0 in this case)
      ins = BCINS_ABC(BC_CALLM, base, 2, lhs.u.s.aux - base - 1 - 1);
   }
   else { // Regular CALL with fixed argument count
      if (args.k != ExpKind::Void) this->materialise_to_next_reg(args, "pipe rhs arguments");
      ins = BCINS_ABC(BC_CALL, base, 2, fs->freereg - base - 1);
   }

   this->lex_state.lastline = call_line;

   BCPos call_pc = BCPos(bcemit_INS(fs, ins));
   ExpDesc result;
   if (nil_guard) {
      auto completed = nil_guard->complete_call(base, call_pc);
      if (not completed.ok()) return completed;
      result = completed.value_ref();
   }
   else result.init(ExpKind::Call, call_pc);
   result.u.s.aux = base;
   result.static_results = call_payload.results;
   if (call_payload.results) {
      const auto &descriptor = this->ctx.descriptors().results(call_payload.results).value_at(0);
      result.result_type = descriptor.primary;
      result.object_class_id = descriptor.object_class_id;
      result.struct_def = descriptor.struct_def;
   }
   fs->freereg = base + 1;
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit a statically resolved built-in dot method.  The receiver is evaluated once before callable selection, copied
// into native argument zero and followed by the written arguments in source order.

ParserResult<ExpDesc> IrEmitter::emit_builtin_method_call(const CallExprPayload &Payload)
{
   if (not Payload.builtin_method or not builtin_callable_valid(Payload.builtin_method->callable)) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "invalid built-in method annotation"));
   }

   const ExprNode *receiver_node = builtin_method_receiver(Payload);
   if (not receiver_node) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "built-in method annotation has no direct receiver"));
   }

   BCLine call_line = this->lex_state.lastline;
   BCReg call_base = this->func_state.free_reg();
   auto receiver_result = this->emit_expression(*receiver_node);
   if (not receiver_result.ok()) return receiver_result;

   std::unique_ptr<NilShortCircuitGuard> nil_guard;
   ExpDesc receiver = receiver_result.value_ref();
   RegisterAllocator allocator(&this->func_state);
   BCReg receiver_reg(0);
   if (Payload.builtin_method->safe) {
      nil_guard = std::make_unique<NilShortCircuitGuard>(this, receiver);
      if (not nil_guard->ok()) return nil_guard->error<ExpDesc>();
      receiver = nil_guard->base_expression();
      receiver_reg = BCReg(receiver.u.s.info);
   }
   else {
      ExpressionValue receiver_value(&this->func_state, receiver);
      receiver_reg = receiver_value.discharge_to_any_reg(allocator);
      receiver = receiver_value.legacy();
   }

   prepare_builtin_method_frame(
      &this->func_state, Payload.builtin_method->callable, receiver_reg, call_base);

   BCReg arg_count(0);
   ExpDesc args(ExpKind::Void);
   if (not Payload.arguments.empty()) {
      auto args_result = this->emit_expression_list(Payload.arguments, arg_count);
      if (not args_result.ok()) return ParserResult<ExpDesc>::failure(args_result.error_ref());
      args = args_result.value_ref();
   }

   BCIns ins;
   bool forward_tail = Payload.forwards_multret and (args.k IS ExpKind::Call);
   if (forward_tail) {
      set_call_result_count(&this->func_state, args, 0);
      ins = BCINS_ABC(BC_CALLM, call_base, 2, args.u.s.aux - call_base - 1 - 1);
   }
   else {
      if (args.k != ExpKind::Void) this->materialise_to_next_reg(args, "built-in method arguments");
      ins = BCINS_ABC(BC_CALL, call_base, 2, this->func_state.freereg - call_base - 1);
   }

   this->lex_state.lastline = call_line;
   BCPos call_pc = BCPos(bcemit_INS(&this->func_state, ins));

   ParserResult<ExpDesc> emitted = nil_guard ? nil_guard->complete_call(call_base, call_pc) :
      ParserResult<ExpDesc>::success(ExpDesc(ExpKind::Call, call_pc));
   if (not emitted.ok()) return emitted;

   ExpDesc result = emitted.value_ref();
   result.u.s.aux = call_base;
   result.static_results = Payload.results;
   if (Payload.results) {
      const auto &descriptor = this->ctx.descriptors().results(Payload.results).value_at(0);
      result.result_type = descriptor.primary;
      result.object_class_id = descriptor.object_class_id;
      result.struct_def = descriptor.struct_def;
   }
   this->func_state.freereg = call_base + 1;
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit a runtime-resolved built-in dot method.  BC_BMETH selects either the canonical built-in call frame or an
// ordinary field-call frame.  The receiver is evaluated once; written arguments remain branch-local so each frame
// retains the same layout as its statically selected counterpart.

ParserResult<ExpDesc> IrEmitter::emit_runtime_builtin_method_call(const CallExprPayload &Payload)
{
   if (not Payload.runtime_builtin_method or not Payload.runtime_builtin_method->member) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "invalid runtime built-in method annotation"));
   }

   const ExprNode *receiver_node = builtin_method_receiver(Payload);
   if (not receiver_node) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "runtime built-in method annotation has no direct receiver"));
   }

   FuncState *state = &this->func_state;
   BCLine call_line = this->lex_state.lastline;
   BCReg call_base = state->free_reg();
   auto receiver_result = this->emit_expression(*receiver_node);
   if (not receiver_result.ok()) return receiver_result;

   std::unique_ptr<NilShortCircuitGuard> nil_guard;
   ExpDesc receiver = receiver_result.value_ref();
   RegisterAllocator allocator(state);
   BCReg receiver_reg(0);
   if (Payload.runtime_builtin_method->safe) {
      nil_guard = std::make_unique<NilShortCircuitGuard>(this, receiver);
      if (not nil_guard->ok()) return nil_guard->error<ExpDesc>();
      receiver = nil_guard->base_expression();
      receiver_reg = BCReg(receiver.u.s.info);
   }
   else {
      ExpressionValue receiver_value(state, receiver);
      receiver_reg = receiver_value.discharge_to_any_reg(allocator);
   }

   BCREG receiver_slot = call_base.raw() + 1 + LJ_FR2;
   BCREG required_top = receiver_slot + 1;
   if (state->freereg < required_top) allocator.reserve(BCReg(required_top - state->freereg));
   bcemit_AD(state, BC_MOV, receiver_slot, receiver_reg.raw());

   BCREG member_constant = const_gc(
      state, obj2gco(Payload.runtime_builtin_method->member), LJ_TSTR);
   BCPos dispatch_pc = BCPos(bcemit_INS(
      state, BCINS_AJP(BC_BMETH, call_base.raw(), NO_JMP, member_constant)));

   auto emit_branch_call = [&](bool BuiltinBranch) -> ParserResult<BCPos> {
      state->freereg = call_base.raw() + 1 + LJ_FR2 + (BuiltinBranch ? 1 : 0);
      BCReg argument_count(0);
      ExpDesc arguments(ExpKind::Void);
      if (not Payload.arguments.empty()) {
         auto arguments_result = this->emit_expression_list(Payload.arguments, argument_count);
         if (not arguments_result.ok()) return ParserResult<BCPos>::failure(arguments_result.error_ref());
         arguments = arguments_result.value_ref();
      }

      BCIns instruction;
      bool forward_tail = Payload.forwards_multret and arguments.k IS ExpKind::Call;
      if (forward_tail) {
         set_call_result_count(state, arguments, 0);
         instruction = BCINS_ABC(BC_CALLM, call_base.raw(), 2,
            arguments.u.s.aux - call_base.raw() - 1 - LJ_FR2);
      }
      else {
         if (arguments.k != ExpKind::Void) {
            this->materialise_to_next_reg(arguments,
               BuiltinBranch ? "runtime built-in method arguments" : "runtime field-call arguments");
         }
         instruction = BCINS_ABC(BC_CALL, call_base.raw(), 2,
            state->freereg - call_base.raw() - 1);
      }
      this->lex_state.lastline = call_line;
      return ParserResult<BCPos>::success(BCPos(bcemit_INS(state, instruction)));
   };

   auto builtin_call_result = emit_branch_call(true);
   if (not builtin_call_result.ok()) return ParserResult<ExpDesc>::failure(builtin_call_result.error_ref());
   BCPos builtin_call = builtin_call_result.value_ref();
   ControlFlowEdge skip_field = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));

   // The instruction immediately before the fallback target carries the call base in operand A.  This matches the
   // VM continuation convention used when ordinary member lookup invokes an __index function before arguments are
   // evaluated.
   bcemit_AD(state, BC_MOV, call_base.raw(), call_base.raw());
   BCPos field_path = state->current_pc();
   this->control_flow.make_unconditional(dispatch_pc).patch_to(field_path);

   // BC_BMETH leaves the resolved ordinary callable in call_base on its fallback path.  Safe-call semantics require a
   // missing field to produce nil without evaluating arguments, even when the receiver itself was non-nil.
   ControlFlowEdge field_nil_jump;
   if (Payload.runtime_builtin_method->safe) {
      ExpDesc nil_value(ExpKind::Nil);
      bcemit_INS(state, BCINS_AD(BC_ISEQP, call_base, const_pri(&nil_value)));
      field_nil_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));
   }

   auto field_call_result = emit_branch_call(false);
   if (not field_call_result.ok()) return ParserResult<ExpDesc>::failure(field_call_result.error_ref());
   BCPos field_call = field_call_result.value_ref();
   skip_field.patch_here();

   if (Payload.runtime_builtin_method->safe) {
      // Both successful call paths jump over the nil assignment.  The missing-field edge alone writes the safe result
      // into call_base before all paths rejoin for the existing receiver nil guard and result metadata handling.
      ControlFlowEdge skip_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(state)));
      field_nil_jump.patch_to(state->current_pc());
      bcemit_nil(state, call_base.raw(), 1);
      skip_nil.patch_here();
   }

   ParserResult<ExpDesc> emitted = nil_guard ? nil_guard->complete_call(call_base, field_call) :
      ParserResult<ExpDesc>::success(ExpDesc(ExpKind::Call, field_call.raw()));
   if (not emitted.ok()) return emitted;

   ExpDesc result = emitted.value_ref();
   result.alternate_call = builtin_call.raw();
   result.u.s.aux = call_base.raw();
   result.static_results = Payload.results;
   if (Payload.results) {
      const auto &descriptor = this->ctx.descriptors().results(Payload.results).value_at(0);
      result.result_type = descriptor.primary;
      result.object_class_id = descriptor.object_class_id;
      result.struct_def = descriptor.struct_def;
   }
   state->freereg = call_base.raw() + 1;
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a direct call expression, including statically resolved built-in dot methods.

ParserResult<ExpDesc> IrEmitter::emit_call_expr(const CallExprPayload &Payload)
{
   if (Payload.builtin_method) return this->emit_builtin_method_call(Payload);
   if (Payload.runtime_builtin_method) return this->emit_runtime_builtin_method_call(Payload);

   kt::Log log(__FUNCTION__);

   // We save lastline here before it gets overwritten by processing sub-expressions.
   BCLine call_line = this->lex_state.lastline;

   // First pass optimisations: In some cases we can optimise functions during parsing.
   // NOTE: Optimisations may cause confusion during debugging sessions, so we may want to add
   // a way to disable them if tracing/profiling is enabled.

   if (const auto *direct = std::get_if<DirectCallTarget>(&Payload.target)) {
      if (direct->callable and direct->callable->kind IS AstNodeKind::IdentifierExpr) {
         const auto *name_ref = std::get_if<NameRef>(&direct->callable->data);
         if (name_ref and name_ref->identifier.symbol) {
            GCstr *func_name = name_ref->identifier.symbol;

            if (func_name->hash IS HASH_ASSERT) this->optimise_assert(const_cast<ExprNodeList&>(Payload.arguments));
            else if ((func_name->hash IS HASH_MSG) and not glPrintMsg) {
               // msg() is eliminated entirely when debug messaging is disabled at compile time.
               return ParserResult<ExpDesc>::success(ExpDesc(ExpKind::Void));
            }
         }
      }
   }

   ExpDesc callee;
   auto base = BCReg(0);
   auto context_result_base = BCReg(0);
   bool is_safe_callable = false;
   bool is_contextual_call = false;
   bool uses_specialised_member_dispatch = false;
   ControlFlowEdge receiver_nil_jump;
   TiriType callee_return_type = TiriType::Unknown;  // First return type of callee (if known)
   CLASSID callee_object_class_id = CLASSID::NIL;  // CLASSID if return type is Object
   struct_record *callee_struct_def = nullptr;

   if (Payload.results) {
      const auto &descriptor = this->ctx.descriptors().results(Payload.results).value_at(0);
      callee_return_type = descriptor.primary;
      callee_object_class_id = descriptor.object_class_id;
      callee_struct_def = descriptor.struct_def;
   }

   // Check if the AST has pre-computed type info (e.g., from obj.new() pattern detection)
   if (callee_return_type IS TiriType::Unknown and Payload.result_type != TiriType::Unknown) {
      callee_return_type = Payload.result_type;
      callee_object_class_id = Payload.object_class_id;
      callee_struct_def = Payload.struct_def;
      if ((callee_return_type IS TiriType::Struct or callee_return_type IS TiriType::Func) and
          not callee_struct_def and not Payload.arguments.empty()) {
         const auto *literal = std::get_if<LiteralValue>(&Payload.arguments[0]->data);
         if (literal and literal->kind IS LiteralKind::String and literal->string_value) {
            std::string_view name(strdata(literal->string_value), literal->string_value->len);
            callee_struct_def = find_struct(this->lex_state.L, name);
         }
      }
   }

   if (const auto *direct = std::get_if<DirectCallTarget>(&Payload.target)) {
      if (not direct->callable) return this->unsupported_expr(AstNodeKind::CallExpr, SourceSpan{});

      // Check if the callable is a safe navigation expression (?.field or ?[index])
      // If so, we need to add a nil check on the result before calling

      is_safe_callable = (direct->callable->kind IS AstNodeKind::SafeMemberExpr) or
                         (direct->callable->kind IS AstNodeKind::SafeIndexExpr);

      const ExprNode *receiver_node = nullptr;
      const ExprNode *index_node = nullptr;
      GCstr *member_name = nullptr;

      if (const auto *member = std::get_if<MemberExprPayload>(&direct->callable->data)) {
         receiver_node = member->table.get();
         member_name = member->member.symbol;
      }
      else if (const auto *member = std::get_if<SafeMemberExprPayload>(&direct->callable->data)) {
         receiver_node = member->table.get();
         member_name = member->member.symbol;
      }
      else if (const auto *index = std::get_if<IndexExprPayload>(&direct->callable->data)) {
         receiver_node = index->table.get();
         index_node = index->index.get();
      }
      else if (const auto *index = std::get_if<SafeIndexExprPayload>(&direct->callable->data)) {
         receiver_node = index->table.get();
         index_node = index->index.get();
      }

      is_contextual_call = receiver_node != nullptr;
      bool proved_specialised_receiver = false;
      if (is_contextual_call and receiver_node->static_value) {
         const auto &descriptor = this->ctx.descriptors().value(receiver_node->static_value);
         if (descriptor.proved() and descriptor.primary != TiriType::Unknown and
             descriptor.primary != TiriType::Any and descriptor.primary != TiriType::Table) {
            is_contextual_call = false;
            proved_specialised_receiver = descriptor.primary IS TiriType::Str or
               descriptor.primary IS TiriType::Array or descriptor.primary IS TiriType::Range;
         }
      }

      if (is_contextual_call) {
         auto receiver_result = this->emit_expression(*receiver_node);
         if (not receiver_result.ok()) return receiver_result;
         ExpDesc receiver = receiver_result.value_ref();
         this->materialise_to_next_reg(receiver, "contextual call receiver");
         auto receiver_reg = BCReg(receiver.u.s.info);
         context_result_base = receiver_reg;

         if (is_safe_callable) {
            ExpDesc nil_value(ExpKind::Nil);
            bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, receiver_reg, const_pri(&nil_value)));
            receiver_nil_jump = this->control_flow.make_unconditional(
               BCPos(bcemit_jmp(&this->func_state)));
         }

         if (member_name) {
            ExpDesc key(member_name);
            auto call_base = this->func_state.free_reg();
            bcreg_reserve(&this->func_state, 1);
            bcemit_tgets(
               &this->func_state, call_base.raw(), receiver_reg.raw(), const_str(&this->func_state, &key));
            callee.init(ExpKind::NonReloc, call_base.raw());
         }
         else {
            if (not index_node) return this->unsupported_expr(AstNodeKind::CallExpr, direct->callable->span);
            auto retained_receiver_reg = this->func_state.free_reg();
            bcreg_reserve(&this->func_state, 1);
            bcemit_AD(&this->func_state, BC_MOV, retained_receiver_reg, receiver_reg);

            auto key_result = this->emit_expression(*index_node);
            if (not key_result.ok()) return key_result;
            ExpDesc key = key_result.value_ref();
            ExpressionValue key_value(&this->func_state, key);
            key_value.to_val();
            key = key_value.legacy();
            ExpDesc lookup(ExpKind::NonReloc, receiver_reg.raw());
            lookup.static_value = receiver.static_value;
            expr_index(&this->func_state, &lookup, &key);
            this->materialise_to_next_reg(lookup, "contextual call callee");
            callee = lookup;
         }
      }
      else {
         auto callee_result = this->emit_expression(*direct->callable);
         if (not callee_result.ok()) return callee_result;
         callee = callee_result.value_ref();
      }

      // TEMPORARY: If the callee is IndexedObject or IndexedStruct, downgrade to Indexed.
      // Currently object methods and struct helpers are resolved via metamethods (__index), so we need
      // the standard TGETS path which dispatches through lj_meta_tget.
      // This will require removal or modification when OBCALL is implemented.

      if (callee.k IS ExpKind::IndexedObject or callee.k IS ExpKind::IndexedStruct) callee.k = ExpKind::Indexed;

      // If callee is a local variable, check if it has known return types

      if (callee.k IS ExpKind::Local) {
         VarInfo* vinfo = &this->lex_state.vstack[callee.u.s.aux];
         callee_return_type = vinfo->result_types[0];
         if (vinfo->struct_def) {
            callee_return_type = TiriType::Struct;
            callee_struct_def = vinfo->struct_def;
         }
      }

      // Prototype registry lookup for global/interface calls

      if (callee_return_type IS TiriType::Unknown) {
         if (direct->callable and direct->callable->kind IS AstNodeKind::IdentifierExpr) {
            // Global C registered function: func(args)
            const auto *name_ref = std::get_if<NameRef>(&direct->callable->data);
            if (name_ref and name_ref->identifier.symbol) {
               if (auto proto = get_func_prototype_by_hash(name_ref->identifier.symbol->hash)) {
                  callee_return_type = proto->first_result();
               }
               else if (is_global_cfunction(lex_state.L, strdata(name_ref->identifier.symbol))) {
                  log.warning("No prototype registered for function: %s", strdata(name_ref->identifier.symbol));
               }
            }
         }
         else if (direct->callable and direct->callable->kind IS AstNodeKind::MemberExpr) {
            // Interface method: iface.method(args)
            const auto *member_payload = std::get_if<MemberExprPayload>(&direct->callable->data);
            if (member_payload and member_payload->table and member_payload->member.symbol) {
               if (member_payload->table->kind IS AstNodeKind::IdentifierExpr) {
                  const auto *iface = std::get_if<NameRef>(&member_payload->table->data);
                  if (iface and iface->identifier.symbol) {
                     if (auto proto = get_prototype_by_hash(iface->identifier.symbol->hash,
                           member_payload->member.symbol->hash)) {
                        callee_return_type = proto->first_result();
                     }
                     else if (is_known_c_interface(iface->identifier.symbol->hash)) {
                        log.warning("No prototype registered for method: %s.%s",
                           strdata(iface->identifier.symbol), strdata(member_payload->member.symbol));
                     }
                  }
               }
            }
         }
      }

      if (not uses_specialised_member_dispatch) {
         if (not is_contextual_call) this->materialise_to_next_reg(callee, "call callee");
         // Reserve register for frame link
         RegisterAllocator allocator(&this->func_state);
         allocator.reserve(BCReg(1));
      }
      base = BCReg(callee.u.s.info);
   }
   else return this->unsupported_expr(AstNodeKind::CallExpr, SourceSpan{});

   // For safe callable expressions (obj?.method()), emit a nil check on the callable.
   // If the callable is nil, skip the call (including argument evaluation) and return nil instead.

   ControlFlowEdge nil_jump;
   if (is_safe_callable) {
      ExpDesc nilv(ExpKind::Nil);
      bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, base, const_pri(&nilv)));
      nil_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));
   }

   if (is_contextual_call) bcemit_INS(&this->func_state, BCINS_AD(BC_CTXENTER, base, 0));

   // Evaluate arguments only after the nil check, so if callable is nil we skip argument evaluation
   auto arg_count = BCReg(0);
   ExpDesc args(ExpKind::Void);
   if (not Payload.arguments.empty()) {
      auto args_result = this->emit_expression_list(Payload.arguments, arg_count);
      if (not args_result.ok()) return ParserResult<ExpDesc>::failure(args_result.error_ref());
      args = args_result.value_ref();
   }

   BCIns ins;
   bool forward_tail = Payload.forwards_multret and (args.k IS ExpKind::Call);
   if (forward_tail) {
      set_call_result_count(&this->func_state, args, 0);
      ins = BCINS_ABC(is_contextual_call ? BC_CTXCALLM : BC_CALLM,
         base, 2, args.u.s.aux - base - 1  - 1);
   }
   else {
      if (not (args.k IS ExpKind::Void)) this->materialise_to_next_reg(args, "call arguments");
      ins = BCINS_ABC(is_contextual_call ? BC_CTXCALL : BC_CALL,
         base, 2, this->func_state.freereg - base - 1);
   }

   // Restore the saved line number so the CALL instruction gets the correct line

   this->lex_state.lastline = call_line;

   auto call_pc = BCPos(bcemit_INS(&this->func_state, ins));
   if (is_contextual_call) {
      bcemit_INS(&this->func_state,
         BCINS_AD(BC_CTXLEAVE, base, BCREG(base.raw() - context_result_base.raw())));
   }

   // For safe callable: emit the nil path and patch jumps

   if (is_safe_callable) {
      ControlFlowEdge skip_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

      BCPos nil_path = BCPos(this->func_state.pc);
      nil_jump.patch_to(nil_path);
      if (not receiver_nil_jump.empty()) receiver_nil_jump.patch_to(nil_path);
      bcemit_nil(&this->func_state, is_contextual_call ? context_result_base.raw() : base.raw(), 1);

      skip_nil.patch_to(BCPos(this->func_state.pc));
   }

   ExpDesc result;
   result.init(ExpKind::Call, call_pc);
   result.u.s.aux = is_contextual_call ? context_result_base.raw() : base.raw();
   result.result_type = callee_return_type;  // Propagate known return type
   result.object_class_id = callee_object_class_id;  // Propagate object class ID for Object types
   result.struct_def = callee_struct_def;
   result.static_results = Payload.results;
   this->func_state.freereg = is_contextual_call ? base : base + 1;
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Optimise assert(condition, message) expressions by:
// 1. Wrapping expensive message expressions in an anonymous thunk for lazy evaluation
// 2. Appending line/column as additional arguments for runtime formatting
//
// Transforms: assert(cond, msg)            -> assert(cond, msg, line, col)
// Transforms: assert(cond, expensive())    -> assert(cond, (thunk():str return expensive() end)(), line, col)

void IrEmitter::optimise_assert(ExprNodeList &Args)
{
   // Requires at least 2 arguments: condition (Args[0]) and message (Args[1])
   if (Args.size() < 2) return;

   ExprNodePtr &msg_arg = Args[1];
   SourceSpan span = msg_arg->span;
   AstNodeKind msg_kind = msg_arg->kind;

   // Check if the message is already a thunk call
   bool is_already_thunk = false;
   if (msg_kind IS AstNodeKind::CallExpr) {
      const auto &call_payload = std::get<CallExprPayload>(msg_arg->data);
      if (std::holds_alternative<DirectCallTarget>(call_payload.target)) {
         const auto &target = std::get<DirectCallTarget>(call_payload.target);
         if (target.callable and target.callable->kind IS AstNodeKind::FunctionExpr) {
            const auto &func_payload = std::get<FunctionExprPayload>(target.callable->data);
            is_already_thunk = func_payload.is_thunk;
         }
      }
   }

   // Wrap expensive expressions in thunk for lazy evaluation
   bool needs_thunk = false;
   switch (msg_kind) {
      case AstNodeKind::LiteralExpr:    // String/number literals are cheap
      case AstNodeKind::IdentifierExpr: // Simple variable access is cheap
         break;
      case AstNodeKind::CallExpr:
         if (not is_already_thunk) needs_thunk = true;
         break;
      default:
         needs_thunk = true;
         break;
   }

   if (needs_thunk and not is_already_thunk) {
      // Wrap in thunk: (thunk():str return msg end)()
      ExprNodeList return_values;
      return_values.push_back(std::move(msg_arg));
      StmtNodePtr return_stmt = make_return_stmt(span, std::move(return_values), false);

      StmtNodeList body_stmts;
      body_stmts.push_back(std::move(return_stmt));
      auto body = make_block(span, std::move(body_stmts));

      FunctionReturnTypes return_types;
      return_types.types[0] = TiriType::Str;
      return_types.count = 1;
      return_types.has_thunk_type = true;
      ExprNodePtr thunk_func = make_function_expr(span, {}, false, std::move(body), true, return_types);

      ExprNodeList call_args;
      msg_arg = make_call_expr(span, std::move(thunk_func), std::move(call_args), false);
   }

   // Append line and column as literal arguments for runtime formatting
   auto line_num = Args[0]->span.line;
   auto column = Args[0]->span.column;

   Args.push_back(make_literal_expr(span, LiteralValue::number(double(line_num))));
   Args.push_back(make_literal_expr(span, LiteralValue::number(double(column))));
}

//********************************************************************************************************************
// Result filter expression: [_*]func(), [*_]obj:method(), etc.
// Transforms to: __filter(mask, count, trailing_keep, func(...))
// The __filter function is a built-in that selectively returns values based on the filter pattern.

ParserResult<ExpDesc> IrEmitter::emit_result_filter_expr(const ResultFilterPayload &Payload)
{
   if (not Payload.expression) return this->unsupported_expr(AstNodeKind::ResultFilterExpr, SourceSpan{});

   FuncState* fs = &this->func_state;

   // Look up and emit the __filter function
   BCReg base = fs->free_reg();
   ExpDesc filter_fn;
   this->lex_state.var_lookup_symbol(lj_str_newlit(this->lex_state.L, "__filter"), &filter_fn);
   this->materialise_to_next_reg(filter_fn, "filter function");

   // Reserve register for frame link
   RegisterAllocator allocator(fs);
   allocator.reserve(BCReg(1));

   // Emit arguments: mask, count, trailing_keep
   ExpDesc mask_expr(double(Payload.keep_mask));
   this->materialise_to_next_reg(mask_expr, "filter mask");

   ExpDesc count_expr(double(Payload.explicit_count));
   this->materialise_to_next_reg(count_expr, "filter count");

   ExpDesc trail_expr(Payload.trailing_keep);
   this->materialise_to_next_reg(trail_expr, "filter trailing");

   // Emit the call expression

   auto call_result = this->emit_expression(*Payload.expression);
   if (not call_result.ok()) return call_result;
   ExpDesc call = call_result.value_ref();

   // Set B=0 on the inner call to request all return values

   if (call.k IS ExpKind::Call) set_call_result_count(fs, call, 0);
   this->materialise_to_next_reg(call, "filter input");

   // Emit CALLM to call __filter with variable arguments from the inner call
   // CALLM: base = function, C+1 = fixed args before vararg (3: mask, count, trailing)
   // The varargs come from the inner call's multiple returns

   BCIns ins = BCINS_ABC(BC_CALLM, base, 0, 3);  // 3 fixed args before vararg

   ExpDesc result;
   result.init(ExpKind::Call, bcemit_INS(fs, ins));
   result.u.s.aux = base;
   fs->freereg = base + 1;

   return ParserResult<ExpDesc>::success(result);
}
