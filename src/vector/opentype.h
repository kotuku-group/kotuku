#pragma once

// Shared OpenType table access for the GPOS positioning and GSUB substitution readers.

#include <kotuku/main.h>
#include <cstdint>
#include <charconv>
#include <optional>
#include <string_view>
#include <vector>

namespace ot_reader {

constexpr uint32_t tag(char A, char B, char C, char D) {
   return (uint32_t(uint8_t(A))<<24) | (uint32_t(uint8_t(B))<<16) | (uint32_t(uint8_t(C))<<8) | uint8_t(D);
}

// Lookup flags shared by GPOS and GSUB.

constexpr uint16_t LF_IGNORE_BASE_GLYPHS     = 0x0002;
constexpr uint16_t LF_IGNORE_LIGATURES       = 0x0004;
constexpr uint16_t LF_IGNORE_MARKS           = 0x0008;
constexpr uint16_t LF_USE_MARK_FILTERING_SET = 0x0010;
constexpr uint16_t LF_RESERVED               = 0x00e0;
constexpr uint16_t LF_MARK_ATTACHMENT_TYPE   = 0xff00;
constexpr uint16_t LF_SKIP_MASK = LF_IGNORE_BASE_GLYPHS | LF_IGNORE_LIGATURES | LF_IGNORE_MARKS |
   LF_USE_MARK_FILTERING_SET | LF_MARK_ATTACHMENT_TYPE;

// GDEF glyph classes.

constexpr uint16_t GLYPH_CLASS_BASE      = 1;
constexpr uint16_t GLYPH_CLASS_LIGATURE  = 2;
constexpr uint16_t GLYPH_CLASS_MARK      = 3;
constexpr uint16_t GLYPH_CLASS_COMPONENT = 4;

// Bounds-checked big-endian reader.  Any out-of-range read returns zero and sets the sticky 'bad' flag, so that a
// sequence of reads can be validated with a single check.

struct reader {
   const uint8_t *data;
   uint32_t size;
   mutable bool bad = false;

   reader(const std::vector<uint8_t> &Table) : data(Table.data()), size(uint32_t(Table.size())) { }

   bool has(uint64_t Offset, uint64_t Length) const {
      if ((Offset <= size) and (Length <= size - Offset)) return true;
      bad = true;
      return false;
   }

   uint16_t u16(uint64_t Offset) const {
      if (not has(Offset, 2)) return 0;
      return uint16_t((data[Offset]<<8) | data[Offset+1]);
   }

   int16_t s16(uint64_t Offset) const { return int16_t(u16(Offset)); }

   uint32_t u32(uint64_t Offset) const {
      if (not has(Offset, 4)) return 0;
      return (uint32_t(data[Offset])<<24) | (uint32_t(data[Offset+1])<<16) | (uint32_t(data[Offset+2])<<8) |
         uint32_t(data[Offset+3]);
   }
};

// Returns the coverage index of a glyph, or -1 if the glyph is not covered.

inline int coverage_index(const reader &R, uint32_t Offset, uint32_t Glyph)
{
   if (Glyph > 0xffff) return -1;
   const auto format = R.u16(Offset);
   const auto count = R.u16(Offset + 2);
   if (format IS 1) {
      int lo = 0, hi = int(count) - 1;
      while (lo <= hi) {
         const int mid = (lo + hi)>>1;
         const auto g = R.u16(Offset + 4 + uint32_t(mid) * 2);
         if (R.bad) return -1;
         if (g < Glyph) lo = mid + 1;
         else if (g > Glyph) hi = mid - 1;
         else return mid;
      }
   }
   else if (format IS 2) {
      int lo = 0, hi = int(count) - 1;
      while (lo <= hi) {
         const int mid = (lo + hi)>>1;
         const uint32_t rec = Offset + 4 + uint32_t(mid) * 6;
         const auto start = R.u16(rec), end = R.u16(rec + 2);
         if (R.bad) return -1;
         if (end < Glyph) lo = mid + 1;
         else if (start > Glyph) hi = mid - 1;
         else return int(R.u16(rec + 4)) + int(Glyph - start);
      }
   }
   return -1;
}

inline bool validate_coverage(const reader &R, uint32_t Offset)
{
   const auto format = R.u16(Offset);
   const auto count = R.u16(Offset + 2);
   if (format IS 1) return R.has(Offset + 4, uint64_t(count) * 2);
   else if (format IS 2) return R.has(Offset + 4, uint64_t(count) * 6);
   else return false;
}

// Returns the class of a glyph.  Glyphs that the ClassDef does not list are in class zero.

inline uint16_t glyph_class(const reader &R, uint32_t Offset, uint32_t Glyph)
{
   if (Glyph > 0xffff) return 0;
   const auto format = R.u16(Offset);
   if (format IS 1) {
      const auto start = R.u16(Offset + 2);
      const auto count = R.u16(Offset + 4);
      if ((Glyph >= start) and (Glyph < uint32_t(start) + count)) return R.u16(Offset + 6 + (Glyph - start) * 2);
   }
   else if (format IS 2) {
      int lo = 0, hi = int(R.u16(Offset + 2)) - 1;
      while (lo <= hi) {
         const int mid = (lo + hi)>>1;
         const uint32_t rec = Offset + 4 + uint32_t(mid) * 6;
         const auto start = R.u16(rec), end = R.u16(rec + 2);
         if (R.bad) return 0;
         if (end < Glyph) lo = mid + 1;
         else if (start > Glyph) hi = mid - 1;
         else return R.u16(rec + 4);
      }
   }
   return 0;
}

inline bool validate_class_def(const reader &R, uint32_t Offset)
{
   const auto format = R.u16(Offset);
   if (format IS 1) return R.has(Offset + 6, uint64_t(R.u16(Offset + 4)) * 2);
   else if (format IS 2) return R.has(Offset + 4, uint64_t(R.u16(Offset + 2)) * 6);
   else return false;
}

// Reads a setting from the canonical TextStyle feature string.  Missing features are deliberately opt-in.

inline std::optional<uint32_t> feature_value(std::string_view Features, uint32_t Tag)
{
   while (not Features.empty()) {
      const auto comma = Features.find(',');
      auto item = Features.substr(0, comma);
      while ((not item.empty()) and (item.front() IS ' ')) item.remove_prefix(1);
      if ((item.size() >= 6) and (tag(item[0], item[1], item[2], item[3]) IS Tag) and (item[4] IS ' ')) {
         item.remove_prefix(5);
         uint32_t value = 0;
         const auto result = std::from_chars(item.data(), item.data() + item.size(), value);
         if ((result.ec IS std::errc()) and (result.ptr IS item.data() + item.size())) return value;
         return std::nullopt;
      }
      if (comma IS std::string_view::npos) break;
      Features.remove_prefix(comma + 1);
   }
   return std::nullopt;
}

// Returns true if a lookup with the given flags ignores the glyph.  Mark filtering sets take precedence over the mark
// attachment type.

inline bool skip_glyph(const reader &R, uint16_t Flags, uint16_t MarkSet, uint32_t Glyph,
   uint32_t GlyphClasses, uint32_t MarkClasses, uint32_t MarkSets)
{
   if ((not GlyphClasses) or (not (Flags & LF_SKIP_MASK))) return false;

   const auto cls = glyph_class(R, GlyphClasses, Glyph);
   if ((cls IS GLYPH_CLASS_BASE) and (Flags & LF_IGNORE_BASE_GLYPHS)) return true;
   if ((cls IS GLYPH_CLASS_LIGATURE) and (Flags & LF_IGNORE_LIGATURES)) return true;
   if (cls IS GLYPH_CLASS_MARK) {
      if (Flags & LF_IGNORE_MARKS) return true;
      if (Flags & LF_USE_MARK_FILTERING_SET) {
         if ((not MarkSets) or (MarkSet >= R.u16(MarkSets + 2))) return true;
         const auto coverage = MarkSets + R.u32(MarkSets + 4 + uint32_t(MarkSet) * 4);
         return coverage_index(R, coverage, Glyph) < 0;
      }
      if (auto type = (Flags & LF_MARK_ATTACHMENT_TYPE)>>8) {
         return (not MarkClasses) or (glyph_class(R, MarkClasses, Glyph) != type);
      }
   }
   return false;
}

} // namespace ot_reader
