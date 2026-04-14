
agg::gamma_lut<uint8_t, uint16_t, 8, 12> glGamma(2.2);
double glDisplayHDPI = 96, glDisplayVDPI = 96, glDisplayDPI = 96;

[[maybe_unused]] static HSV rgb_to_hsl(FRGB Colour);
[[maybe_unused]] static FRGB hsl_to_rgb(HSV Colour);
static void read_numseq_zero(CSTRING &, std::initializer_list<double *>);

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
      case CLASSID::VECTORGRADIENT:  return "Gradient";
      case CLASSID::VECTORPATTERN:   return "Pattern";
      case CLASSID::VECTOR:          return "Vector";
      case CLASSID::VECTORSCENE:     return "Scene";
      default: break;
   }

   return "Unknown";
}

//********************************************************************************************************************

static void update_dpi(void)
{
   static int64_t last_update = -0x7fffffff;
   int64_t current_time = PreciseTime();

   if (current_time - last_update > 3000000LL) {
      DISPLAYINFO *display;
      if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
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

ERR read_path(std::vector<PathCommand> &Path, CSTRING Value)
{
   pf::Log log(__FUNCTION__);

   PathCommand path;

   int max_cmds = 8192; // Maximum commands per path - this acts as a safety net in case the parser gets stuck.
   uint8_t cmd = 0;
   while (*Value) {
      if (std::isalpha(*Value)) cmd = *Value++;
      else if (std::isdigit(*Value) or (*Value IS '-') or (*Value IS '+') or (*Value IS '.')); // Use the previous command
      else if ((*Value <= 0x20) or (*Value IS ',')) { Value++; continue; }
      else break;

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
            log.warning("Invalid path command '%c'", *Value);
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
   pf::Log log(Caller);

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
      else if ((str[0] IS 'c') and (str[1] IS 'm')) { str += 2; multiplier = (1.0 / 2.56) * dpi; } // Centimetres
      else if ((str[0] IS 'm') and (str[1] IS 'm')) { str += 2; multiplier = (1.0 / 20.56) * dpi; } // Millimetres
      else if ((str[0] IS 'p') and (str[1] IS 't')) { str += 2; multiplier = (1.0 / 72.0) * dpi; } // Points.  A point is 1/72 of an inch
      else if ((str[0] IS 'p') and (str[1] IS 'c')) { str += 2; multiplier = (12.0 / 72.0) * dpi; } // Pica.  1 Pica is equal to 12 Points

      auto result = strtod(Value, nullptr) * multiplier;

      Value = str;
      return result;
   }
   else return 0;
}

//********************************************************************************************************************

std::string weight_to_style(CSTRING Style, int Weight)
{
   std::string weight_name;

   if (Weight >= 700) weight_name = "Extra Bold";
   else if (Weight >= 500) weight_name = "Bold";
   else if (Weight <= 200) weight_name = "Extra Light";
   else if (Weight <= 300) weight_name = "Light";

   if (iequals("Italic", Style)) {
      if (weight_name.empty()) return "Italic";
      else return weight_name + " Italic";
   }
   else if (!weight_name.empty()) return weight_name;
   else return "Regular";
}

//********************************************************************************************************************

ERR get_font(pf::Log &Log, CSTRING Family, CSTRING Style, int Weight, int Size, common_font **Handle)
{
   Log.branch("Family: %s, Style: %s, Weight: %d, Size: %d", Family, Style, Weight, Size);

   if (!Style) return Log.warning(ERR::NullArgs);

   const std::lock_guard lock{glFontMutex};

   std::string family(Family ? Family : "*");
   if (!family.ends_with("*")) family.append(",*");
   CSTRING final_name;
   if (fnt::ResolveFamilyName(family.c_str(), &final_name) IS ERR::Okay) family.assign(final_name);

   std::string style(Style);
   if ((Weight) and (Weight != 400)) {
      // If a weight value is specified and is anything other than "Normal"/400 then it will
      // override the named Style completely.
      style = weight_to_style(Style, Weight);
   }

   const int point_size = std::round(Size * (72.0 / DISPLAY_DPI));
   CSTRING location = nullptr;
   FMETA meta = FMETA::NIL;
   if (auto error = fnt::SelectFont(family.c_str(), style.c_str(), &location, &meta); error IS ERR::Okay) {
      LocalResource loc(location);

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
         // For scalable fonts the key is made from the location only, ensuring that the face file is loaded
         // only once.  If the file is variable, it will contain multiple styles.  Otherwise, assume the file
         // represents one type of style.

         auto key = strihash(location);

         if (!glFreetypeFonts.contains(key)) {
            std::string resolved;
            if (ResolvePath(location, RSF::NIL, &resolved) IS ERR::Okay) {
               FT_Face ftface;
               FT_Open_Args openargs = { .flags = FT_OPEN_PATHNAME, .pathname = resolved.data() };
               if (FT_Open_Face(glFTLibrary, &openargs, 0, &ftface)) {
                  Log.warning("Fatal error in attempting to load font \"%s\".", resolved.c_str());
                  return ERR::File;
               }

               freetype_font::METRIC_TABLE metrics;
               freetype_font::STYLE_CACHE styles;

               if (FT_HAS_MULTIPLE_MASTERS(ftface)) {
                  FT_MM_Var *mvar;
                  if (!FT_Get_MM_Var(ftface, &mvar)) {
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
                                    auto str = (uint16_t *)sft_name.string;
                                    uint16_t prev_unicode = 0;
                                    for (FT_UInt i=0; (i < sft_name.string_len>>1) and (out < std::ssize(buffer)-8); i++) {
                                       uint16_t unicode = (str[i]>>8) | (uint8_t(str[i])<<8);
                                       if ((unicode >= 'A') and (unicode <= 'Z')) {
                                          if ((i > 0) and (prev_unicode >= 'a') and (prev_unicode <= 'z')) {
                                             buffer[out++] = ' ';
                                          }
                                       }
                                       out += UTF8WriteValue(unicode, buffer+out, std::ssize(buffer)-out);
                                       prev_unicode = unicode;
                                    }
                                    buffer[out] = 0;
                                    freetype_font::METRIC_GROUP set;
                                    for (unsigned m=0; m < mvar->num_axis; m++) {
                                       set.push_back(mvar->namedstyle[s].coords[m]);
                                    }
                                    metrics.try_emplace(buffer, set);
                                    styles.try_emplace(buffer);
                                    break;
                                 }
                              }
                           }
                        }
                     }
                     FT_Done_MM_Var(glFTLibrary, mvar);
                  }
               }
               else styles.try_emplace(style);

               glFreetypeFonts.try_emplace(key, std::make_unique<freetype_font>(ftface, styles, metrics, meta));
            }
            else return Log.warning(ERR::ResolvePath);
         }

         auto &font = *glFreetypeFonts[key];
         if (auto cache = font.style_cache.find(style); cache != font.style_cache.end()) {
            freetype_font::SIZE_CACHE &sz = cache->second;
            if (auto size = sz.find(Size); size IS sz.end()) {
               // New font size entry required

               if (font.metrics.contains(style)) {
                  auto new_size = sz.try_emplace(Size, font, font.metrics[style], Size);
                  if (!new_size.first->second.ft_size) return ERR::Failed; // Verify success
                  *Handle = &new_size.first->second;
                  return ERR::Okay;
               }
               else {
                  if (!font.metrics.empty()) Log.warning("Font metrics do not support style '%s'", style.c_str());
                  auto new_size = sz.try_emplace(Size, font, Size);
                  if (!new_size.first->second.ft_size) return ERR::Failed; // Verify success
                  *Handle = &new_size.first->second;
                  return ERR::Okay;
               }
            }
            else {
               *Handle = &size->second;
               return ERR::Okay;
            }
         }
         else return ERR::Search;
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

void read_numseq_zero(CSTRING &String, std::initializer_list<double *> Value)
{
   for (double *v : Value) {
      auto next = (STRING)String;
      next_value(String);
      *v = strtod(String, &next);
      String = next;
   }
}
