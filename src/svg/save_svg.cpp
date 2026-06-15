
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

static ERR save_vectorpath(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   std::string path;
   ERR error;

   if (!(error = Vector->get(FID_Sequence, path))) {
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
      ERR error;
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
               default: gradient_type = "<linearGradient/>"; break;
            }
            XTag *tag = nullptr;
            error = XML->insertStatement(def_index, XMI::CHILD_END, gradient_type, &tag);

            if (!error) xml::NewAttrib(tag, "id", key);

            VUNIT units;
            if ((!error) and (!gradient->get(FID_Units, (int &)units))) {
               switch(units) {
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "gradientUnits", "userSpaceOnUse"); break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "gradientUnits", "objectBoundingBox"); break;
                  default: break;
               }
            }

            VSPREAD spread;
            if ((!error) and (!gradient->get(FID_SpreadMethod, (int &)spread))) {
               switch(spread) {
                  default:
                  case VSPREAD::PAD:     break; // Pad is the default SVG setting
                  case VSPREAD::REFLECT: xml::NewAttrib(tag, "spreadMethod", "reflect"); break;
                  case VSPREAD::REPEAT:  xml::NewAttrib(tag, "spreadMethod", "repeat"); break;
               }
            }

            if (def->classID() IS CLASSID::GRADIENTLINEAR) {
               Unit val;
               if ((!error) and (!gradient->get(FID_X1, val))) set_dimension(tag, "x1", val);
               if ((!error) and (!gradient->get(FID_Y1, val))) set_dimension(tag, "y1", val);
               if ((!error) and (!gradient->get(FID_X2, val))) set_dimension(tag, "x2", val);
               if ((!error) and (!gradient->get(FID_Y2, val))) set_dimension(tag, "y2", val);
            }
            else if (def->classID() IS CLASSID::GRADIENTCONTOUR) {
               double dbl;
               if ((!error) and (!gradient->get(FID_Floor, dbl))) xml::NewAttrib(tag, "floor", std::to_string(dbl));
               if ((!error) and (!gradient->get(FID_Multiplier, dbl))) xml::NewAttrib(tag, "multiplier", std::to_string(dbl));
            }
            else if (def->classID() IS CLASSID::GRADIENTDISTAL) {
               double dbl;
               if ((!error) and (!gradient->get(FID_Floor, dbl))) xml::NewAttrib(tag, "floor", std::to_string(dbl));
               if ((!error) and (!gradient->get(FID_Multiplier, dbl))) xml::NewAttrib(tag, "multiplier", std::to_string(dbl));
               if ((!error) and (!gradient->get(FID_Radius, dbl)) and (dbl > 0))
                  xml::NewAttrib(tag, "radius", std::to_string(dbl));
            }
            else if (def->classID() IS CLASSID::GRADIENTRADIAL) {
               Unit val;
               if ((!error) and (!gradient->get(FID_CenterX, val))) set_dimension(tag, "cx", val);
               if ((!error) and (!gradient->get(FID_CenterY, val))) set_dimension(tag, "cy", val);
               if ((!error) and (!gradient->get(FID_FocalX, val))) set_dimension(tag, "fx", val);
               if ((!error) and (!gradient->get(FID_FocalY, val))) set_dimension(tag, "fy", val);
               if ((!error) and (!gradient->get(FID_Radius, val))) set_dimension(tag, "r", val);
            }
            else if ((def->classID() IS CLASSID::GRADIENTDIAMOND) or (def->classID() IS CLASSID::GRADIENTCONIC)) {
               Unit val;
               if ((!error) and (!gradient->get(FID_CenterX, val))) set_dimension(tag, "cx", val);
               if ((!error) and (!gradient->get(FID_CenterY, val))) set_dimension(tag, "cy", val);
               if ((!error) and (!gradient->get(FID_Radius, val))) set_dimension(tag, "r", val);
            }

            VectorMatrix *transform;
            if ((!error) and (!gradient->get(FID_Transforms, transform)) and (transform)) {
               std::stringstream buffer;
               if (!save_svg_transform(transform, buffer)) {
                  xml::NewAttrib(tag, "gradientTransform", buffer.str());
               }
            }

            if (gradient->get<int>(FID_TotalStops) > 0) {
               GradientStop *stops;
               int total_stops, stop_index;
               if (!gradient->get(FID_Stops, stops, total_stops)) {
                  for (int s=0; (s < total_stops) and (!error); s++) {
                     if (!(error = XML->insertXML(def_index, XMI::CHILD_END, "<stop/>", &stop_index))) {
                        XTag *stop_tag;
                        error = XML->getTag(stop_index, &stop_tag);
                        if (!error) xml::NewAttrib(stop_tag, "offset", std::to_string(stops[s].Offset));

                        std::stringstream buffer;
                        buffer << "stop-color:rgb(" << stops[s].RGB.Red*255.0 << "," << stops[s].RGB.Green*255.0 << "," << stops[s].RGB.Blue*255.0 << "," << stops[s].RGB.Alpha*255.0 << ")";
                        if (!error) xml::NewAttrib(stop_tag, "style", buffer.str());
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

            auto dim = filter->get<DMF>(FID_Dimensions);

            if ((!error) and dmf::hasAnyX(dim)) set_dimension(tag, "x", filter->X, dmf::hasScaledX(dim));
            if ((!error) and dmf::hasAnyY(dim)) set_dimension(tag, "y", filter->Y, dmf::hasScaledY(dim));

            if ((!error) and dmf::hasAnyWidth(dim))
               set_dimension(tag, "width", filter->Width, dmf::hasScaledWidth(dim));

            if ((!error) and dmf::hasAnyHeight(dim))
               set_dimension(tag, "height", filter->Height, dmf::hasScaledHeight(dim));

            VUNIT units;
            if ((!error) and (!filter->get(FID_Units, (int &)units))) {
               switch(units) {
                  default:
                  case VUNIT::BOUNDING_BOX: break; // Default
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "filterUnits", "userSpaceOnUse"); break;
               }
            }

            if ((!error) and (!filter->get(FID_PrimitiveUnits, (int &)units))) {
               switch(units) {
                  default:
                  case VUNIT::USERSPACE:    break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "primitiveUnits", "objectBoundingBox"); break;
               }
            }

            std::string effect_xml;
            if ((!error) and (!filter->get(FID_EffectXML, effect_xml))) {
               error = XML->insertStatement(tag->ID, XMI::CHILD, effect_xml.c_str(), nullptr);
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
   float *colour;
   int array_size;
   ERR error = ERR::Okay;

   XTag *tag;
   if ((error = XML->getTag(TagID, &tag)) != ERR::Okay) return error;

   if (Vector->Opacity != 1.0) xml::NewAttrib(tag, "opacity", std::to_string(Vector->Opacity));
   if (Vector->FillOpacity != 1.0) xml::NewAttrib(tag, "fill-opacity", std::to_string(Vector->FillOpacity));
   if (Vector->StrokeOpacity != 1.0) xml::NewAttrib(tag, "stroke-opacity", std::to_string(Vector->StrokeOpacity));

   if ((!Vector->get(FID_Stroke, str)) and not str.empty()) {
      xml::NewAttrib(tag, "stroke", str);
   }
   else if ((!Vector->get(FID_StrokeColour, colour, array_size)) and (colour[3] != 0)) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour[0], colour[1], colour[2], colour[3]);
      xml::NewAttrib(tag, "stroke-color", buffer);
   }

   VLJ line_join;
   if ((!error) and (Vector->get(FID_LineJoin, (int &)line_join) IS ERR::Okay)) {
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
   if ((!error) and (Vector->get(FID_InnerJoin, (int &)inner_join) IS ERR::Okay)) { // Kōtuku only
      switch (inner_join) {
         default:
         case VIJ::MITER:   break; // Default
         case VIJ::BEVEL:   xml::NewAttrib(tag, "stroke-innerjoin", "bevel"); break;
         case VIJ::JAG:     xml::NewAttrib(tag, "stroke-innerjoin", "jag"); break;
         case VIJ::ROUND:   xml::NewAttrib(tag, "stroke-innerjoin", "round"); break;
         case VIJ::INHERIT: xml::NewAttrib(tag, "stroke-innerjoin", "inherit"); break;
      }
   }

   double *dash_array;
   int dash_total;
   if ((!error) and (!Vector->get(FID_DashArray, dash_array, dash_total)) and (dash_array)) {
      double dash_offset;
      if ((!Vector->get(FID_DashOffset, dash_offset)) and (dash_offset != 0)) {
         xml::NewAttrib(tag, "stroke-dashoffset", std::to_string(Vector->DashOffset));
      }

      int pos = 0;
      for (int i=0; i < dash_total; i++) {
         if (pos != 0) buffer[pos++] = ',';
         pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dash_array[i]);
         if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
      }
      xml::NewAttrib(tag, "stroke-dasharray", buffer);
   }

   VLC linecap;
   if ((!error) and (Vector->get(FID_LineCap, (int &)linecap) IS ERR::Okay)) {
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

   std::string stroke_width;
   if ((!error) and (!Vector->get(FID_StrokeWidth, stroke_width))) {
      if (stroke_width.empty()) stroke_width = "0";
      if ((stroke_width[0] != '1') and (stroke_width[1] != 0)) {
         xml::NewAttrib(tag, "stroke-width", stroke_width);
      }
   }

   if ((!error) and (!Vector->get(FID_Fill, str)) and not str.empty()) {
      if (!iequals("rgb(0,0,0)", str)) xml::NewAttrib(tag, "fill", str);
   }
   else if ((!error) and (!Vector->get(FID_FillColour, colour, array_size)) and (colour[3] != 0)) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour[0], colour[1], colour[2], colour[3]);
      xml::NewAttrib(tag, "fill", buffer);
   }

   VFR fill_rule;
   if ((!error) and (!Vector->get(FID_FillRule, (int &)fill_rule))) {
      if (fill_rule IS VFR::EVEN_ODD) xml::NewAttrib(tag, "fill-rule", "evenodd");
   }

   if ((!error) and (!(error = Vector->get(FID_ID, str))) and not str.empty()) xml::NewAttrib(tag, "id", str);

   if ((!error) and (!(error = Vector->get(FID_Filter, str))) and not str.empty()) xml::NewAttrib(tag, "filter", str);

   VectorMatrix *transform;
   if ((!error) and (!Vector->get(FID_Transforms, transform)) and (transform)) {
      std::stringstream buffer;
      if ((error = save_svg_transform(transform, buffer)) IS ERR::Okay) {
         xml::NewAttrib(tag, "transform", buffer.str());
      }
   }

   OBJECTPTR shape;
   if ((!error) and (!Vector->get(FID_Morph, shape)) and (shape)) {
      VMF morph_flags;
      XTag *morph_tag;
      error = XML->insertStatement(TagID, XMI::CHILD_END, "<kotuku:morph/>", &morph_tag);

      std::string_view shape_id;
      if ((!error) and (!shape->get(FID_ID, shape_id)) and not shape_id.empty()) {
         // NB: It is required that the shape has previously been registered as a definition, otherwise the url will refer to a dud tag.
         auto shape_ref = std::format("url(#{})", shape_id);
         xml::NewAttrib(morph_tag, "xlink:href", shape_ref);
      }

      if (!error) error = Vector->get(FID_MorphFlags, (int &)morph_flags);

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

      struct rkVectorTransition *tv;
      if ((!error) and (!Vector->get(FID_Transition, tv))) {
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
      XTag *tag;
      double rx, ry, x, y, width, height;

      error = XML->insertXML(Parent, XMI::CHILD_END, "<rect/>", &new_index);
      if (!error) error = XML->getTag(new_index, &tag);

      if (!error) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if ((!Vector->get(FID_RoundX, rx)) and (rx != 0)) set_dimension(tag, "rx", rx, FALSE);
         if ((!Vector->get(FID_RoundY, ry)) and (ry != 0)) set_dimension(tag, "ry", ry, FALSE);
         if ((!Vector->get(FID_X, x))) set_dimension(tag, "x", x, dmf::hasScaledX(dim));
         if ((!Vector->get(FID_Y, y))) set_dimension(tag, "y", y, dmf::hasScaledY(dim));
         if ((!Vector->get(FID_Width, width))) set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));
         if ((!Vector->get(FID_Height, height))) set_dimension(tag, "height", height, dmf::hasScaledHeight(dim));

         save_svg_scan_std(Self, XML, Vector, new_index);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORELLIPSE) {
      XTag *tag;
      double rx, ry, cx, cy;

      auto dim = Vector->get<DMF>(FID_Dimensions);
      if (!error) error = Vector->get(FID_RadiusX, rx);
      if (!error) error = Vector->get(FID_RadiusY, ry);
      if (!error) error = Vector->get(FID_CenterX, cx);
      if (!error) error = Vector->get(FID_CenterY, cy);
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
      XTag *tag;
      VectorPoint *points;
      int total_points, i;

      if ((!Vector->get(FID_Closed, i)) and (i IS FALSE)) { // Line or Polyline
         if (!(error = Vector->get(FID_PointsArray, points, total_points))) {
            if (total_points IS 2) {
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
                  for (i=0; i < total_points; i++) {
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

         if ((!error) and (!Vector->get(FID_PointsArray, points, total_points))) {
            for (i=0; i < total_points; i++) {
               buffer << points[i].X << "," << points[i].Y << " ";
            }
            xml::NewAttrib(tag, "points", buffer.str());
         }
      }

      double path_length;
      if ((!(error = Vector->get(FID_PathLength, path_length))) and (path_length != 0)) {
         xml::NewAttrib(tag, "pathLength", std::to_string(path_length));
      }

      if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORTEXT) {
      XTag *tag;
      double x, y, *dx, *dy, *rotate, text_length;
      int total, i, weight;
      std::string str;
      std::string_view sv;
      char buffer[1024];

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<text/>", &tag);

      if ((!error) and (!Vector->get(FID_X, x))) set_dimension(tag, "x", x, FALSE);
      if ((!error) and (!Vector->get(FID_Y, y))) set_dimension(tag, "y", y, FALSE);

      if ((!error) and (!(error = Vector->get(FID_DX, dx, total))) and (total > 0)) {
         int pos = 0;
         for (int i=0; i < total; i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dx[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dx", buffer);
      }

      if ((!error) and (!(error = Vector->get(FID_DY, dy, total))) and (total > 0)) {
         int pos = 0;
         for (i=0; i < total; i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dy[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dy", buffer);
      }

      if ((!error) and (!(error = Vector->get(FID_FontSize, str)))) {
         xml::NewAttrib(tag, "font-size", str);
      }

      if ((!error) and (!(error = Vector->get(FID_Rotate, rotate, total))) and (total > 0)) {
         std::stringstream buffer;
         bool comma = false;
         for (i=0; i < total; i++) {
            if (comma) buffer << ',';
            else comma = true;
            buffer << rotate[i];
         }
         xml::NewAttrib(tag, "rotate", buffer.str());
      }

      if ((!error) and (!(error = Vector->get(FID_TextLength, text_length))) and (text_length))
         xml::NewAttrib(tag, "textLength", std::to_string(text_length));

      if ((!error) and (!(error = Vector->get(FID_Face, sv))))
         xml::NewAttrib(tag, "font-family", sv);

      if ((!error) and (!(error = Vector->get(FID_Weight, weight))) and (weight != 400))
         xml::NewAttrib(tag, "font-weight", std::to_string(weight));

      if ((!error) and (!(error = Vector->get(FID_String, sv))))
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
      XTag *tag;
      std::string_view str;
      if ((!(error = Vector->get(FID_ID, str))) and not str.empty()) { // The id is an essential requirement
         error = XML->insertStatement(Parent, XMI::CHILD_END, "<clipPath/>", &tag);

         VUNIT units;
         if (!Vector->get(FID_Units, (int &)units)) {
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
      XTag *tag;
      double dbl;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:wave/>", &tag);

      if (!error) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (!Vector->get(FID_X, dbl)) set_dimension(tag, "x", dbl, dmf::hasScaledX(dim));
         if (!Vector->get(FID_Y, dbl)) set_dimension(tag, "y", dbl, dmf::hasScaledY(dim));
         if (!Vector->get(FID_Width, dbl)) set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (!Vector->get(FID_Height, dbl)) set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (!Vector->get(FID_Amplitude, dbl)) xml::NewAttrib(tag, "amplitude", std::to_string(dbl));
         if (!Vector->get(FID_Frequency, dbl)) xml::NewAttrib(tag, "frequency", std::to_string(dbl));
         if (!Vector->get(FID_Decay, dbl)) xml::NewAttrib(tag, "decay", std::to_string(dbl));
         if (!Vector->get(FID_Degree, dbl)) xml::NewAttrib(tag, "degree", std::to_string(dbl));

         int close;
         if (!Vector->get(FID_Close, close)) xml::NewAttrib(tag, "close", std::to_string(close));
         if (!Vector->get(FID_Thickness, dbl)) xml::NewAttrib(tag, "thickness", std::to_string(dbl));

         if (!error) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSPIRAL) {
      XTag *tag;
      double dbl;
      int length;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:spiral/>", &tag);
      if (error != ERR::Okay) return error;

      if (!error) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (!Vector->get(FID_CenterX, dbl)) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (!Vector->get(FID_CenterY, dbl)) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (!Vector->get(FID_Width, dbl)) set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (!Vector->get(FID_Height, dbl)) set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (!Vector->get(FID_Offset, dbl)) xml::NewAttrib(tag, "offset", std::to_string(dbl));
         if ((!Vector->get(FID_PathLength, length)) and (length != 0)) xml::NewAttrib(tag, "pathLength", std::to_string(length));
         if (!Vector->get(FID_Radius, dbl)) set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (!Vector->get(FID_Scale, dbl)) xml::NewAttrib(tag, "scale", std::to_string(dbl));
         if (!Vector->get(FID_Step, dbl)) xml::NewAttrib(tag, "step", std::to_string(dbl));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSHAPE) {
      XTag *tag;
      double dbl;
      int num;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:shape/>", &tag);

      if (!error) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (!Vector->get(FID_CenterX, dbl)) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (!Vector->get(FID_CenterY, dbl)) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (!Vector->get(FID_Radius, dbl)) set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (!Vector->get(FID_A, dbl)) xml::NewAttrib(tag, "a", std::to_string(dbl));
         if (!Vector->get(FID_B, dbl)) xml::NewAttrib(tag, "b", std::to_string(dbl));
         if (!Vector->get(FID_M, dbl)) xml::NewAttrib(tag, "m", std::to_string(dbl));
         if (!Vector->get(FID_N1, dbl)) xml::NewAttrib(tag, "n1", std::to_string(dbl));
         if (!Vector->get(FID_N2, dbl)) xml::NewAttrib(tag, "n2", std::to_string(dbl));
         if (!Vector->get(FID_N3, dbl)) xml::NewAttrib(tag, "n3", std::to_string(dbl));
         if (!Vector->get(FID_Phi, dbl)) xml::NewAttrib(tag, "phi", std::to_string(dbl));
         if (!Vector->get(FID_Phi, num)) xml::NewAttrib(tag, "phi", std::to_string(num));
         if (!Vector->get(FID_Vertices, num)) xml::NewAttrib(tag, "vertices", std::to_string(num));
         if (!Vector->get(FID_Mod, num)) xml::NewAttrib(tag, "mod", std::to_string(num));
         if (!Vector->get(FID_Spiral, num)) xml::NewAttrib(tag, "spiral", std::to_string(num));
         if (!Vector->get(FID_Repeat, num)) xml::NewAttrib(tag, "repeat", std::to_string(num));
         if (!Vector->get(FID_Close, num)) xml::NewAttrib(tag, "close", std::to_string(num));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORVIEWPORT) {
      XTag *tag;
      double x, y, width, height;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<svg/>", &tag);

      if (!error) error = Vector->get(FID_ViewX, x);
      if (!error) error = Vector->get(FID_ViewY, y);
      if (!error) error = Vector->get(FID_ViewWidth, width);
      if (!error) error = Vector->get(FID_ViewHeight, height);

      if (!error) {
         std::stringstream buffer;
         buffer << x << " " << y << " " << width << " " << height;
         xml::NewAttrib(tag, "viewBox", buffer.str());
      }

      if (!error) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if ((!error) and dmf::hasAnyX(dim) and (!Vector->get(FID_X, x)))
            set_dimension(tag, "x", x, dmf::hasScaledX(dim));

         if ((!error) and dmf::hasAnyY(dim) and (!Vector->get(FID_Y, y)))
            set_dimension(tag, "y", y, dmf::hasScaledY(dim));

         if ((!error) and dmf::hasAnyWidth(dim) and (!Vector->get(FID_Width, width)))
            set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));

         if ((!error) and dmf::hasAnyHeight(dim) and (!Vector->get(FID_Height, height)))
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
