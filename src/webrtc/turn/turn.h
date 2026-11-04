// TURN UDP relay protocol (RFC 8656).  Socket, object and task independent; all times are monotonic milliseconds.

#pragma once

#include "../ice/ice.h"
#include "../stun/stun.h"

#include <deque>
#include <functional>

namespace rtc::turn {

// Methods

constexpr uint16_t ALLOCATE          = 0x0003;
constexpr uint16_t REFRESH           = 0x0004;
constexpr uint16_t CREATE_PERMISSION = 0x0008;
constexpr uint16_t CHANNEL_BIND      = 0x0009;
constexpr uint16_t SEND_INDICATION   = 0x0016;
constexpr uint16_t DATA_INDICATION   = 0x0017;

// Attributes

constexpr uint16_t ATTR_CHANNEL_NUMBER      = 0x000c;
constexpr uint16_t ATTR_LIFETIME            = 0x000d;
constexpr uint16_t ATTR_XOR_PEER_ADDRESS    = 0x0012;
constexpr uint16_t ATTR_DATA                = 0x0013;
constexpr uint16_t ATTR_XOR_RELAYED_ADDRESS = 0x0016;
constexpr uint16_t ATTR_REQUESTED_FAMILY    = 0x0017;
constexpr uint16_t ATTR_REQUESTED_TRANSPORT = 0x0019;

// Resource limits

constexpr size_t MAX_PERMISSIONS = 64;
constexpr size_t MAX_CHANNELS = 64;
constexpr size_t MAX_TRANSACTIONS = 16;
constexpr size_t MAX_FRAME = 65536;
constexpr size_t MAX_QUEUED = 256 * 1024;
constexpr size_t MAINTENANCE_RESERVE = 16 * 1024;
constexpr size_t MAX_DISPATCH_FRAMES = 32;
constexpr size_t MAX_INNER_RECEIVE = 4096; // Receive capacity is independent of the local 1200-byte send budget.

using Endpoint = ice::Endpoint;
using TransactionId = std::array<uint8_t, 12>;

// Adapter-provided identity distinguishes an allocation from other connections to the same server.

struct Identity {
   Endpoint server;
   uint64_t generation = 0;
   uint64_t allocation = 0;
   bool operator==(const Identity &) const = default;
};

bool encode_xor_address(const Endpoint &Peer, const TransactionId &Id, std::vector<uint8_t> &Result);
bool decode_xor_address(const stun::Attribute &Value, const stun::Message &Message, Endpoint &Result);
bool encode_channel_data(uint16_t Channel, std::span<const uint8_t> Payload, bool Stream,
   std::vector<uint8_t> &Result);
bool decode_channel_data(std::span<const uint8_t> Wire, bool Stream, uint16_t &Channel,
   std::span<const uint8_t> &Payload);

// TURN usage allow-list; the Binding/ICE parser's required-attribute policy is unchanged.

bool valid_attributes(const stun::Message &Message);

//********************************************************************************************************************
// One incomplete record, capped at 64 KiB.  feed() consumes a prefix and emits at most 32 records/64 KiB;
// callers must reschedule the remaining input.  Invalid framing is terminal until reset().  Records own their bytes.

class StreamDecoder {
public:
   bool feed(std::span<const uint8_t> Input, size_t &Consumed, std::vector<std::vector<uint8_t>> &Records,
      size_t FrameLimit = MAX_DISPATCH_FRAMES, size_t ByteLimit = MAX_FRAME);
   void reset();
   size_t buffered() const { return buffer.size(); }

private:
   std::vector<uint8_t> buffer;
   bool failed = false;
};

//********************************************************************************************************************

enum class State { NEW, ALLOCATING, ACTIVE, FAILED, CLOSED };
enum class Result { IGNORED, ACCEPTED, DATA, FAILED };

struct Config {
   Identity identity;
   std::string username;
   std::string password;
   bool reliable = false; // TCP/TLS framing and a single 5 s request timeout.
   bool relay_ipv6 = false;
   std::function<bool(TransactionId &)> random;
};

struct Datagram { // Application data received from a peer through the relay
   Endpoint peer;
   std::vector<uint8_t> payload;
};

struct Packet { // A record to be sent to the TURN server
   Identity identity;
   bool maintenance = true; // Allocation maintenance traffic, which may use the reserved queue capacity
   std::vector<uint8_t> wire;
};

//********************************************************************************************************************
// Single-owner state machine.  No callbacks except injected entropy; no system clock or socket access.
// Output is admitted atomically to a bounded queue.  The adapter takes responsibility for its own queue budget
// and partial-write handling after pop_output(), and must deliver records with the original Identity.
// Ingress data must additionally pass the ICE route/peer checks before application dispatch.

class Client {
public:
   explicit Client(Config Options);
   ~Client();
   Client(const Client &) = delete;
   Client & operator=(const Client &) = delete;

   bool start(uint64_t Now);
   bool permission(const Endpoint &Peer, uint64_t Now);
   bool bind(const Endpoint &Peer, uint16_t Channel, uint64_t Now);
   bool send(const Endpoint &Peer, std::span<const uint8_t> Payload, uint64_t Now);

   // Granted IP permission or an outstanding permission/channel request that can establish it.

   bool has_permission_or_request(const Endpoint &Peer) const;
   Result receive(const Identity &Source, std::span<const uint8_t> Wire, uint64_t Now, Datagram &Data);
   void tick(uint64_t Now);

   // Local closure is immediate.  Only a best-effort authenticated deletion remains queued, with a 1 s deadline.

   void close(uint64_t Now);
   bool pop_output(Packet &Output);

   State state() const { return status; }
   const Endpoint &relayed() const { return relay; }
   const Endpoint &mapped() const { return reflexive; }
   uint16_t error() const { return last_error; } // TURN code; 0 denotes local timeout/resource/entropy failure.
   size_t pending() const { return transactions.size(); }
   size_t queued_bytes() const { return output_bytes; }
   size_t permission_count() const { return permissions.size(); }
   size_t channel_count() const { return channels.size(); }

private:
   struct Permission {
      Endpoint peer;
      uint64_t expires = 0, renew = 0;
   };

   struct Channel {
      Endpoint peer;
      uint16_t number = 0;
      uint64_t expires = 0, renew = 0, reusable = 0; // A channel number cannot be rebound until reusable
   };

   struct Transaction { // An outstanding request, retained for retransmission and re-authentication
      uint16_t method = 0, channel = 0;
      Endpoint peer;
      TransactionId id{};
      stun::LongTermChallenge challenge;
      std::vector<uint8_t> wire;
      uint64_t started = 0, next = 0, deadline = 0;
      unsigned transmissions = 0, challenges = 0;
      bool authenticated = false;
   };

   Config config;
   State status = State::NEW;
   stun::LongTermChallenge challenge;
   Endpoint relay, reflexive;
   uint64_t now = 0, expires = 0, renew = 0, close_deadline = 0;
   uint16_t last_error = 0;
   std::vector<Permission> permissions;
   std::vector<Channel> channels;
   std::vector<Transaction> transactions;
   std::deque<Packet> output;
   size_t output_bytes = 0;

   // Reject recently reused IDs and pending collisions.  Adapters must supply cryptographically secure entropy.

   std::deque<TransactionId> recent_ids;

   bool new_id(TransactionId &Id);
   bool queue(std::span<const uint8_t> Wire, bool Maintenance);
   bool build(Transaction &Operation, uint64_t Now);
   bool request(uint16_t Method, const Endpoint &Peer, uint16_t Channel, uint64_t Now);
   bool has_request(uint16_t Method, const Endpoint &Peer = {}) const;
   bool permitted(const Endpoint &Peer, uint64_t Now) const;
   void fail(uint16_t Error = 0);
   void discard_output(const TransactionId &Id);
   void operation_failed(const Transaction &Operation, uint16_t Error);
};

} // namespace rtc::turn
