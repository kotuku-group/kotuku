// TURN client state machine (RFC 8656) for UDP relaying.  The client allocates a relayed address, maintains
// permissions and channel bindings, and frames application data as Send indications or ChannelData.  All requests
// after the first challenge are authenticated with long-term credentials.

#include "turn.h"
#include "../precis/opaque_string.h"

#include <algorithm>

#include <openssl/crypto.h>

namespace rtc::turn {
namespace {

// Big-endian field accessors

uint16_t read16(const uint8_t *Bytes) { return (uint16_t(Bytes[0]) << 8) | Bytes[1]; }
uint32_t read32(const uint8_t *Bytes) { return (uint32_t(read16(Bytes)) << 16) | read16(Bytes + 2); }

void write16(uint8_t *Bytes, uint16_t Value)
{
   Bytes[0] = uint8_t(Value >> 8);
   Bytes[1] = uint8_t(Value);
}

void write32(uint8_t *Bytes, uint32_t Value)
{
   write16(Bytes, uint16_t(Value >> 16));
   write16(Bytes + 2, uint16_t(Value));
}

// Permissions are granted per IP address, regardless of port (RFC 8656 §9).

bool same_ip(const Endpoint &A, const Endpoint &B) { return A.ipv6 IS B.ipv6 and A.address IS B.address; }

bool channel_number(uint16_t Number) { return Number >= 0x4000 and Number <= 0x4fff; }

//********************************************************************************************************************
// Returns true for the attribute types defined by TURN.

bool turn_attribute(uint16_t Type)
{
   switch (Type) {
      case ATTR_CHANNEL_NUMBER: case ATTR_LIFETIME: case ATTR_XOR_PEER_ADDRESS: case ATTR_DATA:
      case ATTR_XOR_RELAYED_ADDRESS: case ATTR_REQUESTED_FAMILY: case ATTR_REQUESTED_TRANSPORT: return true;
      default: return false;
   }
}

//********************************************************************************************************************
// Decodes a TURN error response as a long-term credential challenge.  The STUN decoder only accepts Binding error
// responses with STUN attributes, so the message type is substituted and TURN attributes are removed first.

bool decode_challenge(const stun::Message &Message, stun::LongTermChallenge &Result)
{
   auto auth = Message;
   auth.type = stun::BINDING_ERROR;
   std::erase_if(auth.attributes, [](const auto &Attribute) { return turn_attribute(Attribute.type); });
   return stun::decode_long_term_challenge(auth, Result);
}

//********************************************************************************************************************
// Returns true if two challenges differ by no more than their nonce.

bool same_challenge_policy(const stun::LongTermChallenge &A, const stun::LongTermChallenge &B)
{
   return A.realm IS B.realm and A.password_algorithm IS B.password_algorithm and
      A.password_algorithms IS B.password_algorithms and A.sha256_integrity IS B.sha256_integrity and
      A.userhash IS B.userhash;
}

} // namespace

//********************************************************************************************************************
// Validates the attributes of a TURN message.  Repeated attributes are rejected, and every comprehension-required
// attribute must be known to STUN or TURN.

bool valid_attributes(const stun::Message &Message)
{
   // Do not inherit the ICE usage's PRIORITY/USE-CANDIDATE allow-list.

   if (Message.attributes.size() > 128) return false;

   for (const auto &attribute : Message.attributes) {
      if (attribute.type IS 0x0024 or attribute.type IS 0x0025) return false;
      if (std::count_if(Message.attributes.begin(), Message.attributes.end(),
          [&](const auto &Entry) { return Entry.type IS attribute.type; }) > 1) return false;
   }

   auto unknown = stun::unknown_required_attributes(Message);
   return std::all_of(unknown.begin(), unknown.end(), [](uint16_t Type) { return turn_attribute(Type); });
}

//********************************************************************************************************************
// Encodes an XOR-PEER-ADDRESS value.  The address is masked with the magic cookie and transaction ID.

bool encode_xor_address(const Endpoint &Peer, const TransactionId &Id, std::vector<uint8_t> &Result)
{
   Result.clear();
   if (not Peer.port) return false;

   auto encoded = stun::encode_xor_address(Peer.address, Peer.port, Peer.ipv6, Id);
   Result.assign(encoded.begin(), encoded.begin() + (Peer.ipv6 ? 20 : 8));
   return true;
}

//********************************************************************************************************************
// Decodes an XOR-PEER-ADDRESS, XOR-RELAYED-ADDRESS or XOR-MAPPED-ADDRESS attribute, which share the same format.

bool decode_xor_address(const stun::Attribute &Value, const stun::Message &Message, Endpoint &Result)
{
   if (Value.type != ATTR_XOR_PEER_ADDRESS and Value.type != ATTR_XOR_RELAYED_ADDRESS and
       Value.type != stun::ATTR_XOR_MAPPED_ADDRESS) return false;

   auto address = Value;
   address.type = stun::ATTR_XOR_MAPPED_ADDRESS;

   Endpoint decoded;
   if (not stun::decode_xor_address(address, Message, decoded.address, decoded.port, decoded.ipv6) or
       not decoded.port) return false;

   Result = decoded;
   return true;
}

//********************************************************************************************************************
// Encodes a ChannelData message.  Over stream transports, the message is padded to a multiple of four bytes.

bool encode_channel_data(uint16_t Channel, std::span<const uint8_t> Payload, bool Stream,
   std::vector<uint8_t> &Result)
{
   Result.clear();
   if (not channel_number(Channel) or Payload.size() > 65535) return false;

   size_t length = 4 + Payload.size();
   if (Stream) length = (length + 3) & ~size_t(3);
   if (length > MAX_FRAME) return false;

   Result.resize(length);
   write16(Result.data(), Channel);
   write16(Result.data() + 2, uint16_t(Payload.size()));
   std::copy(Payload.begin(), Payload.end(), Result.begin() + 4);
   return true;
}

//********************************************************************************************************************
// Decodes a ChannelData message.  Padding is mandatory over stream transports and optional over UDP.

bool decode_channel_data(std::span<const uint8_t> Wire, bool Stream, uint16_t &Channel,
   std::span<const uint8_t> &Payload)
{
   Payload = {};
   if (Wire.size() < 4 or Wire.size() > MAX_FRAME or not channel_number(read16(Wire.data()))) return false;

   size_t length = read16(Wire.data() + 2);
   size_t padded = (length + 3) & ~size_t(3);
   if (Stream ? Wire.size() != 4 + padded : (Wire.size() != 4 + length and Wire.size() != 4 + padded)) {
      return false;
   }

   Channel = read16(Wire.data());
   Payload = Wire.subspan(4, length);
   return true;
}

//********************************************************************************************************************

void StreamDecoder::reset()
{
   buffer.clear();
   failed = false;
}

//********************************************************************************************************************
// Splits a TCP or TLS byte stream into STUN and ChannelData records.  The record length is determined from its
// four-byte header, and the record is buffered until complete.

bool StreamDecoder::feed(std::span<const uint8_t> Input, size_t &Consumed,
   std::vector<std::vector<uint8_t>> &Records, size_t FrameLimit, size_t ByteLimit)
{
   Consumed = 0;
   Records.clear();
   if (failed) return false;

   size_t emitted = 0;
   FrameLimit = std::min(FrameLimit, MAX_DISPATCH_FRAMES);
   ByteLimit = std::min(ByteLimit, MAX_FRAME);

   while (Records.size() < FrameLimit) {
      size_t required = 4;
      if (buffer.size() >= 4) {
         auto type = read16(buffer.data());
         if (channel_number(type)) required = 4 + ((size_t(read16(buffer.data() + 2)) + 3) & ~size_t(3));
         else if ((type & 0xc000) IS 0) required = 20 + read16(buffer.data() + 2);
         else {
            failed = true;
            return false;
         }

         if (required > MAX_FRAME or ((type & 0xc000) IS 0 and (required & 3))) {
            failed = true;
            return false;
         }
      }

      // Do not copy an entire second large record after exhausting this dispatch's byte budget.

      if (emitted + required > ByteLimit) break;

      if (buffer.size() < required) {
         size_t count = std::min(required - buffer.size(), Input.size() - Consumed);
         buffer.insert(buffer.end(), Input.begin() + Consumed, Input.begin() + Consumed + count);
         Consumed += count;
         if (buffer.size() < required) break;

         // A newly completed header must be examined before deciding whether the record is complete.

         if (required IS 4 and not channel_number(read16(buffer.data()))) continue;
         if (required IS 4 and read16(buffer.data() + 2)) continue;
      }

      if (emitted + buffer.size() > ByteLimit) break;

      if ((buffer[0] & 0xc0) IS 0) {
         stun::Message message;
         if (not stun::parse(buffer, message)) {
            failed = true;
            return false;
         }
      }

      emitted += buffer.size();
      Records.push_back(std::move(buffer));
      buffer.clear();
      if (Consumed IS Input.size()) break;
   }

   return true;
}

//********************************************************************************************************************

Client::Client(Config Options) : config(std::move(Options)) { }

Client::~Client() { OPENSSL_cleanse(config.password.data(), config.password.size()); }

//********************************************************************************************************************
// Generates a transaction ID that is not in use and has not been used recently.

bool Client::new_id(TransactionId &Id)
{
   if (not config.random or not config.random(Id) or
       std::find(recent_ids.begin(), recent_ids.end(), Id) != recent_ids.end() or
       std::any_of(transactions.begin(), transactions.end(), [&](const auto &Op) { return Op.id IS Id; })) {
      return false;
   }

   recent_ids.push_back(Id);
   if (recent_ids.size() > 128) recent_ids.pop_front();
   return true;
}

//********************************************************************************************************************
// Queues an outbound record.  Maintenance traffic takes priority over application data, and part of the queue is
// reserved for it so that the allocation can always be refreshed.

bool Client::queue(std::span<const uint8_t> Wire, bool Maintenance)
{
   auto limit = Maintenance ? MAX_QUEUED : MAX_QUEUED - MAINTENANCE_RESERVE;

   // A packet-count cap also bounds zero-length application traffic and deque overhead.

   if (output.size() >= (Maintenance ? 1024u : 992u) or Wire.size() > limit - std::min(output_bytes, limit)) {
      return false;
   }

   Packet packet{config.identity, Maintenance, {Wire.begin(), Wire.end()}};
   if (Maintenance) {
      auto position = std::find_if(output.begin(), output.end(), [](const auto &Value) {
         return not Value.maintenance;
      });
      output.insert(position, std::move(packet));
   }
   else output.push_back(std::move(packet));

   output_bytes += Wire.size();
   return true;
}

//********************************************************************************************************************

bool Client::pop_output(Packet &Output)
{
   if (output.empty()) return false;
   output_bytes -= output.front().wire.size();
   Output = std::move(output.front());
   output.pop_front();
   return true;
}

//********************************************************************************************************************
// Removes any queued transmissions of a transaction that has completed or been superseded.

void Client::discard_output(const TransactionId &Id)
{
   std::erase_if(output, [&](const auto &Packet) {
      if (not Packet.maintenance or Packet.wire.size() < 20 or
          not std::equal(Id.begin(), Id.end(), Packet.wire.begin() + 8)) return false;
      output_bytes -= Packet.wire.size();
      return true;
   });
}

//********************************************************************************************************************
// Encodes and queues a request with a fresh transaction ID.  The request is authenticated once a challenge has been
// received.  A channel number cannot be reused for 15 minutes after a ChannelBind is sent (RFC 8656 §12).

bool Client::build(Transaction &Operation, uint64_t Now)
{
   if (not new_id(Operation.id)) return false;

   std::array<uint8_t, 4> lifetime{}, transport{17, 0, 0, 0}, family{};
   std::array<uint8_t, 4> channel{};
   std::vector<uint8_t> peer;
   std::vector<stun::Attribute> attributes;

   if (Operation.method IS ALLOCATE or Operation.method IS REFRESH) {
      write32(lifetime.data(), status IS State::CLOSED ? 0 : 600);
      attributes.push_back({ATTR_LIFETIME, lifetime});
   }

   if (Operation.method IS ALLOCATE) {
      attributes.push_back({ATTR_REQUESTED_TRANSPORT, transport});

      // Explicitly request one relay family, independently of the client-to-server connection family.

      family[0] = config.relay_ipv6 ? 2 : 1;
      attributes.push_back({ATTR_REQUESTED_FAMILY, family});
   }

   if (Operation.method IS CREATE_PERMISSION or Operation.method IS CHANNEL_BIND) {
      if (not encode_xor_address(Operation.peer, Operation.id, peer)) return false;
      attributes.push_back({ATTR_XOR_PEER_ADDRESS, peer});
   }

   if (Operation.method IS CHANNEL_BIND) {
      write16(channel.data(), Operation.channel);
      attributes.push_back({ATTR_CHANNEL_NUMBER, channel});
   }

   Operation.authenticated = not Operation.challenge.nonce.empty();
   bool encoded = Operation.authenticated ?
      stun::encode_long_term_request(Operation.method, Operation.id, attributes, Operation.challenge,
         config.username, config.password, Operation.wire) :
      stun::encode(Operation.method, Operation.id, attributes, {}, false, true, Operation.wire);
   if (not encoded or not queue(Operation.wire, true)) return false;

   if (Operation.method IS CHANNEL_BIND) {
      for (auto &binding : channels) if (binding.peer IS Operation.peer) {
         binding.reusable = std::max(binding.reusable, Now + 900000);
      }
   }

   Operation.transmissions = 1;
   Operation.next = Now + 500;
   return true;
}

//********************************************************************************************************************
// Returns true if a request of the given method is outstanding for Peer.

bool Client::has_request(uint16_t Method, const Endpoint &Peer) const
{
   return std::any_of(transactions.begin(), transactions.end(), [&](const auto &Op) {
      return Op.method IS Method and (Method IS ALLOCATE or Method IS REFRESH or
         (Method IS CREATE_PERMISSION ? same_ip(Op.peer, Peer) : Op.peer IS Peer));
   });
}

//********************************************************************************************************************
// Starts a new transaction.  Two transaction slots are reserved for allocation maintenance.

bool Client::request(uint16_t Method, const Endpoint &Peer, uint16_t Channel, uint64_t Now)
{
   size_t limit = (Method IS ALLOCATE or Method IS REFRESH) ? MAX_TRANSACTIONS : MAX_TRANSACTIONS - 2;
   if (transactions.size() >= limit or has_request(Method, Peer)) return false;

   Transaction operation;
   operation.method = Method;
   operation.peer = Peer;
   operation.channel = Channel;
   operation.challenge = challenge;
   operation.started = Now;
   operation.deadline = Now + (config.reliable ? 5000 : (Method IS ALLOCATE ? 30000 : 39500));
   if (not build(operation, Now)) return false;

   transactions.push_back(std::move(operation));
   return true;
}

//********************************************************************************************************************
// Prepares the credentials and sends the initial Allocate request.

bool Client::start(uint64_t Now)
{
   if (status != State::NEW) return false;

   std::string username, password;
   if (config.username.empty() or config.username.size() > 512 or config.password.empty() or
       config.password.size() > 1024 or not config.identity.server.port or
       not precis::prepare_opaque_string(config.username, username) or username.size() >= 509 or
       not precis::prepare_opaque_string(config.password, password)) {
      fail();
      return false;
   }

   config.username = std::move(username);
   OPENSSL_cleanse(config.password.data(), config.password.size());
   config.password = std::move(password);

   now = Now;
   status = State::ALLOCATING;
   if (not request(ALLOCATE, {}, 0, Now)) {
      fail();
      return false;
   }

   return true;
}

//********************************************************************************************************************
// Requests a permission for Peer's IP address, unless one is already active or pending.

bool Client::permission(const Endpoint &Peer, uint64_t Now)
{
   tick(Now);
   if (status != State::ACTIVE or not Peer.port or Peer.ipv6 != relay.ipv6) return false;

   auto entry = std::find_if(permissions.begin(), permissions.end(),
      [&](const auto &Value) { return same_ip(Value.peer, Peer); });
   if (entry != permissions.end() and (entry->expires > now or has_request(CREATE_PERMISSION, Peer))) return true;
   if (entry IS permissions.end() and permissions.size() >= MAX_PERMISSIONS) return false;

   if (not request(CREATE_PERMISSION, Peer, 0, now)) return false;
   if (entry IS permissions.end()) permissions.push_back({Peer});
   return true;
}

//********************************************************************************************************************
// Binds a channel number to Peer, which also installs a permission for Peer's IP address.  A channel number and peer
// are permanently paired for the lifetime of the binding.

bool Client::bind(const Endpoint &Peer, uint16_t Channel, uint64_t Now)
{
   tick(Now);
   if (status != State::ACTIVE or not channel_number(Channel) or not Peer.port or Peer.ipv6 != relay.ipv6) {
      return false;
   }

   auto binding = channels.end();
   for (auto it = channels.begin(); it != channels.end(); ++it) {
      if (it->number IS Channel or it->peer IS Peer) {
         if (it->number != Channel or it->peer != Peer) return false;
         binding = it;
      }
   }

   if (has_request(CHANNEL_BIND, Peer)) return true;
   if (binding IS channels.end() and channels.size() >= MAX_CHANNELS) return false;

   auto permission = std::find_if(permissions.begin(), permissions.end(),
      [&](const auto &Value) { return same_ip(Value.peer, Peer); });
   if (permission IS permissions.end() and permissions.size() >= MAX_PERMISSIONS) return false;

   if (not request(CHANNEL_BIND, Peer, Channel, now)) return false;

   if (binding IS channels.end()) channels.push_back({Peer, Channel, 0, 0, now + 900000});
   else binding->reusable = std::max(binding->reusable, now + 900000);
   if (permission IS permissions.end()) permissions.push_back({Peer});
   return true;
}

//********************************************************************************************************************

bool Client::permitted(const Endpoint &Peer, uint64_t Now) const
{
   return std::any_of(permissions.begin(), permissions.end(), [&](const auto &Value) {
      return same_ip(Value.peer, Peer) and Value.expires > Now;
   });
}

//********************************************************************************************************************
// Sends application data to Peer through the relay.  ChannelData is used if a channel is bound, otherwise the data
// is sent in a Send indication.

bool Client::send(const Endpoint &Peer, std::span<const uint8_t> Payload, uint64_t Now)
{
   tick(Now);
   if (status != State::ACTIVE or not Peer.port or Payload.size() > 1200 or not permitted(Peer, now)) return false;

   std::vector<uint8_t> wire;
   auto binding = std::find_if(channels.begin(), channels.end(), [&](const auto &Value) {
      return Value.peer IS Peer and Value.expires > now;
   });

   if (binding != channels.end()) {
      if (not encode_channel_data(binding->number, Payload, config.reliable, wire)) return false;
   }
   else {
      TransactionId id;
      std::vector<uint8_t> address;
      if (not new_id(id) or not encode_xor_address(Peer, id, address)) return false;
      std::array<stun::Attribute, 2> attributes{{{ATTR_XOR_PEER_ADDRESS, address}, {ATTR_DATA, Payload}}};
      if (not stun::encode(SEND_INDICATION, id, attributes, {}, false, true, wire)) return false;
   }

   return queue(wire, false);
}

//********************************************************************************************************************

bool Client::has_permission_or_request(const Endpoint &Peer) const
{
   return permitted(Peer, now) or std::any_of(transactions.begin(), transactions.end(), [&](const auto &Operation) {
      return (Operation.method IS CREATE_PERMISSION or Operation.method IS CHANNEL_BIND) and
         same_ip(Operation.peer, Peer);
   });
}

//********************************************************************************************************************
// Terminates the allocation locally and discards all state.

void Client::fail(uint16_t Error)
{
   status = State::FAILED;
   last_error = Error;
   transactions.clear();
   permissions.clear();
   channels.clear();
   output.clear();
   output_bytes = 0;
   expires = renew = 0;
}

//********************************************************************************************************************
// Handles a transaction that failed or timed out.  Failure of an allocation request, or a 401 or 437 error, is fatal.
// Otherwise the affected permission or channel is scheduled for immediate renewal.

void Client::operation_failed(const Transaction &Operation, uint16_t Error)
{
   last_error = Error;

   if (Operation.method IS ALLOCATE or Operation.method IS REFRESH or Error IS 437 or Error IS 401) {
      fail(Error);
      return;
   }

   if (Operation.method IS CREATE_PERMISSION) {
      for (auto &entry : permissions) if (same_ip(entry.peer, Operation.peer)) entry.renew = entry.expires;
   }
   else if (Operation.method IS CHANNEL_BIND) {
      for (auto &entry : channels) if (entry.peer IS Operation.peer) {
         entry.renew = entry.expires;

         // With no acknowledgement the server may have applied a late request just before timeout.

         entry.reusable = std::max(entry.reusable, now + 900000);
      }
   }
}

//********************************************************************************************************************
// Processes timers: transaction retransmission and timeout, allocation expiry and refresh, and renewal of permissions
// and channel bindings.  UDP requests are retransmitted seven times with exponential backoff (RFC 8489 §6.2.1).

void Client::tick(uint64_t Now)
{
   now = std::max(now, Now);

   if (status IS State::CLOSED) {
      if (now >= close_deadline) {
         output.clear();
         output_bytes = 0;
      }
      return;
   }

   if (status IS State::FAILED or status IS State::NEW) return;

   if (status IS State::ACTIVE and now >= expires) {
      fail();
      return;
   }

   for (size_t i = 0; i < transactions.size();) {
      auto &operation = transactions[i];
      if (now >= operation.deadline) {
         auto expired = std::move(operation);
         transactions.erase(transactions.begin() + i);
         discard_output(expired.id);
         operation_failed(expired, 0);
         if (status IS State::FAILED) return;
         continue;
      }

      if (not config.reliable and now >= operation.next and operation.transmissions < 7) {
         if (queue(operation.wire, true)) {
            if (operation.method IS CHANNEL_BIND) {
               for (auto &binding : channels) if (binding.peer IS operation.peer) {
                  binding.reusable = std::max(binding.reusable, now + 900000);
               }
            }
            ++operation.transmissions;
            operation.next = now + (operation.transmissions IS 7 ? 8000 : (500u << (operation.transmissions - 1)));
         }
      }

      ++i;
   }

   if (status != State::ACTIVE) return;

   std::erase_if(channels, [&](const auto &Value) {
      return now >= Value.reusable and not has_request(CHANNEL_BIND, Value.peer);
   });

   std::erase_if(permissions, [&](const auto &Value) {
      return now >= Value.expires and not has_request(CREATE_PERMISSION, Value.peer) and
         not has_request(CHANNEL_BIND, Value.peer);
   });

   if (now >= renew and not has_request(REFRESH) and not request(REFRESH, {}, 0, now)) {
      fail();
      return;
   }

   // Copy endpoints before request() changes the transaction vector; permission/channel storage remains stable.

   for (const auto &entry : channels) {
      if (entry.expires > now and now >= entry.renew and not has_request(CHANNEL_BIND, entry.peer)) {
         (void)request(CHANNEL_BIND, entry.peer, entry.number, now);
      }
   }

   for (const auto &entry : permissions) {
      if (entry.expires > now and now >= entry.renew and not has_request(CREATE_PERMISSION, entry.peer) and
          not has_request(CHANNEL_BIND, entry.peer)) (void)request(CREATE_PERMISSION, entry.peer, 0, now);
   }
}

//********************************************************************************************************************
// Closes the client.  An active allocation is deleted with a zero-lifetime Refresh, which is queued once and never
// retransmitted.

void Client::close(uint64_t Now)
{
   tick(Now);

   bool allocated = status IS State::ACTIVE;
   transactions.clear();
   permissions.clear();
   channels.clear();
   output.clear();
   output_bytes = 0;
   status = State::CLOSED;
   expires = renew = 0;
   close_deadline = now + 1000;

   if (allocated and not challenge.nonce.empty()) (void)request(REFRESH, {}, 0, now);
   transactions.clear(); // Teardown never blocks local cleanup or waits for a response.
}

//********************************************************************************************************************
// Processes a record received from the TURN server.  Application data from permitted peers is returned in Data with
// a result of DATA.  Responses are matched to their transaction and authenticated; unauthenticated responses can
// only deliver a new challenge, and only before the transaction has been authenticated.

Result Client::receive(const Identity &Source, std::span<const uint8_t> Wire, uint64_t Now, Datagram &Data)
{
   Data = {};
   if (Source != config.identity) return Result::IGNORED;

   tick(Now);
   if (status != State::ACTIVE and status != State::ALLOCATING) return Result::IGNORED;

   if (Wire.size() >= 4 and channel_number(read16(Wire.data()))) { // ChannelData
      uint16_t channel = 0;
      std::span<const uint8_t> payload;
      if (not decode_channel_data(Wire, config.reliable, channel, payload) or payload.size() > MAX_INNER_RECEIVE) {
         return Result::IGNORED;
      }

      auto binding = std::find_if(channels.begin(), channels.end(), [&](const auto &Value) {
         return Value.number IS channel and Value.expires > now;
      });
      if (binding IS channels.end() or not permitted(binding->peer, now)) return Result::IGNORED;

      Data = {binding->peer, {payload.begin(), payload.end()}};
      return Result::DATA;
   }

   stun::Message message;
   if (not stun::parse(Wire, message) or not valid_attributes(message) or
       (message.find(stun::ATTR_FINGERPRINT) and not stun::verify_fingerprint(Wire, message))) {
      return Result::IGNORED;
   }

   if (message.type IS DATA_INDICATION) {
      auto address = message.find(ATTR_XOR_PEER_ADDRESS);
      auto payload = message.find(ATTR_DATA);
      Endpoint peer;
      if (status != State::ACTIVE or not address or not payload or payload->value.size() > MAX_INNER_RECEIVE or
          message.find(stun::ATTR_MESSAGE_INTEGRITY) or message.find(stun::ATTR_MESSAGE_INTEGRITY_SHA256) or
          not decode_xor_address(*address, message, peer) or not permitted(peer, now)) return Result::IGNORED;

      Data = {peer, {payload->value.begin(), payload->value.end()}};
      return Result::DATA;
   }

   // Match the response to its transaction

   auto found = std::find_if(transactions.begin(), transactions.end(),
      [&](const auto &Op) { return Op.id IS message.transaction_id; });
   if (found IS transactions.end() or
       (message.type != (found->method | 0x0100) and message.type != (found->method | 0x0110))) {
      return Result::IGNORED;
   }

   bool success = message.type IS (found->method | 0x0100);
   const auto *integrity = message.find(found->challenge.sha256_integrity ?
      stun::ATTR_MESSAGE_INTEGRITY_SHA256 : stun::ATTR_MESSAGE_INTEGRITY);
   if (message.find(found->challenge.sha256_integrity ? stun::ATTR_MESSAGE_INTEGRITY :
       stun::ATTR_MESSAGE_INTEGRITY_SHA256) or message.find(stun::ATTR_USERNAME) or message.find(stun::ATTR_USERHASH)) {
      return Result::IGNORED;
   }

   if (integrity) {
      std::vector<uint8_t> key;
      bool verified = found->authenticated and stun::long_term_key(config.username, found->challenge.realm,
         config.password, key, found->challenge.password_algorithm IS stun::PASSWORD_SHA256) and
         stun::verify_integrity(Wire, message, key, found->challenge.sha256_integrity);
      OPENSSL_cleanse(key.data(), key.size());
      if (not verified) return Result::IGNORED;

      for (const auto &attribute : message.attributes) {
         if (attribute.offset > integrity->offset and attribute.type != stun::ATTR_FINGERPRINT) {
            return Result::IGNORED;
         }
      }
   }

   uint16_t error = 0;
   if (not success) {
      auto attribute = message.find(stun::ATTR_ERROR_CODE);
      if (not attribute or not stun::decode_error_code(*attribute, error)) return Result::IGNORED;
   }

   if (not success and (error IS 401 or error IS 438)) {
      stun::LongTermChallenge next;
      if (not decode_challenge(message, next)) return Result::IGNORED;

      // The only unauthenticated transition after the first challenge is a nonce-only 438 update.

      if (found->authenticated and not integrity and not same_challenge_policy(next, found->challenge)) {
         return Result::IGNORED;
      }

      if ((not found->authenticated and error != 401) or
          (found->authenticated and (error != 438 or next.nonce IS found->challenge.nonce)) or
          found->challenges >= 2) {
         auto rejected = *found;
         discard_output(found->id);
         transactions.erase(found);
         operation_failed(rejected, error);
         return Result::FAILED;
      }

      auto retry = *found;
      retry.challenge = next;
      ++retry.challenges;
      if (not build(retry, now)) {
         fail();
         return Result::FAILED;
      }

      discard_output(found->id);
      *found = std::move(retry);
      challenge = std::move(next);
      return Result::ACCEPTED;
   }

   // Unauthenticated non-challenge errors are useful only before authentication; never invalidate a live route.

   if (not integrity and (success or found->authenticated)) return Result::IGNORED;

   if (not success) {
      auto rejected = *found;
      discard_output(found->id);
      transactions.erase(found);
      operation_failed(rejected, error);
      return Result::FAILED;
   }

   // Success response

   Endpoint relayed, mapped;
   uint32_t lifetime = 0;

   if (found->method IS ALLOCATE or found->method IS REFRESH) {
      auto value = message.find(ATTR_LIFETIME);
      if (not value or value->value.size() != 4) return Result::IGNORED;
      lifetime = read32(value->value.data());
      if (not lifetime) {
         fail();
         return Result::FAILED;
      }
   }

   if (found->method IS ALLOCATE) {
      auto address = message.find(ATTR_XOR_RELAYED_ADDRESS);
      auto reflexive = message.find(stun::ATTR_XOR_MAPPED_ADDRESS);
      if (not address or not reflexive or not decode_xor_address(*address, message, relayed) or
          not decode_xor_address(*reflexive, message, mapped) or relayed.ipv6 != config.relay_ipv6) {
         return Result::IGNORED;
      }
   }

   auto completed = *found;
   discard_output(found->id);
   transactions.erase(found);

   if (completed.method IS ALLOCATE or completed.method IS REFRESH) {
      // Refresh shortly before expiry, measured from when the request was first sent.

      uint64_t duration = uint64_t(lifetime) * 1000;
      expires = completed.started + duration;
      renew = completed.started + std::min(duration * 4 / 5, duration > 5000 ? duration - 5000 : duration / 2);
      if (expires <= now) {
         fail();
         return Result::FAILED;
      }

      if (completed.method IS ALLOCATE) {
         relay = relayed;
         reflexive = mapped;
         status = State::ACTIVE;
      }
   }
   else { // Permissions last five minutes, and channel bindings ten minutes
      auto entry = std::find_if(permissions.begin(), permissions.end(),
         [&](const auto &Value) { return same_ip(Value.peer, completed.peer); });
      if (entry != permissions.end()) {
         entry->expires = std::max(entry->expires, completed.started + 300000);
         entry->renew = entry->expires - 60000;
      }

      if (completed.method IS CHANNEL_BIND) {
         for (auto &binding : channels) if (binding.peer IS completed.peer and binding.number IS completed.channel) {
            binding.expires = completed.started + 600000;
            binding.renew = binding.expires - 120000;
            binding.reusable = std::max(binding.reusable, now + 900000);
         }
      }
   }

   return Result::ACCEPTED;
}

} // namespace rtc::turn
