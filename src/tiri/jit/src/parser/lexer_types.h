#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

inline constexpr uint32_t TKF_NONE = 0x0000;
inline constexpr uint32_t TKF_RESERVED = 0x0001;
inline constexpr uint32_t TKF_CAN_END_RANGE_EXPRESSION = 0x0002;
inline constexpr uint32_t TKF_MEMBER_NAME_CONTEXT = 0x0004;
inline constexpr uint32_t TKF_COMPOUND_ASSIGNMENT = 0x0008;
inline constexpr uint32_t TKF_LITERAL = 0x0010;
inline constexpr uint32_t TKF_STATEMENT_START = 0x0020;
inline constexpr uint32_t TKF_SHORTHAND_STATEMENT = 0x0040;

struct SourceSpan {
   BCLine line = 0;
   BCLine column = 0;
   size_t offset = 0;
};

//********************************************************************************************************************

struct TokenDefinition {
   std::string_view name;    // Token identifier (e.g., "and", "if_empty")
   std::string_view symbol;  // Display symbol (e.g., "and", "??")
   uint32_t flags;           // Token metadata flags

   [[nodiscard]] constexpr bool has_flag(uint32_t Flag) const noexcept { return (this->flags & Flag) != 0; }
   [[nodiscard]] constexpr bool is_reserved() const noexcept { return this->has_flag(TKF_RESERVED); }
};

// Define all tokens once using X-macro pattern
// Format: TOKEN_DEF(name, symbol, flags)
#define TOKEN_DEF_LIST \
   TOKEN_DEF(and,          "and",      TKF_RESERVED) \
   TOKEN_DEF(as,           "as",       TKF_RESERVED) \
   TOKEN_DEF(break,        "break",    TKF_RESERVED | TKF_STATEMENT_START | TKF_SHORTHAND_STATEMENT) \
   TOKEN_DEF(choose,       "choose",   TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(continue,     "continue", TKF_RESERVED | TKF_STATEMENT_START | TKF_SHORTHAND_STATEMENT) \
   TOKEN_DEF(defer,        "defer",    TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(do,           "do",       TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(else,         "else",     TKF_RESERVED) \
   TOKEN_DEF(elseif,       "elseif",   TKF_RESERVED) \
   TOKEN_DEF(end,          "end",      TKF_RESERVED | TKF_CAN_END_RANGE_EXPRESSION) \
   TOKEN_DEF(enum,         "enum",     TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(false,        "false",    TKF_RESERVED | TKF_CAN_END_RANGE_EXPRESSION | TKF_LITERAL) \
   TOKEN_DEF(for,          "for",      TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(from,         "from",     TKF_RESERVED) \
   TOKEN_DEF(function,     "function", TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(global,       "global",   TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(has,          "has",      TKF_RESERVED) \
   TOKEN_DEF(if,           "if",       TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(import,       "import",   TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(in,           "in",       TKF_RESERVED) \
   TOKEN_DEF(is,           "is",       TKF_RESERVED) \
   TOKEN_DEF(local,        "local",    TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(namespace,    "namespace", TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(nil,          "nil",      TKF_RESERVED | TKF_CAN_END_RANGE_EXPRESSION | TKF_LITERAL) \
   TOKEN_DEF(not,          "not",      TKF_RESERVED) \
   TOKEN_DEF(or,           "or",       TKF_RESERVED) \
   TOKEN_DEF(repeat,       "repeat",   TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(return,       "return",   TKF_RESERVED | TKF_STATEMENT_START | TKF_SHORTHAND_STATEMENT) \
   TOKEN_DEF(then,         "then",     TKF_RESERVED) \
   TOKEN_DEF(thunk,        "thunk",    TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(true,         "true",     TKF_RESERVED | TKF_CAN_END_RANGE_EXPRESSION | TKF_LITERAL) \
   TOKEN_DEF(try,          "try",      TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(except,       "except",   TKF_RESERVED) \
   TOKEN_DEF(until,        "until",    TKF_RESERVED) \
   TOKEN_DEF(when,         "when",     TKF_RESERVED) \
   TOKEN_DEF(success,      "success",  TKF_RESERVED) \
   TOKEN_DEF(raise,        "raise",    TKF_RESERVED | TKF_STATEMENT_START | TKF_SHORTHAND_STATEMENT) \
   TOKEN_DEF(check,        "check",    TKF_RESERVED | TKF_STATEMENT_START | TKF_SHORTHAND_STATEMENT) \
   TOKEN_DEF(while,        "while",    TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(with,         "with",     TKF_RESERVED | TKF_STATEMENT_START) \
   TOKEN_DEF(case_arrow,   "->",       TKF_NONE) \
   TOKEN_DEF(if_empty,     "??",       TKF_NONE) \
   TOKEN_DEF(guard,        "?!",       TKF_NONE) \
   TOKEN_DEF(safe_field,   "?.",       TKF_MEMBER_NAME_CONTEXT) \
   TOKEN_DEF(safe_index,   "?[",       TKF_NONE) \
   TOKEN_DEF(safe_method,  "?:",       TKF_MEMBER_NAME_CONTEXT) \
   TOKEN_DEF(arrow,        "=>",       TKF_NONE) \
   TOKEN_DEF(concat,       "..",       TKF_NONE) \
   TOKEN_DEF(dots,         "...",      TKF_NONE) \
   TOKEN_DEF(eq,           "==",       TKF_NONE) \
   TOKEN_DEF(ge,           ">=",       TKF_NONE) \
   TOKEN_DEF(le,           "<=",       TKF_NONE) \
   TOKEN_DEF(ne,           "~=",       TKF_NONE) \
   TOKEN_DEF(shl,          "<<",       TKF_NONE) \
   TOKEN_DEF(shr,          ">>",       TKF_NONE) \
   TOKEN_DEF(ternary_sep,  ":>",       TKF_NONE) \
   TOKEN_DEF(number,       "<number>", TKF_CAN_END_RANGE_EXPRESSION | TKF_LITERAL) \
   TOKEN_DEF(name,         "<name>",   TKF_CAN_END_RANGE_EXPRESSION) \
   TOKEN_DEF(string,       "<string>", TKF_CAN_END_RANGE_EXPRESSION | TKF_LITERAL) \
   TOKEN_DEF(cadd,         "+=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(csub,         "-=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cmul,         "*=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cdiv,         "/=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cconcat,      "..=",      TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cmod,         "%=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cif_empty,    "?\?=",     TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(cif_nil,      "?=",       TKF_COMPOUND_ASSIGNMENT) \
   TOKEN_DEF(plusplus,     "++",       TKF_CAN_END_RANGE_EXPRESSION) \
   TOKEN_DEF(pow,          "**",       TKF_NONE) \
   TOKEN_DEF(pipe,         "|>",       TKF_NONE) \
   TOKEN_DEF(defer_open,   "<{",       TKF_NONE) \
   TOKEN_DEF(defer_typed,  "<type{",   TKF_NONE) \
   TOKEN_DEF(defer_close,  "}>",       TKF_CAN_END_RANGE_EXPRESSION) \
   TOKEN_DEF(array_typed,  "array<type>", TKF_NONE) \
   TOKEN_DEF(annotate,     "@",        TKF_STATEMENT_START) \
   TOKEN_DEF(compif,       "@if",      TKF_STATEMENT_START) \
   TOKEN_DEF(compend,      "@end",     TKF_NONE) \
   TOKEN_DEF(eof,          "<eof>",    TKF_NONE)

// Generate TOKEN_DEFINITIONS array from TOKEN_DEF_LIST
// This array provides compile-time token metadata
#define TOKEN_DEF(name, symbol, flags) TokenDefinition{#name, symbol, flags},
inline constexpr std::array TOKEN_DEFINITIONS = {
   TOKEN_DEF_LIST
};
#undef TOKEN_DEF

// Compile-time count of reserved words
inline constexpr size_t generate_reserved_count() noexcept {
   size_t count = 0;
   for (const auto& def : TOKEN_DEFINITIONS) {
      if (def.is_reserved()) ++count;
   }
   return count;
}

// Compile-time token symbol lookup by index
[[nodiscard]] inline constexpr std::string_view token_symbol(size_t Index) noexcept {
   if (Index < TOKEN_DEFINITIONS.size()) {
      return TOKEN_DEFINITIONS[Index].symbol;
   }
   return "<invalid>";
}

// Compile-time token name lookup by index
[[nodiscard]] inline constexpr std::string_view token_name(size_t Index) noexcept {
   if (Index < TOKEN_DEFINITIONS.size()) {
      return TOKEN_DEFINITIONS[Index].name;
   }
   return "<invalid>";
}

// Generate enum values from TOKEN_DEF_LIST
// SINGLE SOURCE OF TRUTH: All token definitions come from TOKEN_DEF_LIST above

#define TOKEN_DEF(name, symbol, flags) TK_##name,
enum {
   TK_OFS = 256,
   TOKEN_DEF_LIST
   TK_RESERVED = TK_with - TK_OFS
};
#undef TOKEN_DEF

// Static assertions to verify enum and TOKEN_DEFINITIONS stay in sync.
// Token values start at TK_OFS + 1 (e.g., TK_and = 257).

static_assert(TK_eof - TK_OFS == TOKEN_DEFINITIONS.size(), "TOKEN_DEFINITIONS array size must match enum token count");
static_assert(TK_RESERVED == generate_reserved_count(), "Reserved word count mismatch between enum and TOKEN_DEFINITIONS");

typedef int LexChar;    //  Lexical character. Unsigned ext. from char.
typedef int LexToken;   //  Lexical token.

// Combined bytecode ins/line. Only used during bytecode generation.

typedef struct BCInsLine {
   BCIns ins;        //  Bytecode instruction.
   BCLine line;      //  Line number for this bytecode.
} BCInsLine;

// Info for local variables. Only used during bytecode generation.

enum class VarInfoFlag : uint8_t;
enum class TiriType : uint8_t;  // Forward declaration (defined in ast_nodes.h)

typedef struct VarInfo {
   GCRef name;        //  Local variable name.
   std::array<TiriType, MAX_RETURN_TYPES> result_types{};  // Return types if this variable holds a function
   BCPOS startpc;     //  First point where the local variable is active.
   BCPOS endpc;       //  First point where the local variable is dead.
   uint8_t slot;      //  Variable slot.
   VarInfoFlag info;  //  Variable info flags.
   TiriType fixed_type;  // Type once established (Unknown = not yet fixed)
   CLASSID object_class_id = CLASSID::NIL;  // CLASSID for Object types (0 = unknown class)
   BCLine line = 0;    // Line number where the variable was declared (for diagnostics)
   BCLine column = 0;  // Column number where the variable was declared (for diagnostics)
} VarInfo;
