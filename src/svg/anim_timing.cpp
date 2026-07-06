
//********************************************************************************************************************
// Manually activate the animation.  If Reset is true, the animation is guaranteed to start from seek position zero.

void anim_base::activate(bool Reset)
{
   // Reset the variables that control time management and the animation will start from scratch.

   if (Reset) {
      begin_offset = (double(PreciseTime()) / 1000000.0) - svg->AnimEpoch; // Seeks to position zero
      repeat_index = 0;
   }

   start_time = svg->AnimEpoch + begin_offset; // Directly affects the calculated seek value

   end_time   = 0;

   // Test: w3-animate-elem-21-t.svg

   if (auto others = svg->StartOnBegin.find(hash_id()); others != svg->StartOnBegin.end()) {
      for (auto &other : others->second) {
         other->activate(true);
         other->start_time = start_time; // Ensure that times match exactly
      }
   }
}

//********************************************************************************************************************
// Return true if the animation has started.  For absolute consistency, animations start 'at the time they should have
// started', which we can strictly calculate from begin and duration timing values.

bool anim_base::started(double CurrentTime)
{
   if (end_time) return false;
   if (start_time) return true;
   if (repeat_index > 0) return true;
   if (CurrentTime < svg->AnimEpoch + begin_offset) return false;
   activate(false);
   return true;
}

//********************************************************************************************************************
// Advance the seek position to represent the next frame.

bool anim_base::next_frame(double CurrentTime)
{
   if (end_time) return false;

   const double elapsed = CurrentTime - start_time;

   if (!duration) seek = 0;
   else seek = elapsed / duration; // A value between 0 and 1.0

   if (seek >= 1.0) { // Check if the sequence has ended.
      if ((repeat_count < 0) or (repeat_index+1 < repeat_count)) {
         repeat_index++;
         start_time = CurrentTime;
         seek = 0;
         return false;
      }
      else {
         if (seek > 1.0) seek = 1.0;
         return true;
      }
   }

   // repeat_duration prevents the animation from running past a fixed number of seconds since it started.
   if ((repeat_duration > 0) and (elapsed > repeat_duration)) return true;

   return false;
}

//********************************************************************************************************************

void anim_base::stop(double Time)
{
   if (!begin_series.empty()) {
      // Check if there's a serialised begin offset following the one that's completed.
      int i;
      for (i=0; i < std::ssize(begin_series)-1; i++) {
         if (begin_offset IS begin_series[i]) {
            begin_offset = begin_series[i+1];
            start_time = 0;
            return;
         }
      }
   }

   end_time = Time;
   seek = 1.0; // Necessary in case the seek range calculation has overflowed

   // Start animations that are to be triggered from our ending.
   if (auto others = svg->StartOnEnd.find(hash_id()); others != svg->StartOnEnd.end()) {
      for (auto &other : others->second) {
         other->activate(true);
         other->start_time = Time;
      }
   }
}

//********************************************************************************************************************

static ERR animation_timer(extSVG *SVG, int64_t TimeElapsed, int64_t CurrentTime)
{
   kt::Log log(__FUNCTION__);

   if (SVG->Animations.empty()) {
      log.msg("All animations processed, timer suspended.");
      return ERR::Terminate;
   }

   for (auto &matrix : SVG->Animatrix) {
      matrix.second.transforms.clear();
   }

   if (!SVG->AnimEpoch) SVG->AnimEpoch = double(CurrentTime) / 1000000.0;
   double current_time = double(CurrentTime) / 1000000.0;

   for (auto &record : SVG->Animations) {
      std::visit([ current_time ](auto &&anim) {
         if (not anim.started(current_time)) return;

         bool stop = anim.next_frame(current_time);
         anim.perform();
         if (stop) anim.stop(current_time);
      }, record);
   }

   // Apply transforms

   for (auto &record : SVG->Animatrix) {
      kt::ScopedObjectLock<objVector> vector(record.first, 1000);
      if (record.second.transforms.empty()) continue;

      auto &vt = record.second;

      // SVG rules state that only one transformation matrix is active at any time, irrespective of however many
      // <animateTransform> elements are active for a vector.  Multiple transformations are multiplicative by default.
      // If a transform is in REPLACE mode, all prior transforms are overwritten, INCLUDING the vector's 'transform'
      // attribute.

      if (not vt.matrix) {
         vector->newMatrix(&vt.matrix, false);
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
         for (m = vector->Matrices; (m); m=m->Next) {
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

   return ERR::Okay;
}
