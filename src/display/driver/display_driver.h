#pragma once

#include <kotuku/config.h>
#include <kotuku/system/errors.h>

#include <cstdint>
#include <string>
#include <vector>

class extBitmap;
class extDisplay;
enum class BAF : uint32_t;
enum class CLIPTYPE : uint32_t;
enum class CON : uint32_t;
enum class DPMS : int;
enum class DT : int;
enum class HOST : int;
enum class KEY : int;
enum class KQ : uint32_t;
enum class PTC : int;
struct ColourFormat;
struct DisplayInfo;
struct resolution;

constexpr int DISPLAY_DRIVER_INTERFACE_VERSION = 4;

enum class DCAP : uint64_t {
   NIL             = 0,
   WINDOW_POSITION = 0x00000001,
   STACKING        = 0x00000002,
   POINTER_WARP    = 0x00000004,
   MODE_SWITCH     = 0x00000008,
   GAMMA           = 0x00000010,
   DPMS            = 0x00000020,
   CLIPBOARD       = 0x00000040,
   DRAG_DROP       = 0x00000080,
   CONTROLLERS     = 0x00000100,
   COMPOSITING     = 0x00000200,
   CUSTOM_CURSORS  = 0x00000400,
   VIDEO_BITMAPS   = 0x00000800,
   DESKTOP_MANAGER = 0x00001000,
   WINDOW_DECOR    = 0x00002000
};

constexpr DCAP operator|(DCAP A, DCAP B) { return DCAP(uint64_t(A) | uint64_t(B)); }
constexpr DCAP operator&(DCAP A, DCAP B) { return DCAP(uint64_t(A) & uint64_t(B)); }
constexpr DCAP operator~(DCAP A) { return DCAP(~uint64_t(A)); }
constexpr DCAP operator^(DCAP A, DCAP B) { return DCAP(uint64_t(A) ^ uint64_t(B)); }
constexpr DCAP &operator|=(DCAP &A, DCAP B) { return A = A | B; }
constexpr DCAP &operator&=(DCAP &A, DCAP B) { return A = A & B; }

using HOSTWINDOW = APTR;

// The module owns this immutable table and keeps it alive until the driver has closed.  Driver callbacks may be
// invoked only while open() is active.  Callback implementations acquire any object and global locks they require.
struct DriverCallbacks {
   int Version;
   void (*KeyPressed)(KQ Qualifiers, KEY Code, int Unicode);
   void (*KeyReleased)(KQ Qualifiers, KEY Code);
   void (*Movement)(OBJECTID SurfaceID, double AbsX, double AbsY, bool NonClient);
   void (*WheelMovement)(OBJECTID SurfaceID, float Delta);
   void (*ButtonInput)(int Buttons, bool Pressed);
   void (*Crossing)(OBJECTID SurfaceID, bool Entered, double AbsX, double AbsY);
   void (*FocusState)(OBJECTID SurfaceID, bool Focused);
   void (*WindowResized)(OBJECTID SurfaceID, int WindowX, int WindowY, int WindowWidth, int WindowHeight,
      int ClientX, int ClientY, int ClientWidth, int ClientHeight);
   void (*ExposeRegion)(OBJECTID SurfaceID, int X, int Y, int Width, int Height);
   ERR (*WindowClose)(OBJECTID SurfaceID);
   void (*WindowDestroyed)(OBJECTID SurfaceID);
   void (*DPIChanged)(OBJECTID SurfaceID);
   void (*SetFocus)(OBJECTID SurfaceID);
   void (*ClipboardUpdated)();
   void (*DragDropped)(OBJECTID SurfaceID, CSTRING Datatypes);
   void (*ControllerPorts)(int Port, bool Connected, int Total);
   OBJECTID (*ResolveSurface)(APTR HostHandle);
   void (*ConstrainWindowSize)(OBJECTID SurfaceID, int &Width, int &Height, int CurrentWidth, int CurrentHeight,
      int Axis);
   void (*ProcessMessages)();
};

class DisplayDriver {
public:
   virtual ~DisplayDriver() = default;

   virtual CSTRING name() const = 0;
   virtual DT displayType() const = 0;
   virtual DCAP capabilities() const = 0;
   virtual ERR isAvailable() const = 0;
   virtual ERR open(const DriverCallbacks &Callbacks) = 0;
   virtual ERR close() = 0;

   virtual ERR createWindow(extDisplay *Display, HOSTWINDOW &Handle) = 0;
   virtual ERR adoptWindow(extDisplay *Display, APTR NativeHandle, HOSTWINDOW &Handle) = 0;
   virtual ERR nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle) = 0;
   virtual ERR destroyWindow(HOSTWINDOW Window) = 0;
   virtual ERR showWindow(HOSTWINDOW Window, bool Maximise) = 0;
   virtual ERR hideWindow(HOSTWINDOW Window) = 0;
   virtual ERR focusWindow(HOSTWINDOW Window) = 0;
   virtual ERR moveWindow(HOSTWINDOW Window, int X, int Y) = 0;
   virtual ERR resizeWindow(HOSTWINDOW Window, int X, int Y, int Width, int Height) = 0;
   virtual ERR raiseWindow(HOSTWINDOW Window) = 0;
   virtual ERR lowerWindow(HOSTWINDOW Window) = 0;
   virtual ERR minimiseWindow(HOSTWINDOW Window) = 0;
   virtual ERR setWindowTitle(HOSTWINDOW Window, CSTRING Title) = 0;
   virtual ERR setSizeHints(HOSTWINDOW Window, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect) = 0;
   virtual ERR windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height) = 0;
   virtual ERR frameMargins(HOSTWINDOW Window, int &Left, int &Top, int &Right, int &Bottom) = 0;

   // Transitional helpers used while the platform drivers are extracted from the common Surface and Display code.
   // They keep native host operations behind the driver without exposing native types in this contract.

   virtual ERR windowTitle(HOSTWINDOW Window, std::string &Title) { return ERR::NoSupport; }
   virtual ERR setWindowSurface(HOSTWINDOW Window, OBJECTID SurfaceID) { return ERR::NoSupport; }
   virtual ERR windowSurface(HOSTWINDOW Window, OBJECTID &SurfaceID) { return ERR::NoSupport; }
   virtual ERR setWindowControllers(HOSTWINDOW Window, bool Enabled) { return ERR::NoSupport; }
   virtual ERR acquireWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap) { return ERR::NoSupport; }
   virtual ERR releaseWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap) { return ERR::NoSupport; }

   virtual ERR displayInfo(DisplayInfo &Info) = 0;
   virtual ERR density(HOSTWINDOW Window, int &Horizontal, int &Vertical) = 0;
   virtual ERR resolutions(std::vector<resolution> &List) = 0;
   virtual ERR setDisplayMode(int &Width, int &Height, int &BitsPerPixel, double RefreshRate) = 0;
   virtual ERR setGamma(double Red, double Green, double Blue) = 0;
   virtual ERR setPowerMode(DPMS Mode) = 0;
   virtual ERR pixelFormat(ColourFormat &Format) = 0;

   virtual ERR present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
      int XDest, int YDest) = 0;
   virtual ERR blitBitmap(extBitmap *Destination, extBitmap *Source, BAF Flags, int X, int Y, int Width,
      int Height, int XDest, int YDest) = 0;
   virtual ERR fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour) = 0;
   virtual ERR flush() = 0;

   virtual ERR allocBitmap(extBitmap *Bitmap) = 0;
   virtual ERR freeBitmap(extBitmap *Bitmap) = 0;
   virtual ERR resizeBitmap(extBitmap *Bitmap, int Width, int Height) = 0;

   // Access is a mask of SURFACE_READ and SURFACE_WRITE.  Drivers must copy the host surface into the bitmap's
   // data area only when SURFACE_READ is requested, because the read-back is expensive and the caller may intend
   // to overwrite the content in full.

   virtual ERR lockBitmap(extBitmap *Bitmap, int16_t Access) = 0;
   virtual ERR unlockBitmap(extBitmap *Bitmap) = 0;
   virtual ERR bitmapRoutines(extBitmap *Bitmap) = 0;

   virtual ERR setCursor(HOSTWINDOW Window, PTC CursorID) = 0;
   virtual ERR setCustomCursor(HOSTWINDOW Window, extBitmap *Image, int HotX, int HotY) = 0;
   virtual ERR showCursor(HOSTWINDOW Window, bool Visible) = 0;
   virtual ERR warpPointer(HOSTWINDOW Window, int X, int Y) = 0;
   virtual ERR pointerPosition(double &X, double &Y) = 0;
   virtual ERR grabPointer(HOSTWINDOW Window) { return ERR::NoSupport; }
   virtual ERR ungrabPointer() { return ERR::NoSupport; }

   virtual ERR setHostOption(HOST Option, int64_t Value) { return ERR::NoSupport; }
   virtual ERR readController(int Port, double *Axes, CON &Buttons) { return ERR::NoSupport; }
   virtual ERR totalControllerPorts(int &Total) { return ERR::NoSupport; }
   virtual ERR clipboardAddText(CSTRING Text) { return ERR::NoSupport; }
   virtual ERR clipboardAddFiles(CLIPTYPE Type, const std::vector<std::string> &Paths, bool Cut) {
      return ERR::NoSupport;
   }
   virtual ERR clipboardClear() { return ERR::NoSupport; }
   virtual ERR clipboardRead() { return ERR::NoSupport; }
};

class HeadlessDriver : public DisplayDriver {
public:
   CSTRING name() const override;
   DT displayType() const override;
   DCAP capabilities() const override;
   ERR isAvailable() const override;
   ERR open(const DriverCallbacks &Callbacks) override;
   ERR close() override;

   ERR createWindow(extDisplay *Display, HOSTWINDOW &Handle) override;
   ERR adoptWindow(extDisplay *Display, APTR NativeHandle, HOSTWINDOW &Handle) override;
   ERR nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle) override;
   ERR destroyWindow(HOSTWINDOW Window) override;
   ERR showWindow(HOSTWINDOW Window, bool Maximise) override;
   ERR hideWindow(HOSTWINDOW Window) override;
   ERR focusWindow(HOSTWINDOW Window) override;
   ERR moveWindow(HOSTWINDOW Window, int X, int Y) override;
   ERR resizeWindow(HOSTWINDOW Window, int X, int Y, int Width, int Height) override;
   ERR raiseWindow(HOSTWINDOW Window) override;
   ERR lowerWindow(HOSTWINDOW Window) override;
   ERR minimiseWindow(HOSTWINDOW Window) override;
   ERR setWindowTitle(HOSTWINDOW Window, CSTRING Title) override;
   ERR setSizeHints(HOSTWINDOW Window, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect) override;
   ERR windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height) override;
   ERR frameMargins(HOSTWINDOW Window, int &Left, int &Top, int &Right, int &Bottom) override;
   ERR displayInfo(DisplayInfo &Info) override;
   ERR density(HOSTWINDOW Window, int &Horizontal, int &Vertical) override;
   ERR resolutions(std::vector<resolution> &List) override;
   ERR setDisplayMode(int &Width, int &Height, int &BitsPerPixel, double RefreshRate) override;
   ERR setGamma(double Red, double Green, double Blue) override;
   ERR setPowerMode(DPMS Mode) override;
   ERR pixelFormat(ColourFormat &Format) override;
   ERR present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
      int XDest, int YDest) override;
   ERR blitBitmap(extBitmap *Destination, extBitmap *Source, BAF Flags, int X, int Y, int Width,
      int Height, int XDest, int YDest) override;
   ERR fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour) override;
   ERR flush() override;
   ERR allocBitmap(extBitmap *Bitmap) override;
   ERR freeBitmap(extBitmap *Bitmap) override;
   ERR resizeBitmap(extBitmap *Bitmap, int Width, int Height) override;
   ERR lockBitmap(extBitmap *Bitmap, int16_t Access) override;
   ERR unlockBitmap(extBitmap *Bitmap) override;
   ERR bitmapRoutines(extBitmap *Bitmap) override;
   ERR setCursor(HOSTWINDOW Window, PTC CursorID) override;
   ERR setCustomCursor(HOSTWINDOW Window, extBitmap *Image, int HotX, int HotY) override;
   ERR showCursor(HOSTWINDOW Window, bool Visible) override;
   ERR warpPointer(HOSTWINDOW Window, int X, int Y) override;
   ERR pointerPosition(double &X, double &Y) override;

private:
   bool Open = false;
};

#ifdef _WIN32
namespace display {
DisplayDriver * get_win32_driver();
}
#endif

#ifdef __linux__
namespace display {
DisplayDriver * get_x11_driver();
}
#endif
