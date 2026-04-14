// Cygwin users: libuuid-devel will interfere with the resolution of IID_Unknown if installed.
// Removing lib/libuuid.la and lib/uuid.dll.a will resolve the compilation issue.

//#define DEBUG
//#define DBGMSG

#define _WIN32_WINNT 0x0600 // Allow Windows Vista function calls
#define WINVER 0x0600
#define NO_STRICT // Turn off type management due to C++ mangling issues.

#ifdef _MSC_VER
#pragma warning (disable : 4244 4311 4312 4267 4244 4068) // Disable annoying VC++ warnings
#endif

#include "keys.h"
#include <windows.h>
#include <windowsx.h>
//#include <resource.h>
#include <winuser.h>
#include <shlobj.h>
#include <objidl.h>
#include <xinput.h>
#include <map>

#include <kotuku/system/errors.h>

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>

#include "windows.h"

typedef unsigned char uint8_t;

struct winextra {
   int surface_id;   // 0
   int key;          // 4
   int interactive;  // 8
   int borderless;   // 12
};

#if defined(_MSC_VER)
typedef __int64 int64_t;
#else
typedef long long int64_t;
#endif

#define IDT_RESIZE_WINDOW 1

#define WE_SURFACE     0
#define WE_KEY         4
#define WE_INTERACTIVE 8
#define WE_BORDERLESS  12

#define WNS_PLUGIN 0x00000001

#define KEY_SURFACE 0x03929323

#define WIN_LMB 0x0001
#define WIN_RMB 0x0002
#define WIN_MMB 0x0004
#define WIN_DBL 0x8000
#define WIN_NONCLIENT 0x4000

#define BORDERSIZE 6
#define WM_ICONNOTIFY (WM_USER + 101)
#define ID_TRAY 100

//#define MSG(...) fprintf(stderr, __VA_ARGS__)
#define MSG(...)

extern HINSTANCE glInstance;
extern int glLastPort;
void KillMessageHook(void);

static HWND glMainScreen = 0;
static char glCursorEntry = FALSE;
static HCURSOR glDefaultCursor = 0;
static HWND glDeferredActiveWindow = 0;
uint8_t glTrayIcon = 0, glTaskBar = 0, glStickToFront = 0;
struct WinCursor *glCursors = 0;
HCURSOR glCurrentCursor = 0;
static int8_t glScreenClassInit = 0;

#ifdef DBGMSG
static ankerl::unordered_dense::map<int, const char *> glCmd = { {
 { WM_SETCURSOR, "WM_SETCURSOR" },
 { WM_NCMOUSEHOVER, "WM_NCMOUSEHOVER" },
 { WM_NCMOUSELEAVE, "WM_NCMOUSELEAVE" }, { WM_NCMOUSEMOVE, "WM_NCMOUSEMOVE" },
 { WM_MOUSEACTIVATE, "WM_MOUSEACTIVATE" }, { WM_MOUSEMOVE, "WM_MOUSEMOVE" }, { WM_LBUTTONDOWN, "WM_LBUTTONDOWN" }, { WM_LBUTTONUP, "WM_LBUTTONUP" },
 { WM_LBUTTONDBLCLK, "WM_LBUTTONDBLCLK" }, { WM_RBUTTONDOWN, "WM_RBUTTONDOWN" }, { WM_RBUTTONUP, "WM_RBUTTONUP" }, { WM_RBUTTONDBLCLK, "WM_RBUTTONDBLCLK" }, { WM_MBUTTONDOWN, "WM_MBUTTONDOWN" }, { WM_MBUTTONUP, "WM_MBUTTONUP" },
 { WM_MBUTTONDBLCLK, "WM_MBUTTONDBLCLK" }, { WM_MOUSEWHEEL, "WM_MOUSEWHEEL" }, { WM_MOUSEFIRST, "WM_MOUSEFIRST" }, { WM_XBUTTONDOWN, "WM_XBUTTONDOWN" },
 { WM_XBUTTONUP, "WM_XBUTTONUP" }, { WM_XBUTTONDBLCLK, "WM_XBUTTONDBLCLK" }, { WM_MOUSELAST, "WM_MOUSELAST" }, { WM_MOUSEHOVER, "WM_MOUSEHOVER" }, { WM_MOUSELEAVE, "WM_MOUSELEAVE" },
 { WM_NCHITTEST, "WM_NCHITTEST" }, { WM_NCLBUTTONDBLCLK, "WM_NCLBUTTONDBLCLK" }, { WM_NCLBUTTONDOWN, "WM_NCLBUTTONDOWN" },
 { WM_APP, "WM_APP" }, { WM_ACTIVATE, "WM_ACTIVATE" }, { WM_ACTIVATEAPP, "WM_ACTIVATEAPP" }, { WM_AFXFIRST, "WM_AFXFIRST" }, { WM_MOVE, "WM_MOVE" },
 { WM_AFXLAST, "WM_AFXLAST" }, { WM_ASKCBFORMATNAME, "WM_ASKCBFORMATNAME" }, { WM_CANCELJOURNAL, "WM_CANCELJOURNAL" }, { WM_CANCELMODE, "WM_CANCELMODE" },
 { WM_CAPTURECHANGED, "WM_CAPTURECHANGED" }, { WM_CHANGECBCHAIN, "WM_CHANGECBCHAIN" }, { WM_CHAR, "WM_CHAR" }, { WM_CHARTOITEM, "WM_CHARTOITEM" },
 { WM_CHILDACTIVATE, "WM_CHILDACTIVATE" }, { WM_CLEAR, "WM_CLEAR" }, { WM_CLOSE, "WM_CLOSE" }, { WM_COMMAND, "WM_COMMAND" },
 { WM_COMPACTING, "WM_COMPACTING" }, { WM_COMPAREITEM, "WM_COMPAREITEM" }, { WM_CONTEXTMENU, "WM_CONTEXTMENU" }, { WM_COPY, "WM_COPY" },
 { WM_COPYDATA, "WM_COPYDATA" }, { WM_CREATE, "WM_CREATE" }, { WM_CTLCOLORBTN, "WM_CTLCOLORBTN" }, { WM_CTLCOLORDLG, "WM_CTLCOLORDLG" },
 { WM_CTLCOLOREDIT, "WM_CTLCOLOREDIT" }, { WM_CTLCOLORLISTBOX, "WM_CTLCOLORLISTBOX" }, { WM_CTLCOLORMSGBOX, "WM_CTLCOLORMSGBOX" }, { WM_CTLCOLORSCROLLBAR, "WM_CTLCOLORSCROLLBAR" },
 { WM_CTLCOLORSTATIC, "WM_CTLCOLORSTATIC" }, { WM_CUT, "WM_CUT" }, { WM_DEADCHAR, "WM_DEADCHAR" }, { WM_DELETEITEM, "WM_DELETEITEM" },
 { WM_DESTROY, "WM_DESTROY" }, { WM_DESTROYCLIPBOARD, "WM_DESTROYCLIPBOARD" }, { WM_DEVICECHANGE, "WM_DEVICECHANGE" }, { WM_DEVMODECHANGE, "WM_DEVMODECHANGE" }, { WM_DISPLAYCHANGE, "WM_DISPLAYCHANGE" },
 { WM_DRAWCLIPBOARD, "WM_DRAWCLIPBOARD" }, { WM_DRAWITEM, "WM_DRAWITEM" }, { WM_DROPFILES, "WM_DROPFILES" }, { WM_ENABLE, "WM_ENABLE" },
 { WM_ENDSESSION, "WM_ENDSESSION" }, { WM_ENTERIDLE, "WM_ENTERIDLE" }, { WM_ENTERMENULOOP, "WM_ENTERMENULOOP" }, { WM_ENTERSIZEMOVE, "WM_ENTERSIZEMOVE" },
 { WM_ERASEBKGND, "WM_ERASEBKGND" }, { WM_EXITMENULOOP, "WM_EXITMENULOOP" }, { WM_EXITSIZEMOVE, "WM_EXITSIZEMOVE" }, { WM_FONTCHANGE, "WM_FONTCHANGE" },
 { WM_GETDLGCODE, "WM_GETDLGCODE" }, { WM_GETFONT, "WM_GETFONT" }, { WM_GETHOTKEY, "WM_GETHOTKEY" }, { WM_GETICON, "WM_GETICON" },
 { WM_GETMINMAXINFO, "WM_GETMINMAXINFO" }, { WM_GETTEXT, "WM_GETTEXT" }, { WM_GETTEXTLENGTH, "WM_GETTEXTLENGTH" }, { WM_HANDHELDFIRST, "WM_HANDHELDFIRST" },
 { WM_HANDHELDLAST, "WM_HANDHELDLAST" }, { WM_HELP, "WM_HELP" }, { WM_HOTKEY, "WM_HOTKEY" }, { WM_HSCROLL, "WM_HSCROLL" },
 { WM_HSCROLLCLIPBOARD, "WM_HSCROLLCLIPBOARD" }, { WM_ICONERASEBKGND, "WM_ICONERASEBKGND" }, { WM_INITDIALOG, "WM_INITDIALOG" }, { WM_INITMENU, "WM_INITMENU" },
 { WM_INITMENUPOPUP, "WM_INITMENUPOPUP" }, { WM_INPUTLANGCHANGE, "WM_INPUTLANGCHANGE" }, { WM_INPUTLANGCHANGEREQUEST, "WM_INPUTLANGCHANGEREQUEST" },
 { WM_KEYDOWN, "WM_KEYDOWN" }, { WM_KEYUP, "WM_KEYUP" }, { WM_KILLFOCUS, "WM_KILLFOCUS" }, { WM_MDIACTIVATE, "WM_MDIACTIVATE" },
 { WM_MDICASCADE, "WM_MDICASCADE" }, { WM_MDICREATE, "WM_MDICREATE" }, { WM_MDIDESTROY, "WM_MDIDESTROY" }, { WM_MDIGETACTIVE, "WM_MDIGETACTIVE" },
 { WM_MDIICONARRANGE, "WM_MDIICONARRANGE" }, { WM_MDIMAXIMIZE, "WM_MDIMAXIMIZE" }, { WM_MDINEXT, "WM_MDINEXT" }, { WM_MDIREFRESHMENU, "WM_MDIREFRESHMENU" }, { WM_MDIRESTORE, "WM_MDIRESTORE" },
 { WM_MDISETMENU, "WM_MDISETMENU" }, { WM_MDITILE, "WM_MDITILE" }, { WM_MEASUREITEM, "WM_MEASUREITEM" }, { WM_UNINITMENUPOPUP, "WM_UNINITMENUPOPUP" },
 { WM_MENURBUTTONUP, "WM_MENURBUTTONUP" }, { WM_MENUCOMMAND, "WM_MENUCOMMAND" }, { WM_MENUGETOBJECT, "WM_MENUGETOBJECT" }, { WM_MENUDRAG, "WM_MENUDRAG" },
 { WM_MENUCHAR, "WM_MENUCHAR" }, { WM_MENUSELECT, "WM_MENUSELECT" }, { WM_NEXTMENU, "WM_NEXTMENU" }, { WM_SHOWWINDOW, "WM_SHOWWINDOW" },
 { WM_MOVING, "WM_MOVING" }, { WM_NCACTIVATE, "WM_NCACTIVATE" }, { WM_NCCALCSIZE, "WM_NCCALCSIZE" }, { WM_NCCREATE, "WM_NCCREATE" },
 { WM_NCDESTROY, "WM_NCDESTROY" }, { WM_WINDOWPOSCHANGING, "WM_WINDOWPOSCHANGING" }, { WM_WINDOWPOSCHANGED, "WM_WINDOWPOSCHANGED" },
 { WM_NCLBUTTONUP, "WM_NCLBUTTONUP" }, { WM_NCMBUTTONDBLCLK, "WM_NCMBUTTONDBLCLK" }, { WM_NCMBUTTONDOWN, "WM_NCMBUTTONDOWN" }, { WM_NCMBUTTONUP, "WM_NCMBUTTONUP" },
 { WM_NCXBUTTONDOWN, "WM_NCXBUTTONDOWN" }, { WM_NCXBUTTONUP, "WM_NCXBUTTONUP" }, { WM_NCXBUTTONDBLCLK, "WM_NCXBUTTONDBLCLK" },
 { WM_NCPAINT, "WM_NCPAINT" }, { WM_NCRBUTTONDBLCLK, "WM_NCRBUTTONDBLCLK" }, { WM_SYSCOMMAND, "WM_SYSCOMMAND" }, { WM_GETMINMAXINFO, "WM_GETMINMAXINFO" },
 { WM_NCRBUTTONDOWN, "WM_NCRBUTTONDOWN" }, { WM_NCRBUTTONUP, "WM_NCRBUTTONUP" }, { WM_NEXTDLGCTL, "WM_NEXTDLGCTL" }, { WM_NEXTMENU, "WM_NEXTMENU" }, { WM_NOTIFY, "WM_NOTIFY" }, { WM_NOTIFYFORMAT, "WM_NOTIFYFORMAT" },
 { WM_NULL, "WM_NULL" }, { WM_PAINT, "WM_PAINT" }, { WM_PAINTCLIPBOARD, "WM_PAINTCLIPBOARD" }, { WM_PAINTICON, "WM_PAINTICON" }, { WM_PALETTECHANGED, "WM_PALETTECHANGED" }, { WM_PALETTEISCHANGING, "WM_PALETTEISCHANGING" },
 { WM_PARENTNOTIFY, "WM_PARENTNOTIFY" }, { WM_PASTE, "WM_PASTE" }, { WM_PENWINFIRST, "WM_PENWINFIRST" }, { WM_PENWINLAST, "WM_PENWINLAST" },
 { WM_POWER, "WM_POWER" }, { WM_POWERBROADCAST, "WM_POWERBROADCAST" }, { WM_PRINT, "WM_PRINT" }, { WM_PRINTCLIENT, "WM_PRINTCLIENT" }, { WM_QUERYDRAGICON, "WM_QUERYDRAGICON" }, { WM_QUERYENDSESSION, "WM_QUERYENDSESSION" },
 { WM_QUERYNEWPALETTE, "WM_QUERYNEWPALETTE" }, { WM_QUERYOPEN, "WM_QUERYOPEN" }, { WM_QUEUESYNC, "WM_QUEUESYNC" }, { WM_QUIT, "WM_QUIT" }, { WM_RENDERALLFORMATS, "WM_RENDERALLFORMATS" }, { WM_RENDERFORMAT, "WM_RENDERFORMAT" }, { WM_SETFOCUS, "WM_SETFOCUS" },
 { WM_SETFONT, "WM_SETFONT" }, { WM_SETHOTKEY, "WM_SETHOTKEY" }, { WM_SETICON, "WM_SETICON" }, { WM_SETREDRAW, "WM_SETREDRAW" }, { WM_SETTEXT, "WM_SETTEXT" }, { WM_SETTINGCHANGE, "WM_SETTINGCHANGE" },
 { WM_SHOWWINDOW, "WM_SHOWWINDOW" }, { WM_SIZE, "WM_SIZE" }, { WM_SIZECLIPBOARD, "WM_SIZECLIPBOARD" }, { WM_SIZING, "WM_SIZING" },
 { WM_SPOOLERSTATUS, "WM_SPOOLERSTATUS" }, { WM_STYLECHANGED, "WM_STYLECHANGED" }, { WM_STYLECHANGING, "WM_STYLECHANGING" }, { WM_SYSCHAR, "WM_SYSCHAR" }, { WM_SYSCOLORCHANGE, "WM_SYSCOLORCHANGE" },
 { WM_SYSCOMMAND, "WM_SYSCOMMAND" }, { WM_SYSDEADCHAR, "WM_SYSDEADCHAR" }, { WM_SYSKEYDOWN, "WM_SYSKEYDOWN" }, { WM_SYSKEYUP, "WM_SYSKEYUP" }, { WM_TCARD, "WM_TCARD" },
 { WM_TIMECHANGE, "WM_TIMECHANGE" }, { WM_TIMER, "WM_TIMER" }, { WM_UNDO, "WM_UNDO" }, { WM_USER, "WM_USER" }, { WM_USERCHANGED, "WM_USERCHANGED" }, { WM_VKEYTOITEM, "WM_VKEYTOITEM" },
 { WM_VSCROLL, "WM_VSCROLL" }, { WM_VSCROLLCLIPBOARD, "WM_VSCROLLCLIPBOARD" }, { WM_WINDOWPOSCHANGED, "WM_WINDOWPOSCHANGED" }, { WM_WINDOWPOSCHANGING, "WM_WINDOWPOSCHANGING" },
 { WM_WININICHANGE, "WM_WININICHANGE" }, { WM_KEYFIRST, "WM_KEYFIRST" }, { WM_KEYLAST, "WM_KEYLAST" }, { WM_SYNCPAINT, "WM_SYNCPAINT" }
} };
#endif

int winLookupSurfaceID(HWND Window)
{
   return int(int64_t(GetProp(Window, "SurfaceID")));
}

namespace display {

extern void RepaintWindow(int, int, int, int, int);

/*
static void printerror(void)
{
   LPVOID lpMsgBuf;
   FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
      GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, NULL);
   printf(lpMsgBuf);
   LocalFree(lpMsgBuf);
}
*/

//********************************************************************************************************************

ERR winGetCoords(HWND Window, int &WinX, int &WinY, int &WinWidth, int &WinHeight, int &ClientX, int &ClientY,
   int &ClientWidth, int &ClientHeight)
{
   WINDOWINFO info = { .cbSize = sizeof(info) };

   if (GetWindowInfo(Window, &info)) {
      WinX      = info.rcWindow.left;
      WinY      = info.rcWindow.top;
      WinWidth  = info.rcWindow.right - info.rcWindow.left;
      WinHeight = info.rcWindow.bottom - info.rcWindow.top;
      ClientX      = info.rcClient.left;
      ClientY      = info.rcClient.top;
      ClientWidth  = info.rcClient.right - info.rcClient.left;
      ClientHeight = info.rcClient.bottom - info.rcClient.top;
      return ERR::Okay;
   }
   else return ERR::SystemCall;
}

//********************************************************************************************************************

HDC winGetDC(HWND Window)
{
   return GetDC(Window);
}

void winReleaseDC(HWND Window, HDC DC)
{
   ReleaseDC(Window, DC);
}

//********************************************************************************************************************
// If an error occurs, the DPI values are not updated.

void winGetDPI(int *HDPI, int *VDPI)
{
   // The SetProcessDPIAware() function was introduced in Windows Vista - we use it dynamically.

   static bool dpi_called = false;
   if (!dpi_called) {
      dpi_called = true;
      HMODULE hUser32 = LoadLibrary("user32.dll");
      typedef BOOL (*SetProcessDPIAwareFunc)();
      SetProcessDPIAwareFunc setDPIAware = (SetProcessDPIAwareFunc)GetProcAddress(hUser32, "SetProcessDPIAware");
      if (setDPIAware) setDPIAware();
      FreeLibrary(hUser32);
   }

   if (auto screen = GetDC(nullptr)) {
      *HDPI = GetDeviceCaps(screen, LOGPIXELSX);
      *VDPI = GetDeviceCaps(screen, LOGPIXELSY);
      if (*HDPI < 96) *HDPI = 96;
      if (*VDPI < 96) *VDPI = 96;
      ReleaseDC(nullptr, screen);
   }
}

#if 0 // Alternative method for DPI resolution - does it work better than the other method?
#include <SetupApi.h>

#define DISPLAY_DEVICE_ACTIVE    0x00000001
#define DISPLAY_DEVICE_ATTACHED  0x00000002

#define NAME_SIZE 128
const GUID GUID_CLASS_MONITOR = DEFINE_GUID(GUID_DEVCLASS_MONITOR, 0x4d36e96e, 0xe325, 0x11ce, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18);

char GetMonitorSizeFromEDID(const HKEY hDevRegKey, short *WidthMm, short *HeightMm)
{
    DWORD dwType, AcutalValueNameLength = NAME_SIZE;
    TCHAR valueName[NAME_SIZE];
    int8_t EDIDdata[1024];
    DWORD edidsize=sizeof(EDIDdata);
    int retValue;

    for (int i = 0, retValue = ERROR_SUCCESS; retValue != ERROR_NO_MORE_ITEMS; ++i) {
        retValue = RegEnumValue ( hDevRegKey, i, &valueName[0], &AcutalValueNameLength, nullptr, &dwType, EDIDdata, &edidsize);
        if (retValue != ERROR_SUCCESS || 0 != _tcscmp(valueName,_T("EDID"))) continue;
        *WidthMm  = ((EDIDdata[68] & 0xF0) << 4) + EDIDdata[66];
        *HeightMm = ((EDIDdata[68] & 0x0F) << 8) + EDIDdata[67];
        return TRUE; // valid EDID found
    }
    return FALSE; // EDID not found
}

char GetSizeForDevID(const char * *TargetDevID, short *WidthMm, short *HeightMm)
{
    HDEVINFO devInfo = SetupDiGetClassDevsEx(
        &GUID_CLASS_MONITOR, //class GUID
        nullptr, //enumerator
        nullptr, //HWND
        DIGCF_PRESENT, // Flags //DIGCF_ALLCLASSES|
        nullptr, // device info, create a new one.
        nullptr, // machine name, local machine
        nullptr);// reserved

    if (nullptr == devInfo) return FALSE;

    char bRes = FALSE;

    unsigned int i;
    for (i=0; ERROR_NO_MORE_ITEMS != GetLastError(); ++i) {
      SP_DEVINFO_DATA devInfoData;
      memset(&devInfoData,0,sizeof(devInfoData));
      devInfoData.cbSize = sizeof(devInfoData);

      if (SetupDiEnumDeviceInfo(devInfo,i,&devInfoData)) {
         HKEY hDevRegKey = SetupDiOpenDevRegKey(devInfo,&devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
         if(!hDevRegKey or (hDevRegKey == INVALID_HANDLE_VALUE)) continue;
         bRes = GetMonitorSizeFromEDID(hDevRegKey, WidthMm, HeightMm);
         RegCloseKey(hDevRegKey);
      }
   }
   SetupDiDestroyDeviceInfoList(devInfo);
   return bRes;
}

void get_dpi(void)
{
   DISPLAY_DEVICE dd;
   dd.cb = sizeof(dd);
   DWORD dev = 0; // device index
   char bFoundDevice = FALSE;
   while (EnumDisplayDevices(0, dev, &dd, 0) && !bFoundDevice) {
      DISPLAY_DEVICE ddMon;
      ZeroMemory(&ddMon, sizeof(ddMon));
      ddMon.cb = sizeof(ddMon);
      DWORD devMon = 0;

      while (EnumDisplayDevices(dd.DeviceName, devMon, &ddMon, 0) && !bFoundDevice) {
         if (ddMon.StateFlags & DISPLAY_DEVICE_ACTIVE && !(ddMon.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
            // THIS IS UNTESTED
            char device_id[9], buffer[80];
            vsprintf(buffer, sizeof(buffer), "%s", ddMon.DeviceID);
            for (i=9; buffer[i] and (buffer[i] != '\\'); i++);
            StrCopy(buffer+i, device_id, 8);
            short WidthMm, HeightMm;
            bFoundDevice = GetSizeForDevID(&device_id, &WidthMm, &HeightMm);
         }
         devMon++;
         ZeroMemory(&ddMon, sizeof(ddMon));
         ddMon.cb = sizeof(ddMon);
      }

      ZeroMemory(&dd, sizeof(dd));
      dd.cb = sizeof(dd);
      dev++;
   }
}
#endif

//********************************************************************************************************************
// Key conversion table.  This is used for translating raw values from the keyboard into our keyboard values.

static unsigned char keyconv[256] = {
   0,0,0,0,0,0,0,0,K_BACKSPACE,K_TAB,0,0,K_CLEAR,K_ENTER,0,0,                              // 0x00
   K_L_SHIFT,K_WIN_CONTROL,0,K_PAUSE,K_CAPS_LOCK,0,0,0,0,0,0,K_ESCAPE,0,0,0,0,             // 0x10
   K_SPACE,K_PAGE_UP,K_PAGE_DOWN,K_END,K_HOME,K_LEFT,K_UP,K_RIGHT,K_DOWN,K_SELECT,K_PRINT,K_EXECUTE,K_PRT_SCR,K_INSERT,K_DELETE,K_HELP, // 0x20
   K_ZERO,K_ONE,K_TWO,K_THREE,K_FOUR,K_FIVE,K_SIX,K_SEVEN,K_EIGHT,K_NINE,0,0,0,0,0,0,      // 0x30
   0,K_A,K_B,K_C,K_D,K_E,K_F,K_G,K_H,K_I,K_J,K_K,K_L,K_M,K_N,K_O,                          // 0x40
   K_P,K_Q,K_R,K_S,K_T,K_U,K_V,K_W,K_X,K_Y,K_Z,K_L_COMMAND,K_R_COMMAND,K_MENU,0,K_SLEEP,   // 0x50
   K_NP_0,K_NP_1,K_NP_2,K_NP_3,K_NP_4,K_NP_5,K_NP_6,K_NP_7,K_NP_8,K_NP_9,K_NP_MULTIPLY,K_NP_PLUS,K_NP_BAR,K_NP_MINUS,K_NP_DOT,K_NP_DIVIDE, // 0x60 Numeric keypad keys
   K_F1,K_F2,K_F3,K_F4,K_F5,K_F6,K_F7,K_F8,K_F9,K_F10,K_F11,K_F12,K_F13,K_F14,K_F15,K_F16,  // 0x70
   K_F17,K_F18,K_F19,K_F20,K_F1,K_F2,K_F3,K_F4,0,0,0,0,0,0,0,0,                             // 0x80
   K_NUM_LOCK,K_SCR_LOCK,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                       // 0x90
   K_L_SHIFT,K_R_SHIFT,K_L_CONTROL,K_R_CONTROL,K_L_COMMAND,K_R_COMMAND,0,0,0,0,0,0,0,0,0,0, // 0xa0
   0,0,0,0,0,0,0,0,0,0,K_SEMI_COLON,K_EQUALS,K_COMMA,K_MINUS,K_DOT,K_SLASH,                 // 0xb0
   K_REVERSE_QUOTE,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                           // 0xc0
   0,0,0,0,0,0,0,0,0,0,0,K_L_SQUARE,K_BACK_SLASH,K_R_SQUARE,K_APOSTROPHE,0,                 // 0xd0
   0,0,K_BACK_SLASH,0,0,0,0,0,0,0,0,0,0,0,0,0,                                              // 0xe0
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                                         // 0xf0
};

static int glQualifiers = 0;

static const LPCTSTR glWinCursors[] = {
   IDC_ARROW,
   IDC_SIZENESW,
   IDC_SIZENWSE,
   IDC_SIZENWSE,
   IDC_SIZENESW,
   IDC_SIZEWE,
   IDC_SIZEWE,
   IDC_SIZENS,
   IDC_SIZENS,
   IDC_CROSS,
   IDC_WAIT,
   IDC_SIZEALL,
   IDC_SIZENS,
   IDC_SIZEWE,
   IDC_HAND,
   IDC_HAND,
   IDC_HAND,
   IDC_HAND,
   IDC_IBEAM,
   IDC_ARROW,
   IDC_NO,
   nullptr,        // The invisible cursor is the nullptr type
   nullptr,
   IDC_SIZEALL
};

static unsigned int glCancelAutoPlayMsg = 0;

//********************************************************************************************************************

void winSetCursor(HCURSOR Cursor)
{
   glCurrentCursor = Cursor;
   SetCursor(Cursor);
}

//********************************************************************************************************************

void winSetClassCursor(HWND Window, HCURSOR Cursor)
{
   SetClassLong(Window, GCLP_HCURSOR, (LONG_PTR)Cursor);
}

//********************************************************************************************************************

void winInitCursors(struct WinCursor *Cursor, int Total)
{
   for (int i=0; i < Total; i++) {
      if (glWinCursors[i]) Cursor[i].WinCursor = LoadCursor(nullptr, glWinCursors[i]);
      else Cursor[i].WinCursor = nullptr;
   }

   glCursors = Cursor;
   glCurrentCursor = Cursor[0].WinCursor;
}

//********************************************************************************************************************

void winSetCursorPos(int X, int Y)
{
   if (glMainScreen) {
      POINT point = { .x = X, .y = Y };
      ClientToScreen(glMainScreen, &point);
      SetCursorPos(point.x, point.y);
   }
}

//********************************************************************************************************************

void winShowCursor(int State)
{
   ShowCursor(State);
}

//********************************************************************************************************************

void winFindClose(HANDLE Handle)
{
   FindClose(Handle);
}

//********************************************************************************************************************

HANDLE winFindWindow(char *Class, char *Window)
{
   return FindWindow(Class, Window);
}

//********************************************************************************************************************

void winMinimiseWindow(HWND Window)
{
#if 0

   WINDOWPLACEMENT win;

   win.length = sizeof(win);
   GetWindowPlacement(Window, &win);
   win.showCmd = SW_MINIMIZE;
   win.flags = 0; //WPF_SETMINPOSITION;
   win.ptMinPosition.x = 0;
   win.ptMinPosition.y = 3000;
   SetWindowPlacement(Window, &win);

#else

   ShowWindow(Window, SW_MINIMIZE);

#endif
}

//********************************************************************************************************************

int winReadKey(char *Key, char *Value, unsigned char *Buffer, int Length)
{
   HKEY handle;
   DWORD length;
   int errnum;

   errnum = 0;
   length = Length;
   if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, Key, 0, KEY_READ, &handle) == ERROR_SUCCESS) {
      if (RegQueryValueEx(handle, Value, 0, 0, Buffer, &length) == ERROR_SUCCESS) {
         errnum = 1;
      }
      CloseHandle(handle);
   }
   return errnum;
}

//********************************************************************************************************************

int winGetDisplaySettings(int *bits, int *bytes, int *amtcolours)
{
   DEVMODE devmode;
   devmode.dmSize = sizeof(DEVMODE);
   devmode.dmDriverExtra = 0;

   if (EnumDisplaySettings(nullptr, -1, &devmode)) {
      *bits = devmode.dmBitsPerPel;

      if (*bits <= 8) *bits = 24; // Pretend that the screen is 24 bit even though it is 256 colours, as this produces better results

      if (*bits <= 8) { *amtcolours = 256; *bytes = 1; }
      else if (*bits <= 15) { *amtcolours = 32768; *bytes = 2; }
      else if (*bits <= 16) { *amtcolours = 65536; *bytes = 2; }
      else if (*bits <= 24) { *amtcolours = 16777216; *bytes = 3; }
      else if (*bits <= 32) { *amtcolours = 16777216; *bytes = 4; }

      return 1;
   }
   else return 0;
}

//********************************************************************************************************************

int winGetWindowInfo(HWND Window, int *X, int *Y, int *Width, int *Height, int *Maximised)
{
   if (Window) {
      WINDOWPLACEMENT info;
      info.length = sizeof(WINDOWPLACEMENT);
      if (GetWindowPlacement(Window, &info)) {
         RECT rect;
         if (GetWindowRect(Window, &rect)) {
            if (X) *X = rect.left;
            if (X) *Y = rect.top;
            if (GetClientRect(Window, &rect)) {
               if (Width) *Width  = rect.right  - rect.left;
               if (Height) *Height = rect.bottom - rect.top;

               int rectwidth = rect.right - rect.left;
               HWND winDesktop = GetDesktopWindow();
               GetWindowRect(winDesktop, &rect);

               if (rectwidth >= (rect.right - rect.left)) {
                  *Maximised = 1;
                  if (X) *X = 0;
                  if (Y) *Y = 0;
               }
               else *Maximised = 0;

               return 1;
            }
         }
      }
   }
   return 0;
}

//********************************************************************************************************************

static void HandleMovement(HWND window, WPARAM wparam, LPARAM lparam, bool NonClient)
{
   // Note that if the movement is in the non-client portion of the window, we can't mess with the cursor image.

   if ((glCursorEntry == FALSE) and (!NonClient)) {
      winSetCursor(glDefaultCursor);
      glCursorEntry = TRUE;

      {
         TRACKMOUSEEVENT event;
         ZeroMemory(&event, sizeof(event));
         event.cbSize      = sizeof(event);
         event.dwFlags     = TME_LEAVE;
         event.hwndTrack   = window;
         event.dwHoverTime = HOVER_DEFAULT;
         TrackMouseEvent(&event);
      }
   }

   // Get the absolute position of the mouse pointer relative to the desktop, then convert the coordinates relative to
   // the main window.

   if (auto surface_id = winLookupSurfaceID(window)) {
      POINT point;
      GetCursorPos(&point);
      MsgMovement(surface_id, point.x, point.y, (int)(lparam & 0xffff), (lparam>>16) & 0xffff, NonClient);
   }
}

//********************************************************************************************************************

static void HandleWheel(HWND window, WPARAM wparam, LPARAM lparam)
{
   // Get the absolute position of the mouse pointer relative to the desktop, then convert the coordinates relative to
   // the main window.

   if (auto surface_id = winLookupSurfaceID(window)) {
      double delta = -((double)GET_WHEEL_DELTA_WPARAM(wparam) / (double)WHEEL_DELTA) * 3.0;
      MsgWheelMovement(surface_id, delta);
   }
}

//********************************************************************************************************************

static void HandleButtonPress(HWND window, int button)
{
   // Ensure that the clicked window will have the keyboard focus. This is especially important when running in an
   // embedded window.

   SetFocus(window);

   SetCapture(window);

   // Send a Kotuku message

   MsgButtonPress(button, 1);
}

//********************************************************************************************************************

static void HandleButtonRelease(HWND window, int button)
{
   ReleaseCapture();
   MsgButtonPress(button, 0);
}

//********************************************************************************************************************
// Processes MSG_KEYDOWN messages, which are raw character values (useful for detecting keypresses that have no
// character representation).

static void HandleKeyPress(WPARAM value)
{
   RECT winrect, client, desktop;
   int left, top, width, height;
   POINT point;

   if ((glQualifiers & KQ_CTRL) and (value == VK_F11)) {
      // If CTRL+F11 is pressed, maximise the window to full screen

      if (glMainScreen) {
         GetWindowRect(glMainScreen, &winrect);
         GetClientRect(glMainScreen, &client);
         GetWindowRect(GetDesktopWindow(), &desktop);
         point.x = 0;
         point.y = 0;
         ClientToScreen(glMainScreen, &point);
         left = ((winrect.right - winrect.left) - (client.right - client.left)) / 2;
         top = point.y - winrect.top;

         if ((-left == winrect.left) and (-top == winrect.top)) {
            SetWindowPos(glMainScreen, HWND_NOTOPMOST, 0, 0, desktop.right, desktop.bottom, 0);
         }
         else {
            GetWindowRect(GetDesktopWindow(), &desktop);
            width = desktop.right - desktop.left + (left * 2);
            height = (desktop.bottom - desktop.top) + ((winrect.bottom - winrect.top) - (client.bottom - client.top));
            ShowWindow(glMainScreen, SW_RESTORE);
            SetWindowPos(glMainScreen, HWND_TOPMOST, -left, -top, width, height, 0);
         }
      }
   }

   // Process normal key presses

   uint8_t keystate[256];
   WCHAR printable[2];
   int result;

   printable[0] = 0;

   if (GetKeyboardState(keystate)) {
      glQualifiers = 0;
      if (keystate[VK_LMENU] & 0x80)    glQualifiers |= KQ_L_ALT;
      if (keystate[VK_RMENU] & 0x80)    glQualifiers |= KQ_R_ALT;
      if (keystate[VK_LSHIFT] & 0x80)   glQualifiers |= KQ_L_SHIFT;
      if (keystate[VK_RSHIFT] & 0x80)   glQualifiers |= KQ_R_SHIFT;
      if (keystate[VK_LCONTROL] & 0x80) glQualifiers |= KQ_L_CONTROL;
      if (keystate[VK_RCONTROL] & 0x80) glQualifiers |= KQ_R_CONTROL;
      if (keystate[VK_LWIN] & 0x80)     glQualifiers |= KQ_L_COMMAND;
      if (keystate[VK_RWIN] & 0x80)     glQualifiers |= KQ_R_COMMAND;
      if (keystate[VK_CAPITAL] & 0x80)  glQualifiers |= KQ_CAPS_LOCK;

      // ToUnicode() is a Microsoft function that returns the key status, e.g. the number of keys and whether it is a
      // dead-key or not, and also writes the translated key to the printable buffer.

      result = ToUnicode(value, MapVirtualKey(value, 0), keystate, printable, sizeof(printable)/sizeof(printable[0]), 0);

      int flags = 0;
      if ((value >= 0x60) && (value < 0x70)) flags |= KQ_NUM_PAD;
      if (LOWORD(GetKeyState(VK_CAPITAL)) == 1) flags |= KQ_CAPS_LOCK;
      if (keyconv[value]) MsgKeyPress(flags|glQualifiers, keyconv[value], printable[0]);
      else MSG("No equivalent key value for MS key %d.\n", (int)value);
   }
   else MSG("GetKeyboardState() failed.\n");
}

//********************************************************************************************************************

static void HandleKeyRelease(WPARAM value)
{
   char keystate[256];
   glQualifiers = 0;
   if (GetKeyboardState((unsigned char *)keystate)) {
      if (keystate[VK_LMENU] & 0x80)    glQualifiers |= KQ_L_ALT;
      if (keystate[VK_RMENU] & 0x80)    glQualifiers |= KQ_R_ALT;
      if (keystate[VK_LSHIFT] & 0x80)   glQualifiers |= KQ_L_SHIFT;
      if (keystate[VK_RSHIFT] & 0x80)   glQualifiers |= KQ_R_SHIFT;
      if (keystate[VK_LCONTROL] & 0x80) glQualifiers |= KQ_L_CONTROL;
      if (keystate[VK_RCONTROL] & 0x80) glQualifiers |= KQ_R_CONTROL;
      if (keystate[VK_LWIN] & 0x80)     glQualifiers |= KQ_L_COMMAND;
      if (keystate[VK_RWIN] & 0x80)     glQualifiers |= KQ_R_COMMAND;
      if (keystate[VK_CAPITAL] & 0x80)  glQualifiers |= KQ_CAPS_LOCK;
   }

   if (keyconv[value]) MsgKeyRelease(glQualifiers, keyconv[value]);
}

void CALLBACK msg_timeout(HWND hwnd, UINT msg, UINT_PTR idEvent, DWORD time)
{
   MsgTimer();
}

//********************************************************************************************************************

static LRESULT CALLBACK WindowProcedure(HWND window, UINT msgcode, WPARAM wParam, LPARAM lParam)
{
   PAINTSTRUCT paint;
   HDC hdc;

#ifdef DBGMSG
   if (glCmd.contains(msgcode)) {
      fprintf(stderr, "WinProc: %s, $%.8x, $%.8x, Window: %p\n", glCmd[msgcode], (int)wParam, (int)lParam, window);
   }
   else {
      fprintf(stderr, "WinProc: 0x%x, $%.8x, $%.8x, Window: %p\n", msgcode, (int)wParam, (int)lParam, window);
   }
#endif

   switch (msgcode) {
      case WM_CLIPBOARDUPDATE:
         // Clipboard content has changed by some other application.  NOTE: It is common for some
         // applications to open and close the clipboard multiple times in succession, and this
         // causes multiple event triggers.  This problem is combated using intervals.

         // TODO: A better methodology would be to use a 1 second timer delay to process the clipboard

         glClipboardUpdates++;
         if (GetTickCount() - glIgnoreClip < 2000) return 1;
         else {
            win_clipboard_updated();
            return 0;
         }

      case WM_TIMER:
         MsgTimer(); // Calls ProcessMessages()
         return 0;

      case WM_SETCURSOR:
         switch(LOWORD(lParam)) {
            case HTBOTTOM:       SetCursor(glCursors[7].WinCursor); return TRUE;
            case HTBOTTOMLEFT:   SetCursor(glCursors[4].WinCursor); return TRUE;
            case HTBOTTOMRIGHT:  SetCursor(glCursors[3].WinCursor); return TRUE;
            case HTRIGHT:        SetCursor(glCursors[5].WinCursor); return TRUE;
            case HTLEFT:         SetCursor(glCursors[5].WinCursor); return TRUE;
            case HTTOP:          SetCursor(glCursors[7].WinCursor); return TRUE;
            case HTTOPLEFT:      SetCursor(glCursors[2].WinCursor); return TRUE;
            case HTTOPRIGHT:     SetCursor(glCursors[1].WinCursor); return TRUE;
            default:             SetCursor(glCurrentCursor); return TRUE;
         }

      case WM_CREATE:
         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_NCACTIVATE: // "Sent to a window when its nonclient area needs to be changed to indicate an active or inactive state."
         // When a window is about to be activated, we have the opportunity to prevent that activation.  We'll do this
         // if the surface is marked as non-interactive (e.g. a menu, which will prevent the main window from dropping its focus).
         //
         // lParam = The window that is going to be activated.  This will be 0 if the window belongs to some other task.

         if ((!lParam) or (GetWindowLong((HWND)lParam, WE_INTERACTIVE) == TRUE)) {
            MSG("WM_NCACTIVATE: Allow activation of window %p\n", (HWND)lParam);
            glDeferredActiveWindow = 0;
            return DefWindowProc(window, msgcode, wParam, lParam);
         }
         else { // Tell windows to avoid activating this window
            MSG("WM_NCACTIVATE: Do not activate window %p\n", (HWND)lParam);
            glDeferredActiveWindow = (HWND)lParam;
            return FALSE;
         }

      case WM_SYSCOMMAND:
         // If a popup window is active in our application and the user interacts with the main window menu
         // bar, no defocus event is sent for the popup window because the focus lies with the main window.
         // This little hack ensures that the popup window refers a lost-focus event.

         if (glDeferredActiveWindow) {
            MsgFocusState(winLookupSurfaceID(glDeferredActiveWindow), FALSE);
            glDeferredActiveWindow = 0;
         }
         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_ACTIVATE:
         //HWND otherwindow = lParam;
         if (LOWORD(wParam) == WA_ACTIVE) {
            // Window activated by some method other than a mouse click
         }
         else if (LOWORD(wParam) == WA_CLICKACTIVE) {
            // Window activated by a mouse click
         }
         else if (LOWORD(wParam) == WA_INACTIVE) {
            // Window deactivated
         }

         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_MOUSEACTIVATE:
         // This messages indicates that the mouse has been used on a window that is marked as inactive.  Returning MA_NOACTIVATE prevents the window
         // being activated while also telling windows to convert the mouse action into a message for us to process.

         return MA_NOACTIVATE;

      case WM_ACTIVATEAPP: {
         MSG("WM_ACTIVATEAPP: Focus: %d\n", (int)wParam);
         auto surface = winLookupSurfaceID(window);
         if (wParam) {
            // We have the focus
            MsgFocusState(surface, TRUE);

            #if (_WIN32_WINNT < _WIN32_WINNT_WIN10)
               // Deprecated from Windows 10 onwards
               //XInputEnable(FALSE);
            #endif

            glLastPort = 0;
            for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
               XINPUT_CAPABILITIES cap;
               if (XInputGetCapabilities(i, XINPUT_FLAG_GAMEPAD, &cap) == ERROR_SUCCESS) {
                  glLastPort = i;
               }
               else break;
            }
         }
         else {
            // We have lost the focus
            MsgFocusState(surface, FALSE);
         }
         return 0;
      }

      case WM_MOVE: {
         int wx, wy, wwidth, wheight, cx, cy, cwidth, cheight;
         if (winGetCoords(window, wx, wy, wwidth, wheight, cx, cy, cwidth, cheight) == ERR::Okay) {
            MsgResizedWindow(winLookupSurfaceID(window), wx, wy, wwidth, wheight, cx, cy, cwidth, cheight);
         }
         return 0;
      }

      case WM_SHOWWINDOW:
         return 0;

      case WM_PAINT: {
         if ((hdc = BeginPaint(window, &paint))) {
            RepaintWindow(winLookupSurfaceID(window), paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top);
            EndPaint(window, &paint);
         }
         return 0;
      }

      case WM_SIZE: {
         // The WM_SIZE event reports the new size of the client area.  See WM_SIZING for incoming size requests from the user.

         int cwidth  = LOWORD(lParam);
         int cheight = HIWORD(lParam);

         // If the window has just been maximised, check if the surface object has restrictions on the width and height.
         // If so, force the window back to its previous dimensions so that it obeys the developer's requirements.

         if (wParam & SIZE_MAXIMIZED) {
            WINDOWPLACEMENT win;

            if (auto surface_id = winLookupSurfaceID(window)) {
               auto new_width  = cwidth;
               auto new_height = cheight;
               CheckWindowSize(surface_id, new_width, new_height, cwidth, cheight, AXIS_BOTH);
               if ((new_width != cwidth) or (new_height != cheight)) {
                  ZeroMemory(&win, sizeof(WINDOWPLACEMENT));
                  win.length = sizeof(WINDOWPLACEMENT);
                  GetWindowPlacement(window, &win);
                  win.flags = 0;
                  win.showCmd = SW_RESTORE;
                  SetWindowPlacement(window, &win);
                  return 0;
               }
            }
         }

         // Send a resize message to the surface object

         int wx, wy, wwidth, wheight, cx, cy;
         if (winGetCoords(window, wx, wy, wwidth, wheight, cx, cy, cwidth, cheight) == ERR::Okay) {
            MsgResizedWindow(winLookupSurfaceID(window), wx, wy, wwidth, wheight, cx, cy, cwidth, cheight);
         }
         return 0;
      }

      case WM_SIZING: {
         // This procedure is called when the user is resizing a window by its anchor points.

         RECT winrect, client;
         GetWindowRect(window, &winrect);
         GetClientRect(window, &client);

         const int h_margins = ((winrect.right - winrect.left) - (client.right - client.left));
         const int v_margins = ((winrect.bottom - winrect.top) - (client.bottom - client.top));

         auto rect = (LPRECT)lParam;

         int req_width  = (rect->right - rect->left) - h_margins;
         int req_height = (rect->bottom - rect->top) - v_margins;

         int axis = AXIS_BOTH;
         switch(wParam) {
            case WMSZ_LEFT:
            case WMSZ_RIGHT:
               axis = AXIS_HORIZONTAL;
               break;

            case WMSZ_TOP:
            case WMSZ_BOTTOM:
               axis = AXIS_VERTICAL;
               break;
         }

         CheckWindowSize(winLookupSurfaceID(window), req_width, req_height, client.right - client.left, client.bottom - client.top, axis);

         rect->right  = rect->left + req_width + h_margins;
         rect->bottom = rect->top + req_height + v_margins;
         rect->left   = rect->right - req_width - h_margins;
         rect->top    = rect->bottom - req_height - v_margins;
         return 0;
      }

      case WM_WINDOWPOSCHANGING: {
         // Improve performance by preventing the window from being redrawn when it is moved or resized.
         auto winpos = (LPWINDOWPOS)lParam;
         winpos->flags |= SWP_NOCOPYBITS|SWP_NOREDRAW;
         return 0;
      }

      case WM_KILLFOCUS:     // Window has lost the focus.  Also see WM_SETFOCUS
                             glQualifiers = 0; // Kill stored qualifiers when the keyboard is lost
                             return 0;

      case WM_KEYUP:         HandleKeyRelease(wParam); return 0;

      case WM_KEYDOWN:       HandleKeyPress(wParam); return 0;

      case WM_SYSKEYDOWN:    // The ALT keys are treated differently to everything else
                             if (wParam == VK_MENU) {
                                if (lParam & (1<<24)) HandleKeyPress(VK_RMENU);
                                else HandleKeyPress(VK_MENU);
                             }
                             else HandleKeyPress(wParam);
                             return 0;

      case WM_SYSKEYUP:      if (wParam == VK_MENU) {
                                if (lParam & (1<<24)) HandleKeyRelease(VK_RMENU);
                                else HandleKeyRelease(VK_MENU);
                             }
                             else HandleKeyRelease(wParam);
                             return 0;

      case WM_MOUSEMOVE:     HandleMovement(window, wParam, lParam, false); return 0;

      case WM_MOUSELEAVE:    glCursorEntry = FALSE;
                             return 0;

      case WM_MOUSEWHEEL:    HandleWheel(window, wParam, lParam); return 0;

      case WM_LBUTTONDOWN:   HandleButtonPress(window, WIN_LMB); return 0;
      case WM_RBUTTONDOWN:   HandleButtonPress(window, WIN_RMB); return 0;
      case WM_MBUTTONDOWN:   HandleButtonPress(window, WIN_MMB); return 0;
      case WM_LBUTTONDBLCLK: HandleButtonPress(window, WIN_DBL|WIN_LMB); return 0;
      case WM_RBUTTONDBLCLK: HandleButtonPress(window, WIN_DBL|WIN_RMB); return 0;
      case WM_MBUTTONDBLCLK: HandleButtonPress(window, WIN_DBL|WIN_MMB); return 0;
      case WM_LBUTTONUP:     HandleButtonRelease(window, WIN_LMB); return 0;
      case WM_RBUTTONUP:     HandleButtonRelease(window, WIN_RMB); return 0;
      case WM_MBUTTONUP:     HandleButtonRelease(window, WIN_MMB); return 0;

      case WM_NCMOUSEMOVE:
         HandleMovement(window, wParam, lParam, true);
         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_NCLBUTTONDOWN:
         // Click detected on the titlebar or resize area.  Quirks in the way that Windows manages
         // mouse input mean that we need to signal a button press and release consecutively.
         MsgButtonPress(WIN_LMB|WIN_NONCLIENT, 1);
         MsgButtonPress(WIN_LMB|WIN_NONCLIENT, 0);
         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_NCLBUTTONDBLCLK: // Double-click detected on the titlebar
         MsgButtonPress(WIN_DBL|WIN_LMB|WIN_NONCLIENT, 1);
         MsgButtonPress(WIN_DBL|WIN_LMB|WIN_NONCLIENT, 0);
         return DefWindowProc(window, msgcode, wParam, lParam);

      case WM_ICONNOTIFY:
         if (lParam == WM_LBUTTONDOWN) {
            ShowWindow(window, SW_SHOWNORMAL); // Bring window out of minimisation
            SetForegroundWindow(window); // Focus is required in order to go to the front
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
/*
            int surface_id;
            if ((surface_id = winLookupSurfaceID(window))) {
               MsgShowObject(surface_id);
            }
*/
         }
         else if (lParam == WM_RBUTTONUP) {
#if 0
            ShowWindow(window, SW_SHOWNORMAL);

            POINT point;
            HMENU hMenu, hSubMenu;
            // Get mouse position
            GetCursorPos(&point);
            // Popup context menu
            hMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_MYMENU));
            hSubMenu = GetSubMenu(hMenu, 0);
            SetMenuDefaultItem(hSubMenu, IDM_DEFAULTCMD, FALSE);
            SetForegroundWindow(hMainDlg);         // Per KB Article Q135788
            TrackPopupMenu(hSubMenu, TPM_LEFTBUTTON|TPM_RIGHTBUTTON|TPM_LEFTALIGN, point.x, point.y, 0, hWnd, nullptr);
            PostMessage(hMainDlg, WM_nullptr, 0, 0);   // Per KB Article Q135788
            DestroyMenu(hMenu);
#endif
         }
         break;

      //case WM_CAPTURECHANGED:
         // Sent to the window that is losing the mouse capture, refers to the window gaining the capture.
         // return 0;

      case WM_CLOSE: MsgWindowClose(winLookupSurfaceID(window));
                     return 0;

      case WM_DESTROY: MsgWindowDestroyed(winLookupSurfaceID(window));
                       return 0;

      case WM_SETFOCUS:
         // The window has gained the keyboard focus.  MSDN says this is for displaying a caret if the window accepts text input.

         MSG("WM_SETFOCUS: Surface: %d\n", winLookupSurfaceID(window));
         MsgSetFocus(winLookupSurfaceID(window));
         return 0;

      // Windows hangs on DispatchMessage() when the user tries to resize a window.  Handlers for this can be
      // setup when receiving the following two windows messages.

      case WM_ENTERSIZEMOVE:
         MSG("WM_ENTERSIZEMOVE\n");
         SetTimer(window, IDT_RESIZE_WINDOW, 20, (TIMERPROC)&msg_timeout); // Set a timer to help when DispatchMessage() doesn't return in good time.
         return 0;

      case WM_EXITSIZEMOVE:
         MSG("WM_EXITSIZEMOVE\n");
         KillTimer(window, IDT_RESIZE_WINDOW);
         return 0;

      default:
         // AutoPlay handling - allows you to cancel autoplay, but only if your window is in the foreground.

         if (!glCancelAutoPlayMsg) {
            glCancelAutoPlayMsg = RegisterWindowMessage(TEXT("QueryCancelAutoPlay"));
         }

         if (msgcode == glCancelAutoPlayMsg) {
            fprintf(stderr, "An AutoPlay window has been cancelled.\n");
            return TRUE;       // cancel auto-play
         }

         return DefWindowProc(window, msgcode, wParam, lParam);
   }

   return 0;
}

//********************************************************************************************************************

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
void winSetSurfaceID(HWND Window, int SurfaceID)
{
   SetProp(Window, "SurfaceID", (HANDLE)SurfaceID);
   SetWindowLong(Window, WE_SURFACE, SurfaceID);
   SetWindowLong(Window, WE_KEY, KEY_SURFACE);
}
#pragma GCC diagnostic pop

//********************************************************************************************************************

void winDisableBatching(void)
{
   GdiSetBatchLimit(1);
}

//********************************************************************************************************************

ERR winReadController(int Port, double *Values, CON &Buttons)
{
   constexpr double tolerance = 0.08; // At-rest dead zone tolerance for thumb sticks
   XINPUT_STATE state;
   if (XInputGetState(Port, &state) == ERROR_SUCCESS) {
      Values[0] = double(state.Gamepad.bLeftTrigger) * (1.0 / 255.0);
      Values[1] = double(state.Gamepad.bRightTrigger) * (1.0 / 255.0);
      Values[2] = std::clamp(double(state.Gamepad.sThumbLX) * (1.0 / 32767.0), -1.0, 1.0);
      Values[3] = std::clamp(double(state.Gamepad.sThumbLY) * (1.0 / 32767.0), -1.0, 1.0);
      Values[4] = std::clamp(double(state.Gamepad.sThumbRX) * (1.0 / 32767.0), -1.0, 1.0);
      Values[5] = std::clamp(double(state.Gamepad.sThumbRY) * (1.0 / 32767.0), -1.0, 1.0);

      if ((Values[2] < tolerance) and (Values[2] > -tolerance) and
          (Values[3] < tolerance) and (Values[3] > -tolerance)) {
         Values[2] = 0;
         Values[3] = 0;
      }

      if ((Values[4] < tolerance) and (Values[4] > -tolerance) and
          (Values[5] < tolerance) and (Values[5] > -tolerance)) {
         Values[4] = 0;
         Values[5] = 0;
      }

      if (state.Gamepad.wButtons) {
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) Buttons |= CON::DPAD_UP;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) Buttons |= CON::DPAD_DOWN;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) Buttons |= CON::DPAD_LEFT;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) Buttons |= CON::DPAD_RIGHT;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) Buttons |= CON::START;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) Buttons |= CON::SELECT;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) Buttons |= CON::LEFT_THUMB;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) Buttons |= CON::RIGHT_THUMB;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) Buttons |= CON::LEFT_BUMPER_1;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) Buttons |= CON::RIGHT_BUMPER_1;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) Buttons |= CON::GAMEPAD_S;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) Buttons |= CON::GAMEPAD_E;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) Buttons |= CON::GAMEPAD_W;
         if (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) Buttons |= CON::GAMEPAD_N;
      }
      else Buttons = CON::NIL; //state.Gamepad.wButtons;
      return ERR::Okay;
   }
   else return ERR::SystemCall;
}

//********************************************************************************************************************

int winGetDesktopSize(int *width, int *height)
{
   *width = 800;
   *height = 600;
   if (auto window = GetDesktopWindow()) {
      RECT rect;
      if (GetWindowRect(window, &rect)) {
         *width  = rect.right;
         *height = rect.bottom;
         return 1;
      }
   }
   return 0;
}

//********************************************************************************************************************

int winCreateScreenClass(void)
{
   WNDCLASSEX winclass;

   winCreateScreenClassClipboard();

   if (!glCancelAutoPlayMsg) {
      glCancelAutoPlayMsg = RegisterWindowMessage(TEXT("QueryCancelAutoPlay"));
   }

   glDefaultCursor = LoadCursor(nullptr, IDC_ARROW);

   winclass.cbSize        = sizeof(winclass);
   winclass.style         = CS_DBLCLKS|CS_VREDRAW|CS_HREDRAW;
   winclass.lpfnWndProc   = WindowProcedure;
   winclass.cbClsExtra    = 0;
   winclass.cbWndExtra    = sizeof(struct winextra);
   winclass.hInstance     = glInstance;
   if (!(winclass.hIcon = LoadIcon(glInstance, MAKEINTRESOURCE(500)))) winclass.hIcon = LoadIcon(glInstance, IDI_APPLICATION);
   winclass.hCursor       = nullptr; //glDefaultCursor;
   winclass.hbrBackground = nullptr;
   winclass.lpszMenuName  = nullptr;
   winclass.lpszClassName = "ScreenClass";
   winclass.hIconSm       = nullptr;

   if (RegisterClassEx(&winclass)) {
      glScreenClassInit = 1;

      if (!glOleInit) {
         HRESULT result = OleInitialize(nullptr);
         if (result == S_OK) glOleInit = 1; // 1 = Successful initialisation
         else if (result == S_FALSE) glOleInit = 2; // 2 = Attempted initialisation failed.
      }

      return 1;
   }
   else return 0;
}

/*********************************************************************************************************************
** WS_EX_ACCEPTFILES: Specifies that a window created with this style accepts drag-drop files.
** WS_EX_APPWINDOW: Forces a top-level window onto the taskbar when the window is visible.
** WS_EX_CLIENTEDGE: Specifies that a window has a border with a sunken edge.
** WS_EX_CONTROLPARENT: The window itself contains child windows that should take part in dialog box navigation. If this style is specified, the dialog manager recurses into children of this window when performing navigation operations such as handling the TAB key, an arrow key, or a keyboard mnemonic.
** WS_EX_DLGMODALFRAME: Creates a window that has a double border; the window can, optionally, be created with a title bar by specifying the WS_CAPTION style in the dwStyle parameter.
** WS_EX_LAYERED: Windows 2000/XP: Creates a layered window. Note that this cannot be used for child windows. Also, this cannot be used if the window has a class style of either CS_OWNDC or CS_CLASSDC.
** WS_EX_NOACTIVATE: Windows 2000/XP: A top-level window created with this style does not become the foreground window when the user clicks it. The system does not bring this window to the foreground when the user minimizes or closes the foreground window.  To activate the window, use the SetActiveWindow or SetForegroundWindow function.  The window does not appear on the taskbar by default. To force the window to appear on the taskbar, use the WS_EX_APPWINDOW style.
** WS_EX_NOINHERITLAYOUT: Windows 2000/XP: A window created with this style does not pass its window layout to its child windows.
** WS_EX_NOPARENTNOTIFY: Specifies that a child window created with this style does not send the WM_PARENTNOTIFY message to its parent window when it is created or destroyed.
** WS_EX_OVERLAPPEDWINDOW: Combines the WS_EX_CLIENTEDGE and WS_EX_WINDOWEDGE styles.
** WS_EX_STATICEDGE: Creates a window with a three-dimensional border style intended to be used for items that do not accept user input.
** WS_EX_TOPMOST: Specifies that a window created with this style should be placed above all non-topmost windows and should stay above them, even when the window is deactivated. To add or remove this style, use the SetWindowPos function.
** WS_EX_TRANSPARENT: Specifies that a window created with this style should not be painted until siblings beneath the window (that were created by the same thread) have been painted. The window appears transparent because the bits of underlying sibling windows have already been painted.  To achieve transparency without these restrictions, use the SetWindowRgn function.
** WS_EX_WINDOWEDGE: Specifies that a window has a border with a raised edge.
**********************************************************************************************************************
** WS_BORDER: Creates a window that has a thin-line border.
** WS_CAPTION: Creates a window that has a title bar (includes the WS_BORDER style).
** WS_CHILD: Creates a child window. A window with this style cannot have a menu bar. This style cannot be used with the WS_POPUP style.
** WS_CLIPCHILDREN: Excludes the area occupied by child windows when drawing occurs within the parent window. This style is used when creating the parent window.
** WS_CLIPSIBLINGS: Clips child windows relative to each other; that is, when a particular child window receives a WM_PAINT message, the WS_CLIPSIBLINGS style clips all other overlapping child windows out of the region of the child window to be updated. If WS_CLIPSIBLINGS is not specified and child windows overlap, it is possible, when drawing within the client area of a child window, to draw within the client area of a neighboring child window.
** WS_DISABLED: Creates a window that is initially disabled. A disabled window cannot receive input from the user. To change this after a window has been created, use EnableWindow.
** WS_DLGFRAME: Creates a window that has a border of a style typically used with dialog boxes. A window with this style cannot have a title bar.
** WS_GROUP: Specifies the first control of a group of controls. The group consists of this first control and all controls defined after it, up to the next control with the WS_GROUP style. The first control in each group usually has the WS_TABSTOP style so that the user can move from group to group. The user can subsequently change the keyboard focus from one control in the group to the next control in the group by using the direction keys.  You can turn this style on and off to change dialog box navigation. To change this style after a window has been created, use SetWindowLong.
** WS_ICONIC: Creates a window that is initially minimized. Same as the WS_MINIMIZE style.
** WS_MAXIMIZE: Creates a window that is initially maximized.
** WS_MAXIMIZEBOX: Creates a window that has a maximize button. Cannot be combined with the WS_EX_CONTEXTHELP style. The WS_SYSMENU style must also be specified.
** WS_MINIMIZE: Creates a window that is initially minimized. Same as the WS_ICONIC style.
** WS_MINIMIZEBOX: Creates a window that has a minimize button. Cannot be combined with the WS_EX_CONTEXTHELP style. The WS_SYSMENU style must also be specified.
** WS_OVERLAPPED: Creates an overlapped window. An overlapped window has a title bar and a border. Same as the WS_TILED style.
** WS_OVERLAPPEDWINDOW: Creates an overlapped window with the WS_OVERLAPPED, WS_CAPTION, WS_SYSMENU, WS_THICKFRAME, WS_MINIMIZEBOX, and WS_MAXIMIZEBOX styles. Same as the WS_TILEDWINDOW style.
** WS_POPUP: Creates a pop-up window. This style cannot be used with the WS_CHILD style.
** WS_POPUPWINDOW: Creates a pop-up window with WS_BORDER, WS_POPUP, and WS_SYSMENU styles. The WS_CAPTION and WS_POPUPWINDOW styles must be combined to make the window menu visible.
** WS_SIZEBOX: Creates a window that has a sizing border. Same as the WS_THICKFRAME style.
** WS_SYSMENU: Creates a window that has a window menu on its title bar. The WS_CAPTION style must also be specified.
** WS_TABSTOP: Specifies a control that can receive the keyboard focus when the user presses the TAB key. Pressing the TAB key changes the keyboard focus to the next control with the WS_TABSTOP style. You can turn this style on and off to change dialog box navigation. To change this style after a window has been created, use SetWindowLong.
** WS_THICKFRAME: Creates a window that has a sizing border. Same as the WS_SIZEBOX style.
** WS_TILED: Creates an overlapped window. An overlapped window has a title bar and a border. Same as the WS_OVERLAPPED style.
** WS_TILEDWINDOW: Creates an overlapped window with the WS_OVERLAPPED, WS_CAPTION, WS_SYSMENU, WS_THICKFRAME, WS_MINIMIZEBOX, and WS_MAXIMIZEBOX styles. Same as the WS_OVERLAPPEDWINDOW style.
** WS_VISIBLE: Creates a window that is initially visible. This style can be turned on and off by using ShowWindow or SetWindowPos.
*/

HWND winCreateScreen(HWND PopOver, int *X, int *Y, int *Width, int *Height, char Maximise, char Borderless, const char *Name,
   char Composite, unsigned char Opacity, char Desktop)
{
   if (!Name) Name = "Kotuku";

   bool interactive;
   if ((Borderless) and (!glTrayIcon) and (!glTaskBar)) interactive = FALSE;
   else interactive = TRUE;

   HWND Window;
   if (Borderless) {
      if (!(Window = CreateWindowEx(
            (glTaskBar ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW) | (glStickToFront ? WS_EX_TOPMOST : 0),
            "ScreenClass", (glTaskBar ? Name : nullptr),
            WS_POPUP|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|(Maximise ? WS_MAXIMIZE : 0),
            *X, *Y,
            CW_USEDEFAULT, CW_USEDEFAULT,
            (HWND)PopOver,
            (HMENU)nullptr,
            glInstance, nullptr))) return nullptr;
   }
   else if (!(Window = CreateWindowEx(
      (glTaskBar ? WS_EX_APPWINDOW : 0) | WS_EX_WINDOWEDGE | (glStickToFront ? WS_EX_TOPMOST : 0),
      "ScreenClass", Name,
      WS_SIZEBOX|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|(Maximise ? WS_MAXIMIZE : 0),
      *X, *Y,
      CW_USEDEFAULT, CW_USEDEFAULT,
      (HWND)PopOver,
      (HMENU)nullptr,
      glInstance, nullptr))) return nullptr;

   // Set the width and height of the window

   RECT winrect, client;
   GetWindowRect(Window, &winrect);
   GetClientRect(Window, &client);
   int width = *Width + (winrect.right - client.right - winrect.left);
   int height = *Height + (winrect.bottom - client.bottom - winrect.top);
   MoveWindow(Window, winrect.left, winrect.top, width, height, FALSE);

   // Return the absolute coordinates of the client region

   int dummy;
   winGetCoords(Window, dummy, dummy, dummy, dummy, *X, *Y, *Width, *Height);

   if (glTrayIcon) {
      NOTIFYICONDATA nid;
      nid.cbSize = sizeof(NOTIFYICONDATA);
      nid.hWnd   = Window;
      nid.uID    = ID_TRAY;
      nid.uFlags = NIF_ICON | NIF_MESSAGE;
      nid.uCallbackMessage = WM_ICONNOTIFY;
      if (!(nid.hIcon = LoadIcon(glInstance, MAKEINTRESOURCE(500)))) nid.hIcon = LoadIcon(glInstance, IDI_APPLICATION);
      Shell_NotifyIcon(NIM_ADD, &nid);
   }

   if (glStickToFront > 0) glStickToFront--;

   if ((Composite) or (Opacity < 255)) {
      SetLastError(0);
      if (!SetWindowLong(Window, GWL_EXSTYLE, GetWindowLong(Window, GWL_EXSTYLE) | WS_EX_LAYERED)) {
         if (!GetLastError()) {
            return nullptr;
         }
      }

      if (!Composite) {
         if (!SetLayeredWindowAttributes(Window, 0, Opacity, LWA_ALPHA)) {
            return nullptr;
         }
      }
   }

   SetWindowLong(Window, WE_INTERACTIVE, interactive);
   SetWindowLong(Window, WE_BORDERLESS, Borderless);

   if ((Desktop) and (!glMainScreen)) glMainScreen = Window;

   AddClipboardFormatListener(Window);

   winInitDragDrop(Window);

   return Window;
}

/*********************************************************************************************************************
** This is often used for creating windowed areas inside another application, such as a web browser.
*/

HWND winCreateChild(HWND Parent, int X, int Y, int Width, int Height)
{
   if (auto Window = CreateWindowEx(
         0, // WS_EX_NOPARENTNOTIFY
         "ScreenClass", "Kotuku Child Window",
         WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
         0, 0, Width, Height,
         Parent,
         (HMENU)0,
         glInstance, 0)) {

      glMainScreen = Window;

      return Window;
   }
   else return nullptr;
}

//********************************************************************************************************************

int winHideWindow(HWND window)
{
   return ShowWindow(window, SW_HIDE);
}

//********************************************************************************************************************

void winMoveToBack(HWND Window)
{
   SetWindowPos(Window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
}

//********************************************************************************************************************

void winFocus(HWND Window)
{
   SetForegroundWindow(Window);
}

//********************************************************************************************************************

void winMoveToFront(HWND Window)
{
   // Note: The window will require the focus if it is to be moved in front of other MS Windows on the desktop (a window can gain the focus with SetForegroundWindow()).

   SetWindowPos(Window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);

/* This code demonstrates a way to move a window to the front without giving
   it the focus.

  SetWindowPos(Window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
  SetWindowPos(Window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
*/
}

//********************************************************************************************************************

void winGetWindowTitle(HWND Window, char *Buffer, int Size)
{
   GetWindowText(Window, Buffer, Size);
}

//********************************************************************************************************************

void winSetWindowTitle(HWND window, const char *title)
{
   SetWindowText(window, title);
}

//********************************************************************************************************************

void winUpdateWindow(HWND hWnd)
{
   UpdateWindow(hWnd);
}

//********************************************************************************************************************

HINSTANCE winGetModuleHandle(void)
{
   return GetModuleHandle(nullptr);
}

//********************************************************************************************************************

int winDestroyWindow(HWND window)
{
   NOTIFYICONDATA notify;

   if (window == glMainScreen) glMainScreen = nullptr;

   ZeroMemory(&notify, sizeof(notify));
   notify.cbSize = sizeof(notify);
   notify.uID    = ID_TRAY;
   notify.hWnd   = window;
   notify.uFlags = 0;
   Shell_NotifyIcon(NIM_DELETE, &notify);

   return DestroyWindow(window);
}

//********************************************************************************************************************

void winRemoveWindowClass(const char *ClassName)
{
   UnregisterClass(ClassName, glInstance);
}

/*********************************************************************************************************************
** The coordinates are interpreted as being indicative of the client area.
*/

int winMoveWindow(HWND Window, int X, int Y)
{
   WINDOWINFO info;

   info.cbSize = sizeof(info);
   if (GetWindowInfo(Window, &info)) {
      X = X - (info.rcClient.left - info.rcWindow.left);
      Y = Y - (info.rcClient.top - info.rcClient.top);
      return MoveWindow(Window, X, Y, info.rcWindow.right - info.rcWindow.left, info.rcWindow.bottom - info.rcWindow.top, TRUE);
   }
   else return 0;
}

/*********************************************************************************************************************
** The coordinates are interpreted as being relative to the client area.
*/

int winResizeWindow(HWND Window, int X, int Y, int Width, int Height)
{
   WINDOWINFO info;

   info.cbSize = sizeof(info);
   if (!GetWindowInfo(Window, &info)) return 0;

   if (X == 0x7fffffff) X = info.rcClient.left;
   if (Y == 0x7fffffff) Y = info.rcClient.top;

   // Return if the current size is the same as the 'new' size

   if ((Width == (info.rcClient.right - info.rcClient.left)) and
       (Height == (info.rcClient.bottom - info.rcClient.top)) and
       (X == info.rcClient.left) and
       (Y == info.rcClient.top)) return 1;

   // Convert the client coordinates to window coordinates

   X = X - (info.rcClient.left - info.rcWindow.left);
   Y = Y - (info.rcClient.top - info.rcWindow.top);
   Width = Width + (info.rcClient.left - info.rcWindow.left) + (info.rcWindow.right - info.rcClient.right);
   Height = Height + (info.rcClient.top - info.rcWindow.top) + (info.rcWindow.bottom - info.rcClient.bottom);

   // Perform the resize

   return MoveWindow(Window, X, Y, Width, Height, TRUE);
}

//********************************************************************************************************************

void winGetMargins(HWND Window, int *Left, int *Top, int *Right, int *Bottom)
{
   WINDOWINFO info;

   info.cbSize = sizeof(info);
   if (!GetWindowInfo(Window, &info)) return;

   *Left   = info.rcClient.left - info.rcWindow.left;
   *Top    = info.rcClient.top - info.rcWindow.top;
   *Right  = info.rcWindow.right - info.rcClient.right;
   *Bottom = info.rcWindow.bottom - info.rcClient.bottom;
}

//********************************************************************************************************************

int winSettings(int Flags)
{
   if (Flags & WNS_PLUGIN) {
//      KillMessageHook();
   }
   return 0;
}

//********************************************************************************************************************

void precalc_rgb(unsigned char *Data, unsigned char *Dest, int Width, int Height)
{
   unsigned char *data, *dest;
   int x, y;

   for (y=0; y < Height; y++) {
      data = Data + (Width * 4 * y);
      dest = Dest + (Width * 4 * y);
      for (x=0; x < Width; x++) {
         dest[3] = data[3];
         dest[0]= ((int)data[0] * (int)dest[3])>>8;
         dest[1]= ((int)data[1] * (int)dest[3])>>8;
         dest[2]= ((int)data[2] * (int)dest[3])>>8; // divide by 255;
         data += 4;
         dest += 4;
      }
   }
}

void win32RedrawWindow(HWND Window, HDC WindowDC, int X, int Y, int Width,
   int Height, int XDest, int YDest, int ScanWidth, int ScanHeight,
   int BPP, unsigned char *Data, int RedMask, int GreenMask, int BlueMask, int AlphaMask, unsigned char Opacity)
{
   POINT ptSrc = { 0, 0 };
   SIZE size;
   char direct_blit;
   BITMAPV4HEADER info;
   RECT rect;
   unsigned char *alpha_data;

   info.bV4Size          = sizeof(info);
   info.bV4Width         = ScanWidth;     // Width in pixels
   info.bV4Height        = -ScanHeight;   // Height in pixels
   info.bV4Planes        = 1;             // Always 1
   info.bV4BitCount      = BPP;           // Bits per pixel
   info.bV4SizeImage     = 0;
   info.bV4XPelsPerMeter = 0;
   info.bV4YPelsPerMeter = 0;
   info.bV4ClrUsed       = 0;
   info.bV4ClrImportant  = 0;
   info.bV4RedMask       = RedMask;
   info.bV4GreenMask     = GreenMask;
   info.bV4BlueMask      = BlueMask;
   info.bV4AlphaMask     = AlphaMask;
   info.bV4CSType        = 0;
   info.bV4GammaRed      = 0;
   info.bV4GammaGreen    = 0;
   info.bV4GammaBlue     = 0;

   // NB: wingdi.h sometimes defines bV4Compression as bV4V4Compression

   if (BPP == 24) info.bV4V4Compression = BI_RGB; // Must use BI_RGB in 24bit mode, or GDI does nothing
   else info.bV4V4Compression = BI_BITFIELDS; // Must use BI_BITFIELDS and set the RGB masks in other packed modes

   if (info.bV4BitCount == 15) info.bV4BitCount = 16;

   direct_blit = TRUE;
   if (GetWindowLong(Window, GWL_EXSTYLE) & WS_EX_LAYERED) {
      if (AlphaMask) {
         BLENDFUNCTION blend_alpha = { AC_SRC_OVER, 0, Opacity, AC_SRC_ALPHA };

         GetWindowRect(Window,&rect);
         size.cx = rect.right - rect.left;
         size.cy = rect.bottom - rect.top;

         if (auto dcMemory = CreateCompatibleDC(WindowDC)) {
            if (auto bmp = CreateDIBSection(WindowDC, (BITMAPINFO *)&info, DIB_RGB_COLORS, (void **)&alpha_data, nullptr, 0)) {
               //printf("bpp %d, XD %d, YD %d, Width %d, Height %d, X %d, Y %d, ScanHeight %d\n", (int)BPP, XDest, YDest, Width, Height, X, Y, ScanHeight);

               precalc_rgb(Data, alpha_data, ScanWidth, ScanHeight);

               // SetDIBitsToDevice() defines the size of the window for UpdateLayeredWindow().  This is crazy, because
               // that means that the entire layer needs to be updated every time, making this process terribly slow.
               // However MS documentation confirms as much in the API documentation.

               X = 0;
               Y = 0;
               XDest = 0;
               YDest = 0;
               Width = size.cx;
               Height = size.cy;

               auto pOldBitmap = SelectObject(dcMemory, bmp);
               SetDIBitsToDevice(dcMemory, XDest, YDest, Width, Height, X, ScanHeight - (Y + Height), 0, ScanHeight, alpha_data, (BITMAPINFO *)&info, DIB_RGB_COLORS);

               UpdateLayeredWindow(Window, nullptr, nullptr, &size, dcMemory, &ptSrc, 0, &blend_alpha, ULW_ALPHA);

               direct_blit = FALSE;
               SelectObject(dcMemory, pOldBitmap);
               DeleteObject(bmp);
            }
            DeleteDC(dcMemory);
         }
      }
      else SetLayeredWindowAttributes(Window, 0, Opacity, LWA_ALPHA);
   }

   if (direct_blit) {
      SetDIBitsToDevice(WindowDC, XDest, YDest, Width, Height, X, ScanHeight - (Y + Height), 0, ScanHeight, Data, (BITMAPINFO *)&info, DIB_RGB_COLORS);
   }
}

//********************************************************************************************************************

int winGetPixelFormat(int *redmask, int *greenmask, int *bluemask, int *alphamask)
{
   PIXELFORMATDESCRIPTOR  pfd;
   const unsigned char formats[] = { 0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff };
   static int mred=0, mgreen=0, mblue=0, malpha=0;

   // WARNING: Calling DescribePixelFormat() causes layered windows to flicker for some bizarre reason.  Therefore this routine has been modified so that DescribePixelFormat() is only called once.

   if (!mred) {
      if (DescribePixelFormat(GetDC(nullptr), 1, sizeof(PIXELFORMATDESCRIPTOR), &pfd)) {
         if (pfd.cRedBits <= 8) mred = formats[pfd.cRedBits] << pfd.cRedShift;
         if (pfd.cGreenBits <= 8) mgreen = formats[pfd.cGreenBits] << pfd.cGreenShift;
         if (pfd.cBlueBits <= 8) mblue = formats[pfd.cBlueBits] << pfd.cBlueShift;
         if (pfd.cAlphaBits <= 8) malpha = formats[pfd.cAlphaBits] << pfd.cAlphaShift;
      }
   }

   if (mred) {
      *redmask   = mred;
      *greenmask = mgreen;
      *bluemask  = mblue;
      *alphamask = malpha;
      return 0;
   }
   else return -1;
}

//********************************************************************************************************************

void winGetError(int Error, char *Buffer, int BufferSize)
{
   FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, Error, 0, Buffer, BufferSize, 0);
}

//********************************************************************************************************************

void winDrawRectangle(HDC hdc, int x, int y, int width, int height, uint8_t red, uint8_t green, uint8_t blue)
{
   RECT rect;

   rect.left   = x;
   rect.top    = y;
   rect.right  = x + width;
   rect.bottom = y + height;
   auto brush = CreateSolidBrush(RGB(red,green,blue));
   FillRect(hdc, &rect, brush);
   DeleteObject(brush);
}

//********************************************************************************************************************

int winBlit(HDC dest, int xdest, int ydest, int width, int height, HDC src, int x, int y)
{
   if (!BitBlt(dest, xdest, ydest, width, height, src, x, y, SRCCOPY)) {
      return GetLastError();
   }
   else return 0;
}

//********************************************************************************************************************

void * winCreateCompatibleDC(void)
{
   return CreateCompatibleDC(nullptr);
}

//********************************************************************************************************************

void winSetDIBitsToDevice(HDC hdc, int xdest, int ydest, int width, int height,
        int xstart, int ystart, int scanwidth, int scanheight, int bpp, void *data,
        int redmask, int greenmask, int bluemask)
{
   BITMAPV4HEADER info;

   info.bV4Size          = sizeof(info);
   info.bV4Width         = scanwidth;     // Width in pixels
   info.bV4Height        = -scanheight;   // Height in pixels
   info.bV4Planes        = 1;             // Always 1
   info.bV4BitCount      = bpp;           // Bits per pixel
   info.bV4SizeImage     = 0;
   info.bV4XPelsPerMeter = 0;
   info.bV4YPelsPerMeter = 0;
   info.bV4ClrUsed       = 0;
   info.bV4ClrImportant  = 0;
   info.bV4RedMask       = redmask;
   info.bV4GreenMask     = greenmask;
   info.bV4BlueMask      = bluemask;
   info.bV4AlphaMask     = 0;
   info.bV4CSType        = 0;
   info.bV4GammaRed      = 0;
   info.bV4GammaGreen    = 0;
   info.bV4GammaBlue     = 0;

   // NB: wingdi.h sometimes defines bV4Compression as bV4V4Compression

   if (bpp == 24) info.bV4V4Compression = BI_RGB; // Must use BI_RGB in 24bit mode, or GDI does nothing
   else info.bV4V4Compression = BI_BITFIELDS; // Must use BI_BITFIELDS and set the RGB masks in other packed modes

   if (info.bV4BitCount == 15) info.bV4BitCount = 16;

   ystart = scanheight - (ystart + height);
   SetDIBitsToDevice(hdc, xdest, ydest, width, height, xstart, ystart, 0, scanheight, data, (BITMAPINFO *)&info, DIB_RGB_COLORS);
}

//********************************************************************************************************************

void winDeleteDC(HDC hdc)
{
   DeleteDC(hdc);
}

//********************************************************************************************************************

HBITMAP winCreateBitmap(int width, int height, int bpp)
{
   return CreateBitmap(width, height, 1, bpp, nullptr);
}

//********************************************************************************************************************

void winTerminate(void)
{
   winTerminateClipboard();

   if (glScreenClassInit) {
      UnregisterClass("ScreenClass", GetModuleHandle(nullptr));
      glScreenClassInit = 0;
   }

   if (glOleInit == 1) {
      OleUninitialize();
      glOleInit = 0;
   }
}

//********************************************************************************************************************

int winShowWindow(HANDLE window, int Maximise)
{
   int result;

   if (GetWindowLong(HWND(window), WE_BORDERLESS) == TRUE) {
      // Raw surfaces (composites, borderless windows etc) do not get the focus automatically.
      // This mirrors the functionality within the Kotuku desktop.

      result = ShowWindow(HWND(window), Maximise ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
   }
   else {
      if ((!Maximise) and (IsIconic(HWND(window)))) {
         // Window is minimised - restore it to its original position
         result = ShowWindow(HWND(window), SW_RESTORE);
      }
      else result = ShowWindow(HWND(window), Maximise ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
   }

   return result;
}

} // namespace
