// XDND support for the X11 driver, as a drop target and as the source of exported drags.
//
// Kotuku acts as an XDND target for versions 3 to 5 of the protocol (https://freedesktop.org/wiki/Specifications/XDND).
// Files are accepted as text/uri-list and text as UTF8_STRING, text/plain;charset=utf-8, text/plain or STRING.  The
// operation policy is copy-only: any requested action is answered with XdndActionCopy, which the protocol permits a
// target to choose in all cases, and a drag is refused while Shift or Alt is held because those request a move, link
// or choice of action.  This is the policy applied by the Windows and Wayland backends.
//
// Hover feedback, destination snapshots, modality and receipt delivery are delegated to the common DragOver,
// DragStart, DragData and DragLeave callbacks.  Selection data is read through a private requestor window per drop,
// so the CLIPBOARD and PRIMARY transfers and their published data are unaffected.
//
// As a source, the driver exports an internal drag that the Display module hands over with startDragExport() once
// the pointer leaves our windows.  Only XdndActionCopy is offered, and a recipient that answers with any other action
// is treated as refusing.  The internal session keeps following the pointer, so the source is silent while the
// pointer is over one of our own windows.  XdndSelection is owned by a private window and served from an immutable
// copy of the payload, which is retained until XdndFinished or a timeout.

#include "x11_native.h"
#include "../../driver/clipboard_utils.h"

#include <X11/Xatom.h>

#include <algorithm>
#include <optional>

namespace display {

constexpr int XDND_VERSION = 5;
constexpr int XDND_MIN_VERSION = 3;
constexpr size_t MAX_DROP_TRANSFER = 64 * 1024 * 1024;
constexpr long MAX_DROP_TYPES = 256;

enum class XFER { PENDING, COMPLETE, FAILED };

//********************************************************************************************************************

ERR x11_xdnd_open(X11Driver::State *State)
{
   char *names[] = {
      (char *)"XdndAware", (char *)"XdndEnter", (char *)"XdndPosition", (char *)"XdndStatus", (char *)"XdndLeave",
      (char *)"XdndDrop", (char *)"XdndFinished", (char *)"XdndSelection", (char *)"XdndTypeList",
      (char *)"XdndActionCopy", (char *)"KOTUKU_XDND", (char *)"XdndProxy"
   };
   Atom atoms[std::size(names)] = {};
   if (not XInternAtoms(State->Connection, names, int(std::size(names)), False, atoms)) return ERR::SystemCall;
   auto &xdnd = State->Xdnd;
   xdnd.Aware = atoms[0];
   xdnd.Enter = atoms[1];
   xdnd.Position = atoms[2];
   xdnd.StatusReply = atoms[3];
   xdnd.Leave = atoms[4];
   xdnd.Drop = atoms[5];
   xdnd.Finished = atoms[6];
   xdnd.Selection = atoms[7];
   xdnd.TypeList = atoms[8];
   xdnd.ActionCopy = atoms[9];
   xdnd.Property = atoms[10];
   xdnd.Proxy = atoms[11];
   State->RefusedCursor = XCreateFontCursor(State->Connection, XC_circle);
   return ERR::Okay;
}

//********************************************************************************************************************
// Sources look for XdndAware on top-level windows.  Adopted windows are not advertised, because the host application
// owns the top-level window and its drag and drop registration.

void x11_xdnd_advertise(X11Driver::State *State, Window Native)
{
   if ((not State->Xdnd.Aware) or (not Native)) return;
   const Atom version = XDND_VERSION;
   XChangeProperty(State->Connection, Native, State->Xdnd.Aware, XA_ATOM, 32, PropModeReplace,
      (const unsigned char *)&version, 1);
}

//********************************************************************************************************************
// The native lock must be held by the caller of these functions.

static void send_message(X11Driver::State *State, Window Destination, Atom Type, std::array<long, 5> Data)
{
   if (not Destination) return;
   XEvent event = {};
   event.xclient.type = ClientMessage;
   event.xclient.display = State->Connection;
   event.xclient.window = Destination;
   event.xclient.message_type = Type;
   event.xclient.format = 32;
   for (size_t i=0; i < Data.size(); i++) event.xclient.data.l[i] = Data[i];
   XSendEvent(State->Connection, Destination, False, NoEventMask, &event);
   XFlush(State->Connection);
}

// Bit 1 of the flags asks for a position message on every movement, because acceptance varies between widgets.

static void send_status(X11Driver::State *State, Window Source, Window Target, bool Accepted)
{
   send_message(State, Source, State->Xdnd.StatusReply, { long(Target), Accepted ? 3L : 2L, 0, 0,
      Accepted ? long(State->Xdnd.ActionCopy) : long(None) });
}

// The result fields of XdndFinished were introduced in version 5 and are reserved in earlier versions.

static void send_finished(X11Driver::State *State, Window Source, Window Target, int Version, bool Accepted)
{
   const bool result = Version >= 5;
   send_message(State, Source, State->Xdnd.Finished, { long(Target), (result and Accepted) ? 1L : 0L,
      (result and Accepted) ? long(State->Xdnd.ActionCopy) : long(None), 0, 0 });
}

//********************************************************************************************************************
// Source windows are monitored for destruction so that a source that terminates without XdndLeave or a selection
// reply does not leave hover feedback or a transfer behind.  Windows belonging to this driver are never selected,
// because doing so would replace their event masks.

static void watch_source(X11Driver::State *State, Window Source)
{
   if ((not Source) or State->DropWatches.contains(Source) or State->Windows.contains(Source) or
         (Source IS State->ClipboardWindow)) return;
   XWindowAttributes attributes = {};
   if (not XGetWindowAttributes(State->Connection, Source, &attributes)) return;
   State->DropWatches.emplace(Source, attributes.your_event_mask);
   XSelectInput(State->Connection, Source, attributes.your_event_mask|StructureNotifyMask);

   // Clipboard writes restore their saved mask when they finish, which must retain our destruction watch.
   for (auto &write : State->ClipboardWrites) {
      if (write.Requestor IS Source) write.PreviousEventMask |= StructureNotifyMask;
   }
}

static void unwatch_source(X11Driver::State *State, Window Source)
{
   auto watch = State->DropWatches.find(Source);
   if (watch IS State->DropWatches.end()) return;
   if (State->DropSession.Source IS Source) return;
   for (auto &transfer : State->DropTransfers) if (transfer.Source IS Source) return;

   // Remove only the event bit we added.  A clipboard write may have started or finished during the drag.
   if (not (watch->second & StructureNotifyMask)) {
      XWindowAttributes attributes = {};
      if (XGetWindowAttributes(State->Connection, Source, &attributes))
         XSelectInput(State->Connection, Source, attributes.your_event_mask & ~StructureNotifyMask);
      for (auto &write : State->ClipboardWrites) {
         if (write.Requestor IS Source) write.PreviousEventMask &= ~StructureNotifyMask;
      }
   }
   State->DropWatches.erase(watch);
}

//********************************************************************************************************************
// Shift and Alt request a move, link or choice of action from the source.  The pointer state reports the modifiers
// while the source holds the pointer grab, unlike the key events tracked by the driver.

static bool copy_modifiers_allowed(X11Driver::State *State)
{
   Window root, child;
   int root_x, root_y, x, y;
   unsigned mask = 0;
   if (not XQueryPointer(State->Connection, DefaultRootWindow(State->Connection), &root, &child, &root_x, &root_y,
         &x, &y, &mask)) return false;
   return not (mask & (ShiftMask|Mod1Mask));
}

//********************************************************************************************************************

static Atom preferred_type(X11Driver::State *State, const std::vector<Atom> &Types)
{
   for (auto preference : { State->URIListAtom, State->UTF8StringAtom, State->TextUTF8Atom, State->TextPlainAtom,
         Atom(XA_STRING) }) {
      if (std::find(Types.begin(), Types.end(), preference) != Types.end()) return preference;
   }
   return None;
}

static char drop_datatype(X11Driver::State *State, Atom Type)
{
   return Type IS State->URIListAtom ? char(DATA::FILE) : char(DATA::TEXT);
}

//********************************************************************************************************************
// Ends hover feedback for the current session.  Transfers already started by a drop are unaffected.

static void end_hover(X11Driver::State *State, bool Crossing)
{
   X11DropSession session;
   {
      const std::lock_guard lock(State->NativeLock);
      session = State->DropSession;
      State->DropSession = {};
      unwatch_source(State, session.Source);
   }
   if (not session.Source) return;

   auto callbacks = State->Callbacks;
   if (callbacks and callbacks->DragLeave) callbacks->DragLeave();
   if (Crossing and session.Entered and callbacks and callbacks->Crossing)
      callbacks->Crossing(session.SurfaceID, false, session.RootX, session.RootY);
}

//********************************************************************************************************************

static void handle_enter(X11Driver::State *State, const XClientMessageEvent &Event)
{
   const auto source = Window(Event.data.l[0]);
   const int version = int((unsigned long)Event.data.l[1] >> 24) & 0xff;

   // A new session supersedes one that was never closed, for example by a source that failed to send XdndLeave.

   if (State->DropSession.Source) end_hover(State, true);

   Window native = 0;
   OBJECTID surface_id = 0;
   {
      const std::lock_guard lock(State->NativeLock);
      auto it = State->Windows.find(Event.window);
      if ((it IS State->Windows.end()) or (not it->second->Owned) or it->second->Root or it->second->Adopted) return;
      if ((not source) or (version < XDND_MIN_VERSION) or (version > XDND_VERSION)) return;
      native = it->second->Native;
      surface_id = it->second->SurfaceID;
   }

   auto callbacks = State->Callbacks;
   if ((not surface_id) and callbacks and callbacks->ResolveSurface)
      surface_id = callbacks->ResolveSurface((APTR)(uintptr_t)native);
   if (not surface_id) return;

   const std::lock_guard lock(State->NativeLock);
   std::vector<Atom> types;
   if (Event.data.l[1] & 1) {
      Atom type = None;
      int format = 0;
      unsigned long count = 0, remaining = 0;
      unsigned char *property = nullptr;
      if ((XGetWindowProperty(State->Connection, source, State->Xdnd.TypeList, 0, MAX_DROP_TYPES, False, XA_ATOM,
            &type, &format, &count, &remaining, &property) IS Success) and (type IS XA_ATOM) and (format IS 32)) {
         auto list = (const Atom *)property;
         types.assign(list, list + count);
      }
      if (property) XFree(property);
   }
   else for (int i=2; i < 5; i++) if (Event.data.l[i]) types.push_back(Atom(Event.data.l[i]));

   auto &session = State->DropSession;
   session = {};
   session.Source = source;
   session.Target = Event.window;
   session.SurfaceID = surface_id;
   session.Type = preferred_type(State, types);
   session.Version = version;
   session.Generation = ++State->DropGeneration;
   watch_source(State, source);
}

//********************************************************************************************************************
// Every XdndPosition must be answered with XdndStatus, because the source waits for it before sending more.

static void handle_position(X11Driver::State *State, const XClientMessageEvent &Event)
{
   const auto source = Window(Event.data.l[0]);
   const auto target = Event.window;
   const int root_x = int((unsigned long)Event.data.l[2] >> 16) & 0xffff;
   const int root_y = int((unsigned long)Event.data.l[2]) & 0xffff;

   OBJECTID surface_id;
   Atom type;
   double x, y;
   bool eligible, first;
   uint64_t generation;

   {
      const std::lock_guard lock(State->NativeLock);
      auto &session = State->DropSession;
      if ((not session.Source) or (session.Source != source) or (session.Target != target)) {
         send_status(State, source, target, false);
         return;
      }

      int window_x = 0, window_y = 0;
      Window child, root;
      int geometry_x, geometry_y;
      unsigned width = 0, height = 0, border, depth;
      bool inside = XTranslateCoordinates(State->Connection, DefaultRootWindow(State->Connection), target, root_x,
         root_y, &window_x, &window_y, &child) and
         XGetGeometry(State->Connection, target, &root, &geometry_x, &geometry_y, &width, &height, &border, &depth) and
         (window_x >= 0) and (window_y >= 0) and (window_x < int(width)) and (window_y < int(height));

      session.X = window_x;
      session.Y = window_y;
      session.RootX = root_x;
      session.RootY = root_y;
      eligible = inside and session.Type and copy_modifiers_allowed(State);
      first = not session.Entered;
      session.Entered = true;
      surface_id = session.SurfaceID;
      type = session.Type;
      x = session.X;
      y = session.Y;
      generation = session.Generation;
   }

   auto callbacks = State->Callbacks;
   if (first and callbacks and callbacks->Crossing) callbacks->Crossing(surface_id, true, root_x, root_y);

   bool accepted = false;
   if (eligible and callbacks and callbacks->DragOver) {
      const char datatype = drop_datatype(State, type);
      accepted = callbacks->DragOver(surface_id, x, y, &datatype, 1);
   }
   if ((not accepted) and callbacks and callbacks->DragLeave) callbacks->DragLeave();

   // Widget callbacks can destroy the target window or process events that end the session.

   const std::lock_guard lock(State->NativeLock);
   if (State->DropSession.Generation IS generation) State->DropSession.Accepted = accepted;
   else accepted = false;
   send_status(State, source, target, accepted);
}

//********************************************************************************************************************

static void handle_leave(X11Driver::State *State, const XClientMessageEvent &Event)
{
   {
      const std::lock_guard lock(State->NativeLock);
      auto &session = State->DropSession;
      if ((not session.Source) or (session.Source != Window(Event.data.l[0])) or (session.Target != Event.window))
         return;
   }
   end_hover(State, true);
}

//********************************************************************************************************************
// A task-owned timer wakes the message loop even when neither the source nor the rest of the application is active.
// The timer stops with the last transfer and follows the earliest deadline after incremental progress.

static ERR expire_drops(OBJECTPTR, int64_t, int64_t, APTR Meta)
{
   auto state = (X11Driver::State *)Meta;
   x11_xdnd_expire(state);

   const std::lock_guard lock(state->NativeLock);
   if (state->DropTransfers.empty()) {
      state->DropTimer = nullptr;
      return ERR::Terminate;
   }
   auto deadline = state->DropTransfers.front().Updated + state->DropTimeout;
   for (auto &transfer : state->DropTransfers)
      deadline = std::min(deadline, transfer.Updated + state->DropTimeout);
   const auto delay = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
   return UpdateTimer(state->DropTimer, std::max(0.001, delay));
}

//********************************************************************************************************************
// Requests the dropped data.  The caller holds the native lock.

static bool begin_transfer(X11Driver::State *State, const X11DropSession &Session, NativeDrop &Drop, Time Timestamp)
{
   XSetWindowAttributes attributes = {};
   attributes.event_mask = PropertyChangeMask;
   X11DropTransfer transfer;
   transfer.Requestor = XCreateWindow(State->Connection, DefaultRootWindow(State->Connection), -10, -10, 1, 1, 0, 0,
      InputOnly, CopyFromParent, CWEventMask, &attributes);
   if (not transfer.Requestor) return false;

   if (not State->DropTimer) {
      kt::SwitchContext context(CurrentTask());
      const auto delay = std::max(0.001, std::chrono::duration<double>(State->DropTimeout).count());
      if (SubscribeTimer(delay, C_FUNCTION(expire_drops, State), &State->DropTimer) != ERR::Okay) {
         XDestroyWindow(State->Connection, transfer.Requestor);
         return false;
      }
   }

   transfer.Source = Session.Source;
   transfer.Target = Session.Target;
   transfer.Type = Session.Type;
   transfer.Version = Session.Version;
   transfer.Updated = std::chrono::steady_clock::now();
   transfer.Drop = Drop;
   XConvertSelection(State->Connection, State->Xdnd.Selection, Session.Type, State->Xdnd.Property,
      transfer.Requestor, Timestamp);
   XFlush(State->Connection);
   State->DropTransfers.push_back(std::move(transfer));
   watch_source(State, Session.Source);
   return true;
}

//********************************************************************************************************************
// The destination is captured by DragStart() when the drop is received, so later pointer movement, layout changes
// and navigation cannot redirect the data while it is being read.

static void handle_drop(X11Driver::State *State, const XClientMessageEvent &Event)
{
   const auto source = Window(Event.data.l[0]);
   const auto target = Event.window;
   const auto timestamp = Event.data.l[2] ? Time(Event.data.l[2]) : CurrentTime;

   X11DropSession session;
   bool eligible;
   uint64_t drop_session;
   {
      const std::lock_guard lock(State->NativeLock);
      if ((not State->DropSession.Source) or (State->DropSession.Source != source) or
            (State->DropSession.Target != target)) {
         send_finished(State, source, target, XDND_VERSION, false);
         return;
      }
      session = State->DropSession;
      State->DropSession = {};
      eligible = session.Accepted and session.Type and copy_modifiers_allowed(State);
      drop_session = ++State->DropGeneration;
   }

   auto callbacks = State->Callbacks;
   bool started = false;
   if (eligible and callbacks and callbacks->DragStart) {
      NativeDrop drop;
      drop.SurfaceID = session.SurfaceID;
      drop.X = session.X;
      drop.Y = session.Y;
      drop.Session = drop_session;
      if (callbacks->DragStart(drop)) {
         const std::lock_guard lock(State->NativeLock);
         started = begin_transfer(State, session, drop, timestamp);
      }
   }

   {
      const std::lock_guard lock(State->NativeLock);
      if (not started) send_finished(State, source, target, session.Version, false);
      unwatch_source(State, source);
   }

   if (callbacks and callbacks->DragLeave) callbacks->DragLeave();
}

//********************************************************************************************************************

static void handle_export_status(X11Driver::State *State, const XClientMessageEvent &Event);
static void handle_export_finished(X11Driver::State *State, const XClientMessageEvent &Event);

bool x11_xdnd_client_message(X11Driver::State *State, const XClientMessageEvent &Event)
{
   const auto &xdnd = State->Xdnd;
   const auto type = Event.message_type;

   if ((type) and ((type IS xdnd.StatusReply) or (type IS xdnd.Finished))) {
      if ((not State->ExportWindow) or (Event.window != State->ExportWindow)) return false;
      if (Event.format != 32) return true;
      if (type IS xdnd.StatusReply) handle_export_status(State, Event);
      else handle_export_finished(State, Event);
      return true;
   }

   if ((not type) or ((type != xdnd.Enter) and (type != xdnd.Position) and (type != xdnd.Leave) and
         (type != xdnd.Drop))) return false;
   if (Event.format != 32) return true;

   if (type IS xdnd.Enter) handle_enter(State, Event);
   else if (type IS xdnd.Position) handle_position(State, Event);
   else if (type IS xdnd.Leave) handle_leave(State, Event);
   else handle_drop(State, Event);
   return true;
}

//********************************************************************************************************************
// Reads the transfer property, deleting it so that an incremental source sends its next chunk.  The caller holds
// the native lock.

static XFER read_transfer(X11DropTransfer &Transfer, X11Driver::State *State, bool Initial)
{
   Atom type = None;
   int format = 0;
   unsigned long count = 0, remaining = 0;
   unsigned char *property = nullptr;
   auto status = XGetWindowProperty(State->Connection, Transfer.Requestor, State->Xdnd.Property, 0,
      long(MAX_DROP_TRANSFER / 4), True, AnyPropertyType, &type, &format, &count, &remaining, &property);
   const auto release = kt::Defer([&]() { if (property) XFree(property); });

   if ((status != Success) or remaining or (type IS None)) return XFER::FAILED;

   Transfer.Updated = std::chrono::steady_clock::now();
   if (Initial and (type IS State->IncrAtom)) {
      if ((format != 32) or (count != 1) or (*(const unsigned long *)property > MAX_DROP_TRANSFER))
         return XFER::FAILED;
      Transfer.Incremental = true;
      return XFER::PENDING;
   }

   if ((format != 8) or (count > MAX_DROP_TRANSFER - Transfer.Data.size())) return XFER::FAILED;
   if (count) Transfer.Data.append((const char *)property, count);
   if (Transfer.Incremental and count) return XFER::PENDING;
   return XFER::COMPLETE;
}

//********************************************************************************************************************
// Removes a transfer that has completed or failed.  Failed transfers answer the source immediately; completed
// transfers answer it after delivery.  The returned record is released by the caller after the native lock, which
// can free the destination snapshot.

static X11DropTransfer remove_transfer(X11Driver::State *State, std::vector<X11DropTransfer>::iterator Transfer,
   bool Failed)
{
   auto transfer = std::move(*Transfer);
   State->DropTransfers.erase(Transfer);

   if (State->DropTransfers.empty() and State->DropTimer) {
      UpdateTimer(State->DropTimer, 0);
      State->DropTimer = nullptr;
   }

   if (Failed) send_finished(State, transfer.Source, transfer.Target, transfer.Version, false);
   XDestroyWindow(State->Connection, transfer.Requestor);
   unwatch_source(State, transfer.Source);
   return transfer;
}

static void process_transfer(X11Driver::State *State, Window Requestor, bool Initial, bool Refused)
{
   std::optional<X11DropTransfer> finished;
   bool deliver = false;

   {
      const std::lock_guard lock(State->NativeLock);
      auto it = std::find_if(State->DropTransfers.begin(), State->DropTransfers.end(),
         [Requestor](const X11DropTransfer &Transfer) { 
            return Transfer.Requestor IS Requestor; 
         });

      if (it IS State->DropTransfers.end()) return;
      if ((not Initial) and (not it->Incremental)) return;

      auto result = Refused ? XFER::FAILED : read_transfer(*it, State, Initial);
      if (result IS XFER::PENDING) return;
      deliver = result IS XFER::COMPLETE;
      finished = remove_transfer(State, it, not deliver);
   }

   if (not deliver) return;

   auto &transfer = *finished;
   std::string data = transfer.Type IS XA_STRING ?
      latin1_to_utf8((const unsigned char *)transfer.Data.data(), transfer.Data.size()) : std::move(transfer.Data);
   const auto mime = transfer.Type IS State->URIListAtom ? "text/uri-list" : "text/plain;charset=utf-8";

   bool accepted = false;
   auto callbacks = State->Callbacks;
   if (callbacks and callbacks->DragData) accepted = callbacks->DragData(transfer.Drop, mime, data.data(), data.size());

   const std::lock_guard lock(State->NativeLock);
   if (State->Connection) send_finished(State, transfer.Source, transfer.Target, transfer.Version, accepted);
}

//********************************************************************************************************************

bool x11_xdnd_selection_notify(X11Driver::State *State, const XSelectionEvent &Event)
{
   if ((not State->Xdnd.Selection) or (Event.selection != State->Xdnd.Selection)) return false;

   {
      const std::lock_guard lock(State->NativeLock);
      auto it = std::find_if(State->DropTransfers.begin(), State->DropTransfers.end(),
         [&Event](const X11DropTransfer &Transfer) { return Transfer.Requestor IS Event.requestor; });
      if ((it IS State->DropTransfers.end()) or (Event.target != it->Type) or it->Incremental) return true;
   }

   process_transfer(State, Event.requestor, true, Event.property IS None);
   return true;
}

//********************************************************************************************************************
// Incremental chunks are announced by PropertyNewValue.  The initial notification of a direct reply is ignored here
// because its SelectionNotify event follows.

bool x11_xdnd_property_notify(X11Driver::State *State, const XPropertyEvent &Event)
{
   if ((Event.atom != State->Xdnd.Property) or (Event.state != PropertyNewValue)) return false;
   process_transfer(State, Event.window, false, false);
   return true;
}

//********************************************************************************************************************
// Structural events for monitored source windows are consumed here.  Property events still belong to the clipboard
// machinery when the same foreign window requests an incremental clipboard transfer.

bool x11_xdnd_foreign_event(X11Driver::State *State, const XEvent &Event)
{
   std::vector<X11DropTransfer> abandoned;
   bool hover = false;

   {
      const std::lock_guard lock(State->NativeLock);

      if (State->DropWatches.empty()) return false;

      const auto window = Event.type IS DestroyNotify ? Event.xdestroywindow.window : Event.xany.window;

      if (not State->DropWatches.contains(window)) return false;
      if (Event.type != DestroyNotify) return Event.type != PropertyNotify;

      State->DropWatches.erase(window);
      for (auto it = State->DropTransfers.begin(); it != State->DropTransfers.end();) {
         if (it->Source != window) { it++; continue; }
         XDestroyWindow(State->Connection, it->Requestor);
         abandoned.push_back(std::move(*it));
         it = State->DropTransfers.erase(it);
      }

      if (State->DropTransfers.empty() and State->DropTimer) {
         UpdateTimer(State->DropTimer, 0);
         State->DropTimer = nullptr;
      }

      hover = State->DropSession.Source IS window;
   }

   if (hover) end_hover(State, true);
   return true;
}

//********************************************************************************************************************
// A destroyed target window ends hover feedback.  Transfers already started are unaffected because their snapshot
// rejects delivery to a destroyed destination.

void x11_xdnd_forget_window(X11Driver::State *State, Window Native)
{
   {
      const std::lock_guard lock(State->NativeLock);
      if ((not State->DropSession.Source) or (State->DropSession.Target != Native)) return;
   }

   end_hover(State, false);
}

//********************************************************************************************************************
// The protocol has no transfer timeout, so a source that stops answering would otherwise retain the transfer and its
// snapshot indefinitely.  The timeout is measured from the latest progress.

static void expire_export(X11Driver::State *State);

void x11_xdnd_expire(X11Driver::State *State)
{
   State->LocateCache.Valid = false; // Windows may have changed since the previous pass of the event loop
   expire_export(State);

   std::vector<X11DropTransfer> expired;
   const std::lock_guard lock(State->NativeLock);

   if (State->DropTransfers.empty()) return;

   const auto now = std::chrono::steady_clock::now();
   for (auto it = State->DropTransfers.begin(); it != State->DropTransfers.end();) {
      if (now - it->Updated < State->DropTimeout) { it++; continue; }
      kt::Log("X11Mgr").trace("Abandoning an XDND transfer from window %lu.", it->Source);
      expired.push_back(remove_transfer(State, it, true));
      it = State->DropTransfers.begin();
   }
}

//********************************************************************************************************************
// Refuses outstanding transfers so that their sources are not left waiting.  Callbacks are not invoked, because the
// driver is closing.

void x11_xdnd_close(X11Driver::State *State)
{
   std::vector<X11DropTransfer> transfers;
   {
      const std::lock_guard lock(State->NativeLock);

      if (State->DropTimer) {
         UpdateTimer(State->DropTimer, 0);
         State->DropTimer = nullptr;
      }

      if (State->Connection) {
         for (auto &transfer : State->DropTransfers) {
            send_finished(State, transfer.Source, transfer.Target, transfer.Version, false);
            XDestroyWindow(State->Connection, transfer.Requestor);
         }
      }

      transfers = std::move(State->DropTransfers);
      State->DropTransfers.clear();
      State->DropSession = {};
      while (not State->DropWatches.empty()) unwatch_source(State, State->DropWatches.begin()->first);

      // An export is abandoned without a DragExportEnded notification.  A recipient that is still hovering is told
      // to leave; one that has the drop will fail to read the selection.

      if (State->Connection) {
         if (State->Export.Active and State->Export.Over.Target) {
            send_message(State, State->Export.Over.Proxy, State->Xdnd.Leave, { long(State->ExportWindow), 0, 0, 0,
               0 });
         }
         if (State->ExportWindow) XDestroyWindow(State->Connection, State->ExportWindow);
         if (State->RefusedCursor) XFreeCursor(State->Connection, State->RefusedCursor);
      }
      if (State->ExportTimer) UpdateTimer(State->ExportTimer, 0);
      State->ExportTimer = nullptr;
      State->ExportWindow = 0;
      State->RefusedCursor = None;
      State->Export = {};
      State->LocateCache = {};
      State->Xdnd = {};
   }
}

//********************************************************************************************************************
// Outbound XDND.  Hit-testing descends from the top-level window under the pointer until it finds one of our windows
// or a window that is XdndAware, directly or through an XdndProxy.  The caller holds the native lock.

static bool read_aware_version(X11Driver::State *State, Window Window, int &Version)
{
   Atom type = None;
   int format = 0;
   unsigned long count = 0, remaining = 0;
   unsigned char *value = nullptr;
   const bool aware = (XGetWindowProperty(State->Connection, Window, State->Xdnd.Aware, 0, 1, False, XA_ATOM, &type,
      &format, &count, &remaining, &value) IS Success) and (type IS XA_ATOM) and (format IS 32) and (count IS 1) and
      value;
   if (aware) Version = int(*(const Atom *)value);
   if (value) XFree(value);
   return aware;
}

// The proxy is valid only if its own XdndProxy property refers to itself.

static Window read_proxy(X11Driver::State *State, Window Window)
{
   auto read = [State](::Window Holder) -> ::Window {
      Atom type = None;
      int format = 0;
      unsigned long count = 0, remaining = 0;
      unsigned char *value = nullptr;
      ::Window result = 0;
      if ((XGetWindowProperty(State->Connection, Holder, State->Xdnd.Proxy, 0, 1, False, XA_WINDOW, &type, &format,
            &count, &remaining, &value) IS Success) and (type IS XA_WINDOW) and (format IS 32) and (count IS 1) and
            value) result = ::Window(*(const unsigned long *)value);
      if (value) XFree(value);
      return result;
   };

   auto proxy = read(Window);
   if (proxy and (read(proxy) IS proxy)) return proxy;
   return 0;
}

// Returns true if Window is the recipient of drops at its position.  A recipient with an unsupported version ends the
// search without a target.

static bool read_recipient(X11Driver::State *State, Window Window, X11DropWindow &Result)
{
   auto proxy = read_proxy(State, Window);
   auto holder = proxy ? proxy : Window;
   int version = 0;
   if (not read_aware_version(State, holder, version)) return false;
   if (version >= XDND_MIN_VERSION) {
      Result.Target = Window;
      Result.Proxy = holder;
      Result.Version = std::min(version, XDND_VERSION);
   }
   return true;
}

// Returns the topmost viewable child of the root window at the position.  Skip is passed over, which requires the
// stacking order to be read; otherwise the server resolves the window directly.

static Window top_level_at(X11Driver::State *State, int X, int Y, Window Skip)
{
   auto root = DefaultRootWindow(State->Connection);
   if (not Skip) {
      Window child = 0;
      int x, y;
      if (not XTranslateCoordinates(State->Connection, root, root, X, Y, &x, &y, &child)) return 0;
      return child;
   }

   Window root_return, parent, *children = nullptr;
   unsigned count = 0;
   if (not XQueryTree(State->Connection, root, &root_return, &parent, &children, &count)) return 0;
   Window found = 0;
   for (unsigned i=count; (i > 0) and (not found); i--) {
      auto window = children[i - 1];
      if (window IS Skip) continue;
      XWindowAttributes attributes = {};
      if ((not XGetWindowAttributes(State->Connection, window, &attributes)) or
          (attributes.map_state != IsViewable)) continue;
      const int border = attributes.border_width * 2;
      if ((X >= attributes.x) and (Y >= attributes.y) and (X < attributes.x + attributes.width + border) and
          (Y < attributes.y + attributes.height + border)) found = window;
   }
   if (children) XFree(children);
   return found;
}

X11DropWindow x11_xdnd_locate(X11Driver::State *State, int X, int Y, Window Exclude)
{
   const std::lock_guard lock(State->NativeLock);
   auto &cache = State->LocateCache;
   if (cache.Valid and (cache.X IS X) and (cache.Y IS Y) and (cache.Exclude IS Exclude)) return cache.Result;

   X11DropWindow result;
   if ((not State->Connection) or (not State->Xdnd.Aware)) return result;

   auto root = DefaultRootWindow(State->Connection);
   Window top = 0, skip = 0;
   for (int attempt=0; attempt < 2; attempt++) {
      top = top_level_at(State, X, Y, skip);
      bool excluded = false;
      result = {};
      for (auto window = top; window; ) {
         if (Exclude and (window IS Exclude)) { excluded = true; break; }
         if (State->Windows.contains(window)) { result.Own = true; break; }
         if (read_recipient(State, window, result)) break;
         Window child = 0;
         int x, y;
         if (not XTranslateCoordinates(State->Connection, root, window, X, Y, &x, &y, &child)) break;
         window = child;
      }
      if ((not excluded) or (not top)) break;
      skip = top;
      result = {};
   }

   // The desktop can be served through a proxy on the root window when no top-level window covers the position.
   if ((not top) and (not result.Own)) read_recipient(State, root, result);

   cache = { .X = X, .Y = Y, .Exclude = Exclude, .Result = result, .Valid = true };
   return result;
}

//********************************************************************************************************************
// Source messages carry the window under the pointer, but are delivered to its proxy.  The caller holds the native
// lock.

static void send_to_recipient(X11Driver::State *State, Atom Type, std::array<long, 5> Data)
{
   auto &over = State->Export.Over;
   if (not over.Proxy) return;
   XEvent event = {};
   event.xclient.type = ClientMessage;
   event.xclient.display = State->Connection;
   event.xclient.window = over.Target;
   event.xclient.message_type = Type;
   event.xclient.format = 32;
   for (size_t i=0; i < Data.size(); i++) event.xclient.data.l[i] = Data[i];
   XSendEvent(State->Connection, over.Proxy, False, NoEventMask, &event);
}

static Time event_time(X11Driver::State *State)
{
   return State->LastServerTimestamp ? State->LastServerTimestamp : CurrentTime;
}

static void send_enter(X11Driver::State *State)
{
   auto &types = State->Export.Types;
   std::array<long, 5> data = { long(State->ExportWindow), long(State->Export.Over.Version) << 24, 0, 0, 0 };
   if (types.size() > 3) data[1] |= 1;
   else for (size_t i=0; i < types.size(); i++) data[2 + i] = long(types[i]);
   send_to_recipient(State, State->Xdnd.Enter, data);
}

static void send_position(X11Driver::State *State)
{
   auto &e = State->Export;
   send_to_recipient(State, State->Xdnd.Position, { long(State->ExportWindow), 0,
      (long(e.RootX & 0xffff) << 16) | long(e.RootY & 0xffff), long(event_time(State)),
      long(State->Xdnd.ActionCopy) });
   e.WaitingStatus = true;
   e.PositionPending = false;
}

static void send_leave(X11Driver::State *State)
{
   send_to_recipient(State, State->Xdnd.Leave, { long(State->ExportWindow), 0, 0, 0, 0 });
}

static void send_drop(X11Driver::State *State)
{
   send_to_recipient(State, State->Xdnd.Drop, { long(State->ExportWindow), 0, long(event_time(State)), 0, 0 });
}

// The grab cursor shows whether a recipient in another application accepts the drop.  Over our own windows, the
// grab window's cursor is restored so that widget feedback remains visible.  Changing the cursor of the implicit
// button grab keeps its pointer event selection.

static void update_cursor(X11Driver::State *State)
{
   auto &e = State->Export;
   Cursor cursor = None;
   if (e.Active and (not e.Over.Own)) cursor = e.Accepted ? State->Cursors[0] : State->RefusedCursor;
   if (cursor IS e.Shown) return;
   e.Shown = cursor;
   XChangeActivePointerGrab(State->Connection, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|
      EnterWindowMask|LeaveWindowMask, cursor, CurrentTime);
}

// Ends the export and releases XdndSelection.  The caller holds the native lock and reports any result afterwards.

static void reset_export(X11Driver::State *State)
{
   auto &e = State->Export;
   e.Active = false;
   update_cursor(State);
   if (State->ExportWindow) {
      if (XGetSelectionOwner(State->Connection, State->Xdnd.Selection) IS State->ExportWindow) {
         XSetSelectionOwner(State->Connection, State->Xdnd.Selection, None, event_time(State));
      }
      XDeleteProperty(State->Connection, State->ExportWindow, State->Xdnd.TypeList);
   }
   if (State->ExportTimer) {
      UpdateTimer(State->ExportTimer, 0);
      State->ExportTimer = nullptr;
   }
   e = {};
   XFlush(State->Connection);
}

static void report_export(X11Driver::State *State, bool Accepted)
{
   kt::Log("X11Mgr").trace("XDND export ended: %s", Accepted ? "accepted" : "refused");
   auto callbacks = State->Callbacks;
   if (callbacks and callbacks->DragExportEnded) callbacks->DragExportEnded(Accepted);
}

//********************************************************************************************************************
// A task-owned timer bounds the wait for a status after release and for XdndFinished after the drop.

static ERR export_timer(OBJECTPTR, int64_t, int64_t, APTR Meta)
{
   auto state = (X11Driver::State *)Meta;
   expire_export(state);

   const std::lock_guard lock(state->NativeLock);
   auto &e = state->Export;
   if ((not e.Released) and (not e.Dropped)) {
      state->ExportTimer = nullptr;
      return ERR::Terminate;
   }
   const auto delay = std::chrono::duration<double>(e.Deadline - std::chrono::steady_clock::now()).count();
   return UpdateTimer(state->ExportTimer, std::max(0.001, delay));
}

// The caller holds the native lock.

static void arm_export_deadline(X11Driver::State *State)
{
   State->Export.Deadline = std::chrono::steady_clock::now() + State->DropTimeout;
   if (State->ExportTimer) return;
   kt::SwitchContext context(CurrentTask());
   const auto delay = std::max(0.001, std::chrono::duration<double>(State->DropTimeout).count());
   if (SubscribeTimer(delay, C_FUNCTION(export_timer, State), &State->ExportTimer) != ERR::Okay) {
      State->ExportTimer = nullptr; // The event loop still checks the deadline whenever it runs
   }
}

// A recipient that does not answer after release is refused.  One that has the drop but sends no XdndFinished is
// reported as accepted only if it read the selection.

static void expire_export(X11Driver::State *State)
{
   bool accepted;
   {
      const std::lock_guard lock(State->NativeLock);
      auto &e = State->Export;
      if (((not e.Released) and (not e.Dropped)) or (std::chrono::steady_clock::now() < e.Deadline)) return;
      kt::Log("X11Mgr").msg("The XDND recipient %lu did not respond in time.", e.Over.Target);
      if (e.Released) send_leave(State);
      accepted = e.Dropped and e.Served;
      reset_export(State);
   }
   report_export(State, accepted);
}

// Called with the native lock held, after the status for the final position has arrived.  Returns false if the
// recipient refused, in which case the export has ended and the caller reports it.

static bool complete_release(X11Driver::State *State)
{
   auto &e = State->Export;
   e.Released = false;
   if (not e.Accepted) {
      send_leave(State);
      reset_export(State);
      return false;
   }
   send_drop(State);
   e.Dropped = true;
   arm_export_deadline(State);
   return true;
}

//********************************************************************************************************************

ERR x11_xdnd_start_export(X11Driver::State *State, const std::vector<std::string> &Files, const std::string *Text,
   Window Exclude)
{
   const std::lock_guard lock(State->NativeLock);
   auto &e = State->Export;
   if (e.Active or e.Released or e.Dropped) return ERR::InUse;
   if ((not State->Connection) or (not State->Xdnd.Selection)) return ERR::NotInitialised;

   // Paths are native already.  Folders are offered without a trailing separator, as file managers offer them.

   X11DragExport offer;
   for (auto &file : Files) {
      std::string_view path(file);
      while ((path.size() > 1) and (path.back() IS '/')) path.remove_suffix(1);
      if (path.empty() or (path[0] != '/')) continue;
      offer.Uris.append(encode_file_uri(path)).append("\r\n");
   }
   if (not offer.Uris.empty()) offer.Types.push_back(State->URIListAtom);
   if (Text) {
      offer.Text = *Text;
      offer.Types.insert(offer.Types.end(), { State->UTF8StringAtom, State->TextUTF8Atom, State->TextPlainAtom,
         Atom(XA_STRING) });
   }
   if (offer.Types.empty()) return ERR::NoData;

   if (not State->ExportWindow) {
      XSetWindowAttributes attributes = {};
      State->ExportWindow = XCreateWindow(State->Connection, DefaultRootWindow(State->Connection), -10, -10, 1, 1, 0,
         0, InputOnly, CopyFromParent, 0, &attributes);
      if (not State->ExportWindow) return ERR::SystemCall;
   }

   XChangeProperty(State->Connection, State->ExportWindow, State->Xdnd.TypeList, XA_ATOM, 32, PropModeReplace,
      (const unsigned char *)offer.Types.data(), int(offer.Types.size()));
   offer.Timestamp = event_time(State);
   XSetSelectionOwner(State->Connection, State->Xdnd.Selection, State->ExportWindow, offer.Timestamp);
   if (XGetSelectionOwner(State->Connection, State->Xdnd.Selection) != State->ExportWindow) {
      XDeleteProperty(State->Connection, State->ExportWindow, State->Xdnd.TypeList);
      return ERR::LockFailed;
   }

   offer.Exclude = Exclude;
   offer.Active = true;
   e = std::move(offer);
   XFlush(State->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// Called for pointer movement after the framework has processed it.  Each XdndPosition waits for its status; later
// movement is coalesced into a single pending position.

void x11_xdnd_export_motion(X11Driver::State *State, int X, int Y)
{
   const std::lock_guard lock(State->NativeLock);
   auto &e = State->Export;
   if (not e.Active) return;

   e.RootX = X;
   e.RootY = Y;
   const auto found = x11_xdnd_locate(State, X, Y, e.Exclude);
   if ((found.Target != e.Over.Target) or (found.Proxy != e.Over.Proxy) or (found.Own != e.Over.Own)) {
      if (e.Over.Target) send_leave(State);
      e.Over = found;
      e.WaitingStatus = false;
      e.PositionPending = false;
      e.Accepted = false;
      if (e.Over.Target) send_enter(State);
   }

   if (e.Over.Target) {
      if (e.WaitingStatus) e.PositionPending = true;
      else send_position(State);
   }

   update_cursor(State);
   XFlush(State->Connection);
}

//********************************************************************************************************************

static void handle_export_status(X11Driver::State *State, const XClientMessageEvent &Event)
{
   bool refused = false;
   {
      const std::lock_guard lock(State->NativeLock);
      auto &e = State->Export;
      if (((not e.Active) and (not e.Released)) or (not e.Over.Target) or
          (Window(Event.data.l[0]) != e.Over.Target)) return;

      const auto action = Atom(Event.data.l[4]);
      e.WaitingStatus = false;
      e.Accepted = (Event.data.l[1] & 1) and ((action IS State->Xdnd.ActionCopy) or (action IS None));

      if (e.PositionPending) send_position(State);
      else if (e.Released) refused = not complete_release(State);

      update_cursor(State);
      XFlush(State->Connection);
   }
   if (refused) report_export(State, false);
}

// Versions before 5 do not report a result, so their XdndFinished signals success.

static void handle_export_finished(X11Driver::State *State, const XClientMessageEvent &Event)
{
   bool accepted;
   {
      const std::lock_guard lock(State->NativeLock);
      auto &e = State->Export;
      if ((not e.Dropped) or (Window(Event.data.l[0]) != e.Over.Target)) return;
      const auto action = Atom(Event.data.l[2]);
      accepted = (e.Over.Version < 5) or
         ((Event.data.l[1] & 1) and ((action IS State->Xdnd.ActionCopy) or (action IS None)));
      reset_export(State);
   }
   report_export(State, accepted);
}

//********************************************************************************************************************

ERR x11_xdnd_drop_export(X11Driver::State *State)
{
   const std::lock_guard lock(State->NativeLock);
   auto &e = State->Export;
   if (not e.Active) return ERR::NoSupport;

   // Windows can move, appear or disappear under a stationary pointer.  Refresh the recipient before release, and
   // negotiate with a new recipient if necessary instead of using the last motion's status.

   State->LocateCache.Valid = false;
   const auto found = x11_xdnd_locate(State, e.RootX, e.RootY, e.Exclude);
   if ((found.Target != e.Over.Target) or (found.Proxy != e.Over.Proxy) or (found.Own != e.Over.Own)) {
      x11_xdnd_export_motion(State, e.RootX, e.RootY);
   }

   e.Active = false;
   update_cursor(State);

   if (e.Over.Own) {
      reset_export(State);
      return DisplayDriver::DROP_INTERNAL;
   }

   if (not e.Over.Target) {
      reset_export(State);
      return ERR::NoAction;
   }

   if (e.WaitingStatus or e.PositionPending) {
      // The decision follows the status for the final position.
      e.Released = true;
      arm_export_deadline(State);
      XFlush(State->Connection);
      return ERR::Okay;
   }

   if (complete_release(State)) {
      XFlush(State->Connection);
      return ERR::Okay;
   }
   return ERR::Cancelled;
}

void x11_xdnd_cancel_export(X11Driver::State *State)
{
   const std::lock_guard lock(State->NativeLock);
   auto &e = State->Export;
   if (not e.Active) return;
   if (e.Over.Target) send_leave(State);
   reset_export(State);
}

//********************************************************************************************************************
// Selection data for XdndSelection.  STRING is converted from the UTF-8 text by the caller.  The caller holds the
// native lock.

bool x11_xdnd_export_payload(X11Driver::State *State, Atom Target, std::string &Data, Atom &Type)
{
   auto &e = State->Export;
   if ((not e.Active) and (not e.Released) and (not e.Dropped)) return false;
   if (std::find(e.Types.begin(), e.Types.end(), Target) IS e.Types.end()) return false;

   Type = Target;
   Data = Target IS State->URIListAtom ? e.Uris : e.Text;
   e.Served = true;
   if (e.Dropped) e.Deadline = std::chrono::steady_clock::now() + State->DropTimeout; // Progress extends the wait
   return true;
}

const std::vector<Atom> & x11_xdnd_export_types(X11Driver::State *State)
{
   return State->Export.Types;
}

} // namespace display
