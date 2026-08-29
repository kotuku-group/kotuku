#include "../../defs.h"

#include "controller.h"
#include "windows.h"

#include <cstdlib>
#include <new>
#include <unordered_map>

HINSTANCE glInstance = nullptr;

namespace display {

extern const DriverCallbacks *glWin32Callbacks;

static WinCursor glWin32Cursors[24] = {
   { nullptr, PTC::DEFAULT },
   { nullptr, PTC::SIZE_BOTTOM_LEFT },
   { nullptr, PTC::SIZE_BOTTOM_RIGHT },
   { nullptr, PTC::SIZE_TOP_LEFT },
   { nullptr, PTC::SIZE_TOP_RIGHT },
   { nullptr, PTC::SIZE_LEFT },
   { nullptr, PTC::SIZE_RIGHT },
   { nullptr, PTC::SIZE_TOP },
   { nullptr, PTC::SIZE_BOTTOM },
   { nullptr, PTC::CROSSHAIR },
   { nullptr, PTC::SLEEP },
   { nullptr, PTC::SIZING },
   { nullptr, PTC::SPLIT_VERTICAL },
   { nullptr, PTC::SPLIT_HORIZONTAL },
   { nullptr, PTC::MAGNIFIER },
   { nullptr, PTC::HAND },
   { nullptr, PTC::HAND_LEFT },
   { nullptr, PTC::HAND_RIGHT },
   { nullptr, PTC::TEXT },
   { nullptr, PTC::PAINTBRUSH },
   { nullptr, PTC::STOP },
   { nullptr, PTC::INVISIBLE },
   { nullptr, PTC::INVISIBLE },
   { nullptr, PTC::DRAGGABLE }
};

static void win32_colour_component(int SourceMask, uint8_t &Mask, uint8_t &Position, uint8_t &Shift)
{
   Position = 0;
   Shift = 0;
   while (SourceMask and (not (SourceMask & 1))) {
      SourceMask >>= 1;
      Position++;
   }
   Mask = SourceMask;
   for (int bit = 0x80; bit and (not (bit & Mask)); bit >>= 1) Shift++;
}

static void win32_colour_format(ColourFormat &Format, int BitsPerPixel, int RedMask, int GreenMask, int BlueMask,
   int AlphaMask)
{
   win32_colour_component(RedMask, Format.RedMask, Format.RedPos, Format.RedShift);
   win32_colour_component(GreenMask, Format.GreenMask, Format.GreenPos, Format.GreenShift);
   win32_colour_component(BlueMask, Format.BlueMask, Format.BluePos, Format.BlueShift);
   win32_colour_component(AlphaMask, Format.AlphaMask, Format.AlphaPos, Format.AlphaShift);
   Format.BitsPerPixel = BitsPerPixel;
}

#include "lib_pixels.cpp"

class Win32Driver final : public DisplayDriver {
public:
   CSTRING name() const override { return "windows"; }
   DT displayType() const override { return DT::WINGDI; }
   DCAP capabilities() const override;
   ERR isAvailable() const override { return ERR::Okay; }
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
   ERR setWindowControllers(HOSTWINDOW Window, bool Enabled) override;
   ERR acquireWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap) override;
   ERR releaseWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap) override;

   ERR displayInfo(DisplayInfo &Info) override;
   ERR density(HOSTWINDOW Window, int &Horizontal, int &Vertical) override;
   ERR resolutions(std::vector<resolution> &List) override { return ERR::NoSupport; }
   ERR setDisplayMode(int &Width, int &Height, int &BitsPerPixel, double RefreshRate) override {
      return ERR::NoSupport;
   }
   ERR setGamma(double Red, double Green, double Blue) override { return ERR::NoSupport; }
   ERR setPowerMode(DPMS Mode) override { return ERR::NoSupport; }
   ERR pixelFormat(ColourFormat &Format) override;

   ERR present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
      int XDest, int YDest) override;
   ERR blitBitmap(extBitmap *Destination, extBitmap *Source, BAF Flags, int X, int Y, int Width,
      int Height, int XDest, int YDest) override;
   ERR fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour) override;
   ERR flush() override { return ERR::Okay; }

   ERR allocBitmap(extBitmap *Bitmap) override;
   ERR freeBitmap(extBitmap *Bitmap) override;
   ERR resizeBitmap(extBitmap *Bitmap, int Width, int Height) override { return ERR::NoSupport; }
   ERR lockBitmap(extBitmap *Bitmap, int16_t Access) override;
   ERR unlockBitmap(extBitmap *Bitmap) override { return ERR::Okay; }
   ERR bitmapRoutines(extBitmap *Bitmap) override;

   ERR setCursor(HOSTWINDOW Window, PTC CursorID) override;
   ERR setCustomCursor(HOSTWINDOW Window, extBitmap *Image, int HotX, int HotY) override { return ERR::NoSupport; }
   ERR showCursor(HOSTWINDOW Window, bool Visible) override;
   ERR warpPointer(HOSTWINDOW Window, int X, int Y) override;
   ERR pointerPosition(double &X, double &Y) override { return ERR::NoSupport; }

   ERR setHostOption(HOST Option, int64_t Value) override;
   ERR readController(int Port, double *Axes, CON &Buttons) override;
   ERR totalControllerPorts(int &Total) override;

private:
   bool Open = false;
   Win32HostOptions HostOptions;
   std::mutex WindowLock;
   std::unordered_map<HOSTWINDOW, extDisplay *> Displays;
};

DCAP Win32Driver::capabilities() const
{
   return DCAP::WINDOW_POSITION|DCAP::STACKING|DCAP::POINTER_WARP|DCAP::CONTROLLERS|DCAP::COMPOSITING|
      DCAP::VIDEO_BITMAPS|DCAP::WINDOW_DECOR|DCAP::DRAG_DROP;
}

ERR Win32Driver::open(const DriverCallbacks &Callbacks)
{
   if (Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;

   glWin32Callbacks = &Callbacks;
   glInstance = winGetModuleHandle();
   if ((not glInstance) or (not winCreateScreenClass(GetResource(RES::WINDOWS_ICON)))) {
      winTerminate();
      glInstance = nullptr;
      glWin32Callbacks = nullptr;
      return ERR::SystemCall;
   }

   winDisableBatching();
   winInitCursors(glWin32Cursors, std::ssize(glWin32Cursors));
   Open = true;
   return ERR::Okay;
}

ERR Win32Driver::close()
{
   if (not Open) return ERR::Okay;

   winTerminate();
   {
      const std::lock_guard<std::mutex> lock(WindowLock);
      Displays.clear();
   }
   glInstance = nullptr;
   glWin32Callbacks = nullptr;
   Open = false;
   return ERR::Okay;
}

ERR Win32Driver::createWindow(extDisplay *Display, HOSTWINDOW &Handle)
{
   bool desktop = false;
   if ((Display->Flags & SCR::COMPOSITE) IS SCR::NIL) {
      OBJECTID surface_id;
      if (FindObject("SystemSurface", CLASSID::SURFACE, &surface_id) IS ERR::Okay) {
         desktop = surface_id IS Display->ownerID();
      }
   }

   std::string_view name;
   CurrentTask()->getName(name);

   HOSTWINDOW popover = nullptr;
   if (Display->PopOverID) {
      if (ScopedObjectLock<extDisplay> other_display(Display->PopOverID, 3000); other_display.granted()) {
         popover = other_display->WindowHandle;
      }
      else return ERR::AccessObject;
   }

   Handle = winCreateScreen(HWND(popover), &Display->X, &Display->Y, &Display->Width, &Display->Height,
      ((Display->Flags & SCR::MAXIMISE) != SCR::NIL) ? 1 : 0,
      ((Display->Flags & SCR::BORDERLESS) != SCR::NIL) ? 1 : 0, name.data(),
      ((Display->Flags & SCR::COMPOSITE) != SCR::NIL) ? 1 : 0, Display->Opacity, desktop ? 1 : 0, &HostOptions);
   if (not Handle) return ERR::SystemCall;

   winControllerSetWindow(HWND(Handle), (Display->Flags & SCR::GRAB_CONTROLLERS) != SCR::NIL);
   {
      const std::lock_guard<std::mutex> lock(WindowLock);
      Displays[Handle] = Display;
   }
   return ERR::Okay;
}

ERR Win32Driver::adoptWindow(extDisplay *Display, APTR NativeHandle, HOSTWINDOW &Handle)
{
   if (not NativeHandle) return ERR::NullArgs;

   Handle = winCreateChild(HWND(NativeHandle), Display->X, Display->Y, Display->Width, Display->Height);
   if (not Handle) return ERR::SystemCall;

   winControllerSetWindow(HWND(Handle), (Display->Flags & SCR::GRAB_CONTROLLERS) != SCR::NIL);
   {
      const std::lock_guard<std::mutex> lock(WindowLock);
      Displays[Handle] = Display;
   }
   return ERR::Okay;
}

ERR Win32Driver::nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle)
{
   NativeHandle = Window;
   return Window ? ERR::Okay : ERR::NoSupport;
}

ERR Win32Driver::destroyWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::Okay;
   {
      const std::lock_guard<std::mutex> lock(WindowLock);
      Displays.erase(Window);
   }
   return winDestroyWindow(HWND(Window)) ? ERR::Okay : ERR::SystemCall;
}

ERR Win32Driver::showWindow(HOSTWINDOW Window, bool Maximise)
{
   if (not Window) return ERR::NoSupport;
   winShowWindow(Window, Maximise ? 1 : 0);
   winUpdateWindow(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::hideWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::NoSupport;
   winHideWindow(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::focusWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::NoSupport;
   winFocus(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::moveWindow(HOSTWINDOW Window, int X, int Y)
{
   if (not Window) return ERR::NoSupport;
   return winMoveWindow(HWND(Window), X, Y) ? ERR::Okay : ERR::SystemCall;
}

ERR Win32Driver::resizeWindow(HOSTWINDOW Window, int X, int Y, int Width, int Height)
{
   if (not Window) return ERR::NoSupport;
   return winResizeWindow(HWND(Window), X, Y, Width, Height) ? ERR::Okay : ERR::Resize;
}

ERR Win32Driver::raiseWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::NoSupport;
   winMoveToFront(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::lowerWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::NoSupport;
   winMoveToBack(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::minimiseWindow(HOSTWINDOW Window)
{
   if (not Window) return ERR::NoSupport;
   winMinimiseWindow(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::setWindowTitle(HOSTWINDOW Window, CSTRING Title)
{
   if ((not Window) or (not Title)) return ERR::NullArgs;
   winSetWindowTitle(HWND(Window), Title);
   return ERR::Okay;
}

ERR Win32Driver::setSizeHints(HOSTWINDOW Window, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect)
{
   return ERR::NoSupport;
}

ERR Win32Driver::windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height)
{
   int client_x, client_y, client_width, client_height;
   return winGetCoords(HWND(Window), X, Y, Width, Height, client_x, client_y, client_width, client_height);
}

ERR Win32Driver::frameMargins(HOSTWINDOW Window, int &Left, int &Top, int &Right, int &Bottom)
{
   if (not Window) return ERR::NoSupport;
   return winGetMargins(HWND(Window), &Left, &Top, &Right, &Bottom);
}

ERR Win32Driver::windowTitle(HOSTWINDOW Window, std::string &Title)
{
   if (not Window) return ERR::NoSupport;
   char buffer[256] = { 0 };
   winGetWindowTitle(HWND(Window), buffer, sizeof(buffer));
   Title = buffer;
   return ERR::Okay;
}

ERR Win32Driver::setWindowSurface(HOSTWINDOW Window, OBJECTID SurfaceID)
{
   if (not Window) return ERR::NoSupport;
   winSetSurfaceID(HWND(Window), SurfaceID);
   return ERR::Okay;
}

ERR Win32Driver::windowSurface(HOSTWINDOW Window, OBJECTID &SurfaceID)
{
   if (not Window) return ERR::NoSupport;
   SurfaceID = winLookupSurfaceID(HWND(Window));
   return ERR::Okay;
}

ERR Win32Driver::setWindowControllers(HOSTWINDOW Window, bool Enabled)
{
   if (not Window) return ERR::NoSupport;
   winControllerSetWindow(HWND(Window), Enabled);
   return ERR::Okay;
}

ERR Win32Driver::acquireWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap)
{
   if ((not Window) or (not Bitmap)) return ERR::NullArgs;
   Bitmap->DriverData = winGetDC(HWND(Window));
   return Bitmap->DriverData ? ERR::Okay : ERR::SystemCall;
}

ERR Win32Driver::releaseWindowBitmap(HOSTWINDOW Window, extBitmap *Bitmap)
{
   if ((not Window) or (not Bitmap)) return ERR::NullArgs;
   if (Bitmap->DriverData) winReleaseDC(HWND(Window), HDC(Bitmap->DriverData));
   Bitmap->DriverData = nullptr;
   return ERR::Okay;
}

ERR Win32Driver::displayInfo(DisplayInfo &Info)
{
   HOSTWINDOW window = nullptr;
   if (Info.DisplayID) {
      if (ScopedObjectLock<extDisplay> display(Info.DisplayID, 5000); display.granted()) window = display->WindowHandle;
      else return ERR::AccessObject;
   }

   if (winGetDisplayGeometry(HWND(window), Info.MonitorX, Info.MonitorY, Info.MonitorWidth, Info.MonitorHeight,
         Info.VirtualX, Info.VirtualY, Info.VirtualWidth, Info.VirtualHeight,
         Info.PhysicalWidth, Info.PhysicalHeight) IS ERR::Okay) {
      if (not Info.Width) Info.Width = Info.MonitorWidth;
      if (not Info.Height) Info.Height = Info.MonitorHeight;
   }
   else {
      winGetDesktopSize(&Info.VirtualWidth, &Info.VirtualHeight);
      if (not Info.Width) Info.Width = Info.VirtualWidth;
      if (not Info.Height) Info.Height = Info.VirtualHeight;
   }

   int bits = 0;
   int bytes = 0;
   int colours = 0;
   double refresh_rate = 0;
   winGetDisplaySettings(HWND(window), &bits, &bytes, &colours, &refresh_rate);

   if (not Info.BitsPerPixel) Info.BitsPerPixel = bits;
   if (not Info.BytesPerPixel) Info.BytesPerPixel = bytes;
   if (not Info.AmtColours) Info.AmtColours = colours;
   Info.AccelFlags = ACF(-1);
   density(window, Info.HDensity, Info.VDensity);

   if (refresh_rate > 1.0) {
      Info.RefreshRate = float(refresh_rate);
      Info.MinRefresh = Info.RefreshRate;
      Info.MaxRefresh = Info.RefreshRate;
   }

   return ERR::Okay;
}

ERR Win32Driver::density(HOSTWINDOW Window, int &Horizontal, int &Vertical)
{
   Horizontal = 96;
   Vertical = 96;
   winGetDPI(&Horizontal, &Vertical);
   if (Horizontal < 96) Horizontal = 96;
   if (Vertical < 96) Vertical = 96;
   return ERR::Okay;
}

ERR Win32Driver::pixelFormat(ColourFormat &Format)
{
   int red, green, blue, alpha;
   if (winGetPixelFormat(&red, &green, &blue, &alpha)) return ERR::SystemCall;
   win32_colour_format(Format, 32, red, green, blue, alpha);
   return ERR::Okay;
}

ERR Win32Driver::present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   if ((not Window) or (not Source)) return ERR::NullArgs;

   double opacity = 1.0;
   uint32_t alpha_mask = 0;
   HDC destination = nullptr;
   {
      const std::lock_guard<std::mutex> lock(WindowLock);
      if (auto display = Displays.find(Window); display != Displays.end()) {
         opacity = display->second->Opacity;
         destination = HDC(((extBitmap *)display->second->Bitmap)->DriverData);
         if ((display->second->Flags & SCR::COMPOSITE) != SCR::NIL) {
            alpha_mask = Source->ColourFormat->AlphaMask << Source->ColourFormat->AlphaPos;
         }
      }
   }

   bool release_destination = false;
   if (not destination) {
      destination = winGetDC(HWND(Window));
      if (not destination) return ERR::SystemCall;
      release_destination = true;
   }

   win32RedrawWindow(HWND(Window), destination, X, Y, Width, Height, XDest, YDest,
      Source->Width, Source->Height, Source->BitsPerPixel, Source->Data,
      Source->ColourFormat->RedMask << Source->ColourFormat->RedPos,
      Source->ColourFormat->GreenMask << Source->ColourFormat->GreenPos,
      Source->ColourFormat->BlueMask << Source->ColourFormat->BluePos,
      alpha_mask, opacity);
   if (release_destination) winReleaseDC(HWND(Window), destination);
   return ERR::Okay;
}

ERR Win32Driver::blitBitmap(extBitmap *Destination, extBitmap *Source, BAF Flags, int X, int Y, int Width,
   int Height, int XDest, int YDest)
{
   if ((not Destination) or (not Source)) return ERR::NullArgs;
   if (not Destination->DriverData) return ERR::NoSupport;

   auto destination = HDC(Destination->DriverData);
   if (Source->DriverData) {
      if (int error = winBlit(destination, XDest, YDest, Width, Height, Source->DriverData, X, Y)) {
         char buffer[80] = { 0 };
         winGetError(error, buffer, sizeof(buffer));
         kt::Log(__FUNCTION__).warning("BitBlt(): %s", buffer);
         return ERR::SystemCall;
      }
      return ERR::Okay;
   }

   if (not Source->Data) return ERR::NoSupport;

   if (((Flags & BAF::BLEND) != BAF::NIL) and (Source->BitsPerPixel IS 32) and
         ((Source->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
      auto source_data = (uint32_t *)(Source->Data + (Y * Source->LineWidth) + (X << 2));
      while (Height > 0) {
         for (int i=0; i < Width; i++) {
            auto alpha = uint8_t(255 - Source->unpackAlpha(source_data[i]));
            if (alpha >= BLEND_MAX_THRESHOLD) {
               auto red = uint8_t(source_data[i] >> Source->prvColourFormat.RedPos);
               auto green = uint8_t(source_data[i] >> Source->prvColourFormat.GreenPos);
               auto blue = uint8_t(source_data[i] >> Source->prvColourFormat.BluePos);
               SetPixelV(destination, XDest + i, YDest, (blue << 16) | (green << 8) | red);
            }
            else if (alpha >= BLEND_MIN_THRESHOLD) {
               auto colour = uint32_t(GetPixel(destination, XDest + i, YDest));
               auto destination_red = uint8_t(colour);
               auto destination_green = uint8_t(colour >> 8);
               auto destination_blue = uint8_t(colour >> 16);
               auto red = uint8_t(source_data[i] >> Source->prvColourFormat.RedPos);
               auto green = uint8_t(source_data[i] >> Source->prvColourFormat.GreenPos);
               auto blue = uint8_t(source_data[i] >> Source->prvColourFormat.BluePos);
               red = destination_red + (((red - destination_red) * alpha) >> 8);
               green = destination_green + (((green - destination_green) * alpha) >> 8);
               blue = destination_blue + (((blue - destination_blue) * alpha) >> 8);
               SetPixelV(destination, XDest + i, YDest, (blue << 16) | (green << 8) | red);
            }
         }
         source_data = (uint32_t *)(((uint8_t *)source_data) + Source->LineWidth);
         YDest++;
         Height--;
      }
      return ERR::Okay;
   }

   if ((Source->Flags & BMF::TRANSPARENT) != BMF::NIL) {
      while (Height > 0) {
         for (int i=0; i < Width; i++) {
            auto colour = Source->ReadUCPixel(Source, X + i, Y);
            if (colour != uint32_t(Source->TransIndex)) {
               uint32_t native_colour = Source->unpackRed(colour);
               native_colour |= Source->unpackGreen(colour) << 8;
               native_colour |= Source->unpackBlue(colour) << 16;
               SetPixelV(destination, XDest + i, YDest, native_colour);
            }
         }
         Y++;
         YDest++;
         Height--;
      }
      return ERR::Okay;
   }

   winSetDIBitsToDevice(destination, XDest, YDest, Width, Height, X, Y, Source->Width, Source->Height,
      Source->BitsPerPixel, Source->Data,
      Source->ColourFormat->RedMask << Source->ColourFormat->RedPos,
      Source->ColourFormat->GreenMask << Source->ColourFormat->GreenPos,
      Source->ColourFormat->BlueMask << Source->ColourFormat->BluePos);
   return ERR::Okay;
}

ERR Win32Driver::fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour)
{
   if (not Destination) return ERR::NullArgs;
   if (not Destination->DriverData) return ERR::NoSupport;

   winDrawRectangle(Destination->DriverData, X, Y, Width, Height, Destination->unpackRed(Colour),
      Destination->unpackGreen(Colour), Destination->unpackBlue(Colour));
   return ERR::Okay;
}

ERR Win32Driver::allocBitmap(extBitmap *Bitmap)
{
   if (not Bitmap) return ERR::NullArgs;
   if (Bitmap->MemType IS BMT::TEXTURE) Bitmap->MemType = BMT::DATA;
   if (Bitmap->MemType != BMT::VIDEO) return ERR::NoSupport;

   Bitmap->prvAFlags |= BF_WINVIDEO;
   if ((Bitmap->Flags & BMF::NO_DATA) IS BMF::NIL) {
      Bitmap->DriverData = winCreateCompatibleDC();
      if (not Bitmap->DriverData) return ERR::SystemCall;
   }
   return ERR::Okay;
}

ERR Win32Driver::freeBitmap(extBitmap *Bitmap)
{
   if (not Bitmap) return ERR::NullArgs;
   if ((Bitmap->DriverData) and (Bitmap->prvAFlags & BF_WINVIDEO)) winDeleteDC(Bitmap->DriverData);
   Bitmap->DriverData = nullptr;
   return ERR::Okay;
}

// GDI video bitmaps are read through the device context by the pixel routines rather than a host-side snapshot, so
// there is no read-back for Access to control.

ERR Win32Driver::lockBitmap(extBitmap *Bitmap, int16_t)
{
   if (not Bitmap) return ERR::NullArgs;
   if (not Bitmap->Data) return ERR::FieldNotSet;
   return ERR::Okay;
}

ERR Win32Driver::bitmapRoutines(extBitmap *Bitmap)
{
   if ((not Bitmap) or ((Bitmap->prvAFlags & BF_WINVIDEO) IS 0)) return ERR::NoSupport;

   Bitmap->ReadUCPixel = &VideoReadPixel;
   Bitmap->ReadUCRPixel = &VideoReadRGBPixel;
   Bitmap->ReadUCRIndex = &VideoReadRGBIndex;
   Bitmap->DrawUCPixel = &VideoDrawPixel;
   Bitmap->DrawUCRPixel = &VideoDrawRGBPixel;
   Bitmap->DrawUCRIndex = &VideoDrawRGBIndex;
   return ERR::Okay;
}

ERR Win32Driver::setCursor(HOSTWINDOW Window, PTC CursorID)
{
   for (auto &cursor : glWin32Cursors) {
      if (cursor.CursorID IS CursorID) {
         winSetCursor(cursor.WinCursor);
         return ERR::Okay;
      }
   }

   winSetCursor(glWin32Cursors[0].WinCursor);
   return ERR::Search;
}

ERR Win32Driver::showCursor(HOSTWINDOW Window, bool Visible)
{
   winShowCursor(Visible ? 1 : 0);
   return ERR::Okay;
}

ERR Win32Driver::warpPointer(HOSTWINDOW Window, int X, int Y)
{
   winSetCursorPos(X, Y);
   return ERR::Okay;
}

ERR Win32Driver::setHostOption(HOST Option, int64_t Value)
{
   switch (Option) {
      case HOST::TRAY_ICON:
         HostOptions.TrayIcon = Value;
         if (HostOptions.TrayIcon) HostOptions.TaskBar = 0;
         break;

      case HOST::TASKBAR:
         HostOptions.TaskBar = Value;
         if (HostOptions.TaskBar) HostOptions.TrayIcon = 0;
         break;

      case HOST::STICK_TO_FRONT:
         HostOptions.StickToFront = Value;
         break;

      default:
         return ERR::Args;
   }

   return ERR::Okay;
}

ERR Win32Driver::readController(int Port, double *Axes, CON &Buttons)
{
   return winReadController(Port, Axes, Buttons);
}

ERR Win32Driver::totalControllerPorts(int &Total)
{
   return winGetControllerPorts(Total);
}

DisplayDriver * create_win32_display_driver(uint32_t InterfaceVersion, struct CoreBase *Core)
{
   if ((InterfaceVersion != DISPLAY_DRIVER_INTERFACE_VERSION) or (not Core)) return nullptr;
   return new(std::nothrow) Win32Driver;
}

void destroy_win32_display_driver(DisplayDriver *Driver)
{
   delete Driver;
}

} // namespace display
