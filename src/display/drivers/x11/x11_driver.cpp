#include "x11_native.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sys/shm.h>
#include <unistd.h>

namespace display {

static X11Driver::State *glX11State = nullptr;

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

static bool detect_wslg()
{
   if (auto enabled = std::getenv("WSL2_GUI_APPS_ENABLED"); enabled and (enabled[0] IS '1')) return true;
   return access("/mnt/wslg", F_OK) IS 0;
}

static int catch_redirect_error(Display *, XErrorEvent *)
{
   if (glX11State) glX11State->Manager = false;
   return 0;
}

static int catch_x_error(Display *Connection, XErrorEvent *Event)
{
   char message[128] = {};
   XGetErrorText(Connection, Event->error_code, message, sizeof(message));
   kt::Log("X11").warning("Request %d failed: %s", Event->request_code, message);
   return 0;
}

static int catch_xio_error(Display *)
{
   kt::Log("X11").error("The X11 connection was terminated.");
   return 0;
}

static void event_loop(HOSTHANDLE, APTR Data) { x11_process_events((X11Driver::State *)Data); }

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

static Cursor cursor_for(X11Driver::State *State, PTC CursorID)
{
   for (size_t i=0; i < CURSORS.size(); i++) if (CURSORS[i].first IS CursorID) return State->Cursors[i];
   return State->Cursors[0];
}

static GC create_graphics_context(X11Driver::State *State, Drawable DrawableID)
{
   XGCValues values = {};
   values.function = GXcopy;
   values.graphics_exposures = 0;
   return XCreateGC(State->Connection, DrawableID, GCGraphicsExposures|GCFunction, &values);
}

X11WindowRecord * x11_window(X11Driver::State *State, HOSTWINDOW WindowHandle)
{
   if ((not State) or (not WindowHandle)) return nullptr;
   const std::lock_guard lock(State->NativeLock);
   for (auto &[native, record] : State->Windows) if (record IS WindowHandle) return record;
   if (auto it = State->Windows.find(Window(uintptr_t(WindowHandle))); it != State->Windows.end()) return it->second;
   return nullptr;
}

X11BitmapRecord * x11_bitmap(extBitmap *Bitmap)
{
   return Bitmap ? (X11BitmapRecord *)Bitmap->DriverData : nullptr;
}

X11Driver::X11Driver() : Data(new State) { }
X11Driver::~X11Driver() { close(); delete Data; }
CSTRING X11Driver::name() const { return "x11"; }
DT X11Driver::displayType() const { return DT::X11; }

DCAP X11Driver::capabilities() const
{
   if (not Data->Open) return DCAP::NIL;
   auto result = DCAP::WINDOW_POSITION|DCAP::STACKING|DCAP::POINTER_WARP|DCAP::VIDEO_BITMAPS|DCAP::WINDOW_DECOR;
   if (Data->Composite) result |= DCAP::COMPOSITING;
   if (Data->RandR and Data->Manager and (not Data->WSLg)) result |= DCAP::MODE_SWITCH;
   if (Data->Manager) result |= DCAP::DESKTOP_MANAGER;
   return result;
}

ERR X11Driver::isAvailable() const
{
   if (auto value = std::getenv("KOTUKU_XDISPLAY"); value and value[0]) return ERR::Okay;
   if (auto value = std::getenv("DISPLAY"); value and value[0]) return ERR::Okay;
   return ERR::NoSupport;
}

ERR X11Driver::open(const DriverCallbacks &Callbacks)
{
   XGCValues values = {};
   Window root = 0;
   int major = 0, minor = 0, pixmaps = 0;
#ifdef XRANDR_ENABLED
   int event_base = 0, error_base = 0;
#endif
   if (Data->Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;
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
   Data->DeleteAtom = XInternAtom(Data->Connection, "WM_DELETE_WINDOW", 0);
   Data->TakeFocusAtom = XInternAtom(Data->Connection, "WM_TAKE_FOCUS", 0);
   Data->SurfaceAtom = XInternAtom(Data->Connection, "KOTUKU_SCREENID", 0);
   XGetWindowAttributes(Data->Connection, root, &Data->RootAttributes);
   Data->Composite = XMatchVisualInfo(Data->Connection, DefaultScreen(Data->Connection), 32, TrueColor,
      &Data->AlphaVisual) != 0;
#ifdef XDGA_ENABLED
   if ((not Data->WSLg) and ((display_name[0] IS ':') or startswith(display_name, "unix:"))) {
      int events, errors, dga_major, dga_minor;
      if (XDGAQueryExtension(Data->Connection, &events, &errors) and
            XDGAQueryVersion(Data->Connection, &dga_major, &dga_minor) and (dga_major >= 2) and
            (not SetResource(RES::PRIVILEGED_USER, TRUE))) {
         auto screen = DefaultScreen(Data->Connection);
         if (XDGAOpenFramebuffer(Data->Connection, screen)) {
            int memory_size;
            XF86DGAGetVideo(Data->Connection, screen, (char **)&Data->DGAMemory, &Data->DGAPixelsPerLine,
               &Data->DGABankSize, &memory_size);
            XDGACloseFramebuffer(Data->Connection, screen);
            Data->DGA = Data->DGAMemory != nullptr;
         }
         SetResource(RES::PRIVILEGED_USER, FALSE);
         if (GetResource(RES::PRIVILEGED) IS FALSE) setuid(getuid());
      }
   }
#endif
#ifdef XRANDR_ENABLED
   Data->RandR = XRRQueryExtension(Data->Connection, &event_base, &error_base) != 0;
#endif
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

ERR X11Driver::close()
{
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
         if (window->Owned and (not window->Root)) XDestroyWindow(Data->Connection, native);
         delete window;
      }
      Data->Windows.clear();
      for (auto cursor : Data->Cursors) if (cursor) XFreeCursor(Data->Connection, cursor);
      Data->Cursors.fill(0);
      if (Data->GraphicsContext) XFreeGC(Data->Connection, Data->GraphicsContext);
      if (Data->ClipGraphicsContext) XFreeGC(Data->Connection, Data->ClipGraphicsContext);
      XSetErrorHandler(Data->PreviousErrorHandler);
      XSetIOErrorHandler(Data->PreviousIOErrorHandler);
      if (not was_open) XCloseDisplay(Data->Connection);
   }
   glX11State = nullptr;
   Data->Connection = nullptr;
   Data->GraphicsContext = Data->ClipGraphicsContext = 0;
   Data->Open = Data->Closing = Data->WSLg = Data->SharedImages = Data->Composite = Data->RandR = false;
   Data->DGA = false;
   Data->DGAMemory = nullptr;
   Data->DGAPixelsPerLine = Data->DGABankSize = 0;
   Data->Manager = true;
   return was_open ? ERR::DoNotExpunge : ERR::Okay;
}

static ERR create_window_record(X11Driver::State *State, extDisplay *DisplayObject, Window Parent,
   X11WindowRecord *&Record)
{
   XSetWindowAttributes attributes = {};
   attributes.bit_gravity = CenterGravity;
   attributes.win_gravity = CenterGravity;
   attributes.cursor = State->Cursors[0];
   attributes.override_redirect = (DisplayObject->Flags & (SCR::BORDERLESS|SCR::COMPOSITE)) != SCR::NIL;
   attributes.event_mask = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask|
      KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask;
   int flags = CWEventMask|CWOverrideRedirect|CWCursor;
   int depth = CopyFromParent;
   Visual *visual = CopyFromParent;
   Colormap colormap = 0;
   if (attributes.override_redirect and State->Composite) {
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
   Atom protocols[] = { State->DeleteAtom, State->TakeFocusAtom };
   XSetWMProtocols(State->Connection, native, protocols, std::ssize(protocols));
   XSizeHints hints = { .flags = USPosition|USSize };
   XSetWMNormalHints(State->Connection, native, &hints);
   Record = record;
   return ERR::Okay;
}

ERR X11Driver::createWindow(extDisplay *DisplayObject, HOSTWINDOW &Handle)
{
   if ((not Data->Open) or (not DisplayObject)) return ERR::NotInitialised;
   if (Data->WSLg and ((DisplayObject->Flags & (SCR::BORDERLESS|SCR::MAXIMISE)) IS
         (SCR::BORDERLESS|SCR::MAXIMISE))) DisplayObject->Flags &= ~SCR::BORDERLESS;
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
      XSetWindowAttributes attributes = {
         .event_mask = ExposureMask|EnterWindowMask|LeaveWindowMask|PointerMotionMask|StructureNotifyMask|
            KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask
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
   else if (glStickToFront) XSetTransientForHint(Data->Connection, record->Native,
      DefaultRootWindow(Data->Connection));
   if (auto bitmap = x11_bitmap((extBitmap *)DisplayObject->Bitmap)) {
      bitmap->WindowID = record->Native;
      bitmap->WindowGraphicsContext = record->GraphicsContext;
      if ((DisplayObject->Bitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL) bitmap->DrawableID = record->Native;
      else {
         bitmap->PixmapWidth = std::max(DisplayObject->Width, Data->RootAttributes.width);
         bitmap->PixmapHeight = std::max(DisplayObject->Height, Data->RootAttributes.height);
         auto depth = DefaultDepth(Data->Connection, DefaultScreen(Data->Connection));
         record->Background = XCreatePixmap(Data->Connection, record->Native, bitmap->PixmapWidth,
            bitmap->PixmapHeight, depth);
         if (not record->Background) {
            destroyWindow(record);
            Handle = nullptr;
            return ERR::SystemCall;
         }
         bitmap->DrawableID = record->Background;
         XSetWindowBackgroundPixmap(Data->Connection, record->Native, record->Background);
      }
   }
   if (record->Root and Data->DGA) {
      DisplayObject->Bitmap->Flags |= BMF::X11_DGA;
      DisplayObject->Bitmap->Data = (uint8_t *)Data->DGAMemory;
      DisplayObject->Bitmap->LineWidth = Data->DGAPixelsPerLine * DisplayObject->Bitmap->BytesPerPixel;
   }
   return ERR::Okay;
}

ERR X11Driver::adoptWindow(extDisplay *DisplayObject, APTR NativeHandle, HOSTWINDOW &Handle)
{
   if (not NativeHandle) return ERR::NullArgs;
   X11WindowRecord *record = nullptr;
   if (auto error = create_window_record(Data, DisplayObject, Window(uintptr_t(NativeHandle)), record);
         error != ERR::Okay) return error;
   Handle = record;
   DisplayObject->Flags |= SCR::HOSTED;
   if (auto bitmap = x11_bitmap((extBitmap *)DisplayObject->Bitmap)) {
      bitmap->WindowID = bitmap->DrawableID = record->Native;
      bitmap->WindowGraphicsContext = record->GraphicsContext;
   }
   return ERR::Okay;
}

ERR X11Driver::nativeWindowHandle(HOSTWINDOW WindowHandle, APTR &NativeHandle)
{
   auto window = x11_window(Data, WindowHandle);
   if (not window) return ERR::NoSupport;
   NativeHandle = (APTR)(uintptr_t)window->Native;
   return ERR::Okay;
}

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
   if (window->Owned and (not window->Root)) XDestroyWindow(Data->Connection, window->Native);
   delete window;
   return ERR::Okay;
}

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

ERR X11Driver::hideWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XUnmapWindow(Data->Connection, window->Native); window->Visible = false; return ERR::Okay;
}

ERR X11Driver::focusWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XSetInputFocus(Data->Connection, window->Native, RevertToNone, CurrentTime); return ERR::Okay;
}

ERR X11Driver::moveWindow(HOSTWINDOW WindowHandle, int X, int Y)
{
   auto window = x11_window(Data, WindowHandle); if ((not window) or Data->Manager) return ERR::NoSupport;
   XMoveWindow(Data->Connection, window->Native, X, Y); return ERR::Okay;
}

ERR X11Driver::resizeWindow(HOSTWINDOW WindowHandle, int X, int Y, int Width, int Height)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   if (Data->Manager) { int bpp = 0; return setDisplayMode(Width, Height, bpp, 0); }
   // ConfigureNotify feedback has already applied this geometry; echoing it would fight an interactive WM resize.
   if (window->Display and (window->Display->Width IS Width) and (window->Display->Height IS Height) and
         (((X IS 0x7fffffff) and (Y IS 0x7fffffff)) or
            ((window->Display->X IS X) and (window->Display->Y IS Y)))) return ERR::Okay;
   if ((X != 0x7fffffff) and (Y != 0x7fffffff)) XMoveWindow(Data->Connection, window->Native, X, Y);
   XResizeWindow(Data->Connection, window->Native, Width, Height); return ERR::Okay;
}

ERR X11Driver::raiseWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XRaiseWindow(Data->Connection, window->Native); return ERR::Okay;
}

ERR X11Driver::lowerWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XLowerWindow(Data->Connection, window->Native); return ERR::Okay;
}

ERR X11Driver::minimiseWindow(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XIconifyWindow(Data->Connection, window->Native, DefaultScreen(Data->Connection)); return ERR::Okay;
}

ERR X11Driver::setWindowTitle(HOSTWINDOW WindowHandle, CSTRING Title)
{
   auto window = x11_window(Data, WindowHandle); if ((not window) or (not Title)) return ERR::NullArgs;
   XStoreName(Data->Connection, window->Native, Title); return ERR::Okay;
}

ERR X11Driver::windowTitle(HOSTWINDOW WindowHandle, std::string &Title)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   char *title = nullptr; if (not XFetchName(Data->Connection, window->Native, &title)) return ERR::SystemCall;
   Title = title ? title : ""; if (title) XFree(title); return ERR::Okay;
}

ERR X11Driver::setSizeHints(HOSTWINDOW WindowHandle, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XSizeHints hints = {};
   if ((MaxW > 0) and (MaxH > 0)) { hints.max_width = MaxW; hints.max_height = MaxH; hints.flags |= PMaxSize; }
   if ((MinW > 0) and (MinH > 0)) { hints.min_width = MinW; hints.min_height = MinH; hints.flags |= PMinSize; }
   if (EnforceAspect and (hints.flags & PMaxSize) and (hints.flags & PMinSize)) {
      hints.flags |= PAspect; hints.min_aspect = { MinW, MinH }; hints.max_aspect = { MinW, MinH };
   }
   XSetWMNormalHints(Data->Connection, window->Native, &hints); return ERR::Okay;
}

ERR X11Driver::windowCoords(HOSTWINDOW WindowHandle, int &X, int &Y, int &Width, int &Height)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XWindowAttributes attributes;
   if (not XGetWindowAttributes(Data->Connection, window->Native, &attributes)) return ERR::SystemCall;
   Window child; XTranslateCoordinates(Data->Connection, window->Native, DefaultRootWindow(Data->Connection), 0, 0,
      &X, &Y, &child); Width = attributes.width; Height = attributes.height; return ERR::Okay;
}

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

ERR X11Driver::setWindowSurface(HOSTWINDOW WindowHandle, OBJECTID SurfaceID)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   window->SurfaceID = SurfaceID;
   XChangeProperty(Data->Connection, window->Native, Data->SurfaceAtom, Data->SurfaceAtom, 32, PropModeReplace,
      (uint8_t *)&SurfaceID, 1); return ERR::Okay;
}

ERR X11Driver::windowSurface(HOSTWINDOW WindowHandle, OBJECTID &SurfaceID)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   SurfaceID = window->SurfaceID; return ERR::Okay;
}

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

ERR X11Driver::displayInfo(DisplayInfo &Info)
{
   if (not Data->Open) return ERR::NotInitialised;
   Info.Width = Data->RootAttributes.width;
   Info.Height = Data->RootAttributes.height;
   Info.MonitorWidth = Info.VirtualWidth = Info.Width;
   Info.MonitorHeight = Info.VirtualHeight = Info.Height;
   Info.PhysicalWidth = DisplayWidthMM(Data->Connection, DefaultScreen(Data->Connection));
   Info.PhysicalHeight = DisplayHeightMM(Data->Connection, DefaultScreen(Data->Connection));
   Info.HDensity = Info.VDensity = 96;
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

ERR X11Driver::density(HOSTWINDOW, int &Horizontal, int &Vertical)
{
   Horizontal = Vertical = 96;
   return Data->Open ? ERR::Okay : ERR::NotInitialised;
}

ERR X11Driver::resolutions(std::vector<resolution> &List)
{
   if (not Data->Open) return ERR::NotInitialised;
#ifdef XRANDR_ENABLED
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

ERR X11Driver::setGamma(double, double, double) { return ERR::NoSupport; }
ERR X11Driver::setPowerMode(DPMS) { return ERR::NoSupport; }

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
   gfx::GetColourFormat(&Format, bits, info->red_mask, info->green_mask, info->blue_mask, 0xff000000);
   XFree(info);
   return ERR::Okay;
}

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
   return ERR::Okay;
}

ERR X11Driver::blitBitmap(extBitmap *Destination, extBitmap *Source, BAF, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   auto destination = x11_bitmap(Destination);
   auto source = x11_bitmap(Source);
   if ((not destination) or (not destination->DrawableID)) return ERR::NoSupport;
   auto gc = destination->WindowGraphicsContext ? destination->WindowGraphicsContext : Data->GraphicsContext;
   if (source and source->DrawableID) {
      XCopyArea(Data->Connection, source->DrawableID, destination->DrawableID, gc, X, Y, Width, Height, XDest, YDest);
      return ERR::Okay;
   }
   if (Source and Source->Data) {
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

ERR X11Driver::fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour)
{
   auto bitmap = x11_bitmap(Destination);
   if ((not bitmap) or (not bitmap->DrawableID)) return ERR::NoSupport;
   auto gc = bitmap->WindowGraphicsContext ? bitmap->WindowGraphicsContext : Data->GraphicsContext;
   XSetForeground(Data->Connection, gc, Colour);
   XFillRectangle(Data->Connection, bitmap->DrawableID, gc, X, Y, Width, Height);
   return ERR::Okay;
}

ERR X11Driver::flush()
{
   if (not Data->Open) return ERR::NotInitialised;
   XSync(Data->Connection, 0);
   return ERR::Okay;
}

ERR X11Driver::allocBitmap(extBitmap *Bitmap)
{
   const std::lock_guard lock(Data->NativeLock);
   if (not Bitmap) return ERR::NullArgs;
   if (Bitmap->DriverData) return ERR::Okay;
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
   const int line_width = ALIGN32(Width * Bitmap->BytesPerPixel);
   const int size = line_width * Height;
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
   Bitmap->ByteWidth = Width * Bitmap->BytesPerPixel;
   Bitmap->LineWidth = line_width;
   Bitmap->Size = size;
   Bitmap->PlaneMod = Bitmap->ByteWidth * Height;
   Bitmap->Clip = { 0, 0, Width, Height };
   initialise_image(Bitmap, record);
   record->Image.obdata = (char *)&record->Shm;
   return ERR::Okay;
}

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

ERR X11Driver::lockBitmap(extBitmap *Bitmap)
{
   if (not Bitmap) return ERR::NullArgs;
   auto record = x11_bitmap(Bitmap);
   if ((not record) or (not record->DrawableID)) return Bitmap->Data ? ERR::Okay : ERR::NoSupport;

   // DGA maps the framebuffer directly, so no read-back is required.

   if (((Bitmap->Flags & BMF::X11_DGA) != BMF::NIL) and Data->DGA) return ERR::Okay;

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

ERR X11Driver::unlockBitmap(extBitmap *) { return ERR::Okay; }

ERR X11Driver::bitmapRoutines(extBitmap *Bitmap)
{
   if ((not Bitmap) or (not x11_bitmap(Bitmap))) return ERR::NoSupport;
   x11_install_bitmap_routines(Bitmap);
   return ERR::Okay;
}

ERR X11Driver::setCursor(HOSTWINDOW WindowHandle, PTC CursorID)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XDefineCursor(Data->Connection, window->Native, cursor_for(Data, CursorID)); return ERR::Okay;
}

ERR X11Driver::setCustomCursor(HOSTWINDOW, extBitmap *, int, int) { return ERR::NoSupport; }

ERR X11Driver::showCursor(HOSTWINDOW WindowHandle, bool Visible)
{
   return setCursor(WindowHandle, Visible ? PTC::DEFAULT : PTC::INVISIBLE);
}

ERR X11Driver::warpPointer(HOSTWINDOW WindowHandle, int X, int Y)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   XWarpPointer(Data->Connection, None, window->Native, 0, 0, 0, 0, X, Y); XFlush(Data->Connection); return ERR::Okay;
}

ERR X11Driver::pointerPosition(double &X, double &Y)
{
   if (not Data->Open) return ERR::NotInitialised;
   Window root, child; int root_x, root_y, child_x, child_y; unsigned int mask;
   if (not XQueryPointer(Data->Connection, DefaultRootWindow(Data->Connection), &root, &child, &root_x, &root_y,
         &child_x, &child_y, &mask)) return ERR::SystemCall;
   X = root_x; Y = root_y; return ERR::Okay;
}

ERR X11Driver::grabPointer(HOSTWINDOW WindowHandle)
{
   auto window = x11_window(Data, WindowHandle); if (not window) return ERR::NoSupport;
   return XGrabPointer(Data->Connection, window->Native, 1, PointerMotionMask|ButtonPressMask|ButtonReleaseMask,
      GrabModeAsync, GrabModeAsync, window->Native, None, CurrentTime) IS GrabSuccess ? ERR::Okay : ERR::SystemCall;
}

ERR X11Driver::ungrabPointer()
{
   if (not Data->Open) return ERR::NotInitialised;
   XUngrabPointer(Data->Connection, CurrentTime); return ERR::Okay;
}

ERR X11Driver::setHostOption(HOST Option, int64_t Value)
{
   if (Option IS HOST::TRAY_ICON) {
      glTrayIcon = Value;
      if (glTrayIcon) glTaskBar = 0;
   }
   else if (Option IS HOST::TASKBAR) {
      glTaskBar = Value;
      if (glTaskBar) glTrayIcon = 0;
   }
   else if (Option IS HOST::STICK_TO_FRONT) glStickToFront = Value;
   else return ERR::NoSupport;
   return ERR::Okay;
}

DisplayDriver * get_x11_driver()
{
   static X11Driver driver;
   return &driver;
}

}
