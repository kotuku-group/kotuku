// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: function expression and declaration emission
// This file is #included from ir_emitter.cpp

//********************************************************************************************************************
// Emit bytecode for a function expression (function(...) ... end), creating a child function prototype.
// For thunk functions, transforms into a wrapper that returns thunk userdata via AST transformation.
// The optional funcname parameter sets the function's name for tostring() output.

ParserResult<ExpDesc> IrEmitter::emit_function_expr(const FunctionExprPayload &Payload, GCstr *funcname)
{
   if (not Payload.body) return this->unsupported_expr(AstNodeKind::FunctionExpr, SourceSpan{});

   // Handle thunk functions via AST transformation

   if (Payload.is_thunk) {
      // Transform:
      //   thunk compute(x, y):num
      //      return x * y
      //   end
      //
      // Into:
      //   function compute(x, y)
      //      return __create_thunk(function() return x * y end, type_tag)
      //   end

      // Use lastline which was set by emit_expression() to the function definition line,
      // not the body's span which may start at a later line.

      SourceSpan span = Payload.body->span;
      span.line = this->lex_state.lastline;

      // Create inner closure (no parameters, captures parent's as upvalues)
      // Move original body to inner function

      auto inner_body = std::make_unique<BlockStmt>();
      inner_body->span = span;
      for (auto& stmt : Payload.body->statements) {
         inner_body->statements.push_back(std::move(const_cast<StmtNodePtr&>(stmt)));
      }

      ExprNodePtr inner_fn = make_function_expr(span, {}, false, std::move(inner_body), false, TiriType::Any);

      // Create call to __create_thunk(inner_fn, type_tag)
      NameRef create_thunk_ref;
      create_thunk_ref.identifier.symbol = lj_str_newlit(this->lex_state.L, "__create_thunk");
      create_thunk_ref.identifier.span = span;
      create_thunk_ref.resolution = NameResolution::Unresolved;
      ExprNodePtr create_thunk_fn = make_identifier_expr(span, create_thunk_ref);

      // Type tag argument
      ExprNodePtr type_arg = make_literal_expr(span, LiteralValue::number(double(tiri_type_to_lj_tag(Payload.thunk_return_type))));

      // Build argument list
      ExprNodeList call_args;
      call_args.push_back(std::move(inner_fn));
      call_args.push_back(std::move(type_arg));

      // Create call expression
      ExprNodePtr thunk_call = make_call_expr(span, std::move(create_thunk_fn), std::move(call_args), false);

      // Create return statement
      ExprNodeList return_values;
      return_values.push_back(std::move(thunk_call));
      StmtNodePtr return_stmt = make_return_stmt(span, std::move(return_values), false);

      // Create wrapper body with just the return statement
      auto wrapper_body = std::make_unique<BlockStmt>();
      wrapper_body->span = span;
      wrapper_body->statements.push_back(std::move(return_stmt));

      // Create wrapper function payload (same parameters, not a thunk)
      FunctionExprPayload wrapper_payload;
      wrapper_payload.parameters = Payload.parameters;  // Copy parameters
      wrapper_payload.is_vararg = Payload.is_vararg;
      wrapper_payload.is_thunk = false;  // Important: wrapper is not a thunk
      wrapper_payload.body = std::move(wrapper_body);

      // Recursively emit the wrapper function (which is now a regular function)
      return this->emit_function_expr(wrapper_payload);
   }

   // Regular function emission
   FuncState *parent_state = &this->func_state;
   ptrdiff_t oldbase = parent_state->bcbase - this->lex_state.bc_stack;

   // Create FuncState in container first
   FuncState &child_state = this->lex_state.fs_init();
   FuncStateGuard fs_guard(&this->lex_state);  // Restore ls->fs on error

   ParserAllocator allocator = ParserAllocator::from(this->lex_state.L);
   ParserConfig inherited = this->ctx.config();
   ParserContext child_ctx = ParserContext::from(this->lex_state, child_state, allocator, inherited);
   ParserSession session(child_ctx, inherited);

   // Inherit declared globals from parent so nested functions recognize them
   child_state.declared_globals = parent_state->declared_globals;

   // Set linedefined to the earliest line that bytecode might reference.
   // Note: SourceSpan.line represents the END line of a span (due to combine_spans behavior),
   // so we need to find the first statement's line to get the actual start line.
   // For functions with bodies, the first statement's span gives us the earliest bytecode line.

   BCLine body_first_line = this->lex_state.lastline;
   if (Payload.body and not Payload.body->statements.empty()) {
      const StmtNodePtr& first_stmt = Payload.body->statements.front();
      if (first_stmt) body_first_line = first_stmt->span.line;
   }

   child_state.linedefined = std::min(this->lex_state.lastline, body_first_line);
   child_state.bcbase = parent_state->bcbase + parent_state->pc;
   child_state.bclim = parent_state->bclim - parent_state->pc;
   bcemit_AD(&child_state, BC_FUNCF, 0, 0);
   if (Payload.is_vararg) child_state.flags |= PROTO_VARARG;

   FuncScope scope;
   ScopeGuard scope_guard(&child_state, &scope, FuncScopeFlag::None);

   auto param_count = BCReg(BCREG(Payload.parameters.size()));
   for (auto i = BCReg(0); i < param_count; ++i) {
      const FunctionParameter& param = Payload.parameters[i.raw()];
      GCstr *symbol = (param.name.symbol and not param.name.is_blank) ? param.name.symbol : NAME_BLANK;
      this->lex_state.var_new(i, symbol, param.name.span.line, param.name.span.column);
   }

   child_state.numparams = uint8_t(param_count.raw());
   this->lex_state.var_add(param_count);
   if (child_state.varmap.size() > 0) {
      RegisterAllocator child_allocator(&child_state);
      child_allocator.reserve(BCReg(child_state.varmap.size()));
   }

   IrEmitter child_emitter(child_ctx);
   auto base = BCReg(child_state.varmap.size() - param_count.raw());
   for (auto i = BCReg(0); i < param_count; ++i) {
      const FunctionParameter &param = Payload.parameters[i.raw()];
      if (param.name.is_blank or param.name.symbol IS nullptr) continue;
      child_emitter.update_local_binding(param.name.symbol, BCReg(base.raw() + i.raw()));
   }

   // Copy explicit return types to the function state BEFORE emitting the body.
   // This ensures emit_return_stmt can see the types when deciding whether to use tail-calls
   // and whether to emit BC_TYPEFIX instructions.

   child_state.funcname = funcname;
   if (Payload.return_types.is_explicit) {
      for (size_t i = 0; i < Payload.return_types.count and i < child_state.return_types.size(); ++i) {
         child_state.return_types[i] = Payload.return_types.types[i];
      }
   }

   // Save the parent's lastline - function body emission will update it, but we need
   // to restore it so the BC_FNEW instruction gets the correct line (start of function, not body).
   BCLine saved_lastline = this->lex_state.lastline;

   auto body_result = child_emitter.emit_block(*Payload.body, FuncScopeFlag::None);
   if (not body_result.ok()) return ParserResult<ExpDesc>::failure(body_result.error_ref());

   fs_guard.disarm();  // fs_finish will handle cleanup
   GCproto *pt = this->lex_state.fs_finish(Payload.body->span.line);
   scope_guard.disarm();
   parent_state->bcbase = this->lex_state.bc_stack + oldbase;
   parent_state->bclim = BCPos(this->lex_state.size_bc_stack - oldbase).raw();

   // Restore lastline so BC_FNEW and subsequent instructions get the function definition's line
   this->lex_state.lastline = saved_lastline;

   ExpDesc expr;
   expr.init(ExpKind::Relocable, bcemit_AD(parent_state, BC_FNEW, 0, const_gc(parent_state, obj2gco(pt), LJ_TPROTO)));

   if (not (parent_state->flags & PROTO_CHILD)) {
      if (parent_state->flags & PROTO_HAS_RETURN) parent_state->flags |= PROTO_FIXUP_RETURN;
      parent_state->flags |= PROTO_CHILD;
   }

   return ParserResult<ExpDesc>::success(expr);
}

//********************************************************************************************************************
// Emit bytecode for a function declaration path (module.submodule.name or module:method), resolving the lvalue target.

ParserResult<ExpDesc> IrEmitter::emit_function_lvalue(const FunctionNamePath &path)
{
   if (path.segments.empty()) return this->unsupported_expr(AstNodeKind::FunctionExpr, SourceSpan{});

   NameRef base_ref = make_name_ref(path.segments.front());
   auto base_expr = this->emit_identifier_expr(base_ref);
   if (not base_expr.ok()) return base_expr;

   ExpDesc target = base_expr.value_ref();

   size_t traverse_limit = path.method.has_value() ? path.segments.size() : (path.segments.size() > 0 ? path.segments.size() - 1 : 0);
   for (size_t i = 1; i < traverse_limit; ++i) {
      const Identifier &segment = path.segments[i];
      if (not segment.symbol) return this->unsupported_expr(AstNodeKind::FunctionExpr, SourceSpan{});

      ExpDesc key(segment.symbol);
      ExpressionValue target_toval(&this->func_state, target);
      target_toval.to_val();
      target = target_toval.legacy();
      RegisterAllocator allocator(&this->func_state);
      ExpressionValue target_value(&this->func_state, target);
      target_value.discharge_to_any_reg(allocator);
      target = target_value.legacy();
      expr_index(&this->func_state, &target, &key);
   }

   const Identifier *final_name = nullptr;
   if (path.method.has_value()) final_name = &path.method.value();
   else if (path.segments.size() > 1) final_name = &path.segments.back();

   if (not final_name) return ParserResult<ExpDesc>::success(target);

   if (not final_name->symbol) return this->unsupported_expr(AstNodeKind::FunctionExpr, SourceSpan{});

   ExpDesc key(final_name->symbol);
   ExpressionValue target_toval_final(&this->func_state, target);
   target_toval_final.to_val();
   target = target_toval_final.legacy();

   RegisterAllocator allocator(&this->func_state);
   ExpressionValue target_value(&this->func_state, target);
   target_value.discharge_to_any_reg(allocator);
   target = target_value.legacy();
   expr_index(&this->func_state, &target, &key);
   return ParserResult<ExpDesc>::success(target);
}

//********************************************************************************************************************
// Emit bytecode for an lvalue expression (assignable location like identifier, member, or index).
// AllocNewLocal must be false when dealing with update operators like compound assignments (+=, -=) and (++, --)
// where the variable must already exist.

ParserResult<ExpDesc> IrEmitter::emit_lvalue_expr(const ExprNode &Expr, bool AllocNewLocal, ControlFlowEdge* SafeNavSkip)
{
   auto append_safe_nav_skip = [&](BCPos SkipPos) {
      if (not SafeNavSkip) return;
      if (SafeNavSkip->valid()) SafeNavSkip->append(SkipPos);
      else *SafeNavSkip = this->control_flow.make_unconditional(SkipPos);
   };

   switch (Expr.kind) {
      case AstNodeKind::IdentifierExpr: {
         const NameRef& name_ref = std::get<NameRef>(Expr.data);

         // Blank identifiers (_) are treated specially - they discard values
         if (name_ref.identifier.is_blank) {
            ExpDesc blank_expr;
            blank_expr.init(ExpKind::Global, 0);
            blank_expr.u.sval = NAME_BLANK;
            return ParserResult<ExpDesc>::success(blank_expr);
         }

         // Check if this is a registered constant - cannot assign to constants
         if (lookup_constant(name_ref.identifier.symbol)) {
            std::string var_name(strdata(name_ref.identifier.symbol), name_ref.identifier.symbol->len);
            std::string msg = "cannot assign to constant '" + var_name + "'";
            return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::AssignToConstant, msg));
         }

         auto result = this->emit_identifier_expr(name_ref);
         if (not result.ok()) return result;
         ExpDesc value = result.value_ref();
         if (value.k IS ExpKind::Local) { // Local variable already exists
            value.u.s.aux = this->func_state.varmap[value.u.s.info];
         }
         else if (value.k IS ExpKind::Unscoped) { // Undeclared variable used as assignment target
            GCstr *name = value.u.sval;

            // Check if this was explicitly declared as global (in this or parent scope)
            if (this->func_state.declared_globals.count(name) > 0) {
               value.k = ExpKind::Global;
            }
            else if (AllocNewLocal) {
               // Plain assignment: return Unscoped and let prepare_assignment_targets handle
               // the local creation with proper timing for multi-value assignments.
               // The ExpKind::Unscoped with name will signal that a new local should be created.
               // value is already Unscoped with name set, so just return it.
            }
            else { // Compound/update assignment on an undeclared variable is an error
               std::string var_name(strdata(name), name->len);
               std::string msg = "cannot use compound/update operator on undeclared variable '" + var_name + "'";
               return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::UndefinedVariable, msg));
            }
         }
         // Allow Unscoped for deferred local creation in prepare_assignment_targets
         if (not vkisvar(value.k) and value.k != ExpKind::Unscoped) return this->unsupported_expr(Expr.kind, Expr.span);
         return ParserResult<ExpDesc>::success(value);
      }

      case AstNodeKind::MemberExpr: {
         const auto &payload = std::get<MemberExprPayload>(Expr.data);
         if (not payload.table or not payload.member.symbol) return this->unsupported_expr(Expr.kind, Expr.span);

         auto table_result = SafeNavSkip
            ? this->emit_lvalue_expr(*payload.table, false, SafeNavSkip)
            : this->emit_expression(*payload.table);
         if (not table_result.ok()) return table_result;
         ExpDesc table = table_result.value_ref();

         // Save the emitted expression's result_type before discharge operations may modify it.
         // This captures type information propagated from VarInfo during variable lookup.
         TiriType emitted_base_type = table.result_type;

         ExpressionValue table_toval(&this->func_state, table);
         table_toval.to_val();
         table = table_toval.legacy();
         RegisterAllocator allocator(&this->func_state);
         ExpressionValue table_value(&this->func_state, table);
         table_value.discharge_to_any_reg(allocator);
         table = table_value.legacy();
         ExpDesc key(ExpKind::Str);
         key.u.sval = payload.member.symbol;
         expr_index(&this->func_state, &table, &key);

         // Propagate known base type information for downstream optimizations.
         // When base_type is Object, emit specialised BC_OBSETF bytecodes via IndexedObject.
         // Check both AST-level base_type AND emitted expression's result_type.
         if (payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
            table.result_type = TiriType::Object;
            // Only use IndexedObject for string keys (member access always uses string keys)
            if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) {
               table.k = ExpKind::IndexedObject;
            }
         }

         return ParserResult<ExpDesc>::success(table);
      }

      case AstNodeKind::IndexExpr: {
         const auto &payload = std::get<IndexExprPayload>(Expr.data);
         if (not payload.table or not payload.index) return this->unsupported_expr(Expr.kind, Expr.span);

         auto table_result = SafeNavSkip
            ? this->emit_lvalue_expr(*payload.table, false, SafeNavSkip)
            : this->emit_expression(*payload.table);
         if (not table_result.ok()) return table_result;

         ExpDesc table = table_result.value_ref();

         // Save the emitted expression's result_type before discharge operations may modify it.
         // This captures type information propagated from VarInfo during variable lookup.
         TiriType emitted_base_type = table.result_type;

         // Materialize table BEFORE evaluating key, so nested index expressions emit bytecode in
         // the correct order (table first, then key)

         ExpressionValue table_toval_idx(&this->func_state, table);
         table_toval_idx.to_val();
         table = table_toval_idx.legacy();
         RegisterAllocator allocator(&this->func_state);
         ExpressionValue table_value(&this->func_state, table);
         table_value.discharge_to_any_reg(allocator);
         table = table_value.legacy();
         auto key_result = this->emit_expression(*payload.index);
         if (not key_result.ok()) return key_result;

         ExpDesc key = key_result.value_ref();
         ExpressionValue key_toval_idx(&this->func_state, key);
         key_toval_idx.to_val();
         key = key_toval_idx.legacy();
         expr_index(&this->func_state, &table, &key);

         // Propagate known base type information for downstream optimizations.
         // Check both AST-level base_type AND emitted expression's result_type.
         if (payload.base_type IS TiriType::Array or emitted_base_type IS TiriType::Array) {
            // Arrays don't support string keys, so only change kind for numeric indexing
            if (int32_t(table.u.s.aux) >= 0) {
               table.k = ExpKind::IndexedArray;
            }
         }
         else if (payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
            // Objects use string field access - only change kind for string const keys
            if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) {
               table.k = ExpKind::IndexedObject;
            }
         }

         return ParserResult<ExpDesc>::success(table);
      }

      case AstNodeKind::SafeMemberExpr: {
         if (not SafeNavSkip) {
            return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
               "Safe navigation operators (?. and ?[]) cannot be used as assignment targets"));
         }

         const auto &payload = std::get<SafeMemberExprPayload>(Expr.data);
         if (not payload.table or not payload.member.symbol) return this->unsupported_expr(Expr.kind, Expr.span);

         auto table_result = this->emit_lvalue_expr(*payload.table, false, SafeNavSkip);
         if (not table_result.ok()) return table_result;
         ExpDesc table = table_result.value_ref();

         TiriType emitted_base_type = table.result_type;

         ExpressionValue table_toval(&this->func_state, table);
         table_toval.to_val();
         table = table_toval.legacy();
         RegisterAllocator allocator(&this->func_state);
         ExpressionValue table_value(&this->func_state, table);
         BCREG table_reg = table_value.discharge_to_any_reg(allocator);
         table = table_value.legacy();

         ExpDesc nilv(ExpKind::Nil);
         bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, table_reg, const_pri(&nilv)));
         append_safe_nav_skip(BCPos(bcemit_jmp(&this->func_state)));

         ExpDesc key(ExpKind::Str);
         key.u.sval = payload.member.symbol;
         expr_index(&this->func_state, &table, &key);

         if (payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
            table.result_type = TiriType::Object;
            if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) {
               table.k = ExpKind::IndexedObject;
            }
         }

         return ParserResult<ExpDesc>::success(table);
      }

      case AstNodeKind::SafeIndexExpr: {
         if (not SafeNavSkip) {
            return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant,
               "Safe navigation operators (?. and ?[]) cannot be used as assignment targets"));
         }

         const auto &payload = std::get<SafeIndexExprPayload>(Expr.data);
         if (not payload.table or not payload.index) return this->unsupported_expr(Expr.kind, Expr.span);

         auto table_result = this->emit_lvalue_expr(*payload.table, false, SafeNavSkip);
         if (not table_result.ok()) return table_result;
         ExpDesc table = table_result.value_ref();

         TiriType emitted_base_type = table.result_type;

         ExpressionValue table_toval_idx(&this->func_state, table);
         table_toval_idx.to_val();
         table = table_toval_idx.legacy();
         RegisterAllocator allocator(&this->func_state);
         ExpressionValue table_value(&this->func_state, table);
         BCREG table_reg = table_value.discharge_to_any_reg(allocator);
         table = table_value.legacy();

         ExpDesc nilv(ExpKind::Nil);
         bcemit_INS(&this->func_state, BCINS_AD(BC_ISEQP, table_reg, const_pri(&nilv)));
         append_safe_nav_skip(BCPos(bcemit_jmp(&this->func_state)));

         auto key_result = this->emit_expression(*payload.index);
         if (not key_result.ok()) return key_result;

         ExpDesc key = key_result.value_ref();
         ExpressionValue key_toval_idx(&this->func_state, key);
         key_toval_idx.to_val();
         key = key_toval_idx.legacy();
         expr_index(&this->func_state, &table, &key);

         if (payload.base_type IS TiriType::Array or emitted_base_type IS TiriType::Array) {
            if (int32_t(table.u.s.aux) >= 0) {
               table.k = ExpKind::IndexedArray;
            }
         }
         else if (payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
            if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) {
               table.k = ExpKind::IndexedObject;
            }
         }

         return ParserResult<ExpDesc>::success(table);
      }

      default:
         return this->unsupported_expr(Expr.kind, Expr.span);
   }
}

//********************************************************************************************************************
// Emit bytecode for a local function declaration, creating a local variable and assigning a function to it.

ParserResult<IrEmitUnit> IrEmitter::emit_local_function_stmt(const LocalFunctionStmtPayload &Payload)
{
   if (not Payload.function) return this->unsupported_stmt(AstNodeKind::LocalFunctionStmt, SourceSpan{});

   GCstr *symbol = Payload.name.symbol ? Payload.name.symbol : NAME_BLANK;
   auto slot = BCReg(this->func_state.freereg);
   this->lex_state.var_new(0, symbol, Payload.name.span.line, Payload.name.span.column);
   RegisterAllocator allocator(&this->func_state);
   allocator.reserve(BCReg(1));
   this->lex_state.var_add(1);
   ExpDesc variable;
   variable.init(ExpKind::Local, slot);
   variable.u.s.aux = this->func_state.varmap[slot];

   // Pass the function name for tostring() support

   auto function_value = this->emit_function_expr(*Payload.function, Payload.name.symbol);
   if (not function_value.ok()) return ParserResult<IrEmitUnit>::failure(function_value.error_ref());

   ExpDesc fn = function_value.value_ref();
   this->materialise_to_reg(fn, slot, "local function literal");
   VarInfo &var_info = this->func_state.var_get(this->func_state.varmap.size() - 1);
   var_info.startpc = this->func_state.pc;
   var_info.fixed_type = TiriType::Func;  // Function declarations have known type
   if (Payload.name.symbol and not Payload.name.is_blank) this->update_local_binding(Payload.name.symbol, slot);

   // Copy function return types to VarInfo for compile-time type checking at call sites
   if (Payload.function->return_types.is_explicit) {
      for (size_t i = 0; i < Payload.function->return_types.count and i < var_info.result_types.size(); ++i) {
         var_info.result_types[i] = Payload.function->return_types.types[i];
      }
   }

   // Register annotations if present

   if (not Payload.function->annotations.empty()) {
      auto anno_result = this->emit_annotation_registration(slot, Payload.function->annotations, Payload.name.symbol);
      if (not anno_result.ok()) return anno_result;
   }

   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a function declaration statement.
// With protected_globals enabled, simple function declarations (function foo()) create local functions.
// Method syntax (function foo:bar()) and table paths (function foo.bar()) always store to the target.
// Explicit global declarations (global function foo()) always store to global.

ParserResult<IrEmitUnit> IrEmitter::emit_function_stmt(const FunctionStmtPayload &Payload)
{
   if (not Payload.function) return this->unsupported_stmt(AstNodeKind::FunctionStmt, SourceSpan{});

   // If explicitly declared as global, register the name so nested functions can access it

   if (Payload.name.is_explicit_global and not Payload.name.segments.empty()) {
      GCstr *name = Payload.name.segments.front().symbol;
      if (name) this->func_state.declared_globals.insert(name);
   }

   // Check if this is a simple function name (not a path like foo.bar or method foo:bar)
   // and if protected_globals is enabled without explicit global declaration

   bool is_simple_name = Payload.name.segments.size() == 1 and not Payload.name.method.has_value();
   bool should_be_local = is_simple_name and not Payload.name.is_explicit_global;

   // Determine the function name for tostring() support.
   // For simple names, use the single segment. For paths like foo.bar, use the last segment.
   // For methods like foo:bar, use the method name.

   GCstr *funcname = nullptr;
   if (Payload.name.method.has_value() and Payload.name.method->symbol) {
      funcname = Payload.name.method->symbol;
   }
   else if (not Payload.name.segments.empty()) {
      funcname = Payload.name.segments.back().symbol;
   }

   if (should_be_local) {
      // Emit as a local function (same as local function foo())
      GCstr *symbol = Payload.name.segments.front().symbol;
      if (not symbol) return this->unsupported_stmt(AstNodeKind::FunctionStmt, SourceSpan{});

      auto slot = BCReg(this->func_state.freereg);
      auto& first_segment = Payload.name.segments.front();
      this->lex_state.var_new(0, symbol, first_segment.span.line, first_segment.span.column);
      RegisterAllocator allocator(&this->func_state);
      allocator.reserve(BCReg(1));
      this->lex_state.var_add(1);
      ExpDesc variable;
      variable.init(ExpKind::Local, slot);
      variable.u.s.aux = this->func_state.varmap[slot];

      // Pass the function name for tostring() support
      auto function_value = this->emit_function_expr(*Payload.function, funcname);
      if (not function_value.ok()) return ParserResult<IrEmitUnit>::failure(function_value.error_ref());

      ExpDesc fn = function_value.value_ref();
      this->materialise_to_reg(fn, slot, "function literal");
      VarInfo &var_info = this->func_state.var_get(this->func_state.varmap.size() - 1);
      var_info.startpc = this->func_state.pc;
      var_info.fixed_type = TiriType::Func;  // Function declarations have known type
      this->update_local_binding(symbol, slot);

      // Copy function return types to VarInfo for compile-time type checking at call sites
      if (Payload.function->return_types.is_explicit) {
         for (size_t i = 0; i < Payload.function->return_types.count and i < var_info.result_types.size(); ++i) {
            var_info.result_types[i] = Payload.function->return_types.types[i];
         }
      }

      // Register annotations if present
      if (not Payload.function->annotations.empty()) {
         auto anno_result = this->emit_annotation_registration(slot, Payload.function->annotations, funcname);
         if (not anno_result.ok()) return anno_result;
      }

      this->func_state.reset_freereg();
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Original behaviour: store to global or table field
   auto target_result = this->emit_function_lvalue(Payload.name);
   if (not target_result.ok()) return ParserResult<IrEmitUnit>::failure(target_result.error_ref());
   // Pass the function name for tostring() support
   auto function_value = this->emit_function_expr(*Payload.function, funcname);
   if (not function_value.ok()) return ParserResult<IrEmitUnit>::failure(function_value.error_ref());

   ExpDesc target = target_result.value_ref();
   ExpDesc value = function_value.value_ref();

   // For annotation registration, we need the function in a register
   // Materialise the function value to a register before the store
   BCReg func_reg = BCReg(BCREG(0));
   bool has_annotations = not Payload.function->annotations.empty();
   if (has_annotations) {
      func_reg = BCReg(this->func_state.freereg);
      this->materialise_to_next_reg(value, "annotated function");
      // Reserve the function register so emit_annotation_registration doesn't overwrite it
      bcreg_reserve(&this->func_state, 1);
   }

   bcemit_store(&this->func_state, &target, &value);
   release_indexed_original(this->func_state, target);

   // Register annotations if present
   if (has_annotations) {
      auto anno_result = this->emit_annotation_registration(func_reg, Payload.function->annotations, funcname);
      if (not anno_result.ok()) return anno_result;
   }

   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode to register annotations for a function in the _ANNO global table.
// This generates code equivalent to: debug.anno.set(func, "@Anno...", source, name)
// The function reference is expected to be in the specified register.

ParserResult<IrEmitUnit> IrEmitter::emit_annotation_registration(BCReg FuncReg, const std::vector<AnnotationEntry>& Annotations, GCstr* Funcname)
{
   if (Annotations.empty()) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   FuncState* fs = &this->func_state;
   lua_State* L = fs->L;

   // Build annotation string from parsed annotation entries
   // Format: @Name(key=value, ...); @Name2; ...
   std::string anno_str;
   for (const auto& anno : Annotations) {
      if (not anno_str.empty()) anno_str += "; ";
      anno_str += "@";
      if (anno.name) anno_str.append(strdata(anno.name), anno.name->len);

      if (not anno.args.empty()) {
         anno_str += "(";
         bool first_arg = true;
         for (const auto& [key, value] : anno.args) {
            if (not first_arg) anno_str += ", ";
            first_arg = false;
            if (key) anno_str.append(strdata(key), key->len);
            anno_str += "=";
            switch (value.type) {
               case AnnotationArgValue::Type::Bool:
                  anno_str += value.bool_value ? "true" : "false";
                  break;
               case AnnotationArgValue::Type::Number:
                  anno_str += std::to_string(value.number_value);
                  break;
               case AnnotationArgValue::Type::String:
                  anno_str += "\"";
                  if (value.string_value) anno_str.append(strdata(value.string_value), value.string_value->len);
                  anno_str += "\"";
                  break;
               case AnnotationArgValue::Type::Array: {
                  anno_str += "[";
                  bool first_elem = true;
                  for (const auto& elem : value.array_value) {
                     if (not first_elem) anno_str += ",";
                     first_elem = false;
                     if (elem.type IS AnnotationArgValue::Type::String and elem.string_value) {
                        anno_str += "\"";
                        anno_str.append(strdata(elem.string_value), elem.string_value->len);
                        anno_str += "\"";
                     }
                     else if (elem.type IS AnnotationArgValue::Type::Number) {
                        anno_str += std::to_string(elem.number_value);
                     }
                     else if (elem.type IS AnnotationArgValue::Type::Bool) {
                        anno_str += elem.bool_value ? "true" : "false";
                     }
                  }
                  anno_str += "]";
                  break;
               }
               default:
                  anno_str += "nil";
                  break;
            }
         }
         anno_str += ")";
      }
   }

   // Save base register and allocate space for the call
   // With LJ_FR2=1, layout is: [base]=func, [base+1]=frame, [base+2]=arg1, [base+3]=arg2, ...
   // So args start at base + 1 + LJ_FR2 = base + 2
   BCREG base_raw = fs->freereg;

   // Helper to get constant string index
   auto str_const = [fs](GCstr* s) -> BCREG {
      return const_gc(fs, obj2gco(s), LJ_TSTR);
   };

   // Load debug.anno.set into base register
   // GGET debug -> base
   bcemit_AD(fs, BC_GGET, base_raw, str_const(lj_str_newlit(L, "debug")));

   // TGETS anno -> base (debug.anno)
   bcemit_tgets(fs, base_raw, base_raw, str_const(lj_str_newlit(L, "anno")));

   // TGETS set -> base (debug.anno.set)
   bcemit_tgets(fs, base_raw, base_raw, str_const(lj_str_newlit(L, "set")));

   // Args start at base + 1 + LJ_FR2 (skip function slot and frame slot)
   BCREG args_base = base_raw + 1 + LJ_FR2;

   // Arg 1: function reference (copy from FuncReg)
   bcemit_AD(fs, BC_MOV, args_base, FuncReg.raw());

   // Arg 2: annotation string
   bcemit_AD(fs, BC_KSTR, args_base + 1, str_const(lj_str_new(L, anno_str.c_str(), anno_str.size())));

   // Arg 3: source file name
   GCstr* source = this->lex_state.chunk_name;
   bcemit_AD(fs, BC_KSTR, args_base + 2, str_const(source ? source : lj_str_newlit(L, "<unknown>")));

   // Arg 4: function name
   bcemit_AD(fs, BC_KSTR, args_base + 3, str_const(Funcname ? Funcname : lj_str_newlit(L, "<anonymous>")));

   // Emit call: debug.anno.set(func, annostr, source, name)
   // BC_CALL A=base, B=2 (expect 1 result for discard), C=5 (4 args + 1)
   bcemit_ABC(fs, BC_CALL, base_raw, 2, 5);

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
