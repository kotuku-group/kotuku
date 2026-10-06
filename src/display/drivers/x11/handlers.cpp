#include "x11_native.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <chrono>
#include <limits>

namespace display {

static X11Driver::State *glEventState = nullptr;

#define XDisplay (glEventState->Connection)
#define atomSurfaceID (glEventState->SurfaceAtom)
#define XWADeleteWindow (glEventState->DeleteAtom)
#define XWATakeFocus (glEventState->TakeFocusAtom)
#define KeyHeld (glEventState->KeyHeld)
#define glKeyFlags (glEventState->KeyFlags)
#define glDriverCallbacks (*glEventState->Callbacks)

void process_movement(Window Window, int X, int Y);

static inline OBJECTID resolve_surface(Window Window)
{
   const std::lock_guard lock(glEventState->NativeLock);
   if (auto it = glEventState->Windows.find(Window); it != glEventState->Windows.end()) {
      if (it->second->SurfaceID) return it->second->SurfaceID;

      // The fallback matches against Surface.DisplayWindow, which records the native window handle rather than
      // the driver's opaque HOSTWINDOW.

      return glDriverCallbacks.ResolveSurface((APTR)(uintptr_t)it->second->Native);
   }
   return 0;
}

static inline bool adopted_window(Window Window)
{
   const std::lock_guard lock(glEventState->NativeLock);
   auto it = glEventState->Windows.find(Window);
   return (it != glEventState->Windows.end()) and it->second->Adopted;
}

//********************************************************************************************************************

static size_t maximum_property_bytes()
{
   auto units = XExtendedMaxRequestSize(XDisplay);
   if (not units) units = XMaxRequestSize(XDisplay);
   auto bytes = size_t(units) * 4;
   bytes = bytes > 1024 ? bytes - 1024 : 0;
   return std::min(bytes, size_t(std::numeric_limits<int>::max()));
}

//********************************************************************************************************************

static std::string utf8_to_latin1(std::string_view Text)
{
   std::string result;
   result.reserve(Text.size());
   for (size_t offset = 0; offset < Text.size();) {
      auto lead = uint8_t(Text[offset]);
      size_t expected = lead < 0x80 ? 1 : ((lead & 0xe0) IS 0xc0 ? 2 :
         ((lead & 0xf0) IS 0xe0 ? 3 : ((lead & 0xf8) IS 0xf0 ? 4 : 0)));
      if ((not expected) or (offset + expected > Text.size())) {
         result.push_back('?');
         offset++;
         continue;
      }
      int length = 0;
      auto codepoint = UTF8ReadValue(Text.data() + offset, &length);
      if (length <= 0) {
         result.push_back('?');
         offset++;
      }
      else {
         result.push_back(codepoint <= 0xff ? char(codepoint) : '?');
         offset += size_t(length);
      }
   }
   return result;
}

//********************************************************************************************************************

static std::string latin1_to_utf8(const unsigned char *Data, size_t Length)
{
   std::string result;
   result.reserve(Length * 2);
   for (size_t i = 0; i < Length; i++) {
      if (Data[i] < 0x80) result.push_back(char(Data[i]));
      else {
         result.push_back(char(0xc0 | (Data[i] >> 6)));
         result.push_back(char(0x80 | (Data[i] & 0x3f)));
      }
   }
   return result;
}

//********************************************************************************************************************

static void update_server_timestamp(const XEvent &Event)
{
   Time timestamp = CurrentTime;
   switch (Event.type) {
      case KeyPress:
      case KeyRelease: timestamp = Event.xkey.time; break;
      case ButtonPress:
      case ButtonRelease: timestamp = Event.xbutton.time; break;
      case MotionNotify: timestamp = Event.xmotion.time; break;
      case EnterNotify:
      case LeaveNotify: timestamp = Event.xcrossing.time; break;
      case PropertyNotify: timestamp = Event.xproperty.time; break;
      case SelectionClear: timestamp = Event.xselectionclear.time; break;
      case SelectionRequest: timestamp = Event.xselectionrequest.time; break;
      case SelectionNotify: timestamp = Event.xselection.time; break;
   }
   if (timestamp != CurrentTime) {
      const std::lock_guard lock(glEventState->NativeLock);
      glEventState->LastServerTimestamp = timestamp;
   }
}

//********************************************************************************************************************

static void expire_clipboard_transfer()
{
   const std::lock_guard lock(glEventState->NativeLock);
   if (glEventState->ClipboardTransfer and
       (std::chrono::steady_clock::now() - glEventState->TransferStarted >= std::chrono::seconds(2))) {
      kt::Log("X11Mgr").trace("Abandoning an X11 clipboard transfer after two seconds.");
      glEventState->ClipboardTransfer = false;
      glEventState->TransferGeneration++;
   }
}

//********************************************************************************************************************

static bool write_selection_property(Atom Target, Window Requestor, Atom Property)
{
   if ((not Property) or (Target IS glEventState->MultipleAtom)) return false;

   if (Target IS glEventState->TargetsAtom) {
      Atom targets[] = { glEventState->TargetsAtom, glEventState->MultipleAtom, glEventState->TimestampAtom,
         glEventState->UTF8StringAtom, glEventState->TextUTF8Atom, glEventState->TextPlainAtom, XA_STRING,
         glEventState->TextAtom };
      XChangeProperty(XDisplay, Requestor, Property, XA_ATOM, 32, PropModeReplace,
         (const unsigned char *)targets, std::ssize(targets));
      return true;
   }

   if (Target IS glEventState->TimestampAtom) {
      unsigned long timestamp = glEventState->ClipboardTimestamp;
      XChangeProperty(XDisplay, Requestor, Property, XA_INTEGER, 32, PropModeReplace,
         (const unsigned char *)&timestamp, 1);
      return true;
   }

   std::string data;
   if ((Target IS glEventState->UTF8StringAtom) or (Target IS glEventState->TextUTF8Atom) or
       (Target IS glEventState->TextPlainAtom) or (Target IS glEventState->TextAtom)) {
      data = glEventState->ClipboardText;
   }
   else if (Target IS XA_STRING) data = utf8_to_latin1(glEventState->ClipboardText);
   else return false;

   if (data.size() > maximum_property_bytes()) {
      kt::Log("X11Mgr").warning("Clipboard request for %zu bytes exceeds the X11 property limit.", data.size());
      return false;
   }

   XChangeProperty(XDisplay, Requestor, Property, Target, 8, PropModeReplace,
      (const unsigned char *)data.data(), int(data.size()));
   return true;
}

//********************************************************************************************************************

static bool write_multiple_selection(const XSelectionRequestEvent &Request)
{
   if (Request.property IS None) return false;

   Atom actual_type = None;
   int actual_format = 0;
   unsigned long count = 0, remaining = 0;
   unsigned char *property = nullptr;
   auto maximum_longs = std::min<size_t>(maximum_property_bytes() / 4,
      size_t(std::numeric_limits<long>::max()));
   auto status = XGetWindowProperty(XDisplay, Request.requestor, Request.property, 0, long(maximum_longs), False,
      glEventState->AtomPairAtom, &actual_type, &actual_format, &count, &remaining, &property);
   if ((status != Success) or remaining or (actual_type != glEventState->AtomPairAtom) or
       (actual_format != 32) or (count & 1)) {
      if (property) XFree(property);
      return false;
   }

   auto pairs = (Atom *)property;
   for (unsigned long i = 0; i < count; i += 2) {
      if (not write_selection_property(pairs[i], Request.requestor, pairs[i + 1])) pairs[i] = None;
   }
   XChangeProperty(XDisplay, Request.requestor, Request.property, glEventState->AtomPairAtom, 32, PropModeReplace,
      property, int(count));
   if (property) XFree(property);
   return true;
}

//********************************************************************************************************************

static void handle_selection_request(const XSelectionRequestEvent &Request)
{
   XSelectionEvent reply = {};
   reply.type = SelectionNotify;
   reply.display = Request.display;
   reply.requestor = Request.requestor;
   reply.selection = Request.selection;
   reply.target = Request.target;
   reply.time = Request.time;
   reply.property = None;

   const std::lock_guard lock(glEventState->NativeLock);
   if ((Request.owner IS glEventState->ClipboardWindow) and
       (Request.selection IS glEventState->ClipboardAtom) and
       (XGetSelectionOwner(XDisplay, glEventState->ClipboardAtom) IS glEventState->ClipboardWindow)) {
      if (Request.target IS glEventState->MultipleAtom) {
         if (write_multiple_selection(Request)) reply.property = Request.property;
      }
      else {
         auto property = Request.property IS None ? Request.target : Request.property;
         if (write_selection_property(Request.target, Request.requestor, property)) reply.property = property;
      }
   }

   XSendEvent(XDisplay, Request.requestor, False, NoEventMask, (XEvent *)&reply);
}

//********************************************************************************************************************

static void handle_selection_clear(const XSelectionClearEvent &Event)
{
   bool notify = false;
   {
      const std::lock_guard lock(glEventState->NativeLock);
      if ((Event.window IS glEventState->ClipboardWindow) and
          (Event.selection IS glEventState->ClipboardAtom) and
          (XGetSelectionOwner(XDisplay, glEventState->ClipboardAtom) != glEventState->ClipboardWindow)) {
         glEventState->ClipboardText.clear();
         glEventState->ClipboardTimestamp = CurrentTime;
         notify = true;
      }
   }
   if (notify and glDriverCallbacks.ClipboardLost) glDriverCallbacks.ClipboardLost();
}

//********************************************************************************************************************

static void handle_selection_notify(const XSelectionEvent &Event)
{
   std::string data;
   std::string mime;
   bool deliver = false;

   {
      const std::lock_guard lock(glEventState->NativeLock);
      if ((Event.requestor != glEventState->ClipboardWindow) or
          (Event.selection != glEventState->ClipboardAtom) or (not glEventState->ClipboardTransfer) or
          (Event.time != glEventState->TransferTimestamp)) return;

      if (Event.property IS None) {
         kt::Log("X11Mgr").trace("The X11 clipboard owner refused target %lu.", Event.target);
         glEventState->ClipboardTransfer = false;
         return;
      }

      Atom actual_type = None;
      int actual_format = 0;
      unsigned long count = 0, remaining = 0;
      unsigned char *property = nullptr;
      auto maximum_longs = std::min<size_t>(maximum_property_bytes() / 4,
         size_t(std::numeric_limits<long>::max()));
      auto status = XGetWindowProperty(XDisplay, glEventState->ClipboardWindow, Event.property, 0,
         long(maximum_longs), True, AnyPropertyType, &actual_type, &actual_format, &count, &remaining, &property);
      if ((status != Success) or remaining or (actual_type IS glEventState->IncrAtom)) {
         if (property) XFree(property);
         kt::Log("X11Mgr").trace("The X11 clipboard transfer requires an unsupported incremental read.");
         glEventState->ClipboardTransfer = false;
         return;
      }

      if (Event.target IS glEventState->TargetsAtom) {
         Atom selected = None;
         if ((actual_type IS XA_ATOM) and (actual_format IS 32)) {
            auto targets = (Atom *)property;
            const Atom preferences[] = { glEventState->URIListAtom, glEventState->UTF8StringAtom,
               glEventState->TextUTF8Atom, glEventState->TextPlainAtom, XA_STRING };
            for (auto preference : preferences) {
               for (unsigned long i = 0; i < count; i++) if (targets[i] IS preference) {
                  selected = preference;
                  break;
               }
               if (selected != None) break;
            }
         }
         if (property) XFree(property);
         if (selected IS None) {
            glEventState->ClipboardTransfer = false;
            return;
         }
         glEventState->TransferTarget = selected;
         glEventState->TransferStarted = std::chrono::steady_clock::now();
         XConvertSelection(XDisplay, glEventState->ClipboardAtom, selected,
            glEventState->ClipboardPropertyAtom, glEventState->ClipboardWindow,
            glEventState->TransferTimestamp);
         return;
      }

      if (actual_format != 8) {
         if (property) XFree(property);
         glEventState->ClipboardTransfer = false;
         return;
      }

      if ((glEventState->TransferTarget IS XA_STRING) or (actual_type IS XA_STRING))
         data = latin1_to_utf8(property, count);
      else if (count) data.assign((const char *)property, count);
      if (property) XFree(property);
      mime = glEventState->TransferTarget IS glEventState->URIListAtom ? "text/uri-list" :
         "text/plain;charset=utf-8";
      glEventState->ClipboardTransfer = false;
      deliver = true;
   }

   if (deliver and glDriverCallbacks.ClipboardData)
      glDriverCallbacks.ClipboardData(mime.c_str(), data.data(), data.size(), false, 0);
}

#ifdef XFIXES_ENABLED
//********************************************************************************************************************

static void handle_xfixes_selection(const XFixesSelectionNotifyEvent &Event)
{
   if ((Event.selection != glEventState->ClipboardAtom) or
       (Event.window != glEventState->ClipboardWindow)) return;
   {
      const std::lock_guard lock(glEventState->NativeLock);
      glEventState->LastServerTimestamp = Event.timestamp;
   }

   if ((Event.subtype IS XFixesSetSelectionOwnerNotify) and
       (Event.owner != glEventState->ClipboardWindow) and (Event.owner != None)) {
      if (glDriverCallbacks.ClipboardLost) glDriverCallbacks.ClipboardLost();
      x11_request_clipboard(glEventState, Event.selection_timestamp);
   }
   else if (((Event.subtype IS XFixesSetSelectionOwnerNotify) and (Event.owner IS None)) or
            (Event.subtype IS XFixesSelectionWindowDestroyNotify) or
            (Event.subtype IS XFixesSelectionClientCloseNotify)) {
      {
         const std::lock_guard lock(glEventState->NativeLock);
         glEventState->ClipboardTransfer = false;
         glEventState->TransferGeneration++;
      }
      if (glDriverCallbacks.ClipboardLost) glDriverCallbacks.ClipboardLost();
   }
}
#endif

//********************************************************************************************************************
// X11 desktop environments announce a change of display scaling by updating Xft.dpi in the root window's resource
// database.  As with WM_DPICHANGED on Windows, the surface of each window is notified so that its display discards
// the cached density.

static void handle_resource_change()
{
   auto dpi = x11_read_dpi(glEventState);
   if (glEventState->DPI.exchange(dpi) IS dpi) return;

   kt::Log log("X11Mgr");
   log.msg("Desktop DPI changed to %d.", dpi);
   if (not glDriverCallbacks.DPIChanged) return;

   std::vector<Window> windows;
   {
      const std::lock_guard lock(glEventState->NativeLock);
      for (auto &[native, record] : glEventState->Windows) windows.push_back(native);
   }

   for (auto window : windows) {
      if (auto surface_id = resolve_surface(window)) glDriverCallbacks.DPIChanged(surface_id);
   }
}

//********************************************************************************************************************

void X11ManagerLoop(HOSTHANDLE FD, APTR Data)
{
   kt::Log log("X11Mgr");
   XEvent xevent;
   XEvent last_motion;
   bool processed_events = false;
   last_motion.xany.window = 0;

   if (not XDisplay) return;

   while (true) {
      {
         const std::lock_guard lock(glEventState->NativeLock);
         if (not XPending(XDisplay)) break;
         XNextEvent(XDisplay, &xevent);
      }
      processed_events = true;
      update_server_timestamp(xevent);
      expire_clipboard_transfer();
      if ((xevent.type != MotionNotify) and (last_motion.xany.window)) {
         // Buffered MotionNotify event detected, process it now
         process_movement(last_motion.xany.window, last_motion.xmotion.x_root, last_motion.xmotion.y_root);
         last_motion.xany.window = 0;
      }

#ifdef XFIXES_ENABLED
      if (glEventState->XFixes and
          (xevent.type IS glEventState->XFixesEventBase + XFixesSelectionNotify)) {
         handle_xfixes_selection(*(XFixesSelectionNotifyEvent *)&xevent);
         continue;
      }
#endif

      switch (xevent.type) {
         case ButtonPress:      handle_button_press(&xevent); break;
         case ButtonRelease:    handle_button_release(&xevent); break;
         case ConfigureNotify:  handle_configure_notify(&xevent.xconfigure); break;
         case EnterNotify:
         case LeaveNotify:      handle_crossing_notify(&xevent.xcrossing); break;
         //case Expose:           handle_exposure(&xevent.xexpose); break;
         case KeyPress:         handle_key_press(&xevent); break;
         case KeyRelease:       handle_key_release(&xevent); break;
         case CirculateNotify:  handle_stack_change(&xevent.xcirculate); break;
         case SelectionRequest: handle_selection_request(xevent.xselectionrequest); break;
         case SelectionClear:   handle_selection_clear(xevent.xselectionclear); break;
         case SelectionNotify:  handle_selection_notify(xevent.xselection); break;

         case PropertyNotify:
            if ((xevent.xproperty.window IS DefaultRootWindow(XDisplay)) and
                (xevent.xproperty.atom IS XA_RESOURCE_MANAGER)) handle_resource_change();
            break;

         case MotionNotify:
            // Handling of motion events is delayed in case there is a long series of them
            // (i.e. due to rapid pointer movement).
            last_motion = xevent;
            break;

         case FocusIn: {
            kt::Log log("X11Mgr");
            if (auto surface_id = resolve_surface(xevent.xany.window)) {
               log.traceBranch("XFocusIn surface #%d", surface_id);
               glDriverCallbacks.FocusState(surface_id, true);
            }
            else log.trace("XFocusIn failed to resolve the window surface.");
            break;
         }

         case FocusOut: {
            kt::Log log("X11Mgr");
            if (auto surface_id = resolve_surface(xevent.xany.window)) {
               log.traceBranch("XFocusOut surface #%d", surface_id);
               glDriverCallbacks.FocusState(surface_id, false);
            }

            break;
         }

         case ClientMessage:
            // Only WM_PROTOCOLS messages carry a protocol atom in l[0].  Other messages, such as the compositor's
            // _NET_WM_FRAME_DRAWN, carry arbitrary values there that can coincide with a protocol atom.

            if (xevent.xclient.message_type != glEventState->ProtocolsAtom) break;

            if (Atom(xevent.xclient.data.l[0]) IS XWADeleteWindow) {
               auto surface_id = resolve_surface(xevent.xany.window);
               if (glDriverCallbacks.WindowClose(surface_id) IS ERR::Cancelled) break;
            }
            else if (Atom(xevent.xclient.data.l[0]) IS XWATakeFocus) {
               XSetInputFocus(XDisplay, xevent.xclient.window, RevertToParent, Time(xevent.xclient.data.l[1]));
            }
            else if ((glEventState->SyncRequestAtom) and
                  (Atom(xevent.xclient.data.l[0]) IS glEventState->SyncRequestAtom)) {
               // The window manager is about to configure the window and will wait for the counter to reach this
               // value, which is set once the next frame has been presented.

               const std::lock_guard lock(glEventState->NativeLock);
               if (auto it = glEventState->Windows.find(xevent.xclient.window); it != glEventState->Windows.end()) {
                  it->second->SyncRequest = int64_t((uint64_t(uint32_t(xevent.xclient.data.l[3])) << 32) |
                     uint64_t(uint32_t(xevent.xclient.data.l[2])));
                  it->second->SyncRequestExtended = xevent.xclient.data.l[4] != 0;
                  if (it->second->SyncRequestExtended) x11_begin_frame(glEventState, it->second);
               }
            }
            break;

         case DestroyNotify:
            // Only adopted windows are torn down here, because the client owns the parent and its destruction is the
            // sole notification that the display has gone away.  Windows created by this driver are erased from the
            // window table before XDestroyWindow() is issued, so this path must not be relied upon for them.

            if (adopted_window(xevent.xany.window)) {
               glDriverCallbacks.WindowDestroyed(resolve_surface(xevent.xany.window));
            }
            break;
      }

      #ifdef XRANDR_ENABLED
      // XRRUpdateConfiguration() refreshes Xlib's cached screen size and must see every screen change event.

      if ((glEventState->RandR) and XRRUpdateConfiguration(&xevent) and
          (xevent.type IS glEventState->RandREventBase + RRScreenChangeNotify)) {
         XGetWindowAttributes(XDisplay, DefaultRootWindow(XDisplay), &glEventState->RootAttributes);
         log.msg("Screen size changed to %dx%d.", glEventState->RootAttributes.width,
            glEventState->RootAttributes.height);

         // If the root window hosts a display (full-screen mode), resize the display to match.  Refer to
         // SetDisplay() for more information.

         auto notify = (XRRScreenChangeNotifyEvent *)&xevent;
         if (auto surface_id = resolve_surface(xevent.xany.window)) {
            glDriverCallbacks.WindowResized(surface_id, 0, 0, notify->width, notify->height,
               0, 0, notify->width, notify->height);
         }
      }
      #endif
   }

   if (last_motion.xany.window) {
      processed_events = true;
      process_movement(last_motion.xany.window, last_motion.xmotion.x_root, last_motion.xmotion.y_root);
   }

   if (processed_events) {
      XFlush(XDisplay);
   }
}

void x11_process_events(X11Driver::State *State)
{
   if ((not State) or (not State->Callbacks) or State->Closing) return;
   glEventState = State;
   X11ManagerLoop(State->ConnectionFD, State);
   glEventState = nullptr;
}

//********************************************************************************************************************

void handle_button_press(XEvent *xevent)
{
   auto surface_id = resolve_surface(xevent->xbutton.window);

   if (xevent->xbutton.button IS 4) glDriverCallbacks.WheelMovement(surface_id, -9.0);
   else if (xevent->xbutton.button IS 5) glDriverCallbacks.WheelMovement(surface_id, 9.0);
   else if (xevent->xbutton.button IS 1) glDriverCallbacks.ButtonInput(0x0001, true);
   else if (xevent->xbutton.button IS 2) glDriverCallbacks.ButtonInput(0x0004, true);
   else if (xevent->xbutton.button IS 3) glDriverCallbacks.ButtonInput(0x0002, true);
   else if (xevent->xbutton.button IS 8) glDriverCallbacks.ButtonInput(0x0008, true);
   else if (xevent->xbutton.button IS 9) glDriverCallbacks.ButtonInput(0x0010, true);

   XFlush(XDisplay);
}

//********************************************************************************************************************

void handle_button_release(XEvent *xevent)
{
   if (xevent->xbutton.button IS 1) glDriverCallbacks.ButtonInput(0x0001, false);
   else if (xevent->xbutton.button IS 2) glDriverCallbacks.ButtonInput(0x0004, false);
   else if (xevent->xbutton.button IS 3) glDriverCallbacks.ButtonInput(0x0002, false);
   else if (xevent->xbutton.button IS 8) glDriverCallbacks.ButtonInput(0x0008, false);
   else if (xevent->xbutton.button IS 9) glDriverCallbacks.ButtonInput(0x0010, false);

   XFlush(XDisplay);

   // The window manager does not focus override-redirect windows, so a click focusses them directly.  Wheel buttons
   // (4 to 7) do not change the focus, nor do surfaces that refuse it, such as a pop-up menu owned by another window.

   if ((xevent->xbutton.button >= 4) and (xevent->xbutton.button <= 7)) return;

   XWindowAttributes attributes;
   if ((XGetWindowAttributes(XDisplay, xevent->xany.window, &attributes)) and (attributes.override_redirect)) {
      auto surface_id = resolve_surface(xevent->xany.window);
      if ((not surface_id) or (glDriverCallbacks.AcceptsFocus(surface_id))) {
         XSetInputFocus(XDisplay, xevent->xany.window, RevertToNone, CurrentTime);
      }
   }
}

//********************************************************************************************************************

void handle_stack_change(XCirculateEvent *xevent)
{
   kt::Log log(__FUNCTION__);
   log.trace("Window %d stack position has changed.", (int)xevent->window);
}

//********************************************************************************************************************
// Event handler for window resizing and movement

void handle_configure_notify(XConfigureEvent *xevent)
{
   kt::Log log(__FUNCTION__);

   int x = xevent->x;
   int y = xevent->y;
   int width = xevent->width;
   int height = xevent->height;

   XEvent event;
   while (XCheckTypedWindowEvent(XDisplay, xevent->window, ConfigureNotify, &event) IS True) {
      x = event.xconfigure.x;
      y = event.xconfigure.y;
      width = event.xconfigure.width;
      height = event.xconfigure.height;
   }

   log.traceBranch("Win: %d, Pos: %dx%d,%dx%d", (int)xevent->window, x, y, width, height);

   int absx, absy;
   Window child;
   XTranslateCoordinates(XDisplay, xevent->window, DefaultRootWindow(XDisplay), 0, 0, &absx, &absy, &child);
   glDriverCallbacks.WindowResized(resolve_surface(xevent->window), absx, absy, width, height,
      absx, absy, width, height);
}

//********************************************************************************************************************

void handle_exposure(XExposeEvent *event)
{
   XEvent xevent;
   while (XCheckWindowEvent(XDisplay, event->window, ExposureMask, &xevent) IS True);
   glDriverCallbacks.ExposeRegion(resolve_surface(event->window), event->x, event->y, event->width, event->height);
}

//********************************************************************************************************************
// XK symbols are defined in X11/keysymdef.h

KEY xkeysym_to_pkey(KeySym KSym)
{
   switch(KSym) {
      case XK_A: return KEY::A;
      case XK_B: return KEY::B;
      case XK_C: return KEY::C;
      case XK_D: return KEY::D;
      case XK_E: return KEY::E;
      case XK_F: return KEY::F;
      case XK_G: return KEY::G;
      case XK_H: return KEY::H;
      case XK_I: return KEY::I;
      case XK_J: return KEY::J;
      case XK_K: return KEY::K;
      case XK_L: return KEY::L;
      case XK_M: return KEY::M;
      case XK_N: return KEY::N;
      case XK_O: return KEY::O;
      case XK_P: return KEY::P;
      case XK_Q: return KEY::Q;
      case XK_R: return KEY::R;
      case XK_S: return KEY::S;
      case XK_T: return KEY::T;
      case XK_U: return KEY::U;
      case XK_V: return KEY::V;
      case XK_W: return KEY::W;
      case XK_X: return KEY::X;
      case XK_Y: return KEY::Y;
      case XK_Z: return KEY::Z;
      case XK_a: return KEY::A;
      case XK_b: return KEY::B;
      case XK_c: return KEY::C;
      case XK_d: return KEY::D;
      case XK_e: return KEY::E;
      case XK_f: return KEY::F;
      case XK_g: return KEY::G;
      case XK_h: return KEY::H;
      case XK_i: return KEY::I;
      case XK_j: return KEY::J;
      case XK_k: return KEY::K;
      case XK_l: return KEY::L;
      case XK_m: return KEY::M;
      case XK_n: return KEY::N;
      case XK_o: return KEY::O;
      case XK_p: return KEY::P;
      case XK_q: return KEY::Q;
      case XK_r: return KEY::R;
      case XK_s: return KEY::S;
      case XK_t: return KEY::T;
      case XK_u: return KEY::U;
      case XK_v: return KEY::V;
      case XK_w: return KEY::W;
      case XK_x: return KEY::X;
      case XK_y: return KEY::Y;
      case XK_z: return KEY::Z;

      case XK_bracketleft:  return KEY::L_SQUARE;
      case XK_backslash:    return KEY::BACK_SLASH;
      case XK_bracketright: return KEY::R_SQUARE;
      case XK_asciicircum:  return KEY::SIX; // US conversion
      case XK_underscore:   return KEY::MINUS; // US conversion
      case XK_grave:        return KEY::REVERSE_QUOTE;
      case XK_space:        return KEY::SPACE;
      case XK_exclam:       return KEY::ONE; // US conversion
      case XK_quotedbl:     return KEY::APOSTROPHE; // US conversion
      case XK_numbersign:   return KEY::THREE; // US conversion
      case XK_dollar:       return KEY::FOUR; // US conversion
      case XK_percent:      return KEY::FIVE; // US conversion
      case XK_ampersand:    return KEY::SEVEN; // US conversion
      case XK_apostrophe:   return KEY::APOSTROPHE;
      case XK_parenleft:    return KEY::NINE; // US conversion
      case XK_parenright:   return KEY::ZERO; // US conversion
      case XK_asterisk:     return KEY::EIGHT; // US conversion
      case XK_plus:         return KEY::EQUALS; // US conversion
      case XK_comma:        return KEY::COMMA;
      case XK_minus:        return KEY::MINUS;
      case XK_period:       return KEY::PERIOD;
      case XK_slash:        return KEY::SLASH;
      case XK_0:            return KEY::ZERO;
      case XK_1:            return KEY::ONE;
      case XK_2:            return KEY::TWO;
      case XK_3:            return KEY::THREE;
      case XK_4:            return KEY::FOUR;
      case XK_5:            return KEY::FIVE;
      case XK_6:            return KEY::SIX;
      case XK_7:            return KEY::SEVEN;
      case XK_8:            return KEY::EIGHT;
      case XK_9:            return KEY::NINE;
      case XK_KP_0:         return KEY::NP_0;
      case XK_KP_1:         return KEY::NP_1;
      case XK_KP_2:         return KEY::NP_2;
      case XK_KP_3:         return KEY::NP_3;
      case XK_KP_4:         return KEY::NP_4;
      case XK_KP_5:         return KEY::NP_5;
      case XK_KP_6:         return KEY::NP_6;
      case XK_KP_7:         return KEY::NP_7;
      case XK_KP_8:         return KEY::NP_8;
      case XK_KP_9:         return KEY::NP_9;
      case XK_colon:        return KEY::SEMI_COLON; // US conversion
      case XK_semicolon:    return KEY::SEMI_COLON;
      case XK_less:         return KEY::COMMA; // US conversion
      case XK_equal:        return KEY::EQUALS;
      case XK_greater:      return KEY::PERIOD; // US conversion
      case XK_question:     return KEY::SLASH; // US conversion
      case XK_at:           return KEY::AT;
      case XK_KP_Multiply:  return KEY::NP_MULTIPLY;
      case XK_KP_Add:       return KEY::NP_PLUS;
      case XK_KP_Separator: return KEY::NP_BAR;
      case XK_KP_Subtract:  return KEY::NP_MINUS;
      case XK_KP_Decimal:   return KEY::NP_DOT;
      case XK_KP_Divide:    return KEY::NP_DIVIDE;
      case XK_KP_Enter:     return KEY::NP_ENTER;

      case XK_Shift_L:      return KEY::L_SHIFT;
      case XK_Shift_R:      return KEY::R_SHIFT;
      case XK_Control_L:    return KEY::L_CONTROL;
      case XK_Control_R:    return KEY::R_CONTROL;
      case XK_Caps_Lock:    return KEY::CAPS_LOCK;
      //case XK_Shift_Lock:   return KEY::SHIFT_LOCK;

      case XK_Meta_L:       return KEY::L_COMMAND;
      case XK_Meta_R:       return KEY::R_COMMAND;
      case XK_Alt_L:        return KEY::L_ALT;
      case XK_Alt_R:        return KEY::R_ALT;
      //case XK_Super_L:      return KEY::;
      //case XK_Super_R:      return KEY::;
      //case XK_Hyper_L:      return KEY::;
      //case XK_Hyper_R:      return KEY::;

      case XK_BackSpace:    return KEY::BACKSPACE;
      case XK_Tab:          return KEY::TAB;
      case XK_Linefeed:     return KEY::ENTER;
      case XK_Clear:        return KEY::CLEAR;
      case XK_Return:       return KEY::ENTER;
      case XK_Pause:        return KEY::PAUSE;
      case XK_Scroll_Lock:  return KEY::SCR_LOCK;
      case XK_Sys_Req:      return KEY::SYSRQ;
      case XK_Escape:       return KEY::ESCAPE;
      case XK_Delete:       return KEY::DELETE;

      case XK_Home:         return KEY::HOME;
      case XK_Left:         return KEY::LEFT;
      case XK_Up:           return KEY::UP;
      case XK_Right:        return KEY::RIGHT;
      case XK_Down:         return KEY::DOWN;
      case XK_Page_Up:      return KEY::PAGE_UP;
      case XK_Page_Down:    return KEY::PAGE_DOWN;
      case XK_End:          return KEY::END;

      case XK_Select:        return KEY::SELECT;
      //case XK_3270_PrintScreen: return KEY::PRT_SCR;
      case XK_Print:         return KEY::PRINT;
      case XK_Execute:       return KEY::EXECUTE;
      case XK_Insert:        return KEY::INSERT;
      case XK_Undo:          return KEY::UNDO;
      case XK_Redo:          return KEY::REDO;
      case XK_Menu:          return KEY::MENU;
      case XK_Find:          return KEY::FIND;
      case XK_Cancel:        return KEY::CANCEL;
      case XK_Help:          return KEY::HELP;
      case XK_Break:         return KEY::BREAK;
      case XK_Num_Lock:      return KEY::NUM_LOCK;
      //case XK_Mode_switch:   return KEY::;  /* Character set switch */
      //case XK_script_switch: return KEY::;  /* Alias for mode_switch */

      case XK_F1:           return KEY::F1;
      case XK_F2:           return KEY::F2;
      case XK_F3:           return KEY::F3;
      case XK_F4:           return KEY::F4;
      case XK_F5:           return KEY::F5;
      case XK_F6:           return KEY::F6;
      case XK_F7:           return KEY::F7;
      case XK_F8:           return KEY::F8;
      case XK_F9:           return KEY::F9;
      case XK_F10:          return KEY::F10;
      case XK_F11:          return KEY::F11;
      case XK_F12:          return KEY::F12;
      case XK_F13:          return KEY::F13;
      case XK_F14:          return KEY::F14;
      case XK_F15:          return KEY::F15;
      case XK_F16:          return KEY::F16;
      case XK_F17:          return KEY::F17;
      case XK_F18:          return KEY::F18;
      case XK_F19:          return KEY::F19;
      case XK_F20:          return KEY::F20;
      default: return KEY::NIL;
   }
}

/*********************************************************************************************************************
** Refer: man page XKeyEvent
*/

void handle_key_press(XEvent *xevent)
{
   kt::Log log(__FUNCTION__);
   uint32_t unicode = 0;
   KeySym mod_sym; // A KeySym is an encoding of a symbol on the cap of a key.  See X11/keysym.h
   static XComposeStatus glXComposeStatus = { 0, 0 };
   char buffer[12];
   int out;
   if ((out = XLookupString(&xevent->xkey, buffer, sizeof(buffer)-1, &mod_sym, &glXComposeStatus)) > 0) {
      if (buffer[0] >= 0x20) {
         buffer[out] = 0;
         unicode = UTF8ReadValue(buffer, nullptr);
      }
   }
   else if ((mod_sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0,
         xevent->xkey.state & ShiftMask ? 1 : 0)) != NoSymbol) {
   }
   else {
      log.trace("Failed to convert keycode to keysym.");
      return;
   }

   KeySym sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0, 0);

   log.traceBranch("XCode: $%x, XSym: $%x, ModSym: $%x, XState: $%x", xevent->xkey.keycode, (int)sym,
      (int)mod_sym, xevent->xkey.state);

   auto value = xkeysym_to_pkey(sym);
   auto flags = KQ::PRESSED;

   if (xevent->xkey.state & LockMask) flags |= KQ::CAPS_LOCK;
   if (((int(value) >= int(KEY::NP_0)) and (int(value) <= int(KEY::NP_DIVIDE))) or (value IS KEY::NP_ENTER)) {
      flags |= KQ::NUM_PAD;
   }

   if ((value != KEY::NIL) and (int(value) < std::ssize(KeyHeld))) {
      if (KeyHeld[int(value)]) flags |= KQ::REPEAT;
      else KeyHeld[int(value)] = 1;

      if (value IS KEY::L_COMMAND)      glKeyFlags |= KQ::L_COMMAND;
      else if (value IS KEY::R_COMMAND) glKeyFlags |= KQ::R_COMMAND;
      else if (value IS KEY::L_SHIFT)   glKeyFlags |= KQ::L_SHIFT;
      else if (value IS KEY::R_SHIFT)   glKeyFlags |= KQ::R_SHIFT;
      else if (value IS KEY::L_CONTROL) glKeyFlags |= KQ::L_CONTROL;
      else if (value IS KEY::R_CONTROL) glKeyFlags |= KQ::R_CONTROL;
      else if (value IS KEY::L_ALT)     glKeyFlags |= KQ::L_ALT;
      else if (value IS KEY::R_ALT)     glKeyFlags |= KQ::R_ALT;
   }

   if ((value != KEY::NIL) or (unicode != 0xffffffff)) {
     if ((unicode < 0x20) or (unicode IS 127)) flags |= KQ::NOT_PRINTABLE;
      glDriverCallbacks.KeyPressed(glKeyFlags|flags, value, int(unicode));
   }
}

//********************************************************************************************************************

void handle_key_release(XEvent *xevent)
{
   kt::Log log(__FUNCTION__);

   // Check if the key is -really- released (when keys are held down, X11 annoyingly generates a stream of release
   // events until it is really released).

   if (XPending(XDisplay)) {
      XEvent peekevent;
      XPeekEvent(XDisplay, &peekevent);
      if ((peekevent.type IS KeyPress) and
          (peekevent.xkey.keycode IS xevent->xkey.keycode) and
          ((peekevent.xkey.time - xevent->xkey.time) < 2)) {
         // The key is held and repeated, so do not release it
         log.trace("XKey $%x is held and repeated, not releasing.", xevent->xkey.keycode);
         return;
      }
   }

   // A KeySym is an encoding of a symbol on the cap of a key.  See X11/keysym.h

   uint32_t unicode = 0;
   KeySym mod_sym;
   static XComposeStatus glXComposeStatus = { 0, 0 };
   char buf[12];
   int out;
   if ((out = XLookupString(&xevent->xkey, buf, sizeof(buf)-1, &mod_sym, &glXComposeStatus)) > 0) {
      buf[out] = 0;
      unicode = UTF8ReadValue(buf, nullptr);
   }
   else if ((mod_sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0,
         xevent->xkey.state & ShiftMask ? 1 : 0)) != NoSymbol) {
   }
   else {
      log.trace("XLookupString() failed to convert keycode to keysym.");
      return;
   }

   KeySym sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0, 0);

   auto value = xkeysym_to_pkey(sym);
   auto flags = KQ::RELEASED;

   if ((value != KEY::NIL) and (int(value) < std::ssize(KeyHeld))) {
      KeyHeld[int(value)] = 0;

      if (value IS KEY::L_COMMAND)      glKeyFlags &= ~KQ::L_COMMAND;
      else if (value IS KEY::R_COMMAND) glKeyFlags &= ~KQ::R_COMMAND;
      else if (value IS KEY::L_SHIFT)   glKeyFlags &= ~KQ::L_SHIFT;
      else if (value IS KEY::R_SHIFT)   glKeyFlags &= ~KQ::R_SHIFT;
      else if (value IS KEY::L_CONTROL) glKeyFlags &= ~KQ::L_CONTROL;
      else if (value IS KEY::R_CONTROL) glKeyFlags &= ~KQ::R_CONTROL;
      else if (value IS KEY::L_ALT)     glKeyFlags &= ~KQ::L_ALT;
      else if (value IS KEY::R_ALT)     glKeyFlags &= ~KQ::R_ALT;
   }

  if ((value != KEY::NIL) or (unicode != 0xffffffff)) {
     if ((unicode < 0x20) or (unicode IS 127)) flags |= KQ::NOT_PRINTABLE;
      glDriverCallbacks.KeyReleased(glKeyFlags|flags, value);
   }
}

//********************************************************************************************************************

void handle_crossing_notify(XCrossingEvent *xevent)
{
   auto surface_id = resolve_surface(xevent->window);
   bool entered = xevent->type IS EnterNotify;
   glDriverCallbacks.Crossing(surface_id, entered, xevent->x_root, xevent->y_root);
   if (entered) glDriverCallbacks.Movement(surface_id, xevent->x_root, xevent->y_root, false);
}

//********************************************************************************************************************

void process_movement(Window Window, int X, int Y)
{
   glDriverCallbacks.Movement(resolve_surface(Window), X, Y, false);
}

} // namespace display
