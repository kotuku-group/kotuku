
gamma_table glGamma;
const translucent_table glTranslucent;
double glDisplayHDPI = 96, glDisplayVDPI = 96, glDisplayDPI = 96;

[[maybe_unused]] static HSV rgb_to_hsl(FRGB Colour);
[[maybe_unused]] static FRGB hsl_to_rgb(HSV Colour);
static void read_numseq_zero(std::string_view &, std::initializer_list<double *>);
static bool read_transform_number(std::string_view &Value, double &Result);

//********************************************************************************************************************

const FieldDef clAspectRatio[] = {
   { "XMin",  ARF::X_MIN },
   { "XMid",  ARF::X_MID },
   { "XMax",  ARF::X_MAX },
   { "YMin",  ARF::Y_MIN },
   { "YMid",  ARF::Y_MID },
   { "YMax",  ARF::Y_MAX },
   { "Meet",  ARF::MEET },
   { "Slice", ARF::SLICE },
   { "None",  ARF::NONE },
   { nullptr, 0 }
};

//********************************************************************************************************************

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

static FRGB hsl_to_rgb(HSV Colour)
{
   auto hueToRgb = [](float p, float q, float t) -> float {
      if (t < 0) t += 1;
      if (t > 1) t -= 1;
      if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
      if (t < 1.0 / 2.0) return q;
      if (t < 2.0 / 3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
      return p;
   };

   if (Colour.Saturation == 0) {
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

CSTRING get_name(OBJECTPTR Vector)
{
   if (!Vector) return "nullptr";

   switch(Vector->classID()) {
      case CLASSID::VECTORCLIP:      return "Clip";
      case CLASSID::VECTORRECTANGLE: return "Rectangle";
      case CLASSID::VECTORELLIPSE:   return "Ellipse";
      case CLASSID::VECTORPATH:      return "Path";
      case CLASSID::VECTORPOLYGON:   return "Polygon";
      case CLASSID::VECTORTEXT:      return "Text";
      case CLASSID::VECTORFILTER:    return "Filter";
      case CLASSID::VECTORGROUP:     return "Group";
      case CLASSID::VECTORVIEWPORT:  return "Viewport";
      case CLASSID::VECTORWAVE:      return "Wave";
      default: break;
   }

   switch(Vector->baseClassID()) {
      case CLASSID::VECTORCOLOUR:    return "Colour";
      case CLASSID::VECTORFILTER:    return "Filter";
      case CLASSID::VECTORPATTERN:   return "Pattern";
      case CLASSID::VECTOR:          return "Vector";
      case CLASSID::VECTORSCENE:     return "Scene";
      default: break;
   }

   if (Vector->baseClassID() IS CLASSID::GRADIENT) return "Gradient";

   return "Unknown";
}

//********************************************************************************************************************
// Records the density of the display.  Vector units, including font sizes and font metrics, are converted at the fixed
// DISPLAY_DPI so that rendering is independent of the display.  These values must not be used to scale font metrics,
// because the glyph outlines do not account for the display density.

static void update_dpi(void)
{
   static int64_t last_update = -0x7fffffff;
   int64_t current_time = PreciseTime();

   if (current_time - last_update > 3000000LL) {
      DisplayInfo *display;
      if (!gfx::GetDisplayInfo(0, &display)) {
         last_update = PreciseTime();
         if ((display->VDensity >= 72) and (display->HDensity >= 72)) {
            glDisplayVDPI = display->VDensity;
            glDisplayHDPI = display->HDensity;
            glDisplayDPI = (glDisplayVDPI + glDisplayHDPI) * 0.5;
         }
      }
   }
}

//********************************************************************************************************************
// Read a string-based series of vector commands and add them to Path.
//
// SVG position on error handling: Unrecognized contents within a path data stream (i.e., contents that are not part
// of the path data grammar) is an error.  The general rule for error handling in path data is that the SVG user agent
// shall render a ‘path’ element up to (but not including) the path command containing the first error in the path
// data specification. This will provide a visual clue to the user or developer about where the error might be in the
// path data specification.

ERR read_path(std::vector<PathCommand> &Path, std::string_view Value)
{
   kt::Log log(__FUNCTION__);

   PathCommand path;

   int max_cmds = 8192; // Maximum commands per path - this acts as a safety net in case the parser gets stuck.
   uint8_t cmd = 0;
   while (not Value.empty()) {
      const auto current = Value.front();
      const bool use_previous_cmd = std::isdigit((uint8_t)current) or (current IS '-') or (current IS '+') or
         (current IS '.');
      if (std::isalpha((uint8_t)current)) {
         cmd = current;
         Value.remove_prefix(1);
      }
      else if ((current <= 0x20) or (current IS ',')) {
         Value.remove_prefix(1);
         continue;
      }
      else if (not use_previous_cmd) break;

      switch (cmd) {
         case 'M': case 'm': // MoveTo
            read_numseq_zero(Value, { &path.X, &path.Y });
            if (cmd IS 'M') {
               path.Type = PE::Move;
               cmd = 'L'; // This is because the SVG standard requires that uninterrupted coordinate pairs are interpreted as line-to commands.
            }
            else {
               path.Type = PE::MoveRel;
               cmd = 'l';
            }
            break;

         case 'L': case 'l': // LineTo
            read_numseq_zero(Value, { &path.X, &path.Y });
            if (cmd IS 'L') path.Type = PE::Line;
            else path.Type = PE::LineRel;
            break;

         case 'V': case 'v': // Vertical LineTo
            path.X = 0; // Needs to be zero to satisfy any curve instructions that might follow
            read_numseq_zero(Value, { &path.Y });
            if (cmd IS 'V') path.Type = PE::VLine;
            else path.Type = PE::VLineRel;
            break;

         case 'H': case 'h': // Horizontal LineTo
            path.Y = 0; // Needs to be zero to satisfy any curve instructions that might follow
            read_numseq_zero(Value, { &path.X });
            if (cmd IS 'H') path.Type = PE::HLine;
            else path.Type = PE::LineRel;
            break;

         case 'Q': case 'q': // Quadratic Curve To
            read_numseq_zero(Value, { &path.X2, &path.Y2, &path.X, &path.Y });
            if (cmd IS 'Q') path.Type = PE::QuadCurve;
            else path.Type = PE::QuadCurveRel;
            break;

         case 'T': case 't': // Quadratic Smooth Curve To
            read_numseq_zero(Value, { &path.X, &path.Y });
            if (cmd IS 'T') path.Type = PE::QuadSmooth;
            else path.Type = PE::QuadSmoothRel;
           break;

         case 'C': case 'c': // Curve To
            read_numseq_zero(Value, { &path.X2, &path.Y2, &path.X3, &path.Y3, &path.X, &path.Y });
            if (cmd IS 'C') path.Type = PE::Curve;
            else path.Type = PE::CurveRel;
            break;

         case 'S': case 's': // Smooth Curve To
            read_numseq_zero(Value, { &path.X2, &path.Y2, &path.X, &path.Y });
            if (cmd IS 'S') path.Type = PE::Smooth;
            else path.Type = PE::SmoothRel;
            break;

         case 'A': case 'a': { // Arc
            double largearc, sweep;
            read_numseq_zero(Value, { &path.X2, &path.Y2, &path.Angle, &largearc, &sweep, &path.X, &path.Y });
            path.LargeArc = int(largearc);
            path.Sweep = int(sweep);
            if ((path.LargeArc != 1) and (path.LargeArc != 0)) return ERR::InvalidValue;
            if ((path.Sweep != 1) and (path.Sweep != 0)) return ERR::InvalidValue;
            if (cmd IS 'A') path.Type = PE::Arc;
            else path.Type = PE::ArcRel;
            break;
         }

         // W3C: When a subpath ends in a "closepath," it differs in behaviour from what happens when "manually" closing
         // a subpath via a "lineto" command in how ‘stroke-linejoin’ and ‘stroke-linecap’ are implemented. With
         // "closepath", the end of the final segment of the subpath is "joined" with the start of the initial segment
         // of the subpath using the current value of ‘stroke-linejoin’. If you instead "manually" close the subpath
         // via a "lineto" command, the start of the first segment and the end of the last segment are not joined but
         // instead are each capped using the current value of ‘stroke-linecap’. At the end of the command, the new
         // current point is set to the initial point of the current subpath.

         case 'Z': case 'z': { // Close Path
            path.Type = PE::ClosePath;
            break;
         }

         default: {
            log.warning("Invalid path command '%c'", cmd ? cmd : current);
            return ERR::InvalidValue;
         }
      }

      if (--max_cmds == 0) {
         Path.clear();
         return log.warning(ERR::BufferOverflow);
      }

      Path.push_back(path);
   }

   return (Path.size() >= 2) ? ERR::Okay : ERR::InvalidData;
}

//********************************************************************************************************************
// Calculate the target X/Y for a vector path based on an aspect ratio and source/target dimensions.
// Source* defines size of the source area (in SVG, the 'viewbox')
// Target* defines the size of the projection to the display.

void calc_aspectratio(CSTRING Caller, ARF AspectRatio, double TargetWidth, double TargetHeight,
   double SourceWidth, double SourceHeight, double &X, double &Y, double &XScale, double &YScale)
{
   kt::Log log(Caller);

   // Prevent division by zero errors.  Note that the client can legitimately set these values to zero, so we cannot
   // treat such situations as an error on the client's part.

   if (TargetWidth <= DBL_MIN) TargetWidth = 0.1;
   if (TargetHeight <= DBL_MIN) TargetHeight = 0.1;

   // A Source size of 0 is acceptable and will be treated as equivalent to the target.

   if (SourceWidth <= DBL_MIN) SourceWidth = TargetWidth;
   if (SourceHeight <= DBL_MIN) SourceHeight = TargetHeight;

   if ((AspectRatio & (ARF::MEET|ARF::SLICE)) != ARF::NIL) {
      double xScale = TargetWidth / SourceWidth;
      double yScale = TargetHeight / SourceHeight;

      // MEET: Choose the smaller of the two scaling factors, so that the scaled graphics meet the edge of the
      // viewport and do not exceed it.  SLICE: Choose the larger scale, expanding beyond the boundary on one axis.

      if ((AspectRatio & ARF::MEET) != ARF::NIL) {
         xScale = yScale = std::min(xScale, yScale);
      }
      else if ((AspectRatio & ARF::SLICE) != ARF::NIL) {
         xScale = yScale = std::max(xScale, yScale);
      }

      XScale = xScale;
      YScale = yScale;

      X = ((AspectRatio & ARF::X_MIN) != ARF::NIL) ? 0 :
          ((AspectRatio & ARF::X_MID) != ARF::NIL) ? (TargetWidth - (SourceWidth * xScale)) * 0.5 :
          ((AspectRatio & ARF::X_MAX) != ARF::NIL) ? TargetWidth - (SourceWidth * xScale) : 0;

      Y = ((AspectRatio & ARF::Y_MIN) != ARF::NIL) ? 0 :
          ((AspectRatio & ARF::Y_MID) != ARF::NIL) ? (TargetHeight - (SourceHeight * yScale)) * 0.5 :
          ((AspectRatio & ARF::Y_MAX) != ARF::NIL) ? TargetHeight - (SourceHeight * yScale) : 0;
   }
   else { // ARF::NONE
      X = 0;
      XScale = (TargetWidth >= 1.0 and SourceWidth > 0) ? TargetWidth / SourceWidth : 1.0;
      Y = 0;
      YScale = (TargetHeight >= 1.0 and SourceHeight > 0) ? TargetHeight / SourceHeight : 1.0;
   }

   log.trace("ARF Aspect: $%.8x, Target: %.0fx%.0f, View: %.0fx%.0f, AlignXY: %.2fx%.2f, Scale: %.2fx%.2f",
      int(AspectRatio), TargetWidth, TargetHeight, SourceWidth, SourceHeight, X, Y, XScale, YScale);
}

//********************************************************************************************************************
// Calculate the boundaries for a branch of the tree, including transforms, and return the combined maximum bound
// values.  NOTE: This function performs a full traversal (siblings and children) and this may extend beyond the
// viewport's visible boundary.
//
// See also VECTOR_GetBoundary(), for which this function is intended, and set_clip_region() for filters.

void calc_full_boundary(extVector *Vector, TClipRectangle<double> &Bounds, bool IncludeSiblings, bool IncludeTransforms, bool IncludeStrokes)
{
   if (!Vector) return;

   for (; Vector; Vector=(extVector *)Vector->Next) {
      if (Vector->dirty()) gen_vector_path(Vector);

      if (Vector->classID() != CLASSID::VECTORVIEWPORT) { // Don't consider viewport sizes when determining content dimensions.
         if (Vector->BasePath.total_vertices()) {
            double stroke = 0;
            if (IncludeTransforms) {
               if ((IncludeStrokes) and (Vector->Stroked)) {
                  stroke = Vector->fixed_stroke_width() * Vector->Transform.scale() * 0.5;
               }

               if (Vector->Transform.is_complex()) {
                  auto simple_path = Vector->Bounds.as_path();
                  agg::conv_transform<agg::path_storage, agg::trans_affine> path(Vector->BasePath, Vector->Transform);
                  auto pb = get_bounds(path);
                  if (pb.left   - stroke < Bounds.left)   Bounds.left   = pb.left - stroke;
                  if (pb.top    - stroke < Bounds.top)    Bounds.top    = pb.top - stroke;
                  if (pb.right  + stroke > Bounds.right)  Bounds.right  = pb.right + stroke;
                  if (pb.bottom + stroke > Bounds.bottom) Bounds.bottom = pb.bottom + stroke;
               }
               else {
                  if (Vector->Bounds.left   - stroke + Vector->Transform.tx < Bounds.left)   Bounds.left   = Vector->Bounds.left + Vector->Transform.tx - stroke;
                  if (Vector->Bounds.top    - stroke + Vector->Transform.ty < Bounds.top)    Bounds.top    = Vector->Bounds.top + Vector->Transform.ty - stroke;
                  if (Vector->Bounds.right  + stroke + Vector->Transform.tx > Bounds.right)  Bounds.right  = Vector->Bounds.right + Vector->Transform.tx + stroke;
                  if (Vector->Bounds.bottom + stroke + Vector->Transform.ty > Bounds.bottom) Bounds.bottom = Vector->Bounds.bottom + Vector->Transform.ty + stroke;
               }
            }
            else {
               if ((IncludeStrokes) and (Vector->Stroked)) stroke = Vector->fixed_stroke_width() * 0.5;

               if (Vector->Bounds.left   - stroke < Bounds.left)   Bounds.left   = Vector->Bounds.left - stroke;
               if (Vector->Bounds.top    - stroke < Bounds.top)    Bounds.top    = Vector->Bounds.top - stroke;
               if (Vector->Bounds.right  + stroke > Bounds.right)  Bounds.right  = Vector->Bounds.right + stroke;
               if (Vector->Bounds.bottom + stroke > Bounds.bottom) Bounds.bottom = Vector->Bounds.bottom + stroke;
            }
         }
      }

      if (Vector->Child) calc_full_boundary((extVector *)Vector->Child, Bounds, true, IncludeTransforms, IncludeStrokes);

      if (!IncludeSiblings) break;
   }
}

//********************************************************************************************************************
// Calculate the boundary of Vector and its children in the coordinate system of Vector, i.e. without Vector's own
// Transform applied.  The transforms of the children are expressed relative to Vector, so content that is scaled by
// a nested viewBox or transformed group is measured in the same units as Vector.  Siblings are not included.
//
// Used by filters to compute bounding-box regions, which are later mapped to the display by Vector->Transform.

static void extend_local_boundary(extVector *Vector, const agg::trans_affine &Base, const agg::trans_affine &Inverse,
   TClipRectangle<double> &Bounds, bool IncludeSiblings)
{
   for (; Vector; Vector=(extVector *)Vector->Next) {
      if (Vector->dirty()) gen_vector_path(Vector);

      // Viewport sizes are not considered when determining content dimensions.

      if ((Vector->classID() != CLASSID::VECTORVIEWPORT) and (Vector->BasePath.total_vertices())) {
         if (Vector->Transform.is_equal(Base)) {
            if (Vector->Bounds.left   < Bounds.left)   Bounds.left   = Vector->Bounds.left;
            if (Vector->Bounds.top    < Bounds.top)    Bounds.top    = Vector->Bounds.top;
            if (Vector->Bounds.right  > Bounds.right)  Bounds.right  = Vector->Bounds.right;
            if (Vector->Bounds.bottom > Bounds.bottom) Bounds.bottom = Vector->Bounds.bottom;
         }
         else {
            auto relative = Vector->Transform;
            relative.multiply(Inverse);
            agg::conv_transform<agg::path_storage, agg::trans_affine> path(Vector->BasePath, relative);
            auto pb = get_bounds(path);
            if (pb.left   < Bounds.left)   Bounds.left   = pb.left;
            if (pb.top    < Bounds.top)    Bounds.top    = pb.top;
            if (pb.right  > Bounds.right)  Bounds.right  = pb.right;
            if (pb.bottom > Bounds.bottom) Bounds.bottom = pb.bottom;
         }
      }

      if (Vector->Child) extend_local_boundary((extVector *)Vector->Child, Base, Inverse, Bounds, true);

      if (!IncludeSiblings) break;
   }
}

void calc_local_boundary(extVector *Vector, TClipRectangle<double> &Bounds)
{
   if (!Vector) return;

   if (Vector->dirty()) gen_vector_path(Vector);

   if (std::abs(Vector->Transform.determinant()) < agg::affine_epsilon) {
      // A degenerate transform cannot be inverted; fall back to the untransformed bounds of each vector.
      calc_full_boundary(Vector, Bounds, false, false);
      return;
   }

   auto inverse = Vector->Transform;
   inverse.invert();
   extend_local_boundary(Vector, Vector->Transform, inverse, Bounds, false);
}

//********************************************************************************************************************
// Designed for reading unit values such as '50%' and '6px'.  The returned value is scaled to pixels.

double read_unit(CSTRING &Value, bool &Percent)
{
   Percent = false;

   while ((*Value) and (*Value <= 0x20)) Value++;

   CSTRING str = Value;
   if ((*str IS '-') or (*str IS '+')) str++;

   if (std::isdigit(*str)) {
      while (std::isdigit(*str)) str++;

      if (*str IS '.') {
         str++;
         if (std::isdigit(*str)) {
            while (std::isdigit(*str)) str++;
         }
      }

      double multiplier = 1.0;
      double dpi = DISPLAY_DPI;

      if (*str IS '%') {
         Percent = true;
         multiplier = 0.01;
         str++;
      }
      else if ((str[0] IS 'p') and (str[1] IS 'x')) str += 2; // Pixel.  This is the default type
      else if ((str[0] IS 'e') and (str[1] IS 'm')) { str += 2; multiplier = (12.0 / 72.0) * dpi; } // Multiply the current font's pixel height by the provided em value
      else if ((str[0] IS 'e') and (str[1] IS 'x')) { str += 2; multiplier = (6.0 / 72.0) * dpi; } // As for em, but multiple by the pixel height of the 'x' character.  If no x character, revert to 0.5em
      else if ((str[0] IS 'i') and (str[1] IS 'n')) { str += 2; multiplier = dpi; } // Inches
      else if ((str[0] IS 'c') and (str[1] IS 'm')) { str += 2; multiplier = (1.0 / 2.54) * dpi; } // Centimetres
      else if ((str[0] IS 'm') and (str[1] IS 'm')) { str += 2; multiplier = (1.0 / 25.4) * dpi; } // Millimetres
      else if ((str[0] IS 'p') and (str[1] IS 't')) { str += 2; multiplier = (1.0 / 72.0) * dpi; } // Points.  A point is 1/72 of an inch
      else if ((str[0] IS 'p') and (str[1] IS 'c')) { str += 2; multiplier = (12.0 / 72.0) * dpi; } // Pica.  1 Pica is equal to 12 Points

      auto result = strtod(Value, nullptr) * multiplier;

      Value = str;
      return result;
   }
   else return 0;
}

//********************************************************************************************************************

// Axis tags used by the friendly style descriptors.

static constexpr uint32_t TAG_WGHT = make_axis_tag("wght");
static constexpr uint32_t TAG_WDTH = make_axis_tag("wdth");
static constexpr uint32_t TAG_SLNT = make_axis_tag("slnt");
static constexpr uint32_t TAG_ITAL = make_axis_tag("ital");
static constexpr uint32_t TAG_OPSZ = make_axis_tag("opsz");

// The style cache key for static faces.  The file is the style, so all requests that select it share one entry.

static const std::string STATIC_STYLE_KEY("static");

struct resolved_style {
   std::string key;                      // Canonical coordinates, e.g. 'wdth=75;wght=550'
   freetype_font::METRIC_GROUP coords;   // Design coordinates in face axis order
   int auto_opsz = -1;                   // Index of the opsz axis if the optical size follows the font size
};

//********************************************************************************************************************
// Resolves the design coordinates of a variable face for a parsed style.  The precedence, from lowest to highest, is:
// the face defaults, the named instance, the Weight value, friendly descriptors and raw axis tags.  Values are
// clamped to the axis range, and axes that the face does not have are ignored.
//
// The key is built from the coordinates at 16.16 precision, sorted by tag, so that equivalent style strings share a
// cache entry.  An automatic optical size depends on the font size, so it is excluded from the key and applied by
// the caller.

static resolved_style resolve_style(kt::Log &Log, freetype_font &Font, const font_style_spec &Spec, int Weight)
{
   std::vector<double> coords(Font.axes.size());
   for (size_t i=0; i < Font.axes.size(); i++) coords[i] = Font.axes[i].def;

   auto set_axis = [&Font, &coords](uint32_t Tag, double Value) {
      for (size_t i=0; i < Font.axes.size(); i++) {
         if (Font.axes[i].tag IS Tag) {
            coords[i] = std::clamp(Value, Font.axes[i].minimum, Font.axes[i].maximum);
            return true;
         }
      }
      return false;
   };

   if (not Spec.named.empty()) {
      if (auto it = Font.metrics.find(Spec.named); it != Font.metrics.end()) {
         for (size_t i=0; (i < it->second.size()) and (i < coords.size()); i++) {
            coords[i] = double(it->second[i]) / 65536.0;
         }
      }
      else if (auto traits = kt::font_traits::name_traits(Spec.named); traits.recognised) {
         // The face has no instance of this name, e.g. 'Bold' in an italic file that names it 'Bold Italic', so the
         // coordinates are derived from the traits of the name.
         set_axis(TAG_WGHT, traits.weight);
         set_axis(TAG_WDTH, traits.width);
         if (traits.italic) set_axis(TAG_ITAL, 1);
      }
      else Log.warning("Font metrics do not support style '%s'", Spec.named.c_str());
   }

   if (Weight) set_axis(TAG_WGHT, Weight);

   if (Spec.weight) set_axis(TAG_WGHT, *Spec.weight);
   if (Spec.width) set_axis(TAG_WDTH, *Spec.width);
   if (Spec.slant) set_axis(TAG_SLNT, -*Spec.slant); // CSS oblique angles lean right, slnt values lean left
   if (Spec.italic) set_axis(TAG_ITAL, *Spec.italic ? 1 : 0);

   for (auto & [ tag, value ] : Spec.tags) {
      if (not set_axis(tag, value)) Log.trace("Face has no '%s' axis.", axis_tag_name(tag).c_str());
   }

   if (Spec.opsz_mode IS font_style_spec::OPSZ::FIXED) set_axis(TAG_OPSZ, Spec.opsz);

   resolved_style result;
   result.coords.resize(coords.size());
   for (size_t i=0; i < coords.size(); i++) result.coords[i] = FT_Fixed(std::lround(coords[i] * 65536.0));

   std::vector<std::pair<std::string, std::string>> entries;
   for (size_t i=0; i < Font.axes.size(); i++) {
      std::string value;
      if ((Font.axes[i].tag IS TAG_OPSZ) and (Spec.opsz_mode IS font_style_spec::OPSZ::AUTO)) {
         result.auto_opsz = int(i);
         value = "auto";
      }
      else {
         char buffer[32];
         auto end = std::to_chars(buffer, buffer + sizeof(buffer), double(result.coords[i]) / 65536.0).ptr;
         value.assign(buffer, end);
      }
      entries.emplace_back(axis_tag_name(Font.axes[i].tag), std::move(value));
   }

   std::sort(entries.begin(), entries.end());
   for (auto & [ tag, value ] : entries) {
      if (not result.key.empty()) result.key.push_back(';');
      result.key.append(tag).append("=").append(value);
   }

   return result;
}

//********************************************************************************************************************
// Options that a family alias in fonts:options.cfg applies to the family that it refers to.  An alias can refer to
// another alias, in which case the outer alias takes precedence for any option that both declare.

struct alias_options {
   std::string name;        // The style name of the outermost alias that declares one
   std::string descriptors; // Style descriptors of all aliases, innermost first so that outer aliases win
   ConfigKeys metrics;      // Metric options that override those of the family
};

static constexpr std::array<std::string_view, 5> ALIAS_METRIC_KEYS = {
   "LineSpacing", "Descent", "Ascent", "Kerning", "Embolden"
};

static alias_options read_alias_options(kt::Log &Log, const std::string &Alias)
{
   alias_options result;
   if ((Alias.empty()) or (not glFontConfig)) return result;

   kt::ScopedObjectLock<objConfig> config(glFontConfig, 500);
   if (not config.granted()) return result;

   APTR group_ptr;
   if (glFontConfig->getData(group_ptr) != ERR::Okay) return result;
   auto groups = (ConfigGroups *)group_ptr;

   // Collect the alias groups from the outermost inwards.  The chain ends at a group that has no Alias key, which
   // is the target family.  The depth limit guards against cyclic aliases, which ResolveFamilyName() rejects.

   std::vector<const ConfigKeys *> chain;
   std::string name(Alias);
   while (chain.size() < 8) {
      const ConfigKeys *match = nullptr;
      for (auto & [group, keys] : groups[0]) {
         if (auto it = keys.find("Name"); (it != keys.end()) and kt::wildcmp(name, it->second)) {
            match = &keys;
            break;
         }
      }

      if (not match) break;
      auto it = match->find("Alias");
      if ((it IS match->end()) or (it->second.empty())) break;
      chain.push_back(match);
      name = it->second;
   }

   for (auto keys = chain.rbegin(); keys != chain.rend(); keys++) {
      if (auto it = (*keys)->find("Style"); it != (*keys)->end()) {
         kt::font_traits::for_each_descriptor(kt::font_traits::style_part(it->second),
            [&result](std::string_view Key, std::string_view Value, bool HasValue) {
            if (HasValue) result.descriptors.append(Key).append("=").append(Value).append(";");
            else result.name.assign(Value);
         });
      }

      for (auto key : ALIAS_METRIC_KEYS) {
         if (auto it = (*keys)->find(key); it != (*keys)->end()) result.metrics[std::string(key)] = it->second;
      }
   }

   if ((not result.descriptors.empty()) or (not result.name.empty()) or (not result.metrics.empty())) {
      Log.trace("Alias '%s' style: '%s%s%s'", Alias.c_str(), result.name.c_str(),
         result.descriptors.empty() ? "" : ";", result.descriptors.c_str());
   }
   return result;
}

//********************************************************************************************************************
// Merges the style of an alias with the client's style.  The client's style name is used unless it is absent or
// 'Regular', in which case the alias's name applies.  The alias's descriptors precede the client's so that the
// client takes precedence for any descriptor that both declare.

static std::string merge_alias_style(const alias_options &Alias, std::string_view Style)
{
   if ((Alias.name.empty()) and (Alias.descriptors.empty())) return std::string(Style);

   std::string_view name;
   std::string descriptors;
   kt::font_traits::for_each_descriptor(Style, [&](std::string_view Key, std::string_view Value, bool HasValue) {
      if (HasValue) descriptors.append(Key).append("=").append(Value).append(";");
      else if (name.empty()) name = Value;
   });

   auto normal = kt::font_traits::normalise(name);
   std::string result;
   if ((normal.empty()) or (normal IS "regular") or (normal IS "normal")) {
      result = Alias.name.empty() ? std::string("Regular") : Alias.name;
   }
   else result = name;

   if (not Alias.descriptors.empty()) result.append(";").append(Alias.descriptors);
   if (not descriptors.empty()) {
      if (Alias.descriptors.empty()) result.append(";");
      result.append(descriptors);
   }
   if (result.ends_with(';')) result.pop_back();
   return result;
}

//********************************************************************************************************************
// Returns a cached font handle for a family, style string and pixel size.  See the VectorText FontStyle field for
// the style string syntax.  A Weight of 0 or 400 is treated as unset.

ERR get_font(kt::Log &Log, std::string_view Family, std::string_view Style, int Weight, int Size, common_font **Handle)
{
   Log.branch("Family: %.*s, Style: %.*s, Weight: %d, Size: %d", int(Family.size()), Family.data(),
      int(Style.size()), Style.data(), Weight, Size);

   if (Style.empty()) return Log.warning(ERR::NullArgs);

   const std::lock_guard lock{glFontMutex};

   std::string family(Family.empty() ? "*" : Family);
   if (not family.ends_with("*")) family.append(",*");
   std::string_view final_name, alias_name;
   std::string alias;
   if (!fnt::ResolveFamilyName(family, &final_name, &alias_name)) {
      alias.assign(alias_name);
      family.assign(final_name);
   }

   // An alias can declare a Style that is merged with the client's, and metric options that override those of the
   // family.  The metric options are applied per alias, so they are included in the style cache key.

   auto alias_opts = read_alias_options(Log, alias);
   // A definition that has only non-style keys is the same request as an unstyled font.

   auto style_keys = kt::font_traits::style_part(Style);
   std::string style = merge_alias_style(alias_opts, style_keys.empty() ? "Regular" : style_keys);

   auto spec = parse_font_style(style);
   for (auto &warning : spec.warnings) Log.warning("%s", warning.c_str());

   if (Weight IS 400) Weight = 0;

   // Preserve numeric traits until SelectFont() matches them against the family's actual styles.  The Weight
   // argument seeds the request, while descriptors in Style retain their higher precedence.

   if (Weight) style = "weight=" + std::to_string(Weight) + ";" + style;

   std::string metric_key;
   if (not alias_opts.metrics.empty()) metric_key = ";alias=" + alias;

   const int point_size = std::round(Size * (72.0 / DISPLAY_DPI));
   std::string location;
   FMETA meta = FMETA::NIL;
   int face_index = 0;
   if (auto error = fnt::SelectFontFace(family, style, &location, &meta, &face_index); !error) {
      if ((meta & FMETA::SCALED) IS FMETA::NIL) { // Bitmap font
         auto key = strihash(style + ":" + std::to_string(point_size) + ":" + location);

         if (glBitmapFonts.contains(key)) {
            *Handle = glBitmapFonts[key].get();
            return ERR::Okay;
         }
         else if (auto font = objFont::create::global(fl::Name("vector_cached_font"),
               fl::Owner(glVectorModule->UID),
               fl::Face(family),
               fl::Style(style),
               fl::Point(point_size),
               fl::Path(location))) {
            auto bmp_font_ptr = std::make_unique<bmp_font>(font);
            glBitmapFonts.emplace(key, std::move(bmp_font_ptr));
            *Handle = glBitmapFonts[key].get();
            return ERR::Okay;
         }
         else return ERR::CreateObject;
      }
      else {
         // Each face in a collection has its own glyph and variation caches.  Variable styles within a face
         // continue to share that face's cache.

         auto key = strihash(location + ":" + std::to_string(face_index));

         if (!glFreetypeFonts.contains(key)) {
            std::string resolved;
            if (!ResolvePath(location, RSF::NIL, &resolved)) {
               FT_Face ftface;
               FT_Open_Args openargs = { .flags = FT_OPEN_PATHNAME, .pathname = resolved.data() };
               if (FT_Open_Face(glFTLibrary, &openargs, face_index, &ftface)) {
                  Log.warning("Fatal error in attempting to load font \"%s\".", resolved.c_str());
                  return ERR::File;
               }

               freetype_font::METRIC_TABLE metrics;
               freetype_font::STYLE_CACHE styles;
               std::vector<freetype_font::axis_info> axes;

               if (FT_HAS_MULTIPLE_MASTERS(ftface)) {
                  FT_MM_Var *mvar;
                  if (!FT_Get_MM_Var(ftface, &mvar)) {
                     for (FT_UInt a=0; a < mvar->num_axis; a++) {
                        auto &axis = mvar->axis[a];
                        axes.push_back({ uint32_t(axis.tag), double(axis.minimum) / 65536.0,
                           double(axis.def) / 65536.0, double(axis.maximum) / 65536.0 });
                     }

                     FT_UInt index;
                     if (!FT_Get_Default_Named_Instance(ftface, &index)) {
                        auto name_table_size = FT_Get_Sfnt_Name_Count(ftface);
                        for (FT_UInt s=0; s < mvar->num_namedstyles; s++) {
                           for (int n=name_table_size-1; n >= 0; n--) {
                              FT_SfntName sft_name;
                              if (!FT_Get_Sfnt_Name(ftface, n, &sft_name)) {
                                 if (sft_name.name_id IS mvar->namedstyle[s].strid) {
                                    // Decode UTF16 Big Endian
                                    char buffer[100];
                                    int out = 0;
                                    const FT_Byte *str = sft_name.string; // Big-endian bytes, possibly unaligned
                                    uint16_t prev_unicode = 0;
                                    for (FT_UInt i=0; (i < sft_name.string_len>>1) and (out < std::ssize(buffer)-8); i++) {
                                       uint16_t unicode = uint16_t((str[i*2]<<8) | str[(i*2)+1]);
                                       if ((unicode >= 'A') and (unicode <= 'Z')) {
                                          if ((i > 0) and (prev_unicode >= 'a') and (prev_unicode <= 'z')) {
                                             buffer[out++] = ' ';
                                          }
                                       }
                                       out += UTF8WriteValue(unicode,
                                          std::span<int8_t>((int8_t *)(buffer + out), std::size(buffer) - size_t(out)));
                                       prev_unicode = unicode;
                                    }
                                    buffer[out] = 0;
                                    freetype_font::METRIC_GROUP set;
                                    for (unsigned m=0; m < mvar->num_axis; m++) {
                                       set.push_back(mvar->namedstyle[s].coords[m]);
                                    }
                                    metrics.try_emplace(buffer, set);
                                    break;
                                 }
                              }
                           }
                        }
                     }
                     FT_Done_MM_Var(glFTLibrary, mvar);
                  }
               }

               const auto family_hash = strihash(family);
               auto hint = HINT::NIL;
               if (auto it = glHintOverrides.find(family_hash); it != glHintOverrides.end()) hint = it->second;
               glFreetypeFonts.try_emplace(key,
                  std::make_unique<freetype_font>(ftface, styles, metrics, axes, meta, family_hash, hint));
            }
            else return Log.warning(ERR::ResolvePath);
         }

         auto &font = *glFreetypeFonts[key];

         if (font.axes.empty()) { // Static face: the file is the style, so there are no coordinates to resolve
            if (not spec.tags.empty()) Log.trace("Axis tags are ignored by static faces.");

            auto &sizes = font.style_cache[STATIC_STYLE_KEY + metric_key];
            if (auto it = sizes.find(Size); it != sizes.end()) {
               *Handle = &it->second;
               return ERR::Okay;
            }

            auto new_size = sizes.try_emplace(Size, font, Size, alias_opts.metrics);
            if (!new_size.first->second.ft_size) return ERR::CreateResource; // Verify success
            *Handle = &new_size.first->second;
            return ERR::Okay;
         }
         else {
            auto resolved = resolve_style(Log, font, spec, Weight);
            auto &sizes = font.style_cache[resolved.key + metric_key];
            if (auto it = sizes.find(Size); it != sizes.end()) {
               *Handle = &it->second;
               return ERR::Okay;
            }

            if (resolved.auto_opsz >= 0) {
               // The optical size follows the font size in points, as defined by OpenType.
               auto &axis = font.axes[resolved.auto_opsz];
               auto opsz = std::clamp(Size * 72.0 / DISPLAY_DPI, axis.minimum, axis.maximum);
               resolved.coords[resolved.auto_opsz] = FT_Fixed(std::lround(opsz * 65536.0));
            }

            Log.trace("Resolved style '%s' for size %d.", resolved.key.c_str(), Size);
            auto new_size = sizes.try_emplace(Size, font, resolved.coords, Size, alias_opts.metrics);
            if (!new_size.first->second.ft_size) return ERR::CreateResource; // Verify success
            *Handle = &new_size.first->second;
            return ERR::Okay;
         }
      }
   }
   else return error;
}

//********************************************************************************************************************
// The parser will break once the string value terminates, or an invalid character is encountered.
//
// There are two variants - the first aborts if an unparseable value is encountered.  The second will set all
// unparseable result values to zero.
//
// Parsed characters include: 0 - 9 , ( ) - + SPACE

ERR read_numseq(CSTRING &String, std::initializer_list<double *> Value)
{
   for (double *v : Value) {
      STRING next = nullptr;
      next_value(String);
      double num = strtod(String, &next);
      if ((!num) and ((!next) or (String IS next))) {  // Invalid character or end-of-stream check.
         String = next;
         return ERR::Syntax;
      }
      String = next;
      *v = num;
   }

   return ERR::Okay;
}

ERR read_numseq(std::string_view &String, std::initializer_list<double *> Value)
{
   for (double *v : Value) {
      if (not read_transform_number(String, *v)) return ERR::Syntax;
   }

   return ERR::Okay;
}

void read_numseq_zero(std::string_view &String, std::initializer_list<double *> Value)
{
   for (double *v : Value) {
      double result = 0;
      if (read_transform_number(String, result)) *v = result;
      else *v = 0;
   }
}

//********************************************************************************************************************

void next_value(std::string_view &Value)
{
   while ((not Value.empty()) and ((Value.front() <= 0x20) or (Value.front() IS ',') or (Value.front() IS '(') or
      (Value.front() IS ')'))) {
      Value.remove_prefix(1);
   }
}

//********************************************************************************************************************

static bool read_transform_number(std::string_view &Value, double &Result)
{
   next_value(Value);
   if (Value.empty()) return false;

   const char *start = Value.data();
   const char *parse_start = start;

   if (Value.front() IS '+') parse_start++;

   auto [ next, error ] = std::from_chars(parse_start, Value.data() + Value.size(), Result);
   if (error != std::errc()) return false;

   Value.remove_prefix(next - start);
   return true;
}

//********************************************************************************************************************

bool read_transform_unit(std::string_view &Value, double &Result)
{
   if (not read_transform_number(Value, Result)) return false;

   double multiplier = 1.0;
   if (Value.starts_with('%')) {
      multiplier = 0.01;
      Value.remove_prefix(1);
   }
   else if (Value.starts_with("px")) Value.remove_prefix(2);
   else if (Value.starts_with("em")) {
      multiplier = (12.0 / 72.0) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("ex")) {
      multiplier = (6.0 / 72.0) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("in")) {
      multiplier = DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("cm")) {
      multiplier = (1.0 / 2.54) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("mm")) {
      multiplier = (1.0 / 25.4) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("pt")) {
      multiplier = (1.0 / 72.0) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }
   else if (Value.starts_with("pc")) {
      multiplier = (12.0 / 72.0) * DISPLAY_DPI;
      Value.remove_prefix(2);
   }

   Result *= multiplier;
   return true;
}
