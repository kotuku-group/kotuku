#pragma once

#include "../../driver/display_driver.h"

namespace display {

class AndroidDriver final : public HeadlessDriver {
public:
   struct State;

   AndroidDriver();
   ~AndroidDriver() override;
   bool valid() const;

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
   ERR windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height) override;
   ERR displayInfo(DisplayInfo &Info) override;
   ERR density(HOSTWINDOW Window, int &Horizontal, int &Vertical) override;
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

   void nativeWindowInitialised();
   void nativeWindowTerminated();

private:
   State *Data;
};

}
