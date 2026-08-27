#include "display_driver.h"
#include <kotuku/modules/display.h>

CSTRING HeadlessDriver::name() const { return "headless"; }
DT HeadlessDriver::displayType() const { return DT::NATIVE; }
DCAP HeadlessDriver::capabilities() const { return DCAP::NIL; }
ERR HeadlessDriver::isAvailable() const { return ERR::Okay; }

ERR HeadlessDriver::open(const DriverCallbacks &Callbacks)
{
   if (Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;
   Open = true;
   return ERR::Okay;
}

ERR HeadlessDriver::close()
{
   Open = false;
   return ERR::Okay;
}

ERR HeadlessDriver::createWindow(extDisplay *, HOSTWINDOW &) { return ERR::NoSupport; }
ERR HeadlessDriver::adoptWindow(extDisplay *, APTR, HOSTWINDOW &) { return ERR::NoSupport; }
ERR HeadlessDriver::nativeWindowHandle(HOSTWINDOW, APTR &) { return ERR::NoSupport; }
ERR HeadlessDriver::destroyWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::showWindow(HOSTWINDOW, bool) { return ERR::NoSupport; }
ERR HeadlessDriver::hideWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::focusWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::moveWindow(HOSTWINDOW, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::resizeWindow(HOSTWINDOW, int, int, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::raiseWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::lowerWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::minimiseWindow(HOSTWINDOW) { return ERR::NoSupport; }
ERR HeadlessDriver::setWindowTitle(HOSTWINDOW, CSTRING) { return ERR::NoSupport; }
ERR HeadlessDriver::setSizeHints(HOSTWINDOW, int, int, int, int, bool) { return ERR::NoSupport; }
ERR HeadlessDriver::windowCoords(HOSTWINDOW, int &, int &, int &, int &) { return ERR::NoSupport; }
ERR HeadlessDriver::frameMargins(HOSTWINDOW, int &, int &, int &, int &) { return ERR::NoSupport; }
ERR HeadlessDriver::displayInfo(DisplayInfo &) { return ERR::NoSupport; }
ERR HeadlessDriver::density(HOSTWINDOW, int &, int &) { return ERR::NoSupport; }
ERR HeadlessDriver::resolutions(std::vector<resolution> &) { return ERR::NoSupport; }
ERR HeadlessDriver::setDisplayMode(int &, int &, int &, double) { return ERR::NoSupport; }
ERR HeadlessDriver::setGamma(double, double, double) { return ERR::NoSupport; }
ERR HeadlessDriver::setPowerMode(DPMS) { return ERR::NoSupport; }
ERR HeadlessDriver::pixelFormat(ColourFormat &) { return ERR::NoSupport; }
ERR HeadlessDriver::present(HOSTWINDOW, extBitmap *, int, int, int, int, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::blitBitmap(extBitmap *, extBitmap *, BAF, int, int, int, int, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::fillBitmap(extBitmap *, int, int, int, int, uint32_t) { return ERR::NoSupport; }
ERR HeadlessDriver::flush() { return ERR::Okay; }
ERR HeadlessDriver::allocBitmap(extBitmap *) { return ERR::NoSupport; }
ERR HeadlessDriver::freeBitmap(extBitmap *) { return ERR::NoSupport; }
ERR HeadlessDriver::resizeBitmap(extBitmap *, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::lockBitmap(extBitmap *) { return ERR::NoSupport; }
ERR HeadlessDriver::unlockBitmap(extBitmap *) { return ERR::NoSupport; }
ERR HeadlessDriver::bitmapRoutines(extBitmap *) { return ERR::NoSupport; }
ERR HeadlessDriver::setCursor(PTC) { return ERR::NoSupport; }
ERR HeadlessDriver::setCustomCursor(extBitmap *, int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::showCursor(bool) { return ERR::NoSupport; }
ERR HeadlessDriver::warpPointer(int, int) { return ERR::NoSupport; }
ERR HeadlessDriver::pointerPosition(double &, double &) { return ERR::NoSupport; }
