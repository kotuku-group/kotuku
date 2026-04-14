/*********************************************************************************************************************

-CATEGORY-
Name: Cursor
-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

#ifdef __xwindows__

#undef True // X11 name clash
#undef False // X11 name clash

struct XCursor {
   Cursor XCursor;
   PTC CursorID;
   int XCursorID;
};

static XCursor XCursors[] = {
   { 0, PTC::DEFAULT,           XC_left_ptr },
   { 0, PTC::SIZE_BOTTOM_LEFT,  XC_bottom_left_corner },
   { 0, PTC::SIZE_BOTTOM_RIGHT, XC_bottom_right_corner },
   { 0, PTC::SIZE_TOP_LEFT,     XC_top_left_corner },
   { 0, PTC::SIZE_TOP_RIGHT,    XC_top_right_corner },
   { 0, PTC::SIZE_LEFT,         XC_left_side },
   { 0, PTC::SIZE_RIGHT,        XC_right_side },
   { 0, PTC::SIZE_TOP,          XC_top_side },
   { 0, PTC::SIZE_BOTTOM,       XC_bottom_side },
   { 0, PTC::CROSSHAIR,         XC_crosshair },
   { 0, PTC::SLEEP,             XC_clock },
   { 0, PTC::SIZING,            XC_sizing },
   { 0, PTC::SPLIT_VERTICAL,    XC_sb_v_double_arrow },
   { 0, PTC::SPLIT_HORIZONTAL,  XC_sb_h_double_arrow },
   { 0, PTC::MAGNIFIER,         XC_hand2 },
   { 0, PTC::HAND,              XC_hand2 },
   { 0, PTC::HAND_LEFT,         XC_hand1 },
   { 0, PTC::HAND_RIGHT,        XC_hand1 },
   { 0, PTC::TEXT,              XC_xterm },
   { 0, PTC::PAINTBRUSH,        XC_pencil },
   { 0, PTC::STOP,              XC_left_ptr },
   { 0, PTC::INVISIBLE,         XC_dot },
   { 0, PTC::DRAGGABLE,         XC_sizing }
};

static Cursor create_blank_cursor(void)
{
   pf::Log log(__FUNCTION__);
   Pixmap data_pixmap, mask_pixmap;
   XColor black = { 0, 0, 0, 0 };
   Window rootwindow;
   Cursor cursor;

   log.function("Creating blank cursor for X11.");

   rootwindow = DefaultRootWindow(XDisplay);

   data_pixmap = XCreatePixmap(XDisplay, rootwindow, 1, 1, 1);
   mask_pixmap = XCreatePixmap(XDisplay, rootwindow, 1, 1, 1);

   //XSetWindowBackground(XDisplay, data_pixmap, 0);
   //XSetWindowBackground(XDisplay, mask_pixmap, 0);
   //XClearArea(XDisplay, data_pixmap, 0, 0, 1, 1, False);
   //XClearArea(XDisplay, mask_pixmap, 0, 0, 1, 1, False);

   cursor = XCreatePixmapCursor(XDisplay, data_pixmap, mask_pixmap, &black, &black, 0, 0);

   XFreePixmap(XDisplay, data_pixmap); // According to XFree documentation, it is OK to free the pixmaps
   XFreePixmap(XDisplay, mask_pixmap);

   XSync(XDisplay, 0);
   return cursor;
}

static Cursor get_x11_cursor(PTC CursorID)
{
   pf::Log log(__FUNCTION__);

   for (int16_t i=0; i < std::ssize(XCursors); i++) {
      if (XCursors[i].CursorID IS CursorID) return XCursors[i].XCursor;
   }

   log.warning("Cursor #%d is not a recognised cursor ID.", int(CursorID));
   return XCursors[0].XCursor;
}

void init_xcursors(void)
{
   for (int16_t i=0; i < std::ssize(XCursors); i++) {
      if (XCursors[i].CursorID IS PTC::INVISIBLE) XCursors[i].XCursor = create_blank_cursor();
      else XCursors[i].XCursor = XCreateFontCursor(XDisplay, XCursors[i].XCursorID);
   }
}

void free_xcursors(void)
{
   for (int16_t i=0; i < std::ssize(XCursors); i++) {
      if (XCursors[i].XCursor) XFreeCursor(XDisplay, XCursors[i].XCursor);
   }
}
#endif

//********************************************************************************************************************

#ifdef _WIN32
HCURSOR GetWinCursor(PTC CursorID)
{
   for (int16_t i=0; i < std::ssize(winCursors); i++) {
      if (winCursors[i].CursorID IS CursorID) return winCursors[i].WinCursor;
   }

   pf::Log log;
   log.warning("Cursor #%d is not a recognised cursor ID.", int(CursorID));
   return winCursors[0].WinCursor;
}
#endif

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
AccessPointer: Returns a lock on the default pointer object.

Use AccessPointer() to grab a lock on the default pointer object that is active in the system.  This is typically the
first object created from the Pointer class with a name of `SystemPointer`.

Call ~Core.ReleaseObject() to free the lock once it is no longer required.

-RESULT-
obj(Pointer): Returns the address of the default pointer object.

*********************************************************************************************************************/

objPointer * AccessPointer(void)
{
   objPointer *pointer = nullptr;

   if (!glPointerID) {
      if (FindObject("SystemPointer", CLASSID::POINTER, FOF::NIL, &glPointerID) IS ERR::Okay) {
         AccessObject(glPointerID, 2000, (OBJECTPTR *)&pointer);
      }
      return pointer;
   }

   if (AccessObject(glPointerID, 2000, (OBJECTPTR *)&pointer) IS ERR::NoMatchingObject) {
      if (FindObject("SystemPointer", CLASSID::POINTER, FOF::NIL, &glPointerID) IS ERR::Okay) {
         AccessObject(glPointerID, 2000, (OBJECTPTR *)&pointer);
      }
   }

   return pointer;
}

/*********************************************************************************************************************

-FUNCTION-
GetCursorInfo: Retrieves graphics information from the active mouse cursor.

The GetCursorInfo() function is used to retrieve useful information on the graphics structure of the mouse cursor.  It
will return the maximum possible dimensions for custom cursor graphics and indicates the optimal bits-per-pixel setting
for the hardware cursor.

If there is no cursor (e.g. this is likely on touch-screen devices) then all field values will be set to zero.

Note: If the hardware cursor is monochrome, the bits-per-pixel setting will be set to 2 on return.  This does not
indicate a 4 colour cursor image; rather colour 0 is the mask, 1 is the foreground colour (black), 2 is the background
colour (white) and 3 is an XOR pixel.  When creating the bitmap, always set the palette to the RGB values that are
wanted.  The mask colour for the bitmap must refer to colour index 0.

-INPUT-
struct(*CursorInfo) Info: Pointer to a !CursorInfo structure.
structsize Size: The byte-size of the `Info` structure.

-ERRORS-
Okay:
NullArgs:
NoSupport: The device does not support a cursor (common for touch screen displays).

*********************************************************************************************************************/

ERR GetCursorInfo(CursorInfo *Info, int Size)
{
   if (!Info) return ERR::NullArgs;

#ifdef __ANDROID__
   // TODO: Some Android devices probably do support a mouse or similar input device.
   clearmem(Info, sizeof(CursorInfo));
   return ERR::NoSupport;
#else
   Info->Width  = 32;
   Info->Height = 32;
   Info->BitsPerPixel = 1;
   Info->Flags = 0;
   return ERR::Okay;
#endif
}

/*********************************************************************************************************************

-FUNCTION-
GetCursorPos: Returns the coordinates of the UI pointer.

This function will return the current coordinates of the mouse cursor.  If the device is touch-screen based then the
coordinates will reflect the last position that a touch event occurred.

-INPUT-
&double X: Variable that will store the pointer's horizontal coordinate.
&double Y: Variable that will store the pointer's vertical coordinate.

-ERRORS-
Okay
AccessObject: Failed to access the SystemPointer object.

*********************************************************************************************************************/

ERR GetCursorPos(double *X, double *Y)
{
   if (auto pointer = gfx::AccessPointer()) {
      if (X) *X = pointer->X;
      if (Y) *Y = pointer->Y;
      ReleaseObject(pointer);
      return ERR::Okay;
   }
   else {
      pf::Log log(__FUNCTION__);
      return log.warning(ERR::AccessObject);
   }
}

/*********************************************************************************************************************

-FUNCTION-
GetRelativeCursorPos: Returns the coordinates of the pointer cursor, relative to a surface object.

This function is used to retrieve the current coordinates of the pointer cursor. The coordinates are relative to the
surface object that is specified in the Surface argument.

The X and Y parameters will not be set if a failure occurs.

-INPUT-
oid Surface: Unique ID of the surface that the coordinates need to be relative to.
&double X: Variable that will store the pointer's horizontal coordinate.
&double Y: Variable that will store the pointer's vertical coordinate.

-ERRORS-
Okay:
AccessObject: Failed to access the SystemPointer object.

*********************************************************************************************************************/

ERR GetRelativeCursorPos(OBJECTID SurfaceID, double *X, double *Y)
{
   pf::Log log(__FUNCTION__);
   int absx, absy;

   if (get_surface_abs(SurfaceID, &absx, &absy, 0, 0) != ERR::Okay) {
      log.warning("Failed to get info for surface #%d.", SurfaceID);
      return ERR::Search;
   }

   if (auto pointer = gfx::AccessPointer()) {
      if (X) *X = pointer->X - absx;
      if (Y) *Y = pointer->Y - absy;
      ReleaseObject(pointer);
      return ERR::Okay;
   }
   else return log.warning(ERR::AccessObject);
}

/*********************************************************************************************************************

-FUNCTION-
LockCursor: Anchors the cursor so that it cannot move without explicit movement signals.

The LockCursor() function will lock the current pointer position and pass UserMovement signals to the surface
referenced in the `Surface` parameter.  The pointer will not move unless the ~SetCursorPos() function is called.
The anchor is granted on a time-limited basis.  It is necessary to reissue the anchor every time that a UserMovement
signal is intercepted.  Failure to reissue the anchor will return the pointer to its normal state, typically within 200
microseconds.

The anchor can be released at any time by calling the ~UnlockCursor() function.

-INPUT-
oid Surface: Refers to the surface object that the pointer should send movement signals to.

-ERRORS-
Okay
NullArgs
NoSupport: The pointer cannot be locked due to system limitations.
AccessObject: Failed to access the pointer object.

*********************************************************************************************************************/

ERR LockCursor(OBJECTID SurfaceID)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FUNCTION-
RestoreCursor: Returns the pointer image to its original state.

Use the RestoreCursor() function to undo an earlier call to ~SetCursor().  It is necessary to provide the same OwnerID
that was used in the original call to ~SetCursor().

To release ownership of the cursor without changing the current cursor image, use a Cursor setting of
`PTC::NOCHANGE`.

-INPUT-
int(PTC) Cursor: The cursor image that the pointer will be restored to (0 for the default).
oid Owner: The ownership ID that was given in the initial call to SetCursor().

-ERRORS-
Okay
Args
-END-

*********************************************************************************************************************/

ERR RestoreCursor(PTC Cursor, OBJECTID OwnerID)
{
   pf::Log log(__FUNCTION__);

   if (auto pointer = (extPointer *)gfx::AccessPointer()) {
/*
      OBJECTPTR caller;
      caller = CurrentContext();
      log.function("Cursor: %d, Owner: %d, Current-Owner: %d (Caller: %d / Class %d)", Cursor, OwnerID, pointer->CursorOwnerID, caller->UID, caller->ClassID);
*/
      if ((!OwnerID) or (OwnerID IS pointer->CursorOwnerID)) {
         // Restore the pointer to the given cursor image
         if (!OwnerID) gfx::SetCursor(0, CRF::RESTRICT, Cursor, nullptr, pointer->CursorOwnerID);
         else gfx::SetCursor(0, CRF::RESTRICT, Cursor, nullptr, OwnerID);

         pointer->CursorOwnerID   = 0;
         pointer->CursorRelease   = 0;
         pointer->CursorReleaseID = 0;
      }

      // If a cursor change has been buffered, enable it

      if (pointer->BufferOwner) {
         if (OwnerID != pointer->BufferOwner) {
            gfx::SetCursor(pointer->BufferObject, pointer->BufferFlags, pointer->BufferCursor, nullptr, pointer->BufferOwner);
         }
         else pointer->BufferOwner = 0; // Owner and Buffer are identical, so clear due to restored pointer
      }

      ReleaseObject(pointer);
      return ERR::Okay;
   }
   else return ERR::Okay; // The cursor not existing is not necessarily a problem.
}

/*********************************************************************************************************************

-FUNCTION-
SetCursor: Sets the cursor image and can anchor the pointer to any surface.

Use the SetCursor() function to change the pointer image and/or restrict the movement of the pointer to a surface area.

To change the cursor image, set the `Cursor` or `Name` parameters to define the new image.  Valid cursor ID's and
their equivalent names are listed in the documentation for the @Pointer.Cursor field.  If the `Surface` field is set
to a valid surface, the cursor image will switch back to its default once the pointer moves outside of the surface's
area.  If both the `Cursor` and `Name` parameters are `NULL`, the cursor image will remain unchanged from its
current image.

The SetCursor() function accepts the following flags in the `Flags` parameter:

<types lookup="CRF"/>

The `Owner` parameter is used as a locking mechanism to prevent the cursor from being changed whilst it is locked.  We
recommend that it is set to an object ID such as the program's task ID.  As the owner, the cursor remains under your
program's control until ~RestoreCursor() is called.

-INPUT-
oid Surface: Refers to the surface object that the pointer should anchor itself to, if the `RESTRICT` flag is used.  Otherwise, this parameter can be set to a surface that the new cursor image should be limited to.  The object referred to here must be publicly accessible to all tasks.
int(CRF) Flags:  Optional flags that affect the cursor.
int(PTC) Cursor: The ID of the cursor image that is to be set.
cstr Name: The name of the cursor image that is to be set (if `Cursor` is zero).
oid Owner: The object nominated as the owner of the anchor, and/or owner of the cursor image setting.

-ERRORS-
Okay
Args
NoSupport: The pointer cannot be set due to system limitations.
OutOfRange: The cursor ID is outside of acceptable range.
AccessObject: Failed to access the mouse pointer.
LockFailed
NothingDone
-END-

*********************************************************************************************************************/

ERR SetCursor(OBJECTID ObjectID, CRF Flags, PTC CursorID, CSTRING Name, OBJECTID OwnerID)
{
   pf::Log log(__FUNCTION__);
   extPointer *pointer;
   CRF flags;

/*
   if (!OwnerID) {
      log.warning("An Owner must be provided to this function.");
      return ERR::Args;
   }
*/
   // Validate the cursor ID

   if ((int(CursorID) < 0) or (int(CursorID) >= int(PTC::END))) return log.warning(ERR::OutOfRange);

   if (!(pointer = (extPointer *)gfx::AccessPointer())) {
      log.warning("Failed to access the mouse pointer.");
      return ERR::AccessObject;
   }

   if (Name) log.traceBranch("Object: %d, Flags: $%.8x, Owner: %d (Current %d), Cursor: %s", ObjectID, int(Flags), OwnerID, pointer->CursorOwnerID, Name);
   else log.traceBranch("Object: %d, Flags: $%.8x, Owner: %d (Current %d), Cursor: %s", ObjectID, int(Flags), OwnerID, pointer->CursorOwnerID, CursorLookup[int(CursorID)].Name);

   // Extract the cursor ID from the cursor name if no ID was given

   if (CursorID IS PTC::NIL) {
      if (Name) {
         for (int i=0; CursorLookup[i].Name; i++) {
            if (iequals(CursorLookup[i].Name, Name)) {
               CursorID = PTC(CursorLookup[i].Value);
               break;
            }
         }
      }
      else CursorID = pointer->CursorID;
   }

   // Return if the cursor is currently pwn3d by someone

   if ((pointer->CursorOwnerID) and (pointer->CursorOwnerID != OwnerID)) {
      if ((pointer->CursorOwnerID < 0) and (CheckObjectExists(pointer->CursorOwnerID) != ERR::True)) pointer->CursorOwnerID = 0;
      else if ((Flags & CRF::BUFFER) != CRF::NIL) {
         // If the BUFFER option is used, then we can buffer the change so that it
         // will be activated as soon as the current holder releases the cursor.

         log.detail("Request buffered, pointer owned by #%d.", pointer->CursorOwnerID);

         pointer->BufferCursor = CursorID;
         pointer->BufferOwner  = OwnerID;
         pointer->BufferFlags  = Flags;
         pointer->BufferObject = ObjectID;
         ReleaseObject(pointer);
         return ERR::Okay;
      }
      else {
         ReleaseObject(pointer);
         return ERR::LockFailed; // The pointer is locked by someone else
      }
   }

   log.trace("Anchor: %d, Owner: %d, Release: $%x, Cursor: %d", ObjectID, OwnerID, Flags, CursorID);

   // If CRF::NOBUTTONS is used, the cursor can only be set if no mouse buttons are held down at the current time.

   if ((Flags & CRF::NO_BUTTONS) != CRF::NIL) {
      if ((pointer->Buttons[0].LastClicked) or (pointer->Buttons[1].LastClicked) or (pointer->Buttons[2].LastClicked)) {
         ReleaseObject(pointer);
         return ERR::NothingDone;
      }
   }

   // Reset restrictions/anchoring if the correct flags are set, or if the cursor is having a change of ownership.

   if (((Flags & CRF::RESTRICT) != CRF::NIL) or (OwnerID != pointer->CursorOwnerID)) pointer->RestrictID = 0;

   if (OwnerID IS pointer->BufferOwner) pointer->BufferOwner = 0;

   pointer->CursorReleaseID = 0;
   pointer->CursorOwnerID   = 0;
   pointer->CursorRelease   = 0;

   if (CursorID != PTC::NIL) {
      if ((CursorID IS pointer->CursorID) and (CursorID != PTC::CUSTOM)) {
         // Do nothing
      }
      else {
         // Use this routine if our cursor is hardware based

         log.trace("Adjusting hardware/hosted cursor image.");

         #ifdef __xwindows__

            APTR xwin;
            Cursor xcursor;

            if (pointer->SurfaceID) {
               if (ScopedObjectLock<objSurface> surface(pointer->SurfaceID, 1000); surface.granted()) {
                  if (surface->DisplayID) {
                     if (ScopedObjectLock<objDisplay> display(surface->DisplayID, 1000); display.granted()) {
                        if ((display->get(FID_WindowHandle, xwin) IS ERR::Okay) and (xwin)) {
                           xcursor = get_x11_cursor(CursorID);
                           XDefineCursor(XDisplay, (Window)xwin, xcursor);
                           XFlush(XDisplay);
                           pointer->CursorID = CursorID;
                        }
                        else log.warning("Failed to acquire window handle for surface #%d.", pointer->SurfaceID);
                     }
                     else log.warning("Display of surface #%d undefined or inaccessible.", pointer->SurfaceID);
                  }
               }
            }
            else log.warning("Pointer surface undefined or inaccessible.");

         #elif _WIN32

            winSetCursor(GetWinCursor(CursorID));
            pointer->CursorID = CursorID;

         #endif
      }

      if ((ObjectID < 0) and (GetClassID(ObjectID) IS CLASSID::SURFACE) and ((Flags & CRF::RESTRICT) IS CRF::NIL)) {
         pointer->CursorReleaseID = ObjectID; // Release the cursor image if it goes outside of the given surface object
      }
   }

   pointer->CursorOwnerID = OwnerID;

   // Manage button release flag options (useful when the RESTRICT or ANCHOR options are used).

   flags = Flags;
   if ((flags & (CRF::LMB|CRF::MMB|CRF::RMB)) != CRF::NIL) {
      if ((flags & CRF::LMB) != CRF::NIL) {
         if (pointer->Buttons[0].LastClicked) pointer->CursorRelease |= 0x01;
         else flags &= ~(CRF::RESTRICT); // The LMB has already been released by the user, so do not allow restrict/anchoring
      }
      else if ((flags & CRF::RMB) != CRF::NIL) {
         if (pointer->Buttons[1].LastClicked) pointer->CursorRelease |= 0x02;
         else flags &= ~(CRF::RESTRICT); // The MMB has already been released by the user, so do not allow restrict/anchoring
      }
      else if ((flags & CRF::MMB) != CRF::NIL) {
         if (pointer->Buttons[2].LastClicked) pointer->CursorRelease |= 0x04;
         else flags &= ~(CRF::RESTRICT); // The MMB has already been released by the user, so do not allow restrict/anchoring
      }
   }

   if (((flags & CRF::RESTRICT) != CRF::NIL) and (ObjectID)) {
      if ((ObjectID < 0) and (GetClassID(ObjectID) IS CLASSID::SURFACE)) { // Must be a public surface object
         // Restrict the pointer to the specified surface
         pointer->RestrictID = ObjectID;

         #ifdef __xwindows__
            // Pointer grabbing has been turned off for X11 because LBreakout2 was not receiving
            // movement events when run from the desktop.  The reason for this
            // is that only the desktop (which does the X11 input handling) is allowed
            // to grab the pointer.

            //QueueAction(MT_GrabX11Pointer, pointer->Head.UID);
         #endif
      }
      else log.warning("The pointer may only be restricted to public surfaces.");
   }

   ReleaseObject(pointer);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetCustomCursor: Sets the cursor to a customised bitmap image.

Use the SetCustomCursor() function to change the pointer image and/or anchor the position of the pointer so that it
cannot move without permission.  The functionality provided is identical to that of the ~SetCursor() function with
some minor adjustments to allow custom images to be set.

The `Bitmap` that is provided should be within the width, height and bits-per-pixel settings that are returned by the
~GetCursorInfo() function.  If the basic settings are outside the allowable parameters, the `Bitmap` will be trimmed
or resampled appropriately when the cursor is downloaded to the video card.

It may be possible to speed up the creation of custom cursors by drawing directly to the pointer's internal bitmap
buffer rather than supplying a fresh bitmap.  To do this, the `Bitmap` parameter must be `NULL` and it is necessary to
draw to the pointer's bitmap before calling SetCustomCursor().  Note that the bitmap is always returned as a 32-bit,
alpha-enabled graphics area.  The following code illustrates this process:

<pre>
if (auto pointer = gfx::AccessPointer()) {
   if (ScopedObjectLock&lt;objBitmap&gt; bitmap(pointer->BitmapID, 3000); bitmap.granted()) {
      // Adjust clipping to match the cursor size.
      buffer->Clip.Right  = CursorWidth;
      buffer->Clip.Bottom = CursorHeight;
      if (buffer->Clip.Right > buffer->Width) buffer->Clip.Right = buffer->Width;
      if (buffer->Clip.Bottom > buffer->Height) buffer->Clip.Bottom = buffer->Height;

      // Draw to the bitmap here.
      ...

      gfx::SetCustomCursor(ObjectID, NULL, NULL, 1, 1, glTaskID, NULL);
   }
   gfx::ReleasePointer(pointer);
}
</pre>

-INPUT-
oid Surface: Refers to the @Surface object that the pointer should restrict itself to, if the `RESTRICT` flag is used.  Otherwise, this parameter can be set to a surface that the new cursor image should be limited to.  The object referred to here must be publicly accessible to all tasks.
int(CRF) Flags: Optional flags affecting the cursor are set here.
obj(Bitmap) Bitmap: The @Bitmap to set for the mouse cursor.
int HotX: The horizontal position of the cursor hot-spot.
int HotY: The vertical position of the cursor hot-spot.
oid Owner: The object nominated as the owner of the anchor.

-ERRORS-
Okay:
Args:
NoSupport:
AccessObject: Failed to access the internally maintained image object.
-END-

*********************************************************************************************************************/

ERR SetCustomCursor(OBJECTID ObjectID, CRF Flags, objBitmap *Bitmap, int HotX, int HotY, OBJECTID OwnerID)
{
   // If the driver doesn't support custom cursors then divert to gfx::SetCursor()
   return gfx::SetCursor(ObjectID, Flags, PTC::DEFAULT, nullptr, OwnerID);
}

/*********************************************************************************************************************

-FUNCTION-
SetCursorPos: Changes the position of the pointer cursor.

Changes the position of the pointer cursor using coordinates relative to the entire display.

-INPUT-
double X: The new horizontal coordinate for the pointer.
double Y: The new vertical coordinate for the pointer.

-ERRORS-
Okay:
AccessObject: Failed to access the SystemPointer object.

*********************************************************************************************************************/

ERR SetCursorPos(double X, double Y)
{
   struct acMoveToPoint move = { X, Y, 0, MTF::X|MTF::Y };
   if (auto pointer = gfx::AccessPointer()) {
      Action(AC::MoveToPoint, pointer, &move);
      ReleaseObject(pointer);
   }
   else QueueAction(AC::MoveToPoint, glPointerID, &move);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
StartCursorDrag: Attaches an item to the cursor for the purpose of drag and drop.

This function starts a drag and drop operation with the mouse cursor.  The user must be holding the primary mouse
button to initiate the drag and drop operation.

A `Source` object ID is required that indicates the origin of the item being dragged and will be used to retrieve the
data on completion of the drag and drop operation. An `Item` number, which is optional, identifies the item being dragged
from the `Source` object.

The type of data represented by the source item and all other supportable data types are specified in the `Datatypes`
parameter as a null terminated array.  The array is arranged in order of preference, starting with the item's native
data type.  Acceptable data type values are listed in the documentation for the DataFeed action.

The `Surface` parameter allows for a composite surface to be dragged by the mouse cursor as a graphical representation of
the source `Item`.  It is recommended that the graphic be 32x32 pixels in size and no bigger than 64x64 pixels.  The
`Surface` will be hidden on completion of the drag and drop operation.

If the call to StartCursorDrag() is successful, the mouse cursor will operate in drag and drop mode.  The UserMovement
and UserClickRelease actions normally reported from the SystemPointer will now include the `JD_DRAGITEM` flag in the
`ButtonFlags` parameter.  When the user releases the primary mouse button, the drag and drop operation will stop and the
DragDrop action will be passed to the surface immediately underneath the mouse cursor.  Objects that are monitoring for
the DragDrop action on that surface can then contact the Source object with a DataFeed DragDropRequest.  The
resulting data is then passed to the requesting object with a DragDropResult on the DataFeed.

-INPUT-
oid Source:     Refers to an object that is managing the source data.
int Item:       A custom number that represents the item being dragged from the source.
cstr Datatypes: A null terminated byte array that lists the datatypes supported by the source item, in order of conversion preference.
oid Surface:    A 32-bit composite surface that represents the item being dragged.

-ERRORS-
Okay:
NullArgs:
AccessObject:
Failed: The left mouse button is not held by the user.
InUse: A drag and drop operation has already been started.

*********************************************************************************************************************/

ERR StartCursorDrag(OBJECTID Source, int Item, CSTRING Datatypes, OBJECTID Surface)
{
   pf::Log log(__FUNCTION__);

   log.branch("Source: %d, Item: %d, Surface: %d", Source, Item, Surface);

   if (!Source) return log.warning(ERR::NullArgs);

   if (auto pointer = (extPointer *)gfx::AccessPointer()) {
      if (!pointer->Buttons[0].LastClicked) {
         ReleaseObject(pointer);
         return log.warning(ERR::InvalidState);
      }

      if (pointer->DragSourceID) {
         ReleaseObject(pointer);
         return ERR::InUse;
      }

      pointer->DragSurface = Surface;
      pointer->DragItem    = Item;
      pointer->DragSourceID = Source;
      strcopy(Datatypes, pointer->DragData, sizeof(pointer->DragData));

      SURFACEINFO *info;
      if (gfx::GetSurfaceInfo(Surface, &info) IS ERR::Okay) {
         pointer->DragParent = info->ParentID;
      }

      if (Surface) {
         log.trace("Moving draggable surface %d to %dx%d", Surface, pointer->X, pointer->Y);

         pf::ScopedObjectLock surface(Surface);
         if (surface.granted()) {
            acMoveToPoint(*surface, pointer->X+DRAG_XOFFSET, pointer->Y+DRAG_YOFFSET, 0, MTF::X|MTF::Y);
            acShow(*surface);
            acMoveToFront(*surface);
         }
      }

      ReleaseObject(pointer);
      return ERR::Okay;
   }
   else return log.warning(ERR::AccessObject);
}

/*********************************************************************************************************************

-FUNCTION-
UnlockCursor: Undoes an earlier call to LockCursor()

Call this function to undo any earlier calls to LockCursor() and return the mouse pointer to its regular state.

-INPUT-
oid Surface: Refers to the surface object used for calling LockCursor().

-ERRORS-
Okay:
NullArgs:
AccessObject:
ResourceNotLocked: The pointer is not anchored to the given Surface.
-END-

*********************************************************************************************************************/

ERR UnlockCursor(OBJECTID SurfaceID)
{
   pf::Log log(__FUNCTION__);

   if (!SurfaceID) return log.warning(ERR::NullArgs);

   if (auto pointer = (extPointer *)gfx::AccessPointer()) {
      if (pointer->AnchorID IS SurfaceID) {
         pointer->AnchorID = 0;
         ReleaseObject(pointer);
         return ERR::Okay;
      }
      else {
         ReleaseObject(pointer);
         return ERR::ResourceNotLocked;
      }
   }
   else return log.warning(ERR::AccessObject);
}

} // namespace
