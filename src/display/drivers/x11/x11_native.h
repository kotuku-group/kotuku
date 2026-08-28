#pragma once

#include "x11_driver.h"
#include "../../defs.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#ifdef XDGA_ENABLED
#include <X11/extensions/Xxf86dga.h>
#endif
#ifdef XRANDR_ENABLED
#include <X11/extensions/Xrandr.h>
#endif

#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace display {

struct X11WindowRecord {
   Window Native = 0;
   Pixmap Background = 0;
   Colormap CompositeMap = 0;
   GC GraphicsContext = 0;
   extDisplay *Display = nullptr;
   OBJECTID SurfaceID = 0;
   bool Owned = false;
   bool Root = false;
   bool Visible = false;
   bool Adopted = false; // True if the window was created inside a host window supplied by the client
};

struct X11BitmapRecord {
   Window WindowID = 0;
   Drawable DrawableID = 0;
   XImage Image = {};
   XImage *Readable = nullptr;
   XShmSegmentInfo Shm = {};
   GC WindowGraphicsContext = 0;
   GC DefaultGraphicsContext = 0;
   Display *Connection = nullptr;
   int PixmapWidth = 0;
   int PixmapHeight = 0;
   bool SharedImage = false;
   bool OwnsDrawable = false;
};

struct X11Driver::State {
   Display *Connection = nullptr;
   const DriverCallbacks *Callbacks = nullptr;
   XErrorHandler PreviousErrorHandler = nullptr;
   XIOErrorHandler PreviousIOErrorHandler = nullptr;
   XWindowAttributes RootAttributes = {};
   XVisualInfo AlphaVisual = {};
   std::recursive_mutex NativeLock;
   std::unordered_map<Window, X11WindowRecord *> Windows;
   std::unordered_set<extBitmap *> Bitmaps;
   std::array<Cursor, 23> Cursors = {};
   std::array<uint8_t, int(KEY::LIST_END)> KeyHeld = {};
   KQ KeyFlags = KQ::NIL;
   Atom SurfaceAtom = 0;
   Atom DeleteAtom = 0;
   Atom TakeFocusAtom = 0;
   GC GraphicsContext = 0;
   GC ClipGraphicsContext = 0;
   int ConnectionFD = -1;
   bool Open = false;
   bool Manager = true;
   bool WSLg = false;
   bool SharedImages = false;
   bool Composite = false;
   bool RandR = false;
   bool Closing = false;
   bool DGA = false;
   APTR DGAMemory = nullptr;
   int DGAPixelsPerLine = 0;
   int DGABankSize = 0;
};

X11WindowRecord * x11_window(X11Driver::State *State, HOSTWINDOW Window);
X11BitmapRecord * x11_bitmap(extBitmap *Bitmap);
void x11_process_events(X11Driver::State *State);
void x11_install_bitmap_routines(extBitmap *Bitmap);

void handle_button_press(XEvent *Event);
void handle_button_release(XEvent *Event);
void handle_configure_notify(XConfigureEvent *Event);
void handle_crossing_notify(XCrossingEvent *Event);
void handle_exposure(XExposeEvent *Event);
void handle_key_press(XEvent *Event);
void handle_key_release(XEvent *Event);
void handle_stack_change(XCirculateEvent *Event);

}
