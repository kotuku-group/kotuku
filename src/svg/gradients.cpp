// Kōtuku SVG extensions:
//   It is possible to reference in-built colourmaps via 'href'
//   The fx,fy values can be placed outside of the radial gradient if 'focal="unbound"' is used
//   The resolution value can be defined to lower the rate of colour sampling.

static ERR gradient_defaults(extSVG *Self, objGradient *Gradient, uint32_t Attrib, const std::string &Value)
{
   switch (Attrib) {
      case SVF_resolution:
         Gradient->setResolution(svtonum<double>(Value));
         return ERR::Okay;

      case SVF_color_interpolation:
         if (iequals("auto", Value))           Gradient->ColourSpace = VCS::LINEAR_RGB;
         else if (iequals("sRGB", Value))      Gradient->ColourSpace = VCS::SRGB;
         else if (iequals("linearRGB", Value)) Gradient->ColourSpace = VCS::LINEAR_RGB;
         else if (iequals("inherit", Value))   Gradient->ColourSpace = VCS::INHERIT;
         return ERR::Okay;

      case SVF_easing:
         switch (strhash(Value)) {
            case SVF_linear:     Gradient->setEasing(GEZ::LINEAR); break;
            case SVF_in:         Gradient->setEasing(GEZ::IN); break;
            case SVF_out:        Gradient->setEasing(GEZ::OUT); break;
            case SVF_inOut:      Gradient->setEasing(GEZ::IN_OUT); break;
            case SVF_cubicIn:    Gradient->setEasing(GEZ::CUBIC_IN); break;
            case SVF_cubicOut:   Gradient->setEasing(GEZ::CUBIC_OUT); break;
            case SVF_cubicInOut: Gradient->setEasing(GEZ::CUBIC_IN_OUT); break;
            default: kt::Log().warning("Unrecognised gradient easing '%s'", Value.c_str());
         }
         return ERR::Okay;

      // Ignored attributes (sometimes defined to propagate to child tags)
      case SVF_color:
      case SVF_stop_color:
      case SVF_stop_opacity:
         return ERR::Okay;
   }

   return ERR::Failed;
}

//********************************************************************************************************************

static void set_gradient_units(const XTag &Tag, objGradient *Gradient) noexcept
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
         if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) Gradient->Units = VUNIT::USERSPACE;
         else if (iequals("objectBoundingBox", Tag.Attribs[a].Value)) Gradient->Units = VUNIT::BOUNDING_BOX;
         break;
      }
   }
}

//********************************************************************************************************************

static void set_gradient_spread_method(objGradient *Gradient, const std::string &Value) noexcept
{
   if (iequals("pad", Value))          Gradient->setSpreadMethod(VSPREAD::PAD);
   else if (iequals("reflect", Value)) Gradient->setSpreadMethod(VSPREAD::REFLECT);
   else if (iequals("repeat", Value))  Gradient->setSpreadMethod(VSPREAD::REPEAT);
}

//********************************************************************************************************************

static void warn_unknown_gradient_attribute(kt::Log &Log, const XTag &Tag, const std::string &Name) noexcept
{
   if (Name.find(':') != std::string::npos) return;
   Log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Name.c_str(), Tag.LineNo);
}

//********************************************************************************************************************
// Shared resolver for <stop> colour attributes, used by both the generic gradient stop parser and mesh gradient
// stops.  Inline style declarations are converted to real attributes at load time (see convert_styles()), so only
// presentation attributes require handling here.  Style-derived attributes are appended after the presentation
// attributes, so processing in order gives them CSS precedence.  'inherit' resolves against the current state's
// stop-color/stop-opacity and 'currentColor' against the current 'color' value.

FRGB svgState::resolve_stop_colour(const XTag &Tag) noexcept
{
   FRGB colour(0,0,0,1);
   double stop_opacity = 1.0;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &name  = Tag.Attribs[a].Name;
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      if (iequals("stop-color", name)) {
         VectorPainter painter;
         if (iequals("inherit", value)) {
            if (not m_stop_color.empty()) {
               vec::ReadPainter(Self->Scene, m_stop_color, &painter, nullptr);
               colour = painter.Colour;
            }
            // With no inherited value, 'inherit' resolves to the initial stop-color.  It must still
            // overwrite any colour set by an earlier attribute (style declarations arrive last and win).
            else colour = FRGB(0,0,0,1);
         }
         else if (iequals("currentColor", value)) {
            vec::ReadPainter(Self->Scene, m_color, &painter, nullptr);
            colour = painter.Colour;
         }
         else {
            vec::ReadPainter(Self->Scene, value, &painter, nullptr);
            colour = painter.Colour;
         }
      }
      else if (iequals("stop-opacity", name)) {
         if (iequals("inherit", value)) stop_opacity = (m_stop_opacity >= 0) ? m_stop_opacity : 1.0;
         else stop_opacity = svtonum<double>(value);
      }
   }

   colour.Alpha = float(double(colour.Alpha) * stop_opacity);
   return colour;
}

//********************************************************************************************************************
// Note that all offsets are percentages.

const kt::vector<GradientStop> svgState::process_gradient_stops(const XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   double last_stop = 0;
   kt::vector<GradientStop> stops;
   for (auto &scan : Tag.Children) {
      if (svg_tag_hash(scan) IS kt::strhash("stop")) {
         GradientStop stop;
         stop.Offset = 0;
         stop.RGB = resolve_stop_colour(scan);

         for (int a=1; a < std::ssize(scan.Attribs); a++) {
            auto &name  = scan.Attribs[a].Name;
            auto &value = scan.Attribs[a].Value;
            if (value.empty()) continue;

            if (iequals("offset", name)) {
               stop.Offset = svtonum<double>(value);
               for (int j=0; value[j]; j++) {
                  if (value[j] IS '%') {
                     stop.Offset = stop.Offset * 0.01; // Must be in the range of 0 - 1.0
                     break;
                  }
               }

               if (stop.Offset < 0.0) stop.Offset = 0;
               else if (stop.Offset > 1.0) stop.Offset = 1.0;

               if (stop.Offset < last_stop) stop.Offset = last_stop;
               else last_stop = stop.Offset;
            }
            else if (iequals("stop-color", name));   // Resolved by resolve_stop_colour()
            else if (iequals("stop-opacity", name)); // Resolved by resolve_stop_colour()
            else if (iequals("id", name)) {
               log.trace("Use of id attribute in <stop/> ignored.");
            }
            else log.warning("Unable to process stop attribute '%s'", name.c_str());
         }

         stops.emplace_back(stop);
      }
      else log.warning("Unknown element in gradient, '%s'", scan.name());
   }

   // SVG: If one stop is defined, then paint with the solid color fill using the color defined for that gradient stop.

   if (stops.size() IS 1) {
      stops[0].Offset = 0;
      stops.emplace_back(stops[0]);
      stops[1].Offset = 1;
   }

   return stops;
}

//********************************************************************************************************************

bool svgState::parse_gradient_href(const std::string &Value, objGradient *Gradient) noexcept
{
   if (Value.starts_with("url(#cmap:")) {
      auto end = Value.find(')');
      if (end != std::string::npos) {
         auto cmap = Value.substr(5, end - 5);
         if (Gradient->setColourMap(cmap) IS ERR::Okay) return false;
      }
   }
   else if (auto other = find_href_tag(Self, Value)) {
      std::string dummy;

      if (svg_tag_is(*other, SVF_radialGradient)) {
         parse_radialgradient(*other, (objGradientRadial *)Gradient, dummy);
      }
      else if (svg_tag_is(*other, SVF_linearGradient)) {
         parse_lineargradient(*other, (objGradientLinear *)Gradient, dummy);
      }
      else if (svg_tag_is(*other, SVF_diamondGradient)) {
         parse_diamondgradient(*other, (objGradientDiamond *)Gradient, dummy);
      }
      else if (svg_tag_is(*other, SVF_contourGradient)) {
         parse_contourgradient(*other, (objGradientContour *)Gradient, dummy);
      }
      else if (svg_tag_is(*other, SVF_distalGradient)) {
         parse_distalgradient(*other, (objGradientDistal *)Gradient, dummy);
      }
      else if (svg_tag_is(*other, SVF_conicGradient)) {
         parse_conicgradient(*other, (objGradientConic *)Gradient, dummy);
      }
      else if ((svg_tag_is(*other, SVF_meshgradient)) and (Gradient->classID() IS CLASSID::GRADIENTMESH)) {
         parse_meshgradient(*other, (objGradientMesh *)Gradient, dummy);
      }
      else if ((svg_tag_is(*other, SVF_diffusionGradient)) and (Gradient->classID() IS CLASSID::GRADIENTDIFFUSION)) {
         parse_diffusiongradient(*other, (objGradientDiffusion *)Gradient, dummy);
      }
   }

   return true;
}

//********************************************************************************************************************

void svgState::parse_gradient_hrefs(const XTag &Tag, objGradient *Gradient, bool &ProcessStops) noexcept
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strhash(Tag.Attribs[a].Name);
      if ((attrib IS SVF_href) or (attrib IS SVF_xlink_href)) {
         if (not parse_gradient_href(val, Gradient)) ProcessStops = false;
      }
   }
}

//********************************************************************************************************************

void svgState::parse_gradient_defaults(kt::Log &Log, const XTag &Tag, objGradient *Gradient, uint32_t Attrib,
   const std::string &Name, const std::string &Value, std::string &ID) noexcept
{
   switch (Attrib) {
      case SVF_gradientUnits:
         break; // Already processed before interpreting coordinate values.

      case SVF_gradientTransform:
         Gradient->setTransform(Value);
         break;

      case SVF_spreadMethod:
         set_gradient_spread_method(Gradient, Value);
         break;

      case SVF_id:
         ID = Value;
         break;

      case SVF_href:
      case SVF_xlink_href:
         break;

      default:
         if (gradient_defaults(Self, Gradient, Attrib, Value) != ERR::Okay) {
            warn_unknown_gradient_attribute(Log, Tag, Name);
         }
         break;
   }
}

//********************************************************************************************************************

void svgState::set_gradient_stops(const XTag &Tag, objGradient *Gradient, bool ProcessStops) noexcept
{
   if (ProcessStops) {
      auto stops = process_gradient_stops(Tag);
      if (stops.size() >= 2) {
         std::span<GradientStop> span(stops.data(), stops.size());
         Gradient->setStops(span);
      }
   }
}

//********************************************************************************************************************

struct SVGMeshPoint {
   double x = 0;
   double y = 0;
};

struct SVGMeshPatchEdge {
   SVGMeshPoint p0, c0, c1, p1;
};

struct SVGMeshPatch {
   SVGMeshPatchEdge edge[4];
   FRGB corner[4];
};

static SVGMeshPatchEdge reverse_mesh_edge(const SVGMeshPatchEdge &Edge) noexcept
{
   return SVGMeshPatchEdge { Edge.p1, Edge.c1, Edge.c0, Edge.p0 };
}

static bool read_mesh_stop_path(const XTag &Tag, std::string &Path) noexcept
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("path", Tag.Attribs[a].Name)) {
         Path = Tag.Attribs[a].Value;
         return not Path.empty();
      }
   }
   return false;
}

// Mesh stop path grammar (SVG 2 mesh gradient proposal, svgwg.org): each <stop> 'path' consists of exactly one
// segment drawn with a single 'C', 'c', 'L' or 'l' command, starting from the previous stop's end point.  The
// closing stop of a fully specified patch may instead use 'Z'/'z', which draws a straight line back to the patch
// origin.  Multi-segment paths are not part of the grammar and are rejected.  Returns nullptr on success,
// otherwise a description of the fault.

static const char * parse_mesh_edge_path(const std::string &Path, const SVGMeshPoint &Start, SVGMeshPatchEdge &Edge,
   const SVGMeshPoint *Close = nullptr) noexcept
{
   Edge.p0 = Start;
   std::string_view scan(Path);
   auto skip_separators = [](std::string_view &Scan) noexcept {
      while ((not Scan.empty()) and ((Scan.front() <= 0x20) or (Scan.front() IS ','))) Scan.remove_prefix(1);
   };

   skip_separators(scan);
   if (scan.empty()) return "empty stop path";

   const char cmd = scan.front();
   scan.remove_prefix(1);

   std::vector<double> numbers;
   while (not scan.empty()) {
      skip_separators(scan);
      if (scan.empty()) break;

      char *end = nullptr;
      const double value = std::strtod(scan.data(), &end);
      if (end IS scan.data()) return "unrecognised character in stop path";
      if (not std::isfinite(value)) return "non-finite number in stop path";
      numbers.push_back(value);
      scan.remove_prefix(size_t(end - scan.data()));
   }

   auto straight_line = [&Edge](const SVGMeshPoint &From, const SVGMeshPoint &To) noexcept {
      const double dx = To.x - From.x;
      const double dy = To.y - From.y;
      Edge.c0 = SVGMeshPoint { From.x + (dx / 3.0), From.y + (dy / 3.0) };
      Edge.c1 = SVGMeshPoint { From.x + (dx * 2.0 / 3.0), From.y + (dy * 2.0 / 3.0) };
      Edge.p1 = To;
   };

   switch (cmd) {
      case 'C':
      case 'c':
         if (numbers.size() > 6) return "multi-segment stop paths are not supported";
         if (numbers.size() != 6) return "curve commands require six numbers";
         if (cmd IS 'C') {
            Edge.c0 = SVGMeshPoint { numbers[0], numbers[1] };
            Edge.c1 = SVGMeshPoint { numbers[2], numbers[3] };
            Edge.p1 = SVGMeshPoint { numbers[4], numbers[5] };
         }
         else {
            Edge.c0 = SVGMeshPoint { Start.x + numbers[0], Start.y + numbers[1] };
            Edge.c1 = SVGMeshPoint { Start.x + numbers[2], Start.y + numbers[3] };
            Edge.p1 = SVGMeshPoint { Start.x + numbers[4], Start.y + numbers[5] };
         }
         return nullptr;

      case 'L':
      case 'l':
         if (numbers.size() > 2) return "multi-segment stop paths are not supported";
         if (numbers.size() != 2) return "line commands require two numbers";
         if (cmd IS 'L') straight_line(Start, SVGMeshPoint { numbers[0], numbers[1] });
         else straight_line(Start, SVGMeshPoint { Start.x + numbers[0], Start.y + numbers[1] });
         return nullptr;

      case 'Z':
      case 'z':
         if (not Close) return "'Z' is only permitted on the closing stop of a fully specified patch";
         if (not numbers.empty()) return "'Z' does not accept numbers";
         straight_line(Start, *Close);
         return nullptr;

      default:
         return "unsupported stop path command";
   }
}

static MeshPatchRecord mesh_patch_record_from_patch(const SVGMeshPatch &Patch) noexcept
{
   MeshPatchRecord record = {};

   record.Top.StartX    = Patch.edge[0].p0.x; record.Top.StartY = Patch.edge[0].p0.y;
   record.Top.X1        = Patch.edge[0].c0.x; record.Top.Y1 = Patch.edge[0].c0.y;
   record.Top.X2        = Patch.edge[0].c1.x; record.Top.Y2 = Patch.edge[0].c1.y;
   record.Top.EndX      = Patch.edge[0].p1.x; record.Top.EndY = Patch.edge[0].p1.y;
   record.Right.StartX  = Patch.edge[1].p0.x; record.Right.StartY = Patch.edge[1].p0.y;
   record.Right.X1      = Patch.edge[1].c0.x; record.Right.Y1 = Patch.edge[1].c0.y;
   record.Right.X2      = Patch.edge[1].c1.x; record.Right.Y2 = Patch.edge[1].c1.y;
   record.Right.EndX    = Patch.edge[1].p1.x; record.Right.EndY = Patch.edge[1].p1.y;
   record.Bottom.StartX = Patch.edge[2].p0.x; record.Bottom.StartY = Patch.edge[2].p0.y;
   record.Bottom.X1     = Patch.edge[2].c0.x; record.Bottom.Y1 = Patch.edge[2].c0.y;
   record.Bottom.X2     = Patch.edge[2].c1.x; record.Bottom.Y2 = Patch.edge[2].c1.y;
   record.Bottom.EndX   = Patch.edge[2].p1.x; record.Bottom.EndY = Patch.edge[2].p1.y;
   record.Left.StartX   = Patch.edge[3].p0.x; record.Left.StartY = Patch.edge[3].p0.y;
   record.Left.X1       = Patch.edge[3].c0.x; record.Left.Y1 = Patch.edge[3].c0.y;
   record.Left.X2       = Patch.edge[3].c1.x; record.Left.Y2 = Patch.edge[3].c1.y;
   record.Left.EndX     = Patch.edge[3].p1.x; record.Left.EndY = Patch.edge[3].p1.y;
   record.TopLeft       = Patch.corner[0];
   record.TopRight      = Patch.corner[1];
   record.BottomRight   = Patch.corner[2];
   record.BottomLeft    = Patch.corner[3];

   return record;
}

// Snap the closing edge of a fully specified patch back to the patch origin so that numeric drift in authored
// files cannot leave the patch unclosed.  Warn when the gap is large relative to the patch extent, as that
// suggests an authoring error rather than rounding drift.

static void close_mesh_patch(kt::Log &Log, SVGMeshPatch &Patch) noexcept
{
   auto &closing = Patch.edge[3];
   const auto &origin = Patch.edge[0].p0;
   const double dx = closing.p1.x - origin.x;
   const double dy = closing.p1.y - origin.y;
   const double gap = std::sqrt((dx * dx) + (dy * dy));

   double extent = 0;
   for (auto &edge : Patch.edge) {
      extent = std::max({ extent, std::abs(edge.p1.x - edge.p0.x), std::abs(edge.p1.y - edge.p0.y) });
   }

   if (gap > std::max(extent, 1.0) * 0.001) {
      Log.warning("Mesh patch closing edge misses the patch origin by %g units; snapping to close the patch.", gap);
   }

   closing.p1 = origin;
}

static bool read_mesh_patch_origin(const XTag &Tag, SVGMeshPoint &Origin) noexcept
{
   bool found_x = false;
   bool found_y = false;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &name = Tag.Attribs[a].Name;
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      if (iequals("kotuku:x", name)) {
         Origin.x = svtonum<double>(value);
         found_x = true;
      }
      else if (iequals("kotuku:y", name)) {
         Origin.y = svtonum<double>(value);
         found_y = true;
      }
   }

   return found_x and found_y;
}

void svgState::parse_meshgradient(const XTag &Tag, objGradientMesh *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = false;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   double start_x = 0;
   double start_y = 0;
   bool origin_set = false;
   GMT mode = GMT::UNDEFINED;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         // Percentages scale to 0 - 1.0, matching bounding-box semantics (cf. parse_units()).  Under userspace
         // units the viewport size is not known at parse time, so a percentage resolves to the same fraction.
         case SVF_x: { auto v = std::string_view(val); start_x = read_unit(v); origin_set = true; break; }
         case SVF_y: { auto v = std::string_view(val); start_y = read_unit(v); origin_set = true; break; }
         case SVF_type:
            // A tag without a type attribute leaves any bicubic mode inherited via href intact.
            if (iequals("bicubic", val)) { mode = GMT::BICUBIC; }
            else if ((iequals("bilinear", val)) or (iequals("Coons", val))) { mode = GMT::LINEAR; }
            else log.warning("Mesh gradient type '%s' is not supported; using bilinear Coons patches.", val.c_str());
            break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   // The x/y origin only anchors locally defined rows.  A referencing tag without rows of its own cannot
   // re-anchor patches inherited via href because their geometry was baked when the href target was parsed,
   // so x/y is documented as ignored in that case.

   if (origin_set) {
      bool has_rows = false;
      for (auto &row_tag : Tag.Children) {
         if (svg_tag_hash(row_tag) IS SVF_meshrow) { has_rows = true; break; }
      }
      if (not has_rows) log.warning("x/y on a <meshgradient> without its own rows is ignored @ line %d.", Tag.LineNo);
   }

   std::vector<MeshPatchRecord> records;
   std::vector<SVGMeshPatch> previous_row;
   std::vector<SVGMeshPatch> current_row;
   int rows = 0;
   int columns = 0;
   bool rectangular = true;

   // Read one stop: parse its edge path and resolve its colour.  Returns nullptr on success or a fault description.

   auto read_stop = [&](const XTag &Stop, const SVGMeshPoint &Start, SVGMeshPatchEdge &Edge, FRGB &Colour,
         const SVGMeshPoint *Close) noexcept -> const char * {
      std::string path;
      if (not read_mesh_stop_path(Stop, path)) return "missing or empty stop path";
      if (auto fault = parse_mesh_edge_path(path, Start, Edge, Close)) return fault;
      Colour = resolve_stop_colour(Stop);
      return nullptr;
   };

   // Parse one <meshpatch>, appending it to current_row and records.  Returns nullptr on success or a fault
   // description.  Any fault aborts the entire gradient - a silently dropped patch would shift the remaining
   // patches left by one column and sew the wrong shared edges together.

   auto parse_patch = [&](const XTag &PatchTag) noexcept -> const char * {
      SVGMeshPatch patch = {};
      std::vector<const XTag *> stops;
      for (auto &stop_tag : PatchTag.Children) {
         if (svg_tag_hash(stop_tag) IS kt::strhash("stop")) stops.push_back(&stop_tag);
      }

      const int col = int(current_row.size());

      SVGMeshPoint independent_origin;
      if (read_mesh_patch_origin(PatchTag, independent_origin)) {
         if (stops.size() != 4) return "independent patches require exactly four stops";

         patch.edge[0].p0 = independent_origin;
         if (auto fault = read_stop(*stops[0], patch.edge[0].p0, patch.edge[0], patch.corner[1], nullptr)) return fault;
         patch.edge[1].p0 = patch.edge[0].p1;
         if (auto fault = read_stop(*stops[1], patch.edge[1].p0, patch.edge[1], patch.corner[2], nullptr)) return fault;
         patch.edge[2].p0 = patch.edge[1].p1;
         if (auto fault = read_stop(*stops[2], patch.edge[2].p0, patch.edge[2], patch.corner[3], nullptr)) return fault;
         patch.edge[3].p0 = patch.edge[2].p1;
         if (auto fault = read_stop(*stops[3], patch.edge[3].p0, patch.edge[3], patch.corner[0], &patch.edge[0].p0)) return fault;
         close_mesh_patch(log, patch);

         current_row.push_back(patch);
         records.push_back(mesh_patch_record_from_patch(patch));
         return nullptr;
      }

      // Expected stop counts under the shared-edge grammar: 4 for the first patch of the first row, 3 for
      // subsequent first-row patches and for the first patch of later rows, 2 otherwise.

      const size_t expected = (rows IS 0) ? ((col IS 0) ? 4 : 3) : ((col IS 0) ? 3 : 2);
      if (stops.size() != expected) return "unexpected number of stops for the patch's grid position";

      int stop_index = 0;
      if (rows IS 0) {
         if (col IS 0) patch.edge[0].p0 = SVGMeshPoint { start_x, start_y };
         else {
            auto &prev = current_row[size_t(col - 1)];
            patch.edge[3] = reverse_mesh_edge(prev.edge[1]);
            patch.edge[0].p0 = patch.edge[3].p1;
            patch.corner[0] = prev.corner[1];
            patch.corner[3] = prev.corner[2];
         }

         if (auto fault = read_stop(*stops[size_t(stop_index++)], patch.edge[0].p0,
            patch.edge[0], patch.corner[1], nullptr)) return fault;
      }
      else {
         if (col >= int(previous_row.size())) return "row contains more patches than the row above";
         auto &above = previous_row[size_t(col)];
         patch.edge[0] = reverse_mesh_edge(above.edge[2]);
         patch.corner[0] = above.corner[3];
         patch.corner[1] = above.corner[2];
      }

      patch.edge[1].p0 = patch.edge[0].p1;
      if (auto fault = read_stop(*stops[size_t(stop_index++)], patch.edge[1].p0,
         patch.edge[1], patch.corner[2], nullptr)) return fault;

      patch.edge[2].p0 = patch.edge[1].p1;
      if (auto fault = read_stop(*stops[size_t(stop_index++)], patch.edge[2].p0,
         patch.edge[2], patch.corner[3], nullptr)) return fault;

      if (col IS 0) {
         patch.edge[3].p0 = patch.edge[2].p1;
         if (auto fault = read_stop(*stops[size_t(stop_index++)], patch.edge[3].p0,
            patch.edge[3], patch.corner[0], &patch.edge[0].p0)) return fault;
         if (rows IS 0) close_mesh_patch(log, patch);
         else {
            patch.edge[3].p1 = patch.edge[0].p0;
            patch.corner[0] = previous_row[0].corner[3];
         }
      }
      else {
         auto &prev = current_row[size_t(col - 1)];
         if (rows != 0) patch.edge[3] = reverse_mesh_edge(prev.edge[1]);
         patch.edge[2].p1 = patch.edge[3].p0;
         patch.corner[3] = prev.corner[2];
         patch.corner[0] = prev.corner[1];
      }

      current_row.push_back(patch);
      records.push_back(mesh_patch_record_from_patch(patch));
      return nullptr;
   };

   const char *fault = nullptr;
   for (auto &row_tag : Tag.Children) {
      if (svg_tag_hash(row_tag) != SVF_meshrow) continue;

      current_row.clear();
      for (auto &patch_tag : row_tag.Children) {
         if (svg_tag_hash(patch_tag) != SVF_meshpatch) continue;

         if ((fault = parse_patch(patch_tag))) {
            log.warning("Discarding <meshgradient> @ line %d; row %d, patch %d: %s",
               patch_tag.LineNo, rows + 1, int(current_row.size()) + 1, fault);
            break;
         }
      }
      if (fault) break;

      if (not current_row.empty()) {
         if (columns IS 0) columns = int(current_row.size());
         else if (columns != int(current_row.size())) rectangular = false;

         previous_row = current_row;
         rows++;
      }
   }

   if (fault) {
      // A malformed patch invalidates the entire mesh, matching SVG's in-error paint server behaviour.  Clearing
      // the patches also discards any records inherited via href.
      Gradient->setPatches(std::span<const MeshPatchRecord>());
      return;
   }

   if (not records.empty()) {
      std::span<MeshPatchRecord> span(records.data(), records.size());
      Gradient->setPatches(span);

      if ((rectangular) and (rows > 0) and (columns > 0)) {
         Gradient->setRows(rows);
         Gradient->setColumns(columns);
      }
   }

   if (mode != GMT::UNDEFINED) Gradient->setMode(mode);
}

//********************************************************************************************************************

ERR svgState::proc_meshgradient(const XTag &Tag) noexcept
{
   objGradientMesh *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag);

   if (!NewObject(CLASSID::GRADIENTMESH, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGMeshGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);

      state.parse_meshgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id.c_str());
            track_object(Self, gradient);
            return Self->Scene->addDef(id.c_str(), gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************
// Diffusion gradient parsing.  <diffusionGradient> is a Kotuku extension (there is no W3C element for diffusion
// curves).  Each <curve> child carries a single-segment path in 'd' ("M x,y C x1,y1 x2,y2 x,y" or "M x,y L x,y")
// plus per-side colours in 'left-start', 'left-end', 'right-start' and 'right-end'.

static const char * parse_diffusion_curve_path(const std::string &Path, MeshControl &Curve) noexcept
{
   std::string_view scan(Path);
   auto skip_separators = [](std::string_view &Scan) noexcept {
      while ((not Scan.empty()) and ((Scan.front() <= 0x20) or (Scan.front() IS ','))) Scan.remove_prefix(1);
   };

   auto read_numbers = [&](int Count, double *Out) noexcept -> bool {
      for (int i=0; i < Count; i++) {
         skip_separators(scan);
         if (scan.empty()) return false;
         char *end = nullptr;
         const double value = std::strtod(scan.data(), &end);
         if ((end IS scan.data()) or (not std::isfinite(value))) return false;
         Out[i] = value;
         scan.remove_prefix(size_t(end - scan.data()));
      }
      return true;
   };

   skip_separators(scan);
   if ((scan.empty()) or (scan.front() != 'M')) return "curve path must start with 'M'";
   scan.remove_prefix(1);

   double start[2];
   if (not read_numbers(2, start)) return "unreadable start point";
   Curve.StartX = start[0];
   Curve.StartY = start[1];

   skip_separators(scan);
   if (scan.empty()) return "curve path requires a 'C' or 'L' segment";

   const char cmd = scan.front();
   scan.remove_prefix(1);

   if (cmd IS 'C') {
      double numbers[6];
      if (not read_numbers(6, numbers)) return "curve commands require six numbers";
      Curve.X1 = numbers[0];   Curve.Y1 = numbers[1];
      Curve.X2 = numbers[2];   Curve.Y2 = numbers[3];
      Curve.EndX = numbers[4]; Curve.EndY = numbers[5];
   }
   else if (cmd IS 'L') {
      double numbers[2];
      if (not read_numbers(2, numbers)) return "line commands require two numbers";
      const double dx = numbers[0] - Curve.StartX;
      const double dy = numbers[1] - Curve.StartY;
      Curve.X1 = Curve.StartX + (dx / 3.0);
      Curve.Y1 = Curve.StartY + (dy / 3.0);
      Curve.X2 = Curve.StartX + (dx * 2.0 / 3.0);
      Curve.Y2 = Curve.StartY + (dy * 2.0 / 3.0);
      Curve.EndX = numbers[0];
      Curve.EndY = numbers[1];
   }
   else return "unsupported curve path command";

   skip_separators(scan);
   if (not scan.empty()) return "multi-segment curve paths are not supported";
   return nullptr;
}

void svgState::parse_diffusiongradient(const XTag &Tag, objGradientDiffusion *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = false;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strhash(Tag.Attribs[a].Name);
      parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
   }

   auto read_colour = [&](const XTag &Curve, CSTRING Name, FRGB &Colour) noexcept -> bool {
      for (int a=1; a < std::ssize(Curve.Attribs); a++) {
         if (iequals(Name, Curve.Attribs[a].Name)) {
            VectorPainter painter;
            if (vec::ReadPainter(Self->Scene, Curve.Attribs[a].Value, &painter, nullptr) != ERR::Okay) {
               log.warning("Ignoring invalid diffusion curve colour '%s'.", Curve.Attribs[a].Value.c_str());
               return false;
            }
            Colour = painter.Colour;
            return true;
         }
      }
      return false;
   };

   std::vector<DiffusionCurveRecord> records;
   const char *fault = nullptr;

   for (auto &child : Tag.Children) {
      if (svg_tag_hash(child) != kt::strhash("curve")) continue;

      DiffusionCurveRecord record = {};

      std::string path;
      for (int a=1; a < std::ssize(child.Attribs); a++) {
         if (iequals("d", child.Attribs[a].Name)) { path = child.Attribs[a].Value; break; }
      }

      if (path.empty()) { fault = "missing or empty 'd' attribute"; }
      else fault = parse_diffusion_curve_path(path, record.Curve);

      if (fault) {
         log.warning("Discarding <diffusionGradient> @ line %d: %s", child.LineNo, fault);
         break;
      }

      // A missing end colour inherits the matching start colour; a missing start colour defaults to opaque black.

      if (not read_colour(child, "left-start", record.LeftStart)) record.LeftStart = FRGB(0, 0, 0, 1);
      if (not read_colour(child, "left-end", record.LeftEnd)) record.LeftEnd = record.LeftStart;
      if (not read_colour(child, "right-start", record.RightStart)) record.RightStart = FRGB(0, 0, 0, 1);
      if (not read_colour(child, "right-end", record.RightEnd)) record.RightEnd = record.RightStart;

      records.push_back(record);
   }

   if (fault) {
      // A malformed curve invalidates the whole gradient, matching SVG's in-error paint server behaviour.
      Gradient->setCurves(std::span<const DiffusionCurveRecord>());
      return;
   }

   if (not records.empty()) {
      std::span<DiffusionCurveRecord> span(records.data(), records.size());
      Gradient->setCurves(span);
   }
}

//********************************************************************************************************************

ERR svgState::proc_diffusiongradient(const XTag &Tag) noexcept
{
   objGradientDiffusion *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag);

   if (!NewObject(CLASSID::GRADIENTDIFFUSION, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGDiffusionGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);

      state.parse_diffusiongradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id.c_str());
            track_object(Self, gradient);
            return Self->Scene->addDef(id.c_str(), gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************

void svgState::parse_lineargradient(const XTag &Tag, objGradientLinear *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_x1: Gradient->setX1(parse_units(val, Gradient->Units)); break;
         case SVF_y1: Gradient->setY1(parse_units(val, Gradient->Units)); break;
         case SVF_x2: Gradient->setX2(parse_units(val, Gradient->Units)); break;
         case SVF_y2: Gradient->setY2(parse_units(val, Gradient->Units)); break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************

void svgState::parse_radialgradient(const XTag &Tag, objGradientRadial *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;
      log.trace("Processing radial gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_cx: Gradient->setCX(parse_units(val, Gradient->Units)); break;
         case SVF_cy: Gradient->setCY(parse_units(val, Gradient->Units)); break;
         case SVF_fx: Gradient->setFX(parse_units(val, Gradient->Units)); break;
         case SVF_fy: Gradient->setFY(parse_units(val, Gradient->Units)); break;
         case SVF_r:  Gradient->setRadius(parse_units(val, Gradient->Units)); break;

         case SVF_focalPoint: {
            if (iequals("unbound", val)) Gradient->setContainFocal(0);
            break;
         }

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************

void svgState::parse_diamondgradient(const XTag &Tag, objGradientDiamond *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing diamond gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_cx: Gradient->setCX(parse_units(val, Gradient->Units)); break;
         case SVF_cy: Gradient->setCY(parse_units(val, Gradient->Units)); break;
         case SVF_r:  Gradient->setRadius(parse_units(val, Gradient->Units)); break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************

void svgState::parse_contourgradient(const XTag &Tag, objGradientContour *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing contour gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         // Floor and Multiplier adjust the colour ramp bias and scale.
         case SVF_floor:
         case SVF_x1: Gradient->setFloor(svtonum<double>(val)); break;
         case SVF_multiplier:
         case SVF_x2: Gradient->setMultiplier(svtonum<double>(val)); break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************

ERR svgState::proc_lineargradient(const XTag &Tag) noexcept
{
   objGradientLinear *gradient;

   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTLINEAR, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGLinearGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);
      gradient->setX1(Unit(0));
      gradient->setY1(Unit(0));
      gradient->setX2(Unit(1.0, FD_SCALED));
      gradient->setY2(Unit(0));

      state.parse_lineargradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id.c_str());
            track_object(Self, gradient);
            return Self->Scene->addDef(id.c_str(), gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************

ERR svgState::proc_radialgradient(const XTag &Tag) noexcept
{
   objGradientRadial *gradient;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTRADIAL, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGRadialGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);
      gradient->setCX(Unit(0.5, FD_SCALED));
      gradient->setCY(Unit(0.5, FD_SCALED));
      gradient->setRadius(Unit(0.5, FD_SCALED));
      gradient->setContainFocal(1); // Enforce SVG limits on focal point, can be overridden with focal="unbound"

      std::string id;
      state.parse_radialgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id.c_str());
            track_object(Self, gradient);
            return Self->Scene->addDef(id.c_str(), gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************

ERR svgState::proc_diamondgradient(const XTag &Tag) noexcept
{
   objGradientDiamond *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTDIAMOND, &gradient)) {
      SetOwner(gradient, Self->Scene);

      gradient->setName("SVGDiamondGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);
      gradient->setCX(Unit(0.5, FD_SCALED));
      gradient->setCY(Unit(0.5, FD_SCALED));
      gradient->setRadius(Unit(0.5, FD_SCALED));

      state.parse_diamondgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id);
            track_object(Self, gradient);
            return Self->Scene->addDef(id, gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************
// NB: Contour gradients are not part of the SVG standard.

ERR svgState::proc_contourgradient(const XTag &Tag) noexcept
{
   objGradientContour *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTCONTOUR, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGContourGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);

      state.parse_contourgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id);
            track_object(Self, gradient);
            return Self->Scene->addDef(id, gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************
// NB: Distal gradients are not part of the SVG standard.

void svgState::parse_distalgradient(const XTag &Tag, objGradientDistal *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing distal gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         // Floor and Multiplier adjust the colour ramp bias and scale, as with contour gradients.
         case SVF_floor:
         case SVF_x1: Gradient->setFloor(svtonum<double>(val)); break;
         case SVF_multiplier:
         case SVF_x2: Gradient->setMultiplier(svtonum<double>(val)); break;
         // Radius controls the exterior margin (in path units) around the path bounds.
         case SVF_radius: Gradient->setRadius(Unit(svtonum<double>(val))); break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************
// NB: Distal gradients are not part of the SVG standard.

ERR svgState::proc_distalgradient(const XTag &Tag) noexcept
{
   objGradientDistal *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTDISTAL, &gradient)) {
      SetOwner(gradient, Self->Scene);
      gradient->setName("SVGDistalGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);

      state.parse_distalgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id);
            track_object(Self, gradient);
            return Self->Scene->addDef(id, gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}

//********************************************************************************************************************

void svgState::parse_conicgradient(const XTag &Tag, objGradientConic *Gradient, std::string &ID) noexcept
{
   kt::Log log(__FUNCTION__);

   bool process_stops = true;
   parse_gradient_hrefs(Tag, Gradient, process_stops);
   set_gradient_units(Tag, Gradient);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing conic gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_cx: Gradient->setCX(parse_units(val, Gradient->Units)); break;
         case SVF_cy: Gradient->setCY(parse_units(val, Gradient->Units)); break;
         case SVF_r:  Gradient->setRadius(parse_units(val, Gradient->Units)); break;
         case SVF_span: Gradient->setSpan(svtonum<double>(val)); break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   set_gradient_stops(Tag, Gradient, process_stops);
}

//********************************************************************************************************************

ERR svgState::proc_conicgradient(const XTag &Tag) noexcept
{
   objGradientConic *gradient;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTCONIC, &gradient)) {
      SetOwner(gradient, Self->Scene);

      gradient->setName("SVGConicGrad");
      gradient->setUnits(VUNIT::BOUNDING_BOX);
      gradient->setCX(Unit(0.5, FD_SCALED));
      gradient->setCY(Unit(0.5, FD_SCALED));
      gradient->setRadius(Unit(0.5, FD_SCALED));

      std::string id;
      state.parse_conicgradient(Tag, gradient, id);

      if (!InitObject(gradient)) {
         if (!id.empty()) {
            SetName(gradient, id.c_str());
            track_object(Self, gradient);
            return Self->Scene->addDef(id.c_str(), gradient);
         }
         else return ERR::Okay;
      }
      else return ERR::Init;
   }
   else return ERR::NewObject;
}
