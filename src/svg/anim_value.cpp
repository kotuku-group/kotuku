
//********************************************************************************************************************

void anim_value::perform()
{
   kt::Log log;

   if ((end_time) and (!freeze)) return;

   kt::ScopedObjectLock<objVector> vector(target_vector, 1000);
   if (vector.granted()) {
      if (vector->classID() IS CLASSID::VECTORGROUP) {
         // Groups are a special case because they act as a placeholder and aren't guaranteed to propagate all
         // attributes to their children.

         // Note that group attributes do not override values that are defined by the client.

         for (auto &child : tag->Children) {
            if (!child.isTag()) continue;
            // Any tag producing a vector object can theoretically be subject to animation.
            if (auto si = child.attrib("_id")) {
               // We can't override attributes that were defined by the client.
               if (child.attrib(target_attrib)) continue;

               kt::ScopedObjectLock<objVector> cv(std::stoi(*si), 1000);
               if (cv.granted()) set_value(**cv);
            }
         }
      }
      else set_value(**vector);
   }
}

//********************************************************************************************************************
// This function is essentially a mirror of set_property() in terms of targeting fields.

void anim_value::set_value(objVector &Vector)
{
   auto hash = strhash(target_attrib);

   switch(Vector.Class->ClassID) {
      case CLASSID::VECTORWAVE:
         switch (hash) {
            case SVF_close:     Vector.set(FID_Close, get_string()); return;
            case SVF_amplitude: Vector.set(FID_Amplitude, get_numeric_value(Vector, FID_Amplitude)); return;
            case SVF_decay:     Vector.set(FID_Decay, get_numeric_value(Vector, FID_Decay)); return;
            case SVF_frequency: Vector.set(FID_Frequency, get_numeric_value(Vector, FID_Frequency)); return;
            case SVF_thickness: Vector.set(FID_Thickness, get_numeric_value(Vector, FID_Thickness)); return;
         }
         break;

      case CLASSID::VECTORTEXT:
         switch (hash) {
            case SVF_dx: Vector.set(FID_DX, get_string()); return;
            case SVF_dy: Vector.set(FID_DY, get_string()); return;

            case SVF_text_anchor:
               switch(strhash(get_string())) {
                  case SVF_start:   Vector.set(FID_Align, int(ALIGN::LEFT)); return;
                  case SVF_middle:  Vector.set(FID_Align, int(ALIGN::HORIZONTAL)); return;
                  case SVF_end:     Vector.set(FID_Align, int(ALIGN::RIGHT)); return;
                  case SVF_inherit: Vector.set(FID_Align, int(ALIGN::NIL)); return;
               }
               break;

            case SVF_rotate: Vector.set(FID_Rotate, get_string()); return;
            case SVF_string: Vector.set(FID_String, get_string()); return;

            case SVF_kerning:        Vector.set(FID_Kerning, get_string()); return; // Spacing between letters, default=1.0
            case SVF_letter_spacing: Vector.set(FID_LetterSpacing, get_string()); return;
            case SVF_pathLength:     Vector.set(FID_PathLength, get_string()); return;
            case SVF_word_spacing:   Vector.set(FID_WordSpacing, get_string()); return;

            case SVF_font_family: Vector.set(FID_Face, get_string()); return;

            case SVF_font_size: Vector.set(FID_FontSize, get_numeric_value(Vector, FID_FontSize)); return;
         }
         break;

      default: break;
   }

   switch(hash) {
      case SVF_colour:
      case SVF_color: {
         // The 'color' attribute directly targets the currentColor value.  Changes to the currentColor should result
         // in downstream users being affected - most likely fill and stroke references.
         //
         // TODO: Correct implementation requires inspection of the XML tags.  If the parent Vector is a group, its
         // children will need to be checked for currentColor references.
         const FRGB val = get_colour_value(Vector, FID_FillColour);
         Vector.setFillColour(val);
         return;
      }

      case SVF_fill: {
         const auto val = get_colour_value(Vector, FID_FillColour);
         Vector.setFillColour(val);
         return;
      }

      case SVF_fill_rule: {
         auto val = get_string();
         if (val IS "nonzero") Vector.setFillRule(VFR::NON_ZERO);
         else if (val IS "evenodd") Vector.setFillRule(VFR::EVEN_ODD);
         else if (val IS "inherit") Vector.setFillRule(VFR::INHERIT);
         return;
      }

      case SVF_clip_rule: {
         auto val = get_string();
         if (val IS "nonzero")      Vector.setClipRule(VFR::NON_ZERO);
         else if (val IS "evenodd") Vector.setClipRule(VFR::EVEN_ODD);
         else if (val IS "inherit") Vector.setClipRule(VFR::INHERIT);
         return;
      }
      case SVF_fill_opacity: {
         auto val = get_numeric_value(Vector, FID_FillOpacity);
         Vector.setFillOpacity(val);
         return;
      }

      case SVF_stroke: {
         FRGB val = get_colour_value(Vector, FID_StrokeColour);
         Vector.setStrokeColour(val);
         return;
      }

      case SVF_stroke_width:
         Vector.set(FID_StrokeWidth, get_numeric_value(Vector, FID_StrokeWidth));
         return;

      case SVF_stroke_linejoin:
         switch(strhash(get_string())) {
            case SVF_miter: Vector.setLineJoin(VLJ::MITER); return;
            case SVF_round: Vector.setLineJoin(VLJ::ROUND); return;
            case SVF_bevel: Vector.setLineJoin(VLJ::BEVEL); return;
            case SVF_inherit: Vector.setLineJoin(VLJ::INHERIT); return;
            case SVF_miter_clip: Vector.setLineJoin(VLJ::MITER_SMART); return; // Special AGG only join type
            case SVF_miter_round: Vector.setLineJoin(VLJ::MITER_ROUND); return; // Special AGG only join type
         }
         return;

      case SVF_stroke_innerjoin: // AGG ONLY
         switch(strhash(get_string())) {
            case SVF_miter:   Vector.setInnerJoin(VIJ::MITER);  return;
            case SVF_round:   Vector.setInnerJoin(VIJ::ROUND); return;
            case SVF_bevel:   Vector.setInnerJoin(VIJ::BEVEL); return;
            case SVF_inherit: Vector.setInnerJoin(VIJ::INHERIT); return;
            case SVF_jag:     Vector.setInnerJoin(VIJ::JAG); return;
         }
         return;

      case SVF_stroke_linecap:
         switch(strhash(get_string())) {
            case SVF_butt:    Vector.setLineCap(VLC::BUTT); return;
            case SVF_square:  Vector.setLineCap(VLC::SQUARE); return;
            case SVF_round:   Vector.setLineCap(VLC::ROUND); return;
            case SVF_inherit: Vector.setLineCap(VLC::INHERIT); return;
         }
         return;

      case SVF_stroke_opacity:          Vector.set(FID_StrokeOpacity, get_numeric_value(Vector, FID_StrokeOpacity)); break;
      case SVF_stroke_miterlimit:       Vector.set(FID_MiterLimit, get_string()); break;
      case SVF_stroke_miterlimit_theta: Vector.set(FID_MiterLimitTheta, get_string()); break;
      case SVF_stroke_inner_miterlimit: Vector.set(FID_InnerMiterLimit, get_string()); break;
      case SVF_stroke_dasharray:        Vector.set(FID_DashArray, get_string()); return;
      case SVF_stroke_dashoffset:       Vector.set(FID_DashOffset, get_string()); return;
      case SVF_opacity:                 Vector.set(FID_Opacity, get_numeric_value(Vector, FID_Opacity)); return;

      case SVF_display: {
         auto val = get_string();
         if (val IS "none")         Vector.setVisibility(VIS::HIDDEN);
         else if (val IS "inline")  Vector.setVisibility(VIS::VISIBLE);
         else if (val IS "inherit") Vector.setVisibility(VIS::INHERIT);
         return;
      }

      case SVF_visibility: {
         auto val = get_string();
         Vector.set(FID_Visibility, val);
         return;
      }

      case SVF_r:
         Vector.set(FID_Radius, get_dimension(Vector, FID_Radius));
         return;

      case SVF_rx:
         Vector.set(FID_RadiusX, get_dimension(Vector, FID_RadiusX));
         return;

      case SVF_ry:
         Vector.set(FID_RadiusY, get_dimension(Vector, FID_RadiusY));
         return;

      case SVF_cx:
         Vector.set(FID_CX, get_dimension(Vector, FID_CX));
         return;

      case SVF_cy:
         Vector.set(FID_CY, get_dimension(Vector, FID_CY));
         return;

      case SVF_xOffset:
         Vector.set(FID_XOffset, get_dimension(Vector, FID_XOffset));
         return;

      case SVF_yOffset:
         Vector.set(FID_YOffset, get_dimension(Vector, FID_YOffset));
         return;

      case SVF_x1:
         Vector.set(FID_X1, get_dimension(Vector, FID_X1));
         return;

      case SVF_y1:
         Vector.set(FID_Y1, get_dimension(Vector, FID_Y1));
         return;

      case SVF_x2:
         Vector.set(FID_X2, get_dimension(Vector, FID_X2));
         return;

      case SVF_y2:
         Vector.set(FID_Y2, get_dimension(Vector, FID_Y2));
         return;

      case SVF_x: {
         if (Vector.Class->ClassID IS CLASSID::VECTORGROUP) {
            // Special case: SVG groups don't have an (x,y) position, but can declare one in the form of a
            // transform.  Refer to xtag_use() for a working example as to why.

            VectorMatrix *m;
            for (m=Vector.Matrices; (m) and ((unsigned int)m->Tag != MTAG_SVG_TRANSFORM); m=m->Next);

            if (!m) {
               Vector.newMatrix(&m, false);
               m->Tag = MTAG_SVG_TRANSFORM;
            }

            if (m) {
               m->TranslateX = get_dimension(Vector, FID_X);
               vec::FlushMatrix(m);
            }
         }
         else Vector.set(FID_X, get_dimension(Vector, FID_X));
         return;
      }

      case SVF_y: {
         if (Vector.Class->ClassID IS CLASSID::VECTORGROUP) {
            VectorMatrix *m;
            for (m=Vector.Matrices; (m) and ((unsigned int)m->Tag != MTAG_SVG_TRANSFORM); m=m->Next);

            if (!m) {
               Vector.newMatrix(&m, false);
               m->Tag = MTAG_SVG_TRANSFORM;
            }

            if (m) {
               m->TranslateY = get_dimension(Vector, FID_Y);
               vec::FlushMatrix(m);
            }
         }
         else Vector.set(FID_Y, get_dimension(Vector, FID_Y));
         return;
      }

      case SVF_width:
         Vector.set(FID_Width, get_dimension(Vector, FID_Width));
         return;

      case SVF_height:
         Vector.set(FID_Height, get_dimension(Vector, FID_Height));
         return;
   }
}
