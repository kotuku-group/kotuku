// Native array search kernels.
// Copyright © 2026 Paul Manias.

#ifndef LJ_ARRAY_SEARCH_H
#define LJ_ARRAY_SEARCH_H

#include "lj_obj.h"
#include "lj_array.h"
#include "lj_str.h"

#include <cstring>

#ifndef ARRAY_SEARCH_NOINLINE
#define ARRAY_SEARCH_NOINLINE LJ_NOINLINE
#endif

template<typename T>
static int32_t find_forward_contiguous(const void *Data, int32_t Start, int32_t Stop, lua_Number Value)
{
   const T *base = (const T *)Data;
   T value = T(Value);
   for (int32_t i = Start; i <= Stop; i++) {
      if (base[i] IS value) return i;
   }
   return -1;
}

template<typename T>
static int32_t contains_contiguous(GCarray *Array, lua_Number Candidate) noexcept
{
   if (Array->len IS 0) return 0;

   const T *values = Array->get<T>();
   T candidate = T(Candidate);
   for (MSize i = 0; i < Array->len; i++) {
      if (values[i] IS candidate) return 1;
   }
   return 0;
}

template<typename T>
static int32_t find_stepped(const void *Data, int32_t Start, int32_t Stop, int32_t Step, lua_Number Value)
{
   const T *base = (const T *)Data;
   T value = T(Value);
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         if (base[i] IS value) return i;
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         if (base[i] IS value) return i;
      }
   }
   return -1;
}

static ARRAY_SEARCH_NOINLINE int32_t find_in_array(GCarray *Array, lua_Number Value, int32_t Start, int32_t Stop,
   int32_t Step)
{
   const void *data = Array->arraydata();

   if (Step IS 1) {
      switch (Array->elemtype) {
         case AET::BYTE:   return find_forward_contiguous<uint8_t>(data, Start, Stop, Value);
         case AET::INT8:   return find_forward_contiguous<int8_t>(data, Start, Stop, Value);
         case AET::INT16:  return find_forward_contiguous<int16_t>(data, Start, Stop, Value);
         case AET::INT32:  return find_forward_contiguous<int32_t>(data, Start, Stop, Value);
         case AET::INT64:  return find_forward_contiguous<int64_t>(data, Start, Stop, Value);
         case AET::UINT8:  return find_forward_contiguous<uint8_t>(data, Start, Stop, Value);
         case AET::UINT16: return find_forward_contiguous<uint16_t>(data, Start, Stop, Value);
         case AET::UINT32: return find_forward_contiguous<uint32_t>(data, Start, Stop, Value);
         case AET::UINT64: return find_forward_contiguous<uint64_t>(data, Start, Stop, Value);
         case AET::FLOAT:  return find_forward_contiguous<float>(data, Start, Stop, Value);
         case AET::DOUBLE: return find_forward_contiguous<double>(data, Start, Stop, Value);
         default: return -1;
      }
   }

   switch (Array->elemtype) {
      case AET::BYTE:   return find_stepped<uint8_t>(data, Start, Stop, Step, Value);
      case AET::INT8:   return find_stepped<int8_t>(data, Start, Stop, Step, Value);
      case AET::INT16:  return find_stepped<int16_t>(data, Start, Stop, Step, Value);
      case AET::INT32:  return find_stepped<int32_t>(data, Start, Stop, Step, Value);
      case AET::INT64:  return find_stepped<int64_t>(data, Start, Stop, Step, Value);
      case AET::UINT8:  return find_stepped<uint8_t>(data, Start, Stop, Step, Value);
      case AET::UINT16: return find_stepped<uint16_t>(data, Start, Stop, Step, Value);
      case AET::UINT32: return find_stepped<uint32_t>(data, Start, Stop, Step, Value);
      case AET::UINT64: return find_stepped<uint64_t>(data, Start, Stop, Step, Value);
      case AET::FLOAT:  return find_stepped<float>(data, Start, Stop, Step, Value);
      case AET::DOUBLE: return find_stepped<double>(data, Start, Stop, Step, Value);
      default: return -1;
   }
}

static int32_t find_object_in_array(GCarray *Array, OBJECTID SearchUid, int32_t Start, int32_t Stop, int32_t Step)
{
   auto refs = Array->get<GCRef>();
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCobject *object = gco_to_object(gcref(ref));
            if (object and object->uid IS SearchUid) return i;
         }
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCobject *object = gco_to_object(gcref(ref));
            if (object and object->uid IS SearchUid) return i;
         }
      }
   }
   return -1;
}

static bool array_strings_equal(GCstr *Element, GCstr *Candidate, bool CandidateMutable) noexcept
{
   if (Element IS Candidate) return true;
   if (not CandidateMutable and not lj_str_ismutable(Element)) return false;
   return Element->len IS Candidate->len and
      memcmp(strdata(Element), strdata(Candidate), Element->len) IS 0;
}

static int32_t find_string_in_array(GCarray *Array, GCstr *SearchStr, int32_t Start, int32_t Stop, int32_t Step)
{
   auto refs = Array->get<GCRef>();
   bool search_mutable = lj_str_ismutable(SearchStr);
   if (Step > 0) {
      for (int32_t i = Start; i <= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCstr *element = gco_to_string(gcref(ref));
            if (array_strings_equal(element, SearchStr, search_mutable)) return i;
         }
      }
   }
   else {
      for (int32_t i = Start; i >= Stop; i += Step) {
         GCRef ref = refs[i];
         if (gcref(ref)) {
            GCstr *element = gco_to_string(gcref(ref));
            if (array_strings_equal(element, SearchStr, search_mutable)) return i;
         }
      }
   }
   return -1;
}

#undef ARRAY_SEARCH_NOINLINE

#endif // LJ_ARRAY_SEARCH_H
