/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Surface: Manages the display and positioning of 2-Dimensional rendered graphics.

The Surface class is used to manage the positioning, drawing and interaction with layered display interfaces.  It
works in conjunction with the @Bitmap class for rendering graphics, and the @Pointer class for user interaction.

On a platform such as Windows or Linux, the top-level surface will typically be hosted in an application
window.  On Android or when a full screen display is required, a surface can cover the entire display and be
window-less.  The top-level surface can act as a host to additional surfaces, which are referred to as children.
Placing more surface objects inside of these children will create a hierarchy of many objects that requires
sophisticated management that is provisioned by the Surface class.

Although pure surface based UI's are possible, clients should always pursue the more simplistic approach of using
surfaces to host @VectorScene objects that describe vector based interfaces.  Doing so is in keeping with our goal
of proving fully scalable interfaces to users, and we optimise features with that use-case in mind.

-END-

Technical Note: The Surface class uses the "backing store" technique for always preserving the graphics of rendered
areas.

*********************************************************************************************************************/

#undef __xwindows__
#include "../defs.h"
#include <kotuku/modules/picture.h>
#include <numeric> // For std::gcd

#ifdef _WIN32
using namespace display;
#endif

static ERR SET_Opacity(extSurface *, double);
static ERR SET_XOffset(extSurface *, Unit *);
static ERR SET_YOffset(extSurface *, Unit *);

#define MOVE_VERTICAL   0x0001
#define MOVE_HORIZONTAL 0x0002

static ERR consume_input_events(const InputEvent *, int);
static void draw_region(extSurface *, extSurface *, extBitmap *);
static ERR redraw_timer(extSurface *, int64_t, int64_t);

/*********************************************************************************************************************
** This call is used to refresh the pointer image when at least one layer has been rearranged.  The timer is used to
** delay the refresh - useful if multiple surfaces are being rearranged when we only need to do the refresh once.
** The delay also prevents clashes with read/write access to the surface list.
*/

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
      pf::SwitchContext context(glModule);
      SubscribeTimer(0.02, C_FUNCTION(refresh_pointer_timer), &glRefreshPointerTimer);
   }
}

//********************************************************************************************************************

static ERR access_video(OBJECTID DisplayID, objDisplay **Display, objBitmap **Bitmap)
{
   if (AccessObject(DisplayID, 5000, (OBJECTPTR *)Display) IS ERR::Okay) {
      #ifdef _WIN32
      APTR winhandle;
      if (Display[0]->get(FID_WindowHandle, winhandle) IS ERR::Okay) {
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
      Display->Bitmap->get(FID_Handle, surface);

      APTR winhandle;
      if (Display->get(FID_WindowHandle, winhandle) IS ERR::Okay) {
         winReleaseDC(winhandle, surface);
      }

      Display->Bitmap->setHandle((APTR)nullptr);
   #endif

   acFlush(Display);

   ReleaseObject(Display);
}

/*********************************************************************************************************************
** Used By:  MoveToBack(), move_layer()
**
** This is the best way to figure out if a surface object or its children causes it to be volatile.  Use this function
** if you don't want to do any deep scanning to determine who is volatile or not.
**
** Volatile flags are PRECOPY, AFTER_COPY and CURSOR.
**
** NOTE: Surfaces marked as COMPOSITE or TRANSPARENT are not considered volatile as they do not require redraws.  It's
** up to the caller to make a decision as to whether COMPOSITE's are volatile or not.
*/

static bool check_volatile(const SURFACELIST &List, int Index)
{
   if (List[Index].isVolatile()) return true;

   // If there are children with custom root layers or are volatile, that will force volatility

   int j;
   for (int i=Index+1; List[i].Level > List[Index].Level; i++) {
      if (List[i].invisible()) {
         j = List[i].Level;
         while (List[i+1].Level > j) i++;
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
   pf::Log log(__FUNCTION__);

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
   if (access_video(DisplayID, &display, &video_bmp) IS ERR::Okay) {
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
   pf::Log log(__FUNCTION__);
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

      for (j=i+1; list[j].Level > list[i].Level; j++);
      i = j - 1;
   }
}

//********************************************************************************************************************
// Handler for the display being resized.

static void display_resized(OBJECTID DisplayID, int X, int Y, int Width, int Height)
{
   OBJECTID surface_id = GetOwnerID(DisplayID);

   if (pf::ScopedObjectLock<extSurface> surface(surface_id, 4000); surface.granted()) {
      if (surface->classID() IS CLASSID::SURFACE) {
         if ((X != surface->X) or (Y != surface->Y)) {
            surface->X = X;
            surface->Y = Y;
            UpdateSurfaceRecord(*surface);
         }

         if ((surface->Width != Width) or (surface->Height != Height)) {
            acResize(*surface, Width, Height, 0);
         }
      }
   }
}

//********************************************************************************************************************

static void notify_free_parent(OBJECTPTR Object, ACTIONID ActionID, ERR Result)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   // Free ourselves in advance if our parent is in the process of being killed.  This causes a chain reaction
   // that results in a clean deallocation of the surface hierarchy.

   Self->Flags &= ~RNF::VISIBLE;
   UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);
   if (Self->defined(NF::LOCAL)) QueueAction(AC::Free, Self->UID); // If the object is a child of something, give the parent object time to do the deallocation itself
   else FreeResource(Self);
}

static void notify_free_callback(OBJECTPTR Object, ACTIONID ActionID, ERR Result)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   for (int i=0; i < Self->CallbackCount; i++) {
      if (Self->Callback[i].Function.isScript()) {
         if (Self->Callback[i].Function.Context->UID IS Object->UID) {
            Self->Callback[i].Function.clear();

            int j;
            for (j=i; j < Self->CallbackCount-1; j++) { // Shorten the array
               Self->Callback[j] = Self->Callback[j+1];
            }
            i--;
            Self->CallbackCount--;
         }
      }
   }
}

static void notify_draw_display(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acDraw *Args)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   if (Self->collecting()) return;

   // Hosts will sometimes call Draw to indicate that the display has been exposed.

   log.traceBranch("Display exposure received - redrawing display.");

   if (Args) Self->exposeToDisplay(Args->X, Args->Y, Args->Width, Args->Height, EXF::CHILDREN);
   else Self->exposeToDisplay(0, 0, 20000, 20000, EXF::CHILDREN);
}

static void notify_redimension_parent(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acRedimension *Args)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extSurface *)CurrentContext();

   if (Self->Document) return;
   if (Self->collecting()) return;

   log.traceBranch("Redimension notification from parent #%d, currently %dx%d,%dx%d.", Self->ParentID, Self->X, Self->Y, Self->Width, Self->Height);

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
      DISPLAYINFO *display;
      if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
         parentwidth  = display->Width;
         parentheight = display->Height;
      }
      else return;
   }

   // Convert relative offsets to their fixed equivalent

   if (dmf::hasScaledXOffset(Self->Dimensions)) Self->XOffset = parentwidth * Self->XOffsetPercent;
   if (dmf::hasScaledYOffset(Self->Dimensions)) Self->YOffset = parentheight * Self->YOffsetPercent;

   // Calculate absolute width and height values

   if (dmf::hasScaledWidth(Self->Dimensions))   width = parentwidth * Self->WidthPercent;
   else if (dmf::hasWidth(Self->Dimensions)) width = Self->Width;
   else if (dmf::hasAnyXOffset(Self->Dimensions)) {
      if (dmf::hasX(Self->Dimensions)) {
         width = parentwidth - Self->X - Self->XOffset;
      }
      else if (dmf::hasScaledX(Self->Dimensions)) {
         width = parentwidth - (parentwidth * Self->XPercent) - Self->XOffset;
      }
      else width = parentwidth - Self->XOffset;
   }
   else width = Self->Width;

   if (dmf::hasScaledHeight(Self->Dimensions))   height = parentheight * Self->HeightPercent;
   else if (dmf::hasHeight(Self->Dimensions)) height = Self->Height;
   else if (dmf::hasAnyYOffset(Self->Dimensions)) {
      if (dmf::hasY(Self->Dimensions)) {
         height = parentheight - Self->Y - Self->YOffset;
      }
      else if (dmf::hasScaledY(Self->Dimensions)) {
         height = parentheight - (parentheight * Self->YPercent) - Self->YOffset;
      }
      else height = parentheight - Self->YOffset;
   }
   else height = Self->Height;

   // Calculate new coordinates

   if (dmf::hasScaledX(Self->Dimensions)) x = parentwidth * Self->XPercent;
   else if (dmf::hasAnyXOffset(Self->Dimensions)) x = parentwidth - Self->XOffset - width;
   else x = Self->X;

   if (dmf::hasScaledY(Self->Dimensions)) y = parentheight * Self->YPercent;
   else if (dmf::hasAnyYOffset(Self->Dimensions)) y = parentheight - Self->YOffset - height;
   else y = Self->Y;

   // Alignment adjustments

   if ((Self->Align & ALIGN::LEFT) != ALIGN::NIL) x = 0;
   else if ((Self->Align & ALIGN::RIGHT) != ALIGN::NIL) x = parentwidth - width;
   else if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) x = (parentwidth - width) * 0.5;

   if ((Self->Align & ALIGN::TOP) != ALIGN::NIL) y = 0;
   else if ((Self->Align & ALIGN::BOTTOM) != ALIGN::NIL) y = parentheight - height;
   else if ((Self->Align & ALIGN::VERTICAL) != ALIGN::NIL) y = (parentheight - height) * 0.5;

   if ((Self->MaxWidth > 0) and (width > Self->MaxWidth)) {
      log.trace("Calculated width of %.0f exceeds max limit of %d", width, Self->MaxWidth);
      width = Self->MaxWidth;
   }

   if ((Self->MaxHeight > 0) and (height > Self->MaxHeight)) {
      log.trace("Calculated height of %.0f exceeds max limit of %d", height, Self->MaxHeight);
      height = Self->MaxHeight;
   }

   // Perform the resize

   if ((Self->X != x) or (Self->Y != y) or (Self->Width != width) or (Self->Height != height) or (Args->Depth)) {
      acRedimension(Self, x, y, 0, width, height, Args->Depth);
   }
}

/*********************************************************************************************************************
-ACTION-
Activate: Shows a surface object on the display.
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

The AddCallback() method provides a hook for custom functions to draw directly to a surface.  Whenever a surface
object performs a redraw event, all functions inserted by this method will be called in their original subscription
order with a direct reference to the Surface's target bitmap.  The C/C++ prototype is
`Function(APTR Context, *Surface, *Bitmap, APTR Meta)`.

The Tiri prototype is `function draw(Surface, Bitmap)`

The subscriber can draw to the bitmap surface as it would with any freshly allocated bitmap object (refer to the
@Bitmap class).  To get the width and height of the available drawing space, please read the Width and
Height fields from the Surface object.  If writing to the bitmap directly, please observe the bitmap's clipping
region and the XOffset and YOffset values.

-INPUT-
ptr(func) Callback: Pointer to the callback routine or NULL to remove callbacks for the given Object.

-ERRORS-
Okay
NullArgs
ExecViolation: The call was not made from the process that owns the object.
AllocMemory
-END-

*********************************************************************************************************************/

static ERR SURFACE_AddCallback(extSurface *Self, struct drw::AddCallback *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   OBJECTPTR context = ParentContext();
   OBJECTPTR call_context = nullptr;
   if (Args->Callback->isC()) call_context = (OBJECTPTR)Args->Callback->Context;
   else if (Args->Callback->isScript()) call_context = context; // Scripts use runtime ID resolution...

   if (context->UID < 0) {
      log.warning("Public objects may not draw directly to surfaces.");
      return ERR::NoPermission;
   }

   log.msg("Context: %d, Callback Context: %d, Routine: %p (Count: %d)", context->UID, call_context ? call_context->UID : 0, Args->Callback->Routine, Self->CallbackCount);

   if (call_context) context = call_context;

   if (Self->Callback) {
      // Check if the subscription is already on the list for our surface context.

      int i;
      for (i=0; i < Self->CallbackCount; i++) {
         if (Self->Callback[i].Object IS context) {
            if ((Self->Callback[i].Function.isC()) and (Args->Callback->isC())) {
               if (Self->Callback[i].Function.Routine IS Args->Callback->Routine) break;
            }
            else if ((Self->Callback[i].Function.isScript()) and (Args->Callback->isScript())) {
               if (Self->Callback[i].Function.ProcedureID IS Args->Callback->ProcedureID) break;
            }
         }
      }

      if (i < Self->CallbackCount) {
         log.trace("Moving existing subscription to foreground.");

         while (i < Self->CallbackCount-1) {
            Self->Callback[i] = Self->Callback[i+1];
            i++;
         }
         Self->Callback[i].Object   = context;
         Self->Callback[i].Function = *Args->Callback;
         return ERR::Okay;
      }
      else if (Self->CallbackCount < Self->CallbackSize) {
         // Add the callback routine to the cache

         Self->Callback[Self->CallbackCount].Object   = context;
         Self->Callback[Self->CallbackCount].Function = *Args->Callback;
         Self->CallbackCount++;
      }
      else if (Self->CallbackCount < 255) {
         log.detail("Expanding draw subscription array.");

         int new_size = Self->CallbackSize + 10;
         if (new_size > 255) new_size = 255;
         SurfaceCallback *scb;
         if (AllocMemory(sizeof(SurfaceCallback) * new_size, MEM::DATA|MEM::NO_CLEAR, &scb) IS ERR::Okay) {
            copymem(Self->Callback, scb, sizeof(SurfaceCallback) * Self->CallbackCount);

            scb[Self->CallbackCount].Object   = context;
            scb[Self->CallbackCount].Function = *Args->Callback;
            Self->CallbackCount++;
            Self->CallbackSize = new_size;

            if (Self->Callback != Self->CallbackCache) FreeResource(Self->Callback);
            Self->Callback = scb;
         }
         else return ERR::AllocMemory;
      }
      else return ERR::ArrayFull;
   }
   else {
      Self->Callback = Self->CallbackCache;
      Self->CallbackCount = 1;
      Self->CallbackSize = std::ssize(Self->CallbackCache);
      Self->Callback[0].Object = context;
      Self->Callback[0].Function = *Args->Callback;
   }

   if (Args->Callback->Type IS CALL::SCRIPT) {
      SubscribeAction(Args->Callback->Context, AC::Free, C_FUNCTION(notify_free_callback));
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Disable: Disables a surface object.
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
-END-
*********************************************************************************************************************/

static int64_t glLastFocusTime = 0;

static ERR SURFACE_Focus(extSurface *Self)
{
   pf::Log log;

   if (Self->disabled()) return ERR::Okay|ERR::Notified;

   if (auto msg = GetActionMsg()) {
      // This is a message - in which case it could have been delayed and thus superseded by a more recent message.

      if (msg->Time < glLastFocusTime) {
         FOCUSMSG("Ignoring superseded focus message.");
         return ERR::Okay|ERR::Notified;
      }
   }

   if ((Self->Flags & RNF::IGNORE_FOCUS) != RNF::NIL) {
      FOCUSMSG("Focus propagated to parent (IGNORE_FOCUS flag set).");
      pf::ScopedObjectLock focus(Self->ParentID);
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
      pf::ScopedObjectLock<objSurface> obj(*it);
      if (obj.granted()) obj->inheritedFocus(Self->UID, Self->Flags);
      it++;
   }

   // Send out LostFocus actions to all objects that do not intersect with the new focus chain.

   for (auto &id : lostfocus) {
      pf::ScopedObjectLock obj(id);
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
         pf::ScopedObjectLock focus(Self->RevertFocusID);
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
         pf::ScopedObjectLock display(Self->DisplayID);
         if (display.granted()) acFocus(*display);
      }

      if (Self->RevertFocusID) {
         pf::ScopedObjectLock focus(Self->RevertFocusID);
         Self->RevertFocusID = 0;
         if (focus.granted()) acFocus(*focus);
      }

      glLastFocusTime = PreciseTime();
      return ERR::Okay;
   }
}

//********************************************************************************************************************

static ERR SURFACE_Free(extSurface *Self)
{
   if (Self->RedrawTimer) { UpdateTimer(Self->RedrawTimer, 0); Self->RedrawTimer = 0; }

   if ((Self->Callback) and (Self->Callback != Self->CallbackCache)) {
      FreeResource(Self->Callback);
      Self->Callback = nullptr;
      Self->CallbackCount = 0;
      Self->CallbackSize = 0;
   }

   if (Self->ParentID) {
      if (pf::ScopedObjectLock<extSurface> parent(Self->ParentID, 5000); parent.granted()) {
         UnsubscribeAction(*parent, AC::NIL);
         if (Self->transparent()) {
            Action(drw::RemoveCallback::id, Self, nullptr);
         }
      }
   }

   acHide(Self);

   // Remove any references to this surface object from the global surface list

   untrack_layer(Self->UID);

   if ((!Self->ParentID) and (Self->DisplayID)) {
      FreeResource(Self->DisplayID);
      Self->DisplayID = 0;
   }

   if ((Self->BufferID) and ((!Self->BitmapOwnerID) or (Self->BitmapOwnerID IS Self->UID))) {
      if (Self->Bitmap) { ReleaseObject(Self->Bitmap); Self->Bitmap = nullptr; }
      FreeResource(Self->BufferID);
      Self->BufferID = 0;
   }

   // Give the focus to the parent if our object has the primary focus.  Do not apply this technique to surface objects
   // acting as windows, as the window class has its own focus management code.

   if (Self->hasFocus() and (Self->Owner) and (Self->Owner->classID() != CLASSID::WINDOW)) {
      if (Self->ParentID) {
         pf::ScopedObjectLock focus(Self->ParentID);
         if (focus.granted()) acFocus(*focus);
      }
   }

   if ((Self->Flags & RNF::AUTO_QUIT) != RNF::NIL) {
      pf::Log log;
      log.msg("Posting a quit message due to use of AUTOQUIT.");
      SendMessage(MSGID::QUIT, MSF::NIL, nullptr, 0);
   }

   if (Self->InputHandle) gfx::UnsubscribeInput(Self->InputHandle);

   for (auto it = glWindowHooks.begin(); it != glWindowHooks.end();) {
      if (it->first.SurfaceID IS Self->UID) {
         it = glWindowHooks.erase(it);
      }
      else it++;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Hide: Hides a surface object from the display.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Hide(extSurface *Self)
{
   pf::Log log;

   log.traceBranch();

   if (Self->invisible()) return ERR::Okay|ERR::Notified;

   if (!Self->ParentID) {
      Self->Flags &= ~RNF::VISIBLE; // Important to switch off visibliity before Hide(), otherwise a false redraw will occur.
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      pf::ScopedObjectLock display(Self->DisplayID);
      if (display.granted()) if (acHide(*display) != ERR::Okay) return ERR::Failed;
   }
   else {
      // Mark this surface object as invisible, then invalidate the region it was covering in order to have the background redrawn.

      Self->Flags &= ~RNF::VISIBLE;
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      if (Self->BitmapOwnerID != Self->UID) {
         RedrawSurface(Self->ParentID, Self->X, Self->Y, Self->Width, Self->Height, IRF::RELATIVE);
      }
      gfx::ExposeSurface(Self->ParentID, Self->X, Self->Y, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE);
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
-END-

*********************************************************************************************************************/

static ERR SURFACE_InheritedFocus(extSurface *Self, struct drw::InheritedFocus *Args)
{
   if (auto msg = GetActionMsg()) {
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
   pf::Log log;

   bool require_store = false;
   OBJECTID parent_bitmap = 0;
   OBJECTID bitmap_owner  = 0;

   if (!Self->RootID) Self->RootID = Self->UID;

   if (Self->isCursor()) Self->Flags |= RNF::STICK_TO_FRONT;

   // If no parent surface is set, check if the client has set the FULL_SCREEN flag.  If not, try to give the
   // surface a parent.

   if ((!Self->ParentID) and (gfx::GetDisplayType() IS DT::NATIVE)) {
      if ((Self->Flags & RNF::FULL_SCREEN) IS RNF::NIL) {
         if (FindObject("desktop", CLASSID::SURFACE, FOF::NIL, &Self->ParentID) != ERR::Okay) {
            if (!glSurfaces.empty()) Self->ParentID = glSurfaces[0].SurfaceID;
         }
      }
   }

   ERR error = ERR::Okay;
   if (Self->ParentID) {
      pf::ScopedObjectLock<extSurface> parent(Self->ParentID, 3000);
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

      // Set FixedX/FixedY accordingly - this is used to assist in the layout process when a surface is used in a document.

      if ((Self->Dimensions & DMF(0xffff)) != DMF::NIL) {
         if ((dmf::hasAnyX(Self->Dimensions)) and dmf::has(Self->Dimensions, DMF::FIXED_WIDTH|DMF::SCALED_WIDTH|DMF::FIXED_X_OFFSET|DMF::SCALED_X_OFFSET)) {
            Self->FixedX = true;
         }
         else if ((dmf::hasAnyXOffset(Self->Dimensions)) and dmf::has(Self->Dimensions, DMF::FIXED_WIDTH|DMF::SCALED_WIDTH|DMF::FIXED_X|DMF::SCALED_X)) {
            Self->FixedX = true;
         }

         if ((dmf::hasAnyY(Self->Dimensions)) and dmf::has(Self->Dimensions, DMF::FIXED_HEIGHT|DMF::SCALED_HEIGHT|DMF::FIXED_Y_OFFSET|DMF::SCALED_Y_OFFSET)) {
            Self->FixedY = true;
         }
         else if ((dmf::hasAnyYOffset(Self->Dimensions)) and dmf::has(Self->Dimensions, DMF::FIXED_HEIGHT|DMF::SCALED_HEIGHT|DMF::FIXED_Y|DMF::SCALED_Y)) {
            Self->FixedY = true;
         }
      }

      // Recalculate coordinates if offsets are used

      if (dmf::hasXOffset(Self->Dimensions))         Self->setXOffset(Self->XOffset);
      else if (dmf::hasScaledXOffset(Self->Dimensions)) Self->set(FID_XOffset, Unit(Self->XOffsetPercent, FD_SCALED));

      if (dmf::hasYOffset(Self->Dimensions))         Self->setYOffset(Self->YOffset);
      else if (dmf::hasScaledYOffset(Self->Dimensions)) Self->set(FID_YOffset, Unit(Self->YOffsetPercent, FD_SCALED));

      if (dmf::hasScaledX(Self->Dimensions))       Self->set(FID_X, Unit(Self->XPercent, FD_SCALED));
      if (dmf::hasScaledY(Self->Dimensions))       Self->set(FID_Y, Unit(Self->YPercent, FD_SCALED));
      if (dmf::hasScaledWidth(Self->Dimensions))   Self->set(FID_Width,  Unit(Self->WidthPercent, FD_SCALED));
      if (dmf::hasScaledHeight(Self->Dimensions))  Self->set(FID_Height, Unit(Self->HeightPercent, FD_SCALED));

      if (!(dmf::hasAnyWidth(Self->Dimensions))) {
         if (dmf::hasAnyXOffset(Self->Dimensions)) {
            Self->Width = parent->Width - Self->X - Self->XOffset;
         }
         else {
            Self->Width = 20;
            Self->Dimensions |= DMF::FIXED_WIDTH;
         }
      }

      if (!(dmf::hasAnyHeight(Self->Dimensions))) {
         if (dmf::hasAnyYOffset(Self->Dimensions)) {
            Self->Height = parent->Height - Self->Y - Self->YOffset;
         }
         else {
            Self->Height = 20;
            Self->Dimensions |= DMF::FIXED_HEIGHT;
         }
      }

      // Alignment adjustments

      if ((Self->Align & ALIGN::LEFT) != ALIGN::NIL) { Self->X = 0; Self->setX(Self->X); }
      else if ((Self->Align & ALIGN::RIGHT) != ALIGN::NIL) { Self->X = parent->Width - Self->Width; Self->setX(Self->X); }
      else if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) { Self->X = (parent->Width - Self->Width) / 2; Self->setX(Self->X); }

      if ((Self->Align & ALIGN::TOP) != ALIGN::NIL) { Self->Y = 0; Self->setY(Self->Y); }
      else if ((Self->Align & ALIGN::BOTTOM) != ALIGN::NIL) { Self->Y = parent->Height - Self->Height; Self->setY(Self->Y); }
      else if ((Self->Align & ALIGN::VERTICAL) != ALIGN::NIL) { Self->Y = (parent->Height - Self->Height) / 2; Self->setY(Self->Y); }

      if (Self->Width  < Self->MinWidth)  Self->Width  = Self->MinWidth;
      if (Self->Height < Self->MinHeight) Self->Height = Self->MinHeight;
      if ((Self->MaxWidth > 0) and (Self->Width  > Self->MaxWidth))  Self->Width  = Self->MaxWidth;
      if ((Self->MaxHeight > 0) and (Self->Height > Self->MaxHeight)) Self->Height = Self->MaxHeight;

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

      if (!(dmf::hasWidth(Self->Dimensions))) {
         Self->Width = glpDisplayWidth;
         Self->Dimensions |= DMF::FIXED_WIDTH;
      }

      if (!(dmf::hasHeight(Self->Dimensions))) {
         Self->Height = glpDisplayHeight;
         Self->Dimensions |= DMF::FIXED_HEIGHT;
      }

      if (!(dmf::hasX(Self->Dimensions))) {
         if ((Self->Flags & RNF::HOST) != RNF::NIL) Self->X = 0;
         else Self->X = glpDisplayX;
         Self->Dimensions |= DMF::FIXED_X;
      }

      if (!(dmf::hasY(Self->Dimensions))) {
         if ((Self->Flags & RNF::HOST) != RNF::NIL) Self->Y = 0;
         else Self->Y = glpDisplayY;
         Self->Dimensions |= DMF::FIXED_Y;
      }

      if ((Self->Width < 10) or (Self->Height < 6)) {
         Self->Width  = 640;
         Self->Height = 480;
      }

      if (gfx::GetDisplayType() != DT::NATIVE) {
         // Alignment adjustments

         DISPLAYINFO *display;
         if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
            if ((Self->Align & ALIGN::LEFT) != ALIGN::NIL) { Self->X = 0; Self->setX(Self->X); }
            else if ((Self->Align & ALIGN::RIGHT) != ALIGN::NIL) { Self->X = display->Width - Self->Width; Self->setX(Self->X); }
            else if ((Self->Align & ALIGN::HORIZONTAL) != ALIGN::NIL) { Self->X = (display->Width - Self->Width) / 2; Self->setX(Self->X); }

            if ((Self->Align & ALIGN::TOP) != ALIGN::NIL) { Self->Y = 0; Self->setY(Self->Y); }
            else if ((Self->Align & ALIGN::BOTTOM) != ALIGN::NIL) { Self->Y = display->Height - Self->Height; Self->setY(Self->Y); }
            else if ((Self->Align & ALIGN::VERTICAL) != ALIGN::NIL) { Self->Y = (display->Height - Self->Height) / 2; Self->setY(Self->Y); }
         }
      }

      if (Self->Width  < Self->MinWidth)  Self->Width  = Self->MinWidth;
      if (Self->Height < Self->MinHeight) Self->Height = Self->MinHeight;
      if ((Self->MaxWidth > 0) and (Self->Width  > Self->MaxWidth))  Self->Width  = Self->MaxWidth;
      if ((Self->MaxHeight > 0) and (Self->Height > Self->MaxHeight)) Self->Height = Self->MaxHeight;

      if ((Self->Flags & RNF::STICK_TO_FRONT) != RNF::NIL) gfx::SetHostOption(HOST::STICK_TO_FRONT, 1);
      else gfx::SetHostOption(HOST::STICK_TO_FRONT, 0);

      if ((Self->Flags & RNF::COMPOSITE) != RNF::NIL) scrflags |= SCR::COMPOSITE;

      OBJECTID id, pop_display = 0;
      CSTRING name = FindObject("SystemDisplay", CLASSID::NIL, FOF::NIL, &id) != ERR::Okay ? "SystemDisplay" : (CSTRING)nullptr;

      if (Self->PopOverID) {
         if (pf::ScopedObjectLock<extSurface> popsurface(Self->PopOverID, 2000); popsurface.granted()) {
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
            fl::X(Self->X), fl::Y(Self->Y), fl::Width(Self->Width), fl::Height(Self->Height),
            fl::BitsPerPixel(glpDisplayDepth),
            fl::RefreshRate(glpRefreshRate),
            fl::Flags(scrflags),
            fl::Opacity(Self->Opacity * (100.0 / 255.0)),
            fl::PopOver(pop_display),
            fl::WindowHandle((APTR)Self->DisplayWindow))) { // Sometimes a window may be preset, e.g. for a web plugin

         display->setGamma(glpGammaRed, glpGammaGreen, glpGammaBlue, GMF::SAVE);
         gfx::SetHostOption(HOST::TASKBAR, 1); // Reset display system so that windows open with a taskbar by default

         // Get the true coordinates of the client area of the surface

         Self->X = display->X;
         Self->Y = display->Y;
         Self->Width  = display->Width;
         Self->Height = display->Height;

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

            int gcd = std::gcd(Self->Width, Self->Height);
            Self->MinWidth  = Self->Width / gcd;
            Self->MinHeight = Self->Height / gcd;
            Self->MaxWidth  = Self->Width * 10;
            Self->MaxHeight = Self->Height * 10;

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

         display->get(FID_WindowHandle, Self->DisplayWindow);

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
         DISPLAYINFO *info;
         if (gfx::GetDisplayInfo(Self->DisplayID, &info) IS ERR::Okay) {
            if (info->BitsPerPixel != Self->BitsPerPixel) require_store = true;
         }
      }
   }

   if (Self->transparent()) require_store = false;

   if (require_store) {
      Self->BitmapOwnerID = Self->UID;

      pf::ScopedObjectLock<objDisplay> display(Self->DisplayID, 3000);

      if (display.granted()) {
         auto memflags = MEM::DATA;

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
               fl::BitsPerPixel(bpp), fl::Width(Self->Width), fl::Height(Self->Height),
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
      if (pf::ScopedObjectLock<objBitmap> bitmap(Self->BufferID, 5000); bitmap.granted()) {
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
      int index;
      if ((index = find_surface_list(Self)) != -1) {
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

If a surface is hosted in a desktop window, calling the Minimise() method will perform the default minimise action
on that window.  On a platform such as Microsoft Windows, this would normally result in the window being
minimised to the task bar.

Calling Minimise() on a surface that is already in the minimised state may result in the host window being restored to
the desktop.  This behaviour is platform dependent and should be manually tested to confirm its reliability on the
host platform.
-END-

*********************************************************************************************************************/

static ERR SURFACE_Minimise(extSurface *Self)
{
   if (Self->DisplayID) {
      pf::ScopedObjectLock<objDisplay> display(Self->DisplayID);
      if (display.granted()) display->minimise();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Move: Moves a surface object to a new display position.
-END-
*********************************************************************************************************************/

static ERR SURFACE_Move(extSurface *Self, struct acMove *Args)
{
   pf::Log log;
   struct acMove move;
   int i;

   if (!Args) return log.warning(ERR::NullArgs)|ERR::Notified;

   // Check if other move messages are queued for this object - if so, do not do anything until the final message is
   // reached.
   //
   // NOTE: This has a downside if the surface object is being fed a sequence of move messages for the purposes of
   // scrolling from one point to another.  Potentially the user may not see the intended effect or witness erratic
   // response times.

   int index = 0;
   uint8_t msgbuffer[sizeof(Message) + sizeof(ActionMessage) + sizeof(struct acMove)];
   while (ScanMessages(&index, MSGID::ACTION, msgbuffer, sizeof(msgbuffer)) IS ERR::Okay) {
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

   int xchange = Args->DeltaX;
   int ychange = Args->DeltaY;

   if ((Self->Flags & RNF::NO_HORIZONTAL) != RNF::NIL) move.DeltaX = 0;
   else move.DeltaX = xchange;

   if ((Self->Flags & RNF::NO_VERTICAL) != RNF::NIL) move.DeltaY = 0;
   else move.DeltaY = ychange;

   move.DeltaZ = 0;

   // If there isn't any movement, return immediately

   if ((move.DeltaX < 1) and (move.DeltaX > -1) and (move.DeltaY < 1) and (move.DeltaY > -1)) {
      return ERR::Failed|ERR::Notified;
   }

   log.traceBranch("X,Y: %d,%d", xchange, ychange);

   // Limit handling

   if (!Self->ParentID) {
      move_layer(Self, Self->X + move.DeltaX, Self->Y + move.DeltaY);
   }
   else {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
      if ((i = find_parent_list(glSurfaces, Self)) != -1) {
         // Horizontal limit handling

         if (xchange < 0) {
            if ((Self->X + xchange) < Self->LeftLimit) {
               if (Self->X < Self->LeftLimit) move.DeltaX = 0;
               else move.DeltaX = -(Self->X - Self->LeftLimit);
            }
         }
         else if (xchange > 0) {
            if ((Self->X + Self->Width) > (glSurfaces[i].Width - Self->RightLimit)) move.DeltaX = 0;
            else if ((Self->X + Self->Width + xchange) > (glSurfaces[i].Width - Self->RightLimit)) {
               move.DeltaX = (glSurfaces[i].Width - Self->RightLimit - Self->Width) - Self->X;
            }
         }

         // Vertical limit handling

         if (ychange < 0) {
            if ((Self->Y + ychange) < Self->TopLimit) {
               if ((Self->Y + Self->Height) < Self->TopLimit) move.DeltaY = 0;
               else move.DeltaY = -(Self->Y - Self->TopLimit);
            }
         }
         else if (ychange > 0) {
            if ((Self->Y + Self->Height) > (glSurfaces[i].Height - Self->BottomLimit)) move.DeltaY = 0;
            else if ((Self->Y + Self->Height + ychange) > (glSurfaces[i].Height - Self->BottomLimit)) {
               move.DeltaY = (glSurfaces[i].Height - Self->BottomLimit - Self->Height) - Self->Y;
            }
         }

         // Second check: If there isn't any movement, return immediately

         if ((!move.DeltaX) and (!move.DeltaY)) {
            return ERR::Failed|ERR::Notified;
         }
      }

      // Move the graphics layer

      move_layer(Self, Self->X + move.DeltaX, Self->Y + move.DeltaY);
   }

/* These lines cause problems for the resizing of offset surface objects.
   if (dmf::hasAnyXOffset(Self->Dimensions)) Self->XOffset += move.DeltaX;
   if (dmf::hasAnyYOffset(Self->Dimensions)) Self->YOffset += move.DeltaY;
*/

   log.traceBranch("Sending redimension notifications");
   struct acRedimension redimension = { (double)Self->X, (double)Self->Y, 0, (double)Self->Width, (double)Self->Height, 0 };
   NotifySubscribers(Self, AC::Redimension, &redimension, ERR::Okay);
   return ERR::Okay|ERR::Notified;
}

/*********************************************************************************************************************
-ACTION-
MoveToBack: Moves a surface object to the back of its container.
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToBack(extSurface *Self)
{
   pf::Log log;

   if (!Self->ParentID) {
      pf::ScopedObjectLock<objDisplay> display(Self->DisplayID);
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
      _expose_surface(Self->ParentID, list, pos, Self->X, Self->Y, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
   }

   refresh_pointer(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToFront: Moves a surface object to the front of its container.
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToFront(extSurface *Self)
{
   pf::Log log;

   log.branch("%s", Self->Name);

   if (!Self->ParentID) {
      pf::ScopedObjectLock<objDisplay> display(Self->DisplayID);
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
   for (auto i=currentindex+1; (glSurfaces[i].Level >= glSurfaces[currentindex].Level); i++) {
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
                  pf::ScopedObjectLock<objSurface> pop(Self->PopOverID);
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
   while (glSurfaces[i+1].Level > level) i++;

   // Count the number of children that have been assigned to this surface object.

   int total;
   for (total=1; glSurfaces[currentindex+total].Level > glSurfaces[currentindex].Level; total++) { };

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

      if (pf::ScopedObjectLock<objBitmap> bitmap(Self->BufferID, 5000); bitmap.granted()) {
         auto area = ClipRectangle(cplist[i].Left, cplist[i].Top, cplist[i].Right, cplist[i].Bottom);
         invalidate_overlap(Self, cplist, currentindex, i, area, *bitmap);
      }

      if (check_volatile(cplist, i)) _redraw_surface(Self->UID, cplist, i, 0, 0, Self->Width, Self->Height, IRF::RELATIVE);
      _expose_surface(Self->UID, cplist, i, 0, 0, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
   }

   if (Self->PopOverID) {
      // Check if the surface that we're popped over is right behind us.  If not, move it forward.

      for (int i=index-1; i > 0; i--) {
         if (cplist[i].Level IS level) {
            if (cplist[i].SurfaceID != Self->PopOverID) {
               if (pf::ScopedObjectLock<objSurface> pop(Self->PopOverID); pop.granted()) {
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
-END-
*********************************************************************************************************************/

static ERR SURFACE_MoveToPoint(extSurface *Self, struct acMoveToPoint *Args)
{
   struct acMove move;

   if ((Args->Flags & MTF::X) != MTF::NIL) move.DeltaX = Args->X - Self->X;
   else move.DeltaX = 0;

   if ((Args->Flags & MTF::Y) != MTF::NIL) move.DeltaY = Args->Y - Self->Y;
   else move.DeltaY = 0;

   move.DeltaZ = 0;

   return Action(AC::Move, Self, &move)|ERR::Notified;
}

//********************************************************************************************************************

static ERR SURFACE_NewOwner(extSurface *Self, struct acNewOwner *Args)
{
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

//********************************************************************************************************************

static ERR SURFACE_NewObject(extSurface *Self)
{
   Self->LeftLimit   = -1000000000;
   Self->RightLimit  = -1000000000;
   Self->TopLimit    = -1000000000;
   Self->BottomLimit = -1000000000;
   Self->Opacity     = 255;
   Self->RootID      = Self->UID;
   Self->WindowType  = glpWindowType;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveCallback: Removes a callback previously inserted by AddCallback().

The RemoveCallback() method is used to remove any callback that has been previously inserted by #AddCallback().

This method is scope restricted, meaning that callbacks added by other objects will not be affected irrespective of
the parameters that are passed to it.

-INPUT-
ptr(func) Callback: Pointer to the callback routine to remove, or NULL to remove all assoicated callback routines.

-ERRORS-
Okay
Search
-END-

*********************************************************************************************************************/

static ERR SURFACE_RemoveCallback(extSurface *Self, struct drw::RemoveCallback *Args)
{
   pf::Log log;
   OBJECTPTR context = nullptr;

   if (Args) {
      if ((Args->Callback) and (Args->Callback->isC())) {
         context = (OBJECTPTR)Args->Callback->Context;
         log.trace("Context: %d, Routine %p, Current Total: %d", context->UID, Args->Callback->Routine, Self->CallbackCount);
      }
      else log.trace("Current Total: %d", Self->CallbackCount);
   }
   else log.trace("Current Total: %d [Remove All]", Self->CallbackCount);

   if (!context) context = ParentContext();

   if (!Self->Callback) return ERR::Okay;

   if ((!Args) or (!Args->Callback) or (!Args->Callback->defined())) {
      // Remove everything relating to this context if no callback was specified.

      int i;
      int shrink = 0;
      for (i=0; i < Self->CallbackCount; i++) {
         if (Self->Callback[i].Object IS context) {
            shrink--;
            continue;
         }
         if (shrink) Self->Callback[i+shrink] = Self->Callback[i];
      }
      Self->CallbackCount += shrink;
      return ERR::Okay;
   }

   if (Args->Callback->isScript()) {
      UnsubscribeAction(Args->Callback->Context, AC::Free);
   }

   // Find the callback entry, then shrink the list.

   int i;
   for (i=0; i < Self->CallbackCount; i++) {
      //log.msg("  %d: #%d, Routine %p", i, Self->Callback[i].Object->UID, Self->Callback[i].Function.Routine);

      if ((Self->Callback[i].Function.isC()) and
          (Self->Callback[i].Function.Context IS context) and
          (Self->Callback[i].Function.Routine IS Args->Callback->Routine)) break;

      if ((Self->Callback[i].Function.isScript()) and
          (Self->Callback[i].Function.Context IS context) and
          (Self->Callback[i].Function.ProcedureID IS Args->Callback->ProcedureID)) break;
   }

   if (i < Self->CallbackCount) {
      while (i < Self->CallbackCount-1) {
         Self->Callback[i] = Self->Callback[i+1];
         i++;
      }
      Self->CallbackCount--;
      return ERR::Okay;
   }
   else {
      if (Args->Callback->Type IS CALL::STD_C) log.warning("Unable to find callback for #%d, routine %p", context->UID, Args->Callback->Routine);
      else log.warning("Unable to find callback for #%d", context->UID);
      return ERR::Search;
   }
}

/*********************************************************************************************************************

-METHOD-
ResetDimensions: Changes the dimensions of a surface.

The ResetDimensions() method provides a simple way of re-declaring the dimensions of a surface object.  This is
sometimes necessary when a surface needs to make a significant alteration to its display configuration.  For
instance if the width of the surface is declared through a combination of `X` and `XOffset` settings and the width
needs to change to a fixed setting, then ResetDimensions() will have to be used.

It is not necessary to define a value for every parameter - only the ones that are relevant to the new dimension
settings.  For instance if `X` and `Width` are set, `XOffset` is ignored and the Dimensions value must include
`DMF::FIXED_X` and `DMF::FIXED_WIDTH` (or the relative equivalents).  Please refer to the #Dimensions field for a full
list of dimension flags that can be specified.

-INPUT-
double X: New X coordinate.
double Y: New Y coordinate.
double XOffset: New X offset.
double YOffset: New Y offset.
double Width: New width.
double Height: New height.
int(DMF) Dimensions: Dimension flags.

-ERRORS-
Okay
NullArgs
AccessMemory: Unable to access internal surface list.
-END-

*********************************************************************************************************************/

static ERR SURFACE_ResetDimensions(extSurface *Self, struct drw::ResetDimensions *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("%.0f,%.0f %.0fx%.0f %.0fx%.0f, Flags: $%.8x", Args->X, Args->Y, Args->XOffset, Args->YOffset, Args->Width, Args->Height, int(Args->Dimensions));

   if (Args->Dimensions IS DMF::NIL) return log.warning(ERR::NullArgs);

   auto dimensions = Args->Dimensions;

   Self->Dimensions = dimensions;

   int cx = Self->X;
   int cy = Self->Y;
   int cx2 = Self->X + Self->Width;
   int cy2 = Self->Y + Self->Height;

   // Turn off drawing and adjust the dimensions of the surface

   //gfx::ForbidDrawing();

   if (dmf::hasScaledX(dimensions)) Self->set(FID_X, Unit(Args->X, FD_SCALED));
   else if (dmf::hasX(dimensions)) Self->setX(Args->X);

   if (dmf::hasScaledY(dimensions)) Self->set(FID_Y, Unit(Args->Y, FD_SCALED));
   else if (dmf::hasY(dimensions)) Self->setY(Args->Y);

   if (dmf::hasScaledXOffset(dimensions)) Self->set(FID_XOffset, Unit(Args->XOffset, FD_SCALED));
   else if (dmf::hasXOffset(dimensions)) Self->setXOffset(Args->XOffset);

   if (dmf::hasScaledYOffset(dimensions)) Self->set(FID_YOffset, Unit(Args->YOffset, FD_SCALED));
   else if (dmf::hasYOffset(dimensions)) Self->setYOffset(Args->YOffset);

   if (dmf::hasScaledHeight(dimensions)) Self->set(FID_Height, Unit(Args->Height, FD_SCALED));
   else if (dmf::hasHeight(dimensions)) Self->setHeight(Args->Height);

   if (dmf::hasScaledWidth(dimensions)) Self->set(FID_Width, Unit(Args->Width, FD_SCALED));
   else if (dmf::hasWidth(dimensions)) Self->setWidth(Args->Width);

   //gfx::PermitDrawing();

   // Now redraw everything within the area that was adjusted

   int nx = Self->X;
   int ny = Self->Y;
   int nx2 = Self->X + Self->Width;
   int ny2 = Self->Y + Self->Height;
   if (cx < nx) nx = cx;
   if (cy < ny) ny = cy;
   if (cx2 > nx2) nx2 = cx2;
   if (cy2 > ny2) ny2 = cy2;

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   int index;
   if ((index = find_surface_list(Self->ParentID ? Self->ParentID : Self->UID)) != -1) {
      _redraw_surface(Self->ParentID, glSurfaces, index, nx, ny, nx2-nx, ny2-ny, IRF::RELATIVE);
      _expose_surface(Self->ParentID, glSurfaces, index, nx, ny, nx2-nx, ny2-ny, EXF::NIL);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ScheduleRedraw: Schedules a redraw operation for the next frame.

Use ScheduleRedraw to indicate that a surface needs to be drawn to the display.  The surface and all child surfaces
will be drawn on the next frame cycle (typically 1/60th of a second).  All manual draw operations for the target
surface are ignored until the scheduled operation is completed.

Scheduling is ideal in situations where a cluster of redraw events may occur within a tight time period, and it
would be inefficient to draw those changes to the display individually.

Note that redraw schedules do not 'see each other', meaning if a surface and a child are both scheduled, this will
trigger two redraw operations when one would suffice.  It is the client's responsibility to target the most
relevant top-level surface for scheduling.

-ERRORS-
Okay
-END-

*********************************************************************************************************************/

static ERR SURFACE_ScheduleRedraw(extSurface *Self)
{
   // TODO Currently defaults to 60FPS, we should get the correct FPS from the Display object.
   const double FPS = 60.0;

   if (Self->RedrawScheduled) return ERR::Okay;

   if (Self->RedrawTimer) {
      Self->RedrawScheduled = true;
      return ERR::Okay;
   }

   if (SubscribeTimer(1.0 / FPS, C_FUNCTION(redraw_timer), &Self->RedrawTimer) IS ERR::Okay) {
      Self->RedrawCountdown = FPS * 30.0;
      Self->RedrawScheduled = true;
      return ERR::Okay;
   }
   else return ERR::Failed;
}

/*********************************************************************************************************************

-ACTION-
SaveImage: Saves the graphics of a surface object.

To store the rendered image of a surface object, use the SaveImage() action.  Calling SaveImage() on a surface object
will cause it to render an image of its contents and save them to the given destination object.  Any child surfaces
in the region will also be included in the resulting image data.

The image data will be saved in the data format that is indicated by the setting in the `ClassID` parameter.  Options
are limited to members of the @Picture class, for example `CLASSID::JPEG` and `CLASSID::PICTURE` (PNG).  If no `ClassID` is
specified, the user's preferred default file format is used.
-END-

*********************************************************************************************************************/

static ERR SURFACE_SaveImage(extSurface *Self, struct acSaveImage *Args)
{
   pf::Log log;
   int j, level;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch();

   // Create a Bitmap that is the same size as the rendered area

   CLASSID class_id = (Args->ClassID IS CLASSID::NIL) ? CLASSID::PICTURE : Args->ClassID;

   objPicture *picture;
   if (NewObject(class_id, &picture) IS ERR::Okay) {
      picture->setFlags(PCF::NEW);
      picture->Bitmap->setWidth(Self->Width);
      picture->Bitmap->setHeight(Self->Height);

      objDisplay *display;
      objBitmap *video_bmp;
      if (access_video(Self->DisplayID, &display, &video_bmp) IS ERR::Okay) {
         picture->Bitmap->setBitsPerPixel(video_bmp->BitsPerPixel);
         picture->Bitmap->setBytesPerPixel(video_bmp->BytesPerPixel);
         picture->Bitmap->setType(video_bmp->Type);
         release_video(display);
      }

      if (InitObject(picture) IS ERR::Okay) {
         // Scan through the surface list and copy each buffer to our picture

         const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
         auto &list = glSurfaces;

         if (auto i = find_surface_list(Self); i != -1) {
            OBJECTID bitmapid = 0;
            for (j=i; (j < int(list.size())) and ((j IS i) or (list[j].Level > list[i].Level)); j++) {
               if (list[j].invisible() or list[j].isCursor()) {
                  // Skip this surface area and all invisible children
                  level = list[j].Level;
                  while (list[j+1].Level > level) j++;
                  continue;
               }

               // If the bitmaps are different, we have found something new to copy

               if (list[j].BitmapID != bitmapid) {
                  bitmapid = list[j].BitmapID;

                  extBitmap *picbmp;
                  picture->get(FID_Bitmap, picbmp);
                  gfx::CopySurface(list[j].SurfaceID, picbmp, BDF::NIL, 0, 0, list[j].Width, list[j].Height,
                     list[j].Left - list[i].Left, list[j].Top - list[i].Top);
               }
            }
         }

         if (Action(AC::SaveImage, picture, Args) IS ERR::Okay) { // Save the picture to disk
            FreeResource(picture);
            return ERR::Okay;
         }
      }

      FreeResource(picture);
      return log.warning(ERR::Failed);
   }
   else return log.warning(ERR::NewObject);
}

/*********************************************************************************************************************

-METHOD-
SetOpacity: Alters the opacity of a surface object.

This method will change the opacity of the surface and execute a redraw to make the changes to the display.

-INPUT-
double Value: The new opacity value between 0 and 100% (ignored if you have set the Adjustment parameter).
double Adjustment: Adjustment value to add or subtract from the existing opacity (set to zero if you want to set a fixed Value instead).

-ERRORS-
Okay: The opacity of the surface object was changed.
NullArgs

*********************************************************************************************************************/

static ERR SURFACE_SetOpacity(extSurface *Self, struct drw::SetOpacity *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Self->BitmapOwnerID != Self->UID) {
      log.warning("Opacity cannot be set on a surface that does not own its bitmap.");
      return ERR::NoSupport;
   }

   double value;
   if (Args->Adjustment) {
      value = (Self->Opacity * (100.0 / 255.0)) + Args->Adjustment;
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
-END-
*********************************************************************************************************************/

static ERR SURFACE_Show(extSurface *Self)
{
   pf::Log log;

   log.traceBranch("%dx%d, %dx%d, Parent: %d, Modal: %d", Self->X, Self->Y, Self->Width, Self->Height, Self->ParentID, Self->Modal);

   ERR notified;
   if (Self->visible()) {
      notified = ERR::Notified;
      return ERR::Okay|ERR::Notified;
   }
   else notified = ERR::NIL;

   if (!Self->ParentID) {
      pf::ScopedObjectLock display(Self->DisplayID);
      if (acShow(*display) IS ERR::Okay) {
         Self->Flags |= RNF::VISIBLE;
         if (Self->hasFocus()) acFocus(*display);
      }
      else return log.warning(ERR::Failed);
   }
   else Self->Flags |= RNF::VISIBLE;

   if (Self->Modal) Self->PrevModalID = gfx::SetModalSurface(Self->UID);

   if (notified IS ERR::NIL) {
      UpdateSurfaceField(Self, &SurfaceRecord::Flags, Self->Flags);

      RedrawSurface(Self->UID, 0, 0, Self->Width, Self->Height, IRF::RELATIVE);
      gfx::ExposeSurface(Self->UID, 0, 0, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);
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

   if ((Self->Width < 1) or (Self->Height < 1)) return;

   if ((Self->X > Bitmap->Clip.Right) or (Self->Y > Bitmap->Clip.Bottom) or
       (Self->X + Self->Width <= Bitmap->Clip.Left) or
       (Self->Y + Self->Height <= Bitmap->Clip.Top)) {
      return;
   }

   ClipRectangle clip = Bitmap->Clip;

   // Adjust clipping and offset values to match the absolute coordinates of our surface object

   auto data = Bitmap->offset(Self->X, Self->Y);

   // Adjust the clipping region of our parent so that it is relative to our surface area

   Bitmap->Clip.Left   -= Self->X;
   Bitmap->Clip.Top    -= Self->Y;
   Bitmap->Clip.Right  -= Self->X;
   Bitmap->Clip.Bottom -= Self->Y;

   // Make sure that the clipping values do not extend outside of our area

   if (Bitmap->Clip.Left   < 0) Bitmap->Clip.Left = 0;
   if (Bitmap->Clip.Top    < 0) Bitmap->Clip.Top = 0;
   if (Bitmap->Clip.Right  > Self->Width)  Bitmap->Clip.Right = Self->Width;
   if (Bitmap->Clip.Bottom > Self->Height) Bitmap->Clip.Bottom = Self->Height;

   if ((Bitmap->Clip.Left < Bitmap->Clip.Right) and (Bitmap->Clip.Top < Bitmap->Clip.Bottom)) {
      // Clear the Bitmap to the background colour if necessary

      if (Self->Colour.Alpha > 0) {
         gfx::DrawRectangle(Bitmap, 0, 0, Self->Width, Self->Height, Bitmap->packPixel(Self->Colour, 255), BAF::FILL);
      }

      process_surface_callbacks(Self, Bitmap);
   }

   Bitmap->Clip = clip;
   Bitmap->Data = data;
}

//********************************************************************************************************************

static ERR consume_input_events(const InputEvent *Events, int Handle)
{
   pf::Log log(__FUNCTION__);

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
               pf::ScopedObjectLock drag(Self->DragID);
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
               if (gfx::LockCursor(Self->UID) IS ERR::Okay) Self->DragStatus = DRAG::ANCHOR;
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

static const FieldDef MovementFlags[] = {
   { "Vertical",   MOVE_VERTICAL },
   { "Horizontal", MOVE_HORIZONTAL },
   { nullptr, 0 }
};

static const FieldDef clWindowType[] = { // This table is copied from pointer_class.c
   { "Default",  SWIN::HOST },
   { "Host",     SWIN::HOST },
   { "Taskbar",  SWIN::TASKBAR },
   { "IconTray", SWIN::ICON_TRAY },
   { "None",     SWIN::NONE },
   { nullptr, 0 }
};

static const FieldDef clTypeFlags[] = {
   { "Root", RT::ROOT },
   { nullptr, 0 }
};

#include "surface_def.c"

static const FieldArray clSurfaceFields[] = {
   { "Drag",         FDF_OBJECTID|FDF_RW, nullptr, SET_Drag, CLASSID::SURFACE },
   { "Buffer",       FDF_OBJECTID|FDF_R,  nullptr, nullptr, CLASSID::BITMAP },
   { "Parent",       FDF_OBJECTID|FDF_RW, nullptr, SET_Parent, CLASSID::SURFACE },
   { "PopOver",      FDF_OBJECTID|FDF_RI, nullptr, SET_PopOver },
   { "MinWidth",     FDF_INT|FDF_RW,  nullptr, SET_MinWidth },
   { "MinHeight",    FDF_INT|FDF_RW,  nullptr, SET_MinHeight },
   { "MaxWidth",     FDF_INT|FDF_RW,  nullptr, SET_MaxWidth },
   { "MaxHeight",    FDF_INT|FDF_RW,  nullptr, SET_MaxHeight },
   { "LeftLimit",    FDF_INT|FDF_RW,  nullptr, SET_LeftLimit },
   { "RightLimit",   FDF_INT|FDF_RW,  nullptr, SET_RightLimit },
   { "TopLimit",     FDF_INT|FDF_RW,  nullptr, SET_TopLimit },
   { "BottomLimit",  FDF_INT|FDF_RW,  nullptr, SET_BottomLimit },
   { "Display",      FDF_OBJECTID|FDF_R, nullptr, nullptr, CLASSID::DISPLAY },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, SET_Flags, &clSurfaceFlags },
   { "X",            FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_XCoord, SET_XCoord },
   { "Y",            FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_YCoord, SET_YCoord },
   { "Width",        FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_Width,  SET_Width },
   { "Height",       FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_Height, SET_Height },
   { "RootLayer",    FDF_OBJECTID|FDF_RW, nullptr, SET_RootLayer },
   { "Align",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clSurfaceAlign },
   { "Dimensions",   FDF_INT|FDF_RW, nullptr, SET_Dimensions, &clSurfaceDimensions },
   { "DragStatus",   FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clSurfaceDragStatus },
   { "Cursor",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Cursor, &clSurfaceCursor },
   { "Colour",       FDF_RGB|FDF_RW },
   { "Type",         FDF_SYSTEM|FDF_INT|FDF_RI, nullptr, nullptr, &clTypeFlags },
   { "Modal",        FDF_INT|FDF_RW, nullptr, SET_Modal },
   // Virtual fields
   { "AbsX",          FDF_VIRTUAL|FDF_INT|FDF_RW, GET_AbsX, SET_AbsX },
   { "AbsY",          FDF_VIRTUAL|FDF_INT|FDF_RW, GET_AbsY, SET_AbsY },
   { "BitsPerPixel",  FDF_VIRTUAL|FDF_INT|FDF_RI, GET_BitsPerPixel, SET_BitsPerPixel },
   { "Bottom",        FDF_VIRTUAL|FDF_INT|FDF_R,  GET_Bottom },
   { "Movement",      FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, nullptr, SET_Movement, &MovementFlags },
   { "Opacity",       FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, GET_Opacity, SET_Opacity },
   { "RevertFocus",   FDF_SYSTEM|FDF_VIRTUAL|FDF_OBJECTID|FDF_W, nullptr, SET_RevertFocus },
   { "Right",         FDF_VIRTUAL|FDF_INT|FDF_R,  GET_Right },
   { "UserFocus",     FDF_VIRTUAL|FDF_INT|FDF_R,  GET_UserFocus },
   { "Visible",       FDF_VIRTUAL|FDF_INT|FDF_RW, GET_Visible, SET_Visible },
   { "VisibleHeight", FDF_VIRTUAL|FDF_INT|FDF_R,  GET_VisibleHeight },
   { "VisibleWidth",  FDF_VIRTUAL|FDF_INT|FDF_R,  GET_VisibleWidth },
   { "VisibleX",      FDF_VIRTUAL|FDF_INT|FDF_R,  GET_VisibleX },
   { "VisibleY",      FDF_VIRTUAL|FDF_INT|FDF_R,  GET_VisibleY },
   { "WindowType",    FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, GET_WindowType, SET_WindowType, &clWindowType },
   { "WindowHandle",  FDF_VIRTUAL|FDF_POINTER|FDF_RW, GET_WindowHandle, SET_WindowHandle },
   // Unit fields
   { "XOffset",       FDF_VIRTUAL|FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_XOffset, SET_XOffset },
   { "YOffset",       FDF_VIRTUAL|FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_YOffset, SET_YOffset },
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
      fl::Path(MOD_PATH));

   return clSurface ? ERR::Okay : ERR::AddClass;
}
