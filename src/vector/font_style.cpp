// Parser for font style descriptor strings.  See font_style.h for the syntax.

#include "font_style.h"
#include <algorithm>

namespace ft = kt::font_traits;

//********************************************************************************************************************

font_style_spec parse_font_style(std::string_view Style)
{
   font_style_spec spec;

   // Fast path: a plain style name is unchanged from the original behaviour.

   if (not ft::is_descriptor_string(Style)) {
      spec.named = ft::trim(Style);
      return spec;
   }

   auto warn = [&spec](std::string_view Message, std::string_view Key, std::string_view Value) {
      std::string msg(Message);
      msg.append(": '").append(Key);
      if (not Value.empty()) msg.append("=").append(Value);
      msg.append("'");
      spec.warnings.push_back(std::move(msg));
   };

   bool named_set = false;
   ft::for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) {
         if (named_set) warn("Ignored a second style name", Value, {});
         else {
            spec.named.assign(Value);
            named_set = true;
         }
         return;
      }

      if (Key.empty()) {
         warn("Missing descriptor key", Key, Value);
         return;
      }
      else if (Value.empty()) {
         warn("Missing descriptor value", Key, Value);
         return;
      }

      // 'opsz' is both a friendly key and an axis tag, so it is matched case-sensitively.

      if (Key IS "opsz") {
         auto mode = ft::normalise(Value);
         if (mode IS "auto") spec.opsz_mode = font_style_spec::OPSZ::AUTO;
         else if (mode IS "none") spec.opsz_mode = font_style_spec::OPSZ::NONE;
         else if (auto v = ft::parse_number(Value)) {
            spec.opsz_mode = font_style_spec::OPSZ::FIXED;
            spec.opsz = *v;
         }
         else { warn("Invalid opsz value", Key, Value); return; }
         spec.descriptors = true;
         return;
      }

      auto key = ft::normalise(Key);
      if (key IS "weight") {
         if (auto v = ft::parse_weight(Value)) spec.weight = *v;
         else { warn("Invalid weight value", Key, Value); return; }
      }
      else if (key IS "width") {
         if (auto v = ft::parse_width(Value)) spec.width = *v;
         else { warn("Invalid width value", Key, Value); return; }
      }
      else if (key IS "italic") {
         if (auto v = ft::parse_bool(Value)) spec.italic = *v;
         else { warn("Invalid italic value", Key, Value); return; }
      }
      else if (key IS "slant") {
         if (auto v = ft::parse_number(Value)) spec.slant = *v;
         else { warn("Invalid slant value", Key, Value); return; }
      }
      else if ((Key.size() IS 4) and std::ranges::all_of(Key, [](char C) { return (C > 0x20) and (C < 0x7f); })) {
         // Raw OpenType axis tag, matched case-sensitively
         if (auto v = ft::parse_number(Value)) spec.tags.emplace_back(make_axis_tag(Key), *v);
         else { warn("Invalid axis value", Key, Value); return; }
      }
      else { warn("Unknown style descriptor", Key, Value); return; }

      spec.descriptors = true;
   });

   return spec;
}
