// XPath Tokeniser Implementation
//
// The tokeniser converts XPath query strings into a sequence of tokens that can be parsed into an abstract
// syntax tree.  This lexical analysis stage handles all XPath token types including operators, literals,
// keywords, identifiers, and special syntax like axis specifiers and node tests.
//
// The tokeniser uses a single-pass character-by-character scan with lookahead to resolve ambiguous tokens
// (such as differentiating between the multiply operator and wildcard, or recognising multi-character
// operators like '::' and '//').  It maintains keyword mappings for language keywords ('and', 'or', 'if', etc.)
// and properly handles string literals, numeric constants, and qualified names.
//
// This implementation focuses on producing clean token streams that simplify the parser's job, allowing
// the parser to focus on grammatical structure rather than low-level character processing.

#include <kotuku/main.h>

#include <array>
#include <cctype>
#include <ranges>
#include <utility>

namespace {

struct KeywordMapping {
   std::string_view text;
   XPathTokenType type;
};

constexpr std::array keyword_mappings{
   KeywordMapping{ "and", XPathTokenType::AND },
   KeywordMapping{ "or", XPathTokenType::OR },
   KeywordMapping{ "not", XPathTokenType::NOT },
   KeywordMapping{ "div", XPathTokenType::DIVIDE },
   KeywordMapping{ "mod", XPathTokenType::MODULO },
   KeywordMapping{ "eq", XPathTokenType::EQ },
   KeywordMapping{ "ne", XPathTokenType::NE },
   KeywordMapping{ "lt", XPathTokenType::LT },
   KeywordMapping{ "le", XPathTokenType::LE },
   KeywordMapping{ "gt", XPathTokenType::GT },
   KeywordMapping{ "ge", XPathTokenType::GE },
   KeywordMapping{ "if", XPathTokenType::IF },
   KeywordMapping{ "then", XPathTokenType::THEN },
   KeywordMapping{ "else", XPathTokenType::ELSE },
   KeywordMapping{ "for", XPathTokenType::FOR },
   KeywordMapping{ "let", XPathTokenType::LET },
   KeywordMapping{ "in", XPathTokenType::IN },
   KeywordMapping{ "return", XPathTokenType::RETURN },
   KeywordMapping{ "where", XPathTokenType::WHERE },
   KeywordMapping{ "group", XPathTokenType::GROUP },
   KeywordMapping{ "by", XPathTokenType::BY },
   KeywordMapping{ "order", XPathTokenType::ORDER },
   KeywordMapping{ "stable", XPathTokenType::STABLE },
   KeywordMapping{ "ascending", XPathTokenType::ASCENDING },
   KeywordMapping{ "descending", XPathTokenType::DESCENDING },
   KeywordMapping{ "empty", XPathTokenType::EMPTY },
   KeywordMapping{ "default", XPathTokenType::DEFAULT },
   KeywordMapping{ "typeswitch", XPathTokenType::TYPESWITCH },
   KeywordMapping{ "case", XPathTokenType::CASE },
   KeywordMapping{ "declare", XPathTokenType::DECLARE },
   KeywordMapping{ "function", XPathTokenType::FUNCTION },
   KeywordMapping{ "variable", XPathTokenType::VARIABLE },
   KeywordMapping{ "namespace", XPathTokenType::NAMESPACE },
   KeywordMapping{ "external", XPathTokenType::EXTERNAL },
   KeywordMapping{ "boundary-space", XPathTokenType::BOUNDARY_SPACE },
   KeywordMapping{ "base-uri", XPathTokenType::BASE_URI },
   KeywordMapping{ "greatest", XPathTokenType::GREATEST },
   KeywordMapping{ "least", XPathTokenType::LEAST },
   KeywordMapping{ "collation", XPathTokenType::COLLATION },
   KeywordMapping{ "construction", XPathTokenType::CONSTRUCTION },
   KeywordMapping{ "ordering", XPathTokenType::ORDERING },
   KeywordMapping{ "copy-namespaces", XPathTokenType::COPY_NAMESPACES },
   KeywordMapping{ "decimal-format", XPathTokenType::DECIMAL_FORMAT },
   KeywordMapping{ "option", XPathTokenType::OPTION },
   KeywordMapping{ "import", XPathTokenType::IMPORT },
   KeywordMapping{ "module", XPathTokenType::MODULE },
   KeywordMapping{ "schema", XPathTokenType::SCHEMA },
   KeywordMapping{ "count", XPathTokenType::COUNT },
   KeywordMapping{ "some", XPathTokenType::SOME },
   KeywordMapping{ "every", XPathTokenType::EVERY },
   KeywordMapping{ "satisfies", XPathTokenType::SATISFIES },
   KeywordMapping{ "to", XPathTokenType::TO },
   KeywordMapping{ "map", XPathTokenType::MAP },
   KeywordMapping{ "array", XPathTokenType::ARRAY },
   KeywordMapping{ "cast", XPathTokenType::CAST },
   KeywordMapping{ "castable", XPathTokenType::CASTABLE },
   KeywordMapping{ "treat", XPathTokenType::TREAT },
   KeywordMapping{ "as", XPathTokenType::AS },
   KeywordMapping{ "instance", XPathTokenType::INSTANCE },
   KeywordMapping{ "of", XPathTokenType::OF }
};

constexpr std::array multi_char_operators{
   std::pair{ std::string_view("//"), XPathTokenType::DOUBLE_SLASH },
   std::pair{ std::string_view(".."), XPathTokenType::DOUBLE_DOT },
   std::pair{ std::string_view("::"), XPathTokenType::AXIS_SEPARATOR },
   std::pair{ std::string_view("!="), XPathTokenType::NOT_EQUALS },
   std::pair{ std::string_view("<="), XPathTokenType::LESS_EQUAL },
   std::pair{ std::string_view(">="), XPathTokenType::GREATER_EQUAL },
   std::pair{ std::string_view(":="), XPathTokenType::ASSIGN }
};

}

bool XPathTokeniser::is_alpha(char c) const
{
   return std::isalpha((unsigned char)(c));
}

bool XPathTokeniser::is_digit(char c) const
{
   return std::isdigit((unsigned char)(c));
}

bool XPathTokeniser::is_alnum(char c) const
{
   return std::isalnum((unsigned char)(c));
}

bool XPathTokeniser::is_whitespace(char c) const
{
   return std::isspace((unsigned char)(c));
}

bool XPathTokeniser::is_name_start_char(char c) const
{
   return is_alpha(c) or c IS '_';
}

bool XPathTokeniser::is_name_char(char c) const
{
   return is_alnum(c) or c IS '_' or c IS '-' or c IS '.';
}

char XPathTokeniser::peek(size_t offset) const
{
   size_t pos = position + offset;
   return pos < length ? input[pos] : '\0';
}

// Advances the position pointer past all consecutive whitespace characters in the input string.

void XPathTokeniser::skip_whitespace()
{
   while (position < length and is_whitespace(input[position]))
   {
      position++;
   }
}

bool XPathTokeniser::match(std::string_view Str)
{
   if (position + Str.size() > length) return false;
   return input.compare(position, Str.size(), Str) IS 0;
}

char XPathTokeniser::current() const
{
   return position < length ? input[position] : '\0';
}

void XPathTokeniser::advance()
{
   if (position < length) position++;
}

bool XPathTokeniser::has_more() const
{
   return position < length;
}

//********************************************************************************************************************
// Main tokenization function that converts an XPath query string into a vector of tokens. Handles operators,
// literals, identifiers, keywords, and special XPath syntax. Resolves ambiguities such as differentiating
// between the multiply operator and wildcard based on context. Tracks bracket and parenthesis depth to
// inform operator disambiguation logic.

TokenBlock XPathTokeniser::tokenize(std::string_view XPath)
{
   TokenBlock block;
   return tokenize(XPath, std::move(block));
}

TokenBlock XPathTokeniser::tokenize(std::string_view XPath, TokenBlock block)
{
   input = XPath;
   position = 0;
   length = input.size();
   previous_token_type = XPathTokenType::UNKNOWN;
   prior_token_type = XPathTokenType::UNKNOWN;
   block.ensure_storage();
   if (block.storage) block.storage->reset();
   auto &tokens = block.tokens;
   tokens.clear();
   tokens.reserve(XPath.size() / 6);  // Empirical analysis shows typical 1:6 char-to-token ratio
   int bracket_depth = 0;
   int paren_depth = 0;
   int direct_constructor_depth = 0;
   bool inside_direct_tag = false;
   bool pending_close_tag = false;
   int constructor_expr_depth = 0;

   auto lookahead_non_whitespace = [&](size_t index) -> size_t {
      size_t lookahead = index;
      while (lookahead < length and is_whitespace(input[lookahead])) lookahead++;
      return lookahead;
   };

   auto last_token_is_operand = [&](const std::vector<XPathToken> &TokenList) -> bool {
      if (TokenList.empty()) return false;
      const auto &token = TokenList.back();
      switch (token.type) {
         case XPathTokenType::IDENTIFIER:
         case XPathTokenType::NUMBER:
         case XPathTokenType::STRING:
         case XPathTokenType::TEXT_CONTENT:
         case XPathTokenType::RPAREN:
         case XPathTokenType::RBRACKET:
            return true;
         default:
            return false;
      }
   };

   while (position < length) {
      bool in_constructor_content = (direct_constructor_depth > 0) and (not inside_direct_tag) and (constructor_expr_depth IS 0);

      if (!in_constructor_content) {
         skip_whitespace();
         if (position >= length) break;
      }
      else if (position >= length) {
         break;
      }

      if (in_constructor_content) {
         char content_char = current();
         if (content_char IS '<' or content_char IS '{') {
            // fall through to structural handling
         }
         else {
            size_t start = position;
            while (position < length) {
               char segment_char = input[position];
               if (segment_char IS '<' or segment_char IS '{') break;
               position++;
            }

            size_t segment_length = position - start;
            if (segment_length > 0) {
               tokens.emplace_back(XPathTokenType::TEXT_CONTENT, input.substr(start, segment_length), start, segment_length);
               continue;
            }
         }
      }

      if (position >= length) break;

      char ch = current();

      if (inside_direct_tag and ch IS '/' and peek(1) IS '>') {
         size_t start = position;
         position += 2;
         tokens.emplace_back(XPathTokenType::EMPTY_TAG_CLOSE, input.substr(start, 2), start, 2);
         inside_direct_tag = false;
         pending_close_tag = false;
         if (direct_constructor_depth > 0) direct_constructor_depth--;
         continue;
      }
      else if (inside_direct_tag and ch IS '?' and peek(1) IS '>') {
         size_t start = position;
         position += 2;
         tokens.emplace_back(XPathTokenType::PI_END, input.substr(start, 2), start, 2);
         inside_direct_tag = false;
         pending_close_tag = false;
         continue;
      }
      else if (inside_direct_tag and (ch IS '\'' or ch IS '"')) {
         XPathToken token = scan_attribute_value(ch, true, block);
         tokens.push_back(std::move(token));
         continue;
      }
      else if (inside_direct_tag and ch IS '>') {
         size_t start = position;
         position++;
         tokens.emplace_back(XPathTokenType::TAG_CLOSE, input.substr(start, 1), start, 1);
         inside_direct_tag = false;
         if (pending_close_tag and direct_constructor_depth > 0) direct_constructor_depth--;
         pending_close_tag = false;
         continue;
      }
      else if (ch IS '{') {
         size_t start = position;
         position++;
         tokens.emplace_back(XPathTokenType::LBRACE, input.substr(start, 1), start, 1);
         if ((direct_constructor_depth > 0) and (not inside_direct_tag)) constructor_expr_depth++;
         continue;
      }
      else if (ch IS '}') {
         size_t start = position;
         position++;
         tokens.emplace_back(XPathTokenType::RBRACE, input.substr(start, 1), start, 1);
         if ((direct_constructor_depth > 0) and (not inside_direct_tag) and (constructor_expr_depth > 0)) constructor_expr_depth--;
         continue;
      }
      else  if (ch IS '<') {
         size_t start = position;

         if (position + 1 < length and input[position + 1] IS '=') {
            position += 2;
            tokens.emplace_back(XPathTokenType::LESS_EQUAL, input.substr(start, 2), start, 2);
            continue;
         }

         bool prev_is_operand = last_token_is_operand(tokens);
         size_t name_pos = lookahead_non_whitespace(position + 1);
         char lookahead_char = name_pos < length ? input[name_pos] : '\0';

         bool starts_close = lookahead_char IS '/';
         bool starts_pi = lookahead_char IS '?';
         bool starts_name = is_name_start_char(lookahead_char);

         bool constructor_candidate = starts_close or starts_pi or starts_name;
         bool treat_as_constructor = constructor_candidate and (!prev_is_operand or (direct_constructor_depth > 0) or tokens.empty());

         if (treat_as_constructor) {
            if (starts_close) {
               position += 2;
               tokens.emplace_back(XPathTokenType::CLOSE_TAG_OPEN, input.substr(start, 2), start, 2);
               inside_direct_tag = true;
               pending_close_tag = true;
               continue;
            }
            else if (starts_pi) {
               position += 2;
               tokens.emplace_back(XPathTokenType::PI_START, input.substr(start, 2), start, 2);
               inside_direct_tag = true;
               pending_close_tag = false;
               continue;
            }
            else {
               position++;
               tokens.emplace_back(XPathTokenType::TAG_OPEN, input.substr(start, 1), start, 1);
               inside_direct_tag = true;
               pending_close_tag = false;
               direct_constructor_depth++;
               continue;
            }
         }

         position++;
         tokens.emplace_back(XPathTokenType::LESS_THAN, input.substr(start, 1), start, 1);
         continue;
      }

      if (input[position] IS '*') {
         size_t start = position;
         position++;

         XPathTokenType type = XPathTokenType::WILDCARD;

         bool prev_is_operand = false;
         bool prev_forces_wild = false;

         if (not tokens.empty()) {
            auto prev = tokens.back().type;
            prev_is_operand = (prev IS XPathTokenType::NUMBER) or
                              (prev IS XPathTokenType::STRING) or
                              (prev IS XPathTokenType::IDENTIFIER) or
                              (prev IS XPathTokenType::RPAREN) or
                              (prev IS XPathTokenType::RBRACKET);

            prev_forces_wild = (prev IS XPathTokenType::AT) or
                               (prev IS XPathTokenType::AXIS_SEPARATOR) or
                               (prev IS XPathTokenType::SLASH) or
                               (prev IS XPathTokenType::DOUBLE_SLASH) or
                               (prev IS XPathTokenType::COLON);
         }

         auto is_operand_start = [&](size_t index) {
            if (index >= length) return false;
            char ch = input[index];
            if (is_digit(ch)) return true;
            if (ch IS '.') {
               if (index + 1 < length) {
                  char next = input[index + 1];
                  if (is_digit(next)) return true;
                  if (next IS '.' or next IS '/') return true;
               }
               return true;
            }
            if (ch IS '/') return true;
            if (is_name_start_char(ch)) return true;
            if (ch IS '@' or ch IS '$' or ch IS '(') return true;
            if (ch IS '\'' or ch IS '"') return true;
            return false;
         };

         auto unary_context_before = [&](size_t index) {
            size_t prev = index;
            while (prev > 0 and is_whitespace(input[prev - 1])) prev--;
            if (prev IS 0) return true;

            char before = input[prev - 1];
            if (before IS '(' or before IS '[') return true;
            if (before IS '@' or before IS '$' or before IS ',' or before IS ':') return true;
            if (before IS '+' or before IS '-' or before IS '*' or before IS '/' or before IS '|' or before IS '!' or before IS '<' or before IS '>' or before IS '=') return true;
            return false;
         };

         auto next_operand_index = [&]() -> size_t {
            size_t lookahead = position;
            while (lookahead < length and is_whitespace(input[lookahead])) lookahead++;
            if (lookahead >= length) return std::string_view::npos;

            char next_char = input[lookahead];
            if (next_char IS '-' or next_char IS '+') {
               if (not unary_context_before(lookahead)) return std::string_view::npos;
               size_t after_sign = lookahead + 1;
               while (after_sign < length and is_whitespace(input[after_sign])) after_sign++;
               if (after_sign >= length) return std::string_view::npos;
               return is_operand_start(after_sign) ? after_sign : std::string_view::npos;
            }

            return is_operand_start(lookahead) ? lookahead : std::string_view::npos;
         };

         size_t operand_index = next_operand_index();
         bool inside_structural_context = (bracket_depth > 0) or (paren_depth > 0);

         // Detect if we're in an expression context where numeric/string literals should enable
         // multiplication. This is necessary because expressions like "return 2 * 3" are valid
         // arithmetic but occur outside parentheses/brackets. Expression contexts include positions
         // after keywords like RETURN, ASSIGN (:=), comparison operators, and arithmetic operators.
         // This heuristic complements the structural context check and resolves the ambiguity
         // between wildcard (*) and multiply (*) for top-level arithmetic expressions.
         //
         // When the previous token is a NUMBER or STRING, we look at the token before that to
         // determine if we entered an expression context. For other operand types, we check the
         // immediate previous token. If we're at the start of input (tokens is empty), we also
         // treat it as an expression context to handle top-level arithmetic.

         // TODO: Could be optimised by building a boolean table.

         auto is_expr_context_introducer = [](XPathTokenType Type) -> bool {
            return (Type IS XPathTokenType::RETURN) or
                   (Type IS XPathTokenType::ASSIGN) or
                   (Type IS XPathTokenType::COMMA) or
                   (Type IS XPathTokenType::THEN) or
                   (Type IS XPathTokenType::ELSE) or
                   (Type IS XPathTokenType::EQUALS) or
                   (Type IS XPathTokenType::NOT_EQUALS) or
                   (Type IS XPathTokenType::LESS_THAN) or
                   (Type IS XPathTokenType::LESS_EQUAL) or
                   (Type IS XPathTokenType::GREATER_THAN) or
                   (Type IS XPathTokenType::GREATER_EQUAL) or
                   (Type IS XPathTokenType::EQ) or
                   (Type IS XPathTokenType::NE) or
                   (Type IS XPathTokenType::LT) or
                   (Type IS XPathTokenType::LE) or
                   (Type IS XPathTokenType::GT) or
                   (Type IS XPathTokenType::GE) or
                   (Type IS XPathTokenType::PLUS) or
                   (Type IS XPathTokenType::MINUS) or
                   (Type IS XPathTokenType::MULTIPLY) or
                   (Type IS XPathTokenType::DIVIDE) or
                   (Type IS XPathTokenType::MODULO);
         };

         bool in_expression_context = tokens.empty();  // Start-of-input is expression context
         if (not tokens.empty()) {
            auto prev_type = tokens.back().type;

            if ((prev_type IS XPathTokenType::NUMBER) or (prev_type IS XPathTokenType::STRING)) {
               if (tokens.size() >= 2) {
                  auto before_operand_type = tokens[tokens.size() - 2].type;
                  in_expression_context = is_expr_context_introducer(before_operand_type);
               }
               else {
                  // If we have exactly one token (a NUMBER or STRING at start of input),
                  // treat it as expression context to allow "2 * 3" at the top level
                  in_expression_context = true;
               }
            }
            else {
               // For other operand types (identifiers, closing parens/brackets), check immediate prev
               in_expression_context = is_expr_context_introducer(prev_type);
            }
         }

         bool prev_allows_binary = false;
         if (not tokens.empty()) {
            auto prev_type = tokens.back().type;

            if ((prev_type IS XPathTokenType::IDENTIFIER) or
                (prev_type IS XPathTokenType::RPAREN) or
                (prev_type IS XPathTokenType::RBRACKET) or
                (prev_type IS XPathTokenType::WILDCARD)) {
               prev_allows_binary = true;
            }
            else if ((prev_type IS XPathTokenType::NUMBER) or (prev_type IS XPathTokenType::STRING)) {
               // Allow multiplication after numeric/string literals when either:
               // 1. Inside structural delimiters (parentheses/brackets), or
               // 2. In an expression context (after keywords/operators that introduce expressions)
               // This enables "return 2 * 3" while still preserving wildcard semantics in "/root/*"
               if (inside_structural_context or in_expression_context) prev_allows_binary = true;
            }
         }

         if (prev_is_operand and prev_allows_binary and !prev_forces_wild and operand_index != std::string_view::npos) {
            type = XPathTokenType::MULTIPLY;
         }

         tokens.emplace_back(type, input.substr(start, 1), start, 1);
         prior_token_type = previous_token_type;
         previous_token_type = tokens.back().type;
      }
      else {
         XPathToken token = scan_operator();
         if (token.type IS XPathTokenType::UNKNOWN) {
            if (current() IS '\'' or current() IS '"') {
               token = scan_string(current(), block);
            }
            else if (is_digit(current()) or (current() IS '.' and is_digit(peek(1)))) {
               token = scan_number();
            }
            else if (is_name_start_char(current())) {
               token = scan_identifier();
            }
            else {
               size_t start = position;
               auto unknown_char = input.substr(start, 1);
               advance();
               token = XPathToken(XPathTokenType::UNKNOWN, unknown_char, start, 1);
            }
         }

         if (token.type IS XPathTokenType::LBRACKET) bracket_depth++;
         else if (token.type IS XPathTokenType::RBRACKET and bracket_depth > 0) bracket_depth--;
         else if (token.type IS XPathTokenType::LPAREN) paren_depth++;
         else if (token.type IS XPathTokenType::RPAREN and paren_depth > 0) paren_depth--;

         prior_token_type = previous_token_type;
         previous_token_type = token.type;
         tokens.push_back(std::move(token));
      }
   }

   tokens.emplace_back(XPathTokenType::END_OF_INPUT, std::string_view(), position, 0);
   previous_token_type = XPathTokenType::END_OF_INPUT;
   return block;
}

//********************************************************************************************************************
// Scans and tokenizes an identifier or keyword from the current position. Checks the scanned name against
// the keyword mappings to determine if it's a reserved word (like 'and', 'or', 'if') or a regular identifier.

XPathToken XPathTokeniser::scan_identifier()
{
   size_t start = position;

   while (position < length and is_name_char(input[position])) {
      position++;
   }

   auto identifier = input.substr(start, position - start);

   auto match = std::ranges::find_if(keyword_mappings, [identifier](const KeywordMapping &entry) {
      return identifier IS entry.text;
   });

   auto is_followed_by_word = [&](std::string_view Expected) -> bool {
      size_t lookahead = position;
      bool saw_separator = false;
      while (lookahead < length and is_whitespace(input[lookahead])) {
         saw_separator = true;
         lookahead++;
      }

      if (!saw_separator) return false;

      size_t word_end = lookahead;
      while (word_end < length and is_name_char(input[word_end])) word_end++;

      size_t word_length = word_end - lookahead;
      if (word_length != Expected.size()) return false;

      return input.compare(lookahead, Expected.size(), Expected) IS 0;
   };

   XPathTokenType type = XPathTokenType::IDENTIFIER;

   auto peek_non_whitespace_char = [&]() -> char {
      size_t lookahead = position;
      while (lookahead < length and is_whitespace(input[lookahead])) lookahead++;
      return lookahead < length ? input[lookahead] : '\0';
   };

   if (match != keyword_mappings.end()) {
      bool treat_as_keyword = true;

      switch (match->type) {
         case XPathTokenType::FUNCTION:
            treat_as_keyword = (previous_token_type IS XPathTokenType::DECLARE) or
                               (previous_token_type IS XPathTokenType::DEFAULT);
            break;
         case XPathTokenType::VARIABLE:
            treat_as_keyword = (previous_token_type IS XPathTokenType::DECLARE);
            break;
         case XPathTokenType::MAP:
         case XPathTokenType::ARRAY:
            treat_as_keyword = (peek_non_whitespace_char() IS '{');
            break;
         case XPathTokenType::NAMESPACE:
            treat_as_keyword = (previous_token_type IS XPathTokenType::DECLARE) or
                               (previous_token_type IS XPathTokenType::DEFAULT) or
                               (previous_token_type IS XPathTokenType::FUNCTION) or
                               (previous_token_type IS XPathTokenType::MODULE);
            break;
         case XPathTokenType::EXTERNAL: {
            bool identifier_precedes = (previous_token_type IS XPathTokenType::IDENTIFIER) and
                                       ((prior_token_type IS XPathTokenType::DOLLAR) or
                                        (prior_token_type IS XPathTokenType::COLON));

            treat_as_keyword = (previous_token_type IS XPathTokenType::DECLARE) or
                               (previous_token_type IS XPathTokenType::VARIABLE) or
                               (previous_token_type IS XPathTokenType::RPAREN) or
                               identifier_precedes;
            break;
         }
         case XPathTokenType::BOUNDARY_SPACE:
         case XPathTokenType::BASE_URI:
            treat_as_keyword = (previous_token_type IS XPathTokenType::DECLARE);
            break;
         default:
            break;
      }

      if (treat_as_keyword) {
         switch (match->type) {
            case XPathTokenType::ORDER: if (is_followed_by_word("by")) type = match->type; break;
            case XPathTokenType::GROUP: if (is_followed_by_word("by")) type = match->type; break;
            case XPathTokenType::STABLE: if (is_followed_by_word("order")) type = match->type; break;
            default: type = match->type; break;
         }
      }

   }

   return XPathToken(type, identifier, start, position - start);
}

//********************************************************************************************************************
// Scans numeric literals from the input, handling both integers and decimal numbers. Parses consecutive
// digits and at most one decimal point to form a valid numeric token.

XPathToken XPathTokeniser::scan_number()
{
   size_t start = position;
   bool seen_dot = false;
   while (position < length) {
      char current = input[position];
      if (is_digit(current)) {
         position++;
         continue;
      }

      if (not seen_dot and current IS '.') {
         seen_dot = true;
         position++;
         continue;
      }
      break;
   }

   auto number_view = input.substr(start, position - start);
   return XPathToken(XPathTokenType::NUMBER, number_view, start, position - start);
}

//********************************************************************************************************************
// Scans string literals enclosed in quotes (single or double). Handles escape sequences for quote characters,
// backslashes, and wildcards. Uses optimised fast-path for strings without escapes, otherwise builds the
// unescaped string content with proper escape processing.

XPathToken XPathTokeniser::scan_string(char QuoteChar, TokenBlock &Block)
{
   size_t start = position;
   position++;
   size_t content_start = position;

   bool has_escapes = false;
   size_t scan_pos = position;
   while ((scan_pos < length) and (input[scan_pos] != QuoteChar)) {
      if (input[scan_pos] IS '\\') {
         has_escapes = true;
         break;
      }
      scan_pos++;
   }

   if (not has_escapes) {
      size_t content_end = scan_pos;
      position = scan_pos;
      if (position < length) position++;

      auto string_content = input.substr(content_start, content_end - content_start);
      return XPathToken(XPathTokenType::STRING, string_content, start, position - start);
   }

   std::string value;
   value.reserve(scan_pos - content_start + 10);

   while ((position < length) and (input[position] != QuoteChar)) {
      if (input[position] IS '\\' and position + 1 < length) {
         position++;
         char escaped = input[position];
         if (escaped IS QuoteChar or escaped IS '\\' or escaped IS '*') value += escaped;
         else {
            value += '\\';
            value += escaped;
         }
      }
      else value += input[position];

      position++;
   }

   if (position < length) position++;

   std::string_view stored_view = Block.write_copy(value);
   return XPathToken(XPathTokenType::STRING, stored_view, start, position - start, TokenTextKind::ArenaOwned);
}

//********************************************************************************************************************
// Scans an attribute value inside a direct constructor.  When template processing is enabled the function splits the
// string into literal and expression parts so the parser can construct attribute value templates.

XPathToken XPathTokeniser::scan_attribute_value(char QuoteChar, bool ProcessTemplates, TokenBlock &Block)
{
   size_t start = position;
   position++;
   std::vector<XPathAttributeValuePart> parts;
   parts.reserve(4);
   std::string current_literal;
   std::string current_expression;
   // Heuristic pre-sizing to reduce reallocations in typical short attributes
   size_t remaining = (length > position) ? (length - position) : 0;
   size_t hint = remaining > 128 ? 128 : remaining;
   if (hint > 0) current_literal.reserve(hint);
   current_expression.reserve(32);
   bool in_expression = false;
   int brace_depth = 0;

   while (position < length) {
      char ch = input[position];

      if (not in_expression) {
         if (ch IS QuoteChar) break;

         if (ProcessTemplates and ch IS '{') {
            if ((position + 1 < length) and (input[position + 1] IS '{')) {
               current_literal += '{';
               position += 2;
               continue;
            }

            if (!current_literal.empty()) {
               std::string_view literal_view = Block.write_copy(current_literal);
               XPathAttributeValuePart literal_part;
               literal_part.is_expression = false;
               literal_part.text = literal_view;
               literal_part.text_kind = TokenTextKind::ArenaOwned;
               parts.push_back(literal_part);
               current_literal.clear();
            }

            in_expression = true;
            brace_depth = 1;
            position++;
            current_expression.clear();
            continue;
         }
         else if (ProcessTemplates and ch IS '}' and (position + 1 < length) and (input[position + 1] IS '}')) {
            current_literal += '}';
            position += 2;
            continue;
         }

         current_literal += ch;
         position++;
         continue;
      }
      else if (ch IS '\'' or ch IS '"') {
         char expr_quote = ch;
         current_expression += ch;
         position++;
         while (position < length) {
            char inner = input[position];
            current_expression += inner;
            position++;
            if (inner IS expr_quote) break;
            if (inner IS '\\' and position < length) {
               current_expression += input[position];
               position++;
            }
         }
         continue;
      }
      else if (ch IS '{') {
         brace_depth++;
         current_expression += ch;
         position++;
         continue;
      }
      else if (ch IS '}') {
         brace_depth--;
         if (brace_depth IS 0) {
            position++;
            std::string_view expr_view = Block.write_copy(current_expression);
            XPathAttributeValuePart expr_part;
            expr_part.is_expression = true;
            expr_part.text = expr_view;
            expr_part.text_kind = TokenTextKind::ArenaOwned;
            parts.push_back(expr_part);
            current_expression.clear();
            in_expression = false;
            continue;
         }

         current_expression += ch;
         position++;
         continue;
      }

      current_expression += ch;
      position++;
   }

   if (in_expression) {
      // Avoid a temporary string; append unmatched expression marker directly
      current_literal += '{';
      current_literal += current_expression;
      current_expression.clear();
   }

   if (!current_literal.empty() or parts.empty()) {
      std::string_view literal_view = Block.write_copy(current_literal);
      XPathAttributeValuePart literal_tail;
      literal_tail.is_expression = false;
      literal_tail.text = literal_view;
      literal_tail.text_kind = TokenTextKind::ArenaOwned;
      parts.push_back(literal_tail);
      current_literal.clear();
   }

   size_t content_end = position;

   if (position < length) position++;

   auto content_view = input.substr(start + 1, content_end - (start + 1));
   XPathToken token(XPathTokenType::STRING, content_view, start, position - start);
   token.is_attribute_value = true;
   token.attribute_value_parts = std::move(parts);

   return token;
}

//********************************************************************************************************************
// Scans operator tokens including multi-character operators (like '//', '::', '!=') and single-character
// operators. Returns the appropriate token type for recognised operators, or UNKNOWN for unrecognised characters.

XPathToken XPathTokeniser::scan_operator()
{
   size_t start = position;
   char ch = input[position];

   for (const auto &[text, token_type] : multi_char_operators) {
      size_t length_required = text.size();
      if (position + length_required > length) continue;
      if (input.compare(position, length_required, text) IS 0) {
         position += length_required;
         return XPathToken(token_type, input.substr(start, length_required), start, length_required);
      }
   }

   auto single_char = input.substr(position, 1);
   switch (ch) {
      case '/': position++; return XPathToken(XPathTokenType::SLASH, single_char, start, 1);
      case '.': position++; return XPathToken(XPathTokenType::DOT, single_char, start, 1);
      case '*': position++; return XPathToken(XPathTokenType::WILDCARD, single_char, start, 1);
      case '[': position++; return XPathToken(XPathTokenType::LBRACKET, single_char, start, 1);
      case ']': position++; return XPathToken(XPathTokenType::RBRACKET, single_char, start, 1);
      case '(': position++; return XPathToken(XPathTokenType::LPAREN, single_char, start, 1);
      case ')': position++; return XPathToken(XPathTokenType::RPAREN, single_char, start, 1);
      case '@': position++; return XPathToken(XPathTokenType::AT, single_char, start, 1);
      case ',': position++; return XPathToken(XPathTokenType::COMMA, single_char, start, 1);
      case ';': position++; return XPathToken(XPathTokenType::SEMICOLON, single_char, start, 1);
      case '|': position++; return XPathToken(XPathTokenType::PIPE, single_char, start, 1);
      case '=': position++; return XPathToken(XPathTokenType::EQUALS, single_char, start, 1);
      case '<': position++; return XPathToken(XPathTokenType::LESS_THAN, single_char, start, 1);
      case '>': position++; return XPathToken(XPathTokenType::GREATER_THAN, single_char, start, 1);
      case '+': position++; return XPathToken(XPathTokenType::PLUS, single_char, start, 1);
      case '-': position++; return XPathToken(XPathTokenType::MINUS, single_char, start, 1);
      case ':': position++; return XPathToken(XPathTokenType::COLON, single_char, start, 1);
      case '$': position++; return XPathToken(XPathTokenType::DOLLAR, single_char, start, 1);
      case '?': {
         // Only treat as LOOKUP if immediately followed by a lookup key (no whitespace).
         // With whitespace (e.g. "xs:string? external"), treat as occurrence indicator.
         char immediate_next = (position + 1 < length) ? input[position + 1] : '\0';
         bool lookup_context = false;
         if (immediate_next IS '(' or immediate_next IS '*' or is_digit(immediate_next) or is_name_start_char(immediate_next)) {
            lookup_context = true;
         }
         XPathTokenType type = lookup_context ? XPathTokenType::LOOKUP : XPathTokenType::QUESTION_MARK;
         position++;
         return XPathToken(type, single_char, start, 1);
      }
   }

   return XPathToken(XPathTokenType::UNKNOWN, std::string_view(""), start, 0);
}
