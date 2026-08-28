/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

#include "defs.h"
#include <kotuku/modules/module.h>

JUMPTABLE_REGEX

#ifdef _WIN32
using namespace display;
#endif

ERR GET_HDensity(extDisplay *Self, int *Value);
ERR GET_VDensity(extDisplay *Self, int *Value);

//********************************************************************************************************************

std::array<uint8_t, 256 * 256> glAlphaLookup;


#ifdef _WIN32
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


std::recursive_mutex glInputLock;

objCompression *glCompress = nullptr;
static objCompression *glIconArchive = nullptr;
struct CoreBase *CoreBase;
ColourFormat glColourFormat;
bool glHeadless = false;
DisplayDriver *glDriver = nullptr;
static HeadlessDriver glHeadlessDriver;
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
std::unique_ptr<std::array<uint16_t, 256 * 256>> glDemultiply;
std::atomic<int> glLastPort = -1;
#ifndef _WIN32
uint8_t glTrayIcon = 0, glTaskBar = 0, glStickToFront = 0;
#endif

std::vector<OBJECTID> glFocusList;
std::recursive_mutex glFocusLock;
std::recursive_mutex glSurfaceLock;

namespace display {
void DriverKeyPress(KQ Flags, KEY Value, int Printable);
void DriverKeyRelease(KQ Flags, KEY Value);
void DriverMovement(OBJECTID SurfaceID, double AbsX, double AbsY, bool NonClient);
void DriverWheelMovement(OBJECTID SurfaceID, float Wheel);
void DriverButtonInput(int Buttons, bool State);
void DriverCrossing(OBJECTID SurfaceID, bool Entered, double AbsX, double AbsY);
void DriverFocusState(OBJECTID SurfaceID, bool State);
void DriverWindowResized(OBJECTID SurfaceID, int WinX, int WinY, int WinWidth, int WinHeight,
   int ClientX, int ClientY, int ClientWidth, int ClientHeight);
void DriverExposeRegion(OBJECTID SurfaceID, int X, int Y, int Width, int Height);
ERR DriverWindowClose(OBJECTID SurfaceID);
void DriverWindowDestroyed(OBJECTID SurfaceID);
void DriverDPIChanged(OBJECTID SurfaceID);
void DriverSetFocus(OBJECTID SurfaceID);
void DriverClipboardUpdated();
void DriverDragDropped(OBJECTID SurfaceID, CSTRING Datatypes);
void DriverControllerPorts(int Port, bool Connected, int Total);
OBJECTID DriverResolveSurface(APTR HostHandle);
void DriverConstrainWindowSize(OBJECTID SurfaceID, int &Width, int &Height, int CurrentWidth, int CurrentHeight,
   int Axis);
void DriverProcessMessages();
}

const DriverCallbacks glDriverCallbacks = {
   .Version = DISPLAY_DRIVER_INTERFACE_VERSION,
   .KeyPressed = display::DriverKeyPress,
   .KeyReleased = display::DriverKeyRelease,
   .Movement = display::DriverMovement,
   .WheelMovement = display::DriverWheelMovement,
   .ButtonInput = display::DriverButtonInput,
   .Crossing = display::DriverCrossing,
   .FocusState = display::DriverFocusState,
   .WindowResized = display::DriverWindowResized,
   .ExposeRegion = display::DriverExposeRegion,
   .WindowClose = display::DriverWindowClose,
   .WindowDestroyed = display::DriverWindowDestroyed,
   .DPIChanged = display::DriverDPIChanged,
   .SetFocus = display::DriverSetFocus,
   .ClipboardUpdated = display::DriverClipboardUpdated,
   .DragDropped = display::DriverDragDropped,
   .ControllerPorts = display::DriverControllerPorts,
   .ResolveSurface = display::DriverResolveSurface,
   .ConstrainWindowSize = display::DriverConstrainWindowSize,
   .ProcessMessages = display::DriverProcessMessages
};

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
   Self->Resolutions.clear();
   if ((not glDriver) or (glDriver->resolutions(Self->Resolutions) != ERR::Okay)) {
      Self->Resolutions = {
         { 640, 480, 32 }, { 800, 600, 32 }, { 1024, 768, 32 }, { 1152, 864, 32 }, { 1280, 960, 32 }
      };
   }
}

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

         if (glDriver) glDriver->displayInfo(*Info);

            Info->AccelFlags = ACF(-1);

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

      if (glDriver) {
         if (auto error = glDriver->displayInfo(*Info); error IS ERR::NoSupport) {
            Info->Width = 1024;
            Info->Height = 768;
            Info->BitsPerPixel = 32;
            Info->BytesPerPixel = 4;
            Info->AccelFlags = ACF::NIL;
            Info->HDensity = 96;
            Info->VDensity = 96;
            Info->MonitorWidth = Info->Width;
            Info->MonitorHeight = Info->Height;
            Info->VirtualWidth = Info->Width;
            Info->VirtualHeight = Info->Height;
         }
      }

#if   __ANDROID__
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
      if (not glDriver) {
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

   struct InitGuard {
      bool Complete = false;

      ~InitGuard() {
         if ((not Complete) and (glDriver)) {
            glDriver->close();
            glDriver = nullptr;
            glHeadless = false;
         }
      }
   } init_guard;


   CoreBase = argCoreBase;
   glDriver = nullptr;
   glHeadless = false;
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
         glDriver = &glHeadlessDriver;
         if (auto error = glDriver->open(glDriverCallbacks); error != ERR::Okay) {
            glDriver->close();
            glDriver = nullptr;
            glHeadless = false;
            return error;
         }
      }
      #ifdef _WIN32
      else if (iequals(driver_name, "windows")) {
         glDriver = display::get_win32_driver();
         if (auto error = glDriver->open(glDriverCallbacks); error != ERR::Okay) {
            glDriver = nullptr;
            return error;
         }
      }
      else return log.warning(ERR::NoSupport);
      #endif
      #ifdef __linux__
      else if (iequals(driver_name, "x11")) {
         glDriver = display::get_x11_driver();
         if (auto error = glDriver->open(glDriverCallbacks); error != ERR::Okay) {
            glDriver = nullptr;
            return error;
         }
      }
      else return log.warning(ERR::NoSupport);
      #endif
   }

   #ifdef _WIN32
      if (not glDriver) {
         glDriver = display::get_win32_driver();
         if (auto error = glDriver->open(glDriverCallbacks); error != ERR::Okay) {
            glDriver = nullptr;
            return error;
         }
      }
   #endif

   #if defined(__linux__)
   #ifndef __ANDROID__
      if (not glDriver) {
         glDriver = display::get_x11_driver();
         if (auto error = glDriver->isAvailable(); error != ERR::Okay) {
            glDriver = nullptr;
            return error;
         }
         if (auto error = glDriver->open(glDriverCallbacks); error != ERR::Okay) {
            glDriver = nullptr;
            return error;
         }
      }
   #endif
   #endif

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


   // Register input dispatch after platform event sources so that newly queued input is handled before the task sleeps.

   RegisterFD((HOSTHANDLE)-2, RFD::ALWAYS_CALL, input_event_loop, nullptr);

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

   init_guard.Complete = true;
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
   if (glDriver) {
      error = glDriver->close();
      glDriver = nullptr;
      glHeadless = false;
   }

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
   glDemultiply.reset();

   DeregisterFD((HOSTHANDLE)-2); // Disable input_event_loop()

#if   __ANDROID__

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
   winTerminateClipboard();
   winTerminateOLE();

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
   if ((not glDriver) or (glHeadless)) {
      return gfx::CopyArea(Bitmap, (extBitmap *)Self->Bitmap, BAF::NIL, X, Y, Width, Height, XDest, YDest);
   }

   auto dest   = (extBitmap *)Self->Bitmap;
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

   return glDriver->present(Self->WindowHandle, Bitmap, x, y, width, height, xdest, ydest);
}

//********************************************************************************************************************


#include "driver/callbacks.cpp"

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
