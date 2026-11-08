// Parser and serialiser for font definition strings.  The schema is documented in docs/wiki/Font-Definitions.md.

#define PRV_VECTOR_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/vector.h>
#include "font_def.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace ft = kt::font_traits;

static constexpr std::array<std::string_view, 17> BLEND_MODES = {
   "normal", "screen", "multiply", "lighten", "darken", "invert", "contrast", "dodge", "burn", "hardlight",
   "softlight", "difference", "exclusion", "plus", "minus", "overlay", "over"
};

//********************************************************************************************************************

static std::string number_text(double Value)
{
   if (Value IS 0) return "0"; // Also folds negative zero
   char buffer[32];
   auto end = std::to_chars(buffer, buffer + sizeof(buffer), Value).ptr;
   return std::string(buffer, end);
}

static std::string length_text(const font_length &Length)
{
   auto text = number_text(Length.value);
   switch (Length.unit) {
      case font_length::UNIT::PX: text.append("px"); break;
      case font_length::UNIT::PT: text.append("pt"); break;
      case font_length::UNIT::EM: text.append("em"); break;
      default: break;
   }
   return text;
}

// A length without a unit reports UNIT::NONE, which callers can read as pixels or as a multiplier.

static std::optional<font_length> parse_length(std::string_view Value)
{
   Value = ft::trim(Value);
   font_length result;
   if (Value.size() > 2) {
      auto suffix = ft::normalise(Value.substr(Value.size() - 2));
      if (suffix IS "px") result.unit = font_length::UNIT::PX;
      else if (suffix IS "pt") result.unit = font_length::UNIT::PT;
      else if (suffix IS "em") result.unit = font_length::UNIT::EM;
      if (result.unit != font_length::UNIT::NONE) Value.remove_suffix(2);
   }

   auto number = ft::parse_number(Value);
   if (not number) return std::nullopt;
   result.value = *number;
   return result;
}

static bool valid_colour(std::string_view Value)
{
   VectorPainter painter;
   return vec::ReadPainter(nullptr, Value, &painter, nullptr) IS ERR::Okay;
}

static bool valid_painter(std::string_view Value)
{
   if (ft::normalise(Value) IS "none") return true;
   if (Value.starts_with("url(")) return Value.find(')') != std::string_view::npos; // Resolved when applied to a scene
   return valid_colour(Value);
}

// Splits on a delimiter that is outside of brackets, passing each trimmed segment to Function.

template <class T> static void split_outer(std::string_view Value, char Delimiter, T &&Function)
{
   int depth = 0;
   size_t start = 0;
   for (size_t i=0; i <= Value.size(); i++) {
      const char ch = (i < Value.size()) ? Value[i] : Delimiter;
      if (ch IS '(') depth++;
      else if (ch IS ')') depth--;
      else if ((depth <= 0) and ((ch IS Delimiter) or ((Delimiter IS ' ') and (ch IS '\t')))) {
         auto segment = ft::trim(Value.substr(start, i - start));
         if (not segment.empty()) Function(segment);
         start = i + 1;
      }
   }
}

//********************************************************************************************************************
// Numbers are collected in order and the single colour may lead or trail, as in CSS.

static bool read_shadow_layer(std::string_view Value, font_shadow_layer &Layer)
{
   std::vector<double> lengths;
   std::string colour;
   bool valid = true;

   split_outer(Value, ' ', [&](std::string_view Token) {
      if (auto length = parse_length(Token)) {
         if (length->unit IS font_length::UNIT::EM) valid = false; // A shadow has no font size to be relative to
         else lengths.push_back(length->pixels(0));
      }
      else if (colour.empty() and valid_colour(Token)) colour = Token;
      else valid = false;
   });

   if ((not valid) or (lengths.size() < 2) or (lengths.size() > 3)) return false;
   Layer.x = lengths[0];
   Layer.y = lengths[1];
   Layer.blur = (lengths.size() > 2) ? lengths[2] : 0;
   Layer.colour = colour.empty() ? "#000000" : colour;
   return Layer.blur >= 0;
}

std::vector<font_shadow_layer> decode_shadow(std::string_view Value)
{
   std::vector<font_shadow_layer> layers;
   if (ft::normalise(Value) IS "none") return layers;
   split_outer(Value, ',', [&](std::string_view Segment) {
      font_shadow_layer layer;
      if (read_shadow_layer(Segment, layer)) layers.push_back(std::move(layer));
   });
   return layers;
}

static std::optional<std::string> normalise_shadow(std::string_view Value)
{
   if (ft::normalise(Value) IS "none") return "none";

   std::string result;
   bool valid = true;
   int count = 0;
   split_outer(Value, ',', [&](std::string_view Segment) {
      font_shadow_layer layer;
      if (not read_shadow_layer(Segment, layer)) { valid = false; return; }
      if (count++) result.append(", ");
      result.append(number_text(layer.x)).append("px ").append(number_text(layer.y)).append("px ");
      result.append(number_text(layer.blur)).append("px ").append(layer.colour);
   });
   if ((not valid) or (not count)) return std::nullopt;
   return result;
}

bool decode_glow(std::string_view Value, double &Radius, std::string &Colour)
{
   Value = ft::trim(Value);
   auto space = Value.find_first_of(" \t");
   if (space IS std::string_view::npos) return false;

   auto radius = parse_length(Value.substr(0, space));
   auto colour = ft::trim(Value.substr(space + 1));
   if ((not radius) or (radius->unit IS font_length::UNIT::EM) or (radius->value < 0)) return false;
   if (not valid_colour(colour)) return false;
   Radius = radius->pixels(0);
   Colour = colour;
   return true;
}

static std::optional<std::string> normalise_glow(std::string_view Value)
{
   if (ft::normalise(Value) IS "none") return "none";
   double radius;
   std::string colour;
   if (not decode_glow(Value, radius, colour)) return std::nullopt;
   return number_text(radius) + "px " + colour;
}

// Features are sorted by tag, so that equivalent lists are identical strings.  A later tag replaces an earlier one.

static std::optional<std::string> normalise_features(std::string_view Value)
{
   std::map<std::string, int> tags;
   bool valid = true;

   split_outer(Value, ',', [&](std::string_view Segment) {
      auto space = Segment.find_first_of(" \t");
      auto tag = Segment.substr(0, space);
      auto setting = (space IS std::string_view::npos) ? std::string_view() : ft::trim(Segment.substr(space + 1));
      if ((tag.size() != 4) or std::ranges::any_of(tag, [](char C) { return (C <= 0x20) or (C >= 0x7f); })) {
         valid = false;
         return;
      }

      int number = 1;
      if (auto flag = ft::parse_bool(setting)) number = *flag ? 1 : 0;
      else if (not setting.empty()) {
         auto parsed = ft::parse_number(setting);
         if ((not parsed) or (*parsed < 0) or (*parsed != std::floor(*parsed))) { valid = false; return; }
         number = int(*parsed);
      }
      tags[std::string(tag)] = number;
   });

   if ((not valid) or tags.empty()) return std::nullopt;

   std::string result;
   for (auto & [ tag, number ] : tags) {
      if (not result.empty()) result.append(", ");
      result.append(tag).append(" ").append(std::to_string(number));
   }
   return result;
}

// Returns the canonical name of a blend mode.  CSS spellings of the Porter-Duff and separable modes are accepted.

static std::optional<std::string> normalise_blend(std::string_view Value)
{
   auto name = ft::normalise(Value);
   if ((name IS "colourburn") or (name IS "colorburn")) name = "burn";
   else if ((name IS "colourdodge") or (name IS "colordodge")) name = "dodge";
   else if (name IS "subtract") name = "minus";
   else if (name IS "over") name = "normal";

   if (std::ranges::find(BLEND_MODES, std::string_view(name)) IS BLEND_MODES.end()) return std::nullopt;
   return name;
}

static std::optional<std::string> one_of(std::string_view Value, std::initializer_list<std::string_view> Choices)
{
   auto name = ft::normalise(Value);
   for (auto choice : Choices) {
      if (choice IS name) return name;
   }
   return std::nullopt;
}

//********************************************************************************************************************

ERR parse_font_def(kt::Log &Log, std::string_view String, font_def &Def)
{
   font_def def;
   def.style = parse_font_style(String);
   for (auto &warning : def.style.warnings) Log.warning("%s", warning.c_str());

   bool valid = not def.style.invalid;
   if (def.style.weight and ((*def.style.weight < 1) or (*def.style.weight > 1000))) valid = false;
   if (def.style.width and (*def.style.width <= 0)) valid = false;

   auto &tags = def.style.tags; // Later declarations of an axis replace earlier ones
   for (auto it = tags.begin(); it != tags.end(); ) {
      auto later = std::find_if(it + 1, tags.end(), [&](auto &Entry) { return Entry.first IS it->first; });
      it = (later != tags.end()) ? tags.erase(it) : it + 1;
   }

   auto reject = [&](std::string_view Key, std::string_view Value) {
      Log.warning("Invalid value for '%.*s': '%.*s'", int(Key.size()), Key.data(), int(Value.size()), Value.data());
      valid = false;
   };

   auto store = [&]<class T>(std::optional<T> &Target, std::optional<T> &&Parsed, std::string_view Key,
         std::string_view Value) {
      if (Parsed) Target = std::move(Parsed);
      else reject(Key, Value);
   };

   ft::for_each_descriptor(String, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) return;
      const auto key = ft::normalise(Key);
      const bool style_key = (key IS "style") or (key IS "weight") or (key IS "width") or (key IS "italic") or
         (key IS "slant") or (Key IS "opsz") or ft::is_axis_tag(Key);

      if ((not style_key) and (not ft::is_definition_key(Key))) return; // Already warned about by the style parser

      if (Value.empty() or (Value.front() IS '\'') or (Value.front() IS '"')) { // Empty, or an unbalanced quote
         reject(Key, Value);
         return;
      }

      if (style_key) return;

      if (key IS "face") def.face = std::string(Value);
      else if (key IS "size") {
         auto length = parse_length(Value);
         if ((not length) or (length->value <= 0) or (length->unit IS font_length::UNIT::EM)) reject(Key, Value);
         else {
            if (length->unit IS font_length::UNIT::NONE) length->unit = font_length::UNIT::PX;
            def.size = length;
         }
      }
      else if (key IS "features") {
         if (ft::normalise(Value) IS "none") def.features = "none";
         else store(def.features, normalise_features(Value), Key, Value);
      }
      else if (key IS "lineheight") {
         auto length = parse_length(Value);
         if ((not length) or (length->value < 0)) reject(Key, Value);
         else def.line_height = length;
      }
      else if ((key IS "letterspacing") or (key IS "wordspacing") or (key IS "strokewidth")) {
         auto length = parse_length(Value);
         if ((not length) or ((key IS "strokewidth") and (length->value < 0))) { reject(Key, Value); return; }
         if (length->unit IS font_length::UNIT::NONE) length->unit = font_length::UNIT::PX;
         if (key IS "letterspacing") def.letter_spacing = length;
         else if (key IS "wordspacing") def.word_spacing = length;
         else def.stroke_width = length;
      }
      else if (key IS "kerning") store(def.kerning, one_of(Value, { "metrics", "none", "optical" }), Key, Value);
      else if ((key IS "fill") or (key IS "stroke")) {
         if (not valid_painter(Value)) reject(Key, Value);
         else if (key IS "fill") def.fill = std::string(Value);
         else def.stroke = std::string(Value);
      }
      else if (key IS "opacity") {
         bool percent;
         auto number = ft::parse_number(Value, &percent);
         if (number and percent) *number /= 100.0;
         if ((not number) or (*number < 0) or (*number > 1)) reject(Key, Value);
         else def.opacity = *number;
      }
      else if (key IS "strokealign") {
         auto align = one_of(Value, { "centre", "center", "inside", "outside" });
         if (align and (*align IS "center")) align = "centre";
         store(def.stroke_align, std::move(align), Key, Value);
      }
      else if (key IS "linejoin") {
         auto join = one_of(Value, { "miter", "mitre", "round", "bevel" });
         if (join and (*join IS "mitre")) join = "miter";
         store(def.line_join, std::move(join), Key, Value);
      }
      else if (key IS "shadow") store(def.shadow, normalise_shadow(Value), Key, Value);
      else if (key IS "glow") store(def.glow, normalise_glow(Value), Key, Value);
      else if (key IS "blend") store(def.blend, normalise_blend(Value), Key, Value);
   });

   if (not valid) return ERR::Syntax;
   Def = std::move(def);
   return ERR::Okay;
}

//********************************************************************************************************************

std::vector<std::pair<std::string, std::string>> font_def::entries() const
{
   std::vector<std::pair<std::string, std::string>> list;
   auto text   = [&](const char *Key, const std::optional<std::string> &V) { if (V) list.emplace_back(Key, *V); };
   auto length = [&](const char *Key, const std::optional<font_length> &V) {
      if (V) list.emplace_back(Key, length_text(*V));
   };

   text("face", face);
   length("size", size);

   if (not style.named.empty()) list.emplace_back("style", style.named);
   if (style.weight) list.emplace_back("weight", number_text(*style.weight));
   if (style.width) list.emplace_back("width", number_text(*style.width) + "%");
   if (style.italic) list.emplace_back("italic", *style.italic ? "yes" : "no");
   if (style.slant) list.emplace_back("slant", number_text(*style.slant));
   if (style.opsz_set) {
      if (style.opsz_mode IS font_style_spec::OPSZ::AUTO) list.emplace_back("opsz", "auto");
      else if (style.opsz_mode IS font_style_spec::OPSZ::NONE) list.emplace_back("opsz", "none");
      else list.emplace_back("opsz", number_text(style.opsz));
   }

   std::map<std::string, double> tags;
   for (auto & [ tag, value ] : style.tags) tags[axis_tag_name(tag)] = value;
   for (auto & [ tag, value ] : tags) list.emplace_back(tag, number_text(value));

   text("features", features);
   length("lineHeight", line_height);
   length("letterSpacing", letter_spacing);
   length("wordSpacing", word_spacing);
   text("kerning", kerning);
   text("fill", fill);
   if (opacity) list.emplace_back("opacity", number_text(*opacity));
   text("stroke", stroke);
   length("strokeWidth", stroke_width);
   text("strokeAlign", stroke_align);
   text("lineJoin", line_join);
   text("shadow", shadow);
   text("glow", glow);
   text("blend", blend);
   return list;
}

std::string font_def::serialise() const
{
   std::string result;
   for (auto & [ key, value ] : entries()) {
      if (not result.empty()) result.append("; ");
      result.append(key).append("=").append(value);
   }
   return result;
}

std::string font_def::style_string() const
{
   font_def part;
   part.style = style;
   part.style.named.clear();

   std::string result(style.named);
   for (auto & [ key, value ] : part.entries()) {
      if (not result.empty()) result.push_back(';');
      result.append(key).append("=").append(value);
   }
   return result;
}

void font_def::merge(const font_def &Other)
{
   auto overlay = [](auto &Target, const auto &Source) { if (Source) Target = Source; };

   overlay(face, Other.face);
   overlay(size, Other.size);
   overlay(features, Other.features);
   overlay(line_height, Other.line_height);
   overlay(letter_spacing, Other.letter_spacing);
   overlay(word_spacing, Other.word_spacing);
   overlay(kerning, Other.kerning);
   overlay(fill, Other.fill);
   overlay(opacity, Other.opacity);
   overlay(stroke, Other.stroke);
   overlay(stroke_width, Other.stroke_width);
   overlay(stroke_align, Other.stroke_align);
   overlay(line_join, Other.line_join);
   overlay(shadow, Other.shadow);
   overlay(glow, Other.glow);
   overlay(blend, Other.blend);

   if (not Other.style.named.empty()) style.named = Other.style.named;
   overlay(style.weight, Other.style.weight);
   overlay(style.width, Other.style.width);
   overlay(style.slant, Other.style.slant);
   overlay(style.italic, Other.style.italic);
   if (Other.style.opsz_set) {
      style.opsz_set  = true;
      style.opsz_mode = Other.style.opsz_mode;
      style.opsz      = Other.style.opsz;
   }

   for (auto &entry : Other.style.tags) {
      auto it = std::ranges::find_if(style.tags, [&](auto &Tag) { return Tag.first IS entry.first; });
      if (it != style.tags.end()) it->second = entry.second;
      else style.tags.push_back(entry);
   }
}
