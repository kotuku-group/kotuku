/*********************************************************************************************************************

-CLASS-
Display: Manages the video display and graphics hardware.

A Display object represents a region of displayable video memory and metadata that defines the display mode.
The Display is a primitive, hardware oriented interface.  It is recommended that unless otherwise required, the
@Surface class is used to create displayable graphics regions.

-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

// Class definition at end of this source file.

static ERR DISPLAY_Resize(extDisplay *, struct acResize *);
static CSTRING dpms_name(DPMS Index);

static void alloc_display_buffer(extDisplay *Self);

#ifdef _GLES_
static const int attributes[] = {
   EGL_BUFFER_SIZE,
   EGL_ALPHA_SIZE,
   EGL_BLUE_SIZE,
   EGL_GREEN_SIZE,
   EGL_RED_SIZE,
   EGL_DEPTH_SIZE,
   EGL_STENCIL_SIZE,
   EGL_CONFIG_CAVEAT,
   EGL_CONFIG_ID,
   EGL_LEVEL,
   EGL_MAX_PBUFFER_HEIGHT,
   EGL_MAX_PBUFFER_PIXELS,
   EGL_MAX_PBUFFER_WIDTH,
   EGL_NATIVE_RENDERABLE,
   EGL_NATIVE_VISUAL_ID,
   EGL_NATIVE_VISUAL_TYPE,
   0x3030, // EGL10.EGL_PRESERVED_RESOURCES,
   EGL_SAMPLES,
   EGL_SAMPLE_BUFFERS,
   EGL_SURFACE_TYPE,
   EGL_TRANSPARENT_TYPE,
   EGL_TRANSPARENT_RED_VALUE,
   EGL_TRANSPARENT_GREEN_VALUE,
   EGL_TRANSPARENT_BLUE_VALUE,
   0x3039, // EGL10.EGL_BIND_TO_TEXTURE_RGB,
   0x303A, // EGL10.EGL_BIND_TO_TEXTURE_RGBA,
   0x303B, // EGL10.EGL_MIN_SWAP_INTERVAL,
   0x303C, // EGL10.EGL_MAX_SWAP_INTERVAL,
   EGL_LUMINANCE_SIZE,
   EGL_ALPHA_MASK_SIZE,
   EGL_COLOR_BUFFER_TYPE,
   EGL_RENDERABLE_TYPE,
   0x3042 // EGL10.EGL_CONFORMANT
};

static const CSTRING names[] = {
  "EGL_BUFFER_SIZE",         "EGL_ALPHA_SIZE",            "EGL_BLUE_SIZE",               "EGL_GREEN_SIZE",
  "EGL_RED_SIZE",            "EGL_DEPTH_SIZE",            "EGL_STENCIL_SIZE",            "EGL_CONFIG_CAVEAT",
  "EGL_CONFIG_ID",           "EGL_LEVEL",                 "EGL_MAX_PBUFFER_HEIGHT",      "EGL_MAX_PBUFFER_PIXELS",
  "EGL_MAX_PBUFFER_WIDTH",   "EGL_NATIVE_RENDERABLE",     "EGL_NATIVE_VISUAL_ID",        "EGL_NATIVE_VISUAL_TYPE",
  "EGL_PRESERVED_RESOURCES", "EGL_SAMPLES",               "EGL_SAMPLE_BUFFERS",          "EGL_SURFACE_TYPE",
  "EGL_TRANSPARENT_TYPE",    "EGL_TRANSPARENT_RED_VALUE", "EGL_TRANSPARENT_GREEN_VALUE", "EGL_TRANSPARENT_BLUE_VALUE",
  "EGL_BIND_TO_TEXTURE_RGB", "EGL_BIND_TO_TEXTURE_RGBA",  "EGL_MIN_SWAP_INTERVAL",       "EGL_MAX_SWAP_INTERVAL",
  "EGL_LUMINANCE_SIZE",      "EGL_ALPHA_MASK_SIZE",       "EGL_COLOR_BUFFER_TYPE",       "EGL_RENDERABLE_TYPE",
  "EGL_CONFORMANT"
};

static void printConfig(EGLDisplay display, EGLConfig config) __attribute__ ((unused));
static void printConfig(EGLDisplay display, EGLConfig config) {
   pf::Log log(__FUNCTION__);
   int value[1];

   log.branch();

   for (int i=0; i < std::ssize(attributes); i++) {
      int attribute = attributes[i];
      CSTRING name = names[i];
      if (eglGetConfigAttrib(display, config, attribute, value)) {
         log.msg("%d: %s: %d", i, name, value[0]);
      }
      else {
         while (eglGetError() != EGL_SUCCESS);
      }
   }
}

#endif

//********************************************************************************************************************

static void update_displayinfo(extDisplay *Self)
{
   if (!iequals("SystemDisplay", Self->Name)) return;

   glDisplayInfo.DisplayID = 0;
   get_display_info(Self->UID, &glDisplayInfo, sizeof(DISPLAYINFO));
}

//********************************************************************************************************************

void resize_feedback(FUNCTION *Feedback, OBJECTID DisplayID, int X, int Y, int Width, int Height)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%dx%d, %dx%d", X, Y, Width, Height);

   if (Feedback->isC()) {
      auto routine = (ERR (*)(OBJECTID, int, int, int, int, APTR))Feedback->Routine;
      pf::SwitchContext ctx(Feedback->Context);
      routine(DisplayID, X, Y, Width, Height, Feedback->Meta);
   }
   else if (Feedback->isScript()) {
      sc::Call(*Feedback, std::to_array<ScriptArg>({
         { "Display", DisplayID, FD_OBJECTID },
         { "X",       X },
         { "Y",       Y },
         { "Width",   Width },
         { "Height",  Height }
      }));
   }
}

//********************************************************************************************************************

static void notify_resize_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extDisplay *)CurrentContext())->ResizeFeedback.clear();
}

/*********************************************************************************************************************
-ACTION-
Activate: Activating a display has the same effect as calling the Show action.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Activate(extDisplay *Self)
{
   return acShow(Self);
}

/*********************************************************************************************************************
-METHOD-
CheckXWindow: Private. Checks that the Display dimensions match the X11 window dimensions.

Private

-END-
*********************************************************************************************************************/

static ERR DISPLAY_CheckXWindow(extDisplay *Self)
{
#ifdef __xwindows__

   Window childwin;
   int absx, absy;

   XTranslateCoordinates(XDisplay, Self->XWindowHandle, DefaultRootWindow(XDisplay), 0, 0, &absx, &absy, &childwin);

   if ((Self->X != absx) or (Self->Y != absy)) {
      pf::Log log;
      log.msg("Repairing coordinates, pos is %dx%d, was %dx%d", absx, absy, Self->X, Self->Y);

      Self->X = absx;
      Self->Y = absy;

      resize_feedback(&Self->ResizeFeedback, Self->UID, absx, absy, Self->Width, Self->Height);
   }

#endif
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Clears a display's image data and hardware buffers (e.g. OpenGL)
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Clear(extDisplay *Self)
{
#ifdef _GLES_
   if (!lock_graphics_active(__func__)) {
      glClearColorx(Self->Bitmap->BkgdRGB.Red, Self->Bitmap->BkgdRGB.Green, Self->Bitmap->BkgdRGB.Blue, 255);
      glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
      unlock_graphics();
      return ERR::Okay;
   }
   else return ERR::LockFailed;
#else
   return acClear(Self->Bitmap);
#endif
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Declared for internal purposes - do not call.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_DataFeed(extDisplay *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

#ifdef _WIN32
   if (Args->Datatype IS DATA::REQUEST) {
      // Supported for handling the windows clipboard

      auto request = (struct dcRequest *)Args->Buffer;

      log.traceBranch("Received data request from object %d, item %d", Args->Object ? Args->Object->UID : 0, request->Item);

      #ifdef WIN_DRAGDROP
      struct WinDT *data;
      int total_items;
      if (!winGetData(request->Preference, &data, &total_items)) {
         std::ostringstream xml;
         xml << "<receipt totalitems=\"" << total_items << "\" id=\"" << request->Item << "\">";
         for (int i=0; i < total_items; i++) {
            if (DATA(data[i].Datatype) IS DATA::FILE) {
               xml << "<file path=\"" << (CSTRING)data[i].Data << "\"/>";
            }
            else if (DATA(data[i].Datatype) IS DATA::TEXT) {
               xml << "<text>" << (CSTRING)data[i].Data << "</text>";
            }
            //else TODO: other types like images need their data saved to disk and referenced as a path, e.g. <image path="clipboard:abc.001"/>
         }
         xml << "</receipt>";

         struct acDataFeed dc;
         auto result = xml.str();
         dc.Object   = Self;
         dc.Datatype = DATA::RECEIPT;
         dc.Buffer   = pf::strclone(result);
         dc.Size     = result.size() + 1;
         Action(AC::DataFeed, Args->Object, &dc);
      }
      else return log.warning(ERR::NoSupport);
      #endif
   }
#endif

   return log.warning(ERR::NoSupport);
}

/*********************************************************************************************************************

-ACTION-
Disable: Disables the display (goes into power saving mode).

Disabling a display will put the display into power saving mode.  The DPMS mode is determined by the user's system
settings and cannot be changed by the developer.  The display will remain off until the Enable action is called.

This action does nothing if the display is in hosted mode.

-ERRORS-
Okay: The display was disabled.
NoSupport: The display driver does not support DPMS.
-END-

*********************************************************************************************************************/

static ERR DISPLAY_Disable(extDisplay *Self)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************
-ACTION-
Enable: Restores the screen display from power saving mode.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Enable(extDisplay *Self)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************
// On hosted systems like Android, the system may call Draw() on a display as a means of informing a program that a
// redraw is required.  It is the responsibility of the program that created the Display object to subscribe to the
// Draw action and act on it.

static ERR DISPLAY_Draw(extDisplay *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Flush: Flush pending graphics operations to the display.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Flush(extDisplay *Self)
{
#ifdef __xwindows__
   XSync(XDisplay, False);
#elif _GLES_
   if (!lock_graphics_active(__func__)) {
      glFlush();
      unlock_graphics();
   }
#endif
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLAY_Focus(extDisplay *Self)
{
   pf::Log log;

   log.traceBranch();
#ifdef _WIN32
   winFocus(Self->WindowHandle);
#elif __xwindows__
   if ((Self->Flags & SCR::BORDERLESS) != SCR::NIL) XSetInputFocus(XDisplay, Self->XWindowHandle, RevertToNone, CurrentTime);
#endif
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLAY_Free(extDisplay *Self)
{
   pf::Log log;

   if ((Self->Flags & SCR::AUTO_SAVE) != SCR::NIL) {
      log.trace("Autosave enabled.");
      acSaveSettings(Self);
   }
   else log.trace("Autosave disabled.");

#ifdef __xwindows__
   XEvent xevent;

   if (Self->WindowHandle IS (APTR)glDisplayWindow) glDisplayWindow = 0;

   if (Self->XPixmap) {
      XFreePixmap(XDisplay, Self->XPixmap);
      Self->XPixmap = 0;
      ((extBitmap *)Self->Bitmap)->x11.drawable = 0;
   }

   // Kill all expose events associated with the X Window owned by the display

   if (XDisplay) {
      while (XCheckWindowEvent(XDisplay, Self->XWindowHandle,
         ExposureMask|FocusChangeMask|StructureNotifyMask, &xevent) IS True);

      if ((Self->Flags & SCR::CUSTOM_WINDOW) IS SCR::NIL) {
         if (Self->WindowHandle) {
            XDestroyWindow(XDisplay, Self->XWindowHandle);
            Self->WindowHandle = nullptr;
         }
      }
   }

   XSync(XDisplay, False);
#endif

#ifdef _WIN32
   if ((Self->Flags & SCR::CUSTOM_WINDOW) IS SCR::NIL) {
      if (Self->WindowHandle) {
         winDestroyWindow(Self->WindowHandle);
         Self->WindowHandle = nullptr;
      }
   }
#endif

#ifdef _GLES_
   glActiveDisplayID = 0;
#endif

   acHide(Self);  // Hide the display.  In OpenGL this will remove the display resources.

   // Free the display's bitmap buffer

   if (Self->BufferID) { FreeResource(Self->BufferID); Self->BufferID = 0; }

   // Free the display's video bitmap

   if (Self->Bitmap) { FreeResource(Self->Bitmap); Self->Bitmap = nullptr; }

   Self->~extDisplay();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Retrieve formatted information from the display.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_GetKey(extDisplay *Self, struct acGetKey *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Key) or (!Args->Value)) return log.warning(ERR::NullArgs);
   if (Args->Size < 1) return log.warning(ERR::Args);

   if (pf::startswith("resolution(", Args->Key)) {
      // Field is in the format:  Resolution(Index, Format) Where 'Format' contains % symbols to indicate variable references.

      CSTRING str = Args->Key + 11;
      int index = strtol(str, nullptr, 0);
      while ((*str) and (*str != ')') and (*str != ',')) str++;
      if (*str IS ',') str++;
      while ((*str) and (*str <= 0x20)) str++;

      if (Self->Resolutions.empty()) get_resolutions(Self);

      if (!Self->Resolutions.empty()) {
         if (index >= std::ssize(Self->Resolutions)) return ERR::OutOfRange;

         std::ostringstream out;
         while ((*str) and (*str != ')')) {
            if (*str != '%') out << *str++;
            else if (str[1] IS '%') { // Escape?
               out << '%';
               str += 2;
            }
            else {
               switch (str[1]) {
                  case 'w': out << Self->Resolutions[index].width; break;
                  case 'h': out << Self->Resolutions[index].height; break;
                  case 'd': out << Self->Resolutions[index].bpp; break;
                  case 'c': if (Self->Resolutions[index].bpp <= 24) out << (1<<Self->Resolutions[index].bpp);
                            else out << (1<<24);
                            break;
               }
               str += 2;
            }
         }
         pf::strcopy(out.str(), Args->Value, Args->Size);

         return ERR::Okay;
      }
      else return ERR::NoData;
   }
   else return ERR::NoSupport;
}

/*********************************************************************************************************************
-ACTION-
Hide: Hides a display from the user's view.

Calling this action will hide a display from the user's view.  If the hidden display was at the front of the display
and there is a display object behind it, then the next underlying display will be displayed.  If there are no other
displays available then the user's viewport will be blank after calling this action.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Hide(extDisplay *Self)
{
   pf::Log log;

   log.branch();

#ifdef _WIN32
   winHideWindow(Self->WindowHandle);
#elif __xwindows__
   if ((XDisplay) and (Self->XWindowHandle)) {
      XUnmapWindow(XDisplay, Self->XWindowHandle);
      XSync(XDisplay, False);
   }
#elif __snap__
   // If the system is shutting down, don't touch the display.  This makes things look tidier when the system shuts down.

   int state = GetResource(RES::SYSTEM_STATE);
   if ((state IS STATE_SHUTDOWN) or (state IS STATE_RESTART)) {
      log.msg("Not doing anything because system is shutting down.");
   }
   else sciCloseVideoMode(Self->VideoHandle);

#elif _GLES_
   if ((Self->Flags & SCR::VISIBLE) != SCR::NIL) {
      adHideDisplay(Self->UID);
   }
#endif

   Self->Flags &= ~SCR::VISIBLE;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLAY_Init(extDisplay *Self)
{
   pf::Log log;

   #ifdef __xwindows__
      // Figure out how many bits and bytes are used per pixel on this XDisplay

      auto xbpp = DefaultDepth(XDisplay, DefaultScreen(XDisplay));

      if (xbpp <= 8) {
         log.msg(VLF::CRITICAL, "Please change your X11 setup so that it runs in 15 bit mode or better.");
         log.msg(VLF::CRITICAL, "Currently X11 is configured to use %d bit graphics.", xbpp);
         return ERR::NoSupport;
      }

      int xbytes;
      if (xbpp <= 8) xbytes = 1;
      else if (xbpp <= 16) xbytes = 2;
      else if (xbpp <= 24) xbytes = 3;
      else xbytes = 4;

      int count;
      if (auto list = XListPixmapFormats(XDisplay, &count)) {
         for (int i=0; i < count; i++) {
            if (list[i].depth IS xbpp) {
               xbytes = list[i].bits_per_pixel;
               if (list[i].bits_per_pixel <= 8) xbytes = 1;
               else if (list[i].bits_per_pixel <= 16) xbytes = 2;
               else if (list[i].bits_per_pixel <= 24) xbytes = 3;
               else xbytes = 4;
            }
         }
         XFree(list);
      }

      if ((xbpp IS 24) and (xbytes IS 3)) {
         static bool bpp_warning = false;
         if (!bpp_warning) {
            bpp_warning = true;
            log.warning("Running in 32bpp instead of 24bpp is strongly recommended.");
         }
      }

      #ifdef XRANDR_ENABLED
      if (glXRRAvailable) {
         // Set the refresh rate to zero to indicate that we have some control of the display (the default is -1 if there is no control).
         Self->RefreshRate = 0;
      }
      #endif
   #endif

   // Set defaults

   auto bmp = (extBitmap *)Self->Bitmap;

   DISPLAYINFO info;
   if (get_display_info(0, &info, sizeof(info)) != ERR::Okay) return log.warning(ERR::SystemCall);

   if (!Self->Width) {
      Self->Width = info.Width;
      #ifdef _WIN32
         Self->Width -= 60;
      #endif
   }

   if (!Self->Height) {
      Self->Height = info.Height;
      #ifdef _WIN32
         Self->Height -= 80;
      #endif
   }

   if (Self->Width  < 4)  Self->Width  = 4;
   if (Self->Height < 4)  Self->Height = 4;

   if ((info.Flags & SCR::MAXSIZE) != SCR::NIL) {
      if (Self->Width > info.Width) {
         log.msg("Limiting requested width of %d to %d", Self->Width, info.Width);
         Self->Width = info.Width;
      }
      if (Self->Height > info.Height) {
         log.msg("Limiting requested height of %d to %d", Self->Height, info.Height);
         Self->Height = info.Height;
      }
   }
   else {
      if (Self->Width  > 4096) Self->Width  = 4096;
      if (Self->Height > 4096) Self->Height = 4096;
   }

   #ifdef __xwindows__
      // If the display object will act as window manager, the dimensions must match that of the root window.

      if ((glX11.Manager) or ((Self->Flags & SCR::MAXIMISE) != SCR::NIL)) {
         Self->Width  = glRootWindow.width;
         Self->Height = glRootWindow.height;
      }

      if (Self->Width > glRootWindow.width) Self->Width = glRootWindow.width;
      if (Self->Height > glRootWindow.height) Self->Height = glRootWindow.height;
   #endif

   if (bmp->Width  < Self->Width)  bmp->Width = Self->Width;
   if (bmp->Height < Self->Height) bmp->Height = Self->Height;

   // Fix up the bitmap dimensions

   if (!bmp->Width) bmp->Width  = Self->Width;
   else if (Self->Width > bmp->Width) bmp->Width  = Self->Width;

   if (!bmp->Height) bmp->Height = Self->Height;
   else if (Self->Height > bmp->Height) bmp->Height = Self->Height;

   bmp->Type = BMP::CHUNKY;

   #ifdef __xwindows__
      if (xbytes IS 4) bmp->BitsPerPixel = 32;
      else bmp->BitsPerPixel = xbpp;
      bmp->BytesPerPixel = xbytes;
   #elif _WIN32
      if ((Self->Flags & SCR::COMPOSITE) != SCR::NIL) {
         log.msg("Composite mode will force a 32-bit window area.");
         bmp->BitsPerPixel = 32;
         bmp->BytesPerPixel = 4;
      }
   #endif

   if (!bmp->BitsPerPixel) {
      bmp->BitsPerPixel = info.BitsPerPixel;
      bmp->BytesPerPixel = info.BytesPerPixel;
   }

   #ifdef __xwindows__

      bmp->Flags |= BMF::NO_DATA;
      bmp->DataFlags = MEM::VIDEO;

      // Set the Window Attributes structure

      XSetWindowAttributes swa;
      swa.bit_gravity = CenterGravity;
      swa.win_gravity = CenterGravity;
      swa.cursor      = C_Default;
      swa.override_redirect = (Self->Flags & (SCR::BORDERLESS|SCR::COMPOSITE)) != SCR::NIL;
      swa.event_mask  = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask
                        |KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask;

      if (!glX11.Manager) {
         // Window creation for running inside a foreign window manager.

         log.msg("Creating X11 window %dx%d,%dx%d, Override: %d, XDisplay: %p, Parent: %" PRId64, Self->X, Self->Y, Self->Width, Self->Height, swa.override_redirect, XDisplay, (int64_t)Self->XWindowHandle);

         int cwflags   = CWEventMask|CWOverrideRedirect;
         int depth     = CopyFromParent;
         Visual *visual = CopyFromParent;
         if ((swa.override_redirect) and (glXCompositeSupported)) {
            swa.colormap         = XCreateColormap(XDisplay, DefaultRootWindow(XDisplay), glXInfoAlpha.visual, AllocNone);
            swa.background_pixel = 0;
            swa.border_pixel     = 0;
            cwflags |= CWColormap|CWBackPixel|CWBorderPixel;
            visual   = glXInfoAlpha.visual;
            depth    = glXInfoAlpha.depth;
            bmp->Flags |= BMF::ALPHA_CHANNEL|BMF::FIXED_DEPTH;
            bmp->BitsPerPixel  = 32;
            bmp->BytesPerPixel = 4;
            xbpp = 32;
         }

         if (!Self->XWindowHandle) {
            if (!(Self->XWindowHandle = XCreateWindow(XDisplay, DefaultRootWindow(XDisplay),
                  Self->X, Self->Y, Self->Width, Self->Height, 0 /* Border */, depth, InputOutput,
                  visual, cwflags, &swa))) {
               return log.warning(ERR::SystemCall);
            }
         }
         else { // If the WindowHandle field is already set, use it as the parent for the new window.
            if (!(Self->XWindowHandle = XCreateWindow(XDisplay, Self->XWindowHandle,
                  0, 0, Self->Width, Self->Height, 0, depth, InputOutput, visual, cwflags, &swa))) {
               return log.warning(ERR::SystemCall);
            }
         }

         bmp->x11.window = Self->XWindowHandle;

         if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) {
            // For composite windows, we can draw directly to the Window handle
            bmp->x11.drawable = Self->XWindowHandle;
         }
         else {
            // Create a pixmap buffer and associate it with the window by setting it as the background.

            // Although creating a pixmap with the same size as the display is a little excessive, it produces
            // the best user experience when resizing windows
            bmp->x11.pix_width  = info.Width;  //Self->Width;
            bmp->x11.pix_height = info.Height; //Self->Height;
            if (!(Self->XPixmap = XCreatePixmap(XDisplay, Self->XWindowHandle, bmp->x11.pix_width, bmp->x11.pix_height, xbpp))) {
               return log.warning(ERR::SystemCall);
            }

            // Blanking the pixmap reduces visible glitches caused by window resizing.
            if (auto gc = XCreateGC(XDisplay, Self->XPixmap, 0, 0)) {
               XSetFunction(XDisplay, gc, GXcopy);
               if ((swa.override_redirect) and (glXCompositeSupported)) {
                  XSetForeground(XDisplay, gc, 0x000000);
               }
               else XSetForeground(XDisplay, gc, 0xd0d0d0);
               XFillRectangle(XDisplay, Self->XPixmap, gc, 0, 0, info.Width, info.Height);
               XFreeGC(XDisplay, gc);
            }

            XSetWindowBackgroundPixmap(XDisplay, Self->XWindowHandle, Self->XPixmap);

            bmp->x11.drawable = Self->XPixmap;
         }

         CSTRING name;
         if ((CurrentTask()->get(FID_Name, name) IS ERR::Okay) and (name)) {
            XStoreName(XDisplay, Self->XWindowHandle, name);
         }
         else XStoreName(XDisplay, Self->XWindowHandle, "Kotuku");

         Atom protocols[1] = { XWADeleteWindow };
         XSetWMProtocols(XDisplay, Self->XWindowHandle, protocols, std::ssize(protocols));

         Self->Flags |= SCR::HOSTED;

         bmp->Width  = Self->Width;
         bmp->Height = Self->Height;

         if (swa.override_redirect) { // Composite windows require a dedicated GC for drawing
            XGCValues gcv = { .function = GXcopy, .graphics_exposures = False };
            bmp->x11.gc = XCreateGC(XDisplay, Self->XWindowHandle, GCGraphicsExposures|GCFunction, &gcv);
         }

         if (glStickToFront) {
            // KDE doesn't honour this request, not sure how many window managers would but it's worth a go.

            XSetTransientForHint(XDisplay, Self->XWindowHandle, DefaultRootWindow(XDisplay));
         }

         // Indicate that the window position is not to be meddled with by the window manager.

         XSizeHints hints = { .flags = USPosition|USSize };
         XSetWMNormalHints(XDisplay, Self->XWindowHandle, &hints);

         if (InitObject(bmp) != ERR::Okay) return log.warning(ERR::Init);
      }
      else { // If we are the window manager, set up the root window as our display.
         if (!Self->WindowHandle) Self->XWindowHandle = DefaultRootWindow(XDisplay);
         bmp->set(FID_Handle, (APTR)Self->XWindowHandle);
         XChangeWindowAttributes(XDisplay, Self->XWindowHandle, CWEventMask|CWCursor, &swa);

         #ifdef XRANDR_ENABLED
         if (glXRRAvailable) XRRSelectInput(XDisplay, DefaultRootWindow(XDisplay), RRScreenChangeNotifyMask);
         #endif

         XWindowAttributes winattrib;
         XGetWindowAttributes(XDisplay, Self->XWindowHandle, &winattrib);
         Self->Width  = winattrib.width;
         Self->Height = winattrib.height;
         bmp->Width   = Self->Width;
         bmp->Height  = Self->Height;

         if (InitObject(bmp) != ERR::Okay) return log.warning(ERR::Init);

         if (glDGAAvailable) {
            bmp->Flags |= BMF::X11_DGA;
            bmp->Data = (uint8_t *)glDGAVideo;
         }
      }

      glDisplayWindow = Self->XWindowHandle;

      XChangeProperty(XDisplay, Self->XWindowHandle, atomSurfaceID, atomSurfaceID, 32, PropModeReplace, (uint8_t *)&Self->UID, 1);

   #elif _WIN32

      // Initialise the Bitmap.  We will set the Bitmap->Data field later on.  The Drawable field
      // in the Bitmap object will also be pointed to the window that we have created, but this
      // will be managed by the Surface class.

      bmp->Flags |= BMF::NO_DATA;
      bmp->DataFlags = MEM::VIDEO;

      if (InitObject(bmp) != ERR::Okay) {
         return log.warning(ERR::Init);
      }

      if (!Self->WindowHandle) {
         bool desktop = false;
         if ((Self->Flags & SCR::COMPOSITE) != SCR::NIL) {
            // Not a desktop
         }
         else {
            OBJECTID surface_id;
            if (FindObject("SystemSurface", CLASSID::SURFACE, FOF::NIL, &surface_id) IS ERR::Okay) {
               if (surface_id IS Self->ownerID()) desktop = true;
            }
         }

         CSTRING name = nullptr;
         CurrentTask()->get(FID_Name, name);
         HWND popover = 0;
         if (Self->PopOverID) {
            if (ScopedObjectLock<extDisplay> other_display(Self->PopOverID, 3000); other_display.granted()) {
               popover = other_display->WindowHandle;
            }
            else log.warning(ERR::AccessObject);
         }

         if (!(Self->WindowHandle = (APTR)winCreateScreen(popover, &Self->X, &Self->Y, &Self->Width, &Self->Height,
               ((Self->Flags & SCR::MAXIMISE) != SCR::NIL) ? 1 : 0, ((Self->Flags & SCR::BORDERLESS) != SCR::NIL) ? 1 : 0, name,
               ((Self->Flags & SCR::COMPOSITE) != SCR::NIL) ? 1 : 0, Self->Opacity, desktop))) {
            return log.warning(ERR::SystemCall);
         }
      }
      else {
         // If we have been passed a foreign window handle, we need to set the procedure for it so that we can process
         // window related messages.

         if (!(Self->WindowHandle = (APTR)winCreateChild(Self->WindowHandle, Self->X, Self->Y, Self->Width, Self->Height))) {
            return log.warning(ERR::SystemCall);
         }
      }

      Self->Flags |= SCR::HOSTED;

      // Get the size of the host window frame.  Note that the winCreateScreen() function we called earlier
      // would have already reset the X/Y fields so that they reflect the absolute client position of the window.

      winGetMargins(Self->WindowHandle, &Self->LeftMargin, &Self->TopMargin, &Self->RightMargin, &Self->BottomMargin);

   #elif _GLES_
      ERR error;

      if (Self->Bitmap->BitsPerPixel) glEGLPreferredDepth = Self->Bitmap->BitsPerPixel;
      else glEGLPreferredDepth = 0;

      if (!pthread_mutex_lock(&glGraphicsMutex)) {
         error = init_egl();
         eglMakeCurrent(glEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); // Give up our access to EGL because we're releasing the graphics mutex.
         pthread_mutex_unlock(&glGraphicsMutex);
      }
      if (error) return error;

      refresh_display_from_egl(Self);

      // Initialise the video bitmap that will represent the OpenGL surface

      bmp->Flags |= BMF::NO_DATA;
      bmp->DataFlags = MEM::VIDEO;
      if (InitObject(bmp) != ERR::Okay) {
         return log.warning(ERR::Init);
      }

   #else
      return log.warning(ERR::NoSupport);
   #endif

   if ((Self->Flags & SCR::BUFFER) != SCR::NIL) alloc_display_buffer(Self);

   Self->updatePalette(bmp->Palette);

   // Take a record of the pixel format for GetDisplayInfo()

   copymem(bmp->ColourFormat, &glColourFormat, sizeof(glColourFormat));

   if (glSixBitDisplay) Self->Flags |= SCR::BIT_6;

   update_displayinfo(Self); // Update the glDisplayInfo cache.

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Minimise: Minimise the desktop window hosting the display.

If a display is hosted in a desktop window, calling the Minimise method will perform the default minimise action
on that window.  On a platform such as Microsoft Windows, this would normally result in the window being
minimised to the task bar.

Calling Minimise on a display that is already in the minimised state may result in the host window being restored to
the desktop.  This behaviour is platform dependent and should be manually tested to confirm its reliability on the
host platform.

-ERRORS-
Okay
-END-

*********************************************************************************************************************/

static ERR DISPLAY_Minimise(extDisplay *Self)
{
   pf::Log log;
   log.branch();
#ifdef _WIN32
   winMinimiseWindow(Self->WindowHandle);
#elif __xwindows__
   if (XDisplay) {
      XUnmapWindow(XDisplay, Self->XWindowHandle);
      XSync(XDisplay, False);
   }
#endif
   return ERR::Okay;
}

/*********************************************************************************************************************

Bitmap moving should be supported by listening to the Bitmap's Move() action
and responding to it.

MoveBitmap(): Moves a display's bitmap to specified X/Y values.

This routine has two uses: Moving the Bitmap to any position on the display, and for Hardware Scrolling.  It takes the
BmpX and BmpY arguments and uses them to set the new Bitmap position. This method will execute at the same speed for
all offset values.

You must have set the HSCROLL flag for horizontal scrolling and the VSCROLL flag for vertical scrolling if you wish to
use this method.  If you try and move the Bitmap without setting at least one of these flags, the method will fail
immediately.

If you want to perform hardware scrolling suitable for games that need to scroll in any direction, initialise a display
that has a bitmap of twice the size of the display. You can then scroll around in this area and create an infinite
scrolling map.  Because today's game programs typically run in high resolution true colour displays, be aware that the
host graphics card may need a large amount of memory to support this method of scrolling.

*********************************************************************************************************************/

/*********************************************************************************************************************
-ACTION-
Move: Move the display to a new display position (relative coordinates).
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Move(extDisplay *Self, struct acMove *Args)
{
   pf::Log log;

   if (!Args) return ERR::NullArgs;

   //log.branch("Moving display by %dx%d", (LONG)Args->DeltaX, (LONG)Args->DeltaY);

#ifdef _WIN32

   if (!winMoveWindow(Self->WindowHandle,
      Self->X + Self->LeftMargin + Args->DeltaX,
      Self->Y + Self->TopMargin + Args->DeltaY)) return ERR::SystemCall;

   return ERR::Okay;

#elif __xwindows__

   // Handling margins isn't necessary as the window manager will take that into account when it receives the move request.

   if (!XDisplay) return ERR::NoSupport;

   XMoveWindow(XDisplay, Self->XWindowHandle, Self->X + Args->DeltaX, Self->Y + Args->DeltaY);
   return ERR::Okay;

#elif __snap__

   Self->X += Args->DeltaX;
   Self->Y += Args->DeltaY;
   return ERR::Okay;

#else

   return ERR::NoSupport;

#endif

}

/*********************************************************************************************************************
-ACTION-
MoveToBack: Move the display to the back of the display list.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_MoveToBack(extDisplay *Self)
{
   pf::Log log;
   log.branch("%s", Self->Name);

#ifdef _WIN32
   winMoveToBack(Self->WindowHandle);
#elif __xwindows__
   if (XDisplay) XLowerWindow(XDisplay, Self->XWindowHandle);
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToFront: Move the display to the front of the display list.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_MoveToFront(extDisplay *Self)
{
   pf::Log log;
   log.branch("%s", Self->Name);
#ifdef _WIN32
   winMoveToFront(Self->WindowHandle);
#elif __xwindows__
   if (XDisplay) {
      XRaiseWindow(XDisplay, Self->XWindowHandle);
      XSync(XDisplay, False);
   }
#endif
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Move the display to a new position.

The MoveToPoint action moves the display to a new position.

In a hosted environment, the supplied coordinates are treated as being indicative of the absolute position of the host
window (not the client area).

For full-screen displays, MoveToPoint can alter the screen position for the hardware device managing the display
output.  This is a rare feature that requires hardware support.  `ERR::NoSupport` is returned if this feature is
unavailable.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_MoveToPoint(extDisplay *Self, struct acMoveToPoint *Args)
{
   pf::Log log;

   if (!Args) return ERR::NullArgs;

   log.traceBranch("Moving display to %dx%d", int(Args->X), int(Args->Y));

#ifdef _WIN32

   // winMoveWindow() treats the coordinates as being indicative of the client area.

   if (!winMoveWindow(Self->WindowHandle,
         ((Args->Flags & MTF::X) != MTF::NIL) ? Args->X : int(Self->X) + Self->LeftMargin,
         ((Args->Flags & MTF::Y) != MTF::NIL) ? Args->Y : int(Self->Y) + Self->TopMargin)) return ERR::SystemCall;

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->X = int(Args->X) + Self->LeftMargin;
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->Y = int(Args->Y) + Self->TopMargin;
   return ERR::Okay;

#elif __xwindows__

   // Handling margins isn't necessary as the window manager will take that into account when it receives the move request.

   XMoveWindow(XDisplay, Self->XWindowHandle,
      ((Args->Flags & MTF::X) != MTF::NIL) ? int(Args->X) : Self->X,
      ((Args->Flags & MTF::Y) != MTF::NIL) ? int(Args->Y) : Self->Y);

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->X = int(Args->X);
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->Y = int(Args->Y);
   return ERR::Okay;

#else

   return ERR::NoSupport;

#endif
}

//********************************************************************************************************************

static ERR DISPLAY_NewObject(extDisplay *Self)
{
   if (NewLocalObject(CLASSID::BITMAP, &Self->Bitmap) != ERR::Okay) return ERR::NewObject;

   OBJECTID id;
   if (FindObject("SystemVideo", CLASSID::NIL, FOF::NIL, &id) != ERR::Okay) SetName(Self->Bitmap, "SystemVideo");

   if (!Self->Name[0]) {
      if (FindObject("SystemDisplay", CLASSID::NIL, FOF::NIL, &id) != ERR::Okay) SetName(Self, "SystemDisplay");
   }

   #ifdef __xwindows__

      strcopy("X11", Self->Chipset, sizeof(Self->Chipset));
      strcopy("X Windows", Self->Display, sizeof(Self->Display));
      strcopy("N/A", Self->DisplayManufacturer, sizeof(Self->DisplayManufacturer));
      strcopy("N/A", Self->Manufacturer, sizeof(Self->Manufacturer));

   #elif _WIN32

      strcopy("Windows", Self->Chipset, sizeof(Self->Chipset));
      strcopy("Windows", Self->Display, sizeof(Self->Display));
      strcopy("N/A", Self->DisplayManufacturer, sizeof(Self->DisplayManufacturer));
      strcopy("N/A", Self->Manufacturer, sizeof(Self->Manufacturer));

   #elif _GLES_

      strcopy("OpenGLES", Self->Chipset, sizeof(Self->Chipset));
      strcopy("OpenGL", Self->Display, sizeof(Self->Display));
      strcopy("N/A", Self->DisplayManufacturer, sizeof(Self->DisplayManufacturer));
      strcopy("N/A", Self->Manufacturer, sizeof(Self->Manufacturer));

   #else

      strcopy("Unknown", Self->Chipset, sizeof(Self->Chipset));
      strcopy("Unknown", Self->Display, sizeof(Self->Display));
      strcopy("Unknown", Self->DisplayManufacturer, sizeof(Self->DisplayManufacturer));
      strcopy("Unknown", Self->Manufacturer, sizeof(Self->Manufacturer));

   #endif

   Self->Width       = 800;
   Self->Height      = 600;
   Self->RefreshRate = -1;
   Self->Gamma[0]    = 1.0;
   Self->Gamma[1]    = 1.0;
   Self->Gamma[2]    = 1.0;
   Self->Opacity     = 255;

   #ifdef __xwindows__
      Self->DisplayType = DT::X11;
   #elif _WIN32
      Self->DisplayType = DT::WINGDI;
   #elif _GLES_
      Self->DisplayType = DT::GLES;
   #else
      Self->DisplayType = DT::NATIVE;
   #endif

   return ERR::Okay;
}

static ERR DISPLAY_NewPlacement(extDisplay *Self)
{
   new (Self) extDisplay;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redimension: Moves and resizes a display object in a single action call.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Redimension(extDisplay *Self, struct acRedimension *Args)
{
   if (!Args) return ERR::NullArgs;

   struct acMoveToPoint moveto = { Args->X, Args->Y, 0, MTF::X|MTF::Y };
   DISPLAY_MoveToPoint(Self, &moveto);

   struct acResize resize = { Args->Width, Args->Height, Args->Depth };
   DISPLAY_Resize(Self, &resize);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Resizes the dimensions of a display object.

If the display is hosted, the Width and Height values will determine the size of the inside area of the window.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_Resize(extDisplay *Self, struct acResize *Args)
{
   pf::Log log;

   log.branch();

   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

#ifdef _WIN32

   if (!Args) return log.warning(ERR::NullArgs);

   if (!winResizeWindow(Self->WindowHandle, 0x7fffffff, 0x7fffffff, Args->Width, Args->Height)) {
      return ERR::Resize;
   }

   Action(AC::Resize, Self->Bitmap, Args);
   Self->Width = Self->Bitmap->Width;
   Self->Height = Self->Bitmap->Height;

#elif __xwindows__

   if (!Args) return log.warning(ERR::NullArgs);

   if (XDisplay) {
      resize_pixmap(Self, Args->Width, Args->Height);
      XResizeWindow(XDisplay, Self->XWindowHandle, Args->Width, Args->Height);
   }

   Action(AC::Resize, Self->Bitmap, Args);
   Self->Width = Self->Bitmap->Width;
   Self->Height = Self->Bitmap->Height;

#elif __snap__

   // Scan the available display modes and choose the one that most closely matches the requested display dimensions.

   if (!(width = Args->Width)) width = Self->Width;
   if (!(height = Args->Height)) height = Self->Height;

   uint16_t *modes = glSNAPDevice->AvailableModes;
   if (glSNAP->Init.GetDisplayOutput) display = glSNAP->Init.GetDisplayOutput() & gaOUTPUT_SELECTMASK;
   else display = gaOUTPUT_CRT;
   gfxmode = -1;
   bestweight = 0x7fffffff;
   for (i=0; modes[i] != 0xffff; i++) {
      modeinfo.dwSize = sizeof(modeinfo);
      if (!glSNAP->Init.GetVideoModeInfoExt(modes[i], &modeinfo, display, nullptr)) {
         if (modeinfo.AttributesExt & gaIsPanningMode) continue;
         if (modeinfo.Attributes & gaIsTextMode) continue;

         if (modeinfo.BitsPerPixel IS glSNAP->VideoMode.BitsPerPixel) {
            weight = std::abs(modeinfo.XResolution - width) + std::abs(modeinfo.YResolution - height);

            if (weight < bestweight) {
               gfxmode = modes[i];
               bestweight = weight;
            }
         }
      }
   }

   // Broadcast the change in resolution so that all video buffered bitmaps can move their graphics out of video memory.

   evResolutionChange ev = { EVID_DISPLAY_RESOLUTION_CHANGE };
   BroadcastEvent(&ev, sizeof(ev));

   log.msg("Opening display mode: %dx%d", width, height);

   vx = -1;
   vy = -1;
   bytesperline = -1;
   if (sciOpenVideoMode(gfxmode, &modeinfo, &vx, &vy, &bytesperline, &Self->VideoHandle, 0) != ERR::Okay) {
      log.warning("Failed to set the requested video mode.");
      return ERR::NoSupport;
   }

   Self->GfxMode = gfxmode;
   Self->Width  = modeinfo.XResolution;
   Self->Height = modeinfo.YResolution;
   Self->RefreshRate = (glSNAP->Init.GetCurrentRefreshRate() + 50) / 100;

   acResize(Self->Bitmap, Self->Width, Self->Height, 0);

#endif

   // If a display buffer is in use, reallocate it from scratch.

   if ((Self->Flags & SCR::BUFFER) != SCR::NIL) alloc_display_buffer(Self);

   update_displayinfo(Self);

   Self->HDensity = 0; // DPI needs to be recalculated.
   Self->VDensity = 0;

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the image of a display to a data object.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_SaveImage(extDisplay *Self, struct acSaveImage *Args)
{
   return Action(AC::SaveImage, Self->Bitmap, Args);
}

/*********************************************************************************************************************
-ACTION-
SaveSettings: Saves the current display settings as the default.
-END-
*********************************************************************************************************************/

static ERR DISPLAY_SaveSettings(extDisplay *Self)
{
   pf::Log log;

#ifdef __xwindows__

   log.branch();

   objConfig::create config = { fl::Path("user:config/display.cfg") };

   if (config.ok()) {
      if ((Self->Flags & SCR::BORDERLESS) IS SCR::NIL) {
         config->write("DISPLAY", "WindowX", std::to_string(Self->X));
         config->write("DISPLAY", "WindowY", std::to_string(Self->Y));

         if (Self->Width >= 600) config->write("DISPLAY", "WindowWidth", std::to_string(Self->Width));
         else config->write("DISPLAY", "WindowWidth", "600");

         if (Self->Height >= 480) config->write("DISPLAY", "WindowHeight", std::to_string(Self->Height));
         else config->write("DISPLAY", "WindowHeight", "480");
      }

      config->write("DISPLAY", "DPMS", dpms_name(Self->PowerMode));
      config->write("DISPLAY", "FullScreen", ((Self->Flags & SCR::BORDERLESS) != SCR::NIL) ? "1" : "0");

      config->saveSettings();
   }

#elif _WIN32

   if ((Self->WindowHandle) and (Self->Width >= 640) and (Self->Height > 480)) {
      // Save the current window status to file, but only if it is large enough to be considered 'screen sized'.

      objConfig::create config = { fl::Path("user:config/display.cfg") };

      if (config.ok()) {
         int x, y, width, height, maximise;

         if (winGetWindowInfo(Self->WindowHandle, &x, &y, &width, &height, &maximise)) {
            config->write("DISPLAY", "WindowWidth", std::to_string(width));
            config->write("DISPLAY", "WindowHeight", std::to_string(height));
            config->write("DISPLAY", "WindowX", std::to_string(x));
            config->write("DISPLAY", "WindowY", std::to_string(y));
            config->write("DISPLAY", "Maximise", std::to_string(maximise));
            config->write("DISPLAY", "DPMS", dpms_name(Self->PowerMode));
            config->write("DISPLAY", "FullScreen", ((Self->Flags & SCR::BORDERLESS) != SCR::NIL) ? "1" : "0");
            acSaveSettings(*config);
         }
      }
      else return log.warning(ERR::CreateObject);
   }

#endif

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SizeHints: Sets the width and height restrictions for the host window (hosted environments only).

If a display is hosted in a desktop window, it may be possible to enforce size restrictions that prevent the window
from being shrunk or expanded beyond a certain size.  This feature is platform dependent and `ERR::NoSupport`
will be returned if it is not implemented.

-INPUT-
int MinWidth: The minimum width of the window.
int MinHeight: The minimum height of the window.
int MaxWidth: The maximum width of the window.
int MaxHeight: The maximum width of the window.
int EnforceAspect: Set to true to enforce an aspect ratio that is scaled from MinWidth,MinHeight to MaxWidth,MaxHeight.

-ERRORS-
Okay
NoSupport: The host platform does not support this feature.
-END-

*********************************************************************************************************************/

static ERR DISPLAY_SizeHints(extDisplay *Self, gfx::SizeHints *Args)
{
#ifdef __xwindows__
   XSizeHints hints = { .flags = 0 };

   if ((Args->MaxWidth > 0) and (Args->MaxHeight > 0)) {
      hints.max_width  = Args->MaxWidth;
      hints.max_height = Args->MaxHeight;
      hints.flags |= PMaxSize;
   }

   if ((Args->MinWidth > 0) and (Args->MinHeight > 0)) {
      hints.min_width  = Args->MinWidth;
      hints.min_height = Args->MinHeight;
      hints.flags |= PMinSize;
   }

   if ((Args->EnforceAspect) and (hints.flags & PMaxSize) and (hints.flags & PMinSize)) {
      hints.flags |= PAspect;
      hints.min_aspect.x = Args->MinWidth;
      hints.max_aspect.x = Args->MinWidth;
      hints.min_aspect.y = Args->MinHeight;
      hints.max_aspect.y = Args->MinHeight;
   }

   XSetWMNormalHints(XDisplay, Self->XWindowHandle, &hints);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
SetDisplay: Changes the current display mode.

The SetDisplay method changes the current display settings for the screen. It can alter the position and screen
dimensions and the display refresh rate. The new settings are applied immediately, although minor delays are possible
while the graphics card and monitor adjust to the changes.

To keep any of the display settings at their current value, set the appropriate parameters to zero to leave them
unchanged.  Only the parameters that you set will be used.

If the display parameters do not match with a valid display mode - for instance if you request a screen size of
1280x1024 and the nearest equivalent is 1024x768, the SetDisplay method will automatically adjust to match against the
nearest screen size.

Only the original owner of the display object is allowed to change the display settings.

-INPUT-
int X: Horizontal offset of the display, relative to its default position.
int Y: Vertical offset of the display, relative to its default position.
int Width: Width of the display.
int Height: Height of the display.
int InsideWidth: Internal display width (must be equal to or greater than the display width).
int InsideHeight: Internal display height (must be equal to or greater than the display height).
int BitsPerPixel: The desired display depth (15, 16, 24 or 32).
double RefreshRate: Refresh rate, measured in floating point format for precision.
int Flags: Optional flags.

-ERRORS-
Okay:
NullArgs:
Failed: Failed to switch to the requested display mode.
-END-

*********************************************************************************************************************/

static ERR DISPLAY_SetDisplay(extDisplay *Self, gfx::SetDisplay *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

#ifdef _WIN32
   // NOTE: Dimensions are measured relative to the client area, not the window including its borders.

   log.msg(VLF::BRANCH|VLF::DETAIL, "%dx%d, %dx%d", Args->X, Args->Y, Args->Width, Args->Height);

   if (!winResizeWindow(Self->WindowHandle, Args->X, Args->Y, Args->Width, Args->Height)) {
      return log.warning(ERR::Resize);
   }

   log.trace("Resizing the video bitmap.");

   acResize(Self->Bitmap, Args->Width, Args->Height, 0);
   Self->Width = Self->Bitmap->Width;
   Self->Height = Self->Bitmap->Height;

#elif __xwindows__
   // NOTE: Dimensions are measured relative to the client area, not the window.

   log.branch("%dx%d,%dx%d @ %.2fHz, %d bit", Args->X, Args->Y, Args->Width, Args->Height, Args->RefreshRate, Args->BitsPerPixel);

   if ((Args->Width IS Self->Width) and (Args->Height IS Self->Height)) return ERR::Okay;

   int width = Args->Width;
   int height = Args->Height;

   if (glX11.Manager) { // The video mode can only be changed with the XRandR extension
#ifdef XRANDR_ENABLED
      if ((glXRRAvailable) and (xr_set_display_mode(&width, &height) IS ERR::Okay)) {
         Self->RefreshRate = 0;
         Self->Width  = width;
         Self->Height = height;

         // x11SetDisplayMode() posts a request to the X server.  Our response to display mode changes can be found in handler.c.

         // Note: The RandR extension changes the video mode without actually changing the size of the bitmap area, so we don't resize the bitmap.

         return ERR::Okay;
      }
      else return ERR::NoSupport;
#endif
   }
   else {
      XResizeWindow(XDisplay, Self->XWindowHandle, width, height);
      acResize(Self->Bitmap, width, height, 0.0);
      Self->Width  = width;
      Self->Height = height;
   }

#elif __snap__

   // Broadcast the change in resolution so that all video buffered bitmaps can move their graphics out of video memory.

   evResolutionChange ev = { EVID_DISPLAY_RESOLUTION_CHANGE };
   BroadcastEvent(&ev, sizeof(ev));

#endif

   // If a display buffer is in use, reallocate it from scratch.  Note: A failure to allocate a display buffer is not
   // considered terminal.

   if ((Self->Flags & SCR::BUFFER) != SCR::NIL) alloc_display_buffer(Self);

   update_displayinfo(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetGamma: Sets the display gamma levels.

The SetGamma method controls the gamma correction levels for the display.  Gamma levels for the red, green and blue
colour components can be set at floating point precision.  The default gamma level for each component is 1.0; the
minimum value is 0.0 and the maximum value is 100.

Optional flags include `GMF::SAVE`.  This option will save the requested settings as the user default when future displays
are opened.

If you would like to know the default gamma correction settings for a display, please refer to the #Gamma
field.

-INPUT-
double Red:   Gamma correction for the red gun.
double Green: Gamma correction for the green gun.
double Blue:  Gamma correction for the blue gun.
int(GMF) Flags: Optional flags.

-ERRORS-
Okay
NullArgs
NoSupport: The graphics hardware does not support gamma correction.
-END-

*********************************************************************************************************************/

static ERR DISPLAY_SetGamma(extDisplay *Self, gfx::SetGamma *Args)
{
#ifdef __snap__
   pf::Log log;
   GA_palette palette[256];
   double intensity, red, green, blue;

   if (!Args) return log.warning(ERR::NullArgs);

   red   = Args->Red;
   green = Args->Green;
   blue  = Args->Blue;

   if (red   < 0.00) red   = 0.00;
   if (green < 0.00) green = 0.00;
   if (blue  < 0.00) blue  = 0.00;

   if (red   > 100.0) red   = 100.0;
   if (green > 100.0) green = 100.0;
   if (blue  > 100.0) blue  = 100.0;

   if ((Args->Flags & GMF::SAVE) != GMF::NIL) {
      Self->Gamma[0]   = red;
      Self->Gamma[1] = green;
      Self->Gamma[2]  = blue;
   }

   for (int i=0; i < std::ssize(palette); i++) {
      intensity = (double)i / 255.0;
      palette[i].Red   = int(pow(intensity, 1.0 / red)   * 255.0);
      palette[i].Green = int(pow(intensity, 1.0 / green) * 255.0);
      palette[i].Blue  = int(pow(intensity, 1.0 / blue)  * 255.0);
   }

   SetGammaCorrectData(palette, std::ssize(palette), 0, TRUE);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
SetGammaLinear: Sets the display gamma level using a linear algorithm.

Call SetGammaLinear() to update a target display's gamma values with a linear algorithm that takes input from `Red`,
`Green` and `Blue` parameters provided by the client.

-INPUT-
double Red: New red gamma value.
double Green: New green gamma value.
double Blue: New blue gamma value.
int(GMF) Flags: Use `SAVE` to store the new settings.

-ERRORS-
Okay:
NullArgs:
-END-

*********************************************************************************************************************/

static ERR DISPLAY_SetGammaLinear(extDisplay *Self, gfx::SetGammaLinear *Args)
{
#ifdef __snap__
   pf::Log log;
   GA_palette palette[256];

   if (!Args) return log.warning(ERR::NullArgs);

   double red   = Args->Red;
   double green = Args->Green;
   double blue  = Args->Blue;

   if (red   < 0.00) red   = 0.00;
   if (green < 0.00) green = 0.00;
   if (blue  < 0.00) blue  = 0.00;

   if (red   > 100.0) red   = 100.0;
   if (green > 100.0) green = 100.0;
   if (blue  > 100.0) blue  = 100.0;

   if ((Args->Flags & GMF::SAVE) != GMF::NIL) {
      Self->Gamma[0]   = red;
      Self->Gamma[1] = green;
      Self->Gamma[2]  = blue;
   }

   for (int16_t i=0; i < std::ssize(palette); i++) {
      double intensity = (double)i / 255.0;

      if (red > 1.0) palette[i].Red = int(pow(intensity, 1.0 / red) * 255.0);
      else palette[i].Red = int((double)i * red);

      if (green > 1.0) palette[i].Green = int(pow(intensity, 1.0 / green) * 255.0);
      else palette[i].Green = int((double)i * green);

      if (blue > 1.0) palette[i].Blue = int(pow(intensity, 1.0 / blue) * 255.0);
      else palette[i].Blue = int((double)i * blue);
   }

   glSNAP->Driver.SetGammaCorrectData(palette, std::ssize(palette), 0, TRUE);

   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
SetMonitor: Changes the default monitor settings.

Use the SetMonitor() method to change the settings that configure the user's monitor display.  You can set the model name
of the monitor and the frequencies that are supported by it.  Altering the display frequencies will affect the
available display resolutions, as well as the maximum allowable refresh rate.

An AutoDetect option is available, which if defined will cause the display settings to be automatically detected when
the desktop is loaded at startup. If it is not possible to detect the correct settings for the plugged-in display, it
reverts to the default display settings.

This method does not work on hosted platforms.  All parameters passed to this method are optional (set a value to zero
if it should not be changed).

-INPUT-
cstr Name: The name of the display.
int MinH: The minimum horizontal scan rate.  Usually set to 31.
int MaxH: The maximum horizontal scan rate.
int MinV: The minimum vertical scan rate.  Usually set to 50.
int MaxV: The maximum vertical scan rate.
int(MON) Flags: Set to `AUTO_DETECT` if the monitor settings should be auto-detected on startup.  Set `BIT_6` if the device is limited to 6-bit colour output.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

static ERR DISPLAY_SetMonitor(extDisplay *Self, gfx::SetMonitor *Args)
{
#ifdef __snap__
   pf::Log log;
   OBJECTPTR config;
   GA_monitor monitor;
   ERR priverror;

   if (!Args) return log.warning(ERR::NullArgs);

   if (CurrentTaskID() != Self->ownerTask()) {
      log.warning("Only the owner of the display may call this method.");
      return ERR::NoPermission;
   }

   log.branch("%s", Args->Name);

   glSixBitDisplay = ((Args->Flags & MON::BIT_6) != MON::NIL);
   if (glSixBitDisplay) Self->Flags |= SCR::BIT_6;
   else Self->Flags &= ~SCR::BIT_6;

   if (Args->Name) StrCopy(Args->Name, Self->Display, sizeof(Self->Display));

   // Get the current monitor record, then set the new scan rates against it.

   clearmem(&monitor, sizeof(monitor));
   glSNAP->Init.GetMonitorInfo(&monitor, glSNAP->Init.GetActiveHead());

   monitor.maxResolution = 0;  // Must be zero for SNAP to filter display modes

   if (Args->MinH) monitor.minHScan = Args->MinH;
   if (Args->MaxH) monitor.maxHScan = Args->MaxH;
   if (Args->MinV) monitor.minVScan = Args->MinV;
   if (Args->MaxV) monitor.maxVScan = Args->MaxV;

   if (monitor.minHScan < 31) monitor.minHScan = 31;
   if (monitor.minVScan < 50) monitor.minVScan = 50;
   if (monitor.maxHScan < 35) monitor.maxHScan = 35;
   if (monitor.maxVScan < 61) monitor.maxVScan = 61;

   // Apply the scan-rate changes to SNAP

   glSNAP->Init.SetMonitorInfo(&monitor, glSNAP->Init.GetActiveHead());

   // Refresh our display information from SNAP

   glSNAP->Init.GetMonitorInfo(&monitor, glSNAP->Init.GetActiveHead());
   Self->MinHScan = monitor.minHScan;
   Self->MaxHScan = monitor.maxHScan;
   Self->MinVScan = monitor.minVScan;
   Self->MaxVScan = monitor.maxVScan;

   // Mark the resolution list for regeneration

   Self->Resolutions.clear();

   // Regenerate the screen.xml file

   GenerateDisplayXML();

   // Save the changes to the monitor.cfg file.  This requires admin privileges, so this is only going to work if
   // SetMonitor() is messaged to the core desktop process.

   priverror = SetResource(RES::PRIVILEGED_USER, 1);

   objConfig::create config = { fl::Path("config:hardware/monitor.cfg") };
   if (config.ok()) {
      config->write("MONITOR", "Name", Self->Display);
      config->write("MONITOR", "MinH", Self->MinHScan);
      config->write("MONITOR", "MaxH", Self->MaxHScan);
      config->write("MONITOR", "MinV", Self->MinVScan);
      config->write("MONITOR", "MaxV", Self->MaxVScan);
      config->write("MONITOR", "AutoDetect", ((Args->Flags & MON::AUTODETECT) != MON::NIL) ? 1 : 0);
      config->write("MONITOR", "6Bit", glSixBitDisplay);
      config->saveSettings();
   }

   if (!priverror) SetResource(RES::PRIVILEGED_USER, 0);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-ACTION-
Show: Presents a display object to the user.

This method presents a display object to the user.  On a hosted platform, this will result in a window appearing on
screen.  By default the window will be hosted within a window border which may contain regular window gadgets such as a
titlebar and buttons for close, maximise and minimise operations.  The position of the window is determined by the
#X and #Y fields.  In Kōtuku's native environment, the user's screen display will be altered to match the required
resolution and the graphics of the display's #Bitmap object will take up the entirety of the screen.

If the `BORDERLESS` flag has been set in the #Flags field, the window will appear without the surrounding border
and gadgets normally associated with new windows.

In Microsoft Windows, the #LeftMargin, #RightMargin, #TopMargin and #BottomMargin fields will be updated to reflect
the position of the client area within the hosted window.  In X11 these field values are all set to zero.

If the window is minimised at the time this action is called, the window will be restored to its original position if
the code for the host platform supports this capability.

The `VISIBLE` flag in the #Flags field will be set if the Show operation is successful.
-END-

*********************************************************************************************************************/

ERR DISPLAY_Show(extDisplay *Self)
{
   pf::Log log;

   log.branch();

   #ifdef __xwindows__
      if (!XDisplay) {
         log.error("No X11 display has been found for this machine.");
         return ERR::NoSupport;
      }

      // Some window managers fool with our position when mapping, so we use XMoveWindow() before and after to be
      // certain that we get the position that we want.

      if ((Self->Flags & SCR::BORDERLESS) IS SCR::NIL) {
         XMoveWindow(XDisplay, Self->XWindowHandle, Self->X, Self->Y);
      }

      XMapWindow(XDisplay, Self->XWindowHandle);

      if ((Self->Flags & SCR::BORDERLESS) IS SCR::NIL) {
         XMoveWindow(XDisplay, Self->XWindowHandle, Self->X, Self->Y);
      }

      XSync(XDisplay, False);

      Self->LeftMargin   = 0;
      Self->TopMargin    = 0;
      Self->RightMargin  = 0;
      Self->BottomMargin = 0;

      // Mapping a window may cause the window manager to resize it without sending a notification event, so check the
      // window size on a delay.

      QueueAction(gfx::CheckXWindow::id, Self->UID);

      // Originally introduced as a hack to manage focusing for dropdown menus, possibly no longer required as focus should remain with the instigator.

      //if (iequals("SystemDisplay", Self->Name)) {
      //   XSetInputFocus(XDisplay, Self->XWindowHandle, RevertToNone, CurrentTime);
      //}

   #elif _WIN32

      if ((Self->Flags & SCR::MAXIMISE) != SCR::NIL) winShowWindow(Self->WindowHandle, TRUE);
      else winShowWindow(Self->WindowHandle, FALSE);

      winUpdateWindow(Self->WindowHandle);
      winGetMargins(Self->WindowHandle, &Self->LeftMargin, &Self->TopMargin, &Self->RightMargin, &Self->BottomMargin);

   #elif __snap__

      if (glSNAP->Init.GetCurrentRefreshRate) Self->RefreshRate = (glSNAP->Init.GetCurrentRefreshRate() + 50) / 100;
      else Self->RefreshRate = -1;

      gfxSetGamma(Self, Self->Gamma[0], Self->Gamma[1], Self->Gamma[2]);

   #elif _GLES_

      #warning TODO: Bring back the native window if it is hidden.
      glActiveDisplayID = Self->UID;
      Self->Flags &= ~SCR::NOACCELERATION;

   #else
      return log.warning(ERR::NoSupport);
   #endif

   Self->Flags |= SCR::VISIBLE;

   objPointer *pointer;
   OBJECTID pointer_id;
   if (FindObject("SystemPointer", CLASSID::POINTER, FOF::NIL, &pointer_id) != ERR::Okay) {
      if (NewObject(CLASSID::POINTER, NF::UNTRACKED, (OBJECTPTR *)&pointer) IS ERR::Okay) {
         SetName(pointer, "SystemPointer");
         if ((Self->Owner) and (Self->Owner->classID() IS CLASSID::SURFACE)) pointer->setSurface(Self->Owner->UID);

         #ifdef __ANDROID__
            AConfiguration *config;
            if (!adGetConfig(&config)) {
               double dp_factor = 160.0 / AConfiguration_getDensity(config);
               pointer->ClickSlop = F2I(8.0 * dp_factor);
               log.trace("Click-slop calculated as %d.", pointer->ClickSlop);
            }
            else log.warning("Failed to get Android Config object.");
         #endif

         if (InitObject(pointer) != ERR::Okay) FreeResource(pointer);
         else acShow(pointer);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
UpdatePalette: Updates the video display palette to new colour values if in 256 colour mode.

Call UpdatePalette() to copy a new palette to the display bitmap's internal palette.  If the video display is running in
256 colour mode, the new palette colours will also be reflected in the display.

This method has no visible effect on RGB pixel displays.

-INPUT-
struct(*RGBPalette) NewPalette: The new palette to apply to the display bitmap.

-ERRORS-
Okay
NullArgs

*********************************************************************************************************************/

static ERR DISPLAY_UpdatePalette(extDisplay *Self, gfx::UpdatePalette *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->NewPalette)) return ERR::NullArgs;

   log.branch("Palette: %p, Colours: %d", Args->NewPalette, Args->NewPalette->AmtColours);

   if (Args->NewPalette->AmtColours > 256) {
      log.warning("Bad setting of %d colours in the new palette.", Args->NewPalette->AmtColours);
      Args->NewPalette->AmtColours = 256;
   }

   copymem(Args->NewPalette, Self->Bitmap->Palette, sizeof(*Args->NewPalette));

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
WaitVBL: Waits for a vertical blank.

This method waits for the strobe to reach the vertical blank area at the bottom of the display.  Not all graphics
hardware will support this method.  If this is the case, WaitVBL() will return immediately with `ERR::NoSupport`.

-ERRORS-
Okay
NoSupport

*********************************************************************************************************************/

ERR DISPLAY_WaitVBL(extDisplay *Self)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: Reference to the display's bitmap information.

The @Bitmap object describes the video region that will be used for displaying graphics. It holds details on the width,
height, type, number of colours and so on.  The display class inherits the bitmap's attributes, so it is not necessary
to retrieve a direct reference to the bitmap object in order to make adjustments.

The @Bitmap.Width and @Bitmap.Height can be larger than the display area, but never smaller.

-FIELD-
BmpX: The horizontal coordinate of the bitmap within a display.

This field defines the horizontal offset for the #Bitmap, which is positioned 'behind' the display. To achieve
hardware scrolling, call the #Move() action on the Bitmap in order to change this value and update the display.

-FIELD-
BmpY: The vertical coordinate of the Bitmap within a display.

This field defines the vertical offset for the #Bitmap, which is positioned 'behind' the display.  To achieve hardware
scrolling, you will need to call the Move() action on the #Bitmap in order to change this value and update the display.

-FIELD-
BottomMargin: In hosted mode, indicates the bottom margin of the client window.

If the display is hosted in a client window, the BottomMargin indicates the number of pixels between the client area
and the bottom window edge.

-FIELD-
Chipset: String describing the graphics chipset.

This string describes the graphic card's chipset, if known.

*********************************************************************************************************************/

static ERR GET_Chipset(extDisplay *Self, STRING *Value)
{
   *Value = Self->Chipset;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
HDensity: Returns the horizontal pixel density for the display.

Reading the HDensity field will return the horizontal pixel density for the display (pixels per inch).  If the physical
size of the display is unknown, a default value based on knowledge of the platform will be retuned.  For standard PC's
this will usually be 96.

A custom density value can be enforced by setting the `/interface/@dpi` value in the loaded style, or by setting
HDensity.

Reading this field always succeeds.

*********************************************************************************************************************/

ERR GET_HDensity(extDisplay *Self, int *Value)
{
   if (Self->HDensity) {
      *Value = Self->HDensity;
      return ERR::Okay;
   }

   #ifdef __ANDROID__
      Self->HDensity = 160; // Android devices tend to have a high DPI by default (compared to monitors)
   #else
      Self->HDensity = 96; // Standard PC DPI, matches Windows
   #endif

   // If the user has overridden the DPI with a preferred value, we have to use it.

   OBJECTID style_id;
   if (FindObject("glStyle", CLASSID::XML, FOF::NIL, &style_id) IS ERR::Okay) {
      pf::ScopedObjectLock<objXML> style(style_id, 3000);
      if (style.granted()) {
         char strdpi[32];
         if (acGetKey(style.obj, "/interface/@dpi", strdpi, sizeof(strdpi)) IS ERR::Okay) {
            *Value = strtol(strdpi, NULL, 0);
            Self->HDensity = *Value; // Store for future use.
            if (!Self->VDensity) Self->VDensity = Self->HDensity;
         }
         if (*Value >= 96) return ERR::Okay;
      }
   }

   #ifdef __ANDROID__
      AConfiguration *config;
      if (!adGetConfig(&config)) {
         int density = AConfiguration_getDensity(config);
         if ((density > 60) and (density < 20000)) {
            Self->HDensity = density;
            Self->VDensity = density;
         }
      }
   #elif _WIN32
      winGetDPI(&Self->HDensity, &Self->VDensity);
      if (Self->HDensity < 96) Self->HDensity = 96;
      if (Self->VDensity < 96) Self->VDensity = 96;
   #endif

   *Value = Self->HDensity;
   return ERR::Okay;
}

static ERR SET_HDensity(extDisplay *Self, int Value)
{
   Self->HDensity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
VDensity: Returns the vertical pixel density for the display.

Reading the VDensity field will return the vertical pixel density for the display (pixels per inch).  If the physical
size of the display is unknown, a default value based on knowledge of the platform will be retuned.  For standard PC's
this will usually be 96.

A custom density value can be enforced by setting the `/interface/@dpi` value in the loaded style, or by setting
VDensity.

Reading this field always succeeds.

*********************************************************************************************************************/

ERR GET_VDensity(extDisplay *Self, int *Value)
{
   if (Self->VDensity) {
      *Value = Self->VDensity;
      return ERR::Okay;
   }

   #ifdef __ANDROID__
      Self->VDensity = 160; // Android devices tend to have a high DPI by default (compared to monitors)
   #else
      Self->VDensity = 96; // Standard PC DPI, matches Windows
   #endif

   // If the user has overridden the DPI with a preferred value, we have to use it.

   OBJECTID style_id;
   if (FindObject("glStyle", CLASSID::XML, FOF::NIL, &style_id) IS ERR::Okay) {
      pf::ScopedObjectLock<objXML> style(style_id, 3000);
      if (style.granted()) {
         char strdpi[32];
         if (acGetKey(style.obj, "/interface/@dpi", strdpi, sizeof(strdpi)) IS ERR::Okay) {
            *Value = strtol(strdpi, NULL, 0);
            Self->VDensity = *Value;
            if (!Self->HDensity) Self->HDensity = Self->VDensity;
         }
         if (*Value >= 96) return ERR::Okay;
      }
   }

   #ifdef __ANDROID__
      AConfiguration *config;
      if (!adGetConfig(&config)) {
         int density = AConfiguration_getDensity(config);
         if ((density > 60) and (density < 20000)) {
            Self->HDensity = density;
            Self->VDensity = density;
         }
      }
   #elif _WIN32
      winGetDPI(&Self->HDensity, &Self->VDensity);
      if (Self->HDensity < 96) Self->HDensity = 96;
      if (Self->VDensity < 96) Self->VDensity = 96;
   #endif

   *Value = Self->VDensity;
   return ERR::Okay;
}

static ERR SET_VDensity(extDisplay *Self, int Value)
{
   Self->VDensity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Display: String describing the display (e.g. model name of the monitor).

This string describes the display device that is connected to the user's graphics card.

*********************************************************************************************************************/

static ERR GET_Display(extDisplay *Self, CSTRING *Value)
{
   if (Self->Display[0]) *Value = Self->Display;
   else *Value = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DisplayManufacturer: String describing the display manufacturer.

This string names the manufacturer of the user's display device.

*********************************************************************************************************************/

static ERR GET_DisplayManufacturer(extDisplay *Self, CSTRING *Value)
{
   if (Self->DisplayManufacturer[0]) *Value = Self->DisplayManufacturer;
   else *Value = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DisplayType: In hosted mode, indicates the bottom margin of the client window.

If the display is hosted in a client window, the #BottomMargin indicates the number of pixels between the client area
and the bottom window edge.

-FIELD-
Flags: Optional flag settings.

Optional display flags can be defined here.  Post-initialisation, the only flags that can be set are `AUTO_SAVE` and
`BORDERLESS`.

*********************************************************************************************************************/

static ERR SET_Flags(extDisplay *Self, SCR Value)
{
   pf::Log log;

   if (Self->initialised()) {
      // Only flags that are explicitly supported here may be set post-initialisation.

      static const SCR ACCEPT_FLAGS = SCR::AUTO_SAVE|SCR::GRAB_CONTROLLERS;
      auto accept = Value & ACCEPT_FLAGS;
      Self->Flags = (Self->Flags & (~ACCEPT_FLAGS)) | accept;

      if ((((Self->Flags & SCR::BORDERLESS) != SCR::NIL) and ((Value & SCR::BORDERLESS) IS SCR::NIL)) or
          (((Self->Flags & SCR::BORDERLESS) IS SCR::NIL) and ((Value & SCR::BORDERLESS) != SCR::NIL))) {
      #ifdef _WIN32

         log.msg("Switching window type.");

         bool maximise = true;
         CSTRING title = nullptr;
         Self->get(FID_Title, title); // Get the window title before we kill it

         OBJECTID surface_id = winLookupSurfaceID(Self->WindowHandle);
         winSetSurfaceID(Self->WindowHandle, 0); // Nullify the surface ID to prevent WM_DESTROY from being acted upon
         winDestroyWindow(Self->WindowHandle);

         HWND popover = 0;
         if ((Self->WindowHandle = winCreateScreen(popover, &Self->X, &Self->Y, &Self->Width, &Self->Height,
               maximise, ((Self->Flags & SCR::BORDERLESS) != SCR::NIL) ? false : true, title, FALSE, 255, TRUE))) {

            Self->Flags = Self->Flags ^ SCR::BORDERLESS;

            winSetSurfaceID(Self->WindowHandle, surface_id);
            winGetMargins(Self->WindowHandle, &Self->LeftMargin, &Self->TopMargin, &Self->RightMargin, &Self->BottomMargin);

            // Report the new window dimensions

            int cx, cy, cwidth, cheight;
            winGetCoords(Self->WindowHandle, Self->X, Self->Y, Self->Width, Self->Height, cx, cy, cwidth, cheight);

            resize_feedback(&Self->ResizeFeedback, Self->UID, cx, cy, cwidth, cheight);

            if ((Self->Flags & SCR::VISIBLE) != SCR::NIL) {
               winShowWindow(Self->WindowHandle, TRUE);
               QueueAction(AC::Focus, Self->UID);
            }
         }

      #elif __xwindows__

         if (glX11.Manager) return ERR::NoSupport;

         XSetWindowAttributes swa;

         log.msg("Destroying current window.");

         swa.event_mask  = 0;
         XChangeWindowAttributes(XDisplay, Self->XWindowHandle, CWEventMask, &swa);

         XDestroyWindow(XDisplay, Self->XWindowHandle);
         Self->WindowHandle = nullptr;

         Self->Flags = Self->Flags ^ SCR::BORDERLESS;

         swa.bit_gravity = CenterGravity;
         swa.win_gravity = CenterGravity;
         swa.cursor      = C_Default;
         swa.override_redirect = (Self->Flags & (SCR::BORDERLESS|SCR::COMPOSITE)) != SCR::NIL;
         swa.event_mask  = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask
                           |KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask;

         int cwflags = CWEventMask|CWOverrideRedirect;

         if ((Self->Flags & (SCR::BORDERLESS|SCR::COMPOSITE)) != SCR::NIL) {
            Self->X = 0;
            Self->Y = 0;
            Self->Width  = glRootWindow.width;
            Self->Height = glRootWindow.height;
         }
         else {
            Self->X = 50;
            Self->Y = 70;
            Self->Width  = glRootWindow.width - 100;
            Self->Height = glRootWindow.height - 140;
            if (Self->X+Self->Width > glRootWindow.width) Self->Width = glRootWindow.width - Self->X;
            if (Self->Y+Self->Height > glRootWindow.height) Self->Height = glRootWindow.height - Self->Y;
         }

         if (!(Self->WindowHandle = (APTR)XCreateWindow(XDisplay, DefaultRootWindow(XDisplay),
               Self->X, Self->Y, Self->Width, Self->Height, 0, CopyFromParent, InputOutput,
               CopyFromParent, cwflags, &swa))) {
            log.warning("Failed in call to XCreateWindow().");
            return ERR::CreateResource;
         }

         STRING name;
         if ((CurrentTask()->get(FID_Name, name) IS ERR::Okay) and (name)) {
            XStoreName(XDisplay, Self->XWindowHandle, name);
         }
         else XStoreName(XDisplay, Self->XWindowHandle, "Kotuku");

         Atom protocols[1] = { XWADeleteWindow };
         XSetWMProtocols(XDisplay, Self->XWindowHandle, protocols, 1);

         if (glStickToFront) {
            XSetTransientForHint(XDisplay, Self->XWindowHandle, DefaultRootWindow(XDisplay));
         }

         XChangeProperty(XDisplay, Self->XWindowHandle, atomSurfaceID, atomSurfaceID, 32, PropModeReplace, (uint8_t *)&Self->UID, 1);

         // Indicate that the window position is not to be meddled with by the window manager.

         XSizeHints hints = { .flags = USPosition|USSize };
         XSetWMNormalHints(XDisplay, Self->XWindowHandle, &hints);

         // The keyboard qualifiers need to be reset, because if the user is holding down any keys we will lose any
         // key-release messages due on the window that we've terminated.

         glKeyFlags = KQ::NIL;

         Self->Bitmap->set(FID_Handle, Self->WindowHandle);
         acResize(Self->Bitmap, Self->Width, Self->Height, 0);

         if ((Self->Flags & SCR::VISIBLE) != SCR::NIL) {
            acShow(Self);
            XSetInputFocus(XDisplay, Self->XWindowHandle, RevertToNone, CurrentTime);
            QueueAction(AC::Focus, Self->UID);
         }

         resize_feedback(&Self->ResizeFeedback, Self->UID, Self->X, Self->Y, Self->Width, Self->Height);

         XSync(XDisplay, False);
      #endif
      }

      if (((Self->Flags & SCR::MAXIMISE) != SCR::NIL) and ((Value & SCR::MAXIMISE) IS SCR::NIL)) { // Turn maximise off
         #ifdef _WIN32
            if ((Self->Flags & SCR::VISIBLE) != SCR::NIL) winShowWindow(Self->WindowHandle, FALSE);
            Self->Flags |= SCR::MAXIMISE;
         #elif __xwindows__

         #endif
      }

      if (((Self->Flags & SCR::MAXIMISE) IS SCR::NIL) and ((Value & SCR::MAXIMISE) != SCR::NIL)) { // Turn maximise on
         #ifdef _WIN32
            if ((Self->Flags & SCR::VISIBLE) != SCR::NIL) winShowWindow(Self->WindowHandle, TRUE);
            Self->Flags |= SCR::MAXIMISE;
         #elif __xwindows__

         #endif
      }
   }
   else Self->Flags = (Value) & (~SCR::READ_ONLY);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Gamma: Contains red, green and blue values for the display's gamma setting.

The gamma settings for the display are stored in this field.  The settings are stored in an array of 3 floating point
values that represent red, green and blue colours guns.  The default gamma value for each colour gun is 1.0.

To modify the display gamma values, please refer to the #SetGamma() and #SetGammaLinear() methods.

*********************************************************************************************************************/

static ERR GET_Gamma(extDisplay *Self, double **Value, int *Elements)
{
   *Elements = 3;
   *Value = Self->Gamma;
   return ERR::Okay;
}

static ERR SET_Gamma(extDisplay *Self, double *Value, int Elements)
{
   if (Value) {
      if (Elements > 3) Elements = 3;
      int16_t i;
      for (i=0; i < Elements; i++) Self->Gamma[i] = Value[i];
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: Defines the height of the display.

This field defines the height of a display.  This is known as the 'viewport' that the bitmap data is displayed through.
If the height exceeds allowable limits, it will be restricted to a value that the display hardware can handle.

If the display is hosted, the height reflects the internal height of the host window.  On some hosted systems, the true
height of the window can be calculated by reading the #TopMargin and #BottomMargin fields.

*********************************************************************************************************************/

static ERR SET_Height(extDisplay *Self, int Value)
{
   if (Value > 0) Self->Height = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
InsideHeight: Represents the internal height of the display.

On full-screen displays, the video data area can exceed the height of the screen display.  The InsideHeight reflects
the height of the video data in pixels.  If this feature is not in use or is unavailable, the InsideWidth is equal to
the display #Height.

*********************************************************************************************************************/

static ERR GET_InsideHeight(extDisplay *Self, int *Value)
{
   *Value = Self->Bitmap->Height;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
InsideWidth: Represents the internal width of the display.

On full-screen displays, the video data area can exceed the width of the screen display.  The InsideWidth reflects the
width of the video data in pixels.  If this feature is not in use or is unavailable, the InsideWidth is equal to the
display #Width.

*********************************************************************************************************************/

static ERR GET_InsideWidth(extDisplay *Self, int *Value)
{
   *Value = Self->Bitmap->Width;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LeftMargin: In hosted mode, indicates the left-hand margin of the client window.

If the display is hosted in a client window, the LeftMargin indicates the number of pixels between the client area and
the left window edge.

-FIELD-
Manufacturer: String describing the manufacturer of the graphics hardware.

The string in this field returns the name of the manufacturer that created the user's graphics card.  If this
information is not detectable, a `NULL` pointer is returned.

*********************************************************************************************************************/

static ERR GET_Manufacturer(extDisplay *Self, STRING *Value)
{
   if (Self->Manufacturer[0]) *Value = Self->Manufacturer;
   else *Value = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MaxHScan: The maximum horizontal scan rate of the display output device.

If the display output device supports variable refresh rates, this field will refer to the maximum horizontal scan rate
supported by the device.  If variable refresh rates are not supported, this field is set to zero.

-FIELD-
MaxVScan: The maximum vertical scan rate of the display output device.

If the display output device supports variable refresh rates, this field will refer to the maximum vertical scan rate
supported by the device.  If variable refresh rates are not supported, this field is set to zero.

-FIELD-
MinHScan: The minimum horizontal scan rate of the display output device.

If the display output device supports variable refresh rates, this field will refer to the minimum horizontal scan rate
supported by the device.  If variable refresh rates are not supported, this field is set to zero.

-FIELD-
MinVScan: The minimum vertical scan rate of the display output device.

If the display output device supports variable refresh rates, this field will refer to the minimum vertical scan rate
supported by the device.  If variable refresh rates are not supported, this field is set to zero.

-FIELD-
Opacity: Determines the level of translucency applied to the display (hosted displays only).

This field determines the translucency level applied to a display. Its support level is limited to hosted displays that
support translucent windows (for example, Windows XP).  The default setting is 100%, which means that the display will
be solid.  High values will retain the boldness of the display, while low values reduce visibility.

****************************************************************************/

static ERR GET_Opacity(extDisplay *Self, double *Value)
{
   *Value = Self->Opacity * 100 / 255;
   return ERR::Okay;
}

static ERR SET_Opacity(extDisplay *Self, double Value)
{
#ifdef _WIN32
   if (Value < 0) Self->Opacity = 0;
   else if (Value > 100) Self->Opacity = 255;
   else Self->Opacity = Value * 255 / 100;
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FIELD-
PopOver: Enables pop-over support for hosted display windows.

The PopOver field can be used when a display is hosted as a window.  Setting the PopOver field to refer to the object
ID of another display will ensure that the host window is always in front of the other display's window (assuming both
windows are visible on the desktop).

The `ERR::NoSupport` error code is returned if the host does not support this functionality or if the display owns the
output device.

*********************************************************************************************************************/

static ERR SET_PopOver(extDisplay *Self, OBJECTID Value)
{
   pf::Log log;

#ifdef __xwindows__

   if (Self->initialised()) {
      if (!Value) {
         Self->PopOverID = 0;
         XSetTransientForHint(XDisplay, Self->XWindowHandle, (Window)0);
      }
      else if (ScopedObjectLock<extDisplay> popover(Value, 2000); popover.granted()) {
         if (popover->Class->BaseClassID IS CLASSID::DISPLAY) {
            Self->PopOverID = Value;
            XSetTransientForHint(XDisplay, Self->XWindowHandle, (Window)popover->WindowHandle);
         }
      }
      else return ERR::AccessObject;
   }
   else if (Value) {
      if (GetClassID(Value) IS CLASSID::DISPLAY) {
         Self->PopOverID = Value;
      }
      else return log.warning(ERR::WrongClass);
   }
   else Self->PopOverID = 0;

   return ERR::Okay;

#elif _WIN32

   if (Value) {
      if (GetClassID(Value) IS CLASSID::DISPLAY) Self->PopOverID = Value;
      else return log.warning(ERR::WrongClass);
   }
   else Self->PopOverID = 0;

   return ERR::Okay;

#else

   return ERR::NoSupport;

#endif
}

/*********************************************************************************************************************

-FIELD-
PowerMode: The display's power management method.

When DPMS is enabled via a call to #Disable(), the DPMS method that is applied is controlled by this field.

DPMS is a user configurable option and it is not recommended that the PowerMode value is changed manually.

-FIELD-
RefreshRate: This field manages the display refresh rate.

The value in this field reflects the refresh rate of the currently active display, if operating in full-screen mode.

*********************************************************************************************************************/

static ERR SET_RefreshRate(extDisplay *Self, double Value)
{
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ResizeFeedback: This field manages the display refresh rate.

The value in this field reflects the refresh rate of the currently active display, if operating in full-screen mode.

*********************************************************************************************************************/

static ERR GET_ResizeFeedback(extDisplay *Self, FUNCTION **Value)
{
   if (Self->ResizeFeedback.defined()) {
      *Value = &Self->ResizeFeedback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_ResizeFeedback(extDisplay *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->ResizeFeedback.isScript()) UnsubscribeAction(Self->ResizeFeedback.Context, AC::Free);
      Self->ResizeFeedback = *Value;
      if (Self->ResizeFeedback.isScript()) {
         SubscribeAction(Self->ResizeFeedback.Context, AC::Free, C_FUNCTION(notify_resize_free));
      }
   }
   else Self->ResizeFeedback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RightMargin: In hosted mode, indicates the pixel margin between the client window and right window edge.

-FIELD-
TopMargin: In hosted mode, indicates the pixel margin between the client window and top window edge.

-FIELD-
TotalMemory: The total amount of user accessible RAM installed on the video card, or zero if unknown.

-FIELD-
TotalResolutions: The total number of resolutions supported by the display.

*********************************************************************************************************************/

static ERR GET_TotalResolutions(extDisplay *Self, int *Value)
{
   if (Self->Resolutions.empty()) get_resolutions(Self);
   *Value = Self->Resolutions.size();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Width: Defines the width of the display.

This field defines the width of a display.  This is known as the 'viewport' that the bitmap data is displayed through.
If the width exceeds allowable limits, it will be restricted to a value that the display hardware can handle.

If the display is hosted, the width reflects the internal width of the host window.  On some hosted systems, the true
width of the window can be calculated by reading the #LeftMargin and #RightMargin fields.

*********************************************************************************************************************/

static ERR SET_Width(extDisplay *Self, int Value)
{
   if (Value > 0) {
      if (Self->initialised()) {
         acResize(Self, Value, Self->Height, 0);
      }
      else Self->Width = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
WindowHandle: Refers to a display object's window handle, if relevant.

This field refers to the window handle of a display object, but only if such a thing is relevant to the platform that
the system is running on.  Currently, this field is only usable when creating a display within an X11 window manager or
Microsoft Windows.

It is possible to set the WindowHandle field prior to initialisation if you want a display object to be based on a
window that already exists.

*********************************************************************************************************************/

static ERR GET_WindowHandle(extDisplay *Self, APTR *Value)
{
   *Value = Self->WindowHandle;
   return ERR::Okay;
}

static ERR SET_WindowHandle(extDisplay *Self, APTR Value)
{
   if (Self->initialised()) return ERR::Immutable;

   if (Value) {
      Self->WindowHandle = Value;
      Self->Flags |= SCR::CUSTOM_WINDOW;
      #ifdef __xwindows__
         glPlugin = TRUE;
      #endif
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Title: Sets the window title (hosted environments only).

*********************************************************************************************************************/

#if defined(_WIN32)
static STRING glWindowTitle = nullptr;
#endif

static ERR GET_Title(extDisplay *Self, CSTRING *Value)
{
#ifdef __xwindows__
   return ERR::NoSupport;
#elif _WIN32
   char buffer[128];
   STRING str;

   buffer[0] = 0;
   winGetWindowTitle(Self->WindowHandle, buffer, sizeof(buffer));
   if (AllocMemory(strlen(buffer) + 1, MEM::STRING|MEM::UNTRACKED, &str) IS ERR::Okay) {
      strcopy(buffer, str);
      if (glWindowTitle) FreeResource(glWindowTitle);
      glWindowTitle = str;
      *Value = glWindowTitle;
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
#else
   return ERR::NoSupport;
#endif
}

static ERR SET_Title(extDisplay *Self, CSTRING Value)
{
#ifdef __xwindows__
   XStoreName(XDisplay, Self->XWindowHandle, Value);
   return ERR::Okay;
#elif _WIN32
   winSetWindowTitle(Self->WindowHandle, Value);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************
-FIELD-
X: Defines the horizontal coordinate of the display.

The X field defines the horizontal hardware coordinate for a display.  This field should be set to zero unless the
screen requires adjustment.  Most hardware drivers and output devices do not support this feature.

On hosted displays, prior to initialisation the coordinate will reflect the position of the display window when it is
created.  After initialisation, the coordinate is altered to reflect the absolute position of the client area of the
display window.  The #LeftMargin can be used to determine the actual position of the host window.

To adjust the position of the display, use the #MoveToPoint() action rather than setting this field directly.

*********************************************************************************************************************/

static ERR SET_X(extDisplay *Self, int Value)
{
   if (!(Self->initialised())) {
      Self->X = Value;
      return ERR::Okay;
   }
   else return acMoveToPoint(Self, Value, 0, 0, MTF::X);
}

/*********************************************************************************************************************
-FIELD-
Y: Defines the vertical coordinate of the display.

The Y field defines the vertical hardware coordinate for a display.  This field should be set to zero unless the
screen requires adjustment.  Most hardware drivers and output devices do not support this feature.

On hosted displays, prior to initialisation the coordinate will reflect the position of the display window  when it is
created.  After initialisation, the coordinate is altered to reflect the absolute position of the client area of the
display window.  The #TopMargin can be used to determine the actual position of the host window.

To adjust the position of the display, use the #MoveToPoint() action rather than setting this field directly.
-END-
*********************************************************************************************************************/

static ERR SET_Y(extDisplay *Self, int Value)
{
   if (!(Self->initialised())) {
      Self->Y = Value;
      return ERR::Okay;
   }
   else return acMoveToPoint(Self, 0, Value, 0, MTF::Y);
}

//********************************************************************************************************************
// Attempt to create a display buffer (process is not guaranteed, programmer has to check the Buffer field to know if
// this succeeded or not).

void alloc_display_buffer(extDisplay *Self)
{
   pf::Log log(__FUNCTION__);

   log.branch("Allocating a video based buffer bitmap.");

   if (Self->BufferID) { FreeResource(Self->BufferID); Self->BufferID = 0; }

   if (auto buffer = objBitmap::create::local(
         fl::Name("SystemBuffer"),
         fl::BitsPerPixel(Self->Bitmap->BitsPerPixel),
         fl::BytesPerPixel(Self->Bitmap->BytesPerPixel),
         fl::Width(Self->Bitmap->Width),
         fl::Height(Self->Bitmap->Height),
         #ifdef __xwindows__
            fl::DataFlags(MEM::DATA)
         #else
            fl::DataFlags(MEM::TEXTURE)
         #endif
      )) {
      Self->BufferID = buffer->UID;
   }

}

//********************************************************************************************************************

#include "class_display_def.c"

static const FieldArray DisplayFields[] = {
   // Re-compile the TDL if making changes
   { "RefreshRate",    FDF_DOUBLE|FDF_RW, nullptr, SET_RefreshRate },
   { "Bitmap",         FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "Flags",          FDF_INTFLAGS|FDF_RW, nullptr, SET_Flags, &clDisplayFlags },
   { "Width",          FDF_INT|FDF_RW, nullptr, SET_Width },
   { "Height",         FDF_INT|FDF_RW, nullptr, SET_Height },
   { "X",              FDF_INT|FDF_RW, nullptr, SET_X },
   { "Y",              FDF_INT|FDF_RW, nullptr, SET_Y },
   { "BmpX",           FDF_INT|FDF_RW },
   { "BmpY",           FDF_INT|FDF_RW },
   { "Buffer",         FDF_OBJECTID|FDF_R|FDF_SYSTEM, nullptr, nullptr, CLASSID::BITMAP },
   { "TotalMemory",    FDF_INT|FDF_R },
   { "MinHScan",       FDF_INT|FDF_R },
   { "MaxHScan",       FDF_INT|FDF_R },
   { "MinVScan",       FDF_INT|FDF_R },
   { "MaxVScan",       FDF_INT|FDF_R },
   { "DisplayType",    FDF_INT|FDF_LOOKUP|FDF_R, nullptr, nullptr, &clDisplayDisplayType },
   { "PowerMode",      FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clDisplayPowerMode },
   { "PopOver",        FDF_OBJECTID|FDF_W, nullptr, SET_PopOver },
   { "LeftMargin",     FDF_INT|FDF_R },
   { "RightMargin",    FDF_INT|FDF_R },
   { "TopMargin",      FDF_INT|FDF_R },
   { "BottomMargin",   FDF_INT|FDF_R },
   // Virtual fields
   { "Chipset",             FDF_VIRTUAL|FDF_STRING|FDF_R,    GET_Chipset },
   { "Gamma",               FDF_VIRTUAL|FDF_DOUBLE|FDF_ARRAY|FDF_RI, GET_Gamma, SET_Gamma },
   { "HDensity",            FDF_VIRTUAL|FDF_INT|FDF_RW,      GET_HDensity, SET_HDensity },
   { "VDensity",            FDF_VIRTUAL|FDF_INT|FDF_RW,      GET_VDensity, SET_VDensity },
   { "Display",             FDF_VIRTUAL|FDF_STRING|FDF_R,    GET_Display },
   { "DisplayManufacturer", FDF_VIRTUAL|FDF_STRING|FDF_R,    GET_DisplayManufacturer },
   { "InsideWidth",         FDF_VIRTUAL|FDF_INT|FDF_R,       GET_InsideWidth },
   { "InsideHeight",        FDF_VIRTUAL|FDF_INT|FDF_R,       GET_InsideHeight },
   { "Manufacturer",        FDF_VIRTUAL|FDF_STRING|FDF_R,    GET_Manufacturer },
   { "Opacity",             FDF_VIRTUAL|FDF_DOUBLE|FDF_W,    GET_Opacity, SET_Opacity },
   { "ResizeFeedback",      FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_ResizeFeedback, SET_ResizeFeedback },
   { "WindowHandle",        FDF_VIRTUAL|FDF_POINTER|FDF_RW,  GET_WindowHandle, SET_WindowHandle },
   { "Title",               FDF_VIRTUAL|FDF_STRING|FDF_RW,   GET_Title, SET_Title },
   { "TotalResolutions",    FDF_VIRTUAL|FDF_INT|FDF_R,       GET_TotalResolutions },
   END_FIELD
};

//********************************************************************************************************************

CSTRING dpms_name(DPMS Index)
{
   return clDisplayPowerMode[int(Index)].Name;
}

//********************************************************************************************************************

ERR create_display_class(void)
{
   clDisplay = objMetaClass::create::global(
      fl::ClassVersion(VER_DISPLAY),
      fl::Name("Display"),
      fl::Category(CCF::GRAPHICS),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Actions(clDisplayActions),
      fl::Methods(clDisplayMethods),
      fl::Fields(DisplayFields),
      fl::Size(sizeof(extDisplay)),
      fl::Path(MOD_PATH));

   return clDisplay ? ERR::Okay : ERR::AddClass;
}
