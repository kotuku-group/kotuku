// AVX2 bulk TValue operations.
// Copyright (C) 2026 Paul Manias

#define lj_bulk_avx2_c
#define LUA_CORE

#include "lj_bulk.h"

#if LJ_TARGET_X64

#include <immintrin.h>

extern "C" void lj_bulk_avx2_nil_tvalue(TValue *Dst, MSize Count)
{
   __m256i nils = _mm256_set1_epi64x(-1);
   MSize i = 0;
   for (; i + 4 <= Count; i += 4) {
      _mm256_storeu_si256((__m256i *)(Dst + i), nils);
   }
   _mm256_zeroupper();
   for (; i < Count; i++) setnilV(&Dst[i]);
}

extern "C" void lj_bulk_avx2_copy_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   MSize i = 0;
   for (; i + 4 <= Count; i += 4) {
      __m256i values = _mm256_loadu_si256((const __m256i *)(Src + i));
      _mm256_storeu_si256((__m256i *)(Dst + i), values);
   }
   _mm256_zeroupper();
   for (; i < Count; i++) Dst[i] = Src[i];
}

extern "C" void lj_bulk_avx2_move_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   uintptr_t dst_addr = uintptr_t(Dst);
   uintptr_t src_addr = uintptr_t(Src);
   uintptr_t byte_count = uintptr_t(Count) * sizeof(TValue);
   if (dst_addr > src_addr and dst_addr < src_addr + byte_count) {
      MSize i = Count;
      while (i >= 4) {
         i -= 4;
         __m256i values = _mm256_loadu_si256((const __m256i *)(Src + i));
         _mm256_storeu_si256((__m256i *)(Dst + i), values);
      }
      _mm256_zeroupper();
      while (i > 0) {
         i--;
         Dst[i] = Src[i];
      }
   }
   else {
      lj_bulk_avx2_copy_tvalue(Dst, Src, Count);
   }
}

#else

extern "C" void lj_bulk_avx2_nil_tvalue(TValue *Dst, MSize Count)
{
   for (MSize i = 0; i < Count; i++) setnilV(&Dst[i]);
}

extern "C" void lj_bulk_avx2_copy_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   for (MSize i = 0; i < Count; i++) Dst[i] = Src[i];
}

extern "C" void lj_bulk_avx2_move_tvalue(TValue *Dst, const TValue *Src, MSize Count)
{
   uintptr_t dst_addr = uintptr_t(Dst);
   uintptr_t src_addr = uintptr_t(Src);
   uintptr_t byte_count = uintptr_t(Count) * sizeof(TValue);
   if (dst_addr > src_addr and dst_addr < src_addr + byte_count) {
      for (MSize i = Count; i > 0; i--) Dst[i - 1] = Src[i - 1];
   }
   else {
      for (MSize i = 0; i < Count; i++) Dst[i] = Src[i];
   }
}

#endif
