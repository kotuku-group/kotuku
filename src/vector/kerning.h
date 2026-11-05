#pragma once

// Pair kerning reader for the OpenType GPOS table.
//
// FreeType's FT_Get_Kerning() only reads the legacy 'kern' table, and its optional GPOS support skips the device
// records that variable fonts use for per-instance kerning.  This reader answers pair kerning queries from the
// 'kern' feature of the GPOS table, including variation deltas resolved through the GDEF ItemVariationStore.
//
// The reader is independent of FreeType: it is given the raw GPOS and GDEF tables and the normalised design
// coordinates of the instance.  Results are in font units and are not rounded.
//
// Supported: PairPos (lookup type 2) formats 1 and 2, Extension (type 9) wrappers, Coverage and ClassDef formats 1
// and 2, lookup flags that ignore base glyphs, ligatures and marks, and VariationIndex device records.  Hinting
// device tables are ignored, as are all other lookup types.
//
// The tables are fully validated by load().  A malformed table disables kerning rather than failing font loading.
// Instances are not thread-safe; the vector module protects them with glFontMutex.

#include <kotuku/main.h>
#include <cstdint>
#include <span>
#include <vector>
#include <ankerl/unordered_dense.h>

class gpos_kerning {
   public:
      struct adjustment {
         double first_advance = 0, second_advance = 0;
         double first_placement = 0, second_placement = 0;
      };

      // Per-instance state.  Region scalars, resolved variation deltas and pair results are cached here, so one
      // instance is required for each set of design coordinates.

      class instance {
         friend class gpos_kerning;

         // Pair results are memoised in a direct-mapped cache, which is cheaper than a hash map for the access
         // pattern of text layout.  A collision evicts the older pair, which is recomputed if queried again.

         static constexpr int PAIR_CACHE_BITS = 12;
         static constexpr uint32_t EMPTY_PAIR = 0xffffffff;
         struct pair_entry { uint32_t key = EMPTY_PAIR; adjustment value; };

         std::vector<int> coords; // Normalised coordinates in F2Dot14 format (16384 = 1.0)
         std::vector<float> scalars; // Region scalars, negative if not yet computed
         ankerl::unordered_dense::map<uint32_t, float> deltas; // VariationIndex (outer<<16|inner) to delta
         std::vector<pair_entry> pairs; // Allocated on first use; keyed by (First<<16)|Second
         bool variable = false; // True if any coordinate is non-zero

         public:
            instance() = default;
            instance(std::span<const int> Coords) { set_coords(Coords); }

            // Sets the normalised design coordinates of the instance and discards all cached results.

            void set_coords(std::span<const int> Coords) {
               coords.assign(Coords.begin(), Coords.end());
               variable = false;
               for (auto c : coords) if (c) variable = true;
               scalars.clear();
               deltas.clear();
               pairs.clear();
            }
      };

      gpos_kerning() = default;

      // Loads the GPOS and GDEF tables.  GDEF is optional (pass an empty vector).  Returns false and disables
      // kerning if the GPOS table has no usable 'kern' lookups or if either table is malformed.

      bool load(std::vector<uint8_t> GPOS, std::vector<uint8_t> GDEF);

      // True if the font has GPOS pair kerning.

      bool enabled() const { return not lookups.empty(); }

      // Returns the kerning between two glyph indices in font units.  The value is the change to the advance of the
      // pair.  Placement adjustments do not affect its advance.

      double pair_kerning(uint32_t First, uint32_t Second, instance &Instance) const {
         auto value = pair_adjustment(First, Second, Instance);
         return value.first_advance + value.second_advance;
      }

      adjustment pair_adjustment(uint32_t First, uint32_t Second, instance &Instance) const {
         if ((First > 0xffff) or (Second > 0xffff)) return {}; // Coverage tables only list 16-bit glyph indices
         const uint32_t key = (First<<16) | Second;
         if (not Instance.pairs.empty()) {
            auto &entry = Instance.pairs[(key * 2654435761u)>>(32 - instance::PAIR_CACHE_BITS)];
            if (entry.key IS key) return entry.value;
         }
         return compute_pair(key, Instance);
      }

   private:
      struct lookup {
         uint16_t flags = 0;
         uint16_t mark_set = 0;              // Mark filtering set index, if flags & USE_MARK_FILTERING_SET
         std::vector<uint32_t> subtables;   // Absolute offsets of PairPos subtables within the GPOS table
      };

      std::vector<uint8_t> gpos, gdef;
      std::vector<lookup> lookups;
      uint32_t glyph_class_def = 0;  // GDEF offsets, zero if absent
      uint32_t mark_attach_class_def = 0;
      uint32_t mark_glyph_sets = 0;
      uint32_t var_store = 0;
      uint16_t region_count = 0;

      adjustment compute_pair(uint32_t Key, instance &) const;
      bool parse_lookups();
      bool validate_gdef();
      bool skip_glyph(const lookup &, uint32_t Glyph) const;
      bool apply_subtable(uint32_t Offset, uint32_t First, uint32_t Second, instance &, adjustment &Value) const;
      double value_component(uint32_t Base, uint32_t Record, uint16_t Format, uint16_t Bit, instance &) const;
      float variation_delta(uint16_t Outer, uint16_t Inner, instance &) const;
      float region_scalar(uint16_t Region, instance &) const;
};
