// Copyright © 2025-2026 Paul Manias

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <kotuku/main.h>

#include "lexer.h"

// Strongly typed representation of lexer tokens.

enum class TokenKind : uint16_t {
   Unknown = 0,
#define TOKEN_KIND_ENUM(name) name = TK_##name,
#define TOKEN_KIND_ENUM_SYM(name, sym) name = TK_##name,
   Identifier = TK_name,
   Number = TK_number,
   String = TK_string,
   Nil = TK_nil,
   AsToken = TK_as,
   TrueToken = TK_true,
   FalseToken = TK_false,
   Function = TK_function,
   Global = TK_global,
   Local = TK_local,
   EndToken = TK_end,
   ReturnToken = TK_return,
   If = TK_if,
   ImportToken = TK_import,
   NamespaceToken = TK_namespace,
   Else = TK_else,
   ElseIf = TK_elseif,
   For = TK_for,
   WhileToken = TK_while,
   WithToken = TK_with,
   Repeat = TK_repeat,
   Until = TK_until,
   DoToken = TK_do,
   ThenToken = TK_then,
   InToken = TK_in,
   BreakToken = TK_break,
   ContinueToken = TK_continue,
   DeferToken = TK_defer,
   AndToken = TK_and,
   OrToken = TK_or,
   NotToken = TK_not,
   HasToken = TK_has,
   IsToken = TK_is,
   TernarySep = TK_ternary_sep,
   Dots = TK_dots,
   Cat = TK_concat,
   Equal = TK_eq,
   NotEqual = TK_ne,
   LessEqual = TK_le,
   GreaterEqual = TK_ge,
   ShiftLeft = TK_shl,
   ShiftRight = TK_shr,
   CompoundAdd = TK_cadd,
   CompoundSub = TK_csub,
   CompoundMul = TK_cmul,
   CompoundDiv = TK_cdiv,
   CompoundMod = TK_cmod,
   CompoundConcat = TK_cconcat,
   CompoundIfEmpty = TK_cif_empty,
   CompoundIfNil = TK_cif_nil,
   SafeField = TK_safe_field,
   SafeIndex = TK_safe_index,
   SafeMethod = TK_safe_method,
   Presence = TK_if_empty, // NOTE: This single token covers use of both `if present?? then` (postfix) and `(variable ?? default_value)` (if empty).
   PlusPlus = TK_plusplus,
   Power = TK_pow,
   Pipe = TK_pipe,
   Arrow = TK_arrow,
   DeferredOpen = TK_defer_open,
   DeferredTyped = TK_defer_typed,
   DeferredClose = TK_defer_close,
   ArrayTyped = TK_array_typed,
   ThunkToken = TK_thunk,
   Choose = TK_choose,
   From = TK_from,
   When = TK_when,
   CaseArrow = TK_case_arrow,
   Annotate = TK_annotate,
   CompileIf = TK_compif,
   CompileEnd = TK_compend,
   TryToken = TK_try,
   ExceptToken = TK_except,
   SuccessToken = TK_success,
   RaiseToken = TK_raise,
   CheckToken = TK_check,
   EndOfFile = TK_eof,
#undef TOKEN_KIND_ENUM
#undef TOKEN_KIND_ENUM_SYM
   LeftParen = '(',
   RightParen = ')',
   LeftBrace = '{',
   RightBrace = '}',
   LeftBracket = '[',
   RightBracket = ']',
   Dot = '.',
   Colon = ':',
   Comma = ',',
   Semicolon = ';',
   Equals = '=',
   Plus = '+',
   Minus = '-',
   Multiply = '*',
   Divide = '/',
   Modulo = '%',
   Question = '?',
   Less = '<',
   Greater = '>'
};

[[nodiscard]] inline CSTRING token_kind_name(TokenKind kind, LexState &lex) { return lex.token2str((LexToken)kind); }

// Constexpr alternative for compile-time token name lookup
// Returns a string_view without requiring a LexState reference.
[[nodiscard]] constexpr std::string_view token_kind_name_constexpr(TokenKind kind) noexcept {
   switch (kind) {
      case TokenKind::Identifier: return "<name>";
      case TokenKind::Number: return "<number>";
      case TokenKind::String: return "<string>";
      case TokenKind::Nil: return "nil";
      case TokenKind::AsToken: return "as";
      case TokenKind::TrueToken: return "true";
      case TokenKind::FalseToken: return "false";
      case TokenKind::Function: return "function";
      case TokenKind::Global: return "global";
      case TokenKind::Local: return "local";
      case TokenKind::EndToken: return "end";
      case TokenKind::ReturnToken: return "return";
      case TokenKind::If: return "if";
      case TokenKind::ImportToken: return "import";
      case TokenKind::NamespaceToken: return "namespace";
      case TokenKind::Else: return "else";
      case TokenKind::ElseIf: return "elseif";
      case TokenKind::For: return "for";
      case TokenKind::WhileToken: return "while";
      case TokenKind::WithToken: return "with";
      case TokenKind::Repeat: return "repeat";
      case TokenKind::Until: return "until";
      case TokenKind::DoToken: return "do";
      case TokenKind::ThenToken: return "then";
      case TokenKind::InToken: return "in";
      case TokenKind::BreakToken: return "break";
      case TokenKind::ContinueToken: return "continue";
      case TokenKind::DeferToken: return "defer";
      case TokenKind::AndToken: return "and";
      case TokenKind::OrToken: return "or";
      case TokenKind::NotToken: return "not";
      case TokenKind::HasToken: return "has";
      case TokenKind::IsToken: return "is";
      case TokenKind::TernarySep: return ":>";
      case TokenKind::Dots: return "...";
      case TokenKind::Cat: return "..";
      case TokenKind::Equal: return "==";
      case TokenKind::NotEqual: return "!=";
      case TokenKind::LessEqual: return "<=";
      case TokenKind::GreaterEqual: return ">=";
      case TokenKind::ShiftLeft: return "<<";
      case TokenKind::ShiftRight: return ">>";
      case TokenKind::CompoundAdd: return "+=";
      case TokenKind::CompoundSub: return "-=";
      case TokenKind::CompoundMul: return "*=";
      case TokenKind::CompoundDiv: return "/=";
      case TokenKind::CompoundMod: return "%=";
      case TokenKind::CompoundConcat: return "..=";
      case TokenKind::CompoundIfEmpty: return "?\?=";
      case TokenKind::CompoundIfNil: return "?=";
      case TokenKind::SafeField: return "?.";
      case TokenKind::SafeIndex: return "?[";
      case TokenKind::SafeMethod: return "?:";
      case TokenKind::Presence: return "??";
      case TokenKind::PlusPlus: return "++";
      case TokenKind::Power: return "**";
      case TokenKind::Pipe: return "|>";
      case TokenKind::Arrow: return "=>";
      case TokenKind::DeferredOpen: return "<{";
      case TokenKind::DeferredTyped: return "<type{";
      case TokenKind::DeferredClose: return "}>";
      case TokenKind::ArrayTyped: return "array<type>";
      case TokenKind::Choose: return "choose";
      case TokenKind::From: return "from";
      case TokenKind::When: return "when";
      case TokenKind::CaseArrow: return "->";
      case TokenKind::Annotate: return "@";
      case TokenKind::CompileIf: return "@if";
      case TokenKind::CompileEnd: return "@end";
      case TokenKind::TryToken: return "try";
      case TokenKind::ExceptToken: return "except";
      case TokenKind::SuccessToken: return "success";
      case TokenKind::RaiseToken: return "raise";
      case TokenKind::CheckToken: return "check";
      case TokenKind::EndOfFile: return "<eof>";
      case TokenKind::LeftParen: return "(";
      case TokenKind::RightParen: return ")";
      case TokenKind::LeftBrace: return "{";
      case TokenKind::RightBrace: return "}";
      case TokenKind::LeftBracket: return "[";
      case TokenKind::RightBracket: return "]";
      case TokenKind::Dot: return ".";
      case TokenKind::Colon: return ":";
      case TokenKind::Comma: return ",";
      case TokenKind::Semicolon: return ";";
      case TokenKind::Equals: return "=";
      case TokenKind::Plus: return "+";
      case TokenKind::Minus: return "-";
      case TokenKind::Multiply: return "*";
      case TokenKind::Divide: return "/";
      case TokenKind::Modulo: return "%";
      case TokenKind::Question: return "?";
      case TokenKind::Less: return "<";
      case TokenKind::Greater: return ">";
      default: return "<unknown>";
   }
}

//********************************************************************************************************************

struct TokenPayload {
   [[nodiscard]] constexpr inline bool has_value() const noexcept { return this->has_payload; }
   [[nodiscard]] constexpr inline const TValue& value() const noexcept { return this->payload; }

   inline void assign(lua_State *State, const TValue& Value) {
      this->owner = State;
      copyTV(State, &this->payload, &Value);
      this->has_payload = true;
   }

   [[nodiscard]] inline GCstr * as_string() const {
      if (not this->has_payload) return nullptr;
      if (not tvisstr(&this->payload)) return nullptr;
      return strV(&this->payload);
   }

   [[nodiscard]] inline double as_number() const {
      if (not this->has_payload) return 0.0;
      if (tvisnum(&this->payload)) return numV(&this->payload);
      return 0.0;
   }

private:
   TValue payload;
   lua_State* owner = nullptr;
   bool has_payload = false;
};

//********************************************************************************************************************

class Token {
public:
   Token() = default;

   [[nodiscard]] static Token from_current(LexState& state);
   [[nodiscard]] static Token from_lookahead(LexState& state);
   [[nodiscard]] static Token from_buffered(LexState& state, const LexState::BufferedToken& buffered);
   [[nodiscard]] static Token from_span(SourceSpan span, TokenKind kind = TokenKind::Unknown);

   [[nodiscard]] inline TokenKind kind() const { return this->token_kind; }
   [[nodiscard]] inline LexToken raw() const { return this->raw_token; }
   [[nodiscard]] inline SourceSpan span() const { return this->source; }
   [[nodiscard]] inline bool is(TokenKind kind) const { return this->token_kind IS kind; }
   [[nodiscard]] inline bool is_literal() const;
   [[nodiscard]] inline const TokenPayload & payload() const { return this->data; }

   [[nodiscard]] constexpr bool is_identifier() const noexcept { return this->token_kind IS TokenKind::Identifier; }
   [[nodiscard]] constexpr bool is_eof() const noexcept { return this->token_kind IS TokenKind::EndOfFile; }
   [[nodiscard]] inline GCstr * identifier() const { return this->data.as_string(); }

   // Returns true if this token is a reserved keyword (can be contextually used as a name after . or :)
   [[nodiscard]] constexpr bool is_keyword() const noexcept {
      return this->raw_token >= TK_OFS + 1 and this->raw_token < TK_number;
   }

   // Returns true if this token can be used as a name (identifier or contextual keyword)
   [[nodiscard]] constexpr bool is_name() const noexcept {
      return this->is_identifier() or this->is_keyword();
   }

private:
   TokenKind token_kind = TokenKind::Unknown;
   LexToken raw_token = 0;
   SourceSpan source;
   TokenPayload data;
};

[[nodiscard]] inline TokenKind to_token_kind(LexToken token) { return (TokenKind)token; }
