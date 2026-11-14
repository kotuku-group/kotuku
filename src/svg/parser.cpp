//********************************************************************************************************************

void svgState::proc_paint_server(const XTag &Tag) noexcept
{
   switch(svg_tag_hash(Tag)) {
      case SVF_contourGradient:   proc_contourgradient(Tag); break;
      case SVF_distalGradient:    proc_distalgradient(Tag); break;
      case SVF_radialGradient:    proc_radialgradient(Tag); break;
      case SVF_diamondGradient:   proc_diamondgradient(Tag); break;
      case SVF_conicGradient:     proc_conicgradient(Tag); break;
      case SVF_linearGradient:    proc_lineargradient(Tag); break;
      case SVF_meshgradient:      proc_meshgradient(Tag); break;
      case SVF_diffusionGradient: proc_diffusiongradient(Tag); break;
   }
}

//********************************************************************************************************************
// Within a <use> parameter scope, a paint server whose subtree or href chain references parameters must be instantiated
// separately for each distinct set of values.  Returns the ID of the scoped instance, or an empty string if the
// document-scope definition can be used as-is.  Instances with equal values share one definition.

std::string svgState::scoped_paint_id(const XTag &Tag, const std::string &ID) noexcept
{
   // Other definitions, including patterns, retain their document values.

   switch (svg_tag_hash(Tag)) {
      case SVF_contourGradient: case SVF_distalGradient: case SVF_radialGradient: case SVF_diamondGradient:
      case SVF_conicGradient: case SVF_linearGradient: case SVF_meshgradient: case SVF_diffusionGradient:
         break;
      default: return std::string();
   }

   auto deps = Self->ParamDeps.find(ID);
   if (deps IS Self->ParamDeps.end()) {
      kt::vector<std::string> names;
      std::vector<const XTag *> referenced = { &Tag };
      for (size_t i=0; i < referenced.size(); i++) {
         auto &tag = *referenced[i];
         collect_param_names(Self, tag, names);
         for (int a=1; a < std::ssize(tag.Attribs); a++) {
            auto hash = strhash(tag.Attribs[a].Name);
            if ((hash != SVF_href) and (hash != SVF_xlink_href)) continue;
            if (auto other = find_href_tag(Self, tag.Attribs[a].Value)) {
               if (std::ranges::find(referenced, other) IS referenced.end()) referenced.push_back(other);
            }
         }
      }
      std::sort(names.begin(), names.end());
      names.erase(std::unique(names.begin(), names.end()), names.end());
      deps = Self->ParamDeps.emplace(ID, std::move(names)).first;
   }

   if (deps->second.empty()) return std::string();

   std::string key;
   bool differs = false;
   for (auto &name : deps->second) {
      auto scoped = find_param(Self, m_param_scope, name);
      auto original = Self->ParsedParams.find(name);
      auto document = (original != Self->ParsedParams.end()) ? &original->second : nullptr;
      if ((scoped != document) and ((not scoped) or (not document) or (*scoped != *document))) differs = true;

      key.append(name).push_back('\x1f');
      if (scoped) key.append(*scoped);
      else key.push_back('\x1e');
      key.push_back('\x1d');
   }

   if (not differs) return std::string();
   return std::format("{}~p{:x}", ID, uint64_t(ankerl::unordered_dense::hash<std::string>{}(key)));
}

//********************************************************************************************************************
// If the value is a url() then the referenced paint server will be initialised if it has not been already.  Returns
// the effective paint value, which differs from the input if a parameter-scoped instance of the paint server is used.

std::string svgState::resolve_paint_server(const std::string &Value) noexcept
{
   if (not Value.starts_with("url(#")) return Value;

   auto end = Value.find(')', 5);
   if (end IS std::string::npos) return Value;

   std::string lookup(Value, 5, end - 5);
   auto tag = Self->IDs.find(lookup);
   if (tag IS Self->IDs.end()) return Value;

   if ((m_param_scope) and (not Self->ParamAttribs.empty())) {
      if (auto scoped_id = scoped_paint_id(*tag->second, lookup); not scoped_id.empty()) {
         if (Self->Scene->findDef(scoped_id, nullptr) != ERR::Okay) {
            XTag clone = *tag->second;
            rescope_params(Self, clone, m_param_scope);
            for (int a=1; a < std::ssize(clone.Attribs); a++) {
               if (clone.Attribs[a].Name IS "id") { clone.Attribs[a].Value = scoped_id; break; }
            }
            proc_paint_server(clone);
         }

         return "url(#" + scoped_id + Value.substr(end);
      }
   }

   if (Self->Scene->findDef(lookup, nullptr) != ERR::Okay) proc_paint_server(*tag->second);
   return Value;
}

//********************************************************************************************************************
// This function was created to manage inheritance for paint servers in the <defs> section.
//
// Use of 'inherit' is replaced with the actual state value at the time of the call.
// Use of 'currentColor' is also replaced to a deeper level, because it plays by different rules to inherit.

void svgState::process_inherit_refs(XTag &Tag) noexcept
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("inherit", Tag.Attribs[a].Value)) {
         switch (strhash(Tag.Attribs[a].Name)) {
            case SVF_stop_color:
               Tag.Attribs[a].Value = m_stop_color;
               break;
            case SVF_stop_opacity: Tag.Attribs[a].Value = std::to_string(m_stop_opacity); break;
         }
      }
      else if (iequals("currentColor", Tag.Attribs[a].Value)) {
         Tag.Attribs[a].Value = m_color;
      }
   }

   // Replace all use of currentColor in child tags

   std::function<void(kt::vector<XTag> &, const std::string &)> process_color;

   process_color = [&process_color](kt::vector<XTag> &Tags, const std::string &Colour) {
      for (auto &scan : Tags) {
         if (!scan.isTag()) continue;
         for (int a=1; a < std::ssize(scan.Attribs); a++) {
            if (iequals("currentColor", scan.Attribs[a].Value)) {
               scan.Attribs[a].Value = Colour;
            }
         }

         if (!scan.Children.empty()) process_color(scan.Children, Colour);
      }
   };

   if (!Tag.Children.empty()) {
      process_color(Tag.Children, m_color);
   }
}

//********************************************************************************************************************
// Aspect ratios are case insensitive

static ARF parse_aspect_ratio(std::string_view Value)
{
   Value.remove_prefix(std::min(Value.find_first_not_of(" \t\r\n"), Value.size()));

   if (startswith("defer", Value)) {
      Value.remove_prefix(5);
      Value.remove_prefix(std::min(Value.find_first_not_of(" \t\r\n"), Value.size()));
   }

   if (iequals("none", Value)) return ARF::NONE;

   ARF flags = ARF::NIL;
   if (startswith("xMin", Value)) { flags |= ARF::X_MIN; Value.remove_prefix(4); }
   else if (startswith("xMid", Value)) { flags |= ARF::X_MID; Value.remove_prefix(4); }
   else if (startswith("xMax", Value)) { flags |= ARF::X_MAX; Value.remove_prefix(4); }

   if (startswith("yMin", Value)) { flags |= ARF::Y_MIN; Value.remove_prefix(4); }
   else if (startswith("yMid", Value)) { flags |= ARF::Y_MID; Value.remove_prefix(4); }
   else if (startswith("yMax", Value)) { flags |= ARF::Y_MAX; Value.remove_prefix(4); }

   Value.remove_prefix(std::min(Value.find_first_not_of(" \t\r\n"), Value.size()));

   if (startswith("meet", Value)) { flags |= ARF::MEET; }
   else if (startswith("slice", Value)) { flags |= ARF::SLICE; }

   if (flags IS ARF::NIL) return ARF::X_MID|ARF::Y_MID|ARF::MEET;
   if ((flags & (ARF::MEET|ARF::SLICE)) IS ARF::NIL) flags |= ARF::MEET; // Alignment without a fit mode implies 'meet'
   return flags;
}

//********************************************************************************************************************

static RQ shape_rendering_to_render_quality(const std::string_view Value)
{
   if (iequals(Value, "auto")) return RQ::AUTO;
   else if (iequals(Value, "optimize-speed")) return RQ::FAST;
   else if (iequals(Value, "optimizeSpeed")) return RQ::FAST;
   else if (iequals(Value, "crisp-edges")) return RQ::CRISP;
   else if (iequals(Value, "crispEdges")) return RQ::CRISP;
   else if (iequals(Value, "geometric-precision")) return RQ::PRECISE;
   else if (iequals(Value, "geometricPrecision")) return RQ::PRECISE;
   else if (iequals(Value, "best")) return RQ::BEST;
   else {
      kt::Log log;
      log.warning("Unknown shape-rendering value '%s'", std::string(Value).c_str());
   }

   return RQ::AUTO;
}

//********************************************************************************************************************
// The supported inherited CSS subset.  Layout attributes (x/y and positioning lists) are deliberately excluded.

static const char * text_property_initial(uint32_t Hash)
{
   switch (Hash) {
      case SVF_inline_size: return "auto";
      case SVF_text_align: return "start";
      case SVF_white_space:
      case SVF_overflow_wrap:
      case SVF_word_break:
      case SVF_letter_spacing:
      case SVF_word_spacing:
      case SVF_line_height:
      case SVF_font_variant: return "normal";
      case SVF_font_family: return "Noto Sans";
      case SVF_font_size: return "12pt";
      case SVF_font_weight: return "400";
      case SVF_font_style:
      case SVF_font_stretch:
      case SVF_font_width:
      case SVF_font_variation_settings:
      case SVF_font_feature_settings: return "normal";
      case SVF_font_optical_sizing:
      case SVF_font_kerning:
      case SVF_kerning: return "auto";
      case SVF_text_anchor: return "start";
      case SVF_text_decoration: return "none";
      default: return nullptr;
   }
}

// Read complete CSS numeric tokens, including exponents, before converting only the supported units.
// ex uses the CSS half-em fallback: VectorText exposes no unrounded x-height metric.  Other metric units
// (ch, cap, ic) and viewport units are rejected rather than assigned an unrelated reference.

static bool text_length(std::string_view Value, double FontSize, double RootSize, bool Percentage, double &Pixels)
{
   Value = kt::font_traits::trim(Value);
   auto number_text = Value;
   if (number_text.starts_with('+')) number_text.remove_prefix(1);
   double number;
   auto [end, error] = std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
   if ((error != std::errc()) or not std::isfinite(number)) return false;
   auto unit = number_text.substr(end - number_text.data());
   if (unit IS "%") {
      if (not Percentage) return false;
      Pixels = number * FontSize * 0.01;
   }
   else if (unit.empty() or iequals(unit, "px")) Pixels = number;
   else if (iequals(unit, "rem")) Pixels = number * RootSize;
   else if (iequals(unit, "em")) Pixels = number * FontSize;
   else if (iequals(unit, "ex")) Pixels = number * FontSize * 0.5;
   else if (iequals(unit, "in")) Pixels = number * 96;
   else if (iequals(unit, "cm")) Pixels = number * (96 / 2.54);
   else if (iequals(unit, "mm")) Pixels = number * (96 / 25.4);
   else if (iequals(unit, "q")) Pixels = number * (96 / 101.6);
   else if (iequals(unit, "pt")) Pixels = number * (96.0 / 72);
   else if (iequals(unit, "pc")) Pixels = number * 16;
   else return false;
   return std::isfinite(Pixels);
}

static std::string text_number(double Number)
{
   char buffer[64];
   auto end = std::to_chars(buffer, buffer + sizeof(buffer), Number).ptr;
   return std::string(buffer, end);
}

// Face consumes CSV rather than CSS strings.  Reject names that cannot be represented without changing the list.

static bool text_family(std::string_view Value, std::string &Result)
{
   Result.clear();
   while (not Value.empty()) {
      Value = kt::font_traits::trim(Value);
      if (Value.empty()) return false;
      std::string name;
      if ((Value.front() IS '\'') or (Value.front() IS '"')) {
         const char quote = Value.front();
         Value.remove_prefix(1);
         auto end = Value.find(quote);
         if (end IS std::string_view::npos) return false;
         name = Value.substr(0, end);
         Value = kt::font_traits::trim(Value.substr(end + 1));
         if (not Value.empty() and Value.front() != ',') return false;
      }
      else {
         auto end = Value.find(',');
         auto family = kt::font_traits::trim(Value.substr(0, end));
         bool space = false;
         for (auto c : family) {
            if ((c IS ' ') or (c IS '\t') or (c IS '\r') or (c IS '\n')) space = true;
            else {
               if (not ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or
                   (c >= '0' and c <= '9') or c IS '-' or c IS '_' or (unsigned char)c >= 128)) return false;
               if (space and not name.empty()) name.push_back(' ');
               name.push_back(c);
               space = false;
            }
         }
         Value = end IS std::string_view::npos ? std::string_view() : Value.substr(end);
      }
      if (name.empty() or name.find_first_of(",\\\"'") != std::string::npos) return false;
      if (not Result.empty()) Result.push_back(',');
      Result.append(name);
      if (Value.empty()) return true;
      Value.remove_prefix(1);
      if (kt::font_traits::trim(Value).empty()) return false;
   }
   return false;
}

// Expand the supported ordinary font grammar atomically, retaining its declaration position in the cascade.

static bool expand_text_font(std::string_view Value, std::vector<XMLAttrib> &Result)
{
   using kt::font_traits::trim;
   Value = trim(Value);
   const char *names[] = { "font-style", "font-variant", "font-weight", "font-stretch", "font-size",
      "line-height", "font-family", "font-kerning", "font-variation-settings", "font-optical-sizing",
      "font-feature-settings" };
   std::string values[] = { "normal", "normal", "normal", "normal", "", "normal", "", "auto", "normal", "auto",
      "normal" };
   if (iequals(Value, "inherit") or iequals(Value, "initial") or iequals(Value, "unset")) {
      for (auto name : names) Result.push_back(XMLAttrib { name, std::string(Value) });
      return true;
   }
   auto token = [&Value]() {
      Value = trim(Value);
      auto end = Value.find_first_of(" \t\r\n/");
      auto word = Value.substr(0, end);
      Value.remove_prefix(word.size());
      return word;
   };
   bool assigned[4] = {};
   int normals = 0;
   while (not Value.empty()) {
      auto word = token();
      if (word.empty()) return false;
      double size;
      const bool size_keyword = iequals(word, "xx-small") or iequals(word, "x-small") or
         iequals(word, "small") or iequals(word, "medium") or iequals(word, "large") or iequals(word, "x-large") or
         iequals(word, "xx-large") or iequals(word, "xxx-large") or iequals(word, "larger") or iequals(word, "smaller");
      // Percentages in this position are font sizes, never font-stretch.
      if (size_keyword or (not kt::font_traits::parse_number(word) and
          text_length(word, 16, 16, true, size))) {
         values[4] = word;
         break;
      }
      int component;
      if (iequals(word, "normal")) { if (++normals > 4) return false; continue; }
      if (iequals(word, "italic") or iequals(word, "oblique")) component = 0;
      else if (iequals(word, "bold") or iequals(word, "bolder") or iequals(word, "lighter") or
               kt::font_traits::parse_number(word)) component = 2;
      else if (kt::font_traits::parse_width(word)) component = 3;
      else return false; // Includes unrenderable small-caps and system-font forms.
      if (assigned[component]) return false;
      assigned[component] = true;
      values[component] = word;
      if ((component IS 0) and iequals(word, "oblique")) {
         auto remaining = Value;
         auto angle = token();
         if (angle.ends_with("deg")) values[0].append(" ").append(angle);
         else Value = remaining;
      }
   }
   if (values[4].empty()) return false;
   int components = normals;
   for (auto assigned_component : assigned) if (assigned_component) ++components;
   if (components > 4) return false;
   Value = trim(Value);
   if (Value.starts_with('/')) {
      Value.remove_prefix(1);
      values[5] = token();
      if (values[5].empty()) return false;
   }
   std::string family;
   if (not text_family(trim(Value), family)) return false;
   values[6] = trim(Value);
   for (size_t i = 0; i < std::size(names); ++i) Result.push_back(XMLAttrib { names[i], values[i] });
   return true;
}

static constexpr auto glDecorationMask = VTXF::UNDERLINE | VTXF::OVERLINE | VTXF::LINE_THROUGH | VTXF::BLINK;

static bool decoration_flags(std::string_view Value, VTXF &Flags)
{
   Flags = VTXF::NIL;
   Value = kt::font_traits::trim(Value);
   if (iequals(Value, "none")) return true;
   if (Value.empty()) return false;
   while (not Value.empty()) {
      auto end = Value.find_first_of(" \t\r\n");
      auto word = Value.substr(0, end);
      VTXF flag;
      if (iequals(word, "underline")) flag = VTXF::UNDERLINE;
      else if (iequals(word, "overline")) flag = VTXF::OVERLINE;
      else if (iequals(word, "line-through")) flag = VTXF::LINE_THROUGH;
      else if (iequals(word, "blink")) flag = VTXF::BLINK;
      else return false;
      if ((Flags & flag) != VTXF::NIL) return false;
      Flags |= flag;
      if (end IS std::string_view::npos) break;
      Value = kt::font_traits::trim(Value.substr(end));
   }
   return true;
}

//********************************************************************************************************************
// Converts a CSS font property to VectorText FontStyle descriptors, e.g. 'font-stretch: condensed' to 'width=75'.
// Returns false if the property is unsupported or the value is 'inherit', in which case the style is unchanged.  An
// empty Result is valid and resets the property, e.g. 'font-style: normal'.

static bool font_descriptors(uint32_t Hash, std::string_view Value, std::string &Result)
{
   namespace ft = kt::font_traits;
   Value = ft::trim(Value);
   Result.clear();
   if (iequals("inherit", Value)) return false;

   auto format_number = [](double Number) {
      char buffer[32];
      auto end = std::to_chars(buffer, buffer + sizeof(buffer), Number).ptr;
      return std::string(buffer, end);
   };

   switch (Hash) {
      case SVF_font_stretch:
      case SVF_font_width: // CSS: <percentage> | normal | ultra-condensed ... ultra-expanded
         if (auto width = ft::parse_width(Value)) {
            if (*width <= 0) return false;
            if (*width != 100) Result = "width=" + format_number(*width);
            return true;
         }
         break;

      case SVF_font_style: { // CSS: normal | italic | oblique <angle>?
         auto keyword = Value.substr(0, Value.find_first_of(" \t\r\n"));
         if (iequals("normal", Value)) return true;
         else if (iequals("italic", Value)) { Result = "italic=yes"; return true; }
         else if (iequals("oblique", keyword)) {
            auto angle = ft::trim(Value.substr(keyword.size()));
            if (angle.empty()) { Result = "slant=14"; return true; } // CSS default oblique angle
            const bool degrees = angle.ends_with("deg");
            if (degrees) angle.remove_suffix(3);
            if (auto number = ft::parse_number(angle)) {
               if ((not degrees and (*number != 0)) or (*number < -90) or (*number > 90)) return false;
               if (*number != 0) Result = "slant=" + format_number(*number);
               return true;
            }
         }
         break;
      }

      case SVF_font_variation_settings: { // CSS: normal | [ <string> <number> ]#
         if (iequals("normal", Value)) return true;
         std::string_view list(Value);
         while (not list.empty()) {
            auto end = list.find(',');
            auto setting = ft::trim(list.substr(0, end));
            list = (end IS std::string_view::npos) ? std::string_view() : list.substr(end + 1);
            if (setting.size() < 7) return false; // Minimum of "abcd" 0

            const char quote = setting.front();
            if (((quote != '"') and (quote != '\'')) or (setting[5] != quote)) {
               return false;
            }

            if ((setting[6] != ' ') and (setting[6] != '\t') and (setting[6] != '\r') and
                (setting[6] != '\n')) return false;
            for (auto c : setting.substr(1, 4)) {
               if ((c < 0x20) or (c > 0x7e) or (c IS ';') or (c IS '=')) return false;
            }
            if (auto number = ft::parse_number(setting.substr(6))) {
               if (not Result.empty()) Result.push_back(';');
               Result.append(setting.substr(1, 4)).append("=").append(format_number(*number));
            }
            else return false;
            if ((end != std::string_view::npos) and ft::trim(list).empty()) return false;
         }
         return true;
      }

      case SVF_font_optical_sizing: // CSS: auto | none
         if (iequals("auto", Value)) return true;
         else if (iequals("none", Value)) { Result = "opsz=none"; return true; }
         break;

      default:
         return false;
   }

   return false;
}

//********************************************************************************************************************
// Converts CSS font-feature-settings to the value of a font definition's `features` key, e.g. `"liga" 0, "tnum"` to
// `liga 0, tnum 1`.  Returns false if the value is malformed, in which case the declaration is ignored.  `normal`
// produces an empty Result.  Tags that a font definition cannot express, such as those containing spaces, quotes or
// brackets, are valid CSS but no font defines them, so they are skipped.  Duplicate tags are resolved by the font
// definition, where the last one wins as it does in CSS.
//
// CSS: normal | [ <string> [ <integer [0,∞]> | on | off ]? ]#

static bool feature_settings(std::string_view Value, std::string &Result)
{
   namespace ft = kt::font_traits;
   Result.clear();
   Value = ft::trim(Value);
   if (iequals("normal", Value)) return true;

   while (true) {
      // The quoted tag is read first, as it can contain a comma.

      Value = ft::trim(Value);
      if (Value.size() < 6) return false; // Minimum of "abcd"
      const char quote = Value.front();
      if (((quote != '"') and (quote != '\'')) or (Value[5] != quote)) return false;
      auto tag = Value.substr(1, 4);
      if (std::ranges::any_of(tag, [](char C) { return (C < 0x20) or (C > 0x7e); })) return false;
      Value.remove_prefix(6);

      const auto end = Value.find(',');
      auto option = ft::trim(Value.substr(0, end));
      uint32_t number = 1;
      if (iequals(option, "on")) number = 1;
      else if (iequals(option, "off")) number = 0;
      else if (not option.empty()) {
         if (option.starts_with('+')) option.remove_prefix(1);
         auto [ ptr, error ] = std::from_chars(option.data(), option.data() + option.size(), number);
         if ((error != std::errc()) or (ptr != option.data() + option.size())) return false;
      }

      if (tag.find_first_of(" ,;=()'\"") IS std::string_view::npos) { // Characters with meaning in a definition
         if (not Result.empty()) Result.append(", ");
         Result.append(tag).append(" ").append(std::to_string(number));
      }

      if (end IS std::string_view::npos) return true;
      Value.remove_prefix(end + 1);
   }
}

//********************************************************************************************************************
// Replaces the descriptors that are owned by the CSS font property Hash in Style with Descriptors.  This allows the
// properties to be combined and inherited independently, so that a child's font-stretch does not discard a parent's
// font-style.

static void merge_font_style(std::string &Style, uint32_t Hash, std::string_view Descriptors)
{
   namespace ft = kt::font_traits;

   auto owned = [Hash](std::string_view Key, std::string_view Value) {
      auto key = ft::normalise(Key);
      // Numeric opsz values belong to variation-settings; auto/none belong to optical-sizing.
      const bool optical_coordinate = (Key IS "opsz") and ft::parse_number(Value).has_value();
      switch (Hash) {
         case SVF_font_stretch:
         case SVF_font_width:              return key IS "width";
         case SVF_font_style:              return (key IS "italic") or (key IS "slant");
         case SVF_font_optical_sizing:     return (Key IS "opsz") and (not optical_coordinate);
         case SVF_font_variation_settings: return (Key.size() IS 4) and
                                             ((Key != "opsz") or optical_coordinate);
         default:                          return false;
      }
   };

   std::string result;
   std::string optical_coordinate;
   auto append = [&result](std::string_view Segment) {
      if (not result.empty()) result.push_back(';');
      result.append(Segment);
   };

   auto retain = [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (not HasValue) append(Value);
      else if ((Key IS "opsz") and ft::parse_number(Value)) optical_coordinate.assign(Value);
      else append(std::string(Key) + "=" + std::string(Value));
   };

   ft::for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if ((not HasValue) or (not owned(Key, Value))) retain(Key, Value, HasValue);
   });

   ft::for_each_descriptor(Descriptors, retain);

   // Explicit variation coordinates override optical-sizing irrespective of property declaration order.  Keep
   // the mode as well, so clearing variation-settings restores the inherited auto/none behaviour.

   if (not optical_coordinate.empty()) append("opsz=" + optical_coordinate);
   Style = result;
}

//********************************************************************************************************************
// Resolve declarations in cascade order, always using the unchanged parent for CSS-wide and relative values.
// Invalid declarations leave both the computed value and its descriptor ownership unchanged.

ERR svgState::resolve_text_property(uint32_t Hash, std::string_view Value, const svgState &Parent) noexcept
{
   namespace ft = kt::font_traits;

   if (Hash IS SVF_font_width) Hash = SVF_font_stretch;

   const char *initial = text_property_initial(Hash);

   if (not initial) return ERR::NoSupport;

   Value = ft::trim(Value);
   const bool inherited = (Hash != SVF_text_decoration) and (Hash != SVF_inline_size);
   const bool inherit_value = iequals(Value, "inherit") or (inherited and iequals(Value, "unset"));

   if (inherit_value) {
      auto it = Parent.m_text_properties.find(Hash);
      Value = (it != Parent.m_text_properties.end()) ? std::string_view(it->second) : std::string_view(initial);
   }
   else if (iequals(Value, "initial") or iequals(Value, "unset")) Value = initial;

   if (Value.empty()) return ERR::InvalidValue;

   std::string computed(Value);
   switch (Hash) {
      case SVF_white_space:
         if ((not iequals(Value, "normal")) and (not iequals(Value, "pre")) and
             (not iequals(Value, "pre-wrap")) and (not iequals(Value, "pre-line"))) return ERR::NoSupport;
         break;
      case SVF_inline_size: {
         if (iequals(Value, "auto")) break;

         double length;
         if (Value.ends_with('%')) {
            auto number = ft::parse_number(Value.substr(0, Value.size() - 1));
            if ((not number) or (*number < 0)) return ERR::InvalidValue;
            length = *number * (m_viewport_width > 0 ? m_viewport_width : Scene->PageWidth) * 0.01;
            if (not std::isfinite(length)) return ERR::InvalidValue;
         }
         else if ((not text_length(Value, m_font_size_px, m_root_font_size, false, length)) or (length < 0)) {
            return ERR::InvalidValue;
         }

         computed = text_number(length) + "px";
         break;
      }
      case SVF_text_align:
         // Start and justification are verified with start anchoring; other row/block combinations need layout work.
         if ((not iequals(Value, "start")) and (not iequals(Value, "justify"))) return ERR::NoSupport;
         break;
      case SVF_overflow_wrap:
      case SVF_word_break:
         // VTW::ANYWHERE also introduces punctuation breaks; it is not CSS emergency wrapping.
         if (not iequals(Value, "normal")) return ERR::NoSupport;
         break;
      case SVF_font_weight: {
         int weight;
         const int parent_weight = Parent.m_font_weight ? Parent.m_font_weight : 400;
         if (iequals(Value, "normal")) weight = 400;
         else if (iequals(Value, "bold")) weight = 700;
         else if (iequals(Value, "bolder")) {
            weight = parent_weight < 350 ? 400 : (parent_weight < 550 ? 700 : std::max(900, parent_weight));
         }
         else if (iequals(Value, "lighter")) {
            weight = parent_weight < 550 ? std::min(100, parent_weight) : (parent_weight < 750 ? 400 : 700);
         }
         else {
            auto number = ft::parse_number(Value);
            if ((not number) or (*number < 1) or (*number > 1000)) return ERR::InvalidValue;
            weight = int(std::round(*number));
         }

         m_font_weight = weight;
         computed = std::to_string(weight);
         break;
      }
      case SVF_font_style:
      case SVF_font_stretch:
      case SVF_font_variation_settings:
      case SVF_font_optical_sizing: {
         std::string descriptors;
         if (not font_descriptors(Hash, Value, descriptors)) return ERR::InvalidValue;
         merge_font_style(m_font_style, Hash, descriptors);
         break;
      }
      case SVF_font_kerning:
         if (not iequals(Value, "auto") and not iequals(Value, "normal") and not iequals(Value, "none")) {
            return ERR::InvalidValue;
         }
         m_font_kerning = iequals(Value, "none") ? 0 : 1;
         break;
      case SVF_kerning: {
         if (iequals(Value, "auto")) m_font_kerning = 1;
         else {
            auto end = Value.find_first_not_of("+-.0123456789");
            if (not ft::parse_number(Value.substr(0, end))) return ERR::InvalidValue;
            auto unit = (end IS std::string_view::npos) ? std::string_view() : Value.substr(end);
            if (not unit.empty() and unit != "px" and unit != "pt" and unit != "pc" and unit != "in" and
                unit != "cm" and unit != "mm" and unit != "em" and unit != "ex") return ERR::InvalidValue;
            m_font_kerning = 0; // Legacy compatibility: disable automatic kerning without applying a fixed distance.
         }
         break;
      }
      case SVF_text_anchor:
         if (not iequals(Value, "start") and not iequals(Value, "middle") and not iequals(Value, "end")) {
            return ERR::InvalidValue;
         }
         break;
      case SVF_text_decoration: {
         VTXF flags;
         if (not decoration_flags(Value, flags)) return ERR::InvalidValue;
         break;
      }
      case SVF_font_variant:
         if (not iequals(Value, "normal")) return ERR::NoSupport;
         break;
      case SVF_font_feature_settings:
         // Inherited settings are already in font definition syntax, rather than quoted CSS syntax.
         if ((not inherit_value) and (not feature_settings(Value, computed))) return ERR::InvalidValue;
         if (computed.empty()) computed = "normal";
         break;
      case SVF_font_family:
         if (not text_family(Value, computed)) return ERR::InvalidValue;
         m_font_family = computed;
         break;
      case SVF_font_size: {
         double size;
         const std::string_view keywords[] = { "xx-small", "x-small", "small", "medium", "large",
            "x-large", "xx-large", "xxx-large" };
         const double sizes[] = { 9, 10, 13, 16, 18, 24, 32, 48 };
         bool keyword = false;
         for (size_t i = 0; i < std::size(keywords); ++i) {
            if (iequals(Value, keywords[i])) { size = sizes[i]; keyword = true; break; }
         }
         if (iequals(Value, "larger")) { size = Parent.m_font_size_px * 1.2; keyword = true; }
         else if (iequals(Value, "smaller")) { size = Parent.m_font_size_px / 1.2; keyword = true; }
         if (not keyword and not text_length(Value, Parent.m_font_size_px, m_root_font_size, true, size)) {
            return ERR::InvalidValue;
         }
         if (std::round(size) < 1) return ERR::NoSupport;
         m_font_size_px = size;
         m_font_size = computed = text_number(size) + "px";
         break;
      }
      case SVF_letter_spacing:
      case SVF_word_spacing:
      case SVF_line_height: {
         if (iequals(Value, "normal")) { computed = "normal"; break; }
         double length;
         if (Hash IS SVF_line_height) {
            if (auto multiple = ft::parse_number(Value)) {
               if (*multiple <= 0) return ERR::NoSupport; // Zero is the backend's natural-pitch sentinel.
               computed = text_number(*multiple); // Inherit the multiplier, not this element's computed distance.
               break;
            }
         }
         if (not text_length(Value, m_font_size_px, m_root_font_size, Hash IS SVF_line_height, length)) {
            return ERR::InvalidValue;
         }
         if ((Hash IS SVF_line_height) and (length <= 0)) return ERR::NoSupport;
         computed = text_number(length) + "px";
         break;
      }
   }
   m_text_properties[Hash] = std::move(computed);
   if ((Hash IS SVF_kerning) and not iequals(Value, "auto")) return ERR::NoSupport;
   return ERR::Okay;
}

//********************************************************************************************************************
// Collect supported text containers without letting metadata or animation children contribute characters.
// This is deliberately a content model, not a styled-run layout implementation.

ERR svgState::collect_text(XTag &Tag, objVectorText *Text) noexcept
{
   kt::Log log(__FUNCTION__);
   svgTextContent content;
   bool trailing_collapsible = false;
   bool tab_warning = false;
   XTag *single_path = nullptr;

   auto discard_tail = [&]() {
      if (trailing_collapsible and (not content.Text.empty())) {
         const auto offset = content.Text.size() - 1;
         content.Text.pop_back();

         for (auto it = content.Sources.rbegin(); it != content.Sources.rend(); ++it) {
            if ((it->Output != std::string::npos) and (it->Output < offset)) break;
            if (it->Output IS offset) it->Output = std::string::npos;
         }

         for (auto it = content.Paths.rbegin(); it != content.Paths.rend(); ++it) {
            if (it->End < offset) break;
            it->Begin = std::min(it->Begin, offset);
            it->End   = std::min(it->End, offset);
         }
      }
      trailing_collapsible = false;
   };

   auto visit = [&](auto &Visit, XTag &Element, const svgState &State) -> void {
      const auto css        = State.m_text_properties.find(SVF_white_space);
      const bool legacy     = css IS State.m_text_properties.end();
      const bool preserve   = legacy ? iequals(State.m_xml_space, "preserve") :
         (iequals(css->second, "pre") or iequals(css->second, "pre-wrap"));
      const bool hard_lines = (not legacy) and (preserve or iequals(css->second, "pre-line"));

      kt::vector<XTag> replacement_nodes;
      auto *children = &Element.Children;
      if (auto replacement = Element.attrib("content-value")) {
         XTag node(Element.ID, Element.LineNo);
         node.Attribs.emplace_back("", *replacement);
         replacement_nodes.push_back(std::move(node));
         children = &replacement_nodes;
      }

      for (auto &child : *children) {
         if ((child.Flags & XTF::COMMENT) != XTF::NIL) continue;

         if (not child.isTag()) {
            const auto &value = child.Attribs[0].Value;
            for (size_t i = 0; i < value.size();) {
               const auto start = i++;
               while ((i < value.size()) and ((uint8_t(value[i]) & 0xc0) IS 0x80)) ++i;

               const char c = value[start];
               const bool newline    = (c IS '\n') or (c IS '\r');
               const bool whitespace = newline or (c IS '\t') or (c IS ' ');

               // SVG 1.1 removes segment breaks.  XML normally normalises CRLF; if a CR survives, it must not
               // manufacture a second hard break.

               const bool discard = (newline and legacy and (not preserve)) or
                  ((c IS '\r') and (i < value.size()) and (value[i] IS '\n'));

               size_t output = std::string::npos;
               if (discard) {
                  // The character has no output offset
               }
               else if (newline and hard_lines) {
                  if (not preserve) discard_tail();
                  output = content.Text.size();
                  content.Text += '\n';
                  trailing_collapsible = false;
               }
               else if (whitespace and (not preserve)) {
                  if ((not content.Text.empty()) and (content.Text.back() != '\n') and (not trailing_collapsible)) {
                     output = content.Text.size();
                     content.Text += ' ';
                     trailing_collapsible = true;
                  }
                  else if (trailing_collapsible) output = content.Text.size() - 1;
               }
               else {
                  output = content.Text.size();
                  if (whitespace) {
                     content.Text += ' ';
                     if ((c IS '\t') and (not legacy) and (not tab_warning)) {
                        log.warning("Preserved CSS tab stops are unsupported; using one space.");
                        tab_warning = true;
                     }
                  }
                  else content.Text.append(value, start, i - start);
                  trailing_collapsible = false;
               }

               content.Sources.push_back({ Element.ID, child.ID, start, output });
            }
         }
         else {
            const auto hash = svg_tag_hash(child);
            if ((hash != SVF_textPath) and (hash != SVF_tspan) and (hash != SVF_a)) continue;

            auto state = State;
            state.applyTag(child);
            const auto begin = content.Text.size();
            Visit(Visit, child, state);

            if (hash IS SVF_textPath) {
               content.Paths.push_back({ child.ID, begin, content.Text.size() });
               single_path = &child;
            }

            // Styles and positioning are retained in the XML but need a shared run layout interface.

            for (size_t a = 1; a < child.Attribs.size(); ++a) {
               auto name = std::string_view(child.Attribs[a].Name);
               if ((name IS "id") or (name IS "class") or (name IS "xml:space") or
                   (name IS "white-space") or (name IS "content-value")) continue;
               if ((hash IS SVF_textPath) and ((name IS "href") or (name IS "xlink:href") or
                   (name IS "method") or (name IS "spacing") or (name IS "startOffset") or
                   (name IS "transition") or (name IS "align") or (name IS "path"))) continue;
               log.warning("Text child '%s' requires styled-run layout; collecting content only.", child.name());
               break;
            }
         }
      }
   };

   if (auto replacement = Tag.attrib("content-value")) content.Text = *replacement;
   else {
      visit(visit, Tag, *this);
      discard_tail();
   }

   // The string attribute supplies a prefix independently of the element's collected content.

   std::string prefix;
   if (auto error = Text->getString(prefix); error != ERR::Okay) return error;

   if (not prefix.empty()) {
      for (auto &source : content.Sources) {
         if (source.Output != std::string::npos) source.Output += prefix.size();
      }

      for (auto &path : content.Paths) {
         path.Begin += prefix.size();
         path.End   += prefix.size();
      }

      content.Text.insert(0, prefix);
   }

   if (auto error = Text->setString(content.Text); error != ERR::Okay) return error;

   if ((content.Paths.size() IS 1) and (content.Paths.front().Begin IS 0) and
       (content.Paths.front().End IS content.Text.size())) {
      proc_guide_path(*single_path, Text);
   }
   else if (not content.Paths.empty()) {
      log.warning("Mixed text-path ranges require shared run layout; retaining ordered text and path ranges.");
   }

   Self->TextContent[Text->UID] = std::move(content);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR svgState::apply_text_properties(objVectorText *Text) const noexcept
{
   if (auto error = Text->setFace(m_font_family); error != ERR::Okay) return error;
   if (auto error = Text->setFontSize(m_font_size); error != ERR::Okay) return error;
   if (auto error = Text->setWeight(m_font_weight ? m_font_weight : 400); error != ERR::Okay) return error;
   if (auto error = Text->setFontStyle(m_font_style.empty() ? "Regular" : m_font_style); error != ERR::Okay) {
      return error;
   }

   for (auto hash : { SVF_letter_spacing, SVF_word_spacing, SVF_line_height }) {
      Unit length(0);
      auto it = m_text_properties.find(hash);
      if ((it != m_text_properties.end()) and (it->second != "normal")) {
         auto multiple = kt::font_traits::parse_number(it->second);
         if ((hash IS SVF_line_height) and multiple) length = Unit(*multiple, FD_SCALED);
         else length = Unit(it->second);
      }
      auto error = hash IS SVF_line_height ? Text->setLineHeight(length) :
         (hash IS SVF_letter_spacing ? Text->setLetterSpacing(length) : Text->setWordSpacing(length));
      if (error != ERR::Okay) return error;
   }

   // Feature settings are applied through the font definition.  `normal` leaves VectorText's default features.

   if (auto features = m_text_properties.find(SVF_font_feature_settings);
       (features != m_text_properties.end()) and (features->second != "normal")) {
      if (auto error = Text->setTextStyle("features=" + features->second); error != ERR::Okay) return error;
   }

   auto anchor = m_text_properties.find(SVF_text_anchor);
   const auto align = anchor IS m_text_properties.end() ? ALIGN::LEFT :
      (iequals(anchor->second, "middle") ? ALIGN::HORIZONTAL :
      (iequals(anchor->second, "end") ? ALIGN::RIGHT : ALIGN::LEFT));
   if (auto error = Text->setAlign(align); error != ERR::Okay) return error;

   VTXF flags;
   if (auto error = Text->getTextFlags(flags); error != ERR::Okay) return error;
   flags &= ~glDecorationMask;
   auto decoration = m_text_properties.find(SVF_text_decoration);
   if (decoration != m_text_properties.end()) {
      VTXF lines;
      if (not decoration_flags(decoration->second, lines)) return ERR::InvalidValue;
      flags |= lines;
   }

   if (m_font_kerning IS 0) flags |= VTXF::NO_KERNING;
   else flags &= ~VTXF::NO_KERNING;

   double inline_size   = 0;
   auto width           = m_text_properties.find(SVF_inline_size);
   auto whitespace      = m_text_properties.find(SVF_white_space);
   const bool no_wrap   = (whitespace != m_text_properties.end()) and iequals(whitespace->second, "pre");
   if ((not no_wrap) and (width != m_text_properties.end()) and (not iequals(width->second, "auto"))) {
      if (not text_length(width->second, m_font_size_px, m_root_font_size, false, inline_size)) {
         return ERR::InvalidValue;
      }
   }

   if (auto alignment = m_text_properties.find(SVF_text_align); alignment != m_text_properties.end()) {
      if ((inline_size > 0) and (align != ALIGN::LEFT)) {
         kt::Log log(__FUNCTION__);
         log.warning("Independent rectangular text-align with middle/end text-anchor requires row layout support.");
      }
      else if (iequals(alignment->second, "justify")) flags |= VTXF::JUSTIFY;
   }

   if (auto error = Text->setInlineSize(inline_size); error != ERR::Okay) return error;
   if (auto error = Text->setWrapMode(int(VTW::WORD)); error != ERR::Okay) return error;
   return Text->setTextFlags(flags);
}

//********************************************************************************************************************
// Apply current state values to a new vector as its defaults.  Used for applying state to child vectors.
// If you're adding more to this, matching code is needed in svgState::applyTag()

void svgState::applyStateToVector(objVector *Vector) const noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("%s: Fill: %s, Stroke: %s, Font: %s %s",
      Vector->Class->ClassName.c_str(), m_fill.c_str(), m_stroke.c_str(), m_font_family.c_str(), m_font_size.c_str());

   if (!m_fill.empty())   Vector->setFill(m_fill);
   if (!m_stroke.empty()) Vector->setStroke(m_stroke);
   if (m_stroke_width)    Vector->setStrokeWidth(m_stroke_width);
   if (m_line_join != VLJ::NIL)  Vector->setLineJoin(m_line_join);
   if (m_inner_join != VIJ::NIL) Vector->setInnerJoin(m_inner_join);
   if (m_line_cap != VLC::NIL)   Vector->setLineCap(m_line_cap);


   if (!m_display.empty()) {
      if (iequals("none", m_display))          Vector->setVisibility(VIS::HIDDEN);
      else if (iequals("inline", m_display))   Vector->setVisibility(VIS::VISIBLE);
      else if (iequals("inherit", m_display))  Vector->setVisibility(VIS::INHERIT);
   }

   if (!m_visibility.empty()) {
      if (iequals("visible", m_visibility))       Vector->setVisibility(VIS::VISIBLE);
      else if (iequals("hidden", m_visibility))   Vector->setVisibility(VIS::HIDDEN);
      else if (iequals("collapse", m_visibility)) Vector->setVisibility(VIS::COLLAPSE); // Same effect as hidden, kept for SVG compatibility
      else if (iequals("inherit", m_visibility))  Vector->setVisibility(VIS::INHERIT);
   }

   // NB: 'opacity' is not an inherited property.  It is applied only to the element that declares it, and the
   // renderer accounts for the opacity of any parent group.

   if (m_fill_opacity >= 0.0) Vector->setFillOpacity(m_fill_opacity);

   if (Vector->classID() != CLASSID::VECTORTEXT) {
      if (m_path_quality != RQ::AUTO) Vector->setPathQuality(m_path_quality);
   }
}

//********************************************************************************************************************
// Copy a tag's attributes to the current state.
// If you're adding more to this, matching code is needed in svgState::applyStateToVector()

void svgState::applyTag(const XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Total Attributes: %d", int(std::ssize(Tag.Attribs)));

   const auto parent = *this;
   m_text_properties.erase(SVF_text_decoration); // Decoration propagation needs a future styled-run model.
   m_text_properties.erase(SVF_inline_size); // Not inherited

   if (auto space = Tag.attrib("xml:space")) {
      if (iequals(*space, "preserve") or iequals(*space, "default")) m_xml_space = *space;
      else log.warning("Unsupported xml:space value: %s", space->c_str());
   }

   std::vector<XMLAttrib> declarations;
   for (size_t a = 1; a < Tag.Attribs.size(); ++a) {
      const auto &attr = Tag.Attribs[a];
      if (strhash(attr.Name) IS SVF_font) {
         std::vector<XMLAttrib> expanded;
         bool valid = expand_text_font(attr.Value, expanded);
         auto candidate = *this;
         for (const auto &item : expanded) {
            if (candidate.resolve_text_property(strhash(item.Name), item.Value, parent) != ERR::Okay) valid = false;
         }
         if (valid) declarations.insert(declarations.end(), expanded.begin(), expanded.end());
         else log.warning("Invalid or unsupported font shorthand: %s", attr.Value.c_str());
      }
      else if (text_property_initial(strhash(attr.Name))) declarations.push_back(attr);
   }
   // Compute font-size first: spacing lengths use this element's final computed size regardless of source order.
   for (bool sizes : { true, false }) {
      for (const auto &attr : declarations) {
         auto hash = strhash(attr.Name);
         if ((hash IS SVF_font_size) != sizes) continue;
         if (auto error = resolve_text_property(hash, attr.Value, parent); error != ERR::Okay) {
            log.warning("Invalid or unsupported text property '%s': %s", attr.Name.c_str(), attr.Value.c_str());
         }
      }
      if (sizes and not m_has_root_font_size and iequals(Tag.name(), "svg")) {
         m_root_font_size = m_font_size_px;
         m_has_root_font_size = true;
      }
   }

   if (Tag.Children.empty()) {
      // If the tag has no children then few tags are worth saving to the state. E.g.
      // 'color' is only required for the 'currentColor' value.
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         switch (strhash(Tag.Attribs[a].Name)) {
            case SVF_color:
               if ("inherit" IS val) break;
               m_color = val;
               break;
         }
      }
      return;
   }

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch (strhash(Tag.Attribs[a].Name)) {
         case SVF_color: // Affects 'currentColor'
            if ("inherit" IS val) break;
            m_color = val;
            break;

         // Within a parameter scope, inherited paint server references must point to the scoped instance.

         case SVF_fill:
            if ("inherit" IS val);
            else if ("currentColor" IS val) m_fill = m_color;
            else if (m_param_scope) m_fill = resolve_paint_server(val);
            else m_fill = val;
            break;

         case SVF_stroke:
            if ("inherit" IS val);
            else if ("currentColor" IS val) m_stroke = m_color;
            else if (m_param_scope) m_stroke = resolve_paint_server(val);
            else m_stroke = val;
            if (!m_stroke_width) m_stroke_width = 1;
            break;

         case SVF_stop_color: m_stop_color = val; break;
         case SVF_display:    m_display = val; break;
         case SVF_visibility: m_visibility = val; break;

         case SVF_stroke_linejoin:
            switch(strhash(val)) {
               case SVF_miter:       m_line_join = VLJ::MITER; break;
               case SVF_round:       m_line_join = VLJ::ROUND; break;
               case SVF_bevel:       m_line_join = VLJ::BEVEL; break;
               case SVF_inherit:     m_line_join = VLJ::INHERIT; break;
               case SVF_miter_clip:  m_line_join = VLJ::MITER_SMART; break; // Special AGG only join type
               case SVF_miter_round: m_line_join = VLJ::MITER_ROUND; break; // Special AGG only join type
            }
            break;

         case SVF_stroke_innerjoin: // AGG ONLY
            switch(strhash(val)) {
               case SVF_miter:   m_inner_join = VIJ::MITER; break;
               case SVF_round:   m_inner_join = VIJ::ROUND; break;
               case SVF_bevel:   m_inner_join = VIJ::BEVEL; break;
               case SVF_inherit: m_inner_join = VIJ::INHERIT; break;
               case SVF_jag:     m_inner_join = VIJ::JAG; break;
            }
            break;

         case SVF_stroke_linecap:
            switch(strhash(val)) {
               case SVF_butt:    m_line_cap = VLC::BUTT; break;
               case SVF_square:  m_line_cap = VLC::SQUARE; break;
               case SVF_round:   m_line_cap = VLC::ROUND; break;
               case SVF_inherit: m_line_cap = VLC::INHERIT; break;
            }
            break;

         case SVF_stroke_width: svtonum(val, m_stroke_width); break;
         case SVF_fill_opacity: m_fill_opacity = std::clamp(svtonum<double>(val), 0.0, 1.0); break;
         case SVF_stop_opacity: m_stop_opacity = std::clamp(svtonum<double>(val), 0.0, 1.0); break;
         case SVF_shape_rendering: m_path_quality = shape_rendering_to_render_quality(val); break;
      }
   }
}

//********************************************************************************************************************
// Process all child elements that belong to the target Tag.

void svgState::process_children(XTag &Tag, OBJECTPTR Vector) noexcept
{
   objVector *sibling = nullptr;
   for (auto &child : Tag.Children) {
      if (child.isTag()) {
         process_tag(child, Tag, Vector, sibling);
      }
   }
}

//********************************************************************************************************************
// Process a restricted set of children for shape objects.

void svgState::process_shape_children(XTag &Tag, OBJECTPTR Vector) noexcept
{
   kt::Log log;

   for (auto &child : Tag.Children) {
      if (!child.isTag()) continue;

      switch(svg_tag_hash(child)) {
         case SVF_animate:          proc_animate(child, Tag, Vector); break;
         case SVF_animateColour:
         case SVF_animateColor:     proc_animate_colour(child, Tag, Vector); break;
         case SVF_animateTransform: proc_animate_transform(child, Vector); break;
         case SVF_animateMotion:    proc_animate_motion(child, Vector); break;
         case SVF_set:              proc_set(child, Tag, Vector); break;
         case SVF_kotuku_guidePath: proc_guide_path(child, Vector); break;
         case SVF_param:            break; // Consumed by proc_image()
         case SVF_title:
         case SVF_desc:             break; // Metadata is not rendered text.

         case SVF_textPath:
         case SVF_tspan:
         case SVF_a:
            // Text is collected once, in document order, before initialisation.
            if (Vector->classID() IS CLASSID::VECTORTEXT) break;
            [[fallthrough]];

         default:
            log.warning("Failed to interpret vector child element <%s/> @ line %d", child.name(), child.LineNo);
            break;
      }
   }
}

//********************************************************************************************************************

void svgState::proc_pathtransition(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   objVectorTransition *trans;
   if (!NewObject(CLASSID::VECTORTRANSITION, &trans)) {
      trans->setOwner(Self->Scene->UID); // All clips belong to the root page to prevent hierarchy issues.
      trans->setName("SVGTransition");

      std::string id;
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         if (Tag.Attribs[a].Value.empty()) continue;

         switch(strhash(Tag.Attribs[a].Name)) {
            case SVF_id: id = Tag.Attribs[a].Value; break;
         }
      }

      if (!id.empty()) {
         auto stops = process_transition_stops(Self, Tag.Children);
         if (stops.size() >= 2) {
            trans->setStops(stops);

            if (!InitObject(trans)) {
               if (!Self->Cloning) Self->Scene->addDef(id, trans);
               track_object(Self, trans);
               return;
            }
         }
         else log.warning("At least two stops are required for <pathTransition> at line %d.", Tag.LineNo);
      }
      else log.warning("No id attribute specified in <pathTransition> at line %d.", Tag.LineNo);

      FreeResource(trans);
   }
}

//********************************************************************************************************************

void svgState::proc_clippath(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   std::string id, transform, units;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_id:            id        = value; break;
         case SVF_transform:     transform = value; break;
         case SVF_clipPathUnits: units     = value; break;
         case SVF_externalResourcesRequired: break; // Deprecated SVG attribute
         default:
            log.warning("<clipPath> attribute '%s' unrecognised @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            break;
      }
   }

   if (id.empty()) {
      // Declaring a clipPath without an id is poor form, but it is valid SVG and likely that at least
      // one child object will specify an id in this case.
      static int clip_id = 1;
      id = "auto_clippath_" + std::to_string(clip_id++);
   }

   // A clip-path with an ID can only be added once (important when a clip-path is repeatedly referenced)

   if (Self->Scene->findDef(id, nullptr) != ERR::Okay) {
      objVectorClip *clip;
      if (!NewObject(CLASSID::VECTORCLIP, &clip)) {
         clip->setOwner(Self->Scene->UID);
         clip->setName("SVGClip");
         clip->setSID(id);

         if (not transform.empty()) parse_transform((objVector *)clip, transform, MTAG_SVG_TRANSFORM);

         if (not units.empty()) {
            if (iequals("userSpaceOnUse", units)) clip->setUnits(VUNIT::USERSPACE);
            else if (iequals("objectBoundingBox", units)) clip->setUnits(VUNIT::BOUNDING_BOX);
         }

         if (!InitObject(clip)) {
            svgState state(Self);

            // Valid child elements for clip-path are:
            // Shapes:   circle, ellipse, line, path, polygon, polyline, rect, text, ...
            // Commands: use, animate

            objVectorViewport *vp;
            clip->getViewport(vp);
            state.process_children(Tag, vp);

            Self->Scene->addDef(id, clip);
            track_object(Self, clip);
         }
         else FreeResource(clip);
      }
   }
}

//********************************************************************************************************************
// NB: This implementation of mask support uses VectorClip.  An alternative would be to use VectorFilter.
//
// SVG masks are luminance masks by default (as opposed to masking on a per-channel RGBA basis).
//
// The formula used to get the luminance out of a given RGB value is: .2126R + .7152G + .0722B

void svgState::proc_mask(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   std::string id, transform;
   auto units = VUNIT::USERSPACE;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_id:        id = value; break;
         case SVF_transform: transform = value; break;
         case SVF_maskUnits:
            if (iequals("userSpaceOnUse", value)) units = VUNIT::USERSPACE;
            else if (iequals("objectBoundingBox", value)) units = VUNIT::BOUNDING_BOX;
            break;
         case SVF_maskContentUnits: // TODO
            break;
         case SVF_externalResourcesRequired: // Deprecated SVG attribute
            break;
         case SVF_color_interpolation:
            break;
         case SVF_filter:
            break;
         case SVF_x:
         case SVF_y:
         case SVF_width:
         case SVF_height:
            break;
         default:
            log.warning("<mask> attribute '%s' unrecognised @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            break;
      }
   }

   if (id.empty()) {
      static int clip_id = 1;
      id = "auto_mask_" + std::to_string(clip_id++);
   }

   // A clip-path with an ID can only be added once (important when a clip-path is repeatedly referenced)

   if (!Self->Scene->findDef(id, nullptr)) return;

   objVectorClip *clip;
   if (!NewObject(CLASSID::VECTORCLIP, &clip)) {
      clip->setOwner(Self->Scene->UID);
      clip->setName("SVGMask");
      clip->setFlags(VCLF::APPLY_FILLS|VCLF::APPLY_STROKES);
      clip->setUnits(units);
      clip->setSID(id);

      if (!transform.empty()) parse_transform((objVector *)clip, transform, MTAG_SVG_TRANSFORM);

      if (!InitObject(clip)) {
         svgState state(Self);
         objVectorViewport *vp;
         clip->getViewport(vp);
         state.process_children(Tag, vp);

         Self->Scene->addDef(id, clip);
         track_object(Self, clip);
      }
      else FreeResource(clip);
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_blur(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objBlurFX *fx;

   if (NewObject(CLASSID::BLURFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_stdDeviation: { // Y is optional, if not set then it is equivalent to X.
            double x = -1, y = -1;
            read_numseq(val, { &x, &y });
            if ((x) and (y IS -1)) y = x;
            if (x > 0) fx->setSX(x);
            if (y > 0) fx->setSY(y);
            break;
         }

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_offset(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objOffsetFX *fx;

   if (NewObject(CLASSID::OFFSETFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_dx: fx->setXOffset(svtonum<double>(val)); break;
         case SVF_dy: fx->setYOffset(svtonum<double>(val)); break;
         case SVF_in: parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_merge(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objMergeFX *fx;

   if (NewObject(CLASSID::MERGEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_result: result_name = val; break;
      }
   }

   std::vector<MergeSource> list;
   for (auto &child : Tag.Children) {
      if (svg_tag_hash(child) IS SVF_feMergeNode) {
         for (int a=1; a < std::ssize(child.Attribs); a++) {
            if (iequals("in", child.Attribs[a].Name)) {
               switch (strhash(child.Attribs[a].Value)) {
                  case SVF_SourceGraphic:   list.push_back(VSF::GRAPHIC); break;
                  case SVF_SourceAlpha:     list.push_back(VSF::ALPHA); break;
                  case SVF_BackgroundImage: list.push_back(VSF::BKGD); break;
                  case SVF_BackgroundAlpha: list.push_back(VSF::BKGD_ALPHA); break;
                  case SVF_FillPaint:       list.push_back(VSF::FILL); break;
                  case SVF_StrokePaint:     list.push_back(VSF::STROKE); break;
                  default:  {
                     if (auto ref = child.Attribs[a].Value.c_str()) {
                        while ((*ref) and (*ref <= 0x20)) ref++;
                        if (Self->Effects.contains(ref)) {
                           list.emplace_back(VSF::REFERENCE, Self->Effects[ref]);
                        }
                        else log.warning("Invalid 'in' reference '%s'", ref);
                     }
                     else log.warning("'in' reference is an empty string.");

                     break;
                  }
               }
            }
            else log.warning("Invalid feMergeNode attribute '%s'", child.Attribs[a].Name.c_str());
         }
      }
      else log.warning("Unrecognised feMerge child node '%s'", child.name());
   }

   if (!list.empty()) {
      if (fx->setSourceList(list) != ERR::Okay) {
         FreeResource(fx);
         return log.warning(ERR::SetField);
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return log.warning(ERR::Init);
   }
}

//********************************************************************************************************************

#define CM_SIZE 20

static const std::array<double,20> glProtanopia = { 0.567,0.433,0,0,0, 0.558,0.442,0,0,0, 0,0.242,0.758,0,0, 0,0,0,1,0 };
static const std::array<double,20> glProtanomaly = { 0.817,0.183,0,0,0, 0.333,0.667,0,0,0, 0,0.125,0.875,0,0, 0,0,0,1,0 };
static const std::array<double,20> glDeuteranopia = { 0.625,0.375,0,0,0, 0.7,0.3,0,0,0, 0,0.3,0.7,0,0, 0,0,0,1,0 };
static const std::array<double,20> glDeuteranomaly = { 0.8,0.2,0,0,0, 0.258,0.742,0,0,0, 0,0.142,0.858,0,0, 0,0,0,1,0 };
static const std::array<double,20> glTritanopia = { 0.95,0.05,0,0,0, 0,0.433,0.567,0,0, 0,0.475,0.525,0,0, 0,0,0,1,0 };
static const std::array<double,20> glTritanomaly = { 0.967,0.033,0,0,0, 0,0.733,0.267,0,0, 0,0.183,0.817,0,0, 0,0,0,1,0 };
static const std::array<double,20> glAchromatopsia = { 0.299,0.587,0.114,0,0, 0.299,0.587,0.114,0,0, 0.299,0.587,0.114,0,0, 0,0,0,1,0 };
static const std::array<double,20> glAchromatomaly = { 0.618,0.320,0.062,0,0, 0.163,0.775,0.062,0,0, 0.163,0.320,0.516,0,0, 0,0,0,1,0 };

ERR svgState::parse_fe_colour_matrix(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objColourFX *fx;

   if (NewObject(CLASSID::COLOURFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_type: {
            const std::array<double, 20> *m = nullptr;
            CM mode = CM::NIL;
            switch(strhash(val)) {
               case SVF_none:          mode = CM::NONE; break;
               case SVF_matrix:        mode = CM::MATRIX; break;
               case SVF_saturate:      mode = CM::SATURATE; break;
               case SVF_hueRotate:     mode = CM::HUE_ROTATE; break;
               case SVF_luminanceToAlpha: mode = CM::LUMINANCE_ALPHA; break;
               // These are special modes that are not included by SVG
               case SVF_contrast:      mode = CM::CONTRAST; break;
               case SVF_brightness:    mode = CM::BRIGHTNESS; break;
               case SVF_hue:           mode = CM::HUE; break;
               case SVF_colourise:     mode = CM::COLOURISE; break;
               case SVF_desaturate:    mode = CM::DESATURATE; break;
               // Colour blindness modes
               case SVF_protanopia:    mode = CM::MATRIX; m = &glProtanopia; break;
               case SVF_protanomaly:   mode = CM::MATRIX; m = &glProtanomaly; break;
               case SVF_deuteranopia:  mode = CM::MATRIX; m = &glDeuteranopia; break;
               case SVF_deuteranomaly: mode = CM::MATRIX; m = &glDeuteranomaly; break;
               case SVF_tritanopia:    mode = CM::MATRIX; m = &glTritanopia; break;
               case SVF_tritanomaly:   mode = CM::MATRIX; m = &glTritanomaly; break;
               case SVF_achromatopsia: mode = CM::MATRIX; m = &glAchromatopsia; break;
               case SVF_achromatomaly: mode = CM::MATRIX; m = &glAchromatomaly; break;

               default:
                  log.warning("Unrecognised colour matrix type '%s'", val.c_str());
                  FreeResource(fx);
                  return ERR::InvalidValue;
            }

            fx->setMode(mode);
            if ((mode IS CM::MATRIX) and (m)) fx->setValues(*m);
            break;
         }

         case SVF_values: {
            auto m = read_array<double>(val, CM_SIZE);
            fx->setValues(m);
            break;
         }

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_convolve_matrix(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objConvolveFX *fx;

   if (NewObject(CLASSID::CONVOLVEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   auto fail = [fx](ERR Error) {
      FreeResource(fx);
      return Error;
   };

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_order: {
            int ox = 0, oy = 0;
            if (!read_positive_integer_pair(val, ox, oy)) return fail(ERR::InvalidValue);
            if (fx->setMatrixColumns(ox) != ERR::Okay) return fail(ERR::InvalidValue);
            if (fx->setMatrixRows(oy) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         case SVF_kernelMatrix: {
            constexpr int max_matrix_dim = 9; // Matches ConvolveFX's internal matrix limit.
            auto matrix = read_array<double>(val, (max_matrix_dim * max_matrix_dim) + 1);
            if (fx->setMatrix(matrix) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         case SVF_divisor: {
            double divisor = 0;
            read_numseq(val, { &divisor });
            if (fx->setDivisor(divisor) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         case SVF_bias: {
            double bias = 0;
            read_numseq(val, { &bias });
            fx->setBias(bias);
            break;
         }

         case SVF_targetX: {
            int target_x = 0;
            if (!read_integer_value(val, target_x)) return fail(ERR::InvalidValue);
            if (fx->setTargetX(target_x) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         case SVF_targetY: {
            int target_y = 0;
            if (!read_integer_value(val, target_y)) return fail(ERR::InvalidValue);
            if (fx->setTargetY(target_y) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         case SVF_edgeMode:
            if (iequals("duplicate", val)) fx->setEdgeMode(EM::DUPLICATE);
            else if (iequals("wrap", val)) fx->setEdgeMode(EM::WRAP);
            else if (iequals("none", val)) fx->setEdgeMode(EM::NONE);
            break;

         case SVF_kernelUnitLength: {
            double kx = 1.0, ky = 1.0;
            if (!read_positive_number_pair(val, kx, ky)) return fail(ERR::InvalidValue);
            if (fx->setUnitX(kx) != ERR::Okay) return fail(ERR::InvalidValue);
            if (fx->setUnitY(ky) != ERR::Okay) return fail(ERR::InvalidValue);
            break;
         }

         // The modifications will apply to R,G,B only when preserveAlpha is true.
         case SVF_preserveAlpha:
            fx->setPreserveAlpha(iequals("true", val) or (std::string_view("1") IS val));
            break;

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_lighting(objVectorFilter *Filter, XTag &Tag, LT Type) noexcept
{
   kt::Log log(__FUNCTION__);
   objLightingFX *fx;

   if (NewObject(CLASSID::LIGHTINGFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   fx->setType(Type);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_lighting_colour:
         case SVF_lighting_color: {
            VectorPainter painter;
            if (iequals("currentColor", val)) {
               FRGB rgb;
               if (current_colour(Self->Scene->Viewport, rgb)) fx->setColour(rgb);
            }
            else if (!vec::ReadPainter(nullptr, val, &painter, nullptr)) fx->setColour(painter.Colour);
            break;
         }

         case SVF_kernelUnitLength: { // An invalid value is ignored, leaving the kernel unit undefined
            double kx = 1, ky = 1;
            if (read_positive_number_pair(val, kx, ky)) {
               fx->setUnitX(kx);
               fx->setUnitY(ky);
            }
            break;
         }

         case SVF_specularConstant:
         case SVF_diffuseConstant:  fx->setConstant(svtonum<double>(val)); break;
         case SVF_surfaceScale:     fx->setScale(svtonum<double>(val)); break;
         case SVF_specularExponent: fx->setExponent(svtonum<double>(val)); break;

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
         default:         log.warning("Unknown %s attribute %s", Tag.name(), Tag.Attribs[a].Name.c_str());
      }
   }

   // One child tag specifying the light source is required.

   if (!Tag.Children.empty()) {
      ERR error;
      auto &child = Tag.Children[0];
      if (svg_tag_is(child, SVF_feDistantLight)) {
         double azimuth = 0, elevation = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strhash(child.Attribs[a].Name)) {
               case SVF_azimuth:   svtonum(child.Attribs[a].Value, azimuth); break;
               case SVF_elevation: svtonum(child.Attribs[a].Value, elevation); break;
            }
         }

         error = fx->setDistantLight(azimuth, elevation);
      }
      else if (svg_tag_is(child, SVF_fePointLight)) {
         double x = 0, y = 0, z = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strhash(child.Attribs[a].Name)) {
               case SVF_x: svtonum(child.Attribs[a].Value, x); break;
               case SVF_y: svtonum(child.Attribs[a].Value, y); break;
               case SVF_z: svtonum(child.Attribs[a].Value, z); break;
            }
         }

         error = fx->setPointLight(x, y, z);
      }
      else if (svg_tag_is(child, SVF_feSpotLight)) {
         double x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
         double exponent = 1, cone_angle = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            auto &val = child.Attribs[a].Value;
            switch(strhash(child.Attribs[a].Name)) {
               case SVF_x:                 svtonum(val, x); break;
               case SVF_y:                 svtonum(val, y); break;
               case SVF_z:                 svtonum(val, z); break;
               case SVF_pointsAtX:         svtonum(val, px); break;
               case SVF_pointsAtY:         svtonum(val, py); break;
               case SVF_pointsAtZ:         svtonum(val, pz); break;
               case SVF_specularExponent:  svtonum(val, exponent); break;
               case SVF_limitingConeAngle: svtonum(val, cone_angle); break;
            }
         }

         error = fx->setSpotLight(x, y, z, px, py, pz, exponent, cone_angle);
      }
      else {
         log.warning("Unrecognised %s child node '%s'", Tag.name(), child.name());
         error = ERR::NoSupport;
      }

      if (error != ERR::Okay) {
         FreeResource(fx);
         return error;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_displacement_map(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objDisplacementFX *fx;

   if (NewObject(CLASSID::DISPLACEMENTFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_xChannelSelector:
            switch(val[0]) {
               case 'r': case 'R': fx->setXChannel(CMP::RED); break;
               case 'g': case 'G': fx->setXChannel(CMP::GREEN); break;
               case 'b': case 'B': fx->setXChannel(CMP::BLUE); break;
               case 'a': case 'A': fx->setXChannel(CMP::ALPHA); break;
            }
            break;

         case SVF_yChannelSelector:
            switch(val[0]) {
               case 'r': case 'R': fx->setYChannel(CMP::RED); break;
               case 'g': case 'G': fx->setYChannel(CMP::GREEN); break;
               case 'b': case 'B': fx->setYChannel(CMP::BLUE); break;
               case 'a': case 'A': fx->setYChannel(CMP::ALPHA); break;
            }
            break;

         case SVF_scale: fx->setScale(svtonum<double>(val)); break;

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;

         case SVF_image_rendering: {
            if ("optimizeSpeed" IS val) fx->setResampleMethod(VSM::NEIGHBOUR);
            else if ("optimizeQuality" IS val) fx->setResampleMethod(VSM::LANCZOS);
            else if ("auto" IS val); // Leave as internal default
            else if ("inherit" IS val);
            else log.warning("Unrecognised image-rendering option '%s'", val.c_str());
            break;
         }

         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_in2:    parse_input_mix(Self, fx, val); break;

         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_wavefunction(objVectorFilter *Filter, XTag &Tag) noexcept
{
   objWaveFunctionFX *fx;

   if (NewObject(CLASSID::WAVEFUNCTIONFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_resolution: fx->setResolution(svtonum<int>(val)); break;
         case SVF_preserveAspectRatio: fx->setAspectRatio(parse_aspect_ratio(val)); break;
         case SVF_scale:    fx->setScale(svtonum<double>(val)); break;
         case SVF_n:        fx->setN(svtonum<int>(val)); break;
         case SVF_l:        fx->setL(svtonum<int>(val)); break;
         case SVF_m:        fx->setM(svtonum<int>(val)); break;
         case SVF_x:        fx->setX(SVGUnit(val)); break;
         case SVF_y:        fx->setY(SVGUnit(val)); break;
         case SVF_width:    fx->setWidth(SVGUnit(val)); break;
         case SVF_height:   fx->setHeight(SVGUnit(val)); break;
         case SVF_colormap: fx->setColourMap(val); break;
         case SVF_result:   result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);

      if (Tag.hasChildTags()) {
         auto stops = process_gradient_stops(Tag);
         if (stops.size() >= 2) fx->setStops(stops);
      }

      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_component_xfer(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objRemapFX *fx;

   if (NewObject(CLASSID::REMAPFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   for (auto &child : Tag.Children) {
      auto child_name = svg_local_name(child);
      if (wildcmp("feFunc?", std::string(child_name))) {
         auto cmp = CMP::NIL;
         switch(child_name[6]) {
            case 'R': cmp = CMP::RED; break;
            case 'G': cmp = CMP::GREEN; break;
            case 'B': cmp = CMP::BLUE; break;
            case 'A': cmp = CMP::ALPHA; break;
            default:
               log.warning("Invalid feComponentTransfer element %s", child.name());
               return ERR::InvalidData;
         }

         uint32_t type = 0;
         int mask = 0xff;
         double amp = 1.0, offset = 0, exp = 1.0, slope = 1.0, intercept = 0.0;
         kt::vector<double> values;
         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strhash(child.Attribs[a].Name)) {
               case SVF_type:        type = strhash(child.Attribs[a].Value); break;
               case SVF_amplitude:   read_numseq(child.Attribs[a].Value, { &amp }); break;
               case SVF_intercept:   read_numseq(child.Attribs[a].Value, { &intercept }); break;
               case SVF_slope:       read_numseq(child.Attribs[a].Value, { &slope }); break;
               case SVF_exponent:    read_numseq(child.Attribs[a].Value, { &exp }); break;
               case SVF_offset:      read_numseq(child.Attribs[a].Value, { &offset }); break;
               case SVF_mask:        svtonum(child.Attribs[a].Value, mask); break;
               case SVF_tableValues: {
                  values = read_array<double>(child.Attribs[a].Value, 64);
                  break;
               }
               default: log.warning("Unknown %s attribute %s", child.name(), child.Attribs[a].Name.c_str()); break;
            }
         }

         if (type) {
            // An empty tableValues list is equivalent to the identity function for table and discrete types.

            if (((type IS SVF_table) or (type IS SVF_discrete)) and (values.empty())) type = SVF_identity;

            ERR error;
            switch(type) {
               case SVF_table:    error = fx->selectTable(cmp, values); break;
               case SVF_linear:   error = fx->selectLinear(cmp, slope, intercept);  break;
               case SVF_gamma:    error = fx->selectGamma(cmp, amp, offset, exp);  break;
               case SVF_discrete: error = fx->selectDiscrete(cmp, values);  break;
               case SVF_identity: error = fx->selectIdentity(cmp); break;
               // The following additions are specific to Kotuku and not SVG compatible.
               case SVF_invert:   error = fx->selectInvert(cmp); break;
               case SVF_mask:     error = fx->selectMask(cmp, mask); break;
               default:
                  log.warning("feComponentTransfer type invalid.");
                  return ERR::InvalidValue;
            }

            if (error != ERR::Okay) {
               log.warning("Failed to apply the %s transfer function: %s", child.name(), GetErrorMsg(error));
            }
         }
         else {
            log.warning("feComponentTransfer requires a type.");
            return ERR::FieldNotSet;
         }
      }
      else log.warning("Unrecognised feComponentTransfer child node '%s'", child.name());
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_composite(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objCompositeFX *fx;

   if (NewObject(CLASSID::COMPOSITEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_mode:
         case SVF_operator: {
            switch (strhash(val)) {
               // SVG Operator types
               case SVF_normal:
               case SVF_over:       fx->setOperator(OP::OVER); break;
               case SVF_in:         fx->setOperator(OP::IN); break;
               case SVF_out:        fx->setOperator(OP::OUT); break;
               case SVF_atop:       fx->setOperator(OP::ATOP); break;
               case SVF_xor:        fx->setOperator(OP::XOR); break;
               case SVF_arithmetic: fx->setOperator(OP::ARITHMETIC); break;
               // SVG Mode types
               case SVF_screen:   fx->setOperator(OP::SCREEN); break;
               case SVF_multiply: fx->setOperator(OP::MULTIPLY); break;
               case SVF_lighten:  fx->setOperator(OP::LIGHTEN); break;
               case SVF_darken:   fx->setOperator(OP::DARKEN); break;
               // Kotuku modes
               case SVF_invertRGB:  fx->setOperator(OP::INVERT_RGB); break;
               case SVF_invert:     fx->setOperator(OP::INVERT); break;
               case SVF_contrast:   fx->setOperator(OP::CONTRAST); break;
               case SVF_dodge:      fx->setOperator(OP::DODGE); break;
               case SVF_burn:       fx->setOperator(OP::BURN); break;
               case SVF_hardLight:  fx->setOperator(OP::HARD_LIGHT); break;
               case SVF_softLight:  fx->setOperator(OP::SOFT_LIGHT); break;
               case SVF_difference: fx->setOperator(OP::DIFFERENCE); break;
               case SVF_exclusion:  fx->setOperator(OP::EXCLUSION); break;
               case SVF_plus:       fx->setOperator(OP::PLUS); break;
               case SVF_minus:      fx->setOperator(OP::MINUS); break;
               case SVF_overlay:    fx->setOperator(OP::OVERLAY); break;
               default:
                  log.warning("Composite operator '%s' not recognised.", val.c_str());
                  FreeResource(fx);
                  return ERR::InvalidValue;
            }
            break;
         }

         case SVF_k1: {
            double k1;
            read_numseq(val, { &k1 });
            fx->setK1(k1);
            break;
         }

         case SVF_k2: {
            double k2;
            read_numseq(val, { &k2 });
            fx->setK2(k2);
            break;
         }

         case SVF_k3: {
            double k3;
            read_numseq(val, { &k3 });
            fx->setK3(k3);
            break;
         }

         case SVF_k4: {
            double k4;
            read_numseq(val, { &k4 });
            fx->setK4(k4);
            break;
         }

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_in2:    parse_input_mix(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   // SVG's second input has the same default as 'in': the previous result, or SourceGraphic.
   if (fx->MixType IS VSF::NIL) {
      if (fx->Prev) fx->setMix(fx->Prev);
      else fx->setMixType(VSF::GRAPHIC);
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_flood(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objFloodFX *fx;

   if (NewObject(CLASSID::FLOODFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   ERR error = ERR::Okay;
   std::string result_name;
   for (int a=1; (a < std::ssize(Tag.Attribs)) and (!error); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_flood_color:
         case SVF_flood_colour: {
            VectorPainter painter;
            if (iequals("currentColor", val)) {
               if (current_colour(Self->Scene->Viewport, painter.Colour)) error = fx->setColour(painter.Colour);
            }
            else if (!vec::ReadPainter(nullptr, val, &painter, nullptr)) error = fx->setColour(painter.Colour);
            break;
         }

         case SVF_flood_opacity: {
            error = fx->setOpacity(std::clamp(svtonum<double>(val), 0.0, 1.0));
            break;
         }

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return log.warning(error);
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_turbulence(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objTurbulenceFX *fx;

   if (NewObject(CLASSID::TURBULENCEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_baseFrequency: {
            double bfx = -1, bfy = -1;
            read_numseq(val, { &bfx, &bfy });
            if (bfx < 0) bfx = 0;
            if (bfy < 0) bfy = bfx;
            fx->setFX(bfx);
            fx->setFY(bfy);
            break;
         }

         case SVF_numOctaves: fx->setOctaves(svtonum<int>(val)); break;
         case SVF_seed: fx->setSeed(svtonum<int>(val)); break;
         case SVF_stitchTiles: fx->setStitch(iequals("stitch", val)); break;

         case SVF_type:
            if (iequals("fractalNoise", val)) fx->setType(TB::NOISE);
            else fx->setType(TB::NIL);
            break;

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_morphology(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objMorphologyFX *fx;

   if (NewObject(CLASSID::MORPHOLOGYFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_radius: {
            double x = -1, y = -1;
            read_numseq(val, { &x, &y });
            if (x > 0) fx->setRadiusX(int(x));
            if (y > 0) fx->setRadiusY(int(y));
            break;
         }

         case SVF_operator:
            switch(strhash(val)) {
               case SVF_erode:  fx->setOperator(MOP::ERODE); break;
               case SVF_dilate: fx->setOperator(MOP::DILATE); break;
               default: log.warning("Invalid morphology operator '%s'", val.c_str()); break;
            }

            break;

         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_in:     parse_input_source(Self, fx, val); break;
         case SVF_result: result_name = val; break;
      }
   }

   if (!fx->init()) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************
// This code replaces feImage elements where the href refers to a resource name.

ERR svgState::parse_fe_source(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objSourceFX *fx;

   if (NewObject(CLASSID::SOURCEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   bool required = false;
   std::string ref, result_name;

   ERR error = ERR::Okay;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;
         case SVF_preserveAspectRatio: fx->setAspectRatio(parse_aspect_ratio(val)); break;
         case SVF_xlink_href: ref = val; break;
         case SVF_externalResourcesRequired: required = iequals("true", val); break;
         case SVF_result: result_name = val; break;
      }
   }

   objVector *vector = nullptr;
   OBJECTPTR def = nullptr;
   if (!ref.empty()) {
      if (Self->Scene->findDef(ref, &def) IS ERR::Okay) vector = (objVector *)def;
      else {
         // The reference is not an existing vector but should be a pre-registered declaration that would allow
         // us to create it.  Note that creation only occurs once.  Subsequent use of the ID will result in the
         // live reference being found.

         if (auto tagref = find_href_tag(Self, ref)) {
            process_tag(*tagref, Tag, Self->Scene, vector);
         }
         else log.warning("Element id '%s' not found.", ref.c_str());
      }

      if (vector) {
         fx->setSourceName(ref);
         if (error = fx->init(); !error) {
            if (!result_name.empty()) parse_result(Self, fx, result_name);
            return ERR::Okay;
         }
      }
      else error = ERR::Search;
   }
   else error = ERR::FieldNotSet;

   FreeResource(fx);
   if (required) return log.warning(error);
   return ERR::Okay; // Default behaviour is not to force a failure despite the error.
}

//********************************************************************************************************************

ERR svgState::parse_fe_image(objVectorFilter *Filter, XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   // Check if the client has specified an href that refers to a pattern name instead of an image file.  In that
   // case we need to divert to the SourceFX parser.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if ((iequals("xlink:href", Tag.Attribs[a].Name)) or (iequals("href", Tag.Attribs[a].Name))) {
         if ((Tag.Attribs[a].Value[0] IS '#')) {
            return parse_fe_source(Filter, Tag);
         }
         break;
      }
   }

   objImageFX *fx;
   if (NewObject(CLASSID::IMAGEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   bool image_required = false;
   std::string path;
   std::string result_name;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_x:      fx->setX(SVGUnit(val)); break;
         case SVF_y:      fx->setY(SVGUnit(val)); break;
         case SVF_width:  fx->setWidth(SVGUnit(val)); break;
         case SVF_height: fx->setHeight(SVGUnit(val)); break;

         case SVF_image_rendering: {
            if ("optimizeSpeed" IS val) fx->setResampleMethod(VSM::BILINEAR);
            else if ("optimizeQuality" IS val) fx->setResampleMethod(VSM::LANCZOS);
            else if ("auto" IS val);
            else if ("inherit" IS val);
            else log.warning("Unrecognised image-rendering option '%s'", val.c_str());
            break;
         }

         case SVF_preserveAspectRatio:
            fx->setAspectRatio(parse_aspect_ratio(val));
            break;

         case SVF_xlink_href:
            path = val;
            break;

         case SVF_externalResourcesRequired: // If true and the image cannot be loaded, return a fatal error code.
            if (iequals("true", val)) image_required = true;
            break;

         case SVF_result: result_name = val; break;
      }
   }

   if (!path.empty()) {
      // Check for security risks in the path.

      if ((path[0] IS '/') or ((path[0] IS '.') and (path[1] IS '.') and (path[2] IS '/'))) {
         FreeResource(fx);
         return log.warning(ERR::InvalidValue);
      }
      else {
         if (path.find(':') != std::string::npos) {
            FreeResource(fx);
            return log.warning(ERR::InvalidValue);
         }

         for (unsigned i=0; path[i]; i++) {
            if (path[i] IS '/') {
               while (path[i+1] IS '.') i++;
               if (path[i+1] IS '/') {
                  return log.warning(ERR::InvalidValue);
               }
            }
         }
      }

      if (auto fl = folder(Self); not fl.empty()) {
         std::string comp_path = std::string(fl) + path;
         fx->setPath(comp_path);
      }
      else fx->setPath(path);
   }

   if (auto error = fx->init(); error != ERR::Okay) {
      FreeResource(fx);
      if (image_required) return error;
      else return ERR::Okay;
   }
   else {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
}

//********************************************************************************************************************

void svgState::proc_filter(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   objVectorFilter *filter;
   std::string id;

   if (!NewObject(CLASSID::VECTORFILTER, &filter)) {
      filter->setOwner(Self->Scene->UID);
      filter->setName("SVGFilter");
      filter->setUnits(VUNIT::BOUNDING_BOX);
      filter->setColourSpace(VCS::LINEAR_RGB);

      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         int j;
         for (j=0; Tag.Attribs[a].Name[j] and (Tag.Attribs[a].Name[j] != ':'); j++);
         if (Tag.Attribs[a].Name[j] IS ':') continue;

         switch(strhash(Tag.Attribs[a].Name)) {
            case SVF_filterUnits:
               if (iequals("userSpaceOnUse", val)) filter->Units = VUNIT::USERSPACE;
               else if (iequals("objectBoundingBox", val)) filter->Units = VUNIT::BOUNDING_BOX;
               break;

            case SVF_id:     id = val; break;

            case SVF_x:       filter->setX(SVGUnit(val)); break;
            case SVF_y:       filter->setY(SVGUnit(val)); break;
            case SVF_width:   filter->setWidth(SVGUnit(val)); break;
            case SVF_height:  filter->setHeight(SVGUnit(val)); break;
            case SVF_opacity: filter->setOpacity(std::clamp(svtonum<double>(val), 0.0, 1.0)); break;

            case SVF_filterRes: {
               double x = 0, y = 0;
               read_numseq(val, { &x, &y });
               filter->setResX(int(x));
               filter->setResY(int(y));
               break;
            }

            case SVF_color_interpolation_filters: // The default is linearRGB
               if ("auto" IS val) filter->setColourSpace(VCS::LINEAR_RGB);
               else if ("sRGB" IS val) filter->setColourSpace(VCS::SRGB);
               else if ("linearRGB" IS val) filter->setColourSpace(VCS::LINEAR_RGB);
               else if ("inherit" IS val) filter->setColourSpace(VCS::INHERIT);
               break;

            case SVF_primitiveUnits:
               if ("userSpaceOnUse" IS val) filter->PrimitiveUnits = VUNIT::USERSPACE; // Default
               else if ("objectBoundingBox" IS val) filter->PrimitiveUnits = VUNIT::BOUNDING_BOX;
               break;

/*
            case SVF_viewBox: {
               double x=0, y=0, width=0, height=0;
               read_numseq(val, { &x, &y, &width, &height });
               filter->Viewport->setViewX(x);
               filter->Viewport->setViewY(y);
               filter->Viewport->setViewWidth(width);
               filter->Viewport->setViewHeight(height);
               break;
            }
*/
            default:
               log.warning("<%s> attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
               break;
         }
      }

      if ((!id.empty()) and (!filter->init())) {
         SetName(filter, id);

         for (auto child : Tag.Children) {
            log.trace("Parsing filter element '%s'", child.name());

            switch(svg_tag_hash(child)) {
               case SVF_feBlur:              parse_fe_blur(filter, child); break;
               case SVF_feGaussianBlur:      parse_fe_blur(filter, child); break;
               case SVF_feOffset:            parse_fe_offset(filter, child); break;
               case SVF_feMerge:             parse_fe_merge(filter, child); break;
               case SVF_feColorMatrix:       // American spelling
               case SVF_feColourMatrix:      parse_fe_colour_matrix(filter, child); break;
               case SVF_feConvolveMatrix:    parse_fe_convolve_matrix(filter, child); break;
               case SVF_feDropShadow:        log.warning("Support for feDropShadow not yet implemented."); break;
               case SVF_feBlend:             // Blend and composite share the same code.
               case SVF_feComposite:         parse_fe_composite(filter, child); break;
               case SVF_feFlood:             parse_fe_flood(filter, child); break;
               case SVF_feTurbulence:        parse_fe_turbulence(filter, child); break;
               case SVF_feMorphology:        parse_fe_morphology(filter, child); break;
               case SVF_feImage:             parse_fe_image(filter, child); break;
               case SVF_feComponentTransfer: parse_fe_component_xfer(filter, child); break;
               case SVF_feDiffuseLighting:   parse_fe_lighting(filter, child, LT::DIFFUSE); break;
               case SVF_feSpecularLighting:  parse_fe_lighting(filter, child, LT::SPECULAR); break;
               case SVF_feDisplacementMap:   parse_fe_displacement_map(filter, child); break;
               case SVF_feWaveFunction:      parse_fe_wavefunction(filter, child); break;
               case SVF_feTile:
                  log.warning("Filter element '%s' is not currently supported.", child.name());
                  break;

               default:
                  log.warning("Filter element '%s' not recognised.", child.name());
                  break;
            }
         }

         Self->Effects.clear();

         if (!Self->Cloning) Self->Scene->addDef(id, filter);

         track_object(Self, filter);
      }
      else FreeResource(filter);
   }
}

//********************************************************************************************************************
// Pattern attributes are resolved through the href chain before they are applied.  The nearest explicit definition of
// each attribute wins, regardless of its position relative to the href attribute, and defaults apply once the chain
// has been exhausted.  Content is taken from the nearest pattern in the chain that has child elements.
//
// The viewBox and preserveAspectRatio attributes are resolved independently.  A viewBox makes patternContentUnits
// ineffective; the vector renderer enforces that precedence, so no synthetic viewbox is required for bounding-box
// content.

void svgState::proc_pattern(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objVectorPattern *pattern;

   if (NewObject(CLASSID::VECTORPATTERN, &pattern) != ERR::Okay) return;

   SetOwner(pattern, Self->Scene);

   pattern->setName("SVGPattern");
   pattern->setSpreadMethod(VSPREAD::REPEAT);
   pattern->Scene->setHostScene(Self->Scene);

   objVectorViewport *viewport;
   pattern->getViewport(viewport);

   std::string id;
   std::optional<VUNIT> units, content_units;
   std::optional<VOF> overflow;
   std::optional<ARF> aspect_ratio;
   std::optional<std::array<double, 4>> view_box;
   std::optional<double> opacity;
   std::optional<std::string> transform;
   Unit x, y, width, height;
   XTag *content = nullptr;
   ankerl::unordered_dense::set<XTag *> visited;

   for (auto tag = &Tag; tag; ) {
      visited.insert(tag);

      const auto invalid_value = [&](std::string_view Name) {
         log.warning("Invalid value for attribute '%.*s' @ line %d", int(Name.size()), Name.data(), tag->LineNo);
      };

      XTag *next = nullptr;
      // Declarations are stored in ascending cascade order.  Resolve the highest-priority valid value on this
      // pattern before consulting its href, whose values only fill properties that remain unspecified.
      for (auto a = std::ssize(tag->Attribs); a-- > 1; ) {
         auto &name = tag->Attribs[a].Name;
         auto &val = tag->Attribs[a].Value;
         if (val.empty()) continue;

         switch(strhash(name)) {
            case SVF_patternContentUnits:
               if (content_units) break;
               if (iequals("userSpaceOnUse", val)) content_units = VUNIT::USERSPACE;
               else if (iequals("objectBoundingBox", val)) content_units = VUNIT::BOUNDING_BOX;
               else invalid_value(name);
               break;

            case SVF_patternUnits:
               // 'userSpace' is a deprecated option from SVG 1.0 - perhaps due to the introduction of
               // patternContentUnits.
               if (units) break;
               if (iequals("userSpaceOnUse", val) or iequals("userSpace", val)) units = VUNIT::USERSPACE;
               else if (iequals("objectBoundingBox", val)) units = VUNIT::BOUNDING_BOX;
               else invalid_value(name);
               break;

            case SVF_patternTransform: if (not transform) transform = val; break;

            case SVF_id: if (tag IS &Tag) id = val; break;

            case SVF_overflow:
               if (overflow) break;
               switch(strhash(val)) {
                  case SVF_visible: overflow = VOF::VISIBLE; break;
                  case SVF_hidden:  overflow = VOF::HIDDEN; break;
                  case SVF_scroll:  overflow = VOF::SCROLL; break;
                  case SVF_inherit: overflow = VOF::INHERIT; break;
                  default: invalid_value(name);
               }
               break;

            case SVF_opacity: if (not opacity) opacity = svtonum<double>(val); break;
            case SVF_x:       if (not x.defined()) x = SVGUnit(val); break;
            case SVF_y:       if (not y.defined()) y = SVGUnit(val); break;
            case SVF_width:   if (not width.defined()) width = SVGUnit(val); break;
            case SVF_height:  if (not height.defined()) height = SVGUnit(val); break;

            case SVF_preserveAspectRatio: if (not aspect_ratio) aspect_ratio = parse_aspect_ratio(val); break;

            case SVF_viewBox: {
               if (view_box) break;
               std::array<double, 4> box = { 0, 0, 0, 0 };
               read_numseq(val, { &box[0], &box[1], &box[2], &box[3] });
               if ((box[2] > 0) and (box[3] > 0)) view_box = box;
               else invalid_value(name);
               break;
            }

            case SVF_href:
            case SVF_xlink_href:
               if (auto other = find_href_tag(Self, val)) {
                  if (not svg_tag_is(*other, SVF_pattern)) {
                     log.warning("Pattern reference '%s' @ line %d is not a pattern.", val.c_str(), tag->LineNo);
                  }
                  else if (visited.contains(other)) {
                     log.warning("Circular pattern reference '%s' @ line %d.", val.c_str(), tag->LineNo);
                  }
                  else next = other;
               }
               break;

            default:
               log.warning("%s attribute '%s' unrecognised @ line %d", tag->name(), name.c_str(), tag->LineNo);
               break;
         }
      }

      if ((not content) and (tag->hasChildTags())) content = tag;
      tag = next;
   }

   if (id.empty()) {
      log.trace("Failed to create a valid definition.");
      FreeResource(pattern);
      return;
   }

   // NOTE: In SVG 1.0 the default pattern units value is 'userSpaceOnUse'; from 1.1 it changed to 'objectBoundingBox'.

   const bool rel_coords = units.value_or(VUNIT::BOUNDING_BOX) IS VUNIT::BOUNDING_BOX;
   pattern->setUnits(units.value_or(VUNIT::BOUNDING_BOX));
   pattern->setContentUnits(content_units.value_or(VUNIT::USERSPACE));

   if (x.defined())      pattern->setX(Unit(x, rel_coords ? FD_SCALED : 0));
   if (y.defined())      pattern->setY(Unit(y, rel_coords ? FD_SCALED : 0));
   if (width.defined())  pattern->setWidth(Unit(width, rel_coords ? FD_SCALED : 0));
   if (height.defined()) pattern->setHeight(Unit(height, rel_coords ? FD_SCALED : 0));
   if (opacity)          pattern->setOpacity(*opacity);
   if (transform)        pattern->setTransform(*transform);
   if (overflow)         viewport->setOverflow(*overflow);
   if (aspect_ratio)     pattern->setAspectRatio(*aspect_ratio);

   if (view_box) {
      viewport->setViewX((*view_box)[0]);
      viewport->setViewY((*view_box)[1]);
      viewport->setViewWidth((*view_box)[2]);
      viewport->setViewHeight((*view_box)[3]);
   }

   if (!InitObject(pattern)) {
      // Child vectors for the pattern need to be instantiated and belong to the pattern's Viewport.
      if (content) {
         svgState state(Self);
         state.process_children(*content, viewport);
      }

      if (!Self->Cloning) {
         Self->Scene->addDef(id, pattern);
         track_object(Self, pattern);
      }
   }
   else {
      FreeResource(pattern);
      log.trace("Pattern initialisation failed.");
   }
}

//********************************************************************************************************************

ERR svgState::proc_shape(CLASSID VectorID, XTag &Tag, OBJECTPTR Parent, objVector * &Result) noexcept
{
   objVector *vector;

   Result = nullptr;
   if (auto error = NewObject(VectorID, &vector); !error) {
      SetOwner(vector, Parent);
      svgState state = *this;
      state.applyStateToVector(vector);
      state.applyTag(Tag); // Apply all attribute values to the current state.
      if (VectorID IS CLASSID::VECTORTEXT) {
         if (auto error = state.apply_text_properties((objVectorText *)vector); error != ERR::Okay) {
            FreeResource(vector);
            return error;
         }
      }
      state.process_attrib(Tag, vector);
      if (VectorID IS CLASSID::VECTORTEXT) {
         if (auto error = state.collect_text(Tag, (objVectorText *)vector); error != ERR::Okay) {
            FreeResource(vector);
            return error;
         }
      }

      if (!vector->init()) {
         state.process_shape_children(Tag, vector);
         Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(vector->UID) });
         Result = vector;
         return error;
      }
      else {
         if (VectorID IS CLASSID::VECTORTEXT) Self->TextContent.erase(vector->UID);
         FreeResource(vector);
         return ERR::Init;
      }
   }
   else return ERR::CreateObject;
}

//********************************************************************************************************************
// See also process_children()

ERR svgState::process_tag(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("%s", Tag.name());

   switch(svg_tag_hash(Tag)) {
      case SVF_use:              proc_use(Tag, Parent); break;
      case SVF_a:                proc_link(Tag, Parent, Vector); break;
      case SVF_switch:           proc_switch(Tag, Parent, Vector); break;
      case SVF_g:                proc_group(Tag, Parent, Vector); break;
      case SVF_svg:              proc_svg(Tag, Parent, Vector); break;

      case SVF_rect:             proc_shape(CLASSID::VECTORRECTANGLE, Tag, Parent, Vector); break;
      case SVF_ellipse:          proc_shape(CLASSID::VECTORELLIPSE, Tag, Parent, Vector); break;
      case SVF_circle:           proc_shape(CLASSID::VECTORELLIPSE, Tag, Parent, Vector); break;
      case SVF_path:             proc_shape(CLASSID::VECTORPATH, Tag, Parent, Vector); break;
      case SVF_polygon:          proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector); break;
      case SVF_kotuku_spiral:    proc_shape(CLASSID::VECTORSPIRAL, Tag, Parent, Vector); break;
      case SVF_kotuku_wave:      proc_shape(CLASSID::VECTORWAVE, Tag, Parent, Vector); break;
      case SVF_kotuku_shape:     proc_shape(CLASSID::VECTORSHAPE, Tag, Parent, Vector); break;
      case SVF_image:            proc_image(Tag, Parent, Vector); break;
      // Paint servers are deferred and will only be processed if they are referenced via url()
      case SVF_contourGradient:  process_inherit_refs(Tag); break;
      case SVF_distalGradient:   process_inherit_refs(Tag); break;
      case SVF_radialGradient:   process_inherit_refs(Tag); break;
      case SVF_diamondGradient:  process_inherit_refs(Tag); break;
      case SVF_conicGradient:    process_inherit_refs(Tag); break;
      case SVF_linearGradient:   process_inherit_refs(Tag); break;
      case SVF_meshgradient:     process_inherit_refs(Tag); break;
      case SVF_diffusionGradient: process_inherit_refs(Tag); break;
      case SVF_symbol:           proc_symbol(Tag); break;
      case SVF_animate:          proc_animate(Tag, ParentTag, Parent); break;
      case SVF_animateColor:     proc_animate_colour(Tag, ParentTag, Parent); break;
      case SVF_animateTransform: proc_animate_transform(Tag, Parent); break;
      case SVF_animateMotion:    proc_animate_motion(Tag, Parent); break;
      case SVF_set:              proc_set(Tag, ParentTag, Parent); break;
      case SVF_filter:           proc_filter(Tag); break;
      case SVF_defs:             proc_defs(Tag, Parent); break;
      case SVF_clipPath:         proc_clippath(Tag); break;
      case SVF_mask:             proc_mask(Tag); break;
      case SVF_style:            proc_style(Tag); break;
      case SVF_pattern:          proc_pattern(Tag); break;

      case SVF_title:
         Self->Title.clear();
         if (!Tag.Children.empty()) {
            if (auto buffer = Tag.getContent(); !buffer.empty()) {
               kt::ltrim(buffer);
               Self->Title.assign(buffer);
            }
         }
         break;

      case SVF_line:
         if (auto error = proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector); !error) {
            ((objVectorPolygon *)Vector)->setClosed(false);
            break;
         }
         else return error;

      case SVF_polyline:
         if (auto error = proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector); !error) {
            ((objVectorPolygon *)Vector)->setClosed(false);
            break;
         }
         else return error;

      case SVF_text:
         return proc_shape(CLASSID::VECTORTEXT, Tag, Parent, Vector);

      case SVF_desc: break; // Ignore descriptions
      case SVF_param: break; // Parameters are only meaningful as children of <use>; see proc_use()

      case 0: break;

      default: log.warning("Failed to interpret tag <%s/> @ line %d", Tag.name(), Tag.LineNo); return ERR::NoSupport;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// The Width/Height can be zero if the original image dimensions are desired.

static const std::string * xml_base_attrib(const XTag &Tag)
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals(Tag.Attribs[a].Name, "xml:base")) return &Tag.Attribs[a].Value;
   }
   return nullptr;
}

//********************************************************************************************************************

static bool has_kotuku_volume(const std::string &Path)
{
   auto volume_end = Path.find(':');
   if ((volume_end IS std::string::npos) or (volume_end IS 0)) return false;
   auto separator = Path.find_first_of("/\\");
   if ((separator != std::string::npos) and (separator < volume_end)) return false;

   auto volume = Path.substr(0, volume_end + 1);

   static std::mutex cache_lock;
   static ankerl::unordered_dense::map<std::string, bool> volume_cache;

   {
      const std::lock_guard<std::mutex> lock(cache_lock);
      if (auto cached = volume_cache.find(volume); cached != volume_cache.end()) return cached->second;
   }

   // AnalysePath() confirms that the leading "<volume>:" prefix names a registered Kotuku volume.
   auto type = LOC::NIL;
   const bool is_volume = (!AnalysePath(volume, &type)) and (type IS LOC::VOLUME);

   const std::lock_guard<std::mutex> lock(cache_lock);
   volume_cache[volume] = is_volume;
   return is_volume;
}

//********************************************************************************************************************

static std::string resolve_image_href(extSVG *Self, XTag &Tag, const std::string &Path)
{
   // Embedded, Kotuku volume and already absolute image references are complete as provided.
   if (Path.empty() or Path.starts_with("data:") or has_kotuku_volume(Path) or xml::uri::is_absolute_uri(Path)) {
      return Path;
   }

   // Walk from the image tag to the root so inherited xml:base declarations can be applied in document order.
   std::vector<XTag *> chain;
   XTag *scan = &Tag;
   while (scan) {
      chain.push_back(scan);
      if (!scan->ParentID or (!Self->XML)) break;

      XTag *parent = nullptr;
      if (Self->XML->getTag(scan->ParentID, &parent) != ERR::Okay) break;
      scan = parent;
   }

   std::string base;
   if (auto doc_folder = folder(Self); not doc_folder.empty()) {
      base = xml::uri::normalise_uri_separators(std::string(doc_folder));
   }

   // xml:base values are themselves relative to the active base URI at the point where they are declared.
   for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (auto attrib = xml_base_attrib(**it)) {
         if (base.empty()) base = *attrib;
         else base = xml::uri::resolve_relative_uri(*attrib, base);
         base = xml::uri::normalise_uri_separators(std::move(base));
      }
   }

   // No base URI is available for statement-only SVG content, so keep the original relative path.
   if (base.empty()) return Path;
   return xml::uri::resolve_relative_uri(Path, base);
}

//********************************************************************************************************************

// Proxy for creating the RSVG class through kt::Create, which has no public C++ interface.

struct objRSVGImage : public objImage {
   static constexpr CLASSID CLASS_ID = CLASSID::RSVG;
   static constexpr CSTRING CLASS_NAME = "RSVG";
};

static ERR load_pic(extSVG *Self, std::string Path, objImage **Image, Unit Width = Unit(), Unit Height = Unit())
{
   kt::Log log(__FUNCTION__);

   *Image = nullptr;
   objFile *file = nullptr;
   std::string_view val(Path);

   ERR error = ERR::Okay;
   if (val.starts_with("icons:")) {
      // Kotuku feature: Load an SVG image from the icon database.  Nothing needs to be done here
      // because the fielsystem volume is built-in.
   }
   else if (val.starts_with("data:")) { // Check for embedded content
      log.branch("Detected embedded source data");
      val.remove_prefix(5);
      if (val.starts_with("image/")) { // Has to be an image type
         val.remove_prefix(6);
         if (auto sep = val.find(';'); sep != std::string_view::npos) val.remove_prefix(sep);
         else val.remove_prefix(val.size());
         if (val.starts_with(";base64")) { // Is it base 64?
            val.remove_prefix(7);
            if (auto comma = val.find(','); comma != std::string_view::npos) val.remove_prefix(comma + 1);
            else val.remove_prefix(val.size());

            kt::BASE64DECODE state;
            clearmem(&state, sizeof(state));

            std::vector<uint8_t> output(val.size());
            int64_t written;
            if (!(error = kt::Base64Decode(&state, val,
                     std::span<int8_t>((int8_t *)output.data(), output.size()), &written))) {
               Path = "temp:svg.img";
               if ((file = objFile::create::local(fl::Path(Path), fl::Flags(FL::NEW|FL::WRITE)))) {
                  int result;
                  file->write(std::span<const int8_t>((const int8_t *)output.data(), size_t(written)), &result);
               }
               else error = ERR::File;
            }
         }
         else error = ERR::StringFormat;
      }
      else error = ERR::StringFormat;
   }
   else log.branch("%s", Path.c_str());

   if (!error) {
      // IdentifyFile() cannot resolve a path that carries an SVG parameter query string, so the RSVG class is
      // targeted directly in that case.  The query is then interpreted by the nested SVG object.

      std::string_view file, query;
      if (split_svg_query(Path, file, query)) {
         if (!(*Image = kt::Create<objRSVGImage>::global(
            fl::Owner(Self->Scene->UID),
            fl::Path(Path),
            fl::DisplayWidth(Width.defined() ? int(Width) : 0), fl::DisplayHeight(Height.defined() ? int(Height) : 0),
            fl::Flags(PCF::FORCE_ALPHA_32)))) error = ERR::CreateObject;
      }
      // Note: A Width/Height of 0 will use the image's actual width/height.
      else if (!(*Image = objImage::create::global(
         fl::Owner(Self->Scene->UID),
         fl::Path(Path),
         fl::BitsPerPixel(32),
         fl::DisplayWidth(Width.defined() ? int(Width) : 0), fl::DisplayHeight(Height.defined() ? int(Height) : 0),
         fl::Flags(PCF::FORCE_ALPHA_32)))) error = ERR::CreateObject;
   }

   if (file) {
      file->del({});
      FreeResource(file);
   }

   if (error != ERR::Okay) log.warning(error);
   return error;
}

//********************************************************************************************************************
// Definition images are stored once, allowing them to be used multiple times via Fill and Stroke references.

void svgState::proc_def_image(XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);
   objVectorImage *image;
   std::string id, src;
   Unit width, height;

   if (!NewObject(CLASSID::VECTORIMAGE, &image)) {
      image->setOwner(Self->Scene->UID);
      image->setName("SVGImage");
      image->setUnits(VUNIT::BOUNDING_BOX);
      image->setSpreadMethod(VSPREAD::PAD);

      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         switch(strhash(Tag.Attribs[a].Name)) {
            case SVF_units:
               if ("userSpaceOnUse" IS val) image->Units = VUNIT::USERSPACE;
               else if ("objectBoundingBox" IS val) image->Units = VUNIT::BOUNDING_BOX;
               else log.warning("Unknown <image> units reference '%s'", val.c_str());
               break;

            case SVF_xlink_href: src = val; break;
            case SVF_id:     id = val; break;
            // Applying (x,y) values as a texture offset here appears to be a mistake because <use> will deep-clone
            // the values also.  SVG documentation is silent on the validity of (x,y) values when an image
            // is in the <defs> area, so a W3C test may be needed to settle the matter.
            case SVF_x:      /*image->setX(SVGUnit(val));*/ break;
            case SVF_y:      /*image->setY(SVGUnit(val));*/ break;
            case SVF_width:  width = SVGUnit(val); break;
            case SVF_height: height = SVGUnit(val); break;
            default: {
               // Check if this was a reference to some other namespace (ignorable).
               int i;
               for (i=0; val[i] and (val[i] != ':'); i++);
               if (val[i] != ':') log.warning("Failed to parse attrib '%s' in <image/> tag @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
               break;
            }
         }
      }

      if ((!id.empty()) and (!src.empty())) {
         src = apply_image_params(Self, Tag, src, m_param_scope);

         objImage *pic;
         if (!load_pic(Self, resolve_image_href(Self, Tag, src), &pic, width, height)) {
            image->setImage(pic);
            if (!InitObject(image)) {
               if (!Self->Cloning) {
                  Self->Scene->addDef(id, image);
                  track_object(Self, image);
               }
            }
            else {
               FreeResource(image);
               log.trace("Image initialisation failed.");
            }
         }
         else {
            FreeResource(image);
            log.trace("Unable to load an image for <image/> '%s' at line %d", id.c_str(), Tag.LineNo);
         }
      }
      else {
         FreeResource(image);
         log.trace("No id or src specified in <image/> at line %d", Tag.LineNo);
      }
   }
}

//********************************************************************************************************************

ERR svgState::proc_image(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   kt::Log log(__FUNCTION__);

   std::string src, filter, transform, id;
   ARF ratio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // SVG default if the client leaves preserveAspectRatio undefined
   Unit x, y, width, height;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      switch (strhash(Tag.Attribs[a].Name)) {
         case SVF_xlink_href:
         case SVF_href: src = value; break;
         case SVF_id: id = value; break;
         case SVF_transform: transform = value; break;
         case SVF_preserveAspectRatio: ratio = parse_aspect_ratio(value); break;
         case SVF_x: x = SVGUnit(value); break;
         case SVF_y: y = SVGUnit(value); break;
         case SVF_width:
            width = SVGUnit(value);
            if ((not width.defined()) or (double(width) < 0.001)) return log.warning(ERR::InvalidDimension);
            break;
         case SVF_height:
            height = SVGUnit(value);
            if ((not height.defined()) or (double(height) < 0.001)) return log.warning(ERR::InvalidDimension);
            break;
         case SVF_crossOrigin: break; // Defines the value of the credentials flag for CORS requests.
         case SVF_decoding: break; // Hint as to whether image decoding is synchronous or asynchronous
         case SVF_clip: break; // Deprecated from SVG; allows a rect() to be declared that functions as a clip-path
      }
   }

   if (src.empty()) return ERR::FieldNotSet;

   // <param> children are passed to an SVG image as query parameters.  Each distinct href needs its own cached image,
   // so the ID cannot be stored in the tag as it may be reused by other <use> instances.

   auto param_src = apply_image_params(Self, Tag, src, m_param_scope);
   const bool has_params = param_src != src;
   if (has_params) src = std::move(param_src);

   if (id.empty()) {
      // An image always has an ID; this ensures that if the image bitmap is referenced repeatedly via a <symbol> then
      // we won't keep reloading it into the cache.
      id = "img_" + std::to_string(strhash(src));
      if (width.defined()) id += "_" + std::to_string(width);
      if (height.defined()) id += "_" + std::to_string(height);
      if (not has_params) xml::NewAttrib(Tag, "id", id);
   }
   else if (has_params) id += std::format("~p{:x}", uint64_t(ankerl::unordered_dense::hash<std::string>{}(src)));

   if (Self->Scene->findDef(id, nullptr) != ERR::Okay) {
      // Load the image and add it to the vector definition.  It will be rendered as a rectangle within the scene.
      // This may appear a little confusing because an image can be invoked in SVG like a first-class shape; however to
      // do so would be inconsistent with all other scene graph members being true path-based objects.

      objImage *pic = nullptr;
      load_pic(Self, resolve_image_href(Self, Tag, src), &pic, width, height);

      if (pic) {
         if (auto image = objVectorImage::create::global(
               fl::Owner(Self->Scene->UID),
               fl::Image(pic),
               fl::Units(VUNIT::BOUNDING_BOX),
               fl::AspectRatio(ratio))) {

            SetOwner(pic, image); // It's best if the pic belongs to the image.

            Self->Scene->addDef(id, image);
            track_object(Self, image);
         }
         else return ERR::CreateObject;
      }
      else log.warning("Failed to load image via xlink:href.");
   }

   // NOTE: Officially, the SVG standard requires that a viewport is created to host the image (this would still
   // require a filled rectangle to reference the image).  In practice this doesn't seem necessary, as the image
   // object supports built-in viewport concepts like preserveAspectRatio.

   objVectorRectangle *rect = nullptr;
   if (auto error = NewObject(CLASSID::VECTORRECTANGLE, &rect); !error) {
      SetOwner(rect, Parent);
      applyStateToVector(rect);

      // All attributes of <image> will be applied to the rectangle.

      process_attrib(Tag, rect);

      if (x.defined()) rect->setX(x);
      if (y.defined()) rect->setY(y);
      if (width.defined()) rect->setWidth(width);
      if (height.defined()) rect->setHeight(height);

      rect->setFill("url(#" + id + ")");

      if (!rect->init()) {
         process_shape_children(Tag, rect);
         Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(rect->UID) });
         Vector = rect;
         return ERR::Okay;
      }
      else {
         FreeResource(rect);
         return ERR::Init;
      }
   }
   else return ERR::CreateObject;
}

//********************************************************************************************************************

ERR svgState::proc_defs(XTag &Tag, OBJECTPTR Parent) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   for (auto &child : Tag.Children) {
      switch (svg_tag_hash(child)) {
         case SVF_contourGradient: state.proc_contourgradient(child); break;
         case SVF_distalGradient:  state.proc_distalgradient(child); break;
         case SVF_radialGradient:  state.proc_radialgradient(child); break;
         case SVF_diamondGradient: state.proc_diamondgradient(child); break;
         case SVF_conicGradient:   state.proc_conicgradient(child); break;
         case SVF_linearGradient:  state.proc_lineargradient(child); break;
         case SVF_meshgradient:    state.proc_meshgradient(child); break;
         case SVF_diffusionGradient: state.proc_diffusiongradient(child); break;
         case SVF_pattern:         state.proc_pattern(child); break;
         case SVF_image:           state.proc_def_image(child); break;
         case SVF_filter:          state.proc_filter(child); break;
         case SVF_clipPath:        state.proc_clippath(child); break;
         case SVF_mask:            state.proc_mask(child); break;
         case SVF_kotuku_transition: state.proc_pathtransition(child); break;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR svgState::proc_style(XTag &Tag)
{
   // Stylesheets have already been collected before any scene objects are created.
   return ERR::Okay;
}

//********************************************************************************************************************
// Declare a 'symbol' which is basically a template for inclusion elsewhere through the use of a 'use' element.
//
// When a use element is encountered, it looks for the associated symbol ID and then processes the XML child tags that
// belong to it.

void svgState::proc_symbol(XTag &Tag) noexcept
{
   // Symbols are ignored at these stage as they will already have been parsed for their 'id' reference.
}

//********************************************************************************************************************
// Most vector shapes can be morphed to the path of another vector.

void svgState::proc_guide_path(XTag &Tag, OBJECTPTR Parent) noexcept
{
   kt::Log log(__FUNCTION__);

   if ((!Parent) or (Parent->Class->BaseClassID != CLASSID::VECTOR)) {
      log.traceWarning("Unable to apply guidePath to non-vector parent object.");
      return;
   }

   // Find the definition that is being referenced for the guide path.

   std::string ref;
   std::string transition;
   VMF flags = VMF::NIL;
   ARF align = ARF::NIL;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_path:
         case SVF_xlink_href:  ref = val; break;
         case SVF_transition:  transition = val; break;
         case SVF_startOffset:
            log.warning("Guide startOffset is not supported by text layout.");
            break;
         case SVF_method:
            if (iequals("align", val)) flags &= ~VMF::STRETCH;
            else if (iequals("stretch", val)) flags |= VMF::STRETCH;
            break;

         case SVF_spacing:
            if (iequals("auto", val)) {
               flags |= VMF::AUTO_SPACING;
               log.warning("Automatic guide spacing is not supported by text layout.");
            }
            else if (iequals("exact", val)) flags &= ~VMF::AUTO_SPACING;
            break;

         case SVF_align:
            align |= parse_aspect_ratio(val);
            break;
      }
   }

   if (ref.empty()) {
      log.warning("<guidePath> element @ line %d is missing a valid xlink:href attribute.", Tag.LineNo);
      return;
   }

   // Find the matching element with matching ID

   auto uri = uri_name(ref);
   if (uri.empty()) {
      log.warning("Invalid URI string '%s' at line %d", ref.c_str(), Tag.LineNo);
      return;
   }

   auto tag = Self->IDs.find(uri);
   if (tag IS Self->IDs.end()) {
      log.warning("Unable to find element '%s' referenced at line %d", ref.c_str(), Tag.LineNo);
      return;
   }

   OBJECTPTR transvector = nullptr;
   if (!transition.empty()) {
      if (Self->Scene->findDef(transition, &transvector) != ERR::Okay) {
         log.warning("Unable to find element '%s' referenced at line %d", transition.c_str(), Tag.LineNo);
         return;
      }
   }

   auto tagref = tag->second;

   auto class_id = CLASSID::NIL;
   switch (svg_tag_hash(*tagref)) {
      case SVF_path:          class_id = CLASSID::VECTORPATH; break;
      case SVF_rect:          class_id = CLASSID::VECTORRECTANGLE; break;
      case SVF_ellipse:       class_id = CLASSID::VECTORELLIPSE; break;
      case SVF_circle:        class_id = CLASSID::VECTORELLIPSE; break;
      case SVF_polygon:       class_id = CLASSID::VECTORPOLYGON; break;
      case SVF_kotuku_spiral: class_id = CLASSID::VECTORSPIRAL; break;
      case SVF_kotuku_wave:   class_id = CLASSID::VECTORWAVE; break;
      case SVF_kotuku_shape:  class_id = CLASSID::VECTORSHAPE; break;
      default:
         log.warning("Invalid reference '%s', '%s' is not recognised by <guidePath>.", ref.c_str(), tagref->name());
   }

   if ((flags & (VMF::Y_MIN|VMF::Y_MID|VMF::Y_MAX)) IS VMF::NIL) {
      if (Parent->classID() IS CLASSID::VECTORTEXT) flags |= VMF::Y_MIN;
      else flags |= VMF::Y_MID;
   }

   if (class_id != CLASSID::NIL) {
      objVector *shape;
      svgState state(Self);
      state.proc_shape(class_id, *tagref, Self->Scene, shape);
      auto parent_vector = (objVector *)Parent;
      parent_vector->setGuidePath(shape);
      if (transvector) parent_vector->setTransition(transvector);
      parent_vector->setGuideFlags(flags);
      if (!Self->Cloning) Self->Scene->addDef(uri, shape);
   }
}

//********************************************************************************************************************
// Duplicates a referenced area of the SVG definition.
//
// "The effect of a 'use' element is as if the contents of the referenced element were deeply cloned into a separate
// non-exposed DOM tree which had the 'use' element as its parent and all of the 'use' element's ancestors as its
// higher-level ancestors.

void svgState::proc_use(XTag &Tag, OBJECTPTR Parent) noexcept
{
   kt::Log log(__FUNCTION__);

   std::string ref;
   for (int a=1; (a < std::ssize(Tag.Attribs)) and ref.empty(); a++) {
      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_href: // SVG2
         case SVF_xlink_href: ref = Tag.Attribs[a].Value; break;
      }
   }

   if (ref.empty()) {
      log.warning("<use> element @ line %d is missing a valid href attribute.", Tag.LineNo);
      return;
   }

   // Find the matching element with matching ID

   auto tagref = find_href_tag(Self, ref);
   if (!tagref) {
      log.warning("Unable to find element '%s'", ref.c_str());
      return;
   }

   // Increment the Cloning variable to indicate that we are in a region that is being cloned.
   // This is important for some elements like clip-path, whereby the path only needs to be created
   // once and can then be referenced multiple times.

   Self->Cloning++;
   auto dc = deferred_call([this] {
      Self->Cloning--;
   });

   // SVG Parameters: <param> children define values for this instance only.  They inherit the enclosing scope and
   // their values are resolved against it, so <param name="a" value="param(b) red"/> is permitted.  Whitespace and
   // <param> children do not count as content for the purpose of choosing the cloning strategy below.

   ParamScope scope;
   bool has_params = false, has_content = false;
   for (auto &child : Tag.Children) {
      if (child.isContent()) {
         if (not trim_ws(child.Attribs[0].Value).empty()) has_content = true;
         continue;
      }
      else if (not svg_tag_is(child, SVF_param)) {
         has_content = true;
         continue;
      }

      std::string name;
      std::optional<std::string> value;
      if (not read_param_element(Self, child, m_param_scope, name, value)) continue;

      if (not has_params) {
         if (m_param_scope) scope = *m_param_scope;
         has_params = true;
      }

      if (value) scope.Values[name] = std::move(*value);
      else scope.Values.erase(name);
   }

   auto saved_scope = m_param_scope;
   if (has_params) m_param_scope = &scope;
   auto restore_scope = deferred_call([this, saved_scope] {
      m_param_scope = saved_scope;
   });

   if ((iequals("symbol", tagref->name())) or (iequals("svg", tagref->name()))) {
      // SVG spec requires that we create a VectorGroup and then create a Viewport underneath that.  However if there
      // are no attributes to apply to the group then there is no sense in creating an empty one.

      // TODO: We should be using the same replace-and-expand tag method that is applied for group
      // handling, as seen further below in this routine.

      objVectorViewport *viewport = nullptr;

      // Parameterised documents process a private copy so that every instance applies its own nested parameter
      // declarations.

      SymbolInstance symbol(Self, *tagref, m_param_scope);
      tagref = symbol.tag;

      auto state = *this;
      state.applyTag(Tag); // Apply all attribute values to the current state.

      objVector *group = nullptr;
      bool need_group = false;
      for (int a=1; (a < std::ssize(Tag.Attribs)) and (!need_group); a++) {
         switch(strhash(Tag.Attribs[a].Name)) {
            case SVF_x: case SVF_y: case SVF_width: case SVF_height: break;
            default: need_group = true; break;
         }
      }

      if (need_group) {
         if (!NewObject(CLASSID::VECTORGROUP, &group)) {
            SetOwner(group, Parent);
            group->setVisibility(VIS::INHERIT);
            Parent = group;
            group->init();
         }
      }

      if (NewObject(CLASSID::VECTORVIEWPORT, &viewport) != ERR::Okay) return;
      SetOwner(viewport, Parent);
      viewport->setWidth(Unit(1.0, FD_SCALED));
      viewport->setHeight(Unit(1.0, FD_SCALED)); // SVG default

      // Apply attributes from 'use' to the group and/or viewport
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         auto hash = strhash(Tag.Attribs[a].Name);
         switch(hash) {
            // X,Y,Width,Height are applied to the viewport
            case SVF_x:      viewport->setX(SVGUnit(val)); break;
            case SVF_y:      viewport->setY(SVGUnit(val)); break;
            case SVF_width:  viewport->setWidth(SVGUnit(val)); break;
            case SVF_height: viewport->setHeight(SVGUnit(val)); break;

            // All other attributes are applied to the 'g' element
            default:
               if (group) state.set_property(group, hash, Tag, val);
               else state.set_property(viewport, hash, Tag, val);
               break;
         }
      }

      // Apply attributes from the symbol itself to the viewport

      for (int a=1; a < std::ssize(tagref->Attribs); a++) {
         auto &val = tagref->Attribs[a].Value;
         if (val.empty()) continue;

         switch(strhash(tagref->Attribs[a].Name)) {
            case SVF_x:      viewport->setX(SVGUnit(val)); break;
            case SVF_y:      viewport->setY(SVGUnit(val)); break;
            case SVF_width:  viewport->setWidth(SVGUnit(val)); break;
            case SVF_height: viewport->setHeight(SVGUnit(val)); break;
            case SVF_viewBox:  {
               double x=0, y=0, width=0, height=0;
               read_numseq(val, { &x, &y, &width, &height });
               viewport->setViewX(x);
               viewport->setViewY(y);
               viewport->setViewWidth(width);
               viewport->setViewHeight(height);
               break;
            }
            case SVF_id: break; // Ignore (already processed).
            default: log.warning("Not processing attribute '%s'", tagref->Attribs[a].Name.c_str()); break;
         }
      }

      if (viewport->init() != ERR::Okay) { FreeResource(viewport); return; }

      // Add all child elements in <symbol> to the viewport.

      log.traceBranch("Processing all child elements within %s", ref.c_str());
      state.process_children(*tagref, viewport);
   }
   else {
      // W3C: In the generated content, the 'use' will be replaced by 'g', where all attributes from the 'use' element
      // except for 'x', 'y', 'width', 'height' and 'xlink:href' are transferred to the generated 'g' element. An
      // additional transformation translate(x,y) is appended to the end (i.e., right-side) of the 'transform'
      // attribute on the generated 'g', where x and y represent the values of the 'x' and 'y' attributes on the
      // 'use' element. The referenced object and its contents are deep-cloned into the generated tree.

      auto use_attribs = Tag.Attribs;
      Tag.Attribs[0].Name = "g";
      if (Tag.Attribs.size() > 1) Tag.Attribs.erase(Tag.Attribs.begin()+1, Tag.Attribs.end());

      if (not has_content) {
         Tag.Children = { *tagref }; // Deep-clone
         if (not Self->ParamAttribs.empty()) rescope_params(Self, Tag.Children[0], m_param_scope);

         // Apply 'use' attributes, making a special case for 'x' and 'y'.

         Unit tx, ty;
         for (int t=1; t < std::ssize(use_attribs); t++) {
            switch (strhash(use_attribs[t].Name)) {
               case SVF_x: tx = SVGUnit(use_attribs[t].Value); break;
               case SVF_y: ty = SVGUnit(use_attribs[t].Value); break;
               // SVG states that the following are not to be applied to the group...
               case SVF_width:
               case SVF_height:
               case SVF_xlink_href:
               case SVF_href:
                  break;

               default:
                  Tag.Attribs.push_back(use_attribs[t]);
            }
         }

         const double tx_value = tx.defined() ? double(tx) : 0.0;
         const double ty_value = ty.defined() ? double(ty) : 0.0;
         if (tx_value or ty_value) {
            auto transform = "translate(" + std::to_string(tx_value) + " " + std::to_string(ty_value) + ")";
            Tag.Attribs.push_back({ "transform", transform });
         }
      }
      else {
         // The SVG documentation appears to be silent on the matter of children in the <use> element.  So far we've
         // encountered animate instructions in the <use> area, and this expands into a complicated dual-group configuration
         // (at least that appears to be the only way we can get our animations to match expected behaviour patterns in W3C
         // tests).

         auto &subgroup = Tag.Children.emplace_back(XTag(0));
         subgroup.Attribs.push_back(XMLAttrib("g"));
         subgroup.Children.push_back(*tagref);
         if (not Self->ParamAttribs.empty()) rescope_params(Self, subgroup.Children[0], m_param_scope);

         Unit tx, ty;
         for (int t=1; t < std::ssize(use_attribs); t++) {
            switch (strhash(use_attribs[t].Name)) {
               case SVF_x: tx = SVGUnit(use_attribs[t].Value); break;
               case SVF_y: ty = SVGUnit(use_attribs[t].Value); break;
               case SVF_width:
               case SVF_height:
               case SVF_xlink_href:
               case SVF_href:
                  break;

               default:
                  subgroup.Attribs.push_back(use_attribs[t]);
            }
         }

         const double tx_value = tx.defined() ? double(tx) : 0.0;
         const double ty_value = ty.defined() ? double(ty) : 0.0;
         if (tx_value or ty_value) {
            auto transform = "translate(" + std::to_string(tx_value) + " " + std::to_string(ty_value) + ")";
            subgroup.Attribs.push_back({ "transform", transform });
         }
      }

      objVector *new_group = nullptr;
      proc_group(Tag, Parent, new_group);
   }
}

//********************************************************************************************************************
// "a href" link support.  The child vectors belonging to the link must be monitored for click events.
// https://www.w3.org/TR/SVG2/linking.html

static ERR link_event(objVector *Vector, const InputEvent *Events, svgLink *Link)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extSVG *)CurrentContext();

   for (auto event=Events; event; event=event->Next) {
      if ((event->Type IS JET::LMB) and ((event->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
         if (!event->Value) continue;

         if (Link->ref.starts_with('#')) {
            // The link activates a document node, like an animation.
            if (find_href_tag(Self, Link->ref)) {
               for (auto &record : Self->Animations) {
                  std::visit([ Link, Self ](auto &&anim) {
                     if (anim.id IS Link->ref.substr(1)) anim.activate(true);
                  }, record);
               }
            }
            else log.warning("Unknown reference '%s'", Link->ref.c_str());
         }
         else {
            // The link is a URI that could refer to an HTTP location, local file, etc...
            log.warning("URI links are not yet supported.");
         }
      }
   }

   return ERR::Okay;
}

void svgState::proc_link(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   auto link = std::make_unique<svgLink>();

   if (Tag.Children.empty()) return;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         case SVF_href:
         case SVF_xlink_href:
            link.get()->ref = val;
            break;
         // SVF_target: _self, _parent, _top, _blank
         // SVF_download, SVF_ping, SVF_rel, SVF_hrefLang, SVF_type, SVF_referrerPolicy
      }
   }

   // Vectors within <a> will be assigned to a group purely because it's easier for us to manage them that way.

   objVector *group;
   if (!NewObject(CLASSID::VECTORGROUP, &group)) {
      SetOwner(group, Parent);
      group->setVisibility(VIS::INHERIT);

      auto state = *this;
      state.applyTag(Tag);
      state.process_attrib(Tag, group);
      state.process_children(Tag, group);

      kt::vector<ChildEntry> list;
      if (!ListChildren(group->UID, &list)) {
         for (auto &child : list) {
            auto obj = (objVector *)GetObjectPtr(child.ObjectID);
            obj->subscribeInput(JTYPE::BUTTON, C_FUNCTION(link_event, link.get()));
         }
         Self->Links.emplace_back(std::move(link));
      }

      if ((!Self->Links.empty()) and (!group->init())) Vector = group;
      else FreeResource(group);
   }
}

//********************************************************************************************************************
// For each immediate child, evaluate any requiredFeatures, requiredExtensions and systemLanguage attributes.
// The first child where these attributes evaluate to true is rendered, the rest are ignored.

void svgState::proc_switch(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   for (auto &child : Tag.Children) {
      bool render = true;
      for (int a=1; a < std::ssize(child.Attribs); a++) {
         auto &val = child.Attribs[a].Value;
         if (val.empty()) continue;

         switch(strhash(child.Attribs[a].Name)) {
            case SVF_requiredFeatures:
               // requiredFeatures is deprecated as of SVG2, but the following features are explicitly not supported
               // by our SVG implementation:
               if (val IS "http://www.w3.org/TR/SVG11/feature#SVG-dynamic") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#DocumentEventsAttribute") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#GraphicalEventsAttribute") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#Script") render = false;
               break;

            case SVF_requiredExtensions: // Allows the client to check if a given customised extension is supported.
               if (val IS "http://www.kotuku.dev/TR/Kotuku/1.0");
               else render = false;
               break;

            case SVF_systemLanguage: // CSV list of language codes from BCP47
               // GetLocaleInfoEx() LOCALE_SISO639LANGNAME
               break;
         }
      }

      if (render) {
         if (Tag.Attribs.size() > 1) { // The <switch> element is treated as a container type
            kt::Log log(__FUNCTION__);
            log.traceBranch("Tag: %d", Tag.ID);

            auto state = *this;

            objVector *group;
            if (NewObject(CLASSID::VECTORGROUP, &group) != ERR::Okay) return;
            SetOwner(group, Parent);
            group->setVisibility(VIS::INHERIT);
            state.applyTag(Tag);
            state.process_attrib(Tag, group);

            if (!group->init()) {
               Vector = group;
               Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(group->UID) });

               state.process_tag(child, Tag, group, Vector);
            }
            else FreeResource(group);
         }
         else process_tag(child, Tag, Parent, Vector);

         return;
      }
   }
}

//********************************************************************************************************************

void svgState::proc_group(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   auto state = *this;

   objVector *group;
   if (NewObject(CLASSID::VECTORGROUP, &group) != ERR::Okay) return;
   SetOwner(group, Parent);
   // SVG visibility inherits through groups unless an attribute overrides it.
   group->setVisibility(VIS::INHERIT);
   state.applyTag(Tag); // Apply all group attribute values to the current state.
   state.process_attrib(Tag, group);
   state.process_children(Tag, group);

   if (!group->init()) {
      Vector = group;
      Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(group->UID) });
   }
   else FreeResource(group);
}

//********************************************************************************************************************
// <svg/> tags can be embedded inside <svg/> tags - this establishes a new viewport.
// Refer to section 7.9 of the SVG Specification for more information.

void svgState::proc_svg(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   kt::Log log(__FUNCTION__);
   int a;

   if (!Parent) {
      log.warning("A Parent object is required.");
      return;
   }

   // If initialising to a VectorScene, prefer to use its existing viewport if there is one.

   objVectorViewport *viewport;
   bool reused_viewport = false;
   if ((Parent->classID() IS CLASSID::VECTORSCENE) and (((objVectorScene *)Parent)->Viewport)) {
      viewport = ((objVectorScene *)Parent)->Viewport;
      reused_viewport = true;
   }
   else {
      if (NewObject(CLASSID::VECTORVIEWPORT, &viewport) != ERR::Okay) return;
      SetOwner(viewport, Parent);
   }

   // The first viewport to be instantiated is stored as a local reference.  This is important if the developer has
   // specified a custom target, in which case there needs to be a way to discover the root of the SVG.

   if (!Self->Viewport) Self->Viewport = viewport;

   // SVG content composites in sRGB by default, as browsers do.  A top-level viewport sets the policy for the document
   // and nested viewports inherit it.  The color-interpolation attribute can override it for a subtree.  A viewport
   // reused from a target scene belongs to the host, so its colour space is left for the host to decide.

   if ((!reused_viewport) and ((Parent IS Self->Scene) or (Parent IS Self->Target))) {
      viewport->setColourSpace(VCS::SRGB);
   }

   // Process <svg> attributes

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   for (a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;
      if (text_property_initial(strhash(Tag.Attribs[a].Name))) continue;

      switch(strhash(Tag.Attribs[a].Name)) {
         // The viewbox determines what area of the vector definition is to be displayed (in a sense, zooming into the document).
         // The individual x, y, width and height values determine the position and clipping of the displayed SVG content.

         case SVF_viewBox:  {
            auto dim = read_array(val);
            if (dim.size() >= 4) {
               viewport->setViewX(dim[0]);
               viewport->setViewY(dim[1]);
               viewport->setViewWidth(dim[2]);
               viewport->setViewHeight(dim[3]);
            }
            break;
         }

         case SVF_version: {
            auto version = svtonum<double>(val);
            if (version > Self->SVGVersion) Self->SVGVersion = version;
            break;
         }

         case SVF_x: viewport->setX(SVGUnit(val)); break;
         case SVF_y: viewport->setY(SVGUnit(val)); break;

         case SVF_xOffset: viewport->setXOffset(SVGUnit(val)); break;
         case SVF_yOffset: viewport->setYOffset(SVGUnit(val)); break;

         case SVF_width:
            viewport->setWidth(SVGUnit(val));
            viewport->setOverflowX(VOF::HIDDEN);
            break;

         case SVF_height:
            viewport->setHeight(SVGUnit(val));
            viewport->setOverflowY(VOF::HIDDEN);
            break;

         case SVF_preserveAspectRatio: viewport->setAspectRatio(parse_aspect_ratio(val)); break;

         case SVF_opacity: viewport->setOpacity(std::clamp(svtonum<double>(val), 0.0, 1.0)); break;

         case SVF_id:
            viewport->setSID(val);
            SetName(viewport, val);
            break;

         case SVF_enable_background: // Deprecated in favour of 'isolated'
            log.warning("enable-background is deprecated in favour of the isolated attribute.");
            if (iequals("new", val)) viewport->setFlags(viewport->Flags | VF::ISOLATED);
            break;

         case SVF_isolation_mode:
            if (iequals("isolated", val)) viewport->setFlags(viewport->Flags | VF::ISOLATED);
            break;

         case SVF_colour_interpolation:
         case SVF_color_interpolation:
            if (iequals("auto", val)) viewport->setColourSpace(VCS::SRGB);
            else if (iequals("sRGB", val)) viewport->setColourSpace(VCS::SRGB);
            else if (iequals("linearRGB", val)) viewport->setColourSpace(VCS::LINEAR_RGB);
            else if (iequals("inherit", val)) viewport->setColourSpace(VCS::INHERIT);
            else log.warning("Invalid color-interpolation value '%s' at line %d", val.c_str(), Tag.LineNo);
            break;

         case SVF_zoomAndPan:
            if (iequals("magnify", val)) {
               // This option indicates that the scene graph should be scaled to match the size of the client's
               // viewing window.
               log.warning("zoomAndPan not yet supported.");
            }
            break;

         case SVF_xmlns: break; // Ignored
         case SVF_baseProfile: break; // The minimum required SVG standard that is required for rendering the document.

         case SVF_mask: {
            OBJECTPTR clip;
            auto ref = uri_name(val);
            if (!Self->Scene->findDef(ref, &clip)) viewport->setMask(clip);
            else log.warning("Unable to find mask '%s'", val.c_str());
            break;
         }

         case SVF_clip_path: {
            OBJECTPTR clip;
            auto ref = uri_name(val);
            if (!Self->Scene->findDef(ref, &clip)) viewport->setMask(clip);
            else log.warning("Unable to find clip-path '%s'", val.c_str());
            break;
         }

         case SVF_xml_space: break; // Element-scoped state is resolved by applyTag().

         default: {
            // Print a warning unless this was a reference to some other namespace.
            if (val.find(':') IS std::string::npos) {
               log.warning("Failed to parse attrib '%s' in <svg/> tag @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
         }
      }
   }

   // Percentages use the nearest viewport's user coordinate width, including a nested viewBox.

   double view_width = 0;
   Unit width;
   viewport->getViewWidth(view_width);
   viewport->getWidth(width);

   const double parent_width = (m_viewport_width > 0) ? m_viewport_width : Scene->PageWidth;
   state.m_viewport_width = (view_width > 0) ? view_width :
      (width.scaled() ? double(width) * parent_width : double(width));

   for (size_t a = 1; a < Tag.Attribs.size(); ++a) {
      if (strhash(Tag.Attribs[a].Name) IS SVF_inline_size) {
         state.resolve_text_property(SVF_inline_size, Tag.Attribs[a].Value, *this);
      }
   }

   // Process child tags

   objVector *sibling = nullptr;
   for (auto &child : Tag.Children) {
      if (child.isTag()) {
         log.traceBranch("Processing <%s/>", child.name());

         switch(svg_tag_hash(child)) {
            case SVF_defs: state.proc_defs(child, viewport); break;
            default:       state.process_tag(child, Tag, viewport, sibling);  break;
         }
      }
   }

   if (viewport->initialised()) Vector = viewport;
   else if (!viewport->init()) Vector = viewport;
   else FreeResource(viewport);
}

//********************************************************************************************************************
// <animateTransform attributeType="XML" attributeName="transform" type="rotate" from="0,150,150" to="360,150,150"
//   begin="0s" dur="5s" repeatCount="indefinite"/>

ERR svgState::proc_animate_transform(XTag &Tag, OBJECTPTR Parent) noexcept
{
   kt::Log log(__FUNCTION__);

   auto &new_anim = Self->Animations.emplace_back(anim_transform { Self, Parent->UID });
   auto &anim = std::get<anim_transform>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strhash(Tag.Attribs[a].Name);
      switch (hash) {
         case SVF_type: // translate, scale, rotate, skewX, skewY
            if (iequals("translate", value))   anim.type = AT::TRANSLATE;
            else if (iequals("scale", value))  anim.type = AT::SCALE;
            else if (iequals("rotate", value)) anim.type = AT::ROTATE;
            else if (iequals("skewX", value))  anim.type = AT::SKEW_X;
            else if (iequals("skewY", value))  anim.type = AT::SKEW_Y;
            else log.warning("Unsupported type '%s'", value.c_str());
            break;

         default:
            set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!anim.is_valid()) {
      remove_animation_triggers(anim);
      Self->Animations.pop_back();
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// The 'animate' element is used to animate a single attribute or property over time. For example, to make a
// rectangle repeatedly fade away over 5 seconds:

ERR svgState::proc_animate(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   kt::Log log(__FUNCTION__);

   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strhash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   anim.set_orig_value(*this);

   if (!anim.is_valid()) {
      remove_animation_triggers(anim);
      Self->Animations.pop_back();
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// <set> is largely equivalent to <animate> but does not interpolate values.

ERR svgState::proc_set(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strhash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   anim.set_orig_value(*this);
   anim.calc_mode = CMODE::DISCRETE; // Disables interpolation
   anim.non_interpolating = true;

   if (!anim.is_valid()) {
      remove_animation_triggers(anim);
      Self->Animations.pop_back();
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// The <animateColour> tag is considered deprecated because its functionality can be represented entirely by the
// existing <animate> tag.

ERR svgState::proc_animate_colour(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strhash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!anim.is_valid()) {
      remove_animation_triggers(anim);
      Self->Animations.pop_back();
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// <animateMotion from="0,0" to="100,100" dur="4s" fill="freeze"/>

ERR svgState::proc_animate_motion(XTag &Tag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_motion { Self, Parent->UID });
   auto &anim = std::get<anim_motion>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strhash(Tag.Attribs[a].Name);
      switch (hash) {
         case SVF_path: // List of standard path commands, e.g. "M 0 0 L 100 100"
            anim.path.set(objVectorPath::create::global(
                fl::Name("motion_path"),
                fl::Owner(Self->Scene->UID),
                fl::Sequence(value),
                fl::Visibility(VIS::HIDDEN)));
            break;

         case SVF_rotate:
            // Post-multiplies a supplemental transformation matrix onto the CTM of the target element to apply a
            // rotation transformation about the origin of the current user coordinate system. The rotation
            // transformation is applied after the supplemental translation transformation that is computed due to
            // the 'path' attribute.
            //
            // auto: The object is rotated over time by the angle of the direction (i.e., directional tangent
            // vector) of the motion path.
            //
            // auto-reverse: Indicates that the object is rotated over time by the angle of the direction (i.e.,
            // directional tangent vector) of the motion path plus 180 degrees.
            //
            // <number>: Indicates that the target element has a constant rotation transformation applied to it,
            // where the rotation angle is the specified number of degrees.

            if (iequals("auto", value)) anim.auto_rotate = ART::AUTO;
            else if (iequals("auto-reverse", value)) anim.auto_rotate = ART::AUTO_REVERSE;
            else {
               anim.auto_rotate = ART::FIXED;
               svtonum(value, anim.rotate);
            }
            break;

         case SVF_origin: break; // Officially serves no purpose.

         default:
            set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!Tag.Children.empty()) {
      // Search for mpath references, e.g. <mpath xlink:href="#mpathRef"/>

      for (auto &child : Tag.Children) {
         if ((child.isTag()) and (svg_tag_hash(child) IS kt::strhash("mpath"))) {
            auto href = child.attrib("xlink:href");
            if (!href) href = child.attrib("href");

            if (href) {
               OBJECTPTR path;
               if (!Self->Scene->findDef(href->c_str(), &path)) {
                  anim.mpath = (objVector *)path;
               }
            }
         }
      }
   }

   if (!anim.is_valid()) {
      remove_animation_triggers(anim);
      Self->Animations.pop_back();
   }
   return ERR::Okay;
}

//********************************************************************************************************************

void svgState::process_attrib(XTag &Tag, objVector *Vector) noexcept
{
   kt::Log log(__FUNCTION__);

   for (int t=1; t < std::ssize(Tag.Attribs); t++) {
      if (Tag.Attribs[t].Value.empty()) continue;
      auto &name = Tag.Attribs[t].Name;
      auto &value = Tag.Attribs[t].Value;

      if (name.find(':') != std::string::npos) continue; // Do not interpret non-SVG attributes, e.g. 'inkscape:dx'
      if (name == "_id") continue; // Ignore temporary private attribs like '_id'

      // Resolved once by applyTag(), including on groups.
      if (text_property_initial(strhash(name)) or (strhash(name) IS SVF_font)) continue;

      log.trace("%s = %.40s", name.c_str(), value.c_str());

      if (auto error = set_property(Vector, strhash(name), Tag, value); error != ERR::Okay) {
         if (Vector->classID() != CLASSID::VECTORGROUP) {
            log.warning("Failed to set field '%s' with '%s' in %s; Error %s",
               name.c_str(), value.c_str(), Vector->Class->ClassName.c_str(), GetErrorMsg(error));
         }
      }
   }
}

//********************************************************************************************************************
// Serialise Katana values without its fixed-size buffers or loss of CSS string quotes.
static std::string css_values(const KatanaArray *Values)
{
   std::string result;
   if (not Values) return result;
   for (unsigned i=0; i < Values->length; i++) {
      auto value = (KatanaValue *)Values->data[i];
      const bool comma = (value->unit IS KATANA_VALUE_PARSER_OPERATOR) and (value->iValue IS ',');
      // The font-family backend expects CSV names without surrounding whitespace.
      if (not result.empty() and not comma and (result.back() != ',')) result.push_back(' ');
      switch (value->unit) {
         case KATANA_VALUE_STRING:
            result.push_back('"');
            for (const char *c = value->string; *c; c++) {
               if ((*c IS '"') or (*c IS '\\')) result.push_back('\\');
               result.push_back(*c);
            }
            result.push_back('"');
            break;
         case KATANA_VALUE_DIMENSION:
         case KATANA_VALUE_UNICODE_RANGE:
         case KATANA_VALUE_IDENT: result.append(value->string); break;
         case KATANA_VALUE_PARSER_OPERATOR: result.push_back(char(value->iValue)); break;
         case KATANA_VALUE_PARSER_FUNCTION:
            result.append(value->function->name).append(css_values(value->function->args)).push_back(')');
            break;
         case KATANA_VALUE_PARSER_LIST: result.append(css_values(value->list)); break;
         case KATANA_VALUE_PARSER_HEXCOLOR: result.append("#").append(value->string); break;
         case KATANA_VALUE_URI: result.append("url(").append(value->string).push_back(')'); break;
         case KATANA_VALUE_NUMBER:
         case KATANA_VALUE_PERCENTAGE:
         case KATANA_VALUE_EMS:
         case KATANA_VALUE_EXS:
         case KATANA_VALUE_REMS:
         case KATANA_VALUE_CHS:
         case KATANA_VALUE_PX:
         case KATANA_VALUE_CM:
         case KATANA_VALUE_MM:
         case KATANA_VALUE_IN:
         case KATANA_VALUE_PT:
         case KATANA_VALUE_PC:
         case KATANA_VALUE_DEG:
         case KATANA_VALUE_RAD:
         case KATANA_VALUE_GRAD:
         case KATANA_VALUE_MS:
         case KATANA_VALUE_S:
         case KATANA_VALUE_HZ:
         case KATANA_VALUE_KHZ:
         case KATANA_VALUE_TURN:
         case KATANA_VALUE_DPPX:
         case KATANA_VALUE_DPI:
         case KATANA_VALUE_DPCM:
         case KATANA_VALUE_VW:
         case KATANA_VALUE_VH:
         case KATANA_VALUE_VMIN:
         case KATANA_VALUE_VMAX:
         case KATANA_VALUE_FR:
            result.append(value->raw);
            break;
         default: return "!unsupported";
      }
   }
   return result;
}

//********************************************************************************************************************
// Only simple/compound tag, universal, ID and class selectors are supported.  Reject an entire selector containing
// combinators or unsupported components, rather than applying just its rightmost component.

static bool css_matches(const KatanaSelector *Selector, const XTag &Tag, size_t &Specificity)
{
   Specificity = 0;
   for (auto sel = Selector; sel; sel = sel->tagHistory) {
      if (sel->tagHistory and (sel->relation != KatanaSelectorRelationSubSelector)) return false;
      switch (sel->match) {
         case KatanaSelectorMatchTag:
            if (not sel->tag or sel->tag->prefix) return false; // Namespaced selectors are outside this subset.
            if (std::string_view(sel->tag->local) != "*") {
               if (std::string_view(sel->tag->local) != svg_local_name(Tag)) return false;
               Specificity++;
            }
            break;
         case KatanaSelectorMatchId: {
            const auto id = Tag.attrib("id");
            if (not id or not sel->data or *id != sel->data->value) return false;
            Specificity += 0x10000;
            break;
         }
         case KatanaSelectorMatchClass: {
            const auto classes = Tag.attrib("class");
            if (not classes or not sel->data) return false;
            bool found = false;
            std::string_view list(*classes);
            while (not list.empty()) {
               list = kt::font_traits::trim(list);
               auto end = list.find_first_of(" \t\r\n");
               if (list.substr(0, end) IS sel->data->value) found = true;
               if (end IS std::string_view::npos) break;
               list.remove_prefix(end + 1);
            }
            if (not found) return false;
            Specificity += 0x100;
            break;
         }
         default: return false;
      }
   }
   return true;
}

struct CSSDeclaration {
   std::string name;
   std::string value;
   size_t specificity;
   bool important;
};

//********************************************************************************************************************

static void collect_styles(objXML::TAGS &Tags, std::vector<KatanaOutput *> &Sheets)
{
   for (auto &tag : Tags) {
      if (svg_tag_is(tag, SVF_style)) {
         const auto type = tag.attrib("type");
         if (not type or iequals(*type, "text/css")) {
            auto content = tag.getContent();
            if (auto sheet = katana_parse(content.data(), content.size(), KatanaParserModeStylesheet)) {
               Sheets.push_back(sheet);
            }
         }
      }
      collect_styles(tag.Children, Sheets);
   }
}

//********************************************************************************************************************

static void cascade_styles(objXML::TAGS &Tags, const std::vector<KatanaOutput *> &Sheets)
{
   for (auto &tag : Tags) {
      if (not tag.isTag()) continue;
      std::vector<CSSDeclaration> declarations;
      auto append = [&](KatanaArray *Properties, size_t Specificity) {
         for (unsigned i=0; i < Properties->length; i++) {
            auto prop = (KatanaDeclaration *)Properties->data[i];
            declarations.push_back({prop->property, css_values(prop->values), Specificity, prop->important});
         }
      };

      for (auto sheet : Sheets) {
         for (unsigned i=0; i < sheet->stylesheet->rules.length; i++) {
            auto rule = (KatanaRule *)sheet->stylesheet->rules.data[i];
            if (rule->type != KatanaRuleStyle) continue;
            auto style = (KatanaStyleRule *)rule;
            bool matched = false;
            size_t specificity = 0;
            for (unsigned j=0; j < style->selectors->length; j++) {
               size_t score;
               if (css_matches((KatanaSelector *)style->selectors->data[j], tag, score)) {
                  matched = true;
                  specificity = std::max(specificity, score);
               }
            }
            if (matched) append(style->declarations, specificity);
         }
      }

      for (size_t i=1; i < tag.Attribs.size(); ) {
         if (not iequals(tag.Attribs[i].Name, "style")) { i++; continue; }
         auto value = std::move(tag.Attribs[i].Value);
         tag.Attribs.erase(tag.Attribs.begin() + i);
         if (auto css = katana_parse(value.data(), value.size(), KatanaParserModeDeclarationList)) {
            append(css->declarations, 0x1000000); // Inline author specificity exceeds every supported selector.
            katana_destroy_output(css);
         }
      }

      std::stable_sort(declarations.begin(), declarations.end(), [](const auto &Left, const auto &Right) {
         if (Left.important != Right.important) return not Left.important;
         return Left.specificity < Right.specificity;
      });

      // Keep lower-priority declarations: an invalid later value must leave the earlier applicable value intact.
      // Presentation attributes precede all author declarations, independent of their XML attribute order.
      for (const auto &declaration : declarations) tag.Attribs.emplace_back(declaration.name, declaration.value);
      cascade_styles(tag.Children, Sheets);
   }
}

//********************************************************************************************************************

static void prepare_styles(objXML::TAGS &Tags)
{
   std::vector<KatanaOutput *> sheets;
   collect_styles(Tags, sheets);
   cascade_styles(Tags, sheets);
   for (auto sheet : sheets) katana_destroy_output(sheet);
}

//********************************************************************************************************************

ERR svgState::set_property(objVector *Vector, uint32_t Hash, XTag &Tag, const std::string StrValue) noexcept
{
   kt::Log log(__FUNCTION__);

   // Ignore stylesheet attributes, and content-value which is applied by process_tag()
   if ((Hash IS SVF_class) or (Hash IS SVF_content_value)) return ERR::Okay;

   switch(Vector->classID()) {
      case CLASSID::VECTORVIEWPORT: {
         auto vp = (objVectorViewport *)Vector;
         switch (Hash) {
            // The following 'view-*' fields are for defining the SVG view box
            case SVF_viewX:      vp->setViewX(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_viewY:      vp->setViewY(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_viewWidth:  vp->setViewWidth(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_viewHeight: vp->setViewHeight(svtonum<double>(StrValue)); return ERR::Okay;
            // The following dimension fields are for defining the position and clipping of the vector display
            case SVF_x:      vp->setX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_y:      vp->setY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_width:  vp->setWidth(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_height: vp->setHeight(SVGUnit(StrValue)); return ERR::Okay;
         }
         break;
      }

      case CLASSID::VECTORELLIPSE: {
         auto ellipse = (objVectorEllipse *)Vector;
         switch (Hash) {
            case SVF_cx:       ellipse->setCX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_cy:       ellipse->setCY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_r:        ellipse->setRadius(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_rx:       ellipse->setRadiusX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_ry:       ellipse->setRadiusY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_vertices: ellipse->setVertices(SVGUnit(StrValue)); return ERR::Okay;
         }
         break;
      }

      case CLASSID::VECTORWAVE: {
         auto wave = (objVectorWave *)Vector;
         switch (Hash) {
            case SVF_x:         wave->setX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_y:         wave->setY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_length:    wave->setLength(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_close:
               if (iequals("top", StrValue) or iequals("true", StrValue) or (StrValue IS "1")) {
                  wave->setClose(WVC::TOP);
               }
               else if (iequals("bottom", StrValue)) wave->setClose(WVC::BOTTOM);
               else wave->setClose(WVC::NIL);
               return ERR::Okay;
            case SVF_amplitude: wave->setAmplitude(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_decay:     wave->setDecay(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_envelope:
               if (iequals("quadratic", StrValue)) wave->setEnvelope(WVE::QUADRATIC);
               else if (iequals("smoothstep", StrValue)) wave->setEnvelope(WVE::SMOOTHSTEP);
               else if (iequals("exponential", StrValue)) wave->setEnvelope(WVE::EXPONENTIAL);
               else wave->setEnvelope(WVE::LINEAR);
               return ERR::Okay;
            case SVF_frequency:     wave->setFrequency(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_frequency_end:
            case SVF_frequencyEnd:  wave->setFrequencyEnd(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_noise:         wave->setNoise(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_phase:         wave->setPhase(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_thickness:     wave->setThickness(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_type:
               if (iequals("triangle", StrValue)) wave->setType(WVT::TRIANGLE);
               else if (iequals("sawtooth", StrValue)) wave->setType(WVT::SAWTOOTH);
               else if (iequals("square", StrValue)) wave->setType(WVT::SQUARE);
               else wave->setType(WVT::SINE);
               return ERR::Okay;
         }
         break;
      }

      case CLASSID::VECTORRECTANGLE: {
         auto rect = (objVectorRectangle *)Vector;
         switch (Hash) {
            case SVF_x1:
            case SVF_x:      rect->setX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_y1:
            case SVF_y:      rect->setY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_width:  rect->setWidth(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_height: rect->setHeight(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_rx:     rect->setRoundX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_ry:     rect->setRoundY(SVGUnit(StrValue)); return ERR::Okay;

            case SVF_xOffset: rect->setXOffset(SVGUnit(StrValue)); return ERR::Okay; // Kotuku only
            case SVF_yOffset: rect->setYOffset(SVGUnit(StrValue)); return ERR::Okay; // Kotuku only

            case SVF_x2: {
               // Note: For the time being, VectorRectangle doesn't support X2/Y2 as a concept.  This would
               // cause problems if the client was to specify a scaled value here.
               auto width = SVGUnit(StrValue);
               Unit x(0);
               rect->getX(x);
               rect->setWidth(std::abs(width - x));
               return ERR::Okay;
            }

            case SVF_y2: {
               auto height = SVGUnit(StrValue);
               Unit y(0);
               rect->getY(y);
               rect->setHeight(std::abs(height - y));
               return ERR::Okay;
            }
         }
         break;
      }

      // VectorPolygon handles polygon, polyline and line.
      case CLASSID::VECTORPOLYGON: {
         auto polygon = (objVectorPolygon *)Vector;
         switch (Hash) {
            case SVF_points: polygon->setPoints(StrValue); return ERR::Okay;
            case SVF_x1: polygon->setX1(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_y1: polygon->setY1(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_x2: polygon->setX2(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_y2: polygon->setY2(SVGUnit(StrValue)); return ERR::Okay;
         }
         break;
      }

      case CLASSID::VECTORTEXT: {
         auto vt = (objVectorText *)Vector;
         switch (Hash) {
            case SVF_x: return vt->setX(SVGUnit(StrValue));
            case SVF_y: return vt->setY(SVGUnit(StrValue));

            case SVF_dx: {
               auto values = read_array(StrValue);
               if (auto error = vt->setDX(values); error != ERR::Okay) return error;
               return ERR::NoSupport; // Stored for compatibility; not consumed by the renderer.
            }

            case SVF_dy: {
               auto values = read_array(StrValue);
               if (auto error = vt->setDY(values); error != ERR::Okay) return error;
               return ERR::NoSupport; // Stored for compatibility; not consumed by the renderer.
            }

            case SVF_lengthAdjust: // Can be set to either 'spacing' or 'spacingAndGlyphs'
               //if (iequals("spacingAndGlyphs", va_arg(list, STRING))) vt->VT.SpacingAndGlyphs = TRUE;
               //else vt->VT.SpacingAndGlyphs = FALSE;
               return ERR::NoSupport;

            case SVF_font_size_adjust: return ERR::NoSupport;

            case SVF_rotate: {
               // SVG defines rotate as a comma or whitespace separated list of <number> values.
               auto values = read_array(StrValue);
               if (auto error = vt->setRotate(values); error != ERR::Okay) return error;
               return ERR::NoSupport; // Stored for compatibility; not consumed by the renderer.
            }
            case SVF_string: return vt->setString(StrValue);

            case SVF_textLength:
               if (auto error = vt->setTextLength(SVGUnit(StrValue)); error != ERR::Okay) return error;
               return ERR::NoSupport; // Stored for compatibility; length adjustment is not implemented.
            // TextPath only
            //case SVF_startOffset: vt->set(FID_StartOffset, StrValue); return ERR::Okay;
            //case SVF_method:      // The default is align.  For 'stretch' mode, set VMF::STRETCH in GuideFlags
            //                      vt->set(FID_GuideFlags, StrValue); return ERR::Okay;
            //case SVF_spacing:     vt->set(FID_Spacing, StrValue); return ERR::Okay;
            //case SVF_xlink_href:  // Used for drawing text along a path.
            //   return ERR::Okay;

            //case SVF_pathLength:     vt->set(FID_PathLength, StrValue); return ERR::Okay;

         }
         break;
      }

      case CLASSID::VECTORSPIRAL: {
         auto spiral = (objVectorSpiral *)Vector;
         switch (Hash) {
            case SVF_pathLength: spiral->setPathLength(svtonum<int>(StrValue)); return ERR::Okay;
            case SVF_cx:         spiral->setCX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_cy:         spiral->setCY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_radius:
            case SVF_r:          spiral->setRadius(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_offset:     spiral->setOffset(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_step:       spiral->setStep(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_spacing:    spiral->setSpacing(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_decay:      spiral->setDecay(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_startTurn:  spiral->setStartTurn(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_endTurn:    spiral->setEndTurn(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_tolerance:  spiral->setTolerance(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_thickness:  spiral->setThickness(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_clockwise:  spiral->setClockwise(iequals("true", StrValue) or iequals("1", StrValue)); return ERR::Okay;
            case SVF_reverse:    spiral->setReverse(iequals("true", StrValue) or iequals("1", StrValue)); return ERR::Okay;
            case SVF_loop_limit: // Deprecated dash-style
            case SVF_loopLimit:  spiral->setLoopLimit(SVGUnit(StrValue)); return ERR::Okay;
         }
         break;
      }

      case CLASSID::VECTORSHAPE: {
         auto shape = (objVectorShape *)Vector;
         switch (Hash) {
            case SVF_cx:       shape->setCX(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_cy:       shape->setCY(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_r:        shape->setRadius(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_n1:       shape->setN1(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_n2:       shape->setN2(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_n3:       shape->setN3(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_m:        shape->setM(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_a:        shape->setA(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_b:        shape->setB(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_phi:      shape->setPhi(SVGUnit(StrValue)); return ERR::Okay;
            case SVF_offset:   shape->setOffset(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_tolerance:  shape->setTolerance(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_startAngle: shape->setStartAngle(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_endAngle:   shape->setEndAngle(svtonum<double>(StrValue)); return ERR::Okay;
            case SVF_normalise:
               shape->setNormalise(iequals("true", StrValue) or iequals("1", StrValue));
               return ERR::Okay;
            case SVF_vertices: shape->setVertices(int(SVGUnit(StrValue))); return ERR::Okay;
            case SVF_mod:      shape->setMod(int(SVGUnit(StrValue))); return ERR::Okay;
            case SVF_spiral:   shape->setSpiral(int(SVGUnit(StrValue))); return ERR::Okay;
            case SVF_repeat:   shape->setRepeat(int(SVGUnit(StrValue))); return ERR::Okay;
            case SVF_close:
               if ((iequals("true", StrValue)) or (iequals("1", StrValue))) shape->setClose(TRUE);
               else shape->setClose(FALSE);
               break;
         }
         break;
      }

      case CLASSID::VECTORPATH:
         switch (Hash) {
            case SVF_d: ((objVectorPath *)Vector)->setSequence(StrValue); return ERR::Okay;
            case SVF_pathLength: ((objVectorPath *)Vector)->setPathLength(svtonum<int>(StrValue)); return ERR::Okay;
         }
         break;

      default: break;
   }

   // Fall-through to generic attributes.

   switch (Hash) {
      case SVF_append_path: // Deprecated dash-style
      case SVF_appendPath: {
         // The appendPath option is a Kotuku attribute that requires a reference to an instantiated vector with a path.
         OBJECTPTR other = nullptr;
         if (!Self->Scene->findDef(StrValue, &other)) Vector->setAppendPath(other);
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_join_path: // Deprecated dash-style
      case SVF_joinPath: {
         // The joinPath option is a Kotuku attribute that requires a reference to an instantiated vector with a path.
         OBJECTPTR other = nullptr;
         if (!Self->Scene->findDef(StrValue, &other)) {
            Vector->setAppendPath(other);
            Vector->setFlags(VF::JOIN_PATHS|Vector->Flags);
         }
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_transition: {
         OBJECTPTR trans = nullptr;
         if (!Self->Scene->findDef(StrValue, &trans)) Vector->setTransition(trans);
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_colour_interpolation:
      case SVF_color_interpolation:
         if (iequals("auto", StrValue)) Vector->setColourSpace(VCS::SRGB);
         else if (iequals("sRGB", StrValue)) Vector->setColourSpace(VCS::SRGB);
         else if (iequals("linearRGB", StrValue)) Vector->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("inherit", StrValue)) Vector->setColourSpace(VCS::INHERIT);
         else log.warning("Invalid color-interpolation value '%s' at line %d", StrValue.c_str(), Tag.LineNo);
         break;

      case SVF_stroke_linejoin:
         switch(strhash(StrValue)) {
            case SVF_miter:       Vector->setLineJoin(VLJ::MITER); break;
            case SVF_round:       Vector->setLineJoin(VLJ::ROUND); break;
            case SVF_bevel:       Vector->setLineJoin(VLJ::BEVEL); break;
            case SVF_inherit:     Vector->setLineJoin(VLJ::INHERIT); break;
            case SVF_miter_clip:  Vector->setLineJoin(VLJ::MITER_SMART); break; // Special AGG only join type
            case SVF_miter_round: Vector->setLineJoin(VLJ::MITER_ROUND); break; // Special AGG only join type
         }
         break;

      case SVF_stroke_innerjoin: // AGG ONLY
         switch(strhash(StrValue)) {
            case SVF_miter:   Vector->setInnerJoin(VIJ::MITER);  break;
            case SVF_round:   Vector->setInnerJoin(VIJ::ROUND); break;
            case SVF_bevel:   Vector->setInnerJoin(VIJ::BEVEL); break;
            case SVF_inherit: Vector->setInnerJoin(VIJ::INHERIT); break;
            case SVF_jag:     Vector->setInnerJoin(VIJ::JAG); break;
         }
         break;

      case SVF_stroke_linecap:
         switch(strhash(StrValue)) {
            case SVF_butt:    Vector->setLineCap(VLC::BUTT); break;
            case SVF_square:  Vector->setLineCap(VLC::SQUARE); break;
            case SVF_round:   Vector->setLineCap(VLC::ROUND); break;
            case SVF_inherit: Vector->setLineCap(VLC::INHERIT); break;
         }
         break;

      case SVF_visibility:
         if (iequals("visible", StrValue))       Vector->setVisibility(VIS::VISIBLE);
         else if (iequals("hidden", StrValue))   Vector->setVisibility(VIS::HIDDEN);
         else if (iequals("collapse", StrValue)) Vector->setVisibility(VIS::COLLAPSE); // Same effect as hidden, kept for SVG compatibility
         else if (iequals("inherit", StrValue))  Vector->setVisibility(VIS::INHERIT);
         else log.warning("Unsupported visibility value '%s'", StrValue.c_str());
         break;

      case SVF_fill_rule:
         if (iequals("nonzero", StrValue))      Vector->setFillRule(VFR::NON_ZERO);
         else if (iequals("evenodd", StrValue)) Vector->setFillRule(VFR::EVEN_ODD);
         else if (iequals("inherit", StrValue)) Vector->setFillRule(VFR::INHERIT);
         else log.warning("Unsupported fill-rule value '%s'", StrValue.c_str());
         break;

      case SVF_clip_rule:
         if (iequals("nonzero", StrValue)) Vector->setClipRule(VFR::NON_ZERO);
         else if (iequals("evenodd", StrValue)) Vector->setClipRule(VFR::EVEN_ODD);
         else if (iequals("inherit", StrValue)) Vector->setClipRule(VFR::INHERIT);
         else log.warning("Unsupported clip-rule value '%s'", StrValue.c_str());
         break;

      case SVF_enable_background: // Deprecated in favour of 'isolated'
         log.warning("enable-background is deprecated in favour of the isolated attribute.");
         if (iequals("new", StrValue)) Vector->setFlags(Vector->Flags | VF::ISOLATED);
         break;

      case SVF_isolation_mode:
         if (iequals("isolated", StrValue)) Vector->setFlags(Vector->Flags | VF::ISOLATED);
         break;

      case SVF_id:
         if (!Self->Cloning) {
            Vector->setSID(StrValue);
            Self->Scene->addDef(StrValue, Vector);
            SetName(Vector, StrValue);
         }
         break;

      case SVF_display:
         // The difference between 'display=none' and 'visibility=hidden' is that visibilility holds its
         // whitespace in document layout mode.  This has no relevance in our Vector Scene Graph, so 'display' is
         // treated as an obsolete feature and converted to visibility.

         if (iequals("none", StrValue))          Vector->setVisibility(VIS::HIDDEN);
         else if (iequals("inline", StrValue))   Vector->setVisibility(VIS::VISIBLE);
         else if (iequals("inherit", StrValue))  Vector->setVisibility(VIS::INHERIT);
         break;

      case SVF_overflow: // visible | hidden | scroll | auto | inherit
         log.trace("overflow is not supported.");
         break;

      case SVF_marker:       log.warning("marker is not supported."); break;
      case SVF_marker_end:   log.warning("marker-end is not supported."); break;
      case SVF_marker_mid:   log.warning("marker-mid is not supported."); break;
      case SVF_marker_start: log.warning("marker-start is not supported."); break;

      case SVF_filter:       Vector->setFilter(StrValue); break;

      case SVF_color:
         // Handled by svgState::applyTag() for inherited currentColor.
         break;

      case SVF_stroke:
         if (iequals("currentColor", StrValue)) {
            FRGB rgb;
            if (current_colour(Vector, rgb)) Vector->setStrokeColour(rgb);
         }
         else Vector->setStroke(resolve_paint_server(StrValue));
         break;

      case SVF_fill:
         if (iequals("currentColor", StrValue)) {
            FRGB rgb;
            if (current_colour(Vector, rgb)) Vector->setFillColour(rgb);
         }
         else Vector->setFill(resolve_paint_server(StrValue));
         break;

      case SVF_transform: parse_transform(Vector, StrValue, MTAG_SVG_TRANSFORM); break;

      case SVF_stroke_dasharray: {
         auto values = read_array(StrValue);
         Vector->setDashArray(values);
         break;
      }

      case SVF_opacity:          Vector->setOpacity(std::clamp(svtonum<double>(StrValue), 0.0, 1.0)); break;
      case SVF_fill_opacity:     Vector->setFillOpacity(std::clamp(svtonum<double>(StrValue), 0.0, 1.0)); break;
      case SVF_shape_rendering:  Vector->setPathQuality(shape_rendering_to_render_quality(StrValue)); break;

      case SVF_stroke_width:            Vector->setStrokeWidth(SVGUnit(StrValue)); break;
      case SVF_stroke_opacity:          Vector->setStrokeOpacity(std::clamp(svtonum<double>(StrValue), 0.0, 1.0)); break;
      case SVF_stroke_miterlimit:       Vector->setMiterLimit(svtonum<double>(StrValue)); break;
      //case SVF_stroke_miterlimit_theta: Vector->setMiterLimitTheta(svtonum<double>(StrValue)); break;
      //case SVF_stroke_inner_miterlimit: Vector->setInnerMiterLimit(svtonum<double>(StrValue)); break;
      case SVF_stroke_dashoffset:       Vector->setDashOffset(SVGUnit(StrValue)); break;

      case SVF_mask: {
         OBJECTPTR clip;
         auto ref = uri_name(StrValue);
         if (!Self->Scene->findDef(ref, &clip)) {
            Vector->setMask(clip);
         }
         else {
            log.warning("Unable to find mask '%s'", StrValue.c_str());
            return ERR::Search;
         }
         break;
      }

      case SVF_clip_path: {
         OBJECTPTR clip;
         auto ref = uri_name(StrValue);
         if (!Self->Scene->findDef(ref, &clip)) {
            Vector->setMask(clip);
         }
         else {
            log.warning("Unable to find clip-path '%s'", StrValue.c_str());
            return ERR::Search;
         }
         break;
      }

      default: return ERR::UnsupportedField;
   }

   return ERR::Okay;
}
