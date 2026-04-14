
#ifdef KOTUKU_MAIN_H
typedef void * HWND;
typedef void * HDC;
typedef void * HBITMAP;
typedef void * HANDLE;
typedef void * HINSTANCE;
typedef void * WNDPROC;
typedef void * HCURSOR;
enum class PTC : int;
#endif

struct WinCursor {
   #ifdef _WIN32
   HCURSOR WinCursor;
   #else
   void * WinCursor;
   #endif
   #ifdef KOTUKU_MAIN_H
   PTC CursorID;
   #else
   int CursorID;
   #endif
};

struct WinDT {
   int Datatype;
   int Length;
   void *Data;
};

#if !defined(KOTUKU_MAIN_H) && defined(__cplusplus)
enum class CON : unsigned int {
   NIL = 0,
   GAMEPAD_S = 0x00000001,
   GAMEPAD_E = 0x00000002,
   GAMEPAD_W = 0x00000004,
   GAMEPAD_N = 0x00000008,
   DPAD_UP = 0x00000010,
   DPAD_DOWN = 0x00000020,
   DPAD_LEFT = 0x00000040,
   DPAD_RIGHT = 0x00000080,
   START = 0x00000100,
   SELECT = 0x00000200,
   LEFT_BUMPER_1 = 0x00000400,
   LEFT_BUMPER_2 = 0x00000800,
   RIGHT_BUMPER_1 = 0x00001000,
   RIGHT_BUMPER_2 = 0x00002000,
   LEFT_THUMB = 0x00004000,
   RIGHT_THUMB = 0x00008000
};

DEFINE_ENUM_FLAG_OPERATORS(CON)
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern int glIgnoreClip;
extern int glClipboardUpdates;
extern int8_t glOleInit;

int winLookupSurfaceID(HWND);
void winCreateScreenClassClipboard(void);
void winDragDropFromHost_Drop(int, char *); // lib_surfaces.cpp
void win_clipboard_updated(void); // class_clipboard.cpp
void winTerminateClipboard(void);

#ifdef __cplusplus
} // extern C
#endif

#ifdef __cplusplus
namespace display {

extern "C" int winAddClip(int, const void *, int, int);
extern "C" int winAddFileClip(const char16_t *, int, int);
extern "C" void winClearClipboard(void);
extern "C" void winCopyClipboard(void);
extern "C" int winExtractFile(void *, int, char *, int);
extern "C" void winGetClip(int);
extern "C" int winInitDragDrop(HWND);
extern "C" int winCurrentClipboardID(void);
extern "C" int winGetData(char *, struct WinDT **, int *);

extern void winTerminate(void);
extern HDC winGetDC(HWND);
extern void winReleaseDC(HWND, HDC);
extern void winSetSurfaceID(HWND, int);
extern int winBlit(void *, int, int, int, int, void *, int, int);
extern void winGetError(int, char *, int);
extern void * winCreateCompatibleDC(void);
extern HBITMAP winCreateBitmap(int width, int height, int bpp);
extern void winDeleteDC(void *);
extern void winDrawRectangle(void *, int, int, int, int, unsigned char, unsigned char, unsigned char);
extern int winGetPixelFormat(int *, int *, int *, int *);
extern void winSetDIBitsToDevice(void *, int, int, int, int, int, int, int, int, int, void *, int, int, int);

extern void win32RedrawWindow(HWND, HDC, int X, int Y, int Width,
   int Height, int XDest, int YDest, int ScanWidth, int ScanHeight,
   int BPP, unsigned char *Data, int RedMask, int GreenMask, int BlueMask, int AlphaMask, unsigned char Opacity);

extern void MsgKeyPress(int, int, int);
extern void MsgKeyRelease(int, int);
extern void MsgMovement(int, double, double, int, int, bool);
extern void MsgWheelMovement(int, float);
extern void MsgButtonPress(int, int);
extern void MsgFocusState(int SurfaceID, int State);
extern void MsgTotalControllerPorts(int, int);
extern void MsgResizedWindow(int, int, int, int, int, int, int, int, int);
extern void MsgSetFocus(int SurfaceID);
extern void MsgSwitchWindowType(HWND, int);
extern void MsgTimer(void);
extern void MsgWindowClose(int SurfaceID);
extern void MsgWindowDestroyed(int SurfaceID);

#define AXIS_VERTICAL 1
#define AXIS_HORIZONTAL 2
#define AXIS_BOTH 3

extern void CheckWindowSize(int, int &, int &, int, int, int = AXIS_BOTH);

void Win32ManagerLoop(void);

extern void winGetDPI(int *, int *);
extern HWND winCreateChild(HWND, int, int, int, int);
extern HWND winCreateScreen(HWND, int *, int *, int *, int *, char, char, const char *, char, unsigned char, char);
extern int winCreateScreenClass(void);
extern void winDisableBatching(void);
extern void winRemoveWindowClass(const char *);
extern int winProcessMessage(void *);
extern int winDestroyWindow(HWND);
extern int winDetermineWindow(int, int, int *, int *, int *);
extern void winFindClose(HANDLE);
extern HANDLE winFindWindow(char *, char *);
extern void winFocus(HWND);
extern void winFreeDragDrop(void);
extern ERR winGetCoords(HWND, int &, int &, int &, int &, int &, int &, int &, int &);
extern int winGetDesktopSize(int *, int *);
extern int winGetDisplaySettings(int *, int *, int *);
extern void winGetMargins(HWND, int *, int *, int *, int *);
extern HINSTANCE winGetModuleHandle(void);
extern int winGetWindowInfo(HWND, int *, int *, int *, int *, int *);
extern void winGetWindowTitle(HWND, char *, int);
extern int winHideWindow(HWND);
extern void winInitCursors(struct WinCursor *, int);
extern void winMinimiseWindow(HWND);
extern int winMoveWindow(HWND, int, int);
extern void winMoveToBack(HWND);
extern void winMoveToFront(HWND);
extern ERR winReadController(int, double *, CON &);
extern int winReadKey(char *, char *, unsigned char *, int);
extern int winResizeWindow(HWND, int, int, int, int);
extern void winSetCursorPos(int X, int Y);
extern void winShowCursor(int);
extern void winSetCursor(HCURSOR);
extern void winSetSurfaceID(HWND, int);
extern int winSettings(int);
extern void winSetWindowTitle(HWND, const char *);
extern void winUpdateWindow(HWND);
extern void winTerminate(void);
extern HDC winGetDC(HWND);
extern void winReleaseDC(HWND, HDC);
extern int winShowWindow(void *, int);
} // namespace

#endif
