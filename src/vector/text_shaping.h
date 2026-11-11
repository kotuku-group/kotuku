#pragma once

// Maps UTF-8 text to glyph indices and applies opt-in GSUB substitutions, retaining the source cluster of every glyph
// so that carets, selection and wrapping continue to work in terms of codepoints.

#include <cstdint>
#include <string_view>
#include <string>
#include <span>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

struct shaped_glyph {
   static constexpr uint32_t INVISIBLE = 0xffffffff; // Deleted GSUB output retains a zero-width source caret
   uint32_t glyph;      // Glyph index after substitution
   uint32_t cluster;    // Byte offset of the first codepoint in the source line
   uint32_t codepoints; // Cluster's codepoint count on its first glyph; zero on subsequent glyphs
   uint8_t mark_class;  // GDEF class, populated when substitution is supported
   uint32_t unicode;    // Source character for whitespace, word spacing and transition counting
};

class gsub_table;

// Maps UTF-8 codepoints and applies opt-in substitutions to Latin, Greek and Cyrillic script runs.
// Clusters are contiguous and monotonic, including marks skipped during ligature matching.
// The caller must hold glFontMutex.

std::vector<shaped_glyph> shape_line(FT_Face Face, std::string_view Line, gsub_table *Table = nullptr,
   std::string_view Features = {});

// Owned by a TextLine.  Text edits must invalidate it; layout settings and font size are deliberately not keys.
// Face and design coordinates distinguish font instances without reshaping on a size-only change.

class shaped_line_cache {
   FT_Face face = nullptr;
   std::vector<FT_Fixed> axes;
   std::string features;
   std::vector<shaped_glyph> glyphs;
   bool valid = false;

   public:
      void invalidate() { valid = false; }

      // Returns the shaped Line, reshaping it only if the text, face, design coordinates or features have changed.

      const std::vector<shaped_glyph> & get(FT_Face Face, std::string_view Line, std::span<const FT_Fixed> Axes,
         std::string_view Features, gsub_table *Table);
};
