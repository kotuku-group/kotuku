// WebSocket frame encoder, decoder and message reader.  See ws_frame.h.

#include "ws_frame.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ws {

[[nodiscard]] static constexpr bool valid_opcode(uint8_t Op) noexcept
{
   return (Op <= 0x2) or ((Op >= 0x8) and (Op <= 0xa));
}

//********************************************************************************************************************

void apply_mask(std::span<uint8_t> Data, const MaskKey &Mask, uint64_t Offset) noexcept
{
   std::array<uint8_t, 8> pattern;
   for (size_t k = 0; k < pattern.size(); k++) pattern[k] = Mask[(Offset + k) & 3];

   size_t i = 0;
   if (Data.size() >= 8) {
      uint64_t word;
      std::memcpy(&word, pattern.data(), sizeof(word));
      for (; i + 8 <= Data.size(); i += 8) {
         uint64_t value;
         std::memcpy(&value, Data.data() + i, sizeof(value));
         value ^= word;
         std::memcpy(Data.data() + i, &value, sizeof(value));
      }
   }

   for (; i < Data.size(); i++) Data[i] ^= pattern[i & 3];
}

//********************************************************************************************************************

size_t encode_frame_header(std::span<uint8_t> Output, bool Fin, Opcode Op, uint64_t PayloadLength,
   const MaskKey *Mask) noexcept
{
   if (not valid_opcode(uint8_t(Op))) return 0;
   if (is_control(Op) and ((not Fin) or (PayloadLength > MAX_CONTROL_PAYLOAD))) return 0;
   if (PayloadLength > uint64_t(std::numeric_limits<int64_t>::max())) return 0;

   const size_t size = frame_header_size(PayloadLength, Mask != nullptr);
   if (Output.size() < size) return 0;

   const uint8_t mask_bit = Mask ? 0x80 : 0x00;
   Output[0] = (Fin ? 0x80 : 0x00) | uint8_t(Op);

   size_t i = 2;
   if (PayloadLength <= 125) Output[1] = mask_bit | uint8_t(PayloadLength);
   else if (PayloadLength <= 65535) {
      Output[1] = mask_bit | 126;
      Output[i++] = uint8_t(PayloadLength >> 8);
      Output[i++] = uint8_t(PayloadLength);
   }
   else {
      Output[1] = mask_bit | 127;
      for (int shift = 56; shift >= 0; shift -= 8) Output[i++] = uint8_t(PayloadLength >> shift);
   }

   if (Mask) for (auto byte : *Mask) Output[i++] = byte;
   return size;
}

//********************************************************************************************************************

size_t encode_frame(std::span<uint8_t> Output, bool Fin, Opcode Op, std::span<const uint8_t> Payload,
   const MaskKey *Mask) noexcept
{
   if (Output.size() < frame_header_size(Payload.size(), Mask != nullptr) + Payload.size()) return 0;

   auto header = encode_frame_header(Output, Fin, Op, Payload.size(), Mask);
   if (not header) return 0;

   auto body = Output.subspan(header, Payload.size());
   if (not Payload.empty()) std::memcpy(body.data(), Payload.data(), Payload.size());
   if (Mask) apply_mask(body, *Mask);
   return header + Payload.size();
}

//********************************************************************************************************************

size_t encode_close_payload(std::span<uint8_t> Output, int Code, std::string_view Reason) noexcept
{
   if (not valid_close_code(Code)) return 0;
   if (Reason.size() > MAX_CLOSE_REASON) return 0;
   if (not valid_utf8(Reason)) return 0;
   if (Output.size() < 2 + Reason.size()) return 0;

   Output[0] = uint8_t(Code >> 8);
   Output[1] = uint8_t(Code);
   if (not Reason.empty()) std::memcpy(Output.data() + 2, Reason.data(), Reason.size());
   return 2 + Reason.size();
}

//********************************************************************************************************************

int parse_close_payload(std::span<const uint8_t> Payload, int &Code, std::string_view &Reason) noexcept
{
   Reason = {};
   if (Payload.empty()) {
      Code = CLOSE_NO_STATUS;
      return 0;
   }

   Code = 0;
   if ((Payload.size() IS 1) or (Payload.size() > MAX_CONTROL_PAYLOAD)) return CLOSE_PROTOCOL_ERROR;

   const int code = (int(Payload[0]) << 8) | int(Payload[1]);
   if (not valid_close_code(code)) return CLOSE_PROTOCOL_ERROR;

   std::string_view reason((const char *)Payload.data() + 2, Payload.size() - 2);
   if (not valid_utf8(reason)) return CLOSE_INVALID_PAYLOAD;

   Code = code;
   Reason = reason;
   return 0;
}

//********************************************************************************************************************
// FrameDecoder

size_t FrameDecoder::fail(DecodeEvent &Event, int Code, std::string_view Reason, size_t Consumed) noexcept
{
   failed      = true;
   fail_code   = Code;
   fail_reason = Reason;
   Event = DecodeEvent { };
   Event.status     = DecodeStatus::ERROR;
   Event.close_code = Code;
   Event.reason     = Reason;
   return Consumed;
}

// Checks that can be made once the first two header bytes are known, so that a bad frame is rejected before the
// remainder of its header arrives.

bool FrameDecoder::check_first_bytes(DecodeEvent &Event) noexcept
{
   const uint8_t byte0 = header_buffer[0];
   const uint8_t opcode = byte0 & 0x0f;

   if (header_size IS 1) {
      if (byte0 & 0x70) {
         fail(Event, CLOSE_PROTOCOL_ERROR, "Reserved bits are set without a negotiated extension", 0);
         return false;
      }

      if (not valid_opcode(opcode)) {
         fail(Event, CLOSE_PROTOCOL_ERROR, "Reserved opcode", 0);
         return false;
      }

      const bool fin = (byte0 & 0x80) != 0;
      if (is_control(Opcode(opcode))) {
         if (not fin) {
            fail(Event, CLOSE_PROTOCOL_ERROR, "Fragmented control frame", 0);
            return false;
         }
      }
      else if (Opcode(opcode) IS Opcode::CONTINUATION) {
         if (not message_open) {
            fail(Event, CLOSE_PROTOCOL_ERROR, "Continuation frame without a message in progress", 0);
            return false;
         }
      }
      else if (message_open) {
         fail(Event, CLOSE_PROTOCOL_ERROR, "New data frame while a fragmented message is in progress", 0);
         return false;
      }
      return true;
   }

   const uint8_t byte1 = header_buffer[1];
   const bool masked = (byte1 & 0x80) != 0;
   if ((role IS Role::SERVER) and (not masked)) {
      fail(Event, CLOSE_PROTOCOL_ERROR, "Client frame is not masked", 0);
      return false;
   }
   else if ((role IS Role::CLIENT) and masked) {
      fail(Event, CLOSE_PROTOCOL_ERROR, "Server frame is masked", 0);
      return false;
   }

   if (is_control(Opcode(opcode)) and ((byte1 & 0x7f) > MAX_CONTROL_PAYLOAD)) {
      fail(Event, CLOSE_PROTOCOL_ERROR, "Control frame payload exceeds 125 bytes", 0);
      return false;
   }
   return true;
}

// Validates the decoded payload length against the encoding rules and the configured limits, then updates the
// message tracking state.

bool FrameDecoder::check_length(DecodeEvent &Event) noexcept
{
   const uint8_t length7 = header_buffer[1] & 0x7f;

   if ((length7 IS 126) and (current.length < 126)) {
      fail(Event, CLOSE_PROTOCOL_ERROR, "Payload length does not use the minimal encoding", 0);
      return false;
   }
   else if (length7 IS 127) {
      if (current.length >> 63) {
         fail(Event, CLOSE_PROTOCOL_ERROR, "Payload length has the most significant bit set", 0);
         return false;
      }
      else if (current.length < 65536) {
         fail(Event, CLOSE_PROTOCOL_ERROR, "Payload length does not use the minimal encoding", 0);
         return false;
      }
   }

   if (max_frame and (current.length > max_frame)) {
      fail(Event, CLOSE_MESSAGE_TOO_BIG, "Frame exceeds the maximum frame size", 0);
      return false;
   }

   if (is_control(current.opcode)) return true;

   const uint64_t prior = (current.opcode IS Opcode::CONTINUATION) ? message_size : 0;
   if ((current.length > std::numeric_limits<uint64_t>::max() - prior) or
       (max_message and (prior + current.length > max_message))) {
      fail(Event, CLOSE_MESSAGE_TOO_BIG, "Message exceeds the maximum message size", 0);
      return false;
   }

   message_size = prior + current.length;
   message_open = not current.fin;
   return true;
}

size_t FrameDecoder::decode(std::span<uint8_t> Input, DecodeEvent &Event) noexcept
{
   if (failed) return fail(Event, fail_code, fail_reason, 0);

   Event = DecodeEvent { };

   if (in_payload) {
      Event.header = current;

      if (not remaining) { // Zero-length frame
         in_payload = false;
         Event.status = DecodeStatus::PAYLOAD;
         Event.frame_end = true;
         return 0;
      }

      if (Input.empty()) return 0;

      const size_t length = size_t(std::min<uint64_t>(remaining, Input.size()));
      auto slice = Input.first(length);
      if (current.masked) apply_mask(slice, current.mask, mask_offset);
      mask_offset += length;
      remaining -= length;

      Event.status    = DecodeStatus::PAYLOAD;
      Event.payload   = slice;
      Event.frame_end = not remaining;
      if (not remaining) in_payload = false;
      return length;
   }

   size_t consumed = 0;
   while (consumed < Input.size()) {
      header_buffer[header_size++] = Input[consumed++];

      if (header_size <= 2) {
         if (not check_first_bytes(Event)) return consumed;
         if (header_size < 2) continue;
      }

      const uint8_t length7 = header_buffer[1] & 0x7f;
      const bool masked = (header_buffer[1] & 0x80) != 0;
      const size_t length_bytes = (length7 IS 126) ? 2 : (length7 IS 127) ? 8 : 0;
      const size_t required = 2 + length_bytes + (masked ? 4 : 0);
      if (header_size < required) continue;

      current = FrameHeader { };
      current.fin    = (header_buffer[0] & 0x80) != 0;
      current.opcode = Opcode(header_buffer[0] & 0x0f);
      current.masked = masked;

      if (length_bytes) {
         for (size_t i = 0; i < length_bytes; i++) current.length = (current.length << 8) | header_buffer[2 + i];
      }
      else current.length = length7;

      if (masked) std::memcpy(current.mask.data(), header_buffer.data() + 2 + length_bytes, 4);

      header_size = 0;
      if (not check_length(Event)) return consumed;

      in_payload  = true;
      remaining   = current.length;
      mask_offset = 0;

      Event.status = DecodeStatus::HEADER;
      Event.header = current;
      return consumed;
   }

   return consumed; // Header incomplete; NEED_MORE
}

//********************************************************************************************************************
// MessageReader

size_t MessageReader::fail(ReadEvent &Event, int Code, std::string_view Reason, size_t Consumed) noexcept
{
   failed      = true;
   fail_code   = Code;
   fail_reason = Reason;
   Event = ReadEvent { };
   Event.status     = ReadStatus::ERROR;
   Event.close_code = Code;
   Event.reason     = Reason;
   return Consumed;
}

size_t MessageReader::read(std::span<uint8_t> Input, ReadEvent &Event) noexcept
{
   if (failed) return fail(Event, fail_code, fail_reason, 0);

   Event = ReadEvent { };
   size_t consumed = 0;
   while (true) {
      DecodeEvent frame;
      consumed += decoder.decode(Input.subspan(consumed), frame);

      switch (frame.status) {
         case DecodeStatus::NEED_MORE:
            return consumed;

         case DecodeStatus::ERROR:
            return fail(Event, frame.close_code, frame.reason, consumed);

         case DecodeStatus::HEADER:
            if (is_control(frame.header.opcode)) control_size = 0;
            else if (frame.header.opcode != Opcode::CONTINUATION) {
               message_type = frame.header.opcode;
               utf8.reset();
            }
            break;

         case DecodeStatus::PAYLOAD:
            if (is_control(frame.header.opcode)) {
               // The decoder guarantees that control payloads do not exceed the buffer.
               if (not frame.payload.empty()) {
                  std::memcpy(control.data() + control_size, frame.payload.data(), frame.payload.size());
                  control_size += frame.payload.size();
               }
               if (not frame.frame_end) break;

               Event.status = ReadStatus::CONTROL;
               Event.opcode = frame.header.opcode;
               Event.data   = std::span<const uint8_t>(control.data(), control_size);

               if (frame.header.opcode IS Opcode::CLOSE) {
                  if (auto code = parse_close_payload(Event.data, Event.close_code, Event.reason)) {
                     return fail(Event, code, (code IS CLOSE_INVALID_PAYLOAD) ?
                        "Close reason is not valid UTF-8" : "Invalid close frame payload", consumed);
                  }
               }
               return consumed;
            }
            else {
               const bool message_end = frame.frame_end and frame.header.fin;

               if ((message_type IS Opcode::TEXT) and validate_utf8) {
                  if (not utf8.feed(frame.payload)) {
                     return fail(Event, CLOSE_INVALID_PAYLOAD, "Text message is not valid UTF-8", consumed);
                  }
                  else if (message_end and (not utf8.complete())) {
                     return fail(Event, CLOSE_INVALID_PAYLOAD, "Text message ends inside a UTF-8 sequence",
                        consumed);
                  }
               }

               if (frame.payload.empty() and (not message_end)) break;

               Event.status      = ReadStatus::DATA;
               Event.opcode      = message_type;
               Event.data        = frame.payload;
               Event.message_end = message_end;
               return consumed;
            }
      }
   }
}

} // namespace ws
