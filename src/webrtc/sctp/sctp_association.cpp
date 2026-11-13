// SCTP association establishment and teardown (RFC 9260).  The four-way handshake uses a stateless HMAC-signed cookie,
// so no association state is retained for an INIT until the peer echoes a valid cookie.  Data transfer is implemented
// in sctp_transfer.cpp and stream management in sctp_streams.cpp.

#include "sctp_association.h"
#include "../../crypto/protocol_primitives.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rtc {
using namespace sctp;

namespace {

constexpr uint32_t COOKIE_MAGIC = 0x4b534301; // Versioned private cookie format
constexpr size_t COOKIE_BODY = 56;            // Cookie fields that are covered by the HMAC
constexpr size_t COOKIE_SIZE = COOKIE_BODY + 32;

//********************************************************************************************************************
// Returns true for chunk types that the association recognises.  Unknown chunks are handled according to the action
// bits in their type (RFC 9260 §3.2).

bool known_chunk(uint16_t Type)
{
   switch (Type) {
      case DATA: case INIT: case INIT_ACK: case SACK: case HEARTBEAT: case HEARTBEAT_ACK:
      case ABORT: case SHUTDOWN: case SHUTDOWN_ACK: case ERROR_CHUNK: case COOKIE_ECHO: case COOKIE_ACK:
      case SHUTDOWN_COMPLETE: case PADDING: case FORWARD_TSN: case RE_CONFIG: return true;
      default: return false;
   }
}

} // namespace

//********************************************************************************************************************
// Parsed form of an INIT or INIT ACK chunk.  Unknown parameters that request a report are collected in unrecognised
// so that they can be returned to the peer.

struct SctpAssociation::Init {
   SctpPeer peer;
   std::span<const uint8_t> cookie;
   std::vector<uint8_t> unrecognised;
   bool hostname = false; // True if the peer sent a Host Name Address, which is not supported

   bool parse(const Item &Chunk) {
      if (Chunk.value.size() < 16) return false;

      peer.tag = read32(Chunk.value, 0);
      peer.receive_window = read32(Chunk.value, 4);
      peer.outbound_streams = read16(Chunk.value, 8);
      peer.inbound_streams = read16(Chunk.value, 10);
      peer.initial_tsn = read32(Chunk.value, 12);

      std::vector<Item> params;
      if (not parameters(Chunk.value.subspan(16), params)) return false;

      for (auto &param : params) {
         switch (param.type) {
            case STATE_COOKIE:
               if (Chunk.type != INIT_ACK or not cookie.empty() or param.value.empty() or
                   param.value.size() > 512) return false;
               cookie = param.value;
               break;

            case PR_SUPPORTED:
               if (not param.value.empty()) return false;
               peer.forward_tsn = true;
               break;

            case EXTENSIONS:
               for (auto type : param.value) {
                  if (type IS RE_CONFIG) peer.reconfig = true;
               }
               break;

            case 5: if (param.value.size() != 4) return false; break; // IPv4 ignored over DTLS
            case 6: if (param.value.size() != 16) return false; break; // IPv6 ignored over DTLS
            case 9: if (param.value.size() != 4) return false; break; // Cookie preservative: fixed local lifetime
            case 11: hostname = true; break;
            case 12: if (param.value.size() % 2) return false; break; // Supported address types
            case UNRECOGNISED: break;

            default: {
               auto action = unknown_parameter(param.type);
               if (action.report) {
                  while (unrecognised.size() % 4) unrecognised.push_back(0);
                  unrecognised.insert(unrecognised.end(), param.wire.begin(), param.wire.end());
               }
               if (action.stop) return true;
               break;
            }
         }
      }

      return true;
   }
};

//********************************************************************************************************************

SctpAssociation::SctpAssociation(SctpConfig Config) : config(std::move(Config)) { }

SctpAssociation::~SctpAssociation() { crypto_protocol::secure_wipe(secret.data(), secret.size()); }

//********************************************************************************************************************
// Fills Bytes from the configured random source, defaulting to the system generator.  Tests can supply a
// deterministic source.

bool SctpAssociation::random_bytes(std::span<uint8_t> Bytes)
{
   return config.random ? config.random(Bytes) : crypto_protocol::random_bytes(Bytes);
}

//********************************************************************************************************************
// Validates the configuration and generates the cookie secret, verification tag and initial TSN.  This is performed
// once, on the first call to start() or the first INIT received.

bool SctpAssociation::identity()
{
   if (ready) return true;

   if (not config.local_port or not config.remote_port or not config.inbound_streams or
       not config.outbound_streams or config.inbound_streams > 512 or config.outbound_streams > 512 or
       config.receive_window < 1500 or config.packet_capacity < 128 or
       config.packet_capacity > MAX_PACKET_SIZE or not config.initial_rto_us or
       config.initial_rto_us > config.max_rto_us or config.max_rto_us > 60000000 or not config.receive_limit or
       config.receive_limit > MAX_MESSAGE or config.receive_limit > config.receive_window or
       config.receive_window > MAX_PAYLOAD or not config.heartbeat_us or not config.delayed_ack_us or
       not config.shutdown_timeout_us) return fail(ERR::InvalidValue);

   std::array<uint8_t, 8> values;
   if (not random_bytes(secret) or not random_bytes(values)) return fail(ERR::Failed);

   local_tag = read32(values, 0);
   if (not local_tag) local_tag = 1;
   local_tsn = read32(values, 4);
   ready = true;
   return true;
}

//********************************************************************************************************************
// Advances the association clock.  Time is monotonic, so a timestamp earlier than the last one is rejected.

bool SctpAssociation::advance(uint64_t NowUs)
{
   if (has_clock and NowUs < now_us) return false;
   now_us = NowUs;
   has_clock = true;
   return true;
}

//********************************************************************************************************************
// Changes the association state and reports it as an event.  Entering ESTABLISHED initialises the transfer state, and
// the change is abandoned if that fails.

void SctpAssociation::set_state(SctpState State, ERR Error, bool Restarted)
{
   if (current_state IS State and not Restarted) return;

   if (State IS SctpState::ESTABLISHED and (not transfer_ready or Restarted)) {
      initialise_transfer();
      if (not transfer_ready) return;
   }

   current_state = State;
   last_error = Error;
   if (events.size() < MAX_STATE_EVENTS) events.push_back({ State, Error, Restarted });
}

//********************************************************************************************************************
// Terminates the association with Error.  Always returns false so that callers can propagate the failure directly.

bool SctpAssociation::fail(ERR Error)
{
   deadline.reset();
   retry_packet.clear();
   output.clear();
   clear_transfer();
   set_state(SctpState::FAILED, Error);
   return false;
}

//********************************************************************************************************************
// Control packets are bounded separately from DATA, so that a peer cannot provoke unlimited control output.

bool SctpAssociation::control_room(size_t Bytes)
{
   size_t used = retry_packet.size();
   for (const auto &packet : output) if (packet[12] != DATA) used += packet.size();
   return used + Bytes <= MAX_CONTROL_OUTPUT_BYTES ? true : fail(ERR::BufferOverflow);
}

//********************************************************************************************************************
// Queues a packet that contains a single chunk.

bool SctpAssociation::emit(uint8_t Type, uint8_t Flags, std::span<const uint8_t> Value, uint32_t Tag)
{
   if (not control_room(12 + ((4 + Value.size() + 3) & ~size_t(3)))) return false;

   Encoder encoder(config.local_port, config.remote_port, Tag, config.packet_capacity);
   if (not encoder.chunk(Type, Flags, Value)) return fail(ERR::BufferOverflow);
   if (output.size() >= MAX_OUTPUT) return fail(ERR::BufferOverflow);

   output.push_back(encoder.finish());
   return true;
}

//********************************************************************************************************************
// Sends an ERROR chunk, or an ABORT if Abort is true, carrying a single error cause.

bool SctpAssociation::error_cause(uint16_t Cause, std::span<const uint8_t> Value, uint32_t Tag, bool Abort)
{
   // Optional error reports must not make a valid association fail when the offending item exceeds the MTU.

   if (Value.size() + 20 > config.packet_capacity) return true;

   std::vector<uint8_t> causes;
   if (not append_parameter(causes, Cause, Value)) return fail(ERR::BufferOverflow);
   return emit(Abort ? ABORT : ERROR_CHUNK, 0, causes, Tag);
}

//********************************************************************************************************************
// Arms the handshake retransmission timer with the current RTO.

bool SctpAssociation::arm()
{
   if (rto_us > std::numeric_limits<uint64_t>::max() - now_us) return fail(ERR::Timeout);
   deadline = now_us + rto_us;
   return true;
}

//********************************************************************************************************************
// Sends INIT and enters COOKIE-WAIT.  The packet is retained for retransmission.

bool SctpAssociation::send_init()
{
   std::vector<uint8_t> value;
   put32(value, local_tag);
   put32(value, config.receive_window);
   put16(value, config.outbound_streams);
   put16(value, config.inbound_streams);
   put32(value, local_tsn);

   if (not append_parameter(value, PR_SUPPORTED, {}) or
       not append_parameter(value, EXTENSIONS, std::array<uint8_t, 2>{FORWARD_TSN, RE_CONFIG})) {
      return fail(ERR::BufferOverflow);
   }

   // No address, AUTH or interleaving extension is advertised.

   if (not emit(INIT, 0, value, 0)) return false;
   retry_packet = output.back();
   set_state(SctpState::COOKIE_WAIT);
   return arm();
}

//********************************************************************************************************************
// Starts an active open from the CLOSED state.

ERR SctpAssociation::start(uint64_t NowUs)
{
   if (current_state != SctpState::CLOSED) return ERR::InvalidState;
   if (not advance(NowUs)) return ERR::InvalidValue;
   if (not identity()) return last_error;

   retransmits = 0;
   stale_retries = 0;
   rto_us = config.initial_rto_us;
   return send_init() ? ERR::Okay : last_error;
}

//********************************************************************************************************************
// Responds to INIT with INIT ACK and a signed state cookie.  If the association is already established, a fresh tag
// and TSN are generated so that a valid cookie restarts the association (RFC 9260 §5.2.2).  The existing tags are
// recorded in the cookie as tie-tags, which receive_cookie() uses to distinguish restarts from collisions.

bool SctpAssociation::send_init_ack(const Init &Value)
{
   uint32_t tag = local_tag;
   uint32_t tsn = local_tsn;
   uint32_t tie_local = 0, tie_remote = 0;

   if (current_state IS SctpState::COOKIE_ECHOED or current_state IS SctpState::ESTABLISHED) {
      tie_local = local_tag;
      tie_remote = remote.tag;
   }

   if (current_state IS SctpState::ESTABLISHED) {
      std::array<uint8_t, 8> values;
      if (not random_bytes(values)) return fail(ERR::Failed);
      tag = read32(values, 0);
      if (not tag or tag IS local_tag) tag = local_tag IS 1 ? 2 : 1;
      tsn = read32(values, 4);
   }

   auto inbound = std::min(config.inbound_streams, Value.peer.outbound_streams);
   auto outbound = std::min(config.outbound_streams, Value.peer.inbound_streams);

   std::vector<uint8_t> cookie;
   put32(cookie, COOKIE_MAGIC);
   put16(cookie, config.local_port);
   put16(cookie, config.remote_port);
   put64(cookie, now_us);
   put32(cookie, tag);
   put32(cookie, Value.peer.tag);
   put32(cookie, tsn);
   put32(cookie, Value.peer.initial_tsn);
   put32(cookie, Value.peer.receive_window);
   put16(cookie, inbound);
   put16(cookie, outbound);
   put32(cookie, (Value.peer.forward_tsn ? 1u : 0u) | (Value.peer.reconfig ? 2u : 0u));
   put32(cookie, 0); // Reserved for future cookie format changes
   put32(cookie, tie_local);
   put32(cookie, tie_remote);

   std::array<uint8_t, 32> mac;
   if (not crypto_protocol::hmac_sha256(secret, cookie, mac)) return fail(ERR::Failed);
   cookie.insert(cookie.end(), mac.begin(), mac.end());

   std::vector<uint8_t> value;
   put32(value, tag);
   put32(value, config.receive_window);
   put16(value, config.outbound_streams);
   put16(value, config.inbound_streams);
   put32(value, tsn);

   if (not append_parameter(value, STATE_COOKIE, cookie) or not append_parameter(value, PR_SUPPORTED, {}) or
       not append_parameter(value, EXTENSIONS, std::array<uint8_t, 2>{FORWARD_TSN, RE_CONFIG})) {
      return fail(ERR::BufferOverflow);
   }

   if (not Value.unrecognised.empty() and Value.unrecognised.size() + value.size() + 20 <= config.packet_capacity and
       not append_parameter(value, UNRECOGNISED, Value.unrecognised)) {
      return fail(ERR::BufferOverflow);
   }

   return emit(INIT_ACK, 0, value, Value.peer.tag);
}

//********************************************************************************************************************
// Processes COOKIE ECHO.  The cookie is authenticated and checked for staleness before it is compared against the
// current association to detect duplicates, restarts and collisions (RFC 9260 §5.2.4).  Accepted is set to true if
// the cookie establishes or restarts the association.

bool SctpAssociation::receive_cookie(std::span<const uint8_t> Value, uint32_t Tag, bool &Accepted)
{
   Accepted = false;
   if (Value.size() != COOKIE_SIZE or read32(Value, 0) != COOKIE_MAGIC) return true;

   std::array<uint8_t, 32> mac;
   if (not crypto_protocol::hmac_sha256(secret, Value.first(COOKIE_BODY), mac)) return fail(ERR::Failed);
   if (not crypto_protocol::constant_time_equal(mac, Value.subspan(COOKIE_BODY))) return true;

   auto cookie_local = read32(Value, 16);
   auto cookie_peer = read32(Value, 20);
   if (Tag != cookie_local or read16(Value, 4) != config.local_port or
       read16(Value, 6) != config.remote_port or not cookie_local or not cookie_peer) return true;

   if (live_transfer() and current_state != SctpState::ESTABLISHED) {
      return error_cause(10, {}, remote.tag); // Cookie received while shutting down
   }

   auto issued = read64(Value, 8);
   if (issued > now_us) return true;

   bool duplicate = current_state IS SctpState::ESTABLISHED and cookie_local IS local_tag and cookie_peer IS remote.tag;
   if (now_us - issued > COOKIE_LIFETIME_US and not duplicate) {
      std::vector<uint8_t> stale;
      put32(stale, uint32_t(std::min(now_us - issued - COOKIE_LIFETIME_US, uint64_t(0xffffffffu))));
      return error_cause(3, stale, cookie_peer);
   }

   bool restarted = false;
   if (current_state != SctpState::CLOSED) {
      bool local_matches = cookie_local IS local_tag;
      bool peer_matches = cookie_peer IS remote.tag;
      bool restart = not local_matches and not peer_matches and read32(Value, 48) IS local_tag and
         read32(Value, 52) IS remote.tag and remote.tag != 0;
      bool collision = local_matches and (not peer_matches or not remote.tag);
      if (not duplicate and not restart and not collision and not (local_matches and peer_matches)) return true;
      restarted = restart;
   }

   if (not duplicate) {
      local_tag = cookie_local;
      local_tsn = read32(Value, 24);
      remote.tag = cookie_peer;
      remote.initial_tsn = read32(Value, 28);
      remote.receive_window = read32(Value, 32);
      remote.inbound_streams = read16(Value, 36); // Negotiated receive streams from our perspective
      remote.outbound_streams = read16(Value, 38);
      remote.forward_tsn = read32(Value, 40) & 1;
      remote.reconfig = read32(Value, 40) & 2;
   }

   deadline.reset();
   retry_packet.clear();
   set_state(SctpState::ESTABLISHED, ERR::Okay, restarted);
   if (current_state != SctpState::ESTABLISHED) return false;

   Accepted = true;
   return emit(COOKIE_ACK, 0, {}, remote.tag);
}

//********************************************************************************************************************
// Processes an inbound SCTP packet.  Packets that fail authentication, port or verification tag checks are silently
// discarded.  Every chunk is validated before any is processed, so that a malformed packet cannot partially change
// the association state.

ERR SctpAssociation::feed(std::span<const uint8_t> Data, uint64_t NowUs)
{
   if (current_state IS SctpState::FAILED) return ERR::InvalidState;
   if (not advance(NowUs)) return ERR::InvalidValue;

   Packet packet;
   if (not parse(Data, packet) or packet.source_port != config.remote_port or
       packet.destination_port != config.local_port) return ERR::Okay;

   auto &first = packet.chunks.front();
   bool init = first.type IS INIT;
   bool cookie = first.type IS COOKIE_ECHO;
   bool reflected = std::any_of(packet.chunks.begin(), packet.chunks.end(), [](const Item &Chunk) {
      return (Chunk.type IS ABORT or Chunk.type IS SHUTDOWN_COMPLETE) and (Chunk.flags & 1);
   });

   if (cookie and reflected) return ERR::Okay;

   if (current_state IS SctpState::CLOSED and not init and not cookie) {
      // RFC 9260 §8.4: avoid ABORT loops; answer other out-of-the-blue traffic with a reflected tag.
      if (first.type IS ABORT or first.type IS INIT_ACK or first.type IS COOKIE_ACK or
          first.type IS SHUTDOWN_COMPLETE) return ERR::Okay;
      if (not emit(first.type IS SHUTDOWN_ACK ? SHUTDOWN_COMPLETE : ABORT, 1, {},
          packet.verification_tag)) return last_error;
      return ERR::Okay;
   }

   if (init) {
      if (packet.verification_tag != 0 or packet.chunks.size() != 1) return ERR::Okay;
   }
   else if (cookie) {
      // The tag is authenticated against the cookie before any state change, including a restart.
      if (not packet.verification_tag or not ready) return ERR::Okay;
   }
   else if (not ready or (reflected and not remote.tag) or
       packet.verification_tag != (reflected ? remote.tag : local_tag)) return ERR::Okay;

   // Validate all framing/known control payloads before changing association state.

   Init parsed_init;
   for (auto &chunk : packet.chunks) {
      if (chunk.type IS INIT or chunk.type IS INIT_ACK) {
         if (packet.chunks.size() != 1 or not parsed_init.parse(chunk)) return ERR::Okay;
      }

      if (chunk.type IS COOKIE_ECHO and &chunk != &packet.chunks.front()) return ERR::Okay;
      if (chunk.type IS COOKIE_ACK and not chunk.value.empty()) return ERR::Okay;
      if (chunk.type IS DATA and (chunk.value.size() <= 12 or (chunk.flags & 0xf0))) return ERR::Okay;
      if (chunk.type IS FORWARD_TSN and (chunk.flags or chunk.value.size() < 4 or
          chunk.value.size() % 4)) return ERR::Okay;

      if (chunk.type IS RE_CONFIG) {
         std::vector<Item> params;
         if (not parameters(chunk.value, params) or params.empty() or params.size() > 2) {
            return ERR::Okay;
         }

         for (const auto &param : params) {
            if (param.type IS 13 and (param.value.size() < 12 or param.value.size() % 2)) return ERR::Okay;
            if (param.type IS 14 and (param.value.size() < 4 or param.value.size() % 2)) return ERR::Okay;
            if (param.type IS 15 and param.value.size() != 4) return ERR::Okay;
            if ((param.type IS 17 or param.type IS 18) and param.value.size() != 8) return ERR::Okay;
            if (param.type IS 16 and param.value.size() != 8 and param.value.size() != 16) return ERR::Okay;
         }
      }

      if (chunk.type IS SACK) { // Gap blocks must be ascending and non-overlapping
         if (chunk.value.size() < 12) return ERR::Okay;

         auto gaps = read16(chunk.value, 8);
         auto dups = read16(chunk.value, 10);
         if (chunk.value.size() != 12u + 4u * (size_t(gaps) + dups)) return ERR::Okay;

         uint16_t previous = 0;
         for (size_t i = 0; i < gaps; ++i) {
            auto begin = read16(chunk.value, 12 + i * 4);
            auto end = read16(chunk.value, 14 + i * 4);
            if (begin <= previous or end < begin) return ERR::Okay;
            previous = end;
         }
      }

      if (chunk.type IS HEARTBEAT or chunk.type IS HEARTBEAT_ACK) {
         std::vector<Item> params;
         if (not parameters(chunk.value, params) or params.size() != 1 or params.front().type != 1) {
            return ERR::Okay;
         }
      }

      if (chunk.type IS SHUTDOWN and chunk.value.size() != 4) return ERR::Okay;

      if ((chunk.type IS SHUTDOWN_ACK or chunk.type IS SHUTDOWN_COMPLETE) and not chunk.value.empty()) {
         return ERR::Okay;
      }

      if (chunk.type IS ABORT or chunk.type IS ERROR_CHUNK) {
         std::vector<Item> causes;
         if (not parameters(chunk.value, causes)) return ERR::Okay;
      }
   }

   if (init and not identity()) return last_error;

   // Process the validated chunks

   for (auto &chunk : packet.chunks) {
      if (chunk.type IS INIT or chunk.type IS INIT_ACK) {
         if (chunk.type IS INIT_ACK and current_state != SctpState::COOKIE_WAIT) continue;

         auto &value = parsed_init;
         if (not value.peer.tag or not value.peer.outbound_streams or not value.peer.inbound_streams or
             value.peer.receive_window < 1500 or value.hostname) {
            if (value.peer.tag and chunk.type IS INIT) {
               (void)error_cause(value.hostname ? 5 : 7, {}, value.peer.tag, true);
            }

            if (chunk.type IS INIT_ACK) {
               // RFC 9260 erratum 7148: invalid INIT-ACK destroys only the forming TCB.
               (void)fail(ERR::InvalidData);
               if (value.peer.tag) (void)error_cause(value.hostname ? 5 : 7, {}, value.peer.tag, true);
               return last_error;
            }
            continue;
         }

         if (chunk.type IS INIT) {
            if (not send_init_ack(value)) return last_error;
         }
         else {
            if (value.cookie.empty()) {
               (void)fail(ERR::InvalidData);
               std::vector<uint8_t> missing;
               put32(missing, 1);
               put16(missing, STATE_COOKIE);
               (void)error_cause(2, missing, value.peer.tag, true);
               return last_error;
            }

            remote = value.peer;
            remote.outbound_streams = std::min(config.outbound_streams, value.peer.inbound_streams);
            remote.inbound_streams = std::min(config.inbound_streams, value.peer.outbound_streams);
            if (not emit(COOKIE_ECHO, 0, value.cookie, remote.tag)) return last_error;

            retry_packet = output.back();
            retransmits = 0;
            rto_us = config.initial_rto_us;
            set_state(SctpState::COOKIE_ECHOED);
            if (not arm()) return last_error;

            if (not value.unrecognised.empty() and not error_cause(8, value.unrecognised, remote.tag)) {
               return last_error;
            }
         }
      }
      else if (chunk.type IS COOKIE_ECHO) {
         if (&chunk != &packet.chunks.front()) return ERR::Okay;
         bool accepted;
         if (not receive_cookie(chunk.value, packet.verification_tag, accepted)) return last_error;
         if (not accepted) return ERR::Okay;
      }
      else if (chunk.type IS COOKIE_ACK) {
         if (current_state IS SctpState::COOKIE_ECHOED) {
            deadline.reset();
            retry_packet.clear();
            set_state(SctpState::ESTABLISHED);
         }
      }
      else if (chunk.type IS ABORT) {
         if (current_state != SctpState::CLOSED) return fail(ERR::Failed), last_error;
         return ERR::Okay;
      }
      else if (chunk.type IS ERROR_CHUNK) { // A Stale Cookie error restarts the handshake
         std::vector<Item> causes;
         (void)parameters(chunk.value, causes);
         for (auto &cause : causes) {
            if (cause.type IS 3 and cause.value.size() IS 4 and current_state IS SctpState::COOKIE_ECHOED) {
               if (stale_retries++ >= config.max_retransmits) return fail(ERR::Timeout), last_error;
               retransmits = 0;
               if (not send_init()) return last_error;
            }
         }
      }
      else if (live_transfer() and (chunk.type IS DATA or chunk.type IS SACK or
          chunk.type IS HEARTBEAT or chunk.type IS HEARTBEAT_ACK or chunk.type IS SHUTDOWN or
          chunk.type IS SHUTDOWN_ACK or chunk.type IS SHUTDOWN_COMPLETE or
          chunk.type IS FORWARD_TSN or chunk.type IS RE_CONFIG)) {
         if (not transfer_chunk(chunk)) return last_error;
      }
      else if (not known_chunk(chunk.type)) {
         auto action = unknown_chunk(uint8_t(chunk.type));
         if (action.report and remote.tag and not error_cause(6, chunk.wire, remote.tag)) return last_error;
         if (action.stop) break;
      }
   }

   if (live_transfer()) {
      bool data_packet = std::any_of(packet.chunks.begin(), packet.chunks.end(), [](const Item &Chunk) {
         return Chunk.type IS DATA;
      });

      // Acknowledge every second DATA packet immediately, and any packet while gaps are outstanding.

      if (sack_needed and data_packet and (++ack_packets >= 2 or not received.empty())) {
         if (not send_sack()) return last_error;
      }

      if (not reset_progress() or not forward_progress() or not shutdown_progress()) return last_error;
   }

   return current_state IS SctpState::FAILED ? last_error : ERR::Okay;
}

//********************************************************************************************************************
// Processes timers.  During the handshake, the retained INIT or COOKIE ECHO is retransmitted with exponential backoff.

ERR SctpAssociation::tick(uint64_t NowUs)
{
   if (not advance(NowUs)) return ERR::InvalidValue;
   if (live_transfer()) return transfer_tick() ? ERR::Okay : last_error;
   if (not deadline or now_us < *deadline) return ERR::Okay;

   if (retransmits++ >= config.max_retransmits) return fail(ERR::Timeout), last_error;
   if (output.size() >= MAX_OUTPUT) return fail(ERR::BufferOverflow), last_error;

   output.push_back(retry_packet);
   rto_us = rto_us > config.max_rto_us / 2 ? config.max_rto_us : rto_us * 2;
   return arm() ? ERR::Okay : last_error;
}

//********************************************************************************************************************
// Checks that the established association supports the requested extensions.

ERR SctpAssociation::require_extensions(bool ForwardTsn, bool Reconfig) const
{
   if (current_state != SctpState::ESTABLISHED) return ERR::InvalidState;
   return ((ForwardTsn and not remote.forward_tsn) or (Reconfig and not remote.reconfig)) ?
      ERR::NoSupport : ERR::Okay;
}

//********************************************************************************************************************

std::vector<std::vector<uint8_t>> SctpAssociation::take_output() { return std::exchange(output, {}); }

std::vector<SctpEvent> SctpAssociation::take_events() { return std::exchange(events, {}); }

void SctpAssociation::abort(ERR Error) { (void)fail(Error); }

//********************************************************************************************************************
// Closes the association immediately without notifying the peer, and erases the cookie secret.

void SctpAssociation::close()
{
   deadline.reset();
   retry_packet.clear();
   output.clear();
   clear_transfer();
   remote = {};
   ready = false;
   crypto_protocol::secure_wipe(secret.data(), secret.size());
   set_state(SctpState::CLOSED);
}

} // namespace rtc
