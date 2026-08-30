#pragma once

#include "../../driver/display_driver.h"

namespace display {

class X11Driver final : public DisplayDriver {
public:
   struct State;

   X11Driver();
   ~X11Driver() override;

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
   ERR windowTitle(HOSTWINDOW Window, std::string &Title) override;
   ERR setWindowSurface(HOSTWINDOW Window, OBJECTID SurfaceID) override;
   ERR windowSurface(HOSTWINDOW Window, OBJECTID &SurfaceID) override;
   ERR acquireWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap) override;

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
   ERR grabPointer(HOSTWINDOW Window) override;
   ERR ungrabPointer() override;
   ERR setHostOption(HOST Option, int64_t Value) override;

   void processEvents();

private:
   State *Data;
};

}
