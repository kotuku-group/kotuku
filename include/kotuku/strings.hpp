// NB: Keep this code as pure C++ (no external library dependencies)

#pragma once

#include <sstream>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <charconv>
#include <concepts>
#include <ranges>
#include <span>
#include <cstdint>

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

// Standardised hash functions, case sensitive and insensitive versions

[[nodiscard]] constexpr inline uint32_t strhash(const std::string_view String) noexcept
{
   uint32_t hash = 5381;
   std::for_each(String.begin(), String.end(), [&hash](char a) {
      hash = (hash<<5) + hash + a;
   });
   return hash;
}

[[nodiscard]] constexpr inline uint32_t strihash(const std::string_view String) noexcept
{
   uint32_t hash = 5381;
   std::for_each(String.begin(), String.end(), [&hash](char c) {
      if ((c >= 'A') and (c <= 'Z')) c = c - 'A' + 'a';
      hash = (hash<<5) + hash + c;
   });
   return hash;
}

// Hash designed to handle conversion from `UID` -> `uid` and `RGBValue` -> `rgbValue`.  This keeps hashes compatible
// with Tiri naming conventions for field names.

[[nodiscard]] constexpr inline uint32_t fieldhash(const std::string_view String) noexcept
{
   uint32_t hash = 5381;
   size_t k = 0;
   while ((k < String.size()) and (String[k] >= 'A') and (String[k] <= 'Z')) {
      hash = (hash<<5) + hash + (String[k] - 'A' + 'a');
      if (++k >= String.size()) break;
      if ((k + 1 >= String.size()) or ((String[k+1] >= 'A') and (String[k+1] <= 'Z'))) continue;
      else break;
   }
   while (k < String.size()) {
      hash = (hash<<5) + hash + String[k];
      k++;
   }
   return hash;
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
