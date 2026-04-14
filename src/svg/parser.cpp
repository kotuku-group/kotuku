//********************************************************************************************************************
// For setting Fill and Stroke fields.  If the value is a url() then the referenced paint server will be initialised
// if it has not been already.

ERR svgState::set_paint_server(objVector *Vector, FIELD Field, const std::string Value)
{
   if (Value.starts_with("url(")) {
      if (Value[4] IS '#') {
         int i;
         for (i=5; (Value[i] != ')') and Value[i]; i++);
         std::string lookup;
         lookup.assign(Value, 5, i-5);

         if (Self->Scene->findDef(lookup.c_str(), nullptr) != ERR::Okay) {
            if (Self->IDs.contains(lookup)) {
               auto tag = Self->IDs[lookup];

               switch(strihash(tag->name())) {
                  case SVF_CONTOURGRADIENT:  proc_contourgradient(*tag); break;
                  case SVF_RADIALGRADIENT:   proc_radialgradient(*tag); break;
                  case SVF_DIAMONDGRADIENT:  proc_diamondgradient(*tag); break;
                  case SVF_CONICGRADIENT:    proc_conicgradient(*tag); break;
                  case SVF_LINEARGRADIENT:   proc_lineargradient(*tag); break;
               }
            }
         }
      }
   }

   return Vector->set(Field, Value);
}

//********************************************************************************************************************
// This function was created to manage inheritance for paint servers in the <defs> section.
//
// Use of 'inherit' is replaced with the actual state value at the time of the call.
// Use of 'currentColor' is also replaced to a deeper level, because it plays by different rules to inherit.

void svgState::process_inherit_refs(XTag &Tag) noexcept
{
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if (iequals("inherit", Tag.Attribs[a].Value)) {
         switch (strihash(Tag.Attribs[a].Name)) {
            case SVF_STOP_COLOR:
               Tag.Attribs[a].Value = m_stop_color;
               break;
            case SVF_STOP_OPACITY: Tag.Attribs[a].Value = std::to_string(m_stop_opacity); break;
         }
      }
      else if (iequals("currentColor", Tag.Attribs[a].Value)) {
         Tag.Attribs[a].Value = m_color;
      }
   }

   // Replace all use of currentColor in child tags

   std::function<void(pf::vector<XTag> &, const std::string &)> process_color;

   process_color = [&process_color](pf::vector<XTag> &Tags, const std::string &Colour) {
      for (auto &scan : Tags) {
         if (!scan.isTag()) continue;
         for (int a=1; a < std::ssize(scan.Attribs); a++) {
            if (iequals("currentColor", scan.Attribs[a].Value)) {
               scan.Attribs[a].Value = Colour;
            }
         }

         if (!scan.Children.empty()) process_color(scan.Children, Colour);
      }
   };

   if (!Tag.Children.empty()) {
      process_color(Tag.Children, m_color);
   }
}

//********************************************************************************************************************
// Aspect ratios are case insensitive

static ARF parse_aspect_ratio(std::string_view Value)
{
   Value.remove_prefix(std::min(Value.find_first_not_of(" \t\r\n"), Value.size()));

   if (iequals("none", Value)) return ARF::NONE;

   ARF flags = ARF::NIL;
   if (startswith("xMin", Value)) { flags |= ARF::X_MIN; Value.remove_prefix(4); }
   else if (startswith("xMid", Value)) { flags |= ARF::X_MID; Value.remove_prefix(4); }
   else if (startswith("xMax", Value)) { flags |= ARF::X_MAX; Value.remove_prefix(4); }

   if (startswith("yMin", Value)) { flags |= ARF::Y_MIN; Value.remove_prefix(4); }
   else if (startswith("yMid", Value)) { flags |= ARF::Y_MID; Value.remove_prefix(4); }
   else if (startswith("yMax", Value)) { flags |= ARF::Y_MAX; Value.remove_prefix(4); }

   Value.remove_prefix(std::min(Value.find_first_not_of(" \t\r\n"), Value.size()));

   if (startswith("meet", Value)) { flags |= ARF::MEET; }
   else if (startswith("slice", Value)) { flags |= ARF::SLICE; }

   return flags;
}

//********************************************************************************************************************

static RQ shape_rendering_to_render_quality(const std::string_view Value)
{
   if (iequals(Value, "auto")) return RQ::AUTO;
   else if (iequals(Value, "optimize-speed")) return RQ::FAST;
   else if (iequals(Value, "optimizeSpeed")) return RQ::FAST;
   else if (iequals(Value, "crisp-edges")) return RQ::CRISP;
   else if (iequals(Value, "crispEdges")) return RQ::CRISP;
   else if (iequals(Value, "geometric-precision")) return RQ::PRECISE;
   else if (iequals(Value, "geometricPrecision")) return RQ::PRECISE;
   else if (iequals(Value, "best")) return RQ::BEST;
   else {
      pf::Log log;
      log.warning("Unknown shape-rendering value '%s'", std::string(Value).c_str());
   }

   return RQ::AUTO;
}

//********************************************************************************************************************
// Apply current state values to a new vector as its defaults.  Used for applying state to child vectors.
// If you're adding more to this, matching code is needed in svgState::applyTag()

void svgState::applyStateToVector(objVector *Vector) const noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%s: Fill: %s, Stroke: %s, Opacity: %.2f, Font: %s %s",
      Vector->Class->ClassName, m_fill.c_str(), m_stroke.c_str(), m_opacity, m_font_family.c_str(), m_font_size.c_str());

   if (!m_fill.empty())   Vector->setFill(m_fill);
   if (!m_stroke.empty()) Vector->setStroke(m_stroke);
   if (m_stroke_width)    Vector->setStrokeWidth(m_stroke_width);
   if (m_line_join != VLJ::NIL)  Vector->setLineJoin(int(m_line_join));
   if (m_inner_join != VIJ::NIL) Vector->setInnerJoin(int(m_inner_join));
   if (m_line_cap != VLC::NIL)   Vector->setLineCap(int(m_line_cap));

   if (Vector->classID() IS CLASSID::VECTORTEXT) {
      if (!m_font_family.empty()) Vector->setFields(fl::Face(m_font_family));
      if (!m_font_size.empty())   Vector->setFields(fl::FontSize(m_font_size));
      if (m_font_weight)          Vector->setFields(fl::Weight(m_font_weight));
   }

   if (!m_display.empty()) {
      if (iequals("none", m_display))          Vector->setVisibility(VIS::HIDDEN);
      else if (iequals("inline", m_display))   Vector->setVisibility(VIS::VISIBLE);
      else if (iequals("inherit", m_display))  Vector->setVisibility(VIS::INHERIT);
   }

   if (!m_visibility.empty()) {
      if (iequals("visible", m_visibility))       Vector->setVisibility(VIS::VISIBLE);
      else if (iequals("hidden", m_visibility))   Vector->setVisibility(VIS::HIDDEN);
      else if (iequals("collapse", m_visibility)) Vector->setVisibility(VIS::COLLAPSE); // Same effect as hidden, kept for SVG compatibility
      else if (iequals("inherit", m_visibility))  Vector->setVisibility(VIS::INHERIT);
   }

   if (m_fill_opacity >= 0.0) Vector->set(FID_FillOpacity, m_fill_opacity);
   if (m_opacity >= 0.0) Vector->set(FID_Opacity, m_opacity);

   if (Vector->classID() != CLASSID::VECTORTEXT) {
      if (m_path_quality != RQ::AUTO) Vector->set(FID_PathQuality, int(m_path_quality));
   }
}

//********************************************************************************************************************
// Copy a tag's attributes to the current state.
// If you're adding more to this, matching code is needed in svgState::applyStateToVector()

void svgState::applyTag(const XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Total Attributes: %d", int(std::ssize(Tag.Attribs)));

   if (Tag.Children.empty()) {
      // If the tag has no children then few tags are worth saving to the state. E.g.
      // 'color' is only required for the 'currentColor' value.
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         switch (strihash(Tag.Attribs[a].Name)) {
            case SVF_COLOR:
               if ("inherit" IS val) break;
               m_color = val;
               break;
         }
      }
      return;
   }

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch (strihash(Tag.Attribs[a].Name)) {
         case SVF_COLOR: // Affects 'currentColor'
            if ("inherit" IS val) break;
            m_color = val;
            break;

         case SVF_FILL:
            if ("inherit" IS val);
            else if ("currentColor" IS val) m_fill = m_color;
            else m_fill = val;
            break;

         case SVF_STROKE:
            if ("inherit" IS val);
            else if ("currentColor" IS val) m_stroke = m_color;
            else m_stroke = val;
            if (!m_stroke_width) m_stroke_width = 1;
            break;

         case SVF_STOP_COLOR: m_stop_color = val; break;
         case SVF_DISPLAY:    m_display = val; break;
         case SVF_VISIBILITY: m_visibility = val; break;

         case SVF_STROKE_LINEJOIN:
            switch(strihash(val)) {
               case SVF_MITER:   m_line_join = VLJ::MITER; break;
               case SVF_ROUND:   m_line_join = VLJ::ROUND; break;
               case SVF_BEVEL:   m_line_join = VLJ::BEVEL; break;
               case SVF_INHERIT: m_line_join = VLJ::INHERIT; break;
               case SVF_MITER_CLIP: m_line_join = VLJ::MITER_SMART; break; // Special AGG only join type
               case SVF_MITER_ROUND:  m_line_join = VLJ::MITER_ROUND; break; // Special AGG only join type
            }
            break;

         case SVF_STROKE_INNERJOIN: // AGG ONLY
            switch(strihash(val)) {
               case SVF_MITER:   m_inner_join = VIJ::MITER; break;
               case SVF_ROUND:   m_inner_join = VIJ::ROUND; break;
               case SVF_BEVEL:   m_inner_join = VIJ::BEVEL; break;
               case SVF_INHERIT: m_inner_join = VIJ::INHERIT; break;
               case SVF_JAG:     m_inner_join = VIJ::JAG; break;
            }
            break;

         case SVF_STROKE_LINECAP:
            switch(strihash(val)) {
               case SVF_BUTT:    m_line_cap = VLC::BUTT; break;
               case SVF_SQUARE:  m_line_cap = VLC::SQUARE; break;
               case SVF_ROUND:   m_line_cap = VLC::ROUND; break;
               case SVF_INHERIT: m_line_cap = VLC::INHERIT; break;
            }
            break;

         case SVF_STROKE_WIDTH: m_stroke_width = strtod(val.c_str(), nullptr); break;
         case SVF_FONT_FAMILY:  m_font_family = val; break;
         case SVF_FONT_SIZE:
            m_font_size = val;
            m_font_size_px = UNIT(FID_FontSize, val).value;
            break;

         case SVF_FONT_WEIGHT: {
            m_font_weight = strtod(val.c_str(), nullptr);
            if (!m_font_weight) {
               switch(strihash(val)) {
                  case SVF_NORMAL:  m_font_weight = 400; break;
                  case SVF_LIGHTER: m_font_weight = 300; break; // -100 off the inherited weight
                  case SVF_BOLD:    m_font_weight = 700; break;
                  case SVF_BOLDER:  m_font_weight = 900; break; // +100 on the inherited weight
                  case SVF_INHERIT: m_font_weight = 400; break; // Not supported correctly yet.
                  default:
                     log.warning("No support for font-weight value '%s'", val.c_str()); // Non-fatal
                     m_font_weight = 400;
               }
            }
            break;
         }
         case SVF_FILL_OPACITY: m_fill_opacity = std::clamp(strtod(val.c_str(), nullptr), 0.0, 1.0); break;
         case SVF_OPACITY:      m_opacity = std::clamp(strtod(val.c_str(), nullptr), 0.0, 1.0); break;
         case SVF_STOP_OPACITY: m_stop_opacity = std::clamp(strtod(val.c_str(), nullptr), 0.0, 1.0); break;
         case SVF_SHAPE_RENDERING: m_path_quality = shape_rendering_to_render_quality(val); break;
      }
   }
}

//********************************************************************************************************************
// Process all child elements that belong to the target Tag.

void svgState::process_children(XTag &Tag, OBJECTPTR Vector) noexcept
{
   objVector *sibling = nullptr;
   for (auto &child : Tag.Children) {
      if (child.isTag()) {
         process_tag(child, Tag, Vector, sibling);
      }
   }
}

//********************************************************************************************************************
// Process a restricted set of children for shape objects.

void svgState::process_shape_children(XTag &Tag, OBJECTPTR Vector) noexcept
{
   pf::Log log;

   for (auto &child : Tag.Children) {
      if (!child.isTag()) continue;

      switch(strihash(child.name())) {
         case SVF_ANIMATE:          proc_animate(child, Tag, Vector); break;
         case SVF_ANIMATECOLOR:     proc_animate_colour(child, Tag, Vector); break;
         case SVF_ANIMATETRANSFORM: proc_animate_transform(child, Vector); break;
         case SVF_ANIMATEMOTION:    proc_animate_motion(child, Vector); break;
         case SVF_SET:              proc_set(child, Tag, Vector); break;
         case SVF_KOTUKU_MORPH:    proc_morph(child, Vector); break;

         case SVF_TEXTPATH:
            if (Vector->classID() IS CLASSID::VECTORTEXT) {
               if (!child.Children.empty()) {
                  auto buffer = child.getContent();
                  if (!buffer.empty()) {
                     pf::ltrim(buffer);
                     Vector->set(FID_String, buffer);
                  }
                  else log.msg("Failed to retrieve content for <text> @ line %d", Tag.LineNo);
               }

               proc_morph(child, Vector);
            }
            break;

         default:
            log.warning("Failed to interpret vector child element <%s/> @ line %d", child.name(), child.LineNo);
            break;
      }
   }
}

//********************************************************************************************************************

void svgState::proc_pathtransition(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   OBJECTPTR trans;
   if (NewObject(CLASSID::VECTORTRANSITION, &trans) IS ERR::Okay) {
      trans->setFields(
         fl::Owner(Self->Scene->UID), // All clips belong to the root page to prevent hierarchy issues.
         fl::Name("SVGTransition")
      );

      std::string id;
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         if (Tag.Attribs[a].Value.empty()) continue;

         switch(strihash(Tag.Attribs[a].Name)) {
            case SVF_ID: id = Tag.Attribs[a].Value; break;
         }
      }

      if (!id.empty()) {
         auto stops = process_transition_stops(Self, Tag.Children);
         if (stops.size() >= 2) {
            trans->set(FID_Stops, stops);

            if (InitObject(trans) IS ERR::Okay) {
               if (!Self->Cloning) Self->Scene->addDef(id.c_str(), trans);
               track_object(Self, trans);
               return;
            }
         }
         else log.warning("At least two stops are required for <pathTransition> at line %d.", Tag.LineNo);
      }
      else log.warning("No id attribute specified in <pathTransition> at line %d.", Tag.LineNo);

      FreeResource(trans);
   }
}

//********************************************************************************************************************

void svgState::proc_clippath(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   std::string id, transform, units;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_ID:            id        = value; break;
         case SVF_TRANSFORM:     transform = value; break;
         case SVF_CLIPPATHUNITS: units     = value; break;
         case SVF_EXTERNALRESOURCESREQUIRED: break; // Deprecated SVG attribute
         default:
            log.warning("<clipPath> attribute '%s' unrecognised @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            break;
      }
   }

   if (id.empty()) {
      // Declaring a clipPath without an id is poor form, but it is valid SVG and likely that at least
      // one child object will specify an id in this case.
      static int clip_id = 1;
      id = "auto_clippath_" + std::to_string(clip_id++);
   }

   // A clip-path with an ID can only be added once (important when a clip-path is repeatedly referenced)

   if (Self->Scene->findDef(id.c_str(), nullptr) != ERR::Okay) {
      objVector *clip;
      if (NewObject(CLASSID::VECTORCLIP, &clip) IS ERR::Okay) {
         clip->setFields(fl::Owner(Self->Scene->UID), fl::Name("SVGClip"));

         if (!transform.empty()) parse_transform(clip, transform, MTAG_SVG_TRANSFORM);

         if (!units.empty()) {
            if (iequals("userSpaceOnUse", units)) clip->set(FID_Units, int(VUNIT::USERSPACE));
            else if (iequals("objectBoundingBox", units)) clip->set(FID_Units, int(VUNIT::BOUNDING_BOX));
         }

         if (InitObject(clip) IS ERR::Okay) {
            svgState state(Self);

            // Valid child elements for clip-path are:
            // Shapes:   circle, ellipse, line, path, polygon, polyline, rect, text, ...
            // Commands: use, animate

            auto vp = clip->get<OBJECTPTR>(FID_Viewport);
            state.process_children(Tag, vp);

            Self->Scene->addDef(id.c_str(), clip);
            track_object(Self, clip);
         }
         else FreeResource(clip);
      }
   }
}

//********************************************************************************************************************
// NB: This implementation of mask support uses VectorClip.  An alternative would be to use VectorFilter.
//
// SVG masks are luminance masks by default (as opposed to masking on a per-channel RGBA basis).
//
// The formula used to get the luminance out of a given RGB value is: .2126R + .7152G + .0722B

void svgState::proc_mask(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   std::string id, transform;
   auto units = VUNIT::USERSPACE;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_ID:        id = value; break;
         case SVF_TRANSFORM: transform = value; break;
         case SVF_MASKUNITS:
            if (iequals("userSpaceOnUse", value)) units = VUNIT::USERSPACE;
            else if (iequals("objectBoundingBox", value)) units = VUNIT::BOUNDING_BOX;
            break;
         case SVF_MASKCONTENTUNITS: // TODO
            break;
         case SVF_EXTERNALRESOURCESREQUIRED: // Deprecated SVG attribute
            break;
         case SVF_COLOR_INTERPOLATION:
            break;
         case SVF_FILTER:
            break;
         case SVF_X:
         case SVF_Y:
         case SVF_WIDTH:
         case SVF_HEIGHT:
            break;
         default:
            log.warning("<mask> attribute '%s' unrecognised @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            break;
      }
   }

   if (id.empty()) {
      static int clip_id = 1;
      id = "auto_mask_" + std::to_string(clip_id++);
   }

   // A clip-path with an ID can only be added once (important when a clip-path is repeatedly referenced)

   if (Self->Scene->findDef(id.c_str(), nullptr) IS ERR::Okay) return;

   objVector *clip;
   if (NewObject(CLASSID::VECTORCLIP, &clip) IS ERR::Okay) {
      clip->setFields(fl::Owner(Self->Scene->UID), fl::Name("SVGMask"),
         fl::Flags(VCLF::APPLY_FILLS|VCLF::APPLY_STROKES),
         fl::Units(units));

      if (!transform.empty()) parse_transform(clip, transform, MTAG_SVG_TRANSFORM);

      if (InitObject(clip) IS ERR::Okay) {
         svgState state(Self);
         auto vp = clip->get<OBJECTPTR>(FID_Viewport);
         state.process_children(Tag, vp);

         Self->Scene->addDef(id.c_str(), clip);
         track_object(Self, clip);
      }
      else FreeResource(clip);
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_blur(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::BLURFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_STDDEVIATION: { // Y is optional, if not set then it is equivalent to X.
            double x = -1, y = -1;
            read_numseq(val, { &x, &y });
            if ((x) and (y IS -1)) y = x;
            if (x > 0) fx->set(FID_SX, x);
            if (y > 0) fx->set(FID_SY, y);
            break;
         }

         case SVF_X: UNIT(FID_X, val).set(fx); break;

         case SVF_Y: UNIT(FID_Y, val).set(fx); break;

         case SVF_WIDTH: UNIT(FID_Width, val).set(fx); break;

         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;

         case SVF_IN: parse_input(Self, fx, val, FID_SourceType, FID_Input); break;

         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_offset(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::OFFSETFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_DX: fx->set(FID_XOffset, (int64_t)strtol(val.c_str(), nullptr, 0)); break;
         case SVF_DY: fx->set(FID_YOffset, (int64_t)strtol(val.c_str(), nullptr, 0)); break;
         case SVF_IN: parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_merge(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::MERGEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_X: UNIT(FID_X, val).set(fx); break;
         case SVF_Y: UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH: UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
      }
   }

   std::vector<MergeSource> list;
   for (auto &child : Tag.Children) {
      if (iequals("feMergeNode", child.name())) {
         for (int a=1; a < std::ssize(child.Attribs); a++) {
            if (iequals("in", child.Attribs[a].Name)) {
               switch (strihash(child.Attribs[a].Value)) {
                  case SVF_SOURCEGRAPHIC:   list.push_back(VSF::GRAPHIC); break;
                  case SVF_SOURCEALPHA:     list.push_back(VSF::ALPHA); break;
                  case SVF_BACKGROUNDIMAGE: list.push_back(VSF::BKGD); break;
                  case SVF_BACKGROUNDALPHA: list.push_back(VSF::BKGD_ALPHA); break;
                  case SVF_FILLPAINT:       list.push_back(VSF::FILL); break;
                  case SVF_STROKEPAINT:     list.push_back(VSF::STROKE); break;
                  default:  {
                     if (auto ref = child.Attribs[a].Value.c_str()) {
                        while ((*ref) and (*ref <= 0x20)) ref++;
                        if (Self->Effects.contains(ref)) {
                           list.emplace_back(VSF::REFERENCE, Self->Effects[ref]);
                        }
                        else log.warning("Invalid 'in' reference '%s'", ref);
                     }
                     else log.warning("'in' reference is an empty string.");

                     break;
                  }
               }
            }
            else log.warning("Invalid feMergeNode attribute '%s'", child.Attribs[a].Name.c_str());
         }
      }
      else log.warning("Unrecognised feMerge child node '%s'", child.name());
   }

   if (!list.empty()) {
      if (fx->set(FID_SourceList, list) != ERR::Okay) {
         FreeResource(fx);
         return log.warning(ERR::SetField);
      }
   }

   if (fx->init() IS ERR::Okay) return ERR::Okay;
   else {
      FreeResource(fx);
      return log.warning(ERR::Init);
   }
}

//********************************************************************************************************************

#define CM_SIZE 20

static const std::array<double,20> glProtanopia = { 0.567,0.433,0,0,0, 0.558,0.442,0,0,0, 0,0.242,0.758,0,0, 0,0,0,1,0 };
static const std::array<double,20> glProtanomaly = { 0.817,0.183,0,0,0, 0.333,0.667,0,0,0, 0,0.125,0.875,0,0, 0,0,0,1,0 };
static const std::array<double,20> glDeuteranopia = { 0.625,0.375,0,0,0, 0.7,0.3,0,0,0, 0,0.3,0.7,0,0, 0,0,0,1,0 };
static const std::array<double,20> glDeuteranomaly = { 0.8,0.2,0,0,0, 0.258,0.742,0,0,0, 0,0.142,0.858,0,0, 0,0,0,1,0 };
static const std::array<double,20> glTritanopia = { 0.95,0.05,0,0,0, 0,0.433,0.567,0,0, 0,0.475,0.525,0,0, 0,0,0,1,0 };
static const std::array<double,20> glTritanomaly = { 0.967,0.033,0,0,0, 0,0.733,0.267,0,0, 0,0.183,0.817,0,0, 0,0,0,1,0 };
static const std::array<double,20> glAchromatopsia = { 0.299,0.587,0.114,0,0, 0.299,0.587,0.114,0,0, 0.299,0.587,0.114,0,0, 0,0,0,1,0 };
static const std::array<double,20> glAchromatomaly = { 0.618,0.320,0.062,0,0, 0.163,0.775,0.062,0,0, 0.163,0.320,0.516,0,0, 0,0,0,1,0 };

ERR svgState::parse_fe_colour_matrix(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::COLOURFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_TYPE: {
            const std::array<double, 20> *m = nullptr;
            CM mode = CM::NIL;
            switch(strihash(val)) {
               case SVF_NONE:          mode = CM::NONE; break;
               case SVF_MATRIX:        mode = CM::MATRIX; break;
               case SVF_SATURATE:      mode = CM::SATURATE; break;
               case SVF_HUEROTATE:     mode = CM::HUE_ROTATE; break;
               case SVF_LUMINANCETOALPHA: mode = CM::LUMINANCE_ALPHA; break;
               // These are special modes that are not included by SVG
               case SVF_CONTRAST:      mode = CM::CONTRAST; break;
               case SVF_BRIGHTNESS:    mode = CM::BRIGHTNESS; break;
               case SVF_HUE:           mode = CM::HUE; break;
               case SVF_COLOURISE:     mode = CM::COLOURISE; break;
               case SVF_DESATURATE:    mode = CM::DESATURATE; break;
               // Colour blindness modes
               case SVF_PROTANOPIA:    mode = CM::MATRIX; m = &glProtanopia; break;
               case SVF_PROTANOMALY:   mode = CM::MATRIX; m = &glProtanomaly; break;
               case SVF_DEUTERANOPIA:  mode = CM::MATRIX; m = &glDeuteranopia; break;
               case SVF_DEUTERANOMALY: mode = CM::MATRIX; m = &glDeuteranomaly; break;
               case SVF_TRITANOPIA:    mode = CM::MATRIX; m = &glTritanopia; break;
               case SVF_TRITANOMALY:   mode = CM::MATRIX; m = &glTritanomaly; break;
               case SVF_ACHROMATOPSIA: mode = CM::MATRIX; m = &glAchromatopsia; break;
               case SVF_ACHROMATOMALY: mode = CM::MATRIX; m = &glAchromatomaly; break;

               default:
                  log.warning("Unrecognised colour matrix type '%s'", val.c_str());
                  FreeResource(fx);
                  return ERR::InvalidValue;
            }

            fx->set(FID_Mode, int(mode));
            if ((mode IS CM::MATRIX) and (m)) fx->set(FID_Values, m);
            break;
         }

         case SVF_VALUES: {
            auto m = read_array<double>(val, CM_SIZE);
            fx->set(FID_Values, m);
            break;
         }

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_convolve_matrix(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::CONVOLVEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_ORDER: {
            double ox = 0, oy = 0;
            read_numseq(val, { &ox, &oy });
            if (ox < 1) ox = 3;
            if (oy < 1) oy = ox;
            fx->setFields(fl::MatrixColumns(int(ox)), fl::MatrixRows(int(oy)));
            break;
         }

         case SVF_KERNELMATRIX: {
            #define MAX_DIM 9
            auto matrix = read_array<double>(val, MAX_DIM * MAX_DIM);
            fx->set(FID_Matrix, matrix);
            break;
         }

         case SVF_DIVISOR: {
            double divisor = 0;
            read_numseq(val, { &divisor });
            fx->set(FID_Divisor, divisor);
            break;
         }

         case SVF_BIAS: {
            double bias = 0;
            read_numseq(val, { &bias });
            fx->set(FID_Bias, bias);
            break;
         }

         case SVF_TARGETX: fx->set(FID_TargetX, (int64_t)strtol(val.c_str(), nullptr, 0)); break;

         case SVF_TARGETY: fx->set(FID_TargetY, (int64_t)strtol(val.c_str(), nullptr, 0)); break;

         case SVF_EDGEMODE:
            if (iequals("duplicate", val)) fx->set(FID_EdgeMode, int(EM::DUPLICATE));
            else if (iequals("wrap", val)) fx->set(FID_EdgeMode, int(EM::WRAP));
            else if (iequals("none", val)) fx->set(FID_EdgeMode, int(EM::NONE));
            break;

         case SVF_KERNELUNITLENGTH: {
            double kx = 1, ky = 1;
            read_numseq(val, { &kx, &ky });
            if (kx < 1) kx = 1;
            if (ky < 1) ky = kx;
            fx->set(FID_UnitX, kx);
            fx->set(FID_UnitY, ky);
            break;
         }

         // The modifications will apply to R,G,B only when preserveAlpha is true.
         case SVF_PRESERVEALPHA:
            fx->set(FID_PreserveAlpha, iequals("true", val) or (std::string_view("1") IS val));
            break;

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_lighting(objVectorFilter *Filter, XTag &Tag, LT Type) noexcept
{
   pf::Log log(__FUNCTION__);
   objLightingFX *fx;

   if (NewObject(CLASSID::LIGHTINGFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   fx->set(FID_Type, int(Type));

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_LIGHTING_COLOUR:
         case SVF_LIGHTING_COLOR: {
            VectorPainter painter;
            if (iequals("currentColor", val)) {
               FRGB rgb;
               if (current_colour(Self->Scene->Viewport, rgb) IS ERR::Okay) fx->set(FID_Colour, rgb);
            }
            else if (vec::ReadPainter(nullptr, val.c_str(), &painter, nullptr) IS ERR::Okay) fx->set(FID_Colour, painter.Colour);
            break;
         }

         case SVF_KERNELUNITLENGTH: {
            double kx = 1, ky = 1;
            read_numseq(val, { &kx, &ky });
            if (kx < 1) kx = 1;
            if (ky < 1) ky = kx;
            fx->set(FID_UnitX, kx);
            fx->set(FID_UnitY, ky);
            break;
         }

         case SVF_SPECULARCONSTANT:
         case SVF_DIFFUSECONSTANT:  UNIT(FID_Constant, val).set(fx); break;
         case SVF_SURFACESCALE:     UNIT(FID_Scale, val).set(fx); break;
         case SVF_SPECULAREXPONENT: UNIT(FID_Exponent, val).set(fx); break;

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
         default:         log.warning("Unknown %s attribute %s", Tag.name(), Tag.Attribs[a].Name.c_str());
      }
   }

   // One child tag specifying the light source is required.

   if (!Tag.Children.empty()) {
      ERR error;
      auto &child = Tag.Children[0];
      if (std::string_view("feDistantLight") == child.name()) {
         double azimuth = 0, elevation = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strihash(child.Attribs[a].Name)) {
               case SVF_AZIMUTH:   azimuth   = strtod(child.Attribs[a].Value.c_str(), nullptr); break;
               case SVF_ELEVATION: elevation = strtod(child.Attribs[a].Value.c_str(), nullptr); break;
            }
         }

         error = fx->setDistantLight(azimuth, elevation);
      }
      else if (std::string_view("fePointLight") == child.name()) {
         double x = 0, y = 0, z = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strihash(child.Attribs[a].Name)) {
               case SVF_X: x = strtod(child.Attribs[a].Value.c_str(), nullptr); break;
               case SVF_Y: y = strtod(child.Attribs[a].Value.c_str(), nullptr); break;
               case SVF_Z: z = strtod(child.Attribs[a].Value.c_str(), nullptr); break;
            }
         }

         error = fx->setPointLight(x, y, z);
      }
      else if (std::string_view("feSpotLight") == child.name()) {
         double x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
         double exponent = 1, cone_angle = 0;

         for (int a=1; a < std::ssize(child.Attribs); a++) {
            auto &val = child.Attribs[a].Value;
            switch(strihash(child.Attribs[a].Name)) {
               case SVF_X:                 x = strtod(val.c_str(), nullptr); break;
               case SVF_Y:                 y = strtod(val.c_str(), nullptr); break;
               case SVF_Z:                 z = strtod(val.c_str(), nullptr); break;
               case SVF_POINTSATX:         px = strtod(val.c_str(), nullptr); break;
               case SVF_POINTSATY:         py = strtod(val.c_str(), nullptr); break;
               case SVF_POINTSATZ:         pz = strtod(val.c_str(), nullptr); break;
               case SVF_SPECULAREXPONENT:  exponent   = strtod(val.c_str(), nullptr); break;
               case SVF_LIMITINGCONEANGLE: cone_angle = strtod(val.c_str(), nullptr); break;
            }
         }

         error = fx->setSpotLight(x, y, z, px, py, pz, exponent, cone_angle);
      }
      else {
         log.warning("Unrecognised %s child node '%s'", Tag.name(), child.name());
         error = ERR::Failed;
      }

      if (error != ERR::Okay) {
         FreeResource(fx);
         return error;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_displacement_map(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::DISPLACEMENTFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_XCHANNELSELECTOR:
            switch(val[0]) {
               case 'r': case 'R': fx->set(FID_XChannel, int(CMP::RED)); break;
               case 'g': case 'G': fx->set(FID_XChannel, int(CMP::GREEN)); break;
               case 'b': case 'B': fx->set(FID_XChannel, int(CMP::BLUE)); break;
               case 'a': case 'A': fx->set(FID_XChannel, int(CMP::ALPHA)); break;
            }
            break;

         case SVF_YCHANNELSELECTOR:
            switch(val[0]) {
               case 'r': case 'R': fx->set(FID_YChannel, int(CMP::RED)); break;
               case 'g': case 'G': fx->set(FID_YChannel, int(CMP::GREEN)); break;
               case 'b': case 'B': fx->set(FID_YChannel, int(CMP::BLUE)); break;
               case 'a': case 'A': fx->set(FID_YChannel, int(CMP::ALPHA)); break;
            }
            break;

         case SVF_SCALE: fx->set(FID_Scale, strtod(val.c_str(), nullptr)); break;

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;

         case SVF_IN: parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_IN2: parse_input(Self, fx, val, FID_MixType, FID_Mix); break;

         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_wavefunction(objVectorFilter *Filter, XTag &Tag) noexcept
{
   objFilterEffect *fx;

   if (NewObject(CLASSID::WAVEFUNCTIONFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_RESOLUTION: fx->set(FID_Resolution, strtod(val.c_str(), nullptr)); break;
         case SVF_PRESERVEASPECTRATIO: fx->set(FID_AspectRatio, int(parse_aspect_ratio(val))); break;
         case SVF_SCALE:    fx->set(FID_Scale, strtod(val.c_str(), nullptr)); break;
         case SVF_N:        fx->set(FID_N, strtod(val.c_str(), nullptr)); break;
         case SVF_L:        fx->set(FID_L, strtod(val.c_str(), nullptr)); break;
         case SVF_M:        fx->set(FID_M, strtod(val.c_str(), nullptr)); break;
         case SVF_X:        UNIT(FID_X, val).set(fx); break;
         case SVF_Y:        UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:    UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT:   UNIT(FID_Height, val).set(fx); break;
         case SVF_COLORMAP: fx->set(FID_ColourMap, val); break;
         case SVF_RESULT:   result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);

      if (Tag.hasChildTags()) {
         auto stops = process_gradient_stops(Tag);
         if (stops.size() >= 2) fx->set(FID_Stops, stops);
      }

      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_component_xfer(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objRemapFX *fx;

   if (NewObject(CLASSID::REMAPFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   for (auto &child : Tag.Children) {
      if (wildcmp("feFunc?", child.name())) {
         auto cmp = CMP::NIL;
         switch(child.name()[6]) {
            case 'R': cmp = CMP::RED; break;
            case 'G': cmp = CMP::GREEN; break;
            case 'B': cmp = CMP::BLUE; break;
            case 'A': cmp = CMP::ALPHA; break;
            default:
               log.warning("Invalid feComponentTransfer element %s", child.name());
               return ERR::Failed;
         }

         uint32_t type = 0;
         int mask = 0xff;
         double amp = 1.0, offset = 0, exp = 1.0, slope = 1.0, intercept = 0.0;
         std::vector<double> values;
         for (int a=1; a < std::ssize(child.Attribs); a++) {
            switch(strihash(child.Attribs[a].Name)) {
               case SVF_TYPE:        type = strihash(child.Attribs[a].Value); break;
               case SVF_AMPLITUDE:   read_numseq(child.Attribs[a].Value, { &amp }); break;
               case SVF_INTERCEPT:   read_numseq(child.Attribs[a].Value, { &intercept }); break;
               case SVF_SLOPE:       read_numseq(child.Attribs[a].Value, { &slope }); break;
               case SVF_EXPONENT:    read_numseq(child.Attribs[a].Value, { &exp }); break;
               case SVF_OFFSET:      read_numseq(child.Attribs[a].Value, { &offset }); break;
               case SVF_MASK:        mask = strtol(child.Attribs[a].Value.c_str(), nullptr, 0); break;
               case SVF_TABLEVALUES: {
                  values = read_array<double>(child.Attribs[a].Value, 64);
                  break;
               }
               default: log.warning("Unknown %s attribute %s", child.name(), child.Attribs[a].Name.c_str()); break;
            }
         }

         switch(type) {
            case SVF_TABLE:    fx->selectTable(cmp, values.data(), values.size()); break;
            case SVF_LINEAR:   fx->selectLinear(cmp, slope, intercept);  break;
            case SVF_GAMMA:    fx->selectGamma(cmp, amp, offset, exp);  break;
            case SVF_DISCRETE: fx->selectDiscrete(cmp, values.data(), values.size());  break;
            case SVF_IDENTITY: fx->selectIdentity(cmp); break;
            // The following additions are specific to Kotuku and not SVG compatible.
            case SVF_INVERT:   fx->selectInvert(cmp); break;
            case SVF_MASK:     fx->selectMask(cmp, mask); break;
            default:
               log.warning("feComponentTransfer node failed to specify its type.");
               return ERR::UndefinedField;
         }
      }
      else log.warning("Unrecognised feComponentTransfer child node '%s'", child.name());
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_composite(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::COMPOSITEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_MODE:
         case SVF_OPERATOR: {
            switch (strihash(val)) {
               // SVG Operator types
               case SVF_NORMAL:
               case SVF_OVER: fx->set(FID_Operator, int(OP::OVER)); break;
               case SVF_IN:   fx->set(FID_Operator, int(OP::IN)); break;
               case SVF_OUT:  fx->set(FID_Operator, int(OP::OUT)); break;
               case SVF_ATOP: fx->set(FID_Operator, int(OP::ATOP)); break;
               case SVF_XOR:  fx->set(FID_Operator, int(OP::XOR)); break;
               case SVF_ARITHMETIC: fx->set(FID_Operator, int(OP::ARITHMETIC)); break;
               // SVG Mode types
               case SVF_SCREEN:   fx->set(FID_Operator, int(OP::SCREEN)); break;
               case SVF_MULTIPLY: fx->set(FID_Operator, int(OP::MULTIPLY)); break;
               case SVF_LIGHTEN:  fx->set(FID_Operator, int(OP::LIGHTEN)); break;
               case SVF_DARKEN:   fx->set(FID_Operator, int(OP::DARKEN)); break;
               // Kotuku modes
               case SVF_INVERTRGB:  fx->set(FID_Operator, int(OP::INVERT_RGB)); break;
               case SVF_INVERT:     fx->set(FID_Operator, int(OP::INVERT)); break;
               case SVF_CONTRAST:   fx->set(FID_Operator, int(OP::CONTRAST)); break;
               case SVF_DODGE:      fx->set(FID_Operator, int(OP::DODGE)); break;
               case SVF_BURN:       fx->set(FID_Operator, int(OP::BURN)); break;
               case SVF_HARDLIGHT:  fx->set(FID_Operator, int(OP::HARD_LIGHT)); break;
               case SVF_SOFTLIGHT:  fx->set(FID_Operator, int(OP::SOFT_LIGHT)); break;
               case SVF_DIFFERENCE: fx->set(FID_Operator, int(OP::DIFFERENCE)); break;
               case SVF_EXCLUSION:  fx->set(FID_Operator, int(OP::EXCLUSION)); break;
               case SVF_PLUS:       fx->set(FID_Operator, int(OP::PLUS)); break;
               case SVF_MINUS:      fx->set(FID_Operator, int(OP::MINUS)); break;
               case SVF_OVERLAY:    fx->set(FID_Operator, int(OP::OVERLAY)); break;
               default:
                  log.warning("Composite operator '%s' not recognised.", val.c_str());
                  FreeResource(fx);
                  return ERR::InvalidValue;
            }
            break;
         }

         case SVF_K1: {
            double k1;
            read_numseq(val, { &k1 });
            fx->set(FID_K1, k1);
            break;
         }

         case SVF_K2: {
            double k2;
            read_numseq(val, { &k2 });
            fx->set(FID_K2, k2);
            break;
         }

         case SVF_K3: {
            double k3;
            read_numseq(val, { &k3 });
            fx->set(FID_K3, k3);
            break;
         }

         case SVF_K4: {
            double k4;
            read_numseq(val, { &k4 });
            fx->set(FID_K4, k4);
            break;
         }

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_IN2:    parse_input(Self, fx, val, FID_MixType, FID_Mix); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_flood(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::FLOODFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   ERR error = ERR::Okay;
   std::string result_name;
   for (int a=1; (a < std::ssize(Tag.Attribs)) and (error IS ERR::Okay); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_FLOOD_COLOR:
         case SVF_FLOOD_COLOUR: {
            VectorPainter painter;
            if (iequals("currentColor", val)) {
               if (current_colour(Self->Scene->Viewport, painter.Colour) IS ERR::Okay) error = fx->set(FID_Colour, painter.Colour);
            }
            else if (vec::ReadPainter(nullptr, val.c_str(), &painter, nullptr) IS ERR::Okay) error = fx->set(FID_Colour, painter.Colour);
            break;
         }

         case SVF_FLOOD_OPACITY: {
            error = fx->set(FID_Opacity, std::clamp(strtod(val.c_str(), nullptr), 0.0, 1.0));
            break;
         }

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return log.warning(error);
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_turbulence(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::TURBULENCEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_BASEFREQUENCY: {
            double bfx = -1, bfy = -1;
            read_numseq(val, { &bfx, &bfy });
            if (bfx < 0) bfx = 0;
            if (bfy < 0) bfy = bfx;
            fx->setFields(fl::FX(bfx), fl::FY(bfy));
            break;
         }

         case SVF_NUMOCTAVES: fx->set(FID_Octaves, (int64_t)strtol(val.c_str(), nullptr, 0)); break;

         case SVF_SEED: fx->set(FID_Seed, (int64_t)strtol(val.c_str(), nullptr, 0)); break;

         case SVF_STITCHTILES:
            if (iequals("stitch", val)) fx->set(FID_Stitch, TRUE);
            else fx->set(FID_Stitch, FALSE);
            break;

         case SVF_TYPE:
            if (iequals("fractalNoise", val)) fx->set(FID_Type, int(TB::NOISE));
            else fx->set(FID_Type, 0);
            break;

         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************

ERR svgState::parse_fe_morphology(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::MORPHOLOGYFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   std::string result_name;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_RADIUS: {
            double x = -1, y = -1;
            read_numseq(val, { &x, &y });
            if (x > 0) fx->set(FID_RadiusX, int(x));
            if (y > 0) fx->set(FID_RadiusY, int(y));
            break;
         }

         case SVF_OPERATOR: fx->set(FID_Operator, val); break;
         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_IN:     parse_input(Self, fx, val, FID_SourceType, FID_Input); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   if (fx->init() IS ERR::Okay) {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
   else {
      FreeResource(fx);
      return ERR::Init;
   }
}

//********************************************************************************************************************
// This code replaces feImage elements where the href refers to a resource name.

ERR svgState::parse_fe_source(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objFilterEffect *fx;

   if (NewObject(CLASSID::SOURCEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   bool required = false;
   std::string ref, result_name;

   ERR error = ERR::Okay;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;
         case SVF_PRESERVEASPECTRATIO: fx->set(FID_AspectRatio, int(parse_aspect_ratio(val))); break;
         case SVF_XLINK_HREF: ref = val; break;
         case SVF_EXTERNALRESOURCESREQUIRED: required = iequals("true", val); break;
         case SVF_RESULT: result_name = val; break;
      }
   }

   objVector *vector = nullptr;
   if (!ref.empty()) {
      if (Self->Scene->findDef(ref.c_str(), (OBJECTPTR *)&vector) != ERR::Okay) {
         // The reference is not an existing vector but should be a pre-registered declaration that would allow
         // us to create it.  Note that creation only occurs once.  Subsequent use of the ID will result in the
         // live reference being found.

         if (auto tagref = find_href_tag(Self, ref)) {
            process_tag(*tagref, Tag, Self->Scene, vector);
         }
         else log.warning("Element id '%s' not found.", ref.c_str());
      }

      if (vector) {
         fx->set(FID_SourceName, ref);
         if (error = fx->init(); error IS ERR::Okay) {
            if (!result_name.empty()) parse_result(Self, fx, result_name);
            return ERR::Okay;
         }
      }
      else error = ERR::Search;
   }
   else error = ERR::UndefinedField;

   FreeResource(fx);
   if (required) return log.warning(error);
   return ERR::Okay; // Default behaviour is not to force a failure despite the error.
}

//********************************************************************************************************************

ERR svgState::parse_fe_image(objVectorFilter *Filter, XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   // Check if the client has specified an href that refers to a pattern name instead of an image file.  In that
   // case we need to divert to the SourceFX parser.

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      if ((iequals("xlink:href", Tag.Attribs[a].Name)) or (iequals("href", Tag.Attribs[a].Name))) {
         if ((Tag.Attribs[a].Value[0] IS '#')) {
            return parse_fe_source(Filter, Tag);
         }
         break;
      }
   }

   objFilterEffect *fx;
   if (NewObject(CLASSID::IMAGEFX, &fx) != ERR::Okay) return ERR::NewObject;
   SetOwner(fx, Filter);

   bool image_required = false;
   std::string path;
   std::string result_name;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_X:      UNIT(FID_X, val).set(fx); break;
         case SVF_Y:      UNIT(FID_Y, val).set(fx); break;
         case SVF_WIDTH:  UNIT(FID_Width, val).set(fx); break;
         case SVF_HEIGHT: UNIT(FID_Height, val).set(fx); break;

         case SVF_IMAGE_RENDERING: {
            if (iequals("optimizeSpeed", val)) fx->set(FID_ResampleMethod, int(VSM::BILINEAR));
            else if (iequals("optimizeQuality", val)) fx->set(FID_ResampleMethod, int(VSM::LANCZOS));
            else if (iequals("auto", val));
            else if (iequals("inherit", val));
            else log.warning("Unrecognised image-rendering option '%s'", val.c_str());
            break;
         }

         case SVF_PRESERVEASPECTRATIO:
            fx->set(FID_AspectRatio, int(parse_aspect_ratio(val)));
            break;

         case SVF_XLINK_HREF:
            path = val;
            break;

         case SVF_EXTERNALRESOURCESREQUIRED: // If true and the image cannot be loaded, return a fatal error code.
            if (iequals("true", val)) image_required = true;
            break;

         case SVF_RESULT: result_name = val; break;
      }
   }

   if (!path.empty()) {
      // Check for security risks in the path.

      if ((path[0] IS '/') or ((path[0] IS '.') and (path[1] IS '.') and (path[2] IS '/'))) {
         FreeResource(fx);
         return log.warning(ERR::InvalidValue);
      }
      else {
         if (path.find(':') != std::string::npos) {
            FreeResource(fx);
            return log.warning(ERR::InvalidValue);
         }

         for (unsigned i=0; path[i]; i++) {
            if (path[i] IS '/') {
               while (path[i+1] IS '.') i++;
               if (path[i+1] IS '/') {
                  return log.warning(ERR::InvalidValue);
               }
            }
         }
      }

      if (auto fl = folder(Self)) {
         std::string comp_path = std::string(fl) + path;
         fx->set(FID_Path, comp_path);
      }
      else fx->set(FID_Path, path);
   }

   if (auto error = fx->init(); error != ERR::Okay) {
      FreeResource(fx);
      if (image_required) return error;
      else return ERR::Okay;
   }
   else {
      if (!result_name.empty()) parse_result(Self, fx, result_name);
      return ERR::Okay;
   }
}

//********************************************************************************************************************

void svgState::proc_filter(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);

   objVectorFilter *filter;
   std::string id;

   if (NewObject(CLASSID::VECTORFILTER, &filter) IS ERR::Okay) {
      filter->setFields(fl::Owner(Self->Scene->UID), fl::Name("SVGFilter"),
         fl::Units(VUNIT::BOUNDING_BOX), fl::ColourSpace(VCS::LINEAR_RGB));

      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         int j;
         for (j=0; Tag.Attribs[a].Name[j] and (Tag.Attribs[a].Name[j] != ':'); j++);
         if (Tag.Attribs[a].Name[j] IS ':') continue;

         switch(strihash(Tag.Attribs[a].Name)) {
            case SVF_FILTERUNITS:
               if (iequals("userSpaceOnUse", val)) filter->Units = VUNIT::USERSPACE;
               else if (iequals("objectBoundingBox", val)) filter->Units = VUNIT::BOUNDING_BOX;
               break;

            case SVF_ID:      id = val; break;

            case SVF_X:       UNIT(FID_X, val).set(filter); break;
            case SVF_Y:       UNIT(FID_Y, val).set(filter); break;
            case SVF_WIDTH:   UNIT(FID_Width, val).set(filter); break;
            case SVF_HEIGHT:  UNIT(FID_Height, val).set(filter); break;
            case SVF_OPACITY: UNIT(FID_Opacity, std::clamp(strtod(val.c_str(), nullptr), 0.0, 1.0)).set(filter); break;

            case SVF_FILTERRES: {
               double x = 0, y = 0;
               read_numseq(val, { &x, &y });
               filter->setFields(fl::ResX(int(x)), fl::ResY(int(y)));
               break;
            }

            case SVF_COLOR_INTERPOLATION_FILTERS: // The default is linearRGB
               if (iequals("auto", val)) filter->set(FID_ColourSpace, int(VCS::LINEAR_RGB));
               else if (iequals("sRGB", val)) filter->set(FID_ColourSpace, int(VCS::SRGB));
               else if (iequals("linearRGB", val)) filter->set(FID_ColourSpace, int(VCS::LINEAR_RGB));
               else if (iequals("inherit", val)) filter->set(FID_ColourSpace, int(VCS::INHERIT));
               break;

            case SVF_PRIMITIVEUNITS:
               if (iequals("userSpaceOnUse", val)) filter->PrimitiveUnits = VUNIT::USERSPACE; // Default
               else if (iequals("objectBoundingBox", val)) filter->PrimitiveUnits = VUNIT::BOUNDING_BOX;
               break;

/*
            case SVF_VIEWBOX: {
               double x=0, y=0, width=0, height=0;
               read_numseq(val, { &x, &y, &width, &height });
               filter->Viewport->setFields(fl::ViewX(x), fl::ViewY(y), fl::ViewWidth(width), fl::ViewHeight(height));
               break;
            }
*/
            default:
               log.warning("<%s> attribute '%s' unrecognised @ line %d", Tag.name(), Tag.Attribs[a].Name.c_str(), Tag.LineNo);
               break;
         }
      }

      if ((!id.empty()) and (filter->init() IS ERR::Okay)) {
         SetName(filter, id.c_str());

         for (auto child : Tag.Children) {
            log.trace("Parsing filter element '%s'", child.name());

            switch(strihash(child.name())) {
               case SVF_FEBLUR:              parse_fe_blur(filter, child); break;
               case SVF_FEGAUSSIANBLUR:      parse_fe_blur(filter, child); break;
               case SVF_FEOFFSET:            parse_fe_offset(filter, child); break;
               case SVF_FEMERGE:             parse_fe_merge(filter, child); break;
               case SVF_FECOLORMATRIX:       // American spelling
               case SVF_FECOLOURMATRIX:      parse_fe_colour_matrix(filter, child); break;
               case SVF_FECONVOLVEMATRIX:    parse_fe_convolve_matrix(filter, child); break;
               case SVF_FEDROPSHADOW:        log.warning("Support for feDropShadow not yet implemented."); break;
               case SVF_FEBLEND:             // Blend and composite share the same code.
               case SVF_FECOMPOSITE:         parse_fe_composite(filter, child); break;
               case SVF_FEFLOOD:             parse_fe_flood(filter, child); break;
               case SVF_FETURBULENCE:        parse_fe_turbulence(filter, child); break;
               case SVF_FEMORPHOLOGY:        parse_fe_morphology(filter, child); break;
               case SVF_FEIMAGE:             parse_fe_image(filter, child); break;
               case SVF_FECOMPONENTTRANSFER: parse_fe_component_xfer(filter, child); break;
               case SVF_FEDIFFUSELIGHTING:   parse_fe_lighting(filter, child, LT::DIFFUSE); break;
               case SVF_FESPECULARLIGHTING:  parse_fe_lighting(filter, child, LT::SPECULAR); break;
               case SVF_FEDISPLACEMENTMAP:   parse_fe_displacement_map(filter, child); break;
               case SVF_FEWAVEFUNCTION:      parse_fe_wavefunction(filter, child); break;
               case SVF_FETILE:
                  log.warning("Filter element '%s' is not currently supported.", child.name());
                  break;

               default:
                  log.warning("Filter element '%s' not recognised.", child.name());
                  break;
            }
         }

         Self->Effects.clear();

         if (!Self->Cloning) Self->Scene->addDef(id.c_str(), filter);

         track_object(Self, filter);
      }
      else FreeResource(filter);
   }
}

//********************************************************************************************************************
// NB: In bounding-box mode, the default view-box is 0 0 1 1, where 1 is equivalent to 100% of the target space.
// If the client sets a custom view-box then the dimensions are fixed, and no scaling will apply.

void svgState::proc_pattern(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objVectorPattern *pattern;
   std::string id;

   if (NewObject(CLASSID::VECTORPATTERN, &pattern) IS ERR::Okay) {
      SetOwner(pattern, Self->Scene);

      // NOTE: In SVG 1.0 the default pattern units value is 'userSpaceOnUse'; from 1.1 it changed to 'objectBoundingBox'.

      pattern->setFields(fl::Name("SVGPattern"),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::SpreadMethod(VSPREAD::REPEAT),
         fl::HostScene(Self->Scene));

      objVectorViewport *viewport;
      pattern->get(FID_Viewport, viewport);

      bool rel_coords = true; // True because the default is 'objectBoundingBox'
      std::string x, y, width, height;
      std::vector<XTag *> children; // Multiple retrieval points for children is possible due to the href attrib
      std::string dummy;

      const auto parse_pattern = [&](const auto &parse_self, XTag &Tags, std::string &ID) -> void {
         for (int a=1; a < std::ssize(Tags.Attribs); a++) {
            auto &val = Tags.Attribs[a].Value;
            if (val.empty()) continue;

            switch(strihash(Tags.Attribs[a].Name)) {
               case SVF_PATTERNCONTENTUNITS:
                  // SVG: "This attribute has no effect if viewbox is specified"
                  //
                  // userSpaceOnUse: Default. Coordinate values are fixed.
                  // objectBoundingBox: Coordinate values are relative (0 - 1.0) to the bounding box of the requesting element.
                  //   Implementing this means allocating a 1x1 viewbox for the content, then stretching it to fit the parent element.

                  if (iequals("userSpaceOnUse", val)) pattern->setContentUnits(VUNIT::USERSPACE);
                  else if (iequals("objectBoundingBox", val)) {
                     pattern->setContentUnits(VUNIT::BOUNDING_BOX);
                     viewport->setFields(fl::ViewX(0), fl::ViewY(0), fl::ViewWidth(1.0), fl::ViewHeight(1.0));
                  }
                  break;

               case SVF_PATTERNUNITS:
                  // 'userSpace' is a deprecated option from SVG 1.0 - perhaps due to the introduction of patternContentUnits.
                  if (iequals("userSpaceOnUse", val)) { rel_coords = false; pattern->setUnits(VUNIT::USERSPACE); }
                  else if (iequals("objectBoundingBox", val)) { rel_coords = true; pattern->setUnits(VUNIT::BOUNDING_BOX); }
                  else if (iequals("userSpace", val)) { rel_coords = false; pattern->setUnits(VUNIT::USERSPACE); }
                  break;

               case SVF_PATTERNTRANSFORM: pattern->setTransform(val); break;

               case SVF_ID:       ID = val; break;

               case SVF_OVERFLOW: viewport->set(FID_Overflow, val); break;

               case SVF_OPACITY:  UNIT(FID_Opacity, val).set(pattern); break;
               case SVF_X:        x = val; break;
               case SVF_Y:        y = val; break;
               case SVF_WIDTH:    width = val; break;
               case SVF_HEIGHT:   height = val; break;

               case SVF_VIEWBOX: {
                  double vx=0, vy=0, vwidth=1, vheight=1; // Default view-box for bounding-box mode
                  pattern->ContentUnits = VUNIT::USERSPACE;
                  read_numseq(val, { &vx, &vy, &vwidth, &vheight });
                  viewport->setFields(fl::ViewX(vx), fl::ViewY(vy), fl::ViewWidth(vwidth), fl::ViewHeight(vheight));
                  break;
               }

               case SVF_HREF:
               case SVF_XLINK_HREF:
                  if (auto other = find_href_tag(Self, val)) {
                     parse_self(parse_self, *other, dummy);
                  }
                  break;

               default:
                  log.warning("%s attribute '%s' unrecognised @ line %d", Tags.name(), Tags.Attribs[a].Name.c_str(), Tags.LineNo);
                  break;
            }
         }

         if (Tags.hasChildTags()) children.push_back(&Tags);
      };

      parse_pattern(parse_pattern, Tag, id);

      if (!x.empty()) UNIT(FID_X, x, rel_coords ? DU::SCALED : DU::PIXEL).set(pattern);
      if (!y.empty()) UNIT(FID_Y, y, rel_coords ? DU::SCALED : DU::PIXEL).set(pattern);

      if (!width.empty()) {
         if (pattern->ContentUnits IS VUNIT::BOUNDING_BOX) {
            UNIT(FID_Width, width, DU::SCALED).set(pattern);
         }
         else UNIT(FID_Width, width, rel_coords ? DU::SCALED : DU::PIXEL).set(pattern);
      }

      if (!height.empty()) {
         if (pattern->ContentUnits IS VUNIT::BOUNDING_BOX) {
            UNIT(FID_Height, height, DU::SCALED).set(pattern);
         }
         else UNIT(FID_Height, height, rel_coords ? DU::SCALED : DU::PIXEL).set(pattern);
      }

      if (id.empty()) {
         FreeResource(pattern);
         log.trace("Failed to create a valid definition.");
      }

      if (InitObject(pattern) IS ERR::Okay) {
         // Child vectors for the pattern need to be instantiated and belong to the pattern's Viewport.
         svgState state(Self);
         for (auto tag : children) {
            state.process_children(*tag, viewport);
         }

         if (!Self->Cloning) {
            Self->Scene->addDef(id.c_str(), pattern);
            track_object(Self, pattern);
         }
      }
      else {
         FreeResource(pattern);
         log.trace("Pattern initialisation failed.");
      }
   }
}

//********************************************************************************************************************

ERR svgState::proc_shape(CLASSID VectorID, XTag &Tag, OBJECTPTR Parent, objVector * &Result) noexcept
{
   objVector *vector;

   Result = nullptr;
   if (auto error = NewObject(VectorID, &vector); error IS ERR::Okay) {
      SetOwner(vector, Parent);
      svgState state = *this;
      state.applyStateToVector(vector);
      state.applyTag(Tag); // Apply all attribute values to the current state.
      state.process_attrib(Tag, vector);

      if (vector->init() IS ERR::Okay) {
         state.process_shape_children(Tag, vector);
         Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(vector->UID) });
         Result = vector;
         return error;
      }
      else {
         FreeResource(vector);
         return ERR::Init;
      }
   }
   else return ERR::CreateObject;
}

//********************************************************************************************************************
// See also process_children()

ERR svgState::process_tag(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%s", Tag.name());

   switch(strihash(Tag.name())) {
      case SVF_USE:              proc_use(Tag, Parent); break;
      case SVF_A:                proc_link(Tag, Parent, Vector); break;
      case SVF_SWITCH:           proc_switch(Tag, Parent, Vector); break;
      case SVF_G:                proc_group(Tag, Parent, Vector); break;
      case SVF_SVG:              proc_svg(Tag, Parent, Vector); break;

      case SVF_RECT:             proc_shape(CLASSID::VECTORRECTANGLE, Tag, Parent, Vector); break;
      case SVF_ELLIPSE:          proc_shape(CLASSID::VECTORELLIPSE, Tag, Parent, Vector); break;
      case SVF_CIRCLE:           proc_shape(CLASSID::VECTORELLIPSE, Tag, Parent, Vector); break;
      case SVF_PATH:             proc_shape(CLASSID::VECTORPATH, Tag, Parent, Vector); break;
      case SVF_POLYGON:          proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector); break;
      case SVF_KOTUKU_SPIRAL:    proc_shape(CLASSID::VECTORSPIRAL, Tag, Parent, Vector); break;
      case SVF_KOTUKU_WAVE:      proc_shape(CLASSID::VECTORWAVE, Tag, Parent, Vector); break;
      case SVF_KOTUKU_SHAPE:     proc_shape(CLASSID::VECTORSHAPE, Tag, Parent, Vector); break;
      case SVF_IMAGE:            proc_image(Tag, Parent, Vector); break;
      // Paint servers are deferred and will only be processed if they are referenced via url()
      case SVF_CONTOURGRADIENT:  process_inherit_refs(Tag); break;
      case SVF_RADIALGRADIENT:   process_inherit_refs(Tag); break;
      case SVF_DIAMONDGRADIENT:  process_inherit_refs(Tag); break;
      case SVF_CONICGRADIENT:    process_inherit_refs(Tag); break;
      case SVF_LINEARGRADIENT:   process_inherit_refs(Tag); break;
      case SVF_SYMBOL:           proc_symbol(Tag); break;
      case SVF_ANIMATE:          proc_animate(Tag, ParentTag, Parent); break;
      case SVF_ANIMATECOLOR:     proc_animate_colour(Tag, ParentTag, Parent); break;
      case SVF_ANIMATETRANSFORM: proc_animate_transform(Tag, Parent); break;
      case SVF_ANIMATEMOTION:    proc_animate_motion(Tag, Parent); break;
      case SVF_SET:              proc_set(Tag, ParentTag, Parent); break;
      case SVF_FILTER:           proc_filter(Tag); break;
      case SVF_DEFS:             proc_defs(Tag, Parent); break;
      case SVF_CLIPPATH:         proc_clippath(Tag); break;
      case SVF_MASK:             proc_mask(Tag); break;
      case SVF_STYLE:            proc_style(Tag); break;
      case SVF_PATTERN:          proc_pattern(Tag); break;

      case SVF_TITLE:
         if (Self->Title) { FreeResource(Self->Title); Self->Title = nullptr; }
         if (!Tag.Children.empty()) {
            if (auto buffer = Tag.getContent(); !buffer.empty()) {
               pf::ltrim(buffer);
               Self->Title = strclone(buffer);
            }
         }
         break;

      case SVF_LINE:
         proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector);
         Vector->set(FID_Closed, FALSE);
         break;

      case SVF_POLYLINE:
         proc_shape(CLASSID::VECTORPOLYGON, Tag, Parent, Vector);
         Vector->set(FID_Closed, FALSE);
         break;

      case SVF_TEXT: {
         if (proc_shape(CLASSID::VECTORTEXT, Tag, Parent, Vector) IS ERR::Okay) {
            if (!Tag.Children.empty()) {
               CSTRING existing_str = nullptr;
               Vector->get(FID_String, existing_str);

               if (auto buffer = Tag.getContent(); !buffer.empty()) {
                  pf::ltrim(buffer);
                  if (existing_str) buffer.insert(0, existing_str);
                  Vector->set(FID_String, buffer);
               }
               else log.msg("Failed to retrieve content for <text> @ line %d", Tag.LineNo);
            }
         }
         break;
      }

      case SVF_DESC: break; // Ignore descriptions

      default: log.warning("Failed to interpret tag <%s/> @ line %d", Tag.name(), Tag.LineNo); return ERR::NoSupport;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// The Width/Height can be zero if the original image dimensions are desired.

static ERR load_pic(extSVG *Self, std::string Path, objPicture **Picture, double Width = 0, double Height = 0)
{
   pf::Log log(__FUNCTION__);

   *Picture = nullptr;
   objFile *file = nullptr;
   auto val = Path.c_str();

   ERR error = ERR::Okay;
   if (startswith("icons:", val)) {
      // Kotuku feature: Load an SVG image from the icon database.  Nothing needs to be done here
      // because the fielsystem volume is built-in.
   }
   else if (startswith("data:", val)) { // Check for embedded content
      log.branch("Detected embedded source data");
      val += 5;
      if (startswith("image/", val)) { // Has to be an image type
         val += 6;
         while ((*val) and (*val != ';')) val++;
         if (startswith(";base64", val)) { // Is it base 64?
            val += 7;
            while ((*val) and (*val != ',')) val++;
            if (*val IS ',') val++;

            pf::BASE64DECODE state;
            clearmem(&state, sizeof(state));

            uint8_t *output;
            int size = strlen(val);
            if (AllocMemory(size, MEM::DATA|MEM::NO_CLEAR, &output) IS ERR::Okay) {
               int written;
               if ((error = pf::Base64Decode(&state, val, size, output, &written)) IS ERR::Okay) {
                  Path = "temp:svg.img";
                  if ((file = objFile::create::local(fl::Path(Path), fl::Flags(FL::NEW|FL::WRITE)))) {
                     int result;
                     file->write(output, written, &result);
                  }
                  else error = ERR::File;
               }

               FreeResource(output);
            }
            else error = ERR::AllocMemory;
         }
         else error = ERR::StringFormat;
      }
      else error = ERR::StringFormat;
   }
   else log.branch("%s", Path.c_str());

   if (error IS ERR::Okay) {
      if (!(*Picture = objPicture::create::global(
         fl::Owner(Self->Scene->UID),
         fl::Path(Path),
         fl::BitsPerPixel(32),
         fl::DisplayWidth(Width), fl::DisplayHeight(Height),
         fl::Flags(PCF::FORCE_ALPHA_32)))) error = ERR::CreateObject;
   }

   if (file) {
      Action(fl::Delete::id, file, nullptr);
      FreeResource(file);
   }

   if (error != ERR::Okay) log.warning(error);
   return error;
}

//********************************************************************************************************************
// Definition images are stored once, allowing them to be used multiple times via Fill and Stroke references.

void svgState::proc_def_image(XTag &Tag) noexcept
{
   pf::Log log(__FUNCTION__);
   objVectorImage *image;
   std::string id, src;
   FUNIT width, height;

   if (NewObject(CLASSID::VECTORIMAGE, &image) IS ERR::Okay) {
      image->setFields(fl::Owner(Self->Scene->UID),
         fl::Name("SVGImage"),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::SpreadMethod(VSPREAD::PAD));

      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         switch(strihash(Tag.Attribs[a].Name)) {
            case SVF_UNITS:
               if (iequals("userSpaceOnUse", val)) image->Units = VUNIT::USERSPACE;
               else if (iequals("objectBoundingBox", val)) image->Units = VUNIT::BOUNDING_BOX;
               else log.warning("Unknown <image> units reference '%s'", val.c_str());
               break;

            case SVF_XLINK_HREF: src = val; break;
            case SVF_ID:     id = val; break;
            // Applying (x,y) values as a texture offset here appears to be a mistake because <use> will deep-clone
            // the values also.  SVG documentation is silent on the validity of (x,y) values when an image
            // is in the <defs> area, so a W3C test may be needed to settle the matter.
            case SVF_X:      /*UNIT(FID_X, val).set(image);*/ break;
            case SVF_Y:      /*UNIT(FID_Y, val).set(image);*/ break;
            case SVF_WIDTH:  width = UNIT(val); break;
            case SVF_HEIGHT: height = UNIT(val); break;
            default: {
               // Check if this was a reference to some other namespace (ignorable).
               int i;
               for (i=0; val[i] and (val[i] != ':'); i++);
               if (val[i] != ':') log.warning("Failed to parse attrib '%s' in <image/> tag @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
               break;
            }
         }
      }

      if ((!id.empty()) and (!src.empty())) {
         objPicture *pic;
         if (load_pic(Self, src, &pic, width, height) IS ERR::Okay) {
            image->set(FID_Picture, pic);
            if (InitObject(image) IS ERR::Okay) {
               if (!Self->Cloning) {
                  Self->Scene->addDef(id.c_str(), image);
                  track_object(Self, image);
               }
            }
            else {
               FreeResource(image);
               log.trace("Picture initialisation failed.");
            }
         }
         else {
            FreeResource(image);
            log.trace("Unable to load a picture for <image/> '%s' at line %d", id.c_str(), Tag.LineNo);
         }
      }
      else {
         FreeResource(image);
         log.trace("No id or src specified in <image/> at line %d", Tag.LineNo);
      }
   }
}

//********************************************************************************************************************

ERR svgState::proc_image(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   pf::Log log(__FUNCTION__);

   std::string src, filter, transform, id;
   ARF ratio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // SVG default if the client leaves preserveAspectRatio undefined
   FUNIT x, y, width, height;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      switch (strihash(Tag.Attribs[a].Name)) {
         case SVF_XLINK_HREF:
         case SVF_HREF:
            src = value;
            break;
         case SVF_ID: id = value; break;
         case SVF_TRANSFORM: transform = value; break;
         case SVF_PRESERVEASPECTRATIO: ratio = parse_aspect_ratio(value); break;
         case SVF_X: x = UNIT(FID_X, value); break;
         case SVF_Y: y = UNIT(FID_Y, value); break;
         case SVF_WIDTH:
            width = UNIT(FID_Width, value);
            if (!width.valid_size()) return log.warning(ERR::InvalidDimension);
            break;
         case SVF_HEIGHT:
            height = UNIT(FID_Height, value);
            if (!height.valid_size()) return log.warning(ERR::InvalidDimension);
            break;
         case SVF_CROSSORIGIN: break; // Defines the value of the credentials flag for CORS requests.
         case SVF_DECODING: break; // Hint as to whether image decoding is synchronous or asynchronous
         case SVF_CLIP: break; // Deprecated from SVG; allows a rect() to be declared that functions as a clip-path
      }
   }

   if (src.empty()) return ERR::FieldNotSet;

   if (id.empty()) {
      // An image always has an ID; this ensures that if the image bitmap is referenced repeatedly via a <symbol> then
      // we won't keep reloading it into the cache.
      id = "img_" + std::to_string(strihash(src));
      if (!width.empty()) id += "_" + std::to_string(width);
      if (!height.empty()) id += "_" + std::to_string(height);
      xml::NewAttrib(Tag, "id", id);
   }

   if (Self->Scene->findDef(id.c_str(), nullptr) != ERR::Okay) {
      // Load the image and add it to the vector definition.  It will be rendered as a rectangle within the scene.
      // This may appear a little confusing because an image can be invoked in SVG like a first-class shape; however to
      // do so would be inconsistent with all other scene graph members being true path-based objects.

      objPicture *pic = nullptr;
      load_pic(Self, src, &pic, width, height);

      if (pic) {
         if (auto image = objVectorImage::create::global(
               fl::Owner(Self->Scene->UID),
               fl::Picture(pic),
               fl::Units(VUNIT::BOUNDING_BOX),
               fl::AspectRatio(ratio))) {

            SetOwner(pic, image); // It's best if the pic belongs to the image.

            Self->Scene->addDef(id.c_str(), image);
            track_object(Self, image);
         }
         else return ERR::CreateObject;
      }
      else log.warning("Failed to load picture via xlink:href.");
   }

   // NOTE: Officially, the SVG standard requires that a viewport is created to host the image (this would still
   // require a filled rectangle to reference the image).  In practice this doesn't seem necessary, as the image
   // object supports built-in viewport concepts like preserveAspectRatio.

   if (auto error = NewObject(CLASSID::VECTORRECTANGLE, &Vector); error IS ERR::Okay) {
      SetOwner(Vector, Parent);
      applyStateToVector(Vector);

      // All attributes of <image> will be applied to the rectangle.

      process_attrib(Tag, Vector);

      if (!x.empty()) x.set(Vector);
      if (!y.empty()) y.set(Vector);
      if (!width.empty()) width.set(Vector);
      if (!height.empty()) height.set(Vector);

      Vector->set(FID_Fill, "url(#" + id + ")");

      if (Vector->init() IS ERR::Okay) {
         process_shape_children(Tag, Vector);
         Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(Vector->UID) });
         return ERR::Okay;
      }
      else {
         FreeResource(Vector);
         return ERR::Init;
      }
   }
   else return ERR::CreateObject;
}

//********************************************************************************************************************

ERR svgState::proc_defs(XTag &Tag, OBJECTPTR Parent) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   for (auto &child : Tag.Children) {
      switch (strihash(child.name())) {
         case SVF_CONTOURGRADIENT: state.proc_contourgradient(child); break;
         case SVF_RADIALGRADIENT:  state.proc_radialgradient(child); break;
         case SVF_DIAMONDGRADIENT: state.proc_diamondgradient(child); break;
         case SVF_CONICGRADIENT:   state.proc_conicgradient(child); break;
         case SVF_LINEARGRADIENT:  state.proc_lineargradient(child); break;
         case SVF_PATTERN:         state.proc_pattern(child); break;
         case SVF_IMAGE:           state.proc_def_image(child); break;
         case SVF_FILTER:          state.proc_filter(child); break;
         case SVF_CLIPPATH:        state.proc_clippath(child); break;
         case SVF_MASK:            state.proc_mask(child); break;
         case SVF_KOTUKU_TRANSITION: state.proc_pathtransition(child); break;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR svgState::proc_style(XTag &Tag)
{
   pf::Log log(__FUNCTION__);
   ERR error = ERR::Okay;

   if (!Self->XML) {
      // The application of CSS styles is possible on initial loading of the document, but not in post-processing
      // once the XML object has been abandoned.
      log.warning("Unable to apply CSS style-sheet, XML object already terminated.");
      return ERR::Failed;
   }

   for (auto &a : Tag.Attribs) {
      if (iequals("type", a.Name)) {
         if (!iequals("text/css", a.Value)) {
            log.warning("Unsupported stylesheet '%s'", a.Value.c_str());
            return ERR::NoSupport;
         }
         break;
      }
   }

   // Parse the CSS using the Katana Parser.

   auto css_buffer = Tag.getContent();
   if (auto css = katana_parse(css_buffer.c_str(), css_buffer.size(), KatanaParserModeStylesheet)) {
      /*#ifdef _DEBUG
         Self->CSS->mode = KatanaParserModeStylesheet;
         katana_dump_output(css);
      #endif*/

      // For each rule in the stylesheet, apply them to the loaded XML document by injecting tags and attributes.
      // The stylesheet attributes have precedence over inline tag attributes (therefore we can overwrite matching
      // attribute names) however they are outranked by inline styles.

      KatanaStylesheet *sheet = css->stylesheet;

      log.msg("%d CSS rules will be applied", sheet->imports.length + sheet->rules.length);

      for (unsigned i=0; i < sheet->imports.length; ++i) {
         if (sheet->imports.data[i])
            process_rule(Self, Self->XML->Tags, (KatanaRule *)sheet->imports.data[i]);
      }

      for (unsigned i=0; i < sheet->rules.length; ++i) {
         if (sheet->rules.data[i])
            process_rule(Self, Self->XML->Tags, (KatanaRule *)sheet->rules.data[i]);
      }

      katana_destroy_output(css);
   }

   return error;
}

//********************************************************************************************************************
// Declare a 'symbol' which is basically a template for inclusion elsewhere through the use of a 'use' element.
//
// When a use element is encountered, it looks for the associated symbol ID and then processes the XML child tags that
// belong to it.

void svgState::proc_symbol(XTag &Tag) noexcept
{
   // Symbols are ignored at these stage as they will already have been parsed for their 'id' reference.
}

//********************************************************************************************************************
// Most vector shapes can be morphed to the path of another vector.

void svgState::proc_morph(XTag &Tag, OBJECTPTR Parent) noexcept
{
   pf::Log log(__FUNCTION__);

   if ((!Parent) or (Parent->Class->BaseClassID != CLASSID::VECTOR)) {
      log.traceWarning("Unable to apply morph to non-vector parent object.");
      return;
   }

   // Find the definition that is being referenced for the morph.

   std::string offset;
   std::string ref;
   std::string transition;
   VMF flags = VMF::NIL;
   ARF align = ARF::NIL;
   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_PATH:
         case SVF_XLINK_HREF:  ref = val; break;
         case SVF_TRANSITION:  transition = val; break;
         case SVF_STARTOFFSET: offset = val; break;
         case SVF_METHOD:
            if (iequals("align", val)) flags &= ~VMF::STRETCH;
            else if (iequals("stretch", val)) flags |= VMF::STRETCH;
            break;

         case SVF_SPACING:
            if (iequals("auto", val)) flags |= VMF::AUTO_SPACING;
            else if (iequals("exact", val)) flags &= ~VMF::AUTO_SPACING;
            break;

         case SVF_ALIGN:
            align |= parse_aspect_ratio(val);
            break;
      }
   }

   if (ref.empty()) {
      log.warning("<morph> element @ line %d is missing a valid xlink:href attribute.", Tag.LineNo);
      return;
   }

   // Find the matching element with matching ID

   auto uri = uri_name(ref);
   if (uri.empty()) {
      log.warning("Invalid URI string '%s' at line %d", ref.c_str(), Tag.LineNo);
      return;
   }

   if (!Self->IDs.contains(uri)) {
      log.warning("Unable to find element '%s' referenced at line %d", ref.c_str(), Tag.LineNo);
      return;
   }

   OBJECTPTR transvector = nullptr;
   if (!transition.empty()) {
      if (Self->Scene->findDef(transition.c_str(), &transvector) != ERR::Okay) {
         log.warning("Unable to find element '%s' referenced at line %d", transition.c_str(), Tag.LineNo);
         return;
      }
   }

   auto tagref = Self->IDs[uri];

   auto class_id = CLASSID::NIL;
   switch (strihash(tagref->name())) {
      case SVF_PATH:          class_id = CLASSID::VECTORPATH; break;
      case SVF_RECT:          class_id = CLASSID::VECTORRECTANGLE; break;
      case SVF_ELLIPSE:       class_id = CLASSID::VECTORELLIPSE; break;
      case SVF_CIRCLE:        class_id = CLASSID::VECTORELLIPSE; break;
      case SVF_POLYGON:       class_id = CLASSID::VECTORPOLYGON; break;
      case SVF_KOTUKU_SPIRAL: class_id = CLASSID::VECTORSPIRAL; break;
      case SVF_KOTUKU_WAVE:   class_id = CLASSID::VECTORWAVE; break;
      case SVF_KOTUKU_SHAPE:  class_id = CLASSID::VECTORSHAPE; break;
      default:
         log.warning("Invalid reference '%s', '%s' is not recognised by <morph>.", ref.c_str(), tagref->name());
   }

   if ((flags & (VMF::Y_MIN|VMF::Y_MID|VMF::Y_MAX)) IS VMF::NIL) {
      if (Parent->classID() IS CLASSID::VECTORTEXT) flags |= VMF::Y_MIN;
      else flags |= VMF::Y_MID;
   }

   if (class_id != CLASSID::NIL) {
      objVector *shape;
      svgState state(Self);
      state.proc_shape(class_id, *tagref, Self->Scene, shape);
      Parent->set(FID_Morph, shape);
      if (transvector) Parent->set(FID_Transition, transvector);
      Parent->set(FID_MorphFlags, int(flags));
      if (!Self->Cloning) Self->Scene->addDef(uri.c_str(), shape);
   }
}

//********************************************************************************************************************
// Duplicates a referenced area of the SVG definition.
//
// "The effect of a 'use' element is as if the contents of the referenced element were deeply cloned into a separate
// non-exposed DOM tree which had the 'use' element as its parent and all of the 'use' element's ancestors as its
// higher-level ancestors.

void svgState::proc_use(XTag &Tag, OBJECTPTR Parent) noexcept
{
   pf::Log log(__FUNCTION__);

   std::string ref;
   for (int a=1; (a < std::ssize(Tag.Attribs)) and ref.empty(); a++) {
      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_HREF: // SVG2
         case SVF_XLINK_HREF: ref = Tag.Attribs[a].Value; break;
      }
   }

   if (ref.empty()) {
      log.warning("<use> element @ line %d is missing a valid href attribute.", Tag.LineNo);
      return;
   }

   // Find the matching element with matching ID

   auto tagref = find_href_tag(Self, ref);
   if (!tagref) {
      log.warning("Unable to find element '%s'", ref.c_str());
      return;
   }

   // Increment the Cloning variable to indicate that we are in a region that is being cloned.
   // This is important for some elements like clip-path, whereby the path only needs to be created
   // once and can then be referenced multiple times.

   Self->Cloning++;
   auto dc = deferred_call([this] {
      Self->Cloning--;
   });

   if ((iequals("symbol", tagref->name())) or (iequals("svg", tagref->name()))) {
      // SVG spec requires that we create a VectorGroup and then create a Viewport underneath that.  However if there
      // are no attributes to apply to the group then there is no sense in creating an empty one.

      // TODO: We should be using the same replace-and-expand tag method that is applied for group
      // handling, as seen further below in this routine.

      objVector *viewport = nullptr;

      auto state = *this;
      state.applyTag(Tag); // Apply all attribute values to the current state.

      objVector *group;
      bool need_group = false;
      for (int a=1; (a < std::ssize(Tag.Attribs)) and (!need_group); a++) {
         switch(strihash(Tag.Attribs[a].Name)) {
            case SVF_X: case SVF_Y: case SVF_WIDTH: case SVF_HEIGHT: break;
            default: need_group = true; break;
         }
      }

      if (need_group) {
         if (NewObject(CLASSID::VECTORGROUP, &group) IS ERR::Okay) {
            SetOwner(group, Parent);
            Parent = group;
            group->init();
         }
      }

      if (NewObject(CLASSID::VECTORVIEWPORT, &viewport) != ERR::Okay) return;
      SetOwner(viewport, Parent);
      viewport->setFields(fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0))); // SVG default

      // Apply attributes from 'use' to the group and/or viewport
      for (int a=1; a < std::ssize(Tag.Attribs); a++) {
         auto &val = Tag.Attribs[a].Value;
         if (val.empty()) continue;

         auto hash = strihash(Tag.Attribs[a].Name);
         switch(hash) {
            // X,Y,Width,Height are applied to the viewport
            case SVF_X:      UNIT(FID_X, val).set(viewport); break;
            case SVF_Y:      UNIT(FID_Y, val).set(viewport); break;
            case SVF_WIDTH:  UNIT(FID_Width, val).set(viewport); break;
            case SVF_HEIGHT: UNIT(FID_Height, val).set(viewport); break;

            // All other attributes are applied to the 'g' element
            default:
               if (group) state.set_property(group, hash, Tag, val);
               else state.set_property(viewport, hash, Tag, val);
               break;
         }
      }

      // Apply attributes from the symbol itself to the viewport

      for (int a=1; a < std::ssize(tagref->Attribs); a++) {
         auto &val = tagref->Attribs[a].Value;
         if (val.empty()) continue;

         switch(strihash(tagref->Attribs[a].Name)) {
            case SVF_X:      UNIT(FID_X, val).set(viewport); break;
            case SVF_Y:      UNIT(FID_Y, val).set(viewport); break;
            case SVF_WIDTH:  UNIT(FID_Width, val).set(viewport); break;
            case SVF_HEIGHT: UNIT(FID_Height, val).set(viewport); break;
            case SVF_VIEWBOX:  {
               double x=0, y=0, width=0, height=0;
               read_numseq(val, { &x, &y, &width, &height });
               viewport->setFields(fl::ViewX(x), fl::ViewY(y), fl::ViewWidth(width), fl::ViewHeight(height));
               break;
            }
            case SVF_ID: break; // Ignore (already processed).
            default: log.warning("Not processing attribute '%s'", tagref->Attribs[a].Name.c_str()); break;
         }
      }

      if (viewport->init() != ERR::Okay) { FreeResource(viewport); return; }

      // Add all child elements in <symbol> to the viewport.  Some state values have to be reset here because
      // they have already been applied to the viewport and will be inherited via that route.

      state.m_opacity = 1.0;

      log.traceBranch("Processing all child elements within %s", ref.c_str());
      state.process_children(*tagref, viewport);
   }
   else {
      // W3C: In the generated content, the 'use' will be replaced by 'g', where all attributes from the 'use' element
      // except for 'x', 'y', 'width', 'height' and 'xlink:href' are transferred to the generated 'g' element. An
      // additional transformation translate(x,y) is appended to the end (i.e., right-side) of the 'transform'
      // attribute on the generated 'g', where x and y represent the values of the 'x' and 'y' attributes on the
      // 'use' element. The referenced object and its contents are deep-cloned into the generated tree.

      auto use_attribs = Tag.Attribs;
      Tag.Attribs[0].Name = "g";
      if (Tag.Attribs.size() > 1) Tag.Attribs.erase(Tag.Attribs.begin()+1, Tag.Attribs.end());

      if (Tag.Children.empty()) {
         Tag.Children = { *tagref }; // Deep-clone

         // Apply 'use' attributes, making a special case for 'x' and 'y'.

         FUNIT tx, ty;
         for (int t=1; t < std::ssize(use_attribs); t++) {
            switch (strihash(use_attribs[t].Name)) {
               case SVF_X: tx = UNIT(FID_X, use_attribs[t].Value); break;
               case SVF_Y: ty = UNIT(FID_Y, use_attribs[t].Value); break;
               // SVG states that the following are not to be applied to the group...
               case SVF_WIDTH:
               case SVF_HEIGHT:
               case SVF_XLINK_HREF:
               case SVF_HREF:
                  break;

               default:
                  Tag.Attribs.push_back(use_attribs[t]);
            }
         }

         if ((!tx.empty()) or (!ty.empty())) {
            Tag.Attribs.push_back({ "transform", "translate(" + std::to_string(tx) + " " + std::to_string(ty) + ")" });
         }
      }
      else {
         // The SVG documentation appears to be silent on the matter of children in the <use> element.  So far we've
         // encountered animate instructions in the <use> area, and this expands into a complicated dual-group configuration
         // (at least that appears to be the only way we can get our animations to match expected behaviour patterns in W3C
         // tests).

         auto &subgroup = Tag.Children.emplace_back(XTag(0));
         subgroup.Attribs.push_back(XMLAttrib("g"));
         subgroup.Children.push_back(*tagref);

         FUNIT tx, ty;
         for (int t=1; t < std::ssize(use_attribs); t++) {
            switch (strihash(use_attribs[t].Name)) {
               case SVF_X: tx = UNIT(FID_X, use_attribs[t].Value); break;
               case SVF_Y: ty = UNIT(FID_Y, use_attribs[t].Value); break;
               case SVF_WIDTH:
               case SVF_HEIGHT:
               case SVF_XLINK_HREF:
               case SVF_HREF:
                  break;

               default:
                  subgroup.Attribs.push_back(use_attribs[t]);
            }
         }

         if ((!tx.empty()) or (!ty.empty())) {
            subgroup.Attribs.push_back({ "transform", "translate(" + std::to_string(tx) + " " + std::to_string(ty) + ")" });
         }
      }

      objVector *new_group = nullptr;
      proc_group(Tag, Parent, new_group);
   }
}

//********************************************************************************************************************
// "a href" link support.  The child vectors belonging to the link must be monitored for click events.
// https://www.w3.org/TR/SVG2/linking.html

static ERR link_event(objVector *Vector, const InputEvent *Events, svgLink *Link)
{
   pf::Log log(__FUNCTION__);

   auto Self = (extSVG *)CurrentContext();

   for (auto event=Events; event; event=event->Next) {
      if ((event->Type IS JET::LMB) and ((event->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
         if (!event->Value) continue;

         if (Link->ref.starts_with('#')) {
            // The link activates a document node, like an animation.
            if (find_href_tag(Self, Link->ref)) {
               for (auto &record : Self->Animations) {
                  std::visit([ Link, Self ](auto &&anim) {
                     if (anim.id IS Link->ref.substr(1)) anim.activate(true);
                  }, record);
               }
            }
            else log.warning("Unknown reference '%s'", Link->ref.c_str());
         }
         else {
            // The link is a URI that could refer to an HTTP location, local file, etc...
            log.warning("URI links are not yet supported.");
         }
      }
   }

   return ERR::Okay;
}

void svgState::proc_link(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   auto link = std::make_unique<svgLink>();

   if (Tag.Children.empty()) return;

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         case SVF_HREF:
         case SVF_XLINK_HREF:
            link.get()->ref = val;
            break;
         // SVF_TARGET: _self, _parent, _top, _blank
         // SVF_DOWNLOAD, SVF_PING, SVF_REL, SVF_HREFLANG, SVF_TYPE, SVF_REFERRERPOLICY
      }
   }

   // Vectors within <a> will be assigned to a group purely because it's easier for us to manage them that way.

   objVector *group;
   if (NewObject(CLASSID::VECTORGROUP, &group) IS ERR::Okay) {
      SetOwner(group, Parent);

      process_children(Tag, group);

      pf::vector<ChildEntry> list;
      if (ListChildren(group->UID, &list) IS ERR::Okay) {
         for (auto &child : list) {
            auto obj = (objVector *)GetObjectPtr(child.ObjectID);
            obj->subscribeInput(JTYPE::BUTTON, C_FUNCTION(link_event, link.get()));
         }
         Self->Links.emplace_back(std::move(link));
      }

      if ((!Self->Links.empty()) and (group->init() IS ERR::Okay)) Vector = group;
      else FreeResource(group);
   }
}

//********************************************************************************************************************
// For each immediate child, evaluate any requiredFeatures, requiredExtensions and systemLanguage attributes.
// The first child where these attributes evaluate to true is rendered, the rest are ignored.

void svgState::proc_switch(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   for (auto &child : Tag.Children) {
      bool render = true;
      for (int a=1; a < std::ssize(child.Attribs); a++) {
         auto &val = child.Attribs[a].Value;
         if (val.empty()) continue;

         switch(strihash(child.Attribs[a].Name)) {
            case SVF_REQUIREDFEATURES:
               // requiredFeatures is deprecated as of SVG2, but the following features are explicitly not supported
               // by our SVG implementation:
               if (val IS "http://www.w3.org/TR/SVG11/feature#SVG-dynamic") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#DocumentEventsAttribute") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#GraphicalEventsAttribute") render = false;
               else if (val IS "http://www.w3.org/TR/SVG11/feature#Script") render = false;
               break;

            case SVF_REQUIREDEXTENSIONS: // Allows the client to check if a given customised extension is supported.
               if (val IS "http://www.kotuku.dev/TR/Kotuku/1.0");
               else render = false;
               break;

            case SVF_SYSTEMLANGUAGE: // CSV list of language codes from BCP47
               // GetLocaleInfoEx() LOCALE_SISO639LANGNAME
               break;
         }
      }

      if (render) {
         if (Tag.Attribs.size() > 1) { // The <switch> element is treated as a container type
            pf::Log log(__FUNCTION__);
            log.traceBranch("Tag: %d", Tag.ID);

            auto state = *this;

            objVector *group;
            if (NewObject(CLASSID::VECTORGROUP, &group) != ERR::Okay) return;
            SetOwner(group, Parent);
            state.applyTag(Tag);
            state.process_attrib(Tag, group);

            if (group->init() IS ERR::Okay) {
               Vector = group;
               Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(group->UID) });

               state.process_tag(child, Tag, group, Vector);
            }
            else FreeResource(group);
         }
         else process_tag(child, Tag, Parent, Vector);

         return;
      }
   }
}

//********************************************************************************************************************

void svgState::proc_group(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Tag: %d", Tag.ID);

   auto state = *this;

   objVector *group;
   if (NewObject(CLASSID::VECTORGROUP, &group) != ERR::Okay) return;
   SetOwner(group, Parent);
   state.applyTag(Tag); // Apply all group attribute values to the current state.
   state.process_attrib(Tag, group);
   state.process_children(Tag, group);

   if (group->init() IS ERR::Okay) {
      Vector = group;
      Tag.Attribs.push_back(XMLAttrib { "_id", std::to_string(group->UID) });
   }
   else FreeResource(group);
}

//********************************************************************************************************************
// <svg/> tags can be embedded inside <svg/> tags - this establishes a new viewport.
// Refer to section 7.9 of the SVG Specification for more information.

void svgState::proc_svg(XTag &Tag, OBJECTPTR Parent, objVector * &Vector) noexcept
{
   pf::Log log(__FUNCTION__);
   int a;

   if (!Parent) {
      log.warning("A Parent object is required.");
      return;
   }

   // If initialising to a VectorScene, prefer to use its existing viewport if there is one.

   objVectorViewport *viewport;
   if ((Parent->classID() IS CLASSID::VECTORSCENE) and (((objVectorScene *)Parent)->Viewport)) {
      viewport = ((objVectorScene *)Parent)->Viewport;
   }
   else {
      if (NewObject(CLASSID::VECTORVIEWPORT, &viewport) != ERR::Okay) return;
      SetOwner(viewport, Parent);
   }

   // The first viewport to be instantiated is stored as a local reference.  This is important if the developer has
   // specified a custom target, in which case there needs to be a way to discover the root of the SVG.

   if (!Self->Viewport) Self->Viewport = viewport;

   // Process <svg> attributes

   auto state = *this;
   state.applyTag(Tag); // Apply all attribute values to the current state.

   for (a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &val = Tag.Attribs[a].Value;
      if (val.empty()) continue;

      switch(strihash(Tag.Attribs[a].Name)) {
         // The viewbox determines what area of the vector definition is to be displayed (in a sense, zooming into the document).
         // The individual x, y, width and height values determine the position and clipping of the displayed SVG content.

         case SVF_VIEWBOX:  {
            auto dim = read_array(val);
            if (dim.size() >= 4) viewport->setFields(fl::ViewX(dim[0]), fl::ViewY(dim[1]), fl::ViewWidth(dim[2]), fl::ViewHeight(dim[3]));
            break;
         }

         case SVF_VERSION: {
            double version = strtod(val.c_str(), nullptr);
            if (version > Self->SVGVersion) Self->SVGVersion = version;
            break;
         }

         case SVF_X: UNIT(FID_X, val).set(viewport); break;
         case SVF_Y: UNIT(FID_Y, val).set(viewport); break;

         case SVF_XOFFSET: UNIT(FID_XOffset, val).set(viewport); break;
         case SVF_YOFFSET: UNIT(FID_YOffset, val).set(viewport); break;

         case SVF_WIDTH:
            UNIT(FID_Width, val).set(viewport);
            viewport->set(FID_OverflowX, int(VOF::HIDDEN));
            break;

         case SVF_HEIGHT:
            UNIT(FID_Height, val).set(viewport);
            viewport->set(FID_OverflowY, int(VOF::HIDDEN));
            break;

         case SVF_PRESERVEASPECTRATIO:
            viewport->set(FID_AspectRatio, int(parse_aspect_ratio(val)));
            break;

         case SVF_ID:
            viewport->set(FID_ID, val);
            SetName(viewport, val.c_str());
            break;

         case SVF_ENABLE_BACKGROUND: // Deprecated in favour of 'isolated'
            log.warning("enable-background is deprecated in favour of the isolated attribute.");
            if (iequals("new", val)) viewport->setFlags(Vector->Flags | VF::ISOLATED);
            break;

         case SVF_ISOLATION_MODE:
            if (iequals("isolated", val)) viewport->setFlags(Vector->Flags | VF::ISOLATED);
            break;

         case SVF_ZOOMANDPAN:
            if (iequals("magnify", val)) {
               // This option indicates that the scene graph should be scaled to match the size of the client's
               // viewing window.
               log.warning("zoomAndPan not yet supported.");
            }
            break;

         case SVF_XMLNS: break; // Ignored
         case SVF_BASEPROFILE: break; // The minimum required SVG standard that is required for rendering the document.

         case SVF_MASK: {
            OBJECTPTR clip;
            if (Self->Scene->findDef(val.c_str(), &clip) IS ERR::Okay) viewport->set(FID_Mask, clip);
            else log.warning("Unable to find mask '%s'", val.c_str());
            break;
         }

         case SVF_CLIP_PATH: {
            OBJECTPTR clip;
            if (Self->Scene->findDef(val.c_str(), &clip) IS ERR::Okay) viewport->set(FID_Mask, clip);
            else log.warning("Unable to find clip-path '%s'", val.c_str());
            break;
         }

         // default - The browser will remove all newline characters. Then it will convert all tab characters into
         // space characters. Then, it will strip off all leading and trailing space characters. Then, all contiguous
         // space characters will be consolidated.
         //
         // preserve - The browser will will convert all newline and tab characters into space characters. Then, it
         // will draw all space characters, including leading, trailing and multiple contiguous space characters. Thus,
         // when drawn with xml:space="preserve", the string "a   b" (three spaces between "a" and "b") will produce a
         // larger separation between "a" and "b" than "a b" (one space between "a" and "b").

         case SVF_XML_SPACE:
            if (iequals("preserve", val)) Self->PreserveWS = TRUE;
            else Self->PreserveWS = FALSE;
            break;

         default: {
            // Print a warning unless this was a reference to some other namespace.
            if (val.find(':') IS std::string::npos) {
               log.warning("Failed to parse attrib '%s' in <svg/> tag @ line %d", Tag.Attribs[a].Name.c_str(), Tag.LineNo);
            }
         }
      }
   }

   // Process child tags

   objVector *sibling = nullptr;
   for (auto &child : Tag.Children) {
      if (child.isTag()) {
         log.traceBranch("Processing <%s/>", child.name());

         switch(strihash(child.name())) {
            case SVF_DEFS: state.proc_defs(child, viewport); break;
            default:       state.process_tag(child, Tag, viewport, sibling);  break;
         }
      }
   }

   if (viewport->initialised()) Vector = viewport;
   else if (viewport->init() IS ERR::Okay) Vector = viewport;
   else FreeResource(viewport);
}

//********************************************************************************************************************
// <animateTransform attributeType="XML" attributeName="transform" type="rotate" from="0,150,150" to="360,150,150"
//   begin="0s" dur="5s" repeatCount="indefinite"/>

ERR svgState::proc_animate_transform(XTag &Tag, OBJECTPTR Parent) noexcept
{
   pf::Log log(__FUNCTION__);

   auto &new_anim = Self->Animations.emplace_back(anim_transform { Self, Parent->UID });
   auto &anim = std::get<anim_transform>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strihash(Tag.Attribs[a].Name);
      switch (hash) {
         case SVF_TYPE: // translate, scale, rotate, skewX, skewY
            if (iequals("translate", value))   anim.type = AT::TRANSLATE;
            else if (iequals("scale", value))  anim.type = AT::SCALE;
            else if (iequals("rotate", value)) anim.type = AT::ROTATE;
            else if (iequals("skewX", value))  anim.type = AT::SKEW_X;
            else if (iequals("skewY", value))  anim.type = AT::SKEW_Y;
            else log.warning("Unsupported type '%s'", value.c_str());
            break;

         default:
            set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!anim.is_valid()) Self->Animations.pop_back();
   return ERR::Okay;
}

//********************************************************************************************************************
// The 'animate' element is used to animate a single attribute or property over time. For example, to make a
// rectangle repeatedly fade away over 5 seconds:

ERR svgState::proc_animate(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   pf::Log log(__FUNCTION__);

   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strihash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   anim.set_orig_value(*this);

   if (!anim.is_valid()) Self->Animations.pop_back();
   return ERR::Okay;
}

//********************************************************************************************************************
// <set> is largely equivalent to <animate> but does not interpolate values.

ERR svgState::proc_set(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strihash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   anim.set_orig_value(*this);
   anim.calc_mode = CMODE::DISCRETE; // Disables interpolation

   if (!anim.is_valid()) Self->Animations.pop_back();
   return ERR::Okay;
}

//********************************************************************************************************************
// The <animateColour> tag is considered deprecated because its functionality can be represented entirely by the
// existing <animate> tag.

ERR svgState::proc_animate_colour(XTag &Tag, XTag &ParentTag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_value { Self, Parent->UID, &ParentTag });
   auto &anim = std::get<anim_value>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strihash(Tag.Attribs[a].Name);
      switch (hash) {
         default:
            if (value == "currentColor") {
               set_anim_property(anim, Tag, hash, m_color);
            }
            else set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!anim.is_valid()) Self->Animations.pop_back();
   return ERR::Okay;
}

//********************************************************************************************************************
// <animateMotion from="0,0" to="100,100" dur="4s" fill="freeze"/>

ERR svgState::proc_animate_motion(XTag &Tag, OBJECTPTR Parent) noexcept
{
   auto &new_anim = Self->Animations.emplace_back(anim_motion { Self, Parent->UID });
   auto &anim = std::get<anim_motion>(new_anim);

   for (int a=1; a < std::ssize(Tag.Attribs); a++) {
      auto &value = Tag.Attribs[a].Value;
      if (value.empty()) continue;

      auto hash = strihash(Tag.Attribs[a].Name);
      switch (hash) {
         case SVF_PATH: // List of standard path commands, e.g. "M 0 0 L 100 100"
            anim.path.set(objVectorPath::create::global(
                fl::Name("motion_path"),
                fl::Owner(Self->Scene->UID),
                fl::Sequence(value),
                fl::Visibility(VIS::HIDDEN)));
            break;

         case SVF_ROTATE:
            // Post-multiplies a supplemental transformation matrix onto the CTM of the target element to apply a
            // rotation transformation about the origin of the current user coordinate system. The rotation
            // transformation is applied after the supplemental translation transformation that is computed due to
            // the 'path' attribute.
            //
            // auto: The object is rotated over time by the angle of the direction (i.e., directional tangent
            // vector) of the motion path.
            //
            // auto-reverse: Indicates that the object is rotated over time by the angle of the direction (i.e.,
            // directional tangent vector) of the motion path plus 180 degrees.
            //
            // <number>: Indicates that the target element has a constant rotation transformation applied to it,
            // where the rotation angle is the specified number of degrees.

            if (iequals("auto", value)) anim.auto_rotate = ART::AUTO;
            else if (iequals("auto-reverse", value)) anim.auto_rotate = ART::AUTO_REVERSE;
            else {
               anim.auto_rotate = ART::FIXED;
               anim.rotate = strtod(value.c_str(), nullptr);
            }
            break;

         case SVF_ORIGIN: break; // Officially serves no purpose.

         default:
            set_anim_property(anim, Tag, hash, value);
            break;
      }
   }

   if (!Tag.Children.empty()) {
      // Search for mpath references, e.g. <mpath xlink:href="#mpathRef"/>

      for (auto &child : Tag.Children) {
         if ((child.isTag()) and (iequals("mpath", child.name()))) {
            auto href = child.attrib("xlink:href");
            if (!href) child.attrib("href");

            if (href) {
               objVector *path;
               if (Self->Scene->findDef(href->c_str(), (OBJECTPTR *)&path) IS ERR::Okay) {
                  anim.mpath = path;
               }
            }
         }
      }
   }

   if (!anim.is_valid()) Self->Animations.pop_back();
   return ERR::Okay;
}

//********************************************************************************************************************

void svgState::process_attrib(XTag &Tag, objVector *Vector) noexcept
{
   pf::Log log(__FUNCTION__);

   for (int t=1; t < std::ssize(Tag.Attribs); t++) {
      if (Tag.Attribs[t].Value.empty()) continue;
      auto &name = Tag.Attribs[t].Name;
      auto &value = Tag.Attribs[t].Value;

      if (name.find(':') != std::string::npos) continue; // Do not interpret non-SVG attributes, e.g. 'inkscape:dx'
      if (name == "_id") continue; // Ignore temporary private attribs like '_id'

      log.trace("%s = %.40s", name.c_str(), value.c_str());

      if (auto error = set_property(Vector, strihash(name), Tag, value); error != ERR::Okay) {
         if (Vector->classID() != CLASSID::VECTORGROUP) {
            log.warning("Failed to set field '%s' with '%s' in %s; Error %s",
               name.c_str(), value.c_str(), Vector->Class->ClassName, GetErrorMsg(error));
         }
      }
   }
}

//********************************************************************************************************************
// Apply all attributes in a rule to a target tag.

static void apply_rule(extSVG *Self, KatanaArray *Properties, XTag &Tag)
{
   pf::Log log(__FUNCTION__);

   for (unsigned i=0; i < Properties->length; i++) {
      auto prop = (KatanaDeclaration *)Properties->data[i];

      log.trace("Set property %s with %d values", prop->property, prop->values->length);

      for (unsigned v=0; v < prop->values->length; v++) {
         auto value = (KatanaValue *)prop->values->data[v];

         switch (value->unit) {
            case KATANA_VALUE_NUMBER:
            case KATANA_VALUE_PERCENTAGE:
            case KATANA_VALUE_EMS:
            case KATANA_VALUE_EXS:
            case KATANA_VALUE_REMS:
            case KATANA_VALUE_CHS:
            case KATANA_VALUE_PX:
            case KATANA_VALUE_CM:
            case KATANA_VALUE_DPPX:
            case KATANA_VALUE_DPI:
            case KATANA_VALUE_DPCM:
            case KATANA_VALUE_MM:
            case KATANA_VALUE_IN:
            case KATANA_VALUE_PT:
            case KATANA_VALUE_PC:
            case KATANA_VALUE_DEG:
            case KATANA_VALUE_RAD:
            case KATANA_VALUE_GRAD:
            case KATANA_VALUE_MS:
            case KATANA_VALUE_S:
            case KATANA_VALUE_HZ:
            case KATANA_VALUE_KHZ:
            case KATANA_VALUE_TURN:
               xml::UpdateAttrib(Tag, prop->property, value->raw, true);
               break;

            case KATANA_VALUE_IDENT:
               xml::UpdateAttrib(Tag, prop->property, value->string, true);
               break;

            case KATANA_VALUE_STRING:
               xml::UpdateAttrib(Tag, prop->property, value->string, true);
               break;

            case KATANA_VALUE_PARSER_FUNCTION: {
               //const char* args_str = katana_stringify_value_list(parser, value->function->args);
               //snprintf(str, sizeof(str), "%s%s)", value->function->name, args_str);
               //katana_parser_deallocate(parser, (void*) args_str);
               break;
            }

            case KATANA_VALUE_PARSER_OPERATOR: {
               char str[8];
               if (value->iValue != '=') snprintf(str, sizeof(str), " %c ", value->iValue);
               else snprintf(str, sizeof(str), " %c", value->iValue);
               xml::UpdateAttrib(Tag, prop->property, str, true);
               break;
            }

            case KATANA_VALUE_PARSER_LIST:
               //katana_stringify_value_list(parser, value->list);
               break;

            case KATANA_VALUE_PARSER_HEXCOLOR:
               xml::UpdateAttrib(Tag, prop->property, std::string("#") + value->string, true);
               break;

            case KATANA_VALUE_URI:
               xml::UpdateAttrib(Tag, prop->property, std::string("url(") + value->string + ")", true);
               break;

            default:
               log.warning("Unknown property value.");
               break;
         }
      }
   }
}

//********************************************************************************************************************
// Scan and apply all stylesheet selectors to the loaded XML document.

static void process_rule(extSVG *Self, objXML::TAGS &Tags, KatanaRule *Rule)
{
   pf::Log log(__FUNCTION__);

   if (!Rule) return;

   switch (Rule->type) {
      case KatanaRuleStyle: {
         auto sr = (KatanaStyleRule *)Rule;
         for (unsigned i=0; i < sr->selectors->length; ++i) {
            auto sel = (KatanaSelector *)sr->selectors->data[i];

            switch (sel->match) {
               case KatanaSelectorMatchTag: // Applies to all tags matching this name
                  log.trace("Processing selector: %s", (sel->tag) ? sel->tag->local : "UNNAMED");
                  for (auto &tag : Tags) {
                     if (iequals(sel->tag->local, tag.name())) {
                        apply_rule(Self, sr->declarations, tag);
                     }

                     if (!tag.Children.empty()) {
                        process_rule(Self, tag.Children, Rule);
                     }
                  }
                  break;

               case KatanaSelectorMatchId: // Applies to the first tag expressing this id
                  break;

               case KatanaSelectorMatchClass: // Requires tag to specify a class attribute
                  log.trace("Processing class selector: %s", (sel->data) ? sel->data->value : "UNNAMED");
                  for (auto &tag : Tags) {
                     for (auto &a : tag.Attribs) {
                        if (iequals("class", a.Name)) {
                           if (iequals(sel->data->value, a.Value)) {
                              apply_rule(Self, sr->declarations, tag);
                           }
                           break;
                        }
                     }

                     if (!tag.Children.empty()) {
                        process_rule(Self, tag.Children, Rule);
                     }
                  }
                  break;

               case KatanaSelectorMatchPseudoClass: // E.g. a:link
                  break;

               case KatanaSelectorMatchPseudoElement: break;
               case KatanaSelectorMatchPagePseudoClass: break;
               case KatanaSelectorMatchAttributeExact: break;
               case KatanaSelectorMatchAttributeSet: break;
               case KatanaSelectorMatchAttributeList: break;
               case KatanaSelectorMatchAttributeHyphen: break;
               case KatanaSelectorMatchAttributeContain: break;
               case KatanaSelectorMatchAttributeBegin: break;
               case KatanaSelectorMatchAttributeEnd: break;
               case KatanaSelectorMatchUnknown: break;
            }
         }

         break;
      }

      case KatanaRuleImport: //(KatanaImportRule*)rule
         log.msg("Support required for KatanaRuleImport");
         break;

      case KatanaRuleFontFace: //(KatanaFontFaceRule*)rule
         log.msg("Support required for KatanaRuleFontFace");
         break;

      case KatanaRuleKeyframes: //(KatanaKeyframesRule*)rule
         log.msg("Support required for KatanaRuleKeyframes");
         break;

      case KatanaRuleMedia: //(KatanaMediaRule*)rule
         log.msg("Support required for KatanaRuleMedia");
         break;

      case KatanaRuleUnkown:
      case KatanaRuleSupports:
      case KatanaRuleCharset:
      case KatanaRuleHost:
         break;
   }
}

//********************************************************************************************************************

ERR svgState::set_property(objVector *Vector, uint32_t Hash, XTag &Tag, const std::string StrValue) noexcept
{
   pf::Log log(__FUNCTION__);

   // Ignore stylesheet attributes
   if (Hash IS SVF_CLASS) return ERR::Okay;

   switch(Vector->classID()) {
      case CLASSID::VECTORVIEWPORT:
         switch (Hash) {
            // The following 'view-*' fields are for defining the SVG view box
            case SVF_VIEW_X:      UNIT(FID_ViewX, StrValue).set(Vector); return ERR::Okay;
            case SVF_VIEW_Y:      UNIT(FID_ViewY, StrValue).set(Vector); return ERR::Okay;
            case SVF_VIEW_WIDTH:  UNIT(FID_ViewWidth, StrValue).set(Vector); return ERR::Okay;
            case SVF_VIEW_HEIGHT: UNIT(FID_ViewHeight, StrValue).set(Vector); return ERR::Okay;
            // The following dimension fields are for defining the position and clipping of the vector display
            case SVF_X:      UNIT(FID_X, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y:      UNIT(FID_Y, StrValue).set(Vector); return ERR::Okay;
            case SVF_WIDTH:  UNIT(FID_Width, StrValue).set(Vector); return ERR::Okay;
            case SVF_HEIGHT: UNIT(FID_Height, StrValue).set(Vector); return ERR::Okay;
         }
         break;

      case CLASSID::VECTORELLIPSE:
         switch (Hash) {
            case SVF_CX: UNIT(FID_CenterX, StrValue).set(Vector); return ERR::Okay;
            case SVF_CY: UNIT(FID_CenterY, StrValue).set(Vector); return ERR::Okay;
            case SVF_R:  UNIT(FID_Radius, StrValue).set(Vector); return ERR::Okay;
            case SVF_RX: UNIT(FID_RadiusX, StrValue).set(Vector); return ERR::Okay;
            case SVF_RY: UNIT(FID_RadiusY, StrValue).set(Vector); return ERR::Okay;
            case SVF_VERTICES: UNIT(FID_Vertices, StrValue).set(Vector); return ERR::Okay;
         }
         break;

      case CLASSID::VECTORWAVE:
         switch (Hash) {
            case SVF_X: UNIT(FID_X, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y: UNIT(FID_Y, StrValue).set(Vector); return ERR::Okay;
            case SVF_WIDTH:  UNIT(FID_Width, StrValue).set(Vector); return ERR::Okay;
            case SVF_HEIGHT: UNIT(FID_Height, StrValue).set(Vector); return ERR::Okay;
            case SVF_CLOSE:  Vector->set(FID_Close, StrValue); return ERR::Okay;
            case SVF_AMPLITUDE: UNIT(FID_Amplitude, StrValue).set(Vector); return ERR::Okay;
            case SVF_DECAY:     UNIT(FID_Decay, StrValue).set(Vector); return ERR::Okay;
            case SVF_FREQUENCY: UNIT(FID_Frequency, StrValue).set(Vector); return ERR::Okay;
            case SVF_THICKNESS: UNIT(FID_Thickness, StrValue).set(Vector); return ERR::Okay;
         }
         break;

      case CLASSID::VECTORRECTANGLE:
         switch (Hash) {
            case SVF_X1:
            case SVF_X:  UNIT(FID_X, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y1:
            case SVF_Y:  UNIT(FID_Y, StrValue).set(Vector); return ERR::Okay;
            case SVF_WIDTH:  UNIT(FID_Width, StrValue).set(Vector); return ERR::Okay;
            case SVF_HEIGHT: UNIT(FID_Height, StrValue).set(Vector); return ERR::Okay;
            case SVF_RX:     UNIT(FID_RoundX, StrValue).set(Vector); return ERR::Okay;
            case SVF_RY:     UNIT(FID_RoundY, StrValue).set(Vector); return ERR::Okay;

            case SVF_XOFFSET: UNIT(FID_XOffset, StrValue).set(Vector); return ERR::Okay; // Kotuku only
            case SVF_YOFFSET: UNIT(FID_YOffset, StrValue).set(Vector); return ERR::Okay; // Kotuku only

            case SVF_X2: {
               // Note: For the time being, VectorRectangle doesn't support X2/Y2 as a concept.  This would
               // cause problems if the client was to specify a scaled value here.
               auto width = UNIT(FID_Width, StrValue);
               Vector->setFields(fl::Width(std::abs(width - Vector->get<double>(FID_X))));
               return ERR::Okay;
            }

            case SVF_Y2: {
               auto height = UNIT(FID_Height, StrValue);
               Vector->setFields(fl::Height(std::abs(height - Vector->get<double>(FID_Y))));
               return ERR::Okay;
            }
         }
         break;

      // VectorPolygon handles polygon, polyline and line.
      case CLASSID::VECTORPOLYGON:
         switch (Hash) {
            case SVF_POINTS: Vector->set(FID_Points, StrValue); return ERR::Okay;
            case SVF_X1: UNIT(FID_X1, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y1: UNIT(FID_Y1, StrValue).set(Vector); return ERR::Okay;
            case SVF_X2: UNIT(FID_X2, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y2: UNIT(FID_Y2, StrValue).set(Vector); return ERR::Okay;
         }
         break;

      case CLASSID::VECTORTEXT:
         switch (Hash) {
            case SVF_X: UNIT(FID_X, StrValue).set(Vector); return ERR::Okay;
            case SVF_Y: UNIT(FID_Y, StrValue).set(Vector); return ERR::Okay;

            case SVF_DX: Vector->set(FID_DX, StrValue); return ERR::Okay;
            case SVF_DY: Vector->set(FID_DY, StrValue); return ERR::Okay;

            case SVF_LENGTHADJUST: // Can be set to either 'spacing' or 'spacingAndGlyphs'
               //if (iequals("spacingAndGlyphs", va_arg(list, STRING))) Vector->VT.SpacingAndGlyphs = TRUE;
               //else Vector->VT.SpacingAndGlyphs = FALSE;
               return ERR::Okay;

            case SVF_FONT: {
               // Officially accepted examples for the 'font' attribute:
               //
               //    12pt/14pt sans-serif
               //    80% sans-serif
               //    x-large/110% "new century schoolbook", serif
               //    bold italic large Palatino, serif
               //    normal small-caps 120%/120% fantasy
               //    oblique 12pt "Helvetica Nue", serif; font-stretch: condensed
               //
               // [ [ <'font-style'> || <'font-variant'> || <'font-weight'> ]? <'font-size'> [ / <'line-height'> ]? <'font-family'> ] | caption | icon | menu | message-box | small-caption | status-bar | inherit
               // TODO Add support for text font attribute
               return ERR::NoSupport;
            }

            case SVF_FONT_FAMILY:
               Vector->set(FID_Face, StrValue);
               return ERR::Okay;

            case SVF_FONT_SIZE:
               // A plain numeric font size is interpreted as "a height value corresponding to the current user
               // coordinate system".  Alternatively the user can specify the unit identifier, e.g. '12pt', '10%', '30px'
               Vector->set(FID_FontSize, StrValue);
               return ERR::Okay;

            case SVF_FONT_SIZE_ADJUST:
               // Auto-adjust the font height according to the formula "y(a/a') = c" where the value provided is used as 'a'.
               // y = 'font-size' of first-choice font
               // a' = aspect value of available font
               // c = 'font-size' to apply to available font
               return ERR::NoSupport;

            case SVF_FONT_STRETCH:
               switch(strihash(StrValue)) {
                  case SVF_CONDENSED:       Vector->set(FID_Stretch, int(VTS::CONDENSED)); return ERR::Okay;
                  case SVF_EXPANDED:        Vector->set(FID_Stretch, int(VTS::EXPANDED)); return ERR::Okay;
                  case SVF_EXTRA_CONDENSED: Vector->set(FID_Stretch, int(VTS::EXTRA_CONDENSED)); return ERR::Okay;
                  case SVF_EXTRA_EXPANDED:  Vector->set(FID_Stretch, int(VTS::EXTRA_EXPANDED)); return ERR::Okay;
                  case SVF_NARROWER:        Vector->set(FID_Stretch, int(VTS::NARROWER)); return ERR::Okay;
                  case SVF_NORMAL:          Vector->set(FID_Stretch, int(VTS::NORMAL)); return ERR::Okay;
                  case SVF_SEMI_CONDENSED:  Vector->set(FID_Stretch, int(VTS::SEMI_CONDENSED)); return ERR::Okay;
                  case SVF_SEMI_EXPANDED:   Vector->set(FID_Stretch, int(VTS::SEMI_EXPANDED)); return ERR::Okay;
                  case SVF_ULTRA_CONDENSED: Vector->set(FID_Stretch, int(VTS::ULTRA_CONDENSED)); return ERR::Okay;
                  case SVF_ULTRA_EXPANDED:  Vector->set(FID_Stretch, int(VTS::ULTRA_EXPANDED)); return ERR::Okay;
                  case SVF_WIDER:           Vector->set(FID_Stretch, int(VTS::WIDER)); return ERR::Okay;
                  default: log.warning("no support for font-stretch value '%s'", StrValue.c_str());
               }
               break;

            case SVF_FONT_STYLE: return ERR::NoSupport;
            case SVF_FONT_VARIANT: return ERR::NoSupport;

            case SVF_FONT_WEIGHT: { // SVG: normal | bold | bolder | lighter | inherit
               double num = strtod(StrValue.c_str(), nullptr);
               if (num) Vector->set(FID_Weight, num);
               else switch(strihash(StrValue)) {
                  case SVF_NORMAL:  Vector->set(FID_Weight, 400); return ERR::Okay;
                  case SVF_LIGHTER: Vector->set(FID_Weight, 300); return ERR::Okay; // -100 off the inherited weight
                  case SVF_BOLD:    Vector->set(FID_Weight, 700); return ERR::Okay;
                  case SVF_BOLDER:  Vector->set(FID_Weight, 900); return ERR::Okay; // +100 on the inherited weight
                  case SVF_INHERIT: Vector->set(FID_Weight, 400); return ERR::Okay; // Not supported correctly yet.
                  default: log.warning("No support for font-weight value '%s'", StrValue.c_str()); // Non-fatal
               }
               break;
            }

            case SVF_ROTATE: Vector->set(FID_Rotate, StrValue); return ERR::Okay;
            case SVF_STRING: Vector->set(FID_String, StrValue); return ERR::Okay;

            case SVF_TEXT_ANCHOR:
               switch(strihash(StrValue)) {
                  case SVF_START:   Vector->set(FID_Align, int(ALIGN::LEFT)); return ERR::Okay;
                  case SVF_MIDDLE:  Vector->set(FID_Align, int(ALIGN::HORIZONTAL)); return ERR::Okay;
                  case SVF_END:     Vector->set(FID_Align, int(ALIGN::RIGHT)); return ERR::Okay;
                  case SVF_INHERIT: Vector->set(FID_Align, int(ALIGN::NIL)); return ERR::Okay;
                  default: log.warning("text-anchor: No support for value '%s'", StrValue.c_str());
               }
               break;

            case SVF_TEXTLENGTH: Vector->set(FID_TextLength, StrValue); return ERR::Okay;
            // TextPath only
            //case SVF_STARTOFFSET: Vector->set(FID_StartOffset, StrValue); return ERR::Okay;
            //case SVF_METHOD: // The default is align.  For 'stretch' mode, set VMF::STRETCH in MorphFlags
            //                      Vector->set(FID_MorphFlags, StrValue); return ERR::Okay;
            //case SVF_SPACING:     Vector->set(FID_Spacing, StrValue); return ERR::Okay;
            //case SVF_XLINK_HREF:  // Used for drawing text along a path.
            //   return ERR::Okay;

            case SVF_KERNING: Vector->set(FID_Kerning, StrValue); return ERR::Okay; // Spacing between letters, default=1.0
            case SVF_LETTER_SPACING: Vector->set(FID_LetterSpacing, StrValue); return ERR::Okay;
            case SVF_PATHLENGTH: Vector->set(FID_PathLength, StrValue); return ERR::Okay;
            case SVF_WORD_SPACING:   Vector->set(FID_WordSpacing, StrValue); return ERR::Okay;
            case SVF_TEXT_DECORATION:
               switch(strihash(StrValue)) {
                  case SVF_UNDERLINE:    Vector->set(FID_Flags, int(VTXF::UNDERLINE)); return ERR::Okay;
                  case SVF_OVERLINE:     Vector->set(FID_Flags, int(VTXF::OVERLINE)); return ERR::Okay;
                  case SVF_LINETHROUGH:  Vector->set(FID_Flags, int(VTXF::LINE_THROUGH)); return ERR::Okay;
                  case SVF_BLINK:        Vector->set(FID_Flags, int(VTXF::BLINK)); return ERR::Okay;
                  case SVF_INHERIT:      return ERR::Okay;
                  default: log.warning("No support for text-decoration value '%s'", StrValue.c_str());
               }
               return ERR::Okay;
         }
         break;

      case CLASSID::VECTORSPIRAL:
         switch (Hash) {
            case SVF_PATHLENGTH: Vector->set(FID_PathLength, StrValue); return ERR::Okay;
            case SVF_CX:       UNIT(FID_CenterX, StrValue).set(Vector); return ERR::Okay;
            case SVF_CY:       UNIT(FID_CenterY, StrValue).set(Vector); return ERR::Okay;
            case SVF_R:        UNIT(FID_Radius, StrValue).set(Vector); return ERR::Okay;
            case SVF_OFFSET:   UNIT(FID_Offset, StrValue).set(Vector); return ERR::Okay;
            case SVF_STEP:     UNIT(FID_Step, StrValue).set(Vector); return ERR::Okay;
            case SVF_VERTICES: UNIT(FID_Vertices, StrValue).set(Vector); return ERR::Okay;
            case SVF_SPACING:  UNIT(FID_Spacing, StrValue).set(Vector); return ERR::Okay;
            case SVF_LOOP_LIMIT: UNIT(FID_LoopLimit, StrValue).set(Vector); return ERR::Okay;
         }
         break;

      case CLASSID::VECTORSHAPE:
         switch (Hash) {
            case SVF_CX:   UNIT(FID_CenterX, StrValue).set(Vector); return ERR::Okay;
            case SVF_CY:   UNIT(FID_CenterY, StrValue).set(Vector); return ERR::Okay;
            case SVF_R:    UNIT(FID_Radius, StrValue).set(Vector); return ERR::Okay;
            case SVF_N1:   UNIT(FID_N1, StrValue).set(Vector); return ERR::Okay;
            case SVF_N2:   UNIT(FID_N2, StrValue).set(Vector); return ERR::Okay;
            case SVF_N3:   UNIT(FID_N3, StrValue).set(Vector); return ERR::Okay;
            case SVF_M:    UNIT(FID_M, StrValue).set(Vector); return ERR::Okay;
            case SVF_A:    UNIT(FID_A, StrValue).set(Vector); return ERR::Okay;
            case SVF_B:    UNIT(FID_B, StrValue).set(Vector); return ERR::Okay;
            case SVF_PHI:  UNIT(FID_Phi, StrValue).set(Vector); return ERR::Okay;
            case SVF_VERTICES: UNIT(FID_Vertices, StrValue).set(Vector); return ERR::Okay;
            case SVF_MOD:      UNIT(FID_Mod, StrValue).set(Vector); return ERR::Okay;
            case SVF_SPIRAL:   UNIT(FID_Spiral, StrValue).set(Vector); return ERR::Okay;
            case SVF_REPEAT:   UNIT(FID_Repeat, StrValue).set(Vector); return ERR::Okay;
            case SVF_CLOSE:
               if ((iequals("true", StrValue)) or (iequals("1", StrValue))) Vector->set(FID_Close, TRUE);
               else Vector->set(FID_Close, FALSE);
               break;
         }
         break;

      case CLASSID::VECTORPATH:
         switch (Hash) {
            case SVF_D: Vector->set(FID_Sequence, StrValue); return ERR::Okay;
            case SVF_PATHLENGTH: Vector->set(FID_PathLength, StrValue); return ERR::Okay;
         }
         break;

      default: break;
   }

   // Fall-through to generic attributes.

   switch (Hash) {
      case SVF_APPEND_PATH: {
         // The append-path option is a Kotuku attribute that requires a reference to an instantiated vector with a path.
         OBJECTPTR other = nullptr;
         if (Self->Scene->findDef(StrValue.c_str(), &other) IS ERR::Okay) Vector->setAppendPath(other);
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_JOIN_PATH: {
         // The join-path option is a Kotuku attribute that requires a reference to an instantiated vector with a path.
         OBJECTPTR other = nullptr;
         if (Self->Scene->findDef(StrValue.c_str(), &other) IS ERR::Okay) {
            Vector->set(FID_AppendPath, other);
            Vector->setFlags(VF::JOIN_PATHS|Vector->Flags);
         }
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_TRANSITION: {
         OBJECTPTR trans = nullptr;
         if (Self->Scene->findDef(StrValue.c_str(), &trans) IS ERR::Okay) Vector->setTransition(trans);
         else log.warning("Unable to find element '%s' referenced at line %d", StrValue.c_str(), Tag.LineNo);
         break;
      }

      case SVF_COLOUR_INTERPOLATION:
      case SVF_COLOR_INTERPOLATION:
         if (iequals("auto", StrValue)) Vector->setColourSpace(VCS::SRGB);
         else if (iequals("sRGB", StrValue)) Vector->setColourSpace(VCS::SRGB);
         else if (iequals("linearRGB", StrValue)) Vector->setColourSpace(VCS::LINEAR_RGB);
         else if (iequals("inherit", StrValue)) Vector->setColourSpace(VCS::INHERIT);
         else log.warning("Invalid color-interpolation value '%s' at line %d", StrValue.c_str(), Tag.LineNo);
         break;

      case SVF_STROKE_LINEJOIN:
         switch(strihash(StrValue)) {
            case SVF_MITER: Vector->setLineJoin(int(VLJ::MITER)); break;
            case SVF_ROUND: Vector->setLineJoin(int(VLJ::ROUND)); break;
            case SVF_BEVEL: Vector->setLineJoin(int(VLJ::BEVEL)); break;
            case SVF_INHERIT: Vector->setLineJoin(int(VLJ::INHERIT)); break;
            case SVF_MITER_CLIP: Vector->setLineJoin(int(VLJ::MITER_SMART)); break; // Special AGG only join type
            case SVF_MITER_ROUND: Vector->setLineJoin(int(VLJ::MITER_ROUND)); break; // Special AGG only join type
         }
         break;

      case SVF_STROKE_INNERJOIN: // AGG ONLY
         switch(strihash(StrValue)) {
            case SVF_MITER:   Vector->setInnerJoin(int(VIJ::MITER));  break;
            case SVF_ROUND:   Vector->setInnerJoin(int(VIJ::ROUND)); break;
            case SVF_BEVEL:   Vector->setInnerJoin(int(VIJ::BEVEL)); break;
            case SVF_INHERIT: Vector->setInnerJoin(int(VIJ::INHERIT)); break;
            case SVF_JAG:     Vector->setInnerJoin(int(VIJ::JAG)); break;
         }
         break;

      case SVF_STROKE_LINECAP:
         switch(strihash(StrValue)) {
            case SVF_BUTT:    Vector->setLineCap(int(VLC::BUTT)); break;
            case SVF_SQUARE:  Vector->setLineCap(int(VLC::SQUARE)); break;
            case SVF_ROUND:   Vector->setLineCap(int(VLC::ROUND)); break;
            case SVF_INHERIT: Vector->setLineCap(int(VLC::INHERIT)); break;
         }
         break;

      case SVF_VISIBILITY:
         if (iequals("visible", StrValue))       Vector->setVisibility(VIS::VISIBLE);
         else if (iequals("hidden", StrValue))   Vector->setVisibility(VIS::HIDDEN);
         else if (iequals("collapse", StrValue)) Vector->setVisibility(VIS::COLLAPSE); // Same effect as hidden, kept for SVG compatibility
         else if (iequals("inherit", StrValue))  Vector->setVisibility(VIS::INHERIT);
         else log.warning("Unsupported visibility value '%s'", StrValue.c_str());
         break;

      case SVF_FILL_RULE:
         if (iequals("nonzero", StrValue)) Vector->setFillRule(int(VFR::NON_ZERO));
         else if (iequals("evenodd", StrValue)) Vector->setFillRule(int(VFR::EVEN_ODD));
         else if (iequals("inherit", StrValue)) Vector->setFillRule(int(VFR::INHERIT));
         else log.warning("Unsupported fill-rule value '%s'", StrValue.c_str());
         break;

      case SVF_CLIP_RULE:
         if (iequals("nonzero", StrValue)) Vector->setClipRule(int(VFR::NON_ZERO));
         else if (iequals("evenodd", StrValue)) Vector->setClipRule(int(VFR::EVEN_ODD));
         else if (iequals("inherit", StrValue)) Vector->setClipRule(int(VFR::INHERIT));
         else log.warning("Unsupported clip-rule value '%s'", StrValue.c_str());
         break;

      case SVF_ENABLE_BACKGROUND: // Deprecated in favour of 'isolated'
         log.warning("enable-background is deprecated in favour of the isolated attribute.");
         if (iequals("new", StrValue)) Vector->setFlags(Vector->Flags | VF::ISOLATED);
         break;

      case SVF_ISOLATION_MODE:
         if (iequals("isolated", StrValue)) Vector->setFlags(Vector->Flags | VF::ISOLATED);
         break;

      case SVF_ID:
         if (!Self->Cloning) {
            Vector->set(FID_ID, StrValue);
            Self->Scene->addDef(StrValue.c_str(), Vector);
            SetName(Vector, StrValue.c_str());
         }
         break;

      case SVF_DISPLAY:
         // The difference between 'display=none' and 'visibility=hidden' is that visibilility holds its
         // whitespace in document layout mode.  This has no relevance in our Vector Scene Graph, so 'display' is
         // treated as an obsolete feature and converted to visibility.

         if (iequals("none", StrValue))          Vector->setVisibility(VIS::HIDDEN);
         else if (iequals("inline", StrValue))   Vector->setVisibility(VIS::VISIBLE);
         else if (iequals("inherit", StrValue))  Vector->setVisibility(VIS::INHERIT);
         break;

      case SVF_NUMERIC_ID: Vector->set(FID_NumericID, StrValue); break;

      case SVF_OVERFLOW: // visible | hidden | scroll | auto | inherit
         log.trace("overflow is not supported.");
         break;

      case SVF_MARKER:       log.warning("marker is not supported."); break;
      case SVF_MARKER_END:   log.warning("marker-end is not supported."); break;
      case SVF_MARKER_MID:   log.warning("marker-mid is not supported."); break;
      case SVF_MARKER_START: log.warning("marker-start is not supported."); break;

      case SVF_FILTER:       Vector->setFilter(StrValue); break;

      case SVF_COLOR:
         if (StrValue != "inherit") Vector->setFill(StrValue);
         break;

      case SVF_STROKE:
         if (iequals("currentColor", StrValue)) {
            FRGB rgb;
            if (current_colour(Vector, rgb) IS ERR::Okay) Vector->set(FID_StrokeColour, rgb);
         }
         else set_paint_server(Vector, FID_Stroke, StrValue);
         break;

      case SVF_FILL:
         if (iequals("currentColor", StrValue)) {
            FRGB rgb;
            if (current_colour(Vector, rgb) IS ERR::Okay) Vector->set(FID_FillColour, rgb);
         }
         else set_paint_server(Vector, FID_Fill, StrValue);
         break;

      case SVF_TRANSFORM: parse_transform(Vector, StrValue, MTAG_SVG_TRANSFORM); break;

      case SVF_STROKE_DASHARRAY: {
         auto values = read_array(StrValue);
         Vector->setDashArray(values.data(), values.size());
         break;
      }

      case SVF_OPACITY:          Vector->set(FID_Opacity, std::clamp(strtod(StrValue.c_str(), nullptr), 0.0, 1.0)); break;
      case SVF_FILL_OPACITY:     Vector->set(FID_FillOpacity, std::clamp(strtod(StrValue.c_str(), nullptr), 0.0, 1.0)); break;
      case SVF_SHAPE_RENDERING:  Vector->set(FID_PathQuality, int(shape_rendering_to_render_quality(StrValue))); break;

      case SVF_STROKE_WIDTH:            UNIT(FID_StrokeWidth, StrValue).set(Vector); break;
      case SVF_STROKE_OPACITY:          Vector->set(FID_StrokeOpacity, std::clamp(strtod(StrValue.c_str(), nullptr), 0.0, 1.0)); break;
      case SVF_STROKE_MITERLIMIT:       Vector->set(FID_MiterLimit, StrValue); break;
      case SVF_STROKE_MITERLIMIT_THETA: Vector->set(FID_MiterLimitTheta, StrValue); break;
      case SVF_STROKE_INNER_MITERLIMIT: Vector->set(FID_InnerMiterLimit, StrValue); break;
      case SVF_STROKE_DASHOFFSET:       UNIT(FID_DashOffset, StrValue).set(Vector); break;

      case SVF_MASK: {
         OBJECTPTR clip;
         if (Self->Scene->findDef(StrValue.c_str(), &clip) IS ERR::Okay) {
            Vector->set(FID_Mask, clip);
         }
         else {
            log.warning("Unable to find mask '%s'", StrValue.c_str());
            return ERR::Search;
         }
         break;
      }

      case SVF_CLIP_PATH: {
         OBJECTPTR clip;
         if (Self->Scene->findDef(StrValue.c_str(), &clip) IS ERR::Okay) {
            Vector->set(FID_Mask, clip);
         }
         else {
            log.warning("Unable to find clip-path '%s'", StrValue.c_str());
            return ERR::Search;
         }
         break;
      }

      default: return ERR::UnsupportedField;
   }

   return ERR::Okay;
}
