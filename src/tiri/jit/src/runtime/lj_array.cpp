// Native array handling.
// Copyright © 2025-2026 Paul Manias

#define lj_array_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_array.h"
#include "lj_bulk.h"
#include "lj_object.h"
#include "lj_str.h"
#include "lj_tab.h"

#include <cstring>
#include <cfloat>
#include <cmath>
#include <limits>
#include <kotuku/main.h>
#include <kotuku/modules/tiri.h>
#include <kotuku/strings.hpp>
#include "../../defs.h"
#include "../../struct_def.h"

// Element sizes for each type (must match AET enum order)

static const uint8_t glElemSizes[] = {
   sizeof(uint8_t),      // AET::BYTE
   sizeof(int16_t),      // AET::INT16
   sizeof(int32_t),      // AET::INT32
   sizeof(int64_t),      // AET::INT64
   sizeof(float),        // AET::FLOAT
   sizeof(double),       // AET::DOUBLE
   sizeof(void*),        // AET::PTR
   sizeof(const char*),  // AET::CSTR
   sizeof(std::string),  // AET::STR_CPP
   sizeof(GCRef),        // AET::STR_GC
   sizeof(GCRef),        // AET::TABLE
   sizeof(GCRef),        // AET::ARRAY
   sizeof(TValue),       // AET::ANY
   0,                    // AET::STRUCT (variable)
   sizeof(GCRef)         // AET::OBJECT
};

//********************************************************************************************************************

uint8_t lj_array_elemsize(AET Type)
{
   lj_assertX(int(Type) >= 0 and int(Type) < int(AET::MAX), "invalid array element type");
   return glElemSizes[int(Type)];
}

//********************************************************************************************************************

static bool array_is_gc_ref_type(AET Type)
{
   return Type IS AET::STR_GC or Type IS AET::TABLE or Type IS AET::ARRAY or Type IS AET::OBJECT;
}

//********************************************************************************************************************

static uint64_t array_unsigned_integer(lua_Number Value, unsigned Bits);

static LJ_AINLINE ArrayElementResult array_prepare_int32(cTValue *Value, uint32_t *Bits)
{
   uint32_t value_type = itype(Value);
   if (value_type IS LJ_TNIL) {
      *Bits = 0;
      return ArrayElementResult::OK;
   }
   if (LJ_DUALNUM and value_type IS LJ_TISNUM) {
      *Bits = uint32_t(intV(Value));
      return ArrayElementResult::OK;
   }
   if (value_type > LJ_TISNUM) return ArrayElementResult::INVALID_TYPE;

   lua_Number number = numV(Value);
   if (not std::isfinite(number)) return ArrayElementResult::OUT_OF_RANGE;
   if (number >= std::numeric_limits<int32_t>::min() and number <= std::numeric_limits<int32_t>::max()) {
      int32_t signed_value = int32_t(number);
      std::memcpy(Bits, &signed_value, sizeof(signed_value));
   }
   else *Bits = uint32_t(array_unsigned_integer(number, 32));
   return ArrayElementResult::OK;
}

//********************************************************************************************************************

static LJ_AINLINE ArrayElementResult array_validate_integer(cTValue *Value)
{
   if (tvisnil(Value)) return ArrayElementResult::OK;
   if (not tvisnumber(Value)) return ArrayElementResult::INVALID_TYPE;
   if (tvisnum(Value) and not std::isfinite(numV(Value))) return ArrayElementResult::OUT_OF_RANGE;
   return ArrayElementResult::OK;
}

//********************************************************************************************************************

static LJ_AINLINE ArrayElementResult array_validate_element(GCarray *Array, cTValue *Value)
{
   if (Array->elemtype IS AET::ANY) return ArrayElementResult::OK;

   if (tvisnil(Value)) {
      switch (Array->elemtype) {
         case AET::BYTE:
         case AET::INT16:
         case AET::INT32:
         case AET::INT64:
         case AET::FLOAT:
         case AET::DOUBLE:
         case AET::STR_GC:
         case AET::TABLE:
         case AET::ARRAY:
         case AET::OBJECT:
            return ArrayElementResult::OK;
         case AET::CSTR:
         case AET::STR_CPP:
            return ArrayElementResult::UNSUPPORTED_STORAGE;
         default:
            return ArrayElementResult::INVALID_TYPE;
      }
   }

   switch (Array->elemtype) {
      case AET::BYTE:
      case AET::INT16:
      case AET::INT64:
         return array_validate_integer(Value);
      case AET::INT32: {
         uint32_t bits;
         return array_prepare_int32(Value, &bits);
      }

      case AET::FLOAT:
         if (not tvisnumber(Value)) return ArrayElementResult::INVALID_TYPE;
         if (tvisnum(Value) and std::isfinite(numV(Value)) and std::abs(numV(Value)) > FLT_MAX) {
            return ArrayElementResult::OUT_OF_RANGE;
         }
         return ArrayElementResult::OK;

      case AET::DOUBLE:
         return tvisnumber(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::STR_GC:
         return tvisstr(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::TABLE:
         return tvistab(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::ARRAY:
         return tvisarray(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::OBJECT:
         return tvisobject(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::PTR:
         return tvislightud(Value) ? ArrayElementResult::OK : ArrayElementResult::INVALID_TYPE;
      case AET::STRUCT:
         if (tvisstruct(Value)) {
            auto source = structV(Value);
            if (source->def IS Array->structdef and source->structsize IS Array->elemsize) {
               return ArrayElementResult::OK;
            }
         }
         return ArrayElementResult::INVALID_TYPE;
      case AET::CSTR:
      case AET::STR_CPP:
         return ArrayElementResult::UNSUPPORTED_STORAGE;
      default:
         return ArrayElementResult::UNSUPPORTED_STORAGE;
   }
}

//********************************************************************************************************************

ArrayElementResult lj_array_validate_element(GCarray *Array, cTValue *Value)
{
   return array_validate_element(Array, Value);
}

//********************************************************************************************************************

void lj_array_check_element(lua_State *L, GCarray *Array, cTValue *Value)
{
   auto result = array_validate_element(Array, Value);
   if (result IS ArrayElementResult::OK) return;
   if (result IS ArrayElementResult::OUT_OF_RANGE) lj_err_msg(L, ErrMsg::NUMRNG);
   lj_err_msg(L, ErrMsg::ARRTYPE);
}

//********************************************************************************************************************

static uint64_t array_unsigned_integer(lua_Number Value, unsigned Bits)
{
   double modulus = std::ldexp(1.0, int(Bits));
   double reduced = std::fmod(std::trunc(Value), modulus);
   if (Bits IS 64) {
      double signed_limit = std::ldexp(1.0, 63);
      if (reduced >= signed_limit) reduced -= modulus;
      else if (reduced < -signed_limit) reduced += modulus;
      return uint64_t(int64_t(reduced));
   }
   if (reduced < 0) reduced += modulus;
   return uint64_t(reduced);
}

//********************************************************************************************************************

static LJ_AINLINE void array_store_int32(void *Element, cTValue *Value)
{
   uint32_t bits = 0;
   if (array_prepare_int32(Value, &bits) != ArrayElementResult::OK) {
      lj_assertX(false, "invalid prepared int32 array value");
      return;
   }
   std::memcpy(Element, &bits, sizeof(bits));
}

//********************************************************************************************************************

static LJ_AINLINE void array_store_validated(lua_State *L, GCarray *Array, MSize Index, cTValue *Value)
{
   void *element = lj_array_index(Array, Index);

   if (tvisnil(Value)) {
      if (Array->elemtype IS AET::ANY) setnilV((TValue *)element);
      else std::memset(element, 0, Array->elemsize);
      return;
   }

   switch (Array->elemtype) {
      case AET::BYTE:
         if (tvisint(Value)) *(uint8_t *)element = uint8_t(intV(Value));
         else if (numV(Value) >= 0 and numV(Value) <= std::numeric_limits<uint8_t>::max()) {
            *(uint8_t *)element = uint8_t(numV(Value));
         }
         else *(uint8_t *)element = uint8_t(array_unsigned_integer(numV(Value), 8));
         return;
      case AET::INT16: {
         if (tvisint(Value) and intV(Value) >= std::numeric_limits<int16_t>::min() and
               intV(Value) <= std::numeric_limits<int16_t>::max()) {
            *(int16_t *)element = int16_t(intV(Value));
            return;
         }
         if (tvisnum(Value) and numV(Value) >= std::numeric_limits<int16_t>::min() and
               numV(Value) <= std::numeric_limits<int16_t>::max()) {
            *(int16_t *)element = int16_t(numV(Value));
            return;
         }
         uint16_t bits = tvisint(Value) ? uint16_t(intV(Value)) : uint16_t(array_unsigned_integer(numV(Value), 16));
         std::memcpy(element, &bits, sizeof(bits));
         return;
      }
      case AET::INT32: {
         array_store_int32(element, Value);
         return;
      }
      case AET::INT64: {
         double signed_limit = std::ldexp(1.0, 63);
         if (tvisnum(Value) and numV(Value) >= -signed_limit and numV(Value) < signed_limit) {
            *(int64_t *)element = int64_t(numV(Value));
            return;
         }
         uint64_t bits = tvisint(Value) ? uint64_t(int64_t(intV(Value))) : array_unsigned_integer(numV(Value), 64);
         std::memcpy(element, &bits, sizeof(bits));
         return;
      }
      case AET::FLOAT:
         *(float *)element = tvisint(Value) ? float(intV(Value)) : float(numV(Value));
         return;
      case AET::DOUBLE:
         *(double *)element = tvisint(Value) ? double(intV(Value)) : numV(Value);
         return;
      case AET::STR_GC:
      case AET::TABLE:
      case AET::ARRAY:
      case AET::OBJECT: {
         auto object = gcV(Value);
         setgcref(*(GCRef *)element, object);
         lj_gc_objbarrier(L, Array, object);
         return;
      }
      case AET::ANY:
         copyTV(L, (TValue *)element, Value);
         if (tvisgcv(Value)) lj_gc_objbarrier(L, Array, gcV(Value));
         return;
      case AET::PTR:
         *(void **)element = (void *)(Value->u64 & LJ_GCVMASK);
         return;
      case AET::STRUCT:
         std::memcpy(element, structV(Value)->data, Array->elemsize);
         return;
      default:
         lj_assertL(false, "unsupported validated array store");
         return;
   }
}

//********************************************************************************************************************

void lj_array_store_validated(lua_State *L, GCarray *Array, MSize Index, cTValue *Value)
{
   array_store_validated(L, Array, Index, Value);
}

//********************************************************************************************************************

void lj_array_store_checked(lua_State *L, GCarray *Array, MSize Index, cTValue *Value)
{
   bool exact_integer = tvisint(Value) and glArrayConversion[size_t(Array->elemtype)].primitive;
   if (not exact_integer) {
      auto result = array_validate_element(Array, Value);
      if (result IS ArrayElementResult::OUT_OF_RANGE) lj_err_msg(L, ErrMsg::NUMRNG);
      if (result != ArrayElementResult::OK) lj_err_msg(L, ErrMsg::ARRTYPE);
   }
   array_store_validated(L, Array, Index, Value);
}

//********************************************************************************************************************

template<typename T>
static bool array_store_integer_sequence(GCarray *Array, cTValue *Values, MSize Count)
{
   for (MSize index = 0; index < Count; index++) {
      if (not tvisint(Values + index)) return false;
      Array->get<T>()[index] = T(intV(Values + index));
   }
   return true;
}

//********************************************************************************************************************

// Populate a new, unexposed array.  If the integer fast path encounters another value type, the checked fallback
// validates the complete sequence before overwriting any values already stored by the fast path.

void lj_array_store_new_values(lua_State *L, GCarray *Array, cTValue *Values, MSize Count)
{
   if (glArrayConversion[size_t(Array->elemtype)].primitive) {
      bool stored = false;
      switch (Array->elemtype) {
         case AET::BYTE:   stored = array_store_integer_sequence<uint8_t>(Array, Values, Count); break;
         case AET::INT16:  stored = array_store_integer_sequence<int16_t>(Array, Values, Count); break;
         case AET::INT32:  stored = array_store_integer_sequence<int32_t>(Array, Values, Count); break;
         case AET::INT64:  stored = array_store_integer_sequence<int64_t>(Array, Values, Count); break;
         case AET::FLOAT:  stored = array_store_integer_sequence<float>(Array, Values, Count); break;
         case AET::DOUBLE: stored = array_store_integer_sequence<double>(Array, Values, Count); break;
         default: break;
      }
      if (stored) return;
   }

   for (MSize index = 0; index < Count; index++) lj_array_check_element(L, Array, Values + index);
   for (MSize index = 0; index < Count; index++) array_store_validated(L, Array, index, Values + index);
}

//********************************************************************************************************************
// Append a single value to an array.  Called from JIT traces when recording array.push(), mirroring the semantics
// of array.push() for one value.  Byte arrays accept strings, appending their bytes verbatim.

extern "C" void lj_arr_push1(lua_State *L, GCarray *Array, cTValue *Value)
{
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);

   if (Array->elemtype IS AET::BYTE and tvisstr(Value)) {
      GCstr *string = strV(Value);
      if (string->len > (~MSize(0) - Array->len)) lj_err_msg(L, ErrMsg::ARREXT);
      MSize new_len = Array->len + string->len;
      if (new_len > Array->capacity and not lj_array_grow(L, Array, new_len)) lj_err_msg(L, ErrMsg::ARREXT);
      if (string->len > 0) memcpy((uint8_t *)Array->arraydata() + Array->len, strdata(string), string->len);
      Array->len = new_len;
      return;
   }

   MSize index = Array->len;
   if (index IS ~MSize(0)) lj_err_msg(L, ErrMsg::ARREXT);

   if (Array->elemtype IS AET::INT32) {
      uint32_t bits;
      auto result = array_prepare_int32(Value, &bits);
      if (result IS ArrayElementResult::OUT_OF_RANGE) lj_err_msg(L, ErrMsg::NUMRNG);
      if (result != ArrayElementResult::OK) lj_err_msg(L, ErrMsg::ARRTYPE);
      if (index + 1 > Array->capacity and not lj_array_grow(L, Array, index + 1)) lj_err_msg(L, ErrMsg::ARREXT);
      std::memcpy(lj_array_index(Array, index), &bits, sizeof(bits));
      Array->len = index + 1;
      return;
   }

   bool exact_integer = tvisint(Value) and glArrayConversion[size_t(Array->elemtype)].primitive;
   if (not exact_integer) {
      auto result = array_validate_element(Array, Value);
      if (result IS ArrayElementResult::OUT_OF_RANGE) lj_err_msg(L, ErrMsg::NUMRNG);
      if (result != ArrayElementResult::OK) lj_err_msg(L, ErrMsg::ARRTYPE);
   }

   if (index + 1 > Array->capacity and not lj_array_grow(L, Array, index + 1)) lj_err_msg(L, ErrMsg::ARREXT);
   array_store_validated(L, Array, index, Value);
   Array->len = index + 1;
}

//********************************************************************************************************************

static void array_attach_basemt(lua_State *L, GCarray *Arr)
{
   GCRef mt_ref = basemt_it(G(L), LJ_TARRAY);
   if (gcref(mt_ref)) {
      GCtab *mt = tabref(mt_ref);
      setgcref(Arr->metatable, obj2gco(mt));
      lj_gc_objbarrier(L, Arr, mt);
   }
}

//********************************************************************************************************************
// Create a new array structure without placing it on the Lua stack (use lua_createarray otherwise).  Throws on error.
//
// For string arrays (CSTRING/STRING_CPP) with caching:
// - Data points to an array of CSTRING or std::string pointers
// - String content is copied into a vector<char> owned by the array
// - The storage area stores CSTRING pointers into the vector

extern GCarray * lj_array_new(lua_State *L, uint32_t Length, AET Type, void *Data, uint8_t Flags,
   std::string_view StructName, struct_record *StructDef)
{
   MSize elem_size;
   struct_record *sdef = nullptr;

   if (not StructName.empty()) {
      // Struct-backed array
      if (auto def = StructDef ? StructDef : find_struct(L, StructName)) {
         sdef = def;
         elem_size = sdef->Size;
      }
      else {
         lj_err_callerv(L, ErrMsg::NOSTRUCT, "%.*s", StructName.size(), StructName.data());
         return nullptr;
      }
   }
   else elem_size = lj_array_elemsize(Type);

   lj_assertL(elem_size > 0, "invalid element size for array creation");

   if (Data) {
      if (Flags & ARRAY_EXTERNAL) {
         // External data - caller manages lifetime (no storage allocation needed)
         // External arrays have capacity = length and cannot grow
         auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
         arr->init(Data, Type, elem_size, Length, Length, Flags, sdef);
         array_attach_basemt(L, arr);
         return arr;
      }
      else {
         // Cached data - copy into owned storage
         if (Type IS AET::CSTR or Type IS AET::STR_CPP) {
            // String caching: store CSTRING pointers that point into strcache
            size_t byte_size = Length * sizeof(CSTRING);
            void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
            auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
            arr->init(storage, AET::CSTR, sizeof(CSTRING), Length, Length, 0, sdef);
            array_attach_basemt(L, arr);

            // Calculate total string content size
            size_t content_size = 0;
            if (Type IS AET::CSTR) {
               auto strings = (CSTRING *)Data;
               for (uint32_t i = 0; i < Length; i++) {
                  if (strings[i]) content_size += strlen(strings[i]) + 1;
                  else content_size += 1; // For null string, store empty string
               }
            }
            else { // AET::STR_CPP
               kt::vector<std::string> &strings = ((kt::vector<std::string> *)Data)[0];
               for (uint32_t i = 0; i < Length; i++) {
                  content_size += strings[i].size() + 1;
               }
            }

            // Allocate and populate the string cache
            arr->strcache = new std::vector<char>(content_size);
            auto cache_ptr = arr->strcache->data();
            auto ptr_array = (CSTRING *)arr->arraydata();

            if (Type IS AET::CSTR) {
               auto strings = (CSTRING *)Data;
               for (uint32_t i = 0; i < Length; i++) {
                  ptr_array[i] = cache_ptr;
                  if (strings[i]) {
                     size_t slen = strlen(strings[i]);
                     std::memcpy(cache_ptr, strings[i], slen + 1);
                     cache_ptr += slen + 1;
                  }
                  else {
                     *cache_ptr++ = '\0';
                  }
               }
            }
            else { // AET::STR_CPP
               kt::vector<std::string> &strings = ((kt::vector<std::string> *)Data)[0];
               for (uint32_t i = 0; i < Length; i++) {
                  ptr_array[i] = cache_ptr;
                  std::memcpy(cache_ptr, strings[i].c_str(), strings[i].size() + 1);
                  cache_ptr += strings[i].size() + 1;
               }
            }

            return arr;
         }
         else if ((Type IS AET::TABLE) or (Type IS AET::ARRAY)) {
            // Table arrays not supported for caching (not used by the Kōtuku API)
            lj_err_callerv(L, ErrMsg::BADVAL);
            return nullptr;
         }
         else if (Type IS AET::OBJECT) {
            // Object arrays returned by the Kōtuku API contain native OBJECTPTR values.  Tiri object arrays store
            // GCobject references, so wrap each native object before exposing the array to the script.
            size_t byte_size = Length * elem_size;
            void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
            auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
            arr->init(storage, Type, elem_size, Length, Length, Flags, sdef);
            array_attach_basemt(L, arr);

            auto refs = arr->get<GCRef>();
            auto objects = (OBJECTPTR *)Data;
            for (uint32_t i=0; i < Length; i++) {
               if (objects[i]) {
                  auto obj = lj_object_new(L, objects[i]->UID, nullptr, objects[i]->Class, GCOBJ_DETACHED);
                  lj_object_pin(obj, objects[i]);
                  setgcref(refs[i], obj2gco(obj));
                  lj_gc_objbarrier(L, arr, obj);
               }
               else setgcrefnull(refs[i]);
            }
            return arr;
         }
         else {
            // Non-string cached array - allocate storage via GC, then copy data
            // Capacity equals length for cached arrays
            size_t byte_size = Length * elem_size;
            void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
            auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
            arr->init(storage, Type, elem_size, Length, Length, Flags, sdef);
            array_attach_basemt(L, arr);
            if (byte_size > 0) {
               if (Type IS AET::ANY) lj_bulk_copy_tvalue((TValue *)storage, (const TValue *)Data, Length);
               else std::memcpy(storage, Data, byte_size);
            }
            return arr;
         }
      }
   }
   else {
      // New empty array with owned storage allocated via GC
      // Capacity equals length for newly created arrays
      size_t byte_size = Length * elem_size;
      void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
      auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
      arr->init(storage, Type, elem_size, Length, Length, Flags & ~(ARRAY_EXTERNAL|ARRAY_CACHED), sdef);
      array_attach_basemt(L, arr);
      if (storage) {
         if (Type IS AET::ANY) {
            // _ANY arrays require explicit nil initialization (nil TValue = -1, not 0)
            lj_bulk_nil_tvalue((TValue *)storage, Length);
         }
         else std::memset(storage, 0, byte_size);
      }
      return arr;
   }
}

//********************************************************************************************************************
// Grow array capacity to accommodate at least MinCapacity elements.
// Uses a growth factor of 1.5x or the minimum required capacity, whichever is larger.
// Returns true on success, false if the array cannot grow (external/cached string arrays).

bool lj_array_grow(lua_State *L, GCarray *Array, MSize MinCapacity)
{
   // Cannot grow external arrays - they don't own their storage
   if (Array->flags & ARRAY_EXTERNAL) return false;

   // Cannot grow cached string arrays - strcache pointers would be invalidated
   if (Array->strcache) return false;

   // Already have enough capacity
   if (Array->capacity >= MinCapacity) return true;

   // Calculate new capacity using 1.5x growth factor
   MSize new_capacity = Array->capacity + (Array->capacity >> 1);  // capacity * 1.5
   if (new_capacity < MinCapacity) new_capacity = MinCapacity;
   if (new_capacity < 8) new_capacity = 8;  // Minimum allocation

   size_t old_size = size_t(Array->capacity) * Array->elemsize;
   size_t new_size = size_t(new_capacity) * Array->elemsize;

   // Reallocate storage
   void *new_storage = lj_mem_realloc(L, Array->storage, old_size, new_size);

   // Initialise new elements for vulnerable types (pointers, strings, tables)
   if (Array->elemtype IS AET::ANY) {
      lj_bulk_nil_tvalue((TValue *)((char *)new_storage + old_size), new_capacity - Array->capacity);
   }
   else if (int(Array->elemtype) >= int(AET::VULNERABLE)) {
      size_t zerolen = new_size - old_size;
      std::memset((char *)new_storage + old_size, 0, zerolen);
   }

   Array->storage = new_storage;
   Array->capacity = new_capacity;
   return true;
}

//********************************************************************************************************************

void lj_array_free(global_State *g, GCarray *Array)
{
   // Free owned storage first (external storage is managed by caller)
   size_t storage_size = Array->storage_size();
   if (storage_size > 0) {
      lj_mem_free(g, Array->storage, storage_size);
   }
   Array->~GCarray(); // Call destructor (handles strcache cleanup)
   lj_mem_free(g, Array, sizeof(GCarray));
}

//********************************************************************************************************************

void lj_array_clear_range(GCarray *Array, MSize Start, MSize Count)
{
   if (Count IS 0) return;

   if (array_is_gc_ref_type(Array->elemtype)) {
      auto refs = &Array->get<GCRef>()[Start];
      for (MSize i = 0; i < Count; i++) setgcrefnull(refs[i]);
   }
   else if (Array->elemtype IS AET::ANY) {
      lj_bulk_nil_tvalue(&Array->get<TValue>()[Start], Count);
   }
}

//********************************************************************************************************************

void lj_array_copy(lua_State *L, GCarray *Dest, uint32_t DstIdx, GCarray *Src, uint32_t SrcIdx, uint32_t Count)
{
   if (SrcIdx > Src->len or Count > Src->len - SrcIdx or DstIdx > Dest->len or Count > Dest->len - DstIdx) {
      lj_err_caller(L, ErrMsg::IDXRNG);
   }
   if (Dest->is_readonly()) lj_err_caller(L, ErrMsg::ARRRO);
   if (not (Dest->elemtype IS Src->elemtype)) lj_err_caller(L, ErrMsg::ARRTYPE);
   if (Count IS 0) return;

   void *dst_ptr = lj_array_index(Dest, DstIdx);
   void *src_ptr = lj_array_index(Src, SrcIdx);
   if (Dest->elemtype IS AET::ANY) {
      lj_bulk_move_tvalue((TValue *)dst_ptr, (const TValue *)src_ptr, Count);
      auto dst_slots = (TValue *)dst_ptr;
      for (MSize i = 0; i < Count; i++) {
         if (tvisgcv(&dst_slots[i])) lj_gc_objbarrier(L, Dest, gcV(&dst_slots[i]));
      }
   }
   else if (array_is_gc_ref_type(Dest->elemtype)) {
      size_t byte_count = Count * Dest->elemsize;
      memmove(dst_ptr, src_ptr, byte_count);

      auto refs = (GCRef *)dst_ptr;
      for (MSize i = 0; i < Count; i++) {
         if (gcref(refs[i])) lj_gc_objbarrier(L, Dest, gcref(refs[i]));
      }
   }
   else {
      size_t byte_count = Count * Dest->elemsize;
      memmove(dst_ptr, src_ptr, byte_count); // Use memmove to handle overlapping regions
   }
}

//********************************************************************************************************************

GCtab * lj_array_to_table(lua_State *L, GCarray *Array)
{
   GCtab *t = lj_tab_new(L, Array->len, 0);  // 0-based: indices 0..len-1
   auto array_part = tvref(t->array);

   if (Array->elemtype IS AET::ANY) {
      lj_bulk_copy_tvalue(array_part, (const TValue *)Array->arraydata(), Array->len);
      return t;
   }

   auto data = (uint8_t *)Array->arraydata();
   for (MSize i = 0; i < Array->len; i++) {
      auto slot = &array_part[i];
      void *elem = data + (i * Array->elemsize);

      switch (Array->elemtype) {
         case AET::BYTE:   setintV(slot, *(uint8_t*)elem); break;
         case AET::INT16:  setintV(slot, *(int16_t*)elem); break;
         case AET::INT32:  setintV(slot, *(int32_t*)elem); break;
         case AET::INT64:  setnumV(slot, lua_Number(*(int64_t*)elem)); break;
         case AET::FLOAT:  setnumV(slot, *(float*)elem); break;
         case AET::DOUBLE: setnumV(slot, *(double*)elem); break;
         case AET::TABLE: {
            GCRef ref = *(GCRef*)elem;
            if (gcref(ref)) settabV(L, slot, gco_to_table(gcref(ref)));
            else setnilV(slot);
            break;
         }
         case AET::ARRAY: {
            GCRef ref = *(GCRef*)elem;
            if (gcref(ref)) setarrayV(L, slot, gco_to_array(gcref(ref)));
            else setnilV(slot);
            break;
         }
         case AET::OBJECT: {
            GCRef ref = *(GCRef*)elem;
            if (gcref(ref)) setobjectV(L, slot, gco_to_object(gcref(ref)));
            else setnilV(slot);
            break;
         }
         case AET::ANY: {
            TValue *src = (TValue*)elem;
            copyTV(L, slot, src);
            break;
         }
         default: setnilV(slot); break;
      }
   }

   return t;
}
