/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

#include "defs.h"

JUMPTABLE_REGEX

#ifdef _WIN32
using namespace display;
#endif

ERR GET_HDensity(extDisplay *Self, int *Value);
ERR GET_VDensity(extDisplay *Self, int *Value);

//********************************************************************************************************************

std::array<uint8_t, 256 * 256> glAlphaLookup;

#ifdef __xwindows__

#define MAX_KEYCODES 256
#define TIME_X11DBLCLICK 600

void X11ManagerLoop(HOSTHANDLE, APTR);
void handle_button_press(XEvent *);
void handle_button_release(XEvent *);
void handle_configure_notify(XConfigureEvent *);
void handle_enter_notify(XCrossingEvent *);
void handle_exposure(XExposeEvent *);
void handle_key_press(XEvent *);
void handle_key_release(XEvent *);
void handle_motion_notify(XMotionEvent *);
void handle_stack_change(XCirculateEvent *);

X11Globals glX11;
_XDisplay *XDisplay = 0;
XVisualInfo glXInfoAlpha;
bool glX11ShmImage = false;
bool glXCompositeSupported = false;
uint8_t KeyHeld[int(KEY::LIST_END)];
uint8_t glTrayIcon = 0, glTaskBar = 1, glStickToFront = 0;
KQ glKeyFlags = KQ::NIL;
int glXFD = -1, glDGAPixelsPerLine = 0, glDGABankSize = 0;
Atom atomSurfaceID = 0, XWADeleteWindow = 0, XWATakeFocus = 0;
GC glXGC = 0, glClipXGC = 0;
XWindowAttributes glRootWindow;
Window glDisplayWindow = 0;
Cursor C_Default;
OBJECTPTR modXRR = nullptr;
int16_t glPlugin = FALSE;
APTR glDGAVideo = nullptr;

#ifdef XRANDR_ENABLED
bool glXRRAvailable = false;
#endif

#endif

#ifdef _WIN32

HINSTANCE glInstance = 0;

WinCursor winCursors[24] = {
   { 0, PTC::DEFAULT,           },  // NOTE: Refer to the microsoft.c file if you change anything here
   { 0, PTC::SIZE_BOTTOM_LEFT,  },
   { 0, PTC::SIZE_BOTTOM_RIGHT, },
   { 0, PTC::SIZE_TOP_LEFT,     },
   { 0, PTC::SIZE_TOP_RIGHT,    },
   { 0, PTC::SIZE_LEFT,         },
   { 0, PTC::SIZE_RIGHT,        },
   { 0, PTC::SIZE_TOP,          },
   { 0, PTC::SIZE_BOTTOM,       },
   { 0, PTC::CROSSHAIR,         },
   { 0, PTC::SLEEP,             },
   { 0, PTC::SIZING,            },
   { 0, PTC::SPLIT_VERTICAL,    },
   { 0, PTC::SPLIT_HORIZONTAL,  },
   { 0, PTC::MAGNIFIER,         },
   { 0, PTC::HAND,              },
   { 0, PTC::HAND_LEFT,         },
   { 0, PTC::HAND_RIGHT,        },
   { 0, PTC::TEXT,              },
   { 0, PTC::PAINTBRUSH,        },
   { 0, PTC::STOP,              },
   { 0, PTC::INVISIBLE,         },
   { 0, PTC::INVISIBLE,         },
   { 0, PTC::DRAGGABLE,         }
};

extern int16_t GetWindowsIcon()
{
   return GetResource(RES::WINDOWS_ICON);
}
#endif

#ifdef __ANDROID__
OBJECTPTR modAndroid;
struct AndroidBase *AndroidBase;

static void android_init_window(int);
static void android_term_window(int);
#endif

#include "module_def.c"

//********************************************************************************************************************
// Note: These values are used as the input masks

const InputType glInputType[int(JET::END)] = {
   { JTYPE::NIL, JTYPE::NIL },                                         // UNUSED
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_1
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_2
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_3
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_4
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_5
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_6
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_7
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_8
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_9
   { JTYPE::BUTTON,                 JTYPE::BUTTON },   // JET::BUTTON_10
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::WHEEL
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::WHEEL_TILT
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::PEN_TILT_XY
   { JTYPE::MOVEMENT,               JTYPE::MOVEMENT },     // JET::ABS_XY
   { JTYPE::CROSSING,               JTYPE::CROSSING },     // JET::CROSSING_IN
   { JTYPE::CROSSING,               JTYPE::CROSSING },     // JET::CROSSING_OUT
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::PRESSURE
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::DEVICE_TILT_XY
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }, // JET::DEVICE_TILT_Z
   { JTYPE::EXT_MOVEMENT,           JTYPE::EXT_MOVEMENT }  // JET::DISPLAY_EDGE
};

const CSTRING glInputNames[int(JET::END)] = {
   "",
   "BUTTON_1",
   "BUTTON_2",
   "BUTTON_3",
   "BUTTON_4",
   "BUTTON_5",
   "BUTTON_6",
   "BUTTON_7",
   "BUTTON_8",
   "BUTTON_9",
   "BUTTON_10",
   "WHEEL",
   "WHEEL_TILT",
   "PEN_TILT_XY",
   "ABS_XY",
   "CROSSING_IN",
   "CROSSING_OUT",
   "PRESSURE",
   "DEVICE_TILT_XY",
   "DEVICE_TILT_Z",
   "DISPLAY_EDGE"
};

#ifdef _GLES_ // OpenGL specific data
enum { EGL_STOPPED=0, EGL_REQUIRES_INIT, EGL_INITIALISED, EGL_TERMINATED };
static uint8_t glEGLState = 0;
static uint8_t glEGLRefreshDisplay = FALSE;
static OBJECTID glEGLPreferredDepth = 0;
static EGLContext glEGLContext = EGL_NO_CONTEXT;
static EGLSurface glEGLSurface = EGL_NO_SURFACE;
static EGLDisplay glEGLDisplay = EGL_NO_DISPLAY;
static EGLint glEGLWidth, glEGLHeight, glEGLDepth;
static pthread_mutex_t glGraphicsMutex;
static CSTRING glLastLock = nullptr;
static int glLockCount = 0;
static OBJECTID glActiveDisplayID = 0;
#endif

#ifdef __xwindows__

#ifdef XRANDR_ENABLED
static XRRScreenSize glCustomSizes[] = { { 640,480,0,0 }, { 800,600,0,0 }, { 1024,768,0,0 }, { 1280,1024,0,0 } };
static XRRScreenSize *glSizes = glCustomSizes;
static int glSizeCount = std::ssize(glCustomSizes);
static int glActualCount = 0;

static bool get_xrandr_version(int *Major, int *Minor)
{
   if ((!XDisplay) or (!Major) or (!Minor)) return false;

   int event_base = 0;
   int error_base = 0;
   if (!XRRQueryExtension(XDisplay, &event_base, &error_base)) return false;

   *Major = 0;
   *Minor = 0;
   if (!XRRQueryVersion(XDisplay, Major, Minor)) return false;

   return true;
}

static bool get_xrandr_refresh_rate(RRCrtc Crtc, float *RefreshRate)
{
   if ((!XDisplay) or (!RefreshRate)) return false;

   auto resources = XRRGetScreenResourcesCurrent(XDisplay, DefaultRootWindow(XDisplay));
   if (!resources) return false;

   auto result = false;

   for (int i=0; i < resources->ncrtc; i++) {
      if ((Crtc) and (resources->crtcs[i] != Crtc)) continue;

      if (auto crtc_info = XRRGetCrtcInfo(XDisplay, resources, resources->crtcs[i])) {
         if ((crtc_info->mode) and (crtc_info->noutput > 0)) {
            for (int m=0; m < resources->nmode; m++) {
               if (resources->modes[m].id IS crtc_info->mode) {
                  auto &mode = resources->modes[m];
                  if ((mode.hTotal > 0) and (mode.vTotal > 0) and (mode.dotClock > 0)) {
                     *RefreshRate = float((double)mode.dotClock / ((double)mode.hTotal * (double)mode.vTotal));
                     result = true;
                  }
                  break;
               }
            }
         }

         XRRFreeCrtcInfo(crtc_info);
      }

      if (result) break;
   }

   XRRFreeScreenResources(resources);
   return result;
}

static bool get_xrandr_output_refresh_rate(RROutput Output, float *RefreshRate)
{
   if ((!XDisplay) or (!Output) or (!RefreshRate)) return false;

   auto resources = XRRGetScreenResourcesCurrent(XDisplay, DefaultRootWindow(XDisplay));
   if (!resources) return false;

   auto result = false;

   if (auto output_info = XRRGetOutputInfo(XDisplay, resources, Output)) {
      if (output_info->crtc) result = get_xrandr_refresh_rate(output_info->crtc, RefreshRate);
      XRRFreeOutputInfo(output_info);
   }

   XRRFreeScreenResources(resources);
   return result;
}
#endif

static void get_x11_display_geometry(DisplayInfo *Info, int X, int Y, bool MatchPoint)
{
   if ((!Info) or (!XDisplay)) return;

   Info->VirtualX      = 0;
   Info->VirtualY      = 0;
   Info->VirtualWidth  = glRootWindow.width;
   Info->VirtualHeight = glRootWindow.height;
   Info->MonitorX      = 0;
   Info->MonitorY      = 0;
   Info->MonitorWidth  = glRootWindow.width;
   Info->MonitorHeight = glRootWindow.height;
   Info->PhysicalWidth = DisplayWidthMM(XDisplay, DefaultScreen(XDisplay));
   Info->PhysicalHeight = DisplayHeightMM(XDisplay, DefaultScreen(XDisplay));

   #ifdef XRANDR_ENABLED
      int major_version = 0;
      int minor_version = 0;
      if (not get_xrandr_version(&major_version, &minor_version)) return;

      if ((major_version > 1) or ((major_version IS 1) and (minor_version >= 5))) {
         int monitor_count = 0;
         auto monitors = XRRGetMonitors(XDisplay, DefaultRootWindow(XDisplay), True, &monitor_count);
         if ((monitors) and (monitor_count > 0)) {
            int monitor_index = 0;
            int primary_x = monitors[0].x;
            int primary_y = monitors[0].y;

            for (int i=0; i < monitor_count; i++) {
               if (monitors[i].primary) {
                  primary_x = monitors[i].x;
                  primary_y = monitors[i].y;
                  break;
               }
            }

            for (int i=0; i < monitor_count; i++) {
               if ((MatchPoint) and (X >= monitors[i].x) and (X < monitors[i].x + monitors[i].width) and
                     (Y >= monitors[i].y) and (Y < monitors[i].y + monitors[i].height)) {
                  monitor_index = i;
                  break;
               }
               else if ((not MatchPoint) and (monitors[i].primary)) {
                  monitor_index = i;
                  break;
               }
            }

            Info->MonitorX       = monitors[monitor_index].x - primary_x;
            Info->MonitorY       = monitors[monitor_index].y - primary_y;
            Info->MonitorWidth   = monitors[monitor_index].width;
            Info->MonitorHeight  = monitors[monitor_index].height;
            Info->PhysicalWidth  = monitors[monitor_index].mwidth;
            Info->PhysicalHeight = monitors[monitor_index].mheight;

            if (monitors[monitor_index].noutput > 0) {
               float refresh_rate = 0;
               if (get_xrandr_output_refresh_rate(monitors[monitor_index].outputs[0], &refresh_rate)) {
                  Info->RefreshRate = refresh_rate;
                  Info->MinRefresh  = refresh_rate;
                  Info->MaxRefresh  = refresh_rate;
               }
            }

            XRRFreeMonitors(monitors);
            return;
         }

         if (monitors) XRRFreeMonitors(monitors);
      }

      float refresh_rate = 0;
      if (get_xrandr_refresh_rate(0, &refresh_rate)) {
         Info->RefreshRate = refresh_rate;
         Info->MinRefresh  = refresh_rate;
         Info->MaxRefresh  = refresh_rate;
      }
   #endif
}

#endif

std::recursive_mutex glInputLock;

objCompression *glCompress = nullptr;
static objCompression *glIconArchive = nullptr;
struct CoreBase *CoreBase;
ColourFormat glColourFormat;
bool glHeadless = false;
OBJECTPTR glModule = nullptr, glDisplayContext = nullptr;
static OBJECTPTR modRegex = nullptr;
OBJECTPTR clDisplay = nullptr, clPointer = nullptr, clBitmap = nullptr, clClipboard = nullptr, clSurface = nullptr, clController = nullptr;
OBJECTID glPointerID = 0;
DisplayInfo glDisplayInfo;
bool glSixBitDisplay = false;
TIMER glRefreshPointerTimer = 0;
extBitmap *glComposite = nullptr;
static auto glDisplayType = DT::NATIVE;
double glpRefreshRate = -1, glpGammaRed = 1, glpGammaGreen = 1, glpGammaBlue = 1;
int glpDisplayWidth = 1024, glpDisplayHeight = 768, glpDisplayX = 0, glpDisplayY = 0;
int glpDisplayDepth = 0; // If zero, the display depth will be based on the hosted desktop's bit depth.
int glpMaximise = FALSE, glpFullScreen = FALSE;
SWIN glpWindowType = SWIN::HOST;
char glpDPMS[20] = "Standby";
uint8_t *glDemultiply = nullptr;
std::atomic<int> glLastPort = -1;

std::vector<OBJECTID> glFocusList;
std::recursive_mutex glFocusLock;
std::recursive_mutex glSurfaceLock;

thread_local int16_t tlNoDrawing = 0, tlNoExpose = 0, tlVolatileIndex = 0;
thread_local OBJECTID tlFreeExpose = 0;

//********************************************************************************************************************
// Alpha blending data.

inline uint8_t clipByte(int value)
{
   value = (0 & (-(int16_t)(value < 0))) | (value & (-(int16_t)!(value < 0)));
   value = (255 & (-(int16_t)(value > 255))) | (value & (-(int16_t)!(value > 255)));
   return value;
}

//********************************************************************************************************************
// Build a list of valid resolutions.

void get_resolutions(extDisplay *Self)
{
#if defined(__xwindows__) && defined(XRANDR_ENABLED)
   kt::Log log(__FUNCTION__);

   if (glXRRAvailable) {
      if (!Self->Resolutions.empty()) return;

      if (!glActualCount) {
         for (int i=0; i < glSizeCount; i++) {
            if ((glSizes[i].width >= 640) and (glSizes[i].height >= 480)) {
               glActualCount++;
            }
         }
      }

      auto get_mode = [&Self, &log](int Index) {
         for (int i=0; i < glSizeCount; i++) {
            if ((glSizes[i].width >= 640) and (glSizes[i].height >= 480)) {
               if (!Index) {
                  Self->Resolutions.emplace_back(glSizes[i].width, glSizes[i].height, DefaultDepth(XDisplay, DefaultScreen(XDisplay)));
                  return;
               }
               Index--;
            }
         }
      };

      for (int i=0; i < glActualCount; i++) {
         get_mode(i);
      }
   }
   else {
      log.msg("RandR extension not available.");
      Self->Resolutions.emplace_back(glRootWindow.width, glRootWindow.height, DefaultDepth(XDisplay, DefaultScreen(XDisplay)));
   }
#else
   Self->Resolutions = {
      { 640, 480, 32 },
      { 800, 600, 32 },
      { 1024, 768, 32 },
      { 1152, 864, 32 },
      { 1280, 960, 32 },
      { 0, 0, 0 }
   };
#endif
}

//********************************************************************************************************************

#ifdef XRANDR_ENABLED
ERR xr_set_display_mode(int *Width, int *Height)
{
   kt::Log log(__FUNCTION__);
   int count, i;
   int width = *Width;
   int height = *Height;

   if (glX11.WSLg) return ERR::NoSupport;

   XRRScreenSize *sizes;
   if ((sizes = XRRSizes(XDisplay, DefaultScreen(XDisplay), &count)) and (count)) {
      int16_t index    = -1;
      int bestweight = 0x7fffffff;

      for (i=0; i < count; i++) {
         int weight = std::abs(sizes[i].width - width) + std::abs(sizes[i].height - height);
         if (weight < bestweight) {
            index = i;
            bestweight = weight;
         }
      }

      if (index IS -1) {
         log.warning("No support for requested screen mode %dx%d", width, height);
         return ERR::NoSupport;
      }

      if (auto scrconfig = XRRGetScreenInfo(XDisplay, DefaultRootWindow(XDisplay))) {
         if (!XRRSetScreenConfig(XDisplay, scrconfig, DefaultRootWindow(XDisplay), index, RR_Rotate_0, CurrentTime)) {
            *Width = sizes[index].width;
            *Height = sizes[index].height;

            log.msg("New mode: %dx%d (index %d/%d) from request %dx%d", sizes[index].width, sizes[index].height, index, count, width, height);

            XRRFreeScreenConfigInfo(scrconfig);
            return ERR::Okay;
         }
         else {
            XRRFreeScreenConfigInfo(scrconfig);
            return log.warning(ERR::SystemCall);
         }
      }
      else return log.warning(ERR::SystemCall);
   }
   else return log.warning(ERR::SystemCall);
}
#endif // XRANDR_ENABLED

//********************************************************************************************************************
// GLES specific functions

#ifdef _GLES_
static int nearestPower(int value)
{
   int i = 1;

   if (value == 0) return value;
   if (value < 0) value = -value;

   for (;;) {
      if (value == 1) break;
      else if (value == 3) {
         i = i * 4;
         break;
      }
      value >>= 1;
      i *= 2;
   }

   return i;
}

[[maybe_unused]] int pthread_mutex_timedlock (pthread_mutex_t *mutex, int Timeout)
{
   struct timespec sleepytime;
   int retcode;

   sleepytime.tv_sec = 0;
   sleepytime.tv_nsec = 10000000; // 10ms

   int64_t start = PreciseTime();
   while ((retcode = pthread_mutex_trylock(mutex)) IS EBUSY) {
      if (PreciseTime() - start >= Timeout * 1000LL) return ETIMEDOUT;
      nanosleep(&sleepytime, nullptr);
   }

   return retcode;
}
#endif

//********************************************************************************************************************
// lock_graphics_active() is intended for functionality that MUST have access to an active OpenGL display.  If an EGL
// display is unavailable then this function will fail even if the lock could otherwise be granted.

#ifdef _GLES_
ERR lock_graphics_active(CSTRING Caller)
{
   kt::Log log(__FUNCTION__);

   //log.traceBranch("%s, Count: %d, State: %d, Display: $%x, Context: $%x", Caller, glLockCount, glEGLState, (LONG)glEGLDisplay, (LONG)glEGLContext); // See unlock_graphics() for the matching step.
   if (!pthread_mutex_lock(&glGraphicsMutex)) {
   //if (!(errno = pthread_mutex_timedlock(&glGraphicsMutex, 7000))) {
      glLastLock = Caller;

      if (glEGLState IS EGL_REQUIRES_INIT) {
         init_egl();
      }

      if ((glEGLState != EGL_INITIALISED) or (glEGLDisplay IS EGL_NO_DISPLAY)) {
         pthread_mutex_unlock(&glGraphicsMutex);
         //log.trace("EGL not initialised.");
         return ERR::NotInitialised;
      }

      if ((glEGLContext != EGL_NO_CONTEXT) and (!glLockCount)) {
         // eglMakeCurrent() allows our thread to use OpenGL.
         if (eglMakeCurrent(glEGLDisplay, glEGLSurface, glEGLSurface, glEGLContext) == EGL_FALSE) { // Failure probably indicates that a power management event has occurred (requires re-initialisation).
            pthread_mutex_unlock(&glGraphicsMutex);
            return ERR::NotInitialised;
         }
      }

      glLockCount++;
      return ERR::Okay;
   }
   else {
      log.warning("Failed to get lock for %s.  Locked by %s.  Error: %s", Caller, glLastLock, strerror(errno));
      return ERR::TimeOut;
   }
}

void unlock_graphics(void)
{
   glLockCount--;
   if (!glLockCount) {
      glLastLock = nullptr;
      if (glEGLContext != EGL_NO_CONTEXT) { // Turn off eglMakeCurrent() so that other threads can use OpenGL
         eglMakeCurrent(glEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      }
   }
   pthread_mutex_unlock(&glGraphicsMutex);
}

#endif

//********************************************************************************************************************

#ifdef __xwindows__

int16_t glDGAAvailable = -1; // -1 indicates that we have not tried the setup process yet

static bool detect_wslg(void)
{
   if (auto gui_enabled = getenv("WSL2_GUI_APPS_ENABLED")) {
      if (gui_enabled[0] IS '1') return true;
   }

   return access("/mnt/wslg", F_OK) IS 0;
}

#ifdef XDGA_AVAILABLE
APTR glDGAMemory = nullptr;

int x11DGAAvailable(APTR *VideoAddress, int *PixelsPerLine, int *BankSize)
{
   kt::Log log(__FUNCTION__);
   STRING displayname;

   static int checked = true;
   *VideoAddress = NULL;

   if (glX11.WSLg) {
      glDGAAvailable = FALSE;
      glDGAMemory = nullptr;
      glDGAVideo = nullptr;
      glX11.PixelsPerLine = 0;
      glX11.BankSize = 0;

      if (PixelsPerLine) *PixelsPerLine = 0;
      if (BankSize) *BankSize = 0;
      return glDGAAvailable;
   }

   if (glDGAAvailable IS -1) {
      // Check for the DGA driver.  This will only work if the extension is version 2.0+ and we have permissions
      // to map memory.

      glDGAAvailable = FALSE;

      displayname = XDisplayName(nullptr);
      if ((startswith(displayname, ":")) or (startswith(displayname, "unix:")) ) {
         int events, errors, major, minor, screen;

         if (XDGAQueryExtension(XDisplay, &events, &errors) and XDGAQueryVersion(XDisplay, &major, &minor)) {
            screen = DefaultScreen(XDisplay);

            // This part will map the video buffer memory into our process.  Access to /dev/mem is required in order
            // for this to work.  After doing this, superuser privileges are dropped immediately.

            if (!SetResource(RES::PRIVILEGED_USER, TRUE)) {
               if ((major >= 2) and (XDGAOpenFramebuffer(XDisplay, screen))) { // Success, DGA is enabled
                  int ram;

                  // Get RAM address, pixels-per-line, bank-size and total amount of video memory

                  XF86DGAGetVideo(XDisplay, DefaultScreen(XDisplay), (char **)&glDGAMemory, &glX11.PixelsPerLine, &glX11.BankSize, &ram);

                  XDGACloseFramebuffer(XDisplay, screen);
                  glDGAAvailable = TRUE;
               }
               else if (checked <= 1) printf("\033[1mFast video access is not available (driver needs root access)\033[0m\n");

               SetResource(RES::PRIVILEGED_USER, FALSE);

               // Now we permanently drop root capabilities.  The exception to the rule is the desktop executable,
               // which always runs with privileges (indicated via RES::PRIVILEGED).

               if (GetResource(RES::PRIVILEGED) IS FALSE) setuid(getuid());
            }
            else if (checked <= 1) printf("\033[1mFast video access is not available (driver needs root access)\033[0m\n");
         }
         else if (checked <= 1) printf("Fast video access is not available (DGA extension failure).\n");
      }
      else log.warning("DGA is not available (display %s).", displayname);
   }

   if (VideoAddress)  *VideoAddress = glDGAMemory;
   if (PixelsPerLine) *PixelsPerLine = glX11.PixelsPerLine;
   if (BankSize)      *BankSize = glX11.BankSize;
   return glDGAAvailable;
}
#else
int x11DGAAvailable(APTR *VideoAddress, int *PixelsPerLine, int *BankSize)
{
   glDGAAvailable = FALSE;
   return glDGAAvailable;
}
#endif

//********************************************************************************************************************
// This routine is called if there is another window manager running.

XErrorHandler CatchRedirectError(Display *XDisplay, XErrorEvent *event)
{
   kt::Log log("X11");
   log.msg("A window manager has been detected on this X11 server.");
   glX11.Manager = false;
   return 0;
}

//********************************************************************************************************************

const CSTRING glXProtoList[] = { nullptr,
"CreateWindow","ChangeWindowAttributes","GetWindowAttributes","DestroyWindow","DestroySubwindows","ChangeSaveSet","ReparentWindow","MapWindow","MapSubwindows",
"UnmapWindow","UnmapSubwindows","ConfigureWindow","CirculateWindow","GetGeometry","QueryTree","InternAtom","GetAtomName",
"ChangeProperty","DeleteProperty","GetProperty","ListProperties","SetSelectionOwner","GetSelectionOwner","ConvertSelection","SendEvent",
"GrabPointer","UngrabPointer","GrabButton","UngrabButton","ChangeActivePointerGrab","GrabKeyboard","UngrabKeyboard","GrabKey",
"UngrabKey","AllowEvents","GrabServer","UngrabServer","QueryPointer","GetMotionEvents","TranslateCoords","WarpPointer",
"SetInputFocus","GetInputFocus","QueryKeymap","OpenFont","CloseFont","QueryFont","QueryTextExtents","ListFonts",
"ListFontsWithInfo","SetFontPath","GetFontPath","CreatePixmap","FreePixmap","CreateGC","ChangeGC","CopyGC",
"SetDashes","SetClipRectangles","FreeGC","ClearArea","CopyArea","CopyPlane","PolyVertex","PolyLine",
"PolySegment","PolyRectangle","PolyArc","FillPoly","PolyFillRectangle","PolyFillArc","PutImage","GetImage",
"PolyText8","PolyText16","ImageText8","ImageText16","CreateColormap","FreeColormap","CopyColormapAndFree","InstallColormap",
"UninstallColormap","ListInstalledColormaps","AllocColor","AllocNamedColor","AllocColorCells","AllocColorPlanes","FreeColors","StoreColors",
"StoreNamedColor","QueryColors","LookupColor","CreateCursor","CreateGlyphCursor","FreeCursor","RecolorCursor","QueryBestSize",
"QueryExtension","ListExtensions","ChangeKeyboardMapping","GetKeyboardMapping","ChangeKeyboardControl","GetKeyboardControl","Bell","ChangePointerControl",
"GetPointerControl","SetScreenSaver","GetScreenSaver","ChangeHosts","ListHosts","SetAccessControl","SetCloseDownMode","KillClient",
"RotateProperties","ForceScreenSaver","SetPointerMapping","GetPointerMapping","SetModifierMapping","GetModifierMapping","NoOperation"
};

XErrorHandler CatchXError(Display *XDisplay, XErrorEvent *XEvent)
{
   kt::Log log("X11");
   char buffer[80];

   if (XDisplay) {
      XGetErrorText(XDisplay, XEvent->error_code, buffer, sizeof(buffer)-1);
      if ((XEvent->request_code > 0) and (XEvent->request_code < std::ssize(glXProtoList))) {
         log.warning("Function: %s, XError: %s", glXProtoList[XEvent->request_code], buffer);
      }
      else log.warning("Function: Unknown, XError: %s", buffer);
   }
   return 0;
}

//********************************************************************************************************************

int CatchXIOError(Display *XDisplay)
{
   kt::Log log("X11");
   log.error("A fatal XIO error occurred in relation to display \"%s\".", XDisplayName(nullptr));
   return 0;
}

//********************************************************************************************************************
// Resize the pixmap buffer for a window, but only if the new dimensions exceed the existing values.

extern ERR resize_pixmap(extDisplay *Self, int Width, int Height)
{
   auto bmp = (extBitmap *)Self->Bitmap;
   if ((bmp->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) return ERR::Okay; // Composite window

   if ((bmp->x11.pix_width > Width) and (bmp->x11.pix_height > Height)) return ERR::Okay;

   if (Width  > bmp->x11.pix_width)  bmp->x11.pix_width  = Width;
   if (Height > bmp->x11.pix_height) bmp->x11.pix_height = Height;

   auto xbpp = DefaultDepth(XDisplay, DefaultScreen(XDisplay));

   if ((bmp->Flags & BMF::FIXED_DEPTH) != BMF::NIL) xbpp = bmp->BitsPerPixel;

   if (auto pixmap = XCreatePixmap(XDisplay, Self->XWindowHandle, bmp->x11.pix_width, bmp->x11.pix_height, xbpp)) {
      XSetWindowBackgroundPixmap(XDisplay, Self->XWindowHandle, pixmap);
      if (Self->XPixmap) XFreePixmap(XDisplay, Self->XPixmap);
      Self->XPixmap = pixmap;
      bmp->x11.drawable = pixmap;
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

#endif

//********************************************************************************************************************

ERR get_display_info(OBJECTID DisplayID, DisplayInfo *Info)
{
   kt::Log log(__FUNCTION__);

  //log.traceBranch("Display: %d, Info: %p", DisplayID, Info);

   if (not Info) return log.warning(ERR::NullArgs);

   clearmem(Info, sizeof(DisplayInfo));

   if (DisplayID) {
      if (ScopedObjectLock<extDisplay> display(DisplayID, 5000); display.granted()) {
         Info->DisplayID     = DisplayID;
         Info->Flags         = display->Flags;
         Info->Width         = display->Width;
         Info->Height        = display->Height;
         Info->BitsPerPixel  = display->Bitmap->BitsPerPixel;
         Info->BytesPerPixel = display->Bitmap->BytesPerPixel;
         Info->AmtColours    = display->Bitmap->AmtColours;
         GET_HDensity(*display, &Info->HDensity);
         GET_VDensity(*display, &Info->VDensity);
         Info->HostedX       = display->X;
         Info->HostedY       = display->Y;

         #ifdef __xwindows__
            int monitor_x = Info->HostedX + (Info->Width / 2);
            int monitor_y = Info->HostedY + (Info->Height / 2);
            get_x11_display_geometry(Info, monitor_x, monitor_y, true);
            Info->AccelFlags = ACF(-1);
            if (glDGAAvailable IS TRUE) {
               // X11DGA does not provide blitter syncing.
               Info->AccelFlags &= ~ACF::VIDEO_BLIT;
            }
         #elif _WIN32
            Info->AccelFlags = ACF(-1);

            int monitor_x, monitor_y, monitor_width, monitor_height;
            winGetDisplayGeometry(display->WindowHandle,
               Info->MonitorX, Info->MonitorY, Info->MonitorWidth, Info->MonitorHeight,
               Info->VirtualX, Info->VirtualY, Info->VirtualWidth, Info->VirtualHeight,
               Info->PhysicalWidth, Info->PhysicalHeight);

            double refresh_rate = 0;
            winGetDisplaySettings(display->WindowHandle, nullptr, nullptr, nullptr, &refresh_rate);
            if (refresh_rate <= 1.0) {
               refresh_rate = display->RefreshRate;
            }

            if (refresh_rate > 1.0) {
               Info->RefreshRate = float(refresh_rate);
               Info->MinRefresh  = Info->RefreshRate;
               Info->MaxRefresh  = Info->RefreshRate;
            }

         #else
            Info->AccelFlags = ACF(-1);
         #endif

         Info->PixelFormat.RedShift   = display->Bitmap->ColourFormat->RedShift;
         Info->PixelFormat.GreenShift = display->Bitmap->ColourFormat->GreenShift;
         Info->PixelFormat.BlueShift  = display->Bitmap->ColourFormat->BlueShift;
         Info->PixelFormat.AlphaShift = display->Bitmap->ColourFormat->AlphaShift;
         Info->PixelFormat.RedMask    = display->Bitmap->ColourFormat->RedMask;
         Info->PixelFormat.GreenMask  = display->Bitmap->ColourFormat->GreenMask;
         Info->PixelFormat.BlueMask   = display->Bitmap->ColourFormat->BlueMask;
         Info->PixelFormat.AlphaMask  = display->Bitmap->ColourFormat->AlphaMask;
         Info->PixelFormat.RedPos     = display->Bitmap->ColourFormat->RedPos;
         Info->PixelFormat.GreenPos   = display->Bitmap->ColourFormat->GreenPos;
         Info->PixelFormat.BluePos    = display->Bitmap->ColourFormat->BluePos;
         Info->PixelFormat.AlphaPos   = display->Bitmap->ColourFormat->AlphaPos;
         return ERR::Okay;
      }
      else return log.warning(ERR::AccessObject);
   }
   else {
      // If no display is specified, return default display settings for the main monitor and availability flags.

      Info->Flags = SCR::NIL;

#ifdef __xwindows__
      if ((glHeadless) or (!XDisplay)) {
         Info->Width         = 1024;
         Info->Height        = 768;
         Info->AccelFlags    = ACF::NIL;
         Info->VDensity      = 96;
         Info->HDensity      = 96;
         Info->BitsPerPixel  = 32;
         Info->BytesPerPixel = 4;
         Info->MonitorWidth  = Info->Width;
         Info->MonitorHeight = Info->Height;
         Info->VirtualWidth  = Info->Width;
         Info->VirtualHeight = Info->Height;
      }
      else {
         XPixmapFormatValues *list;
         int count, i;

         Info->Width  = glRootWindow.width;
         Info->Height = glRootWindow.height;
         get_x11_display_geometry(Info, 0, 0, false);
         if ((Info->MonitorWidth > 0) and (Info->MonitorHeight > 0)) {
            Info->Width  = int16_t(Info->MonitorWidth);
            Info->Height = int16_t(Info->MonitorHeight);
         }
         Info->AccelFlags = ACF(-1);
         #warning TODO: Get display density
         Info->VDensity = 96;
         Info->HDensity = 96;

         if (glDGAAvailable IS TRUE) {
            Info->AccelFlags &= ~ACF::VIDEO_BLIT; // Turn off video blitting when DGA is active
         }

         Info->BitsPerPixel = DefaultDepth(XDisplay, DefaultScreen(XDisplay));

         if (Info->BitsPerPixel <= 8) Info->BytesPerPixel = 1;
         else if (Info->BitsPerPixel <= 16) Info->BytesPerPixel = 2;
         else if (Info->BitsPerPixel <= 24) Info->BytesPerPixel = 3;
         else Info->BytesPerPixel = 4;

         if ((list = XListPixmapFormats(XDisplay, &count))) {
            for (i=0; i < count; i++) {
               if (list[i].depth IS Info->BitsPerPixel) {
                  Info->BytesPerPixel = list[i].bits_per_pixel;
                  if (list[i].bits_per_pixel <= 8) Info->BytesPerPixel = 1;
                  else if (list[i].bits_per_pixel <= 16) Info->BytesPerPixel = 2;
                  else if (list[i].bits_per_pixel <= 24) Info->BytesPerPixel = 3;
                  else {
                     Info->BytesPerPixel = 4;
                     Info->BitsPerPixel  = 32;
                  }
               }
            }
            XFree(list);
         }
      }

#elif _WIN32

      // TODO: Allow the user to set a custom DPI via style values.

      if (!winGetDisplayGeometry(nullptr,
            Info->MonitorX, Info->MonitorY, Info->MonitorWidth, Info->MonitorHeight,
            Info->VirtualX, Info->VirtualY, Info->VirtualWidth,
            Info->VirtualHeight, Info->PhysicalWidth, Info->PhysicalHeight)) {
         Info->Width  = Info->MonitorWidth;
         Info->Height = Info->MonitorHeight;
      }
      else {
         winGetDesktopSize(&Info->VirtualWidth, &Info->VirtualHeight);
         Info->Width = Info->VirtualWidth;
         Info->Height = Info->VirtualHeight;
      }

      int bits = 0;
      int bytes = 0;
      int colours = 0;
      int hdpi = 96;
      int vdpi = 96;
      double refresh_rate = 0;
      winGetDisplaySettings(nullptr, &bits, &bytes, &colours, &refresh_rate);
      winGetDPI(&hdpi, &vdpi);

      Info->BitsPerPixel  = bits;
      Info->BytesPerPixel = bytes;
      Info->AccelFlags    = ACF(-1);
      Info->HDensity      = hdpi;
      Info->VDensity      = vdpi;
      if (refresh_rate > 1.0) {
         Info->RefreshRate = float(refresh_rate);
         Info->MinRefresh  = Info->RefreshRate;
         Info->MaxRefresh  = Info->RefreshRate;
      }
      if (Info->HDensity < 96) Info->HDensity = 96;
      if (Info->VDensity < 96) Info->VDensity = 96;

#elif __ANDROID__
      // On Android the current display information is always returned.

      log.trace("Refresh");
      if (!adLockAndroid(3000)) {
         ANativeWindow *window;
         if (!adGetWindow(&window)) {
            // TODO: The recommended pixel depth should be determined by analysing the device's CPU capability, the
            // graphics chip and available memory.

            glDisplayInfo.DisplayID     = 0;
            glDisplayInfo.Width         = ANativeWindow_getWidth(window);
            glDisplayInfo.Height        = ANativeWindow_getHeight(window);
            glDisplayInfo.BitsPerPixel  = 16;
            glDisplayInfo.BytesPerPixel = 2;
            glDisplayInfo.AccelFlags    = ACF::VIDEO_BLIT;
            glDisplayInfo.Flags         = SCR::MAXSIZE;  // Indicates that the width and height are the display's maximum.

            AConfiguration *config;
            if (!adGetConfig(&config)) {
               glDisplayInfo.HDensity = AConfiguration_getDensity(config);
               if (glDisplayInfo.HDensity < 60) glDisplayInfo.HDensity = 160;
            }
            else glDisplayInfo.HDensity = 160;

            glDisplayInfo.VDensity = glDisplayInfo.HDensity;

            int pixel_format = ANativeWindow_getFormat(window);
            if ((pixel_format IS WINDOW_FORMAT_RGBA_8888) or (pixel_format IS WINDOW_FORMAT_RGBX_8888)) {
               glDisplayInfo.BytesPerPixel = 32;
               if (pixel_format IS WINDOW_FORMAT_RGBA_8888) glDisplayInfo.BitsPerPixel = 32;
               else glDisplayInfo.BitsPerPixel = 24;
            }

            copymem(&glColourFormat, &glDisplayInfo.PixelFormat, sizeof(glDisplayInfo.PixelFormat));

            if ((glDisplayInfo.BitsPerPixel < 8) or (glDisplayInfo.BitsPerPixel > 32)) {
               if (glDisplayInfo.BitsPerPixel > 32) glDisplayInfo.BitsPerPixel = 32;
               else if (glDisplayInfo.BitsPerPixel < 15) glDisplayInfo.BitsPerPixel = 16;
            }

            if (glDisplayInfo.BitsPerPixel > 24) glDisplayInfo.AmtColours = 1<<24;
            else glDisplayInfo.AmtColours = 1<<glDisplayInfo.BitsPerPixel;

            log.trace("%dx%dx%d", glDisplayInfo.Width, glDisplayInfo.Height, glDisplayInfo.BitsPerPixel);
         }
         else {
            adUnlockAndroid();
            return log.warning(ERR::SystemCall);
         }

         adUnlockAndroid();
      }
      else return log.warning(ERR::TimeOut);

      kt::copymem(&glDisplayInfo, Info, sizeof(DisplayInfo));
      return ERR::Okay;
#else

      if (glDisplayInfo.DisplayID) {
         kt::copymem(&glDisplayInfo, Info, sizeof(DisplayInfo));
         return ERR::Okay;
      }
      else {
         Info->Width         = 1024;
         Info->Height        = 768;
         Info->BitsPerPixel  = 32;
         Info->BytesPerPixel = 4;
         Info->AccelFlags = ACF::SOFTWARE_BLIT;
         Info->HDensity = 96;
         Info->VDensity = 96;
      }
#endif

      Info->PixelFormat.RedShift   = glColourFormat.RedShift;
      Info->PixelFormat.GreenShift = glColourFormat.GreenShift;
      Info->PixelFormat.BlueShift  = glColourFormat.BlueShift;
      Info->PixelFormat.AlphaShift = glColourFormat.AlphaShift;
      Info->PixelFormat.RedMask    = glColourFormat.RedMask;
      Info->PixelFormat.GreenMask  = glColourFormat.GreenMask;
      Info->PixelFormat.BlueMask   = glColourFormat.BlueMask;
      Info->PixelFormat.AlphaMask  = glColourFormat.AlphaMask;
      Info->PixelFormat.RedPos     = glColourFormat.RedPos;
      Info->PixelFormat.GreenPos   = glColourFormat.GreenPos;
      Info->PixelFormat.BluePos    = glColourFormat.BluePos;
      Info->PixelFormat.AlphaPos   = glColourFormat.AlphaPos;

      if ((Info->BitsPerPixel < 8) or (Info->BitsPerPixel > 32)) {
         log.warning("Invalid bpp of %d.", Info->BitsPerPixel);
         if (Info->BitsPerPixel > 32) Info->BitsPerPixel = 32;
         else if (Info->BitsPerPixel < 8) Info->BitsPerPixel = 8;
      }

      if (Info->BitsPerPixel > 24) Info->AmtColours = 1<<24;
      else Info->AmtColours = 1<<Info->BitsPerPixel;

      log.trace("%dx%dx%d", Info->Width, Info->Height, Info->BitsPerPixel);
      return ERR::Okay;
   }
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log(__FUNCTION__);

   #ifdef __xwindows__
      int shmmajor, shmminor, pixmaps;
   #endif

   CoreBase = argCoreBase;
   glDisplayContext = CurrentContext();

   glModule = (OBJECTPTR)((objModule *)argModule)->Root;

   if (objModule::load("regex", &modRegex, &RegexBase) != ERR::Okay) return ERR::InitModule;

#ifndef KOTUKU_STATIC

   if (GetSystemState()->Stage < 0) { // An early load indicates that classes are being probed, so just return them.
      glHeadless = true;
      create_pointer_class();
      create_display_class();
      create_bitmap_class();
      create_clipboard_class();
      create_surface_class();
      create_controller_class();
      return ERR::Okay;
   }
#endif

   if (auto driver_name = (CSTRING)GetResourcePtr(RES::DISPLAY_DRIVER)) {
      log.msg("User requested display driver '%s'", driver_name);
      if ((iequals(driver_name, "none")) or (iequals(driver_name, "headless"))) {
         glHeadless = true;
      }
   }

   // Register a fake FD as input_event_loop() so that we can process input events on every ProcessMessages() cycle.

   RegisterFD((HOSTHANDLE)-2, RFD::ALWAYS_CALL, input_event_loop, nullptr);

   #ifdef _GLES_
      pthread_mutexattr_t attr;
      pthread_mutexattr_init(&attr);
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); // Allow recursive use of lock_graphics()

      pthread_mutex_init(&glGraphicsMutex, &attr);
   #endif

   #ifdef __ANDROID__
      if (GetResource(RES::SYSTEM_STATE) >= 0) {
         if (objModule::load("android", (OBJECTPTR *)&modAndroid, &AndroidBase) != ERR::Okay) return ERR::InitModule;

         FUNCTION fInitWindow, fTermWindow;
         SET_CALLBACK_STDC(fInitWindow, &android_init_window); // Sets EGL for re-initialisation and draws the display.
         SET_CALLBACK_STDC(fTermWindow, &android_term_window); // Frees EGL

         if (adAddCallbacks(ACB_INIT_WINDOW, &fInitWindow,
                            ACB_TERM_WINDOW, &fTermWindow,
                            TAGEND) != ERR::Okay) {
            return ERR::SystemCall;
         }
      }
   #endif

   glDisplayInfo.DisplayID = 0xffffffff; // Indicate a refresh of the cache is required.

#ifdef __xwindows__
   if (!glHeadless) {
      // Attempt to open X11.  Use KOTUKU_XDISPLAY if set, otherwise use the DISPLAY variable.

      CSTRING strdisplay = getenv("KOTUKU_XDISPLAY");
      if (!strdisplay) strdisplay = getenv("DISPLAY");

      if ((XDisplay = XOpenDisplay(strdisplay))) {
         // Select the X messages that we want to receive from the root window.  This will also tell us if an X11 manager
         // is currently running or not (refer to the CatchRedirectError() exception routine).

         glX11.WSLg = detect_wslg();

         if (glX11.WSLg) {
            glX11.Manager = false;
         }
         else {
            XSetErrorHandler((XErrorHandler)CatchRedirectError);

            XSelectInput(XDisplay, RootWindow(XDisplay, DefaultScreen(XDisplay)),
               LeaveWindowMask|EnterWindowMask|PointerMotionMask|
               PropertyChangeMask|SubstructureRedirectMask| // SubstructureNotifyMask |
               KeyPressMask|ButtonPressMask|ButtonReleaseMask);

            XSync(XDisplay, 0);
         }

         if (!getenv("KOTUKU_XDISPLAY")) setenv("KOTUKU_XDISPLAY", strdisplay, FALSE);

         XSetErrorHandler((XErrorHandler)CatchXError);
         XSetIOErrorHandler(CatchXIOError);
      }
      else return ERR::SystemCall;

      // Get the X11 file descriptor (for incoming events) and tell the Core to listen to it when the task is sleeping.
      // Using ALWAYS_CALL here causes X11ManagerLoop() to run on every message-loop cycle, even when there are no X11
      // events pending.  That can keep the UI thread active indefinitely.  A READ subscription is sufficient because
      // X11ManagerLoop() drains the queued events whenever the X11 connection becomes readable.

      glXFD = XConnectionNumber(XDisplay);
      fcntl(glXFD, F_SETFD, 1); // FD does not duplicate across exec()
      RegisterFD(glXFD, RFD::READ, X11ManagerLoop, nullptr);

      // This function checks for DGA and also maps the video memory for us

      glDGAAvailable = x11DGAAvailable(&glDGAVideo, &glDGAPixelsPerLine, &glDGABankSize);

      log.msg("DGA Enabled: %d", glDGAAvailable);

      // Create the graphics contexts for drawing directly to X11 windows

      XGCValues gcv;
      gcv.function = GXcopy;
      gcv.graphics_exposures = False;
      glXGC = XCreateGC(XDisplay, DefaultRootWindow(XDisplay), GCGraphicsExposures|GCFunction, &gcv);

      gcv.function = GXcopy;
      gcv.graphics_exposures = False;
      glClipXGC = XCreateGC(XDisplay, DefaultRootWindow(XDisplay), GCGraphicsExposures|GCFunction, &gcv);

      #ifdef USE_XIMAGE
         if (XShmQueryVersion(XDisplay, &shmmajor, &shmminor, &pixmaps)) {
            log.msg("X11 shared image extension is active.");
            glX11ShmImage = true;
         }
      #endif

      if (!glX11.Manager) { // We are a client of X11
//         XSelectInput(XDisplay, RootWindow(XDisplay, DefaultScreen(XDisplay)), NULL);
      }

      C_Default = XCreateFontCursor(XDisplay, XC_left_ptr);

      XWADeleteWindow = XInternAtom(XDisplay, "WM_DELETE_WINDOW", False);
      XWATakeFocus    = XInternAtom(XDisplay, "WM_TAKE_FOCUS", False);
      atomSurfaceID   = XInternAtom(XDisplay, "KOTUKU_SCREENID", False);

      XGetWindowAttributes(XDisplay, DefaultRootWindow(XDisplay), &glRootWindow);

      if (XMatchVisualInfo(XDisplay, DefaultScreen(XDisplay), 32, TrueColor, &glXInfoAlpha)) {
         glXCompositeSupported = true;
      }
      else glXCompositeSupported = false;

      clearmem(KeyHeld, sizeof(KeyHeld));

      // Drop superuser privileges following X11 initialisation (we only need suid for DGA).

      seteuid(getuid());

      init_xcursors();

      // Set the DISPLAY variable for clients to :10, which is the default X11 display for the rootless X Server.

      if (glX11.Manager) setenv("DISPLAY", ":10", TRUE);

#ifdef XRANDR_ENABLED
      int16_t i;
      XRRScreenSize *sizes;
      XPixmapFormatValues *list;
      int errors, count;
      char buffer[512];

      int events;
      if ((not glX11.WSLg) and (glX11.Manager) and (XRRQueryExtension(XDisplay, &events, &errors))) {
         glXRRAvailable = true;
         if ((sizes = XRRSizes(XDisplay, DefaultScreen(XDisplay), &count)) and (count)) {
            glSizes = sizes;
            glSizeCount = count;
         }
         else log.msg("XRRSizes() failed.");

         // Build the screen.xml file if this is the first task to initialise the RandR extension.

         auto file = objFile::create { fl::Path("user:config/screen.xml"), fl::Flags(FL::NEW|FL::WRITE) };

         if (file.ok()) {
            auto write_string = [](objFile *File, CSTRING String) {
               struct acWrite write = { .Buffer = String, .Length = int(strlen(String)) };
               Action(AC::Write, File, &write);
            };

            write_string(*file, "<?xml version=\"1.0\"?>\n\n");
            write_string(*file, "<displayinfo>\n");
            write_string(*file, "  <manufacturer value=\"XFree86\"/>\n");
            write_string(*file, "  <chipset value=\"X11\"/>\n");
            write_string(*file, "  <dac value=\"N/A\"/>\n");
            write_string(*file, "  <clock value=\"N/A\"/>\n");
            write_string(*file, "  <version value=\"1.00\"/>\n");
            write_string(*file, "  <certified value=\"February 2023\"/>\n");
            write_string(*file, "  <monitor_mfr value=\"Unknown\"/>\n");
            write_string(*file, "  <monitor_model value=\"Unknown\"/>\n");
            write_string(*file, "  <scanrates minhscan=\"0\" maxhscan=\"0\" minvscan=\"0\" maxvscan=\"0\"/>\n");
            write_string(*file, "  <gfx_output unknown/>\n");
            write_string(*file, "</displayinfo>\n\n");

            int16_t xbpp = DefaultDepth(XDisplay, DefaultScreen(XDisplay));

            int16_t xbytes;
            if (xbpp <= 8) xbytes = 1;
            else if (xbpp <= 16) xbytes = 2;
            else if (xbpp <= 24) xbytes = 3;
            else xbytes = 4;

            if ((list = XListPixmapFormats(XDisplay, &count))) {
               for (i=0; i < count; i++) {
                  if (list[i].depth IS xbpp) {
                     xbytes = list[i].bits_per_pixel;
                     if (list[i].bits_per_pixel <= 8) xbytes = 1;
                     else if (list[i].bits_per_pixel <= 16) xbytes = 2;
                     else if (list[i].bits_per_pixel <= 24) xbytes = 3;
                     else xbytes = 4;
                  }
               }
            }

            if (xbytes IS 4) xbpp = 32;

            int xcolours;
            switch(xbpp) {
               case 1:  xcolours = 2; break;
               case 8:  xcolours = 256; break;
               case 15: xcolours = 32768; break;
               case 16: xcolours = 65536; break;
               default: xcolours = 16777216; break;
            }

            for (i=0; i < glSizeCount; i++) {
               if ((glSizes[i].width >= 640) and (glSizes[i].height >= 480)) {
                  snprintf(buffer, sizeof(buffer), "<screen name=\"%dx%d\" width=\"%d\" height=\"%d\" depth=\"%d\" colours=\"%d\"\n",
                     glSizes[i].width, glSizes[i].height, glSizes[i].width, glSizes[i].height, xbpp, xcolours);
                  write_string(*file, buffer);

                  snprintf(buffer, sizeof(buffer), "  bytes=\"%d\" defaultrefresh=\"0\" minrefresh=\"0\" maxrefresh=\"0\">\n", xbytes);
                  write_string(*file, buffer);

                  write_string(*file, "</screen>\n\n");
               }
            }
         }
      }
      else log.msg("XRRQueryExtension() failed.");
#endif // XRANDR_ENABLED

   }

#elif _WIN32

   if ((glInstance = winGetModuleHandle())) {
      if (!winCreateScreenClass()) return log.warning(ERR::SystemCall);
   }
   else return log.warning(ERR::SystemCall);

   winDisableBatching();

   winInitCursors(winCursors, std::ssize(winCursors));

#endif

   if (create_pointer_class() != ERR::Okay) return log.warning(ERR::AddClass);
   if (create_display_class() != ERR::Okay) return log.warning(ERR::AddClass);
   if (create_bitmap_class() != ERR::Okay) return log.warning(ERR::AddClass);
   if (create_clipboard_class() != ERR::Okay) return log.warning(ERR::AddClass);
   if (create_surface_class() != ERR::Okay) return log.warning(ERR::AddClass);
   if (create_controller_class() != ERR::Okay) return log.warning(ERR::AddClass);

   // Initialise 64K alpha blending table, for cutting down on multiplications.

   int i = 0;
   for (int16_t iAlpha=0; iAlpha < 256; iAlpha++) {
      double fAlpha = (double)iAlpha * (1.0 / 255.0);
      for (int16_t iValue=0; iValue < 256; iValue++) {
         glAlphaLookup[i++] = clipByte(std::lrint((double)iValue * fAlpha));
      }
   }

   glDisplayType = gfx::GetDisplayType();

#ifdef __ANDROID__
      glpFullScreen = TRUE;
      glpDisplayDepth = 16;

      DisplayInfo *info;
      if (!gfxGetDisplayInfo(0, &info)) {
         glpDisplayWidth  = info.Width;
         glpDisplayHeight = info.Height;
         glpDisplayDepth  = info.BitsPerPixel;
      }
#else
   if (auto config = objConfig::create { fl::Path("user:config/display.cfg") }; config.ok()) {
      config->read("DISPLAY", "Maximise", glpMaximise);

      if ((glDisplayType IS DT::X11) or (glDisplayType IS DT::WINGDI)) {
         log.msg("Using hosted window dimensions: %dx%d,%dx%d", glpDisplayX, glpDisplayY, glpDisplayWidth, glpDisplayHeight);
         if ((config->read("DISPLAY", "WindowWidth", glpDisplayWidth) != ERR::Okay) or (!glpDisplayWidth)) {
            config->read("DISPLAY", "Width", glpDisplayWidth);
         }

         if ((config->read("DISPLAY", "WindowHeight", glpDisplayHeight) != ERR::Okay) or (!glpDisplayHeight)) {
            config->read("DISPLAY", "Height", glpDisplayHeight);
         }

         config->read("DISPLAY", "WindowX", glpDisplayX);
         config->read("DISPLAY", "WindowY", glpDisplayY);
         config->read("DISPLAY", "FullScreen", glpFullScreen);
      }
      else {
         config->read("DISPLAY", "Width", glpDisplayWidth);
         config->read("DISPLAY", "Height", glpDisplayHeight);
         config->read("DISPLAY", "XCoord", glpDisplayX);
         config->read("DISPLAY", "YCoord", glpDisplayY);
         config->read("DISPLAY", "Depth", glpDisplayDepth);
         log.msg("Using default display dimensions: %dx%d,%dx%d", glpDisplayX, glpDisplayY, glpDisplayWidth, glpDisplayHeight);
      }

      config->read("DISPLAY", "RefreshRate", glpRefreshRate);
      config->read("DISPLAY", "GammaRed", glpGammaRed);
      config->read("DISPLAY", "GammaGreen", glpGammaGreen);
      config->read("DISPLAY", "GammaBlue", glpGammaBlue);

      std::string dpms;
      if (!config->read("DISPLAY", "DPMS", dpms)) {
         strcopy(dpms, glpDPMS, sizeof(glpDPMS));
      }
   }
#endif

   // Icons are stored in compressed archives, accessible via "archive:icons/<category>/<icon>.svg"

   std::string icon_path;
   if (ResolvePath("iconsource:", RSF::NIL, &icon_path) != ERR::Okay) { // The client can set iconsource: to redefine the icon origins
      icon_path = "styles:icons/";
   }

   auto src = icon_path + "Default.zip";
   if ((glIconArchive = objCompression::create::local(fl::Path(src), fl::ArchiveName("icons"), fl::Flags(CMF::READ_ONLY)))) {
      // The icons: special volume is a simple reference to the archive path.
      if (SetVolume("icons", "archive:icons/", "misc/picture", "", "", VOLUME::REPLACE|VOLUME::HIDDEN) != ERR::Okay) return ERR::SetVolume;
   }

#ifdef _WIN32 // Get any existing Windows clipboard content

   log.branch("Populating clipboard for the first time from the Windows host.");
   winCopyClipboard();
   log.debranch();

#endif

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODOpen(OBJECTPTR Module)
{
   ((objModule *)Module)->setFunctionList(glFunctions);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   kt::Log log(__FUNCTION__);
   ERR error = ERR::Okay;

   if (clDisplay) {
      clean_clipboard();
      {
         const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);
         glClips.clear();
      }
   }

   if (glRefreshPointerTimer) { UpdateTimer(glRefreshPointerTimer, 0); glRefreshPointerTimer = 0; }
   if (glComposite)           { FreeResource(glComposite); glComposite = nullptr; }
   if (glCompress)            { FreeResource(glCompress); glCompress = nullptr; }
   if (glDemultiply)          { FreeResource(glDemultiply); glDemultiply = nullptr; }

   DeregisterFD((HOSTHANDLE)-2); // Disable input_event_loop()

#ifdef __xwindows__

   if (!glHeadless) {
      if (modXRR) { FreeResource(modXRR); modXRR = nullptr; }

      if (glXFD != -1) { DeregisterFD(glXFD); glXFD = -1; }

      XSetErrorHandler(nullptr);
      XSetIOErrorHandler(nullptr);

      if (XDisplay) {
         free_xcursors();

         if (glXGC) { XFreeGC(XDisplay, glXGC); glXGC = 0; }
         if (glClipXGC) { XFreeGC(XDisplay, glClipXGC); glClipXGC = 0; }

         // Closing the display causes a crash, so we're not doing it anymore ...
         /*
         xtmp = XDisplay;
         XDisplay = nullptr;
         XCloseDisplay(xtmp);
         */
      }

      // Note: In full-screen mode, expunging of the display module causes segfaults right at the end of program
      // termination.  In order to resolve this problem we return DoNotExpunge to prevent the removal of X11
      // dependent code.

      error = ERR::DoNotExpunge;
   }

#elif __ANDROID__

   if (modAndroid) {
      FUNCTION fInitWindow, fTermWindow;
      SET_CALLBACK_STDC(fInitWindow, &android_init_window);
      SET_CALLBACK_STDC(fTermWindow, &android_term_window);

      adRemoveCallbacks(ACB_INIT_WINDOW, &fInitWindow,
                        ACB_TERM_WINDOW, &fTermWindow,
                        TAGEND);

      FreeResource(modAndroid);
      modAndroid = nullptr;
   }

#elif _WIN32

   winRemoveWindowClass("ScreenClass");
   winTerminate();

#endif

   if (glIconArchive) { FreeResource(glIconArchive); glIconArchive = nullptr; }
   if (clPointer)     { FreeResource(clPointer);     clPointer     = nullptr; }
   if (clDisplay)     { FreeResource(clDisplay);     clDisplay     = nullptr; }
   if (clBitmap)      { FreeResource(clBitmap);      clBitmap      = nullptr; }
   if (clClipboard)   { FreeResource(clClipboard);   clClipboard   = nullptr; }
   if (clSurface)     { FreeResource(clSurface);     clSurface     = nullptr; }
   if (clController)  { FreeResource(clController);  clController  = nullptr; }
   if (modRegex)      { FreeResource(modRegex);      modRegex      = nullptr; }

   #ifdef _GLES_
      free_egl();
      pthread_mutex_destroy(&glGraphicsMutex);
   #endif

   return error;
}

/*********************************************************************************************************************
** Use this function to allocate simple 2D OpenGL textures.  It configures the texture so that it is suitable for basic
** rendering operations.  Note that the texture will still be bound on returning.
*/

#ifdef _GLES_
GLenum alloc_texture(int Width, int Height, GLuint *TextureID)
{
   GLenum glerror;

   glGenTextures(1, TextureID); // Generate a new texture ID
   glBindTexture(GL_TEXTURE_2D, TextureID[0]); // Target the new texture bank

   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // Filter for minification, GL_LINEAR is smoother than GL_NEAREST
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); // Filter for magnification
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // Texture wrap behaviour
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // Texture wrap behaviour

   glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

   if ((glerror = glGetError()) IS GL_NO_ERROR) {
      GLint crop[4] = { 0, Height, Width, -Height };
      glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop); // This is for glDrawTex*OES

      glerror = glGetError();
      if (glerror != GL_NO_ERROR) log.warning("glTexParameteriv() error: %d", glerror);
   }
   else log.warning("glTexEnvf() error: %d", glerror);

   return glerror;
}
#endif

#ifdef _GLES_
/*********************************************************************************************************************
** This function is designed so that it can be re-called in case the OpenGL display needs to be reset.  THIS FUNCTION
** REQUIRES THAT THE GRAPHICS MUTEX IS LOCKED.
**
** PLEASE NOTE: EGL's design for embedded devices means that only one Display object can be active at any time.
*/

ERR init_egl(void)
{
   kt::Log log(__FUNCTION__);
   EGLint format;
   int depth;

   log.branch("Requested Depth: %d", glEGLPreferredDepth);

   if (glEGLDisplay != EGL_NO_DISPLAY) {
      log.msg("EGL display is already initialised.");
      return ERR::Okay;
   }

   depth = glEGLPreferredDepth;
   if (depth < 16) depth = 16;

   glEGLRefreshDisplay = TRUE; // The active Display will need to refresh itself because the width/height/depth that EGL provides may differ from that desired.
   glEGLDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);

   eglInitialize(glEGLDisplay, 0, 0);

   // Here, the application chooses the configuration it desires. In this sample, we have a very simplified selection
   // process, where we pick the first EGLConfig that matches our criteria

   EGLint attribs[20];
   int a = 0;
   attribs[a++] = EGL_SURFACE_TYPE; attribs[a++] = EGL_WINDOW_BIT;
   attribs[a++] = EGL_BLUE_SIZE;    attribs[a++] = (depth IS 16) ? 5 : 8;
   attribs[a++] = EGL_GREEN_SIZE;   attribs[a++] = (depth IS 16) ? 6 : 8;
   attribs[a++] = EGL_RED_SIZE;     attribs[a++] = (depth IS 16) ? 5 : 8;
   attribs[a++] = EGL_DEPTH_SIZE;   attribs[a++] = 0; // Turns off 3D depth buffer if zero
   attribs[a++] = EGL_NONE;

   EGLConfig config;
   EGLint numConfigs;
   eglChooseConfig(glEGLDisplay, attribs, &config, 1, &numConfigs);

   // EGL_NATIVE_VISUAL_ID is an attribute of the EGLConfig that is guaranteed to be accepted by ANativeWindow_setBuffersGeometry().
   // As soon as we picked a EGLConfig, we can safely reconfigure the ANativeWindow buffers to match, using EGL_NATIVE_VISUAL_ID.

   int redsize, greensize, bluesize, alphasize, bufsize;
   eglGetConfigAttrib(glEGLDisplay, config, EGL_NATIVE_VISUAL_ID, &format);
   eglGetConfigAttrib(glEGLDisplay, config, EGL_RED_SIZE, &redsize);
   eglGetConfigAttrib(glEGLDisplay, config, EGL_GREEN_SIZE, &greensize);
   eglGetConfigAttrib(glEGLDisplay, config, EGL_BLUE_SIZE, &bluesize);
   eglGetConfigAttrib(glEGLDisplay, config, EGL_ALPHA_SIZE, &alphasize);
   eglGetConfigAttrib(glEGLDisplay, config, EGL_BUFFER_SIZE, &bufsize);
   glEGLDepth = bufsize; //redsize + greensize + bluesize + alphasize;

   ANativeWindow *window;
   if (!adGetWindow(&window)) {
      ANativeWindow_setBuffersGeometry(window, 0, 0, format);
      glEGLSurface = eglCreateWindowSurface(glEGLDisplay, config, window, nullptr);
      glEGLContext = eglCreateContext(glEGLDisplay, config, nullptr, nullptr);
   }
   else {
      log.warning(ERR::SystemCall);
      return ERR::SystemCall;
   }

   if (eglMakeCurrent(glEGLDisplay, glEGLSurface, glEGLSurface, glEGLContext) == EGL_FALSE) {
      log.warning(ERR::SystemCall);
      return ERR::SystemCall;
   }

   eglQuerySurface(glEGLDisplay, glEGLSurface, EGL_WIDTH, &glEGLWidth);
   eglQuerySurface(glEGLDisplay, glEGLSurface, EGL_HEIGHT, &glEGLHeight);

   log.trace("Actual width and height set by EGL: %dx%dx%d", glEGLWidth, glEGLHeight, glEGLDepth);

   glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
   //glEnable(GL_CULL_FACE);
   glClearColorx(0, 0, 0, 0xffff); // Default background colour.
   glShadeModel(GL_SMOOTH); // Switching from GL_SMOOTH to GL_FLAT gives more performance and 2D pixel accuracy
   glEnable(GL_BLEND); // Enable alpha blending.
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glDisable(GL_DEPTH_TEST); // Disabling depth test is good for 2D only
   glEnable(GL_TEXTURE_2D);
   //glDisable(GL_DITHER); // Dithering affects performance slightly if converting 24/32-bit to 16-bit, but quality is then an issue
   glDisable(GL_LIGHTING); // Improves performance for 2D
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glDisplayInfo.DisplayID = 0xffffffff; // Force refresh of display info cache.

   if (!glPointerID) {
      FindObject("SystemPointer", 0, &glPointerID);
   }

   if (glPointerID) {
      AConfiguration *config;
      objPointer *pointer;
      if (!adGetConfig(&config)) {
         double dp_factor = 160.0 / AConfiguration_getDensity(config);
         if (!AccessObject(glPointerID, 3000, &pointer)) {
            pointer->ClickSlop = F2I(8.0 * dp_factor);
            log.msg("Click-slop calculated as %d.", pointer->ClickSlop);
            ReleaseObject(pointer);
         }
         else log.warning(ERR::AccessObject);
      }
      else log.warning("Failed to get Android Config object.");
   }

   glEGLState = EGL_INITIALISED;
   return ERR::Okay;
}

//********************************************************************************************************************

void refresh_display_from_egl(extDisplay *Self)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("%dx%dx%d", glEGLWidth, glEGLHeight, glEGLDepth);

   glEGLRefreshDisplay = FALSE;

   Self->Width = glEGLWidth;
   Self->Height = glEGLHeight;

   ANativeWindow *window;
   if (!adGetWindow(&window)) {
      Self->WindowHandle = window;
   }

   // If the display's bitmap depth / size needs to change, resize it here.

   if (Self->Bitmap->Head.Flags & NF::INITIALISED) {
      if ((Self->Width != Self->Bitmap->Width) or (Self->Height != Self->Bitmap->Height)) {
         log.trace("Resizing OpenGL representative bitmap to match new dimensions.");
         acResize(Self->Bitmap, glEGLWidth, glEGLHeight, glEGLDepth);
      }
   }
}

/*********************************************************************************************************************
** Free EGL resources.  This does not relate to hiding or switch off of the display - in fact the display can remain
** active as it normally does.  For this reason, we just focus on resource deallocation.
*/

void free_egl(void)
{
   kt::Log log(__FUNCTION__);

   log.branch("Current Display: $%x", (int)glEGLDisplay);

   if (!pthread_mutex_lock(&glGraphicsMutex)) {
      log.msg("Lock granted - terminating EGL resources.");
      glEGLState = EGL_TERMINATED;

      if (glEGLDisplay != EGL_NO_DISPLAY) {
         eglMakeCurrent(glEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         if (glEGLContext != EGL_NO_CONTEXT) eglDestroyContext(glEGLDisplay, glEGLContext);
         if (glEGLSurface != EGL_NO_SURFACE) eglDestroySurface(glEGLDisplay, glEGLSurface);
         eglTerminate(glEGLDisplay);
      }

      glEGLDisplay = EGL_NO_DISPLAY;
      glEGLContext = EGL_NO_CONTEXT;
      glEGLSurface = EGL_NO_SURFACE;

      pthread_mutex_unlock(&glGraphicsMutex);
   }
   else log.warning(ERR::LockFailed);

   log.msg("EGL successfully terminated.");
}
#endif

//********************************************************************************************************************
// Updates the display using content from a source bitmap.

ERR update_display(extDisplay *Self, extBitmap *Bitmap, int X, int Y, int Width, int Height, int XDest, int YDest)
{
#ifdef _WIN32
   auto dest   = Self->Bitmap;
   auto x      = X;
   auto y      = Y;
   auto width  = Width;
   auto height = Height;
   auto xdest  = XDest;
   auto ydest  = YDest;

   // Check if the destination that we are copying to is within the drawable area.

   if ((xdest < dest->Clip.Left)) {
      width = width - (dest->Clip.Left - xdest);
      if (width < 1) return ERR::Okay;
      x = x + (dest->Clip.Left - xdest);
      xdest = dest->Clip.Left;
   }
   else if (xdest >= dest->Clip.Right) return ERR::Okay;

   if ((ydest < dest->Clip.Top)) {
      height = height - (dest->Clip.Top - ydest);
      if (height < 1) return ERR::Okay;
      y = y + (dest->Clip.Top - ydest);
      ydest = dest->Clip.Top;
   }
   else if (ydest >= dest->Clip.Bottom) return ERR::Okay;

   // Check if the source that we are copying from is within its own drawable area.

   if (x < 0) {
      if ((width += x) < 1) return ERR::Okay;
      x = 0;
   }
   else if (x >= Bitmap->Width) return ERR::Okay;

   if (y < 0) {
      if ((height += y) < 1) return ERR::Okay;
      y = 0;
   }
   else if (y >= Bitmap->Height) return ERR::Okay;

   // Clip the Width and Height

   if ((xdest + width)  >= dest->Clip.Right)  width  = dest->Clip.Right - xdest;
   if ((ydest + height) >= dest->Clip.Bottom) height = dest->Clip.Bottom - ydest;

   if ((x + width)  >= Bitmap->Width)  width  = Bitmap->Width - x;
   if ((y + height) >= Bitmap->Height) height = Bitmap->Height - y;

   if (width < 1) return ERR::Okay;
   if (height < 1) return ERR::Okay;

   // Adjust coordinates by offset values

   APTR drawable;
   dest->getHandle(drawable);

   win32RedrawWindow(Self->WindowHandle, drawable,
      x, y, width, height, xdest, ydest,
      Bitmap->Width, Bitmap->Height,
      Bitmap->BitsPerPixel, Bitmap->Data,
      Bitmap->ColourFormat->RedMask   << Bitmap->ColourFormat->RedPos,
      Bitmap->ColourFormat->GreenMask << Bitmap->ColourFormat->GreenPos,
      Bitmap->ColourFormat->BlueMask  << Bitmap->ColourFormat->BluePos,
      ((Self->Flags & SCR::COMPOSITE) != SCR::NIL) ? (Bitmap->ColourFormat->AlphaMask << Bitmap->ColourFormat->AlphaPos) : 0,
      Self->Opacity);
   return ERR::Okay;
#else
   return(gfx::CopyArea(Bitmap, (extBitmap *)Self->Bitmap, BAF::NIL, X, Y, Width, Height, XDest, YDest));
#endif
}

//********************************************************************************************************************

#ifdef __xwindows__
#include "x11/handlers.cpp"
#endif

#ifdef _WIN32
#include "win32/handlers.cpp"
#endif

#ifdef __ANDROID__
#include "android/android.cpp"
#endif

//********************************************************************************************************************

static ModHeader::STRUCTS glStructures = {
   { "BitmapSurface", { sizeof(BitmapSurface), alignof(BitmapSurface) } },
   { "CursorInfo",    { sizeof(CursorInfo),    alignof(CursorInfo)    } },
   { "DisplayInfo",   { sizeof(DisplayInfo),   alignof(DisplayInfo)   } },
   { "PixelFormat",   { sizeof(PixelFormat),   alignof(PixelFormat)   } },
   { "SurfaceCoords", { sizeof(SurfaceCoords), alignof(SurfaceCoords) } },
   { "SurfaceInfo",   { sizeof(SurfaceInfo),   alignof(SurfaceInfo)   } }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_display_module() { return &ModHeader; }
