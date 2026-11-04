// Reliable, single-path SCTP DATA and path management (RFC 9260, RFC 8899).

#include "sctp_association.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rtc {
using namespace sctp;

namespace {

bool after(uint32_t Left, uint32_t Right) { return int32_t(Left - Right) > 0; }

bool due(const std::optional<uint64_t> &Timer, uint64_t NowUs) { return Timer and NowUs >= *Timer; }

} // namespace

//********************************************************************************************************************
// Returns true if the association is able to transfer data, including during shutdown.

bool SctpAssociation::live_transfer() const
{
   return transfer_ready and current_state != SctpState::CLOSED and current_state != SctpState::FAILED;
}

//********************************************************************************************************************
// Sets Timer to expire after Delay microseconds.  A zero or overflowing delay fails the association.

bool SctpAssociation::schedule(std::optional<uint64_t> &Timer, uint64_t Delay)
{
   if (not Delay or Delay > std::numeric_limits<uint64_t>::max() - now_us) return fail(ERR::Timeout);
   Timer = now_us + Delay;
   return true;
}

//********************************************************************************************************************
// Discards all data transfer state, including queued messages, timers and buffer accounting.

void SctpAssociation::clear_transfer()
{
   clear_streams();

   queued.clear();
   sent.clear();
   fragments.clear();
   reset_deferred.clear();
   received.clear();
   duplicates.clear();
   assemblies.clear();
   ordered_messages.clear();
   messages.clear();
   send_sequences.clear();
   receive_sequences.clear();
   channel_send_bytes.clear();
   channel_receive_bytes.clear();
   channel_message_counts.clear();

   t3_due.reset();
   sack_due.reset();
   heartbeat_due.reset();
   probe_due.reset();
   close_due.reset();
   shutdown_due.reset();
   send_due.reset();
   heartbeat_value.clear();

   transfer_ready = false;
   retained_bytes = unsent_bytes = receive_bytes = receive_records = flight_size = send_message_count = 0;
   control_send_bytes = control_message_count = 0;
   recovering = sack_needed = congestion_limited = false;
   srtt_us = rttvar_us = 0;
   data_timeouts = fast_count = ack_packets = heartbeat_failures = probe_attempts = 0;
   retransmission_count = 0;
   shutdown_retries = slow_start_updates = congestion_updates = 0;
   last_stream = 65535;
}

//********************************************************************************************************************
// Completes a graceful shutdown and enters the CLOSED state.

void SctpAssociation::finish_transfer()
{
   // Wire shutdown must not erase messages already accepted for delivery to the owner.

   auto pending = std::exchange(messages, {});
   auto ordered = std::exchange(ordered_messages, {});
   auto sequences = receive_sequences;

   clear_transfer();

   receive_sequences = std::move(sequences);
   messages = std::move(pending);
   ordered_messages = std::move(ordered);

   for (const auto &message : messages) {
      receive_bytes += message.payload.size();
      channel_receive_bytes[message.stream] += message.payload.size();
      ++receive_records;
   }

   for (const auto &[stream, items] : ordered_messages) {
      for (const auto &[sequence, message] : items) {
         receive_bytes += message.payload.size();
         channel_receive_bytes[stream] += message.payload.size();
         ++receive_records;
      }
   }

   set_state(SctpState::CLOSED);
}

//********************************************************************************************************************
// Prepares data transfer for a newly established association.  The path MTU starts at the proven floor, and
// heartbeats are used to probe for a larger MTU up to the configured packet capacity (RFC 8899).

void SctpAssociation::initialise_transfer()
{
   clear_transfer();

   transfer_ready = true;
   next_tsn = local_tsn;
   last_ack = local_tsn - 1;
   cumulative_tsn = remote.initial_tsn - 1;
   peer_window = remote.receive_window;
   reset_sequence = local_tsn;
   peer_reset_sequence = remote.initial_tsn - 1;
   path_mtu = DATA_PACKET_FLOOR;
   probe_upper = config.packet_capacity & ~size_t(3);
   cwnd = std::min(4 * path_mtu, std::max(2 * path_mtu, size_t(4380)));
   ssthresh = remote.receive_window;
   partial_bytes = 0;
   rto_us = config.initial_rto_us;

   (void)schedule(heartbeat_due, config.heartbeat_us);
   if (probe_upper > path_mtu) (void)schedule(probe_due, rto_us);
}

//********************************************************************************************************************
// Returns the earliest time at which tick() needs to be called, including message expiry and reset deadlines.

std::optional<uint64_t> SctpAssociation::next_deadline_us() const
{
   auto result = deadline;

   for (auto timer : {t3_due, sack_due, heartbeat_due, probe_due, close_due, shutdown_due,
       send_due, forward_due, reset_due}) {
      if (timer and (not result or *timer < *result)) result = timer;
   }

   for (const auto &message : queued) {
      if (not message.policy->abandoned and message.policy->expires and
          (not result or *message.policy->expires < *result)) {
         result = message.policy->expires;
      }
   }

   for (const auto &chunk : sent) {
      if (not chunk.abandoned and chunk.policy->expires and (not result or *chunk.policy->expires < *result)) {
         result = chunk.policy->expires;
      }
   }

   for (const auto &[stream, reset] : resetting) {
      if (not (reset.incoming and reset.outgoing) and (not result or reset.deadline < *result)) {
         result = reset.deadline;
      }
   }

   return result;
}

//********************************************************************************************************************

SctpDiagnostics SctpAssociation::diagnostics() const
{
   return {flight_size, cwnd, receive_bytes, retained_bytes, unsent_bytes, path_mtu, rto_us,
      retransmission_count, fast_count, slow_start_updates, congestion_updates};
}

//********************************************************************************************************************
// Queues a message for transmission on Stream.  Buffer limits apply per association and per stream, and DCEP control
// messages (PPID 50) have a separate reservation.  Unordered or partially reliable control messages are not permitted.

ERR SctpAssociation::enqueue(uint16_t Stream, uint32_t PPID, std::span<const uint8_t> Payload,
   bool Unordered, uint64_t NowUs, SctpReliability Policy)
{
   if (current_state != SctpState::ESTABLISHED) return ERR::InvalidState;
   if (not advance(NowUs)) return ERR::InvalidValue;

   auto policy_error = Policy.validate();
   if (policy_error != ERR::Okay) return policy_error;

   if (PPID IS 50 and (Unordered or Policy.partial())) return ERR::InvalidValue;
   if (Policy.partial() and not remote.forward_tsn) return ERR::NoSupport;
   if (resetting.contains(Stream)) return ERR::InvalidState;
   if (Stream >= remote.outbound_streams or Payload.empty()) return ERR::InvalidValue;
   if (Payload.size() > MAX_MESSAGE) return ERR::OutOfRange;

   auto bytes = channel_send_bytes.find(Stream);
   auto count = channel_message_counts.find(Stream);
   bool control = PPID IS 50;

   // DCEP has a separate reservation: an application backlog must not reject the opening ACK.

   if (control) {
      if (control_message_count >= MAX_CONTROL_MESSAGES or Payload.size() > MAX_CONTROL_BYTES - control_send_bytes) {
         return ERR::BufferOverflow;
      }
   }
   else if (send_message_count - control_message_count >= MAX_QUEUED_MESSAGES or
       (count != channel_message_counts.end() and count->second >= MAX_STREAM_MESSAGES) or
       retained_bytes - control_send_bytes > MAX_PAYLOAD - Payload.size()) return ERR::BufferOverflow;

   if (bytes != channel_send_bytes.end() and bytes->second > MAX_MESSAGE - Payload.size()) {
      return ERR::BufferOverflow;
   }

   if (Policy.max_lifetime_ms and uint64_t(*Policy.max_lifetime_ms) * 1000 >
       std::numeric_limits<uint64_t>::max() - now_us) return ERR::OutOfRange;

   auto policy = std::make_shared<PolicyState>();
   policy->policy = Policy;
   if (Policy.max_lifetime_ms) policy->expires = now_us + uint64_t(*Policy.max_lifetime_ms) * 1000;

   auto payload = std::make_shared<std::vector<uint8_t>>(Payload.begin(), Payload.end());
   uint16_t sequence = Unordered ? 0 : send_sequences[Stream]++;
   queued.push_back({Stream, sequence, PPID, Unordered, 0, std::move(payload), std::move(policy)});

   retained_bytes += Payload.size();
   unsent_bytes += Payload.size();
   channel_send_bytes[Stream] += Payload.size();
   ++channel_message_counts[Stream];
   ++send_message_count;

   if (control) {
      control_send_bytes += Payload.size();
      ++control_message_count;
   }

   return (expire_messages() and pump_data()) ? ERR::Okay : last_error;
}

//********************************************************************************************************************
// Returns the padded on-wire size of a DATA chunk.

size_t SctpAssociation::chunk_bytes(const SentChunk &Chunk) const
{
   return (16 + Chunk.length + 3) & ~size_t(3);
}

//********************************************************************************************************************
// Builds the value of a DATA chunk from its descriptor.

std::vector<uint8_t> SctpAssociation::data_value(const SentChunk &Chunk) const
{
   std::vector<uint8_t> value;
   put32(value, Chunk.tsn);
   put16(value, Chunk.stream);
   put16(value, Chunk.sequence);
   put32(value, Chunk.ppid);
   value.insert(value.end(), Chunk.payload->begin() + Chunk.offset,
      Chunk.payload->begin() + Chunk.offset + Chunk.length);
   return value;
}

//********************************************************************************************************************
// Packs DATA chunks into packets, subject to the congestion window, the peer's receive window and the path MTU.
// Retransmissions take priority over new data.  A burst is limited to MAX_BURST_CHUNKS, after which the send timer
// resumes transmission so that control output is never starved.

bool SctpAssociation::pump_data()
{
   if (not live_transfer() or current_state IS SctpState::SHUTDOWN_SENT or
       current_state IS SctpState::SHUTDOWN_ACK_SENT) return true;

   if (not expire_messages()) return false;
   send_due.reset();

   // Reserve output slots for SACK, HEARTBEAT and shutdown even under sustained application load.

   size_t payload_flight = 0;
   for (const auto &chunk : sent) if (not chunk.gap_acked and not chunk.abandoned) payload_flight += chunk.length;

   size_t remaining = MAX_BURST_CHUNKS;
   for (size_t packets = 0; packets < MAX_BURST_PACKETS and remaining and output.size() < MAX_OUTPUT - OUTPUT_RESERVE;
        ++packets) {
      Encoder encoder(config.local_port, config.remote_port, remote.tag, path_mtu);
      size_t used = HEADER_SIZE, chunks = 0;
      bool added = false;

      // Retransmissions

      for (auto &chunk : sent) {
         if (not remaining) break;
         if (not chunk.retry or chunk.gap_acked or chunk.abandoned) continue;

         auto size = chunk_bytes(chunk);
         if (used + size > path_mtu or chunks >= MAX_ITEMS) break;
         if (not chunk.in_flight and flight_size + size > cwnd) break;
         if (not encoder.chunk(DATA, chunk.flags, data_value(chunk))) return fail(ERR::BufferOverflow);

         if (not chunk.in_flight) flight_size += size;
         chunk.in_flight = true;
         chunk.retry = false;
         chunk.sent_us = now_us;
         ++chunk.sends;
         if (retransmission_count != UINT64_MAX) ++retransmission_count;
         used += size;
         ++chunks;
         --remaining;
         added = true;
      }

      bool retry_pending = std::any_of(sent.begin(), sent.end(), [](const SentChunk &Chunk) {
         return Chunk.retry and not Chunk.gap_acked and not Chunk.abandoned;
      });

      // New data

      while (remaining and not retry_pending and not queued.empty() and sent.size() < MAX_CHUNKS) {
         // Fragment a message contiguously.  Rotate streams only at complete-message boundaries.

         if (not queued.front().offset and queued.front().stream IS last_stream) {
            auto other = std::find_if(queued.begin(), queued.end(), [this](const SendMessage &Message) {
               return Message.stream != last_stream;
            });
            if (other != queued.end()) std::rotate(queued.begin(), other, other + 1);
         }

         auto &message = queued.front();

         if (not message.payload) { // Abandoned before transmission; consume a TSN as an abandoned marker
            SentChunk marker{next_tsn++, message.stream, message.sequence, message.ppid,
               uint8_t(message.unordered ? 7 : 3), 0, 0, {}, message.policy, now_us};
            marker.in_flight = false;
            marker.abandoned = true;
            sent.push_back(std::move(marker));
            last_stream = message.stream;
            queued.pop_front();
            --remaining;
            if (not forward_progress()) return false;
            continue;
         }

         size_t length = std::min(DATA_PACKET_FLOOR - 28, message.payload->size() - message.offset);
         size_t size = (16 + length + 3) & ~size_t(3);
         if (flight_size + size > cwnd) {
            congestion_limited = true;
            break;
         }

         if (used + size > path_mtu or chunks >= MAX_ITEMS) break;

         // RFC 9260 zero-window probe: exactly one DATA chunk when nothing is outstanding.

         if (peer_window <= payload_flight and not sent.empty()) break;
         if (peer_window > payload_flight) length = std::min(length, size_t(peer_window) - payload_flight);

         size = (16 + length + 3) & ~size_t(3);
         uint8_t flags = (message.unordered ? 4 : 0) | (not message.offset ? 2 : 0) |
            (message.offset + length IS message.payload->size() ? 1 : 0);
         SentChunk chunk{next_tsn++, message.stream, message.sequence, message.ppid, flags,
            message.offset, length, message.payload, message.policy, now_us};
         if (not encoder.chunk(DATA, flags, data_value(chunk))) return fail(ERR::BufferOverflow);

         sent.push_back(std::move(chunk));
         message.offset += length;
         unsent_bytes -= length;
         payload_flight += length;
         flight_size += size;
         used += size;
         ++chunks;
         --remaining;
         added = true;

         if (message.offset IS message.payload->size()) {
            last_stream = message.stream;
            queued.pop_front();
         }

         if (not t3_due and not schedule(t3_due, rto_us)) return false;
         if (not peer_window) break;
      }

      if (not added) break;
      output.push_back(encoder.finish());
   }

   if ((not queued.empty() or not remaining) and (output.size() >= MAX_OUTPUT - OUTPUT_RESERVE or not remaining)) {
      return schedule(send_due, 1000);
   }

   return true;
}

//********************************************************************************************************************
// Sends a SACK that reports the cumulative TSN, gap blocks for out-of-order TSNs, and duplicates.  The block lists are
// truncated to fit within one packet.

bool SctpAssociation::send_sack()
{
   std::vector<uint32_t> offsets;
   for (auto tsn : received) {
      auto offset = tsn - cumulative_tsn;
      if (offset and offset <= 65535) offsets.push_back(offset);
   }
   std::sort(offsets.begin(), offsets.end());

   std::vector<std::pair<uint16_t, uint16_t>> gaps;
   for (auto offset : offsets) {
      if (not gaps.empty() and offset IS uint32_t(gaps.back().second) + 1) gaps.back().second = uint16_t(offset);
      else gaps.emplace_back(uint16_t(offset), uint16_t(offset));
   }

   size_t entries = (config.packet_capacity - 28) / 4;
   if (gaps.size() > entries) gaps.resize(entries);
   size_t dups = std::min(duplicates.size(), entries - gaps.size());

   std::vector<uint8_t> value;
   put32(value, cumulative_tsn);
   put32(value, uint32_t(config.receive_window - receive_bytes));
   put16(value, uint16_t(gaps.size()));
   put16(value, uint16_t(dups));
   for (auto [begin, end] : gaps) {
      put16(value, begin);
      put16(value, end);
   }
   for (size_t i = 0; i < dups; ++i) put32(value, duplicates[i]);

   duplicates.clear();
   sack_needed = false;
   ack_packets = 0;
   sack_due.reset();

   // Only the latest unsent SACK is useful.  Coalescing also preserves control capacity during bursts.

   std::erase_if(output, [](const std::vector<uint8_t> &Packet) { return Packet[12] IS SACK; });
   return emit(SACK, 0, value, remote.tag);
}

//********************************************************************************************************************
// Returns the maximum size of a received message for the given PPID.

size_t SctpAssociation::message_limit(uint32_t PPID) const
{
   // SDP's application message limit does not restrict the two 65535-byte DCEP strings.
   return PPID IS 50 ? std::min(size_t(config.receive_window), std::max(size_t(config.receive_limit), size_t(131082))) :
      config.receive_limit;
}

//********************************************************************************************************************
// Processes a DATA chunk.  Duplicates and chunks that cannot be stored are answered with an immediate SACK, so that
// the peer retransmits them later.  Accepted fragments are stored until collect_messages() can reassemble them.  The
// receive window is managed so that one complete maximum-sized message can always be reassembled.

bool SctpAssociation::receive_data(const Item &Chunk)
{
   auto tsn = read32(Chunk.value, 0);
   if (not after(tsn, cumulative_tsn) or received.contains(tsn)) {
      if (duplicates.size() < 32) duplicates.push_back(tsn);
      sack_needed = true;
      return send_sack();
   }

   if (reset_deferred.contains(tsn)) return true;

   // Gap offsets have 16 bits; bound descriptor storage and the serial-number comparison interval.

   if (tsn - cumulative_tsn > 65535 or fragments.size() + received.size() + reset_deferred.size() >= MAX_CHUNKS) {
      return send_sack();
   }

   auto stream = read16(Chunk.value, 4);
   auto sequence = read16(Chunk.value, 6);
   auto ppid = read32(Chunk.value, 8);
   auto payload = Chunk.value.subspan(12);

   if (stream >= remote.inbound_streams) { // Invalid Stream Identifier
      std::vector<uint8_t> cause;
      put16(cause, stream);
      put16(cause, 0);
      return error_cause(1, cause, remote.tag);
   }

   if (receive_bytes > config.receive_window - std::min(size_t(config.receive_window), payload.size())) {
      return send_sack();
   }

   auto limit = message_limit(ppid);
   if (payload.size() > limit) return fail(ERR::OutOfRange);
   if (channel_receive_bytes[stream] > MAX_MESSAGE - payload.size()) return send_sack();

   // Data that follows the barrier of a pending incoming stream reset belongs to the new stream sequence, and is held
   // until the reset completes.

   if (incoming_reset and after(tsn, incoming_reset->barrier) and
       std::ranges::find(incoming_reset->streams, stream) != incoming_reset->streams.end()) {
      if (reset_deferred.size() >= MAX_DEFERRED_FRAGMENTS) return send_sack();
      reset_deferred.emplace(tsn, Fragment{stream, sequence, ppid, Chunk.flags,
         std::vector<uint8_t>(payload.begin(), payload.end())});
      receive_bytes += payload.size();
      channel_receive_bytes[stream] += payload.size();
      return true;
   }

   // A fragmented message reserves ReceiveLimit until complete.  Unrelated fragments cannot consume it.
   // At least one whole maximum-sized message always fits, preventing rwnd/reassembly deadlock.

   size_t reserved = 0, channel_reserved = 0;
   for (const auto &[begin, assembly] : assemblies) {
      auto remaining = message_limit(assembly.message.ppid) - assembly.message.payload.size();
      reserved += remaining;
      if (assembly.message.stream IS stream) channel_reserved += remaining;
   }

   bool continuation = false;
   for (const auto &[begin, assembly] : assemblies) {
      if (assembly.message.stream IS stream and assembly.sequence IS sequence and
          assembly.message.unordered IS bool(Chunk.flags & 4)) continuation = true;
   }

   if (not continuation and not (Chunk.flags & 3) and reserved + receive_bytes + payload.size() >
       config.receive_window) return send_sack();

   if (not continuation and not (Chunk.flags & 2)) {
      size_t orphan_count = 0;
      for (const auto &[number, part] : fragments) if (part.stream IS stream) ++orphan_count;
      if (assemblies.size() + fragments.size() >= MAX_ASSEMBLIES or orphan_count >= MAX_STREAM_ORPHANS) {
         return send_sack();
      }
   }

   if (Chunk.flags & 2) { // First fragment: begin a new assembly
      if (receive_records >= MAX_RECEIVE_RECORDS or messages.size() >= MAX_PENDING_MESSAGES or
          assemblies.size() >= MAX_ASSEMBLIES) return send_sack();

      size_t in_channel = 0;
      for (const auto &[begin, assembly] : assemblies) if (assembly.message.stream IS stream) ++in_channel;
      if (in_channel >= MAX_STREAM_ASSEMBLIES) return send_sack();

      size_t reserve = (Chunk.flags & 1) ? payload.size() : limit;
      size_t orphan_bytes = 0;
      if (not (Chunk.flags & 1)) {
         // The beginning may arrive after ACKed tail fragments.  Count those bytes inside, rather than
         // in addition to, this reservation, or a reordered ReceiveLimit-sized message deadlocks.

         uint32_t end_offset = 65536;
         for (const auto &[number, part] : fragments) {
            auto offset = number - tsn;
            if (offset and offset < end_offset and (part.flags & 2)) end_offset = offset;
         }

         for (const auto &[number, part] : fragments) {
            auto offset = number - tsn;
            if (offset and offset < end_offset and part.stream IS stream and part.sequence IS sequence and
                part.ppid IS ppid and bool(part.flags & 4) IS bool(Chunk.flags & 4)) {
               orphan_bytes += part.payload.size();
            }
         }
      }

      if (orphan_bytes > reserve or reserved + receive_bytes + reserve - orphan_bytes > config.receive_window or
          channel_reserved + channel_receive_bytes[stream] + reserve - orphan_bytes > MAX_MESSAGE) return send_sack();

      if (not (Chunk.flags & 4)) {
         auto expected = receive_sequences[stream];
         if (uint16_t(sequence - expected) >= 1024) return fail(ERR::InvalidData);
      }

      Assembly assembly{tsn, sequence, {stream, ppid, bool(Chunk.flags & 4), {}}};

      // Reserve once so the retained allocation is bounded independently of vector growth.

      assembly.message.payload.reserve((Chunk.flags & 1) ? payload.size() : limit);
      assemblies.emplace(tsn, std::move(assembly));
      ++receive_records;
   }

   fragments.emplace(tsn, Fragment{stream, sequence, ppid, Chunk.flags,
      std::vector<uint8_t>(payload.begin(), payload.end())});
   received.insert(tsn);
   receive_bytes += payload.size();
   channel_receive_bytes[stream] += payload.size();
   while (received.erase(cumulative_tsn + 1)) ++cumulative_tsn;

   if (not collect_messages()) return false;

   sack_needed = true;
   if (Chunk.flags & 8) return send_sack(); // The I-bit requests an immediate SACK
   if (not sack_due and not schedule(sack_due, std::min(config.delayed_ack_us, rto_us))) return false;
   return true;
}

//********************************************************************************************************************
// Reassembles stored fragments into complete messages.  Unordered messages are delivered immediately, while ordered
// messages are held until every earlier SSN on their stream has been delivered.  No more than MAX_PENDING_MESSAGES
// are pending delivery at any time.

bool SctpAssociation::collect_messages()
{
   for (auto it = assemblies.begin(); it != assemblies.end() and messages.size() < MAX_PENDING_MESSAGES;) {
      auto &assembly = it->second;
      bool complete = false;

      while (true) {
         auto fragment = fragments.find(assembly.next_tsn);
         if (fragment IS fragments.end()) break;

         auto &part = fragment->second;
         if (part.stream != assembly.message.stream or part.ppid != assembly.message.ppid or
             part.sequence != assembly.sequence or bool(part.flags & 4) != assembly.message.unordered or
             (assembly.next_tsn != it->first and (part.flags & 2))) return fail(ERR::InvalidData);

         if (part.payload.size() > message_limit(assembly.message.ppid) - assembly.message.payload.size()) {
            return fail(ERR::OutOfRange);
         }

         assembly.message.payload.insert(assembly.message.payload.end(), part.payload.begin(), part.payload.end());
         complete = part.flags & 1;
         ++assembly.next_tsn;
         fragments.erase(fragment);
         if (complete) break;
      }

      if (not complete) {
         ++it;
         continue;
      }

      // Completed events retain only their actual payload allocation, not the reassembly reservation.

      std::vector<uint8_t>(assembly.message.payload).swap(assembly.message.payload);

      if (assembly.message.unordered) messages.push_back(std::move(assembly.message));
      else {
         auto stream = assembly.message.stream;
         auto &pending = ordered_messages[stream];
         if (pending.contains(assembly.sequence)) return fail(ERR::InvalidData);
         pending.emplace(assembly.sequence, std::move(assembly.message));
      }

      it = assemblies.erase(it);
   }

   for (auto &[stream, pending] : ordered_messages) {
      auto &sequence = receive_sequences[stream];
      while (messages.size() < MAX_PENDING_MESSAGES) {
         auto item = pending.find(sequence);
         if (item IS pending.end()) break;
         messages.push_back(std::move(item->second));
         pending.erase(item);
         ++sequence;
      }
   }

   if (messages.size() > MAX_PENDING_MESSAGES) return fail(ERR::BufferOverflow);
   return true;
}

//********************************************************************************************************************
// Returns the messages that are ready for delivery.  If RetainCredit is true, the receive window is not reopened
// until the caller releases each message with release_message().

std::vector<SctpMessage> SctpAssociation::take_messages(bool RetainCredit)
{
   auto result = std::exchange(messages, {});

   for (const auto &message : result) {
      if (RetainCredit) continue;
      receive_bytes -= message.payload.size();
      channel_receive_bytes[message.stream] -= message.payload.size();
      --receive_records;
   }

   if (not result.empty()) (void)collect_messages();

   if (not RetainCredit and live_transfer() and not result.empty() and current_state != SctpState::SHUTDOWN_ACK_SENT and
       current_state != SctpState::SHUTDOWN_SENT) {
      // Return credit promptly, including the zero-window reopening case.
      (void)send_sack();
   }

   return result;
}

//********************************************************************************************************************
// Returns the receive credit of a message that was taken with RetainCredit, and advertises the reopened window.

ERR SctpAssociation::release_message(uint16_t Stream, size_t Bytes)
{
   auto it = channel_receive_bytes.find(Stream);
   if (not receive_records or it IS channel_receive_bytes.end() or it->second < Bytes or receive_bytes < Bytes) {
      return ERR::InvalidValue;
   }

   receive_bytes -= Bytes;
   it->second -= Bytes;
   --receive_records;

   if (live_transfer() and current_state != SctpState::SHUTDOWN_ACK_SENT and
       current_state != SctpState::SHUTDOWN_SENT) {
      if (not send_sack()) return last_error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Updates the smoothed RTT and RTO from a round-trip Sample (RFC 9260 §6.3.1).

void SctpAssociation::estimate_rto(uint64_t Sample)
{
   Sample = std::clamp(Sample, uint64_t(1), config.max_rto_us);

   if (not srtt_us) {
      srtt_us = Sample;
      rttvar_us = Sample / 2;
   }
   else {
      auto difference = srtt_us > Sample ? srtt_us - Sample : Sample - srtt_us;
      rttvar_us = (3 * rttvar_us + difference) / 4;
      srtt_us = (7 * srtt_us + Sample) / 8;
   }

   rto_us = std::clamp(srtt_us + std::max(uint64_t(1000), 4 * rttvar_us),
      std::min(config.min_rto_us, config.initial_rto_us), config.max_rto_us);
}

//********************************************************************************************************************
// Processes a SACK.  Acknowledged chunks are released, the RTT is sampled, and the congestion window is updated.
// Chunks that are reported missing by three SACKs are marked for fast retransmission (RFC 9260 §7.2.4).  SACKs that
// would move the cumulative acknowledgement backwards or acknowledge unsent TSNs are ignored.

bool SctpAssociation::receive_sack(std::span<const uint8_t> Value)
{
   uint32_t ack = read32(Value, 0);
   if (after(ack, next_tsn - 1) or after(last_ack, ack)) return true;

   auto gap_count = read16(Value, 8);
   for (size_t i = 0; i < gap_count; ++i) {
      if (after(ack + read16(Value, 14 + 4 * i), next_tsn - 1)) return true;
   }

   bool advanced = after(ack, last_ack);
   size_t old_flight = flight_size, newly_acked = 0;
   uint32_t highest_new = ack;
   bool any_new = advanced;
   bool sampled = false;

   for (auto &chunk : sent) {
      if (chunk.abandoned) continue;

      bool cumulative = not after(chunk.tsn, ack);
      bool gap = false;
      auto offset = chunk.tsn - ack;
      for (size_t i = 0; i < gap_count and not cumulative; ++i) {
         if (offset >= read16(Value, 12 + 4 * i) and offset <= read16(Value, 14 + 4 * i)) {
            gap = true;
            break;
         }
      }

      if ((cumulative or gap) and not chunk.gap_acked) {
         newly_acked += chunk_bytes(chunk);
         if (chunk.in_flight) {
            flight_size -= chunk_bytes(chunk);
            chunk.in_flight = false;
         }
         if (after(chunk.tsn, highest_new)) highest_new = chunk.tsn;
         any_new = true;

         // Karn: never sample a retransmitted chunk or a SACK containing ambiguous cumulative ACKs.

         if (not sampled and chunk.sends IS 1 and now_us >= chunk.sent_us) {
            bool ambiguous = std::any_of(sent.begin(), sent.end(), [ack](const SentChunk &Other) {
               return not after(Other.tsn, ack) and Other.sends > 1;
            });
            if (not ambiguous) {
               estimate_rto(now_us - chunk.sent_us);
               sampled = true;
            }
         }
      }
      else if (chunk.gap_acked and not cumulative and not gap) {
         // A receiver may renege on gap ACKs; retain the payload until cumulative acknowledgement.
         if (not chunk.in_flight) {
            flight_size += chunk_bytes(chunk);
            chunk.in_flight = true;
         }
      }

      chunk.gap_acked = gap;
      if (cumulative or gap) chunk.retry = false;
   }

   while (not sent.empty() and not after(sent.front().tsn, ack)) {
      auto chunk = std::move(sent.front());
      sent.pop_front();

      if (chunk.policy.use_count() IS 1) {
         --channel_message_counts[chunk.stream];
         --send_message_count;
         if (chunk.ppid IS 50) --control_message_count;
      }

      // Charge the complete shared allocation until its last descriptor is released.

      if (chunk.payload and chunk.payload.use_count() IS 1) {
         retained_bytes -= chunk.payload->size();
         if (chunk.ppid IS 50) control_send_bytes -= chunk.payload->size();
         channel_send_bytes[chunk.stream] -= chunk.payload->size();
      }
   }

   last_ack = ack;
   if (advanced) forward_retries = 0;
   if (not forward_progress()) return false;

   peer_window = read32(Value, 4);
   if (recovering and not after(recovery_tsn, ack)) recovering = false;

   // Slow start and congestion avoidance (RFC 9260 §7.2.1, §7.2.2)

   if (advanced and not recovering and (congestion_limited or old_flight >= cwnd)) {
      if (cwnd <= ssthresh) {
         cwnd += std::min(newly_acked, path_mtu);
         ++slow_start_updates;
      }
      else {
         partial_bytes += newly_acked;
         if (partial_bytes >= cwnd) {
            partial_bytes -= cwnd;
            cwnd += path_mtu;
            ++congestion_updates;
         }
      }
   }

   if (advanced) congestion_limited = false;
   cwnd = std::min(cwnd, MAX_PAYLOAD);

   // Fast retransmission

   if (any_new) {
      for (auto &chunk : sent) {
         if (chunk.abandoned or chunk.gap_acked or not after(highest_new, chunk.tsn) or chunk.fast_done) continue;
         if (++chunk.missing < 3) continue;

         chunk.fast_done = true;
         chunk.retry = true;
         ++fast_count;
         if (not send_due and not schedule(send_due, 1000)) return false;

         if (not recovering) {
            ssthresh = std::max(cwnd / 2, 4 * path_mtu);
            cwnd = ssthresh;
            partial_bytes = 0;
            recovery_tsn = next_tsn - 1;
            recovering = true;
         }
      }
   }

   if (sent.empty()) {
      t3_due.reset();
      data_timeouts = 0;
   }
   else if (advanced) {
      data_timeouts = 0;
      if (not schedule(t3_due, rto_us)) return false;
   }

   if (not queued.empty() and flight_size < cwnd) {
      size_t payload_flight = 0;
      for (const auto &chunk : sent) if (not chunk.gap_acked and not chunk.abandoned) payload_flight += chunk.length;
      if ((peer_window > payload_flight or sent.empty()) and not send_due and not schedule(send_due, 1000)) {
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Sends a HEARTBEAT padded to Size bytes.  A heartbeat larger than the current path MTU acts as a PMTU probe.  The
// heartbeat information includes a random nonce, so that only a genuine HEARTBEAT ACK is accepted.

bool SctpAssociation::heartbeat(size_t Size)
{
   if (not control_room(Size)) return false;

   std::array<uint8_t, 8> nonce;
   if (not random_bytes(nonce)) return fail(ERR::Failed);

   std::vector<uint8_t> info(nonce.begin(), nonce.end());
   put64(info, now_us);
   put32(info, uint32_t(Size));
   heartbeat_value.clear();
   if (not append_parameter(heartbeat_value, 1, info)) return fail(ERR::BufferOverflow);

   Encoder encoder(config.local_port, config.remote_port, remote.tag, Size);
   if (not encoder.chunk(HEARTBEAT, 0, heartbeat_value)) return fail(ERR::BufferOverflow);

   size_t used = 12 + 4 + heartbeat_value.size();
   if (Size >= used + 4 and not encoder.chunk(PADDING, 0, std::vector<uint8_t>(Size - used - 4))) {
      return fail(ERR::BufferOverflow);
   }

   if (output.size() >= MAX_OUTPUT) return fail(ERR::BufferOverflow);
   output.push_back(encoder.finish());
   probe_size = Size;
   return schedule(heartbeat_due, rto_us);
}

//********************************************************************************************************************
// Dispatches a chunk that is only valid once the association has been established.

bool SctpAssociation::transfer_chunk(const Item &Chunk)
{
   switch (Chunk.type) {
      case FORWARD_TSN: return remote.forward_tsn ? receive_forward(Chunk.value) : true;

      case RE_CONFIG: return remote.reconfig ? receive_reconfig(Chunk.value) : true;

      case DATA:
         if (current_state IS SctpState::SHUTDOWN_ACK_SENT) return true;
         return receive_data(Chunk);

      case SACK: return receive_sack(Chunk.value);

      case HEARTBEAT:
         if (Chunk.value.size() + 16 > config.packet_capacity) return true;
         return emit(HEARTBEAT_ACK, 0, Chunk.value, remote.tag);

      case HEARTBEAT_ACK: // A successful probe raises the path MTU
         if (heartbeat_value.empty() or not std::ranges::equal(Chunk.value, heartbeat_value)) return true;
         heartbeat_value.clear();
         heartbeat_failures = 0;
         if (probe_size > path_mtu) {
            path_mtu = probe_size;
            probe_attempts = 0;
            if (path_mtu < probe_upper and not schedule(probe_due, std::max(rto_us, uint64_t(1000000)))) return false;
         }
         return schedule(heartbeat_due, config.heartbeat_us);

      case SHUTDOWN: {
         if (after(read32(Chunk.value, 0), next_tsn - 1)) return true;

         // Treat the cumulative TSN in SHUTDOWN as an acknowledgement without gap blocks.

         std::vector<uint8_t> sack(Chunk.value.begin(), Chunk.value.end());
         put32(sack, peer_window);
         put16(sack, 0);
         put16(sack, 0);
         if (not receive_sack(sack)) return false;

         heartbeat_due.reset();
         probe_due.reset();
         heartbeat_value.clear();
         if (current_state != SctpState::SHUTDOWN_ACK_SENT) set_state(SctpState::SHUTDOWN_RECEIVED);
         if (not close_due and not schedule(close_due, config.shutdown_timeout_us)) return false;
         return shutdown_progress();
      }

      case SHUTDOWN_ACK:
         if (current_state IS SctpState::SHUTDOWN_SENT or current_state IS SctpState::SHUTDOWN_ACK_SENT) {
            if (not emit(SHUTDOWN_COMPLETE, 0, {}, remote.tag)) return false;
            finish_transfer();
         }
         return true;

      case SHUTDOWN_COMPLETE:
         if (current_state IS SctpState::SHUTDOWN_ACK_SENT) { finish_transfer(); }
         return true;

      default: return true;
   }
}

//********************************************************************************************************************
// Advances a pending shutdown once all outstanding data has been acknowledged.

bool SctpAssociation::shutdown_progress()
{
   if (not live_transfer() or not queued.empty() or not sent.empty()) return true;

   if (current_state IS SctpState::SHUTDOWN_PENDING) {
      std::vector<uint8_t> value;
      put32(value, cumulative_tsn);
      if (not emit(SHUTDOWN, 0, value, remote.tag)) return false;
      set_state(SctpState::SHUTDOWN_SENT);
      return schedule(shutdown_due, rto_us);
   }

   if (current_state IS SctpState::SHUTDOWN_RECEIVED) {
      if (not emit(SHUTDOWN_ACK, 0, {}, remote.tag)) return false;
      sack_due.reset();
      set_state(SctpState::SHUTDOWN_ACK_SENT);
      return schedule(shutdown_due, rto_us);
   }

   return true;
}

//********************************************************************************************************************
// Begins a graceful shutdown.  Queued data is delivered before SHUTDOWN is sent, subject to the shutdown timeout.

ERR SctpAssociation::shutdown(uint64_t NowUs)
{
   if (not advance(NowUs)) return ERR::InvalidValue;

   if (current_state IS SctpState::CLOSED or current_state IS SctpState::SHUTDOWN_PENDING or
       current_state IS SctpState::SHUTDOWN_SENT or current_state IS SctpState::SHUTDOWN_RECEIVED or
       current_state IS SctpState::SHUTDOWN_ACK_SENT) return ERR::Okay;

   if (current_state != SctpState::ESTABLISHED) return ERR::InvalidState;

   heartbeat_due.reset();
   probe_due.reset();
   heartbeat_value.clear();
   set_state(SctpState::SHUTDOWN_PENDING);
   if (not schedule(close_due, config.shutdown_timeout_us) or not shutdown_progress()) return last_error;
   return ERR::Okay;
}

//********************************************************************************************************************
// Called when the underlying transport switches to a new path.  Congestion, RTT and PMTU state are reset, and all
// unacknowledged DATA is retransmitted on the new path.

ERR SctpAssociation::path_changed(size_t Capacity, uint64_t NowUs)
{
   if (not advance(NowUs)) return ERR::InvalidValue;
   if (Capacity < DATA_PACKET_FLOOR or Capacity > MAX_PACKET_SIZE) return ERR::OutOfRange;

   config.packet_capacity = Capacity;
   if (not live_transfer()) return ERR::Okay;

   // Discard queued old-path packets; retained DATA descriptors regenerate them at the new packet budget.

   output.clear();
   path_mtu = DATA_PACKET_FLOOR;
   probe_upper = Capacity & ~size_t(3);
   cwnd = 4 * path_mtu;
   ssthresh = remote.receive_window;
   partial_bytes = 0;
   recovering = false;
   srtt_us = rttvar_us = 0;
   rto_us = config.initial_rto_us;
   heartbeat_value.clear();
   heartbeat_failures = probe_attempts = 0;
   congestion_limited = false;
   flight_size = 0;

   for (auto &chunk : sent) {
      chunk.in_flight = false;
      chunk.retry = not chunk.gap_acked and not chunk.abandoned;
   }

   if (not sent.empty() and not schedule(t3_due, rto_us)) return last_error;
   if (not schedule(heartbeat_due, config.heartbeat_us) or not schedule(probe_due, rto_us)) return last_error;
   return pump_data() ? ERR::Okay : last_error;
}

//********************************************************************************************************************
// Processes the transfer timers: shutdown guard, delayed SACK, T3-rtx, shutdown retransmission, heartbeat and PMTU
// probing.

bool SctpAssociation::transfer_tick()
{
   if (due(close_due, now_us)) {
      (void)fail(ERR::Timeout);
      (void)emit(ABORT, 0, {}, remote.tag);
      return false;
   }

   if (not expire_messages() or not reset_progress()) return false;
   if (due(sack_due, now_us) and not send_sack()) return false;

   if (due(t3_due, now_us)) {
      if (++data_timeouts > config.max_retransmits) return fail(ERR::Timeout);
      ssthresh = std::max(cwnd / 2, 4 * path_mtu);

      // Zero-window probes back off without collapsing congestion state.

      if (peer_window) {
         cwnd = path_mtu;
         partial_bytes = 0;
         recovering = false;
      }

      // Two failed DATA flights reset packet bundling to the proven minimum, without resegmenting TSNs.

      if (data_timeouts >= 2) {
         path_mtu = DATA_PACKET_FLOOR;
         if (not schedule(probe_due, config.heartbeat_us)) return false;
      }

      for (auto &chunk : sent) {
         if (chunk.gap_acked or chunk.abandoned) continue;
         if (chunk.in_flight) flight_size -= chunk_bytes(chunk);
         chunk.in_flight = false;
         chunk.retry = true;
         chunk.missing = 0;
         chunk.fast_done = false;
      }

      rto_us = rto_us > config.max_rto_us / 2 ? config.max_rto_us : rto_us * 2;
      if (not schedule(t3_due, rto_us)) return false;
   }

   if (due(shutdown_due, now_us)) {
      if (++shutdown_retries > config.max_retransmits) return fail(ERR::Timeout);

      if (current_state IS SctpState::SHUTDOWN_SENT) {
         std::vector<uint8_t> value;
         put32(value, cumulative_tsn);
         if (not emit(SHUTDOWN, 0, value, remote.tag)) return false;
      }
      else if (current_state IS SctpState::SHUTDOWN_ACK_SENT) {
         if (not emit(SHUTDOWN_ACK, 0, {}, remote.tag)) return false;
      }

      rto_us = std::min(config.max_rto_us, rto_us > config.max_rto_us / 2 ? config.max_rto_us : 2 * rto_us);
      if (not schedule(shutdown_due, rto_us)) return false;
   }

   if (due(heartbeat_due, now_us)) {
      if (not heartbeat_value.empty()) { // The previous heartbeat was not acknowledged
         if (probe_size > path_mtu) {
            // Probe loss is not evidence of peer failure.  Retry boundedly, then narrow the search.
            if (++probe_attempts >= 3) {
               probe_upper = probe_size - 4;
               probe_attempts = 0;
            }
            heartbeat_value.clear();
            if (not schedule(probe_due, config.heartbeat_us)) return false;
         }
         else {
            if (++heartbeat_failures > config.max_retransmits) return fail(ERR::Timeout);
            rto_us = rto_us > config.max_rto_us / 2 ? config.max_rto_us : 2 * rto_us;
         }
      }

      if (not heartbeat(DATA_PACKET_FLOOR)) return false;
   }

   // PMTU probes use a binary search between the current MTU and the upper bound.

   if (due(probe_due, now_us) and heartbeat_value.empty()) {
      probe_due.reset();
      if (probe_upper > path_mtu) {
         size_t size = ((path_mtu + probe_upper + 4) / 2) & ~size_t(3);
         if (not heartbeat(size)) return false;
      }
   }
   else if (due(probe_due, now_us)) {
      if (not schedule(probe_due, rto_us)) return false;
   }

   if (not expire_messages() or not forward_progress() or not pump_data()) return false;
   return shutdown_progress();
}

//********************************************************************************************************************
// Count original application bytes only.  Packetisation and retransmission do not imply adapter acceptance.

std::map<uint16_t, size_t> SctpAssociation::buffered_amounts() const
{
   std::map<uint16_t, size_t> result;

   for (const auto &message : queued) {
      if (message.payload and (message.ppid IS 51 or message.ppid IS 53)) {
         result[message.stream] += message.payload->size() - message.offset;
      }
   }

   for (const auto &chunk : sent) {
      if (not chunk.adapter_accepted and not chunk.abandoned and (chunk.ppid IS 51 or chunk.ppid IS 53)) {
         result[chunk.stream] += chunk.length;
      }
   }

   return result;
}

//********************************************************************************************************************
// Called by the owner once an output packet has been accepted by the transport.  The DATA chunks in the packet no
// longer count towards the buffered amount.

void SctpAssociation::output_accepted(std::span<const uint8_t> Packet)
{
   sctp::Packet parsed;
   if (not sctp::parse(Packet, parsed)) return;

   for (const auto &item : parsed.chunks) {
      if (item.type != sctp::DATA or item.value.size() < 12) continue;
      auto tsn = sctp::read32(item.value, 0);
      auto it = std::lower_bound(sent.begin(), sent.end(), tsn,
         [](const SentChunk &Chunk, uint32_t TSN) { return int32_t(Chunk.tsn - TSN) < 0; });
      if (it != sent.end() and it->tsn IS tsn) it->adapter_accepted = true;
   }
}

} // namespace rtc
