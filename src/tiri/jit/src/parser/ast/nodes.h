// Lua parser - AST/IR node schema
// Copyright © 2025-2026 Paul Manias
//
// The schema mirrors every construct currently handled by the LuaJIT parser and describes how child nodes, source
// locations and semantic attributes are stored.
//
//  *  Expressions include literals, identifiers, calls, table constructors, unary/binary operators (including Tiri
//     extensions such as `??`, ternary selections and postfix increment), function literals and suffix operations
//     (field access, indexing, method calls, presence checks).
//  *  Statements cover assignments, declarations, control-flow (if/while/repeat, numeric & generic for loops,
//     break/continue), defer blocks, returns, chunk/local blocks and bare expression statements.
//  *  Dedicated structs capture reusable metadata (Identifier, NameRef, FunctionParameter, TableField, BlockStmt) so
//     later work can extend the IR without mutating parser internals.
//
// Nodes own their children through std::unique_ptr / std::vector so lifetime is explicit and node hierarchies can be
// transferred between passes. Every node stores a SourceSpan for diagnostics, and lightweight view helpers expose the
// contents of frequently iterated collections without leaking storage details.  Extensions should prefer adding new
// structs + AstNodeKind tags rather than repurposing existing payloads; see the "Extension guidelines" comment near
// the end of this file.
//
// Extension guidelines
// 1. Add a new AstNodeKind entry and dedicated payload struct. Do NOT overload existing
//    payloads unless semantics are identical.
// 2. Prefer storing ownership via std::unique_ptr/std::vector so that AST lifetimes are
//    explicit. Views (StatementListView / ExpressionListView) should wrap storage instead
//    of returning raw references.
// 3. Always assign a SourceSpan when constructing nodes. Builder helpers above centralise
//    invariant checks; extend them when new nodes require validation.
// 4. Keep Identifier/NameRef semantics stable so scope resolution can be threaded through
//    later passes without rewriting node shapes.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lexer.h"

class ParserDiagnostics;

// Forward declarations for recursive ownership.
struct ExprNode;
struct StmtNode;
struct BlockStmt;

using ExprNodePtr = std::unique_ptr<ExprNode>;
using StmtNodePtr = std::unique_ptr<StmtNode>;
using ExprNodeList = std::vector<ExprNodePtr>;
using StmtNodeList = std::vector<StmtNodePtr>;

// Function return type declaration for static analysis
struct FunctionReturnTypes {
   std::array<TiriType, MAX_RETURN_TYPES> types{};  // Return types (Unknown = unused slot)
   uint8_t count = 0;           // Number of declared types (0 = not declared)
   bool is_variadic = false;    // True if declaration ends with ... (last type repeats)
   bool is_explicit = false;    // True if explicitly declared, false if inferred

   [[nodiscard]] bool is_void() const { return count IS 0 and is_explicit; }
   [[nodiscard]] bool is_any() const { return count IS 1 and types[0] IS TiriType::Any; }
   [[nodiscard]] TiriType type_at(size_t Index) const {
      if (Index >= count) return is_variadic ? types[count - 1] : TiriType::Unknown;
      return types[Index];
   }
};

// Convert type name string to TiriType
[[nodiscard]] TiriType parse_type_name(std::string_view Name);

// Convert TiriType to display string
[[nodiscard]] std::string_view type_name(TiriType Type);

// Infer the result type of an expression from its AST structure
// Returns TiriType::Unknown if type cannot be inferred (e.g., function calls without return type info)
[[nodiscard]] TiriType infer_expression_type(const ExprNode& Expr);

// Extended type inference that also returns object class ID for object types
struct InferredTypeInfo {
   TiriType type = TiriType::Unknown;
   CLASSID object_class_id = CLASSID::NIL; // Object type identifier
};
[[nodiscard]] InferredTypeInfo infer_expression_type_ext(const ExprNode& Expr);

// Convert TiriType to LJ type tag base value (0-13)
// The LJ_T* tags are defined as ~value, so LJ_TNIL = ~0, LJ_TSTR = ~4, etc.
// Returns 0xFF (255) for Unknown/Any types that should trigger evaluation
[[nodiscard]] uint8_t tiri_type_to_lj_tag(TiriType Type);

enum class LiteralKind : uint8_t {
   Nil,
   Boolean,
   Number,
   String
};

// Annotation argument value - supports the types allowed in annotation syntax
struct AnnotationArgValue {
   enum class Type : uint8_t { Nil, Bool, Number, String, Array };
   Type type = Type::Nil;
   bool bool_value = false;
   double number_value = 0.0;
   GCstr* string_value = nullptr;
   std::vector<AnnotationArgValue> array_value;
};

// Single annotation entry with name and arguments
struct AnnotationEntry {
   GCstr* name = nullptr;                                     // Annotation name (e.g., "Test", "Requires")
   SourceSpan span{};                                         // Source location of the annotation
   std::vector<std::pair<GCstr*, AnnotationArgValue>> args;   // Named arguments (key-value pairs)
};

enum class AstUnaryOperator : uint8_t {
   Negate,
   Not,
   Length,
   BitNot
};

enum class AstUpdateOperator : uint8_t {
   Increment,
   Decrement
};

enum class AstBinaryOperator : uint8_t {
   Add,
   Subtract,
   Multiply,
   Divide,
   Modulo,
   Power,
   Concat,
   NotEqual,
   Equal,
   LessThan,
   GreaterEqual,
   LessEqual,
   GreaterThan,
   BitAnd,
   BitOr,
   BitXor,
   ShiftLeft,
   ShiftRight,
   LogicalAnd,
   LogicalOr,
   IfEmpty,
   HasFlag
};

enum class NameResolution : uint8_t {
   Unresolved,
   Local,
   Upvalue,
   Global,
   Environment
};

enum class TableFieldKind : uint8_t {
   Array,
   Record,
   Computed
};

enum class AssignmentOperator : uint8_t {
   Plain,
   Add,
   Subtract,
   Multiply,
   Divide,
   Modulo,
   Concat,
   IfEmpty,
   IfNil
};

enum class LoopStyle : uint8_t {
   WhileLoop,
   RepeatUntil
};

enum class CallDispatch : uint8_t {
   Direct,
   Method
};

//********************************************************************************************************************

struct Identifier {
   GCstr *symbol = nullptr;
   SourceSpan span{};
   bool is_blank = false;
   bool has_close = false;
   bool has_const = false;  // Const attribute flag - variable cannot be reassigned
   TiriType type = TiriType::Unknown;  // Explicit type annotation (Unknown = no annotation)

   // Default constructor
   Identifier() = default;

   // Constructor for creating identifier from a string
   // Uses lua_State to intern the string
   Identifier(lua_State* L, const char* Name, SourceSpan Span = {});

   // Constructor for creating identifier using keepstr (LexState method)
   // This variant is used when the string needs to be kept in the lexer's string table
   static Identifier from_keepstr(GCstr* Symbol, SourceSpan Span = {}) {
      Identifier id;
      id.symbol = Symbol;
      id.span = Span;
      id.is_blank = false;
      id.has_close = false;
      id.has_const = false;
      return id;
   }
};

struct NameRef {
   Identifier identifier;
   NameResolution resolution = NameResolution::Unresolved;
   uint16_t slot = 0;
};

struct LiteralValue {
   LiteralKind kind = LiteralKind::Nil;
   bool bool_value = false;
   lua_Number number_value = 0.0;
   GCstr *string_value = nullptr;

   // Default constructor (nil)
   LiteralValue() = default;

   // Construct a nil literal
   static LiteralValue nil() { return LiteralValue{}; }

   // Construct a boolean literal
   static LiteralValue boolean(bool Value) {
      LiteralValue lit;
      lit.kind = LiteralKind::Boolean;
      lit.bool_value = Value;
      return lit;
   }

   // Construct a number literal
   static LiteralValue number(lua_Number Value) {
      LiteralValue lit;
      lit.kind = LiteralKind::Number;
      lit.number_value = Value;
      return lit;
   }

   // Construct a string literal
   static LiteralValue string(GCstr* Value) {
      LiteralValue lit;
      lit.kind = LiteralKind::String;
      lit.string_value = Value;
      return lit;
   }
};

struct FunctionParameter {
   Identifier name;
   TiriType type = TiriType::Any;
   bool is_self = false;
};

struct DirectCallTarget {
   DirectCallTarget() = default;
   DirectCallTarget(const DirectCallTarget&) = delete;
   DirectCallTarget& operator=(const DirectCallTarget&) = delete;
   DirectCallTarget(DirectCallTarget&&) noexcept = default;
   DirectCallTarget& operator=(DirectCallTarget&&) noexcept = default;
   ExprNodePtr callable;
   ~DirectCallTarget();
};

struct MethodCallTarget {
   MethodCallTarget() = default;
   MethodCallTarget(const MethodCallTarget&) = delete;
   MethodCallTarget& operator=(const MethodCallTarget&) = delete;
   MethodCallTarget(MethodCallTarget&&) noexcept = default;
   MethodCallTarget& operator=(MethodCallTarget&&) noexcept = default;
   ExprNodePtr receiver;
   Identifier method;
   ~MethodCallTarget();
};

struct SafeMethodCallTarget {
   SafeMethodCallTarget() = default;
   SafeMethodCallTarget(const SafeMethodCallTarget&) = delete;
   SafeMethodCallTarget& operator=(const SafeMethodCallTarget&) = delete;
   SafeMethodCallTarget(SafeMethodCallTarget&&) noexcept = default;
   SafeMethodCallTarget& operator=(SafeMethodCallTarget&&) noexcept = default;
   ExprNodePtr receiver;
   Identifier method;
   ~SafeMethodCallTarget();
};

using CallTarget = std::variant<DirectCallTarget, MethodCallTarget, SafeMethodCallTarget>;

struct VarArgExprPayload {};

struct UnaryExprPayload {
   UnaryExprPayload() = default;
   UnaryExprPayload(const UnaryExprPayload&) = delete;
   UnaryExprPayload& operator=(const UnaryExprPayload&) = delete;
   UnaryExprPayload(UnaryExprPayload&&) noexcept = default;
   UnaryExprPayload& operator=(UnaryExprPayload&&) noexcept = default;
   AstUnaryOperator op = AstUnaryOperator::Negate;
   ExprNodePtr operand;
   ~UnaryExprPayload();
};

struct UpdateExprPayload {
   UpdateExprPayload() = default;
   UpdateExprPayload(const UpdateExprPayload&) = delete;
   UpdateExprPayload& operator=(const UpdateExprPayload&) = delete;
   UpdateExprPayload(UpdateExprPayload&&) noexcept = default;
   UpdateExprPayload& operator=(UpdateExprPayload&&) noexcept = default;
   AstUpdateOperator op = AstUpdateOperator::Increment;
   bool is_postfix = false;
   ExprNodePtr target;
   ~UpdateExprPayload();
};

struct BinaryExprPayload {
   BinaryExprPayload() = default;
   BinaryExprPayload(const BinaryExprPayload&) = delete;
   BinaryExprPayload& operator=(const BinaryExprPayload&) = delete;
   BinaryExprPayload(BinaryExprPayload&&) noexcept = default;
   BinaryExprPayload& operator=(BinaryExprPayload&&) noexcept = default;
   AstBinaryOperator op = AstBinaryOperator::Add;
   ExprNodePtr left;
   ExprNodePtr right;
   ~BinaryExprPayload();
};

struct TernaryExprPayload {
   TernaryExprPayload() = default;
   TernaryExprPayload(const TernaryExprPayload&) = delete;
   TernaryExprPayload& operator=(const TernaryExprPayload&) = delete;
   TernaryExprPayload(TernaryExprPayload&&) noexcept = default;
   TernaryExprPayload& operator=(TernaryExprPayload&&) noexcept = default;
   ExprNodePtr condition;
   ExprNodePtr if_true;
   ExprNodePtr if_false;
   ~TernaryExprPayload();
};

struct PresenceExprPayload {
   PresenceExprPayload() = default;
   PresenceExprPayload(const PresenceExprPayload&) = delete;
   PresenceExprPayload& operator=(const PresenceExprPayload&) = delete;
   PresenceExprPayload(PresenceExprPayload&&) noexcept = default;
   PresenceExprPayload& operator=(PresenceExprPayload&&) noexcept = default;
   ExprNodePtr value;
   ~PresenceExprPayload();
};

struct PipeExprPayload {
   PipeExprPayload() = default;
   PipeExprPayload(const PipeExprPayload&) = delete;
   PipeExprPayload& operator=(const PipeExprPayload&) = delete;
   PipeExprPayload(PipeExprPayload&&) noexcept = default;
   PipeExprPayload& operator=(PipeExprPayload&&) noexcept = default;
   ExprNodePtr lhs;              // Left-hand side expression (piped value)
   ExprNodePtr rhs_call;         // Right-hand side call expression (or function ref for deferred iteration)
   uint32_t limit = 0;           // Result limit (0 = unlimited)
   bool deferred_iteration = false; // True when RHS is a function ref and LHS type is unknown at parse time
   ~PipeExprPayload();
};

struct CallExprPayload {
   CallExprPayload() = default;
   CallExprPayload(const CallExprPayload&) = delete;
   CallExprPayload& operator=(const CallExprPayload&) = delete;
   CallExprPayload(CallExprPayload&&) noexcept = default;
   CallExprPayload& operator=(CallExprPayload&&) noexcept = default;
   CallTarget target;
   ExprNodeList arguments;
   bool forwards_multret = false;
   mutable TiriType result_type = TiriType::Unknown;  // Inferred return type (e.g., Object for obj.new())
   mutable CLASSID object_class_id = CLASSID::NIL; // CLASSID if result is Object
   ~CallExprPayload();
};

struct MemberExprPayload {
   MemberExprPayload() = default;
   MemberExprPayload(const MemberExprPayload&) = delete;
   MemberExprPayload& operator=(const MemberExprPayload&) = delete;
   MemberExprPayload(MemberExprPayload&&) noexcept = default;
   MemberExprPayload& operator=(MemberExprPayload&&) noexcept = default;
   ExprNodePtr table;
   Identifier member;
   bool uses_method_dispatch = false;
   bool is_call_target = false;                       // True if this expression is the callee of a function call
   mutable TiriType base_type = TiriType::Unknown;  // Known type of the base (table/object) expression
   mutable CLASSID class_id = CLASSID::NIL;           // CLASSID if base is Object
   ~MemberExprPayload();
};

struct IndexExprPayload {
   IndexExprPayload() = default;
   IndexExprPayload(const IndexExprPayload&) = delete;
   IndexExprPayload& operator=(const IndexExprPayload&) = delete;
   IndexExprPayload(IndexExprPayload&&) noexcept = default;
   IndexExprPayload& operator=(IndexExprPayload&&) noexcept = default;
   ExprNodePtr table;
   ExprNodePtr index;
   mutable TiriType base_type = TiriType::Unknown;  // Known type of the base (table/array) expression
   ~IndexExprPayload();
};

struct SafeMemberExprPayload {
   SafeMemberExprPayload() = default;
   SafeMemberExprPayload(const SafeMemberExprPayload&) = delete;
   SafeMemberExprPayload& operator=(const SafeMemberExprPayload&) = delete;
   SafeMemberExprPayload(SafeMemberExprPayload&&) noexcept = default;
   SafeMemberExprPayload& operator=(SafeMemberExprPayload&&) noexcept = default;
   ExprNodePtr table;
   Identifier member;
   bool is_call_target = false;                       // True if this expression is the callee of a function call
   mutable TiriType base_type = TiriType::Unknown;  // Known type of the base (table/object) expression
   mutable CLASSID class_id = CLASSID::NIL;           // CLASSID if base is Object
   ~SafeMemberExprPayload();
};

struct SafeIndexExprPayload {
   SafeIndexExprPayload() = default;
   SafeIndexExprPayload(const SafeIndexExprPayload&) = delete;
   SafeIndexExprPayload& operator=(const SafeIndexExprPayload&) = delete;
   SafeIndexExprPayload(SafeIndexExprPayload&&) noexcept = default;
   SafeIndexExprPayload& operator=(SafeIndexExprPayload&&) noexcept = default;
   ExprNodePtr table;
   ExprNodePtr index;
   mutable TiriType base_type = TiriType::Unknown;  // Known type of the base (table/array) expression
   ~SafeIndexExprPayload();
};

struct ResultFilterPayload {
   ResultFilterPayload() = default;
   ResultFilterPayload(const ResultFilterPayload&) = delete;
   ResultFilterPayload& operator=(const ResultFilterPayload&) = delete;
   ResultFilterPayload(ResultFilterPayload&&) noexcept = default;
   ResultFilterPayload& operator=(ResultFilterPayload&&) noexcept = default;

   ExprNodePtr expression;     // The wrapped expression (must be a call)
   uint64_t keep_mask = 0;     // Bitmask: bit N = 1 means keep value N
   uint8_t explicit_count = 0; // Number of explicitly specified positions
   bool trailing_keep = false; // True if last symbol was *, false if _

   ~ResultFilterPayload();
};

struct TableField {
   TableField() = default;
   TableField(const TableField&) = delete;
   TableField& operator=(const TableField&) = delete;
   TableField(TableField&&) noexcept = default;
   TableField& operator=(TableField&&) noexcept = default;
   TableFieldKind kind = TableFieldKind::Array;
   SourceSpan span{};
   std::optional<Identifier> name;
   ExprNodePtr key;
   ExprNodePtr value;
   ~TableField();
};

struct TableExprPayload {
   TableExprPayload() = default;
   TableExprPayload(const TableExprPayload&) = delete;
   TableExprPayload& operator=(const TableExprPayload&) = delete;
   TableExprPayload(TableExprPayload&&) noexcept = default;
   TableExprPayload& operator=(TableExprPayload&&) noexcept = default;
   std::vector<TableField> fields;
   bool has_array_part = false;
   ~TableExprPayload();
};

struct FunctionExprPayload {
   FunctionExprPayload() = default;
   FunctionExprPayload(const FunctionExprPayload&) = delete;
   FunctionExprPayload& operator=(const FunctionExprPayload&) = delete;
   FunctionExprPayload(FunctionExprPayload&&) noexcept = default;
   FunctionExprPayload& operator=(FunctionExprPayload&&) noexcept = default;
   std::vector<FunctionParameter> parameters;
   bool is_vararg = false;
   bool is_thunk = false;              // Marks function as thunk
   TiriType thunk_return_type = TiriType::Any;  // Return type for thunk (kept for IR emission compatibility)
   FunctionReturnTypes return_types{};            // General return type tracking for type checking
   std::unique_ptr<BlockStmt> body;
   std::vector<AnnotationEntry> annotations;  // Annotations attached to this function
   ~FunctionExprPayload();
};

// Deferred expression payload: wraps an expression for lazy evaluation <{ expr }> or <type{ expr }>
struct DeferredExprPayload {
   DeferredExprPayload() = default;
   DeferredExprPayload(const DeferredExprPayload&) = delete;
   DeferredExprPayload& operator=(const DeferredExprPayload&) = delete;
   DeferredExprPayload(DeferredExprPayload&&) noexcept = default;
   DeferredExprPayload& operator=(DeferredExprPayload&&) noexcept = default;
   ExprNodePtr inner;              // The wrapped expression to be evaluated lazily
   TiriType deferred_type = TiriType::Unknown;  // Expected result type (inferred or explicit)
   bool type_explicit = false;     // True if type was explicitly annotated (e.g. <string{ expr }>)
   ~DeferredExprPayload();
};

// Range expression payload: represents {start..stop} or {start...stop} literal syntax

struct RangeExprPayload {
   RangeExprPayload() = default;
   RangeExprPayload(const RangeExprPayload&) = delete;
   RangeExprPayload& operator=(const RangeExprPayload&) = delete;
   RangeExprPayload(RangeExprPayload&&) noexcept = default;
   RangeExprPayload& operator=(RangeExprPayload&&) noexcept = default;
   ExprNodePtr start;      // Start index expression
   ExprNodePtr stop;       // Stop index expression
   bool inclusive = false; // True for ... (inclusive), false for .. (exclusive)
   ~RangeExprPayload();
};

// Relational operator for choose expression patterns

enum class ChooseRelationalOp : uint8_t {
   None,         // Equality comparison (default)
   LessThan,     // < pattern
   LessEqual,    // <= pattern
   GreaterThan,  // > pattern
   GreaterEqual  // >= pattern
};

// A single case arm in a choose expression

struct ChooseCase {
   ChooseCase() = default;
   ChooseCase(const ChooseCase&) = delete;
   ChooseCase& operator=(const ChooseCase&) = delete;
   ChooseCase(ChooseCase&&) noexcept = default;
   ChooseCase& operator=(ChooseCase&&) noexcept = default;

   ExprNodePtr pattern;     // Pattern to match (nullptr for else or wildcard or tuple)
   ExprNodeList tuple_patterns;    // Patterns for tuple match positions (empty if single-value)
   std::vector<bool> tuple_wildcards; // True for wildcard positions in tuple patterns
   ExprNodePtr guard;       // Optional when clause (nullptr if none)
   ExprNodePtr result;      // Result expression (nullptr if statement result)
   StmtNodePtr result_stmt; // Result statement for statement context (nullptr if expression result)
   bool is_else = false;    // True if this is the else branch
   bool is_wildcard = false; // True if pattern is _ (matches any value, no comparison)
   bool is_table_pattern = false; // True if pattern is { key = value, ... } (table pattern)
   bool is_tuple_pattern = false; // True if pattern is (p1, p2, ...) (tuple pattern)
   bool has_statement_result = false; // True if result is a statement (assignment) rather than expression
   ChooseRelationalOp relational_op = ChooseRelationalOp::None; // Relational pattern operator (< <= > >=)
   SourceSpan span{};

   ~ChooseCase();
};

// Choose expression payload: choose scrutinee from pattern -> result ... end
struct ChooseExprPayload {
   ChooseExprPayload() = default;
   ChooseExprPayload(const ChooseExprPayload&) = delete;
   ChooseExprPayload& operator=(const ChooseExprPayload&) = delete;
   ChooseExprPayload(ChooseExprPayload&&) noexcept = default;
   ChooseExprPayload& operator=(ChooseExprPayload&&) noexcept = default;

   ExprNodePtr scrutinee;           // Single value being matched (nullptr if tuple)
   ExprNodeList scrutinee_tuple;    // Tuple scrutinee elements (empty if single-value)
   std::vector<ChooseCase> cases;   // Pattern cases
   size_t inferred_tuple_arity = 0; // Non-zero when arity was inferred from first pattern (for func() returning N values)

   // Helper methods for tuple scrutinee
   bool is_tuple_scrutinee() const { return not scrutinee_tuple.empty(); }
   size_t tuple_arity() const { return scrutinee_tuple.size(); }
   bool has_inferred_arity() const { return inferred_tuple_arity > 0; }

   ~ChooseExprPayload();
};

struct ExprNode {
   AstNodeKind kind = AstNodeKind::LiteralExpr;
   SourceSpan span{};
   std::variant<LiteralValue, NameRef, VarArgExprPayload, UnaryExprPayload,
      UpdateExprPayload, BinaryExprPayload, TernaryExprPayload,
      PresenceExprPayload, PipeExprPayload, CallExprPayload, MemberExprPayload,
      IndexExprPayload, SafeMemberExprPayload, SafeIndexExprPayload,
      ResultFilterPayload, TableExprPayload, FunctionExprPayload, DeferredExprPayload,
      RangeExprPayload, ChooseExprPayload>
      data;

   ExprNode() = default;
   ExprNode(AstNodeKind Kind, SourceSpan Span) : kind(Kind), span(Span) {}
};

struct IfClause {
   IfClause() = default;
   IfClause(const IfClause&) = delete;
   IfClause& operator=(const IfClause&) = delete;
   IfClause(IfClause&&) noexcept = default;
   IfClause& operator=(IfClause&&) noexcept = default;
   ExprNodePtr condition;
   std::unique_ptr<BlockStmt> block;
   ~IfClause();
};

struct AssignmentStmtPayload {
   AssignmentStmtPayload() = default;
   AssignmentStmtPayload(AssignmentOperator op, ExprNodeList targets, ExprNodeList values)
      : op(op), targets(std::move(targets)), values(std::move(values)) {}
   AssignmentStmtPayload(const AssignmentStmtPayload&) = delete;
   AssignmentStmtPayload& operator=(const AssignmentStmtPayload&) = delete;
   AssignmentStmtPayload(AssignmentStmtPayload&&) noexcept = default;
   AssignmentStmtPayload& operator=(AssignmentStmtPayload&&) noexcept = default;
   AssignmentOperator op = AssignmentOperator::Plain;
   ExprNodeList targets;
   ExprNodeList values;
   ~AssignmentStmtPayload();
};

struct LocalDeclStmtPayload {
   LocalDeclStmtPayload(AssignmentOperator op, std::vector<Identifier> names, ExprNodeList values)
      : op(op), names(std::move(names)), values(std::move(values)) {}
   LocalDeclStmtPayload(const LocalDeclStmtPayload&) = delete;
   LocalDeclStmtPayload& operator=(const LocalDeclStmtPayload&) = delete;
   LocalDeclStmtPayload(LocalDeclStmtPayload&&) noexcept = default;
   LocalDeclStmtPayload& operator=(LocalDeclStmtPayload&&) noexcept = default;
   AssignmentOperator op = AssignmentOperator::Plain;  // Supports ??= conditional assignment
   std::vector<Identifier> names;
   ExprNodeList values;
   ~LocalDeclStmtPayload();
};

struct GlobalDeclStmtPayload {
   GlobalDeclStmtPayload(AssignmentOperator op, std::vector<Identifier> names, ExprNodeList values)
      : op(op), names(std::move(names)), values(std::move(values)) {}
   GlobalDeclStmtPayload(const GlobalDeclStmtPayload&) = delete;
   GlobalDeclStmtPayload& operator=(const GlobalDeclStmtPayload&) = delete;
   GlobalDeclStmtPayload(GlobalDeclStmtPayload&&) noexcept = default;
   GlobalDeclStmtPayload& operator=(GlobalDeclStmtPayload&&) noexcept = default;
   AssignmentOperator op = AssignmentOperator::Plain;  // Supports ??= conditional assignment
   std::vector<Identifier> names;
   ExprNodeList values;
   ~GlobalDeclStmtPayload();
};

struct LocalFunctionStmtPayload {
   LocalFunctionStmtPayload(Identifier name, std::unique_ptr<FunctionExprPayload> function)
      : name(std::move(name)), function(std::move(function)) {}
   LocalFunctionStmtPayload(const LocalFunctionStmtPayload&) = delete;
   LocalFunctionStmtPayload& operator=(const LocalFunctionStmtPayload&) = delete;
   LocalFunctionStmtPayload(LocalFunctionStmtPayload&&) noexcept = default;
   LocalFunctionStmtPayload& operator=(LocalFunctionStmtPayload&&) noexcept = default;
   Identifier name;
   std::unique_ptr<FunctionExprPayload> function;
   ~LocalFunctionStmtPayload();
};

struct FunctionNamePath {
   std::vector<Identifier> segments;
   std::optional<Identifier> method;
   bool is_explicit_global = false;  // True when declared with `global function`
};

struct FunctionStmtPayload {
   FunctionStmtPayload(FunctionNamePath name, std::unique_ptr<FunctionExprPayload> function)
      : name(std::move(name)), function(std::move(function)) {}
   FunctionStmtPayload(const FunctionStmtPayload&) = delete;
   FunctionStmtPayload& operator=(const FunctionStmtPayload&) = delete;
   FunctionStmtPayload(FunctionStmtPayload&&) noexcept = default;
   FunctionStmtPayload& operator=(FunctionStmtPayload&&) noexcept = default;
   FunctionNamePath name;
   std::unique_ptr<FunctionExprPayload> function;
   ~FunctionStmtPayload();
};

struct IfStmtPayload {
   IfStmtPayload(std::vector<IfClause> clauses)
      : clauses(std::move(clauses)) {}
   IfStmtPayload(const IfStmtPayload&) = delete;
   IfStmtPayload& operator=(const IfStmtPayload&) = delete;
   IfStmtPayload(IfStmtPayload&&) noexcept = default;
   IfStmtPayload& operator=(IfStmtPayload&&) noexcept = default;
   std::vector<IfClause> clauses;
   ~IfStmtPayload();
};

struct LoopStmtPayload {
   LoopStmtPayload(LoopStyle style, ExprNodePtr condition, std::unique_ptr<BlockStmt> body)
      : style(style), condition(std::move(condition)), body(std::move(body)) {}
   LoopStmtPayload(const LoopStmtPayload&) = delete;
   LoopStmtPayload& operator=(const LoopStmtPayload&) = delete;
   LoopStmtPayload(LoopStmtPayload&&) noexcept = default;
   LoopStmtPayload& operator=(LoopStmtPayload&&) noexcept = default;
   LoopStyle style = LoopStyle::WhileLoop;
   ExprNodePtr condition;
   std::unique_ptr<BlockStmt> body;
   ~LoopStmtPayload();
};

struct NumericForStmtPayload {
   NumericForStmtPayload(Identifier control, ExprNodePtr start, ExprNodePtr stop,
                         ExprNodePtr step, std::unique_ptr<BlockStmt> body)
      : control(std::move(control)), start(std::move(start)), stop(std::move(stop)),
        step(std::move(step)), body(std::move(body)) {}
   NumericForStmtPayload(const NumericForStmtPayload&) = delete;
   NumericForStmtPayload& operator=(const NumericForStmtPayload&) = delete;
   NumericForStmtPayload(NumericForStmtPayload&&) noexcept = default;
   NumericForStmtPayload& operator=(NumericForStmtPayload&&) noexcept = default;
   Identifier control;
   ExprNodePtr start;
   ExprNodePtr stop;
   ExprNodePtr step;
   std::unique_ptr<BlockStmt> body;
   ~NumericForStmtPayload();
};

struct GenericForStmtPayload {
   GenericForStmtPayload(std::vector<Identifier> names, ExprNodeList iterators,
                         std::unique_ptr<BlockStmt> body)
      : names(std::move(names)), iterators(std::move(iterators)), body(std::move(body)) {}
   GenericForStmtPayload(const GenericForStmtPayload&) = delete;
   GenericForStmtPayload& operator=(const GenericForStmtPayload&) = delete;
   GenericForStmtPayload(GenericForStmtPayload&&) noexcept = default;
   GenericForStmtPayload& operator=(GenericForStmtPayload&&) noexcept = default;
   std::vector<Identifier> names;
   ExprNodeList iterators;
   std::unique_ptr<BlockStmt> body;
   ~GenericForStmtPayload();
};

struct ReturnStmtPayload {
   ReturnStmtPayload(ExprNodeList values, bool forwards_call)
      : values(std::move(values)), forwards_call(forwards_call) {}
   ReturnStmtPayload(const ReturnStmtPayload&) = delete;
   ReturnStmtPayload& operator=(const ReturnStmtPayload&) = delete;
   ReturnStmtPayload(ReturnStmtPayload&&) noexcept = default;
   ReturnStmtPayload& operator=(ReturnStmtPayload&&) noexcept = default;
   ExprNodeList values;
   bool forwards_call = false;
   ~ReturnStmtPayload();
};

struct BreakStmtPayload {};

struct ContinueStmtPayload {};

struct DeferStmtPayload {
   DeferStmtPayload(std::unique_ptr<FunctionExprPayload> callable, ExprNodeList arguments = {})
      : callable(std::move(callable)), arguments(std::move(arguments)) {}
   DeferStmtPayload(const DeferStmtPayload&) = delete;
   DeferStmtPayload& operator=(const DeferStmtPayload&) = delete;
   DeferStmtPayload(DeferStmtPayload&&) noexcept = default;
   DeferStmtPayload& operator=(DeferStmtPayload&&) noexcept = default;
   std::unique_ptr<FunctionExprPayload> callable;
   ExprNodeList arguments;
   ~DeferStmtPayload();
};

struct DoStmtPayload {
   DoStmtPayload(std::unique_ptr<BlockStmt> block)
      : block(std::move(block)) {}
   DoStmtPayload(const DoStmtPayload&) = delete;
   DoStmtPayload& operator=(const DoStmtPayload&) = delete;
   DoStmtPayload(DoStmtPayload&&) noexcept = default;
   DoStmtPayload& operator=(DoStmtPayload&&) noexcept = default;
   std::unique_ptr<BlockStmt> block;
   ~DoStmtPayload();
};

struct ConditionalShorthandStmtPayload {
   ConditionalShorthandStmtPayload(ExprNodePtr condition, StmtNodePtr body)
      : condition(std::move(condition)), body(std::move(body)) {}
   ConditionalShorthandStmtPayload(const ConditionalShorthandStmtPayload&) = delete;
   ConditionalShorthandStmtPayload& operator=(const ConditionalShorthandStmtPayload&) = delete;
   ConditionalShorthandStmtPayload(ConditionalShorthandStmtPayload&&) noexcept = default;
   ConditionalShorthandStmtPayload& operator=(ConditionalShorthandStmtPayload&&) noexcept = default;
   ExprNodePtr condition;
   StmtNodePtr body;
   ~ConditionalShorthandStmtPayload();
};

// Exception clause for try...except...end blocks
struct ExceptClause {
   ExceptClause() = default;
   ExceptClause(const ExceptClause&) = delete;
   ExceptClause& operator=(const ExceptClause&) = delete;
   ExceptClause(ExceptClause&&) noexcept = default;
   ExceptClause& operator=(ExceptClause&&) noexcept = default;

   std::optional<Identifier> exception_var;  // Optional: e in "except e when ..."
   ExprNodeList filter_codes;                // Error codes from when clause (empty = catch-all)
   std::unique_ptr<BlockStmt> block;         // except block body
   SourceSpan span{};

   ~ExceptClause();
};

// Try-except statement payload: try ... except ... success ... end
struct TryExceptPayload {
   TryExceptPayload() = default;
   TryExceptPayload(const TryExceptPayload&) = delete;
   TryExceptPayload& operator=(const TryExceptPayload&) = delete;
   TryExceptPayload(TryExceptPayload&&) noexcept = default;
   TryExceptPayload& operator=(TryExceptPayload&&) noexcept = default;

   std::unique_ptr<BlockStmt> try_block;       // The try body
   std::vector<ExceptClause> except_clauses;   // Zero or more except handlers
   std::unique_ptr<BlockStmt> success_block;   // Optional success block (runs after defers, only if no exception)
   bool enable_trace = false;                  // If true, capture stack trace on exception

   ~TryExceptPayload();
};

// Raise statement payload: raise error_code [, message]
struct RaiseStmtPayload {
   RaiseStmtPayload() = default;
   RaiseStmtPayload(const RaiseStmtPayload&) = delete;
   RaiseStmtPayload& operator=(const RaiseStmtPayload&) = delete;
   RaiseStmtPayload(RaiseStmtPayload&&) noexcept = default;
   RaiseStmtPayload& operator=(RaiseStmtPayload&&) noexcept = default;

   ExprNodePtr error_code;   // Required: expression evaluating to error code
   ExprNodePtr message;      // Optional: custom error message

   ~RaiseStmtPayload();
};

// Check statement payload: check expression
struct CheckStmtPayload {
   CheckStmtPayload() = default;
   CheckStmtPayload(const CheckStmtPayload&) = delete;
   CheckStmtPayload& operator=(const CheckStmtPayload&) = delete;
   CheckStmtPayload(CheckStmtPayload&&) noexcept = default;
   CheckStmtPayload& operator=(CheckStmtPayload&&) noexcept = default;

   ExprNodePtr error_code;   // Expression evaluating to error code

   ~CheckStmtPayload();
};

// Import statement payload: import 'library' - compile-time file inlining
struct ImportStmtPayload {
   ImportStmtPayload() = default;
   ImportStmtPayload(const ImportStmtPayload&) = delete;
   ImportStmtPayload& operator=(const ImportStmtPayload&) = delete;
   ImportStmtPayload(ImportStmtPayload&&) noexcept = default;
   ImportStmtPayload& operator=(ImportStmtPayload&&) noexcept = default;

   std::optional<Identifier> namespace_name;  // The local variable name (alias or default)
   std::string lib_path;                      // Resolved path to library file
   std::string default_namespace;             // The declared namespace for _LIB lookup
   std::unique_ptr<BlockStmt> inlined_body;   // Parsed content of imported file
   uint8_t file_source_idx = 0;               // FileSource index for this imported file

   ~ImportStmtPayload();
};

// With statement payload: with obj1, obj2 do ... end - auto-lock/unlock objects
struct WithStmtPayload {
   WithStmtPayload(ExprNodeList objects, std::unique_ptr<BlockStmt> block)
      : objects(std::move(objects)), block(std::move(block)) {}
   WithStmtPayload(const WithStmtPayload&) = delete;
   WithStmtPayload& operator=(const WithStmtPayload&) = delete;
   WithStmtPayload(WithStmtPayload&&) noexcept = default;
   WithStmtPayload& operator=(WithStmtPayload&&) noexcept = default;
   ExprNodeList objects;
   std::unique_ptr<BlockStmt> block;
   ~WithStmtPayload();
};

struct ExpressionStmtPayload {
   ExpressionStmtPayload(ExprNodePtr expression)
      : expression(std::move(expression)) {}
   ExpressionStmtPayload(const ExpressionStmtPayload&) = delete;
   ExpressionStmtPayload& operator=(const ExpressionStmtPayload&) = delete;
   ExpressionStmtPayload(ExpressionStmtPayload&&) noexcept = default;
   ExpressionStmtPayload& operator=(ExpressionStmtPayload&&) noexcept = default;
   ExprNodePtr expression;
   ~ExpressionStmtPayload();
};

//********************************************************************************************************************

struct StmtNode {
   AstNodeKind kind = AstNodeKind::ExpressionStmt;
   SourceSpan span{};
   std::variant<AssignmentStmtPayload, LocalDeclStmtPayload, GlobalDeclStmtPayload,
      LocalFunctionStmtPayload, FunctionStmtPayload, IfStmtPayload,
      LoopStmtPayload, NumericForStmtPayload, GenericForStmtPayload,
      ReturnStmtPayload, BreakStmtPayload, ContinueStmtPayload, DeferStmtPayload,
      DoStmtPayload, ConditionalShorthandStmtPayload, TryExceptPayload,
      RaiseStmtPayload, CheckStmtPayload, ImportStmtPayload, WithStmtPayload,
      ExpressionStmtPayload>
      data;

   StmtNode() = default;
   StmtNode(AstNodeKind Kind, SourceSpan Span) : kind(Kind), span(Span) {}
};

//********************************************************************************************************************

// Concept for AST node pointer types (ExprNodePtr, StmtNodePtr)
template<typename NodePtr>
concept AstNodePointer = requires(NodePtr ptr) {
   { *ptr } -> std::convertible_to<const typename NodePtr::element_type&>;
   { ptr.get() } -> std::same_as<typename NodePtr::element_type*>;
};

// Unified template for AST node list views
// Provides a read-only view over std::vector<std::unique_ptr<T>> collections.
template<AstNodePointer NodePtr>
class AstNodeListView {
   using NodeList = std::vector<NodePtr>;
   using NodeType = typename NodePtr::element_type;

public:
   constexpr AstNodeListView() noexcept = default;
   explicit constexpr AstNodeListView(const NodeList& Nodes) noexcept : storage(&Nodes) {}

   class Iterator {
   public:
      using InnerIterator = typename NodeList::const_iterator;
      using iterator_category = std::forward_iterator_tag;
      using difference_type = std::ptrdiff_t;
      using value_type = const NodeType;
      using pointer = const NodeType*;
      using reference = const NodeType&;

      Iterator() = default;
      explicit Iterator(InnerIterator It) : iter(It) {}
      reference operator*() const { return *(*this->iter); }
      pointer operator->() const { return this->iter->get(); }
      Iterator& operator++() { ++this->iter; return *this; }
      bool operator==(const Iterator& Other) const { return this->iter IS Other.iter; }
      bool operator!=(const Iterator& Other) const { return not(*this IS Other); }

   private:
      InnerIterator iter{};
   };

   [[nodiscard]] const NodeType& operator[](size_t Index) const {
      lj_assertX(this->storage and Index < this->storage->size(), "Node index out of range");
      return *(*this->storage)[Index];
   }

   [[nodiscard]] Iterator begin() const { return this->storage ? Iterator(this->storage->begin()) : Iterator(); }
   [[nodiscard]] Iterator end() const { return this->storage ? Iterator(this->storage->end()) : Iterator(); }
   [[nodiscard]] size_t size() const noexcept { return this->storage ? this->storage->size() : 0; }
   [[nodiscard]] bool empty() const noexcept { return this->size() IS 0; }

private:
   const NodeList* storage = nullptr;
};

// Type aliases for specific node list views
using StatementListView = AstNodeListView<StmtNodePtr>;
using ExpressionListView = AstNodeListView<ExprNodePtr>;

//********************************************************************************************************************

struct BlockStmt {
   SourceSpan span{};
   StmtNodeList statements;

   [[nodiscard]] StatementListView view() const { return StatementListView(this->statements); }

   ~BlockStmt();
};

//********************************************************************************************************************
// Builder helpers

ExprNodePtr make_literal_expr(SourceSpan span, const LiteralValue& literal);
ExprNodePtr make_identifier_expr(SourceSpan span, const NameRef& reference);
ExprNodePtr make_vararg_expr(SourceSpan span);
ExprNodePtr make_unary_expr(SourceSpan span, AstUnaryOperator op, ExprNodePtr operand);
ExprNodePtr make_update_expr(SourceSpan span, AstUpdateOperator op, bool is_postfix, ExprNodePtr target);
ExprNodePtr make_binary_expr(SourceSpan span, AstBinaryOperator op, ExprNodePtr left, ExprNodePtr right);
ExprNodePtr make_ternary_expr(SourceSpan span, ExprNodePtr condition, ExprNodePtr if_true, ExprNodePtr if_false);
ExprNodePtr make_presence_expr(SourceSpan span, ExprNodePtr value);
ExprNodePtr make_pipe_expr(SourceSpan span, ExprNodePtr lhs, ExprNodePtr rhs_call, uint32_t limit);
ExprNodePtr make_call_expr(SourceSpan span, ExprNodePtr callee, ExprNodeList arguments, bool forwards_multret);
ExprNodePtr make_method_call_expr(SourceSpan span, ExprNodePtr receiver, Identifier method, ExprNodeList arguments, bool forwards_multret);
ExprNodePtr make_safe_method_call_expr(SourceSpan span, ExprNodePtr receiver, Identifier method, ExprNodeList arguments,
   bool forwards_multret);
ExprNodePtr make_member_expr(SourceSpan span, ExprNodePtr table, Identifier member, bool uses_method_dispatch);
ExprNodePtr make_index_expr(SourceSpan span, ExprNodePtr table, ExprNodePtr index);
ExprNodePtr make_safe_member_expr(SourceSpan span, ExprNodePtr table, Identifier member);
ExprNodePtr make_safe_index_expr(SourceSpan span, ExprNodePtr table, ExprNodePtr index);
ExprNodePtr make_result_filter_expr(SourceSpan span, ExprNodePtr expression, uint64_t keep_mask, uint8_t explicit_count, bool trailing_keep);
ExprNodePtr make_table_expr(SourceSpan span, std::vector<TableField> fields, bool has_array_part);
ExprNodePtr make_function_expr(SourceSpan span, std::vector<FunctionParameter> parameters, bool is_vararg, std::unique_ptr<BlockStmt> body, bool is_thunk = false, TiriType thunk_return_type = TiriType::Any, FunctionReturnTypes return_types = {});
ExprNodePtr make_deferred_expr(SourceSpan span, ExprNodePtr inner, TiriType type = TiriType::Unknown, bool type_explicit = false);
ExprNodePtr make_range_expr(SourceSpan span, ExprNodePtr start, ExprNodePtr stop, bool inclusive);
ExprNodePtr make_choose_expr(SourceSpan span, ExprNodePtr scrutinee, std::vector<ChooseCase> cases, size_t inferred_arity = 0);
ExprNodePtr make_choose_expr_tuple(SourceSpan span, ExprNodeList scrutinee_tuple, std::vector<ChooseCase> cases);
std::unique_ptr<FunctionExprPayload> make_function_payload(std::vector<FunctionParameter> parameters, bool is_vararg, std::unique_ptr<BlockStmt> body, bool is_thunk = false, TiriType thunk_return_type = TiriType::Any, FunctionReturnTypes return_types = {});
std::unique_ptr<BlockStmt> make_block(SourceSpan span, StmtNodeList statements);
StmtNodePtr make_assignment_stmt(SourceSpan span, AssignmentOperator op, ExprNodeList targets, ExprNodeList values);
StmtNodePtr make_local_decl_stmt(SourceSpan span, std::vector<Identifier> names, ExprNodeList values);
StmtNodePtr make_return_stmt(SourceSpan span, ExprNodeList values, bool forwards_call);
StmtNodePtr make_expression_stmt(SourceSpan span, ExprNodePtr expression);

[[nodiscard]] size_t ast_statement_child_count(const StmtNode& node);
[[nodiscard]] size_t ast_expression_child_count(const ExprNode& node);
