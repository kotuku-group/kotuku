// Debug library.
// Copyright © 2025-2026 Paul Manias
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h
//
// Major portions taken verbatim or adapted from the Lua interpreter.
// Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
//
// The debug library provides introspection functions for examining and manipulating the Tiri runtime
// environment.  It includes standard Tiri debug functions as well as Kotuku-specific extensions for
// code validation and annotation management.
//
//   debug.getRegistry()         - Returns the Tiri registry table
//   debug.getMetatable(obj)     - Returns the metatable of any object
//   debug.setMetatable(obj, mt) - Sets the metatable of any object
//   debug.getEnv(obj)           - Returns the environment of a function/thread/userdata
//   debug.setEnv(obj, env)      - Sets the environment of a function/thread/userdata
//   debug.getInfo(f [, what])   - Returns debug information about a function or stack level
//   debug.getLocal(level, idx)  - Returns local variable name and value at stack level
//   debug.setLocal(level, idx, val) - Sets local variable value at stack level
//   debug.getUpvalue(f, idx)    - Returns upvalue name and value from a function
//   debug.setUpvalue(f, idx, val) - Sets upvalue value in a function
//   debug.upvalueID(f, idx)     - Returns unique identifier for an upvalue
//   debug.upvalueJoin(f1, n1, f2, n2) - Makes upvalue n1 of f1 share storage with upvalue n2 of f2
//   debug.getUserValue(u)       - Returns the environment table of a userdata
//   debug.setUserValue(u, t)    - Sets the environment table of a userdata
//   debug.setHook([hook, mask [, count]]) - Sets the debug hook
//   debug.getHook()             - Returns the current hook settings
//   debug.traceback([msg [, level]]) - Returns a traceback string
//   debug.validate(code [, flags]) - Parses code and returns diagnostics without execution
//   debug.locality(name [, level]) - Returns locality of a variable: "local", "upvalue", "global", or "nil"
//   debug.anno.get(func)        - Returns annotation entry for a function
//   debug.anno.set(func, annotations [, source [, name]]) - Sets annotations for a function
//   debug.anno.list()           - Returns shallow copy of entire annotation table
//
// Note: Local variable and upvalue indices in this implementation are 0-based, consistent with
// Tiri's zero-based indexing convention.

#define lib_debug_c
#define LUA_LIB

#include <kotuku/strings.hpp>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_frame.h"
#include "lj_array.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_state.h"
#include "lib.h"
#include "debug/error_guard.h"
#include "debug/filesource.h"
#include "parser/parser_diagnostics.h"
#include "parser/parser_tips.h"
#include "../../defs.h"

#include <cctype>       // For isalnum, isdigit
#include <cstdlib>      // For strtod
#include <string_view>  // For std::string_view
#include <charconv>     // For std::from_chars

#define LEVELS1   12   //  size of the first part of the stack
#define LEVELS2   10   //  size of the second part of the stack

#define LJLIB_MODULE_debug

static void settabss(lua_State *L, CSTRING i, CSTRING v)
{
   lua_pushstring(L, v);
   lua_setfield(L, -2, i);
}

static void settabsi(lua_State *L, CSTRING i, int v)
{
   lua_pushinteger(L, v);
   lua_setfield(L, -2, i);
}

static void settabsb(lua_State *L, CSTRING i, int v)
{
   lua_pushboolean(L, v);
   lua_setfield(L, -2, i);
}

static void treatstackoption(lua_State *L, CSTRING fname)
{
   lua_pushvalue(L, -2);
   lua_remove(L, -3);
   lua_setfield(L, -2, fname);
}

// Convert 0-based user index to 1-based internal slot number.
// Negative indices (varargs) pass through unchanged.

static int32_t debug_idx(int32_t Index)
{
   if (Index >= 0) return Index + 1;  // 0-based: slot 0 → internal 1
   return Index;  // Negative (varargs) unchanged
}

//********************************************************************************************************************
// Internal helper for get/set upvalue operations

static int debug_getupvalue(lua_State *L, int get)
{
   int32_t n = debug_idx(lj_lib_checkint(L, 2));
   lj_lib_checkfunc(L, 1);

   auto name = get ? lua_getupvalue(L, 1, n) : lua_setupvalue(L, 1, n);
   if (name) {
      lua_pushstring(L, name);
      if (not get) return 1;
      copyTV(L, L->top, L->top - 2);
      L->top++;
      return 2;
   }
   return 0;
}

//********************************************************************************************************************

#define KEY_HOOK   (U64x(80000000,00000000)|'h')

static void hookf(lua_State *L, lua_Debug* ar)
{
   static CSTRING const hooknames[] =
   { "call", "return", "line", "count", "tail return" };
   (L->top++)->u64 = KEY_HOOK;
   lua_rawget(L, LUA_REGISTRYINDEX);
   if (lua_isfunction(L, -1)) {
      lua_pushstring(L, hooknames[(int)ar->event]);
      if (ar->currentline >= 0) lua_pushinteger(L, ar->currentline);
      else lua_pushnil(L);
      lua_call(L, 2, 0);
   }
}

//********************************************************************************************************************

static int makemask(CSTRING smask, int count)
{
   int mask = 0;
   if (strchr(smask, 'c')) mask |= LUA_MASKCALL;
   if (strchr(smask, 'r')) mask |= LUA_MASKRET;
   if (strchr(smask, 'l')) mask |= LUA_MASKLINE;
   if (count > 0) mask |= LUA_MASKCOUNT;
   return mask;
}

//********************************************************************************************************************

static char* unmakemask(int mask, char* smask)
{
   int i = 0;
   if (mask & LUA_MASKCALL) smask[i++] = 'c';
   if (mask & LUA_MASKRET) smask[i++] = 'r';
   if (mask & LUA_MASKLINE) smask[i++] = 'l';
   smask[i] = '\0';
   return smask;
}

//********************************************************************************************************************
// Internal helper: Parses a single annotation value from a string view.
//
// Supported value types:
//   - String literals: "text" or 'text' (with escape sequences)
//   - Boolean literals: true, false
//   - Number literals: integer or floating-point
//   - Array literals: [item, item, ...] or {item, item, ...}
//
// Returns true and pushes the value to the Tiri stack on success.
// Returns false on parse error (nothing pushed to stack).

static bool parse_annotation_value(lua_State *L, std::string_view& sv)
{
   // Skip leading whitespace
   while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t' or sv.front() IS '\n')) {
      sv.remove_prefix(1);
   }

   if (sv.empty()) return false;

   // String literal (double or single quotes)
   if (sv.front() IS '"' or sv.front() IS '\'') {
      char quote = sv.front();
      sv.remove_prefix(1);

      size_t end_pos = 0;
      while (end_pos < sv.size() and sv[end_pos] != quote) {
         if (sv[end_pos] IS '\\' and end_pos + 1 < sv.size()) end_pos++;  // Skip escaped characters
         end_pos++;
      }
      if (end_pos >= sv.size()) return false;  // Unterminated string

      lua_pushlstring(L, sv.data(), end_pos);
      sv.remove_prefix(end_pos + 1);  // Skip content and closing quote
      return true;
   }

   // Boolean literals
   if (sv.starts_with("true") and (sv.size() IS 4 or (not std::isalnum(sv[4]) and sv[4] != '_'))) {
      lua_pushboolean(L, 1);
      sv.remove_prefix(4);
      return true;
   }
   if (sv.starts_with("false") and (sv.size() IS 5 or (not std::isalnum(sv[5]) and sv[5] != '_'))) {
      lua_pushboolean(L, 0);
      sv.remove_prefix(5);
      return true;
   }

   // Number literal
   if (std::isdigit(sv.front()) or (sv.front() IS '-' and sv.size() > 1 and std::isdigit(sv[1]))) {
      double num;
      auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), num);
      if (ec IS std::errc()) {
         lua_pushnumber(L, num);
         sv.remove_prefix(ptr - sv.data());
         return true;
      }
      return false;
   }

   // Array literal: [item, item, ...] or {item, item, ...}
   if (sv.front() IS '[' or sv.front() IS '{') {
      char close = (sv.front() IS '[') ? ']' : '}';
      sv.remove_prefix(1);
      lua_newtable(L);
      int idx = 0;

      while (not sv.empty() and sv.front() != close) {
         // Skip whitespace
         while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t' or sv.front() IS '\n')) {
            sv.remove_prefix(1);
         }
         if (not sv.empty() and sv.front() IS close) break;

         // Parse array element
         if (not parse_annotation_value(L, sv)) {
            lua_pop(L, 1);  // Pop table
            return false;
         }
         lua_rawseti(L, -2, idx++);

         // Skip whitespace and comma
         while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t' or sv.front() IS '\n')) {
            sv.remove_prefix(1);
         }
         if (not sv.empty() and sv.front() IS ',') sv.remove_prefix(1);
      }

      if (not sv.empty() and sv.front() IS close) sv.remove_prefix(1);
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Internal helper: Parses an annotation string into a Tiri table.
//
// Annotation Syntax:
//   @Name                 - Simple annotation with no arguments
//   @Name(key=value, ...) - Annotation with key-value arguments
//   @Name(bareKey, ...)   - Bare identifiers are treated as key=true
//   @Name; @Other         - Multiple annotations separated by semicolons
//
// Example Input:
//   "@Test(name=\"foo\", labels=[\"a\",\"b\"]); @Requires(network=true)"
//
// Produces an array of annotation entries, each containing:
//   { name = "AnnotationName", args = { key = value, ... } }
//
// Returns true and pushes the annotations array to the Tiri stack on success.
// Returns false on parse error (nothing pushed to stack).

static bool lj_parse_annotation_string(lua_State *L, std::string_view sv)
{
   lua_newtable(L);  // Result array
   int anno_idx = 0;

   while (not sv.empty()) {
      // Skip whitespace and semicolons
      while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t' or sv.front() IS '\n' or sv.front() IS ';')) {
         sv.remove_prefix(1);
      }
      if (sv.empty()) break;

      // Expect @

      if (sv.front() != '@') {
         lua_pop(L, 1);
         return false;
      }
      sv.remove_prefix(1);

      // Parse annotation name (identifier)

      size_t name_len = 0;
      while (name_len < sv.size() and (std::isalnum(sv[name_len]) or sv[name_len] IS '_')) {
         name_len++;
      }

      if (name_len IS 0) {
         lua_pop(L, 1);
         return false;
      }

      lua_newtable(L);  // Annotation entry
      lua_pushlstring(L, sv.data(), name_len);
      lua_setfield(L, -2, "name");
      sv.remove_prefix(name_len);

      // Parse optional arguments
      while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t')) {
         sv.remove_prefix(1);
      }

      if (not sv.empty() and sv.front() IS '(') {
         sv.remove_prefix(1);
         lua_newtable(L);  // Args table

         while (not sv.empty() and sv.front() != ')') {
            // Skip whitespace
            while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t' or sv.front() IS '\n')) {
               sv.remove_prefix(1);
            }
            if (not sv.empty() and sv.front() IS ')') break;

            // Parse key or bare identifier
            size_t key_len = 0;
            while (key_len < sv.size() and (std::isalnum(sv[key_len]) or sv[key_len] IS '_')) {
               key_len++;
            }

            if (key_len IS 0) {
               lua_pop(L, 3);  // Pop args, entry, result
               return false;
            }

            std::string_view key = sv.substr(0, key_len);
            sv.remove_prefix(key_len);

            while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t')) {
               sv.remove_prefix(1);
            }

            if (not sv.empty() and sv.front() IS '=') {
               // key=value pair
               sv.remove_prefix(1);
               while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t')) {
                  sv.remove_prefix(1);
               }

               // Parse value (string, number, bool, array)
               if (not parse_annotation_value(L, sv)) {
                  lua_pop(L, 3);  // Pop args, entry, result
                  return false;
               }
               // Use lua_pushlstring to create key, then lua_rawset
               lua_pushlstring(L, key.data(), key.size());  // Push key
               lua_pushvalue(L, -2);  // Copy value
               lua_rawset(L, -4);     // args[key] = value
               lua_pop(L, 1);         // Pop original value
            }
            else {
               // Bare identifier = true
               lua_pushlstring(L, key.data(), key.size());  // Push key
               lua_pushboolean(L, 1);  // Push value
               lua_rawset(L, -3);      // args[key] = true
            }

            // Skip comma
            while (not sv.empty() and (sv.front() IS ' ' or sv.front() IS '\t')) {
               sv.remove_prefix(1);
            }
            if (not sv.empty() and sv.front() IS ',') sv.remove_prefix(1);
         }

         if (not sv.empty() and sv.front() IS ')') sv.remove_prefix(1);
         lua_setfield(L, -2, "args");
      }
      else {
         // No args - set empty args table
         lua_newtable(L);
         lua_setfield(L, -2, "args");
      }

      lua_rawseti(L, -2, anno_idx++);
   }

   // Convert the temp table to a proper array<table>

   lj_gc_check(L);
   GCarray *arr = lj_array_new(L, anno_idx, AET::TABLE);
   GCRef *refs = (GCRef *)arr->arraydata();
   for (int i = 0; i < anno_idx; i++) {
      lua_rawgeti(L, -1, i);  // Push temp_table[i] onto stack
      GCtab *entry = tabV(L->top - 1);
      setgcref(refs[i], obj2gco(entry));
      lj_gc_anybarriert(L, entry);
      lua_pop(L, 1);
   }
   setarrayV(L, L->top, arr);
   incr_top(L);
   lua_remove(L, -2);  // Remove the temp table, leaving the array on top
   return true;
}

//********************************************************************************************************************
// debug.getRegistry():table
//
// Returns the Tiri registry table, a pre-defined table used by C code to store Tiri values.  The registry is a global
// table accessible only from C code, used to store references that should not be accessible from Tiri code.

LJLIB_CF(debug_getRegistry)
{
   copyTV(L, L->top++, registry(L));
   return 1;
}

//********************************************************************************************************************
// debug.fileSources(): Returns a table of all registered file sources.
//
// Each entry in the returned array contains:
//   index       - File index (0 = main file, 255 = overflow)
//   path        - Full resolved path
//   filename    - Short name for error display
//   namespace   - Declared namespace (empty string if none)
//   firstLine   - First line in unified space
//   sourceLines - Total lines in source file
//   parentIndex - Which file imported this one (0 for main)
//   importLine  - Line in parent where import occurred (0 for main)
//   isOverflow  - True if this is the overflow fallback (index 255)
//
// Example:
//   local sources = debug.fileSources()
//   for i, src in ipairs(sources) do
//      print(src.filename, "imported from", sources[src.parentIndex].filename)
//   end

LJLIB_CF(debug_fileSources)
{
   uint32_t count = uint32_t(L->file_sources.size());

   // Create native array of tables

   lj_gc_check(L);
   GCarray *arr = lj_array_new(L, count, AET::TABLE);
   GCRef *refs = (GCRef *)arr->arraydata();

   // Iterate over all file sources

   for (uint32_t i = 0; i < count; i++) {
      const FileSource* source = get_file_source(L, uint8_t(i));
      if (not source) {
         setnilV((TValue*)&refs[i]);
         continue;
      }

      // Create entry table (9 fields)
      GCtab *entry = lj_tab_new(L, 0, 9);

      // Store table reference in array first (roots it for GC)
      setgcref(refs[i], obj2gco(entry));

      // Populate fields
      TValue *slot;

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "index"));
      setintV(slot, int(i));

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "path"));
      setstrV(L, slot, lj_str_new(L, source->path.c_str(), source->path.size()));

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "filename"));
      setstrV(L, slot, lj_str_new(L, source->filename.c_str(), source->filename.size()));

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "namespace"));
      setstrV(L, slot, lj_str_new(L, source->declared_namespace.c_str(), source->declared_namespace.size()));

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "firstLine"));
      setintV(slot, source->first_line.lineNumber());

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "totalLines"));
      setintV(slot, source->total_lines.lineNumber());

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "parentIndex"));
      setintV(slot, source->parent_file_index);

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "importLine"));
      setintV(slot, source->import_line.lineNumber());

      slot = lj_tab_setstr(L, entry, lj_str_newlit(L, "isOverflow"));
      setboolV(slot, is_file_source_overflow(uint8_t(i)));

      lj_gc_anybarriert(L, entry);
   }

   // Push array onto stack
   setarrayV(L, L->top, arr);
   incr_top(L);

   return 1;
}

//********************************************************************************************************************
// debug.getMetatable(object:any):table
//
// Returns the metatable of the given object, or nil if it has no metatable.  Unlike getmetatable(), this function
// bypasses the __metatable metamethod and always returns the actual metatable.

LJLIB_CF(debug_getMetatable)   LJLIB_REC(.)
{
   lj_lib_checkany(L, 1);
   if (not lua_getmetatable(L, 1)) {
      setnilV(L->top - 1);
   }
   return 1;
}

//********************************************************************************************************************
// debug.setMetatable(object:any, table):any
//
// Sets the metatable for the given object to the given table (which can be nil).  Returns the original object.
// This function bypasses any __metatable protection.
//
// Example: debug.setMetatable(obj, { __index = defaults })

LJLIB_CF(debug_setMetatable)
{
   lj_lib_checktabornil(L, 2);
   L->top = L->base + 2;
   lua_setmetatable(L, 1);
   return 1;
}

//********************************************************************************************************************
// debug.getEnv(object:any):table
//
// Returns the environment of the given object.  The object can be a Tiri function, a thread, or a userdata.
// For functions, this is the table that is used for global variable access within the function.
//
// Example:
//   env = debug.getEnv(myFunction)
//   print(env._G)

LJLIB_CF(debug_getEnv)
{
   lj_lib_checkany(L, 1);
   lua_getfenv(L, 1);
   return 1;
}

//********************************************************************************************************************
// debug.setEnv(object:any, table):any
//
// Sets the environment of the given object to the given table.  The object can be a Tiri function, a thread,
// or a userdata.  For functions, this changes the table used for global variable access.  Throws an error if the
// environment cannot be set (e.g., for C functions)
//
//   object - A function, thread, or userdata
//   table  - The new environment table
//
// Example:
//   sandbox = { print = print }
//   debug.setEnv(untrustedFunc, sandbox)

LJLIB_CF(debug_setEnv)
{
   lj_lib_checktab(L, 2);
   L->top = L->base + 2;
   if (not lua_setfenv(L, 1)) lj_err_caller(L, ErrMsg::SETFENV);
   return 1;
}

//********************************************************************************************************************
// debug.getInfo(function [, what]): table
//
// Returns a table with information about a function or stack level.  The function can be specified as a function
// value or as a number representing a stack level (0 = getInfo itself, 1 = function that called getInfo, etc.).
//
//   function - A function value or stack level number
//   what     - (optional) String specifying which fields to fill (default: "flnSu"):
//              'n' - name, nameWhat
//              'S' - source, shortSource, lineDefined, lastLineDefined, what
//              'l' - currentLine
//              'u' - nups, nParams, isVarArg
//              'f' - func (pushes the function onto the stack)
//              'L' - activeLines (table of valid line numbers)
//
// Returns:
//   table - A table containing the requested information:
//     source         - Source file name
//     shortSource    - Short version of source (for error messages)
//     lineDefined    - Line where the function definition starts
//     lastLineDefined - Line where the function definition ends
//     what           - "Tiri", "C", "main", or "tail"
//     currentLine    - Current line being executed
//     name           - Name of the function (if available)
//     nameWhat       - How the name was obtained ("global", "local", "method", "field", "")
//     nups           - Number of upvalues
//     nParams        - Number of parameters
//     isVarArg       - Whether the function is variadic
//     func           - The function itself
//     activeLines    - Table with line numbers as keys
//
// Returns nil if the stack level is invalid.

LJLIB_CF(debug_getInfo)
{
   lj_Debug ar;
   int opt_f = 0, opt_L = 0;
   bool from_func_arg = false;  // Track if function was passed directly
   CSTRING options = luaL_optstring(L, 2, "flnSu");
   if (lua_isnumber(L, 1)) {
      if (not lua_getstack(L, (int)lua_tointeger(L, 1), (lua_Debug*)&ar)) {
         setnilV(L->top - 1);
         return 1;
      }
   }
   else if (L->base < L->top and tvisfunc(L->base)) {
      from_func_arg = true;
      options = lua_pushfstring(L, ">%s", options);
      setfuncV(L, L->top++, funcV(L->base));
   }
   else lj_err_arg(L, 1, ErrMsg::NOFUNCL);

   if (not lj_debug_getinfo(L, options, &ar, 1)) lj_err_arg(L, 2, ErrMsg::INVOPT);

   // Get function for fileIndex using the frame info
   GCfunc* fn = nullptr;
   if (from_func_arg) {
      // Function was passed as argument, get it from where lj_debug_getinfo left it
      // The '>' case pops it from top, so we need to get it differently
      fn = funcV(L->base);
   }
   else {
      // Stack level case: extract function from frame info
      uint32_t offset = uint32_t(ar.i_ci) & 0xffff;
      if (offset) {
         TValue* frame = tvref(L->stack) + offset;
         fn = frame_func(frame);
      }
   }

   lua_createtable(L, 0, 16);  //  Create result table.
   for (; *options; options++) {
      switch (*options) {
         case 'S':
            settabss(L, "source", ar.source);
            settabss(L, "shortSource", ar.short_src);
            settabsi(L, "lineDefined", ar.linedefined);
            settabsi(L, "lastLineDefined", ar.lastlinedefined);
            settabss(L, "what", ar.what);
            // Add fileIndex for FileSource lookup
            if (fn and isluafunc(fn)) {
               GCproto* pt = funcproto(fn);
               settabsi(L, "fileIndex", pt->file_source_idx);
            }
            break;
         case 'l':
            settabsi(L, "currentLine", ar.currentline);
            break;
         case 'u':
            settabsi(L, "nups", ar.nups);
            settabsi(L, "nParams", ar.nparams);
            settabsb(L, "isVarArg", ar.isvararg);
            break;
         case 'n':
            settabss(L, "name", ar.name);
            settabss(L, "nameWhat", ar.namewhat);
            break;
         case 'f': opt_f = 1; break;
         case 'L': opt_L = 1; break;
         default: break;
      }
   }
   if (opt_L) treatstackoption(L, "activeLines");
   if (opt_f) treatstackoption(L, "func");
   return 1;  //  Return result table.
}

//********************************************************************************************************************
// debug.getLocal(level, index):<str, any>
// debug.getLocal(func, index):str
//
// Returns the name and value of a local variable at the given stack level and index.  If given a function instead
// of a level, returns only the name of the parameter at the given index (useful for introspecting parameter names).
//
//   level  - Stack level (1 = function that called getLocal, etc.)
//   func   - Alternative: a function to query parameter names
//   index  - 0-based local variable index (negative indices access vararg values)
//
// Notes:
//   - Negative indices access vararg parameters
//   - Variables include parameters, declared locals, and temporaries
//
// Example:
//   name, value = debug.getLocal(1, 0)  -- First local in caller
//   if name then print(name, "=", value) end

LJLIB_CF(debug_getLocal)
{
   lua_Debug ar;

   int slot = debug_idx(lj_lib_checkint(L, 2));
   if (tvisfunc(L->base)) {
      L->top = L->base + 1;
      lua_pushstring(L, lua_getlocal(L, nullptr, slot));
      return 1;
   }

   if (not lua_getstack(L, lj_lib_checkint(L, 1), &ar)) lj_err_arg(L, 1, ErrMsg::LVLRNG);

   if (auto name = lua_getlocal(L, &ar, slot)) {
      lua_pushstring(L, name);
      lua_pushvalue(L, -2);
      return 2;
   }
   else {
      setnilV(L->top - 1);
      return 1;
   }
}

//********************************************************************************************************************
// debug.setLocal(level, index, value): str
//
// Sets the value of a local variable at the given stack level and index.  Returns the name of the variable,
// or nil if the index is out of range.
//
//   level  - Stack level (1 = function that called setLocal, etc.)
//   index  - 0-based local variable index (negative indices access vararg values)
//   value  - The new value to assign
//
// Note: This modifies the actual variable in the running code
//
// Example:
//   debug.setLocal(1, 0, "new value")  -- Set first local in caller

LJLIB_CF(debug_setLocal)
{
   lua_Debug ar;
   if (not lua_getstack(L, lj_lib_checkint(L, 1), &ar)) lj_err_arg(L, 1, ErrMsg::LVLRNG);
   TValue *tv = lj_lib_checkany(L, 3);
   copyTV(L, L->top++, tv);
   lua_pushstring(L, lua_setlocal(L, &ar, debug_idx(lj_lib_checkint(L, 2))));
   return 1;
}

//********************************************************************************************************************
// debug.getUpvalue(func, index):<str, any>
//
// Returns the name and value of an upvalue from a function.  Upvalues are external local variables that the
// function uses and that are captured in the function's closure.  For C functions, upvalue names are always ""
//
//   func  - A Tiri function
//   index - 0-based upvalue index

LJLIB_CF(debug_getUpvalue)
{
   return debug_getupvalue(L, 1);
}

//********************************************************************************************************************
// debug.setUpvalue(func, index, value):str
//
// Sets the value of an upvalue in a function.  Returns the name of the upvalue, or nil if the index is out of range.
// Note: This modifies the upvalue for all closures sharing it.
//
//   func  - A Tiri function
//   index - 0-based upvalue index
//   value - The new value to assign
//
// Example:
//   debug.setUpvalue(myFunc, 0, newValue)

LJLIB_CF(debug_setUpvalue)
{
   lj_lib_checkany(L, 3);
   return debug_getupvalue(L, 0);
}

//********************************************************************************************************************
// debug.upvalueID(func, index):any
//
// Returns a unique identifier for the upvalue at the given index in the given function.  This identifier can be
// used to check if two closures share the same upvalue (by comparing their upvalue IDs).
// Two upvalues share storage if and only if they have the same ID
//
//   func  - A function
//   index - 0-based upvalue index
//
// Example:
//   id1 = debug.upvalueID(func1, 0)
//   id2 = debug.upvalueID(func2, 0)
//   if id1 is id2 then print("Same upvalue") end

LJLIB_CF(debug_upvalueID)
{
   GCfunc* fn = lj_lib_checkfunc(L, 1);
   int32_t n = debug_idx(lj_lib_checkint(L, 2)) - 1;
   if ((uint32_t)n >= fn->l.nupvalues) lj_err_arg(L, 2, ErrMsg::IDXRNG);
   lua_pushlightuserdata(L, isluafunc(fn) ? (void*)gcref(fn->l.uvptr[n]) : (void*)&fn->c.upvalue[n]);
   return 1;
}

//********************************************************************************************************************
// debug.upvalueJoin(func1, n1, func2, n2)
//
// Makes the n1-th upvalue of func1 refer to the n2-th upvalue of func2.  After this call, both upvalues share
// the same storage - modifying one will affect the other.  Useful for debuggers and profilers that need to:
//
// * Inject logging into existing closures
// * Share breakpoint state across multiple function instances
// * Connect instrumentation code to production closures
//
//   func1 - First Tiri function (must be a Tiri function, not C)
//   n1    - 0-based upvalue index in func1
//   func2 - Second Tiri function (must be a Tiri function, not C)
//   n2    - 0-based upvalue index in func2
//
// Notes:
//   - Both functions must be Tiri functions (not C functions)
//   - After joining, both upvalues share the same value
//
// Example:
//   debug.upvalueJoin(counter1, 0, counter2, 0) -- Make counter1 and counter2 funcs share the same 'count' upvalue

LJLIB_CF(debug_upvalueJoin)
{
   GCfunc * fn[2];
   GCRef * p[2];

   for (int i = 0; i < 2; i++) {
      int32_t n;
      fn[i] = lj_lib_checkfunc(L, 2 * i + 1);
      if (not isluafunc(fn[i])) lj_err_arg(L, 2 * i + 1, ErrMsg::NOLFUNC);
      n = debug_idx(lj_lib_checkint(L, 2 * i + 2)) - 1;
      if ((uint32_t)n >= fn[i]->l.nupvalues) lj_err_arg(L, 2 * i + 2, ErrMsg::IDXRNG);
      p[i] = &fn[i]->l.uvptr[n];
   }
   setgcrefr(*p[0], *p[1]);
   lj_gc_objbarrier(L, fn[0], gcref(*p[1]));
   return 0;
}

//********************************************************************************************************************
// debug.getUserValue(userdata):table
//
// Returns the Tiri value (typically a table) associated with the given userdata, or nil if the userdata has no
// associated value or if the argument is not a userdata.
//
//   userdata - A userdata value

LJLIB_CF(debug_getUserValue)
{
   TValue *o = L->base;
   if (o < L->top and tvisudata(o)) settabV(L, o, tabref(udataV(o)->env));
   else setnilV(o);
   L->top = o + 1;
   return 1;
}

//********************************************************************************************************************
// debug.setUserValue(userdata, value):any
//
// Sets the Tiri value (typically a table) associated with the given userdata.  Returns the userdata.
//
//   userdata - A userdata value
//   value    - A table to associate with the userdata
//
// Throws an error if the first argument is not a userdata or the second is not a table

LJLIB_CF(debug_setUserValue)
{
   TValue* o = L->base;
   if (not (o < L->top and tvisudata(o))) lj_err_argt(L, 1, LUA_TUSERDATA);
   if (not (o + 1 < L->top and tvistab(o + 1))) lj_err_argt(L, 2, LUA_TTABLE);
   L->top = o + 2;
   lua_setfenv(L, 1);
   return 1;
}

//********************************************************************************************************************
// debug.setHook(hook, mask [, count])
//
// Sets the given function as a debug hook.  The hook function is called for the events specified in the mask
// string.  When called without arguments, turns off the hook.
//
//   hook   - A function to be called on hook events, or nil to turn off hooks
//   mask   - A string specifying when the hook is called:
//            'c' - Call: hook is called whenever Tiri calls a function
//            'r' - Return: hook is called whenever Tiri returns from a function
//            'l' - Line: hook is called whenever Tiri executes a new line of code
//   count  - (optional) If > 0, hook is also called after every 'count' instructions
//
// Hook Function Signature:
//   hook(event, line)
//   - event: "call", "return", "line", "count", or "tail return"
//   - line: Current line number (for "line" events), or nil

LJLIB_CF(debug_setHook)
{
   int mask, count;
   lua_Hook func;

   if (lua_isnoneornil(L, 1)) {
      lua_settop(L, 1);
      func = nullptr; mask = 0; count = 0;  //  turn off hooks
   }
   else {
      CSTRING smask = luaL_checkstring(L, 2);
      luaL_checktype(L, 1, LUA_TFUNCTION);
      count = luaL_optint(L, 3, 0);
      func = hookf; mask = makemask(smask, count);
   }

   (L->top++)->u64 = KEY_HOOK;
   lua_pushvalue(L, 1);
   lua_rawset(L, LUA_REGISTRYINDEX);
   lua_sethook(L, func, mask, count);
   return 0;
}

//********************************************************************************************************************
// debug.getHook([thread]):<hook, mask, count>
//
// Returns the current hook settings for the given thread.
//
// Returns:
//   hook  - The current hook function, or nil if none is set
//   mask  - A string with the current hook mask ('c', 'r', 'l')
//   count - The current hook count

LJLIB_CF(debug_getHook)
{
   char buff[5];
   int mask = lua_gethookmask(L);
   lua_Hook hook = lua_gethook(L);
   if (hook != nullptr and hook != hookf) {  // external hook?
      lua_pushliteral(L, "external hook");
   }
   else {
      (L->top++)->u64 = KEY_HOOK;
      lua_rawget(L, LUA_REGISTRYINDEX);   //  get hook
   }
   lua_pushstring(L, unmakemask(mask, buff));
   lua_pushinteger(L, lua_gethookcount(L));
   return 3;
}

//********************************************************************************************************************
// debug.traceback([message [, level]]):str
//
// Returns a string with a traceback of the call stack.  The optional message string is prepended to the traceback.
// The optional level number specifies at which level to start the traceback (default is 1, the function calling
// traceback).
//
//   message - (optional) A message to prepend to the traceback
//   level   - (optional) The stack level to start from (default: 1)

LJLIB_CF(debug_traceback)
{
   CSTRING msg = lua_tostring(L, 1);
   if (msg == nullptr and L->top > L->base) L->top = L->base + 1;
   else luaL_traceback(L, L, msg, lj_lib_optint(L, 2, 1));
   return 1;
}

//********************************************************************************************************************
// debug.validate(statement [, flags]) - Parse code and return diagnostics without execution
//
// Returns a table with:
//   success     - boolean: true if no errors
//   diagnostics - array of diagnostic entries, each containing:
//     line      - 0-based line number
//     column    - 0-based column number
//     endColumn - 0-based end column (column + 1 as approximation)
//     severity  - 0=Info, 1=Warning, 2=Error
//     code      - string error code (e.g., "UnexpectedToken")
//     message   - human-readable error message
//   tips        - array of code improvement hints (LSP severity 4), each containing:
//     line      - 0-based line number
//     column    - 0-based column number
//     endColumn - 0-based end column
//     severity  - 3 (Hint - maps to LSP severity 4)
//     priority  - 1=critical, 2=medium, 3=low
//     category  - hint category (e.g., "performance", "code-quality")
//     message   - human-readable improvement suggestion
//
// Optional flags parameter (string, reserved for future use):
//   "s" - syntax only (default, just parse)
//   "t" - include type checking (future)

static CSTRING diagnostic_code_name(ParserErrorCode Code)
{
   switch (Code) {
      case ParserErrorCode::None:                   return "None";
      case ParserErrorCode::UnexpectedToken:        return "UnexpectedToken";
      case ParserErrorCode::ExpectedToken:          return "ExpectedToken";
      case ParserErrorCode::ExpectedIdentifier:     return "ExpectedIdentifier";
      case ParserErrorCode::UnexpectedEndOfFile:    return "UnexpectedEndOfFile";
      case ParserErrorCode::InternalInvariant:      return "InternalInvariant";
      case ParserErrorCode::ExpectedTypeName:       return "ExpectedTypeName";
      case ParserErrorCode::UnknownTypeName:        return "UnknownTypeName";
      case ParserErrorCode::TypeMismatchArgument:   return "TypeMismatchArgument";
      case ParserErrorCode::TypeMismatchAssignment: return "TypeMismatchAssignment";
      case ParserErrorCode::TypeMismatchReturn:     return "TypeMismatchReturn";
      case ParserErrorCode::DeferredTypeRequired:   return "DeferredTypeRequired";
      case ParserErrorCode::UndefinedVariable:      return "UndefinedVariable";
      case ParserErrorCode::ThunkDirectCall:        return "ThunkDirectCall";
      case ParserErrorCode::ReturnTypeMismatch:     return "ReturnTypeMismatch";
      case ParserErrorCode::ReturnCountMismatch:    return "ReturnCountMismatch";
      case ParserErrorCode::RecursiveFunctionNeedsType: return "RecursiveFunctionNeedsType";
      case ParserErrorCode::TooManyReturnTypes:     return "TooManyReturnTypes";
      case ParserErrorCode::RecoverySkippedTokens:  return "RecoverySkippedTokens";
      default: return "Unknown";
   }
}

LJLIB_CF(debug_validate)
{
   CSTRING statement = luaL_checkstring(L, 1);
   // flags parameter reserved for future use (type checking, etc.)
   // CSTRING flags = luaL_optstring(L, 2, "s");

   // Create result table
   lua_newtable(L);

   // Check that script context is available
   if (L->script IS nullptr) {
      settabsb(L, "success", false);
      lua_newtable(L);
      lua_setfield(L, -2, "diagnostics");
      return 1;
   }

   // Parse the statement using lua_load with DIAGNOSE mode
   // This requires temporarily enabling JOF::DIAGNOSE
   auto *prv = (prvTiri *)L->script->ChildPrivate;
   JOF old_options = prv ? prv->JitOptions : JOF::NIL;
   if (prv) prv->JitOptions |= JOF::DIAGNOSE|JOF::ALL_TIPS;

   int parse_result = lua_load(L, std::string_view(statement, strlen(statement)), "=validate");

   if (prv) prv->JitOptions = old_options;  // Restore options

   // Pop the compiled chunk or error message
   lua_pop(L, 1);

   // Build diagnostics array
   lua_newtable(L);
   int diag_idx = 0;

   if (L->parser_diagnostics) {
      auto *diagnostics = (ParserDiagnostics*)L->parser_diagnostics;
      for (const auto &entry : diagnostics->entries()) {
         lua_newtable(L);  // diagnostic entry

         SourceSpan span = entry.token.span();
         // LSP uses 0-based line/column, Tiri parser uses 1-based
         settabsi(L, "line", span.line > 0 ? span.line - 1 : 0);
         settabsi(L, "column", span.column > 0 ? span.column - 1 : 0);
         settabsi(L, "endColumn", span.column);  // Already correct after -1 adjustment
         settabsi(L, "severity", int(entry.severity));
         settabss(L, "code", diagnostic_code_name(entry.code));
         settabss(L, "message", entry.message.empty() ? "Syntax error" : entry.message.c_str());

         lua_rawseti(L, -2, diag_idx++);
      }

      // Clear diagnostics for next use
      delete diagnostics;
      L->parser_diagnostics = nullptr;
   }

   lua_setfield(L, -2, "diagnostics");

   // Build tips array (code improvement hints)
   lua_newtable(L);
   int tip_idx = 0;

   if (L->parser_tips) {
      auto *tips = L->parser_tips;
      for (const auto &entry : tips->entries()) {
         lua_newtable(L);  // tip entry

         SourceSpan span = entry.token.span();
         // LSP uses 0-based line/column, Tiri parser uses 1-based
         settabsi(L, "line", span.line > 0 ? span.line - 1 : 0);
         settabsi(L, "column", span.column > 0 ? span.column - 1 : 0);
         settabsi(L, "endColumn", span.column);  // Already correct after -1 adjustment
         settabsi(L, "severity", 3);  // Hint severity (maps to LSP severity 4)
         settabsi(L, "priority", entry.priority);
         settabss(L, "category", category_name(entry.category));
         settabss(L, "message", entry.message.c_str());

         lua_rawseti(L, -2, tip_idx++);
      }

      // Clear tips for next use
      delete tips;
      L->parser_tips = nullptr;
   }

   lua_setfield(L, -2, "tips");

   // Set success field
   settabsb(L, "success", parse_result IS 0);

   return 1;
}

//********************************************************************************************************************
// debug.locality(name [, level]):str
//
// Returns the locality of a variable, indicating where it is defined in the current scope.  This is useful for
// debugging and introspection to understand variable resolution.
//
//   name   - The variable name as a string
//   level  - (optional) Stack level to examine (default: 1 = caller's frame)
//
// Returns: local / upvalue / global / nil
//
// Notes:
//   - Search order: locals -> upvalues -> globals
//   - Returns the first match found

LJLIB_CF(debug_locality)
{
   // Check for nil or missing argument
   if (L->base >= L->top or tvisnil(L->base)) {
      lua_pushliteral(L, "nil");
      return 1;
   }

   CSTRING varname = luaL_checkstring(L, 1);
   int level = luaL_optint(L, 2, 1);

   // Get the stack frame at the specified level
   lua_Debug ar;
   if (not lua_getstack(L, level, &ar)) {
      // Invalid level - check global table only
      lua_getglobal(L, varname);
      if (lua_isnil(L, -1)) {
         lua_pop(L, 1);
         lua_pushliteral(L, "nil");
      }
      else {
         lua_pop(L, 1);
         lua_pushliteral(L, "global");
      }
      return 1;
   }

   // Search local variables in the frame
   int slot = 1;
   CSTRING name;
   while ((name = lua_getlocal(L, &ar, slot)) != nullptr) {
      lua_pop(L, 1);  // Pop the value
      if (strcmp(name, varname) == 0) {
         lua_pushliteral(L, "local");
         return 1;
      }
      slot++;
   }

   // Get the function at this level to check upvalues
   if (lua_getinfo(L, "f", &ar)) {
      int uv = 1;
      while ((name = lua_getupvalue(L, -1, uv)) != nullptr) {
         lua_pop(L, 1);  // Pop the value
         if (strcmp(name, varname) == 0) {
            lua_pop(L, 1);  // Pop the function
            lua_pushliteral(L, "upvalue");
            return 1;
         }
         uv++;
      }
      lua_pop(L, 1);  // Pop the function
   }

   // Check global table
   lua_getglobal(L, varname);
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_pushliteral(L, "nil");
   }
   else {
      lua_pop(L, 1);
      lua_pushliteral(L, "global");
   }
   return 1;
}

//********************************************************************************************************************

#define LJLIB_MODULE_debug_anno

//********************************************************************************************************************
// debug.anno.get(func): table
//
// Returns the annotation entry for a function, or nil if no annotations are registered.  The annotation system
// allows attaching metadata to functions for testing frameworks, documentation generators, and other tools.
//
//   func - A function to look up annotations for

LJLIB_CF(debug_anno_get)
{
   lj_lib_checkfunc(L, 1);

   // Get _ANNO global
   lua_getglobal(L, "_ANNO");
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      return 1;
   }

   // Look up annotations for this function: _ANNO[func]
   lua_pushvalue(L, 1);  // Push function reference
   lua_gettable(L, -2);  // Get _ANNO[func]
   lua_remove(L, -2);    // Remove _ANNO table
   return 1;
}

//********************************************************************************************************************
// debug.anno.set(func, annotations [, source [, name]]): table
//
// Sets annotations for a function.  Annotations can be provided as an array<table> (for programmatic use) or as a
// string in annotation syntax (for parsing).  The annotations are stored in the global _ANNO table, indexed by
// function.  Returns the annotation entry that was created/updated.
//
//   func        - The function to annotate
//   annotations - Either:
//                 - An array<table> of annotation objects, where each element is a table with at least a name field
//                 - A string in annotation syntax: "@Name(key=value, ...)"
//   source      - (optional) Source identifier, defaults to "<runtime>"
//   name        - (optional) Function name, falls back to debug info or "<anonymous>"
//
// Annotation String Syntax:
//   "@Test"                         - Simple annotation
//   "@Test(name=\"foo\")"           - With string argument
//   "@Test(enabled=true, count=5)"  - With boolean and number arguments
//   "@Test(labels=[\"a\", \"b\"])"  - With array argument
//   "@Test; @Requires(network)"     - Multiple annotations

LJLIB_CF(debug_anno_set)
{
   lj_lib_checkfunc(L, 1);
   lj_lib_checkany(L, 2);

   auto source = luaL_optstring(L, 3, "<runtime>");
   auto name = luaL_optstring(L, 4, nullptr);

   // Get or create _ANNO global

   lua_getglobal(L, "_ANNO");
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setglobal(L, "_ANNO");
   }

   // Handle string input - parse annotation syntax

   if (lua_isstring(L, 2)) {
      CSTRING str = lua_tostring(L, 2);
      if (not lj_parse_annotation_string(L, str)) {
         lua_pop(L, 1);  // Pop _ANNO
         luaL_error(L, ERR::Syntax, "Failed to parse annotation string");
      }
      // Parsed annotations array is now on stack
   }
   else if (lua_isarray(L, 2)) {
      lua_pushvalue(L, 2);  // Push annotations array
   }
   else {
      lua_pop(L, 1);  // Pop _ANNO
      lj_err_argt(L, 2, LUA_TARRAY);
   }

   // Create entry table with name, source, and annotations

   lua_newtable(L);  // Entry table

   // Get function name: use provided name, fall back to debug info, then "<anonymous>"

   if (name) lua_pushstring(L, name);
   else {
      lua_Debug ar;
      lua_pushvalue(L, 1);
      if (lua_getinfo(L, ">n", &ar) and ar.name) lua_pushstring(L, ar.name);
      else lua_pushliteral(L, "<anonymous>");
   }
   lua_setfield(L, -2, "name");

   // Set source
   lua_pushstring(L, source);
   lua_setfield(L, -2, "source");

   // Set annotations array
   lua_pushvalue(L, -2);  // Push annotations array
   lua_setfield(L, -2, "annotations");
   lua_remove(L, -2);  // Remove standalone annotations array

   // _ANNO[func] = entry
   lua_pushvalue(L, 1);   // Push function reference as key
   lua_pushvalue(L, -2);  // Push entry table as value
   lua_settable(L, -4);   // _ANNO[func] = entry

   lua_remove(L, -2);  // Remove _ANNO table
   return 1;  // Return the entry table
}

//********************************************************************************************************************
// debug.anno.list(): table
//
// Returns a shallow copy of the entire _ANNO table, which contains all registered function annotations.
// The returned table is indexed by function references, with each value being an annotation entry.
//
// Notes:
//   - Returns an empty table if no annotations have been registered
//   - The returned table is a copy; modifying it does not affect _ANNO
//
// Example:
//   all_annotations = debug.anno.list()
//   for func, entry in pairs(all_annotations) do
//      print(entry.name, #entry.annotations, "annotations")
//   end

LJLIB_CF(debug_anno_list)
{
   lua_getglobal(L, "_ANNO");
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);  // Return empty table if _ANNO doesn't exist
   }
   else {
      // Create shallow copy
      lua_newtable(L);
      lua_pushnil(L);
      while (lua_next(L, -3) != 0) {
         lua_pushvalue(L, -2);  // Copy key
         lua_pushvalue(L, -2);  // Copy value
         lua_settable(L, -5);   // Set in new table
         lua_pop(L, 1);         // Pop value, keep key for next iteration
      }
      lua_remove(L, -2);  // Remove original _ANNO
   }
   return 1;
}

//********************************************************************************************************************

#include "lj_libdef.h"  // Includes LJLIB_MODULE_debug table
#include "lj_proto_registry.h"

extern int luaopen_debug(lua_State *L)
{
   LJ_LIB_REG(L, LUA_DBLIBNAME, debug);

   // Register debug.anno as a sub-table of debug
   lua_getglobal(L, LUA_DBLIBNAME);     // Get the debug table we just created
   LJ_LIB_REG(L, nullptr, debug_anno);  // Create anno table without setting in globals
   lua_setfield(L, -2, "anno");         // debug.anno = anno_table
   lua_pop(L, 1);                       // Pop debug table

   // Register debug interface prototypes for compile-time type inference
   reg_iface_prototype("debug", "getRegistry", { TiriType::Table }, {});
   reg_iface_prototype("debug", "fileSources", { TiriType::Array }, {});
   reg_iface_prototype("debug", "getMetatable", { TiriType::Table }, { TiriType::Any });
   reg_iface_prototype("debug", "setMetatable", { TiriType::Any }, { TiriType::Any, TiriType::Table });
   reg_iface_prototype("debug", "getEnv", { TiriType::Table }, { TiriType::Any });
   reg_iface_prototype("debug", "setEnv", { TiriType::Any }, { TiriType::Any, TiriType::Table });
   reg_iface_prototype("debug", "getInfo", { TiriType::Table }, { TiriType::Any, TiriType::Str });
   reg_iface_prototype("debug", "getLocal", { TiriType::Str, TiriType::Any }, { TiriType::Num, TiriType::Num });
   reg_iface_prototype("debug", "setLocal", { TiriType::Str }, { TiriType::Num, TiriType::Num, TiriType::Any });
   reg_iface_prototype("debug", "getUpvalue", { TiriType::Str, TiriType::Any }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("debug", "setUpvalue", { TiriType::Str }, { TiriType::Func, TiriType::Num, TiriType::Any });
   reg_iface_prototype("debug", "upvalueID", { TiriType::Any }, { TiriType::Func, TiriType::Num });
   reg_iface_prototype("debug", "upvalueJoin", {}, { TiriType::Func, TiriType::Num, TiriType::Func, TiriType::Num });
   reg_iface_prototype("debug", "getUserValue", { TiriType::Table }, { TiriType::Any });
   reg_iface_prototype("debug", "setUserValue", { TiriType::Any }, { TiriType::Any, TiriType::Table });
   reg_iface_prototype("debug", "setHook", {}, { TiriType::Func, TiriType::Str, TiriType::Num });
   reg_iface_prototype("debug", "getHook", { TiriType::Func, TiriType::Str, TiriType::Num }, {});
   reg_iface_prototype("debug", "traceback", { TiriType::Str }, { TiriType::Str, TiriType::Num });
   reg_iface_prototype("debug", "validate", { TiriType::Table }, { TiriType::Str, TiriType::Str });
   reg_iface_prototype("debug", "locality", { TiriType::Str }, { TiriType::Str, TiriType::Num });

   // Register debug.anno interface prototypes
   reg_iface_prototype("debug.anno", "get", { TiriType::Table }, { TiriType::Func });
   reg_iface_prototype("debug.anno", "set", { TiriType::Table }, { TiriType::Func, TiriType::Any, TiriType::Str, TiriType::Str });
   reg_iface_prototype("debug.anno", "list", { TiriType::Table }, {});

   return 1;
}
