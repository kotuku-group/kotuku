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
#include "lj_bcdump.h"
#include "lj_strfmt.h"
#include "lexer.h"
#include "parser.h"
#include "lj_vm.h"
#include "lj_meta.h"
#include "lj_vmevent.h"
#include "lauxlib.h"
#include "lualib.h"
#include "field_type_lookup.h"
#include "../../../defs.h"
#include "../../../import_module_bundle.h"
#include "tiri_build_identity.h"

#include <kotuku/main.h>

#include <limits>

// Priorities for each binary operator. ORDER OPR.

static const struct {
   uint8_t left;      // Left priority.
   uint8_t right;     // Right priority.
   CSTRING name;      // Name for bitlib function (if applicable).
   uint8_t name_len;  // Cached name length for bitlib lookups.
} priority[] = {
   {9,9,nullptr,0}, {9,9,nullptr,0},                         // ADD SUB
   {10,10,nullptr,0}, {10,10,nullptr,0}, {10,10,nullptr,0},  // MUL DIV MOD
   {12,11,nullptr,0}, {6,5,nullptr,0},                       // POW CONCAT (right associative)
   {3,3,nullptr,0}, {3,3,nullptr,0},                         // EQ NE
   {3,3,nullptr,0}, {3,3,nullptr,0}, {3,3,nullptr,0}, {3,3,nullptr,0}, // LT GE GT LE
   {7,7,"band",4}, {4,4,"bor",3}, {5,5,"bxor",4},        // BAND BOR BXOR
   {8,8,"lshift",6}, {8,8,"rshift",6},                    // SHL SHR
   {2,2,nullptr,0}, {1,1,nullptr,0}, {1,1,nullptr,0},        // AND OR IF_EMPTY
   {3,3,"band",4},                                          // HAS (flag test: bit.band(a,b) != 0)
   {3,3,nullptr,0},                                          // APPROX
   {1,1,nullptr,0}                                           // TERNARY
};

#include "dump_bytecode.h"
#include "token_types.h"
#include "parse_types.h"
#include "parse_internal.h"
#include "parser_symbols.h"
#include "parser_profiler.h"
#include "import_interface_export.h"
#include "assignment_target_resolution.h"
#include "static_type_descriptor.h"
#include "static_descriptor_analysis.h"
#include "value_categories.h"
#include "../../../defs.h"

#include "token_types.cpp"
#include "token_stream.cpp"
#include "parser_diagnostics.cpp"
#include "parser_context.cpp"
#include "static_type_descriptor.cpp"
#include "static_descriptor_analysis.cpp"
#include "table_ownership.cpp"
#include "ast/nodes.cpp"
#include "assignment_target_resolution.cpp"
#include "ast/builder.cpp"
#include "parser_symbols.cpp"
#include "import_interface_export.cpp"
#include "parse_control_flow.cpp"
#include "constant_evaluator.cpp"
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
   Context.emit_error(Error);  // Preserves the error's file_index for errors raised in imported files
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
      builder.rollback_registered_structs();
      report_pipeline_error(Context, chunk_result.error_ref());
      flush_non_fatal_errors(Context);
      return;
   }

   std::unique_ptr<BlockStmt> chunk = std::move(chunk_result.value_ref());
   parse_timer.stop();

   if (Context.diagnostics().has_errors() and Context.config().abort_on_error) {
      builder.rollback_registered_enum_constants();
      builder.rollback_registered_structs();
      raise_accumulated_diagnostics(Context);
      return;
   }

   trace_ast_boundary(Context, *chunk, "parse");
   resolve_assignment_targets(Context, *chunk);
   discover_static_bindings(Context, *chunk);
   // Publish the first descriptor pass before semantic type analysis so dynamic-ingress policy can distinguish
   // genuinely unknown values from concrete native and callable results.  A second pass below refreshes descriptors
   // after type analysis has refined inferred function results.
   propagate_static_descriptors(Context, *chunk);

   if (Context.config().enable_type_analysis) {
      ParserProfiler::StageTimer type_timer = Profiler.stage("type_analysis");
      run_type_analysis(Context, *chunk);
      type_timer.stop();

      // Raise errors now, required to check for type violations.
      // In diagnose mode (abort_on_error=false), continue to emit to collect more errors.
      if (Context.diagnostics().has_errors() and Context.config().abort_on_error) {
         builder.rollback_registered_enum_constants();
         builder.rollback_registered_structs();
         raise_accumulated_diagnostics(Context);
         return;
      }
   }

   propagate_static_descriptors(Context, *chunk);
   std::string interface_diagnostic;
   if (not prepare_import_interfaces(Context, *chunk, interface_diagnostic)) {
      Context.emit_error(ParserErrorCode::InternalInvariant, Token{}, interface_diagnostic);
      raise_accumulated_diagnostics(Context);
      return;
   }
   collect_parser_symbols(Context.lua(), Context.lex(), *chunk);

   // Emit bytecode instructions

   ParserProfiler::StageTimer emit_timer = Profiler.stage("emit");
   IrEmitter emitter(Context);
   auto emit_result = emitter.emit_chunk(*chunk);
   if (not emit_result.ok()) {
      builder.rollback_registered_enum_constants();
      builder.rollback_registered_structs();
      report_pipeline_error(Context, emit_result.error_ref());
      flush_non_fatal_errors(Context);
      return;
   }

   if (Context.diagnostics().has_errors()) {
      builder.rollback_registered_enum_constants();
      builder.rollback_registered_structs();
      return;
   }

   builder.commit_registered_enum_constants();
   builder.commit_registered_structs();

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
      config.warn_unresolved_methods = not State.script->SuppressUnresolvedMethodWarnings;
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

   BCLine source_lines = 1;
   for (char c : State->source) {
      if (c IS '\n') source_lines++;
   }

   if (State->chunk_arg and State->chunk_arg[0] IS '@') {
      std::string path = State->chunk_arg;
      path = path.substr(1);

      // Extract just the filename from the path
      auto pos = path.find_last_of("/\\");
      std::string filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;

      State->current_file_index = register_main_file_source(L, path, filename, source_lines);
      State->compilation_sources.push_back(CompilationSourceRecord{
         .role = CompilationSourceRole::Main,
         .canonical_path = path,
         .display_filename = filename,
         .first_line = 1,
         .total_lines = source_lines,
         .runtime_index = State->current_file_index
      });
   }
   else {
      State->current_file_index = FILESOURCE_SYNTHETIC_INDEX;
      std::string display = State->chunk_arg ? State->chunk_arg : "=(anonymous)";
      State->compilation_sources.push_back(CompilationSourceRecord{
         .role = CompilationSourceRole::Synthetic,
         .display_filename = std::move(display),
         .first_line = 1,
         .total_lines = source_lines,
         .runtime_index = FILESOURCE_SYNTHETIC_INDEX
      });
   }
   State->current_source_descriptor = 0;

   log.branch("Chunk: %.*s, Registered: %c", State->chunk_name->len, strdata(State->chunk_name),
      State->current_file_index < FILESOURCE_SYNTHETIC_INDEX ? 'Y' : 'N');

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

   if (not root_context.config().abort_on_error and not root_context.diagnostics().empty()) {
      if (L->parser_diagnostics) delete (ParserDiagnostics*)L->parser_diagnostics;
      L->parser_diagnostics = new ParserDiagnostics(root_context.diagnostics());
   }

   if (profiler.enabled()) profiler.log_results(log);

   if (State->tok != TK_eof) State->err_token(TK_eof);
   pt = State->fs_finish(State->effective_line());
   setprotoV(L, L->top, pt);
   incr_top(L);
   attach_compilation_sources(L, pt, State->compilation_sources);

   std::vector<uint8_t> struct_manifest;
   std::string manifest_detail;
   ERR manifest_error = build_declared_struct_manifest(L, State->compilation_struct_roots,
      State->compilation_structs, State->dynamic_struct_reference, struct_manifest, &manifest_detail);
   if (manifest_error IS ERR::Okay) {
      auto manifest = (uint8_t *)lj_mem_new(L, MSize(struct_manifest.size()));
      memcpy(manifest, struct_manifest.data(), struct_manifest.size());
      setmref(pt->struct_manifest, manifest);
      pt->struct_manifest_size = uint32_t(struct_manifest.size());
   }
   std::vector<tiri::import_cache::RootModuleRecord> module_records;
   std::vector<GCproto *> module_initialisers;
   module_records.reserve(State->import_module_records.size());
   module_initialisers.reserve(State->import_module_records.size());
   for (const auto &record : State->import_module_records) {
      uint8_t source_index = FILESOURCE_OVERFLOW_INDEX;
      for (size_t i = 0; i < State->compilation_sources.size(); ++i) {
         if (State->compilation_sources[i].runtime_index IS record.source_index) {
            source_index = uint8_t(i);
            break;
         }
      }
      module_records.push_back({
         record.lookup_identity, record.compiled_identity, record.interface_bytes, record.dependencies, source_index
      });
      module_initialisers.push_back(record.initialiser);
   }
   std::vector<tiri::import_cache::RootModuleRecord> canonical_modules;
   if (tiri::import_cache::assemble_root_module_graph(module_records, canonical_modules) !=
       tiri::cache::FormatError::OKAY) {
      luaL_error(L, ERR::InvalidData, "Invalid imported-module executable graph.");
   }
   std::vector<uint32_t> module_mapping(module_records.size(), UINT32_MAX);
   std::vector<GCproto *> canonical_initialisers(canonical_modules.size(), nullptr);
   for (size_t i = 0; i < module_records.size(); ++i) {
      auto found = std::ranges::find(canonical_modules, module_records[i].CompiledIdentity,
         &tiri::import_cache::RootModuleRecord::CompiledIdentity);
      if (found IS canonical_modules.end()) {
         luaL_error(L, ERR::InvalidData, "Invalid imported-module executable graph.");
      }
      const uint32_t index = uint32_t(found - canonical_modules.begin());
      module_mapping[i] = index;
      canonical_initialisers[index] = module_initialisers[i];
   }
   if (not remap_import_module_references(pt, module_mapping)) {
      luaL_error(L, ERR::InvalidData, "Invalid imported-module executable references.");
   }
   for (GCproto *initialiser : canonical_initialisers) {
      if (not initialiser or not remap_import_module_references(initialiser, module_mapping)) {
         luaL_error(L, ERR::InvalidData, "Invalid imported-module executable references.");
      }
   }
   module_records = std::move(canonical_modules);
   module_initialisers = std::move(canonical_initialisers);
   std::string module_bundle;
   if (tiri::import_cache::encode_root_module_bundle(module_records, module_bundle) !=
       tiri::cache::FormatError::OKAY or
       not install_import_module_table(L, pt, module_records, module_initialisers)) {
      luaL_error(L, ERR::InvalidData, "Invalid imported-module executable graph.");
   }
   auto bundle = (uint8_t *)lj_mem_new(L, MSize(module_bundle.size()));
   memcpy(bundle, module_bundle.data(), module_bundle.size());
   setmref(pt->import_module_bundle, bundle);
   pt->import_module_bundle_size = uint32_t(module_bundle.size());
   for (GCproto *linked_root : State->linked_import_module_roots) {
      auto staged_table = linked_root->import_module_table.get<ImportModuleTable>();
      if (not staged_table) continue;
      const uint32_t staged_size = staged_table->byte_size;
      setmref(linked_root->import_module_table, nullptr);
      lj_mem_free(G(L), staged_table, staged_size);
   }
   State->linked_import_module_roots.clear();
   for (int reference : State->import_module_anchors) luaL_unref(L, LUA_REGISTRYINDEX, reference);
   State->import_module_anchors.clear();
   L->top--;
   L->top--;  // Drop chunk_name.

   // Transfer tips to lua_State for debug.validate() access
   if (State->tip_emitter and State->tip_emitter->has_tip()) {
      L->parser_tips = State->tip_emitter.release();
   }

   lj_assertL(State->func_stack.empty() and State->fs IS nullptr, "mismatched frame nesting");
   lj_assertL(pt->sizeuv IS 0, "toplevel proto has upvalues");
   return pt;
}
