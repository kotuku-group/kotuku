// Lua parser
//
// Copyright © 2025-2026 Paul Manias

#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "filesource.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_strfmt.h"
#include "lexer.h"
#include "parser.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "field_type_lookup.h"

#include <kotuku/main.h>

// Priorities for each binary operator. ORDER OPR.

static const struct {
   uint8_t left;      // Left priority.
   uint8_t right;     // Right priority.
   CSTRING name;      // Name for bitlib function (if applicable).
   uint8_t name_len;  // Cached name length for bitlib lookups.
} priority[] = {
  {6,6,nullptr,0}, {6,6,nullptr,0}, {7,7,nullptr,0}, {7,7,nullptr,0}, {7,7,nullptr,0},   // ADD SUB MUL DIV MOD
  {10,9,nullptr,0}, {5,4,nullptr,0},                  // POW CONCAT (right associative)
  {3,3,nullptr,0}, {3,3,nullptr,0},                  // EQ NE
  {3,3,nullptr,0}, {3,3,nullptr,0}, {3,3,nullptr,0}, {3,3,nullptr,0},      // LT GE GT LE
  {5,4,"band",4}, {3,2,"bor",3}, {4,3,"bxor",4}, {7,5,"lshift",6}, {7,5,"rshift",6},   // BAND BOR BXOR SHL SHR (C-style precedence: XOR binds tighter than OR)
  {2,2,nullptr,0}, {1,1,nullptr,0}, {1,1,nullptr,0},         // AND OR IF_EMPTY
  {3,3,"band",4},                     // HAS (flag test: bit.band(a,b) != 0)
  {3,3,nullptr,0},                     // APPROX
  {1,1,nullptr,0}                     // TERNARY
};

#include "dump_bytecode.h"
#include "token_types.h"
#include "parse_types.h"
#include "parse_internal.h"
#include "parser_symbols.h"
#include "parser_profiler.h"
#include "value_categories.h"
#include "../../../defs.h"

#include "token_types.cpp"
#include "token_stream.cpp"
#include "parser_diagnostics.cpp"
#include "parser_context.cpp"
#include "ast/nodes.cpp"
#include "ast/builder.cpp"
#include "parser_symbols.cpp"
#include "parse_control_flow.cpp"
#include "ir_emitter/ir_emitter.cpp"
#include "parse_constants.cpp"
#include "parse_scope.cpp"
#include "parse_regalloc.cpp"
#include "parse_expr.cpp"
#include "ir_emitter/operator_emitter.cpp"
#include "value_categories.cpp"
#include "type_checker.cpp"
#include "type_analysis.cpp"
#include "func_state.cpp"
#include "field_type_lookup.cpp"

static constexpr size_t kMaxLoggedStatements = 12;

static void raise_accumulated_diagnostics(ParserContext &Context)
{
   auto entries = Context.diagnostics().entries();
   if (entries.empty()) return;

   auto summary = std::format("parser reported {} {}:\n", entries.size(), entries.size() IS 1 ? "error" : "errors");

   lua_State *L = &Context.lua();

   for (const auto& diagnostic : entries) {
      SourceSpan span = diagnostic.token.span();
      std::string location = std::format("{}:{}", span.line.lineNumber(), span.column.lineNumber());
      if (not L->file_sources.empty()) {
         const FileSource *src = get_file_source(L, diagnostic.file_index);
         if (src and not src->filename.empty()) location = src->filename + ":" + location;
      }
      if (diagnostic.message.empty()) summary += std::format("   {} - unexpected token\n", location);
      else summary += std::format("   {} - {}\n", location, diagnostic.message);
   }

   // Store diagnostic information in lua_State before throwing
   if (L->parser_diagnostics) delete (ParserDiagnostics*)L->parser_diagnostics;
   L->parser_diagnostics = new ParserDiagnostics(Context.diagnostics());

   GCstr *message = lj_str_new(L, summary.data(), summary.size());
   setstrV(L, L->top++, message);
   lj_err_throw(L, LUA_ERRSYNTAX);
}

//********************************************************************************************************************

static void report_pipeline_error(ParserContext &Context, const ParserError &Error)
{
   Context.emit_error(Error.code, Error.token, Error.message);
}

//********************************************************************************************************************

static void flush_non_fatal_errors(ParserContext &Context)
{
   if (Context.config().abort_on_error) return;
   if (Context.diagnostics().has_errors()) raise_accumulated_diagnostics(Context);
}

//********************************************************************************************************************

static void trace_ast_boundary(ParserContext &Context, const BlockStmt &Chunk, CSTRING Stage)
{
   kt::Log log("AST-Boundary");

   auto script = Context.lua().script;
   if ((script->JitOptions & JOF::TRACE_BOUNDARY) IS JOF::NIL) return;

   StatementListView statements = Chunk.view();
   SourceSpan span = Chunk.span;
   log.branch("[%s]: statements=%" PRId64 " span=%d:%d offset=%" PRId64,
      Stage, statements.size(), span.line.lineNumber(), span.column.lineNumber(), span.offset);

   size_t index = 0;
   for (const StmtNode &stmt : statements) {
      if (index >= kMaxLoggedStatements) {
         log.msg("... truncated after %" PRId64 " statements ...", index);
         break;
      }

      size_t children = ast_statement_child_count(stmt);
      SourceSpan stmt_span = stmt.span;
      log.msg("stmt[%" PRId64 "] kind=%d children=%" PRId64 " span=%d:%d offset=%" PRId64, index,
         int(stmt.kind), children, stmt_span.line.lineNumber(), stmt_span.column.lineNumber(), stmt_span.offset);

      if (stmt.kind IS AstNodeKind::ExpressionStmt) {
         const auto *payload = std::get_if<ExpressionStmtPayload>(&stmt.data);
         if (payload and payload->expression) {
            const ExprNode& expr = *payload->expression;
            size_t expr_children = ast_expression_child_count(expr);
            SourceSpan expr_span = expr.span;
            log.msg("   expr kind=%d children=%" PRId64 " span=%d:%d offset=%" PRId64,
               int(expr.kind), expr_children, expr_span.line.lineNumber(), expr_span.column.lineNumber(), expr_span.offset);
         }
      }

      ++index;
   }
}

//********************************************************************************************************************
// Run the AST-based parsing pipeline.

static void run_ast_pipeline(ParserContext &Context, ParserProfiler &Profiler)
{
   ParserProfiler::StageTimer parse_timer = Profiler.stage("parse");
   AstBuilder builder(Context);

   auto chunk_result = builder.parse_chunk();

   if (not chunk_result.ok()) {
      builder.rollback_registered_enum_constants();
      report_pipeline_error(Context, chunk_result.error_ref());
      flush_non_fatal_errors(Context);
      return;
   }

   std::unique_ptr<BlockStmt> chunk = std::move(chunk_result.value_ref());
   parse_timer.stop();

   if (Context.diagnostics().has_errors() and Context.config().abort_on_error) {
      builder.rollback_registered_enum_constants();
      raise_accumulated_diagnostics(Context);
      return;
   }

   trace_ast_boundary(Context, *chunk, "parse");
   collect_parser_symbols(Context.lua(), *chunk);

   if (Context.config().enable_type_analysis) {
      ParserProfiler::StageTimer type_timer = Profiler.stage("type_analysis");
      run_type_analysis(Context, *chunk);
      type_timer.stop();

      // Raise errors now, required to check for type violations.
      // In diagnose mode (abort_on_error=false), continue to emit to collect more errors.
      if (Context.diagnostics().has_errors() and Context.config().abort_on_error) {
         builder.rollback_registered_enum_constants();
         raise_accumulated_diagnostics(Context);
         return;
      }
   }

   // Emit bytecode instructions

   ParserProfiler::StageTimer emit_timer = Profiler.stage("emit");
   IrEmitter emitter(Context);
   auto emit_result = emitter.emit_chunk(*chunk);
   if (not emit_result.ok()) {
      builder.rollback_registered_enum_constants();
      report_pipeline_error(Context, emit_result.error_ref());
      flush_non_fatal_errors(Context);
      return;
   }

   if (Context.diagnostics().has_errors()) {
      builder.rollback_registered_enum_constants();
      return;
   }

   builder.commit_registered_enum_constants();

   emit_timer.stop();
}

//********************************************************************************************************************

static ParserConfig make_parser_config(lua_State &State)
{
   ParserConfig config;

   if ((State.script->JitOptions & JOF::DIAGNOSE) != JOF::NIL) {
      // Cancel aborting on error and enable deeper log tracing.
      config.abort_on_error = false;
      config.max_diagnostics = 32;
   }

   return config;
}

//********************************************************************************************************************
// Entry point of bytecode parser.

extern GCproto * lj_parse(LexState *State)
{
   kt::Log log("Parser");
   FuncScope bl;
   GCproto   *pt;
   lua_State *L = State->L;

#ifdef LUAJIT_DISABLE_DEBUGINFO
   State->chunk_name = lj_str_newlit(L, "=");
#else
   State->chunk_name = lj_str_newz(L, State->chunk_arg);
#endif

   // Register real file chunks with FileSource tracking.  Synthetic chunks such as "=validate" have no stable path and
   // should fall back to their chunk name in diagnostics rather than being added to the persistent file source map.
   // Note: We don't clear existing file_sources to preserve import deduplication across loadFile() calls.

   if (State->chunk_arg and State->chunk_arg[0] IS '@') {
      std::string path = State->chunk_arg;
      path = path.substr(1);

      // Extract just the filename from the path
      auto pos = path.find_last_of("/\\");
      std::string filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;

      // Estimate source lines from the source view (count newlines + 1)
      BCLine source_lines = 1;
      for (char c : State->source) {
         if (c IS '\n') source_lines++;
      }
      State->current_file_index = register_main_file_source(L, path, filename, source_lines);
   }
   else State->current_file_index = FILESOURCE_OVERFLOW_INDEX;

   log.branch("Chunk: %.*s, Registered: %c", State->chunk_name->len, strdata(State->chunk_name), State->current_file_index IS FILESOURCE_OVERFLOW_INDEX ? 'N' : 'Y');

   setstrV(L, L->top, State->chunk_name);  // Anchor chunk_name string.
   incr_top(L);
   State->level = 0;
   FuncState &fs = State->fs_init();
   fs.linedefined = 0;
   fs.numparams   = 0;
   fs.bcbase      = nullptr;
   fs.bclim       = 0;
   fs.flags      |= PROTO_VARARG;  // Main chunk is always a vararg func.
   fscope_begin(&fs, &bl, FuncScopeFlag::None);
   bcemit_AD(&fs, BC_FUNCV, 0, 0);  // Placeholder.

   ParserAllocator allocator      = ParserAllocator::from(L);
   ParserContext   root_context   = ParserContext::from(*State, fs, allocator);
   ParserConfig    session_config = make_parser_config(*L);

   ParserSession   root_session(root_context, session_config);
   ParserProfiler  profiler((L->script->JitOptions & JOF::PROFILE) != JOF::NIL, &root_context.profiling_result());

   State->next(); // Read-ahead first token.

   run_ast_pipeline(root_context, profiler);

   if ((L->script->JitOptions & JOF::DUMP_BYTECODE) != JOF::NIL) dump_bytecode(root_context.func());

   flush_non_fatal_errors(root_context);

   if (profiler.enabled()) profiler.log_results(log);

   if (State->tok != TK_eof) State->err_token(TK_eof);
   pt = State->fs_finish(State->effective_line());
   L->top--;  // Drop chunk_name.

   // Transfer tips to lua_State for debug.validate() access
   if (State->tip_emitter and State->tip_emitter->has_tip()) {
      L->parser_tips = State->tip_emitter.release();
   }

   lj_assertL(State->func_stack.empty() and State->fs IS nullptr, "mismatched frame nesting");
   lj_assertL(pt->sizeuv IS 0, "toplevel proto has upvalues");
   return pt;
}
