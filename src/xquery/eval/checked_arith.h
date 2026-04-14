// Minimal checked arithmetic helpers for 64-bit operations
// Portable across MSVC and GCC/Clang without relying on __int128.

#pragma once

#include <cstdint>
#include <limits>

[[nodiscard]] static inline bool checked_add_u64(uint64_t A, uint64_t B, uint64_t &Out) noexcept
{
   if (A > (std::numeric_limits<uint64_t>::max)() - B) return false;
   Out = A + B;
   return true;
}

[[nodiscard]] static inline bool compute_range_length_s64(int64_t Start, int64_t End, uint64_t &Out) noexcept
{
   if (Start > End) { Out = 0u; return true; }
   int64_t diff = End - Start; // safe because End >= Start
   uint64_t base = (uint64_t)diff;
   return checked_add_u64(base, 1u, Out);
}

