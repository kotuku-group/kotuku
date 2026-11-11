// GPOS pair kerning reader.  See kerning.h for an overview.
//
// References: OpenType specification, chapters 'GPOS', 'GDEF', 'OpenType Layout Common Table Formats' and
// 'OpenType Font Variations Common Table Formats'.

#include "kerning.h"
#include "opentype.h"
#include <algorithm>
#include <bit>

using ot_reader::reader;
using ot_reader::coverage_index;
using ot_reader::validate_coverage;
using ot_reader::glyph_class;
using ot_reader::validate_class_def;
using ot_reader::LF_USE_MARK_FILTERING_SET;

namespace {

constexpr uint32_t TAG_KERN = ot_reader::tag('k','e','r','n');
constexpr uint32_t TAG_DFLT = ot_reader::tag('D','F','L','T');
constexpr uint32_t TAG_LATN = ot_reader::tag('l','a','t','n');

constexpr uint16_t LOOKUP_PAIR_POS  = 2;
constexpr uint16_t LOOKUP_EXTENSION = 9;

constexpr uint16_t VF_X_PLACEMENT  = 0x0001;
constexpr uint16_t VF_X_PLA_DEVICE = 0x0010;
constexpr uint16_t VF_X_ADVANCE    = 0x0004;
constexpr uint16_t VF_X_ADV_DEVICE = 0x0040;

constexpr uint16_t DELTA_VARIATION_INDEX = 0x8000;

constexpr int MAX_LOOKUPS = 1024; // Sanity limits for the lookup list
constexpr int MAX_SUBTABLES = 4096;

// The size of a ValueRecord in bytes.  Each bit of the ValueFormat adds one 16-bit field.

inline uint32_t value_size(uint16_t Format) { return uint32_t(std::popcount(uint16_t(Format & 0xff))) * 2; }

// Device and VariationIndex tables have the same 6-byte header.

bool validate_value(const reader &R, uint32_t Base, uint32_t Record, uint16_t Format)
{
   if (not R.has(Record, value_size(Format))) return false;
   for (auto bit : { VF_X_PLA_DEVICE, VF_X_ADV_DEVICE }) {
      if (not (Format & bit)) continue;
      const auto device = R.u16(Record + uint32_t(std::popcount(uint16_t(Format & (bit - 1)))) * 2);
      if (device and not R.has(uint64_t(Base) + device, 6)) return false;
   }
   return true;
}

} // namespace

//********************************************************************************************************************

bool gpos_kerning::load(std::vector<uint8_t> GPOS, std::vector<uint8_t> GDEF)
{
   gpos = std::move(GPOS);
   gdef = std::move(GDEF);
   lookups.clear();
   mark_lookups.clear();
   glyph_class_def = mark_attach_class_def = mark_glyph_sets = var_store = 0;
   region_count = 0;

   if ((gpos.size() > 64 * 1024 * 1024) or (gdef.size() > 64 * 1024 * 1024) or
       (not parse_lookups()) or (not validate_gdef()) or (not parse_marks())) {
      lookups.clear();
      mark_lookups.clear();
      gpos.clear();
      gdef.clear();
      return false;
   }

   if (lookups.empty() and mark_lookups.empty()) { // Nothing to apply; release the table memory
      gpos.clear();
      gdef.clear();
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Selects the lookups of the 'kern' feature from the default language system of the 'latn' script, falling back to
// DFLT.  Text is not itemised by script, so the Latin lookups are preferred: fonts such as Noto Sans place their
// Latin pair kerning in a lookup that only 'latn' references, which is also what HarfBuzz and browsers apply to Latin
// text.  Every subtable is validated in full so that queries cannot encounter a malformed record.

bool gpos_kerning::parse_lookups()
{
   const reader r(gpos);
   if (r.u16(0) != 1) return false; // Major version
   const uint32_t script_list = r.u16(4);
   const uint32_t feature_list = r.u16(6);
   const uint32_t lookup_list = r.u16(8);
   if (r.bad or (not script_list) or (not feature_list) or (not lookup_list)) return false;

   // Find the default language system

   uint32_t lang_sys = 0;
   const auto script_count = r.u16(script_list);
   for (auto tag : { TAG_LATN, TAG_DFLT }) {
      for (uint32_t s=0; s < script_count; s++) {
         const uint32_t rec = script_list + 2 + s * 6;
         if (r.u32(rec) IS tag) {
            const uint32_t script = script_list + r.u16(rec + 4);
            if (auto def = r.u16(script)) lang_sys = script + def;
            break;
         }
      }
      if (r.bad) return false;
      if (lang_sys) break;
   }

   if (not lang_sys) return true; // No applicable script; not an error

   // Collect the lookup indices of every 'kern' feature referenced by the language system.

   std::vector<uint16_t> indices;
   const auto feature_count = r.u16(feature_list);

   auto add_feature = [&](uint16_t Index) {
      if (Index >= feature_count) { r.bad = true; return; }
      const uint32_t rec = feature_list + 2 + uint32_t(Index) * 6;
      if (r.u32(rec) != TAG_KERN) return;
      const uint32_t feature = feature_list + r.u16(rec + 4);
      const auto count = r.u16(feature + 2);
      for (uint32_t i=0; i < count; i++) indices.push_back(r.u16(feature + 4 + i * 2));
   };

   if (auto required = r.u16(lang_sys + 2); required != 0xffff) add_feature(required);
   const auto lang_feature_count = r.u16(lang_sys + 4);
   for (uint32_t i=0; i < lang_feature_count; i++) add_feature(r.u16(lang_sys + 6 + i * 2));
   if (r.bad) return false;

   // Lookups are applied in lookup list order, regardless of the order in which features reference them.

   std::sort(indices.begin(), indices.end());
   indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

   const auto lookup_count = r.u16(lookup_list);
   int total_subtables = 0;
   for (auto index : indices) {
      if ((index >= lookup_count) or (std::ssize(lookups) >= MAX_LOOKUPS)) return false;
      const uint32_t offset = lookup_list + r.u16(lookup_list + 2 + uint32_t(index) * 2);
      const auto type = r.u16(offset);
      const auto flags = r.u16(offset + 2);
      const auto count = r.u16(offset + 4);
      if (r.bad) return false;

      lookup lk;
      lk.flags = flags;
      if (flags & LF_USE_MARK_FILTERING_SET) lk.mark_set = r.u16(offset + 6 + uint32_t(count) * 2);

      for (uint32_t s=0; s < count; s++) {
         uint32_t sub = offset + r.u16(offset + 6 + s * 2);
         auto sub_type = type;
         if (type IS LOOKUP_EXTENSION) {
            if (r.u16(sub) != 1) return false;
            sub_type = r.u16(sub + 2);
            const uint64_t target = uint64_t(sub) + r.u32(sub + 4);
            if (target >= gpos.size()) return false;
            sub = uint32_t(target);
         }
         if (r.bad) return false;
         if (sub_type != LOOKUP_PAIR_POS) continue; // Other lookup types do not define pair kerning

         // Validate the PairPos subtable in full

         const auto format = r.u16(sub);
         const auto coverage = r.u16(sub + 2);
         const auto vf1 = r.u16(sub + 4);
         const auto vf2 = r.u16(sub + 6);
         if (r.bad or (not coverage) or (not validate_coverage(r, sub + coverage))) return false;
         const uint32_t record_size = value_size(vf1) + value_size(vf2);

         if (format IS 1) {
            const auto set_count = r.u16(sub + 8);
            if (not r.has(sub + 10, uint64_t(set_count) * 2)) return false;
            for (uint32_t p=0; p < set_count; p++) {
               const uint32_t set = sub + r.u16(sub + 10 + p * 2);
               const auto pair_count = r.u16(set);
               if (not r.has(uint64_t(set) + 2, uint64_t(pair_count) * (2 + record_size))) return false;
               for (uint32_t v=0; v < pair_count; v++) {
                  const uint32_t rec = set + 2 + v * (2 + record_size);
                  if (not validate_value(r, set, rec + 2, vf1)) return false;
                  if (not validate_value(r, set, rec + 2 + value_size(vf1), vf2)) return false;
               }
            }
         }
         else if (format IS 2) {
            const auto class_def1 = r.u16(sub + 8);
            const auto class_def2 = r.u16(sub + 10);
            const auto class1_count = r.u16(sub + 12);
            const auto class2_count = r.u16(sub + 14);
            if (r.bad or (not class_def1) or (not class_def2)) return false;
            if ((not validate_class_def(r, sub + class_def1)) or (not validate_class_def(r, sub + class_def2))) {
               return false;
            }
            const uint64_t records = uint64_t(class1_count) * class2_count;
            if (not r.has(uint64_t(sub) + 16, records * record_size)) return false;
            if ((vf1 | vf2) & (VF_X_PLA_DEVICE | VF_X_ADV_DEVICE)) {
               for (uint32_t v=0; v < records; v++) {
                  const uint32_t rec = sub + 16 + v * record_size;
                  if (not validate_value(r, sub, rec, vf1)) return false;
                  if (not validate_value(r, sub, rec + value_size(vf1), vf2)) return false;
               }
            }
         }
         else return false;

         if (++total_subtables > MAX_SUBTABLES) return false;
         lk.subtables.push_back(sub);
      }

      if (not lk.subtables.empty()) lookups.push_back(std::move(lk));
   }

   return not r.bad;
}

//********************************************************************************************************************
// Reads the GDEF offsets that kerning relies on: glyph classes for the lookup flags, and the ItemVariationStore for
// device deltas.  An absent GDEF table is valid.

bool gpos_kerning::validate_gdef()
{
   if (gdef.empty()) return true;

   const reader r(gdef);
   if (r.u16(0) != 1) return false;
   const auto minor = r.u16(2);
   if (r.bad) return false;

   if (auto offset = r.u16(4)) {
      glyph_class_def = offset;
      if (not validate_class_def(r, glyph_class_def)) return false;
   }

   if (auto offset = r.u16(10)) {
      mark_attach_class_def = offset;
      if (not validate_class_def(r, mark_attach_class_def)) return false;
   }

   if (minor >= 2) {
      if (auto offset = r.u16(12)) {
         mark_glyph_sets = offset;
         if (r.u16(mark_glyph_sets) != 1) return false;
         const auto count = r.u16(mark_glyph_sets + 2);
         for (uint32_t i=0; i < count; i++) {
            const uint64_t coverage = uint64_t(mark_glyph_sets) + r.u32(mark_glyph_sets + 4 + i * 4);
            if ((coverage > 0xffffffff) or (not validate_coverage(r, uint32_t(coverage)))) return false;
         }
      }
   }

   if (minor >= 3) {
      if (auto offset = r.u32(14)) {
         var_store = offset;
         if (r.u16(var_store) != 1) return false;
         const uint64_t region_list = uint64_t(var_store) + r.u32(var_store + 2);
         const auto data_count = r.u16(var_store + 6);
         if (r.bad or (region_list > 0xffffffff)) return false;

         const auto axis_count = r.u16(region_list);
         region_count = r.u16(region_list + 2);
         if (not r.has(region_list + 4, uint64_t(axis_count) * region_count * 6)) return false;

         for (uint32_t d=0; d < data_count; d++) {
            const uint64_t data = uint64_t(var_store) + r.u32(var_store + 8 + d * 4);
            if (data > 0xffffffff) return false;
            const auto item_count = r.u16(data);
            const auto word_delta_count = r.u16(data + 2);
            const auto index_count = r.u16(data + 4);
            const uint32_t word_count = word_delta_count & 0x7fff;
            const bool long_words = word_delta_count & 0x8000;
            if (word_count > index_count) return false;
            for (uint32_t i=0; i < index_count; i++) {
               if (r.u16(data + 6 + i * 2) >= region_count) return false;
            }
            const uint64_t row = word_count * (long_words ? 4 : 2) + (index_count - word_count) * (long_words ? 2 : 1);
            if (not r.has(data + 6 + uint64_t(index_count) * 2, row * item_count)) return false;
         }
      }
   }

   return not r.bad;
}

//********************************************************************************************************************
// Returns true if the lookup flags exclude a glyph from matching.

bool gpos_kerning::skip_glyph(const lookup &Lookup, uint32_t Glyph) const
{
   return ot_reader::skip_glyph(reader(gdef), Lookup.flags, Lookup.mark_set, Glyph,
      glyph_class_def, mark_attach_class_def, mark_glyph_sets);
}

//********************************************************************************************************************
// Region scalars follow the ItemVariationStore algorithm in the OpenType specification.

float gpos_kerning::region_scalar(uint16_t Region, instance &Instance) const
{
   if (Instance.scalars.empty()) Instance.scalars.assign(region_count, -1.0f);
   if (Region >= Instance.scalars.size()) return 0;
   if (Instance.scalars[Region] >= 0) return Instance.scalars[Region];

   const reader r(gdef);
   const uint32_t region_list = var_store + r.u32(var_store + 2);
   const auto axis_count = r.u16(region_list);
   float scalar = 1.0f;
   for (uint32_t a=0; (a < axis_count) and (scalar > 0); a++) {
      const uint32_t rec = region_list + 4 + (uint32_t(Region) * axis_count + a) * 6;
      const int start = r.s16(rec), peak = r.s16(rec + 2), end = r.s16(rec + 4);
      const int coord = (a < Instance.coords.size()) ? Instance.coords[a] : 0;

      if ((start > peak) or (peak > end)) continue; // Invalid axis ranges are ignored
      if ((start < 0) and (end > 0) and (peak != 0)) continue;
      if ((peak IS 0) or (coord IS peak)) continue;
      if ((coord <= start) or (end <= coord)) scalar = 0;
      else if (coord < peak) scalar *= float(coord - start) / float(peak - start);
      else scalar *= float(end - coord) / float(end - peak);
   }

   if (r.bad) scalar = 0;
   Instance.scalars[Region] = scalar;
   return scalar;
}

//********************************************************************************************************************
// Resolves a VariationIndex to a delta in font units for the instance.  Unknown indices (including the 0xffff/0xffff
// 'no variation' index) resolve to zero.

float gpos_kerning::variation_delta(uint16_t Outer, uint16_t Inner, instance &Instance) const
{
   if ((not var_store) or (not Instance.variable)) return 0;

   const uint32_t key = (uint32_t(Outer)<<16) | Inner;
   if (auto it = Instance.deltas.find(key); it != Instance.deltas.end()) return it->second;

   const reader r(gdef);
   float delta = 0;
   if (Outer < r.u16(var_store + 6)) {
      const uint32_t data = var_store + r.u32(var_store + 8 + uint32_t(Outer) * 4);
      const auto item_count = r.u16(data);
      const auto word_delta_count = r.u16(data + 2);
      const auto index_count = r.u16(data + 4);
      const uint32_t word_count = word_delta_count & 0x7fff;
      const bool long_words = word_delta_count & 0x8000;
      if (Inner < item_count) {
         const uint32_t row_size = word_count * (long_words ? 4 : 2) +
            (index_count - word_count) * (long_words ? 2 : 1);
         uint32_t pos = data + 6 + uint32_t(index_count) * 2 + uint32_t(Inner) * row_size;
         for (uint32_t i=0; i < index_count; i++) {
            int value;
            if (i < word_count) {
               if (long_words) { value = int32_t(r.u32(pos)); pos += 4; }
               else { value = r.s16(pos); pos += 2; }
            }
            else if (long_words) { value = r.s16(pos); pos += 2; }
            else { value = r.has(pos, 1) ? int8_t(r.data[pos]) : 0; pos += 1; }

            if (value) delta += float(value) * region_scalar(r.u16(data + 6 + i * 2), Instance);
         }
      }
   }

   if (r.bad) delta = 0;
   Instance.deltas[key] = delta;
   return delta;
}

//********************************************************************************************************************
// Reads a horizontal placement or advance component, including its variation delta.
// Device offsets are relative to the PairSet (format 1) or PairPos subtable (format 2).

double gpos_kerning::value_component(uint32_t Base, uint32_t Record, uint16_t Format, uint16_t Bit,
   instance &Instance) const
{
   const reader r(gpos);
   double value = 0;
   if (Format & Bit) value = r.s16(Record + uint32_t(std::popcount(uint16_t(Format & (Bit - 1)))) * 2);
   const uint16_t device_bit = Bit << 4;
   if ((Format & device_bit) and Instance.variable) {
      if (auto device = r.u16(Record + uint32_t(std::popcount(uint16_t(Format & (device_bit - 1)))) * 2)) {
         const uint32_t table = Base + device;
         if (r.u16(table + 4) IS DELTA_VARIATION_INDEX) {
            value += variation_delta(r.u16(table), r.u16(table + 2), Instance);
         }
      }
   }
   return r.bad ? 0 : value;
}

//********************************************************************************************************************
// Returns true if the subtable applies to the pair, with the advance adjustment in Value.  As in OpenType layout
// engines, a format 1 subtable applies only if it lists the pair, while a format 2 subtable applies to any pair whose
// first glyph it covers, even if the class pair has no adjustment.

bool gpos_kerning::apply_subtable(uint32_t Offset, uint32_t First, uint32_t Second, instance &Instance,
   adjustment &Value) const
{
   const reader r(gpos);
   const int index = coverage_index(r, Offset + r.u16(Offset + 2), First);
   if (index < 0) return false;

   const auto format = r.u16(Offset);
   const auto vf1 = r.u16(Offset + 4);
   const auto vf2 = r.u16(Offset + 6);
   const uint32_t size1 = value_size(vf1);
   const uint32_t record_size = size1 + value_size(vf2);

   if (format IS 1) {
      if (index >= r.u16(Offset + 8)) return false;
      const uint32_t set = Offset + r.u16(Offset + 10 + uint32_t(index) * 2);
      int lo = 0, hi = int(r.u16(set)) - 1;
      while (lo <= hi) {
         const int mid = (lo + hi)>>1;
         const uint32_t rec = set + 2 + uint32_t(mid) * (2 + record_size);
         const auto glyph = r.u16(rec);
         if (r.bad) return false;
         if (glyph < Second) lo = mid + 1;
         else if (glyph > Second) hi = mid - 1;
         else {
            Value = { value_component(set, rec + 2, vf1, VF_X_ADVANCE, Instance),
               value_component(set, rec + 2 + size1, vf2, VF_X_ADVANCE, Instance),
               value_component(set, rec + 2, vf1, VF_X_PLACEMENT, Instance),
               value_component(set, rec + 2 + size1, vf2, VF_X_PLACEMENT, Instance) };
            return true;
         }
      }
      return false;
   }
   else if (format IS 2) {
      const auto class1 = glyph_class(r, Offset + r.u16(Offset + 8), First);
      const auto class2 = glyph_class(r, Offset + r.u16(Offset + 10), Second);
      const auto class1_count = r.u16(Offset + 12);
      const auto class2_count = r.u16(Offset + 14);
      if ((class1 >= class1_count) or (class2 >= class2_count) or r.bad) return false;
      const uint32_t rec = Offset + 16 + (uint32_t(class1) * class2_count + class2) * record_size;
      Value = { value_component(Offset, rec, vf1, VF_X_ADVANCE, Instance),
         value_component(Offset, rec + size1, vf2, VF_X_ADVANCE, Instance),
         value_component(Offset, rec, vf1, VF_X_PLACEMENT, Instance),
         value_component(Offset, rec + size1, vf2, VF_X_PLACEMENT, Instance) };
      return true;
   }
   return false;
}

//********************************************************************************************************************

// Computes a pair that is not in the instance cache, and stores the result.

gpos_kerning::adjustment gpos_kerning::compute_pair(uint32_t Key, instance &Instance) const
{
   if (lookups.empty()) return {};

   using inst = instance;
   if (Instance.pairs.empty()) Instance.pairs.resize(size_t(1)<<inst::PAIR_CACHE_BITS);

   const uint32_t first = Key>>16, second = Key & 0xffff;
   adjustment total;
   for (auto &lk : lookups) {
      if (skip_glyph(lk, first) or skip_glyph(lk, second)) continue;
      for (auto sub : lk.subtables) {
         adjustment value;
         if (apply_subtable(sub, first, second, Instance, value)) {
            total.first_advance += value.first_advance;
            total.second_advance += value.second_advance;
            total.first_placement += value.first_placement;
            total.second_placement += value.second_placement;
            break; // Only the first applicable subtable of a lookup is used
         }
      }
   }

   if (Key != inst::EMPTY_PAIR) Instance.pairs[(Key * 2654435761u)>>(32 - inst::PAIR_CACHE_BITS)] = { Key, total };
   return total;
}
