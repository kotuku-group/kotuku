// Copyright © 2025-2026 Paul Manias

#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include "../../../defs.h"
#include "parser/parser_context.h"

#include <cstdio>

#include "parser/parse_types.h"
#include "parser.h"
#ifdef INCLUDE_TIPS
#include "parser/parser_tips.h"
#endif

//********************************************************************************************************************

ParserContext & ParserContext::operator=(ParserContext &&other) noexcept
{
   if (this != &other) {
      this->detach_from_lex();
      this->lex_state      = other.lex_state;
      this->func_state     = other.func_state;
      this->lua_state      = other.lua_state;
      this->allocator      = other.allocator;
      this->current_config = other.current_config;
      this->diag           = std::move(other.diag);
      this->diag.set_limit(this->current_config.max_diagnostics);
      this->token_stream     = other.token_stream;
      this->previous_context = other.previous_context;
      if (this->lex_state) this->lex_state->active_context = this;
      other.lex_state        = nullptr;
      other.func_state       = nullptr;
      other.lua_state        = nullptr;
      other.previous_context = nullptr;
   }
   return *this;
}

//********************************************************************************************************************

ParserResult<Token> ParserContext::match(TokenKind kind)
{
   Token current = this->tokens().current();
   if (current.is(kind)) {
      this->token_stream.advance();
      return ParserResult<Token>::success(current);
   }

   auto prv = (prvTiri *)this->lua_state->script->ChildPrivate;
   if ((prv->JitOptions & JOF::TRACE_EXPECT) != JOF::NIL) {
      std::string expectation = this->format_expected_message(kind);
      ParserDiagnostic diagnostic;
      diagnostic.severity = ParserDiagnosticSeverity::Info;
      diagnostic.code     = ParserErrorCode::ExpectedToken;
      diagnostic.message  = expectation;
      diagnostic.token    = current;
      this->diag.report(diagnostic);
      this->log_trace(ParserChannel::Expect, current, expectation);
   }

   ParserError error = this->make_error(ParserErrorCode::ExpectedToken, current, std::string_view{});
   return ParserResult<Token>::failure(error);
}

//********************************************************************************************************************

ParserResult<Token> ParserContext::consume(TokenKind kind, ParserErrorCode code)
{
   auto result = this->match(kind);
   if (result.ok()) return result;

   std::string expectation = this->format_expected_message(kind);
   Token current = this->tokens().current();
   this->emit_error(code, current, expectation);
   return ParserResult<Token>::failure(this->make_error(code, current, expectation));
}

//********************************************************************************************************************

ParserResult<Token> ParserContext::expect_identifier(ParserErrorCode code)
{
   Token current = this->tokens().current();
   if (current.is_identifier()) {
      this->token_stream.advance();
      return ParserResult<Token>::success(current);
   }
   this->emit_error(code, current, "expected identifier");
   return ParserResult<Token>::failure(this->make_error(code, current, "expected identifier"));
}

//********************************************************************************************************************
// Expects an identifier or a reserved keyword that can be used as a name (e.g., after . or : for field/method access).
// This allows keywords like 'check' or 'raise' to be used as method names: object.check(), object:raise()

ParserResult<Token> ParserContext::expect_name(ParserErrorCode code)
{
   Token current = this->tokens().current();
   if (current.is_name()) {
      this->token_stream.advance();
      return ParserResult<Token>::success(current);
   }
   this->emit_error(code, current, "expected identifier");
   return ParserResult<Token>::failure(this->make_error(code, current, "expected identifier"));
}

//********************************************************************************************************************

int ParserContext::lex_opt(LexToken Token)
{
   if (this->tokens().current().raw() IS Token) {
      this->token_stream.advance();
      return 1;
   }
   return 0;
}

//********************************************************************************************************************

void ParserContext::lex_match(LexToken what, LexToken who, BCLine line)
{
   if (this->lex_opt(what)) return;

   if (line IS this->lex_state->effective_line()) {
      this->err_token(what);
      return;
   }

   auto swhat = this->lex_state->token2str(what);
   auto swho = this->lex_state->token2str(who);
   lj_lex_error(this->lex_state, this->lex_state->tok, ErrMsg::XMATCH, swhat, swho, line);
}

//********************************************************************************************************************

GCstr* ParserContext::lex_str()
{
   Token current = this->tokens().current();
   if (not current.is_identifier()) {
      this->err_token(TK_name);
      return NAME_BLANK;
   }

   GCstr *result = current.identifier();
   this->token_stream.advance();
   return result ? result : NAME_BLANK;
}

//********************************************************************************************************************

void ParserContext::err_syntax(ErrMsg message)
{
   Token current = this->tokens().current();
   ParserDiagnostic diagnostic;
   diagnostic.severity = ParserDiagnosticSeverity::Error;
   diagnostic.code     = ParserErrorCode::UnexpectedToken;
   GCstr *text = lj_err_str(this->lua_state, message);
   if (text) diagnostic.message.assign(strdata(text), text->len);
   diagnostic.token = current;
   this->diag.report(diagnostic);
   lj_lex_error(this->lex_state, this->lex_state->tok, message);
}

//********************************************************************************************************************

void ParserContext::err_token(LexToken Token)
{
   ::Token current = this->tokens().current();
   ParserDiagnostic diagnostic;
   diagnostic.severity = ParserDiagnosticSeverity::Error;
   diagnostic.code     = ParserErrorCode::UnexpectedToken;
   diagnostic.message  = this->format_lex_error(Token);
   diagnostic.token    = current;
   this->diag.report(diagnostic);
   lj_lex_error(this->lex_state, this->lex_state->tok, ErrMsg::XTOKEN, this->lex_state->token2str(Token));
}

//********************************************************************************************************************

void ParserContext::report_limit_error(FuncState &FState, uint32_t Limit, CSTRING What)
{
   ParserDiagnostic diagnostic;
   diagnostic.severity = ParserDiagnosticSeverity::Error;
   diagnostic.code     = ParserErrorCode::UnexpectedToken;
   diagnostic.message  = std::string("function limit exceeded for ") + What;
   diagnostic.token    = this->tokens().current();
   this->diag.report(diagnostic);
   if (FState.linedefined IS 0) {
      lj_lex_error(FState.ls, 0, ErrMsg::XLIMM, Limit, What);
      return;
   }
   lj_lex_error(FState.ls, 0, ErrMsg::XLIMF, FState.linedefined, Limit, What);
}

//********************************************************************************************************************

std::string ParserContext::format_expected_message(TokenKind kind) const
{
   CSTRING name = token_kind_name(kind, this->lex());
   std::string message = std::string("expected ") + name;
   if ((LexToken)kind <= TK_OFS) lua_pop(this->lua_state, 1);
   return message;
}

//********************************************************************************************************************

ParserError ParserContext::make_error(ParserErrorCode code, const Token &token, std::string_view Message)
{
   ParserError error;
   error.code = code;
   error.token = token;
   error.message.assign(Message.begin(), Message.end());
   return error;
}

//********************************************************************************************************************

void ParserContext::trace_token_advance(const Token &previous, const Token &current) const
{
   auto prv = (prvTiri *)this->lua_state->script->ChildPrivate;

   if ((prv->JitOptions & JOF::TRACE_TOKENS) != JOF::NIL) {
      std::string detail = std::string("previous: ") + this->describe_token(previous);
      this->log_trace(ParserChannel::Advance, current, detail);
   }
}

//********************************************************************************************************************
// Note: This function does not return if abort_on_error is true

void ParserContext::emit_error(ParserErrorCode code, const Token &token, std::string_view Message)
{
   ParserDiagnostic diagnostic;
   diagnostic.severity = ParserDiagnosticSeverity::Error;
   diagnostic.code     = code;
   diagnostic.message.assign(Message.begin(), Message.end());
   diagnostic.token    = token;
   this->diag.report(diagnostic);

   if (this->current_config.abort_on_error) {
      // Log immediately since we're about to throw
      this->log_trace(ParserChannel::Error, token, Message);

      // Save the diagnostics for client analysis
      if (this->lua().parser_diagnostics) delete (ParserDiagnostics*)this->lua().parser_diagnostics;
      this->lua().parser_diagnostics = new ParserDiagnostics(this->diagnostics());

      lj_lex_error(this->lex_state, this->lex_state->tok, ErrMsg::XTOKEN, this->lex_state->token2str(token.raw()));
   }
   // In DIAGNOSE mode (abort_on_error=false), skip logging - errors will be reported later
}

//********************************************************************************************************************
// Emit a warning diagnostic (non-fatal)

void ParserContext::emit_warning(ParserErrorCode code, const Token &Token, std::string_view Message)
{
   ParserDiagnostic diagnostic;
   diagnostic.severity = ParserDiagnosticSeverity::Warning;
   diagnostic.code     = code;
   diagnostic.message.assign(Message.begin(), Message.end());
   diagnostic.token    = Token;
   this->diag.report(diagnostic);

   this->log_trace(ParserChannel::Warning, Token, Message);
}

//********************************************************************************************************************

void ParserContext::attach_to_lex()
{
   if (not this->lex_state) return;
   this->previous_context = this->lex_state->active_context;
   this->lex_state->active_context = this;
}

//********************************************************************************************************************

void ParserContext::detach_from_lex()
{
   if (not this->lex_state) return;
   if (this->lex_state->active_context IS this) {
      this->lex_state->active_context = this->previous_context;
   }
}

//********************************************************************************************************************

std::string ParserContext::format_lex_error(LexToken Token) const
{
   auto text = this->lex_state->token2str(Token);
   if (not text) return std::string("unexpected token");
   return std::string("unexpected ") + text;
}

//********************************************************************************************************************

std::string ParserContext::describe_token(const Token &Token) const
{
   auto name = token_kind_name(Token.kind(), this->lex());
   std::string result;

   result.assign(name ? name : "token");

   if ((LexToken)Token.kind() <= TK_OFS) lua_pop(this->lua_state, 1);

   if (Token.is_identifier()) {
      if (GCstr *identifier = Token.identifier()) {
         result.push_back(' ');
         result.push_back('\'');
         result.append(strdata(identifier), identifier->len);
         result.push_back('\'');
      }
   }
   return result;
}

//********************************************************************************************************************

void ParserContext::log_trace(ParserChannel Channel, const Token &Token, std::string_view Note) const
{
   pf::Log log("Parser");

   std::string name = this->describe_token(Token);
   BCLine line = Token.span().line;
   BCLine column = Token.span().column;

   VLF level = VLF::API;
   CSTRING channel;
   switch(Channel) {
      case ParserChannel::Error:   channel = "Error"; level = VLF::WARNING; break;
      case ParserChannel::Warning: channel = "Warning"; level = VLF::WARNING; break;
      case ParserChannel::Expect:  channel = "Expect"; break;
      case ParserChannel::Advance: channel = "Advance"; break;
      default:                     channel = "Unknown"; break;
   }

   if (Note.empty()) {
      log.msg(level, "[%d:%d] %s: %s", (int)line, (int)column, channel, name.c_str());
      return;
   }

   log.msg(level, "[%d:%d] %s: %s - %.*s", (int)line, (int)column, channel, name.c_str(), (int)Note.size(), Note.data());
}

//********************************************************************************************************************
// Check if tip at the given priority level would be emitted.
// This allows callers to skip expensive checks when tip would be filtered out anyway.

//********************************************************************************************************************
// Check if a file path is already in the import stack (circular dependency detection).

bool ParserContext::is_importing(const std::string& Path) const
{
   for (const auto& imported : this->import_stack_) {
      if (imported == Path) return true;
   }
   return false;
}

//********************************************************************************************************************
// Push a file path onto the import stack.

void ParserContext::push_import(const std::string& Path)
{
   this->import_stack_.push_back(Path);
}

//********************************************************************************************************************
// Pop the most recent file path from the import stack.

void ParserContext::pop_import()
{
   if (not this->import_stack_.empty()) {
      this->import_stack_.pop_back();
   }
}

//********************************************************************************************************************
// Resolve an import path relative to the file currently being parsed.
// The Library parameter will be sanitised on return.

std::string ParserContext::resolve_lib_to_path(std::string_view &Library) const
{
   // For security purposes, check the validity of the library name.

   int slash_count = 0;

   bool local = false;
   if (Library.starts_with("./")) { // Local libraries are permitted if the name starts with "./" and otherwise adheres to path rules
      local = true;
      Library.remove_prefix(2);
   }

   size_t i;
   for (i=0; i < Library.size(); i++) {
      if ((Library[i] >= 'a') and (Library[i] <= 'z')) continue;
      if ((Library[i] >= 'A') and (Library[i] <= 'Z')) continue;
      if ((Library[i] >= '0') and (Library[i] <= '9')) continue;
      if ((Library[i] IS '-') or (Library[i] IS '_')) continue;
      if (Library[i] IS '/') { slash_count++; continue; }
      break;
   }

   if ((i < Library.size()) or (i >= 96) or (slash_count > 2)) {
      lj_lex_error(this->lex_state, 0, ErrMsg::BADLIBRARY);
      return "";
   }

   std::string result(Library);

   // Prepend the base path

   if (local) {
      if (this->lex_state->chunk_arg) {
         // Get the directory of the current file from chunk_arg
         std::string current_file(this->lex_state->chunk_arg);

         // Strip leading '@' or '=' from chunk_arg (Lua conventions for source naming)
         if (not current_file.empty() and (current_file[0] == '@' or current_file[0] == '=')) {
            current_file = current_file.substr(1);
         }

         // Find the last path separator
         size_t last_sep = current_file.find_last_of("/\\");
         if (last_sep != std::string::npos) {
            std::string dir = current_file.substr(0, last_sep + 1);
            result = dir + result;
         }
         else { // Use script's working path as fallback
            auto working_path = this->lua_state->script->get<CSTRING>(FID_WorkingPath);
            if (working_path) result.insert(0, working_path);
         }
      }
      else { // Use script's working path as fallback
         auto working_path = this->lua_state->script->get<CSTRING>(FID_WorkingPath);
         if (working_path) result.insert(0, working_path);
      }
   }
   else result.insert(0, "scripts:");

   result.append(".tiri");
   return result;
}

//********************************************************************************************************************

#ifdef INCLUDE_TIPS
bool ParserContext::should_emit_tip(uint8_t Priority) const
{
   auto *emitter = this->tip();
   if (not emitter) return false;
   return emitter->should_emit(Priority);
}

//********************************************************************************************************************
// Emit a tip message if the tip system is enabled.

void ParserContext::emit_tip(uint8_t Priority, TipCategory Category, std::string Message, const Token &Location)
{
   auto *emitter = this->tip();
   if (not emitter) return;

   std::string_view filename;
   if (this->lex_state->chunk_name) {
      filename = std::string_view(strdata(this->lex_state->chunk_name), this->lex_state->chunk_name->len);
   }

   emitter->emit(Priority, Category, std::move(Message), Location, filename);
}
#endif
