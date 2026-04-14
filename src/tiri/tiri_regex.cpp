/*********************************************************************************************************************

Examples:

  reg = regex.new("\\d+", REGEX_ICASE)
  matches = reg.match("Hello 123 World")
  result = reg.replace("abc123def", "XXX")

*********************************************************************************************************************/

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/modules/regex.h>
#include <kotuku/strings.hpp>
#include <string>
#include <string_view>
#include <cctype>

#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_array.h"
#include "lj_str.h"
#include "lj_gc.h"
#include "lj_proto_registry.h"
#include "hashes.h"
#include "defs.h"

struct regex_callback {
   lua_State *lua_state;
   std::string_view subject;
   int result_index = 0;
   int result_len = 0;
   int total_captures = 0;
   GCarray *results = nullptr;   // For array-based multi-match results
   GCarray *captures = nullptr;  // For single-match capture results

   explicit regex_callback(lua_State *LuaState)
      : lua_state(LuaState) {}
};

//*********************************************************************************************************************
// Dynamic loader for the Regex functionality.  We only load it as needed due to the size of the module.

static ERR load_regex(void)
{
#ifndef KOTUKU_STATIC
   if (not modRegex) {
      pf::SwitchContext ctx(glTiriContext);
      if (objModule::load("regex", &modRegex, &RegexBase) != ERR::Okay) return ERR::InitModule;
   }
#endif
   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR match_many(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, regex_callback &Meta)
{
   auto lua = Meta.lua_state;

   bool skip_match = false;
   if ((MatchStart > 0) and (Captures.size() > 1)) {
      const std::string_view &full_match = Captures[0];
      const std::string_view &first_group = Captures[1];

      if ((full_match.size() == first_group.size()) and (full_match.size() > 0) and (MatchStart <= Meta.subject.size())) {
         auto preceding_char = (uint8_t)Meta.subject[MatchStart - 1];
         auto match_char = (uint8_t)full_match.front();

         if (std::isalpha(preceding_char) and std::isalpha(match_char)) {
            bool preceding_is_word_start = false;
            if (MatchStart >= 2) {
               auto pre_preceding = (uint8_t)Meta.subject[MatchStart - 2];
               preceding_is_word_start = (not std::isalnum(pre_preceding)) and (pre_preceding != '_');
            }

            if (preceding_is_word_start) skip_match = true;
         }
      }
   }

   if (skip_match) return ERR::Okay;

   // Grow results array if needed
   auto slot = MSize(Meta.result_index);
   if (slot >= Meta.results->capacity) {
      lj_array_grow(lua, Meta.results, slot + 8);
   }

   // Create string array for captures
   auto count = uint32_t(Captures.size());
   GCarray *capture_arr = lj_array_new(lua, count, AET::STR_GC);
   GCRef *refs = capture_arr->get<GCRef>();

   // Captures are normalised: unmatched optional groups appear as empty entries to preserve indices.
   for (uint32_t j = 0; j < count; ++j) {
      GCstr *s;
      if (Captures[j].data()) s = lj_str_new(lua, Captures[j].data(), Captures[j].length());
      else s = lj_str_new(lua, "", 0);

      setgcref(refs[j], obj2gco(s));
      lj_gc_objbarrier(lua, capture_arr, s);
   }

   // Store capture array in results array
   setgcref(Meta.results->get<GCRef>()[slot], obj2gco(capture_arr));
   lj_gc_objbarrier(lua, Meta.results, capture_arr);

   Meta.result_index = slot + 1;

   return ERR::Okay;
}

//*********************************************************************************************************************
// Differs to match_many() in that it only ever returns one match without the indexed array.
// Returns an array of captured strings.

static ERR match_one(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, regex_callback &Meta)
{
   auto L = Meta.lua_state;

   // Create array with exact size for captures
   auto count = uint32_t(Captures.size());
   GCarray *arr = lj_array_new(L, count, AET::STR_GC);
   GCRef *refs = arr->get<GCRef>();

   // Captures are normalised: unmatched optional groups appear as empty entries to preserve indices.

   for (uint32_t j = 0; j < count; ++j) {
      GCstr *s;
      if (Captures[j].data()) s = lj_str_new(L, Captures[j].data(), Captures[j].length());
      else s = lj_str_new(L, "", 0);

      setgcref(refs[j], obj2gco(s));
      lj_gc_objbarrier(L, arr, s);
   }

   setarrayV(L, L->top++, arr);
   return ERR::Terminate; // Don't match more than once
}

//*********************************************************************************************************************
// Return the indices of the first match along with capture groups.

static ERR match_first(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, regex_callback &Meta)
{
   Meta.result_index = int(MatchStart);
   Meta.result_len = int(MatchEnd - MatchStart);

   // Build capture array if the client used at least 1 bracketed capture
   if (auto count = uint32_t(Captures.size()); count > 1) {
      auto L = Meta.lua_state;
      GCarray *arr = lj_array_new(L, count-1, AET::STR_GC);
      GCRef *refs = arr->get<GCRef>();

      for (uint32_t j = 1; j < count; ++j) { // Skip the first capture (already reflected in the index & length)
         GCstr *s;
         if (Captures[j].data()) s = lj_str_new(L, Captures[j].data(), Captures[j].length());
         else s = lj_str_new(L, "", 0);

         setgcref(refs[j-1], obj2gco(s));
         lj_gc_objbarrier(L, arr, s);
      }

      Meta.captures = arr;
   }
   return ERR::Terminate;
}

//*********************************************************************************************************************
// Extract captures as result values

static ERR match_extract(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, regex_callback &Meta)
{
   Meta.result_index = int(MatchStart);
   Meta.result_len = int(MatchEnd - MatchStart);

   if (auto count = uint32_t(Captures.size()); count > 1) {
      auto L = Meta.lua_state;
      for (uint32_t j = 1; j < count; ++j) { // Skip the first capture (already reflected in the index & length)
         if (Captures[j].empty()) lua_pushlstring(L, "", 0);
         else lua_pushlstring(L, Captures[j].data(), Captures[j].length());
      }
      Meta.total_captures = int(Captures.size()) - 1;
   }
   return ERR::Terminate;
}

//*********************************************************************************************************************

static ERR match_none(int Index, std::vector<std::string_view> &Captures, size_t MatchStart, size_t MatchEnd, regex_callback &Meta)
{
   return ERR::Terminate;
}

//********************************************************************************************************************
// Constructor: regex.new(pattern [, flags])
// Will throw if compilation of the pattern fails.

static int regex_new(lua_State *Lua)
{
   pf::Log log(__FUNCTION__);

   if (auto error = load_regex(); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to load regex module");
      return 0;
   }

   CSTRING pattern = luaL_checkstring(Lua, 1);
   auto flags = REGEX(luaL_optint(Lua, 2, 0));

   log.trace("Creating regex with pattern: '%s', flags: %d", pattern, int(flags));

   if (auto r = (struct fregex *)lua_newuserdata(Lua, sizeof(struct fregex))) {
      new (r) fregex(pattern, flags);

      // Set metatable immediately so __gc is called even if compilation fails
      luaL_getmetatable(Lua, "Tiri.regex");
      lua_setmetatable(Lua, -2);

      std::string error_msg;
      if (rx::Compile(pattern, flags, &error_msg, &r->regex_obj) != ERR::Okay) {
         luaL_error(Lua, ERR::Syntax, "Regex compilation failed: %s", error_msg.c_str());
      }

      return 1; // userdata is already on stack
   }
   else {
      luaL_error(Lua, ERR::Memory, "Failed to create regex object");
      return 0;
   }
}

//********************************************************************************************************************
// Static method: regex.escape(string) -> string
// Escapes all regex metacharacters in the input string so it can be used as a literal pattern.

static int regex_escape(lua_State *Lua)
{
   size_t len = 0;
   CSTRING input = luaL_checklstring(Lua, 1, &len);

   std::string result;
   result.reserve(len + 16); // Reserve extra space for escape characters

   for (size_t i = 0; i < len; ++i) {
      char c = input[i];
      switch (c) {
         case '\\': case '^': case '$': case '.': case '|':
         case '?': case '*': case '+': case '(': case ')':
         case '[': case ']': case '{': case '}': case '-':
            result += '\\';
            result += c;
            break;
         default:
            result += c;
            break;
      }
   }

   lua_pushlstring(Lua, result.c_str(), result.length());
   return 1;
}

//********************************************************************************************************************
// Method: regex.extract(text, [pos], [flags]) -> captures, ...
// Extracts all captures from the first regex match, returning them as individual values.

static int regex_extract(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");
   size_t text_len = 0;
   CSTRING text = luaL_checklstring(Lua, 1, &text_len);

   auto start = luaL_optint(Lua, 2, 0);
   if (start < 0) start = 0;
   if (start >= int(text_len)) start = int(text_len);

   auto flags = RMATCH(luaL_optint(Lua, 3, int(RMATCH::NIL)));
   auto meta  = regex_callback { Lua };
   auto cb    = C_FUNCTION(match_extract, &meta);
   if (rx::Search(r->regex_obj, std::string_view(text + start, text_len - start), flags, &cb) IS ERR::Okay) {
      return meta.total_captures;
   }
   else return 0;
}

//********************************************************************************************************************
// Method: regex.findFirst(text, [pos], [flags]) -> start, end, [captures]
// This is the fastest available means for searching for the position of a match.
// Returns nil on failure, or the start and end of the first match/capture.

static int regex_findFirst(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");
   size_t text_len = 0;
   CSTRING text = luaL_checklstring(Lua, 1, &text_len);

   auto start = luaL_optint(Lua, 2, 0);
   if (start < 0) start = 0;
   if (start >= int(text_len)) start = int(text_len);

   auto flags = RMATCH(luaL_optint(Lua, 3, int(RMATCH::NIL)));
   auto meta = regex_callback { Lua };
   auto cb = C_FUNCTION(match_first, &meta);
   if (rx::Search(r->regex_obj, std::string_view(text + start, text_len - start), flags, &cb) IS ERR::Okay) {
      // Adjust the returned position to account for the starting offset
      lua_pushinteger(Lua, int(start) + meta.result_index);
      lua_pushinteger(Lua, int(start) + meta.result_index + meta.result_len);
      if (meta.captures) setarrayV(Lua, Lua->top++, meta.captures);
      else lua_pushnil(Lua);
      return 3;
   }
   else return 0;
}

//********************************************************************************************************************
// Iterator function for findAll. Upvalues: [1] regex, [2] text, [3] current_pos, [4] flags

static int regex_findAll_iter(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");

   size_t text_len = 0;
   CSTRING text = lua_tolstring(Lua, lua_upvalueindex(2), &text_len);
   auto current_pos = size_t(lua_tointeger(Lua, lua_upvalueindex(3)));
   auto flags = RMATCH(lua_tointeger(Lua, lua_upvalueindex(4)));

   if (current_pos >= text_len) {
      lua_pushnil(Lua);
      return 1;
   }

   auto meta = regex_callback { Lua };
   auto cb = C_FUNCTION(match_first, &meta);
   if (rx::Search(r->regex_obj, std::string_view(text + current_pos, text_len - current_pos), flags, &cb) IS ERR::Okay) {
      auto match_pos = current_pos + meta.result_index;
      auto match_len = meta.result_len;

      // Update position for next iteration. Advance by at least 1 to avoid infinite loops on zero-width matches.
      auto next_pos = match_pos + (match_len > 0 ? match_len : 1);
      lua_pushinteger(Lua, int(next_pos));
      lua_replace(Lua, lua_upvalueindex(3));

      lua_pushinteger(Lua, int(match_pos));
      lua_pushinteger(Lua, int(match_pos) + meta.result_len);
      if (meta.captures) setarrayV(Lua, Lua->top++, meta.captures);
      else lua_pushnil(Lua);
      return 3;
   }

   lua_pushnil(Lua);
   return 1;
}

//********************************************************************************************************************
// Method: regex.findAll(text, [pos], [flags]) -> iterator
// Returns an iterator function for use in for loops:
//    for start, end, cap in rx.findAll(text) do ... end

static int regex_findAll(lua_State *Lua)
{
   //auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");
   luaL_checkstring(Lua, 1); // Validate text argument

   auto start = luaL_optint(Lua, 2, 0);
   auto flags = luaL_optint(Lua, 3, int(RMATCH::NIL));

   // Create closure with upvalues: regex, text, current_pos, flags
   lua_pushvalue(Lua, lua_upvalueindex(1));
   lua_pushvalue(Lua, 1);
   lua_pushinteger(Lua, start);
   lua_pushinteger(Lua, flags);

   lua_pushcclosure(Lua, regex_findAll_iter, 4);
   return 1;
}

//********************************************************************************************************************
// Method: regex.match(text, flags) -> array|nil
// Returns nil on failure, or an array of indexed captures on success.

static int regex_match(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");
   size_t text_len = 0;
   CSTRING text = luaL_checklstring(Lua, 1, &text_len);
   auto flags = RMATCH(luaL_optint(Lua, 2, int(RMATCH::NIL)));

   auto meta = regex_callback { Lua };
   auto cb = C_FUNCTION(match_one, &meta);
   if (rx::Match(r->regex_obj, std::string_view(text, text_len), flags, &cb) IS ERR::Okay) {
      return 1;
   }
   else {
      lua_pushnil(Lua);
      return 1;
   }
}

//********************************************************************************************************************
// Method: regex.search(text, [flags]) -> array|nil
// Returns nil if no matches, otherwise an array of capture arrays.
// TODO: Allow a client callback to be defined after the flags

static int regex_search(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");

   size_t text_len = 0;
   auto text = luaL_checklstring(Lua, 1, &text_len);
   auto flags = RMATCH(luaL_optint(Lua, 2, int(RMATCH::NIL)));

   GCarray *results = lj_array_new(Lua, 0, AET::ARRAY);
   setarrayV(Lua, Lua->top++, results); // Root results to prevent GC during callbacks

   // Use match_many() to populate the array with the matches.
   auto meta = regex_callback { Lua };
   meta.subject = std::string_view(text, text_len);
   meta.results = results;
   auto cb = C_FUNCTION(match_many, &meta);

   if (rx::Search(r->regex_obj, std::string_view(text, text_len), flags, &cb) IS ERR::Okay) {
      // Adjust array length to actual match count
      results->len = MSize(meta.result_index);
      return 1;
   }
   else {
      lua_pop(Lua, 1);
      lua_pushnil(Lua);
      return 1;
   }
}

//********************************************************************************************************************
// Method: regex.replace(text, replacement) -> string

static int regex_replace(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");

   size_t text_len = 0, replace_len = 0;
   auto text = luaL_checklstring(Lua, 1, &text_len);
   auto replacement = luaL_checklstring(Lua, 2, &replace_len);
   auto flags = RMATCH(luaL_optint(Lua, 3, int(RMATCH::NIL)));

   std::string output;
   rx::Replace(r->regex_obj, std::string_view(text, text_len), std::string_view(replacement, replace_len), &output, flags);

   lua_pushlstring(Lua, output.c_str(), output.length());
   return 1;
}

//********************************************************************************************************************
// Method: regex.split(text) -> array
// Returns a first-class array of split string parts.

static int regex_split(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");

   size_t text_len = 0;
   auto text = luaL_checklstring(Lua, 1, &text_len);
   auto flags = RMATCH(luaL_optint(Lua, 2, int(RMATCH::NIL)));

   pf::vector<std::string> parts;
   rx::Split(r->regex_obj, std::string_view(text, text_len), &parts, flags);

   // Create array with exact size
   auto count = uint32_t(parts.size());
   GCarray *arr = lj_array_new(Lua, count, AET::STR_GC);
   GCRef *refs = arr->get<GCRef>();

   for (uint32_t i = 0; i < count; ++i) {
      GCstr *s = lj_str_new(Lua, parts[i].c_str(), parts[i].length());
      setgcref(refs[i], obj2gco(s));
      lj_gc_objbarrier(Lua, arr, s);
   }

   setarrayV(Lua, Lua->top++, arr);
   return 1;
}

//********************************************************************************************************************
// Method: regex.test(text) -> boolean
// Performs a search to see if the regex matches anywhere in the text.

static int regex_test(lua_State *Lua)
{
   auto r = (struct fregex *)get_meta(Lua, lua_upvalueindex(1), "Tiri.regex");
   size_t text_len = 0;
   CSTRING text = luaL_checklstring(Lua, 1, &text_len);
   auto flags = RMATCH(luaL_optint(Lua, 2, int(RMATCH::NIL)));

   auto meta = regex_callback { Lua };
   auto cb = C_FUNCTION(match_none, &meta);
   if (rx::Search(r->regex_obj, std::string_view(text, text_len), flags, &cb) IS ERR::Okay) {
      lua_pushboolean(Lua, true);
      return 1;
   }
   else {
      lua_pushboolean(Lua, false);
      return 1;
   }
}

//********************************************************************************************************************
// Property and method access: __index

constexpr auto HASH_pattern   = pf::strhash("pattern");
constexpr auto HASH_flags     = pf::strhash("flags");
constexpr auto HASH_error     = pf::strhash("error");
constexpr auto HASH_test      = pf::strhash("test");
constexpr auto HASH_match     = pf::strhash("match");
constexpr auto HASH_search    = pf::strhash("search");
constexpr auto HASH_replace   = pf::strhash("replace");
constexpr auto HASH_split     = pf::strhash("split");
constexpr auto HASH_extract   = pf::strhash("extract");
constexpr auto HASH_findFirst = pf::strhash("findFirst");
constexpr auto HASH_findAll   = pf::strhash("findAll");

static int regex_get(lua_State *Lua)
{
   if (auto r = (struct fregex *)luaL_checkudata(Lua, 1, "Tiri.regex")) {
      if (auto field = luaL_checkstring(Lua, 2)) {
         const auto hash = pf::strhash(field);

         switch(hash) {
            case HASH_pattern: lua_pushstring(Lua, r->pattern.c_str()); return 1;
            case HASH_flags: lua_pushinteger(Lua, int(r->flags)); return 1;
            case HASH_error:
               if (r->error_msg.empty()) lua_pushnil(Lua);
               else lua_pushstring(Lua, r->error_msg.c_str());
               return 1;
            case HASH_test:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_test, 1);
               return 1;
            case HASH_match:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_match, 1);
               return 1;
            case HASH_search:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_search, 1);
               return 1;
            case HASH_replace:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_replace, 1);
               return 1;
            case HASH_split:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_split, 1);
               return 1;
            case HASH_extract:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_extract, 1);
               return 1;
            case HASH_findFirst:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_findFirst, 1);
               return 1;
            case HASH_findAll:
               lua_pushvalue(Lua, 1);
               lua_pushcclosure(Lua, regex_findAll, 1);
               return 1;
         }

         luaL_error(Lua, ERR::UnknownProperty, "Unknown regex property/method: %s", field);
      }
      else luaL_error(Lua, ERR::Args, "No field reference provided");
   }
   else luaL_error(Lua, ERR::Args, "Invalid caller, expected Tiri.regex");

   return 0;
}

//********************************************************************************************************************
// Garbage collection: __gc

static int regex_destruct(lua_State *Lua)
{
   if (auto r = (struct fregex *)luaL_checkudata(Lua, 1, "Tiri.regex")) {
      if (r->regex_obj) { FreeResource(r->regex_obj); r->regex_obj = nullptr; }
      r->~fregex();  // Explicitly call destructor to clean up std::string members
   }

   return 0;
}

//********************************************************************************************************************
// String representation: __tostring
// Returns: regex(pattern, flags=flags)

static int regex_tostring(lua_State *Lua)
{
   if (auto r = (struct fregex *)luaL_checkudata(Lua, 1, "Tiri.regex")) {
      std::string desc = "regex(";
      desc += r->pattern;
      if (r->flags != REGEX::NIL) desc += ", flags=" + std::to_string(uint32_t(r->flags));
      desc += ")";

      lua_pushstring(Lua, desc.c_str());
   }
   else lua_pushstring(Lua, "[INVALID REGEX]");

   return 1;
}

//********************************************************************************************************************
// Register the regex interface

void register_regex_class(lua_State *Lua)
{
   static const struct luaL_Reg functions[] = {
      { "new", regex_new },
      { "escape", regex_escape },
      { nullptr, nullptr }
   };

   static const struct luaL_Reg methods[] = {
      { "__index",    regex_get },
      { "__gc",       regex_destruct },
      { "__tostring", regex_tostring },
      { nullptr, nullptr }
   };

   pf::Log(__FUNCTION__).trace("Registering regex interface");

   // Create metatable
   luaL_newmetatable(Lua, "Tiri.regex");
   lua_pushstring(Lua, "Tiri.regex");
   lua_setfield(Lua, -2, "__name");
   lua_pushstring(Lua, "__index");
   lua_pushvalue(Lua, -2);  // Push the Tiri.regex metatable
   lua_settable(Lua, -3);   // metatable.__index = metatable
   luaL_openlib(Lua, nullptr, methods, 0);

   // Create regex module
   luaL_openlib(Lua, "regex", functions, 0);

   // Add flag constants to regex module.  These match the REGEX_* flags but making them available
   // in this way means that scripts don't need to include the regex module.

   lua_getglobal(Lua, "regex");
   if (lua_istable(Lua, -1)) {
      lua_pushinteger(Lua, int(REGEX::ICASE));
      lua_setfield(Lua, -2, "ICASE");

      lua_pushinteger(Lua, int(REGEX::MULTILINE));
      lua_setfield(Lua, -2, "MULTILINE");

      lua_pushinteger(Lua, int(REGEX::DOT_ALL));
      lua_setfield(Lua, -2, "DOT_ALL");

      lua_pushinteger(Lua, int(RMATCH::NOT_BEGIN_OF_LINE));
      lua_setfield(Lua, -2, "NOT_BEGIN_OF_LINE");

      lua_pushinteger(Lua, int(RMATCH::NOT_END_OF_LINE));
      lua_setfield(Lua, -2, "NOT_END_OF_LINE");

      lua_pushinteger(Lua, int(RMATCH::NOT_BEGIN_OF_WORD));
      lua_setfield(Lua, -2, "NOT_BEGIN_OF_WORD");

      lua_pushinteger(Lua, int(RMATCH::NOT_END_OF_WORD));
      lua_setfield(Lua, -2, "NOT_END_OF_WORD");

      lua_pushinteger(Lua, int(RMATCH::NOT_NULL));
      lua_setfield(Lua, -2, "NOT_NULL");

      lua_pushinteger(Lua, int(RMATCH::CONTINUOUS));
      lua_setfield(Lua, -2, "CONTINUOUS");

      lua_pushinteger(Lua, int(RMATCH::PREV_AVAILABLE));
      lua_setfield(Lua, -2, "PREV_AVAILABLE");

      lua_pushinteger(Lua, int(RMATCH::REPLACE_NO_COPY));
      lua_setfield(Lua, -2, "REPLACE_NO_COPY");

      lua_pushinteger(Lua, int(RMATCH::REPLACE_FIRST_ONLY));
      lua_setfield(Lua, -2, "REPLACE_FIRST_ONLY");
   }

   lua_pop(Lua, 1); // Remove regex table from stack

   // Register regex interface prototypes for compile-time type inference
   reg_iface_prototype("regex", "new", { TiriType::Any }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "escape", { TiriType::Str }, { TiriType::Str });
   reg_iface_prototype("regex", "test", { TiriType::Bool }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "match", { TiriType::Array }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "search", { TiriType::Array }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "replace", { TiriType::Str }, { TiriType::Str, TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "split", { TiriType::Array }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("regex", "findFirst", { TiriType::Num, TiriType::Num, TiriType::Array }, { TiriType::Str, TiriType::Num, TiriType::Num });
   reg_iface_prototype("regex", "extract", { TiriType::Str }, { TiriType::Str, TiriType::Num, TiriType::Num });
   reg_iface_prototype("regex", "findAll", { TiriType::Func }, { TiriType::Str, TiriType::Num, TiriType::Num });
}
