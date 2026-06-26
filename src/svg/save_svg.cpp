
// Format a floating point value for SVG output without trailing zeros, e.g. "10" rather than "10.000000".  Required
// where the result is concatenated into a larger string (such as a percentage); plain attribute values are handled
// by xml::NewAttrib() directly.

inline std::string fmt_num(double Value)
{
   return xml::attrib_to_string(Value);
}

static void set_dimension(XTag *Tag, const std::string Attrib, double Value, bool Scaled)
{
   if (Scaled) xml::NewAttrib(*Tag, Attrib, fmt_num(Value * 100.0) + "%");
   else xml::NewAttrib(*Tag, Attrib, fmt_num(Value));
}

static void set_dimension(XTag *Tag, const std::string Attrib, Unit &Value)
{
   if (Value.defined()) {
      if (Value.scaled()) xml::NewAttrib(*Tag, Attrib, fmt_num(double(Value) * 100.0) + "%");
      else xml::NewAttrib(*Tag, Attrib, fmt_num(double(Value)));
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

static void save_mesh_stop(objXML *XML, int Parent, double C0X, double C0Y, double C1X, double C1Y, double P1X,
   double P1Y, const FRGB &Colour)
{
   int stop_index;
   if (XML->insertXML(Parent, XMI::CHILD_END, "<stop/>", &stop_index) != ERR::Okay) return;

   XTag *stop_tag;
   if (XML->getTag(stop_index, &stop_tag) != ERR::Okay) return;

   std::stringstream path;
   path << "C " << C0X << " " << C0Y << " " << C1X << " " << C1Y << " " << P1X << " " << P1Y;
   xml::NewAttrib(stop_tag, "path", path.str());

   std::stringstream colour;
   colour << "rgb(" << Colour.Red * 255.0 << "," << Colour.Green * 255.0 << "," << Colour.Blue * 255.0 << ")";
   xml::NewAttrib(stop_tag, "stop-color", colour.str());
   if (Colour.Alpha != 1.0) xml::NewAttrib(stop_tag, "stop-opacity", Colour.Alpha);
}

static bool mesh_colour_match(const FRGB &A, const FRGB &B) noexcept
{
   return (A.Red IS B.Red) and (A.Green IS B.Green) and (A.Blue IS B.Blue) and (A.Alpha IS B.Alpha);
}

static bool mesh_patch_shares_previous_left_edge(const MeshPatchRecord &Patch, const MeshPatchRecord &Previous) noexcept
{
   return (Patch.TopP0X IS Previous.RightP0X) and (Patch.TopP0Y IS Previous.RightP0Y) and
      (Patch.BottomP1X IS Previous.RightP1X) and (Patch.BottomP1Y IS Previous.RightP1Y) and
      (Patch.LeftP0X IS Previous.RightP1X) and (Patch.LeftP0Y IS Previous.RightP1Y) and
      (Patch.LeftC0X IS Previous.RightC1X) and (Patch.LeftC0Y IS Previous.RightC1Y) and
      (Patch.LeftC1X IS Previous.RightC0X) and (Patch.LeftC1Y IS Previous.RightC0Y) and
      (Patch.LeftP1X IS Previous.RightP0X) and (Patch.LeftP1Y IS Previous.RightP0Y) and
      mesh_colour_match(Patch.TopLeft, Previous.TopRight) and
      mesh_colour_match(Patch.BottomLeft, Previous.BottomRight);
}

static void save_mesh_patch(objXML *XML, int RowIndex, int ColIndex, int Parent, const MeshPatchRecord &Patch,
   bool Independent)
{
   int patch_index;
   if (XML->insertXML(Parent, XMI::CHILD_END, "<meshpatch/>", &patch_index) != ERR::Okay) return;

   XTag *patch_tag;
   if ((Independent) and (XML->getTag(patch_index, &patch_tag) IS ERR::Okay)) {
      xml::NewAttrib(patch_tag, "kotuku:x", Patch.TopP0X);
      xml::NewAttrib(patch_tag, "kotuku:y", Patch.TopP0Y);
   }

   if ((RowIndex IS 0) or (Independent)) {
      save_mesh_stop(XML, patch_index, Patch.TopC0X, Patch.TopC0Y, Patch.TopC1X, Patch.TopC1Y,
         Patch.TopP1X, Patch.TopP1Y, Patch.TopRight);
   }

   save_mesh_stop(XML, patch_index, Patch.RightC0X, Patch.RightC0Y, Patch.RightC1X, Patch.RightC1Y,
      Patch.RightP1X, Patch.RightP1Y, Patch.BottomRight);
   save_mesh_stop(XML, patch_index, Patch.BottomC0X, Patch.BottomC0Y, Patch.BottomC1X, Patch.BottomC1Y,
      Patch.BottomP1X, Patch.BottomP1Y, Patch.BottomLeft);

   if ((ColIndex IS 0) or (Independent)) {
      save_mesh_stop(XML, patch_index, Patch.LeftC0X, Patch.LeftC0Y, Patch.LeftC1X, Patch.LeftC1Y,
         Patch.LeftP1X, Patch.LeftP1Y, Patch.TopLeft);
   }
}

static void save_mesh_gradient(objXML *XML, XTag *Tag, objGradientMesh *Mesh)
{
   std::span<MeshPatchRecord> patches;
   if ((Mesh->getPatches(patches) != ERR::Okay) or patches.empty()) return;

   xml::NewAttrib(Tag, "x", patches[0].TopP0X);
   xml::NewAttrib(Tag, "y", patches[0].TopP0Y);

   int row_index;
   if (XML->insertXML(Tag->ID, XMI::CHILD_END, "<meshrow/>", &row_index) != ERR::Okay) return;
   for (int col=0; col < int(patches.size()); col++) {
      bool independent = false;
      if (col > 0) {
         independent = not mesh_patch_shares_previous_left_edge(patches[size_t(col)], patches[size_t(col - 1)]);
      }
      save_mesh_patch(XML, 0, col, row_index, patches[size_t(col)], independent);
   }
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
               case CLASSID::GRADIENTMESH:    gradient_type = "<meshgradient/>"; break;
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
               xml::NewAttrib(tag, "resolution", resolution);
            }

            if (def->classID() IS CLASSID::GRADIENTLINEAR) {
               auto linear = (objGradientLinear *)gradient;
               Unit val;
               if ((!error) and (!linear->getX1(val))) set_dimension(tag, "x1", val);
               if ((!error) and (!linear->getY1(val))) set_dimension(tag, "y1", val);
               if ((!error) and (!linear->getX2(val))) set_dimension(tag, "x2", val);
               if ((!error) and (!linear->getY2(val))) set_dimension(tag, "y2", val);
            }
            else if (def->classID() IS CLASSID::GRADIENTCONTOUR) {
               auto contour = (objGradientContour *)gradient;
               double val;
               if ((!error) and (!contour->getFloor(val))) xml::NewAttrib(tag, "floor", fmt_num(val));
               if ((!error) and (!contour->getMultiplier(val))) xml::NewAttrib(tag, "multiplier", fmt_num(val));
            }
            else if (def->classID() IS CLASSID::GRADIENTDISTAL) {
               auto distal = (objGradientDistal *)gradient;
               double val;
               Unit unit;
               if ((!error) and (!distal->getFloor(val))) xml::NewAttrib(tag, "floor", fmt_num(val));
               if ((!error) and (!distal->getMultiplier(val))) xml::NewAttrib(tag, "multiplier", fmt_num(val));
               if ((!error) and (!distal->getRadius(unit)) and (unit.defined()) and (unit > 0.0)) {
                  set_dimension(tag, "radius", unit);
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
                     xml::NewAttrib(tag, "span", span);
                  }
               }
               else {
                  auto diamond = (objGradientDiamond *)gradient;
                  if ((!error) and (!diamond->getCX(val))) set_dimension(tag, "cx", val);
                  if ((!error) and (!diamond->getCY(val))) set_dimension(tag, "cy", val);
                  if ((!error) and (!diamond->getRadius(val))) set_dimension(tag, "r", val);
               }
            }
            else if (def->classID() IS CLASSID::GRADIENTMESH) {
               save_mesh_gradient(XML, tag, (objGradientMesh *)gradient);
            }

            std::span<VectorMatrix> transform;
            if ((!error) and (!gradient->getMatrices(transform)) and (not transform.empty())) {
               std::stringstream buffer;
               if (!save_svg_transform(transform.data(), buffer)) {
                  xml::NewAttrib(tag, "gradientTransform", buffer.str());
               }
            }

            std::span<GradientStop> stops;
            if ((def->classID() != CLASSID::GRADIENTMESH) and (!gradient->getStops(stops))) {
               for (size_t s=0; (s < stops.size()) and (!error); s++) {
                  auto &stop = stops[s];
                  int stop_index;
                  if (!(error = XML->insertXML(tag->ID, XMI::CHILD_END, "<stop/>", &stop_index))) {
                     XTag *stop_tag;
                     error = XML->getTag(stop_index, &stop_tag);
                     if (!error) xml::NewAttrib(stop_tag, "offset", stop.Offset);

                     std::stringstream buffer;
                     buffer << "rgb(" << stop.RGB.Red*255.0 << "," << stop.RGB.Green*255.0 << ","
                        << stop.RGB.Blue*255.0 << ")";
                     if (!error) xml::NewAttrib(stop_tag, "stop-color", buffer.str());
                     if ((!error) and (stop.RGB.Alpha != 1.0)) {
                        xml::NewAttrib(stop_tag, "stop-opacity", stop.RGB.Alpha);
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

            if ((!error) and filter->X.defined()) set_dimension(tag, "x", filter->X);
            if ((!error) and filter->Y.defined()) set_dimension(tag, "y", filter->Y);
            if ((!error) and filter->Width.defined()) set_dimension(tag, "width", filter->Width);
            if ((!error) and filter->Height.defined()) set_dimension(tag, "height", filter->Height);

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

   if (Vector->Opacity != 1.0) xml::NewAttrib(tag, "opacity", (Vector->Opacity));
   if (Vector->FillOpacity != 1.0) xml::NewAttrib(tag, "fill-opacity", (Vector->FillOpacity));
   if (Vector->StrokeOpacity != 1.0) xml::NewAttrib(tag, "stroke-opacity", (Vector->StrokeOpacity));

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
         xml::NewAttrib(tag, "stroke-dashoffset", (Vector->DashOffset));
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
      if (stroke_width.scaled()) {
         xml::NewAttrib(tag, "stroke-width", fmt_num(double(stroke_width)) + "%");
      }
      else if (stroke_width != 1) {
         xml::NewAttrib(tag, "stroke-width", fmt_num(double(stroke_width)));
      }
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
// Per-class serialisation handlers.  Each handler inserts the appropriate SVG element under Parent and, on success,
// sets ChildIndex to the XML node index that any child vectors should be attached to (or leaves it untouched at -1
// when no element was created, e.g. an unrecognised class or a clip with no id).  The dispatcher uses ChildIndex for
// the recursion into Vector->Child.

static ERR save_svg_scan_rectangle(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto rect = (objVectorRectangle *)Vector;
   XTag *tag;
   Unit rx, ry, x, y, width, height;
   int new_index;

   ERR error = XML->insertXML(Parent, XMI::CHILD_END, "<rect/>", &new_index);
   if (!error) error = XML->getTag(new_index, &tag);

   if (!error) {
      if ((!rect->getRoundX(rx)) and rx.defined() and (rx > 0)) set_dimension(tag, "rx", rx);
      if ((!rect->getRoundY(ry)) and ry.defined() and (ry > 0)) set_dimension(tag, "ry", ry);
      if ((!rect->getX(x))) set_dimension(tag, "x", x);
      if ((!rect->getY(y))) set_dimension(tag, "y", y);
      if ((!rect->getWidth(width))) set_dimension(tag, "width", width);
      if ((!rect->getHeight(height))) set_dimension(tag, "height", height);

      ChildIndex = new_index;
      error = save_svg_scan_std(Self, XML, Vector, new_index);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_ellipse(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto ellipse = (objVectorEllipse *)Vector;
   XTag *tag;
   Unit rx, ry, cx, cy;
   ERR error;

   if (!(error = ellipse->getRadiusX(rx)) and !(error = ellipse->getRadiusY(ry)) and
       !(error = ellipse->getCX(cx)) and !(error = ellipse->getCY(cy)) and
       !(error = XML->insertStatement(Parent, XMI::CHILD_END, "<ellipse/>", &tag))) {
      set_dimension(tag, "rx", rx);
      set_dimension(tag, "ry", ry);
      set_dimension(tag, "cx", cx);
      set_dimension(tag, "cy", cy);

      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************
// Serves <polygon>, <line> and <polyline>

static ERR save_svg_scan_polygon(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto vp = (objVectorPolygon *)Vector;
   XTag *tag = nullptr;
   std::span<VectorPoint> points;
   int i;
   ERR error = ERR::Okay;

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
      xml::NewAttrib(tag, "pathLength", (path_length));
   }

   if (!error) {
      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_text(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto vt = (objVectorText *)Vector;
   XTag *tag;
   Unit x, y;
   double text_length;
   int weight;
   std::string str;
   std::string_view sv;
   char buffer[1024];

   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<text/>", &tag);

   if ((!error) and (!vt->getX(x))) set_dimension(tag, "x", x);
   if ((!error) and (!vt->getY(y))) set_dimension(tag, "y", y);

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
      xml::NewAttrib(tag, "textLength", text_length);

   if ((!error) and (!(error = vt->getFace(sv))))
      xml::NewAttrib(tag, "font-family", sv);

   if ((!error) and (!(error = vt->getWeight(weight))) and (weight != 400))
      xml::NewAttrib(tag, "font-weight", weight);

   if ((!error) and (!(error = vt->getString(sv))))
      error = XML->insertContent(tag->ID, XMI::CHILD, sv, nullptr);

   // TODO: lengthAdjust, font, font-size-adjust, font-stretch, font-style, font-variant, text-anchor, kerning, letter-spacing, path-length, word-spacing, text-decoration

   if (!error) {
      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_group(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   XTag *tag;
   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<g/>", &tag);
   if (!error) {
      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }
   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_clip(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto clip = (objVectorClip *)Vector;
   XTag *tag;
   std::string_view str;
   ERR error = ERR::Okay;

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

      if (!error) {
         ChildIndex = tag->ID;
         error = save_svg_scan_std(Self, XML, Vector, tag->ID);
      }
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_wave(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto wave = (objVectorWave *)Vector;
   XTag *tag;
   double dbl;
   Unit unit;

   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:wave/>", &tag);

   if (!error) {
      if (!wave->getX(unit))        set_dimension(tag, "x", unit);
      if (!wave->getY(unit))        set_dimension(tag, "y", unit);
      if (!wave->getWidth(unit))    set_dimension(tag, "width", unit);
      if (!wave->getHeight(unit))   set_dimension(tag, "height", unit);
      if (!wave->getAmplitude(dbl)) xml::NewAttrib(tag, "amplitude", dbl);
      if (!wave->getFrequency(dbl)) xml::NewAttrib(tag, "frequency", dbl);
      if (!wave->getDecay(dbl))     xml::NewAttrib(tag, "decay", dbl);
      if (!wave->getDegree(dbl))    xml::NewAttrib(tag, "degree", dbl);

      int close;
      if (!wave->getClose(close)) xml::NewAttrib(tag, "close", close);
      if (!wave->getThickness(dbl)) xml::NewAttrib(tag, "thickness", dbl);

      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_spiral(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto spiral = (objVectorSpiral *)Vector;
   XTag *tag;
   int length;

   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:spiral/>", &tag);

   if (!error) {
      Unit unit;
      double dbl;
      if (!spiral->getCX(unit))     set_dimension(tag, "cx", unit);
      if (!spiral->getCY(unit))     set_dimension(tag, "cy", unit);
      if (!spiral->getWidth(unit))  set_dimension(tag, "width", unit);
      if (!spiral->getHeight(unit)) set_dimension(tag, "height", unit);
      if (!spiral->getOffset(dbl))  xml::NewAttrib(tag, "offset", dbl);
      if (!spiral->getRadius(unit)) set_dimension(tag, "r", unit);
      if (!spiral->getStep(dbl))    xml::NewAttrib(tag, "step", dbl);
      if ((!spiral->getPathLength(length)) and (length != 0)) xml::NewAttrib(tag, "pathLength", length);

      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_shape(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   auto shape = (objVectorShape *)Vector;
   XTag *tag;

   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<kotuku:shape/>", &tag);

   if (!error) {
      double dbl;
      Unit val;
      int num;
      if (!shape->getCX(val))       set_dimension(tag, "cx", val);
      if (!shape->getCY(val))       set_dimension(tag, "cy", val);
      if (!shape->getRadius(val))   set_dimension(tag, "r", val);
      if (!shape->getA(dbl))        xml::NewAttrib(tag, "a", dbl);
      if (!shape->getB(dbl))        xml::NewAttrib(tag, "b", dbl);
      if (!shape->getM(dbl))        xml::NewAttrib(tag, "m", dbl);
      if (!shape->getN1(dbl))       xml::NewAttrib(tag, "n1", dbl);
      if (!shape->getN2(dbl))       xml::NewAttrib(tag, "n2", dbl);
      if (!shape->getN3(dbl))       xml::NewAttrib(tag, "n3", dbl);
      if (!shape->getPhi(dbl))      xml::NewAttrib(tag, "phi", dbl);
      if (!shape->getVertices(num)) xml::NewAttrib(tag, "vertices", num);
      if (!shape->getMod(num))      xml::NewAttrib(tag, "mod", num);
      if (!shape->getSpiral(num))   xml::NewAttrib(tag, "spiral", num);
      if (!shape->getRepeat(num))   xml::NewAttrib(tag, "repeat", num);
      if (!shape->getClose(num))    xml::NewAttrib(tag, "close", num);

      ChildIndex = tag->ID;
      error = save_svg_scan_std(Self, XML, Vector, tag->ID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan_viewport(extSVG *Self, objXML *XML, objVector *Vector, int Parent, int &ChildIndex)
{
   XTag *tag;
   double x, y, width, height;

   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<svg/>", &tag);

   auto viewport = (objVectorViewport *)Vector;

   // Build the viewBox value

   if (!error) error = viewport->getViewX(x);
   if (!error) error = viewport->getViewY(y);
   if (!error) error = viewport->getViewWidth(width);
   if (!error) error = viewport->getViewHeight(height);

   if (!error) {
      std::stringstream buffer;
      buffer << x << " " << y << " " << width << " " << height;
      xml::NewAttrib(tag, "viewBox", buffer.str());
   }

   // Output viewport dimensions, if defined

   if (!error) {
      Unit unit(0, FD_PURE); // Request original client setting

      if ((!error) and (!viewport->getX(unit))) set_dimension(tag, "x", unit);
      if ((!error) and (!viewport->getY(unit))) set_dimension(tag, "y", unit);
      if ((!error) and (!viewport->getWidth(unit))) set_dimension(tag, "width", unit);
      if ((!error) and (!viewport->getHeight(unit))) set_dimension(tag, "height", unit);
   }

   if (!error) ChildIndex = tag->ID;

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_scan(extSVG *Self, objXML *XML, objVector *Vector, int Parent)
{
   kt::Log log(__FUNCTION__);

   log.branch("%s", Vector->Class->ClassName.c_str());

   // child_index identifies the XML node that child vectors attach to.  Handlers set it on success; the VECTORPATH
   // branch handles its own child recursion via save_vectorpath(), and the default branch leaves it at -1 so that
   // unrecognised classes are skipped without recursing.

   int child_index = -1;
   ERR error = ERR::Okay;

   switch (Vector->classID()) {
      case CLASSID::VECTORRECTANGLE: error = save_svg_scan_rectangle(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORELLIPSE:   error = save_svg_scan_ellipse(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORPATH:      return save_vectorpath(Self, XML, Vector, Parent);
      case CLASSID::VECTORPOLYGON:   error = save_svg_scan_polygon(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORTEXT:      error = save_svg_scan_text(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORGROUP:     error = save_svg_scan_group(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORCLIP:      error = save_svg_scan_clip(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORWAVE:      error = save_svg_scan_wave(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORSPIRAL:    error = save_svg_scan_spiral(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORSHAPE:     error = save_svg_scan_shape(Self, XML, Vector, Parent, child_index); break;
      case CLASSID::VECTORVIEWPORT:  error = save_svg_scan_viewport(Self, XML, Vector, Parent, child_index); break;
      default:
         log.msg("Unrecognised class \"%s\"", Vector->Class->ClassName.c_str());
         return ERR::Okay; // Skip objects in the scene graph that we don't recognise
   }

   if ((!error) and (child_index != -1)) {
      for (auto scan=Vector->Child; scan; scan=scan->Next) {
         save_svg_scan(Self, XML, scan, child_index);
      }
   }

   return error;
}
