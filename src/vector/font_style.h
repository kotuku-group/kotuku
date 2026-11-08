#pragma once

// Parser for font style strings, e.g. `Semi Bold;width=87.5%` or `weight=550;slant=12;GRAD=-50`.
//
// A style string is an optional named style followed by `;` separated `key=value` descriptors.  The parser has no
// FreeType or Kotuku core dependency so that it can be unit tested in isolation.  Problems are reported through the
// warnings list rather than a log, and never cause the parse to fail.

#include <kotuku/font_traits.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct font_style_spec {
   enum class OPSZ { AUTO, NONE, FIXED };

   std::string named;                             // Leading named style, may be empty
   std::optional<double> weight, width, slant;    // CSS weight, width percentage and oblique angle in degrees
   std::optional<bool> italic;
   OPSZ opsz_mode = OPSZ::AUTO;
   bool opsz_set = false;                         // True if opsz was declared
   bool invalid = false;                          // True if a known key has a malformed value
   double opsz = 0;                               // Optical size if opsz_mode is FIXED
   std::vector<std::pair<uint32_t, double>> tags; // Raw axis tags in declaration order; later wins
   std::vector<std::string> warnings;             // Descriptors that were ignored
   bool descriptors = false;                      // True if at least one valid descriptor was given

   bool has_descriptors() const { return descriptors; }
};

// Converts a four character axis tag to its OpenType integer form, and back.

constexpr uint32_t make_axis_tag(std::string_view Tag) {
   return (uint32_t(uint8_t(Tag[0]))<<24) | (uint32_t(uint8_t(Tag[1]))<<16) | (uint32_t(uint8_t(Tag[2]))<<8) |
      uint32_t(uint8_t(Tag[3]));
}

inline std::string axis_tag_name(uint32_t Tag) {
   return { char(Tag>>24), char((Tag>>16) & 0xff), char((Tag>>8) & 0xff), char(Tag & 0xff) };
}

extern font_style_spec parse_font_style(std::string_view Style);
