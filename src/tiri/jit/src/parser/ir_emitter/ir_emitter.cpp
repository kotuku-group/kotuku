// Copyright © 2025-2026 Paul Manias

#include "ir_emitter.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <kotuku/main.h>

#include "lj_debug.h"
#include "lj_tab.h"
#include "lj_proto_registry.h"
#include "../parse_internal.h"
#include "../parse_value.h"
#include "../token_types.h"
#include "../../../defs.h"  // For glPrintMsg, TiriConstant

//********************************************************************************************************************
// Returns nullptr if not found.

inline const TiriConstant * lookup_constant(const GCstr *Name)
{
   std::shared_lock lock(glConstantMutex);
   auto it = glConstantRegistry.find(Name->hash);
   if (it != glConstantRegistry.end()) return &it->second;
   return nullptr;
}

//********************************************************************************************************************
// NilShortCircuitGuard - RAII helper for safe navigation nil-check pattern.
//
// Encapsulates the common control flow for safe navigation operators:
//   1. Discharge operand to register
//   2. Emit BC_ISEQP nil check with conditional jump
//   3. [Caller performs operation on non-nil path]
//   4. complete() emits nil path and patches jumps
//
// Usage:
//   NilShortCircuitGuard guard(emitter, base_expression);
//   if (not guard.ok()) return guard.error<ExpDesc>();
//   // ... perform operation using guard.base_register() ...
//   materialise_to_reg(result, guard.base_register(), "...");
//   return guard.complete();

class NilShortCircuitGuard {
public:
   NilShortCircuitGuard(IrEmitter *Emitter, ExpDesc BaseExpr)
      : emitter(Emitter), register_guard(&Emitter->func_state), allocator(&Emitter->func_state)
   {
      ExpressionValue base_value(&this->emitter->func_state, BaseExpr);
      this->result_reg = base_value.discharge_to_any_reg(this->allocator);
      this->base_expr = base_value.legacy();

      ExpDesc nilv(ExpKind::Nil);
      bcemit_INS(&this->emitter->func_state, BCINS_AD(BC_ISEQP, this->result_reg, const_pri(&nilv)));
      this->nil_jump = this->emitter->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->emitter->func_state)));
      this->setup_ok = true;
   }

   [[nodiscard]] bool ok() const { return this->setup_ok; }

   template<typename T>
   ParserResult<T> error() const {
      ParserError err;
      err.code = ParserErrorCode::InternalInvariant;
      err.message = "nil guard setup failed";
      return ParserResult<T>::failure(err);
   }

   [[nodiscard]] inline BCREG base_register() const { return this->result_reg; }
   [[nodiscard]] inline ExpDesc base_expression() const { return this->base_expr; }
   [[nodiscard]] inline RegisterAllocator& reg_allocator() { return this->allocator; }
   [[nodiscard]] inline ControlFlowEdge& nil_jump_edge() { return this->nil_jump; }

   // Complete the nil short-circuit: emit nil path, patch jumps, return result.
   // The result is stored in base_register() as a NonReloc expression.

   ParserResult<ExpDesc> complete()
   {
      this->allocator.collapse_freereg(BCReg(this->result_reg));

      ControlFlowEdge skip_nil = this->emitter->control_flow.make_unconditional(
         BCPos(bcemit_jmp(&this->emitter->func_state)));

      BCPos nil_path = BCPos(this->emitter->func_state.pc);
      this->nil_jump.patch_to(nil_path);
      bcemit_nil(&this->emitter->func_state, this->result_reg, 1);

      skip_nil.patch_to(BCPos(this->emitter->func_state.pc));

      this->register_guard.disarm();

      ExpDesc result;
      result.init(ExpKind::NonReloc, this->result_reg);
      return ParserResult<ExpDesc>::success(result);
   }

   // Complete with a custom result register (for call expressions where result may differ).
   // Note: Unlike complete(), we don't call collapse_freereg(result_reg) here because CallBase
   // may differ from result_reg after method dispatch setup, and we explicitly set freereg to
   // CallBase + 1 at the end, which is the correct final state for call expressions.

   ParserResult<ExpDesc> complete_call(BCReg CallBase, BCPos CallPc)
   {
      ControlFlowEdge skip_nil = this->emitter->control_flow.make_unconditional(
         BCPos(bcemit_jmp(&this->emitter->func_state)));

      BCPos nil_path = BCPos(this->emitter->func_state.pc);
      this->nil_jump.patch_to(nil_path);
      bcemit_nil(&this->emitter->func_state, CallBase.raw(), 1);

      skip_nil.patch_to(BCPos(this->emitter->func_state.pc));

      this->register_guard.adopt_saved(BCReg(CallBase.raw() + 1));
      this->register_guard.disarm();

      ExpDesc result;
      result.init(ExpKind::Call, CallPc);
      result.u.s.aux = CallBase;
      this->emitter->func_state.freereg = CallBase + 1;
      return ParserResult<ExpDesc>::success(result);
   }

private:
   IrEmitter *emitter;
   RegisterGuard register_guard;
   RegisterAllocator allocator;
   ControlFlowEdge nil_jump;
   ExpDesc base_expr;
   BCReg result_reg = BCReg(0);
   bool setup_ok = false;
};

//********************************************************************************************************************
// Assignment helpers implemented in emit_assignment.cpp, which is included later in this translation unit.

static bool contains_safe_nav_target(const ExprNode& Expr);

//********************************************************************************************************************
// Check if any active local variables have the <close> attribute.
// Close handlers use temporary registers that could clobber return values.

static bool has_close_variables(FuncState* fs)
{
   for (BCREG i = 0; i < fs->varmap.size(); ++i) {
      VarInfo* v = &fs->var_get(i);
      if (has_flag(v->info, VarInfoFlag::Close)) return true;
   }
   return false;
}

//********************************************************************************************************************
// Snapshot return register state.
// Used by ir_emitter for return statement handling.
//
// This function ensures return values are in safe registers before __close and defer handlers run.
// Close handlers (bcemit_close) use temporary registers starting at freereg (which is set to varmap.size()).
// They reserve 5+LJ_FR2 registers for: getmetatable function, metatable result, __close function, args.
// If return values overlap with these temporary registers, they must be moved to safe slots.

static constexpr BCREG CLOSE_HANDLER_TEMP_REGS = 5 + LJ_FR2;

static void snapshot_return_regs(FuncState* fs, BCIns* ins)
{
   BCOp op = bc_op(*ins);

   // Calculate the "danger zone" for return values.
   // If there are close handlers, they use varmap.size() to varmap.size()+CLOSE_HANDLER_TEMP_REGS as temporaries.
   // Return values in this range must be snapshotted to safe slots.
   bool has_closes = has_close_variables(fs);
   BCReg danger_limit = BCReg(fs->varmap.size() + (has_closes ? CLOSE_HANDLER_TEMP_REGS : 0));

   if (op IS BC_RET1) {
      auto src = BCReg(bc_a(*ins));
      if (src < danger_limit) {
         RegisterAllocator allocator(fs);
         auto dst = fs->free_reg();
         // Skip past close handler temporaries if needed
         if (has_closes and dst < danger_limit) {
            allocator.reserve(BCReg(danger_limit.raw() - dst.raw()));
            dst = fs->free_reg();
         }
         allocator.reserve(BCReg(1));
         bcemit_AD(fs, BC_MOV, dst, src);
         setbc_a(ins, dst);
      }
   }
   else if (op IS BC_RET) {
      auto base = BCReg(bc_a(*ins));
      auto nres = BCReg(bc_d(*ins));
      auto top = BCReg(base.raw() + nres.raw() - 1);
      if (top < danger_limit) {
         RegisterAllocator allocator(fs);
         auto dst = fs->free_reg();
         // Skip past close handler temporaries if needed
         if (has_closes and dst < danger_limit) {
            allocator.reserve(BCReg(danger_limit.raw() - dst.raw()));
            dst = fs->free_reg();
         }
         allocator.reserve(nres);
         for (auto i = BCReg(0); i < nres; ++i) {
            bcemit_AD(fs, BC_MOV, dst + i, base + i);
         }
         setbc_a(ins, dst);
      }
   }
   else if (op IS BC_RETM) {
      auto base = BCReg(bc_a(*ins));
      auto nfixed = BCReg(bc_d(*ins));
      // For multi-result returns (nfixed=0 from call), we know at least 1 value is at base.
      // We need to protect it if it falls in the danger zone.
      auto min_values = BCReg(nfixed.raw() > 0 ? nfixed.raw() : 1);
      auto top = BCReg(base.raw() + min_values.raw() - 1);
      if (top < danger_limit) {
         RegisterAllocator allocator(fs);
         auto dst = fs->free_reg();
         // Skip past close handler temporaries if needed
         if (has_closes and dst < danger_limit) {
            allocator.reserve(BCReg(danger_limit.raw() - dst.raw()));
            dst = fs->free_reg();
         }
         allocator.reserve(min_values);
         for (auto i = BCReg(0); i < min_values; ++i) {
            bcemit_AD(fs, BC_MOV, dst + i, base + i);
         }
         setbc_a(ins, dst);
      }
   }
}

//********************************************************************************************************************
// Adjust LHS/RHS of an assignment.
// Exclusively used by ir_emitter for assignment statements, local declarations, and for loops.
// TODO: May as well be a regular function instead of a LexState method.

void LexState::assign_adjust(BCREG nvars, BCREG nexps, ExpDesc *Expr)
{
   FuncState* fs = this->fs;
   RegisterAllocator allocator(fs);
   int32_t extra = int32_t(nvars) - int32_t(nexps);
   if (Expr->k IS ExpKind::Call) {
      extra++;  // Compensate for the ExpKind::Call itself.
      if (extra < 0) extra = 0;
      setbc_b(bcptr(fs, Expr), extra + 1);  // Fixup call results.
      if (extra > 1) allocator.reserve(BCReg(BCREG(extra) - 1));
   }
   else {
      if (Expr->k IS ExpKind::Void) {
         // Void expression contributes no values, so all LHS variables need nil.
         // This handles cases like `local a, b = assert(...)` where a shadow function might return void.
         extra = int32_t(nvars);
      }
      else {
         ExpressionValue value(fs, *Expr);
         value.to_next_reg(allocator);
         *Expr = value.legacy();
      }

      if (extra > 0) {  // Leftover LHS are set to nil.
         auto reg = fs->free_reg();
         allocator.reserve(BCReg(BCREG(extra)));
         bcemit_nil(fs, reg, BCREG(extra));
      }
   }

   if (nexps > nvars) fs->freereg -= nexps - nvars;  // Drop leftover regs.
}

//********************************************************************************************************************
// Restore the local binding table to the previous scope level by removing bindings added in the current scope.

void LocalBindingTable::pop_scope()
{
   if (this->scope_marks.empty()) {
      this->bindings.clear();
      this->depth = 0;
      return;
   }
   size_t restore = this->scope_marks.back();
   this->scope_marks.pop_back();
   this->bindings.resize(restore);
   if (this->depth > 0) this->depth--;
}

// Add a new local variable binding to the table, associating a symbol with its register slot.

void LocalBindingTable::add(GCstr* symbol, BCReg slot)
{
   if (not symbol) return;
   LocalBindingEntry entry;
   entry.symbol = symbol;
   entry.slot = slot;
   entry.depth = this->depth;
   this->bindings.push_back(entry);
}

//********************************************************************************************************************
// IR emission context implementation

constexpr size_t kAstNodeKindCount = size_t(AstNodeKind::ExpressionStmt) + 1;

class UnsupportedNodeRecorder {
public:
   void record(AstNodeKind kind, const SourceSpan &span, CSTRING stage) {
      size_t index = size_t(kind);
      if (index >= this->counts.size()) return;
      uint32_t total = ++this->counts[index];
      if ((total <= 8) or (total % 32 IS 0)) {
         kt::Log log("Parser");
         log.msg("Unsupported %s node kind=%u hits=%u line=%d column=%d offset=%lld",
            stage, unsigned(kind), unsigned(total), int(span.line), int(span.column),
            (long long)(span.offset));
      }
   }

private:
   std::array<uint32_t, kAstNodeKindCount> counts{};
};

static UnsupportedNodeRecorder glUnsupportedNodes;

//********************************************************************************************************************
// Check if an identifier is blank (underscore placeholder) or has no associated symbol.

[[nodiscard]] static bool is_blank_symbol(const Identifier& identifier)
{
   return identifier.is_blank or identifier.symbol IS nullptr;
}

//********************************************************************************************************************
// Check if an ExpDesc represents a blank identifier target (used in assignments).
// Blank identifiers are represented as Global with NAME_BLANK symbol.

[[nodiscard]] static bool is_blank_target(const ExpDesc& expr)
{
   return expr.k IS ExpKind::Global and expr.u.sval IS NAME_BLANK;
}

//********************************************************************************************************************
// Map an AST binary operator to its corresponding bytecode binary operator representation.

[[nodiscard]] static std::optional<BinOpr> map_binary_operator(AstBinaryOperator op)
{
   switch (op) {
      case AstBinaryOperator::Add:          return BinOpr::Add;
      case AstBinaryOperator::Subtract:     return BinOpr::Sub;
      case AstBinaryOperator::Multiply:     return BinOpr::Mul;
      case AstBinaryOperator::Divide:       return BinOpr::Div;
      case AstBinaryOperator::Modulo:       return BinOpr::Mod;
      case AstBinaryOperator::Power:        return BinOpr::Pow;
      case AstBinaryOperator::Concat:       return BinOpr::Concat;
      case AstBinaryOperator::NotEqual:     return BinOpr::NotEqual;
      case AstBinaryOperator::Equal:        return BinOpr::Equal;
      case AstBinaryOperator::LessThan:     return BinOpr::LessThan;
      case AstBinaryOperator::GreaterEqual: return BinOpr::GreaterEqual;
      case AstBinaryOperator::LessEqual:    return BinOpr::LessEqual;
      case AstBinaryOperator::GreaterThan:  return BinOpr::GreaterThan;
      case AstBinaryOperator::Approx:       return BinOpr::Approx;
      case AstBinaryOperator::BitAnd:       return BinOpr::BitAnd;
      case AstBinaryOperator::BitOr:        return BinOpr::BitOr;
      case AstBinaryOperator::BitXor:       return BinOpr::BitXor;
      case AstBinaryOperator::ShiftLeft:    return BinOpr::ShiftLeft;
      case AstBinaryOperator::ShiftRight:   return BinOpr::ShiftRight;
      case AstBinaryOperator::LogicalAnd:   return BinOpr::LogicalAnd;
      case AstBinaryOperator::LogicalOr:    return BinOpr::LogicalOr;
      case AstBinaryOperator::IfEmpty:      return BinOpr::IfEmpty;
      case AstBinaryOperator::HasFlag:     return BinOpr::HasFlag;
      default: return std::nullopt;
   }
}

//********************************************************************************************************************
// Map a compound assignment operator (+=, -=, etc.) to its corresponding binary operator.

[[nodiscard]] static std::optional<BinOpr> map_assignment_operator(AssignmentOperator op)
{
   switch (op) {
      case AssignmentOperator::Add:      return BinOpr::Add;
      case AssignmentOperator::Subtract: return BinOpr::Sub;
      case AssignmentOperator::Multiply: return BinOpr::Mul;
      case AssignmentOperator::Divide:   return BinOpr::Div;
      case AssignmentOperator::Modulo:   return BinOpr::Mod;
      case AssignmentOperator::Concat:   return BinOpr::Concat;
      default: return std::nullopt;
   }
}

//********************************************************************************************************************
// Check if an auxiliary value represents a valid register key (used for indexed expressions).

[[nodiscard]] static bool is_register_key(uint32_t aux)
{
   return (int32_t(aux) >= 0) and (aux <= BCMAX_C);
}

//********************************************************************************************************************
// Convert an AST node kind enumeration to its human-readable string representation for debugging and logging.

[[nodiscard]] static std::string_view describe_node_kind(AstNodeKind kind)
{
   switch (kind) {
      case AstNodeKind::LiteralExpr:    return "LiteralExpr";
      case AstNodeKind::IdentifierExpr: return "IdentifierExpr";
      case AstNodeKind::VarArgExpr:     return "VarArgExpr";
      case AstNodeKind::UnaryExpr:      return "UnaryExpr";
      case AstNodeKind::BinaryExpr:     return "BinaryExpr";
      case AstNodeKind::ComparisonChainExpr: return "ComparisonChainExpr";
      case AstNodeKind::UpdateExpr:     return "UpdateExpr";
      case AstNodeKind::TernaryExpr:    return "TernaryExpr";
      case AstNodeKind::PresenceExpr:   return "PresenceExpr";
      case AstNodeKind::CallExpr:       return "CallExpr";
      case AstNodeKind::MemberExpr:     return "MemberExpr";
      case AstNodeKind::IndexExpr:      return "IndexExpr";
      case AstNodeKind::ResultFilterExpr: return "ResultFilterExpr";
      case AstNodeKind::TableExpr:      return "TableExpr";
      case AstNodeKind::RangeExpr:      return "RangeExpr";
      case AstNodeKind::FunctionExpr:   return "FunctionExpr";
      case AstNodeKind::BlockStmt:      return "BlockStmt";
      case AstNodeKind::AssignmentStmt: return "AssignmentStmt";
      case AstNodeKind::LocalDeclStmt:  return "LocalDeclStmt";
      case AstNodeKind::GlobalDeclStmt: return "GlobalDeclStmt";
      case AstNodeKind::LocalFunctionStmt: return "LocalFunctionStmt";
      case AstNodeKind::FunctionStmt:   return "FunctionStmt";
      case AstNodeKind::IfStmt:         return "IfStmt";
      case AstNodeKind::WhileStmt:      return "WhileStmt";
      case AstNodeKind::RepeatStmt:     return "RepeatStmt";
      case AstNodeKind::NumericForStmt: return "NumericForStmt";
      case AstNodeKind::GenericForStmt: return "GenericForStmt";
      case AstNodeKind::BreakStmt:      return "BreakStmt";
      case AstNodeKind::ContinueStmt:   return "ContinueStmt";
      case AstNodeKind::ReturnStmt:     return "ReturnStmt";
      case AstNodeKind::DeferStmt:      return "DeferStmt";
      case AstNodeKind::DoStmt:         return "DoStmt";
      case AstNodeKind::ConditionalShorthandStmt: return "ConditionalShorthandStmt";
      case AstNodeKind::TryExceptStmt: return "TryExceptStmt";
      case AstNodeKind::RaiseStmt:     return "RaiseStmt";
      case AstNodeKind::CheckStmt:     return "CheckStmt";
      case AstNodeKind::WithStmt:      return "WithStmt";
      case AstNodeKind::ExpressionStmt: return "ExpressionStmt";
      default: return "Unknown";
   }
}

//********************************************************************************************************************
// Create an unresolved name reference from an identifier for later symbol resolution.

[[nodiscard]] static NameRef make_name_ref(const Identifier& identifier)
{
   NameRef ref;
   ref.identifier = identifier;
   ref.resolution = NameResolution::Unresolved;
   ref.slot = 0;
   return ref;
}

//********************************************************************************************************************
// Predict if a bytecode instruction loads the 'pairs' or 'next' iterator, used to optimise generic for loops.

[[nodiscard]] static int predict_next(LexState& lex_state, FuncState &func_state, BCPos pc)
{
   BCIns ins = func_state.bcbase[pc].ins;
   GCstr *name = nullptr;
   cTValue *table_entry = nullptr;
   auto read_var_name = [&](int32_t Slot) -> GCstr* {
      if (Slot < 0 or Slot >= int32_t(func_state.varmap.size())) return nullptr;
      GCRef name_ref = func_state.var_get(Slot).name;
      if (gcrefu(name_ref) < VARNAME__MAX) return nullptr;
      return gco_to_string(gcref(name_ref));
   };

   switch (bc_op(ins)) {
      case BC_MOV:
         name = read_var_name(int32_t(bc_d(ins)));
         break;

      case BC_UGET:
         if (bc_d(ins) >= func_state.nuv) return 0;
         name = read_var_name(int32_t(func_state.uvmap[bc_d(ins)]));
         break;

      case BC_GGET:
         table_entry = lj_tab_getstr(func_state.kt, lj_str_newlit(lex_state.L, "pairs"));
         if (table_entry and tvhaskslot(table_entry) and tvkslot(table_entry) IS bc_d(ins)) return 1;
         table_entry = lj_tab_getstr(func_state.kt, lj_str_newlit(lex_state.L, "next"));
         if (table_entry and tvhaskslot(table_entry) and tvkslot(table_entry) IS bc_d(ins)) return 1;
         return 0;

      default:
         return 0;
   }

   if (not name) return 0;
   if (name->len IS 5 and !strcmp(strdata(name), "pairs")) return 1;
   if (name->len IS 4 and !strcmp(strdata(name), "next")) return 1;
   return 0;
}

//********************************************************************************************************************
// Detect if a generic for iterator expression is a direct variable access suitable for array specialisation.

[[nodiscard]] static int predict_array_iter(FuncState &func_state, BCPos pc)
{
   BCIns ins = func_state.bcbase[pc].ins;
   BCOp op = bc_op(ins);

   // The array type check is performed at runtime by BC_ISARR; this pass only verifies that the iterator
   // expression is sourced from a direct variable load. BC_MOV, BC_UGET and BC_GGET load variables from locals,
   // upvalues and globals respectively.

   return ((op IS BC_MOV) or (op IS BC_UGET) or (op IS BC_GGET)) ? 1 : 0;
}

//********************************************************************************************************************
// Release registers held by an indexed expression's base and key after they are no longer needed.

static void release_indexed_original(FuncState &func_state, const ExpDesc &original)
{
   if (original.k IS ExpKind::Indexed) {
      RegisterAllocator allocator(&func_state);
      uint32_t orig_aux = original.u.s.aux;
      if (is_register_key(orig_aux)) allocator.release_register(BCReg(orig_aux));
      allocator.release_register(BCReg(original.u.s.info));
   }
}

// Get a pointer to the bytecode instruction referenced by an expression descriptor.

[[nodiscard]] static BCIns* ir_bcptr(FuncState* func_state, const ExpDesc* expression)
{
   return &func_state->bcbase[expression->u.s.info].ins;
}

struct FalseyJumpOptions {
   bool include_false = true;
   bool include_zero = true;
   bool include_empty_string = true;
   bool include_empty_array = false;
};

static ControlFlowEdge emit_falsey_jumps(
   FuncState &State, LexState &Lexer, ControlFlowGraph &Graph, BCREG Register, FalseyJumpOptions Options)
{
   ControlFlowEdge edge = Graph.make_unconditional();

   ExpDesc nilv(ExpKind::Nil);
   bcemit_INS(&State, BCINS_AD(BC_ISEQP, Register, const_pri(&nilv)));
   edge.append(BCPos(bcemit_jmp(&State)));

   if (Options.include_false) {
      ExpDesc falsev(ExpKind::False);
      bcemit_INS(&State, BCINS_AD(BC_ISEQP, Register, const_pri(&falsev)));
      edge.append(BCPos(bcemit_jmp(&State)));
   }

   if (Options.include_zero) {
      ExpDesc zerov(0.0);
      bcemit_INS(&State, BCINS_AD(BC_ISEQN, Register, const_num(&State, &zerov)));
      edge.append(BCPos(bcemit_jmp(&State)));
   }

   if (Options.include_empty_string) {
      ExpDesc emptyv(Lexer.intern_empty_string());
      bcemit_INS(&State, BCINS_AD(BC_ISEQS, Register, const_str(&State, &emptyv)));
      edge.append(BCPos(bcemit_jmp(&State)));
   }

   if (Options.include_empty_array) {
      bcemit_INS(&State, BCINS_AD(BC_ISEMPTYARR, Register, 0));
      edge.append(BCPos(bcemit_jmp(&State)));
   }

   return edge;
}

static void emit_bit_function_lookup(FuncState &State, RegisterAllocator &Allocator, std::string_view Name, BCReg Base)
{
   ExpDesc callee, key;
   callee.init(ExpKind::Global, 0);
   callee.u.sval = State.ls->keepstr("bit");

   ExpressionValue callee_value(&State, callee);
   callee_value.to_reg(Allocator, Base);
   callee = callee_value.legacy();

   key.init(ExpKind::Str, 0);
   key.u.sval = State.ls->keepstr(Name);
   expr_index(&State, &callee, &key);

   ExpressionValue callee_indexed(&State, callee);
   callee_indexed.to_reg(Allocator, Base);
}

IrEmitter::IrEmitter(ParserContext& context)
   : ctx(context),
     func_state(context.func()),
     lex_state(context.lex()),
     register_allocator(&this->func_state),
     control_flow(&this->func_state),
     operator_emitter(&this->func_state, &this->register_allocator, &this->control_flow)
{
}

void IrEmitter::apply_inferred_local_type(BCReg Slot, const ExprNode& Value)
{
   InferredTypeInfo inferred = infer_expression_type_ext(Value);
   if (inferred.type IS TiriType::Unknown or inferred.type IS TiriType::Any or inferred.type IS TiriType::Nil) return;

   VarInfo *info = &this->func_state.var_get(Slot.raw());
   info->fixed_type = inferred.type;
   info->object_class_id = (inferred.type IS TiriType::Object) ? inferred.object_class_id : CLASSID::NIL;
}

BCReg IrEmitter::finalise_pending_local_assignment(PreparedAssignment& Target)
{
   if (not Target.needs_var_add or not Target.pending_symbol) return BCReg(NO_REG);

   this->lex_state.var_new(BCReg(0), Target.pending_symbol, Target.pending_line, Target.pending_column);
   this->lex_state.var_add(BCReg(1));

   BCReg slot = BCReg(this->func_state.varmap.size() - 1);
   Target.storage.init(ExpKind::Local, slot);
   Target.storage.u.s.aux = this->func_state.varmap[slot.raw()];
   Target.needs_var_add = false;
   Target.newly_created = false;

   this->update_local_binding(Target.pending_symbol, slot);
   return slot;
}

IrEmitter::LoopStackGuard IrEmitter::push_loop_context(BCPos continue_target)
{
   LoopContext loop_context{};
   loop_context.break_edge = this->control_flow.make_break_edge();
   loop_context.continue_edge = this->control_flow.make_continue_edge();
   loop_context.defer_base = this->func_state.active_var_count();
   loop_context.continue_target = continue_target;
   loop_context.try_depth_at_entry = this->func_state.try_depth;  // Track try depth at loop entry
   this->loop_stack.push_back(loop_context);
   return LoopStackGuard(this);
}

//********************************************************************************************************************
// Emit bytecode for a complete function chunk, the top-level code block of a function.

ParserResult<IrEmitUnit> IrEmitter::emit_chunk(const BlockStmt& chunk)
{
   this->control_flow.reset(&this->func_state);
   FuncScope chunk_scope;
   ScopeGuard guard(&this->func_state, &chunk_scope, FuncScopeFlag::None);
   auto result = this->emit_block(chunk, FuncScopeFlag::None);
   if (not result.ok()) return result;
   this->control_flow.finalize();

   if (glPrintMsg) {
      // Verify no register leaks at function exit
      RegisterAllocator verifier(&this->func_state);
      verifier.verify_no_leaks("function exit");
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

// Emit bytecode for a block statement, creating a new scope with the specified flags.

ParserResult<IrEmitUnit> IrEmitter::emit_block(const BlockStmt& block, FuncScopeFlag flags)
{
   return this->emit_block_with_bindings(block, flags, std::span<const BlockBinding>());
}

//********************************************************************************************************************
// Emit bytecode for a block statement with pre-existing local bindings (used for loops and function parameters).

ParserResult<IrEmitUnit> IrEmitter::emit_block_with_bindings(
   const BlockStmt& block, FuncScopeFlag flags, std::span<const BlockBinding> bindings)
{
   FuncScope scope;
   ScopeGuard guard(&this->func_state, &scope, flags);
   LocalBindingScope binding_scope(this->binding_table);
   for (const BlockBinding& binding : bindings) {
      if (binding.symbol) this->update_local_binding(binding.symbol, binding.slot);
   }
   for (const StmtNode& stmt : block.view()) {
      auto status = this->emit_statement(stmt);
      if (not status.ok()) return status;
      this->ensure_register_balance(describe_node_kind(stmt.kind));
   }
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Dispatch statement emission to the appropriate handler based on the AST node kind.

ParserResult<IrEmitUnit> IrEmitter::emit_statement(const StmtNode& stmt)
{
   // Update lexer's last line so bytecode emission uses correct line numbers
   this->lex_state.lastline = stmt.span.line;

   switch (stmt.kind) {
   case AstNodeKind::ExpressionStmt: {
      const auto &payload = std::get<ExpressionStmtPayload>(stmt.data);
      return this->emit_expression_stmt(payload);
   }
   case AstNodeKind::ReturnStmt: {
      const auto &payload = std::get<ReturnStmtPayload>(stmt.data);
      return this->emit_return_stmt(payload);
   }
   case AstNodeKind::LocalDeclStmt: {
      const auto &payload = std::get<LocalDeclStmtPayload>(stmt.data);
      return this->emit_local_decl_stmt(payload);
   }
   case AstNodeKind::GlobalDeclStmt: {
      const auto &payload = std::get<GlobalDeclStmtPayload>(stmt.data);
      return this->emit_global_decl_stmt(payload);
   }
   case AstNodeKind::LocalFunctionStmt: {
      const auto &payload = std::get<LocalFunctionStmtPayload>(stmt.data);
      return this->emit_local_function_stmt(payload);
   }
   case AstNodeKind::FunctionStmt: {
      const auto &payload = std::get<FunctionStmtPayload>(stmt.data);
      return this->emit_function_stmt(payload);
   }
   case AstNodeKind::AssignmentStmt: {
      const auto &payload = std::get<AssignmentStmtPayload>(stmt.data);
      return this->emit_assignment_stmt(payload);
   }
   case AstNodeKind::IfStmt: {
      const auto &payload = std::get<IfStmtPayload>(stmt.data);
      return this->emit_if_stmt(payload);
   }
   case AstNodeKind::WhileStmt: {
      const auto &payload = std::get<LoopStmtPayload>(stmt.data);
      return this->emit_while_stmt(payload);
   }
   case AstNodeKind::RepeatStmt: {
      const auto &payload = std::get<LoopStmtPayload>(stmt.data);
      return this->emit_repeat_stmt(payload);
   }
   case AstNodeKind::NumericForStmt: {
      const auto &payload = std::get<NumericForStmtPayload>(stmt.data);
      return this->emit_numeric_for_stmt(payload);
   }
   case AstNodeKind::GenericForStmt: {
      const auto &payload = std::get<GenericForStmtPayload>(stmt.data);
      return this->emit_generic_for_stmt(payload);
   }
   case AstNodeKind::DeferStmt: {
      const auto &payload = std::get<DeferStmtPayload>(stmt.data);
      return this->emit_defer_stmt(payload);
   }
   case AstNodeKind::BreakStmt: {
      const auto &payload = std::get<BreakStmtPayload>(stmt.data);
      return this->emit_break_stmt(payload);
   }
   case AstNodeKind::ContinueStmt: {
      const auto &payload = std::get<ContinueStmtPayload>(stmt.data);
      return this->emit_continue_stmt(payload);
   }
   case AstNodeKind::DoStmt: {
      const auto &payload = std::get<DoStmtPayload>(stmt.data);
      if (payload.block) return this->emit_block(*payload.block, FuncScopeFlag::None);
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }
   case AstNodeKind::ConditionalShorthandStmt: {
      const auto &payload = std::get<ConditionalShorthandStmtPayload>(stmt.data);
      return this->emit_conditional_shorthand_stmt(payload);
   }
   case AstNodeKind::TryExceptStmt: {
      const auto &payload = std::get<TryExceptPayload>(stmt.data);
      return this->emit_try_except_stmt(payload);
   }
   case AstNodeKind::RaiseStmt: {
      const auto &payload = std::get<RaiseStmtPayload>(stmt.data);
      return this->emit_raise_stmt(payload, stmt.span);
   }
   case AstNodeKind::CheckStmt: {
      const auto &payload = std::get<CheckStmtPayload>(stmt.data);
      return this->emit_check_stmt(payload, stmt.span);
   }
   case AstNodeKind::ImportStmt: {
      const auto &payload = std::get<ImportStmtPayload>(stmt.data);
      return this->emit_import_stmt(payload);
   }
   case AstNodeKind::WithStmt: {
      const auto &payload = std::get<WithStmtPayload>(stmt.data);
      return this->emit_with_stmt(payload);
   }
   default:
      return this->unsupported_stmt(stmt.kind, stmt.span);
   }
}

//********************************************************************************************************************
// Emit bytecode for an expression statement, evaluating the expression and discarding its result.

ParserResult<IrEmitUnit> IrEmitter::emit_expression_stmt(const ExpressionStmtPayload &payload)
{
   if (not payload.expression) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   auto expression = this->emit_expression(*payload.expression);
   if (not expression.ok()) return ParserResult<IrEmitUnit>::failure(expression.error_ref());

   ExpDesc value = expression.value_ref();

   // We have a bare Unscoped identifier as an expression statement, this is an error - the user must explicitly
   // declare locals with 'local'.

   if (value.k IS ExpKind::Unscoped) {
      GCstr* name = value.u.sval;
      std::string msg = "undeclared variable '";
      msg += std::string_view(strdata(name), name->len);
      msg += "' - use 'local' to declare new variables";
      return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::UndefinedVariable, msg, payload.expression->span));
   }

   // For other expression statements, we need to ensure any bytecode emitted for the expression
   // doesn't clobber local variables. Using to_any_reg ensures Relocable expressions
   // (like GGET for global reads) get properly relocated to a register above varmap.size().

   RegisterAllocator allocator(&this->func_state);
   ExpressionValue expr_value(&this->func_state, value);
   expr_value.to_any_reg(allocator);
   value = expr_value.legacy();

   release_indexed_original(this->func_state, value);
   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a conditional shorthand statement (executes body only for falsey values like nil, false, 0, or empty string).

ParserResult<IrEmitUnit> IrEmitter::emit_conditional_shorthand_stmt(const ConditionalShorthandStmtPayload &Payload)
{
   if (not Payload.condition or not Payload.body) {
      return this->unsupported_stmt(AstNodeKind::ConditionalShorthandStmt, SourceSpan{});
   }

   auto condition_result = this->emit_expression(*Payload.condition);
   if (not condition_result.ok()) {
      return ParserResult<IrEmitUnit>::failure(condition_result.error_ref());
   }

   RegisterGuard register_guard(&this->func_state);
   RegisterAllocator allocator(&this->func_state);

   ExpDesc condition = condition_result.value_ref();
   ExpressionValue condition_value(&this->func_state, condition);
   auto cond_reg = condition_value.discharge_to_any_reg(allocator);

   FalseyJumpOptions options;
   options.include_empty_array = true;
   ControlFlowEdge falsey_edge = emit_falsey_jumps(
      this->func_state, this->lex_state, this->control_flow, cond_reg, options);

   ControlFlowEdge skip_body = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   BCPos body_start = BCPos(this->func_state.pc);
   falsey_edge.patch_to(body_start);

   auto body_result = this->emit_statement(*Payload.body);
   if (not body_result.ok()) return body_result;

   skip_body.patch_to(BCPos(this->func_state.pc));

   allocator.collapse_freereg(BCReg(cond_reg));
   register_guard.disarm();
   this->func_state.reset_freereg();

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a return statement, handling zero, single, or multiple return values.

ParserResult<IrEmitUnit> IrEmitter::emit_return_stmt(const ReturnStmtPayload &Payload)
{
   BCIns ins;
   this->func_state.flags |= PROTO_HAS_RETURN;

   // Check if function needs runtime type inference (no explicit return types declared)
   bool needs_typefix = true;
   for (size_t i = 0; i < this->func_state.return_types.size(); ++i) {
      if (this->func_state.return_types[i] != TiriType::Unknown) {
         needs_typefix = false;
         break;
      }
   }

   if (Payload.values.empty()) {
      ins = BCINS_AD(BC_RET0, 0, 1);
   }
   else {
      auto count = BCReg(0);
      auto list = this->emit_expression_list(Payload.values, count);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());

      ExpDesc last = list.value_ref();

      // Handle tail-call case: return f() or return f(...)
      if (count IS 1 and last.k IS ExpKind::Call) {
         BCIns* ip = ir_bcptr(&this->func_state, &last);
         if (this->func_state.try_depth > 0) {
            // DISABLE TAIL-CALL inside try blocks: use CALL + RET instead of CALLT.
            // CALLT doesn't return to the caller, so if the called function throws an exception, it happens after
            // TRYLEAVE has popped the try frame - the exception escapes.  By using CALL + RET, the exception occurs
            // while still inside the try block. This must be checked before any tail-call optimisation paths.

            setbc_b(ip, 0);  // Request all results (MULTRES)
            if (needs_typefix) bcemit_AD(&this->func_state, BC_TYPEFIX, last.u.s.aux, 1);
            ins = BCINS_AD(BC_RETM, this->func_state.varmap.size(), last.u.s.aux - this->func_state.varmap.size());
         }
         else if (bc_op(*ip) IS BC_VARG) {
            // Variadic return: return ...
            setbc_b(ir_bcptr(&this->func_state, &last), 0);
            // For VARG returns, we can't know count at compile time - skip typefix
            ins = BCINS_AD(BC_RETM, this->func_state.varmap.size(), last.u.s.aux - this->func_state.varmap.size());
         }
         else if (needs_typefix and bc_op(*ip) IS BC_CALL) {
            // DISABLE TAIL-CALL: emit BC_CALL + BC_TYPEFIX + BC_RET instead of BC_CALLT
            // This ensures BC_TYPEFIX runs for the return value.
            // Only apply to simple BC_CALL - not BC_CALLM (used by result filters) or other call types.
            bool has_closes = has_close_variables(&this->func_state);
            if (has_closes) {
               // With close handlers: Use fixed 1 result (B=2) because MULTRES can be corrupted
               // by close handlers that run between the call and return.
               setbc_b(ip, 2);
               bcemit_AD(&this->func_state, BC_TYPEFIX, last.u.s.aux, 1);
               ins = BCINS_AD(BC_RET1, last.u.s.aux, 2);
            }
            else {
               // No close handlers: Safe to use RETM with all results
               setbc_b(ip, 0);  // Request all results (MULTRES)
               bcemit_AD(&this->func_state, BC_TYPEFIX, last.u.s.aux, 1);
               ins = BCINS_AD(BC_RETM, this->func_state.varmap.size(), last.u.s.aux - this->func_state.varmap.size());
            }
         }
         else {
            // Normal tail-call for:
            // - Explicitly typed functions (needs_typefix=false)
            // - Special call types like BC_CALLM (result filters) where we can't safely modify
            // - Not inside a try block (handled by the try_depth check above)
            this->func_state.pc--;
            ins = BCINS_AD(bc_op(*ip) - BC_CALL + BC_CALLT, bc_a(*ip), bc_c(*ip));
         }
      }
      else if (count IS 1) {
         // Single non-call return value
         RegisterAllocator allocator(&this->func_state);
         ExpressionValue value(&this->func_state, last);
         auto reg = value.discharge_to_any_reg(allocator);
         if (needs_typefix) bcemit_AD(&this->func_state, BC_TYPEFIX, reg, 1);
         ins = BCINS_AD(BC_RET1, reg, 2);
      }
      else {
         // Multiple return values
         if (last.k IS ExpKind::Call) {
            setbc_b(ir_bcptr(&this->func_state, &last), 0);
            // Variadic tail - count unknown, skip typefix for safety
            ins = BCINS_AD(BC_RETM, this->func_state.varmap.size(), last.u.s.aux - this->func_state.varmap.size());
         }
         else {
            this->materialise_to_next_reg(last, "return tail value");
            if (needs_typefix) {
               auto typefix_count = std::min(count.raw(), BCREG(PROTO_MAX_RETURN_TYPES));
               bcemit_AD(&this->func_state, BC_TYPEFIX, this->func_state.varmap.size(), typefix_count);
            }
            ins = BCINS_AD(BC_RET, this->func_state.varmap.size(), count + 1);
         }
      }
   }

   snapshot_return_regs(&this->func_state, &ins);
   // Both __close and defer handlers must run before returning from function.
   // Order: closes before defers (LIFO - most recently declared runs first).
   execute_closes(&this->func_state, 0);
   execute_defers(&this->func_state, 0);

   // Emit BC_TRYLEAVE for each try scope we're exiting with this return.
   // A return from inside a try block must pop all pending try frames to prevent
   // stale frames accumulating on L->try_stack across multiple function calls.
   // Use varmap.size() (active local variable count) as the base register, not freereg,
   // because freereg may still point past temporary registers used by the return expression.

   if (this->func_state.try_depth > 0) {
      BCReg base_reg = BCReg(this->func_state.varmap.size());
      for (uint8_t i = 0; i < this->func_state.try_depth; ++i) {
         bcemit_AD(&this->func_state, BC_TRYLEAVE, base_reg, BCReg(0));
      }
   }

   if (this->func_state.flags & PROTO_CHILD) bcemit_AJ(&this->func_state, BC_UCLO, 0, 0);
   bcemit_INS(&this->func_state, ins);
   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a local variable declaration statement, allocating slots and initialising values.

ParserResult<IrEmitUnit> IrEmitter::emit_local_decl_stmt(const LocalDeclStmtPayload &Payload)
{
   auto nvars = BCReg(BCREG(Payload.names.size()));
   if (nvars IS 0) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   // For local declarations with ??= or ?=, since the variables are newly declared (undefined),
   // they are semantically empty/nil, so we just perform a plain assignment.
   // The ??= and ?= operators for local declarations are equivalent to plain = assignment.
   // However, we still enforce that ??= and ?= only support a single target variable for consistency.

   if ((Payload.op IS AssignmentOperator::IfEmpty or Payload.op IS AssignmentOperator::IfNil) and nvars != 1) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant,
         "conditional assignment (?=/?\?=) only supports a single target variable"));
   }

   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];
      GCstr* symbol = identifier.symbol;
      this->lex_state.var_new(i, is_blank_symbol(identifier) ? NAME_BLANK : symbol,
                              identifier.span.line, identifier.span.column);
   }

   ExpDesc tail;
   auto nexps = BCReg(0);
   if (Payload.values.empty()) tail = ExpDesc(ExpKind::Void);
   else {
      auto list = this->emit_expression_list(Payload.values, nexps);
      if (not list.ok()) return ParserResult<IrEmitUnit>::failure(list.error_ref());
      tail = list.value_ref();
   }

   this->lex_state.assign_adjust(nvars.raw(), nexps.raw(), &tail);
   this->lex_state.var_add(nvars);
   auto base = BCReg(this->func_state.varmap.size() - nvars.raw());

   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];
      if (not identifier.has_close) continue;

      // Check slot limit for closeslots bitmap (max 64 slots supported)
      uint8_t slot = uint8_t(base.raw() + i.raw());
      if (slot >= 64) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant,
            "too many local variables with <close> attribute (max 64 slots)"));
      }

      VarInfo* info = &this->func_state.var_get(base.raw() + i.raw());
      info->info |= VarInfoFlag::Close;
   }

   // Handle <const> attribute - mark local variables that cannot be reassigned
   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];
      if (not identifier.has_const) continue;

      // Validate: const requires initialiser
      if (i.raw() >= Payload.values.size()) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::ConstRequiresInitialiser,
            std::format("const local '{}' requires an initialiser",
               identifier.symbol ? std::string_view(strdata(identifier.symbol), identifier.symbol->len) : "_")));
      }

      VarInfo* info = &this->func_state.var_get(base.raw() + i.raw());
      info->info |= VarInfoFlag::Const;
   }

   // Set fixed_type for variables - explicit annotations take precedence, otherwise infer from initialisers
   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];
      VarInfo* info = &this->func_state.var_get(base.raw() + i.raw());

      if (identifier.type != TiriType::Unknown) {
         // Explicit type annotation takes precedence
         info->fixed_type = identifier.type;
      }
      else if (i.raw() < Payload.values.size()) {
         // No explicit annotation - infer type from initialiser expression
         // Note: Nil is excluded because it represents absence of value, not a type constraint
         this->apply_inferred_local_type(base + i, *Payload.values[i.raw()]);
      }
      // If no initialiser and no annotation, fixed_type remains Unknown (set in var_add)
   }

   for (auto i = BCReg(0); i < nvars; ++i) {
      const Identifier& identifier = Payload.names[i.raw()];
      if (is_blank_symbol(identifier)) continue;
      this->update_local_binding(identifier.symbol, BCReg(base.raw() + i.raw()));
   }
   this->func_state.reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for an if statement with one or more conditional clauses and an optional else clause.

ParserResult<IrEmitUnit> IrEmitter::emit_if_stmt(const IfStmtPayload &Payload)
{
   if (Payload.clauses.empty()) return ParserResult<IrEmitUnit>::success(IrEmitUnit{});

   ControlFlowEdge escapelist = this->control_flow.make_unconditional();
   for (size_t i = 0; i < Payload.clauses.size(); ++i) {
      const IfClause& clause = Payload.clauses[i];
      bool has_next = (i + 1) < Payload.clauses.size();
      if (clause.condition) {
         auto condexit_result = this->emit_condition_jump(*clause.condition);
         if (not condexit_result.ok()) return ParserResult<IrEmitUnit>::failure(condexit_result.error_ref());

         ControlFlowEdge condexit = condexit_result.value_ref();

         if (clause.block) {
            auto block_result = this->emit_block(*clause.block, FuncScopeFlag::None);
            if (not block_result.ok()) return block_result;
         }

         if (has_next) {
            escapelist.append(BCPos(bcemit_jmp(&this->func_state)));
            condexit.patch_here();
         }
         else escapelist.append(condexit);
      }
      else if (clause.block) {
         auto block_result = this->emit_block(*clause.block, FuncScopeFlag::None);
         if (not block_result.ok()) return block_result;
      }
   }

   escapelist.patch_here();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a while loop, evaluating the condition before each iteration.

ParserResult<IrEmitUnit> IrEmitter::emit_while_stmt(const LoopStmtPayload &Payload)
{
   if (Payload.style != LoopStyle::WhileLoop or not Payload.condition or not Payload.body) {
      return this->unsupported_stmt(AstNodeKind::WhileStmt, SourceSpan{});
   }

   FuncState* fs = &this->func_state;
   BCPos start = BCPos(fs->lasttarget = fs->pc);
   auto loop_stack_guard = this->push_loop_context(start);
   auto condexit_result = this->emit_condition_jump(*Payload.condition);
   if (not condexit_result.ok()) return ParserResult<IrEmitUnit>::failure(condexit_result.error_ref());

   ControlFlowEdge condexit = condexit_result.value_ref();
   ControlFlowEdge loop;

   {
      FuncScope loop_scope;
      ScopeGuard guard(fs, &loop_scope, FuncScopeFlag::Loop);
      loop = this->control_flow.make_unconditional(BCPos(bcemit_AD(fs, BC_LOOP, fs->varmap.size(), 0)));
      auto block_result = this->emit_block(*Payload.body, FuncScopeFlag::None);
      if (not block_result.ok()) {
         return block_result;
      }
      ControlFlowEdge body_jump = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
      body_jump.patch_to(BCPos(start));
   }

   condexit.patch_here();
   loop.patch_head(fs->current_pc());

   this->loop_stack.back().continue_edge.patch_to(start);
   this->loop_stack.back().break_edge.patch_here();

   if (glPrintMsg) { // Verify no register leaks at loop exit
      RegisterAllocator verifier(fs);
      verifier.verify_no_leaks("while loop exit");
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a repeat-until loop, executing the body at least once before testing the condition.

ParserResult<IrEmitUnit> IrEmitter::emit_repeat_stmt(const LoopStmtPayload &Payload)
{
   if (Payload.style != LoopStyle::RepeatUntil or not Payload.condition or not Payload.body) {
      return this->unsupported_stmt(AstNodeKind::RepeatStmt, SourceSpan{});
   }

   FuncState* fs = &this->func_state;
   BCPos loop = BCPos(fs->lasttarget = fs->pc);
   BCPos iter = BCPos(NO_JMP);
   ControlFlowEdge condexit;
   bool inner_has_upvals = false;

   auto loop_stack_guard = this->push_loop_context(loop);

   FuncScope outer_scope;
   ScopeGuard loop_guard(fs, &outer_scope, FuncScopeFlag::Loop);
   {
      FuncScope inner_scope;
      ScopeGuard inner_guard(fs, &inner_scope, FuncScopeFlag::None);
      bcemit_AD(fs, BC_LOOP, fs->varmap.size(), 0);
      auto block_result = this->emit_block(*Payload.body, FuncScopeFlag::None);
      if (not block_result.ok()) return block_result;

      iter = fs->current_pc();
      auto cond_result = this->emit_condition_jump(*Payload.condition);
      if (not cond_result.ok()) return ParserResult<IrEmitUnit>::failure(cond_result.error_ref());

      condexit = cond_result.value_ref();
      inner_has_upvals = has_flag(inner_scope.flags, FuncScopeFlag::Upvalue);
      if (inner_has_upvals) {
         auto break_result = this->emit_break_stmt(BreakStmtPayload{});
         if (not break_result.ok()) return break_result;
         condexit.patch_here();
      }
   }

   if (inner_has_upvals) condexit = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

   condexit.patch_to(BCPos(loop));
   ControlFlowEdge loop_head = this->control_flow.make_unconditional(BCPos(loop));
   loop_head.patch_head(fs->current_pc());

   this->loop_stack.back().continue_target = iter;
   this->loop_stack.back().continue_edge.patch_to(BCPos(iter));
   this->loop_stack.back().break_edge.patch_here();

   if (glPrintMsg) {
      // Verify no register leaks at loop exit
      RegisterAllocator verifier(fs);
      verifier.verify_no_leaks("repeat loop exit");
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a numeric for loop with start, stop, and optional step values.

ParserResult<IrEmitUnit> IrEmitter::emit_numeric_for_stmt(const NumericForStmtPayload &Payload)
{
   if (not Payload.start or not Payload.stop or not Payload.body) {
      return this->unsupported_stmt(AstNodeKind::NumericForStmt, SourceSpan{});
   }

   FuncState* fs = &this->func_state;
   auto base = fs->free_reg();
   GCstr* control_symbol = Payload.control.symbol ? Payload.control.symbol : NAME_BLANK;

   FuncScope outer_scope;
   ScopeGuard loop_guard(fs, &outer_scope, FuncScopeFlag::Loop);

   this->lex_state.var_new_fixed(FORL_IDX, VARNAME_FOR_IDX);
   this->lex_state.var_new_fixed(FORL_STOP, VARNAME_FOR_STOP);
   this->lex_state.var_new_fixed(FORL_STEP, VARNAME_FOR_STEP);
   this->lex_state.var_new(FORL_EXT, control_symbol, Payload.control.span.line, Payload.control.span.column);

   auto start_expr = this->emit_expression(*Payload.start);
   if (not start_expr.ok()) return ParserResult<IrEmitUnit>::failure(start_expr.error_ref());

   ExpDesc start_value = start_expr.value_ref();
   this->materialise_to_next_reg(start_value, "numeric for start");

   auto stop_expr = this->emit_expression(*Payload.stop);
   if (not stop_expr.ok()) return ParserResult<IrEmitUnit>::failure(stop_expr.error_ref());

   ExpDesc stop_value = stop_expr.value_ref();
   this->materialise_to_next_reg(stop_value, "numeric for stop");

   if (Payload.step) {
      auto step_expr = this->emit_expression(*Payload.step);
      if (not step_expr.ok()) return ParserResult<IrEmitUnit>::failure(step_expr.error_ref());
      ExpDesc step_value = step_expr.value_ref();
      this->materialise_to_next_reg(step_value, "numeric for step");
   }
   else {
      RegisterAllocator allocator(fs);
      bcemit_AD(fs, BC_KSHORT, fs->freereg, 1);
      allocator.reserve(BCReg(1));
   }

   this->lex_state.var_add(3);

   auto loop_stack_guard = this->push_loop_context(BCPos(NO_JMP));

   ControlFlowEdge loop = this->control_flow.make_unconditional(BCPos(bcemit_AJ(fs, BC_FORI, base, NO_JMP)));

   {
      FuncScope visible_scope;
      ScopeGuard guard(fs, &visible_scope, FuncScopeFlag::None);
      this->lex_state.var_add(1);
      RegisterAllocator allocator(fs);
      allocator.reserve(BCReg(1));
      std::array<BlockBinding, 1> loop_bindings{};
      std::span<const BlockBinding> binding_span;
      if (Payload.control.symbol and not Payload.control.is_blank) {
         loop_bindings[0].symbol = Payload.control.symbol;
         loop_bindings[0].slot = BCReg(base.raw() + FORL_EXT);
         binding_span = std::span<const BlockBinding>(loop_bindings.data(), 1);
      }
      auto block_result = this->emit_block_with_bindings(*Payload.body, FuncScopeFlag::None, binding_span);
      if (not block_result.ok()) return block_result;
   }

   ControlFlowEdge loopend = this->control_flow.make_unconditional(BCPos(bcemit_AJ(fs, BC_FORL, base, NO_JMP)));
   fs->bcbase[loopend.head().raw()].line = BCLine::encode(this->lex_state.current_file_index, Payload.body->span.line.lineNumber());
   loopend.patch_head(BCPos(loop.head().raw() + 1));
   loop.patch_head(fs->current_pc());
   this->loop_stack.back().continue_target = loopend.head();
   this->loop_stack.back().continue_edge.patch_to(loopend.head());
   this->loop_stack.back().break_edge.patch_here();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a generic for loop using iterator functions (e.g., pairs, ipairs).

ParserResult<IrEmitUnit> IrEmitter::emit_generic_for_stmt(const GenericForStmtPayload &Payload)
{
   if (Payload.names.empty() or Payload.iterators.empty() or not Payload.body) {
      return this->unsupported_stmt(AstNodeKind::GenericForStmt, SourceSpan{});
   }

   FuncState* fs = &this->func_state;
   auto base = BCReg(fs->freereg + 3);
   auto nvars = BCReg(0);

   FuncScope outer_scope;
   ScopeGuard loop_guard(fs, &outer_scope, FuncScopeFlag::Loop);

   this->lex_state.var_new_fixed(nvars++, VARNAME_FOR_GEN);
   this->lex_state.var_new_fixed(nvars++, VARNAME_FOR_STATE);
   this->lex_state.var_new_fixed(nvars++, VARNAME_FOR_CTL);

   for (const Identifier& identifier : Payload.names) {
      GCstr* symbol = identifier.symbol ? identifier.symbol : NAME_BLANK;
      this->lex_state.var_new(nvars++, symbol, identifier.span.line, identifier.span.column);
   }

   BCPos exprpc = fs->current_pc();
   auto iterator_count = BCReg(0);
   auto iter_values = this->emit_expression_list(Payload.iterators, iterator_count);
   if (not iter_values.ok()) return ParserResult<IrEmitUnit>::failure(iter_values.error_ref());

   ExpDesc tail = iter_values.value_ref();
   this->lex_state.assign_adjust(3, iterator_count.raw(), &tail);

   bcreg_bump(fs, 3  + 1);
   int isnext = (nvars <= 5) ? predict_next(this->lex_state, *fs, exprpc) : 0;
   int isarr = 0;
   this->lex_state.var_add(3);

   // Array iteration prediction is mutually exclusive with the 'next' optimisation.
   // Only attempt array prediction when the 'next' optimisation is not selected.

   if ((isnext IS 0) and iterator_count IS BCREG(1) and nvars <= 5) {
      isarr = predict_array_iter(*fs, exprpc);
   }

   auto loop_stack_guard = this->push_loop_context(BCPos(NO_JMP));

   ControlFlowEdge loop = this->control_flow.make_unconditional(
      BCPos(bcemit_AJ(fs, isnext ? BC_ISNEXT : (isarr ? BC_ISARR : BC_JMP), base, NO_JMP)));

   {
      FuncScope visible_scope;
      ScopeGuard guard(fs, &visible_scope, FuncScopeFlag::None);
      auto visible = BCReg(nvars.raw() - 3);
      this->lex_state.var_add(visible);
      RegisterAllocator allocator(fs);
      allocator.reserve(visible);
      std::vector<BlockBinding> loop_bindings;
      loop_bindings.reserve(visible.raw());
      for (auto i = BCReg(0); i < visible; ++i) {
         const Identifier& identifier = Payload.names[i.raw()];
         if (identifier.symbol and not identifier.is_blank) {
            BlockBinding binding;
            binding.symbol = identifier.symbol;
            binding.slot = BCReg(base.raw() + i.raw());
            loop_bindings.push_back(binding);
         }
      }
      std::span<const BlockBinding> binding_span(loop_bindings.data(), loop_bindings.size());
      auto block_result = this->emit_block_with_bindings(*Payload.body, FuncScopeFlag::None, binding_span);
      if (not block_result.ok()) {
         return block_result;
      }
   }

   loop.patch_head(fs->current_pc());
   BCPos iter = BCPos(bcemit_ABC(fs, isnext ? BC_ITERN : isarr ? BC_ITERA : BC_ITERC, base, nvars - BCREG(3) + BCREG(1), 3));
   ControlFlowEdge loopend = this->control_flow.make_unconditional(BCPos(bcemit_AJ(fs, BC_ITERL, base, NO_JMP)));
   BCLine encoded_body_line = BCLine::encode(this->lex_state.current_file_index, Payload.body->span.line.lineNumber());
   fs->bcbase[loopend.head().raw() - 1].line = encoded_body_line;
   fs->bcbase[loopend.head().raw()].line = encoded_body_line;
   loopend.patch_head(BCPos(loop.head().raw() + 1));
   this->loop_stack.back().continue_target = iter;
   this->loop_stack.back().continue_edge.patch_to(iter);
   this->loop_stack.back().break_edge.patch_here();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a defer statement, registering a function to execute when the current scope exits.

ParserResult<IrEmitUnit> IrEmitter::emit_defer_stmt(const DeferStmtPayload &Payload)
{
   if (not Payload.callable) return this->unsupported_stmt(AstNodeKind::DeferStmt, SourceSpan{});

   FuncState* fs = &this->func_state;
   auto reg = fs->free_reg();
   this->lex_state.var_new(0, NAME_BLANK);
   RegisterAllocator allocator(fs);
   allocator.reserve(BCReg(1));
   this->lex_state.var_add(1);
   VarInfo* info = &fs->var_get(fs->varmap.size() - 1);
   info->info |= VarInfoFlag::Defer;

   auto function_value = this->emit_function_expr(*Payload.callable);
   if (not function_value.ok()) return ParserResult<IrEmitUnit>::failure(function_value.error_ref());

   ExpDesc fn = function_value.value_ref();
   this->materialise_to_reg(fn, reg, "defer callable");

   auto nargs = BCReg(0);
   for (const ExprNodePtr& argument : Payload.arguments) {
      if (not argument) continue;

      auto arg_expr = this->emit_expression(*argument);
      if (not arg_expr.ok()) return ParserResult<IrEmitUnit>::failure(arg_expr.error_ref());

      ExpDesc arg = arg_expr.value_ref();
      this->materialise_to_next_reg(arg, "defer argument");
      nargs++;
   }

   if (nargs > 0) {
      for (auto i = BCReg(0); i < nargs; ++i) {
         this->lex_state.var_new(i, NAME_BLANK);
      }

      this->lex_state.var_add(nargs);

      for (auto i = BCReg(0); i < nargs; ++i) {
         VarInfo* arg_info = &fs->var_get(fs->varmap.size() - nargs.raw() + i.raw());
         arg_info->info |= VarInfoFlag::DeferArg;
      }
   }

   fs->reset_freereg();
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a `with` statement: with obj1, obj2 do ... end
//
// For each object expression:
//   1. Evaluate to a register
//   2. Store in a hidden local slot flagged with VarInfoFlag::Close
//   3. Emit bytecode to call obj.__lock(obj) to acquire the lock
// The scope exit then automatically calls __close (= release_object) via the <close> machinery.

ParserResult<IrEmitUnit> IrEmitter::emit_with_stmt(const WithStmtPayload &Payload)
{
   FuncState *fs = &this->func_state;
   LexState *ls = &this->lex_state;

   // Open a new scope for the with block
   FuncScope scope;
   ScopeGuard guard(fs, &scope, FuncScopeFlag::None);
   LocalBindingScope binding_scope(this->binding_table);

   auto obj_count = BCReg(BCREG(Payload.objects.size()));
   if (obj_count IS 0) {
      // Empty with - just emit the body
      if (Payload.block) {
         for (const StmtNode &stmt : Payload.block->view()) {
            auto status = this->emit_statement(stmt);
            if (not status.ok()) return status;
            this->ensure_register_balance(describe_node_kind(stmt.kind));
         }
      }
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   // Evaluate each object expression and store in <close> locals

   for (auto i = BCReg(0); i < obj_count; ++i) {
      const ExprNodePtr &obj_expr = Payload.objects[i.raw()];
      if (not obj_expr) continue;

      // Declare a hidden local variable for this object
      ls->var_new(0, NAME_BLANK);

      // Evaluate the expression
      auto expr_result = this->emit_expression(*obj_expr);
      if (not expr_result.ok()) return ParserResult<IrEmitUnit>::failure(expr_result.error_ref());

      ExpDesc obj_value = expr_result.value_ref();
      auto obj_reg = fs->free_reg();

      // Materialise to the next register (the local variable slot)
      this->materialise_to_reg(obj_value, obj_reg, "with object");

      RegisterAllocator allocator(fs);
      allocator.reserve(BCReg(1));  // Reserve the local slot
      ls->var_add(1);

      // Mark the local as <close> so scope exit calls __close
      uint8_t slot = uint8_t(obj_reg.raw());
      if (slot >= 64) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant,
            "too many local variables with <close> attribute (max 64 slots)"));
      }

      VarInfo *info = &fs->var_get(fs->varmap.size() - 1);
      info->info |= VarInfoFlag::Close;

      // Emit bytecode to call __lock(obj) to acquire the lock
      // Pattern: getmetatable(obj) -> get __lock field -> call __lock(obj)

      BCREG base = fs->freereg;
      allocator.reserve(BCReg(4 + LJ_FR2));

      // Load getmetatable global
      ExpDesc getmt;
      getmt.init(ExpKind::Global, 0);
      getmt.u.sval = ls->keepstr("getmetatable");
      expr_toreg(fs, &getmt, base);

      // Copy object to argument position
      bcemit_AD(fs, BC_MOV, base + 1 + LJ_FR2, obj_reg);

      // Call getmetatable(obj) -> metatable in base
      bcemit_ABC(fs, BC_CALL, base, 2, 2);  // 1 result, 1 arg

      // Get __lock from metatable
      GCstr *lock_str = ls->keepstr("__lock");
      ExpDesc key;
      key.init(ExpKind::Str, 0);
      key.u.sval = lock_str;
      BCREG lock_fn_reg = base + 1;
      bcemit_tgets(fs, lock_fn_reg, base, const_str(fs, &key));

      // Call __lock(obj)
      BCREG call_base = base;
      bcemit_AD(fs, BC_MOV, call_base, lock_fn_reg);
      bcemit_AD(fs, BC_MOV, call_base + 1 + LJ_FR2, obj_reg);
      bcemit_ABC(fs, BC_CALL, call_base, 1, 2);  // 0 results, 1 arg

      fs->reset_freereg(); // Release the temporary registers
   }

   // Emit the body block

   if (Payload.block) {
      for (const StmtNode &stmt : Payload.block->view()) {
         auto status = this->emit_statement(stmt);
         if (not status.ok()) return status;
         this->ensure_register_balance(describe_node_kind(stmt.kind));
      }
   }

   // ScopeGuard destructor calls fscope_end -> execute_closes -> bcemit_close for each <close> local
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a break statement, exiting the innermost loop after executing close and defer handlers.

ParserResult<IrEmitUnit> IrEmitter::emit_break_stmt(const BreakStmtPayload&)
{
   if (this->loop_stack.empty()) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant, "break outside loop"));
   }

   LoopContext& loop = this->loop_stack.back();

   // Both __close and defer handlers must run when jumping out of scope via break.
   // Order: closes before defers (LIFO - most recently declared runs first).

   execute_closes(&this->func_state, loop.defer_base);
   execute_defers(&this->func_state, loop.defer_base);

   // Emit BC_TRYLEAVE for each try scope we're exiting with this break.
   // The try_depth tracks how deep we are in try blocks; when breaking out of
   // a loop that's inside a try, we need to pop the try frame(s).

   if (this->func_state.try_depth > loop.try_depth_at_entry) {
      uint8_t leave_count = this->func_state.try_depth - loop.try_depth_at_entry;
      BCReg base_reg = BCReg(this->func_state.freereg);
      for (uint8_t i = 0; i < leave_count; ++i) {
         bcemit_AD(&this->func_state, BC_TRYLEAVE, base_reg, BCReg(0));
      }
   }

   loop.break_edge.append(BCPos(bcemit_jmp(&this->func_state)));
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for a continue statement, jumping to the next iteration after executing close and defer handlers.

ParserResult<IrEmitUnit> IrEmitter::emit_continue_stmt(const ContinueStmtPayload&)
{
   if (this->loop_stack.empty()) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant, "continue outside loop"));
   }

   LoopContext& loop = this->loop_stack.back();

   // Both __close and defer handlers must run when jumping out of scope via continue.
   // Order: closes before defers (LIFO - most recently declared runs first).

   execute_closes(&this->func_state, loop.defer_base);
   execute_defers(&this->func_state, loop.defer_base);

   // Emit BC_TRYLEAVE for each try scope we're exiting with this continue.
   // The try_depth tracks how deep we are in try blocks; when continuing
   // inside a loop that's inside a try, we need to pop the try frame(s).

   if (this->func_state.try_depth > loop.try_depth_at_entry) {
      uint8_t leave_count = this->func_state.try_depth - loop.try_depth_at_entry;
      BCReg base_reg = BCReg(this->func_state.freereg);
      for (uint8_t i = 0; i < leave_count; ++i) {
         bcemit_AD(&this->func_state, BC_TRYLEAVE, base_reg, BCReg(0));
      }
   }

   loop.continue_edge.append(BCPos(bcemit_jmp(&this->func_state)));
   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for an assignment statement, dispatching to plain, compound, or if-empty assignment handlers.

ParserResult<IrEmitUnit> IrEmitter::emit_assignment_stmt(const AssignmentStmtPayload &Payload)
{
   if (Payload.targets.empty()) return this->unsupported_stmt(AstNodeKind::AssignmentStmt, SourceSpan{});

   bool has_safe_nav_target = false;
   for (const ExprNodePtr& node : Payload.targets) {
      if (node and contains_safe_nav_target(*node)) {
         has_safe_nav_target = true;
         break;
      }
   }

   // For compound assignments (+=, -=, etc.), do NOT create new locals for unscoped variables.
   // The variable must already exist - we should modify the existing storage.
   // For plain (=) and if-empty/if-nil (?=/??=) assignments, allow new local creation.
   // If-empty/if-nil on an undeclared variable creates a local and assigns (since undefined is empty/nil).

   bool AllocNewLocal = (Payload.op IS AssignmentOperator::Plain or Payload.op IS AssignmentOperator::IfEmpty or Payload.op IS AssignmentOperator::IfNil);

   if (has_safe_nav_target and Payload.targets.size() > 1) {
      SourceSpan span = SourceSpan{};
      for (const ExprNodePtr& node : Payload.targets) {
         if (node and contains_safe_nav_target(*node)) {
            span = node->span;
            break;
         }
      }

      return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InvalidAssignment,
         "safe-navigation assignment targets cannot be used in multi-assignment", span));
   }

   auto targets_result = this->prepare_assignment_targets(Payload.targets, AllocNewLocal, has_safe_nav_target);
   if (not targets_result.ok()) return ParserResult<IrEmitUnit>::failure(targets_result.error_ref());

   std::vector<PreparedAssignment> targets = std::move(targets_result.value_ref());

   if (Payload.op IS AssignmentOperator::Plain) return this->emit_plain_assignment(std::move(targets), Payload.values);

   if (targets.size() != 1) {
      const ExprNodePtr& node = Payload.targets.front();
      const ExprNode* raw = node ? node.get() : nullptr;
      SourceSpan span = raw ? raw->span : SourceSpan{};
      return this->unsupported_stmt(AstNodeKind::AssignmentStmt, span);
   }

   PreparedAssignment target = std::move(targets.front());

   if (Payload.op IS AssignmentOperator::IfEmpty) {
      return this->emit_if_empty_assignment(std::move(target), Payload.values);
   }

   if (Payload.op IS AssignmentOperator::IfNil) {
      return this->emit_if_nil_assignment(std::move(target), Payload.values);
   }

   return this->emit_compound_assignment(Payload.op, std::move(target), Payload.values);
}

//********************************************************************************************************************
// Dispatch expression emission to the appropriate handler based on the AST node kind.

ParserResult<ExpDesc> IrEmitter::emit_expression(const ExprNode& expr)
{
   // Update lexer's last line so bytecode emission uses correct line numbers.
   // Complex expressions (like calls) may save/restore this to ensure their
   // final instruction gets the correct line.

   this->lex_state.lastline = expr.span.line;

   switch (expr.kind) {
      case AstNodeKind::LiteralExpr:      return this->emit_literal_expr(std::get<LiteralValue>(expr.data));
      case AstNodeKind::IdentifierExpr:   return this->emit_identifier_expr(std::get<NameRef>(expr.data));
      case AstNodeKind::VarArgExpr:       return this->emit_vararg_expr();
      case AstNodeKind::UnaryExpr:        return this->emit_unary_expr(std::get<UnaryExprPayload>(expr.data));
      case AstNodeKind::UpdateExpr:       return this->emit_update_expr(std::get<UpdateExprPayload>(expr.data));
      case AstNodeKind::BinaryExpr:       return this->emit_binary_expr(std::get<BinaryExprPayload>(expr.data));
      case AstNodeKind::ComparisonChainExpr:
         return this->emit_comparison_chain_expr(std::get<ComparisonChainExprPayload>(expr.data));
      case AstNodeKind::TernaryExpr:      return this->emit_ternary_expr(std::get<TernaryExprPayload>(expr.data));
      case AstNodeKind::PresenceExpr:     return this->emit_presence_expr(std::get<PresenceExprPayload>(expr.data));
      case AstNodeKind::PipeExpr:         return this->emit_pipe_expr(std::get<PipeExprPayload>(expr.data));
      case AstNodeKind::MemberExpr:       return this->emit_member_expr(std::get<MemberExprPayload>(expr.data));
      case AstNodeKind::IndexExpr:        return this->emit_index_expr(std::get<IndexExprPayload>(expr.data));
      case AstNodeKind::SafeMemberExpr:   return this->emit_safe_member_expr(std::get<SafeMemberExprPayload>(expr.data));
      case AstNodeKind::SafeIndexExpr:    return this->emit_safe_index_expr(std::get<SafeIndexExprPayload>(expr.data));
      case AstNodeKind::SafeCallExpr:     return this->emit_safe_call_expr(std::get<CallExprPayload>(expr.data));
      case AstNodeKind::CallExpr:         return this->emit_call_expr(std::get<CallExprPayload>(expr.data));
      case AstNodeKind::ResultFilterExpr: return this->emit_result_filter_expr(std::get<ResultFilterPayload>(expr.data));
      case AstNodeKind::TableExpr:        return this->emit_table_expr(std::get<TableExprPayload>(expr.data));
      case AstNodeKind::RangeExpr:        return this->emit_range_expr(std::get<RangeExprPayload>(expr.data));
      case AstNodeKind::ChooseExpr:       return this->emit_choose_expr(std::get<ChooseExprPayload>(expr.data));
      case AstNodeKind::FunctionExpr:     return this->emit_function_expr(std::get<FunctionExprPayload>(expr.data));
      default: return this->unsupported_expr(expr.kind, expr.span);
   }
}

//********************************************************************************************************************
// Emit bytecode for a conditional expression with a jump on false (used in if/while statements).

ParserResult<ControlFlowEdge> IrEmitter::emit_condition_jump(const ExprNode& expr)
{
   auto condition = this->emit_expression(expr);
   if (not condition.ok()) return ParserResult<ControlFlowEdge>::failure(condition.error_ref());
   ExpDesc result = condition.value_ref();
   if (result.k IS ExpKind::Nil) result.k = ExpKind::False;
   bcemit_branch_t(&this->func_state, &result);

   // After processing the condition expression, reset freereg to varmap.size().  The condition has been fully evaluated
   // and emitted as a conditional jump - any temporary registers used during evaluation are no longer needed.

   this->func_state.reset_freereg();

   return ParserResult<ControlFlowEdge>::success(this->control_flow.make_false_edge(BCPos(result.f)));
}

//********************************************************************************************************************
// Emit bytecode for a literal expression (nil, boolean, number, string).

ParserResult<ExpDesc> IrEmitter::emit_literal_expr(const LiteralValue& literal)
{
   ExpDesc expr;
   switch (literal.kind) {
      case LiteralKind::Nil:     expr = ExpDesc(ExpKind::Nil); break;
      case LiteralKind::Boolean: expr = ExpDesc(literal.bool_value); break;
      case LiteralKind::Number:  expr = ExpDesc(literal.number_value); break;
      case LiteralKind::String:  expr = ExpDesc(literal.string_value); break;
   }
   return ParserResult<ExpDesc>::success(expr);
}

//********************************************************************************************************************
// Emit bytecode for an identifier expression, resolving the name to a local, upvalue, or global variable.

ParserResult<ExpDesc> IrEmitter::emit_identifier_expr(const NameRef& reference)
{
   // Blank identifiers cannot be read - they are only valid as assignment targets
   if (reference.identifier.is_blank) {
      return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::UnexpectedToken,
         "cannot read blank identifier '_'"));
   }

   // Check if this is a registered constant - substitute with literal value
   if (auto constant = lookup_constant(reference.identifier.symbol)) {
      ExpDesc expr(constant->to_number());
      return ParserResult<ExpDesc>::success(expr);
   }

   // Normal variable lookup
   ExpDesc resolved;
   this->lex_state.var_lookup_symbol(reference.identifier.symbol, &resolved);

   // If unscoped, check if this was explicitly declared as global
   if (resolved.k IS ExpKind::Unscoped) {
      if (this->func_state.declared_globals.count(reference.identifier.symbol) > 0) {
         resolved.k = ExpKind::Global;
      }
   }

   return ParserResult<ExpDesc>::success(resolved);
}

//********************************************************************************************************************
// Emit bytecode for a vararg expression (...), accessing variadic function arguments.

ParserResult<ExpDesc> IrEmitter::emit_vararg_expr()
{
   ExpDesc expr;
   RegisterAllocator allocator(&this->func_state);
   allocator.reserve(BCReg(1));
   auto base = BCReg(this->func_state.freereg) - BCREG(1);
   expr.init(ExpKind::Call, bcemit_ABC(&this->func_state, BC_VARG, base, 2, this->func_state.numparams));
   expr.u.s.aux = base;
   expr.flags |= ExprFlag::HasRhsReg;
   return ParserResult<ExpDesc>::success(expr);
}

//********************************************************************************************************************
// Emit bytecode for a unary expression (negation, not, length, or bitwise not).

ParserResult<ExpDesc> IrEmitter::emit_unary_expr(const UnaryExprPayload &Payload)
{
   if (not Payload.operand) return this->unsupported_expr(AstNodeKind::UnaryExpr, SourceSpan{});
   auto operand_result = this->emit_expression(*Payload.operand);
   if (not operand_result.ok()) return operand_result;
   ExpDesc operand = operand_result.value_ref();

   // Use OperatorEmitter facade for unary operators
   switch (Payload.op) {
      case AstUnaryOperator::Negate: this->operator_emitter.emit_unary(BC_UNM, ExprValue(&operand)); break;
      case AstUnaryOperator::Not: this->operator_emitter.emit_unary(BC_NOT, ExprValue(&operand)); break;
      case AstUnaryOperator::Length: this->operator_emitter.emit_unary(BC_LEN, ExprValue(&operand)); break;
      case AstUnaryOperator::BitNot:
         // BitNot calls bit.bnot library function
         this->operator_emitter.emit_bitnot(ExprValue(&operand));
         break;
   }
   return ParserResult<ExpDesc>::success(operand);
}

//********************************************************************************************************************
// Emit bytecode for an update expression (++, --), incrementing or decrementing a variable in place.

ParserResult<ExpDesc> IrEmitter::emit_update_expr(const UpdateExprPayload &Payload)
{
   if (not Payload.target) return this->unsupported_expr(AstNodeKind::UpdateExpr, SourceSpan{});

   // For update expressions, do not create a new local for unscoped variables.  The variable must already exist.

   auto target_result = this->emit_lvalue_expr(*Payload.target, false);
   if (not target_result.ok()) return target_result;
   ExpDesc target = target_result.value_ref();

   RegisterAllocator allocator(&this->func_state);

   // For indexed expressions, we need to duplicate table operands to avoid clobbering

   TableOperandCopies copies = allocator.duplicate_table_operands(target);
   ExpDesc working = copies.duplicated;

   BinOpr op = (Payload.op IS AstUpdateOperator::Increment) ? BinOpr::Add : BinOpr::Sub;

   // Discharge the value to a register for arithmetic
   ExpressionValue operand_value(&this->func_state, working);
   auto operand_reg = operand_value.discharge_to_any_reg(allocator);

   auto saved_reg = operand_reg;
   if (Payload.is_postfix) {
      saved_reg = BCReg(this->func_state.freereg);
      bcemit_AD(&this->func_state, BC_MOV, saved_reg, operand_reg);
      allocator.reserve(BCReg(1));
   }

   ExpDesc operand = operand_value.legacy();  // Get ExpDesc for subsequent operations
   ExpDesc delta(1.0);
   ExpDesc infix = operand;

   // Use OperatorEmitter for arithmetic operation (operand +/- 1)
   this->operator_emitter.emit_binary_arith(op, ExprValue(&infix), delta);

   bcemit_store(&this->func_state, &target, &infix);
   release_indexed_original(this->func_state, target);

   if (Payload.is_postfix) {
      allocator.collapse_freereg(BCReg(saved_reg));
      ExpDesc result;
      result.init(ExpKind::NonReloc, saved_reg);
      return ParserResult<ExpDesc>::success(result);
   }

   return ParserResult<ExpDesc>::success(infix);
}

//********************************************************************************************************************
// Emit bytecode for a binary expression (arithmetic, comparison, logical, bitwise, or concatenation operators).

ParserResult<ExpDesc> IrEmitter::emit_binary_expr(const BinaryExprPayload &Payload)
{
   auto lhs_result = this->emit_expression(*Payload.left);
   if (not lhs_result.ok()) return lhs_result;

   auto mapped = map_binary_operator(Payload.op);
   if (not mapped.has_value()) {
      SourceSpan span = Payload.left ? Payload.left->span : SourceSpan{};
      return this->unsupported_expr(AstNodeKind::BinaryExpr, span);
   }

   BinOpr opr = mapped.value();
   ExpDesc lhs = lhs_result.value_ref();

   // IF_EMPTY requires special handling - it must emit RHS conditionally like ternary
   // Cannot use the standard prepare/emit RHS/complete pattern

   if (opr IS BinOpr::IfEmpty) return this->emit_if_empty_expr(lhs, *Payload.right);

   // HasFlag operator: bit.band(lhs, rhs) != 0, producing a boolean result
   if (opr IS BinOpr::HasFlag) return this->emit_has_flag_expr(lhs, *Payload.right);

   // Bitwise operators need special handling to control bytecode order for JIT compatibility.
   // The JIT expects callee to be loaded BEFORE arguments, matching explicit bit.band() pattern.
   if (opr IS BinOpr::BitAnd or opr IS BinOpr::BitOr or opr IS BinOpr::BitXor or
       opr IS BinOpr::ShiftLeft or opr IS BinOpr::ShiftRight) {
      return this->emit_bitwise_expr(opr, lhs, *Payload.right);
   }

   // ALL binary operators need binop_left preparation before RHS evaluation

   // This discharges LHS to appropriate form to prevent register clobbering

   if (opr IS BinOpr::LogicalAnd) { // Logical AND: CFG-based short-circuit implementation
      this->operator_emitter.prepare_logical_and(ExprValue(&lhs));
   }
   else if (opr IS BinOpr::LogicalOr) { // Logical OR: CFG-based short-circuit implementation
      this->operator_emitter.prepare_logical_or(ExprValue(&lhs));
   }
   else if (opr IS BinOpr::Concat) { // CONCAT: Discharge to consecutive register for BC_CAT chaining
      this->operator_emitter.prepare_concat(ExprValue(&lhs));
   }
   else { // All other operators use OperatorEmitter facade
      this->operator_emitter.emit_binop_left(opr, ExprValue(&lhs));
   }

   // Now evaluate RHS (safe because binop_left prepared LHS)

   auto rhs_result = this->emit_expression(*Payload.right);
   if (not rhs_result.ok()) return rhs_result;
   ExpDesc rhs = rhs_result.value_ref();

   // Emit the actual operation based on operator type
   if (opr IS BinOpr::LogicalAnd) { // Logical AND: CFG-based short-circuit implementation
      this->operator_emitter.complete_logical_and(ExprValue(&lhs), rhs);
   }
   else if (opr IS BinOpr::LogicalOr) { // Logical OR: CFG-based short-circuit implementation
      this->operator_emitter.complete_logical_or(ExprValue(&lhs), rhs);
   }
   else if (is_comparison_op(opr)) { // Comparison operators (NE, EQ, LT, GE, LE, GT, APPROX)
      this->operator_emitter.emit_comparison(opr, ExprValue(&lhs), rhs);
   }
   else if (opr IS BinOpr::Concat) { // CONCAT: CFG-based implementation with BC_CAT chaining
      this->operator_emitter.complete_concat(ExprValue(&lhs), rhs);
   }
   else if (opr IS BinOpr::BitAnd or opr IS BinOpr::BitOr or opr IS BinOpr::BitXor or opr IS BinOpr::ShiftLeft or opr IS BinOpr::ShiftRight) {
      // Bitwise operators: Route through OperatorEmitter (emits bit.* library calls)
      this->operator_emitter.emit_binary_bitwise(opr, ExprValue(&lhs), rhs);
   }
   else { // Arithmetic operators (ADD, SUB, MUL, DIV, MOD, POW)
      this->operator_emitter.emit_binary_arith(opr, ExprValue(&lhs), rhs);
   }

   return ParserResult<ExpDesc>::success(lhs);
}

//********************************************************************************************************************
// Emit bytecode for ordering comparison chains such as `0 <= x < 10`.

ParserResult<ExpDesc> IrEmitter::emit_comparison_chain_expr(const ComparisonChainExprPayload &Payload)
{
   if (Payload.operands.size() < 2 or Payload.operators.size() + 1 != Payload.operands.size()) {
      SourceSpan span = Payload.operands.empty() ? SourceSpan{} : Payload.operands.front()->span;
      return this->unsupported_expr(AstNodeKind::ComparisonChainExpr, span);
   }

   auto lhs_result = this->emit_expression(*Payload.operands[0]);
   if (not lhs_result.ok()) return lhs_result;

   ExpDesc lhs = lhs_result.value_ref();
   ExpDesc result;
   std::vector<BCReg> preserved_regs;
   bool has_result = false;

   for (size_t index = 0; index < Payload.operators.size(); ++index) {
      if (has_result) this->operator_emitter.prepare_logical_and(ExprValue(&result));

      auto mapped = map_binary_operator(Payload.operators[index]);
      if (not mapped.has_value()) {
         SourceSpan span = Payload.operands[index] ? Payload.operands[index]->span : SourceSpan{};
         return this->unsupported_expr(AstNodeKind::ComparisonChainExpr, span);
      }

      BinOpr opr = mapped.value();
      this->operator_emitter.emit_binop_left(opr, ExprValue(&lhs));

      auto rhs_result = this->emit_expression(*Payload.operands[index + 1]);
      if (not rhs_result.ok()) return rhs_result;

      ExpDesc rhs = rhs_result.value_ref();
      bool preserve_rhs = index + 1 < Payload.operators.size();
      BCReg next_lhs_reg = BCReg(NO_REG);
      TiriType preserved_type = rhs.result_type;

      if (preserve_rhs) {
         this->register_allocator.discharge_to_next_register(rhs);
         BCReg comparison_rhs_reg = BCReg(rhs.u.s.info);
         preserved_regs.push_back(comparison_rhs_reg);

         next_lhs_reg = this->func_state.free_reg();
         this->register_allocator.reserve(BCReg(1));
         bcemit_AD(&this->func_state, BC_MOV, next_lhs_reg, comparison_rhs_reg);
         preserved_regs.push_back(next_lhs_reg);
      }

      ExpDesc comparison = lhs;
      this->operator_emitter.emit_comparison(opr, ExprValue(&comparison), rhs);

      if (has_result) this->operator_emitter.complete_logical_and(ExprValue(&result), comparison);
      else {
         result = comparison;
         has_result = true;
      }

      if (preserve_rhs) {
         lhs.init(ExpKind::NonReloc, next_lhs_reg.raw());
         lhs.result_type = preserved_type;
      }
   }

   for (auto reg = preserved_regs.rbegin(); reg != preserved_regs.rend(); ++reg) {
      this->register_allocator.release_register(*reg);
   }

   result.result_type = TiriType::Bool;
   return ParserResult<ExpDesc>::success(result);
}
//********************************************************************************************************************
// IF_EMPTY (lhs ?? rhs) with conditional RHS emission for proper short-circuit semantics
// Similar to ternary but with extended falsey checks (nil, false, 0, "")

ParserResult<ExpDesc> IrEmitter::emit_if_empty_expr(ExpDesc lhs, const ExprNode& rhs_ast)
{
   // Use RegisterGuard for automatic register cleanup on all exit paths (RAII)
   RegisterGuard register_guard(&this->func_state);

   RegisterAllocator allocator(&this->func_state);
   ExpressionValue lhs_value(&this->func_state, lhs);
   auto lhs_reg = lhs_value.discharge_to_any_reg(allocator);

   FalseyJumpOptions options;
   options.include_empty_array = true;
   ControlFlowEdge falsey_edge = emit_falsey_jumps(
      this->func_state, this->lex_state, this->control_flow, lhs_reg, options);

   // LHS is truthy - it's already in lhs_reg, just skip RHS
   ControlFlowEdge skip_rhs = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   // Patch falsey checks to jump here (RHS evaluation)

   BCPos rhs_start = BCPos(this->func_state.pc);
   falsey_edge.patch_to(rhs_start);

   // Emit RHS - only executed when LHS is falsey

   auto rhs_result = this->emit_expression(rhs_ast);
   if (not rhs_result.ok()) return rhs_result;
   ExpressionValue rhs_value(&this->func_state, rhs_result.value_ref());
   rhs_value.discharge();
   this->materialise_to_reg(rhs_value.legacy(), lhs_reg, "if_empty rhs");

   // Clean up any RHS temporaries, but preserve the result register

   allocator.collapse_freereg(BCReg(lhs_reg));

   // Patch skip jump to here (after RHS)

   skip_rhs.patch_to(BCPos(this->func_state.pc));

   // Preserve result register by adjusting what RegisterGuard will restore to
   // Only restore to saved_freereg if it's beyond the result register

   if (register_guard.saved() > BCReg(lhs_reg + 1)) register_guard.adopt_saved(register_guard.saved());
   else register_guard.disarm();  // Keep current freereg (lhs_reg + 1)

   // Result is in lhs_reg

   ExpDesc result;
   result.init(ExpKind::NonReloc, lhs_reg);
   return ParserResult<ExpDesc>::success(result);
}
//********************************************************************************************************************
// Emit bytecode for bitwise binary operators (&, |, ~, <<, >>)
// These are converted to bit.* library function calls.
// This method handles RHS evaluation internally to ensure correct register allocation.

ParserResult<ExpDesc> IrEmitter::emit_bitwise_expr(BinOpr opr, ExpDesc lhs, const ExprNode& rhs_ast)
{
   FuncState* fs = &this->func_state;
   RegisterAllocator allocator(fs);

   // Discharge Call expressions to NonReloc first. This ensures that function calls
   // returning multiple values are properly truncated to single values before being
   // used as operands, matching Lua's standard semantics for binary operators.

   if (lhs.k IS ExpKind::Call) {
      ExpressionValue lhs_discharge(fs, lhs);
      lhs_discharge.discharge();
      lhs = lhs_discharge.legacy();
   }

   // Discharge LHS to any register if needed (for non-constant values)

   if (not lhs.is_num_constant_nojump()) {
      ExpressionValue lhs_val(fs, lhs);
      lhs_val.discharge_to_any_reg(allocator);
      lhs = lhs_val.legacy();
   }

   // Calculate base register for the call frame.
   // Check if LHS is at the top of the stack to avoid orphaning registers when chaining
   // operations (e.g., 1 | 2 | 4 produces AST: (1 | 2) | 4, so LHS is the previous result).

   BCREG call_base;
   if (lhs.k IS ExpKind::NonReloc and lhs.u.s.info >= fs->varmap.size() and lhs.u.s.info + 1 IS fs->freereg) {
      // LHS is at the top - reuse its register to avoid orphaning
      call_base = lhs.u.s.info;
   }
   else call_base = fs->freereg;

   CSTRING op_name = priority[int(opr)].name;
   size_t op_name_len = priority[int(opr)].name_len;

   // Calculate argument slots
   BCREG arg1 = call_base + 1 + LJ_FR2;
   BCREG arg2 = arg1 + 1;

   // Convert LHS to value form
   ExpressionValue lhs_toval(fs, lhs);
   lhs_toval.to_val();
   lhs = lhs_toval.legacy();

   // Check if LHS is at base (for chaining). If so, move it before loading callee.

   bool lhs_was_base = (lhs.k IS ExpKind::NonReloc and lhs.u.s.info IS call_base);
   if (lhs_was_base) {
      ExpressionValue lhs_to_arg1(fs, lhs);
      lhs_to_arg1.to_reg(allocator, BCReg(arg1));
      lhs = lhs_to_arg1.legacy();
   }

   // Ensure freereg is past the call frame to prevent callee loading from clobbering
   if (fs->freereg <= arg2) fs->freereg = arg2 + 1;

   // Sequence for JIT compatibility (matches explicit bit.band() bytecode pattern):
   // 1. Check and move any operands (e.g., LHS) that conflict with the call_base register.
   // 2. Load bit.fname (the callee) to the call_base register.
   // 3. Move any remaining operands as needed.
   // Critical for JIT compatibility - JIT expects callee loaded before arguments.

   emit_bit_function_lookup(*fs, allocator, std::string_view(op_name, op_name_len), BCReg(call_base));

   // Now move LHS to arg1 if it wasn't at call_base
   if (not lhs_was_base) {
      ExpressionValue lhs_to_arg1(fs, lhs);
      lhs_to_arg1.to_reg(allocator, BCReg(arg1));
      lhs = lhs_to_arg1.legacy();
   }

   // NOW evaluate RHS - it will go to freereg (past the call frame)
   auto rhs_result = this->emit_expression(rhs_ast);
   if (not rhs_result.ok()) return rhs_result;
   ExpDesc rhs = rhs_result.value_ref();

   // Move RHS to arg2 if not already there
   ExpressionValue rhs_toval(fs, rhs);
   rhs_toval.to_val();
   rhs = rhs_toval.legacy();

   ExpressionValue rhs_to_arg2(fs, rhs);
   rhs_to_arg2.to_reg(allocator, BCReg(arg2));
   rhs = rhs_to_arg2.legacy();

   // Emit CALL instruction
   fs->freereg = arg2 + 1;
   lhs.k = ExpKind::Call;
   lhs.u.s.info = bcemit_INS(fs, BCINS_ABC(BC_CALL, call_base, 2, fs->freereg - call_base - LJ_FR2));
   lhs.u.s.aux = call_base;
   fs->freereg = call_base + 1;

   // Discharge call result
   ExpressionValue result_val(fs, lhs);
   result_val.discharge();
   lhs = result_val.legacy();

   return ParserResult<ExpDesc>::success(lhs);
}

//********************************************************************************************************************
// Emit bytecode for the `has` operator (bitwise flag test): `a has b` => `bit.band(a, b) != 0`
// Produces a boolean (jump-based) result, like comparison operators.
// Evaluation order is intentional:
// 1) Materialise/capture LHS first.
// 2) Resolve callee/call frame.
// 3) Evaluate RHS last.
// This preserves `has` left-to-right operand semantics when RHS evaluation has side effects
// (for example, mutating the source referenced by LHS).

ParserResult<ExpDesc> IrEmitter::emit_has_flag_expr(ExpDesc lhs, const ExprNode& rhs_ast)
{
   FuncState* fs = &this->func_state;

   // Constant folding: if both operands are numeric constants, compute at compile time.
   // Read RHS directly from AST so we do not emit/evaluate RHS before resolving bit.band.
   if (lhs.is_num_constant_nojump() and rhs_ast.kind IS AstNodeKind::LiteralExpr) {
      auto *rhs_literal = std::get_if<LiteralValue>(&rhs_ast.data);
      if (rhs_literal and rhs_literal->kind IS LiteralKind::Number) {
         auto k1 = lj_num2bit(lhs.number_value());
         auto k2 = lj_num2bit(rhs_literal->number_value);
         bool result = (k1 & k2) != 0;
         lhs.k = result ? ExpKind::True : ExpKind::False;
         lhs.result_type = TiriType::Bool;
         return ParserResult<ExpDesc>::success(lhs);
      }
   }

   // Runtime path: emit bit.band(lhs, rhs) call, then compare result against 0.
   // Keep LHS stable before RHS emission so RHS side effects cannot alter which LHS value we test.

   RegisterAllocator allocator(fs);

   // Discharge Call expressions to NonReloc first
   if (lhs.k IS ExpKind::Call) {
      ExpressionValue lhs_discharge(fs, lhs);
      lhs_discharge.discharge();
      lhs = lhs_discharge.legacy();
   }

   // Discharge LHS to register if needed
   if (not lhs.is_num_constant_nojump()) {
      ExpressionValue lhs_val(fs, lhs);
      lhs_val.discharge_to_any_reg(allocator);
      lhs = lhs_val.legacy();
   }

   // Calculate base register for the bit.band call frame
   BCREG call_base;
   if (lhs.k IS ExpKind::NonReloc and lhs.u.s.info >= fs->varmap.size() and lhs.u.s.info + 1 IS fs->freereg) {
      call_base = lhs.u.s.info;
   }
   else call_base = fs->freereg;

   BCREG arg1 = call_base + 1 + LJ_FR2;
   BCREG arg2 = arg1 + 1;

   // Convert LHS to value form
   ExpressionValue lhs_toval(fs, lhs);
   lhs_toval.to_val();
   lhs = lhs_toval.legacy();

   // Move LHS out of call_base if it's there (callee goes at call_base)
   bool lhs_was_base = (lhs.k IS ExpKind::NonReloc and lhs.u.s.info IS call_base);
   if (lhs_was_base) {
      ExpressionValue lhs_to_arg1(fs, lhs);
      lhs_to_arg1.to_reg(allocator, BCReg(arg1));
      lhs = lhs_to_arg1.legacy();
   }

   // Ensure freereg is past the call frame
   if (fs->freereg <= arg2) fs->freereg = arg2 + 1;

   // Load bit.band callee to call_base before RHS emission.
   // LHS is already captured above; delaying RHS avoids violating left-to-right operand semantics.
   emit_bit_function_lookup(*fs, allocator, "band", BCReg(call_base));

   // Move LHS to arg1 if it wasn't at call_base
   if (not lhs_was_base) {
      ExpressionValue lhs_to_arg1(fs, lhs);
      lhs_to_arg1.to_reg(allocator, BCReg(arg1));
      lhs = lhs_to_arg1.legacy();
   }

   // Evaluate RHS only after LHS is fixed and call frame is prepared.
   // This ordering prevents `x has f()` from reading a mutated `x` when `f()` has side effects.
   auto rhs_result = this->emit_expression(rhs_ast);
   if (not rhs_result.ok()) return rhs_result;
   ExpDesc rhs = rhs_result.value_ref();

   if (rhs.k IS ExpKind::Call) {
      ExpressionValue rhs_discharge(fs, rhs);
      rhs_discharge.discharge();
      rhs = rhs_discharge.legacy();
   }

   // Move RHS to arg2
   ExpressionValue rhs_toval(fs, rhs);
   rhs_toval.to_val();
   rhs = rhs_toval.legacy();

   ExpressionValue rhs_to_arg2(fs, rhs);
   rhs_to_arg2.to_reg(allocator, BCReg(arg2));
   rhs = rhs_to_arg2.legacy();

   // Emit CALL instruction for bit.band(lhs, rhs)
   fs->freereg = arg2 + 1;
   ExpDesc band_result;
   band_result.k = ExpKind::Call;
   band_result.u.s.info = bcemit_INS(fs, BCINS_ABC(BC_CALL, call_base, 2, fs->freereg - call_base - LJ_FR2));
   band_result.u.s.aux = call_base;
   fs->freereg = call_base + 1;

   // Discharge call result to a register
   ExpressionValue band_val(fs, band_result);
   band_val.discharge();
   band_result = band_val.legacy();

   BCREG result_reg = band_result.u.s.info;

   // Compare result against 0: BC_ISNEN skips the JMP when result != 0 (truthy/has flag)
   ExpDesc zerov(0.0);
   bcemit_INS(fs, BCINS_AD(BC_ISNEN, result_reg, const_num(fs, &zerov)));

   // Release the result register
   allocator.release_register(BCReg(result_reg));

   // Produce a Jmp expression (same pattern as bcemit_comp)
   lhs.u.s.info = bcemit_jmp(fs);
   lhs.k = ExpKind::Jmp;
   lhs.result_type = TiriType::Bool;

   return ParserResult<ExpDesc>::success(lhs);
}

//********************************************************************************************************************
// Emit bytecode for a ternary expression (condition ? true_value : false_value).

ParserResult<ExpDesc> IrEmitter::emit_ternary_expr(const TernaryExprPayload &Payload)
{
   if (not Payload.condition or not Payload.if_true or not Payload.if_false) {
      return this->unsupported_expr(AstNodeKind::TernaryExpr, SourceSpan{});
   }

   auto condition_result = this->emit_expression(*Payload.condition);
   if (not condition_result.ok()) return condition_result;

   // Use RegisterGuard for automatic register cleanup on all exit paths (RAII)

   RegisterGuard register_guard(&this->func_state);
   RegisterAllocator allocator(&this->func_state);
   ControlFlowEdge false_edge = this->control_flow.make_false_edge();
   BCReg condition_reg = BCReg(NO_REG);

   if (Payload.condition_mode IS TernaryConditionMode::Extended) {
      ExpressionValue condition_value(&this->func_state, condition_result.value_ref());
      condition_reg = condition_value.discharge_to_any_reg(allocator);

      FalseyJumpOptions options;
      options.include_empty_array = true;
      false_edge.append(emit_falsey_jumps(
         this->func_state, this->lex_state, this->control_flow, condition_reg, options));
   }
   else {
      ExpDesc condition = condition_result.value_ref();
      if (condition.k IS ExpKind::Nil) condition.k = ExpKind::False;
      bcemit_branch_t(&this->func_state, &condition);
      false_edge.append(BCPos(condition.f));
   }

   // Determine the result register. If cond_reg is a local variable, we must allocate a fresh register
   // to avoid corrupting it. Otherwise, we can reuse cond_reg (which is a temporary).
   BCReg result_reg;
   if (condition_reg.raw() != NO_REG and not this->func_state.is_local_register(condition_reg)) {
      result_reg = condition_reg; // condition_reg is a temporary - safe to reuse
   }
   else {
      // cond_reg holds a local variable that may be referenced later - allocate fresh register
      result_reg = BCReg(this->func_state.freereg);
      bcreg_reserve(&this->func_state, 1);
   }

   auto true_result = this->emit_expression(*Payload.if_true);
   if (not true_result.ok()) return true_result;

   ExpressionValue true_value(&this->func_state, true_result.value_ref());
   true_value.discharge();
   this->materialise_to_reg(true_value.legacy(), result_reg, "ternary true branch");
   allocator.collapse_freereg(result_reg);

   ControlFlowEdge skip_false = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   BCPos false_start = BCPos(this->func_state.pc);
   false_edge.patch_to(false_start);

   auto false_result = this->emit_expression(*Payload.if_false);
   if (not false_result.ok()) return false_result;
   ExpressionValue false_value(&this->func_state, false_result.value_ref());
   false_value.discharge();
   this->materialise_to_reg(false_value.legacy(), result_reg, "ternary false branch");
   allocator.collapse_freereg(result_reg);

   skip_false.patch_to(BCPos(this->func_state.pc));

   // Preserve result register by adjusting what RegisterGuard will restore to
   // Only restore to saved_freereg if it's beyond the result register

   if (register_guard.saved() > BCReg(result_reg + 1)) register_guard.adopt_saved(register_guard.saved());
   else register_guard.disarm();  // Keep current freereg (result_reg + 1)

   ExpDesc result;
   result.init(ExpKind::NonReloc, result_reg);
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a presence check expression (??), testing if a value is not nil.

ParserResult<ExpDesc> IrEmitter::emit_presence_expr(const PresenceExprPayload &Payload)
{
   SourceSpan span = Payload.value ? Payload.value->span : SourceSpan{};
   if (not Payload.value) return this->unsupported_expr(AstNodeKind::PresenceExpr, span);
   auto value_result = this->emit_expression(*Payload.value);
   if (not value_result.ok()) return value_result;
   ExpDesc value = value_result.value_ref();
   this->operator_emitter.emit_presence_check(ExprValue(&value));
   return ParserResult<ExpDesc>::success(value);
}

//********************************************************************************************************************
// Emit bytecode for a member access expression (table.field), indexing a table with a string key.
// base_type and class_id are tracked for potential future optimizations (Object-specific bytecode paths).

ParserResult<ExpDesc> IrEmitter::emit_member_expr(const MemberExprPayload &Payload)
{
   if (not Payload.table or Payload.member.symbol IS nullptr) {
      return this->unsupported_expr(AstNodeKind::MemberExpr, Payload.member.span);
   }

   auto table_result = this->emit_expression(*Payload.table);
   if (not table_result.ok()) return table_result;

   ExpDesc table = table_result.value_ref();

   // Save the emitted expression's result_type and object_class_id before discharge operations may modify it.
   // This captures type information propagated from VarInfo during variable lookup.

   TiriType emitted_base_type = table.result_type;
   CLASSID emitted_class_id = table.object_class_id;

   RegisterAllocator allocator(&this->func_state);
   ExpressionValue table_value(&this->func_state, table);
   table_value.discharge_to_any_reg(allocator);
   table = table_value.legacy();
   ExpDesc key(Payload.member.symbol);
   expr_index(&this->func_state, &table, &key);

   // Propagate known base type information for downstream optimizations.
   // When base_type is Object, emit specialised BC_OBGETF/BC_OBSETF bytecodes by changing the expression kind
   // to IndexedObject.  Check both AST-level base_type AND emitted expression's result_type (the latter captures
   // type info from variable declarations like `fl = obj.new(...)`).

   if (Payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
      table.result_type = TiriType::Object;
      // Only use IndexedObject for string keys (member access always uses string keys)

      if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) table.k = ExpKind::IndexedObject;

      // Look up the field type from the class dictionary for compile-time type checking.
      // Use either the AST-level class_id or the emitted expression's class_id.

      CLASSID class_id = (Payload.class_id != CLASSID::NIL) ? Payload.class_id : emitted_class_id;
      if (class_id != CLASSID::NIL and Payload.member.symbol) {
         auto field_info = lookup_field_type(class_id, Payload.member.symbol->hash);
         bool is_field = field_info.has_value() and (field_info->type != TiriType::Unknown);

         if (is_field) {
            table.result_type = field_info->type;
            table.object_class_id = field_info->object_class_id;
            table.type_confirmed = true;  // Type is confirmed from class dictionary lookup
         }
         else if (not Payload.is_call_target) {
            // Field not found in dictionary and not being called as a function - raise parse error
            auto *meta_class = FindClass(class_id);
            CSTRING class_name = meta_class ? meta_class->ClassName.c_str() : "Unknown";
            lj_lex_error(this->func_state.ls, 0, ErrMsg::BADFIELD, strdata(Payload.member.symbol), class_name);
            return this->unsupported_expr(AstNodeKind::MemberExpr, Payload.member.span);
         }
         // If is_call_target is true, skip type checking and let runtime handle the method/action call
      }
   }
   else {
      // Reset result_type so the base type does not leak into downstream chained expressions.
      // E.g. arr[1].list should not propagate Array type from arr — .list returns whatever it holds.
      table.result_type = TiriType::Unknown;
   }

   return ParserResult<ExpDesc>::success(table);
}

//********************************************************************************************************************
// Emit bytecode for an index expression (table[key]), indexing a table or array with an arbitrary key.
// Special case: if key is a range expression, emit a call to table.slice() instead.
// If base_type is TiriType::Array, emits array-specific bytecodes (BC_AGETV/BC_AGETB).

ParserResult<ExpDesc> IrEmitter::emit_index_expr(const IndexExprPayload &Payload)
{
   if (not Payload.table or not Payload.index) return this->unsupported_expr(AstNodeKind::IndexExpr, SourceSpan{});

   // Check if index is a range expression - handle at parse time by emitting table.slice() call
   if (Payload.index->kind IS AstNodeKind::RangeExpr) {
      return this->emit_table_slice_call(Payload);
   }

   auto table_result = this->emit_expression(*Payload.table);
   if (not table_result.ok()) return table_result;
   ExpDesc table = table_result.value_ref();

   // Save the emitted expression's result_type before discharge operations may modify it.
   // This captures type information propagated from VarInfo during variable lookup.
   TiriType emitted_base_type = table.result_type;

   // Materialize table BEFORE evaluating key, so nested index expressions emit bytecode in
   // the correct order (table first, then key)
   RegisterAllocator allocator(&this->func_state);
   ExpressionValue table_value(&this->func_state, table);
   table_value.discharge_to_any_reg(allocator);
   table = table_value.legacy();
   auto key_result = this->emit_expression(*Payload.index);
   if (not key_result.ok()) return key_result;
   ExpDesc key = key_result.value_ref();
   ExpressionValue key_toval(&this->func_state, key);
   key_toval.to_val();
   key = key_toval.legacy();
   expr_index(&this->func_state, &table, &key);

   // If base type is known to be an array, use array-specific bytecodes
   // Check both AST-level base_type AND emitted expression's result_type.
   if (Payload.base_type IS TiriType::Array or emitted_base_type IS TiriType::Array) {
      // Arrays don't support string keys, so only change kind for numeric indexing
      // (aux >= 0 means numeric index, aux < 0 means string const key)
      if (int32_t(table.u.s.aux) >= 0) {
         table.k = ExpKind::IndexedArray;
      }
   }
   // If base type is known to be an object with a string key, use object-specific bytecodes
   // Check both AST-level base_type AND emitted expression's result_type.
   else if (Payload.base_type IS TiriType::Object or emitted_base_type IS TiriType::Object) {
      // Objects use string field access - only change kind for string const keys
      // (aux < 0 means string const key)
      if (table.k IS ExpKind::Indexed and int32_t(table.u.s.aux) < 0) {
         table.k = ExpKind::IndexedObject;
      }
   }

   // Reset result_type so the base type does not leak into downstream chained expressions.
   // The result of arr[i] is the element (which could be table, object, etc.), not another array.
   table.result_type = TiriType::Unknown;

   return ParserResult<ExpDesc>::success(table);
}

//********************************************************************************************************************
// Emit bytecode for slicing: expr[{range}] -> range.slice(expr, range)
// This is called when the parser detects that the index is a RangeExpr.
// Works for both tables and strings - range.slice dispatches based on type at runtime.

ParserResult<ExpDesc> IrEmitter::emit_table_slice_call(const IndexExprPayload &Payload)
{
   FuncState *fs = &this->func_state;
   RegisterAllocator allocator(fs);

   // Capture the call base register before emitting anything
   BCReg call_base = fs->free_reg();

   // Load range.slice function (range global, then access .slice field)
   ExpDesc range_lib;
   range_lib.init(ExpKind::Global, 0);
   range_lib.u.sval = fs->ls->keepstr("range");

   // Discharge range global to a register
   ExpressionValue range_value(fs, range_lib);
   range_value.discharge_to_any_reg(allocator);
   range_lib = range_value.legacy();

   // Access the .slice field
   ExpDesc slice_key(fs->ls->keepstr("slice"));
   expr_index(fs, &range_lib, &slice_key);

   // Materialise the function to call base register
   this->materialise_to_next_reg(range_lib, "range.slice function");

   // Reserve register for frame link (LJ_FR2)
   allocator.reserve(BCReg(1));

   // Emit base expression (table or string) as arg1
   auto base_result = this->emit_expression(*Payload.table);
   if (not base_result.ok()) return base_result;
   ExpDesc base_arg = base_result.value_ref();
   this->materialise_to_next_reg(base_arg, "slice base arg");

   // Emit range expression as arg2 (this will call range() constructor)
   auto range_result = this->emit_expression(*Payload.index);
   if (not range_result.ok()) return range_result;
   ExpDesc range_arg = range_result.value_ref();
   this->materialise_to_next_reg(range_arg, "slice range arg");

   // Emit CALL instruction: range.slice(expr, range)
   // BC_CALL A=base, B=2 (expect 1 result), C=3 (2 args + 1)
   BCIns ins = BCINS_ABC(BC_CALL, call_base, 2, 3);

   ExpDesc result;
   result.init(ExpKind::Call, bcemit_INS(fs, ins));
   result.u.s.aux = call_base;
   fs->freereg = call_base + 1;

   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a safe member access expression (table?.field), returning nil if the table is nil.
// base_type and class_id are tracked for potential future optimizations (Object-specific bytecode paths).

ParserResult<ExpDesc> IrEmitter::emit_safe_member_expr(const SafeMemberExprPayload &Payload)
{
   if (not Payload.table or Payload.member.symbol IS nullptr) {
      return this->unsupported_expr(AstNodeKind::SafeMemberExpr, Payload.member.span);
   }

   auto table_result = this->emit_expression(*Payload.table);
   if (not table_result.ok()) return table_result;

   NilShortCircuitGuard guard(this, table_result.value_ref());
   if (not guard.ok()) return guard.error<ExpDesc>();

   ExpDesc table = guard.base_expression();
   ExpDesc key(Payload.member.symbol);
   expr_index(&this->func_state, &table, &key);

   // Propagate known base type information for downstream optimizations.
   // When base_type is Object and class_id is set, field-level type resolution may be possible.
   if (Payload.base_type IS TiriType::Object) {
      table.result_type = TiriType::Object;
      table.object_class_id = CLASSID::NIL;

      // Look up the field type from the class dictionary for compile-time type checking.

      if (Payload.class_id != CLASSID::NIL and Payload.member.symbol) {
         auto field_info = lookup_field_type(Payload.class_id, Payload.member.symbol->hash);
         bool is_field = field_info.has_value() and (field_info->type != TiriType::Unknown);

         if (is_field) {
            table.result_type     = field_info->type;
            table.object_class_id = field_info->object_class_id;
            table.type_confirmed  = true;  // Type is confirmed from class dictionary lookup
         }
         else if (not Payload.is_call_target) {
            auto *meta_class = FindClass(Payload.class_id);
            const char *class_name = meta_class ? meta_class->ClassName.c_str() : "Unknown";
            lj_lex_error(this->func_state.ls, 0, ErrMsg::BADFIELD, strdata(Payload.member.symbol), class_name);
            return this->unsupported_expr(AstNodeKind::SafeMemberExpr, Payload.member.span);
         }
         // If is_call_target is true, skip type checking and let runtime handle the method/action call
      }
   }

   // Materialize the indexed result to a new register.
   // Do NOT reuse base_register() as that would clobber the table variable

   ExpressionValue indexed_value(&this->func_state, table);
   BCReg result_reg = indexed_value.discharge_to_any_reg(guard.reg_allocator());

   // Collapse freereg to include the result register
   guard.reg_allocator().collapse_freereg(result_reg);

   // Emit the nil path
   ControlFlowEdge skip_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   BCPos nil_path = BCPos(this->func_state.pc);
   guard.nil_jump_edge().patch_to(nil_path);
   bcemit_nil(&this->func_state, result_reg.raw(), 1);

   skip_nil.patch_to(BCPos(this->func_state.pc));

   ExpDesc result;
   result.init(ExpKind::NonReloc, result_reg);
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a safe index expression (table?[key]), returning nil if the table is nil.
// If base_type is TiriType::Array, emits array-specific bytecodes (BC_AGETV/BC_AGETB).

ParserResult<ExpDesc> IrEmitter::emit_safe_index_expr(const SafeIndexExprPayload &Payload)
{
   if (not Payload.table or not Payload.index) {
      return this->unsupported_expr(AstNodeKind::SafeIndexExpr, SourceSpan{});
   }

   auto table_result = this->emit_expression(*Payload.table);
   if (not table_result.ok()) return table_result;

   NilShortCircuitGuard guard(this, table_result.value_ref());
   if (not guard.ok()) return guard.error<ExpDesc>();

   // Index expression is evaluated only on non-nil path (short-circuit)
   auto key_result = this->emit_expression(*Payload.index);
   if (not key_result.ok()) return key_result;

   ExpDesc key = key_result.value_ref();
   ExpressionValue key_toval(&this->func_state, key);
   key_toval.to_val();
   key = key_toval.legacy();

   ExpDesc table = guard.base_expression();
   expr_index(&this->func_state, &table, &key);

   // For safe index expressions (?[]), always use SafeIndexedArray for numeric keys.
   // This emits BC_ASGETV/BC_ASGETB which:
   // - For arrays: return nil for out-of-bounds instead of throwing
   // - For non-arrays: fall back to regular table indexing
   // We only do this for numeric keys (aux >= 0); string keys use regular table indexing.

   if (int32_t(table.u.s.aux) >= 0) table.k = ExpKind::SafeIndexedArray;

   // Materialize the indexed result to a new register.
   // Do NOT reuse base_register() as that would clobber the table variable,
   // causing issues if the table is referenced again.
   ExpressionValue indexed_value(&this->func_state, table);
   BCReg result_reg = indexed_value.discharge_to_any_reg(guard.reg_allocator());

   // Collapse freereg to include the result register
   guard.reg_allocator().collapse_freereg(result_reg);

   // Emit the nil path
   ControlFlowEdge skip_nil = this->control_flow.make_unconditional(BCPos(bcemit_jmp(&this->func_state)));

   BCPos nil_path = BCPos(this->func_state.pc);
   guard.nil_jump_edge().patch_to(nil_path);
   bcemit_nil(&this->func_state, result_reg.raw(), 1);

   skip_nil.patch_to(BCPos(this->func_state.pc));

   ExpDesc result;
   result.init(ExpKind::NonReloc, result_reg);
   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Emit bytecode for a range literal expression ({start to stop} or {start into stop}).
// Emits a call to the global `range` function: range(start, stop, inclusive [, step])

ParserResult<ExpDesc> IrEmitter::emit_range_expr(const RangeExprPayload &Payload)
{
   FuncState* fs = &this->func_state;
   RegisterAllocator allocator(fs);

   // Emit the start and stop expressions first
   if (not Payload.start or not Payload.stop) {
      return this->unsupported_expr(AstNodeKind::RangeExpr, SourceSpan{});
   }

   // Load the 'range' global function first
   BCReg base = fs->free_reg();
   ExpDesc callee;
   callee.init(ExpKind::Global, 0);
   callee.u.sval = fs->ls->keepstr("range");
   this->materialise_to_next_reg(callee, "range function");

   // Reserve register for frame link (LJ_FR2)
   allocator.reserve(BCReg(1));

   // Emit start expression as arg1
   auto start_result = this->emit_expression(*Payload.start);
   if (not start_result.ok()) return start_result;
   ExpDesc start_expr = start_result.value_ref();
   this->materialise_to_next_reg(start_expr, "range start");

   // Emit stop expression as arg2
   auto stop_result = this->emit_expression(*Payload.stop);
   if (not stop_result.ok()) return stop_result;
   ExpDesc stop_expr = stop_result.value_ref();
   this->materialise_to_next_reg(stop_expr, "range stop");

   // Emit inclusive flag as arg3
   ExpDesc inclusive_expr(Payload.inclusive);
   this->materialise_to_next_reg(inclusive_expr, "range inclusive");

   uint8_t argument_count = 3;
   if (Payload.step) {
      auto step_result = this->emit_expression(*Payload.step);
      if (not step_result.ok()) return step_result;
      ExpDesc step_expr = step_result.value_ref();
      this->materialise_to_next_reg(step_expr, "range step");
      argument_count = 4;
   }

   // Emit CALL instruction: range(start, stop, inclusive [, step])
   // BC_CALL A=base, B=2 (expect 1 result), C=args + 1
   BCIns ins = BCINS_ABC(BC_CALL, base, 2, argument_count + 1);

   ExpDesc result;
   result.init(ExpKind::Call, bcemit_INS(fs, ins));
   result.u.s.aux = base;
   fs->freereg = base + 1;

   return ParserResult<ExpDesc>::success(result);
}

//********************************************************************************************************************
// Materialise an expression to the next available register, ensuring it's stored in a concrete location.

void IrEmitter::materialise_to_next_reg(ExpDesc& expression, std::string_view usage)
{
   RegisterAllocator allocator(&this->func_state);
   ExpressionValue value(&this->func_state, expression);
   value.to_next_reg(allocator);
   expression = value.legacy();
   this->ensure_register_floor(usage);
}

// Materialise an expression to a specific register slot.

void IrEmitter::materialise_to_reg(ExpDesc& expression, BCReg slot, std::string_view usage)
{
   RegisterAllocator allocator(&this->func_state);
   ExpressionValue value(&this->func_state, expression);
   value.to_reg(allocator, slot);
   expression = value.legacy();
   this->ensure_register_floor(usage);
}

// Ensure the free register pointer hasn't fallen below the active variable count (register underrun check).

void IrEmitter::ensure_register_floor(std::string_view usage)
{
   if (this->func_state.freereg < this->func_state.varmap.size()) {
      kt::Log log("Parser");
      log.warning("Register underrun during %.*s (free=%u active=%u)",
         int(usage.size()), usage.data(), unsigned(this->func_state.freereg), unsigned(this->func_state.varmap.size()));
      this->func_state.reset_freereg();
   }
}

// Ensure registers are balanced (no leaks or underruns) after completing an operation.

void IrEmitter::ensure_register_balance(std::string_view usage)
{
   this->ensure_register_floor(usage);
   if (this->func_state.freereg > this->func_state.varmap.size()) {
      kt::Log log("Parser");
      int line = this->lex_state.lastline;
      log.warning("Leaked %u registers after %.*s at line %d (free=%u active=%u)",
         unsigned(this->func_state.freereg - this->func_state.varmap.size()), int(usage.size()), usage.data(),
         line + 1, unsigned(this->func_state.freereg), unsigned(this->func_state.varmap.size()));
      this->func_state.reset_freereg();
   }
}

//********************************************************************************************************************
// Report an unsupported statement node and return an internal invariant error.

ParserResult<IrEmitUnit> IrEmitter::unsupported_stmt(AstNodeKind kind, const SourceSpan &span)
{
   glUnsupportedNodes.record(kind, span, "stmt");
   std::string message = "IR emitter does not yet support statement kind " + std::to_string(int(kind));
   return ParserResult<IrEmitUnit>::failure(this->make_error(ParserErrorCode::InternalInvariant, message));
}

// Report an unsupported expression node and return an internal invariant error.

ParserResult<ExpDesc> IrEmitter::unsupported_expr(AstNodeKind kind, const SourceSpan &span)
{
   glUnsupportedNodes.record(kind, span, "expr");
   std::string message = "IR emitter does not yet support expression kind " + std::to_string(int(kind));
   return ParserResult<ExpDesc>::failure(this->make_error(ParserErrorCode::InternalInvariant, message));
}

//********************************************************************************************************************

#include "ir_emitter/emit_choose.cpp"
#include "ir_emitter/emit_global.cpp"
#include "ir_emitter/emit_assignment.cpp"
#include "ir_emitter/emit_function.cpp"
#include "ir_emitter/emit_table.cpp"
#include "ir_emitter/emit_call.cpp"
#include "ir_emitter/emit_try.cpp"
