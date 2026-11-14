// Inbound XDND support for the X11 driver.
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

#include "x11_native.h"

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
      (char *)"XdndActionCopy", (char *)"KOTUKU_XDND"
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

bool x11_xdnd_client_message(X11Driver::State *State, const XClientMessageEvent &Event)
{
   const auto &xdnd = State->Xdnd;
   const auto type = Event.message_type;
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

void x11_xdnd_expire(X11Driver::State *State)
{
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
      State->Xdnd = {};
   }
}

} // namespace display
