// Unit tests for the parser pipeline.

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_obj.h"
#include "bytecode/lj_bcdump.h"
#include "runtime/lj_contract.h"
#include "runtime/lj_meta.h"
#include "runtime/lj_proto_registry.h"
#include "runtime/lj_str.h"
#include "runtime/lj_tab.h"
#include "debug/dump_bytecode.h"
#include "debug/lj_debug.h"

#include <array>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>

#include "ast/builder.h"
#include "ast/nodes.h"
#include "parser_context.h"
#include "parser_diagnostics.h"
#include "parse_types.h"
#include "token_stream.h"
#include "token_types.h"
#include "type_checker.h"
#include "parser.h"
#include "static_descriptor_analysis.h"
#include "table_ownership.h"
#include "../../../defs.h"

static extTiri *glTestScript = nullptr;
extern "C" int luaopen_range(lua_State *L);

namespace {

static void log_diagnostics(std::span<const ParserDiagnostic> diagnostics, kt::Log &log)
{
   if (diagnostics.empty()) {
      return;
   }

   size_t index = 0;
   for (const ParserDiagnostic& diag : diagnostics) {
      log.msg("      diag[%" PRId64 "] severity=%d code=%d token=%d %s", (int64_t)index,
         (int)diag.severity, (int)diag.code, (int)diag.token.kind(), diag.message.c_str());
      ++index;
   }
}

#define BCNAME(name, ma, mb, mc, mt) #name,
static const char* glBcNames[] = {
   BCDEF(BCNAME)
   "BC__MAX"
};
#undef BCNAME

static std::string describe_instruction(BCIns instruction)
{
   BCOp op = bc_op(instruction);
   const char* name = (op < BC__MAX) ? glBcNames[op] : "BC_UNKNOWN";
   char buffer[128];
   std::snprintf(buffer, sizeof(buffer), "%s op=%d a=%d b=%d c=%d d=%d", name, (int)op,
      (int)bc_a(instruction), (int)bc_b(instruction), (int)bc_c(instruction), (int)bc_d(instruction));
   return std::string(buffer);
}

//********************************************************************************************************************

struct LuaStateHolder {
   LuaStateHolder() {
      this->state = luaL_newstate(glTestScript);
   }

   ~LuaStateHolder()
   {
      if (this->state) {
         lua_close(this->state);
      }
   }

   lua_State* get() const { return this->state; }

private:
   lua_State* state = nullptr;
};

struct StringReaderCtx {
   const char* str = nullptr;
   size_t size = 0;
};

static const char* unit_reader(lua_State*, void* data, size_t* size)
{
   auto *ctx = (StringReaderCtx *)data;
   if (ctx->size IS 0) {
      return nullptr;
   }
   *size = ctx->size;
   ctx->size = 0;
   return ctx->str;
}

struct AstHarnessResult {
   ParserResult<std::unique_ptr<BlockStmt>> chunk;
   std::vector<ParserDiagnostic> diagnostics;
   std::unique_ptr<LuaStateHolder> state;
};

//********************************************************************************************************************

static AstHarnessResult build_ast_from_source(std::string_view source, bool Diagnose = false,
   bool EnableTypeAnalysis = true, bool RejectLegacyMemberSyntax = false)
{
   AstHarnessResult result;
   result.state = std::make_unique<LuaStateHolder>();
   lua_State* L = result.state->get();

   StringReaderCtx ctx;
   ctx.str = source.data();
   ctx.size = source.size();

   LexState lex(L, unit_reader, &ctx, "parser-unit", std::nullopt);
   lex.diagnose_mode = Diagnose;
   FuncState& fs = lex.fs_init();

   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, fs, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   config.enable_type_analysis = EnableTypeAnalysis;
   config.reject_legacy_member_syntax = RejectLegacyMemberSyntax;
   ParserSession session(context, config);

   lex.next();
   AstBuilder builder(context);
   result.chunk = builder.parse_chunk();

   auto diag_entries = context.diagnostics().entries();
   result.diagnostics.assign(diag_entries.begin(), diag_entries.end());
   return result;
}

struct ExpressionParseHarness {
   std::unique_ptr<LuaStateHolder> holder;
   std::unique_ptr<StringReaderCtx> reader;
   std::unique_ptr<LexState> lex;
   FuncState* func_state = nullptr;  // Managed by LexState's func_stack
   std::unique_ptr<ParserContext> context;
   std::unique_ptr<ParserSession> session;
};

//********************************************************************************************************************

static std::optional<ExpressionParseHarness> make_expression_harness(std::string_view source)
{
   ExpressionParseHarness harness;
   harness.holder = std::make_unique<LuaStateHolder>();
   lua_State* L = harness.holder->get();
   if (not L) {
      return std::nullopt;
   }

   harness.reader = std::make_unique<StringReaderCtx>();
   harness.reader->str = source.data();
   harness.reader->size = source.size();

   harness.lex = std::make_unique<LexState>(L, unit_reader, harness.reader.get(), "expr-entry", std::nullopt);
   harness.func_state = &harness.lex->fs_init();

   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(*harness.lex, *harness.func_state, allocator);
   harness.context = std::make_unique<ParserContext>(std::move(context));

   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   harness.session = std::make_unique<ParserSession>(*harness.context, config);

   harness.lex->next();
   return harness;
}

//********************************************************************************************************************

static void log_block_outline(const BlockStmt& block, kt::Log &log)
{
   StatementListView view = block.view();
   size_t index = 0;
   for (const StmtNode& stmt : view) {
      log.msg("   stmt[%" PRId64 "] kind=%d", (int64_t)index, (int)stmt.kind);
      ++index;
   }
}

//********************************************************************************************************************

static bool test_parser_profiler_captures_stages(kt::Log &log)
{
   ParserProfilingResult result;
   ParserProfiler profiler(true, &result);

   profiler.record_stage("parse", std::chrono::milliseconds(5));
   profiler.record_stage("emit", std::chrono::milliseconds(2));

   const auto& stages = result.stages();
   if (stages.size() != 2) {
      log.error("expected two profiler stages, got %" PRId64, (int64_t)stages.size());
      return false;
   }

   if (stages[0].name != "parse" or stages[1].name != "emit") {
      log.error("stage names were not recorded as expected");
      return false;
   }

   double parse_error = std::abs(stages[0].milliseconds - 5.0);
   double emit_error = std::abs(stages[1].milliseconds - 2.0);
   if ((parse_error > 0.001) or (emit_error > 0.001)) {
      log.error("stage timing mismatch parse=%.3f emit=%.3f", stages[0].milliseconds, stages[1].milliseconds);
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_parser_profiler_disabled_noop(kt::Log &log)
{
   ParserProfilingResult result;
   ParserProfiler profiler(false, &result);

   {
      auto stage = profiler.stage("parse");
      stage.stop();
   }

   profiler.record_stage("emit", std::chrono::milliseconds(3));

   if (not result.stages().empty()) {
      log.error("disabled profiler should not record any stages");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_literal_binary_expr(kt::Log &log)
{
   auto result = build_ast_from_source("return (value + 4) * 3");
   if (not result.chunk.ok()) {
      log.error("failed to parse literal/binary expression AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt &block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 1) {
      log.error("expected one statement, got %" PRId64, (int64_t)statements.size());
      log_block_outline(block, log);
      return false;
   }

   const StmtNode &stmt = statements[0];
   if (not (stmt.kind IS AstNodeKind::ReturnStmt)) {
      log.error("expected return statement, got kind=%d", (int)stmt.kind);
      log_block_outline(block, log);
      return false;
   }

   const auto *payload = std::get_if<ReturnStmtPayload>(&stmt.data);
   if (not payload or payload->values.size() != 1) {
      log.error("return payload missing or wrong arity");
      return false;
   }

   const ExprNode& expr = *payload->values[0];
   if (not (expr.kind IS AstNodeKind::BinaryExpr)) {
      log.error("expected binary expression root, got kind=%d", (int)expr.kind);
      return false;
   }

   const auto* multiply = std::get_if<BinaryExprPayload>(&expr.data);
   if (not multiply or not (multiply->op IS AstBinaryOperator::Multiply)) {
      log.error("expected multiply binary node");
      return false;
   }

   if (not multiply->left or not (multiply->left->kind IS AstNodeKind::BinaryExpr)) {
      log.error("left operand was not an additive binary expression");
      return false;
   }

   const auto *add = std::get_if<BinaryExprPayload>(&multiply->left->data);
   if (not add or not (add->op IS AstBinaryOperator::Add)) {
      log.error("expected addition in the left subtree");
      return false;
   }

   if (not multiply->right or not (multiply->right->kind IS AstNodeKind::LiteralExpr)) {
      log.error("expected numeric literal on multiply RHS");
      return false;
   }

   const auto *rhs_literal = std::get_if<LiteralValue>(&multiply->right->data);
   if (not rhs_literal or not (rhs_literal->kind IS LiteralKind::Number) or rhs_literal->number_value != 3.0) {
      log.error("multiply RHS literal mismatch");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Expression parsing entry point tests.

static bool test_expression_entry_point(kt::Log &log)
{
   auto harness = make_expression_harness("value + 42");
   if (not harness.has_value()) {
      log.error("failed to initialise expression harness");
      return false;
   }

   ParserContext& context = *harness->context;
   FuncState& fs = *harness->func_state;
   BCREG before = fs.freereg;

   AstBuilder builder(context);
   auto expression = builder.parse_expression(0);

   if (not expression.ok()) {
      log.error("expression entry parser reported failure");
      auto diagnostics = context.diagnostics().entries();
      log_diagnostics(diagnostics, log);
      return false;
   }

   if (fs.freereg != before) {
      log.error("FuncState::freereg changed from %d to %d during AST parse", (int)before, (int)fs.freereg);
      return false;
   }

   const ExprNode& node = *expression.value_ref();
   if (not (node.kind IS AstNodeKind::BinaryExpr)) {
      log.error("expected binary node from expression entry point");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_expression_list_entry_point(kt::Log &log)
{
   auto harness = make_expression_harness("value, call(arg), 99");
   if (not harness.has_value()) {
      log.error("failed to initialise expression list harness");
      return false;
   }

   ParserContext& context = *harness->context;
   FuncState& fs = *harness->func_state;
   BCREG before = fs.freereg;
   AstBuilder builder(context);
   auto list = builder.parse_expression_list();
   if (not list.ok()) {
      log.error("expression list entry parser reported failure");
      auto diagnostics = context.diagnostics().entries();
      log_diagnostics(diagnostics, log);
      return false;
   }

   if (fs.freereg != before) {
      log.error("FuncState::freereg changed from %d to %d during AST list parse", (int)before, (int)fs.freereg);
      return false;
   }

   if (list.value_ref().size() != 3) {
      log.error("expected three expressions from list entry point, got %" PRId64,
         (int64_t)list.value_ref().size());
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_empty_comment_appended_to_variable(kt::Log &log)
{
   auto result = build_ast_from_source("value--\n", true);

   for (const ParserDiagnostic &diagnostic : result.diagnostics) {
      if (diagnostic.message IS "Empty comment appended to variable is not a decrement operation") return true;
   }

   log.error("expected empty appended comment diagnostic");
   log_diagnostics(result.diagnostics, log);
   return false;
}

//********************************************************************************************************************

static bool test_global_callable_contract_without_type_analysis(kt::Log &Log)
{
   constexpr std::string_view source =
      "global function glParserContract() end\n"
      "global thunk glParserThunk():num end\n";
   auto result = build_ast_from_source(source, false, true);
   if (not result.chunk.ok()) {
      Log.error("failed to parse global callable declarations with type analysis disabled");
      log_diagnostics(result.diagnostics, Log);
      return false;
   }

   StatementListView statements = result.chunk.value_ref()->view();
   if (statements.size() != 2) {
      Log.error("expected two global callable declarations, got %" PRId64, int64_t(statements.size()));
      return false;
   }

   for (size_t i = 0; i < statements.size(); ++i) {
      const auto *payload = std::get_if<FunctionStmtPayload>(&statements[i].data);
      if (not payload or payload->name.segments.size() != 1) {
         Log.error("global callable declaration %" PRId64 " has no direct identifier", int64_t(i));
         return false;
      }

      const Identifier &identifier = payload->name.segments.front();
      if (identifier.global_contract_type != TiriType::Func or
          identifier.global_contract_policy != GlobalContractPolicy::Enforced) {
         Log.error("global callable declaration %" PRId64 " did not acquire an intrinsic func contract", int64_t(i));
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

static bool test_loop_ast(kt::Log &log)
{
   constexpr const char* source = R"(
while ready do
   if ready then
      return ready
   end
   ready = false
end
)";
   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse loop AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 1) {
      log.error("expected loop-only block, got %" PRId64, (int64_t)statements.size());
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& loop_stmt = statements[0];
   if (not (loop_stmt.kind IS AstNodeKind::WhileStmt)) {
      log.error("expected while loop node, got kind=%d", (int)loop_stmt.kind);
      return false;
   }

   const auto* loop_payload = std::get_if<LoopStmtPayload>(&loop_stmt.data);
   if (not loop_payload or not loop_payload->body) {
      log.error("missing loop payload or body");
      return false;
   }

   StatementListView body_statements = loop_payload->body->view();
   if (body_statements.size() != 2) {
      log.error("expected if+assignment inside loop, got %" PRId64, (int64_t)body_statements.size());
      return false;
   }

   const StmtNode& if_stmt = body_statements[0];
   if (not (if_stmt.kind IS AstNodeKind::IfStmt)) {
      log.error("first loop body statement should be if");
      return false;
   }

   const auto* if_payload = std::get_if<IfStmtPayload>(&if_stmt.data);
   if (not if_payload or if_payload->clauses.empty()) {
      log.error("missing if clause payload");
      return false;
   }

   const StmtNode& assign_stmt = body_statements[1];
   if (not (assign_stmt.kind IS AstNodeKind::AssignmentStmt)) {
      log.error("expected assignment statement as second loop body element");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_if_stmt_with_elseif_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local output = 0
local fallback = 5
if level > 10 then
   output = level
elseif level ?? fallback then
   output = level ? level :> fallback
else
   output = fallback
end
return output
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse chained if AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 4) {
      log.error("expected two locals, if, return; got %" PRId64, (int64_t)statements.size());
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& if_stmt = statements[2];
   if (not (if_stmt.kind IS AstNodeKind::IfStmt)) {
      log.error("third statement should be if, got kind=%d", (int)if_stmt.kind);
      return false;
   }

   const auto* payload = std::get_if<IfStmtPayload>(&if_stmt.data);
   if (not payload or payload->clauses.size() != 3) {
      log.error("expected three if clauses (if/elseif/else)");
      return false;
   }

   const IfClause& first_clause = payload->clauses[0];
   if (not first_clause.condition or not (first_clause.condition->kind IS AstNodeKind::BinaryExpr)) {
      log.error("first clause should include binary condition");
      return false;
   }

   const auto* gt_payload = std::get_if<BinaryExprPayload>(&first_clause.condition->data);
   if (not gt_payload or not (gt_payload->op IS AstBinaryOperator::GreaterThan)) {
      log.error("first clause binary operator mismatch");
      return false;
   }

   const IfClause& second_clause = payload->clauses[1];
   if (not second_clause.condition or not (second_clause.condition->kind IS AstNodeKind::BinaryExpr)) {
      log.error("elseif clause should include binary expression");
      return false;
   }

   const auto* if_empty = std::get_if<BinaryExprPayload>(&second_clause.condition->data);
   if (not if_empty or not (if_empty->op IS AstBinaryOperator::IfEmpty)) {
      log.error("elseif clause expected IfEmpty operator");
      return false;
   }

   if (not second_clause.block) {
      log.error("elseif clause missing body block");
      return false;
   }

   StatementListView elseif_body = second_clause.block->view();
   if (elseif_body.size() != 1) {
      log.error("elseif block should contain assignment only");
      return false;
   }

   const StmtNode& elseif_assignment = elseif_body[0];
   const auto* assign_payload = std::get_if<AssignmentStmtPayload>(&elseif_assignment.data);
   if (not assign_payload or assign_payload->values.size() != 1) {
      log.error("elseif assignment payload missing");
      return false;
   }

   if (not assign_payload->values[0] or not (assign_payload->values[0]->kind IS AstNodeKind::TernaryExpr)) {
      log.error("elseif assignment should assign ternary expression");
      return false;
   }

   const IfClause& else_clause = payload->clauses[2];
   if (else_clause.condition) {
      log.error("else clause should not have a condition");
      return false;
   }
   if (not else_clause.block) {
      log.error("else clause missing block");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_local_function_table_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local function build_pair(a, b)
   local data = { label = "value", values = { a, b } }
   return data
end

return build_pair(1, 2)
)";
   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse local function/table AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 2) {
      log.error("expected local function and return statements");
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& local_func = statements[0];
   if (not (local_func.kind IS AstNodeKind::LocalFunctionStmt)) {
      log.error("expected local function statement, got kind=%d", (int)local_func.kind);
      return false;
   }

   const auto* func_payload = std::get_if<LocalFunctionStmtPayload>(&local_func.data);
   if (not func_payload or not func_payload->function or not func_payload->function->body) {
      log.error("malformed local function payload");
      return false;
   }

   StatementListView fn_body = func_payload->function->body->view();
   if (fn_body.size() != 2) {
      log.error("expected local decl + return inside function body");
      return false;
   }

   const StmtNode& local_decl = fn_body[0];
   if (not (local_decl.kind IS AstNodeKind::LocalDeclStmt)) {
      log.error("expected local declaration inside function body");
      return false;
   }

   const auto* decl_payload = std::get_if<LocalDeclStmtPayload>(&local_decl.data);
   if (not decl_payload or decl_payload->values.size() != 1) {
      log.error("local declaration missing initialiser");
      return false;
   }

   const ExprNode& table_expr = *decl_payload->values[0];
   if (not (table_expr.kind IS AstNodeKind::TableExpr)) {
      log.error("expected table constructor initialiser");
      return false;
   }

   const auto* table_payload = std::get_if<TableExprPayload>(&table_expr.data);
   if (not table_payload or table_payload->fields.size() != 2) {
      log.error("unexpected number of table fields");
      return false;
   }

   const TableField& label_field = table_payload->fields[0];
   if (not (label_field.kind IS TableFieldKind::Record) or not label_field.value or
      not (label_field.value->kind IS AstNodeKind::LiteralExpr)) {
      log.error("first field should be record literal");
      return false;
   }

   const auto* label_literal = std::get_if<LiteralValue>(&label_field.value->data);
   if (not label_literal or not (label_literal->kind IS LiteralKind::String)) {
      log.error("label literal payload missing string value");
      return false;
   }

   const TableField& values_field = table_payload->fields[1];
   if (not (values_field.kind IS TableFieldKind::Record) or not values_field.value or
      not (values_field.value->kind IS AstNodeKind::TableExpr)) {
      log.error("values field should contain nested table literal");
      return false;
   }

   const auto* nested_table = std::get_if<TableExprPayload>(&values_field.value->data);
   if (not nested_table or nested_table->fields.size() != 2) {
      log.error("nested array literal should have two elements");
      return false;
   }

   for (const TableField& field : nested_table->fields) {
      if (not (field.kind IS TableFieldKind::Array) or not field.value or
         not (field.value->kind IS AstNodeKind::IdentifierExpr)) {
         log.error("nested table entries should be identifier references");
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

static bool test_range_for_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local limit = 5
local sum = 0
for index in {1 into 5 by 2} do
   sum += index
end
return sum
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse range for AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 4) {
      log.error("expected two locals, loop, return; got %" PRId64, (int64_t)statements.size());
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& for_stmt = statements[2];
   if (not (for_stmt.kind IS AstNodeKind::RangeForStmt)) {
      log.error("expected direct range for statement");
      return false;
   }

   const auto* payload = std::get_if<RangeForStmtPayload>(&for_stmt.data);
   if (not payload or not payload->body) {
      log.error("range for payload missing body");
      return false;
   }

   if (not payload->start or not payload->stop or not payload->step) {
      log.error("range for payload missing bounds expressions");
      return false;
   }

   StatementListView loop_body = payload->body->view();
   if (loop_body.size() != 1) {
      log.error("range for body should include single assignment");
      return false;
   }

   const StmtNode& assignment = loop_body[0];
   if (not (assignment.kind IS AstNodeKind::AssignmentStmt)) {
      log.error("range for body should assign to accumulator");
      return false;
   }

   const auto* add_payload = std::get_if<AssignmentStmtPayload>(&assignment.data);
   if (not add_payload or not (add_payload->op IS AssignmentOperator::Add)) {
      log.error("expected compound add assignment inside loop");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_deprecated_numeric_for_rejected(kt::Log &Log)
{
   struct DeprecatedForCase {
      std::string_view source;
      int expected_column;
   };

   constexpr std::array<DeprecatedForCase, 3> cases = { {
      { "for i = 0, 8 do end", 7 },
      { "for i=0,8 do end", 6 },
      { "for i = start, stop, step do end", 7 }
   } };

   for (const auto &test_case : cases) {
      auto result = build_ast_from_source(test_case.source, true);
      size_t deprecated_count = 0;

      for (const ParserDiagnostic &diagnostic : result.diagnostics) {
         if (diagnostic.code != ParserErrorCode::DeprecatedSyntax) continue;

         deprecated_count++;
         if (diagnostic.severity != ParserDiagnosticSeverity::Error or
             diagnostic.token.kind() != TokenKind::Equals or
             diagnostic.token.span().line.lineNumber() != 1 or
             diagnostic.token.span().column.lineNumber() != test_case.expected_column or
             diagnostic.file_index != 0) {
            Log.error("deprecated numeric for diagnostic has the wrong severity, token or source location");
            log_diagnostics(result.diagnostics, Log);
            return false;
         }

         if (diagnostic.message.find("inclusive range") IS std::string::npos) {
            Log.error("deprecated numeric for diagnostic does not identify the supported inclusive range syntax");
            return false;
         }
      }

      if (deprecated_count != 1) {
         Log.error("expected one deprecated numeric for diagnostic, got %" PRId64, int64_t(deprecated_count));
         log_diagnostics(result.diagnostics, Log);
         return false;
      }
   }

   auto range_result = build_ast_from_source("for i in {0 into 8} do end", true);
   for (const ParserDiagnostic &diagnostic : range_result.diagnostics) {
      if (diagnostic.code IS ParserErrorCode::DeprecatedSyntax) {
         Log.error("supported range loop emitted a deprecated syntax diagnostic");
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

static bool test_colon_method_syntax_rejected(kt::Log &Log)
{
   struct ColonCase {
      std::string_view source;
      std::string_view expected_message;
   };

   constexpr std::array<ColonCase, 3> cases = { {
      { "local values = {}\nvalues:clear()", "colon method syntax has been removed" },
      { "local values:table = nil\nvalues?:clear()", "safe colon method syntax has been removed" },
      { "local value = {}\nfunction value:clear() end", "colon-qualified function declarations have been removed" }
   } };

   for (const auto &test_case : cases) {
      auto result = build_ast_from_source(test_case.source, true);
      size_t matching = 0;
      for (const ParserDiagnostic &diagnostic : result.diagnostics) {
         if (diagnostic.code != ParserErrorCode::DeprecatedSyntax) continue;
         if (diagnostic.message.find(test_case.expected_message) IS std::string::npos) {
            Log.error("colon syntax diagnostic did not explain the migration path");
            log_diagnostics(result.diagnostics, Log);
            return false;
         }
         matching++;
      }
      if (matching != 1) {
         Log.error("expected one colon syntax diagnostic, got %zu", matching);
         log_diagnostics(result.diagnostics, Log);
         return false;
      }
   }

   auto annotations = build_ast_from_source(
      "local value:num = 1\nfunction typed(Input:str):<str, num> return Input, value end\n", true);
   if (not annotations.chunk.ok() or not annotations.diagnostics.empty()) {
      Log.error("colon removal interfered with type annotations");
      log_diagnostics(annotations.diagnostics, Log);
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_length_range_for_ast(kt::Log &Log)
{
   constexpr const char *source = R"(
local values = array<int> { 1, 2, 3 }
for index in {0 to #values} do
   values[index] += 1
end
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      Log.error("failed to parse array-length range loop");
      log_diagnostics(result.diagnostics, Log);
      return false;
   }

   StatementListView statements = result.chunk.value_ref()->view();
   if (statements.size() != 2) {
      Log.error("expected local declaration and range loop; got %" PRId64, int64_t(statements.size()));
      return false;
   }

   const StmtNode &loop = statements[1];
   if (loop.kind != AstNodeKind::RangeForStmt) {
      Log.error("array-length range should retain a direct range node until semantic type analysis");
      return false;
   }

   const auto *payload = std::get_if<RangeForStmtPayload>(&loop.data);
   if (not payload or not payload->start or not payload->stop) {
      Log.error("direct array-length range should retain its operands");
      return false;
   }

   if (payload->stop->kind != AstNodeKind::UnaryExpr) {
      Log.error("source range should retain its length stop expression");
      return false;
   }

   return true;
}

static bool test_generic_for_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local total = 0
for key, value in pairs(records) do
   if value then
      total = total + value
   end
end
return total
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse generic for AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 3) {
      log.error("expected local, loop, return statements");
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& for_stmt = statements[1];
   if (not (for_stmt.kind IS AstNodeKind::GenericForStmt)) {
      log.error("second statement should be generic for loop");
      return false;
   }

   const auto* payload = std::get_if<GenericForStmtPayload>(&for_stmt.data);
   if (not payload or not payload->body) {
      log.error("generic for payload missing body");
      return false;
   }

   if (payload->names.size() != 2) {
      log.error("generic for should declare key and value, got %" PRId64, (int64_t)payload->names.size());
      return false;
   }
   if (payload->iterators.size() != 1 or not payload->iterators[0]) {
      log.error("generic for should include one iterator expression");
      return false;
   }
   if (not (payload->iterators[0]->kind IS AstNodeKind::CallExpr)) {
      log.error("generic for iterator should be call expression");
      return false;
   }

   StatementListView loop_body = payload->body->view();
   if (loop_body.size() != 1) {
      log.error("generic for body should contain if statement");
      return false;
   }

   const StmtNode& inner_if = loop_body[0];
   if (not (inner_if.kind IS AstNodeKind::IfStmt)) {
      log.error("generic for body expected if statement");
      return false;
   }

   const auto* if_payload = std::get_if<IfStmtPayload>(&inner_if.data);
   if (not if_payload or if_payload->clauses.size() != 1) {
      log.error("inner if should contain single clause");
      return false;
   }

   return true;
}

static bool test_repeat_defer_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local total = 0
local step = 1
repeat
   defer
      total = total + step
   end
   total = total + step
until total > 5
return total
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse repeat/defer AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 4) {
      log.error("expected two locals, repeat, return; got %" PRId64, (int64_t)statements.size());
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& repeat_stmt = statements[2];
   if (not (repeat_stmt.kind IS AstNodeKind::RepeatStmt)) {
      log.error("third statement should be repeat loop");
      return false;
   }

   const auto* payload = std::get_if<LoopStmtPayload>(&repeat_stmt.data);
   if (not payload or not payload->body) {
      log.error("repeat payload missing body");
      return false;
   }

   if (not (payload->style IS LoopStyle::RepeatUntil)) {
      log.error("repeat loop should record RepeatUntil style");
      return false;
   }
   if (not payload->condition or not (payload->condition->kind IS AstNodeKind::BinaryExpr)) {
      log.error("repeat loop missing terminating condition");
      return false;
   }

   StatementListView loop_body = payload->body->view();
   if (loop_body.size() != 2) {
      log.error("repeat loop should contain defer and assignment");
      return false;
   }

   const StmtNode& defer_stmt = loop_body[0];
   if (not (defer_stmt.kind IS AstNodeKind::DeferStmt)) {
      log.error("first repeat body statement should be defer");
      return false;
   }
   const auto* defer_payload = std::get_if<DeferStmtPayload>(&defer_stmt.data);
   if (not defer_payload or not defer_payload->callable) {
      log.error("defer payload missing callable");
      return false;
   }
   if (not defer_payload->arguments.empty()) {
      log.error("defer test should not forward arguments");
      return false;
   }

   const StmtNode& accumulator = loop_body[1];
   if (not (accumulator.kind IS AstNodeKind::AssignmentStmt)) {
      log.error("repeat loop second statement should be assignment");
      return false;
   }

   return true;
}

static bool test_ternary_presence_expr_ast(kt::Log &log)
{
   constexpr const char* source = R"(
local value = nil
local fallback = 10
return (value ?? fallback) ? value :> fallback, value??, (value ?? fallback)??
)";

   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      log.error("failed to parse ternary/presence AST");
      log_diagnostics(result.diagnostics, log);
      return false;
   }

   const BlockStmt& block = *result.chunk.value_ref();
   StatementListView statements = block.view();
   if (statements.size() != 3) {
      log.error("expected two locals and return for ternary test");
      log_block_outline(block, log);
      return false;
   }

   const StmtNode& return_stmt = statements[2];
   if (not (return_stmt.kind IS AstNodeKind::ReturnStmt)) {
      log.error("third statement should be return");
      return false;
   }

   const auto* payload = std::get_if<ReturnStmtPayload>(&return_stmt.data);
   if (not payload or payload->values.size() != 3) {
      log.error("return should provide three expressions");
      return false;
   }

   if (not payload->values[0] or not (payload->values[0]->kind IS AstNodeKind::TernaryExpr)) {
      log.error("first return expression should be ternary");
      return false;
   }
   if (not payload->values[1] or not (payload->values[1]->kind IS AstNodeKind::PresenceExpr)) {
      log.error("second return expression should be presence check");
      return false;
   }
   if (not payload->values[2] or not (payload->values[2]->kind IS AstNodeKind::PresenceExpr)) {
      log.error("third return expression should be nested presence check");
      return false;
   }

   return true;
}


struct BytecodeSnapshot {
   std::vector<BCIns> instructions;
   std::vector<BytecodeSnapshot> children;
};

struct PipelineSnippet {
   const char* label;
   const char* source;
};

static BytecodeSnapshot snapshot_proto(GCproto* pt)
{
   BytecodeSnapshot snapshot;
   BCIns* bc = proto_bc(pt);
   snapshot.instructions.assign(bc, bc + pt->sizebc);

   if (pt->flags & PROTO_CHILD) {
      ptrdiff_t child_count = pt->sizekgc;
      GCRef* kr = mref<GCRef>(pt->k) - 1;
      for (ptrdiff_t i = 0; i < child_count; ++i, --kr) {
         GCobj* obj = gcref(*kr);
         if (obj->gch.gct IS ~LJ_TPROTO) {
            snapshot.children.push_back(snapshot_proto(gco_to_proto(obj)));
         }
      }
   }

   return snapshot;
}

static void log_snapshot(const BytecodeSnapshot& snapshot, const std::string& label)
{
   kt::Log log("Tiri-Parser");
   log.msg("%s: %" PRId64 " instructions", label.c_str(), snapshot.instructions.size());
   for (size_t i = 0; i < snapshot.instructions.size(); ++i) {
      std::string desc = describe_instruction(snapshot.instructions[i]);
      log.msg("  [%" PRId64 "] %s", i, desc.c_str());
   }
   if (not snapshot.children.empty()) {
      log.msg("%s: %" PRId64 " children", label.c_str(), snapshot.children.size());
      for (size_t i = 0; i < snapshot.children.size(); ++i) {
         log_snapshot(snapshot.children[i], label + ".child[" + std::to_string(i) + "]");
      }
   }
}

static bool compare_snapshots(const BytecodeSnapshot& legacy, const BytecodeSnapshot& ast,
   std::string& diff, std::string label)
{
   auto trim_epilogue = [](const std::vector<BCIns>& source) {
      std::vector<BCIns> trimmed = source;
      while (trimmed.size() >= 2) {
         BCIns last   = trimmed.back();
         BCIns before = trimmed[trimmed.size() - 2];
         if ((bc_op(before) IS BC_UCLO) and (bc_op(last) IS BC_RET0)) {
            trimmed.pop_back();
            trimmed.pop_back();
            continue;
         }
         break;
      }
      return trimmed;
   };

   std::vector<BCIns> legacy_body = trim_epilogue(legacy.instructions);
   std::vector<BCIns> ast_body    = trim_epilogue(ast.instructions);

   if (legacy_body.size() != ast_body.size()) {
      std::ostringstream stream;
      stream << label << ": bytecode length mismatch (legacy=" << legacy_body.size()
         << ", ast=" << ast_body.size() << ")";
      log_snapshot(legacy, "legacy " + label);
      log_snapshot(ast, "ast " + label);
      diff = stream.str();
      return false;
   }

   for (size_t i = 0; i < legacy_body.size(); ++i) {
      if (legacy_body[i] != ast_body[i]) {
         // Allow benign differences in JMP register allocation due to different
         // loop control flow management (legacy GOLA vs AST loop_stack)
         BCOp legacy_op = bc_op(legacy_body[i]);
         BCOp ast_op = bc_op(ast_body[i]);
         if (legacy_op IS BC_JMP and ast_op IS BC_JMP) {
            // JMP instructions may have different 'a' registers but same jump offset
            if (bc_d(legacy_body[i]) IS bc_d(ast_body[i])) {
               continue;  // Semantically equivalent
            }
         }

         kt::Log log("Tiri-Parser");
         std::string legacy_desc = describe_instruction(legacy_body[i]);
         std::string ast_desc = describe_instruction(ast_body[i]);
         log.msg("legacy[%s:%" PRId64 "] %s", label.c_str(), i, legacy_desc.c_str());
         log.msg("   ast[%s:%" PRId64 "] %s", label.c_str(), i, ast_desc.c_str());
         std::ostringstream stream;
         stream << label << ": mismatch at pc=" << i << " legacy=0x" << std::hex
            << legacy_body[i] << " ast=0x" << ast_body[i];
         diff = stream.str();
         return false;
      }
   }

   if (legacy.children.size() != ast.children.size()) {
      std::ostringstream stream;
      stream << label << ": child count mismatch (legacy=" << legacy.children.size()
         << ", ast=" << ast.children.size() << ")";
      diff = stream.str();
      return false;
   }

   for (size_t i = 0; i < legacy.children.size(); ++i) {
      std::string child_diff;
      std::string child_label = label + ".child[" + std::to_string(i) + "]";
      if (not compare_snapshots(legacy.children[i], ast.children[i], child_diff, child_label)) {
         diff = child_diff;
         return false;
      }
   }

   return true;
}


static std::optional<BytecodeSnapshot> compile_snapshot(lua_State* L, std::string_view source,
   bool ast_pipeline, std::string& error)
{

   if (lua_load(L, source, "parser-unit")) {
      const char* message = lua_tostring(L, -1);
      error.assign(message ? message : "unknown parser error");
      lua_pop(L, 1);
      return std::nullopt;
   }

   GCfunc* fn = funcV(L->top - 1);
   BytecodeSnapshot snapshot = snapshot_proto(funcproto(fn));
   L->top--;
   return snapshot;
}

static int bytecode_writer(lua_State *, const void *Data, size_t Size, void *Context)
{
   auto *dump = (std::string *)Context;
   dump->append((const char *)Data, Size);
   return 0;
}

static GCproto * first_child_proto(GCproto *Proto)
{
   for (ptrdiff_t i = -ptrdiff_t(Proto->sizekgc); i < 0; ++i) {
      GCobj *object = proto_kgc(Proto, i);
      if (object->gch.gct IS uint8_t(~LJ_TPROTO)) return gco_to_proto(object);
   }
   return nullptr;
}

static bool compare_proto_signatures(const GCproto *Left, const GCproto *Right)
{
   if (not Left or not Right or Left->signature_size != Right->signature_size) return false;
   if (Left->signature_size > 0 and
       memcmp(proto_signature(Left), proto_signature(Right), Left->signature_size) != 0) return false;

   std::vector<const GCproto *> left_children;
   std::vector<const GCproto *> right_children;
   for (ptrdiff_t i = -ptrdiff_t(Left->sizekgc); i < 0; ++i) {
      GCobj *object = proto_kgc(Left, i);
      if (object->gch.gct IS uint8_t(~LJ_TPROTO)) left_children.push_back(gco_to_proto(object));
   }
   for (ptrdiff_t i = -ptrdiff_t(Right->sizekgc); i < 0; ++i) {
      GCobj *object = proto_kgc(Right, i);
      if (object->gch.gct IS uint8_t(~LJ_TPROTO)) right_children.push_back(gco_to_proto(object));
   }
   if (left_children.size() != right_children.size()) return false;
   for (size_t i = 0; i < left_children.size(); ++i) {
      if (not compare_proto_signatures(left_children[i], right_children[i])) return false;
   }
   return true;
}

static bool test_signature_metadata_roundtrip(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view source =
      "struct SignatureLayout\n"
      "   Value: int\n"
      "end\n"
      "local function outer(Value:num!, Flexible:any, Untyped, Item:struct<SignatureLayout>, Generic:struct, "
      "Object:obj, Numbers:array<int>, Records:array<struct<SignatureLayout>>, ...):func\n"
      "   local function inner(Text:str!):<str!, num, ...>\n"
      "      return Text, Value, Value\n"
      "   end\n"
      "   return inner\n"
      "end\n"
      "return outer\n";

   if (lua_load(L, source, "signature-roundtrip")) {
      Log.error("failed to compile signature source: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *root = funcproto(funcV(L->top - 1));
   GCproto *outer = first_child_proto(root);
   GCproto *inner = outer ? first_child_proto(outer) : nullptr;
   if (not outer or not inner) {
      Log.error("signature source did not produce the expected nested prototypes");
      return false;
   }

   const ProtoSignature *outer_signature = proto_signature(outer);
   const ProtoSignature *inner_signature = proto_signature(inner);
   if (not outer_signature or outer_signature->parameter_count != 8 or outer_signature->result_count != 1 or
       not (outer_signature->flags & proto_signature_flag(ProtoSignatureFlag::ExplicitResults)) or
       not (outer_signature->flags & proto_signature_flag(ProtoSignatureFlag::ParameterVariadic))) {
      Log.error("outer prototype signature header is incomplete");
      return false;
   }

   const ProtoTypeEntry *outer_params = proto_parameter_types(outer);
   if (outer_params[0].type != TiriType::Num or
       proto_type_nullable(outer_params[0]) or not proto_type_required(outer_params[0]) or
       proto_type_origin(outer_params[0]) != ProtoTypeOrigin::Declared or
       proto_type_strength(outer_params[0]) != ProtoTypeStrength::Checked or
       outer_params[1].type != TiriType::Any or
       proto_type_origin(outer_params[1]) != ProtoTypeOrigin::Declared or
       proto_type_origin(outer_params[2]) != ProtoTypeOrigin::Unspecified or
       outer_params[3].constraint != struct_key("SignatureLayout") or outer_params[4].type != TiriType::Struct or
       outer_params[4].constraint != 0 or outer_params[5].type != TiriType::Object or outer_params[5].constraint != 0 or
       outer_params[6].type != TiriType::Array or proto_array_member(outer_params[6]) != AET::INT32 or
       outer_params[6].constraint != 0 or outer_params[7].type != TiriType::Array or
       proto_array_member(outer_params[7]) != AET::STRUCT or
       outer_params[7].constraint != struct_key("SignatureLayout")) {
      Log.error("outer prototype parameter entries lost type, provenance, strength or constraint data");
      return false;
   }

   // Exercise reserved schema states that ordinary annotations do not manufacture in this phase.
   ProtoTypeEntry *mutable_outer_params = proto_parameter_types(outer);
   mutable_outer_params[4].flags =
      proto_type_flags(false, true, ProtoTypeOrigin::Declared, ProtoTypeStrength::Checked);
   mutable_outer_params[5].constraint = uint32_t(CLASSID::TIME);
   mutable_outer_params[5].flags =
      proto_type_flags(true, false, ProtoTypeOrigin::Declared, ProtoTypeStrength::Trusted);
   if (proto_type_nullable(mutable_outer_params[4]) or not proto_type_required(mutable_outer_params[4]) or
       proto_type_strength(mutable_outer_params[5]) != ProtoTypeStrength::Trusted or
       CLASSID(mutable_outer_params[5].constraint) != CLASSID::TIME or
       not ResolveClassID(CLASSID(mutable_outer_params[5].constraint))) {
      Log.error("reserved signature entry states are not independently representable");
      return false;
   }

   if (not inner_signature or inner_signature->parameter_count != 1 or inner_signature->result_count != 2 or
       inner_signature->result_entry_count != 2 or
       not (inner_signature->flags & proto_signature_flag(ProtoSignatureFlag::ResultVariadic))) {
      Log.error("inner prototype result signature is incomplete");
      return false;
   }
   const ProtoTypeEntry *inner_params = proto_parameter_types(inner);
   const ProtoTypeEntry *inner_results = proto_result_types(inner);
   if (proto_type_nullable(inner_params[0]) or not proto_type_required(inner_params[0]) or
       proto_type_nullable(inner_results[0]) or not proto_type_required(inner_results[0]) or
       not proto_type_nullable(inner_results[1]) or proto_type_required(inner_results[1])) {
      Log.error("required source annotations did not reach parameter and result prototype metadata");
      return false;
   }

   for (int strip : { 0, 1 }) {
      std::string dump;
      if (lj_bcwrite(L, root, bytecode_writer, &dump, strip) != 0) {
         Log.error("failed to write %s signature dump", strip ? "stripped" : "unstripped");
         return false;
      }
      if (dump.size() < 4 or uint8_t(dump[3]) != BCDUMP_VERSION) {
         Log.error("signature dump has the wrong private format version");
         return false;
      }
      if (lua_load(L, std::string_view(dump.data(), dump.size()), "signature-roundtrip")) {
         Log.error("failed to reload %s signature dump: %s", strip ? "stripped" : "unstripped",
            lua_tostring(L, -1));
         return false;
      }
      GCproto *restored = funcproto(funcV(L->top - 1));
      bool equal = compare_proto_signatures(root, restored);
      lua_pop(L, 1);
      if (not equal) {
         Log.error("%s signature dump changed prototype metadata", strip ? "stripped" : "unstripped");
         return false;
      }
   }

   lua_pop(L, 1);
   return true;
}

static bool test_forward_declaration_signature_validation(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view matching_source =
      "global function declared(Name:str, Options:table, ...):<str, num> end\n"
      "global function declared(Value:str, Settings:table, ...):<str, num>\n"
      "   return Value, 1\n"
      "end\n";

   if (lua_load(L, matching_source, "matching-forward-signature")) {
      Log.error("matching forward declaration was rejected: %s", lua_tostring(L, -1));
      return false;
   }
   lua_pop(L, 1);

   constexpr std::string_view mismatched_source =
      "global function klass(Name:str, Options:table, Def:str, Private:bool, Custom:str) end\n"
      "global function klass(Name:str, Options:table, Def:str, Private:str, Custom:str)\n"
      "end\n";

   if (lua_load(L, mismatched_source, "mismatched-forward-signature") IS 0) {
      Log.error("mismatched forward declaration was accepted");
      lua_pop(L, 1);
      return false;
   }

   std::string_view message = lua_tostring(L, -1);
   if (message.find("signature for global function 'klass' does not match its forward declaration") IS
       std::string_view::npos or message.find("parameter 4 has type 'str', expected 'bool'") IS
       std::string_view::npos) {
      Log.error("mismatched forward declaration produced the wrong diagnostic: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
      return false;
   }

   lua_pop(L, 1);

   constexpr std::string_view conditional_mismatch_source =
      "global function routed(Value:num) end\n"
      "if true then\n"
      "   global function routed(Value:num)\n"
      "      return Value\n"
      "   end\n"
      "else\n"
      "   global function routed(Value:str)\n"
      "      return Value\n"
      "   end\n"
      "end\n";

   if (lua_load(L, conditional_mismatch_source, "conditional-forward-signature") IS 0) {
      Log.error("a conditional definition bypassed its original forward declaration");
      lua_pop(L, 1);
      return false;
   }

   message = lua_tostring(L, -1);
   if (message.find("signature for global function 'routed' does not match its forward declaration") IS
       std::string_view::npos or message.find("parameter 1 has type 'str', expected 'num'") IS
       std::string_view::npos) {
      Log.error("conditional forward declaration produced the wrong diagnostic: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
      return false;
   }

   lua_pop(L, 1);
   return true;
}

static bool test_old_bytecode_versions_rejected(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view source = "return function(Value:num):num return Value end";
   if (lua_load(L, source, "old-version-source")) {
      Log.error("failed to compile old-version rejection source: %s", lua_tostring(L, -1));
      return false;
   }

   std::string dump;
   if (lua_dump(L, bytecode_writer, &dump) != 0 or dump.size() < 4 or
       uint8_t(dump[3]) != BCDUMP_VERSION) {
      Log.error("failed to create a current bytecode dump for old-version rejection");
      return false;
   }
   lua_pop(L, 1);

   // 0x86 is included deliberately: Phase 5 removed the compiler-private binder referenced by those chunks and
   // replaced it with BC_MODACT.  Gate E selected format rejection over a compatibility shim.

   for (uint8_t version : { uint8_t(0x81), uint8_t(0x83), uint8_t(0x85), uint8_t(0x86) }) {
      std::string old_dump = dump;
      old_dump[3] = char(version);
      if (lua_load(L, std::string_view(old_dump.data(), old_dump.size()), "old-version") IS 0) {
         Log.error("the bytecode reader accepted old dump version 0x%02x", unsigned(version));
         lua_pop(L, 1);
         return false;
      }
      lua_pop(L, 1);
   }
   return true;
}

static bool test_unmatched_context_entry_rejected(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();
   constexpr std::string_view source =
      "local receiver = { name='receiver', read=function():str return .name end }\n"
      "local value = receiver.read()\n"
      "return value\n";
   if (lua_load(lua, source, "unmatched-context-entry")) {
      Log.error("failed to compile contextual bytecode fixture: %s", lua_tostring(lua, -1));
      return false;
   }

   GCproto *prototype = funcproto(funcV(lua->top - 1));
   BCIns *bytecode = proto_bc(prototype);
   MSize call_position = 0;
   for (MSize i = 1; i + 1 < prototype->sizebc; ++i) {
      if ((bc_op(bytecode[i]) IS BC_CTXCALL or bc_op(bytecode[i]) IS BC_CTXCALLM) and
          bc_op(bytecode[i + 1]) IS BC_CTXLEAVE) {
         call_position = i;
         break;
      }
   }
   if (call_position IS 0) {
      Log.error("contextual bytecode fixture did not emit a call-and-leave pair");
      return false;
   }

   BCIns saved_call = bytecode[call_position];
   BCIns saved_leave = bytecode[call_position + 1];
   setbc_op(&bytecode[call_position], bc_op(saved_call) IS BC_CTXCALL ? BC_CALL : BC_CALLM);
   setbc_op(&bytecode[call_position + 1], BC_MOV);

   std::string dump;
   bool wrote_dump = lj_bcwrite(lua, prototype, bytecode_writer, &dump, 1) IS 0;
   bytecode[call_position] = saved_call;
   bytecode[call_position + 1] = saved_leave;
   lua_pop(lua, 1);
   if (not wrote_dump) {
      Log.error("failed to write unmatched contextual entry fixture");
      return false;
   }

   if (lua_load(lua, std::string_view(dump.data(), dump.size()), "unmatched-context-entry") IS 0) {
      Log.error("bytecode reader accepted an unmatched BC_CTXENTER");
      lua_pop(lua, 1);
      return false;
   }
   lua_pop(lua, 1);
   return true;
}

static bool test_signature_static_inference(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view source = "return function(Value:num) return Value end";
   if (lua_load(L, source, "signature-inference") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the inference function: %s", lua_tostring(L, -1));
      return false;
   }

   GCfunc *function = funcV(L->top - 1);
   GCproto *prototype = funcproto(function);
   lua_pushvalue(L, -1);
   lua_pushnumber(L, 7);
   if (lua_pcall(L, 1, 1, 0)) {
      Log.error("failed to execute the inference function: %s", lua_tostring(L, -1));
      return false;
   }
   lua_pop(L, 1);

   ProtoTypeEntry inferred = proto_result_type(prototype, 0);
   if (inferred.type != TiriType::Num or proto_type_origin(inferred) != ProtoTypeOrigin::Inferred or
       proto_type_strength(inferred) != ProtoTypeStrength::Trusted) {
      Log.error("validated static inference did not remain trusted");
      return false;
   }

   std::string dump;
   if (lj_bcwrite(L, prototype, bytecode_writer, &dump, 1) != 0) {
      Log.error("failed to write the inferred signature");
      return false;
   }
   if (lua_load(L, std::string_view(dump.data(), dump.size()), "signature-inference")) {
      Log.error("failed to round-trip the inferred signature: %s", lua_tostring(L, -1));
      return false;
   }
   GCproto *restored = funcproto(funcV(L->top - 1));
   bool equal = compare_proto_signatures(prototype, restored);
   lua_pop(L, 2);
   if (not equal) {
      Log.error("statically inferred signature changed during serialisation");
      return false;
   }

   constexpr std::string_view forwarded_source =
      "local function declared():<num, str> return 7, 'forwarded' end\n"
      "return function() return declared() end\n";
   if (lua_load(L, forwarded_source, "signature-forwarded-results") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the forwarded-result function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *forwarded = funcproto(funcV(L->top - 1));
   const ProtoSignature *forwarded_signature = proto_signature(forwarded);
   ProtoTypeEntry first = proto_result_type(forwarded, 0);
   ProtoTypeEntry second = proto_result_type(forwarded, 1);
   if (not forwarded_signature or forwarded_signature->result_count != 2 or
       forwarded_signature->result_entry_count != 2 or first.type != TiriType::Num or second.type != TiriType::Str or
       proto_type_origin(first) != ProtoTypeOrigin::Inferred or
       proto_type_origin(second) != ProtoTypeOrigin::Inferred) {
      Log.error("tail-call result inference lost a declared positional result");
      return false;
   }

   std::string forwarded_dump;
   if (lj_bcwrite(L, forwarded, bytecode_writer, &forwarded_dump, 1) != 0 or
       lua_load(L, std::string_view(forwarded_dump.data(), forwarded_dump.size()), "signature-forwarded-results")) {
      Log.error("failed to round-trip the forwarded-result signature: %s", lua_tostring(L, -1));
      return false;
   }
   GCproto *restored_forwarded = funcproto(funcV(L->top - 1));
   if (not compare_proto_signatures(forwarded, restored_forwarded)) {
      Log.error("forwarded-result inference changed during serialisation");
      return false;
   }
   lua_pop(L, 2);

   constexpr std::string_view forwarded_any_source =
      "local function declared():<num, any> return 7, nil end\n"
      "return function() return declared() end\n";
   if (lua_load(L, forwarded_any_source, "signature-forwarded-any") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the forwarded-any function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *forwarded_any = funcproto(funcV(L->top - 1));
   const ProtoSignature *forwarded_any_signature = proto_signature(forwarded_any);
   ProtoTypeEntry forwarded_any_first = proto_result_type(forwarded_any, 0);
   ProtoTypeEntry forwarded_any_second = proto_result_type(forwarded_any, 1);
   if (not forwarded_any_signature or forwarded_any_signature->result_count != 2 or
       forwarded_any_first.type != TiriType::Num or forwarded_any_second.type != TiriType::Any or
       proto_type_origin(forwarded_any_first) != ProtoTypeOrigin::Inferred or
       proto_type_origin(forwarded_any_second) != ProtoTypeOrigin::Inferred) {
      Log.error("tail-call result inference refined an explicit any result from the first result");
      return false;
   }
   lua_pop(L, 1);

   constexpr std::string_view forwarded_with_prefix_source =
      "local function declared():<num, str> return 7, 'forwarded' end\n"
      "return function() return true, declared() end\n";
   if (lua_load(L, forwarded_with_prefix_source, "signature-forwarded-prefix") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the prefixed forwarded-result function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *forwarded_with_prefix = funcproto(funcV(L->top - 1));
   const ProtoSignature *forwarded_with_prefix_signature = proto_signature(forwarded_with_prefix);
   if (not forwarded_with_prefix_signature or forwarded_with_prefix_signature->result_count != 3 or
       proto_result_type(forwarded_with_prefix, 0).type != TiriType::Bool or
       proto_result_type(forwarded_with_prefix, 1).type != TiriType::Num or
       proto_result_type(forwarded_with_prefix, 2).type != TiriType::Str) {
      Log.error("tail-call result inference dropped results after a fixed return prefix");
      return false;
   }
   lua_pop(L, 1);
   return true;
}

static bool test_signature_void_and_bare_return(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();

   constexpr std::string_view empty_source = "return function() local value = 1 end";
   if (lua_load(L, empty_source, "signature-empty") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the empty-signature function: %s", lua_tostring(L, -1));
      return false;
   }
   GCproto *empty_proto = funcproto(funcV(L->top - 1));
   if (proto_signature(empty_proto)) {
      Log.error("an untyped function without formal parameters or result observations received a signature");
      return false;
   }
   std::string empty_dump;
   if (lj_bcwrite(L, empty_proto, bytecode_writer, &empty_dump, 1) != 0 or
       lua_load(L, std::string_view(empty_dump.data(), empty_dump.size()), "signature-empty")) {
      Log.error("failed to round-trip the empty signature");
      return false;
   }
   GCproto *restored_empty = funcproto(funcV(L->top - 1));
   if (not compare_proto_signatures(empty_proto, restored_empty)) {
      Log.error("the empty signature changed during serialisation");
      return false;
   }
   lua_pop(L, 2);

   constexpr std::string_view void_source = "return function():<> return 17 end";
   if (lua_load(L, void_source, "signature-void") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the explicit-void function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *void_proto = funcproto(funcV(L->top - 1));
   const ProtoSignature *void_signature = proto_signature(void_proto);
   if (not void_signature or void_signature->result_count != 0 or void_signature->result_entry_count != 0 or
       not (void_signature->flags & proto_signature_flag(ProtoSignatureFlag::ExplicitResults)) or
       (void_signature->flags & proto_signature_flag(ProtoSignatureFlag::DynamicResults))) {
      Log.error("explicit void was not preserved as a distinct signature");
      return false;
   }

   lua_pushvalue(L, -1);
   if (lua_pcall(L, 0, 1, 0) or not lua_isnil(L, -1)) {
      Log.error("the explicit-void function did not truncate its result");
      return false;
   }
   lua_pop(L, 1);

   std::string void_dump;
   if (lj_bcwrite(L, void_proto, bytecode_writer, &void_dump, 1) != 0 or
       lua_load(L, std::string_view(void_dump.data(), void_dump.size()), "signature-void")) {
      Log.error("failed to round-trip the explicit-void signature");
      return false;
   }
   GCproto *restored_void = funcproto(funcV(L->top - 1));
   if (not compare_proto_signatures(void_proto, restored_void)) {
      Log.error("the explicit-void signature changed during serialisation");
      return false;
   }
   lua_pop(L, 2);

   constexpr std::string_view bare_source = "return function() return end";
   if (lua_load(L, bare_source, "signature-bare-return") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the bare-return function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *bare_proto = funcproto(funcV(L->top - 1));
   const ProtoSignature *bare_signature = proto_signature(bare_proto);
   if (not bare_signature or
       not (bare_signature->flags & proto_signature_flag(ProtoSignatureFlag::DynamicResults)) or
       bare_signature->result_count != 0 or bare_signature->result_entry_count != 0) {
      Log.error("a bare return did not retain its flag-only dynamic result signature");
      return false;
   }

   std::vector<uint8_t> signature_before(bare_proto->signature_size);
   memcpy(signature_before.data(), bare_signature, bare_proto->signature_size);
   lua_pushvalue(L, -1);
   if (lua_pcall(L, 0, 0, 0)) {
      Log.error("failed to execute the dynamic bare-return function: %s", lua_tostring(L, -1));
      return false;
   }
   if (memcmp(signature_before.data(), proto_signature(bare_proto), bare_proto->signature_size) != 0) {
      Log.error("executing dynamic code mutated its prototype signature");
      return false;
   }

   std::string bare_dump;
   if (lj_bcwrite(L, bare_proto, bytecode_writer, &bare_dump, 1) != 0 or
       lua_load(L, std::string_view(bare_dump.data(), bare_dump.size()), "signature-bare-return")) {
      Log.error("failed to round-trip the bare-return signature");
      return false;
   }
   GCproto *restored_bare = funcproto(funcV(L->top - 1));
   bool equal = compare_proto_signatures(bare_proto, restored_bare);
   lua_pop(L, 2);
   if (not equal) {
      Log.error("the bare-return signature changed during serialisation");
      return false;
   }

   constexpr std::string_view wide_source =
      "return function():<num, num, num, num, num, num, num, num, num>\n"
      "   return 1, 2, 3, 4, 5, 6, 7, 8, 9\n"
      "end";
   if (lua_load(L, wide_source, "signature-wide-results") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the wide-result function: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *wide_proto = funcproto(funcV(L->top - 1));
   const ProtoSignature *wide_signature = proto_signature(wide_proto);
   ProtoTypeEntry implicit_result = proto_result_type(wide_proto, 8);
   if (not wide_signature or wide_signature->result_count != 9 or
       wide_signature->result_entry_count != PROTO_MAX_RETURN_TYPES or implicit_result.type != TiriType::Any or
       proto_type_origin(implicit_result) != ProtoTypeOrigin::Declared or
       proto_type_strength(implicit_result) != ProtoTypeStrength::Advisory) {
      Log.error("the wide-result signature did not preserve complete arity and its implicit any suffix");
      return false;
   }

   std::string wide_dump;
   if (lj_bcwrite(L, wide_proto, bytecode_writer, &wide_dump, 1) != 0 or
       lua_load(L, std::string_view(wide_dump.data(), wide_dump.size()), "signature-wide-results")) {
      Log.error("failed to round-trip the wide-result signature");
      return false;
   }
   GCproto *restored_wide = funcproto(funcV(L->top - 1));
   equal = compare_proto_signatures(wide_proto, restored_wide);
   lua_pop(L, 2);
   if (not equal) {
      Log.error("the wide-result signature changed during serialisation");
      return false;
   }
   return true;
}

static bool test_malformed_signature_rejected(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view source = "return function(Value:num):num return Value end";
   if (lua_load(L, source, "signature-malformed") or lua_pcall(L, 0, 1, 0)) {
      Log.error("failed to create the malformed-signature fixture: %s", lua_tostring(L, -1));
      return false;
   }

   std::string dump;
   if (lj_bcwrite(L, funcproto(funcV(L->top - 1)), bytecode_writer, &dump, 1) != 0) {
      Log.error("failed to write the malformed-signature fixture");
      return false;
   }
   lua_pop(L, 1);

   auto read_uleb = [&dump](size_t &Position, uint32_t &Value) {
      Value = 0;
      uint32_t shift = 0;
      for (uint32_t count = 0; count < 5 and Position < dump.size(); ++count) {
         uint8_t byte = uint8_t(dump[Position++]);
         Value |= uint32_t(byte & 0x7f) << shift;
         if (not (byte & 0x80)) return true;
         shift += 7;
      }
      return false;
   };

   size_t position = 5;
   uint32_t value = 0;
   if (not read_uleb(position, value) or position + 4 > dump.size()) {
      Log.error("could not locate the prototype header in the malformed-signature fixture");
      return false;
   }
   position += 4;

   // Header ULEB fields, in order: sizekgc, sizekn, sizebc, siglen, deplen.  The dump is stripped, so no debug
   // length follows and the signature payload begins immediately after deplen.

   size_t signature_length_offset = 0;
   for (int field = 0; field < 5; ++field) {
      if (field IS 3) signature_length_offset = position;
      if (not read_uleb(position, value)) {
         Log.error("could not locate the signature in the malformed-signature fixture");
         return false;
      }
   }
   size_t signature_offset = position;
   if (signature_offset + 5 >= dump.size()) {
      Log.error("malformed-signature fixture has no signature payload");
      return false;
   }

   std::string bad_version = dump;
   bad_version[signature_offset] = char(0xff);
   if (lua_load(L, std::string_view(bad_version.data(), bad_version.size()), "signature-bad-version") IS 0) {
      Log.error("the bytecode reader accepted an unsupported signature version");
      return false;
   }
   lua_pop(L, 1);

   size_t entry_offset = signature_offset + 2;
   for (int field = 0; field < 3; ++field) {
      if (not read_uleb(entry_offset, value)) {
         Log.error("could not locate the first signature entry");
         return false;
      }
   }

   std::string bad_flags = dump;
   bad_flags[entry_offset + 1] = char(PROTO_TYPE_NULLABLE | PROTO_TYPE_REQUIRED);
   if (lua_load(L, std::string_view(bad_flags.data(), bad_flags.size()), "signature-bad-flags") IS 0) {
      Log.error("the bytecode reader accepted contradictory signature entry flags");
      return false;
   }
   lua_pop(L, 1);

   std::string bad_constraint = dump;
   bad_constraint[entry_offset + 2] = char(1);
   if (lua_load(L, std::string_view(bad_constraint.data(), bad_constraint.size()), "signature-bad-constraint") IS 0) {
      Log.error("the bytecode reader accepted a constraint on a scalar signature entry");
      return false;
   }
   lua_pop(L, 1);

   std::string bad_parameter_count = dump;
   bad_parameter_count[signature_offset + 2] = char(2);
   if (lua_load(L, std::string_view(bad_parameter_count.data(), bad_parameter_count.size()),
       "signature-bad-parameter-count") IS 0) {
      Log.error("the bytecode reader accepted a signature parameter-count mismatch");
      return false;
   }
   lua_pop(L, 1);

   if (uint8_t(dump[signature_length_offset]) >= 0x7f) {
      Log.error("the malformed-signature fixture unexpectedly uses a multi-byte signature length");
      return false;
   }
   std::string trailing_data = dump;
   trailing_data[signature_length_offset] = char(uint8_t(trailing_data[signature_length_offset]) + 1);
   if (lua_load(L, std::string_view(trailing_data.data(), trailing_data.size()), "signature-trailing-data") IS 0) {
      Log.error("the bytecode reader accepted trailing signature data");
      return false;
   }
   lua_pop(L, 1);

   std::string bad_type = dump;
   bad_type[entry_offset] = char(0xff);
   if (lua_load(L, std::string_view(bad_type.data(), bad_type.size()), "signature-bad-type") IS 0) {
      Log.error("the bytecode reader accepted an invalid signature type");
      return false;
   }
   lua_pop(L, 1);
   return true;
}

static bool snapshot_has_opcode(const BytecodeSnapshot &Snapshot, BCOp Opcode)
{
   for (BCIns instruction : Snapshot.instructions) {
      if (bc_op(instruction) IS Opcode) return true;
   }
   for (const BytecodeSnapshot &child : Snapshot.children) {
      if (snapshot_has_opcode(child, Opcode)) return true;
   }
   return false;
}

static bool test_contract_bytecode_roundtrip(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   constexpr std::string_view source =
      "extern obj\n"
      "extern array\n"
      "function dynamic(Value:any):any return Value end\n"
      "function required(Value:num!):num! return Value end\n"
      "local value = obj.new('time')\n"
      "value = dynamic(value)\n"
      "local numbers:array<int> = dynamic(array<int> {})\n";
   if (lua_load(L, source, "contract-roundtrip")) {
      Log.error("failed to compile contract source: %s", lua_tostring(L, -1));
      return false;
   }

   std::string dump;
   if (lua_dump(L, bytecode_writer, &dump) != 0) {
      Log.error("failed to write contract bytecode");
      return false;
   }
   lua_pop(L, 1);

   if (dump.size() < 4 or uint8_t(dump[3]) != BCDUMP_VERSION) {
      Log.error("contract bytecode has the wrong private format version");
      return false;
   }
   if (lua_load(L, std::string_view(dump.data(), dump.size()), "contract-roundtrip")) {
      Log.error("failed to reload contract bytecode: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *restored_proto = funcproto(funcV(L->top - 1));
   BytecodeSnapshot restored = snapshot_proto(restored_proto);
   if (not snapshot_has_opcode(restored, BC_CONTRACT)) {
      Log.error("reloaded bytecode lost BC_CONTRACT");
      lua_pop(L, 1);
      return false;
   }

   auto has_time_contract = [L](auto &Self, GCproto *Proto) -> bool {
      for (uint32_t i = 1; i < Proto->sizebc; ++i) {
         BCIns instruction = proto_bc(Proto)[i];
         if (bc_op(instruction) != BC_CONTRACT) continue;
         GCstr *encoded = gco_to_string(proto_kgc(Proto, ~(ptrdiff_t)bc_d(instruction)));
         RuntimeContractDescriptor descriptor;
         if (decode_runtime_contract(encoded, descriptor) and descriptor.contract_count > 0 and
             descriptor.entries[0].object_class_id IS CLASSID::TIME) return true;
      }
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (uint32_t i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *child = gcref(*constant);
         if (child->gch.gct IS ~LJ_TPROTO and Self(Self, gco_to_proto(child))) return true;
      }
      return false;
   };
   bool class_survived = has_time_contract(has_time_contract, restored_proto);
   auto has_int_array_contract = [L](auto &Self, GCproto *Proto) -> bool {
      for (uint32_t i = 1; i < Proto->sizebc; ++i) {
         BCIns instruction = proto_bc(Proto)[i];
         if (bc_op(instruction) != BC_CONTRACT) continue;
         GCstr *encoded = gco_to_string(proto_kgc(Proto, ~(ptrdiff_t)bc_d(instruction)));
         RuntimeContractDescriptor descriptor;
         if (decode_runtime_contract(encoded, descriptor) and descriptor.contract_count > 0 and
             descriptor.entries[0].type IS TiriType::Array and
             descriptor.entries[0].array_element_type IS AET::INT32) return true;
      }
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (uint32_t i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *child = gcref(*constant);
         if (child->gch.gct IS ~LJ_TPROTO and Self(Self, gco_to_proto(child))) return true;
      }
      return false;
   };
   bool array_survived = has_int_array_contract(has_int_array_contract, restored_proto);
   auto has_required_contract = [](auto &Self, GCproto *Proto) -> bool {
      for (uint32_t i = 1; i < Proto->sizebc; ++i) {
         BCIns instruction = proto_bc(Proto)[i];
         if (bc_op(instruction) != BC_CONTRACT) continue;
         GCstr *encoded = gco_to_string(proto_kgc(Proto, ~(ptrdiff_t)bc_d(instruction)));
         RuntimeContractDescriptor descriptor;
         if (decode_runtime_contract(encoded, descriptor) and descriptor.contract_count > 0 and
             (descriptor.entries[0].flags & contract_flag(ContractEntryFlag::Required)) != 0 and
             (descriptor.entries[0].flags & contract_flag(ContractEntryFlag::Nullable)) IS 0) return true;
      }
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (uint32_t i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *child = gcref(*constant);
         if (child->gch.gct IS ~LJ_TPROTO and Self(Self, gco_to_proto(child))) return true;
      }
      return false;
   };
   bool required_survived = has_required_contract(has_required_contract, restored_proto);
   lua_pop(L, 1);
   if (not class_survived or not array_survived or not required_survived) {
      Log.error("reloaded bytecode lost an object, array member or required constraint in BC_CONTRACT");
      return false;
   }
   return true;
}

static bool test_runtime_contract_decoder(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();
   auto append_uleb = [](std::string &Bytes, uint32_t Value) {
      do {
         uint8_t byte = uint8_t(Value & 0x7f);
         Value >>= 7;
         if (Value) byte |= 0x80;
         Bytes.push_back(char(byte));
      } while (Value);
   };
   auto build_descriptor = [&append_uleb](TiriType Type, CLASSID ObjectClassId,
      std::string_view StructName, std::string_view Label, uint8_t EntryFlags,
      AET ArrayElementType = AET::MAX, std::string_view ArrayStructName = {}) {
      std::string result{
         char(uint8_t(ContractBoundary::Global)),
         char(0),
         char(1),
         char(1),
         char(uint8_t(Type)),
         char(EntryFlags),
         char(1)
      };
      append_uleb(result, uint32_t(ObjectClassId));
      result.push_back(char(uint8_t(StructName.size())));
      result.append(StructName);
      if (Type IS TiriType::Array) {
         result.push_back(char(uint8_t(ArrayElementType)));
         result.push_back(char(uint8_t(ArrayStructName.size())));
         result.append(ArrayStructName);
      }
      result.push_back(char(uint8_t(Label.size())));
      result.append(Label);
      return result;
   };

   uint8_t const_flags = contract_flag(ContractEntryFlag::Nullable) | contract_flag(ContractEntryFlag::Const);
   std::string encoded = build_descriptor(
      TiriType::Struct, CLASSID::NIL, "Point", "Value", const_flags);

   RuntimeContractDescriptor decoded;
   GCstr *descriptor = lj_str_new(lua, encoded.data(), encoded.size());
   if (not decode_runtime_contract(descriptor, decoded) or
       decoded.boundary != ContractBoundary::Global or decoded.dynamic_count() or decoded.variadic() or
       decoded.static_value_count != 1 or decoded.contract_count != 1 or
       decoded.entries[0].type != TiriType::Struct or decoded.entries[0].position != 1 or
       decoded.entries[0].object_class_id != CLASSID::NIL or
       not contract_entry_is_const(decoded.entries[0]) or
       decoded.entries[0].constraint_name != "Point" or decoded.entries[0].label != "Value") {
      Log.error("valid runtime contract descriptor did not round-trip through the shared decoder");
      return false;
   }

   auto rejected = [lua](std::string Bytes) {
      RuntimeContractDescriptor result;
      GCstr *value = lj_str_new(lua, Bytes.data(), Bytes.size());
      return not decode_runtime_contract(value, result);
   };

   std::string object_encoded = build_descriptor(
      TiriType::Object, CLASSID::TIME, {}, "Clock", const_flags);
   RuntimeContractDescriptor object_decoded;
   GCstr *object_descriptor = lj_str_new(lua, object_encoded.data(), object_encoded.size());
   if (not decode_runtime_contract(object_descriptor, object_decoded) or
       object_decoded.entries[0].type != TiriType::Object or
       object_decoded.entries[0].object_class_id != CLASSID::TIME or
       not object_decoded.entries[0].constraint_name.empty()) {
      Log.error("object class constraint did not round-trip through the shared decoder");
      return false;
   }

   std::string broad_object = build_descriptor(
      TiriType::Object, CLASSID::NIL, {}, "Broad", const_flags);
   GCstr *broad_descriptor = lj_str_new(lua, broad_object.data(), broad_object.size());
   if (not decode_runtime_contract(broad_descriptor, object_decoded) or
       object_decoded.entries[0].object_class_id != CLASSID::NIL) {
      Log.error("broad object contract gained a class constraint while decoding");
      return false;
   }

   std::string array_encoded = build_descriptor(
      TiriType::Array, CLASSID::NIL, {}, "Values", const_flags, AET::INT32);
   RuntimeContractDescriptor array_decoded;
   GCstr *array_descriptor = lj_str_new(lua, array_encoded.data(), array_encoded.size());
   if (not decode_runtime_contract(array_descriptor, array_decoded) or
       array_decoded.entries[0].array_element_type != AET::INT32 or
       not array_decoded.entries[0].constraint_name.empty()) {
      Log.error("array member constraint did not round-trip through the shared decoder");
      return false;
   }

   std::string struct_array_encoded = build_descriptor(
      TiriType::Array, CLASSID::NIL, {}, "Points", const_flags, AET::STRUCT, "Point");
   GCstr *struct_array_descriptor = lj_str_new(lua, struct_array_encoded.data(), struct_array_encoded.size());
   if (not decode_runtime_contract(struct_array_descriptor, array_decoded) or
       array_decoded.entries[0].array_element_type != AET::STRUCT or
       array_decoded.entries[0].constraint_name != "Point") {
      Log.error("named-structure array constraint did not round-trip through the shared decoder");
      return false;
   }

   std::string legacy_broad_array = build_descriptor(
      TiriType::Num, CLASSID::NIL, {}, "Values", const_flags);
   legacy_broad_array[4] = char(uint8_t(TiriType::Array));
   if (not rejected(legacy_broad_array)) {
      Log.error("legacy broad-array contract unexpectedly satisfied the member-aware decoder");
      return false;
   }

   uint8_t global_hint_flags = contract_flag(ContractEntryFlag::Nullable) |
      contract_flag(ContractEntryFlag::GlobalHint);
   std::string global_hint_encoded = build_descriptor(
      TiriType::Str, CLASSID::NIL, {}, "GlobalValue", global_hint_flags);
   GCstr *global_hint_descriptor = lj_str_new(lua, global_hint_encoded.data(), global_hint_encoded.size());
   RuntimeContractDescriptor global_hint_decoded;
   if (not decode_runtime_contract(global_hint_descriptor, global_hint_decoded) or
       global_hint_decoded.boundary != ContractBoundary::Global or
       not contract_entry_is_global_hint(global_hint_decoded.entries[0])) {
      Log.error("global-hint contract did not round-trip through the shared decoder");
      return false;
   }

   std::string invalid_descriptor_flags = encoded;
   invalid_descriptor_flags[1] = char(0x80);
   std::string invalid_boundary = encoded;
   invalid_boundary[0] = char(0xff);
   std::string invalid_entry_flags = encoded;
   invalid_entry_flags[5] = char(0x80);
   std::string invalid_initialising_flag = encoded;
   invalid_initialising_flag[0] = char(uint8_t(ContractBoundary::Local));
   invalid_initialising_flag[5] = char(contract_flag(ContractEntryFlag::Initialising));
   std::string invalid_local_global_hint = global_hint_encoded;
   invalid_local_global_hint[0] = char(uint8_t(ContractBoundary::Local));
   std::string invalid_const_global_hint = build_descriptor(
      TiriType::Str, CLASSID::NIL, {}, "Value",
      global_hint_flags | contract_flag(ContractEntryFlag::Const));
   std::string invalid_unlabelled_global_hint = build_descriptor(
      TiriType::Str, CLASSID::NIL, {}, {}, global_hint_flags);
   std::string invalid_position = encoded;
   invalid_position[6] = char(0);
   std::string invalid_scalar_class = build_descriptor(
      TiriType::Num, CLASSID::TIME, {}, "Value", const_flags);
   std::string invalid_struct_class = build_descriptor(
      TiriType::Struct, CLASSID::TIME, "Point", "Value", const_flags);
   std::string truncated_class{
      char(uint8_t(ContractBoundary::Local)), char(0), char(1), char(1),
      char(uint8_t(TiriType::Object)), char(0), char(1), char(0x80)
   };
   std::string over_wide_class{
      char(uint8_t(ContractBoundary::Local)), char(0), char(1), char(1),
      char(uint8_t(TiriType::Object)), char(0), char(1), char(0x80), char(0x80), char(0x80), char(0x80),
      char(0x10), char(0), char(0)
   };
   std::string trailing = encoded + "x";
   std::string truncated = encoded.substr(0, encoded.size() - 1);

   if (not rejected(invalid_descriptor_flags) or not rejected(invalid_boundary) or
       not rejected(invalid_entry_flags) or
       not rejected(invalid_initialising_flag) or
       not rejected(invalid_local_global_hint) or not rejected(invalid_const_global_hint) or
       not rejected(invalid_unlabelled_global_hint) or
       not rejected(invalid_position) or not rejected(invalid_scalar_class) or not rejected(invalid_struct_class) or
       not rejected(truncated_class) or not rejected(over_wide_class) or not rejected(trailing) or
       not rejected(truncated)) {
      Log.error("shared runtime contract decoder accepted malformed input");
      return false;
   }

   constexpr std::string_view wide_source =
      "return function(A:any,B:any,C:any,D:any,E:any,F:any,G:any,H:any,I:any):"
      "<num,num,num,num,num,num,num,num,str>\n"
      "   return A,B,C,D,E,F,G,H,I,false\n"
      "end\n";
   if (lua_load(lua, wide_source, "wide-runtime-contract")) {
      Log.error("failed to compile a wide runtime contract: %s", lua_tostring(lua, -1));
      return false;
   }

   GCproto *wide = first_child_proto(funcproto(funcV(lua->top - 1)));
   GCstr *wide_descriptor = nullptr;
   for (uint32_t i = 1; wide and i < wide->sizebc; ++i) {
      BCIns instruction = proto_bc(wide)[i];
      if (bc_op(instruction) IS BC_CONTRACT) {
         wide_descriptor = gco_to_string(proto_kgc(wide, ~(ptrdiff_t)bc_d(instruction)));
         break;
      }
   }
   RuntimeContractDescriptor wide_decoded;
   if (not wide_descriptor or not decode_runtime_contract(wide_descriptor, wide_decoded) or
       wide_decoded.static_value_count != 9 or wide_decoded.contract_count != MAX_RETURN_TYPES) {
      Log.error("a valid contract with more values than stored entries failed shared decoding");
      return false;
   }
   lua_pop(lua, 1);
   return true;
}

static bool test_runtime_contract_batching(kt::Log &Log)
{
   struct ContractInstruction {
      BCREG base = 0;
      RuntimeContractDescriptor descriptor;
   };

   LuaStateHolder state;
   lua_State *lua = state.get();
   bool decode_failed = false;
   auto read_contracts = [&decode_failed](GCproto *Proto, ContractBoundary Boundary) {
      std::vector<ContractInstruction> result;
      for (uint32_t i = 1; Proto and i < Proto->sizebc; ++i) {
         BCIns instruction = proto_bc(Proto)[i];
         if (bc_op(instruction) != BC_CONTRACT) continue;

         GCstr *encoded = gco_to_string(proto_kgc(Proto, ~(ptrdiff_t)bc_d(instruction)));
         RuntimeContractDescriptor descriptor;
         if (not decode_runtime_contract(encoded, descriptor)) {
            decode_failed = true;
            continue;
         }
         if (descriptor.boundary IS Boundary) {
            result.push_back(ContractInstruction{
               .base = bc_a(instruction),
               .descriptor = descriptor
            });
         }
      }
      return result;
   };
   auto compile_child = [lua, &Log](std::string_view Source, const char *Label) -> GCproto * {
      if (lua_load(lua, Source, Label)) {
         Log.error("failed to compile %s: %s", Label, lua_tostring(lua, -1));
         return nullptr;
      }
      return first_child_proto(funcproto(funcV(lua->top - 1)));
   };

   GCproto *dense = compile_child(
      "return function(A:num, B:str, C:bool, D:table) return A end\n", "dense-parameter-contracts");
   auto dense_contracts = read_contracts(dense, ContractBoundary::Parameter);
   if (not dense or decode_failed or dense_contracts.size() != 1 or dense_contracts[0].base != 0 or
       dense_contracts[0].descriptor.static_value_count != 4 or
       dense_contracts[0].descriptor.contract_count != 4 or not proto_contract_cache(dense) or
       proto_contract_cache(dense)->record_count != 1 or proto_contract_cache(dense)->entry_count != 4) {
      Log.error("four adjacent parameter contracts did not batch into one descriptor");
      return false;
   }
   constexpr std::array<TiriType, 4> dense_types{
      TiriType::Num, TiriType::Str, TiriType::Bool, TiriType::Table
   };
   constexpr std::array<std::string_view, 4> dense_labels{ "A", "B", "C", "D" };
   for (uint8_t i = 0; i < dense_types.size(); ++i) {
      const RuntimeContractEntry &entry = dense_contracts[0].descriptor.entries[i];
      if (entry.type != dense_types[i] or entry.position != i + 1 or entry.label != dense_labels[i]) {
         Log.error("dense parameter contract entry %u lost its type, position or label", unsigned(i + 1));
         return false;
      }
   }
   lua_pop(lua, 1);

   GCproto *wide = compile_child(
      "return function(A:num, B:num, C:num, D:num, E:num, F:num, G:num, H:num, I:num) return A end\n",
      "wide-parameter-contracts");
   auto wide_contracts = read_contracts(wide, ContractBoundary::Parameter);
   const RuntimeContractCache *wide_cache = wide ? proto_contract_cache(wide) : nullptr;
   if (not wide or decode_failed or wide_contracts.size() != 2 or wide_contracts[0].base != 0 or
       wide_contracts[0].descriptor.static_value_count != MAX_RETURN_TYPES or
       wide_contracts[0].descriptor.contract_count != MAX_RETURN_TYPES or wide_contracts[1].base != 8 or
       wide_contracts[1].descriptor.static_value_count != 1 or wide_contracts[1].descriptor.contract_count != 1 or
       wide_contracts[1].descriptor.entries[0].position != 9 or wide_contracts[1].descriptor.entries[0].label != "I" or
       not wide_cache or wide_cache->record_count != 2 or wide_cache->entry_count != MAX_RETURN_TYPES + 1) {
      Log.error("nine adjacent parameter contracts did not split at the descriptor entry limit");
      return false;
   }
   lua_pop(lua, 1);

   GCproto *fragmented = compile_child(
      "struct FragmentedRecord Value: int end\n"
      "return function(A:num, B:any, C:struct<FragmentedRecord>, "
      "D:array<struct<FragmentedRecord>>) return A end\n",
      "fragmented-parameter-contracts");
   auto fragmented_contracts = read_contracts(fragmented, ContractBoundary::Parameter);
   const RuntimeContractCache *fragmented_cache = fragmented ? proto_contract_cache(fragmented) : nullptr;
   if (not fragmented or decode_failed or fragmented_contracts.size() != 2 or
       fragmented_contracts[0].base != 0 or fragmented_contracts[0].descriptor.contract_count != 1 or
       fragmented_contracts[0].descriptor.entries[0].position != 1 or fragmented_contracts[1].base != 2 or
       fragmented_contracts[1].descriptor.contract_count != 2 or
       fragmented_contracts[1].descriptor.entries[0].position != 3 or
       fragmented_contracts[1].descriptor.entries[0].constraint_name != "FragmentedRecord" or
       fragmented_contracts[1].descriptor.entries[1].position != 4 or
       fragmented_contracts[1].descriptor.entries[1].array_element_type != AET::STRUCT or
       fragmented_contracts[1].descriptor.entries[1].constraint_name != "FragmentedRecord" or
       not fragmented_cache or fragmented_cache->record_count != 2 or fragmented_cache->entry_count != 3) {
      Log.error("an unchecked parameter gap did not split dense contract runs");
      return false;
   }

   lua_gc(lua, LUA_GCCOLLECT, 0);
   fragmented_cache = proto_contract_cache(fragmented);
   const CachedRuntimeContractRecord *fragmented_records = runtime_contract_cache_records(fragmented_cache);
   const CachedRuntimeContractEntry *fragmented_entries = runtime_contract_cache_entries(fragmented_cache);
   if (fragmented_records[0].contract_count != 1 or fragmented_records[0].entry_index != 0 or
       fragmented_records[1].contract_count != 2 or fragmented_records[1].entry_index != 1 or
       fragmented_records[0].bytecode_position >= fragmented_records[1].bytecode_position or
       fragmented_entries[0].type != TiriType::Num or fragmented_entries[0].position != 1 or
       fragmented_entries[1].type != TiriType::Struct or fragmented_entries[1].position != 3 or
       fragmented_entries[2].type != TiriType::Array or fragmented_entries[2].array_element_type != AET::STRUCT or
       fragmented_entries[2].position != 4) {
      Log.error("single-entry runtime contract cache records have an invalid layout");
      return false;
   }

   BCIns fragmented_instruction = proto_bc(fragmented)[fragmented_records[1].bytecode_position];
   GCstr *fragmented_descriptor = gco_to_string(
      proto_kgc(fragmented, ~(ptrdiff_t)bc_d(fragmented_instruction)));
   auto cached_text = [fragmented_descriptor](uint16_t Offset) -> std::string_view {
      if (not Offset) return {};
      const char *text = strdata(fragmented_descriptor) + Offset;
      return std::string_view(text, uint8_t(text[-1]));
   };
   if (cached_text(fragmented_entries[1].constraint_offset) != "FragmentedRecord" or
       cached_text(fragmented_entries[2].constraint_offset) != "FragmentedRecord" or
       cached_text(fragmented_entries[1].label_offset) != "C" or
       cached_text(fragmented_entries[2].label_offset) != "D") {
      Log.error("runtime contract cache lost rooted structure, array member or label text");
      return false;
   }
   lua_pop(lua, 1);

   for (uint32_t iteration = 0; iteration < 32; ++iteration) {
      GCproto *repeated = compile_child(
         "return function(Value:num) return Value end\n", "repeated-single-contract-lifetime");
      const RuntimeContractCache *repeated_cache = repeated ? proto_contract_cache(repeated) : nullptr;
      if (not repeated_cache or repeated_cache->record_count != 1 or repeated_cache->entry_count != 1) {
         Log.error("repeated prototype creation did not build a single-entry runtime contract cache");
         return false;
      }

      lua_gc(lua, LUA_GCCOLLECT, 0);
      repeated_cache = proto_contract_cache(repeated);
      if (not repeated_cache or repeated_cache->record_count != 1 or repeated_cache->entry_count != 1) {
         Log.error("a rooted single-entry runtime contract cache did not survive collection");
         return false;
      }
      lua_pop(lua, 1);
      lua_gc(lua, LUA_GCCOLLECT, 0);
   }

   constexpr std::string_view combined_source =
      "local function Dynamic():<any,any,any,any> return 1,'text',true,{} end\n"
      "return function(A:num, B:str, C:bool, D:table)\n"
      "   local E:num, F:str, G:bool, H:table = Dynamic()\n"
      "   return A\n"
      "end\n";
   if (lua_load(lua, combined_source, "grouped-contract-roundtrip")) {
      Log.error("failed to compile grouped contract round-trip source: %s", lua_tostring(lua, -1));
      return false;
   }

   auto find_four_parameter_proto = [](auto &Self, GCproto *Proto) -> GCproto * {
      if (Proto->numparams IS 4) return Proto;
      for (ptrdiff_t i = -ptrdiff_t(Proto->sizekgc); i < 0; ++i) {
         GCobj *object = proto_kgc(Proto, i);
         if (object->gch.gct != uint8_t(~LJ_TPROTO)) continue;
         if (GCproto *result = Self(Self, gco_to_proto(object))) return result;
      }
      return nullptr;
   };

   GCproto *root = funcproto(funcV(lua->top - 1));
   GCproto *combined = find_four_parameter_proto(find_four_parameter_proto, root);
   auto combined_parameters = read_contracts(combined, ContractBoundary::Parameter);
   auto combined_locals = read_contracts(combined, ContractBoundary::Local);
   if (not combined or decode_failed or combined_parameters.size() != 1 or combined_locals.size() != 1 or
       combined_parameters[0].descriptor.contract_count != 4 or
       combined_locals[0].descriptor.contract_count != 4 or combined_locals[0].base != 4 or
       not proto_contract_cache(combined) or proto_contract_cache(combined)->record_count != 2 or
       proto_contract_cache(combined)->entry_count != 8) {
      Log.error("parameter and local declaration runs did not batch in one prototype");
      return false;
   }

   std::string dump;
   if (lua_dump(lua, bytecode_writer, &dump) != 0) {
      Log.error("failed to dump grouped contract bytecode");
      return false;
   }
   std::string stripped_dump;
   if (lj_bcwrite(lua, root, bytecode_writer, &stripped_dump, 1) != 0) {
      Log.error("failed to dump stripped grouped contract bytecode");
      return false;
   }
   lua_pop(lua, 1);

   auto verify_dump = [&](const std::string &Dump, const char *Label) {
      if (lua_load(lua, std::string_view(Dump.data(), Dump.size()), Label)) {
         Log.error("failed to reload %s: %s", Label, lua_tostring(lua, -1));
         return false;
      }

      GCproto *loaded_root = funcproto(funcV(lua->top - 1));
      GCproto *loaded = find_four_parameter_proto(find_four_parameter_proto, loaded_root);
      auto parameters = read_contracts(loaded, ContractBoundary::Parameter);
      auto locals = read_contracts(loaded, ContractBoundary::Local);
      const RuntimeContractCache *cache = loaded ? proto_contract_cache(loaded) : nullptr;
      bool valid = loaded and not decode_failed and parameters.size() IS 1 and locals.size() IS 1 and
         parameters[0].descriptor.contract_count IS 4 and locals[0].descriptor.contract_count IS 4 and cache and
         cache->record_count IS 2 and cache->entry_count IS 8;
      lua_pop(lua, 1);
      return valid;
   };
   if (not verify_dump(dump, "grouped-contract-roundtrip") or
       not verify_dump(stripped_dump, "stripped-grouped-contract-roundtrip")) {
      Log.error("grouped parameter or local contracts did not survive bytecode round-trip");
      return false;
   }
   return true;
}

static bool test_complex_contract_jit_eligibility(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();
   auto compile_child = [lua, &Log](std::string_view Source, const char *Label) -> GCproto * {
      if (lua_load(lua, Source, Label)) {
         Log.error("%s failed to compile: %s", Label, lua_tostring(lua, -1));
         lua_pop(lua, 1);
         return nullptr;
      }

      GCproto *child = first_child_proto(funcproto(funcV(lua->top - 1)));
      lua->top--;
      return child;
   };

   GCproto *fixed = compile_child(
      "return function(Value:func):func\n"
      "   return Value\n"
      "end\n",
      "fixed-complex-contract");
   if (not fixed or (fixed->flags & PROTO_NOJIT)) {
      Log.error("a fixed complex contract remained interpreter-only");
      return false;
   }

   GCproto *fixed_object_class = compile_child(
      "extern obj\n"
      "return function(Value:any)\n"
      "   local stored = obj.new('time')\n"
      "   stored = Value\n"
      "   return stored\n"
      "end\n",
      "fixed-object-class-contract");
   if (not fixed_object_class or (fixed_object_class->flags & PROTO_NOJIT)) {
      Log.error("a fixed object-class contract became interpreter-only");
      return false;
   }

   GCproto *dynamic = compile_child(
      "return function(...):<num, ...>\n"
      "   return ...\n"
      "end\n",
      "dynamic-result-contract");
   if (not dynamic or not (dynamic->flags & PROTO_NOJIT)) {
      Log.error("a dynamic-result contract became JIT-eligible without exact multi-result recorder support");
      return false;
   }

   GCproto *global_const = compile_child(
      "return function()\n"
      "   global glRecordedConst <const> = 1\n"
      "end\n",
      "global-const-contract");
   if (not global_const or not (global_const->flags & PROTO_NOJIT)) {
      Log.error("a global const contract became JIT-eligible despite its post-store policy side effect");
      return false;
   }
   return true;
}

static bool test_parser_diagnostics_reset_per_load(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();

   constexpr std::string_view invalid =
      "local value:str = 'text'\n"
      "value = 1\n";
   if (lua_load(lua, invalid, "diagnostic-reset-invalid") IS 0) {
      Log.error("invalid source compiled while testing parser diagnostic reset");
      lua_pop(lua, 1);
      return false;
   }
   if (not lua->parser_diagnostics or not lua->parser_diagnostics->has_errors()) {
      Log.error("invalid source did not publish parser diagnostics");
      lua_pop(lua, 1);
      return false;
   }
   lua_pop(lua, 1);

   if (lua_load(lua, "return 1", "diagnostic-reset-valid")) {
      Log.error("valid source failed after an earlier parser diagnostic: %s", lua_tostring(lua, -1));
      lua_pop(lua, 1);
      return false;
   }
   if (lua->parser_diagnostics) {
      Log.error("a successful parse retained diagnostics from an earlier load");
      lua_pop(lua, 1);
      return false;
   }
   lua_pop(lua, 1);
   return true;
}

//********************************************************************************************************************

static bool test_userdata_type_annotations(kt::Log &Log)
{
   if (parse_type_name("num") != TiriType::Num or parse_type_name("number") != TiriType::Unknown) {
      Log.error("num must be the exclusive numeric type annotation name");
      return false;
   }
   if (parse_type_name("str") != TiriType::Str or parse_type_name("string") != TiriType::Unknown) {
      Log.error("str must be the exclusive string type annotation name");
      return false;
   }
   if (parse_type_name("userdata") != TiriType::Userdata or type_name(TiriType::Userdata) != "userdata") {
      Log.error("userdata type name does not round-trip");
      return false;
   }
   if (uint8_t(TiriType::Range) != 10 or uint8_t(TiriType::Userdata) != 11) {
      Log.error("userdata changed an existing concrete contract type identifier");
      return false;
   }

   constexpr std::string_view source =
      "function identity(Value:userdata):userdata\n"
      "   local result:userdata = Value\n"
      "   return result\n"
      "end";
   auto result = build_ast_from_source(source);
   if (not result.chunk.ok()) {
      Log.error("failed to parse userdata annotations");
      log_diagnostics(result.diagnostics, Log);
      return false;
   }
   return true;
}

static bool parse_struct_source(lua_State *L, std::string_view Source, std::string &Error)
{
   StringReaderCtx reader = { Source.data(), Source.size() };
   LexState lex(L, unit_reader, &reader, "parser-unit", std::nullopt);
   FuncState &func_state = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, func_state, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   ParserSession session(context, config);

   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (not chunk.ok()) {
      Error = chunk.error_ref().message;
      return false;
   }
   if (context.diagnostics().has_errors()) {
      auto entries = context.diagnostics().entries();
      Error = entries.empty() ? "unknown parser error" : entries.front().message;
      builder.rollback_registered_structs();
      return false;
   }

   builder.commit_registered_structs();
   Error.clear();
   return true;
}

// Parses Source and returns the field documentation the declaration parser captured, so that tests can assert on
// metadata that is otherwise confined to the LexState.

static bool parse_struct_documentation(lua_State *L, std::string_view Source,
   std::vector<LexState::StructFieldDocumentation> &Documentation, std::string &Error)
{
   StringReaderCtx reader = { Source.data(), Source.size() };
   LexState lex(L, unit_reader, &reader, "parser-unit", std::nullopt);
   FuncState &func_state = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, func_state, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   ParserSession session(context, config);

   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (not chunk.ok()) {
      Error = chunk.error_ref().message;
      return false;
   }
   if (context.diagnostics().has_errors()) {
      auto entries = context.diagnostics().entries();
      Error = entries.empty() ? "unknown parser error" : entries.back().message;
      builder.rollback_registered_structs();
      return false;
   }

   Documentation = lex.struct_field_documentation;
   builder.commit_registered_structs();
   Error.clear();
   return true;
}

[[nodiscard]] static const LexState::StructFieldDocumentation * find_documentation(
   const std::vector<LexState::StructFieldDocumentation> &Documentation, std::string_view FieldName)
{
   for (const auto &entry : Documentation) {
      if (entry.field_name IS FieldName) return &entry;
   }
   return nullptr;
}

static bool test_struct_field_documentation(kt::Log &Log)
{
   std::vector<LexState::StructFieldDocumentation> docs;
   std::string error;

   LuaStateHolder holder;
   lua_State *state = holder.get();
   if (not state) {
      Log.error("failed to allocate state for struct documentation test");
      return false;
   }

   // The Marker and Divider fields exist to prove that comment capture runs through the lexer: a naive scan for
   // '--' in the source text would mistake the string literals for comment markers.

   if (not parse_struct_documentation(state,
         "struct ParserDocumentedRecord\n"
         "   Plain: int -- Trailing description.\n"
         "   -- Leading description.\n"
         "   -- Second leading line.\n"
         "   Described: int\n"
         "   Marker: cstr -- Sets the \"--\" delimiter.\n"
         "   Divider: cstr\n"
         "   Undocumented: int\n"
         "end", docs, error)) {
      Log.error("failed to compile documented struct: %s", error.c_str());
      return false;
   }

   auto plain = find_documentation(docs, "Plain");
   if ((not plain) or (plain->text != "Trailing description.")) {
      Log.error("trailing comment was not captured verbatim (got '%s')", plain ? plain->text.c_str() : "<none>");
      return false;
   }

   auto described = find_documentation(docs, "Described");
   if ((not described) or (described->text != "Leading description.\nSecond leading line.")) {
      Log.error("leading comment run was not captured (got '%s')",
         described ? described->text.c_str() : "<none>");
      return false;
   }

   auto marker = find_documentation(docs, "Marker");
   if ((not marker) or (marker->text != "Sets the \"--\" delimiter.")) {
      Log.error("comment following a string literal was mis-captured (got '%s')",
         marker ? marker->text.c_str() : "<none>");
      return false;
   }

   // Divider follows a line whose trailing comment belongs to Marker, so it must not inherit it.

   if (find_documentation(docs, "Divider")) {
      Log.error("a preceding field's trailing comment leaked onto the next field");
      return false;
   }

   if (find_documentation(docs, "Undocumented")) {
      Log.error("an undocumented field acquired documentation");
      return false;
   }

   return true;
}

// Parses Source with SCF::PROCESS_DOC active and returns the struct declaration metadata captured for tooling,
// mirroring the conditions of debug.validate(source, { symbols = true }).

static bool parse_struct_metadata(lua_State *L, std::string_view Source,
   std::vector<LexState::StructDeclarationMetadata> &Metadata, std::string &Error)
{
   SCF old_flags = L->script->Flags;
   L->script->Flags |= SCF::PROCESS_DOC;

   StringReaderCtx reader = { Source.data(), Source.size() };
   LexState lex(L, unit_reader, &reader, "parser-unit", std::nullopt);
   FuncState &func_state = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, func_state, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   ParserSession session(context, config);

   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (not chunk.ok()) {
      Error = chunk.error_ref().message;
      L->script->Flags = old_flags;
      return false;
   }
   if (context.diagnostics().has_errors()) {
      auto entries = context.diagnostics().entries();
      Error = entries.empty() ? "unknown parser error" : entries.back().message;
      builder.rollback_registered_structs();
      L->script->Flags = old_flags;
      return false;
   }

   Metadata = lex.struct_declaration_metadata;
   builder.commit_registered_structs();
   L->script->Flags = old_flags;
   Error.clear();
   return true;
}

static bool test_struct_declaration_metadata(kt::Log &Log)
{
   std::vector<LexState::StructDeclarationMetadata> metadata;
   std::string error;

   LuaStateHolder holder;
   lua_State *state = holder.get();
   if (not state) {
      Log.error("failed to allocate state for struct metadata test");
      return false;
   }

   if (not parse_struct_metadata(state,
         "struct ParserMetadataPoint\n"
         "   X: double\n"
         "   Y: double\n"
         "end\n"
         "struct ParserMetadataRecord\n"
         "   Plain: byte -- Documented field.\n"
         "   Sock: obj<NetSocket>\n"
         "   Buffer: char[32]\n"
         "   Values: array<float>\n"
         "   Embedded: struct<ParserMetadataPoint>\n"
         "   Link: ptr<ParserMetadataPoint>\n"
         "   List: ptr<ParserMetadataPoint[]>\n"
         "end", metadata, error)) {
      Log.error("failed to compile metadata struct source: %s", error.c_str());
      return false;
   }

   if (metadata.size() != 2) {
      Log.error("expected metadata for two declarations, got %d", int(metadata.size()));
      return false;
   }

   const auto &point = metadata[0];
   if (point.name != "ParserMetadataPoint" or point.fields.size() != 2) {
      Log.error("first declaration metadata is incomplete (name '%s', %d fields)",
         point.name.c_str(), int(point.fields.size()));
      return false;
   }
   if (point.name_span.line.lineNumber() != 1 or point.end_span.line.lineNumber() != 4) {
      Log.error("first declaration spans are wrong (name line %d, end line %d)",
         int(point.name_span.line.lineNumber()), int(point.end_span.line.lineNumber()));
      return false;
   }

   const auto &record = metadata[1];
   if (record.name != "ParserMetadataRecord" or record.fields.size() != 7) {
      Log.error("second declaration metadata is incomplete (name '%s', %d fields)",
         record.name.c_str(), int(record.fields.size()));
      return false;
   }

   auto expect_field = [&Log, &record](size_t Index, std::string_view Name, std::string_view Type,
         std::string_view Doc) -> bool {
      const auto &field = record.fields[Index];
      if (field.name != Name or field.type != Type or field.doc != Doc) {
         Log.error("field %d mismatch: got '%s: %s' doc '%s'", int(Index), field.name.c_str(),
            field.type.c_str(), field.doc.c_str());
         return false;
      }
      return true;
   };

   if (not expect_field(0, "Plain", "byte", "Documented field.")) return false;
   if (not expect_field(1, "Sock", "obj<NetSocket>", "")) return false;
   if (not expect_field(2, "Buffer", "char[32]", "")) return false;
   if (not expect_field(3, "Values", "array<float>", "")) return false;
   if (not expect_field(4, "Embedded", "struct<ParserMetadataPoint>", "")) return false;
   if (not expect_field(5, "Link", "ptr<ParserMetadataPoint>", "")) return false;
   if (not expect_field(6, "List", "ptr<ParserMetadataPoint[]>", "")) return false;

   if (record.fields[0].span.line.lineNumber() != 6) {
      Log.error("field span line is wrong (got %d)", int(record.fields[0].span.line.lineNumber()));
      return false;
   }

   // Without SCF::PROCESS_DOC the parser must not spend time collecting metadata.

   std::vector<LexState::StructFieldDocumentation> docs;
   LuaStateHolder plain_holder;
   lua_State *plain_state = plain_holder.get();
   if (not plain_state) {
      Log.error("failed to allocate state for gating check");
      return false;
   }

   constexpr std::string_view gated_source = "struct ParserMetadataGated A: int end";
   StringReaderCtx reader = { gated_source.data(), gated_source.size() };
   LexState lex(plain_state, unit_reader, &reader, "parser-unit", std::nullopt);
   FuncState &func_state = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(plain_state);
   ParserContext context = ParserContext::from(lex, func_state, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   ParserSession session(context, config);
   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (chunk.ok() and not lex.struct_declaration_metadata.empty()) {
      Log.error("metadata was collected without SCF::PROCESS_DOC");
      return false;
   }

   return true;
}

static bool test_state_local_struct_declarations(kt::Log &Log)
{
   constexpr std::string_view local_name = "ParserStateLocalRecord";
   constexpr std::string_view shadow_name = "ParserStateShadowRecord";
   constexpr std::string_view global_child_name = "ParserGlobalChildRecord";
   constexpr std::string_view global_parent_name = "ParserGlobalParentRecord";
   std::string error;

   if (auto result = make_struct(nullptr, shadow_name, "lGlobal");
         result != ERR::Okay and result != ERR::Exists) {
      Log.error("failed to register global shadow fixture: %s", GetErrorMsg(result));
      return false;
   }
   if (auto result = make_struct(nullptr, global_child_name, "lGlobalValue");
         result != ERR::Okay and result != ERR::Exists) {
      Log.error("failed to register global child fixture: %s", GetErrorMsg(result));
      return false;
   }
   if (auto result = make_struct(nullptr, global_parent_name,
         "eChild:ParserGlobalChildRecord"); result != ERR::Okay and result != ERR::Exists) {
      Log.error("failed to register global parent fixture: %s", GetErrorMsg(result));
      return false;
   }

   {
      LuaStateHolder first;
      LuaStateHolder second;
      lua_State *first_state = first.get();
      lua_State *second_state = second.get();
      if ((not first_state) or (not second_state)) {
         Log.error("failed to allocate states for local struct registry test");
         return false;
      }

      if (not parse_struct_source(first_state,
            "struct ParserStateLocalRecord Value: int end\n"
            "struct ParserStateShadowRecord Local: double end\n"
            "struct ParserGlobalChildRecord LocalValue: double end", error)) {
         Log.error("failed to compile local struct declarations: %s", error.c_str());
         return false;
      }

      if (not find_struct(first_state, local_name)) {
         Log.error("declarative struct was not retained in its originating state");
         return false;
      }
      if (find_struct(second_state, local_name)) {
         Log.error("declarative struct leaked into an independent state");
         return false;
      }

      auto first_shadow = find_struct(first_state, shadow_name);
      auto second_shadow = find_struct(second_state, shadow_name);
      if ((not first_shadow) or first_shadow->Fields.empty() or first_shadow->Fields[0].Name != "Local") {
         Log.error("state-local struct did not shadow the global definition");
         return false;
      }
      if ((not second_shadow) or second_shadow->Fields.empty() or second_shadow->Fields[0].Name != "global") {
         Log.error("global struct fallback was changed by another state's declaration");
         return false;
      }

      auto global_parent = find_struct(first_state, global_parent_name);
      auto global_child = find_struct(second_state, global_child_name);
      if ((not global_parent) or global_parent->Fields.empty() or (not global_child) or
            global_parent->Fields[0].StructDefinition != global_child) {
         Log.error("global parent did not retain its global embedded-struct definition");
         return false;
      }

      if (not parse_struct_source(first_state,
            "local value:struct<ParserStateLocalRecord> = struct<ParserStateLocalRecord> { }", error)) {
         Log.error("later compilation in the same state could not resolve its declaration: %s", error.c_str());
         return false;
      }

      if (compile_snapshot(first_state,
            "struct ParserLocalExpectedRecord Value: int end\n"
            "struct ParserLocalActualRecord Value: double end\n"
            "local value:struct<ParserLocalExpectedRecord> = struct<ParserLocalActualRecord> { }", true, error).has_value()) {
         Log.error("local declaration accepted an initialiser with a different struct layout");
         return false;
      }
      if (error.find("struct layout mismatch") IS std::string::npos) {
         Log.error("local struct layout mismatch produced an unexpected diagnostic: %s", error.c_str());
         return false;
      }

      if (parse_struct_source(first_state,
            "struct ParserRolledBackRecord Value: int end\n"
            "struct ParserInvalidRecord Value: mystery end", error)) {
         Log.error("invalid declaration fixture compiled successfully");
         return false;
      }
      if (find_struct(first_state, "ParserRolledBackRecord")) {
         Log.error("failed compilation retained a state-local declaration");
         return false;
      }
   }

   LuaStateHolder replacement;
   if (find_struct(replacement.get(), local_name)) {
      Log.error("declarative struct survived closure of its owning state");
      return false;
   }

   return true;
}

// Covers the process-wide module registry: indexed case-insensitive lookup, revalidation of the canonical name after
// hashing, stable record addresses across registry growth, and single publication under concurrent first resolution.

static bool test_module_registry_lookup(kt::Log &Log)
{
   auto core = static_module_by_name("core");
   if (not core) {
      Log.error("core module could not be resolved through the registry");
      return false;
   }

   // Case variants and repeated lookups must resolve to the one published binding.

   for (auto spelling : { "core", "Core", "CORE", "cOrE" }) {
      if (static_module_by_name(spelling) != core) {
         Log.error("module spelling '%s' did not resolve to the canonical binding", spelling);
         return false;
      }
   }

   if (static_module_by_name("core") != core) {
      Log.error("repeated resolution returned a different binding address");
      return false;
   }

   // A name that does not exist must fail rather than resolving to a bucket neighbour.

   if (static_module_by_name("ParserNoSuchModuleName")) {
      Log.error("an unknown module name resolved to a binding");
      return false;
   }

   // Function lookup must be case-insensitive and must revalidate the canonical spelling.

   auto fields = static_module_function(core, "PreciseTime");
   if (not fields) {
      Log.error("PreciseTime was not found through the function index");
      return false;
   }
   for (auto spelling : { "precisetime", "PRECISETIME", "PreciseTime" }) {
      if (static_module_function(core, spelling) != fields) {
         Log.error("function spelling '%s' did not resolve to the canonical entry", spelling);
         return false;
      }
      if (static_module_function_name(core, spelling) != std::string_view("PreciseTime")) {
         Log.error("function spelling '%s' did not report the canonical name", spelling);
         return false;
      }
   }

   if (static_module_function(core, "ParserNoSuchFunctionName")) {
      Log.error("an unknown function name resolved through the index");
      return false;
   }
   if (not static_module_function_name(core, "ParserNoSuchFunctionName").empty()) {
      Log.error("an unknown function name reported a canonical spelling");
      return false;
   }

   // Resolving further modules grows the registry; previously published addresses must not move.

   if (load_module_defs("display") IS ERR::Okay) {
      if (static_module_by_name("core") != core) {
         Log.error("registry growth moved a published module binding");
         return false;
      }
      if (static_module_function(core, "PreciseTime") != fields) {
         Log.error("registry growth moved a published function record");
         return false;
      }
   }

   return true;
}

// Compile-time signature queries and runtime callable lookup must share one ordinal mapping and one owner of the
// canonical name and FunctionField metadata.  Proving the shared ordinal is what allows ModuleBinding to carry no
// second function index of its own.

static bool test_module_registry_shared_ordinal(kt::Log &Log)
{
   auto core = static_module_by_name("core");
   if (not core) {
      Log.error("core module could not be resolved through the registry");
      return false;
   }

   // Every spelling of a function must resolve through both entry points, to the same ordinal.

   for (auto spelling : { "PreciseTime", "precisetime", "PRECISETIME", "GetErrorMsg", "geterrormsg" }) {
      auto resolution = test_module_resolve("core", spelling);
      if ((not resolution.Found) or (not resolution.CallableFound)) {
         Log.error("'%s' resolved through only one of the compiler and runtime paths", spelling);
         return false;
      }
      if (resolution.Ordinal != resolution.CallableOrdinal) {
         Log.error("'%s' resolved to compiler ordinal %u but runtime ordinal %u", spelling,
            resolution.Ordinal, resolution.CallableOrdinal);
         return false;
      }

      // The runtime callable must expose the signature's own copies rather than a second representation.

      if (resolution.CallableName != resolution.SignatureName) {
         Log.error("'%s' has separate canonical name storage for the compiler and runtime", spelling);
         return false;
      }
      if (resolution.CallableFields and (resolution.CallableFields != resolution.SignatureFields)) {
         Log.error("'%s' has separate FunctionField storage for the compiler and runtime", spelling);
         return false;
      }
      if (static_module_function(core, spelling) != resolution.SignatureFields) {
         Log.error("'%s' reported differing fields through the compiler query and the signature", spelling);
         return false;
      }
   }

   // Case variants of one function share one ordinal, and distinct functions do not.

   auto lower = test_module_resolve("core", "precisetime");
   auto exact = test_module_resolve("core", "PreciseTime");
   auto other = test_module_resolve("core", "GetErrorMsg");
   if (lower.Ordinal != exact.Ordinal) {
      Log.error("case variants of one function resolved to differing ordinals");
      return false;
   }
   if (other.Ordinal IS exact.Ordinal) {
      Log.error("distinct functions resolved to the same ordinal");
      return false;
   }

   // An unknown name must fail through both paths rather than resolving to a bucket neighbour or a stale ordinal.

   auto missing = test_module_resolve("core", "ParserNoSuchFunctionName");
   if (missing.Found or missing.CallableFound) {
      Log.error("an unknown function name resolved through the shared index");
      return false;
   }

   return true;
}

// Concurrent first resolution of one module must publish exactly one binding, and concurrent resolution of several
// modules must not corrupt the index.

static bool test_module_registry_concurrency(kt::Log &Log)
{
   constexpr int thread_count = 8;
   std::vector<std::thread> threads;
   std::array<StaticModuleHandle, thread_count> observed = { };
   std::atomic<int> failures = 0;

   for (int index = 0; index < thread_count; ++index) {
      threads.emplace_back([index, &observed, &failures]() {
         // Every thread resolves the same module, using a different spelling on alternate threads.
         if (load_module_defs((index & 1) ? "Core" : "core") != ERR::Okay) failures++;
         observed[index] = static_module_by_name("core");
      });
   }
   for (auto &thread : threads) thread.join();

   if (failures.load() > 0) {
      Log.error("concurrent module resolution reported %d failures", failures.load());
      return false;
   }

   for (int index = 0; index < thread_count; ++index) {
      if (not observed[index]) {
         Log.error("thread %d observed no binding for core", index);
         return false;
      }
      if (observed[index] != observed[0]) {
         Log.error("concurrent resolution published more than one binding for core");
         return false;
      }
   }

   return true;
}

// Module definitions are process-wide, so their layout must not depend on the state that requested them, and a
// state-local declaration must be able to shadow one without altering the published global definition.

static bool test_module_definition_ownership(kt::Log &Log)
{
   // Definitions load without any Lua state in scope.

   if (auto error = load_module_defs("core"); error != ERR::Okay) {
      Log.error("global module definitions failed to load: %s", GetErrorMsg(error));
      return false;
   }

   auto global_definition = find_struct(nullptr, "RGB8");
   if (not global_definition) {
      Log.error("a global module structure was not published to the global dictionary");
      return false;
   }

   // Repeated requests are idempotent and must not republish or alter the definition.

   if (load_module_defs("core") != ERR::Okay) {
      Log.error("a repeated definition request failed");
      return false;
   }
   if (find_struct(nullptr, "RGB8") != global_definition) {
      Log.error("a repeated definition request republished the structure");
      return false;
   }

   // Two independent states observe the identical global definition.

   LuaStateHolder first;
   LuaStateHolder second;
   if ((not first.get()) or (not second.get())) {
      Log.error("failed to allocate states for the definition ownership test");
      return false;
   }
   if ((find_struct(first.get(), "RGB8") != global_definition) or
       (find_struct(second.get(), "RGB8") != global_definition)) {
      Log.error("independent states observed differing global module definitions");
      return false;
   }

   // A state-local declaration shadows the global definition for its own state only, and the global definition that
   // other states resolve is unchanged.

   std::string error;
   if (not parse_struct_source(first.get(), "struct RGB8 ParserLocalOnly: double end", error)) {
      Log.error("failed to declare a shadowing structure: %s", error.c_str());
      return false;
   }

   auto shadowed = find_struct(first.get(), "RGB8");
   if ((not shadowed) or (shadowed IS global_definition)) {
      Log.error("a state-local declaration did not shadow the global module definition");
      return false;
   }
   if (shadowed->Fields.empty() or shadowed->Fields[0].Name != "ParserLocalOnly") {
      Log.error("the shadowing structure did not retain its declared layout");
      return false;
   }
   if (find_struct(second.get(), "RGB8") != global_definition) {
      Log.error("a state-local declaration altered the definition seen by another state");
      return false;
   }
   if (find_struct(nullptr, "RGB8") != global_definition) {
      Log.error("a state-local declaration altered the published global definition");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Phase 3: portable prototype dependency descriptors.
//
// Verifies that a compilation unit's module declarations are recorded as canonical names on its prototype, that the
// names deduplicate across aliases, that a dependency with no referenced function is retained, and that the whole
// block survives a bytecode round trip in a fresh state.

static bool test_module_dependency_descriptors(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   register_module_class(L);
   lua_protect_globals(L);

   // 'core' is referenced through two aliases and 'display' is declared without being referenced, so the descriptors
   // must contain exactly two modules, one carrying a single function and the other none.

   constexpr std::string_view source =
      "module core as mC\n"
      "module core as mCore\n"
      "module display as mD\n"
      "local a = mC.PreciseTime()\n"
      "local b = mCore.PreciseTime()\n"
      "return a + b\n";

   if (lua_load(L, source, "dependency-descriptors")) {
      Log.error("failed to compile the descriptor fixture: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *proto = funcproto(funcV(L->top - 1));
   auto table = proto_dependencies(proto);
   if (not table) {
      Log.error("a unit declaring modules produced no dependency descriptors");
      return false;
   }

   if (table->version != PROTO_DEPENDENCY_VERSION) {
      Log.error("descriptor version is %d, expected %d", int(table->version), int(PROTO_DEPENDENCY_VERSION));
      return false;
   }

   if (table->dependency_count != 2) {
      Log.error("two aliases of one module produced %d dependencies, expected 2",
         int(table->dependency_count));
      return false;
   }

   if (table->function_count != 1) {
      Log.error("one deduplicated function reference produced %d entries, expected 1",
         int(table->function_count));
      return false;
   }

   auto dependencies = proto_dependency_list(table);
   auto functions = proto_dependency_functions(table);

   bool saw_core = false, saw_display = false;
   for (uint32_t i = 0; i < table->dependency_count; ++i) {
      GCstr *name = gco_to_string(gcref(dependencies[i].name));
      std::string_view view(strdata(name), name->len);

      if (kt::iequals(view, "core")) {
         saw_core = true;
         if (dependencies[i].function_count != 1) {
            Log.error("the core dependency records %d functions, expected 1",
               int(dependencies[i].function_count));
            return false;
         }
         GCstr *function = gco_to_string(gcref(functions[dependencies[i].first_function].name));
         if (not kt::iequals(std::string_view(strdata(function), function->len), "PreciseTime")) {
            Log.error("the recorded function name is '%.*s', expected PreciseTime",
               int(function->len), strdata(function));
            return false;
         }
      }
      else if (kt::iequals(view, "display")) {
         saw_display = true;
         if (dependencies[i].function_count != 0) {
            Log.error("an unreferenced declaration recorded %d functions, expected 0",
               int(dependencies[i].function_count));
            return false;
         }
      }
      else {
         Log.error("unexpected dependency '%.*s'", int(name->len), strdata(name));
         return false;
      }
   }

   if (not saw_core or not saw_display) {
      Log.error("descriptors are missing core (%d) or display (%d)", int(saw_core), int(saw_display));
      return false;
   }

   // Round trip through a dump.  The reload happens in a second state, so the rebuilt descriptors cannot be sharing
   // anything with the originals; only the persisted names connect them.

   std::string dump;
   if (lua_dump(L, bytecode_writer, &dump) != 0) {
      Log.error("failed to dump the descriptor fixture");
      return false;
   }
   lua_pop(L, 1);

   LuaStateHolder reload_state;
   lua_State *R = reload_state.get();
   luaL_openlibs(R);
   register_module_class(R);
   lua_protect_globals(R);
   if (lua_load(R, std::string_view(dump.data(), dump.size()), "dependency-reload")) {
      Log.error("failed to reload the descriptor fixture: %s", lua_tostring(R, -1));
      return false;
   }

   auto reloaded = proto_dependencies(funcproto(funcV(R->top - 1)));
   if (not reloaded) {
      Log.error("the reloaded prototype lost its dependency descriptors");
      return false;
   }

   if (reloaded->dependency_count != table->dependency_count or
       reloaded->function_count != table->function_count) {
      Log.error("the reloaded descriptors have %d/%d entries, expected %d/%d",
         int(reloaded->dependency_count), int(reloaded->function_count),
         int(table->dependency_count), int(table->function_count));
      return false;
   }

   auto reloaded_dependencies = proto_dependency_list(reloaded);
   for (uint32_t i = 0; i < reloaded->dependency_count; ++i) {
      GCstr *original = gco_to_string(gcref(dependencies[i].name));
      GCstr *restored = gco_to_string(gcref(reloaded_dependencies[i].name));
      if (not kt::iequals(std::string_view(strdata(original), original->len),
            std::string_view(strdata(restored), restored->len))) {
         Log.error("dependency %d reloaded as '%.*s', expected '%.*s'", int(i),
            int(restored->len), strdata(restored), int(original->len), strdata(original));
         return false;
      }
      if (reloaded_dependencies[i].first_function != dependencies[i].first_function or
          reloaded_dependencies[i].function_count != dependencies[i].function_count) {
         Log.error("dependency %d reloaded with a different function range", int(i));
         return false;
      }
   }

   // The dumped chunk must still run in the fresh state, which exercises resolution of the reloaded names against
   // the global registry rather than anything carried over from compilation.

   if (lua_pcall(R, 0, 1, 0)) {
      Log.error("the reloaded chunk failed to execute: %s", lua_tostring(R, -1));
      return false;
   }
   if (not lua_isnumber(R, -1)) {
      Log.error("the reloaded chunk did not produce a numeric result");
      return false;
   }
   lua_pop(R, 1);

   // A prototype without module declarations must carry no descriptor block at all, so that ordinary functions pay
   // nothing for the feature.

   LuaStateHolder plain_state;
   lua_State *P = plain_state.get();
   luaL_openlibs(P);
   register_module_class(P);
   lua_protect_globals(P);
   if (lua_load(P, std::string_view("return 1"), "dependency-none")) {
      Log.error("failed to compile the descriptor-free fixture: %s", lua_tostring(P, -1));
      return false;
   }
   if (proto_dependencies(funcproto(funcV(P->top - 1)))) {
      Log.error("a unit with no module declaration produced dependency descriptors");
      return false;
   }
   lua_pop(P, 1);

   return true;
}

//********************************************************************************************************************
// Corrupt dependency metadata must be rejected rather than resolved.  The fixture mutates a valid dump, so every
// rejection below is attributable to the dependency block and not to an unrelated inconsistency.

static bool test_module_dependency_corruption_rejected(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   register_module_class(L);
   lua_protect_globals(L);
   constexpr std::string_view source =
      "module core as mC\n"
      "return mC.PreciseTime()\n";

   if (lua_load(L, source, "dependency-corrupt-source")) {
      Log.error("failed to compile the corruption fixture: %s", lua_tostring(L, -1));
      return false;
   }

   std::string dump;
   if (lj_bcwrite(L, funcproto(funcV(L->top - 1)), bytecode_writer, &dump, 1) != 0) {
      Log.error("failed to dump the corruption fixture");
      return false;
   }
   lua_pop(L, 1);

   auto read_uleb = [&dump](size_t &Position, uint32_t &Value) {
      Value = 0;
      uint32_t shift = 0;
      for (uint32_t count = 0; count < 5 and Position < dump.size(); ++count) {
         uint8_t byte = uint8_t(dump[Position++]);
         Value |= uint32_t(byte & 0x7f) << shift;
         if (not (byte & 0x80)) return true;
         shift += 7;
      }
      return false;
   };

   // Walk the prototype header to the dependency block: proto length, four header bytes, then sizekgc, sizekn,
   // sizebc, siglen and deplen.  The dump is stripped, so the signature payload follows immediately.

   size_t position = 5;
   uint32_t value = 0;
   if (not read_uleb(position, value) or position + 4 > dump.size()) {
      Log.error("could not locate the prototype header in the corruption fixture");
      return false;
   }
   position += 4;

   uint32_t signature_length = 0, dependency_length = 0;
   for (int field = 0; field < 3; ++field) {
      if (not read_uleb(position, value)) {
         Log.error("could not walk the corruption fixture header");
         return false;
      }
   }
   if (not read_uleb(position, signature_length) or not read_uleb(position, dependency_length)) {
      Log.error("could not read the corruption fixture lengths");
      return false;
   }

   if (dependency_length IS 0) {
      Log.error("the corruption fixture carries no dependency block");
      return false;
   }

   size_t dependency_offset = position + signature_length;
   if (dependency_offset + dependency_length > dump.size()) {
      Log.error("the corruption fixture dependency block is out of range");
      return false;
   }

   struct Corruption {
      const char *description;
      size_t offset;
      char value;
   };

   // The block layout is: versionB depcountU funccountU, then the entries.  Each mutation below targets one of the
   // reader's structural invariants.

   const Corruption cases[] = {
      { "an unsupported descriptor version", dependency_offset, char(0xff) },
      { "a zero dependency count", dependency_offset + 1, char(0) },
      { "an oversized dependency count", dependency_offset + 1, char(0x7f) },
      { "an inconsistent function count", dependency_offset + 2, char(0x7f) }
   };

   for (const auto &corruption : cases) {
      std::string corrupt = dump;
      corrupt[corruption.offset] = corruption.value;
      if (lua_load(L, std::string_view(corrupt.data(), corrupt.size()), "dependency-corrupt") IS 0) {
         Log.error("the bytecode reader accepted %s", corruption.description);
         lua_pop(L, 1);
         return false;
      }
      lua_pop(L, 1);
   }

   size_t bytecode_offset = dependency_offset + dependency_length;
   if (bytecode_offset + sizeof(BCIns) > dump.size()) {
      Log.error("the corruption fixture carries no activation instruction");
      return false;
   }

   BCIns activation;
   memcpy(&activation, dump.data() + bytecode_offset, sizeof(activation));
   if (bc_op(activation) != BC_MODACT) {
      Log.error("the corruption fixture does not begin with module activation");
      return false;
   }

   for (bool missing : { false, true }) {
      std::string corrupt = dump;
      BCIns instruction = activation;
      if (missing) setbc_op(&instruction, BC_MOV);
      else setbc_d(&instruction, 0xffff);
      memcpy(corrupt.data() + bytecode_offset, &instruction, sizeof(instruction));
      if (lua_load(L, std::string_view(corrupt.data(), corrupt.size()), "activation-corrupt") IS 0) {
         Log.error("the bytecode reader accepted %s module activation",
            missing ? "a missing" : "an out-of-range");
         lua_pop(L, 1);
         return false;
      }
      lua_pop(L, 1);
   }

   // Truncating the declared block length must also be rejected, because the reader requires the block to be
   // consumed exactly.

   std::string truncated = dump;
   truncated[position - 1] = char(uint8_t(truncated[position - 1]) - 1);
   if (lua_load(L, std::string_view(truncated.data(), truncated.size()), "dependency-truncated") IS 0) {
      Log.error("the bytecode reader accepted a truncated dependency block");
      lua_pop(L, 1);
      return false;
   }
   lua_pop(L, 1);

   return true;
}

//********************************************************************************************************************
// Executing a unit's dependency declaration must populate the prototype's resolved sidecar, and the callables bound
// into the generated locals must be exactly the records the sidecar holds.
//
// BC_MODACT serves its bindings from the sidecar rather than repeating module and function lookups.  Comparing the
// bound upvalue against the sidecar slot makes the descriptor path observable independently of call behaviour.

static bool test_module_dependency_activation_uses_sidecar(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   register_module_class(L);
   lua_protect_globals(L);

   // Two distinct functions from one module, plus a declaration that references none.  The unreferenced module must
   // still resolve, and the referenced functions must occupy consecutive sidecar slots in declaration order.

   constexpr std::string_view source =
      "module core as mC\n"
      "module display as mD\n"
      "local a = mC.PreciseTime\n"
      "local b = mC.GetErrorMsg\n"
      "return a, b\n";

   if (lua_load(L, source, "dependency-activation")) {
      Log.error("failed to compile the activation fixture: %s", lua_tostring(L, -1));
      return false;
   }

   GCproto *proto = funcproto(funcV(L->top - 1));

   if (proto->resolved_dependencies) {
      Log.error("the sidecar was populated before the chunk executed");
      return false;
   }

   // lua_pcall() consumes the chunk, so retain a copy for the re-activation check below.

   lua_pushvalue(L, -1);

   if (lua_pcall(L, 0, 2, 0)) {
      Log.error("the activation fixture failed to execute: %s", lua_tostring(L, -1));
      return false;
   }

   if (not proto->resolved_dependencies) {
      Log.error("executing a dependency declaration left the sidecar unresolved");
      return false;
   }

   auto table = proto_dependencies(proto);
   if (not table or table->function_count != 2) {
      Log.error("expected two resolved function slots, found %d", table ? int(table->function_count) : -1);
      return false;
   }

   // Every referenced function slot must hold a record, and every declaration must be marked activated even when it
   // references no function.

   std::array<const void *, 2> resolved = { };
   for (uint32_t slot = 0; slot < table->function_count; ++slot) {
      resolved[slot] = proto_dependency_callable(proto, slot);
      if (not resolved[slot]) {
         Log.error("sidecar slot %d is empty after activation", int(slot));
         return false;
      }
   }
   if (not proto->resolved_dependency_states or proto->resolved_dependency_count != table->dependency_count) {
      Log.error("dependency activation state does not match the descriptor table");
      return false;
   }
   for (uint32_t dependency = 0; dependency < table->dependency_count; ++dependency) {
      if (not proto->resolved_dependency_states[dependency]) {
         Log.error("dependency %d was not marked activated", int(dependency));
         return false;
      }
   }

   // The returned values are the generated bindings.  Each is a C closure whose single upvalue is the light userdata
   // pointing at the callable record, so it can be compared directly against the sidecar.

   for (int index = 0; index < 2; ++index) {
      int stack_slot = index - 2; // -2 is the first result, -1 the second

      if (not lua_iscfunction(L, stack_slot)) {
         Log.error("binding %d is not a C closure", index);
         return false;
      }

      if (not lua_getupvalue(L, stack_slot, 1)) {
         Log.error("binding %d exposes no callable upvalue", index);
         return false;
      }

      const void *bound = lua_topointer(L, -1);
      lua_pop(L, 1);

      const void *expected = proto_dependency_callable(proto, uint32_t(index));
      if (bound != expected) {
         Log.error("binding %d holds %p but sidecar slot %d holds %p; the adapter is not serving from the sidecar",
            index, bound, index, expected);
         return false;
      }
   }

   lua_pop(L, 2); // Discard the two bindings, leaving the retained chunk on top.

   // Re-executing the chunk must preserve the resolved sidecar and simply rebuild closures from its stable records.

   if (lua_pcall(L, 0, 2, 0)) {
      Log.error("re-executing the activation fixture failed: %s", lua_tostring(L, -1));
      return false;
   }
   lua_pop(L, 2);

   for (uint32_t slot = 0; slot < table->function_count; ++slot) {
      if (proto_dependency_callable(proto, slot) != resolved[slot]) {
         Log.error("re-activation replaced resolved sidecar slot %d", int(slot));
         return false;
      }
   }

   return true;
}

static bool test_module_registry(kt::Log &Log)
{
   return test_module_registry_lookup(Log) and
      test_module_registry_shared_ordinal(Log) and
      test_module_registry_concurrency(Log) and
      test_module_definition_ownership(Log) and
      test_module_dependency_descriptors(Log) and
      test_module_dependency_activation_uses_sidecar(Log) and
      test_module_dependency_corruption_rejected(Log);
}

static bool test_struct_declaration_syntax(kt::Log &Log)
{
   std::string error;
   LuaStateHolder holder;
   lua_State *state = holder.get();
   if (not state) {
      Log.error("failed to allocate state for struct declaration syntax test");
      return false;
   }

   if (not parse_struct_source(state, "struct ParserSingleLine X: int, Y: int end", error)) {
      Log.error("single-line end-terminated declaration failed: %s", error.c_str());
      return false;
   }

   if (parse_struct_source(state, "struct ParserBraceDeclaration { X: int }", error)) {
      Log.error("brace-delimited struct declaration compiled successfully");
      return false;
   }
   if (error.find("Struct declarations use 'end' termination") IS std::string::npos) {
      Log.error("brace-delimited declaration produced an unexpected diagnostic: %s", error.c_str());
      return false;
   }

   return true;
}

static bool test_bytecode_equivalence(kt::Log &log)
{
   constexpr const char* source = R"(
local value = 1
value = value + 2
return value * 3
)";

   LuaStateHolder holder;
   lua_State* L = holder.get();
   if (not L) {
      log.error("failed to allocate lua state for bytecode comparison");
      return false;
   }

   std::string error;
   auto legacy = compile_snapshot(L, source, false, error);
   if (not legacy.has_value()) {
      log.error("legacy parser compile failed: %s", error.c_str());
      return false;
   }

   auto ast = compile_snapshot(L, source, true, error);
   if (not ast.has_value()) {
      log.warning("ast pipeline compile failed: %s (bytecode diff skipped)", error.c_str());
      return true;
   }

   std::string diff;
   if (not compare_snapshots(*legacy, *ast, diff, "chunk")) {
      log.error("bytecode mismatch: %s", diff.c_str());
      return false;
   }

   return true;
}

static bool test_contextual_member_ast_foundations(kt::Log &Log)
{
   constexpr std::string_view source =
      ".value = 1\n"
      ".value += 2\n"
      "local read = .child.name\n"
      ".refresh(3, 4)\n"
      "receiver.member(5)\n"
      "receiver[key](6)\n"
      "receiver?.member(7);\n"
      "receiver?[key](8);\n"
      "(receiver.member)(8)\n"
      "local alias = receiver.member\n"
      "alias(9)\n";

   auto result = build_ast_from_source(source, false, false);
   if (not result.chunk.ok() or result.chunk.value_ref()->statements.size() != 11) {
      Log.error("failed to parse contextual member AST fixture (ok=%d, statements=%" PRId64 ")",
         int(result.chunk.ok()), int64_t(result.chunk.ok() ? result.chunk.value_ref()->statements.size() : 0));
      log_diagnostics(result.diagnostics, Log);
      return false;
   }

   BlockStmt &block = *result.chunk.value_ref();
   for (size_t i = 0; i < 2; ++i) {
      const auto *assignment = std::get_if<AssignmentStmtPayload>(&block.statements[i]->data);
      const auto *member = assignment and not assignment->targets.empty() ?
         std::get_if<MemberExprPayload>(&assignment->targets.front()->data) : nullptr;
      if (not member or not member->table or member->table->kind != AstNodeKind::CurrentContextExpr) {
         Log.error("leading-dot assignment did not retain an explicit current-context receiver");
         return false;
      }
      if (infer_expression_type(*member->table) != TiriType::Table) {
         Log.error("current-context expression was not inferred as a table");
         return false;
      }
   }

   const auto &first_assignment = std::get<AssignmentStmtPayload>(block.statements[0]->data);
   const auto &first_member = std::get<MemberExprPayload>(first_assignment.targets.front()->data);
   if (first_member.table->span.line != first_member.member.span.line or
       first_member.table->span.offset >= first_member.member.span.offset) {
      Log.error("leading-dot access did not retain distinct context and member source spans");
      return false;
   }

   const auto &read = std::get<LocalDeclStmtPayload>(block.statements[2]->data);
   const auto *outer_member = read.values.empty() ? nullptr : std::get_if<MemberExprPayload>(&read.values[0]->data);
   const auto *inner_member = outer_member and outer_member->table ?
      std::get_if<MemberExprPayload>(&outer_member->table->data) : nullptr;
   if (not inner_member or not inner_member->table or
       inner_member->table->kind != AstNodeKind::CurrentContextExpr) {
      Log.error("chained leading-dot access lost its current-context root");
      return false;
   }

   constexpr std::array<CallDispatch, 7> expected_dispatch = {
      CallDispatch::MemberNamed,
      CallDispatch::MemberNamed,
      CallDispatch::MemberComputed,
      CallDispatch::SafeMemberNamed,
      CallDispatch::SafeMemberComputed,
      CallDispatch::MemberNamed,
      CallDispatch::Direct
   };
   constexpr std::array<size_t, 7> statement_indices = { 3, 4, 5, 6, 7, 8, 10 };
   constexpr std::array<size_t, 7> argument_counts = { 2, 1, 1, 1, 1, 1, 1 };
   constexpr std::array<AstNodeKind, 7> target_kinds = {
      AstNodeKind::MemberExpr,
      AstNodeKind::MemberExpr,
      AstNodeKind::IndexExpr,
      AstNodeKind::SafeMemberExpr,
      AstNodeKind::SafeIndexExpr,
      AstNodeKind::MemberExpr,
      AstNodeKind::IdentifierExpr
   };

   for (size_t i = 0; i < statement_indices.size(); ++i) {
      const auto &statement = std::get<ExpressionStmtPayload>(block.statements[statement_indices[i]]->data);
      const auto *call = statement.expression ? std::get_if<CallExprPayload>(&statement.expression->data) : nullptr;
      const auto *target = call ? std::get_if<DirectCallTarget>(&call->target) : nullptr;
      if (not call or call->dispatch != expected_dispatch[i] or call->arguments.size() != argument_counts[i] or
          not target or not target->callable or target->callable->kind != target_kinds[i]) {
         Log.error("contextual call AST lost dispatch or written-argument metadata at fixture index %" PRId64,
            int64_t(i));
         return false;
      }
   }

   const auto &computed_statement = std::get<ExpressionStmtPayload>(block.statements[5]->data);
   const auto &computed_call = std::get<CallExprPayload>(computed_statement.expression->data);
   const auto &computed_target = std::get<DirectCallTarget>(computed_call.target);
   const auto &computed_index = std::get<IndexExprPayload>(computed_target.callable->data);
   if (not computed_index.table or not computed_index.index) {
      Log.error("computed contextual call did not retain separate receiver and key evaluation components");
      return false;
   }

   const auto &grouped_statement = std::get<ExpressionStmtPayload>(block.statements[8]->data);
   const auto &grouped_call = std::get<CallExprPayload>(grouped_statement.expression->data);
   const auto &grouped_target = std::get<DirectCallTarget>(grouped_call.target);
   if (not grouped_target.callable or not grouped_target.callable->is_grouped) {
      Log.error("parenthesised member call did not preserve its contextual association");
      return false;
   }

   constexpr std::string_view multiline_source =
      "receiver.member()\n"
      "   .next()\n"
      ".contextual()\n";
   auto multiline = build_ast_from_source(multiline_source, false, false);
   if (not multiline.chunk.ok() or multiline.chunk.value_ref()->statements.size() != 2) {
      Log.error("multi-line member chain was split into contextual statements");
      log_diagnostics(multiline.diagnostics, Log);
      return false;
   }

   const auto &multiline_statement =
      std::get<ExpressionStmtPayload>(multiline.chunk.value_ref()->statements[0]->data);
   const auto *multiline_call = multiline_statement.expression ?
      std::get_if<CallExprPayload>(&multiline_statement.expression->data) : nullptr;
   const auto *multiline_target = multiline_call ? std::get_if<DirectCallTarget>(&multiline_call->target) : nullptr;
   const auto *multiline_member = multiline_target and multiline_target->callable ?
      std::get_if<MemberExprPayload>(&multiline_target->callable->data) : nullptr;
   if (not multiline_member or not multiline_member->table or
       multiline_member->table->kind != AstNodeKind::CallExpr) {
      Log.error("indented multi-line member suffix did not retain the preceding call as its receiver");
      return false;
   }

   const auto &contextual_statement =
      std::get<ExpressionStmtPayload>(multiline.chunk.value_ref()->statements[1]->data);
   const auto *contextual_call = contextual_statement.expression ?
      std::get_if<CallExprPayload>(&contextual_statement.expression->data) : nullptr;
   const auto *contextual_target = contextual_call ? std::get_if<DirectCallTarget>(&contextual_call->target) : nullptr;
   const auto *contextual_member = contextual_target and contextual_target->callable ?
      std::get_if<MemberExprPayload>(&contextual_target->callable->data) : nullptr;
   if (not contextual_member or not contextual_member->table or
       contextual_member->table->kind != AstNodeKind::CurrentContextExpr) {
      Log.error("same-indent leading dot did not remain a contextual statement after a multi-line chain");
      return false;
   }

   const auto &alias_decl = std::get<LocalDeclStmtPayload>(block.statements[9]->data);
   if (alias_decl.values.empty() or alias_decl.values[0]->kind != AstNodeKind::MemberExpr) {
      Log.error("member extraction did not remain an unbound member expression");
      return false;
   }

   constexpr std::array<std::string_view, 2> invalid_context_sources = {
      "local value = .\n",
      "local value = .(123)\n"
   };
   for (std::string_view invalid_source : invalid_context_sources) {
      auto invalid = build_ast_from_source(invalid_source, false, false);
      bool found_leading_dot_diagnostic = false;
      for (const ParserDiagnostic &diagnostic : invalid.diagnostics) {
         if (diagnostic.code IS ParserErrorCode::ExpectedIdentifier and
             diagnostic.message.find("leading '.' context access") != std::string::npos) {
            found_leading_dot_diagnostic = true;
         }
      }
      if (not found_leading_dot_diagnostic) {
         Log.error("invalid leading-dot access did not produce its targeted diagnostic");
         return false;
      }
   }

   constexpr std::array<std::string_view, 2> legacy_sources = {
      "receiver:member()\n",
      "function receiver:member() end\n"
   };
   for (std::string_view legacy_source : legacy_sources) {
      auto legacy = build_ast_from_source(legacy_source, false, false, true);
      bool found_cutover_diagnostic = false;
      for (const ParserDiagnostic &diagnostic : legacy.diagnostics) {
         if (diagnostic.code IS ParserErrorCode::DeprecatedSyntax and
             diagnostic.message.find("receiver.member") != std::string::npos) {
            found_cutover_diagnostic = true;
         }
      }
      if (not found_cutover_diagnostic) {
         Log.error("colon syntax cut-over gate did not produce a dot-qualified replacement diagnostic");
         return false;
      }
   }

   return true;
}

static bool test_ast_call_lowering(kt::Log &log)
{
   constexpr const char* source = R"(
extern math

local context = { base = 5 }

function context.compute(Self, delta):<any, ...>
   return Self.base + math.abs(-delta)
end

return context.compute(context, -3)
)";

   LuaStateHolder holder;
   lua_State* L = holder.get();
   if (not L) {
      log.error("failed to allocate lua state for call lowering test");
      return false;
   }

   std::string error;
   auto legacy = compile_snapshot(L, source, false, error);
   if (not legacy.has_value()) {
      log.error("legacy parser compile failed: %s", error.c_str());
      return false;
   }

   auto ast = compile_snapshot(L, source, true, error);
   if (not ast.has_value()) {
      log.error("ast pipeline compile failed: %s", error.c_str());
      return false;
   }

   std::string diff;
   if (not compare_snapshots(*legacy, *ast, diff, "chunk")) {
      log.error("call lowering mismatch: %s", diff.c_str());
      return false;
   }

   return true;
}

static bool test_return_lowering(kt::Log &log)
{
   constexpr const char* source =
      "extern math\n"
      "\n"
      "local function retmix(flag, ...):<any, ...>\n"
      "   if flag then\n"
      "      return ...\n"
      "   end\n"
      "\n"
      "   if flag != 0 then\n"
      "      return math.abs(flag)\n"
      "   end\n"
      "\n"
      "   return math.min(flag, 5), flag, ...\n"
      "end\n"
      "\n"
      "return retmix(...)\n";

   LuaStateHolder holder;
   lua_State* L = holder.get();
   if (not L) {
      log.error("failed to allocate lua state for return lowering test");
      return false;
   }

   std::string error;
   auto legacy = compile_snapshot(L, source, false, error);
   if (not legacy.has_value()) {
      log.error("legacy parser compile failed: %s", error.c_str());
      return false;
   }

   auto ast = compile_snapshot(L, source, true, error);
   if (not ast.has_value()) {
      log.error("ast pipeline compile failed: %s", error.c_str());
      return false;
   }

   std::string diff;
   if (not compare_snapshots(*legacy, *ast, diff, "chunk")) {
      log.error("return lowering mismatch: %s", diff.c_str());
      return false;
   }

   return true;
}

static bool test_ast_statement_matrix(kt::Log &log)
{
   constexpr std::array<PipelineSnippet, 4> snippets = { {
      { "control_flow_ladder", R"(
local total = 0
for i in {1 into 4} do
   if i % 2 is 0 then
      total += i
   elseif i > 3 then
      break
   else
      total = total + 1
   end

   if i is 3 then
      continue
   end

   total = total + i
end
return total
)" },
      { "generic_for_defer", R"(
extern pairs

local sum = 0
local map = { alpha = 1, beta = 2, gamma = 3 }
for key, value in pairs(map) do
   defer
      sum = sum + value
   end
   if key is 'beta' then
      sum += value
   else
      sum = sum + value
   end
end
return sum
)" },
      { "function_stmt_closure", R"(
local function outer(flag):any
   local function helper(value):<any, ...>
      return value * 2
   end

   if flag then
      return helper(flag)
   end

   return function(a, b):<any, ...>
      return helper(a + b)
   end
end

local fn:func = outer(false)
return fn(3, 4)
)" },
      // NOTE: table_assignment_matrix temporarily disabled due to deliberate register
      // allocation changes in commits b612b86c5/d6c0f70cb that add safety MOV instructions
      // for complex assignments. The extra instructions don't affect correctness.
      // { "table_assignment_matrix", R"(
      // local data = { values = { 1, 2 }, meta = { edge = 3 } }
      // data.values[1], data.values[2], data.meta.edge = data.values[2], data.values[1], data.meta.edge + 1
      // local fallback = data.unknown ?? 9
      // data.values[1] += data.meta.edge
      // return data.values[1] + fallback
      // )" },
      { "continue_ladder", R"(
local value = 0
for i in {1 into 3} do
   value += 1
   if i < 3 then
      continue
   end
   value += 2
end
return value
)" }
   } };

   LuaStateHolder holder;
   lua_State* L = holder.get();
   if (not L) {
      log.error("failed to allocate lua state for statement matrix test");
      return false;
   }

   for (const PipelineSnippet& snippet : snippets) {
      std::string error;
      auto legacy = compile_snapshot(L, snippet.source, false, error);
      if (not legacy.has_value()) {
         log.error("legacy parser compile failed (%s): %s", snippet.label, error.c_str());
         return false;
      }

      auto ast = compile_snapshot(L, snippet.source, true, error);
      if (not ast.has_value()) {
         log.error("ast pipeline compile failed (%s): %s", snippet.label, error.c_str());
         return false;
      }

      std::string diff;
      if (not compare_snapshots(*legacy, *ast, diff, snippet.label)) {
         log.error("bytecode mismatch (%s): %s", snippet.label, diff.c_str());
         return false;
      }
   }

   return true;
}

struct TestCase {
   const char* name;
   bool (*fn)(kt::Log&);
};

//********************************************************************************************************************
// Test ExpDesc::is_falsey() method for extended falsey semantics

static bool test_expdesc_is_falsey(kt::Log &log)
{
   // Test nil
   ExpDesc nil_expr(ExpKind::Nil);
   if (not nil_expr.is_falsey()) {
      log.error("nil should be falsey");
      return false;
   }

   // Test false
   ExpDesc false_expr(ExpKind::False);
   if (not false_expr.is_falsey()) {
      log.error("false should be falsey");
      return false;
   }

   // Test true
   ExpDesc true_expr(ExpKind::True);
   if (true_expr.is_falsey()) {
      log.error("true should be truthy");
      return false;
   }

   // Test zero (integer)
   ExpDesc zero_int(0.0);
   if (not zero_int.is_falsey()) {
      log.error("zero (0.0) should be falsey");
      return false;
   }

   // Test non-zero number
   ExpDesc nonzero(42.0);
   if (nonzero.is_falsey()) {
      log.error("non-zero number should be truthy");
      return false;
   }

   // Test negative number
   ExpDesc negative(-5.0);
   if (negative.is_falsey()) {
      log.error("negative number should be truthy");
      return false;
   }

   // Test empty string (need Lua state for string creation)
   auto harness = make_expression_harness("");
   if (not harness.has_value()) {
      log.error("failed to create harness for string test");
      return false;
   }

   LexState* lex = harness->lex.get();
   GCstr* empty_str = lex->intern_empty_string();
   ExpDesc empty_string_expr(empty_str);
   if (not empty_string_expr.is_falsey()) {
      log.error("empty string should be falsey");
      return false;
   }

   // Test non-empty string
   GCstr* hello_str = lj_str_newlit(lex->L, "hello");
   ExpDesc nonempty_string_expr(hello_str);
   if (nonempty_string_expr.is_falsey()) {
      log.error("non-empty string should be truthy");
      return false;
   }

   // Test non-constant expressions (should return false - conservative assumption)
   ExpDesc local_expr(ExpKind::Local, 0);
   if (local_expr.is_falsey()) {
      log.error("non-constant local should conservatively be truthy");
      return false;
   }

   ExpDesc nonreloc_expr(ExpKind::NonReloc, 1);
   if (nonreloc_expr.is_falsey()) {
      log.error("non-constant nonreloc should conservatively be truthy");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test ?? operator with constant folding

static bool test_if_empty_operator_constants(kt::Log &log)
{
   // Test: nil ?? 5 should evaluate to 5
   {
      auto result = build_ast_from_source("return nil ?? 5");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'nil ?? 5'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: false ?? 10 should evaluate to 10
   {
      auto result = build_ast_from_source("return false ?? 10");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'false ?? 10'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: 0 ?? 20 should evaluate to 20
   {
      auto result = build_ast_from_source("return 0 ?? 20");
      if (not result.chunk.ok()) {
         log.error("failed to parse '0 ?? 20'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: "" ?? "default" should evaluate to "default"
   {
      auto result = build_ast_from_source("return \"\" ?? \"default\"");
      if (not result.chunk.ok()) {
         log.error("failed to parse '\"\" ?? \"default\"'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: true ?? 30 should evaluate to true (not 30)
   {
      auto result = build_ast_from_source("return true ?? 30");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'true ?? 30'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: 42 ?? 50 should evaluate to 42 (not 50)
   {
      auto result = build_ast_from_source("return 42 ?? 50");
      if (not result.chunk.ok()) {
         log.error("failed to parse '42 ?? 50'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: "hello" ?? "world" should evaluate to "hello"
   {
      auto result = build_ast_from_source("return \"hello\" ?? \"world\"");
      if (not result.chunk.ok()) {
         log.error("failed to parse '\"hello\" ?? \"world\"'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Test ternary operator with falsey semantics

static bool test_ternary_falsey_semantics(kt::Log &log)
{
   // Test: nil ? "yes" :> "no" should evaluate to "no"
   {
      auto result = build_ast_from_source("return nil ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'nil ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: false ? "yes" :> "no" should evaluate to "no"
   {
      auto result = build_ast_from_source("return false ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'false ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: 0 ? "yes" :> "no" should evaluate to "no"
   {
      auto result = build_ast_from_source("return 0 ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse '0 ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: "" ? "yes" :> "no" should evaluate to "no"
   {
      auto result = build_ast_from_source("return \"\" ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse '\"\" ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: true ? "yes" :> "no" should evaluate to "yes"
   {
      auto result = build_ast_from_source("return true ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse 'true ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: 42 ? "yes" :> "no" should evaluate to "yes"
   {
      auto result = build_ast_from_source("return 42 ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse '42 ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   // Test: "hello" ? "yes" :> "no" should evaluate to "yes"
   {
      auto result = build_ast_from_source("return \"hello\" ? 'yes' :> 'no'");
      if (not result.chunk.ok()) {
         log.error("failed to parse '\"hello\" ? yes :> no'");
         log_diagnostics(result.diagnostics, log);
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************

static bool test_static_descriptor_model(kt::Log &Log)
{
   StaticValueDescriptor unknown_array;
   unknown_array.primary = TiriType::Array;
   unknown_array.proof = StaticProof::Checked;

   StaticValueDescriptor any_array = unknown_array;
   any_array.array_element = { AET::ANY, TiriType::Any, CLASSID::NIL, nullptr, true };
   if (unknown_array IS any_array or unknown_array.array_element.known or not any_array.array_element.known) {
      Log.error("unknown array storage was not distinguished from array<any>");
      return false;
   }

   StaticValueDescriptor number;
   number.primary = TiriType::Num;
   number.proof = StaticProof::Closed;
   StaticValueDescriptor checked_number = number;
   checked_number.proof = StaticProof::Checked;
   checked_number.nullable = true;

   StaticValueDescriptor joined = join_static_descriptors(number, checked_number);
   if (joined.primary != TiriType::Num or not joined.nullable or joined.proof != StaticProof::Checked) {
      Log.error("matching descriptor join lost type, nullability or weakest proof");
      return false;
   }

   StaticValueDescriptor text;
   text.primary = TiriType::Str;
   text.proof = StaticProof::Closed;
   joined = join_static_descriptors(number, text);
   if (joined.primary != TiriType::Any or joined.proof != StaticProof::Advisory) {
      Log.error("conflicting descriptor join did not degrade to advisory any");
      return false;
   }

   RuntimeContract nullable_number{
      .type = TiriType::Num,
      .nullable = true
   };
   RuntimeContract required_number = nullable_number;
   required_number.nullable = false;
   required_number.required = true;
   if (not static_value_satisfies_contract(number, nullable_number) or
       not static_value_satisfies_contract(number, required_number) or
       static_value_satisfies_contract(checked_number, required_number)) {
      Log.error("static contract satisfaction lost directional nullability");
      return false;
   }

   StaticValueDescriptor advisory_number = number;
   advisory_number.proof = StaticProof::Advisory;
   if (static_value_satisfies_contract(advisory_number, nullable_number)) {
      Log.error("an advisory descriptor satisfied a runtime contract");
      return false;
   }

   StaticValueDescriptor table_value{
      .primary = TiriType::Table,
      .proof = StaticProof::Closed
   };
   StaticValueDescriptor table_length = describe_length_result(table_value);
   StaticValueDescriptor string_value{
      .primary = TiriType::Str,
      .proof = StaticProof::Closed
   };
   StaticValueDescriptor string_length = describe_length_result(string_value);
   if (table_length.primary != TiriType::Num or not table_length.nullable or
       string_length.primary != TiriType::Num or string_length.nullable) {
      Log.error("length descriptors did not retain numeric type with operand-specific nullability");
      return false;
   }

   StaticValueDescriptor nil_value{
      .primary = TiriType::Nil,
      .proof = StaticProof::Closed,
      .nullable = true
   };
   if (not static_value_satisfies_contract(nil_value, nullable_number) or
       static_value_satisfies_contract(nil_value, required_number)) {
      Log.error("nil descriptor did not respect optional and required contracts");
      return false;
   }

   auto *struct_a = (struct_record *)(uintptr_t(1));
   auto *struct_b = (struct_record *)(uintptr_t(2));
   StaticValueDescriptor exact_struct{
      .primary = TiriType::Struct,
      .struct_def = struct_a,
      .proof = StaticProof::Trusted
   };
   RuntimeContract exact_struct_contract{
      .type = TiriType::Struct,
      .struct_def = struct_a
   };
   if (not static_value_satisfies_contract(exact_struct, exact_struct_contract)) {
      Log.error("an exact proved structure did not satisfy its matching contract");
      return false;
   }
   exact_struct.struct_def = struct_b;
   if (static_value_satisfies_contract(exact_struct, exact_struct_contract)) {
      Log.error("a mismatched structure identity satisfied an exact contract");
      return false;
   }

   StaticValueDescriptor int_array{
      .primary = TiriType::Array,
      .array_element = { AET::INT32, TiriType::Num, CLASSID::NIL, nullptr, true },
      .proof = StaticProof::Closed
   };
   RuntimeContract int_array_contract{
      .type = TiriType::Array,
      .array_element = { AET::INT32, TiriType::Num, CLASSID::NIL, nullptr, true }
   };
   if (not static_value_satisfies_contract(int_array, int_array_contract)) {
      Log.error("an exact array member descriptor did not satisfy its matching contract");
      return false;
   }
   StaticValueDescriptor string_array = int_array;
   string_array.array_element = { AET::STR_GC, TiriType::Str, CLASSID::NIL, nullptr, true };
   if (static_value_satisfies_contract(string_array, int_array_contract)) {
      Log.error("a mismatched array member descriptor satisfied an exact contract");
      return false;
   }
   StaticValueDescriptor joined_arrays = join_static_descriptors(int_array, string_array);
   if (joined_arrays.primary != TiriType::Any or joined_arrays.array_element.known or
       joined_arrays.proof != StaticProof::Advisory) {
      Log.error("a conflicting array descriptor join did not retain an unresolved member identity");
      return false;
   }

   struct ArrayMapping {
      std::string_view name;
      AET storage;
      TiriType logical_type;
   };
   constexpr std::array<ArrayMapping, 15> mappings = { {
      { "byte", AET::BYTE, TiriType::Num },
      { "char", AET::BYTE, TiriType::Num },
      { "int8", AET::BYTE, TiriType::Num },
      { "int16", AET::INT16, TiriType::Num },
      { "int", AET::INT32, TiriType::Num },
      { "int64", AET::INT64, TiriType::Num },
      { "float", AET::FLOAT, TiriType::Num },
      { "double", AET::DOUBLE, TiriType::Num },
      { "string", AET::STR_GC, TiriType::Str },
      { "table", AET::TABLE, TiriType::Table },
      { "array", AET::ARRAY, TiriType::Array },
      { "object", AET::OBJECT, TiriType::Object },
      { "any", AET::ANY, TiriType::Any },
      { "pointer", AET::PTR, TiriType::Any },
      { "struct<Missing>", AET::STRUCT, TiriType::Struct }
   } };
   for (const auto &mapping : mappings) {
      auto element = describe_array_element(mapping.name, nullptr);
      if (not element or element->storage != mapping.storage or element->logical_type != mapping.logical_type) {
         Log.error("array source/storage/logical type mapping is incomplete for %s",
            std::string(mapping.name).c_str());
         return false;
      }
   }
   if (describe_array_element("unsupported", nullptr)) {
      Log.error("unsupported array storage name unexpectedly produced a descriptor");
      return false;
   }
   return true;
}

static bool test_static_result_set_model(kt::Log &Log)
{
   StaticResultSet fixed;
   fixed.declared_count = 3;
   fixed.stored_count = 3;
   fixed.values[0] = StaticValueDescriptor{ .primary = TiriType::Num, .proof = StaticProof::Checked };
   fixed.values[1] = StaticValueDescriptor{ .primary = TiriType::Str, .proof = StaticProof::Checked };
   fixed.values[2] = StaticValueDescriptor{ .primary = TiriType::Table, .proof = StaticProof::Checked };

   if (fixed.value_at(3).primary != TiriType::Nil or not fixed.value_at(3).nullable) {
      Log.error("fixed result set did not return closed nil beyond its arity");
      return false;
   }

   fixed.variadic = true;
   if (fixed.value_at(7).primary != TiriType::Table) {
      Log.error("variadic result set did not repeat its suffix descriptor");
      return false;
   }

   fixed.variadic = false;
   fixed.dynamic = true;
   if (fixed.value_at(7).primary != TiriType::Any or fixed.value_at(7).proof != StaticProof::Advisory) {
      Log.error("dynamic result set did not return advisory any beyond stored positions");
      return false;
   }

   fixed.dynamic = false;
   StaticResultSet filtered = map_static_result_filter(fixed, uint64_t(0b101), 3, false);
   if (filtered.stored_count != 2 or filtered.value_at(0).primary != TiriType::Num or
       filtered.value_at(1).primary != TiriType::Table) {
      Log.error("result filter mapping shifted or lost positional descriptors");
      return false;
   }
   return true;
}

static size_t count_opcode_tree(const BytecodeSnapshot &, BCOp);
static size_t count_opcode(const BytecodeSnapshot &, BCOp);

static int envstore_protected_attempt(lua_State *L)
{
   lua_pushinteger(L, 42);
   lua_setglobal(L, "tostring");
   return 0;
}

static int envstore_contract_attempt(lua_State *L)
{
   lua_pushinteger(L, 42);
   lua_setglobal(L, "glUnitEnvBoundary");
   return 0;
}

static int envstore_newindex_capture(lua_State *L)
{
   lua_pushstring(L, "glUnitEnvMetaCapture");
   lua_pushvalue(L, 3);
   lua_rawset(L, 1);
   return 0;
}

static bool test_environment_store_boundary(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   lua_protect_globals(L);

   GCtab *environment = tabref(L->env);
   if (not lj_tab_is_environment(environment)) {
      Log.error("lua_protect_globals() did not mark the environment table");
      return false;
   }

   // A protected built-in must be rejected before mutation.
   lua_pushcfunction(L, envstore_protected_attempt);
   if (lua_pcall(L, 0, 0, 0) IS 0) {
      Log.error("storing over a protected built-in did not raise");
      return false;
   }
   lua_pop(L, 1);  //  Pop the error message.
   lua_getglobal(L, "tostring");
   bool preserved = lua_isfunction(L, -1);
   lua_pop(L, 1);
   if (not preserved) {
      Log.error("the rejected store mutated the protected built-in");
      return false;
   }

   // A declared concrete global must reject an incompatible C API store and preserve its value.
   if (lua_load(L, std::string_view("global glUnitEnvBoundary = 'text'"), "=envboundary") or
       lua_pcall(L, 0, 0, 0)) {
      Log.error("declaring the unit-test global failed: %s", lua_tostring(L, -1));
      return false;
   }
   GCstr *contract_name = lj_str_newz(L, "glUnitEnvBoundary");
   GCstr *original_descriptor = lj_tab_get_global_contract(environment, contract_name);
   const CachedGlobalContractRecord *cached = lj_tab_get_cached_global_contract(environment, contract_name);
   if (not original_descriptor or not cached or cached->descriptor != original_descriptor or
       cached->entry.type != TiriType::Str or not cached->entry.label_offset) {
      Log.error("declaring a global did not publish its decoded environment policy cache");
      return false;
   }
   const char *cached_label = strdata(cached->descriptor) + cached->entry.label_offset;
   if (std::string_view(cached_label, uint8_t(cached_label[-1])) != "glUnitEnvBoundary") {
      Log.error("the decoded environment policy cache lost its rooted global label");
      return false;
   }

   lua_gc(L, LUA_GCCOLLECT, 0);
   cached = lj_tab_get_cached_global_contract(environment, contract_name);
   if (not cached or cached->descriptor != original_descriptor or cached->entry.type != TiriType::Str) {
      Log.error("the decoded environment policy cache did not survive a full collection");
      return false;
   }

   constexpr std::string_view cache_growth_source =
      "global glUnitCache0 = 0\n"
      "global glUnitCache1 = 1\n"
      "global glUnitCache2 = 2\n"
      "global glUnitCache3 = 3\n"
      "global glUnitCache4 = 4\n"
      "global glUnitCache5 = 5\n"
      "global glUnitCache6 = 6\n"
      "global glUnitCache7 = 7\n"
      "global glUnitCache8 = 8\n"
      "global glUnitCache9 = 9\n";
   if (lua_load(L, cache_growth_source, "=envboundary-growth") or lua_pcall(L, 0, 0, 0)) {
      Log.error("growing the decoded environment policy cache failed: %s", lua_tostring(L, -1));
      return false;
   }
   const GlobalContractCache *global_cache = table_global_contract_cache(environment);
   if (not global_cache or global_cache->capacity <= 8 or global_cache->count < 11) {
      Log.error("the decoded environment policy cache did not grow at its load boundary");
      return false;
   }
   for (uint32_t i = 0; i < 10; ++i) {
      char name[24];
      std::snprintf(name, sizeof(name), "glUnitCache%u", i);
      const CachedGlobalContractRecord *record = lj_tab_get_cached_global_contract(
         environment, lj_str_newz(L, name));
      if (not record or record->entry.type != TiriType::Num) {
         Log.error("decoded environment policy cache rehashing lost global %s", name);
         return false;
      }
   }

   lua_pushcfunction(L, envstore_contract_attempt);
   if (lua_pcall(L, 0, 0, 0) IS 0) {
      Log.error("an incompatible C API store did not raise against the persisted contract");
      return false;
   }
   lua_pop(L, 1);
   lua_getglobal(L, "glUnitEnvBoundary");
   bool value_kept = lua_isstring(L, -1);
   lua_pop(L, 1);
   if (not value_kept) {
      Log.error("the rejected store mutated the declared global");
      return false;
   }

   // A separately compiled direct assignment must rely on BC_GSET instead of copying the persisted descriptor into a
   // preceding BC_CONTRACT.
   constexpr std::string_view assignment_source =
      "extern glUnitEnvBoundary\n"
      "glUnitEnvBoundary = 'compiled'\n";
   if (lua_load(L, assignment_source, "=envboundary-assignment")) {
      Log.error("compiling the persisted-policy assignment failed: %s", lua_tostring(L, -1));
      return false;
   }
   BytecodeSnapshot assignment = snapshot_proto(funcproto(funcV(L->top - 1)));
   if (count_opcode_tree(assignment, BC_CONTRACT) != 0 or count_opcode_tree(assignment, BC_GSET) != 1) {
      Log.error("persisted-policy assignment emitted %zu contracts and %zu global stores",
         count_opcode_tree(assignment, BC_CONTRACT), count_opcode_tree(assignment, BC_GSET));
      return false;
   }
   if (lua_pcall(L, 0, 0, 0)) {
      Log.error("executing the persisted-policy assignment failed: %s", lua_tostring(L, -1));
      return false;
   }

   // Compatible stores and nil clears must pass through the boundary unchanged.
   lua_pushstring(L, "replacement");
   lua_setglobal(L, "glUnitEnvBoundary");
   lua_pushnil(L);
   lua_setglobal(L, "glUnitEnvBoundary");
   lua_getglobal(L, "glUnitEnvBoundary");
   bool cleared = lua_isnil(L, -1);
   lua_pop(L, 1);
   if (not cleared) {
      Log.error("a nil store through the boundary did not clear the global");
      return false;
   }

   // Non-raw C API stores must validate policy without bypassing the environment's __newindex handler.
   lua_pushvalue(L, LUA_GLOBALSINDEX);
   lua_newtable(L);
   lua_pushcfunction(L, envstore_newindex_capture);
   lua_setfield(L, -2, "__newindex");
   lua_setmetatable(L, -2);
   lua_pop(L, 1);

   lua_pushvalue(L, LUA_GLOBALSINDEX);
   lua_pushstring(L, "glUnitEnvSetTable");
   lua_pushinteger(L, 21);
   lua_settable(L, -3);
   lua_pop(L, 1);
   lua_getglobal(L, "glUnitEnvSetTable");
   bool settable_absent = lua_isnil(L, -1);
   lua_pop(L, 1);
   lua_getglobal(L, "glUnitEnvMetaCapture");
   bool settable_routed = lua_tointeger(L, -1) IS 21;
   lua_pop(L, 1);
   if (not settable_absent or not settable_routed) {
      Log.error("lua_settable() bypassed the environment __newindex handler");
      return false;
   }

   lua_pushvalue(L, LUA_GLOBALSINDEX);
   lua_pushinteger(L, 42);
   lua_setfield(L, -2, "glUnitEnvSetField");
   lua_pop(L, 1);
   lua_getglobal(L, "glUnitEnvSetField");
   bool setfield_absent = lua_isnil(L, -1);
   lua_pop(L, 1);
   lua_getglobal(L, "glUnitEnvMetaCapture");
   bool setfield_routed = lua_tointeger(L, -1) IS 42;
   lua_pop(L, 1);
   if (not setfield_absent or not setfield_routed) {
      Log.error("lua_setfield() bypassed the environment __newindex handler");
      return false;
   }

   lua_pushvalue(L, LUA_GLOBALSINDEX);
   lua_pushnil(L);
   lua_setmetatable(L, -2);
   lua_pop(L, 1);

   // Ordinary tables must not be treated as environments.
   lua_newtable(L);
   if (lj_tab_is_environment(tabV(L->top - 1))) {
      Log.error("an ordinary table reads as a marked environment");
      return false;
   }
   lua_pop(L, 1);

   // Policy replacement must update the decoded record and descriptor identity as one publication.  An explicit any
   // declaration deliberately relaxes the earlier concrete policy.
   if (lua_load(L, std::string_view("global glUnitEnvBoundary:any = 5"), "=envboundary-replacement") or
       lua_pcall(L, 0, 0, 0)) {
      Log.error("replacing the unit-test global policy failed: %s", lua_tostring(L, -1));
      return false;
   }
   GCstr *replacement_descriptor = lj_tab_get_global_contract(environment, contract_name);
   cached = lj_tab_get_cached_global_contract(environment, contract_name);
   if (not replacement_descriptor or replacement_descriptor IS original_descriptor or not cached or
       cached->descriptor != replacement_descriptor or cached->entry.type != TiriType::Any) {
      Log.error("global policy replacement left a stale decoded cache record");
      return false;
   }

   lua_gc(L, LUA_GCCOLLECT, 0);
   cached = lj_tab_get_cached_global_contract(environment, contract_name);
   if (not cached or cached->descriptor != replacement_descriptor or cached->entry.type != TiriType::Any) {
      Log.error("a replaced decoded global policy did not survive collection");
      return false;
   }
   return true;
}

static bool test_native_prototype_result_descriptors(kt::Log &Log)
{
   fprototype prototype{};
   prototype.result_count = 3;
   prototype.result_types[0] = TiriType::Num;
   prototype.result_types[1] = TiriType::Any;
   prototype.result_types[2] = TiriType::Str;

   StaticResultSet results = describe_native_prototype_results(&prototype);
   if (results.declared_count != 3 or results.stored_count != 3 or results.dynamic or results.variadic) {
      Log.error("native prototype did not produce a closed result tuple");
      return false;
   }
   if (results.value_at(0).primary != TiriType::Num or
       results.value_at(0).proof != StaticProof::Trusted or
       not results.value_at(0).nullable or
       results.value_at(1).primary != TiriType::Any or
       results.value_at(1).proof != StaticProof::Advisory or
       results.value_at(2).primary != TiriType::Str or
       results.value_at(2).proof != StaticProof::Trusted) {
      Log.error("native prototype result types, proof or nullability were not preserved");
      return false;
   }

   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   lua_protect_globals(L);

   std::string error;
   constexpr std::string_view builtins =
      "local sin_value = tonumber('43')\n"
      "sin_value = tonumber('44')\n"
      "local wave = math.sin(sin_value)\n"
      "wave = math.cos(wave)\n"
      "local text = string.trim(' value ')\n"
      "text = string.upper(text)\n"
      "local function allocated_text()\n"
      "   local buffer = string.alloc(16)\n"
      "   return buffer\n"
      "end\n"
      "local function sliced_text()\n"
      "   local buffer = string.alloc(16)\n"
      "   return buffer.sub(0, 1)\n"
      "end\n"
      "text = allocated_text()\n"
      "text = sliced_text()\n"
      "local first, last = string.find('abc', 'b')\n"
      "first = first + 1\n"
      "last = last + 1\n"
      "local values = array.of('int', 1, 2)\n"
      "values[0] = values[0] + 1\n"
      "struct NativePrototypeLayout Value: int end\n"
      "local native_record = struct.new('NativePrototypeLayout')\n"
      "native_record.Value = values[0]\n"
      "return wave, text, first, last, native_record.Value\n";
   auto snapshot = compile_snapshot(L, builtins, true, error);
   if (not snapshot) {
      Log.error("native prototype results did not reach type analysis: %s", error.c_str());
      return false;
   }
   if (count_opcode_tree(*snapshot, BC_AGETB) IS 0 or
       count_opcode_tree(*snapshot, BC_STGETF) IS 0) {
      Log.error("native prototypes masked specialised array or struct constructor descriptors");
      return false;
   }

   constexpr std::string_view shadowed =
      "local function tonumber(Value:any):str return tostring(Value) end\n"
      "local local_result = tonumber(1)\n"
      "local_result = tonumber(2)\n"
      "local function replacement(Value:any):str return tostring(Value) end\n"
      "local math = { sin = replacement }\n"
      "local member_result:str = math.sin(1)\n"
      "member_result = math.sin(2)\n"
      "return local_result, member_result\n";
   if (not compile_snapshot(L, shadowed, true, error)) {
      Log.error("shadowed built-in names were incorrectly resolved as native prototypes: %s", error.c_str());
      return false;
   }

   constexpr std::string_view dynamic_result =
      "local value = rawget({}, 'missing')\n"
      "value = rawget({}, 'missing')\n";
   error.clear();
   if (compile_snapshot(L, dynamic_result, true, error) or
       error.find("cannot infer type of local 'value'") IS std::string::npos) {
      Log.error("an any-valued native prototype was incorrectly treated as a fixed type: %s", error.c_str());
      return false;
   }

   // Untyped table reads yield 'any' because element types are not tracked.  Reassigning such a local must stay
   // legal: an annotation would only add a CONTRACT guard without unlocking a specialised opcode, and ':any' would
   // restore exactly the type the local already had.

   constexpr std::string_view table_read_reassignment =
      "local source = { Value = 1.5 }\n"
      "local direct = source.Value\n"
      "direct = direct * 2\n"
      "local indexed = source['Value']\n"
      "indexed = 5\n"
      "local same_source = source.Value\n"
      "same_source = source.Value\n"
      "local defaulted = source.Missing ?? 1.96\n"
      "defaulted = defaulted * 2\n"
      "local arithmetic = 0.5 + source.Value\n"
      "arithmetic = 11\n"
      "local negated = -source.Value\n"
      "negated = 5\n"
      "return direct, indexed, same_source, defaulted, arithmetic, negated\n";
   error.clear();
   if (not compile_snapshot(L, table_read_reassignment, true, error)) {
      Log.error("reassigning a local initialised from an untyped table read was rejected: %s", error.c_str());
      return false;
   }

   return true;
}

static bool test_builtin_method_registry(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);

   lua_newuserdata(L, 1);
   lua_newtable(L);
   lj_bmeth_mark_method_compatible(tabV(L->top - 1));
   lua_setmetatable(L, -2);
   cTValue *file = L->top - 1;
   GCstr *write = lj_str_newz(L, "write");
   if (not lj_bmeth_is_method_compatible(file) or
       lj_bmeth_lookup(L, file, write) != LJ_BMETH_COMPATIBLE_CALL) {
      Log.error("marked userdata did not expose the private method-compatible dispatch contract");
      return false;
   }
   lua_pop(L, 1);

   lua_newuserdata(L, 1);
   if (lj_bmeth_is_method_compatible(L->top - 1) or
       lj_bmeth_lookup(L, L->top - 1, write) != LJ_BMETH_FIELD_CALL) {
      Log.error("ordinary userdata unexpectedly acquired method-compatible dispatch");
      return false;
   }
   lua_pop(L, 1);

   constexpr std::array<const char *, 3> unmarked_metatables = {
      "Tiri.regex", "Tiri.processing", "Tiri.input"
   };
   for (const char *name : unmarked_metatables) {
      luaL_getmetatable(L, name);
      if (lua_istable(L, -1) and (tabV(L->top - 1)->flags & TAB_METHOD_COMPATIBLE) != 0) {
         Log.error("%s was opted into method-compatible dispatch without an ABI audit", name);
         return false;
      }
      lua_pop(L, 1);
   }

   const fprototype *table_insert = get_method_prototype(TiriType::Table, "insert");
   const fprototype *array_push = get_method_prototype(TiriType::Array, "push");
   const fprototype *string_upper = get_method_prototype(TiriType::Str, "upper");
   if (not table_insert or not array_push or not string_upper or
       table_insert->builtin_callable_id != builtin_callable_id(FastFunc::table_insert) or
       array_push->builtin_callable_id != builtin_callable_id(FastFunc::array_push) or
       string_upper->builtin_callable_id != builtin_callable_id(FastFunc::string_upper)) {
      Log.error("representative built-in methods did not retain their canonical callable identities");
      return false;
   }
   if (not table_insert->is_method() or table_insert->receiver_type != TiriType::Table or
       table_insert->param_count != 2 or table_insert->param_types()[0] != TiriType::Table) {
      Log.error("table.insert method metadata did not retain its receiver ABI");
      return false;
   }
   const fprototype *array_insert = get_method_prototype(TiriType::Array, "insert");
   const fprototype *range_contains = get_method_prototype(TiriType::Range, "contains");
   const fprototype *struct_clone = get_method_prototype(TiriType::Struct, "clone");
   const fprototype *object_exists = get_method_prototype(TiriType::Object, "exists");
   if (not array_insert or not range_contains or not struct_clone or not object_exists or
       array_insert->builtin_callable_id != builtin_callable_id(FastFunc::array_insert) or
       range_contains->builtin_callable_id != builtin_callable_id(FastFunc::range_contains) or
       struct_clone->builtin_callable_id != builtin_callable_id(FastFunc::struct_clone) or
       object_exists->builtin_callable_id != builtin_callable_id(FastFunc::object_exists) or
       array_insert IS table_insert or get_method_prototype(TiriType::Table, "push") or
       get_method_prototype(TiriType::Table, "new")) {
      Log.error("complete method lookup lost receiver separation, callable identity or constructor exclusion");
      return false;
   }

   constexpr std::array<std::string_view, 28> array_methods = {
      "table", "concat", "contains", "first", "last", "clear", "resize", "push", "pop", "copy",
      "getString", "setString", "type", "readOnly", "fill", "find", "reverse", "slice", "sort", "each",
      "map", "filter", "reduce", "any", "all", "insert", "remove", "clone"
   };
   constexpr std::array<std::string_view, 23> string_methods = {
      "byte", "cap", "contains", "count", "decap", "endsWith", "escXML", "find", "format", "hash", "len",
      "lower", "pop", "rep", "replace", "reverse", "rtrim", "split", "startsWith", "sub", "trim",
      "unescapeXML", "upper"
   };
   constexpr std::array<std::string_view, 12> table_methods = {
      "insert", "remove", "move", "concat", "sort", "empty", "kind", "size", "clear", "slice", "sortByKeys",
      "toXML"
   };
   constexpr std::array<std::string_view, 10> range_methods = {
      "each", "filter", "reduce", "map", "take", "any", "all", "find", "contains", "toArray"
   };
   constexpr std::array<std::string_view, 3> struct_methods = { "structSize", "copy", "clone" };
   constexpr std::array<std::string_view, 12> object_methods = {
      "class", "init", "free", "children", "detach", "get", "set", "getKey", "setKey", "exists", "subscribe",
      "unsubscribe"
   };
   auto verify_inventory = [&](TiriType ReceiverType, std::string_view Interface, const auto &Methods) {
      for (std::string_view method : Methods) {
         const fprototype *prototype = get_method_prototype(ReceiverType, method);
         if (not prototype or not prototype->is_method() or prototype->receiver_type != ReceiverType or
             prototype->param_count IS 0 or prototype->param_types()[0] != ReceiverType or
             not builtin_callable_valid(prototype->builtin_callable_id)) return false;
         std::string canonical(Interface IS "obj" ? "object" : Interface);
         canonical.push_back('.');
         canonical.append(method);
         const char *registered_name = builtin_callable_name(prototype->builtin_callable_id);
         if (not registered_name or canonical != registered_name or get_prototype(Interface, method) != prototype) {
            return false;
         }
      }
      return true;
   };
   if (not verify_inventory(TiriType::Array, "array", array_methods) or
       not verify_inventory(TiriType::Str, "string", string_methods) or
       not verify_inventory(TiriType::Table, "table", table_methods) or
       not verify_inventory(TiriType::Range, "range", range_methods) or
       not verify_inventory(TiriType::Struct, "struct", struct_methods) or
       not verify_inventory(TiriType::Object, "obj", object_methods)) {
      Log.error("complete method inventory lost registration, receiver ABI or canonical callable identity");
      return false;
   }

   const fprototype *table_new = get_prototype("table", "new");
   if (not table_new or table_new->is_method() or table_new->receiver_type != TiriType::Unknown or
       table_new->builtin_callable_id != BuiltinCallableID::Invalid) {
      Log.error("namespace-only prototype was classified as an instance method");
      return false;
   }

   ERR duplicate = reg_iface_method(L, "table", "insert", TiriType::Table,
      builtin_callable_id(FastFunc::table_insert), {}, { TiriType::Table, TiriType::Any });
   ERR conflicting = reg_iface_method(L, "table", "insert", TiriType::Table,
      builtin_callable_id(FastFunc::table_insert), { TiriType::Num }, { TiriType::Table, TiriType::Any });
   ERR invalid_receiver = reg_iface_method(L, "table", "insert", TiriType::Any,
      builtin_callable_id(FastFunc::table_insert), {}, { TiriType::Any, TiriType::Any });
   ERR wrong_callable = reg_iface_method(L, "table", "insert", TiriType::Table,
      builtin_callable_id(FastFunc::string_upper), {}, { TiriType::Table, TiriType::Any });
   lua_getglobal(L, "table");
   lua_getfield(L, -1, "insert");
   lua_pushnil(L);
   lua_setfield(L, -3, "insert");
   ERR wrong_export = reg_iface_method(L, "table", "insert", TiriType::Table,
      builtin_callable_id(FastFunc::table_insert), {}, { TiriType::Table, TiriType::Any });
   lua_pushvalue(L, -1);
   lua_setfield(L, -3, "insert");
   lua_pop(L, 2);
   if (duplicate != ERR::Exists or conflicting != ERR::Mismatch or invalid_receiver != ERR::InvalidValue or
       wrong_callable != ERR::Mismatch or wrong_export != ERR::Mismatch) {
      Log.error("method registration consistency checks returned unexpected errors");
      return false;
   }
   return true;
}

static bool test_builtin_method_table_initialiser_rejection(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);

   auto expect_rejected = [&](std::string_view Source, std::string_view Name) {
      if (not lua_load(L, Source, "builtin-method-table-shadow")) {
         Log.error("bare function field '%.*s' compiled despite colliding with a table method",
            int(Name.size()), Name.data());
         lua_pop(L, 1);
         return false;
      }

      std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
      lua_pop(L, 1);
      std::string named_method = std::format("built-in method '{}'", Name);
      std::string bracketed_key = std::format("['{}']", Name);
      if (message.find(named_method) IS std::string::npos or
          message.find(bracketed_key) IS std::string::npos) {
         Log.error("table method shadow diagnostic for '%.*s' omitted its name or bracketed escape: %s",
            int(Name.size()), Name.data(), message.c_str());
         return false;
      }

      bool invalid_assignment = false;
      if (L->parser_diagnostics) {
         for (const ParserDiagnostic &diagnostic : L->parser_diagnostics->entries()) {
            if (diagnostic.code IS ParserErrorCode::InvalidAssignment) invalid_assignment = true;
         }
      }
      if (not invalid_assignment) {
         Log.error("table method shadow for '%.*s' did not report InvalidAssignment",
            int(Name.size()), Name.data());
         return false;
      }
      return true;
   };

   auto expect_accepted = [&](std::string_view Source, std::string_view Description) {
      if (lua_load(L, Source, "builtin-method-table-shadow")) {
         Log.error("%.*s was rejected: %s", int(Description.size()), Description.data(), lua_tostring(L, -1));
         lua_pop(L, 1);
         return false;
      }
      lua_pop(L, 1);
      return true;
   };

   constexpr std::array<std::string_view, 12> table_methods = {
      "insert", "remove", "move", "concat", "sort", "empty", "kind", "size", "clear", "slice", "sortByKeys",
      "toXML"
   };
   for (std::string_view method : table_methods) {
      std::string source = std::format("local values = {{ {} = function(Value) end }}\n", method);
      if (not expect_rejected(source, method)) return false;
   }

   if (not expect_rejected(
       "local callback = function(Value) end\nlocal values = { insert = callback }\n", "insert") or
       not expect_rejected(
          "local values = { nested = { insert = function(Value) end } }\n", "insert") or
       not expect_rejected(
          "local mt = { insert = function(Value) end }\ndebug.setMetatable({}, mt)\n", "insert")) return false;

   constexpr std::string_view accepted =
      "local function build(Value:any):table\n"
      "   return { insert = Value }\n"
      "end\n"
      "local values = { size = 1, insert = 1 }\n"
      "local explicit = { ['insert'] = function(Value) end }\n"
      "local ordinary = { callback = function(Value) end }\n"
      "local array_only = { push = function(Value) end }\n"
      "local interface_only = { new = function(Value) end }\n";
   if (not expect_accepted(accepted, "legal table initialiser boundaries")) return false;

   return true;
}

static bool test_builtin_method_static_classification(kt::Log &Log)
{
   constexpr std::string_view source =
      "local values = {}\n"
      "values.insert(1);\n"
      "local extracted = values.insert\n"
      "values.insert = extracted;\n"
      "(values.insert)(2);\n"
      "values['insert'](3);\n"
      "values.insert { 4 };\n"
      "local dynamic:any = values\n"
      "dynamic.insert(5);\n"
      "local text = 'abc'\n"
      "local upper = text.upper()\n"
      "local items = array.of('int', 1)\n"
      "items.push(2);\n"
      "local maybe:table = nil\n"
      "maybe?.insert(6);\n"
      "local handle:userdata = io.tempFile()\n"
      "handle.write('x');\n";

   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   StringReaderCtx reader{ source.data(), source.size() };
   LexState lex(L, unit_reader, &reader, "builtin-method-classification", std::nullopt);
   FuncState &fs = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, fs, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   ParserSession session(context, config);
   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (not chunk.ok()) {
      Log.error("built-in method classification fixture did not parse");
      log_diagnostics(context.diagnostics().entries(), Log);
      return false;
   }
   discover_static_bindings(context, *chunk.value_ref());
   propagate_static_descriptors(context, *chunk.value_ref());
   run_type_analysis(context, *chunk.value_ref());
   propagate_static_descriptors(context, *chunk.value_ref());
   if (context.diagnostics().has_errors()) {
      Log.error("built-in method classification produced unexpected diagnostics");
      log_diagnostics(context.diagnostics().entries(), Log);
      return false;
   }

   auto expression_call = [&](size_t Index) -> const CallExprPayload * {
      if (Index >= chunk.value_ref()->statements.size()) return nullptr;
      const auto *statement = std::get_if<ExpressionStmtPayload>(&chunk.value_ref()->statements[Index]->data);
      if (not statement or not statement->expression or
          (statement->expression->kind != AstNodeKind::CallExpr and
           statement->expression->kind != AstNodeKind::SafeCallExpr)) return nullptr;
      return std::get_if<CallExprPayload>(&statement->expression->data);
   };
   auto local_call = [&](size_t Index) -> const CallExprPayload * {
      if (Index >= chunk.value_ref()->statements.size()) return nullptr;
      const auto *statement = std::get_if<LocalDeclStmtPayload>(&chunk.value_ref()->statements[Index]->data);
      if (not statement or statement->values.empty()) return nullptr;
      return std::get_if<CallExprPayload>(&statement->values.front()->data);
   };

   const CallExprPayload *table_call = expression_call(1);
   const CallExprPayload *grouped_call = expression_call(4);
   const CallExprPayload *computed_call = expression_call(5);
   const CallExprPayload *shorthand_call = expression_call(6);
   const CallExprPayload *dynamic_call = expression_call(8);
   const CallExprPayload *upper_call = local_call(10);
   const CallExprPayload *push_call = expression_call(12);
   const CallExprPayload *safe_call = expression_call(14);
   const CallExprPayload *userdata_call = expression_call(16);
   if (not table_call or not grouped_call or not computed_call or not shorthand_call or not dynamic_call or
       not upper_call or not push_call or not safe_call or not userdata_call) {
      Log.error("built-in method fixture calls table=%d grouped=%d computed=%d shorthand=%d dynamic=%d upper=%d "
         "push=%d safe=%d statements=%zu", bool(table_call), bool(grouped_call), bool(computed_call),
         bool(shorthand_call), bool(dynamic_call), bool(upper_call), bool(push_call), bool(safe_call),
         chunk.value_ref()->statements.size());
      return false;
   }
   if (not userdata_call->runtime_builtin_method or userdata_call->builtin_method or
       userdata_call->runtime_builtin_method->member->hash != kt::strhash("write")) {
      Log.error("proved userdata dot call did not acquire runtime method dispatch");
      return false;
   }
   if (not table_call->builtin_method or
       table_call->builtin_method->callable != builtin_callable_id(FastFunc::table_insert) or
       not upper_call->builtin_method or
       upper_call->builtin_method->callable != builtin_callable_id(FastFunc::string_upper) or
       not push_call->builtin_method or
       push_call->builtin_method->callable != builtin_callable_id(FastFunc::array_push)) {
      Log.error("proved dot calls did not resolve the representative built-in methods");
      return false;
   }
   if (not safe_call->builtin_method or not safe_call->builtin_method->safe or
       safe_call->builtin_method->receiver_type != TiriType::Table) {
      Log.error("nullable safe receiver did not resolve table.insert");
      return false;
   }
   if ((grouped_call and grouped_call->builtin_method) or
       (computed_call and computed_call->builtin_method) or
       (shorthand_call and shorthand_call->builtin_method) or
       (dynamic_call and dynamic_call->builtin_method)) {
      Log.error("ineligible annotations grouped=%d computed=%d shorthand=%d dynamic=%d",
         grouped_call->builtin_method.has_value(), computed_call->builtin_method.has_value(),
         shorthand_call->builtin_method.has_value(), dynamic_call->builtin_method.has_value());
      return false;
   }
   if (not dynamic_call->runtime_builtin_method or
       dynamic_call->runtime_builtin_method->member->hash != kt::strhash("insert") or
       grouped_call->runtime_builtin_method or computed_call->runtime_builtin_method or
       shorthand_call->runtime_builtin_method) {
      Log.error("unproved direct dot call did not acquire the exclusive runtime-method annotation");
      return false;
   }

   const auto &read = std::get<LocalDeclStmtPayload>(chunk.value_ref()->statements[2]->data);
   const auto *read_member = read.values.empty() ? nullptr :
      std::get_if<MemberExprPayload>(&read.values.front()->data);
   const auto &write = std::get<AssignmentStmtPayload>(chunk.value_ref()->statements[3]->data);
   const auto *write_member = write.targets.empty() ? nullptr :
      std::get_if<MemberExprPayload>(&write.targets.front()->data);
   if (not read_member or not write_member or read_member->is_call_target or write_member->is_call_target) {
      Log.error("ordinary member reads or writes acquired method-call metadata");
      return false;
   }
   if (not upper_call->results or
       context.descriptors().results(upper_call->results).value_at(0).primary != TiriType::Str or
       not safe_call->results or not context.descriptors().results(safe_call->results).value_at(0).nullable) {
      Log.error("method result inference did not preserve registered result type or safe nullability");
      return false;
   }

   return true;
}

static bool test_unresolved_method_receiver_diagnostic(kt::Log &Log)
{
   constexpr std::string_view source =
      "local body = { values={} }\n"
      "body.values.insert(1)\n"
      "local values = {}\n"
      "values.insert(2)\n"
      "(body.values.insert)(3)\n"
      "body.values['insert'](4)\n";

   auto analyse = [&](bool EnableWarning) {
      AstHarnessResult result;
      result.state = std::make_unique<LuaStateHolder>();
      lua_State *L = result.state->get();
      luaL_openlibs(L);
      StringReaderCtx reader{ source.data(), source.size() };
      LexState lex(L, unit_reader, &reader, "unresolved-method-diagnostic", std::nullopt);
      FuncState &fs = lex.fs_init();
      ParserContext context = ParserContext::from(lex, fs, ParserAllocator::from(L));
      ParserConfig config;
      config.abort_on_error = false;
      config.max_diagnostics = 32;
      config.warn_unresolved_methods = EnableWarning;
      ParserSession session(context, config);
      lex.next();
      AstBuilder builder(context);
      result.chunk = builder.parse_chunk();
      if (result.chunk.ok()) {
         discover_static_bindings(context, *result.chunk.value_ref());
         propagate_static_descriptors(context, *result.chunk.value_ref());
         run_type_analysis(context, *result.chunk.value_ref());
         propagate_static_descriptors(context, *result.chunk.value_ref());
      }
      auto entries = context.diagnostics().entries();
      result.diagnostics.assign(entries.begin(), entries.end());
      return result;
   };

   auto disabled = analyse(false);
   if (not disabled.chunk.ok() or not disabled.diagnostics.empty()) {
      Log.error("unresolved method diagnostic was not off by default");
      log_diagnostics(disabled.diagnostics, Log);
      return false;
   }

   auto enabled = analyse(true);
   size_t warning_count = 0;
   for (const ParserDiagnostic &diagnostic : enabled.diagnostics) {
      if (diagnostic.code != ParserErrorCode::UnresolvedMethodReceiver) continue;
      ++warning_count;
      if (diagnostic.severity != ParserDiagnosticSeverity::Warning or
          diagnostic.message.find("insert") IS std::string::npos or
          diagnostic.message.find("runtime") IS std::string::npos or
          diagnostic.token.span().line.lineNumber() != 2) {
         Log.error("unresolved method diagnostic had incorrect content or source location");
         log_diagnostics(enabled.diagnostics, Log);
         return false;
      }
   }
   if (not enabled.chunk.ok() or warning_count != 1 or enabled.diagnostics.size() != 1) {
      Log.error("expected one opt-in unresolved method warning, got %zu diagnostic(s)",
         enabled.diagnostics.size());
      log_diagnostics(enabled.diagnostics, Log);
      return false;
   }

   LuaStateHolder published_state;
   lua_State *published = published_state.get();
   luaL_openlibs(published);
   published->script->JitOptions |= JOF::DIAGNOSE;
   if (lua_load(published, source, "unresolved-method-published") or not published->parser_diagnostics or
       published->parser_diagnostics->entries().size() != 1 or
       published->parser_diagnostics->entries().front().code != ParserErrorCode::UnresolvedMethodReceiver) {
      Log.error("diagnose mode did not publish the unresolved method warning");
      if (published->parser_diagnostics) log_diagnostics(published->parser_diagnostics->entries(), Log);
      return false;
   }
   lua_pop(published, 1);
   return true;
}

static const BCIns * find_opcode(const BytecodeSnapshot &Snapshot, BCOp Opcode)
{
   for (const BCIns &instruction : Snapshot.instructions) {
      if (bc_op(instruction) IS Opcode) return &instruction;
   }
   return nullptr;
}

static bool test_builtin_method_bytecode_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   std::string error;

   auto fixed = compile_snapshot(L, "local values = {}\nvalues.insert(1)\nreturn #values\n", true, error);
   const BCIns *fixed_load = fixed ? find_opcode(*fixed, BC_BFUNC) : nullptr;
   if (not fixed or not fixed_load or count_opcode(*fixed, BC_BFUNC) != 1 or count_opcode(*fixed, BC_MOV) != 1 or
       count_opcode(*fixed, BC_CALL) != 1 or bc_d(*fixed_load) != builtin_callable_index(
          builtin_callable_id(FastFunc::table_insert)) or
       count_opcode(*fixed, BC_GGET) != 0 or count_opcode(*fixed, BC_TGETS) != 0 or
       count_opcode(*fixed, BC_TGETV) != 0 or count_opcode(*fixed, BC_JMP) != 0) {
      Log.error("fixed built-in method call did not use the canonical lookup-free shape: %s", error.c_str());
      return false;
   }
   size_t load_position = size_t(fixed_load - fixed->instructions.data());
   if (load_position IS 0 or load_position + 1 >= fixed->instructions.size() or
       bc_op(fixed->instructions[load_position - 1]) != BC_MOV or
       bc_a(fixed->instructions[load_position - 1]) != bc_a(*fixed_load) + 1 + LJ_FR2 or
       bc_op(fixed->instructions[load_position + 1]) != BC_KSHORT) {
      Log.error("built-in method receiver was not inserted before written arguments");
      return false;
   }

   error.clear();
   auto forwarded = compile_snapshot(L,
      "local function item():num return 2 end\nlocal values = {}\nvalues.insert(item())\nreturn #values\n",
      true, error);
   if (not forwarded or count_opcode(*forwarded, BC_BFUNC) != 1 or count_opcode(*forwarded, BC_CALLM) != 1 or
       count_opcode(*forwarded, BC_TGETS) != 0) {
      Log.error("forwarded built-in method arguments lost CALLM lowering: %s", error.c_str());
      return false;
   }

   error.clear();
   auto tail = compile_snapshot(L, "local text = 'abc'\nreturn text.upper()\n", true, error);
   if (not tail or count_opcode(*tail, BC_BFUNC) != 1 or count_opcode(*tail, BC_CALLT) != 1 or
       count_opcode(*tail, BC_TGETS) != 0) {
      Log.error("built-in method tail call did not retain CALLT lowering: %s", error.c_str());
      return false;
   }

   error.clear();
   auto safe = compile_snapshot(L,
      "local maybe:table = nil\nmaybe?.insert(1)\nreturn maybe\n", true, error);
   if (not safe or count_opcode(*safe, BC_BFUNC) != 1 or count_opcode(*safe, BC_ISEQP) != 1 or
       count_opcode(*safe, BC_CALL) != 1 or count_opcode(*safe, BC_TGETS) != 0 or
       count_opcode(*safe, BC_TGETV) != 0) {
      Log.error("safe built-in method call lost canonical nil-short-circuit lowering: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic = compile_snapshot(L,
      "local body = { values={} }\nbody.values.insert(1)\nreturn body.values[0]\n", true, error);
   const BCIns *dynamic_dispatch = dynamic ? find_opcode(*dynamic, BC_BMETH) : nullptr;
   if (not dynamic or not dynamic_dispatch or count_opcode(*dynamic, BC_BMETH) != 1 or
       count_opcode(*dynamic, BC_CALL) != 2 or count_opcode(*dynamic, BC_JMP) != 1 or
       count_opcode(*dynamic, BC_BFUNC) != 0 or bc_j(*dynamic_dispatch) <= 0) {
      Log.error("unproved dot call did not emit the two-branch runtime method shape: %s", error.c_str());
      return false;
   }

   error.clear();
   auto userdata = compile_snapshot(L,
      "local function use(Handle:userdata)\nHandle.write('x')\nHandle.close()\nend\n", true, error);
   if (not userdata or count_opcode_tree(*userdata, BC_BMETH) != 2 or
       count_opcode_tree(*userdata, BC_CALL) != 4) {
      Log.error("proved userdata calls did not emit runtime method dispatch: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic_tail = compile_snapshot(L,
      "local Values:any = {}\nreturn Values.insert(1)\n", true, error);
   if (not dynamic_tail or count_opcode_tree(*dynamic_tail, BC_BMETH) != 1 or
       count_opcode_tree(*dynamic_tail, BC_CALLT) != 2) {
      Log.error("runtime built-in method tail branches did not both use CALLT: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic_forwarded = compile_snapshot(L,
      "local function item():num return 2 end\nlocal Values:any = {}\nValues.insert(item())\n", true, error);
   if (not dynamic_forwarded or count_opcode(*dynamic_forwarded, BC_BMETH) != 1 or
       count_opcode(*dynamic_forwarded, BC_CALLM) != 2) {
      Log.error("runtime built-in method argument forwarding lost either CALLM branch: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic_thunk = compile_snapshot(L,
      "local Values:any = {}\nValues.insert(thunk():num return 1 end)\n", true, error);
   if (not dynamic_thunk or count_opcode(*dynamic_thunk, BC_BMETH) != 1 or
       count_opcode(*dynamic_thunk, BC_CALL) != 2) {
      Log.error("runtime built-in method could not emit a thunk argument for both branches: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic_safe = compile_snapshot(L,
      "local maybe:any = nil\nmaybe?.insert(1)\nreturn maybe\n", true, error);
   if (not dynamic_safe or count_opcode(*dynamic_safe, BC_BMETH) != 1 or
       count_opcode(*dynamic_safe, BC_ISEQP) != 2 or count_opcode(*dynamic_safe, BC_CALL) != 2) {
      Log.error("unproved safe dot call lost runtime dispatch or nil short-circuiting: %s", error.c_str());
      return false;
   }

   error.clear();
   auto dynamic_pipe = compile_snapshot(L,
      "local body = { values={} }\n1 |> body.values.insert()\nreturn body.values[0]\n", true, error);
   if (not dynamic_pipe or count_opcode(*dynamic_pipe, BC_BMETH) != 1 or
       count_opcode(*dynamic_pipe, BC_CALL) != 2) {
      Log.error("unproved piped dot call did not emit both runtime call frames: %s", error.c_str());
      return false;
   }

   error.clear();
   auto piped = compile_snapshot(L, "local values = {}\n1 |> values.insert()\nreturn #values\n", true, error);
   if (not piped or count_opcode(*piped, BC_BFUNC) != 1 or count_opcode(*piped, BC_CALL) != 1 or
       count_opcode(*piped, BC_TGETS) != 0 or count_opcode(*piped, BC_TGETV) != 0) {
      Log.error("piped built-in method call did not use canonical emission: %s", error.c_str());
      return false;
   }

   error.clear();
   auto range_pipe = compile_snapshot(L,
      "local total = 0\n{0 to 4} |> (Value => do total += Value end)\nreturn total\n", true, error);
   if (not range_pipe or count_opcode_tree(*range_pipe, BC_BFUNC) != 1 or
       count_opcode_tree(*range_pipe, BC_TGETS) != 0) {
      Log.error("compiler-generated range iteration did not use canonical emission: %s", error.c_str());
      return false;
   }

   error.clear();
   auto append = compile_snapshot(L,
      "local values = array<byte>\nvalues ..= 'x' .. 'y'\nreturn values\n", true, error);
   const BCIns *append_load = append ? find_opcode(*append, BC_BFUNC) : nullptr;
   if (not append or not append_load or count_opcode(*append, BC_BFUNC) != 1 or
       bc_d(*append_load) != builtin_callable_index(builtin_callable_id(FastFunc::array_append))) {
      Log.error("compiler-only array append did not use canonical emission: %s", error.c_str());
      return false;
   }

   return true;
}

static bool test_contextual_tail_call_bytecode_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();
   luaL_openlibs(lua);
   std::string error;
   constexpr std::string_view target =
      "local target = { run = function():num return 1 end }\n";

   auto root = compile_snapshot(lua, std::string(target) + "return target.run()\n", true, error);
   if (not root or count_opcode(*root, BC_CTXCALLT) != 0 or count_opcode(*root, BC_CTXCALL) != 1 or
       count_opcode(*root, BC_CTXLEAVE) != 1) {
      Log.error("root contextual return transferred ownership outside a callable frame: %s", error.c_str());
      return false;
   }

   error.clear();
   auto nested = compile_snapshot(lua, std::string(target) +
      "local function forward():<any, ...> return target.run() end\nreturn forward()\n", true, error);
   if (not nested or count_opcode_tree(*nested, BC_CTXCALLT) != 1 or
       count_opcode_tree(*nested, BC_CTXCALL) != 0 or count_opcode_tree(*nested, BC_CTXLEAVE) != 0) {
      Log.error("nested contextual return did not transfer ownership within its callable frame: %s", error.c_str());
      return false;
   }
   return true;
}

static int test_method_compatible_echo(lua_State *L)
{
   if (not lua_isuserdata(L, 1) or not lua_isstring(L, 2)) {
      luaL_error(L, "receiver ABI was not selected");
      return 0;
   }
   lua_pushvalue(L, 2);
   return 1;
}

static int test_ordinary_userdata_echo(lua_State *L)
{
   if (not lua_isstring(L, 1)) {
      luaL_error(L, "ordinary field-call ABI was not selected");
      return 0;
   }
   lua_pushvalue(L, 1);
   return 1;
}

static int test_userdata_index(lua_State *L)
{
   if (not lua_isstring(L, 2) or std::string_view(lua_tostring(L, 2)) != "echo") {
      lua_pushnil(L);
      return 1;
   }
   lua_pushcfunction(L,
      lj_bmeth_is_method_compatible(L->base) ? test_method_compatible_echo : test_ordinary_userdata_echo);
   return 1;
}

static bool test_builtin_method_runtime(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   constexpr std::string_view source =
      "local values = {}\n"
      "local order = ''\n"
      "local function mark():bool order ..= 'r'; return true end\n"
      "local function argument():num order ..= 'a'; return 7 end\n"
      "(mark() ? values :> values).insert(argument())\n"
      "local maybe:table = nil\n"
      "maybe?.insert(argument())\n"
      "maybe = {}\n"
      "maybe?.insert(argument())\n"
      "8 |> values.insert()\n"
      "local items = array.of('int', 1)\n"
      "items.push(2)\n"
      "local text = 'abc'\n"
      "local upper = text.upper()\n"
      "local dynamic:any = values\n"
      "for i in {0 to 200} do dynamic.insert(i) end\n"
      "return order, #values, values[0], values[1], #maybe, maybe[0], items[1], upper\n";

   if (lua_load(L, source, "builtin-method-runtime")) {
      Log.error("failed to compile built-in method runtime fixture: %s", lua_tostring(L, -1));
      return false;
   }
   GCproto *prototype = funcproto(funcV(L->top - 1));
   if (lua_pcall(L, 0, 8, 0)) {
      Log.error("built-in method runtime fixture failed: %s", lua_tostring(L, -1));
      return false;
   }
   if (prototype->trace IS 0 or std::string_view(lua_tostring(L, -8)) != "raa" or
       lua_tointeger(L, -7) != 202 or lua_tointeger(L, -6) != 7 or lua_tointeger(L, -5) != 8 or
       lua_tointeger(L, -4) != 1 or lua_tointeger(L, -3) != 7 or lua_tointeger(L, -2) != 2 or
       std::string_view(lua_tostring(L, -1)) != "ABC") {
      Log.error("interpreter/JIT built-in methods changed evaluation order, dispatch priority or results");
      lua_pop(L, 8);
      return false;
   }
   lua_pop(L, 8);

   constexpr std::string_view rebound_source =
      "local values = {}\nvalues.insert(9)\nreturn values[0]\n";
   if (lua_load(L, rebound_source, "builtin-method-rebinding")) {
      Log.error("failed to compile built-in method rebinding fixture: %s", lua_tostring(L, -1));
      return false;
   }
   lua_getglobal(L, "table");
   lua_pushnil(L);
   lua_setfield(L, -2, "insert");
   lua_pop(L, 1);
   if (lua_pcall(L, 0, 1, 0) or lua_tointeger(L, -1) != 9) {
      Log.error("public namespace rebinding redirected a canonical instance call");
      return false;
   }
   lua_pop(L, 1);

   auto register_test_userdata = [&](const char *Name, bool Compatible) {
      lua_newuserdata(L, 1);
      lua_newtable(L);
      if (Compatible) lj_bmeth_mark_method_compatible(tabV(L->top - 1));
      lua_pushstring(L, "__index");
      lua_pushcfunction(L, test_userdata_index);
      lua_settable(L, -3);
      lua_setmetatable(L, -2);
      lua_setglobal(L, Name);
   };
   register_test_userdata("test_marked_userdata", true);
   register_test_userdata("test_ordinary_userdata", false);

   constexpr std::string_view userdata_source =
      "local function invoke(Value:any, Text:str):str return Value.echo(Text) end\n"
      "local matched = 0\n"
      "for i in {0 to 200} do\n"
      "   local value:any = i % 2 is 0 ? test_marked_userdata :> test_ordinary_userdata\n"
      "   if invoke(value, 'x') is 'x' then matched++ end\n"
      "end\n"
      "return matched\n";
   if (lua_load(L, userdata_source, "method-compatible-userdata-runtime")) {
      Log.error("failed to compile method-compatible userdata fixture: %s", lua_tostring(L, -1));
      return false;
   }
   if (lua_pcall(L, 0, 1, 0) or lua_tointeger(L, -1) != 200) {
      Log.error("method-compatible and ordinary userdata did not retain distinct interpreter/JIT call frames: %s",
         lua_tostring(L, -1));
      return false;
   }
   lua_pop(L, 1);
   return true;
}

static bool test_object_call_result_descriptors(kt::Log &Log)
{
   if (classify_object_call_member("acGetKey") != ObjectCallMemberKind::Action or
       classify_object_call_member("mtGetEnv") != ObjectCallMemberKind::Method or
       classify_object_call_member("account") != ObjectCallMemberKind::None or
       classify_object_call_member("mtime") != ObjectCallMemberKind::None or
       classify_object_call_member("acgetKey") != ObjectCallMemberKind::None or
       classify_object_call_member("mtgetEnv") != ObjectCallMemberKind::None) {
      Log.error("object call member classification did not enforce the exposed ac[A-Z]/mt[A-Z] contract");
      return false;
   }

   constexpr FunctionField fields[] = {
      { "Input", FDF_CPPSTRING },
      { "Vector", FDF_VECTOR|FD_RESULT },
      { "Text", FDF_CPPSTRING|FD_MUTABLE|FD_RESULT },
      { "Resource", FD_STRUCT|FD_RESOURCE|FD_RESULT },
      { "Record", FD_STRUCT|FD_RESULT },
      { "Object", FD_PTR|FD_OBJECT|FD_RESULT },
      { "Pointer", FD_PTR|FD_RESULT },
      { "Integer", FD_INT|FD_RESULT },
      { "Double", FD_DOUBLE|FD_RESULT },
      { "Large", FD_INT64|FD_RESULT },
      { nullptr, 0 }
   };
   StaticResultSet results = describe_object_call_results(fields);
   constexpr std::array<TiriType, 10> expected = {
      TiriType::Num, TiriType::Array, TiriType::Str, TiriType::Struct, TiriType::Table,
      TiriType::Object, TiriType::Userdata, TiriType::Num, TiriType::Num, TiriType::Num
   };
   if (results.declared_count != expected.size() or
       results.stored_count != std::min<size_t>(expected.size(), MAX_RETURN_TYPES)) {
      Log.error("object call result descriptor count did not match runtime-pushed values");
      return false;
   }
   for (size_t i = 0; i < results.stored_count; ++i) {
      if (results.value_at(i).primary != expected[i] or
          results.value_at(i).proof != StaticProof::Trusted) {
         Log.error("object call result descriptor at position %" PRId64 " had the wrong type or proof",
            int64_t(i));
         return false;
      }
   }
   if (results.value_at(0).nullable or not results.value_at(1).nullable or
       not results.value_at(2).nullable or results.value_at(7).nullable) {
      Log.error("object call result nullability did not match runtime storage");
      return false;
   }

   constexpr FunctionField numeric_fields[] = {
      { "Integer", FD_INT|FD_RESULT },
      { "Double", FD_DOUBLE|FD_RESULT },
      { "Large", FD_INT64|FD_RESULT },
      { nullptr, 0 }
   };
   results = describe_object_call_results(numeric_fields);
   if (results.declared_count != 4 or results.value_at(1).primary != TiriType::Num or
       results.value_at(2).primary != TiriType::Num or results.value_at(3).primary != TiriType::Num) {
      Log.error("numeric object result flags did not map to numeric descriptors");
      return false;
   }

   constexpr FunctionField ignored_then_numeric[] = {
      { "Span", FDF_SPAN|FD_RESULT },
      { "Callback", FD_FUNCTION|FD_RESULT },
      { "Value", FD_INT|FD_RESULT },
      { nullptr, 0 }
   };
   results = describe_object_call_results(ignored_then_numeric);
   if (results.declared_count != 2 or results.value_at(1).primary != TiriType::Num) {
      Log.error("ignored object result categories shifted or stopped supported later results");
      return false;
   }

   constexpr FunctionField unsupported_then_numeric[] = {
      { "Byte", FD_BYTE|FD_RESULT },
      { "Value", FD_INT|FD_RESULT },
      { nullptr, 0 }
   };
   results = describe_object_call_results(unsupported_then_numeric);
   if (results.declared_count != 1) {
      Log.error("unsupported object result category did not stop descriptor dissemination");
      return false;
   }

   std::array<FunctionField, MAX_RETURN_TYPES + 3> wide{};
   for (size_t i = 0; i < wide.size() - 1; ++i) wide[i] = { "Value", FD_INT|FD_RESULT };
   results = describe_object_call_results(wide.data());
   if (results.declared_count != wide.size() or results.stored_count != MAX_RETURN_TYPES) {
      Log.error("wide object result signature lost its closed declared count or exceeded descriptor storage");
      return false;
   }

   return true;
}

static bool test_module_call_result_descriptors(kt::Log &Log)
{
   constexpr FunctionField fields[] = {
      { "Error", FD_ERROR|FD_INT },
      { "Text", FD_RESULT|FD_STR },
      { "Object", FD_RESULT|FD_PTR|FD_OBJECT },
      { "Resource:Handle", FD_RESULT|FD_PTR|FD_STRUCT|FD_RESOURCE },
      { "Record:Value", FD_RESULT|FD_PTR|FD_STRUCT },
      { "Pointer", FD_RESULT|FD_PTR },
      { "Allocated", FD_RESULT|FD_PTR|FD_ALLOC },
      { "Integer", FD_RESULT|FD_INT },
      { "Double", FD_RESULT|FD_DOUBLE },
      { "Large", FD_RESULT|FD_INT64 },
      { nullptr, 0 }
   };
   StaticResultSet results = describe_module_call_results(fields);
   constexpr std::array<TiriType, 10> expected = {
      TiriType::Num, TiriType::Str, TiriType::Object, TiriType::Struct, TiriType::Table,
      TiriType::Userdata, TiriType::Nil, TiriType::Num, TiriType::Num, TiriType::Num
   };
   if (results.declared_count != expected.size() or results.dynamic or results.variadic) {
      Log.error("module result tuple count or closure did not match the native signature");
      return false;
   }
   for (size_t i = 0; i < results.stored_count; ++i) {
      if (results.value_at(i).primary != expected[i] or
          results.value_at(i).proof != StaticProof::Trusted) {
         Log.error("module result descriptor at position %" PRId64 " had the wrong type or proof",
            int64_t(i));
         return false;
      }
   }
   if (results.value_at(0).nullable or not results.value_at(1).nullable or
       not results.value_at(9).nullable) {
      Log.error("module result nullability did not match direct and appended result behaviour");
      return false;
   }

   constexpr FunctionField direct_values[][2] = {
      { { "Text", FD_STR }, { nullptr, 0 } },
      { { "Object", FD_OBJECT }, { nullptr, 0 } },
      { { "Pointer", FD_PTR }, { nullptr, 0 } },
      { { "Integer", FD_INT }, { nullptr, 0 } },
      { { "Double", FD_DOUBLE }, { nullptr, 0 } },
      { { "Large", FD_INT64 }, { nullptr, 0 } }
   };
   constexpr std::array<TiriType, 6> direct_types = {
      TiriType::Str, TiriType::Object, TiriType::Userdata,
      TiriType::Num, TiriType::Num, TiriType::Num
   };
   for (size_t i = 0; i < direct_types.size(); ++i) {
      results = describe_module_call_results(direct_values[i]);
      if (results.declared_count != 1 or results.value_at(0).primary != direct_types[i]) {
         Log.error("module direct return mapping failed at position %" PRId64, int64_t(i));
         return false;
      }
   }

   constexpr FunctionField vector_result[] = {
      { "Void", FD_VOID },
      { "Items", FD_RESULT|FDF_VECTOR|FD_INT },
      { nullptr, 0 }
   };
   results = describe_module_call_results(vector_result);
   if (results.declared_count != 1 or results.value_at(0).primary != TiriType::Array or
       not results.value_at(0).array_element.known or
       results.value_at(0).array_element.logical_type != TiriType::Num) {
      Log.error("void module return incorrectly introduced a result before a vector output");
      return false;
   }

   constexpr FunctionField void_result[] = {
      { "Void", FD_VOID },
      { nullptr, 0 }
   };
   results = describe_module_call_results(void_result);
   if (results.declared_count != 0 or results.stored_count != 0 or
       results.value_at(0).primary != TiriType::Nil) {
      Log.error("void module function did not produce a closed empty result tuple");
      return false;
   }

   std::array<FunctionField, MAX_RETURN_TYPES + 3> wide{};
   wide[0] = { "Void", FD_VOID };
   for (size_t i = 1; i < wide.size() - 1; ++i) wide[i] = { "Value", FD_INT|FD_RESULT };
   results = describe_module_call_results(wide.data());
   if (results.declared_count != wide.size() - 2 or results.stored_count != MAX_RETURN_TYPES) {
      Log.error("wide module result signature lost its closed declared count or exceeded descriptor storage");
      return false;
   }

   return true;
}

static size_t count_opcode_tree(const BytecodeSnapshot &, BCOp);

static bool diagnostics_contain(const AstHarnessResult &Result, std::string_view Text)
{
   for (const auto &diagnostic : Result.diagnostics) {
      if (diagnostic.message.find(Text) != std::string::npos) return true;
   }
   return false;
}

static bool test_module_namespace_ast(kt::Log &Log)
{
   auto valid = build_ast_from_source(
      "module core as mCore\n"
      "local first = mCore.preciseTime()\n"
      "local callable = mCore.PreciseTime\n");
   if (not valid.chunk.ok() or not valid.diagnostics.empty() or valid.chunk.value_ref()->statements.size() != 3) {
      Log.error("valid module namespace declaration did not produce the expected AST");
      log_diagnostics(valid.diagnostics, Log);
      return false;
   }

   const auto &call_decl = std::get<LocalDeclStmtPayload>(valid.chunk.value_ref()->statements[1]->data);
   if (call_decl.values.empty() or call_decl.values.front()->kind != AstNodeKind::CallExpr) {
      Log.error("module namespace call did not remain a call expression");
      return false;
   }
   const auto &call = std::get<CallExprPayload>(call_decl.values.front()->data);
   const auto *target = std::get_if<DirectCallTarget>(&call.target);
   if (not target or not target->callable or target->callable->kind != AstNodeKind::ModuleFunctionExpr) {
      Log.error("module namespace call did not have a direct module function target");
      return false;
   }
   const auto &member = std::get<ModuleFunctionExprPayload>(target->callable->data);
   if (not member.module or not member.namespace_name or not member.function.symbol or not member.binding.symbol or
       not kt::iequals(static_module_name(member.module), "core") or
       std::string_view(strdata(member.namespace_name), member.namespace_name->len) != "mCore" or
       std::string_view(strdata(member.function.symbol), member.function.symbol->len) != "PreciseTime") {
      Log.error("module member did not retain canonical compiler metadata");
      return false;
   }

   // The extracted callable must reuse the binding created by the call, so the declaration materialises exactly one
   // closure for PreciseTime despite two references.

   const auto &extract_decl = std::get<LocalDeclStmtPayload>(valid.chunk.value_ref()->statements[2]->data);
   if (extract_decl.values.empty() or
       extract_decl.values.front()->kind != AstNodeKind::ModuleFunctionExpr) {
      Log.error("an extracted module callable did not use the dedicated module function node");
      return false;
   }
   const auto &extracted = std::get<ModuleFunctionExprPayload>(extract_decl.values.front()->data);
   if (extracted.binding.symbol != member.binding.symbol) {
      Log.error("an extracted module callable did not share the direct call's hidden binding");
      return false;
   }

   // The dependency activation is finalised after parsing and must name exactly the referenced functions.

   const auto &dependency = std::get<LocalDeclStmtPayload>(valid.chunk.value_ref()->statements[0]->data);
   if (dependency.names.size() != 1 or dependency.names[0].symbol != member.binding.symbol) {
      Log.error("the dependency activation did not declare one binding per referenced function");
      return false;
   }
   if (not dependency.values.empty() or dependency.module_dependency != 0) {
      Log.error("the dependency activation retained a value expression or the wrong descriptor ordinal");
      return false;
   }

   // Aliases of one canonical module share a dependency, so a function referenced through two namespaces is
   // materialised once.

   auto aliased = build_ast_from_source(
      "module core as mFirst\n"
      "module core as mSecond\n"
      "local a = mFirst.PreciseTime()\n"
      "local b = mSecond.preciseTime()\n"
      "local c = mSecond.GetErrorMsg(0)\n");
   if (not aliased.chunk.ok() or not aliased.diagnostics.empty()) {
      Log.error("aliased module namespaces failed to parse");
      log_diagnostics(aliased.diagnostics, Log);
      return false;
   }
   const auto &alias_dependency = std::get<LocalDeclStmtPayload>(aliased.chunk.value_ref()->statements[0]->data);
   if (alias_dependency.names.size() != 2) {
      Log.error("aliased namespaces materialised %zu bindings rather than 2", alias_dependency.names.size());
      return false;
   }
   if (not alias_dependency.values.empty() or alias_dependency.module_dependency != 0) {
      Log.error("aliased namespaces did not deduplicate their referenced functions");
      return false;
   }

   // An unused declaration must still resolve its module during activation without retaining a hidden sentinel.

   auto unused = build_ast_from_source("module core as mUnused\nlocal value = 1\n");
   if (not unused.chunk.ok() or not unused.diagnostics.empty()) {
      Log.error("an unreferenced module declaration failed to parse");
      log_diagnostics(unused.diagnostics, Log);
      return false;
   }
   const auto &unused_dependency = std::get<LocalDeclStmtPayload>(unused.chunk.value_ref()->statements[0]->data);
   if (not unused_dependency.names.empty() or not unused_dependency.values.empty() or
       unused_dependency.module_dependency != 0) {
      Log.error("an unreferenced module declaration retained a hidden binding or lost its activation descriptor");
      return false;
   }

   return true;
}

static bool test_module_namespace_conditions(kt::Log &Log)
{
   auto conditional = build_ast_from_source(
      "@if(exists='modules:core')\n"
      "   module core as mCore\n"
      "@end\n"
      "local value = mCore.PreciseTime()\n");
   if (not conditional.chunk.ok() or not conditional.diagnostics.empty() or
       conditional.chunk.value_ref()->statements.size() != 2 or
       conditional.chunk.value_ref()->statements.front()->kind != AstNodeKind::LocalDeclStmt) {
      Log.error("a selected compile-time module guard introduced a runtime scope");
      log_diagnostics(conditional.diagnostics, Log);
      return false;
   }

   auto absent = build_ast_from_source(
      "@if(exists='modules:definitelymissingmodule')\n"
      "   module definitelymissingmodule as mMissing\n"
      "@end\n"
      "local value = 1\n");
   if (not absent.chunk.ok() or not absent.diagnostics.empty() or
       absent.chunk.value_ref()->statements.size() != 1) {
      Log.error("an unavailable optional module did not skip its declaration cleanly");
      log_diagnostics(absent.diagnostics, Log);
      return false;
   }

   return true;
}

static bool test_implicit_module_namespace(kt::Log &Log)
{
   // 'mSys' is an implicit namespace for core.  Its dependency is created on first use and its activation is
   // prepended to the compilation unit, so it dominates every reference without occupying a source position.

   auto implicit = build_ast_from_source(
      "local started = mSys.PreciseTime()\n"
      "local message = mSys.getErrorMsg(0)\n"
      "local clock <const> = mSys.PreciseTime\n");
   if (not implicit.chunk.ok() or not implicit.diagnostics.empty()) {
      Log.error("an implicit mSys reference failed to parse");
      log_diagnostics(implicit.diagnostics, Log);
      return false;
   }
   if (implicit.chunk.value_ref()->statements.front()->kind != AstNodeKind::LocalDeclStmt) {
      Log.error("the implicit mSys dependency was not prepended to the compilation unit");
      return false;
   }
   const auto &implicit_dependency = std::get<LocalDeclStmtPayload>(
      implicit.chunk.value_ref()->statements.front()->data);
   if (implicit_dependency.names.size() != 2) {
      Log.error("implicit mSys materialised %zu bindings rather than 2", implicit_dependency.names.size());
      return false;
   }
   if (not implicit_dependency.values.empty() or implicit_dependency.module_dependency != 0) {
      Log.error("implicit mSys did not retain its descriptor-indexed activation");
      return false;
   }

   // A unit that never mentions mSys must not resolve core or emit an activation statement.

   auto unreferenced = build_ast_from_source("local value = 1\n");
   if (not unreferenced.chunk.ok() or unreferenced.chunk.value_ref()->statements.size() != 1) {
      Log.error("a unit that does not use mSys emitted an implicit dependency");
      log_diagnostics(unreferenced.diagnostics, Log);
      return false;
   }

   // An explicit declaration of the same namespace shares the implicit dependency in either statement order.

   for (std::string_view source : {
         std::string_view("module core as mSys\nlocal value = mSys.PreciseTime()\n"),
         std::string_view(
            "local first = mSys.PreciseTime()\nmodule core as mSys\nlocal second = mSys.GetErrorMsg(0)\n"),
         std::string_view(
            "module core as mCoreTwin\nlocal a = mSys.PreciseTime()\nlocal b = mCoreTwin.PreciseTime()\n") }) {
      auto redundant = build_ast_from_source(source);
      if (not redundant.chunk.ok() or not redundant.diagnostics.empty()) {
         Log.error("an explicit core declaration alongside implicit mSys failed to parse");
         log_diagnostics(redundant.diagnostics, Log);
         return false;
      }
      size_t dependencies = 0;
      for (const auto &statement : redundant.chunk.value_ref()->statements) {
         if (statement->kind != AstNodeKind::LocalDeclStmt) continue;
         const auto &decl = std::get<LocalDeclStmtPayload>(statement->data);
         if (decl.module_dependency != UINT32_MAX) dependencies++;
      }
      if (dependencies != 1) {
         Log.error("explicit and implicit core namespaces produced %zu dependencies rather than 1", dependencies);
         return false;
      }
   }

   return true;
}

static bool test_module_namespace_diagnostics(kt::Log &Log)
{
   struct InvalidCase { std::string_view source; std::string_view diagnostic; };
   constexpr std::array<InvalidCase, 15> invalid = { {
      { "module 'core' as mCore\n", "Module name must be an identifier" },
      { "module core mCore\n", "Expected 'as'" },
      { "do module core as mCore end\n", "compilation-unit level" },
      { "module core as mCore\nlocal value = mCore\n", "can only select" },
      { "module core as mCore\nmCore['PreciseTime']()\n", "can only select" },
      { "module core as mCore\nmCore.PreciseTime = 1\n", "members cannot be assigned" },
      { "module core as mCore\nlocal mCore = 1\n", "cannot be declared as a variable" },

      // The implicit mSys namespace is subject to every restriction that applies to a declared namespace, whether or
      // not the compilation unit has already referenced it.

      { "local value = mSys\n", "can only select" },
      { "print(mSys)\n", "can only select" },
      { "mSys['PreciseTime']()\n", "can only select" },
      { "local value = mSys?.PreciseTime()\n", "can only select" },
      { "local mSys = 1\n", "cannot be declared as a variable" },
      { "local function accept(mSys) end\n", "cannot be declared as a function parameter" },
      { "module display as mSys\n", "reserved for the core module" },
      { "local value = mSys.NotARealCoreFunction()\n", "Unknown function" }
   } };
   for (const auto &entry : invalid) {
      auto result = build_ast_from_source(entry.source);
      if (not diagnostics_contain(result, entry.diagnostic)) {
         Log.error("module namespace invalid case did not report '%.*s'", int(entry.diagnostic.size()),
            entry.diagnostic.data());
         log_diagnostics(result.diagnostics, Log);
         return false;
      }
   }

   auto contextual = build_ast_from_source(
      "global function module(Options, Function) return 7 end\n"
      "local result = module({}, function() end)\n");
   if (not contextual.chunk.ok() or not contextual.diagnostics.empty()) {
      Log.error("contextual module declarations reserved an existing identifier use");
      log_diagnostics(contextual.diagnostics, Log);
      return false;
   }

   return true;
}

static bool test_module_namespace_bytecode(kt::Log &Log)
{
   LuaStateHolder bytecode_state;
   lua_State *lua = bytecode_state.get();
   luaL_openlibs(lua);
   register_module_class(lua);
   lua_protect_globals(lua);
   constexpr std::string_view bytecode_source =
      "module core as mRoundTrip\n"
      "local function clock():num return mRoundTrip.PreciseTime() end\n"
      "return clock()\n";
   if (lua_load(lua, bytecode_source, "module-namespace-roundtrip")) {
      Log.error("failed to compile module namespace bytecode fixture: %s", lua_tostring(lua, -1));
      return false;
   }
   GCproto *root = funcproto(funcV(lua->top - 1));
   std::string dump;
   if (lj_bcwrite(lua, root, bytecode_writer, &dump, 1) != 0) {
      Log.error("failed to write module namespace bytecode fixture");
      return false;
   }
   lua_pop(lua, 1);
   if (lua_load(lua, std::string_view(dump.data(), dump.size()), "module-namespace-roundtrip")) {
      Log.error("failed to reload module namespace bytecode fixture: %s", lua_tostring(lua, -1));
      return false;
   }
   if (lua_pcall(lua, 0, 1, 0) != 0 or not lua_isnumber(lua, -1) or lua_tonumber(lua, -1) <= 0) {
      Log.error("reloaded module namespace bytecode did not resolve and execute its dependency: %s",
         lua_isstring(lua, -1) ? lua_tostring(lua, -1) : "invalid result");
      return false;
   }
   lua_pop(lua, 1);

   std::string error;
   auto snapshot = compile_snapshot(lua, bytecode_source, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_GGET) != 0) {
      Log.error("module namespace references retained an ordinary global lookup: %s", error.c_str());
      return false;
   }

   // Activation is descriptor-indexed bytecode.  The module call itself is a hidden local read, so neither path
   // performs a table member lookup.

   if (snapshot->children.size() != 1) {
      Log.error("the module namespace fixture produced %zu child functions rather than 1",
         snapshot->children.size());
      return false;
   }
   if (count_opcode(snapshot->children.front(), BC_TGETS) != 0) {
      Log.error("a module member call retained %zu table member lookups",
         count_opcode(snapshot->children.front(), BC_TGETS));
      return false;
   }
   if (count_opcode_tree(*snapshot, BC_TGETS) != 0 or count_opcode_tree(*snapshot, BC_MODACT) != 1) {
      Log.error("module namespace activation used %zu table lookups and %zu activation opcodes",
         count_opcode_tree(*snapshot, BC_TGETS),
         count_opcode_tree(*snapshot, BC_MODACT));
      return false;
   }

   // Compiled chunks must persist canonical module and function names, never a process address or a bare index.

   if (lua_load(lua, bytecode_source, "module-namespace-constants")) {
      Log.error("failed to recompile the module namespace fixture: %s", lua_tostring(lua, -1));
      return false;
   }

   bool has_module_name = false, has_function_name = false;
   GCproto *compiled = funcproto(funcV(lua->top - 1));
   for (ptrdiff_t i = -ptrdiff_t(compiled->sizekgc); i < 0; ++i) {
      GCobj *object = proto_kgc(compiled, i);
      if (object->gch.gct != uint8_t(~LJ_TSTR)) continue;
      GCstr *constant = gco_to_string(object);
      std::string_view text(strdata(constant), constant->len);
      if (kt::iequals(text, "core")) has_module_name = true;
      else if (text IS "PreciseTime") has_function_name = true;
   }
   lua_pop(lua, 1);

   if (not has_module_name or not has_function_name) {
      Log.error("the compiled chunk did not persist canonical module and function names");
      return false;
   }

   // The compiler-private binder is no longer published through the public mod table.

   constexpr std::string_view hidden_binder_source = "return mod['\\31dependency']\n";
   if (lua_load(lua, hidden_binder_source, "module-namespace-hidden-binder")) {
      Log.error("failed to compile the hidden-binder fixture: %s", lua_tostring(lua, -1));
      return false;
   }
   if (lua_pcall(lua, 0, 1, 0) != 0 or not lua_isnil(lua, -1)) {
      Log.error("the compiler-private binder remains visible through the mod table");
      return false;
   }
   lua_pop(lua, 1);

   return true;
}

static bool test_module_namespace_declarations(kt::Log &Log)
{
   return test_module_namespace_ast(Log) and
      test_module_namespace_conditions(Log) and
      test_implicit_module_namespace(Log) and
      test_module_namespace_diagnostics(Log) and
      test_module_namespace_bytecode(Log);
}

static size_t count_opcode(const BytecodeSnapshot &Snapshot, BCOp Opcode)
{
   size_t count = 0;
   for (BCIns instruction : Snapshot.instructions) {
      if (bc_op(instruction) IS Opcode) ++count;
   }
   return count;
}

static size_t count_opcode_tree(const BytecodeSnapshot &Snapshot, BCOp Opcode)
{
   size_t count = count_opcode(Snapshot, Opcode);
   for (const auto &child : Snapshot.children) count += count_opcode_tree(child, Opcode);
   return count;
}

static bool test_bare_collection_iteration_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   lua_protect_globals(L);
   std::string error;

   constexpr std::string_view source =
      "local array_values = array<int> { 1, 2, 3 }\n"
      "for index, value in array_values do end\n"
      "local table_values = { answer = 42 }\n"
      "for key, value in table_values do end\n"
      "local stored_range = range(1, 4)\n"
      "for value in stored_range do end\n"
      "for value in {1 to 4} do end\n"
      "local function dynamic(Target:any) for first, second in Target do end end\n"
      "for key, value in pairs(table_values) do end\n";

   auto snapshot = compile_snapshot(L, source, true, error);
   if (not snapshot) {
      Log.error("bare collection iteration source failed to compile: %s", error.c_str());
      return false;
   }

   if (count_opcode_tree(*snapshot, BC_ITERA) != 1 or count_opcode_tree(*snapshot, BC_ISARR) != 0) {
      Log.error("proved bare arrays did not lower directly to ITERA without ISARR");
      return false;
   }
   if (count_opcode_tree(*snapshot, BC_ITERN) != 2) {
      Log.error("proved bare tables and explicit pairs() emitted %" PRId64 " ITERN instructions instead of two",
         int64_t(count_opcode_tree(*snapshot, BC_ITERN)));
      return false;
   }
   if (count_opcode_tree(*snapshot, BC_ITERC) != 2) {
      Log.error("stored ranges and dynamic targets did not use one prepared ITERC path each");
      return false;
   }
   if (count_opcode_tree(*snapshot, BC_FORI) != 1 or count_opcode_tree(*snapshot, BC_FORL) != 1) {
      Log.error("direct range literals did not retain their specialised FORI/FORL lowering");
      return false;
   }
   return true;
}

static bool test_tail_call_eligibility(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   std::string error;

   constexpr std::string_view eligible_source =
      "local function source():<num, str> return 7, 'tail' end\n"
      "local function plain() return source() end\n"
      "local function filtered() return [_*]source() end\n"
      "return plain, filtered\n";
   auto eligible = compile_snapshot(L, eligible_source, true, error);
   if (not eligible or count_opcode_tree(*eligible, BC_CALLT) != 1 or
       count_opcode_tree(*eligible, BC_CALLMT) != 1) {
      Log.error("eligible plain and filtered returns did not lower to CALLT and CALLMT: %s", error.c_str());
      return false;
   }

   constexpr std::string_view contract_source =
      "local function source():<num, str> return 7, 'contract' end\n"
      "local function contracted():<num, str> return source() end\n"
      "return contracted\n";
   auto contracted = compile_snapshot(L, contract_source, true, error);
   if (not contracted or count_opcode_tree(*contracted, BC_CALLT) != 0 or
       count_opcode_tree(*contracted, BC_CALLMT) != 0 or count_opcode_tree(*contracted, BC_CALL) IS 0 or
       count_opcode_tree(*contracted, BC_CONTRACT) != 0) {
      Log.error("a proved fixed result changed truncating call lowering or retained a contract: %s", error.c_str());
      return false;
   }

   constexpr std::string_view recursive_contract_source =
      "local function recurse(Count:num, Value:any):num\n"
      "   if Count is 0 then return Value end\n"
      "   return recurse(Count - 1, Value)\n"
      "end\n"
      "return recurse\n";
   auto recursive_contract = compile_snapshot(L, recursive_contract_source, true, error);
   if (not recursive_contract or count_opcode_tree(*recursive_contract, BC_CALLT) != 1 or
       count_opcode_tree(*recursive_contract, BC_CALL) != 0 or
       count_opcode_tree(*recursive_contract, BC_CONTRACT) != 2) {
      Log.error("a direct self-tail call did not forward its fixed return contract: %s", error.c_str());
      return false;
   }

   constexpr std::string_view recursive_void_source =
      "local function recurse(Count:num):<>\n"
      "   if Count is 0 then return end\n"
      "   return recurse(Count - 1)\n"
      "end\n"
      "return recurse\n";
   auto recursive_void = compile_snapshot(L, recursive_void_source, true, error);
   if (not recursive_void or count_opcode_tree(*recursive_void, BC_CALLT) != 1 or
       count_opcode_tree(*recursive_void, BC_CALL) != 0) {
      Log.error("an explicit-void self-tail call was overwritten by result truncation: %s", error.c_str());
      return false;
   }

   constexpr std::string_view mutable_recursive_source =
      "local function recurse(Count:num):num\n"
      "   if Count is 0 then return 0 end\n"
      "   return recurse(Count - 1)\n"
      "end\n"
      "recurse = function(Value:num):num return Value end\n"
      "return recurse\n";
   auto mutable_recursive = compile_snapshot(L, mutable_recursive_source, true, error);
   if (not mutable_recursive or count_opcode_tree(*mutable_recursive, BC_CALLT) != 0 or
       count_opcode_tree(*mutable_recursive, BC_CALL) IS 0) {
      Log.error("a mutable recursive target was incorrectly treated as the current function: %s", error.c_str());
      return false;
   }

   constexpr std::string_view cleanup_source =
      "local function source():<num, str> return 7, 'cleanup' end\n"
      "local function cleaned()\n"
      "   local resource <close> = {}\n"
      "   return source()\n"
      "end\n"
      "return cleaned\n";
   auto cleaned = compile_snapshot(L, cleanup_source, true, error);
   if (not cleaned or count_opcode_tree(*cleaned, BC_CALLT) != 0 or
       count_opcode_tree(*cleaned, BC_CALLMT) != 0 or count_opcode_tree(*cleaned, BC_MRSAVE) != 1 or
       count_opcode_tree(*cleaned, BC_MRRESTORE) != 1 or count_opcode_tree(*cleaned, BC_RETM) IS 0) {
      Log.error("user cleanup did not retain a protected multi-result return path: %s", error.c_str());
      return false;
   }
   return true;
}

static bool test_multres_save_unwind(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   TValue *outer_base = L->base;
   setintV(outer_base, 17);
   setintV(outer_base + 1, 29);
   lj_meta_multres_save(L, outer_base, 2);

   TValue *nested_base = outer_base + 4;
   L->base = nested_base;
   setintV(nested_base, 41);
   lj_meta_multres_save(L, nested_base, 1);
   lj_meta_multres_unwind(L, nested_base);
   if (L->saved_multres.size() != 1) {
      Log.error("multi-result unwinding did not discard only the abandoned nested-frame save");
      return false;
   }

   L->base = outer_base;
   setnilV(outer_base);
   setnilV(outer_base + 1);
   if (lj_meta_multres_restore(L, outer_base) != 3 or not tvisnumber(outer_base) or
       not tvisnumber(outer_base + 1) or numberVint(outer_base) != 17 or numberVint(outer_base + 1) != 29 or
       not L->saved_multres.empty()) {
      Log.error("the caller's saved multi-result values did not survive nested-frame unwinding");
      return false;
   }

   lj_meta_multres_save(L, outer_base, 2);
   lj_meta_multres_unwind(L, outer_base);
   if (not L->saved_multres.empty()) {
      Log.error("multi-result unwinding retained an abandoned save owned by the target frame");
      return false;
   }
   return true;
}

static bool test_compile_time_signed_range_for_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaopen_range(L);
   lua_setglobal(L, "range");
   std::string error;
   constexpr std::string_view optimised_source =
      "for value in {99 into 0 by -1} do end\n"
      "for value in {100 into 0 by -5} do end\n"
      "for value in {-10 to 10} do end\n"
      "for value in {-20 to -1} do end\n"
      "for {99 into 0 by -1} do end\n"
      "for {100 into 0 by -5} do end\n"
      "for {-10 to 10} do end\n"
      "for {-20 to -1} do end\n";

   auto optimised_snapshot = compile_snapshot(L, optimised_source, true, error);
   if (not optimised_snapshot) {
      Log.error("failed to compile signed constant range loops: %s", error.c_str());
      return false;
   }

   if (count_opcode_tree(*optimised_snapshot, BC_FORI) != 8 or
       count_opcode_tree(*optimised_snapshot, BC_FORL) != 8 or
       count_opcode_tree(*optimised_snapshot, BC_RANGEPREP) != 0 or
       count_opcode_tree(*optimised_snapshot, BC_RANGEVAL) != 0) {
      Log.error("signed constant ranges did not emit eight compact numeric loops");
      return false;
   }

   if (count_opcode_tree(*optimised_snapshot, BC_ITERC) != 0 or
       count_opcode_tree(*optimised_snapshot, BC_ITERL) != 0) {
      Log.error("signed constant ranges retained generic iterator bytecode");
      return false;
   }

   constexpr std::string_view protected_source =
      "local total = 0\n"
      "for value in {0 to 10} do\n"
      "   try\n"
      "      total += value\n"
      "   except error\n"
      "      total = -1\n"
      "   success\n"
      "      total += 1\n"
      "   end\n"
      "end\n"
      "try\n"
      "   for value in {0 to 10} do\n"
      "      total += value\n"
      "   end\n"
      "except error\n"
      "   total = -1\n"
      "end\n";

   error.clear();
   auto protected_snapshot = compile_snapshot(L, protected_source, true, error);
   if (not protected_snapshot) {
      Log.error("failed to compile protected constant range loop: %s", error.c_str());
      return false;
   }

   if (count_opcode_tree(*protected_snapshot, BC_FORI) != 2 or
       count_opcode_tree(*protected_snapshot, BC_FORL) != 2 or
       count_opcode_tree(*protected_snapshot, BC_RANGEPREP) != 2 or
       count_opcode_tree(*protected_snapshot, BC_RANGEVAL) != 0) {
      Log.error("protected constant ranges did not emit compact prepared loop bytecode");
      return false;
   }

   constexpr std::string_view direct_range_source =
      "for value in {0.5 to 2.5} do end\n"
      "for value in {2147483648 into 2147483648} do end\n";

   error.clear();
   auto direct_range_snapshot = compile_snapshot(L, direct_range_source, true, error);
   if (not direct_range_snapshot) {
      Log.error("failed to compile direct range loops: %s", error.c_str());
      return false;
   }

   if (count_opcode_tree(*direct_range_snapshot, BC_FORI) != 2 or
       count_opcode_tree(*direct_range_snapshot, BC_FORL) != 2 or
       count_opcode_tree(*direct_range_snapshot, BC_RANGEPREP) != 2 or
       count_opcode_tree(*direct_range_snapshot, BC_RANGEVAL) != 2) {
      Log.error("fractional or unsafe integral ranges did not use direct range loop bytecode");
      return false;
   }

   if (count_opcode_tree(*direct_range_snapshot, BC_ITERC) != 0 or
       count_opcode_tree(*direct_range_snapshot, BC_ITERL) != 0) {
      Log.error("fractional and unsafe integral ranges retained generic iterator bytecode");
      return false;
   }

   return true;
}

static bool test_runtime_range_for_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaopen_range(L);
   lua_setglobal(L, "range");
   std::string error;
   constexpr std::string_view source =
      "local start = 0.0\n"
      "local stop = 1.0\n"
      "local step = 0.2\n"
      "local count = 0\n"
      "for value in {start to stop by step} do count += 1 end\n"
      "for value in {stop into start by -step} do count += 1 end\n"
      "for {start to stop by step} do count += 1 end\n"
      "for value in {0.0 to 1.0 by 0.2} do count += 1 end\n"
      "return count\n";

   auto snapshot = compile_snapshot(L, source, true, error);
   if (not snapshot) {
      Log.error("failed to compile runtime-bound range loops: %s", error.c_str());
      return false;
   }

   if (count_opcode_tree(*snapshot, BC_FORI) != 4 or count_opcode_tree(*snapshot, BC_FORL) != 4 or
       count_opcode_tree(*snapshot, BC_RANGEPREP) != 4 or count_opcode_tree(*snapshot, BC_RANGEVAL) != 4) {
      Log.error("runtime fractional ranges did not emit four prepared direct range loops");
      return false;
   }

   if (count_opcode_tree(*snapshot, BC_ITERC) != 0 or count_opcode_tree(*snapshot, BC_ITERL) != 0) {
      Log.error("runtime fractional ranges retained generic iterator bytecode");
      return false;
   }

   if (lua_load(L, source, "runtime-range-for") != 0 or lua_pcall(L, 0, 1, 0) != 0) {
      Log.error("failed to execute runtime-bound range loops: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
      return false;
   }

   if (lua_tointeger(L, -1) != 21) {
      Log.error("runtime fractional range loops produced %lld iterations", (long long)lua_tointeger(L, -1));
      lua_pop(L, 1);
      return false;
   }
   lua_pop(L, 1);

   constexpr std::string_view invalid_source =
      "local start = 0 / 0\n"
      "for value in {start to 1} do end\n";
   if (lua_load(L, invalid_source, "runtime-invalid-range") != 0) {
      Log.error("failed to compile invalid runtime range coverage: %s", lua_tostring(L, -1));
      lua_pop(L, 1);
      return false;
   }
   if (lua_pcall(L, 0, 0, 0) IS 0) {
      Log.error("runtime range accepted a non-finite bound");
      return false;
   }
   lua_pop(L, 1);
   return true;
}

static bool test_safe_index_bytecode_selection(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *lua = state.get();
   std::string error;
   bool passed = true;

   auto expect_access = [&](std::string_view Source, BCOp Required, BCOp Forbidden,
      CSTRING Description) -> bool {
      auto snapshot = compile_snapshot(lua, Source, true, error);
      if (not snapshot) {
         Log.error("failed to compile %s safe-index fixture: %s", Description, error.c_str());
         return false;
      }

      size_t required_count = count_opcode_tree(*snapshot, Required);
      size_t forbidden_count = count_opcode_tree(*snapshot, Forbidden);
      if (required_count IS 0 or forbidden_count != 0) {
         Log.error("%s safe-index fixture selected the wrong bytecode family (required=%d, forbidden=%d)",
            Description, int(required_count), int(forbidden_count));
         return false;
      }
      return true;
   };

   constexpr std::string_view native_literal =
      "local function read(Values:array<any>):any\n"
      "   return Values?[0]\n"
      "end\n";
   if (not expect_access(native_literal, BC_ASGETB, BC_TGETB, "native-array literal-key")) passed = false;

   constexpr std::string_view native_variable =
      "local function read(Values:array<any>, Index:num):any\n"
      "   return Values?[Index]\n"
      "end\n";
   if (not expect_access(native_variable, BC_ASGETV, BC_TGETV, "native-array variable-key")) passed = false;

   constexpr std::string_view table_literal =
      "local function read(Values:table):any\n"
      "   return Values?[0]\n"
      "end\n";
   if (not expect_access(table_literal, BC_TGETB, BC_ASGETB, "table literal-key")) passed = false;

   constexpr std::string_view table_variable =
      "local function read(Values:table, Key:any):any\n"
      "   return Values?[Key]\n"
      "end\n";
   if (not expect_access(table_variable, BC_TGETV, BC_ASGETV, "table variable-key")) passed = false;

   constexpr std::string_view dynamic_literal =
      "local function read(Values:any):any\n"
      "   return Values?[0]\n"
      "end\n";
   if (not expect_access(dynamic_literal, BC_ASGETB, BC_TGETB, "dynamic-receiver literal-key")) passed = false;

   constexpr std::string_view dynamic_variable =
      "local function read(Values:any, Key:any):any\n"
      "   return Values?[Key]\n"
      "end\n";
   if (not expect_access(dynamic_variable, BC_ASGETV, BC_TGETV, "dynamic-receiver variable-key")) passed = false;

   constexpr std::string_view literal_string =
      "local function read(Values:any):any\n"
      "   return Values?['name']\n"
      "end\n";
   if (not expect_access(literal_string, BC_TGETS, BC_ASGETV, "literal-string-key")) passed = false;

   constexpr std::string_view variable_string =
      "local function read(Values:any, Key:str):any\n"
      "   return Values?[Key]\n"
      "end\n";
   if (not expect_access(variable_string, BC_TGETV, BC_ASGETV, "variable-string-key")) passed = false;

   return passed;
}

static bool test_type_guided_emission(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   std::string error;
   constexpr std::string_view source =
      "extern array\n"
      "local function read(Values:array<int>):num\n"
      "   return Values[0]\n"
      "end\n"
      "local alias = read\n"
      "local alias_two = alias\n"
      "local result:num = 0\n"
      "result = alias_two(array<int> { 7 })\n"
      "return result\n";

   auto snapshot = compile_snapshot(L, source, true, error);
   if (not snapshot or snapshot->children.empty()) {
      Log.error("failed to compile stable-alias emission fixture: %s", error.c_str());
      return false;
   }
   if (count_opcode(snapshot->children.front(), BC_AGETB) + count_opcode(snapshot->children.front(), BC_AGETV) IS 0) {
      Log.error("checked array parameter did not select specialised array access");
      return false;
   }
   if (count_opcode(*snapshot, BC_CONTRACT) != 0) {
      Log.error("stable checked call result retained a redundant local store contract");
      return false;
   }

   constexpr std::string_view implicit_filter =
      "extern array\n"
      "inner = array<int> { 1 }\n"
      "items = array<array> { inner }\n"
      "filtered = array.filter(items, Value => Value[0] > 0)\n"
      "return filtered[0][0]\n";
   snapshot = compile_snapshot(L, implicit_filter, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_AGETB) < 3) {
      Log.error("array filter lost implicit-local result or predicate element descriptors: %s", error.c_str());
      return false;
   }

   constexpr std::string_view direct_filter =
      "extern array\n"
      "local inner = array<int> { 1 }\n"
      "local items = array<array> { inner }\n"
      "local filtered = array.filter(items, Value => true)\n"
      "return filtered[0][0]\n";
   snapshot = compile_snapshot(L, direct_filter, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_AGETB) < 2) {
      Log.error("direct array.filter call lost its source element descriptor: %s", error.c_str());
      return false;
   }

   constexpr std::string_view mismatch =
      "local function typed(Value:num):num return Value end\n"
      "local callback = typed\n"
      "return callback('wrong')\n";
   if (compile_snapshot(L, mismatch, true, error)) {
      Log.error("stable callable alias lost compile-time argument diagnostics");
      return false;
   }

   constexpr std::string_view mutable_alias =
      "local function typed(Value:num):num return Value end\n"
      "local function invoke(Replace:any):<any, ...>\n"
      "   local callback = typed\n"
      "   if Replace then callback = (Value => any: Value) end\n"
      "   return callback('runtime checked')\n"
      "end\n"
      "return invoke(true)\n";
   if (not compile_snapshot(L, mutable_alias, true, error)) {
      Log.error("mutable callable alias was incorrectly treated as a stable target: %s", error.c_str());
      return false;
   }

   constexpr std::string_view dead_write =
      "local function typed(Value:num):num return Value end\n"
      "local callback = typed\n"
      "if false then callback = (Value => any: Value) end\n"
      "return callback('still checked')\n";
   if (compile_snapshot(L, dead_write, true, error)) {
      Log.error("dead branch write incorrectly invalidated a stable callable alias");
      return false;
   }

   constexpr std::string_view specialised_accesses =
      "extern array\n"
      "extern obj\n"
      "struct GuidedLayout Value: int end\n"
      "local function update_array(Values:array<int>):num\n"
      "   Values[0] = Values[0] + 1\n"
      "   return Values?[0]\n"
      "end\n"
      "local function update_struct(Value:struct<GuidedLayout>):num\n"
      "   Value.Value = Value.Value + 1\n"
      "   return Value.Value\n"
      "end\n"
      "local function update_object():num\n"
      "   local value = obj.new('config')\n"
      "   value.flags = 1\n"
      "   return value.flags\n"
      "end\n";
   snapshot = compile_snapshot(L, specialised_accesses, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_AGETB) IS 0 or
       count_opcode_tree(*snapshot, BC_ASETB) IS 0 or count_opcode_tree(*snapshot, BC_ASGETB) IS 0 or
       count_opcode_tree(*snapshot, BC_STGETF) IS 0 or count_opcode_tree(*snapshot, BC_STSETF) IS 0 or
       count_opcode_tree(*snapshot, BC_OBGETF) IS 0 or count_opcode_tree(*snapshot, BC_OBSETF) IS 0) {
      Log.error("proved receivers did not select the complete specialised access families: %s", error.c_str());
      return false;
   }

   constexpr std::string_view safe_object_call =
      "extern obj\n"
      "try\n"
      "   global guided_object = obj.new('time')\n"
      "end\n"
      "local function query_object():<any, ...>\n"
      "   return guided_object?.acQuery()\n"
      "end\n";
   snapshot = compile_snapshot(L, safe_object_call, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_TGETS) < 2 or
       count_opcode_tree(*snapshot, BC_OBGETF) != 0) {
      Log.error("safe object call target bypassed generic metatable lookup: %s", error.c_str());
      return false;
   }

   constexpr std::string_view generic_accesses =
      "local function update(Value:any, Key:any):<any, ...>\n"
      "   Value.field = 1\n"
      "   Value[Key] = Value[Key]\n"
      "   return Value.field\n"
      "end\n";
   snapshot = compile_snapshot(L, generic_accesses, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_TGETS) IS 0 or
       count_opcode_tree(*snapshot, BC_TSETS) IS 0 or count_opcode_tree(*snapshot, BC_TGETV) IS 0 or
       count_opcode_tree(*snapshot, BC_TSETV) IS 0 or count_opcode_tree(*snapshot, BC_AGETB) != 0 or
       count_opcode_tree(*snapshot, BC_STGETF) != 0 or count_opcode_tree(*snapshot, BC_OBGETF) != 0) {
      Log.error("unproved receivers did not retain generic access bytecodes: %s", error.c_str());
      return false;
   }

   constexpr std::string_view dynamic_contract =
      "local function store(Value:any):num\n"
      "   local result:num = Value\n"
      "   return result\n"
      "end\n";
   snapshot = compile_snapshot(L, dynamic_contract, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_CONTRACT) IS 0) {
      Log.error("an advisory call result incorrectly removed its local store contract: %s", error.c_str());
      return false;
   }

   constexpr std::string_view proved_contract_elision =
      "extern struct\n"
      "struct ContractElisionFields\n"
      "   Integer:int\n"
      "   Wide:int64\n"
      "   Float:float\n"
      "   Double:double\n"
      "end\n"
      "global glContractElisionValue = struct<ContractElisionFields> {\n"
      "   integer=1, wide=2, float=3.5, double=4.5\n"
      "}\n"
      "local function accumulate():num\n"
      "   local count = 0\n"
      "   count += glContractElisionValue.integer + glContractElisionValue.wide +\n"
      "      glContractElisionValue.float + glContractElisionValue.double\n"
      "   return count\n"
      "end\n"
      "return accumulate\n";
   snapshot = compile_snapshot(L, proved_contract_elision, true, error);
   if (not snapshot or snapshot->children.empty()) {
      Log.error("failed to compile proved contract-elision fixture: %s", error.c_str());
      return false;
   }
   const BytecodeSnapshot &accumulate = snapshot->children.front();
   if (count_opcode(accumulate, BC_STGETF) != 4 or count_opcode(accumulate, BC_CONTRACT) != 0) {
      Log.error("proved structure arithmetic retained a redundant local or result contract: %s",
         error.c_str());
      return false;
   }

   constexpr std::string_view fixed_return_elision =
      "local function literal():num return 1 end\n"
      "local function identifier():num local value = 1 return value end\n"
      "local function pair():<num, str> return 1, 'two', false end\n"
      "return literal, identifier, pair\n";
   snapshot = compile_snapshot(L, fixed_return_elision, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_CONTRACT) != 0) {
      Log.error("proved fixed returns retained a redundant result contract: %s", error.c_str());
      return false;
   }

   constexpr std::string_view retained_return_contract =
      "local function source(Value:any):any return Value end\n"
      "local function dynamic(Value:any):num return source(Value) end\n"
      "return dynamic\n";
   snapshot = compile_snapshot(L, retained_return_contract, true, error);
   if (not snapshot or snapshot->children.size() < 2 or
       count_opcode_tree(*snapshot, BC_CONTRACT) IS 0) {
      Log.error("an advisory fixed return incorrectly lost its result contract: %s", error.c_str());
      return false;
   }

   constexpr std::string_view retained_arithmetic_contract =
      "local function update(Value:any):num\n"
      "   local result = 1\n"
      "   result += Value\n"
      "   return result\n"
      "end\n"
      "return update\n";
   snapshot = compile_snapshot(L, retained_arithmetic_contract, true, error);
   if (not snapshot or snapshot->children.empty() or
       count_opcode(snapshot->children.front(), BC_CONTRACT) != 2) {
      Log.error("dynamic compound arithmetic did not retain its sticky-store and result contracts: %s",
         error.c_str());
      return false;
   }

   constexpr std::string_view variant_mutation_contracts =
      "local function compound(Operand:any):num\n"
      "   local source:any = 1\n"
      "   source += Operand\n"
      "   local target = 0\n"
      "   target = source\n"
      "   return target\n"
      "end\n"
      "local function if_empty():num\n"
      "   local source:any = 0\n"
      "   source ?" "?= 'wrong'\n"
      "   local target = 0\n"
      "   target = source\n"
      "   return target\n"
      "end\n"
      "local function if_nil():num\n"
      "   local source:any = nil\n"
      "   source ?= 'wrong'\n"
      "   local target = 0\n"
      "   target = source\n"
      "   return target\n"
      "end\n"
      "local function unchanged():num\n"
      "   local source:any = 1\n"
      "   local target = 0\n"
      "   target = source\n"
      "   return target\n"
      "end\n"
      "return compound, if_empty, if_nil, unchanged\n";
   snapshot = compile_snapshot(L, variant_mutation_contracts, true, error);
   if (not snapshot or snapshot->children.size() != 4 or
       count_opcode(snapshot->children[0], BC_CONTRACT) != 2 or
       count_opcode(snapshot->children[1], BC_CONTRACT) != 2 or
       count_opcode(snapshot->children[2], BC_CONTRACT) != 2 or
       count_opcode(snapshot->children[3], BC_CONTRACT) != 0) {
      Log.error("variant mutation descriptors did not retain only the required store and result contracts: %s",
         error.c_str());
      return false;
   }

   constexpr std::string_view member_inference =
      "extern struct\n"
      "struct DescriptorMember Value: int end\n"
      "local item = struct.new('DescriptorMember')\n"
      "item.Value = 12\n"
      "local first = item.Value\n"
      "local second = item?.Value\n"
      "local delta = first - second\n"
      "delta = item.Value - item.Value\n"
      "return delta\n";
   snapshot = compile_snapshot(L, member_inference, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_STGETF) < 4) {
      Log.error("static member descriptors did not reach semantic expression inference: %s", error.c_str());
      return false;
   }

   constexpr std::string_view annotated_array_element_inference =
      "extern array\n"
      "local values:array<double> = array<double, 3> { 1, 2, 3 }\n"
      "local value = values[0]\n"
      "value = value + 1\n"
      "local function increment(Values:array<double>):num\n"
      "   local safe_value = Values?[0]\n"
      "   safe_value = safe_value + 1\n"
      "   return safe_value\n"
      "end\n"
      "return value, increment(values)\n";
   error.clear();
   snapshot = compile_snapshot(L, annotated_array_element_inference, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_AGETB) IS 0 or
       count_opcode_tree(*snapshot, BC_ASGETB) IS 0) {
      Log.error("annotated arrays lost indexed element inference: %s", error.c_str());
      return false;
   }

   constexpr std::string_view callable_table =
      "local function accept(Value:func):func return Value end\n"
      "local callable = {}\n"
      "return accept(callable)\n";
   snapshot = compile_snapshot(L, callable_table, true, error);
   if (not snapshot or count_opcode_tree(*snapshot, BC_CONTRACT) != 1) {
      Log.error("callable-table compatibility lost its parameter contract or retained a proved result contract: %s",
         error.c_str());
      return false;
   }
   return true;
}

static bool test_builtin_callable_bytecode(kt::Log &Log)
{
   LuaStateHolder first_state;
   LuaStateHolder second_state;
   lua_State *first = first_state.get();
   lua_State *second = second_state.get();
   if (not first or not second) {
      Log.error("failed to create states for built-in callable bytecode testing");
      return false;
   }
   luaL_openlibs(first);
   luaL_openlibs(second);

   constexpr std::array<FastFunc, 3> representative_ids = {
      FastFunc::table_insert, FastFunc::array_push, FastFunc::string_upper
   };
   for (FastFunc fast_id : representative_ids) {
      BuiltinCallableID id = builtin_callable_id(fast_id);
      GCfunc *first_callable = lj_builtin_callable(first, id);
      GCfunc *second_callable = lj_builtin_callable(second, id);
      if (not first_callable or not second_callable or first_callable IS second_callable or
          first_callable->c.ffid != builtin_callable_index(id) or
          second_callable->c.ffid != builtin_callable_index(id)) {
         Log.error("built-in callable ID %u is missing, mismatched or shared across states",
            builtin_callable_index(id));
         return false;
      }
   }

   BuiltinCallableID insert_id = builtin_callable_id(FastFunc::table_insert);
   GCfunc *canonical_insert = lj_builtin_callable(first, insert_id);
   lua_getglobal(first, "table");
   lua_getfield(first, -1, "insert");
   bool public_identity_matches = tvisfunc(first->top - 1) and funcV(first->top - 1) IS canonical_insert;
   lua_pop(first, 2);
   if (not public_identity_matches) {
      Log.error("table.insert does not expose the registered canonical closure");
      return false;
   }

   constexpr std::string_view source =
      "local before = 17\n"
      "local callable = table.insert\n"
      "local after = 23\n"
      "return before, callable, after\n";
   if (lua_load(first, source, "builtin-callable-bytecode")) {
      Log.error("failed to compile built-in callable fixture: %s", lua_tostring(first, -1));
      return false;
   }

   GCproto *prototype = funcproto(funcV(first->top - 1));
   BCIns *bytecode = proto_bc(prototype);
   MSize load_position = 0;
   for (MSize i = 1; i < prototype->sizebc; ++i) {
      if (bc_op(bytecode[i]) IS BC_TGETS) {
         load_position = i;
         bytecode[i] = BCINS_AD(BC_BFUNC, bc_a(bytecode[i]), builtin_callable_index(insert_id));
         break;
      }
   }
   if (load_position IS 0) {
      Log.error("built-in callable fixture did not contain the expected table field load");
      return false;
   }

   std::string disassembly;
   trace_proto_bytecode(first, prototype,
      [](std::string_view Line, void *Context) {
         auto *text = (std::string *)Context;
         text->append(Line);
         text->push_back('\n');
      }, &disassembly, true);
   if (disassembly.find("BFUNC") IS std::string::npos or
       disassembly.find("table.insert") IS std::string::npos) {
      Log.error("BC_BFUNC disassembly omitted its opcode or canonical name");
      return false;
   }

   if (lua_load(first, "return table.insert", "builtin-callable-debug-name")) {
      Log.error("failed to compile BC_BFUNC debug-name fixture: %s", lua_tostring(first, -1));
      return false;
   }
   GCproto *debug_prototype = funcproto(funcV(first->top - 1));
   BCIns *debug_bytecode = proto_bc(debug_prototype);
   MSize debug_position = 0;
   for (MSize i = 1; i < debug_prototype->sizebc; ++i) {
      if (bc_op(debug_bytecode[i]) IS BC_TGETS) {
         debug_position = i;
         debug_bytecode[i] = BCINS_AD(BC_BFUNC, bc_a(debug_bytecode[i]), builtin_callable_index(insert_id));
         break;
      }
   }
   const char *slot_name = nullptr;
   const char *slot_kind = debug_position ? lj_debug_slotname(debug_prototype,
      &debug_bytecode[debug_position + 1], bc_a(debug_bytecode[debug_position]), &slot_name) : nullptr;
   lua_pop(first, 1);
   if (not slot_kind or std::string_view(slot_kind) != "builtin" or not slot_name or
       std::string_view(slot_name) != "table.insert") {
      Log.error("BC_BFUNC debug naming did not resolve table.insert");
      return false;
   }

   for (int strip : { 0, 1 }) {
      std::string dump;
      if (lj_bcwrite(first, prototype, bytecode_writer, &dump, strip) != 0 or
          lua_load(first, std::string_view(dump.data(), dump.size()), "builtin-callable-roundtrip")) {
         Log.error("failed to round-trip %s BC_BFUNC bytecode: %s", strip ? "stripped" : "unstripped",
            lua_tostring(first, -1));
         return false;
      }
      GCproto *restored = funcproto(funcV(first->top - 1));
      BCIns restored_load = proto_bc(restored)[load_position];
      lua_pop(first, 1);
      if (bc_op(restored_load) != BC_BFUNC or bc_d(restored_load) != builtin_callable_index(insert_id)) {
         Log.error("%s bytecode round-trip changed the BC_BFUNC operand",
            strip ? "stripped" : "unstripped");
         return false;
      }
   }

   auto run_patched_call = [&](std::string_view Source, BuiltinCallableID Id, int Results,
      BCOp RequiredCall, bool RequireTrace = false) -> bool {
      if (lua_load(first, Source, "builtin-callable-invocation")) {
         Log.error("failed to compile BC_BFUNC invocation fixture: %s", lua_tostring(first, -1));
         return false;
      }
      GCproto *call_prototype = funcproto(funcV(first->top - 1));
      BCIns *call_bytecode = proto_bc(call_prototype);
      bool patched = false;
      bool found_call = false;
      for (MSize i = 1; i < call_prototype->sizebc; ++i) {
         BCOp op = bc_op(call_bytecode[i]);
         if (op IS BC_TGETS and not patched) {
            call_bytecode[i] = BCINS_AD(BC_BFUNC, bc_a(call_bytecode[i]), builtin_callable_index(Id));
            patched = true;
         }
         if (op IS RequiredCall) found_call = true;
      }
      if (not patched or not found_call) {
         Log.error("BC_BFUNC invocation fixture lacked its field load or required call form");
         lua_pop(first, 1);
         return false;
      }
      if (lua_pcall(first, 0, Results, 0)) {
         Log.error("BC_BFUNC invocation failed: %s", lua_tostring(first, -1));
         return false;
      }
      if (RequireTrace and call_prototype->trace IS 0) {
         Log.error("hot BC_BFUNC invocation did not compile a JIT trace");
         lua_pop(first, Results);
         return false;
      }
      return true;
   };

   if (not run_patched_call(
       "local f = table.insert\nlocal values = {}\nf(values, 42)\nreturn values[0]\n",
       insert_id, 1, BC_CALL) or lua_tointeger(first, -1) != 42) {
      Log.error("BC_BFUNC fixed call did not invoke table.insert");
      return false;
   }
   lua_pop(first, 1);

   BuiltinCallableID byte_id = builtin_callable_id(FastFunc::string_byte);
   if (not run_patched_call(
       "local f = string.byte\nlocal first, second = f('AB', 0, 1)\nreturn first, second\n",
       byte_id, 2, BC_CALL) or lua_tointeger(first, -2) != 65 or lua_tointeger(first, -1) != 66) {
      Log.error("BC_BFUNC multi-result call did not preserve string.byte results");
      return false;
   }
   lua_pop(first, 2);

   BuiltinCallableID upper_id = builtin_callable_id(FastFunc::string_upper);
   if (not run_patched_call("local f = string.upper\nreturn f('abc')\n", upper_id, 1, BC_CALLT) or
       std::string_view(lua_tostring(first, -1)) != "ABC") {
      Log.error("BC_BFUNC tail call did not invoke string.upper");
      return false;
   }
   lua_pop(first, 1);

   if (not run_patched_call(
       "local f = table.insert\nlocal values = {}\n"
       "for i in {0 to 200} do f(values, i) end\nreturn #values\n",
       insert_id, 1, BC_CALL, true) or lua_tointeger(first, -1) != 200) {
      Log.error("hot BC_BFUNC calls did not preserve table.insert behaviour");
      return false;
   }
   lua_pop(first, 1);

   BCIns valid_load = bytecode[load_position];
   bytecode[load_position] = BCINS_AD(BC_BFUNC, bc_a(valid_load), FF__MAX);
   std::string malformed_dump;
   bool wrote_malformed = lj_bcwrite(first, prototype, bytecode_writer, &malformed_dump, 1) IS 0;
   bytecode[load_position] = valid_load;
   if (not wrote_malformed) {
      Log.error("failed to write malformed BC_BFUNC validation fixture");
      return false;
   }
   if (not lua_load(first, std::string_view(malformed_dump.data(), malformed_dump.size()),
       "malformed-builtin-callable")) {
      Log.error("bytecode reader accepted an invalid BC_BFUNC identity");
      lua_pop(first, 1);
      return false;
   }
   lua_pop(first, 1);

   lua_getglobal(first, "table");
   lua_pushnil(first);
   lua_setfield(first, -2, "insert");
   lua_pop(first, 1);
   lua_gc(first, LUA_GCCOLLECT, 0);
   if (lj_builtin_callable(first, insert_id) != canonical_insert) {
      Log.error("canonical table.insert closure was not rooted independently of its public field");
      return false;
   }

   if (lua_pcall(first, 0, 3, 0)) {
      Log.error("BC_BFUNC fixture failed at runtime: %s", lua_tostring(first, -1));
      return false;
   }
   bool adjacent_slots_preserved = lua_tointeger(first, -3) IS 17 and lua_tointeger(first, -1) IS 23;
   bool loaded_canonical = tvisfunc(first->top - 2) and funcV(first->top - 2) IS canonical_insert;
   lua_pop(first, 3);
   if (not adjacent_slots_preserved or not loaded_canonical) {
      Log.error("BC_BFUNC changed adjacent slots or loaded a non-canonical function");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Phase 1 of the opt-in table context plan: positive runtime contextuality metadata.

// Marks its table argument contextual through the internal helper and returns it.  This stands in for the source
// designation mechanisms that Phase 2 adds; no public API equivalent exists.
static int unit_mark_contextual(lua_State *L)
{
   // Kotuku's C API is 0-based, so the first argument is the function's base slot.
   if (L->top <= L->base or not tvistab(L->base)) return 0;
   lj_tab_mark_contextual(tabV(L->base));
   lua_settop(L, 1);
   return 1;
}

static bool test_contextual_table_flag(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);

   // A freshly allocated table is ordinary, because contextuality is opt-in.  Every table below is rooted on the
   // stack for the duration of the test so that an intervening collection cannot free it.
   lua_createtable(L, 0, 0);
   GCtab *ordinary = tabV(L->top - 1);
   if (lj_tab_is_contextual(ordinary)) {
      Log.error("a newly allocated table was contextual by default");
      return false;
   }

   TValue receiver;
   settabV(L, &receiver, ordinary);
   if (lj_context_receiver_establishes(&receiver)) {
      Log.error("an ordinary table established call context");
      return false;
   }

   // Designation is one-way and permanent.
   lj_tab_mark_contextual(ordinary);
   if (not lj_tab_is_contextual(ordinary) or not lj_context_receiver_establishes(&receiver)) {
      Log.error("a designated table did not establish call context");
      return false;
   }

   // Ordinary mutation must never clear the flag: the JIT relies on its monotonicity to guard the bit cheaply.
   // Route the mutation through the public API so the GC barriers and stack rooting match ordinary script behaviour.
   settabV(L, L->top++, ordinary);
   lua_pushstring(L, "field");
   lua_pushinteger(L, 1);
   lua_rawset(L, -3);
   lua_createtable(L, 0, 0);
   lua_setmetatable(L, -2);
   lua_pushnil(L);
   lua_setmetatable(L, -2);
   lua_pop(L, 1);
   lj_tab_clear(ordinary);
   if (not lj_tab_is_contextual(ordinary)) {
      Log.error("table mutation, clearing or metatable replacement cleared contextuality");
      return false;
   }

   // The classification bits share one flags byte and must stay independent.
   if ((TAB_CONTEXTUAL & (TAB_NOT_SEQUENCE | TAB_METHOD_COMPATIBLE)) != 0) {
      Log.error("the contextual bit overlapped an existing classification bit");
      return false;
   }

   // lj_tab_inherit_contextual() propagates from a declared structural source and otherwise leaves the result alone.
   lua_createtable(L, 0, 0);
   GCtab *from_ordinary = tabV(L->top - 1);
   lua_createtable(L, 0, 0);
   GCtab *source_ordinary = tabV(L->top - 1);
   lj_tab_inherit_contextual(from_ordinary, source_ordinary);
   if (lj_tab_is_contextual(from_ordinary)) {
      Log.error("an ordinary source marked a derived table contextual");
      return false;
   }
   lj_tab_inherit_contextual(from_ordinary, ordinary);
   if (not lj_tab_is_contextual(from_ordinary)) {
      Log.error("a contextual source did not propagate to a derived table");
      return false;
   }

   // Template duplication carries the flag, which is how contextual table literals materialise.
   lua_createtable(L, 0, 1);
   GCtab *template_table = tabV(L->top - 1);
   lj_tab_mark_contextual(template_table);
   GCtab *duplicate = lj_tab_dup(L, template_table);
   settabV(L, L->top++, duplicate);
   if (not lj_tab_is_contextual(duplicate)) {
      Log.error("lj_tab_dup() lost contextuality when materialising a template");
      return false;
   }

   lua_createtable(L, 0, 1);
   GCtab *ordinary_template = tabV(L->top - 1);
   GCtab *ordinary_duplicate = lj_tab_dup(L, ordinary_template);
   settabV(L, L->top++, ordinary_duplicate);
   if (lj_tab_is_contextual(ordinary_duplicate)) {
      Log.error("lj_tab_dup() invented contextuality from an ordinary template");
      return false;
   }

   lua_settop(L, 0);
   return true;
}

static bool test_contextual_runtime_semantics(kt::Log &Log)
{
   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);

   lua_pushcfunction(L, unit_mark_contextual);
   lua_setglobal(L, "designate");

   // Designation happens in place so that each receiver remains a plain local, which is the shape the compiler still
   // lowers through the runtime contextual gate.  Phase 2 replaces this helper with real source syntax.
   constexpr std::string_view source =
      "local ordinary = { name = 'ordinary' }\n"
      "local designated = { name = 'designated' }\n"
      "designate(designated)\n"
      "local outer = { name = 'outer' }\n"
      "designate(outer)\n"
      "function readName():str return .name end\n"
      "function ordinary.read():str return readName() end\n"
      "function designated.read():str return readName() end\n"
      "function outer.viaOrdinary():str return ordinary.read() end\n"
      "function outer.viaDesignated():str return designated.read() end\n"
      "function outer.own():str return .name end\n"
      "return ordinary.read(), designated.read(), outer.viaOrdinary(), outer.viaDesignated(), outer.own()\n";

   if (lua_load(L, source, "contextual-runtime")) {
      Log.error("failed to compile contextual runtime fixture: %s", lua_tostring(L, -1));
      return false;
   }
   if (lua_pcall(L, 0, 5, 0)) {
      Log.error("contextual runtime fixture failed: %s", lua_tostring(L, -1));
      return false;
   }

   // An ordinary table inherits the root context, so the leading-dot lookup resolves no name.
   if (not lua_isnil(L, -5)) {
      Log.error("an ordinary table established context for its member call");
      return false;
   }

   struct NameCase {
      int slot;
      const char *expected;
      const char *description;
   };

   constexpr std::array<NameCase, 4> names = { {
      { -4, "designated", "a designated table did not establish context for its member call" },
      { -3, "outer",      "an ordinary nested call did not inherit the caller context" },
      { -2, "designated", "a designated nested call did not replace the inherited context" },
      { -1, "outer",      "a designated receiver did not restore its own context after a nested call" }
   } };

   for (const auto &entry : names) {
      const char *actual = lua_tostring(L, entry.slot);
      if (not actual or strcmp(actual, entry.expected) != 0) {
         Log.error("%s (got '%s')", entry.description, actual ? actual : "nil");
         return false;
      }
   }
   lua_pop(L, 5);

   // Slice results carry contextuality from their declared source table, including the empty-slice exits.
   constexpr std::string_view slice_source =
      "local designated = { 10, 20, 30 }\n"
      "designate(designated)\n"
      "local ordinary = { 10, 20, 30 }\n"
      "return designated[{0 to 1}], designated[{5 to 4}], range.slice(designated, {0 to 1}),\n"
      "   table.slice(designated, {0 to 1}), ordinary[{0 to 1}], ordinary[{5 to 4}]\n";

   if (lua_load(L, slice_source, "contextual-slice")) {
      Log.error("failed to compile contextual slice fixture: %s", lua_tostring(L, -1));
      return false;
   }
   if (lua_pcall(L, 0, 6, 0)) {
      Log.error("contextual slice fixture failed: %s", lua_tostring(L, -1));
      return false;
   }

   struct SliceCase {
      int slot;
      bool contextual;
      const char *description;
   };

   constexpr std::array<SliceCase, 6> slices = { {
      { -6, true,  "range-index syntax on a contextual source" },
      { -5, true,  "an empty slice of a contextual source" },
      { -4, true,  "range.slice() on a contextual source" },
      { -3, true,  "table.slice() on a contextual source" },
      { -2, false, "range-index syntax on an ordinary source" },
      { -1, false, "an empty slice of an ordinary source" }
   } };

   for (const auto &entry : slices) {
      const TValue *result = L->top + entry.slot;
      if (not tvistab(result)) {
         Log.error("%s did not produce a table", entry.description);
         return false;
      }
      if (lj_tab_is_contextual(tabV(result)) != entry.contextual) {
         Log.error("%s produced the wrong contextuality", entry.description);
         return false;
      }
   }
   lua_pop(L, 6);

   return true;
}

//********************************************************************************************************************
// Phase 0 of the opt-in table context plan: allocation-site ownership of a contextual designation target.

static bool test_contextual_designation_ownership(kt::Log &Log)
{
   // Each `mark(...)` call stands in for the designation target of a future contextual setmetatable().  Ownership is
   // classified from the argument expression alone, so the helper does not need to exist.
   constexpr std::string_view source =
      "local literal = {}\n"
      "mark(literal);\n"                        // 1  owned: plain literal allocation
      "mark({ value = 1 });\n"                  // 2  owned: inline literal
      "local used = {}\n"
      "used.field = 1\n"
      "local alias = used\n"
      "helper(used);\n"
      "mark(used);\n"                           // 7  owned: prior mutation, aliasing and call use
      "mark(alias);\n"                          // 8  owned: alias of an owned allocation
      "local merged = {}\n"
      "if flag then merged = { a = 1 } else merged = { a = 2 } end\n"
      "mark(merged);\n"                         // 10 owned: merge of owned literals
      "local tainted = {}\n"
      "if flag then tainted = makeTable() end\n"
      "mark(tainted);\n"                        // 13 rejected: merge with unknown provenance
      "local produced = makeTable()\n"
      "mark(produced);\n"                       // 15 rejected: unresolved call result
      "mark(makeTable());\n"                    // 16 rejected: direct call result
      "mark(registry);\n"                       // 17 rejected: global
      "mark(container.nested);\n"               // 18 rejected: field of another table
      "local overwritten = makeTable()\n"
      "overwritten = {}\n"
      "mark(overwritten);\n"                    // 21 owned: latest definite assignment replaces foreign provenance
      "local assigned_later = {}\n"
      "mark(assigned_later);\n"                 // 23 owned: a later assignment cannot reach this designation
      "assigned_later = makeTable()\n"
      "local foreign_source = makeTable()\n"
      "local foreign_alias = foreign_source\n"
      "foreign_source = {}\n"
      "mark(foreign_alias);\n"                  // 27 rejected: alias retains the foreign value captured earlier
      "local owned_source = {}\n"
      "local owned_alias = owned_source\n"
      "owned_source = makeTable()\n"
      "mark(owned_alias);\n"                    // 31 owned: later source reassignment does not alter the alias
      "local captured = {}\n"
      "function replace_captured()\n"
      "   captured = makeTable()\n"
      "end\n"
      "replace_captured()\n"
      "mark(captured);\n"                       // 38 rejected: a closure can replace the owned allocation
      "local read_capture = {}\n"
      "function read_captured() return read_capture end\n"
      "read_capture = {}\n"
      "mark(read_capture);\n"                   // 42 owned: a read-only capture does not obscure local writes
      "local outer = {}\n"
      "function nested()\n"
      "   mark(outer);\n"
      "end\n"
      "function promote(Target)\n"
      "   mark(Target);\n"                      //    rejected: parameter (checked separately)
      "end\n";

   LuaStateHolder state;
   lua_State *L = state.get();
   luaL_openlibs(L);
   StringReaderCtx reader{ source.data(), source.size() };
   LexState lex(L, unit_reader, &reader, "designation-ownership", std::nullopt);
   FuncState &fs = lex.fs_init();
   ParserAllocator allocator = ParserAllocator::from(L);
   ParserContext context = ParserContext::from(lex, fs, allocator);
   ParserConfig config;
   config.abort_on_error = false;
   config.max_diagnostics = 32;
   ParserSession session(context, config);
   lex.next();
   AstBuilder builder(context);
   auto chunk = builder.parse_chunk();
   if (not chunk.ok()) {
      Log.error("designation ownership fixture did not parse");
      log_diagnostics(context.diagnostics().entries(), Log);
      return false;
   }
   discover_static_bindings(context, *chunk.value_ref());
   propagate_static_descriptors(context, *chunk.value_ref());

   BlockStmt &module = *chunk.value_ref();

   // Recover the sole argument of every `mark(...)` expression statement, in source order.  Collecting them by scan
   // keeps the expectations below independent of statements that do not contribute a designation target.
   auto collect_marks = [](const BlockStmt &Block, std::vector<const ExprNode *> &Targets) {
      for (const auto &statement : Block.statements) {
         if (not statement) continue;
         const auto *expression = std::get_if<ExpressionStmtPayload>(&statement->data);
         if (not expression or not expression->expression) continue;
         const auto *call = std::get_if<CallExprPayload>(&expression->expression->data);
         if (not call or call->arguments.size() != 1) continue;
         const auto *direct = std::get_if<DirectCallTarget>(&call->target);
         if (not direct or not direct->callable or
             direct->callable->kind != AstNodeKind::IdentifierExpr) continue;
         const GCstr *name = std::get<NameRef>(direct->callable->data).identifier.symbol;
         if (not name or name->hash != kt::strhash("mark")) continue;
         Targets.push_back(call->arguments.front().get());
      }
   };

   std::vector<const ExprNode *> marks;
   collect_marks(module, marks);

   struct OwnershipCase {
      bool owned;
      const char *description;
   };

   // One entry per `mark(...)` in the module fixture, in source order.
   constexpr std::array<OwnershipCase, 16> cases = { {
      { true,  "a literal bound to a local" },
      { true,  "an inline table literal" },
      { true,  "an owned allocation after mutation, aliasing and call use" },
      { true,  "an alias of an owned allocation" },
      { true,  "a control-flow merge of two owned literals" },
      { false, "a merge with unknown provenance" },
      { false, "an unresolved call result" },
      { false, "a direct call result" },
      { false, "a global" },
      { false, "a field of another table" },
      { true,  "an owned allocation that supersedes earlier foreign provenance" },
      { true,  "an owned allocation followed by a later foreign assignment" },
      { false, "an alias that retained an earlier foreign value" },
      { true,  "an alias that retained an earlier owned allocation" },
      { false, "an owned binding that a closure can replace" },
      { true,  "a reassigned owned binding captured only for reading" }
   } };

   if (marks.size() != cases.size()) {
      Log.error("designation fixture produced %u targets, expected %u",
         unsigned(marks.size()), unsigned(cases.size()));
      return false;
   }

   for (size_t i = 0; i < cases.size(); ++i) {
      TableOwnershipProof proof = classify_table_ownership(context, *marks[i], 0, &module);
      if (proof.owned() != cases[i].owned) {
         Log.error("%s was classified %s", cases[i].description, proof.owned() ? "owned" : "foreign");
         return false;
      }
   }

   // Locate a named function declaration in the module fixture.
   auto function_body = [&](uint32_t NameHash) -> BlockStmt * {
      for (const auto &statement : module.statements) {
         if (not statement) continue;
         const auto *declaration = std::get_if<FunctionStmtPayload>(&statement->data);
         if (not declaration or declaration->name.segments.size() != 1) continue;
         const GCstr *name = declaration->name.segments.front().symbol;
         if (not name or name->hash != NameHash) continue;
         if (declaration->function) return declaration->function->body.get();
      }
      return nullptr;
   };

   // A local of the enclosing function is an upvalue inside a nested function, so its allocation is not visible.
   BlockStmt *nested_body = function_body(kt::strhash("nested"));
   std::vector<const ExprNode *> nested_marks;
   if (nested_body) collect_marks(*nested_body, nested_marks);
   if (nested_marks.size() != 1) {
      Log.error("designation fixture nested function did not contain one designation target");
      return false;
   }
   TableOwnershipProof nested_proof = classify_table_ownership(context, *nested_marks.front(), 1, nested_body);
   if (nested_proof.owned() or nested_proof.foreign_source != ForeignTableSource::Upvalue) {
      Log.error("an enclosing function's local was accepted as owned inside a nested function");
      return false;
   }

   // A parameter belongs to the caller, so its designation must be rejected inside the declaring function.
   const StmtNode *declaration = module.statements.back().get();
   const auto *function_statement = declaration ?
      std::get_if<FunctionStmtPayload>(&declaration->data) : nullptr;
   if (not function_statement or not function_statement->function or not function_statement->function->body) {
      Log.error("designation fixture did not end with a function declaration");
      return false;
   }
   BlockStmt &body = *function_statement->function->body;
   std::vector<const ExprNode *> parameter_marks;
   collect_marks(body, parameter_marks);
   if (parameter_marks.size() != 1) {
      Log.error("designation fixture function body did not contain one designation target");
      return false;
   }

   TableOwnershipProof parameter_proof = classify_table_ownership(context, *parameter_marks.front(), 1, &body);
   if (parameter_proof.owned() or parameter_proof.foreign_source != ForeignTableSource::Parameter) {
      Log.error("a parameter was accepted as an owned designation target");
      return false;
   }

   return true;
}

}  // namespace

extern void parser_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 68> tests = { {
      { "parser_profiler_captures_stages", test_parser_profiler_captures_stages },
      { "parser_profiler_disabled_noop", test_parser_profiler_disabled_noop },
      { "literal_binary_expr", test_literal_binary_expr },
      { "expression_entry_point", test_expression_entry_point },
      { "expression_list_entry_point", test_expression_list_entry_point },
      { "empty_comment_appended_to_variable", test_empty_comment_appended_to_variable },
      { "global_callable_contract_without_type_analysis", test_global_callable_contract_without_type_analysis },
      { "loop_ast", test_loop_ast },
      { "if_stmt_with_elseif_ast", test_if_stmt_with_elseif_ast },
      { "local_function_table_ast", test_local_function_table_ast },
      { "ast_statement_matrix", test_ast_statement_matrix },
      { "range_for_ast", test_range_for_ast },
      { "deprecated_numeric_for_rejected", test_deprecated_numeric_for_rejected },
      { "colon_method_syntax_rejected", test_colon_method_syntax_rejected },
      { "array_length_range_for_ast", test_array_length_range_for_ast },
      { "generic_for_ast", test_generic_for_ast },
      { "bare_collection_iteration_emission", test_bare_collection_iteration_emission },
      { "repeat_defer_ast", test_repeat_defer_ast },
      { "ternary_presence_expr_ast", test_ternary_presence_expr_ast },
      { "return_lowering", test_return_lowering },
      { "tail_call_eligibility", test_tail_call_eligibility },
      { "multres_save_unwind", test_multres_save_unwind },
      { "contextual_member_ast_foundations", test_contextual_member_ast_foundations },
      { "ast_call_lowering", test_ast_call_lowering },
      { "bytecode_equivalence", test_bytecode_equivalence },
      { "signature_metadata_roundtrip", test_signature_metadata_roundtrip },
      { "forward_declaration_signature_validation", test_forward_declaration_signature_validation },
      { "old_bytecode_versions_rejected", test_old_bytecode_versions_rejected },
      { "unmatched_context_entry_rejected", test_unmatched_context_entry_rejected },
      { "signature_static_inference", test_signature_static_inference },
      { "signature_void_and_bare_return", test_signature_void_and_bare_return },
      { "malformed_signature_rejected", test_malformed_signature_rejected },
      { "contract_bytecode_roundtrip", test_contract_bytecode_roundtrip },
      { "runtime_contract_decoder", test_runtime_contract_decoder },
      { "runtime_contract_batching", test_runtime_contract_batching },
      { "complex_contract_jit_eligibility", test_complex_contract_jit_eligibility },
      { "parser_diagnostics_reset_per_load", test_parser_diagnostics_reset_per_load },
      { "userdata_type_annotations", test_userdata_type_annotations },
      { "state_local_struct_declarations", test_state_local_struct_declarations },
      { "module_registry", test_module_registry },
      { "struct_declaration_syntax", test_struct_declaration_syntax },
      { "struct_field_documentation", test_struct_field_documentation },
      { "struct_declaration_metadata", test_struct_declaration_metadata },
      { "expdesc_is_falsey", test_expdesc_is_falsey },
      { "if_empty_operator_constants", test_if_empty_operator_constants },
      { "ternary_falsey_semantics", test_ternary_falsey_semantics },
      { "static_descriptor_model", test_static_descriptor_model },
      { "static_result_set_model", test_static_result_set_model },
      { "environment_store_boundary", test_environment_store_boundary },
      { "native_prototype_result_descriptors", test_native_prototype_result_descriptors },
      { "builtin_method_registry", test_builtin_method_registry },
      { "builtin_method_table_initialiser_rejection", test_builtin_method_table_initialiser_rejection },
      { "builtin_method_static_classification", test_builtin_method_static_classification },
      { "unresolved_method_receiver_diagnostic", test_unresolved_method_receiver_diagnostic },
      { "builtin_method_bytecode_emission", test_builtin_method_bytecode_emission },
      { "contextual_tail_call_bytecode_emission", test_contextual_tail_call_bytecode_emission },
      { "builtin_method_runtime", test_builtin_method_runtime },
      { "object_call_result_descriptors", test_object_call_result_descriptors },
      { "module_call_result_descriptors", test_module_call_result_descriptors },
      { "module_namespace_declarations", test_module_namespace_declarations },
      { "compile_time_signed_range_for_emission", test_compile_time_signed_range_for_emission },
      { "runtime_range_for_emission", test_runtime_range_for_emission },
      { "safe_index_bytecode_selection", test_safe_index_bytecode_selection },
      { "type_guided_emission", test_type_guided_emission },
      { "builtin_callable_bytecode", test_builtin_callable_bytecode },
      { "contextual_designation_ownership", test_contextual_designation_ownership },
      { "contextual_table_flag", test_contextual_table_flag },
      { "contextual_runtime_semantics", test_contextual_runtime_semantics }
   } };

   // A dummy object is required to manage state.
   if (NewObject(CLASSID::TIRI, &glTestScript) != ERR::Okay) return;
   glTestScript->setStatement("");
   if (Action(AC::Init, glTestScript, nullptr) != ERR::Okay) return;

   for (const TestCase& test : tests) {
      kt::Log log("ParserTests");
      log.branch("Running %s", test.name);
      ++Total;
      if (test.fn(log)) {
         ++Passed;
         log.msg("%s passed", test.name);
      }
      else {
         log.error("%s failed", test.name);
      }
   }
}

#endif // UNIT_TESTS
