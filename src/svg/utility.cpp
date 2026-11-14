
[[maybe_unused]] static HSV rgb_to_hsl(FRGB Colour);
[[maybe_unused]] static FRGB hsl_to_rgb(HSV Colour);

#if defined(DEBUG)
[[maybe_unused]] static void debug_branch(CSTRING Header, OBJECTPTR Vector, int &Level)
{
   kt::Log log(Header);

   auto spacing = std::string(Level + 1, ' ');
   Level++;

   while (Vector) {
      if (Vector->classID() IS CLASSID::VECTORSCENE) {
         log.msg("Scene: %p", Vector);
         if (((objVectorScene *)Vector)->Viewport) debug_branch(Header, ((objVectorScene *)Vector)->Viewport, Level);
      }
      else if (Vector->Class->BaseClassID IS CLASSID::VECTOR) {
         objVector *shape = (objVector *)Vector;
         log.msg("%p<-%p->%p Child %p %s%s", shape->Prev, shape, shape->Next, shape->Child, spacing.c_str(), shape->className());
         if (shape->Child) debug_branch(Header, shape->Child, Level);
         Vector = shape->Next;
      }
      else break;
   }

   Level--;
}

static void debug_tree(CSTRING Header, OBJECTPTR Vector)
{
   int level = 0;
   while (Vector) {
      debug_branch(Header, Vector, level);
      if (Vector->Class->BaseClassID IS CLASSID::VECTOR) {
         Vector = (((objVector *)Vector)->Next);
      }
      else break;
   }
}

#endif

//********************************************************************************************************************
// HSV values are from 0 - 1.0

static HSV rgb_to_hsl(FRGB Colour)
{
   double vmax = std::ranges::max({ Colour.Red, Colour.Green, Colour.Blue });
   double vmin = std::ranges::min({ Colour.Red, Colour.Green, Colour.Blue });
   double light = (vmax + vmin) * 0.5;

   if (vmax IS vmin) return HSV { 0, 0, light };

   double sat = light, hue = light;
   double d = vmax - vmin;
   sat = light > 0.5 ? d / (2 - vmax - vmin) : d / (vmax + vmin);
   if (vmax IS Colour.Red)   hue = (Colour.Green - Colour.Blue) / d + (Colour.Green < Colour.Blue ? 6.0 : 0.0);
   if (vmax IS Colour.Green) hue = (Colour.Blue  - Colour.Red) / d + 2.0;
   if (vmax IS Colour.Blue)  hue = (Colour.Red   - Colour.Green) / d + 4.0;
   hue /= 6.0;

   return HSV { hue, sat, light, Colour.Alpha };
}

//********************************************************************************************************************
// HSV values are from 0 - 1.0

static FRGB hsl_to_rgb(HSV Colour)
{
   auto hueToRgb = [](float p, float q, float t) -> float {
      if (t < 0) t++;
      if (t > 1) t--;
      if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
      if (t < 1.0/2.0) return q;
      if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
      return p;
   };

   if (Colour.Saturation IS 0) {
      return { float(Colour.Value), float(Colour.Value), float(Colour.Value), float(Colour.Alpha) };
   }
   else {
      const double q = (Colour.Value < 0.5) ? Colour.Value * (1.0 + Colour.Saturation) : Colour.Value + Colour.Saturation - Colour.Value * Colour.Saturation;
      const double p = 2.0 * Colour.Value - q;
      return {
         hueToRgb(p, q, Colour.Hue + 1.0/3.0),
         hueToRgb(p, q, Colour.Hue),
         hueToRgb(p, q, Colour.Hue - 1.0/3.0),
         float(Colour.Alpha)
      };
   }
}

//********************************************************************************************************************
// Support for the 'currentColor' colour value.  Finds the first parent with a defined fill colour and returns it.

bool svgState::current_colour(objVector *Vector, FRGB &RGB) noexcept
{
   if (not m_color.empty()) {
      VectorPainter painter;
      if (!vec::ReadPainter(nullptr, m_color, &painter, nullptr)) {
         RGB = painter.Colour;
         return true;
      }
   }

   if (Vector->Class->BaseClassID != CLASSID::VECTOR) return false;

   Vector = (objVector *)Vector->Parent;
   while (Vector) {
      if (Vector->Class->BaseClassID != CLASSID::VECTOR) return false;

      FRGB *rgb;
      if (!Vector->getFillColour(rgb)) {
         if (rgb->Alpha) {
            RGB = *rgb;
            return true;
         }
         else return false;
      }
      Vector = (objVector *)Vector->Parent;
   }

   return false;
}

//********************************************************************************************************************

static void parse_result(extSVG *Self, objFilterEffect *Effect, std::string Value)
{
   if (not Self->Effects.contains(Value)) {
      Self->Effects.emplace(Value, Effect);
   }
}

//********************************************************************************************************************

static void parse_input_source(extSVG *Self, objFilterEffect *Effect, const std::string Input)
{
   switch (strhash(Input)) {
      case SVF_SourceGraphic:   Effect->setSourceType(VSF::GRAPHIC); break;
      case SVF_SourceAlpha:     Effect->setSourceType(VSF::ALPHA); break;
      case SVF_BackgroundImage: Effect->setSourceType(VSF::BKGD); break;
      case SVF_BackgroundAlpha: Effect->setSourceType(VSF::BKGD_ALPHA); break;
      case SVF_FillPaint:       Effect->setSourceType(VSF::FILL); break;
      case SVF_StrokePaint:     Effect->setSourceType(VSF::STROKE); break;
      default:  {
         if (Self->Effects.contains(Input)) Effect->setInput(Self->Effects[Input]);
         else kt::Log().warning("Unrecognised input '%s'", Input.c_str());
         break;
      }
   }
}

static void parse_input_mix(extSVG *Self, objFilterEffect *Effect, const std::string Input)
{
   switch (strhash(Input)) {
      case SVF_SourceGraphic:   Effect->setMixType(VSF::GRAPHIC); break;
      case SVF_SourceAlpha:     Effect->setMixType(VSF::ALPHA); break;
      case SVF_BackgroundImage: Effect->setMixType(VSF::BKGD); break;
      case SVF_BackgroundAlpha: Effect->setMixType(VSF::BKGD_ALPHA); break;
      case SVF_FillPaint:       Effect->setMixType(VSF::FILL); break;
      case SVF_StrokePaint:     Effect->setMixType(VSF::STROKE); break;
      default:  {
         if (Self->Effects.contains(Input)) Effect->setMix(Self->Effects[Input]);
         else kt::Log().warning("Unrecognised input '%s'", Input.c_str());
         break;
      }
   }
}

//********************************************************************************************************************
// Note that all offsets are percentages.

static std::vector<Transition> process_transition_stops(extSVG *Self, const objXML::TAGS &Tags)
{
   kt::Log log("process_stops");

   log.traceBranch();

   std::vector<Transition> stops;
   for (auto &scan : Tags) {
      if (svg_tag_hash(scan) IS kt::strhash("stop")) {
         Transition stop;
         stop.Offset = 0;
         for (unsigned a=1; a < scan.Attribs.size(); a++) {
            auto &name = scan.Attribs[a].Name;
            auto &value = scan.Attribs[a].Value;
            if (value.empty()) continue;

            if (iequals("offset", name)) {
               char *end;
               stop.Offset = strtod(value.c_str(), &end);
               if (*end IS '%') {
                  stop.Offset = stop.Offset * 0.01; // Must be in the range of 0 - 1.0
               }

               if (stop.Offset < 0.0) stop.Offset = 0;
               else if (stop.Offset > 1.0) stop.Offset = 1.0;
            }
            else if (iequals("transform", name)) {
               stop.Transform = value;
            }
            else log.warning("Unable to process stop attribute '%s'", name.c_str());
         }
         stops.emplace_back(stop);
      }
      else log.warning("Unknown element in transition, '%s'", scan.name());
   }

   return stops;
}

//********************************************************************************************************************

static std::string_view folder(extSVG *Self)
{
   if (not Self->Folder.empty()) return Self->Folder;
   if (Self->Path.empty()) return std::string_view{};

   // Setting a path of "my/house/is/red.svg" results in "my/house/is/".  A query string is excluded because its
   // values may contain separators.

   std::string_view path(Self->Path), query;
   split_svg_query(Self->Path, path, query);

   if (!ResolvePath(path, RSF::NO_FILE_CHECK, &Self->Folder)) {
      if (auto last = Self->Folder.find_last_of("/\\"); last != std::string::npos) {
         Self->Folder.resize(last + 1);
         return Self->Folder;
      }
      else Self->Folder.clear();
   }
   return std::string_view{};
}

//********************************************************************************************************************

static void parse_transform(objVector *Vector, std::string_view Value, int Tag)
{
   if ((Vector->Class->BaseClassID IS CLASSID::VECTOR) and (not Value.empty())) {
      VectorMatrix *matrix;
      if (!Vector->newMatrix(&matrix, false)) {
         vec::ParseTransform(matrix, Value);
         matrix->Tag = Tag;
      }
      else kt::Log(__FUNCTION__).warning("Failed to create vector transform matrix.");
   }
}

//********************************************************************************************************************

static std::string_view uri_name(const std::string_view Ref)
{
   std::size_t skip = 0;
   while ((skip < Ref.size()) and (Ref[skip] <= 0x20)) skip++;
   if (skip >= Ref.size()) return {};

   if (Ref[skip] IS '#') {
      return Ref.substr(skip + 1);
   }
   else if (Ref.substr(skip).starts_with("url(#")) {
      std::size_t i;
      skip += 5;
      for (i=0; ((skip + i) < Ref.size()) and (Ref[skip+i] != ')'); i++);
      return Ref.substr(skip, i);
   }
   else return Ref.substr(skip);
}

//********************************************************************************************************************

static XTag * find_href_tag(extSVG *Self, std::string_view Ref)
{
   auto ref = uri_name(Ref);
   if (not ref.empty()) {
      if (auto tag = Self->IDs.find(ref); tag != Self->IDs.end()) {
         return tag->second;
      }
   }
   return nullptr;
}

/*********************************************************************************************************************
** Converts a SVG time string into seconds.
**
** Full clock example:  50:00:10.25 = 50 hours, 10 seconds and 250 milliseconds
** Partial clock value: 00:10.5 = 10.5 seconds = 10 seconds and 500 milliseconds
** Time count values:
**  3.2h    = 3.2 hours = 3 hours and 12 minutes
**  45min   = 45 minutes
**  30s     = 30 seconds
**  5ms     = 5 milliseconds
**  12.467  = 12
*/

static double read_time(const std::string_view Value)
{
   std::array<double, 3> units;

   std::size_t i = 0;
   while ((i < Value.size()) and (unsigned(Value[i]) <= 0x20)) i++;

   double num;
   auto [ v, error ] = std::from_chars(Value.data() + i, Value.data() + Value.size(), num);

   if (error IS std::errc()) {
      units[0] = num;

      if (v[0] IS ':') {
         v++;
         units[1] = strtod(v, (char **)&v);

         if (v[0] IS ':') {
            v++;
            units[2] = strtod(v, (char **)&v);

            // hh:nn:ss
            return (units[0] * 60 * 60) + (units[1] * 60) + units[2];
         }
         else { // hh:nn
            return (units[0] * 60 * 60) + (units[1] * 60);
         }
      }
      else if (Value.ends_with("h")) return units[0] * 60 * 60;
      else if (Value.ends_with("min")) return units[0] * 60;
      else if (Value.ends_with("ms")) return double(units[0]) / 1000.0;
      else if (Value.ends_with("s")) return units[0];
      else return units[0];
   }
   else return 0;
}

//********************************************************************************************************************
// Designed for reading unit values such as '50%' and '6px'.  The returned value is scaled to pixels.

static double read_unit(std::string_view &Value)
{
   const double dpi = 96.0; // TODO: Needs to be derived from the display

   std::size_t i = 0;
   while ((i < Value.size()) and (unsigned(Value[i]) <= 0x20)) i++;
   if (i > 0) Value.remove_prefix(i);

   double fv;
   auto [ ptr, error ] = std::from_chars(Value.data(), Value.data() + Value.size(), fv);

   if (error IS std::errc()) {
      Value.remove_prefix(ptr - Value.data());

      if (Value.starts_with('%')) {
         Value.remove_prefix(1);
         return fv * 0.01;
      }
      else if (Value.starts_with("em")) { Value.remove_prefix(2); return fv * 12.0 * (4.0 / 3.0); } // Multiply the current font's pixel height by the provided em value
      else if (Value.starts_with("ex")) { Value.remove_prefix(2); return fv * 6.0 * (4.0 / 3.0); } // As for em, but multiple by the pixel height of the 'x' character.  If no x character, revert to 0.5em
      else if (Value.starts_with("in")) { Value.remove_prefix(2); return fv * dpi; } // Inches
      else if (Value.starts_with("cm")) { Value.remove_prefix(2); return fv * (1.0 / 2.54) * dpi; } // Centimetres
      else if (Value.starts_with("mm")) { Value.remove_prefix(2); return fv * (1.0 / 25.4) * dpi; } // Millimetres
      else if (Value.starts_with("pt")) { Value.remove_prefix(2); return fv * (4.0 / 3.0); } // Points.  A point is 4/3 of a pixel
      else if (Value.starts_with("pc")) { Value.remove_prefix(2); return fv * (4.0 / 3.0) * 12.0; } // Pica.  1 Pica is equal to 12 Points
      else if (Value.starts_with("px")) { Value.remove_prefix(2); return fv; } // Pixel

      return fv; // Default to 'px' / pixel
   }
   Value = std::string_view();
   return DBL_MIN;
}

//********************************************************************************************************************
// This function forces all coordinates to be interpreted as relative when in BOUNDINGBOX mode.
//
// NOTE: It would be possible to deprecate this in future if the viewport host is given a viewbox area of (0 0 1 1)
// as it should be.

inline Unit parse_units(const std::string_view Value, VUNIT Units)
{
   auto v = std::string_view(Value);
   double num = read_unit(v);
   if (Units IS VUNIT::BOUNDING_BOX) return Unit(num, FD_SCALED);
   else return Unit(num);
}

//********************************************************************************************************************

inline std::string_view next_value(const std::string_view Value)
{
   std::size_t i = 0;
   while ((i < Value.size()) and ((Value[i] <= 0x20) or (Value[i] IS ',') or (Value[i] IS '(') or (Value[i] IS ')'))) i++;
   return std::string_view(Value.data() + i, Value.size() - i);
}

//********************************************************************************************************************
// The parser will break once the string value terminates, or an invalid character is encountered.  Parsed characters
// include: 0 - 9 , ( ) - + SPACE

template <class T = double> std::string_view read_numseq(std::string_view String, std::initializer_list<T *> Value)
{
   for (T *v : Value) {
      String = next_value(String);

      T num;
      auto [ next, error ] = std::from_chars(String.data(), String.data() + String.size(), num);
      if (error != std::errc()) return String;
      String = std::string_view(next, String.data() + String.size() - next);
      *v = num;
   }

   return String;
}

//********************************************************************************************************************
// Read a sequence of doubles from a string.  Commas, parenthesis and whitespace is ignored.

template<class T = double> kt::vector<T> read_array(const std::string &Value, int Limit = 0x7fffffff)
{
   kt::vector<T> result;

   if (iequals("none", Value)) return result;

   auto v = std::string_view(Value);
   while ((not v.empty()) and (std::ssize(result) < Limit)) {
      while ((not v.empty()) and ((v[0] <= 0x20) or (v[0] IS ',') or (v[0] IS '(') or (v[0] IS ')'))) v.remove_prefix(1);
      if (v.empty()) return result;

      auto num = read_unit(v);
      if (num IS DBL_MIN) return result;

      result.push_back(num);
   }

   return result;
}

//********************************************************************************************************************
// This function is called before fully parsing the document so that we can extract all tags making use of the
// 'id' attribute.

static void parse_ids(extSVG *Self, XTag &Tag)
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &name = Tag.Attribs[a].Name;
      if ((name.size() IS 2) and (name[0] IS 'i') and (name[1] IS 'd')) {
         if (Tag.Attribs[a].Value.empty()) continue;

         if (Self->IDs.contains(Tag.Attribs[a].Value)) break;
         Self->IDs.emplace(Tag.Attribs[a].Value, &Tag);
         break;
      }
   }

   for (auto &child : Tag.Children) {
      if (child.isTag()) {
         parse_ids(Self, child);
      }
   }
}

//********************************************************************************************************************
// SVG Parameters: an attribute value of the form "param(name) fallback" is substituted with the value of the named
// parameter, or the fallback if the parameter is undefined.  Only values that start with "param(" are recognised.

static std::string_view trim_ws(std::string_view Value) noexcept
{
   while ((not Value.empty()) and (unsigned(Value.front()) <= 0x20)) Value.remove_prefix(1);
   while ((not Value.empty()) and (unsigned(Value.back()) <= 0x20)) Value.remove_suffix(1);
   return Value;
}

static bool is_param_value(std::string_view Value) noexcept
{
   while ((not Value.empty()) and (unsigned(Value.front()) <= 0x20)) Value.remove_prefix(1);
   return Value.starts_with("param(");
}

//********************************************************************************************************************
// Splits a param() expression into its name and fallback.  Returns false if the name is malformed, in which case the
// fallback still applies.

static bool parse_param_value(std::string_view Value, std::string_view &Name, std::string_view &Fallback) noexcept
{
   Name = {};
   Fallback = {};

   Value = trim_ws(Value);
   if (not Value.starts_with("param(")) return false;
   Value.remove_prefix(6);

   auto end = Value.find(')');
   if (end IS std::string_view::npos) return false;

   Fallback = trim_ws(Value.substr(end + 1));
   Name = trim_ws(Value.substr(0, end));

   for (auto ch : Name) {
      if ((unsigned(ch) <= 0x20) or (ch IS '(')) { Name = {}; break; }
   }

   return not Name.empty();
}

//********************************************************************************************************************
// Lookup order: the active <use> scope, then client values from SetKey(), then the Path query string.

static const std::string * find_param(extSVG *Self, const ParamScope *Scope, std::string_view Name) noexcept
{
   if (Scope) {
      if (auto it = Scope->Values.find(Name); it != Scope->Values.end()) return &it->second;
   }

   if (auto it = Self->Params.find(Name); it != Self->Params.end()) return &it->second;
   if (auto it = Self->QueryParams.find(Name); it != Self->QueryParams.end()) return &it->second;
   return nullptr;
}

// Returns the substituted value, or nullopt if the attribute must be erased (no value and no fallback).

static std::optional<std::string> resolve_param_value(extSVG *Self, std::string_view Value, const ParamScope *Scope)
{
   std::string_view name, fallback;
   if (parse_param_value(Value, name, fallback)) {
      if (auto value = find_param(Self, Scope, name)) return *value;
   }

   if (fallback.empty()) return std::nullopt;
   return std::string(fallback);
}

//********************************************************************************************************************
// Rebuilds the attributes of a tag from its raw (unresolved) copy.  The 'id' attribute is never substituted because
// it must remain stable for href lookups.

static void resolve_tag_params(extSVG *Self, XTag &Tag, const kt::vector<XMLAttrib> &Raw, const ParamScope *Scope)
{
   Tag.Attribs.clear();
   Tag.Attribs.reserve(Raw.size());
   Tag.Attribs.push_back(Raw[0]);

   for (int a=1; a < std::ssize(Raw); a++) {
      auto &attrib = Raw[a];
      if ((attrib.Name != "id") and (is_param_value(attrib.Value))) {
         if (auto value = resolve_param_value(Self, attrib.Value, Scope)) {
            Tag.Attribs.emplace_back(attrib.Name, *value);
         }
      }
      else Tag.Attribs.push_back(attrib);
   }
}

//********************************************************************************************************************
// Document pass, performed once after styles are folded into attributes.  The raw attributes of every tag using
// param() are saved so that they can be re-resolved for <use> scopes and ParseSymbol().

static void apply_document_params(extSVG *Self, objXML::TAGS &Tags)
{
   for (auto &tag : Tags) {
      if (not tag.isTag()) continue;

      for (int a=1; a < std::ssize(tag.Attribs); a++) {
         if ((tag.Attribs[a].Name != "id") and (is_param_value(tag.Attribs[a].Value))) {
            auto &raw = Self->ParamAttribs[tag.ID];
            raw = tag.Attribs;
            resolve_tag_params(Self, tag, raw, nullptr);
            break;
         }
      }

      apply_document_params(Self, tag.Children);
   }
}

//********************************************************************************************************************
// Re-resolves the param() attributes of a tag and its descendants against a scope.  A tag renamed since the document
// pass (a <use> expanded to a <g>) keeps its current attributes.

static void rescope_params(extSVG *Self, XTag &Tag, const ParamScope *Scope)
{
   if (auto it = Self->ParamAttribs.find(Tag.ID); it != Self->ParamAttribs.end()) {
      if (Tag.Attribs[0].Name IS it->second[0].Name) resolve_tag_params(Self, Tag, it->second, Scope);
   }

   for (auto &child : Tag.Children) {
      if (child.isTag()) rescope_params(Self, child, Scope);
   }
}

//********************************************************************************************************************
// Temporarily re-resolves a shared subtree (e.g. a <symbol>) against a scope, restoring the original attributes on
// destruction.  The children of <use> elements are not visited because proc_use() rewrites them, which would
// invalidate the saved tag pointers.  A tag that has been renamed during processing (<use> to <g>) is not restored.

class ParamGuard {
   struct Saved {
      XTag *Tag;
      kt::vector<XMLAttrib> Attribs;
   };

   std::vector<Saved> m_saved;

   void apply(extSVG *Self, XTag &Tag, const ParamScope *Scope) {
      if (auto it = Self->ParamAttribs.find(Tag.ID); it != Self->ParamAttribs.end()) {
         if (Tag.Attribs[0].Name IS it->second[0].Name) {
            m_saved.push_back({ &Tag, Tag.Attribs });
            resolve_tag_params(Self, Tag, it->second, Scope);
         }
      }

      if (svg_tag_is(Tag, SVF_use)) return;

      for (auto &child : Tag.Children) {
         if (child.isTag()) apply(Self, child, Scope);
      }
   }

public:
   ParamGuard(extSVG *Self, XTag &Tag, const ParamScope *Scope) {
      if (not Self->ParamAttribs.empty()) apply(Self, Tag, Scope);
   }

   ~ParamGuard() {
      for (auto &saved : m_saved) {
         if (saved.Tag->Attribs[0].Name IS saved.Attribs[0].Name) saved.Tag->Attribs = std::move(saved.Attribs);
      }
   }

   ParamGuard(const ParamGuard &) = delete;
   ParamGuard & operator=(const ParamGuard &) = delete;
};

//********************************************************************************************************************
// Selects the tree to process for one instance of a <symbol>.  Expanding nested <use> elements mutates the tree, so
// a document with parameters processes a private copy that is resolved against the instance scope.  The copy is
// released on destruction unless an animation was created from it, because animations retain their tag pointers.

class SymbolInstance {
   extSVG *m_svg;
   std::list<XTag>::iterator m_copy;
   size_t m_animations = 0;
   bool m_copied = false;

public:
   XTag *tag;

   SymbolInstance(extSVG *Self, XTag &Symbol, const ParamScope *Scope) : m_svg(Self), tag(&Symbol) {
      if (Self->ParamAttribs.empty()) return;
      m_copy = Self->SymbolInstances.emplace(Self->SymbolInstances.end(), Symbol);
      rescope_params(Self, *m_copy, Scope);
      tag = &*m_copy;
      m_animations = Self->Animations.size();
      m_copied = true;
   }

   ~SymbolInstance() {
      if ((m_copied) and (m_svg->Animations.size() IS m_animations)) m_svg->SymbolInstances.erase(m_copy);
   }

   SymbolInstance(const SymbolInstance &) = delete;
   SymbolInstance & operator=(const SymbolInstance &) = delete;
};

//********************************************************************************************************************
// Collects the sorted names of all parameters referenced by a tag and its descendants.

static void collect_param_names(extSVG *Self, const XTag &Tag, kt::vector<std::string> &Names)
{
   if (auto it = Self->ParamAttribs.find(Tag.ID); it != Self->ParamAttribs.end()) {
      for (int a=1; a < std::ssize(it->second); a++) {
         std::string_view name, fallback;
         if (parse_param_value(it->second[a].Value, name, fallback)) Names.emplace_back(name);
      }
   }

   for (auto &child : Tag.Children) {
      if (child.isTag()) collect_param_names(Self, child, Names);
   }
}

//********************************************************************************************************************
// Reads a <param> element from its raw attributes, resolving the value against Scope.  Returns false if the name or
// value is missing, in which case the element is ignored.  Value is nullopt if a param() value resolved to nothing.

static bool read_param_element(extSVG *Self, const XTag &Tag, const ParamScope *Scope, std::string &Name,
   std::optional<std::string> &Value)
{
   auto raw_it = Self->ParamAttribs.find(Tag.ID);
   auto &raw = (raw_it != Self->ParamAttribs.end()) ? raw_it->second : Tag.Attribs;

   const std::string *name = nullptr, *value = nullptr;
   for (int a=1; a < std::ssize(raw); a++) {
      switch (strhash(raw[a].Name)) {
         case SVF_name:  name = &raw[a].Value; break;
         case SVF_value: value = &raw[a].Value; break;
      }
   }

   if ((not name) or (name->empty()) or (not value)) return false;

   Name = *name;
   if (is_param_value(*value)) Value = resolve_param_value(Self, *value, Scope);
   else Value = *value;
   return true;
}

//********************************************************************************************************************
// Splits a query string from an SVG path, e.g. "button.svg?color=red".  Because '?' is a legal file name character
// on some platforms, it is only treated as a separator when the text preceding it names an SVG file.

static bool split_svg_query(std::string_view Path, std::string_view &File, std::string_view &Query) noexcept
{
   for (auto q = Path.find('?'); q != std::string_view::npos; q = Path.find('?', q + 1)) {
      if (wildcmp("*.svg|*.svgz", Path.substr(0, q))) {
         File  = Path.substr(0, q);
         Query = Path.substr(q + 1);
         return true;
      }
   }
   return false;
}

static std::string percent_decode(std::string_view Value)
{
   auto hex = [](char Ch) -> int {
      if ((Ch >= '0') and (Ch <= '9')) return Ch - '0';
      if ((Ch >= 'a') and (Ch <= 'f')) return Ch - 'a' + 10;
      if ((Ch >= 'A') and (Ch <= 'F')) return Ch - 'A' + 10;
      return -1;
   };

   std::string result;
   result.reserve(Value.size());
   for (size_t i=0; i < Value.size(); i++) {
      if ((Value[i] IS '%') and (i + 2 < Value.size()) and (hex(Value[i+1]) >= 0) and (hex(Value[i+2]) >= 0)) {
         result.push_back(char((hex(Value[i+1])<<4) | hex(Value[i+2])));
         i += 2;
      }
      else result.push_back(Value[i]);
   }
   return result;
}

//********************************************************************************************************************

static std::string percent_encode(std::string_view Value)
{
   static constexpr char hex[] = "0123456789ABCDEF";
   std::string result;
   result.reserve(Value.size());
   for (auto ch : Value) {
      if (((ch >= 'a') and (ch <= 'z')) or ((ch >= 'A') and (ch <= 'Z')) or ((ch >= '0') and (ch <= '9')) or
          (ch IS '-') or (ch IS '.') or (ch IS '_') or (ch IS '~')) {
         result.push_back(ch);
      }
      else {
         result.push_back('%');
         result.push_back(hex[uint8_t(ch)>>4]);
         result.push_back(hex[uint8_t(ch) & 0xf]);
      }
   }
   return result;
}

//********************************************************************************************************************
// SVG Parameters: the <param> children of an <image> are appended to an SVG href as a query string, overriding values
// of the same name already present in the href.  The nested document then resolves them as query parameters.  Values
// are resolved against Scope, so a <use> or SetKey() value can be forwarded with <param name="a" value="param(a)"/>.

static std::string apply_image_params(extSVG *Self, const XTag &Tag, const std::string &Href, const ParamScope *Scope)
{
   std::vector<std::pair<std::string, std::string>> params;
   for (auto &child : Tag.Children) {
      if ((not child.isTag()) or (not svg_tag_is(child, SVF_param))) continue;

      std::string name;
      std::optional<std::string> value;
      if (not read_param_element(Self, child, Scope, name, value)) continue;
      if (not value) continue; // A param() value with no result and no fallback is undefined

      auto it = std::ranges::find_if(params, [&](const auto &Param) { return Param.first IS name; });
      if (it != params.end()) it->second = std::move(*value);
      else params.emplace_back(std::move(name), std::move(*value));
   }

   if (params.empty()) return Href;

   std::string_view file, query;
   if (not split_svg_query(Href, file, query)) {
      if (not wildcmp("*.svg|*.svgz", Href)) {
         kt::Log(__FUNCTION__).warning("<param> ignored for non-SVG image '%s' @ line %d", Href.c_str(), Tag.LineNo);
         return Href;
      }
      file = Href;
   }

   std::string result(file);
   char sep = '?';

   while (not query.empty()) { // Retain existing pairs that are not overridden
      auto end = query.find('&');
      auto pair = query.substr(0, end);
      query = (end IS std::string_view::npos) ? std::string_view{} : query.substr(end + 1);
      if (pair.empty()) continue;

      auto name = percent_decode(pair.substr(0, pair.find('=')));
      if (std::ranges::any_of(params, [&](const auto &Param) { return Param.first IS name; })) continue;

      result.push_back(sep);
      result.append(pair);
      sep = '&';
   }

   for (auto &[name, value] : params) {
      result.push_back(sep);
      result.append(percent_encode(name)).push_back('=');
      result.append(percent_encode(value));
      sep = '&';
   }

   return result;
}

//********************************************************************************************************************
// Parses "name=value&name=value" into the QueryParams store.  Later duplicates overwrite earlier ones.

static void parse_query_params(extSVG *Self, std::string_view Query)
{
   while (not Query.empty()) {
      auto end = Query.find('&');
      auto pair = Query.substr(0, end);
      Query = (end IS std::string_view::npos) ? std::string_view{} : Query.substr(end + 1);

      auto eq = pair.find('=');
      auto name = percent_decode(pair.substr(0, eq));
      if (name.empty()) continue;
      Self->QueryParams[name] = (eq IS std::string_view::npos) ? std::string() : percent_decode(pair.substr(eq + 1));
   }
}

//********************************************************************************************************************
// Returns true if a tag in the document declares a Kotuku namespace, whether or not the prefix is used.

static bool declares_kotuku_namespace(const kt::vector<XTag> &Tags)
{
   for (auto &tag : Tags) {
      for (int a=1; a < std::ssize(tag.Attribs); a++) {
         const auto &name = tag.Attribs[a].Name;
         if ((name != "xmlns") and (not (name.starts_with("xmlns:") and (name.size() > 6)))) continue;
         const auto uri = strhash(tag.Attribs[a].Value);
         if ((uri IS glKotukuNamespace) or (uri IS glKotukuSVGNamespace)) return true;
      }
      if (declares_kotuku_namespace(tag.Children)) return true;
   }
   return false;
}

//********************************************************************************************************************
// Parse SVG from a file or string buffer.

static ERR parse_svg(extSVG *Self, std::string_view Path, std::string_view Buffer)
{
   kt::Log log(__FUNCTION__);

   if ((Path.empty()) and (Buffer.empty())) return ERR::NullArgs;

   if (not Path.empty()) log.branch("Path: %.*s [Log-level reduced]", int(Path.size()), Path.data());
   else log.branch("Path: <xml-statement> [Log-level reduced]");

#ifndef DEBUG
   AdjustLogLevel(1);
#endif

   Self->IDs.clear();
   Self->QueryParams.clear();
   Self->ParamAttribs.clear();
   Self->ParamDeps.clear();
   Self->TextContent.clear();
   Self->AutoscaleWidth  = Unit();
   Self->AutoscaleHeight = Unit();
   if (Self->XML) { FreeResource(Self->XML); Self->XML = nullptr; }

   // Parameters can be passed in the query string of an SVG path, e.g. "button.svg?color=red&label=OK"

   if (std::string_view file, query; split_svg_query(Path, file, query)) {
      parse_query_params(Self, query);
      Path = file;
   }

   Self->ParsedParams = Self->QueryParams;
   for (auto &[name, value] : Self->Params) Self->ParsedParams[name] = value;

   objXML *xml;
   ERR error = ERR::Okay;
   if (!NewLocalObject(CLASSID::XML, &xml)) {
      xml->setFlags(XMF::NAMESPACE_AWARE|XMF::WELL_FORMED);

      if (not Path.empty()) {
         if (wildcmp("*.svgz", Path)) {
            if (auto file = objFile::create::global(fl::Owner(xml->UID), fl::Path(Path), fl::Flags(FL::READ))) {
               if (auto stream = objCompressedStream::create::global(fl::Owner(file->UID), fl::Input(file))) {
                  xml->setSource(stream);
               }
               else {
                  FreeResource(xml);
                  FreeResource(file);
                  error = ERR::CreateObject;
                  goto end;
               }
            }
            else {
               FreeResource(xml);
               error = ERR::CreateObject;
               goto end;
            }
         }
         else xml->setPath(Path);
      }
      else if (not Buffer.empty()) xml->setStatement(Buffer);

      if (!InitObject(xml)) {
         Self->SVGVersion = 1.0;

         Self->XML = xml;

         // A document that declares a Kotuku namespace is asking for features beyond the SVG standard, so SVG
         // compliance is dropped.  This applies only to a scene that the SVG object owns; a target's scene belongs to
         // the client.

         if ((Self->Scene) and (Self->Scene->ownerID() IS Self->UID)) {
            Self->Scene->setCompliance(declares_kotuku_namespace(xml->Tags) ? VSC::NONE : VSC::SVG2);
         }

         prepare_styles(xml->Tags);
         apply_document_params(Self, xml->Tags);

         objVector *sibling = nullptr;
         for (auto &scan : xml->Tags) {
            if (svg_tag_is(scan, SVF_svg)) {
               svgState state(Self);

               // Parse all tags with an 'id' reference so that href's can target them even when
               // are out-of-order.

               parse_ids(Self, scan);

               if (Self->Target) state.proc_svg(scan, Self->Target, sibling);
               else state.proc_svg(scan, Self->Scene, sibling);
            }
         }

         // Support for inheritance

         for (auto &inherit : Self->Inherit) {
            OBJECTPTR ref;
            if (!Self->Scene->findDef(inherit.ID, &ref)) {
               OBJECTPTR target;
               if (auto field = FindField(inherit.Object, strhash("inherit"), &target)) target->set(field, ref);
            }
            else log.warning("Failed to resolve ID %s for inheritance.", inherit.ID.c_str());
         }

         if ((Self->Flags & SVF::AUTOSCALE) != SVF::NIL) {
            // If auto-scale is enabled, access the top-level viewport and set the Width and Height to 100%

            auto view = Self->Scene->Viewport;
            while ((view) and (view->classID() != CLASSID::VECTORVIEWPORT)) view = (objVectorViewport *)view->Next;
            if (view) {
               view->getWidth(Self->AutoscaleWidth);
               view->getHeight(Self->AutoscaleHeight);
               view->setWidth(Unit(1.0, FD_SCALED));
               view->setHeight(Unit(1.0, FD_SCALED));
            }
         }
      }
      else {
         FreeResource(xml);
         error = ERR::Init;
      }
   }
   else error = ERR::NewObject;

end:
#ifndef DEBUG
   AdjustLogLevel(-1);
#endif
   return error;
}

//********************************************************************************************************************

static bool read_integer_value(std::string_view Value, int &Result) noexcept
{
   Value = next_value(Value);
   if (Value.empty()) return false;

   auto [ next, error ] = std::from_chars(Value.data(), Value.data() + Value.size(), Result);
   if (error != std::errc()) return false;

   Value = next_value(std::string_view(next, Value.data() + Value.size() - next));
   return Value.empty();
}

//********************************************************************************************************************

static bool read_positive_integer_pair(std::string_view Value, int &X, int &Y) noexcept
{
   int values[2] = { 0, 0 };
   int count = 0;

   while (true) {
      Value = next_value(Value);
      if (Value.empty()) break;
      if (count >= 2) return false;

      auto [ next, error ] = std::from_chars(Value.data(), Value.data() + Value.size(), values[count]);
      if (error != std::errc()) return false;

      Value = std::string_view(next, Value.data() + Value.size() - next);
      count++;
   }

   if (not count) return false;
   if (values[0] <= 0) return false;
   if ((count > 1) and (values[1] <= 0)) return false;

   X = values[0];
   Y = (count > 1) ? values[1] : X;
   return true;
}

//********************************************************************************************************************

static bool read_positive_number_pair(std::string_view Value, double &X, double &Y) noexcept
{
   double values[2] = { 0.0, 0.0 };
   int count = 0;

   while (true) {
      Value = next_value(Value);
      if (Value.empty()) break;
      if (count >= 2) return false;

      auto [ next, error ] = std::from_chars(Value.data(), Value.data() + Value.size(), values[count]);
      if (error != std::errc()) return false;

      Value = std::string_view(next, Value.data() + Value.size() - next);
      count++;
   }

   if (not count) return false;
   if (values[0] <= 0.0) return false;
   if ((count > 1) and (values[1] <= 0.0)) return false;

   X = values[0];
   Y = (count > 1) ? values[1] : X;
   return true;
}
