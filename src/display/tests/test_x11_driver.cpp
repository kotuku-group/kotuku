#include "../drivers/x11/x11_driver.h"

#include <kotuku/modules/display.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

static std::string glClipboardData;
static std::string glClipboardMime;
static int glClipboardLost = 0;
static bool glSharedImageAttachFailed = false;

static int check(bool Condition)
{
   return Condition ? 0 : 1;
}

static void clipboard_data(CSTRING Mime, CSTRING Data, size_t Length, bool, OBJECTID)
{
   glClipboardMime = Mime ? Mime : "";
   glClipboardData.assign(Data, Length);
}

static void clipboard_lost()
{
   glClipboardLost++;
}

static ERR register_fd(HOSTHANDLE, RFD, void (*)(HOSTHANDLE, APTR), APTR)
{
   return ERR::Okay;
}

// The driver logs through Core, e.g. when it abandons a stalled clipboard transfer.

static void vlogf(VLF, CSTRING, CSTRING, va_list) { }
static void log_return() { }
static int adjust_log_level(int) { return 0; }

// Refuse a selection request so that its requestor does not wait for a reply until it times out.

static void refuse_request(Display *Connection, const XSelectionRequestEvent &Request)
{
   XSelectionEvent reply = {};
   reply.type = SelectionNotify;
   reply.display = Connection;
   reply.requestor = Request.requestor;
   reply.selection = Request.selection;
   reply.target = Request.target;
   reply.property = None;
   reply.time = Request.time;
   XSendEvent(Connection, Request.requestor, False, NoEventMask, (XEvent *)&reply);
   XFlush(Connection);
}

// Returns true if a selection request was made by the driver.  Requests from other clients are refused: desktop clients
// routinely request TARGETS whenever the clipboard owner changes, and the tests must not mistake them for the driver's.

static bool from_driver(DisplayDriver *Driver, Display *Connection, const XSelectionRequestEvent &Request)
{
   if (Request.requestor IS Window(display::x11_driver_clipboard_window(Driver))) return true;
   refuse_request(Connection, Request);
   return false;
}

static ERR resolve_path(const std::string_view &Path, RSF, std::string *Result)
{
   auto resolved = realpath(std::string(Path).c_str(), nullptr);
   if (not resolved) return ERR::FileNotFound;
   if (Result) Result->assign(resolved);
   free(resolved);
   return ERR::Okay;
}

static std::optional<std::string> environment_value(CSTRING Name)
{
   if (auto value = std::getenv(Name)) return value;
   return std::nullopt;
}

static void restore_environment(CSTRING Name, const std::optional<std::string> &Value)
{
   if (Value) setenv(Name, Value->c_str(), 1);
   else unsetenv(Name);
}

static bool create_file(CSTRING Path)
{
   auto descriptor = open(Path, O_CREAT|O_EXCL|O_WRONLY, 0600);
   if (descriptor IS -1) return false;
   close(descriptor);
   return true;
}

static bool request_selection(DisplayDriver *Driver, Display *Connection, Window WindowID, Atom Selection,
   Atom Target, std::string &Data, bool &Incremental)
{
   auto property = XInternAtom(Connection, "KOTUKU_TEST_SELECTION", False);
   auto incr = XInternAtom(Connection, "INCR", False);
   XDeleteProperty(Connection, WindowID, property);
   XConvertSelection(Connection, Selection, Target, property, WindowID, CurrentTime);
   XFlush(Connection);
   Data.clear();
   Incremental = false;
   bool notified = false;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

   while (std::chrono::steady_clock::now() < deadline) {
      display::x11_process_driver_events(Driver);
      while (XPending(Connection)) {
         XEvent event = {};
         XNextEvent(Connection, &event);
         if ((event.type IS SelectionNotify) and (event.xselection.requestor IS WindowID) and
             (event.xselection.selection IS Selection) and (event.xselection.target IS Target)) {
            if (event.xselection.property IS None) return false;
            notified = true;
         }
         else if ((event.type != PropertyNotify) or (event.xproperty.window != WindowID) or
                  (event.xproperty.atom != property) or (event.xproperty.state != PropertyNewValue)) continue;

         if (not notified) continue;
         Atom actual_type = None;
         int actual_format = 0;
         unsigned long count = 0, remaining = 0;
         unsigned char *value = nullptr;
         auto status = XGetWindowProperty(Connection, WindowID, property, 0,
            long(std::numeric_limits<int>::max() / 4), True, AnyPropertyType,
            &actual_type, &actual_format, &count, &remaining, &value);
         if ((status != Success) or remaining) {
            if (value) XFree(value);
            return false;
         }
         if (actual_type IS incr) {
            Incremental = true;
            if (value) XFree(value);
            continue;
         }
         if (actual_format != 8) {
            if (value) XFree(value);
            return false;
         }
         if (count) Data.append((const char *)value, count);
         if (value) XFree(value);
         if ((not Incremental) or (not count)) return true;
      }
      usleep(1000);
   }
   return false;
}

static bool receive_external_selection(DisplayDriver *Driver, Display *Connection, Atom Target,
   const std::string &Mime, const std::string &Data, bool Incremental, bool IncrementalTargets = false)
{
   auto selection = XInternAtom(Connection, "CLIPBOARD", False);
   auto targets_atom = XInternAtom(Connection, "TARGETS", False);
   auto incr_atom = XInternAtom(Connection, "INCR", False);
   auto owner = XCreateSimpleWindow(Connection, DefaultRootWindow(Connection), 0, 0, 1, 1, 0, 0, 0);
   XSetSelectionOwner(Connection, selection, owner, CurrentTime);
   XFlush(Connection);
   glClipboardData.clear();
   glClipboardMime.clear();

   Window requestor = 0;
   Atom property = None;
   size_t offset = 0;
   bool sending = false;
   bool sending_targets = false;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while ((std::chrono::steady_clock::now() < deadline) and (glClipboardData != Data)) {
      display::x11_process_driver_events(Driver);
      while (XPending(Connection)) {
         XEvent event = {};
         XNextEvent(Connection, &event);
         if ((event.type IS SelectionRequest) and (event.xselectionrequest.selection IS selection)) {
            auto &request = event.xselectionrequest;
            if (not from_driver(Driver, Connection, request)) continue;
            auto reply_property = request.property IS None ? request.target : request.property;
            XSelectionEvent reply = {};
            reply.type = SelectionNotify;
            reply.display = Connection;
            reply.requestor = request.requestor;
            reply.selection = request.selection;
            reply.target = request.target;
            reply.property = reply_property;
            reply.time = request.time;
            if (request.target IS targets_atom) {
               if (IncrementalTargets) {
                  unsigned long length = sizeof(Atom);
                  requestor = request.requestor;
                  property = reply_property;
                  offset = 0;
                  sending = true;
                  sending_targets = true;
                  XSelectInput(Connection, requestor, PropertyChangeMask);
                  XChangeProperty(Connection, requestor, property, incr_atom, 32, PropModeReplace,
                     (const unsigned char *)&length, 1);
               }
               else {
                  Atom targets[] = { Target };
                  XChangeProperty(Connection, request.requestor, reply_property, XA_ATOM, 32, PropModeReplace,
                     (const unsigned char *)targets, 1);
               }
            }
            else if (request.target IS Target) {
               if (Incremental) {
                  unsigned long length = Data.size();
                  requestor = request.requestor;
                  property = reply_property;
                  offset = 0;
                  sending = true;
                  sending_targets = false;
                  XSelectInput(Connection, requestor, PropertyChangeMask);
                  XChangeProperty(Connection, requestor, property, incr_atom, 32, PropModeReplace,
                     (const unsigned char *)&length, 1);
               }
               else {
                  XChangeProperty(Connection, request.requestor, reply_property, Target, 8, PropModeReplace,
                     (const unsigned char *)Data.data(), int(Data.size()));
               }
            }
            else reply.property = None;
            XSendEvent(Connection, request.requestor, False, NoEventMask, (XEvent *)&reply);
            XFlush(Connection);
         }
         else if (sending and (event.type IS PropertyNotify) and (event.xproperty.state IS PropertyDelete) and
                  (event.xproperty.window IS requestor) and (event.xproperty.atom IS property)) {
            size_t length = 0;
            if (sending_targets) {
               Atom targets[] = { Target };
               length = offset ? 0 : 1;
               XChangeProperty(Connection, requestor, property, XA_ATOM, 32, PropModeAppend,
                  (const unsigned char *)targets, int(length));
            }
            else {
               length = std::min<size_t>(32768, Data.size() - offset);
               XChangeProperty(Connection, requestor, property, Target, 8, PropModeAppend,
                  (const unsigned char *)Data.data() + offset, int(length));
            }
            offset += length;
            if (not length) sending = false;
            XFlush(Connection);
         }
      }
      usleep(1000);
   }

   auto received = (glClipboardData IS Data) and (glClipboardMime IS Mime);
   auto lost_before = glClipboardLost;
   XDestroyWindow(Connection, owner);
   XFlush(Connection);
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while ((std::chrono::steady_clock::now() < deadline) and (glClipboardLost IS lost_before)) {
      display::x11_process_driver_events(Driver);
      usleep(1000);
   }
   return received and (glClipboardLost > lost_before);
}

static bool replace_unresponsive_owner(DisplayDriver *Driver, Display *Connection, Atom Target)
{
   auto selection = XInternAtom(Connection, "CLIPBOARD", False);
   auto owner = XCreateSimpleWindow(Connection, DefaultRootWindow(Connection), 0, 0, 1, 1, 0, 0, 0);
   XSetSelectionOwner(Connection, selection, owner, CurrentTime);
   XFlush(Connection);

   bool request_seen = false;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while ((std::chrono::steady_clock::now() < deadline) and (not request_seen)) {
      display::x11_process_driver_events(Driver);
      while (XPending(Connection)) {
         XEvent event = {};
         XNextEvent(Connection, &event);
         if ((event.type IS SelectionRequest) and (event.xselectionrequest.selection IS selection) and
             from_driver(Driver, Connection, event.xselectionrequest)) request_seen = true;
      }
      usleep(1000);
   }

   auto replaced = receive_external_selection(Driver, Connection, Target, "text/plain;charset=utf-8",
      "Replacement after unresponsive owner", false);
   XDestroyWindow(Connection, owner);
   XFlush(Connection);
   return request_seen and replaced;
}

static bool next_selection_request(DisplayDriver *Driver, Display *Connection, Atom Selection,
   XSelectionRequestEvent &Request)
{
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while (std::chrono::steady_clock::now() < deadline) {
      display::x11_process_driver_events(Driver);
      while (XPending(Connection)) {
         XEvent event = {};
         XNextEvent(Connection, &event);
         if ((event.type IS SelectionRequest) and (event.xselectionrequest.selection IS Selection) and
             from_driver(Driver, Connection, event.xselectionrequest)) {
            Request = event.xselectionrequest;
            return true;
         }
      }
      usleep(1000);
   }
   return false;
}

static void reply_to_selection(Display *Connection, const XSelectionRequestEvent &Request, Atom Type,
   const void *Data, int Format, int Count)
{
   auto property = Request.property IS None ? Request.target : Request.property;
   XChangeProperty(Connection, Request.requestor, property, Type, Format, PropModeReplace,
      (const unsigned char *)Data, Count);
   XSelectionEvent reply = {};
   reply.type = SelectionNotify;
   reply.display = Connection;
   reply.requestor = Request.requestor;
   reply.selection = Request.selection;
   reply.target = Request.target;
   reply.property = property;
   reply.time = Request.time;
   XSendEvent(Connection, Request.requestor, False, NoEventMask, (XEvent *)&reply);
   XFlush(Connection);
}

static bool reject_stale_reply(DisplayDriver *Driver, Display *Connection, Atom Target)
{
   auto selection = XInternAtom(Connection, "CLIPBOARD", False);
   auto time_property = XInternAtom(Connection, "KOTUKU_TEST_TIME", False);
   auto first_owner = XCreateSimpleWindow(Connection, DefaultRootWindow(Connection), 0, 0, 1, 1, 0, 0, 0);
   auto second_owner = XCreateSimpleWindow(Connection, DefaultRootWindow(Connection), 0, 0, 1, 1, 0, 0, 0);
   XSelectInput(Connection, first_owner, PropertyChangeMask);
   XChangeProperty(Connection, first_owner, time_property, XA_INTEGER, 8, PropModeAppend, nullptr, 0);
   XEvent time_event = {};
   XWindowEvent(Connection, first_owner, PropertyChangeMask, &time_event);
   auto timestamp = time_event.xproperty.time;

   XSetSelectionOwner(Connection, selection, first_owner, timestamp);
   XFlush(Connection);
   XSelectionRequestEvent first_request = {};
   if (not next_selection_request(Driver, Connection, selection, first_request)) return false;

   XSetSelectionOwner(Connection, selection, second_owner, timestamp);
   XFlush(Connection);
   if (XGetSelectionOwner(Connection, selection) != second_owner) return false;
   XSelectionRequestEvent second_request = {};
   if (not next_selection_request(Driver, Connection, selection, second_request)) return false;

   Atom offered[] = { Target };
   reply_to_selection(Connection, first_request, XA_ATOM, offered, 32, 1);
   display::x11_process_driver_events(Driver);
   glClipboardData.clear();
   glClipboardMime.clear();
   reply_to_selection(Connection, second_request, XA_ATOM, offered, 32, 1);

   XSelectionRequestEvent data_request = {};
   if (not next_selection_request(Driver, Connection, selection, data_request)) return false;
   const std::string expected = "Current owner after stale reply";
   reply_to_selection(Connection, data_request, Target, expected.data(), 8, int(expected.size()));

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while ((std::chrono::steady_clock::now() < deadline) and (glClipboardData != expected)) {
      display::x11_process_driver_events(Driver);
      usleep(1000);
   }
   auto succeeded = (glClipboardData IS expected) and (glClipboardMime IS "text/plain;charset=utf-8");
   XDestroyWindow(Connection, first_owner);
   XDestroyWindow(Connection, second_owner);
   XFlush(Connection);
   return succeeded;
}

static bool save_with_clipboard_manager(DisplayDriver *Driver, Display *Connection, Atom Selection, Atom UTF8)
{
   auto manager_selection = XInternAtom(Connection, "CLIPBOARD_MANAGER", False);
   auto save_targets = XInternAtom(Connection, "SAVE_TARGETS", False);
   auto data_property = XInternAtom(Connection, "KOTUKU_TEST_MANAGER_DATA", False);
   auto manager = XCreateSimpleWindow(Connection, DefaultRootWindow(Connection), 0, 0, 1, 1, 0, 0, 0);
   usleep(20000); // Ensure that the manager's ownership timestamp is newer than the clipboard timestamp.
   XSetSelectionOwner(Connection, manager_selection, manager, CurrentTime);
   XFlush(Connection);

   std::string saved;
   std::atomic_bool complete = false;
   auto driver_window = Window(display::x11_driver_clipboard_window(Driver)); // Read before close() releases it
   std::thread worker([&]() {
      XSelectionRequestEvent save_request = {};
      bool requested_data = false;
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while ((std::chrono::steady_clock::now() < deadline) and (not complete)) {
         while (XPending(Connection)) {
            XEvent event = {};
            XNextEvent(Connection, &event);
            if ((event.type IS SelectionRequest) and
                (event.xselectionrequest.selection IS manager_selection) and
                (event.xselectionrequest.target IS save_targets) and
                (event.xselectionrequest.requestor IS driver_window)) {
               save_request = event.xselectionrequest;
               XDeleteProperty(Connection, manager, data_property);
               XConvertSelection(Connection, Selection, UTF8, data_property, manager, CurrentTime);
               XFlush(Connection);
               requested_data = true;
            }
            else if (requested_data and (event.type IS SelectionNotify) and
                     (event.xselection.selection IS Selection) and (event.xselection.target IS UTF8)) {
               Atom actual_type = None;
               int actual_format = 0;
               unsigned long count = 0, remaining = 0;
               unsigned char *value = nullptr;
               auto status = XGetWindowProperty(Connection, manager, data_property, 0,
                  long(std::numeric_limits<int>::max() / 4), True, AnyPropertyType,
                  &actual_type, &actual_format, &count, &remaining, &value);
               if ((status IS Success) and (actual_format IS 8) and (not remaining))
                  saved.assign((const char *)value, count);
               if (value) XFree(value);
               XSetSelectionOwner(Connection, Selection, manager, CurrentTime);
               XSelectionEvent reply = {};
               reply.type = SelectionNotify;
               reply.display = Connection;
               reply.requestor = save_request.requestor;
               reply.selection = save_request.selection;
               reply.target = save_request.target;
               reply.property = save_request.property;
               reply.time = save_request.time;
               XSendEvent(Connection, save_request.requestor, False, NoEventMask, (XEvent *)&reply);
               XFlush(Connection);
               complete = true;
            }
            // Refuse other clients' requests for the manager or the clipboard it takes over.  from_driver() is not
            // used because it reads the driver, which close() is releasing on the main thread.
            else if (event.type IS SelectionRequest) refuse_request(Connection, event.xselectionrequest);
         }
         usleep(1000);
      }
   });

   auto lost_before = glClipboardLost;
   auto close_error = Driver->close();
   worker.join();
   auto succeeded = (close_error IS ERR::DoNotExpunge) and complete and
      (saved IS "Persisted X11 clipboard ✓") and (XGetSelectionOwner(Connection, Selection) IS manager) and
      (glClipboardLost IS lost_before);
   XDestroyWindow(Connection, manager);
   XFlush(Connection);
   return succeeded;
}

static void test_clipboard(DisplayDriver *Driver, const std::optional<std::string> &KotukuDisplay,
   const std::optional<std::string> &DisplayName, int &Failures)
{
   auto display_name = KotukuDisplay ? *KotukuDisplay : (DisplayName ? *DisplayName : std::string());
   if (display_name.empty()) return;

   auto peer = XOpenDisplay(display_name.c_str());
   if (not peer) { Failures++; return; }
   auto window = XCreateSimpleWindow(peer, DefaultRootWindow(peer), 0, 0, 1, 1, 0, 0, 0);
   XSelectInput(peer, window, PropertyChangeMask);

   DriverCallbacks callbacks = {
      .Version = DISPLAY_DRIVER_INTERFACE_VERSION,
      .ClipboardData = clipboard_data,
      .ClipboardLost = clipboard_lost
   };
   Failures += check(Driver->open(callbacks) IS ERR::Okay);
   if ((Driver->capabilities() & DCAP::CLIPBOARD) IS DCAP::NIL) Failures++;

   auto selection = XInternAtom(peer, "CLIPBOARD", False);
   auto utf8 = XInternAtom(peer, "UTF8_STRING", False);
   auto uris = XInternAtom(peer, "text/uri-list", False);
   auto gnome_files = XInternAtom(peer, "x-special/gnome-copied-files", False);
   std::string received;
   bool incremental = false;

   Failures += check(Driver->clipboardAddText("Kōtuku X11 clipboard ✓") IS ERR::Okay);
   Failures += check(request_selection(Driver, peer, window, selection, utf8, received, incremental));
   Failures += check(not incremental);
   Failures += check(received IS "Kōtuku X11 clipboard ✓");

   char directory_template[] = "/tmp/kotuku-x11-XXXXXX";
   if (auto directory = mkdtemp(directory_template)) {
      std::string first_path = std::string(directory) + "/Kotuku clipboard ✓.txt";
      std::string second_path = std::string(directory) + "/second.txt";
      std::vector<std::string> missing = { std::string(directory) + "/missing.txt" };
      Failures += check(Driver->clipboardAddFiles(CLIPTYPE::FILE, missing, true) IS ERR::InvalidPath);
      auto files_created = create_file(first_path.c_str()) and create_file(second_path.c_str());
      Failures += check(files_created);
      if (files_created) {
         std::vector<std::string> paths = { first_path, second_path };
         auto uri_root = std::string("file://") + directory;
         auto expected_uris = uri_root +
            "/Kotuku%20clipboard%20%E2%9C%93.txt\r\n" + uri_root + "/second.txt\r\n";
         auto expected_gnome = "cut\n" + uri_root +
            "/Kotuku%20clipboard%20%E2%9C%93.txt\n" + uri_root + "/second.txt\n";
         Failures += check(Driver->clipboardAddFiles(CLIPTYPE::FILE, paths, true) IS ERR::Okay);
         Failures += check(request_selection(Driver, peer, window, selection, uris, received, incremental));
         Failures += check(received IS expected_uris);
         Failures += check(request_selection(Driver, peer, window, selection, gnome_files, received, incremental));
         Failures += check(received IS expected_gnome);
      }
      unlink(first_path.c_str());
      unlink(second_path.c_str());
      rmdir(directory);
   }
   else Failures++;

   auto units = XExtendedMaxRequestSize(peer);
   if (not units) units = XMaxRequestSize(peer);
   auto direct_limit = (size_t(units) * 4) - 1024;
   if (direct_limit < 32 * 1024 * 1024) {
      std::string large(direct_limit + 4096, 'x');
      Failures += check(Driver->clipboardAddText(large.c_str()) IS ERR::Okay);
      Failures += check(request_selection(Driver, peer, window, selection, utf8, received, incremental));
      Failures += check(incremental);
      Failures += check(received IS large);
   }

   Failures += check(receive_external_selection(Driver, peer, utf8, "text/plain;charset=utf-8",
      "External X11 text ✓", false));
   Failures += check(receive_external_selection(Driver, peer, uris, "text/uri-list",
      "file:///tmp/external%20file.txt\r\n", false));
   std::string incoming_large(256 * 1024, 'i');
   Failures += check(receive_external_selection(Driver, peer, utf8, "text/plain;charset=utf-8",
      incoming_large, true));
   Failures += check(receive_external_selection(Driver, peer, utf8, "text/plain;charset=utf-8",
      "External text with incremental targets", false, true));
   Failures += check(replace_unresponsive_owner(Driver, peer, utf8));
   Failures += check(reject_stale_reply(Driver, peer, utf8));

   Failures += check(Driver->clipboardAddText("Persisted X11 clipboard ✓") IS ERR::Okay);
   Failures += check(save_with_clipboard_manager(Driver, peer, selection, utf8));
   XDestroyWindow(peer, window);
   XCloseDisplay(peer);
}

// A 32-bit BGRA memory bitmap for capture tests.  The extBitmap layout is private to the driver, so the storage is
// sized by the driver and only the public objBitmap fields that captureDisplay() reads are set.

struct CaptureBitmap {
   ColourFormat Format = {};
   std::vector<uint32_t> Pixels;
   objBitmap *Bitmap = nullptr;

   CaptureBitmap(int Width, int Height) : Pixels(size_t(Width) * Height, 0x12345678) {
      Bitmap = (objBitmap *)std::calloc(1, display::x11_driver_bitmap_size());
      Format.RedMask = Format.GreenMask = Format.BlueMask = Format.AlphaMask = 0xff;
      Format.BluePos = 0;
      Format.GreenPos = 8;
      Format.RedPos = 16;
      Format.AlphaPos = 24;
      Format.BitsPerPixel = 32;
      Bitmap->ColourFormat = &Format;
      Bitmap->Data = (uint8_t *)Pixels.data();
      Bitmap->Width = Width;
      Bitmap->Height = Height;
      Bitmap->LineWidth = Width * 4;
      Bitmap->ByteWidth = Width * 4;
      Bitmap->BytesPerPixel = 4;
      Bitmap->BitsPerPixel = 32;
      Bitmap->Clip.Right = Width;
      Bitmap->Clip.Bottom = Height;
   }

   ~CaptureBitmap() { std::free(Bitmap); }

   extBitmap * target() { return (extBitmap *)Bitmap; }
   uint32_t pixel(int X, int Y) const { return Pixels[size_t(Y) * Bitmap->Width + X]; }
};

// A server can advertise MIT-SHM without sharing the client's IPC namespace.  Probe an actual attachment on a
// separate connection so that a driver regression cannot silently skip the shared image tests.

static bool shared_images_available(Display *Connection)
{
   if ((not Connection) or (not XShmQueryExtension(Connection))) return false;

   XShmSegmentInfo shm = {};
   shm.shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT|IPC_EXCL|0600);
   if (shm.shmid IS -1) return false;
   shm.shmaddr = (char *)shmat(shm.shmid, nullptr, 0);
   if (shm.shmaddr IS (char *)-1) {
      shmctl(shm.shmid, IPC_RMID, nullptr);
      return false;
   }

   // The clipboard worker has already joined, so no other test thread uses Xlib while this handler is installed.

   XSync(Connection, False);
   glSharedImageAttachFailed = false;
   auto previous_handler = XSetErrorHandler([](Display *, XErrorEvent *) {
      glSharedImageAttachFailed = true;
      return 0;
   });
   const bool requested = XShmAttach(Connection, &shm) != 0;
   XSync(Connection, False);
   const bool attached = requested and (not glSharedImageAttachFailed);
   if (attached) {
      XShmDetach(Connection, &shm);
      XSync(Connection, False);
   }
   XSetErrorHandler(previous_handler);
   shmdt(shm.shmaddr);
   shmctl(shm.shmid, IPC_RMID, nullptr);
   return attached;
}

// Marking storage for deletion must not invalidate it while the bitmap is alive, but both normal cleanup and
// abrupt process death must release the segment.  Use a private X server via the existing CTest registration.

static void test_shared_images(DisplayDriver *Driver, const std::optional<std::string> &KotukuDisplay,
   const std::optional<std::string> &DisplayName, int &Failures)
{
   if ((not KotukuDisplay) and (not DisplayName)) return;
   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   if (Driver->open(callbacks) != ERR::Okay) { Failures++; return; }

   auto display_name = KotukuDisplay ? *KotukuDisplay : *DisplayName;
   auto peer = XOpenDisplay(display_name.c_str());
   const bool shared_images = shared_images_available(peer);
   if (peer) XCloseDisplay(peer);

   CaptureBitmap bitmap(32, 32);
   bitmap.Bitmap->Data = nullptr;
   bitmap.Bitmap->Size = 32 * 32 * 4;
   bitmap.Bitmap->MemType = BMT::DATA;
   bitmap.Bitmap->Type = BMP::CHUNKY;
   Failures += check(Driver->allocBitmap(bitmap.target()) IS ERR::Okay);
   int shmid = display::x11_driver_bitmap_shmid(bitmap.target());
   if (shmid IS -1) { // MIT-SHM may be unavailable, e.g. on a remote display.
      Failures += check(not shared_images);
      Failures += check(bitmap.Bitmap->Data IS nullptr); // BITMAP_Init supplies ordinary pixel memory on fallback.
      Driver->freeBitmap(bitmap.target());
      Driver->close();
      return;
   }

   shmid_ds status = {};
   Failures += check(shmctl(shmid, IPC_STAT, &status) IS 0);
   Failures += check((status.shm_perm.mode & SHM_DEST) != 0);
   bitmap.Bitmap->Data[0] = 42;

   // Failed resize leaves the original shared image and its pixels intact.

   Failures += check(Driver->resizeBitmap(bitmap.target(), 0, 0) != ERR::Okay);
   Failures += check(display::x11_driver_bitmap_shmid(bitmap.target()) IS shmid);
   Failures += check(bitmap.Bitmap->Data[0] IS 42);

   for (int width : { 64, 16, 48 }) {
      Failures += check(Driver->resizeBitmap(bitmap.target(), width, 32) IS ERR::Okay);
      Failures += check(shmctl(shmid, IPC_STAT, &status) IS -1);
      shmid = display::x11_driver_bitmap_shmid(bitmap.target());
      Failures += check(shmctl(shmid, IPC_STAT, &status) IS 0);
      Failures += check((status.shm_perm.mode & SHM_DEST) != 0);
      bitmap.Bitmap->Data[width * 32 * 4 - 1] = 23;
   }
   Driver->freeBitmap(bitmap.target());
   Failures += check(shmctl(shmid, IPC_STAT, &status) IS -1);
   Driver->close();

   for (bool resize : { false, true }) {
      int descriptors[2];
      if (pipe(descriptors) != 0) { Failures++; return; }
      auto child = fork();
      if (child IS 0) {
         close(descriptors[0]);
         int child_shmid = -1;
         if (Driver->open(callbacks) IS ERR::Okay) {
            bitmap.Bitmap->Data = nullptr;
            if (Driver->allocBitmap(bitmap.target()) IS ERR::Okay) {
               if ((not resize) or (Driver->resizeBitmap(bitmap.target(), 80, 64) IS ERR::Okay))
                  child_shmid = display::x11_driver_bitmap_shmid(bitmap.target());
            }
         }
         if (write(descriptors[1], &child_shmid, sizeof(child_shmid)) != sizeof(child_shmid)) _exit(1);
         close(descriptors[1]);
         for (;;) pause();
      }
      close(descriptors[1]);
      if (child IS -1) { close(descriptors[0]); Failures++; return; }
      int child_shmid = -1;
      Failures += check(read(descriptors[0], &child_shmid, sizeof(child_shmid)) IS sizeof(child_shmid));
      close(descriptors[0]);
      Failures += check(child_shmid != -1);
      kill(child, SIGKILL);
      int child_status = 0;
      waitpid(child, &child_status, 0);
      Failures += check(WIFSIGNALED(child_status) and (WTERMSIG(child_status) IS SIGKILL));
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while ((shmctl(child_shmid, IPC_STAT, &status) IS 0) and
            (std::chrono::steady_clock::now() < deadline)) usleep(1000);
      Failures += check((shmctl(child_shmid, IPC_STAT, &status) IS -1) and
         ((errno IS EINVAL) or (errno IS EIDRM)));
   }
}

static unsigned long alloc_colour(Display *Connection, uint16_t Red, uint16_t Green, uint16_t Blue)
{
   XColor colour = { .red = Red, .green = Green, .blue = Blue, .flags = DoRed|DoGreen|DoBlue };
   XAllocColor(Connection, DefaultColormap(Connection, DefaultScreen(Connection)), &colour);
   return colour.pixel;
}

// Draws a window that is red on its left half and blue on its right, then captures areas of the desktop that cross
// the boundary and the screen edges.  The window is override-redirect so that the driver, which takes the window
// manager role on a bare X server, does not intercept the map request.

static void test_capture(DisplayDriver *Driver, const std::optional<std::string> &KotukuDisplay,
   const std::optional<std::string> &DisplayName, int &Failures)
{
   auto display_name = KotukuDisplay ? *KotukuDisplay : (DisplayName ? *DisplayName : std::string());
   if (display_name.empty()) return;

   CaptureBitmap bitmap(4, 4);
   Failures += check(Driver->captureDisplay(0, 0, 1, 1, bitmap.target(), 0, 0) IS ERR::NotInitialised);

   auto peer = XOpenDisplay(display_name.c_str());
   if (not peer) { Failures++; return; }

   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   Failures += check(Driver->open(callbacks) IS ERR::Okay);

   const int win_x = 100, win_y = 100, win_width = 40, win_height = 20;
   XSetWindowAttributes attributes = {};
   attributes.override_redirect = True;
   attributes.event_mask = ExposureMask;
   auto window = XCreateWindow(peer, DefaultRootWindow(peer), win_x, win_y, win_width, win_height, 0,
      CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect|CWEventMask, &attributes);
   XMapRaised(peer, window);

   bool exposed = false;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while ((not exposed) and (std::chrono::steady_clock::now() < deadline)) {
      while (XPending(peer)) {
         XEvent event = {};
         XNextEvent(peer, &event);
         if ((event.type IS Expose) and (event.xexpose.window IS window)) exposed = true;
      }
      if (not exposed) usleep(1000);
   }
   Failures += check(exposed);

   auto gc = XCreateGC(peer, window, 0, nullptr);
   XSetForeground(peer, gc, alloc_colour(peer, 0xffff, 0, 0));
   XFillRectangle(peer, window, gc, 0, 0, win_width / 2, win_height);
   XSetForeground(peer, gc, alloc_colour(peer, 0, 0, 0xffff));
   XFillRectangle(peer, window, gc, win_width / 2, 0, win_width / 2, win_height);
   XSync(peer, False);

   auto error = Driver->captureDisplay(win_x + (win_width / 2) - 2, win_y + 5, 4, 2, bitmap.target(), 0, 1);
   if (error IS ERR::NoSupport) { // XWayland cannot read the desktop
      XFreeGC(peer, gc);
      XDestroyWindow(peer, window);
      XCloseDisplay(peer);
      Driver->close();
      return;
   }

   Failures += check(error IS ERR::Okay);
   for (int y=1; y < 3; y++) {
      for (int x=0; x < 4; x++) {
         Failures += check(bitmap.pixel(x, y) IS (x < 2 ? 0xffff0000u : 0xff0000ffu));
      }
   }
   Failures += check(bitmap.pixel(0, 0) IS 0x12345678u); // Rows outside the requested area are untouched
   Failures += check(bitmap.pixel(3, 3) IS 0x12345678u);

   // A request that crosses the top-left corner of the desktop is clipped, and its destination is offset to match.

   std::fill(bitmap.Pixels.begin(), bitmap.Pixels.end(), 0x12345678);
   Failures += check(Driver->captureDisplay(-2, -2, 4, 4, bitmap.target(), 0, 0) IS ERR::Okay);
   Failures += check(bitmap.pixel(1, 1) IS 0x12345678u);
   Failures += check(bitmap.pixel(2, 1) IS 0x12345678u);
   Failures += check(bitmap.pixel(1, 2) IS 0x12345678u);
   Failures += check((bitmap.pixel(2, 2) >> 24) IS 0xffu);
   Failures += check((bitmap.pixel(3, 3) >> 24) IS 0xffu);

   // The bottom-right corner is clipped in the same way.

   const int root_width = DisplayWidth(peer, DefaultScreen(peer));
   const int root_height = DisplayHeight(peer, DefaultScreen(peer));
   std::fill(bitmap.Pixels.begin(), bitmap.Pixels.end(), 0x12345678);
   Failures += check(Driver->captureDisplay(root_width - 2, root_height - 2, 4, 4, bitmap.target(), 0, 0) IS
      ERR::Okay);
   Failures += check((bitmap.pixel(1, 1) >> 24) IS 0xffu);
   Failures += check(bitmap.pixel(2, 1) IS 0x12345678u);
   Failures += check(bitmap.pixel(1, 2) IS 0x12345678u);

   Failures += check(Driver->captureDisplay(-10, -10, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange);
   Failures += check(Driver->captureDisplay(root_width, 0, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange);
   Failures += check(Driver->captureDisplay(0, root_height, 4, 4, bitmap.target(), 0, 0) IS ERR::OutOfRange);
   Failures += check(Driver->captureDisplay(0, 0, 4, 4, nullptr, 0, 0) IS ERR::NullArgs);
   Failures += check(Driver->captureDisplay(0, 0, 0, 4, bitmap.target(), 0, 0) IS ERR::NullArgs);

   XFreeGC(peer, gc);
   XDestroyWindow(peer, window);
   XCloseDisplay(peer);
   Driver->close();
}

//********************************************************************************************************************
// The driver has no windows here, so the position must come from the host rather than from pointer events.  It
// must also lie in the desktop area that displayInfo() reports.

static void test_pointer_position(DisplayDriver *Driver, const std::optional<std::string> &KotukuDisplay,
   const std::optional<std::string> &DisplayName, int &Failures)
{
   auto display_name = KotukuDisplay ? *KotukuDisplay : (DisplayName ? *DisplayName : std::string());
   if (display_name.empty()) return;

   double x = -1, y = -1;
   Failures += check(Driver->pointerPosition(x, y) IS ERR::NotInitialised);

   auto peer = XOpenDisplay(display_name.c_str());
   if (not peer) { Failures++; return; }

   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   Failures += check(Driver->open(callbacks) IS ERR::Okay);

   DisplayInfo info = {};
   Failures += check(Driver->displayInfo(info) IS ERR::Okay);

   for (auto [warp_x, warp_y] : { std::pair { 37, 23 }, std::pair { 211, 145 } }) {
      XWarpPointer(peer, None, DefaultRootWindow(peer), 0, 0, 0, 0, warp_x, warp_y);
      XSync(peer, False);
      Failures += check(Driver->pointerPosition(x, y) IS ERR::Okay);
      Failures += check((x IS warp_x) and (y IS warp_y));
      Failures += check((x >= info.VirtualX) and (x < info.VirtualX + info.VirtualWidth));
      Failures += check((y >= info.VirtualY) and (y < info.VirtualY + info.VirtualHeight));
   }

   XCloseDisplay(peer);
   Driver->close();
}

//********************************************************************************************************************

int main()
{
   const auto original_kotuku_display = environment_value("KOTUKU_XDISPLAY");
   const auto original_display = environment_value("DISPLAY");
   struct CoreBase core_table = {};
   core_table._RegisterFD = register_fd;
   core_table._ResolvePath = resolve_path;
   core_table._VLogF = vlogf;
   core_table._LogReturn = log_return;
   core_table._AdjustLogLevel = adjust_log_level;
   auto core = &core_table;
#ifdef KOTUKU_STATIC
   auto driver = display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#else
   auto driver = create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#endif
   int failures = 0;

   failures += check(driver != nullptr);
   failures += check(std::string(driver->name()) IS "x11");
   failures += check(driver->displayType() IS DT::X11);
   failures += check(driver->capabilities() IS DCAP::NIL);

   unsetenv("KOTUKU_XDISPLAY");
   unsetenv("DISPLAY");
   failures += check(driver->isAvailable() IS ERR::NoSupport);

   setenv("DISPLAY", ":91", 1);
   failures += check(driver->isAvailable() IS ERR::Okay);

   setenv("KOTUKU_XDISPLAY", ":92", 1);
   unsetenv("DISPLAY");
   failures += check(driver->isAvailable() IS ERR::Okay);

   DriverCallbacks wrong_callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION - 1 };
   failures += check(driver->open(wrong_callbacks) IS ERR::WrongVersion);
   failures += check(driver->close() IS ERR::Okay);

   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   failures += check(driver->open(callbacks) IS ERR::SystemCall);
   failures += check(driver->close() IS ERR::Okay);
   failures += check(driver->close() IS ERR::Okay);

   restore_environment("KOTUKU_XDISPLAY", original_kotuku_display);
   restore_environment("DISPLAY", original_display);
   test_clipboard(driver, original_kotuku_display, original_display, failures);
   test_capture(driver, original_kotuku_display, original_display, failures);
   test_shared_images(driver, original_kotuku_display, original_display, failures);
   test_pointer_position(driver, original_kotuku_display, original_display, failures);

#ifdef KOTUKU_STATIC
   display::destroy_x11_display_driver(driver);
   failures += check(display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#else
   destroy_display_driver(driver);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#endif

   return failures;
}
