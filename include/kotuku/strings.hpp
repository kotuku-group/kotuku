// NB: Keep this code as pure C++ (no external library dependencies)

#pragma once

#include <sstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <charconv>
#include <concepts>
#include <cstring>
#include <ranges>
#include <span>
#include <cstdint>
#include <type_traits>

#if defined(_MSC_VER) && defined(_M_X64)
   #include <intrin.h>
   #define PF_HAS_HW_CRC32C 1
   #define PF_CRC32C_RUNTIME_CHECK 1
#elif defined(__x86_64__) && defined(__SSE4_2__)
   #include <nmmintrin.h>
   #define PF_HAS_HW_CRC32C 1
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
   #include <arm_acle.h>
   #define PF_HAS_HW_CRC32C 1
#endif

namespace pf {

// USAGE: std::vector<std::string> list; pf::split(value, std::back_inserter(list));

template <class InType, class OutIt>
void split(InType Input, OutIt Output, char Sep = ',') noexcept
{
   auto begin = Input.begin();
   auto end = Input.end();
   auto current = begin;
   while (begin != end) {
      if (*begin == Sep) {
         *Output++ = std::string(current, begin);
         current = ++begin;
      }
      else ++begin;
   }
   *Output++ = std::string(current, begin);
}

inline void ltrim(std::string_view &String, const std::string &Whitespace = " \n\r\t") noexcept
{
   const auto start = String.find_first_not_of(Whitespace);
   if (start != std::string::npos) String.remove_prefix(start);
}

inline void ltrim(std::string &String, const std::string &Whitespace = " \n\r\t") noexcept
{
   const auto start = String.find_first_not_of(Whitespace);
   if (start != std::string::npos) String.erase(0, start);
}

inline void rtrim(std::string &String, const std::string &Whitespace = " \n\r\t") noexcept
{
   const auto end = String.find_last_not_of(Whitespace);
   if (end != std::string::npos) String.erase(end + 1);
}

inline void trim(std::string &String, const std::string &Whitespace = " \n\r\t") noexcept
{
   ltrim(String, Whitespace);
   rtrim(String, Whitespace);
}

inline void camelcase(std::string &s) noexcept {
   bool raise = true;
   for (auto &ch : s) {
      if (raise) {
         ch = std::toupper(ch);
         raise = false;
      }
      else if (unsigned(ch) <= 0x20) raise = true;
   }
}

// Case-insensitive string comparison, both of which must be the same length.

[[nodiscard]] inline bool iequals(const std::string_view lhs, const std::string_view rhs) noexcept
{
   if (lhs.size() != rhs.size()) return false;
   return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](char a, char b) {
       return std::tolower((uint8_t)(a)) == std::tolower((uint8_t)(b));
   });
}

[[nodiscard]] inline bool wildcmp(const std::string_view Wildcard, std::string_view String, bool Case = false) noexcept
{
   auto Original = String;

   if (Wildcard.empty()) return true;

   std::size_t w = 0, s = 0;
   while ((w < Wildcard.size()) and (s < String.size())) {
      bool fail = false;
      if (Wildcard[w] == '*') {
         while (w < Wildcard.size() and Wildcard[w] == '*') w++;
         if (w == Wildcard.size()) return true; // Wildcard terminated with a '*'; rest of String will match.

         auto i = Wildcard.find_first_of("*|", w); // Count the printable characters after the '*'

         if ((i != std::string::npos) and (Wildcard[i] == '|')) {
            // Scan to the end of the string for wildcard situation like "*.txt"

            auto printable = i - w;
            auto j = String.size() - s; // Number of characters left in the String
            if (j < printable) fail = true; // The string has run out of characters to cover itself for the wildcard
            else s += j - printable; // Skip everything in the second string that covers us for the '*' character
         }
         else { // Skip past the non-matching characters
            while (s < String.size()) {
               if (Case) {
                  if (Wildcard[w] == String[s]) break;
               }
               else {
                  auto char1 = std::tolower((uint8_t)(Wildcard[w]));
                  auto char2 = std::tolower((uint8_t)(String[s]));
                  if (char1 == char2) break;
               }
               s++;
            }
            // If we reached end of string without finding the required character, fail
            if (s == String.size()) fail = true;
         }
      }
      else if (Wildcard[w] == '?') { // Do not compare ? wildcards
         w++;
         s++;
      }
      else if ((Wildcard[w] == '\\') and (w+1 < Wildcard.size())) { // Escape character
         w++;
         if (Case) {
            if (Wildcard[w++] != String[s++]) fail = true;
         }
         else {
            auto char1 = std::tolower((uint8_t)(Wildcard[w++]));
            auto char2 = std::tolower((uint8_t)(String[s++]));
            if (char1 != char2) fail = true;
         }
      }
      else if ((Wildcard[w] == '|') and (w + 1 < Wildcard.size())) {
         w++;
         String = Original; // Restart the comparison
         s = 0;
      }
      else {
         if (Case) {
            if (Wildcard[w++] != String[s++]) fail = true;
         }
         else {
            auto char1 = std::tolower((uint8_t)(Wildcard[w++]));
            auto char2 = std::tolower((uint8_t)(String[s++]));
            if (char1 != char2) fail = true;
         }
      }

      if (fail) {
         // Check for an or character, if we find one, we can restart the comparison process.

         auto or_index = Wildcard.find('|', w);
         if (or_index == std::string::npos) return false;

         w = or_index + 1;
         String = Original;
         s = 0;
      }
   }

   if (String.size() == s) {
      if (w == Wildcard.size() or Wildcard[w] == '|') return true;
   }

   while (w < Wildcard.size() && Wildcard[w] == '*') w++;

   return (w == Wildcard.size() && s == String.size());
}

// A case insensitive alternative to std::string_view.starts_with()

[[nodiscard]] inline bool startswith(const std::string_view Prefix, const std::string_view String) noexcept
{
   if (Prefix.size() > String.size()) return false;
   return std::ranges::equal(Prefix, String.substr(0, Prefix.size()),
                            [](char a, char b) { return std::tolower((uint8_t)(a)) == std::tolower((uint8_t)(b)); });
}

[[nodiscard]] inline bool startswith(const std::string_view Prefix, const char * String) noexcept
{
   for (std::size_t i = 0; i < Prefix.size(); i++) {
      if (std::tolower(Prefix[i]) != std::tolower(String[i])) return false;
   }
   return true;
}

namespace detail {

inline constexpr uint32_t crc32c_init = 0xffffffffu;

[[nodiscard]] consteval std::array<uint32_t, 256> make_crc32c_table() noexcept
{
   std::array<uint32_t, 256> table = { };
   for (std::size_t i = 0; i < table.size(); i++) {
      uint32_t crc = uint32_t(i);
      for (int j = 0; j < 8; j++) {
         if (crc & 1u) crc = (crc >> 1) ^ 0x82f63b78u;
         else crc >>= 1;
      }
      table[i] = crc;
   }
   return table;
}

inline constexpr auto glCRC32cTable = make_crc32c_table();

[[nodiscard]] inline bool has_hw_crc32c() noexcept
{
#if defined(PF_CRC32C_RUNTIME_CHECK)
   static const bool available = []() noexcept {
      int cpu_info[4] = { };
      __cpuid(cpu_info, 1);
      return (cpu_info[2] & (1 << 20)) ? true : false;
   }();
   return available;
#elif defined(PF_HAS_HW_CRC32C)
   return true;
#else
   return false;
#endif
}

[[nodiscard]] constexpr inline uint32_t crc32c_finalise(const uint32_t Crc) noexcept
{
   return Crc ^ crc32c_init;
}

[[nodiscard]] constexpr inline uint8_t to_lower_ascii(const char Value) noexcept
{
   auto byte = uint8_t(Value);
   if ((byte >= 'A') and (byte <= 'Z')) return byte - 'A' + 'a';
   return byte;
}

[[nodiscard]] constexpr inline uint32_t crc32c_byte(const uint32_t Crc, const uint8_t Byte) noexcept
{
   return (Crc >> 8) ^ glCRC32cTable[(Crc ^ Byte) & 0xffu];
}

[[nodiscard]] constexpr inline uint32_t crc32c_constexpr(const std::string_view String) noexcept
{
   uint32_t crc = crc32c_init;
   for (char value : String) {
      crc = crc32c_byte(crc, uint8_t(value));
   }
   return crc32c_finalise(crc);
}

[[nodiscard]] inline uint32_t crc32c_runtime_byte(const uint32_t Crc, const uint8_t Byte) noexcept
{
   if (has_hw_crc32c()) {
      #if defined(PF_HAS_HW_CRC32C)
         #if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
            return __crc32cb(Crc, Byte);
         #else
            return uint32_t(_mm_crc32_u8(Crc, Byte));
         #endif
      #endif
   }

   return crc32c_byte(Crc, Byte);
}

[[nodiscard]] inline uint32_t crc32c_runtime(const std::string_view String) noexcept
{
   uint32_t crc = crc32c_init;

   if (has_hw_crc32c()) {
      #if defined(PF_HAS_HW_CRC32C)
         auto data = (const uint8_t *)String.data();
         auto size = String.size();
         while (size >= sizeof(uint64_t)) {
            uint64_t chunk = 0;
            std::memcpy(&chunk, data, sizeof(chunk));
            #if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
               crc = __crc32cd(crc, chunk);
            #else
               crc = uint32_t(_mm_crc32_u64(crc, chunk));
            #endif
            data += sizeof(uint64_t);
            size -= sizeof(uint64_t);
         }

         while (size > 0) {
            #if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
               crc = __crc32cb(crc, *data++);
            #else
               crc = uint32_t(_mm_crc32_u8(crc, *data++));
            #endif
            size--;
         }
      #endif
   }
   else {
      for (char value : String) {
         crc = crc32c_byte(crc, uint8_t(value));
      }
   }

   return crc32c_finalise(crc);
}

} // namespace detail

// Standardised hash functions, case sensitive and insensitive versions

[[nodiscard]] constexpr inline uint32_t strhash(const std::string_view String) noexcept
{
   if (std::is_constant_evaluated()) return detail::crc32c_constexpr(String);
   else return detail::crc32c_runtime(String);
}

[[nodiscard]] constexpr inline uint32_t strihash(const std::string_view String) noexcept
{
   uint32_t crc = detail::crc32c_init;
   if (std::is_constant_evaluated()) {
      for (char value : String) {
         crc = detail::crc32c_byte(crc, detail::to_lower_ascii(value));
      }
   }
   else {
      for (char value : String) {
         crc = detail::crc32c_runtime_byte(crc, detail::to_lower_ascii(value));
      }
   }
   return detail::crc32c_finalise(crc);
}

// Hash designed to handle conversion from `UID` -> `uid` and `RGBValue` -> `rgbValue`.  This keeps hashes compatible
// with Tiri naming conventions for field names.

[[nodiscard]] constexpr inline uint32_t fieldhash(const std::string_view String) noexcept
{
   uint32_t crc = detail::crc32c_init;
   size_t k = 0;
   while ((k < String.size()) and (String[k] >= 'A') and (String[k] <= 'Z')) {
      auto value = detail::to_lower_ascii(String[k]);
      if (std::is_constant_evaluated()) crc = detail::crc32c_byte(crc, value);
      else crc = detail::crc32c_runtime_byte(crc, value);
      if (++k >= String.size()) break;
      if ((k + 1 >= String.size()) or ((String[k+1] >= 'A') and (String[k+1] <= 'Z'))) continue;
      else break;
   }
   while (k < String.size()) {
      if (std::is_constant_evaluated()) crc = detail::crc32c_byte(crc, uint8_t(String[k]));
      else crc = detail::crc32c_runtime_byte(crc, uint8_t(String[k]));
      k++;
   }
    return detail::crc32c_finalise(crc);
}

// Simple string copy.  Supports std::string and CSTRING only

template <class T> inline int strcopy(T &&Source, char *Dest, int Length = 0x7fffffff) noexcept
{
   const char *src;
   if constexpr (std::is_pointer_v<std::decay_t<T>>) src = Source;
   else src = Source.c_str();  // Works for std::string.

   if ((Length > 0) and (src) and (Dest)) {
      int i = 0;
      while (*src) {
         if (i == Length) {
            Dest[i-1] = 0;
            return i;
         }
         Dest[i++] = *src++;
      }

      Dest[i] = 0;
      return i;
   }
   else return 0;
}

// String copy using std::span for better memory safety

template <class T, std::size_t N>
inline int strcopy(T &&Source, std::span<char, N> Dest) noexcept
{
   const char *src;
   if constexpr (std::is_pointer_v<std::decay_t<T>>) src = Source;
   else src = Source.c_str()();  // Works for std::string

   if (src and not Dest.empty()) {
      std::size_t i = 0;
      while (*src and i < Dest.size() - 1) Dest[i++] = *src++;
      Dest[i] = 0;
      return int(i);
   }
   else return 0;
}

// Case-sensitive keyword search

[[nodiscard]] inline int strsearch(const std::string_view Keyword, const char * String) noexcept
{
   size_t i;
   size_t pos = 0;
   while (String[pos]) {
      for (i=0; i < Keyword.size(); i++) if (String[pos+i] != Keyword[i]) break;
      if (i == Keyword.size()) return int(pos);
      for (++pos; (String[pos] & 0xc0) == 0x80; pos++);
   }

   return -1;
}

// Case-insensitive keyword search

[[nodiscard]] inline int strisearch(const std::string_view Keyword, const char * String) noexcept
{
   size_t i;
   size_t pos = 0;
   while (String[pos]) {
      for (i=0; i < Keyword.size(); i++) if (std::toupper(String[pos+i]) != std::toupper(Keyword[i])) break;
      if (i == Keyword.size()) return int(pos);
      for (++pos; (String[pos] & 0xc0) == 0x80; pos++);
   }

   return -1;
}

// std::string_view conversion to numeric type.  Returns zero on error.
// Leading whitespace is not ignored, unlike strtol() and strtod()

template <class T>
requires std::is_arithmetic_v<T>
[[nodiscard]] T svtonum(const std::string_view String) noexcept {
   T val;
   auto [ v, error ] = std::from_chars(String.data(), String.data() + String.size(), val);
   if (error == std::errc()) return val;
   else return 0;
}

} // namespace
