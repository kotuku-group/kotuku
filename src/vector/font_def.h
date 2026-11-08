#pragma once

// A font definition is a single-line string that describes a complete font selection.  It is a strict superset of the
// style string in font_style.h.  This struct is private to the Vector module; clients exchange definitions as strings
// and read them with NormaliseTextStyle() and ReadTextStyle().

#include "font_style.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct font_length {
   enum class UNIT { NONE, PX, PT, EM };

   double value = 0;
   UNIT unit = UNIT::NONE;

   // Resolves to pixels.  FontSize is the pixel em size that the `em` unit is relative to.

   [[nodiscard]] double pixels(double FontSize) const noexcept {
      switch (unit) {
         case UNIT::PT: return value * 96.0 / 72.0;
         case UNIT::EM: return value * FontSize;
         default:       return value;
      }
   }

   bool operator==(const font_length &) const = default;
};

struct font_shadow_layer {
   double x = 0, y = 0, blur = 0;
   std::string colour;
};

struct font_def {
   font_style_spec style;
   std::optional<std::string> face, features, kerning, fill, stroke, stroke_align, line_join, shadow, glow, blend;
   std::optional<font_length> size, line_height, letter_spacing, word_spacing, stroke_width;
   std::optional<double> opacity;

   // Declared keys and their normalised values, in canonical order.

   [[nodiscard]] std::vector<std::pair<std::string, std::string>> entries() const;

   [[nodiscard]] std::string serialise() const; // The canonical form of the whole definition
   [[nodiscard]] std::string style_string() const; // The canonical form of the style keys only, for FontStyle
   void merge(const font_def &Other); // Overwrites the keys that Other declares
};

// Fails with ERR::Syntax if a known key has a malformed value.  Unknown keys are logged and skipped.

[[nodiscard]] extern ERR parse_font_def(kt::Log &, std::string_view, font_def &);

// Effect value decoders.  Definitions that passed parse_font_def() always decode.

[[nodiscard]] extern std::vector<font_shadow_layer> decode_shadow(std::string_view);
[[nodiscard]] extern bool decode_glow(std::string_view, double &Radius, std::string &Colour);
