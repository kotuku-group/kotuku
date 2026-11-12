#pragma once

// Glyph substitution reader for the OpenType GSUB table.
//
// Supports lookup types 1 to 6, Extension (type 7) wrappers, lookup filtering and FeatureVariations.  Contextual
// lookups support glyph, class and coverage patterns.  Source clusters survive every substitution.
//
// A lookup is only applied if a feature that references it is requested.  Default features are the caller's
// responsibility; VectorText adds its own before shaping.
//
// The tables are fully validated by load().  A malformed table disables substitution rather than failing font
// loading.  Instances are not thread-safe; the vector module protects them with glFontMutex.

#include "text_shaping.h"
#include <algorithm>
#include <map>
#include <span>
#include <string>
#include <tuple>

class gsub_table {
   public:
      // Loads the GSUB and GDEF tables.  GDEF is optional (pass an empty vector).  Returns false and disables
      // substitution if the GSUB table has no lookups or if either table is malformed.

      bool load(std::vector<uint8_t> GSUB, std::vector<uint8_t> GDEF, uint32_t GlyphCount);

      // True if the font has usable substitution lookups.

      bool enabled() const { return not lookups.empty(); }
      bool has_variations() const { return not variations.empty(); }

      // Adds the tags of the features that apply() can act upon for any of the Scripts to Tags.  A feature qualifies
      // if it is listed by the script's default language system, or DFLT for a script that the font doesn't define,
      // and references at least one supported lookup, including through a FeatureVariations alternate.

      void feature_tags(std::span<const uint32_t> Scripts, std::vector<uint32_t> &Tags) const;

      // Applies the lookups of the requested Features to a run of glyphs in a single Script.  Budget limits the
      // total work and GrowthBudget limits the glyphs that multiple substitutions can insert.  Both are shared
      // across runs if provided.  Coordinates are normalised, avar-mapped F2Dot14 values in face axis order.

      void apply(std::vector<shaped_glyph> &Glyphs, uint32_t Script, std::string_view Features,
         size_t *Budget = nullptr, size_t *GrowthBudget = nullptr, std::span<const int> Coordinates = {});

      // Default work and growth budgets for a line of the given glyph count.

      static size_t work_budget(size_t Glyphs) {
         return std::min(size_t(4000000), size_t(4096) + Glyphs * size_t(256));
      }

      static size_t growth_budget(size_t Glyphs) {
         return std::min(size_t(1000000), size_t(1024) + Glyphs * size_t(8));
      }

   private:
      struct sequence_lookup { uint16_t sequence, lookup; };

      struct context_rule {
         uint32_t backtrack = 0, input = 0, lookahead = 0; // Absolute offsets of pattern arrays
         uint16_t backtrack_count = 0, input_count = 0, lookahead_count = 0;
         std::vector<sequence_lookup> actions;
      };

      struct subtable {
         uint32_t offset; // Absolute offset of the subtable within the GSUB table, after Extension resolution
         uint16_t type;   // Resolved lookup type
         uint16_t format = 0;
         uint32_t coverage = 0, backtrack_classes = 0, input_classes = 0, lookahead_classes = 0;
         std::vector<context_rule> rules;
         std::vector<std::pair<uint32_t, uint32_t>> rule_sets; // First rule and count, by coverage index or class
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

      struct condition { uint16_t axis; int16_t minimum, maximum; };
      struct variation {
         bool supported = true;
         std::vector<condition> conditions;
         std::map<uint16_t, std::vector<uint16_t>> replacements;
      };

      std::vector<uint8_t> gsub, gdef;
      std::vector<lookup> lookups;
      std::vector<feature> features;
      std::map<uint32_t, std::vector<uint16_t>> scripts; // Script tag to the feature indices of its default language
      std::vector<variation> variations;
      std::map<std::tuple<uint32_t, std::string, int>, std::vector<selection>> selections;
      uint32_t glyph_classes = 0; // GDEF offsets, zero if absent
      uint32_t mark_classes = 0;
      uint32_t mark_sets = 0;
      uint32_t glyph_count = 0;

      bool validate();
      bool validate_subtable(uint32_t Offset, uint16_t Type, uint32_t &Budget) const;
      bool validate_context(subtable &Sub, uint32_t &Budget);
      bool validate_variations(uint32_t Offset, uint32_t &Budget);
      bool validate_gdef();
      int variation_index(std::span<const int> Coordinates, size_t &Budget) const;
      const std::vector<selection> & select(uint32_t Script, std::string_view Features, int Variation);
};
