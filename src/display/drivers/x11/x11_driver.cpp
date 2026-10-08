#include "x11_native.h"
#include "../../driver/clipboard_utils.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sys/shm.h>
#include <unistd.h>

namespace display {

static X11Driver::State *glX11State = nullptr;

//********************************************************************************************************************

static void x11_colour_component(int SourceMask, uint8_t &Mask, uint8_t &Position, uint8_t &Shift)
{
   Position = 0;
   Shift = 0;
   while (SourceMask and (not (SourceMask & 1))) {
      SourceMask >>= 1;
      Position++;
   }
   Mask = SourceMask;
   for (int bit = 0x80; bit and (not (bit & Mask)); bit >>= 1) Shift++;
}

//********************************************************************************************************************

static void x11_colour_format(ColourFormat &Format, int BitsPerPixel, int RedMask, int GreenMask, int BlueMask,
   int AlphaMask)
{
   x11_colour_component(RedMask, Format.RedMask, Format.RedPos, Format.RedShift);
   x11_colour_component(GreenMask, Format.GreenMask, Format.GreenPos, Format.GreenShift);
   x11_colour_component(BlueMask, Format.BlueMask, Format.BluePos, Format.BlueShift);
   x11_colour_component(AlphaMask, Format.AlphaMask, Format.AlphaPos, Format.AlphaShift);
   Format.BitsPerPixel = BitsPerPixel;
}

//********************************************************************************************************************

static constexpr std::array<std::pair<PTC, unsigned int>, 23> CURSORS = {{
   { PTC::DEFAULT, XC_left_ptr }, { PTC::SIZE_BOTTOM_LEFT, XC_bottom_left_corner },
   { PTC::SIZE_BOTTOM_RIGHT, XC_bottom_right_corner }, { PTC::SIZE_TOP_LEFT, XC_top_left_corner },
   { PTC::SIZE_TOP_RIGHT, XC_top_right_corner }, { PTC::SIZE_LEFT, XC_left_side },
   { PTC::SIZE_RIGHT, XC_right_side }, { PTC::SIZE_TOP, XC_top_side }, { PTC::SIZE_BOTTOM, XC_bottom_side },
   { PTC::CROSSHAIR, XC_crosshair }, { PTC::SLEEP, XC_clock }, { PTC::SIZING, XC_sizing },
   { PTC::SPLIT_VERTICAL, XC_sb_v_double_arrow }, { PTC::SPLIT_HORIZONTAL, XC_sb_h_double_arrow },
   { PTC::MAGNIFIER, XC_hand2 }, { PTC::HAND, XC_hand2 }, { PTC::HAND_LEFT, XC_hand1 },
   { PTC::HAND_RIGHT, XC_hand1 }, { PTC::TEXT, XC_xterm }, { PTC::PAINTBRUSH, XC_pencil },
   { PTC::STOP, XC_left_ptr }, { PTC::INVISIBLE, XC_dot }, { PTC::DRAGGABLE, XC_sizing }
}};

//********************************************************************************************************************

static bool detect_wslg()
{
   if (auto enabled = std::getenv("WSL2_GUI_APPS_ENABLED"); enabled and (enabled[0] IS '1')) return true;
   return access("/mnt/wslg", F_OK) IS 0;
}

//********************************************************************************************************************

static int catch_redirect_error(Display *, XErrorEvent *)
{
   if (glX11State) glX11State->Manager = false;
   return 0;
}

//********************************************************************************************************************

static int catch_x_error(Display *Connection, XErrorEvent *Event)
{
   char message[128] = {};
   XGetErrorText(Connection, Event->error_code, message, sizeof(message));
   kt::Log("X11").warning("Request %d failed: %s", Event->request_code, message);
   return 0;
}

//********************************************************************************************************************

static int catch_xio_error(Display *)
{
   kt::Log("X11").error("The X11 connection was terminated.");
   return 0;
}

//********************************************************************************************************************

static void event_loop(HOSTHANDLE, APTR Data) { x11_process_events((X11Driver::State *)Data); }

//********************************************************************************************************************

static Bool timestamp_event(Display *, XEvent *Event, XPointer Data)
{
   auto state = (X11Driver::State *)Data;
   return (Event->type IS PropertyNotify) and (Event->xproperty.window IS state->ClipboardWindow) and
      (Event->xproperty.atom IS state->TimestampPropertyAtom);
}

//********************************************************************************************************************

static Time server_timestamp(X11Driver::State *State)
{
   XChangeProperty(State->Connection, State->ClipboardWindow, State->TimestampPropertyAtom, XA_INTEGER, 8,
      PropModeAppend, nullptr, 0);
   XEvent event;
   XIfEvent(State->Connection, &event, timestamp_event, (XPointer)State);
   State->LastServerTimestamp = event.xproperty.time;
   return State->LastServerTimestamp;
}

//********************************************************************************************************************

void x11_request_clipboard(X11Driver::State *State, Time Timestamp, Window Owner)
{
   const std::lock_guard lock(State->NativeLock);
   if ((not State->Connection) or (not State->ClipboardWindow)) return;

   State->TransferGeneration++;
   char property_name[40];
   snprintf(property_name, sizeof(property_name), "KOTUKU_CLIP_%u",
      unsigned(State->TransferGeneration % 64));
   State->TransferProperty = XInternAtom(State->Connection, property_name, False);
   XDeleteProperty(State->Connection, State->ClipboardWindow, State->TransferProperty);
   State->TransferOwner = Owner;
   State->TransferTimestamp = Timestamp;
   State->TransferTarget = State->TargetsAtom;
   State->TransferDataType = None;
   State->TransferData.clear();
   State->TransferStarted = std::chrono::steady_clock::now();
   State->ClipboardTransfer = true;
   State->TransferIncremental = false;
   State->TransferDiscard = false;
   XConvertSelection(State->Connection, State->ClipboardAtom, State->TargetsAtom, State->TransferProperty,
      State->ClipboardWindow, Timestamp);
   XFlush(State->Connection);
}

//********************************************************************************************************************

static Cursor blank_cursor(X11Driver::State *State)
{
   XColor black = {};
   auto root = DefaultRootWindow(State->Connection);
   auto image = XCreatePixmap(State->Connection, root, 1, 1, 1);
   auto mask = XCreatePixmap(State->Connection, root, 1, 1, 1);
   auto cursor = XCreatePixmapCursor(State->Connection, image, mask, &black, &black, 0, 0);
   XFreePixmap(State->Connection, image);
   XFreePixmap(State->Connection, mask);
   return cursor;
}

//********************************************************************************************************************

static Cursor cursor_for(X11Driver::State *State, PTC CursorID)
{
   for (size_t i=0; i < CURSORS.size(); i++) if (CURSORS[i].first IS CursorID) return State->Cursors[i];
   return State->Cursors[0];
}

//********************************************************************************************************************

static GC create_graphics_context(X11Driver::State *State, Drawable DrawableID)
{
   XGCValues values = {};
   values.function = GXcopy;
   values.graphics_exposures = 0;
   return XCreateGC(State->Connection, DrawableID, GCGraphicsExposures|GCFunction, &values);
}

//********************************************************************************************************************

// The Motif hints control the window's decorations and the functions that the window manager offers for it.  The
// property is replaced as a whole, so every update carries the window's full state.  Removing the maximise function
// is the only way to stop window managers such as Mutter from maximising a window, which they do in disregard of its
// aspect ratio and maximum size hints.

static void set_motif_hints(X11Driver::State *State, X11WindowRecord *Record)
{
   struct MotifHints {
      unsigned long Flags;
      unsigned long Functions;
      unsigned long Decorations;
      long InputMode;
      unsigned long StatusValue;
   };

   static constexpr unsigned long motif_hints_functions   = 1 << 0;
   static constexpr unsigned long motif_hints_decorations = 1 << 1;
   static constexpr unsigned long motif_func_resize   = 1 << 1;
   static constexpr unsigned long motif_func_move     = 1 << 2;
   static constexpr unsigned long motif_func_minimise = 1 << 3;
   static constexpr unsigned long motif_func_close    = 1 << 5;

   bool decorated = (not Record->Display) or ((Record->Display->Flags & SCR::BORDERLESS) IS SCR::NIL);
   MotifHints hints = {
      .Flags = motif_hints_decorations,
      .Decorations = decorated ? 1UL : 0UL
   };

   if (not Record->Maximisable) {
      hints.Flags |= motif_hints_functions;
      hints.Functions = motif_func_resize|motif_func_move|motif_func_minimise|motif_func_close;
   }

   auto property = XInternAtom(State->Connection, "_MOTIF_WM_HINTS", False);
   XChangeProperty(State->Connection, Record->Native, property, property, 32, PropModeReplace,
      (const unsigned char *)&hints, 5);
}

//********************************************************************************************************************

static void hide_window_from_taskbar(X11Driver::State *State, Window WindowID)
{
   auto property = XInternAtom(State->Connection, "_NET_WM_STATE", False);
   Atom states[] = { XInternAtom(State->Connection, "_NET_WM_STATE_SKIP_TASKBAR", False) };
   XChangeProperty(State->Connection, WindowID, property, XA_ATOM, 32, PropModeReplace,
      (const unsigned char *)states, std::ssize(states));
}

//********************************************************************************************************************

X11WindowRecord * x11_window(X11Driver::State *State, HOSTWINDOW WindowHandle)
{
   if ((not State) or (not WindowHandle)) return nullptr;
   const std::lock_guard lock(State->NativeLock);
   for (auto &[native, record] : State->Windows) if (record IS WindowHandle) return record;
   if (auto it = State->Windows.find(Window(uintptr_t(WindowHandle))); it != State->Windows.end()) return it->second;
   return nullptr;
}

//********************************************************************************************************************
// Compositor frame synchronisation, per the extended _NET_WM_SYNC_REQUEST_COUNTER protocol.  An odd counter value
// tells the compositor that a frame is being drawn, so it continues to show the last complete frame.  Setting an
// even value completes the frame.  Frames are opened when the window is resized and closed once the new content has
// been presented, which prevents the compositor from showing the resized window with stale content.

static void set_sync_counter(X11Driver::State *State, XSyncCounter Counter, int64_t Value)
{
   XSyncValue value;
   XSyncIntsToValue(&value, uint32_t(Value & 0xffffffff), int(Value >> 32));
   XSyncSetCounter(State->Connection, Counter, value);
}

//********************************************************************************************************************

void x11_begin_frame(X11Driver::State *State, X11WindowRecord *Window)
{
   const std::lock_guard lock(State->NativeLock);
   if ((not Window) or (not Window->FrameCounter) or Window->FrameOpen or (not Window->Visible)) return;

   if (Window->SyncRequest and Window->SyncRequestExtended and (Window->FrameValue <= Window->SyncRequest)) {
      Window->FrameValue = Window->SyncRequest;
      if (Window->FrameValue & 1) Window->FrameValue++;
   }

   Window->FrameValue++;
   Window->FrameOpen = true;
   set_sync_counter(State, Window->FrameCounter, Window->FrameValue);
   XFlush(State->Connection);
}

//********************************************************************************************************************
// Completes an open frame, or acknowledges a _NET_WM_SYNC_REQUEST from the window manager by advancing the counter
// to the requested value.

void x11_end_frame(X11Driver::State *State, X11WindowRecord *Window)
{
   const std::lock_guard lock(State->NativeLock);
   if ((not Window) or (not Window->FrameCounter)) return;
   if ((not Window->FrameOpen) and (not Window->SyncRequest)) return;

   bool update_frame_counter = Window->FrameOpen;
   if (Window->SyncRequest) {
      if (Window->SyncRequestExtended) {
         if (Window->FrameValue <= Window->SyncRequest) Window->FrameValue = Window->SyncRequest + 1;
         update_frame_counter = true;
      }
      else set_sync_counter(State, Window->BasicCounter, Window->SyncRequest);
   }

   if (update_frame_counter) {
      if (Window->FrameValue & 1) Window->FrameValue++;
      set_sync_counter(State, Window->FrameCounter, Window->FrameValue);
   }

   Window->SyncRequest = 0;
   Window->SyncRequestExtended = false;
   Window->FrameOpen = false;
   XFlush(State->Connection);
}

//********************************************************************************************************************

X11BitmapRecord * x11_bitmap(extBitmap *Bitmap)
{
   return Bitmap ? (X11BitmapRecord *)Bitmap->DriverData : nullptr;
}

//********************************************************************************************************************

X11Driver::X11Driver() : Data(new State) { }
X11Driver::~X11Driver() { delete Data; }
CSTRING X11Driver::name() const { return "x11"; }
DT X11Driver::displayType() const { return DT::X11; }

#ifdef X11_DRIVER_TESTS
extern "C" DISPLAY_DRIVER_EXPORT void x11_process_driver_events(DisplayDriver *Driver)
{
   if (Driver) ((X11Driver *)Driver)->processEvents();
}

void X11Driver::processEvents() { x11_process_events(Data); }
#endif

//********************************************************************************************************************

DCAP X11Driver::capabilities() const
{
   if (not Data->Open) return DCAP::NIL;
   auto result = DCAP::WINDOW_POSITION|DCAP::STACKING|DCAP::POINTER_WARP|DCAP::VIDEO_BITMAPS|DCAP::WINDOW_DECOR|
      DCAP::CLIPBOARD;
   if (Data->Composite) result |= DCAP::COMPOSITING;
   if (Data->RandR and Data->Manager and (not Data->WSLg)) result |= DCAP::MODE_SWITCH;
   if (Data->Manager) result |= DCAP::DESKTOP_MANAGER;
   return result;
}

//********************************************************************************************************************

ERR X11Driver::isAvailable() const
{
   if (auto value = std::getenv("KOTUKU_XDISPLAY"); value and value[0]) return ERR::Okay;
   if (auto value = std::getenv("DISPLAY"); value and value[0]) return ERR::Okay;
   return ERR::NoSupport;
}

//********************************************************************************************************************

ERR X11Driver::open(const DriverCallbacks &Callbacks)
{
   static const bool xlib_thread_safe = XInitThreads() != 0;
   XGCValues values = {};
   XSetWindowAttributes clipboard_attributes = {};
   Window root = 0;
   int major = 0, minor = 0, pixmaps = 0;

#ifdef XRANDR_ENABLED
   int event_base = 0, error_base = 0;
#endif

   if (Data->Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;
   if (not xlib_thread_safe) return ERR::SystemCall;
   if (auto error = isAvailable(); error != ERR::Okay) return error;

   auto display_name = std::getenv("KOTUKU_XDISPLAY");
   if ((not display_name) or (not display_name[0])) display_name = std::getenv("DISPLAY");

   Data->Callbacks = &Callbacks;
   Data->Closing = false;
   Data->Manager = true;
   Data->WSLg = detect_wslg();
   Data->Connection = XOpenDisplay(display_name);
   if (not Data->Connection) goto fail;

   glX11State = Data;
   Data->PreviousErrorHandler = XSetErrorHandler(Data->WSLg ? catch_x_error : catch_redirect_error);
   Data->PreviousIOErrorHandler = XSetIOErrorHandler(catch_xio_error);

   if (Data->WSLg) Data->Manager = false;
   else {
      XSelectInput(Data->Connection, DefaultRootWindow(Data->Connection),
         LeaveWindowMask|EnterWindowMask|PointerMotionMask|PropertyChangeMask|SubstructureRedirectMask|
         KeyPressMask|ButtonPressMask|ButtonReleaseMask);
      XSync(Data->Connection, 0);
   }

   // Without the window manager role, the root window is monitored passively so that changes to the desktop's
   // resource database (which declares Xft.dpi) are reported.  The selection above cannot be relied upon for this
   // because it fails as a whole if another client holds SubstructureRedirect.

   if (not Data->Manager) XSelectInput(Data->Connection, DefaultRootWindow(Data->Connection), PropertyChangeMask);

   XSetErrorHandler(catch_x_error);
   Data->ConnectionFD = XConnectionNumber(Data->Connection);
   fcntl(Data->ConnectionFD, F_SETFD, FD_CLOEXEC);

   if (RegisterFD(Data->ConnectionFD, RFD::READ|RFD::ALWAYS_CALL, event_loop, Data) != ERR::Okay) goto fail;

   values.function = GXcopy;
   values.graphics_exposures = 0;
   root = DefaultRootWindow(Data->Connection);
   Data->GraphicsContext = XCreateGC(Data->Connection, root, GCGraphicsExposures|GCFunction, &values);
   Data->ClipGraphicsContext = XCreateGC(Data->Connection, root, GCGraphicsExposures|GCFunction, &values);
   if ((not Data->GraphicsContext) or (not Data->ClipGraphicsContext)) goto fail;

   Data->SharedImages = XShmQueryVersion(Data->Connection, &major, &minor, &pixmaps) != 0;
   Data->ProtocolsAtom = XInternAtom(Data->Connection, "WM_PROTOCOLS", 0);
   Data->DeleteAtom = XInternAtom(Data->Connection, "WM_DELETE_WINDOW", 0);
   Data->TakeFocusAtom = XInternAtom(Data->Connection, "WM_TAKE_FOCUS", 0);
   Data->SurfaceAtom = XInternAtom(Data->Connection, "KOTUKU_SCREENID", 0);
   Data->ClipboardAtom = XInternAtom(Data->Connection, "CLIPBOARD", False);
   Data->TargetsAtom = XInternAtom(Data->Connection, "TARGETS", False);
   Data->UTF8StringAtom = XInternAtom(Data->Connection, "UTF8_STRING", False);
   Data->TextAtom = XInternAtom(Data->Connection, "TEXT", False);
   Data->TextPlainAtom = XInternAtom(Data->Connection, "text/plain", False);
   Data->TextUTF8Atom = XInternAtom(Data->Connection, "text/plain;charset=utf-8", False);
   Data->URIListAtom = XInternAtom(Data->Connection, "text/uri-list", False);
   Data->GnomeFilesAtom = XInternAtom(Data->Connection, "x-special/gnome-copied-files", False);
   Data->TimestampAtom = XInternAtom(Data->Connection, "TIMESTAMP", False);
   Data->IncrAtom = XInternAtom(Data->Connection, "INCR", False);
   Data->MultipleAtom = XInternAtom(Data->Connection, "MULTIPLE", False);
   Data->AtomPairAtom = XInternAtom(Data->Connection, "ATOM_PAIR", False);
   Data->ClipboardManagerAtom = XInternAtom(Data->Connection, "CLIPBOARD_MANAGER", False);
   Data->SaveTargetsAtom = XInternAtom(Data->Connection, "SAVE_TARGETS", False);
   Data->SaveTargetsPropertyAtom = XInternAtom(Data->Connection, "KOTUKU_SAVE_TARGETS", False);
   Data->TimestampPropertyAtom = XInternAtom(Data->Connection, "KOTUKU_TIME", False);

   clipboard_attributes.event_mask = PropertyChangeMask;
   Data->ClipboardWindow = XCreateWindow(Data->Connection, root, -10, -10, 1, 1, 0, 0, InputOnly,
      CopyFromParent, CWEventMask, &clipboard_attributes);
   if (not Data->ClipboardWindow) goto fail;

#ifdef XFIXES_ENABLED
   {
      int event_base = 0, error_base = 0;
      Data->XFixes = XFixesQueryExtension(Data->Connection, &event_base, &error_base) != 0;
      if (Data->XFixes) {
         Data->XFixesEventBase = event_base;
         XFixesSelectSelectionInput(Data->Connection, Data->ClipboardWindow, Data->ClipboardAtom,
            XFixesSetSelectionOwnerNotifyMask|XFixesSelectionWindowDestroyNotifyMask|
            XFixesSelectionClientCloseNotifyMask);
         auto owner = XGetSelectionOwner(Data->Connection, Data->ClipboardAtom);
         if ((owner != None) and (owner != Data->ClipboardWindow)) {
            x11_request_clipboard(Data, server_timestamp(Data), owner);
         }
      }
   }
#endif

   {
      int sync_event = 0, sync_error = 0, sync_major = 0, sync_minor = 0;
      Data->FrameSync = XSyncQueryExtension(Data->Connection, &sync_event, &sync_error) and
         XSyncInitialize(Data->Connection, &sync_major, &sync_minor);
   }

   if (Data->FrameSync) {
      Data->SyncRequestAtom = XInternAtom(Data->Connection, "_NET_WM_SYNC_REQUEST", 0);
      Data->SyncCounterAtom = XInternAtom(Data->Connection, "_NET_WM_SYNC_REQUEST_COUNTER", 0);
   }

   XGetWindowAttributes(Data->Connection, root, &Data->RootAttributes);
   Data->Composite = XMatchVisualInfo(Data->Connection, DefaultScreen(Data->Connection), 32, TrueColor,
      &Data->AlphaVisual) != 0;

#ifdef XRANDR_ENABLED
   Data->RandR = XRRQueryExtension(Data->Connection, &event_base, &error_base) != 0;
   if (Data->RandR) {
      Data->RandREventBase = event_base;
      XRRSelectInput(Data->Connection, root, RRScreenChangeNotifyMask);
      int major = 0, minor = 0;
      if (XRRQueryVersion(Data->Connection, &major, &minor)) {
         Data->RandRMonitors = (major > 1) or ((major IS 1) and (minor >= 5));
      }
   }
#endif

   Data->DPI = x11_read_dpi(Data);

   for (size_t i=0; i < CURSORS.size(); i++) {
      Data->Cursors[i] = CURSORS[i].first IS PTC::INVISIBLE ? blank_cursor(Data) :
         XCreateFontCursor(Data->Connection, CURSORS[i].second);
   }

   if (not std::getenv("KOTUKU_XDISPLAY")) setenv("KOTUKU_XDISPLAY", display_name, 0);
   if (Data->Manager) setenv("DISPLAY", ":10", 1);
   seteuid(getuid());
   Data->Open = true;
   return ERR::Okay;

fail:
   close();
   return ERR::SystemCall;
}

//********************************************************************************************************************

ERR X11Driver::close()
{
   if (Data->Open and Data->Connection and Data->ClipboardWindow)
      x11_save_clipboard(Data, server_timestamp(Data));
   const std::lock_guard lock(Data->NativeLock);
   const bool was_open = Data->Open;
   Data->Closing = true;
   if (Data->ConnectionFD != -1) { DeregisterFD(Data->ConnectionFD); Data->ConnectionFD = -1; }
   Data->Callbacks = nullptr;
   if (Data->Connection) {
      while (not Data->Bitmaps.empty()) freeBitmap(*Data->Bitmaps.begin());
      for (auto &[native, window] : Data->Windows) {
         if (window->Background) XFreePixmap(Data->Connection, window->Background);
         if (window->GraphicsContext) XFreeGC(Data->Connection, window->GraphicsContext);
         if (window->CompositeMap) XFreeColormap(Data->Connection, window->CompositeMap);
         if (window->FrameCounter) XSyncDestroyCounter(Data->Connection, window->FrameCounter);
         if (window->BasicCounter) XSyncDestroyCounter(Data->Connection, window->BasicCounter);
         if (window->Owned and (not window->Root)) XDestroyWindow(Data->Connection, native);
         delete window;
      }
      Data->Windows.clear();
      for (auto cursor : Data->Cursors) if (cursor) XFreeCursor(Data->Connection, cursor);
      Data->Cursors.fill(0);
      if (Data->ClipboardWindow) XDestroyWindow(Data->Connection, Data->ClipboardWindow);
      if (Data->GraphicsContext) XFreeGC(Data->Connection, Data->GraphicsContext);
      if (Data->ClipGraphicsContext) XFreeGC(Data->Connection, Data->ClipGraphicsContext);
      XSetErrorHandler(Data->PreviousErrorHandler);
      XSetIOErrorHandler(Data->PreviousIOErrorHandler);
      if (not was_open) XCloseDisplay(Data->Connection);
   }
   glX11State = nullptr;
   Data->Connection = nullptr;
   Data->ClipboardWindow = 0;
   Data->ClipboardText.clear();
   Data->ClipboardUris.clear();
   Data->ClipboardGnomeFiles.clear();
   Data->TransferData.clear();
   Data->ClipboardWrites.clear();
   Data->ClipboardTimestamp = Data->LastServerTimestamp = Data->TransferTimestamp = CurrentTime;
   Data->TransferTarget = Data->TransferProperty = Data->TransferDataType = None;
   Data->TransferOwner = 0;
   Data->ClipboardTransfer = false;
   Data->ClipboardIsFiles = Data->TransferIncremental = Data->TransferDiscard = false;
   Data->SaveTargetsPending = Data->SaveTargetsComplete = Data->SaveTargetsSucceeded = false;
#ifdef XFIXES_ENABLED
   Data->XFixes = false;
   Data->XFixesEventBase = 0;
#endif
   Data->GraphicsContext = Data->ClipGraphicsContext = 0;
   Data->Open = Data->Closing = Data->WSLg = Data->SharedImages = Data->Composite = Data->RandR = false;
   Data->Manager = true;
   return was_open ? ERR::DoNotExpunge : ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::clipboardAddText(CSTRING Text)
{
   const std::lock_guard lock(Data->NativeLock);
   if ((not Data->Open) or (not Data->Connection) or (not Data->ClipboardWindow)) return ERR::NoSupport;

   auto timestamp = server_timestamp(Data);
   Data->ClipboardText = Text ? Text : "";
   Data->ClipboardUris.clear();
   Data->ClipboardGnomeFiles.clear();
   Data->ClipboardIsFiles = false;
   Data->ClipboardTimestamp = timestamp;
   Data->ClipboardTransfer = false;
   Data->TransferGeneration++;
   XSetSelectionOwner(Data->Connection, Data->ClipboardAtom, Data->ClipboardWindow, timestamp);
   XFlush(Data->Connection);
   if (XGetSelectionOwner(Data->Connection, Data->ClipboardAtom) != Data->ClipboardWindow) {
      Data->ClipboardText.clear();
      return ERR::SystemCall;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::clipboardAddFiles(CLIPTYPE Type, const std::vector<std::string> &Paths, bool Cut)
{
   if (Type != CLIPTYPE::FILE) return ERR::NoSupport;
   ClipboardFilePayload payload;
   if (auto error = encode_clipboard_files(Paths, Cut, payload); error != ERR::Okay) return error;

   const std::lock_guard lock(Data->NativeLock);
   if ((not Data->Open) or (not Data->Connection) or (not Data->ClipboardWindow)) return ERR::NoSupport;

   auto timestamp = server_timestamp(Data);
   Data->ClipboardText.clear();
   Data->ClipboardUris = std::move(payload.URIList);
   Data->ClipboardGnomeFiles = std::move(payload.GnomeFiles);
   Data->ClipboardIsFiles = true;
   Data->ClipboardTimestamp = timestamp;
   Data->ClipboardTransfer = false;
   Data->TransferGeneration++;
   XSetSelectionOwner(Data->Connection, Data->ClipboardAtom, Data->ClipboardWindow, timestamp);
   XFlush(Data->Connection);
   if (XGetSelectionOwner(Data->Connection, Data->ClipboardAtom) != Data->ClipboardWindow) {
      Data->ClipboardUris.clear();
      Data->ClipboardGnomeFiles.clear();
      Data->ClipboardIsFiles = false;
      return ERR::SystemCall;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::clipboardClear()
{
   const std::lock_guard lock(Data->NativeLock);
   if ((not Data->Open) or (not Data->Connection) or (not Data->ClipboardWindow)) return ERR::NoSupport;

   if (XGetSelectionOwner(Data->Connection, Data->ClipboardAtom) IS Data->ClipboardWindow) {
      XSetSelectionOwner(Data->Connection, Data->ClipboardAtom, None, server_timestamp(Data));
      XFlush(Data->Connection);
   }
   Data->ClipboardText.clear();
   Data->ClipboardUris.clear();
   Data->ClipboardGnomeFiles.clear();
   Data->ClipboardIsFiles = false;
   Data->ClipboardTimestamp = CurrentTime;
   Data->ClipboardTransfer = false;
   Data->TransferGeneration++;
   return ERR::Okay;
}

//********************************************************************************************************************
// WM_CLASS links the window to a .desktop entry through its StartupWMClass key.  Desktop environments such as GNOME
// rely on this link for the application name, icon and window grouping.  The instance name is the lower-case form
// of the class, following convention.  A null or empty Class applies the default identity.

static void set_window_class(Display *Connection, Window Native, CSTRING Class)
{
   std::string res_class = (Class and Class[0]) ? Class : DEFAULT_WM_CLASS;
   std::string res_name = res_class;
   std::transform(res_name.begin(), res_name.end(), res_name.begin(),
      [](char Ch) { return ((Ch >= 'A') and (Ch <= 'Z')) ? char(Ch - 'A' + 'a') : Ch; });

   XClassHint class_hint = { res_name.data(), res_class.data() };
   XSetClassHint(Connection, Native, &class_hint);
}

//********************************************************************************************************************

static ERR create_window_record(X11Driver::State *State, extDisplay *DisplayObject, Window Parent,
   X11WindowRecord *&Record)
{
   XSetWindowAttributes attributes = {};
   attributes.bit_gravity = NorthWestGravity; // Retain content in place on resize; new areas are exposed
   attributes.win_gravity = CenterGravity;
   attributes.cursor = State->Cursors[0];

   // Presence controls whether the window manager participates.  Borderless taskbar windows remain managed, while
   // presence-less and tray windows bypass the manager.  Compositing only selects an alpha-capable visual.

   attributes.override_redirect = ((DisplayObject->Flags & SCR::BORDERLESS) != SCR::NIL) and (not State->TaskBar);
   attributes.event_mask = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask|
      KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask;
   int flags = CWEventMask|CWOverrideRedirect|CWCursor|CWBitGravity;
   int depth = CopyFromParent;
   Visual *visual = CopyFromParent;
   Colormap colormap = 0;

   if (((DisplayObject->Flags & SCR::COMPOSITE) != SCR::NIL) and State->Composite) {
      colormap = XCreateColormap(State->Connection, DefaultRootWindow(State->Connection), State->AlphaVisual.visual,
         AllocNone);
      attributes.colormap = colormap;
      attributes.background_pixel = attributes.border_pixel = 0;
      flags |= CWColormap|CWBackPixel|CWBorderPixel;
      visual = State->AlphaVisual.visual;
      depth = State->AlphaVisual.depth;
      DisplayObject->Bitmap->Flags |= BMF::ALPHA_CHANNEL|BMF::FIXED_DEPTH;
      DisplayObject->Bitmap->BitsPerPixel = 32;
      DisplayObject->Bitmap->BytesPerPixel = 4;
   }

   const bool root_parent = Parent IS DefaultRootWindow(State->Connection);
   auto native = XCreateWindow(State->Connection, Parent, root_parent ? DisplayObject->X : 0,
      root_parent ? DisplayObject->Y : 0, DisplayObject->Width, DisplayObject->Height, 0, depth, InputOutput, visual,
      flags, &attributes);

   if (not native) { if (colormap) XFreeColormap(State->Connection, colormap); return ERR::SystemCall; }

   auto graphics_context = create_graphics_context(State, native);
   if (not graphics_context) {
      XDestroyWindow(State->Connection, native);
      if (colormap) XFreeColormap(State->Connection, colormap);
      return ERR::SystemCall;
   }

   auto record = new(std::nothrow) X11WindowRecord;
   if (not record) {
      XFreeGC(State->Connection, graphics_context);
      XDestroyWindow(State->Connection, native);
      if (colormap) XFreeColormap(State->Connection, colormap);
      return ERR::AllocMemory;
   }

   record->Native = native;
   record->CompositeMap = colormap;
   record->GraphicsContext = graphics_context;
   record->Display = DisplayObject;
   record->Owned = true;

   {
      const std::lock_guard lock(State->NativeLock);
      State->Windows[native] = record;
   }

   std::string_view title;
   CurrentTask()->getName(title);
   XStoreName(State->Connection, native, title.empty() ? "Kotuku" : title.data());

   set_window_class(State->Connection, native, nullptr);

   Atom protocols[] ={ State->DeleteAtom, State->TakeFocusAtom, State->SyncRequestAtom };
   XSetWMProtocols(State->Connection, native, protocols, State->FrameSync ? 3 : 2);

   // Advertising a basic and an extended counter enables frame synchronisation with a compositing window manager.
   // While the extended counter is odd, the compositor holds the last complete frame on screen, including the
   // window's previous size.  See x11_begin_frame().

   if (State->FrameSync) {
      XSyncValue zero;
      XSyncIntToValue(&zero, 0);
      record->BasicCounter = XSyncCreateCounter(State->Connection, zero);
      record->FrameCounter = XSyncCreateCounter(State->Connection, zero);
      long counters[] = { long(record->BasicCounter), long(record->FrameCounter) };
      XChangeProperty(State->Connection, native, State->SyncCounterAtom, XA_CARDINAL, 32, PropModeReplace,
         (unsigned char *)counters, std::ssize(counters));
   }

   XSizeHints hints = { .flags = USPosition|USSize };
   XSetWMNormalHints(State->Connection, native, &hints);
   set_motif_hints(State, record);
   if ((not attributes.override_redirect) and (not State->TaskBar)) hide_window_from_taskbar(State, native);
   Record = record;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::createWindow(extDisplay *DisplayObject, HOSTWINDOW &Handle)
{
   if ((not Data->Open) or (not DisplayObject)) return ERR::NotInitialised;

   if (Data->Manager or ((DisplayObject->Flags & SCR::MAXIMISE) != SCR::NIL)) {
      DisplayObject->Width = Data->RootAttributes.width;
      DisplayObject->Height = Data->RootAttributes.height;
   }

   X11WindowRecord *record = nullptr;
   if (Data->Manager) {
      record = new(std::nothrow) X11WindowRecord;
      if (not record) return ERR::AllocMemory;

      record->Native = DefaultRootWindow(Data->Connection);
      record->Display = DisplayObject;
      record->Root = true;
      record->GraphicsContext = create_graphics_context(Data, record->Native);

      if (not record->GraphicsContext) { delete record; return ERR::SystemCall; }

      {
         const std::lock_guard lock(Data->NativeLock);
         Data->Windows[record->Native] = record;
      }

      // PropertyChangeMask is retained so that changes to the desktop's resource database (Xft.dpi) are reported.

      XSetWindowAttributes attributes = {
         .event_mask = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask|
            KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask|PropertyChangeMask
      };

      XChangeWindowAttributes(Data->Connection, record->Native, CWEventMask, &attributes);
   }
   else if (auto error = create_window_record(Data, DisplayObject, DefaultRootWindow(Data->Connection), record);
         error != ERR::Okay) return error;

   Handle = record;
   DisplayObject->Flags |= SCR::HOSTED;
   if (DisplayObject->PopOverID) {
      if (ScopedObjectLock<extDisplay> other(DisplayObject->PopOverID, 3000); other.granted()) {
         if (auto popover = x11_window(Data, other->WindowHandle)) {
            XSetTransientForHint(Data->Connection, record->Native, popover->Native);
         }
      }
      else {
         destroyWindow(record);
         Handle = nullptr;
         return ERR::AccessObject;
      }
   }
   else if (Data->StickToFront) XSetTransientForHint(Data->Connection, record->Native, DefaultRootWindow(Data->Connection));

   if (auto bitmap = x11_bitmap((extBitmap *)DisplayObject->Bitmap)) {
      bitmap->WindowID = record->Native;
      bitmap->WindowGraphicsContext = record->GraphicsContext;

      if ((DisplayObject->Bitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) bitmap->DrawableID = record->Native;
      else {
         bitmap->PixmapWidth = std::max(DisplayObject->Width, Data->RootAttributes.width);
         bitmap->PixmapHeight = std::max(DisplayObject->Height, Data->RootAttributes.height);
         auto depth = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
         record->Background = XCreatePixmap(Data->Connection, record->Native, bitmap->PixmapWidth, bitmap->PixmapHeight, depth);

         if (not record->Background) {
            destroyWindow(record);
            Handle = nullptr;
            return ERR::SystemCall;
         }

         bitmap->DrawableID = record->Background;
         XSetWindowBackgroundPixmap(Data->Connection, record->Native, record->Background);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::adoptWindow(extDisplay *DisplayObject, APTR NativeHandle, HOSTWINDOW &Handle)
{
   if (not NativeHandle) return ERR::NullArgs;
   X11WindowRecord *record = nullptr;
   if (auto error = create_window_record(Data, DisplayObject, Window(uintptr_t(NativeHandle)), record); error != ERR::Okay) return error;

   record->Adopted = true;
   Handle = record;
   DisplayObject->Flags |= SCR::HOSTED;
   if (auto bitmap = x11_bitmap((extBitmap *)DisplayObject->Bitmap)) {
      bitmap->WindowID = bitmap->DrawableID = record->Native;
      bitmap->WindowGraphicsContext = record->GraphicsContext;
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::nativeWindowHandle(HOSTWINDOW WindowHandle, APTR &NativeHandle)
{
   auto window = x11_window(Data, WindowHandle);
   if (not window) return ERR::NoSupport;
   NativeHandle = (APTR)(uintptr_t)window->Native;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::destroyWindow(HOSTWINDOW WindowHandle)
{
   const std::lock_guard lock(Data->NativeLock);
   auto window = x11_window(Data, WindowHandle);
   if (not window) return WindowHandle ? ERR::NoSupport : ERR::Okay;
   Data->Windows.erase(window->Native);

   if (window->Display and window->Display->Bitmap) {
      if (auto bitmap = x11_bitmap((extBitmap *)window->Display->Bitmap);
            bitmap and (bitmap->WindowID IS window->Native)) {
         bitmap->WindowID = bitmap->DrawableID = 0;
         bitmap->WindowGraphicsContext = 0;
      }
   }

   if (window->Background) XFreePixmap(Data->Connection, window->Background);
   if (window->GraphicsContext) XFreeGC(Data->Connection, window->GraphicsContext);
   if (window->CompositeMap) XFreeColormap(Data->Connection, window->CompositeMap);
   if (window->FrameCounter) XSyncDestroyCounter(Data->Connection, window->FrameCounter);
   if (window->BasicCounter) XSyncDestroyCounter(Data->Connection, window->BasicCounter);
   if (window->Owned and (not window->Root)) XDestroyWindow(Data->Connection, window->Native);
   delete window;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::showWindow(HOSTWINDOW WindowHandle, bool)
{
   auto window = x11_window(Data, WindowHandle);
   if (not window) return ERR::NoSupport;
   XMapWindow(Data->Connection, window->Native);
   if (window->Display and ((window->Display->Flags & SCR::BORDERLESS) IS SCR::NIL)) {
      XMoveWindow(Data->Connection, window->Native, window->Display->X, window->Display->Y);
   }
   window->Visible = true;
   XSync(Data->Connection, 0);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::hideWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   x11_end_frame(Data, window);
   XUnmapWindow(Data->Connection, window->Native); window->Visible = false; return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::focusWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XSetInputFocus(Data->Connection, window->Native, RevertToNone, CurrentTime); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::moveWindow(HOSTWINDOW WindowHandle, int X, int Y)
{
   auto window = x11_window(Data, WindowHandle); if ((not window) or Data->Manager) return ERR::NoSupport;
   XMoveWindow(Data->Connection, window->Native, X, Y); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::resizeWindow(HOSTWINDOW WindowHandle, int X, int Y, int Width, int Height)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   if (Data->Manager) { int bpp = 0; return setDisplayMode(Width, Height, bpp, 0); }
   // ConfigureNotify feedback has already applied this geometry; echoing it would fight an interactive WM resize.
   if (window->Display and (window->Display->Width IS Width) and (window->Display->Height IS Height) and
         (((X IS 0x7fffffff) and (Y IS 0x7fffffff)) or
            ((window->Display->X IS X) and (window->Display->Y IS Y)))) return ERR::Okay;
   // The frame for the new size is drawn after this call returns and is closed by present(), so the compositor
   // shows the new size and its content together.

   x11_begin_frame(Data, window);
   if ((X != 0x7fffffff) and (Y != 0x7fffffff)) XMoveWindow(Data->Connection, window->Native, X, Y);
   XResizeWindow(Data->Connection, window->Native, Width, Height); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::raiseWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XRaiseWindow(Data->Connection, window->Native); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::lowerWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XLowerWindow(Data->Connection, window->Native); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::minimiseWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XIconifyWindow(Data->Connection, window->Native, DefaultScreen(Data->Connection)); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setWindowTitle(HOSTWINDOW WindowHandle, CSTRING Title)
{
   auto window = x11_window(Data, WindowHandle); if ((not window) or (not Title)) return ERR::NullArgs;
   XStoreName(Data->Connection, window->Native, Title); return ERR::Okay;
}

//********************************************************************************************************************
// _NET_WM_ICON is an array of CARDINALs containing each image's width and height followed by its ARGB pixels.  Xlib
// expects 32-bit property data to be supplied as an array of longs, regardless of the size of a long.  Images that
// would exceed the server's maximum request size are skipped, which is only likely if BIG-REQUESTS is unavailable.

ERR X11Driver::setWindowIcon(HOSTWINDOW WindowHandle, const std::vector<DisplayIcon> &Icons)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NullArgs;

   auto property = XInternAtom(Data->Connection, "_NET_WM_ICON", False);

   if (Icons.empty()) {
      XDeleteProperty(Data->Connection, window->Native, property);
      return ERR::Okay;
   }

   long max_request = XExtendedMaxRequestSize(Data->Connection);
   if (not max_request) max_request = XMaxRequestSize(Data->Connection);
   const size_t max_cardinals = size_t(max_request) - 64; // Allow for the request header

   size_t total = 0;
   for (auto &icon : Icons) total += 2 + icon.Pixels.size();

   std::vector<long> data;
   data.reserve(std::min(total, max_cardinals));
   for (auto &icon : Icons) {
      if (data.size() + 2 + icon.Pixels.size() > max_cardinals) continue;
      data.push_back(icon.Width);
      data.push_back(icon.Height);
      for (auto pixel : icon.Pixels) data.push_back(long(pixel));
   }

   XChangeProperty(Data->Connection, window->Native, property, XA_CARDINAL, 32, PropModeReplace,
      (unsigned char *)data.data(), int(data.size()));
   XFlush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setWindowClass(HOSTWINDOW WindowHandle, CSTRING Class)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NullArgs;
   set_window_class(Data->Connection, window->Native, Class);
   XFlush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::windowTitle(HOSTWINDOW WindowHandle, std::string &Title)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   char *title = nullptr; if (not XFetchName(Data->Connection, window->Native, &title)) return ERR::SystemCall;
   Title = title ? title : ""; if (title) XFree(title); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setSizeHints(HOSTWINDOW WindowHandle, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;

   // WM_NORMAL_HINTS is replaced as a whole, so a negative value retains the limit from the previous call.

   if (MinW >= 0) window->MinWidth  = MinW;
   if (MinH >= 0) window->MinHeight = MinH;
   if (MaxW >= 0) window->MaxWidth  = MaxW;
   if (MaxH >= 0) window->MaxHeight = MaxH;

   XSizeHints hints = {};
   if ((window->MaxWidth > 0) and (window->MaxHeight > 0)) {
      hints.max_width = window->MaxWidth; hints.max_height = window->MaxHeight; hints.flags |= PMaxSize;
   }
   if ((window->MinWidth > 0) and (window->MinHeight > 0)) {
      hints.min_width = window->MinWidth; hints.min_height = window->MinHeight; hints.flags |= PMinSize;
   }
   if (EnforceAspect and (hints.flags & PMinSize)) {
      hints.flags |= PAspect;
      hints.min_aspect = { window->MinWidth, window->MinHeight };
      hints.max_aspect = { window->MinWidth, window->MinHeight };
   }
   XSetWMNormalHints(Data->Connection, window->Native, &hints);

   if (window->Owned and (window->Maximisable IS EnforceAspect)) {
      window->Maximisable = not EnforceAspect;
      set_motif_hints(Data, window);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::windowCoords(HOSTWINDOW WindowHandle, int &X, int &Y, int &Width, int &Height)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XWindowAttributes attributes;
   if (not XGetWindowAttributes(Data->Connection, window->Native, &attributes)) return ERR::SystemCall;
   Window child; XTranslateCoordinates(Data->Connection, window->Native, DefaultRootWindow(Data->Connection), 0, 0,
      &X, &Y, &child); Width = attributes.width; Height = attributes.height; return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::frameMargins(HOSTWINDOW WindowHandle, int &Left, int &Top, int &Right, int &Bottom)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   Left = Top = Right = Bottom = 0;
   auto atom = XInternAtom(Data->Connection, "_NET_FRAME_EXTENTS", 1); if (atom IS None) return ERR::Okay;
   Atom type; int format; unsigned long count, remaining; unsigned char *value = nullptr;
   if ((XGetWindowProperty(Data->Connection, window->Native, atom, 0, 4, 0, AnyPropertyType, &type, &format,
         &count, &remaining, &value) IS Success) and value and (count >= 4)) {
      auto margins = (unsigned long *)value;
      Left = margins[0]; Right = margins[1]; Top = margins[2]; Bottom = margins[3];
   }
   if (value) XFree(value);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setWindowSurface(HOSTWINDOW WindowHandle, OBJECTID SurfaceID)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   window->SurfaceID = SurfaceID;
   XChangeProperty(Data->Connection, window->Native, Data->SurfaceAtom, Data->SurfaceAtom, 32, PropModeReplace,
      (uint8_t *)&SurfaceID, 1); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::windowSurface(HOSTWINDOW WindowHandle, OBJECTID &SurfaceID)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   SurfaceID = window->SurfaceID; return ERR::Okay;
}

//********************************************************************************************************************
// Unlike the Win32 device context, the X11 drawable is owned by the window for as long as the window exists, so
// there is no matching release operation.  Tearing the drawable down between paint cycles would leave the display
// bitmap unusable for host-side blits, fills and resizes.

ERR X11Driver::acquireWindowBitmap(HOSTWINDOW WindowHandle, extBitmap *Bitmap)
{
   auto window = x11_window(Data, WindowHandle); auto bitmap = x11_bitmap(Bitmap);
   if ((not window) or (not bitmap)) return ERR::NullArgs;
   bitmap->WindowID = window->Native; bitmap->DrawableID = window->Background ? window->Background : window->Native;
   bitmap->WindowGraphicsContext = window->GraphicsContext;
   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef XRANDR_ENABLED
// Returns the refresh rate of the mode that is active on an output's CRTC, or zero if it cannot be determined.

static double output_refresh_rate(Display *Connection, RROutput Output)
{
   double rate = 0;
   auto resources = XRRGetScreenResourcesCurrent(Connection, DefaultRootWindow(Connection));
   if (not resources) return 0;

   if (auto output = XRRGetOutputInfo(Connection, resources, Output)) {
      if (output->crtc) {
         if (auto crtc = XRRGetCrtcInfo(Connection, resources, output->crtc)) {
            for (int i=0; i < resources->nmode; i++) {
               auto &mode = resources->modes[i];
               if ((mode.id IS crtc->mode) and mode.hTotal and mode.vTotal) {
                  rate = double(mode.dotClock) / (double(mode.hTotal) * double(mode.vTotal));
                  if (mode.modeFlags & RR_DoubleScan) rate *= 0.5;
                  if (mode.modeFlags & RR_Interlace) rate *= 2.0;
                  break;
               }
            }
            XRRFreeCrtcInfo(crtc);
         }
      }
      XRRFreeOutputInfo(output);
   }

   XRRFreeScreenResources(resources);
   return rate;
}
#endif

//********************************************************************************************************************
// Monitor geometry follows the Win32 model.  The monitor fields describe the monitor that has the largest overlap with
// the display's window (or the nearest monitor if there is no overlap), or the primary monitor if no display is
// specified.  The virtual fields describe the bounding rectangle of all active monitors.  If RandR 1.5 is unavailable,
// the root window is reported for both.

ERR X11Driver::displayInfo(DisplayInfo &Info)
{
   if (not Data->Open) return ERR::NotInitialised;

   auto screen = DefaultScreen(Data->Connection);
   Info.MonitorX = Info.MonitorY = Info.VirtualX = Info.VirtualY = 0;
   Info.MonitorWidth = Info.VirtualWidth = DisplayWidth(Data->Connection, screen);
   Info.MonitorHeight = Info.VirtualHeight = DisplayHeight(Data->Connection, screen);
   Info.PhysicalWidth = DisplayWidthMM(Data->Connection, screen);
   Info.PhysicalHeight = DisplayHeightMM(Data->Connection, screen);

#ifdef XRANDR_ENABLED
   int count = 0;
   XRRMonitorInfo *monitors = Data->RandRMonitors ?
      XRRGetMonitors(Data->Connection, DefaultRootWindow(Data->Connection), 1, &count) : nullptr;

   if (monitors and (count > 0)) {
      // Resolve the window area from the display, if one was specified.

      bool has_window = false;
      int wx = 0, wy = 0, ww = 0, wh = 0;
      if (Info.DisplayID) {
         if (ScopedObjectLock<extDisplay> display(Info.DisplayID, 5000); display.granted()) {
            has_window = windowCoords(display->WindowHandle, wx, wy, ww, wh) IS ERR::Okay;
         }
      }

      int left = monitors[0].x, top = monitors[0].y;
      int right = left + monitors[0].width, bottom = top + monitors[0].height;
      int selected = 0;
      int64_t best_area = -1;
      int64_t best_distance = INT64_MAX;

      for (int i=0; i < count; i++) {
         auto &m = monitors[i];
         left   = std::min(left, m.x);
         top    = std::min(top, m.y);
         right  = std::max(right, m.x + m.width);
         bottom = std::max(bottom, m.y + m.height);

         if (has_window) {
            auto ox = std::min(wx + ww, m.x + m.width) - std::max(wx, m.x);
            auto oy = std::min(wy + wh, m.y + m.height) - std::max(wy, m.y);
            if ((ox > 0) and (oy > 0)) {
               if (int64_t(ox) * oy > best_area) { best_area = int64_t(ox) * oy; selected = i; }
            }
            else if (best_area < 0) {
               // No overlap; measure the gap between the window and the monitor.
               int64_t dx = std::max({ m.x - (wx + ww), wx - (m.x + m.width), 0 });
               int64_t dy = std::max({ m.y - (wy + wh), wy - (m.y + m.height), 0 });
               if (dx * dx + dy * dy < best_distance) { best_distance = dx * dx + dy * dy; selected = i; }
            }
         }
         else if (m.primary) selected = i;
      }

      auto &monitor = monitors[selected];
      Info.MonitorX      = monitor.x;
      Info.MonitorY      = monitor.y;
      Info.MonitorWidth  = monitor.width;
      Info.MonitorHeight = monitor.height;
      Info.VirtualX      = left;
      Info.VirtualY      = top;
      Info.VirtualWidth  = right - left;
      Info.VirtualHeight = bottom - top;
      if ((monitor.mwidth > 0) and (monitor.mheight > 0)) {
         Info.PhysicalWidth  = monitor.mwidth;
         Info.PhysicalHeight = monitor.mheight;
      }

      if (monitor.noutput > 0) {
         if (auto rate = output_refresh_rate(Data->Connection, monitor.outputs[0]); rate > 1.0) {
            Info.RefreshRate = Info.MinRefresh = Info.MaxRefresh = float(rate);
         }
      }
   }

   if (monitors) XRRFreeMonitors(monitors);

   if ((Info.RefreshRate <= 1.0) and Data->RandR) {
      // Fallback for servers without RandR 1.5, which only report an integer rate for the screen as a whole.

      if (auto config = XRRGetScreenInfo(Data->Connection, DefaultRootWindow(Data->Connection))) {
         if (auto rate = XRRConfigCurrentRate(config); rate > 1) {
            Info.RefreshRate = Info.MinRefresh = Info.MaxRefresh = float(rate);
         }
         XRRFreeScreenConfigInfo(config);
      }
   }
#endif

   if (not Info.Width) Info.Width = Info.MonitorWidth;
   if (not Info.Height) Info.Height = Info.MonitorHeight;
   if ((not Info.HDensity) or (not Info.VDensity)) {
      // Densities that are already defined are preserved because they include any user override from the style.
      int horizontal = 0, vertical = 0;
      density(nullptr, horizontal, vertical);
      if (not Info.HDensity) Info.HDensity = horizontal;
      if (not Info.VDensity) Info.VDensity = vertical;
   }

   Info.BitsPerPixel = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
   Info.BytesPerPixel = Info.BitsPerPixel <= 8 ? 1 : Info.BitsPerPixel <= 16 ? 2 : Info.BitsPerPixel <= 24 ? 3 : 4;
   int format_count = 0;
   if (auto formats = XListPixmapFormats(Data->Connection, &format_count)) {
      for (int i=0; i < format_count; i++) if (formats[i].depth IS Info.BitsPerPixel) {
         Info.BytesPerPixel = (formats[i].bits_per_pixel + 7) / 8;
         break;
      }
      XFree(formats);
   }
   if (Info.BytesPerPixel IS 4) Info.BitsPerPixel = 32;
   Info.AccelFlags = ACF(-1);
   return ERR::Okay;
}

//********************************************************************************************************************
// Xft.dpi is the X11 counterpart of the Windows system DPI: desktop environments set it to reflect the user's scaling
// preference.  As on Windows, values below 96 are not reported.

ERR X11Driver::density(HOSTWINDOW, int &Horizontal, int &Vertical)
{
   if (not Data->Open) return ERR::NotInitialised;
   Horizontal = Vertical = std::max(96, Data->DPI.load());
   return ERR::Okay;
}

//********************************************************************************************************************
// Reads Xft.dpi from the root window's RESOURCE_MANAGER property, or returns zero if it is undefined.
// XResourceManagerString() is not used because it is a snapshot taken when the connection was opened.

int x11_read_dpi(X11Driver::State *State)
{
   constexpr std::string_view KEY = "Xft.dpi:";
   Atom type; int format; unsigned long count, remaining; unsigned char *value = nullptr;
   int dpi = 0;

   if ((XGetWindowProperty(State->Connection, DefaultRootWindow(State->Connection), XA_RESOURCE_MANAGER, 0,
         0x10000, 0, XA_STRING, &type, &format, &count, &remaining, &value) IS Success) and value and
         (format IS 8)) {
      std::string_view resources((const char *)value, count);
      for (size_t pos=0; pos < resources.size();) {
         auto end = resources.find('\n', pos);
         if (end IS std::string_view::npos) end = resources.size();
         if (auto line = resources.substr(pos, end - pos); line.starts_with(KEY)) {
            auto number = std::strtod(std::string(line.substr(KEY.size())).c_str(), nullptr);
            if ((number > 0) and (number < 10000)) dpi = int(std::lround(number));
            break;
         }
         pos = end + 1;
      }
   }

   if (value) XFree(value);
   return dpi;
}

//********************************************************************************************************************

// Hosted windows list the primary monitor's modes in the server's order of preference.  In window manager mode,
// list the whole-screen sizes used by setDisplayMode().  These sizes are also the fallback for older servers.

ERR X11Driver::resolutions(std::vector<resolution> &List)
{
   if (not Data->Open) return ERR::NotInitialised;
#ifdef XRANDR_ENABLED
   auto root = DefaultRootWindow(Data->Connection);
   auto depth = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));

   if ((not Data->Manager) and Data->RandRMonitors) {
      if (auto resources = XRRGetScreenResourcesCurrent(Data->Connection, root)) {
         // Use the primary output, or the first connected output if no primary has been nominated.

         XRROutputInfo *output = nullptr;
         if (auto primary = XRRGetOutputPrimary(Data->Connection, root)) {
            output = XRRGetOutputInfo(Data->Connection, resources, primary);
         }

         for (int i=0; (not output) and (i < resources->noutput); i++) {
            output = XRRGetOutputInfo(Data->Connection, resources, resources->outputs[i]);
            if (output and ((output->connection != RR_Connected) or (not output->crtc))) {
               XRRFreeOutputInfo(output);
               output = nullptr;
            }
         }

         if (output) {
            for (int m=0; m < output->nmode; m++) {
               for (int i=0; i < resources->nmode; i++) {
                  auto &mode = resources->modes[i];
                  if (mode.id != output->modes[m]) continue;
                  if ((mode.width >= 640) and (mode.height >= 480) and
                      std::none_of(List.begin(), List.end(), [&](const resolution &Res) {
                         return (Res.width IS int(mode.width)) and (Res.height IS int(mode.height));
                      })) {
                     List.emplace_back(mode.width, mode.height, depth);
                  }
                  break;
               }
            }
            XRRFreeOutputInfo(output);
         }

         XRRFreeScreenResources(resources);
         if (not List.empty()) return ERR::Okay;
      }
   }

   int count = 0;
   if (Data->RandR) {
      if (auto sizes = XRRSizes(Data->Connection, DefaultScreen(Data->Connection), &count); sizes and count) {
         for (int i=0; i < count; i++) if ((sizes[i].width >= 640) and (sizes[i].height >= 480)) {
            List.emplace_back(sizes[i].width, sizes[i].height,
               DefaultDepth(Data->Connection, DefaultScreen(Data->Connection)));
         }
         return ERR::Okay;
      }
   }
#endif
   List.emplace_back(Data->RootAttributes.width, Data->RootAttributes.height,
      DefaultDepth(Data->Connection, DefaultScreen(Data->Connection)));
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setDisplayMode(int &Width, int &Height, int &BitsPerPixel, double)
{
#ifdef XRANDR_ENABLED
   if ((not Data->RandR) or (not Data->Manager) or Data->WSLg) return ERR::NoSupport;
   int count = 0;
   auto sizes = XRRSizes(Data->Connection, DefaultScreen(Data->Connection), &count);
   if ((not sizes) or (not count)) return ERR::SystemCall;
   int best = -1;
   int weight = 0x7fffffff;
   for (int i=0; i < count; i++) {
      auto candidate = std::abs(sizes[i].width - Width) + std::abs(sizes[i].height - Height);
      if (candidate < weight) { best = i; weight = candidate; }
   }
   auto config = XRRGetScreenInfo(Data->Connection, DefaultRootWindow(Data->Connection));
   if ((best < 0) or (not config)) return ERR::SystemCall;
   auto status = XRRSetScreenConfig(Data->Connection, config, DefaultRootWindow(Data->Connection), best, RR_Rotate_0,
      CurrentTime);
   XRRFreeScreenConfigInfo(config);
   if (status) return ERR::SystemCall;
   Width = sizes[best].width;
   Height = sizes[best].height;
   BitsPerPixel = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

ERR X11Driver::setGamma(double, double, double) { return ERR::NoSupport; }

ERR X11Driver::setPowerMode(DPMS) { return ERR::NoSupport; }

//********************************************************************************************************************

ERR X11Driver::pixelFormat(ColourFormat &Format)
{
   if (not Data->Open) return ERR::NotInitialised;
   XVisualInfo visual = {
      .visualid = XVisualIDFromVisual(DefaultVisual(Data->Connection, DefaultScreen(Data->Connection)))
   };
   int count = 0;
   auto info = XGetVisualInfo(Data->Connection, VisualIDMask, &visual, &count);
   if (not info) return ERR::SystemCall;
   int bits = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
   int format_count = 0;
   if (auto formats = XListPixmapFormats(Data->Connection, &format_count)) {
      for (int i=0; i < format_count; i++) if (formats[i].depth IS bits) { bits = formats[i].bits_per_pixel; break; }
      XFree(formats);
   }
   x11_colour_format(Format, bits, info->red_mask, info->green_mask, info->blue_mask, 0xff000000);
   XFree(info);
   return ERR::Okay;
}

//********************************************************************************************************************

static void initialise_image(extBitmap *Bitmap, X11BitmapRecord *Record)
{
   Record->Image = {};
   Record->Image.width = Bitmap->Width;
   Record->Image.height = Bitmap->Height;
   Record->Image.format = ZPixmap;
   Record->Image.data = (char *)Bitmap->Data;
   Record->Image.byte_order = LSBFirst;
   Record->Image.bitmap_unit = 32;
   Record->Image.bitmap_bit_order = LSBFirst;
   Record->Image.bitmap_pad = 32;
   Record->Image.depth = (Bitmap->BitsPerPixel IS 32) and ((Bitmap->Flags & BMF::ALPHA_CHANNEL) IS BMF::NIL) ?
      24 : Bitmap->BitsPerPixel;
   Record->Image.bytes_per_line = Bitmap->LineWidth;
   Record->Image.bits_per_pixel = Bitmap->BytesPerPixel * 8;
   if (Record->SharedImage) Record->Image.obdata = (char *)&Record->Shm;
   XInitImage(&Record->Image);
}

//********************************************************************************************************************

static ERR upload_bitmap(X11Driver::State *State, Drawable DrawableID, GC GraphicsContext, extBitmap *Bitmap,
   X11BitmapRecord *Record, int X, int Y, int Width, int Height, int XDest, int YDest)
{
   const bool alpha = (Bitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL;
   const bool convert_alpha = alpha and ((Bitmap->Flags & BMF::PREMUL) IS BMF::NIL);
   if (convert_alpha) {
      if (auto error = Bitmap->premultiply(); error != ERR::Okay) return error;
   }

   initialise_image(Bitmap, Record);
   if ((not Record->SharedImage) or
         (not XShmPutImage(State->Connection, DrawableID, GraphicsContext, &Record->Image,
            X, Y, XDest, YDest, Width, Height, 0))) {
      XPutImage(State->Connection, DrawableID, GraphicsContext, &Record->Image,
         X, Y, XDest, YDest, Width, Height);
   }

   if (alpha) XSync(State->Connection, 0);
   if (convert_alpha) return Bitmap->demultiply();
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::present(HOSTWINDOW WindowHandle, extBitmap *Source, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   auto window = x11_window(Data, WindowHandle);
   if ((not window) or (not Source)) return ERR::NullArgs;
   auto drawable = window->Background ? Drawable(window->Background) : Drawable(window->Native);
   auto gc = window->GraphicsContext ? window->GraphicsContext : Data->GraphicsContext;
   auto source = x11_bitmap(Source);
   if (source and source->DrawableID) {
      XCopyArea(Data->Connection, source->DrawableID, drawable, gc, X, Y, Width, Height, XDest, YDest);
   }
   else if (Source->Data) {
      if (not source) {
         source = new(std::nothrow) X11BitmapRecord;
         if (not source) return ERR::AllocMemory;
         source->Connection = Data->Connection;
         source->DefaultGraphicsContext = Data->GraphicsContext;
         Source->DriverData = source;
      }
      if (auto error = upload_bitmap(Data, drawable, gc, Source, source, X, Y, Width, Height, XDest, YDest);
            error != ERR::Okay) return error;
   }
   else return ERR::NoSupport;
   if (window->Background) XClearArea(Data->Connection, window->Native, XDest, YDest, Width, Height, 0);
   x11_end_frame(Data, window);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::blitBitmap(extBitmap *Destination, extBitmap *Source, BAF Flags, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   if ((not Destination) or (not Source)) return ERR::NullArgs;
   auto destination = x11_bitmap(Destination);
   auto source = x11_bitmap(Source);
   if ((not destination) or (not destination->DrawableID)) return ERR::NoSupport;
   if (((Flags & BAF::BLEND) != BAF::NIL) or ((Source->Flags & BMF::TRANSPARENT) != BMF::NIL)) {
      return ERR::NoSupport;
   }
   auto gc = destination->WindowGraphicsContext ? destination->WindowGraphicsContext : Data->GraphicsContext;
   if (source and source->DrawableID) {
      XCopyArea(Data->Connection, source->DrawableID, destination->DrawableID, gc, X, Y, Width, Height, XDest, YDest);
      return ERR::Okay;
   }
   if (Source->Data) {
      if (not source) {
         source = new(std::nothrow) X11BitmapRecord;
         if (not source) return ERR::AllocMemory;
         source->Connection = Data->Connection;
         source->DefaultGraphicsContext = Data->GraphicsContext;
         Source->DriverData = source;
      }
      return upload_bitmap(Data, destination->DrawableID, gc, Source, source,
         X, Y, Width, Height, XDest, YDest);
   }
   return ERR::NoSupport;
}

//********************************************************************************************************************

ERR X11Driver::fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour)
{
   auto bitmap = x11_bitmap(Destination);
   if ((not bitmap) or (not bitmap->DrawableID)) return ERR::NoSupport;
   auto gc = bitmap->WindowGraphicsContext ? bitmap->WindowGraphicsContext : Data->GraphicsContext;
   XSetForeground(Data->Connection, gc, Colour);
   XFillRectangle(Data->Connection, bitmap->DrawableID, gc, X, Y, Width, Height);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::flush()
{
   if (not Data->Open) return ERR::NotInitialised;
   XSync(Data->Connection, 0);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::allocBitmap(extBitmap *Bitmap)
{
   const std::lock_guard lock(Data->NativeLock);
   if (not Bitmap) return ERR::NullArgs;
   if (Bitmap->DriverData) return ERR::Okay;

   if ((Bitmap->MemType IS BMT::TEXTURE) or
         ((Bitmap->MemType IS BMT::VIDEO) and ((Bitmap->Flags & BMF::NO_DATA) IS BMF::NIL))) {
      Bitmap->MemType = BMT::DATA;
   }

   auto record = new(std::nothrow) X11BitmapRecord;
   if (not record) return ERR::AllocMemory;

   record->Connection = Data->Connection;
   record->DefaultGraphicsContext = Data->GraphicsContext;
   if (Data->SharedImages and (Bitmap->MemType IS BMT::DATA) and (not Bitmap->Data) and
         ((Bitmap->Flags & BMF::NO_DATA) IS BMF::NIL) and (Bitmap->Size > 0)) {
      record->Shm.shmid = shmget(IPC_PRIVATE, Bitmap->Size, IPC_CREAT|IPC_EXCL|0600);
      if (record->Shm.shmid IS -1) {
         delete record;
         return ERR::Memory;
      }

      record->Shm.shmaddr = (char *)shmat(record->Shm.shmid, nullptr, 0);
      if (record->Shm.shmaddr IS (char *)-1) {
         shmctl(record->Shm.shmid, IPC_RMID, nullptr);
         delete record;
         return ERR::LockFailed;
      }

      record->Shm.readOnly = 0;
      Bitmap->Data = (uint8_t *)record->Shm.shmaddr;
      initialise_image(Bitmap, record);
      record->Image.obdata = (char *)&record->Shm;

      if (XShmAttach(Data->Connection, &record->Shm)) record->SharedImage = true;
      else {
         shmdt(record->Shm.shmaddr);
         shmctl(record->Shm.shmid, IPC_RMID, nullptr);
         Bitmap->Data = nullptr;
         delete record;
         return ERR::SystemCall;
      }

      if (record->SharedImage) Bitmap->prvAFlags |= BF_DRIVER_DATA;
   }

   Bitmap->DriverData = record;
   Data->Bitmaps.insert(Bitmap);
   if (Bitmap->MemType IS BMT::VIDEO) Bitmap->prvAFlags |= BF_WINVIDEO;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::freeBitmap(extBitmap *Bitmap)
{
   const std::lock_guard lock(Data->NativeLock);
   auto record = x11_bitmap(Bitmap);
   if (not record) return ERR::Okay;
   const bool clear_data = record->SharedImage or record->Readable;

   if (Data->Connection) {
      if (record->SharedImage) {
         XShmDetach(Data->Connection, &record->Shm);
         XSync(Data->Connection, 0);
         shmdt(record->Shm.shmaddr);
         shmctl(record->Shm.shmid, IPC_RMID, nullptr);
         Bitmap->Data = nullptr;
      }

      if (record->Readable) XDestroyImage(record->Readable);
      if (record->OwnsDrawable and record->DrawableID) XFreePixmap(Data->Connection, record->DrawableID);
   }

   delete record;
   Bitmap->DriverData = nullptr;
   if (clear_data) Bitmap->Data = nullptr;
   Data->Bitmaps.erase(Bitmap);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::resizeBitmap(extBitmap *Bitmap, int Width, int Height)
{
   const std::lock_guard lock(Data->NativeLock);

   auto record = x11_bitmap(Bitmap);
   if (not record) return ERR::NoSupport;

   if (record->DrawableID and record->WindowID) {
      auto window_it = Data->Windows.find(record->WindowID);
      if ((window_it IS Data->Windows.end()) or (not window_it->second->Background)) return ERR::NoSupport;
      if ((record->PixmapWidth >= Width) and (record->PixmapHeight >= Height)) return ERR::Okay;
      record->PixmapWidth = std::max(record->PixmapWidth, Width);
      record->PixmapHeight = std::max(record->PixmapHeight, Height);
      auto depth = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
      auto pixmap = XCreatePixmap(Data->Connection, record->WindowID, record->PixmapWidth, record->PixmapHeight,
         depth);
      if (not pixmap) return ERR::AllocMemory;
      XSetWindowBackgroundPixmap(Data->Connection, record->WindowID, pixmap);
      XFreePixmap(Data->Connection, window_it->second->Background);
      window_it->second->Background = pixmap;
      record->DrawableID = pixmap;
      Bitmap->Width = Width;
      Bitmap->Height = Height;
      Bitmap->Clip.Right = Width;
      Bitmap->Clip.Bottom = Height;
      return ERR::Okay;
   }

   if (not record->SharedImage) return ERR::NoSupport;

   const int byte_width = Bitmap->Type IS BMP::PLANAR ? (Width + 7) / 8 : Width * Bitmap->BytesPerPixel;
   const int line_width = ALIGN32(byte_width);
   const int plane_mod = line_width * Height;
   const int size = Bitmap->Type IS BMP::PLANAR ? plane_mod * Bitmap->BitsPerPixel : plane_mod;

   XShmSegmentInfo next = {};
   next.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT|IPC_EXCL|0600);
   if (next.shmid IS -1) return ERR::Memory;
   next.shmaddr = (char *)shmat(next.shmid, nullptr, 0);
   if (next.shmaddr IS (char *)-1) {
      shmctl(next.shmid, IPC_RMID, nullptr);
      return ERR::LockFailed;
   }

   next.readOnly = 0;
   if (not XShmAttach(Data->Connection, &next)) {
      shmdt(next.shmaddr);
      shmctl(next.shmid, IPC_RMID, nullptr);
      return ERR::SystemCall;
   }

   XShmDetach(Data->Connection, &record->Shm);
   XSync(Data->Connection, 0);
   shmdt(record->Shm.shmaddr);
   shmctl(record->Shm.shmid, IPC_RMID, nullptr);
   record->Shm = next;
   Bitmap->Data = (uint8_t *)next.shmaddr;
   Bitmap->Width = Width;
   Bitmap->Height = Height;
   Bitmap->ByteWidth = byte_width;
   Bitmap->LineWidth = line_width;
   Bitmap->Size = size;
   Bitmap->PlaneMod = plane_mod;
   Bitmap->Clip = { 0, 0, Width, Height };
   initialise_image(Bitmap, record);
   record->Image.obdata = (char *)&record->Shm;
   return ERR::Okay;
}

//********************************************************************************************************************
// Copy the clipped region of the drawable into the readable image so that the caller observes the current content
// of the host surface rather than a snapshot taken by an earlier lock.

static void refresh_readable(X11Driver::State *State, extBitmap *Bitmap, X11BitmapRecord *Record)
{
   const int width = Bitmap->Clip.Right - Bitmap->Clip.Left;
   const int height = Bitmap->Clip.Bottom - Bitmap->Clip.Top;
   if ((width < 1) or (height < 1)) return;
   XGetSubImage(State->Connection, Record->DrawableID, Bitmap->Clip.Left, Bitmap->Clip.Top, width, height,
      0xffffffff, ZPixmap, Record->Readable, Bitmap->Clip.Left, Bitmap->Clip.Top);
}

//********************************************************************************************************************

ERR X11Driver::lockBitmap(extBitmap *Bitmap, int16_t Access)
{
   if (not Bitmap) return ERR::NullArgs;
   auto record = x11_bitmap(Bitmap);
   if ((not record) or (not record->DrawableID)) return Bitmap->Data ? ERR::Okay : ERR::NoSupport;

   // A host-side copy is required only for readers.  Write-only callers reach the drawable through the pixel
   // routines, so allocating and populating a readable image for them would be wasted effort.

   if (not (Access & SURFACE_READ)) return ERR::Okay;

   if (record->Readable) {
      // Reuse the existing image when it remains large enough for the bitmap.

      if ((record->Readable->width >= Bitmap->Width) and (record->Readable->height >= Bitmap->Height)) {
         refresh_readable(Data, Bitmap, record);
         return ERR::Okay;
      }

      XDestroyImage(record->Readable); // Releases Bitmap->Data, which the image owns
      record->Readable = nullptr;
      Bitmap->Data = nullptr;
   }
   else if (Bitmap->Data) return ERR::Okay; // The data area is owned elsewhere, e.g. a shared memory segment

   int alignment;
   if (Bitmap->LineWidth & 0x0001) alignment = 8;
   else if (Bitmap->LineWidth & 0x0002) alignment = 16;
   else alignment = 32;

   const int size = Bitmap->Type IS BMP::PLANAR ? Bitmap->LineWidth * Bitmap->Height * Bitmap->BitsPerPixel
      : Bitmap->LineWidth * Bitmap->Height;

   Bitmap->Data = (uint8_t *)std::malloc(size);
   if (not Bitmap->Data) return ERR::AllocMemory;
   record->Readable = XCreateImage(Data->Connection, CopyFromParent, Bitmap->BitsPerPixel, ZPixmap, 0,
      (char *)Bitmap->Data, Bitmap->Width, Bitmap->Height, alignment, Bitmap->LineWidth);
   if (not record->Readable) { std::free(Bitmap->Data); Bitmap->Data = nullptr; return ERR::CreateResource; }
   refresh_readable(Data, Bitmap, record);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::unlockBitmap(extBitmap *) { return ERR::Okay; }

//********************************************************************************************************************

ERR X11Driver::bitmapRoutines(extBitmap *Bitmap)
{
   if ((not Bitmap) or (not x11_bitmap(Bitmap))) return ERR::NoSupport;
   // Video bitmaps receive their drawable after initialisation, when the display creates its window.
   // Shared images are driver-owned RAM and must retain the generic memory pixel routines.

   if ((Bitmap->prvAFlags & BF_WINVIDEO) IS 0) return ERR::NoSupport;
   x11_install_bitmap_routines(Bitmap);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setCursor(HOSTWINDOW WindowHandle, PTC CursorID)
{
   if (not Data->Open) return ERR::NotInitialised;
   auto cursor = cursor_for(Data, CursorID);

   // X11 attaches the cursor to a window rather than the process, so a null handle is satisfied by applying the
   // change to every window that this driver manages.

   if (not WindowHandle) {
      const std::lock_guard lock(Data->NativeLock);
      if (Data->Windows.empty()) return ERR::NoSupport;
      for (auto &entry : Data->Windows) XDefineCursor(Data->Connection, entry.first, cursor);
      XFlush(Data->Connection);
      return ERR::Okay;
   }

   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XDefineCursor(Data->Connection, window->Native, cursor);
   XFlush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setCustomCursor(HOSTWINDOW, extBitmap *, int, int) { return ERR::NoSupport; }

//********************************************************************************************************************

ERR X11Driver::showCursor(HOSTWINDOW WindowHandle, bool Visible)
{
   return setCursor(WindowHandle, Visible ? PTC::DEFAULT : PTC::INVISIBLE);
}

//********************************************************************************************************************

ERR X11Driver::warpPointer(HOSTWINDOW WindowHandle, int X, int Y)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XWarpPointer(Data->Connection, None, window->Native, 0, 0, 0, 0, X, Y); XFlush(Data->Connection); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::pointerPosition(double &X, double &Y)
{
   if (not Data->Open) return ERR::NotInitialised;
   Window root, child; int root_x, root_y, child_x, child_y; unsigned int mask;
   if (not XQueryPointer(Data->Connection, DefaultRootWindow(Data->Connection), &root, &child, &root_x, &root_y,
         &child_x, &child_y, &mask)) return ERR::SystemCall;
   X = root_x; Y = root_y; return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::grabPointer(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   return XGrabPointer(Data->Connection, window->Native, 1, PointerMotionMask|ButtonPressMask|ButtonReleaseMask,
      GrabModeAsync, GrabModeAsync, window->Native, None, CurrentTime) IS GrabSuccess ? ERR::Okay : ERR::SystemCall;
}

//********************************************************************************************************************

ERR X11Driver::ungrabPointer()
{
   if (not Data->Open) return ERR::NotInitialised;
   XUngrabPointer(Data->Connection, CurrentTime); return ERR::Okay;
}

//********************************************************************************************************************

ERR X11Driver::setHostOption(HOST Option, int64_t Value)
{
   if (Option IS HOST::TRAY_ICON) {
      Data->TrayIcon = Value;
      if (Data->TrayIcon) Data->TaskBar = false;
   }
   else if (Option IS HOST::TASKBAR) {
      Data->TaskBar = Value;
      if (Data->TaskBar) Data->TrayIcon = false;
   }
   else if (Option IS HOST::STICK_TO_FRONT) Data->StickToFront = Value;
   else return ERR::NoSupport;
   return ERR::Okay;
}

} // namespace
