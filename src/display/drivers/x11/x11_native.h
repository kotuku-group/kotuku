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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
   int AspectWidth = 0, AspectHeight = 0; // Enforced aspect ratio, taken from the window size; zero if not enforced
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

struct X11ClipboardWrite {
   Window Requestor = 0;
   Atom Property = None;
   Atom Type = None;
   std::string Data;
   size_t Offset = 0;
   long PreviousEventMask = 0;
   std::chrono::steady_clock::time_point Updated = {};
};

// The pointer hover state of an inbound XDND session.  Only one session exists at a time because the X server has a
// single core pointer.

struct X11DropSession {
   Window Source = 0;
   Window Target = 0;
   OBJECTID SurfaceID = 0;
   Atom Type = None;          // The preferred type offered by the source, or None if no type is supported
   int Version = 0;
   double X = 0, Y = 0;       // Window-relative position of the latest XdndPosition
   int RootX = 0, RootY = 0;  // Root position of the latest XdndPosition
   uint64_t Generation = 0;   // Identifies the session across callbacks that may end it
   bool Entered = false;      // True once a Crossing notification has been sent for the target
   bool Accepted = false;     // The acceptance sent in the latest XdndStatus
};

// An accepted drop whose data is being read from XdndSelection.  A private requestor window isolates each transfer
// from the clipboard and from late replies to other transfers.

struct X11DropTransfer {
   Window Requestor = 0;
   Window Source = 0;
   Window Target = 0;
   Atom Type = None;
   int Version = 0;
   bool Incremental = false;
   std::string Data;
   std::chrono::steady_clock::time_point Updated = {};
   NativeDrop Drop;
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
   Atom GnomeFilesAtom = 0;
   Atom TimestampAtom = 0;
   Atom IncrAtom = 0;
   Atom MultipleAtom = 0;
   Atom AtomPairAtom = 0;
   Atom ClipboardManagerAtom = 0;
   Atom SaveTargetsAtom = 0;
   Atom SaveTargetsPropertyAtom = 0;
   Atom TimestampPropertyAtom = 0;
   Window ClipboardWindow = 0;
   Window TransferOwner = 0;
   Time ClipboardTimestamp = CurrentTime;
   Time LastServerTimestamp = CurrentTime;
   Time TransferTimestamp = CurrentTime;
   Atom TransferTarget = None;
   Atom TransferProperty = None;
   Atom TransferDataType = None;
   int TransferDataFormat = 0;
   std::string PrimaryText;
   Time PrimaryTimestamp = CurrentTime;
   struct primary_read {
      Window window = 0;
      Window owner = 0;
      Atom property = None;
      Atom target = None;
      Atom type = None;
      OBJECTID recipient = 0;
      int item = 0;
      bool incremental = false;
      std::string text;
      std::chrono::steady_clock::time_point started;
   } PrimaryRead;
   std::string ClipboardText;
   std::string ClipboardUris;
   std::string ClipboardGnomeFiles;
   std::string TransferData;
   std::vector<X11ClipboardWrite> ClipboardWrites;
   X11DropSession DropSession;
   std::vector<X11DropTransfer> DropTransfers;
   std::unordered_map<Window, long> DropWatches; // Original event masks of monitored foreign sources
   TIMER DropTimer = nullptr;
   uint64_t DropGeneration = 0;
   std::chrono::milliseconds DropTimeout = std::chrono::seconds(10);
   struct {
      Atom Aware, Enter, Position, StatusReply, Leave, Drop, Finished, Selection, TypeList, ActionCopy, Property;
   } Xdnd = {};
   uint64_t TransferGeneration = 0;
   std::chrono::steady_clock::time_point TransferStarted = {};
   GC GraphicsContext = 0;
   GC ClipGraphicsContext = 0;
   int ConnectionFD = -1;
   bool Open = false;
   bool Manager = true;
   bool WSLg = false;
   bool XWayland = false;  // True if the server is XWayland, whose root window does not show Wayland clients
   bool SharedImages = false;
   bool Composite = false;
   bool RandR = false;
   bool RandRMonitors = false; // True if the server supports RandR 1.5 monitor enumeration
   int RandREventBase = 0;
   std::atomic<int> DPI = 0;   // Logical DPI declared by Xft.dpi, or zero if the desktop does not define it
   bool FrameSync = false; // True if the XSync extension is available for compositor frame synchronisation
   bool ClipboardTransfer = false;
   bool ClipboardIsFiles = false;
   bool TransferIncremental = false;
   bool TransferDiscard = false;
   bool SaveTargetsPending = false;
   bool SaveTargetsComplete = false;
   bool SaveTargetsSucceeded = false;
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
void x11_request_clipboard(X11Driver::State *State, Time Timestamp, Window Owner);
void x11_save_clipboard(X11Driver::State *State, Time Timestamp);

std::string latin1_to_utf8(const unsigned char *Data, size_t Length);

ERR x11_xdnd_open(X11Driver::State *State);
void x11_xdnd_close(X11Driver::State *State);
void x11_xdnd_advertise(X11Driver::State *State, Window Native);
void x11_xdnd_forget_window(X11Driver::State *State, Window Native);
void x11_xdnd_expire(X11Driver::State *State);
bool x11_xdnd_client_message(X11Driver::State *State, const XClientMessageEvent &Event);
bool x11_xdnd_selection_notify(X11Driver::State *State, const XSelectionEvent &Event);
bool x11_xdnd_property_notify(X11Driver::State *State, const XPropertyEvent &Event);
bool x11_xdnd_foreign_event(X11Driver::State *State, const XEvent &Event);

void handle_button_press(XEvent *Event);
void handle_button_release(XEvent *Event);
void handle_configure_notify(XConfigureEvent *Event);
void handle_crossing_notify(XCrossingEvent *Event);
void handle_exposure(XExposeEvent *Event);
void handle_key_press(XEvent *Event);
void handle_key_release(XEvent *Event);
void handle_stack_change(XCirculateEvent *Event);

}
