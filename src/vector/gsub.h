#pragma once

// Glyph substitution reader for the OpenType GSUB table.
//
// Supported: Single (lookup type 1) formats 1 and 2, Multiple (type 2), Alternate (type 3) and Ligature (type 4)
// substitutions, Extension (type 7) wrappers, and lookup flags that ignore base glyphs, ligatures and marks.  Source
// clusters are retained through every substitution.  Contextual lookups and FeatureVariations are not supported.
//
// Substitution is opt-in: a lookup is only applied if a feature that references it is explicitly requested.
//
// The tables are fully validated by load().  A malformed table disables substitution rather than failing font
// loading.  Instances are not thread-safe; the vector module protects them with glFontMutex.

#include "text_shaping.h"
#include <algorithm>
#include <map>
#include <span>
#include <string>

class gsub_table {
   public:
      // Loads the GSUB and GDEF tables.  GDEF is optional (pass an empty vector).  Returns false and disables
      // substitution if the GSUB table has no lookups or if either table is malformed.

      bool load(std::vector<uint8_t> GSUB, std::vector<uint8_t> GDEF, uint32_t GlyphCount);

      // True if the font has usable substitution lookups.

      bool enabled() const { return not lookups.empty(); }

      // Applies the lookups of the requested Features to a run of glyphs in a single Script.  Budget limits the
      // total work and GrowthBudget limits the glyphs that multiple substitutions can insert.  Both are shared
      // across runs if provided.

      void apply(std::vector<shaped_glyph> &Glyphs, uint32_t Script, std::string_view Features,
         size_t *Budget = nullptr, size_t *GrowthBudget = nullptr);

      // Default work and growth budgets for a line of the given glyph count.

      static size_t work_budget(size_t Glyphs) {
         return std::min(size_t(4000000), size_t(4096) + Glyphs * size_t(256));
      }

      static size_t growth_budget(size_t Glyphs) {
         return std::min(size_t(1000000), size_t(1024) + Glyphs * size_t(8));
      }

   private:
      struct subtable {
         uint32_t offset; // Absolute offset of the subtable within the GSUB table, after Extension resolution
         uint16_t type;   // Resolved lookup type
      };

      struct lookup {
         uint16_t flags = 0;
         uint16_t mark_set = 0; // Mark filtering set index, if flags & USE_MARK_FILTERING_SET
         std::vector<subtable> subtables;
      };

      struct feature {
         uint32_t tag;
         std::vector<uint16_t> lookups;
      };

      struct selection {
         uint16_t lookup;
         uint32_t value; // The requested feature value; selects the alternate for type 3 lookups
      };

      std::vector<uint8_t> gsub, gdef;
      std::vector<lookup> lookups;
      std::vector<feature> features;
      std::map<uint32_t, std::vector<uint16_t>> scripts; // Script tag to the feature indices of its default language
      std::map<std::pair<uint32_t, std::string>, std::vector<selection>> selections; // Cached lookup selections
      uint32_t glyph_classes = 0; // GDEF offsets, zero if absent
      uint32_t mark_classes = 0;
      uint32_t mark_sets = 0;
      uint32_t glyph_count = 0;

      bool validate();
      bool validate_subtable(uint32_t Offset, uint16_t Type, uint32_t &Budget) const;
      bool validate_gdef();
      const std::vector<selection> & select(uint32_t Script, std::string_view Features);
};
