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

void MsgKeyPress(KQ Flags, KEY Value, int Printable)
{
   if (Value IS KEY::NIL) return;

   if ((Printable < 0x20) or (Printable IS 127)) Flags |= KQ::NOT_PRINTABLE;

   evKey key = {
      .EventID    = EVID_IO_KEYBOARD_KEYPRESS,
      .Qualifiers = Flags|KQ::PRESSED,
      .Code       = Value,
      .Unicode    = Printable
   };
   BroadcastEvent(&key, sizeof(key));
}

void MsgKeyPress(int Flags, int Value, int Printable) { MsgKeyPress(KQ(Flags), KEY(Value), Printable); }

//********************************************************************************************************************

void MsgKeyRelease(KQ Flags, KEY Value)
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

void MsgKeyRelease(int Flags, int Value) { MsgKeyRelease(KQ(Flags), KEY(Value)); }

//********************************************************************************************************************

void MsgMovement(OBJECTID SurfaceID, double AbsX, double AbsY, int WinX, int WinY, bool NonClient)
{
   if (auto pointer = gfx::AccessPointer(); pointer) {
      pointer->setSurface(SurfaceID);  // Alter the surface of the pointer so that it refers to the correct root window

      struct dcDeviceInput joy = {
         .Values = { AbsX, AbsY },
         .Timestamp = PreciseTime(),
         .Flags = NonClient ? JTYPE::SECONDARY : JTYPE::NIL,
         .Type  = JET::ABS_XY
      };
      acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &joy, sizeof(joy));
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void MsgWheelMovement(OBJECTID SurfaceID, float Wheel)
{
   if (!glPointerID) {
      if (FindObject("SystemPointer", CLASSID::NIL, &glPointerID) != ERR::Okay) return;
   }

   if (auto pointer = gfx::AccessPointer(); pointer) {
      struct dcDeviceInput joy = {
         .Values    = { Wheel, 0 },
         .Timestamp = PreciseTime(),
         .Flags     = JTYPE::NIL,
         .Type      = JET::WHEEL
      };

      acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &joy, sizeof(joy));
      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void MsgFocusState(OBJECTID SurfaceID, int State)
{
   //log.msg("Windows focus state for surface #%d: %d", SurfaceID, State);

   kt::ScopedObjectLock surface(SurfaceID);
   if (surface.granted()) {
      if (State) acFocus(*surface);
      else {
         acLostFocus(*surface);
         // for (auto &id : glFocusList) acLostFocus(id);
      }
   }
}

//********************************************************************************************************************
// If a button press is incoming from the non-client area (e.g. titlebar, resize edge) then the SECONDARY flag is
// applied.

void MsgButtonPress(int button, int State)
{
   if (auto pointer = gfx::AccessPointer()) {
      struct dcDeviceInput joy[5];

      int i = 0;
      int64_t timestamp = PreciseTime();

      if (button & 0x0001) {
         joy[i].Type  = JET::BUTTON_1;
         joy[i].Flags = (button & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (button & 0x0002) {
         joy[i].Type  = JET::BUTTON_2;
         joy[i].Flags = (button & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (button & 0x0004) {
         joy[i].Type  = JET::BUTTON_3;
         joy[i].Flags = (button & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (button & 0x0008) {
         joy[i].Type  = JET::BUTTON_4;
         joy[i].Flags = (button & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (button & 0x0010) {
         joy[i].Type  = JET::BUTTON_5;
         joy[i].Flags = (button & 0x4000) ? JTYPE::SECONDARY : JTYPE::NIL;
         joy[i].Values[0] = State;
         joy[i].Timestamp = timestamp;
         i++;
      }

      if (i) acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &joy, sizeof(struct dcDeviceInput) * i);

      ReleaseObject(pointer);
   }
}

//********************************************************************************************************************

void MsgResizedWindow(OBJECTID SurfaceID, int WinX, int WinY, int WinWidth, int WinHeight,
   int ClientX, int ClientY, int ClientWidth, int ClientHeight)
{
   kt::Log log("ResizedWindow");
   //log.branch("#%d, Window: %dx%d,%dx%d, Client: %dx%d,%dx%d", SurfaceID, WinX, WinY, WinWidth, WinHeight, ClientX, ClientY, ClientWidth, ClientHeight);

   if ((!SurfaceID) or (WinWidth < 1) or (WinHeight < 1)) return;

   FUNCTION feedback;
   OBJECTID display_id = 0;
   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      display_id = surface->DisplayID;
      if (ScopedObjectLock<extDisplay> display(display_id, 3000); display.granted()) {
         if (!display->ResizeFeedback.defined()) return;
         feedback = display->ResizeFeedback;
         display->X = WinX;
         display->Y = WinY;
         display->Width  = WinWidth;
         display->Height = WinHeight;
      }
      else return;
   }
   else return;

   // Notification occurs with the display and surface released so as to reduce the potential for dead-locking.

   resize_feedback(&feedback, display_id, ClientX, ClientY, ClientWidth, ClientHeight);
}

//********************************************************************************************************************
// We're interested in this message only when Windows soft-sets one of our windows.  A 'soft-set' means that our Window
// has received the focus without direct user interaction (typically a window on the desktop has closed and our
// window is inheriting the focus).
//
// Being able to tell the difference between a soft-set and a hard-set is difficult, but checking for visibility seems
// to be enough in preventing confusion.

void MsgSetFocus(OBJECTID SurfaceID)
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

void MsgDPIChanged(OBJECTID SurfaceID)
{
   if (not SurfaceID) return;

   if (ScopedObjectLock<objSurface> surface(SurfaceID, 3000); surface.granted()) {
      auto display_id = surface->DisplayID;
      if (not display_id) return;

      if (ScopedObjectLock<objDisplay> display(display_id, 3000); display.granted()) {
         display->setHDensity(0);
         display->setVDensity(0);
      }
   }
}

//********************************************************************************************************************
// Called from WM_SIZE and WM_SIZING events to confirm that the requested window size is within the limits set by the
// surface object.

void CheckWindowSize(OBJECTID SurfaceID, int &Width, int &Height, int CurrentWidth, int CurrentHeight, int Axis)
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
         if (Axis IS AXIS_BOTH) {
            if (min_width > min_height) {
               auto scale = (double)min_height / (double)min_width;
               Height = int(Width * scale);
            }
            else {
               auto scale = (double)min_width / (double)min_height;
               Width = int(Height * scale);
            }
         }
         else if (Axis IS AXIS_HORIZONTAL) {
            auto scale = (double)min_height / (double)min_width;
            Height = int(Width * scale);
         }
         else if (Axis IS AXIS_VERTICAL) {
            auto scale = (double)min_width / (double)min_height;
            Width = int(Height * scale);
         }
      }
   }
}

//********************************************************************************************************************

void RepaintWindow(OBJECTID SurfaceID, int X, int Y, int Width, int Height)
{
   kt::ScopedObjectLock<objSurface> surface(SurfaceID);

   if (surface.granted()) {
      if ((Width) and (Height)) surface->exposeToDisplay(X, Y, Width, Height, EXF::CHILDREN);
      else surface->exposeToDisplay(0, 0, 0x7fff, 0x7fff, EXF::CHILDREN);
   }
}

//********************************************************************************************************************

void MsgTimer(void)
{
   ProcessMessages(PMF::NIL, 0);
}

//********************************************************************************************************************

void MsgWindowClose(OBJECTID SurfaceID)
{
   kt::Log log(__FUNCTION__);

   if (SurfaceID) {
      const WinHook hook(SurfaceID, WH::CLOSE);
      FUNCTION func;
      bool has_hook = false;

      {
         const std::lock_guard<std::recursive_mutex> lock(glWindowHookLock);
         if (auto it = glWindowHooks.find(hook); it != glWindowHooks.end()) {
            func = it->second;
            has_hook = true;
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
            glWindowHooks.erase(hook);
         }
         else if (result IS ERR::Cancelled) {
            log.msg("Window closure cancelled by client.");
            return;
         }
      }

      if (!CheckResourceExists(SurfaceID)) FreeResource(SurfaceID);
   }
}

//********************************************************************************************************************

void MsgWindowDestroyed(OBJECTID SurfaceID)
{
   if (SurfaceID) {
      kt::Log log("WinMgr");
      log.branch("Freeing window surface #%d.", SurfaceID);
      FreeResource(SurfaceID);
   }
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

} // namespace
