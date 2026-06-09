/*********************************************************************************************************************

-CLASS-
Pointer: Tracks pointer position, button state and cursor image selection.

The Pointer class represents the active pointing device used by the display system.  It tracks global pointer
coordinates, the surface and object under the hot spot, button state, drag state and the cursor image currently being
shown to the user.

On hosted systems such as Windows and X11, pointer movement and cursor images are synchronised with the host windowing
system.  On native displays, the display module is responsible for drawing and managing the cursor directly.

A system-wide pointer object named `SystemPointer` is created automatically.  Applications and module code should use
this shared object, usually via ~Display.AccessPointer(), when reading pointer state or changing cursor behaviour.

-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

static ERR GET_ButtonOrder(extPointer *, std::string_view &);
static ERR GET_ButtonState(extPointer *, int *);

static ERR SET_ButtonOrder(extPointer *, std::string_view &);
static ERR SET_MaxSpeed(extPointer *, int);
static ERR PTR_SET_X(extPointer *, double);
static ERR PTR_SET_Y(extPointer *, double);

#ifdef _WIN32
static ERR PTR_SetWinCursor(extPointer *, struct ptrSetWinCursor *);
static FunctionField mthSetWinCursor[]  = { { "Cursor", FD_INT }, { nullptr, 0 } };
#endif

#ifdef __xwindows__
#undef True
#undef False
static ERR PTR_GrabX11Pointer(extPointer *, struct ptrGrabX11Pointer *);
static ERR PTR_UngrabX11Pointer(extPointer *);
static FunctionField mthGrabX11Pointer[] = { { "Surface", FD_INT }, { nullptr, 0 } };
#endif

static int glDefaultSpeed = 160;
static double glDefaultAcceleration = 0.8;
static TIMER glRepeatTimer = 0;

static ERR repeat_timer(extPointer *, int64_t, int64_t);
static void set_pointer_defaults(extPointer *);
static int examine_chain(extPointer *, int, SURFACELIST &, int);
static bool get_over_object(extPointer *);
static void process_ptr_button(extPointer *, struct dcDeviceInput *);
static void process_ptr_movement(extPointer *, struct dcDeviceInput *);
static void process_ptr_wheel(extPointer *, struct dcDeviceInput *);

//********************************************************************************************************************

inline void add_input(CSTRING Debug, InputEvent &input, JTYPE Flags, OBJECTID RecipientID, OBJECTID OverID,
   double AbsX, double AbsY, double OverX, double OverY)
{
   //kt::Log log(__FUNCTION__);
   //log.trace("Type: %s, Value: %.2f, Recipient: %d, Over: %d %.2fx%.2f, Abs: %.2fx%.2f %s",
   //   (input->Type < JET::END) ? glInputNames[input->Type] : (CSTRING)"", input->Value, RecipientID, OverID, OverX, OverY, AbsX, AbsY, Debug);

   input.Mask        = glInputType[int(input.Type)].Mask;
   input.Flags       = glInputType[int(input.Type)].Flags | Flags;
   input.RecipientID = RecipientID;
   input.OverID      = OverID;
   input.AbsX        = AbsX;
   input.AbsY        = AbsY;
   input.X           = OverX;
   input.Y           = OverY;

   const std::lock_guard<std::recursive_mutex> lock(glInputLock);
   glInputEvents.push_back(input);
}

//********************************************************************************************************************
#ifdef _WIN32
static ERR PTR_SetWinCursor(extPointer *Self, struct ptrSetWinCursor *Args)
{
   winSetCursor(GetWinCursor(Args->Cursor));
   Self->CursorID = Args->Cursor;
   return ERR::Okay;
}
#endif

//********************************************************************************************************************
// Private action used to grab the window cursor under X11.  Can only be executed by the task that owns the pointer.

#ifdef __xwindows__
static ERR PTR_GrabX11Pointer(extPointer *Self, struct ptrGrabX11Pointer *Args)
{
   APTR xwin;
   OBJECTPTR surface;

   if (AccessObject(Self->SurfaceID, 5000, &surface) IS ERR::Okay) {
      surface->get(FID_WindowHandle, xwin);
      ReleaseObject(surface);

      if (xwin) XGrabPointer(XDisplay, (Window)xwin, 1, 0, GrabModeAsync, GrabModeAsync, (Window)xwin, None, CurrentTime);
   }

   return ERR::Okay;
}

static ERR PTR_UngrabX11Pointer(extPointer *Self)
{
   XUngrabPointer(XDisplay, CurrentTime);
   return ERR::Okay;
}
#endif

/*********************************************************************************************************************

-ACTION-
DataFeed: Sends device input events to the pointer.

Use DataFeed with the `DATA::DEVICE_INPUT` data type to submit one or more `dcDeviceInput` records to a pointer object.
The supplied records are interpreted in the same way as input received from host or native hardware.

Button presses are stateful.  If a submitted record presses a button, the client must later submit the corresponding
release record so click, drag and repeat handling can return to a consistent state.

-END-

*********************************************************************************************************************/

// NOTE: See input_event_loop() if you are looking for the main input event processing loop.  Incoming events are
// pushed onto the glInputEvents queue and processed in the main thread at a later time.

static ERR PTR_DataFeed(extPointer *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::DEVICE_INPUT) {
      if (auto input = (struct dcDeviceInput *)Args->Buffer) {
         for (int i=0; i < std::ssize(Self->Buttons); i++) {
            if ((Self->Buttons[i].LastClicked) and (CheckResourceExists(Self->Buttons[i].LastClicked) != ERR::Okay)) Self->Buttons[i].LastClicked = 0;
         }

         for (auto i=sizeof(struct dcDeviceInput); i <= (size_t)Args->Size; i+=sizeof(struct dcDeviceInput), input++) {
            if ((int(input->Type) < 1) or (int(input->Type) >= int(JET::END))) continue;

            input->Flags |= glInputType[int(input->Type)].Flags;

            //log.traceBranch("Incoming Input: %s, Value: %.2f, Flags: $%.8x, Time: %" PF64, (input->Type < JET::END) ? glInputNames[input->Type] : (STRING)"", input->Value, input->Flags, input->Timestamp);

            if (input->Type IS JET::WHEEL) process_ptr_wheel(Self, input);
            else if ((input->Flags & JTYPE::BUTTON) != JTYPE::NIL) process_ptr_button(Self, input);
            else process_ptr_movement(Self, input);
         }
      }
   }
   else return log.warning(ERR::WrongType);

   return ERR::Okay;
}

//********************************************************************************************************************

static void process_ptr_button(extPointer *Self, struct dcDeviceInput *Input)
{
   kt::Log log(__FUNCTION__);
   InputEvent userinput;
   OBJECTID target;
   int buttonflag, bi;

   clearmem(&userinput, sizeof(userinput));
   userinput.Value     = Input->Values[0];
   userinput.Timestamp = Input->Timestamp;
   userinput.Type      = Input->Type;
   userinput.Flags     = Input->Flags;
   userinput.DeviceID  = Input->DeviceID;

   if (!userinput.Timestamp) userinput.Timestamp = PreciseTime();

   auto uiflags = userinput.Flags;

   if ((userinput.Type >= JET::BUTTON_1) and (userinput.Type <= JET::BUTTON_10)) {
      bi = int(userinput.Type) - int(JET::BUTTON_1);
      buttonflag = Self->ButtonOrderFlags[bi];
   }
   else {
      // This subroutine is used when the button is not one of the regular 1-10 available button types

      add_input("IrregularButton", userinput, uiflags, Self->OverObjectID, Self->OverObjectID,
         Self->X, Self->Y, Self->OverX, Self->OverY);
      return;
   }

   if (userinput.Value <= 0) {
      // Button released.  Button releases are always reported relative to the object that received the original button press.
      // The surface immediately below the pointer does not receive any information about the release.

      log.trace("Button %d released.", bi);

      // Restore the cursor to its default state if cursor release flags have been met

      if ((Self->CursorRelease & buttonflag) and (Self->CursorOwnerID)) {
         gfx::RestoreCursor(PTC::DEFAULT, 0);
      }

      if (Self->Buttons[bi].LastClicked) {
         int absx, absy;
         if (get_surface_abs(Self->Buttons[bi].LastClicked, &absx, &absy, 0, 0) IS ERR::Okay) {
            uiflags |= Self->DragSourceID ? JTYPE::DRAG_ITEM : JTYPE::NIL;

            if ((std::abs(Self->X - Self->LastReleaseX) > Self->ClickSlop) or
                (std::abs(Self->Y - Self->LastReleaseY) > Self->ClickSlop)) {
               uiflags |= JTYPE::DRAGGED;
            }

            if (Self->Buttons[bi].DblClick) {
               if ((uiflags & JTYPE::DRAGGED) IS JTYPE::NIL) uiflags |= JTYPE::DBL_CLICK;
            }

            add_input("ButtonRelease-LastClicked", userinput, uiflags, Self->Buttons[bi].LastClicked, Self->OverObjectID,
               Self->X, Self->Y, Self->X - absx, Self->Y - absy); // OverX/Y is reported relative to the click-held surface
         }
         Self->Buttons[bi].LastClicked = 0;
      }

      Self->LastReleaseX = Self->X;
      Self->LastReleaseY = Self->Y;
   }

   // Check for a modal surface.  The modal_id variable is set if a modal surface is active and the pointer is not
   // positioned over that surface (or its children).  The modal_id is therefore zero if the pointer is over the modal
   // surface, or if no modal surface is defined.

   auto modal_id = gfx::GetModalSurface();
   if (modal_id) {
      if (modal_id IS Self->OverObjectID) {
         // If the pointer is interacting with the modal surface, modality is irrelevant.
         modal_id = 0;
      }
      else { // Check if the OverObject is one of the children of modal_id.
         ERR error = gfx::CheckIfChild(modal_id, Self->OverObjectID);
         if ((error IS ERR::True) or (error IS ERR::LimitedSuccess)) modal_id = 0;
      }
   }

   // Button Press Handler

   if (userinput.Value > 0) {
      log.trace("Button %d depressed @ %" PF64 " Coords: %.2fx%.2f", bi, userinput.Timestamp, Self->X, Self->Y);

      //if ((modal_id) and (modal_id != Self->OverObjectID)) {
      //   log.branch("Surface %d is modal, button click on %d cancelled.", modal_id, Self->OverObjectID);
      //   QueueAction(AC::MoveToFront, modal_id);
      //   QueueAction(AC::Focus, modal_id);
      //}

      //if (!modal_id) {
         // Before performing the click, we first check that there are no objects waiting for click-releases in the
         // designated fields.  If there are, we send them UserClickRelease() actions to retain system integrity.

         if (Self->Buttons[bi].LastClicked) {
            log.warning("Did not receive a release for button %d on surface #%d.", bi, Self->Buttons[bi].LastClicked);

            add_input("ButtonPress-ForceRelease", userinput, uiflags, Self->Buttons[bi].LastClicked, Self->OverObjectID,
               Self->X, Self->Y, Self->OverX, Self->OverY);
         }

         if (((double)(userinput.Timestamp - Self->Buttons[bi].LastClickTime)) / 1000000.0 < Self->DoubleClick) {
            log.trace("Double click detected (under %.2fs)", Self->DoubleClick);
            Self->Buttons[bi].DblClick = TRUE;
            uiflags |= JTYPE::DBL_CLICK;
         }
         else Self->Buttons[bi].DblClick = FALSE;

         Self->Buttons[bi].LastClicked   = Self->OverObjectID;
         Self->Buttons[bi].LastClickTime = userinput.Timestamp;

         Self->LastClickX = Self->X;
         Self->LastClickY = Self->Y;

         // If a modal surface is active for the current process, the button press is reported to the modal surface only.

         target = modal_id ? modal_id : Self->OverObjectID;

         QueueAction(AC::Focus, target);

         add_input("ButtonPress", userinput, uiflags, target, Self->OverObjectID,
            Self->X, Self->Y, Self->OverX, Self->OverY);
      //}

      SubscribeTimer(0.02, C_FUNCTION(repeat_timer), &glRepeatTimer); // Use a timer subscription so that repeat button clicks can be supported (the interval indicates the rate of the repeat)
   }

   if ((Self->DragSourceID) and (!Self->Buttons[bi].LastClicked)) {
      // Drag and drop has been released.  Inform the destination surface of the item's release.

      if (Self->DragSurface) {
         kt::ScopedObjectLock surface(Self->DragSurface);
         if (surface.granted()) acHide(*surface);
         Self->DragSurface = 0;
      }

      if (!modal_id) {
         kt::ScopedObjectLock src(Self->DragSourceID);
         if (src.granted()) {
            kt::ScopedObjectLock surface(Self->OverObjectID);
            if (surface.granted()) acDragDrop(*surface, *src, Self->DragItem, Self->DragData);
         }
      }

      Self->DragItem = 0;
      Self->DragSourceID = 0;
   }
}

//********************************************************************************************************************

static void process_ptr_wheel(extPointer *Self, struct dcDeviceInput *Input)
{
   InputEvent msg;
   msg.Type        = JET::WHEEL;
   msg.Flags       = JTYPE::ANALOG|JTYPE::EXT_MOVEMENT | Input->Flags;
   msg.Mask        = JTYPE::EXT_MOVEMENT;
   msg.Value       = Input->Values[0];
   msg.Timestamp   = Input->Timestamp;
   msg.DeviceID    = Input->DeviceID;
   msg.RecipientID = Self->OverObjectID;
   msg.OverID      = Self->OverObjectID;
   msg.AbsX        = Self->X;
   msg.AbsY        = Self->Y;
   msg.X           = Self->OverX;
   msg.Y           = Self->OverY;

   {
      const std::lock_guard<std::recursive_mutex> lock(glInputLock);
      glInputEvents.push_back(msg);
   }
}

//********************************************************************************************************************

static void process_ptr_movement(extPointer *Self, struct dcDeviceInput *Input)
{
   kt::Log log(__FUNCTION__);
   InputEvent userinput;

   clearmem(&userinput, sizeof(userinput));
   userinput.X         = Input->Values[0];
   userinput.Y         = Input->Values[1];
   userinput.Timestamp = Input->Timestamp;
   userinput.Type      = Input->Type;
   userinput.Flags     = Input->Flags;
   userinput.DeviceID  = Input->DeviceID;

   if (!userinput.Timestamp) userinput.Timestamp = PreciseTime();

   bool moved = false, underlying_change = false;
   double current_x = Self->X;
   double current_y = Self->Y;
   switch (userinput.Type) {
      case JET::ABS_XY:
         current_x = userinput.X;
         if (current_x != Self->X) moved = true;

         current_y = userinput.Y;
         if (current_y != Self->Y) moved = true;
         break;

      default: break;
   }

   if (!moved) {
      // Check if the surface that we're over has changed due to hide, show or movement of surfaces in the display.

      if (get_over_object(Self)) {
         log.trace("Detected change to underlying surface.");
         underlying_change = true;
      }
   }

   if ((moved) or (underlying_change)) {
      // Movement handling.  Pointer coordinates are managed here on the basis that they are 'global', i.e. in a hosted
      // environment the coordinates are relative to the top-left of the host display.  Anchoring is enabled by calling
      // LockCursor().  Typically this support is not available on hosted environments because we can't guarantee that
      // the pointer is locked.

      if (Self->AnchorID) {
         if (CheckResourceExists(Self->AnchorID) != ERR::Okay) {
            Self->AnchorID = 0;
         }
      }

      double xchange = current_x - Self->X;
      double ychange = current_y - Self->Y;

      Self->X = current_x;
      Self->Y = current_y;

      if (Self->AnchorID) {
         // When anchoring is enabled we send a movement message signal to the anchored object.  NOTE: In hosted
         // environments we cannot maintain a true anchor since the pointer is out of our control, but we still must
         // perform the necessary notification.

         add_input("Movement-Anchored", userinput, JTYPE::NIL, Self->AnchorID, Self->AnchorID, current_x, current_y, xchange, ychange);
      }
      else {
         // Report movement event for clients that are interested in the pointer position.  Useful for things like
         // disabling the active screensaver, but not to be used for monitoring movement across surfaces.

         struct acMoveToPoint moveto = { Self->X, Self->Y, 0, MTF::X|MTF::Y };
         NotifySubscribers(Self, AC::MoveToPoint, &moveto, ERR::Okay);

         // Recalculate the OverObject due to cursor movement

         get_over_object(Self);
      }

      if (Self->AnchorID) {
         // Do nothing as only the anchor surface receives a message (see earlier)
      }
      else if (Self->Buttons[0].LastClicked) {
         // This routine is used when the user is holding down the left mouse button (indicated by LastClicked).  The X/Y
         // coordinates are worked out in relation to the clicked object by climbing the Surface object hierarchy.

         if (Self->DragSurface) {
            double sx = Self->X + DRAG_XOFFSET;
            double sy = Self->Y + DRAG_YOFFSET;
            if (Self->DragParent) {
               int absx, absy;
               if (gfx::GetSurfaceCoords(Self->DragParent, nullptr, nullptr, &absx, &absy, nullptr, nullptr) IS ERR::Okay) {
                  sx -= absx;
                  sy -= absy;
               }
            }

            kt::ScopedObjectLock surface(Self->DragSurface);
            if (surface.granted()) acMoveToPoint(*surface, sx, sy, 0, MTF::X|MTF::Y);
         }

         int absx, absy;
         if (get_surface_abs(Self->Buttons[0].LastClicked, &absx, &absy, 0, 0) IS ERR::Okay) {
            auto uiflags = Self->DragSourceID ? JTYPE::DRAG_ITEM : JTYPE::NIL;

            // Send the movement message to the last clicked object

            add_input("Movement-LastClicked", userinput, uiflags, Self->Buttons[0].LastClicked, Self->OverObjectID,
               Self->X, Self->Y, Self->X - absx, Self->Y - absy); // OverX/Y reported relative to the click-held surface

            get_over_object(Self);

            // The surface directly under the pointer also needs notification - important for the view to highlight
            // folders during drag and drop for example.

            // JTYPE::SECONDARY indicates to the receiver of the input message that it is not the primary recipient.

            if (Self->Buttons[0].LastClicked != Self->OverObjectID) {
               add_input("Movement-LastClicked", userinput, uiflags|JTYPE::SECONDARY, Self->OverObjectID, Self->OverObjectID,
                  Self->X, Self->Y, Self->OverX, Self->OverY);
            }

         }
         else {
            log.warning("Failed to get info for surface #%d.", Self->Buttons[0].LastClicked);
            Self->Buttons[0].LastClicked = 0;
         }
      }
      else {
         if (Self->OverObjectID) {
            add_input("OverObject", userinput, JTYPE::NIL, Self->OverObjectID, Self->OverObjectID,
               Self->X, Self->Y, Self->OverX, Self->OverY);
         }

         // If the surface that we're over has changed, send a message to the previous surface to tell it that the
         // pointer has moved for one final time.

         if ((moved) and (Self->LastSurfaceID) and (Self->LastSurfaceID != Self->OverObjectID)) {
            add_input("Movement-PrevSurface", userinput, JTYPE::NIL, Self->LastSurfaceID, Self->OverObjectID,
               Self->X, Self->Y, Self->OverX, Self->OverY);
         }
      }

      Self->LastSurfaceID = Self->OverObjectID; // Reset the LastSurfaceID
   }

   // If a release object has been specified and the cursor is not positioned over it, call the RestoreCursor method.

   if ((userinput.Flags & JTYPE::SECONDARY) != JTYPE::NIL); // No cursor manipulation when it's in a Win32 area
   else if ((Self->CursorReleaseID) and (Self->CursorReleaseID != Self->OverObjectID)) {
      gfx::RestoreCursor(PTC::DEFAULT, 0);
   }
}

//********************************************************************************************************************

static ERR PTR_Free(extPointer *Self)
{
   acHide(Self);

   if (Self->Bitmap) { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }

/*
   OBJECTPTR object;
   if ((Self->SurfaceID) and (!AccessObject(Self->SurfaceID, 5000, &object))) {
      UnsubscribeFeed(object);
      ReleaseObject(object);
   }
*/
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Hide: Hides the pointer cursor.
-END-
*********************************************************************************************************************/

static ERR PTR_Hide(extPointer *Self)
{
   kt::Log log;

   log.branch();

   #ifdef __xwindows__
/*
      APTR xwin;
      OBJECTPTR surface;

      if (AccessObject(Self->SurfaceID, 5000, &surface) IS ERR::Okay) {
         surface->get(FID_WindowHandle, xwin);
         XDefineCursor(XDisplay, (Window)xwin, GetX11Cursor(Self->CursorID));
         ReleaseObject(surface);
      }
*/
   #elif _WIN32
      winShowCursor(0);
   #endif

   Self->Flags &= ~PF::VISIBLE;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR PTR_Init(extPointer *Self)
{
   kt::Log log;

   // Find the Surface object that we are associated with.  Note that it is okay if no surface is available at this
   // stage, but the host system must have a mechanism for setting the Surface field at a later stage or else
   // GetOverObject will not function.

   if (!Self->SurfaceID) {
      Self->SurfaceID = Self->UID;
      while ((Self->SurfaceID) and (GetClassID(Self->SurfaceID) != CLASSID::SURFACE)) {
         Self->SurfaceID = GetOwnerID(Self->SurfaceID);
      }

      if (!Self->SurfaceID) FindObject("SystemSurface", CLASSID::NIL, &Self->SurfaceID);
   }

   // Allocate a custom cursor bitmap

   if ((Self->Bitmap = objBitmap::create::local(
         fl::Name("CustomCursor"),
         fl::Width(MAX_CURSOR_WIDTH),
         fl::Height(MAX_CURSOR_HEIGHT),
         fl::BitsPerPixel(32),
         fl::BytesPerPixel(4),
         fl::Flags(BMF::ALPHA_CHANNEL)))) {
   }
   else log.warning(ERR::NewObject);

   if (Self->MaxSpeed < 1) Self->MaxSpeed = 10;
   if (Self->Speed < 1)    Self->Speed    = 150;

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Move: Moves the pointer by a relative offset.

The Move action adjusts the current #X and #Y coordinates by the supplied delta values.  It applies the movement
immediately by forwarding the resulting position to #MoveToPoint().

-END-

*********************************************************************************************************************/

static ERR PTR_Move(extPointer *Self, struct acMove *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::Args);
   if ((!Args->DeltaX) and (!Args->DeltaY)) return ERR::Okay;
   return acMoveToPoint(Self, Self->X + Args->DeltaX, Self->Y + Args->DeltaY, 0, MTF::X|MTF::Y);
}

/*********************************************************************************************************************

-ACTION-
MoveToPoint: Moves the pointer to an absolute location.

The MoveToPoint action changes the pointer's #X and #Y coordinates immediately.  It updates the host cursor position
where supported, refreshes the object under the hot spot and notifies subscribers to MoveToPoint with the final
coordinates.

This action is intended for programmatic repositioning.  Hardware input is normally delivered through DataFeed and is
translated into input events for the affected surface or object.
-END-

*********************************************************************************************************************/

static ERR PTR_MoveToPoint(extPointer *Self, struct acMoveToPoint *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs)|ERR::Notified;
/*
   if ((!(Args->Flags & MTF::X)) or ((Args->Flags & MTF::X) and (Self->X IS Args->X))) {
      if ((!(Args->Flags & MTF::Y)) or ((Args->Flags & MTF::Y) and (Self->Y IS Args->Y))) {
         return ERR::Okay|ERR::Notified;
      }
   }
*/
#ifdef __xwindows__
   OBJECTPTR surface;

   if (auto error = AccessObject(Self->SurfaceID, 3000, &surface); error IS ERR::Okay) {
      APTR xwin;

      if (surface->get(FID_WindowHandle, xwin) IS ERR::Okay) {
         if ((Args->Flags & MTF::X) != MTF::NIL) Self->X = Args->X;
         if ((Args->Flags & MTF::Y) != MTF::NIL) Self->Y = Args->Y;
         if (Self->X < 0) Self->X = 0;
         if (Self->Y < 0) Self->Y = 0;

         XWarpPointer(XDisplay, None, (Window)xwin, 0, 0, 0, 0, Self->X, Self->Y);
         Self->HostX = Self->X;
         Self->HostY = Self->Y;
      }
      ReleaseObject(surface);
   }
   else return log.warning(error)|ERR::Notified;
#elif _WIN32
   OBJECTPTR surface;

   if (auto error = AccessObject(Self->SurfaceID, 3000, &surface); error IS ERR::Okay) {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->X = Args->X;
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->Y = Args->Y;
      if (Self->X < 0) Self->X = 0;
      if (Self->Y < 0) Self->Y = 0;

      winSetCursorPos(Self->X, Self->Y);
      Self->HostX = Self->X;
      Self->HostY = Self->Y;
      ReleaseObject(surface);
   }
   else return log.warning(error)|ERR::Notified;
#endif

   // Determine the surface object that we are currently positioned over.  If it has set a cursor image, switch to it if the pointer is not locked.

   get_over_object(Self);

   // Customised notification (ensures that both X and Y coordinates are reported).

   struct acMoveToPoint moveto = { Self->X, Self->Y, 0, MTF::X|MTF::Y };
   NotifySubscribers(Self, AC::MoveToPoint, &moveto, ERR::Okay);

   return ERR::Okay|ERR(ERR::Notified);
}

//********************************************************************************************************************

static ERR PTR_NewObject(extPointer *Self)
{
   Self->CursorID = PTC::DEFAULT;
   Self->ClickSlop = 2;
   set_pointer_defaults(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Refresh: Refreshes the pointer's target and cursor image.

This action recalculates the object under the pointer hot spot and reapplies any cursor image selected by the
underlying surface.

-END-
*********************************************************************************************************************/

static ERR PTR_Refresh(extPointer *Self)
{
   // Calling OverObject will refresh the cursor image from the underlying surface object.  Incidentally, the point of
   // all this is to satisfy the Surface class' need to have the pointer refreshed if a surface's cursor ID is changed.

   get_over_object(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Reset: Restores user-adjustable pointer settings to their defaults.

This action resets movement speed, acceleration, double-click interval, maximum speed and wheel speed.  It does not
move the pointer or clear the current cursor ownership state.

-END-
*********************************************************************************************************************/

static ERR PTR_Reset(extPointer *Self)
{
   Self->Speed        = 150;
   Self->Acceleration = 0.50;
   Self->DoubleClick  = 0.30;
   Self->MaxSpeed     = 100;
   Self->WheelSpeed   = DEFAULT_WHEELSPEED;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves pointer preferences to another object.

This action writes the current speed, acceleration, double-click interval, maximum speed, wheel speed and button order
to the destination object in configuration format.

-END-
*********************************************************************************************************************/

static ERR PTR_SaveToObject(extPointer *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Dest)) return log.warning(ERR::NullArgs);

   auto config = objConfig::create { };
   if (config.ok()) {
      config->write("POINTER", "Speed", std::to_string(Self->Speed));
      config->write("POINTER", "Acceleration", std::to_string(Self->Acceleration));
      config->write("POINTER", "DoubleClick", std::to_string(Self->DoubleClick));
      config->write("POINTER", "MaxSpeed", std::to_string(Self->MaxSpeed));
      config->write("POINTER", "WheelSpeed", std::to_string(Self->WheelSpeed));
      config->write("POINTER", "ButtonOrder", Self->ButtonOrder);
      return config->saveToObject(Args->Dest);
   }

   return log.warning(ERR::CreateObject);
}

/*********************************************************************************************************************
-ACTION-
Show: Shows the pointer cursor.
-END-
*********************************************************************************************************************/

static ERR PTR_Show(extPointer *Self)
{
   kt::Log log;

   log.branch();

   #ifdef __xwindows__
/*
      APTR xwin;
      OBJECTPTR surface;

      if (!AccessObject(Self->SurfaceID, 5000, &surface)) {
         surface->get(FID_WindowHandle, xwin);
         XDefineCursor(XDisplay, (Window)xwin, GetX11Cursor(Self->CursorID));
         ReleaseObject(surface);
      }
*/
 #elif _WIN32

      winShowCursor(1);
   #endif

   Self->Flags |= PF::VISIBLE;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Acceleration: The rate of acceleration for relative pointer movement.

This field affects relative pointer movement before the final coordinates are applied.  It is normally treated as a
user preference, because suitable acceleration values depend on the input device and user expectations.

Hosted display drivers may apply their own pointer acceleration before events reach the display module, so this field
is not always relevant in hosted environments.

-FIELD-
Anchor: Can refer to a surface that the pointer has been anchored to.

If the pointer has been anchored to a surface through ~Display.SetCursor(), this field refers to the surface that receives
anchored movement events.

-FIELD-
Bitmap: Refers to bitmap in which custom cursor images can be drawn.

The pointer graphic can be changed to a custom image by drawing into this @Bitmap and selecting `PTC::CUSTOM` with
~Display.SetCustomCursor().

-FIELD-
ButtonOrder: Defines the order in which mouse buttons are interpreted.

This field defines how physical pointer buttons are mapped to logical button positions when pressed.  It can be used
to remap a right-handed device for left-handed use, or to normalise unusual button layouts.

The default button order is `123456789AB`.  The left, right and middle buttons are defined as `1`, `2` and `3`
respectively.  Additional buttons are assigned by the device.

Buttons may be referenced more than once.  For example, `111` maps the first three physical buttons to the logical
left button.

Changes to this field will have an immediate impact on the pointing device's behaviour.

*********************************************************************************************************************/

static ERR GET_ButtonOrder(extPointer *Self, std::string_view &Value)
{
   if (Self->ButtonOrder.empty()) return ERR::FieldNotSet;
   Value = Self->ButtonOrder;
   return ERR::Okay;
}

static ERR SET_ButtonOrder(extPointer *Self, std::string_view &Value)
{
   kt::Log log;

   log.msg("%.*s", int(Value.size()), Value.data());

   // Assign the buttons.
   Self->ButtonOrder.assign(Value);

   // Eliminate any invalid buttons.

   for (size_t i=0; i < Self->ButtonOrder.size(); i++) {
      if (((Self->ButtonOrder[i] >= '1') and (Self->ButtonOrder[i] <= '9')) or
          ((Self->ButtonOrder[i] >= 'A') and (Self->ButtonOrder[i] <= 'Z'))) {
      }
      else Self->ButtonOrder[i] = ' ';
   }

   // Reduce the length of the button list if there are gaps.

   size_t j = 0;
   for (size_t i=0; i < Self->ButtonOrder.size(); i++) {
      if (Self->ButtonOrder[i] != ' ') {
         Self->ButtonOrder[j++] = Self->ButtonOrder[i];
      }
   }

   Self->ButtonOrder.resize(j);

   // Convert the button indexes into their relevant flags.

   int button_index = 0;
   for (size_t i=0; i < std::size(Self->ButtonOrderFlags); i++) {
      char button = (i < Self->ButtonOrder.size()) ? Self->ButtonOrder[i] : 0;

      if ((button >= '1') and (button <= '9')) button_index = button - '1';
      else if ((button >= 'A') and (button <= 'Z')) button_index = button - 'A' + 9;
      else button_index = 0;
      Self->ButtonOrderFlags[i] = 1<<button_index;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ButtonState: Indicates the current button-press state.

This field returns the current pointer button state as bit flags after #ButtonOrder mapping has been applied.  A set
bit indicates that the corresponding logical button is being held down.

The first three bits represent left, right and middle button state respectively, with the left button at bit position
zero.  Additional buttons are supported, but their order depends on the device and the active #ButtonOrder setting.

*********************************************************************************************************************/

static ERR GET_ButtonState(extPointer *Self, int *Value)
{
   int i;
   int state = 0;
   for (i=0; i < std::ssize(Self->Buttons); i++) {
      if (Self->Buttons[i].LastClicked) state |= 1<<i;
   }

   *Value = state;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ClickSlop: A leniency value that assists in determining if the user intended to click or drag.

ClickSlop defines the allowed pointer movement, in pixels, before a press-and-release sequence is treated as a drag
rather than a click.  The same tolerance is used when deciding whether a second click is close enough to qualify as a
double-click.

-FIELD-
CursorID: Identifies the active cursor image.
Lookup: PTC

This field stores the cursor image currently selected for the pointer.  Use ~Display.SetCursor() or ~Display.SetCustomCursor() to
change the cursor after initialisation.

-FIELD-
CursorOwner: The object that currently owns the cursor state.

If the cursor is currently locked by an owner, this field refers to that owner's object ID.  Cursor ownership is
managed by ~Display.SetCursor() and released with ~Display.RestoreCursor().

-FIELD-
DoubleClick: The maximum interval between two clicks for a double click to be recognised.

A double-click is recognised when two presses of the same logical button occur within this interval.  The value is
measured in seconds and defaults to the user's pointer preference.

-FIELD-
DragItem: The currently dragged item, as defined by ~Display.StartCursorDrag().

When a drag-and-drop operation is active, this field contains the custom item number supplied to ~Display.StartCursorDrag().
At all other times it is zero.

-FIELD-
DragSource: The object managing the current drag operation, as defined by ~Display.StartCursorDrag().

When a drag-and-drop operation is active, this field refers to the object managing the source data.  At all other times
it is zero.

Item dragging is managed by ~Display.StartCursorDrag().

-FIELD-
Flags: Optional flags.
Lookup: PF

-FIELD-
Input: Declares the I/O object to read movement from.

This field records an alternate object intended to supply pointer movement.  Input records delivered to the pointer
must use the same `DATA::DEVICE_INPUT` format accepted by #DataFeed().

-FIELD-
MaxSpeed: Restricts the maximum speed of a pointer's movement.

This field limits the maximum relative movement applied to the pointer during a single update.  Values assigned to the
field are clamped to the supported range.

*********************************************************************************************************************/

static ERR SET_MaxSpeed(extPointer *Self, int Value)
{
   if (Value < 2) Self->MaxSpeed = 2;
   else if (Value > 200) Self->MaxSpeed = 200;
   else Self->MaxSpeed = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OverObject: Readable field that gives the ID of the object under the pointer.

This field returns the object directly under the pointer hot spot.  `NULL` can be returned if there is no surface
object under the pointer.

-FIELD-
OverX: The horizontal position of the pointer with respect to the object underneath the hot spot.

This field gives the horizontal position of the pointer hot spot relative to #OverObject.  It can be read when handling
input to determine the object-local coordinate affected by a click, wheel or movement event.

-FIELD-
OverY: The vertical position of the pointer with respect to the object underneath the hot spot.

This field gives the vertical position of the pointer hot spot relative to #OverObject.  It can be read together with
#OverX to determine the object-local coordinate affected by pointer input.

-FIELD-
OverZ: The position of the Pointer within an object.

This field is reserved for interfaces that can report pointer depth.  It reflects the pointer coordinate on the Z axis
relative to the object under the hot spot.

-FIELD-
Restrict: Refers to a surface when the pointer is restricted.

If the pointer has been restricted to a surface through ~Display.SetCursor(), this field refers to that surface.  If the
pointer is not restricted, this field is zero.

-FIELD-
Speed: Speed multiplier for pointer movement.

This field controls the relative movement multiplier, expressed as a percentage.  Values below 100 reduce movement,
while values above 100 increase movement.  #MaxSpeed is applied as a separate upper limit.

-FIELD-
Surface: The top-most surface that is under the pointer's hot spot.

This field refers to the top-most @Surface under the pointer hot spot.  It is automatically updated when the pointer
moves or when surface visibility, position or stacking changes affect the object under the pointer.

-FIELD-
WheelSpeed: Defines a multiplier to be applied to the mouse wheel.

This field defines a multiplier applied to pointer wheel values.  A setting of `1.0` leaves wheel input unchanged,
while `2.0` doubles the reported value.

-FIELD-
X: The horizontal position of the pointer within its display.

*********************************************************************************************************************/

static ERR PTR_SET_X(extPointer *Self, double Value)
{
   if (Self->initialised()) acMoveToPoint(Self, Value, 0, 0, MTF::X);
   else Self->X = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Y: The vertical position of the pointer within its display.

Setting #X or #Y on an initialised pointer forwards the change through #MoveToPoint().
-END-

*********************************************************************************************************************/

static ERR PTR_SET_Y(extPointer *Self, double Value)
{
   if (Self->initialised()) acMoveToPoint(Self, 0, Value, 0, MTF::Y);
   else Self->Y = Value;
   return ERR::Okay;
}

//********************************************************************************************************************

static void set_pointer_defaults(extPointer *Self)
{
   double speed        = glDefaultSpeed;
   double acceleration = glDefaultAcceleration;
   int maxspeed       = 100;
   double wheelspeed   = DEFAULT_WHEELSPEED;
   double doubleclick  = 0.36;
   std::string buttonorder = "123456789ABCDEF";

   if (auto config = objConfig::create { fl::Path("user:config/pointer.cfg") }; config.ok()) {
      config->read("POINTER", "Speed", speed);
      config->read("POINTER", "Acceleration", acceleration);
      config->read("POINTER", "MaxSpeed", maxspeed);
      config->read("POINTER", "WheelSpeed", wheelspeed);
      config->read("POINTER", "DoubleClick", doubleclick);
      config->read("POINTER", "ButtonOrder", buttonorder);
   }

   if (doubleclick < 0.2) doubleclick = 0.2;

   Self->setFields(fl::Speed(speed),
       fl::Acceleration(acceleration),
       fl::MaxSpeed(maxspeed),
       fl::WheelSpeed(wheelspeed),
       fl::DoubleClick(doubleclick),
       fl::ButtonOrder(buttonorder));
}

//********************************************************************************************************************
// Returns true if the underlying object has changed.  The OverObjectID will reflect the current underlying surface.

static bool get_over_object(extPointer *Self)
{
   kt::Log log(__FUNCTION__);

   if ((Self->SurfaceID) and (CheckResourceExists(Self->SurfaceID) != ERR::Okay)) Self->SurfaceID = 0;

   bool changed = false;

   // Find the surface that the pointer resides in (usually SystemSurface @ index 0)

   const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);

   if (glSurfaces.empty()) return false;

   size_t index;
   if (!Self->SurfaceID) {
      Self->SurfaceID = glSurfaces[0].SurfaceID;
      index = 0;
   }
   else for (index=0; (index < glSurfaces.size()) and (glSurfaces[index].SurfaceID != Self->SurfaceID); index++);
   if (index >= glSurfaces.size()) {
      Self->SurfaceID = glSurfaces[0].SurfaceID;
      index = 0;
   }

   auto i = examine_chain(Self, index, glSurfaces, glSurfaces.size());

   OBJECTID li_objectid = glSurfaces[i].SurfaceID;
   double li_left       = glSurfaces[i].Left;
   double li_top        = glSurfaces[i].Top;
   auto cursor_image    = PTC(glSurfaces[i].Cursor); // Preferred cursor ID

   if (Self->OverObjectID != li_objectid) {
      kt::Log log(__FUNCTION__);

      log.traceBranch("OverObject changing from #%d to #%d.", Self->OverObjectID, li_objectid);

      changed = true;

      InputEvent input = {
         .Next        = nullptr,
         .Value       = (double)Self->OverObjectID,
         .Timestamp   = PreciseTime(),
         .RecipientID = Self->OverObjectID, // Recipient is the surface we are leaving
         .OverID      = li_objectid, // New surface (entering)
         .AbsX        = Self->X,
         .AbsY        = Self->Y,
         .X           = Self->X - li_left,
         .Y           = Self->Y - li_top,
         .DeviceID    = Self->UID,
         .Type        = JET::CROSSED_OUT,
         .Flags       = JTYPE::CROSSING,
         .Mask        = JTYPE::CROSSING
      };

      const std::lock_guard<std::recursive_mutex> lock(glInputLock);
      glInputEvents.push_back(input);

      input.Type        = JET::CROSSED_IN;
      input.Value       = li_objectid;
      input.RecipientID = li_objectid; // Recipient is the surface we are entering
      glInputEvents.push_back(input);

      Self->OverObjectID = li_objectid;
   }

   Self->OverX = Self->X - li_left;
   Self->OverY = Self->Y - li_top;

   if (cursor_image != PTC::NIL) {
      if (cursor_image != Self->CursorID) gfx::SetCursor(0, CRF::NIL, cursor_image, "", 0);
   }
   else if ((Self->CursorID != PTC::DEFAULT) and (!Self->CursorOwnerID)) {
      // Restore the pointer to the default image if the cursor isn't locked
      gfx::SetCursor(0, CRF::NIL, PTC::DEFAULT, "", 0);
   }

   return changed;
}

//********************************************************************************************************************

static int examine_chain(extPointer *Self, int Index, SURFACELIST &List, int End)
{
   // NB: Traversal is in reverse to catch the front-most objects first.

   if ((Index < 0) or (Index >= int(List.size()))) return 0;
   if (End > int(List.size())) End = int(List.size());

   auto objectid = List[Index].SurfaceID;
   auto x = Self->X;
   auto y = Self->Y;
   for (auto i=End-1; i >= 0; i--) {
      if ((List[i].ParentID IS objectid) and (List[i].visible())) {
         if ((x >= List[i].Left) and (x < List[i].Right) and (y >= List[i].Top) and (y < List[i].Bottom)) {
            int new_end;
            for (new_end=i+1; (new_end < End) and (List[new_end].Level > List[i].Level); new_end++);
            return examine_chain(Self, i, List, new_end);
         }
      }
   }

   return Index;
}

//********************************************************************************************************************
// This timer is used for handling repeat-clicks.

static ERR repeat_timer(extPointer *Self, int64_t Elapsed, int64_t Unused)
{
   kt::Log log(__FUNCTION__);

   // The subscription is automatically removed if no buttons are held down

   bool unsub = true;
   for (int i=0; i < std::ssize(Self->Buttons); i++) {
      if (Self->Buttons[i].LastClicked) {
         auto time = PreciseTime();
         if (Self->Buttons[i].LastClickTime + 300000LL <= time) {
            InputEvent input;
            clearmem(&input, sizeof(input));

            int surface_x, surface_y;
            if (Self->Buttons[i].LastClicked IS Self->OverObjectID) {
               input.X = Self->OverX;
               input.Y = Self->OverY;
            }
            else if (get_surface_abs(Self->Buttons[i].LastClicked, &surface_x, &surface_y, 0, 0) IS ERR::Okay) {
               input.X = Self->X - surface_x;
               input.Y = Self->Y - surface_y;
            }
            else {
               input.X = Self->OverX;
               input.Y = Self->OverY;
            }

            input.Type        = JET(int(JET::BUTTON_1) + i);
            input.Mask        = JTYPE::BUTTON|JTYPE::REPEATED;
            input.Flags       = JTYPE::BUTTON|JTYPE::REPEATED;
            input.Value       = 1.0; // Self->Buttons[i].LastValue
            input.Timestamp   = time;
            input.DeviceID    = 0;
            input.RecipientID = Self->Buttons[i].LastClicked;
            input.OverID      = Self->OverObjectID;
            input.AbsX        = Self->X;
            input.AbsY        = Self->Y;

            const std::lock_guard<std::recursive_mutex> lock(glInputLock);
            glInputEvents.push_back(input);
         }

         unsub = false;
      }
   }

   if (unsub) return ERR::Terminate;
   else return ERR::Okay;
}

//********************************************************************************************************************

FieldDef CursorLookup[] = {
   { "None",            0 },
   { "Default",         PTC::DEFAULT },             // Values start from 1 and go up
   { "SizeBottomLeft",  PTC::SIZE_BOTTOM_LEFT },
   { "SizeBottomRight", PTC::SIZE_BOTTOM_RIGHT },
   { "SizeTopLeft",     PTC::SIZE_TOP_LEFT },
   { "SizeTopRight",    PTC::SIZE_TOP_RIGHT },
   { "SizeLeft",        PTC::SIZE_LEFT },
   { "SizeRight",       PTC::SIZE_RIGHT },
   { "SizeTop",         PTC::SIZE_TOP },
   { "SizeBottom",      PTC::SIZE_BOTTOM },
   { "Crosshair",       PTC::CROSSHAIR },
   { "Sleep",           PTC::SLEEP },
   { "Sizing",          PTC::SIZING },
   { "SplitVertical",   PTC::SPLIT_VERTICAL },
   { "SplitHorizontal", PTC::SPLIT_HORIZONTAL },
   { "Magnifier",       PTC::MAGNIFIER },
   { "Hand",            PTC::HAND },
   { "HandLeft",        PTC::HAND_LEFT },
   { "HandRight",       PTC::HAND_RIGHT },
   { "Text",            PTC::TEXT },
   { "Paintbrush",      PTC::PAINTBRUSH },
   { "Stop",            PTC::STOP },
   { "Invisible",       PTC::INVISIBLE },
   { "Custom",          PTC::CUSTOM },
   { "Dragable",        PTC::DRAGGABLE },
   { nullptr, 0 }
};

static const ActionArray clPointerActions[] = {
   { AC::DataFeed,     PTR_DataFeed },
   { AC::Free,         PTR_Free },
   { AC::Hide,         PTR_Hide },
   { AC::Init,         PTR_Init },
   { AC::Move,         PTR_Move },
   { AC::MoveToPoint,  PTR_MoveToPoint },
   { AC::NewObject,    PTR_NewObject },
   { AC::Refresh,      PTR_Refresh },
   { AC::Reset,        PTR_Reset },
   { AC::SaveToObject, PTR_SaveToObject },
   { AC::Show,         PTR_Show },
   { AC::NIL, nullptr }
};

static const FieldDef clPointerFlags[] = {
   { "Visible",  PF::VISIBLE },
   { nullptr, 0 }
};

static const FunctionField mthSetCursor[]     = { { "Surface", FD_INT }, { "Flags", FD_INT }, { "Cursor", FD_INT }, { "Name", FD_STRING }, { "Owner", FD_INT }, { "PreviousCursor", FD_INT|FD_RESULT }, { nullptr, 0 } };
static const FunctionField mthRestoreCursor[] = { { "Cursor", FD_INT }, { "Owner", FD_INT }, { nullptr, 0 } };

static const MethodEntry clPointerMethods[] = {
   // Private methods
#ifdef _WIN32
   { MT_PtrSetWinCursor,     (APTR)PTR_SetWinCursor,   "SetWinCursor",   mthSetWinCursor,  sizeof(struct ptrSetWinCursor) },
#endif
#ifdef __xwindows__
   { MT_PtrGrabX11Pointer,   (APTR)PTR_GrabX11Pointer,   "GrabX11Pointer",   mthGrabX11Pointer, sizeof(struct ptrGrabX11Pointer) },
   { MT_PtrUngrabX11Pointer, (APTR)PTR_UngrabX11Pointer, "UngrabX11Pointer", nullptr, 0 },
#endif
   { AC::NIL, nullptr, nullptr, nullptr, 0 }
};

static const FieldArray clPointerFields[] = {
   { "Speed",        FDF_DOUBLE|FDF_RW },
   { "Acceleration", FDF_DOUBLE|FDF_RW },
   { "DoubleClick",  FDF_DOUBLE|FDF_RW },
   { "WheelSpeed",   FDF_DOUBLE|FDF_RW },
   { "X",            FDF_DOUBLE|FDF_RW, nullptr, PTR_SET_X },
   { "Y",            FDF_DOUBLE|FDF_RW, nullptr, PTR_SET_Y },
   { "OverX",        FDF_DOUBLE|FDF_R },
   { "OverY",        FDF_DOUBLE|FDF_R },
   { "OverZ",        FDF_DOUBLE|FDF_R },
   { "MaxSpeed",     FDF_INT|FDF_RW, nullptr, SET_MaxSpeed },
   { "Input",        FDF_OBJECTID|FDF_RW },
   { "Surface",      FDF_OBJECTID|FDF_RW, nullptr, nullptr, CLASSID::SURFACE },
   { "Anchor",       FDF_OBJECTID|FDF_R, nullptr, nullptr, CLASSID::SURFACE },
   { "CursorID",     FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, nullptr, &CursorLookup },
   { "CursorOwner",  FDF_OBJECTID|FDF_RW },
   { "Flags",        FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clPointerFlags },
   { "Restrict",     FDF_OBJECTID|FDF_R, nullptr, nullptr, CLASSID::SURFACE },
   { "HostX",        FDF_INT|FDF_R|FDF_SYSTEM },
   { "HostY",        FDF_INT|FDF_R|FDF_SYSTEM },
   { "Bitmap",       FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "DragSource",   FDF_OBJECTID|FDF_R },
   { "DragItem",     FDF_INT|FDF_R },
   { "OverObject",   FDF_OBJECTID|FDF_R },
   { "ClickSlop",    FDF_INT|FDF_RW },
   // Virtual Fields
   { "ButtonState",  FDF_INT|FDF_R|FDF_PURE, GET_ButtonState },
   { "ButtonOrder",  FDF_CPPSTRING|FDF_RW|FDF_PURE, GET_ButtonOrder, SET_ButtonOrder },
   END_FIELD
};

//********************************************************************************************************************

ERR create_pointer_class(void)
{
   clPointer = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::POINTER),
      fl::ClassVersion(VER_POINTER),
      fl::Name("Pointer"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clPointerActions),
      fl::Methods(clPointerMethods),
      fl::Fields(clPointerFields),
      fl::Size(sizeof(extPointer)),
      fl::Path(MOD_PATH));

   return clPointer ? ERR::Okay : ERR::AddClass;
}
