// GSUB substitution reader.  See gsub.h for an overview.
//
// References: OpenType specification, chapters 'GSUB', 'GDEF' and 'OpenType Layout Common Table Formats'.

#include "gsub.h"
#include "opentype.h"
#include <algorithm>
#include <functional>

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
constexpr uint16_t LOOKUP_CONTEXT   = 5;
constexpr uint16_t LOOKUP_CHAINED   = 6;
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
   variations.clear();
   glyph_classes = mark_classes = mark_sets = 0;
   if ((gsub.size() > MAX_TABLE) or (gdef.size() > MAX_TABLE) or (not glyph_count) or
       (glyph_count > 65536) or (not validate()) or (not validate_gdef())) {
      lookups.clear();
      features.clear();
      scripts.clear();
      variations.clear();
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

         if (real_type > LOOKUP_CHAINED) continue;
         lookup.subtables.push_back({ sub, real_type });
         if (real_type >= LOOKUP_CONTEXT) {
            if (not validate_context(lookup.subtables.back(), budget)) return false;
         }
         else if (not validate_subtable(sub, real_type, budget)) return false;
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
   if ((header IS 14) and r.u32(10)) {
      if (not validate_variations(child(r, 0, r.u32(10), header), budget)) return false;
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
// Context formats share counted pattern arrays and sequence lookup records.  Keep offsets into the validated table
// rather than copying coverage sets or expanding class zero into a potentially large glyph list.

bool gsub_table::validate_context(subtable &Sub, uint32_t &Budget)
{
   const reader r(gsub);
   const auto offset = Sub.offset;
   const bool chained = Sub.type IS LOOKUP_CHAINED;
   const auto format = Sub.format = r.u16(offset);
   if ((format < 1) or (format > 3)) return false;

   auto array = [&](uint32_t &Cursor, uint32_t Count) {
      if ((Count > Budget) or (not r.has(Cursor, uint64_t(Count) * 2))) return false;
      Budget -= Count;
      Cursor += Count * 2;
      return true;
   };

   auto actions = [&](context_rule &Rule, uint32_t Cursor, uint16_t Count) {
      if ((Count > Budget) or (not r.has(Cursor, uint64_t(Count) * 4))) return false;
      Budget -= Count;
      Rule.actions.reserve(Count);
      for (uint32_t i=0; i < Count; i++) {
         const auto sequence = r.u16(Cursor + i * 4), lookup = r.u16(Cursor + i * 4 + 2);
         if (lookup >= lookups.size()) return false;
         Rule.actions.push_back({ sequence, lookup });
      }
      return not r.bad;
   };

   // Reads one rule's pattern arrays.  Format 3 includes the initial input glyph; the other formats use the
   // coverage/class set index for it.  Backtrack arrays are already in nearest-first order in the font.

   auto pattern = [&](context_rule &Rule, uint32_t &Cursor, bool Coverage) {
      if (chained) {
         Rule.backtrack_count = r.u16(Cursor);
         Rule.backtrack = Cursor += 2;
         if (not array(Cursor, Rule.backtrack_count)) return false;
      }
      Rule.input_count = r.u16(Cursor);
      Rule.input = Cursor += 2;
      if (not Rule.input_count) return false;
      if (not array(Cursor, Rule.input_count - (Coverage ? 0 : 1))) return false;
      if (chained) {
         Rule.lookahead_count = r.u16(Cursor);
         Rule.lookahead = Cursor += 2;
         if (not array(Cursor, Rule.lookahead_count)) return false;
      }
      return not r.bad;
   };

   if (format IS 3) {
      context_rule rule;
      uint32_t cursor = offset + 2;
      uint16_t count;
      if (chained) {
         if (not pattern(rule, cursor, true)) return false;
         count = r.u16(cursor);
         cursor += 2;
      }
      else {
         rule.input_count = r.u16(cursor);
         count = r.u16(cursor + 2);
         rule.input = cursor += 4;
         if ((not rule.input_count) or (not array(cursor, rule.input_count))) return false;
      }
      if (not actions(rule, cursor, count)) return false;
      const auto header = cursor + uint32_t(count) * 4 - offset;
      for (auto [start, length] : { std::pair(rule.backtrack, rule.backtrack_count),
            std::pair(rule.input, rule.input_count), std::pair(rule.lookahead, rule.lookahead_count) }) {
         for (uint32_t i=0; i < length; i++) {
            if (coverage_size(r, child(r, offset, r.u16(start + i * 2), header), glyph_count, Budget) < 0) {
               return false;
            }
         }
      }
      Sub.rules.push_back(std::move(rule));
      return not r.bad;
   }

   const uint32_t base_header = (format IS 1) ? 6 : (chained ? 12 : 8);
   const auto count = r.u16(offset + base_header - 2);
   const auto header = base_header + uint32_t(count) * 2;
   if (not r.has(offset, header)) return false;
   Sub.coverage = child(r, offset, r.u16(offset + 2), header);
   const auto covered = coverage_size(r, Sub.coverage, glyph_count, Budget);
   if ((covered < 0) or ((format IS 1) and (covered != count))) return false;
   if (format IS 2) {
      uint32_t cursor = offset + 4;
      if (chained) {
         Sub.backtrack_classes = child(r, offset, r.u16(cursor), header);
         cursor += 2;
         if (not valid_classes(r, Sub.backtrack_classes, glyph_count, 65535, Budget)) return false;
      }
      Sub.input_classes = child(r, offset, r.u16(cursor), header);
      if (not valid_classes(r, Sub.input_classes, glyph_count, 65535, Budget)) return false;
      if (chained) {
         Sub.lookahead_classes = child(r, offset, r.u16(cursor + 2), header);
         if (not valid_classes(r, Sub.lookahead_classes, glyph_count, 65535, Budget)) return false;
      }
   }
   if (count > Budget) return false;
   Budget -= count;
   Sub.rule_sets.resize(count);
   for (uint32_t i=0; i < count; i++) {
      const auto relative = r.u16(offset + base_header + i * 2);
      if (not relative) continue; // Null rule sets are explicitly allowed.
      const auto set = child(r, offset, relative, header);
      const auto rules = r.u16(set);
      if ((rules > Budget) or (not r.has(set + 2, uint64_t(rules) * 2))) return false;
      Budget -= rules;
      Sub.rule_sets[i] = { uint32_t(Sub.rules.size()), rules };
      for (uint32_t j=0; j < rules; j++) {
         context_rule rule;
         uint32_t cursor = child(r, set, r.u16(set + 2 + j * 2), 2 + uint32_t(rules) * 2);
         uint16_t action_count;
         if (chained) {
            if (not pattern(rule, cursor, false)) return false;
            action_count = r.u16(cursor);
            cursor += 2;
         }
         else {
            rule.input_count = r.u16(cursor);
            action_count = r.u16(cursor + 2);
            rule.input = cursor += 4;
            if ((not rule.input_count) or (not array(cursor, rule.input_count - 1))) return false;
         }
         if (not actions(rule, cursor, action_count)) return false;
         if (format IS 1) {
            for (auto [start, length] : { std::pair(rule.backtrack, rule.backtrack_count),
                  std::pair(rule.input, uint16_t(rule.input_count - 1)),
                  std::pair(rule.lookahead, rule.lookahead_count) }) {
               for (uint32_t k=0; k < length; k++) if (r.u16(start + k * 2) >= glyph_count) return false;
            }
         }
         Sub.rules.push_back(std::move(rule));
      }
   }
   return not r.bad;
}

//********************************************************************************************************************

bool gsub_table::validate_variations(uint32_t Offset, uint32_t &Budget)
{
   const reader r(gsub);
   const auto count = r.u32(Offset + 4);
   if ((r.u16(Offset) != 1) or r.u16(Offset + 2) or (count > Budget) or
       (not r.has(Offset + 8, uint64_t(count) * 8))) return false;
   Budget -= count;
   variations.reserve(count);
   for (uint32_t i=0; i < count; i++) {
      variation entry;
      const auto rec = Offset + 8 + i * 8;
      if (r.u32(rec)) {
         const auto set = child(r, Offset, r.u32(rec), 8 + count * 8);
         const auto conditions = r.u16(set);
         if ((conditions > Budget) or (not r.has(set + 2, uint64_t(conditions) * 4))) return false;
         Budget -= conditions;
         for (uint32_t c=0; c < conditions; c++) {
            const auto condition = child(r, set, r.u32(set + 2 + c * 4), 2 + uint32_t(conditions) * 4);
            if (r.u16(condition) != 1) { entry.supported = false; continue; }
            const auto minimum = r.s16(condition + 4), maximum = r.s16(condition + 6);
            if ((minimum < -16384) or (maximum > 16384) or (minimum > maximum)) return false;
            entry.conditions.push_back({ r.u16(condition + 2), minimum, maximum });
         }
      }
      if (r.u32(rec + 4)) {
         const auto substitution = child(r, Offset, r.u32(rec + 4), 8 + count * 8);
         if ((r.u16(substitution) != 1) or r.u16(substitution + 2)) entry.supported = false;
         else {
            const auto replacements = r.u16(substitution + 4);
            if ((replacements > Budget) or (not r.has(substitution + 6, uint64_t(replacements) * 6))) return false;
            Budget -= replacements;
            int previous = -1;
            for (uint32_t f=0; f < replacements; f++) {
               const auto record = substitution + 6 + f * 6;
               const auto index = r.u16(record);
               if ((index <= previous) or (index >= features.size())) return false;
               previous = index;
               const auto alternate = child(r, substitution, r.u32(record + 2), 6 + uint32_t(replacements) * 6);
               const auto length = r.u16(alternate + 2);
               if ((length > Budget) or (not r.has(alternate + 4, uint64_t(length) * 2))) return false;
               Budget -= length;
               if (r.u16(alternate)) {
                  child(r, alternate, r.u16(alternate), 4 + uint32_t(length) * 2);
               }
               auto &indices = entry.replacements[index];
               for (uint32_t l=0; l < length; l++) {
                  const auto lookup = r.u16(alternate + 4 + l * 2);
                  if (lookup >= lookups.size()) return false;
                  indices.push_back(lookup);
               }
            }
         }
      }
      variations.push_back(std::move(entry));
   }
   return not r.bad;
}

//********************************************************************************************************************
// Only the first matching variation record applies.  Unknown conditions and invalid axis indices fail that record.

int gsub_table::variation_index(std::span<const int> Coordinates, size_t &Budget) const
{
   for (size_t i=0; i < variations.size(); i++) {
      if (not Budget) return -1;
      Budget--;
      const auto &entry = variations[i];
      if (not entry.supported) continue;
      bool matches = true;
      for (const auto &condition : entry.conditions) {
         if (not Budget) return -1;
         Budget--;
         if ((condition.axis >= Coordinates.size()) or (Coordinates[condition.axis] < condition.minimum) or
             (Coordinates[condition.axis] > condition.maximum)) { matches = false; break; }
      }
      if (matches) return int(i);
   }
   return -1;
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

const std::vector<gsub_table::selection> & gsub_table::select(uint32_t Script, std::string_view Features, int Variation)
{
   auto key = std::make_tuple(Script, std::string(Features), Variation);
   if (const auto found = selections.find(key); found != selections.end()) return found->second;

   // Bound the cache, as clients can supply arbitrary feature strings.

   if (selections.size() >= MAX_SELECTIONS) selections.clear();

   auto &selected = selections[std::move(key)];
   auto script = scripts.find(Script);
   if (script IS scripts.end()) script = scripts.find(ot_reader::tag('D','F','L','T'));
   if (script IS scripts.end()) return selected;

   // Required variation alternates precede composition/decomposition, which precedes ordinary features.
   // Shared lookups run in their earliest enabled pass, using the highest requested value across all features.
   // Each pass retains LookupList order, even when the earlier pass references greater lookup indices.

   std::vector<uint32_t> values(lookups.size());
   std::vector<int> passes(lookups.size(), 3);
   for (const auto index : script->second) {
      const auto &feature = features[index];
      const auto value = ot_reader::feature_value(Features, feature.tag).value_or(0);
      if (not value) continue;
      const int feature_pass = (feature.tag IS ot_reader::tag('r','v','r','n')) ? 0 :
         (feature.tag IS ot_reader::tag('c','c','m','p')) ? 1 : 2;
      const auto *indices = &feature.lookups;
      if (Variation >= 0) {
         const auto &replacements = variations[Variation].replacements;
         if (const auto found = replacements.find(index); found != replacements.end()) indices = &found->second;
      }
      for (const auto lookup : *indices) {
         values[lookup] = std::max(values[lookup], value);
         passes[lookup] = std::min(passes[lookup], feature_pass);
      }
   }
   for (int pass=0; pass < 3; pass++) {
      for (uint32_t i=0; i < values.size(); i++) {
         if ((passes[i] IS pass) and (not lookups[i].subtables.empty())) {
            selected.push_back({ uint16_t(i), values[i] });
         }
      }
   }
   return selected;
}

//********************************************************************************************************************
// Applies the selected lookups in order.  Each lookup makes one pass over the glyphs, and at each position applies
// only the first subtable that matches.  Work is metered so that hostile fonts cannot cause excessive processing:
// exhausting the budget stops substitution and leaves the glyphs as they are at that point.

void gsub_table::apply(std::vector<shaped_glyph> &Glyphs, uint32_t Script, std::string_view Features,
   size_t *Budget, size_t *GrowthBudget, std::span<const int> Coordinates)
{
   if (Features.empty() or (Features IS "none") or (not enabled())) return;
   const reader r(gsub), def(gdef);
   size_t local_budget = work_budget(Glyphs.size());
   size_t local_growth = growth_budget(Glyphs.size());
   auto &budget = Budget ? *Budget : local_budget;
   auto &growth = GrowthBudget ? *GrowthBudget : local_growth;
   const auto variant = variation_index(Coordinates, budget);
   if (not budget) return;
   const auto &selected = select(Script, Features, variant);
   if (selected.empty()) return;

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

   // Sequence indices address the current matched input.  Multiple outputs add sequence positions; ligatures and
   // deletions remove them.  Keep buffer positions for ignored glyphs accurate in every enclosing contextual match.

   struct context_match { std::vector<size_t> positions; size_t end; };
   std::vector<context_match *> active;
   auto track_growth = [&](size_t Index, size_t Count) {
      size_t cost = 0;
      for (auto frame : active) cost += frame->positions.size() + Count + 1;
      if (not spend(cost)) return false;
      for (auto frame : active) {
         auto &positions = frame->positions;
         const auto found = std::lower_bound(positions.begin(), positions.end(), Index);
         const auto insertion = size_t(found - positions.begin()) + 1;
         const bool included = (found != positions.end()) and (*found IS Index);
         for (auto &position : positions) if (position > Index) position += Count;
         if (included) {
            positions.insert(positions.begin() + insertion, Count, 0);
            for (size_t c=0; c < Count; c++) positions[insertion + c] = Index + c + 1;
         }
         if (frame->end > Index) frame->end += Count;
      }
      return true;
   };
   auto track_deletion = [&](size_t Index) {
      size_t cost = 0;
      for (auto frame : active) cost += frame->positions.size();
      if (not spend(cost)) return false;
      for (auto frame : active) std::erase(frame->positions, Index);
      return true;
   };
   auto track_ligature = [&](const std::vector<size_t> &Positions) {
      size_t cost = 0;
      for (auto frame : active) cost += (frame->positions.size() + 1) * 17; // At most 16 binary-search steps
      if (not spend(cost)) return false;
      for (auto frame : active) {
         size_t output = 0;
         for (auto position : frame->positions) {
            const auto found = std::lower_bound(Positions.begin() + 1, Positions.end(), position);
            if ((found != Positions.end()) and (*found IS position)) continue;
            frame->positions[output++] = position - size_t(found - Positions.begin() - 1);
         }
         frame->positions.resize(output);
         frame->end -= size_t(std::lower_bound(Positions.begin() + 1, Positions.end(), frame->end) -
            Positions.begin() - 1);
      }
      return true;
   };

   // Multiple substitution.  An empty sequence deletes the glyph, leaving a zero-width source caret.

   auto substitute_multiple = [&](size_t &Index, uint32_t Set) {
      const auto count = r.u16(Set);
      if (not count) {
         if (not track_deletion(Index)) return MATCH::HALT;
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

      if ((count > 1) and (not track_growth(Index, count - 1))) return MATCH::HALT;
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
         if (not track_ligature(positions)) return MATCH::HALT;
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

   std::function<MATCH(uint16_t, size_t &, uint32_t, unsigned)> apply_lookup;

   auto contextual = [&](const subtable &Sub, const lookup &Lookup, size_t &Index, uint32_t Value, unsigned Depth) {
      uint32_t first_rule = 0, rule_count = Sub.rules.size();
      if (Sub.format != 3) {
         auto first = coverage_index(r, Sub.coverage, Glyphs[Index].glyph);
         if (first < 0) return MATCH::NONE;
         if (Sub.format IS 2) first = glyph_class(r, Sub.input_classes, Glyphs[Index].glyph);
         if (size_t(first) >= Sub.rule_sets.size()) return MATCH::NONE;
         std::tie(first_rule, rule_count) = Sub.rule_sets[first];
      }
      for (uint32_t rule_index=first_rule; rule_index < first_rule + rule_count; rule_index++) {
         if (not spend(1)) return MATCH::HALT;
         const auto &rule = Sub.rules[rule_index];
         context_match match { { Index }, Index + 1 };

         auto matches = [&](uint32_t Array, uint32_t Item, size_t Position, uint32_t Classes) {
            const auto expected = r.u16(Array + Item * 2);
            if (Sub.format IS 3) return coverage_index(r, Sub.offset + expected, Glyphs[Position].glyph) >= 0;
            if (Sub.format IS 2) return glyph_class(r, Classes, Glyphs[Position].glyph) IS expected;
            return Glyphs[Position].glyph IS expected;
         };
         auto advance = [&](size_t &Position, bool Backward) {
            while (true) {
               if (not spend(1)) return false;
               if (Backward) { if (not Position) return false; Position--; }
               else { if (++Position >= Glyphs.size()) return false; }
               if (not skip(Position, Lookup)) return true;
            }
         };

         if ((Sub.format IS 3) and (not matches(rule.input, 0, Index, 0))) continue;
         size_t position = Index;
         bool okay = true;
         for (uint32_t c=1; c < rule.input_count; c++) {
            if ((not advance(position, false)) or
                (not matches(rule.input, c - ((Sub.format IS 3) ? 0 : 1), position, Sub.input_classes))) {
               okay = false;
               break;
            }
            match.positions.push_back(position);
         }
         if (not okay) continue;
         match.end = position + 1;
         for (uint32_t c=0; c < rule.lookahead_count; c++) {
            if ((not advance(position, false)) or (not matches(rule.lookahead, c, position, Sub.lookahead_classes))) {
               okay = false;
               break;
            }
         }
         if (not okay) continue;
         position = Index;
         for (uint32_t c=0; c < rule.backtrack_count; c++) {
            if ((not advance(position, true)) or (not matches(rule.backtrack, c, position, Sub.backtrack_classes))) {
               okay = false;
               break;
            }
         }
         if (not okay) continue;

         active.push_back(&match);
         auto result = MATCH::APPLIED; // A matching rule with no actions still blocks subsequent rules.
         for (const auto &action : rule.actions) {
            if (not spend(1)) { result = MATCH::HALT; break; }
            if (action.sequence >= match.positions.size()) continue;
            auto target = match.positions[action.sequence];
            if (apply_lookup(action.lookup, target, Value, Depth + 1) IS MATCH::HALT) {
               result = MATCH::HALT;
               break;
            }
         }
         active.pop_back();
         Index = match.end - 1; // Continue beyond the matched input, including any inserted outputs.
         return result;
      }
      return budget ? MATCH::NONE : MATCH::HALT;
   };

   apply_lookup = [&](uint16_t LookupIndex, size_t &Index, uint32_t Value, unsigned Depth) {
      if (not spend(1)) return MATCH::HALT;
      if (Depth > 6) { budget = 0; return MATCH::HALT; }
      const auto &lookup = lookups[LookupIndex];
      if ((Index >= Glyphs.size()) or skip(Index, lookup)) return MATCH::NONE;
      for (const auto &sub : lookup.subtables) {
         if (not spend(1)) return MATCH::HALT;
         if (sub.type >= LOOKUP_CONTEXT) {
            const auto result = contextual(sub, lookup, Index, Value, Depth);
            if (result != MATCH::NONE) return result;
            continue;
         }
         const auto index = coverage_index(r, sub.offset + r.u16(sub.offset + 2), Glyphs[Index].glyph);
         if (index < 0) continue;
         if (sub.type IS LOOKUP_SINGLE) {
            if (r.u16(sub.offset) IS 1) {
               Glyphs[Index].glyph = uint16_t(int(Glyphs[Index].glyph) + r.s16(sub.offset + 4));
            }
            else Glyphs[Index].glyph = r.u16(sub.offset + 6 + uint32_t(index) * 2);
            classify(Glyphs[Index]);
            return MATCH::APPLIED;
         }
         const auto set = sub.offset + r.u16(sub.offset + 6 + uint32_t(index) * 2);
         if (sub.type IS LOOKUP_ALTERNATE) {
            if (Value <= r.u16(set)) Glyphs[Index].glyph = r.u16(set + 2 + (Value - 1) * 2);
            classify(Glyphs[Index]);
            return MATCH::APPLIED;
         }
         const auto result = (sub.type IS LOOKUP_MULTIPLE) ? substitute_multiple(Index, set) :
            substitute_ligature(Index, set, lookup);
         if (result != MATCH::NONE) return result;
      }
      return MATCH::NONE;
   };

   for (const auto &selection : selected) {
      for (size_t i=0; i < Glyphs.size(); i++) {
         if (apply_lookup(selection.lookup, i, selection.value, 0) IS MATCH::HALT) return;
      }
   }
}
