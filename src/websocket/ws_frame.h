// WebSocket frame encoder, resumable frame decoder and message reader (RFC 6455 section 5).
//
// The decoder consumes an arbitrary byte stream and never relies on frame-aligned input.  Payload bytes are unmasked in
// place and returned as slices of the caller's buffer, so no allocation is made beyond the caller's buffers.  This file
// has no object-system dependencies.

#pragma once

#include <kotuku/config.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ws_utf8.h"

namespace ws {

constexpr size_t MAX_CONTROL_PAYLOAD = 125;
constexpr size_t MAX_CLOSE_REASON    = MAX_CONTROL_PAYLOAD - 2;
constexpr size_t MAX_FRAME_HEADER    = 14; // 2 + 8 byte extended length + 4 byte mask

// Close codes produced by the engine.  The values match the public WSC constants.

constexpr int CLOSE_NORMAL           = 1000;
constexpr int CLOSE_PROTOCOL_ERROR   = 1002;
constexpr int CLOSE_NO_STATUS        = 1005;
constexpr int CLOSE_INVALID_PAYLOAD  = 1007;
constexpr int CLOSE_MESSAGE_TOO_BIG  = 1009;

enum class Opcode : uint8_t {
   CONTINUATION = 0x0,
   TEXT         = 0x1,
   BINARY       = 0x2,
   CLOSE        = 0x8,
   PING         = 0x9,
   PONG         = 0xa
};

// The local endpoint's role.  Clients mask outgoing frames and require unmasked incoming frames; servers do the
// opposite.

enum class Role : uint8_t { CLIENT, SERVER };

[[nodiscard]] constexpr bool is_control(Opcode Op) noexcept { return (uint8_t(Op) & 0x08) != 0; }

using MaskKey = std::array<uint8_t, 4>;

struct FrameHeader {
   uint64_t length = 0;
   MaskKey mask    = {};
   Opcode opcode   = Opcode::CONTINUATION;
   bool fin        = false;
   bool masked     = false;
};

//********************************************************************************************************************
// Encoding

// Returns the size of the header for a frame with the given payload length.

[[nodiscard]] constexpr size_t frame_header_size(uint64_t PayloadLength, bool Masked) noexcept
{
   size_t size = 2;
   if (PayloadLength > 65535) size += 8;
   else if (PayloadLength > 125) size += 2;
   if (Masked) size += 4;
   return size;
}

// Writes a frame header using the minimal length encoding.  Mask is optional and must be supplied by clients only.
// Returns the number of bytes written, or zero if Output is too small or the header would be invalid (a control frame
// without FIN or with a payload over 125 bytes, or a length with the top bit set).

size_t encode_frame_header(std::span<uint8_t> Output, bool Fin, Opcode Op, uint64_t PayloadLength,
   const MaskKey *Mask = nullptr) noexcept;

// Writes a complete frame: the header followed by Payload, masked if Mask is supplied.  Returns the number of bytes
// written, or zero under the same conditions as encode_frame_header().

size_t encode_frame(std::span<uint8_t> Output, bool Fin, Opcode Op, std::span<const uint8_t> Payload,
   const MaskKey *Mask = nullptr) noexcept;

// XORs Data with Mask in place.  Offset is the position of Data[0] within the frame's payload, allowing a payload to
// be masked or unmasked in several pieces.

void apply_mask(std::span<uint8_t> Data, const MaskKey &Mask, uint64_t Offset = 0) noexcept;

//********************************************************************************************************************
// Close frame payloads (RFC 6455 sections 5.5.1 and 7.4)

// True if Code may appear in a Close frame: 1000 to 1003, 1007 to 1011 or 3000 to 4999.

[[nodiscard]] constexpr bool valid_close_code(int Code) noexcept
{
   return ((Code >= 1000) and (Code <= 1003)) or ((Code >= 1007) and (Code <= 1011)) or
      ((Code >= 3000) and (Code <= 4999));
}

// Writes a Close payload of a 2-byte code and an optional UTF-8 reason.  Returns the number of bytes written, or zero
// if Code is invalid, Reason exceeds 123 bytes or is not valid UTF-8, or Output is too small.

size_t encode_close_payload(std::span<uint8_t> Output, int Code, std::string_view Reason) noexcept;

// Parses a received Close payload.  An empty payload yields CLOSE_NO_STATUS.  Returns zero on success, otherwise the
// close code that the receiver must fail the connection with: CLOSE_PROTOCOL_ERROR for a 1-byte payload or an invalid
// code, CLOSE_INVALID_PAYLOAD for a reason that is not valid UTF-8.

int parse_close_payload(std::span<const uint8_t> Payload, int &Code, std::string_view &Reason) noexcept;

//********************************************************************************************************************
// Frame decoder.  Each frame produces one HEADER event followed by one or more PAYLOAD events, the last of which has
// frame_end set.  A zero-length frame produces a single empty PAYLOAD event.  Frame sequencing (continuations and
// fragmented messages) and the size limits are enforced here; UTF-8 and close payload checks are left to
// MessageReader.

enum class DecodeStatus : uint8_t { NEED_MORE, HEADER, PAYLOAD, ERROR };

struct DecodeEvent {
   FrameHeader header;           // HEADER and PAYLOAD: the current frame's header
   std::span<uint8_t> payload;   // PAYLOAD: unmasked slice of the caller's input
   std::string_view reason;      // ERROR: static description of the failure
   int close_code = 0;           // ERROR: the code to close the connection with
   DecodeStatus status = DecodeStatus::NEED_MORE;
   bool frame_end = false;       // PAYLOAD: true if this slice completes the frame
};

class FrameDecoder {
public:
   // A limit of zero means no limit.  MaxMessageSize applies to the sum of a fragmented message's frames.

   FrameDecoder(Role LocalRole, uint64_t MaxFrameSize = 0, uint64_t MaxMessageSize = 0) noexcept
      : role(LocalRole), max_frame(MaxFrameSize), max_message(MaxMessageSize) { }

   // Decodes from Input and returns the number of bytes consumed.  Call repeatedly, advancing Input by the consumed
   // count, until the event status is NEED_MORE or ERROR.  Payload bytes are unmasked in place within Input.  After an
   // error every call returns zero with the same ERROR event.

   size_t decode(std::span<uint8_t> Input, DecodeEvent &Event) noexcept;

   [[nodiscard]] bool in_message() const noexcept { return message_open; }
   [[nodiscard]] bool has_failed() const noexcept { return failed; }

   void set_max_frame_size(uint64_t Size) noexcept { max_frame = Size; }
   void set_max_message_size(uint64_t Size) noexcept { max_message = Size; }

private:
   size_t fail(DecodeEvent &Event, int Code, std::string_view Reason, size_t Consumed) noexcept;
   bool check_first_bytes(DecodeEvent &Event) noexcept;
   bool check_length(DecodeEvent &Event) noexcept;

   std::array<uint8_t, MAX_FRAME_HEADER> header_buffer = {};
   FrameHeader current;
   std::string_view fail_reason;
   uint64_t remaining = 0;      // Payload bytes not yet returned for the current frame
   uint64_t mask_offset = 0;    // Payload bytes already unmasked for the current frame
   uint64_t message_size = 0;   // Total payload size of the data message in progress
   Role role;
   uint64_t max_frame;
   uint64_t max_message;
   int fail_code = 0;
   uint8_t header_size = 0;     // Bytes buffered in header_buffer
   bool in_payload = false;
   bool message_open = false;   // A fragmented data message awaits its final frame
   bool failed = false;
};

//********************************************************************************************************************
// Message reader.  Layers message semantics over FrameDecoder: data frames are reported as slices of their message,
// text is validated incrementally across fragments, control payloads are collected into an internal 125-byte buffer
// and reported whole, and Close payloads are parsed and validated.

enum class ReadStatus : uint8_t { NEED_MORE, DATA, CONTROL, ERROR };

struct ReadEvent {
   std::span<const uint8_t> data; // DATA: message slice, possibly empty.  CONTROL: the complete control payload.
   std::string_view reason;       // CONTROL (CLOSE): the close reason.  ERROR: static description of the failure.
   int close_code = 0;            // CONTROL (CLOSE): the received code.  ERROR: the code to close with.
   ReadStatus status = ReadStatus::NEED_MORE;
   Opcode opcode = Opcode::CONTINUATION; // DATA: TEXT or BINARY for the whole message.  CONTROL: CLOSE, PING or PONG.
   bool message_end = false;      // DATA: true if this slice completes the message
};

class MessageReader {
public:
   MessageReader(Role LocalRole, uint64_t MaxFrameSize = 0, uint64_t MaxMessageSize = 0,
      bool ValidateUTF8 = true) noexcept
      : decoder(LocalRole, MaxFrameSize, MaxMessageSize), validate_utf8(ValidateUTF8) { }

   // Reads from Input and returns the number of bytes consumed.  Call repeatedly, advancing Input by the consumed
   // count, until the event status is NEED_MORE or ERROR.  DATA slices refer to Input and are only valid until Input
   // is modified.  Empty non-final data slices are not reported.

   size_t read(std::span<uint8_t> Input, ReadEvent &Event) noexcept;

   [[nodiscard]] bool has_failed() const noexcept { return failed; }

   void set_max_frame_size(uint64_t Size) noexcept { decoder.set_max_frame_size(Size); }
   void set_max_message_size(uint64_t Size) noexcept { decoder.set_max_message_size(Size); }

private:
   size_t fail(ReadEvent &Event, int Code, std::string_view Reason, size_t Consumed) noexcept;

   FrameDecoder decoder;
   Utf8Validator utf8;
   std::array<uint8_t, MAX_CONTROL_PAYLOAD> control = {};
   std::string_view fail_reason;
   size_t control_size = 0;
   int fail_code = 0;
   Opcode message_type = Opcode::CONTINUATION;
   bool validate_utf8;
   bool failed = false;
};

} // namespace ws
