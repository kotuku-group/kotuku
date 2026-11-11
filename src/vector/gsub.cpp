// GSUB substitution reader.  See gsub.h for an overview.
//
// References: OpenType specification, chapters 'GSUB', 'GDEF' and 'OpenType Layout Common Table Formats'.

#include "gsub.h"
#include "opentype.h"
#include <algorithm>

using ot_reader::reader;
using ot_reader::coverage_index;
using ot_reader::glyph_class;
using ot_reader::LF_RESERVED;
using ot_reader::LF_USE_MARK_FILTERING_SET;

namespace {

constexpr uint16_t LOOKUP_SINGLE    = 1;
constexpr uint16_t LOOKUP_MULTIPLE  = 2;
constexpr uint16_t LOOKUP_ALTERNATE = 3;
constexpr uint16_t LOOKUP_LIGATURE  = 4;
constexpr uint16_t LOOKUP_EXTENSION = 7;
constexpr uint16_t LOOKUP_MAX_TYPE  = 8;

constexpr uint32_t MAX_TABLE = 64 * 1024 * 1024;
constexpr uint32_t MAX_RECORDS = 1000000; // Sanity limit for the total records visited during validation
constexpr uint32_t MAX_LOOKUPS = 1024;
constexpr uint32_t MAX_SUBTABLES = 4096;
constexpr size_t MAX_SELECTIONS = 64; // Cached lookup selections per face

enum class MATCH { NONE, APPLIED, HALT };

// Structural offsets must point beyond their parent's header, never back into it.  Shared child tables are valid.

uint32_t child(const reader &R, uint32_t Base, uint32_t Relative, uint32_t Header)
{
   const uint64_t offset = uint64_t(Base) + Relative;
   if ((Relative < Header) or (not R.has(offset, 2))) { R.bad = true; return 0; }
   return uint32_t(offset);
}

// GSUB coverage needs ordered, disjoint ranges with consecutive coverage indices, not just bounded arrays.  Returns
// the number of covered glyphs, or -1 if the table is invalid.

int coverage_size(const reader &R, uint32_t Offset, uint32_t GlyphCount, uint32_t &Budget)
{
   const auto format = R.u16(Offset), count = R.u16(Offset + 2);
   if ((count > Budget) or (not ot_reader::validate_coverage(R, Offset))) return -1;
   Budget -= count;
   int last = -1, total = 0;
   for (uint32_t i=0; i < count; i++) {
      if (format IS 1) {
         const auto glyph = R.u16(Offset + 4 + i * 2);
         if ((glyph <= last) or (glyph >= GlyphCount)) return -1;
         last = glyph;
         total++;
      }
      else {
         const auto rec = Offset + 4 + i * 6;
         const auto first = R.u16(rec), end = R.u16(rec + 2);
         if ((first <= last) or (end < first) or (end >= GlyphCount) or (R.u16(rec + 4) != total)) return -1;
         last = end;
         total += end - first + 1;
      }
   }
   return R.bad ? -1 : total;
}

bool valid_classes(const reader &R, uint32_t Offset, uint32_t GlyphCount, uint32_t Maximum, uint32_t &Budget)
{
   if (not ot_reader::validate_class_def(R, Offset)) return false;
   const auto format = R.u16(Offset);
   const auto count = R.u16(Offset + ((format IS 1) ? 4 : 2));
   if (count > Budget) return false;
   Budget -= count;
   if ((format IS 1) and (uint32_t(R.u16(Offset + 2)) + count > GlyphCount)) return false;
   int last = -1;
   for (uint32_t i=0; i < count; i++) {
      uint16_t cls;
      if (format IS 1) cls = R.u16(Offset + 6 + i * 2);
      else {
         const auto rec = Offset + 4 + i * 6;
         const auto first = R.u16(rec), end = R.u16(rec + 2);
         if ((first <= last) or (end < first) or (end >= GlyphCount)) return false;
         last = end;
         cls = R.u16(rec + 4);
      }
      if (cls > Maximum) return false;
   }
   return not R.bad;
}

} // namespace

//********************************************************************************************************************

bool gsub_table::load(std::vector<uint8_t> GSUB, std::vector<uint8_t> GDEF, uint32_t GlyphCount)
{
   gsub = std::move(GSUB);
   gdef = std::move(GDEF);
   glyph_count = GlyphCount;
   lookups.clear();
   features.clear();
   scripts.clear();
   selections.clear();
   glyph_classes = mark_classes = mark_sets = 0;
   if ((gsub.size() > MAX_TABLE) or (gdef.size() > MAX_TABLE) or (not glyph_count) or
       (glyph_count > 65536) or (not validate()) or (not validate_gdef())) {
      lookups.clear();
      features.clear();
      scripts.clear();
      gsub.clear();
      gdef.clear();
      return false;
   }
   return enabled();
}

//********************************************************************************************************************
// Validates the GSUB header, lookup, feature and script lists, and records the lookups and features.

bool gsub_table::validate()
{
   const reader r(gsub);
   if ((r.u16(0) != 1) or (r.u16(2) > 1)) return false;
   const uint32_t header = (r.u16(2) IS 1) ? 14 : 10;
   if (not r.has(0, header)) return false;
   const auto script_list = child(r, 0, r.u16(4), header);
   const auto feature_list = child(r, 0, r.u16(6), header);
   const auto lookup_list = child(r, 0, r.u16(8), header);
   if (r.bad) return false;

   // FeatureVariations are not evaluated, but their top-level offset must be in bounds.

   if ((header IS 14) and r.u32(10)) {
      if (not child(r, 0, r.u32(10), header)) return false;
   }

   uint32_t budget = MAX_RECORDS;
   const auto lookup_count = r.u16(lookup_list);
   if ((lookup_count > MAX_LOOKUPS) or (not r.has(lookup_list + 2, uint64_t(lookup_count) * 2))) return false;
   lookups.resize(lookup_count);
   uint32_t subtable_count = 0;
   for (uint32_t i=0; i < lookup_count; i++) {
      const auto offset = child(r, lookup_list, r.u16(lookup_list + 2 + i * 2), 2 + lookup_count * 2);
      const auto type = r.u16(offset), count = r.u16(offset + 4);
      auto &lookup = lookups[i];
      lookup.flags = r.u16(offset + 2);
      const bool filtered = lookup.flags & LF_USE_MARK_FILTERING_SET;
      const uint32_t lookup_header = 6 + uint32_t(count) * 2 + (filtered ? 2 : 0);
      if (r.bad or (not type) or (type > LOOKUP_MAX_TYPE) or (lookup.flags & LF_RESERVED) or
          (not r.has(offset, lookup_header)) or ((subtable_count += count) > MAX_SUBTABLES)) return false;
      if (filtered) lookup.mark_set = r.u16(offset + 6 + uint32_t(count) * 2);

      uint16_t extension_type = 0;
      for (uint32_t s=0; s < count; s++) {
         auto sub = child(r, offset, r.u16(offset + 6 + s * 2), lookup_header);
         auto real_type = type;
         if (type IS LOOKUP_EXTENSION) {
            if ((r.u16(sub) != 1) or (not r.has(sub, 8))) return false;
            real_type = r.u16(sub + 2);
            if ((not real_type) or (real_type > LOOKUP_MAX_TYPE) or (real_type IS LOOKUP_EXTENSION) or
                (extension_type and (extension_type != real_type))) return false;
            extension_type = real_type;
            sub = child(r, sub, r.u32(sub + 4), 8);
         }
         if (r.bad) return false;

         // The contents of unsupported lookup types are never dereferenced.

         if ((real_type < LOOKUP_SINGLE) or (real_type > LOOKUP_LIGATURE)) continue;

         if (not validate_subtable(sub, real_type, budget)) return false;
         lookup.subtables.push_back({ sub, real_type });
      }
   }

   const auto feature_count = r.u16(feature_list);
   if (not r.has(feature_list + 2, uint64_t(feature_count) * 6)) return false;
   features.reserve(feature_count);
   for (uint32_t i=0; i < feature_count; i++) {
      const auto rec = feature_list + 2 + i * 6;
      const auto offset = child(r, feature_list, r.u16(rec + 4), 2 + uint32_t(feature_count) * 6);
      const auto count = r.u16(offset + 2);
      if ((count > budget) or (not r.has(offset + 4, uint64_t(count) * 2))) return false;
      budget -= count;
      if (r.u16(offset) and (not child(r, offset, r.u16(offset), 4 + uint32_t(count) * 2))) return false;
      features.push_back({ r.u32(rec), {} });
      auto &feature = features.back();
      for (uint32_t j=0; j < count; j++) {
         const auto index = r.u16(offset + 4 + j * 2);
         if (index >= lookup_count) return false;
         feature.lookups.push_back(index);
      }
   }

   // Validates a LangSys table.  Feature indices are recorded if Indices is provided.  A required feature is
   // treated as an ordinary one, so it still needs an explicit request.

   auto language = [&](uint32_t Offset, std::vector<uint16_t> *Indices) {
      if (r.u16(Offset) != 0) return false;
      const auto required = r.u16(Offset + 2), count = r.u16(Offset + 4);
      if ((count > budget) or (not r.has(Offset + 6, uint64_t(count) * 2))) return false;
      budget -= count;
      if (required != 0xffff) {
         if (required >= feature_count) return false;
         if (Indices) Indices->push_back(required);
      }
      for (uint32_t i=0; i < count; i++) {
         const auto index = r.u16(Offset + 6 + i * 2);
         if (index >= feature_count) return false;
         if (Indices) Indices->push_back(index);
      }
      return not r.bad;
   };

   const auto script_count = r.u16(script_list);
   if (not r.has(script_list + 2, uint64_t(script_count) * 6)) return false;
   for (uint32_t i=0; i < script_count; i++) {
      const auto rec = script_list + 2 + i * 6;
      const auto offset = child(r, script_list, r.u16(rec + 4), 2 + uint32_t(script_count) * 6);
      const auto count = r.u16(offset + 2);
      if ((count > budget) or (not r.has(offset + 4, uint64_t(count) * 6))) return false;
      budget -= count;
      const uint32_t script_header = 4 + uint32_t(count) * 6;
      if (const auto def = r.u16(offset)) {
         if (scripts.contains(r.u32(rec))) return false;
         if (not language(child(r, offset, def, script_header), &scripts[r.u32(rec)])) return false;
      }
      for (uint32_t j=0; j < count; j++) {
         const auto lang = child(r, offset, r.u16(offset + 8 + j * 6), script_header);
         if (not language(lang, nullptr)) return false;
      }
   }
   return not r.bad;
}

//********************************************************************************************************************
// Validates a Single, Multiple, Alternate or Ligature subtable.  Every output glyph is checked against the face's
// glyph count.

bool gsub_table::validate_subtable(uint32_t Offset, uint16_t Type, uint32_t &Budget) const
{
   const reader r(gsub);
   const auto format = r.u16(Offset);
   const bool delta_format = (Type IS LOOKUP_SINGLE) and (format IS 1);
   if (((Type IS LOOKUP_SINGLE) and (format != 1) and (format != 2)) or
       ((Type != LOOKUP_SINGLE) and (format != 1))) return false;

   const auto count = delta_format ? 0 : r.u16(Offset + 4);
   const uint32_t sub_header = 6 + uint32_t(count) * 2;
   if (not r.has(Offset, sub_header)) return false;
   const auto coverage = child(r, Offset, r.u16(Offset + 2), sub_header);
   const auto covered = coverage_size(r, coverage, glyph_count, Budget);
   if (r.bad or (covered < 0)) return false;

   if (delta_format) {
      // Validate every result, including modular negative deltas.

      const auto delta = r.s16(Offset + 4);
      if (uint32_t(covered) > Budget) return false;
      Budget -= covered;
      const auto coverage_format = r.u16(coverage), ranges = r.u16(coverage + 2);
      for (uint32_t c=0; c < ranges; c++) {
         const auto rec = coverage + 4 + c * ((coverage_format IS 1) ? 2 : 6);
         const uint32_t first = r.u16(rec);
         const uint32_t end = (coverage_format IS 1) ? first : r.u16(rec + 2);
         for (uint32_t glyph=first; glyph <= end; glyph++) {
            if (uint16_t(int(glyph) + delta) >= glyph_count) return false;
         }
      }
      return not r.bad;
   }

   if ((covered != count) or (uint32_t(count) > Budget)) return false;
   Budget -= count;
   for (uint32_t c=0; c < uint32_t(count); c++) {
      const auto value = r.u16(Offset + 6 + c * 2);
      if (Type IS LOOKUP_SINGLE) {
         if (value >= glyph_count) return false;
         continue;
      }

      // Sequence, AlternateSet and LigatureSet tables share a counted array of 16-bit entries.  Only a Sequence
      // can be empty, which deletes the glyph.

      const auto set = child(r, Offset, value, sub_header);
      const auto entries = r.u16(set);
      if (((not entries) and (Type != LOOKUP_MULTIPLE)) or (entries > Budget) or
          (not r.has(set + 2, uint64_t(entries) * 2))) return false;
      Budget -= entries;
      for (uint32_t a=0; a < entries; a++) {
         const auto entry = r.u16(set + 2 + a * 2);
         if (Type != LOOKUP_LIGATURE) {
            if (entry >= glyph_count) return false;
            continue;
         }

         const auto ligature = child(r, set, entry, 2 + uint32_t(entries) * 2);
         const auto components = r.u16(ligature + 2);
         if ((r.u16(ligature) >= glyph_count) or (components < 2) or (components > Budget) or
             (not r.has(ligature + 4, uint64_t(components - 1) * 2))) return false;
         Budget -= components;
         for (uint32_t c=0; c < uint32_t(components - 1); c++) {
            if (r.u16(ligature + 4 + c * 2) >= glyph_count) return false;
         }
      }
   }
   return not r.bad;
}

//********************************************************************************************************************

bool gsub_table::validate_gdef()
{
   if (gdef.empty()) {
      for (const auto &lookup : lookups) if (lookup.flags & LF_USE_MARK_FILTERING_SET) return false;
      return true;
   }

   const reader r(gdef);
   const auto minor = r.u16(2);
   if ((r.u16(0) != 1) or ((minor != 0) and (minor != 2) and (minor != 3))) return false;
   const uint32_t header = (minor IS 3) ? 18 : ((minor IS 2) ? 14 : 12);
   if (not r.has(0, header)) return false;

   uint32_t budget = MAX_RECORDS;
   if (r.u16(4)) {
      glyph_classes = child(r, 0, r.u16(4), header);
      if (not valid_classes(r, glyph_classes, glyph_count, ot_reader::GLYPH_CLASS_COMPONENT, budget)) return false;
   }

   if (r.u16(10)) {
      mark_classes = child(r, 0, r.u16(10), header);
      if (not valid_classes(r, mark_classes, glyph_count, 255, budget)) return false;
   }

   if ((minor >= 2) and r.u16(12)) {
      mark_sets = child(r, 0, r.u16(12), header);
      const auto count = r.u16(mark_sets + 2);
      if ((r.u16(mark_sets) != 1) or (not r.has(mark_sets + 4, uint64_t(count) * 4))) return false;
      for (uint32_t i=0; i < count; i++) {
         const auto coverage = child(r, mark_sets, r.u32(mark_sets + 4 + i * 4), 4 + uint32_t(count) * 4);
         if (coverage_size(r, coverage, glyph_count, budget) < 0) return false;
      }
   }

   for (const auto &lookup : lookups) {
      if ((lookup.flags & LF_USE_MARK_FILTERING_SET) and
          ((not mark_sets) or (lookup.mark_set >= r.u16(mark_sets + 2)))) return false;
   }
   return not r.bad;
}

//********************************************************************************************************************
// Returns the lookups that a feature string enables for a script, in lookup list order.  Scripts that the font does
// not define fall back to DFLT.  When several features share a lookup, the highest requested value is used.

const std::vector<gsub_table::selection> & gsub_table::select(uint32_t Script, std::string_view Features)
{
   auto key = std::make_pair(Script, std::string(Features));
   if (const auto found = selections.find(key); found != selections.end()) return found->second;

   // Bound the cache, as clients can supply arbitrary feature strings.

   if (selections.size() >= MAX_SELECTIONS) selections.clear();

   auto &selected = selections[std::move(key)];
   auto script = scripts.find(Script);
   if (script IS scripts.end()) script = scripts.find(ot_reader::tag('D','F','L','T'));
   if (script IS scripts.end()) return selected;

   std::vector<uint32_t> values(lookups.size());
   for (const auto index : script->second) {
      const auto &feature = features[index];
      const auto value = ot_reader::feature_value(Features, feature.tag).value_or(0);
      if (not value) continue;
      for (const auto lookup : feature.lookups) values[lookup] = std::max(values[lookup], value);
   }

   for (uint32_t i=0; i < values.size(); i++) {
      if (values[i] and (not lookups[i].subtables.empty())) selected.push_back({ uint16_t(i), values[i] });
   }
   return selected;
}

//********************************************************************************************************************
// Applies the selected lookups in order.  Each lookup makes one pass over the glyphs, and at each position applies
// only the first subtable that matches.  Work is metered so that hostile fonts cannot cause excessive processing:
// exhausting the budget stops substitution and leaves the glyphs as they are at that point.

void gsub_table::apply(std::vector<shaped_glyph> &Glyphs, uint32_t Script, std::string_view Features,
   size_t *Budget, size_t *GrowthBudget)
{
   if (Features.empty() or (Features IS "none") or (not enabled())) return;
   const auto &selected = select(Script, Features);
   if (selected.empty()) return;

   const reader r(gsub), def(gdef);
   size_t local_budget = work_budget(Glyphs.size());
   size_t local_growth = growth_budget(Glyphs.size());
   auto &budget = Budget ? *Budget : local_budget;
   auto &growth = GrowthBudget ? *GrowthBudget : local_growth;

   auto classify = [&](shaped_glyph &Glyph) {
      Glyph.mark_class = glyph_classes ? uint8_t(glyph_class(def, glyph_classes, Glyph.glyph)) : 0;
   };

   auto skip = [&](size_t Index, const lookup &Lookup) {
      return (Glyphs[Index].glyph IS shaped_glyph::INVISIBLE) or
         ot_reader::skip_glyph(def, Lookup.flags, Lookup.mark_set, Glyphs[Index].glyph,
            glyph_classes, mark_classes, mark_sets);
   };

   auto spend = [&](size_t Cost) {
      if (Cost > budget) { budget = 0; return false; }
      budget -= Cost;
      return true;
   };

   // Multiple substitution.  An empty sequence deletes the glyph, leaving a zero-width source caret.

   auto substitute_multiple = [&](size_t &Index, uint32_t Set) {
      const auto count = r.u16(Set);
      if (not count) {
         Glyphs[Index].glyph = shaped_glyph::INVISIBLE;
         Glyphs[Index].mark_class = 0;
         return MATCH::APPLIED;
      }

      // Insertions can move the entire tail.  Charge that work as well as limiting total growth.

      const auto cost = (count > 1) ? Glyphs.size() - Index + count : 1;
      if ((size_t(count - 1) > growth) or (not spend(cost))) {
         budget = 0;
         return MATCH::HALT;
      }

      const auto source = Glyphs[Index];
      Glyphs.insert(Glyphs.begin() + Index + 1, count - 1, source);
      growth -= count - 1;
      for (uint32_t c=0; c < count; c++) {
         auto &glyph = Glyphs[Index + c];
         glyph.glyph = r.u16(Set + 2 + c * 2);
         if (c) glyph.codepoints = 0;
         classify(glyph);
      }
      Index += count - 1; // A lookup never reapplies to its own multiple-substitution outputs.
      return MATCH::APPLIED;
   };

   // Ligature substitution.  LigatureSet order defines preference, rather than longest-match order.

   auto substitute_ligature = [&](size_t Index, uint32_t Set, const lookup &Lookup) {
      const auto count = r.u16(Set);
      for (uint32_t l=0; l < count; l++) {
         if (not spend(1)) return MATCH::HALT;
         const auto ligature = Set + r.u16(Set + 2 + l * 2);
         const auto components = r.u16(ligature + 2);
         std::vector<size_t> positions { Index };
         size_t next = Index + 1;
         for (uint32_t c=1; c < components; c++) {
            while (next < Glyphs.size()) {
               if (not spend(1)) return MATCH::HALT;
               if (not skip(next, Lookup)) break;
               next++;
            }
            if ((next IS Glyphs.size()) or (Glyphs[next].glyph != r.u16(ligature + 2 + c * 2))) break;
            positions.push_back(next++);
         }
         if (positions.size() != components) continue;
         if (not spend(Glyphs.size() - Index + next - Index)) return MATCH::HALT;

         // Merge the complete source span, including ignored marks and surviving siblings from a multiple
         // substitution.  Only the first glyph owns the count, so repeated merges cannot double-count a codepoint
         // or produce decreasing cluster offsets.

         size_t first = Index, end = next;
         while (first and (Glyphs[first - 1].cluster IS Glyphs[Index].cluster)) {
            if (not spend(1)) return MATCH::HALT;
            first--;
         }

         while ((end < Glyphs.size()) and (Glyphs[end].cluster IS Glyphs[next - 1].cluster)) {
            if (not spend(1)) return MATCH::HALT;
            end++;
         }

         if (not spend(end - first)) return MATCH::HALT;
         uint32_t codepoints = 0;
         const auto cluster = Glyphs[first].cluster;
         const auto unicode = Glyphs[first].unicode;
         for (size_t c=first; c < end; c++) {
            codepoints += Glyphs[c].codepoints;
            Glyphs[c].codepoints = 0;
            Glyphs[c].cluster = cluster;
            Glyphs[c].unicode = unicode;
         }
         Glyphs[first].codepoints = codepoints;
         Glyphs[Index].glyph = r.u16(ligature);
         classify(Glyphs[Index]);

         // Compact once; erasing each component separately would allow quadratic work.

         size_t output = Index + 1, component = 1;
         for (size_t c=Index + 1; c < Glyphs.size(); c++) {
            if ((component < positions.size()) and (c IS positions[component])) component++;
            else Glyphs[output++] = Glyphs[c];
         }
         Glyphs.resize(output);
         return MATCH::APPLIED;
      }
      return MATCH::NONE;
   };

   for (auto &glyph : Glyphs) classify(glyph);

   for (const auto &selection : selected) {
      const auto &lookup = lookups[selection.lookup];
      for (size_t i=0; i < Glyphs.size(); i++) {
         if (not spend(1)) return;
         if (skip(i, lookup)) continue;
         for (const auto &sub : lookup.subtables) {
            if (not spend(1)) return;
            const auto index = coverage_index(r, sub.offset + r.u16(sub.offset + 2), Glyphs[i].glyph);
            if (index < 0) continue;

            if (sub.type IS LOOKUP_SINGLE) {
               if (r.u16(sub.offset) IS 1) Glyphs[i].glyph = uint16_t(int(Glyphs[i].glyph) + r.s16(sub.offset + 4));
               else Glyphs[i].glyph = r.u16(sub.offset + 6 + uint32_t(index) * 2);
               classify(Glyphs[i]);
               break;
            }

            const auto set = sub.offset + r.u16(sub.offset + 6 + uint32_t(index) * 2);
            auto result = MATCH::APPLIED;
            if (sub.type IS LOOKUP_ALTERNATE) {
               if (selection.value <= r.u16(set)) Glyphs[i].glyph = r.u16(set + 2 + (selection.value - 1) * 2);
               classify(Glyphs[i]);
            }
            else if (sub.type IS LOOKUP_MULTIPLE) result = substitute_multiple(i, set);
            else result = substitute_ligature(i, set, lookup);

            if (result IS MATCH::HALT) return;
            if (result IS MATCH::APPLIED) break; // Only the first matching subtable applies at a position.
            // Otherwise another subtable can still match this position.
         }
      }
   }
}
