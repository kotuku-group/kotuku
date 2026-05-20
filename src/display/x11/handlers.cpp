
void process_movement(Window Window, int X, int Y);

static inline OBJECTID get_display(Window Window)
{
   OBJECTID *data, display_id;
   unsigned long nitems, nbytes;
   int format;
   Atom atom;

   if (!XDisplay) return 0;

   if (XGetWindowProperty(XDisplay, Window, atomSurfaceID, 0, 1, False, AnyPropertyType, &atom, &format, &nitems,
         &nbytes, (uint8_t **)&data) IS Success) {
      display_id = data[0];
      XFree(data);
      return display_id;
   }
   return 0;
}

//********************************************************************************************************************

void X11ManagerLoop(HOSTHANDLE FD, APTR Data)
{
   kt::Log log("X11Mgr");
   XEvent xevent;
   XEvent last_motion;
   bool processed_events = false;
   last_motion.xany.window = 0;

   if (!XDisplay) return;

   while (XPending(XDisplay)) {
      processed_events = true;
      XNextEvent(XDisplay, &xevent);
      if ((xevent.type != MotionNotify) and (last_motion.xany.window)) {
         // Buffered MotionNotify event detected, process it now
         process_movement(last_motion.xany.window, last_motion.xmotion.x_root, last_motion.xmotion.y_root);
         last_motion.xany.window = 0;
      }

      switch (xevent.type) {
         case ButtonPress:      handle_button_press(&xevent); break;
         case ButtonRelease:    handle_button_release(&xevent); break;
         case ConfigureNotify:  handle_configure_notify(&xevent.xconfigure); break;
         case EnterNotify:      handle_enter_notify(&xevent.xcrossing); break;
         //case Expose:           handle_exposure(&xevent.xexpose); break;
         case KeyPress:         handle_key_press(&xevent); break;
         case KeyRelease:       handle_key_release(&xevent); break;
         case CirculateNotify:  handle_stack_change(&xevent.xcirculate); break;

         case MotionNotify:
            // Handling of motion events is delayed in case there is a long series of them
            // (i.e. due to rapid pointer movement).
            last_motion = xevent;
            break;

         case FocusIn: {
            kt::Log log("X11Mgr");
            if (auto display_id = get_display(xevent.xany.window)) {
               auto surface_id = GetOwnerID(display_id);
               log.traceBranch("XFocusIn surface #%d", surface_id);
               kt::ScopedObjectLock<objSurface> surface(surface_id);
               if (surface.granted()) surface->focus();
            }
            else log.trace("XFocusIn Failed to get window display ID.");
            break;
         }

         case FocusOut: {
            kt::Log log("X11Mgr");
            if (auto display_id = get_display(xevent.xany.window)) {
               auto surface_id = GetOwnerID(display_id);
               log.traceBranch("XFocusOut surface #%d", surface_id);

               std::vector<OBJECTID> list;
               { // Make a local copy of the focus list
                  const std::lock_guard<std::recursive_mutex> lock(glFocusLock);
                  list = glFocusList;
               }

               if (!list.empty()) {
                  bool in_focus = false;
                  for (auto &id : list) {
                     if ((!in_focus) and (id != surface_id)) continue;
                     in_focus = true;
                     kt::ScopedObjectLock<objSurface> surface(id);
                     if (surface.granted()) surface->lostFocus();
                  }
               }
            }

            break;
         }

         case ClientMessage:
            if (Atom(xevent.xclient.data.l[0]) IS XWADeleteWindow) {
               if (auto display_id = get_display(xevent.xany.window)) {
                  auto surface_id = GetOwnerID(display_id);
                  const WinHook hook(surface_id, WH::CLOSE);
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
                        auto callback = (ERR (*)(OBJECTID, APTR))func.Routine;
                        result = callback(surface_id, func.Meta);
                     }
                     else if (func.isScript()) {
                        sc::Call(func, std::to_array<ScriptArg>({ { "SurfaceID", surface_id, FDF_OBJECTID } }), result);
                     }
                     else result = ERR::Okay;

                     if (result IS ERR::Terminate) {
                        const std::lock_guard<std::recursive_mutex> lock(glWindowHookLock);
                        glWindowHooks.erase(hook);
                     }
                     else if (result IS ERR::Cancelled) {
                        log.msg("Window closure cancelled by client.");
                        break;
                     }
                  }

                  log.msg("Freeing surface %d from display %d.", surface_id, display_id);
                  FreeResource(surface_id);
               }
               else {
                  log.msg("Failed to retrieve display ID for window $%x.", (unsigned int)xevent.xany.window);
                  XDestroyWindow(XDisplay, (Window)xevent.xany.window);
               }
            }
            else if (Atom(xevent.xclient.data.l[0]) IS XWATakeFocus) {
               XSetInputFocus(XDisplay, xevent.xclient.window, RevertToParent, Time(xevent.xclient.data.l[1]));
            }
            break;

         case DestroyNotify:
            if (glPlugin) {
               if (auto display_id = get_display(xevent.xany.window)) {
                  auto surface_id = GetOwnerID(display_id);
                  FreeResource(surface_id);
               }
            }
            break;
      }

      #ifdef XRANDR_ENABLED
      if ((glXRRAvailable) and (XRRUpdateConfiguration(&xevent))) {
         // If randr indicates that the display has been resized, we must adjust the system display to match.  Refer to
         // SetDisplay() for more information.

         auto notify = (XRRScreenChangeNotifyEvent *)&xevent;

         if (auto display_id = get_display(xevent.xany.window)) {
            if (ScopedObjectLock<objSurface> surface(GetOwnerID(display_id), 5000); surface.granted()) {
               // Update the display width/height so that we don't recursively post further display mode updates to the
               // X server.

               if (ScopedObjectLock<extDisplay> display(display_id, 5000); display.granted()) {
                  display->Width  = notify->width;
                  display->Height = notify->height;
                  acResize(*surface, notify->width, notify->height, 0);
               }
            }
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
      if (XDisplay) XSync(XDisplay, False);
   }
}

//********************************************************************************************************************

void handle_button_press(XEvent *xevent)
{
   if (auto pointer = gfx::AccessPointer()) {
      if ((xevent->xbutton.button IS 4) or (xevent->xbutton.button IS 5)) {
         // Mouse wheel movement

         struct dcDeviceInput input = {
            .Values    = { (xevent->xbutton.button IS 4) ? -9.0 : 9.0, 0 },
            .Timestamp = PreciseTime(),
            .Flags     = JTYPE::EXT_MOVEMENT|JTYPE::DIGITAL,
            .Type      = JET::WHEEL
         };

         acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &input, sizeof(input));
      }
      else {
         struct dcDeviceInput input;
         input.Type = JET::NIL;

         if (xevent->xbutton.button IS 1) {
            input.Type  = JET::BUTTON_1;
            input.Values[0] = 1;
         }
         else if (xevent->xbutton.button IS 2) {
            input.Type  = JET::BUTTON_3;
            input.Values[0] = 1;
         }
         else if (xevent->xbutton.button IS 3) {
            input.Type  = JET::BUTTON_2;
            input.Values[0] = 1;
         }

         if (input.Type != JET::NIL) {
            input.Flags = glInputType[int(input.Type)].Flags;
            input.Timestamp = PreciseTime();

            acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &input, sizeof(input));
         }
      }

      ReleaseObject(pointer);
   }

   XFlush(XDisplay);
}

//********************************************************************************************************************

void handle_button_release(XEvent *xevent)
{
   if (auto pointer = gfx::AccessPointer()) {
      struct dcDeviceInput input = {
         .Values    = { 0, 0 },
         .Timestamp = PreciseTime(),
         .Flags     = JTYPE::NIL,
         .Type      = JET::NIL
      };

      if (xevent->xbutton.button IS 1) input.Type  = JET::BUTTON_1;
      else if (xevent->xbutton.button IS 2) input.Type  = JET::BUTTON_3;
      else if (xevent->xbutton.button IS 3) input.Type  = JET::BUTTON_2;

      if (input.Type != JET::NIL) acDataFeed(pointer, nullptr, DATA::DEVICE_INPUT, &input, sizeof(input));
      ReleaseObject(pointer);
   }

   XFlush(XDisplay);

   XWindowAttributes attributes;
   if ((XGetWindowAttributes(XDisplay, xevent->xany.window, &attributes)) and (attributes.override_redirect)) {
      XSetInputFocus(XDisplay, xevent->xany.window, RevertToNone, CurrentTime);
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

   FUNCTION feedback;
   OBJECTID display_id;
   int absx, absy;
   if ((display_id = get_display(xevent->window))) {
      if (ScopedObjectLock<extDisplay> display(display_id, 3000); display.granted()) {
         Window childwin;

         XTranslateCoordinates(XDisplay, (Window)display->WindowHandle, DefaultRootWindow(XDisplay),
            0, 0, &absx, &absy, &childwin);

         display->X = absx;
         display->Y = absy;
         display->Width  = width;
         display->Height = height;
         resize_pixmap(*display, width, height);
         acResize(display->Bitmap, width, height, 0.0);

         feedback = display->ResizeFeedback;
      }
      else {
         log.warning("Failed to get display ID for window %u.", (uint32_t)xevent->window);
         return;
      }
   }
   else {
      log.warning("Failed to retrieve Display from X window.");
      return;
   }

   // Notify with the display and surface unlocked, this reduces the potential for dead-locking.

   log.trace("Sending redimension notification: %dx%d,%dx%d", absx, absy, width, height);

   resize_feedback(&feedback, display_id, absx, absy, width, height);
}

//********************************************************************************************************************

void handle_exposure(XExposeEvent *event)
{
   kt::Log log(__FUNCTION__);
   OBJECTID display_id;

   if ((display_id = get_display(event->window))) {
      OBJECTID surface_id = GetOwnerID(display_id);

      XEvent xevent;
      while (XCheckWindowEvent(XDisplay, event->window, ExposureMask, &xevent) IS True);

      struct drw::ExposeToDisplay region = { .X = 0, .Y = 0, .Width = 20000, .Height = 20000, .Flags = EXF::CHILDREN };
      QueueAction(drw::ExposeToDisplay::id, surface_id, &region); // Redraw everything
   }
   else log.warning("XEvent.Expose: Failed to find a Surface ID for window %u.", (uint32_t)event->window);
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
   else if ((mod_sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0, xevent->xkey.state & ShiftMask ? 1 : 0)) != NoSymbol) {
   }
   else {
      log.trace("Failed to convert keycode to keysym.");
      return;
   }

   KeySym sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0, 0);

   log.traceBranch("XCode: $%x, XSym: $%x, ModSym: $%x, XState: $%x", xevent->xkey.keycode, (int)sym, (int)mod_sym, xevent->xkey.state);

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
      evKey key = {
         .EventID    = EVID_IO_KEYBOARD_KEYPRESS,
         .Qualifiers = glKeyFlags|flags,
         .Code       = value,
         .Unicode    = (int)unicode
      };
      BroadcastEvent(&key, sizeof(key));
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
   else if ((mod_sym = XkbKeycodeToKeysym(XDisplay, xevent->xkey.keycode, 0, xevent->xkey.state & ShiftMask ? 1 : 0)) != NoSymbol) {
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
      evKey key = {
         .EventID    = EVID_IO_KEYBOARD_KEYPRESS,
         .Qualifiers = glKeyFlags|flags,
         .Code       = value,
         .Unicode    = (int)unicode
      };
      BroadcastEvent(&key, sizeof(key));
   }
}

//********************************************************************************************************************

void handle_enter_notify(XCrossingEvent *xevent)
{
   process_movement(xevent->window, xevent->x_root, xevent->y_root);
}

//********************************************************************************************************************

void process_movement(Window Window, int X, int Y)
{
   if (auto pointer = gfx::AccessPointer()) {
      // Refer to the Pointer class to see how this works
      pointer->HostX = X;
      pointer->HostY = Y;

      OBJECTID display_id;
      if ((display_id = get_display(Window))) {
         pointer->set(FID_Surface, GetOwnerID(display_id)); // Alter the surface of the pointer so that it refers to the correct root window
      }

      // Refer to the handler code in the Display class to see how the HostX and HostY fields are updated from afar.

      struct acDataFeed feed;
      struct dcDeviceInput input;
      feed.Object   = nullptr;
      feed.Datatype = DATA::DEVICE_INPUT;
      feed.Buffer   = &input;
      feed.Size     = sizeof(input);
      input.Type      = JET::ABS_XY;
      input.Flags     = JTYPE::NIL;
      input.Values[0] = X;
      input.Values[1] = Y;
      input.Timestamp = PreciseTime();
      Action(AC::DataFeed, pointer, &feed);

      ReleaseObject(pointer);
   }
}
