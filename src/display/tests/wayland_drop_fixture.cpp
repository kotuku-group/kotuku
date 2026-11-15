// Included in wayland_driver.cpp to drive the real listener and transfer code through a private Wayland socket.
#include "wayland_drop_fixture.h"

static thread_local WaylandSeat *glDropFixtureSeat = nullptr;
static thread_local const DriverCallbacks *glDropFixtureCallbacks = nullptr;

static bool close_on_hover(OBJECTID SurfaceID, double X, double Y, CSTRING Datatypes, size_t Length)
{
   glDropFixtureCallbacks->DragOver(SurfaceID, X, Y, Datatypes, Length);
   auto window = glDropFixtureSeat->DragWindow;
   data_leave(glDropFixtureSeat, nullptr);
   glDropFixtureSeat->Owner->Windows.erase(window);
   return true;
}

extern "C" DISPLAY_DRIVER_EXPORT int wayland_drop_test(const DriverCallbacks *Callbacks, DropCase Case)
{
   int sockets[2];
   if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0, sockets) < 0) return 1;
   WaylandDriver::State state;
   state.Callbacks = Callbacks;
   state.Connection = wl_display_connect_to_fd(sockets[0]);
   if (not state.Connection) { ::close(sockets[0]); ::close(sockets[1]); return 1; }
   auto registry = wl_display_get_registry(state.Connection);
   WaylandWindow window;
   window.Owner = &state;
   window.SurfaceID = 42;
   window.Width = window.Height = 200;
   window.Surface = (wl_surface *)wl_registry_bind(registry, 1, &wl_surface_interface, 1);
   window.TopLevel = (xdg_toplevel *)window.Surface; // Only its presence is used to calculate the client frame.
   state.Windows.insert(&window);
   WaylandSeat seat;
   seat.Owner = &state;
   const bool focus_lost = (Case IS DropCase::ShiftFocusLost) or (Case IS DropCase::AltFocusLost);
   if ((Case IS DropCase::Shift) or (Case IS DropCase::Alt) or (Case IS DropCase::Control) or focus_lost) {
      seat.Context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      xkb_rule_names names = {};
      names.layout = "us";
      seat.Keymap = xkb_keymap_new_from_names(seat.Context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      seat.KeyState = xkb_state_new(seat.Keymap);
      const bool shift = (Case IS DropCase::Shift) or (Case IS DropCase::ShiftFocusLost);
      const auto name = shift ? XKB_MOD_NAME_SHIFT :
         ((Case IS DropCase::Alt) or (Case IS DropCase::AltFocusLost) ? XKB_MOD_NAME_ALT : XKB_MOD_NAME_CTRL);
      const auto index = xkb_keymap_mod_get_index(seat.Keymap, name);
      if (focus_lost) {
         wl_array keys = {};
         keyboard_enter(&seat, nullptr, 1, window.Surface, &keys);
         seat.HeldModifiers.insert(shift ? KEY::L_SHIFT : KEY::L_ALT);
      }
      keyboard_modifiers(&seat, nullptr, 1, xkb_mod_mask_t(1) << index, 0, 0, 0);
      // A modifier released in the source application need not generate another event for this client.
      if (focus_lost) keyboard_leave(&seat, nullptr, 2, window.Surface);
   }
   state.Seats.push_back(&seat);
   auto callbacks = *Callbacks;
   if (Case IS DropCase::CloseOnHover) {
      glDropFixtureSeat = &seat;
      glDropFixtureCallbacks = Callbacks;
      callbacks.DragOver = close_on_hover;
      state.Callbacks = &callbacks;
   }
   auto handle = (wl_data_offer *)wl_registry_bind(registry, 2, &wl_data_offer_interface,
      Case IS DropCase::Legacy ? 2 : 3);
   data_offer(&seat, nullptr, handle);
   auto offer = find_offer(&seat, handle);
   const auto offer_id = wl_proxy_get_id((wl_proxy *)handle);
   const auto mime = Case IS DropCase::Unsupported ? "image/png" :
      (Case IS DropCase::Text ? "text/plain;charset=utf-8" : "text/uri-list");
   offer_mime(offer, handle, mime);
   offer_actions(offer, handle, Case IS DropCase::MoveOnly ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE :
      WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
   offer_action(offer, handle, Case IS DropCase::NoAction ? 0 :
      (Case IS DropCase::SelectedMove ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE :
       WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY));
   const auto frame = window_frame(&window);
   data_enter(&seat, nullptr, 123, window.Surface, wl_fixed_from_double(25.5 + frame.Border),
      wl_fixed_from_double(40.25 + frame.Top), handle);
   if (Case IS DropCase::Leave) data_leave(&seat, nullptr);
   else data_drop(&seat, nullptr);

   int failures = 0;
   const bool expected_transfer = (Case IS DropCase::Copy) or (Case IS DropCase::ReadFailure) or
      (Case IS DropCase::Text) or (Case IS DropCase::Legacy) or (Case IS DropCase::RejectData) or
      (Case IS DropCase::Control) or focus_lost;
   if (state.Transfers.empty() IS expected_transfer) failures++;
   std::vector<char> wire;
   auto read_wire = [&]() {
      char bytes[4096];
      char control[CMSG_SPACE(sizeof(int))];
      iovec buffer = { bytes, sizeof(bytes) };
      msghdr message = {};
      message.msg_iov = &buffer;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto length = recvmsg(sockets[1], &message, MSG_DONTWAIT);
      if (length > 0) wire.insert(wire.end(), bytes, bytes + length);
      auto ancillary = CMSG_FIRSTHDR(&message);
      if ((length > 0) and ancillary and (ancillary->cmsg_level IS SOL_SOCKET) and
            (ancillary->cmsg_type IS SCM_RIGHTS)) {
         int fd;
         std::memcpy(&fd, CMSG_DATA(ancillary), sizeof(fd));
         return fd;
      }
      return -1;
   };
   wl_display_flush(state.Connection);
   int writer = read_wire();
   if (not state.Transfers.empty()) {
      auto transfer = state.Transfers.front();
      if (writer < 0) failures++;
      else {
         if ((fcntl(writer, F_GETFL) & O_NONBLOCK) != 0) failures++;
         const std::string payload = Case IS DropCase::Text ? "Wayland <&> 三😀" :
            "file:///tmp/two%20words%20%26%20%E4%B8%89.txt\r\n";
         const auto half = payload.size() / 2;
         if (write_without_sigpipe(writer, payload.data(), half) != ssize_t(half)) failures++;
         transfer_ready(HOSTHANDLE{}, transfer);
         if (state.Transfers.empty()) failures++;
         else {
            // A leave, another window and pointer motion during the transfer must not change its snapshot.
            data_leave(&seat, nullptr);
            seat.X = seat.Y = 180;
            window.SurfaceID = 99;
            if (Case IS DropCase::ReadFailure) {
               finish_transfer(transfer, false);
            }
            else {
               if (write_without_sigpipe(writer, payload.data() + half, payload.size() - half) !=
                     ssize_t(payload.size() - half)) failures++;
               ::close(writer);
               writer = -1;
               transfer_ready(HOSTHANDLE{}, transfer);
            }
         }
      }
   }
   if (writer >= 0) ::close(writer);
   if (seat.DragOffer) data_leave(&seat, nullptr);
   wl_display_flush(state.Connection);
   read_wire();
   int finished = 0;
   for (size_t offset = 0; offset + 8 <= wire.size(); ) {
      uint32_t id, header;
      std::memcpy(&id, wire.data() + offset, 4);
      std::memcpy(&header, wire.data() + offset + 4, 4);
      const auto size = header >> 16;
      if (size < 8) { failures++; break; }
      if ((id IS offer_id) and ((header & 0xffff) IS WL_DATA_OFFER_FINISH)) finished++;
      offset += size;
   }
   const bool expected_finish = (Case IS DropCase::Copy) or (Case IS DropCase::Text) or
      (Case IS DropCase::Control) or focus_lost;
   if (finished != int(expected_finish)) failures++;
   for (auto transfer : state.Transfers) {
      DeregisterFD(transfer->FD);
      ::close(transfer->FD);
      delete transfer;
   }
   while (not state.Offers.empty()) dispose_offer(state.Offers.back());
   wl_surface_destroy(window.Surface);
   wl_registry_destroy(registry);
   wl_display_disconnect(state.Connection);
   ::close(sockets[1]);
   if (seat.KeyState) xkb_state_unref(seat.KeyState);
   if (seat.Keymap) xkb_keymap_unref(seat.Keymap);
   if (seat.Context) xkb_context_unref(seat.Context);
   glDropFixtureSeat = nullptr;
   glDropFixtureCallbacks = nullptr;
   return failures;
}

//********************************************************************************************************************
// Outbound drag export.  The fixture's ButtonInput callback answers a synthesised release as the Display module does,
// by calling drop_export() synchronously.

namespace {

struct ExportRecord {
   WaylandDriver::State *State = nullptr;
   bool SimulateDisplay = true;
   int Releases = 0;
   std::vector<ERR> DropResults;
   int Ended = 0;
   bool Accepted = false;
   int CrossIn = 0;
   int CrossOut = 0;
   int DragOvers = 0;
   std::vector<std::array<double, 3>> Moves;
};

struct WireMessage {
   uint32_t ID;
   uint32_t Opcode;
   std::vector<uint32_t> Words;
};

}

static thread_local ExportRecord *glExport = nullptr;

static void export_button(int Buttons, bool Pressed)
{
   if (Pressed or (not (Buttons & 0x0001))) return;
   glExport->Releases++;
   if (glExport->SimulateDisplay) glExport->DropResults.push_back(drop_export(glExport->State));
}

static void export_movement(OBJECTID SurfaceID, double X, double Y, bool)
{
   glExport->Moves.push_back({ double(SurfaceID), X, Y });
}

static void export_crossing(OBJECTID, bool Entered, double, double)
{
   if (Entered) glExport->CrossIn++;
   else glExport->CrossOut++;
}

static bool export_drag_over(OBJECTID, double, double, CSTRING, size_t) { glExport->DragOvers++; return true; }
static void export_drag_leave() { }

static void export_ended(bool Accepted)
{
   glExport->Ended++;
   glExport->Accepted = Accepted;
}

static void read_wire_messages(int Socket, std::vector<WireMessage> &Messages)
{
   std::vector<uint8_t> bytes;
   while (true) {
      uint8_t buffer[8192];
      char control[CMSG_SPACE(sizeof(int) * 8)];
      iovec io = { buffer, sizeof(buffer) };
      msghdr message = {};
      message.msg_iov = &io;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto length = recvmsg(Socket, &message, MSG_DONTWAIT);
      if (length <= 0) break;
      bytes.insert(bytes.end(), buffer, buffer + length);
      for (auto cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
         if ((cmsg->cmsg_level IS SOL_SOCKET) and (cmsg->cmsg_type IS SCM_RIGHTS)) {
            const auto count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
               int fd;
               std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
               ::close(fd);
            }
         }
      }
   }
   for (size_t offset = 0; offset + 8 <= bytes.size(); ) {
      uint32_t id, header;
      std::memcpy(&id, bytes.data() + offset, 4);
      std::memcpy(&header, bytes.data() + offset + 4, 4);
      const size_t size = header >> 16;
      if ((size < 8) or (offset + size > bytes.size())) break;
      WireMessage msg = { id, header & 0xffff, std::vector<uint32_t>((size - 8) / 4) };
      if (size > 8) std::memcpy(msg.Words.data(), bytes.data() + offset + 8, size - 8);
      Messages.push_back(std::move(msg));
      offset += size;
   }
}

// Returns the string argument that starts at word Index, or null for a null string.

static std::optional<std::string> wire_string(const WireMessage &Message, size_t Index)
{
   if (Index >= Message.Words.size()) return std::nullopt;
   const auto length = Message.Words[Index];
   if (not length) return std::nullopt;
   if ((Index + 1) * 4 + length > Message.Words.size() * 4) return std::nullopt;
   return std::string((const char *)(Message.Words.data() + Index + 1), length - 1);
}

static std::string read_export(WaylandDriver::State *State, const char *Mime)
{
   int pipes[2];
   if (pipe2(pipes, O_CLOEXEC) < 0) return "<pipe>";
   export_send(State, State->Export.Source, Mime, pipes[1]);
   std::string result;
   char buffer[1024];
   ssize_t count;
   while ((count = ::read(pipes[0], buffer, sizeof(buffer))) > 0) result.append(buffer, size_t(count));
   ::close(pipes[0]);
   return result;
}

extern "C" DISPLAY_DRIVER_EXPORT int wayland_export_test(ExportCase Case)
{
   int failures = 0;
   auto check = [&](bool Condition, const char *Description) {
      if (Condition) return;
      std::fprintf(stderr, "Wayland export case %d: %s\n", int(Case), Description);
      failures++;
   };

   int sockets[2];
   if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0, sockets) < 0) return 1;
   WaylandDriver::State state;
   state.Connection = wl_display_connect_to_fd(sockets[0]);
   if (not state.Connection) { ::close(sockets[0]); ::close(sockets[1]); return 1; }
   auto registry = wl_display_get_registry(state.Connection);
   state.DataManager = (wl_data_device_manager *)wl_registry_bind(registry, 1, &wl_data_device_manager_interface,
      Case IS ExportCase::Unavailable ? 2 : 3);

   WaylandWindow window;
   window.Owner = &state;
   window.SurfaceID = 42;
   window.Width = window.Height = 200;
   window.Surface = (wl_surface *)wl_registry_bind(registry, 2, &wl_surface_interface, 1);
   state.Windows.insert(&window);

   WaylandSeat seat;
   seat.Owner = &state;
   seat.DataDevice = (wl_data_device *)wl_registry_bind(registry, 3, &wl_data_device_interface, 3);
   seat.PointerWindow = &window;
   seat.X = 100;
   seat.Y = 50;
   seat.PointerInContent = true;
   state.Seats.push_back(&seat);

   ExportRecord record;
   record.State = &state;
   glExport = &record;
   DriverCallbacks callbacks = {};
   callbacks.Version = DISPLAY_DRIVER_INTERFACE_VERSION;
   callbacks.Movement = export_movement;
   callbacks.ButtonInput = export_button;
   callbacks.Crossing = export_crossing;
   callbacks.DragOver = export_drag_over;
   callbacks.DragLeave = export_drag_leave;
   callbacks.DragExportEnded = export_ended;
   state.Callbacks = &callbacks;
   pointer_button(&seat, nullptr, 77, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED); // The press that starts the drag

   std::vector<WireMessage> wire;
   auto sync_wire = [&]() { wl_display_flush(state.Connection); read_wire_messages(sockets[1], wire); };
   auto find_message = [&](uint32_t ID, uint32_t Opcode) -> const WireMessage * {
      for (auto &msg : wire) if ((msg.ID IS ID) and (msg.Opcode IS Opcode)) return &msg;
      return nullptr;
   };
   auto count_messages = [&](uint32_t ID, uint32_t Opcode) {
      int total = 0;
      for (auto &msg : wire) if ((msg.ID IS ID) and (msg.Opcode IS Opcode)) total++;
      return total;
   };

   const std::vector<std::string> files = { "/tmp/two words & 三/", "relative/ignored" };
   const std::string text = "Wayland <&> 三😀";
   const auto idle = WaylandExport::Phase::None;

   check(not foreign_point(&state), "a press inside the window is not foreign");
   seat.X = 250;
   check(foreign_point(&state), "the held button outside the window is foreign");

   if (Case IS ExportCase::Unavailable) {
      check(start_export(&state, files, &text, nullptr) IS ERR::NoSupport, "version 2 data devices cannot export");
      wl_proxy_destroy((wl_proxy *)state.DataManager);
      state.DataManager = (wl_data_device_manager *)wl_registry_bind(registry, 1, &wl_data_device_manager_interface, 3);
      check(start_export(&state, {}, nullptr, nullptr) IS ERR::NoData, "an empty payload is refused");
      check(start_export(&state, { "relative" }, nullptr, nullptr) IS ERR::NoData, "relative paths are not offered");
      seat.ContentButtons = 0;
      check(start_export(&state, files, &text, nullptr) IS ERR::NotPossible, "an export needs the held button");
      check(state.Export.Stage IS idle, "failed starts leave no export");
   }
   else {
      const bool with_text = (Case != ExportCase::ForeignCopy);
      check(start_export(&state, files, with_text ? &text : nullptr, nullptr) IS ERR::Okay, "the export starts");
      check(start_export(&state, files, &text, nullptr) IS ERR::InUse, "a second export is refused");
      sync_wire();
      const auto source_id = state.Export.Source ? wl_proxy_get_id((wl_proxy *)state.Export.Source) : 0;
      const auto marker = state.Export.Marker;
      check(marker.starts_with(export_marker_prefix()), "the marker identifies this process");

      auto created = find_message(wl_proxy_get_id((wl_proxy *)state.DataManager), 0);
      check(created and (created->Words.size() IS 1) and (created->Words[0] IS source_id), "a data source is created");
      std::vector<std::string> offered;
      for (auto &msg : wire) {
         if ((msg.ID IS source_id) and (msg.Opcode IS 0)) {
            if (auto mime = wire_string(msg, 0)) offered.push_back(*mime);
         }
      }
      std::vector<std::string> expected = { "text/uri-list" };
      if (with_text) expected.insert(expected.end(), { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" });
      expected.push_back(marker);
      check(offered IS expected, "the offered types");
      auto actions = find_message(source_id, 2);
      check(actions and (actions->Words[0] IS WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY), "only copy is offered");
      auto drag = find_message(wl_proxy_get_id((wl_proxy *)seat.DataDevice), 0);
      check(drag and (drag->Words.size() IS 4) and (drag->Words[0] IS source_id) and
         (drag->Words[1] IS wl_proxy_get_id((wl_proxy *)window.Surface)) and (drag->Words[2] IS 0) and
         (drag->Words[3] IS 77), "start_drag uses the press serial and the pressed surface");

      const std::string uris = "file:///tmp/two%20words%20%26%20%E4%B8%89\r\n";

      if (Case IS ExportCase::StartRefused) {
         // The compositor refuses the drag without taking the pointer, so the button is still held.
         export_cancelled(&state, state.Export.Source);
         check(record.Releases IS 0, "no release is synthesised while the pointer is ours");
         check(foreign_point(&state), "geometry decides foreign points after a refused start");
         seat.X = 100;
         check(not foreign_point(&state), "returning to the window is not foreign");
         seat.ContentButtons = 0;
         check(drop_export(&state) IS DisplayDriver::DROP_INTERNAL, "a release inside the window is internal");
         check(state.Export.Stage IS idle, "the export ends at the release");
      }
      else if ((Case IS ExportCase::Cancel) or (Case IS ExportCase::CancelGranted)) {
         const bool granted = Case IS ExportCase::CancelGranted;
         if (granted) {
            pointer_leave(&seat, nullptr, 0, window.Surface);
            check(state.Export.Granted, "the compositor took the pointer before cancellation");
            check(record.Releases IS 0, "taking the pointer preserves the held button");
         }
         // The internal session has ended before the driver is asked to cancel it.
         record.SimulateDisplay = false;
         cancel_export(&state);
         sync_wire();
         check(count_messages(source_id, 1) IS 1, "cancellation destroys the source");
         check((state.Export.Stage IS idle) and (record.Ended IS 0), "cancelled without a deferred result");
         check(record.Releases IS int(granted), "only a granted cancellation synthesises a release");
         cancel_export(&state);
         seat.PointerWindow = &window;
         pointer_button(&seat, nullptr, 78, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
         check(record.Releases IS 1, "the button is released exactly once after cancellation");
         check(seat.ContentButtons IS 0, "no button remains held after cancellation");
         check(record.DropResults.empty(), "cancellation never delivers a drop");
         seat.X = 100;
         pointer_button(&seat, nullptr, 79, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
         check(start_export(&state, files, nullptr, nullptr) IS ERR::Okay, "a new export can start");
         check(state.Export.Marker != marker, "each export has its own marker");
      }
      else {
         pointer_leave(&seat, nullptr, 0, window.Surface);
         check(state.Export.Granted, "the pointer leave grants the drag");
         check(record.Releases IS 0, "the grant does not release the button");
         check(foreign_point(&state), "the drag is foreign until it enters our window");

         switch (Case) {
            case ExportCase::ForeignCopy:
               check(read_export(&state, "text/plain").empty(), "a file-only export has no text");
               check(read_export(&state, marker.c_str()) IS marker, "the marker is readable");
               [[fallthrough]];
            case ExportCase::TimeoutServed:
               check(read_export(&state, "text/uri-list") IS uris, "the URI list");
               break;
            case ExportCase::ForeignRefused:
            case ExportCase::ForeignNoAction:
               check(read_export(&state, "UTF8_STRING") IS text, "UTF8_STRING text");
               check(read_export(&state, "text/plain;charset=utf-8") IS text, "UTF-8 text");
               check(read_export(&state, "image/png").empty(), "an unoffered type is empty");
               break;
            default: break;
         }

         if ((Case IS ExportCase::ForeignCopy) or (Case IS ExportCase::ForeignRefused) or
               (Case IS ExportCase::ForeignNoAction) or (Case IS ExportCase::TimeoutServed) or
               (Case IS ExportCase::TimeoutUnread)) {
            export_target(&state, state.Export.Source, "text/uri-list");
            export_action(&state, state.Export.Source, Case IS ExportCase::ForeignNoAction ? 0 :
               WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
            export_drop_performed(&state, state.Export.Source);
            check((record.Releases IS 1) and (record.DropResults.size() IS 1) and
               (record.DropResults[0] IS ERR::Okay), "the drop synthesises a release that awaits the result");
            check(record.Ended IS 0, "no result is reported from within the release");
            check(state.Export.TimerFD >= 0, "the result is bounded by a timer");
            check(start_export(&state, files, &text, nullptr) IS ERR::InUse, "a pending drop refuses new exports");
            cancel_export(&state);
            check(state.Export.Stage IS WaylandExport::Phase::Dropped, "cancellation is ignored after the drop");
            check(foreign_point(&state), "a dropped export is foreign");

            if ((Case IS ExportCase::TimeoutServed) or (Case IS ExportCase::TimeoutUnread)) {
               export_timeout(state.Export.TimerFD, &state);
               check(record.Ended IS 0, "the timer does not expire early");
               itimerspec soon = {};
               soon.it_value.tv_nsec = 1000;
               timerfd_settime(state.Export.TimerFD, 0, &soon, nullptr);
               usleep(2000);
               export_timeout(state.Export.TimerFD, &state);
               check((record.Ended IS 1) and (record.Accepted IS (Case IS ExportCase::TimeoutServed)),
                  "expiry reports acceptance only if the payload was read");
            }
            else if (Case IS ExportCase::ForeignRefused) {
               export_cancelled(&state, state.Export.Source);
               check((record.Ended IS 1) and (not record.Accepted), "a cancelled drop is refused");
            }
            else {
               export_finished(&state, state.Export.Source);
               check((record.Ended IS 1) and (record.Accepted IS (Case IS ExportCase::ForeignCopy)),
                  "the finished copy is reported once");
            }
            check(record.Releases IS 1, "exactly one release");
         }
         else if ((Case IS ExportCase::NoTarget) or (Case IS ExportCase::SourceEnded)) {
            record.SimulateDisplay = (Case IS ExportCase::NoTarget);
            export_cancelled(&state, state.Export.Source);
            check(record.Releases IS 1, "the cancelled drag synthesises the release");
            if (Case IS ExportCase::NoTarget) {
               check((record.DropResults.size() IS 1) and (record.DropResults[0] IS ERR::Cancelled),
                  "a release with no recipient is cancelled");
            }
            check(record.Ended IS 0, "no deferred result");
         }
         else if (Case IS ExportCase::ReleaseElsewhere) {
            check(drop_export(&state) IS ERR::Cancelled, "a release without a compositor drop is cancelled");
            sync_wire();
            check(count_messages(source_id, 1) IS 1, "the source is destroyed");
         }
         else if ((Case IS ExportCase::ReturnToOwn) or (Case IS ExportCase::StaleOwnOffer) or
                  (Case IS ExportCase::ForeignKotuku)) {
            auto make_offer = [&](std::vector<std::string> Mimes) {
               auto handle = (wl_data_offer *)wl_registry_bind(registry, 4, &wl_data_offer_interface, 3);
               data_offer(&seat, nullptr, handle);
               auto offer = find_offer(&seat, handle);
               for (auto &mime : Mimes) offer_mime(offer, handle, mime.c_str());
               offer_actions(offer, handle, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
               return handle;
            };

            if (Case IS ExportCase::ReturnToOwn) {
               auto handle = make_offer({ marker, "text/plain" });
               const auto first_id = wl_proxy_get_id((wl_proxy *)handle);
               data_enter(&seat, nullptr, 900, window.Surface, wl_fixed_from_double(30), wl_fixed_from_double(40),
                  handle);
               sync_wire();
               auto accept = find_message(first_id, 0);
               check(accept and (accept->Words[0] IS 900) and (wire_string(*accept, 1) IS marker),
                  "our own offer is accepted with its marker");
               check((record.CrossIn IS 1) and (not record.Moves.empty()) and
                  (record.Moves.back() IS std::array<double, 3>{ 42, 30, 40 }), "hover is routed internally");
               check(record.DragOvers IS 0, "our own offer is never imported");
               check(not foreign_point(&state), "our window is not foreign");
               data_motion(&seat, nullptr, 0, wl_fixed_from_double(60), wl_fixed_from_double(70));
               check(record.Moves.back() IS std::array<double, 3>{ 42, 60, 70 }, "motion is routed internally");
               const auto crossed_out = record.CrossOut;
               data_leave(&seat, nullptr);
               check((record.CrossOut IS crossed_out + 1) and
                  (record.Moves.back() IS std::array<double, 3>{ 42, -1, 70 }),
                  "leaving moves the session outside the nearest edge");
               check(foreign_point(&state), "the drag is foreign again after leaving");
               sync_wire();
               check(count_messages(first_id, 2) IS 1, "the left offer is destroyed");

               auto second = make_offer({ marker });
               data_enter(&seat, nullptr, 901, window.Surface, wl_fixed_from_double(20), wl_fixed_from_double(25),
                  second);
               const auto second_id = wl_proxy_get_id((wl_proxy *)second);
               data_drop(&seat, nullptr);
               check((record.Releases IS 1) and (record.DropResults.size() IS 1) and
                  (record.DropResults[0] IS DisplayDriver::DROP_INTERNAL), "the drop is delivered internally");
               check((state.Export.Stage IS idle) and (record.Ended IS 0), "the export ends without a result");
               sync_wire();
               check(count_messages(second_id, 3) IS 0, "our own offer is not finished");
               check(count_messages(second_id, 2) IS 1, "our own offer is destroyed");
               check(count_messages(source_id, 1) IS 1, "the source is destroyed");
               data_leave(&seat, nullptr);
            }
            else {
               const auto other = Case IS ExportCase::StaleOwnOffer ? export_marker_prefix() + "id=999999" :
                  std::string(EXPORT_MARKER) + std::to_string(getpid() + 1) + ";id=1";
               auto handle = make_offer({ other, "text/plain" });
               const auto offer_id = wl_proxy_get_id((wl_proxy *)handle);
               data_enter(&seat, nullptr, 902, window.Surface, wl_fixed_from_double(30), wl_fixed_from_double(40),
                  handle);
               sync_wire();
               auto accept = find_message(offer_id, 0);
               if (Case IS ExportCase::StaleOwnOffer) {
                  check(accept and (accept->Words[0] IS 902) and (not wire_string(*accept, 1)),
                     "a stale export of ours is refused");
                  check((record.CrossIn IS 0) and record.Moves.empty() and (record.DragOvers IS 0),
                     "a stale export is not routed or imported");
                  data_drop(&seat, nullptr);
                  check(record.Releases IS 0, "a stale export cannot drop");
               }
               else {
                  check(record.DragOvers > 0, "another process's export is imported");
                  check(accept and (wire_string(*accept, 1) IS std::string("text/plain")),
                     "another process's export is accepted as text");
                  check(record.Moves.empty(), "another process's export is not routed internally");
               }
               data_leave(&seat, nullptr);
               check(state.Export.Stage IS WaylandExport::Phase::Hover, "the active export is unaffected");
               cancel_export(&state);
            }
         }
         check(state.Export.Stage IS idle, "the export has ended");
      }
   }

   reset_export(&state);
   for (auto transfer : state.Transfers) {
      DeregisterFD(transfer->FD);
      ::close(transfer->FD);
      delete transfer;
   }
   state.Transfers.clear();
   while (not state.Offers.empty()) dispose_offer(state.Offers.back());
   wl_proxy_destroy((wl_proxy *)seat.DataDevice);
   wl_proxy_destroy((wl_proxy *)state.DataManager);
   wl_surface_destroy(window.Surface);
   wl_registry_destroy(registry);
   wl_display_disconnect(state.Connection);
   ::close(sockets[1]);
   glExport = nullptr;
   return failures;
}
