// Range library for Tiri.
// Copyright © 2025-2026 Paul Manias.
//
// Implements a Range type as userdata with support for:
// - Exclusive (default) and inclusive ranges
// - Forward and reverse iteration
// - Custom step values
// - Membership testing via contains()
// - Conversion to table via toTable()

#define lib_range_c
#define LUA_LIB

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_array.h"
#include "lib.h"
#include "lib_range.h"
#include "runtime/lj_proto_registry.h"
#include <kotuku/strings.hpp>

#define LJLIB_MODULE_range

//********************************************************************************************************************
// Helper to get range userdata from stack with type checking

static tiri_range * get_range(lua_State *L, int idx)
{
   return (tiri_range *)luaL_checkudata(L, idx, RANGE_METATABLE);
}

//********************************************************************************************************************
// Check if a stack value at the given index is a range userdata (returns nullptr if not).
// This function is exported via lib_range.h for use by lib_table.cpp.

tiri_range * check_range(lua_State *L, int idx)
{
   if (void *ud = lua_touserdata(L, idx)) {
      if (lua_getmetatable(L, idx)) {
         lua_getfield(L, LUA_REGISTRYINDEX, RANGE_METATABLE);
         if (lua_rawequal(L, -1, -2)) {
            lua_pop(L, 2);
            return (tiri_range *)ud;
         }
         lua_pop(L, 2);
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Check if a TValue is a range userdata (for use in metamethod implementations).
// This avoids stack manipulation and is more efficient for internal use.

tiri_range * check_range_tv(lua_State *L, cTValue *tv)
{
   if (not tvisudata(tv)) return nullptr;

   GCudata *ud = udataV(tv);
   GCtab *mt = tabref(ud->metatable);
   if (not mt) return nullptr;

   // Get the expected metatable for ranges from the registry
   lua_getfield(L, LUA_REGISTRYINDEX, RANGE_METATABLE);
   if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      return nullptr;
   }

   GCtab *range_mt = tabV(L->top - 1);
   lua_pop(L, 1);

   if (mt IS range_mt) {
      return (tiri_range *)uddata(ud);
   }
   return nullptr;
}

//********************************************************************************************************************
// Shared floating-point range model.

static constexpr size_t FP_RANGE_MAX_ARRAY_COUNT =
   size_t(std::numeric_limits<uint32_t>::max()) / sizeof(TValue);

static bool fp_range_is_int32(lua_Number Value)
{
   if (not std::isfinite(Value) or std::trunc(Value) != Value) return false;
   return Value >= lua_Number(std::numeric_limits<int32_t>::min()) and
      Value <= lua_Number(std::numeric_limits<int32_t>::max());
}

static lua_Number fp_range_value(const tiri_range *Range, size_t Ordinal)
{
   return std::fma(lua_Number(Ordinal), Range->step, Range->start);
}

static bool fp_range_in_bounds(const tiri_range *Range, lua_Number Value)
{
   if (Range->step > 0) return Range->inclusive ? Value <= Range->stop : Value < Range->stop;
   return Range->inclusive ? Value >= Range->stop : Value > Range->stop;
}

static bool fp_range_aligned_ordinal(const tiri_range *Range, size_t *Ordinal)
{
   long double distance = ((long double)Range->stop - (long double)Range->start) / (long double)Range->step;
   if (distance < 0) return false;

   long double nearest = std::round(distance);
   long double tolerance = std::min(0.25L, 8.0L * (long double)std::numeric_limits<lua_Number>::epsilon() *
      std::max(1.0L, std::abs(distance)));
   if (std::abs(distance - nearest) > tolerance or nearest < 0 or
       nearest > (long double)std::numeric_limits<lua_Integer>::max()) {
      return false;
   }

   *Ordinal = size_t(nearest);
   return true;
}

static bool fp_range_generated_in_bounds(const tiri_range *Range, lua_Number Value)
{
   bool in_bounds = fp_range_in_bounds(Range, Value);
   if (in_bounds or not Range->inclusive) return in_bounds;

   size_t ordinal;
   return fp_range_aligned_ordinal(Range, &ordinal) and Value IS fp_range_value(Range, ordinal);
}

static size_t fp_range_count(lua_State *L, const tiri_range *Range, bool ForAllocation)
{
   if (not fp_range_in_bounds(Range, Range->start)) return 0;

   long double distance = ((long double)Range->stop - (long double)Range->start) / (long double)Range->step;
   if (distance < 0) return 0;

   long double estimate = Range->inclusive ? std::floor(distance) + 1.0L : std::ceil(distance);
   long double maximum = (long double)std::numeric_limits<lua_Integer>::max();
   if (not std::isfinite(estimate) or estimate < 0 or estimate > maximum) {
      lj_err_caller(L, ErrMsg::NUMRNG);
   }

   size_t count = size_t(estimate);
   while (count > 0 and not fp_range_generated_in_bounds(Range, fp_range_value(Range, count - 1))) count--;
   while (fp_range_generated_in_bounds(Range, fp_range_value(Range, count))) {
      if (count > 0 and fp_range_value(Range, count) IS fp_range_value(Range, count - 1)) {
         lj_err_caller(L, ErrMsg::NUMRNG);
      }
      if (count >= size_t(std::numeric_limits<lua_Integer>::max())) lj_err_caller(L, ErrMsg::NUMRNG);
      count++;
   }

   if (ForAllocation and count > FP_RANGE_MAX_ARRAY_COUNT) {
      lj_err_caller(L, ErrMsg::NUMRNG);
   }
   return count;
}

static void fp_range_push(lua_State *L, lua_Number Value)
{
   if (fp_range_is_int32(Value)) lua_pushinteger(L, lua_Integer(Value));
   else lua_pushnumber(L, Value);
}

static void fp_range_set(TValue *Target, lua_Number Value)
{
   if (fp_range_is_int32(Value)) setintV(Target, int32_t(Value));
   else setnumV(Target, Value);
}

static bool fp_range_integer_array(const tiri_range *Range)
{
   return fp_range_is_int32(Range->start) and fp_range_is_int32(Range->stop) and fp_range_is_int32(Range->step);
}

static GCarray * fp_range_materialise(lua_State *L, const tiri_range *Range, size_t Count)
{
   bool integer_array = fp_range_integer_array(Range);
   GCarray *array = lj_array_new(L, uint32_t(Count), integer_array ? AET::INT32 : AET::DOUBLE);
   if (integer_array) {
      auto data = array->get<int32_t>();
      for (size_t ordinal = 0; ordinal < Count; ordinal++) {
         data[ordinal] = int32_t(fp_range_value(Range, ordinal));
      }
   }
   else {
      auto data = array->get<double>();
      for (size_t ordinal = 0; ordinal < Count; ordinal++) data[ordinal] = fp_range_value(Range, ordinal);
   }
   return array;
}

static lua_Number fp_range_check_number(lua_State *L, int Argument)
{
   if (lua_type(L, Argument) != LUA_TNUMBER) lj_err_argt(L, Argument, LUA_TNUMBER);
   lua_Number value = lua_tonumber(L, Argument);
   if (not std::isfinite(value)) lj_err_arg(L, Argument, ErrMsg::NUMRNG);
   return value;
}

static int fp_range_construct(lua_State *L)
{
   if (lua_gettop(L) < 2) lj_err_caller(L, ErrMsg::NUMRNG);

   lua_Number start = fp_range_check_number(L, 1);
   lua_Number stop = fp_range_check_number(L, 2);
   bool inclusive = lua_gettop(L) >= 3 and not lua_isnil(L, 3) ? lua_toboolean(L, 3) : false;
   lua_Number step;
   if (lua_gettop(L) >= 4 and not lua_isnil(L, 4)) {
      step = fp_range_check_number(L, 4);
      if (step IS 0.0) lj_err_arg(L, 4, ErrMsg::NUMRNG);
   }
   else step = start <= stop ? 1.0 : -1.0;

   auto range = (tiri_range *)lua_newuserdata(L, sizeof(tiri_range));
   range->start = start;
   range->stop = stop;
   range->step = step;
   range->inclusive = inclusive;
   luaL_getmetatable(L, RANGE_METATABLE);
   lua_setmetatable(L, -2);
   return 1;
}

void range_check_index(lua_State *L, const tiri_range *Range, tiri_index_range *Result)
{
   if (not Range or not Result or not fp_range_is_int32(Range->start) or not fp_range_is_int32(Range->stop) or
       not fp_range_is_int32(Range->step)) {
      lj_err_caller(L, ErrMsg::IDXRNG);
   }
   Result->start = int32_t(Range->start);
   Result->stop = int32_t(Range->stop);
   Result->step = int32_t(Range->step);
   Result->inclusive = Range->inclusive;
}

static int fp_range_each(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, false);
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_pushvalue(L, 2);
      fp_range_push(L, fp_range_value(range, ordinal));
      lua_call(L, 1, 1);
      bool terminate = not lua_isnil(L, -1) and not lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (terminate) break;
   }
   lua_pushvalue(L, 1);
   return 1;
}

static int fp_range_filter(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, true);
   GCarray *array = lj_array_new(L, uint32_t(count), AET::ANY);
   TValue *data = array->get<TValue>();
   uint32_t result_index = 0;
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_Number value = fp_range_value(range, ordinal);
      lua_pushvalue(L, 2);
      fp_range_push(L, value);
      lua_call(L, 1, 1);
      if (lua_toboolean(L, -1)) fp_range_set(&data[result_index++], value);
      lua_pop(L, 1);
   }
   array->len = result_index;
   setarrayV(L, L->top++, array);
   return 1;
}

static int fp_range_reduce(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 3, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, false);
   lua_pushvalue(L, 2);
   int accumulator = lua_gettop(L);
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_pushvalue(L, 3);
      lua_pushvalue(L, accumulator);
      fp_range_push(L, fp_range_value(range, ordinal));
      lua_call(L, 2, 1);
      lua_replace(L, accumulator);
   }
   return 1;
}

static int fp_range_map(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, true);
   GCarray *array = lj_array_new(L, uint32_t(count), AET::ANY);
   TValue *data = array->get<TValue>();
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_pushvalue(L, 2);
      fp_range_push(L, fp_range_value(range, ordinal));
      lua_call(L, 1, 1);
      TValue *source = L->top - 1;
      copyTV(L, &data[ordinal], source);
      if (tvisgcv(source)) lj_gc_objbarrier(L, array, gcV(source));
      lua_pop(L, 1);
   }
   setarrayV(L, L->top++, array);
   return 1;
}

static int fp_range_take(lua_State *L)
{
   auto range = get_range(L, 1);
   lua_Integer requested = luaL_checkinteger(L, 2);
   if (requested < 0) requested = 0;
   size_t count = std::min(fp_range_count(L, range, false), size_t(requested));
   if (count > FP_RANGE_MAX_ARRAY_COUNT) lj_err_caller(L, ErrMsg::NUMRNG);
   GCarray *array = fp_range_materialise(L, range, count);
   setarrayV(L, L->top++, array);
   return 1;
}

static int fp_range_any(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, false);
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_pushvalue(L, 2);
      fp_range_push(L, fp_range_value(range, ordinal));
      lua_call(L, 1, 1);
      bool matched = lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (matched) {
         lua_pushboolean(L, 1);
         return 1;
      }
   }
   lua_pushboolean(L, 0);
   return 1;
}

static int fp_range_all(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, false);
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_pushvalue(L, 2);
      fp_range_push(L, fp_range_value(range, ordinal));
      lua_call(L, 1, 1);
      bool matched = lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (not matched) {
         lua_pushboolean(L, 0);
         return 1;
      }
   }
   lua_pushboolean(L, 1);
   return 1;
}

static int fp_range_find(lua_State *L)
{
   auto range = get_range(L, 1);
   luaL_checktype(L, 2, LUA_TFUNCTION);
   size_t count = fp_range_count(L, range, false);
   for (size_t ordinal = 0; ordinal < count; ordinal++) {
      lua_Number value = fp_range_value(range, ordinal);
      lua_pushvalue(L, 2);
      fp_range_push(L, value);
      lua_call(L, 1, 1);
      bool matched = lua_toboolean(L, -1);
      lua_pop(L, 1);
      if (matched) {
         fp_range_push(L, value);
         return 1;
      }
   }
   lua_pushnil(L);
   return 1;
}

static std::string fp_range_number_string(lua_State *L, lua_Number Value)
{
   fp_range_push(L, Value);
   size_t length = 0;
   const char *text = lua_tolstring(L, -1, &length);
   std::string result(text, length);
   lua_pop(L, 1);
   return result;
}

static int fp_range_tostring(lua_State *L)
{
   auto range = get_range(L, 1);
   lua_Number inferred_step = range->start <= range->stop ? 1.0 : -1.0;
   std::string result = "{" + fp_range_number_string(L, range->start);
   result += range->inclusive ? " into " : " to ";
   result += fp_range_number_string(L, range->stop);
   if (range->step != inferred_step) result += " by " + fp_range_number_string(L, range->step);
   result += "}";
   lua_pushlstring(L, result.data(), result.size());
   return 1;
}

static int fp_range_eq(lua_State *L)
{
   auto left = check_range(L, 1);
   auto right = check_range(L, 2);
   bool equal = left and right and left->start IS right->start and left->stop IS right->stop and
      left->step IS right->step and left->inclusive IS right->inclusive;
   lua_pushboolean(L, equal);
   return 1;
}

static int fp_range_len(lua_State *L)
{
   lua_pushinteger(L, lua_Integer(fp_range_count(L, get_range(L, 1), false)));
   return 1;
}

static int fp_range_contains(lua_State *L)
{
   auto range = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   int argument = lua_isuserdata(L, 1) ? 2 : 1;
   if (not range or lua_type(L, argument) != LUA_TNUMBER) {
      lua_pushboolean(L, 0);
      return 1;
   }
   lua_Number value = lua_tonumber(L, argument);
   if (not std::isfinite(value) or not fp_range_in_bounds(range, value)) {
      lua_pushboolean(L, 0);
      return 1;
   }
   lua_Number ordinal_value = (value - range->start) / range->step;
   if (not std::isfinite(ordinal_value) or ordinal_value < 0) {
      lua_pushboolean(L, 0);
      return 1;
   }
   lua_Number nearest = std::round(ordinal_value);
   if (nearest > lua_Number(std::numeric_limits<lua_Integer>::max())) {
      lua_pushboolean(L, 0);
      return 1;
   }
   lua_Number reconstructed = fp_range_value(range, size_t(nearest));
   if (not fp_range_generated_in_bounds(range, reconstructed)) {
      lua_pushboolean(L, 0);
      return 1;
   }
   lua_Number scale = std::max({ 1.0, std::abs(value), std::abs(reconstructed), std::abs(range->start),
      std::abs(range->step * nearest) });
   lua_Number tolerance = 8.0 * std::numeric_limits<lua_Number>::epsilon() * scale;
   lua_pushboolean(L, std::abs(value - reconstructed) <= tolerance);
   return 1;
}

static int fp_range_toarray(lua_State *L)
{
   auto range = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   if (not range) lj_err_caller(L, ErrMsg::BADVAL);
   size_t count = fp_range_count(L, range, true);
   GCarray *array = fp_range_materialise(L, range, count);
   setarrayV(L, L->top++, array);
   return 1;
}

static int fp_range_iterator_next(lua_State *L)
{
   auto range = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   if (not range) return 0;
   size_t ordinal = size_t(lua_tointeger(L, lua_upvalueindex(2)));
   size_t count = fp_range_count(L, range, false);
   if (ordinal >= count) return 0;
   lua_pushinteger(L, lua_Integer(ordinal + 1));
   lua_replace(L, lua_upvalueindex(2));
   fp_range_push(L, fp_range_value(range, ordinal));
   return 1;
}

static int fp_range_call(lua_State *L)
{
   get_range(L, 1);
   if (lua_gettop(L) >= 2) {
      int argument_type = lua_type(L, 2);
      if (argument_type IS LUA_TNIL or argument_type IS LUA_TNUMBER) {
         luaL_error(L, ERR::Syntax, "range used incorrectly in for loop; use 'for i in range()' not 'for i in range'");
      }
   }
   lua_pushvalue(L, 1);
   lua_pushinteger(L, 0);
   lua_pushcclosure(L, fp_range_iterator_next, 2);
   lua_pushnil(L);
   lua_pushnil(L);
   return 3;
}

//********************************************************************************************************************
// range.new(start, stop [, inclusive [, step]])
// Creates a new range object

LJLIB_CF(range_new)
{
   return fp_range_construct(L);
}

//********************************************************************************************************************
// __index metamethod
// Handles property access (.start, .stop, .step, .inclusive, .length)
// and method calls (:contains, :toArray)

constexpr auto HASH_start     = kt::strhash("start");
constexpr auto HASH_stop      = kt::strhash("stop");
constexpr auto HASH_step      = kt::strhash("step");
constexpr auto HASH_inclusive = kt::strhash("inclusive");
constexpr auto HASH_length    = kt::strhash("length");
constexpr auto HASH_contains  = kt::strhash("contains");
constexpr auto HASH_toArray   = kt::strhash("toArray");
constexpr auto HASH_each      = kt::strhash("each");
constexpr auto HASH_filter    = kt::strhash("filter");
constexpr auto HASH_reduce    = kt::strhash("reduce");
constexpr auto HASH_map       = kt::strhash("map");
constexpr auto HASH_take      = kt::strhash("take");
constexpr auto HASH_any       = kt::strhash("any");
constexpr auto HASH_all       = kt::strhash("all");
constexpr auto HASH_find      = kt::strhash("find");

static int range_index(lua_State *Lua)
{
   auto r = get_range(Lua, 1);

   if (lua_type(Lua, 2) IS LUA_TSTRING) {
      auto hash = kt::strhash(lua_tostring(Lua, 2));

      switch(hash) {
         case HASH_start:     fp_range_push(Lua, r->start); return 1;
         case HASH_stop:      fp_range_push(Lua, r->stop); return 1;
         case HASH_step:      fp_range_push(Lua, r->step); return 1;
         case HASH_inclusive: lua_pushboolean(Lua, r->inclusive); return 1;
         case HASH_length:    lua_pushinteger(Lua, lua_Integer(fp_range_count(Lua, r, false))); return 1;
         case HASH_each:      lua_pushcfunction(Lua, fp_range_each); return 1;
         case HASH_filter:    lua_pushcfunction(Lua, fp_range_filter); return 1;
         case HASH_reduce:    lua_pushcfunction(Lua, fp_range_reduce); return 1;
         case HASH_map:       lua_pushcfunction(Lua, fp_range_map); return 1;
         case HASH_take:      lua_pushcfunction(Lua, fp_range_take); return 1;
         case HASH_any:       lua_pushcfunction(Lua, fp_range_any); return 1;
         case HASH_all:       lua_pushcfunction(Lua, fp_range_all); return 1;
         case HASH_find:      lua_pushcfunction(Lua, fp_range_find); return 1;
         case HASH_contains: // Methods - return closures with range as upvalue
            lua_pushvalue(Lua, 1);  // Push the range userdata
            lua_pushcclosure(Lua, fp_range_contains, 1);
            return 1;
         case HASH_toArray:
            lua_pushvalue(Lua, 1);  // Push the range userdata
            lua_pushcclosure(Lua, fp_range_toarray, 1);
            return 1;
      }
   }

   lua_pushnil(Lua);
   return 1;
}

//********************************************************************************************************************
// __call metamethod for the library table
// Allows range(start, stop, ...) syntax instead of range.new(start, stop, ...)

static int range_lib_call(lua_State *L)
{
   lua_remove(L, 1);
   return fp_range_construct(L);
}

//********************************************************************************************************************
// range.slice(obj, range) - Generic slicing for tables and strings.
// For tables: returns a new table with elements from the range
// For strings: returns a substring based on the range

static int range_slice_impl(lua_State *L)
{
   tiri_range *r = check_range(L, 2);
   if (not r) lj_err_argt(L, 2, LUA_TUSERDATA);

   cTValue *o = L->base;

   // String slicing
   if (tvisstr(o)) {
      GCstr *str = strV(o);
      int32_t len = int32_t(str->len);
      tiri_index_range index_range;
      range_check_index(L, r, &index_range);
      int32_t start = index_range.start;
      int32_t stop = index_range.stop;
      int32_t step = index_range.step;

      // Resolve negative indices while preserving the range's inclusive/exclusive mode.
      bool use_inclusive = index_range.inclusive;
      if (start < 0 or stop < 0) {
         if (start < 0) start += len;
         if (stop < 0) stop += len;
      }

      // Determine iteration direction
      bool forward = (start <= stop);
      if (step IS 0) step = forward ? 1 : -1;
      if (forward and step < 0) step = 1;
      if (not forward and step > 0) step = -1;

      // Calculate effective stop for exclusive ranges
      int32_t effective_stop = stop;
      if (not use_inclusive) {
         if (forward) effective_stop = stop - 1;
         else effective_stop = stop + 1;
      }

      // Bounds clipping
      if (forward) {
         if (start < 0) start = 0;
         if (effective_stop >= len) effective_stop = len - 1;
      }
      else {
         if (start >= len) start = len - 1;
         if (effective_stop < 0) effective_stop = 0;
      }

      // Check for empty/invalid ranges
      if (forward and start > effective_stop) {
         lua_pushstring(L, "");
         return 1;
      }
      if (not forward and start < effective_stop) {
         lua_pushstring(L, "");
         return 1;
      }

      // For simple forward contiguous slices (step=1), use efficient substring
      if (forward and step IS 1) {
         int32_t sublen = effective_stop - start + 1;
         lua_pushlstring(L, strdata(str) + start, size_t(sublen));
         return 1;
      }

      // For reverse or stepped slices, build the result character by character
      int32_t result_size = 0;
      if (forward) result_size = ((effective_stop - start) / step) + 1;
      else result_size = ((start - effective_stop) / (-step)) + 1;

      // Use LuaJIT's string buffer
      SBuf *sb = lj_buf_tmp_(L);
      lj_buf_reset(sb);
      (void)lj_buf_need(sb, MSize(result_size));

      const char *src = strdata(str);

      if (forward) {
         for (int32_t i = start; i <= effective_stop; i += step) {
            *sb->w++ = src[i];
         }
      }
      else {
         for (int32_t i = start; i >= effective_stop; i += step) {
            *sb->w++ = src[i];
         }
      }

      setstrV(L, L->top++, lj_buf_str(L, sb));
      return 1;
   }

   // Table slicing
   if (tvistab(o)) {
      GCtab *t = tabV(o);
      int32_t len = int32_t(lj_tab_len(t));
      tiri_index_range index_range;
      range_check_index(L, r, &index_range);
      int32_t start = index_range.start;
      int32_t stop = index_range.stop;
      int32_t step = index_range.step;

      // Resolve negative indices while preserving the range's inclusive/exclusive mode.
      bool use_inclusive = index_range.inclusive;
      if (start < 0 or stop < 0) {
         if (start < 0) start += len;
         if (stop < 0) stop += len;
      }

      // Determine iteration direction
      bool forward = (start <= stop);
      if (step IS 0) step = forward ? 1 : -1;
      if (forward and step < 0) step = 1;
      if (not forward and step > 0) step = -1;

      // Calculate effective stop for exclusive ranges
      int32_t effective_stop = stop;
      if (not use_inclusive) {
         if (forward) effective_stop = stop - 1;
         else effective_stop = stop + 1;
      }

      // Bounds clipping
      if (forward) {
         if (start < 0) start = 0;
         if (effective_stop >= len) effective_stop = len - 1;
      }
      else {
         if (start >= len) start = len - 1;
         if (effective_stop < 0) effective_stop = 0;
      }

      // Check for empty/invalid ranges
      if (forward and start > effective_stop) {
         lua_createtable(L, 0, 0);
         return 1;
      }
      if (not forward and start < effective_stop) {
         lua_createtable(L, 0, 0);
         return 1;
      }

      // Calculate result size
      int32_t result_size = 0;
      if (forward) result_size = ((effective_stop - start) / step) + 1;
      else result_size = ((start - effective_stop) / (-step)) + 1;

      // Create result table
      lua_createtable(L, result_size, 0);
      int result_table_idx = lua_gettop(L);
      int32_t result_idx = 0;

      // Copy elements
      if (forward) {
         for (int32_t i = start; i <= effective_stop; i += step) {
            cTValue *src = lj_tab_getint(t, i);
            if (src and not tvisnil(src)) {
               copyTV(L, L->top, src);
               L->top++;
               lua_rawseti(L, result_table_idx, result_idx++);
            }
            else {
               lua_pushnil(L);
               lua_rawseti(L, result_table_idx, result_idx++);
            }
         }
      }
      else {
         for (int32_t i = start; i >= effective_stop; i += step) {
            cTValue *src = lj_tab_getint(t, i);
            if (src and not tvisnil(src)) {
               copyTV(L, L->top, src);
               L->top++;
               lua_rawseti(L, result_table_idx, result_idx++);
            }
            else {
               lua_pushnil(L);
               lua_rawseti(L, result_table_idx, result_idx++);
            }
         }
      }

      return 1;
   }

   // Array slicing
   if (tvisarray(o)) {
      GCarray *arr = arrayV(o);
      int32_t len = int32_t(arr->len);
      tiri_index_range index_range;
      range_check_index(L, r, &index_range);
      int32_t start = index_range.start;
      int32_t stop = index_range.stop;
      int32_t step = index_range.step;

      // Resolve negative indices while preserving the range's inclusive/exclusive mode.
      bool use_inclusive = index_range.inclusive;
      if (start < 0 or stop < 0) {
         if (start < 0) start += len;
         if (stop < 0) stop += len;
      }

      // Determine iteration direction
      bool forward = (start <= stop);
      if (step IS 0) step = forward ? 1 : -1;
      if (forward and step < 0) step = 1;
      if (not forward and step > 0) step = -1;

      // Calculate effective stop for exclusive ranges
      int32_t effective_stop = stop;
      if (not use_inclusive) {
         if (forward) effective_stop = stop - 1;
         else effective_stop = stop + 1;
      }

      // Bounds clipping
      if (forward) {
         if (start < 0) start = 0;
         if (effective_stop >= len) effective_stop = len - 1;
      }
      else {
         if (start >= len) start = len - 1;
         if (effective_stop < 0) effective_stop = 0;
      }

      // Check for empty/invalid ranges
      if (len IS 0 or (forward and start > effective_stop) or (not forward and start < effective_stop)) {
         GCarray *new_arr = lj_array_new(L, 0, arr->elemtype);
         // Per-instance metatable is null - base metatable will be used automatically
         setarrayV(L, L->top++, new_arr);
         return 1;
      }

      // Calculate result size
      int32_t result_size = 0;
      if (forward) result_size = ((effective_stop - start) / step) + 1;
      else result_size = ((start - effective_stop) / (-step)) + 1;

      // Create result array
      GCarray *new_arr = lj_array_new(L, MSize(result_size), arr->elemtype);
      auto src_base = arr->get<uint8_t>();
      auto dst_base = new_arr->get<uint8_t>();
      MSize elemsize = arr->elemsize;

      // Copy elements
      int32_t dst_idx = 0;
      if (forward) {
         for (int32_t i = start; i <= effective_stop; i += step) {
            memcpy(dst_base + dst_idx * elemsize, src_base + i * elemsize, elemsize);
            dst_idx++;
         }
      }
      else {
         for (int32_t i = start; i >= effective_stop; i += step) {
            memcpy(dst_base + dst_idx * elemsize, src_base + i * elemsize, elemsize);
            dst_idx++;
         }
      }

      setarrayV(L, L->top++, new_arr); // Push array onto the stack
      return 1;
   }

   // Unsupported type
   lj_err_arg(L, 1, ErrMsg::SLARGRNG);
   return 0;
}

LJLIB_CF(range_slice)
{
   return range_slice_impl(L);
}

// Exported wrapper

int lj_range_slice(lua_State *L)
{
   return range_slice_impl(L);
}

//********************************************************************************************************************

#include "lj_libdef.h"

//********************************************************************************************************************
// Register the range library

extern "C" int luaopen_range(lua_State *L)
{
   // Create metatable for range objects
   luaL_newmetatable(L, RANGE_METATABLE);

   // Set __name
   lua_pushstring(L, RANGE_METATABLE);
   lua_setfield(L, -2, "__name");

   // Register metamethods
   lua_pushcfunction(L, fp_range_tostring);
   lua_setfield(L, -2, "__tostring");

   lua_pushcfunction(L, fp_range_eq);
   lua_setfield(L, -2, "__eq");

   lua_pushcfunction(L, fp_range_len);
   lua_setfield(L, -2, "__len");

   lua_pushcfunction(L, range_index);
   lua_setfield(L, -2, "__index");

   lua_pushcfunction(L, fp_range_call);
   lua_setfield(L, -2, "__call");

   lua_pop(L, 1);  // Pop metatable

   // Register the library using LuaJIT's library registration system
   LJ_LIB_REG(L, "range", range);

   // At this point the range table is on the stack, add a metatable with __call

   lua_createtable(L, 0, 1);  // Create metatable for the library table
   lua_pushcfunction(L, range_lib_call);
   lua_setfield(L, -2, "__call");
   lua_setmetatable(L, -2);  // Set metatable on the range library table

   // Register prototypes for range methods (used for type inference)

   reg_func_prototype("range", { TiriType::Range }, { TiriType::Num, TiriType::Num, TiriType::Bool, TiriType::Num });
   reg_iface_prototype("range", "new", { TiriType::Range },
      { TiriType::Num, TiriType::Num, TiriType::Bool, TiriType::Num });
   reg_iface_prototype("range", "slice", { TiriType::Any }, { TiriType::Any, TiriType::Range });
   reg_iface_prototype("range", "each", { TiriType::Range }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "filter", { TiriType::Array }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "reduce", { TiriType::Any }, { TiriType::Range, TiriType::Any, TiriType::Func });
   reg_iface_prototype("range", "map", { TiriType::Array }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "take", { TiriType::Array }, { TiriType::Range, TiriType::Num });
   reg_iface_prototype("range", "any", { TiriType::Bool }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "all", { TiriType::Bool }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "find", { TiriType::Num }, { TiriType::Range, TiriType::Func });
   reg_iface_prototype("range", "contains", { TiriType::Bool }, { TiriType::Range, TiriType::Num });
   reg_iface_prototype("range", "toArray", { TiriType::Array }, { TiriType::Range });

   return 1;
}
