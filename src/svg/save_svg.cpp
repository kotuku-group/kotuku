
static void set_dimension(XTag *Tag, const std::string Attrib, double Value, bool Scaled)
{
   if (Scaled) xml::NewAttrib(*Tag, Attrib, std::to_string(Value * 100.0) + "%");
   else xml::NewAttrib(*Tag, Attrib, std::to_string(Value));
}

//*********************************************************************************************************************

static ERR save_vectorpath(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   CSTRING path;
   ERR error;

   if ((error = Vector->get(FID_Sequence, path)) IS ERR::Okay) {
      int new_index;
      error = XML->insertXML(Parent, XMI::CHILD_END, "<path/>", &new_index);
      if (error IS ERR::Okay) {
         XTag *tag;
         error = XML->getTag(new_index, &tag);
         if (error IS ERR::Okay) xml::NewAttrib(tag, "d", path);
      }
      FreeResource(path);

      if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, new_index);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_defs(extSVG *Self, objXML *XML, objVectorScene *Scene, int Parent)
{
   pf::Log log(__FUNCTION__);
   ankerl::unordered_dense::map<std::string, OBJECTPTR> *defs;

   if (Scene->get(FID_Defs, defs) IS ERR::Okay) {
      ERR error;
      int def_index = 0;
      for (auto & [ key, def ] : *defs) {
         if (!def_index) {
            if ((error = XML->insertXML(Parent, XMI::CHILD_END, "<defs/>", &def_index)) != ERR::Okay) return error;
         }

         log.msg("Processing definition %s (%x)", def->Class->ClassName, uint32_t(def->classID()));

         if (def->classID() IS CLASSID::VECTORGRADIENT) {
            auto gradient = (objVectorGradient *)def;
            std::string gradient_type;
            switch(gradient->Type) {
               case VGT::RADIAL:  gradient_type = "<radialGradient/>"; break;
               case VGT::CONIC:   gradient_type = "<conicGradient/>"; break;
               case VGT::DIAMOND: gradient_type = "<diamondGradient/>"; break;
               case VGT::CONTOUR: gradient_type = "<contourGradient/>"; break;
               case VGT::LINEAR:
               default:           gradient_type = "<linearGradient/>"; break;
            }
            XTag *tag = nullptr;
            error = XML->insertStatement(def_index, XMI::CHILD_END, gradient_type, &tag);

            if (error IS ERR::Okay) xml::NewAttrib(tag, "id", key);

            VUNIT units;
            if ((error IS ERR::Okay) and (gradient->get(FID_Units, (int &)units) IS ERR::Okay)) {
               switch(units) {
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "gradientUnits", "userSpaceOnUse"); break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "gradientUnits", "objectBoundingBox"); break;
                  default: break;
               }
            }

            VSPREAD spread;
            if ((error IS ERR::Okay) and (gradient->get(FID_SpreadMethod, (int &)spread) IS ERR::Okay)) {
               switch(spread) {
                  default:
                  case VSPREAD::PAD:     break; // Pad is the default SVG setting
                  case VSPREAD::REFLECT: xml::NewAttrib(tag, "spreadMethod", "reflect"); break;
                  case VSPREAD::REPEAT:  xml::NewAttrib(tag, "spreadMethod", "repeat"); break;
               }
            }

            if ((gradient->Type IS VGT::LINEAR) or (gradient->Type IS VGT::CONTOUR)) {
               if (error IS ERR::Okay) {
                  xml::NewAttrib(tag, "x1", std::to_string(gradient->X1));
                  xml::NewAttrib(tag, "y1", std::to_string(gradient->Y1));
                  xml::NewAttrib(tag, "x2", std::to_string(gradient->X2));
                  xml::NewAttrib(tag, "y2", std::to_string(gradient->Y2));
               }
            }
            else if ((gradient->Type IS VGT::RADIAL) or (gradient->Type IS VGT::DIAMOND) or (gradient->Type IS VGT::CONIC)) {
               if ((error IS ERR::Okay) and ((gradient->Flags & (VGF::FIXED_CX|VGF::SCALED_CX)) != VGF::NIL))
                  set_dimension(tag, "cx", gradient->CenterX, (gradient->Flags & VGF::SCALED_CX) != VGF::NIL);

               if ((error IS ERR::Okay) and ((gradient->Flags & (VGF::FIXED_CY|VGF::SCALED_CY)) != VGF::NIL))
                  set_dimension(tag, "cy", gradient->CenterY, (gradient->Flags & VGF::SCALED_CY) != VGF::NIL);

               if ((error IS ERR::Okay) and ((gradient->Flags & (VGF::FIXED_FX|VGF::SCALED_FX)) != VGF::NIL))
                  set_dimension(tag, "fx", gradient->FocalX, (gradient->Flags & VGF::SCALED_FX) != VGF::NIL);

               if ((error IS ERR::Okay) and ((gradient->Flags & (VGF::FIXED_FY|VGF::SCALED_FY)) != VGF::NIL))
                  set_dimension(tag, "fy", gradient->FocalY, (gradient->Flags & VGF::SCALED_FY) != VGF::NIL);

               if ((error IS ERR::Okay) and ((gradient->Flags & (VGF::FIXED_RADIUS|VGF::SCALED_RADIUS)) != VGF::NIL))
                  set_dimension(tag, "r", gradient->Radius, (gradient->Flags & VGF::SCALED_RADIUS) != VGF::NIL);
            }

            VectorMatrix *transform;
            if ((error IS ERR::Okay) and (gradient->get(FID_Transforms, transform) IS ERR::Okay) and (transform)) {
               std::stringstream buffer;
               if (save_svg_transform(transform, buffer) IS ERR::Okay) {
                  xml::NewAttrib(tag, "gradientTransform", buffer.str());
               }
            }

            if (gradient->get<int>(FID_TotalStops) > 0) {
               GradientStop *stops;
               int total_stops, stop_index;
               if (gradient->get(FID_Stops, stops, total_stops) IS ERR::Okay) {
                  for (int s=0; (s < total_stops) and (error IS ERR::Okay); s++) {
                     if ((error = XML->insertXML(def_index, XMI::CHILD_END, "<stop/>", &stop_index)) IS ERR::Okay) {
                        XTag *stop_tag;
                        error = XML->getTag(stop_index, &stop_tag);
                        if (error IS ERR::Okay) xml::NewAttrib(stop_tag, "offset", std::to_string(stops[s].Offset));

                        std::stringstream buffer;
                        buffer << "stop-color:rgb(" << stops[s].RGB.Red*255.0 << "," << stops[s].RGB.Green*255.0 << "," << stops[s].RGB.Blue*255.0 << "," << stops[s].RGB.Alpha*255.0 << ")";
                        if (error IS ERR::Okay) xml::NewAttrib(stop_tag, "style", buffer.str());
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

            if (error IS ERR::Okay) xml::NewAttrib(tag, "id", key);

            auto dim = filter->get<DMF>(FID_Dimensions);

            if ((error IS ERR::Okay) and dmf::hasAnyX(dim))
               set_dimension(tag, "x", filter->X, dmf::hasScaledX(dim));

            if ((error IS ERR::Okay) and dmf::hasAnyY(dim))
               set_dimension(tag, "y", filter->Y, dmf::hasScaledY(dim));

            if ((error IS ERR::Okay) and dmf::hasAnyWidth(dim))
               set_dimension(tag, "width", filter->Width, dmf::hasScaledWidth(dim));

            if ((error IS ERR::Okay) and dmf::hasAnyHeight(dim))
               set_dimension(tag, "height", filter->Height, dmf::hasScaledHeight(dim));

            VUNIT units;
            if ((error IS ERR::Okay) and (filter->get(FID_Units, (int &)units) IS ERR::Okay)) {
               switch(units) {
                  default:
                  case VUNIT::BOUNDING_BOX: break; // Default
                  case VUNIT::USERSPACE:    xml::NewAttrib(tag, "filterUnits", "userSpaceOnUse"); break;
               }
            }

            if ((error IS ERR::Okay) and (filter->get(FID_PrimitiveUnits, (int &)units) IS ERR::Okay)) {
               switch(units) {
                  default:
                  case VUNIT::USERSPACE:    break;
                  case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "primitiveUnits", "objectBoundingBox"); break;
               }
            }

            CSTRING effect_xml;
            if ((error IS ERR::Okay) and (filter->get(FID_EffectXML, effect_xml) IS ERR::Okay)) {
               error = XML->insertStatement(tag->ID, XMI::CHILD, effect_xml, nullptr);
               FreeResource(effect_xml);
            }
         }
         else if (def->classID() IS CLASSID::VECTORTRANSITION) {
            log.warning("VectorTransition not supported.");
         }
         else if (def->classID() IS CLASSID::VECTORCLIP) {
            log.warning("VectorClip not supported.");
         }
         else if (def->Class->BaseClassID IS CLASSID::VECTOR) {
            log.warning("%s not supported.", def->Class->ClassName);
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
   pf::Log log(__FUNCTION__);
   char buffer[160];
   CSTRING str;
   float *colour;
   int array_size;
   ERR error = ERR::Okay;

   XTag *tag;
   if ((error = XML->getTag(TagID, &tag)) != ERR::Okay) return error;

   if (Vector->Opacity != 1.0) xml::NewAttrib(tag, "opacity", std::to_string(Vector->Opacity));
   if (Vector->FillOpacity != 1.0) xml::NewAttrib(tag, "fill-opacity", std::to_string(Vector->FillOpacity));
   if (Vector->StrokeOpacity != 1.0) xml::NewAttrib(tag, "stroke-opacity", std::to_string(Vector->StrokeOpacity));

   if ((Vector->get(FID_Stroke, str) IS ERR::Okay) and (str)) {
      xml::NewAttrib(tag, "stroke", str);
   }
   else if ((Vector->get(FID_StrokeColour, colour, array_size) IS ERR::Okay) and (colour[3] != 0)) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour[0], colour[1], colour[2], colour[3]);
      xml::NewAttrib(tag, "stroke-color", buffer);
   }

   VLJ line_join;
   if ((error IS ERR::Okay) and (Vector->get(FID_LineJoin, (int &)line_join) IS ERR::Okay)) {
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
   if ((error IS ERR::Okay) and (Vector->get(FID_InnerJoin, (int &)inner_join) IS ERR::Okay)) { // Kōtuku only
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
   if ((error IS ERR::Okay) and (Vector->get(FID_DashArray, dash_array, dash_total) IS ERR::Okay) and (dash_array)) {
      double dash_offset;
      if ((Vector->get(FID_DashOffset, dash_offset) IS ERR::Okay) and (dash_offset != 0)) {
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
   if ((error IS ERR::Okay) and (Vector->get(FID_LineCap, (int &)linecap) IS ERR::Okay)) {
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

   CSTRING stroke_width;
   if ((error IS ERR::Okay) and (Vector->get(FID_StrokeWidth, stroke_width) IS ERR::Okay)) {
      if (!stroke_width) stroke_width = "0";
      if ((stroke_width[0] != '1') and (stroke_width[1] != 0)) {
         xml::NewAttrib(tag, "stroke-width", stroke_width);
      }
   }

   if ((error IS ERR::Okay) and (Vector->get(FID_Fill, str) IS ERR::Okay) and (str)) {
      if (!iequals("rgb(0,0,0)", str)) xml::NewAttrib(tag, "fill", str);
   }
   else if ((error IS ERR::Okay) and (Vector->get(FID_FillColour, colour, array_size) IS ERR::Okay) and (colour[3] != 0)) {
      snprintf(buffer, sizeof(buffer), "rgb(%g,%g,%g,%g)", colour[0], colour[1], colour[2], colour[3]);
      xml::NewAttrib(tag, "fill", buffer);
   }

   VFR fill_rule;
   if ((error IS ERR::Okay) and (Vector->get(FID_FillRule, (int &)fill_rule) IS ERR::Okay)) {
      if (fill_rule IS VFR::EVEN_ODD) xml::NewAttrib(tag, "fill-rule", "evenodd");
   }

   if ((error IS ERR::Okay) and ((error = Vector->get(FID_ID, str)) IS ERR::Okay) and (str)) xml::NewAttrib(tag, "id", str);

   if ((error IS ERR::Okay) and (Vector->get(FID_Filter, str) IS ERR::Okay) and (str)) xml::NewAttrib(tag, "filter", str);

   VectorMatrix *transform;
   if ((error IS ERR::Okay) and (Vector->get(FID_Transforms, transform) IS ERR::Okay) and (transform)) {
      std::stringstream buffer;
      if ((error = save_svg_transform(transform, buffer)) IS ERR::Okay) {
         xml::NewAttrib(tag, "transform", buffer.str());
      }
   }

   OBJECTPTR shape;
   if ((error IS ERR::Okay) and (Vector->get(FID_Morph, shape) IS ERR::Okay) and (shape)) {
      VMF morph_flags;
      XTag *morph_tag;
      error = XML->insertStatement(TagID, XMI::CHILD_END, "<kotuku:morph/>", &morph_tag);

      CSTRING shape_id;
      if ((error IS ERR::Okay) and (shape->get(FID_ID, shape_id) IS ERR::Okay) and (shape_id)) {
         // NB: It is required that the shape has previously been registered as a definition, otherwise the url will refer to a dud tag.
         char shape_ref[120];
         snprintf(shape_ref, sizeof(shape_ref), "url(#%s)", shape_id);
         xml::NewAttrib(morph_tag, "xlink:href", shape_ref);
      }

      if (error IS ERR::Okay) error = Vector->get(FID_MorphFlags, (int &)morph_flags);

      if ((error IS ERR::Okay) and ((morph_flags & VMF::STRETCH) != VMF::NIL)) xml::NewAttrib(morph_tag, "method", "stretch");
      if ((error IS ERR::Okay) and ((morph_flags & VMF::AUTO_SPACING) != VMF::NIL)) xml::NewAttrib(morph_tag, "spacing", "auto");

      if ((error IS ERR::Okay) and ((morph_flags & (VMF::X_MIN|VMF::X_MID|VMF::X_MAX|VMF::Y_MIN|VMF::Y_MID|VMF::Y_MAX)) != VMF::NIL)) {
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
      if ((error IS ERR::Okay) and (Vector->get(FID_Transition, tv) IS ERR::Okay)) {
         // TODO save_svg_scan_std transition support





      }
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   pf::Log log(__FUNCTION__);

   int new_index = -1;

   log.branch("%s", Vector->Class->ClassName);

   ERR error = ERR::Okay;
   if (Vector->classID() IS CLASSID::VECTORRECTANGLE) {
      XTag *tag;
      double rx, ry, x, y, width, height;

      error = XML->insertXML(Parent, XMI::CHILD_END, "<rect/>", &new_index);
      if (error IS ERR::Okay) error = XML->getTag(new_index, &tag);

      if (error IS ERR::Okay) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if ((Vector->get(FID_RoundX, rx) IS ERR::Okay) and (rx != 0)) set_dimension(tag, "rx", rx, FALSE);
         if ((Vector->get(FID_RoundY, ry) IS ERR::Okay) and (ry != 0)) set_dimension(tag, "ry", ry, FALSE);
         if ((Vector->get(FID_X, x) IS ERR::Okay)) set_dimension(tag, "x", x, dmf::hasScaledX(dim));
         if ((Vector->get(FID_Y, y) IS ERR::Okay)) set_dimension(tag, "y", y, dmf::hasScaledY(dim));
         if ((Vector->get(FID_Width, width) IS ERR::Okay)) set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));
         if ((Vector->get(FID_Height, height) IS ERR::Okay)) set_dimension(tag, "height", height, dmf::hasScaledHeight(dim));

         save_svg_scan_std(Self, XML, Vector, new_index);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORELLIPSE) {
      XTag *tag;
      double rx, ry, cx, cy;

      auto dim = Vector->get<DMF>(FID_Dimensions);
      if (error IS ERR::Okay) error = Vector->get(FID_RadiusX, rx);
      if (error IS ERR::Okay) error = Vector->get(FID_RadiusY, ry);
      if (error IS ERR::Okay) error = Vector->get(FID_CenterX, cx);
      if (error IS ERR::Okay) error = Vector->get(FID_CenterY, cy);
      if (error IS ERR::Okay) error = XML->insertStatement(Parent, XMI::CHILD_END, "<ellipse/>", &tag);

      if (error IS ERR::Okay) {
         set_dimension(tag, "rx", rx, dmf::hasScaledRadiusX(dim));
         set_dimension(tag, "ry", ry, dmf::hasScaledRadiusY(dim));
         set_dimension(tag, "cx", cx, dmf::hasScaledCenterX(dim));
         set_dimension(tag, "cy", cy, dmf::hasScaledCenterY(dim));
      }

      if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, new_index);
   }
   else if (Vector->classID() IS CLASSID::VECTORPATH) {
      error = save_vectorpath(Self, XML, Vector, Parent);
   }
   else if (Vector->classID() IS CLASSID::VECTORPOLYGON) { // Serves <polygon>, <line> and <polyline>
      XTag *tag;
      VectorPoint *points;
      int total_points, i;

      if ((Vector->get(FID_Closed, i) IS ERR::Okay) and (i IS FALSE)) { // Line or Polyline
         if ((error = Vector->get(FID_PointsArray, points, total_points)) IS ERR::Okay) {
            if (total_points IS 2) {
               error = XML->insertStatement(Parent, XMI::CHILD_END, "<line/>", &tag);
               if (error IS ERR::Okay) {
                  set_dimension(tag, "x1", points[0].X, points[0].XScaled);
                  set_dimension(tag, "y1", points[0].Y, points[0].YScaled);
                  set_dimension(tag, "x2", points[1].X, points[1].XScaled);
                  set_dimension(tag, "y2", points[1].Y, points[1].YScaled);
               }
            }
            else {
               std::stringstream buffer;
               error = XML->insertStatement(Parent, XMI::CHILD_END, "<polyline/>", &tag);
               if (error IS ERR::Okay) {
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

         if ((error IS ERR::Okay) and (Vector->get(FID_PointsArray, points, total_points) IS ERR::Okay)) {
            for (i=0; i < total_points; i++) {
               buffer << points[i].X << "," << points[i].Y << " ";
            }
            xml::NewAttrib(tag, "points", buffer.str());
         }
      }

      double path_length;
      if (((error = Vector->get(FID_PathLength, path_length)) IS ERR::Okay) and (path_length != 0)) {
         xml::NewAttrib(tag, "pathLength", std::to_string(path_length));
      }

      if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORTEXT) {
      XTag *tag;
      double x, y, *dx, *dy, *rotate, text_length;
      int total, i, weight;
      CSTRING str;
      char buffer[1024];

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<text/>", &tag);

      if ((error IS ERR::Okay) and (Vector->get(FID_X, x) IS ERR::Okay)) set_dimension(tag, "x", x, FALSE);
      if ((error IS ERR::Okay) and (Vector->get(FID_Y, y) IS ERR::Okay)) set_dimension(tag, "y", y, FALSE);

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_DX, dx, total)) IS ERR::Okay) and (total > 0)) {
         int pos = 0;
         for (int i=0; i < total; i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dx[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dx", buffer);
      }

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_DY, dy, total)) IS ERR::Okay) and (total > 0)) {
         int pos = 0;
         for (i=0; i < total; i++) {
            if (pos != 0) buffer[pos++] = ',';
            pos += snprintf(buffer+pos, sizeof(buffer)-pos, "%g", dy[i]);
            if ((size_t)pos >= sizeof(buffer)-2) return ERR::BufferOverflow;
         }
         xml::NewAttrib(tag, "dy", buffer);
      }

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_FontSize, str)) IS ERR::Okay)) {
         xml::NewAttrib(tag, "font-size", str);
         FreeResource(str);
      }

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_Rotate, rotate, total)) IS ERR::Okay) and (total > 0)) {
         std::stringstream buffer;
         bool comma = false;
         for (i=0; i < total; i++) {
            if (comma) buffer << ',';
            else comma = true;
            buffer << rotate[i];
         }
         xml::NewAttrib(tag, "rotate", buffer.str());
      }

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_TextLength, text_length)) IS ERR::Okay) and (text_length))
         xml::NewAttrib(tag, "textLength", std::to_string(text_length));

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_Face, str)) IS ERR::Okay))
         xml::NewAttrib(tag, "font-family", str);

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_Weight, weight)) IS ERR::Okay) and (weight != 400))
         xml::NewAttrib(tag, "font-weight", std::to_string(weight));

      if ((error IS ERR::Okay) and ((error = Vector->get(FID_String, str)) IS ERR::Okay))
         error = XML->insertContent(tag->ID, XMI::CHILD, str, nullptr);

      // TODO: lengthAdjust, font, font-size-adjust, font-stretch, font-style, font-variant, text-anchor, kerning, letter-spacing, path-length, word-spacing, text-decoration

      if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORGROUP) {
      XTag *tag;
      error = XML->insertStatement(Parent, XMI::CHILD_END, "<g/>", &tag);
      if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   else if (Vector->classID() IS CLASSID::VECTORCLIP) {
      XTag *tag;
      CSTRING str;
      if (((error = Vector->get(FID_ID, str)) IS ERR::Okay) and (str)) { // The id is an essential requirement
         error = XML->insertStatement(Parent, XMI::CHILD_END, "<clipPath/>", &tag);

         VUNIT units;
         if (Vector->get(FID_Units, (int &)units) IS ERR::Okay) {
            switch(units) {
               default:
               case VUNIT::USERSPACE:    break; // Default
               case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "clipPathUnits", "objectBoundingBox"); break;
            }
         }

         if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORWAVE) {
      XTag *tag;
      double dbl;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:wave/>", &tag);

      if (error IS ERR::Okay) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (Vector->get(FID_X, dbl) IS ERR::Okay) set_dimension(tag, "x", dbl, dmf::hasScaledX(dim));
         if (Vector->get(FID_Y, dbl) IS ERR::Okay) set_dimension(tag, "y", dbl, dmf::hasScaledY(dim));
         if (Vector->get(FID_Width, dbl) IS ERR::Okay) set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (Vector->get(FID_Height, dbl) IS ERR::Okay) set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (Vector->get(FID_Amplitude, dbl) IS ERR::Okay) xml::NewAttrib(tag, "amplitude", std::to_string(dbl));
         if (Vector->get(FID_Frequency, dbl) IS ERR::Okay) xml::NewAttrib(tag, "frequency", std::to_string(dbl));
         if (Vector->get(FID_Decay, dbl) IS ERR::Okay) xml::NewAttrib(tag, "decay", std::to_string(dbl));
         if (Vector->get(FID_Degree, dbl) IS ERR::Okay) xml::NewAttrib(tag, "degree", std::to_string(dbl));

         int close;
         if (Vector->get(FID_Close, close) IS ERR::Okay) xml::NewAttrib(tag, "close", std::to_string(close));
         if (Vector->get(FID_Thickness, dbl) IS ERR::Okay) xml::NewAttrib(tag, "thickness", std::to_string(dbl));

         if (error IS ERR::Okay) error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSPIRAL) {
      XTag *tag;
      double dbl;
      int length;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:spiral/>", &tag);
      if (error != ERR::Okay) return error;

      if (error IS ERR::Okay) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (Vector->get(FID_CenterX, dbl) IS ERR::Okay) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (Vector->get(FID_CenterY, dbl) IS ERR::Okay) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (Vector->get(FID_Width, dbl) IS ERR::Okay) set_dimension(tag, "width", dbl, dmf::hasScaledWidth(dim));
         if (Vector->get(FID_Height, dbl) IS ERR::Okay) set_dimension(tag, "height", dbl, dmf::hasScaledHeight(dim));
         if (Vector->get(FID_Offset, dbl) IS ERR::Okay) xml::NewAttrib(tag, "offset", std::to_string(dbl));
         if ((Vector->get(FID_PathLength, length) IS ERR::Okay) and (length != 0)) xml::NewAttrib(tag, "pathLength", std::to_string(length));
         if (Vector->get(FID_Radius, dbl) IS ERR::Okay) set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (Vector->get(FID_Scale, dbl) IS ERR::Okay) xml::NewAttrib(tag, "scale", std::to_string(dbl));
         if (Vector->get(FID_Step, dbl) IS ERR::Okay) xml::NewAttrib(tag, "step", std::to_string(dbl));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORSHAPE) {
      XTag *tag;
      double dbl;
      int num;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:shape/>", &tag);

      if (error IS ERR::Okay) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if (Vector->get(FID_CenterX, dbl) IS ERR::Okay) set_dimension(tag, "cx", dbl, dmf::hasScaledCenterX(dim));
         if (Vector->get(FID_CenterY, dbl) IS ERR::Okay) set_dimension(tag, "cy", dbl, dmf::hasScaledCenterY(dim));
         if (Vector->get(FID_Radius, dbl) IS ERR::Okay) set_dimension(tag, "r", dbl, dmf::hasAnyScaledRadius(dim));
         if (Vector->get(FID_A, dbl) IS ERR::Okay) xml::NewAttrib(tag, "a", std::to_string(dbl));
         if (Vector->get(FID_B, dbl) IS ERR::Okay) xml::NewAttrib(tag, "b", std::to_string(dbl));
         if (Vector->get(FID_M, dbl) IS ERR::Okay) xml::NewAttrib(tag, "m", std::to_string(dbl));
         if (Vector->get(FID_N1, dbl) IS ERR::Okay) xml::NewAttrib(tag, "n1", std::to_string(dbl));
         if (Vector->get(FID_N2, dbl) IS ERR::Okay) xml::NewAttrib(tag, "n2", std::to_string(dbl));
         if (Vector->get(FID_N3, dbl) IS ERR::Okay) xml::NewAttrib(tag, "n3", std::to_string(dbl));
         if (Vector->get(FID_Phi, dbl) IS ERR::Okay) xml::NewAttrib(tag, "phi", std::to_string(dbl));
         if (Vector->get(FID_Phi, num) IS ERR::Okay) xml::NewAttrib(tag, "phi", std::to_string(num));
         if (Vector->get(FID_Vertices, num) IS ERR::Okay) xml::NewAttrib(tag, "vertices", std::to_string(num));
         if (Vector->get(FID_Mod, num) IS ERR::Okay) xml::NewAttrib(tag, "mod", std::to_string(num));
         if (Vector->get(FID_Spiral, num) IS ERR::Okay) xml::NewAttrib(tag, "spiral", std::to_string(num));
         if (Vector->get(FID_Repeat, num) IS ERR::Okay) xml::NewAttrib(tag, "repeat", std::to_string(num));
         if (Vector->get(FID_Close, num) IS ERR::Okay) xml::NewAttrib(tag, "close", std::to_string(num));

         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }
   else if (Vector->classID() IS CLASSID::VECTORVIEWPORT) {
      XTag *tag;
      double x, y, width, height;

      error = XML->insertStatement(Parent, XMI::CHILD_END, "<svg/>", &tag);

      if (error IS ERR::Okay) error = Vector->get(FID_ViewX, x);
      if (error IS ERR::Okay) error = Vector->get(FID_ViewY, y);
      if (error IS ERR::Okay) error = Vector->get(FID_ViewWidth, width);
      if (error IS ERR::Okay) error = Vector->get(FID_ViewHeight, height);

      if (error IS ERR::Okay) {
         std::stringstream buffer;
         buffer << x << " " << y << " " << width << " " << height;
         xml::NewAttrib(tag, "viewBox", buffer.str());
      }

      if (error IS ERR::Okay) {
         auto dim = Vector->get<DMF>(FID_Dimensions);
         if ((error IS ERR::Okay) and dmf::hasAnyX(dim) and (Vector->get(FID_X, x) IS ERR::Okay))
            set_dimension(tag, "x", x, dmf::hasScaledX(dim));

         if ((error IS ERR::Okay) and dmf::hasAnyY(dim) and (Vector->get(FID_Y, y) IS ERR::Okay))
            set_dimension(tag, "y", y, dmf::hasScaledY(dim));

         if ((error IS ERR::Okay) and dmf::hasAnyWidth(dim) and (Vector->get(FID_Width, width) IS ERR::Okay))
            set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));

         if ((error IS ERR::Okay) and dmf::hasAnyHeight(dim) and (Vector->get(FID_Height, height) IS ERR::Okay))
            set_dimension(tag, "height", height, dmf::hasScaledHeight(dim));
      }
   }
   else {
      log.msg("Unrecognised class \"%s\"", Vector->Class->ClassName);
      return ERR::Okay; // Skip objects in the scene graph that we don't recognise
   }

   if (error IS ERR::Okay) {
      for (auto scan=Vector->Child; scan; scan=scan->Next) {
         save_svg_scan(Self, XML, scan, new_index);
      }
   }

   return error;
}
