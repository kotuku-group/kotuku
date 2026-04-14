// Unit tests for native array type.
// Copyright (C) 2025-2026 Paul Manias

#include <kotuku/main.h>

#ifdef ENABLE_UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_array.h"
#include "lj_tab.h"
#include "lj_vmarray.h"

#include <cmath>
#include <cstring>
#include <array>

#include "../../defs.h"

static objScript* glArrayTestScript = nullptr;

// Helper: dostring equivalent - loads and executes a string
static int dostring(lua_State *L, const char* s)
{
   int result = lua_load(L, std::string_view(s, strlen(s)), "test");
   if (result IS 0) result = lua_pcall(L, 0, LUA_MULTRET, 0);
   return result;
}

namespace {

struct TestCase {
   const char* name;
   bool (*fn)(pf::Log &Log);
};

struct LuaStateHolder {
   LuaStateHolder()
   {
      this->state = luaL_newstate(glArrayTestScript);
   }

   ~LuaStateHolder()
   {
      if (this->state) {
         lua_close(this->state);
      }
   }

   lua_State* get() const { return this->state; }

private:
   lua_State* state = nullptr;
};

//********************************************************************************************************************
// Core Data Structures

static bool test_array_creation_byte(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 100, AET::BYTE);

   if (not arr) {
      Log.error("char array creation failed");
      return false;
   }
   if (arr->len != 100) {
      Log.error("char array has incorrect length: %d", arr->len);
      return false;
   }
   if (arr->elemtype != AET::BYTE) {
      Log.error("char array has incorrect elemtype: %d", int(arr->elemtype));
      return false;
   }
   if (arr->elemsize != sizeof(uint8_t)) {
      Log.error("char array has incorrect elemsize: %d", arr->elemsize);
      return false;
   }
   if (arr->storage IS nullptr) {
      Log.error("char array storage is null");
      return false;
   }
   if (arr->arraydata() IS nullptr) {
      Log.error("char array data pointer is null");
      return false;
   }

   // Note: Primitive type arrays (BYTE, INT16, etc.) are NOT zero-initialised for performance.
   // Only vulnerable types (PTR, CSTR, etc.) are zeroed to avoid dangling pointers.
   // Use arr->zero() explicitly if zero-initialisation is required.

   return true;
}

static bool test_array_creation_int32(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 50, AET::INT32);

   if (not arr) {
      Log.error("int32 array creation failed");
      return false;
   }
   if (arr->len != 50) {
      Log.error("int32 array has incorrect length: %d", arr->len);
      return false;
   }
   if (arr->elemsize != sizeof(int32_t)) {
      Log.error("int32 array has incorrect elemsize: %d", arr->elemsize);
      return false;
   }

   // Write and read back values
   int32_t* data = (int32_t*)arr->arraydata();
   for (int i = 0; i < 50; i++) {
      data[i] = i * 100;
   }

   for (int i = 0; i < 50; i++) {
      if (data[i] != i * 100) {
         Log.error("int32 array read/write mismatch at index %d", i);
         return false;
      }
   }

   return true;
}

static bool test_array_creation_double(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 25, AET::DOUBLE);

   if (not arr) {
      Log.error("double array creation failed");
      return false;
   }
   if (arr->elemsize != sizeof(double)) {
      Log.error("double array has incorrect elemsize: %d", arr->elemsize);
      return false;
   }

   double* data = (double*)arr->arraydata();
   data[0] = 3.14159265358979;
   data[24] = -2.71828182845904;

   if (std::abs(data[0] - 3.14159265358979) > 1e-10) {
      Log.error("double array does not store pi correctly");
      return false;
   }
   if (std::abs(data[24] + 2.71828182845904) > 1e-10) {
      Log.error("double array does not store e correctly");
      return false;
   }

   return true;
}

static bool test_array_index_access(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::INT32);
   int32_t* data = (int32_t*)arr->arraydata();

   for (int i = 0; i < 10; i++) {
      data[i] = i + 1;
   }

   // Test lj_array_index
   int32_t* elem0 = (int32_t*)lj_array_index(arr, 0);
   int32_t* elem5 = (int32_t*)lj_array_index(arr, 5);
   int32_t* elem9 = (int32_t*)lj_array_index(arr, 9);

   if (*elem0 != 1) {
      Log.error("array_index returns incorrect element 0: %d", *elem0);
      return false;
   }
   if (*elem5 != 6) {
      Log.error("array_index returns incorrect element 5: %d", *elem5);
      return false;
   }
   if (*elem9 != 10) {
      Log.error("array_index returns incorrect element 9: %d", *elem9);
      return false;
   }

   return true;
}

static bool test_array_elemsize(pf::Log &Log)
{
   if (lj_array_elemsize(AET::BYTE) != 1) {
      Log.error("AET::BYTE size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT16) != 2) {
      Log.error("AET::INT16 size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT32) != 4) {
      Log.error("AET::INT32 size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::INT64) != 8) {
      Log.error("AET::INT64 size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::FLOAT) != 4) {
      Log.error("AET::FLOAT size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::DOUBLE) != 8) {
      Log.error("AET::DOUBLE size incorrect");
      return false;
   }
   if (lj_array_elemsize(AET::PTR) != sizeof(void*)) {
      Log.error("AET::PTR size incorrect");
      return false;
   }

   return true;
}

static bool test_array_external(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Create external buffer
   int32_t external_data[5] = { 10, 20, 30, 40, 50 };

   GCarray* arr = lj_array_new(L, 5, AET::INT32, external_data, ARRAY_EXTERNAL|ARRAY_READONLY);

   if (not arr) {
      Log.error("external array creation failed");
      return false;
   }
   if (!(arr->flags & ARRAY_EXTERNAL)) {
      Log.error("external array not marked as external");
      return false;
   }
   if (!(arr->flags & ARRAY_READONLY)) {
      Log.error("external array not marked as readonly");
      return false;
   }
   if (arr->arraydata() != external_data) {
      Log.error("external array does not point to original data");
      return false;
   }

   int32_t* data = (int32_t*)arr->arraydata();
   if (data[2] != 30) {
      Log.error("external array reads incorrectly: got %d, expected 30", data[2]);
      return false;
   }

   return true;
}

static bool test_array_to_table(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 5, AET::INT32);
   int32_t* data = (int32_t*)arr->arraydata();
   data[0] = 100;
   data[1] = 200;
   data[2] = 300;
   data[3] = 400;
   data[4] = 500;

   GCtab* t = lj_array_to_table(L, arr);
   if (not t) {
      Log.error("table creation from array failed");
      return false;
   }

   TValue* array_part = tvref(t->array);

   // Helper to get int value from TValue (handles both DUALNUM and non-DUALNUM modes)
   auto getintval = [](cTValue* o) -> int32_t {
      if (tvisint(o)) return intV(o);
      if (tvisnum(o)) return int32_t(numV(o));
      return 0;
   };

   // 0-based indexing: array[0] = 100, array[2] = 300, array[4] = 500
   if (!tvisnumber(&array_part[0])) {
      Log.error("table[0] is not a number, itype=%u", itype(&array_part[0]));
      return false;
   }
   if (getintval(&array_part[0]) != 100) {
      Log.error("table[0] has wrong value, expected 100");
      return false;
   }
   if (!tvisnumber(&array_part[2]) or getintval(&array_part[2]) != 300) {
      Log.error("table[2] is not 300");
      return false;
   }
   if (!tvisnumber(&array_part[4]) or getintval(&array_part[4]) != 500) {
      Log.error("table[4] is not 500");
      return false;
   }

   return true;
}

static bool test_array_type_tag(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::BYTE);

   if (arr->gct != uint8_t(~LJ_TARRAY)) {
      Log.error("array has incorrect GC type tag: %d, expected %d", arr->gct, uint8_t(~LJ_TARRAY));
      return false;
   }

   return true;
}

//********************************************************************************************************************
// VM Type System Integration

static bool test_tvalue_array(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::INT32);

   // Create a TValue holding the array
   TValue tv;
   setgcVraw(&tv, obj2gco(arr), LJ_TARRAY);

   if (itype(&tv) != LJ_TARRAY) {
      Log.error("TValue itype does not match LJ_TARRAY: %u vs %u", itype(&tv), LJ_TARRAY);
      return false;
   }
   if (!tvisarray(&tv)) {
      Log.error("tvisarray check failed");
      return false;
   }
   if (arrayV(&tv) != arr) {
      Log.error("arrayV does not extract correct pointer");
      return false;
   }

   return true;
}

static bool test_typename_array(pf::Log &Log)
{
   const char* name = lj_obj_itypename[~LJ_TARRAY];
   if (strcmp(name, "array") != 0) {
      Log.error("array typename is '%s', expected 'array'", name);
      return false;
   }

   return true;
}

static bool test_setarrayV(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 5, AET::DOUBLE);

   TValue tv;
   setarrayV(L, &tv, arr);

   if (!tvisarray(&tv)) {
      Log.error("setarrayV did not set array type");
      return false;
   }
   if (arrayV(&tv) != arr) {
      Log.error("setarrayV did not store correct pointer");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Bytecode C Helpers

// Helper to check if TValue contains an integer value (handles LJ_DUALNUM=0 case)
static bool tv_is_integer(cTValue* o, int32_t expected)
{
   if (tvisint(o)) return intV(o) IS expected;
   if (tvisnum(o)) return numberVint(o) IS expected;
   return false;
}

static bool test_arr_getidx_int32(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::INT32);
   int32_t* data = (int32_t*)arr->arraydata();
   for (int i = 0; i < 10; i++) {
      data[i] = (i + 1) * 100;  // 100, 200, 300, ...
   }

   TValue result;
   lj_arr_getidx(L, arr, 0, &result);
   if (not tv_is_integer(&result, 100)) {
      Log.error("arr_getidx at index 0 failed: expected 100");
      return false;
   }

   lj_arr_getidx(L, arr, 5, &result);
   if (not tv_is_integer(&result, 600)) {
      Log.error("arr_getidx at index 5 failed: expected 600");
      return false;
   }

   lj_arr_getidx(L, arr, 9, &result);
   if (not tv_is_integer(&result, 1000)) {
      Log.error("arr_getidx at index 9 failed: expected 1000");
      return false;
   }

   return true;
}

static bool test_arr_getidx_double(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 5, AET::DOUBLE);
   double* data = (double*)arr->arraydata();
   data[0] = 3.14159;
   data[2] = -2.71828;
   data[4] = 1.41421;

   TValue result;
   lj_arr_getidx(L, arr, 0, &result);
   if (!tvisnum(&result) or std::abs(numV(&result) - 3.14159) > 1e-5) {
      Log.error("arr_getidx double at index 0 failed");
      return false;
   }

   lj_arr_getidx(L, arr, 2, &result);
   if (!tvisnum(&result) or std::abs(numV(&result) + 2.71828) > 1e-5) {
      Log.error("arr_getidx double at index 2 failed");
      return false;
   }

   return true;
}

static bool test_arr_setidx_int32(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 10, AET::INT32);
   int32_t* data = (int32_t*)arr->arraydata();

   // Set values using lj_arr_setidx
   TValue val;
   setintV(&val, 12345);
   lj_arr_setidx(L, arr, 0, &val);

   setintV(&val, 67890);
   lj_arr_setidx(L, arr, 5, &val);

   setintV(&val, -99999);
   lj_arr_setidx(L, arr, 9, &val);

   // Verify values were stored correctly
   if (data[0] != 12345) {
      Log.error("arr_setidx at index 0 failed: got %d, expected 12345", data[0]);
      return false;
   }
   if (data[5] != 67890) {
      Log.error("arr_setidx at index 5 failed: got %d, expected 67890", data[5]);
      return false;
   }
   if (data[9] != -99999) {
      Log.error("arr_setidx at index 9 failed: got %d, expected -99999", data[9]);
      return false;
   }

   return true;
}

static bool test_arr_setidx_double(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 5, AET::DOUBLE);
   double* data = (double*)arr->arraydata();

   TValue val;
   setnumV(&val, 3.14159);
   lj_arr_setidx(L, arr, 0, &val);

   setnumV(&val, -2.71828);
   lj_arr_setidx(L, arr, 2, &val);

   if (std::abs(data[0] - 3.14159) > 1e-5) {
      Log.error("arr_setidx double at index 0 failed");
      return false;
   }
   if (std::abs(data[2] + 2.71828) > 1e-5) {
      Log.error("arr_setidx double at index 2 failed");
      return false;
   }

   return true;
}

static bool test_arr_roundtrip(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 100, AET::INT32);

   // Write values using setidx, read back using getidx
   for (int32_t i = 0; i < 100; i++) {
      TValue val;
      setintV(&val, i * i);
      lj_arr_setidx(L, arr, i, &val);
   }

   for (int32_t i = 0; i < 100; i++) {
      TValue result;
      lj_arr_getidx(L, arr, i, &result);
      if (not tv_is_integer(&result, i * i)) {
         Log.error("roundtrip failed at index %d: expected %d", i, i * i);
         return false;
      }
   }

   return true;
}

static bool test_arr_byte_type(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 256, AET::BYTE);
   uint8_t* data = (uint8_t*)arr->arraydata();

   // Test byte array stores and retrieves correctly
   for (int i = 0; i < 256; i++) {
      TValue val;
      setintV(&val, i);
      lj_arr_setidx(L, arr, i, &val);
   }

   for (int i = 0; i < 256; i++) {
      TValue result;
      lj_arr_getidx(L, arr, i, &result);
      if (not tv_is_integer(&result, i)) {
         Log.error("byte array roundtrip failed at index %d", i);
         return false;
      }
      if (data[i] != uint8_t(i)) {
         Log.error("byte array data mismatch at index %d", i);
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Library Functions

static bool test_lib_array_new(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.new via Lua
   const char* code = R"(
      local arr = array.new(100, "int")
      return arr != nil and #arr is 100 and array.type(arr) is "int"
   )";

   if (dostring(L, code) != 0) {
      Log.error("array.new test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array.new did not create array correctly");
      return false;
   }

   return true;
}

static bool test_lib_array_index(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array indexing via library metamethods
   const char* code = R"(
      local arr = array.new(10, "int")
      arr[0] = 100
      arr[5] = 500
      arr[9] = 900
      return arr[0] is 100 and arr[5] is 500 and arr[9] is 900
   )";

   if (dostring(L, code) != 0) {
      Log.error("array index test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array indexing did not work correctly");
      return false;
   }

   return true;
}

static bool test_lib_array_table(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.table conversion
   const char* code = R"(
      local arr = array.new(5, "int")
      arr[0] = 10
      arr[1] = 20
      arr[2] = 30
      arr[3] = 40
      arr[4] = 50
      local t = array.table(arr)
      return t[0] is 10 and t[2] is 30 and t[4] is 50
   )";

   if (dostring(L, code) != 0) {
      Log.error("array.table test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array.table conversion failed");
      return false;
   }

   return true;
}

static bool test_lib_array_copy(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.copy
   const char* code = R"(
      local src = array.new(5, "int")
      local dst = array.new(5, "int")
      src[0] = 100
      src[1] = 200
      src[2] = 300
      src[3] = 400
      src[4] = 500
      array.copy(dst, src)
      return dst[0] is 100 and dst[2] is 300 and dst[4] is 500
   )";

   if (dostring(L, code) != 0) {
      Log.error("array.copy test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array.copy did not copy correctly");
      return false;
   }

   return true;
}

static bool test_lib_array_string(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.getString and setString
   const char* code = R"(
      local arr = array.new(10, "char")
      array.setString(arr, "hello")
      local s = array.getString(arr, 0, 5)
      return s is "hello"
   )";

   if (dostring(L, code) != 0) {
      Log.error("array string test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array string operations failed");
      return false;
   }

   return true;
}

static bool test_lib_array_fill(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.fill
   const char* code = R"(
      local arr = array.new(10, "int")
      array.fill(arr, 42)
      local ok = true
      for i = 0, 9 do
         if arr[i] != 42 then ok = false end
      end
      return ok
   )";

   if (dostring(L, code) != 0) {
      Log.error("array.fill test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array.fill did not fill correctly");
      return false;
   }

   return true;
}

static bool test_lib_array_len_operator(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test # operator via __len metamethod
   const char* code = R"(
      local arr = array.new(42, "double")
      return #arr is 42
   )";

   if (dostring(L, code) != 0) {
      Log.error("array # operator test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array # operator did not return correct length");
      return false;
   }

   return true;
}

static bool test_lib_array_double_type(pf::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test double array type
   const char* code = R"(
      local arr = array.new(5, "double")
      arr[0] = 3.14159
      arr[2] = -2.71828
      arr[4] = 1.41421
      local ok = math.abs(arr[0] - 3.14159) < 0.00001
      ok = ok and math.abs(arr[2] + 2.71828) < 0.00001
      ok = ok and math.abs(arr[4] - 1.41421) < 0.00001
      return ok
   )";

   if (dostring(L, code) != 0) {
      Log.error("array double type test code failed: %s", lua_tostring(L, -1));
      return false;
   }

   if (not lua_toboolean(L, -1)) {
      Log.error("array double type did not work correctly");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Test runner

} // namespace

void array_unit_tests(int &Passed, int &Total)
{
   constexpr std::array<TestCase, 25> Tests = { {
      // Core Data Structures
      { "array_creation_byte", test_array_creation_byte },
      { "array_creation_int32", test_array_creation_int32 },
      { "array_creation_double", test_array_creation_double },
      { "array_index_access", test_array_index_access },
      { "array_elemsize", test_array_elemsize },
      { "array_external", test_array_external },
      { "array_to_table", test_array_to_table },
      { "array_type_tag", test_array_type_tag },
      // VM Type System
      { "tvalue_array", test_tvalue_array },
      { "typename_array", test_typename_array },
      { "setarrayV", test_setarrayV },
      // Bytecode C Helpers
      { "arr_getidx_int32", test_arr_getidx_int32 },
      { "arr_getidx_double", test_arr_getidx_double },
      { "arr_setidx_int32", test_arr_setidx_int32 },
      { "arr_setidx_double", test_arr_setidx_double },
      { "arr_roundtrip", test_arr_roundtrip },
      { "arr_byte_type", test_arr_byte_type },
      // Library Functions (basic integration - detailed tests in test_array.tiri)
      { "lib_array_new", test_lib_array_new },
      { "lib_array_index", test_lib_array_index },
      { "lib_array_table", test_lib_array_table },
      { "lib_array_copy", test_lib_array_copy },
      { "lib_array_string", test_lib_array_string },
      { "lib_array_fill", test_lib_array_fill },
      { "lib_array_len_operator", test_lib_array_len_operator },
      { "lib_array_double_type", test_lib_array_double_type }
   } };

   if (NewObject(CLASSID::TIRI, &glArrayTestScript) != ERR::Okay) return;
   glArrayTestScript->setStatement("");
   if (Action(AC::Init, glArrayTestScript, nullptr) != ERR::Okay) return;

   for (const TestCase& Test : Tests) {
      pf::Log Log("ArrayTests");
      Log.branch("Running %s", Test.name);
      ++Total;
      if (Test.fn(Log)) {
         ++Passed;
         Log.msg("%s passed", Test.name);
      }
      else {
         Log.error("%s failed", Test.name);
      }
   }

   FreeResource(glArrayTestScript);
   glArrayTestScript = nullptr;
}

#else

#endif // ENABLE_UNIT_TESTS
