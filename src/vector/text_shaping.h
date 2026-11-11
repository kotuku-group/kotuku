#pragma once

// Maps UTF-8 text to glyph indices, applies requested GSUB substitutions and records GPOS mark attachments.
// Source clusters retain codepoint-based carets, selection and wrapping.  VectorText supplies its default ccmp request.

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
   uint32_t attachment = INVISIBLE; // Index of an attached mark's base within the shaped hard line
   double mark_x = 0, mark_y = 0; // Base-relative anchor difference in font units, with Y increasing upwards
};

class gsub_table;
class gpos_kerning;

// Maps UTF-8 codepoints and applies substitutions and mark attachments to Latin, Greek and Cyrillic script runs.
// Clusters are contiguous and monotonic, including marks skipped during ligature matching.
// The caller must hold glFontMutex.

std::vector<shaped_glyph> shape_line(FT_Face Face, std::string_view Line, gsub_table *Table = nullptr,
   std::string_view Features = {}, std::span<const int> Coordinates = {}, gpos_kerning *Positioning = nullptr);

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
         std::string_view Features, gsub_table *Table, std::span<const int> Coordinates = {},
         gpos_kerning *Positioning = nullptr);
};
