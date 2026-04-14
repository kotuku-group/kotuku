/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
Regex: Provides support for regular expression pattern matching and text processing.

The Regex module provides ECMAScript-compatible regex functionality with Unicode support. It offers efficient pattern
compilation, flexible matching modes, and text manipulation capabilities including search, replace, and split
operations.

Key features include:

<list type="bullet">
<li>ECMAScript (JavaScript) regex syntax.</li>
<li>Full Unicode support including character classes and properties.</li>
<li>Case-insensitive and multiline matching options.</li>
<li>Reusable compiled patterns for optimal performance.</li>
<li>Callback-based result processing for custom handling.</li>
</list>

Note: Tiri scripts are expected to use the built-in regex functions for better integration as opposed to this
module.

-END-

*********************************************************************************************************************/

#define PRV_REGEX_MODULE

#include <kotuku/main.h>
#include <kotuku/modules/regex.h>
#include <kotuku/strings.hpp>
#include "srell/srell.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

static ERR MODInit(OBJECTPTR, struct CoreBase *);
static ERR MODExpunge(void);
static ERR MODOpen(OBJECTPTR);

#include "regex_def.c"

JUMPTABLE_CORE

struct regex_engine : public srell::u8cregex {
   using srell::u8cregex::u8cregex;

   bool resolve_named_capture(const std::string_view &Name, pf::vector<int> *Indices) const
   {
      using view_type = typename srell::re_detail::groupname_mapper<char>::view_type;
      view_type name_view(Name.data(), Name.size());

      const srell::re_detail::ui_l32 *list = this->namedcaptures[name_view];

      if ((not list) or (list[0] IS 0)) return false;

      for (srell::re_detail::ui_l32 i = 1; i <= list[0]; ++i) {
         Indices->push_back((int)list[i]);
      }

      return true;
   }
};

struct extRegex : public Regex {
   regex_engine *srell = nullptr;  // Compiled SRELL regex object

   ~extRegex() {
      delete srell;
   }
};

//********************************************************************************************************************

static std::string map_error_code(uint32_t ErrorCode)
{
   if (not ErrorCode) return "Okay";

   switch (ErrorCode) {
      case srell::regex_constants::error_collate:    return "Invalid collating element";
      case srell::regex_constants::error_ctype:      return "Invalid character class";
      case srell::regex_constants::error_escape:     return "Invalid escape sequence";
      case srell::regex_constants::error_backref:    return "Invalid back reference";
      case srell::regex_constants::error_brack:      return "Mismatched brackets";
      case srell::regex_constants::error_paren:      return "Mismatched parentheses";
      case srell::regex_constants::error_brace:      return "Mismatched braces";
      case srell::regex_constants::error_badbrace:   return "Invalid range quantifier";
      case srell::regex_constants::error_range:      return "Invalid character range";
      case srell::regex_constants::error_space:      return "Insufficient memory";
      case srell::regex_constants::error_badrepeat:  return "Nothing to repeat";
      case srell::regex_constants::error_complexity: return "Pattern is too complex";
      case srell::regex_constants::error_stack:      return "Stack exhausted";
      case srell::regex_constants::error_utf8:       return "Invalid UTF-8 sequence";
      case srell::regex_constants::error_property:   return "Unknown Unicode property";
      case srell::regex_constants::error_noescape:   return "Escape is required in Unicode set mode for: ( ) [ ] { } / - |";
      case srell::regex_constants::error_operator:   return "Invalid set operator in Unicode set mode";
      case srell::regex_constants::error_complement: return "Invalid complement in Unicode set mode";
      case srell::regex_constants::error_modifier:   return "Duplicated or misplaced inline modifier";
      default: break;
   }

   if (ErrorCode IS srell::regex_constants::error_internal) return "error_internal: internal engine failure";

#if defined(SRELL_FIXEDWIDTHLOOKBEHIND)
   if (ErrorCode IS srell::regex_constants::error_lookbehind) return "error_lookbehind: variable width look-behind";
#endif

   return std::string("error_unknown: ") + std::to_string(ErrorCode);
}

//********************************************************************************************************************

static srell::regex_constants::match_flag_type convert_match_flags(RMATCH Flags)
{
   unsigned int native = 0;

   if ((Flags & RMATCH::NOT_BEGIN_OF_LINE) != RMATCH::NIL)  native |= (unsigned int)srell::regex_constants::match_not_bol;
   if ((Flags & RMATCH::NOT_END_OF_LINE) != RMATCH::NIL)    native |= (unsigned int)srell::regex_constants::match_not_eol;
   if ((Flags & RMATCH::NOT_BEGIN_OF_WORD) != RMATCH::NIL)  native |= (unsigned int)srell::regex_constants::match_not_bow;
   if ((Flags & RMATCH::NOT_END_OF_WORD) != RMATCH::NIL)    native |= (unsigned int)srell::regex_constants::match_not_eow;
   if ((Flags & RMATCH::NOT_NULL) != RMATCH::NIL)           native |= (unsigned int)srell::regex_constants::match_not_null;
   if ((Flags & RMATCH::CONTINUOUS) != RMATCH::NIL)         native |= (unsigned int)srell::regex_constants::match_continuous;
   if ((Flags & RMATCH::PREV_AVAILABLE) != RMATCH::NIL)     native |= (unsigned int)srell::regex_constants::match_prev_avail;
   if ((Flags & RMATCH::WHOLE) != RMATCH::NIL)              native |= (unsigned int)srell::regex_constants::match_whole;
   if ((Flags & RMATCH::REPLACE_NO_COPY) != RMATCH::NIL)    native |= (unsigned int)srell::regex_constants::format_no_copy;
   if ((Flags & RMATCH::REPLACE_FIRST_ONLY) != RMATCH::NIL) native |= (unsigned int)srell::regex_constants::format_first_only;

   return (srell::regex_constants::match_flag_type)native;
}

//********************************************************************************************************************
// C++ destructor for cleaning up compiled Regex objects

static ERR regex_free(APTR Address)
{
   ((extRegex *)Address)->~extRegex();
   return ERR::Okay;
}

static ResourceManager glRegexMgr = {
   "Regex",
   &regex_free
};

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;
   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   Module->set(FID_FunctionList, glFunctions);
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   return ERR::Okay;
}

//********************************************************************************************************************

namespace rx {

/*********************************************************************************************************************

-FUNCTION-
Compile: Compiles a regex pattern and returns a regex object.

Use Compile() to compile a regex pattern into a regex object that can be used for matching and searching.  The
compiled regex object can be reused for multiple match or search operations, improving performance.  It must be
removed with ~Core:FreeResource() when no longer needed to avoid memory leaks.

-INPUT-
cpp(strview) Pattern: A regex pattern string.
flags(REGEX) Flags:  Optional flags.
&cpp(str) ErrorMsg: Optional reference for storing custom error messages.
!ptr(struct(Regex)) Result: Pointer to store the created regex object.

-ERRORS-
Okay
NullArgs
AllocMemory
Syntax
-END-

*********************************************************************************************************************/

ERR Compile(const std::string_view &Pattern, REGEX Flags, std::string *ErrorMsg, Regex **Result)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Pattern: '%.*s', Flags: $%.8x", int(Pattern.size()), Pattern.data(), int(Flags));

   extRegex *regex;
   if (AllocMemory(sizeof(struct extRegex), MEM::MANAGED, &regex) IS ERR::Okay) {
      SetResourceMgr(regex, &glRegexMgr);
      new (regex) extRegex();
      regex->Pattern = Pattern;
      regex->Flags = Flags;

      auto reg_flags = srell::regex_constants::ECMAScript | srell::regex_constants::unicodesets;  // Default syntax with Unicode support
      if ((Flags & REGEX::ICASE) != REGEX::NIL)     reg_flags |= srell::regex_constants::icase;
      if ((Flags & REGEX::MULTILINE) != REGEX::NIL) reg_flags |= srell::regex_constants::multiline;
      if ((Flags & REGEX::DOT_ALL) != REGEX::NIL)   reg_flags |= srell::regex_constants::dotall;

      regex->srell = new (std::nothrow) regex_engine(Pattern.data(), Pattern.size(), reg_flags);
      if (not regex->srell) {
         static CSTRING msg = "Regex constructor failed";
         if (ErrorMsg) *ErrorMsg = msg;
         log.msg("%s", msg);
         FreeResource(regex);
         return ERR::AllocMemory;
      }
      else if (auto err = regex->srell->ecode(); err != 0) {
         auto error_msg = map_error_code(err);
         log.warning("Regex compilation failed: %s; Pattern: %.*s", error_msg.c_str(), int(Pattern.size()), Pattern.data());
         if (ErrorMsg) *ErrorMsg = error_msg;
         FreeResource(regex);
         return ERR::Syntax;
      }
      else {
         *Result = regex;
         return ERR::Okay;
      }
   }
   else {
      if (ErrorMsg) *ErrorMsg = "AllocMemory() failed";
      return ERR::AllocMemory;
   }
}

/*********************************************************************************************************************

-FUNCTION-
GetCaptureIndex: Retrieves capture indices for a named group.

Use GetCaptureIndex() to resolve the numeric capture indices associated with a named capture group. ECMAScript allows
multiple groups to share the same name; this function therefore returns every index that matches the provided name.
If no capture groups match the provided name, `ERR::Search` is returned.

-INPUT-
ptr(struct(Regex)) Regex: The compiled regex object.
cpp(strview) Name: The capture group name to resolve.
&cpp(array(int)) Indices: Receives the resulting capture indices.

-ERRORS-
Okay: The name was resolved and Indices populated.
NullArgs: One or more required arguments were null.
Search: The provided name does not exist within the regex.
-END-

*********************************************************************************************************************/

ERR GetCaptureIndex(Regex *Regex, const std::string_view &Name, pf::vector<int> *Indices)
{
   pf::Log log(__FUNCTION__);

   if ((not Regex) or (not Indices)) return log.warning(ERR::NullArgs);

   Indices->clear();

   auto sr = ((const extRegex *)Regex)->srell;
   if (not sr) return log.warning(ERR::NullArgs);

   if (sr->resolve_named_capture(Name, Indices)) return ERR::Okay;
   else return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
Replace: Replaces occurrences of the regex pattern in the input text with a specified replacement string.

Call Replace() to perform regex-based replacements in a given text. The function takes a compiled regex object,
the input text, a replacement string, and optional flags to modify the replacement behavior. The replacement
string can include back-references like `\1`, `\2`, etc., to refer to captured groups from the regex match.

-INPUT-
ptr(struct(Regex)) Regex: The compiled regex object.
cpp(strview) Text: The input text to perform replacements on.
cpp(strview) Replacement: The replacement string, which can include back-references like `\1`, `\2`, etc.
&cpp(str) Output: Receives the resulting string after replacements.
int(RMATCH) Flags: Optional flags to modify the replacement behavior.

-ERRORS-
Okay: Successful execution, does not necessarily mean replacements were made.
NullArgs: One or more required input arguments were null.
-END-

*********************************************************************************************************************/

namespace {

static bool has_flag(RMATCH Flags, RMATCH Flag)
{
   return (Flags & Flag) != RMATCH::NIL;
}

static void append_range(std::string *Output, const char *Begin, const char *End)
{
   if ((not Output) or (not Begin) or (not End)) return;

   if (End >= Begin) Output->append(Begin, (size_t)(End - Begin));
}

static void append_match_substring(std::string *Output, const srell::u8ccmatch &Match, size_t Index)
{
   if (not Output) return;

   if (Index >= Match.size()) return;

   const auto &sub = Match[Index];
   if (not sub.matched) return;

   append_range(Output, &(*sub.first), &(*sub.second));
}

static void append_named_capture(std::string *Output, const srell::u8ccmatch &Match, const std::string_view &Name, regex_engine *Engine)
{
   if ((not Output) or (not Engine) or Name.empty()) return;

   pf::vector<int> indices;
   if (not Engine->resolve_named_capture(Name, &indices)) return;

   for (size_t i = 0; i < indices.size(); ++i) {
      const int value = indices[i];
      if (value < 0) continue;

      const size_t index = (size_t)value;
      if (index >= Match.size()) continue;

      const auto &sub = Match[index];
      if (sub.matched) {
         append_range(Output, &(*sub.first), &(*sub.second));
         break;
      }
   }
}

static void append_replacement(std::string *Output, const std::string_view &Text, size_t MatchBegin, size_t MatchEnd, const srell::u8ccmatch &Match, const std::string_view &Replacement, regex_engine *Engine)
{
   if (not Output) return;

   const char *cursor = Replacement.data();
   const char *const end = cursor + Replacement.size();

   while (cursor != end) {
      if (*cursor != '$') {
         Output->push_back(*cursor);
         ++cursor;
         continue;
      }

      ++cursor;
      if (cursor IS end) {
         Output->push_back('$');
         break;
      }

      const char marker = *cursor;

      if (marker IS '$') {
         Output->push_back('$');
         ++cursor;
      }
      else if (marker IS '&') {
         append_match_substring(Output, Match, 0);
         ++cursor;
      }
      else if (marker IS '`') {
         append_range(Output, Text.data(), Text.data() + MatchBegin);
         ++cursor;
      }
      else if (marker IS '\'') {
         append_range(Output, Text.data() + MatchEnd, Text.data() + Text.size());
         ++cursor;
      }
      else if ((marker IS '<') and Engine) {
         const char *const lt_position = cursor;
         ++cursor;
         const char *const name_begin = cursor;

         while ((cursor != end) and (*cursor != '>')) ++cursor;

         if (cursor IS end) {
            Output->push_back('$');
            cursor = lt_position;
         }
         else {
            const std::string_view name(name_begin, (size_t)(cursor - name_begin));
            append_named_capture(Output, Match, name, Engine);
            ++cursor;
         }
      }
      else if ((marker >= '0') and (marker <= '9')) {
         size_t number = (size_t)(marker - '0');
         ++cursor;

         if ((cursor != end) and (*cursor >= '0') and (*cursor <= '9')) {
            number *= 10;
            number += (size_t)(*cursor - '0');
            ++cursor;
         }

         if (number < Match.size()) {
            append_match_substring(Output, Match, number);
         }
         else {
            Output->push_back('$');
            if (number >= 10u) {
               const size_t first_digit = number / 10u;
               const size_t second_digit = number % 10u;
               Output->push_back(char('0' + first_digit));
               Output->push_back(char('0' + second_digit));
            }
            else Output->push_back(char('0' + number));
         }
      }
      else {
         Output->push_back('$');
         Output->push_back(marker);
         ++cursor;
      }
   }
}

} // namespace

ERR Replace(Regex *Regex, const std::string_view &Text, const std::string_view &Replacement, std::string *Output, RMATCH Flags)
{
   pf::Log log(__FUNCTION__);

   if ((not Regex) or (not Output)) return log.warning(ERR::NullArgs);

   Output->clear();

   auto sr = ((extRegex *)Regex)->srell;
   if (not sr) return log.warning(ERR::NullArgs);

   const auto native_flags = convert_match_flags(Flags);
   const char *const text_begin = Text.data();
   const char *const text_end = text_begin + Text.size();

   srell::u8ccregex_iterator iter(text_begin, text_end, *sr, native_flags);
   srell::u8ccregex_iterator sentinel;

   if (iter IS sentinel) {
      if (not has_flag(Flags, RMATCH::REPLACE_NO_COPY)) Output->assign(Text);
      return ERR::Okay;
   }

   std::string result;
   result.reserve(Text.size() + Replacement.size());

   size_t copy_position = 0;
   const bool copy_segments = not has_flag(Flags, RMATCH::REPLACE_NO_COPY);

   for (; iter != sentinel; ++iter) {
      const srell::u8ccmatch &match = *iter;
      const size_t match_begin = (size_t)match.position(0);
      const size_t match_length = (size_t)match.length(0);
      const size_t match_end = match_begin + match_length;

      if (copy_segments and (match_begin > copy_position)) {
         append_range(&result, text_begin + copy_position, text_begin + match_begin);
      }

      append_replacement(&result, Text, match_begin, match_end, match, Replacement, sr);

      copy_position = match_end;

      if (has_flag(Flags, RMATCH::REPLACE_FIRST_ONLY)) break;
   }

   if (copy_segments and (copy_position < Text.size())) {
      append_range(&result, text_begin + copy_position, text_end);
   }

   *Output = std::move(result);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Search: Performs regex matching.

Call Search() to search for a regex pattern in a given text. The function takes a compiled regex object,
the input text, optional flags to modify the matching behavior, and a callback function to process the
match results. For each match that is found, the callback function is invoked with details about the match.

The C++ prototype for the Callback function is:

<pre>
ERR callback(int Index, std::vector&lt;std::string_view&gt; &Capture, size_t MatchStart, size_t MatchEnd, APTR Meta);
</pre>

Note the inclusion of the `Index` parameter, which indicates the match number (starting from 0). The `MatchStart`
and `MatchEnd` parameters provide explicit byte offsets into the input text for the matched region.

The Capture vector is always normalised so that its size matches the total number of capturing groups defined by the
pattern (including the full match at index 0). Optional groups that did not match are provided as empty
`std::string_view` instances, ensuring consistent indexing across matches.

-INPUT-
ptr(struct(Regex)) Regex: The compiled regex object.
cpp(strview) Text: The input text to perform matching on.
int(RMATCH) Flags: Optional flags to modify the matching behavior.
ptr(func) Callback: Receives the match results.

-ERRORS-
Okay: At least one match was found and processed.
NullArgs: One or more required input arguments were null.
Search: No matches were found.
-END-

*********************************************************************************************************************/

ERR Search(Regex *Regex, const std::string_view &Text, RMATCH Flags, FUNCTION *Callback)
{
   pf::Log log(__FUNCTION__);

   if (not Regex) return log.warning(ERR::NullArgs);

   auto sr = ((extRegex *)Regex)->srell;

   auto begin = srell::u8ccregex_iterator(Text.data(), Text.data() + Text.size(), *sr, convert_match_flags(Flags));
   auto end   = srell::u8ccregex_iterator();

   bool match_found = false;
   int match_index = 0;
   for (srell::u8ccregex_iterator i = begin; not (i IS end); ++i) {
      const srell::u8ccmatch &match = *i;
      std::vector<std::string_view> captures;
      captures.reserve(match.size());
      for (size_t j = 0; j < match.size(); ++j) {
         if (match[j].matched) {
            captures.emplace_back(&(*match[j].first), match[j].length());
            match_found = true;
         }
         else captures.emplace_back();
      }

      if ((not captures.empty()) and (Callback)) {
         // Calculate explicit match offsets
         size_t match_start = match.position(0);
         size_t match_end = match_start + match.length(0);

         ERR error;
         if (Callback->isC()) {
            pf::SwitchContext ctx(Callback->Context);
            auto routine = (ERR(*)(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, APTR))Callback->Routine;
            error = routine(match_index, captures, match_start, match_end, Callback->Meta);
            if (error IS ERR::Terminate) break;
         }
         else if (Callback->isScript()) {
            // TODO: Implement script callback handling
         }
      }
      else break;

      match_index++;
   }

   if (match_found) return ERR::Okay;
   else return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
Split: Split a string into tokens, using a regex pattern to denote the delimiter.

Call Split() to divide a string into multiple tokens based on a regex pattern that defines the delimiters.
The function takes a compiled regex object, the input text, and optional flags to modify the splitting behavior.

The resulting tokens are stored in the provided output array.

If no matches are found, the entire input text is returned as a single token.

-INPUT-
ptr(struct(Regex)) Regex: The compiled regex object.
cpp(strview) Text: The input text to split.
&cpp(array(cpp(str))) Output: Receives the resulting string tokens.
int(RMATCH) Flags: Optional flags to modify the splitting behavior.

-ERRORS-
Okay: The string was successfully split into tokens. If no matches are found, the entire input text is returned as a single token.
NullArgs: One or more required input arguments were null.
-END-

*********************************************************************************************************************/

ERR Split(Regex *Regex, const std::string_view &Text, pf::vector<std::string> *Output, RMATCH Flags)
{
   pf::Log log(__FUNCTION__);

   if ((not Regex) or (not Output)) return log.warning(ERR::NullArgs);

   Output->clear();

   auto sr = ((extRegex *)Regex)->srell;
   auto native_flags = convert_match_flags(Flags);

   const char *text_begin = Text.data();
   const char *text_end = text_begin + Text.size();
   size_t last_index = 0;
   const size_t text_length = Text.size();

   srell::u8ccregex_iterator begin(text_begin, text_end, *sr, native_flags);
   srell::u8ccregex_iterator sentinel;

   for (srell::u8ccregex_iterator iter = begin; not (iter IS sentinel); ++iter) {
      const srell::u8ccmatch &current = *iter;
      size_t match_pos = (size_t)current.position(0);
      size_t match_length = current.length(0);

      if (match_pos > text_length) match_pos = text_length;

      Output->emplace_back(std::string(Text.substr(last_index, match_pos - last_index)));

      size_t next_index = match_pos + match_length;
      if ((match_length IS 0) and (match_pos < text_length)) next_index = match_pos + 1;
      if (next_index > text_length) next_index = text_length;
      last_index = next_index;
   }

   if (last_index <= text_length) {
      Output->emplace_back(std::string(Text.substr(last_index)));
   }

   return ERR::Okay;
}

//********************************************************************************************************************

} // namespace rx

//********************************************************************************************************************

static STRUCTS glStructures = {
   { "Regex", sizeof(struct Regex) }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_regex_module() { return &ModHeader; }
