// Input event handling for VectorScene

//********************************************************************************************************************
// Return the front-most viewport at (X,Y) by recursively scanning the scene graph from front to back.
// Clipping paths are taken into account for non-rectangular viewports.

extVectorViewport * get_viewport_at_xy(extVectorScene *Scene, double X, double Y)
{
   //kt::Log log(__FUNCTION__);
   //log.branch("Scene: %d", Scene->UID);

   auto inspect_xy = [](auto &Self, extVector *Vector, double X, double Y) -> extVectorViewport * {
      //kt::Log log("get_viewport_node");
      //log.branch("Vector: %d %s", Vector->UID, Vector->Name);

      // Always scan backwards since the last vector in the branch is top-most in its stack.
      extVector *last = Vector;
      while (last->Next) last = (extVector *)last->Next;
      extVectorViewport *hit_vp = nullptr;

      for (auto node=last; node; node=(extVector *)node->Prev) {
         if (node->Visibility IS VIS::HIDDEN) continue; // Checking for HIDDEN ensures that INHERIT is handled correctly

         if (node->classID() IS CLASSID::VECTORVIEWPORT) {
            auto vp = (extVectorViewport *)node;

            if (vp->dirty()) gen_vector_path(vp);

            bool hit = vp->vpBounds.hit_test(X, Y);
            if ((hit) and (vp->vpClip)) {
               // Hit-test could drop through due to a non-rectangular clipping path
               agg::rasterizer_scanline_aa<> raster;
               raster.add_path(vp->BasePath); // NB: Path is already transformed
               hit = raster.hit_test(X, Y);
            }

            if (hit) {
               hit_vp = vp;
               if (node->Child) {
                  auto child_hit = Self(Self, (extVector *)node->Child, X, Y);
                  return child_hit ? child_hit : hit_vp;
               }
            }
         }
         else if (node->Child) { // For VectorGroup and similar class types
            auto child_hit = Self(Self, (extVector *)node->Child, X, Y);
            if (child_hit) return child_hit;
         }
      }

      return hit_vp;
   };

   // Recursively inspect the scene graph

   auto hit_vp = inspect_xy(inspect_xy, (extVector *)Scene->Viewport, X, Y);

   return hit_vp ? hit_vp : (extVectorViewport *)Scene->Viewport;
}

//********************************************************************************************************************
// Send input event(s) to client subscribers

static void send_input_events(extVector *Vector, InputEvent *Event, bool Propagate = false)
{
   if (not Vector->InputSubscriptions) {
      if ((Propagate) and (Vector->Parent) and (Vector->Parent->Class->BaseClassID IS CLASSID::VECTOR)) {
         send_input_events((extVector *)Vector->Parent, Event, true);
      }
      return;
   }

   //kt::Log log(__FUNCTION__);
   //log.branch("Vector: %d, Type: %d", Vector->UID, Event->Type);

   bool consumed = false;
   for (auto it=Vector->InputSubscriptions->begin(); it != Vector->InputSubscriptions->end(); ) {
      auto &sub = *it;

      if (((Event->Mask & JTYPE::REPEATED) != JTYPE::NIL) and ((sub.Mask & JTYPE::REPEATED) IS JTYPE::NIL)) it++;
      else if ((sub.Mask & Event->Mask) != JTYPE::NIL) {
         ERR result = ERR::Terminate;
         consumed = true;

         if (sub.Callback.isC()) {
            kt::SwitchContext ctx(sub.Callback.Context);
            auto callback = (ERR (*)(objVector *, InputEvent *, APTR))sub.Callback.Routine;
            result = callback(Vector, Event, sub.Callback.Meta);
         }
         else if (sub.Callback.isScript()) {
            sc::Call(sub.Callback, std::to_array<ScriptArg>({
               { "Vector", Vector, FDF_OBJECT },
               { "InputEvent:Events", Event, FDF_STRUCT }
            }), result);
         }

      if (result IS ERR::Terminate) {
         deref_vector_callback(sub.Callback);
         it = Vector->InputSubscriptions->erase(it);
         update_input_subscription_state(Vector);
         mark_input_boundary_dirty(Vector);
         }
         else it++;
      }
      else it++;
   }

   // Some events can bubble-up if they are not intercepted by the target vector.

   if ((not consumed) and (Event->Type IS JET::WHEEL)) {
      if ((Vector->Parent) and (Vector->Parent->Class->BaseClassID IS CLASSID::VECTOR)) {
         send_input_events((extVector *)Vector->Parent, Event, true);
      }
   }
}

//********************************************************************************************************************

static void send_enter_event(extVector *Vector, const InputEvent *Event, double X = 0, double Y = 0)
{
   InputEvent event = {
      .Next        = nullptr,
      .Value       = double(Vector->UID),
      .Timestamp   = Event->Timestamp,
      .RecipientID = Vector->UID,
      .OverID      = Vector->UID,
      .AbsX        = Event->X,
      .AbsY        = Event->Y,
      .X           = Event->X - X,
      .Y           = Event->Y - Y,
      .DeviceID    = Event->DeviceID,
      .Type        = JET::CROSSED_IN,
      .Flags       = JTYPE::CROSSING,
      .Mask        = JTYPE::CROSSING
   };
   send_input_events(Vector, &event);
}

//********************************************************************************************************************

static void send_left_event(extVector *Vector, const InputEvent *Event, double X = 0, double Y = 0)
{
   InputEvent event = {
      .Next        = nullptr,
      .Value       = double(Vector->UID),
      .Timestamp   = Event->Timestamp,
      .RecipientID = Vector->UID,
      .OverID      = Vector->UID,
      .AbsX        = Event->X,
      .AbsY        = Event->Y,
      .X           = Event->X - X,
      .Y           = Event->Y - Y,
      .DeviceID    = Event->DeviceID,
      .Type        = JET::CROSSED_OUT,
      .Flags       = JTYPE::CROSSING,
      .Mask        = JTYPE::CROSSING
   };
   send_input_events(Vector, &event);
}

//********************************************************************************************************************

static void send_wheel_event(extVectorScene *Scene, extVector *Vector, const InputEvent *Event)
{
   InputEvent event = {
      .Next        = nullptr,
      .Value       = Event->Value,
      .Timestamp   = Event->Timestamp,
      .RecipientID = Vector->UID,
      .OverID      = Event->OverID,
      .AbsX        = Event->X,
      .AbsY        = Event->Y,
      .X           = Scene->ActiveVectorX,
      .Y           = Scene->ActiveVectorY,
      .DeviceID    = Event->DeviceID,
      .Type        = JET::WHEEL,
      .Flags       = JTYPE::ANALOG|JTYPE::EXT_MOVEMENT,
      .Mask        = JTYPE::EXT_MOVEMENT
   };
   send_input_events(Vector, &event);
}

//********************************************************************************************************************
// Receiver for input events from the Surface that hosts the scene graph.  Events are distributed to input
// subscribers.

ERR scene_input_events(const InputEvent *Events, int Handle)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extVectorScene *)CurrentContext();
   if (not Self->SurfaceID) return ERR::Okay; // Sanity check

   auto cursor = PTC::NIL;

   // Distribute input events to vectors that have subscriptions.
   // Be mindful that client code can potentially destroy the scene's surface at any time.
   //
   // NOTE: The ActiveVector refers to the vector that received the most recent input movement event.  It
   // receives wheel events and button presses.

   for (auto input=Events; input; input=input->Next) {
      if ((input->Flags & (JTYPE::ANCHORED|JTYPE::MOVEMENT)) != JTYPE::NIL) {
         while ((input->Next) and ((input->Next->Flags & JTYPE::MOVEMENT) != JTYPE::NIL)) { // Consolidate movement
            input = input->Next;
         }
      }

      if ((input->OverID != Self->SurfaceID) and ((input->Flags & JTYPE::CROSSING) IS JTYPE::NIL)) {
         // Activity occurring on another surface may be reported to us in circumstances where our surface is modal.
         continue;
      }

      // Focus management - clicking with the LMB can result in a change of focus.

      if (((input->Flags & JTYPE::BUTTON) != JTYPE::NIL) and (not ((input->Flags & JTYPE::REPEATED) != JTYPE::NIL)) and
          (input->Type IS JET::LMB) and (input->Value)) {
         apply_focus(Self, (extVector *)get_viewport_at_xy(Self, input->X, input->Y));
      }

      if (input->Type IS JET::WHEEL) {
         if (Self->ActiveVector) {
            kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
            if (lock.granted()) send_wheel_event(Self, lock.obj, input);
         }
      }
      else if (input->Type IS JET::CROSSED_OUT) {
         if (Self->ActiveVector) {
            kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
            if (lock.granted()) send_left_event(lock.obj, input, Self->ActiveVectorX, Self->ActiveVectorY);
         }
      }
      else if (input->Type IS JET::CROSSED_IN);
      else if ((input->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         OBJECTID target = Self->ButtonLock ? Self->ButtonLock : Self->ActiveVector;

         if (target) {
            kt::ScopedObjectLock<extVector> lk_vector(target);
            if (lk_vector.granted()) {
               InputEvent event = *input;
               event.Next   = nullptr;
               event.OverID = Self->ActiveVector;
               event.AbsX   = input->X; // Absolute coordinates are not translated.
               event.AbsY   = input->Y;
               event.X      = Self->ActiveVectorX;
               event.Y      = Self->ActiveVectorY;
               send_input_events(lk_vector.obj, &event);

               if ((input->Type IS JET::LMB) and ((input->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
                  Self->ButtonLock = input->Value ? target : 0;
               }
            }

            if (not Self->ButtonLock) {
               // If the button has been released then we need to compute the correct cursor and check if
               // an enter event is required.  This code has been pulled from the JTYPE::MOVEMENT handler
               // and reduced appropriately.

               if (cursor IS PTC::NIL) cursor = PTC::DEFAULT;
               bool processed = false;
               for (auto it = Self->InputBoundaries.rbegin(); it != Self->InputBoundaries.rend(); it++) {
                  auto &bounds = *it;

                  if ((processed) and (bounds.cursor IS PTC::NIL)) continue;

                  if (not bounds.bounds.hit_test(input->X, input->Y)) continue;

                  kt::ScopedObjectLock<extVector> lock(bounds.vector_id);
                  if (not lock.granted()) continue;
                  auto vector = lock.obj;

                  if (vector->pointInPath(input->X, input->Y) != ERR::Okay) continue;

                  if ((not Self->ButtonLock) and (vector->Cursor != PTC::NIL)) cursor = vector->Cursor;

                  if (bounds.pass_through) {
                     // For pass-through subscriptions input events are ignored, but cursor changes still apply.
                     continue;
                  }

                  if (Self->ActiveVector != bounds.vector_id) {
                     send_enter_event(vector, input, bounds.x, bounds.y);
                  }

                  if (not processed) {
                     double tx = input->X, ty = input->Y; // Invert the coordinates to pass localised coords to the vector.
                     auto invert = ~vector->Transform; // Presume that prior path generation has configured the transform.
                     invert.transform(&tx, &ty);

                     if ((Self->ActiveVector) and (Self->ActiveVector != vector->UID)) {
                        kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
                        if (lock.granted()) send_left_event(lock.obj, input, Self->ActiveVectorX, Self->ActiveVectorY);
                     }

                     Self->ActiveVector  = vector->UID;
                     Self->ActiveVectorX = tx;
                     Self->ActiveVectorY = ty;

                     processed = true;
                  }

                  if (cursor IS PTC::DEFAULT) continue; // Keep scanning in case an input boundary defines a cursor.
                  else break; // Input consumed and cursor image identified.
               }

               // If no vectors received a hit for a movement message, we may need to inform the last active vector that the
               // cursor left its area.

               if ((Self->ActiveVector) and (not processed)) {
                  kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
                  Self->ActiveVector = 0;
                  if (lock.granted()) send_left_event(lock.obj, input, Self->ActiveVectorX, Self->ActiveVectorY);
               }
            }
         }
      }
      else if ((input->Flags & (JTYPE::ANCHORED|JTYPE::MOVEMENT)) != JTYPE::NIL) {
         if (cursor IS PTC::NIL) cursor = PTC::DEFAULT;
         bool processed = false;
         for (auto it = Self->InputBoundaries.rbegin(); it != Self->InputBoundaries.rend(); it++) {
            auto &bounds = *it;

            if ((processed) and (bounds.cursor IS PTC::NIL)) continue;

            // When the user holds a mouse button over a vector, a 'button lock' will be held.  This causes all events to
            // be captured by that vector until the button is released.

            bool in_bounds = false;
            if ((Self->ButtonLock) and (Self->ButtonLock IS bounds.vector_id));
            else if ((Self->ButtonLock) and (Self->ButtonLock != bounds.vector_id)) continue;
            else { // No button lock, perform a simple bounds check
               in_bounds = bounds.bounds.hit_test(input->X, input->Y);
               if (not in_bounds) continue;
            }

            kt::ScopedObjectLock<extVector> lock(bounds.vector_id);
            if (not lock.granted()) {
               log.warning("Unable to lock vector #%d", bounds.vector_id);
               continue;
            }
            auto vector = lock.obj;

            // Additional bounds check to cater for transforms, clip masks etc.

            if (in_bounds) {
               if (vector->pointInPath(input->X, input->Y) != ERR::Okay) continue;
            }

            if (Self->ActiveVector != bounds.vector_id) {
               send_enter_event(vector, input, bounds.x, bounds.y);
            }

            if ((not Self->ButtonLock) and (vector->Cursor != PTC::NIL)) cursor = vector->Cursor;

            if (bounds.pass_through) {
               // For pass-through subscriptions, input events are ignored, but cursor changes still apply.
               continue;
            }

            if (not processed) {
               double tx = input->X, ty = input->Y; // Invert the coordinates to pass localised coords to the vector.
               auto invert = ~vector->Transform; // Presume that prior path generation has configured the transform.
               invert.transform(&tx, &ty);

               if ((Self->ActiveVector IS vector->UID) and (tx IS Self->ActiveVectorX) and (ty IS Self->ActiveVectorY)) {
                  // No change in position
               }
               else {
                  InputEvent event = *input;
                  event.Next   = nullptr;
                  event.OverID = vector->UID;
                  event.AbsX   = input->X; // Absolute coordinates are not translated.
                  event.AbsY   = input->Y;
                  event.X      = tx;
                  event.Y      = ty;
                  send_input_events(vector, &event);

                  if ((Self->ActiveVector) and (Self->ActiveVector != vector->UID)) {
                     kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
                     if (lock.granted()) send_left_event(lock.obj, input, Self->ActiveVectorX, Self->ActiveVectorY);
                  }

                  Self->ActiveVector  = vector->UID;
                  Self->ActiveVectorX = tx;
                  Self->ActiveVectorY = ty;
               }

               processed = true;
            }

            if (cursor IS PTC::DEFAULT) continue; // Keep scanning in case an input boundary defines a cursor.
            else break; // Input consumed and cursor image identified.
         }

         // If no vectors received a hit for a movement message, we may need to inform the last active vector that the
         // cursor left its area.

         if ((Self->ActiveVector) and (not processed)) {
            kt::ScopedObjectLock<extVector> lock(Self->ActiveVector);
            Self->ActiveVector = 0;
            if (lock.granted()) send_left_event(lock.obj, input, Self->ActiveVectorX, Self->ActiveVectorY);
         }
      }
      else log.warning("Unrecognised movement type %d", int(input->Type));
   }

   if (not Self->ButtonLock) {
      if (cursor IS PTC::NIL) cursor = PTC::DEFAULT; // Revert the cursor to the default if nothing is defined

      if (Self->Cursor != cursor) {
         Self->Cursor = cursor;
         kt::ScopedObjectLock<objSurface> surface(Self->SurfaceID);
         if (surface.granted() and (surface.obj->Cursor != Self->Cursor)) {
            surface.obj->setCursor(cursor);
         }
      }
   }

   return ERR::Okay;
}
