#include "ast/nodes.h"

#include "lj_def.h"
#include <kotuku/strings.hpp>

#include <unordered_map>

TiriType parse_type_name(std::string_view Name)
{
   static const std::unordered_map<std::string_view, TiriType> type_map = {
      { "any",       TiriType::Any },
      { "nil",       TiriType::Nil },
      { "bool",      TiriType::Bool },
      { "boolean",   TiriType::Bool },
      { "num",       TiriType::Num },
      { "number",    TiriType::Num },
      { "str",       TiriType::Str },
      { "string",    TiriType::Str },
      { "table",     TiriType::Table },
      { "array",     TiriType::Array },
      { "func",      TiriType::Func },
      { "function",  TiriType::Func },
      { "thread",    TiriType::Thread },
      { "obj",       TiriType::Object },
      { "object",    TiriType::Object },
      { "range",     TiriType::Range }
   };

   auto it = type_map.find(Name);
   return (it != type_map.end()) ? it->second : TiriType::Unknown;
}

std::string_view type_name(TiriType Type)
{
   switch (Type) {
      case TiriType::Nil:    return "nil";
      case TiriType::Bool:   return "bool";
      case TiriType::Num:    return "num";
      case TiriType::Str:    return "str";
      case TiriType::Table:  return "table";
      case TiriType::Array:  return "array";
      case TiriType::Func:   return "func";
      case TiriType::Thread: return "thread";
      case TiriType::Object: return "obj";
      case TiriType::Range:  return "range";
      case TiriType::Any:
      default: return "any";
   }
}

//********************************************************************************************************************
// Convert TiriType to LJ type tag base value.
// The LJ_T* tags are defined as ~value (bitwise NOT), e.g.:
//   LJ_TNIL = ~0, LJ_TFALSE = ~1, LJ_TTRUE = ~2, LJ_TSTR = ~4, LJ_TTAB = ~11, LJ_TARRAY = ~13, LJ_TNUMX = ~14
// We store the base value (0-14) and recover the tag with ~value
// Returns 0xFF for Unknown/Any types to signal "needs evaluation"

uint8_t tiri_type_to_lj_tag(TiriType Type)
{
   switch (Type) {
      case TiriType::Nil:    return 0;   // ~0 = LJ_TNIL
      case TiriType::Bool:   return 2;   // ~2 = LJ_TTRUE (we use true as the canonical boolean)
      case TiriType::Str:    return 4;   // ~4 = LJ_TSTR
      case TiriType::Thread: return 6;   // ~6 = LJ_TTHREAD
      case TiriType::Func:   return 8;   // ~8 = LJ_TFUNC
      case TiriType::Object: return 10;  // ~10 = LJ_TOBJECT
      case TiriType::Table:  return 11;  // ~11 = LJ_TTAB
      case TiriType::Range:  return 12;  // ~12 = LJ_TUDATA (ranges are userdata at runtime)
      case TiriType::Array:  return 13;  // ~13 = LJ_TARRAY
      case TiriType::Num:    return 14;  // ~14 = LJ_TNUMX
      case TiriType::Any:
      case TiriType::Unknown:
      default: return 0xFF;  // Unknown - needs evaluation
   }
}

//********************************************************************************************************************
// Infer the result type of an expression from its AST structure.
// This is used for type-carrying deferred expressions to store the expected result type.

TiriType infer_expression_type(const ExprNode &Expr)
{
   switch (Expr.kind) {
      case AstNodeKind::LiteralExpr: {
         const auto& literal = std::get<LiteralValue>(Expr.data);
         switch (literal.kind) {
            case LiteralKind::Nil:     return TiriType::Nil;
            case LiteralKind::Boolean: return TiriType::Bool;
            case LiteralKind::Number:  return TiriType::Num;
            case LiteralKind::String:  return TiriType::Str;
         }
         break;
      }

      case AstNodeKind::TableExpr:
         return TiriType::Table;

      case AstNodeKind::FunctionExpr:
         return TiriType::Func;

      // Unary operators: result type depends on operator
      case AstNodeKind::UnaryExpr: {
         const auto& payload = std::get<UnaryExprPayload>(Expr.data);
         switch (payload.op) {
            case AstUnaryOperator::Negate:  return TiriType::Num;
            case AstUnaryOperator::Not:     return TiriType::Bool;
            case AstUnaryOperator::Length:  return TiriType::Num;
            case AstUnaryOperator::BitNot:  return TiriType::Num;
         }
         break;
      }

      // Binary operators: result type depends on operator
      case AstNodeKind::BinaryExpr: {
         const auto& payload = std::get<BinaryExprPayload>(Expr.data);
         switch (payload.op) {
            // Arithmetic operators return numbers
            case AstBinaryOperator::Add:
            case AstBinaryOperator::Subtract:
            case AstBinaryOperator::Multiply:
            case AstBinaryOperator::Divide:
            case AstBinaryOperator::Modulo:
            case AstBinaryOperator::Power:
            case AstBinaryOperator::BitAnd:
            case AstBinaryOperator::BitOr:
            case AstBinaryOperator::BitXor:
            case AstBinaryOperator::ShiftLeft:
            case AstBinaryOperator::ShiftRight:
               return TiriType::Num;

            // Comparison operators return boolean
            case AstBinaryOperator::NotEqual:
            case AstBinaryOperator::Equal:
            case AstBinaryOperator::LessThan:
            case AstBinaryOperator::GreaterEqual:
            case AstBinaryOperator::LessEqual:
            case AstBinaryOperator::GreaterThan:
            case AstBinaryOperator::HasFlag:
               return TiriType::Bool;

            // Concatenation returns string
            case AstBinaryOperator::Concat:
               return TiriType::Str;

            // Logical operators return the type of their operands (short-circuit)
            // Cannot reliably infer without evaluating
            case AstBinaryOperator::LogicalAnd:
            case AstBinaryOperator::LogicalOr:
            case AstBinaryOperator::IfEmpty:
               return TiriType::Unknown;
         }
         break;
      }

      // Update expressions (++/--) return numbers
      case AstNodeKind::UpdateExpr:
         return TiriType::Num;

      // Ternary expression: type depends on branches (would need to check both)
      case AstNodeKind::TernaryExpr: {
         const auto& payload = std::get<TernaryExprPayload>(Expr.data);
         if (payload.if_true) {
            TiriType true_type = infer_expression_type(*payload.if_true);
            if (payload.if_false) {
               TiriType false_type = infer_expression_type(*payload.if_false);
               // If both branches have the same type, use it
               if (true_type IS false_type) return true_type;
            }
            // If types differ or false branch unknown, return unknown
            return TiriType::Unknown;
         }
         return TiriType::Unknown;
      }

      // Presence check returns boolean
      case AstNodeKind::PresenceExpr:
         return TiriType::Bool;

      // Call expressions may have a known result type (e.g., obj.new() returns Object)
      case AstNodeKind::CallExpr:
      case AstNodeKind::SafeCallExpr: {
         const auto& payload = std::get<CallExprPayload>(Expr.data);
         if (payload.result_type != TiriType::Unknown) {
            return payload.result_type;
         }
         return TiriType::Unknown;
      }

      // For these, we cannot infer without runtime information
      case AstNodeKind::IdentifierExpr:
      case AstNodeKind::VarArgExpr:
      case AstNodeKind::MemberExpr:
      case AstNodeKind::IndexExpr:
      case AstNodeKind::SafeMemberExpr:
      case AstNodeKind::SafeIndexExpr:
      case AstNodeKind::PipeExpr:
      case AstNodeKind::ResultFilterExpr:
         return TiriType::Unknown;

      // Deferred expressions return the type of their inner expression
      case AstNodeKind::DeferredExpr: {
         const auto& payload = std::get<DeferredExprPayload>(Expr.data);
         if (payload.type_explicit) return payload.deferred_type;
         if (payload.inner) return infer_expression_type(*payload.inner);
         return TiriType::Unknown;
      }

      // Range expressions have their own type for static tracking
      case AstNodeKind::RangeExpr:
         return TiriType::Range;
      default:
         break;
   }
   return TiriType::Unknown;
}

//********************************************************************************************************************
// Extended type inference that also returns object class ID for Object types.
// This is used when creating local variables to capture the full type information.

InferredTypeInfo infer_expression_type_ext(const ExprNode &Expr)
{
   InferredTypeInfo result;
   result.type = infer_expression_type(Expr);

   // For call expressions, also extract the object class ID if available
   if (Expr.kind IS AstNodeKind::CallExpr or Expr.kind IS AstNodeKind::SafeCallExpr) {
      const auto &payload = std::get<CallExprPayload>(Expr.data);
      if (payload.result_type IS TiriType::Object) {
         result.object_class_id = payload.object_class_id;
      }
   }

   return result;
}

namespace {

[[nodiscard]] inline bool ensure_operand(const ExprNodePtr &node) { return node != nullptr; }

inline void assert_node(bool condition, CSTRING message) { lj_assertX(condition, message); }

[[nodiscard]] inline size_t block_child_count(const std::unique_ptr<BlockStmt> &block) {
   return block ? block->view().size() : 0;
}

struct CallTargetChildCounter {
   [[nodiscard]] size_t operator()(const DirectCallTarget &Target) const { return Target.callable ? 1 : 0; }
   [[nodiscard]] size_t operator()(const MethodCallTarget &Target) const { return Target.receiver ? 1 : 0; }
   [[nodiscard]] size_t operator()(const SafeMethodCallTarget &Target) const { return Target.receiver ? 1 : 0; }
};

struct ExpressionChildCounter {
   [[nodiscard]] inline size_t operator()(const LiteralValue &) const { return 0; }
   [[nodiscard]] inline size_t operator()(const NameRef &) const { return 0; }
   [[nodiscard]] inline size_t operator()(const VarArgExprPayload &) const { return 0; }

   [[nodiscard]] inline size_t operator()(const UnaryExprPayload &Payload) const {
      return Payload.operand ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const UpdateExprPayload &Payload) const {
      return Payload.target ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const BinaryExprPayload &Payload) const {
      size_t total = Payload.left ? 1 : 0;
      if (Payload.right) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const TernaryExprPayload &Payload) const {
      size_t total = Payload.condition ? 1 : 0;
      if (Payload.if_true) total++;
      if (Payload.if_false) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const PresenceExprPayload &Payload) const {
      return Payload.value ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const PipeExprPayload &Payload) const {
      size_t total = Payload.lhs ? 1 : 0;
      if (Payload.rhs_call) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const CallExprPayload &Payload) const {
      size_t total = std::visit(CallTargetChildCounter{}, Payload.target);
      total += Payload.arguments.size();
      return total;
   }

   [[nodiscard]] inline size_t operator()(const MemberExprPayload &Payload) const {
      return Payload.table ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const IndexExprPayload &Payload) const {
      size_t total = Payload.table ? 1 : 0;
      if (Payload.index) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const SafeMemberExprPayload &Payload) const
   {
      return Payload.table ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const SafeIndexExprPayload &Payload) const
   {
      size_t total = Payload.table ? 1 : 0;
      if (Payload.index) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const ResultFilterPayload &Payload) const
   {
      return Payload.expression ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const TableExprPayload &Payload) const
   {
      size_t total = 0;
      for (const TableField &field : Payload.fields) {
         if (field.key) total++;
         if (field.value) total++;
      }
      return total;
   }

   [[nodiscard]] inline size_t operator()(const FunctionExprPayload &Payload) const
   {
      return block_child_count(Payload.body);
   }

   [[nodiscard]] inline size_t operator()(const DeferredExprPayload &Payload) const
   {
      return Payload.inner ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const RangeExprPayload &Payload) const
   {
      size_t total = Payload.start ? 1 : 0;
      if (Payload.stop) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const ChooseExprPayload &Payload) const
   {
      size_t total = Payload.scrutinee ? 1 : 0;
      for (const ChooseCase& c : Payload.cases) {
         if (c.pattern) total++;
         if (c.guard) total++;
         if (c.result) total++;
      }
      return total;
   }
};

struct StatementChildCounter {
   [[nodiscard]] inline size_t operator()(const AssignmentStmtPayload &Payload) const
   {
      return Payload.targets.size() + Payload.values.size();
   }

   [[nodiscard]] inline size_t operator()(const LocalDeclStmtPayload &Payload) const
   {
      return Payload.values.size();
   }

   [[nodiscard]] inline size_t operator()(const GlobalDeclStmtPayload &Payload) const
   {
      return Payload.values.size();
   }

   [[nodiscard]] inline size_t operator()(const LocalFunctionStmtPayload &Payload) const
   {
      return Payload.function ? block_child_count(Payload.function->body) : 0;
   }

   [[nodiscard]] inline size_t operator()(const FunctionStmtPayload &Payload) const
   {
      return Payload.function ? block_child_count(Payload.function->body) : 0;
   }

   [[nodiscard]] inline size_t operator()(const IfStmtPayload &Payload) const
   {
      size_t total = 0;
      for (const IfClause& clause : Payload.clauses) {
         if (clause.condition) total++;
         total += block_child_count(clause.block);
      }
      return total;
   }

   [[nodiscard]] inline size_t operator()(const LoopStmtPayload &Payload) const
   {
      size_t total = Payload.condition ? 1 : 0;
      total += block_child_count(Payload.body);
      return total;
   }

   [[nodiscard]] inline size_t operator()(const NumericForStmtPayload &Payload) const
   {
      size_t total = 0;
      if (Payload.start) total++;
      if (Payload.stop) total++;
      if (Payload.step) total++;
      total += block_child_count(Payload.body);
      return total;
   }

   [[nodiscard]] inline size_t operator()(const GenericForStmtPayload &Payload) const
   {
      size_t total = Payload.iterators.size();
      total += block_child_count(Payload.body);
      return total;
   }

   [[nodiscard]] inline size_t operator()(const ReturnStmtPayload &Payload) const
   {
      return Payload.values.size();
   }

   [[nodiscard]] inline size_t operator()(const BreakStmtPayload &) const { return 0; }
   [[nodiscard]] inline size_t operator()(const ContinueStmtPayload &) const { return 0; }

   [[nodiscard]] inline size_t operator()(const DeferStmtPayload &Payload) const
   {
      size_t total = Payload.arguments.size();
      if (Payload.callable) {
         total += block_child_count(Payload.callable->body);
      }
      return total;
   }

   [[nodiscard]] inline size_t operator()(const DoStmtPayload &Payload) const
   {
      return block_child_count(Payload.block);
   }

   [[nodiscard]] inline size_t operator()(const ConditionalShorthandStmtPayload &Payload) const
   {
      size_t total = Payload.condition ? 1 : 0;
      if (Payload.body) total += ast_statement_child_count(*Payload.body);
      return total;
   }

   [[nodiscard]] inline size_t operator()(const TryExceptPayload &Payload) const
   {
      size_t total = block_child_count(Payload.try_block);
      for (const ExceptClause& clause : Payload.except_clauses) {
         total += clause.filter_codes.size();
         total += block_child_count(clause.block);
      }
      return total;
   }

   [[nodiscard]] inline size_t operator()(const RaiseStmtPayload &Payload) const
   {
      size_t total = Payload.error_code ? 1 : 0;
      if (Payload.message) total++;
      return total;
   }

   [[nodiscard]] inline size_t operator()(const CheckStmtPayload &Payload) const
   {
      return Payload.error_code ? 1 : 0;
   }

   [[nodiscard]] inline size_t operator()(const ImportStmtPayload &Payload) const
   {
      return block_child_count(Payload.inlined_body);
   }

   [[nodiscard]] inline size_t operator()(const WithStmtPayload &Payload) const
   {
      return Payload.objects.size() + block_child_count(Payload.block);
   }

   [[nodiscard]] inline size_t operator()(const ExpressionStmtPayload &Payload) const
   {
      return Payload.expression ? 1 : 0;
   }
};

}  // namespace

DirectCallTarget::~DirectCallTarget() = default;
MethodCallTarget::~MethodCallTarget() = default;
SafeMethodCallTarget::~SafeMethodCallTarget() = default;
UnaryExprPayload::~UnaryExprPayload() = default;
UpdateExprPayload::~UpdateExprPayload() = default;
BinaryExprPayload::~BinaryExprPayload() = default;
TernaryExprPayload::~TernaryExprPayload() = default;
PresenceExprPayload::~PresenceExprPayload() = default;
PipeExprPayload::~PipeExprPayload() = default;
CallExprPayload::~CallExprPayload() = default;
MemberExprPayload::~MemberExprPayload() = default;
IndexExprPayload::~IndexExprPayload() = default;
SafeMemberExprPayload::~SafeMemberExprPayload() = default;
SafeIndexExprPayload::~SafeIndexExprPayload() = default;
ResultFilterPayload::~ResultFilterPayload() = default;
TableField::~TableField() = default;
TableExprPayload::~TableExprPayload() = default;
FunctionExprPayload::~FunctionExprPayload() = default;
DeferredExprPayload::~DeferredExprPayload() = default;
RangeExprPayload::~RangeExprPayload() = default;
ChooseCase::~ChooseCase() = default;
ChooseExprPayload::~ChooseExprPayload() = default;
IfClause::~IfClause() = default;
AssignmentStmtPayload::~AssignmentStmtPayload() = default;
LocalDeclStmtPayload::~LocalDeclStmtPayload() = default;
GlobalDeclStmtPayload::~GlobalDeclStmtPayload() = default;
LocalFunctionStmtPayload::~LocalFunctionStmtPayload() = default;
FunctionStmtPayload::~FunctionStmtPayload() = default;
IfStmtPayload::~IfStmtPayload() = default;
LoopStmtPayload::~LoopStmtPayload() = default;
NumericForStmtPayload::~NumericForStmtPayload() = default;
GenericForStmtPayload::~GenericForStmtPayload() = default;
ReturnStmtPayload::~ReturnStmtPayload() = default;
DeferStmtPayload::~DeferStmtPayload() = default;
DoStmtPayload::~DoStmtPayload() = default;
ExpressionStmtPayload::~ExpressionStmtPayload() = default;
ConditionalShorthandStmtPayload::~ConditionalShorthandStmtPayload() = default;
ExceptClause::~ExceptClause() = default;
TryExceptPayload::~TryExceptPayload() = default;
RaiseStmtPayload::~RaiseStmtPayload() = default;
CheckStmtPayload::~CheckStmtPayload() = default;
ImportStmtPayload::~ImportStmtPayload() = default;
WithStmtPayload::~WithStmtPayload() = default;
BlockStmt::~BlockStmt() = default;

ExprNodePtr make_literal_expr(SourceSpan Span, const LiteralValue &Literal)
{
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::LiteralExpr;
   node->span = Span;
   node->data = Literal;
   return node;
}

ExprNodePtr make_identifier_expr(SourceSpan Span, const NameRef &Reference)
{
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::IdentifierExpr;
   node->span = Span;
   node->data = Reference;
   return node;
}

ExprNodePtr make_vararg_expr(SourceSpan Span)
{
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::VarArgExpr;
   node->span = Span;
   node->data.emplace<VarArgExprPayload>();
   return node;
}

ExprNodePtr make_unary_expr(SourceSpan Span, AstUnaryOperator op, ExprNodePtr Operand)
{
   assert_node(ensure_operand(Operand), "unary expression requires operand");
   UnaryExprPayload payload;
   payload.op = op;
   payload.operand = std::move(Operand);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::UnaryExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_update_expr(SourceSpan Span, AstUpdateOperator op, bool is_postfix,
   ExprNodePtr target)
{
   assert_node(ensure_operand(target), "update expression requires target");
   UpdateExprPayload payload;
   payload.op = op;
   payload.is_postfix = is_postfix;
   payload.target = std::move(target);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::UpdateExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_binary_expr(SourceSpan Span, AstBinaryOperator op, ExprNodePtr left,
   ExprNodePtr right)
{
   assert_node(ensure_operand(left) and ensure_operand(right), "binary expression requires operands");
   BinaryExprPayload payload;
   payload.op = op;
   payload.left = std::move(left);
   payload.right = std::move(right);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::BinaryExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_ternary_expr(SourceSpan Span, ExprNodePtr condition, ExprNodePtr if_true, ExprNodePtr if_false)
{
   assert_node(ensure_operand(condition) and ensure_operand(if_true) and ensure_operand(if_false),
      "ternary expression requires three operands");
   TernaryExprPayload payload;
   payload.condition = std::move(condition);
   payload.if_true = std::move(if_true);
   payload.if_false = std::move(if_false);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::TernaryExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_presence_expr(SourceSpan Span, ExprNodePtr value)
{
   assert_node(ensure_operand(value), "presence expression requires operand");
   PresenceExprPayload payload;
   payload.value = std::move(value);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::PresenceExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_pipe_expr(SourceSpan Span, ExprNodePtr lhs, ExprNodePtr rhs_call, uint32_t limit)
{
   assert_node(ensure_operand(lhs) and ensure_operand(rhs_call), "pipe expression requires lhs and rhs_call");
   PipeExprPayload payload;
   payload.lhs = std::move(lhs);
   payload.rhs_call = std::move(rhs_call);
   payload.limit = limit;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::PipeExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

//********************************************************************************************************************
// Helper to detect obj.new("classname", ...) pattern and extract class information.
// Returns true if the pattern is detected and sets result_type and object_class_id.

static bool detect_obj_new_call(const ExprNode &Callee, const ExprNodeList &Arguments, TiriType &ResultType, CLASSID &ClassID)
{
   // Pattern: obj.new("classname", ...) where callee is MemberExpr(obj, "new")
   if (Callee.kind != AstNodeKind::MemberExpr) return false;

   const auto& member_payload = std::get<MemberExprPayload>(Callee.data);

   // Check if member name is "new"
   if (not member_payload.member.symbol) return false;
   if (strcmp(strdata(member_payload.member.symbol), "new") != 0) return false;

   // Check if table is identifier "obj"
   if (not member_payload.table) return false;
   if (member_payload.table->kind != AstNodeKind::IdentifierExpr) return false;

   const auto& name_ref = std::get<NameRef>(member_payload.table->data);
   if (not name_ref.identifier.symbol) return false;
   if (strcmp(strdata(name_ref.identifier.symbol), "obj") != 0) return false;

   ResultType = TiriType::Object; // obj.new() always returns an Object

   // Check if first argument is a string literal (class name)
   if (Arguments.empty()) {
      // TODO: Report a parsing error, class name is required
      return true;
   }

   const ExprNode& first_arg = *Arguments[0];
   if (first_arg.kind != AstNodeKind::LiteralExpr) return true;

   const auto &literal = std::get<LiteralValue>(first_arg.data);
   if (literal.kind != LiteralKind::String or not literal.string_value) {
      // Technically allowed, object type will be class-less
      return true;
   }

   // Compute CLASSID from class name (case-insensitive hash)
   std::string_view class_name(strdata(literal.string_value), literal.string_value->len);
   ClassID = CLASSID(pf::strihash(class_name));
   return true;
}

//********************************************************************************************************************

ExprNodePtr make_call_expr(SourceSpan Span, ExprNodePtr callee, ExprNodeList arguments, bool forwards_multret)
{
   assert_node(ensure_operand(callee), "call expression requires callee");
   CallExprPayload payload;

   // Detect obj.new("classname", ...) pattern before moving callee
   detect_obj_new_call(*callee, arguments, payload.result_type, payload.object_class_id);

   // Mark MemberExpr/SafeMemberExpr callees as call targets so type-checking can be deferred to runtime
   if (callee->kind IS AstNodeKind::MemberExpr) {
      auto *member_payload = std::get_if<MemberExprPayload>(&callee->data);
      if (member_payload) member_payload->is_call_target = true;
   }
   else if (callee->kind IS AstNodeKind::SafeMemberExpr) {
      auto *member_payload = std::get_if<SafeMemberExprPayload>(&callee->data);
      if (member_payload) member_payload->is_call_target = true;
   }

   DirectCallTarget target;
   target.callable = std::move(callee);
   payload.target = std::move(target);
   payload.arguments = std::move(arguments);
   payload.forwards_multret = forwards_multret;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::CallExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_method_call_expr(SourceSpan Span, ExprNodePtr receiver, Identifier method, ExprNodeList arguments,
   bool forwards_multret)
{
   assert_node(ensure_operand(receiver), "method call requires receiver");
   CallExprPayload payload;
   MethodCallTarget target;
   target.receiver = std::move(receiver);
   target.method = method;
   payload.target = std::move(target);
   payload.arguments = std::move(arguments);
   payload.forwards_multret = forwards_multret;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::CallExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_safe_method_call_expr(SourceSpan Span, ExprNodePtr receiver, Identifier method, ExprNodeList arguments,
   bool forwards_multret)
{
   assert_node(ensure_operand(receiver), "safe method call requires receiver");
   CallExprPayload payload;
   SafeMethodCallTarget target;
   target.receiver = std::move(receiver);
   target.method = method;
   payload.target = std::move(target);
   payload.arguments = std::move(arguments);
   payload.forwards_multret = forwards_multret;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::SafeCallExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_member_expr(SourceSpan Span, ExprNodePtr Table, Identifier member, bool uses_method_dispatch)
{
   assert_node(ensure_operand(Table), "member expression requires table value");
   MemberExprPayload payload;
   // Infer base type before moving the table expression
   payload.base_type = infer_expression_type(*Table);
   payload.table = std::move(Table);
   payload.member = member;
   payload.uses_method_dispatch = uses_method_dispatch;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::MemberExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_index_expr(SourceSpan Span, ExprNodePtr Table, ExprNodePtr index)
{
   assert_node(ensure_operand(Table) and ensure_operand(index), "index expression requires operands");
   IndexExprPayload payload;
   payload.table = std::move(Table);
   payload.index = std::move(index);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::IndexExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_safe_member_expr(SourceSpan Span, ExprNodePtr Table, Identifier Member)
{
   assert_node(ensure_operand(Table), "safe member expression requires table value");
   SafeMemberExprPayload payload;
   // Infer base type before moving the table expression
   payload.base_type = infer_expression_type(*Table);
   payload.table = std::move(Table);
   payload.member = Member;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::SafeMemberExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_safe_index_expr(SourceSpan Span, ExprNodePtr Table, ExprNodePtr Index)
{
   assert_node(ensure_operand(Table) and ensure_operand(Index), "safe index expression requires operands");
   SafeIndexExprPayload payload;
   payload.table = std::move(Table);
   payload.index = std::move(Index);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::SafeIndexExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_result_filter_expr(SourceSpan Span, ExprNodePtr Expression, uint64_t KeepMask,
   uint8_t ExplicitCount, bool TrailingKeep)
{
   assert_node(ensure_operand(Expression), "result filter expression requires call expression");
   ResultFilterPayload payload;
   payload.expression = std::move(Expression);
   payload.keep_mask = KeepMask;
   payload.explicit_count = ExplicitCount;
   payload.trailing_keep = TrailingKeep;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::ResultFilterExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_table_expr(SourceSpan Span, std::vector<TableField> fields, bool has_array_part)
{
   TableExprPayload payload;
   payload.fields = std::move(fields);
   payload.has_array_part = has_array_part;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::TableExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_function_expr(SourceSpan Span, std::vector<FunctionParameter> parameters, bool is_vararg, std::unique_ptr<BlockStmt> body, bool IsThunk, TiriType ThunkReturnType, FunctionReturnTypes ReturnTypes)
{
   assert_node(body != nullptr, "function literal body required");
   FunctionExprPayload payload;
   payload.parameters = std::move(parameters);
   payload.is_vararg = is_vararg;
   payload.is_thunk = IsThunk;
   payload.thunk_return_type = ThunkReturnType;
   payload.return_types = ReturnTypes;
   payload.body = std::move(body);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::FunctionExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_deferred_expr(SourceSpan Span, ExprNodePtr inner, TiriType Type, bool TypeExplicit)
{
   assert_node(ensure_operand(inner), "deferred expression requires inner expression");
   DeferredExprPayload payload;
   payload.inner = std::move(inner);
   payload.deferred_type = Type;
   payload.type_explicit = TypeExplicit;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::DeferredExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_range_expr(SourceSpan Span, ExprNodePtr Start, ExprNodePtr Stop, bool Inclusive)
{
   assert_node(ensure_operand(Start), "range expression requires start expression");
   assert_node(ensure_operand(Stop), "range expression requires stop expression");
   RangeExprPayload payload;
   payload.start = std::move(Start);
   payload.stop = std::move(Stop);
   payload.inclusive = Inclusive;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::RangeExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_choose_expr(SourceSpan Span, ExprNodePtr Scrutinee, std::vector<ChooseCase> Cases, size_t InferredArity)
{
   assert_node(ensure_operand(Scrutinee), "choose expression requires scrutinee expression");
   ChooseExprPayload payload;
   payload.scrutinee = std::move(Scrutinee);
   payload.cases = std::move(Cases);
   payload.inferred_tuple_arity = InferredArity;
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::ChooseExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

ExprNodePtr make_choose_expr_tuple(SourceSpan Span, ExprNodeList ScrutineeTuple, std::vector<ChooseCase> Cases)
{
   assert_node(ScrutineeTuple.size() >= 2, "tuple scrutinee requires at least 2 elements");
   ChooseExprPayload payload;
   payload.scrutinee_tuple = std::move(ScrutineeTuple);
   payload.cases = std::move(Cases);
   ExprNodePtr node = std::make_unique<ExprNode>();
   node->kind = AstNodeKind::ChooseExpr;
   node->span = Span;
   node->data = std::move(payload);
   return node;
}

std::unique_ptr<FunctionExprPayload> make_function_payload(std::vector<FunctionParameter> parameters,
   bool is_vararg, std::unique_ptr<BlockStmt> body, bool IsThunk, TiriType ThunkReturnType, FunctionReturnTypes ReturnTypes)
{
   assert_node(body != nullptr, "function body required");
   auto payload = std::make_unique<FunctionExprPayload>();
   payload->parameters = std::move(parameters);
   payload->is_vararg = is_vararg;
   payload->is_thunk = IsThunk;
   payload->thunk_return_type = ThunkReturnType;
   payload->return_types = ReturnTypes;
   payload->body = std::move(body);
   return payload;
}

std::unique_ptr<BlockStmt> make_block(SourceSpan Span, StmtNodeList statements)
{
   auto block = std::make_unique<BlockStmt>();
   block->span = Span;
   block->statements = std::move(statements);
   return block;
}

StmtNodePtr make_assignment_stmt(SourceSpan Span, AssignmentOperator op, ExprNodeList targets, ExprNodeList values)
{
   StmtNodePtr node = std::make_unique<StmtNode>();
   node->kind = AstNodeKind::AssignmentStmt;
   node->span = Span;
   AssignmentStmtPayload payload(op, std::move(targets), std::move(values));
   node->data = std::move(payload);
   return node;
}

StmtNodePtr make_local_decl_stmt(SourceSpan Span, std::vector<Identifier> names, ExprNodeList values)
{
   StmtNodePtr node = std::make_unique<StmtNode>();
   node->kind = AstNodeKind::LocalDeclStmt;
   node->span = Span;
   LocalDeclStmtPayload payload(AssignmentOperator::Plain, std::move(names), std::move(values));
   node->data = std::move(payload);
   return node;
}

StmtNodePtr make_return_stmt(SourceSpan Span, ExprNodeList values, bool forwards_call)
{
   StmtNodePtr node = std::make_unique<StmtNode>();
   node->kind = AstNodeKind::ReturnStmt;
   node->span = Span;
   ReturnStmtPayload payload(std::move(values), forwards_call);
   node->data = std::move(payload);
   return node;
}

StmtNodePtr make_expression_stmt(SourceSpan Span, ExprNodePtr expression)
{
   StmtNodePtr node = std::make_unique<StmtNode>();
   node->kind = AstNodeKind::ExpressionStmt;
   node->span = Span;
   ExpressionStmtPayload payload(std::move(expression));
   node->data = std::move(payload);
   return node;
}

size_t ast_statement_child_count(const StmtNode &Node)
{
   return std::visit(StatementChildCounter{}, Node.data);
}

size_t ast_expression_child_count(const ExprNode &Node)
{
   return std::visit(ExpressionChildCounter{}, Node.data);
}

//********************************************************************************************************************
// Constructor for Identifier that creates an identifier from a string.

Identifier::Identifier(lua_State* L, const char* Name, SourceSpan Span)
   : symbol(lj_str_new(L, Name, std::strlen(Name))), span(Span), is_blank(false), has_close(false), type(TiriType::Unknown)
{
}
