#include "../drivers/x11/x11_driver.h"

#include <kotuku/modules/display.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

static std::string glClipboardData;
static std::string glClipboardMime;
static int glClipboardLost = 0;

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
         if ((event.type IS SelectionRequest) and (event.xselectionrequest.selection IS selection))
            request_seen = true;
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
         if ((event.type IS SelectionRequest) and (event.xselectionrequest.selection IS Selection)) {
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
                (event.xselectionrequest.target IS save_targets)) {
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

int main()
{
   const auto original_kotuku_display = environment_value("KOTUKU_XDISPLAY");
   const auto original_display = environment_value("DISPLAY");
   struct CoreBase core_table = {};
   core_table._RegisterFD = register_fd;
   core_table._ResolvePath = resolve_path;
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
