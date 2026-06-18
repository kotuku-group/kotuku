
static void set_dimension(XTag *Tag, const std::string Attrib, double Value, bool Scaled)
{
   if (Scaled) xml::NewAttrib(*Tag, Attrib, std::to_string(Value * 100.0) + "%");
   else xml::NewAttrib(*Tag, Attrib, std::to_string(Value));
}

static void set_dimension(XTag *Tag, const std::string Attrib, Unit &Value)
{
   if (Value.defined()) {
      if (Value.scaled()) xml::NewAttrib(*Tag, Attrib, std::to_string(Value * 100.0) + "%");
      else xml::NewAttrib(*Tag, Attrib, std::to_string(double(Value)));
   }
}

//*********************************************************************************************************************

static void set_gradient_colour_space(XTag *Tag, objGradient *Gradient, ERR &Error)
{
   VCS colour_space;
   if ((!Error) and (!Gradient->getColourSpace(colour_space))) {
      switch(colour_space) {
         case VCS::SRGB:       xml::NewAttrib(Tag, "color-interpolation", "sRGB"); break;
         case VCS::LINEAR_RGB: xml::NewAttrib(Tag, "color-interpolation", "linearRGB"); break;
         default: break;
      }
   }
}

//*********************************************************************************************************************

static ERR save_vectorpath(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   std::string path;
   ERR error;

   if (!(error = Vector->getSequence(path))) {
      int new_index;
      error = XML->insertXML(Parent, XMI::CHILD_END, "<path/>", &new_index);
      if (!error) {
         XTag *tag;
         error = XML->getTag(new_index, &tag);
         if (!error) xml::NewAttrib(tag, "d", path);
      }
      if (!error) error = save_svg_scan_std(Self, XML, Vector, new_index);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_defs(extSVG *Self, objXML *XML, objVectorScene *Scene, int Parent)
{
   kt::Log log(__FUNCTION__);
   ankerl::unordered_dense::map<std::string, OBJECTPTR> *defs;

   if (!Scene->get(FID_Defs, defs)) {
      ERR error = ERR::Okay;
      int def_index = 0;
      for (auto & [ key, def ] : *defs) {
         if (!def_index) {
            if ((error = XML->insertXML(Parent, XMI::CHILD_END, "<defs/>", &def_index)) != ERR::Okay) return error;
         }

         log.msg("Processing definition %s (%x)", def->Class->ClassName.c_str(), uint32_t(def->classID()));

         if (def->baseClassID() IS CLASSID::GRADIENT) {
            auto gradient = (objGradient *)def;
            std::string gradient_type;
            switch(def->classID()) {
               case CLASSID::GRADIENTRADIAL:  gradient_type = "<radialGradient/>"; break;
               case CLASSID::GRADIENTCONIC:   gradient_type = "<conicGradient/>"; break;
               case CLASSID::GRADIENTDIAMOND: gradient_type = "<diamondGradient/>"; break;
               case CLASSID::GRADIENTCONTOUR: gradient_type = "<contourGradient/>"; break;
               case CLASSID::GRADIENTDISTAL:  gradient_type = "<distalGradient/>"; break;
               case CLASSID::GRADIENTLINEAR:  gradient_type = "<linearGradient/>"; break;
               case CLASSID::GRADIENTGOURAUD:
                  log.warning("GradientGouraud not supported.");
                  continue;
               default:
                  log.warning("%s not supported.", def->Class->ClassName.c_str());
                  continue;
            }
            XTag *tag = nullptr;
            error = XML->insertStatement(def_index, XMI::CHILD_END, gradient_type, &tag);

            if (!error) xml::NewAttrib(tag, "id", key);

            VUNIT units;
            if ((!error) and (!gradient->getUnits(units))) {
               switch(units) {
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "gradientUnits", "userSpaceOnUse"); break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "gradientUnits", "objectBoundingBox"); break;
                  default: break;
               }
            }

            VSPREAD spread;
            if ((!error) and (!gradient->getSpreadMethod(spread))) {
               switch(spread) {
                  default:
                  case VSPREAD::PAD:     break; // Pad is the default SVG setting
                  case VSPREAD::REFLECT: xml::NewAttrib(tag, "spreadMethod", "reflect"); break;
                  case VSPREAD::REPEAT:  xml::NewAttrib(tag, "spreadMethod", "repeat"); break;
               }
            }

            set_gradient_colour_space(tag, gradient, error);

            double resolution;
            if ((!error) and (!gradient->getResolution(resolution)) and (resolution != 1.0)) {
               xml::NewAttrib(tag, "resolution", std::to_string(resolution));
            }

            if (def->classID() IS CLASSID::GRADIENTLINEAR) {
               auto linear = (objGradientLinear *)gradient;
               Unit val;
               if ((!error) and (!linear->get(FID_X1, val))) set_dimension(tag, "x1", val);
               if ((!error) and (!linear->get(FID_Y1, val))) set_dimension(tag, "y1", val);
               if ((!error) and (!linear->get(FID_X2, val))) set_dimension(tag, "x2", val);
               if ((!error) and (!linear->get(FID_Y2, val))) set_dimension(tag, "y2", val);
            }
            else if (def->classID() IS CLASSID::GRADIENTCONTOUR) {
               auto contour = (objGradientContour *)gradient;
               Unit val;
               if ((!error) and (!contour->getFloor(val))) set_dimension(tag, "floor", val);
               if ((!error) and (!contour->getMultiplier(val))) set_dimension(tag, "multiplier", val);
            }
            else if (def->classID() IS CLASSID::GRADIENTDISTAL) {
               auto distal = (objGradientDistal *)gradient;
               Unit val;
               if ((!error) and (!distal->getFloor(val))) set_dimension(tag, "floor", val);
               if ((!error) and (!distal->getMultiplier(val))) set_dimension(tag, "multiplier", val);
               if ((!error) and (!distal->getRadius(val)) and (val.defined()) and (double(val) > 0)) {
                  set_dimension(tag, "radius", val);
               }
            }
            else if (def->classID() IS CLASSID::GRADIENTRADIAL) {
               auto radial = (objGradientRadial *)gradient;
               Unit val;
               if ((!error) and (!radial->getCX(val))) set_dimension(tag, "cx", val);
               if ((!error) and (!radial->getCY(val))) set_dimension(tag, "cy", val);
               if ((!error) and (!radial->getFX(val))) set_dimension(tag, "fx", val);
               if ((!error) and (!radial->getFY(val))) set_dimension(tag, "fy", val);
               if ((!error) and (!radial->getRadius(val))) set_dimension(tag, "r", val);

               int contain_focal;
               if ((!error) and (!radial->getContainFocal(contain_focal)) and (!contain_focal)) {
                  xml::NewAttrib(tag, "focal", "unbound");
               }
            }
            else if ((def->classID() IS CLASSID::GRADIENTDIAMOND) or (def->classID() IS CLASSID::GRADIENTCONIC)) {
               Unit val;

               if (def->classID() IS CLASSID::GRADIENTCONIC) {
                  auto conic = (objGradientConic *)gradient;
                  if ((!error) and (!conic->getCX(val))) set_dimension(tag, "cx", val);
                  if ((!error) and (!conic->getCY(val))) set_dimension(tag, "cy", val);
                  if ((!error) and (!conic->getRadius(val))) set_dimension(tag, "r", val);

                  double span;
                  if ((!error) and (!conic->getSpan(span)) and (span != 1.0)) {
                     xml::NewAttrib(tag, "span", std::to_string(span));
                  }
               }
               else {
                  auto diamond = (objGradientDiamond *)gradient;
                  if ((!error) and (!diamond->getCX(val))) set_dimension(tag, "cx", val);
                  if ((!error) and (!diamond->getCY(val))) set_dimension(tag, "cy", val);
                  if ((!error) and (!diamond->getRadius(val))) set_dimension(tag, "r", val);
               }
            }

            VectorMatrix *transform;
            if ((!error) and (!gradient->getMatrices(transform)) and (transform)) {
               std::stringstream buffer;
               if (!save_svg_transform(transform, buffer)) {
                  xml::NewAttrib(tag, "gradientTransform", buffer.str());
               }
            }

            std::span<GradientStop> stops;
            if (!gradient->getStops(stops)) {
               for (size_t s=0; (s < stops.size()) and (!error); s++) {
                  auto &stop = stops[s];
                  int stop_index;
                  if (!(error = XML->insertXML(tag->ID, XMI::CHILD_END, "<stop/>", &stop_index))) {
                     XTag *stop_tag;
                     error = XML->getTag(stop_index, &stop_tag);
                     if (!error) xml::NewAttrib(stop_tag, "offset", std::to_string(stop.Offset));

                     std::stringstream buffer;
                     buffer << "rgb(" << stop.RGB.Red*255.0 << "," << stop.RGB.Green*255.0 << ","
                        << stop.RGB.Blue*255.0 << ")";
                     if (!error) xml::NewAttrib(stop_tag, "stop-color", buffer.str());
                     if ((!error) and (stop.RGB.Alpha != 1.0)) {
                        xml::NewAttrib(stop_tag, "stop-opacity", std::to_string(stop.RGB.Alpha));
                     }
                  }
               }
            }
         }
         else if (def->classID() IS CLASSID::VECTORIMAGE) {
            log.warning("VectorImage not supported.");
         }
         else if (def->classID() IS CLASSID::VECTORPATH) {
            error = save_vectorpath(Self, XML, (objVector *)def, def_index);
         }
         else if (def->classID() IS CLASSID::VECTORPATTERN) {
            log.warning("VectorPattern not supported.");
         }
         else if (def->classID() IS CLASSID::VECTORFILTER) {
            objVectorFilter *filter = (objVectorFilter *)def;

            XTag *tag;
            error = XML->insertStatement(def_index, XMI::CHILD_END, "<filter/>", &tag);

            if (!error) xml::NewAttrib(tag, "id", key);

            DMF dim;
            filter->getDimensions(dim);

            if ((!error) and dmf::hasAnyX(dim)) set_dimension(tag, "x", filter->X, dmf::hasScaledX(dim));
            if ((!error) and dmf::hasAnyY(dim)) set_dimension(tag, "y", filter->Y, dmf::hasScaledY(dim));

            if ((!error) and dmf::hasAnyWidth(dim))
               set_dimension(tag, "width", filter->Width, dmf::hasScaledWidth(dim));

            if ((!error) and dmf::hasAnyHeight(dim))
               set_dimension(tag, "height", filter->Height, dmf::hasScaledHeight(dim));

            VUNIT units;
            if ((!error) and (!filter->getUnits(units))) {
               switch(units) {
                  default:
                  case VUNIT::BOUNDING_BOX: break; // Default
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "filterUnits", "userSpaceOnUse"); break;
               }
            }

            if ((!error) and (!filter->getPrimitiveUnits(units))) {
               switch(units) {
                  default:
                  case VUNIT::USERSPACE:    break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "primitiveUnits", "objectBoundingBox"); break;
               }
            }

            std::string effect_xml;
            if ((!error) and (!filter->getEffectXML(effect_xml))) {
               error = XML->insertStatement(tag->ID, XMI::CHILD, effect_xml, nullptr);
            }
         }
         else if (def->classID() IS CLASSID::VECTORTRANSITION) {
            log.warning("VectorTransition not supported.");
         }
         else if (def->classID() IS CLASSID::VECTORCLIP) {
            log.warning("VectorClip not supported.");
         }
         else if (def->Class->BaseClassID IS CLASSID::VECTOR) {
            log.warning("%s not supported.", def->Class->ClassName.c_str());
         }
         else log.warning("Unrecognised definition class %x", uint32_t(def->classID()));
      }

      return ERR::Okay;
   }
   else return ERR::Failed;
}

//*********************************************************************************************************************

static ERR save_svg_transform(VectorMatrix *Transform, std::stringstream &Buffer)
{
   std::vector<VectorMatrix *> list;

   for (auto t=Transform; t; t=t->Next) list.push_back(t);

   Buffer.setf(std::ios_base::hex);
   bool need_space = false;
   std::for_each(list.rbegin(), list.rend(), [&](auto t) {
      if (need_space) Buffer << " ";
      else need_space = true;
      Buffer << "matrix(" << t->ScaleX << " " << t->ShearY << " " << t->ShearX << " " << t->ScaleY << " " << t->TranslateX << " " << t->TranslateY << ")";
   });

   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR save_svg_scan_std(extSVG *Self, objXML *XML, objVector *Vector, int TagID)
{
   kt::Log log(__FUNCTION__);
   char buffer[160];
   std::string str;
   std::string_view sv;
   FRGB *colour;
   ERR error = ERR::Okay;

   XTag *tag;
   if ((error = XML->getTag(TagID, &tag)) != ERR::Okay) return error;

   if (Vector->Opacity != 1.0) xml::NewAttrib(tag, "opacity", std::to_string(Vector->Opacity));
   if (Vector->FillOpacity != 1.0) xml::NewAttrib(tag, "fill-opacity", std::to_string(Vector->FillOpacity));
   if (Vector->StrokeOpacity != 1.0) xml::NewAttrib(tag, "stroke-opacity", std::to_string(Vector->StrokeOpacity));

   if ((!Vector->getStroke(sv)) and not sv.empty()) {
      xml::NewAttrib(tag, "stroke", sv);
   }
   else if ((!Vector->getStrokeColour(colour)) and colour->Alpha) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour->Red, colour->Green, colour->Blue, colour->Alpha);
      xml::NewAttrib(tag, "stroke-color", buffer);
   }

   VLJ line_join;
   if ((!error) and (Vector->getLineJoin(line_join) IS ERR::Okay)) {
      switch (line_join) {
         default:
         case VLJ::MITER:        break; // Default
         case VLJ::MITER_SMART:  xml::NewAttrib(tag, "stroke-linejoin", "miter-clip"); break; // Kōtuku
         case VLJ::ROUND:        xml::NewAttrib(tag, "stroke-linejoin", "round"); break;
         case VLJ::BEVEL:        xml::NewAttrib(tag, "stroke-linejoin", "bevel"); break;
         case VLJ::MITER_ROUND:  xml::NewAttrib(tag, "stroke-linejoin", "arcs"); break; // (SVG2) Not sure if compliant
         case VLJ::INHERIT:      xml::NewAttrib(tag, "stroke-linejoin", "inherit"); break;
      } // "miter-clip" SVG2
   }

   VIJ inner_join;
   if ((!error) and (Vector->getInnerJoin(inner_join) IS ERR::Okay)) { // Kōtuku only
      switch (inner_join) {
         default:
         case VIJ::MITER:   break; // Default
         case VIJ::BEVEL:   xml::NewAttrib(tag, "stroke-innerjoin", "bevel"); break;
         case VIJ::JAG:     xml::NewAttrib(tag, "stroke-innerjoin", "jag"); break;
         case VIJ::ROUND:   xml::NewAttrib(tag, "stroke-innerjoin", "round"); break;
         case VIJ::INHERIT: xml::NewAttrib(tag, "stroke-innerjoin", "inherit"); break;
      }
   }

   std::span<double> dash_array;
   if ((!error) and (!Vector->getDashArray(dash_array)) and (not dash_array.empty())) {
      double dash_offset;
      if ((!Vector->getDashOffset(dash_offset)) and (dash_offset != 0)) {
         xml::NewAttrib(tag, "stroke-dashoffset", std::to_string(Vector->DashOffset));
      }

      int pos = 0;
      for (size_t i=0; i < dash_array.size(); i++) {
         if (pos != 0) buffer[pos++] = ',';
         pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dash_array[i]);
         if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
      }
      xml::NewAttrib(tag, "stroke-dasharray", buffer);
   }

   VLC linecap;
   if ((!error) and (Vector->getLineCap(linecap) IS ERR::Okay)) {
      switch (linecap) {
         default:
         case VLC::BUTT:    break; // Default
         case VLC::SQUARE:  xml::NewAttrib(tag, "stroke-linecap", "square"); break;
         case VLC::ROUND:   xml::NewAttrib(tag, "stroke-linecap", "round"); break;
         case VLC::INHERIT: xml::NewAttrib(tag, "stroke-linecap", "inherit"); break;
      }
   }

   if (Vector->Visibility IS VIS::HIDDEN)        xml::NewAttrib(tag, "visibility", "hidden");
   else if (Vector->Visibility IS VIS::COLLAPSE) xml::NewAttrib(tag, "visibility", "collapse");
   else if (Vector->Visibility IS VIS::INHERIT)  xml::NewAttrib(tag, "visibility", "inherit");

   Unit stroke_width;
   if ((!error) and (!Vector->getStrokeWidth(stroke_width))) {
      if (not stroke_width.defined()) stroke_width = 0;
      if (stroke_width != 1) xml::NewAttrib(tag, "stroke-width", std::to_string(stroke_width));
   }

   if ((!error) and (!Vector->getFill(sv)) and not sv.empty()) {
      if (!iequals("rgb(0,0,0)", sv)) xml::NewAttrib(tag, "fill", sv);
   }
   else if ((!error) and (!Vector->getFillColour(colour)) and colour->Alpha) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour->Red, colour->Green, colour->Blue, colour->Alpha);
      xml::NewAttrib(tag, "fill", buffer);
   }

   VFR fill_rule;
   if ((!error) and (!Vector->getFillRule(fill_rule))) {
      if (fill_rule IS VFR::EVEN_ODD) xml::NewAttrib(tag, "fill-rule", "evenodd");
   }

   if ((!error) and (!(error = Vector->getSID(sv))) and not sv.empty()) xml::NewAttrib(tag, "id", sv);

   if ((!error) and (!(error = Vector->getFilter(sv))) and not sv.empty()) xml::NewAttrib(tag, "filter", sv);

   VectorMatrix *transform;
   if ((!error) and (!Vector->get(FID_Transforms, transform)) and (transform)) {
      std::stringstream buffer;
      if ((error = save_svg_transform(transform, buffer)) IS ERR::Okay) {
         xml::NewAttrib(tag, "transform", buffer.str());
      }
   }

   OBJECTPTR shape;
   if ((!error) and (!Vector->getMorph(shape)) and (shape)) {
      VMF morph_flags;
      XTag *morph_tag;
      error = XML->insertStatement(TagID, XMI::CHILD_END, "<kotuku:morph/>", &morph_tag);

      std::string_view shape_id;
      if ((!error) and (!shape->get(FID_ID, shape_id)) and not shape_id.empty()) {
         // NB: It is required that the shape has previously been registered as a definition, otherwise the url will refer to a dud tag.
         auto shape_ref = std::format("url(#{})", shape_id);
         xml::NewAttrib(morph_tag, "xlink:href", shape_ref);
      }

      if (!error) error = Vector->getMorphFlags(morph_flags);

      if ((!error) and ((morph_flags & VMF::STRETCH) != VMF::NIL)) xml::NewAttrib(morph_tag, "method", "stretch");
      if ((!error) and ((morph_flags & VMF::AUTO_SPACING) != VMF::NIL)) xml::NewAttrib(morph_tag, "spacing", "auto");

      if ((!error) and ((morph_flags & (VMF::X_MIN|VMF::X_MID|VMF::X_MAX|VMF::Y_MIN|VMF::Y_MID|VMF::Y_MAX)) != VMF::NIL)) {
         std::string align;
         if ((morph_flags & VMF::X_MIN) != VMF::NIL) align = "xMin ";
         else if ((morph_flags & VMF::X_MID) != VMF::NIL) align = "xMid ";
         else if ((morph_flags & VMF::X_MAX) != VMF::NIL) align = "xMax ";

         if ((morph_flags & VMF::Y_MIN) != VMF::NIL) align += "yMin";
         else if ((morph_flags & VMF::Y_MID) != VMF::NIL) align += "yMid";
         else if ((morph_flags & VMF::Y_MAX) != VMF::NIL) align += "yMax";

         xml::NewAttrib(morph_tag, "align", align);
      }

      OBJECTPTR tv;
      if ((!error) and (!Vector->getTransition(tv))) {
         // TODO save_svg_scan_std transition support





      }
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   kt::Log log(__FUNCTION__);

   int new_index = -1;

   log.branch("%s", Vector->Class->ClassName.c_str());

   ERR error = ERR::Okay;
   if (Vector->classID() IS CLASSID::VECTORRECTANGLE) {
      auto rect = (objVectorRectangle *)Vector;
      XTag *tag;
      Unit rx, ry, x, y, width, height;

      error = XML->insertXML(Parent, XMI::CHILD_END, "<rect/>", &new_index);
      if (!error) error = XML->getTag(new_index, &tag);

      if (!error) {
         if ((!rect->getRoundX(rx)) and rx.defined() and (rx > 0)) set_dimension(tag, "rx", rx);
         if ((!rect->getRoundY(ry)) and ry.defined() and (ry > 0)) set_dimension(tag, "ry", ry);
         if ((!rect->getX(x))) set_dimension(tag, "x", x);
         if ((!rect->getY(y))) set_dimension(tag, "y", y);
         if ((!rect->getWidth(width))) set_dimension(tag, "width", width);
         if ((!rect->getHeight(height))) set_dimension(tag, "height", height);

         save_svg_scan_std(Self, XML, Vector, new_index);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORELLIPSE) {
      auto ellipse = (objVectorEllipse *)Vector;
      XTag *tag;
      double rx, ry;
      double cx, cy;
      DMF dim;

      ellipse->getDimensions(dim);
      if (!error) error = ellipse->getRadiusX(rx);
      if (!error) error = ellipse->getRadiusY(ry);
      if (!error) error = ellipse->getCenterX(cx);
      if (!error) error = ellipse->getCenterY(cy);
      if (!error) error = XML->insertStatement(Parent, XMI::CHILD_END, "<ellipse/>", &tag);

      if (!error) {
         set_dimension(tag, "rx", rx, dmf::hasScaledRadiusX(dim));
         set_dimension(tag, "ry", ry, dmf::hasScaledRadiusY(dim));
         set_dimension(tag, "cx", cx, dmf::hasScaledCenterX(dim));
         set_dimension(tag, "cy", cy, dmf::hasScaledCenterY(dim));
      }

      if (!error) error = save_svg_scan_std(Self, XML, Vector, new_index);
   }
   else if (Vector->classID() IS CLASSID::VECTORPATH) {
      error = save_vectorpath(Self, XML, Vector, Parent);
   }
   else if (Vector->classID() IS CLASSID::VECTORPOLYGON) { // Serves <polygon>, <line> and <polyline>
      auto vp = (objVectorPolygon *)Vector;
      XTag *tag;
      std::span<VectorPoint> points;
      int i;

      if ((!vp->getClosed(i)) and (i IS FALSE)) { // Line or Polyline
         if (!(error = vp->getPointsArray(points))) {
            if (points.size() IS 2) {
               error = XML->insertStatement(Parent, XMI::CHILD_END, "<line/>", &tag);
               if (!error) {
                  set_dimension(tag, "x1", points[0].X, points[0].XScaled);
                  set_dimension(tag, "y1", points[0].Y, points[0].YScaled);
                  set_dimension(tag, "x2", points[1].X, points[1].XScaled);
                  set_dimension(tag, "y2", points[1].Y, points[1].YScaled);
               }
            }
            else {
               std::stringstream buffer;
               error = XML->insertStatement(Parent, XMI::CHILD_END, "<polyline/>", &tag);
               if (!error) {
                  for (unsigned i=0; i < points.size(); i++) {
                     buffer << points[i].X << "," << points[i].Y << " ";
                  }
                  xml::NewAttrib(tag, "points", buffer.str());
               }
            }
         }
      }
      else {
         std::stringstream buffer;
         error = XML->insertStatement(Parent, XMI::CHILD_END, "<polygon/>", &tag);

         if ((!error) and (!vp->getPointsArray(points))) {
            for (unsigned i=0; i < points.size(); i++) {
               buffer << points[i].X << "," << points[i].Y << " ";
            }
            xml::NewAttrib(tag, "points", buffer.str());
         }
      }

      int path_length;
      if ((!(error = vp->getPathLength(path_length))) and (path_length != 0)) {
         xml::NewAttrib(tag, "pathLength", std::to_string(path_length));
      }

      if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORTEXT) {
      auto vt = (objVectorText *)Vector;
      XTag *tag;
      double x, y, text_length;
      int weight;
      std::string str;
      std::string_view sv;
      char buffer[1024];

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<text/>", &tag);

      if ((!error) and (!vt->getX(x))) set_dimension(tag, "x", x, FALSE);
      if ((!error) and (!vt->getY(y))) set_dimension(tag, "y", y, FALSE);

      std::span<double> dx;
      if ((!error) and (!(error = vt->getDX(dx))) and (not dx.empty())) {
         int pos = 0;
         for (unsigned i=0; i < dx.size(); i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dx[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dx", buffer);
      }

      std::span<double> dy;
      if ((!error) and (!(error = vt->getDY(dy))) and (not dy.empty())) {
         int pos = 0;
         for (unsigned i=0; i < dy.size(); i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dy[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dy", buffer);
      }

      if ((!error) and (!(error = vt->getFontSize(sv)))) {
         xml::NewAttrib(tag, "font-size", sv);
      }

      std::span<double> rotate;
      if ((!error) and (!(error = vt->getRotate(rotate))) and (not rotate.empty())) {
         std::stringstream buffer;
         bool comma = false;
         for (unsigned i=0; i < rotate.size(); i++) {
            if (comma) buffer << ',';
            else comma = true;
            buffer << rotate[i];
         }
         xml::NewAttrib(tag, "rotate", buffer.str());
      }

      if ((!error) and (!(error = vt->getTextLength(text_length))) and (text_length))
         xml::NewAttrib(tag, "textLength", std::to_string(text_length));

      if ((!error) and (!(error = vt->getFace(sv))))
         xml::NewAttrib(tag, "font-family", sv);

      if ((!error) and (!(error = vt->getWeight(weight))) and (weight != 400))
         xml::NewAttrib(tag, "font-weight", std::to_string(weight));

      if ((!error) and (!(error = vt->getString(sv))))
         error = XML->insertContent(tag->ID, XMI::CHILD, sv, nullptr);

      // TODO: lengthAdjust, font, font-size-adjust, font-stretch, font-style, font-variant, text-anchor, kerning, letter-spacing, path-length, word-spacing, text-decoration

      if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORGROUP) {
      XTag *tag;
      error = XML->insertStatement(Parent, XMI::CHILD_END, "<g/>", &tag);
      if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORCLIP) {
      auto clip = (objVectorClip *)Vector;
      XTag *tag;
      std::string_view str;
      if ((!(error = clip->getSID(str))) and not str.empty()) { // The id is an essential requirement
         error = XML->insertStatement(Parent, XMI::CHILD_END, "<clipPath/>", &tag);

         VUNIT units;
         if (!clip->getUnits(units)) {
            switch(units) {
               default:
               case VUNIT::USERSPACE:    break; // Default
               case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "clipPathUnits", "objectBoundingBox"); break;
            }
         }

         if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORWAVE) {
      auto wave = (objVectorWave *)Vector;
      XTag *tag;
      double dbl;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:wave/>", &tag);

      if (!error) {
         DMF dim;
         wave->getDimensions(dim);
         if (!wave->getX(dbl)) set_dimension(tag, "x", dbl, dmf::hasScaledX(dim));
         if (!wave->getY(dbl)) set_dimension(tag, "y", dbl, dmf::hasScaledY(dim));
         if (!wave->getWidth(dbl)) set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (!wave->getHeight(dbl)) set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (!wave->getAmplitude(dbl)) xml::NewAttrib(tag, "amplitude", std::to_string(dbl));
         if (!wave->getFrequency(dbl)) xml::NewAttrib(tag, "frequency", std::to_string(dbl));
         if (!wave->getDecay(dbl)) xml::NewAttrib(tag, "decay", std::to_string(dbl));
         if (!wave->getDegree(dbl)) xml::NewAttrib(tag, "degree", std::to_string(dbl));

         int close;
         if (!wave->getClose(close)) xml::NewAttrib(tag, "close", std::to_string(close));
         if (!wave->getThickness(dbl)) xml::NewAttrib(tag, "thickness", std::to_string(dbl));

         if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSPIRAL) {
      auto spiral = (objVectorSpiral *)Vector;
      XTag *tag;
      double dbl;
      int length;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:spiral/>", &tag);
      if (error != ERR::Okay) return error;

      if (!error) {
         DMF dim;
         spiral->getDimensions(dim);
         if (!spiral->getCenterX(dbl)) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (!spiral->getCenterY(dbl)) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (!spiral->getWidth(dbl))   set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (!spiral->getHeight(dbl))  set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (!spiral->getOffset(dbl))  xml::NewAttrib(tag, "offset", std::to_string(dbl));
         if (!spiral->getRadius(dbl))  set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (!spiral->getStep(dbl))    xml::NewAttrib(tag, "step", std::to_string(dbl));
         if ((!spiral->getPathLength(length)) and (length != 0)) xml::NewAttrib(tag, "pathLength", std::to_string(length));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSHAPE) {
      auto shape = (objVectorShape *)Vector;
      XTag *tag;
      double dbl;
      int num;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:shape/>", &tag);

      if (!error) {
         DMF dim;
         shape->getDimensions(dim);
         if (!shape->getCenterX(dbl)) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (!shape->getCenterY(dbl)) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (!shape->getRadius(dbl)) set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (!shape->getA(dbl)) xml::NewAttrib(tag, "a", std::to_string(dbl));
         if (!shape->getB(dbl)) xml::NewAttrib(tag, "b", std::to_string(dbl));
         if (!shape->getM(dbl)) xml::NewAttrib(tag, "m", std::to_string(dbl));
         if (!shape->getN1(dbl)) xml::NewAttrib(tag, "n1", std::to_string(dbl));
         if (!shape->getN2(dbl)) xml::NewAttrib(tag, "n2", std::to_string(dbl));
         if (!shape->getN3(dbl)) xml::NewAttrib(tag, "n3", std::to_string(dbl));
         if (!shape->getPhi(dbl)) xml::NewAttrib(tag, "phi", std::to_string(dbl));
         if (!shape->getVertices(num)) xml::NewAttrib(tag, "vertices", std::to_string(num));
         if (!shape->getMod(num)) xml::NewAttrib(tag, "mod", std::to_string(num));
         if (!shape->getSpiral(num)) xml::NewAttrib(tag, "spiral", std::to_string(num));
         if (!shape->getRepeat(num)) xml::NewAttrib(tag, "repeat", std::to_string(num));
         if (!shape->getClose(num)) xml::NewAttrib(tag, "close", std::to_string(num));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORVIEWPORT) {
      XTag *tag;
      double x, y, width, height;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<svg/>", &tag);

      auto viewport = (objVectorViewport *)Vector;
      if (!error) error = viewport->getViewX(x);
      if (!error) error = viewport->getViewY(y);
      if (!error) error = viewport->getViewWidth(width);
      if (!error) error = viewport->getViewHeight(height);

      if (!error) {
         std::stringstream buffer;
         buffer << x << " " << y << " " << width << " " << height;
         xml::NewAttrib(tag, "viewBox", buffer.str());
      }

      if (!error) {
         DMF dim;
         viewport->getDimensions(dim);

         if ((!error) and dmf::hasAnyX(dim) and (!viewport->getX(x)))
            set_dimension(tag, "x", x, dmf::hasScaledX(dim));

         if ((!error) and dmf::hasAnyY(dim) and (!viewport->getY(y)))
            set_dimension(tag, "y", y, dmf::hasScaledY(dim));

         if ((!error) and dmf::hasAnyWidth(dim) and (!viewport->getWidth(width)))
            set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));

         if ((!error) and dmf::hasAnyHeight(dim) and (!viewport->getHeight(height)))
            set_dimension(tag, "height", height, dmf::hasScaledHeight(dim));
      }
   }
   else {
      log.msg("Unrecognised class \"%s\"", Vector->Class->ClassName.c_str());
      return ERR::Okay; // Skip objects in the scene graph that we don't recognise
   }

   if (!error) {
      for (auto scan=Vector->Child; scan; scan=scan->Next) {
         save_svg_scan(Self, XML, scan, new_index);
      }
   }

   return error;
}
