// Parser Diagnostics - Error collection and reporting for the Tiri parser.
//
// Copyright © 2025-2026 Paul Manias
//
// This module provides infrastructure for collecting and reporting parser diagnostics during
// compilation. It supports multiple severity levels (Info, Warning, Error) and implements a
// configurable limit on the number of diagnostics collected to prevent overwhelming output
// during error recovery.
//
// Key features:
// - Accumulates diagnostics with source location information
// - Supports DIAGNOSE mode for collecting multiple errors in a single parse
// - Limits diagnostic collection to prevent runaway error cascades
// - Provides formatted output for user-facing error messages

#include "parser/parser_diagnostics.h"
#include <kotuku/main.h>
#include <format>

//********************************************************************************************************************
// Reports a function limit error (too many locals, upvalues, etc.) and throws.
// This is called when parser limits are exceeded during compilation.

[[maybe_unused]] void err_limit(FuncState *FS, uint32_t Limit, CSTRING What)
{
   if (FS->ls->active_context) FS->ls->active_context->report_limit_error(*FS, Limit, What);

   if (FS->linedefined IS 0) lj_lex_error(FS->ls, 0, ErrMsg::XLIMM, Limit, What);
   else lj_lex_error(FS->ls, 0, ErrMsg::XLIMF, FS->linedefined, Limit, What);
}

//********************************************************************************************************************
// Returns a human-readable name for a diagnostic severity level.

static CSTRING severity_name(ParserDiagnosticSeverity Severity)
{
   switch (Severity) {
      case ParserDiagnosticSeverity::Info:    return "Info";
      case ParserDiagnosticSeverity::Warning: return "Warning";
      case ParserDiagnosticSeverity::Error:   return "Error";
      default: return "unknown";
   }
}

//********************************************************************************************************************
// Returns a human-readable name for a parser error code.

static CSTRING error_code_name(ParserErrorCode Code)
{
   switch (Code) {
      case ParserErrorCode::None:                   return "None";
      case ParserErrorCode::UnexpectedToken:        return "Unexpected Token";
      case ParserErrorCode::ExpectedToken:          return "Expected Token";
      case ParserErrorCode::ExpectedIdentifier:     return "Expected Identifier";
      case ParserErrorCode::UnexpectedEndOfFile:    return "Unexpected EOF";
      case ParserErrorCode::InternalInvariant:      return "Internal invariant";
      case ParserErrorCode::ExpectedTypeName:       return "Expected type name";
      case ParserErrorCode::UnknownTypeName:        return "Unknown type name";
      case ParserErrorCode::TypeMismatchArgument:   return "Type mismatch (argument)";
      case ParserErrorCode::TypeMismatchAssignment: return "Type mismatch (assignment)";
      case ParserErrorCode::TypeMismatchReturn:     return "Type mismatch (return)";
      case ParserErrorCode::DeferredTypeRequired:   return "Deferred type required";
      case ParserErrorCode::UndefinedVariable:      return "Undefined variable";
      case ParserErrorCode::ThunkDirectCall:        return "Thunk direct call";
      case ParserErrorCode::RecoverySkippedTokens:  return "Recovery skipped tokens";
      case ParserErrorCode::AssignToConstant:       return "Assign to constant";
      default: return "Unknown";
   }
}

//********************************************************************************************************************
// Formats the diagnostic as a human-readable string for display.
// The LineOffset parameter allows adjusting line numbers for embedded scripts.

std::string ParserDiagnostic::to_string(int LineOffset) const
{
   SourceSpan span = this->token.span();
   return std::format("[{}:{}] {}: {}: {}", span.line + LineOffset, span.column,
      severity_name(this->severity), error_code_name(this->code),
      this->message.empty() ? "No message" : this->message);
}

//********************************************************************************************************************
// Constructs a diagnostics collector with a default limit of 8 entries.

ParserDiagnostics::ParserDiagnostics() : limit(8)
{
}

//********************************************************************************************************************
// Sets the maximum number of error/warning diagnostics to collect.
// Info-level diagnostics do not count against this limit.

void ParserDiagnostics::set_limit(uint32_t NewLimit)
{
   this->limit = NewLimit;
}

//********************************************************************************************************************
// Records a diagnostic entry. Error and Warning severities count against the configured limit;
// Info-level diagnostics are always accepted. Once the limit is reached, additional errors
// and warnings are silently discarded to prevent overwhelming output during error recovery.

void ParserDiagnostics::report(const ParserDiagnostic &Diagnostic)
{
   bool counts_against_limit = Diagnostic.severity IS ParserDiagnosticSeverity::Error
      or Diagnostic.severity IS ParserDiagnosticSeverity::Warning;

   if (counts_against_limit and this->counted_entries >= this->limit) return;
   this->storage.push_back(Diagnostic);
   if (counts_against_limit) this->counted_entries += 1;
}

//********************************************************************************************************************
// Returns true if any error-level diagnostics have been recorded.

bool ParserDiagnostics::has_errors() const
{
   for (const auto& entry : this->storage) {
      if (entry.severity IS ParserDiagnosticSeverity::Error) return true;
   }
   return false;
}

//********************************************************************************************************************
// Returns a read-only view of all collected diagnostics.

std::span<const ParserDiagnostic> ParserDiagnostics::entries() const
{
   return std::span<const ParserDiagnostic>(this->storage.data(), this->storage.size());
}

//********************************************************************************************************************
// Clears all collected diagnostics and resets the entry counter.

void ParserDiagnostics::clear()
{
   this->storage.clear();
   this->counted_entries = 0;
}
