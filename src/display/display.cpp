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

std::recursive_mutex glInputLock;

objCompression *glCompress = nullptr;
static objCompression *glIconArchive = nullptr;
struct CoreBase *CoreBase;
ColourFormat glColourFormat;
bool glHeadless = false;
DisplayDriver *glDriver = nullptr;
static HeadlessDriver glHeadlessDriver;

enum class DriverSource { NIL, BUILT_IN, MODULE, STATIC };

struct DriverOwnership {
   DisplayDriver *Driver = nullptr;
   OBJECTPTR Module = nullptr;
   DestroyDisplayDriver Destroy = nullptr;
   DriverSource Source = DriverSource::NIL;
   bool Opened = false;
   std::string CanonicalName;
};

static DriverOwnership glDriverOwnership;
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
ERR DriverEnableDragDrop(APTR HostHandle);
void DriverDisableDragDrop(APTR HostHandle);
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
   .EnableDragDrop = display::DriverEnableDragDrop,
   .DisableDragDrop = display::DriverDisableDragDrop,
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

static CSTRING driver_source_name(DriverSource Source)
{
   if (Source IS DriverSource::BUILT_IN) return "built-in";
   if (Source IS DriverSource::MODULE) return "module";
   if (Source IS DriverSource::STATIC) return "static";
   return "none";
}

static ERR release_driver(bool Expunging)
{
   auto close_error = ERR::Okay;
   if (glDriverOwnership.Driver and glDriverOwnership.Opened) {
      close_error = glDriverOwnership.Driver->close();
      glDriverOwnership.Opened = false;
   }
   if (glDriverOwnership.Driver and glDriverOwnership.Destroy) {
      glDriverOwnership.Destroy(glDriverOwnership.Driver);
   }
   glDriverOwnership.Driver = nullptr;
   glDriverOwnership.Destroy = nullptr;
   glDriver = nullptr;
   glHeadless = false;

   if (glDriverOwnership.Module) {
      if ((not Expunging) or (close_error != ERR::DoNotExpunge)) {
         FreeResource(glDriverOwnership.Module);
         glDriverOwnership.Module = nullptr;
      }
   }

   if (not glDriverOwnership.Module) glDriverOwnership = {};
   else {
      glDriverOwnership.Source = DriverSource::MODULE;
      glDriverOwnership.CanonicalName.clear();
   }
   return close_error;
}

struct DriverCandidate {
   CSTRING CanonicalName;
   CSTRING ModuleName;
#ifdef KOTUKU_STATIC
   CreateDisplayDriver Create;
   DestroyDisplayDriver Destroy;
#endif
};

static ERR open_driver_candidate(const DriverCandidate &Candidate, bool Explicit)
{
   kt::Log log(__FUNCTION__);
   DriverOwnership pending;
   pending.CanonicalName = Candidate.CanonicalName;
   log.msg("Considering display driver '%s'.", Candidate.CanonicalName);

   if (iequals(Candidate.CanonicalName, "headless")) {
      pending.Driver = &glHeadlessDriver;
      pending.Source = DriverSource::BUILT_IN;
   }
   else {
#ifdef KOTUKU_STATIC
      pending.Source = DriverSource::STATIC;
      pending.Destroy = Candidate.Destroy;
      if ((not Candidate.Create) or (not Candidate.Destroy)) return ERR::NoSupport;
      auto core = CoreBase ? CoreBase : (struct CoreBase *)(uintptr_t)1;
      pending.Driver = Candidate.Create(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#else
      pending.Source = DriverSource::MODULE;
      auto error = objModule::load(Candidate.ModuleName, &pending.Module);
      if (error != ERR::Okay) {
         log.warning("Failed to load display driver module '%s': %s.", Candidate.ModuleName, GetErrorMsg(error));
         return error;
      }
      APTR create_address = nullptr;
      APTR destroy_address = nullptr;
      if ((((objModule *)pending.Module)->resolveSymbol("create_display_driver", &create_address) != ERR::Okay) or
            (((objModule *)pending.Module)->resolveSymbol("destroy_display_driver", &destroy_address) != ERR::Okay) or
            (not create_address) or (not destroy_address)) {
         log.warning("Display driver module '%s' does not export the required factory ABI.", Candidate.ModuleName);
         FreeResource(pending.Module);
         return ERR::ResolveSymbol;
      }
      auto create = CreateDisplayDriver(create_address);
      pending.Destroy = DestroyDisplayDriver(destroy_address);
      pending.Driver = create(DISPLAY_DRIVER_INTERFACE_VERSION, CoreBase);
#endif
      if (not pending.Driver) {
         if (pending.Module) FreeResource(pending.Module);
         return ERR::WrongVersion;
      }
   }

   if (not iequals(pending.Driver->name(), Candidate.CanonicalName)) {
      log.warning("Display driver identity does not match candidate '%s'.", Candidate.CanonicalName);
      if (pending.Destroy) pending.Destroy(pending.Driver);
      if (pending.Module) FreeResource(pending.Module);
      return ERR::InvalidData;
   }

   auto available = pending.Driver->isAvailable();
   log.msg("Display driver '%s' availability result: %s.", Candidate.CanonicalName, GetErrorMsg(available));
   if ((not Explicit) and (available != ERR::Okay)) {
      if (pending.Destroy) pending.Destroy(pending.Driver);
      if (pending.Module) FreeResource(pending.Module);
      return available;
   }

   auto error = pending.Driver->open(glDriverCallbacks);
   log.msg("Display driver '%s' open result: %s.", Candidate.CanonicalName, GetErrorMsg(error));
   if (error != ERR::Okay) {
      if (pending.Destroy) pending.Destroy(pending.Driver);
      if (pending.Module) FreeResource(pending.Module);
      return error;
   }

   pending.Opened = true;
   glDriverOwnership = std::move(pending);
   glDriver = glDriverOwnership.Driver;
   glHeadless = glDriverOwnership.Source IS DriverSource::BUILT_IN;
   log.msg("Selected display driver '%s' from %s.", glDriverOwnership.CanonicalName.c_str(),
      driver_source_name(glDriverOwnership.Source));
   return ERR::Okay;
}

static ERR select_display_driver(CSTRING RequestedName)
{
   if (RequestedName) {
      if ((iequals(RequestedName, "headless")) or (iequals(RequestedName, "none"))) {
         const DriverCandidate candidate = { "headless", nullptr
#ifdef KOTUKU_STATIC
            , nullptr, nullptr
#endif
         };
         return open_driver_candidate(candidate, true);
      }
#if defined(DISPLAY_X11_DRIVER)
      if (iequals(RequestedName, "x11")) {
         const DriverCandidate candidate = { "x11", "display-x11"
#ifdef KOTUKU_STATIC
            , display::create_x11_display_driver, display::destroy_x11_display_driver
#endif
         };
         return open_driver_candidate(candidate, true);
      }
#endif
#if defined(DISPLAY_WINDOWS_DRIVER)
      if (iequals(RequestedName, "windows")) {
         const DriverCandidate candidate = { "windows", "display-windows"
#ifdef KOTUKU_STATIC
            , display::create_win32_display_driver, display::destroy_win32_display_driver
#endif
         };
         return open_driver_candidate(candidate, true);
      }
#endif
#if defined(DISPLAY_ANDROID_DRIVER)
      if (iequals(RequestedName, "android")) {
         const DriverCandidate candidate = { "android", "display-android"
#ifdef KOTUKU_STATIC
            , display::create_android_display_driver, display::destroy_android_display_driver
#endif
         };
         return open_driver_candidate(candidate, true);
      }
#endif
      return ERR::NoSupport;
   }

#if defined(DISPLAY_X11_DRIVER)
   const DriverCandidate candidates[] = {{ "x11", "display-x11"
#ifdef KOTUKU_STATIC
      , display::create_x11_display_driver, display::destroy_x11_display_driver
#endif
   }};
#endif
#if defined(DISPLAY_WINDOWS_DRIVER)
   const DriverCandidate candidates[] = {{ "windows", "display-windows"
#ifdef KOTUKU_STATIC
      , display::create_win32_display_driver, display::destroy_win32_display_driver
#endif
   }};
#endif
#if defined(DISPLAY_ANDROID_DRIVER)
   const DriverCandidate candidates[] = {{ "android", "display-android"
#ifdef KOTUKU_STATIC
      , display::create_android_display_driver, display::destroy_android_display_driver
#endif
   }};
#endif

   auto error = ERR::NoSupport;
#if defined(DISPLAY_X11_DRIVER) or defined(DISPLAY_WINDOWS_DRIVER) or defined(DISPLAY_ANDROID_DRIVER)
   for (const auto &candidate : candidates) {
      error = open_driver_candidate(candidate, false);
      if (error IS ERR::Okay) return error;
   }
#endif
   return error;
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   kt::Log log(__FUNCTION__);

   struct InitGuard {
      bool Complete = false;
#ifdef _WIN32
      bool ClipboardInitialised = false;
#endif

      ~InitGuard() {
         if (not Complete) {
            release_driver(false);
#ifdef _WIN32
            if (ClipboardInitialised) winTerminateClipboard();
#endif
         }
      }
   } init_guard;


   CoreBase = argCoreBase;
   release_driver(false);
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

#ifdef _WIN32
   winInitialiseClipboard();
   init_guard.ClipboardInitialised = true;
#endif

   if (auto driver_name = (CSTRING)GetResourcePtr(RES::DISPLAY_DRIVER)) {
      log.trace("User requested display driver '%s'", driver_name);
      if (auto error = select_display_driver(driver_name); error != ERR::Okay) return log.warning(error);
   }
   else {
      log.trace("Selecting the display driver automatically.");
      if (auto error = select_display_driver(nullptr); error != ERR::Okay) return error;
   }

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

   if ((glDriver) and (glDriver->displayType() IS DT::GLES)) {
      glpFullScreen = TRUE;
      glpDisplayDepth = 16;

      DisplayInfo info;
      if (get_display_info(0, &info) IS ERR::Okay) {
         glpDisplayWidth  = info.Width;
         glpDisplayHeight = info.Height;
         glpDisplayDepth  = info.BitsPerPixel;
      }
   }
   else {
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
   }

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
   ERR error = release_driver(true);

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

#if _WIN32
   winTerminateClipboard();

#endif

   if (glIconArchive) { FreeResource(glIconArchive); glIconArchive = nullptr; }
   if (clPointer)     { FreeResource(clPointer);     clPointer     = nullptr; }
   if (clDisplay)     { FreeResource(clDisplay);     clDisplay     = nullptr; }
   if (clBitmap)      { FreeResource(clBitmap);      clBitmap      = nullptr; }
   if (clClipboard)   { FreeResource(clClipboard);   clClipboard   = nullptr; }
   if (clSurface)     { FreeResource(clSurface);     clSurface     = nullptr; }
   if (clController)  { FreeResource(clController);  clController  = nullptr; }
   if (modRegex)      { FreeResource(modRegex);      modRegex      = nullptr; }

   return error;
}

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
