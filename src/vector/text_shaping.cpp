// Text shaping for VectorText.  See text_shaping.h for an overview.

#include "text_shaping.h"
#include "gsub.h"
#include "opentype.h"
#include "../link/unicode.h"
#include <algorithm>

namespace {

constexpr uint32_t SCRIPT_UNSUPPORTED = 0xffffffff;
constexpr uint32_t SCRIPT_COMMON = 0; // Common or Inherited; joins the neighbouring script run
constexpr uint32_t SCRIPT_LATIN = ot_reader::tag('l','a','t','n');
constexpr uint32_t SCRIPT_GREEK = ot_reader::tag('g','r','e','k');
constexpr uint32_t SCRIPT_CYRILLIC = ot_reader::tag('c','y','r','l');

// A conservative range table for the three supported scripts.  Every other script remains untouched, even when a
// font advertises substitutions for it under DFLT.

uint32_t script_of(uint32_t Unicode)
{
   auto range = [Unicode](uint32_t First, uint32_t Last) { return (Unicode >= First) and (Unicode <= Last); };
   if (range('A', 'Z') or range('a', 'z') or (Unicode IS 0xaa) or (Unicode IS 0xba) or
       range(0xc0, 0xd6) or range(0xd8, 0xf6) or range(0xf8, 0x2e4) or range(0x1d00, 0x1d25) or
       range(0x1d2c, 0x1d5c) or range(0x1d62, 0x1d65) or range(0x1d6b, 0x1d77) or range(0x1d79, 0x1dbe) or
       range(0x1e00, 0x1eff) or range(0x2c60, 0x2c7f) or range(0xa720, 0xa7ff) or range(0xab30, 0xab64) or
       range(0xab66, 0xab6f) or range(0xff21, 0xff3a) or range(0xff41, 0xff5a) or
       range(0x10780, 0x107bf) or range(0x1df00, 0x1dfff)) return SCRIPT_LATIN;
   if (range(0x370, 0x3ff) or range(0x1f00, 0x1fff) or range(0x1d26, 0x1d2a) or
       range(0x1d5d, 0x1d61) or range(0x1d66, 0x1d6a)) return SCRIPT_GREEK;
   if (range(0x400, 0x52f) or range(0x1c80, 0x1c8f) or (Unicode IS 0x1d2b) or (Unicode IS 0x1d78) or
       range(0x2de0, 0x2dff) or range(0xa640, 0xa69f) or range(0x1e030, 0x1e08f)) return SCRIPT_CYRILLIC;
   if ((Unicode <= 0xbf) or (Unicode IS 0xd7) or (Unicode IS 0xf7) or range(0x2e5, 0x36f) or
       range(0x1ab0, 0x1aff) or range(0x1dc0, 0x1dff) or range(0x2000, 0x27ff) or range(0x2900, 0x2bff) or
       range(0xfe00, 0xfe0f) or range(0xfe20, 0xfe2f) or range(0xff00, 0xff20) or
       range(0xff3b, 0xff40) or range(0xff5b, 0xff65) or range(0x1f000, 0x1faff) or
       range(0xe0100, 0xe01ef)) return SCRIPT_COMMON;
   return SCRIPT_UNSUPPORTED;
}

} // namespace

//********************************************************************************************************************

std::vector<shaped_glyph> shape_line(FT_Face Face, std::string_view Line, gsub_table *Table,
   std::string_view Features)
{
   std::vector<shaped_glyph> glyphs;
   glyphs.reserve(Line.size());
   for (size_t offset=0; offset < Line.size();) {
      int length;
      const auto unicode = UTF8ReadValue(Line.substr(offset), &length);
      if (not unicode) break;
      glyphs.push_back({ FT_Get_Char_Index(Face, unicode), uint32_t(offset), 1, 0, unicode });
      offset += length;
   }
   if ((not Table) or Features.empty() or (Features IS "none") or (not Table->enabled())) return glyphs;

   // Leading common/inherited characters join the first strong script.  A neutral-only line uses Latin so that
   // numeric features work on strings containing only digits.  Later neutral characters join the preceding run.

   uint32_t script = SCRIPT_LATIN;
   for (const auto &glyph : glyphs) {
      if (const auto strong = script_of(glyph.unicode); strong != SCRIPT_COMMON) { script = strong; break; }
   }
   std::vector<shaped_glyph> result;
   result.reserve(glyphs.size());
   size_t budget = gsub_table::work_budget(glyphs.size());
   size_t growth = gsub_table::growth_budget(glyphs.size());
   for (size_t start=0; start < glyphs.size();) {
      size_t end = start + 1;
      while (end < glyphs.size()) {
         const auto next = script_of(glyphs[end].unicode);
         if ((next != SCRIPT_COMMON) and (next != script)) break;
         end++;
      }
      if ((script != SCRIPT_UNSUPPORTED) and budget) {
         std::vector<shaped_glyph> run(glyphs.begin() + start, glyphs.begin() + end);
         Table->apply(run, script, Features, &budget, &growth);
         result.insert(result.end(), run.begin(), run.end());
      }
      else result.insert(result.end(), glyphs.begin() + start, glyphs.begin() + end);
      start = end;
      if (start < glyphs.size()) script = script_of(glyphs[start].unicode);
   }
   return result;
}

//********************************************************************************************************************

const std::vector<shaped_glyph> & shaped_line_cache::get(FT_Face Face, std::string_view Line,
   std::span<const FT_Fixed> Axes, std::string_view Features, gsub_table *Table)
{
   if ((not valid) or (face != Face) or (features != Features) or (not std::ranges::equal(axes, Axes))) {
      glyphs = shape_line(Face, Line, Table, Features);
      face = Face;
      axes.assign(Axes.begin(), Axes.end());
      features = Features;
      valid = true;
   }
   return glyphs;
}
