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
         if (iequals("auto", Value)) Gradient->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("sRGB", Value)) Gradient->setColourSpace(VCS::SRGB);
         else if (iequals("linearRGB", Value)) Gradient->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("inherit", Value)) Gradient->setColourSpace(VCS::INHERIT);
         return ERR::Okay;

      case SVF_easing:
         switch (strhash(Value)) {
            case SVF_linear: Gradient->setEasing(GEZ::LINEAR); break;
            case SVF_in: Gradient->setEasing(GEZ::IN); break;
            case SVF_out: Gradient->setEasing(GEZ::OUT); break;
            case SVF_inOut: Gradient->setEasing(GEZ::IN_OUT); break;
            case SVF_cubicIn: Gradient->setEasing(GEZ::CUBIC_IN); break;
            case SVF_cubicOut: Gradient->setEasing(GEZ::CUBIC_OUT); break;
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
         double stop_opacity = 1.0;
         stop.Offset = 0;
         stop.RGB = FRGB(0,0,0,1);

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
            else if (iequals("stop-color", name)) {
               if (iequals("inherit", value)) {
                  VectorPainter painter;
                  vec::ReadPainter(Self->Scene, m_stop_color, &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
               else if (iequals("currentColor", value)) {
                  VectorPainter painter;
                  vec::ReadPainter(Self->Scene, m_color, &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
               else {
                  VectorPainter painter;
                  vec::ReadPainter(Self->Scene, value, &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
            }
            else if (iequals("stop-opacity", name)) {
               if (iequals("inherit", value)) {
                  stop_opacity = m_stop_opacity;
               }
               else stop_opacity = svtonum<double>(value);
            }
            else if (iequals("id", name)) {
               log.trace("Use of id attribute in <stop/> ignored.");
            }
            else log.warning("Unable to process stop attribute '%s'", name.c_str());
         }

         stop.RGB.Alpha = ((double)stop.RGB.Alpha) * stop_opacity;

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
         if (!Gradient->setColourMap(cmap)) return false;
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

static bool read_mesh_stop_colour(extSVG *Self, const XTag &Tag, FRGB &Colour) noexcept
{
   Colour = FRGB(0,0,0,1);

   double stop_opacity = 1.0;
   bool found_colour = false;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &name = Tag.Attribs[a].Name;
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      if (iequals("stop-color", name)) {
         VectorPainter painter;
         vec::ReadPainter(Self->Scene, value, &painter, nullptr);
         Colour = painter.Colour;
         found_colour = true;
      }
      else if (iequals("stop-opacity", name)) stop_opacity = svtonum<double>(value);
   }

   Colour.Alpha = ((double)Colour.Alpha) * stop_opacity;
   return found_colour;
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

static bool parse_mesh_edge_path(const std::string &Path, const SVGMeshPoint &Start, SVGMeshPatchEdge &Edge) noexcept
{
   Edge.p0 = Start;
   std::string_view scan(Path);
   auto skip_separators = [](std::string_view &Scan) noexcept {
      while ((not Scan.empty()) and ((Scan.front() <= 0x20) or (Scan.front() IS ','))) Scan.remove_prefix(1);
   };

   if (scan.empty()) return false;
   skip_separators(scan);
   if (scan.empty()) return false;

   const char cmd = scan.front();
   scan.remove_prefix(1);

   std::vector<double> numbers;
   while (not scan.empty()) {
      skip_separators(scan);
      if (scan.empty()) break;

      char *end = nullptr;
      const double value = std::strtod(scan.data(), &end);
      if (end IS scan.data()) return false;
      numbers.push_back(value);
      scan.remove_prefix(size_t(end - scan.data()));
   }

   switch (cmd) {
      case 'C':
         if (numbers.size() < 6) return false;
         Edge.c0 = SVGMeshPoint { numbers[0], numbers[1] };
         Edge.c1 = SVGMeshPoint { numbers[2], numbers[3] };
         Edge.p1 = SVGMeshPoint { numbers[4], numbers[5] };
         return true;

      case 'c':
         if (numbers.size() < 6) return false;
         Edge.c0 = SVGMeshPoint { Start.x + numbers[0], Start.y + numbers[1] };
         Edge.c1 = SVGMeshPoint { Start.x + numbers[2], Start.y + numbers[3] };
         Edge.p1 = SVGMeshPoint { Start.x + numbers[4], Start.y + numbers[5] };
         return true;

      case 'L': {
         if (numbers.size() < 2) return false;
         Edge.p1 = SVGMeshPoint { numbers[0], numbers[1] };
         const double dx = Edge.p1.x - Start.x;
         const double dy = Edge.p1.y - Start.y;
         Edge.c0 = SVGMeshPoint { Start.x + (dx / 3.0), Start.y + (dy / 3.0) };
         Edge.c1 = SVGMeshPoint { Start.x + (dx * 2.0 / 3.0), Start.y + (dy * 2.0 / 3.0) };
         return true;
      }

      case 'l': {
         if (numbers.size() < 2) return false;
         Edge.p1 = SVGMeshPoint { Start.x + numbers[0], Start.y + numbers[1] };
         const double dx = Edge.p1.x - Start.x;
         const double dy = Edge.p1.y - Start.y;
         Edge.c0 = SVGMeshPoint { Start.x + (dx / 3.0), Start.y + (dy / 3.0) };
         Edge.c1 = SVGMeshPoint { Start.x + (dx * 2.0 / 3.0), Start.y + (dy * 2.0 / 3.0) };
         return true;
      }

      default:
         return false;
   }
}

static MeshPatchRecord mesh_patch_record_from_patch(const SVGMeshPatch &Patch) noexcept
{
   MeshPatchRecord record = {};

   record.TopP0X = Patch.edge[0].p0.x;       record.TopP0Y = Patch.edge[0].p0.y;
   record.TopC0X = Patch.edge[0].c0.x;       record.TopC0Y = Patch.edge[0].c0.y;
   record.TopC1X = Patch.edge[0].c1.x;       record.TopC1Y = Patch.edge[0].c1.y;
   record.TopP1X = Patch.edge[0].p1.x;       record.TopP1Y = Patch.edge[0].p1.y;
   record.RightP0X = Patch.edge[1].p0.x;     record.RightP0Y = Patch.edge[1].p0.y;
   record.RightC0X = Patch.edge[1].c0.x;     record.RightC0Y = Patch.edge[1].c0.y;
   record.RightC1X = Patch.edge[1].c1.x;     record.RightC1Y = Patch.edge[1].c1.y;
   record.RightP1X = Patch.edge[1].p1.x;     record.RightP1Y = Patch.edge[1].p1.y;
   record.BottomP0X = Patch.edge[2].p0.x;    record.BottomP0Y = Patch.edge[2].p0.y;
   record.BottomC0X = Patch.edge[2].c0.x;    record.BottomC0Y = Patch.edge[2].c0.y;
   record.BottomC1X = Patch.edge[2].c1.x;    record.BottomC1Y = Patch.edge[2].c1.y;
   record.BottomP1X = Patch.edge[2].p1.x;    record.BottomP1Y = Patch.edge[2].p1.y;
   record.LeftP0X = Patch.edge[3].p0.x;      record.LeftP0Y = Patch.edge[3].p0.y;
   record.LeftC0X = Patch.edge[3].c0.x;      record.LeftC0Y = Patch.edge[3].c0.y;
   record.LeftC1X = Patch.edge[3].c1.x;      record.LeftC1Y = Patch.edge[3].c1.y;
   record.LeftP1X = Patch.edge[3].p1.x;      record.LeftP1Y = Patch.edge[3].p1.y;
   record.TopLeft = Patch.corner[0];
   record.TopRight = Patch.corner[1];
   record.BottomRight = Patch.corner[2];
   record.BottomLeft = Patch.corner[3];

   return record;
}

static bool read_mesh_patch_stop(extSVG *Self, const XTag &Stop, const SVGMeshPoint &Start, SVGMeshPatchEdge &Edge,
   FRGB &Colour) noexcept
{
   std::string path;
   if (!read_mesh_stop_path(Stop, path)) return false;
   if (!parse_mesh_edge_path(path, Start, Edge)) return false;
   read_mesh_stop_colour(Self, Stop, Colour);
   return true;
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

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strhash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_x: start_x = svtonum<double>(val); break;
         case SVF_y: start_y = svtonum<double>(val); break;
         case SVF_type:
            if ((not iequals("bilinear", val)) and (not iequals("Coons", val))) {
               log.warning("Mesh gradient type '%s' is not supported; using bilinear Coons patches.", val.c_str());
            }
            break;

         default:
            parse_gradient_defaults(log, Tag, Gradient, attrib, Tag.Attribs[a].Name, val, ID);
            break;
      }
   }

   std::vector<MeshPatchRecord> records;
   std::vector<SVGMeshPatch> previous_row;
   std::vector<SVGMeshPatch> current_row;
   int rows = 0;

   for (auto &row_tag : Tag.Children) {
      if (svg_tag_hash(row_tag) != SVF_meshrow) continue;

      current_row.clear();
      int col = 0;
      for (auto &patch_tag : row_tag.Children) {
         if (svg_tag_hash(patch_tag) != SVF_meshpatch) continue;

         SVGMeshPatch patch = {};
         std::vector<const XTag *> stops;
         for (auto &stop_tag : patch_tag.Children) {
            if (svg_tag_hash(stop_tag) IS kt::strhash("stop")) stops.push_back(&stop_tag);
         }

         SVGMeshPoint independent_origin;
         if (read_mesh_patch_origin(patch_tag, independent_origin)) {
            if (stops.size() < 4) continue;

            patch.edge[0].p0 = independent_origin;
            int stop_index = 0;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[0].p0,
               patch.edge[0], patch.corner[1])) continue;

            patch.edge[1].p0 = patch.edge[0].p1;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[1].p0,
               patch.edge[1], patch.corner[2])) continue;

            patch.edge[2].p0 = patch.edge[1].p1;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[2].p0,
               patch.edge[2], patch.corner[3])) continue;

            patch.edge[3].p0 = patch.edge[2].p1;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[3].p0,
               patch.edge[3], patch.corner[0])) continue;

            current_row.push_back(patch);
            records.push_back(mesh_patch_record_from_patch(patch));
            col++;
            continue;
         }

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

            if (stop_index >= int(stops.size())) continue;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[0].p0,
               patch.edge[0], patch.corner[1])) continue;
         }
         else {
            if (col >= int(previous_row.size())) continue;
            auto &above = previous_row[size_t(col)];
            patch.edge[0] = reverse_mesh_edge(above.edge[2]);
            patch.corner[0] = above.corner[3];
            patch.corner[1] = above.corner[2];
         }

         patch.edge[1].p0 = patch.edge[0].p1;
         if (stop_index >= int(stops.size())) continue;
         if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[1].p0,
            patch.edge[1], patch.corner[2])) continue;

         patch.edge[2].p0 = patch.edge[1].p1;
         if (stop_index >= int(stops.size())) continue;
         if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[2].p0,
            patch.edge[2], patch.corner[3])) continue;

         if (col IS 0) {
            patch.edge[3].p0 = patch.edge[2].p1;
            if (stop_index >= int(stops.size())) continue;
            if (!read_mesh_patch_stop(Self, *stops[size_t(stop_index++)], patch.edge[3].p0,
               patch.edge[3], patch.corner[0])) continue;
            if (rows != 0) {
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
         col++;
      }

      if (not current_row.empty()) {
         previous_row = current_row;
         rows++;
      }
   }

   if (not records.empty()) {
      std::span<MeshPatchRecord> span(records.data(), records.size());
      Gradient->setPatches(span);

   }
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
      gradient->setFields(fl::Name("SVGMeshGrad"), fl::Units(VUNIT::BOUNDING_BOX));

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
         case SVF_x1: set_double_units(Gradient, FID_X1, val, Gradient->Units); break;
         case SVF_y1: set_double_units(Gradient, FID_Y1, val, Gradient->Units); break;
         case SVF_x2: set_double_units(Gradient, FID_X2, val, Gradient->Units); break;
         case SVF_y2: set_double_units(Gradient, FID_Y2, val, Gradient->Units); break;

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
         case SVF_cx: set_double_units(Gradient, FID_CX, val, Gradient->Units); break;
         case SVF_cy: set_double_units(Gradient, FID_CY, val, Gradient->Units); break;
         case SVF_fx: set_double_units(Gradient, FID_FX, val, Gradient->Units); break;
         case SVF_fy: set_double_units(Gradient, FID_FY, val, Gradient->Units); break;
         case SVF_r:  set_double_units(Gradient, FID_Radius, val, Gradient->Units); break;

         case SVF_focalPoint: {
            if (iequals("unbound", val)) Gradient->set(kt::fieldhash("ContainFocal"), 0);
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
         case SVF_cx: set_double_units(Gradient, FID_CX, val, Gradient->Units); break;
         case SVF_cy: set_double_units(Gradient, FID_CY, val, Gradient->Units); break;
         case SVF_r:  set_double_units(Gradient, FID_Radius, val, Gradient->Units); break;

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
         case SVF_floor: set_double_units(Gradient, FID_Floor, val, Gradient->Units); break;
         case SVF_multiplier: set_double_units(Gradient, FID_Multiplier, val, Gradient->Units); break;
         case SVF_x1: set_double_units(Gradient, FID_Floor, val, Gradient->Units); break;
         case SVF_x2: set_double_units(Gradient, FID_Multiplier, val, Gradient->Units); break;

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
      gradient->setFields(
         fl::Name("SVGLinearGrad"),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::X1(0.0),
         fl::Y1(0.0),
         fl::X2(SCALE(1.0)),
         fl::Y2(0.0));

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
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (!NewObject(CLASSID::GRADIENTRADIAL, &gradient)) {
      SetOwner(gradient, Self->Scene);

      gradient->setFields(fl::Name("SVGRadialGrad"),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::CX(SCALE(0.5)), fl::CY(SCALE(0.5)),
         fl::Radius(SCALE(0.5)),
         FieldValue("ContainFocal", 1)); // Enforce SVG limits on focal point, can be overridden with focal="unbound"

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

      gradient->setFields(fl::Name("SVGDiamondGrad"), fl::Units(VUNIT::BOUNDING_BOX),
         fl::CX(SCALE(0.5)), fl::CY(SCALE(0.5)), fl::Radius(SCALE(0.5)));

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
      gradient->setFields(fl::Name("SVGContourGrad"), fl::Units(VUNIT::BOUNDING_BOX));

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
         case SVF_floor: set_double_units(Gradient, FID_Floor, val, Gradient->Units); break;
         case SVF_multiplier: set_double_units(Gradient, FID_Multiplier, val, Gradient->Units); break;
         case SVF_x1: set_double_units(Gradient, FID_Floor, val, Gradient->Units); break;
         case SVF_x2: set_double_units(Gradient, FID_Multiplier, val, Gradient->Units); break;
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
      gradient->setFields(fl::Name("SVGDistalGrad"), fl::Units(VUNIT::BOUNDING_BOX));

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
         case SVF_cx: set_double_units(Gradient, FID_CX, val, Gradient->Units); break;
         case SVF_cy: set_double_units(Gradient, FID_CY, val, Gradient->Units); break;
         case SVF_r:  set_double_units(Gradient, FID_Radius, val, Gradient->Units); break;
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

      gradient->setFields(fl::Name("SVGConicGrad"), fl::Units(VUNIT::BOUNDING_BOX),
         fl::CX(SCALE(0.5)), fl::CY(SCALE(0.5)), fl::Radius(SCALE(0.5)));

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
