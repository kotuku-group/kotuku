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

const std::vector<GradientStop> svgState::process_gradient_stops(const XTag &Tag) noexcept
{
   kt::Log log(__FUNCTION__);

   log.traceBranch();

   double last_stop = 0;
   std::vector<GradientStop> stops;
   for (auto &scan : Tag.Children) {
      if (svg_tag_hash(scan) IS kt::strhash("stop")) {
         GradientStop stop;
         double stop_opacity = 1.0;
         stop.Offset = 0;
         stop.RGB.Red   = 0;
         stop.RGB.Green = 0;
         stop.RGB.Blue  = 0;
         stop.RGB.Alpha = 1.0;

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
      if (stops.size() >= 2) Gradient->setStops(stops);
   }
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
