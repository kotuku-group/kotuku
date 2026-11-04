// Copyright © 2025-2026 Paul Manias
// IR emitter implementation for checkall...end blocks.

ParserResult<IrEmitUnit> IrEmitter::emit_checkall_stmt(const CheckallStmtPayload &Payload)
{
   if (not Payload.block) return this->unsupported_stmt(AstNodeKind::CheckallStmt, SourceSpan{});

   FuncState *fs = &this->func_state;
   BCReg base_reg = BCReg(fs->freereg);
   bcemit_AD(fs, BC_CHECKALLENTER, base_reg, BCReg(0));
   fs->runtime_scopes.push_back(RuntimeScope{ RuntimeScopeKind::Checkall, base_reg });

   {
      FuncScope scope;
      ScopeGuard guard(fs, &scope, FuncScopeFlag::None);
      LocalBindingScope binding_scope(this->binding_table);
      for (const StmtNode &statement : Payload.block->view()) {
         auto status = this->emit_statement(statement);
         if (not status.ok()) return status;
         this->ensure_register_balance(describe_node_kind(statement.kind));
      }
      // Leave checkall before ScopeGuard emits deferred calls.  A defer is cleanup outside the block's normal execution
      // and its returned errors must not be promoted by the enclosing checkall.
      bcemit_AD(fs, BC_CHECKALLLEAVE, base_reg, BCReg(0));
   }

   lj_assertX(not fs->runtime_scopes.empty() and fs->runtime_scopes.back().kind IS RuntimeScopeKind::Checkall,
      "checkall runtime scope stack is unbalanced");
   fs->runtime_scopes.pop_back();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
