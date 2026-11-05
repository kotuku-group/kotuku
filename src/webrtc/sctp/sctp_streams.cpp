// SCTP partial reliability (RFC 3758), stream reconfiguration (RFC 6525) and the DCEP channel layer that is built
// over them.

#include "sctp_streams.h"
#include "sctp_association.h"
#include "dcep.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace rtc {
using namespace sctp;

namespace {

// Serial number comparisons for TSNs and SSNs (RFC 1982).

bool after(uint32_t Left, uint32_t Right) { return int32_t(Left - Right) > 0; }
bool sequence_after(uint16_t Left, uint16_t Right) { return int16_t(Left - Right) > 0; }

bool due(const std::optional<uint64_t> &Timer, uint64_t NowUs) { return Timer and NowUs >= *Timer; }

// RE-CONFIG parameter types

constexpr uint16_t RESET_OUTGOING = 13;
constexpr uint16_t RESET_INCOMING = 14;
constexpr uint16_t RESET_RESPONSE = 16;

} // namespace

//********************************************************************************************************************
// Discards all stream reset state.

void SctpAssociation::clear_streams()
{
   resetting.clear();
   outgoing_reset.reset();
   incoming_reset.reset();
   reset_events.clear();
   reset_wire.clear();
   forward_due.reset();
   reset_due.reset();
   reset_retries = forward_retries = 0;
}

//********************************************************************************************************************
// Abandons every chunk of the message that is governed by Policy, whether sent, queued or waiting in the output
// buffer.  The message's buffer accounting is released once.

bool SctpAssociation::abandon_message(const std::shared_ptr<PolicyState> &Policy)
{
   std::shared_ptr<std::vector<uint8_t>> payload;
   uint16_t stream = 0;
   uint32_t ppid = 0;
   bool found = false;

   for (auto &chunk : sent) {
      if (chunk.policy != Policy or chunk.abandoned) continue;
      payload = chunk.payload;
      stream = chunk.stream;
      ppid = chunk.ppid;
      found = true;
      if (chunk.in_flight) flight_size -= chunk_bytes(chunk);
      if (not chunk.adapter_accepted) release_buffered(chunk.stream, chunk.ppid, chunk.length);
      chunk.in_flight = chunk.retry = false;
      chunk.abandoned = true;
      chunk.payload.reset();
   }

   for (auto it = queued.begin(); it != queued.end();) {
      if (it->policy != Policy) {
         ++it;
         continue;
      }

      payload = it->payload;
      stream = it->stream;
      ppid = it->ppid;
      unsent_bytes -= payload->size() - it->offset;
      release_buffered(stream, ppid, payload->size() - it->offset);

      if (found) {
         it = queued.erase(it);
      }
      else {
         // Preserve contiguous TSNs for any earlier active fragmented message.  The scheduler materialises
         // this SSN skip at the next complete-message boundary, without retaining its payload.
         it->payload.reset();
         ++it;
      }
   }

   Policy->abandoned = true;

   // An owner may not yet have drained output.  Remove abandoned DATA from those records too.  A packet is only
   // re-encoded if it loses a chunk.  The sent list is ordered by TSN.

   auto abandoned_tsn = [this](uint32_t TSN) {
      auto descriptor = std::lower_bound(sent.begin(), sent.end(), TSN,
         [](const SentChunk &Chunk, uint32_t Value) { return int32_t(Chunk.tsn - Value) < 0; });
      return descriptor != sent.end() and descriptor->tsn IS TSN and descriptor->abandoned;
   };

   for (auto it = output.begin(); it != output.end();) {
      Packet parsed;
      if (not parse_local(*it, parsed)) return fail(ERR::InvalidData);

      bool drop = std::any_of(parsed.chunks.begin(), parsed.chunks.end(), [&abandoned_tsn](const Item &Chunk) {
         return Chunk.type IS DATA and abandoned_tsn(read32(Chunk.value, 0));
      });

      if (not drop) {
         ++it;
         continue;
      }

      Encoder encoder(config.local_port, config.remote_port, remote.tag, config.packet_capacity);
      bool any = false;
      for (const auto &chunk : parsed.chunks) {
         if (chunk.type IS DATA and abandoned_tsn(read32(chunk.value, 0))) continue;
         if (not encoder.chunk(uint8_t(chunk.type), chunk.flags, chunk.value)) return fail(ERR::BufferOverflow);
         any = true;
      }

      if (any) {
         *it = encoder.finish();
         ++it;
      }
      else it = output.erase(it);
   }

   if (payload) {
      retained_bytes -= payload->size();
      if (ppid IS 50) control_send_bytes -= payload->size();
      channel_send_bytes[stream] -= payload->size();
   }

   return true;
}

//********************************************************************************************************************
// Abandons messages that have exceeded their lifetime or retransmission limit, then advances the peer's cumulative
// TSN with FORWARD-TSN if possible.

bool SctpAssociation::expire_messages()
{
   // Reliable messages never expire, so a queue without partially reliable messages needs no scan.

   if (not partial_message_count) return forward_progress();

   std::vector<std::shared_ptr<PolicyState>> abandoned;
   std::set<const PolicyState *> seen;

   for (const auto &message : queued) {
      if (not message.policy->abandoned and due(message.policy->expires, now_us) and
          seen.insert(message.policy.get()).second) {
         abandoned.push_back(message.policy);
      }
   }

   for (const auto &chunk : sent) {
      if (chunk.abandoned) continue;
      auto &policy = chunk.policy;
      if (due(policy->expires, now_us) or (chunk.retry and policy->policy.max_retransmits and
          chunk.sends > *policy->policy.max_retransmits)) {
         if (seen.insert(policy.get()).second) abandoned.push_back(policy);
      }
   }

   for (const auto &policy : abandoned) if (not abandon_message(policy)) return false;
   return forward_progress();
}

//********************************************************************************************************************
// Sends FORWARD-TSN to skip the run of abandoned chunks that follows the cumulative acknowledgement point.  Each
// ordered stream that is affected reports the last SSN skipped, and the stream list is limited to what fits in one
// packet.  If Force is false, the message is only sent when the retransmission timer has expired.

bool SctpAssociation::forward_progress(bool Force)
{
   if (not live_transfer() or not remote.forward_tsn) return true;

   uint32_t point = last_ack;
   std::map<uint16_t, uint16_t> streams;
   bool abandoned = false;

   for (const auto &chunk : sent) {
      if (not chunk.abandoned) break;
      if (chunk.abandoned and not (chunk.flags & 4)) {
         if (not streams.contains(chunk.stream) and 20 + 4 * (streams.size() + 1) > config.packet_capacity) break;
         auto it = streams.find(chunk.stream);
         if (it IS streams.end() or sequence_after(chunk.sequence, it->second)) streams[chunk.stream] = chunk.sequence;
      }
      point = chunk.tsn;
      abandoned |= chunk.abandoned;
   }

   if (not abandoned or not after(point, last_ack)) {
      forward_due.reset();
      forward_retries = 0;
      return true;
   }

   if (not Force and forward_due and now_us < *forward_due) return true;
   if (forward_due and ++forward_retries > config.max_retransmits) return fail(ERR::Timeout);

   std::vector<uint8_t> value;
   put32(value, point);
   for (auto [stream, sequence] : streams) {
      put16(value, stream);
      put16(value, sequence);
   }

   std::erase_if(output, [](const std::vector<uint8_t> &Packet) { return Packet[12] IS FORWARD_TSN; });
   if (not emit(FORWARD_TSN, 0, value, remote.tag)) return false;

   // Use T3's backoff interval.  A lost FORWARD-TSN never strands a later ordered message.

   return schedule(forward_due, rto_us);
}

//********************************************************************************************************************
// Processes a FORWARD-TSN chunk.  Partially reassembled messages up to the new cumulative TSN are discarded, and
// complete ordered messages that precede each stream's skipped SSN are delivered first.

bool SctpAssociation::receive_forward(std::span<const uint8_t> Value)
{
   auto point = read32(Value, 0);
   if (not after(point, cumulative_tsn)) return send_sack();

   // Refuse a jump outside the supported bounded serial-number interval.

   if (point - cumulative_tsn > MAX_CHUNKS) return true;

   for (size_t i = 4; i < Value.size(); i += 4) {
      if (read16(Value, i) >= remote.inbound_streams) return true;
   }

   for (auto it = assemblies.begin(); it != assemblies.end();) {
      if (after(it->first, point)) {
         ++it;
         continue;
      }

      auto &message = it->second.message;
      receive_bytes -= message.payload.size();
      channel_receive_bytes[message.stream] -= message.payload.size();
      --receive_records;
      it = assemblies.erase(it);
   }

   for (auto it = fragments.begin(); it != fragments.end();) {
      if (after(it->first, point)) {
         ++it;
         continue;
      }

      receive_bytes -= it->second.payload.size();
      channel_receive_bytes[it->second.stream] -= it->second.payload.size();
      it = fragments.erase(it);
   }

   cumulative_tsn = point;
   std::erase_if(received, [point](uint32_t TSN) { return not after(TSN, point); });
   while (received.erase(cumulative_tsn + 1)) ++cumulative_tsn;

   for (size_t i = 4; i < Value.size(); i += 4) {
      auto stream = read16(Value, i);
      auto skip = read16(Value, i + 2);
      auto &expected = receive_sequences[stream];
      if (sequence_after(expected, skip)) continue;

      // Preserve complete messages that arrived before the skip, in stream order.

      auto &pending = ordered_messages[stream];
      for (unsigned count = 0; count < 1024 and not sequence_after(expected, skip); ++count, ++expected) {
         auto it = pending.find(expected);
         if (it != pending.end()) {
            if (messages.size() >= MAX_PENDING_MESSAGES) return fail(ERR::BufferOverflow);
            messages.push_back(std::move(it->second));
            pending.erase(it);
         }
      }

      if (not sequence_after(expected, skip)) return fail(ERR::InvalidData);
   }

   if (not collect_messages() or not complete_incoming_reset()) return false;
   return send_sack();
}

//********************************************************************************************************************
// Requests a reset of the outgoing direction of Stream.  The request is deferred until the stream has no queued or
// unacknowledged data, and the stream remains reserved until both directions have been reset.

ERR SctpAssociation::reset_stream(uint16_t Stream, uint64_t NowUs)
{
   if (not advance(NowUs)) return ERR::InvalidValue;

   auto error = require_extensions(false, true);
   if (error != ERR::Okay) return error;

   if (Stream >= std::min(remote.inbound_streams, remote.outbound_streams)) return ERR::OutOfRange;
   if (resetting.contains(Stream)) return ERR::Okay;

   if (resetting.size() >= 512 or now_us > std::numeric_limits<uint64_t>::max() - config.shutdown_timeout_us) {
      return ERR::BufferOverflow;
   }

   resetting.emplace(Stream, ResetState{false, false, now_us + config.shutdown_timeout_us});
   return reset_progress() ? ERR::Okay : last_error;
}

//********************************************************************************************************************
// Returns true once both directions of Stream have been reset.

bool SctpAssociation::stream_reset_complete(uint16_t Stream) const
{
   auto it = resetting.find(Stream);
   return it != resetting.end() and it->second.incoming and it->second.outgoing;
}

//********************************************************************************************************************
// Makes a fully reset stream available for reuse.

ERR SctpAssociation::release_stream(uint16_t Stream)
{
   if (not stream_reset_complete(Stream)) return ERR::InvalidState;
   resetting.erase(Stream);
   return ERR::Okay;
}

//********************************************************************************************************************

std::vector<SctpResetEvent> SctpAssociation::take_resets()
{
   return std::exchange(reset_events, {});
}

//********************************************************************************************************************
// Sends a Re-configuration Response Parameter with the given result code.

bool SctpAssociation::reset_response(uint32_t Sequence, uint32_t Result)
{
   std::vector<uint8_t> value, wire;
   put32(value, Sequence);
   put32(value, Result);
   if (not append_parameter(wire, RESET_RESPONSE, value)) return fail(ERR::BufferOverflow);
   return emit(RE_CONFIG, 0, wire, remote.tag);
}

//********************************************************************************************************************
// Completes our outstanding outgoing reset request after the peer has accepted it.  Outgoing SSNs restart from zero.

bool SctpAssociation::acknowledge_reset()
{
   if (not outgoing_reset) return true;

   for (auto stream : outgoing_reset->streams) {
      send_sequences[stream] = 0;
      resetting[stream].outgoing = true;
      if (reset_events.size() >= 1024) return fail(ERR::BufferOverflow);
      reset_events.push_back({stream, false});
   }

   outgoing_reset.reset();
   reset_due.reset();
   reset_wire.clear();
   reset_retries = 0;
   return true;
}

//********************************************************************************************************************
// Performs the peer's outgoing reset request once every TSN up to its barrier has been received and the old ordered
// messages have been delivered.  DATA for the reset streams that arrived after the barrier was deferred, and is now
// replayed under the new SSN numbering.

bool SctpAssociation::complete_incoming_reset()
{
   if (not incoming_reset or after(incoming_reset->barrier, cumulative_tsn)) return true;
   if (not collect_messages()) return false;

   for (auto stream : incoming_reset->streams) if (not ordered_messages[stream].empty()) return true;

   for (auto stream : incoming_reset->streams) {
      // Drain old ordered messages before restarting SSN numbering.  The barrier covers every old fragment.
      receive_sequences[stream] = 0;

      auto it = resetting.find(stream);
      if (it IS resetting.end()) {
         if (now_us > std::numeric_limits<uint64_t>::max() - config.shutdown_timeout_us) return fail(ERR::Timeout);
         it = resetting.emplace(stream, ResetState{false, false, now_us + config.shutdown_timeout_us}).first;
      }

      it->second.incoming = true;
      if (reset_events.size() >= 1024) return fail(ERR::BufferOverflow);
      reset_events.push_back({stream, true});
   }

   auto sequence = incoming_reset->sequence;
   incoming_reset.reset();
   peer_reset_result = 1;

   auto deferred = std::exchange(reset_deferred, {});
   for (const auto &[tsn, part] : deferred) {
      receive_bytes -= part.payload.size();
      channel_receive_bytes[part.stream] -= part.payload.size();

      std::vector<uint8_t> value;
      put32(value, tsn);
      put16(value, part.stream);
      put16(value, part.sequence);
      put32(value, part.ppid);
      value.insert(value.end(), part.payload.begin(), part.payload.end());
      if (not receive_data(Item{DATA, part.flags, value, {}})) return false;
   }

   return reset_response(sequence, 1);
}

//********************************************************************************************************************
// Drives pending stream resets: enforces reset deadlines, retransmits an unanswered request, or sends a new outgoing
// request for the first idle stream that is waiting.  Only one outgoing request is outstanding at a time.

bool SctpAssociation::reset_progress()
{
   if (not live_transfer()) return true;
   if (not complete_incoming_reset()) return false;

   for (const auto &[stream, reset] : resetting) {
      if (not (reset.incoming and reset.outgoing) and now_us >= reset.deadline) return fail(ERR::Timeout);
   }

   if (outgoing_reset) {
      if (due(reset_due, now_us)) {
         if (++reset_retries > config.max_retransmits) return fail(ERR::Timeout);
         if (not emit(RE_CONFIG, 0, reset_wire, remote.tag)) return false;
         reset_rto = std::min(config.max_rto_us, reset_rto * 2);
         return schedule(reset_due, reset_rto);
      }
      return true;
   }

   for (const auto &[stream, reset] : resetting) {
      if (reset.outgoing) continue;

      bool busy = std::any_of(queued.begin(), queued.end(), [stream](const SendMessage &Message) {
         return Message.stream IS stream;
      }) or std::any_of(sent.begin(), sent.end(), [stream](const SentChunk &Chunk) {
         return Chunk.stream IS stream;
      });
      if (busy) continue;

      outgoing_reset = ResetRequest{reset_sequence++, next_tsn - 1, {stream}};

      std::vector<uint8_t> value;
      put32(value, outgoing_reset->sequence);
      put32(value, peer_reset_sequence);
      put32(value, outgoing_reset->barrier);
      put16(value, stream);

      reset_wire.clear();
      if (not append_parameter(reset_wire, RESET_OUTGOING, value) or
          not emit(RE_CONFIG, 0, reset_wire, remote.tag)) return false;

      reset_rto = rto_us;
      reset_retries = 0;
      return schedule(reset_due, reset_rto);
   }

   return true;
}

//********************************************************************************************************************
// Processes a RE-CONFIG chunk.  Responses to our own request complete the outgoing reset, while peer requests are
// checked against the expected request sequence number.  Response result codes are 0 (nothing to do), 1 (performed),
// 2 (denied), 4 (request already in progress), 5 (bad sequence number) and 6 (in progress).

bool SctpAssociation::receive_reconfig(std::span<const uint8_t> Value)
{
   std::vector<Item> params;
   if (not parameters(Value, params)) return true;

   for (const auto &param : params) {
      if (param.type < RESET_OUTGOING or param.type > 18) {
         auto action = unknown_parameter(param.type);
         if (action.report and not error_cause(8, param.wire, remote.tag)) return false;
         if (action.stop) break;
         continue;
      }

      if (param.type IS RESET_RESPONSE) {
         auto sequence = read32(param.value, 0);
         auto result = read32(param.value, 4);
         if (not outgoing_reset or sequence != outgoing_reset->sequence) continue;
         if (result IS 0 or result IS 1) {
            if (not acknowledge_reset()) return false;
         }
         else if (result != 6) return fail(ERR::NoSupport);
         continue;
      }

      if (param.value.size() < 4) continue;

      // A repeated request is answered with the original result.

      auto sequence = read32(param.value, 0);
      if (sequence IS peer_reset_sequence) {
         if (not reset_response(sequence, peer_reset_result)) return false;
         continue;
      }

      if (sequence != peer_reset_sequence + 1) {
         if (not reset_response(sequence, 5)) return false;
         continue;
      }

      if (incoming_reset) {
         if (not reset_response(sequence, 4)) return false;
         continue;
      }

      std::vector<uint16_t> streams;
      size_t start = param.type IS RESET_OUTGOING ? 12 : 4;

      if (param.type != RESET_OUTGOING and param.type != RESET_INCOMING) { // SSN/TSN and add-stream requests
         peer_reset_sequence = sequence;
         peer_reset_result = 2;
         if (not reset_response(sequence, 2)) return false;
         continue;
      }

      bool valid = true;
      for (size_t i = start; i < param.value.size(); i += 2) {
         auto stream = read16(param.value, i);
         if (stream >= std::min(remote.inbound_streams, remote.outbound_streams)) valid = false;
         streams.push_back(stream);
      }

      if (streams.empty()) { // An empty list applies to every stream
         for (uint16_t stream = 0; stream < std::min(remote.inbound_streams, remote.outbound_streams); ++stream) {
            streams.push_back(stream);
         }
      }

      std::sort(streams.begin(), streams.end());
      streams.erase(std::unique(streams.begin(), streams.end()), streams.end());
      peer_reset_sequence = sequence;

      if (not valid) {
         peer_reset_result = 2;
         if (not reset_response(sequence, 2)) return false;
         continue;
      }

      if (param.type IS RESET_INCOMING) { // The peer wants us to reset our outgoing streams
         peer_reset_result = 0;
         for (auto stream : streams) {
            auto error = reset_stream(stream, now_us);
            if (error != ERR::Okay) return fail(error);
         }
         if (not reset_response(sequence, 0)) return false;
      }
      else { // The peer is resetting its outgoing streams, which are our incoming streams
         if (outgoing_reset and read32(param.value, 4) IS outgoing_reset->sequence) {
            if (not acknowledge_reset()) return false;
         }

         for (auto stream : streams) {
            if (not resetting.contains(stream)) {
               if (now_us > UINT64_MAX - config.shutdown_timeout_us) return fail(ERR::Timeout);
               resetting.emplace(stream, ResetState{false, false, now_us + config.shutdown_timeout_us});
            }
         }

         incoming_reset = ResetRequest{sequence, read32(param.value, 8), std::move(streams)};
         peer_reset_result = 6;

         if (after(incoming_reset->barrier, cumulative_tsn)) {
            if (not reset_response(sequence, 6)) return false;
         }
         else if (not complete_incoming_reset()) return false;
      }
   }

   if (not send_sack()) return false;
   return reset_progress();
}

//********************************************************************************************************************
// SctpChannels maps DCEP data channels onto the streams of an association.  The DTLS client allocates even stream
// identifiers and the server allocates odd identifiers (RFC 8832).

SctpChannels::SctpChannels(SctpAssociation &Association, bool DtlsClient)
   : association(Association), client(DtlsClient) { }

//********************************************************************************************************************
// Returns the receive credit of every message that is still held by the channel layer.

SctpChannels::~SctpChannels()
{
   for (const auto &event : events) {
      if (event.type IS SctpChannelEventType::MESSAGE) {
         (void)association.release_message(event.stream, event.receive_credit);
      }
   }

   for (const auto &[id, channel] : channels) {
      for (const auto &event : channel.pending) (void)association.release_message(id, event.receive_credit);
   }
}

//********************************************************************************************************************

const SctpChannel * SctpChannels::channel(uint16_t ID) const
{
   auto it = channels.find(ID);
   return it IS channels.end() ? nullptr : &it->second;
}

//********************************************************************************************************************
// Queues an event for the owner, subject to bounds on the event count and total payload size.

bool SctpChannels::event(SctpChannelEvent Event)
{
   if (events.size() >= MAX_EVENTS or Event.payload.size() > MAX_EVENT_BYTES - event_bytes) return false;
   event_bytes += Event.payload.size();
   events.push_back(std::move(Event));
   return true;
}

//********************************************************************************************************************
// Returns the queued events.  Unless RetainCredit is set, the receive credit of each message is returned to the
// association immediately.  Channels that have closed and completed their stream reset are released here.

std::vector<SctpChannelEvent> SctpChannels::take_events(bool RetainCredit)
{
   for (const auto &event : events) {
      if (not RetainCredit and event.type IS SctpChannelEventType::MESSAGE) {
         (void)association.release_message(event.stream, event.receive_credit);
      }

      if (event.type IS SctpChannelEventType::CLOSE and association.stream_reset_complete(event.stream)) {
         auto it = channels.find(event.stream);
         if (it != channels.end()) {
            label_bytes -= it->second.options.label.size() + it->second.options.protocol.size();
            channels.erase(it);
         }
         (void)association.release_stream(event.stream);
      }
   }

   event_bytes = 0;
   return std::exchange(events, {});
}

//********************************************************************************************************************
// Opens a data channel.  If no ID is specified, the lowest free stream of the correct parity is used.  Negotiated
// channels open immediately, while others send DATA_CHANNEL_OPEN and remain CONNECTING until the peer's ACK.

ERR SctpChannels::open(const SctpChannelOptions &Options, uint64_t NowUs, uint16_t &ID)
{
   auto error = dcep::validate(Options);
   if (error != ERR::Okay) return error;

   error = association.require_extensions(Options.reliability.partial(), true);
   if (error != ERR::Okay) return error;

   if (channels.size() >= MAX_CHANNELS or
       Options.label.size() + Options.protocol.size() > MAX_LABEL_BYTES - label_bytes) {
      return ERR::BufferOverflow;
   }

   auto count = std::min(association.peer().inbound_streams, association.peer().outbound_streams);
   uint16_t id = Options.id.value_or(client ? 0 : 1);
   if (not Options.id) {
      while (id < count and channels.contains(id)) id += 2;
   }

   if (id >= count) return ERR::OutOfRange;
   if (channels.contains(id) or (not Options.negotiated and bool(id & 1) IS client)) return ERR::InvalidValue;
   if (events.size() >= MAX_EVENTS) return ERR::BufferOverflow;

   if (not Options.negotiated) {
      std::vector<uint8_t> wire;
      error = dcep::encode_open(Options, wire);
      if (error IS ERR::Okay) error = association.enqueue(id, dcep::PPID_CONTROL, wire, false, NowUs);
      if (error != ERR::Okay) return error;
   }

   label_bytes += Options.label.size() + Options.protocol.size();

   auto &channel = channels[id];
   channel.options = Options;
   channel.options.id = id;

   if (Options.negotiated) {
      channel.state = SctpChannelState::OPEN;
      (void)event({id, SctpChannelEventType::OPEN});
   }

   ID = id;
   return ERR::Okay;
}

//********************************************************************************************************************
// Sends a message on an open channel.  An empty message is sent as a single filler byte with an empty-message PPID.

ERR SctpChannels::send(uint16_t ID, std::span<const uint8_t> Payload, bool Text, uint64_t NowUs)
{
   auto it = channels.find(ID);
   if (it IS channels.end() or it->second.state != SctpChannelState::OPEN) return ERR::InvalidState;
   if (Text and not dcep::utf8(Payload)) return ERR::InvalidData;

   uint32_t ppid = Text ? dcep::PPID_TEXT : dcep::PPID_BINARY;
   std::array<uint8_t, 1> filler{0};
   if (Payload.empty()) {
      ppid = Text ? dcep::PPID_EMPTY_TEXT : dcep::PPID_EMPTY_BINARY;
      Payload = filler;
   }

   return association.enqueue(ID, ppid, Payload, not it->second.options.ordered, NowUs, it->second.options.reliability);
}

//********************************************************************************************************************
// Closes a channel by resetting its stream.  The peer has five seconds to complete the reset before the association
// is aborted.

ERR SctpChannels::close(uint16_t ID, uint64_t NowUs)
{
   auto it = channels.find(ID);
   if (it IS channels.end()) return ERR::InvalidValue;
   if (it->second.state IS SctpChannelState::CLOSING or it->second.state IS SctpChannelState::CLOSED) return ERR::Okay;

   auto error = association.reset_stream(ID, NowUs);
   if (error != ERR::Okay) return error;

   it->second.state = SctpChannelState::CLOSING;
   it->second.close_deadline = NowUs > UINT64_MAX - 5000000 ? UINT64_MAX : NowUs + 5000000;
   return ERR::Okay;
}

//********************************************************************************************************************
// Reports a channel error and closes the channel, or resets the stream if it does not belong to a known channel.

ERR SctpChannels::channel_error(uint16_t ID, ERR Error, uint64_t NowUs)
{
   if (not event({ID, SctpChannelEventType::ERROR, Error})) return ERR::BufferOverflow;

   auto it = channels.find(ID);
   if (it IS channels.end()) return association.reset_stream(ID, NowUs);
   return close(ID, NowUs);
}

//********************************************************************************************************************
// Processes one received message.  DCEP control messages open channels, while application messages are passed to the
// owner, or held until the channel opens if the ACK is still outstanding.  Retained is set if the message's receive
// credit is now held by the channel layer.

ERR SctpChannels::process(uint16_t ID, uint32_t PPID, bool Unordered, std::vector<uint8_t> Payload,
   uint64_t NowUs, bool &Retained)
{
   auto it = channels.find(ID);

   if (PPID IS dcep::PPID_CONTROL) {
      if (dcep::ack(Payload)) {
         if (it IS channels.end() or not it->second.local or it->second.options.negotiated) return ERR::Okay;

         // Chromium sends unordered ACKs on unordered channels.  An ACK has no application ordering
         // dependency, so accept this receive-side variation while always transmitting ordered DCEP.

         auto &channel = it->second;
         if (channel.state != SctpChannelState::CONNECTING) return ERR::Okay;

         channel.state = SctpChannelState::OPEN;
         if (not event({ID, SctpChannelEventType::OPEN})) return ERR::BufferOverflow;
         for (auto &pending : channel.pending) {
            if (not event(std::move(pending))) return ERR::BufferOverflow;
         }

         pending_bytes -= channel.pending_bytes;
         channel.pending.clear();
         channel.pending_bytes = 0;
         return ERR::Okay;
      }

      // DATA_CHANNEL_OPEN from the peer, which must be ordered and use a stream of the peer's parity

      SctpChannelOptions options;
      auto error = dcep::decode_open(Payload, options);
      if (error != ERR::Okay or Unordered or bool(ID & 1) != client or it != channels.end()) {
         return channel_error(ID, ERR::InvalidData, NowUs);
      }

      if (channels.size() >= MAX_CHANNELS or
          options.label.size() + options.protocol.size() > MAX_LABEL_BYTES - label_bytes) {
         return channel_error(ID, ERR::BufferOverflow, NowUs);
      }

      error = association.require_extensions(options.reliability.partial(), true);
      if (error != ERR::Okay) return channel_error(ID, error, NowUs);

      std::array<uint8_t, 1> ack{2};
      error = association.enqueue(ID, dcep::PPID_CONTROL, ack, false, NowUs);
      if (error != ERR::Okay) return error;

      label_bytes += options.label.size() + options.protocol.size();

      auto &channel = channels[ID];
      channel.options = std::move(options);
      channel.options.id = ID;
      channel.local = false;
      channel.state = SctpChannelState::OPEN;

      if (not event({ID, SctpChannelEventType::REMOTE_OPEN, ERR::Okay, false, {}, 0, channel.options}) or
          not event({ID, SctpChannelEventType::OPEN})) {
         return ERR::BufferOverflow;
      }
      return ERR::Okay;
   }

   if (it IS channels.end()) {
      return channel_error(ID, ERR::InvalidData, NowUs);
   }

   auto &channel = it->second;
   bool text = false;
   auto credit = Payload.size();
   auto error = dcep::decode_payload(PPID, Payload, text);
   if (error != ERR::Okay) {
      return channel_error(ID, error, NowUs);
   }

   SctpChannelEvent message{ID, SctpChannelEventType::MESSAGE, ERR::Okay, text, std::move(Payload), credit};

   if (channel.state IS SctpChannelState::CONNECTING) {
      if (channel.pending.size() >= 1024 or message.payload.size() > 16 * 1024 * 1024 - channel.pending_bytes or
          message.payload.size() > 32 * 1024 * 1024 - pending_bytes) return ERR::BufferOverflow;

      channel.pending_bytes += message.payload.size();
      pending_bytes += message.payload.size();
      channel.pending.push_back(std::move(message));
      Retained = true;
   }
   else if (channel.state IS SctpChannelState::OPEN or channel.state IS SctpChannelState::CLOSING) {
      if (not event(std::move(message))) return ERR::BufferOverflow;
      Retained = true;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Moves received messages and stream resets from the association into channel events, and completes or times out
// closing channels.  If the association has terminated, every open channel is closed.  Protocol errors abort the
// association, after which the pump is repeated to report the closure.

ERR SctpChannels::pump(uint64_t NowUs)
{
   if (association.state() IS SctpState::FAILED or association.state() IS SctpState::CLOSED) {
      auto error = association.error();
      for (auto &[id, channel] : channels) {
         if (channel.state IS SctpChannelState::CLOSED) continue;
         channel.state = SctpChannelState::CLOSED;
         if (error != ERR::Okay and not event({id, SctpChannelEventType::ERROR, error})) return ERR::BufferOverflow;
         if (not event({id, SctpChannelEventType::CLOSE})) return ERR::BufferOverflow;
      }

      pending_bytes = 0;
      for (auto &[id, channel] : channels) {
         channel.pending.clear();
         channel.pending_bytes = 0;
      }
      return error;
   }

   for (auto &message : association.take_messages(true)) {
      auto credit = message.payload.size();
      bool retained = false;
      auto error = process(message.stream, message.ppid, message.unordered,
         std::move(message.payload), NowUs, retained);
      if (not retained) (void)association.release_message(message.stream, credit);
      if (error != ERR::Okay) {
         association.abort(error);
         return pump(NowUs);
      }
   }

   for (auto reset : association.take_resets()) {
      auto it = channels.find(reset.stream);
      if (it IS channels.end()) {
         if (association.stream_reset_complete(reset.stream)) (void)association.release_stream(reset.stream);
         continue;
      }

      if (reset.incoming and it->second.state != SctpChannelState::CLOSING) {
         it->second.state = SctpChannelState::CLOSING;
         it->second.close_deadline = NowUs > UINT64_MAX - 5000000 ? UINT64_MAX : NowUs + 5000000;
      }
   }

   for (auto it = channels.begin(); it != channels.end();) {
      if (it->second.state != SctpChannelState::CLOSING) {
         ++it;
         continue;
      }

      if (association.stream_reset_complete(it->first)) {
         auto id = it->first;
         if (not event({id, SctpChannelEventType::CLOSE})) return ERR::BufferOverflow;

         pending_bytes -= it->second.pending_bytes;
         for (const auto &pending : it->second.pending) {
            (void)association.release_message(it->first, pending.receive_credit);
         }
         it->second.pending.clear();
         it->second.pending_bytes = 0;
         it->second.state = SctpChannelState::CLOSED;
         ++it;
      }
      else {
         if (NowUs >= it->second.close_deadline) {
            association.abort(ERR::Timeout);
            return pump(NowUs);
         }
         ++it;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the earliest close deadline of any closing channel.

std::optional<uint64_t> SctpChannels::next_deadline_us() const
{
   std::optional<uint64_t> result;
   for (const auto &[id, channel] : channels) {
      if (channel.state IS SctpChannelState::CLOSING and (not result or channel.close_deadline < *result)) {
         result = channel.close_deadline;
      }
   }
   return result;
}

} // namespace rtc
