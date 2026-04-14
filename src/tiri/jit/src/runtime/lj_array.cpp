// Native array handling.
// Copyright © 2025-2026 Paul Manias

#define lj_array_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_array.h"
#include "lj_tab.h"

#include <cstring>
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
// Create a new array structure without placing it on the Lua stack (use lua_createarray otherwise).  Throws on error.
//
// For string arrays (CSTRING/STRING_CPP) with caching:
// - Data points to an array of CSTRING or std::string pointers
// - String content is copied into a vector<char> owned by the array
// - The storage area stores CSTRING pointers into the vector

extern GCarray * lj_array_new(lua_State *L, uint32_t Length, AET Type, void *Data, uint8_t Flags, std::string_view StructName)
{
   MSize elem_size;
   struct_record *sdef = nullptr;

   if (not StructName.empty()) {
      // Struct-backed array
      auto name = struct_name(StructName);
      if (glStructs.contains(name)) {
         sdef = &glStructs[name];
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
         return arr;
      }
      else {
         // Cached data - copy into owned storage
         if (Type IS AET::CSTR or Type IS AET::STR_CPP) {
            // String caching: store CSTRING pointers that point into strcache
            size_t byte_size = Length * sizeof(CSTRING);
            void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
            auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
            if (storage) arr->init(storage, AET::CSTR, sizeof(CSTRING), Length, Length, 0, sdef);

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
               pf::vector<std::string> &strings = ((pf::vector<std::string> *)Data)[0];
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
               pf::vector<std::string> &strings = ((pf::vector<std::string> *)Data)[0];
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
         else {
            // Non-string cached array - allocate storage via GC, then copy data
            // Capacity equals length for cached arrays
            size_t byte_size = Length * elem_size;
            void *storage = (byte_size > 0) ? lj_mem_new(L, byte_size) : nullptr;
            auto arr = (GCarray *)lj_mem_newgco(L, sizeof(GCarray));
            arr->init(storage, Type, elem_size, Length, Length, Flags, sdef);
            if (byte_size > 0) std::memcpy(storage, Data, byte_size);
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
      if (storage) {
         if (Type IS AET::ANY) {
            // _ANY arrays require explicit nil initialization (nil TValue = -1, not 0)
            TValue *slots = (TValue*)storage;
            for (MSize i = 0; i < Length; i++) setnilV(&slots[i]);
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

   // Zero-initialise new elements for vulnerable types (pointers, strings, tables)
   if (int(Array->elemtype) >= int(AET::VULNERABLE)) {
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

void lj_array_copy(lua_State *L, GCarray *Dest, uint32_t DstIdx, GCarray *Src, uint32_t SrcIdx, uint32_t Count)
{
   // Safety checks - unsigned types can't be negative so just check bounds
   if (SrcIdx + Count > Src->len or DstIdx + Count > Dest->len) lj_err_caller(L, ErrMsg::IDXRNG);
   if (Dest->is_readonly()) lj_err_caller(L, ErrMsg::ARRRO);
   if (Dest->elemtype != Src->elemtype) lj_err_caller(L, ErrMsg::ARRTYPE);

   void *dst_ptr = lj_array_index(Dest, DstIdx);
   void *src_ptr = lj_array_index(Src, SrcIdx);
   size_t byte_count = Count * Dest->elemsize;
   memmove(dst_ptr, src_ptr, byte_count); // Use memmove to handle overlapping regions
}

//********************************************************************************************************************

GCtab * lj_array_to_table(lua_State *L, GCarray *Array)
{
   GCtab *t = lj_tab_new(L, Array->len, 0);  // 0-based: indices 0..len-1
   auto array_part = tvref(t->array);

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
