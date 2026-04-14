/*********************************************************************************************************************

-CATEGORY-
Name: Surfaces
-END-

*********************************************************************************************************************/

#include "defs.h"

SURFACELIST glSurfaces;
static OBJECTID glModalID = 0;

//********************************************************************************************************************
// Called when windows has an item to be dropped on our display area.

#ifdef _WIN32
void winDragDropFromHost_Drop(int SurfaceID, char *Datatypes)
{
#ifdef WIN_DRAGDROP
   pf::Log log(__FUNCTION__);

   log.branch("Surface: %d", SurfaceID);

   if (auto pointer = gfx::AccessPointer()) {
      // Pass AC::DragDrop to the surface underneath the mouse cursor.  If a surface subscriber accepts the data, it
      // will send a DATA::REQUEST to the relevant display object.  See DISPLAY_DataFeed() and winGetData().

      OBJECTID modal_id = gfx::GetModalSurface();
      if (modal_id IS SurfaceID) modal_id = 0;

      if (!modal_id) {
         SURFACEINFO *info;
         if (gfx::GetSurfaceInfo(pointer->OverObjectID, &info) IS ERR::Okay) {
            pf::ScopedObjectLock display(info->DisplayID);
            if (display.granted()) {
               pf::ScopedObjectLock obj(pointer->OverObjectID);
               if (obj.granted()) acDragDrop(*obj, *display, -1, Datatypes);
            }
         }
         else log.warning(ERR::GetSurfaceInfo);
      }
      else log.msg("Program is modal - drag/drop cancelled.");

      ReleaseObject(pointer);
   }
#endif
}
#endif

//********************************************************************************************************************
// Checks if an object is visible, according to its visibility and its parents visibility.

static bool check_visibility(const SURFACELIST &list, int index)
{
   OBJECTID scan = list[index].SurfaceID;
   for (int i=index; i >= 0; i--) {
      if (list[i].SurfaceID IS scan) {
         if (list[i].invisible()) return false;
         if (!(scan = list[i].ParentID)) return true;
      }
   }

   return true;
}

//********************************************************************************************************************

ERR get_surface_abs(OBJECTID SurfaceID, int *AbsX, int *AbsY, int *Width, int *Height)
{
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   for (auto &record : glSurfaces) {
      if (record.SurfaceID IS SurfaceID) {
         if (AbsX)   *AbsX = record.Left;
         if (AbsY)   *AbsY = record.Top;
         if (Width)  *Width  = record.Width;
         if (Height) *Height = record.Height;
         return ERR::Okay;
      }
   }

   return ERR::Search;
}

static void check_bmp_buffer_depth(extSurface *Self, objBitmap *Bitmap)
{
   pf::Log log(__FUNCTION__);

   if ((Bitmap->Flags & BMF::FIXED_DEPTH) != BMF::NIL) return;  // Don't change bitmaps marked as fixed-depth

   DISPLAYINFO *info;
   if (gfx::GetDisplayInfo(Self->DisplayID, &info) IS ERR::Okay) {
      if (info->BitsPerPixel != Bitmap->BitsPerPixel) {
         log.msg("[%d] Updating buffer Bitmap %dx%dx%d to match new display depth of %dbpp.", Bitmap->UID, Bitmap->Width, Bitmap->Height, Bitmap->BitsPerPixel, info->BitsPerPixel);
         acResize(Bitmap, Bitmap->Width, Bitmap->Height, info->BitsPerPixel);
         Self->LineWidth     = Bitmap->LineWidth;
         Self->BytesPerPixel = Bitmap->BytesPerPixel;
         Self->BitsPerPixel  = Bitmap->BitsPerPixel;
         Self->Data          = Bitmap->Data;
         UpdateSurfaceRecord(Self);
      }
   }
}

//********************************************************************************************************************
// Redraw everything in RegionB that does not intersect with RegionA.

void redraw_nonintersect(OBJECTID SurfaceID, const SURFACELIST &List, int Index,
   const ClipRectangle &Region, const ClipRectangle &RegionB, IRF RedrawFlags, EXF ExposeFlags)
{
   pf::Log log(__FUNCTION__);

   if (!SurfaceID) { // Implemented this check because an invalid SurfaceID has happened before.
      log.warning("SurfaceID == 0");
      return;
   }

   log.traceBranch("redraw_nonintersect: (A) %dx%d,%dx%d Vs (B) %dx%d,%dx%d", Region.Left, Region.Top, Region.Right, Region.Bottom, RegionB.Left, RegionB.Top, RegionB.Right, RegionB.Bottom);

   ExposeFlags |= EXF::ABSOLUTE;

   auto rect = RegionB;

   if (rect.Right > Region.Right) { // Right
      log.trace("redraw_nonrect: Right exposure");

      if (RedrawFlags != IRF(-1)) _redraw_surface(SurfaceID, List, Index, (rect.Left > Region.Right) ? rect.Left : Region.Right, rect.Top, rect.Right, rect.Bottom, RedrawFlags);
      if (ExposeFlags != EXF(-1)) _expose_surface(SurfaceID, List, Index, (rect.Left > Region.Right) ? rect.Left : Region.Right, rect.Top, rect.Right, rect.Bottom, ExposeFlags);
      rect.Right = Region.Right;
      if (rect.Left >= rect.Right) return;
   }

   if (rect.Bottom > Region.Bottom) { // Bottom
      log.trace("redraw_nonrect: Bottom exposure");
      if (RedrawFlags != IRF(-1)) _redraw_surface(SurfaceID, List, Index, rect.Left, (rect.Top > Region.Bottom) ? rect.Top : Region.Bottom, rect.Right, rect.Bottom, RedrawFlags);
      if (ExposeFlags != EXF(-1)) _expose_surface(SurfaceID, List, Index, rect.Left, (rect.Top > Region.Bottom) ? rect.Top : Region.Bottom, rect.Right, rect.Bottom, ExposeFlags);
      rect.Bottom = Region.Bottom;
      if (rect.Top >= rect.Bottom) return;
   }

   if (rect.Top < Region.Top) { // Top
      log.trace("redraw_nonrect: Top exposure");
      if (RedrawFlags != IRF(-1)) _redraw_surface(SurfaceID, List, Index, rect.Left, rect.Top, rect.Right, (rect.Bottom < Region.Top) ? rect.Bottom : Region.Top, RedrawFlags);
      if (ExposeFlags != EXF(-1)) _expose_surface(SurfaceID, List, Index, rect.Left, rect.Top, rect.Right, (rect.Bottom < Region.Top) ? rect.Bottom : Region.Top, ExposeFlags);
      rect.Top = Region.Top;
   }

   if (rect.Left < Region.Left) { // Left
      log.trace("redraw_nonrect: Left exposure");
      if (RedrawFlags != IRF(-1)) _redraw_surface(SurfaceID, List, Index, rect.Left, rect.Top, (rect.Right < Region.Left) ? rect.Right : Region.Left, rect.Bottom, RedrawFlags);
      if (ExposeFlags != EXF(-1)) _expose_surface(SurfaceID, List, Index, rect.Left, rect.Top, (rect.Right < Region.Left) ? rect.Right : Region.Left, rect.Bottom, ExposeFlags);
   }
}

//********************************************************************************************************************

ERR _redraw_surface(OBJECTID SurfaceID, const SURFACELIST &list, int index,
   int Left, int Top, int Right, int Bottom, IRF Flags)
{
   pf::Log log("redraw_surface");
   static thread_local int8_t recursive = 0;

   if ((list[index].Flags & RNF::TOTAL_REDRAW) != RNF::NIL) {
      // If the TOTAL_REDRAW flag is set against the surface then the entire surface must be redrawn regardless
      // of the circumstances.  This is often required for algorithmic effects as seen in the Blur class.

      Left   = list[index].Left;
      Top    = list[index].Top;
      Right  = list[index].Right;
      Bottom = list[index].Bottom;
   }
   else if ((Flags & IRF::RELATIVE) != IRF::NIL) {
      Left   = list[index].Left + Left;
      Top    = list[index].Top + Top;
      Right  = Left + Right;
      Bottom = Top + Bottom;
      Flags &= ~IRF::RELATIVE;
   }

   log.traceBranch("[%d] %d Size: %dx%d,%dx%d Expose: %dx%d,%dx%d", SurfaceID, index, list[index].Left, list[index].Top, list[index].Width, list[index].Height, Left, Top, Right-Left, Bottom-Top);

   if ((list[index].transparent()) and (!recursive)) {
      log.trace("Passing draw request to parent (I am transparent)");
      if (auto parent_index = find_surface_list(list[index].ParentID, list.size()); parent_index != -1) {
         _redraw_surface(list[parent_index].SurfaceID, list, parent_index, Left, Top, Right, Bottom, Flags & (~IRF::IGNORE_CHILDREN));
      }
      else log.trace("Failed to find parent surface #%d", list[index].ParentID); // No big deal, this often happens when freeing a bunch of surfaces due to the parent/child relationships.
      return ERR::Okay;
   }

   // Check if any of the parent surfaces are invisible

   if ((Flags & IRF::FORCE_DRAW) IS IRF::NIL) {
      if (list[index].invisible() or (check_visibility(list, index) IS FALSE)) {
         log.trace("Surface is not visible.");
         return ERR::Okay;
      }
   }

   // Check if the exposed dimensions are outside of our boundary and/or our parent(s) boundaries.  If so then we must restrict the exposed dimensions.

   if ((Flags & IRF::FORCE_DRAW) != IRF::NIL) {
      if (Left   < list[index].Left)   Left   = list[index].Left;
      if (Top    < list[index].Top)    Top    = list[index].Top;
      if (Right  > list[index].Right)  Right  = list[index].Right;
      if (Bottom > list[index].Bottom) Bottom = list[index].Bottom;
   }
   else {
      OBJECTID parent_id = SurfaceID;
      int i = index;
      while (parent_id) {
         while ((list[i].SurfaceID != parent_id) and (i > 0)) i--;

         if (list[i].BitmapID != list[index].BitmapID) break; // Stop if we encounter a separate bitmap

         if (Left   < list[i].Left)   Left   = list[i].Left;
         if (Top    < list[i].Top)    Top    = list[i].Top;
         if (Right  > list[i].Right)  Right  = list[i].Right;
         if (Bottom > list[i].Bottom) Bottom = list[i].Bottom;

         parent_id = list[i].ParentID;
      }
   }

   if ((Left >= Right) or (Top >= Bottom)) return ERR::Okay;

   // Draw the surface graphics into the bitmap buffer

   if (ScopedObjectLock<extSurface> surface(list[index].SurfaceID, 5000); surface.granted()) {
      log.trace("Area: %dx%d,%dx%d", Left, Top, Right-Left, Bottom-Top);

      if (ScopedObjectLock<extBitmap> bitmap(list[index].BitmapID, 5000); bitmap.granted()) {
         // Check if there has been a change in the video bit depth.  If so, regenerate the bitmap with a matching depth.

         check_bmp_buffer_depth(*surface, *bitmap);
         auto clip = ClipRectangle(Left, Top, Right, Bottom);
         _redraw_surface_do(*surface, list, index, clip, *bitmap, Flags & (IRF::FORCE_DRAW|IRF::IGNORE_CHILDREN|IRF::IGNORE_NV_CHILDREN));
      }
      else return log.warning(ERR::AccessObject);
   }
   else { // If the object does not exist then its task has crashed and we need to remove it from the surface list.
      if (surface.error IS ERR::NoMatchingObject) {
         log.warning("Removing references to surface object #%d (owner crashed).", list[index].SurfaceID);
         untrack_layer(list[index].SurfaceID);
      }
      else log.warning("Unable to access surface object #%d, error %d.", list[index].SurfaceID, int(surface.error));
      return surface.error;
   }

   // We have done the redraw, so now we can send invalidation messages to any intersecting -child- surfaces for this region.  This process is
   // not recursive (notice the use of IRF::IGNORE_CHILDREN) but all children will be covered due to the way the tree is traversed.

   if ((Flags & IRF::IGNORE_CHILDREN) IS IRF::NIL) {
      log.trace("Redrawing intersecting child surfaces.");
      int level = list[index].Level;
      for (unsigned i=index+1; i < list.size(); i++) {
         if (list[i].Level <= level) break; // End of list - exit this loop

         if ((Flags & IRF::IGNORE_NV_CHILDREN) != IRF::NIL) {
            // Ignore children except for those that are volatile
            if (!list[i].isVolatile()) continue;
         }
         else {
            if (((Flags & IRF::SINGLE_BITMAP) != IRF::NIL) and (list[i].BitmapID != list[index].BitmapID)) continue;
         }

         if (list[i].isCursor() or list[i].invisible()) {
            continue; // Skip non-visible children
         }

         if ((list[i].Right > Left) and (list[i].Bottom > Top) and
             (list[i].Left < Right) and (list[i].Top < Bottom)) {
            recursive++;
            _redraw_surface(list[i].SurfaceID, list, i, Left, Top, Right, Bottom, Flags|IRF::IGNORE_CHILDREN);
            recursive--;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// This function fulfils the recursive drawing requirements of _redraw_surface() and is not intended for any other use.

void _redraw_surface_do(extSurface *Self, const SURFACELIST &list, int Index, ClipRectangle &Area,
   extBitmap *DestBitmap, IRF Flags)
{
   pf::Log log("redraw_surface");

   if (Self->transparent()) return;

   if (Index >= std::ssize(list)) log.warning("Index %d > %d", Index, int(list.size()));

   auto abs = Area;

   if (abs.Left   < list[Index].Left)   abs.Left   = list[Index].Left;
   if (abs.Top    < list[Index].Top)    abs.Top    = list[Index].Top;
   if (abs.Right  > list[Index].Right)  abs.Right  = list[Index].Right;
   if (abs.Bottom > list[Index].Bottom) abs.Bottom = list[Index].Bottom;

   int i;
   if ((Flags & IRF::FORCE_DRAW) IS IRF::NIL) {
      int level = list[Index].Level + 1;   // The +1 is used to include children contained in the surface object

      for (i=Index+1; (i < std::ssize(list)) and (list[i].Level > 1); i++) {
         if (list[i].Level < level) level = list[i].Level;

         // If the listed object obscures our surface area, analyse the region around it

         if (list[i].Level <= level) {
            // If we have a bitmap buffer and the underlying child region also has its own bitmap,
            // we have to ignore it in order for our graphics buffer to be correct when exposes are made.

            if (list[i].BitmapID != Self->BufferID) continue;
            if (list[i].invisible()) continue;

            // Check for an intersection and respond to it

            auto listx      = list[i].Left;
            auto listy      = list[i].Top;
            auto listright  = list[i].Right;
            auto listbottom = list[i].Bottom;

            if ((listx < Area.Right) and (listy < Area.Bottom) and (listright > Area.Left) and (listbottom > Area.Top)) {
               if (list[i].isCursor()) {
                  // Objects like the pointer cursor are ignored completely.  They are redrawn following exposure.

                  return;
               }
               else if (list[i].transparent()) {
                  // If the surface object is see-through then we will ignore its bounds, but legally
                  // it can also contain child surface objects that are solid.  For that reason,
                  // we have to 'go inside' to check for solid children and draw around them.

                  _redraw_surface_do(Self, list, i, Area, DestBitmap, Flags);
                  return;
               }

               if (((Flags & (IRF::IGNORE_CHILDREN|IRF::IGNORE_NV_CHILDREN)) IS IRF::NIL) and (list[i].Level > list[Index].Level)) {
                  // Client intends to redraw all children surfaces.
                  // In this case, we may as well ignore children when they are smaller than 100x100 in size,
                  // because splitting our drawing process into four sectors is probably going to be slower
                  // than just redrawing the entire background in one shot.

                  if (list[i].Width + list[i].Height <= 200) continue;
               }

               if (listx <= Area.Left) listx = Area.Left;
               else {
                  auto clip = ClipRectangle(Area.Left, Area.Top, listx, Area.Bottom);
                  _redraw_surface_do(Self, list, Index, clip, DestBitmap, Flags); // left
               }

               if (listright >= Area.Right) listright = Area.Right;
               else {
                  auto clip = ClipRectangle(listright, Area.Top, Area.Right, Area.Bottom);
                  _redraw_surface_do(Self, list, Index, clip, DestBitmap, Flags); // right
               }

               if (listy <= Area.Top) listy = Area.Top;
               else {
                  auto clip = ClipRectangle(listx, Area.Top, listright, listy);
                  _redraw_surface_do(Self, list, Index, clip, DestBitmap, Flags); // top
               }

               if (listbottom < Area.Bottom) {
                  auto clip = ClipRectangle(listx, listbottom, listright, Area.Bottom);
                  _redraw_surface_do(Self, list, Index, clip, DestBitmap, Flags); // bottom
               }

               return;
            }
         }
      }
   }

   log.traceBranch("Index %d, %dx%d,%dx%d", Index, Area.Left, Area.Top, Area.Right - Area.Left, Area.Bottom - Area.Top);

   // If we have been called recursively due to the presence of volatile/invisible regions (see above),
   // our Index field will not match with the surface that is referenced in Self.  We need to ensure
   // correctness before going any further.

   if (list[Index].SurfaceID != Self->UID) Index = find_surface_list(Self, list.size());

   // Prepare the buffer so that it matches the exposed area

   uint8_t *data;
   int xo = 0, yo = 0;
   if (Self->BitmapOwnerID != Self->UID) {
      for (i=Index; (i > 0) and (list[i].SurfaceID != Self->BitmapOwnerID); i--);
      xo = list[Index].Left - list[i].Left;
      yo = list[Index].Top - list[i].Top;
      data = DestBitmap->offset(xo, yo);
   }
   else data = DestBitmap->Data;

   DestBitmap->Clip.Left   = Area.Left - list[Index].Left - xo; // Untested
   DestBitmap->Clip.Top    = Area.Top - list[Index].Top - yo;
   DestBitmap->Clip.Right  = Area.Right - list[Index].Left - xo;
   DestBitmap->Clip.Bottom = Area.Bottom - list[Index].Top - yo;

   // THIS SHOULD NOT BE NEEDED - but occasionally it detects surface problems (bugs in other areas of the surface code?)

   if (((DestBitmap->Clip.Left) < 0) or ((DestBitmap->Clip.Top) < 0) or
       ((DestBitmap->Clip.Right) > DestBitmap->Width) or ((DestBitmap->Clip.Bottom) > DestBitmap->Height)) {
      log.warning("Invalid coordinates detected (outside of the surface area).  CODE FIX REQUIRED!");
      if ((DestBitmap->Clip.Left) < 0) DestBitmap->Clip.Left = 0;
      if ((DestBitmap->Clip.Top) < 0)  DestBitmap->Clip.Top = 0;
      DestBitmap->Clip.Right = DestBitmap->Width;
      DestBitmap->Clip.Bottom = DestBitmap->Height;
   }

   // Clear the background

   if (((Self->Flags & RNF::PRECOPY) != RNF::NIL) and ((Self->Flags & RNF::COMPOSITE) IS RNF::NIL)) {
      prepare_background(Self, list, Index, DestBitmap, abs, STAGE_PRECOPY);
   }
   else if ((Self->Flags & RNF::COMPOSITE) != RNF::NIL) {
      gfx::DrawRectangle(DestBitmap, 0, 0, Self->Width, Self->Height, DestBitmap->packPixel(0, 0, 0, 0), BAF::FILL);
   }
   else if (Self->Colour.Alpha > 0) {
      gfx::DrawRectangle(DestBitmap, 0, 0, Self->Width, Self->Height, DestBitmap->packPixel(Self->Colour.Red, Self->Colour.Green, Self->Colour.Blue), BAF::FILL);
   }

   // Draw graphics to the buffer

   tlFreeExpose = DestBitmap->UID;

      process_surface_callbacks(Self, DestBitmap);

   tlFreeExpose = 0;

   // After-copy management

   if ((Self->Flags & RNF::COMPOSITE) IS RNF::NIL) {
      if ((Self->Flags & RNF::AFTER_COPY) != RNF::NIL) {
         #ifdef DBG_DRAW_ROUTINES
            log.trace("After-copy graphics drawing.");
         #endif
         prepare_background(Self, list, Index, DestBitmap, abs, STAGE_AFTERCOPY);
      }
      else if ((Self->Type & RT::ROOT) != RT::NIL) {
         // If the surface object is part of a global background, we have to look for the root layer and check if it has the AFTER_COPY flag set.

         if (auto i = find_surface_list(Self->RootID); i != -1) {
            if ((list[i].Flags & RNF::AFTER_COPY) != RNF::NIL) {
               #ifdef DBG_DRAW_ROUTINES
                  log.trace("After-copy graphics drawing.");
               #endif
               prepare_background(Self, list, Index, DestBitmap, abs, STAGE_AFTERCOPY);
            }
         }
      }
   }

   DestBitmap->Data = data;
}

//********************************************************************************************************************

ERR RedrawSurface(OBJECTID SurfaceID, int Left, int Top, int Right, int Bottom, IRF Flags)
{
   pf::Log log(__FUNCTION__);

   if (tlNoDrawing) {
      log.trace("tlNoDrawing: %d", tlNoDrawing);
      return ERR::Okay;
   }

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   int index;
   if ((index = find_surface_list(SurfaceID)) IS -1) {
      log.traceWarning("Unable to find surface #%d in surface list.", SurfaceID);
      return ERR::Search;
   }

   return _redraw_surface(SurfaceID, glSurfaces, index, Left, Top, Right, Bottom, Flags);
}

//********************************************************************************************************************
// Scans the surfacelist for the 'true owner' of a given bitmap.

int find_bitmap_owner(const SURFACELIST &List, int Index)
{
   auto owner = Index;
   for (auto i=Index; i >= 0; i--) {
      if (List[i].SurfaceID IS List[owner].ParentID) {
         if (List[i].BitmapID != List[owner].BitmapID) return owner;
         owner = i;
      }
   }
   return owner;
}

//********************************************************************************************************************
// This function is responsible for inserting new surface objects into the list of layers for positional/depth management.
//
// Surface levels start at 1, which indicates the top-most level.

ERR track_layer(extSurface *Self)
{
   pf::Log log(__FUNCTION__);

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   SurfaceRecord record;

   record.ParentID      = Self->ParentID;
   record.SurfaceID     = Self->UID;
   record.BitmapID      = Self->BufferID;
   record.DisplayID     = Self->DisplayID;
   record.PopOverID     = Self->PopOverID;
   record.Flags         = Self->Flags;
   record.X             = Self->X;
   record.Y             = Self->Y;
   record.Opacity       = Self->Opacity;
   record.BitsPerPixel  = Self->BitsPerPixel;
   record.BytesPerPixel = Self->BytesPerPixel;
   record.LineWidth     = Self->LineWidth;
   record.Data          = Self->Data;
   record.Cursor        = int8_t(Self->Cursor);
   record.RootID        = Self->RootID;
   record.Width         = Self->Width;
   record.Height        = Self->Height;

   // Find the position at which the surface object should be inserted

   if (!Self->ParentID) {
      record.setArea(Self->X, Self->Y, Self->X + Self->Width, Self->Y + Self->Height);
      record.Level  = 1;
      glSurfaces.push_back(record);
   }
   else {
      int parent;
      if ((parent = find_parent_list(glSurfaces, Self)) IS -1) {
         log.warning("Failed to find parent surface #%d.", Self->ParentID);
         return ERR::Search;
      }

      record.setArea(glSurfaces[parent].Left + Self->X, glSurfaces[parent].Top + Self->Y,
         record.X + Self->Width, record.Y + Self->Height);

      record.Level = glSurfaces[parent].Level + 1;

      // Find the insertion point

      unsigned i = parent + 1;
      while ((i < glSurfaces.size()) and (glSurfaces[i].Level >= record.Level)) {
         if ((Self->Flags & RNF::STICK_TO_FRONT) != RNF::NIL) {
            if ((glSurfaces[i].Flags & RNF::POINTER) != RNF::NIL) break;
         }
         else if (((glSurfaces[i].Flags & RNF::STICK_TO_FRONT) != RNF::NIL) and (glSurfaces[i].Level IS record.Level)) break;
         i++;
      }

      if (i < glSurfaces.size()) glSurfaces.insert(glSurfaces.begin() + i, record);
      else glSurfaces.push_back(record);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

void untrack_layer(OBJECTID ObjectID)
{
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   if (auto i = find_surface_list(ObjectID); i != -1) {
      #ifdef DBG_LAYERS
         pf::Log log(__FUNCTION__);
         log.msg("%d, Index: %d/%d", ObjectID, i, int(glSurfaces.size()));
         //print_layer_list("untrack_layer", glSurfaces, i);
      #endif

      // Mark all subsequent child layers as invisible

      size_t end;
      for (end=i+1; (end < glSurfaces.size()) and (glSurfaces[end].Level > glSurfaces[i].Level); end++) {
         glSurfaces[end].Flags &= ~RNF::VISIBLE;
      }

      if (end >= glSurfaces.size()) glSurfaces.resize(i);
      else glSurfaces.erase(glSurfaces.begin() + i, glSurfaces.begin() + end);

      #ifdef DBG_LAYERS
         print_layer_list("untrack_layer_end", glSurfaces, i);
      #endif
   }
}

//********************************************************************************************************************

ERR update_surface_copy(extSurface *Self)
{
   if (!Self) return ERR::NullArgs;
   if (!Self->initialised()) return ERR::Okay;

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   auto &list = glSurfaces;

   // Calculate absolute coordinates by looking for the parent of this object.  Then simply add the parent's
   // absolute X,Y coordinates to our X and Y fields.

   int i;
   int absx, absy;
   if (Self->ParentID) {
      if ((i = find_parent_list(list, Self)) != -1) {
         absx = list[i].Left + Self->X;
         absy = list[i].Top + Self->Y;
         i = find_surface_list(Self);
      }
      else {
         absx = 0;
         absy = 0;
      }
   }
   else {
      absx = Self->X;
      absy = Self->Y;
      i = find_surface_list(Self);
   }

   if (i != -1) {
      list[i].ParentID      = Self->ParentID;
      //list[i].SurfaceID    = Self->UID; Never changes
      list[i].BitmapID      = Self->BufferID;
      list[i].DisplayID     = Self->DisplayID;
      list[i].PopOverID     = Self->PopOverID;
      list[i].X             = Self->X;
      list[i].Y             = Self->Y;
      list[i].Left          = absx;        // Synonym: Left
      list[i].Top           = absy;        // Synonym: Top
      list[i].Width         = Self->Width;
      list[i].Height        = Self->Height;
      list[i].Right         = absx + Self->Width;
      list[i].Bottom        = absy + Self->Height;
      list[i].Flags         = Self->Flags;
      list[i].Opacity       = Self->Opacity;
      list[i].BitsPerPixel  = Self->BitsPerPixel;
      list[i].BytesPerPixel = Self->BytesPerPixel;
      list[i].LineWidth     = Self->LineWidth;
      list[i].Data          = Self->Data;
      list[i].Cursor        = int8_t(Self->Cursor);
      list[i].RootID        = Self->RootID;

      // Rebuild absolute coordinates of child objects

      auto level = list[i].Level;
      int c = i+1;
      while ((c < std::ssize(list)) and (list[c].Level > level)) {
         for (auto j=c-1; j >= 0; j--) {
            if (list[j].SurfaceID IS list[c].ParentID) {
               list[c].Left   = list[j].Left + list[c].X;
               list[c].Top    = list[j].Top  + list[c].Y;
               list[c].Right  = list[c].Left + list[c].Width;
               list[c].Bottom = list[c].Top  + list[c].Height;
               break;
            }
         }
         c++;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

void move_layer_pos(SURFACELIST &List, int Src, int Dest)
{
   if (Src IS Dest) return;

   int children;
   for (children=Src+1; (children < std::ssize(List)) and (List[children].Level > List[Src].Level); children++);
   children -= Src;

   if ((Dest >= Src) and (Dest <= Src + children)) return;

   // Move the source entry into a buffer
   auto tmp = SURFACELIST(List.begin() + Src, List.begin() + Src + children);
   List.erase(List.begin() + Src, List.begin() + Src + children);

   // Insert the saved content
   int target_index = (Dest > Src) ? Dest - children : Dest;
   List.insert(List.begin() + target_index, tmp.begin(), tmp.end());
}

//********************************************************************************************************************
// This function is responsible for managing the resizing of top-most surface objects and is also used by some of the
// field management functions for Width/Height adjustments.
//
// This function is also useful for skipping the dimension limits normally imposed when resizing.

ERR resize_layer(extSurface *Self, int X, int Y, int Width, int Height, int InsideWidth,
   int InsideHeight, int BPP, double RefreshRate, int DeviceFlags)
{
   if (!Width)  Width = Self->Width;
   if (!Height) Height = Self->Height;

   if (!Self->initialised()) {
      Self->X = X;
      Self->Y = Y;
      Self->Width  = Width;
      Self->Height = Height;
      return ERR::Okay;
   }

   if ((Self->X IS X) and (Self->Y IS Y) and (Self->Width IS Width) and (Self->Height IS Height) and
       (Self->ParentID)) {
      return ERR::Okay;
   }

   pf::Log log;

   log.traceBranch("resize_layer() %dx%d,%dx%d TO %dx%d,%dx%dx%d", Self->X, Self->Y, Self->Width, Self->Height, X, Y, Width, Height, BPP);

   if (Self->BitmapOwnerID IS Self->UID) {
      pf::ScopedObjectLock<objBitmap> bitmap(Self->BufferID, 5000);
      if (bitmap.granted()) {
         if (bitmap->resize(Width, Height, BPP) IS ERR::Okay) {
            Self->LineWidth     = bitmap->LineWidth;
            Self->BytesPerPixel = bitmap->BytesPerPixel;
            Self->BitsPerPixel  = bitmap->BitsPerPixel;
            Self->Data          = bitmap->Data;
            UpdateSurfaceRecord(Self);
         }
         else return log.warning(ERR::Resize);
      }
      else return log.warning(ERR::AccessObject);
   }

   if (!Self->ParentID) {
      if ((Self->MaxWidth > 0) and (Width > Self->MaxWidth)) Width = Self->MaxWidth;
      if ((Self->MaxHeight > 0) and (Height > Self->MaxHeight)) Height = Self->MaxHeight;
      if (InsideWidth < Width) InsideWidth = Width;
      if (InsideHeight < Height) InsideHeight = Height;

      if (ScopedObjectLock<objDisplay> display(Self->DisplayID, 5000); display.granted()) { // NB: SetDisplay() always processes coordinates relative to the client area in order to resolve issues when in hosted mode.
         if (display->setDisplay(X, Y, Width, Height, InsideWidth, InsideHeight, BPP, RefreshRate, DeviceFlags) != ERR::Okay) {
            return log.warning(ERR::Redimension);
         }

         Width = display->get<int>(FID_Width);
         Height = display->get<int>(FID_Height);
      }
      else return log.warning(ERR::AccessObject);
   }

   auto oldx = Self->X;
   auto oldy = Self->Y;
   auto oldw = Self->Width;
   auto oldh = Self->Height;

   Self->X = X;
   Self->Y = Y;
   Self->Width  = Width;
   Self->Height = Height;
   UpdateSurfaceRecord(Self);

   if (!Self->initialised()) return ERR::Okay;

   // Send a Resize notification to our subscribers.  Basically, this informs our surface children to resize themselves
   // to the new dimensions.  Surface objects are not permitted to redraw themselves when they receive the Redimension
   // notification - we will send a delayed draw message later in this routine.

   forbidDrawing();

   struct acRedimension redimension = { (double)X, (double)Y, 0, (double)Width, (double)Height, (double)BPP };
   NotifySubscribers(Self, AC::Redimension, &redimension, ERR::Okay);

   permitDrawing();

   if (Self->invisible()) return ERR::Okay;

   if (!tlNoDrawing) {
      // Post the drawing update.  This method is the only reliable way to generate updates when our surface may
      // contain children that belong to foreign tasks.

      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
      auto &list = glSurfaces;

      int index;
      if ((index = find_surface_list(Self)) IS -1) { // The surface might not be listed if the parent is in the process of being dstroyed.
         return ERR::Search;
      }

      pf::Log log;
      log.traceBranch("Redrawing the resized surface.");

      _redraw_surface(Self->UID, list, index, list[index].Left, list[index].Top, list[index].Right, list[index].Bottom, IRF::NIL);
      _expose_surface(Self->UID, list, index, 0, 0, Self->Width, Self->Height, EXF::CHILDREN|EXF::REDRAW_VOLATILE_OVERLAP);

      if (Self->ParentID) {
         // Update external regions on all four sides that have been exposed by the resize, for example due to a decrease in area or a coordinate shift.
         //
         // Note: tlVolatileIndex determines the point at which volatile exposes will start.  We want volatile exposes to start just after our target surface, and not
         // anything that sits behind us in the containing parent.

         int vindex;
         for (vindex=index+1; (vindex < std::ssize(list)) and (list[vindex].Level > list[index].Level); vindex++);
         tlVolatileIndex = vindex;

         int parent_index;
         for (parent_index=index-1; parent_index >= 0; parent_index--) {
            if (list[parent_index].SurfaceID IS Self->ParentID) break;
         }

         ClipRectangle region_b(list[parent_index].Left + oldx, list[parent_index].Top + oldy,
            (list[parent_index].Left + oldx) + oldw, (list[parent_index].Top + oldy) + oldh);

         ClipRectangle region_a(list[index].Left, list[index].Top, list[index].Right, list[index].Bottom);

         if (Self->BitmapOwnerID IS Self->UID) {
            redraw_nonintersect(Self->ParentID, list, parent_index, region_a, region_b, IRF(-1), EXF::CHILDREN|EXF::REDRAW_VOLATILE);
         }
         else redraw_nonintersect(Self->ParentID, list, parent_index, region_a, region_b, IRF::NIL, EXF::CHILDREN|EXF::REDRAW_VOLATILE);

         tlVolatileIndex = 0;
      }
   }

   refresh_pointer(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

void process_surface_callbacks(extSurface *Self, extBitmap *Bitmap)
{
   pf::Log log(__FUNCTION__);

   #ifdef DBG_DRAW_ROUTINES
      log.traceBranch("Bitmap: %d, Count: %d", Bitmap->UID, Self->CallbackCount);
   #endif

   for (int i=0; i < Self->CallbackCount; i++) {
      Bitmap->Opacity = 255;
      auto &cb = Self->Callback[i].Function;
      if (cb.isC()) {
         auto routine = (void (*)(APTR, extSurface *, objBitmap *, APTR))cb.Routine;

         #ifdef DBG_DRAW_ROUTINES
            pf::Log log(__FUNCTION__);
            log.branch("%d/%d: Routine: %p, Object: %p, Context: %p", i, Self->CallbackCount, routine, Self->Callback[i].Object, cb.Context);
         #endif

         if (cb.Context) {
            pf::SwitchContext context(cb.Context);
            routine(cb.Context, Self, Bitmap, cb.Meta);
         }
         else routine(Self->Callback[i].Object, Self, Bitmap, cb.Meta);
      }
      else if (cb.isScript()) {
         sc::Call(cb, std::to_array<ScriptArg>({
            { "Surface", Self, FD_OBJECTPTR },
            { "Bitmap",  Bitmap, FD_OBJECTPTR }
         }));
      }
   }

   Bitmap->Opacity = 255;
}

//********************************************************************************************************************
// This routine will modify a clip region to match the visible area, as governed by parent surfaces within the same
// bitmap space (if MatchBitmap is TRUE).  It also scans the whole parent tree to ensure that all parents are visible,
// returning TRUE or FALSE accordingly.  If the region is completely obscured regardless of visibility settings, -1 is
// returned.

int8_t restrict_region_to_parents(const SURFACELIST &List, int Index, ClipRectangle &Area, bool MatchBitmap)
{
   bool visible = true;
   OBJECTID id = List[Index].SurfaceID;
   for (int j=Index; (j >= 0) and (id); j--) {
      if (List[j].SurfaceID IS id) {
         if (List[j].invisible()) visible = false;

         id = List[j].ParentID;

         if ((!MatchBitmap) or (List[j].BitmapID IS List[Index].BitmapID)) {
            if (Area.Left   < List[j].Left)   Area.Left   = List[j].Left;
            if (Area.Top    < List[j].Top)    Area.Top    = List[j].Top;
            if (Area.Right  > List[j].Right)  Area.Right  = List[j].Right;
            if (Area.Bottom > List[j].Bottom) Area.Bottom = List[j].Bottom;
         }
      }
   }

   if ((Area.Right <= Area.Left) or (Area.Bottom <= Area.Top)) {
      Area.Right  = Area.Left;
      Area.Bottom = Area.Top;
      return -1;
   }

   return visible;
}

//********************************************************************************************************************

void forbidDrawing(void)
{
   tlNoDrawing++;
   tlNoExpose++;
}

void forbidExpose(void)
{
   tlNoExpose++;
}

void permitDrawing(void)
{
   tlNoDrawing--;
   tlNoExpose--;
}

void permitExpose(void)
{
   tlNoExpose--;
}

#ifdef DBG_LAYERS
void print_layer_list(STRING Function, int POI)
{
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   auto &list = glSurfaces;
   fprintf(stderr, "LAYER LIST: %d, From %s()\n", int(list.size()), Function);

   int j;
   for (int i=0; i < int(list.size()); i++) {
      fprintf(stderr, "%.2d: ", i);
      for (j=0; j < list[i].Level; j++) fprintf(stderr, " ");
      fprintf(stderr, "#%d, Parent: %d, Flags: $%.8x", list[i].SurfaceID, list[i].ParentID, list[i].Flags);

      // Highlight any point of interest

      if (i IS POI) fprintf(stderr, " <---- POI");

      // Error checks

      if (!list[i].SurfaceID) fprintf(stderr, " <---- ERROR");
      else if (CheckObjectExists(list[i].SurfaceID) != ERR::True) fprintf(stderr, " <---- OBJECT MISSING");

      // Does the parent exist in the layer list?

      if (list[i].ParentID) {
         for (j=i-1; j >= 0; j--) {
            if (list[j].SurfaceID IS list[i].ParentID) break;
         }
         if (j < 0) fprintf(stderr, " <---- PARENT MISSING");
      }

      fprintf(stderr, "\n");
   }
}
#endif

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
CheckIfChild: Check if a surface is the child of another surface.

This function checks if a surface identified by the `Child` value is the child of the surface identified by the `Parent`
value.  `ERR::True` is returned if the surface is confirmed as being a child of the parent, or if the `Child` and `Parent`
values are equal.  All other return codes indicate false or failure.

-INPUT-
oid Parent: The surface that is assumed to be the parent.
oid Child: The child surface to check.

-ERRORS-
True: The `Child` surface belongs to the `Parent`.
False: The `Child` surface is not a child of `Parent`.
NullArgs

*********************************************************************************************************************/

ERR CheckIfChild(OBJECTID ParentID, OBJECTID ChildID)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Parent: %d, Child: %d", ParentID, ChildID);

   if ((!ParentID) or (!ChildID)) return ERR::NullArgs;

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   // Find the parent surface, then examine its children to find a match for child ID.

   if (auto i = find_surface_list(ParentID); i != -1) {
      auto level = glSurfaces[i].Level;
      for (++i; (i < std::ssize(glSurfaces)) and (glSurfaces[i].Level > level); i++) {
         if (glSurfaces[i].SurfaceID IS ChildID) {
            log.trace("Child confirmed.");
            return ERR::True;
         }
      }
   }

   return ERR::False;
}

/*********************************************************************************************************************

-FUNCTION-
CopySurface: Copies surface graphics data into any bitmap object

This function will copy the graphics data from any surface object to a target @Bitmap.  This is
the fastest and most convenient way to get graphics information out of any surface.  As surfaces are buffered, it is
guaranteed that the result will not be obscured by any overlapping surfaces that are on the display.

-INPUT-
oid Surface: The ID of the surface object to copy from.
obj(Bitmap) Bitmap: Must reference a target @Bitmap object.
int(BDF) Flags:  Optional flags.
int X:      The horizontal source coordinate.
int Y:      The vertical source coordinate.
int Width:  The width of the graphic that will be copied.
int Height: The height of the graphic that will be copied.
int XDest:  The horizontal target coordinate.
int YDest:  The vertical target coordinate.

-ERRORS-
Okay
NullArgs
Search: The supplied `Surface` ID did not refer to a recognised surface object
AccessObject

*********************************************************************************************************************/

ERR CopySurface(OBJECTID SurfaceID, objBitmap *Bitmap, BDF Flags,
          int X, int Y, int Width, int Height, int XDest, int YDest)
{
   pf::Log log(__FUNCTION__);

   if ((!SurfaceID) or (!Bitmap)) return log.warning(ERR::NullArgs);

   log.traceBranch("%dx%d,%dx%d TO %dx%d, Flags $%.8x", X, Y, Width, Height, XDest, YDest, int(Flags));

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   BITMAPSURFACE surface;
   for (unsigned i=0; i < glSurfaces.size(); i++) {
      if (glSurfaces[i].SurfaceID IS SurfaceID) {
         if (X < 0) { XDest -= X; Width  += X; X = 0; }
         if (Y < 0) { YDest -= Y; Height += Y; Y = 0; }
         if (X+Width  > glSurfaces[i].Width)  Width  = glSurfaces[i].Width-X;
         if (Y+Height > glSurfaces[i].Height) Height = glSurfaces[i].Height-Y;

         // Find the bitmap root

         int root = find_bitmap_owner(glSurfaces, i);

         SurfaceRecord list_i = glSurfaces[i];
         SurfaceRecord list_root = glSurfaces[root];

         if ((Flags & BDF::REDRAW) != BDF::NIL) {
            auto state = tlNoDrawing;
            tlNoDrawing = 0;
            RedrawSurface(SurfaceID, list_i.Left+X, list_i.Top+Y, list_i.Left+X+Width, list_i.Top+Y+Height, IRF::FORCE_DRAW);
            tlNoDrawing = state;
         }

         if (((Flags & BDF::DITHER) != BDF::NIL) or (!list_root.Data)) {
            if (ScopedObjectLock<extBitmap> src(list_root.BitmapID, 4000); src.granted()) {
               auto data = src->offset(list_i.Left - list_root.Left, list_i.Top - list_root.Top);

               src->Clip.Left   = 0; // ???
               src->Clip.Top    = 0;
               src->Clip.Right  = list_i.Width;
               src->Clip.Bottom = list_i.Height;

               if (((list_i.Flags & RNF::COMPOSITE) != RNF::NIL)) {
                  gfx::CopyArea(*src, Bitmap, BAF::BLEND|(((Flags & BDF::DITHER) != BDF::NIL) ? BAF::DITHER : BAF::NIL), X, Y, Width, Height, XDest, YDest);
               }
               else gfx::CopyArea(*src, Bitmap, ((Flags & BDF::DITHER) != BDF::NIL) ? BAF::DITHER : BAF::NIL, X, Y, Width, Height, XDest, YDest);

               src->Data = data;
               return ERR::Okay;
            }
            else return log.warning(ERR::AccessObject);
         }
         else {
            surface.Data          = list_root.Data;
            surface.XOffset       = list_i.Left - list_root.Left;
            surface.YOffset       = list_i.Top - list_root.Top;
            surface.LineWidth     = list_root.LineWidth;
            surface.Height        = list_i.Height;
            surface.BitsPerPixel  = list_root.BitsPerPixel;
            surface.BytesPerPixel = list_root.BytesPerPixel;

            bool composite = ((list_i.Flags & RNF::COMPOSITE) != RNF::NIL);

            if (composite) gfx::CopyRawBitmap(&surface, Bitmap, CSRF::DEFAULT_FORMAT|CSRF::OFFSET|CSRF::ALPHA, X, Y, Width, Height, XDest, YDest);
            else gfx::CopyRawBitmap(&surface, Bitmap, CSRF::DEFAULT_FORMAT|CSRF::OFFSET, X, Y, Width, Height, XDest, YDest);

            return ERR::Okay;
         }
      }
   }

   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
ExposeSurface: Exposes the content of a surface to the display.

This expose routine will expose all content within a defined surface area, copying it to the display.  This will
include all child surfaces that intersect with the region being exposed if you set the `EXF::CHILDREN` flag.

-INPUT-
oid Surface: The ID of the surface object that will be exposed.
int X:       The horizontal coordinate of the area to expose.
int Y:       The vertical coordinate of the area to expose.
int Width:   The width of the expose area.
int Height:  The height of the expose area.
int(EXF) Flags: Optional flags - using `CHILDREN` will expose all intersecting child regions.

-ERRORS-
Okay
NullArgs
Search: The `Surface` ID does not refer to an existing surface object

*********************************************************************************************************************/

ERR ExposeSurface(OBJECTID SurfaceID, int X, int Y, int Width, int Height, EXF Flags)
{
   pf::Log log(__FUNCTION__);

   if (tlNoDrawing) return ERR::Okay;
   if (!SurfaceID) return ERR::NullArgs;
   if ((Width < 1) or (Height < 1)) return ERR::Okay;

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   int index;
   if ((index = find_surface_list(SurfaceID)) IS -1) { // The surface might not be listed if the parent is in the process of being dstroyed.
      log.traceWarning("Surface %d is not in the surfacelist.", SurfaceID);
      return ERR::Search;
   }

   return _expose_surface(SurfaceID, glSurfaces, index, X, Y, Width, Height, Flags);
}

/*********************************************************************************************************************

-FUNCTION-
GetModalSurface: Returns the current modal surface (if defined).

This function returns the modal surface for the running process.  Returns zero if no modal surface is active.

-RESULT-
oid: The UID of the modal surface, or zero.

*********************************************************************************************************************/

OBJECTID GetModalSurface(void)
{
   // Safety check: Confirm that the object still exists
   if ((glModalID) and (CheckObjectExists(glModalID) != ERR::True)) {
      pf::Log log(__FUNCTION__);
      log.msg("Modal surface #%d no longer exists.", glModalID);
      glModalID = 0;
   }

   return glModalID;
}

/*********************************************************************************************************************

-FUNCTION-
GetSurfaceCoords: Returns the dimensions of a surface.

GetSurfaceCoords() retrieves the dimensions that describe a surface object's area as X, Y, Width and Height.  This is
the fastest way to retrieve surface dimensions when access to the object structure is not already available.

-INPUT-
oid Surface: The surface to query.  If zero, the top-level display is queried.
&int X: The X coordinate of the surface is returned here.
&int Y: The Y coordinate of the surface is returned here.
&int AbsX: The absolute X coordinate of the surface is returned here.
&int AbsY: The absolute Y coordinate of the surface is returned here.
&int Width: The width of the surface is returned here.
&int Height: The height of the surface is returned here.

-ERRORS-
Okay
Search: The supplied `Surface` ID did not refer to a recognised surface object.

*********************************************************************************************************************/

ERR GetSurfaceCoords(OBJECTID SurfaceID, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height)
{
   pf::Log log(__FUNCTION__);

   if (!SurfaceID) {
      DISPLAYINFO *display;
      if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
         if (X)      *X = 0;
         if (Y)      *Y = 0;
         if (AbsX)   *AbsX = 0;
         if (AbsY)   *AbsY = 0;
         if (Width)  *Width  = display->Width;
         if (Height) *Height = display->Height;
         return ERR::Okay;
      }
      else return ERR::SystemCall;
   }

   int i;
   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   if ((i = find_surface_list(SurfaceID)) IS -1) return ERR::Search;

   if (X)      *X      = glSurfaces[i].X;
   if (Y)      *Y      = glSurfaces[i].Y;
   if (Width)  *Width  = glSurfaces[i].Width;
   if (Height) *Height = glSurfaces[i].Height;
   if (AbsX)   *AbsX   = glSurfaces[i].Left;
   if (AbsY)   *AbsY   = glSurfaces[i].Top;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GetSurfaceFlags: Retrieves the `Flags` field from a @Surface.

This function returns the current `Flags` field from a @Surface.  It provides the same result as reading the field
directly, however it is considered advantageous in circumstances where the overhead of locking a surface object for a
read operation is undesirable.

For information on the available flags, please refer to the @Surface.Flags field.

-INPUT-
oid Surface: The surface to query.  If zero, the top-level surface is queried.
&int(RNF) Flags: The flags value is returned here.

-ERRORS-
Okay
NullArgs
AccessMemory

*********************************************************************************************************************/

ERR GetSurfaceFlags(OBJECTID SurfaceID, RNF *Flags)
{
   pf::Log log(__FUNCTION__);

   if (Flags) *Flags = RNF::NIL;
   else return log.warning(ERR::NullArgs);

   if (!SurfaceID) return log.warning(ERR::NullArgs);

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
   int i;
   if ((i = find_surface_list(SurfaceID)) IS -1) return ERR::Search;

   *Flags = glSurfaces[i].Flags;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GetSurfaceInfo: Retrieves display information for any surface object without having to access it directly.

GetSurfaceInfo() is used for quickly retrieving basic information from surfaces, allowing the client to bypass the
~Core.AccessObject() function.  The resulting structure values are good only up until the next call to this function,
at which point those values will be overwritten.

-INPUT-
oid Surface: The unique ID of a @Surface to query.  If zero, the root surface is returned.
&struct(SurfaceInfo) Info: This parameter will receive a !SurfaceInfo pointer that describes the surface object.

-ERRORS-
Okay
NullArgs
Search: The supplied SurfaceID did not refer to a recognised surface object.

*********************************************************************************************************************/

ERR GetSurfaceInfo(OBJECTID SurfaceID, SURFACEINFO **Info)
{
   pf::Log log(__FUNCTION__);
   static thread_local SURFACEINFO info;

   // Note that a SurfaceID of zero is fine (returns the root surface).

   if (!Info) return log.warning(ERR::NullArgs);

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   int i, root;
   if (!SurfaceID) {
      i = 0;
      root = 0;
   }
   else {
      if ((i = find_surface_list(SurfaceID)) IS -1) return ERR::Search;
      root = find_bitmap_owner(glSurfaces, i);
   }

   info.ParentID      = glSurfaces[i].ParentID;
   info.BitmapID      = glSurfaces[i].BitmapID;
   info.DisplayID     = glSurfaces[i].DisplayID;
   info.Data          = glSurfaces[root].Data;
   info.Flags         = glSurfaces[i].Flags;
   info.X             = glSurfaces[i].X;
   info.Y             = glSurfaces[i].Y;
   info.Width         = glSurfaces[i].Width;
   info.Height        = glSurfaces[i].Height;
   info.AbsX          = glSurfaces[i].Left;
   info.AbsY          = glSurfaces[i].Top;
   info.Level         = glSurfaces[i].Level;
   info.BytesPerPixel = glSurfaces[root].BytesPerPixel;
   info.BitsPerPixel  = glSurfaces[root].BitsPerPixel;
   info.LineWidth     = glSurfaces[root].LineWidth;
   *Info = &info;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GetUserFocus: Returns the ID of the surface that currently has the user's focus.

This function returns the unique ID of the surface that has the user's focus.

-RESULT-
oid: Returns the ID of the surface object that has the user focus, or zero on failure.

*********************************************************************************************************************/

OBJECTID GetUserFocus(void)
{
   const std::lock_guard<std::recursive_mutex> lock(glFocusLock);
   if (!glFocusList.empty()) {
      auto objectid = glFocusList[0];
      return objectid;
   }
   else return 0;
}

/*********************************************************************************************************************

-FUNCTION-
GetVisibleArea: Returns the visible region of a @Surface.

The GetVisibleArea() function returns the visible area of a @Surface, which is based on its position within its parent
surfaces. The resulting coordinates are relative to point `0,0` of the queried surface. If the surface is not obscured,
then the resulting coordinates will be `(0,0),(Width,Height)`.

-INPUT-
oid Surface: The surface to query.  If zero, the top-level display will be queried.
&int X: The X coordinate of the visible area.
&int Y: The Y coordinate of the visible area.
&int AbsX: The absolute X coordinate of the visible area.
&int AbsY: The absolute Y coordinate of the visible area.
&int Width: The visible width of the surface.
&int Height: The visible height of the surface.

-ERRORS-
Okay
Failed
Search: The supplied `Surface` ID did not refer to a recognised surface object.

*********************************************************************************************************************/

ERR GetVisibleArea(OBJECTID SurfaceID, int *X, int *Y, int *AbsX, int *AbsY, int *Width, int *Height)
{
   pf::Log log(__FUNCTION__);

   if (!SurfaceID) {
      DISPLAYINFO *display;
      if (gfx::GetDisplayInfo(0, &display) IS ERR::Okay) {
         if (X) *X = 0;
         if (Y) *Y = 0;
         if (Width)  *Width = display->Width;
         if (Height) *Height = display->Height;
         if (AbsX)   *AbsX = 0;
         if (AbsY)   *AbsY = 0;
         return ERR::Okay;
      }
      else return ERR::SystemCall;
   }

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   int i;
   if ((i = find_surface_list(SurfaceID)) IS -1) return ERR::Search;

   auto clip = glSurfaces[i].area();

   restrict_region_to_parents(glSurfaces, i, clip, false);

   if (X)      *X      = clip.Left   - glSurfaces[i].Left;
   if (Y)      *Y      = clip.Top    - glSurfaces[i].Top;
   if (Width)  *Width  = clip.Right  - clip.Left;
   if (Height) *Height = clip.Bottom - clip.Top;
   if (AbsX)   *AbsX   = clip.Left;
   if (AbsY)   *AbsY   = clip.Top;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetModalSurface: Enables a modal surface for the current task.

Any surface that is created by a task can be enabled as a modal surface.  A surface that has been enabled as modal
becomes the central point for all GUI interaction with the task.  All other I/O between the user and surfaces
maintained by the task will be ignored for as long as the target surface remains modal.

A task can switch off the current modal surface by calling this function with a `Surface` parameter of zero.

If a surface is modal at the time that this function is called, it is not possible to switch to a new modal surface
until the current modal state is dropped.

-INPUT-
oid Surface: The surface to enable as modal.

-RESULT-
oid: The object ID of the previous modal surface is returned (zero if there was no currently modal surface).

*********************************************************************************************************************/

OBJECTID SetModalSurface(OBJECTID SurfaceID)
{
   pf::Log log(__FUNCTION__);

   log.branch("#%d, CurrentFocus: %d", SurfaceID, gfx::GetUserFocus());

   // Check if the surface is invisible, in which case the mode has to be diverted to the modal that was previously
   // targetted or turned off altogether if there was no previously modal surface.

   if (SurfaceID) {
      OBJECTID divert = 0;
      if (ScopedObjectLock<extSurface> surface(SurfaceID, 3000); surface.granted()) {
         if (surface->invisible()) {
            divert = surface->PrevModalID;
            if (!divert) SurfaceID = 0;
         }
      }
      if (divert) return gfx::SetModalSurface(divert);
   }

   if (SurfaceID IS -1) return glModalID; // Return the current modal surface, don't do anything else
   else if (!SurfaceID) { // Turn off modal surface mode for the current task
      auto old_modal = glModalID;
      glModalID = 0;
      return old_modal;
   }
   else { // We are the new modal surface
      auto old_modal = glModalID;
      glModalID = SurfaceID;
      pf::ScopedObjectLock<objSurface> surface(SurfaceID);
      if (surface.granted()) {
         acMoveToFront(*surface);

         // Do not change the primary focus if the targetted surface already has it (this ensures that if any children have the focus, they will keep it).

         if ((surface->Flags & RNF::HAS_FOCUS) IS RNF::NIL) {
            acFocus(*surface);
         }
      }
      return old_modal;
   }
}

/*********************************************************************************************************************

-FUNCTION-
WindowHook: Adds a function hook for receiving window messages from a host desktop.

Adds a function hook for receiving window events from a host desktop.

-INPUT-
oid SurfaceID: A hosted surface to be monitored.
int(WH) Event: A window hook event.
ptr(func) Callback: A function to callback when the event is triggered.

-ERRORS-
Okay
NullArgs

-END-

*********************************************************************************************************************/

ERR WindowHook(OBJECTID SurfaceID, WH Event, FUNCTION *Callback)
{
   if ((!SurfaceID) or (Event IS WH::NIL) or (!Callback)) return ERR::NullArgs;

   const WinHook hook(SurfaceID, Event);
   glWindowHooks[hook] = *Callback;
   return ERR::Okay;
}

} // namespace
