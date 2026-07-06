// Exporting a vector scene graph to SVG is handled here.
//
// Note on round-tripping: we don't save unnecessary metadata from the source, so a true round-trip isn't possible.
// However, the client should be able to rely on the output creating a scene that renders identically to the original
// input, as long as the SVG and Vector modules provide sufficient features for generating the original scene.
//
// With respect to SVG optimisation commands like <use>, these are a second-pass detail that can be taken up after
// the initial generation of the XML document [TODO]

//*********************************************************************************************************************
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

static bool has_attrib(const XTag *Tag, std::string_view Name) noexcept
{
   return Tag->attrib(Name) != nullptr;
}

//*********************************************************************************************************************

static void set_missing_attrib(XTag *Tag, std::string_view Name, std::string_view Value)
{
   if ((not Value.empty()) and (not has_attrib(Tag, Name))) xml::NewAttrib(Tag, Name, Value);
}

//*********************************************************************************************************************

static ERR save_svg_scan_def(extSVG *Self, objXML *XML, objVector *Vector, int Parent, std::string_view DefID)
{
   XTag *parent_tag;
   ERR error = XML->getTag(Parent, &parent_tag);
   if (error != ERR::Okay) return error;

   const auto child_count = parent_tag->Children.size();

   error = save_svg_scan(Self, XML, Vector, Parent);
   if ((error != ERR::Okay) or DefID.empty()) return error;

   error = XML->getTag(Parent, &parent_tag);
   if ((error IS ERR::Okay) and (parent_tag->Children.size() > child_count)) {
      set_missing_attrib(&parent_tag->Children[child_count], "id", DefID);
   }

   return error;
}

//*********************************************************************************************************************

static ERR save_svg_clip(extSVG *Self, objXML *XML, objVectorClip *Clip, int Parent, std::string_view DefID)
{
   XTag *tag;
   std::string_view sid;
   ERR error = Clip->getSID(sid);
   if (error != ERR::Okay) return error;

   const std::string_view id = sid.empty() ? DefID : sid;
   if (id.empty()) return ERR::Okay;

   error = XML->insertStatement(Parent, XMI::CHILD_END, "<clipPath/>", &tag);
   if (error != ERR::Okay) return error;

   xml::NewAttrib(tag, "id", id);

   VUNIT units;
   if (!Clip->getUnits(units)) {
      switch(units) {
         default:
         case VUNIT::USERSPACE:    break; // Default
         case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "clipPathUnits", "objectBoundingBox"); break;
      }
   }

   objVectorViewport *viewport = nullptr;
   if ((error = Clip->getViewport(viewport)) != ERR::Okay) return error;

   if (viewport) {
      for (auto scan=viewport->Child; (scan) and (!error); scan=scan->Next) {
         error = save_svg_scan(Self, XML, scan, tag->ID);
      }
   }

   return error;
}

//*********************************************************************************************************************

static void set_unit_attrib(XTag *Tag, std::string_view Name, VUNIT Units)
{
   switch(Units) {
      default:
      case VUNIT::UNDEFINED:    break;
      case VUNIT::USERSPACE:    xml::NewAttrib(Tag, Name, "userSpaceOnUse"); break;
      case VUNIT::BOUNDING_BOX: xml::NewAttrib(Tag, Name, "objectBoundingBox"); break;
   }
}

//*********************************************************************************************************************

static ERR save_svg_pattern(extSVG *Self, objXML *XML, objVectorPattern *Pattern, int Parent, std::string_view DefID)
{
   XTag *tag;
   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<pattern/>", &tag);
   if (error != ERR::Okay) return error;

   xml::NewAttrib(tag, "id", DefID);

   Unit unit;
   if ((!error) and (!Pattern->getX(unit))) set_dimension(tag, "x", unit);
   if ((!error) and (!Pattern->getY(unit))) set_dimension(tag, "y", unit);
   if ((!error) and (!Pattern->getWidth(unit))) set_dimension(tag, "width", unit);
   if ((!error) and (!Pattern->getHeight(unit))) set_dimension(tag, "height", unit);

   VUNIT units;
   if ((!error) and (!Pattern->getUnits(units))) set_unit_attrib(tag, "patternUnits", units);
   if ((!error) and (!Pattern->getContentUnits(units))) set_unit_attrib(tag, "patternContentUnits", units);

   std::span<VectorMatrix> transform;
   if ((!error) and (!Pattern->getMatrices(transform)) and (not transform.empty())) {
      std::stringstream buffer;
      if (!save_svg_transform(transform.data(), buffer)) {
         xml::NewAttrib(tag, "patternTransform", buffer.str());
      }
   }

   objVectorViewport *viewport = nullptr;
   if ((!error) and (!Pattern->getViewport(viewport)) and (viewport)) {
      double x, y, width, height;
      if ((!viewport->getViewX(x)) and (!viewport->getViewY(y)) and
          (!viewport->getViewWidth(width)) and (!viewport->getViewHeight(height)) and (width > 0) and (height > 0)) {
         xml::NewAttrib(tag, "viewBox", std::format("{} {} {} {}", x, y, width, height));
      }

      for (auto scan=viewport->Child; (scan) and (!error); scan=scan->Next) {
         error = save_svg_scan(Self, XML, scan, tag->ID);
      }
   }

   return error;
}

//*********************************************************************************************************************

static std::string save_aspect_ratio(ARF AspectRatio)
{
   if ((AspectRatio & ARF::NONE) != ARF::NIL) return "none";

   std::string result;

   if ((AspectRatio & ARF::X_MIN) != ARF::NIL) result = "xMin";
   else if ((AspectRatio & ARF::X_MAX) != ARF::NIL) result = "xMax";
   else result = "xMid";

   if ((AspectRatio & ARF::Y_MIN) != ARF::NIL) result += "YMin";
   else if ((AspectRatio & ARF::Y_MAX) != ARF::NIL) result += "YMax";
   else result += "YMid";

   if ((AspectRatio & ARF::SLICE) != ARF::NIL) result += " slice";
   else result += " meet";

   return result;
}

//*********************************************************************************************************************

static bool external_image_path(std::string_view Path, std::string &Result)
{
   if (Path.empty() or Path.starts_with("data:") or Path.starts_with("temp:")) return false;

   std::string resolved;
   LOC type = LOC::NIL;
   if ((ResolvePath(Path, RSF::NIL, &resolved) IS ERR::Okay) and
       (AnalysePath(resolved, &type) IS ERR::Okay) and (type IS LOC::FILE)) {
      Result.assign(Path);
      return true;
   }

   return false;
}

//*********************************************************************************************************************

static ERR encode_bitmap_png(objBitmap *Bitmap, std::string &Href)
{
   if (not Bitmap) return ERR::FieldNotSet;

   objFile::create file = {
      fl::Size(0),
      fl::Flags(FL::BUFFER|FL::READ|FL::WRITE)
   };
   if (!file.ok()) return file.error;

   objImage::create image(NF::LOCAL);
   if (!image.ok()) return image.error;

   if (auto owned_bitmap = image->Bitmap) FreeResource(owned_bitmap);
   image->Bitmap = Bitmap;
   image->Flags = PCF::NIL;

   ERR error = image->saveImage(*file);
   image->Bitmap = nullptr;
   if (error != ERR::Okay) return error;

   std::span<int8_t> data;
   if ((error = file->getBuffer(data)) != ERR::Okay) return error;
   if (data.empty()) return ERR::NoData;

   std::vector<char> encoded((data.size() * 2) + 8);
   kt::BASE64ENCODE state;

   auto len = kt::Base64Encode(&state, std::span((const char *)data.data(), data.size()), encoded);
   if (len <= 0) return ERR::NoData;

   auto final_len = kt::Base64Encode(&state, {}, std::span(encoded.data() + len, encoded.size() - len));
   if (final_len <= 0) return ERR::NoData;

   Href = "data:image/png;base64,";
   Href.reserve(Href.size() + size_t(len + final_len));

   for (int64_t i=0; i < len + final_len; i++) {
      if ((encoded[i] != '\n') and (encoded[i] != '\r') and (encoded[i] != 0)) Href.push_back(encoded[i]);
   }

   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR save_svg_image(objXML *XML, objVectorImage *Image, int Parent, std::string_view DefID)
{
   XTag *tag;
   ERR error = XML->insertStatement(Parent, XMI::CHILD_END, "<image/>", &tag);
   if (error != ERR::Okay) return error;

   xml::NewAttrib(tag, "id", DefID);

   objBitmap *bitmap;
   if ((error = Image->getBitmap(bitmap)) != ERR::Okay) return error;
   if (not bitmap) return ERR::FieldNotSet;

   xml::NewAttrib(tag, "width", bitmap->Width);
   xml::NewAttrib(tag, "height", bitmap->Height);

   Unit unit;
   if ((!error) and (!Image->getX(unit)) and unit.defined() and (double(unit) != 0)) set_dimension(tag, "x", unit);
   if ((!error) and (!Image->getY(unit)) and unit.defined() and (double(unit) != 0)) set_dimension(tag, "y", unit);

   VUNIT units;
   if ((!error) and (!Image->getUnits(units))) {
      switch(units) {
         default:
         case VUNIT::BOUNDING_BOX: xml::NewAttrib(tag, "units", "objectBoundingBox"); break;
         case VUNIT::USERSPACE:    xml::NewAttrib(tag, "units", "userSpaceOnUse"); break;
      }
   }

   ARF aspect_ratio;
   if ((!error) and (!Image->getAspectRatio(aspect_ratio))) {
      xml::NewAttrib(tag, "preserveAspectRatio", save_aspect_ratio(aspect_ratio));
   }

   std::string href;
   objImage *source_image;
   if ((!error) and (!Image->getImage(source_image)) and source_image) {
      std::string_view path;
      if ((!source_image->getPath(path)) and external_image_path(path, href)) {
         xml::NewAttrib(tag, "xlink:href", href);
         return ERR::Okay;
      }
   }

   if ((error = encode_bitmap_png(bitmap, href)) != ERR::Okay) return error;
   xml::NewAttrib(tag, "xlink:href", href);

   return ERR::Okay;
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

static void save_mesh_stop(objXML *XML, int Parent, double X1, double Y1, double X2, double Y2, double EndX,
   double EndY, const FRGB &Colour)
{
   int stop_index;
   if (XML->insertXML(Parent, XMI::CHILD_END, "<stop/>", &stop_index) != ERR::Okay) return;

   XTag *stop_tag;
   if (XML->getTag(stop_index, &stop_tag) != ERR::Okay) return;

   xml::NewAttrib(stop_tag, "path", std::format("C {} {} {} {} {} {}", X1, Y1, X2, Y2, EndX, EndY));

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
   return (Patch.Top.StartX IS Previous.Right.StartX) and (Patch.Top.StartY IS Previous.Right.StartY) and
      (Patch.Bottom.EndX IS Previous.Right.EndX) and (Patch.Bottom.EndY IS Previous.Right.EndY) and
      (Patch.Left.StartX IS Previous.Right.EndX) and (Patch.Left.StartY IS Previous.Right.EndY) and
      (Patch.Left.X1 IS Previous.Right.X2) and (Patch.Left.Y1 IS Previous.Right.Y2) and
      (Patch.Left.X2 IS Previous.Right.X1) and (Patch.Left.Y2 IS Previous.Right.Y1) and
      (Patch.Left.EndX IS Previous.Right.StartX) and (Patch.Left.EndY IS Previous.Right.StartY) and
      mesh_colour_match(Patch.TopLeft, Previous.TopRight) and
      mesh_colour_match(Patch.BottomLeft, Previous.BottomRight);
}

static bool mesh_patch_shares_previous_top_edge(const MeshPatchRecord &Patch, const MeshPatchRecord &Previous) noexcept
{
   return (Patch.Top.StartX IS Previous.Bottom.EndX) and (Patch.Top.StartY IS Previous.Bottom.EndY) and
      (Patch.Top.X1 IS Previous.Bottom.X2) and (Patch.Top.Y1 IS Previous.Bottom.Y2) and
      (Patch.Top.X2 IS Previous.Bottom.X1) and (Patch.Top.Y2 IS Previous.Bottom.Y1) and
      (Patch.Top.EndX IS Previous.Bottom.StartX) and (Patch.Top.EndY IS Previous.Bottom.StartY) and
      mesh_colour_match(Patch.TopLeft, Previous.BottomLeft) and
      mesh_colour_match(Patch.TopRight, Previous.BottomRight);
}

static void save_mesh_patch(objXML *XML, int RowIndex, int ColIndex, int Parent, const MeshPatchRecord &Patch,
   bool Independent)
{
   int patch_index;
   if (XML->insertXML(Parent, XMI::CHILD_END, "<meshpatch/>", &patch_index) != ERR::Okay) return;

   XTag *patch_tag;
   if ((Independent) and (XML->getTag(patch_index, &patch_tag) IS ERR::Okay)) {
      xml::NewAttrib(patch_tag, "kotuku:x", Patch.Top.StartX);
      xml::NewAttrib(patch_tag, "kotuku:y", Patch.Top.StartY);
   }

   if ((RowIndex IS 0) or (Independent)) {
      save_mesh_stop(XML, patch_index, Patch.Top.X1, Patch.Top.Y1, Patch.Top.X2, Patch.Top.Y2,
         Patch.Top.EndX, Patch.Top.EndY, Patch.TopRight);
   }

   save_mesh_stop(XML, patch_index, Patch.Right.X1, Patch.Right.Y1, Patch.Right.X2, Patch.Right.Y2,
      Patch.Right.EndX, Patch.Right.EndY, Patch.BottomRight);
   save_mesh_stop(XML, patch_index, Patch.Bottom.X1, Patch.Bottom.Y1, Patch.Bottom.X2, Patch.Bottom.Y2,
      Patch.Bottom.EndX, Patch.Bottom.EndY, Patch.BottomLeft);

   if ((ColIndex IS 0) or (Independent)) {
      save_mesh_stop(XML, patch_index, Patch.Left.X1, Patch.Left.Y1, Patch.Left.X2, Patch.Left.Y2,
         Patch.Left.EndX, Patch.Left.EndY, Patch.TopLeft);
   }
}

static void save_mesh_gradient(objXML *XML, XTag *Tag, objGradientMesh *Mesh)
{
   std::span<MeshPatchRecord> patches;
   if ((Mesh->getPatches(patches) != ERR::Okay) or patches.empty()) return;

   int rows = 0;
   int columns = 0;
   Mesh->get(kt::fieldhash("Rows"), rows);
   Mesh->get(kt::fieldhash("Columns"), columns);

   if ((rows <= 0) or (columns <= 0) or ((rows * columns) != int(patches.size()))) {
      rows = 1;
      columns = int(patches.size());
   }

   xml::NewAttrib(Tag, "x", patches[0].Top.StartX);
   xml::NewAttrib(Tag, "y", patches[0].Top.StartY);

   for (int row=0; row < rows; row++) {
      int row_index;
      if (XML->insertXML(Tag->ID, XMI::CHILD_END, "<meshrow/>", &row_index) != ERR::Okay) return;

      for (int col=0; col < columns; col++) {
         const int patch_index = (row * columns) + col;
         bool independent = false;

         if (col > 0) {
            independent = not mesh_patch_shares_previous_left_edge(patches[size_t(patch_index)],
               patches[size_t(patch_index - 1)]);
         }

         if ((not independent) and (row > 0)) {
            independent = not mesh_patch_shares_previous_top_edge(patches[size_t(patch_index)],
               patches[size_t(patch_index - columns)]);
         }

         save_mesh_patch(XML, row, col, row_index, patches[size_t(patch_index)], independent);
      }
   }
}

//*********************************************************************************************************************

static ERR save_svg_defs(extSVG *Self, objXML *XML, objVectorScene *Scene, int Parent)
{
   kt::Log log(__FUNCTION__);
   ankerl::unordered_dense::map<std::string, OBJECTPTR> *defs;

   if (!Scene->get(strhash("defs"), defs)) {
      ERR error = ERR::Okay;
      int def_index = 0;
      for (auto & [ key, def ] : *defs) {
         if (!def_index) {
            if ((error = XML->insertXML(Parent, XMI::CHILD_END, "<defs/>", &def_index)) != ERR::Okay) return error;
         }

         log.msg("Processing definition %s (%x)", def->Class->ClassName.c_str(), uint32_t(def->classID()));

         if (def->baseClassID() IS CLASSID::GRADIENT) {
            auto gradient = (objGradient *)def;
            std::string gradient_def;
            if (gradient->get(kt::fieldhash("XMLDef"), gradient_def) != ERR::Okay) {
               log.warning("%s not supported.", def->Class->ClassName.c_str());
               continue;
            }

            XTag *tag = nullptr;
            auto gradient_type = "<" + gradient_def + "/>";
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

            if (def->classID() IS CLASSID::GRADIENTMESH) {
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
            error = save_svg_image(XML, (objVectorImage *)def, def_index, key);
         }
         else if (def->classID() IS CLASSID::VECTORPATH) {
            error = save_svg_scan_def(Self, XML, (objVector *)def, def_index, key);
         }
         else if (def->classID() IS CLASSID::VECTORPATTERN) {
            error = save_svg_pattern(Self, XML, (objVectorPattern *)def, def_index, key);
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
            int transition_index;
            XTag *tag;
            error = XML->insertXML(def_index, XMI::CHILD_END, "<kotuku:transition/>", &transition_index);
            if (!error) error = XML->getTag(transition_index, &tag);
            if (!error) xml::NewAttrib(tag, "id", key);

            std::string transition_xml;
            if ((!error) and (!def->get(kt::fieldhash("XMLDef"), transition_xml))) {
               XTag *stop_tag;
               error = XML->insertStatement(transition_index, XMI::CHILD_END, transition_xml, &stop_tag);
            }
         }
         else if (def->classID() IS CLASSID::VECTORCLIP) {
            error = save_svg_clip(Self, XML, (objVectorClip *)def, def_index, key);
         }
         else if (def->Class->BaseClassID IS CLASSID::VECTOR) {
            error = save_svg_scan_def(Self, XML, (objVector *)def, def_index, key);
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
      Buffer << std::format("matrix({} {} {} {} {} {})", t->ScaleX, t->ShearY, t->ShearX, t->ScaleY, t->TranslateX, t->TranslateY);
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
   if ((!error) and (!Vector->getMatrices(transform)) and (transform)) {
      std::stringstream buffer;
      if ((error = save_svg_transform(transform, buffer)) IS ERR::Okay) {
         xml::NewAttrib(tag, "transform", buffer.str());
      }
   }

   OBJECTPTR guide_obj;
   if ((!error) and (!Vector->getGuidePath(guide_obj)) and (guide_obj)) {
      auto shape = (objVector *)guide_obj;
      VMF guide_flags;
      XTag *guide_tag;
      error = XML->insertStatement(TagID, XMI::CHILD_END, "<kotuku:guidePath/>", &guide_tag);

      std::string_view shape_id;
      if ((!error) and (!shape->getSID(shape_id)) and not shape_id.empty()) {
         // NB: It is required that the shape has previously been registered as a definition, otherwise the url will refer to a dud tag.
         auto shape_ref = std::format("url(#{})", shape_id);
         xml::NewAttrib(guide_tag, "xlink:href", shape_ref);
      }

      if (!error) error = Vector->getGuideFlags(guide_flags);

      if ((!error) and ((guide_flags & VMF::STRETCH) != VMF::NIL)) xml::NewAttrib(guide_tag, "method", "stretch");
      if ((!error) and ((guide_flags & VMF::AUTO_SPACING) != VMF::NIL)) xml::NewAttrib(guide_tag, "spacing", "auto");

      if ((!error) and ((guide_flags & (VMF::X_MIN|VMF::X_MID|VMF::X_MAX|VMF::Y_MIN|VMF::Y_MID|VMF::Y_MAX)) != VMF::NIL)) {
         std::string align;
         if ((guide_flags & VMF::X_MIN) != VMF::NIL) align = "xMin ";
         else if ((guide_flags & VMF::X_MID) != VMF::NIL) align = "xMid ";
         else if ((guide_flags & VMF::X_MAX) != VMF::NIL) align = "xMax ";

         if ((guide_flags & VMF::Y_MIN) != VMF::NIL) align += "yMin";
         else if ((guide_flags & VMF::Y_MID) != VMF::NIL) align += "yMid";
         else if ((guide_flags & VMF::Y_MAX) != VMF::NIL) align += "yMax";

         xml::NewAttrib(guide_tag, "align", align);
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
                  buffer << std::format("{},{} ", points[i].X, points[i].Y);
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
            buffer << std::format("{},{} ", points[i].X, points[i].Y);
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
   ERR error = save_svg_clip(Self, XML, (objVectorClip *)Vector, Parent, {});
   if (!error) ChildIndex = -1;
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
      if (!wave->getLength(unit))   set_dimension(tag, "length", unit);
      if (!wave->getAmplitude(unit)) set_dimension(tag, "amplitude", unit);
      if (!wave->getFrequency(dbl)) {
         xml::NewAttrib(tag, "frequency", dbl);

         double frequency_end;
         if ((!wave->getFrequencyEnd(frequency_end)) and (not (frequency_end IS dbl))) {
            xml::NewAttrib(tag, "frequencyEnd", frequency_end);
         }
      }
      if (!wave->getDecay(dbl))     xml::NewAttrib(tag, "decay", dbl);
      if ((!wave->getNoise(dbl)) and (not (dbl IS 0.0))) xml::NewAttrib(tag, "noise", dbl);
      if (!wave->getPhase(dbl))     xml::NewAttrib(tag, "phase", dbl);

      WVE envelope;
      if (!wave->getEnvelope(envelope)) {
         switch (envelope) {
            case WVE::QUADRATIC:   xml::NewAttrib(tag, "envelope", "quadratic"); break;
            case WVE::SMOOTHSTEP:  xml::NewAttrib(tag, "envelope", "smoothstep"); break;
            case WVE::EXPONENTIAL: xml::NewAttrib(tag, "envelope", "exponential"); break;
            default:               xml::NewAttrib(tag, "envelope", "linear"); break;
         }
      }

      WVC close;
      if (!wave->getClose(close)) {
         switch (close) {
            case WVC::TOP:    xml::NewAttrib(tag, "close", "top"); break;
            case WVC::BOTTOM: xml::NewAttrib(tag, "close", "bottom"); break;
            default: break;
         }
      }

      WVT type;
      if (!wave->getType(type)) {
         switch (type) {
            case WVT::TRIANGLE: xml::NewAttrib(tag, "type", "triangle"); break;
            case WVT::SAWTOOTH: xml::NewAttrib(tag, "type", "sawtooth"); break;
            case WVT::SQUARE:   xml::NewAttrib(tag, "type", "square"); break;
            default: break;
         }
      }

      if (!wave->getThickness(unit)) set_dimension(tag, "thickness", unit);

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

   if (!error) xml::NewAttrib(tag, "viewBox", std::format("{} {} {} {}", x, y, width, height));

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
