// Unit tests for native array type.
// Copyright (C) 2025-2026 Paul Manias

#include <kotuku/main.h>

#ifdef UNIT_TESTS

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_array.h"
#include "lj_str.h"
#include "lj_struct.h"
#include "lj_tab.h"
#include "lj_object.h"
#include "lj_vmarray.h"

#include <cmath>
#include <cfloat>
#include <cstring>
#include <array>
#include <atomic>
#include <limits>

#include "../../defs.h"

static extTiri* glArrayTestScript = nullptr;
static std::atomic_int glArrayResourceFrees = 0;

static ERR array_test_resource_free(ResourceRecord &, APTR)
{
   glArrayResourceFrees.fetch_add(1, std::memory_order_relaxed);
   return ERR::Terminate;
}

static ResourceManager glArrayResourceManager = {
   "TiriArrayTest",
   &array_test_resource_free,
   false
};

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
   bool (*fn)(kt::Log &Log);
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

static bool test_array_creation_byte(kt::Log &Log)
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

static bool test_array_creation_int32(kt::Log &Log)
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

static bool test_array_recursive_identity(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) return false;

   GCarray *ordinary = lj_array_new(lua, 0, AET::ARRAY);
   if (not gcref(ordinary->type_identity) or
       std::string_view(strdata(strref(ordinary->type_identity)), strref(ordinary->type_identity)->len) !=
          "array<array>") {
      Log.error("the C++ convenience allocator did not derive an unconstrained canonical identity");
      return false;
   }

   ArrayAllocationDescriptor descriptor {
      .storage = AET::ARRAY,
      .canonical_identity = "array<array<double>>"
   };
   GCarray *matrix = lj_array_new(lua, 0, descriptor);
   setarrayV(lua, lua->top++, matrix);
   lua_gc(lua, LUA_GCCOLLECT);
   GCstr *identity = strref(matrix->type_identity);
   if (not identity or std::string_view(strdata(identity), identity->len) != "array<array<double>>") {
      Log.error("a descriptor-aware array allocation lost its identity during GC traversal");
      return false;
   }

   GCarray *matching = lj_array_new(lua, 0, AET::DOUBLE);
   GCarray *mismatch = lj_array_new(lua, 0, AET::STR_GC);
   TValue value;
   setarrayV(lua, &value, matching);
   if (lj_array_validate_element(matrix, &value) != ArrayElementResult::OK) {
      Log.error("recursive array storage rejected a matching inner identity");
      return false;
   }
   setarrayV(lua, &value, mismatch);
   if (lj_array_validate_element(matrix, &value) != ArrayElementResult::INVALID_TYPE) {
      Log.error("recursive array storage accepted a mismatched inner identity");
      return false;
   }

   ArrayAllocationDescriptor wildcard_descriptor {
      .storage = AET::ARRAY,
      .canonical_identity = "array<array<any>>"
   };
   GCarray *wildcard = lj_array_new(lua, 0, wildcard_descriptor);
   if (lj_array_validate_element(wildcard, &value) != ArrayElementResult::OK) {
      Log.error("a recursive wildcard rejected a valid inner array");
      return false;
   }
   return true;
}

//********************************************************************************************************************

static bool test_unsigned_array_types(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(lua);

   struct UnsignedArrayCase {
      AET type;
      uint8_t size;
      uint64_t value;
   };
   constexpr std::array<UnsignedArrayCase, 4> cases = { {
      { AET::UINT8, sizeof(uint8_t), 255 },
      { AET::UINT16, sizeof(uint16_t), 65535 },
      { AET::UINT32, sizeof(uint32_t), 4294967295ULL },
      { AET::UINT64, sizeof(uint64_t), 9007199254740991ULL }
   } };

   for (const auto &test : cases) {
      GCarray *array = lj_array_new(lua, 1, test.type);
      if (not array or array->elemsize != test.size or array->elemtype != test.type) {
         Log.error("unsigned array allocation has the wrong layout for type %d", int(test.type));
         return false;
      }

      switch (test.type) {
         case AET::UINT8:  array->get<uint8_t>()[0] = uint8_t(test.value); break;
         case AET::UINT16: array->get<uint16_t>()[0] = uint16_t(test.value); break;
         case AET::UINT32: array->get<uint32_t>()[0] = uint32_t(test.value); break;
         case AET::UINT64: array->get<uint64_t>()[0] = uint64_t(test.value); break;
         default: return false;
      }

      TValue result;
      lj_arr_getidx(lua, array, 0, &result);
      if (test.type IS AET::UINT8 or test.type IS AET::UINT16) {
         const bool matches = (tvisint(&result) and intV(&result) IS int32_t(test.value)) or
            (tvisnum(&result) and numberVint(&result) IS int32_t(test.value));
         if (not matches) {
            Log.error("unsigned small-width array readback failed for type %d", int(test.type));
            return false;
         }
      }
      else if (not tvisnum(&result) or numV(&result) != lua_Number(test.value)) {
         Log.error("unsigned wide array readback failed for type %d", int(test.type));
         return false;
      }
   }

   GCarray *wrapped = lj_array_new(lua, 1, AET::UINT32);
   TValue negative;
   setintV(&negative, -1);
   lj_arr_setidx(lua, wrapped, 0, &negative);
   if (wrapped->get<uint32_t>()[0] != std::numeric_limits<uint32_t>::max()) {
      Log.error("unsigned array negative conversion did not wrap modulo its width");
      return false;
   }

   GCarray *wide_wrapped = lj_array_new(lua, 1, AET::UINT64);
   TValue negative_number;
   setnumV(&negative_number, -1.0);
   lj_arr_setidx(lua, wide_wrapped, 0, &negative_number);
   if (wide_wrapped->get<uint64_t>()[0] != std::numeric_limits<uint64_t>::max()) {
      Log.error("uint64 array conversion lost a negative numeric value during modulo reduction");
      return false;
   }

   setnumV(&negative_number, -4294967295.0);
   lj_arr_setidx(lua, wide_wrapped, 0, &negative_number);
   if (wide_wrapped->get<uint64_t>()[0] != UINT64_C(18446744069414584321)) {
      Log.error("uint64 array conversion produced incorrect modulo bits for a wide negative number");
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Null-terminated pointer fields use a negative array size as a sentinel.  The struct reader must scan the pointed-to
// values before creating the cached array rather than passing that sentinel to the unsigned array allocator.

static bool test_struct_pointer_array_sentinel(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   int values[] = { 17, -4, 0 };
   APTR pointer = values;
   struct_field field;
   field.Type = FD_POINTER|FD_ARRAY|FD_INT;
   field.ArraySize = -1;

   lj_struct_getfield_core(lua, nullptr, field, &pointer);
   if (not lua_isarray(lua, -1)) {
      Log.error("struct pointer array field did not produce an array");
      return false;
   }

   auto array = lua_toarray(lua, -1);
   if ((array->len != 2) or (array->elemtype != AET::INT32)) {
      Log.error("struct pointer array field produced length %d and type %d", array->len, int(array->elemtype));
      return false;
   }

   auto result = array->get<int>();
   if ((result[0] != 17) or (result[1] != -4)) {
      Log.error("struct pointer array field did not copy values up to its sentinel");
      return false;
   }

   return true;
}

//********************************************************************************************************************
// struct_to_table() receives the address of a native structure rather than using the struct field getter.  String
// vectors require the vector object itself because the array cache reads std::string elements through its vector header.

static bool test_struct_to_table_string_vector(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   kt::vector<std::string> names;
   names.push_back("alpha");
   names.push_back("beta");

   struct_record def("StringArrayResult");
   def.Size = int(sizeof(names));
   struct_field field;
   field.Name = "Names";
   field.Type = FD_VECTOR|FD_CPP|FD_STRING;
   field.ArraySize = 1;
   def.Fields.push_back(field);

   std::vector<lua_ref> references;
   if (struct_to_table(lua, references, def, &names) != ERR::Okay) {
      Log.error("failed to convert native structure containing a string vector");
      unref_struct_references(lua, references);
      return false;
   }
   unref_struct_references(lua, references);

   lua_getfield(lua, -1, "Names");
   if (not lua_isarray(lua, -1)) {
      Log.error("string vector field did not produce an array");
      return false;
   }

   auto array = lua_toarray(lua, -1);
   if ((array->len != 2) or (array->elemtype != AET::CSTR)) {
      Log.error("string vector field produced length %d and type %d", array->len, int(array->elemtype));
      return false;
   }

   auto result = array->get<CSTRING>();
   if ((std::string_view(result[0]) != "alpha") or (std::string_view(result[1]) != "beta")) {
      Log.error("C++ string array field did not preserve its values");
      return false;
   }

   return true;
}

static bool test_array_creation_double(kt::Log &Log)
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

static bool test_array_index_access(kt::Log &Log)
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

static bool test_array_elemsize(kt::Log &Log)
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

static bool test_array_external(kt::Log &Log)
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

static bool test_array_external_unmanaged(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   int32_t external_data[2] = { 10, 20 };
   GCarray *array = lj_array_new(lua, 2, AET::INT32, external_data, ARRAY_EXTERNAL|ARRAY_READONLY);
   if (array->resource_id) {
      Log.error("unmanaged external array retained a resource pin");
      return false;
   }

   external_data[1] = 30;
   if (array->get<int32_t>()[1] != 30) {
      Log.error("unmanaged external array did not observe native mutation");
      return false;
   }

   setarrayV(lua, lua->top++, array);
   lua_settop(lua, 0);
   lua_gc(lua, LUA_GCCOLLECT);
   return true;
}

static bool test_array_external_resource(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(lua);

   APTR memory = nullptr;
   if (AllocResource(sizeof(int32_t) * 2, MEM::NIL, &memory, &glArrayResourceManager) != ERR::Okay) return false;
   auto resource_id = GetMemoryID(memory);
   auto values = (int32_t *)memory;
   values[0] = 10;
   values[1] = 20;
   glArrayResourceFrees.store(0, std::memory_order_relaxed);

   GCarray *array = lj_array_new(lua, 2, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, array);
   if (PinResource(resource_id) != ERR::Okay) return false;
   array->resource_id = resource_id;
   lua_setglobal(lua, "array_resource_view");

   if (dostring(lua, "local total = 0; for index in {0 to 200} do "
         "total += array_resource_view[index % #array_resource_view] end; assert(total is 3000)") != 0) {
      Log.error("resource-backed external array did not use the normal hot-loop load path");
      return false;
   }

   if ((array->resource_id != resource_id) or (FreeResource(resource_id) != ERR::InUse) or
       (array->get<int32_t>()[1] != 20)) {
      Log.error("resource-backed external array did not retain readable storage");
      return false;
   }

   lua_pushnil(lua);
   lua_setglobal(lua, "array_resource_view");
   lua_gc(lua, LUA_GCCOLLECT);
   if (glArrayResourceFrees.load(std::memory_order_relaxed) != 1) {
      Log.error("resource-backed external array did not release its pin exactly once");
      return false;
   }

   memory = nullptr;
   if (AllocResource(sizeof(int32_t), MEM::NIL, &memory, &glArrayResourceManager) != ERR::Okay) return false;
   resource_id = GetMemoryID(memory);
   values = (int32_t *)memory;
   values[0] = 42;
   glArrayResourceFrees.store(0, std::memory_order_relaxed);

   GCarray *first = lj_array_new(lua, 1, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, first);
   if (PinResource(resource_id) != ERR::Okay) return false;
   first->resource_id = resource_id;

   GCarray *second = lj_array_new(lua, 1, AET::INT32, memory, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, second);
   if (PinResource(resource_id) != ERR::Okay) return false;
   second->resource_id = resource_id;

   if (FreeResource(resource_id) != ERR::InUse) return false;
   lua_settop(lua, 1);
   lua_gc(lua, LUA_GCCOLLECT);
   if ((glArrayResourceFrees.load(std::memory_order_relaxed) != 0) or (first->get<int32_t>()[0] != 42)) {
      Log.error("the first collected view released a multiply pinned resource");
      return false;
   }

   lua_settop(lua, 0);
   lua_gc(lua, LUA_GCCOLLECT);
   if (glArrayResourceFrees.load(std::memory_order_relaxed) != 1) {
      Log.error("the final collected view did not release the deferred resource");
      return false;
   }

   int32_t local_value = 7;
   GCarray *failed = lj_array_new(lua, 1, AET::INT32, &local_value, ARRAY_EXTERNAL|ARRAY_READONLY);
   setarrayV(lua, lua->top++, failed);
   auto missing_id = AllocateID(IDTYPE::RESOURCE);
   if ((PinResource(missing_id) IS ERR::Okay) or failed->resource_id) {
      Log.error("a failed resource pin left an ownership token on the array");
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool test_array_element_contract(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *L = holder.get();
   if (not L) return false;
   luaL_openlibs(L);

   TValue value;
   GCarray *integers = lj_array_new(L, 2, AET::INT32);
   setnumV(&value, 12.75);
   if (lj_array_validate_element(integers, &value) != ArrayElementResult::OK) return false;
   lj_array_store_checked(L, integers, 0, &value);
   if (integers->get<int32_t>()[0] != 12) return false;

   setnumV(&value, std::numeric_limits<lua_Number>::infinity());
   if (lj_array_validate_element(integers, &value) != ArrayElementResult::OUT_OF_RANGE) return false;
   setnilV(&value);
   lj_array_store_checked(L, integers, 1, &value);
   if (integers->get<int32_t>()[1] != 0) return false;

   GCstr *string = lj_str_newlit(L, "12");
   setstrV(L, &value, string);
   if (lj_array_validate_element(integers, &value) != ArrayElementResult::INVALID_TYPE) return false;

   GCarray *floats = lj_array_new(L, 1, AET::FLOAT);
   setnumV(&value, double(FLT_MAX) * 2.0);
   if (lj_array_validate_element(floats, &value) != ArrayElementResult::OUT_OF_RANGE) return false;
   setnumV(&value, std::numeric_limits<lua_Number>::infinity());
   if (lj_array_validate_element(floats, &value) != ArrayElementResult::OK) return false;

   GCarray *strings = lj_array_new(L, 1, AET::STR_GC);
   setstrV(L, &value, string);
   lj_array_store_checked(L, strings, 0, &value);
   if (gcref(strings->get<GCRef>()[0]) != obj2gco(string)) return false;

   GCtab *table = lj_tab_new(L, 0, 0);
   settabV(L, &value, table);
   if (lj_array_validate_element(strings, &value) != ArrayElementResult::INVALID_TYPE) return false;
   GCarray *tables = lj_array_new(L, 1, AET::TABLE);
   if (lj_array_validate_element(tables, &value) != ArrayElementResult::OK) return false;

   GCarray *any = lj_array_new(L, 1, AET::ANY);
   lj_array_store_checked(L, any, 0, &value);
   if (not tvistab(&any->get<TValue>()[0])) return false;

   GCarray *pointers = lj_array_new(L, 1, AET::PTR);
   setrawlightudV(&value, integers);
   if (lj_array_validate_element(pointers, &value) != ArrayElementResult::OK) return false;
   GCarray *cached_strings = lj_array_new(L, 0, AET::CSTR);
   if (lj_array_validate_element(cached_strings, &value) != ArrayElementResult::UNSUPPORTED_STORAGE) return false;
   setnilV(&value);
   if (lj_array_validate_element(pointers, &value) != ArrayElementResult::INVALID_TYPE) return false;
   if (lj_array_validate_element(cached_strings, &value) != ArrayElementResult::UNSUPPORTED_STORAGE) return false;

   return true;
}

//********************************************************************************************************************

template<typename T>
static bool test_numeric_search_type(lua_State *L, AET ElementType, kt::Log &Log)
{
   GCarray *array = lj_array_new(L, 5, ElementType);
   T *values = array->get<T>();
   values[0] = T(3);
   values[1] = T(7);
   values[2] = T(11);
   values[3] = T(7);
   values[4] = T(19);

   if (lj_arr_find_num(array, 3, 0, 4, 1) != 0 or lj_arr_find_num(array, 11, 0, 4, 1) != 2 or
       lj_arr_find_num(array, 19, 0, 4, 1) != 4 or lj_arr_find_num(array, 23, 0, 4, 1) != -1 or
       lj_arr_find_num(array, 7, 0, 4, 2) != -1 or lj_arr_find_num(array, 7, 4, 0, -1) != 3) {
      Log.error("numeric runtime search failed for element type %d", int(ElementType));
      return false;
   }
   return true;
}

static bool test_array_runtime_search(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }

   if (not test_numeric_search_type<int32_t>(lua, AET::INT32, Log) or
       not test_numeric_search_type<uint32_t>(lua, AET::UINT32, Log) or
       not test_numeric_search_type<float>(lua, AET::FLOAT, Log) or
       not test_numeric_search_type<double>(lua, AET::DOUBLE, Log)) return false;

   GCarray *empty = lj_array_new(lua, 0, AET::INT32);
   if (lj_arr_find_num(empty, 1, 0, -1, 1) != -1) {
      Log.error("empty numeric runtime search returned a match");
      return false;
   }

   GCarray *single = lj_array_new(lua, 1, AET::INT16);
   single->get<int16_t>()[0] = 31;
   if (lj_arr_find_num(single, 31, 0, 0, 1) != 0 or lj_arr_find_num(single, 31, 1, 0, 1) != -1 or
       lj_arr_find_num(single, 31, 0, 1, -1) != -1) {
      Log.error("single-element or empty-span numeric runtime search failed");
      return false;
   }

   GCarray *strings = lj_array_new(lua, 3, AET::STR_GC);
   TValue value;
   const char embedded_text[] = { 'a', '\0', 'b' };
   GCstr *first = lj_str_newz(lua, "first");
   GCstr *embedded = lj_str_new(lua, embedded_text, sizeof(embedded_text));
   GCstr *last = lj_str_newz(lua, "last");
   setstrV(lua, &value, first);
   lj_array_store_checked(lua, strings, 0, &value);
   setstrV(lua, &value, embedded);
   lj_array_store_checked(lua, strings, 1, &value);
   setstrV(lua, &value, last);
   lj_array_store_checked(lua, strings, 2, &value);

   GCstr *embedded_match = lj_str_new(lua, embedded_text, sizeof(embedded_text));
   GCstr *missing = lj_str_newz(lua, "missing");
   if (lj_arr_find_str(strings, first, 0, 2, 1) != 0 or lj_arr_find_str(strings, embedded_match, 0, 2, 1) != 1 or
       lj_arr_find_str(strings, last, 2, 0, -1) != 2 or lj_arr_find_str(strings, missing, 0, 2, 1) != -1 or
       lj_arr_find_str(strings, embedded, 2, 0, -2) != -1) {
      Log.error("string runtime search failed");
      return false;
   }

   GCarray *objects = lj_array_new(lua, 2, AET::OBJECT);
   GCobject *first_object = lj_object_new(lua, OBJECTID(101), nullptr, nullptr, GCOBJ_DETACHED);
   GCobject *second_object = lj_object_new(lua, OBJECTID(202), nullptr, nullptr, GCOBJ_DETACHED);
   setobjectV(lua, &value, first_object);
   lj_array_store_checked(lua, objects, 0, &value);
   setobjectV(lua, &value, second_object);
   lj_array_store_checked(lua, objects, 1, &value);

   if (lj_arr_find_object(objects, first_object->uid, 0, 1, 1) != 0 or
       lj_arr_find_object(objects, second_object->uid, 1, 0, -1) != 1 or
       lj_arr_find_object(objects, OBJECTID(303), 0, 1, 1) != -1) {
      Log.error("object runtime search failed");
      return false;
   }

   return true;
}

static bool test_array_to_table(kt::Log &Log)
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

static bool test_array_type_tag(kt::Log &Log)
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

static bool test_tvalue_array(kt::Log &Log)
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

static bool test_typename_array(kt::Log &Log)
{
   const char* name = lj_obj_itypename[~LJ_TARRAY];
   if (strcmp(name, "array") != 0) {
      Log.error("array typename is '%s', expected 'array'", name);
      return false;
   }

   return true;
}

static bool test_setarrayV(kt::Log &Log)
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

static bool test_any_array_nil_initialisation(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 129, AET::ANY);
   TValue *slots = arr->get<TValue>();

   for (MSize i = 0; i < arr->len; i++) {
      if (not tvisnil(&slots[i])) {
         Log.error("ANY array slot %d is not nil after creation", int(i));
         return false;
      }
   }
   return true;
}

static bool test_any_array_grow_nil_initialisation(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 2, AET::ANY);
   TValue *slots = arr->get<TValue>();
   setintV(&slots[0], 17);
   setnumV(&slots[1], 25.0);

   if (not lj_array_grow(L, arr, 129)) {
      Log.error("ANY array grow failed");
      return false;
   }

   slots = arr->get<TValue>();
   if (not tv_is_integer(&slots[0], 17) or not tvisnum(&slots[1])) {
      Log.error("ANY array grow damaged existing values");
      return false;
   }

   for (MSize i = 2; i < arr->capacity; i++) {
      if (not tvisnil(&slots[i])) {
         Log.error("ANY array grown slot %d is not nil", int(i));
         return false;
      }
   }
   return true;
}

static bool test_any_array_bulk_copy_overlap(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 140, AET::ANY);
   TValue *slots = arr->get<TValue>();
   for (int32_t i = 0; i < 140; i++) setintV(&slots[i], i);

   lj_array_copy(L, arr, 2, arr, 0, 129);
   slots = arr->get<TValue>();
   for (int32_t i = 0; i < 129; i++) {
      if (not tv_is_integer(&slots[i + 2], i)) {
         Log.error("ANY array overlapping copy failed at slot %d", int(i + 2));
         return false;
      }
   }
   return true;
}

static bool test_any_array_to_table_mixed_values(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   GCarray* arr = lj_array_new(L, 4, AET::ANY);
   TValue *slots = arr->get<TValue>();
   setnilV(&slots[0]);
   setintV(&slots[1], 42);
   setnumV(&slots[2], 3.5);
   GCstr *str = lj_str_newz(L, "bulk");
   setstrV(L, &slots[3], str);
   lj_gc_objbarrier(L, arr, str);

   GCtab *table = lj_array_to_table(L, arr);
   TValue *array_part = tvref(table->array);

   if (not tvisnil(&array_part[0])) {
      Log.error("ANY table slot 0 is not nil");
      return false;
   }
   if (not tv_is_integer(&array_part[1], 42)) {
      Log.error("ANY table slot 1 is not 42");
      return false;
   }
   if (not tvisnum(&array_part[2]) or not (numV(&array_part[2]) IS 3.5)) {
      Log.error("ANY table slot 2 is not 3.5");
      return false;
   }
   if (not tvisstr(&array_part[3]) or not (strV(&array_part[3]) IS str)) {
      Log.error("ANY table slot 3 is not the source string");
      return false;
   }
   return true;
}

static bool test_arr_getidx_int32(kt::Log &Log)
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

static bool test_arr_getidx_double(kt::Log &Log)
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

static bool test_array_load_allocation_classification(kt::Log &Log)
{
   for (int type_index = 0; type_index < int(AET::MAX); type_index++) {
      AET element_type = AET(type_index);
      bool expected = element_type IS AET::CSTR or element_type IS AET::STR_CPP or element_type IS AET::STRUCT;
      if (array_element_load_allocates(element_type) != expected) {
         Log.error("array load allocation classification is incorrect for type %d", type_index);
         return false;
      }
   }
   return true;
}

static bool test_arr_getidx_noalloc(kt::Log &Log)
{
   LuaStateHolder holder;
   lua_State *lua = holder.get();
   if (not lua) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(lua);

   TValue result;
   GCarray *integers = lj_array_new(lua, 1, AET::INT32);
   integers->get<int32_t>()[0] = 73;
   lj_arr_getidx_noalloc(lua, integers, 0, &result);
   if (not tv_is_integer(&result, 73)) {
      Log.error("no-allocation integer array load failed");
      return false;
   }

   GCarray *strings = lj_array_new(lua, 1, AET::STR_GC);
   GCstr *text = lj_str_newz(lua, "native");
   TValue value;
   setstrV(lua, &value, text);
   lj_array_store_checked(lua, strings, 0, &value);
   lj_arr_getidx_noalloc(lua, strings, 0, &result);
   if (not tvisstr(&result) or strV(&result) != text) {
      Log.error("no-allocation GC-reference array load failed");
      return false;
   }

   GCarray *values = lj_array_new(lua, 3, AET::ANY);
   TValue *slots = values->get<TValue>();
   setnilV(&slots[0]);
   setnumV(&slots[1], 4.5);
   setstrV(lua, &slots[2], text);
   lj_gc_objbarrier(lua, values, text);

   lj_arr_getidx_noalloc(lua, values, 0, &result);
   if (not tvisnil(&result)) {
      Log.error("no-allocation ANY nil load failed");
      return false;
   }
   lj_arr_getidx_noalloc(lua, values, 1, &result);
   if (not tvisnum(&result) or numV(&result) != 4.5) {
      Log.error("no-allocation ANY number load failed");
      return false;
   }
   lj_arr_getidx_noalloc(lua, values, 2, &result);
   if (not tvisstr(&result) or strV(&result) != text) {
      Log.error("no-allocation ANY string load failed");
      return false;
   }

   return true;
}

static bool test_arr_setidx_int32(kt::Log &Log)
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

static bool test_arr_setidx_double(kt::Log &Log)
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

static bool test_arr_roundtrip(kt::Log &Log)
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

static bool test_arr_byte_type(kt::Log &Log)
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

static bool test_lib_array_new(kt::Log &Log)
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
      local arr:array<int> = array.new(100, "int")
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

static bool test_lib_array_index(kt::Log &Log)
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
      local arr:array<int> = array.new(10, "int")
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

static bool test_lib_array_table(kt::Log &Log)
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
      local arr:array<int> = array.new(5, "int")
      arr[0] = 10
      arr[1] = 20
      arr[2] = 30
      arr[3] = 40
      arr[4] = 50
      local t:table = array.table(arr)
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

static bool test_lib_array_copy(kt::Log &Log)
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
      local src:array<int> = array.new(5, "int")
      local dst:array<int> = array.new(5, "int")
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

static bool test_lib_array_string(kt::Log &Log)
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
      local arr:array<byte> = array.new(10, "char")
      array.setString(arr, "hello")
      local s:str = array.getString(arr, 0, 5)
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

static bool test_lib_array_fill(kt::Log &Log)
{
   LuaStateHolder Holder;
   lua_State *L = Holder.get();
   if (not L) {
      Log.error("failed to create Lua state");
      return false;
   }
   luaL_openlibs(L);

   // Test array.fill, including its half-open positional span.
   const char* code = R"(
      local arr:array<int> = array.new(10, "int")
      array.fill(arr, 0)
      array.fill(arr, 42, 3, 7)
      local ok = true
      for i in {0 to 10} do
         local expected = (i >= 3 and i < 7) ? 42 :> 0
         if arr[i] != expected then ok = false end
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

static bool test_lib_array_len_operator(kt::Log &Log)
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
      local arr:array<double> = array.new(42, "double")
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

static bool test_lib_array_double_type(kt::Log &Log)
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
      local arr:array<double> = array.new(5, "double")
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
   constexpr std::array<TestCase, 39> Tests = { {
      // Core Data Structures
      { "array_creation_byte", test_array_creation_byte },
      { "array_creation_int32", test_array_creation_int32 },
      { "array_recursive_identity", test_array_recursive_identity },
      { "unsigned_array_types", test_unsigned_array_types },
      { "struct_pointer_array_sentinel", test_struct_pointer_array_sentinel },
      { "struct_to_table_string_vector", test_struct_to_table_string_vector },
      { "array_creation_double", test_array_creation_double },
      { "array_index_access", test_array_index_access },
      { "array_elemsize", test_array_elemsize },
      { "array_external", test_array_external },
      { "array_external_unmanaged", test_array_external_unmanaged },
      { "array_external_resource", test_array_external_resource },
      { "array_element_contract", test_array_element_contract },
      { "array_runtime_search", test_array_runtime_search },
      { "array_to_table", test_array_to_table },
      { "array_type_tag", test_array_type_tag },
      // VM Type System
      { "tvalue_array", test_tvalue_array },
      { "typename_array", test_typename_array },
      { "setarrayV", test_setarrayV },
      // Bytecode C Helpers
      { "arr_getidx_int32", test_arr_getidx_int32 },
      { "arr_getidx_double", test_arr_getidx_double },
      { "array_load_allocation_classification", test_array_load_allocation_classification },
      { "arr_getidx_noalloc", test_arr_getidx_noalloc },
      { "arr_setidx_int32", test_arr_setidx_int32 },
      { "arr_setidx_double", test_arr_setidx_double },
      { "arr_roundtrip", test_arr_roundtrip },
      { "arr_byte_type", test_arr_byte_type },
      { "any_array_nil_initialisation", test_any_array_nil_initialisation },
      { "any_array_grow_nil_initialisation", test_any_array_grow_nil_initialisation },
      { "any_array_bulk_copy_overlap", test_any_array_bulk_copy_overlap },
      { "any_array_to_table_mixed_values", test_any_array_to_table_mixed_values },
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
      kt::Log Log("ArrayTests");
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

#endif // UNIT_TESTS
