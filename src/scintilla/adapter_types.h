#pragma once

#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace KotukuScintilla {

// Reserve room for both the trailing NUL and the extra line in an all-newline document.
inline constexpr int document_limit = std::numeric_limits<int>::max() - 1;

template<std::integral T> constexpr bool PublicInteger(T Value, int &Result) noexcept
{
   if (not std::in_range<int>(Value)) return false;
   Result = int(Value);
   return true;
}

constexpr bool DocumentGrowth(std::ptrdiff_t Current, std::ptrdiff_t Removed, size_t Added) noexcept
{
   if ((Current < 0) or (Current > document_limit) or (Removed < 0) or (Removed > Current)) return false;
   return Added <= size_t(document_limit - (Current - Removed));
}

}
