//********************************************************************************************************************
// Only rebuild timing when parsing or an event changes the instance lists.

static void resolve_animations(extSVG *SVG)
{
   if (!SVG->AnimTimingDirty) return;

   std::vector<anim_timeline *> timelines;
   for (auto &record : SVG->Animations) {
      std::visit([&timelines](auto &Anim) { timelines.push_back(&Anim); }, record);
   }

   resolve_animation_times(timelines);
   SVG->AnimationDuration = 0;
   kt::Log log(__FUNCTION__);
   for (auto anim : timelines) {
      if (anim->cyclic) log.warning("Cyclic animation timing for '%s'; begin remains unresolved.", anim->id.c_str());
      if (anim->resolved_end.empty()) SVG->AnimationDuration = std::numeric_limits<double>::infinity();
      for (double end : anim->resolved_end) {
         SVG->AnimationDuration = std::max(SVG->AnimationDuration, end);
      }
   }

   SVG->AnimTimingDirty = false;
}

//********************************************************************************************************************
// Finalise animation targets after parsing. Each animated attribute has one base value and an ordered list of
// effects, including group effects on children without client-defined attributes.

static void prepare_value_animations(extSVG *SVG)
{
   // Resolve forward references now that all renderable targets have been instantiated.
   SVG->Animations.remove_if([SVG](auto &Record) {
      return std::visit([SVG](auto &Anim) {
         if (Anim.target_vector) return false;
         OBJECTPTR target;
         if (Anim.target_ref.empty() or (SVG->Scene->findDef(Anim.target_ref, &target) != ERR::Okay)) return true;
         Anim.target_vector = target->UID;
         Anim.target_ref.clear();
         return false;
      }, Record);
   });

   auto previous = std::move(SVG->ValueAnimations);
   SVG->ValueAnimations.clear();
   using key_type = std::pair<OBJECTID, uint32_t>;
   std::map<key_type, size_t> attributes, originals;

   for (size_t i=0; i < previous.size(); i++) originals.emplace(key_type(previous[i].target, previous[i].field), i);

   for (auto &record : SVG->Animations) {
      auto anim = std::get_if<anim_value>(&record);
      if (!anim) continue;
      auto bind = [&](OBJECTID Target) {
         kt::ScopedObjectLock<objVector> vector(Target, 1000);
         if (!vector.granted()) return;
         const auto field = animation_field(anim->target_attrib, **vector);
         auto key = std::make_pair(Target, field);
         const auto [entry, inserted] = attributes.emplace(key, SVG->ValueAnimations.size());

         if (inserted) {
            if (auto old = originals.find(key); old != originals.end()) {
               auto &value = SVG->ValueAnimations.emplace_back(std::move(previous[old->second]));
               value.animations.clear();
            }
            else SVG->ValueAnimations.push_back({ Target, field, anim->target_attrib,
               anim->read_orig_value(**vector, anim->target_attrib_orig), {} });
         }
         SVG->ValueAnimations[entry->second].animations.push_back(anim);
      };

      kt::ScopedObjectLock<objVector> target(anim->target_vector, 1000);

      if (!target.granted()) continue;

      if (target->classID() IS CLASSID::VECTORGROUP) {
         for (auto &child : anim->tag->Children) {
            if (!child.isTag() or child.attrib(anim->target_attrib)) continue;
            if (auto id = child.attrib("_id")) bind(std::stoi(*id));
         }
      }
      else bind(anim->target_vector);
   }
}

//********************************************************************************************************************
// Matrix storage belongs to the target vector.  Lock the owner before touching a cached matrix pointer.

static void reset_animation_matrices(extSVG *SVG)
{
   for (auto &[id, record] : SVG->Animatrix) {
      record.transforms.clear();
      kt::ScopedObjectLock<objVector> vector(id, 1000);
      if (vector.granted() and record.matrix) {
         vec::ResetMatrix(record.matrix);
         vec::FlushMatrix(record.matrix);
      }
   }

   for (auto &record : SVG->Animations) {
      if (auto motion = std::get_if<anim_motion>(&record); motion and motion->matrix) {
         kt::ScopedObjectLock<objVector> vector(motion->target_vector, 1000);
         if (vector.granted()) {
            vec::ResetMatrix(motion->matrix);
            vec::FlushMatrix(motion->matrix);
         }
      }
   }
}

//********************************************************************************************************************
// Follow the underlying effects required by a to-only animation.  Frozen effects sample the lower layers at their
// endpoint, so their value does not depend on the order in which document times are visited.

static void apply_value_animation(svgValueAnimation &Value, objVector &Vector, anim_value &Selected, double Time)
{
   if (Selected.non_interpolating or !Selected.from.empty() or !Selected.values.empty() or Selected.to.empty()) {
      std::swap(Selected.target_attrib_orig, Value.original);
      Selected.set_value(Vector);
      std::swap(Selected.target_attrib_orig, Value.original);
      return;
   }

   std::vector<anim_value *> effects;
   double sample_time = Time;
   for (size_t i=Value.animations.size(); i > 0; ) {
      auto anim = Value.animations[--i];
      anim->evaluate(sample_time);
      if (!anim->contributes()) continue;
      effects.push_back(anim);
      if (anim->non_interpolating or !anim->from.empty() or !anim->values.empty() or anim->to.empty()) break;
      if (anim->effect_ended) sample_time = anim->start_time + anim->effect_duration();
   }

   bool underlying = false;
   for (auto it=effects.rbegin(); it != effects.rend(); ++it) {
      auto anim = *it;
      std::string original = Value.original;
      if (underlying) {
         const auto attribute = strhash(anim->target_attrib);
         if ((attribute IS SVF_fill) or (attribute IS SVF_stroke) or
             (attribute IS SVF_color) or (attribute IS SVF_colour)) {
            FRGB *colour = nullptr;
            const auto field = (attribute IS SVF_stroke) ? strhash("strokeColour") : strhash("fillColour");
            if (!Vector.get(field, colour) and colour) {
               original = std::format("rgba({},{},{},{})", colour->Red * 255, colour->Green * 255,
                  colour->Blue * 255, colour->Alpha);
            }
         }
         else original = anim->read_orig_value(Vector, original);
      }
      std::swap(anim->target_attrib_orig, original);
      anim->set_value(Vector);
      std::swap(anim->target_attrib_orig, original);
      underlying = true;
   }

   // Group animations can be shared by several targets.  Leave their timeline at the requested document time.
   for (auto anim : Value.animations) anim->evaluate(Time);
}

//********************************************************************************************************************
// Render a frame at the given document time in seconds, relative to AnimEpoch.

static void render_frame(extSVG *SVG, double Time)
{
   if (Time < SVG->LastAnimTime) {
      for (auto &record : SVG->Animations) {
         std::visit([SVG, Time](auto &Anim) {
            if (Anim.discard_events_after(Time)) SVG->AnimTimingDirty = true;
         }, record);
      }
   }

   SVG->LastAnimTime = Time;
   resolve_animations(SVG);

   reset_animation_matrices(SVG);
   for (auto &record : SVG->Animations) {
      std::visit([Time](auto &Anim) { Anim.evaluate(Time); }, record);
   }

   svgState state(SVG);
   for (auto &value : SVG->ValueAnimations) {
      anim_value *selected = nullptr;
      for (auto anim : value.animations) {
         if (anim->contributes()) selected = anim;
      }

      if (!selected and !value.applied) continue;

      kt::ScopedObjectLock<objVector> vector(value.target, 1000);

      if (!vector.granted()) continue;

      if (selected) {
         apply_value_animation(value, **vector, *selected, Time);
      }
      else {
         const auto attribute = strhash(value.attribute);
         if (vector->classID() IS CLASSID::VECTORTEXT) {
            auto text = (objVectorText *)*vector;
            switch (attribute) {
               case SVF_font_family: text->setFace(value.original); break;
               case SVF_font_size: text->setFontSize(value.original); break;
               case SVF_text_anchor:
                  text->setAlign(value.original IS "end" ? ALIGN::RIGHT :
                     (value.original IS "middle" ? ALIGN::HORIZONTAL : ALIGN::LEFT));
                  break;
               default: state.set_property(*vector, attribute, *value.animations.front()->tag, value.original); break;
            }
         }
         else state.set_property(*vector, attribute, *value.animations.front()->tag, value.original);
      }
      value.applied = selected != nullptr;
   }

   apply_light_animations(SVG);

   for (auto &record : SVG->Animations) {
      if (auto motion = std::get_if<anim_motion>(&record)) {
         if (motion->contributes()) motion->perform();
      }
      else if (auto transform = std::get_if<anim_transform>(&record)) {
         if (transform->contributes()) transform->perform();
      }
   }

   // Apply transforms

   for (auto &record : SVG->Animatrix) {
      kt::ScopedObjectLock<objVector> vector(record.first, 1000);
      if (!vector.granted() or record.second.transforms.empty()) continue;

      auto &vt = record.second;

      // SVG rules state that only one transformation matrix is active at any time, irrespective of however many
      // <animateTransform> elements are active for a vector.  Multiple transformations are multiplicative by default.
      // If a transform is in REPLACE mode, all prior transforms are overwritten, INCLUDING the vector's 'transform'
      // attribute.

      if (not vt.matrix) {
         if (vector->newMatrix(&vt.matrix, false) != ERR::Okay) continue;
         vt.matrix->Tag = MTAG_ANIMATE_TRANSFORM;
      }

      // Replace mode is a little tricky if the vector has a transform attribute applied to it.  We want to
      // override the existing transform, but we could cause problems if we were to permanently destroy
      // that information.  The solution we're taking is to create an inversion of the transform declaration
      // in order to undo it.
      //
      // Tested in: w3-animate-elem-(24|81)-t.svg

      VectorMatrix *m = nullptr;
      if (vt.transforms.front()->additive IS ADD::REPLACE) {
         for (vector->getMatrices(m); (m); m=m->Next) {
            if ((unsigned int)m->Tag IS MTAG_SVG_TRANSFORM) {
               double d = 1.0 / (m->ScaleX * m->ScaleY - m->ShearY * m->ShearX);

               double t0 = m->ScaleY * d;
               vt.matrix->ScaleY = m->ScaleX * d;
               vt.matrix->ShearY = -m->ShearY * d;
               vt.matrix->ShearX = -m->ShearX * d;

               double t4 = -m->TranslateX * t0 - m->TranslateY * vt.matrix->ShearX;
               vt.matrix->TranslateY = -m->TranslateX * vt.matrix->ShearY - m->TranslateY * vt.matrix->ScaleY;

               vt.matrix->ScaleX = t0;
               vt.matrix->TranslateX = t4;
               break;
            }
         }
      }

      if (!m) vec::ResetMatrix(vt.matrix);

      // Apply the transforms in reverse.

      for (auto t = vt.transforms.rbegin(); t != vt.transforms.rend(); t++) {
         vt.matrix[0] *= t[0]->matrix;
         vec::FlushMatrix(vt.matrix);
         if (t[0]->additive IS ADD::REPLACE) break;
      }
   }

   SVG->Scene->Viewport->draw();

   if (SVG->FrameCallback.stale()) {
      SVG->FrameCallback.unpin();
      SVG->FrameCallback.clear();
   }

   if (SVG->FrameCallback.defined()) {
      if (SVG->FrameCallback.isC()) {
         kt::SwitchContext context(SVG->FrameCallback.Context);
         auto routine = (void (*)(extSVG *, APTR))SVG->FrameCallback.Routine;
         routine(SVG, SVG->FrameCallback.Meta);
      }
      else if (SVG->FrameCallback.isScript()) {
         sc::Call(SVG->FrameCallback, std::to_array<ScriptArg>({ { "SVG", SVG, FD_OBJECTPTR } }));
      }
   }
}

//********************************************************************************************************************

static ERR animation_timer(extSVG *SVG, int64_t TimeElapsed, int64_t CurrentTime)
{
   kt::Log log(__FUNCTION__);

   if (SVG->Animations.empty()) {
      log.msg("All animations processed, timer suspended.");
      SVG->AnimationTimer = nullptr;
      if (SVG->Scene) UnsubscribeAction(SVG->Scene, AC::Free);
      return ERR::Terminate;
   }

   const double current_time = (double(CurrentTime) / 1000000.0) - SVG->PausedTime;
   if (!SVG->AnimEpoch) SVG->AnimEpoch = current_time;
   render_frame(SVG, current_time - SVG->AnimEpoch);

   return ERR::Okay;
}
