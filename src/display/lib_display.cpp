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
GetDisplayInfo: Retrieves display information.

The GetDisplayInfo() function returns information about a display, which includes information such as its size and bit
depth.  If the system is running on a hosted display (e.g. Windows or X11) then GetDisplayInfo() can also be used to
retrieve information about the default monitor by using a Display of zero.

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
   static thread_local DisplayInfo *t_info = nullptr;

   if (!Result) return ERR::NullArgs;

   if (!t_info) {
      // Each thread gets an allocation that can't be resource tracked, so MEM::HIDDEN is used in this case.
      // Note that this could conceivably cause memory leaks if temporary threads were to use this function.
      if (AllocMemory(sizeof(DisplayInfo), MEM::NO_CLEAR|MEM::HIDDEN, &t_info) != ERR::Okay) {
         return ERR::AllocMemory;
      }
   }

   if (auto error = get_display_info(DisplayID, t_info); error IS ERR::Okay) {
      *Result = t_info;
      return ERR::Okay;
   }
   else return error;
}

/*********************************************************************************************************************

-FUNCTION-
GetDisplayType: Returns the type of display supported.

This function returns the type of display supported by the loaded Display module.  Current return values are:

<types lookup="DT"/>

-RESULT-
int(DT): Returns an integer indicating the display type.

-TAGS-
pure-query

*********************************************************************************************************************/

DT GetDisplayType(void)
{
#ifdef _WIN32
   return DT::WINGDI;
#elif __xwindows__
   return DT::X11;
#elif _GLES_
   return DT::GLES;
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
while (scrScanDisplayModes("depth=32", &info) IS ERR::Okay) {
   ...
}
</pre>

-INPUT-
cpp(strview) Filter: The filter to apply to the resolution database.  May be NULL for no filter.
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
#if defined(_WIN32) || defined(__xwindows__)
   kt::Log log(__FUNCTION__);

   switch (Option) {
      case HOST::TRAY_ICON:
         glTrayIcon = Value;
         if (glTrayIcon) glTaskBar = 0;
         break;

      case HOST::TASKBAR:
         glTaskBar = Value;
         if (glTaskBar) glTrayIcon = 0;
         break;

      case HOST::STICK_TO_FRONT:
         glStickToFront = Value;
         break;

      default:
         log.warning("Invalid option %d, Data %" PF64, int(Option), (long long)Value);
   }
#endif

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
