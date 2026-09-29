#pragma once

#include "../../driver/display_driver.h"

namespace display {

class WaylandDriver final : public HeadlessDriver {
public:
   struct State;
   State *Data;
   WaylandDriver();
   ~WaylandDriver() override;
   CSTRING name() const override;
   DT displayType() const override;
   DCAP capabilities() const override;
   ERR isAvailable() const override;
   ERR open(const DriverCallbacks &Callbacks) override;
   ERR close() override;
   ERR createWindow(extDisplay *Display, HOSTWINDOW &Handle) override;
   ERR destroyWindow(HOSTWINDOW Window) override;
   ERR nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle) override;
   ERR showWindow(HOSTWINDOW Window, bool Maximise) override;
   ERR hideWindow(HOSTWINDOW Window) override;
   ERR resizeWindow(HOSTWINDOW Window, int X, int Y, int Width, int Height) override;
   ERR minimiseWindow(HOSTWINDOW Window) override;
   ERR setFullscreen(HOSTWINDOW Window, bool Enabled) override;
   ERR normalWindowSize(HOSTWINDOW Window, int &Width, int &Height) override;
   ERR setWindowTitle(HOSTWINDOW Window, CSTRING Title) override;
   ERR setSizeHints(HOSTWINDOW Window, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect) override;
   ERR windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height) override;
   ERR frameMargins(HOSTWINDOW Window, int &Left, int &Top, int &Right, int &Bottom) override;
   ERR setWindowSurface(HOSTWINDOW Window, OBJECTID SurfaceID) override;
   ERR windowSurface(HOSTWINDOW Window, OBJECTID &SurfaceID) override;
   ERR windowTitle(HOSTWINDOW Window, std::string &Title) override;
   ERR displayInfo(DisplayInfo &Info) override;
   ERR density(HOSTWINDOW Window, int &Horizontal, int &Vertical) override;
   ERR resolutions(std::vector<resolution> &List) override;
   ERR pixelFormat(ColourFormat &Format) override;
   ERR present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
      int XDest, int YDest) override;
   ERR flush() override;
   ERR setCursor(HOSTWINDOW Window, PTC CursorID) override;
   ERR setCustomCursor(HOSTWINDOW Window, extBitmap *Image, int HotX, int HotY) override;
   ERR showCursor(HOSTWINDOW Window, bool Visible) override;
   ERR warpPointer(HOSTWINDOW Window, int X, int Y) override;
   ERR pointerPosition(double &X, double &Y) override;
   ERR grabPointer(HOSTWINDOW Window) override;
   ERR ungrabPointer() override;
   ERR clipboardAddText(CSTRING Text) override;
   ERR clipboardAddFiles(CLIPTYPE Type, const std::vector<std::string> &Paths, bool Cut) override;
   ERR clipboardClear() override;
};

}
