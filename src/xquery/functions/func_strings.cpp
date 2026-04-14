// XPath String Functions

#include "../eval/eval_detail.h"

//********************************************************************************************************************
// Converts a value to a string representation.
// Example: string(123) returns "123"

XPathVal XPathFunctionLibrary::function_string(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) {
      if (Context.attribute_node) {
         return XPathVal(Context.attribute_node->Value);
      }

      if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_set_value(nodes);
         return XPathVal(node_set_value.to_string());
      }
      return XPathVal(std::string());
   }
   return XPathVal(Args[0].to_string());
}

//********************************************************************************************************************
// Concatenates multiple strings together.
// Example: concat("Hello", " ", "World") returns "Hello World"

XPathVal XPathFunctionLibrary::function_concat(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   // Pre-calculate total length to avoid quadratic behavior
   size_t total_length = 0;
   std::vector<std::string> arg_strings;
   arg_strings.reserve(Args.size());

   for (const auto &arg : Args) {
      arg_strings.emplace_back(arg.to_string());
      total_length += arg_strings.back().length();
   }

   std::string result;
   result.reserve(total_length);

   for (const auto &str : arg_strings) {
      result += str;
   }

   return XPathVal(result);
}

//********************************************************************************************************************
// Converts a sequence of Unicode codepoints to a string.
// Example: codepoints-to-string((72, 101, 108, 108, 111)) returns "Hello"

XPathVal XPathFunctionLibrary::function_codepoints_to_string(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::string());

   const XPathVal &sequence = Args[0];
   size_t length = sequence_length(sequence);
   if (length IS 0) return XPathVal(std::string());

   std::string output;
   output.reserve(length * 4u);

   for (size_t index = 0; index < length; ++index) {
      XPathVal item = extract_sequence_item(sequence, index);
      double numeric = item.to_number();
      if (std::isnan(numeric)) continue;

      auto rounded = (int64_t)std::llround(numeric);
      if (rounded < 0) {
         append_codepoint_utf8(output, 0xFFFDu);
         continue;
      }

      append_codepoint_utf8(output, (uint32_t)rounded);
   }

   (void)Context;
   return XPathVal(output);
}

//********************************************************************************************************************
// Converts a string to a sequence of Unicode codepoints.
// Example: string-to-codepoints("Hello") returns (72, 101, 108, 108, 111)

XPathVal XPathFunctionLibrary::function_string_to_codepoints(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(pf::vector<XTag *>());

   std::string input = Args[0].to_string();

   SequenceBuilder builder;
   size_t offset = 0;

   while (offset < input.length()) {
      int length = 0;
      uint32_t code = UTF8ReadValue(input.c_str() + offset, &length);
      if (length <= 0) {
         code = (unsigned char)input[offset];
         length = 1;
      }

      builder.nodes.push_back(nullptr);
      builder.attributes.push_back(nullptr);
      builder.strings.push_back(std::to_string(code));

      offset += (size_t)length;
   }

   (void)Context;
   return make_sequence_value(std::move(builder));
}

//********************************************************************************************************************
// Compares two strings lexicographically, returning -1, 0, or 1.
// Example: compare("abc", "abd") returns -1

XPathVal XPathFunctionLibrary::function_compare(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal();
   if (Args[0].is_empty() or Args[1].is_empty()) return XPathVal();

   std::string left = Args[0].to_string();
   std::string right = Args[1].to_string();
   std::string collation = (Args.size() > 2) ? Args[2].to_string() : std::string();

   if (!collation.empty() and !xpath_collation_supported(collation)) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
      return XPathVal();
   }

   int result = 0;
   if (left < right) result = -1;
   else if (left IS right) result = 0;
   else result = 1;

   return XPathVal((double)result);
}

//********************************************************************************************************************
// Tests whether two strings are equal based on their Unicode codepoints.
// Example: codepoint-equal("test", "test") returns true

XPathVal XPathFunctionLibrary::function_codepoint_equal(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal();
   if (Args[0].is_empty() or Args[1].is_empty()) return XPathVal();

   std::string first = Args[0].to_string();
   std::string second = Args[1].to_string();

   (void)Context;
   return XPathVal(first IS second);
}

//********************************************************************************************************************
// Tests whether a string starts with a specified prefix.
// Example: starts-with("Hello World", "Hello") returns true

XPathVal XPathFunctionLibrary::function_starts_with(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 2) return XPathVal(false);
   std::string str = Args[0].to_string();
   std::string prefix = Args[1].to_string();
   return XPathVal(str.substr(0, prefix.length()) IS prefix);
}

//********************************************************************************************************************
// Tests whether a string ends with a specified suffix.
// Example: ends-with("Hello World", "World") returns true

XPathVal XPathFunctionLibrary::function_ends_with(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 2) return XPathVal(false);

   std::string input = Args[0].to_string();
   std::string suffix = Args[1].to_string();
   if (suffix.length() > input.length()) return XPathVal(false);

   return XPathVal(input.compare(input.length() - suffix.length(), suffix.length(), suffix) IS 0);
}

//********************************************************************************************************************
// Tests whether a string contains a specified substring.
// Example: contains("Hello World", "lo W") returns true

XPathVal XPathFunctionLibrary::function_contains(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 2) return XPathVal(false);
   std::string str = Args[0].to_string();
   std::string substr = Args[1].to_string();
   return XPathVal(str.find(substr) != std::string::npos);
}

//********************************************************************************************************************
// Returns the substring before the first occurrence of a pattern.
// Example: substring-before("Hello World", " ") returns "Hello"

XPathVal XPathFunctionLibrary::function_substring_before(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 2) return XPathVal(std::string());

   std::string source = Args[0].to_string();
   std::string pattern = Args[1].to_string();

   if (pattern.empty()) return XPathVal(std::string());

   size_t position = source.find(pattern);
   if (position IS std::string::npos) return XPathVal(std::string());

   return XPathVal(source.substr(0, position));
}

//********************************************************************************************************************
// Returns the substring after the first occurrence of a pattern.
// Example: substring-after("Hello World", " ") returns "World"

XPathVal XPathFunctionLibrary::function_substring_after(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() != 2) return XPathVal(std::string());

   std::string source = Args[0].to_string();
   std::string pattern = Args[1].to_string();

   if (pattern.empty()) return XPathVal(source);

   size_t position = source.find(pattern);
   if (position IS std::string::npos) return XPathVal(std::string());

   return XPathVal(source.substr(position + pattern.length()));
}

//********************************************************************************************************************
// Extracts a substring from a string using 1-based indexing.
// Example: substring("Hello World", 7, 5) returns "World"

XPathVal XPathFunctionLibrary::function_substring(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2 or Args.size() > 3) return XPathVal(std::string());

   std::string str = Args[0].to_string();
   if (str.empty()) return XPathVal(std::string());

   double start_pos = Args[1].to_number();
   if (std::isnan(start_pos) or std::isinf(start_pos)) return XPathVal(std::string());

   // XPath uses 1-based indexing
   int start_index = (int)std::round(start_pos) - 1;
   if (start_index < 0) start_index = 0;
   if (start_index >= (int)str.length()) return XPathVal(std::string());

   if (Args.size() IS 3) {
      double length = Args[2].to_number();
      if (std::isnan(length) or std::isinf(length) or length <= 0) return XPathVal(std::string());

      int len = (int)std::round(length);
      int remaining = (int)str.length() - start_index;
      if (len > remaining) len = remaining;

      // For small substrings, avoid extra allocation overhead
      if (len <= 0) return XPathVal(std::string());

      return XPathVal(str.substr(start_index, len));
   }

   // Return substring from start_index to end
   return XPathVal(str.substr(start_index));
}

//********************************************************************************************************************
// Returns the length of a string in characters.
// Example: string-length("Hello") returns 5

XPathVal XPathFunctionLibrary::function_string_length(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string str;
   if (Args.empty()) {
      if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_set_value(nodes);
         str = node_set_value.to_string();
      }
   }
   else str = Args[0].to_string();
   return XPathVal((double)str.length());
}

//********************************************************************************************************************
// Normalises whitespace by trimming and collapsing consecutive spaces.
// Example: normalize-space("  Hello   World  ") returns "Hello World"

XPathVal XPathFunctionLibrary::function_normalize_space(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string str;
   if (Args.empty()) {
      if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_set_value(nodes);
         str = node_set_value.to_string();
      }
   }
   else str = Args[0].to_string();

   // Remove leading and trailing whitespace, collapse internal whitespace
   size_t start = str.find_first_not_of(" \t\n\r");
   if (start IS std::string::npos) return XPathVal(std::string());

   size_t end = str.find_last_not_of(" \t\n\r");
   str = str.substr(start, end - start + 1);

   // Collapse internal whitespace
   std::string result;
   result.reserve(estimate_normalize_space_size(str));
   bool in_whitespace = false;
   for (char c : str) {
      if (c IS ' ' or c IS '\t' or c IS '\n' or c IS '\r') {
         if (not in_whitespace) {
            result += ' ';
            in_whitespace = true;
         }
      }
      else {
         result += c;
         in_whitespace = false;
      }
   }

   return XPathVal(result);
}

//********************************************************************************************************************
// Normalises Unicode characters to a specified form (NFC, NFD, NFKC, NFKD).
// Example: normalize-unicode("café", "NFC") returns normalised form

XPathVal XPathFunctionLibrary::function_normalize_unicode(const std::vector<XPathVal> &Args,
   const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::string());

   std::string input = Args[0].to_string();
   std::string form = (Args.size() > 1) ? Args[1].to_string() : std::string("NFC");

   bool unsupported = false;
   std::string normalised = simple_normalise_unicode(input, form, &unsupported);
   if (unsupported and Context.expression_unsupported) *Context.expression_unsupported = true;

   return XPathVal(normalised);
}

//********************************************************************************************************************
// Joins a sequence of strings with an optional separator.
// Example: string-join(("one", "two", "three"), ", ") returns "one, two, three"

XPathVal XPathFunctionLibrary::function_string_join(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(std::string());

   const XPathVal &sequence = Args[0];
   std::string separator = (Args.size() > 1) ? Args[1].to_string() : std::string();

   size_t length = sequence_length(sequence);
   if (length IS 0) return XPathVal(std::string());

   std::string result;
   for (size_t index = 0; index < length; ++index) {
      if (index > 0) result.append(separator);
      result.append(sequence_item_string(sequence, index));
   }

   (void)Context;
   return XPathVal(result);
}

//********************************************************************************************************************
// Translates characters in a string based on a character mapping.
// Example: translate("abcdef", "abc", "123") returns "123def"

XPathVal XPathFunctionLibrary::function_translate(const std::vector<XPathVal> &Args, const XPathContext &Context) {
   if (Args.size() != 3) return XPathVal(std::string());

   std::string source = Args[0].to_string();
   std::string from = Args[1].to_string();
   std::string to = Args[2].to_string();

   if (source.empty()) return XPathVal(std::string());

   // Pre-size result string based on input length (worst case: no characters removed)
   std::string result;
   result.reserve(source.size());

   // Build character mapping for faster lookups if worth it
   if (from.size() > 10) {
      // Use array mapping for better performance with larger translation sets
      std::array<int, 256> char_map;
      char_map.fill(-1);

      for (size_t i = 0; i < from.size() and i < 256; ++i) {
         unsigned char ch = (unsigned char)from[i];
         if (char_map[ch] IS -1) { // First occurrence takes precedence
            char_map[ch] = (int)i;
         }
      }

      for (char ch : source) {
         unsigned char uch = (unsigned char)ch;
         int index = char_map[uch];
         if (index IS -1) result.push_back(ch);
         else if (index < (int)to.length()) result.push_back(to[index]);
      }
   }
   else {
      // Use simple find for small translation sets
      for (char ch : source) {
         size_t index = from.find(ch);
         if (index IS std::string::npos) result.push_back(ch);
         else if (index < to.length()) result.push_back(to[index]);
      }
   }

   return XPathVal(result);
}

//********************************************************************************************************************
// Converts a string to uppercase.
// Example: upper-case("hello") returns "HELLO"

XPathVal XPathFunctionLibrary::function_upper_case(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string input;

   if (Args.empty()) {
      if (Context.attribute_node) input = Context.attribute_node->Value;
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_value(nodes);
         input = node_value.to_string();
      }
      else input = std::string();
   }
   else input = Args[0].to_string();

   return XPathVal(apply_string_case(input, true));
}

//********************************************************************************************************************
// Converts a string to lowercase.
// Example: lower-case("HELLO") returns "hello"

XPathVal XPathFunctionLibrary::function_lower_case(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string input;

   if (Args.empty()) {
      if (Context.attribute_node) input = Context.attribute_node->Value;
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_value(nodes);
         input = node_value.to_string();
      }
      else input = std::string();
   }
   else input = Args[0].to_string();

   return XPathVal(apply_string_case(input, false));
}

//********************************************************************************************************************
// Converts an IRI (Internationalised Resource Identifier) to URI format by percent-encoding.
// Example: iri-to-uri("http://example.com/café") returns "http://example.com/caf%C3%A9"

XPathVal XPathFunctionLibrary::function_iri_to_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string input;

   if (Args.empty()) {
      if (Context.attribute_node) input = Context.attribute_node->Value;
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_value(nodes);
         input = node_value.to_string();
      }
      else input = std::string();
   }
   else input = Args[0].to_string();

   std::string result;
   result.reserve(input.length() * 3u);

   static const char hex_digits[] = "0123456789ABCDEF";

   for (unsigned char code : input) {
      if (code <= 0x7Fu) {
         result.push_back((char)code);
      }
      else {
         result.push_back('%');
         result.push_back(hex_digits[(code >> 4u) & 0x0Fu]);
         result.push_back(hex_digits[code & 0x0Fu]);
      }
   }

   return XPathVal(result);
}

//********************************************************************************************************************
// Encodes a string for use in a URI by percent-encoding special characters.
// Example: encode-for-uri("hello world") returns "hello%20world"

XPathVal XPathFunctionLibrary::function_encode_for_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string input;

   if (Args.empty()) {
      if (Context.attribute_node) input = Context.attribute_node->Value;
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_value(nodes);
         input = node_value.to_string();
      }
      else input = std::string();
   }
   else input = Args[0].to_string();

   return XPathVal(encode_for_uri_impl(input));
}

//********************************************************************************************************************
// Escapes characters for use in HTML URIs, preserving already-encoded sequences.
// Example: escape-html-uri("a&b") returns "a&amp;b"

XPathVal XPathFunctionLibrary::function_escape_html_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   std::string input;

   if (Args.empty()) {
      if (Context.attribute_node) input = Context.attribute_node->Value;
      else if (Context.context_node) {
         pf::vector<XTag *> nodes = { Context.context_node };
         XPathVal node_value(nodes);
         input = node_value.to_string();
      }
      else input = std::string();
   }
   else input = Args[0].to_string();

   return XPathVal(escape_html_uri_impl(input));
}

//********************************************************************************************************************
// Tests whether a string matches a regular expression pattern.
// Example: matches("hello world", "^hello") returns true

XPathVal XPathFunctionLibrary::function_matches(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2 or Args.size() > 3) return XPathVal(false);

   std::string input = Args[0].to_string();
   std::string pattern = Args[1].to_string();
   std::string flags = (Args.size() IS 3) ? Args[2].to_string() : std::string();

   if (load_regex() != ERR::Okay) return XPathVal(false);

   std::string error_msg;
   Regex *compiled;
   if (rx::Compile(pattern, build_regex_options(flags, Context.expression_unsupported), &error_msg, &compiled) != ERR::Okay) {
      return XPathVal(false);
   }

   bool matched = rx::Search(compiled, input, RMATCH::NIL, nullptr) IS ERR::Okay;
   FreeResource(compiled);
   return XPathVal(matched);
}

//********************************************************************************************************************
// Replaces occurrences of a regular expression pattern with a replacement string.
// Example: replace("hello world", "world", "universe") returns "hello universe"

XPathVal XPathFunctionLibrary::function_replace(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if ((Args.size() < 3) or (Args.size() > 4)) return XPathVal(std::string());

   std::string input = Args[0].to_string();
   std::string pattern = Args[1].to_string();
   std::string replacement = Args[2].to_string();
   std::string flags = (Args.size() IS 4) ? Args[3].to_string() : std::string();

   if (load_regex() != ERR::Okay) return XPathVal(input);

   std::string error_msg;
   Regex *compiled;
   if (rx::Compile(pattern, build_regex_options(flags, Context.expression_unsupported), &error_msg, &compiled) != ERR::Okay) {
      return XPathVal(input);
   }

   std::string replaced;
   if (rx::Replace(compiled, input, replacement, &replaced, RMATCH::NIL) != ERR::Okay) {
      FreeResource(compiled);
      return XPathVal(input);
   }
   else {
      FreeResource(compiled);
      return XPathVal(replaced);
   }
}

//********************************************************************************************************************
// Splits a string into a sequence of strings based on a regular expression pattern.
// Example: tokenize("The quick brown fox", "\s+")

XPathVal XPathFunctionLibrary::function_tokenize(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if ((Args.size() < 2) or (Args.size() > 3)) return XPathVal(pf::vector<XTag *>());

   std::string input = Args[0].to_string();
   std::string pattern = Args[1].to_string();
   std::string flags = (Args.size() IS 3) ? Args[2].to_string() : std::string();

   std::vector<std::string> tokens;

   if (load_regex() != ERR::Okay) return XPathVal(pf::vector<XTag *>());

   if (pattern.empty()) {
      for (size_t index = 0; index < input.length(); ++index) {
         tokens.emplace_back(input.substr(index, 1));
      }
   }
   else {
      std::string error_msg;
      Regex *compiled;
      if (rx::Compile(pattern, build_regex_options(flags, Context.expression_unsupported), &error_msg, &compiled) != ERR::Okay) {
         return XPathVal(pf::vector<XTag *>());
      }

      pf::vector<std::string> ptokens;
      rx::Split(compiled, input, &ptokens, RMATCH::NIL);
      tokens = std::vector<std::string>(std::make_move_iterator(ptokens.begin()), std::make_move_iterator(ptokens.end()));

      if (not tokens.empty() and tokens.back().empty()) tokens.pop_back();

      FreeResource(compiled);
   }

   pf::vector<XTag *> placeholders;
   for (size_t i = 0; i < tokens.size(); ++i) placeholders.push_back(nullptr);
   return XPathVal(placeholders, std::nullopt, tokens);
}

//********************************************************************************************************************
// Analyses a string against a regex pattern, returning matching and non-matching segments with capture groups.
// Example: analyze-string("Order 2024-01-15 received", "(\d+)-(\d+)-(\d+)")
//
// Evaluates to:
//   <analyze-string-result>
//     <non-match>Order </non-match>
//     <match>
//       <group nr="1">2024</group>
//       <group nr="2">01</group>
//       <group nr="3">15</group>
//     </match>
//     <non-match> received</non-match>
//   </analyze-string-result>

inline void append_analyze_string_segment(SequenceBuilder &Builder, std::string_view Segment)
{
   if (Segment.empty()) return;

   Builder.nodes.push_back(nullptr);
   Builder.attributes.push_back(nullptr);
   Builder.strings.push_back(std::format("non-match:{}", Segment));
}

static ERR analyze_string_cb(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, AnalyzeStringState &State)
{
   if (Captures.empty()) return ERR::Okay;

   SequenceBuilder &builder = *State.builder;

   // Handle gap between last match and this match
   if (MatchStart > State.last_offset) {
      std::string_view gap(State.input_data + State.last_offset, MatchStart - State.last_offset);
      append_analyze_string_segment(builder, gap);
   }

   // Add the match itself
   builder.nodes.push_back(nullptr);
   builder.attributes.push_back(nullptr);
   builder.strings.push_back(std::format("match:{}", Captures[0]));

   // Add capture groups
   for (size_t index = 1; index < Captures.size(); ++index) {
      builder.nodes.push_back(nullptr);
      builder.attributes.push_back(nullptr);
      builder.strings.push_back(std::format("group{}:{}", index, Captures[index]));
   }

   // Update last offset to end of this match
   State.last_offset = MatchEnd;

   return ERR::Okay;
}

XPathVal XPathFunctionLibrary::function_analyze_string(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if ((Args.size() < 2) or (Args.size() > 3)) return XPathVal(pf::vector<XTag *>());

   std::string input   = Args[0].to_string();
   std::string pattern = Args[1].to_string();
   std::string flags   = (Args.size() > 2) ? Args[2].to_string() : std::string();

   if (load_regex() != ERR::Okay) return XPathVal(pf::vector<XTag *>());

   std::string error_msg;
   Regex *compiled;
   if (rx::Compile(pattern, build_regex_options(flags, Context.expression_unsupported), &error_msg, &compiled) != ERR::Okay) {
      return XPathVal(pf::vector<XTag *>());
   }

   SequenceBuilder builder;
   AnalyzeStringState state{ &builder, input.data(), input.length(), 0 };
   auto cb = C_FUNCTION(&analyze_string_cb, &state);
   ERR search_result = rx::Search(compiled, input, RMATCH::NIL, &cb);

   if (search_result IS ERR::Okay) {
      if (state.last_offset < state.input_length) {
         std::string_view tail(state.input_data + state.last_offset, state.input_length - state.last_offset);
         append_analyze_string_segment(builder, tail);
      }
   }
   else {
      builder.nodes.push_back(nullptr);
      builder.attributes.push_back(nullptr);
      builder.strings.push_back(std::format("non-match:{}", input));
   }

   FreeResource(compiled);
   return make_sequence_value(std::move(builder));
}

//********************************************************************************************************************
// Resolves a relative URI against a base URI.
// Example: resolve-uri("page.html", "http://example.com/") returns "http://example.com/page.html"

XPathVal XPathFunctionLibrary::function_resolve_uri(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal();

   std::string relative = Args[0].to_string();
   std::string base;

   if ((Args.size() > 1) and (not Args[1].is_empty())) base = Args[1].to_string();
   else if (Context.xml) base = Context.xml->Path;

   if (relative.empty()) {
      if (base.empty()) return XPathVal();
      return XPathVal(base);
   }

   if (is_absolute_uri(relative)) return XPathVal(relative);
   if (base.empty()) return XPathVal();

   std::string resolved = resolve_relative_uri(relative, base);
   return XPathVal(resolved);
}

//********************************************************************************************************************
// Formats a date value according to a picture string.
// Example: format-date("2024-01-15", "[Y0001]-[M01]-[D01]") returns "2024-01-15"

XPathVal XPathFunctionLibrary::function_format_date(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(std::string());
   if (Args[0].is_empty()) return XPathVal();

   std::string value = Args[0].to_string();
   std::string picture = Args[1].to_string();

   if (Args.size() > 2 and not Args[2].is_empty()) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
   }

   DateTimeComponents components;
   if (!parse_date_value(value, components)) return XPathVal(value);

   std::string formatted = format_with_picture(components, picture);
   return XPathVal(formatted);
}

//********************************************************************************************************************
// Formats a time value according to a picture string.
// Example: format-time("14:30:00", "[H01]:[m01]") returns "14:30"

XPathVal XPathFunctionLibrary::function_format_time(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(std::string());
   if (Args[0].is_empty()) return XPathVal();

   std::string value = Args[0].to_string();
   std::string picture = Args[1].to_string();

   if ((Args.size() > 2) and (not Args[2].is_empty())) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
   }

   DateTimeComponents components;
   if (!parse_time_value(value, components)) return XPathVal(value);

   return XPathVal(format_with_picture(components, picture));
}

//********************************************************************************************************************
// Formats a date-time value according to a picture string.
// Example: format-dateTime("2024-01-15T14:30:00", "[Y]-[M01]-[D01] [H01]:[m]") returns "2024-01-15 14:30"

XPathVal XPathFunctionLibrary::function_format_date_time(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(std::string());
   if (Args[0].is_empty()) return XPathVal();

   auto value = Args[0].to_string();
   auto picture = Args[1].to_string();

   if ((Args.size() > 2) and (not Args[2].is_empty())) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
   }

   DateTimeComponents components;
   if (!parse_date_time_components(value, components)) return XPathVal(value);

   return XPathVal(format_with_picture(components, picture));
}

//********************************************************************************************************************
// Formats an integer according to a picture string (e.g., decimal, roman numerals).
// Example: format-integer(42, "001") returns "042"

XPathVal XPathFunctionLibrary::function_format_integer(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(std::string());

   double number = Args[0].to_number();
   if (std::isnan(number) or std::isinf(number)) return XPathVal(std::string());

   if (Args.size() > 2 and not Args[2].is_empty()) {
      if (Context.expression_unsupported) *Context.expression_unsupported = true;
   }

   auto rounded = (int64_t)std::llround(number);
   std::string picture = Args[1].to_string();
   std::string formatted = format_integer_picture(rounded, picture);
   return XPathVal(formatted);
}
