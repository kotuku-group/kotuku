// Copyright © 2025-2026 Paul Manias
//
// IR emission pass that lowers AST nodes to LuaJIT bytecode.

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../ast/nodes.h"
#include "operator_emitter.h"
#include "../parser_context.h"
#include "../parse_control_flow.h"
#include "../parse_regalloc.h"
#include "../parse_types.h"

//********************************************************************************************************************

struct LocalBindingEntry {
   GCstr *symbol = nullptr;
   BCReg slot = BCReg(0);
   uint32_t depth = 0;
};

//********************************************************************************************************************

struct BlockBinding {
   GCstr *symbol = nullptr;
   BCReg slot = BCReg(0);
};

//********************************************************************************************************************

class LocalBindingTable {
public:
   inline LocalBindingTable() = default;

   inline void push_scope() {
      this->scope_marks.push_back(this->bindings.size());
      this->depth++;
   }

   void pop_scope();
   void add(GCstr *, BCReg);

   [[nodiscard]] inline std::optional<BCReg> resolve(GCstr *symbol) const {
      if (not symbol) return std::nullopt;
      for (auto it = this->bindings.rbegin(); it != this->bindings.rend(); ++it) {
         if (it->symbol IS symbol) return it->slot;
      }
      return std::nullopt;
   }

private:
   std::vector<LocalBindingEntry> bindings;
   std::vector<size_t> scope_marks;
   uint32_t depth = 0;
};

//********************************************************************************************************************

class LocalBindingScope {
public:
   LocalBindingScope(const LocalBindingScope &) = delete;
   LocalBindingScope& operator=(const LocalBindingScope &) = delete;

   explicit inline LocalBindingScope(LocalBindingTable &table) : table(table) {
      this->table.push_scope();
   }

   inline ~LocalBindingScope() { this->table.pop_scope(); }

private:
   LocalBindingTable &table;
};

//********************************************************************************************************************

struct IrEmitUnit {
};

//********************************************************************************************************************
// IR emission context that bundles allocator, CFG, operator emitter, and FuncState

class IrEmissionContext {
public:
   explicit IrEmissionContext(FuncState* State)
      : func_state(State), register_allocator(State), control_flow_graph(State),
        operator_emitter(State, &this->register_allocator, &this->control_flow_graph)
   { }

   inline RegisterAllocator & allocator() { return this->register_allocator; }
   inline ControlFlowGraph & cfg() { return this->control_flow_graph; }
   inline OperatorEmitter & operators() { return this->operator_emitter; }
   inline FuncState * state() const { return this->func_state; }

private:
   FuncState *func_state;
   RegisterAllocator register_allocator;
   ControlFlowGraph control_flow_graph;
   OperatorEmitter operator_emitter;
};

struct PreparedAssignment {
   PreparedAssignment() = default;
   PreparedAssignment(PreparedAssignment&&) = default;
   PreparedAssignment& operator=(PreparedAssignment&&) = default;
   PreparedAssignment(const PreparedAssignment&) = delete;
   PreparedAssignment& operator=(const PreparedAssignment&) = delete;

   LValue target{};
   ExpDesc storage{};
   RegisterSpan reserved;
   bool newly_created = false;   // True if a new local was created for an undeclared variable
   bool needs_var_add = false;   // True if var_add() must be called after expression evaluation
   GCstr* pending_symbol = nullptr;  // Symbol name for deferred var_add
   BCLine pending_line = 0;      // Line number for deferred variable declaration
   BCLine pending_column = 0;    // Column number for deferred variable declaration
   ControlFlowEdge safe_nav_skip;
};

//********************************************************************************************************************

class NilShortCircuitGuard;

class IrEmitter {
public:
   explicit IrEmitter(ParserContext& context);

   ParserResult<IrEmitUnit> emit_chunk(const BlockStmt& chunk);

private:
   friend struct LoopStackGuard;
   friend class NilShortCircuitGuard;

   ParserContext &ctx;
   FuncState &func_state;
   LexState &lex_state;
   RegisterAllocator register_allocator;
   ControlFlowGraph  control_flow;
   OperatorEmitter   operator_emitter;
   LocalBindingTable binding_table;

   ParserResult<IrEmitUnit> emit_block(const BlockStmt& block, FuncScopeFlag flags = FuncScopeFlag::None);
   ParserResult<IrEmitUnit> emit_block_with_bindings(const BlockStmt& block, FuncScopeFlag flags, std::span<const BlockBinding> bindings);
   ParserResult<IrEmitUnit> emit_statement(const StmtNode& stmt);
   ParserResult<IrEmitUnit> emit_expression_stmt(const ExpressionStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_return_stmt(const ReturnStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_local_decl_stmt(const LocalDeclStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_global_decl_stmt(const GlobalDeclStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_local_function_stmt(const LocalFunctionStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_function_stmt(const FunctionStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_if_stmt(const IfStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_while_stmt(const LoopStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_repeat_stmt(const LoopStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_numeric_for_stmt(const NumericForStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_generic_for_stmt(const GenericForStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_defer_stmt(const DeferStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_break_stmt(const BreakStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_continue_stmt(const ContinueStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_conditional_shorthand_stmt(const ConditionalShorthandStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_try_except_stmt(const TryExceptPayload& payload);
   ParserResult<IrEmitUnit> emit_raise_stmt(const RaiseStmtPayload& payload, const SourceSpan& span);
   ParserResult<IrEmitUnit> emit_check_stmt(const CheckStmtPayload& payload, const SourceSpan& span);
   ParserResult<IrEmitUnit> emit_import_stmt(const ImportStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_with_stmt(const WithStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_assignment_stmt(const AssignmentStmtPayload& payload);
   ParserResult<IrEmitUnit> emit_plain_assignment(std::vector<PreparedAssignment> targets, const ExprNodeList& values);
   ParserResult<IrEmitUnit> emit_plain_assignment_safe_multi(const ExprNodeList& targets, const ExprNodeList& values);
   ParserResult<IrEmitUnit> emit_compound_assignment(AssignmentOperator op, PreparedAssignment target, const ExprNodeList& values);
   ParserResult<IrEmitUnit> emit_if_empty_assignment(PreparedAssignment target, const ExprNodeList& values);
   ParserResult<IrEmitUnit> emit_if_nil_assignment(PreparedAssignment target, const ExprNodeList& values);

   ParserResult<ExpDesc> emit_expression(const ExprNode& expr);
   ParserResult<ExpDesc> emit_literal_expr(const LiteralValue& literal);
   ParserResult<ExpDesc> emit_identifier_expr(const NameRef& reference);
   ParserResult<ExpDesc> emit_vararg_expr();
   ParserResult<ExpDesc> emit_unary_expr(const UnaryExprPayload& payload);
   ParserResult<ExpDesc> emit_update_expr(const UpdateExprPayload& payload);
   ParserResult<ExpDesc> emit_binary_expr(const BinaryExprPayload& payload);
   ParserResult<ExpDesc> emit_ternary_expr(const TernaryExprPayload& payload);
   ParserResult<ExpDesc> emit_if_empty_expr(ExpDesc lhs, const ExprNode& rhs_ast);
   ParserResult<ExpDesc> emit_bitwise_expr(BinOpr opr, ExpDesc lhs, const ExprNode& rhs_ast);
   ParserResult<ExpDesc> emit_has_flag_expr(ExpDesc lhs, const ExprNode& rhs_ast);
   ParserResult<ExpDesc> emit_presence_expr(const PresenceExprPayload& payload);
   ParserResult<ExpDesc> emit_pipe_expr(const PipeExprPayload& payload);
   ParserResult<ExpDesc> emit_member_expr(const MemberExprPayload& payload);
   ParserResult<ExpDesc> emit_index_expr(const IndexExprPayload& payload);
   ParserResult<ExpDesc> emit_table_slice_call(const IndexExprPayload& payload);
   ParserResult<ExpDesc> emit_safe_member_expr(const SafeMemberExprPayload& payload);
   ParserResult<ExpDesc> emit_safe_index_expr(const SafeIndexExprPayload& payload);
   ParserResult<ExpDesc> emit_safe_call_expr(const CallExprPayload& payload);
   ParserResult<ExpDesc> emit_call_expr(const CallExprPayload& payload);
   ParserResult<ExpDesc> emit_result_filter_expr(const ResultFilterPayload& payload);
   ParserResult<ExpDesc> emit_table_expr(const TableExprPayload& payload);
   ParserResult<ExpDesc> emit_range_expr(const RangeExprPayload& payload);
   ParserResult<ExpDesc> emit_choose_expr(const ChooseExprPayload& payload);
   ParserResult<ExpDesc> emit_function_expr(const FunctionExprPayload& payload, GCstr* funcname = nullptr);
   ParserResult<IrEmitUnit> emit_annotation_registration(BCReg func_reg, const std::vector<AnnotationEntry>& annotations, GCstr* funcname);
   ParserResult<ExpDesc> emit_expression_list(const ExprNodeList& expressions, BCReg& count);
   ParserResult<ExpDesc> emit_lvalue_expr(
      const ExprNode& expr, bool allow_new_local = true, ControlFlowEdge* safe_nav_skip = nullptr);
   ParserResult<ControlFlowEdge> emit_condition_jump(const ExprNode& expr);
   ParserResult<ExpDesc> emit_function_lvalue(const FunctionNamePath& path);
   ParserResult<std::vector<PreparedAssignment>> prepare_assignment_targets(
      const ExprNodeList& targets, bool allow_new_local = true, bool allow_safe_nav = false);
   void materialise_to_next_reg(ExpDesc& expression, std::string_view usage);
   void materialise_to_reg(ExpDesc& expression, BCReg slot, std::string_view usage);
   void ensure_register_floor(std::string_view usage);
   void ensure_register_balance(std::string_view usage);
   void optimise_assert(ExprNodeList &Args);

   ParserResult<IrEmitUnit> unsupported_stmt(AstNodeKind kind, const SourceSpan& span);
   ParserResult<ExpDesc> unsupported_expr(AstNodeKind kind, const SourceSpan& span);

   // Create a parser error with the specified error code and message, capturing the current token context.

   inline ParserError make_error(ParserErrorCode Code, std::string_view Message) const {
      return ParserError(Code, Token::from_current(this->lex_state), Message);
   }

   inline ParserError make_error(ParserErrorCode Code, std::string_view Message, const SourceSpan &Span) const {
      return ParserError(Code, Token::from_span(Span, TokenKind::Unknown), Message);
   }

   inline std::optional<BCReg> resolve_local(GCstr *symbol) const { return this->binding_table.resolve(symbol); }
   inline void update_local_binding(GCstr *symbol, BCReg slot) { this->binding_table.add(symbol, slot); }
   inline void release_expression(ExpDesc &expression, std::string_view usage) { expr_free(&this->func_state, &expression); this->ensure_register_floor(usage); }

   struct LoopContext {
      ControlFlowEdge break_edge;
      ControlFlowEdge continue_edge;
      BCReg defer_base;
      BCPos continue_target;
      uint8_t try_depth_at_entry;  // try_depth when loop was entered
   };

   struct LoopStackGuard {
      explicit LoopStackGuard(IrEmitter* Owner) : emitter(Owner) {}
      ~LoopStackGuard() { if (this->active and this->emitter) this->emitter->loop_stack.pop_back(); }

      void release() { this->active = false; }

      IrEmitter *emitter;
      bool active = true;
   };

   LoopStackGuard push_loop_context(BCPos continue_target);
   std::vector<LoopContext> loop_stack;
};
