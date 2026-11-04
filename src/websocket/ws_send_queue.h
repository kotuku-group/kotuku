// WebSocket send queue (RFC 6455 sections 5.4, 5.5 and 7).
//
// Messages and control frames are queued as payloads and encoded one frame at a time, so a queued message costs its
// payload plus a few bytes of bookkeeping.  The caller obtains the bytes for the current frame with next_chunk(), hands
// them to the transport and reports how many the transport accepted with consume().  Unaccepted bytes are produced
// again by the next call to next_chunk(), so partial writes resume from the exact offset without duplication or
// reordering.  Control frames are only ever started at frame boundaries.
//
// This file has no object-system dependencies.  Mask keys are obtained through a caller-supplied generator so that
// the module can use the Crypto module and the unit tests can use fixed keys.

#pragma once

#include <kotuku/config.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

#include "ws_frame.h"

namespace ws {

// Fills Key with a fresh masking key.  Returns false on failure.

using MaskGenerator = bool (*)(MaskKey &Key);

enum class QueueResult : uint8_t {
   OKAY,
   LIMIT,   // Accepting the message would exceed the send limit
   CLOSED,  // A Close frame has been queued; no further frames are accepted
   INVALID  // The frame cannot be encoded (for example a control payload over 125 bytes)
};

class SendQueue {
public:
   // FragmentSize is the largest payload per data frame; zero sends every message as a single frame.  Limit bounds the
   // encoded size of everything queued and not yet consumed; zero means no limit.  Clients must supply a mask
   // generator, servers must not.

   SendQueue(Role LocalRole, MaskGenerator Generator, uint64_t FragmentSize = 0, uint64_t Limit = 0) noexcept
      : generator(Generator), fragment_size(FragmentSize), limit(Limit), role(LocalRole) { }

   // Returns the number of bytes that a data message of PayloadLength bytes occupies once framed.

   [[nodiscard]] uint64_t encoded_size(uint64_t PayloadLength) const noexcept;

   // Queues a complete data message (TEXT or BINARY) behind any queued messages.  The payload is copied.

   QueueResult push_message(Opcode Type, std::span<const uint8_t> Payload);

   // Queues a Ping or Pong ahead of queued data, to be sent at the next frame boundary.  A Pong replaces the payload
   // of a Pong that has not yet been started (RFC 6455 section 5.5.3).  Pings count towards the send limit unless
   // Exempt is set; Pongs never do.

   QueueResult push_control(Opcode Op, std::span<const uint8_t> Payload, bool Exempt = false);

   // Queues a Close frame.  The payload must already be encoded (see encode_close_payload()).  When DiscardData is set,
   // queued Pings, Pongs and data that have not been started are dropped, so the Close follows the frame in progress.
   // Otherwise the Close is sent after all queued frames.  Later pushes are refused with CLOSED.

   QueueResult push_close(std::span<const uint8_t> Payload, bool DiscardData);

   // Drops data messages, Pings and Pongs that have not been started.  The frame in progress is completed so that the
   // byte stream stays valid, but the remaining fragments of its message are not sent.

   void discard_unsent() noexcept;

   // Returns the bytes to write next, starting a new frame if necessary.  The result refers to Scratch, which must be
   // at least MAX_FRAME_HEADER bytes.  An empty result means that there is nothing to send, or that a mask key could
   // not be generated (see has_failed()).

   std::span<const uint8_t> next_chunk(std::span<uint8_t> Scratch) noexcept;

   // Records that the transport accepted Count bytes of the most recent chunk.

   void consume(size_t Count) noexcept;

   [[nodiscard]] uint64_t pending() const noexcept { return pending_bytes; }
   [[nodiscard]] bool idle() const noexcept { return pending_bytes IS 0; }
   [[nodiscard]] bool close_queued() const noexcept { return close_state != CloseState::NONE; }
   [[nodiscard]] bool close_sent() const noexcept { return close_state IS CloseState::SENT; }
   [[nodiscard]] bool has_failed() const noexcept { return failed; }
   [[nodiscard]] uint64_t limit_bytes() const noexcept { return limit; }

   void set_limit(uint64_t Limit) noexcept { limit = Limit; }
   void set_fragment_size(uint64_t Size) noexcept;

private:
   struct DataMessage {
      std::vector<uint8_t> payload;
      uint64_t framed = 0;  // Payload bytes assigned to frames that have been started
      uint64_t encoded = 0; // Encoded size of the frames not yet started, including their headers
      Opcode type = Opcode::BINARY;
   };

   struct ControlFrame {
      std::array<uint8_t, MAX_CONTROL_PAYLOAD> payload = {};
      uint8_t size = 0;
      Opcode opcode = Opcode::PING;
   };

   enum class FrameSource : uint8_t { NONE, DATA, CONTROL, CLOSE };
   enum class CloseState : uint8_t { NONE, QUEUED, STARTED, SENT };

   bool start_frame() noexcept;
   void finish_frame() noexcept;
   [[nodiscard]] uint64_t frame_size(uint64_t PayloadLength) const noexcept;
   [[nodiscard]] bool masked() const noexcept { return role IS Role::CLIENT; }

   std::deque<DataMessage> messages;
   std::deque<ControlFrame> controls;
   ControlFrame close_frame;
   ControlFrame active_control;               // Copy of the control or Close frame in progress
   std::array<uint8_t, MAX_FRAME_HEADER> header = {};
   MaskKey mask = {};
   const uint8_t *payload = nullptr;          // Payload of the frame in progress
   uint64_t payload_size = 0;
   uint64_t payload_sent = 0;
   uint64_t pending_bytes = 0;                // Encoded bytes queued and not yet consumed
   MaskGenerator generator;
   uint64_t fragment_size;
   uint64_t limit;
   size_t last_chunk = 0;                     // Size of the chunk returned by the last next_chunk()
   uint8_t header_size = 0;
   uint8_t header_sent = 0;
   Role role;
   FrameSource source = FrameSource::NONE;    // Origin of the frame in progress
   CloseState close_state = CloseState::NONE;
   bool truncate_message = false;             // Drop the rest of the active message once its frame completes
   bool data_closed = false;                  // Unsent data was discarded; no further messages are accepted
   bool failed = false;
};

} // namespace ws
