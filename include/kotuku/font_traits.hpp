#pragma once

// Font style traits shared by the Font and Vector modules.
//
// Style names such as `Semi Bold Italic` and style descriptors such as `weight=Medium;width=Condensed` are reduced to
// CSS weight, width and italic traits.  A requested set of traits can then be matched to the nearest available style
// with the CSS Fonts 4 font matching order.
//
// This header does not depend on the Kotuku core, so it can be included by stand-alone unit tests.

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef IS
#define IS ==
#endif

namespace kt::font_traits {

struct keyword {
   std::string_view name; // Normalised form: lower case, without spaces, hyphens or underscores
   double value;
};

inline constexpr std::array<keyword, 18> WEIGHT_KEYWORDS = {{
   { "thin", 100 }, { "hairline", 100 }, { "extralight", 200 }, { "ultralight", 200 }, { "light", 300 },
   { "regular", 400 }, { "normal", 400 }, { "book", 400 }, { "medium", 500 }, { "semibold", 600 },
   { "demibold", 600 }, { "bold", 700 }, { "extrabold", 800 }, { "ultrabold", 800 }, { "black", 900 },
   { "heavy", 900 }, { "extrablack", 950 }, { "ultrablack", 950 }
}};

inline constexpr std::array<keyword, 9> WIDTH_KEYWORDS = {{
   { "ultracondensed", 50 }, { "extracondensed", 62.5 }, { "condensed", 75 }, { "semicondensed", 87.5 },
   { "normal", 100 }, { "semiexpanded", 112.5 }, { "expanded", 125 }, { "extraexpanded", 150 },
   { "ultraexpanded", 200 }
}};

// Conventional style names, as used by the font database, for building a style name from traits.

inline constexpr std::array<keyword, 10> WEIGHT_NAMES = {{
   { "Thin", 100 }, { "Extra Light", 200 }, { "Light", 300 }, { "", 400 }, { "Medium", 500 }, { "Semi Bold", 600 },
   { "Bold", 700 }, { "Extra Bold", 800 }, { "Black", 900 }, { "Extra Black", 950 }
}};

inline constexpr std::array<keyword, 9> WIDTH_NAMES = {{
   { "Ultra Condensed", 50 }, { "Extra Condensed", 62.5 }, { "Condensed", 75 }, { "Semi Condensed", 87.5 },
   { "", 100 }, { "Semi Expanded", 112.5 }, { "Expanded", 125 }, { "Extra Expanded", 150 },
   { "Ultra Expanded", 200 }
}};

struct traits {
   double weight = 400;  // CSS font-weight, 1 - 1000
   double width  = 100;  // CSS font-width as a percentage
   bool italic   = false;
   bool recognised = true; // False if a style name contained words that are not traits
};

//********************************************************************************************************************

[[nodiscard]] inline std::string_view trim(std::string_view Value) noexcept
{
   while ((not Value.empty()) and (unsigned(Value.front()) <= 0x20)) Value.remove_prefix(1);
   while ((not Value.empty()) and (unsigned(Value.back()) <= 0x20)) Value.remove_suffix(1);
   return Value;
}

// Keywords are case-insensitive, and spaces, hyphens and underscores are ignored.

[[nodiscard]] inline std::string normalise(std::string_view Value)
{
   std::string result;
   result.reserve(Value.size());
   for (auto ch : Value) {
      if ((unsigned(ch) <= 0x20) or (ch IS '-') or (ch IS '_')) continue;
      if ((ch >= 'A') and (ch <= 'Z')) ch = ch - 'A' + 'a';
      result.push_back(ch);
   }
   return result;
}

template <size_t N>
[[nodiscard]] inline std::optional<double> find_keyword(const std::array<keyword, N> &Table, std::string_view Value)
{
   auto name = normalise(Value);
   for (auto &entry : Table) {
      if (entry.name IS name) return entry.value;
   }
   return std::nullopt;
}

// Parses a complete number, optionally followed by a '%' sign.  Leading '+' signs are permitted.

[[nodiscard]] inline std::optional<double> parse_number(std::string_view Value, bool *Percent = nullptr)
{
   Value = trim(Value);
   if (Percent) *Percent = false;
   if (Value.ends_with('%')) {
      if (not Percent) return std::nullopt;
      *Percent = true;
      Value = trim(Value.substr(0, Value.size() - 1));
   }
   if (Value.starts_with('+')) Value.remove_prefix(1);
   if (Value.empty()) return std::nullopt;

   double result;
   auto [ end, error ] = std::from_chars(Value.data(), Value.data() + Value.size(), result);
   if ((error != std::errc()) or (end != Value.data() + Value.size())) return std::nullopt;
   if (not std::isfinite(result)) return std::nullopt;
   return result;
}

// Weight values are a number or a weight keyword.

[[nodiscard]] inline std::optional<double> parse_weight(std::string_view Value)
{
   if (auto number = parse_number(Value)) return *number;
   return find_keyword(WEIGHT_KEYWORDS, Value);
}

// Width values are a width keyword, a percentage, or a plain number that is read as a percentage.

[[nodiscard]] inline std::optional<double> parse_width(std::string_view Value)
{
   bool percent;
   if (auto number = parse_number(Value, &percent)) return *number;
   return find_keyword(WIDTH_KEYWORDS, Value);
}

[[nodiscard]] inline std::optional<bool> parse_bool(std::string_view Value)
{
   auto name = normalise(Value);
   if ((name IS "yes") or (name IS "true") or (name IS "on") or (name IS "1")) return true;
   if ((name IS "no") or (name IS "false") or (name IS "off") or (name IS "0")) return false;
   return std::nullopt;
}

//********************************************************************************************************************
// Calls Function(Key, Value) for each ';' separated segment of a style string, with whitespace trimmed and a pair of
// enclosing quotes removed from the value.  Empty segments are skipped.  A segment without '=' is passed with an empty
// Key and the segment as the Value.  A segment with '=' always has HasValue set, even if the key or value is empty.
// Values cannot contain ';', with or without quotes.

[[nodiscard]] inline std::string_view unquote(std::string_view Value) noexcept
{
   if ((Value.size() >= 2) and ((Value.front() IS '\'') or (Value.front() IS '"')) and
         (Value.back() IS Value.front())) {
      return Value.substr(1, Value.size() - 2);
   }
   return Value;
}

template <class T> void for_each_descriptor(std::string_view Style, T &&Function)
{
   while (not Style.empty()) {
      auto end = Style.find(';');
      auto segment = trim(Style.substr(0, end));
      Style = (end IS std::string_view::npos) ? std::string_view() : Style.substr(end + 1);
      if (segment.empty()) continue;

      if (auto eq = segment.find('='); eq != std::string_view::npos) {
         Function(trim(segment.substr(0, eq)), unquote(trim(segment.substr(eq + 1))), true);
      }
      else Function(std::string_view(), segment, false);
   }
}

[[nodiscard]] inline bool is_descriptor_string(std::string_view Style) noexcept
{
   return Style.find_first_of("=;") != std::string_view::npos;
}

// A four character key is an OpenType axis tag if it is a registered axis, or if it has no lower case letters.  Other
// lower case keys are reserved for the font definition schema.  Tags are case sensitive, so `FILL` is an axis and
// `fill` is a definition key.

[[nodiscard]] inline bool is_axis_tag(std::string_view Key) noexcept
{
   if (Key.size() != 4) return false;
   if ((Key IS "wght") or (Key IS "wdth") or (Key IS "ital") or (Key IS "slnt") or (Key IS "opsz")) return true;
   for (auto ch : Key) {
      if ((ch <= 0x20) or (ch >= 0x7f) or ((ch >= 'a') and (ch <= 'z'))) return false;
   }
   return true;
}

// Definition keys describe a complete font selection.  They are valid in a style string, where they have no effect.

[[nodiscard]] inline bool is_definition_key(std::string_view Key)
{
   static constexpr std::array<std::string_view, 16> KEYS = {
      "face", "size", "features", "lineheight", "letterspacing", "wordspacing", "kerning", "fill", "opacity", "stroke",
      "strokewidth", "strokealign", "linejoin", "shadow", "glow", "blend"
   };

   if (is_axis_tag(Key)) return false;
   auto key = normalise(Key);
   for (auto entry : KEYS) {
      if (entry IS key) return true;
   }
   return false;
}

// Returns the style name declared by a style string.  The last `style` key takes precedence, then the first bare name.

[[nodiscard]] inline std::string_view declared_name(std::string_view Style)
{
   std::string_view bare, keyed;
   for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) { if (bare.empty()) bare = Value; }
      else if ((normalise(Key) IS "style") and (not Value.empty())) keyed = Value;
   });
   return keyed.empty() ? bare : keyed;
}

// Reduces a style string or font definition to the part that selects a font: the style name, in leading position, and
// its descriptors.  Definition keys are removed, so the result is suitable for caching and for merging.

[[nodiscard]] inline std::string style_part(std::string_view Style)
{
   if (not is_descriptor_string(Style)) return std::string(Style);

   std::string result(declared_name(Style));
   bool first_bare = true;
   auto append = [&result](std::string_view Key, std::string_view Value) {
      if (not result.empty()) result.push_back(';');
      if (not Key.empty()) result.append(Key).push_back('=');
      result.append(Value);
   };

   for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) {
         if (not first_bare) append({ }, Value);
         first_bare = false;
      }
      else if ((normalise(Key) != "style") and (not is_definition_key(Key))) append(Key, Value);
   });

   return result;
}

//********************************************************************************************************************
// Reduces a style name such as `Semi Condensed Bold Italic` to its traits.  Words that are not recognised clear the
// recognised flag and do not affect the result.

[[nodiscard]] inline traits name_traits(std::string_view Name)
{
   traits result;
   auto name = normalise(Name);

   auto erase = [&name](std::string_view Word) {
      if (auto pos = name.find(Word); pos != std::string::npos) {
         name.erase(pos, Word.size());
         return true;
      }
      return false;
   };

   if (erase("italic") or erase("oblique")) result.italic = true;

   // The longest matching width keyword wins, so that `semicondensed` is not read as `condensed`.

   const keyword *width = nullptr;
   for (auto &entry : WIDTH_KEYWORDS) {
      if (entry.name IS "normal") continue;
      if (name.find(entry.name) IS std::string::npos) continue;
      if ((not width) or (entry.name.size() > width->name.size())) width = &entry;
   }

   if (width) {
      erase(width->name);
      result.width = width->value;
   }

   if (not name.empty()) {
      bool found = false;
      for (auto &entry : WEIGHT_KEYWORDS) {
         if (entry.name IS name) {
            result.weight = entry.value;
            found = true;
            break;
         }
      }
      if (not found) result.recognised = false;
   }

   return result;
}

// Reduces a style string, which may be a plain name or contain descriptors, to its traits.  A Weight value that is
// non-zero and not 400 seeds the weight, and a weight descriptor takes precedence over it.  A non-zero slant is
// treated as italic, as CSS oblique styles fall back to italic faces.

[[nodiscard]] inline traits style_traits(std::string_view Style, double Weight = 0)
{
   traits result;
   std::optional<double> weight;
   if ((Weight > 0) and (Weight != 400)) weight = Weight;

   if (auto name = declared_name(Style); not name.empty()) {
      auto from_name = name_traits(name);
      result.width  = from_name.width;
      result.italic = from_name.italic;
      result.recognised = from_name.recognised;
      if (not weight) result.weight = from_name.weight;
   }

   for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) return;

      auto key = normalise(Key);
      if (key IS "weight") { if (auto v = parse_weight(Value)) weight = *v; }
      else if (key IS "width") { if (auto v = parse_width(Value)) result.width = *v; }
      else if (key IS "italic") { if (auto v = parse_bool(Value)) result.italic = *v; }
      else if (key IS "slant") { if (auto v = parse_number(Value); v and (*v != 0)) result.italic = true; }
   });

   if (weight) result.weight = *weight;
   return result;
}

//********************************************************************************************************************
// Builds the conventional style name that is nearest to the traits, e.g. `Condensed Semi Bold Italic`.  The result
// is `Regular` for normal traits.

template <size_t N>
[[nodiscard]] inline std::string_view nearest_name(const std::array<keyword, N> &Table, double Value)
{
   const keyword *best = &Table[0];
   for (auto &entry : Table) {
      if (std::abs(entry.value - Value) < std::abs(best->value - Value)) best = &entry;
   }
   return best->name;
}

[[nodiscard]] inline std::string style_name(const traits &Traits)
{
   std::string result;
   auto append = [&result](std::string_view Word) {
      if (Word.empty()) return;
      if (not result.empty()) result.push_back(' ');
      result.append(Word);
   };

   append(nearest_name(WIDTH_NAMES, Traits.width));
   append(nearest_name(WEIGHT_NAMES, Traits.weight));
   if (Traits.italic) append("Italic");
   if (result.empty()) result = "Regular";
   return result;
}

// Returns the style name to use for file selection.  A named style is used as given unless weight, width, italic or
// slant traits are requested, or Weight is non-zero and not 400.  In that case the nearest conventional name is built
// from the combined traits.

[[nodiscard]] inline std::string selection_name(std::string_view Style, double Weight = 0)
{
   auto named = declared_name(Style);
   bool traits_given = (Weight > 0) and (Weight != 400);

   for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) return;
      auto key = normalise(Key);
      if ((key IS "weight") or (key IS "width") or (key IS "italic") or (key IS "slant")) traits_given = true;
   });

   if (traits_given) return style_name(style_traits(Style, Weight));
   else if (not named.empty()) return std::string(named);
   else return "Regular";
}

//********************************************************************************************************************
// Returns the index of the candidate that best matches Target, following the CSS Fonts 4 matching order of width,
// then italic, then weight.  Returns -1 if there are no candidates.

[[nodiscard]] inline int nearest_match(const traits &Target, const std::vector<traits> &Candidates)
{
   if (Candidates.empty()) return -1;

   std::vector<int> set(Candidates.size());
   for (int i=0; i < int(Candidates.size()); i++) set[i] = i;

   // Keeps the members of the set with the best rank.  A rank of nullopt excludes the candidate.

   auto keep_best = [&](auto &&Rank) {
      std::optional<double> best;
      for (auto i : set) {
         auto rank = Rank(Candidates[i]);
         if (rank and ((not best) or (*rank < *best))) best = rank;
      }
      std::vector<int> result;
      for (auto i : set) {
         auto rank = Rank(Candidates[i]);
         if (rank and (*rank IS *best)) result.push_back(i);
      }
      set = std::move(result);
   };

   // Width: up to 100%, narrower widths are checked first (nearest first), then wider.  Above 100%, the reverse.

   const double tw = Target.width;
   keep_best([tw](const traits &C) -> std::optional<double> {
      const double distance = std::abs(C.width - tw);
      const bool preferred = (tw <= 100) ? (C.width <= tw) : (C.width >= tw);
      return preferred ? distance : 10000 + distance;
   });

   // Italic: a matching slope is preferred, then the other.

   const bool ti = Target.italic;
   keep_best([ti](const traits &C) -> std::optional<double> { return (C.italic IS ti) ? 0 : 1; });

   // Weight: from 400 to 500, heavier weights up to 500 are checked first, then lighter, then heavier than 500.
   // Below 400, lighter weights are checked first and then heavier.  Above 500, heavier first and then lighter.

   const double tg = Target.weight;
   keep_best([tg](const traits &C) -> std::optional<double> {
      const double w = C.weight;
      if (w IS tg) return 0;
      if ((tg >= 400) and (tg <= 500)) {
         if ((w > tg) and (w <= 500)) return 1000 + (w - tg);
         if (w < tg) return 2000 + (tg - w);
         return 3000 + (w - tg);
      }
      else if (tg < 400) {
         if (w < tg) return 1000 + (tg - w);
         return 2000 + (w - tg);
      }
      else {
         if (w > tg) return 1000 + (w - tg);
         return 2000 + (tg - w);
      }
   });

   return set.front();
}

} // namespace
