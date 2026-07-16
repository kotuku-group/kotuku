/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Surface: Manages display regions for two-dimensional rendered graphics.

The Surface class represents a rectangular region in the display hierarchy.  It manages positioning, visibility,
redraws, backing storage and focus for layered user interfaces.  Surfaces work with @Bitmap objects for rendered
pixel data and with the @Pointer class for pointer interaction.

A top-level surface is normally backed by an application window on hosted platforms such as Windows and Linux.  For
full screen displays and Android, a top-level surface can cover the display without a separate host
window.  Child surfaces are attached to a parent surface, creating a hierarchy whose ordering, clipping, redraw and
input behaviour is managed by this class.

Building nested surface-only interfaces is supported, but most applications should use surfaces as hosts for
@VectorScene objects.  This keeps interfaces resolution independent and matches the rendering path that Kōtuku
optimises most heavily.

Surface content is preserved with a backing store where required.  Surfaces that share a backing store are composited
through their parent, while surfaces that need independent redraw, opacity, video memory or cursor behaviour can own a
separate bitmap buffer.

-END-

*********************************************************************************************************************/

#undef __xwindows__
#include "../defs.h"
#include <kotuku/modules/image.h>
#include <numeric> // For std::gcd

#ifdef _WIN32
using namespace display;
#endif

static ERR SET_Opacity(extSurface *, double);
static ERR SET_XOffset(extSurface *, Unit &);
static ERR SET_YOffset(extSurface *, Unit &);

constexpr int MOVE_VERTICAL   = 0x0001;
constexpr int MOVE_HORIZONTAL = 0x0002;

static ERR consume_input_events(const InputEvent *, int);
static void draw_region(extSurface *, extSurface *, extBitmap *);
static ERR redraw_timer(extSurface *, int64_t, int64_t);

static void deref_surface_callback(FUNCTION &Function)
{
   if (Function.defined()) {
      if (Function.isScript() and (not Function.stale())) ((objScript *)Function.Context)->derefProcedure(Function);
      Function.unpin();
      Function.clear();
   }
}

static void deref_surface_callbacks(std::vector<FUNCTION> &Callbacks)
{
   for (auto &callback : Callbacks) deref_surface_callback(callback);
}

//********************************************************************************************************************
// This call is used to refresh the pointer image when at least one layer has been rearranged.  The timer is used to
// delay the refresh - useful if multiple surfaces are being rearranged when we only need to do the refresh once.
// The delay also prevents clashes with read/write access to the surface list.

static ERR refresh_pointer_timer(OBJECTPTR Task, int64_t Elapsed, int64_t CurrentTime)
{
   objPointer *pointer;
   if ((pointer = gfx::AccessPointer())) {
      acRefresh(pointer);
      ReleaseObject(pointer);
   }
   glRefreshPointerTimer = 0;
   return ERR::Terminate; // Timer is only called once
}

void refresh_pointer(extSurface *Self)
{
   if (!glRefreshPointerTimer) {
      kt::SwitchContext context(glModule);
      SubscribeTimer(0.02, C_FUNCTION(refresh_pointer_timer), &glRefreshPointerTimer);
   }
}

//********************************************************************************************************************

static ERR access_video(OBJECTID DisplayID, objDisplay **Display, objBitmap **Bitmap)
{
   if (!AccessObject(DisplayID, 5000, (OBJECTPTR *)Display)) {
      #ifdef _WIN32
      APTR winhandle;
      if (!Display[0]->getWindowHandle(winhandle)) {
         Display[0]->Bitmap->setHandle(winGetDC(winhandle));
      }
      #endif

      if (Bitmap) *Bitmap = Display[0]->Bitmap;
      return ERR::Okay;
   }
   else return ERR::AccessObject;
}

//********************************************************************************************************************

static void release_video(objDisplay *Display)
{
   #ifdef _WIN32
      APTR surface;
      Display->Bitmap->getHandle(surface);

      APTR winhandle;
      if (!Display->getWindowHandle(winhandle)) {
         winReleaseDC(winhandle, surface);
      }

      Display->Bitmap->setHandle((APTR)nullptr);
   #endif

   acFlush(Display);

   ReleaseObject(Display);
}

//********************************************************************************************************************
// Used By:  MoveToBack(), move_layer()
//
// This is the best way to figure out if a surface object or its children causes it to be volatile.  Use this function
// if you don't want to do any deep scanning to determine who is volatile or not.
//
// Volatile flags are PRECOPY, AFTER_COPY and CURSOR.
//
// NOTE: Surfaces marked as COMPOSITE or TRANSPARENT are not considered volatile as they do not require redraws.  It's
// up to the caller to make a decision as to whether COMPOSITE's are volatile or not.

static bool check_volatile(const SURFACELIST &List, int Index)
{
   if ((Index < 0) or (Index >= int(List.size()))) return false;

   if (List[Index].isVolatile()) return true;

   // If there are children with custom root layers or are volatile, that will force volatility

   int j;
   for (int i=Index+1; (i < int(List.size())) and (List[i].Level > List[Index].Level); i++) {
      if (List[i].invisible()) {
         j = List[i].Level;
         while ((i + 1 < int(List.size())) and (List[i+1].Level > j)) i++;
         continue;
      }

      if (List[i].isVolatile()) {
         // If a child surface is marked as volatile and is a member of our bitmap space, then effectively all members of the bitmap are volatile.

         if (List[Index].BitmapID IS List[i].BitmapID) return true;

         // If this is a custom root layer, check if it refers to a surface that is going to affect our own volatility.

         if (List[i].RootID != List[i].SurfaceID) {
            for (j=i; j > Index; j--) {
               if (List[i].RootID IS List[j].SurfaceID) break;
            }

            if (j <= Index) return true; // Custom root of a child is outside of bounds - that makes us volatile
         }
      }
   }

   return false;
}

//********************************************************************************************************************

static void expose_buffer(const SURFACELIST &list, int Limit, int Index, int ScanIndex, int Left, int Top,
                   int Right, int Bottom, OBJECTID DisplayID, extBitmap *Bitmap)
{
   kt::Log log(__FUNCTION__);

   // Scan for overlapping parent/sibling regions and avoid them

   int i, j;
   for (i=ScanIndex+1; (i < Limit) and (list[i].Level > 1); i++) {
      if (list[i].invisible()) { // Skip past non-visible areas and their content
         j = list[i].Level;
         while ((i+1 < Limit) and (list[i+1].Level > j)) i++;
         continue;
      }
      else if (list[i].isCursor()); // Skip the cursor
      else {
         auto listclip = list[i].area();

         if (restrict_region_to_parents(list, i, listclip, false) IS -1); // Skip
         else if ((listclip.Left < Right) and (listclip.Top < Bottom) and (listclip.Right > Left) and (listclip.Bottom > Top)) {
            if (list[i].BitmapID IS list[Index].BitmapID) continue; // Ignore any children that overlap & form part of our bitmap space.  Children that do not overlap are skipped.

            if (listclip.Left <= Left) listclip.Left = Left;
            else expose_buffer(list, Limit, Index, ScanIndex, Left, Top, listclip.Left, Bottom, DisplayID, Bitmap); // left

            if (listclip.Right >= Right) listclip.Right = Right;
            else expose_buffer(list, Limit, Index, ScanIndex, listclip.Right, Top, Right, Bottom, DisplayID, Bitmap); // right

            if (listclip.Top <= Top) listclip.Top = Top;
            else expose_buffer(list, Limit, Index, ScanIndex, listclip.Left, Top, listclip.Right, listclip.Top, DisplayID, Bitmap); // top

            if (listclip.Bottom < Bottom) expose_buffer(list, Limit, Index, ScanIndex, listclip.Left, listclip.Bottom, listclip.Right, Bottom, DisplayID, Bitmap); // bottom

            if (list[i].transparent()) {
               // In the case of invisible regions, we will have split the expose process as normal.  However,
               // we also need to look deeper into the invisible region to discover if there is more that
               // we can draw, depending on the content of the invisible region.

               listclip = list[i].area();

               if (Left > listclip.Left)     listclip.Left   = Left;
               if (Top > listclip.Top)       listclip.Top    = Top;
               if (Right < listclip.Right)   listclip.Right  = Right;
               if (Bottom < listclip.Bottom) listclip.Bottom = Bottom;

               expose_buffer(list, Limit, Index, i, listclip.Left, listclip.Top, listclip.Right, listclip.Bottom, DisplayID, Bitmap);
            }

            return;
         }
      }

      // Skip past any children of the non-overlapping object.  This ensures that we only look at immediate parents and siblings that are in our way.

      j = i + 1;
      while ((j < Limit) and (list[j].Level > list[i].Level)) j++;
      i = j - 1;
   }

   log.traceBranch("[%d] %dx%d,%dx%d Bmp: %d, Idx: %d/%d", list[Index].SurfaceID, Left, Top, Right - Left, Bottom - Top, list[Index].BitmapID, Index, ScanIndex);

   // The region is not obscured, so perform the redraw

   int owner = find_bitmap_owner(list, Index);

   // Set the clipping to match the source bitmap exactly (i.e. nothing fancy happening here).
   // The real clipping occurs in the display clip.

   Bitmap->Clip.Left   = list[Index].Left - list[owner].Left;
   Bitmap->Clip.Top    = list[Index].Top - list[owner].Top;
   Bitmap->Clip.Right  = list[Index].Right - list[owner].Left;
   Bitmap->Clip.Bottom = list[Index].Bottom - list[owner].Top;
   if (Bitmap->Clip.Right  > Bitmap->Width)  Bitmap->Clip.Right  = Bitmap->Width;
   if (Bitmap->Clip.Bottom > Bitmap->Height) Bitmap->Clip.Bottom = Bitmap->Height;

   // Set the clipping so that we are only drawing to the display area that has been exposed

   int iscr = Index;
   while ((iscr > 0) and (list[iscr].ParentID)) iscr--; // Find the top-level display entry

   // If COMPOSITE is in use, this means we have to do compositing on the fly.  This involves copying the background
   // graphics into a temporary buffer, then blitting the composite buffer to the display.

   // Note: On hosted displays in Windows or Linux, compositing is handled by the host's graphics system if the surface
   // is at the root level (no ParentID).

   int sx, sy;
   if (((list[Index].Flags & RNF::COMPOSITE) != RNF::NIL) and
       ((list[Index].ParentID) or (list[Index].isCursor()))) {
      if (glComposite) {
         if (glComposite->BitsPerPixel != list[Index].BitsPerPixel) {
            FreeResource(glComposite);
            glComposite = nullptr;
         }
         else {
            if ((glComposite->Width < list[Index].Width) or (glComposite->Height < list[Index].Height)) {
               acResize(glComposite, (list[Index].Width > glComposite->Width) ? list[Index].Width : glComposite->Width,
                                     (list[Index].Height > glComposite->Height) ? list[Index].Height : glComposite->Height,
                                     0);
            }
         }
      }

      if (!glComposite) {
         if (!(glComposite = extBitmap::create::untracked(fl::Width(list[Index].Width), fl::Height(list[Index].Height)))) {
            return;
         }

         SetOwner(glComposite, glModule);
      }

      // Build the background in our buffer

      ClipRectangle clip(Left, Top, Right, Bottom);
      prepare_background(nullptr, list, Index, glComposite, clip, STAGE_COMPOSITE);

      // Blend the surface's graphics into the composited buffer
      // NOTE: THE FOLLOWING IS NOT OPTIMISED WITH RESPECT TO CLIPPING

      gfx::CopyArea(Bitmap, glComposite, BAF::BLEND, 0, 0, list[Index].Width, list[Index].Height, 0, 0);

      Bitmap = glComposite;
      sx = 0;  // Always zero as composites own their bitmap
      sy = 0;
   }
   else {
      sx = list[Index].Left - list[owner].Left;
      sy = list[Index].Top - list[owner].Top;
   }

   objDisplay *display;
   objBitmap *video_bmp;
   if (!access_video(DisplayID, &display, &video_bmp)) {
      video_bmp->Clip.Left   = Left   - list[iscr].Left; // Ensure that the coords are relative to the display bitmap (important for Windows, X11)
      video_bmp->Clip.Top    = Top    - list[iscr].Top;
      video_bmp->Clip.Right  = Right  - list[iscr].Left;
      video_bmp->Clip.Bottom = Bottom - list[iscr].Top;
      if (video_bmp->Clip.Left   < 0) video_bmp->Clip.Left = 0;
      if (video_bmp->Clip.Top    < 0) video_bmp->Clip.Top  = 0;
      if (video_bmp->Clip.Right  > video_bmp->Width)  video_bmp->Clip.Right  = video_bmp->Width;
      if (video_bmp->Clip.Bottom > video_bmp->Height) video_bmp->Clip.Bottom = video_bmp->Height;

      update_display((extDisplay *)display, Bitmap, sx, sy, // Src X/Y (bitmap relative)
         list[Index].Width, list[Index].Height,
         list[Index].Left - list[iscr].Left, list[Index].Top - list[iscr].Top); // Dest X/Y (absolute display position)

      release_video(display);
   }
   else log.warning("Unable to access display #%d.", DisplayID);
}

/*********************************************************************************************************************
** Used by MoveToFront()
**
** This function will expose areas that are uncovered when a surface changes its position in the surface tree (e.g.
** moving towards the front).
**
** This function is only interested in siblings of the surface that we've moved.  Also, any intersecting surfaces need
** to share the same bitmap surface.
**
** All coordinates are expressed in absolute format.
*/

static void invalidate_overlap(extSurface *Self, const SURFACELIST &list, int OldIndex, int Index,
   const ClipRectangle &Area, objBitmap *Bitmap)
{
   kt::Log log(__FUNCTION__);
   int j;

   log.traceBranch("%dx%d %dx%d, Between %d to %d", Area.Left, Area.Top, Area.width(), Area.height(), OldIndex, Index);

   if (list[Index].transparent() or list[Index].invisible()) {
      return;
   }

   for (auto i=OldIndex; i < Index; i++) {
      // A redraw is required for:
      //   Any volatile regions that were in front of our surface prior to the move-to-front (by moving to the front, their background has been changed).
      //   Areas of our surface that were obscured by surfaces that also shared our bitmap space.

      if (list[i].invisible()) goto skipcontent;
      if (list[i].transparent()) continue;

      if (list[i].BitmapID != list[Index].BitmapID) {
         // We're not using the deep scanning technique, so use check_volatile() to thoroughly determine if the surface is volatile or not.

         if (check_volatile(list, i)) {
            // The surface is volatile and on a different bitmap - it will have to be redrawn
            // because its background has changed.  It will not have to be exposed because our
            // surface is sitting on top of it.

            _redraw_surface(list[i].SurfaceID, list, i, Area.Left, Area.Top, Area.Right, Area.Bottom, IRF::NIL);
         }
         else goto skipcontent;
      }

      if ((list[i].Left < Area.Right) and (list[i].Top < Area.Bottom) and (list[i].Right > Area.Left) and (list[i].Bottom > Area.Top)) {
         // Intersecting surface discovered.  What we do now is keep scanning for other overlapping siblings to restrict our
         // exposure space (so that we don't repeat expose drawing for overlapping areas).  Then we call RedrawSurface() to draw the exposed area.

         auto listx      = list[i].Left;
         auto listy      = list[i].Top;
         auto listright  = list[i].Right;
         auto listbottom = list[i].Bottom;

         if (Area.Left > listx)        listx      = Area.Left;
         if (Area.Top > listy)         listy      = Area.Top;
         if (Area.Bottom < listbottom) listbottom = Area.Bottom;
         if (Area.Right < listright)   listright  = Area.Right;

         _redraw_surface(Self->UID, list, i, listx, listy, listright, listbottom, IRF::NIL);
      }

skipcontent:
      // Skip past any children of the overlapping object

      for (j=i+1; (j < int(list.size())) and (list[j].Level > list[i].Level); j++);
      i = j - 1;
   }
}

//********************************************************************************************************************
// Handler for the display being resized - hooks into Display->ResizeFeedback
// Clients can get resize notifications by subscribing to the Redimension action

static void display_resized(OBJECTID DisplayID, int X, int Y, int Width, int Height)
{
   OBJECTID surface_id = GetOwnerID(DisplayID);

   if (kt::ScopedObjectLock<extSurface> surface(surface_id, 4000); surface.granted()) {
      if (surface->classID() IS CLASSID::SURFACE) {
         if ((surface->FixedWidth != Width) or (surface->FixedHeight != Height)) {
            surface->X = Unit(X);
            surface->Y = Unit(Y);
            surface->Width = Unit(Width);
            surface->Height = Unit(Height);
            surface->FixedX = X;
            surface->FixedY = Y;
            UpdateSurfaceRecord(*surface);

            acRedimension(*surface, surface->FixedX, surface->FixedY, 0, Width, Height, 0);
         }
         else if ((X != surface->FixedX) or (Y != surface->FixedY)) {
            // Window is being moved to a new position only.  Notifying subscribers with no forced redraw is sufficient
            surface->X = Unit(X);
            surface->Y = Unit(Y);
            surface->FixedX = X;
            surface->FixedY = Y;
            UpdateSurfaceRecord(*surface);

            struct acRedimension redimension = { (double)X, (double)Y, 0, (double)Width, (double)Height, (double)surface->BitsPerPixel };
            NotifySubscribers(*surface, AC::Redimension, &redimension, ERR::Okay);
         }
      }
   }
}

//********************************************************************************************************************

static void notify_free_parent(OBJECTPTR Object, ACTIONID ActionID, ERR Result)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   // Free ourselves in advance if our parent is in the process of being killed.  This causes a chain reaction
   // that results in a clean deallocation of the surface hierarchy.

   Self->Flags &= ~RNF::VISIBLE;
   UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
   if (Self->defined(NF::LOCAL)) SendMessage(MSGID::FREE, MSF::NIL, &Self->UID, sizeof(Self->UID)); // If the object is a child of something, give the parent object time to do the deallocation itself
   else FreeResource(Self);
}

static void notify_free_callback(OBJECTPTR Object, ACTIONID ActionID, ERR Result)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   for (auto callback = Self->Callback.begin(); callback != Self->Callback.end(); ) {
      if (callback->isScript() and (callback->Context->UID IS Object->UID)) {
         deref_surface_callback(*callback);
         callback = Self->Callback.erase(callback);
      }
      else callback++;
   }
}

static void notify_draw_display(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acDraw *Args)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   if (Self->collecting()) return;

   // Hosts will sometimes call Draw to indicate that the display has been exposed.

   log.traceBranch("Display exposure received - redrawing display.");

   if (Args) Self->exposeToDisplay(Args->X, Args->Y, Args->Width, Args->Height, EXF::CHILDREN);
   else Self->exposeToDisplay(0, 0, 20000, 20000, EXF::CHILDREN);
}

static void notify_redimension_parent(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acRedimension *Args)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   if (Self->collecting()) return;

   log.traceBranch("Redimension notification from parent #%d, currently %dx%d,%dx%d.", Self->ParentID,
      Self->FixedX, Self->FixedY, Self->FixedWidth, Self->FixedHeight);

   // Get the width and height of our parent surface

   double parentwidth, parentheight, width, height, x, y;

   if (Self->ParentID) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      int i;
      for (i=0; (i < int(glSurfaces.size())) and (glSurfaces[i].SurfaceID != Self->ParentID); i++);
      if (i >= int(glSurfaces.size())) {
         log.warning(ERR::Search);
         return;
      }
      parentwidth  = glSurfaces[i].Width;
      parentheight = glSurfaces[i].Height;
   }
   else {
      DisplayInfo *display;
      if (!gfx::GetDisplayInfo(0, &display)) {
         parentwidth  = display->Width;
         parentheight = display->Height;
      }
      else return;
   }

   // Convert relative offsets to their fixed equivalent without changing their declared units.

   if (Self->XOffset.defined()) {
      if (Self->XOffset.scaled()) Self->FixedXO = int16_t(parentwidth * Self->XOffset);
      else Self->FixedXO = int16_t(Self->XOffset);
   }

   if (Self->YOffset.defined()) {
      if (Self->YOffset.scaled()) Self->FixedYO = int16_t(parentheight * Self->YOffset);
      else Self->FixedYO = int16_t(Self->YOffset);
   }

   // Calculate absolute width and height values

   if (Self->Width.scaled())   width = parentwidth * Self->Width;
   else if (Self->Width.defined()) width = Self->Width;
   else if (Self->XOffset.defined()) {
      if (Self->X.scaled()) width = parentwidth - (parentwidth * Self->X) - Self->FixedXO;
      else if (Self->X.defined()) width = parentwidth - Self->X - Self->FixedXO;
      else width = parentwidth - Self->FixedXO;
   }
   else width = Self->Width;

   if (Self->Height.scaled())   height = parentheight * Self->Height;
   else if (Self->Height.defined()) height = Self->Height;
   else if (Self->YOffset.defined()) {
      if (Self->Y.scaled()) height = parentheight - (parentheight * Self->Y) - Self->FixedYO;
      else if (Self->Y.defined()) height = parentheight - Self->Y - Self->FixedYO;
      else height = parentheight - Self->FixedYO;
   }
   else height = Self->Height;

   // Calculate new coordinates

   if (Self->X.scaled()) x = parentwidth * Self->X;
   else if (Self->XOffset.defined()) x = parentwidth - Self->FixedXO - width;
   else x = Self->X;

   if (Self->Y.scaled()) y = parentheight * Self->Y;
   else if (Self->YOffset.defined()) y = parentheight - Self->FixedYO - height;
   else y = Self->Y;

   if ((Self->MaxWidth > 0) and (width > Self->MaxWidth)) {
      log.trace("Calculated width of %.0f exceeds max limit of %d", width, Self->MaxWidth);
      width = Self->MaxWidth;
   }

   if ((Self->MaxHeight > 0) and (height > Self->MaxHeight)) {
      log.trace("Calculated height of %.0f exceeds max limit of %d", height, Self->MaxHeight);
      height = Self->MaxHeight;
   }

   // Perform the resize

   if ((Self->FixedX != x) or (Self->FixedY != y) or (Self->FixedWidth != width) or
       (Self->FixedHeight != height) or (Args->Depth)) {
      acRedimension(Self, x, y, 0, width, height, Args->Depth);
   }
}

/*********************************************************************************************************************
-ACTION-
Activate: Shows a surface object on the display.

For top-level surfaces, Activate() delegates to #Show() so that the hosted display window becomes visible.  Child
surfaces are not affected by this action.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Activate(extSurface *Self)
{
   if (!Self->ParentID) acShow(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddCallback: Inserts a function hook into the drawing process of a surface object.

The AddCallback() method installs a draw callback for custom rendering directly into a surface.  During a redraw,
callbacks are invoked in subscription order and receive the target @Bitmap for the surface.

The C/C++ callback prototype is `ERR Function(APTR Context, objSurface *Surface, objBitmap *Bitmap, APTR Meta)`.
The Tiri callback prototype is `function draw(Surface, Bitmap)`.

Callbacks may draw to the bitmap as they would to any other @Bitmap.  Use the Surface #Width and #Height fields to
determine the drawable area, and respect the bitmap clipping region when writing pixels directly.

Calling AddCallback() with a callback that is already registered for the same object moves that callback to the end of
the callback list, making it run after the callbacks that remain before it.

-INPUT-
func Callback: Callback routine to insert or move in the draw callback list.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, callback-held
-END-

*********************************************************************************************************************/

static ERR SURFACE_AddCallback(extSurface *Self, struct drw::AddCallback *Args)
{
   kt::Log log;
   bool new_callback = true;
   bool retained_callback = false;

   if ((not Args) or (not Args->Callback.defined())) return log.warning(ERR::NullArgs);

   auto consume_callback = kt::Defer([&]() {
      if (not retained_callback) Args->Callback.consume();
   });

   log.msg("Count: %d", int(Self->Callback.size()));

   // Check if the subscription is already registered

   auto callback = Self->Callback.begin();
   for (; callback != Self->Callback.end(); callback++) {
      if (callback->Context IS Args->Callback.Context) {
         if ((callback->isC()) and (Args->Callback.isC())) {
            if (callback->Routine IS Args->Callback.Routine) break;
         }
         else if ((callback->isScript()) and (Args->Callback.isScript())) {
            if (callback->ProcedureID IS Args->Callback.ProcedureID) break;
         }
      }
   }

   if (callback != Self->Callback.end()) {
      log.trace("Moving existing subscription to foreground.");
      deref_surface_callback(*callback);
      Self->Callback.erase(callback);
      new_callback = false;
   }

   Self->Callback.push_back(Args->Callback);

   if (new_callback and (Args->Callback.Type IS CALL::SCRIPT)) {
      SubscribeAction(Args->Callback.Context, AC::Free, C_FUNCTION(notify_free_callback));
   }

   Args->Callback.pin();
   retained_callback = true;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Disable: Disables a surface object.

Disabled surfaces remain visible but cannot accept user focus through #Focus().
-END-
*********************************************************************************************************************/

static ERR SURFACE_Disable(extSurface *Self)
{
   Self->Flags |= RNF::DISABLED;
   UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Enable: Enables a disabled surface object.

Enable() clears the disabled state so that the surface can receive focus and normal interaction again.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Enable(extSurface *Self)
{
   Self->Flags &= ~RNF::DISABLED;
   UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Focus: Changes the primary user focus to the surface object.

Focus() makes the surface the primary focus target and notifies the affected focus subscribers.  Focus is propagated
through the surface hierarchy so that parent surfaces also record inherited focus.

The request is ignored if the surface is disabled, marked with `RNF::NO_FOCUS`, or outside the active modal surface.
If `RNF::IGNORE_FOCUS` is set, the focus request is forwarded to the parent surface instead.
-END-
*********************************************************************************************************************/

static int64_t glLastFocusTime = 0;

static ERR SURFACE_Focus(extSurface *Self)
{
   kt::Log log;

   if (Self->disabled()) return ERR::Okay|ERR::Notified;

   if (auto msg = GetActionMsg(AC::Focus)) {
      // This is a message - in which case it could have been delayed and thus superseded by a more recent message.

      if (msg->Time < glLastFocusTime) {
         FOCUSMSG("Ignoring superseded focus message.");
         return ERR::Okay|ERR::Notified;
      }
   }

   if ((Self->Flags & RNF::IGNORE_FOCUS) != RNF::NIL) {
      FOCUSMSG("Focus propagated to parent (IGNORE_FOCUS flag set).");
      kt::ScopedObjectLock focus(Self->ParentID);
      if (focus.granted()) acFocus(*focus);
      glLastFocusTime = PreciseTime();
      return ERR::Okay|ERR::Notified;
   }

   if ((Self->Flags & RNF::NO_FOCUS) != RNF::NIL) {
      FOCUSMSG("Focus cancelled (NO_FOCUS flag set).");
      glLastFocusTime = PreciseTime();
      return ERR::Okay|ERR::Notified;
   }

   FOCUSMSG("Focussing...  HasFocus: %c", (Self->hasFocus()) ? 'Y' : 'N');

   if (auto modal = gfx::GetModalSurface()) {
      if (modal != Self->UID) {
         ERR error;
         error = gfx::CheckIfChild(modal, Self->UID);

         if ((error != ERR::True) and (error != ERR::LimitedSuccess)) {
            // Focussing is not OK - surface is out of the modal's scope
            log.warning("Surface #%d is not within modal #%d's scope.", Self->UID, modal);
            glLastFocusTime = PreciseTime();
            return ERR::Failed|ERR::Notified;
         }
      }
   }

   const std::lock_guard<std::recursive_mutex> lock(glFocusLock);

   // Return immediately if this surface object already has the -primary- focus

   if (Self->hasFocus() and (glFocusList[0] IS Self->UID)) {
      FOCUSMSG("Surface already has the primary focus.");
      glLastFocusTime = PreciseTime();
      return ERR::Okay|ERR::Notified;
   }

   int j;
   std::vector<OBJECTID> lostfocus;
   glFocusList.clear();

   {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

      int surface_index;
      if ((surface_index = find_surface_list(Self)) IS -1) {
         // This is not a critical failure as child surfaces can be expected to disappear from the surface list
         // during the free process.

         glLastFocusTime = PreciseTime();
         return ERR::Failed|ERR::Notified;
      }

      // Build the new focus chain in a local focus list.  Also also reset the HAS_FOCUS flag.  Surfaces that have
      // lost the focus go in the lostfocus list.

      // Starting from the end of the list, everything leading towards the target surface will need to lose the focus.

      for (j=glSurfaces.size()-1; j > surface_index; j--) {
         if (glSurfaces[j].hasFocus()) {
            lostfocus.push_back(glSurfaces[j].SurfaceID);
            glSurfaces[j].dropFocus();
         }
      }

      // The target surface and all its parents will need to gain the focus

      auto surface_id = Self->UID;
      for (j=surface_index; j >= 0; j--) {
         if (glSurfaces[j].SurfaceID != surface_id) {
            if (glSurfaces[j].hasFocus()) {
               lostfocus.push_back(glSurfaces[j].SurfaceID);
               glSurfaces[j].dropFocus();
            }
         }
         else {
            glSurfaces[j].Flags |= RNF::HAS_FOCUS;
            glFocusList.push_back(surface_id);
            surface_id = glSurfaces[j].ParentID;
            if (!surface_id) {
               j--;
               break; // Break out of the loop when there are no more parents left
            }
         }
      }

      // This next loop is important for hosted environments where multiple windows are active.  It ensures that
      // surfaces contained by other windows also lose the focus.

      while (j >= 0) {
         if (glSurfaces[j].hasFocus()) {
            lostfocus.push_back(glSurfaces[j].SurfaceID);
            glSurfaces[j].dropFocus();
         }
         j--;
      }
   }

   // Send a Focus action to all parent surface objects in our generated focus list.

   auto it = glFocusList.begin() + 1; // Skip Self
   while (it != glFocusList.end()) {
      kt::ScopedObjectLock<objSurface> obj(*it);
      if (obj.granted()) obj->inheritedFocus(Self->UID, Self->Flags);
      it++;
   }

   // Send out LostFocus actions to all objects that do not intersect with the new focus chain.

   for (auto &id : lostfocus) {
      kt::ScopedObjectLock obj(id);
      if (obj.granted()) acLostFocus(*obj);
   }

   // Send a global focus event to all listeners.  The list consists of two sections with the focus-chain
   // placed first, then the lost-focus chain.

   int event_size = sizeof(evFocus) + (glFocusList.size() * sizeof(OBJECTID)) + (lostfocus.size() * sizeof(OBJECTID));
   auto buffer = std::make_unique<int8_t[]>(event_size);
   auto ev = (evFocus *)(buffer.get());
   ev->EventID        = EVID_GUI_SURFACE_FOCUS;
   ev->TotalWithFocus = glFocusList.size();
   ev->TotalLostFocus = lostfocus.size();

   OBJECTID *outlist = &ev->FocusList[0];
   int o = 0;
   for (auto &id : glFocusList) outlist[o++] = id;
   for (auto &id : lostfocus) outlist[o++] = id;
   BroadcastEvent(ev, event_size);

   if (Self->hasFocus()) {
      // Return without notification as we already have the focus

      if (Self->RevertFocusID) {
         kt::ScopedObjectLock focus(Self->RevertFocusID);
         Self->RevertFocusID = 0;
         if (focus.granted()) acFocus(*focus);
      }

      glLastFocusTime = PreciseTime();
      return ERR::Okay|ERR::Notified;
   }
   else {
      Self->Flags |= RNF::HAS_FOCUS;
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      // Focussing on the display window is important in hosted environments

      if (Self->DisplayID) {
         kt::ScopedObjectLock display(Self->DisplayID);
         if (display.granted()) acFocus(*display);
      }

      if (Self->RevertFocusID) {
         kt::ScopedObjectLock focus(Self->RevertFocusID);
         Self->RevertFocusID = 0;
         if (focus.granted()) acFocus(*focus);
      }

      glLastFocusTime = PreciseTime();
      return ERR::Okay;
   }
}

//********************************************************************************************************************

extSurface::~extSurface()
{
   if (RedrawTimer) UpdateTimer(RedrawTimer, 0);

   deref_surface_callbacks(Callback);
   Callback.clear();

   if (ParentID) {
      if (kt::ScopedObjectLock<extSurface> parent(ParentID, 5000); parent.granted()) {
         UnsubscribeAction(*parent, AC::NIL);
         if (transparent()) Action(drw::RemoveCallback::id, this, nullptr);
      }
   }

   acHide(this);

   // Remove any references to this surface object from the global surface list

   untrack_layer(UID);

   if ((!ParentID) and (DisplayID)) FreeResource(DisplayID);

   if ((BufferID) and ((!BitmapOwnerID) or (BitmapOwnerID IS UID))) {
      if (Bitmap) { ReleaseObject(Bitmap); Bitmap = nullptr; }
      FreeResource(BufferID);
   }

   // Give the focus to the parent if our object has the primary focus.  Do not apply this technique to surface objects
   // acting as windows, as the window class has its own focus management code.

   if (hasFocus() and Owner) {
      if (ParentID) {
         kt::ScopedObjectLock focus(ParentID);
         if (focus.granted()) acFocus(*focus);
      }
   }

   if ((Flags & RNF::AUTO_QUIT) != RNF::NIL) {
      kt::Log().msg("Posting a quit message due to use of AUTOQUIT.");
      SendMessage(MSGID::QUIT, MSF::NIL, nullptr, 0);
   }

   if (InputHandle) gfx::UnsubscribeInput(InputHandle);

   {
      const std::lock_guard<std::recursive_mutex> lock(glWindowHookLock);
      for (auto it = glWindowHooks.begin(); it != glWindowHooks.end();) {
         if (it->first.SurfaceID IS UID) {
            release_display_callback(it->second);
            it = glWindowHooks.erase(it);
         }
         else it++;
      }
   }
}

/*********************************************************************************************************************
-ACTION-
Hide: Hides a surface object from the display.

Hide() clears the visible state.  For top-level surfaces it hides the hosted display window; for child surfaces it
invalidates and exposes the covered parent area so that the background is redrawn.  Hiding a modal surface also
restores the previous modal surface, or clears modal mode if there is no previous surface.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Hide(extSurface *Self)
{
   kt::Log log;

   log.traceBranch();

   if (Self->invisible()) return ERR::Okay|ERR::Notified;

   if (!Self->ParentID) {
      Self->Flags &= ~RNF::VISIBLE; // Important to switch off visibliity before Hide(), otherwise a false redraw will occur.
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      kt::ScopedObjectLock display(Self->DisplayID);
      if (display.granted()) if (acHide(*display) != ERR::Okay) return ERR::Failed;
   }
   else {
      // Mark this surface object as invisible, then invalidate the region it was covering in order to have the background redrawn.

      Self->Flags &= ~RNF::VISIBLE;
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      if (Self->BitmapOwnerID != Self->UID) {
         RedrawSurface(Self->ParentID, Self->FixedX, Self->FixedY, Self->FixedWidth, Self->FixedHeight,
            IRF::RELATIVE);
      }
      gfx::ExposeSurface(Self->ParentID, Self->FixedX, Self->FixedY, Self->FixedWidth, Self->FixedHeight,
         EXF::CHILDREN|EXF::REDRAW_VOLATILE);
   }

   // Check if the surface is modal, if so, switch it off

   if (Self->PrevModalID) {
      gfx::SetModalSurface(Self->PrevModalID);
      Self->PrevModalID = 0;
   }
   else if (gfx::GetModalSurface() IS Self->UID) {
      log.msg("Surface is modal, switching off modal mode.");
      gfx::SetModalSurface(0);
   }

   refresh_pointer(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InheritedFocus: Private

Private

-INPUT-
oid FocusID: Private
int(RNF) Flags: Private

-ERRORS-
Okay
Notified

-TAGS-
mutates-object, private
-END-

*********************************************************************************************************************/

static ERR SURFACE_InheritedFocus(extSurface *Self, struct drw::InheritedFocus *Args)
{
   if (auto msg = GetActionMsg(drw::InheritedFocus::id)) {
      // This is a message - in which case it could have been delayed and thus superseded by a more recent message.

      if (msg->Time < glLastFocusTime) {
         FOCUSMSG("Ignoring superseded focus message.");
         return ERR::Okay|ERR::Notified;
      }
   }

   glLastFocusTime = PreciseTime();

   if (Self->hasFocus()) {
      FOCUSMSG("This surface already has focus.");
      return ERR::Okay;
   }
   else {
      FOCUSMSG("Object has received the focus through inheritance.");

      Self->Flags |= RNF::HAS_FOCUS;

      //UpdateSurfaceField(Self, Flags); // Not necessary because SURFACE_Focus sets the surfacelist

      NotifySubscribers(Self, AC::Focus, nullptr, ERR::Okay);
      return ERR::Okay;
   }
}

//********************************************************************************************************************

static ERR SURFACE_Init(extSurface *Self)
{
   kt::Log log;

   bool require_store = false;
   OBJECTID parent_bitmap = 0;
   OBJECTID bitmap_owner  = 0;

   if (!Self->RootID) Self->RootID = Self->UID;

   if (Self->isCursor()) Self->Flags |= RNF::STICK_TO_FRONT;

   // If no parent surface is set, check if the client has set the FULL_SCREEN flag.  If not, try to give the
   // surface a parent.

   if ((!Self->ParentID) and (gfx::GetDisplayType() IS DT::NATIVE)) {
      if ((Self->Flags & RNF::FULL_SCREEN) IS RNF::NIL) {
         if (FindObject("desktop", CLASSID::SURFACE, &Self->ParentID) != ERR::Okay) {
            if (!glSurfaces.empty()) Self->ParentID = glSurfaces[0].SurfaceID;
         }
      }
   }

   ERR error = ERR::Okay;
   if (Self->ParentID) {
      kt::ScopedObjectLock<extSurface> parent(Self->ParentID, 3000);
      if (!parent.granted()) return ERR::AccessObject;

      log.trace("Initialising surface to parent #%d.", Self->ParentID);

      // If the parent has the ROOT flag set, we have to inherit whatever root layer that the parent is using, as well
      // as the PRECOPY and/or AFTERCOPY and opacity flags if they are set.

      if ((parent->Type & RT::ROOT) != RT::NIL) { // The window class can set the ROOT type
         Self->Type |= RT::ROOT;
         if (Self->RootID IS Self->UID) {
            Self->InheritedRoot = true;
            Self->RootID = parent->RootID; // Inherit the parent's root layer
         }
      }

      // Subscribe to the surface parent's Resize and Redimension actions

      SubscribeAction(*parent, AC::Free, C_FUNCTION(notify_free_parent));
      SubscribeAction(*parent, AC::Redimension, C_FUNCTION(notify_redimension_parent));

      // If the surface object is transparent, subscribe to the Draw action of the parent object.

      if (Self->transparent()) {
         parent->addCallback(C_FUNCTION(draw_region));

         // Turn off flags that should never be combined with transparent surfaces.
         Self->Flags &= ~(RNF::PRECOPY|RNF::AFTER_COPY|RNF::COMPOSITE);
         Self->Colour.Alpha = 0;
      }

      // Recalculate coordinates if offsets are used

      if (Self->XOffset.defined()) Self->setXOffset(Self->XOffset);
      if (Self->YOffset.defined()) Self->setYOffset(Self->YOffset);

      if (Self->X.scaled())       Self->setX(Self->X);
      if (Self->Y.scaled())       Self->setY(Self->Y);
      if (Self->Width.scaled())   Self->setWidth(Self->Width);
      if (Self->Height.scaled())  Self->setHeight(Self->Height);

      int fixed_x = Self->X.scaled() ? Self->FixedX : (Self->X.defined() ? int(Self->X) : 0);
      int fixed_y = Self->Y.scaled() ? Self->FixedY : (Self->Y.defined() ? int(Self->Y) : 0);
      int fixed_width = Self->Width.scaled() ? Self->FixedWidth : (Self->Width.defined() ? int(Self->Width) : 0);
      int fixed_height = Self->Height.scaled() ? Self->FixedHeight : (Self->Height.defined() ? int(Self->Height) : 0);

      if (not Self->Width.defined()) {
         if (Self->XOffset.defined()) fixed_width = parent->FixedWidth - fixed_x - Self->FixedXO;
         else {
            fixed_width = 20;
            Self->Width = Unit(fixed_width);
         }
      }

      if (not Self->Height.defined()) {
         if (Self->YOffset.defined()) fixed_height = parent->FixedHeight - fixed_y - Self->FixedYO;
         else {
            fixed_height = 20;
            Self->Height = Unit(fixed_height);
         }
      }

      if (fixed_width < Self->MinWidth) fixed_width = Self->MinWidth;
      if (fixed_height < Self->MinHeight) fixed_height = Self->MinHeight;
      if ((Self->MaxWidth > 0) and (fixed_width > Self->MaxWidth)) fixed_width = Self->MaxWidth;
      if ((Self->MaxHeight > 0) and (fixed_height > Self->MaxHeight)) fixed_height = Self->MaxHeight;

      if ((not Self->X.defined()) and Self->XOffset.defined() and Self->Width.defined()) {
         fixed_x = parent->FixedWidth - Self->FixedXO - fixed_width;
      }

      if ((not Self->Y.defined()) and Self->YOffset.defined() and Self->Height.defined()) {
         fixed_y = parent->FixedHeight - Self->FixedYO - fixed_height;
      }

      if (Self->Width.defined() and (not Self->Width.scaled())) Self->Width = Unit(fixed_width);
      if (Self->Height.defined() and (not Self->Height.scaled())) Self->Height = Unit(fixed_height);

      Self->setFixedArea(fixed_x, fixed_y, fixed_width, fixed_height);

      Self->DisplayID     = parent->DisplayID;
      Self->DisplayWindow = parent->DisplayWindow;
      parent_bitmap       = parent->BufferID;
      bitmap_owner        = parent->BitmapOwnerID;

      // If the parent is a host, all child surfaces within it must get their own bitmap space.
      // If not, managing layered surfaces between processes becomes more difficult.

      if ((parent->Flags & RNF::HOST) != RNF::NIL) require_store = true;
   }
   else {
      log.trace("This surface object will be display-based.");

      // Turn off any flags that may not be used for the top-most layer

      Self->Flags &= ~(RNF::TRANSPARENT|RNF::PRECOPY|RNF::AFTER_COPY);

      SCR scrflags = SCR::NIL;

      if ((Self->Type & RT::ROOT) != RT::NIL) {
         gfx::SetHostOption(HOST::TASKBAR, 1);
         gfx::SetHostOption(HOST::TRAY_ICON, 0);
      }
      else switch(Self->WindowType) {
         default: // SWIN::HOST
            log.trace("Enabling standard hosted window mode.");
            gfx::SetHostOption(HOST::TASKBAR, 1);
            break;

         case SWIN::TASKBAR:
            log.trace("Enabling borderless taskbar based surface.");
            scrflags |= SCR::BORDERLESS; // Stop the display from creating a host window for the surface
            if ((Self->Flags & RNF::HOST) != RNF::NIL) scrflags |= SCR::MAXIMISE;
            gfx::SetHostOption(HOST::TASKBAR, 1);
            break;

         case SWIN::ICON_TRAY:
            log.trace("Enabling borderless icon-tray based surface.");
            scrflags |= SCR::BORDERLESS; // Stop the display from creating a host window for the surface
            if ((Self->Flags & RNF::HOST) != RNF::NIL) scrflags |= SCR::MAXIMISE;
            gfx::SetHostOption(HOST::TRAY_ICON, 1);
            break;

         case SWIN::NONE:
            log.trace("Enabling borderless, presence-less surface.");
            scrflags |= SCR::BORDERLESS; // Stop the display from creating a host window for the surface
            if ((Self->Flags & RNF::HOST) != RNF::NIL) scrflags |= SCR::MAXIMISE;
            gfx::SetHostOption(HOST::TASKBAR, 0);
            gfx::SetHostOption(HOST::TRAY_ICON, 0);
            break;
      }

      if (gfx::GetDisplayType() IS DT::NATIVE) Self->Flags &= ~(RNF::COMPOSITE);

      if (((gfx::GetDisplayType() IS DT::WINGDI) or (gfx::GetDisplayType() IS DT::X11)) and ((Self->Flags & RNF::HOST) != RNF::NIL)) {
         if (glpMaximise) scrflags |= SCR::MAXIMISE;
         if (glpFullScreen) scrflags |= SCR::MAXIMISE|SCR::BORDERLESS;
      }

      if (not Self->Width.defined()) Self->Width = Unit(glpDisplayWidth);
      if (not Self->Height.defined()) Self->Height = glpDisplayHeight;

      if (not Self->X.defined()) {
         if ((Self->Flags & RNF::HOST) != RNF::NIL) Self->X = Unit(0);
         else Self->X = Unit(glpDisplayX);
      }

      if (not Self->Y.defined()) {
         if ((Self->Flags & RNF::HOST) != RNF::NIL) Self->Y = Unit(0);
         else Self->Y = Unit(glpDisplayY);
      }

      if ((Self->Width < 10) or (Self->Height < 6)) {
         Self->Width  = Unit(640);
         Self->Height = Unit(480);
      }

      if (Self->Width  < Self->MinWidth)  Self->Width  = Unit(Self->MinWidth);
      if (Self->Height < Self->MinHeight) Self->Height = Unit(Self->MinHeight);
      if ((Self->MaxWidth > 0) and (Self->Width  > Self->MaxWidth))  Self->Width  = Unit(Self->MaxWidth);
      if ((Self->MaxHeight > 0) and (Self->Height > Self->MaxHeight)) Self->Height = Unit(Self->MaxHeight);

      Self->setFixedArea(int(Self->X), int(Self->Y), int(Self->Width), int(Self->Height));

      if ((Self->Flags & RNF::STICK_TO_FRONT) != RNF::NIL) gfx::SetHostOption(HOST::STICK_TO_FRONT, 1);
      else gfx::SetHostOption(HOST::STICK_TO_FRONT, 0);

      if ((Self->Flags & RNF::COMPOSITE) != RNF::NIL) scrflags |= SCR::COMPOSITE;

      OBJECTID id, pop_display = 0;
      CSTRING name = FindObject("SystemDisplay", CLASSID::NIL, &id) != ERR::Okay ? "SystemDisplay" : (CSTRING)nullptr;

      if (Self->PopOverID) {
         if (kt::ScopedObjectLock<extSurface> popsurface(Self->PopOverID, 2000); popsurface.granted()) {
            pop_display = popsurface->DisplayID;

            if (!pop_display) log.warning("Surface #%d doesn't have a display ID for pop-over.", Self->PopOverID);
         }
      }

      // For hosted displays:  On initialisation, the X and Y fields reflect the position at which the window will be
      // opened on the host desktop.  However, hosted surfaces operate on the absolute coordinates of client regions
      // and are ignorant of window frames, so we read the X, Y fields back from the display after initialisation (the
      // display will adjust the coordinates to reflect the absolute position of the surface on the desktop).

      if (auto display = objDisplay::create::local(
            fl::Name(name),
            fl::X(Self->FixedX), fl::Y(Self->FixedY),
            fl::Width(Self->FixedWidth), fl::Height(Self->FixedHeight),
            fl::BitsPerPixel(glpDisplayDepth),
            fl::RefreshRate(glpRefreshRate),
            fl::Flags(scrflags),
            fl::Opacity(Self->Opacity),
            fl::PopOver(pop_display),
            fl::WindowHandle((APTR)Self->DisplayWindow))) { // Sometimes a window may be preset, e.g. for a web plugin

         display->setGamma(glpGammaRed, glpGammaGreen, glpGammaBlue, GMF::SAVE);
         gfx::SetHostOption(HOST::TASKBAR, 1); // Reset display system so that windows open with a taskbar by default

         // Get the true coordinates of the client area of the surface

         Self->setFixedArea(display->X, display->Y, display->Width, display->Height);

         // Configure sizing hints for the display.

         if ((Self->MaxWidth > 0) or (Self->MaxHeight > 0) or (Self->MinWidth > 0) or (Self->MinHeight > 0)) {
            int mxW = (Self->MaxWidth > 0)  ? Self->MaxWidth  : 0;
            int mxH = (Self->MaxHeight > 0) ? Self->MaxHeight : 0;
            int mnW = (Self->MinWidth > 0)  ? Self->MinWidth  : 0;
            int mnH = (Self->MinHeight > 0) ? Self->MinHeight : 0;
            display->sizeHints(mnW, mnH, mxW, mxH, (Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL);
         }
         else if ((Self->Flags & RNF::ASPECT_RATIO) != RNF::NIL) {
            // When aspect ratio is used without min & max dimensions, the current width & height is used to set the
            // min/max values.

            int gcd = std::gcd(Self->FixedWidth, Self->FixedHeight);
            Self->MinWidth  = Self->FixedWidth / gcd;
            Self->MinHeight = Self->FixedHeight / gcd;
            Self->MaxWidth  = Self->FixedWidth * 10;
            Self->MaxHeight = Self->FixedHeight * 10;

            if (Self->MinWidth < 140) {
               int rescale = 140 / Self->MinWidth;
               Self->MinWidth *= rescale;
               Self->MinHeight *= rescale;
            }

            if (Self->MinHeight < 10) {
               int rescale = 10 / Self->MinHeight;
               Self->MinWidth *= rescale;
               Self->MinHeight *= rescale;
            }

            display->sizeHints(Self->MinWidth, Self->MinHeight, Self->MaxWidth, Self->MaxHeight, true);
         }

         acFlush(display);

         // For hosted environments, record the window handle (NB: this is doubling up the display handle, we should
         // just make the window handle a virtual field so that we don't need a permanent record of it).

         display->getWindowHandle(Self->DisplayWindow);

         #ifdef _WIN32
            winSetSurfaceID(Self->DisplayWindow, Self->UID);
         #endif

         // Subscribe to Redimension notifications if the display is hosted.  Also subscribe to Draw because this
         // can be used by the host to notify of window exposures.

         if (Self->DisplayWindow) {
            display->setResizeFeedback(C_FUNCTION(display_resized));

            SubscribeAction(display, AC::Draw, C_FUNCTION(notify_draw_display));
         }

         Self->DisplayID = display->UID;
         error = ERR::Okay;
      }
      else return log.warning(ERR::CreateObject);
   }

   // Allocate a backing store if this is a host object, or the parent is foreign, or we are the child of a host object
   // (check made earlier), or surface object is masked.

   if (!Self->ParentID) require_store = true;
   else if ((Self->Flags & (RNF::PRECOPY|RNF::COMPOSITE|RNF::AFTER_COPY|RNF::CURSOR)) != RNF::NIL) require_store = true;
   else {
      if (Self->BitsPerPixel >= 8) {
         DisplayInfo *info;
         if (!gfx::GetDisplayInfo(Self->DisplayID, &info)) {
            if (info->BitsPerPixel != Self->BitsPerPixel) require_store = true;
         }
      }
   }

   if (Self->transparent()) require_store = false;

   if (require_store) {
      Self->BitmapOwnerID = Self->UID;

      kt::ScopedObjectLock<objDisplay> display(Self->DisplayID, 3000);

      if (display.granted()) {
         auto memflags = MEM::NIL;

         if ((Self->Flags & RNF::VIDEO) != RNF::NIL) {
            // If acceleration is available then it is OK to create the buffer in video RAM.

            if ((display->Flags & SCR::NO_ACCELERATION) IS SCR::NIL) memflags = MEM::TEXTURE;
         }

         int bpp;
         if ((Self->Flags & RNF::COMPOSITE) != RNF::NIL) {
            // If dynamic compositing will be used then we must have an alpha channel
            bpp = 32;
         }
         else if (Self->BitsPerPixel) {
            bpp = Self->BitsPerPixel; // BPP has been preset by the client
            log.msg("Preset depth of %d bpp detected.", bpp);
         }
         else bpp = display->Bitmap->BitsPerPixel;

         if (auto bitmap = objBitmap::create::local(
               fl::BitsPerPixel(bpp), fl::Width(Self->FixedWidth), fl::Height(Self->FixedHeight),
               fl::DataFlags(memflags),
               fl::Flags((((Self->Flags & RNF::COMPOSITE) != RNF::NIL) ? (BMF::ALPHA_CHANNEL|BMF::FIXED_DEPTH) : BMF::NIL)))) {

            if (Self->BitsPerPixel) bitmap->Flags |= BMF::FIXED_DEPTH; // This flag prevents automatic changes to the bit depth

            Self->BitsPerPixel  = bitmap->BitsPerPixel;
            Self->BytesPerPixel = bitmap->BytesPerPixel;
            Self->LineWidth     = bitmap->LineWidth;
            Self->Data          = bitmap->Data;
            Self->BufferID      = bitmap->UID;
            error = ERR::Okay;
         }
         else error = ERR::CreateObject;
      }
      else error = ERR::AccessObject;

      if (error != ERR::Okay) return log.warning(error);
   }
   else {
      Self->BufferID      = parent_bitmap;
      Self->BitmapOwnerID = bitmap_owner;
   }

   // If the FIXED_BUFFER option is set, pass the NEVER_SHRINK option to the bitmap

   if ((Self->Flags & RNF::FIXED_BUFFER) != RNF::NIL) {
      if (kt::ScopedObjectLock<objBitmap> bitmap(Self->BufferID, 5000); bitmap.granted()) {
         bitmap->Flags |= BMF::NEVER_SHRINK;
      }
   }

   // Track the surface object

   if (track_layer(Self) != ERR::Okay) return ERR::Failed;

   // The PopOver reference can only be managed once track_layer() has been called if this is a surface with a parent.

   if ((Self->ParentID) and (Self->PopOverID)) {
      // Ensure that the referenced surface is in front of the sibling.  Note that if we can establish that the
      // provided surface ID is not a sibling, the request is cancelled.

      OBJECTID popover_id = Self->PopOverID;
      Self->PopOverID = 0;

      Self->moveToFront();

      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
      if (auto index = find_surface_list(Self); index != -1) {
         for (int j=index; (j >= 0) and (glSurfaces[j].SurfaceID != glSurfaces[index].ParentID); j--) {
            if (glSurfaces[j].SurfaceID IS popover_id) {
               Self->PopOverID = popover_id;
               break;
            }
         }
      }

      if (!Self->PopOverID) {
         log.warning("PopOver surface #%d is not a sibling of this surface.", popover_id);
         UpdateSurfaceField(Self, &SurfaceRecord::PopOverID, Self->PopOverID);
      }
   }

   // Move the surface object to the back of the surface list when stick-to-back is enforced.

   if ((Self->Flags & RNF::STICK_TO_BACK) != RNF::NIL) acMoveToBack(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
LostFocus: Informs a surface object that it has lost the user focus.

LostFocus() clears the `RNF::HAS_FOCUS` flag when the surface currently holds focus.  If the surface has already lost
focus, the action returns without further notification.
-END-
*********************************************************************************************************************/

static ERR SURFACE_LostFocus(extSurface *Self)
{
#if 0
   if (auto msg = GetActionMsg()) {
      // This is a message - in which case it could have been delayed and thus superseded by a more recent call.

      if (msg->Time < glLastFocusTime) {
         FOCUSMSG("Ignoring superseded focus message.");
         return ERR::Okay|ERR::Notified;
      }
   }

   glLastFocusTime = PreciseTime();
#endif

   if (Self->hasFocus()) {
      Self->Flags &= ~RNF::HAS_FOCUS;
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
      return ERR::Okay;
   }
   else return ERR::Okay | ERR::Notified;
}

/*********************************************************************************************************************

-METHOD-
Minimise: For hosted surfaces only, this method will minimise the surface to an icon.

If a surface is hosted in a desktop window, calling Minimise() performs the host platform's default minimise action
on that window.  On Microsoft Windows, this normally minimises the window to the taskbar.

Calling Minimise() on a surface that is already in the minimised state may result in the host window being restored to
the desktop.  This behaviour is platform dependent and should be manually tested to confirm its reliability on the
host platform.

-TAGS-
blocking, mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_Minimise(extSurface *Self)
{
   if (Self->DisplayID) {
      kt::ScopedObjectLock<objDisplay> display(Self->DisplayID);
      if (display.granted()) display->minimise();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves a surface object to a new display position.

Move() applies relative X and Y deltas to the surface.  The request honours movement restrictions such as
`RNF::STICKY`, `RNF::NO_HORIZONTAL`, `RNF::NO_VERTICAL` and the configured movement limits.  Child surfaces are
clamped within their parent surface when limits are active.

Queued Move() requests for the same surface may be combined before drawing occurs.  After a successful move,
redimension subscribers are notified with the surface's updated position and size.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Move(extSurface *Self, struct acMove *Args)
{
   kt::Log log;
   struct acMove move;

   if (!Args) return log.warning(ERR::NullArgs)|ERR::Notified;

   // Check if other move messages are queued for this object - if so, do not do anything until the final message is
   // reached.
   //
   // NOTE: This has a downside if the surface object is being fed a series of move messages for the purposes of
   // scrolling from one point to another.  Potentially the user may not see the intended effect or witness erratic
   // response times.

   int index = 0;
   uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage) + sizeof(struct acMove)];
   while (!ScanMessages(&index, MSGID::ACTION, msgbuffer, sizeof(msgbuffer))) {
      auto action = (ActionMessage *)(msgbuffer + sizeof(Message));

      if ((action->ActionID IS AC::MoveToPoint) and (action->ObjectID IS Self->UID)) {
         return ERR::Okay|ERR::Notified;
      }
      else if ((action->ActionID IS AC::Move) and (action->SendArgs IS true) and
               (action->ObjectID IS Self->UID)) {
         auto msgmove = (struct acMove *)(action + 1);
         msgmove->DeltaX += Args->DeltaX;
         msgmove->DeltaY += Args->DeltaY;
         msgmove->DeltaZ += Args->DeltaZ;

         UpdateMessage(((Message *)msgbuffer)->UID, MSGID::NIL, action, sizeof(ActionMessage) + sizeof(struct acMove));

         return ERR::Okay|ERR::Notified;
      }
   }

   if ((Self->Flags & RNF::STICKY) != RNF::NIL) return ERR::Failed|ERR::Notified;

   if ((Self->Flags & RNF::NO_HORIZONTAL) != RNF::NIL) move.DeltaX = 0;
   else move.DeltaX = Args->DeltaX;

   if ((Self->Flags & RNF::NO_VERTICAL) != RNF::NIL) move.DeltaY = 0;
   else move.DeltaY = Args->DeltaY;

   move.DeltaZ = 0;

   // If there isn't any movement, return immediately

   if ((move.DeltaX < 1) and (move.DeltaX > -1) and (move.DeltaY < 1) and (move.DeltaY > -1)) {
      return ERR::Failed|ERR::Notified;
   }

   move_layer(Self, Self->FixedX + move.DeltaX, Self->FixedY + move.DeltaY);

   log.traceBranch("Sending redimension notifications");
   struct acRedimension redimension = {
      (double)Self->FixedX, (double)Self->FixedY, 0, (double)Self->FixedWidth, (double)Self->FixedHeight, 0
   };
   NotifySubscribers(Self, AC::Redimension, &redimension, ERR::Okay);
   return ERR::Okay|ERR::Notified;
}

/*********************************************************************************************************************
-ACTION-
MoveToBack: Moves a surface object to the back of its container.

For child surfaces, MoveToBack() reorders the surface within its parent while preserving child hierarchy, bitmap
ownership constraints, pop-over relationships and `RNF::STICK_TO_BACK` ordering.  Top-level surfaces delegate the
request to their hosted display window.
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToBack(extSurface *Self)
{
   kt::Log log;

   if (!Self->ParentID) {
      kt::ScopedObjectLock<objDisplay> display(Self->DisplayID);
      if (display.granted()) display->moveToBack();
      return ERR::Okay|ERR::Notified;
   }

   log.branch("%s", Self->Name);

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   auto &list = glSurfaces;

   int index; // Get our position within the chain
   if ((index = find_surface_list(Self)) IS -1) return log.warning(ERR::Search)|ERR::Notified;

   OBJECTID parent_bitmap;
   if (auto i = find_parent_list(list, Self); i != -1) parent_bitmap = list[i].BitmapID;
   else parent_bitmap = 0;

   // Find the position in the list that our surface object will be moved to

   auto pos = index;
   auto level = list[index].Level;
   for (auto i=index-1; (i >= 0) and (list[i].Level >= level); i--) {
      if (list[i].Level IS level) {
         if (Self->BitmapOwnerID IS Self->UID) { // If we own an independent bitmap, we cannot move behind surfaces that are members of the parent region
            if (list[i].BitmapID IS parent_bitmap) break;
         }
         if (list[i].SurfaceID IS Self->PopOverID) break; // Do not move behind surfaces that we must stay in front of
         if (((Self->Flags & RNF::STICK_TO_BACK) IS RNF::NIL) and ((list[i].Flags & RNF::STICK_TO_BACK) != RNF::NIL)) break;
         pos = i;
      }
   }

   if (pos >= index) return ERR::Okay|ERR::Notified; // If the position is unchanged, return immediately

   move_layer_pos(list, index, pos); // Reorder the list so that our surface object is inserted at the new position

   if (Self->visible()) {
      // Redraw our background if we are volatile
      if (check_volatile(list, index)) _redraw_surface(Self->UID, list, pos, list[pos].Left, list[pos].Top, list[pos].Right, list[pos].Bottom, IRF::NIL);

      // Expose changes to the display
      _expose_surface(Self->ParentID, list, pos, Self->FixedX, Self->FixedY, Self->FixedWidth,
         Self->FixedHeight, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
   }

   refresh_pointer(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToFront: Moves a surface object to the front of its container.

For child surfaces, MoveToFront() raises the surface within its parent while preserving child hierarchy, cursor
ordering, bitmap ownership constraints, pop-over relationships and `RNF::STICK_TO_FRONT` ordering.  Top-level
surfaces delegate the request to their hosted display window.
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToFront(extSurface *Self)
{
   kt::Log log;

   log.branch("%s", Self->Name);

   if (!Self->ParentID) {
      kt::ScopedObjectLock<objDisplay> display(Self->DisplayID);
      if (display.granted()) display->moveToFront();
      return ERR::Okay|ERR::Notified;
   }

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   int currentindex;
   if ((currentindex = find_surface_list(Self)) IS -1) {
      return log.warning(ERR::Search)|ERR::Notified;
   }

   // Find the object in the list that our surface object will displace

   auto index = currentindex;
   auto level = glSurfaces[currentindex].Level;
   for (auto i=currentindex+1; (i < int(glSurfaces.size())) and
         (glSurfaces[i].Level >= glSurfaces[currentindex].Level); i++) {
      if (glSurfaces[i].Level IS level) {
         if ((glSurfaces[i].Flags & RNF::POINTER) != RNF::NIL) break; // Do not move in front of the mouse cursor
         if (glSurfaces[i].PopOverID IS Self->UID) break; // A surface has been discovered that has to be in front of us.

         if (Self->BitmapOwnerID != Self->UID) {
            // If we are a member of our parent's bitmap, we cannot be moved in front of bitmaps that own an independent buffer.

            if (glSurfaces[i].BitmapID != Self->BufferID) break;
         }

         if (((Self->Flags & RNF::STICK_TO_FRONT) IS RNF::NIL) and ((glSurfaces[i].Flags & RNF::STICK_TO_FRONT) != RNF::NIL)) break;
         index = i;
      }
   }

   // If the position hasn't changed, return immediately

   if (index <= currentindex) {
      if (Self->PopOverID) {
         // Check if the surface that we're popped over is right behind us.  If not, move it forward.

         for (auto i=index-1; i > 0; i--) {
            if (glSurfaces[i].Level IS level) {
               if (glSurfaces[i].SurfaceID != Self->PopOverID) {
                  kt::ScopedObjectLock<objSurface> pop(Self->PopOverID);
                  if (pop.granted()) pop->moveToFront();
                  return ERR::Okay|ERR::Notified;
               }
               break;
            }
         }
      }

      return ERR::Okay|ERR::Notified;
   }

   // Skip past the children that belong to the target object

   auto i = index;
   level = glSurfaces[i].Level;
   while ((i + 1 < int(glSurfaces.size())) and (glSurfaces[i+1].Level > level)) i++;

   // Count the number of children that have been assigned to this surface object.

   int total;
   for (total=1; (currentindex + total < int(glSurfaces.size())) and
         (glSurfaces[currentindex+total].Level > glSurfaces[currentindex].Level); total++) { };

   // Reorder the list so that this surface object is inserted at the new index.

   {
      auto tmp = SURFACELIST(glSurfaces.begin() + currentindex, glSurfaces.begin() + currentindex + total); // Copy the source entry into a buffer
      glSurfaces.erase(glSurfaces.begin() + currentindex, glSurfaces.begin() + currentindex + total);
      glSurfaces.insert(glSurfaces.begin() + i, tmp.begin(), tmp.end());
   }

   auto cplist = glSurfaces;

   if (Self->visible()) {
      // A redraw is required for:
      //   Any volatile regions that were in front of our surface prior to the move-to-front (by moving to the front, their background has been changed).
      //   Areas of our surface that were obscured by surfaces that also shared our bitmap space.

      if (kt::ScopedObjectLock<objBitmap> bitmap(Self->BufferID, 5000); bitmap.granted()) {
         auto area = ClipRectangle(cplist[i].Left, cplist[i].Top, cplist[i].Right, cplist[i].Bottom);
         invalidate_overlap(Self, cplist, currentindex, i, area, *bitmap);
      }

      if (check_volatile(cplist, i)) {
         _redraw_surface(Self->UID, cplist, i, 0, 0, Self->FixedWidth, Self->FixedHeight, IRF::RELATIVE);
      }
      _expose_surface(Self->UID, cplist, i, 0, 0, Self->FixedWidth, Self->FixedHeight,
         EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
   }

   if (Self->PopOverID) {
      // Check if the surface that we're popped over is right behind us.  If not, move it forward.

      for (int i=index-1; i > 0; i--) {
         if (cplist[i].Level IS level) {
            if (cplist[i].SurfaceID != Self->PopOverID) {
               if (kt::ScopedObjectLock<objSurface> pop(Self->PopOverID); pop.granted()) {
                  pop->moveToFront();
               }
               return ERR::Okay;
            }
            break;
         }
      }
   }

   refresh_pointer(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves a surface object to an absolute coordinate.

MoveToPoint() converts the supplied absolute X and Y values into relative movement and forwards the request to
#Move().  Coordinates are changed only for axes enabled by the `MTF::X` and `MTF::Y` flags.
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToPoint(extSurface *Self, struct acMoveToPoint *Args)
{
   struct acMove move;

   if (!Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::X) != MTF::NIL) move.DeltaX = Args->X - Self->FixedX;
   else move.DeltaX = 0;

   if ((Args->Flags & MTF::Y) != MTF::NIL) move.DeltaY = Args->Y - Self->FixedY;
   else move.DeltaY = 0;

   move.DeltaZ = 0;

   return Action(AC::Move, Self, &move)|ERR::Notified;
}

//********************************************************************************************************************

static ERR SURFACE_NewOwner(extSurface *Self, struct acNewOwner *Args)
{
   if ((!Args) or (!Args->NewOwner)) return ERR::NullArgs;

   if ((!Self->ParentDefined) and (!Self->initialised())) {
      OBJECTID owner_id = Args->NewOwner->UID;
      while ((owner_id) and (GetClassID(owner_id) != CLASSID::SURFACE)) {
         owner_id = GetOwnerID(owner_id);
      }
      if (owner_id) Self->ParentID = owner_id;
      else Self->ParentID = 0;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveCallback: Removes a callback previously inserted by AddCallback().

RemoveCallback() removes a draw callback that was previously inserted by #AddCallback().

This method is scope restricted.  A caller can remove only callbacks associated with its own object context, so
callbacks added by other objects are not affected.

-INPUT-
func Callback: Callback routine to remove, or leave undefined to remove all associated callback routines for the caller.

-ERRORS-
Okay
Search: The requested callback was not found.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_RemoveCallback(extSurface *Self, struct drw::RemoveCallback *Args)
{
   kt::Log log;
   OBJECTPTR context = nullptr;

   if (Args) {
      context = Args->Callback.Context;
      if (Args->Callback.isC()) {
         log.trace("Context: %d, Routine %p, Current Total: %d", context->UID, Args->Callback.Routine,
            int(Self->Callback.size()));
      }
      else {
         log.trace("Current Total: %d", int(Self->Callback.size()));
         Args->Callback.consume();
      }
   }
   else log.trace("Current Total: %d [Remove All]", int(Self->Callback.size()));

   if (not context) context = ParentContext();

   if (Self->Callback.empty()) return ERR::Okay;

   if ((not Args) or (not Args->Callback.defined())) {
      // Remove everything relating to this context if no callback was specified.

      for (auto callback = Self->Callback.begin(); callback != Self->Callback.end(); ) {
         if (callback->Context IS context) {
            deref_surface_callback(*callback);
            callback = Self->Callback.erase(callback);
         }
         else callback++;
      }
      return ERR::Okay;
   }

   if (Args->Callback.isScript()) UnsubscribeAction(Args->Callback.Context, AC::Free);

   // Find the callback entry, then shrink the list.

   auto callback = Self->Callback.begin();
   for (; callback != Self->Callback.end(); callback++) {
      if (callback->Context != context) continue;

      if ((callback->isC()) and (callback->Routine IS Args->Callback.Routine)) break;

      if ((callback->isScript()) and
          (callback->ProcedureID IS Args->Callback.ProcedureID)) break;
   }

   if (callback != Self->Callback.end()) {
      deref_surface_callback(*callback);
      Self->Callback.erase(callback);
      return ERR::Okay;
   }
   else {
      if (Args->Callback.Type IS CALL::STD_C) {
         log.warning("Unable to find callback for #%d, routine %p", context->UID, Args->Callback.Routine);
      }
      else log.warning("Unable to find callback for #%d", context->UID);
      return ERR::Search;
   }
}

/*********************************************************************************************************************

-METHOD-
ScheduleRedraw: Schedules a redraw operation for the next frame.

Use ScheduleRedraw() to mark a surface for redraw on the next frame cycle.  The delay is governed by the chosen
RefreshRate, which is measured in frames per second.  If a RefreshRate is not specified, the frame rate is derived
from the display.

Specifying a custom RefreshRate is recommended when processing an animation at a lower frame rate than that of the
display would be acceptable.

Scheduling with this method over #Draw() (immediate mode) is recommended when a cluster of redraw events may occur
within a tight time period, and it would be inefficient to draw those changes to the display individually.  Repeated
redraw requests made to the target surface are coalesced until the scheduled redraw is processed.

Redraw schedules do not collapse in nested surfaces.  If both a surface and one of its children are scheduled, two
redraw operations may be triggered where one would otherwise suffice.  Target the top-level surface only in such
instances.

-INPUT-
int RefreshRate: Optional refresh rate in frames per second.  If not specified, the refresh rate from the display is used.

-ERRORS-
Okay
Failed

-TAGS-
non-blocking, mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_ScheduleRedraw(extSurface *Self, struct drw::ScheduleRedraw *Args)
{
   int refresh_rate;

   if ((Args) and (Args->RefreshRate > 0)) {
      refresh_rate = Args->RefreshRate;
      if (refresh_rate > 255) refresh_rate = 255;
   }
   else {
      if (!Self->RefreshRate) {
         // Prefer to use the display refresh rate.  Note: Refresh rate should not be a determining factor for animation
         // frame rates - the client application is responsible for making that determination with the user.
         DisplayInfo *info;
         if (!gfx::GetDisplayInfo(Self->DisplayID, &info)) {
            if (info->RefreshRate > 255) Self->RefreshRate = 255;
            else Self->RefreshRate = info->RefreshRate;
         }

         if (!Self->RefreshRate) Self->RefreshRate = 60;
         else if (Self->RefreshRate < 25) Self->RefreshRate = 25;
         else if (Self->RefreshRate > 120) Self->RefreshRate = 120;
      }

      refresh_rate = Self->RefreshRate;
   }

   Self->RedrawCountdown = refresh_rate * 2; // Maintain subscriptions for up to 2 seconds

   if ((Self->RedrawScheduled) and (refresh_rate IS Self->RedrawRate)) return ERR::Okay;

   if (Self->RedrawTimer) {
      UpdateTimer(Self->RedrawTimer, -(1.0 / double(refresh_rate)));
      Self->RedrawScheduled = true;
      Self->RedrawRate = refresh_rate;
      return ERR::Okay;
   }

   if (!SubscribeTimer(1.0 / double(refresh_rate), C_FUNCTION(redraw_timer), &Self->RedrawTimer)) {
      Self->RedrawScheduled = true;
      Self->RedrawRate = refresh_rate;
      return ERR::Okay;
   }
   else return ERR::Failed;
}

/*********************************************************************************************************************

-ACTION-
SaveImage: Saves the graphics of a surface object.

SaveImage() renders the visible content of a surface into an image and writes it to the destination object supplied in
the action arguments.  Visible child surfaces in the captured region are included in the resulting image.

The image format is selected with the `ClassID` argument.  Supported values are @Image-compatible image classes such
as `CLASSID::JPEG` and `CLASSID::IMAGE` (PNG).  If `ClassID` is `CLASSID::NIL`, the default @Image implementation
is used.

Errors returned while copying individual child surfaces can be propagated from ~CopySurface().

-ERRORS-
Okay
NullArgs
NewObject: The intermediate image object could not be created.
Failed: The intermediate image object could not be initialised or saved.
Search
AccessObject
-END-

*********************************************************************************************************************/

static ERR SURFACE_SaveImage(extSurface *Self, struct acSaveImage *Args)
{
   kt::Log log;
   int j, level;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch();

   // Create a Bitmap that is the same size as the rendered area

   CLASSID class_id = (Args->ClassID IS CLASSID::NIL) ? CLASSID::IMAGE : Args->ClassID;

   objImage *img;
   if (!NewObject(class_id, &img)) {
      img->setFlags(PCF::NEW);
      img->Bitmap->setWidth(Self->FixedWidth);
      img->Bitmap->setHeight(Self->FixedHeight);

      objDisplay *display;
      objBitmap *video_bmp;
      if (!access_video(Self->DisplayID, &display, &video_bmp)) {
         img->Bitmap->setBitsPerPixel(video_bmp->BitsPerPixel);
         img->Bitmap->setBytesPerPixel(video_bmp->BytesPerPixel);
         img->Bitmap->setType(video_bmp->Type);
         release_video(display);
      }

      if (!InitObject(img)) {
         // Scan through the surface list and copy each buffer to our image

         const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
         auto &list = glSurfaces;

         if (auto i = find_surface_list(Self); i != -1) {
            OBJECTID bitmapid = 0;
            for (j=i; (j < int(list.size())) and ((j IS i) or (list[j].Level > list[i].Level)); j++) {
               if (list[j].invisible() or list[j].isCursor()) {
                  // Skip this surface area and all invisible children
                  level = list[j].Level;
                  while ((j + 1 < int(list.size())) and (list[j+1].Level > level)) j++;
                  continue;
               }

               // If the bitmaps are different, we have found something new to copy

               if (list[j].BitmapID != bitmapid) {
                  bitmapid = list[j].BitmapID;

                  objBitmap *picbmp;
                  img->getBitmap(picbmp);
                  if (auto error = gfx::CopySurface(list[j].SurfaceID, picbmp, BDF::NIL, 0, 0, list[j].Width,
                        list[j].Height, list[j].Left - list[i].Left, list[j].Top - list[i].Top);
                        error != ERR::Okay) {
                     FreeResource(img);
                     return log.warning(error);
                  }
               }
            }
         }

         if (!Action(AC::SaveImage, img, Args)) { // Save the image to disk
            FreeResource(img);
            return ERR::Okay;
         }
      }

      FreeResource(img);
      return log.warning(ERR::Failed);
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
SetOpacity: Alters the opacity of a surface object.

SetOpacity() changes the opacity multiplier for a surface that owns its bitmap buffer.  The final value is clamped by
the #Opacity field setter.  If the surface is visible, a redraw is queued so the new opacity is reflected on the
display without blocking the caller.

-INPUT-
double Value: New opacity multiplier, used when `Adjustment` is zero.
double Adjustment: Value to add to the current opacity multiplier, or zero to assign `Value` directly.

-ERRORS-
Okay
NullArgs
NoSupport: The surface does not own the bitmap buffer required for independent opacity.

-TAGS-
non-blocking, mutates-object
-END-

*********************************************************************************************************************/

static ERR SURFACE_SetOpacity(extSurface *Self, struct drw::SetOpacity *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->BitmapOwnerID != Self->UID) {
      kt::Log().warning("Opacity cannot be set on a surface that does not own its bitmap.");
      return ERR::NoSupport;
   }

   double value;
   if (Args->Adjustment) {
      value = Self->Opacity + Args->Adjustment;
      SET_Opacity(Self, value);
   }
   else {
      value = Args->Value;
      SET_Opacity(Self, value);
   }

   // Use the QueueAction() feature so that we don't end up with major lag problems when SetOpacity is being used for things like fading.

   if (Self->visible()) QueueAction(drw::InvalidateRegion::id, Self->UID);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Show: Shows a surface object on the display.

Show() makes the surface visible.  For top-level surfaces it shows the hosted display window; for child surfaces it
sets the visible flag, redraws the surface and exposes the affected area.  If the surface is modal, it becomes the
active modal surface until it is hidden.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Show(extSurface *Self)
{
   kt::Log log;

   log.traceBranch("%dx%d, %dx%d, Parent: %d, Modal: %d", Self->FixedX, Self->FixedY, Self->FixedWidth,
      Self->FixedHeight, Self->ParentID, Self->Modal);

   ERR notified;
   if (Self->visible()) {
      notified = ERR::Notified;
      return ERR::Okay|ERR::Notified;
   }
   else notified = ERR::NIL;

   if (!Self->ParentID) {
      kt::ScopedObjectLock display(Self->DisplayID);
      if (!acShow(*display)) {
         Self->Flags |= RNF::VISIBLE;
         if (Self->hasFocus()) acFocus(*display);
      }
      else return log.warning(ERR::Failed);
   }
   else Self->Flags |= RNF::VISIBLE;

   if (Self->Modal) Self->PrevModalID = gfx::SetModalSurface(Self->UID);

   if (notified IS ERR::NIL) {
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      RedrawSurface(Self->UID, 0, 0, Self->FixedWidth, Self->FixedHeight, IRF::RELATIVE);
      gfx::ExposeSurface(Self->UID, 0, 0, Self->FixedWidth, Self->FixedHeight,
         EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
   }

   refresh_pointer(Self);

   return ERR::Okay|notified;
}

//********************************************************************************************************************

static ERR redraw_timer(extSurface *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if (Self->RedrawScheduled) {
      Self->RedrawScheduled = false; // Done before Draw() because it tests this field.
      acDraw(Self);
   }
   else {
      // Rather than unsubscribe from the timer immediately, we hold onto it until the countdown reaches zero.  This
      // is because there is a noticeable performance penalty if you frequently subscribe and unsubscribe from the timer
      // system.
      if (Self->RedrawCountdown > 0) Self->RedrawCountdown--;
      if (!Self->RedrawCountdown) {
         Self->RedrawTimer = nullptr;
         return ERR::Terminate;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void draw_region(extSurface *Self, extSurface *Parent, extBitmap *Bitmap)
{
   // Only region objects can respond to draw messages

   if (!Self->transparent()) return;

   // If the surface object is invisible, return immediately

   if (Self->invisible()) return;

   if ((Self->FixedWidth < 1) or (Self->FixedHeight < 1)) return;

   if ((Self->FixedX > Bitmap->Clip.Right) or (Self->FixedY > Bitmap->Clip.Bottom) or
       (Self->FixedX + Self->FixedWidth <= Bitmap->Clip.Left) or
       (Self->FixedY + Self->FixedHeight <= Bitmap->Clip.Top)) {
      return;
   }

   ClipRectangle clip = Bitmap->Clip;

   // Adjust clipping and offset values to match the absolute coordinates of our surface object

   auto data = Bitmap->offset(Self->FixedX, Self->FixedY);

   // Adjust the clipping region of our parent so that it is relative to our surface area

   Bitmap->Clip.Left   -= Self->FixedX;
   Bitmap->Clip.Top    -= Self->FixedY;
   Bitmap->Clip.Right  -= Self->FixedX;
   Bitmap->Clip.Bottom -= Self->FixedY;

   // Make sure that the clipping values do not extend outside of our area

   if (Bitmap->Clip.Left   < 0) Bitmap->Clip.Left = 0;
   if (Bitmap->Clip.Top    < 0) Bitmap->Clip.Top = 0;
   if (Bitmap->Clip.Right  > Self->FixedWidth)  Bitmap->Clip.Right = Self->FixedWidth;
   if (Bitmap->Clip.Bottom > Self->FixedHeight) Bitmap->Clip.Bottom = Self->FixedHeight;

   if ((Bitmap->Clip.Left < Bitmap->Clip.Right) and (Bitmap->Clip.Top < Bitmap->Clip.Bottom)) {
      // Clear the Bitmap to the background colour if necessary

      if (Self->Colour.Alpha > 0) {
         gfx::DrawRectangle(Bitmap, 0, 0, Self->FixedWidth, Self->FixedHeight,
            Bitmap->packPixel(Self->Colour, 255), BAF::FILL);
      }

      process_surface_callbacks(Self, Bitmap);
   }

   Bitmap->Clip = clip;
   Bitmap->Data = data;
}

//********************************************************************************************************************

static ERR consume_input_events(const InputEvent *Events, int Handle)
{
   kt::Log log(__FUNCTION__);

   auto Self = (extSurface *)CurrentContext();

   static double glAnchorX = 0, glAnchorY = 0; // Anchoring is process-exclusive, so we can store the coordinates as global variables

   for (auto event=Events; event; event=event->Next) {
      // Process events that support consolidation first.

      if ((event->Flags & (JTYPE::ANCHORED|JTYPE::MOVEMENT)) != JTYPE::NIL) {
         double xchange, ychange;
         int dragindex;

         // Dragging support

         if (Self->DragStatus != DRAG::NIL) { // Consolidate movement changes
            if (Self->DragStatus IS DRAG::ANCHOR) {
               xchange = event->X;
               ychange = event->Y;
               while ((event->Next) and ((event->Next->Flags & JTYPE::ANCHORED) != JTYPE::NIL)) {
                  event = event->Next;
                  xchange += event->X;
                  ychange += event->Y;
               }
            }
            else {
               while ((event->Next) and ((event->Next->Flags & JTYPE::MOVEMENT) != JTYPE::NIL)) {
                  event = event->Next;
               }

               double absx = event->AbsX - glAnchorX;
               double absy = event->AbsY - glAnchorY;

               xchange = 0;
               ychange = 0;

               const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
               if ((dragindex = find_surface_list(Self)) != -1) {
                  xchange = absx - glSurfaces[dragindex].Left;
                  ychange = absy - glSurfaces[dragindex].Top;
               }
            }

            // Move the dragging surface to the new location

            if ((Self->DragID) and (Self->DragID != Self->UID)) {
               kt::ScopedObjectLock drag(Self->DragID);
               if (drag.granted()) acMove(*drag, xchange, ychange, 0);
            }
            else {
               auto sticky = (Self->Flags & RNF::STICKY) != RNF::NIL;
               Self->Flags &= ~RNF::STICKY; // Turn off the sticky flag, as it prevents movement

               acMove(Self, xchange, ychange, 0);

               if (sticky) {
                  Self->Flags |= RNF::STICKY;
                  UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags); // (Required to put back the sticky flag)
               }
            }

            // The new pointer position is based on the position of the surface that's being dragged.

            if (Self->DragStatus IS DRAG::ANCHOR) {
               const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
               if ((dragindex = find_surface_list(Self)) != -1) {
                  double absx = glSurfaces[dragindex].Left + glAnchorX;
                  double absy = glSurfaces[dragindex].Top + glAnchorY;
                  gfx::SetCursorPos(absx, absy);
               }
            }
         }
      }
      else if ((event->Type IS JET::LMB) and ((event->Flags & JTYPE::REPEATED) IS JTYPE::NIL)) {
         if (event->Value > 0) {
            if (Self->disabled()) continue;

            // Anchor the pointer position if dragging is enabled

            if ((Self->DragID) and (Self->DragStatus IS DRAG::NONE)) {
               log.trace("Dragging object %d; Anchored to %dx%d", Self->DragID, event->X, event->Y);

               // Ask the pointer to anchor itself to our surface.  If the left mouse button is released, the
               // anchor will be released by the pointer automatically.

               glAnchorX  = event->X;
               glAnchorY  = event->Y;
               if (!gfx::LockCursor(Self->UID)) Self->DragStatus = DRAG::ANCHOR;
               else Self->DragStatus = DRAG::NORMAL;
            }
         }
         else { // Click released
            if (Self->DragStatus != DRAG::NIL) {
               gfx::UnlockCursor(Self->UID);
               Self->DragStatus = DRAG::NONE;
            }
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

#include "surface_drawing.cpp"
#include "surface_fields.cpp"
#include "surface_dimensions.cpp"
#include "surface_resize.cpp"
#include "../lib_surfaces.cpp"

//********************************************************************************************************************

#include "surface_def.c"

static const FieldArray clSurfaceFields[] = {
   { "X",            FDF_UNIT|FDF_PURE|FDF_RW, GET_X, SET_X },
   { "Y",            FDF_UNIT|FDF_PURE|FDF_RW, GET_Y, SET_Y },
   { "Width",        FDF_UNIT|FDF_PURE|FDF_RW, GET_Width, SET_Width },
   { "Height",       FDF_UNIT|FDF_PURE|FDF_RW, GET_Height, SET_Height },
   { "XOffset",      FDF_UNIT|FDF_PURE|FDF_RW, GET_XOffset, SET_XOffset },
   { "YOffset",      FDF_UNIT|FDF_PURE|FDF_RW, GET_YOffset, SET_YOffset },
   { "Drag",         FDF_OBJECTID|FDF_RW, nullptr, SET_Drag, CLASSID::SURFACE },
   { "Buffer",       FDF_OBJECTID|FDF_R,  nullptr, nullptr, CLASSID::BITMAP },
   { "Parent",       FDF_OBJECTID|FDF_RW, nullptr, SET_Parent, CLASSID::SURFACE },
   { "PopOver",      FDF_OBJECTID|FDF_RI, nullptr, SET_PopOver },
   { "MinWidth",     FDF_INT|FDF_RW,  nullptr, SET_MinWidth },
   { "MinHeight",    FDF_INT|FDF_RW,  nullptr, SET_MinHeight },
   { "MaxWidth",     FDF_INT|FDF_RW,  nullptr, SET_MaxWidth },
   { "MaxHeight",    FDF_INT|FDF_RW,  nullptr, SET_MaxHeight },
   { "Display",      FDF_OBJECTID|FDF_R, nullptr, nullptr, CLASSID::DISPLAY },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, SET_Flags, &clSurfaceFlags },
   { "RootLayer",    FDF_OBJECTID|FDF_RW, nullptr, SET_RootLayer },
   { "DragStatus",   FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clSurfaceDragStatus },
   { "Cursor",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Cursor, &clSurfaceCursor },
   { "Colour",       FDF_STRUCT|FDF_RW, nullptr, nullptr, "RGB8" },
   { "Type",         FDF_SYSTEM|FDF_INT|FDF_RI, nullptr, nullptr, &clSurfaceType },
   { "Modal",        FDF_INT|FDF_RW, nullptr, SET_Modal },
   // Virtual fields
   { "AbsX",          FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_RW, GET_AbsX, SET_AbsX },
   { "AbsY",          FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_RW, GET_AbsY, SET_AbsY },
   { "BitsPerPixel",  FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_RI, GET_BitsPerPixel, SET_BitsPerPixel },
   { "Bottom",        FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_R,  GET_Bottom },
   { "Opacity",       FDF_VIRTUAL|FDF_DOUBLE|FDF_PURE|FDF_RW, GET_Opacity, SET_Opacity },
   { "RevertFocus",   FDF_SYSTEM|FDF_VIRTUAL|FDF_OBJECTID|FDF_W, nullptr, SET_RevertFocus },
   { "Right",         FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_R,  GET_Right },
   { "UserFocus",     FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_R,  GET_UserFocus },
   { "Visible",       FDF_VIRTUAL|FDF_INT|FDF_PURE|FDF_RW, GET_Visible, SET_Visible },
   { "WindowType",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_PURE|FDF_RW, GET_WindowType, SET_WindowType, &clSurfaceSWIN },
   { "WindowHandle",  FDF_VIRTUAL|FDF_POINTER|FDF_PURE|FDF_RW, GET_WindowHandle, SET_WindowHandle },
   END_FIELD
};

//********************************************************************************************************************

ERR create_surface_class(void)
{
   clSurface = objMetaClass::create::global(
      fl::ClassVersion(VER_SURFACE),
      fl::Name("Surface"),
      fl::Category(CCF::GUI),
      fl::Actions(clSurfaceActions),
      fl::Methods(clSurfaceMethods),
      fl::Fields(clSurfaceFields),
      fl::Size(sizeof(extSurface)),
      fl::Path("modules:display"));

   return clSurface ? ERR::Okay : ERR::AddClass;
}
