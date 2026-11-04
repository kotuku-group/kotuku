// Network adapter for the TURN client.  Each TurnSocket owns one allocation on one TURN server, using a NetServer
// for UDP or a NetSocket for TCP and TLS.

#include "turn_socket.h"
#include "../ice/network_endpoint.h"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>

namespace rtc {
namespace {

//********************************************************************************************************************

uint64_t turn_now_us()
{
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

//********************************************************************************************************************
// Builds the client configuration.  The relay family matches the family of the server address.

turn::Config options(const IceServer &Server, const turn::Identity &Id)
{
   turn::Config result;
   result.identity = Id;
   result.username = Server.username;
   result.password = Server.credential;
   result.reliable = Server.transport != ServerTransport::UDP;
   result.relay_ipv6 = Id.server.ipv6;
   result.random = [](turn::TransactionId &Id) { return RAND_bytes(Id.data(), int(Id.size())) IS 1; };
   return result;
}

} // namespace

//********************************************************************************************************************

TurnSocket::TurnSocket(const IceServer &Server, const turn::Identity &Identity) :
   server(Server), id(Identity), client(options(Server, Identity)) { }

//********************************************************************************************************************
// Releases the allocation by sending a single zero-lifetime Refresh.

void TurnSocket::retire()
{
   if (retired) return;
   retired = true;

   // Best effort; never process framework events or wait for an acknowledgement during destruction.

   client.close(turn_now_us() / 1000);
   flush(turn_now_us());
}

//********************************************************************************************************************

TurnSocket::~TurnSocket()
{
   retire();
   if (stream) FreeResource(stream);
   if (udp) FreeResource(udp);
}

//********************************************************************************************************************
// Opens the server connection.  For UDP the allocation starts immediately; stream transports start it once the
// connection has been established.  The allocation must complete within 30 seconds.

ERR TurnSocket::open(uint64_t NowUs)
{
   deadline = NowUs + 30000000;

   auto address = to_network(id.server);
   std::string text;
   if (net::AddressToStr(&address, &text) != ERR::Okay) return error = ERR::InvalidValue;

   if (server.transport IS ServerTransport::UDP) {
      udp = objNetServer::create::local(kt::FieldValue(kt::fieldhash("Address"), id.server.ipv6 ? "::" : "0.0.0.0"),
         fl::Port(0), fl::Flags(NSF::UDP), fl::Incoming(C_FUNCTION(incoming_udp, this)));
      if (not udp) return error = ERR::CreateObject;

      connected = true;
      if (not client.start(NowUs / 1000)) return error = ERR::Failed;
      flush(NowUs);
   }
   else {
      stream = objNetSocket::create::local(fl::Flags(server.transport IS ServerTransport::TLS ? NSF::SSL : NSF::NIL),
         kt::FieldValue(kt::fieldhash("PeerName"), server.host), kt::FieldValue(kt::fieldhash("MsgLimit"), 65536),
         fl::Incoming(C_FUNCTION(incoming_stream, this)), fl::Feedback(C_FUNCTION(feedback, this)));
      if (not stream) return error = ERR::CreateObject;

      auto result = stream->connect(text, id.server.port, 5.0);
      if (result != ERR::Okay and error IS ERR::Okay) error = result;
   }

   return error;
}

//********************************************************************************************************************
// Returns the number of bytes held at every stage of the output path, including the socket's own queue.

size_t TurnSocket::buffered() const
{
   int queued = 0;
   if (stream) (void)stream->getOutQueueSize(queued);
   return size_t(std::max(queued, 0)) + output_bytes + client.queued_bytes() + waiting_bytes;
}

//********************************************************************************************************************
// Moves output from the client to the socket.  Maintenance records are placed ahead of application data, but never
// ahead of a partially written record.  If no progress is made for five seconds, the connection is declared stalled.

void TurnSocket::flush(uint64_t NowUs)
{
   if (not connected or error != ERR::Okay) return;

   // Sends can record progress after the owner captured its timer timestamp.

   NowUs = std::max(NowUs, stall_since);

   turn::Packet packet;
   while (client.pop_output(packet)) {
      output_bytes += packet.wire.size();
      if (packet.maintenance) {
         auto begin = output.begin();
         if (offset and begin != output.end()) ++begin;
         auto position = std::find_if(begin, output.end(), [](const auto &Packet) { return not Packet.maintenance; });
         output.insert(position, std::move(packet));
      }
      else output.push_back(std::move(packet));
   }

   if (buffered() > turn::MAX_QUEUED) {
      error = ERR::BufferOverflow;
      return;
   }

   size_t work = 0;
   while (not output.empty() and work < 65536) {
      auto &wire = output.front().wire;
      auto bytes = std::span<const int8_t>((const int8_t *)wire.data() + offset, wire.size() - offset);
      int accepted = 0;
      ERR result;

      if (stream) {
         int queued = 0;
         if (stream->getOutQueueSize(queued) != ERR::Okay) {
            error = ERR::Disconnected;
            return;
         }

         auto available = turn::MAX_QUEUED - size_t(std::max(queued, 0));
         if (not available) break;
         bytes = bytes.first(std::min({bytes.size(), available, size_t(65536)}));

         // The generated write wrapper discards Result on BufferOverflow.  Inspect the action result directly.

         struct acWrite args{bytes};
         result = Action(AC::Write, stream, &args);
         accepted = args.Result;
      }
      else {
         auto destination = to_network(id.server);
         result = udp->sendTo(&destination, bytes, &accepted);
      }

      if (accepted < 0 or size_t(accepted) > bytes.size()) {
         error = ERR::Failed;
         return;
      }

      add_counter(counters.turn_sent_bytes, size_t(accepted));
      offset += size_t(accepted);
      output_bytes -= size_t(accepted);
      work += size_t(accepted);

      if (offset IS wire.size()) {
         output.pop_front();
         offset = 0;
      }

      if (result != ERR::Okay and result != ERR::BufferOverflow and result != ERR::LimitedSuccess) {
         error = result;
         return;
      }

      if (not accepted) break;

      if (not stream and size_t(accepted) != bytes.size()) { // Datagrams cannot be partially sent
         error = ERR::Disconnected;
         return;
      }
   }

   // Stall detection

   int queued = 0;
   if (stream) (void)stream->getOutQueueSize(queued);
   if (queued < previous_queue or work) stall_since = NowUs;
   previous_queue = queued;

   if (buffered() > 0) {
      if (not stall_since) stall_since = NowUs;
      else if (NowUs - stall_since >= 5000000) {
         timeout_reason = "TURN queue stalled";
         error = ERR::Timeout;
      }
   }
   else stall_since = 0;
}

//********************************************************************************************************************
// Installs a permission and channel binding for a new peer.  Channel numbers are allocated sequentially from 0x4000.

void TurnSocket::permit(const ice::Endpoint &Peer, uint64_t NowUs)
{
   if (not active() or Peer.ipv6 != id.server.ipv6) return;

   auto it = std::find(peers.begin(), peers.end(), Peer);
   if (it IS peers.end()) {
      if (peers.size() >= turn::MAX_CHANNELS) return;
      peers.push_back(Peer);
      (void)client.permission(Peer, NowUs / 1000);
      (void)client.bind(Peer, uint16_t(0x4000 + peers.size() - 1), NowUs / 1000);
   }
}

//********************************************************************************************************************
// Sends a datagram to Peer through the relay.  Data is held in the waiting queue until the peer's permission has
// been granted.

bool TurnSocket::send(const ice::Endpoint &Peer, std::span<const uint8_t> Data, uint64_t NowUs)
{
   if (not active() or Data.size() > 1200 or buffered() + Data.size() + 128 >
       turn::MAX_QUEUED - turn::MAINTENANCE_RESERVE) return false;

   permit(Peer, NowUs);

   if (not client.send(Peer, Data, NowUs / 1000)) {
      // A denied candidate behaves like an unreachable UDP destination.  ICE expires that pair;
      // it must not retain unsendable packets until the whole allocation is declared stalled.

      if (not client.has_permission_or_request(Peer)) return true;
      if (waiting.size() >= 128) return false;
      waiting.push_back({Peer, {Data.begin(), Data.end()}});
      waiting_bytes += Data.size() + 128;
   }

   flush(NowUs);
   return error IS ERR::Okay;
}

//********************************************************************************************************************
// Passes a record from the server to the client, and buffers any application data that it returns.

void TurnSocket::receive(std::span<const uint8_t> Wire, uint64_t NowUs)
{
   if (error != ERR::Okay) return;

   turn::Datagram data;
   auto result = client.receive(id, Wire, NowUs / 1000, data);
   bool known_peer = std::find(peers.begin(), peers.end(), data.peer) != peers.end();

   // Permit authenticated ICE to discover a new port on an already permitted IP (symmetric NAT).
   // The TURN client has checked the IP permission; the ICE agent checks credentials before learning this endpoint.

   bool ice_check = not data.payload.empty() and data.payload[0] < 4;
   if (result IS turn::Result::DATA and (known_peer or ice_check)) {
      // Small channel frames can fill a packet-count cap well before reaching the byte budget.
      if (input.size() < 1024 and input_bytes + data.payload.size() <= turn::MAX_QUEUED) {
         input_bytes += data.payload.size();
         input.push_back(std::move(data));
      }
      else error = ERR::BufferOverflow;
   }
}

//********************************************************************************************************************
// Connection state changes for stream transports.

ERR TurnSocket::feedback(objNetSocket *Socket, NTC State, APTR Context)
{
   auto self = (TurnSocket *)Context;

   if (State IS NTC::CONNECTED) {
      self->connected = true;
      if (not self->client.start(turn_now_us() / 1000)) self->error = ERR::Failed;
   }
   else if (State IS NTC::DISCONNECTED) {
      if (self->error IS ERR::Okay) {
         bool tls_failure = not self->connected and self->server.transport IS ServerTransport::TLS and
            Socket->Error IS ERR::Handshake;
         self->error = Socket->Error IS ERR::Security or tls_failure ? ERR::Security :
            (Socket->Error IS ERR::Timeout ? ERR::Timeout : ERR::Disconnected);
      }
      self->connected = false;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Receives UDP datagrams, within a budget of 32 datagrams or 64 KiB per call.  Datagrams that do not originate from
// the TURN server are discarded.  IPv4-mapped IPv6 sources are converted to IPv4 before comparison.

ERR TurnSocket::incoming_udp(objNetServer *Socket, APTR Context)
{
   auto self = (TurnSocket *)Context;
   std::array<int8_t, 8192> buffer{};
   size_t work = 0;

   for (size_t count = 0; count < 32 and work < 65536; ++count) {
      IPAddress source{};
      int received = 0;
      auto capacity = std::min(buffer.size(), size_t(65536 - work));
      if (Socket->recvFrom(&source, std::span(buffer).first(capacity), &received) != ERR::Okay or received <= 0) break;
      work += size_t(received);

      // Dual-stack sockets can report an IPv4 server through an IPv4-mapped address.

      auto expected = unmap_ipv4(self->id.server);
      expected.route = 0;
      if (from_network(source) != expected) continue;

      add_counter(self->counters.turn_received_bytes, size_t(received));
      self->receive({(const uint8_t *)buffer.data(), size_t(received)}, turn_now_us());
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Reads from a stream transport and decodes the byte stream into records, within a budget of 32 records or 64 KiB
// per call.  Undecoded input is retained for the next call.

ERR TurnSocket::incoming_stream(objNetSocket *Socket, APTR Context)
{
   auto self = (TurnSocket *)Context;
   if (self->error != ERR::Okay or not self->connected) return ERR::Okay;

   size_t frames = 0, bytes = 0, read_bytes = 0;
   while (bytes < 65536 and read_bytes < 65536 and frames < 32) {
      if (self->stream_input.empty()) {
         std::array<int8_t, 4096> buffer{};
         int received = 0;
         auto result = Socket->read(buffer, &received);
         if (result != ERR::Okay or not received) break;

         add_counter(self->counters.turn_received_bytes, size_t(received));
         read_bytes += size_t(received);
         self->stream_input.assign((const uint8_t *)buffer.data(), (const uint8_t *)buffer.data() + received);
      }

      size_t consumed = 0;
      std::vector<std::vector<uint8_t>> records;
      if (not self->decoder.feed(self->stream_input, consumed, records, 32 - frames, 65536 - bytes)) {
         self->error = ERR::InvalidData;
         break;
      }

      self->stream_input.erase(self->stream_input.begin(), self->stream_input.begin() + consumed);
      for (auto &record : records) {
         bytes += record.size();
         self->receive(record, turn_now_us());
         ++frames;
      }

      if (not consumed) break;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Drives the client and flushes output.  On failure, the sockets are disabled so that no further input is processed.

void TurnSocket::tick(uint64_t NowUs)
{
   if (error != ERR::Okay) {
      if (not disabled) {
         disabled = true;
         if (stream) (void)stream->disable();
         if (udp) (void)udp->disable();
      }
      return;
   }

   if (stream and connected) (void)incoming_stream(stream, this);

   client.tick(NowUs / 1000);
   if (client.state() IS turn::State::FAILED) {
      timeout_reason = "TURN allocation or transaction failed";
      error = client.error() IS 401 or client.error() IS 438 or client.error() IS 441 ? ERR::Security : ERR::Timeout;
      return;
   }

   if (pending() and NowUs >= deadline) {
      timeout_reason = "TURN allocation opening deadline exhausted";
      error = ERR::Timeout;
      return;
   }

   for (auto it = waiting.begin(); it != waiting.end();) {
      if (client.send(it->peer, it->payload, NowUs / 1000) or not client.has_permission_or_request(it->peer)) {
         waiting_bytes -= it->payload.size() + 128;
         it = waiting.erase(it);
      }
      else ++it;
   }

   flush(NowUs);
}

//********************************************************************************************************************

const char * TurnSocket::failure_reason() const
{
   if (error IS ERR::Security) return client.error() ? "TURN authentication failed" :
      "TURN TLS handshake or certificate/identity validation failed";
   if (error IS ERR::Timeout) return timeout_reason;
   if (error IS ERR::BufferOverflow) return "TURN resource budget exhausted";
   if (error IS ERR::InvalidData) return "TURN stream framing rejected";
   return "TURN transport disconnected or unavailable";
}

//********************************************************************************************************************
// Returns the relayed candidate.  Its route identifies the allocation that owns it.

ice::Candidate TurnSocket::candidate() const
{
   ice::Candidate result{client.relayed(), 16777215u, ice::CandidateType::RELAY};
   result.endpoint.route = id.allocation;
   return result;
}

//********************************************************************************************************************

std::vector<turn::Datagram> TurnSocket::take_input()
{
   input_bytes = 0;
   auto result = std::move(input);
   input.clear();
   return result;
}

} // namespace rtc
