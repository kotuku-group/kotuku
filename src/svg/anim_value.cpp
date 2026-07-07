
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
      case CLASSID::VECTORPOLYGON: {
         auto &poly = (objVectorPolygon &)Vector;
         switch (hash) {
            case SVF_x1: poly.setX1(Unit(get_dimension(Vector, strhash("x1")))); return;
            case SVF_y1: poly.setY1(Unit(get_dimension(Vector, strhash("y1")))); return;
            case SVF_x2: poly.setX2(Unit(get_dimension(Vector, strhash("x2")))); return;
            case SVF_y2: poly.setY2(Unit(get_dimension(Vector, strhash("y2")))); return;
         }
         break;
      }

      case CLASSID::VECTORWAVE: {
         auto &wave = (objVectorWave &)Vector;
         switch (hash) {
            case SVF_x:  wave.setX(Unit(get_dimension(Vector, strhash("x")))); return;
            case SVF_y:  wave.setY(Unit(get_dimension(Vector, strhash("y")))); return;
            case SVF_close:
               switch(strhash(get_string())) {
                  case SVF_bottom: wave.setClose(WVC::BOTTOM); return;
                  case SVF_top:    wave.setClose(WVC::TOP); return;
                  default:         wave.setClose(WVC::NIL); return;
               }

            case SVF_envelope:
               switch(strhash(get_string())) {
                  case SVF_linear:      wave.setEnvelope(WVE::LINEAR); return;
                  case SVF_quadratic:   wave.setEnvelope(WVE::QUADRATIC); return;
                  case SVF_smoothstep:  wave.setEnvelope(WVE::SMOOTHSTEP); return;
                  case SVF_exponential: wave.setEnvelope(WVE::EXPONENTIAL); return;
                  default:              wave.setEnvelope(WVE::NIL); return;
               }

            case SVF_amplitude:     wave.setAmplitude(get_numeric_value(Vector, strhash("amplitude"))); return;
            case SVF_decay:         wave.setDecay(get_numeric_value(Vector, strhash("decay"))); return;
            case SVF_frequency:     wave.setFrequency(get_numeric_value(Vector, strhash("frequency"))); return;
            case SVF_frequency_end:
            case SVF_frequencyEnd:
               wave.setFrequencyEnd(get_numeric_value(Vector, strhash(target_attrib)));
               return;
            case SVF_noise:         wave.setNoise(get_numeric_value(Vector, strhash("noise"))); return;
            case SVF_phase:         wave.setPhase(get_numeric_value(Vector, strhash("phase"))); return;
            case SVF_thickness:     wave.setThickness(get_numeric_value(Vector, strhash("thickness"))); return;
         }
         break;
      }

      case CLASSID::VECTORTEXT: {
         auto &text = (objVectorText &)Vector;
         switch (hash) {
            case SVF_x:  text.setX(Unit(get_dimension(Vector, strhash("x")))); return;
            case SVF_y:  text.setY(Unit(get_dimension(Vector, strhash("y")))); return;
            case SVF_dx: text.set(strhash("dx"), get_string()); return;
            case SVF_dy: text.set(strhash("dy"), get_string()); return;

            case SVF_text_anchor:
               switch(strhash(get_string())) {
                  case SVF_start:   text.setAlign(ALIGN::LEFT); return;
                  case SVF_middle:  text.setAlign(ALIGN::HORIZONTAL); return;
                  case SVF_end:     text.setAlign(ALIGN::RIGHT); return;
                  case SVF_inherit: text.setAlign(ALIGN::NIL); return;
               }
               break;

            case SVF_rotate:         text.set(strhash("rotate"), get_string()); return;
            case SVF_string:         text.setString(get_string()); return;
            case SVF_kerning:        text.set(strhash("kerning"), get_string()); return; // Spacing between letters, default=1.0
            case SVF_letter_spacing: text.set(strhash("letterSpacing"), get_string()); return;
            case SVF_pathLength:     text.set(strhash("pathLength"), get_string()); return;
            case SVF_word_spacing:   text.set(strhash("wordSpacing"), get_string()); return;
            case SVF_font_family:    text.setFace(get_string()); return;
            case SVF_font_size:      text.set(strhash("fontSize"), get_numeric_value(Vector, strhash("fontSize"))); return;
         }
         break;
      }

      case CLASSID::VECTORELLIPSE: {
         auto &ellipse = (objVectorEllipse &)Vector;
         switch (hash) {
            case SVF_rx:     ellipse.setRadiusX(Unit(get_dimension(Vector, strhash("radiusX")))); return;
            case SVF_ry:     ellipse.setRadiusY(Unit(get_dimension(Vector, strhash("radiusY")))); return;
            case SVF_r:      ellipse.setRadius(Unit(get_dimension(Vector, strhash("radius")))); return;
            case SVF_cx:     ellipse.setCX(Unit(get_dimension(Vector, strhash("cx")))); return;
            case SVF_cy:     ellipse.setCY(Unit(get_dimension(Vector, strhash("cy")))); return;
            case SVF_width:  ellipse.setWidth(Unit(get_dimension(Vector, strhash("width")))); return;
            case SVF_height: ellipse.setHeight(Unit(get_dimension(Vector, strhash("height")))); return;
         }
         break;
      }

      case CLASSID::VECTORSHAPE: {
         auto &shape = (objVectorShape &)Vector;
         switch (hash) {
            case SVF_r:  shape.setRadius(Unit(get_dimension(Vector, strhash("radius")))); return;
            case SVF_cx: shape.setCX(Unit(get_dimension(Vector, strhash("cx")))); return;
            case SVF_cy: shape.setCY(Unit(get_dimension(Vector, strhash("cy")))); return;
         }
         break;
      }

      case CLASSID::VECTORSPIRAL: {
         auto &spiral = (objVectorSpiral &)Vector;
         switch (hash) {
            case SVF_r:      spiral.setRadius(Unit(get_dimension(Vector, strhash("radius")))); return;
            case SVF_cx:     spiral.setCX(Unit(get_dimension(Vector, strhash("cx")))); return;
            case SVF_cy:     spiral.setCY(Unit(get_dimension(Vector, strhash("cy")))); return;
            case SVF_width:  spiral.setWidth(Unit(get_dimension(Vector, strhash("width")))); return;
            case SVF_height: spiral.setHeight(Unit(get_dimension(Vector, strhash("height")))); return;
         }
         break;
      }

      case CLASSID::VECTORRECTANGLE: {
         auto &rect = (objVectorRectangle &)Vector;
         switch (hash) {
            case SVF_xOffset: rect.setXOffset(Unit(get_dimension(Vector, strhash("xOffset")))); return;
            case SVF_yOffset: rect.setYOffset(Unit(get_dimension(Vector, strhash("yOffset")))); return;
            case SVF_x:       rect.setX(Unit(get_dimension(Vector, strhash("x")))); return;
            case SVF_y:       rect.setY(Unit(get_dimension(Vector, strhash("y")))); return;
            case SVF_width:   rect.setWidth(Unit(get_dimension(Vector, strhash("width")))); return;
            case SVF_height:  rect.setHeight(Unit(get_dimension(Vector, strhash("height")))); return;
         }
         break;
      }

      case CLASSID::VECTORVIEWPORT: {
         auto &vp = (objVectorViewport &)Vector;
         switch (hash) {
            case SVF_x:       vp.setX(Unit(get_dimension(Vector, strhash("x")))); return;
            case SVF_y:       vp.setY(Unit(get_dimension(Vector, strhash("y")))); return;
            case SVF_xOffset: vp.setXOffset(Unit(get_dimension(Vector, strhash("xOffset")))); return;
            case SVF_yOffset: vp.setYOffset(Unit(get_dimension(Vector, strhash("yOffset")))); return;
            case SVF_width:   vp.setWidth(Unit(get_dimension(Vector, strhash("width")))); return;
            case SVF_height:  vp.setHeight(Unit(get_dimension(Vector, strhash("height")))); return;
         }
         break;
      }

      case CLASSID::VECTORPATH: {
         auto &path = (objVectorPath &)Vector;
         switch (hash) {
            case SVF_x:       path.setX(Unit(get_dimension(Vector, strhash("x")))); return;
            case SVF_y:       path.setY(Unit(get_dimension(Vector, strhash("y")))); return;
         }
      }

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
         const FRGB val = get_colour_value(Vector, strhash("fillColour"));
         Vector.setFillColour(val);
         return;
      }

      case SVF_fill: {
         const auto val = get_colour_value(Vector, strhash("fillColour"));
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
         auto val = get_numeric_value(Vector, strhash("fillOpacity"));
         Vector.setFillOpacity(val);
         return;
      }

      case SVF_stroke: {
         FRGB val = get_colour_value(Vector, strhash("strokeColour"));
         Vector.setStrokeColour(val);
         return;
      }

      case SVF_stroke_width:
         Vector.setStrokeWidth(Unit(get_numeric_value(Vector, strhash("strokeWidth"))));
         return;

      case SVF_stroke_linejoin:
         switch(strhash(get_string())) {
            case SVF_miter:       Vector.setLineJoin(VLJ::MITER); return;
            case SVF_round:       Vector.setLineJoin(VLJ::ROUND); return;
            case SVF_bevel:       Vector.setLineJoin(VLJ::BEVEL); return;
            case SVF_inherit:     Vector.setLineJoin(VLJ::INHERIT); return;
            case SVF_miter_clip:  Vector.setLineJoin(VLJ::MITER_SMART); return; // Special AGG only join type
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

      case SVF_stroke_opacity:          Vector.setStrokeOpacity(get_numeric_value(Vector, strhash("strokeOpacity"))); break;
      case SVF_stroke_miterlimit:       Vector.setMiterLimit(get_numeric_value(Vector, strhash("miterLimit"))); break;
      case SVF_stroke_inner_miterlimit: Vector.setInnerMiterLimit(get_numeric_value(Vector, strhash("innerMiterLimit"))); break;
      case SVF_stroke_dasharray:        Vector.set(strhash("dashArray"), get_string()); return;
      case SVF_stroke_dashoffset:       Vector.setDashOffset(get_numeric_value(Vector, strhash("dashOffset"))); return;
      case SVF_opacity:                 Vector.setOpacity(get_numeric_value(Vector, strhash("opacity"))); return;

      case SVF_display: {
         auto val = get_string();
         if (val IS "none")         Vector.setVisibility(VIS::HIDDEN);
         else if (val IS "inline")  Vector.setVisibility(VIS::VISIBLE);
         else if (val IS "inherit") Vector.setVisibility(VIS::INHERIT);
         return;
      }

      case SVF_visibility:
         switch (strhash(get_string())) {
            case SVF_hidden:   Vector.setVisibility(VIS::HIDDEN); return;
            case SVF_visible:  Vector.setVisibility(VIS::VISIBLE); return;
            case SVF_collapse: Vector.setVisibility(VIS::COLLAPSE); return;
            case SVF_inherit:  Vector.setVisibility(VIS::INHERIT); return;
            default: return;
         }

      case SVF_x: {
         if (Vector.Class->ClassID IS CLASSID::VECTORGROUP) {
            // Special case: SVG groups don't have an (x,y) position, but can declare one in the form of a
            // transform.  Refer to xtag_use() for a working example as to why.

            VectorMatrix *m = nullptr;
            Vector.getMatrices(m);
            for (; (m) and ((uint32_t)m->Tag != MTAG_SVG_TRANSFORM); m=m->Next);

            if (!m) {
               Vector.newMatrix(&m, false);
               m->Tag = MTAG_SVG_TRANSFORM;
            }

            if (m) {
               m->TranslateX = get_dimension(Vector, 0);
               vec::FlushMatrix(m);
            }
         }
         return;
      }

      case SVF_y: {
         if (Vector.Class->ClassID IS CLASSID::VECTORGROUP) {
            VectorMatrix *m = nullptr;
            Vector.getMatrices(m);
            for (; (m) and ((uint32_t)m->Tag != MTAG_SVG_TRANSFORM); m=m->Next);

            if (!m) {
               Vector.newMatrix(&m, false);
               m->Tag = MTAG_SVG_TRANSFORM;
            }

            if (m) {
               m->TranslateY = get_dimension(Vector, 0);
               vec::FlushMatrix(m);
            }
         }
         return;
      }
   }
}
