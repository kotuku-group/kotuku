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
AllocMemory:

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
while (!scrScanDisplayModes("depth=32", &info)) {
   ...
}
</pre>

-INPUT-
cstr Filter: The filter to apply to the resolution database.  May be NULL for no filter.
struct(*DisplayInfo) Info: A pointer to a !DisplayInfo structure must be referenced here.  The structure will be filled with information when the function returns.

-ERRORS-
Okay: The resolution information was retrieved.
Args:
NoSupport: Native graphics system not available (e.g. hosted on Windows or X11).
Search: There are no more display modes to return that are a match for the Filter.

*********************************************************************************************************************/

ERR ScanDisplayModes(CSTRING Filter, DisplayInfo *Info)
{
#ifdef __snap__

   GA_modeInfo modeinfo;
   uint16_t *modes;
   int colours, bytes, i, j, minrefresh, maxrefresh, refresh, display;
   int16_t f_depth, c_depth; // f = filter, c = condition (0 = equal; -1 <, -2 <=; +1 >, +2 >=)
   int16_t f_bytes, c_bytes;
   int16_t f_width, c_width, f_height, c_height;
   int16_t f_refresh, c_refresh;
   int16_t f_minrefresh, c_minrefresh;
   int16_t f_maxrefresh, c_maxrefresh;
   int8_t interlace, matched;

   if (!Info) return ERR::Args;

   // Reset all filters

   f_depth   = c_depth   = 0;
   f_bytes   = c_bytes   = 0;
   f_width   = c_width   = 0;
   f_height  = c_height  = 0;
   f_refresh = c_refresh = 0;
   f_minrefresh = c_minrefresh = 0;
   f_maxrefresh = c_maxrefresh = 0;

   if (Filter) {
      while (*Filter) {
         while ((*Filter) and (*Filter <= 0x20)) Filter++;
         while (*Filter IS ',') Filter++;
         while ((*Filter) and (*Filter <= 0x20)) Filter++;

         if (startswith("depth", Filter))   extract_value(Filter, &f_depth, &c_depth);
         if (startswith("bytes", Filter))   extract_value(Filter, &f_bytes, &c_bytes);
         if (startswith("width", Filter))   extract_value(Filter, &f_width, &c_width);
         if (startswith("height", Filter))  extract_value(Filter, &f_height, &c_height);
         if (startswith("refresh", Filter)) extract_value(Filter, &f_refresh, &c_refresh);
         if (startswith("minrefresh", Filter)) extract_value(Filter, &f_minrefresh, &c_minrefresh);
         if (startswith("maxrefresh", Filter)) extract_value(Filter, &f_maxrefresh, &c_maxrefresh);

         while ((*Filter) and (*Filter != ',')) Filter++;
      }
   }

   modes = glSNAPDevice->AvailableModes;
   display = glSNAP->Init.GetDisplayOutput() & gaOUTPUT_SELECTMASK;
   for (i=Info->Index; modes[i] != 0xffff; i++) {
      modeinfo.dwSize = sizeof(modeinfo);
      if (glSNAP->Init.GetVideoModeInfoExt(modes[i], &modeinfo, display, glSNAP->Init.GetActiveHead())) continue;

      if (modeinfo.AttributesExt & gaIsPanningMode) continue;
      if (modeinfo.Attributes & gaIsTextMode) continue;
      if (modeinfo.BitsPerPixel < 8) continue;

      if (modeinfo.BitsPerPixel <= 8) bytes = 1;
      else if (modeinfo.BitsPerPixel <= 16) bytes = 2;
      else if (modeinfo.BitsPerPixel <= 24) bytes = 3;
      else bytes = 4;

      if (modeinfo.BitsPerPixel <= 24) colours = 1<<modeinfo.BitsPerPixel;
      else colours = 1<<24;

      minrefresh = 0x7fffffff;
      maxrefresh = 0;
      for (j=0; modeinfo.RefreshRateList[j] != -1; j++) {
         if ((refresh = modeinfo.RefreshRateList[j]) < 0) refresh = -refresh;
         if (refresh > maxrefresh) maxrefresh = refresh;
         if (refresh < minrefresh) minrefresh = refresh;
      }

      if (minrefresh IS 0x7fffffff) minrefresh = 0;

      refresh = modeinfo.DefaultRefreshRate;
      if (refresh < 0) {
         refresh = -refresh;
         interlace = TRUE;
      }
      else interlace = TRUE;

      // Check if this mode meets the filters, if so then reduce the index

      if (Filter) {
         matched = TRUE;
         if ((f_depth)   and (!compare_values(f_depth,   c_depth,   modeinfo.BitsPerPixel))) matched = FALSE;
         if ((f_bytes)   and (!compare_values(f_bytes,   c_bytes,   bytes))) matched = FALSE;
         if ((f_width)   and (!compare_values(f_width,   c_width,   modeinfo.XResolution)))  matched = FALSE;
         if ((f_height)  and (!compare_values(f_height,  c_height,  modeinfo.YResolution)))  matched = FALSE;
         if ((f_refresh) and (!compare_values(f_refresh, c_refresh, modeinfo.BitsPerPixel))) matched = FALSE;
         if ((f_minrefresh) and (!compare_values(f_minrefresh, c_minrefresh, minrefresh)))   matched = FALSE;
         if ((f_maxrefresh) and (!compare_values(f_maxrefresh, c_maxrefresh, maxrefresh)))   matched = FALSE;

         if (matched IS FALSE) continue;
      }

      // Return information for this mode

      Info->Width         = modeinfo.XResolution;
      Info->Height        = modeinfo.YResolution;
      Info->Depth         = modeinfo.BitsPerPixel;
      Info->BytesPerPixel = bytes;
      Info->AmtColours    = colours;
      Info->MinRefresh    = minrefresh;
      Info->MaxRefresh    = maxrefresh;
      Info->RefreshRate   = refresh;
      Info->Index         = i + 1;
      return ERR::Okay;
   }

   return ERR::Search;

#else

   return ERR::NoSupport;

#endif
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
-END-

*********************************************************************************************************************/

double ScaleToDPI(double Value)
{
   if ((!glDisplayInfo.HDensity) or (!glDisplayInfo.VDensity)) return Value;
   else return 96.0 / (((double)glDisplayInfo.HDensity + (double)glDisplayInfo.VDensity) * 0.5) * Value;
}

} // namespace
