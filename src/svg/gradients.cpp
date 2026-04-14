// Kōtuku SVG extensions:
//   It is possible to reference in-built colourmaps via 'href'
//   The fx,fy values can be placed outside of the radial gradient if 'focal="unbound"' is used
//   The resolution value can be defined to lower the rate of colour sampling.

static ERR gradient_defaults(extSVG *Self, objVectorGradient *Gradient, uint32_t Attrib, const std::string Value)
{
   switch (Attrib) {
      case SVF_RESOLUTION:
         Gradient->setResolution(strtod(Value.c_str(), nullptr));
         return ERR::Okay;

      case SVF_COLOR_INTERPOLATION:
         if (iequals("auto", Value)) Gradient->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("sRGB", Value)) Gradient->setColourSpace(VCS::SRGB);
         else if (iequals("linearRGB", Value)) Gradient->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("inherit", Value)) Gradient->setColourSpace(VCS::INHERIT);
         return ERR::Okay;

      // Ignored attributes (sometimes defined to propagate to child tags)
      case SVF_COLOR:
      case SVF_STOP_COLOR:
      case SVF_STOP_OPACITY:
         return ERR::Okay;
   }

   return ERR::Failed;
}

//********************************************************************************************************************
// Note that all offsets are percentages.

const std::vector<GradientStop> svgState::process_gradient_stops(const XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch();

   double last_stop = 0;
   std::vector<GradientStop> stops;
   for (auto &scan : Tag.Children) {
      if (iequals("stop", scan.name())) {
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
               stop.Offset = strtod(value.c_str(), nullptr);
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
                  vec::ReadPainter(Self->Scene, m_stop_color.c_str(), &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
               else if (iequals("currentColor", value)) {
                  VectorPainter painter;
                  vec::ReadPainter(Self->Scene, m_color.c_str(), &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
               else {
                  VectorPainter painter;
                  vec::ReadPainter(Self->Scene, value.c_str(), &painter, nullptr);
                  stop.RGB = painter.Colour;
               }
            }
            else if (iequals("stop-opacity", name)) {
               if (iequals("inherit", value)) {
                  stop_opacity = m_stop_opacity;
               }
               else stop_opacity = strtod(value.c_str(), nullptr);
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

void svgState::parse_lineargradient(const XTag &Tag, objVectorGradient *Gradient, std::string &ID) noexcept
{
   pf::Log log(__FUNCTION__);

   // Determine the user coordinate system first.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
         if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) Gradient->Units = VUNIT::USERSPACE;
         break;
      }
   }

   bool process_stops = true;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      auto attrib = strihash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_GRADIENTUNITS: break; // Already checked gradientUnits earlier
         case SVF_GRADIENTTRANSFORM: Gradient->setTransform(val); break;
         case SVF_X1: set_double_units(Gradient, FID_X1, val, Gradient->Units); break;
         case SVF_Y1: set_double_units(Gradient, FID_Y1, val, Gradient->Units); break;
         case SVF_X2: set_double_units(Gradient, FID_X2, val, Gradient->Units); break;
         case SVF_Y2: set_double_units(Gradient, FID_Y2, val, Gradient->Units); break;

         case SVF_COLOR_INTERPOLATION:
            if (iequals("auto", val)) Gradient->setColourSpace(VCS::LINEAR_RGB);
            else if (iequals("sRGB", val)) Gradient->setColourSpace(VCS::SRGB);
            else if (iequals("linearRGB", val)) Gradient->setColourSpace(VCS::LINEAR_RGB);
            else if (iequals("inherit", val)) Gradient->setColourSpace(VCS::INHERIT);
            break;

         case SVF_SPREADMETHOD:
            if (iequals("pad", val))          Gradient->setSpreadMethod(VSPREAD::PAD);
            else if (iequals("reflect", val)) Gradient->setSpreadMethod(VSPREAD::REFLECT);
            else if (iequals("repeat", val))  Gradient->setSpreadMethod(VSPREAD::REPEAT);
            break;

         case SVF_ID: ID = val; break;

         case SVF_HREF:
         case SVF_XLINK_HREF: {
            if (val.starts_with("url(#cmap:")) {
               auto end = val.find(')');
               auto cmap = val.substr(5, end-5);
               if (Gradient->setColourMap(cmap) IS ERR::Okay) process_stops = false;
            }
            else if (auto other = find_href_tag(Self, val)) {
               std::string dummy;
               if (iequals("radialGradient", other->name())) {
                  parse_radialgradient(*other, *Gradient, dummy);
               }
               else if (iequals("linearGradient", other->name())) {
                  parse_lineargradient(*other, Gradient, dummy);
               }
               else if (iequals("diamondGradient", other->name())) {
                  parse_diamondgradient(*other, Gradient, dummy);
               }
               else if (iequals("contourGradient", other->name())) {
                  parse_contourgradient(*other, Gradient, dummy);
               }
            }
            break;
         }

         default: {
            if (gradient_defaults(Self, Gradient, attrib, val) != ERR::Okay) {
               if (Tag.Attribs[a].Name.find(':') != std::string::npos) break;
               log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
            break;
         }
      }
   }

   auto stops = process_gradient_stops(Tag);
   if (stops.size() >= 2) Gradient->set(FID_Stops, stops);
}

//********************************************************************************************************************

void svgState::parse_radialgradient(const XTag &Tag, objVectorGradient &Gradient, std::string &ID) noexcept
{
   pf::Log log(__FUNCTION__);

   // Determine the user coordinate system first.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
         if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) Gradient.Units = VUNIT::USERSPACE;
         break;
      }
   }

   bool process_stops = true;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;
      log.trace("Processing radial gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strihash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_CX: set_double_units(&Gradient, FID_CenterX, val, Gradient.Units); break;
         case SVF_CY: set_double_units(&Gradient, FID_CenterY, val, Gradient.Units); break;
         case SVF_FX: set_double_units(&Gradient, FID_FX, val, Gradient.Units); break;
         case SVF_FY: set_double_units(&Gradient, FID_FY, val, Gradient.Units); break;
         case SVF_R:  set_double_units(&Gradient, FID_Radius, val, Gradient.Units); break;
         case SVF_GRADIENTUNITS: break; // Already processed
         case SVF_GRADIENTTRANSFORM: Gradient.setTransform(val); break;
         case SVF_ID: ID = val; break;
         case SVF_SPREADMETHOD:
            if (iequals("pad", val))          Gradient.setSpreadMethod(VSPREAD::PAD);
            else if (iequals("reflect", val)) Gradient.setSpreadMethod(VSPREAD::REFLECT);
            else if (iequals("repeat", val))  Gradient.setSpreadMethod(VSPREAD::REPEAT);
            break;

         case SVF_FOCALPOINT:
            if (iequals("unbound", val)) Gradient.Flags &= ~VGF::CONTAIN_FOCAL;
            break;

         case SVF_HREF:
         case SVF_XLINK_HREF: {
            if (val.starts_with("url(#cmap:")) {
               auto end = val.find(')');
               auto cmap = val.substr(5, end-5);
               if (Gradient.setColourMap(cmap) IS ERR::Okay) process_stops = false;
            }
            else if (auto other = find_href_tag(Self, val)) {
               std::string dummy;
               if (iequals("radialGradient", other->name())) {
                  parse_radialgradient(*other, Gradient, dummy);
               }
               else if (iequals("linearGradient", other->name())) {
                  parse_lineargradient(*other, &Gradient, dummy);
               }
               else if (iequals("diamondGradient", other->name())) {
                  parse_diamondgradient(*other, &Gradient, dummy);
               }
               else if (iequals("contourGradient", other->name())) {
                  parse_contourgradient(*other, &Gradient, dummy);
               }
            }
            break;
         }

         default: {
            if (gradient_defaults(Self, &Gradient, attrib, val) != ERR::Okay) {
               if (Tag.Attribs[a].Name.find(':') != std::string::npos) break;
               log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
         }
      }
   }

   if (process_stops) {
      auto stops = process_gradient_stops(Tag);
      if (stops.size() >= 2) Gradient.set(FID_Stops, stops);
   }
}

//********************************************************************************************************************

void svgState::parse_diamondgradient(const XTag &Tag, objVectorGradient *Gradient, std::string &ID) noexcept
{
   pf::Log log(__FUNCTION__);

   // Determine the user coordinate system first.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
         if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) Gradient->Units = VUNIT::USERSPACE;
         break;
      }
   }

   bool process_stops = true;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing diamond gradient attribute %s =  %s", Tag.Attribs[a].Name, val);

      auto attrib = strihash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_GRADIENTUNITS: break; // Already processed
         case SVF_GRADIENTTRANSFORM: Gradient->setTransform(val); break;
         case SVF_CX: set_double_units(Gradient, FID_CenterX, val, Gradient->Units); break;
         case SVF_CY: set_double_units(Gradient, FID_CenterY, val, Gradient->Units); break;
         case SVF_R:  set_double_units(Gradient, FID_Radius, val, Gradient->Units); break;
         case SVF_SPREADMETHOD: {
            if (iequals("pad", val))          Gradient->setSpreadMethod(VSPREAD::PAD);
            else if (iequals("reflect", val)) Gradient->setSpreadMethod(VSPREAD::REFLECT);
            else if (iequals("repeat", val))  Gradient->setSpreadMethod(VSPREAD::REPEAT);
            break;
         }
         case SVF_ID: ID = val; break;
         case SVF_HREF:
         case SVF_XLINK_HREF: {
            if (val.starts_with("url(#cmap:")) {
               auto end = val.find(')');
               auto cmap = val.substr(5, end-5);
               if (Gradient->setColourMap(cmap) IS ERR::Okay) process_stops = false;
            }
            else if (auto other = find_href_tag(Self, val)) {
               std::string dummy;
               if (iequals("radialGradient", other->name())) {
                  parse_radialgradient(*other, *Gradient, dummy);
               }
               else if (iequals("linearGradient", other->name())) {
                  parse_lineargradient(*other, Gradient, dummy);
               }
               else if (iequals("diamondGradient", other->name())) {
                  parse_diamondgradient(*other, Gradient, dummy);
               }
               else if (iequals("contourGradient", other->name())) {
                  parse_contourgradient(*other, Gradient, dummy);
               }
            }
            break;
         }
         default: {
            if (gradient_defaults(Self, Gradient, attrib, val) != ERR::Okay) {
               if (Tag.Attribs[a].Name.find(':') != std::string::npos) break;
               log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
         }
      }
   }

   auto stops = process_gradient_stops(Tag);
   if (stops.size() >= 2) Gradient->set(FID_Stops, stops);
}

//********************************************************************************************************************

void svgState::parse_contourgradient(const XTag &Tag, objVectorGradient *Gradient, std::string &ID) noexcept
{
   pf::Log log(__FUNCTION__);

   // Determine the user coordinate system first.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
         if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) Gradient->Units = VUNIT::USERSPACE;
         break;
      }
   }

   bool process_stops = true;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      log.trace("Processing contour gradient attribute %s = %s", Tag.Attribs[a].Name, val);

      auto attrib = strihash(Tag.Attribs[a].Name);
      switch(attrib) {
         case SVF_GRADIENTUNITS: break; // Already processed
         case SVF_GRADIENTTRANSFORM: Gradient->setTransform(val); break;
         // X1 and X2 adjust padding of the gradient within the target vector.
         case SVF_X1: set_double_units(Gradient, FID_X1, val, Gradient->Units); break;
         case SVF_X2: set_double_units(Gradient, FID_X2, val, Gradient->Units); break;
         case SVF_SPREADMETHOD: {
            if (iequals("pad", val))          Gradient->setSpreadMethod(VSPREAD::PAD);
            else if (iequals("reflect", val)) Gradient->setSpreadMethod(VSPREAD::REFLECT);
            else if (iequals("repeat", val))  Gradient->setSpreadMethod(VSPREAD::REPEAT);
            break;
         }
         case SVF_ID: ID = val; break;
         case SVF_HREF:
         case SVF_XLINK_HREF: {
            if (val.starts_with("url(#cmap:")) {
               auto end = val.find(')');
               auto cmap = val.substr(5, end-5);
               if (Gradient->setColourMap(cmap) IS ERR::Okay) process_stops = false;
            }
            else if (auto other = find_href_tag(Self, val)) {
               std::string dummy;
               if (iequals("radialGradient", other->name())) {
                  parse_radialgradient(*other, *Gradient, dummy);
               }
               else if (iequals("linearGradient", other->name())) {
                  parse_lineargradient(*other, Gradient, dummy);
               }
               else if (iequals("diamondGradient", other->name())) {
                  parse_diamondgradient(*other, Gradient, dummy);
               }
               else if (iequals("contourGradient", other->name())) {
                  parse_contourgradient(*other, Gradient, dummy);
               }
            }
            break;
         }
         default: {
            if (gradient_defaults(Self, Gradient, attrib, val) != ERR::Okay) {
               if (Tag.Attribs[a].Name.find(':') != std::string::npos) break;
               log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
         }
      }
   }
}

//********************************************************************************************************************

ERR svgState::proc_lineargradient(const XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objVectorGradient *gradient;

   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
      SetOwner(gradient, Self->Scene);
      gradient->setFields(
         fl::Name("SVGLinearGrad"),
         fl::Type(VGT::LINEAR),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::X1(0.0),
         fl::Y1(0.0),
         fl::X2(SCALE(1.0)),
         fl::Y2(0.0));

      state.parse_lineargradient(Tag, gradient, id);

      if (InitObject(gradient) IS ERR::Okay) {
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
   pf::Log log(__FUNCTION__);
   objVectorGradient *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
      SetOwner(gradient, Self->Scene);

      gradient->setFields(fl::Name("SVGRadialGrad"), fl::Type(VGT::RADIAL), fl::Units(VUNIT::BOUNDING_BOX),
         fl::CenterX(SCALE(0.5)), fl::CenterY(SCALE(0.5)), fl::Radius(SCALE(0.5)));

      // Enforce SVG limits on focal point positioning.  Can be overridden with focal="unbound", which is a Kotuku
      // specific feature.

      gradient->Flags |= VGF::CONTAIN_FOCAL;

      state.parse_radialgradient(Tag, *gradient, id);

      if (InitObject(gradient) IS ERR::Okay) {
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
   pf::Log log(__FUNCTION__);
   objVectorGradient *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
      SetOwner(gradient, Self->Scene);

      gradient->setFields(fl::Name("SVGDiamondGrad"), fl::Type(VGT::DIAMOND), fl::Units(VUNIT::BOUNDING_BOX),
         fl::CenterX(SCALE(0.5)), fl::CenterY(SCALE(0.5)), fl::Radius(SCALE(0.5)));

      state.parse_diamondgradient(Tag, gradient, id);

      if (InitObject(gradient) IS ERR::Okay) {
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
// NB: Contour gradients are not part of the SVG standard.

ERR svgState::proc_contourgradient(const XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objVectorGradient *gradient;
   std::string id;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
      SetOwner(gradient, Self->Scene);
      gradient->setFields(fl::Name("SVGContourGrad"), fl::Type(VGT::CONTOUR), fl::Units(VUNIT::BOUNDING_BOX));

      state.parse_contourgradient(Tag, gradient, id);

      auto stops = process_gradient_stops(Tag);
      if (stops.size() >= 2) gradient->set(FID_Stops, stops);

      if (InitObject(gradient) IS ERR::Okay) {
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

ERR svgState::proc_conicgradient(const XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objVectorGradient *gradient;

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   if (NewObject(CLASSID::VECTORGRADIENT, &gradient) IS ERR::Okay) {
      SetOwner(gradient, Self->Scene);

      gradient->setFields(fl::Name("SVGConicGrad"), fl::Type(VGT::CONIC), fl::Units(VUNIT::BOUNDING_BOX),
         fl::CenterX(SCALE(0.5)), fl::CenterY(SCALE(0.5)), fl::Radius(SCALE(0.5)));

      std::string id;

      // Determine the user coordinate system first.

      gradient->Units = VUNIT::BOUNDING_BOX;
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         if (iequals("gradientUnits", Tag.Attribs[a].Name)) {
            if (iequals("userSpaceOnUse", Tag.Attribs[a].Value)) gradient->Units = VUNIT::USERSPACE;
            break;
         }
      }

      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         log.trace("Processing diamond gradient attribute %s =  %s", Tag.Attribs[a].Name, val);

         auto attrib = strihash(Tag.Attribs[a].Name);
         switch(attrib) {
            case SVF_GRADIENTUNITS:
               if (iequals("userSpaceOnUse", val)) gradient->setUnits(VUNIT::USERSPACE);
               else if (iequals("objectBoundingBox", val)) gradient->setUnits(VUNIT::BOUNDING_BOX);
               break;
            case SVF_GRADIENTTRANSFORM: gradient->setTransform(val); break;
            case SVF_CX: set_double_units(gradient, FID_CenterX, val, gradient->Units); break;
            case SVF_CY: set_double_units(gradient, FID_CenterY, val, gradient->Units); break;
            case SVF_R:  set_double_units(gradient, FID_Radius, val, gradient->Units); break;
            case SVF_SPREADMETHOD: {
               if (iequals("pad", val))          gradient->setSpreadMethod(VSPREAD::PAD);
               else if (iequals("reflect", val)) gradient->setSpreadMethod(VSPREAD::REFLECT);
               else if (iequals("repeat", val))  gradient->setSpreadMethod(VSPREAD::REPEAT);
               break;
            }
            case SVF_ID: id = val; break;
            default: {
               if (gradient_defaults(Self, gradient, attrib, val) != ERR::Okay) {
                  if (Tag.Attribs[a].Name.find(':') != std::string::npos) break;
                  log.warning("%s attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
               }
            }
         }
      }

      auto stops = process_gradient_stops(Tag);
      if (stops.size() >= 2) gradient->set(FID_Stops, stops);

      if (InitObject(gradient) IS ERR::Okay) {
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
