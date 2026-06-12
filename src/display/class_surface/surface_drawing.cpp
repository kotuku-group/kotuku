
void copy_bkgd(const SURFACELIST &, int, int, int, ClipRectangle &, extBitmap *, extBitmap *, int16_t, bool);

ERR _expose_surface(OBJECTID SurfaceID, const SURFACELIST &List, int index, int X, int Y, int Width, int Height, EXF Flags)
{
   kt::Log log("expose_surface");
   int i, j;
   bool skip;
   OBJECTID parent_id;

   if ((Width < 1) or (Height < 1)) return ERR::Okay;
   if (!SurfaceID) return log.warning(ERR::NullArgs);
   if (index >= int(List.size())) return log.warning(ERR::OutOfRange);

   if (List[index].invisible() or (List[index].Width < 1) or (List[index].Height < 1)) {
      log.trace("Surface %d invisible or too small to draw.", SurfaceID);
      return ERR::Okay;
   }

   // Calculate the absolute coordinates of the exposed area

   ClipRectangle abs;
   if ((Flags & EXF::ABSOLUTE) != EXF::NIL) {
      abs.Left   = X;
      abs.Top    = Y;
      abs.Right  = Width;
      abs.Bottom = Height;
      Flags &= ~EXF::ABSOLUTE;
   }
   else {
      abs.Left   = List[index].Left + X;
      abs.Top    = List[index].Top + Y;
      abs.Right  = abs.Left + Width;
      abs.Bottom = abs.Top  + Height;
   }

   log.traceBranch("Surface:%d, %dx%d,%dx%d Flags: $%.4x", SurfaceID, abs.Left, abs.Top, abs.Right-abs.Left, abs.Bottom-abs.Top, int(Flags));

   // If the object is transparent, we need to scan back to a visible parent

   if (List[index].transparent()) {
      log.trace("Surface is transparent; scan to solid starting from index %d.", index);

      OBJECTID id = List[index].SurfaceID;
      for (j=index; j > 0; j--) {
         if (List[j].SurfaceID != id) continue;
         if (List[j].transparent()) id = List[j].ParentID;
         else break;
      }
      Flags |= EXF::CHILDREN;
      index = j;

      log.trace("New index %d.", index);
   }

   // Check if the exposed dimensions are outside of our boundary and/or our parent(s) boundaries.  If so then we must
   // restrict the exposed dimensions.  NOTE: This loop looks strange but is both correct & fast.  Don't alter it!

   for (i=index, parent_id = SurfaceID; ;) {
      if (List[i].invisible()) return ERR::Okay;
      auto area = List[i].area();
      clip_rectangle(abs, area);
      if (!(parent_id = List[i].ParentID)) break;
      i--;
      while (List[i].SurfaceID != parent_id) i--;
   }

   if ((abs.Left >= abs.Right) or (abs.Top >= abs.Bottom)) return ERR::Okay;

   // Check that the expose area actually overlaps the target surface

   if (abs.Left   >= List[index].Right) return ERR::Okay;
   if (abs.Top    >= List[index].Bottom) return ERR::Okay;
   if (abs.Right  <= List[index].Left) return ERR::Okay;
   if (abs.Bottom <= List[index].Top) return ERR::Okay;

   // The expose routine starts from the front and works to the back, so if the EXF::CHILDREN flag has been specified,
   // the first thing we do is scan to the final child that is listed in this particular area.

   if ((Flags & EXF::CHILDREN) != EXF::NIL) {
      // Change the index to the root bitmap of the exposed object
      index = find_bitmap_owner(List, index);
      for (i=index; (i < int(List.size())-1) and (List[i+1].Level > List[index].Level); i++); // Go all the way to the end of the list
   }
   else i = index;

   for (; i >= index; i--) {
      // Ignore non-visible surfaces

      if (List[i].transparent()) continue;
      if (List[i].isCursor() and (List[i].SurfaceID != SurfaceID)) continue;

      // If this is not a root bitmap object, skip it (i.e. consider it like a region)

      skip = false;
      parent_id = List[i].ParentID;
      for (j=i-1; j >= index; j--) {
         if (List[j].SurfaceID IS parent_id) {
            if (List[j].BitmapID IS List[i].BitmapID) skip = TRUE;
            break;
         }
      }
      if (skip) continue;

      auto childexpose = abs;

      if (i != index) {
         // Check this child object and its parents to make sure they are visible

         parent_id = List[i].SurfaceID;
         for (j=i; (j >= index) and (parent_id); j--) {
            if (List[j].SurfaceID IS parent_id) {
               if (List[j].invisible()) {
                  skip = TRUE;
                  break;
               }

               auto area = List[j].area();
               clip_rectangle(childexpose, area);

               parent_id = List[j].ParentID;
            }
         }
         if (skip) continue;

         // Skip this surface if there is nothing to be seen (lies outside the expose boundary)

         if ((childexpose.Right <= childexpose.Left) or (childexpose.Bottom <= childexpose.Top)) continue;
      }

      // Do the expose

      if (ScopedObjectLock<extBitmap> bitmap(List[i].BitmapID, 2000); bitmap.granted()) {
         expose_buffer(List, List.size(), i, i, childexpose.Left, childexpose.Top, childexpose.Right, childexpose.Bottom, List[index].DisplayID, *bitmap);
      }
      else {
         log.trace("Unable to access internal bitmap, sending delayed expose message.  Error: %s", GetErrorMsg(bitmap.error));

         struct drw::ExposeToDisplay expose {
            childexpose.Left   - List[i].Left,
            childexpose.Top    - List[i].Top,
            childexpose.Right  - childexpose.Left,
            childexpose.Bottom - childexpose.Top,
            EXF::NIL
         };
         QueueAction(drw::ExposeToDisplay::id, List[i].SurfaceID, &expose);
      }
   }

   // These flags should be set if the surface has had some area of it redrawn prior to the ExposeSurface() call.
   // This can be very important if the application has been writing to the surface directly rather than the more
   // conventional drawing procedures.

   // If the surface bitmap has not been changed, volatile redrawing just wastes CPU time for the user.

   if ((Flags & (EXF::REDRAW_VOLATILE|EXF::REDRAW_VOLATILE_OVERLAP)) != EXF::NIL) {
      // Redraw any volatile regions that intersect our expose area (such regions must be updated to reflect the new
      // background graphics).  Note that this routine does a fairly deep scan, due to the selective area copying
      // features in our system (i.e. we cannot just skim over the stuff that is immediately in front of us).
      //
      // EXF::REDRAW_VOLATILE: Redraws every single volatile object that intersects the expose, including internal
      //    volatile children.
      //
      // EXF::REDRAW_VOLATILE_OVERLAP: Only redraws volatile objects that obscure the expose from a position outside of
      //    the surface and its children.  Useful if no redrawing has occurred internally, but the surface object has
      //    been moved to a new position and the parents need to be redrawn.

      int level = List[index].Level + 1;

      if ((Flags & EXF::REDRAW_VOLATILE_OVERLAP) != EXF::NIL) { //OR (Flags & EXF::CHILDREN)) {
         // All children in our area have already been redrawn or do not need redrawing, so skip past them.

         for (i=index+1; (i < int(List.size())) and (List[i].Level > List[index].Level); i++);
         if (List[i-1].isCursor()) i--; // Never skip past the cursor
      }
      else {
         i = index;
         if (i < int(List.size())) i = i + 1;
         while ((i < int(List.size())) and (List[i].BitmapID IS List[index].BitmapID)) i++;
      }

      kt::Log log(__FUNCTION__);
      log.traceBranch("Redraw volatiles from idx %d, area %dx%d,%dx%d", i, abs.Left, abs.Top, abs.Right - abs.Left, abs.Bottom - abs.Top);

      if (i < tlVolatileIndex) i = tlVolatileIndex; // Volatile index allows the starting point to be specified

      // Redraw and expose volatile overlaps

      for (; (i < int(List.size())) and (List[i].Level > 1); i++) {
         if (List[i].Level < level) level = List[i].Level; // Drop the comparison level down so that we only observe objects in our general drawing space

         if (List[i].invisible()) {
            j = List[i].Level;
            while ((i+1 < int(List.size())) and (List[i+1].Level > j)) i++;
            continue;
         }

         if ((List[i].Flags & (RNF::VOLATILE|RNF::COMPOSITE|RNF::CURSOR)) != RNF::NIL) {
            if (List[i].SurfaceID IS SurfaceID) continue;

            if ((List[i].Right > abs.Left) and (List[i].Bottom > abs.Top) and
                (List[i].Left < abs.Right) and (List[i].Top < abs.Bottom)) {

               if ((List[i].Flags & RNF::COMPOSITE) IS RNF::NIL) { // Composites never require redrawing because they are not completely volatile, but we will expose them
                  _redraw_surface(List[i].SurfaceID, List, i, abs.Left, abs.Top, abs.Right, abs.Bottom, IRF::IGNORE_CHILDREN); // Redraw the volatile surface, ignore children
               }

               _expose_surface(List[i].SurfaceID, List, i, abs.Left, abs.Top, abs.Right, abs.Bottom, EXF::ABSOLUTE); // Redraw the surface, ignore children

               //while (List[i].BitmapID IS List[i+1].BitmapID) i++; This only works if the surfaces being skipped are completely intersecting one another.
            }
         }
      }
   }
   else {
      // Look for a software cursor at the end of the surfacelist and redraw it.  (We have to redraw the cursor as
      // expose_buffer() ignores it for optimisation purposes.)

      int i = List.size() - 1;
      if ((List[i].isCursor()) and (List[i].SurfaceID != SurfaceID)) {
         if ((List[i].Right > abs.Left) and (List[i].Bottom > abs.Top) and
             (List[i].Left < abs.Right) and (List[i].Top < abs.Bottom)) {

            kt::Log log(__FUNCTION__);
            log.traceBranch("Redrawing/Exposing cursor.");

            if ((List[i].Flags & RNF::COMPOSITE) IS RNF::NIL) { // Composites never require redrawing because they are not completely volatile
               _redraw_surface(List[i].SurfaceID, List, i, abs.Left, abs.Top, abs.Right, abs.Bottom, IRF::NIL);
            }

            _expose_surface(List[i].SurfaceID, List, i, abs.Left, abs.Top, abs.Right, abs.Bottom, EXF::ABSOLUTE);
         }
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Draw: Redraws the contents of a surface object.

Calling the Draw action on a surface object will send redraw messages to every hook that has been attached to the
surface object's drawing system.  This has the effect of redrawing all graphics within the surface object.  The
procedure is as follows:

<list type="ordered">
<li>If the surface object's #Colour field has been set, the target bitmap will be cleared to that colour.</li>
<li>If the surface is volatile, graphics from background surfaces will be copied to the  target bitmap.</li>
<li>Subscribers to the surface object are now called via their hooks so that they can draw to the bitmap.</li>
<li>The bitmap is copied to the video display buffer to complete the process.</li>
</>

Please be aware that:

<list>
<li>If the target surface contains child surfaces, they will not be redrawn unless they are volatile (using special
effects such as transparency, or using the region flag will make a surface volatile).</li>
<li>If the surface object has not had its background colour set, or if the object is not volatile, the bitmap
contents will not be automatically cleared (this is advantageous in situations where a particular object will clear
the surface area first).</li>
</>

-END-

*********************************************************************************************************************/

ERR SURFACE_Draw(extSurface *Self, struct acDraw *Args)
{
   kt::Log log;

   // If the Surface object is invisible, return immediately

   if (Self->invisible() or (tlNoDrawing) or (Self->Width < 1) or (Self->Height < 1)) {
      log.trace("Not drawing (invisible or tlNoDrawing set).");
      return ERR::Okay|ERR::Notified;
   }

   // Do not perform manual redraws when a redraw is scheduled.

   if (Self->RedrawScheduled) return ERR::Okay|ERR::Notified;

   int x, y, width, height;
   if (!Args) {
      x = 0;
      y = 0;
      width  = Self->Width;
      height = Self->Height;
   }
   else {
      x      = Args->X;
      y      = Args->Y;
      width  = Args->Width;
      height = Args->Height;
      if (!width) width = Self->Width;
      if (!height) height = Self->Height;
   }

   // Check if other draw messages are queued for this object - if so, do not do anything until the final message is reached.

   uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage) + sizeof(struct acDraw)];
   int msgindex = 0;
   while (!ScanMessages(&msgindex, MSGID::ACTION, msgbuffer, sizeof(msgbuffer))) {
      auto action = (ActionMessage *)(msgbuffer + sizeof(Message));

      if ((action->ActionID IS drw::InvalidateRegion::id) and (action->ObjectID IS Self->UID)) {
         if (!action->SendArgs) {
            return ERR::Okay|ERR::Notified;
         }
      }
      else if ((action->ActionID IS AC::Draw) and (action->ObjectID IS Self->UID)) {
         if (action->SendArgs IS TRUE) {
            auto msgdraw = (struct acDraw *)(action + 1);

            if (!Args) { // Tell the next message to draw everything.
               action->SendArgs = FALSE;
            }
            else {
               double right  = msgdraw->X + msgdraw->Width;
               double bottom = msgdraw->Y + msgdraw->Height;

               if (x < msgdraw->X) msgdraw->X = x;
               if (y < msgdraw->Y) msgdraw->Y = y;
               if ((x + width) > right)   right  = x + width;
               if ((y + height) > bottom) bottom = y + height;

               msgdraw->Width  = right - msgdraw->X;
               msgdraw->Height = bottom - msgdraw->Y;
            }

            UpdateMessage(((Message *)msgbuffer)->UID, MSGID::NIL, action, sizeof(ActionMessage) + sizeof(struct acDraw));
         }
         else {
            // We do nothing here because the next draw message will draw everything.
         }

         return ERR::Okay|ERR::Notified;
      }
   }


   log.traceBranch("%dx%d,%dx%d", x, y, width, height);
   if (auto error = RedrawSurface(Self->UID, x, y, width, height, IRF::RELATIVE|IRF::IGNORE_CHILDREN);
         error != ERR::Okay) {
      return error|ERR::Notified;
   }
   if (auto error = gfx::ExposeSurface(Self->UID, x, y, width, height, EXF::REDRAW_VOLATILE);
         error != ERR::Okay) {
      return error|ERR::Notified;
   }
   return ERR::Okay|ERR::Notified;
}

/*********************************************************************************************************************

-METHOD-
ExposeToDisplay: Redraws a surface region to the display, preferably from its graphics buffer.

Call the ExposeToDisplay() method to copy a surface region to the display.  The functionality is identical to that of the
~Surface.ExposeSurface() function.  Please refer to it for further documentation.

-INPUT-
int X: X coordinate of the expose area.
int Y: Y coordinate of the expose area.
int Width: Width of the expose area.
int Height: Height of the expose area.
int(EXF) Flags: Optional flags.

-ERRORS-
Okay
Notified

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_ExposeToDisplay(extSurface *Self, struct drw::ExposeToDisplay *Args)
{
   if (tlNoExpose) return ERR::Okay;

   // Check if other draw messages are queued for this object - if so, do not do anything until the final message is reached.

   uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage) + sizeof(*Args)];
   int msgindex = 0;
   while (!ScanMessages(&msgindex, MSGID::ACTION, msgbuffer, sizeof(msgbuffer))) {
      auto action = (ActionMessage *)(msgbuffer + sizeof(Message));

      if ((action->ActionID IS drw::ExposeToDisplay::id) and (action->ObjectID IS Self->UID)) {
         if (action->SendArgs) {
            auto msgexpose = (struct drw::ExposeToDisplay *)(action + 1);

            if (!Args) {
               // Invalidate everything
               msgexpose->X = 0;
               msgexpose->Y = 0;
               msgexpose->Width  = 20000;
               msgexpose->Height = 20000;
            }
            else {
               int right  = msgexpose->X + msgexpose->Width;
               int bottom = msgexpose->Y + msgexpose->Height;

               // Ignore region if it doesn't intersect

               if ((Args->X+Args->Width < msgexpose->X) or
                   (Args->Y+Args->Height < msgexpose->Y) or
                   (Args->X > right) or (Args->Y > bottom)) continue;

               if (Args->X < msgexpose->X) msgexpose->X = Args->X;
               if (Args->Y < msgexpose->Y) msgexpose->Y = Args->Y;
               if ((Args->X + Args->Width) > right)   right  = Args->X + Args->Width;
               if ((Args->Y + Args->Height) > bottom) bottom = Args->Y + Args->Height;

               msgexpose->Width  = right - msgexpose->X;
               msgexpose->Height = bottom - msgexpose->Y;
               msgexpose->Flags  |= Args->Flags;
            }

            UpdateMessage(((Message *)msgbuffer)->UID, MSGID::NIL, action, sizeof(ActionMessage) + sizeof(struct drw::ExposeToDisplay));
         }
         else {
            // We do nothing here because the next expose message will draw everything.
         }

         return ERR::Okay|ERR::Notified;
      }
   }


   ERR error;
   if (Args) error = gfx::ExposeSurface(Self->UID, Args->X, Args->Y, Args->Width, Args->Height, Args->Flags);
   else error = gfx::ExposeSurface(Self->UID, 0, 0, Self->Width, Self->Height, EXF::NIL);

   return error;
}

/*********************************************************************************************************************

-METHOD-
InvalidateRegion: Redraws all of the content in a surface object.

Invalidating a surface object will cause everything within a specified area to be redrawn.  This includes child surface
objects that intersect with the area that you have specified.  Parent regions that overlap are not included in the
redraw.

To quickly redraw an entire surface object's content, call this method directly without supplying an argument structure.
If you want to redraw a surface object and ignore all of its surface children then you should use the Draw action
instead of this method.

If you want to refresh a surface area to the display then you should use the #ExposeToDisplay() method instead.  Exposing
will use the graphics buffer to refresh the graphics, thus avoiding the speed loss of a complete redraw.

-INPUT-
int X: X coordinate of the region to invalidate.
int Y: Y coordinate of the region to invalidate.
int Width:  Width of the region to invalidate.
int Height: Height of the region to invalidate.

-ERRORS-
Okay:
AccessMemory: Failed to access the internal surface list.
Notified

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_InvalidateRegion(extSurface *Self, struct drw::InvalidateRegion *Args)
{
   if (Self->invisible() or (tlNoDrawing) or (Self->Width < 1) or (Self->Height < 1)) {
      return ERR::Okay|ERR::Notified;
   }

   // Do not perform manual redraws when a redraw is scheduled.

   if (Self->RedrawTimer) return ERR::Okay|ERR::Notified;

   // Check if other draw messages are queued for this object - if so, do not do anything until the final message is reached.

   int msgindex = 0;
   uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage) + sizeof(*Args)];
   while (!ScanMessages(&msgindex, MSGID::ACTION, msgbuffer, sizeof(msgbuffer))) {
      auto action = (ActionMessage *)(msgbuffer + sizeof(Message));
      if ((action->ActionID IS drw::InvalidateRegion::id) and (action->ObjectID IS Self->UID)) {
         if (action->SendArgs IS TRUE) {
            auto msginvalid = (struct drw::InvalidateRegion *)(action + 1);

            if (!Args) { // Invalidate everything
               action->SendArgs = FALSE;
            }
            else {
               double right  = msginvalid->X + msginvalid->Width;
               double bottom = msginvalid->Y + msginvalid->Height;

               if (Args->X < msginvalid->X) msginvalid->X = Args->X;
               if (Args->Y < msginvalid->Y) msginvalid->Y = Args->Y;
               if ((Args->X + Args->Width) > right)   right  = Args->X + Args->Width;
               if ((Args->Y + Args->Height) > bottom) bottom = Args->Y + Args->Height;

               msginvalid->Width  = right - msginvalid->X;
               msginvalid->Height = bottom - msginvalid->Y;
            }

            UpdateMessage(((Message *)msgbuffer)->UID, MSGID::NIL, action, sizeof(ActionMessage) + sizeof(struct drw::InvalidateRegion));
         }
         else { } // We do nothing here because the next invalidation message will draw everything.

         return ERR::Okay|ERR::Notified;
      }
   }


   if (Args) {
      if (auto error = RedrawSurface(Self->UID, Args->X, Args->Y, Args->Width, Args->Height, IRF::RELATIVE);
            error != ERR::Okay) {
         return error|ERR::Notified;
      }
      if (auto error = gfx::ExposeSurface(Self->UID, Args->X, Args->Y, Args->Width, Args->Height,
            EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP); error != ERR::Okay) {
         return error|ERR::Notified;
      }
   }
   else {
      if (auto error = RedrawSurface(Self->UID, 0, 0, Self->Width, Self->Height, IRF::RELATIVE);
            error != ERR::Okay) {
         return error|ERR::Notified;
      }
      if (auto error = gfx::ExposeSurface(Self->UID, 0, 0, Self->Width, Self->Height,
            EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP); error != ERR::Okay) {
         return error|ERR::Notified;
      }
   }

   return ERR::Okay|ERR::Notified;
}

//********************************************************************************************************************

void move_layer(extSurface *Self, int X, int Y)
{
   kt::Log log(__FUNCTION__);

   // If the coordinates are unchanged, do nothing

   if ((X IS Self->X) and (Y IS Self->Y)) return;

   if (!Self->initialised()) {
      Self->X = X;
      Self->Y = Y;
      return;
   }

   // This subroutine is used if the surface object is display-based

   if (!Self->ParentID) {
      if (ScopedObjectLock<objDisplay> display(Self->DisplayID, 2000); display.granted()) {
         // Subtract the host window's LeftMargin and TopMargin as MoveToPoint() is based on the coordinates of the window frame.

         if (!acMoveToPoint(*display, X - display->LeftMargin, Y - display->TopMargin, 0, MTF::X|MTF::Y)) {
            Self->X = X;
            Self->Y = Y;
            UpdateSurfaceRecord(Self);
         }
      }
      else log.warning(ERR::AccessObject);

      return;
   }

   // If the window is invisible, set the new coordinates and return immediately.

   if (Self->invisible()) {
      Self->X = X;
      Self->Y = Y;
      UpdateSurfaceRecord(Self);
      return;
   }

   int vindex, index;
   if ((index = find_surface_list(Self)) IS -1) return;

   ClipRectangle old(glSurfaces[index].Left, glSurfaces[index].Top, glSurfaces[index].Right, glSurfaces[index].Bottom);

   int destx = old.Left + X - Self->X;
   int desty = old.Top  + Y - Self->Y;

   int parent_index = find_parent_list(glSurfaces, Self);
   if (parent_index IS -1) return;

   // Since we do not own our graphics buffer, we need to shift the content in the buffer first, then send an
   // expose message to have the changes displayed on screen.
   //
   // This process is made more complex if there are siblings above and intersecting our surface.

   auto volatilegfx = check_volatile(glSurfaces, index);

   log.traceBranch("MoveLayer: Using simple expose technique [%s]", (volatilegfx ? "Volatile" : "Not Volatile"));

   Self->X = X;
   Self->Y = Y;

   update_surface_copy(Self);

   bool redraw;
   if (Self->transparent()) { // Transparent surfaces are treated as volatile if they contain graphics
      if (Self->CallbackCount > 0) redraw = true;
      else redraw = false;
   }
   else if ((volatilegfx) and ((Self->Flags & RNF::COMPOSITE) IS RNF::NIL)) redraw = true;
   else if (glSurfaces[index].BitmapID IS glSurfaces[parent_index].BitmapID) redraw = true;
   else redraw = false;

   if (redraw) _redraw_surface(Self->UID, glSurfaces, index, destx, desty, destx+Self->Width, desty+Self->Height, IRF::NIL);
   _expose_surface(Self->UID, glSurfaces, index, 0, 0, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);

   // Expose underlying graphics resulting from the movement

   for (vindex=index+1; (vindex < int(glSurfaces.size())) and
         (glSurfaces[vindex].Level > glSurfaces[index].Level); vindex++);
   tlVolatileIndex = vindex;
   auto clip = glSurfaces[index].area();
   redraw_nonintersect(Self->ParentID, glSurfaces, parent_index, clip, old,
      (glSurfaces[index].BitmapID IS glSurfaces[parent_index].BitmapID) ? IRF::SINGLE_BITMAP : IRF(-1),
      EXF::CHILDREN|EXF::REDRAW_VOLATILE);
   tlVolatileIndex = 0;

   refresh_pointer(Self);
}

/*********************************************************************************************************************
** Used for PRECOPY, AFTERCOPY and compositing surfaces.
**
** Self:       The surface object being drawn to.
** Index:      The index of the surface that needs its background copied.
** DestBitmap: The Bitmap related to the surface.
** Clip:       The absolute display coordinates of the expose area.
** Stage:      Either STAGE_PRECOPY or STAGE_AFTERCOPY.
*/

void prepare_background(extSurface *Self, const SURFACELIST &List, int Index, extBitmap *DestBitmap,
   const ClipRectangle &clip, int8_t Stage)
{
   kt::Log log("prepare_bkgd");

   log.traceBranch("%d Position: %dx%d,%dx%d", List[Index].SurfaceID, clip.Left, clip.Top, clip.Right - clip.Left, clip.Bottom - clip.Top);

   int end = Index;
   int master = Index;

   // Check if a root layer is set for this object.  A RootLayer determines the layer to use when opacity and
   // background graphics have precedence.  E.g. if a Window has 50% opacity, that means that all surfaces within
   // that window need to share the opacity and the background graphics of that window.

   int i, j;
   if ((Self) and (List[Index].SurfaceID != Self->RootID)) {
      for (j=0; j < int(List.size()); j++) {
         if (List[j].SurfaceID IS Self->RootID) {
            // Root layers are only considered when they are volatile (otherwise we want the current surface
            // object's own opacity settings to take precedence).  This ensures that objects like translucent
            // scrollbars can take priority if the parent is not translucent.
            //
            // If a custom root layer has been specified, then we are forced into using it as the end index.

            if (!Self->InheritedRoot) end = j; // A custom root layer has been specified by the user
            else if (List[j].isVolatile()) end = j; // The root layer is volatile and must be used
            break;
         }
      }
   }

   end = find_bitmap_owner(List, end);

   // Find the parent that owns this surface (we will use this as the starting point for our copy operation).
   // Everything that gets in the way between the parent and the location of our surface is what will be copied across.

   if (!List[end].ParentID) return;
   int parentindex = end;
   while ((parentindex > 0) and (List[parentindex].SurfaceID != List[end].ParentID)) parentindex--;
   if (List[parentindex].SurfaceID != List[end].ParentID) return;

   // If the parent object is invisible, we need to scan back to a visible parent

   OBJECTID id = List[parentindex].SurfaceID;
   for (j=parentindex; (j >= 0) and (List[j].Level > 1); j--) {
      if (List[j].SurfaceID IS id) {
         if (!List[j].transparent()) break;
         id = List[j].ParentID;
      }
   }
   if (j < 0) return;
   parentindex = j;

   // This loop will copy surface content to the buffered graphics area.  If the parentindex and end values are
   // correct, only siblings of the parent are considered in this loop.

   for (i=parentindex; i < end; i++) {
      if ((List[i].Flags & (RNF::TRANSPARENT|RNF::CURSOR)) != RNF::NIL) continue; // Ignore regions

      auto expose = clip;

      // Check the visibility of this layer and its parents

      if (restrict_region_to_parents(List, i, expose, true) <= 0) continue;

      int opaque;
      if (Stage IS STAGE_AFTERCOPY) {
         if (List[Index].RootID != List[Index].SurfaceID) opaque = List[Index].Opacity;
         else opaque = List[end].Opacity;
      }
      else opaque = 255;

      bool pervasive = ((List[Index].Flags & RNF::PERVASIVE_COPY) != RNF::NIL) and (Stage IS STAGE_AFTERCOPY);

      if (ScopedObjectLock<extBitmap> bitmap(List[i].BitmapID, 2000); bitmap.granted()) {
         copy_bkgd(List, i, end, master, expose, DestBitmap, *bitmap, opaque, pervasive);
      }
      else {
         log.warning("prepare_bkgd: %d failed to access bitmap #%d of surface #%d (error %d).", List[Index].SurfaceID, List[i].BitmapID, List[i].SurfaceID, int(bitmap.error));
         break;
      }
   }
}

//********************************************************************************************************************
// Coordinates are absolute.

void copy_bkgd(const SURFACELIST &List, int Index, int End, int Master, ClipRectangle &Area,
   extBitmap *DestBitmap, extBitmap *SrcBitmap, int16_t Opacity, bool Pervasive)
{
   kt::Log log(__FUNCTION__);

   // Scan for overlapping parent/sibling regions and avoid them

   for (int i=Index+1; (i < End) and (List[i].Level > 1); i++) {
      if ((List[i].Flags & (RNF::CURSOR|RNF::COMPOSITE)) != RNF::NIL); // Ignore regions
      else if (List[i].invisible()); // Skip hidden surfaces and their content
      else if (List[i].transparent()) continue; // Invisibles may contain important regions we have to block
      else if ((Pervasive) and (List[i].Level > List[Index].Level)); // If the copy is pervasive then all children must be ignored (so that we can copy translucent graphics over them)
      else {
         ClipRectangle lc(List[i].Left, List[i].Top, List[i].Right, List[i].Bottom);

         if ((lc.Left < Area.Right) and (lc.Top < Area.Bottom) and (lc.Right > Area.Left) and (lc.Bottom > Area.Top)) {
            if (lc.Left <= Area.Left) lc.Left = Area.Left;
            else {
               auto clip = ClipRectangle(Area.Left, Area.Top, lc.Left, Area.Bottom);
               copy_bkgd(List, Index, End, Master, clip, DestBitmap, SrcBitmap, Opacity, Pervasive); // left
            }

            if (lc.Right >= Area.Right) lc.Right = Area.Right;
            else {
               auto clip = ClipRectangle(lc.Right, Area.Top, Area.Right, Area.Bottom);
               copy_bkgd(List, Index, End, Master, clip, DestBitmap, SrcBitmap, Opacity, Pervasive); // right
            }

            if (lc.Top <= Area.Top) lc.Top = Area.Top;
            else {
               auto clip = ClipRectangle(lc.Left, Area.Top, lc.Right, lc.Top);
               copy_bkgd(List, Index, End, Master, clip, DestBitmap, SrcBitmap, Opacity, Pervasive); // top
            }

            if (lc.Bottom < Area.Bottom) {
               auto clip = ClipRectangle(lc.Left, lc.Bottom, lc.Right, Area.Bottom);
               copy_bkgd(List, Index, End, Master, clip, DestBitmap, SrcBitmap, Opacity, Pervasive); // bottom
            }

            return;
         }
      }

      // Skip past any children of the overlapping object.  This ensures that we only look at immediate parents and
      // siblings that are in our way.

      int j = i + 1;
      while ((j < End) and (j < int(List.size())) and (List[j].Level > List[i].Level)) j++;
      i = j - 1;
   }

   // Check if the exposed dimensions are outside of our boundary and/or our parent(s) boundaries.  If so then we must
   // restrict the exposed dimensions.

   auto expose = Area;
   if (restrict_region_to_parents(List, Index, expose, false) IS -1) return;

   log.traceBranch("[%d] Pos: %dx%d,%dx%d Bitmap: %d, Index: %d/%d", List[Index].SurfaceID, expose.Left, expose.Top, expose.Right - expose.Left, expose.Bottom - expose.Top, List[Index].BitmapID, Index, End);

   // The region is not obscured, so perform the redraw

   auto owner = find_bitmap_owner(List, Index);

   SrcBitmap->Clip.Left   = 0;
   SrcBitmap->Clip.Top    = 0;
   SrcBitmap->Clip.Right  = SrcBitmap->Width;
   SrcBitmap->Clip.Bottom = SrcBitmap->Height;

   if (Opacity < 255) SrcBitmap->Opacity = 255 - Opacity;

   gfx::CopyArea(SrcBitmap, DestBitmap, BAF::BLEND, expose.Left - List[owner].Left, expose.Top - List[owner].Top,
      expose.Right - expose.Left, expose.Bottom - expose.Top, expose.Left - List[Master].Left, expose.Top - List[Master].Top);

   SrcBitmap->Opacity = 255;
}
