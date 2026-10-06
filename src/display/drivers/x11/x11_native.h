#pragma once

#include "x11_driver.h"
#include "../../defs.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/sync.h>
#ifdef XFIXES_ENABLED
#include <X11/extensions/Xfixes.h>
#endif
#ifdef XRANDR_ENABLED
#include <X11/extensions/Xrandr.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
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
   XSyncCounter BasicCounter = 0;  // _NET_WM_SYNC_REQUEST_COUNTER pair; the extended counter drives frame sync
   XSyncCounter FrameCounter = 0;
   int64_t FrameValue = 0;         // Current value of FrameCounter; odd while a frame is being drawn
   int64_t SyncRequest = 0;        // Value from a pending _NET_WM_SYNC_REQUEST message, or zero
   int MinWidth = 0, MinHeight = 0; // Size limits last applied by setSizeHints(); zero means no limit
   int MaxWidth = 0, MaxHeight = 0;
   bool SyncRequestExtended = false;
   bool FrameOpen = false;
   bool Owned = false;
   bool Root = false;
   bool Visible = false;
   bool Adopted = false; // True if the window was created inside a host window supplied by the client
   bool Maximisable = true; // False while an aspect ratio is enforced, because window managers ignore it when maximised
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
   Atom ProtocolsAtom = 0;
   Atom DeleteAtom = 0;
   Atom TakeFocusAtom = 0;
   Atom SyncRequestAtom = 0;
   Atom SyncCounterAtom = 0;
   Atom ClipboardAtom = 0;
   Atom TargetsAtom = 0;
   Atom UTF8StringAtom = 0;
   Atom TextAtom = 0;
   Atom TextPlainAtom = 0;
   Atom TextUTF8Atom = 0;
   Atom URIListAtom = 0;
   Atom TimestampAtom = 0;
   Atom IncrAtom = 0;
   Atom MultipleAtom = 0;
   Atom AtomPairAtom = 0;
   Atom ClipboardPropertyAtom = 0;
   Atom TimestampPropertyAtom = 0;
   Window ClipboardWindow = 0;
   Time ClipboardTimestamp = CurrentTime;
   Time LastServerTimestamp = CurrentTime;
   Time TransferTimestamp = CurrentTime;
   Atom TransferTarget = None;
   std::string ClipboardText;
   uint64_t TransferGeneration = 0;
   std::chrono::steady_clock::time_point TransferStarted = {};
   GC GraphicsContext = 0;
   GC ClipGraphicsContext = 0;
   int ConnectionFD = -1;
   bool Open = false;
   bool Manager = true;
   bool WSLg = false;
   bool SharedImages = false;
   bool Composite = false;
   bool RandR = false;
   bool RandRMonitors = false; // True if the server supports RandR 1.5 monitor enumeration
   int RandREventBase = 0;
   std::atomic<int> DPI = 0;   // Logical DPI declared by Xft.dpi, or zero if the desktop does not define it
   bool FrameSync = false; // True if the XSync extension is available for compositor frame synchronisation
   bool ClipboardTransfer = false;
#ifdef XFIXES_ENABLED
   bool XFixes = false;
   int XFixesEventBase = 0;
#endif
   bool Closing = false;
   bool TrayIcon = false;
   bool TaskBar = false;
   int StickToFront = 0;
};

X11WindowRecord * x11_window(X11Driver::State *State, HOSTWINDOW Window);
X11BitmapRecord * x11_bitmap(extBitmap *Bitmap);
int x11_read_dpi(X11Driver::State *State);
void x11_process_events(X11Driver::State *State);
void x11_begin_frame(X11Driver::State *State, X11WindowRecord *Window);
void x11_end_frame(X11Driver::State *State, X11WindowRecord *Window);
void x11_install_bitmap_routines(extBitmap *Bitmap);
void x11_request_clipboard(X11Driver::State *State, Time Timestamp);

void handle_button_press(XEvent *Event);
void handle_button_release(XEvent *Event);
void handle_configure_notify(XConfigureEvent *Event);
void handle_crossing_notify(XCrossingEvent *Event);
void handle_exposure(XExposeEvent *Event);
void handle_key_press(XEvent *Event);
void handle_key_release(XEvent *Event);
void handle_stack_change(XCirculateEvent *Event);

}
