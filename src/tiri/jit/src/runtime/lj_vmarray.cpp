// Array helper functions for assembler VM.
// Copyright © 2025-2026 Paul Manias

#define lj_vmarray_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_array.h"
#include "lj_str.h"
#include "lj_strfmt.h"
#include "lj_buf.h"
#include "lj_vmarray.h"
#include "lj_vm.h"
#include "lj_frame.h"

#include <cstring>
#include <string>

//********************************************************************************************************************
// Helper to convert TValue index to integer, returns -1 if invalid

static int32_t arr_idx_from_tv(cTValue *K)
{
   if (tvisint(K)) return intV(K);
   else if (tvisnum(K)) {
      lua_Number n = numV(K);
      int32_t i = lj_num2int(n);
      if ((lua_Number)i IS n) return i;
   }
   return -1;  // Invalid index
}

//********************************************************************************************************************
// Helper to retrieve array element into TValue based on element type

static void arr_load_elem(lua_State *L, GCarray *Array, uint32_t Idx, TValue *Result)
{
   void *elem = lj_array_index(Array, Idx);

   switch (Array->elemtype) {
      case AET::BYTE:   setintV(Result, *(uint8_t*)elem); break;
      case AET::INT16:  setintV(Result, *(int16_t*)elem); break;
      case AET::INT32:  setintV(Result, *(int32_t*)elem); break;
      case AET::INT64:  setnumV(Result, lua_Number(*(int64_t*)elem)); break;
      case AET::FLOAT:  setnumV(Result, *(float*)elem); break;
      case AET::DOUBLE: setnumV(Result, *(double*)elem); break;

      case AET::CSTR: {
         if (auto str = *(CSTRING *)elem) setstrV(L, Result, lj_str_newz(L, str));
         else setnilV(Result);
         break;
      }

      case AET::STR_CPP: {
         auto str = (std::string*)elem;
         if (str->empty()) setnilV(Result);
         else setstrV(L, Result, lj_str_new(L, str->data(), str->size()));
         break;
      }

      case AET::PTR:
         // Store raw pointer value as light userdata
         setrawlightudV(Result, *(void**)elem);
         break;

      case AET::STR_GC: {
         GCRef ref = *(GCRef*)elem;
         if (gcref(ref)) setstrV(L, Result, gco_to_string(gcref(ref)));
         else setnilV(Result);
         break;
      }

      case AET::TABLE: {
         GCRef ref = *(GCRef*)elem;
         if (gcref(ref)) settabV(L, Result, gco_to_table(gcref(ref)));
         else setnilV(Result);
         break;
      }

      case AET::ARRAY: {
         GCRef ref = *(GCRef*)elem;
         if (gcref(ref)) setarrayV(L, Result, gco_to_array(gcref(ref)));
         else setnilV(Result);
         break;
      }

      case AET::OBJECT: {
         GCRef ref = *(GCRef*)elem;
         if (gcref(ref)) setobjectV(L, Result, gco_to_object(gcref(ref)));
         else setnilV(Result);
         break;
      }

      case AET::ANY: {
         TValue *source = (TValue*)elem;
         copyTV(L, Result, source);
         break;
      }

      default: setnilV(Result); break;
   }
}

//********************************************************************************************************************
// Helper to store TValue into array element based on element type

static void arr_store_elem(lua_State *L, GCarray *Array, uint32_t Idx, cTValue *Val)
{
   void *elem = lj_array_index(Array, Idx);

   // Handle non-numeric types first

   if (not glArrayConversion[uint8_t(Array->elemtype)].primitive) {
      switch (Array->elemtype) {
         case AET::STR_GC:
         case AET::TABLE:
         case AET::ARRAY:
         case AET::OBJECT: {
            if (tvisnil(Val)) setgcrefnull(*(GCRef*)elem);
            else {
               if (Array->itype IS uint8_t(itype(Val))) {
                  auto gcobj = gcV(Val);
                  setgcref(*(GCRef*)elem, gcobj);
                  lj_gc_objbarrier(L, Array, gcobj);
               }
               else lj_err_msgv(L, ErrMsg::ARRTYPE);
            }
            return;
         }

         case AET::ANY: {
            auto dest = (TValue *)elem;
            copyTV(L, dest, Val);
            if (tvisgcv(Val)) lj_gc_objbarrier(L, Array, gcV(Val));
            return;
         }

         case AET::PTR:
            if (tvislightud(Val)) { // Extract raw pointer (note: lightudV on 64-bit requires global_State)
               *(void**)elem = (void*)(Val->u64 & LJ_GCVMASK);
               return;
            }
            break;

         case AET::CSTR:
         case AET::STR_CPP:
            // Storing pointers to C strings is potentially feasible but currently unsafe; for this reason we disallow it.
         default: break;
      }

      // We could attempt automated conversion (e.g. string to int), but this would be unpredictable
      // between releases and the user can perform explicit conversion if desired.

      lj_err_msgv(L, ErrMsg::ARRTYPE);
   }
   else { // All primitive types are numeric
      if (tvisint(Val)) {
         int32_t ival = intV(Val);
         switch (Array->elemtype) {
            case AET::BYTE:   *(uint8_t*)elem = uint8_t(ival); return;
            case AET::INT16:  *(int16_t*)elem = int16_t(ival); return;
            case AET::INT32:  *(int32_t*)elem = ival; return;
            case AET::INT64:  *(int64_t*)elem = ival; return;
            case AET::FLOAT:  *(float*)elem = float(ival); return;
            case AET::DOUBLE: *(double*)elem = double(ival); return;
            default: break;
         }
      }
      else if (tvisnum(Val)) {
         lua_Number num = numV(Val);
         switch (Array->elemtype) {
            case AET::BYTE:   *(uint8_t*)elem = uint8_t(num); return;
            case AET::INT16:  *(int16_t*)elem = int16_t(num); return;
            case AET::INT32:  *(int32_t*)elem = int32_t(num); return;
            case AET::INT64:  *(int64_t*)elem = int64_t(num); return;
            case AET::FLOAT:  *(float*)elem = float(num); return;
            case AET::DOUBLE: *(double*)elem = num; return;
            default: break;
         }
      }
      else if (tvisnil(Val)) {
         memset(elem, 0, Array->elemsize);
         return;
      }
      lj_err_msgv(L, ErrMsg::ARRTYPE);
   }
}

//********************************************************************************************************************
// Helper for AGETV/AGETB. Array get with metamethod support.
// Returns pointer to result TValue, or nullptr to trigger metamethod call.

extern "C" cTValue * lj_arr_get(lua_State *L, cTValue *O, cTValue *K)
{
   if (not tvisarray(O)) { // Not an array - check for __index metamethod
      cTValue *mo = lj_meta_lookup(L, O, MM_index);
      if (tvisnil(mo)) {
         lj_err_optype(L, O, ErrMsg::OPINDEX);
         return nullptr;  // unreachable
      }

      // Would need to trigger metamethod (none implemented yet) - for now, error
      lj_err_optype(L, O, ErrMsg::OPINDEX);
      return nullptr;
   }

   GCarray *arr = arrayV(O);

   // Check if key is a string (method lookup like arr:concat())

   if (tvisstr(K)) {
      // Look up directly in array's metatable for methods (per-instance first, then base)
      GCtab *mt = tabref(arr->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TARRAY));

      if (mt) {
         cTValue *tv = lj_tab_get(L, mt, K);
         if (not tvisnil(tv)) return tv;  // Found method in metatable
      }
      // String key not recognised as a method - raise error
      lj_err_optype(L, O, ErrMsg::OPCALL);
      return nullptr;
   }

   // Convert index to integer (0-based internally)

   int32_t idx = arr_idx_from_tv(K);
   if ((idx < 0) or (idx >= int32_t(arr->len))) {
      // Check for __index metamethod on array's metatable (per-instance first, then base)
      GCtab *mt = tabref(arr->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TARRAY));

      if (mt and lj_meta_fast(L, mt, MM_index)) {
         // Metamethod exists - return nullptr to trigger it
         // The assembler VM will handle calling the metamethod
         return nullptr;
      }

      // No metamethod - raise error
      lj_err_msgv(L, ErrMsg::ARROB, idx, int(arr->len));
      return nullptr;  // unreachable
   }

   // Load element into a static result TValue
   // Note: This uses a thread-local or static buffer that the assembly caller is expected to copy

   static thread_local TValue result;
   arr_load_elem(L, arr, uint32_t(idx), &result);
   return &result;
}

//********************************************************************************************************************
// Helper for ASETV/ASETB. Array set with metamethod support.
// Performs the actual store. Returns 1 on success, 0 to trigger metamethod call.

extern "C" int lj_arr_set(lua_State *L, cTValue *O, cTValue *K, cTValue *V)
{
   if (not tvisarray(O)) {
      // Not an array - check for __newindex metamethod
      cTValue *mo = lj_meta_lookup(L, O, MM_newindex);
      if (tvisnil(mo)) {
         lj_err_optype(L, O, ErrMsg::OPINDEX);
         return 0;  // unreachable
      }
      // Would need to trigger metamethod - for now, error
      lj_err_optype(L, O, ErrMsg::OPINDEX);
      return 0;
   }

   GCarray *arr = arrayV(O);

   if (arr->flags & ARRAY_READONLY) {
      lj_err_msg(L, ErrMsg::ARRRO);
      return 0;  // unreachable
   }

   // Convert index to integer (0-based internally)
   int32_t idx = arr_idx_from_tv(K);
   if (idx < 0 or MSize(idx) >= arr->len) {
      // Check for __newindex metamethod on array's metatable (per-instance first, then base)
      GCtab *mt = tabref(arr->metatable);
      if (not mt) mt = tabref(basemt_it(G(L), LJ_TARRAY));

      if (mt) {
         cTValue *mo = lj_meta_fast(L, mt, MM_newindex);
         if (mo) return 0; // Metamethod exists - return 0 to trigger it
      }
      // No metamethod - raise error
      lj_err_msgv(L, ErrMsg::ARROB, idx, int(arr->len));
      return 0;  // unreachable
   }

   // Perform the actual store
   arr_store_elem(L, arr, uint32_t(idx), V);
   return 1;  // Success
}

//********************************************************************************************************************
// Direct array get by index - called after type and bounds checks pass

extern "C" void lj_arr_getidx(lua_State *L, GCarray *Array, int32_t Idx, TValue *Result)
{
   if (Idx < 0 or MSize(Idx) >= Array->len) lj_err_msgv(L, ErrMsg::ARROB, Idx, int(Array->len));
   arr_load_elem(L, Array, uint32_t(Idx), Result);
}

//********************************************************************************************************************
// Safe array get by index - returns nil for out-of-bounds instead of throwing an error.
// Used by safe navigation operator (?[]) on arrays.

extern "C" void lj_arr_safe_getidx(lua_State *L, GCarray *Array, int32_t Idx, TValue *Result)
{
   if (Idx < 0 or MSize(Idx) >= Array->len) {
      setnilV(Result);
      return;
   }
   arr_load_elem(L, Array, uint32_t(Idx), Result);
}

//********************************************************************************************************************
// Direct array set by index - called after type and bounds checks pass

extern "C" void lj_arr_setidx(lua_State *L, GCarray *Array, int32_t Idx, cTValue *Val)
{
   if (Idx < 0 or MSize(Idx) >= Array->len) lj_err_msgv(L, ErrMsg::ARROB, Idx, int(Array->len));
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);
   arr_store_elem(L, Array, uint32_t(Idx), Val);
}

//********************************************************************************************************************
// Append a single value to an array.  Called from JIT traces when recording array.append(), mirroring the semantics
// of array.push() for one value.  Byte arrays accept strings, appending their bytes verbatim.

extern "C" void lj_arr_push1(lua_State *L, GCarray *Array, cTValue *Val)
{
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);

   if (Array->elemtype IS AET::BYTE and tvisstr(Val)) {
      GCstr *str = strV(Val);
      if (str->len > (~MSize(0) - Array->len)) lj_err_msg(L, ErrMsg::ARREXT);
      MSize new_len = Array->len + str->len;
      if (new_len > Array->capacity and not lj_array_grow(L, Array, new_len)) lj_err_msg(L, ErrMsg::ARREXT);
      if (str->len > 0) memcpy((uint8_t*)Array->arraydata() + Array->len, strdata(str), str->len);
      Array->len = new_len;
      return;
   }

   MSize idx = Array->len;
   if (idx IS ~MSize(0)) lj_err_msg(L, ErrMsg::ARREXT);
   if (idx + 1 > Array->capacity and not lj_array_grow(L, Array, idx + 1)) lj_err_msg(L, ErrMsg::ARREXT);
   arr_store_elem(L, Array, idx, Val);
   Array->len = idx + 1;
}

//********************************************************************************************************************

static void lj_arr_append_bytes(lua_State *L, GCarray *Array, const char *Data, MSize Len)
{
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);
   if (not (Array->elemtype IS AET::BYTE)) lj_err_msg(L, ErrMsg::ARRSTR);
   if (Len > (~MSize(0) - Array->len)) lj_err_msg(L, ErrMsg::ARREXT);

   MSize new_len = Array->len + Len;
   if (new_len > Array->capacity and not lj_array_grow(L, Array, new_len)) lj_err_msg(L, ErrMsg::ARREXT);
   if (Len > 0) memcpy((uint8_t*)Array->arraydata() + Array->len, Data, Len);
   Array->len = new_len;
}

//********************************************************************************************************************
// Append string bytes to a byte array from a recorded trace.

extern "C" void lj_arr_putstr(lua_State *L, GCarray *Array, GCstr *Str)
{
   lj_arr_append_bytes(L, Array, strdata(Str), Str->len);
}

//********************************************************************************************************************
// Append the bytes currently held by a string buffer to a byte array from a recorded trace.

extern "C" void lj_arr_putsbuf(lua_State *L, GCarray *Array, SBuf *Buf)
{
   lj_arr_append_bytes(L, Array, Buf->b, sbuflen(Buf));
}

//********************************************************************************************************************
// Append a number TValue formatted with concat/tostring semantics to a byte array from a recorded trace.

extern "C" void lj_arr_putnumtv(lua_State *L, GCarray *Array, cTValue *Value)
{
   SBuf *sb;
   if (tvisint(Value)) sb = lj_strfmt_putint(lj_buf_tmp_(L), intV(Value));
   else if (tvisnum(Value)) sb = lj_strfmt_putfnum(lj_buf_tmp_(L), STRFMT_G14, numV(Value));
   else lj_err_msg(L, ErrMsg::BADVAL);
   lj_arr_append_bytes(L, Array, sb->b, sbuflen(sb));
}

//********************************************************************************************************************
// Clear an array from a recorded trace.

extern "C" void lj_arr_clear(lua_State *L, GCarray *Array)
{
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);

   lj_array_clear_range(Array, 0, Array->len);
   Array->len = 0;
}

//********************************************************************************************************************
// Resize an array from a recorded trace.

extern "C" int32_t lj_arr_resize(lua_State *L, GCarray *Array, int32_t NewSize)
{
   if (Array->flags & ARRAY_READONLY) lj_err_msg(L, ErrMsg::ARRRO);
   if (NewSize < 0) lj_err_msgv(L, ErrMsg::NUMRNG, "non-negative", "negative");

   MSize target_len = MSize(NewSize);
   MSize old_len = Array->len;

   if (target_len > old_len) {
      if (target_len > Array->capacity and not lj_array_grow(L, Array, target_len)) lj_err_msg(L, ErrMsg::ARREXT);

      if (Array->elemtype IS AET::STR_GC or Array->elemtype IS AET::TABLE or
          Array->elemtype IS AET::ARRAY or Array->elemtype IS AET::OBJECT or Array->elemtype IS AET::ANY) {
         lj_array_clear_range(Array, old_len, target_len - old_len);
      }
      else {
         void *start = (char*)Array->arraydata() + (old_len * Array->elemsize);
         size_t bytes = (target_len - old_len) * Array->elemsize;
         memset(start, 0, bytes);
      }
   }
   else if (target_len < old_len) {
      lj_array_clear_range(Array, target_len, old_len - target_len);
   }

   Array->len = target_len;
   return int32_t(Array->len);
}

//********************************************************************************************************************
// Extract a string from a byte array in a recorded trace.  Len -1 means "remaining from Start".

extern "C" GCstr * lj_arr_getstring(lua_State *L, GCarray *Array, int32_t Start, int32_t Len)
{
   if (not (Array->elemtype IS AET::BYTE)) lj_err_msg(L, ErrMsg::ARRSTR);

   int32_t count = Len;
   if (Len IS -1) {
      if (Start < 0 or MSize(Start) > Array->len) count = 0;
      else count = int32_t(Array->len - MSize(Start));
   }

   if (Start < 0 or count < 0 or MSize(Start) > Array->len) lj_err_msg(L, ErrMsg::IDXRNG);

   MSize start = MSize(Start);
   MSize byte_count = MSize(count);
   if (byte_count > Array->len - start) lj_err_msg(L, ErrMsg::IDXRNG);

   CSTRING data = byte_count > 0 ? Array->get<const char>() + start : "";
   return lj_str_new(L, data, byte_count);
}

//********************************************************************************************************************
// Create an array from a recorded trace.  The recorder resolves and validates ElemType from a constant type literal.

extern "C" GCarray * lj_arr_new_jit(lua_State *L, uint32_t Length, uint32_t ElemType)
{
   return lj_array_new(L, Length, AET(ElemType));
}
