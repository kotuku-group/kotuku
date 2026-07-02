// Native array library.
// Copyright © 2025-2026 Paul Manias.
//
// TODO: Allow array lifetimes to be linked to Kotuku objects.  This would allow external array data to be managed
// safely without having to be cached.  In the event that the object is destroyed, the array should be marked as invalid
// and the length reduced to 0 to prevent usage.
//
// TODO: Optimise frequently used code paths like push() and pop() for the JIT compiler

#define lib_array_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_strfmt.h"
#include "lj_buf.h"
#include "lj_array.h"
#include "lj_bulk.h"
#include "lj_meta.h"
#include "lib.h"
#include "lib_range.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string_view>
#include <kotuku/strings.hpp>
#include <kotuku/main.h>

#define LJLIB_MODULE_array

static constexpr std::string_view glArrayAppendHelperKey("\x1f" "array.append", 13);

constexpr auto HASH_INT     = kt::strhash("int");
constexpr auto HASH_BYTE    = kt::strhash("byte");
constexpr auto HASH_CHAR    = kt::strhash("char");
constexpr auto HASH_INT16   = kt::strhash("int16");
constexpr auto HASH_INT64   = kt::strhash("int64");
constexpr auto HASH_FLOAT   = kt::strhash("float");
constexpr auto HASH_DOUBLE  = kt::strhash("double");
constexpr auto HASH_STRING  = kt::strhash("string");
constexpr auto HASH_STRUCT  = kt::strhash("struct");
constexpr auto HASH_POINTER = kt::strhash("pointer");
constexpr auto HASH_OBJECT  = kt::strhash("object");
constexpr auto HASH_TABLE   = kt::strhash("table");
constexpr auto HASH_ARRAY   = kt::strhash("array");
constexpr auto HASH_ANY     = kt::strhash("any");

// Forward declarations
static int32_t find_in_array(GCarray *Arr, lua_Number Value, int32_t Start, int32_t Stop, int32_t Step);
static int32_t find_object_in_array(GCarray *Arr, OBJECTID SearchUid, int32_t Start, int32_t Stop, int32_t Step);
static void array_push_element(lua_State *L, GCarray *Arr, MSize Idx);

const array_meta glArrayConversion[size_t(AET::MAX)] = {
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::BYTE
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::INT16
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::INT32
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::INT64
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::FLOAT
   { uint8_t(LJ_TNUMX),    LUA_TNUMBER, true },         // AET::DOUBLE
   { uint8_t(LJ_TLIGHTUD), LUA_TLIGHTUSERDATA, false }, // AET::PTR
   { uint8_t(LJ_TSTR),     LUA_TSTRING, false },        // AET::CSTR
   { uint8_t(LJ_TSTR),     LUA_TSTRING, false },        // AET::STR_CPP
   { uint8_t(LJ_TSTR),     LUA_TSTRING, false },        // AET::STR_GC
   { uint8_t(LJ_TTAB),     LUA_TTABLE, false },         // AET::TABLE
   { uint8_t(LJ_TARRAY),   LUA_TARRAY, false },         // AET::ARRAY
   { uint8_t(LJ_TNIL),     0, false },                  // AET::ANY
   { uint8_t(LJ_TUDATA),   LUA_TUSERDATA, false },      // AET::STRUCT
   { uint8_t(LJ_TOBJECT),  LUA_TOBJECT, false }         // AET::OBJECT
};

//********************************************************************************************************************

static OBJECTID object_uid_from_value(lua_State *L, int ArgIndex)
{
   if (lua_isobject(L, ArgIndex)) {
      TValue *tv = L->base + ArgIndex - 1;
      GCobject *obj = objectV(tv);
      return obj->uid;
   }
   if (lua_isnumber(L, ArgIndex)) return OBJECTID(lua_tointeger(L, ArgIndex));
   lj_err_argv(L, ArgIndex, ErrMsg::BADTYPE, "object or uid", luaL_typename(L, ArgIndex));
   return 0;
}

//********************************************************************************************************************
// Helper to parse element type string

static AET parse_elemtype(lua_State *L, int NArg)
{
   GCstr *type_str = lj_lib_checkstr(L, NArg);

   switch (type_str->hash) {
      case HASH_INT:     return AET::INT32;
      case HASH_BYTE:    return AET::BYTE;
      case HASH_CHAR:    return AET::BYTE;
      case HASH_INT16:   return AET::INT16;
      case HASH_INT64:   return AET::INT64;
      case HASH_FLOAT:   return AET::FLOAT;
      case HASH_DOUBLE:  return AET::DOUBLE;
      case HASH_STRING:  return AET::STR_GC;
      case HASH_STRUCT:  return AET::STRUCT;
      case HASH_POINTER: return AET::PTR;
      case HASH_OBJECT:  return AET::OBJECT;
      case HASH_TABLE:   return AET::TABLE;
      case HASH_ARRAY:   return AET::ARRAY;
      case HASH_ANY:     return AET::ANY;
   }

   lj_err_argv(L, NArg, ErrMsg::BADTYPE, "valid array type", strdata(type_str));
   return AET(0);  // unreachable
}

//********************************************************************************************************************
// Helper to get element type name

static CSTRING elemtype_name(AET Type)
{
   switch (Type) {
      case AET::BYTE:       return "char";
      case AET::INT16:      return "int16";
      case AET::INT32:      return "int";
      case AET::INT64:      return "int64";
      case AET::FLOAT:      return "float";
      case AET::DOUBLE:     return "double";
      case AET::PTR:        return "pointer";
      case AET::STRUCT:     return "struct";
      case AET::TABLE:      return "table";
      case AET::ARRAY:      return "array";
      case AET::OBJECT:     return "object";
      case AET::CSTR:
      case AET::STR_GC:
      case AET::STR_CPP:    return "string";
      case AET::ANY:        return "any";
      default: return "unknown";
   }
}

//********************************************************************************************************************

template <typename... Args>
static void append_formatted(std::string &Result, CSTRING Format, Args... Values)
{
   constexpr size_t BUFFER_SIZE = 256;

   if ((Result.capacity() - Result.size()) >= BUFFER_SIZE) {
      const auto start = Result.size();
      Result.resize(start + BUFFER_SIZE);

      const int written = snprintf(Result.data() + start, BUFFER_SIZE, Format, Values...);
      if (written < 0) {
         Result.resize(start);
         return;
      }

      const auto used = strlen(Result.data() + start);
      Result.resize(start + used);
   }
   else {
      char buffer[BUFFER_SIZE];
      const int written = snprintf(buffer, sizeof(buffer), Format, Values...);
      if (written < 0) return;

      Result += buffer;
   }
}

//********************************************************************************************************************

static char * write_integer(char *End, uint64_t Magnitude, bool Negative)
{
   char *pos = End;

   if (Magnitude IS 0) *--pos = '0';
   else {
      while (Magnitude > 0) {
         const auto digit = Magnitude % 10;
         *--pos = char('0' + digit);
         Magnitude /= 10;
      }
   }

   if (Negative) *--pos = '-';
   return pos;
}

//********************************************************************************************************************

static void append_integer(std::string &Result, int64_t Value)
{
   constexpr size_t BUFFER_SIZE = 21;

   const bool negative = Value < 0;
   const uint64_t magnitude = negative ? uint64_t(-(Value + 1)) + 1 : uint64_t(Value);

   if ((Result.capacity() - Result.size()) >= BUFFER_SIZE) {
      const auto start = Result.size();
      Result.resize(start + BUFFER_SIZE);

      char *end = Result.data() + start + BUFFER_SIZE;
      char *text = write_integer(end, magnitude, negative);
      const auto used = size_t(end - text);

      memmove(Result.data() + start, text, used);
      Result.resize(start + used);
   }
   else {
      char buffer[BUFFER_SIZE];
      char *end = buffer + BUFFER_SIZE;
      char *text = write_integer(end, magnitude, negative);

      Result.append(text, size_t(end - text));
   }
}

//********************************************************************************************************************
// Usage: array.new(size, type) or array.new('string')
//
// Creates a new array of the specified size and element type.
//
//   size: number of elements (must be non-negative)
//   type: element type string ("char", "int16", "int", "int64", "float", "double", "string", "StructName")

LJLIB_CF(array_new)      LJLIB_REC(.)
{
   GCarray *arr;

   auto type = lua_type(L, 1);
   if (type IS LUA_TSTRING) {
      TValue *o = L->base;
      GCstr *s = strV(o);
      auto elem_type = AET::BYTE;
      arr = lj_array_new(L, s->len, elem_type);

      kt::copymem(strdata(s), arr->get<CSTRING>(), s->len);
   }
   else {
      auto size = lj_lib_checkint(L, 1);
      if (size < 0) lj_err_argv(L, 1, ErrMsg::NUMRNG, "non-negative", "negative");
      auto elem_type = parse_elemtype(L, 2);

      if (elem_type IS AET::PTR) lj_err_argv(L, 2, ErrMsg::ARRTYPE); // For Kotuku functions only
      else if (elem_type IS AET::STRUCT) lj_err_argv(L, 2, ErrMsg::ARRTYPE); // For Kotuku functions only (for now)

      arr = lj_array_new(L, uint32_t(size), elem_type);
   }

   // Per-instance metatable is null - base metatable will be used automatically

   setarrayV(L, L->top++, arr);
   return 1;
}

//********************************************************************************************************************
// Usage: array.of(type, value1, value2, ...)
//
// Creates a new array populated with the given values.
//
//   type: element type string ("char", "int16", "int", "int64", "float", "double", "string")
//   value1, value2, ...: values to populate the array with
//
// Example: array.of('string', 'google.com', 'kotuku.dev', 'amazon.co.uk')
// Example: array.of('int', 1, 2, 3, 4, 5)

LJLIB_CF(array_of)
{
   auto elem_type = parse_elemtype(L, 1);

   if (elem_type IS AET::PTR) lj_err_argv(L, 1, ErrMsg::BADTYPE, "non-pointer type", "pointer");
   if (elem_type IS AET::STRUCT) lj_err_argv(L, 1, ErrMsg::BADTYPE, "non-struct type", "struct");

   // Count number of values provided (all arguments after the type string)

   int num_values = lua_gettop(L) - 1;
   if (num_values < 1) {
      luaL_error(L, ERR::Args, "array.of() requires at least one value");
      return 0;
   }

   GCarray *arr = lj_array_new(L, uint32_t(num_values), elem_type);
   setarrayV(L, L->top++, arr);

   // Populate the array with provided values

   for (int i = 0; i < num_values; i++) {
      int arg_idx = i + 2;  // Arguments start at index 2 (after type string)

      switch (elem_type) {
         case AET::STR_GC: {
            GCstr *s = lj_lib_checkstr(L, arg_idx);
            setgcref(arr->get<GCRef>()[i], obj2gco(s));
            lj_gc_objbarrier(L, arr, s);
            break;
         }
         case AET::FLOAT:  arr->get<float>()[i] = float(luaL_checknumber(L, arg_idx)); break;
         case AET::DOUBLE: arr->get<double>()[i] = luaL_checknumber(L, arg_idx); break;
         case AET::INT64:  arr->get<int64_t>()[i] = int64_t(luaL_checknumber(L, arg_idx)); break;
         case AET::INT32:  arr->get<int32_t>()[i] = int32_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::INT16:  arr->get<int16_t>()[i] = int16_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::BYTE:   arr->get<uint8_t>()[i] = uint8_t(luaL_checkinteger(L, arg_idx)); break;

         case AET::OBJECT: {
            if (not lua_isobject(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "object", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCobject *obj = objectV(tv);
            setgcref(arr->get<GCRef>()[i], obj2gco(obj));
            lj_gc_objbarrier(L, arr, obj);
            break;
         }

         case AET::TABLE: {
            if (not lua_istable(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "table", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCtab *tab = tabV(tv);
            setgcref(arr->get<GCRef>()[i], obj2gco(tab));
            lj_gc_objbarrier(L, arr, tab);
            break;
         }
         case AET::ARRAY: {
            if (not lua_isarray(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "array", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCarray *a = arrayV(tv);
            setgcref(arr->get<GCRef>()[i], obj2gco(a));
            lj_gc_objbarrier(L, arr, a);
            break;
         }

         case AET::ANY: {
            // Copy the TValue directly (any type is allowed)
            TValue *dest = &arr->get<TValue>()[i];
            TValue *src = L->base + arg_idx - 1;
            copyTV(L, dest, src);
            // Write barrier for GC values
            if (tvisgcv(src)) lj_gc_objbarrier(L, arr, gcV(src));
            break;
         }
         default: lj_err_argv(L, 1, ErrMsg::BADTYPE, "supported type", elemtype_name(elem_type)); return 0;
      }
   }

   return 1;
}

//********************************************************************************************************************
// Usage: array.table(arr)
//
// Converts an array to a Lua table.
//
//   arr: the array to convert

LJLIB_CF(array_table)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   GCtab *t = lj_array_to_table(L, arr);
   settabV(L, L->top++, t);
   return 1;
}

//********************************************************************************************************************

struct array_range_span {
   int32_t start;
   int32_t stop;
   int32_t step;
   bool empty;
};

struct array_count_span {
   MSize start;
   MSize count;
};

static int32_t array_default_remaining(MSize Length, int32_t Start)
{
   if (Start < 0 or MSize(Start) > Length) return 0;
   return int32_t(Length - MSize(Start));
}

static array_range_span array_strict_inclusive_span(lua_State *L, MSize Length, int32_t Start, int32_t Stop,
   bool Explicit)
{
   if (Length IS 0) {
      if (Explicit) lj_err_caller(L, ErrMsg::IDXRNG);
      return { 0, -1, 1, true };
   }

   if (Start < 0 or Stop < 0 or MSize(Start) >= Length or MSize(Stop) >= Length) {
      lj_err_caller(L, ErrMsg::IDXRNG);
   }

   if (Stop < Start) return { Start, Stop, 1, true };
   return { Start, Stop, 1, false };
}

static array_count_span array_strict_count_span(lua_State *L, int32_t Start, int32_t Count, MSize Length)
{
   if (Start < 0 or Count < 0 or MSize(Start) > Length) lj_err_caller(L, ErrMsg::IDXRNG);

   auto start = MSize(Start);
   auto count = MSize(Count);
   if (count > Length - start) lj_err_caller(L, ErrMsg::IDXRNG);
   return { start, count };
}

static array_count_span array_clamped_count_span(lua_State *L, int32_t Start, int32_t Count, MSize Length)
{
   if (Start < 0 or Count < 0) lj_err_caller(L, ErrMsg::IDXRNG);

   auto start = MSize(Start);
   if (start >= Length) return { Length, 0 };

   auto count = MSize(Count);
   if (count > Length - start) count = Length - start;
   return { start, count };
}

static array_range_span array_range_to_span(const tiri_range *Range, MSize Length)
{
   int32_t len = int32_t(Length);
   int32_t start = Range->start;
   int32_t stop = Range->stop;
   int32_t step = Range->step;

   bool use_inclusive = Range->inclusive;
   if (start < 0 or stop < 0) {
      use_inclusive = true;
      if (start < 0) start += len;
      if (stop < 0) stop += len;
   }

   bool forward = start <= stop;
   if (step IS 0) step = forward ? 1 : -1;
   if (forward and step < 0) step = 1;
   if (not forward and step > 0) step = -1;

   int32_t effective_stop = stop;
   if (not use_inclusive) {
      if (forward) effective_stop = stop - 1;
      else effective_stop = stop + 1;
   }

   if (forward) {
      if (start < 0) start = 0;
      if (effective_stop >= len) effective_stop = len - 1;
   }
   else {
      if (start >= len) start = len - 1;
      if (effective_stop < 0) effective_stop = 0;
   }

   bool empty = len IS 0 or (forward and start > effective_stop) or (not forward and start < effective_stop);
   return { start, effective_stop, step, empty };
}

static array_count_span array_copy_span(lua_State *L, int32_t DestIdx, MSize DestLen, int32_t SrcIdx, MSize SrcLen,
   int32_t Count, bool ClampSource)
{
   if (DestIdx < 0 or SrcIdx < 0 or Count < 0 or MSize(DestIdx) > DestLen or MSize(SrcIdx) > SrcLen) {
      lj_err_caller(L, ErrMsg::IDXRNG);
   }

   auto dest_idx = MSize(DestIdx);
   auto src_idx = MSize(SrcIdx);
   auto count = MSize(Count);

   if (count > SrcLen - src_idx) {
      if (ClampSource) count = SrcLen - src_idx;
      else lj_err_caller(L, ErrMsg::IDXRNG);
   }

   if (count > DestLen - dest_idx) lj_err_caller(L, ErrMsg::IDXRNG);
   return { dest_idx, count };
}

static bool array_find_start(int32_t Start, MSize Length, int32_t *Result)
{
   if (Start < 0) Start = 0;
   if (MSize(Start) >= Length) return false;
   *Result = Start;
   return true;
}

static int array_push_find_result(lua_State *L, int32_t Result)
{
   if (Result >= 0) setintV(L->top++, Result);
   else lua_pushnil(L);
   return 1;
}

//********************************************************************************************************************
// Usage: array.concat([Separator], [StringFormat], [Start], [End])
//
// Concatenates array elements into a string using the specified separator and optional format.
//
// Separator is placed between each concatenated element.  StringFormat optionally specifies how each element should be
// formatted (e.g., "%d", "%f", "%s").  If no format is provided, the fastest available conversion path is used.
// Start and End are optional zero-based, inclusive indexes.

LJLIB_CF(array_concat)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   const bool start_provided = not lua_isnoneornil(L, 4);
   const bool end_provided = not lua_isnoneornil(L, 5);

   size_t separator_len = 0;
   auto separator = luaL_optlstring(L, 2, "", &separator_len);
   auto format = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
   auto start = start_provided ? lj_lib_checkint(L, 4) : 0;
   auto end = end_provided ? lj_lib_checkint(L, 5) : array_default_remaining(arr->len, 0) - 1;

   auto span = array_strict_inclusive_span(L, arr->len, start, end, start_provided or end_provided);
   if (span.empty) {
      lua_pushstring(L, "");
      return 1;
   }

   if (format) {
      // Validate format string - ensure exactly one format specifier

      int format_count = 0;
      bool in_format = false;
      for (auto p = format; *p; p++) {
         if (*p IS '%') {
            if (*(p + 1) IS '%') {
               p++; // Skip escaped %
               continue;
            }

            if (in_format) {
               luaL_error(L, ERR::Syntax, "Invalid format string: multiple format specifiers not allowed");
               return 0;
            }
            in_format = true;
         }
         else if (in_format) {
            // Check for end of format specifier
            if (*p IS 'd' or *p IS 'i' or *p IS 'o' or *p IS 'x' or *p IS 'X' or
                *p IS 'u' or *p IS 'c' or *p IS 's' or *p IS 'p' or
                *p IS 'f' or *p IS 'F' or *p IS 'e' or *p IS 'E' or
                *p IS 'g' or *p IS 'G') {
               format_count++;
               in_format = false;
            }
            // Allow format modifiers and flags
            else if (!(*p IS '-' or *p IS '+' or *p IS ' ' or *p IS '#' or *p IS '0' or
                       (*p >= '1' and *p <= '9') or *p IS '.' or *p IS 'l' or *p IS 'h')) {
               luaL_error(L, ERR::Syntax, "Invalid character '%c' in format string", *p);
               return 0;
            }
         }
      }

      if (in_format) {
         luaL_error(L, ERR::Syntax, "Incomplete format specifier");
         return 0;
      }

      if (format_count != 1) {
         luaL_error(L, ERR::Syntax, "Format string must contain exactly one format specifier, found %d", format_count);
         return 0;
      }
   }

   std::string result;
   result.reserve(MSize(span.stop - span.start + 1) * 16);

   for (MSize i = MSize(span.start); i <= MSize(span.stop); i++) {
      if (i > MSize(span.start)) result.append(separator, separator_len);

      if (format) {
         switch(arr->elemtype) {
            case AET::STR_GC: {
               GCRef ref = arr->get<GCRef>()[i];
               if (gcref(ref)) {
                  GCstr *str = gco_to_string(gcref(ref));
                  if (strcmp(format, "%s") IS 0) result.append(strdata(str), str->len);
                  else append_formatted(result, format, strdata(str));
               }
               else append_formatted(result, format, "");
               break;
            }
            case AET::CSTR:
               append_formatted(result, format, arr->get<CSTRING>()[i]);
               break;
            case AET::STR_CPP: {
               const auto &str = arr->get<std::string>()[i];
               if (strcmp(format, "%s") IS 0) result.append(str.data(), str.size());
               else append_formatted(result, format, str.c_str());
               break;
            }
            case AET::FLOAT:
               append_formatted(result, format, arr->get<float>()[i]);
               break;
            case AET::DOUBLE:
               append_formatted(result, format, arr->get<double>()[i]);
               break;
            case AET::INT64:
               append_formatted(result, format, arr->get<long long>()[i]);
               break;
            case AET::INT32:
               append_formatted(result, format, arr->get<int>()[i]);
               break;
            case AET::INT16:
               append_formatted(result, format, arr->get<int16_t>()[i]);
               break;
            case AET::BYTE:
               append_formatted(result, format, arr->get<int8_t>()[i]);
               break;
            default:
               luaL_error(L, ERR::InvalidType, "concat() does not support %s types.", elemtype_name(arr->elemtype));
               return 0;
         }
      }
      else {
         switch(arr->elemtype) {
            case AET::STR_GC: {
               GCRef ref = arr->get<GCRef>()[i];
               if (gcref(ref)) {
                  GCstr *str = gco_to_string(gcref(ref));
                  result.append(strdata(str), str->len);
               }
               break;
            }
            case AET::CSTR: {
               CSTRING str = arr->get<CSTRING>()[i];
               if (str) result += str;
               break;
            }
            case AET::STR_CPP: {
               const auto &str = arr->get<std::string>()[i];
               result.append(str.data(), str.size());
               break;
            }
            case AET::FLOAT:
               append_formatted(result, "%g", double(arr->get<float>()[i]));
               break;
            case AET::DOUBLE:
               append_formatted(result, "%g", arr->get<double>()[i]);
               break;
            case AET::INT64:
               append_integer(result, arr->get<int64_t>()[i]);
               break;
            case AET::INT32:
               append_integer(result, arr->get<int32_t>()[i]);
               break;
            case AET::INT16:
               append_integer(result, arr->get<int16_t>()[i]);
               break;
            case AET::BYTE:
               append_integer(result, arr->get<uint8_t>()[i]);
               break;
            default:
               luaL_error(L, ERR::InvalidType, "concat() does not support %s types.", elemtype_name(arr->elemtype));
               return 0;
         }
      }
   }

   lua_pushlstring(L, result.data(), result.size());
   return 1;
}

//********************************************************************************************************************
// Usage: array.contains(arr, value)
//
// Not for client use; provided to support the 'in' keyword.
//
// Returns true if the value exists in the array, false otherwise.
// This is a convenience wrapper around find() that returns a boolean.
//
// Parameters:
//   arr: the array to search
//   value: the value to find
//
// Returns: true if found, false otherwise

LJLIB_CF(array_contains)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->len IS 0) {
      lua_pushboolean(L, 0);
      return 1;
   }

   if (arr->elemtype IS AET::OBJECT) {
      OBJECTID search_uid = object_uid_from_value(L, 2);
      int32_t result = find_object_in_array(arr, search_uid, 0, int32_t(arr->len - 1), 1);
      lua_pushboolean(L, result >= 0 ? 1 : 0);
      return 1;
   }

   // For string arrays, we need special handling
   if (arr->elemtype IS AET::STR_GC) {
      GCstr *search_str = lj_lib_checkstr(L, 2);
      auto refs = arr->get<GCRef>();
      for (MSize i = 0; i < arr->len; i++) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCstr *elem = gco_to_string(gcref(ref));
            if (elem->len IS search_str->len and
                memcmp(strdata(elem), strdata(search_str), elem->len) IS 0) {
               lua_pushboolean(L, 1);
               return 1;
            }
         }
      }
      lua_pushboolean(L, 0);
      return 1;
   }

   // For numeric types, use the existing find logic
   lua_Number value = lj_lib_checknum(L, 2);
   int32_t result = find_in_array(arr, value, 0, int32_t(arr->len - 1), 1);

   lua_pushboolean(L, result >= 0 ? 1 : 0);
   return 1;
}

//********************************************************************************************************************
// Usage: array.first(arr)
//
// Returns the first element of the array, or nil if empty. Provides bounds-safe access.
//
// Parameters:
//   arr: the array
//
// Returns: first element value, or nil if array is empty

LJLIB_CF(array_first)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->len IS 0) {
      lua_pushnil(L);
      return 1;
   }

   void *elem = lj_array_index(arr, 0);

   switch (arr->elemtype) {
      case AET::BYTE:   lua_pushinteger(L, *(uint8_t *)elem); break;
      case AET::INT16:  lua_pushinteger(L, *(int16_t *)elem); break;
      case AET::INT32:  lua_pushinteger(L, *(int32_t *)elem); break;
      case AET::INT64:  lua_pushnumber(L, lua_Number(*(int64_t *)elem)); break;
      case AET::FLOAT:  lua_pushnumber(L, *(float *)elem); break;
      case AET::DOUBLE: lua_pushnumber(L, *(double *)elem); break;

      case AET::STR_GC: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setstrV(L, L->top++, gco_to_string(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }

      case AET::CSTR: {
         CSTRING str = *(CSTRING *)elem;
         if (str) lua_pushstring(L, str);
         else lua_pushnil(L);
         break;
      }

      case AET::OBJECT: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setobjectV(L, L->top++, gco_to_object(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }

      case AET::TABLE: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) settabV(L, L->top++, gco_to_table(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }

      case AET::ARRAY: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setarrayV(L, L->top++, gco_to_array(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }

      case AET::ANY: {
         TValue *source = (TValue *)elem;
         copyTV(L, L->top++, source);
         return 1;
      }
      default: lua_pushnil(L); break;
   }

   return 1;
}

//********************************************************************************************************************
// Usage: array.last(arr)
//
// Returns the last element of the array, or nil if empty. Provides bounds-safe access.
//
// Parameters:
//   arr: the array
//
// Returns: last element value, or nil if array is empty

LJLIB_CF(array_last)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->len IS 0) {
      lua_pushnil(L);
      return 1;
   }

   void *elem = lj_array_index(arr, arr->len - 1);

   switch (arr->elemtype) {
      case AET::BYTE:   lua_pushinteger(L, *(uint8_t *)elem); break;
      case AET::INT16:  lua_pushinteger(L, *(int16_t *)elem); break;
      case AET::INT32:  lua_pushinteger(L, *(int32_t *)elem); break;
      case AET::INT64:  lua_pushnumber(L, lua_Number(*(int64_t *)elem)); break;
      case AET::FLOAT:  lua_pushnumber(L, *(float *)elem); break;
      case AET::DOUBLE: lua_pushnumber(L, *(double *)elem); break;
      case AET::STR_GC: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setstrV(L, L->top++, gco_to_string(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }
      case AET::CSTR: {
         CSTRING str = *(CSTRING *)elem;
         if (str) lua_pushstring(L, str);
         else lua_pushnil(L);
         break;
      }
      case AET::OBJECT: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setobjectV(L, L->top++, gco_to_object(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }
      case AET::TABLE: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) settabV(L, L->top++, gco_to_table(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }
      case AET::ARRAY: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setarrayV(L, L->top++, gco_to_array(gcref(ref)));
         else lua_pushnil(L);
         return 1;
      }
      case AET::ANY: {
         TValue *source = (TValue *)elem;
         copyTV(L, L->top++, source);
         return 1;
      }
      default: lua_pushnil(L); break;
   }

   return 1;
}

//********************************************************************************************************************
// Usage: array.clear(arr)
//
// Resets the array length to zero without deallocating storage. Capacity is preserved for reuse.
//
// Parameters:
//   arr: the array to clear (must not be read-only)
//
// Note: For string arrays with GC references, this also nullifies the references to allow garbage collection.

LJLIB_CF(array_clear)      LJLIB_REC(.)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   lj_array_clear_range(arr, 0, arr->len);
   arr->len = 0;
   return 0;
}

//********************************************************************************************************************
// Usage: new_len = array.resize(arr, new_size)
//
// Resizes an array to the specified length, growing or shrinking as needed.  When growing, new elements are
// zero-initialized (or nil for reference types).  When shrinking, excess elements are discarded (references are
// cleared for GC).
//
//   arr: the array to resize (must not be read-only or external)
//   new_size: the new length of the array (must be non-negative)
//
// Note: External arrays and cached string arrays cannot grow and will raise an error.

LJLIB_CF(array_resize)      LJLIB_REC(.)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   auto new_size = lj_lib_checkint(L, 2);
   if (new_size < 0) lj_err_argv(L, 2, ErrMsg::NUMRNG, "non-negative", "negative");

   MSize target_len = MSize(new_size);
   MSize old_len = arr->len;

   if (target_len > old_len) {
      // Growing: ensure capacity and zero-initialize new elements
      if (target_len > arr->capacity) {
         if (not lj_array_grow(L, arr, target_len)) lj_err_caller(L, ErrMsg::ARREXT);
      }

      if (arr->elemtype IS AET::STR_GC or arr->elemtype IS AET::TABLE or arr->elemtype IS AET::ARRAY or
          arr->elemtype IS AET::OBJECT or arr->elemtype IS AET::ANY) {
         lj_array_clear_range(arr, old_len, target_len - old_len);
      }
      else {
         // Numeric types: zero-fill the new region
         void *start = (char*)arr->arraydata() + (old_len * arr->elemsize);
         size_t bytes = (target_len - old_len) * arr->elemsize;
         memset(start, 0, bytes);
      }
   }
   else if (target_len < old_len) {
      lj_array_clear_range(arr, target_len, old_len - target_len);
   }

   arr->len = target_len;
   setintV(L->top++, int32_t(arr->len));
   return 1;
}

//********************************************************************************************************************
// Usage: array.push(arr, value, ...)
//
// Appends one or more elements to the end of the array, growing capacity as needed.
//
// Parameters:
//   arr: the array to append to (must not be read-only or external)
//   value, ...: one or more values to append
//
// Returns: new length of the array
//
// Note: External arrays and cached string arrays cannot grow and will raise an error.

LJLIB_CF(array_push)      LJLIB_REC(.)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   int num_values = lua_gettop(L) - 1;
   if (num_values < 1) {
      setintV(L->top++, int32_t(arr->len));
      return 1;
   }

   MSize append_count = MSize(num_values);

   if (arr->elemtype IS AET::BYTE) {
      append_count = 0;
      for (int i = 0; i < num_values; i++) {
         int arg_idx = i + 2;
         TValue *tv = L->base + arg_idx - 1;
         MSize value_count;
         if (tvisstr(tv)) value_count = strV(tv)->len;
         else {
            luaL_checkinteger(L, arg_idx);
            value_count = 1;
         }

         if (value_count > (~MSize(0) - append_count)) lj_err_caller(L, ErrMsg::ARREXT);
         append_count += value_count;
      }
   }

   // Ensure we have capacity for the new elements
   if (append_count > (~MSize(0) - arr->len)) lj_err_caller(L, ErrMsg::ARREXT);
   MSize new_len = arr->len + append_count;
   if (new_len > arr->capacity) {
      if (not lj_array_grow(L, arr, new_len)) {
         lj_err_caller(L, ErrMsg::ARREXT);
      }
   }

   if (arr->elemtype IS AET::BYTE) {
      MSize idx = arr->len;
      for (int i = 0; i < num_values; i++) {
         int arg_idx = i + 2;
         TValue *tv = L->base + arg_idx - 1;

         if (tvisstr(tv)) {
            GCstr *str = strV(tv);
            if (str->len > 0) memcpy(arr->get<uint8_t>() + idx, strdata(str), str->len);
            idx += str->len;
         }
         else {
            arr->get<uint8_t>()[idx] = uint8_t(luaL_checkinteger(L, arg_idx));
            idx++;
         }
      }

      arr->len = new_len;
      setintV(L->top++, int32_t(arr->len));
      return 1;
   }

   // Push each value
   for (int i = 0; i < num_values; i++) {
      int arg_idx = i + 2;
      MSize idx = arr->len + MSize(i);

      switch (arr->elemtype) {
         case AET::STR_GC: {
            GCstr *s = lj_lib_checkstr(L, arg_idx);
            setgcref(arr->get<GCRef>()[idx], obj2gco(s));
            lj_gc_objbarrier(L, arr, s);
            break;
         }

         case AET::OBJECT: {
            if (not lua_isobject(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "object", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCobject *obj = objectV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(obj));
            lj_gc_objbarrier(L, arr, obj);
            break;
         }

         case AET::TABLE: {
            if (not lua_istable(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "table", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCtab *tab = tabV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(tab));
            lj_gc_objbarrier(L, arr, tab);
            break;
         }

         case AET::ARRAY: {
            if (not lua_isarray(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "array", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCarray *a = arrayV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(a));
            lj_gc_objbarrier(L, arr, a);
            break;
         }

         case AET::FLOAT:  arr->get<float>()[idx] = float(luaL_checknumber(L, arg_idx)); break;
         case AET::DOUBLE: arr->get<double>()[idx] = luaL_checknumber(L, arg_idx); break;
         case AET::INT64:  arr->get<int64_t>()[idx] = int64_t(luaL_checknumber(L, arg_idx)); break;
         case AET::INT32:  arr->get<int32_t>()[idx] = int32_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::INT16:  arr->get<int16_t>()[idx] = int16_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::BYTE:   arr->get<uint8_t>()[idx] = uint8_t(luaL_checkinteger(L, arg_idx)); break;

         case AET::ANY: {
            TValue *dest = &arr->get<TValue>()[idx];
            TValue *src = L->base + arg_idx - 1;
            copyTV(L, dest, src);
            if (tvisgcv(src)) lj_gc_objbarrier(L, arr, gcV(src));
            break;
         }

         default:
            lj_err_argv(L, 1, ErrMsg::BADTYPE, "pushable type", elemtype_name(arr->elemtype));
            return 0;
      }
   }

   arr->len = new_len;
   setintV(L->top++, int32_t(arr->len));
   return 1;
}

//********************************************************************************************************************
// Usage: array.pop(arr [, n])
//
// Removes and returns the last element(s) from the array.
//
// Parameters:
//   arr: the array to pop from (must not be read-only)
//   n: number of elements to pop (default: 1)
//
// Returns: the popped value(s), or nil if the array is empty.
//          Multiple values are returned in reverse order (last element first).
//
// Note: Stops early if the array becomes exhausted (not an error).

LJLIB_CF(array_pop)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   if (arr->len IS 0) {
      lua_pushnil(L);
      return 1;
   }

   int n = lj_lib_optint(L, 2, 1);
   if (n < 1) n = 1;

   int returned = 0;
   for (int i = 0; i < n and arr->len > 0; i++) {
      MSize idx = arr->len - 1;
      void *elem = lj_array_index(arr, idx);

      // Push the value to the stack
      switch (arr->elemtype) {
         case AET::BYTE:   lua_pushinteger(L, *(uint8_t *)elem); break;
         case AET::INT16:  lua_pushinteger(L, *(int16_t *)elem); break;
         case AET::INT32:  lua_pushinteger(L, *(int32_t *)elem); break;
         case AET::INT64:  lua_pushnumber(L, lua_Number(*(int64_t *)elem)); break;
         case AET::FLOAT:  lua_pushnumber(L, *(float *)elem); break;
         case AET::DOUBLE: lua_pushnumber(L, *(double *)elem); break;

         case AET::STR_GC: {
            GCRef ref = *(GCRef *)elem;
            if (gcref(ref)) {
               setstrV(L, L->top++, gco_to_string(gcref(ref)));
               setgcrefnull(*(GCRef *)elem);  // Clear reference
            }
            else lua_pushnil(L);
            break;
         }

         case AET::CSTR: {
            CSTRING str = *(CSTRING *)elem;
            if (str) lua_pushstring(L, str);
            else lua_pushnil(L);
            break;
         }

         case AET::OBJECT: {
            GCRef ref = *(GCRef *)elem;
            if (gcref(ref)) {
               setobjectV(L, L->top++, gco_to_object(gcref(ref)));
               setgcrefnull(*(GCRef *)elem);  // Clear reference
            }
            else lua_pushnil(L);
            break;
         }

         case AET::TABLE: {
            GCRef ref = *(GCRef *)elem;
            if (gcref(ref)) {
               settabV(L, L->top++, gco_to_table(gcref(ref)));
               setgcrefnull(*(GCRef *)elem);  // Clear reference
            }
            else lua_pushnil(L);
            break;
         }

         case AET::ARRAY: {
            GCRef ref = *(GCRef *)elem;
            if (gcref(ref)) {
               setarrayV(L, L->top++, gco_to_array(gcref(ref)));
               setgcrefnull(*(GCRef *)elem);  // Clear reference
            }
            else lua_pushnil(L);
            break;
         }

         case AET::ANY: {
            TValue *source = (TValue *)elem;
            copyTV(L, L->top++, source);
            setnilV(source);  // Clear the slot
            break;
         }

         default:
            lua_pushnil(L);
            break;
      }

      arr->len--;
      returned++;
   }

   return returned;
}

//********************************************************************************************************************
// Usage: array.copy(dst, src [, dest_idx [, src_idx [, count]]])
//
// Copies elements from source array to destination array.
//
// Parameters:
//   dest:     destination array
//   src:      source array, string, or table
//   dest_idx: starting index in destination (0-based, default 0)
//   src_idx:  starting index in source (0-based, default 0)
//   count:    number of elements to copy (default: all remaining elements in source)
//
// For array sources, both arrays must have the same element type.
// The destination array must not be read-only.

LJLIB_CF(array_copy)
{
   GCarray *dest = lj_lib_checkarray(L, 1);

   if (dest->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   size_t str_len;
   auto src_type = lua_type(L, 2);
   if (src_type IS LUA_TARRAY) {
      GCarray *src  = lj_lib_checkarray(L, 2);
      auto dest_idx = lj_lib_optint(L, 3, 0);
      auto src_idx  = lj_lib_optint(L, 4, 0);
      auto count = lj_lib_optint(L, 5, array_default_remaining(src->len, src_idx));

      if (not (dest->elemtype IS src->elemtype)) lj_err_caller(L, ErrMsg::ARRTYPE);
      auto span = array_copy_span(L, dest_idx, dest->len, src_idx, src->len, count, false);
      if (span.count IS 0) return 0;

      lj_array_copy(L, dest, span.start, src, MSize(src_idx), span.count);
      return 0;
   }
   else if (src_type IS LUA_TSTRING) {
      // Treat string sequences as a byte array
      auto str = lua_tolstring(L, 2, &str_len);
      if (not str or str_len < 1) return 0; // Do nothing - no error necessary

      auto dest_idx   = lj_lib_optint(L, 3, 0);
      auto src_idx    = lj_lib_optint(L, 4, 0);
      auto copy_total = lj_lib_optint(L, 5, array_default_remaining(MSize(str_len), src_idx));
      auto span = array_copy_span(L, dest_idx, dest->len, src_idx, MSize(str_len), copy_total, true);
      if (span.count IS 0) return 0;

      // Copy string bytes to array
      auto data = dest->get<uint8_t>() + span.start;
      memcpy(data, str + src_idx, span.count);
      return 0;
   }
   else if (src_type IS LUA_TTABLE) {
      // Get table length for bounds checking
      MSize table_len = lua_objlen(L, 2);
      if (table_len < 1) {
         luaL_argerror(L, 2, "Table is empty.");
         return 0;
      }

      auto dest_idx   = lj_lib_optint(L, 3, 0);
      auto src_idx    = lj_lib_optint(L, 4, 0);
      auto copy_total = lj_lib_optint(L, 5, array_default_remaining(table_len, src_idx));
      auto span = array_copy_span(L, dest_idx, dest->len, src_idx, table_len, copy_total, true);
      if (span.count IS 0) return 0;

      // Copy table elements using ipairs-style iteration

      auto c_index = span.start;

      for (MSize i = 0; i < span.count; i++) {
         int32_t source_index = src_idx + int32_t(i);
         lua_pushinteger(L, source_index);
         lua_gettable(L, 2);        // Get table[src_idx + i]

         MSize dest_index = c_index + i;

         // Convert and store based on array type

         switch(dest->elemtype) {
            case AET::STR_CPP:
               if (lua_tostring(L, -1)) dest->get<std::string>()[dest_index].assign(lua_tostring(L, -1));
               else dest->get<std::string>()[dest_index].clear();
               break;
            case AET::STR_GC:
               if (lua_tostring(L, -1)) {
                  luaL_error(L, ERR::NoSupport, "Writing to string arrays from tables is not yet supported.");
                  lua_pop(L, 1);
                  return 0;
               }
               break;
            case AET::CSTR:
            case AET::PTR:
               luaL_error(L, ERR::NoSupport, "Writing to pointer arrays from tables is not supported.");
               lua_pop(L, 1);
               return 0;
            case AET::FLOAT:
               dest->get<float>()[dest_index] = lua_tonumber(L, -1);
               break;
            case AET::DOUBLE:
               dest->get<double>()[dest_index] = lua_tonumber(L, -1);
               break;
            case AET::INT64:
               dest->get<int64_t>()[dest_index] = lua_tointeger(L, -1);
               break;
            case AET::INT32:
               dest->get<int>()[dest_index] = lua_tointeger(L, -1);
               break;
            case AET::INT16:
               dest->get<int16_t>()[dest_index] = lua_tointeger(L, -1);
               break;
            case AET::BYTE:
               dest->get<int8_t>()[dest_index] = lua_tointeger(L, -1);
               break;

            case AET::STRUCT:
               // TODO: We should check the struct fields to confirm if its content can be safely copied.
               // This would only have to be done once per struct type, so we could cache the result.
               luaL_error(L, ERR::NoSupport, "Writing to struct arrays from tables is not yet supported.");
               lua_pop(L, 1);
               return 0;

            case AET::OBJECT:
               if (lua_isobject(L, -1)) {
                  TValue *tv = L->top - 1;
                  GCobject *obj = objectV(tv);
                  setgcref(dest->get<GCRef>()[dest_index], obj2gco(obj));
                  lj_gc_objbarrier(L, dest, obj);
               }
               else if (lua_isnil(L, -1)) {
                  setgcrefnull(dest->get<GCRef>()[dest_index]);
               }
               else {
                  luaL_error(L, ERR::InvalidType, "Expected object value at index %d.", source_index);
                  lua_pop(L, 1);
                  return 0;
               }
               break;

            case AET::TABLE:
               if (lua_istable(L, -1)) {
                  TValue *tv = L->top - 1;
                  GCtab *tab = tabV(tv);
                  setgcref(dest->get<GCRef>()[dest_index], obj2gco(tab));
                  lj_gc_objbarrier(L, dest, tab);
               }
               else if (lua_isnil(L, -1)) {
                  setgcrefnull(dest->get<GCRef>()[dest_index]);
               }
               else {
                  luaL_error(L, ERR::InvalidType, "Expected table value at index %d.", source_index);
                  lua_pop(L, 1);
                  return 0;
               }
               break;

            case AET::ARRAY:
               if (lua_isarray(L, -1)) {
                  TValue *tv = L->top - 1;
                  GCarray *a = arrayV(tv);
                  setgcref(dest->get<GCRef>()[dest_index], obj2gco(a));
                  lj_gc_objbarrier(L, dest, a);
               }
               else if (lua_isnil(L, -1)) {
                  setgcrefnull(dest->get<GCRef>()[dest_index]);
               }
               else {
                  luaL_error(L, ERR::InvalidType, "Expected array value at index %d.", source_index);
                  lua_pop(L, 1);
                  return 0;
               }
               break;

            default:
               luaL_error(L, ERR::InvalidType, "Unsupported array type $%.8x", dest->elemtype);
               lua_pop(L, 1);
               return 0;
         }

         lua_pop(L, 1); // Remove the value from stack
      }

      return 0;
   }
   else {
      luaL_argerror(L, 2, "String, array or table expected.");
      return 0;
   }
}

//********************************************************************************************************************
// Usage: array.getString(arr [, start [, len]])
//
// Extracts a string from a byte array.
//
//   start: starting index (0-based, default 0)
//   len: number of bytes to extract (default: remaining bytes from start)

LJLIB_CF(array_getString)      LJLIB_REC(.)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (not (arr->elemtype IS AET::BYTE)) lj_err_caller(L, ErrMsg::ARRSTR);

   auto start = lj_lib_optint(L, 2, 0);
   auto len = lj_lib_optint(L, 3, array_default_remaining(arr->len, start));
   auto span = array_strict_count_span(L, start, len, arr->len);

   auto data = (span.count > 0) ? arr->get<const char>() + span.start : "";
   GCstr *s = lj_str_new(L, data, span.count);
   setstrV(L, L->top++, s);
   return 1;
}

//********************************************************************************************************************
// Usage: array.setString(arr, str [, start])
//
// Copies string bytes into a byte array.
//
//   str: string to copy
//   start: starting index in array (0-based, default 0)
//
// Returns: number of bytes written

LJLIB_CF(array_setString)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   GCstr *str = lj_lib_checkstr(L, 2);

   if (not (arr->elemtype IS AET::BYTE)) lj_err_caller(L, ErrMsg::ARRSTR);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   auto start = lj_lib_optint(L, 3, 0);
   auto span = array_clamped_count_span(L, start, int32_t(str->len), arr->len);

   if (span.count > 0) {
      auto data = arr->get<char>() + span.start;
      memcpy(data, strdata(str), span.count);
   }

   setintV(L->top++, int32_t(span.count));
   return 1;
}

//********************************************************************************************************************
// Returns the element type of an array as a string.
//
// Returns: element type string ("byte", "int16", "int", etc.)

LJLIB_CF(array_type)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   auto name = elemtype_name(arr->elemtype);
   setstrV(L, L->top++, lj_str_newz(L, name));
   return 1;
}

//********************************************************************************************************************
// Usage: array.readOnly(arr)
//
// Returns whether the array is read-only.
//
// Parameters:
//   arr: the array
//
// Returns: true if read-only, false otherwise

LJLIB_CF(array_readOnly)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   setboolV(L->top++, (arr->flags & ARRAY_READONLY) != 0);
   return 1;
}

//********************************************************************************************************************
// Template-based fill for contiguous ranges (step=1). Uses std::fill for optimal performance.

template<typename T>
static void fill_contiguous(void *Data, int32_t Start, int32_t Count, lua_Number Value)
{
   T *ptr = (T *)Data + Start;
   std::fill(ptr, ptr + Count, T(Value));
}

//********************************************************************************************************************
// Template-based fill for stepped ranges. Hoists type dispatch outside the loop.

template<typename T>
static void fill_stepped(void *Data, int32_t Start, int32_t Stop, int32_t Step, T Value)
{
   T *base = (T *)Data;
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) base[i] = Value;
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) base[i] = Value;
   }
}

//********************************************************************************************************************
// Helper function to fill array elements with a value.
// Uses optimised contiguous fill when step=1, otherwise falls back to stepped fill.

static void fill_array_elements(GCarray *Arr, lua_Number Value, int32_t Start, int32_t Stop, int32_t Step)
{
   void *data = Arr->arraydata();

   // Optimised path for contiguous fills (step=1, forward direction)
   if (Step IS 1) {
      int32_t count = Stop - Start + 1;
      switch (Arr->elemtype) {
         case AET::BYTE:   fill_contiguous<uint8_t>(data, Start, count, Value); return;
         case AET::INT16:  fill_contiguous<int16_t>(data, Start, count, Value); return;
         case AET::INT32:  fill_contiguous<int32_t>(data, Start, count, Value); return;
         case AET::INT64:  fill_contiguous<int64_t>(data, Start, count, Value); return;
         case AET::FLOAT:  fill_contiguous<float>(data, Start, count, Value); return;
         case AET::DOUBLE: fill_contiguous<double>(data, Start, count, Value); return;
         default: return;
      }
   }

   // Stepped fill path (non-contiguous or reverse direction)
   switch (Arr->elemtype) {
      case AET::BYTE:   fill_stepped<uint8_t>(data, Start, Stop, Step, Value); break;
      case AET::INT16:  fill_stepped<int16_t>(data, Start, Stop, Step, Value); break;
      case AET::INT32:  fill_stepped<int32_t>(data, Start, Stop, Step, Value); break;
      case AET::INT64:  fill_stepped<int64_t>(data, Start, Stop, Step, Value); break;
      case AET::FLOAT:  fill_stepped<float>(data, Start, Stop, Step, Value); break;
      case AET::DOUBLE: fill_stepped<double>(data, Start, Stop, Step, Value); break;
      default: break;
   }
}

//********************************************************************************************************************
// Usage: array.fill(arr, value [, start [, count]]) or array.fill(arr, value, range)
//
// Fills array elements with a value.
//
// Parameters (integer form):
//   arr: the array (must not be read-only)
//   value: value to fill with (number)
//   start: starting index (0-based, default 0)
//   count: number of elements to fill (default: all remaining)
//
// Parameters (range form):
//   arr: the array (must not be read-only)
//   value: value to fill with (number)
//   range: range object specifying which elements to fill

LJLIB_CF(array_fill)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   lua_Number value = lj_lib_checknum(L, 2);

   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   // Check if third argument is a range
   tiri_range *r = check_range(L, 3);
   if (r) {
      auto span = array_range_to_span(r, arr->len);
      if (span.empty) return 0;

      fill_array_elements(arr, value, span.start, span.stop, span.step);
      return 0;
   }

   // Original integer-based fill
   auto start = lj_lib_optint(L, 3, 0);
   auto count = lj_lib_optint(L, 4, array_default_remaining(arr->len, start));
   auto span = array_clamped_count_span(L, start, count, arr->len);

   if (span.count IS 0) return 0;

   fill_array_elements(arr, value, int32_t(span.start), int32_t(span.start + span.count - 1), 1);
   return 0;
}

//********************************************************************************************************************
// Template-based find for contiguous forward search (step=1). Hoists type dispatch outside the loop.

template<typename T>
static int32_t find_forward_contiguous(const void *Data, int32_t Start, int32_t Stop, lua_Number Value)
{
   const T *base = (const T *)Data;
   T val = T(Value);
   for (int32_t i = Start; i <= Stop; i++) {
      if (base[i] IS val) return i;
   }
   return -1;
}

//********************************************************************************************************************
// Template-based find for stepped ranges. Hoists type dispatch outside the loop.

template<typename T>
static int32_t find_stepped(const void *Data, int32_t Start, int32_t Stop, int32_t Step, lua_Number Value)
{
   const T *base = (const T *)Data;
   T val = T(Value);
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         if (base[i] IS val) return i;
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         if (base[i] IS val) return i;
      }
   }
   return -1;
}

//********************************************************************************************************************
// Dispatches find operation based on array element type.
// Returns index if found, -1 if not found.

static int32_t find_in_array(GCarray *Arr, lua_Number Value, int32_t Start, int32_t Stop, int32_t Step)
{
   const void *data = Arr->arraydata();

   // Optimised path for contiguous forward search (step=1)
   if (Step IS 1) {
      switch (Arr->elemtype) {
         case AET::BYTE:   return find_forward_contiguous<uint8_t>(data, Start, Stop, Value);
         case AET::INT16:  return find_forward_contiguous<int16_t>(data, Start, Stop, Value);
         case AET::INT32:  return find_forward_contiguous<int32_t>(data, Start, Stop, Value);
         case AET::INT64:  return find_forward_contiguous<int64_t>(data, Start, Stop, Value);
         case AET::FLOAT:  return find_forward_contiguous<float>(data, Start, Stop, Value);
         case AET::DOUBLE: return find_forward_contiguous<double>(data, Start, Stop, Value);
         default: return -1;
      }
   }

   // Stepped search path (non-contiguous or reverse direction)
   switch (Arr->elemtype) {
      case AET::BYTE:   return find_stepped<uint8_t>(data, Start, Stop, Step, Value);
      case AET::INT16:  return find_stepped<int16_t>(data, Start, Stop, Step, Value);
      case AET::INT32:  return find_stepped<int32_t>(data, Start, Stop, Step, Value);
      case AET::INT64:  return find_stepped<int64_t>(data, Start, Stop, Step, Value);
      case AET::FLOAT:  return find_stepped<float>(data, Start, Stop, Step, Value);
      case AET::DOUBLE: return find_stepped<double>(data, Start, Stop, Step, Value);
      default: return -1;
   }
}

//********************************************************************************************************************
// Object search by UID for stepped ranges.

static int32_t find_object_in_array(GCarray *Arr, OBJECTID SearchUid, int32_t Start, int32_t Stop, int32_t Step)
{
   auto refs = Arr->get<GCRef>();
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCobject *obj = gco_to_object(gcref(ref));
            if (obj and obj->uid IS SearchUid) return i;
         }
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCobject *obj = gco_to_object(gcref(ref));
            if (obj and obj->uid IS SearchUid) return i;
         }
      }
   }
   return -1;
}

//********************************************************************************************************************
// String search by value for stepped ranges.
// Returns index if found, -1 if not found.

static int32_t find_string_in_array(GCarray *Arr, GCstr *SearchStr, int32_t Start, int32_t Stop, int32_t Step)
{
   auto refs = Arr->get<GCRef>();
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCstr *elem = gco_to_string(gcref(ref));
            if (elem->len IS SearchStr->len and
                memcmp(strdata(elem), strdata(SearchStr), elem->len) IS 0) {
               return i;
            }
         }
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCstr *elem = gco_to_string(gcref(ref));
            if (elem->len IS SearchStr->len and
                memcmp(strdata(elem), strdata(SearchStr), elem->len) IS 0) {
               return i;
            }
         }
      }
   }
   return -1;
}

//********************************************************************************************************************
// Usage: array.find(arr, value [, start]) or array.find(arr, value, {range})
//
// Searches for a value in the array.
//
// Parameters (integer form):
//   arr: the array to search
//   value: the value to find
//   start: starting index (0-based, default 0)
//
// Parameters (range form):
//   arr: the array to search
//   value: the value to find
//   range: range object specifying which elements to search
//
// Returns: index of first occurrence, or nil if not found

LJLIB_CF(array_find)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->elemtype IS AET::OBJECT) {
      auto search_uid = object_uid_from_value(L, 2);
      if (tiri_range *r = check_range(L, 3)) {
         auto span = array_range_to_span(r, arr->len);
         if (span.empty) return array_push_find_result(L, -1);
         return array_push_find_result(L, find_object_in_array(arr, search_uid, span.start, span.stop, span.step));
      }

      auto start = lj_lib_optint(L, 3, 0);
      if (not array_find_start(start, arr->len, &start)) return array_push_find_result(L, -1);
      return array_push_find_result(L, find_object_in_array(arr, search_uid, start, int32_t(arr->len - 1), 1));
   }
   else if (arr->elemtype IS AET::STR_GC) { // Handle string arrays (STR_GC)
      GCstr *search_str = lj_lib_checkstr(L, 2);

      if (tiri_range *r = check_range(L, 3)) {
         auto span = array_range_to_span(r, arr->len);
         if (span.empty) return array_push_find_result(L, -1);
         return array_push_find_result(L, find_string_in_array(arr, search_str, span.start, span.stop, span.step));
      }

      auto start = lj_lib_optint(L, 3, 0);
      if (not array_find_start(start, arr->len, &start)) return array_push_find_result(L, -1);
      return array_push_find_result(L, find_string_in_array(arr, search_str, start, int32_t(arr->len - 1), 1));
   }

   int ok;
   lua_Number value = lua_tonumberx(L, 2, &ok);
   if (not ok) luaL_error(L, "Unsupported value type '%s'", lua_typename(L, lua_type(L, 2)));

   if (tiri_range *r = check_range(L, 3)) {
      auto span = array_range_to_span(r, arr->len);
      if (span.empty) return array_push_find_result(L, -1);
      return array_push_find_result(L, find_in_array(arr, value, span.start, span.stop, span.step));
   }

   // Original integer-based find
   auto start = lj_lib_optint(L, 3, 0);
   if (not array_find_start(start, arr->len, &start)) return array_push_find_result(L, -1);
   return array_push_find_result(L, find_in_array(arr, value, start, int32_t(arr->len - 1), 1));
}

//********************************************************************************************************************
// Usage: array.reverse(arr)
//
// Reverses the array elements in place.
//
// Parameters:
//   arr: the array to reverse (must not be read-only)

LJLIB_CF(array_reverse)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);
   if (arr->len < 2) return 0;

   void *data = arr->arraydata();

   // Use std::reverse with typed pointers for optimal performance

   switch (arr->elemtype) {
      case AET::BYTE:   { auto *p = (uint8_t *)data; std::reverse(p, p + arr->len); break; }
      case AET::INT16:  { auto *p = (int16_t *)data; std::reverse(p, p + arr->len); break; }
      case AET::INT32:  { auto *p = (int32_t *)data; std::reverse(p, p + arr->len); break; }
      case AET::INT64:  { auto *p = (int64_t *)data; std::reverse(p, p + arr->len); break; }
      case AET::FLOAT:  { auto *p = (float *)data; std::reverse(p, p + arr->len); break; }
      case AET::DOUBLE: { auto *p = (double *)data; std::reverse(p, p + arr->len); break; }
      case AET::PTR:    { auto *p = (void **)data; std::reverse(p, p + arr->len); break; }
      case AET::STR_GC:
      case AET::TABLE:
      case AET::ARRAY:
      case AET::OBJECT: { auto *p = (GCRef *)data; std::reverse(p, p + arr->len); break; }
      case AET::ANY: { auto *p = (TValue *)data; std::reverse(p, p + arr->len); break; }

      default: {
         // Fallback for struct types using byte-level swap
         auto *base = (uint8_t *)data;
         auto elemsize = arr->elemsize;
         uint8_t temp[64];  // Large enough for typical struct sizes
         for (MSize i = 0; i < arr->len / 2; i++) {
            MSize j = arr->len - 1 - i;
            void *elem_i = base + i * elemsize;
            void *elem_j = base + j * elemsize;
            memcpy(temp, elem_i, elemsize);
            memcpy(elem_i, elem_j, elemsize);
            memcpy(elem_j, temp, elemsize);
         }
         break;
      }
   }

   return 0;
}

//********************************************************************************************************************
// Usage: array.slice(arr, range)
//
// Creates a new array containing elements specified by the range.
//
// Parameters:
//   arr: the source array
//   range: a range object specifying start, stop, step, and inclusivity
//
// Returns: new array containing the slice
//
// Delegates to range.slice() for the actual implementation.

LJLIB_CF(array_slice)
{
   lj_lib_checkarray(L, 1);  // Validate first arg is an array
   return lj_range_slice(L);
}

//********************************************************************************************************************
// Usage: array.sort(arr [, descending])
//
// Sorts the numeric array in place using quicksort.
//
// Parameters:
//   arr: the array to sort (must not be read-only, must be numeric type)
//   descending: if true, sort in descending order (default: false/ascending)
//
// Note: Does not support pointer or struct arrays.

//********************************************************************************************************************
// Type priority for sorting _ANY arrays: nil=0, false=1, true=2, number=3, string=4, other=5

static int tvalue_type_priority(cTValue *v)
{
   if (tvisnil(v)) return 0;
   if (tvisfalse(v)) return 1;
   if (tvistrue(v)) return 2;
   if (tvisnumber(v)) return 3;
   if (tvisstr(v)) return 4;
   return 5;  // tables, functions, etc.
}

//********************************************************************************************************************
// Compare two TValues for sorting. Returns <0 if a<b, 0 if a==b, >0 if a>b.

static int tvalue_compare(cTValue *a, cTValue *b, bool descending)
{
   int type_a = tvalue_type_priority(a);
   int type_b = tvalue_type_priority(b);

   if (type_a != type_b) {
      return descending ? (type_b - type_a) : (type_a - type_b);
   }

   // Same type - compare values
   int result = 0;
   if (tvisnumber(a)) {
      lua_Number na = numberVnum(a);
      lua_Number nb = numberVnum(b);
      result = (na < nb) ? -1 : (na > nb) ? 1 : 0;
   }
   else if (tvisstr(a)) {
      GCstr *sa = strV(a);
      GCstr *sb = strV(b);
      result = strcmp(strdata(sa), strdata(sb));
   }
   else if (tvisgcv(a)) {
      // For GC objects (tables, etc.), compare by address
      result = (gcrefu(a->gcr) < gcrefu(b->gcr)) ? -1 :
               (gcrefu(a->gcr) > gcrefu(b->gcr)) ? 1 : 0;
   }
   // nil, true, false compare equal within their type

   return descending ? -result : result;
}

//********************************************************************************************************************
// Call the user comparator function at stack position FnIdx with two array elements.
// Pushes the function, the element at index A, and the element at index B, then calls and returns the boolean result.

static bool array_sort_comp(lua_State *L, GCarray *Arr, int32_t A, int32_t B, int FnIdx)
{
   lua_pushvalue(L, FnIdx);
   array_push_element(L, Arr, A);
   array_push_element(L, Arr, B);
   lua_call(L, 2, 1);
   bool result = lua_toboolean(L, -1);
   lua_pop(L, 1);
   return result;
}

//********************************************************************************************************************
// Quicksort using a user-provided comparator function. Operates on raw array data with byte-level swaps, calling back
// into Tiri for each comparison.

static void quicksort_func(lua_State *L, GCarray *Arr, int32_t Left, int32_t Right, int FnIdx)
{
   luaL_checkstack(L, 5, "array sort");

   auto elem_swap = [&](int32_t I, int32_t J) {
      if (I IS J) return;
      void *a = lj_array_index(Arr, I);
      void *b = lj_array_index(Arr, J);
      MSize sz = Arr->elemsize;
      uint8_t tmp[64];
      std::memcpy(tmp, a, sz);
      std::memcpy(a, b, sz);
      std::memcpy(b, tmp, sz);
   };

   while (Left < Right) {
      // Partition using median-of-three pivot selection
      int32_t mid = (Left + Right) / 2;
      if (array_sort_comp(L, Arr, mid, Left, FnIdx)) elem_swap(Left, mid);
      if (array_sort_comp(L, Arr, Right, Left, FnIdx)) elem_swap(Left, Right);
      if (array_sort_comp(L, Arr, Right, mid, FnIdx)) elem_swap(mid, Right);

      if (Right - Left < 2) break;

      elem_swap(mid, Right - 1);  // Place pivot at Right-1
      int32_t pivot_idx = Right - 1;

      int32_t i = Left;
      int32_t j = Right - 1;
      while (true) {
         while (array_sort_comp(L, Arr, ++i, pivot_idx, FnIdx)) {
            if (i >= Right) lj_err_caller(L, ErrMsg::TABSORT);
         }
         while (array_sort_comp(L, Arr, pivot_idx, --j, FnIdx)) {
            if (j <= Left) lj_err_caller(L, ErrMsg::TABSORT);
         }
         if (i >= j) break;
         elem_swap(i, j);
      }
      elem_swap(i, Right - 1);  // Restore pivot

      // Recurse on the smaller partition, loop on the larger
      if (i - Left < Right - i) {
         quicksort_func(L, Arr, Left, i - 1, FnIdx);
         Left = i + 1;
      }
      else {
         quicksort_func(L, Arr, i + 1, Right, FnIdx);
         Right = i - 1;
      }
   }
}

//********************************************************************************************************************

template<typename T>
static void quicksort(T *Data, int32_t Left, int32_t Right, bool Descending)
{
   if (Left >= Right) return;

   // Partition
   T pivot = Data[(Left + Right) / 2];
   int32_t i = Left, j = Right;

   while (i <= j) {
      if (Descending) {
         while (Data[i] > pivot) i++;
         while (Data[j] < pivot) j--;
      }
      else {
         while (Data[i] < pivot) i++;
         while (Data[j] > pivot) j--;
      }

      if (i <= j) {
         T temp = Data[i];
         Data[i] = Data[j];
         Data[j] = temp;
         i++;
         j--;
      }
   }

   if (Left < j) quicksort(Data, Left, j, Descending);
   if (i < Right) quicksort(Data, i, Right, Descending);
}

LJLIB_CF(array_sort)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);
   if (arr->len < 2) return 0;

   // If the second argument is a function, use it as a custom comparator.
   // Reject element types that cannot be pushed to the stack for comparison.
   if (lua_isfunction(L, 2)) {
      if (arr->elemtype IS AET::STRUCT or arr->elemtype IS AET::PTR or arr->elemtype IS AET::STR_CPP) {
         luaL_error(L, ERR::WrongType, "sort() with comparator does not support this array type.");
         return 0;
      }
      quicksort_func(L, arr, 0, int32_t(arr->len - 1), 2);
      return 0;
   }

   bool descending = lua_toboolean(L, 2);

   switch (arr->elemtype) {
      case AET::BYTE: quicksort(arr->get<uint8_t>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::INT16: quicksort(arr->get<int16_t>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::INT32: quicksort(arr->get<int32_t>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::INT64: quicksort(arr->get<int64_t>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::FLOAT: quicksort(arr->get<float>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::DOUBLE: quicksort(arr->get<double>(), 0, int32_t(arr->len - 1), descending); break;
      case AET::STR_GC: {
         GCRef *refs = arr->get<GCRef>();
         std::sort(refs, refs + arr->len, [descending](const GCRef &a, const GCRef &b) {
            GCstr *sa = gco_to_string(gcref(a));
            GCstr *sb = gco_to_string(gcref(b));
            // Handle nil/null references
            if (not sa and not sb) return false;
            if (not sa) return not descending;  // nil sorts before non-nil
            if (not sb) return descending;
            // Compare by content using strncmp for lexicographic ordering
            MSize min_len = (sa->len < sb->len) ? sa->len : sb->len;
            int cmp = memcmp(strdata(sa), strdata(sb), min_len);
            if (cmp IS 0) cmp = (sa->len < sb->len) ? -1 : (sa->len > sb->len) ? 1 : 0;
            return descending ? (cmp > 0) : (cmp < 0);
         });
         break;
      }
      case AET::ANY: {
         TValue *data = arr->get<TValue>();
         // Use std::sort with custom comparator for type-grouped sorting
         std::sort(data, data + arr->len, [descending](const TValue &a, const TValue &b) {
            return tvalue_compare(&a, &b, descending) < 0;
         });
         break;
      }
      default: luaL_error(L, ERR::WrongType, "sort() does not support this array type."); return 0;
   }

   return 0;
}

//********************************************************************************************************************
// Push array element value to Lua stack based on element type.
// Used by the array iterator to return element values.

static void array_push_element(lua_State *L, GCarray *Arr, MSize Idx)
{
   void *elem = lj_array_index(Arr, Idx);

   switch (Arr->elemtype) {
      case AET::BYTE:   lua_pushinteger(L, *(uint8_t *)elem); break;
      case AET::INT16:  lua_pushinteger(L, *(int16_t *)elem); break;
      case AET::INT32:  lua_pushinteger(L, *(int32_t *)elem); break;
      case AET::INT64:  lua_pushnumber(L, lua_Number(*(int64_t *)elem)); break;
      case AET::FLOAT:  lua_pushnumber(L, *(float *)elem); break;
      case AET::DOUBLE: lua_pushnumber(L, *(double *)elem); break;
      case AET::STR_GC: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setstrV(L, L->top++, gco_to_string(gcref(ref)));
         else lua_pushnil(L);
         break;
      }
      case AET::CSTR: {
         CSTRING str = *(CSTRING *)elem;
         if (str) lua_pushstring(L, str);
         else lua_pushnil(L);
         break;
      }
      case AET::OBJECT: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setobjectV(L, L->top++, gco_to_object(gcref(ref)));
         else lua_pushnil(L);
         break;
      }
      case AET::TABLE: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) settabV(L, L->top++, gco_to_table(gcref(ref)));
         else lua_pushnil(L);
         break;
      }
      case AET::ARRAY: {
         GCRef ref = *(GCRef *)elem;
         if (gcref(ref)) setarrayV(L, L->top++, gco_to_array(gcref(ref)));
         else lua_pushnil(L);
         break;
      }
      case AET::ANY: {
         TValue *source = (TValue *)elem;
         copyTV(L, L->top++, source);
         break;
      }
      default: lua_pushnil(L); break;
   }
}

//********************************************************************************************************************
// Iterator function for array iteration.
// Called repeatedly by the for loop until it returns nil.
//
// Usage: for i, v in arr do ... end

static int array_iterator_next(lua_State *L)
{
   GCarray *arr = lua_toarray(L, lua_upvalueindex(1));
   if (not arr) return 0;

   int32_t idx;
   if (lua_isnil(L, 2)) {
      idx = 0;  // First iteration
   }
   else {
      idx = int32_t(lua_tointeger(L, 2)) + 1;  // Next index
   }

   if (idx < 0 or MSize(idx) >= arr->len) return 0;  // Iteration complete

   lua_pushinteger(L, idx);         // Return index (control var)
   array_push_element(L, arr, idx); // Return value
   return 2;
}

//********************************************************************************************************************
// __call metamethod for array iteration.  Enables: for i, v in array_variable do
//
// This function serves dual purposes:
//
// 1. When called directly (arr()), returns (iterator, nil, nil) for manual iteration setup
// 2. When called by BC_ITERC with (state, control_var), acts as the iterator itself
//
// Detection: If arg2 is nil or integer (control var pattern), act as iterator.
//            Otherwise, return the iterator triple for manual setup.

static int array_call(lua_State *L)
{
   GCarray *arr = lua_toarray(L, 1);
   if (not arr) return 0;

   // Check if this is being called as an iterator (by BC_ITERC via lj_meta_call)
   // After lj_meta_call rewrites the stack: __call(array, state, control_var)
   // So: arg1 = array, arg2 = state (nil), arg3 = control_var (nil or integer)

   auto nargs = lua_gettop(L);
   if (nargs >= 3) {
      auto type = lua_type(L, 3);
      if ((type IS LUA_TNIL) or (type IS LUA_TNUMBER)) {
         // Acting as iterator
         int32_t idx;
         if (type IS LUA_TNIL) idx = 0;  // First iteration
         else idx = int32_t(lua_tointeger(L, 3)) + 1;  // Next index

         if (idx < 0 or MSize(idx) >= arr->len) return 0;  // Iteration complete

         lua_pushinteger(L, idx);         // Return index (control var + first loop var)
         array_push_element(L, arr, idx); // Return value (second loop var)
         return 2;
      }
   }

   // Manual setup: arr() returns (iterator, nil, nil)
   // Use array_iterator_next with array as upvalue for cleaner separation

   lua_pushvalue(L, 1);  // Push array as upvalue
   lua_pushcclosure(L, array_iterator_next, 1);
   lua_pushnil(L);       // State (not used)
   lua_pushnil(L);       // Initial control variable
   return 3;
}

//********************************************************************************************************************
// Usage: array.each(arr, callback)
//
// Iterates over array elements, calling the callback for each element.
// The callback receives (value, index) as arguments.
//
// Parameters:
//   arr: the array to iterate
//   callback: function(value, index) to call for each element
//
// Returns: the array (for chaining)

LJLIB_CF(array_each)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);

   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 2);            // Push the callback function
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 2, 0);              // Call callback(value, index)
   }

   // Return the array for chaining
   lua_pushvalue(L, 1);
   return 1;
}

//********************************************************************************************************************
// Usage: array.map(arr, transform)
//
// Returns a new array with each element transformed by the function.
// The transform function receives (value, index) and returns the new value.
//
// Parameters:
//   arr: the source array
//   transform: function(value, index) returning transformed value
//
// Returns: new array of the same type with transformed elements

LJLIB_CF(array_map)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);

   // Create new array of same type and size
   GCarray *result = lj_array_new(L, arr->len, arr->elemtype);
   setarrayV(L, L->top++, result);
   int result_idx = lua_gettop(L);

   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 2);            // Push the transform function
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 2, 1);              // Call transform(value, index) -> result

      // Store the result in the new array
      switch (result->elemtype) {
         case AET::STR_GC: {
            if (lua_isstring(L, -1)) {
               GCstr *s = lj_str_new(L, lua_tostring(L, -1), lua_strlen(L, -1));
               setgcref(result->get<GCRef>()[i], obj2gco(s));
               lj_gc_objbarrier(L, result, s);
            }
            else {
               setgcrefnull(result->get<GCRef>()[i]);
            }
            break;
         }

         case AET::OBJECT: {
            if (lua_isobject(L, -1)) {
               TValue *tv = L->top - 1;
               GCobject *obj = objectV(tv);
               setgcref(result->get<GCRef>()[i], obj2gco(obj));
               lj_gc_objbarrier(L, result, obj);
            }
            else setgcrefnull(result->get<GCRef>()[i]);
            break;
         }

         case AET::TABLE: {
            if (lua_istable(L, -1)) {
               TValue *tv = L->top - 1;
               GCtab *tab = tabV(tv);
               setgcref(result->get<GCRef>()[i], obj2gco(tab));
               lj_gc_objbarrier(L, result, tab);
            }
            else setgcrefnull(result->get<GCRef>()[i]);
            break;
         }

         case AET::ARRAY: {
            if (lua_isarray(L, -1)) {
               TValue *tv = L->top - 1;
               GCarray *a = arrayV(tv);
               setgcref(result->get<GCRef>()[i], obj2gco(a));
               lj_gc_objbarrier(L, result, a);
            }
            else setgcrefnull(result->get<GCRef>()[i]);
            break;
         }

         case AET::ANY: {
            TValue *dest = &result->get<TValue>()[i];
            TValue *src = L->top - 1;
            copyTV(L, dest, src);
            if (tvisgcv(src)) lj_gc_objbarrier(L, result, gcV(src));
            break;
         }

         case AET::FLOAT:  result->get<float>()[i] = float(lua_tonumber(L, -1)); break;
         case AET::DOUBLE: result->get<double>()[i] = lua_tonumber(L, -1); break;
         case AET::INT64:  result->get<int64_t>()[i] = int64_t(lua_tonumber(L, -1)); break;
         case AET::INT32:  result->get<int32_t>()[i] = int32_t(lua_tointeger(L, -1)); break;
         case AET::INT16:  result->get<int16_t>()[i] = int16_t(lua_tointeger(L, -1)); break;
         case AET::BYTE:   result->get<uint8_t>()[i] = uint8_t(lua_tointeger(L, -1)); break;
         default:
            break;
      }

      lua_pop(L, 1);  // Pop the result value
   }

   // Push the result array (already on stack at result_idx)
   lua_pushvalue(L, result_idx);
   return 1;
}

//********************************************************************************************************************
// Usage: array.filter(arr, predicate)
//
// Returns a new array containing only elements that satisfy the predicate.
// The predicate function receives (value, index) and returns true/false.
//
// Parameters:
//   arr: the source array
//   predicate: function(value, index) returning boolean
//
// Returns: new array of the same type with filtered elements

LJLIB_CF(array_filter)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);

   // Create result array with zero length (will grow dynamically)
   GCarray *result = lj_array_new(L, 0, arr->elemtype);
   setarrayV(L, L->top++, result);

   // Single pass: filter and copy matching elements
   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 2);            // Push the predicate function
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 2, 1);              // Call predicate(value, index) -> boolean

      if (lua_toboolean(L, -1)) {
         // Grow array to accommodate new element
         MSize new_len = result->len + 1;
         if (not lj_array_grow(L, result, new_len)) {
            lj_err_caller(L, ErrMsg::ARREXT);
         }

         result->len = new_len;
         lj_array_copy(L, result, new_len - 1, arr, i, 1);
      }

      lua_pop(L, 1);
   }

   return 1;
}

//********************************************************************************************************************
// Usage: array.reduce(arr, initial, reducer)
//
// Folds all elements into a single accumulated value.
// The reducer function receives (accumulator, value, index) and returns the new accumulator.
//
// Parameters:
//   arr: the source array
//   initial: the initial accumulator value
//   reducer: function(accumulator, value, index) returning new accumulator
//
// Returns: the final accumulated value

LJLIB_CF(array_reduce)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   // Arg 2: initial value (any type)
   luaL_checktype(L, 3, LUA_TFUNCTION);

   // Start with the initial value on the stack
   lua_pushvalue(L, 2);  // Push initial value as current accumulator

   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 3);            // Push the reducer function
      lua_pushvalue(L, -2);           // Push current accumulator
      lua_remove(L, -3);              // Remove old accumulator from stack
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 3, 1);              // Call reducer(acc, value, index) -> new_acc
      // New accumulator is now on top of stack
   }

   // Return the final accumulated value (already on stack)
   return 1;
}

//********************************************************************************************************************
// Usage: array.any(arr, predicate)
//
// Returns true if any element satisfies the predicate. Short-circuits on first match.
// The predicate function receives (value, index) and returns true/false.
//
// Parameters:
//   arr: the source array
//   predicate: function(value, index) returning boolean
//
// Returns: true if any element satisfies predicate, false otherwise

LJLIB_CF(array_any)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);

   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 2);            // Push the predicate function
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 2, 1);              // Call predicate(value, index) -> boolean

      if (lua_toboolean(L, -1)) {
         lua_pushboolean(L, 1);
         return 1;
      }
      lua_pop(L, 1);
   }

   lua_pushboolean(L, 0);
   return 1;
}

//********************************************************************************************************************
// Usage: array.all(arr, predicate)
//
// Returns true if all elements satisfy the predicate. Short-circuits on first failure.
// The predicate function receives (value, index) and returns true/false.
//
// Parameters:
//   arr: the source array
//   predicate: function(value, index) returning boolean
//
// Returns: true if all elements satisfy predicate, false otherwise

LJLIB_CF(array_all)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);

   for (MSize i = 0; i < arr->len; i++) {
      lua_pushvalue(L, 2);            // Push the predicate function
      array_push_element(L, arr, i);  // Push value
      lua_pushinteger(L, i);          // Push index
      lua_call(L, 2, 1);              // Call predicate(value, index) -> boolean

      if (not lua_toboolean(L, -1)) {
         lua_pushboolean(L, 0);
         return 1;
      }
      lua_pop(L, 1);
   }

   lua_pushboolean(L, 1);
   return 1;
}

//********************************************************************************************************************
// Usage: array.insert(arr, index, value, ...)
//
// Inserts one or more values at the specified index, shifting subsequent elements.
//
// Parameters:
//   arr: the array to modify (must not be read-only)
//   index: the position to insert at (0-based)
//   value, ...: one or more values to insert
//
// Returns: new length of the array
//
// Note: If index equals the array length, values are appended (equivalent to push).
//       If index is beyond array length, an error is raised.

LJLIB_CF(array_insert)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   int32_t index = lj_lib_checkint(L, 2);
   if (index < 0 or MSize(index) > arr->len) {
      lj_err_callerv(L, ErrMsg::ARROB, index, int(arr->len));
   }

   int num_values = lua_gettop(L) - 2;
   if (num_values < 1) {
      setintV(L->top++, int32_t(arr->len));
      return 1;
   }

   // Ensure we have capacity for the new elements
   MSize new_len = arr->len + MSize(num_values);
   if (new_len > arr->capacity) {
      if (not lj_array_grow(L, arr, new_len)) {
         lj_err_caller(L, ErrMsg::ARREXT);
      }
   }

   // Shift existing elements to make room
   MSize shift_count = arr->len - MSize(index);
   if (shift_count > 0) {
      void *src = lj_array_index(arr, index);
      void *dst = lj_array_index(arr, index + num_values);
      if (arr->elemtype IS AET::ANY) lj_bulk_move_tvalue((TValue *)dst, (const TValue *)src, shift_count);
      else memmove(dst, src, shift_count * arr->elemsize);
   }

   // Insert the new values
   for (int i = 0; i < num_values; i++) {
      int arg_idx = i + 3;
      MSize idx = MSize(index) + MSize(i);

      switch (arr->elemtype) {
         case AET::STR_GC: {
            GCstr *s = lj_lib_checkstr(L, arg_idx);
            setgcref(arr->get<GCRef>()[idx], obj2gco(s));
            lj_gc_objbarrier(L, arr, s);
            break;
         }

         case AET::OBJECT: {
            if (not lua_isobject(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "object", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCobject *obj = objectV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(obj));
            lj_gc_objbarrier(L, arr, obj);
            break;
         }

         case AET::TABLE: {
            if (not lua_istable(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "table", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCtab *tab = tabV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(tab));
            lj_gc_objbarrier(L, arr, tab);
            break;
         }

         case AET::ARRAY: {
            if (not lua_isarray(L, arg_idx)) {
               lj_err_argv(L, arg_idx, ErrMsg::BADTYPE, "array", luaL_typename(L, arg_idx));
            }
            TValue *tv = L->base + arg_idx - 1;
            GCarray *a = arrayV(tv);
            setgcref(arr->get<GCRef>()[idx], obj2gco(a));
            lj_gc_objbarrier(L, arr, a);
            break;
         }

         case AET::FLOAT:  arr->get<float>()[idx] = float(luaL_checknumber(L, arg_idx)); break;
         case AET::DOUBLE: arr->get<double>()[idx] = luaL_checknumber(L, arg_idx); break;
         case AET::INT64:  arr->get<int64_t>()[idx] = int64_t(luaL_checknumber(L, arg_idx)); break;
         case AET::INT32:  arr->get<int32_t>()[idx] = int32_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::INT16:  arr->get<int16_t>()[idx] = int16_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::BYTE:   arr->get<uint8_t>()[idx] = uint8_t(luaL_checkinteger(L, arg_idx)); break;
         case AET::ANY: {
            TValue *dest = &arr->get<TValue>()[idx];
            TValue *src = L->base + arg_idx - 1;
            copyTV(L, dest, src);
            if (tvisgcv(src)) lj_gc_objbarrier(L, arr, gcV(src));
            break;
         }
         default:
            lj_err_argv(L, 1, ErrMsg::BADTYPE, "insertable type", elemtype_name(arr->elemtype));
            return 0;
      }
   }

   arr->len = new_len;
   setintV(L->top++, int32_t(arr->len));
   return 1;
}

//********************************************************************************************************************
// Usage: array.remove(arr, index [, count])
//
// Removes one or more elements at the specified index, shifting subsequent elements.
//
// Parameters:
//   arr: the array to modify (must not be read-only)
//   index: the position to remove from (0-based)
//   count: number of elements to remove (default: 1)
//
// Returns: the new length of the array
//
// Note: Count is automatically limited to available elements from index to end.
//       A count of 0 does nothing.

LJLIB_CF(array_remove)
{
   GCarray *arr = lj_lib_checkarray(L, 1);
   if (arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   int32_t index = lj_lib_checkint(L, 2);
   if (index < 0 or MSize(index) >= arr->len) {
      lj_err_callerv(L, ErrMsg::ARROB, index, int(arr->len));
   }

   int32_t count = lj_lib_optint(L, 3, 1);
   if (count < 0) {
      luaL_error(L, ERR::Args, "count must be non-negative");
      return 0;
   }
   if (count IS 0) {
      setintV(L->top++, int32_t(arr->len));
      return 1;
   }

   // Limit count to available elements
   MSize available = arr->len - MSize(index);
   if (MSize(count) > available) count = int32_t(available);

   // Shift remaining elements down
   MSize shift_start = MSize(index) + MSize(count);
   MSize shift_count = arr->len - shift_start;
   if (shift_count > 0) {
      void *src = lj_array_index(arr, shift_start);
      void *dst = lj_array_index(arr, index);
      if (arr->elemtype IS AET::ANY) lj_bulk_move_tvalue((TValue *)dst, (const TValue *)src, shift_count);
      else memmove(dst, src, shift_count * arr->elemsize);
   }

   lj_array_clear_range(arr, arr->len - MSize(count), MSize(count));

   arr->len -= MSize(count);
   setintV(L->top++, int32_t(arr->len));
   return 1;
}

//********************************************************************************************************************
// Usage: array.clone(arr)
//
// Creates a deep copy of the array.
//
// Parameters:
//   arr: the source array
//
// Returns: new array with copied elements
//
// Note: For GC-tracked types (strings, tables), references are copied (not deep-cloned).
//       The new array has capacity equal to the source array's length.

LJLIB_CF(array_clone)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->elemtype IS AET::STRUCT) {
      luaL_error(L, ERR::NoSupport, "array.clone() does not support struct types.");
      return 0;
   }

   // Create new array with same type and length
   GCarray *result = lj_array_new(L, arr->len, arr->elemtype);
   setarrayV(L, L->top++, result);

   if (arr->len IS 0) return 1;

   lj_array_copy(L, result, 0, arr, 0, arr->len);
   return 1;
}

//********************************************************************************************************************
// Created exclusively for implementing array<byte> concatenation operations

struct ArrayAppendPiece {
   enum class Kind : uint8_t {
      String,
      Scratch,
      ByteArray
   };

   Kind kind = Kind::String;
   const char *data = nullptr;
   GCarray *array = nullptr;
   MSize offset = 0;
   MSize len = 0;
};

static void array_append_add_len(lua_State *L, MSize &Total, MSize Len)
{
   if (Len > (~MSize(0) - Total)) lj_err_caller(L, ErrMsg::ARREXT);
   Total += Len;
}

static constexpr int max_array_append_stack_pieces = 16;

static void array_append_store_piece(ArrayAppendPiece *Pieces, int &PieceCount, const ArrayAppendPiece &Piece)
{
   lj_assertX(PieceCount < max_array_append_stack_pieces, "array append stack piece overflow");
   Pieces[PieceCount] = Piece;
   PieceCount++;
}

static void array_append_push_number(lua_State *L, SBuf *Scratch, cTValue *Value, ArrayAppendPiece *Pieces,
   int &PieceCount, MSize &Total)
{
   MSize offset = sbuflen(Scratch);
   if (tvisint(Value)) lj_strfmt_putint(Scratch, intV(Value));
   else lj_strfmt_putnum(Scratch, Value);

   MSize len = sbuflen(Scratch) - offset;
   array_append_store_piece(Pieces, PieceCount, { ArrayAppendPiece::Kind::Scratch, nullptr, nullptr, offset, len });
   array_append_add_len(L, Total, len);
}

static void array_append_push_piece(lua_State *L, cTValue *Value, ArrayAppendPiece *Pieces, int &PieceCount,
   SBuf *Scratch, MSize &Total)
{
   if (tvisstr(Value)) {
      GCstr *str = strV(Value);
      array_append_store_piece(Pieces, PieceCount, { ArrayAppendPiece::Kind::String, strdata(str), nullptr, 0,
         str->len });
      array_append_add_len(L, Total, str->len);
      return;
   }

   if (tvisnumber(Value)) {
      array_append_push_number(L, Scratch, Value, Pieces, PieceCount, Total);
      return;
   }

   if (tvisarray(Value)) {
      GCarray *source = arrayV(Value);
      if (source->elemtype IS AET::BYTE) {
         array_append_store_piece(Pieces, PieceCount, { ArrayAppendPiece::Kind::ByteArray, nullptr, source, 0,
            source->len });
         array_append_add_len(L, Total, source->len);
         return;
      }
   }

   lj_err_optype(L, Value, ErrMsg::OPCAT);
}

static void array_append_byte_pieces(lua_State *L, GCarray *Arr, int PieceCount)
{
   if (Arr->flags & ARRAY_READONLY) lj_err_caller(L, ErrMsg::ARRRO);

   if (PieceCount > max_array_append_stack_pieces) {
      L->top = L->base + 1 + PieceCount;
      lua_concat(L, PieceCount);
      PieceCount = 1;
   }

   SBuf *scratch = lj_buf_tmp_(L);
   ArrayAppendPiece pieces[max_array_append_stack_pieces];
   int piece_count = 0;

   MSize append_len = 0;
   for (int i = 0; i < PieceCount; i++) {
      array_append_push_piece(L, L->base + i + 1, pieces, piece_count, scratch, append_len);
   }

   if (append_len > (~MSize(0) - Arr->len)) lj_err_caller(L, ErrMsg::ARREXT);
   MSize old_len = Arr->len;
   MSize new_len = old_len + append_len;
   if (new_len > Arr->capacity and not lj_array_grow(L, Arr, new_len)) lj_err_caller(L, ErrMsg::ARREXT);

   uint8_t *dest_base = Arr->get<uint8_t>();
   MSize write_pos = old_len;
   for (int i = 0; i < piece_count; i++) {
      const ArrayAppendPiece &piece = pieces[i];
      if (piece.len IS 0) continue;

      const char *source = nullptr;
      if (piece.kind IS ArrayAppendPiece::Kind::String) {
         source = piece.data;
         memcpy(dest_base + write_pos, source, piece.len);
      }
      else if (piece.kind IS ArrayAppendPiece::Kind::Scratch) {
         source = scratch->b + piece.offset;
         memcpy(dest_base + write_pos, source, piece.len);
      }
      else {
         source = piece.array->get<const char>();
         memmove(dest_base + write_pos, source, piece.len);
      }
      write_pos += piece.len;
   }

   Arr->len = new_len;
}

static void array_append_reduce_rhs(lua_State *L, int PieceCount)
{
   L->top = L->base + 1 + PieceCount;
   if (PieceCount > 1) lua_concat(L, PieceCount);
}

LJLIB_CF(array_append)      LJLIB_REC(.)
{
   TValue *left = lj_lib_checkany(L, 1);
   int piece_count = lua_gettop(L) - 1;
   if (piece_count < 1) lj_err_argv(L, 2, ErrMsg::NOVAL);

   if (tvisarray(left)) {
      GCarray *arr = arrayV(left);
      if (arr->elemtype IS AET::BYTE) {
         array_append_byte_pieces(L, arr, piece_count);
         setarrayV(L, L->base, arr);
         L->top = L->base + 1;
         return 1;
      }

      array_append_reduce_rhs(L, piece_count);
      lj_cf_array_push(L);
      setarrayV(L, L->base, arr);
      L->top = L->base + 1;
      return 1;
   }

   array_append_reduce_rhs(L, piece_count);
   lua_concat(L, 2);
   return 1;
}

//********************************************************************************************************************
// __tostring metamethod.

static int array_tostring(lua_State *L)
{
   GCarray *arr = lj_lib_checkarray(L, 1);

   if (arr->elemtype IS AET::BYTE) {
      GCstr *s = lj_str_new(L, arr->get<const char>(), arr->len);
      setstrV(L, L->top++, s);
      return 1;
   }

   CSTRING type_name = elemtype_name(arr->elemtype);
   lua_pushfstring(L, "array<%s, %d>", type_name, int(arr->len));
   return 1;
}

//********************************************************************************************************************
// __concat metamethod.  Produces a new string value from the combination of the LHS and RHS

static GCstr * array_concat_value(lua_State *L, cTValue *Value)
{
   if (tvisstr(Value)) return strV(Value);
   if (tvisnumber(Value)) return lj_strfmt_number(L, Value);

   if (tvisarray(Value)) {
      GCarray *arr = arrayV(Value);
      if (arr->elemtype IS AET::BYTE) return lj_str_new(L, arr->get<const char>(), arr->len);
   }

   return nullptr;
}

static int array_concat_meta(lua_State *L)
{
   cTValue *left = L->base;
   cTValue *right = L->base + 1;

   GCstr *left_str = array_concat_value(L, left);
   if (not left_str) lj_err_optype(L, left, ErrMsg::OPCAT);
   setstrV(L, L->top++, left_str);

   GCstr *right_str = array_concat_value(L, right);
   if (not right_str) lj_err_optype(L, right, ErrMsg::OPCAT);
   setstrV(L, L->top++, right_str);

   GCstr *result = lj_buf_cat2str(L, left_str, right_str);
   L->top -= 2;
   setstrV(L, L->top++, result);
   return 1;
}

static bool array_string_equal(GCarray *Left, GCarray *Right)
{
   GCRef *left_refs = Left->get<GCRef>();
   GCRef *right_refs = Right->get<GCRef>();

   for (MSize i = 0; i < Left->len; i++) {
      GCobj *left_obj = gcref(left_refs[i]);
      GCobj *right_obj = gcref(right_refs[i]);

      if (left_obj IS right_obj) continue;
      if (not left_obj or not right_obj) return false;

      GCstr *left_str = gco_to_string(left_obj);
      GCstr *right_str = gco_to_string(right_obj);

      if (not (left_str->hash IS right_str->hash)) return false;
      if (not (left_str->len IS right_str->len)) return false;
      if (not (memcmp(strdata(left_str), strdata(right_str), left_str->len) IS 0)) return false;
   }

   return true;
}

static bool array_object_equal(GCarray *Left, GCarray *Right)
{
   GCRef *left_refs = Left->get<GCRef>();
   GCRef *right_refs = Right->get<GCRef>();

   for (MSize i = 0; i < Left->len; i++) {
      GCobj *left_gc = gcref(left_refs[i]);
      GCobj *right_gc = gcref(right_refs[i]);

      if (left_gc IS right_gc) continue;
      if (not left_gc or not right_gc) return false;

      GCobject *left_obj = gco_to_object(left_gc);
      GCobject *right_obj = gco_to_object(right_gc);

      if (not (left_obj->uid IS right_obj->uid)) return false;
   }

   return true;
}

//********************************************************************************************************************
// __eq metamethod.

static int array_eq_meta(lua_State *L)
{
   GCarray *left = lj_lib_checkarray(L, 1);
   GCarray *right = lj_lib_checkarray(L, 2);

   if ((not (left->elemtype IS right->elemtype)) or (not (left->len IS right->len))) {
      lua_pushboolean(L, 0);
      return 1;
   }

   if (glArrayConversion[size_t(left->elemtype)].primitive) {
      size_t byte_count = size_t(left->len) * left->elemsize;
      bool equal = (byte_count IS 0) or (memcmp(left->arraydata(), right->arraydata(), byte_count) IS 0);
      lua_pushboolean(L, equal);
   }
   else if (left->elemtype IS AET::STR_GC) {
      lua_pushboolean(L, array_string_equal(left, right));
   }
   else if (left->elemtype IS AET::OBJECT) {
      lua_pushboolean(L, array_object_equal(left, right));
   }
   else lj_err_caller(L, ErrMsg::ARRTYPE);

   return 1;
}

//********************************************************************************************************************
// Registers the array library and sets up the base metatable for arrays.
// Unlike the Lua table, arrays are created via conventional means, i.e. array.new().
//
// The array library table itself serves as the base metatable, allowing direct method
// lookup (arr:concat(), arr:sort(), etc.) via lj_tab_get in the VM array helpers.

#include "lj_libdef.h"
#include "lj_proto_registry.h"

extern "C" int luaopen_array(lua_State *L)
{
   LJ_LIB_REG(L, "array", array);
   // Stack: [..., array_lib_table]

   // Compound concatenation lowers through this hidden key so rebinding the public array global cannot shadow it.
   lua_getfield(L, -1, "append");
   lua_pushlstring(L, glArrayAppendHelperKey.data(), glArrayAppendHelperKey.size());
   lua_pushvalue(L, -2);
   lua_rawset(L, LUA_GLOBALSINDEX);
   lua_pop(L, 1);

   // Use the library table directly as the base metatable for arrays.
   // This allows lj_arr_get to find methods like concat, sort, etc. via direct table lookup.
   GCtab *lib = tabV(L->top - 1);
   global_State *g = G(L);

   // Add __call metamethod to the library table for iteration support.  Enables: for ... in ... do
   lua_pushcfunction(L, array_call);
   lua_setfield(L, -2, "__call");

   // Byte arrays are commonly used as string buffers, so tostring() exposes their contents directly.
   lua_pushcfunction(L, array_tostring);
   lua_setfield(L, -2, "__tostring");

   lua_pushcfunction(L, array_concat_meta);
   lua_setfield(L, -2, "__concat");

   lua_pushcfunction(L, array_eq_meta);
   lua_setfield(L, -2, "__eq");

   // NOBARRIER: basemt is a GC root.
   setgcref(basemt_it(g, LJ_TARRAY), obj2gco(lib));

   // Register array interface prototypes for compile-time type inference
   reg_iface_prototype("array", "new", { TiriType::Array }, { TiriType::Num, TiriType::Str });
   reg_iface_prototype("array", "of", { TiriType::Array }, { TiriType::Str }, FProtoFlags::Variadic);
   // Methods
   reg_iface_prototype("array", "table", { TiriType::Table }, { TiriType::Array });
   reg_iface_prototype("array", "concat", { TiriType::Str }, { TiriType::Array, TiriType::Str, TiriType::Str, TiriType::Num, TiriType::Num });
   reg_iface_prototype("array", "contains", { TiriType::Bool }, { TiriType::Array, TiriType::Any });
   reg_iface_prototype("array", "first", { TiriType::Any }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "last", { TiriType::Any }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "clear", {}, { TiriType::Array });
   reg_iface_prototype("array", "resize", {}, { TiriType::Array, TiriType::Num });
   reg_iface_prototype("array", "push", {}, { TiriType::Array, TiriType::Any });
   // Private
   // reg_iface_prototype("array", "append", { TiriType::Any }, { TiriType::Any, TiriType::Any });
   reg_iface_prototype("array", "pop", { TiriType::Any }, { TiriType::Array });
   reg_iface_prototype("array", "copy", {}, { TiriType::Array, TiriType::Num, TiriType::Num, TiriType::Num });
   reg_iface_prototype("array", "getString", { TiriType::Str }, { TiriType::Array, TiriType::Num, TiriType::Num });
   reg_iface_prototype("array", "setString", {}, { TiriType::Array, TiriType::Str, TiriType::Num });
   reg_iface_prototype("array", "type", { TiriType::Str }, { TiriType::Array });
   reg_iface_prototype("array", "readOnly", { TiriType::Bool }, { TiriType::Array });
   reg_iface_prototype("array", "fill", {}, { TiriType::Array, TiriType::Any, TiriType::Num, TiriType::Num });
   reg_iface_prototype("array", "find", { TiriType::Num }, { TiriType::Array, TiriType::Any, TiriType::Num, TiriType::Num });
   reg_iface_prototype("array", "reverse", { TiriType::Array }, { TiriType::Array });
   reg_iface_prototype("array", "slice", { TiriType::Array }, { TiriType::Array, TiriType::Any });
   reg_iface_prototype("array", "sort", { TiriType::Array }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "each", {}, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "map", { TiriType::Array }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "filter", { TiriType::Array }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "reduce", { TiriType::Any }, { TiriType::Array, TiriType::Func, TiriType::Any });
   reg_iface_prototype("array", "any", { TiriType::Bool }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "all", { TiriType::Bool }, { TiriType::Array, TiriType::Func });
   reg_iface_prototype("array", "insert", {}, { TiriType::Array, TiriType::Num, TiriType::Any });
   reg_iface_prototype("array", "remove", { TiriType::Any }, { TiriType::Array, TiriType::Num });
   reg_iface_prototype("array", "clone", { TiriType::Array }, { TiriType::Array });

   return 1;
}
