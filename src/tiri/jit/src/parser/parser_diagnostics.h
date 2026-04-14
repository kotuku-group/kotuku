// Copyright © 2025-2026 Paul Manias

#pragma once

#include <span>
#include <string>
#include <vector>

#include "parser/token_types.h"

enum class ParserDiagnosticSeverity : uint8_t {
   Info,
   Warning,
   Error
};

enum class ParserErrorCode : uint16_t {
   None = 0,
   UnexpectedToken,
   ExpectedToken,
   ExpectedIdentifier,
   UnexpectedEndOfFile,
   InternalInvariant,
   ExpectedTypeName,
   UnknownTypeName,
   TypeMismatchArgument,
   TypeMismatchAssignment,
   ObjectClassMismatch,      // Object class ID mismatch in assignment
   TypeMismatchReturn,
   DeferredTypeRequired,
   UndefinedVariable,
   ThunkDirectCall,         // Warning: thunk called without assignment defeats memoization
   ReturnTypeMismatch,      // Return value type doesn't match declaration
   ReturnCountMismatch,     // Too many return values
   RecursiveFunctionNeedsType, // Recursive function must have explicit return type
   TooManyReturnTypes,      // More than 8 return types declared
   RecoverySkippedTokens,   // Info: tokens skipped during error recovery
   AssignToConstant,        // Cannot assign to a registered constant
   ConstRequiresInitialiser // Const variable requires an initialiser
};

struct ParserDiagnostic {
   ParserDiagnosticSeverity severity = ParserDiagnosticSeverity::Error;
   ParserErrorCode code = ParserErrorCode::UnexpectedToken;
   std::string message;
   Token token;

   [[nodiscard]] std::string to_string(int LineOffset = 0) const;
};

class ParserDiagnostics {
public:
   ParserDiagnostics();

   void set_limit(uint32_t Limit);
   void report(const ParserDiagnostic& Diagnostic);
   [[nodiscard]] bool has_errors() const;
   [[nodiscard]] bool empty() const { return this->storage.empty(); }
   [[nodiscard]] size_t count() const { return this->counted_entries; }
   [[nodiscard]] std::span<const ParserDiagnostic> entries() const;
   void clear();

private:
   uint32_t limit;
   uint32_t counted_entries = 0;
   std::vector<ParserDiagnostic> storage;
};
