#include <kotuku/config.h>
#define __OBJECTID_DEFINED
#define NO_STRICT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

// Windows and Kotuku share several enum names.  Keep the native header's templates and discard its token macros.
#undef RELATIVE
#undef TRANSPARENT
#undef ERROR
#undef DELETE
#undef MAXINT
#undef ABSOLUTE
#undef NO_DATA

#include "../driver/display_driver.h"
#include <kotuku/modules/display.h>

#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

#ifdef KOTUKU_STATIC
extern "C" size_t win32_driver_bitmap_size();
#else
extern "C" __declspec(dllimport) size_t win32_driver_bitmap_size();
#endif

static int check(bool Condition)
{
   return Condition ? 0 : 1;
}

static int expect(bool Condition, CSTRING Message)
{
   if (not Condition) std::fprintf(stderr, "%s\n", Message);
   return check(Condition);
}

// Only public bitmap fields are used.  Extra row padding and sentinel pixels reveal incorrect destination strides.

struct CaptureBitmap {
   ColourFormat Format = {};
   std::vector<uint32_t> Pixels;
   objBitmap *Bitmap;

   CaptureBitmap(int Width, int Height) : Pixels(size_t(Width + 2) * Height, 0x12345678) {
      Bitmap = (objBitmap *)std::calloc(1, win32_driver_bitmap_size());
      if (not Bitmap) return;
      Format.RedMask = Format.GreenMask = Format.BlueMask = Format.AlphaMask = 0xff;
      Format.RedPos = 16;
      Format.GreenPos = 8;
      Format.AlphaPos = 24;
      Format.BitsPerPixel = 32;
      Bitmap->ColourFormat = &Format;
      Bitmap->Data = (uint8_t *)Pixels.data();
      Bitmap->Width = Width;
      Bitmap->Height = Height;
      Bitmap->LineWidth = (Width + 2) * 4;
      Bitmap->ByteWidth = Width * 4;
      Bitmap->BytesPerPixel = 4;
      Bitmap->BitsPerPixel = 32;
      Bitmap->Clip.Right = Width;
      Bitmap->Clip.Bottom = Height;
   }

   ~CaptureBitmap() { std::free(Bitmap); }
   CaptureBitmap(const CaptureBitmap &) = delete;
   CaptureBitmap & operator=(const CaptureBitmap &) = delete;

   extBitmap * target() { return (extBitmap *)Bitmap; }
   uint32_t pixel(int X, int Y) const { return Pixels[size_t(Y) * (Bitmap->Width + 2) + X]; }
   void clear() { std::fill(Pixels.begin(), Pixels.end(), 0x12345678); }
};

static LRESULT CALLBACK capture_window(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
   if (Message IS WM_PAINT) {
      PAINTSTRUCT paint;
      auto dc = BeginPaint(Window, &paint);
      RECT client;
      GetClientRect(Window, &client);
      const COLORREF colours[] = { RGB(200, 100, 50), RGB(10, 80, 220), RGB(30, 160, 70), RGB(240, 20, 110) };
      for (int i=0; i < 4; i++) {
         RECT rect = { (i & 1) ? client.right / 2 : 0, (i & 2) ? client.bottom / 2 : 0,
            (i & 1) ? client.right : client.right / 2, (i & 2) ? client.bottom : client.bottom / 2 };
         auto brush = CreateSolidBrush(colours[i]);
         FillRect(dc, &rect, brush);
         DeleteObject(brush);
      }
      EndPaint(Window, &paint);
      return 0;
   }
   return DefWindowProc(Window, Message, WParam, LParam);
}

struct CaptureWindow {
   HWND Window = nullptr;
   ~CaptureWindow() { if (Window) DestroyWindow(Window); }
};

struct SavedCursor {
   POINT Position = {};
   bool Valid = GetCursorPos(&Position) != 0;
   ~SavedCursor() { if (Valid) SetCursorPos(Position.x, Position.y); }
};

static BOOL CALLBACK monitor_rect(HMONITOR, HDC, RECT *Rect, LPARAM Data)
{
   ((std::vector<RECT> *)Data)->push_back(*Rect);
   return TRUE;
}

struct PointerInput {
   OBJECTID Surface = 0;
   OBJECTID ReleasedOver = 0;
   int Releases = 0;
};

static PointerInput glPointerInput;

static void test_captured_pointer(DisplayDriver *Driver, int &Failures)
{
   glPointerInput = {};
   DriverCallbacks callbacks = {
      .Version = DISPLAY_DRIVER_INTERFACE_VERSION,
      .Movement = [](OBJECTID SurfaceID, double, double, bool) { glPointerInput.Surface = SurfaceID; },
      .ButtonInput = [](int Buttons, bool Pressed) {
         if ((Buttons & 0x0001) and (not Pressed)) { // The driver callback's primary-button bit.
            glPointerInput.ReleasedOver = glPointerInput.Surface;
            glPointerInput.Releases++;
         }
      }
   };
   const auto error = Driver->open(callbacks);
   Failures += expect(error IS ERR::Okay, "Open driver for captured pointer tests");
   if (error != ERR::Okay) return;
   SavedCursor saved_cursor;
   {
      CaptureWindow source, target, foreign;
      RECT work = {};
      SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
      const auto instance = GetModuleHandle(nullptr);
      source.Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "ScreenClass", "Drag source",
         WS_POPUP, work.left + 40, work.top + 40, 120, 100, nullptr, nullptr, instance, nullptr);
      target.Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "ScreenClass", "Drag target",
         WS_POPUP, work.left + 220, work.top + 40, 120, 100, nullptr, nullptr, instance, nullptr);
      foreign.Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "ScreenClass", "Unregistered window",
         WS_POPUP, work.left + 400, work.top + 40, 120, 100, nullptr, nullptr, instance, nullptr);
      Failures += expect(source.Window and target.Window and foreign.Window, "Create captured pointer test windows");
      if (source.Window and target.Window and foreign.Window) {
         Driver->setWindowSurface(source.Window, 101);
         Driver->setWindowSurface(target.Window, 102);
         ShowWindow(source.Window, SW_SHOWNOACTIVATE);
         ShowWindow(target.Window, SW_SHOWNOACTIVATE);
         ShowWindow(foreign.Window, SW_SHOWNOACTIVATE);

         SetCursorPos(work.left + 60, work.top + 60);
         SendMessage(source.Window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
         Failures += expect(GetCapture() IS source.Window, "Primary press captures the source window");
         const auto focus = GetFocus();
         Driver->raiseWindow(target.Window);
         Failures += expect(GetFocus() IS focus and GetCapture() IS source.Window,
            "Raising a separate preview window preserves source focus and capture");
         SetCursorPos(work.left + 240, work.top + 60);
         SendMessage(source.Window, WM_MOUSEMOVE, MK_LBUTTON, 0);
         Failures += expect(glPointerInput.Surface IS 102, "Captured movement resolves the destination window");
         SendMessage(source.Window, WM_RBUTTONDOWN, MK_LBUTTON | MK_RBUTTON, 0);
         SendMessage(source.Window, WM_RBUTTONUP, MK_LBUTTON, 0);
         Failures += expect(GetCapture() IS source.Window, "Secondary release retains primary capture");
         Failures += expect(glPointerInput.Releases IS 0, "Secondary release does not finish a primary drag");

         SetCursorPos(work.left + 420, work.top + 60);
         SendMessage(source.Window, WM_MOUSEMOVE, MK_LBUTTON, 0);
         Failures += expect(glPointerInput.Surface IS 0, "An unregistered window is not an internal destination");

         // The final release can precede the next WM_MOUSEMOVE.  It must refresh the destination before delivery.

         SetCursorPos(work.left + 240, work.top + 60);
         SendMessage(source.Window, WM_LBUTTONUP, 0, 0);
         Failures += expect(glPointerInput.Releases IS 1 and glPointerInput.ReleasedOver IS 102,
            "Release uses the current destination without requiring a prior motion message");
         Failures += expect(GetCapture() IS nullptr, "Last button release relinquishes native capture");

         SendMessage(source.Window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
         SetCursorPos(work.left + 420, work.top + 60);
         SendMessage(source.Window, WM_LBUTTONUP, 0, 0);
         Failures += expect(glPointerInput.Releases IS 2 and glPointerInput.ReleasedOver IS 0,
            "Releasing outside registered windows has no internal destination");
      }
      ReleaseCapture();
   }
   Failures += expect(Driver->close() IS ERR::Okay, "Close driver after captured pointer tests");
}

static void test_capture(DisplayDriver *Driver, int &Failures)
{
   CaptureBitmap bitmap(8, 6);
   Failures += expect(bitmap.Bitmap != nullptr, "Allocate test bitmap");
   if (not bitmap.Bitmap) return;
   Failures += expect(Driver->captureDisplay(0, 0, 1, 1, bitmap.target(), 0, 0) IS ERR::NotInitialised,
      "Capture on a closed driver");

   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   auto error = Driver->open(callbacks);
   Failures += expect(error IS ERR::Okay, "Open Windows driver");
   if (error != ERR::Okay) return;
   Failures += expect(Driver->open(callbacks) IS ERR::DoubleInit, "Reject duplicate open");

   WNDCLASSA window_class = {};
   window_class.lpfnWndProc = capture_window;
   window_class.hInstance = GetModuleHandle(nullptr);
   window_class.lpszClassName = "KotukuCaptureTest";
   Failures += expect(RegisterClassA(&window_class) != 0, "Register capture test class");
   std::vector<RECT> monitors;
   Failures += expect(EnumDisplayMonitors(nullptr, nullptr, monitor_rect, (LPARAM)&monitors) != 0,
      "Enumerate desktop monitors");
   Failures += expect(not monitors.empty(), "At least one monitor must be tested");
   SavedCursor saved_cursor;

   for (const auto &monitor : monitors) {
      CaptureWindow host;
      const int win_x = monitor.left + (monitor.right - monitor.left) / 2;
      const int win_y = monitor.top + (monitor.bottom - monitor.top) / 2;
      host.Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
         window_class.lpszClassName, "Capture test", WS_POPUP, win_x, win_y, 64, 64,
         nullptr, nullptr, window_class.hInstance, nullptr);
      Failures += expect(host.Window != nullptr, "Create capture test window");
      if (not host.Window) continue;
      ShowWindow(host.Window, SW_SHOWNOACTIVATE);
      UpdateWindow(host.Window);
      DwmFlush();
      RECT logical_rect = {}, physical_rect = {};
      const bool have_bounds = GetWindowRect(host.Window, &logical_rect) and
         SUCCEEDED(DwmGetWindowAttribute(host.Window, DWMWA_EXTENDED_FRAME_BOUNDS, &physical_rect,
            sizeof(physical_rect))) and (logical_rect.right > logical_rect.left);
      Failures += expect(have_bounds, "Read physical test window bounds");
      if (not have_bounds) continue;
      std::printf("Host logical (%ld,%ld)-(%ld,%ld), physical (%ld,%ld)-(%ld,%ld), window scale %ld%%\n",
         logical_rect.left, logical_rect.top, logical_rect.right, logical_rect.bottom,
         physical_rect.left, physical_rect.top, physical_rect.right, physical_rect.bottom,
         (physical_rect.right - physical_rect.left) * 100 / (logical_rect.right - logical_rect.left));
      const int screen_x = physical_rect.left, screen_y = physical_rect.top;
      const int middle_x = (physical_rect.left + physical_rect.right) / 2;
      const int middle_y = (physical_rect.top + physical_rect.bottom) / 2;

      // Both rows and columns cross a colour boundary, so bottom-up images or an incorrect source offset fail.

      bitmap.clear();
      error = Driver->captureDisplay(middle_x - 2, middle_y - 1, 4, 2, bitmap.target(), 1, 2);
      Failures += expect(error IS ERR::Okay, "Capture colour boundary");
      const uint32_t colours[] = { 0xffc86432, 0xff0a50dc, 0xff1ea046, 0xfff0146e };
      auto screen = GetDC(nullptr);
      Failures += expect(screen != nullptr, "Read reference desktop pixels");
      for (int y=0; y < 6; y++) {
         for (int x=0; x < 10; x++) {
            const bool copied = (x >= 1) and (x < 5) and (y >= 2) and (y < 4);
            auto expected = copied ? colours[(x >= 3 ? 1 : 0) + (y >= 3 ? 2 : 0)] : 0x12345678;
            if (copied and ((physical_rect.right - physical_rect.left) != 64)) {
               // DWM interpolates bitmap-stretched system-aware windows.  At a scaled colour boundary compare
               // against the presented pixel; the solid regions below must still match their known colours.
               const auto pixel = GetPixel(screen, middle_x - 2 + x - 1, middle_y - 1 + y - 2);
               expected = 0xff000000 | (uint32_t(GetRValue(pixel)) << 16) |
                  (uint32_t(GetGValue(pixel)) << 8) | GetBValue(pixel);
            }
            Failures += expect(bitmap.pixel(x, y) IS expected, "Colour, alpha, destination offset or row padding");
         }
      }
      ReleaseDC(nullptr, screen);

      // Exercise a different 32-bit channel order, then the per-pixel path used by lower-depth targets.

      std::swap(bitmap.Format.RedPos, bitmap.Format.BluePos);
      error = Driver->captureDisplay(screen_x + 2, screen_y + 2, 1, 1, bitmap.target(), 0, 0);
      Failures += expect((error IS ERR::Okay) and (bitmap.pixel(0, 0) IS 0xff3264c8), "RGBA target packing");
      std::swap(bitmap.Format.RedPos, bitmap.Format.BluePos);
      bitmap.Bitmap->BytesPerPixel = 2;
      bitmap.Bitmap->DrawUCRPixel = [](objBitmap *Bitmap, int X, int Y, RGB8 *RGB) {
         auto target = (uint16_t *)(Bitmap->Data + (Y * Bitmap->LineWidth)) + X;
         *target = uint16_t(((RGB->Red >> 3) << 11) | ((RGB->Green >> 2) << 5) | (RGB->Blue >> 3));
      };
      error = Driver->captureDisplay(screen_x + 2, screen_y + 2, 1, 1, bitmap.target(), 2, 1);
      const auto packed = *(uint16_t *)(bitmap.Bitmap->Data + bitmap.Bitmap->LineWidth + 4);
      Failures += expect((error IS ERR::Okay) and (packed IS 0xcb26), "RGB565 per-pixel capture");
      bitmap.Bitmap->BytesPerPixel = 4;
      bitmap.Bitmap->DrawUCRPixel = nullptr;

      Failures += expect(SetCursorPos(screen_x + 4, screen_y + 4) != 0, "Position desktop cursor");
      double x = 0, y = 0;
      Failures += expect(Driver->pointerPosition(x, y) IS ERR::Okay, "Read desktop cursor without driver windows");
      POINT physical_cursor = {};
      GetPhysicalCursorPos(&physical_cursor);
      std::printf("Cursor queried (%.0f,%.0f), physical (%ld,%ld)\n", x, y, physical_cursor.x, physical_cursor.y);
      Failures += expect((x IS physical_cursor.x) and (y IS physical_cursor.y), "Cursor uses physical coordinates");
      Failures += expect((x IS screen_x + 4) and (y IS screen_y + 4), "Cursor and capture coordinates agree");
      error = Driver->captureDisplay(int(x), int(y), 1, 1, bitmap.target(), 0, 0);
      Failures += expect((error IS ERR::Okay) and (bitmap.pixel(0, 0) IS colours[0]),
         "Capture follows cursor coordinates and excludes cursor image");

      // Opaque layered windows must be included by CAPTUREBLT.

      SetWindowLongPtr(host.Window, GWL_EXSTYLE, GetWindowLongPtr(host.Window, GWL_EXSTYLE) | WS_EX_LAYERED);
      Failures += expect(SetLayeredWindowAttributes(host.Window, 0, 255, LWA_ALPHA) != 0, "Set layered window");
      RedrawWindow(host.Window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
      DwmFlush();
      error = Driver->captureDisplay(middle_x + 4, middle_y + 4, 1, 1, bitmap.target(), 0, 0);
      Failures += expect((error IS ERR::Okay) and (bitmap.pixel(0, 0) IS colours[3]), "Capture layered window");

      std::thread worker([&]() {
         error = Driver->captureDisplay(middle_x + 4, middle_y + 4, 1, 1, bitmap.target(), 0, 0);
      });
      worker.join();
      Failures += expect((error IS ERR::Okay) and (bitmap.pixel(0, 0) IS colours[3]), "Capture from worker thread");
   }
   UnregisterClassA(window_class.lpszClassName, window_class.hInstance);

   const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
   const int right = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
   const int bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
   std::printf("Virtual capture bounds (%d,%d)-(%d,%d)\n", left, top, right, bottom);
   bitmap.clear();
   error = Driver->captureDisplay(left - 2, top - 2, 4, 4, bitmap.target(), 1, 1);
   Failures += expect(error IS ERR::Okay, "Clip upper-left source");
   for (int y=0; y < 6; y++) {
      for (int x=0; x < 10; x++) {
         const bool copied = (x >= 3) and (x < 5) and (y >= 3) and (y < 5);
         Failures += expect(copied ? ((bitmap.pixel(x, y) >> 24) IS 255) :
            (bitmap.pixel(x, y) IS 0x12345678), "Upper-left clipping preserves untouched pixels");
      }
   }
   bitmap.clear();
   error = Driver->captureDisplay(right - 2, bottom - 2, 4, 4, bitmap.target(), 1, 1);
   Failures += expect(error IS ERR::Okay, "Clip lower-right source");
   for (int y=0; y < 6; y++) {
      for (int x=0; x < 10; x++) {
         const bool copied = (x >= 1) and (x < 3) and (y >= 1) and (y < 3);
         Failures += expect(copied ? ((bitmap.pixel(x, y) >> 24) IS 255) :
            (bitmap.pixel(x, y) IS 0x12345678), "Lower-right clipping preserves untouched pixels");
      }
   }
   bitmap.clear();
   Failures += expect(Driver->captureDisplay(right, top, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange,
      "Source beyond virtual right edge");
   Failures += expect(Driver->captureDisplay(left, bottom, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange,
      "Source beyond virtual bottom edge");
   Failures += expect(Driver->captureDisplay(left - 4, top, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange,
      "Source beyond virtual left edge");
   Failures += expect(Driver->captureDisplay(left, top - 4, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange,
      "Source beyond virtual top edge");
   Failures += expect(Driver->captureDisplay(std::numeric_limits<int>::max(), top, 4, 4, bitmap.target(), 0, 0) IS
      ERR::OutOfRange, "Extreme source coordinate");
   Failures += expect(Driver->captureDisplay(left, top, 4, 4, nullptr, 0, 0) IS ERR::NullArgs, "Missing bitmap");
   Failures += expect(Driver->captureDisplay(left, top, 0, 4, bitmap.target(), 0, 0) IS ERR::NullArgs, "Zero width");
   Failures += expect(Driver->captureDisplay(left, top, 4, -1, bitmap.target(), 0, 0) IS ERR::NullArgs,
      "Negative height");
   Failures += expect(std::all_of(bitmap.Pixels.begin(), bitmap.Pixels.end(), [](uint32_t Pixel) {
      return Pixel IS 0x12345678;
   }), "Failed requests preserve destination");

   // Warm up GDI before measuring handles, then exercise the loupe's repeated small-capture workload.

   Driver->captureDisplay(0, 0, 1, 1, bitmap.target(), 0, 0);
   const auto handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
   for (int i=0; i < 100; i++) {
      Failures += expect(Driver->captureDisplay(0, 0, 1, 1, bitmap.target(), 0, 0) IS ERR::Okay,
         "Repeated small capture");
   }
   Failures += expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) IS handles, "Capture leaks GDI handles");
   Failures += expect(Driver->close() IS ERR::Okay, "Close Windows driver");
   Failures += expect(Driver->captureDisplay(0, 0, 1, 1, bitmap.target(), 0, 0) IS ERR::NotInitialised,
      "Capture after close");
}

int main()
{
   // Match the system DPI awareness requested by the framework's winGetDPI().
   SetProcessDPIAware();
   struct CoreBase core_table = {};
   core_table._GetResource = [](RES) -> int64_t { return 0; };
   auto core = &core_table;
#ifdef KOTUKU_STATIC
   auto driver = display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#else
   auto driver = create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#endif
   int failures = 0;

   failures += check(driver != nullptr);
   if (not driver) return failures;
   failures += check(std::string(driver->name()) IS "windows");
   failures += check(driver->displayType() IS DT::WINGDI);
   failures += check(driver->isAvailable() IS ERR::Okay);

   DriverCallbacks wrong_callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION - 1 };
   failures += check(driver->open(wrong_callbacks) IS ERR::WrongVersion);
   failures += check(driver->close() IS ERR::Okay);

   test_capture(driver, failures);
   test_captured_pointer(driver, failures);

#ifdef KOTUKU_STATIC
   display::destroy_win32_display_driver(driver);
   display::destroy_win32_display_driver(nullptr);
   failures += check(display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#else
   destroy_display_driver(driver);
   destroy_display_driver(nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#endif
   return failures;
}
