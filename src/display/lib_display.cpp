/*********************************************************************************************************************

-CATEGORY-
Name: Display
-END-

*********************************************************************************************************************/

#include "defs.h"

ankerl::unordered_dense::map<WinHook, FUNCTION> glWindowHooks;
std::recursive_mutex glWindowHookLock;

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
CaptureDisplay: Copies a rectangle of the host desktop to a bitmap.

CaptureDisplay() reads the pixels currently presented by the host desktop and copies them to `Bitmap`.  It allows
tools such as colour pickers and magnifiers to sample content that is not owned by a Kōtuku surface.

`X` and `Y` are desktop coordinates, i.e. the same coordinate space as ~GetCursorPos() and the @Pointer `X` and
`Y` fields.  On multi-monitor systems they can be negative.  The source rectangle is clipped to the desktop, and the
destination is clipped to the bitmap's `Clip` region.  Pixels outside the clipped area are left untouched.

Captured pixels are written as the display presents them, i.e. as sRGB-encoded values with an alpha of 255.  No
conversion is applied if the bitmap uses the `CS::LINEAR_RGB` colour space.  The mouse cursor is not included.
Kōtuku's own windows are included as they appear on screen.

On Windows, layered windows are included.  Protected content such as DRM video and windows excluded from screen
capture may appear black or be omitted by the host.

Desktop capture is not available on all platforms.  Wayland sessions (including X11 clients running under XWayland),
headless sessions and Android return `ERR::NoSupport`, and callers should fall back to an alternative where possible.

-INPUT-
int X: Horizontal desktop coordinate of the area to capture.
int Y: Vertical desktop coordinate of the area to capture.
int Width: Width of the area to capture.
int Height: Height of the area to capture.
obj(Bitmap) Bitmap: The bitmap that will receive the captured pixels.
int XDest: Horizontal position in the bitmap to copy the area to.
int YDest: Vertical position in the bitmap to copy the area to.

-ERRORS-
Okay: At least part of the area was copied.
NullArgs: `Bitmap` was not specified, or `Width` or `Height` is less than 1.
InvalidObject: `Bitmap` does not refer to a Bitmap object.
OutOfRange: The area lies entirely outside of the desktop or the bitmap's clip region.
NoSupport: The display driver cannot read the host desktop.
SystemCall: The host capture request failed.
AllocMemory: A temporary host image could not be allocated.

-TAGS-
mutates-object, blocking

*********************************************************************************************************************/

ERR CaptureDisplay(int X, int Y, int Width, int Height, objBitmap *Bitmap, int XDest, int YDest)
{
   kt::Log log(__FUNCTION__);

   if ((not Bitmap) or (Width < 1) or (Height < 1)) return log.warning(ERR::NullArgs);
   if (Bitmap->classID() != CLASSID::BITMAP) return log.warning(ERR::InvalidObject);
   if (not glDriver) return ERR::NoSupport;

   auto bitmap = (extBitmap *)Bitmap;
   const int left = std::max(bitmap->Clip.Left, 0);
   const int top = std::max(bitmap->Clip.Top, 0);
   const int right = std::min(bitmap->Clip.Right, bitmap->Width);
   const int bottom = std::min(bitmap->Clip.Bottom, bitmap->Height);

   if (XDest < left) { X += left - XDest; Width -= left - XDest; XDest = left; }
   if (YDest < top) { Y += top - YDest; Height -= top - YDest; YDest = top; }
   if (XDest + Width > right) Width = right - XDest;
   if (YDest + Height > bottom) Height = bottom - YDest;
   if ((Width < 1) or (Height < 1)) return ERR::OutOfRange;

   if (auto error = lock_surface(bitmap, SURFACE_WRITE); error != ERR::Okay) return log.warning(error);
   auto error = glDriver->captureDisplay(X, Y, Width, Height, bitmap, XDest, YDest);
   unlock_surface(bitmap);
   return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetDisplayInfo: Retrieves display information.

The GetDisplayInfo() function returns information about a display, such as its size and bit depth.  If the system is
running on a hosted display (e.g. Windows or X11) then GetDisplayInfo() can also be used to retrieve information
about the default monitor by using a Display of zero.

The resulting !DisplayInfo structure values remain good until the next call to this function, at which point they will
be overwritten.

-INPUT-
oid Display: Object ID of the display to be analysed.
&struct(*DisplayInfo) Info: This reference will receive a pointer to a !DisplayInfo structure.

-ERRORS-
Okay:
NullArgs:
AccessObject:
AllocMemory:
SystemCall:
TimeOut:

-TAGS-
api-owns-result, volatile-result, blocking

*********************************************************************************************************************/

ERR GetDisplayInfo(OBJECTID DisplayID, DisplayInfo **Result)
{
   static thread_local std::unique_ptr<DisplayInfo, decltype(&std::free)> t_info(nullptr, &std::free);

   if (!Result) return ERR::NullArgs;

   if (!t_info) { // Each thread owns its result until thread exit.
      t_info.reset((DisplayInfo *)malloc(sizeof(DisplayInfo)));
      if (!t_info) return ERR::AllocMemory;
   }

   if (auto error = get_display_info(DisplayID, t_info.get()); !error) {
      *Result = t_info.get();
      return ERR::Okay;
   }
   else return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetDisplayType: Returns the selected display driver's backend type.

This function reports the backend identity of the driver selected during Display module initialisation.  Explicit and
automatic driver selection therefore return the same type when they resolve to the same backend.  Current values are:

On desktop Linux, automatic selection tries Wayland when `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` are set, then X11
when available, and finally the headless driver.  The headless driver reports `DT::NATIVE`.

<types lookup="DT"/>

-RESULT-
int(DT): Returns an integer indicating the display type.

-TAGS-
pure-query

*********************************************************************************************************************/

DT GetDisplayType(void)
{
   if (glDriver) return glDriver->displayType();
#ifdef _WIN32
   return DT::WINGDI;
#else
   return DT::NATIVE;
#endif
}

/*********************************************************************************************************************

-FUNCTION-
ScanDisplayModes: Private. Returns formatted resolution information from the display database.

For internal use only.

<pre>
DisplayInfo info;
clearmem(&info, sizeof(info));
while (!scrScanDisplayModes("depth=32", &info)) {
   ...
}
</pre>

-INPUT-
strview Filter: The filter to apply to the resolution database.  May be NULL for no filter.
struct(*DisplayInfo) Info: A pointer to a !DisplayInfo structure must be referenced here.  The structure will be filled with information when the function returns.

-ERRORS-
NoSupport: Native graphics system not available (e.g. hosted on Windows or X11).

-TAGS-
mutates-input

*********************************************************************************************************************/

ERR ScanDisplayModes(const std::string_view &Filter, DisplayInfo *Info)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FUNCTION-
SetHostOption: Alter options associated with the host display system.

For internal usage only.

-INPUT-
int(HOST) Option: One of TRAY_ICON, TASKBAR or STICK_TO_FRONT.
large Value: The value to be applied to the option.

-ERRORS-
Okay

*********************************************************************************************************************/

ERR SetHostOption(HOST Option, int64_t Value)
{
   if (glDriver) return glDriver->setHostOption(Option, Value);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ScaleToDPI: Scales a value to the active display's DPI.

ScaleToDPI() is a convenience function for scaling a value to the display's current DPI setting.  The provided value
must be relative to the system wide default of 96 DPI.  If the display's DPI is not equal to 96, the value will be
scaled to match.  For instance, an 8 point font at 96 DPI would be scaled to 20 points if the display was 240 DPI.

If the DPI of the display is unknown, the value will be returned unscaled.

-INPUT-
double Value: The number to be scaled.

-RESULT-
double: The scaled value is returned.

-TAGS-
pure-query
-END-

*********************************************************************************************************************/

double ScaleToDPI(double Value)
{
   if ((!glDisplayInfo.HDensity) or (!glDisplayInfo.VDensity)) return Value;
   else return 96.0 / (((double)glDisplayInfo.HDensity + (double)glDisplayInfo.VDensity) * 0.5) * Value;
}

} // namespace
