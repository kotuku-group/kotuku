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

#include <cstdlib>
#include <cmath>
#include <cstring>

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
// Calculate the number of elements in a range

static int32_t range_length(const tiri_range *r)
{
   if (r->step IS 0) return 0;

   int32_t start = r->start;
   int32_t stop = r->stop;
   int32_t step = r->step;

   // Adjust stop for exclusive ranges

   if (not r->inclusive) {
      if (step > 0) stop = stop - 1;
      else stop = stop + 1;
   }

   // Calculate count

   if (step > 0) {
      if (stop < start) return 0;
      return ((stop - start) / step) + 1;
   }
   else {
      if (stop > start) return 0;
      return ((start - stop) / (-step)) + 1;
   }
}

//********************************************************************************************************************
// range:each(function(Value) ... end)

static int range_each(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range before setting up callback

   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      lua_pushvalue(L, 1);
      return 1;
   }

   lua_pushvalue(L, 2);
   int callback_index = lua_gettop(L);

   // Invoke callback and check for early termination (returns false)

   auto invoke_callback = [L, callback_index](int32_t Value) -> bool {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, Value);
      lua_call(L, 1, 1);
      bool terminate = (not lua_isnil(L, -1) and not lua_toboolean(L, -1));
      lua_pop(L, 1);
      return terminate;
   };

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      if (invoke_callback(value)) {
         lua_pop(L, 1);
         lua_pushvalue(L, 1);
         return 1;
      }
   }

   lua_pop(L, 1);
   lua_pushvalue(L, 1);
   return 1;
}

//********************************************************************************************************************
// range:filter(function(Value) return bool end) -> array
// Returns an array containing only values for which the predicate returns true.

static int range_filter(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range - return empty array
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      GCarray *arr = lj_array_new(L, 0, AET::ANY);
      setarrayV(L, L->top++, arr);
      return 1;
   }

   // Pre-allocate array to maximum possible size
   int32_t max_size = range_length(r);
   GCarray *arr = lj_array_new(L, MSize(max_size), AET::ANY);
   TValue *data = arr->get<TValue>();
   int32_t array_index = 0;

   lua_pushvalue(L, 2);  // Push callback
   int callback_index = lua_gettop(L);

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, value);
      lua_call(L, 1, 1);

      if (lua_toboolean(L, -1)) {
         setintV(&data[array_index++], value);
      }
      lua_pop(L, 1);
   }

   lua_pop(L, 1);  // Pop callback

   // Adjust array length to actual count
   arr->len = MSize(array_index);

   setarrayV(L, L->top++, arr);
   return 1;
}

//********************************************************************************************************************
// range:reduce(initial, function(Acc, Value) return new_acc end) -> value
// Folds the range into a single accumulated value.

static int range_reduce(lua_State *L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   // Arg 2: initial value (any type)
   // Arg 3: reducer function
   luaL_checktype(L, 3, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Start with initial value on stack
   lua_pushvalue(L, 2);
   int acc_index = lua_gettop(L);

   // Check for empty range - return initial value
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      return 1;
   }

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, 3);           // Push reducer function
      lua_pushvalue(L, acc_index);   // Push current accumulator
      lua_pushinteger(L, value);     // Push current value
      lua_call(L, 2, 1);             // Call reducer(acc, value)

      // Replace accumulator with result
      lua_replace(L, acc_index);
   }

   return 1;  // Return final accumulator
}

//********************************************************************************************************************
// range:map(function(Value) return transformed end) -> array
// Returns an array with each value transformed by the function.

static int range_map(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range - return empty array
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      GCarray *arr = lj_array_new(L, 0, AET::ANY);
      setarrayV(L, L->top++, arr);
      return 1;
   }

   // Create result array with exact size
   int32_t size = range_length(r);
   GCarray *arr = lj_array_new(L, MSize(size), AET::ANY);
   TValue *data = arr->get<TValue>();
   int32_t array_index = 0;

   lua_pushvalue(L, 2);  // Push callback
   int callback_index = lua_gettop(L);

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, value);
      lua_call(L, 1, 1);

      // Store transformed value in result array
      TValue *src = L->top - 1;
      copyTV(L, &data[array_index++], src);
      if (tvisgcv(src)) {
         lj_gc_objbarrier(L, arr, gcV(src));
      }
      lua_pop(L, 1);
   }

   lua_pop(L, 1);  // Pop callback

   setarrayV(L, L->top++, arr);
   return 1;
}

//********************************************************************************************************************
// range:take(n) -> array
// Returns an array containing the first n values from the range.

static int range_take(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   auto n = (int32_t)luaL_checkinteger(L, 2);
   if (n < 0) n = 0;

   auto step = r->step;
   auto stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range or zero take - return empty array
   if (n IS 0 or (step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      GCarray *arr = lj_array_new(L, 0, AET::INT32);
      setarrayV(L, L->top++, arr);
      return 1;
   }

   // Calculate actual count (may be less than n if range is shorter)
   int32_t range_len = range_length(r);
   int32_t actual_count = (n < range_len) ? n : range_len;

   // Create result array
   GCarray *arr = lj_array_new(L, MSize(actual_count), AET::INT32);
   int32_t *data = arr->get<int32_t>();

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   int32_t array_index = 0;
   for (int32_t value = r->start; should_continue(value, stop) and array_index < n; value += step) {
      data[array_index++] = value;
   }

   setarrayV(L, L->top++, arr);
   return 1;
}

//********************************************************************************************************************
// range:any(function(Value) return bool end) -> bool
// Returns true if any value in the range satisfies the predicate.

static int range_any(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range - return false
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      lua_pushboolean(L, 0);
      return 1;
   }

   lua_pushvalue(L, 2);  // Push callback
   int callback_index = lua_gettop(L);

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, value);
      lua_call(L, 1, 1);

      if (lua_toboolean(L, -1)) {
         lua_pop(L, 2);  // Pop result and callback
         lua_pushboolean(L, 1);
         return 1;
      }
      lua_pop(L, 1);
   }

   lua_pop(L, 1);  // Pop callback
   lua_pushboolean(L, 0);
   return 1;
}

//********************************************************************************************************************
// range:all(function(Value) return bool end) -> bool
// Returns true if all values in the range satisfy the predicate.

static int range_all(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range - return true (vacuous truth)
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      lua_pushboolean(L, 1);
      return 1;
   }

   lua_pushvalue(L, 2);  // Push callback
   int callback_index = lua_gettop(L);

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, value);
      lua_call(L, 1, 1);

      if (not lua_toboolean(L, -1)) {
         lua_pop(L, 2);  // Pop result and callback
         lua_pushboolean(L, 0);
         return 1;
      }
      lua_pop(L, 1);
   }

   lua_pop(L, 1);  // Pop callback
   lua_pushboolean(L, 1);
   return 1;
}

//********************************************************************************************************************
// range:find(function(Value) return bool end) -> value or nil
// Returns the first value that satisfies the predicate, or nil if none found.

static int range_find(lua_State* L)
{
   auto r = check_range(L, 1);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   luaL_checktype(L, 2, LUA_TFUNCTION);

   int32_t step = r->step;
   int32_t stop = r->stop;

   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   // Check for empty range - return nil
   if ((step > 0 and r->start > stop) or (step < 0 and r->start < stop)) {
      lua_pushnil(L);
      return 1;
   }

   lua_pushvalue(L, 2);  // Push callback
   int callback_index = lua_gettop(L);

   auto should_continue = (step > 0)
      ? [](int32_t Value, int32_t Stop) { return Value <= Stop; }
      : [](int32_t Value, int32_t Stop) { return Value >= Stop; };

   for (int32_t value = r->start; should_continue(value, stop); value += step) {
      lua_pushvalue(L, callback_index);
      lua_pushinteger(L, value);
      lua_call(L, 1, 1);

      if (lua_toboolean(L, -1)) {
         lua_pop(L, 2);  // Pop result and callback
         lua_pushinteger(L, value);
         return 1;
      }
      lua_pop(L, 1);
   }

   lua_pop(L, 1);  // Pop callback
   lua_pushnil(L);
   return 1;
}

//********************************************************************************************************************
// range.new(start, stop [, inclusive [, step]])
// Creates a new range object

LJLIB_CF(range_new)
{
   if (lua_gettop(L) < 2) { // Check required arguments
      lj_err_caller(L, ErrMsg::NUMRNG);
      return 0;
   }

   if (not lua_isnumber(L, 1)) { // Validate start is a number
      lj_err_argt(L, 1, LUA_TNUMBER);
      return 0;
   }

   if (not lua_isnumber(L, 2)) { // Validate stop is a number
      lj_err_argt(L, 2, LUA_TNUMBER);
      return 0;
   }

   lua_Number start_num = lua_tonumber(L, 1);
   lua_Number stop_num = lua_tonumber(L, 2);

   // Check for integer values
   int32_t start = (int32_t)start_num;
   int32_t stop = (int32_t)stop_num;

   if ((lua_Number)start != start_num) {
      lj_err_arg(L, 1, ErrMsg::NUMRNG);
      return 0;
   }

   if ((lua_Number)stop != stop_num) {
      lj_err_arg(L, 2, ErrMsg::NUMRNG);
      return 0;
   }

   // Get optional inclusive flag (default: false)

   bool inclusive = false;
   if (lua_gettop(L) >= 3 and not lua_isnil(L, 3)) {
      inclusive = lua_toboolean(L, 3);
   }

   // Get optional step value

   int32_t step;
   if (lua_gettop(L) >= 4 and not lua_isnil(L, 4)) {
      if (not lua_isnumber(L, 4)) {
         lj_err_argt(L, 4, LUA_TNUMBER);
         return 0;
      }

      lua_Number step_num = lua_tonumber(L, 4);
      step = (int32_t)step_num;
      if ((lua_Number)step != step_num) {
         lj_err_arg(L, 4, ErrMsg::NUMRNG);
         return 0;
      }

      if (step IS 0) {
         lj_err_arg(L, 4, ErrMsg::NUMRNG);
         return 0;
      }
   }
   else { // Auto-detect step based on direction
      step = (start <= stop) ? 1 : -1;
   }

   // Create userdata
   auto r = (tiri_range *)lua_newuserdata(L, sizeof(tiri_range));
   r->start = start;
   r->stop = stop;
   r->step = step;
   r->inclusive = inclusive;

   // Set metatable
   luaL_getmetatable(L, RANGE_METATABLE);
   lua_setmetatable(L, -2);

   return 1;
}

//********************************************************************************************************************
// range.check(value)
// Returns true if the value is a range object

LJLIB_CF(range_check)
{
   auto r = check_range(L, 1);
   lua_pushboolean(L, r != nullptr);
   return 1;
}

//********************************************************************************************************************
// __tostring metamethod
// Returns "{start..stop}" or "{start...stop}" based on inclusivity

static int range_tostring(lua_State *L)
{
   auto r = get_range(L, 1);
   if (r->inclusive) lua_pushfstring(L, "{%d...%d}", r->start, r->stop);
   else lua_pushfstring(L, "{%d..%d}", r->start, r->stop);
   return 1;
}

//********************************************************************************************************************
// __eq metamethod
// Compares two ranges for equality (all fields must match)

static int range_eq(lua_State *L)
{
   auto r1 = check_range(L, 1);
   auto r2 = check_range(L, 2);

   if (not r1 or not r2) {
      lua_pushboolean(L, 0);
      return 1;
   }

   bool equal = (r1->start IS r2->start) and
                (r1->stop IS r2->stop) and
                (r1->step IS r2->step) and
                (r1->inclusive IS r2->inclusive);

   lua_pushboolean(L, equal);
   return 1;
}

//********************************************************************************************************************
// __len metamethod
// Returns the number of elements in the range

static int range_len(lua_State *L)
{
   auto r = get_range(L, 1);
   lua_pushinteger(L, range_length(r));
   return 1;
}

//********************************************************************************************************************
// range:contains(n)
// Returns true if n is within the range (respecting step)

static int range_contains(lua_State *L)
{
   auto r = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   // Handle both r:contains(n) and r.contains(n) syntax
   // With method syntax, position 1 is self (userdata), position 2 is the value
   // With function syntax via upvalue, position 1 is the value
   int arg_pos = lua_isuserdata(L, 1) ? 2 : 1;

   if (not lua_isnumber(L, arg_pos)) {
      lua_pushboolean(L, 0);
      return 1;
   }

   lua_Number n_num = lua_tonumber(L, arg_pos);
   int32_t n = (int32_t)n_num;

   // Check if it's an integer
   if ((lua_Number)n != n_num) {
      lua_pushboolean(L, 0);
      return 1;
   }

   int32_t start = r->start;
   int32_t stop = r->stop;
   int32_t step = r->step;

   // Calculate effective stop for exclusive ranges

   int32_t effective_stop = stop;
   if (not r->inclusive) {
      if (step > 0) effective_stop = stop - 1;
      else effective_stop = stop + 1;
   }

   // Check bounds

   if (step > 0) {
      if (n < start or n > effective_stop) {
         lua_pushboolean(L, 0);
         return 1;
      }
   }
   else {
      if (n > start or n < effective_stop) {
         lua_pushboolean(L, 0);
         return 1;
      }
   }

   // Check step alignment

   int32_t diff = n - start;
   if (std::abs(diff) % std::abs(step) != 0) {
      lua_pushboolean(L, 0);
      return 1;
   }

   lua_pushboolean(L, 1);
   return 1;
}

//********************************************************************************************************************
// range:toArray()
// Returns an array containing all values in the range

static int range_toarray(lua_State *L)
{
   auto r = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   int32_t len = range_length(r);

   // Create array with appropriate size
   GCarray *arr = lj_array_new(L, MSize(len), AET::INT32);

   if (len IS 0) {
      setarrayV(L, L->top++, arr);
      return 1;
   }

   int32_t *data = arr->get<int32_t>();
   int32_t step = r->step;
   int32_t stop = r->stop;

   // Calculate effective stop
   if (not r->inclusive) {
      if (step > 0) stop--;
      else stop++;
   }

   int32_t idx = 0;
   if (step > 0) {
      for (int32_t i = r->start; i <= stop; i += step) {
         data[idx++] = i;
      }
   }
   else {
      for (int32_t i = r->start; i >= stop; i += step) {
         data[idx++] = i;
      }
   }

   setarrayV(L, L->top++, arr);
   return 1;
}

//********************************************************************************************************************
// __index metamethod
// Handles property access (.start, .stop, .step, .inclusive, .length)
// and method calls (:contains, :toArray)

constexpr auto HASH_start     = pf::strhash("start");
constexpr auto HASH_stop      = pf::strhash("stop");
constexpr auto HASH_step      = pf::strhash("step");
constexpr auto HASH_inclusive = pf::strhash("inclusive");
constexpr auto HASH_length    = pf::strhash("length");
constexpr auto HASH_contains  = pf::strhash("contains");
constexpr auto HASH_toArray   = pf::strhash("toArray");
constexpr auto HASH_each      = pf::strhash("each");
constexpr auto HASH_filter    = pf::strhash("filter");
constexpr auto HASH_reduce    = pf::strhash("reduce");
constexpr auto HASH_map       = pf::strhash("map");
constexpr auto HASH_take      = pf::strhash("take");
constexpr auto HASH_any       = pf::strhash("any");
constexpr auto HASH_all       = pf::strhash("all");
constexpr auto HASH_find      = pf::strhash("find");

static int range_index(lua_State *Lua)
{
   auto r = get_range(Lua, 1);

   if (lua_type(Lua, 2) IS LUA_TSTRING) {
      auto hash = pf::strhash(lua_tostring(Lua, 2));

      switch(hash) {
         case HASH_start:     lua_pushinteger(Lua, r->start); return 1;
         case HASH_stop:      lua_pushinteger(Lua, r->stop); return 1;
         case HASH_step:      lua_pushinteger(Lua, r->step); return 1;
         case HASH_inclusive: lua_pushboolean(Lua, r->inclusive); return 1;
         case HASH_length:    lua_pushinteger(Lua, range_length(r)); return 1;
         case HASH_each:      lua_pushcfunction(Lua, range_each); return 1;
         case HASH_filter:    lua_pushcfunction(Lua, range_filter); return 1;
         case HASH_reduce:    lua_pushcfunction(Lua, range_reduce); return 1;
         case HASH_map:       lua_pushcfunction(Lua, range_map); return 1;
         case HASH_take:      lua_pushcfunction(Lua, range_take); return 1;
         case HASH_any:       lua_pushcfunction(Lua, range_any); return 1;
         case HASH_all:       lua_pushcfunction(Lua, range_all); return 1;
         case HASH_find:      lua_pushcfunction(Lua, range_find); return 1;
         case HASH_contains: // Methods - return closures with range as upvalue
            lua_pushvalue(Lua, 1);  // Push the range userdata
            lua_pushcclosure(Lua, range_contains, 1);
            return 1;
         case HASH_toArray:
            lua_pushvalue(Lua, 1);  // Push the range userdata
            lua_pushcclosure(Lua, range_toarray, 1);
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
   // Remove the table argument (first argument in __call is the table itself)
   lua_remove(L, 1);

   if (lua_gettop(L) < 2) { // Check required arguments
      lj_err_caller(L, ErrMsg::NUMRNG);
      return 0;
   }

   if (not lua_isnumber(L, 1)) {  // Validate start is a number
      lj_err_argt(L, 1, LUA_TNUMBER);
      return 0;
   }

   if (not lua_isnumber(L, 2)) {  // Validate stop is a number
      lj_err_argt(L, 2, LUA_TNUMBER);
      return 0;
   }

   lua_Number start_num = lua_tonumber(L, 1);
   lua_Number stop_num = lua_tonumber(L, 2);

   // Check for integer values
   int32_t start = (int32_t)start_num;
   int32_t stop = (int32_t)stop_num;

   if ((lua_Number)start != start_num) {
      lj_err_arg(L, 1, ErrMsg::NUMRNG);
      return 0;
   }

   if ((lua_Number)stop != stop_num) {
      lj_err_arg(L, 2, ErrMsg::NUMRNG);
      return 0;
   }

   // Get optional inclusive flag (default: false)
   bool inclusive = false;
   if (lua_gettop(L) >= 3 and not lua_isnil(L, 3)) {
      inclusive = lua_toboolean(L, 3);
   }

   // Get optional step value
   int32_t step;
   if (lua_gettop(L) >= 4 and not lua_isnil(L, 4)) {
      if (not lua_isnumber(L, 4)) {
         lj_err_argt(L, 4, LUA_TNUMBER);
         return 0;
      }
      lua_Number step_num = lua_tonumber(L, 4);
      step = (int32_t)step_num;
      if ((lua_Number)step != step_num) {
         lj_err_arg(L, 4, ErrMsg::NUMRNG);
         return 0;
      }
      if (step IS 0) {
         lj_err_arg(L, 4, ErrMsg::NUMRNG);
         return 0;
      }
   }
   else {
      // Auto-detect step based on direction
      step = (start <= stop) ? 1 : -1;
   }

   // Create userdata
   auto r = (tiri_range *)lua_newuserdata(L, sizeof(tiri_range));
   r->start = start;
   r->stop = stop;
   r->step = step;
   r->inclusive = inclusive;

   luaL_getmetatable(L, RANGE_METATABLE);
   lua_setmetatable(L, -2);

   return 1;
}

//********************************************************************************************************************
// Iterator function for range iteration
// Called repeatedly by the for loop until it returns nil
//
// Generic for loop calls: iterator(state, control_var)
// We use: iterator(nil, previous_value) where previous_value is what we returned last time

static int range_iterator_next(lua_State *L)
{
   // Upvalue 1: the range userdata
   auto r = (tiri_range *)lua_touserdata(L, lua_upvalueindex(1));
   if (not r) return 0;

   // Argument 2 is the control variable (previous return value, or initial value on first call)
   // For generic for: f(s, var) where var is the control variable

   int32_t current;
   if (lua_isnil(L, 2)) { // First iteration - return the start value
      current = r->start;
   }
   else { // Subsequent iterations - advance from previous value
      current = (int32_t)lua_tointeger(L, 2) + r->step;
   }

   // Calculate the limit
   int32_t limit = r->stop;
   if (not r->inclusive) {
      // For exclusive ranges, adjust the limit
      if (r->step > 0) limit = r->stop - 1;
      else limit = r->stop + 1;
   }

   // Check if we've passed the end
   if (r->step > 0) {
      if (current > limit) return 0;  // Iteration complete
   }
   else {
      if (current < limit) return 0;  // Iteration complete
   }

   // Return the current value (becomes the new control variable)
   lua_pushinteger(L, current);
   return 1;
}

//********************************************************************************************************************
// __call metamethod for range userdata
// Enables `for i in range do` syntax by returning iterator, state, initial value

static int range_call(lua_State *L)
{
   // Argument 1 is the range userdata itself
   auto r = (tiri_range *)luaL_checkudata(L, 1, RANGE_METATABLE);
   if (not r) {
      lj_err_caller(L, ErrMsg::BADVAL);
      return 0;
   }

   // Detect misuse: if called with 2+ args where arg2 is nil or number,
   // this looks like Lua's for-loop is calling us as an iterator function
   // rather than us being called once to return the iterator.
   // Correct usage: r() returns (iter, nil, nil), then for-loop calls iter(nil, nil)
   // Incorrect: for i in r do -> for-loop calls r(nil, nil) directly

   if (lua_gettop(L) >= 2) {
      int arg2_type = lua_type(L, 2);
      if (arg2_type IS LUA_TNIL or arg2_type IS LUA_TNUMBER) {
         luaL_error(L, ERR::Syntax, "range used incorrectly in for loop; use 'for i in range()' not 'for i in range'");
         return 0;
      }
   }

   // Return iterator function (closure with range as upvalue), nil state, nil initial
   lua_pushvalue(L, 1);  // Push the range userdata as upvalue
   lua_pushcclosure(L, range_iterator_next, 1);  // Create iterator closure
   lua_pushnil(L);       // State (not used, range is in upvalue)
   lua_pushnil(L);       // Initial control variable (nil triggers first iteration logic)
   return 3;
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
      int32_t start = r->start;
      int32_t stop = r->stop;
      int32_t step = r->step;

      // Handle negative indices (always inclusive for negative ranges)
      bool use_inclusive = r->inclusive;
      if (start < 0 or stop < 0) {
         use_inclusive = true;
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
      int32_t start = r->start;
      int32_t stop = r->stop;
      int32_t step = r->step;

      // Handle negative indices
      bool use_inclusive = r->inclusive;
      if (start < 0 or stop < 0) {
         use_inclusive = true;
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
      int32_t start = r->start;
      int32_t stop = r->stop;
      int32_t step = r->step;

      // Handle negative indices
      bool use_inclusive = r->inclusive;
      if (start < 0 or stop < 0) {
         use_inclusive = true;
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
   lua_pushcfunction(L, range_tostring);
   lua_setfield(L, -2, "__tostring");

   lua_pushcfunction(L, range_eq);
   lua_setfield(L, -2, "__eq");

   lua_pushcfunction(L, range_len);
   lua_setfield(L, -2, "__len");

   lua_pushcfunction(L, range_index);
   lua_setfield(L, -2, "__index");

   lua_pushcfunction(L, range_call);
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

   reg_iface_prototype("range", "check", { TiriType::Bool }, { TiriType::Any });
   reg_iface_prototype("range", "new", { TiriType::Range }, { TiriType::Num, TiriType::Num });
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
