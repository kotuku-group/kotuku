// WebSocket send queue.  See ws_send_queue.h.

#include "ws_send_queue.h"

#include <algorithm>
#include <cstring>

namespace ws {

//********************************************************************************************************************

uint64_t SendQueue::frame_size(uint64_t PayloadLength) const noexcept
{
   return frame_header_size(PayloadLength, masked()) + PayloadLength;
}

uint64_t SendQueue::encoded_size(uint64_t PayloadLength) const noexcept
{
   if ((not fragment_size) or (PayloadLength <= fragment_size)) return frame_size(PayloadLength);

   const uint64_t full = PayloadLength / fragment_size;
   const uint64_t remainder = PayloadLength % fragment_size;
   return (full * frame_size(fragment_size)) + (remainder ? frame_size(remainder) : 0);
}

//********************************************************************************************************************

void SendQueue::set_fragment_size(uint64_t Size) noexcept
{
   if (fragment_size IS Size) return;

   fragment_size = Size;
   for (size_t i = 0; i < messages.size(); i++) {
      auto &message = messages[i];
      pending_bytes -= message.encoded;

      const auto remaining = uint64_t(message.payload.size()) - message.framed;
      if ((i IS 0) and (source IS FrameSource::DATA)) message.encoded = remaining ? encoded_size(remaining) : 0;
      else message.encoded = encoded_size(remaining);

      pending_bytes += message.encoded;
   }
}

//********************************************************************************************************************

QueueResult SendQueue::push_message(Opcode Type, std::span<const uint8_t> Payload)
{
   if ((close_state != CloseState::NONE) or data_closed) return QueueResult::CLOSED;
   if ((Type != Opcode::TEXT) and (Type != Opcode::BINARY)) return QueueResult::INVALID;
   if (Payload.size() > uint64_t(INT64_MAX)) return QueueResult::INVALID;

   const auto size = encoded_size(Payload.size());
   if (limit and ((size > limit) or (pending_bytes > limit - size))) return QueueResult::LIMIT;

   auto &message = messages.emplace_back();
   message.payload.assign(Payload.begin(), Payload.end());
   message.encoded = size;
   message.type    = Type;
   pending_bytes += size;
   return QueueResult::OKAY;
}

//********************************************************************************************************************

QueueResult SendQueue::push_control(Opcode Op, std::span<const uint8_t> Payload, bool Exempt)
{
   if (close_state != CloseState::NONE) return QueueResult::CLOSED;
   if ((Op != Opcode::PING) and (Op != Opcode::PONG)) return QueueResult::INVALID;
   if (Payload.size() > MAX_CONTROL_PAYLOAD) return QueueResult::INVALID;

   const auto size = frame_size(Payload.size());

   ControlFrame *target = nullptr;
   if (Op IS Opcode::PONG) {
      for (auto &control : controls) {
         if (control.opcode IS Opcode::PONG) { target = &control; break; }
      }
   }

   if (target) pending_bytes -= frame_size(target->size); // Replace the unsent Pong
   else {
      if ((Op IS Opcode::PING) and (not Exempt) and limit and ((size > limit) or (pending_bytes > limit - size))) {
         return QueueResult::LIMIT;
      }
      target = &controls.emplace_back();
   }

   target->opcode = Op;
   target->size   = uint8_t(Payload.size());
   if (not Payload.empty()) std::memcpy(target->payload.data(), Payload.data(), Payload.size());
   pending_bytes += size;
   return QueueResult::OKAY;
}

//********************************************************************************************************************

QueueResult SendQueue::push_close(std::span<const uint8_t> Payload, bool DiscardData)
{
   if (close_state != CloseState::NONE) return QueueResult::CLOSED;
   if (Payload.size() > MAX_CONTROL_PAYLOAD) return QueueResult::INVALID;

   if (DiscardData) discard_unsent();

   close_frame.opcode = Opcode::CLOSE;
   close_frame.size   = uint8_t(Payload.size());
   if (not Payload.empty()) std::memcpy(close_frame.payload.data(), Payload.data(), Payload.size());
   close_state = CloseState::QUEUED;
   pending_bytes += frame_size(Payload.size());
   return QueueResult::OKAY;
}

//********************************************************************************************************************

void SendQueue::discard_unsent() noexcept
{
   data_closed = true;

   for (auto &control : controls) pending_bytes -= frame_size(control.size);
   controls.clear();

   size_t keep = 0;
   if ((source IS FrameSource::DATA) and (not messages.empty())) {
      // The active message's frame in progress must be completed; its unstarted frames are dropped.
      auto &active = messages.front();
      pending_bytes -= active.encoded;
      active.encoded = 0;
      truncate_message = true;
      keep = 1;
   }

   for (size_t i = keep; i < messages.size(); i++) pending_bytes -= messages[i].encoded;
   messages.erase(messages.begin() + keep, messages.end());
}

//********************************************************************************************************************
// Selects the next frame at a frame boundary: control frames first, then data, then the Close frame.

bool SendQueue::start_frame() noexcept
{
   if (source != FrameSource::NONE) return true;

   const bool have_control = not controls.empty();
   const bool have_data    = not messages.empty();
   const bool have_close   = close_state IS CloseState::QUEUED;
   if ((not have_control) and (not have_data) and (not have_close)) return false;

   if (masked()) {
      if ((not generator) or (not generator(mask))) {
         failed = true;
         return false;
      }
   }

   bool fin = true;
   Opcode opcode;

   if (have_control) {
      active_control = controls.front();
      controls.pop_front();
      opcode       = active_control.opcode;
      payload      = active_control.payload.data();
      payload_size = active_control.size;
      source       = FrameSource::CONTROL;
   }
   else if (have_data) {
      auto &message = messages.front();
      const uint64_t remaining = message.payload.size() - message.framed;
      const uint64_t length = fragment_size ? std::min(fragment_size, remaining) : remaining;

      opcode         = (message.framed IS 0) ? message.type : Opcode::CONTINUATION;
      fin            = (message.framed + length) IS message.payload.size();
      payload        = message.payload.data() + message.framed;
      payload_size   = length;
      message.framed += length;
      message.encoded -= frame_size(length);
      source         = FrameSource::DATA;
   }
   else {
      active_control = close_frame;
      opcode       = Opcode::CLOSE;
      payload      = active_control.payload.data();
      payload_size = active_control.size;
      source       = FrameSource::CLOSE;
      close_state  = CloseState::STARTED;
   }

   header_size  = uint8_t(encode_frame_header(header, fin, opcode, payload_size, masked() ? &mask : nullptr));
   header_sent  = 0;
   payload_sent = 0;
   return true;
}

//********************************************************************************************************************

void SendQueue::finish_frame() noexcept
{
   if (source IS FrameSource::DATA) {
      auto &message = messages.front();
      if (truncate_message or (message.framed IS message.payload.size())) {
         messages.pop_front();
         truncate_message = false;
      }
   }
   else if (source IS FrameSource::CLOSE) close_state = CloseState::SENT;

   source  = FrameSource::NONE;
   payload = nullptr;
}

//********************************************************************************************************************

std::span<const uint8_t> SendQueue::next_chunk(std::span<uint8_t> Scratch) noexcept
{
   last_chunk = 0;
   if (failed or (Scratch.size() < MAX_FRAME_HEADER)) return {};
   if (not start_frame()) return {};

   size_t size = header_size - header_sent;
   std::memcpy(Scratch.data(), header.data() + header_sent, size);

   const auto take = size_t(std::min<uint64_t>(Scratch.size() - size, payload_size - payload_sent));
   if (take) {
      auto body = Scratch.subspan(size, take);
      std::memcpy(body.data(), payload + payload_sent, take);
      if (masked()) apply_mask(body, mask, payload_sent);
      size += take;
   }

   last_chunk = size;
   return std::span<const uint8_t>(Scratch.data(), size);
}

//********************************************************************************************************************

void SendQueue::consume(size_t Count) noexcept
{
   Count = std::min(Count, last_chunk);
   last_chunk = 0;
   if ((not Count) or (source IS FrameSource::NONE)) return;

   pending_bytes -= Count;

   const auto header_part = std::min<size_t>(Count, header_size - header_sent);
   header_sent += uint8_t(header_part);
   payload_sent += Count - header_part;

   if ((header_sent IS header_size) and (payload_sent IS payload_size)) finish_frame();
}

} // namespace ws
