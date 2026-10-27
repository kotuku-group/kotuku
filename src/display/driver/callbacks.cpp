/*********************************************************************************************************************

Notes
-----
* Use TrackMouseEvent() to receive notification of the mouse leaving a window.
* GetWindowThreadProcessId() can tell you the ID of the thread that created a window.  Also, GetWindowLongPtr() can
  give information on the HINSTANCE, HWNDPARENT, window ID.
* The win32 FindWindow() and FindWindowEx() functions can be used to retrieve foreign window handles.
* The IsWindow() function can be used to determine if a window handle is still valid.

*********************************************************************************************************************/

namespace display {

constexpr int CALLBACK_AXIS_VERTICAL = 1;
constexpr int CALLBACK_AXIS_HORIZONTAL = 2;
constexpr int CALLBACK_AXIS_BOTH = 3;

void DriverKeyPress(KQ Flags, KEY Value, int Printable)
{
   if (Value IS KEY::NIL) return;

   // Cancel before queuing the key event: a following pointer release can deliver a drop synchronously.

   if (Value IS KEY::ESCAPE) {
      if (auto pointer = gfx::AccessPointer()) {
         if (pointer->DragSourceID) acDeactivate(pointer);
         ReleaseObject(pointer);
      }
   }

   if ((Printable < 0x20) or (Printable IS 127)) Flags |= KQ::NOT_PRINTABLE;

   evKey key = {
      .EventID    = EVID_IO_KEYBOARD_KEYPRESS,
      .Qualifiers = Flags|KQ::PRESSED,
      .Code       = Value,
      .Unicode    = Printable
   };
   BroadcastEvent(&key, sizeof(key));
}

//********************************************************************************************************************

void DriverKeyRelease(KQ Flags, KEY Value)
{
   if (Value IS KEY::NIL) return;

   evKey key = {
      .EventID    = EVID_IO_KEYBOARD_KEYPRESS,
      .Qualifiers = Flags|KQ::RELEASED,
      .Code       = Value,
      .Unicode    = 0
   };
   BroadcastEvent(&key, sizeof(key));
}

//********************************************************************************************************************

void DriverMovement(OBJECTID SurfaceID, double AbsX, double AbsY, bool NonClient)
{
   if (auto pointer = gfx::AccessPointer(); pointer) {
      pointer->setSurface(SurfaceID);  // Alter the surface of the pointer so that it refers to the correct root window

      // Record the host cursor position, which the Pointer class reports through its HostX and HostY fields.

      pointer->HostX = AbsX;
      pointer->HostY = AbsY;

      struct dcDeviceInput joy = {
         .Values = { AbsX, AbsY },
         .Timestamp = PreciseTime(),
         .Flags = NonClient ? JTYPE::SECONDARY : JTYPE::NIL,
         .Type  = JET::ABS_XY
      };
      acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT,
         std::span<const int8_t>((const int8_t *)&joy, sizeof(joy)));
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void DriverRelativeMovement(OBJECTID SurfaceID, double DX, double DY)
{
   if (auto pointer = gfx::AccessPointer(); pointer) {
      if (pointer->AnchorID IS SurfaceID) {
         InputEvent input = {};
         input.Type = JET::ABS_XY;
         input.Flags = JTYPE::MOVEMENT;
         input.Mask = JTYPE::MOVEMENT;
         input.Timestamp = PreciseTime();
         input.RecipientID = SurfaceID;
         input.OverID = SurfaceID;
         input.AbsX = pointer->X;
         input.AbsY = pointer->Y;
         input.X = DX;
         input.Y = DY;
         const std::lock_guard<std::recursive_mutex> lock(glInputLock);
         glInputEvents.push_back(input);
      }
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void DriverPointerLockLost(OBJECTID SurfaceID)
{
   if (auto pointer = (extPointer *)gfx::AccessPointer(); pointer) {
      if (pointer->AnchorID IS SurfaceID) pointer->AnchorID = 0;
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void DriverTouchInput(OBJECTID SurfaceID, uint32_t SeatID, int32_t ContactID, JET Type, double X, double Y)
{
   if (not SurfaceID) return;
   InputEvent input = {};
   input.Type = Type;
   input.Flags = JTYPE::TOUCH;
   input.Mask = JTYPE::TOUCH;
   input.Timestamp = PreciseTime();
   input.RecipientID = SurfaceID;
   input.OverID = SurfaceID;
   input.DeviceID = OBJECTID(SeatID);
   input.Value = ContactID;
   input.AbsX = input.X = X;
   input.AbsY = input.Y = Y;
   const std::lock_guard<std::recursive_mutex> lock(glInputLock);
   glInputEvents.push_back(input);
}

//********************************************************************************************************************

// Wheel movement is reported as JET::WHEEL for the vertical axis and JET::WHEEL_X for the horizontal axis.  Holding
// Shift redirects purely vertical rotation to the horizontal axis, as is the convention on desktop platforms.  The
// redirected event is flagged with JTYPE::ALT so that clients can recognise it, e.g. to apply the rotation to a
// one-dimensional control such as a slider.

void DriverWheelMovement(OBJECTID SurfaceID, float DeltaX, float DeltaY, KQ Qualifiers)
{
   if (!glPointerID) {
      if (FindObject("SystemPointer", CLASSID::NIL, &glPointerID) != ERR::Okay) return;
   }

   auto x_flags = JTYPE::NIL;
   if (((Qualifiers & KQ::SHIFT) != KQ::NIL) and (DeltaX IS 0) and (DeltaY != 0)) {
      DeltaX  = DeltaY;
      DeltaY  = 0;
      x_flags = JTYPE::ALT;
   }

   std::array<dcDeviceInput, 2> input;
   size_t total = 0;
   auto timestamp = PreciseTime();

   if (DeltaY != 0) {
      input[total++] = { .Values = { DeltaY, 0 }, .Timestamp = timestamp, .Flags = JTYPE::NIL, .Type = JET::WHEEL };
   }

   if (DeltaX != 0) {
      input[total++] = { .Values = { DeltaX, 0 }, .Timestamp = timestamp, .Flags = x_flags, .Type = JET::WHEEL_X };
   }

   if (not total) return;

   if (auto pointer = gfx::AccessPointer(); pointer) {
      acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT,
         std::span<const int8_t>((const int8_t *)input.data(), sizeof(dcDeviceInput) * total));
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************
// The host has already applied the focus change that it is reporting.  While it is processed, Focus and MoveToFront
// requests for the same display are not sent back to the host, as doing so would generate new focus events.  Two
// windows that receive the focus in quick succession would otherwise take it back from each other indefinitely.

void DriverFocusState(OBJECTID SurfaceID, bool State)
{
   //log.msg("Host focus state for surface #%d: %d", SurfaceID, State);

   OBJECTID display_id = 0;
   if (kt::ScopedObjectLock<extSurface> surface(SurfaceID); surface.granted()) display_id = surface->DisplayID;

   const auto previous_display = glHostFocusDisplay;
   glHostFocusDisplay = display_id;
   auto restore = kt::Defer([previous_display]() { glHostFocusDisplay = previous_display; });

   if (State) {
      kt::ScopedObjectLock surface(SurfaceID);
      if (surface.granted()) acFocus(*surface);
      return;
   }

   std::vector<OBJECTID> list;
   {
      const std::lock_guard<std::recursive_mutex> lock(glFocusLock);
      list = glFocusList;
   }

   // glFocusList is ordered with the most deeply focused surface first, followed by each of its ancestors.  The
   // surface named by a host window therefore sits at the end of the chain, and every entry preceding it is a
   // descendant that must also lose the focus.

   auto pos = std::find(list.begin(), list.end(), SurfaceID);

   if (pos IS list.end()) { // Not part of the focus chain, so the window surface is the only one affected
      kt::ScopedObjectLock surface(SurfaceID);
      if (surface.granted()) acLostFocus(*surface);
      return;
   }

   for (auto it = list.begin(); it != std::next(pos); it++) {
      kt::ScopedObjectLock surface(*it);
      if (surface.granted()) acLostFocus(*surface);
   }
}

//********************************************************************************************************************
// If a button press is incoming from the non-client area (e.g. titlebar, resize edge) then the SECONDARY flag is
// applied.

void DriverButtonInput(int Buttons, bool State)
{
   if (auto pointer = gfx::AccessPointer()) {
      struct dcDeviceInput joy[5];

      int i = 0;
      int64_t timestamp = PreciseTime();

      if (Buttons & 0x0001) {
         joy[i].Type  = JET::BUTTON_1;
         joy[i].Flags = (Buttons & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (Buttons & 0x0002) {
         joy[i].Type  = JET::BUTTON_2;
         joy[i].Flags = (Buttons & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (Buttons & 0x0004) {
         joy[i].Type  = JET::BUTTON_3;
         joy[i].Flags = (Buttons & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (Buttons & 0x0008) {
         joy[i].Type  = JET::BUTTON_4;
         joy[i].Flags = (Buttons & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (Buttons & 0x0010) {
         joy[i].Type  = JET::BUTTON_5;
         joy[i].Flags = (Buttons & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (i) acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT,
         std::span<const int8_t>((const int8_t *)&joy, sizeof(struct dcDeviceInput) * i));

      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void DriverCrossing(OBJECTID SurfaceID, bool Entered, double AbsX, double AbsY)
{
   if (not SurfaceID) return;

   if (auto pointer = gfx::AccessPointer(); pointer) {
      if (Entered) pointer->setSurface(SurfaceID);
      else if (pointer->SurfaceID IS SurfaceID) pointer->setSurface(0);

      pointer->HostX = AbsX;
      pointer->HostY = AbsY;
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void DriverWindowResized(OBJECTID SurfaceID, int WinX, int WinY, int WinWidth, int WinHeight,
   int ClientX, int ClientY, int ClientWidth, int ClientHeight)
{
   kt::Log log("ResizedWindow");
   //log.branch("#%d, Window: %dx%d,%dx%d, Client: %dx%d,%dx%d", SurfaceID, WinX, WinY, WinWidth, WinHeight,
   //   ClientX, ClientY, ClientWidth, ClientHeight);

   if ((!SurfaceID) or (WinWidth < 1) or (WinHeight < 1)) return;

   FUNCTION feedback;
   OBJECTID display_id = 0;
   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      display_id = surface->DisplayID;
      if (ScopedObjectLock<extDisplay> display(display_id, 3000); display.granted()) {
         release_stale_resize_feedback(*display);
         display->X = WinX;
         display->Y = WinY;
         display->Width  = WinWidth;
         display->Height = WinHeight;
         acResize(display->Bitmap, ClientWidth, ClientHeight, 0);
         if (display->ResizeFeedback.defined()) feedback = display->ResizeFeedback;
      }
      else return;
   }
   else return;

   // Notification occurs with the display and surface released so as to reduce the potential for dead-locking.

   if (feedback.defined()) resize_feedback(&feedback, display_id, ClientX, ClientY, ClientWidth, ClientHeight);
}

//********************************************************************************************************************
// We're interested in this message only when Windows soft-sets one of our windows.  A 'soft-set' means that our Window
// has received the focus without direct user interaction (typically a window on the desktop has closed and our
// window is inheriting the focus).
//
// Being able to tell the difference between a soft-set and a hard-set is difficult, but checking for visibility seems
// to be enough in preventing confusion.

void DriverSetFocus(OBJECTID SurfaceID)
{
   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      kt::Log log;
      if ((!surface->hasFocus()) and (surface->visible())) {
         log.msg("WM_SETFOCUS: Sending focus to surface #%d.", SurfaceID);
         QueueAction(AC::Focus, SurfaceID);
      }
      else log.trace("WM_SETFOCUS: Surface #%d already has the focus, or is hidden.", SurfaceID);
   }
}

//********************************************************************************************************************

void DriverDPIChanged(OBJECTID SurfaceID)
{
   if (not SurfaceID) return;

   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      auto display_id = surface->DisplayID;
      if (not display_id) return;

      if (ScopedObjectLock<extDisplay> display(display_id, 3000); display.granted()) {
         display->invalidateDensity();

         // ScaleToDPI() reads the system display cache rather than the display's density fields.

         if (display_id IS glDisplayInfo.DisplayID) {
            int horizontal = 0, vertical = 0;
            display->getHDensity(horizontal);
            display->getVDensity(vertical);
            glDisplayInfo.HDensity = horizontal;
            glDisplayInfo.VDensity = vertical;
         }
      }
   }
}

//********************************************************************************************************************
// Called from WM_SIZE and WM_SIZING events to confirm that the requested window size is within the limits set by the
// surface object.

void DriverConstrainWindowSize(OBJECTID SurfaceID, int &Width, int &Height, int CurrentWidth, int CurrentHeight,
   int Axis)
{
   if (!SurfaceID) return;
   if ((Width IS CurrentWidth) and (Height IS CurrentHeight)) return;

   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      int min_width, min_height, max_width, max_height;
      surface->getMinWidth(min_width);
      surface->getMinHeight(min_height);
      surface->getMaxWidth(max_width);
      surface->getMaxHeight(max_height);

      if ((min_width > 0) and (Width < min_width))    Width  = min_width;
      if ((min_height > 0) and (Height < min_height)) Height = min_height;
      if ((max_width > 0) and (Width > max_width))    Width  = max_width;
      if ((max_height > 0) and (Height > max_height)) Height = max_height;

      if ((surface->Flags & RNF::ASPECT_RATIO) != RNF::NIL) {
         if (Axis IS CALLBACK_AXIS_BOTH) {
            if (min_width > min_height) {
               auto scale = (double)min_height / (double)min_width;
               Height = int(Width * scale);
            }
            else {
               auto scale = (double)min_width / (double)min_height;
               Width = int(Height * scale);
            }
         }
         else if (Axis IS CALLBACK_AXIS_HORIZONTAL) {
            auto scale = (double)min_height / (double)min_width;
            Height = int(Width * scale);
         }
         else if (Axis IS CALLBACK_AXIS_VERTICAL) {
            auto scale = (double)min_width / (double)min_height;
            Width = int(Height * scale);
         }
      }
   }
}

//********************************************************************************************************************

void DriverExposeRegion(OBJECTID SurfaceID, int X, int Y, int Width, int Height)
{
   kt::ScopedObjectLock<objSurface> surface(SurfaceID);

   if (surface.granted()) {
      if ((Width) and (Height)) surface->exposeToDisplay(X, Y, Width, Height, EXF::CHILDREN);
      else surface->exposeToDisplay(0, 0, 0x7fff, 0x7fff, EXF::CHILDREN);
   }
}

//********************************************************************************************************************

void DriverProcessMessages()
{
   ProcessMessages(PMF::NIL, 0);
}

//********************************************************************************************************************

ERR DriverWindowClose(OBJECTID SurfaceID)
{
   kt::Log log(__FUNCTION__);

   if (SurfaceID) {
      const WinHook hook(SurfaceID, WH::CLOSE);
      FUNCTION func;
      bool has_hook = false;

      {
         const std::lock_guard<std::recursive_mutex> lock(glWindowHookLock);
         if (auto it = glWindowHooks.find(hook); it != glWindowHooks.end()) {
            if (it->second.stale()) {
               release_display_callback(it->second);
               glWindowHooks.erase(it);
            }
            else {
               func = it->second;
               func.pin();
               has_hook = true;
            }
         }
      }

      if (has_hook) {
         ERR result;

         if (func.isC()) {
            kt::SwitchContext ctx(func.Context);
            auto callback = (ERR (*)(OBJECTID SurfaceID, APTR))func.Routine;
            result = callback(SurfaceID, func.Meta);
         }
         else if (func.isScript()) {
            sc::Call(func, std::to_array<ScriptArg>({ { "SurfaceID", SurfaceID, FDF_OBJECTID } }), result);
         }
         else result = ERR::Okay;

         if (result IS ERR::Terminate) {
            const std::lock_guard<std::recursive_mutex> lock(glWindowHookLock);
            if (auto it = glWindowHooks.find(hook); it != glWindowHooks.end()) {
               release_display_callback(it->second);
               glWindowHooks.erase(it);
            }
         }
         else if (result IS ERR::Cancelled) {
            log.msg("Window closure cancelled by client.");
            if (func.defined()) func.unpin();
            return ERR::Cancelled;
         }

         if (func.defined()) func.unpin();
      }

      if (!CheckResourceExists(SurfaceID)) FreeObject(SurfaceID);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

void DriverWindowDestroyed(OBJECTID SurfaceID)
{
   if (SurfaceID) {
      kt::Log log("WinMgr");
      log.branch("Freeing window surface #%d.", SurfaceID);
      FreeObject(SurfaceID);
   }
}

//********************************************************************************************************************

void DriverWindowHidden(OBJECTID SurfaceID)
{
   if (kt::ScopedObjectLock<extSurface> surface(SurfaceID, 2000); surface.granted()) acHide(*surface);
}

//********************************************************************************************************************

void MsgShowObject(OBJECTID ObjectID)
{
   kt::ScopedObjectLock obj(ObjectID);
   if (obj.granted()) {
      acShow(*obj);
      acMoveToFront(*obj);
   }
}

//********************************************************************************************************************

void DriverClipboardUpdated()
{
   #ifdef _WIN32
      if (winClipboardChanged()) win_clipboard_updated();
   #endif
}

void DriverClipboardData(CSTRING Mime, CSTRING Data, size_t Length, bool Dropped, OBJECTID SurfaceID)
{
   receive_host_clipboard(Mime, std::string_view(Data, Length), Dropped, SurfaceID);
}

void DriverClipboardLost()
{
   lose_host_clipboard();
}

// Native offers use the same feedback path as internal drags.  The host retains ownership of button release.

void DriverDragLeave()
{
   if (auto pointer = (extPointer *)gfx::AccessPointer()) {
      if (pointer->NativeDrag) end_cursor_drag(pointer, false);
      ReleaseObject(pointer);
      input_event_loop(HOSTHANDLE{}, nullptr);
   }
}

bool DriverDragOver(OBJECTID SurfaceID, double X, double Y, CSTRING Datatypes, size_t Length)
{
   auto pointer = (extPointer *)gfx::AccessPointer();
   if (not pointer) return false;
   auto release = kt::Defer([&]() { ReleaseObject(pointer); });
   if ((pointer->DragSourceID) and (not pointer->NativeDrag)) return false;

   const auto modal = gfx::GetModalSurface();
   SURFACEINFO *info;
   if ((not SurfaceID) or (not Length) or ((modal) and (not in_modal_scope(modal, SurfaceID))) or
         (gfx::GetSurfaceInfo(SurfaceID, &info) != ERR::Okay)) {
      DriverDragLeave();
      return false;
   }
   int abs_x, abs_y;
   if (gfx::GetSurfaceCoords(SurfaceID, nullptr, nullptr, &abs_x, &abs_y, nullptr, nullptr) != ERR::Okay)
      return false;

   pointer->NativeDrag = true;
   pointer->DragSourceID = info->DisplayID;
   pointer->DragItem = -1;
   pointer->DragFeedback = -1;
   pointer->DragData.fill(0);
   pointer->DragDataSize = uint8_t(std::min(Length, pointer->DragData.size()));
   copymem(Datatypes, pointer->DragData.data(), pointer->DragDataSize);
   watch_drag_keys(pointer);
   DriverMovement(SurfaceID, X + abs_x, Y + abs_y, false);

   InputEvent event = {};
   event.Type = JET::ABS_XY;
   event.Flags = event.Mask = JTYPE::DRAG_ITEM;
   event.RecipientID = event.OverID = pointer->OverObjectID;
   event.X = pointer->OverX;
   event.Y = pointer->OverY;
   {
      const std::lock_guard lock(glInputLock);
      glInputEvents.push_back(event);
   }
   input_event_loop(HOSTHANDLE{}, nullptr);
   return pointer->NativeDrag and (pointer->DragFeedback != 0);
}

bool DriverDragStart(NativeDrop &Drop)
{
   auto pointer = (extPointer *)gfx::AccessPointer();
   if (not pointer) return false;
   auto release = kt::Defer([&]() { ReleaseObject(pointer); });
   if (not pointer->NativeDrag) return false;
   const auto datatypes = pointer->DragData;
   const auto size = pointer->DragDataSize;
   if (not DriverDragOver(Drop.SurfaceID, Drop.X, Drop.Y, datatypes.data(), size)) return false;
   const auto modal = gfx::GetModalSurface();
   if ((modal) and (not in_modal_scope(modal, pointer->OverObjectID))) return false;
   if (not in_modal_scope(Drop.SurfaceID, pointer->OverObjectID)) return false;
   Drop.TargetID = pointer->OverObjectID;
   Drop.DisplayID = pointer->DragSourceID;
   Drop.X = pointer->OverX;
   Drop.Y = pointer->OverY;
   return true;
}

// Read the imported payload only during synchronous delivery.  Restore the live pointer afterwards: it may already
// be over another window, or carrying another drag, when the asynchronous pipe finishes.

bool DriverDragData(const NativeDrop &Drop, CSTRING Mime, CSTRING Data, size_t Length)
{
   const auto modal = gfx::GetModalSurface();
   if ((modal) and (not in_modal_scope(modal, Drop.TargetID))) return false;
   if (not in_modal_scope(Drop.SurfaceID, Drop.TargetID)) return false;
   kt::ScopedObjectLock destination(Drop.TargetID);
   kt::ScopedObjectLock source(Drop.DisplayID);
   if ((not destination.granted()) or (not source.granted())) return false;
   auto pointer = (extPointer *)gfx::AccessPointer();
   if (not pointer) return false;
   auto release = kt::Defer([&]() { ReleaseObject(pointer); });
   if (pointer->DragDelivering) return false;
   const std::lock_guard clipboard_lock(glClipboardLock);

   const auto over_id = pointer->OverObjectID;
   const auto over_x = pointer->OverX;
   const auto over_y = pointer->OverY;
   const auto feedback = pointer->DragFeedback;
   const auto live_surface = pointer->SurfaceID;
   const auto live_x = pointer->X;
   const auto live_y = pointer->Y;
   auto restore = kt::Defer([&]() {
      if ((pointer->SurfaceID IS live_surface) and (pointer->X IS live_x) and (pointer->Y IS live_y)) {
         pointer->OverObjectID = over_id;
         pointer->OverX = over_x;
         pointer->OverY = over_y;
         pointer->DragFeedback = feedback;
      }
      pointer->DragDelivering = false;
      clear_host_drop();
   });
   pointer->OverObjectID = Drop.TargetID;
   pointer->OverX = Drop.X;
   pointer->OverY = Drop.Y;
   pointer->DragFeedback = -1;
   pointer->DragDelivering = true;
   if (not receive_host_clipboard(Mime, std::string_view(Data, Length), true, 0)) return false;
   const char datatype = std::string_view(Mime) IS "text/uri-list" ? char(DATA::FILE) : char(DATA::TEXT);
   acDragDrop(*destination, *source, -1, std::string_view(&datatype, 1));
   return pointer->DragFeedback != 0;
}

//********************************************************************************************************************

ERR DriverEnableDragDrop(APTR HostHandle)
{
   #ifdef _WIN32
      return ERR(winInitDragDrop(HWND(HostHandle)));
   #else
      return ERR::NoSupport;
   #endif
}

//********************************************************************************************************************

void DriverDisableDragDrop(APTR HostHandle)
{
   #ifdef _WIN32
      winDisableDragDrop(HWND(HostHandle));
   #endif
}

//********************************************************************************************************************

#ifdef _WIN32
extern "C" int winResolveSurfaceID(HWND Window)
{
   return int(DriverResolveSurface(Window));
}
#endif

//********************************************************************************************************************

void DriverDragDropped(OBJECTID SurfaceID, CSTRING Datatypes)
{
   #ifdef _WIN32
      if (auto pointer = gfx::AccessPointer()) {
         winDragDropFromHost_Drop(SurfaceID, pointer->HostX, pointer->HostY, Datatypes);
         ReleaseObject(pointer);
      }
   #else
      if (auto pointer = gfx::AccessPointer()) {
         auto target_id = pointer->OverObjectID ? pointer->OverObjectID : SurfaceID;
         kt::ScopedObjectLock target(target_id);
         kt::ScopedObjectLock surface(SurfaceID);
         if (target.granted() and surface.granted()) {
            SURFACEINFO *info;
            if (gfx::GetSurfaceInfo(SurfaceID, &info) IS ERR::Okay) {
               kt::ScopedObjectLock display(info->DisplayID);
               if (display.granted()) acDragDrop(*target, *display, -1, Datatypes);
            }
         }
         ReleaseObject(pointer);
      }
   #endif
}

//********************************************************************************************************************

void DriverControllerPorts(int Port, bool Connected, int Total)
{
   if (Total >= 0) glLastPort = Total - 1;
   else if ((Connected) and (Port > glLastPort)) glLastPort = Port;
   else if ((not Connected) and (Port IS glLastPort)) glLastPort = -1;
}

//********************************************************************************************************************
// Reports whether a visible surface can receive the focus.  Drivers check this before focussing a host window that the
// window manager does not manage, such as a pop-up menu that has been clicked.

bool DriverAcceptsFocus(OBJECTID SurfaceID)
{
   RNF flags;
   if (gfx::GetSurfaceFlags(SurfaceID, &flags) != ERR::Okay) return false;
   if ((flags & (RNF::NO_FOCUS|RNF::DISABLED)) != RNF::NIL) return false;
   return (flags & RNF::VISIBLE) != RNF::NIL;
}

//********************************************************************************************************************

OBJECTID DriverResolveSurface(APTR HostHandle)
{
   if (!HostHandle) return 0;

   std::vector<OBJECTID> surface_ids;
   {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
      surface_ids.reserve(glSurfaces.size());
      for (auto &record : glSurfaces) surface_ids.push_back(record.SurfaceID);
   }

   for (auto surface_id : surface_ids) {
      if (ScopedObjectLock<extSurface> surface(surface_id, 3000); surface.granted()) {
         if (surface->DisplayWindow IS HostHandle) return surface_id;
      }
   }

   return 0;
}

} // namespace
