
#include "wayland_driver.h"
#include "../../defs.h"
#include "../../driver/clipboard_utils.h"
#include "xdg-shell-client-protocol.h"
#include "decorations.h"

#ifdef WAYLAND_FRACTIONAL_SCALE
#include "fractional-scale-client-protocol.h"
#include "viewporter-client-protocol.h"
#endif

#ifdef WAYLAND_POINTER_CONSTRAINTS
#include "pointer-constraints-client-protocol.h"
#endif

#ifdef WAYLAND_RELATIVE_POINTER
#include "relative-pointer-client-protocol.h"
#endif

#ifdef WAYLAND_CURSOR_SHAPE
#include "cursor-shape-client-protocol.h"
#endif

#ifdef WAYLAND_XDG_DECORATION
#include "xdg-decoration-client-protocol.h"
#endif

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <array>
#include <string_view>
#include <utility>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#ifdef WAYLAND_DRIVER_TESTS
#include <optional>
#include <sys/socket.h>
#include "../../tests/wayland_drop_fixture.h"
#endif

namespace display {

struct WaylandWindow;
struct WaylandSeat;
struct WaylandOutput;
struct CursorBuffer;
static void unmap_children(WaylandWindow *Window);

struct WaylandBuffer {
   WaylandDriver::State *Owner = nullptr;
   WaylandWindow *Window = nullptr;
   wl_buffer *Handle = nullptr;
   void *Pixels = nullptr;
   size_t Length = 0;
   int Width = 0;
   int Height = 0;
   bool Busy = false;
};

struct WaylandWindow {
   enum class Phase { Pending, Configured, Mapped, Closing };
   WaylandDriver::State *Owner = nullptr;
   wl_surface *Surface = nullptr;
   xdg_surface *ShellSurface = nullptr;
   xdg_toplevel *TopLevel = nullptr;
   xdg_popup *Popup = nullptr;
   WaylandWindow *Parent = nullptr;
   std::vector<WaylandOutput *> Outputs;
   int Scale = 1;
   int Scale120 = 120;
#ifdef WAYLAND_FRACTIONAL_SCALE
   wp_fractional_scale_v1 *FractionalScale = nullptr;
   wp_viewport *Viewport = nullptr;
   int PreferredScale120 = 0;
#endif
#ifdef WAYLAND_XDG_DECORATION
   zxdg_toplevel_decoration_v1 *Decoration = nullptr;
#endif
   wl_callback *Frame = nullptr;
   extDisplay *Display = nullptr;
   OBJECTID SurfaceID = 0;
   std::string Title;
   std::string AppID; // Empty for the default identity; see setWindowClass()
   std::vector<WaylandBuffer *> Buffers;
   std::vector<uint8_t> Staged;
   std::vector<uint8_t> Presented;
   int Width = 0;
   int Height = 0;
   int NormalWidth = 0;
   int NormalHeight = 0;
   int PendingWidth = 0;
   int PendingHeight = 0;
   int StageWidth = 0;
   int StageHeight = 0;
   int MinWidth = 0;
   int MinHeight = 0;
   int MaxWidth = 0;
   int MaxHeight = 0;
   bool Maximised = false;
   bool Fullscreen = false;
   bool PendingMaximised = false;
   bool PendingFullscreen = false;
   bool RequestedMaximised = false;
   bool RequestedFullscreen = false;
   bool ClientDecorations = true;
   bool PendingClientDecorations = true;
   bool DecorDirty = true;
   Phase Lifecycle = Phase::Pending;
   bool Visible = false;
   bool Dirty = false;
   PTC Cursor = PTC::DEFAULT;
   bool CursorVisible = true;
   CursorBuffer *CustomCursor = nullptr;
};

//********************************************************************************************************************
// Returns the frame geometry of a window.  Client-side decorations are drawn for top-level windows that are not
// fullscreen, unless the compositor has agreed to draw server-side decorations.

static WaylandFrame window_frame(const WaylandWindow *Window)
{
   return { Window->Width, Window->Height,
      Window->TopLevel and Window->ClientDecorations and (not Window->Fullscreen) };
}

//********************************************************************************************************************
// Sends the window's size limits to the compositor.  xdg-shell applies the limits to the window geometry, so the
// client-side frame is added to the content limits.  A maximum of zero means that there is no limit.

static void set_size_hints(WaylandWindow *Window)
{
   if (not Window->TopLevel) return;

   const auto frame = window_frame(Window);

   xdg_toplevel_set_min_size(Window->TopLevel, Window->MinWidth + frame.Border * 2,
      Window->MinHeight + frame.Top + frame.Border);
   xdg_toplevel_set_max_size(Window->TopLevel,
      Window->MaxWidth ? Window->MaxWidth + frame.Border * 2 : 0,
      Window->MaxHeight ? Window->MaxHeight + frame.Top + frame.Border : 0);
}

struct WaylandOutput {
   WaylandDriver::State *Owner = nullptr;
   wl_output *Handle = nullptr;
   uint32_t Name = 0;
   int X = 0;
   int Y = 0;
   int PhysicalWidth = 0;
   int PhysicalHeight = 0;
   int Width = 0;
   int Height = 0;
   int Refresh = 0;
   int Scale = 1;
   std::string Make;
   std::string Model;
};

struct CursorBuffer {
   WaylandDriver::State *Owner = nullptr;
   wl_buffer *Handle = nullptr;
   int Width = 0;
   int Height = 0;
   int HotX = 0;
   int HotY = 0;
   bool Busy = false;
   bool Retired = false;
};

struct WaylandTouchPoint {
   WaylandWindow *Window = nullptr;
   double X = 0;
   double Y = 0;
};

struct WaylandSeat {
   WaylandDriver::State *Owner = nullptr;
   uint32_t Name = 0;
   wl_seat *Handle = nullptr;
   wl_pointer *Pointer = nullptr;
   wl_keyboard *Keyboard = nullptr;
   wl_touch *Touch = nullptr;
   wl_data_device *DataDevice = nullptr;
   struct WaylandSource *SelectionSource = nullptr;
   struct WaylandOffer *SelectionOffer = nullptr;
   struct WaylandOffer *DragOffer = nullptr;
   WaylandWindow *DragWindow = nullptr;
   uint32_t DragSerial = 0;
   uint64_t PublishedGeneration = 0;
   std::unordered_map<int32_t, WaylandTouchPoint> TouchPoints;
   wl_surface *CursorSurface = nullptr;
#ifdef WAYLAND_CURSOR_SHAPE
   wp_cursor_shape_device_v1 *ShapeDevice = nullptr;
#endif
   WaylandWindow *LockedWindow = nullptr;
   WaylandWindow *PointerWindow = nullptr;
   WaylandWindow *KeyboardWindow = nullptr;
   uint32_t PointerSerial = 0;
   uint32_t PressSerial = 0;   // Primary button press in the content area, which can start a drag
   uint32_t ContentButtons = 0;
   bool PointerInContent = false;
   float ScrollX = 0;
   float ScrollY = 0;
   uint32_t KeyboardSerial = 0;
   uint32_t SelectionSerial = 0;
   double X = 0;
   double Y = 0;
   xkb_context *Context = nullptr;
   xkb_keymap *Keymap = nullptr;
   xkb_state *KeyState = nullptr;
   xkb_compose_table *ComposeTable = nullptr;
   xkb_compose_state *ComposeState = nullptr;
   std::unordered_set<KEY> HeldModifiers;
   uint32_t RepeatKey = UINT32_MAX;
   uint32_t RepeatUnicode = 0;
   int RepeatRate = 0;
   int RepeatDelay = 0;
   int TimerFD = -1;
#ifdef WAYLAND_POINTER_CONSTRAINTS
   zwp_locked_pointer_v1 *Locked = nullptr;
#endif
#ifdef WAYLAND_RELATIVE_POINTER
   zwp_relative_pointer_v1 *Relative = nullptr;
#endif
};

// An outbound drag started by startDragExport().  At most one exists.  Once the compositor accepts
// wl_data_device.start_drag it runs the drag itself, and our pointer receives no events until the drag ends.  Hover
// over one of our own windows arrives as a data-device offer carrying the export's private marker type, and is routed
// back into the internal session.  See the outbound dragging section below.

struct WaylandExport {
   enum class Phase { None, Hover, Dropped };
   Phase Stage = Phase::None;
   wl_data_source *Source = nullptr;
   struct WaylandSeat *Seat = nullptr;
   HOSTWINDOW Exclude = nullptr;
   struct WaylandWindow *OwnWindow = nullptr; // Our window under the drag, while routed to the internal session
   std::string Uris;
   std::string Text;
   std::string Marker;    // Private MIME type identifying this export
   uint32_t Action = 0;   // Latest action selected by the compositor
   bool HasText = false;
   bool Granted = false;   // The compositor has taken the pointer for the drag
   bool Cancelled = false; // The compositor cancelled the drag before a drop was performed
   bool OwnDrop = false;   // Released over one of our own windows
   bool Served = false;    // A recipient has requested the payload
   int TimerFD = -1;       // Bounds the wait for the recipient's result after a drop
};

struct WaylandDriver::State {
   wl_display *Connection = nullptr;
   wl_registry *Registry = nullptr;
   wl_compositor *Compositor = nullptr;
   wl_shm *Shm = nullptr;
   xdg_wm_base *Shell = nullptr;
   wl_data_device_manager *DataManager = nullptr;
   std::string ClipboardText;
   std::string ClipboardUris;
   bool ClipboardIsFiles = false;
   uint64_t ClipboardGeneration = 0;
   uint64_t SelectionGeneration = 0;
   uint64_t DropGeneration = 0;
   std::vector<struct WaylandOffer *> Offers;
   std::vector<struct WaylandSource *> Sources;
   std::vector<struct WaylandTransfer *> Transfers;
   WaylandExport Export;
   uint32_t ExportCounter = 0;
#ifdef WAYLAND_XDG_DECORATION
   zxdg_decoration_manager_v1 *DecorationManager = nullptr;
#endif
   const DriverCallbacks *Callbacks = nullptr;
   std::unordered_set<WaylandWindow *> Windows;
   std::vector<WaylandBuffer *> Retired;
   std::vector<WaylandOutput *> Outputs;
   std::vector<WaylandSeat *> Seats;
   std::vector<CursorBuffer *> CursorBuffers;
   wl_cursor_theme *CursorTheme = nullptr;
#ifdef WAYLAND_CURSOR_SHAPE
   wp_cursor_shape_manager_v1 *ShapeManager = nullptr;
#endif
#ifdef WAYLAND_POINTER_CONSTRAINTS
   zwp_pointer_constraints_v1 *Constraints = nullptr;
#endif
#ifdef WAYLAND_RELATIVE_POINTER
   zwp_relative_pointer_manager_v1 *RelativeManager = nullptr;
#endif
   int ConnectionFD = -1;
#ifdef WAYLAND_FRACTIONAL_SCALE
   wp_fractional_scale_manager_v1 *FractionalManager = nullptr;
   wp_viewporter *Viewporter = nullptr;
#endif
   int OutputWidth = 1024;
   int OutputHeight = 768;
   bool Open = false;
   bool Dispatching = false;
   PTC DefaultCursor = PTC::DEFAULT;
   bool DefaultCursorVisible = true;
};

static void try_present(WaylandWindow *Window);
static void update_scale(WaylandWindow *Window);
static void dispose_buffer(WaylandBuffer *Buffer);
static void destroy_protocol_window(WaylandWindow *Window);
static void publish_selection(WaylandSeat *Seat);
static WaylandWindow *surface_window(WaylandDriver::State *State, wl_surface *Surface);
static KQ key_qualifiers(WaylandSeat *Seat);

struct WaylandOffer {
   WaylandDriver::State *Owner = nullptr;
   wl_data_offer *Handle = nullptr;
   std::vector<std::string> Mimes;
   uint32_t Actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
   uint32_t Action = 0;
   bool ActionKnown = false;
   bool PendingDrop = false;
   bool Own = false; // Offers one of this process's exports; never imported
};

struct WaylandSource {
   WaylandDriver::State *Owner = nullptr;
   wl_data_source *Handle = nullptr;
   std::string Payload;
   std::string Mime;
};

struct WaylandTransfer {
   WaylandDriver::State *Owner = nullptr;
   int FD = -1;
   std::string Data;
   size_t Offset = 0;
   std::string Mime;
   bool Dropped = false;
   bool Sending = false;
   uint64_t SelectionGeneration = 0;
   WaylandOffer *Offer = nullptr;
   NativeDrop Drop;
};

static void dispose_offer(WaylandOffer *Offer);
static bool drag_feedback(WaylandSeat *Seat, bool Negotiate = true);

//********************************************************************************************************************
// Ends a data transfer and releases its pipe.  Data that was read successfully is delivered to the Display module as
// drop or clipboard content, unless a newer selection has replaced the clipboard content being read.  An accepted drop
// is reported to the source with wl_data_offer.finish, and the drop offer is disposed.

static void finish_transfer(WaylandTransfer *Transfer, bool Success)
{
   auto state = Transfer->Owner;
   DeregisterFD(Transfer->FD);
   close(Transfer->FD);

   const bool current_selection = Transfer->Dropped or
      (Transfer->SelectionGeneration IS state->SelectionGeneration);
   bool accepted = false;
   if (Success and not Transfer->Sending and current_selection and state->Callbacks) {
      if (Transfer->Dropped and state->Callbacks->DragData)
         accepted = state->Callbacks->DragData(Transfer->Drop, Transfer->Mime.c_str(),
            Transfer->Data.data(), Transfer->Data.size());
      else if (not Transfer->Dropped and state->Callbacks->ClipboardData)
         state->Callbacks->ClipboardData(Transfer->Mime.c_str(), Transfer->Data.data(), Transfer->Data.size(),
            false, 0);
   }

   if (Transfer->Offer) {
      if (accepted and Transfer->Dropped and (Transfer->Offer->Action IS WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) and
            (wl_data_offer_get_version(Transfer->Offer->Handle) >= WL_DATA_OFFER_FINISH_SINCE_VERSION))
         wl_data_offer_finish(Transfer->Offer->Handle);
      dispose_offer(Transfer->Offer);
      wl_display_flush(state->Connection);
   }

   auto &transfers = state->Transfers;
   transfers.erase(std::remove(transfers.begin(), transfers.end(), Transfer), transfers.end());
   delete Transfer;
}

//********************************************************************************************************************
// Writes to a pipe whose reader may have closed it, without raising SIGPIPE.  The signal is blocked for the calling
// thread and a SIGPIPE generated by the write is consumed, unless one was already pending.

static ssize_t write_without_sigpipe(int FD, const char *Data, size_t Length)
{
   sigset_t blocked, previous, pending;
   sigemptyset(&blocked);
   sigaddset(&blocked, SIGPIPE);
   if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) return -1;

   sigpending(&pending);
   bool already_pending = sigismember(&pending, SIGPIPE) IS 1;
   auto count = write(FD, Data, Length);
   int saved_errno = errno;
   if ((count < 0) and (saved_errno IS EPIPE) and (not already_pending)) {
      timespec timeout = {};
      sigtimedwait(&blocked, nullptr, &timeout);
   }

   pthread_sigmask(SIG_SETMASK, &previous, nullptr);
   errno = saved_errno;
   return count;
}

//********************************************************************************************************************
// FD callback for a transfer pipe.  Outgoing transfers write as much of the payload as the pipe accepts, and incoming
// transfers read until the end of the stream, up to a limit of 128 MiB.  The transfer is finished on completion or
// failure, otherwise it waits for the next callback.

static void transfer_ready(HOSTHANDLE, APTR Data)
{
   auto transfer = (WaylandTransfer *)Data;

   if (transfer->Sending) {
      while (transfer->Offset < transfer->Data.size()) {
         auto count = write_without_sigpipe(transfer->FD, transfer->Data.data() + transfer->Offset,
            transfer->Data.size() - transfer->Offset);
         if (count > 0) transfer->Offset += size_t(count);
         else if ((count < 0) and (errno IS EINTR)) continue;
         else if ((count < 0) and ((errno IS EAGAIN) or (errno IS EWOULDBLOCK))) return;
         else { finish_transfer(transfer, false); return; }
      }
      finish_transfer(transfer, true);
   }
   else {
      std::array<char, 16384> buffer;
      while (true) {
         auto count = read(transfer->FD, buffer.data(), buffer.size());
         if (count > 0) {
            constexpr size_t max_transfer = 128 * 1024 * 1024;
            if (size_t(count) > max_transfer - transfer->Data.size()) {
               finish_transfer(transfer, false);
               return;
            }
            transfer->Data.append(buffer.data(), size_t(count));
         }
         else if (count IS 0) { finish_transfer(transfer, true); return; }
         else if (errno IS EINTR) continue;
         else if ((errno IS EAGAIN) or (errno IS EWOULDBLOCK)) return;
         else { finish_transfer(transfer, false); return; }
      }
   }
}

//********************************************************************************************************************
// Makes the transfer's pipe non-blocking and registers it with the event loop.  On failure the caller retains ownership
// of the transfer.

static bool watch_transfer(WaylandTransfer *Transfer)
{
   if (fcntl(Transfer->FD, F_SETFL, fcntl(Transfer->FD, F_GETFL) | O_NONBLOCK) < 0) return false;
   if (RegisterFD(Transfer->FD, Transfer->Sending ? RFD::WRITE : RFD::READ,
         transfer_ready, Transfer) != ERR::Okay) return false;

   Transfer->Owner->Transfers.push_back(Transfer);
   return true;
}

//********************************************************************************************************************
// Outbound dragging.  The Display module hands an internal drag to startDragExport() when the pointer leaves the
// pressed window with the primary button held, which the compositor reports as motion outside the surface during the
// implicit grab.  The export offers copies of the payload only.
//
// The compositor consumes the button release that ends its drag, so the release is synthesised for the Display module
// when the drop is performed or the drag is cancelled.  A drop over one of our own windows is delivered internally
// without reading the offer; the source result that follows it is ignored.

static constexpr std::string_view EXPORT_MARKER = "application/x-kotuku-drag-export;pid=";
static constexpr int EXPORT_TIMEOUT = 10; // Seconds to wait for the recipient's result after a drop

//********************************************************************************************************************
// Returns the prefix of the private MIME types that identify this process's exports.

static std::string export_marker_prefix()
{
   return std::string(EXPORT_MARKER) + std::to_string(getpid()) + ";";
}

//********************************************************************************************************************
// True if the offer carries a marker from this process, whether or not its export is still active.

static bool own_offer(const WaylandOffer *Offer)
{
   const auto prefix = export_marker_prefix();
   for (auto &mime : Offer->Mimes) if (mime.starts_with(prefix)) return true;
   return false;
}

//********************************************************************************************************************
// True if the offer belongs to the active export and the drag is still hovering.

static bool exporting_offer(WaylandDriver::State *State, const WaylandOffer *Offer)
{
   auto &e = State->Export;
   if ((e.Stage != WaylandExport::Phase::Hover) or e.Cancelled or e.OwnDrop) return false;
   return std::find(Offer->Mimes.begin(), Offer->Mimes.end(), e.Marker) != Offer->Mimes.end();
}

//********************************************************************************************************************
// Releases every resource of the export without reporting a result.

static void reset_export(WaylandDriver::State *State)
{
   auto &e = State->Export;

   if (e.Source) wl_data_source_destroy(e.Source);
   if (e.TimerFD >= 0) {
      DeregisterFD(e.TimerFD);
      ::close(e.TimerFD);
   }
   e = WaylandExport();
   if (State->Connection) wl_display_flush(State->Connection);
}

//********************************************************************************************************************
// Ends the export and reports its result to the Display module.

static void report_export(WaylandDriver::State *State, bool Accepted)
{
   reset_export(State);
   if (State->Callbacks and State->Callbacks->DragExportEnded) State->Callbacks->DragExportEnded(Accepted);
}

//********************************************************************************************************************
// Synthesises the release of the primary button that started the export.

static void release_export_button(WaylandDriver::State *State)
{
   if (State->Callbacks and State->Callbacks->ButtonInput) State->Callbacks->ButtonInput(0x0001, false);
}

//********************************************************************************************************************
// Timer callback that ends a dropped export if the recipient has not reported a result within EXPORT_TIMEOUT seconds.

static void export_timeout(HOSTHANDLE, APTR Data)
{
   auto state = (WaylandDriver::State *)Data;
   auto &e = state->Export;
   uint64_t expirations;

   if ((e.TimerFD >= 0) and (::read(e.TimerFD, &expirations, sizeof(expirations)) < 0) and (errno IS EAGAIN)) return;
   if (e.Stage != WaylandExport::Phase::Dropped) return;

   // A recipient that read the payload without reporting a result is assumed to have accepted it.

   report_export(state, e.Served);
}

//********************************************************************************************************************
// Starts the timer that bounds the wait for the recipient's result after a drop.  If the timer cannot be created, the
// export relies on the source events alone.

static void arm_export_timeout(WaylandDriver::State *State)
{
   auto &e = State->Export;
   if (e.TimerFD >= 0) return;

   e.TimerFD = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
   if (e.TimerFD < 0) return;

   itimerspec timeout = {};
   timeout.it_value.tv_sec = EXPORT_TIMEOUT;
   if ((timerfd_settime(e.TimerFD, 0, &timeout, nullptr) < 0) or
         (RegisterFD(e.TimerFD, RFD::READ, export_timeout, State) != ERR::Okay)) {
      ::close(e.TimerFD);
      e.TimerFD = -1;
   }
}

static void export_target(void *, wl_data_source *, const char *) { }

//********************************************************************************************************************
// A recipient has requested the export payload.  The payload for the requested MIME type is copied to an outgoing
// transfer, and the FD is closed if the type is not offered.  Any request other than for the marker means that the
// payload has been served.

static void export_send(void *Data, wl_data_source *Source, const char *Mime, int FD)
{
   auto state = (WaylandDriver::State *)Data;
   auto &e = state->Export;
   const std::string *payload = nullptr;

   if ((e.Source IS Source) and Mime) {
      const std::string_view mime(Mime);
      if (mime IS "text/uri-list") payload = e.Uris.empty() ? nullptr : &e.Uris;
      else if ((mime IS "text/plain;charset=utf-8") or (mime IS "text/plain") or (mime IS "UTF8_STRING")) {
         payload = e.HasText ? &e.Text : nullptr;
      }
      else if (mime IS e.Marker) payload = &e.Marker;
   }

   auto transfer = payload ? new(std::nothrow) WaylandTransfer : nullptr;
   if (not transfer) { ::close(FD); return; }

   if (payload != &e.Marker) e.Served = true;
   transfer->Owner = state;
   transfer->FD = FD;
   transfer->Data = *payload; // The transfer outlives the export if the recipient reads slowly
   transfer->Sending = true;
   if (not watch_transfer(transfer)) { ::close(FD); delete transfer; return; }
   transfer_ready(FD, transfer);
}

//********************************************************************************************************************
// Sent when no recipient accepts the release, when the compositor or recipient ends the drag, or when the drag cannot
// start.  In the last case the compositor never took the pointer, so the button is still held in our window.

static void export_cancelled(void *Data, wl_data_source *Source)
{
   auto state = (WaylandDriver::State *)Data;
   auto &e = state->Export;

   if ((e.Source != Source) or e.OwnDrop) return;
   if (e.Stage IS WaylandExport::Phase::Dropped) { report_export(state, false); return; }
   if (e.Cancelled) return;

   e.Cancelled = true;
   e.OwnWindow = nullptr;
   if (not e.Granted) return;
   release_export_button(state);

   // The release ends the export unless the internal session had already ended.

   if (e.Source IS Source) reset_export(state);
}

//********************************************************************************************************************
// The drag was released over a target that accepted it.  The button release is synthesised for the Display module and
// the export waits for the recipient's result.

static void export_drop_performed(void *Data, wl_data_source *Source)
{
   auto state = (WaylandDriver::State *)Data;
   auto &e = state->Export;
   if ((e.Source != Source) or (e.Stage != WaylandExport::Phase::Hover) or e.OwnDrop or e.Cancelled) return;

   e.Stage = WaylandExport::Phase::Dropped;
   e.OwnWindow = nullptr;
   arm_export_timeout(state);
   release_export_button(state);
}

//********************************************************************************************************************
// The recipient has completed the drop.  The export succeeds if the final action was a copy.

static void export_finished(void *Data, wl_data_source *Source)
{
   auto state = (WaylandDriver::State *)Data;
   auto &e = state->Export;
   if ((e.Source != Source) or (e.Stage != WaylandExport::Phase::Dropped)) return;
   report_export(state, e.Action IS WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}

//********************************************************************************************************************
// Records the action that the compositor has selected for the export.

static void export_action(void *Data, wl_data_source *Source, uint32_t Action)
{
   auto state = (WaylandDriver::State *)Data;
   if (state->Export.Source IS Source) state->Export.Action = Action;
}

static const wl_data_source_listener export_source_listener = {
   export_target, export_send, export_cancelled, export_drop_performed, export_finished, export_action
};

//********************************************************************************************************************
// Moves the internal session just outside the nearest edge of a window that the drag has left, so its hover ends as
// it would for real pointer motion.

static void leave_own_window(WaylandDriver::State *State, WaylandWindow *Window, double X, double Y)
{
   State->Export.OwnWindow = nullptr;
   auto callbacks = State->Callbacks;
   if ((not callbacks) or (not Window->SurfaceID) or (not State->Windows.contains(Window))) return;

   const auto surface_id = Window->SurfaceID;
   callbacks->Crossing(surface_id, false, X, Y);

   const double left = X, right = Window->Width - X, top = Y, bottom = Window->Height - Y;
   const double nearest = std::min({ left, right, top, bottom });
   if (nearest IS left) X = -1;
   else if (nearest IS right) X = Window->Width;
   else if (nearest IS top) Y = -1;
   else Y = Window->Height;
   if (callbacks->Movement) callbacks->Movement(surface_id, X, Y, false);
}

//********************************************************************************************************************
// Records a MIME type advertised by an offer.

static void offer_mime(void *Data, wl_data_offer *, const char *Mime)
{
   auto offer = (WaylandOffer *)Data;
   if (Mime) offer->Mimes.emplace_back(Mime);
}

//********************************************************************************************************************
// True if the seat and offer still exist and the offer is the seat's current drag.  Callbacks into the Display module
// can destroy either, so this is checked once they return.

static bool active_drag(WaylandDriver::State *State, WaylandSeat *Seat, WaylandOffer *Offer)
{
   if (std::find(State->Seats.begin(), State->Seats.end(), Seat) IS State->Seats.end()) return false;
   if (std::find(State->Offers.begin(), State->Offers.end(), Offer) IS State->Offers.end()) return false;
   return Seat->DragOffer IS Offer;
}

//********************************************************************************************************************
// Records the actions supported by the drag source and refreshes the drag feedback.

static void offer_actions(void *Data, wl_data_offer *, uint32_t Actions)
{
   auto offer = (WaylandOffer *)Data;
   auto state = offer->Owner;
   const auto seats = state->Seats;

   offer->Actions = Actions;
   if (offer->Own) return;

   for (auto seat : seats) {
      if (active_drag(state, seat, offer) and not offer->PendingDrop) drag_feedback(seat);
   }
}

//********************************************************************************************************************
// Records the action selected by the compositor.  Only copies are supported, so any other action ends the hover
// feedback.

static void offer_action(void *Data, wl_data_offer *, uint32_t Action)
{
   auto offer = (WaylandOffer *)Data;
   auto state = offer->Owner;
   const auto seats = state->Seats;

   offer->Action = Action;
   offer->ActionKnown = true;
   if (offer->Own) return;

   for (auto seat : seats) {
      if (not active_drag(state, seat, offer) or offer->PendingDrop) continue;
      if (Action IS WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) drag_feedback(seat, false);
      else if (state->Callbacks and state->Callbacks->DragLeave) state->Callbacks->DragLeave();
   }
}

static const wl_data_offer_listener data_offer_listener = { offer_mime, offer_actions, offer_action };

//********************************************************************************************************************
// Destroys an offer and removes every reference to it.

static void dispose_offer(WaylandOffer *Offer)
{
   if (not Offer) return;

   for (auto seat : Offer->Owner->Seats) {
      if (seat->SelectionOffer IS Offer) seat->SelectionOffer = nullptr;
      if (seat->DragOffer IS Offer) seat->DragOffer = nullptr;
   }

   auto &offers = Offer->Owner->Offers;
   offers.erase(std::remove(offers.begin(), offers.end(), Offer), offers.end());
   wl_data_offer_destroy(Offer->Handle);
   delete Offer;
}

//********************************************************************************************************************
// Returns the preferred MIME type that the offer supports, or null if nothing can be imported.  File lists are
// preferred to text.

static const char *preferred_mime(WaylandOffer *Offer)
{
   for (auto name : { "text/uri-list", "text/plain;charset=utf-8", "text/plain" }) {
      if (std::find(Offer->Mimes.begin(), Offer->Mimes.end(), name) != Offer->Mimes.end()) return name;
   }
   return nullptr;
}

//********************************************************************************************************************
// Requests the offer's content in its preferred MIME type, which is read asynchronously through a pipe.  For drops, the
// transfer takes ownership of the offer and passes the drop details to the Display module with the data.

static bool receive_offer(WaylandSeat *Seat, WaylandOffer *Offer, bool Dropped, const NativeDrop &Drop = {})
{
   if (not Offer) return false;
   auto mime = preferred_mime(Offer);
   if (not mime) return false;

   int pipes[2];
   if (pipe2(pipes, O_CLOEXEC) < 0) return false;

   auto transfer = new(std::nothrow) WaylandTransfer;
   if (not transfer) { close(pipes[0]); close(pipes[1]); return false; }

   transfer->Owner = Seat->Owner;
   transfer->FD = pipes[0];
   transfer->Mime = mime;
   transfer->Dropped = Dropped;
   transfer->Drop = Drop;
   transfer->SelectionGeneration = Seat->Owner->SelectionGeneration;
   if (Dropped) transfer->Offer = Offer;
   if (not watch_transfer(transfer)) {
      close(pipes[0]); close(pipes[1]); delete transfer; return false;
   }

   if (Dropped) Offer->PendingDrop = true;
   wl_data_offer_receive(Offer->Handle, mime, pipes[1]);
   close(pipes[1]);
   wl_display_flush(Seat->Owner->Connection);
   return true;
}

//********************************************************************************************************************
// A new offer has been introduced ahead of the enter or selection event that uses it.  Its MIME types follow in offer
// events.

static void data_offer(void *Data, wl_data_device *, wl_data_offer *Handle)
{
   auto seat = (WaylandSeat *)Data;
   auto offer = new(std::nothrow) WaylandOffer;
   if (not offer) { wl_data_offer_destroy(Handle); return; }

   offer->Owner = seat->Owner;
   offer->Handle = Handle;
   seat->Owner->Offers.push_back(offer);
   wl_data_offer_add_listener(Handle, &data_offer_listener, offer);
}

//********************************************************************************************************************
// Returns the offer that wraps a protocol handle, or null.

static WaylandOffer *find_offer(WaylandSeat *Seat, wl_data_offer *Handle)
{
   for (auto offer : Seat->Owner->Offers) if (offer->Handle IS Handle) return offer;
   return nullptr;
}

//********************************************************************************************************************
// True if no modifier that requests a move or link (Shift or Alt) is held.

static bool copy_modifiers_allowed(WaylandSeat *Seat)
{
   if ((key_qualifiers(Seat) & (KQ::SHIFT|KQ::ALT)) != KQ::NIL) return false;

   // A modifier may have been pressed in the source application, before this client received keyboard focus.

   if (Seat->KeyState) {
      for (auto name : { XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_ALT }) {
         if (xkb_state_mod_name_is_active(Seat->KeyState, name, XKB_STATE_MODS_EFFECTIVE) > 0) return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Asks the Display module whether the target under the pointer will accept the offer, and shows the result.  The
// acceptance of the MIME type is reported to the compositor if Negotiate is true or the offer is refused.  Returns true
// if the offer is accepted.

static bool drag_feedback(WaylandSeat *Seat, bool Negotiate)
{
   auto state = Seat->Owner;
   const auto serial = Seat->DragSerial;
   auto offer = Seat->DragOffer;
   auto window = Seat->DragWindow;
   auto callbacks = Seat->Owner->Callbacks;
   const auto mime = offer ? preferred_mime(offer) : nullptr;
   bool accepted = mime and window and window->SurfaceID and window_frame(window).contains(Seat->X, Seat->Y) and
      ((offer->Actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0) and
      copy_modifiers_allowed(Seat);

   if (accepted and callbacks and callbacks->DragOver) {
      const char datatype = std::string_view(mime) IS "text/uri-list" ? char(DATA::FILE) : char(DATA::TEXT);
      accepted = callbacks->DragOver(window->SurfaceID, Seat->X, Seat->Y, &datatype, 1);
   }
   else accepted = false;

   if (not accepted and callbacks and callbacks->DragLeave) callbacks->DragLeave();

   // Widget callbacks can close the window, ending and destroying this offer synchronously.

   if (not active_drag(state, Seat, offer)) return false;
   if ((Seat->DragSerial != serial) or (Seat->DragWindow != window)) return false;

   if (offer and (Negotiate or not accepted))
      wl_data_offer_accept(offer->Handle, serial, accepted ? mime : nullptr);

   // Keep negotiating MIME acceptance when entering a new target, but do not show copy feedback while the
   // compositor's current action refuses it (for example, Shift held in the source application).

   if (accepted and offer->ActionKnown and (offer->Action != WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) and
         callbacks and callbacks->DragLeave) callbacks->DragLeave();
   return accepted;
}

//********************************************************************************************************************
// Our own export is routed to the internal session, which provides the hover feedback.  The offer is accepted over
// any of our windows, so that a release there is reported as a drop, and refused if it belongs to an export that has
// ended.

static void enter_own_offer(WaylandSeat *Seat, uint32_t Serial)
{
   auto state = Seat->Owner;
   auto &e = state->Export;
   auto offer = Seat->DragOffer;
   auto window = Seat->DragWindow;

   offer->Own = true;
   const bool route = exporting_offer(state, offer) and window and window->SurfaceID and
      (HOSTWINDOW(window) != e.Exclude);

   if (wl_data_offer_get_version(offer->Handle) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
      wl_data_offer_set_actions(offer->Handle, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
         WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
   wl_data_offer_accept(offer->Handle, Serial, route ? e.Marker.c_str() : nullptr);
   wl_display_flush(state->Connection);

   if ((not route) or (not state->Callbacks)) return;
   e.OwnWindow = window;
   state->Callbacks->Crossing(window->SurfaceID, true, Seat->X, Seat->Y);
   if ((e.OwnWindow IS window) and state->Windows.contains(window) and state->Callbacks->Movement)
      state->Callbacks->Movement(window->SurfaceID, Seat->X, Seat->Y, false);
}

//********************************************************************************************************************
// A drag has entered one of our windows.  Offers from our own exports are routed to the internal session.  Other offers
// are limited to copying and the hover feedback is requested from the Display module.

static void data_enter(void *Data, wl_data_device *, uint32_t Serial, wl_surface *Surface,
   wl_fixed_t X, wl_fixed_t Y, wl_data_offer *Handle)
{
   auto seat = (WaylandSeat *)Data;
   seat->DragOffer = find_offer(seat, Handle);
   seat->DragWindow = surface_window(seat->Owner, Surface);
   seat->DragSerial = Serial;
   const auto frame = seat->DragWindow ? window_frame(seat->DragWindow) : WaylandFrame(0, 0, false);
   seat->X = wl_fixed_to_double(X) - frame.Border;
   seat->Y = wl_fixed_to_double(Y) - frame.Top;

   if (seat->DragOffer and own_offer(seat->DragOffer)) {
      enter_own_offer(seat, Serial);
      return;
   }

   if (seat->DragOffer) {
      if (wl_data_offer_get_version(Handle) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
         wl_data_offer_set_actions(Handle, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
   }

   if (seat->DragWindow and seat->DragWindow->SurfaceID and seat->Owner->Callbacks) {
      seat->Owner->Callbacks->Crossing(seat->DragWindow->SurfaceID, true, seat->X, seat->Y);
   }

   drag_feedback(seat);
}

//********************************************************************************************************************
// The drag has left our window, or the drop has completed.  The hover state is cleared, the offer is disposed unless a
// drop is still reading it, and the Display module is told that the hover has ended.

static void data_leave(void *Data, wl_data_device *)
{
   auto seat = (WaylandSeat *)Data;
   auto callbacks = seat->Owner->Callbacks;
   const auto surface_id = seat->DragWindow ? seat->DragWindow->SurfaceID : 0;
   const auto x = seat->X, y = seat->Y;
   auto offer  = seat->DragOffer;
   auto window = seat->DragWindow;

   // Detach before notifying widgets: a dragLeave callback can itself close the window or remove the seat.

   seat->DragWindow = nullptr;
   seat->DragSerial = 0;
   seat->DragOffer  = nullptr;

   if (offer and offer->Own) {
      dispose_offer(offer);
      if (window and (seat->Owner->Export.OwnWindow IS window)) leave_own_window(seat->Owner, window, x, y);
      return;
   }

   if (offer and not offer->PendingDrop) dispose_offer(offer);
   if (callbacks and callbacks->DragLeave) callbacks->DragLeave();
   if (surface_id and callbacks) callbacks->Crossing(surface_id, false, x, y);
}

//********************************************************************************************************************
// The drag has moved within our window.  The position is converted to content coordinates and the hover feedback is
// refreshed.

static void data_motion(void *Data, wl_data_device *, uint32_t, wl_fixed_t X, wl_fixed_t Y)
{
   auto seat = (WaylandSeat *)Data;
   const auto frame = seat->DragWindow ? window_frame(seat->DragWindow) : WaylandFrame(0, 0, false);
   seat->X = wl_fixed_to_double(X) - frame.Border;
   seat->Y = wl_fixed_to_double(Y) - frame.Top;

   if (seat->DragOffer and seat->DragOffer->Own) {
      auto state = seat->Owner;
      auto window = seat->DragWindow;
      if (window and (state->Export.OwnWindow IS window) and state->Callbacks and state->Callbacks->Movement)
         state->Callbacks->Movement(window->SurfaceID, seat->X, seat->Y, false);
      return;
   }

   drag_feedback(seat);
}

//********************************************************************************************************************
// The drag was released over our window.  If the Display module accepts the drop, the offer's content is read and
// delivered asynchronously.  Drops of our own exports are delivered through the internal session.

static void data_drop(void *Data, wl_data_device *)
{
   auto seat = (WaylandSeat *)Data;
   auto offer = seat->DragOffer;
   auto callbacks = seat->Owner->Callbacks;

   if (offer and offer->Own) {
      // The internal session delivers the drop; the offer is never read.
      auto state = seat->Owner;
      auto &e = state->Export;
      const bool route = e.OwnWindow and (e.OwnWindow IS seat->DragWindow) and exporting_offer(state, offer);

      seat->DragOffer = nullptr;
      seat->DragWindow = nullptr;
      seat->DragSerial = 0;
      dispose_offer(offer);

      e.OwnWindow = nullptr;
      if (not route) return;

      e.OwnDrop = true;
      const auto source = e.Source;
      release_export_button(state);
      if (state->Export.Source IS source) reset_export(state); // The internal session had already ended
      return;
   }

   if (offer and offer->PendingDrop) return;

   // The final compositor-selected action must be honoured; moves and links are not supported.

   if (offer and seat->DragWindow and callbacks and callbacks->DragStart and
         window_frame(seat->DragWindow).contains(seat->X, seat->Y) and
         ((offer->Actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0) and
         copy_modifiers_allowed(seat) and
         ((wl_data_offer_get_version(offer->Handle) < WL_DATA_OFFER_ACTION_SINCE_VERSION) or
          (offer->Action IS WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY))) {
      NativeDrop drop;
      drop.SurfaceID = seat->DragWindow->SurfaceID;
      drop.X = seat->X;
      drop.Y = seat->Y;
      drop.Session = ++seat->Owner->DropGeneration;
      if (callbacks->DragStart(drop)) receive_offer(seat, offer, true, drop);
   }

   if (callbacks and callbacks->DragLeave) callbacks->DragLeave();
}

//********************************************************************************************************************
// The clipboard selection has changed.  The new content is read immediately so that it is available to the clipboard.
// A cleared selection is reported as a loss of the clipboard unless this client owns it.

static void data_selection(void *Data, wl_data_device *, wl_data_offer *Handle)
{
   auto seat = (WaylandSeat *)Data;
   if (seat->SelectionOffer and (seat->SelectionOffer->Handle IS Handle)) return;

   seat->Owner->SelectionGeneration++;
   if (seat->SelectionOffer) dispose_offer(seat->SelectionOffer);
   seat->SelectionOffer = find_offer(seat, Handle);
   if (seat->SelectionOffer) {
      if (seat->Owner->Callbacks and seat->Owner->Callbacks->ClipboardLost)
         seat->Owner->Callbacks->ClipboardLost();
      receive_offer(seat, seat->SelectionOffer, false);
   }
   else if (not seat->SelectionSource and seat->Owner->Callbacks and seat->Owner->Callbacks->ClipboardLost)
      seat->Owner->Callbacks->ClipboardLost();
}

static const wl_data_device_listener data_device_listener = {
   data_offer, data_enter, data_leave, data_motion, data_drop, data_selection
};

static void source_target(void *, wl_data_source *, const char *) { }

//********************************************************************************************************************
// A recipient has requested our clipboard content.  The payload is written to the FD if the requested type matches the
// type offered.

static void source_send(void *Data, wl_data_source *, const char *Mime, int FD)
{
   auto source = (WaylandSource *)Data;
   auto transfer = new(std::nothrow) WaylandTransfer;
   if ((not transfer) or (not Mime) or ((source->Mime IS "text/uri-list") and
         (std::strcmp(Mime, "text/uri-list") != 0)) or ((source->Mime != "text/uri-list") and
         (std::strcmp(Mime, "text/plain") != 0) and (std::strcmp(Mime, "text/plain;charset=utf-8") != 0))) {
      close(FD); delete transfer; return;
   }

   transfer->Owner = source->Owner;
   transfer->FD = FD;
   transfer->Data = source->Payload;
   transfer->Sending = true;
   if (not watch_transfer(transfer)) { close(FD); delete transfer; return; }
   transfer_ready(FD, transfer);
}

//********************************************************************************************************************
// Our clipboard selection has been replaced by another client.  The loss of the clipboard is reported and the source is
// destroyed.

static void source_cancelled(void *Data, wl_data_source *)
{
   auto source = (WaylandSource *)Data;
   for (auto seat : source->Owner->Seats) if (seat->SelectionSource IS source) {
      seat->SelectionSource = nullptr;
      seat->PublishedGeneration = 0;
      source->Owner->ClipboardGeneration = 0;
      if (source->Owner->Callbacks and source->Owner->Callbacks->ClipboardLost)
         source->Owner->Callbacks->ClipboardLost();
   }

   auto &sources = source->Owner->Sources;
   sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
   wl_data_source_destroy(source->Handle);
   delete source;
}

static void source_drop(void *, wl_data_source *) { }
static void source_finished(void *, wl_data_source *) { }
static void source_action(void *, wl_data_source *, uint32_t) { }

static const wl_data_source_listener data_source_listener = {
   source_target, source_send, source_cancelled, source_drop, source_finished, source_action
};

//********************************************************************************************************************
// Publishes the clipboard content as the seat's selection.  Wayland requires the serial of a recent input event, so
// publication waits until the seat has one and is repeated for each new clipboard generation.

static void publish_selection(WaylandSeat *Seat)
{
   auto state = Seat->Owner;
   if ((not state->DataManager) or (not Seat->DataDevice) or
         (not state->ClipboardGeneration) or (Seat->PublishedGeneration IS state->ClipboardGeneration) or
         (not Seat->SelectionSerial)) return;

   auto source = new(std::nothrow) WaylandSource;
   if (not source) return;

   source->Owner = state;
   source->Mime = state->ClipboardIsFiles ? "text/uri-list" : "text/plain;charset=utf-8";
   source->Payload = state->ClipboardIsFiles ? state->ClipboardUris : state->ClipboardText;
   source->Handle = wl_data_device_manager_create_data_source(state->DataManager);
   if (not source->Handle) { delete source; return; }

   wl_data_source_add_listener(source->Handle, &data_source_listener, source);
   wl_data_source_offer(source->Handle, source->Mime.c_str());
   if (not state->ClipboardIsFiles) wl_data_source_offer(source->Handle, "text/plain");
   state->Sources.push_back(source);
   Seat->SelectionSource = source;
   Seat->PublishedGeneration = state->ClipboardGeneration;
   wl_data_device_set_selection(Seat->DataDevice, source->Handle, Seat->SelectionSerial);
   wl_display_flush(state->Connection);
}

//********************************************************************************************************************
// The compositor has released a window buffer.  A pending frame is presented, and retired buffers are destroyed.

static void release_buffer(void *Data, wl_buffer *)
{
   auto buffer = (WaylandBuffer *)Data;
   buffer->Busy = false;
   if (buffer->Window) try_present(buffer->Window);
   else {
      auto &retired = buffer->Owner->Retired;
      retired.erase(std::remove(retired.begin(), retired.end(), buffer), retired.end());
      dispose_buffer(buffer);
   }
}

static const wl_buffer_listener buffer_listener = { release_buffer };

//********************************************************************************************************************
// Destroys a window buffer and unmaps its pixels.

static void dispose_buffer(WaylandBuffer *Buffer)
{
   if (Buffer->Handle) wl_buffer_destroy(Buffer->Handle);
   if (Buffer->Pixels) munmap(Buffer->Pixels, Buffer->Length);
   delete Buffer;
}

//********************************************************************************************************************
// The compositor has released a custom cursor buffer.  A retired buffer is destroyed once it is no longer in use.

static void cursor_buffer_release(void *Data, wl_buffer *)
{
   auto buffer = (CursorBuffer *)Data;
   buffer->Busy = false;
   if (not buffer->Retired) return;

   auto &buffers = buffer->Owner->CursorBuffers;
   buffers.erase(std::remove(buffers.begin(), buffers.end(), buffer), buffers.end());
   wl_buffer_destroy(buffer->Handle);
   delete buffer;
}

//********************************************************************************************************************
// Marks a custom cursor buffer for destruction once the compositor has released it.

static void retire_cursor_buffer(CursorBuffer *Buffer)
{
   if (not Buffer) return;
   Buffer->Retired = true;
   if (not Buffer->Busy) cursor_buffer_release(Buffer, Buffer->Handle);
}

static const wl_buffer_listener cursor_buffer_listener = { cursor_buffer_release };

//********************************************************************************************************************
// Creates a shared memory buffer for the window and its decorations at the current scale.  Composited windows use ARGB
// pixels, other windows use XRGB.

static WaylandBuffer * make_buffer(WaylandWindow *Window)
{
   const auto frame = window_frame(Window);
   const auto width = (frame.Width * Window->Scale120 + 119) / 120;
   const auto height = (frame.Height * Window->Scale120 + 119) / 120;

   // Keep the existing 8192-pixel content budget plus room for decorations at the maximum scale of eight.

   if ((width <= 0) or (height <= 0) or (width > 8448) or (height > 8448)) return nullptr;

   const size_t length = size_t(width) * size_t(height) * 4;
   int fd = memfd_create("kotuku-wayland", MFD_CLOEXEC);
   if (fd < 0) return nullptr;
   if (ftruncate(fd, off_t(length)) < 0) { close(fd); return nullptr; }
   auto pixels = mmap(nullptr, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
   if (pixels IS MAP_FAILED) { close(fd); return nullptr; }

   auto pool = wl_shm_create_pool(Window->Owner->Shm, fd, int(length));
   if (not pool) { munmap(pixels, length); close(fd); return nullptr; }
   const bool alpha = Window->Display and ((Window->Display->Flags & SCR::COMPOSITE) != SCR::NIL);
   auto handle = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
      alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);
   if (not handle) { munmap(pixels, length); return nullptr; }

   auto buffer = new(std::nothrow) WaylandBuffer;
   if (not buffer) { wl_buffer_destroy(handle); munmap(pixels, length); return nullptr; }

   buffer->Owner = Window->Owner;
   buffer->Window = Window;
   buffer->Handle = handle;
   buffer->Pixels = pixels;
   buffer->Length = length;
   buffer->Width = width;
   buffer->Height = height;
   wl_buffer_add_listener(handle, &buffer_listener, buffer);

   return buffer;
}

//********************************************************************************************************************
// The compositor is ready for a new frame.

static void frame_done(void *Data, wl_callback *Callback, uint32_t)
{
   auto window = (WaylandWindow *)Data;
   if (window->Frame IS Callback) window->Frame = nullptr;
   wl_callback_destroy(Callback);
   try_present(window);
}

static const wl_callback_listener frame_listener = { frame_done };

//********************************************************************************************************************
// Removes the window's buffers that do not match its current size and scale.  Buffers still held by the compositor are
// retired until they are released.

static void retire_obsolete(WaylandWindow *Window)
{
   const auto frame = window_frame(Window);
   auto &buffers = Window->Buffers;
   for (auto it = buffers.begin(); it != buffers.end();) {
      auto buffer = *it;
      if ((buffer->Width IS (frame.Width * Window->Scale120 + 119) / 120) and
            (buffer->Height IS (frame.Height * Window->Scale120 + 119) / 120)) { ++it; continue; }
      buffer->Window = nullptr;
      if (buffer->Busy) Window->Owner->Retired.push_back(buffer);
      else dispose_buffer(buffer);
      it = buffers.erase(it);
   }
}

//********************************************************************************************************************
// Presents the staged window content when the compositor is ready for it.  Decorations are composed around the content
// and the result is scaled into a free buffer.  Nothing is presented while the window is hidden, unconfigured, or
// awaiting a frame callback, or while the staged content does not match the window size.

static void try_present(WaylandWindow *Window)
{
   if ((not Window->Visible) or (Window->Lifecycle IS WaylandWindow::Phase::Pending) or
         (Window->Lifecycle IS WaylandWindow::Phase::Closing) or Window->Frame or
         ((not Window->Dirty) and (not Window->DecorDirty))) return;
   if ((Window->StageWidth != Window->Width) or (Window->StageHeight != Window->Height)) return;
   if ((not Window->DecorDirty) and (Window->Staged IS Window->Presented)) { Window->Dirty = false; return; }

   retire_obsolete(Window);
   WaylandBuffer *buffer = nullptr;
   for (auto item : Window->Buffers) if (not item->Busy) { buffer = item; break; }
   if ((not buffer) and (Window->Buffers.size() < 3)) {
      buffer = make_buffer(Window);
      if (buffer) Window->Buffers.push_back(buffer);
   }
   if (not buffer) return;

   const auto frame = window_frame(Window);
   auto target = (uint32_t *)buffer->Pixels;
   auto source = (const uint32_t *)Window->Staged.data();
   for (int y = 0; y < buffer->Height; y++) {
      const int logical_y = std::min(frame.Height - 1, y * 120 / Window->Scale120);
      const int source_y = logical_y - frame.Top;
      for (int x = 0; x < buffer->Width; x++) {
         const int logical_x = std::min(frame.Width - 1, x * 120 / Window->Scale120);
         const int source_x = logical_x - frame.Border;
         target[size_t(y) * buffer->Width + x] = frame.contains(source_x, source_y) ?
            source[size_t(source_y) * Window->Width + source_x] :
            frame_pixel(frame, logical_x, logical_y, Window->Title);
      }
   }

   Window->DecorDirty = false;
   xdg_surface_set_window_geometry(Window->ShellSurface, 0, 0, frame.Width, frame.Height);
   buffer->Busy = true;
   Window->Presented = Window->Staged;
   Window->Dirty = false;

   if (wl_surface_get_version(Window->Surface) >= WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)
      wl_surface_set_buffer_scale(Window->Surface, Window->Scale);
#ifdef WAYLAND_FRACTIONAL_SCALE
   if (Window->Viewport) wp_viewport_set_destination(Window->Viewport, frame.Width, frame.Height);
#endif
   wl_surface_attach(Window->Surface, buffer->Handle, 0, 0);
   if (wl_surface_get_version(Window->Surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
      wl_surface_damage_buffer(Window->Surface, 0, 0, buffer->Width, buffer->Height);
   else wl_surface_damage(Window->Surface, 0, 0, frame.Width, frame.Height);

   Window->Frame = wl_surface_frame(Window->Surface);
   if (Window->Frame) wl_callback_add_listener(Window->Frame, &frame_listener, Window);
   wl_surface_commit(Window->Surface);
   Window->Lifecycle = WaylandWindow::Phase::Mapped;
   wl_display_flush(Window->Owner->Connection);
}

static void shell_ping(void *, xdg_wm_base *Shell, uint32_t Serial) { xdg_wm_base_pong(Shell, Serial); }

static const xdg_wm_base_listener shell_listener = { shell_ping };

//********************************************************************************************************************
// Records the size and states requested by the compositor for a top-level window.  They are applied by
// surface_configure().

static void top_configure(void *Data, xdg_toplevel *, int32_t Width, int32_t Height, wl_array *States)
{
   auto window = (WaylandWindow *)Data;
   window->PendingWidth = Width;
   window->PendingHeight = Height;
   window->PendingMaximised = false;
   window->PendingFullscreen = false;

   for (size_t i = 0; i < States->size / sizeof(uint32_t); i++) {
      const auto state = ((uint32_t *)States->data)[i];
      if (state IS XDG_TOPLEVEL_STATE_MAXIMIZED) window->PendingMaximised = true;
      if (state IS XDG_TOPLEVEL_STATE_FULLSCREEN) window->PendingFullscreen = true;
   }
}

//********************************************************************************************************************
// The user has asked to close the window.  The request is passed to the Display module.

static void top_close(void *Data, xdg_toplevel *)
{
   auto window = (WaylandWindow *)Data;
   if (window->Lifecycle IS WaylandWindow::Phase::Closing) return;
   if (window->SurfaceID and window->Owner->Callbacks) window->Owner->Callbacks->WindowClose(window->SurfaceID);
}

static const xdg_toplevel_listener top_listener = { top_configure, top_close, nullptr, nullptr };

//********************************************************************************************************************
// Records the size that the compositor has given to a popup.

static void popup_configure(void *Data, xdg_popup *, int32_t, int32_t, int32_t Width, int32_t Height)
{
   auto window = (WaylandWindow *)Data;
   window->PendingWidth = Width;
   window->PendingHeight = Height;
}

//********************************************************************************************************************
// The compositor has dismissed the popup, for example after a click outside of it.  The popup and its children are
// unmapped, the Display module is told to close it, and it is destroyed if it remains.

static void popup_done(void *Data, xdg_popup *)
{
   auto window = (WaylandWindow *)Data;
   auto state = window->Owner;
   const auto surface_id = window->SurfaceID;

   window->Visible = false;
   unmap_children(window);
   if (not state->Windows.contains(window)) return;

   destroy_protocol_window(window);
   if (surface_id and state->Callbacks) {
      state->Callbacks->WindowClose(surface_id);

      // xdg_popup.done cannot be cancelled: the compositor has already dismissed the popup role.

      if (state->Windows.contains(window)) state->Callbacks->WindowDestroyed(surface_id);
   }
}

static const xdg_popup_listener popup_listener = { popup_configure, popup_done, nullptr };

//********************************************************************************************************************
// Applies a configure sequence from the compositor.  The pending size and states take effect, the decoration margins
// are updated, and the Display module is told of any size change before the window is redrawn.

static void surface_configure(void *Data, xdg_surface *ShellSurface, uint32_t Serial)
{
   auto window = (WaylandWindow *)Data;
   auto state = window->Owner;

   xdg_surface_ack_configure(ShellSurface, Serial);
   window->Lifecycle = WaylandWindow::Phase::Configured;
   window->Maximised = window->PendingMaximised;
   window->Fullscreen = window->PendingFullscreen;
   window->ClientDecorations = window->PendingClientDecorations;
   window->DecorDirty = true;

   const auto frame = window_frame(window);
   set_size_hints(window);
   if (window->Display) {
      window->Display->LeftMargin = window->Display->RightMargin = window->Display->BottomMargin = frame.Border;
      window->Display->TopMargin = frame.Top;
   }

   const int width = window->PendingWidth > 0 ? std::max(1, window->PendingWidth - frame.Border * 2) :
      (((not window->Maximised) and (not window->Fullscreen)) ? window->NormalWidth : window->Width);
   const int height = window->PendingHeight > 0 ? std::max(1, window->PendingHeight - frame.Top - frame.Border) :
      (((not window->Maximised) and (not window->Fullscreen)) ? window->NormalHeight : window->Height);
   window->PendingWidth = window->PendingHeight = 0;

   if ((not window->Maximised) and (not window->Fullscreen)) {
      window->NormalWidth = width;
      window->NormalHeight = height;
   }

   if (window->TopLevel and window->Display) {
      if (window->Maximised) window->Display->Flags |= SCR::MAXIMISE;
      else window->Display->Flags &= ~SCR::MAXIMISE;
      if (window->Fullscreen) window->Display->Flags |= SCR::BORDERLESS;
      else window->Display->Flags &= ~SCR::BORDERLESS;
   }

   if ((width != window->Width) or (height != window->Height)) {
      window->Width = width;
      window->Height = height;
      window->Presented.clear();
      retire_obsolete(window);
      if (window->SurfaceID and state->Callbacks) {
         state->Callbacks->WindowResized(window->SurfaceID, 0, 0, width, height,
            0, 0, width, height);
         if (not state->Windows.contains(window)) return;
      }
   }

   if (window->Visible and window->SurfaceID and state->Callbacks)
      state->Callbacks->ExposeRegion(window->SurfaceID, 0, 0, width, height);
   if (not state->Windows.contains(window)) return;
   try_present(window);
}

static const xdg_surface_listener surface_listener = { surface_configure };

//********************************************************************************************************************
// Records whether the compositor requires client-side decorations.

#ifdef WAYLAND_XDG_DECORATION
static void decoration_configure(void *Data, zxdg_toplevel_decoration_v1 *, uint32_t Mode)
{
   ((WaylandWindow *)Data)->PendingClientDecorations = Mode IS ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}

static const zxdg_toplevel_decoration_v1_listener decoration_listener = { decoration_configure };
#endif

//********************************************************************************************************************
// Recalculates the window's buffer scale from the outputs that it occupies, or from its preferred fractional scale.
// The window is redrawn if the scale changes.

static void update_scale(WaylandWindow *Window)
{
   int scale = 1;
   for (auto output : Window->Outputs) scale = std::max(scale, std::min(8, output->Scale));
   if ((not Window->Surface) or
         (wl_surface_get_version(Window->Surface) < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)) scale = 1;
   int scale_120 = scale * 120;
#ifdef WAYLAND_FRACTIONAL_SCALE
   if (Window->Viewport and (Window->PreferredScale120 > 0)) scale_120 = Window->PreferredScale120;
   if (Window->Viewport) scale = 1;
#endif

   if ((Window->Scale IS scale) and (Window->Scale120 IS scale_120)) return;
   Window->Scale = scale;
   Window->Scale120 = scale_120;
   Window->Presented.clear();
   Window->Dirty = true;
   retire_obsolete(Window);

   if (Window->Visible and Window->SurfaceID and Window->Owner->Callbacks)
      Window->Owner->Callbacks->ExposeRegion(Window->SurfaceID, 0, 0, Window->Width, Window->Height);
   if (Window->Owner->Windows.contains(Window)) try_present(Window);
}

//********************************************************************************************************************
// The window has entered an output, which can change its scale.

static void surface_enter(void *Data, wl_surface *, wl_output *Output)
{
   auto window = (WaylandWindow *)Data;
   for (auto output : window->Owner->Outputs) if (output->Handle IS Output) {
      if (std::find(window->Outputs.begin(), window->Outputs.end(), output) IS window->Outputs.end())
         window->Outputs.push_back(output);
      update_scale(window);
      return;
   }
}

//********************************************************************************************************************
// The window has left an output, which can change its scale.

static void surface_leave(void *Data, wl_surface *, wl_output *Output)
{
   auto window = (WaylandWindow *)Data;
   auto &outputs = window->Outputs;
   outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [Output](auto item) {
      return item->Handle IS Output;
   }), outputs.end());
   update_scale(window);
}

static const wl_surface_listener surface_output_listener = { surface_enter, surface_leave };

//********************************************************************************************************************
// The compositor has given the preferred scale for the window, in 120ths.

#ifdef WAYLAND_FRACTIONAL_SCALE
static void fractional_preferred(void *Data, wp_fractional_scale_v1 *, uint32_t Scale)
{
   auto window = (WaylandWindow *)Data;
   window->PreferredScale120 = std::clamp(int(Scale), 120, 960);
   update_scale(window);
}

static const wp_fractional_scale_v1_listener fractional_listener = { fractional_preferred };
#endif

//********************************************************************************************************************
// Records the position, physical size and identification of an output.

static void output_geometry(void *Data, wl_output *, int32_t X, int32_t Y, int32_t PhysicalWidth,
   int32_t PhysicalHeight, int32_t, const char *Make, const char *Model, int32_t)
{
   auto output = (WaylandOutput *)Data;
   output->X = X;
   output->Y = Y;
   output->PhysicalWidth = std::max(0, PhysicalWidth);
   output->PhysicalHeight = std::max(0, PhysicalHeight);
   output->Make = Make ? Make : "";
   output->Model = Model ? Model : "";
}

//********************************************************************************************************************
// Records the current resolution and refresh rate of an output.

static void output_mode(void *Data, wl_output *, uint32_t Flags, int32_t Width, int32_t Height, int32_t Refresh)
{
   auto output = (WaylandOutput *)Data;
   if ((Flags & WL_OUTPUT_MODE_CURRENT) and (Width > 0) and (Height > 0)) {
      output->Width = Width;
      output->Height = Height;
      output->Refresh = Refresh;
   }
}

//********************************************************************************************************************
// The output's properties are complete.  Its resolution becomes the default display size.

static void output_done(void *Data, wl_output *)
{
   auto output = (WaylandOutput *)Data;
   if ((output->Width > 0) and (output->Height > 0)) {
      output->Owner->OutputWidth = output->Width;
      output->Owner->OutputHeight = output->Height;
   }
}

//********************************************************************************************************************
// Records the output's integer scale and updates the windows that occupy it.

static void output_scale(void *Data, wl_output *, int32_t Scale)
{
   auto output = (WaylandOutput *)Data;
   output->Scale = std::max(1, Scale);
   auto windows = std::vector<WaylandWindow *>(output->Owner->Windows.begin(), output->Owner->Windows.end());

   for (auto window : windows)
      if (output->Owner->Windows.contains(window) and
            (std::find(window->Outputs.begin(), window->Outputs.end(), output) != window->Outputs.end()))
         update_scale(window);
}
static const wl_output_listener output_listener = { output_geometry, output_mode, output_done, output_scale };

//********************************************************************************************************************
// Returns the window that owns a protocol surface, or null.

static WaylandWindow *surface_window(WaylandDriver::State *State, wl_surface *Surface)
{
   for (auto window : State->Windows) if (window->Surface IS Surface) return window;
   return nullptr;
}

//********************************************************************************************************************
// Converts an xkb key symbol to a Kotuku key code.

static KEY key_from_sym(xkb_keysym_t Sym)
{
   if ((Sym >= XKB_KEY_a) and (Sym <= XKB_KEY_z)) return KEY(int(KEY::A) + int(Sym - XKB_KEY_a));
   if ((Sym >= XKB_KEY_A) and (Sym <= XKB_KEY_Z)) return KEY(int(KEY::A) + int(Sym - XKB_KEY_A));
   if ((Sym >= XKB_KEY_1) and (Sym <= XKB_KEY_9)) return KEY(int(KEY::ONE) + int(Sym - XKB_KEY_1));
   if ((Sym >= XKB_KEY_KP_0) and (Sym <= XKB_KEY_KP_9)) return KEY(int(KEY::NP_0) + int(Sym - XKB_KEY_KP_0));
   if ((Sym >= XKB_KEY_F1) and (Sym <= XKB_KEY_F17)) return KEY(int(KEY::F1) + int(Sym - XKB_KEY_F1));

   switch (Sym) {
      case XKB_KEY_0: return KEY::ZERO;
      case XKB_KEY_space: return KEY::SPACE;
      case XKB_KEY_grave: return KEY::REVERSE_QUOTE;
      case XKB_KEY_minus: return KEY::MINUS;
      case XKB_KEY_equal: return KEY::EQUALS;
      case XKB_KEY_bracketleft: return KEY::L_SQUARE;
      case XKB_KEY_bracketright: return KEY::R_SQUARE;
      case XKB_KEY_backslash: return KEY::BACK_SLASH;
      case XKB_KEY_semicolon: return KEY::SEMI_COLON;
      case XKB_KEY_apostrophe: return KEY::APOSTROPHE;
      case XKB_KEY_comma: return KEY::COMMA;
      case XKB_KEY_period: return KEY::PERIOD;
      case XKB_KEY_slash: return KEY::SLASH;
      case XKB_KEY_Shift_L: return KEY::L_SHIFT;
      case XKB_KEY_Shift_R: return KEY::R_SHIFT;
      case XKB_KEY_Control_L: return KEY::L_CONTROL;
      case XKB_KEY_Control_R: return KEY::R_CONTROL;
      case XKB_KEY_Alt_L: return KEY::L_ALT;
      case XKB_KEY_Alt_R: return KEY::R_ALT;
      case XKB_KEY_Super_L: return KEY::L_COMMAND;
      case XKB_KEY_Super_R: return KEY::R_COMMAND;
      case XKB_KEY_Caps_Lock: return KEY::CAPS_LOCK;
      case XKB_KEY_Num_Lock: return KEY::NUM_LOCK;
      case XKB_KEY_Scroll_Lock: return KEY::SCR_LOCK;
      case XKB_KEY_BackSpace: return KEY::BACKSPACE;
      case XKB_KEY_Tab: return KEY::TAB;
      case XKB_KEY_Return: return KEY::ENTER;
      case XKB_KEY_KP_Enter: return KEY::NP_ENTER;
      case XKB_KEY_Escape: return KEY::ESCAPE;
      case XKB_KEY_Delete: return KEY::DELETE;
      case XKB_KEY_Insert: return KEY::INSERT;
      case XKB_KEY_Home: return KEY::HOME;
      case XKB_KEY_End: return KEY::END;
      case XKB_KEY_Page_Up: return KEY::PAGE_UP;
      case XKB_KEY_Page_Down: return KEY::PAGE_DOWN;
      case XKB_KEY_Left: return KEY::LEFT;
      case XKB_KEY_Right: return KEY::RIGHT;
      case XKB_KEY_Up: return KEY::UP;
      case XKB_KEY_Down: return KEY::DOWN;
      case XKB_KEY_KP_Add: return KEY::NP_PLUS;
      case XKB_KEY_KP_Subtract: return KEY::NP_MINUS;
      case XKB_KEY_KP_Multiply: return KEY::NP_MULTIPLY;
      case XKB_KEY_KP_Divide: return KEY::NP_DIVIDE;
      case XKB_KEY_KP_Decimal: return KEY::NP_DECIMAL;
      case XKB_KEY_KP_Insert: return KEY::NP_0;
      case XKB_KEY_KP_End: return KEY::NP_1;
      case XKB_KEY_KP_Down: return KEY::NP_2;
      case XKB_KEY_KP_Next: return KEY::NP_3;
      case XKB_KEY_KP_Left: return KEY::NP_4;
      case XKB_KEY_KP_Begin: return KEY::NP_5;
      case XKB_KEY_KP_Right: return KEY::NP_6;
      case XKB_KEY_KP_Home: return KEY::NP_7;
      case XKB_KEY_KP_Up: return KEY::NP_8;
      case XKB_KEY_KP_Prior: return KEY::NP_9;
      case XKB_KEY_KP_Delete: return KEY::NP_DECIMAL;
      case XKB_KEY_F18: return KEY::F18;
      case XKB_KEY_F19: return KEY::F19;
      case XKB_KEY_F20: return KEY::F20;
      default: return KEY::NIL;
   }
}

//********************************************************************************************************************
// Returns the key for a keycode.  The first shift level is used so that the key does not change with the modifiers.

static KEY key_from_code(WaylandSeat *Seat, xkb_keycode_t Code)
{
   if (not Seat->Keymap) return KEY::NIL;

   const xkb_keysym_t *syms = nullptr;
   if (xkb_keymap_key_get_syms_by_level(Seat->Keymap, Code, 0, 0, &syms) > 0)
      return key_from_sym(syms[0]);
   return KEY::NIL;
}

//********************************************************************************************************************
// Returns the current qualifiers.  Held modifier keys are tracked individually so that the left and right keys can be
// told apart.  Lock states are read from xkb.

static KQ key_qualifiers(WaylandSeat *Seat)
{
   if (not Seat->KeyState) return KQ::NIL;

   auto active = [Seat](const char *Name) {
      return xkb_state_mod_name_is_active(Seat->KeyState, Name, XKB_STATE_MODS_EFFECTIVE) > 0;
   };

   KQ flags = KQ::NIL;
   if (Seat->HeldModifiers.contains(KEY::L_SHIFT)) flags |= KQ::L_SHIFT;
   if (Seat->HeldModifiers.contains(KEY::R_SHIFT)) flags |= KQ::R_SHIFT;
   if (Seat->HeldModifiers.contains(KEY::L_CONTROL)) flags |= KQ::L_CONTROL;
   if (Seat->HeldModifiers.contains(KEY::R_CONTROL)) flags |= KQ::R_CONTROL;
   if (Seat->HeldModifiers.contains(KEY::L_ALT)) flags |= KQ::L_ALT;
   if (Seat->HeldModifiers.contains(KEY::R_ALT)) flags |= KQ::R_ALT;
   if (Seat->HeldModifiers.contains(KEY::L_COMMAND)) flags |= KQ::L_COMMAND;
   if (Seat->HeldModifiers.contains(KEY::R_COMMAND)) flags |= KQ::R_COMMAND;
   if (active(XKB_MOD_NAME_CAPS)) flags |= KQ::CAPS_LOCK;
   if (active(XKB_MOD_NAME_NUM)) flags |= KQ::NUM_LOCK;
   return flags;
}

//********************************************************************************************************************
// Cancels key repeat.

static void stop_repeat(WaylandSeat *Seat)
{
   Seat->RepeatKey = UINT32_MAX;
   Seat->RepeatUnicode = 0;
   if (Seat->TimerFD >= 0) {
      itimerspec timer = {};
      timerfd_settime(Seat->TimerFD, 0, &timer, nullptr);
   }
}

//********************************************************************************************************************
// Returns the character produced by a key press once compose sequences (e.g. dead keys) have been applied.  Returns
// zero while a sequence is incomplete.

static uint32_t key_unicode(WaylandSeat *Seat, xkb_keycode_t Code)
{
   if (not Seat->ComposeState) return xkb_state_key_get_utf32(Seat->KeyState, Code);
   xkb_compose_state_feed(Seat->ComposeState, xkb_state_key_get_one_sym(Seat->KeyState, Code));

   switch (xkb_compose_state_get_status(Seat->ComposeState)) {
      case XKB_COMPOSE_COMPOSING:
         return 0;
      case XKB_COMPOSE_COMPOSED: {
         const auto unicode = xkb_keysym_to_utf32(xkb_compose_state_get_one_sym(Seat->ComposeState));
         xkb_compose_state_reset(Seat->ComposeState);
         return unicode;
      }
      case XKB_COMPOSE_CANCELLED:
         xkb_compose_state_reset(Seat->ComposeState);
         [[fallthrough]];
      case XKB_COMPOSE_NOTHING:
         return xkb_state_key_get_utf32(Seat->KeyState, Code);
   }
   return 0;
}

//********************************************************************************************************************
// Timer callback for key repeat.  Missed repeats are delivered, up to a limit of eight.

static void repeat_event(HOSTHANDLE, APTR Data)
{
   auto seat = (WaylandSeat *)Data;
   uint64_t count = 0;
   if (read(seat->TimerFD, &count, sizeof(count)) != sizeof(count)) return;

   if ((seat->RepeatKey IS UINT32_MAX) or (not seat->KeyboardWindow) or (not seat->KeyState) or
         (not seat->Owner->Callbacks)) return;

   const auto code = seat->RepeatKey + 8;
   const auto key = key_from_code(seat, code);
   const auto unicode = seat->RepeatUnicode;
   auto flags = key_qualifiers(seat)|KQ::REPEAT;
   if (((int(key) >= int(KEY::NP_0)) and (int(key) <= int(KEY::NP_DIVIDE))) or (key IS KEY::NP_ENTER))
      flags |= KQ::NUM_PAD;
   for (uint64_t i = 0; i < std::min(count, uint64_t(8)); i++)
      seat->Owner->Callbacks->KeyPressed(flags, key, unicode);
}

//********************************************************************************************************************
// Loads the keymap sent by the compositor.  The current keymap is kept if the new one cannot be compiled.

static void keymap_event(void *Data, wl_keyboard *, uint32_t Format, int FD, uint32_t Size)
{
   auto seat = (WaylandSeat *)Data;
   if (Format IS WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 and Size) {
      auto map = mmap(nullptr, Size, PROT_READ, MAP_PRIVATE, FD, 0);
      if (map != MAP_FAILED) {
         auto keymap = xkb_keymap_new_from_string(seat->Context, (const char *)map,
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
         if (keymap) {
            auto state = xkb_state_new(keymap);
            if (state) {
               stop_repeat(seat);
               if (seat->ComposeState) xkb_compose_state_reset(seat->ComposeState);
               if (seat->KeyState) xkb_state_unref(seat->KeyState);
               if (seat->Keymap) xkb_keymap_unref(seat->Keymap);
               seat->Keymap = keymap;
               seat->KeyState = state;
            }
            else xkb_keymap_unref(keymap);
         }
         munmap(map, Size);
      }
   }
   close(FD);
}

//********************************************************************************************************************
// One of our windows has gained keyboard focus.  Keys that are already held are recorded, so modifiers pressed
// beforehand are known.

static void keyboard_enter(void *Data, wl_keyboard *, uint32_t Serial, wl_surface *Surface, wl_array *Keys)
{
   auto seat = (WaylandSeat *)Data;
   seat->KeyboardSerial = Serial;
   seat->KeyboardWindow = surface_window(seat->Owner, Surface);
   seat->HeldModifiers.clear();

   for (size_t i = 0; i < Keys->size / sizeof(uint32_t); i++) {
      auto key = key_from_code(seat, ((uint32_t *)Keys->data)[i] + 8);
      if (key != KEY::NIL) seat->HeldModifiers.insert(key);
   }

   if (seat->KeyboardWindow and seat->KeyboardWindow->SurfaceID and seat->Owner->Callbacks)
      seat->Owner->Callbacks->FocusState(seat->KeyboardWindow->SurfaceID, true);
}

//********************************************************************************************************************
// Our window has lost keyboard focus.

static void keyboard_leave(void *Data, wl_keyboard *, uint32_t, wl_surface *)
{
   auto seat = (WaylandSeat *)Data;
   stop_repeat(seat);

   auto window = seat->KeyboardWindow;
   seat->KeyboardWindow = nullptr;
   seat->KeyboardSerial = 0;
   seat->HeldModifiers.clear();

   // Keyboard leave resets modifier state; releases in another client need not be reported to us.

   if (seat->KeyState) xkb_state_update_mask(seat->KeyState, 0, 0, 0, 0, 0, 0);
   if (seat->ComposeState) xkb_compose_state_reset(seat->ComposeState);
   if (window and window->SurfaceID and seat->Owner->Callbacks)
      seat->Owner->Callbacks->FocusState(window->SurfaceID, false);
}

//********************************************************************************************************************
// A key has been pressed or released.  Presses are reported with the composed character and start key repeat.  A press
// also provides the serial that publishes the clipboard selection.

static void keyboard_key(void *Data, wl_keyboard *, uint32_t Serial, uint32_t, uint32_t Key, uint32_t State)
{
   auto seat = (WaylandSeat *)Data;
   seat->KeyboardSerial = Serial;
   if (State IS WL_KEYBOARD_KEY_STATE_PRESSED) seat->SelectionSerial = Serial;
   publish_selection(seat);

   if ((not seat->KeyState) or (not seat->KeyboardWindow) or (not seat->Owner->Callbacks)) return;

   const auto code = Key + 8;
   const auto key = key_from_code(seat, code);
   if (State IS WL_KEYBOARD_KEY_STATE_PRESSED) seat->HeldModifiers.insert(key);
   else seat->HeldModifiers.erase(key);

   auto flags = key_qualifiers(seat);
   if (((int(key) >= int(KEY::NP_0)) and (int(key) <= int(KEY::NP_DIVIDE))) or (key IS KEY::NP_ENTER))
      flags |= KQ::NUM_PAD;

   if (State IS WL_KEYBOARD_KEY_STATE_PRESSED) {
      const auto unicode = key_unicode(seat, code);
      seat->Owner->Callbacks->KeyPressed(flags, key, unicode);
      if (seat->RepeatRate > 0 and xkb_keymap_key_repeats(seat->Keymap, code)) {
         seat->RepeatKey = Key;
         seat->RepeatUnicode = unicode;
         itimerspec timer = {};
         timer.it_value.tv_sec = seat->RepeatDelay / 1000;
         timer.it_value.tv_nsec = (seat->RepeatDelay % 1000) * 1000000;
         if ((not timer.it_value.tv_sec) and (not timer.it_value.tv_nsec)) timer.it_value.tv_nsec = 1;
         timer.it_interval.tv_sec = 1 / seat->RepeatRate;
         timer.it_interval.tv_nsec = (1000000000 / seat->RepeatRate) % 1000000000;
         if (seat->TimerFD >= 0) timerfd_settime(seat->TimerFD, 0, &timer, nullptr);
      }
   }
   else {
      if (seat->RepeatKey IS Key) stop_repeat(seat);
      seat->Owner->Callbacks->KeyReleased(flags, key);
   }
}

//********************************************************************************************************************
// Updates the modifier state.  The drag feedback is refreshed because the modifiers determine whether a drop is
// accepted.

static void keyboard_modifiers(void *Data, wl_keyboard *, uint32_t, uint32_t Depressed, uint32_t Latched,
   uint32_t Locked, uint32_t Group)
{
   auto seat = (WaylandSeat *)Data;
   if (seat->KeyState) xkb_state_update_mask(seat->KeyState, Depressed, Latched, Locked, 0, 0, Group);
   if (seat->DragOffer and not seat->DragOffer->PendingDrop) drag_feedback(seat);
}

//********************************************************************************************************************
// Records the key repeat rate (per second) and delay (in milliseconds).  A rate of zero disables repeat.

static void keyboard_repeat_info(void *Data, wl_keyboard *, int32_t Rate, int32_t Delay)
{
   auto seat = (WaylandSeat *)Data;
   seat->RepeatRate = std::max(0, Rate);
   seat->RepeatDelay = std::max(0, Delay);
   if (not seat->RepeatRate) stop_repeat(seat);
}

static const wl_keyboard_listener keyboard_listener = { keymap_event, keyboard_enter, keyboard_leave, keyboard_key,
   keyboard_modifiers, keyboard_repeat_info };

//********************************************************************************************************************
// Returns the cursor theme name for a cursor ID, or null if there is no equivalent.

static const char *cursor_name(PTC Cursor)
{
   switch (Cursor) {
      case PTC::DEFAULT: return "left_ptr";
      case PTC::SIZE_BOTTOM_LEFT: return "bottom_left_corner";
      case PTC::SIZE_BOTTOM_RIGHT: return "bottom_right_corner";
      case PTC::SIZE_TOP_LEFT: return "top_left_corner";
      case PTC::SIZE_TOP_RIGHT: return "top_right_corner";
      case PTC::SIZE_LEFT: return "left_side";
      case PTC::SIZE_RIGHT: return "right_side";
      case PTC::SIZE_TOP: return "top_side";
      case PTC::SIZE_BOTTOM: return "bottom_side";
      case PTC::CROSSHAIR: return "crosshair";
      case PTC::SLEEP: return "watch";
      case PTC::SIZING: return "fleur";
      case PTC::SPLIT_VERTICAL: return "sb_v_double_arrow";
      case PTC::SPLIT_HORIZONTAL: return "sb_h_double_arrow";
      case PTC::MAGNIFIER: return "zoom-in";
      case PTC::HAND: case PTC::HAND_LEFT: case PTC::HAND_RIGHT: return "hand2";
      case PTC::TEXT: return "xterm";
      case PTC::PAINTBRUSH: return "pencil";
      case PTC::STOP: return "not-allowed";
      case PTC::DRAGGABLE: return "grab";
      default: return nullptr;
   }
}

//********************************************************************************************************************
// Returns the cursor-shape-v1 shape for a cursor ID, or zero if there is no equivalent.

#ifdef WAYLAND_CURSOR_SHAPE
static uint32_t cursor_shape(PTC Cursor)
{
   switch (Cursor) {
      case PTC::DEFAULT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
      case PTC::SIZE_BOTTOM_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
      case PTC::SIZE_BOTTOM_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
      case PTC::SIZE_TOP_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
      case PTC::SIZE_TOP_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
      case PTC::SIZE_LEFT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
      case PTC::SIZE_RIGHT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
      case PTC::SIZE_TOP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
      case PTC::SIZE_BOTTOM: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
      case PTC::CROSSHAIR: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
      case PTC::SLEEP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
      case PTC::SIZING: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL;
      case PTC::SPLIT_VERTICAL: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
      case PTC::SPLIT_HORIZONTAL: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
      case PTC::MAGNIFIER: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN;
      case PTC::HAND: case PTC::HAND_LEFT: case PTC::HAND_RIGHT:
         return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
      case PTC::TEXT: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
      case PTC::PAINTBRUSH: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL;
      case PTC::STOP: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
      case PTC::DRAGGABLE: return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
      default: return 0;
   }
}
#endif

//********************************************************************************************************************
// Handles a press on the client-side decorations.  The frame edges start a resize, the title bar controls close,
// maximise or minimise the window, and the rest of the title bar starts a move.  Returns false if the press is in the
// content area.

static bool frame_action(WaylandWindow *Window, WaylandSeat *Seat, uint32_t Serial, double X, double Y)
{
   const auto frame = window_frame(Window);
   if (not frame.Border or frame.contains(X, Y)) return false;

   const auto edges = frame.edges(X, Y);
   if (edges) {
      if (not Window->Maximised) xdg_toplevel_resize(Window->TopLevel, Seat->Handle, Serial, edges);
   }
   else switch (frame.control(X, Y)) {
      case 1: top_close(Window, Window->TopLevel); return true;
      case 2:
         Window->RequestedMaximised = not Window->Maximised;
         if (Window->RequestedMaximised) xdg_toplevel_set_maximized(Window->TopLevel);
         else xdg_toplevel_unset_maximized(Window->TopLevel);
         break;
      case 3: xdg_toplevel_set_minimized(Window->TopLevel); break;
      default: xdg_toplevel_move(Window->TopLevel, Seat->Handle, Serial); break;
   }

   wl_display_flush(Seat->Owner->Connection);
   return true;
}

//********************************************************************************************************************
// Updates the seat's pointer position in content coordinates and reports crossing and movement to the Display module.
// Movement over the decorations is only reported while a button pressed in the content area is held.

static void pointer_position(WaylandSeat *Seat, wl_fixed_t X, wl_fixed_t Y)
{
   auto window = Seat->PointerWindow;
   if (not window) return;

   const auto frame = window_frame(window);
   Seat->X = wl_fixed_to_double(X) - frame.Border;
   Seat->Y = wl_fixed_to_double(Y) - frame.Top;

   const bool inside = frame.contains(Seat->X, Seat->Y);
   const bool crossed = inside != Seat->PointerInContent;
   Seat->PointerInContent = inside;

   if (window->SurfaceID and Seat->Owner->Callbacks) {
      if (crossed) Seat->Owner->Callbacks->Crossing(window->SurfaceID, inside, Seat->X, Seat->Y);
      if (not Seat->Owner->Windows.contains(window)) return;
      if (inside or Seat->ContentButtons)
         Seat->Owner->Callbacks->Movement(window->SurfaceID, Seat->X, Seat->Y, false);
   }
}

//********************************************************************************************************************
// Sets the cursor for the window under the pointer, with resize cursors over the frame edges.  A custom cursor takes
// precedence, followed by the cursor-shape protocol and then the cursor theme.

static void apply_cursor(WaylandSeat *Seat)
{
   auto window = Seat->PointerWindow;
   if ((not window) or (not Seat->Pointer) or (not Seat->PointerSerial)) return;

   auto cursor_id = window->Cursor;
   const bool content = window_frame(window).contains(Seat->X, Seat->Y);
   if (not content) {
      cursor_id = PTC::DEFAULT;
      switch (window_frame(window).edges(Seat->X, Seat->Y)) {
         case XDG_TOPLEVEL_RESIZE_EDGE_TOP: cursor_id = PTC::SIZE_TOP; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: cursor_id = PTC::SIZE_BOTTOM; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: cursor_id = PTC::SIZE_LEFT; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: cursor_id = PTC::SIZE_RIGHT; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: cursor_id = PTC::SIZE_TOP_LEFT; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: cursor_id = PTC::SIZE_TOP_RIGHT; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: cursor_id = PTC::SIZE_BOTTOM_LEFT; break;
         case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: cursor_id = PTC::SIZE_BOTTOM_RIGHT; break;
      }
   }

   if ((content and not window->CursorVisible) or (cursor_id IS PTC::INVISIBLE)) {
      wl_pointer_set_cursor(Seat->Pointer, Seat->PointerSerial, nullptr, 0, 0);
      return;
   }

   if (cursor_id IS PTC::CUSTOM and window->CustomCursor) {
      auto buffer = window->CustomCursor;
      wl_pointer_set_cursor(Seat->Pointer, Seat->PointerSerial, Seat->CursorSurface,
         buffer->HotX, buffer->HotY);
      wl_surface_attach(Seat->CursorSurface, buffer->Handle, 0, 0);
      wl_surface_damage(Seat->CursorSurface, 0, 0, buffer->Width, buffer->Height);
      wl_surface_commit(Seat->CursorSurface);
      buffer->Busy = true;
      return;
   }

#ifdef WAYLAND_CURSOR_SHAPE
   if (Seat->ShapeDevice) {
      const auto shape = cursor_shape(cursor_id);
      if (shape) {
         wp_cursor_shape_device_v1_set_shape(Seat->ShapeDevice, Seat->PointerSerial, shape);
         return;
      }
   }
#endif

   auto name = cursor_name(cursor_id);
   auto cursor = name and Seat->Owner->CursorTheme ?
      wl_cursor_theme_get_cursor(Seat->Owner->CursorTheme, name) : nullptr;
   if ((not cursor) or (not cursor->image_count) or (not Seat->CursorSurface)) return;

   auto image = cursor->images[0];
   auto buffer = wl_cursor_image_get_buffer(image);
   if (not buffer) return;

   wl_pointer_set_cursor(Seat->Pointer, Seat->PointerSerial, Seat->CursorSurface,
      image->hotspot_x, image->hotspot_y);
   wl_surface_attach(Seat->CursorSurface, buffer, 0, 0);
   wl_surface_damage(Seat->CursorSurface, 0, 0, image->width, image->height);
   wl_surface_commit(Seat->CursorSurface);
}

//********************************************************************************************************************
// The pointer has entered one of our windows.

static void pointer_enter(void *Data, wl_pointer *, uint32_t Serial, wl_surface *Surface, wl_fixed_t X, wl_fixed_t Y)
{
   auto seat = (WaylandSeat *)Data;
   seat->PointerSerial = Serial;
   seat->PointerWindow = surface_window(seat->Owner, Surface);
   seat->PointerInContent = false;
   pointer_position(seat, X, Y);
   apply_cursor(seat);
}

//********************************************************************************************************************
// The pointer has left our window.  Buttons held in the content area are released, unless the compositor has taken the
// pointer for an exported drag.

static void pointer_leave(void *Data, wl_pointer *, uint32_t, wl_surface *)
{
   auto seat = (WaylandSeat *)Data;
   seat->ScrollX = 0;
   seat->ScrollY = 0;

   auto window = seat->PointerWindow;
   const bool inside = seat->PointerInContent;
   seat->PointerWindow = nullptr;
   seat->PointerInContent = false;
   seat->PointerSerial = 0;
   const auto buttons = seat->ContentButtons;
   seat->ContentButtons = 0;

   // The compositor takes the pointer when it starts an exported drag.  The button is still held, and its release is
   // synthesised when the drag ends.

   auto &e = seat->Owner->Export;
   const bool granted = (e.Stage IS WaylandExport::Phase::Hover) and (e.Seat IS seat) and (not e.Granted) and
      (not e.Cancelled);
   if (granted) e.Granted = true;

   if (seat->Owner->Callbacks) {
      if (not granted) for (int mask = 1; mask <= 0x10; mask <<= 1)
         if (buttons & mask) seat->Owner->Callbacks->ButtonInput(mask, false);
      if (window and inside and seat->Owner->Windows.contains(window) and window->SurfaceID)
         seat->Owner->Callbacks->Crossing(window->SurfaceID, false, seat->X, seat->Y);
   }
}

//********************************************************************************************************************
// The pointer has moved.  The cursor is updated when the pointer moves between the content and the frame, or between
// frame edges.

static void pointer_motion(void *Data, wl_pointer *, uint32_t, wl_fixed_t X, wl_fixed_t Y)
{
   auto seat = (WaylandSeat *)Data;
   auto window = seat->PointerWindow;
   if (not window) return;

   const auto old_edges = window_frame(window).edges(seat->X, seat->Y);
   const bool was_inside = seat->PointerInContent;

   pointer_position(seat, X, Y);
   if (seat->PointerWindow and ((was_inside != seat->PointerInContent) or
         (old_edges != window_frame(seat->PointerWindow).edges(seat->X, seat->Y)))) apply_cursor(seat);
}

//********************************************************************************************************************
// A pointer button has been pressed or released.  Presses on the decorations are handled by frame_action(), and only
// buttons pressed in the content area are reported to the Display module.  A press also provides the serials that
// publish the clipboard selection and start drags.

static void pointer_button(void *Data, wl_pointer *, uint32_t Serial, uint32_t, uint32_t Button, uint32_t State)
{
   auto seat = (WaylandSeat *)Data;
   seat->PointerSerial = Serial;
   if (State IS WL_POINTER_BUTTON_STATE_PRESSED) seat->SelectionSerial = Serial;
   publish_selection(seat);

   int mask = 0;
   if (Button IS BTN_LEFT) mask = 0x0001;
   else if (Button IS BTN_RIGHT) mask = 0x0002;
   else if (Button IS BTN_MIDDLE) mask = 0x0004;
   else if ((Button IS BTN_SIDE) or (Button IS BTN_BACK)) mask = 0x0008;
   else if ((Button IS BTN_EXTRA) or (Button IS BTN_FORWARD)) mask = 0x0010;

   auto window = seat->PointerWindow;
   if (not window or not mask) return;

   const bool pressed = State IS WL_POINTER_BUTTON_STATE_PRESSED;
   if (pressed and not window_frame(window).contains(seat->X, seat->Y)) {
      if (Button IS BTN_LEFT) frame_action(window, seat, Serial, seat->X, seat->Y);
      return;
   }

   if (pressed) {
      seat->ContentButtons |= mask;
      if (mask IS 0x0001) seat->PressSerial = Serial;
   }
   else {
      if (not (seat->ContentButtons & mask)) return;
      seat->ContentButtons &= ~uint32_t(mask);
   }

   if (seat->Owner->Callbacks) seat->Owner->Callbacks->ButtonInput(mask, pressed);
}

//********************************************************************************************************************
// A wheel notch is reported as 15 axis units, which is rescaled to match the X11 driver.  Wayland does not report
// modifiers with pointer events, so the Shift state is only known while the keyboard focus is on one of our windows.

static void pointer_frame(void *Data, wl_pointer *)
{
   auto seat = (WaylandSeat *)Data;
   auto delta_x = std::exchange(seat->ScrollX, 0);
   auto delta_y = std::exchange(seat->ScrollY, 0);

   if ((not seat->PointerWindow) or (not seat->PointerInContent) or (not seat->Owner->Callbacks)) return;

   if ((delta_x != 0) or (delta_y != 0)) {
      seat->Owner->Callbacks->WheelMovement(seat->PointerWindow->SurfaceID,
         delta_x, delta_y, key_qualifiers(seat));
   }
}

//********************************************************************************************************************
// Accumulates scroll movement, which is reported at the end of the pointer frame.

static void pointer_axis(void *Data, wl_pointer *Pointer, uint32_t, uint32_t Axis, wl_fixed_t Value)
{
   auto seat = (WaylandSeat *)Data;
   if ((not seat->PointerWindow) or (not seat->PointerInContent) or (not seat->Owner->Callbacks)) return;

   auto delta = float(wl_fixed_to_double(Value) * 9.0 / 15.0);
   if (Axis IS WL_POINTER_AXIS_VERTICAL_SCROLL) seat->ScrollY += delta;
   else if (Axis IS WL_POINTER_AXIS_HORIZONTAL_SCROLL) seat->ScrollX += delta;

   // Older protocol versions do not send frames.  Newer versions group both axes so that Shift only redirects
   // purely vertical gestures.

   if (wl_pointer_get_version(Pointer) < WL_POINTER_FRAME_SINCE_VERSION) pointer_frame(Data, Pointer);
}

static void pointer_axis_source(void *, wl_pointer *, uint32_t) { }
static void pointer_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) { }
static void pointer_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) { }

static const wl_pointer_listener pointer_listener = { pointer_enter, pointer_leave, pointer_motion, pointer_button,
   pointer_axis, pointer_frame, pointer_axis_source, pointer_axis_stop, pointer_axis_discrete };

//********************************************************************************************************************
// Releases a pointer device, with the release request where the protocol version supports it.

static void release_pointer(wl_pointer *Pointer)
{
   if (wl_pointer_get_version(Pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(Pointer);
   else wl_pointer_destroy(Pointer);
}

//********************************************************************************************************************
// Releases a keyboard device, with the release request where the protocol version supports it.

static void release_keyboard(wl_keyboard *Keyboard)
{
   if (wl_keyboard_get_version(Keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(Keyboard);
   else wl_keyboard_destroy(Keyboard);
}

//********************************************************************************************************************
// Releases a seat, with the release request where the protocol version supports it.

static void release_seat(wl_seat *Seat)
{
   if (wl_seat_get_version(Seat) >= WL_SEAT_RELEASE_SINCE_VERSION) wl_seat_release(Seat);
   else wl_seat_destroy(Seat);
}

//********************************************************************************************************************
// Releases a touch device, with the release request where the protocol version supports it.

static void release_touch(wl_touch *Touch)
{
   if (wl_touch_get_version(Touch) >= WL_TOUCH_RELEASE_SINCE_VERSION) wl_touch_release(Touch);
   else wl_touch_destroy(Touch);
}

//********************************************************************************************************************
// Cancels the seat's touch points, either all of them or only those in the given window.

static void cancel_touches(WaylandSeat *Seat, WaylandWindow *Window = nullptr)
{
   for (auto it = Seat->TouchPoints.begin(); it != Seat->TouchPoints.end();) {
      auto point = it->second;
      if (Window and point.Window != Window) { ++it; continue; }
      if (point.Window and point.Window->SurfaceID and Seat->Owner->Callbacks and
            Seat->Owner->Callbacks->TouchInput)
         Seat->Owner->Callbacks->TouchInput(point.Window->SurfaceID, Seat->Name, it->first,
            JET::TOUCH_CANCEL, point.X, point.Y);
      it = Seat->TouchPoints.erase(it);
   }
}

//********************************************************************************************************************
// A touch has started.  Touches on the decorations are handled by frame_action(), and a reused touch ID cancels the
// earlier touch.

static void touch_down(void *Data, wl_touch *, uint32_t Serial, uint32_t, wl_surface *Surface,
   int32_t ID, wl_fixed_t X, wl_fixed_t Y)
{
   auto seat = (WaylandSeat *)Data;
   seat->SelectionSerial = Serial;
   publish_selection(seat);

   auto window = surface_window(seat->Owner, Surface);
   if ((not window) or (not window->SurfaceID)) return;

   if (auto previous = seat->TouchPoints.find(ID); previous != seat->TouchPoints.end()) {
      if (previous->second.Window and previous->second.Window->SurfaceID and seat->Owner->Callbacks)
         seat->Owner->Callbacks->TouchInput(previous->second.Window->SurfaceID, seat->Name, ID,
            JET::TOUCH_CANCEL, previous->second.X, previous->second.Y);
   }

   const auto frame = window_frame(window);
   const double x = wl_fixed_to_double(X) - frame.Border;
   const double y = wl_fixed_to_double(Y) - frame.Top;
   if (frame_action(window, seat, Serial, x, y)) return;

   seat->TouchPoints[ID] = { window, x, y };
   if (seat->Owner->Callbacks and seat->Owner->Callbacks->TouchInput)
      seat->Owner->Callbacks->TouchInput(window->SurfaceID, seat->Name, ID, JET::TOUCH_DOWN, x, y);
}

//********************************************************************************************************************
// A touch has ended.

static void touch_up(void *Data, wl_touch *, uint32_t, uint32_t, int32_t ID)
{
   auto seat = (WaylandSeat *)Data;
   auto it = seat->TouchPoints.find(ID);
   if (it IS seat->TouchPoints.end()) return;

   auto point = it->second;
   seat->TouchPoints.erase(it);

   if (point.Window and point.Window->SurfaceID and seat->Owner->Callbacks and seat->Owner->Callbacks->TouchInput)
      seat->Owner->Callbacks->TouchInput(point.Window->SurfaceID, seat->Name, ID,
         JET::TOUCH_UP, point.X, point.Y);
}

//********************************************************************************************************************
// A touch point has moved.

static void touch_motion(void *Data, wl_touch *, uint32_t, int32_t ID, wl_fixed_t X, wl_fixed_t Y)
{
   auto seat = (WaylandSeat *)Data;
   auto it = seat->TouchPoints.find(ID);
   if (it IS seat->TouchPoints.end()) return;

   const auto frame = window_frame(it->second.Window);
   it->second.X = wl_fixed_to_double(X) - frame.Border;
   it->second.Y = wl_fixed_to_double(Y) - frame.Top;

   if (it->second.Window and it->second.Window->SurfaceID and seat->Owner->Callbacks and
         seat->Owner->Callbacks->TouchInput)
      seat->Owner->Callbacks->TouchInput(it->second.Window->SurfaceID, seat->Name, ID,
         JET::TOUCH_MOTION, it->second.X, it->second.Y);
}

static void touch_frame(void *, wl_touch *) { }
static void touch_cancel(void *Data, wl_touch *) { cancel_touches((WaylandSeat *)Data); }
static void touch_shape(void *, wl_touch *, int32_t, wl_fixed_t, wl_fixed_t) { }
static void touch_orientation(void *, wl_touch *, int32_t, wl_fixed_t) { }

static const wl_touch_listener touch_listener = { touch_down, touch_up, touch_motion, touch_frame,
   touch_cancel, touch_shape, touch_orientation };

//********************************************************************************************************************
// Reports unaccelerated relative motion while the pointer is locked.

#ifdef WAYLAND_RELATIVE_POINTER
static void relative_motion(void *Data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, wl_fixed_t DX,
   wl_fixed_t DY, wl_fixed_t DXUnaccelerated, wl_fixed_t DYUnaccelerated)
{
   auto seat = (WaylandSeat *)Data;
   if (seat->LockedWindow and seat->LockedWindow->SurfaceID and seat->Owner->Callbacks and
         seat->Owner->Callbacks->RelativeMovement)
      seat->Owner->Callbacks->RelativeMovement(seat->LockedWindow->SurfaceID,
         wl_fixed_to_double(DXUnaccelerated), wl_fixed_to_double(DYUnaccelerated));
}

static const zwp_relative_pointer_v1_listener relative_listener = { relative_motion };
#endif

#ifdef WAYLAND_POINTER_CONSTRAINTS
static void pointer_locked(void *, zwp_locked_pointer_v1 *) { }

//********************************************************************************************************************
// The compositor has removed the pointer lock.  The Display module is notified and the lock is released.

static void pointer_unlocked(void *Data, zwp_locked_pointer_v1 *)
{
   auto seat = (WaylandSeat *)Data;
   if (seat->LockedWindow and seat->LockedWindow->SurfaceID and seat->Owner->Callbacks and
         seat->Owner->Callbacks->PointerLockLost)
      seat->Owner->Callbacks->PointerLockLost(seat->LockedWindow->SurfaceID);
   seat->LockedWindow = nullptr;
   if (seat->Locked) { zwp_locked_pointer_v1_destroy(seat->Locked); seat->Locked = nullptr; }
#ifdef WAYLAND_RELATIVE_POINTER
   if (seat->Relative) { zwp_relative_pointer_v1_destroy(seat->Relative); seat->Relative = nullptr; }
#endif
}

static const zwp_locked_pointer_v1_listener locked_listener = { pointer_locked, pointer_unlocked };
#endif

//********************************************************************************************************************
// Creates or releases the seat's pointer, keyboard and touch devices as its capabilities change.

static void seat_capabilities(void *Data, wl_seat *Handle, uint32_t Capabilities)
{
   auto seat = (WaylandSeat *)Data;
   if ((Capabilities & WL_SEAT_CAPABILITY_POINTER) and (not seat->Pointer)) {
      seat->Pointer = wl_seat_get_pointer(Handle);
      if (seat->Pointer) wl_pointer_add_listener(seat->Pointer, &pointer_listener, seat);
      seat->CursorSurface = wl_compositor_create_surface(seat->Owner->Compositor);
#ifdef WAYLAND_CURSOR_SHAPE
      if (seat->Pointer and seat->Owner->ShapeManager)
         seat->ShapeDevice = wp_cursor_shape_manager_v1_get_pointer(seat->Owner->ShapeManager, seat->Pointer);
#endif
   }
   else if ((not (Capabilities & WL_SEAT_CAPABILITY_POINTER)) and seat->Pointer) {
      if (seat->LockedWindow and seat->LockedWindow->SurfaceID and seat->Owner->Callbacks and
            seat->Owner->Callbacks->PointerLockLost)
         seat->Owner->Callbacks->PointerLockLost(seat->LockedWindow->SurfaceID);
#ifdef WAYLAND_CURSOR_SHAPE
      if (seat->ShapeDevice) { wp_cursor_shape_device_v1_destroy(seat->ShapeDevice); seat->ShapeDevice = nullptr; }
#endif
#ifdef WAYLAND_POINTER_CONSTRAINTS
      if (seat->Locked) { zwp_locked_pointer_v1_destroy(seat->Locked); seat->Locked = nullptr; }
#endif
#ifdef WAYLAND_RELATIVE_POINTER
      if (seat->Relative) { zwp_relative_pointer_v1_destroy(seat->Relative); seat->Relative = nullptr; }
#endif

      seat->LockedWindow = nullptr;
      pointer_leave(seat, seat->Pointer, 0, nullptr);
      release_pointer(seat->Pointer);
      seat->Pointer = nullptr;
      if (seat->CursorSurface) wl_surface_destroy(seat->CursorSurface);
      seat->CursorSurface = nullptr;
   }

   if ((Capabilities & WL_SEAT_CAPABILITY_KEYBOARD) and (not seat->Keyboard)) {
      seat->Keyboard = wl_seat_get_keyboard(Handle);
      if (seat->Keyboard) wl_keyboard_add_listener(seat->Keyboard, &keyboard_listener, seat);
   }
   else if ((not (Capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) and seat->Keyboard) {
      stop_repeat(seat);
      release_keyboard(seat->Keyboard);
      seat->Keyboard = nullptr;
      seat->KeyboardWindow = nullptr;
   }

   if ((Capabilities & WL_SEAT_CAPABILITY_TOUCH) and (not seat->Touch)) {
      seat->Touch = wl_seat_get_touch(Handle);
      if (seat->Touch) wl_touch_add_listener(seat->Touch, &touch_listener, seat);
   }
   else if ((not (Capabilities & WL_SEAT_CAPABILITY_TOUCH)) and seat->Touch) {
      cancel_touches(seat);
      release_touch(seat->Touch);
      seat->Touch = nullptr;
   }
}

static void seat_name(void *, wl_seat *, const char *) { }

static const wl_seat_listener seat_listener = { seat_capabilities, seat_name };

//********************************************************************************************************************
// Binds the globals that the driver uses as the compositor advertises them.  Outputs and seats can be added at any
// time.

static void registry_global(void *Data, wl_registry *Registry, uint32_t Name, const char *Interface, uint32_t Version)
{
   auto state = (WaylandDriver::State *)Data;
   if ((std::strcmp(Interface, wl_compositor_interface.name) IS 0) and (not state->Compositor))
      state->Compositor = (wl_compositor *)wl_registry_bind(Registry, Name, &wl_compositor_interface,
         std::min(Version, 4u));
   else if ((std::strcmp(Interface, wl_shm_interface.name) IS 0) and (not state->Shm))
      state->Shm = (wl_shm *)wl_registry_bind(Registry, Name, &wl_shm_interface, 1);
   else if ((std::strcmp(Interface, wl_data_device_manager_interface.name) IS 0) and (not state->DataManager))
      state->DataManager = (wl_data_device_manager *)wl_registry_bind(Registry, Name,
         &wl_data_device_manager_interface, std::min(Version, 3u));
   else if ((std::strcmp(Interface, xdg_wm_base_interface.name) IS 0) and (not state->Shell)) {
      state->Shell = (xdg_wm_base *)wl_registry_bind(Registry, Name, &xdg_wm_base_interface, 1);
      if (state->Shell) xdg_wm_base_add_listener(state->Shell, &shell_listener, state);
   }
   else if (std::strcmp(Interface, wl_output_interface.name) IS 0) {
      auto output = new(std::nothrow) WaylandOutput;
      if (not output) return;

      output->Owner = state;
      output->Name = Name;
      output->Handle = (wl_output *)wl_registry_bind(Registry, Name, &wl_output_interface,
         std::min(Version, 2u));
      if (output->Handle) {
         state->Outputs.push_back(output);
         wl_output_add_listener(output->Handle, &output_listener, output);
      }
      else delete output;
   }
#ifdef WAYLAND_FRACTIONAL_SCALE
   else if ((std::strcmp(Interface, wp_fractional_scale_manager_v1_interface.name) IS 0) and
         (not state->FractionalManager))
      state->FractionalManager = (wp_fractional_scale_manager_v1 *)wl_registry_bind(Registry, Name,
         &wp_fractional_scale_manager_v1_interface, 1);
   else if ((std::strcmp(Interface, wp_viewporter_interface.name) IS 0) and (not state->Viewporter))
      state->Viewporter = (wp_viewporter *)wl_registry_bind(Registry, Name, &wp_viewporter_interface, 1);
#endif
   else if (std::strcmp(Interface, wl_seat_interface.name) IS 0) {
      auto seat = new(std::nothrow) WaylandSeat;
      if (not seat) return;

      seat->Owner = state;
      seat->Name = Name;
      seat->Handle = (wl_seat *)wl_registry_bind(Registry, Name, &wl_seat_interface, std::min(Version, 5u));
      seat->Context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (seat->Context) {
         const char *locale = std::getenv("LC_ALL");
         if ((not locale) or (not locale[0])) locale = std::getenv("LC_CTYPE");
         if ((not locale) or (not locale[0])) locale = std::getenv("LANG");
         if ((not locale) or (not locale[0])) locale = "C";
         seat->ComposeTable = xkb_compose_table_new_from_locale(seat->Context, locale,
            XKB_COMPOSE_COMPILE_NO_FLAGS);
         if (seat->ComposeTable) seat->ComposeState = xkb_compose_state_new(seat->ComposeTable,
            XKB_COMPOSE_STATE_NO_FLAGS);
      }

      seat->TimerFD = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
      if ((not seat->Handle) or (not seat->Context) or (seat->TimerFD < 0) or
            (RegisterFD(seat->TimerFD, RFD::READ, repeat_event, seat) != ERR::Okay)) {
         if (seat->TimerFD >= 0) close(seat->TimerFD);
         if (seat->ComposeState) xkb_compose_state_unref(seat->ComposeState);
         if (seat->ComposeTable) xkb_compose_table_unref(seat->ComposeTable);
         if (seat->Context) xkb_context_unref(seat->Context);
         if (seat->Handle) release_seat(seat->Handle);
         delete seat;
         return;
      }

      state->Seats.push_back(seat);
      wl_seat_add_listener(seat->Handle, &seat_listener, seat);
      if (state->Open and state->DataManager) {
         seat->DataDevice = wl_data_device_manager_get_data_device(state->DataManager, seat->Handle);
         if (seat->DataDevice) wl_data_device_add_listener(seat->DataDevice, &data_device_listener, seat);
      }
   }
#ifdef WAYLAND_CURSOR_SHAPE
   else if ((std::strcmp(Interface, wp_cursor_shape_manager_v1_interface.name) IS 0) and
         (not state->ShapeManager))
      state->ShapeManager = (wp_cursor_shape_manager_v1 *)wl_registry_bind(Registry, Name,
         &wp_cursor_shape_manager_v1_interface, 1);
#endif
#ifdef WAYLAND_POINTER_CONSTRAINTS
   else if ((std::strcmp(Interface, zwp_pointer_constraints_v1_interface.name) IS 0) and
         (not state->Constraints))
      state->Constraints = (zwp_pointer_constraints_v1 *)wl_registry_bind(Registry, Name,
         &zwp_pointer_constraints_v1_interface, 1);
#endif
#ifdef WAYLAND_RELATIVE_POINTER
   else if ((std::strcmp(Interface, zwp_relative_pointer_manager_v1_interface.name) IS 0) and
         (not state->RelativeManager))
      state->RelativeManager = (zwp_relative_pointer_manager_v1 *)wl_registry_bind(Registry, Name,
         &zwp_relative_pointer_manager_v1_interface, 1);
#endif
#ifdef WAYLAND_XDG_DECORATION
   else if ((std::strcmp(Interface, zxdg_decoration_manager_v1_interface.name) IS 0) and
         (not state->DecorationManager))
      state->DecorationManager = (zxdg_decoration_manager_v1 *)wl_registry_bind(Registry, Name,
         &zxdg_decoration_manager_v1_interface, 1);
#endif
}

//********************************************************************************************************************
// Releases all resources held by a seat.

static void dispose_seat(WaylandSeat *Seat)
{
   if (Seat->Owner->Export.Seat IS Seat) Seat->Owner->Export.Seat = nullptr;
   if (Seat->DragOffer or Seat->DragWindow) data_leave(Seat, nullptr);

   if (Seat->DataDevice) {
      if (wl_data_device_get_version(Seat->DataDevice) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
         wl_data_device_release(Seat->DataDevice);
      else wl_data_device_destroy(Seat->DataDevice);
   }

   if (Seat->SelectionOffer) dispose_offer(Seat->SelectionOffer);
   if (Seat->DragOffer and not Seat->DragOffer->PendingDrop) dispose_offer(Seat->DragOffer);
   Seat->SelectionSource = nullptr;

   stop_repeat(Seat);
   cancel_touches(Seat);
   if (Seat->TimerFD >= 0) { DeregisterFD(Seat->TimerFD); ::close(Seat->TimerFD); }

#ifdef WAYLAND_POINTER_CONSTRAINTS
   if (Seat->Locked) zwp_locked_pointer_v1_destroy(Seat->Locked);
#endif
#ifdef WAYLAND_RELATIVE_POINTER
   if (Seat->Relative) zwp_relative_pointer_v1_destroy(Seat->Relative);
#endif
   if (Seat->Keyboard) release_keyboard(Seat->Keyboard);
   if (Seat->Touch) release_touch(Seat->Touch);
#ifdef WAYLAND_CURSOR_SHAPE
   if (Seat->ShapeDevice) wp_cursor_shape_device_v1_destroy(Seat->ShapeDevice);
#endif
   if (Seat->Pointer) release_pointer(Seat->Pointer);
   if (Seat->CursorSurface) wl_surface_destroy(Seat->CursorSurface);
   if (Seat->Handle) release_seat(Seat->Handle);
   if (Seat->KeyState) xkb_state_unref(Seat->KeyState);
   if (Seat->Keymap) xkb_keymap_unref(Seat->Keymap);
   if (Seat->ComposeState) xkb_compose_state_unref(Seat->ComposeState);
   if (Seat->ComposeTable) xkb_compose_table_unref(Seat->ComposeTable);
   if (Seat->Context) xkb_context_unref(Seat->Context);
   delete Seat;
}

//********************************************************************************************************************
// A global has been removed.  Seats and outputs are disposed.

static void registry_remove(void *Data, wl_registry *, uint32_t Name)
{
   auto state = (WaylandDriver::State *)Data;
   for (auto it = state->Seats.begin(); it != state->Seats.end(); ++it) {
      auto seat = *it;
      if (seat->Name IS Name) {
         stop_repeat(seat);
         if (seat->PointerWindow and seat->PointerWindow->SurfaceID and state->Callbacks)
            state->Callbacks->Crossing(seat->PointerWindow->SurfaceID, false, seat->X, seat->Y);
         if (seat->KeyboardWindow and seat->KeyboardWindow->SurfaceID and state->Callbacks)
            state->Callbacks->FocusState(seat->KeyboardWindow->SurfaceID, false);
         state->Seats.erase(it);
         dispose_seat(seat);
         return;
      }
   }

   for (auto it = state->Outputs.begin(); it != state->Outputs.end(); ++it) if ((*it)->Name IS Name) {
      auto output = *it;
      auto windows = std::vector<WaylandWindow *>(state->Windows.begin(), state->Windows.end());
      for (auto window : windows) {
         if (not state->Windows.contains(window)) continue;
         auto &outputs = window->Outputs;
         outputs.erase(std::remove(outputs.begin(), outputs.end(), output), outputs.end());
         update_scale(window);
      }
      state->Outputs.erase(it);
      wl_output_destroy(output->Handle);
      delete output;
      return;
   }
}

static const wl_registry_listener registry_listener = { registry_global, registry_remove };

//********************************************************************************************************************
// Destroys the window's protocol objects, so that the window can be mapped again later.  Seats are detached from the
// window, which ends its drags, touches, pointer lock and focus.

static void destroy_protocol_window(WaylandWindow *Window)
{
   // An export routed to this window loses its hover without moving the internal session over a closing window.
   if (Window->Owner->Export.OwnWindow IS Window) Window->Owner->Export.OwnWindow = nullptr;

   for (auto seat : Window->Owner->Seats) {
      if (seat->DragWindow IS Window) data_leave(seat, nullptr);
      cancel_touches(seat, Window);
      if (seat->LockedWindow IS Window) {
         if (Window->SurfaceID and Window->Owner->Callbacks and Window->Owner->Callbacks->PointerLockLost)
            Window->Owner->Callbacks->PointerLockLost(Window->SurfaceID);
#ifdef WAYLAND_POINTER_CONSTRAINTS
         if (seat->Locked) { zwp_locked_pointer_v1_destroy(seat->Locked); seat->Locked = nullptr; }
#endif
#ifdef WAYLAND_RELATIVE_POINTER
         if (seat->Relative) { zwp_relative_pointer_v1_destroy(seat->Relative); seat->Relative = nullptr; }
#endif
         seat->LockedWindow = nullptr;
      }
      if (seat->PointerWindow IS Window) {
         pointer_leave(seat, seat->Pointer, 0, Window->Surface);
      }
      if (seat->KeyboardWindow IS Window) {
         stop_repeat(seat);
         if (Window->SurfaceID and Window->Owner->Callbacks)
            Window->Owner->Callbacks->FocusState(Window->SurfaceID, false);
         seat->KeyboardWindow = nullptr;
      }
   }

   if (Window->Frame) { wl_callback_destroy(Window->Frame); Window->Frame = nullptr; }
#ifdef WAYLAND_FRACTIONAL_SCALE
   if (Window->FractionalScale) wp_fractional_scale_v1_destroy(Window->FractionalScale);
   if (Window->Viewport) wp_viewport_destroy(Window->Viewport);
   Window->FractionalScale = nullptr;
   Window->Viewport = nullptr;
   Window->PreferredScale120 = 0;
#endif
   Window->Outputs.clear();
   Window->Scale = 1;
   Window->Scale120 = 120;

#ifdef WAYLAND_XDG_DECORATION
   if (Window->Decoration) zxdg_toplevel_decoration_v1_destroy(Window->Decoration);
   Window->Decoration = nullptr;
#endif
   if (Window->Popup) xdg_popup_destroy(Window->Popup);
   if (Window->TopLevel) xdg_toplevel_destroy(Window->TopLevel);
   if (Window->ShellSurface) xdg_surface_destroy(Window->ShellSurface);
   if (Window->Surface) wl_surface_destroy(Window->Surface);

   Window->TopLevel = nullptr;
   Window->Popup = nullptr;
   Window->Parent = nullptr;
   Window->ShellSurface = nullptr;
   Window->Surface = nullptr;
   Window->Lifecycle = WaylandWindow::Phase::Closing;
   Window->Presented.clear();
}

//********************************************************************************************************************
// Recursively unmaps the popups that belong to the window, and tells the Display module that each one is hidden.

static void unmap_children(WaylandWindow *Window)
{
   auto state = Window->Owner;
   const std::vector<WaylandWindow *> children(state->Windows.begin(), state->Windows.end());
   for (auto child : children) {
      if (not state->Windows.contains(child)) continue;
      if (child->Parent IS Window) {
         unmap_children(child);
         if (not state->Windows.contains(child)) continue;
         child->Visible = false;
         destroy_protocol_window(child);
         if (child->SurfaceID and state->Callbacks and state->Callbacks->WindowHidden)
            state->Callbacks->WindowHidden(child->SurfaceID);
      }
   }
}

//********************************************************************************************************************
// Creates the surface and role of a window.  A window with a PopOverID becomes a popup of its parent and fails with
// ERR::NoSupport if the parent is not mapped.  Other windows become top-levels.  The surface is committed without a
// buffer to request the first configure.

static ERR create_protocol_window(WaylandWindow *Window, bool Maximise)
{
   auto state = Window->Owner;

   Window->ClientDecorations = Window->PendingClientDecorations = true;
   Window->DecorDirty = true;
   Window->Fullscreen = false;
   Window->PendingFullscreen = false;
   Window->PendingMaximised = false;

   Window->Surface = wl_compositor_create_surface(state->Compositor);
   if (Window->Surface) {
      wl_surface_add_listener(Window->Surface, &surface_output_listener, Window);
#ifdef WAYLAND_FRACTIONAL_SCALE
      if (state->FractionalManager and state->Viewporter) {
         Window->Viewport = wp_viewporter_get_viewport(state->Viewporter, Window->Surface);
         if (Window->Viewport) {
            Window->FractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
               state->FractionalManager, Window->Surface);
            if (Window->FractionalScale)
               wp_fractional_scale_v1_add_listener(Window->FractionalScale, &fractional_listener, Window);
            else { wp_viewport_destroy(Window->Viewport); Window->Viewport = nullptr; }
         }
      }
#endif
   }

   if (Window->Surface) Window->ShellSurface = xdg_wm_base_get_xdg_surface(state->Shell, Window->Surface);
   if (Window->ShellSurface and Window->Display->PopOverID) {
      for (auto candidate : state->Windows) {
         if ((candidate->Display->UID IS Window->Display->PopOverID) and candidate->ShellSurface and
               (candidate->Lifecycle IS WaylandWindow::Phase::Mapped)) {
            Window->Parent = candidate;
            break;
         }
      }
      if (Window->Parent) {
         auto positioner = xdg_wm_base_create_positioner(state->Shell);
         if (positioner) {
            const auto parent_frame = window_frame(Window->Parent);
            const int x = Window->Display->X - Window->Parent->Display->X + parent_frame.Border;
            const int y = Window->Display->Y - Window->Parent->Display->Y + parent_frame.Top;

            xdg_positioner_set_size(positioner, Window->Width, Window->Height);
            xdg_positioner_set_anchor_rect(positioner, x, y, 1, 1);
            xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
            xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
            xdg_positioner_set_constraint_adjustment(positioner,
               XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X|XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
            Window->Popup = xdg_surface_get_popup(Window->ShellSurface, Window->Parent->ShellSurface, positioner);
            xdg_positioner_destroy(positioner);
         }
      }
   }
   else if (Window->ShellSurface) Window->TopLevel = xdg_surface_get_toplevel(Window->ShellSurface);

   if ((not Window->Surface) or (not Window->ShellSurface) or
         ((not Window->TopLevel) and (not Window->Popup))) {
      destroy_protocol_window(Window);
      return Window->Display->PopOverID ? ERR::NoSupport : ERR::CreateResource;
   }

   xdg_surface_add_listener(Window->ShellSurface, &surface_listener, Window);
   if (Window->Popup) xdg_popup_add_listener(Window->Popup, &popup_listener, Window);
   else {
      xdg_toplevel_add_listener(Window->TopLevel, &top_listener, Window);
      xdg_toplevel_set_title(Window->TopLevel, Window->Title.c_str());
      xdg_toplevel_set_app_id(Window->TopLevel, Window->AppID.empty() ? DEFAULT_APP_ID : Window->AppID.c_str());
      set_size_hints(Window);
      if (Maximise or Window->RequestedMaximised) xdg_toplevel_set_maximized(Window->TopLevel);
      if (Window->RequestedFullscreen) xdg_toplevel_set_fullscreen(Window->TopLevel, nullptr);
   }

#ifdef WAYLAND_XDG_DECORATION
   if (state->DecorationManager and Window->TopLevel) {
      Window->Decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(state->DecorationManager,
         Window->TopLevel);
      if (Window->Decoration) {
         zxdg_toplevel_decoration_v1_add_listener(Window->Decoration, &decoration_listener, Window);
         zxdg_toplevel_decoration_v1_set_mode(Window->Decoration,
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
      }
   }
#endif

   wl_surface_commit(Window->Surface);
   Window->Lifecycle = WaylandWindow::Phase::Pending;
   wl_display_flush(state->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// FD callback that dispatches the events received from the compositor.

static void dispatch_events(HOSTHANDLE, APTR Data)
{
   auto state = (WaylandDriver::State *)Data;
   if ((not state->Connection) or state->Dispatching) return;

   state->Dispatching = true;
   if (wl_display_dispatch(state->Connection) < 0) kt::Log("Wayland").error("The compositor connection was lost.");
   else wl_display_flush(state->Connection);
   state->Dispatching = false;
}

//********************************************************************************************************************

WaylandDriver::WaylandDriver() : Data(new State) { }
WaylandDriver::~WaylandDriver() { close(); delete Data; }
CSTRING WaylandDriver::name() const { return "wayland"; }
DT WaylandDriver::displayType() const { return DT::WAYLAND; }

//********************************************************************************************************************
// Clipboard and drag and drop support require the compositor's data device manager.

DCAP WaylandDriver::capabilities() const
{
   auto data = Data->DataManager ? DCAP::CLIPBOARD|DCAP::DRAG_DROP : DCAP::NIL;
   return data|DCAP::WINDOW_DECOR|DCAP::CUSTOM_CURSORS;
}

//********************************************************************************************************************
// The driver is available if the environment names a Wayland display.

ERR WaylandDriver::isAvailable() const
{
   auto runtime = std::getenv("XDG_RUNTIME_DIR");
   auto name = std::getenv("WAYLAND_DISPLAY");
   return (runtime and runtime[0] and name and name[0]) ? ERR::Okay : ERR::NoSupport;
}

//********************************************************************************************************************
// Connects to the compositor and binds its globals.  wl_compositor, wl_shm and xdg_wm_base are required.

ERR WaylandDriver::open(const DriverCallbacks &Callbacks)
{
   kt::Log log("Wayland");
   if (Data->Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;

   auto runtime = std::getenv("XDG_RUNTIME_DIR");
   if ((not runtime) or (not runtime[0])) {
      log.warning("Cannot connect to Wayland: XDG_RUNTIME_DIR is not set.");
      return ERR::NoSupport;
   }

   Data->Callbacks = &Callbacks;
   Data->Connection = wl_display_connect(nullptr);
   if (not Data->Connection) {
      auto name = std::getenv("WAYLAND_DISPLAY");
      log.warning("Cannot connect to Wayland display '%s' in %s. Start a Wayland compositor or use the X11 driver.",
         (name and name[0]) ? name : "wayland-0", runtime);
      Data->Callbacks = nullptr;
      return ERR::NoSupport;
   }

   Data->Registry = wl_display_get_registry(Data->Connection);
   if ((not Data->Registry) or (wl_registry_add_listener(Data->Registry, &registry_listener, Data) < 0) or
         (wl_display_roundtrip(Data->Connection) < 0) or (wl_display_roundtrip(Data->Connection) < 0)) {
      close();
      return log.warning(ERR::SystemCall);
   }

   if ((not Data->Compositor) or (not Data->Shm) or (not Data->Shell)) {
      log.warning("The Wayland compositor does not advertise wl_compositor, wl_shm and xdg_wm_base.");
      close();
      return ERR::NoSupport;
   }

   Data->CursorTheme = wl_cursor_theme_load(nullptr, 24, Data->Shm);
   if (Data->DataManager) for (auto seat : Data->Seats) {
      seat->DataDevice = wl_data_device_manager_get_data_device(Data->DataManager, seat->Handle);
      if (seat->DataDevice) wl_data_device_add_listener(seat->DataDevice, &data_device_listener, seat);
   }

   Data->ConnectionFD = wl_display_get_fd(Data->Connection);
   if (RegisterFD(Data->ConnectionFD, RFD::READ, dispatch_events, Data) != ERR::Okay) {
      close();
      return ERR::SystemCall;
   }

   Data->Open = true;
   return ERR::Okay;
}

//********************************************************************************************************************
// Destroys all windows and protocol objects and disconnects from the compositor.  An active export is abandoned without
// a result.

ERR WaylandDriver::close()
{
   if (Data->ConnectionFD >= 0) { DeregisterFD(Data->ConnectionFD); Data->ConnectionFD = -1; }
   reset_export(Data); // An outstanding export is abandoned without a result
   while (not Data->Windows.empty()) destroyWindow(*Data->Windows.begin());

   for (auto transfer : Data->Transfers) {
      DeregisterFD(transfer->FD);
      ::close(transfer->FD);
      delete transfer;
   }
   Data->Transfers.clear();

   std::vector<WaylandSeat *> seats;
   seats.swap(Data->Seats);
   for (auto seat : seats) dispose_seat(seat);

   while (not Data->Offers.empty()) dispose_offer(Data->Offers.back());
   for (auto source : Data->Sources) { wl_data_source_destroy(source->Handle); delete source; }
   Data->Sources.clear();

   if (Data->DataManager) wl_data_device_manager_destroy(Data->DataManager);
   Data->DataManager = nullptr;
#ifdef WAYLAND_CURSOR_SHAPE
   if (Data->ShapeManager) wp_cursor_shape_manager_v1_destroy(Data->ShapeManager);
   Data->ShapeManager = nullptr;
#endif

   for (auto buffer : Data->CursorBuffers) {
      wl_buffer_destroy(buffer->Handle);
      delete buffer;
   }
   Data->CursorBuffers.clear();
   if (Data->CursorTheme) wl_cursor_theme_destroy(Data->CursorTheme);
   Data->CursorTheme = nullptr;

#ifdef WAYLAND_POINTER_CONSTRAINTS
   if (Data->Constraints) zwp_pointer_constraints_v1_destroy(Data->Constraints);
   Data->Constraints = nullptr;
#endif
#ifdef WAYLAND_RELATIVE_POINTER
   if (Data->RelativeManager) zwp_relative_pointer_manager_v1_destroy(Data->RelativeManager);
   Data->RelativeManager = nullptr;
#endif

   for (auto buffer : Data->Retired) dispose_buffer(buffer);
   Data->Retired.clear();
   for (auto output : Data->Outputs) { wl_output_destroy(output->Handle); delete output; }
   Data->Outputs.clear();

#ifdef WAYLAND_FRACTIONAL_SCALE
   if (Data->FractionalManager) wp_fractional_scale_manager_v1_destroy(Data->FractionalManager);
   if (Data->Viewporter) wp_viewporter_destroy(Data->Viewporter);
   Data->FractionalManager = nullptr;
   Data->Viewporter = nullptr;
#endif
#ifdef WAYLAND_XDG_DECORATION
   if (Data->DecorationManager) zxdg_decoration_manager_v1_destroy(Data->DecorationManager);
   Data->DecorationManager = nullptr;
#endif

   if (Data->Shell) xdg_wm_base_destroy(Data->Shell);
   if (Data->Shm) wl_shm_destroy(Data->Shm);
   if (Data->Compositor) wl_compositor_destroy(Data->Compositor);
   if (Data->Registry) wl_registry_destroy(Data->Registry);
   if (Data->Connection) wl_display_disconnect(Data->Connection);

   Data->Connection = nullptr;
   Data->Shell = nullptr;
   Data->Shm = nullptr;
   Data->Compositor = nullptr;
   Data->Registry = nullptr;
   Data->Callbacks = nullptr;
   Data->Open = false;
   return ERR::Okay;
}

//********************************************************************************************************************
// Creates a window for a Display object.  The surface is created immediately, but the window is not shown until
// showWindow() is called.

ERR WaylandDriver::createWindow(extDisplay *DisplayObject, HOSTWINDOW &Handle)
{
   Handle = nullptr;
   if ((not Data->Open) or (not DisplayObject)) return ERR::NotInitialised;

   auto window = new(std::nothrow) WaylandWindow;
   if (not window) return ERR::AllocMemory;

   window->Owner = Data;
   window->Display = DisplayObject;
   window->Width = DisplayObject->Width;
   window->Height = DisplayObject->Height;
   window->NormalWidth = window->Width;
   window->NormalHeight = window->Height;
   window->Title = "Kotuku";
   window->Cursor = Data->DefaultCursor;
   window->CursorVisible = Data->DefaultCursorVisible;
   window->RequestedMaximised = (DisplayObject->Flags & SCR::MAXIMISE) != SCR::NIL;
   window->RequestedFullscreen = (not DisplayObject->PopOverID) and
      ((DisplayObject->Flags & SCR::BORDERLESS) != SCR::NIL);

   if (auto error = create_protocol_window(window, false); error != ERR::Okay) { delete window; return error; }

   Data->Windows.insert(window);
   Handle = window;
   return ERR::Okay;
}

//********************************************************************************************************************
// Destroys a window, its popups and its buffers.  Buffers held by the compositor are retired until they are released.

ERR WaylandDriver::destroyWindow(HOSTWINDOW Window)
{
   auto window = (WaylandWindow *)Window;
   if ((not window) or (not Data->Windows.erase(window))) return ERR::NoSupport;

   unmap_children(window);
   retire_cursor_buffer(window->CustomCursor);
   for (auto seat : Data->Seats) {
      if (seat->KeyboardWindow IS window) { stop_repeat(seat); seat->KeyboardWindow = nullptr; }
   }

   destroy_protocol_window(window);
   for (auto buffer : window->Buffers) {
      buffer->Window = nullptr;
      if (buffer->Busy) Data->Retired.push_back(buffer);
      else dispose_buffer(buffer);
   }

   delete window;
   if (Data->Connection) wl_display_flush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the window's wl_surface.

ERR WaylandDriver::nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle)
{
   if (not Data->Windows.contains((WaylandWindow *)Window)) return ERR::NoSupport;
   NativeHandle = ((WaylandWindow *)Window)->Surface;
   if (not NativeHandle) return ERR::NoSupport;
   return ERR::Okay;
}

//********************************************************************************************************************
// Shows the window, recreating its protocol objects if it was hidden.

ERR WaylandDriver::showWindow(HOSTWINDOW Window, bool Maximise)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;

   if (not window->Surface) {
      if (auto error = create_protocol_window(window, Maximise); error != ERR::Okay) return error;
   }
   window->Visible = true;

   if (window->TopLevel and (window->RequestedMaximised != Maximise)) {
      window->RequestedMaximised = Maximise;
      if (Maximise) xdg_toplevel_set_maximized(window->TopLevel);
      else xdg_toplevel_unset_maximized(window->TopLevel);
   }

   if ((window->Lifecycle != WaylandWindow::Phase::Pending) and window->SurfaceID and Data->Callbacks)
      Data->Callbacks->ExposeRegion(window->SurfaceID, 0, 0, window->Width, window->Height);
   if (not Data->Windows.contains(window)) return ERR::NoSupport;

   try_present(window);
   wl_display_flush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// Hides the window and its popups.  Wayland cannot hide a surface, so the protocol objects are destroyed.

ERR WaylandDriver::hideWindow(HOSTWINDOW Window)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;

   window->Visible = false;
   unmap_children(window);
   if (not Data->Windows.contains(window)) return ERR::Okay;

   destroy_protocol_window(window);
   wl_display_flush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// Resizes the window content.  Wayland does not let clients position windows, so moves are refused, as are resizes
// while the window is maximised or fullscreen.

ERR WaylandDriver::resizeWindow(HOSTWINDOW Window, int X, int Y, int Width, int Height)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   if ((X != 0x7fffffff) or (Y != 0x7fffffff)) return ERR::NoSupport;
   if ((Width <= 0) or (Height <= 0)) return ERR::InvalidDimension;
   if (window->Popup) return ERR::NoSupport;
   if ((Width > 8192) or (Height > 8192)) return ERR::InvalidDimension;
   if ((window->Width IS Width) and (window->Height IS Height)) return ERR::Okay;
   if (window->Maximised or window->Fullscreen or window->RequestedMaximised or
         window->RequestedFullscreen) return ERR::NoSupport;
   if ((window->MinWidth and Width < window->MinWidth) or (window->MinHeight and Height < window->MinHeight) or
         (window->MaxWidth and Width > window->MaxWidth) or (window->MaxHeight and Height > window->MaxHeight))
      return ERR::InvalidDimension;

   window->Width = Width;
   window->Height = Height;
   window->NormalWidth = Width;
   window->NormalHeight = Height;
   window->Presented.clear();
   retire_obsolete(window);

   if (window->TopLevel) {
      const auto frame = window_frame(window);
      xdg_surface_set_window_geometry(window->ShellSurface, 0, 0, frame.Width, frame.Height);
      wl_display_flush(Data->Connection);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Asks the compositor to minimise the window.

ERR WaylandDriver::minimiseWindow(HOSTWINDOW Window)
{
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or (not window->TopLevel)) return ERR::NoSupport;
   xdg_toplevel_set_minimized(window->TopLevel);
   wl_display_flush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// Requests fullscreen mode for a top-level window.  The change takes effect when the compositor configures the window.

ERR WaylandDriver::setFullscreen(HOSTWINDOW Window, bool Enabled)
{
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or window->Popup) return ERR::NoSupport;
   if (window->RequestedFullscreen IS Enabled) return ERR::Okay;

   window->RequestedFullscreen = Enabled;
   if (window->TopLevel) {
      if (Enabled) xdg_toplevel_set_fullscreen(window->TopLevel, nullptr);
      else xdg_toplevel_unset_fullscreen(window->TopLevel);
      wl_display_flush(Data->Connection);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the size of the window when it is neither maximised nor fullscreen.

ERR WaylandDriver::normalWindowSize(HOSTWINDOW Window, int &Width, int &Height)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   Width = window->NormalWidth;
   Height = window->NormalHeight;
   return ERR::Okay;
}

//********************************************************************************************************************
// Sets the window title, which is drawn in the client-side decorations and passed to the compositor.

ERR WaylandDriver::setWindowTitle(HOSTWINDOW Window, CSTRING Title)
{
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or (not Title)) return ERR::NullArgs;

   window->Title = Title;
   window->DecorDirty = true;
   try_present(window);

   if (window->TopLevel) xdg_toplevel_set_title(window->TopLevel, Title);
   wl_display_flush(Data->Connection);
   return ERR::Okay;
}

//********************************************************************************************************************
// The app ID is matched to a .desktop entry by name or by its StartupWMClass key.  The default matches the installed
// org.kotuku.Origo.desktop entry.

ERR WaylandDriver::setWindowClass(HOSTWINDOW Window, CSTRING Class)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NullArgs;

   window->AppID = Class ? Class : "";
   if (window->TopLevel) {
      xdg_toplevel_set_app_id(window->TopLevel, window->AppID.empty() ? DEFAULT_APP_ID : window->AppID.c_str());
      wl_display_flush(Data->Connection);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the window title.

ERR WaylandDriver::windowTitle(HOSTWINDOW Window, std::string &Title)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   Title = window->Title;
   return ERR::Okay;
}

//********************************************************************************************************************
// Sets the window's size limits.  A negative value keeps the current limit.  xdg-shell does not support aspect ratios,
// so EnforceAspect results in ERR::NoSupport after the limits are applied.

ERR WaylandDriver::setSizeHints(HOSTWINDOW Window, int MinW, int MinH, int MaxW, int MaxH, bool EnforceAspect)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   if (window->Popup) return ERR::NoSupport;

   const int min_width = MinW < 0 ? window->MinWidth : MinW;
   const int min_height = MinH < 0 ? window->MinHeight : MinH;
   const int max_width = MaxW < 0 ? window->MaxWidth : MaxW;
   const int max_height = MaxH < 0 ? window->MaxHeight : MaxH;

   if ((min_width > 8192) or (min_height > 8192) or (max_width > 8192) or (max_height > 8192))
      return ERR::InvalidDimension;
   if ((max_width and min_width > max_width) or (max_height and min_height > max_height))
      return ERR::InvalidDimension;

   window->MinWidth = min_width;
   window->MinHeight = min_height;
   window->MaxWidth = max_width;
   window->MaxHeight = max_height;

   if (window->TopLevel) {
      set_size_hints(window);
      wl_surface_commit(window->Surface);
   }
   wl_display_flush(Data->Connection);
   return EnforceAspect ? ERR::NoSupport : ERR::Okay;
}

//********************************************************************************************************************
// Wayland does not reveal window positions, so the position is reported as (0, 0) with ERR::NoSupport.  The size is
// still returned.

ERR WaylandDriver::windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   X = Y = 0;
   Width = window->Width;
   Height = window->Height;
   return ERR::NoSupport;
}

//********************************************************************************************************************
// Returns the size of the client-side decorations around the content.

ERR WaylandDriver::frameMargins(HOSTWINDOW Window, int &Left, int &Top, int &Right, int &Bottom)
{
   if (not Data->Windows.contains((WaylandWindow *)Window)) return ERR::NoSupport;
   const auto frame = window_frame((WaylandWindow *)Window);
   Left = Right = Bottom = frame.Border;
   Top = frame.Top;
   return ERR::Okay;
}

//********************************************************************************************************************
// Associates the window with a Surface object.

ERR WaylandDriver::setWindowSurface(HOSTWINDOW Window, OBJECTID SurfaceID)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   window->SurfaceID = SurfaceID;
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the Surface object associated with the window.

ERR WaylandDriver::windowSurface(HOSTWINDOW Window, OBJECTID &SurfaceID)
{
   auto window = (WaylandWindow *)Window;
   if (not Data->Windows.contains(window)) return ERR::NoSupport;
   SurfaceID = window->SurfaceID;
   return ERR::Okay;
}

//********************************************************************************************************************
// Describes the output that holds the given display, or the first output.  Wayland does not report density, so 96 DPI
// is assumed.

ERR WaylandDriver::displayInfo(DisplayInfo &Info)
{
   if (not Data->Open) return ERR::NotInitialised;

   auto output = Data->Outputs.empty() ? nullptr : Data->Outputs.front();
   if (Info.DisplayID) for (auto window : Data->Windows) {
      if (window->Display and (window->Display->UID IS Info.DisplayID) and (not window->Outputs.empty())) {
         output = window->Outputs.front();
         break;
      }
   }

   if (not Info.Width) Info.Width = output and output->Width ? output->Width : Data->OutputWidth;
   if (not Info.Height) Info.Height = output and output->Height ? output->Height : Data->OutputHeight;
   Info.MonitorWidth = Info.VirtualWidth = output and output->Width ? output->Width : Data->OutputWidth;
   Info.MonitorHeight = Info.VirtualHeight = output and output->Height ? output->Height : Data->OutputHeight;
   Info.PhysicalWidth = output ? output->PhysicalWidth : 0;
   Info.PhysicalHeight = output ? output->PhysicalHeight : 0;
   Info.RefreshRate = output ? float(output->Refresh) / 1000.0f : 0;
   Info.HDensity = Info.VDensity = 96;
   Info.BitsPerPixel = 32;
   Info.BytesPerPixel = 4;
   Info.AccelFlags = ACF::NIL;
   return ERR::Okay;
}

//********************************************************************************************************************
// Wayland does not report display density, so 96 DPI is assumed.

ERR WaylandDriver::density(HOSTWINDOW, int &Horizontal, int &Vertical)
{
   Horizontal = Vertical = 96;
   return Data->Open ? ERR::Okay : ERR::NotInitialised;
}

//********************************************************************************************************************
// Lists the current resolution of each output.  Wayland clients cannot change display modes.

ERR WaylandDriver::resolutions(std::vector<resolution> &List)
{
   if (not Data->Open) return ERR::NotInitialised;
   for (auto output : Data->Outputs) if ((output->Width > 0) and (output->Height > 0))
      List.emplace_back(output->Width, output->Height, 32);
   if (List.empty()) List.emplace_back(Data->OutputWidth, Data->OutputHeight, 32);
   return ERR::Okay;
}

//********************************************************************************************************************
// Window buffers use 32-bit ARGB pixels, matching WL_SHM_FORMAT_ARGB8888.

ERR WaylandDriver::pixelFormat(ColourFormat &Format)
{
   Format = { .RedMask = 0xff, .GreenMask = 0xff, .BlueMask = 0xff, .AlphaMask = 0xff,
      .RedPos = 16, .GreenPos = 8, .BluePos = 0, .AlphaPos = 24, .BitsPerPixel = 32 };
   return ERR::Okay;
}

//********************************************************************************************************************
// Copies an area of a bitmap to the window's staged content and presents it.  Composited windows are premultiplied as
// Wayland requires, and other windows are made opaque.

ERR WaylandDriver::present(HOSTWINDOW Window, extBitmap *Source, int X, int Y, int Width, int Height,
   int XDest, int YDest)
{
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or (not Source) or (not Source->Data)) return ERR::NoSupport;
   if ((Source->BitsPerPixel != 32) or (Source->BytesPerPixel != 4)) return ERR::NoSupport;

   const int width = window->Width;
   const int height = window->Height;
   if ((width <= 0) or (height <= 0) or (width > 8192) or (height > 8192)) return ERR::InvalidDimension;
   if ((X < 0) or (Y < 0) or (XDest < 0) or (YDest < 0) or (Width < 1) or (Height < 1) or
         (X + Width > Source->Width) or (Y + Height > Source->Height) or
         (XDest + Width > width) or (YDest + Height > height)) return ERR::InvalidDimension;

   const size_t length = size_t(width) * size_t(height) * 4;
   if (window->Staged.size() != length) window->Staged.assign(length, 0);
   const bool alpha = window->Display and ((window->Display->Flags & SCR::COMPOSITE) != SCR::NIL);
   const bool premul = (Source->Flags & BMF::PREMUL) != BMF::NIL;

   for (int row = 0; row < Height; row++) {
      auto source = (const uint32_t *)(Source->Data + (Y + row) * Source->LineWidth) + X;
      auto target = (uint32_t *)(window->Staged.data() + size_t(YDest + row) * size_t(width) * 4) + XDest;
      for (int column = 0; column < Width; column++) {
         const uint32_t pixel = source[column];
         if (alpha and (not premul)) {
            const uint32_t a = pixel >> 24;
            const uint32_t r = ((pixel >> 16) & 0xff) * a / 255;
            const uint32_t g = ((pixel >> 8) & 0xff) * a / 255;
            const uint32_t b = (pixel & 0xff) * a / 255;
            target[column] = (a << 24) | (r << 16) | (g << 8) | b;
         }
         else target[column] = alpha ? pixel : pixel | 0xff000000;
      }
   }

   window->StageWidth = width;
   window->StageHeight = height;
   window->Dirty = true;
   try_present(window);
   return ERR::Okay;
}

//********************************************************************************************************************
// Sends buffered requests to the compositor.

ERR WaylandDriver::flush()
{
   if (not Data->Connection) return ERR::NotInitialised;
   return wl_display_flush(Data->Connection) < 0 and errno != EAGAIN ? ERR::SystemCall : ERR::Okay;
}

//********************************************************************************************************************
// Sets the cursor for a window, or the default cursor of all windows if Window is null.  A custom cursor is released.

ERR WaylandDriver::setCursor(HOSTWINDOW Window, PTC CursorID)
{
   if ((CursorID != PTC::INVISIBLE) and (not cursor_name(CursorID))) return ERR::NoSupport;
   if ((CursorID != PTC::INVISIBLE) and (not Data->CursorTheme)) {
#ifdef WAYLAND_CURSOR_SHAPE
      if (not Data->ShapeManager) return ERR::NoSupport;
#else
      return ERR::NoSupport;
#endif
   }

   if (Window and (not Data->Windows.contains((WaylandWindow *)Window))) return ERR::NoSupport;
   if (not Window) Data->DefaultCursor = CursorID;

   for (auto window : Data->Windows) {
      if (Window and window != Window) continue;
      if (window->CustomCursor) { retire_cursor_buffer(window->CustomCursor); window->CustomCursor = nullptr; }
      window->Cursor = CursorID;
      for (auto seat : Data->Seats) if (seat->PointerWindow IS window) apply_cursor(seat);
   }
   return flush();
}

//********************************************************************************************************************
// Sets a cursor image of up to 256x256 pixels for a window.  The image is copied to a premultiplied ARGB buffer.

ERR WaylandDriver::setCustomCursor(HOSTWINDOW Window, extBitmap *Image, int HotX, int HotY)
{
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or (not Image) or (not Image->Data)) return ERR::NullArgs;
   if ((Image->Width < 1) or (Image->Height < 1) or (Image->Width > 256) or (Image->Height > 256) or
         (HotX < 0) or (HotY < 0) or (HotX >= Image->Width) or (HotY >= Image->Height)) return ERR::InvalidDimension;
   if (Image->BytesPerPixel != 4) return ERR::NoSupport;

   const size_t length = size_t(Image->Width) * size_t(Image->Height) * 4;
   int fd = memfd_create("kotuku-wayland-cursor", MFD_CLOEXEC);
   if (fd < 0) return ERR::CreateResource;
   if (ftruncate(fd, off_t(length)) < 0) { ::close(fd); return ERR::SystemCall; }
   auto pixels = mmap(nullptr, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
   if (pixels IS MAP_FAILED) { ::close(fd); return ERR::SystemCall; }

   const bool premul = (Image->Flags & BMF::PREMUL) != BMF::NIL;
   for (int row = 0; row < Image->Height; row++) {
      auto source = (const uint32_t *)(Image->Data + row * Image->LineWidth);
      auto target = (uint32_t *)pixels + row * Image->Width;
      for (int column = 0; column < Image->Width; column++) {
         const uint32_t pixel = source[column];
         if (premul) target[column] = pixel;
         else {
            const uint32_t a = pixel >> 24;
            target[column] = (a << 24) | (((pixel >> 16) & 0xff) * a / 255 << 16) |
               (((pixel >> 8) & 0xff) * a / 255 << 8) | ((pixel & 0xff) * a / 255);
         }
      }
   }

   auto pool = wl_shm_create_pool(Data->Shm, fd, int(length));
   auto handle = pool ? wl_shm_pool_create_buffer(pool, 0, Image->Width, Image->Height,
      Image->Width * 4, WL_SHM_FORMAT_ARGB8888) : nullptr;
   if (pool) wl_shm_pool_destroy(pool);
   munmap(pixels, length);
   ::close(fd);
   if (not handle) return ERR::CreateResource;

   auto buffer = new(std::nothrow) CursorBuffer;
   if (not buffer) { wl_buffer_destroy(handle); return ERR::AllocMemory; }
   buffer->Owner = Data;
   buffer->Handle = handle;
   buffer->Width = Image->Width;
   buffer->Height = Image->Height;
   buffer->HotX = HotX;
   buffer->HotY = HotY;
   wl_buffer_add_listener(handle, &cursor_buffer_listener, buffer);
   Data->CursorBuffers.push_back(buffer);

   retire_cursor_buffer(window->CustomCursor);
   window->CustomCursor = buffer;
   window->Cursor = PTC::CUSTOM;
   for (auto seat : Data->Seats) if (seat->PointerWindow IS window) apply_cursor(seat);
   return flush();
}

//********************************************************************************************************************
// Shows or hides the cursor over a window's content, or over all windows if Window is null.

ERR WaylandDriver::showCursor(HOSTWINDOW Window, bool Visible)
{
   if (Window and (not Data->Windows.contains((WaylandWindow *)Window))) return ERR::NoSupport;
   if (not Window) Data->DefaultCursorVisible = Visible;

   for (auto window : Data->Windows) {
      if (Window and window != Window) continue;
      window->CursorVisible = Visible;
      for (auto seat : Data->Seats) if (seat->PointerWindow IS window) apply_cursor(seat);
   }
   return flush();
}

//********************************************************************************************************************

ERR WaylandDriver::warpPointer(HOSTWINDOW, int, int) { return ERR::NoSupport; }
ERR WaylandDriver::pointerPosition(double &, double &) { return ERR::NoSupport; }

//********************************************************************************************************************
// Locks the pointer to the window and enables relative motion.  The pointer must be over the window.

ERR WaylandDriver::grabPointer(HOSTWINDOW Window)
{
#if defined(WAYLAND_POINTER_CONSTRAINTS) and defined(WAYLAND_RELATIVE_POINTER)
   auto window = (WaylandWindow *)Window;
   if ((not Data->Windows.contains(window)) or (not window->Surface) or (not Data->Constraints) or
         (not Data->RelativeManager)) return ERR::NoSupport;

   for (auto seat : Data->Seats) {
      if ((seat->PointerWindow != window) or (not seat->Pointer)) continue;
      if (seat->Locked) return seat->LockedWindow IS window ? ERR::Okay : ERR::NoSupport;

      seat->Locked = zwp_pointer_constraints_v1_lock_pointer(Data->Constraints, window->Surface,
         seat->Pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
      if (not seat->Locked) return ERR::CreateResource;
      seat->Relative = zwp_relative_pointer_manager_v1_get_relative_pointer(Data->RelativeManager, seat->Pointer);
      if (not seat->Relative) {
         zwp_locked_pointer_v1_destroy(seat->Locked);
         seat->Locked = nullptr;
         return ERR::CreateResource;
      }

      seat->LockedWindow = window;
      zwp_locked_pointer_v1_add_listener(seat->Locked, &locked_listener, seat);
      zwp_relative_pointer_v1_add_listener(seat->Relative, &relative_listener, seat);
      return flush();
   }
#endif
   return ERR::NoSupport;
}

//********************************************************************************************************************
// Releases all pointer locks.

ERR WaylandDriver::ungrabPointer()
{
#if defined(WAYLAND_POINTER_CONSTRAINTS) and defined(WAYLAND_RELATIVE_POINTER)
   bool unlocked = false;
   for (auto seat : Data->Seats) {
      if (seat->Locked) {
         zwp_locked_pointer_v1_destroy(seat->Locked);
         seat->Locked = nullptr;
         unlocked = true;
      }
      if (seat->Relative) { zwp_relative_pointer_v1_destroy(seat->Relative); seat->Relative = nullptr; }
      seat->LockedWindow = nullptr;
   }
   return unlocked ? flush() : ERR::ResourceNotLocked;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************
// Replaces the clipboard content with text and publishes it on every seat.

ERR WaylandDriver::clipboardAddText(CSTRING Text)
{
   if (not Data->DataManager) return ERR::NoSupport;

   Data->ClipboardText = Text ? Text : "";
   Data->ClipboardUris.clear();
   Data->ClipboardIsFiles = false;
   Data->ClipboardGeneration++;
   for (auto seat : Data->Seats) publish_selection(seat);
   return ERR::Okay;
}

//********************************************************************************************************************
// Replaces the clipboard content with a list of files, offered as text/uri-list.  Only CLIPTYPE::FILE is supported.

ERR WaylandDriver::clipboardAddFiles(CLIPTYPE Type, const std::vector<std::string> &Paths, bool Cut)
{
   if ((not Data->DataManager) or (Type != CLIPTYPE::FILE)) return ERR::NoSupport;
   ClipboardFilePayload payload;
   if (auto error = encode_clipboard_files(Paths, Cut, payload); error != ERR::Okay) return error;

   Data->ClipboardUris = std::move(payload.URIList);
   Data->ClipboardText.clear();
   Data->ClipboardIsFiles = true;
   Data->ClipboardGeneration++;
   for (auto seat : Data->Seats) publish_selection(seat);
   return ERR::Okay;
}

//********************************************************************************************************************
// Clears the clipboard and withdraws our selection.

ERR WaylandDriver::clipboardClear()
{
   if (not Data->DataManager) return ERR::NoSupport;

   Data->ClipboardGeneration = 0;
   Data->ClipboardText.clear();
   Data->ClipboardUris.clear();

   for (auto seat : Data->Seats) if (seat->DataDevice and seat->SelectionSerial) {
      wl_data_device_set_selection(seat->DataDevice, nullptr, seat->SelectionSerial);
      seat->SelectionSource = nullptr;
      seat->PublishedGeneration = 0;
   }
   return flush();
}

//********************************************************************************************************************
// Outbound dragging; see export_send() and the notes that precede it.  Wayland does not report the position of the
// pointer or of our windows on the desktop, so a point is foreign if the pointer has left the window that holds the
// primary button, or if the compositor's drag is not over one of our windows.

static bool in_surface(const WaylandWindow *Window, double X, double Y)
{
   const auto frame = window_frame(Window);
   return (X >= -frame.Border) and (Y >= -frame.Top) and (X < frame.Width - frame.Border) and
      (Y < frame.Height - frame.Top);
}

//********************************************************************************************************************
// True if the drag is outside of our windows.

static bool foreign_point(WaylandDriver::State *Data)
{
   auto &e = Data->Export;
   if ((e.Stage IS WaylandExport::Phase::Hover) and e.Granted and (not e.Cancelled)) return not e.OwnWindow;
   if (e.Stage IS WaylandExport::Phase::Dropped) return true;

   for (auto seat : Data->Seats) {
      if ((seat->ContentButtons & 0x0001) and seat->PointerWindow) {
         return not in_surface(seat->PointerWindow, seat->X, seat->Y);
      }
   }
   return false;
}

//********************************************************************************************************************
// Starts an outbound drag of files and text.  The drag is started from the implicit grab of a primary button press in
// one of our windows.  Returns ERR::InUse if an export is already active.

static ERR start_export(WaylandDriver::State *Data, const std::vector<std::string> &Files, const std::string *Text,
   HOSTWINDOW Exclude)
{
   auto &e = Data->Export;
   if (e.Stage != WaylandExport::Phase::None) return ERR::InUse;
   if ((not Data->DataManager) or
         (wl_data_device_manager_get_version(Data->DataManager) < WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)) {
      return ERR::NoSupport;
   }

   // The drag must be started from the implicit grab of the primary button press.

   WaylandSeat *seat = nullptr;
   for (auto candidate : Data->Seats) {
      if ((candidate->ContentButtons & 0x0001) and candidate->PointerWindow and candidate->PointerWindow->Surface and
            candidate->DataDevice and candidate->PressSerial) {
         seat = candidate;
         break;
      }
   }
   if (not seat) return ERR::NotPossible;

   // Paths are native already.  Folders are offered without a trailing separator, as file managers offer them.

   WaylandExport offer;
   for (auto &file : Files) {
      std::string_view path(file);
      while ((path.size() > 1) and (path.back() IS '/')) path.remove_suffix(1);
      if (path.empty() or (path[0] != '/')) continue;
      offer.Uris.append(encode_file_uri(path)).append("\r\n");
   }
   if (Text) {
      offer.Text = *Text;
      offer.HasText = true;
   }
   if (offer.Uris.empty() and (not offer.HasText)) return ERR::NoData;

   offer.Marker = export_marker_prefix() + "id=" + std::to_string(++Data->ExportCounter);
   offer.Source = wl_data_device_manager_create_data_source(Data->DataManager);
   if (not offer.Source) return ERR::CreateResource;

   wl_data_source_add_listener(offer.Source, &export_source_listener, Data);
   if (not offer.Uris.empty()) wl_data_source_offer(offer.Source, "text/uri-list");
   if (offer.HasText) {
      for (auto mime : { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" }) {
         wl_data_source_offer(offer.Source, mime);
      }
   }
   wl_data_source_offer(offer.Source, offer.Marker.c_str());
   wl_data_source_set_actions(offer.Source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

   offer.Seat = seat;
   offer.Exclude = Exclude;
   offer.Stage = WaylandExport::Phase::Hover;
   e = std::move(offer);

   wl_data_device_start_drag(seat->DataDevice, e.Source, seat->PointerWindow->Surface, nullptr, seat->PressSerial);
   return wl_display_flush(Data->Connection) < 0 ? ERR::SystemCall : ERR::Okay;
}

//********************************************************************************************************************
// Called when the primary button is released during an export.  DROP_INTERNAL means that the drop is delivered to one
// of our own windows.  ERR::Okay means that the result will follow from the source events.

static ERR drop_export(WaylandDriver::State *Data)
{
   auto &e = Data->Export;
   if (e.Stage IS WaylandExport::Phase::None) return ERR::NoSupport;
   if (e.Stage IS WaylandExport::Phase::Dropped) return ERR::Okay; // The result follows from the source

   if (e.OwnDrop) {
      reset_export(Data);
      return DisplayDriver::DROP_INTERNAL;
   }

   if (not e.Granted) {
      // The compositor never started the drag, so the release came from our own pointer.
      bool internal = false;
      if (auto seat = e.Seat; seat and seat->PointerWindow) {
         internal = window_frame(seat->PointerWindow).contains(seat->X, seat->Y);
      }

      reset_export(Data);
      return internal ? DisplayDriver::DROP_INTERNAL : ERR::Cancelled;
   }

   // Nothing accepted the drop, or the release was not reported by the compositor.

   reset_export(Data);
   return ERR::Cancelled;
}

//********************************************************************************************************************
// Destroying the source ends the compositor's drag.

static void cancel_export(WaylandDriver::State *Data)
{
   if (Data->Export.Stage != WaylandExport::Phase::Hover) return;
   const bool granted = Data->Export.Granted;
   reset_export(Data);

   // A granted drag consumed our pointer leave, so its held button has no physical release to follow.

   if (granted) release_export_button(Data);
}

//********************************************************************************************************************

bool WaylandDriver::foreignPoint(double, double, HOSTWINDOW) { return foreign_point(Data); }

//********************************************************************************************************************

ERR WaylandDriver::startDragExport(const std::vector<std::string> &Files, const std::string *Text, HOSTWINDOW Exclude)
{
   return start_export(Data, Files, Text, Exclude);
}

//********************************************************************************************************************

ERR WaylandDriver::dropDragExport() { return drop_export(Data); }
void WaylandDriver::cancelDragExport() { cancel_export(Data); }

#ifdef WAYLAND_DRIVER_TESTS
#include "../../tests/wayland_drop_fixture.cpp"
#endif

}
