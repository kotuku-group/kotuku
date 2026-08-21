// Native array search runtime.
// Copyright © 2026 Paul Manias.

#define find_forward_contiguous runtime_find_forward_contiguous
#define find_stepped runtime_find_stepped
#define find_in_array runtime_find_in_array
#define find_string_in_array runtime_find_string_in_array
#define find_object_in_array runtime_find_object_in_array
#define ARRAY_SEARCH_NOINLINE
#include "lj_array_search.h"
#undef find_forward_contiguous
#undef find_stepped
#undef find_in_array
#undef find_string_in_array
#undef find_object_in_array
#include "lj_vmarray.h"

LJ_NOAPI int32_t lj_arr_find_num(GCarray *Array, lua_Number Value, int32_t Start, int32_t Stop, int32_t Step)
{
   return runtime_find_in_array(Array, Value, Start, Stop, Step);
}

LJ_NOAPI int32_t lj_arr_find_str(GCarray *Array, GCstr *SearchStr, int32_t Start, int32_t Stop, int32_t Step)
{
   return runtime_find_string_in_array(Array, SearchStr, Start, Stop, Step);
}

LJ_NOAPI int32_t lj_arr_find_object(GCarray *Array, OBJECTID SearchUid, int32_t Start, int32_t Stop, int32_t Step)
{
   return runtime_find_object_in_array(Array, SearchUid, Start, Stop, Step);
}

extern "C" int32_t lj_arr_contains_num(GCarray *Array, lua_Number Candidate)
{
   if (Array->len IS 0) return 0;
   return runtime_find_in_array(Array, Candidate, 0, int32_t(Array->len - 1), 1) >= 0;
}

extern "C" int32_t lj_arr_contains_str(GCarray *Array, GCstr *Candidate)
{
   if (Array->len IS 0) return 0;
   return runtime_find_string_in_array(Array, Candidate, 0, int32_t(Array->len - 1), 1) >= 0;
}
