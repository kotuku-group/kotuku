// SCTP establishment over an authenticated, single-homed DTLS transport.
// The owner supplies monotonic time and drains output after transport callbacks return.

#pragma once

#include "sctp_packet.h"
#include "sctp_streams.h"

#include <kotuku/system/errors.h>

#include <array>
#include <functional>
#include <optional>
#include <deque>
#include <map>
#include <set>
#include <memory>

namespace rtc {

enum class SctpState {
   CLOSED, COOKIE_WAIT, COOKIE_ECHOED, ESTABLISHED, SHUTDOWN_PENDING, SHUTDOWN_SENT, SHUTDOWN_RECEIVED,
   SHUTDOWN_ACK_SENT, FAILED
};

struct SctpEvent { // Reports a change of association state

   SctpState state;
   ERR error = ERR::Okay;
   bool restarted = false;
};

struct SctpMessage { // A complete, reassembled inbound message

   uint16_t stream = 0;
   uint32_t ppid = 0;
   bool unordered = false;
   std::vector<uint8_t> payload;
};

struct SctpDiagnostics {
   size_t flight_size = 0;
   size_t congestion_window = 0;
   size_t receive_bytes = 0;
   size_t retained_bytes = 0;
   size_t unsent_bytes = 0;
   size_t path_mtu = 0;
   uint64_t rto_us = 0;
   uint64_t retransmissions = 0;
   unsigned fast_retransmissions = 0;
   unsigned slow_start_updates = 0;
   unsigned congestion_avoidance_updates = 0;
};

struct SctpConfig {
   uint16_t local_port = 5000;
   uint16_t remote_port = 5000;
   uint16_t outbound_streams = 512;
   uint16_t inbound_streams = 512;
   uint32_t receive_window = 32 * 1024 * 1024;
   size_t packet_capacity = 1100;
   uint32_t receive_limit = 262144;
   uint64_t min_rto_us = 1000000;
   uint64_t delayed_ack_us = 200000;
   uint64_t heartbeat_us = 30000000;
   uint64_t shutdown_timeout_us = 5000000;
   uint64_t initial_rto_us = 3000000;
   uint64_t max_rto_us = 60000000;
   unsigned max_retransmits = 8;

   // Optional deterministic entropy for tests.  Production uses OpenSSL's CSPRNG.
   std::function<bool(std::span<uint8_t>)> random;
};

struct SctpPeer {
   uint32_t tag = 0;
   uint32_t initial_tsn = 0;
   uint32_t receive_window = 0;

   // Negotiated stream counts from the local endpoint's perspective.
   uint16_t outbound_streams = 0;
   uint16_t inbound_streams = 0;
   bool forward_tsn = false; // Partial reliability (RFC 3758) is supported
   bool reconfig = false;    // Stream reconfiguration (RFC 6525) is supported
};

//********************************************************************************************************************
// A single SCTP association.  The implementation is split across sctp_association.cpp (establishment),
// sctp_transfer.cpp (data transfer and path management) and sctp_streams.cpp (partial reliability and stream
// reconfiguration).

class SctpAssociation {
public:
   static constexpr uint64_t COOKIE_LIFETIME_US = 60000000;
   static constexpr size_t MAX_OUTPUT = 64; // Maximum number of queued output packets

   explicit SctpAssociation(SctpConfig Config = {});
   ~SctpAssociation();
   SctpAssociation(const SctpAssociation &) = delete;
   SctpAssociation & operator=(const SctpAssociation &) = delete;

   [[nodiscard]] ERR start(uint64_t NowUs);
   [[nodiscard]] ERR feed(std::span<const uint8_t> Packet, uint64_t NowUs);
   [[nodiscard]] ERR tick(uint64_t NowUs);
   [[nodiscard]] SctpState state() const { return current_state; }
   [[nodiscard]] ERR error() const { return last_error; }
   [[nodiscard]] const SctpPeer & peer() const { return remote; }

   // Gate channel modes until their extensions have been implemented and negotiated.
   [[nodiscard]] ERR require_extensions(bool ForwardTsn, bool Reconfig) const;
   [[nodiscard]] uint32_t tag() const { return local_tag; }
   [[nodiscard]] uint32_t initial_tsn() const { return local_tsn; }
   [[nodiscard]] std::optional<uint64_t> next_deadline_us() const;

   [[nodiscard]] ERR enqueue(uint16_t Stream, uint32_t PPID, std::span<const uint8_t> Payload,
      bool Unordered, uint64_t NowUs, SctpReliability Policy = {});
   [[nodiscard]] ERR reset_stream(uint16_t Stream, uint64_t NowUs);
   [[nodiscard]] bool stream_reset_complete(uint16_t Stream) const;
   [[nodiscard]] ERR release_stream(uint16_t Stream);
   std::vector<SctpResetEvent> take_resets();
   [[nodiscard]] ERR shutdown(uint64_t NowUs);
   [[nodiscard]] ERR path_changed(size_t Capacity, uint64_t NowUs);
   [[nodiscard]] SctpDiagnostics diagnostics() const;
   std::vector<SctpMessage> take_messages(bool RetainCredit = false);

   // Channel events retain receive credit until the owner drains or discards the complete message.
   [[nodiscard]] ERR release_message(uint16_t Stream, size_t Bytes);
   std::vector<std::vector<uint8_t>> take_output();
   std::vector<SctpEvent> take_events();

   // Application bytes per stream that the transport has not yet accepted.  Streams with no buffered bytes are
   // omitted.  The totals are maintained incrementally and are not recomputed on access.
   [[nodiscard]] const std::map<uint16_t, size_t> & buffered_amounts() const { return buffered_bytes; }
   [[nodiscard]] size_t buffered_amount(uint16_t Stream) const;
   void output_accepted(std::span<const uint8_t> Packet);
   void abort(ERR Error = ERR::Failed);
   void close(); // Immediate owner teardown; shutdown() performs the wire handshake

private:
   struct Init;

   struct PolicyState { // Reliability state that is shared by every chunk of one message

      SctpReliability policy;
      bool abandoned = false;
      std::optional<uint64_t> expires;
   };

   struct SendMessage { // A queued outbound message; offset tracks how much has been fragmented into chunks

      uint16_t stream, sequence;
      uint32_t ppid;
      bool unordered;
      size_t offset = 0;
      std::shared_ptr<std::vector<uint8_t>> payload;
      std::shared_ptr<PolicyState> policy;
   };

   struct SentChunk { // A transmitted DATA chunk that is awaiting acknowledgement

      uint32_t tsn;
      uint16_t stream, sequence;
      uint32_t ppid;
      uint8_t flags;
      size_t offset, length;
      std::shared_ptr<std::vector<uint8_t>> payload;
      std::shared_ptr<PolicyState> policy;
      uint64_t sent_us;
      unsigned sends = 1, missing = 0;
      bool gap_acked = false, in_flight = true, retry = false, fast_done = false, abandoned = false;
      bool adapter_accepted = false;
   };

   struct Fragment { // A received DATA chunk that is awaiting reassembly

      uint16_t stream, sequence;
      uint32_t ppid;
      uint8_t flags;
      std::vector<uint8_t> payload;
   };

   struct Assembly { // A message under reassembly, keyed by the TSN of its first fragment

      uint32_t next_tsn;
      uint16_t sequence;
      SctpMessage message;
   };

   // Data transfer (sctp_transfer.cpp)

   void initialise_transfer();
   void clear_transfer();
   void finish_transfer();
   bool schedule(std::optional<uint64_t> &Timer, uint64_t Delay);
   bool transfer_chunk(const sctp::Item &Chunk);
   bool receive_data(const sctp::Item &Chunk);
   size_t message_limit(uint32_t PPID) const;
   bool receive_sack(std::span<const uint8_t> Value);
   bool send_sack();
   bool collect_messages();
   bool pump_data();
   bool transfer_tick();
   bool heartbeat(size_t Size);
   bool shutdown_progress();
   void estimate_rto(uint64_t Sample);
   size_t chunk_bytes(const SentChunk &Chunk) const;
   void release_buffered(uint16_t Stream, uint32_t PPID, size_t Bytes);
   static bool buffered_ppid(uint32_t PPID) { return PPID IS 51 or PPID IS 53; } // Text and binary messages
   std::vector<uint8_t> data_value(const SentChunk &Chunk) const;
   bool live_transfer() const;

   // Partial reliability and stream reconfiguration (sctp_streams.cpp)

   bool expire_messages();
   bool abandon_message(const std::shared_ptr<PolicyState> &Policy);
   bool forward_progress(bool Force = false);
   bool receive_forward(std::span<const uint8_t> Value);
   bool receive_reconfig(std::span<const uint8_t> Value);
   bool reset_progress();
   bool reset_response(uint32_t Sequence, uint32_t Result);
   bool complete_incoming_reset();
   bool acknowledge_reset();
   void clear_streams();

   struct ResetState { bool incoming = false, outgoing = false; uint64_t deadline = 0; };
   struct ResetRequest { uint32_t sequence, barrier; std::vector<uint16_t> streams; };
   std::map<uint16_t, ResetState> resetting;
   std::optional<ResetRequest> outgoing_reset, incoming_reset;
   uint32_t reset_sequence = 0, peer_reset_sequence = 0, peer_reset_result = 0;
   uint64_t reset_rto = 0;
   unsigned reset_retries = 0, forward_retries = 0;
   std::vector<uint8_t> reset_wire;
   std::vector<SctpResetEvent> reset_events;
   std::optional<uint64_t> forward_due, reset_due;

   static constexpr size_t MAX_MESSAGE = 16 * 1024 * 1024;
   static constexpr size_t MAX_PAYLOAD = 32 * 1024 * 1024;
   static constexpr size_t MAX_CHUNKS = 65536;

   // Send queue admission.  DCEP control messages have a separate reservation so that an application backlog
   // cannot reject a channel's opening ACK.

   static constexpr size_t MAX_QUEUED_MESSAGES = 4096;      // Application messages across all streams
   static constexpr size_t MAX_STREAM_MESSAGES = 1024;      // Application messages on one stream
   static constexpr size_t MAX_CONTROL_MESSAGES = 512;
   static constexpr size_t MAX_CONTROL_BYTES = 256 * 1024;

   // Output.  Control packets are bounded by bytes separately from DATA.  A DATA burst leaves output slots free for
   // SACK, HEARTBEAT and shutdown.  State events are bounded independently of output despite the equal value.

   static constexpr size_t MAX_CONTROL_OUTPUT_BYTES = 256 * 1024;
   static constexpr size_t OUTPUT_RESERVE = 8;
   static constexpr size_t MAX_BURST_PACKETS = 48;
   static constexpr size_t MAX_BURST_CHUNKS = 256;
   static constexpr size_t MAX_STATE_EVENTS = 64;
   static_assert(MAX_BURST_PACKETS <= MAX_OUTPUT - OUTPUT_RESERVE);

   // Receive admission.  A record is a message from its first fragment until its receive credit is released.

   static constexpr size_t MAX_RECEIVE_RECORDS = 4096;
   static constexpr size_t MAX_PENDING_MESSAGES = 64;       // Complete messages awaiting take_messages()
   static constexpr size_t MAX_ASSEMBLIES = 256;            // Partial messages, including orphaned fragments
   static constexpr size_t MAX_STREAM_ASSEMBLIES = 16;
   static constexpr size_t MAX_STREAM_ORPHANS = 16;         // Fragments received before their first fragment
   static constexpr size_t MAX_DEFERRED_FRAGMENTS = 256;    // Held behind a pending incoming stream reset

   // DATA remains retransmittable down to the codec's minimum supported packet capacity.
   static constexpr size_t DATA_PACKET_FLOOR = 128;

   // Disjoint hard reservations cover payloads, one completion copy, descriptors/nodes, records and output.
   // The generous metadata allowance includes 64-bit container links, shared owners and allocation overhead.
   // The channel reserve covers channel metadata/events and the bounded aggregate label/protocol storage.
   // RECORD_METADATA_ALLOWANCE is reserved for every receive record and every queued send message (its SendMessage
   // descriptor and shared PolicyState).

   static constexpr size_t CHUNK_METADATA_ALLOWANCE = 256;
   static constexpr size_t RECORD_METADATA_ALLOWANCE = 512;
   static constexpr size_t CHANNEL_METADATA_RESERVE = 2 * 1024 * 1024;
   static constexpr size_t MEMORY_BUDGET = 128 * 1024 * 1024;
   static_assert(SctpChannels::MAX_LABEL_BYTES < CHANNEL_METADATA_RESERVE);
   static constexpr size_t MAX_SEND_RECORDS = MAX_QUEUED_MESSAGES + MAX_CONTROL_MESSAGES;
   static_assert(sizeof(SendMessage) + sizeof(PolicyState) <= RECORD_METADATA_ALLOWANCE);
   static_assert(2 * MAX_PAYLOAD + MAX_MESSAGE +
      MAX_CHUNKS * (sizeof(SentChunk) + sizeof(Fragment) + CHUNK_METADATA_ALLOWANCE) +
      (MAX_RECEIVE_RECORDS + MAX_SEND_RECORDS) * RECORD_METADATA_ALLOWANCE + MAX_OUTPUT * sctp::MAX_PACKET_SIZE +
      MAX_CONTROL_BYTES + CHANNEL_METADATA_RESERVE < MEMORY_BUDGET);

   std::deque<SendMessage> queued;
   std::deque<SentChunk> sent;
   std::map<uint16_t, uint16_t> send_sequences, receive_sequences;
   std::map<uint32_t, Fragment> fragments, reset_deferred;
   std::set<uint32_t> received;
   std::vector<uint32_t> duplicates;
   std::map<uint32_t, Assembly> assemblies;
   std::map<uint16_t, std::map<uint16_t, SctpMessage>> ordered_messages;
   std::vector<SctpMessage> messages;
   std::map<uint16_t, size_t> channel_send_bytes, channel_receive_bytes, channel_message_counts;

   // Text and binary bytes per stream that are queued or packetised but not yet accepted by the transport, and not
   // abandoned.  Charged by enqueue() and released by output_accepted(), abandonment and cumulative acknowledgement.
   std::map<uint16_t, size_t> buffered_bytes;
   uint32_t next_tsn = 0, cumulative_tsn = 0, last_ack = 0, peer_window = 0, recovery_tsn = 0;
   size_t flight_size = 0, cwnd = 0, ssthresh = 0, partial_bytes = 0, path_mtu = 0;
   size_t retained_bytes = 0, unsent_bytes = 0, receive_bytes = 0, receive_records = 0, send_message_count = 0;
   size_t control_send_bytes = 0, control_message_count = 0;
   size_t partial_message_count = 0; // Messages with a lifetime or retransmission limit, until fully acknowledged
   uint64_t srtt_us = 0, rttvar_us = 0;
   unsigned data_timeouts = 0, fast_count = 0, ack_packets = 0;
   uint64_t retransmission_count = 0;
   unsigned heartbeat_failures = 0, probe_attempts = 0;
   unsigned slow_start_updates = 0, congestion_updates = 0;
   bool transfer_ready = false, recovering = false, sack_needed = false, congestion_limited = false;
   uint16_t last_stream = 65535;
   size_t probe_size = 0, probe_upper = 0;
   std::vector<uint8_t> heartbeat_value;
   std::optional<uint64_t> t3_due, sack_due, heartbeat_due, probe_due, close_due, shutdown_due, send_due;
   unsigned shutdown_retries = 0;

   // Establishment (sctp_association.cpp)

   bool advance(uint64_t NowUs);
   bool identity();
   bool random_bytes(std::span<uint8_t> Bytes);
   bool control_room(size_t Bytes);
   bool emit(uint8_t Type, uint8_t Flags, std::span<const uint8_t> Value, uint32_t Tag);
   bool error_cause(uint16_t Cause, std::span<const uint8_t> Value, uint32_t Tag, bool Abort = false);
   bool send_init();
   bool send_init_ack(const Init &Value);
   bool receive_cookie(std::span<const uint8_t> Value, uint32_t Tag, bool &Accepted);
   bool arm();
   void set_state(SctpState State, ERR Error = ERR::Okay, bool Restarted = false);
   bool fail(ERR Error);

   SctpConfig config;
   SctpState current_state = SctpState::CLOSED;
   SctpPeer remote;
   ERR last_error = ERR::Okay;
   uint32_t local_tag = 0;
   uint32_t local_tsn = 0;
   std::array<uint8_t, 32> secret{};
   bool ready = false;
   bool has_clock = false;
   uint64_t now_us = 0;
   uint64_t rto_us = 0;
   unsigned retransmits = 0;
   unsigned stale_retries = 0;
   std::optional<uint64_t> deadline;
   std::vector<uint8_t> retry_packet;
   std::vector<std::vector<uint8_t>> output;
   std::vector<SctpEvent> events;
};

} // namespace rtc
