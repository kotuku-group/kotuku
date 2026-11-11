// MarkToBase positioning shares the face's GPOS/GDEF storage and variation resolver with pair kerning.

#include "kerning.h"
#include "text_shaping.h"
#include "opentype.h"

#include <algorithm>

namespace {

using namespace ot_reader;

constexpr size_t MAX_RECORDS = 1000000;

uint32_t child(const reader &R, uint32_t Parent, uint32_t Offset, uint32_t Minimum)
{
   const auto target = uint64_t(Parent) + Offset;
   if ((Offset < Minimum) or (target > 0xffffffff) or (not R.has(target, 2))) {
      R.bad = true;
      return 0;
   }
   return uint32_t(target);
}

bool consume(size_t &Budget, size_t Count = 1)
{
   if (Count > Budget) return false;
   Budget -= Count;
   return true;
}

// Mark/base array indices require ordered, contiguous coverage indices, not merely in-bounds records.

bool coverage(const reader &R, uint32_t Offset, uint32_t Expected, size_t &Budget)
{
   if (not validate_coverage(R, Offset)) return false;
   const auto format = R.u16(Offset), count = R.u16(Offset + 2);
   if (not consume(Budget, count)) return false;
   uint32_t total = 0, previous = 0;
   for (uint32_t i=0; i < count; i++) {
      const auto rec = Offset + 4 + i * ((format IS 1) ? 2 : 6);
      const uint32_t first = R.u16(rec), last = (format IS 1) ? first : R.u16(rec + 2);
      if ((first > last) or (i and (first <= previous))) return false;
      if ((format IS 2) and (R.u16(rec + 4) != total)) return false;
      total += last - first + 1;
      previous = last;
   }
   return (not R.bad) and (total IS Expected);
}

bool anchor(const reader &R, uint32_t Offset, size_t &Budget)
{
   if (not consume(Budget)) return false;
   const auto format = R.u16(Offset);
   if ((format < 1) or (format > 3) or (not R.has(Offset, (format IS 1) ? 6 : (format IS 2) ? 8 : 10))) {
      return false;
   }
   if (format != 3) return true;
   for (auto field : { 6u, 8u }) {
      if (const auto relative = R.u16(Offset + field)) {
         const auto device = child(R, Offset, relative, 10);
         if (not R.has(device, 6)) return false;
         const auto type = R.u16(device + 4);
         if (type IS 0x8000) continue; // VariationIndex; unknown indices resolve to zero
         if ((type < 1) or (type > 3)) return false;
         const auto first = R.u16(device), last = R.u16(device + 2);
         if (last < first) return false;
         const uint32_t per_word = 16u >> type;
         const uint32_t words = (uint32_t(last) - first + per_word) / per_word;
         if (not R.has(uint64_t(device) + 6, uint64_t(words) * 2)) return false;
      }
   }
   return not R.bad;
}

bool subtable(const reader &R, uint32_t Offset, size_t &Budget)
{
   if ((R.u16(Offset) != 1) or (not R.has(Offset, 12))) return false;
   const auto classes = R.u16(Offset + 6);
   if (not classes) return false;
   const auto marks = child(R, Offset, R.u16(Offset + 8), 12);
   const auto bases = child(R, Offset, R.u16(Offset + 10), 12);
   const auto mark_count = R.u16(marks), base_count = R.u16(bases);
   const uint64_t base_records = uint64_t(base_count) * classes;
   if (R.bad or (not consume(Budget, mark_count)) or (not consume(Budget, base_records)) or
       (not R.has(uint64_t(marks) + 2, uint64_t(mark_count) * 4)) or
       (not R.has(uint64_t(bases) + 2, base_records * 2))) return false;
   if ((not coverage(R, child(R, Offset, R.u16(Offset + 2), 12), mark_count, Budget)) or
       (not coverage(R, child(R, Offset, R.u16(Offset + 4), 12), base_count, Budget))) return false;
   for (uint32_t i=0; i < mark_count; i++) {
      const auto rec = marks + 2 + i * 4;
      if ((R.u16(rec) >= classes) or
          (not anchor(R, child(R, marks, R.u16(rec + 2), 2 + uint32_t(mark_count) * 4), Budget))) return false;
   }
   for (uint32_t i=0; i < base_records; i++) {
      if (const auto relative = R.u16(bases + 2 + i * 2)) {
         if (not anchor(R, child(R, bases, relative, 2 + uint32_t(base_records) * 2), Budget)) return false;
      }
   }
   return not R.bad;
}

} // namespace

//********************************************************************************************************************
// Retain each supported script's default language system, including an empty one (which prevents DFLT fallback).

bool gpos_kerning::parse_marks()
{
   const reader r(gpos);
   if ((r.u16(0) != 1) or (r.u16(2) > 1) or (not r.has(0, (r.u16(2) IS 1) ? 14 : 10))) return false;
   const auto scripts = child(r, 0, r.u16(4), 10);
   const auto features = child(r, 0, r.u16(6), 10);
   const auto list = child(r, 0, r.u16(8), 10);
   const auto script_count = r.u16(scripts), feature_count = r.u16(features), lookup_count = r.u16(list);
   if ((lookup_count > 1024) or (not r.has(scripts + 2, uint64_t(script_count) * 6)) or
       (not r.has(features + 2, uint64_t(feature_count) * 6)) or
       (not r.has(list + 2, uint64_t(lookup_count) * 2))) return false;
   size_t budget = MAX_RECORDS;
   size_t total_subtables = 0;
   for (uint32_t s=0; s < script_count; s++) {
      const auto rec = scripts + 2 + s * 6;
      const auto script_tag = r.u32(rec);
      if ((script_tag != tag('l','a','t','n')) and (script_tag != tag('g','r','e','k')) and
          (script_tag != tag('c','y','r','l')) and (script_tag != tag('D','F','L','T'))) continue;
      if (mark_lookups.contains(script_tag)) return false;
      const auto script = child(r, scripts, r.u16(rec + 4), 2 + uint32_t(script_count) * 6);
      const auto languages = r.u16(script + 2);
      if (not r.has(script, 4 + uint64_t(languages) * 6)) return false;
      const auto relative = r.u16(script);
      if (not relative) continue;
      const auto lang = child(r, script, relative, 4 + uint32_t(languages) * 6);
      const auto count = r.u16(lang + 4);
      if (not r.has(lang, 6 + uint64_t(count) * 2)) return false;
      std::vector<uint16_t> indices;
      auto add_feature = [&](uint16_t Index) {
         if (Index >= feature_count) { r.bad = true; return; }
         const auto feature_rec = features + 2 + uint32_t(Index) * 6;
         if (r.u32(feature_rec) != tag('m','a','r','k')) return;
         const auto feature = child(r, features, r.u16(feature_rec + 4), 2 + uint32_t(feature_count) * 6);
         const auto size = r.u16(feature + 2);
         if ((not consume(budget, size)) or (not r.has(feature, 4 + uint64_t(size) * 2))) {
            r.bad = true;
            return;
         }
         for (uint32_t i=0; i < size; i++) indices.push_back(r.u16(feature + 4 + i * 2));
      };
      if (const auto required = r.u16(lang + 2); required != 0xffff) add_feature(required);
      for (uint32_t i=0; i < count; i++) add_feature(r.u16(lang + 6 + i * 2));
      if (r.bad) return false;
      std::sort(indices.begin(), indices.end());
      indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
      auto &selected = mark_lookups[script_tag];
      for (auto index : indices) {
         if (index >= lookup_count) return false;
         const auto offset = child(r, list, r.u16(list + 2 + uint32_t(index) * 2),
            2 + uint32_t(lookup_count) * 2);
         const auto type = r.u16(offset), flags = r.u16(offset + 2), size = r.u16(offset + 4);
         const uint32_t header = 6 + uint32_t(size) * 2 + ((flags & LF_USE_MARK_FILTERING_SET) ? 2 : 0);
         if ((flags & LF_RESERVED) or (not r.has(offset, header)) or
             (total_subtables + size > 4096)) return false;
         total_subtables += size;
         lookup entry;
         entry.flags = flags;
         if (flags & LF_USE_MARK_FILTERING_SET) {
            entry.mark_set = r.u16(offset + 6 + uint32_t(size) * 2);
            if ((not mark_glyph_sets) or (entry.mark_set >= reader(gdef).u16(mark_glyph_sets + 2))) return false;
         }
         uint16_t extension_type = 0;
         for (uint32_t i=0; i < size; i++) {
            auto sub = child(r, offset, r.u16(offset + 6 + i * 2), header);
            auto real_type = type;
            if (type IS 9) {
               if ((r.u16(sub) != 1) or (not r.has(sub, 8))) return false;
               real_type = r.u16(sub + 2);
               if ((real_type < 1) or (real_type > 8) or
                   (extension_type and (extension_type != real_type))) return false;
               extension_type = real_type;
               sub = child(r, sub, r.u32(sub + 4), 8);
            }
            if (real_type != 4) continue;
            if (not subtable(r, sub, budget)) return false;
            entry.subtables.push_back(sub);
         }
         if (not entry.subtables.empty()) selected.push_back(std::move(entry));
      }
   }
   return not r.bad;
}

//********************************************************************************************************************
// Anchor formats 1/2 use design coordinates.  As with pair positioning, pixel Device corrections are ignored;
// format 3 VariationIndex corrections follow the selected variable instance.

double gpos_kerning::anchor_component(uint32_t Anchor, bool Vertical, instance &Instance) const
{
   const reader r(gpos);
   double value = r.s16(Anchor + (Vertical ? 4 : 2));
   if (r.u16(Anchor) IS 3) {
      if (const auto relative = r.u16(Anchor + (Vertical ? 8 : 6))) {
         const auto device = Anchor + relative;
         if (r.u16(device + 4) IS 0x8000) {
            value += variation_delta(r.u16(device), r.u16(device + 2), Instance);
         }
      }
   }
   return value;
}

//********************************************************************************************************************

void gpos_kerning::position_marks(std::span<shaped_glyph> Glyphs, uint32_t Script, instance &Instance) const
{
   auto selected = mark_lookups.find(Script);
   if (selected IS mark_lookups.end()) selected = mark_lookups.find(ot_reader::tag('D','F','L','T'));
   if ((selected IS mark_lookups.end()) or selected->second.empty() or Glyphs.empty()) return;
   const reader r(gpos), def(gdef);
   size_t budget = std::min(size_t(4000000), size_t(4096) + Glyphs.size() * 128);

   // Without GDEF, recognise marks across all selected lookups before finding bases.  A mark covered only by a
   // later lookup must not replace the base of another mark in an earlier lookup.

   std::vector<bool> covered_marks;
   if (not glyph_class_def) {
      covered_marks.resize(Glyphs.size());
      for (size_t i=0; i < Glyphs.size(); i++) {
         if (Glyphs[i].glyph IS shaped_glyph::INVISIBLE) continue;
         for (const auto &entry : selected->second) {
            for (auto sub : entry.subtables) {
               if (not consume(budget)) return;
               if (coverage_index(r, sub + r.u16(sub + 2), Glyphs[i].glyph) >= 0) {
                  covered_marks[i] = true;
                  break;
               }
            }
            if (covered_marks[i]) break;
         }
      }
   }

   for (const auto &entry : selected->second) {
      size_t base = Glyphs.size();
      for (size_t i=0; i < Glyphs.size(); i++) {
         if (not consume(budget)) break;
         auto &glyph = Glyphs[i];
         if (glyph.glyph IS shaped_glyph::INVISIBLE) continue;
         if ((glyph.unicode IS '\n') or (glyph.unicode IS '\r') or (glyph.unicode IS '\t')) {
            base = Glyphs.size();
            continue;
         }
         const auto cls = glyph_class_def ? glyph_class(def, glyph_class_def, glyph.glyph) : 0;
         bool mark = (cls IS GLYPH_CLASS_MARK) or ((not glyph_class_def) and covered_marks[i]) or
            (glyph.attachment != shaped_glyph::INVISIBLE);
         for (auto sub : entry.subtables) {
            if (not consume(budget)) break;
            const auto index = coverage_index(r, sub + r.u16(sub + 2), glyph.glyph);
            if (index < 0) continue;
            mark = true; // Coverage also recognises marks in fonts without GDEF classes
            if ((base IS Glyphs.size()) or skip_glyph(entry, glyph.glyph) or
                skip_glyph(entry, Glyphs[base].glyph)) continue;
            const auto base_index = coverage_index(r, sub + r.u16(sub + 4), Glyphs[base].glyph);
            if (base_index < 0) continue;
            const auto marks = sub + r.u16(sub + 8), bases = sub + r.u16(sub + 10);
            const auto rec = marks + 2 + uint32_t(index) * 4;
            const auto mark_anchor = marks + r.u16(rec + 2);
            const auto base_relative = r.u16(bases + 2 +
               (uint32_t(base_index) * r.u16(sub + 6) + r.u16(rec)) * 2);
            if (not base_relative) continue;
            const auto base_anchor = bases + base_relative;
            glyph.attachment = uint32_t(base);
            glyph.mark_x = anchor_component(base_anchor, false, Instance) -
               anchor_component(mark_anchor, false, Instance);
            glyph.mark_y = anchor_component(base_anchor, true, Instance) -
               anchor_component(mark_anchor, true, Instance);
            break; // First matching subtable; later lookups may replace this attachment
         }
         if (not mark) base = (cls IS GLYPH_CLASS_LIGATURE) ? Glyphs.size() : i;
      }
      if (not budget) break;
   }

   // Merge attachment ranges into source clusters in linear time.  This preserves all source counts and keeps
   // both direct combining sequences and GSUB decompositions intact under spacing and anywhere wrapping.

   std::vector<size_t> owners(Glyphs.size()), ends(Glyphs.size());
   size_t owner = 0;
   for (size_t i=0; i < Glyphs.size(); i++) {
      if (i and (Glyphs[i].cluster != Glyphs[i - 1].cluster)) owner = i;
      owners[i] = owner;
      ends[owner] = i + 1;
   }
   for (size_t i=0; i < Glyphs.size(); i++) {
      if (Glyphs[i].attachment < i) {
         auto &end = ends[owners[Glyphs[i].attachment]];
         end = std::max(end, i + 1);
      }
   }
   for (size_t start=0; start < Glyphs.size();) {
      size_t end = ends[start];
      uint32_t count = 0;
      for (size_t i=start; i < end; i++) {
         end = std::max(end, ends[owners[i]]);
         count += Glyphs[i].codepoints;
         Glyphs[i].cluster = Glyphs[start].cluster;
         Glyphs[i].codepoints = 0;
      }
      Glyphs[start].codepoints = count;
      start = end;
   }
}
