
//********************************************************************************************************************

void anim_value::perform()
{
   pf::Log log;

   if ((end_time) and (!freeze)) return;

   pf::ScopedObjectLock<objVector> vector(target_vector, 1000);
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

               pf::ScopedObjectLock<objVector> cv(std::stoi(*si), 1000);
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
   auto hash = strihash(target_attrib);

   switch(Vector.Class->ClassID) {
      case CLASSID::VECTORWAVE:
         switch (hash) {
            case SVF_CLOSE:     Vector.set(FID_Close, get_string()); return;
            case SVF_AMPLITUDE: Vector.set(FID_Amplitude, get_numeric_value(Vector, FID_Amplitude)); return;
            case SVF_DECAY:     Vector.set(FID_Decay, get_numeric_value(Vector, FID_Decay)); return;
            case SVF_FREQUENCY: Vector.set(FID_Frequency, get_numeric_value(Vector, FID_Frequency)); return;
            case SVF_THICKNESS: Vector.set(FID_Thickness, get_numeric_value(Vector, FID_Thickness)); return;
         }
         break;

      case CLASSID::VECTORTEXT:
         switch (hash) {
            case SVF_DX: Vector.set(FID_DX, get_string()); return;
            case SVF_DY: Vector.set(FID_DY, get_string()); return;

            case SVF_TEXT_ANCHOR:
               switch(strihash(get_string())) {
                  case SVF_START:   Vector.set(FID_Align, int(ALIGN::LEFT)); return;
                  case SVF_MIDDLE:  Vector.set(FID_Align, int(ALIGN::HORIZONTAL)); return;
                  case SVF_END:     Vector.set(FID_Align, int(ALIGN::RIGHT)); return;
                  case SVF_INHERIT: Vector.set(FID_Align, int(ALIGN::NIL)); return;
               }
               break;

            case SVF_ROTATE: Vector.set(FID_Rotate, get_string()); return;
            case SVF_STRING: Vector.set(FID_String, get_string()); return;

            case SVF_KERNING:        Vector.set(FID_Kerning, get_string()); return; // Spacing between letters, default=1.0
            case SVF_LETTER_SPACING: Vector.set(FID_LetterSpacing, get_string()); return;
            case SVF_PATHLENGTH:     Vector.set(FID_PathLength, get_string()); return;
            case SVF_WORD_SPACING:   Vector.set(FID_WordSpacing, get_string()); return;

            case SVF_FONT_FAMILY: Vector.set(FID_Face, get_string()); return;

            case SVF_FONT_SIZE: Vector.set(FID_FontSize, get_numeric_value(Vector, FID_FontSize)); return;
         }
         break;

      default: break;
   }

   switch(hash) {
      case SVF_COLOUR:
      case SVF_COLOR: {
         // The 'color' attribute directly targets the currentColor value.  Changes to the currentColor should result
         // in downstream users being affected - most likely fill and stroke references.
         //
         // TODO: Correct implementation requires inspection of the XML tags.  If the parent Vector is a group, its
         // children will need to be checked for currentColor references.
         const FRGB val = get_colour_value(Vector, FID_FillColour);
         Vector.set(FID_FillColour, val);
         return;
      }

      case SVF_FILL: {
         const auto val = get_colour_value(Vector, FID_FillColour);
         Vector.set(FID_FillColour, val);
         return;
      }

      case SVF_FILL_RULE: {
         auto val = get_string();
         if (val IS "nonzero") Vector.set(FID_FillRule, int(VFR::NON_ZERO));
         else if (val IS "evenodd") Vector.set(FID_FillRule, int(VFR::EVEN_ODD));
         else if (val IS "inherit") Vector.set(FID_FillRule, int(VFR::INHERIT));
         return;
      }

      case SVF_CLIP_RULE: {
         auto val = get_string();
         if (val IS "nonzero")      Vector.set(FID_ClipRule, int(VFR::NON_ZERO));
         else if (val IS "evenodd") Vector.set(FID_ClipRule, int(VFR::EVEN_ODD));
         else if (val IS "inherit") Vector.set(FID_ClipRule, int(VFR::INHERIT));
         return;
      }
      case SVF_FILL_OPACITY: {
         auto val = get_numeric_value(Vector, FID_FillOpacity);
         Vector.set(FID_FillOpacity, val);
         return;
      }

      case SVF_STROKE: {
         auto val = get_colour_value(Vector, FID_StrokeColour);
         Vector.set(FID_StrokeColour, val);
         return;
      }

      case SVF_STROKE_WIDTH:
         Vector.set(FID_StrokeWidth, get_numeric_value(Vector, FID_StrokeWidth));
         return;

      case SVF_STROKE_LINEJOIN:
         switch(strihash(get_string())) {
            case SVF_MITER: Vector.setLineJoin(int(VLJ::MITER)); return;
            case SVF_ROUND: Vector.setLineJoin(int(VLJ::ROUND)); return;
            case SVF_BEVEL: Vector.setLineJoin(int(VLJ::BEVEL)); return;
            case SVF_INHERIT: Vector.setLineJoin(int(VLJ::INHERIT)); return;
            case SVF_MITER_CLIP: Vector.setLineJoin(int(VLJ::MITER_SMART)); return; // Special AGG only join type
            case SVF_MITER_ROUND: Vector.setLineJoin(int(VLJ::MITER_ROUND)); return; // Special AGG only join type
         }
         return;

      case SVF_STROKE_INNERJOIN: // AGG ONLY
         switch(strihash(get_string())) {
            case SVF_MITER:   Vector.set(FID_InnerJoin, int(VIJ::MITER));  return;
            case SVF_ROUND:   Vector.set(FID_InnerJoin, int(VIJ::ROUND)); return;
            case SVF_BEVEL:   Vector.set(FID_InnerJoin, int(VIJ::BEVEL)); return;
            case SVF_INHERIT: Vector.set(FID_InnerJoin, int(VIJ::INHERIT)); return;
            case SVF_JAG:     Vector.set(FID_InnerJoin, int(VIJ::JAG)); return;
         }
         return;

      case SVF_STROKE_LINECAP:
         switch(strihash(get_string())) {
            case SVF_BUTT:    Vector.set(FID_LineCap, int(VLC::BUTT)); return;
            case SVF_SQUARE:  Vector.set(FID_LineCap, int(VLC::SQUARE)); return;
            case SVF_ROUND:   Vector.set(FID_LineCap, int(VLC::ROUND)); return;
            case SVF_INHERIT: Vector.set(FID_LineCap, int(VLC::INHERIT)); return;
         }
         return;

      case SVF_STROKE_OPACITY:          Vector.set(FID_StrokeOpacity, get_numeric_value(Vector, FID_StrokeOpacity)); break;

      case SVF_STROKE_MITERLIMIT:       Vector.set(FID_MiterLimit, get_string()); break;
      case SVF_STROKE_MITERLIMIT_THETA: Vector.set(FID_MiterLimitTheta, get_string()); break;
      case SVF_STROKE_INNER_MITERLIMIT: Vector.set(FID_InnerMiterLimit, get_string()); break;

      case SVF_STROKE_DASHARRAY: Vector.set(FID_DashArray, get_string()); return;

      case SVF_STROKE_DASHOFFSET: Vector.set(FID_DashOffset, get_string()); return;

      case SVF_OPACITY:
         Vector.set(FID_Opacity, get_numeric_value(Vector, FID_Opacity));
         return;

      case SVF_DISPLAY: {
         auto val = get_string();
         if (val IS "none")         Vector.set(FID_Visibility, int(VIS::HIDDEN));
         else if (val IS "inline")  Vector.set(FID_Visibility, int(VIS::VISIBLE));
         else if (val IS "inherit") Vector.set(FID_Visibility, int(VIS::INHERIT));
         return;
      }

      case SVF_VISIBILITY: {
         auto val = get_string();
         Vector.set(FID_Visibility, val);
         return;
      }

      case SVF_R:
         Vector.set(FID_Radius, get_dimension(Vector, FID_Radius));
         return;

      case SVF_RX:
         Vector.set(FID_RadiusX, get_dimension(Vector, FID_RadiusX));
         return;

      case SVF_RY:
         Vector.set(FID_RadiusY, get_dimension(Vector, FID_RadiusY));
         return;

      case SVF_CX:
         Vector.set(FID_CX, get_dimension(Vector, FID_CX));
         return;

      case SVF_CY:
         Vector.set(FID_CY, get_dimension(Vector, FID_CY));
         return;

      case SVF_XOFFSET:
         Vector.set(FID_XOffset, get_dimension(Vector, FID_XOffset));
         return;

      case SVF_YOFFSET:
         Vector.set(FID_YOffset, get_dimension(Vector, FID_YOffset));
         return;

      case SVF_X1:
         Vector.set(FID_X1, get_dimension(Vector, FID_X1));
         return;

      case SVF_Y1:
         Vector.set(FID_Y1, get_dimension(Vector, FID_Y1));
         return;

      case SVF_X2:
         Vector.set(FID_X2, get_dimension(Vector, FID_X2));
         return;

      case SVF_Y2:
         Vector.set(FID_Y2, get_dimension(Vector, FID_Y2));
         return;

      case SVF_X: {
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

      case SVF_Y: {
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

      case SVF_WIDTH:
         Vector.set(FID_Width, get_dimension(Vector, FID_Width));
         return;

      case SVF_HEIGHT:
         Vector.set(FID_Height, get_dimension(Vector, FID_Height));
         return;
   }
}