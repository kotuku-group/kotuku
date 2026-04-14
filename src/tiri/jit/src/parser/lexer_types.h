#pragma once

struct SourceSpan {
   BCLine line = 0;
   BCLine column = 0;
   size_t offset = 0;
};

//********************************************************************************************************************

struct TokenDefinition {
   std::string_view name;    // Token identifier (e.g., "and", "if_empty")
   std::string_view symbol;  // Display symbol (e.g., "and", "??")
   bool reserved;            // True for reserved words that cannot be used as identifiers

   [[nodiscard]] constexpr bool is_reserved() const noexcept { return reserved; }
};

// Define all tokens once using X-macro pattern
// Format: TOKEN_DEF(name, symbol, reserved)
#define TOKEN_DEF_LIST \
   TOKEN_DEF(and,          "and",      true) \
   TOKEN_DEF(as,           "as",       true) \
   TOKEN_DEF(break,        "break",    true) \
   TOKEN_DEF(choose,       "choose",   true) \
   TOKEN_DEF(continue,     "continue", true) \
   TOKEN_DEF(defer,        "defer",    true) \
   TOKEN_DEF(do,           "do",       true) \
   TOKEN_DEF(else,         "else",     true) \
   TOKEN_DEF(elseif,       "elseif",   true) \
   TOKEN_DEF(end,          "end",      true) \
   TOKEN_DEF(false,        "false",    true) \
   TOKEN_DEF(for,          "for",      true) \
   TOKEN_DEF(from,         "from",     true) \
   TOKEN_DEF(function,     "function", true) \
   TOKEN_DEF(global,       "global",   true) \
   TOKEN_DEF(has,          "has",      true) \
   TOKEN_DEF(if,           "if",       true) \
   TOKEN_DEF(import,       "import",   true) \
   TOKEN_DEF(in,           "in",       true) \
   TOKEN_DEF(is,           "is",       true) \
   TOKEN_DEF(local,        "local",    true) \
   TOKEN_DEF(namespace,    "namespace", true) \
   TOKEN_DEF(nil,          "nil",      true) \
   TOKEN_DEF(not,          "not",      true) \
   TOKEN_DEF(or,           "or",       true) \
   TOKEN_DEF(repeat,       "repeat",   true) \
   TOKEN_DEF(return,       "return",   true) \
   TOKEN_DEF(then,         "then",     true) \
   TOKEN_DEF(thunk,        "thunk",    true) \
   TOKEN_DEF(true,         "true",     true) \
   TOKEN_DEF(try,          "try",      true) \
   TOKEN_DEF(except,       "except",   true) \
   TOKEN_DEF(until,        "until",    true) \
   TOKEN_DEF(when,         "when",     true) \
   TOKEN_DEF(success,      "success",  true) \
   TOKEN_DEF(raise,        "raise",    true) \
   TOKEN_DEF(check,        "check",    true) \
   TOKEN_DEF(while,        "while",    true) \
   TOKEN_DEF(with,         "with",     true) \
   TOKEN_DEF(case_arrow,   "->",       false) \
   TOKEN_DEF(if_empty,     "??",       false) \
   TOKEN_DEF(safe_field,   "?.",       false) \
   TOKEN_DEF(safe_index,   "?[",       false) \
   TOKEN_DEF(safe_method,  "?:",       false) \
   TOKEN_DEF(arrow,        "=>",       false) \
   TOKEN_DEF(concat,       "..",       false) \
   TOKEN_DEF(dots,         "...",      false) \
   TOKEN_DEF(eq,           "==",       false) \
   TOKEN_DEF(ge,           ">=",       false) \
   TOKEN_DEF(le,           "<=",       false) \
   TOKEN_DEF(ne,           "~=",       false) \
   TOKEN_DEF(shl,          "<<",       false) \
   TOKEN_DEF(shr,          ">>",       false) \
   TOKEN_DEF(ternary_sep,  ":>",       false) \
   TOKEN_DEF(number,       "<number>", false) \
   TOKEN_DEF(name,         "<name>",   false) \
   TOKEN_DEF(string,       "<string>", false) \
   TOKEN_DEF(cadd,         "+=",       false) \
   TOKEN_DEF(csub,         "-=",       false) \
   TOKEN_DEF(cmul,         "*=",       false) \
   TOKEN_DEF(cdiv,         "/=",       false) \
   TOKEN_DEF(cconcat,      "..=",      false) \
   TOKEN_DEF(cmod,         "%=",       false) \
   TOKEN_DEF(cif_empty,    "?\?=",     false) \
   TOKEN_DEF(cif_nil,      "?=",       false) \
   TOKEN_DEF(plusplus,     "++",       false) \
   TOKEN_DEF(pow,          "**",       false) \
   TOKEN_DEF(pipe,         "|>",       false) \
   TOKEN_DEF(defer_open,   "<{",       false) \
   TOKEN_DEF(defer_typed,  "<type{",   false) \
   TOKEN_DEF(defer_close,  "}>",       false) \
   TOKEN_DEF(array_typed,  "array<type>", false) \
   TOKEN_DEF(annotate,     "@",        false) \
   TOKEN_DEF(compif,       "@if",      false) \
   TOKEN_DEF(compend,      "@end",     false) \
   TOKEN_DEF(eof,          "<eof>",    false)

// Generate TOKEN_DEFINITIONS array from TOKEN_DEF_LIST
// This array provides compile-time token metadata
#define TOKEN_DEF(name, symbol, reserved) TokenDefinition{#name, symbol, reserved},
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

#define TOKEN_DEF(name, symbol, reserved) TK_##name,
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
