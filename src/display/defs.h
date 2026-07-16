#pragma once

#define __system__
//#define DEBUG
#define PRIVATE_DISPLAY
#define PRV_DISPLAY_MODULE
#define PRV_BITMAP
#define PRV_DISPLAY
#define PRV_POINTER
#define PRV_CLIPBOARD
#define PRV_SURFACE
#define PRV_CONTROLLER
//#define DBG_DRAW_ROUTINES // Use this if you want to debug any external code that is subscribed to surface drawing routines
//#define FASTACCESS
//#define DBG_LAYERS
#define FOCUSMSG(...) //LogF(NULL, __VA_ARGS__)

#include <kotuku/system/errors.h>

#ifdef DBG_LAYERS
#include <stdio.h>
#endif

#include <unordered_set>
#include <mutex>
#include <atomic>
#include <queue>
#include <sstream>
#include <array>
#include <memory>
#include <new>
#include <vector>
#include <math.h>

#ifdef __linux__
 #include <dlfcn.h>
 #include <stdlib.h>
 #include <signal.h>
 #include <fcntl.h>
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/utsname.h>
 #include <sys/wait.h>
 #include <sys/mman.h>
 #include <errno.h>
#endif

#ifdef __xwindows__
 #include <X11/Xlib.h>
 #include <X11/Xos.h>
 #include <X11/keysym.h>
 #include <X11/XKBlib.h>
 #include <X11/keysymdef.h>
 #include <X11/Xproto.h>
 #include <X11/extensions/XShm.h>
 #include <X11/cursorfont.h>
 #include <stdlib.h>
 #include <X11/Xlib.h>
 #include <X11/Xos.h>
 #include <X11/Xutil.h>
 #include <sys/shm.h>
 #include <stdio.h>

 #ifdef XDGA_ENABLED
  #include <X11/extensions/Xxf86dga.h> // Requires libxxf86dga-dev
 #endif

 #ifdef XRANDR_ENABLED
  #include <X11/extensions/Xrandr.h> // Requires libxrandr-dev
 #endif
#endif

#ifdef _GLES_
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#endif

#ifdef __ANDROID__
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/configuration.h>
#endif

#define USE_XIMAGE 1

constexpr bool REPEAT_BUTTONS    = true;
constexpr int SIZE_FOCUSLIST     = 30;
constexpr int DEFAULT_WHEELSPEED = 500;
constexpr int TIME_DBLCLICK      = 40;
constexpr int MAX_CURSOR_WIDTH   = 32;
constexpr int MAX_CURSOR_HEIGHT  = 32;
constexpr int DRAG_XOFFSET       = 10;
constexpr int DRAG_YOFFSET       = 12;

constexpr uint8_t BF_DATA     = 0x01;
constexpr uint8_t BF_WINVIDEO = 0x02;

constexpr int BLEND_MAX_THRESHOLD = 255;
constexpr int BLEND_MIN_THRESHOLD = 1;

#define ALIGN32(a) (((a) + 3) & (~3))

#define SURFACE_READ      (0x0001)   // Read access
#define SURFACE_WRITE     (0x0002)   // Write access
#define SURFACE_READWRITE (SURFACE_READ|SURFACE_WRITE)

#include <kotuku/modules/display.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/processes.h>
#include <kotuku/modules/compression.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/regex.h>
#include <kotuku/modules/config.h>
#include <kotuku/modules/script.h>
#include <kotuku/strings.hpp>
#include "../link/linear_rgb.h"
#include "../link/unicode.h"

using namespace kt;
class extBitmap;

extern SWIN glpWindowType;
extern PTC get_cursor_id(std::string_view Name);

#define UpdateSurfaceRecord(a) update_surface_copy(a)

struct SurfaceRecord {
   APTR     Data;          // For drwCopySurface()
   OBJECTID ParentID;      // Object that owns the surface area
   OBJECTID SurfaceID;     // ID of the surface area
   OBJECTID BitmapID;      // Shared bitmap buffer, if available
   OBJECTID DisplayID;     // Display
   OBJECTID RootID;        // RootLayer
   OBJECTID PopOverID;
   RNF      Flags;         // Surface flags (RNF::VISIBLE etc)
   int      X;             // Horizontal coordinate
   int      Y;             // Vertical coordinate
   int      Width;         // Width
   int      Height;        // Height
   int      Left;          // Absolute X
   int      Top;           // Absolute Y
   int      Right;         // Absolute right coordinate
   int      Bottom;        // Absolute bottom coordinate
   int16_t  Level;         // Level number within the hierarchy
   int16_t  LineWidth;     // [applies to the bitmap owner]
   int8_t   BytesPerPixel; // [applies to the bitmap owner]
   int8_t   BitsPerPixel;  // [applies to the bitmap owner]
   int8_t   Cursor;        // Preferred cursor image ID
   uint8_t  Opacity;       // Current opacity setting, 0 - 255

   inline void setArea(int pLeft, int pTop, int pRight, int pBottom) {
      Left   = pLeft;
      Top    = pTop;
      Right  = pRight;
      Bottom = pBottom;
   }

   inline ClipRectangle area() const {
      return ClipRectangle(Left, Top, Right, Bottom);
   }

   inline bool hasFocus() const { return (Flags & RNF::HAS_FOCUS) != RNF::NIL; }
   inline void dropFocus() { Flags &= RNF::HAS_FOCUS; }
   inline bool transparent() const { return (Flags & RNF::TRANSPARENT) != RNF::NIL; }
   inline bool visible() const { return (Flags & RNF::VISIBLE) != RNF::NIL; }
   inline bool invisible() const { return (Flags & RNF::VISIBLE) IS RNF::NIL; }
   inline bool isVolatile() const { return (Flags & RNF::VOLATILE) != RNF::NIL; }
   inline bool isCursor() const { return (Flags & RNF::CURSOR) != RNF::NIL; }
};

static inline uint8_t surface_opacity_to_byte(double Opacity)
{
   if (Opacity <= 0.0) return 0;
   if (Opacity >= 1.0) return 255;
   return uint8_t(Opacity * 255.0 + 0.5);
}

typedef std::vector<SurfaceRecord> SURFACELIST;
extern std::recursive_mutex glSurfaceLock;

class WinHook {
public:
   OBJECTID SurfaceID;
   WH Event;

   WinHook(OBJECTID aSurfaceID, WH aEvent) : SurfaceID(aSurfaceID), Event(aEvent) { };

   bool operator== (const WinHook &rhs) const {
      return (SurfaceID == rhs.SurfaceID) and (Event == rhs.Event);
   }

   bool operator() (const WinHook &lhs, const WinHook &rhs) const {
       if (lhs.SurfaceID == rhs.SurfaceID) return uint8_t(lhs.Event) < uint8_t(rhs.Event);
       else return lhs.SurfaceID < rhs.SurfaceID;
   }
};

namespace std {
   template <> struct hash<WinHook> {
      std::size_t operator()(const WinHook &k) const {
         return ((std::hash<OBJECTID>()(k.SurfaceID)
            ^ (std::hash<uint8_t>()(uint8_t(k.Event)) << 1)) >> 1);
      }
   };
}

enum {
   STAGE_PRECOPY=1,
   STAGE_AFTERCOPY,
   STAGE_COMPOSITE
};

static const auto MT_PtrSetWinCursor  = AC(-1);
static const auto MT_PtrGrabX11Pointer = AC(-2);
static const auto MT_PtrUngrabX11Pointer = AC(-3);

struct ptrSetWinCursor { PTC Cursor;  };
struct ptrGrabX11Pointer { OBJECTID SurfaceID;  };

inline ERR ptrSetWinCursor(OBJECTPTR Ob, PTC Cursor) {
   struct ptrSetWinCursor args = { Cursor };
   return Action(MT_PtrSetWinCursor, Ob, &args);
}

#define ptrUngrabX11Pointer(obj) Action(MT_PtrUngrabX11Pointer,(obj),0)

inline ERR ptrGrabX11Pointer(OBJECTPTR Ob, OBJECTID SurfaceID) {
   struct ptrGrabX11Pointer args = { SurfaceID };
   return Action(MT_PtrGrabX11Pointer, Ob, &args);
}

#include "idl.h"

#ifdef __ANDROID__
#include <kotuku/modules/android.h>
#endif

struct resolution {
   int16_t width;
   int16_t height;
   int16_t bpp;
};

extern std::vector<InputEvent> glInputEvents;

// Each input event subscription is registered as an InputCallback

class InputCallback {
public:
   OBJECTID SurfaceFilter;
   JTYPE    InputMask; // JTYPE flags
   FUNCTION Callback;

   bool operator==(const InputCallback &other) const {
      return (SurfaceFilter == other.SurfaceFilter);
   }
};

namespace std {
   template <>
   struct hash<InputCallback>
   {
      std::size_t operator()(const InputCallback& k) const {
         return (k.SurfaceFilter);
      }
   };
}

//********************************************************************************************************************

struct ClipItem {
   std::string Path; // Path to a file containing the data.
   std::vector<char> Data; // Vector containing raw data.

   ClipItem(std::string pPath) : Path(pPath) { }
};

struct ClipRecord {
   CLIPTYPE Datatype; // The type of data clipped
   CEF Flags;         // CEF::DELETE may be set for the 'cut' operation
   std::vector<ClipItem> Items;  // List of file locations referencing all the data in this clip entry

   ~ClipRecord();
   ClipRecord(CLIPTYPE pDatatype, CEF pFlags, const std::vector<ClipItem> pItems) :
      Datatype(pDatatype), Flags(pFlags), Items(pItems) { }
};

//********************************************************************************************************************

extern std::vector<SurfaceRecord> glSurfaces;

//********************************************************************************************************************

class extPointer : public objPointer {
   struct ButtonClick {
      int64_t LastClickTime = 0;    // Timestamp of recorded click
      OBJECTID LastClicked = 0;     // Most recently clicked object for this button
      uint8_t DblClick:1 = false;   // TRUE if last click was a double-click
   };

   public:
   using create = kt::Create<extPointer>;

   std::vector<ButtonClick> ButtonClicks;
   int64_t    ClickTime;
   int64_t    AnchorTime;
   double   LastClickX, LastClickY;
   double   LastReleaseX, LastReleaseY;
   OBJECTID LastSurfaceID;      // Last object that the pointer was positioned over
   OBJECTID CursorReleaseID;
   OBJECTID DragSurface;        // Draggable surface anchored to the pointer position
   OBJECTID DragParent;         // Parent of the draggable surface
   int      CursorRelease;
   // Changes to the cursor can be buffered until the pointer is released
   PTC      BufferCursor;
   CRF      BufferFlags;
   OBJECTID BufferOwner;
   OBJECTID BufferObject;
   char     DragData[8];          // Data preferences for current drag & drop item
   char     Device[32];
   std::string ButtonOrder;       // The order of the first 11 buttons can be changed here
   int16_t     ButtonOrderFlags[12]; // Button order represented as JD flags
   uint8_t    prvOverCursorID;

   extPointer(objMetaClass *ClassPtr, OBJECTID ObjectID) : objPointer(ClassPtr, ObjectID) {
      CursorID = PTC::DEFAULT;
      ClickSlop = 2;
      ButtonClicks.resize(3); // 0 = LMB, 1 = RMB, 2 = MMB

      Speed        = 160;
      Acceleration = 0.8;
      MaxSpeed     = 100;
      WheelSpeed   = DEFAULT_WHEELSPEED;
      DoubleClick  = 0.36;
      ButtonOrder  = "123456789ABCDEF";

      // Currently unused because all current targets have their own cursor management
      #if 0
      if (auto config = objConfig::create { fl::Path("user:config/pointer.cfg") }; config.ok()) {
         config->read("POINTER", "Speed", Speed);
         config->read("POINTER", "Acceleration", Acceleration);
         config->read("POINTER", "MaxSpeed", MaxSpeed);
         config->read("POINTER", "WheelSpeed", WheelSpeed);
         config->read("POINTER", "DoubleClick", DoubleClick);
         config->read("POINTER", "ButtonOrder", ButtonOrder);

         if (DoubleClick < 0.2) DoubleClick = 0.2;

         if (MaxSpeed < 2) MaxSpeed = 2;
         else if (MaxSpeed > 200) MaxSpeed = 200;
      }
      #endif

   }

   ~extPointer();
};

class extSurface : public objSurface {
   public:
   using create = kt::Create<extSurface>;

   int64_t    LastRedimension;      // Timestamp of the last redimension call
   objBitmap *Bitmap;
   std::vector<FUNCTION> Callback;
   APTR     Data;
   double   Opacity;
   WINHANDLE DisplayWindow;       // Reference to the platform dependent window representing the Surface object
   OBJECTID PrevModalID;          // Previous surface to have been modal
   OBJECTID BitmapOwnerID;        // The surface object that owns the root bitmap
   OBJECTID RevertFocusID;
   int      LineWidth;            // Bitmap line width, in bytes
   int      ListIndex;            // Last known list index
   int      InputHandle;          // Input handler for dragging of surfaces
   SWIN     WindowType;           // See SWIN constants
   TIMER    RedrawTimer;          // For ScheduleRedraw()
   int16_t FixedWidth, FixedHeight, FixedX, FixedY, FixedXO, FixedYO;
   uint16_t InheritedRoot:1;      // TRUE if the user set the RootLayer manually
   uint16_t ParentDefined:1;      // TRUE if the parent field was set manually
   uint16_t RedrawScheduled:1;
   uint16_t RedrawCountdown;      // Unsubscribe from the timer when this value reaches zero.
   uint8_t  RefreshRate;          // Cached copy of the display refresh rate.  0 = Not queried
   uint8_t  RedrawRate;           // Last refresh rate used for scheduled redrawing
   int8_t   BitsPerPixel;         // Bitmap bits per pixel
   int8_t   BytesPerPixel;        // Bitmap bytes per pixel

   extSurface(objMetaClass *ClassPtr, OBJECTID ObjectID) : objSurface(ClassPtr, ObjectID) {
      Opacity    = 1.0;
      WindowType = glpWindowType;
   }

   ~extSurface();

   inline void setFixedPosition(int X, int Y) {
      FixedX = std::clamp(X, -0x8000, 0x7fff);
      FixedY = std::clamp(Y, -0x8000, 0x7fff);
   }

   inline void setFixedSize(int Width, int Height) {
      FixedWidth  = std::clamp(Width, -0x8000, 0x7fff);
      FixedHeight = std::clamp(Height, -0x8000, 0x7fff);
   }

   inline void setFixedArea(int X, int Y, int Width, int Height) {
      setFixedPosition(X, Y);
      setFixedSize(Width, Height);
   }
};

class extDisplay : public objDisplay {
   public:
   using create = kt::Create<extDisplay>;

   std::string Manufacturer;
   std::string Chipset;
   std::string Display;
   std::string DisplayMfr;
   double Opacity;

   double Gamma[3];          // Red, green, blue gamma radioactivity indicator
   std::vector<struct resolution> Resolutions;
   FUNCTION  ResizeFeedback;
   int  ControllerPorts;
   int  VDensity;          // Cached DPI value, if calculable.
   int  HDensity;
   #ifdef __xwindows__
   union {
      APTR   WindowHandle;
      Window XWindowHandle;
   };
   Pixmap XPixmap;
   #elif __ANDROID__
      ANativeWindow *WindowHandle;
   #else
      APTR   WindowHandle;
   #endif

   extDisplay(objMetaClass *ClassPtr, OBJECTID ObjectID) : objDisplay(ClassPtr, ObjectID) {
      if (NewLocalObject(CLASSID::BITMAP, &Bitmap) != ERR::Okay) {
         kt::Log().fatal(ERR::NewObject);
      }

      OBJECTID id;
      if (FindObject("SystemVideo", CLASSID::NIL, &id) != ERR::Okay) SetName(Bitmap, "SystemVideo");

      if (not Name[0]) {
         if (FindObject("SystemDisplay", CLASSID::NIL, &id) != ERR::Okay) SetName(this, "SystemDisplay");
      }

      #ifdef __xwindows__

         Chipset      = "X11";
         Display      = "X Windows";
         DisplayMfr   = "N/A";
         Manufacturer = "N/A";

      #elif _WIN32

         Chipset      = "Windows";
         Display      = "Windows";
         DisplayMfr   = "N/A";
         Manufacturer = "N/A";

      #elif _GLES_

         Chipset      = "OpenGLES";
         Display      = "OpenGL";
         DisplayMfr   = "N/A";
         Manufacturer = "N/A";

      #else

         Chipset      = "Unknown";
         Display      = "Unknown";
         DisplayMfr   = "Unknown";
         Manufacturer = "Unknown";

      #endif

      Width       = 800;
      Height      = 600;
      RefreshRate = -1;
      Gamma[0]    = 1.0;
      Gamma[1]    = 1.0;
      Gamma[2]    = 1.0;
      Opacity     = 1.0;

      #ifdef __xwindows__
         DisplayType = DT::X11;
      #elif _WIN32
         DisplayType = DT::WINGDI;
      #elif _GLES_
         DisplayType = DT::GLES;
      #else
         DisplayType = DT::NATIVE;
      #endif
   }

   ~extDisplay();
};

extern void clean_clipboard(void);
extern ERR  create_bitmap_class(void);
extern ERR  create_clipboard_class(void);
extern ERR  create_controller_class(void);
extern ERR  create_display_class(void);
extern ERR  create_pointer_class(void);
extern ERR  create_surface_class(void);
extern ERR  get_surface_abs(OBJECTID, int *, int *, int *, int *);
extern void input_event_loop(HOSTHANDLE, APTR);
extern ERR  lock_surface(extBitmap *, int16_t);
extern ERR  unlock_surface(extBitmap *);
extern ERR  get_display_info(OBJECTID, DisplayInfo *);
extern void resize_feedback(FUNCTION *, OBJECTID, int X, int Y, int Width, int Height);

extern void forbidDrawing(void);
extern void forbidExpose(void);
extern void permitDrawing(void);
extern void permitExpose(void);
extern ERR  apply_style(OBJECTPTR, OBJECTPTR, CSTRING);
extern ERR  load_styles(void);
extern int find_bitmap_owner(const SURFACELIST &, int);
extern void move_layer(extSurface *, int, int);
extern void move_layer_pos(SURFACELIST &, int, int);
extern void prepare_background(extSurface *, const SURFACELIST &, int, extBitmap *, const ClipRectangle &, int8_t);
extern void process_surface_callbacks(extSurface *, extBitmap *);
extern void refresh_pointer(extSurface *Self);
extern ERR  track_layer(extSurface *);
extern void untrack_layer(OBJECTID);
extern int8_t restrict_region_to_parents(const SURFACELIST &, int, ClipRectangle &, bool);
extern ERR  load_style_values(void);
extern ERR  resize_layer(extSurface *, int X, int Y, int, int, int, int, int BPP, double, int);
extern void redraw_nonintersect(OBJECTID, const SURFACELIST &, int, const ClipRectangle &, const ClipRectangle &, IRF, EXF);
extern ERR  _expose_surface(OBJECTID, const SURFACELIST &, int, int, int, int, int, EXF);
extern ERR  _redraw_surface(OBJECTID, const SURFACELIST &, int, int, int, int, int, IRF);
extern void _redraw_surface_do(extSurface *, const SURFACELIST &, int, ClipRectangle &, extBitmap *, IRF);
[[maybe_unused]] extern void check_styles(STRING Path, OBJECTPTR *Script);
extern ERR  update_surface_copy(extSurface *);
extern ERR  update_display(extDisplay *, extBitmap *, int X, int Y, int Width, int Height, int XDest, int YDest);
extern void get_resolutions(extDisplay *);

extern ERR RedrawSurface(OBJECTID, int, int, int, int, IRF);

#ifdef DBG_LAYERS
extern void print_layer_list(STRING Function, SurfaceControl *Ctl, int POI)
#endif

extern bool glSixBitDisplay;
extern OBJECTPTR glModule, glDisplayContext;
extern OBJECTPTR clDisplay, clPointer, clBitmap, clClipboard, clSurface, clController;
extern OBJECTID glPointerID;
extern DisplayInfo glDisplayInfo;
extern objCompression *glCompress;
extern struct CoreBase *CoreBase;
extern ColourFormat glColourFormat;
extern bool glHeadless;
extern TIMER glRefreshPointerTimer;
extern extBitmap *glComposite;
extern double glpRefreshRate, glpGammaRed, glpGammaGreen, glpGammaBlue;
extern int glpDisplayWidth, glpDisplayHeight, glpDisplayX, glpDisplayY;
extern int glpDisplayDepth; // If zero, the display depth will be based on the hosted desktop's bit depth.
extern int glpMaximise, glpFullScreen;
extern char glpDPMS[20];
extern std::unique_ptr<std::array<uint16_t, 256 * 256>> glDemultiply;
extern std::array<uint8_t, 256 * 256> glAlphaLookup;
extern std::list<ClipRecord> glClips;
extern std::recursive_mutex glClipboardLock;
extern std::atomic<int> glLastPort;

extern ankerl::unordered_dense::map<WinHook, FUNCTION> glWindowHooks;
extern std::recursive_mutex glWindowHookLock;
extern std::vector<OBJECTID> glFocusList;
extern std::recursive_mutex glFocusLock;
extern std::recursive_mutex glSurfaceLock;
extern std::recursive_mutex glInputLock;

inline void release_display_callback(FUNCTION &Function)
{
   if (Function.defined()) {
      if (Function.isScript() and (not Function.stale())) ((objScript *)Function.Context)->derefProcedure(Function);
      Function.unpin();
      Function.disable();
   }
}

// Thread-specific variables.

extern thread_local int16_t tlNoDrawing, tlNoExpose, tlVolatileIndex;
extern thread_local OBJECTID tlFreeExpose;

struct InputType {
   JTYPE Flags;  // As many flags as necessary to describe the input type
   JTYPE Mask;   // Limited flags to declare the mask that must be used to receive that type
};

extern const InputType glInputType[int(JET::END)];
extern const CSTRING glInputNames[int(JET::END)];

//********************************************************************************************************************

#ifdef _GLES_ // OpenGL related prototypes
GLenum alloc_texture(int Width, int Height, GLuint *TextureID);
void refresh_display_from_egl(objDisplay *Self);
ERR init_egl(void);
void free_egl(void);
#endif

extern uint8_t glTrayIcon, glTaskBar, glStickToFront;

#ifdef _WIN32

#define DLLCALL // __declspec(dllimport)
#define WINAPI  __stdcall

extern "C" {
DLLCALL int WINAPI SetPixelV(APTR, int, int, int);
DLLCALL int WINAPI SetPixel(APTR, int, int, int);
DLLCALL int WINAPI GetPixel(APTR, int, int);
}

#include "win32/windows.h"

HCURSOR GetWinCursor(PTC CursorID);

extern WinCursor winCursors[24];

#endif // _WIN32

#ifdef __xwindows__

struct X11Globals {
   bool Manager;
   bool WSLg;
   int PixelsPerLine; // Defined by DGA
   int BankSize; // Definfed by DGA
};

extern void X11ManagerLoop(HOSTHANDLE, APTR);
extern void handle_button_press(XEvent *);
extern void handle_button_release(XEvent *);
extern void handle_configure_notify(XConfigureEvent *);
extern void handle_enter_notify(XCrossingEvent *);
extern void handle_exposure(XExposeEvent *);
extern void handle_key_press(XEvent *);
extern void handle_key_release(XEvent *);
extern void handle_motion_notify(XMotionEvent *);
extern void handle_stack_change(XCirculateEvent *);
extern void init_xcursors(void);
extern void free_xcursors(void);
extern ERR resize_pixmap(extDisplay *, int, int);
extern ERR xr_set_display_mode(int *, int *);

extern int16_t glDGAAvailable;
extern APTR glDGAMemory;
extern XVisualInfo glXInfoAlpha;
extern X11Globals glX11;
extern _XDisplay *XDisplay;
extern bool glX11ShmImage;
extern bool glXCompositeSupported;
extern uint8_t KeyHeld[int(KEY::LIST_END)];
extern KQ glKeyFlags;
extern int glXFD, glDGAPixelsPerLine, glDGABankSize;
extern Atom atomSurfaceID, XWADeleteWindow, XWATakeFocus;
extern GC glXGC, glClipXGC;
extern XWindowAttributes glRootWindow;
extern Window glDisplayWindow;
extern Cursor C_Default;
extern OBJECTPTR modXRR;
extern int16_t glPlugin;
extern APTR glDGAVideo;
extern bool glXRRAvailable;

#endif

#include "prototypes.h"

//********************************************************************************************************************
// Lazily releases a stale ResizeFeedback subscription.  The display must be locked by the caller because copies of
// the FUNCTION share the stored field's weak pin, so only the owning field may be unpinned and cleared.

inline void release_stale_resize_feedback(extDisplay *Display)
{
   if (Display->ResizeFeedback.stale()) {
      Display->ResizeFeedback.unpin();
      Display->ResizeFeedback.clear();
   }
}

//********************************************************************************************************************

template <typename T>
void UpdateSurfaceField(objSurface *Self, T SurfaceRecord::*LValue, T Value)
{
   if (Self->initialised()) {
      const std::lock_guard<std::recursive_mutex> lock(glSurfaceLock);
      for (auto &record : glSurfaces) {
         if (record.SurfaceID IS Self->UID) {
            record.*LValue = Value;
            return;
         }
      }
   }
}

//********************************************************************************************************************

inline void clip_rectangle(ClipRectangle &rect, ClipRectangle &clip)
{
   if (rect.Left   < clip.Left)   rect.Left   = clip.Left;
   if (rect.Top    < clip.Top)    rect.Top    = clip.Top;
   if (rect.Right  > clip.Right)  rect.Right  = clip.Right;
   if (rect.Bottom > clip.Bottom) rect.Bottom = clip.Bottom;
}

//********************************************************************************************************************

inline int find_bitmap_owner(int Index)
{
   return find_bitmap_owner(glSurfaces, Index);
}

//********************************************************************************************************************
// Surface list lookup routines.

inline int find_surface_list(extSurface *Surface, int Limit = -1)
{
   if (Limit IS -1) Limit = int(glSurfaces.size());
   else if (Limit > int(glSurfaces.size())) {
      kt::Log(__FUNCTION__).warning("Invalid Limit parameter of %d (max %d)", Limit, int(glSurfaces.size()));
      Limit = int(glSurfaces.size());
   }

   for (int i=0; i < Limit; i++) {
      if (glSurfaces[i].SurfaceID IS Surface->UID) return i;
   }

   return -1;
}

inline int find_surface_list(OBJECTID SurfaceID, int Limit = -1)
{
   if (Limit IS -1) Limit = int(glSurfaces.size());
   else if (Limit > int(glSurfaces.size())) {
      kt::Log(__FUNCTION__).warning("Invalid Limit parameter of %d (max %d)", Limit, int(glSurfaces.size()));
      Limit = int(glSurfaces.size());
   }

   for (int i=0; i < Limit; i++) {
      if (glSurfaces[i].SurfaceID IS SurfaceID) return i;
   }

   return -1;
}

inline int find_parent_list(const SURFACELIST &list, extSurface *Self)
{
   if ((Self->ListIndex < std::ssize(list)) and (list[Self->ListIndex].SurfaceID IS Self->UID)) {
      for (int i=Self->ListIndex-1; i >= 0; i--) {
         if (list[i].SurfaceID IS Self->ParentID) return i;
      }
   }

   return find_surface_list(Self->ParentID);
}

//********************************************************************************************************************

class extBitmap : public objBitmap {
   public:
   using create = kt::Create<extBitmap>;

   uint32_t  *Gradients;
   APTR   ResolutionChangeHandle;
   RGBPalette prvPaletteArray;
   struct ColourFormat prvColourFormat;
   uint8_t *prvCompress;
   int   prvAFlags;                  // Private allocation flags
   #ifdef __xwindows__
      struct {
         Window window;
         XImage   ximage;
         Drawable drawable;
         XImage   *readable;
         XShmSegmentInfo ShmInfo;
         GC gc;
         int pix_width, pix_height;
         bool XShmImage;
      } x11;

      inline GC getGC() {
         if (x11.gc) return x11.gc;
         else return glXGC;
      }

   #elif _WIN32
      struct {
         APTR Drawable;  // HDC for the Bitmap
      } win;
   #elif _GLES_
      uint32_t prvWriteBackBuffer:1;  // For OpenGL surface locking.
      int prvGLPixel;
      int prvGLFormat;
   #endif

   extBitmap(objMetaClass *ClassPtr, OBJECTID ObjectID);
   ~extBitmap();
};
