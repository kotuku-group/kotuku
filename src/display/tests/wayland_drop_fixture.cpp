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
