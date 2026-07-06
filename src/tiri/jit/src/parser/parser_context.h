#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "parser/parser_diagnostics.h"
#include "parser/token_stream.h"
#include "parser/parser_profiler.h"

#ifdef INCLUDE_TIPS
class TipEmitter;
enum class TipCategory : uint8_t;
#endif

enum class ParserChannel : uint8_t {
   Error,
   Warning,
   Expect,
   Advance
};

//********************************************************************************************************************

struct ParserAllocator {
   void* userdata = nullptr;

   static ParserAllocator from(lua_State* state) {
      ParserAllocator allocator;
      allocator.userdata = (void*)state;
      return allocator;
   }
};

//********************************************************************************************************************

struct ParserConfig {
   uint32_t max_diagnostics = 8; // Defines the 'limit' value in ParserDiagnostics
   bool abort_on_error = true;
   bool enable_type_analysis = true;       // Enable static type checking
   bool type_errors_are_fatal = true;      // Treat type mismatches as errors
   bool infer_local_types = true;          // Track types of local variables
   ParserProfilingResult profiling_result;
};

//********************************************************************************************************************

struct ParserError {
   ParserErrorCode code = ParserErrorCode::None;
   uint8_t file_index = 0;  // FileSource index of the file the error was raised in (token spans store raw lines)
   std::string message;
   Token token;

   // Default constructor
   ParserError() = default;

   // Constructor to simplify error creation (accepts std::string, std::string_view, or const char*)
   ParserError(ParserErrorCode Code, const Token& ErrorToken, std::string_view Message, uint8_t FileIndex = 0)
      : code(Code), file_index(FileIndex), message(Message), token(ErrorToken) {}
};

//********************************************************************************************************************

template<typename T>
class ParserResult {
public:
   ParserResult() : data(ParserError{}) {}

   static ParserResult<T> success(T value) {
      ParserResult<T> result;
      result.data = std::move(value);
      return result;
   }

   static ParserResult<T> failure(const ParserError& error) {
      ParserResult<T> result;
      result.data = error;
      return result;
   }

   [[nodiscard]] bool ok() const { return std::holds_alternative<T>(this->data); }
   [[nodiscard]] const T& value_ref() const { return std::get<T>(this->data); }
   [[nodiscard]] T& value_ref() { return std::get<T>(this->data); }
   [[nodiscard]] const ParserError& error_ref() const { return std::get<ParserError>(this->data); }

private:
   std::variant<T, ParserError> data;
};

//********************************************************************************************************************

class ParserContext {
public:
   using ErrorRollbackCallback = void (*)(void *);

   static ParserContext from(LexState &lex_state, FuncState &func_state, ParserAllocator allocator,
      ParserConfig config = ParserConfig{}) {
      return ParserContext(lex_state, func_state, *lex_state.L, allocator, config);
   }

   ParserContext(LexState &lex_state, FuncState &func_state, lua_State &lua_state,
      ParserAllocator allocator, ParserConfig config)
      : lex_state(&lex_state), func_state(&func_state), lua_state(&lua_state), allocator(allocator)
      , current_config(config), token_stream(lex_state)
   {
      this->diag.set_limit(config.max_diagnostics);
      this->attach_to_lex();
   }

   ParserContext(const ParserContext&) = delete;
   ParserContext& operator=(const ParserContext&) = delete;
   ParserContext& operator=(ParserContext&& other) noexcept;

   ~ParserContext() { this->detach_from_lex(); }

   ParserContext(ParserContext &&other) noexcept
      : lex_state(other.lex_state), func_state(other.func_state)
      , lua_state(other.lua_state), allocator(other.allocator)
      , current_config(other.current_config), diag(std::move(other.diag))
      , token_stream(other.token_stream), previous_context(other.previous_context)
      , error_rollback_callback(other.error_rollback_callback)
      , error_rollback_user_data(other.error_rollback_user_data)
   {
      if (this->lex_state) this->lex_state->active_context = this;
      other.lex_state = nullptr;
      other.func_state = nullptr;
      other.lua_state = nullptr;
      other.previous_context = nullptr;
      other.error_rollback_callback = nullptr;
      other.error_rollback_user_data = nullptr;
   }

   inline LexState & lex() const { return *this->lex_state; }
   inline FuncState & func() const { return *this->func_state; }
   inline lua_State & lua() const { return *this->lua_state; }
#ifdef INCLUDE_TIPS
   inline TipEmitter * tip() const { return this->lex_state->tip_emitter.get(); }

   // Check if tip at the given priority level would be emitted.
   // Use this before performing expensive checks to avoid unnecessary computation.
   [[nodiscard]] bool should_emit_tip(uint8_t Priority) const;

   // Emit tip if the tip system is enabled
   void emit_tip(uint8_t Priority, TipCategory Category, std::string Message, const Token &Location);
#endif
   inline ParserDiagnostics & diagnostics() { return this->diag; }
   inline const ParserDiagnostics & diagnostics() const { return this->diag; }
   inline TokenStreamAdapter & tokens() { return this->token_stream; }
   inline const TokenStreamAdapter & tokens() const { return this->token_stream; }
   inline const ParserConfig & config() const { return this->current_config; }
   inline ParserProfilingResult & profiling_result() { return this->current_config.profiling_result; }
   inline const ParserProfilingResult & profiling_result() const { return this->current_config.profiling_result; }
   inline void override_config(const ParserConfig& config) { this->current_config = config; this->diag.set_limit(config.max_diagnostics); }
   inline void restore_config(const ParserConfig& config) { this->current_config = config; this->diag.set_limit(config.max_diagnostics); }

   void trace_token_advance(const Token& previous, const Token& current) const;

   ParserResult<Token> match(TokenKind kind);
   ParserResult<Token> consume(TokenKind kind, ParserErrorCode code);
   ParserResult<Token> expect_identifier(ParserErrorCode code);
   ParserResult<Token> expect_name(ParserErrorCode code);  // Accepts identifiers or keywords (for field/method names)

   [[nodiscard]] inline bool check(TokenKind kind) const { return this->tokens().current().is(kind); }

   inline void lex_check(LexToken token) { if (not this->lex_opt(token)) this->err_token(token); }

   int lex_opt(LexToken);
   void lex_match(LexToken, LexToken, BCLine);
   GCstr * lex_str();

   void err_syntax(ErrMsg message);
   void err_token(LexToken token);
   void report_limit_error(FuncState &, uint32_t limit, const char *);

   void emit_error(ParserErrorCode code, const Token &, std::string_view);
   void emit_error(const ParserError &);   // Preserves the error's own file_index (e.g. errors from imported files)
   void emit_warning(ParserErrorCode code, const Token &, std::string_view);
   ParserError make_error(ParserErrorCode, const Token &, std::string_view);  // Stamps the lexer's current file_index
   void set_error_rollback_callback(ErrorRollbackCallback, void *);
   void clear_error_rollback_callback(void *);
   void rollback_before_error();

   // Import stack tracking for compile-time import statement
   [[nodiscard]] bool is_importing(const std::string &) const;
   void push_import(const std::string &);
   void pop_import();
   [[nodiscard]] const std::vector<std::string> & import_stack() const { return import_stack_; }
   [[nodiscard]] bool is_being_imported() const { return not import_stack_.empty(); }
   [[nodiscard]] std::string resolve_lib_to_path(std::string_view &) const;

private:
   void attach_to_lex();
   void detach_from_lex();
   std::string format_lex_error(LexToken) const;
   std::string format_expected_message(TokenKind) const;
   std::string describe_token(const Token &) const;
   void log_trace(ParserChannel, const Token &, std::string_view,
      std::optional<uint8_t> FileIndex = std::nullopt) const;

   LexState *lex_state;
   FuncState *func_state;
   lua_State *lua_state;
   ParserAllocator allocator;
   ParserConfig current_config;
   ParserDiagnostics diag;
   TokenStreamAdapter token_stream;
   ParserContext *previous_context = nullptr;
   ErrorRollbackCallback error_rollback_callback = nullptr;
   void *error_rollback_user_data = nullptr;
   std::vector<std::string> import_stack_;  // Stack of imported file paths for circular dependency detection
};

//********************************************************************************************************************

class ParserSession {
public:
   inline ParserSession(ParserContext& context, ParserConfig config)
   : ctx(&context), previous(context.config()) {
      this->ctx->override_config(config);
   }

   inline ~ParserSession() { this->ctx->restore_config(this->previous); }

private:
   ParserContext *ctx;
   ParserConfig previous;
};
