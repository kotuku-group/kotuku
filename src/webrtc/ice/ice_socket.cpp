// Network adapters for the ICE/DTLS transports.  IceSocket serves ICE-lite on a single NetServer, while FullIceSocket
// manages host sockets, server-reflexive discovery and TURN relays for full ICE, and accounts for traffic per route.

#include "ice_socket.h"
#include "network_endpoint.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace rtc {

//********************************************************************************************************************
// IceSocket: ICE-lite and DTLS on a single UDP socket.

IceSocket::IceSocket(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
   std::string_view LocalPassword, std::string_view RemoteUfrag, IceDtlsCallbacks Callbacks) :
   transport(std::move(Certificate), LocalUfrag, LocalPassword, RemoteUfrag,
      IceDtlsCallbacks{
         [this](std::span<const uint8_t> Datagram, const ice::Endpoint &Destination) {
            send(Datagram, Destination);
         },
         std::move(Callbacks.receive), std::move(Callbacks.state)
      }) { }

IceSocket::~IceSocket()
{
   if (socket) FreeResource(socket);
}

//********************************************************************************************************************
// Binds the UDP socket.  Large socket buffers absorb bursts of SCTP traffic.

ERR IceSocket::open(std::string_view Address, int Port)
{
   if (socket or Port < 0 or Port > 65535) return ERR::Args;

   socket = objNetServer::create::local(
      kt::FieldValue(kt::fieldhash("Address"), Address),
      fl::Port(Port), fl::Flags(NSF::UDP), fl::Incoming(C_FUNCTION(incoming, this)));
   if (not socket) return ERR::Failed;

   auto error = socket->setBufferSizes(1 << 20, 1 << 20);
   if (error != ERR::Okay) {
      FreeResource(socket);
      socket = nullptr;
   }
   return error;
}

//********************************************************************************************************************

ERR IceSocket::set_remote_fingerprint(std::string_view Fingerprint)
{
   return transport.set_remote_fingerprint(Fingerprint);
}

//********************************************************************************************************************
// Sends a datagram.  Failures are recorded in last_socket_error for the owner to collect.

void IceSocket::send(std::span<const uint8_t> Datagram, const ice::Endpoint &Destination)
{
   if (not socket) {
      last_socket_error = ERR::Failed;
      return;
   }

   auto address = to_network(Destination);
   auto bytes = std::span<const int8_t>((const int8_t *)Datagram.data(), Datagram.size());
   int sent = 0;
   auto error = socket->sendTo(&address, bytes, &sent);
   if (error != ERR::Okay or sent != int(Datagram.size())) {
      last_socket_error = error IS ERR::Okay ? ERR::Failed : error;
   }
}

//********************************************************************************************************************
// Reads every pending datagram and passes it to the transport.  Oversized datagrams are skipped.

ERR IceSocket::incoming(objNetServer *Socket, APTR Context)
{
   auto self = (IceSocket *)Context;

   for (;;) {
      IPAddress source{};
      int bytes_read = 0;
      auto error = Socket->recvFrom(&source, self->receive_buffer, &bytes_read);
      if (error IS ERR::BufferOverflow) continue;
      if (error != ERR::Okay or bytes_read <= 0) break;
      if (source.Port <= 0 or source.Port > 65535) continue;

      auto datagram = std::span<const uint8_t>((const uint8_t *)self->receive_buffer.data(), size_t(bytes_read));
      auto feed_error = self->transport.feed(datagram, from_network(source));
      if (feed_error != ERR::Okay) self->last_socket_error = feed_error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR IceSocket::handle_timeout()
{
   return transport.handle_timeout();
}

int64_t IceSocket::timeout_us() const
{
   return transport.timeout_us();
}

//********************************************************************************************************************
// FullIceSocket: full ICE and DTLS over host sockets and TURN relays.  Local endpoints with a non-zero route are
// relayed candidates, whose datagrams are sent through the TurnSocket that owns the allocation.

FullIceSocket::FullIceSocket(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
   std::string_view LocalPassword, ice::Role IceRole, DtlsRole DtlsRole, IceDtlsCallbacks Callbacks) :
   transport(std::move(Certificate), LocalUfrag, LocalPassword, IceRole, DtlsRole,
      IceDtlsCallbacks{
         [this](std::span<const uint8_t> Datagram, const ice::Endpoint &Destination) {
            send_datagram(Datagram, local_endpoint, Destination);
         },
         std::move(Callbacks.receive), std::move(Callbacks.state),
         [this](std::span<const uint8_t> Datagram, const ice::Endpoint &Local,
            const ice::Endpoint &Destination) { send_datagram(Datagram, Local, Destination); }
      }) { }

FullIceSocket::~FullIceSocket() { reset_routes(); }

//********************************************************************************************************************
// Releases every socket and relay, and clears gathering state.  TURN byte counts are retained in the totals.

void FullIceSocket::reset_routes()
{
   // Retire every sending identity before releasing its adapter.  Keep DTLS and its certificate alive.

   transport.retire_local_route(local_endpoint, true);
   for (auto &bound : additional_sockets) {
      transport.retire_local_route(bound.endpoint, true);
      if (bound.socket) FreeResource(bound.socket);
   }

   for (const auto &route : relay_candidates) transport.retire_local_route(route, true);

   for (const auto &relay : relays) {
      relay->retire();
      add_counter(totals.turn_sent_bytes, relay->counters.turn_sent_bytes);
      add_counter(totals.turn_received_bytes, relay->counters.turn_received_bytes);
   }

   route_counts.clear();
   additional_sockets.clear();
   relays.clear();
   relay_candidates.clear();
   reported_failures.clear();
   discoveries.clear();
   gathered_candidates.clear();
   remote_peers.clear();

   if (socket) FreeResource(socket);
   socket = nullptr;
   local_endpoint = {};
   reflexive_candidate = {};
   has_reflexive_candidate = false;
   last_socket_error = ERR::Okay;
   requested_start = false;
   agent_started = false;
}

//********************************************************************************************************************
// Binds the primary socket and adds it as a host candidate, unless the gathering policy excludes host candidates.

ERR FullIceSocket::open(std::string_view Address, int Port, uint32_t Priority)
{
   if (socket or Port < 0 or Port > 65535) return ERR::Args;

   socket = objNetServer::create::local(
      kt::FieldValue(kt::fieldhash("Address"), Address),
      fl::Port(Port), fl::Flags(NSF::UDP), fl::Incoming(C_FUNCTION(incoming, this)));
   if (not socket) return ERR::Failed;

   auto error = socket->setBufferSizes(1 << 20, 1 << 20);
   IPAddress bound{};
   if (error IS ERR::Okay) error = socket->getLocalIPAddress(&bound);

   if (error IS ERR::Okay) {
      local_endpoint = from_network(bound);
      if (not local_endpoint.port) local_endpoint.port = uint16_t(socket->Port);
      if (local_endpoint.port IS 0 or
          ((not deferred_hosts and not (policy & (POLICY_RELAY_ONLY | POLICY_NO_HOST))) and
           not transport.add_local_candidate({ local_endpoint, Priority, ice::CandidateType::HOST }))) {
         error = ERR::Failed;
      }
   }

   if (error != ERR::Okay) {
      FreeResource(socket);
      socket = nullptr;
   }
   return error;
}

//********************************************************************************************************************
// Binds an additional socket to a specific interface address.  Bound is set to the socket's local endpoint.

ERR FullIceSocket::add_interface(std::string_view Address, uint32_t Priority, ice::Endpoint &Bound)
{
   if (not socket or Priority IS 0) return ERR::InvalidState;
   if (additional_sockets.size() >= 127) return ERR::BufferOverflow;

   auto next = objNetServer::create::local(
      kt::FieldValue(kt::fieldhash("Address"), Address),
      fl::Port(0), fl::Flags(NSF::UDP), fl::Incoming(C_FUNCTION(incoming, this)));
   if (not next) return ERR::Failed;

   auto release = kt::Defer([&]() { if (next) FreeResource(next); });
   auto error = next->setBufferSizes(1 << 20, 1 << 20);
   IPAddress address{};
   if (error IS ERR::Okay) error = next->getLocalIPAddress(&address);
   if (error != ERR::Okay) return error;

   auto endpoint = from_network(address);
   if (not endpoint.port) endpoint.port = uint16_t(next->Port);
   if (endpoint.port IS 0 or
       ((not deferred_hosts and not (policy & (POLICY_RELAY_ONLY | POLICY_NO_HOST))) and
        not transport.add_local_candidate({ endpoint, Priority, ice::CandidateType::HOST }))) return ERR::Failed;

   Bound = endpoint;
   additional_sockets.push_back({ next, endpoint });
   next = nullptr;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR FullIceSocket::set_remote_fingerprint(std::string_view Fingerprint)
{
   return transport.set_remote_fingerprint(Fingerprint);
}

ERR FullIceSocket::set_dtls_role(DtlsRole Role)
{
   return transport.set_dtls_role(Role);
}

bool FullIceSocket::set_remote_credentials(std::string_view Ufrag, std::string_view Password, bool Lite)
{
   return transport.set_remote_credentials(Ufrag, Password, Lite);
}

//********************************************************************************************************************
// Adds a remote candidate.  The peer address is remembered so that relays can install permissions for it.

bool FullIceSocket::add_remote_candidate(const ice::Candidate &Value)
{
   if ((policy & POLICY_DISABLE_IPV6) and Value.endpoint.ipv6) return true;

   if (std::find(remote_peers.begin(), remote_peers.end(), Value.endpoint) IS remote_peers.end()) {
      if (remote_peers.size() >= 128) return false;
      remote_peers.push_back(Value.endpoint);
   }

   return transport.add_remote_candidate(Value);
}

//********************************************************************************************************************

void FullIceSocket::end_remote_candidates()
{
   transport.end_remote_candidates();
}

//********************************************************************************************************************
// Requests that connectivity checks start.  The agent is started by try_start() once it is able to.

bool FullIceSocket::start_ice(uint64_t NowUs)
{
   if (not socket) return false;
   requested_start = true;
   try_start(NowUs);
   return agent_started or gathering_pending();
}

//********************************************************************************************************************

void FullIceSocket::restart_ice(std::string_view LocalUfrag, std::string_view LocalPassword,
   ice::Role NewRole)
{
   ++generation;

   // A negotiated generation owns fresh discovery transactions and relay permissions.

   remote_peers.clear();
   requested_start = false;
   agent_started = false;
   transport.restart_ice(LocalUfrag, LocalPassword, NewRole);
}

//********************************************************************************************************************
// Starts server-reflexive discovery against a STUN server.  The request is sent from a socket of the server's
// address family; a loopback server requires a loopback socket, which is created if necessary.

bool FullIceSocket::start_server_reflexive(const ice::Endpoint &Server, uint64_t NowUs)
{
   if (not socket or discoveries.size() >= 16 or (policy & POLICY_RELAY_ONLY)) return false;

   ice::Endpoint gather_base;

   auto loopback = [](const ice::Endpoint &Endpoint) {
      if (not Endpoint.ipv6) return Endpoint.address[0] IS 127;
      return Endpoint.address[15] IS 1 and
         std::all_of(Endpoint.address.begin(), Endpoint.address.begin() + 15,
            [](uint8_t Byte) { return Byte IS 0; });
   };

   auto compatible = [&](const ice::Endpoint &Endpoint) {
      return Endpoint.ipv6 IS Server.ipv6 and (not loopback(Server) or loopback(Endpoint));
   };

   gather_base = local_endpoint;
   if (not compatible(gather_base)) {
      auto matching = std::find_if(additional_sockets.begin(), additional_sockets.end(),
         [&](const BoundSocket &Bound) { return compatible(Bound.endpoint); });
      if (matching != additional_sockets.end()) gather_base = matching->endpoint;
      else if (loopback(Server)) {
         // Windows cannot route a loopback destination from a socket bound to a physical interface.
         if (add_interface(Server.ipv6 ? "::1" : "127.0.0.1", 2130706431u, gather_base) != ERR::Okay) return false;
      }
      else return false;
   }

   auto next = std::make_unique<ice::ServerReflexiveGatherer>(gather_base, Server);
   if (not next->start()) return false;

   send_datagram(next->request(), gather_base, next->server());
   discoveries.push_back({gather_base, std::move(next), NowUs + 500000});
   return last_socket_error IS ERR::Okay;
}

//********************************************************************************************************************
// Sends a datagram from the Local route.  Relayed routes are sent through their TURN allocation; host routes are
// suppressed under the relay-only policy.

void FullIceSocket::send_datagram(std::span<const uint8_t> Datagram, const ice::Endpoint &Local,
   const ice::Endpoint &Destination)
{
   if (Local.route) {
      for (auto &relay : relays) if (relay->identity().allocation IS Local.route) {
         if (not relay->send(Destination, Datagram, uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count()))) {
            last_socket_error = relay->failure() IS ERR::Okay ? ERR::BufferOverflow : relay->failure();
         }
         else count_datagram(Local, Destination, Datagram.size(), true);
         return;
      }

      last_socket_error = ERR::Disconnected;
      return;
   }

   if (policy & POLICY_RELAY_ONLY) return;

   auto outgoing = Local IS local_endpoint ? socket : nullptr;
   if (not outgoing) {
      for (const auto &bound : additional_sockets) {
         if (bound.endpoint IS Local) {
            outgoing = bound.socket;
            break;
         }
      }
   }

   if (not outgoing) {
      last_socket_error = ERR::Failed;
      return;
   }

   auto address = to_network(Destination);
   auto bytes = std::span<const int8_t>((const int8_t *)Datagram.data(), Datagram.size());
   int sent = 0;
   auto error = outgoing->sendTo(&address, bytes, &sent);
   if (error != ERR::Okay or sent != int(Datagram.size())) {
      last_socket_error = error IS ERR::Okay ? ERR::Failed : error;
   }
   else count_datagram(Local, Destination, Datagram.size(), true);
}

//********************************************************************************************************************
// Reads up to 16 datagrams from a host socket.  Responses to server-reflexive discovery are consumed here, and all
// other datagrams are passed to the transport.

ERR FullIceSocket::incoming(objNetServer *Socket, APTR Context)
{
   auto self = (FullIceSocket *)Context;

   auto local = Socket IS self->socket ? &self->local_endpoint : nullptr;
   if (not local) {
      for (auto &bound : self->additional_sockets) {
         if (Socket IS bound.socket) {
            local = &bound.endpoint;
            break;
         }
      }
   }

   if (not local) return ERR::Okay;

   auto now = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());

   for (size_t count = 0; count < 16; ++count) {
      IPAddress source{};
      int bytes_read = 0;
      auto error = Socket->recvFrom(&source, self->receive_buffer, &bytes_read);
      if (error IS ERR::BufferOverflow) continue;
      if (error != ERR::Okay or bytes_read <= 0) break;
      if (source.Port <= 0 or source.Port > 65535) continue;

      auto datagram = std::span<const uint8_t>((const uint8_t *)self->receive_buffer.data(), size_t(bytes_read));
      auto remote = from_network(source);

      bool gathered = false;
      for (auto it = self->discoveries.begin(); it != self->discoveries.end(); ++it) {
         ice::Candidate candidate;
         if (it->base IS *local and it->gatherer->receive(datagram, remote, candidate)) {
            if (self->transport.add_local_candidate(candidate)) {
               self->reflexive_candidate = candidate;
               self->has_reflexive_candidate = true;
               self->gathered_candidates.push_back(candidate);
            }
            self->discoveries.erase(it);
            gathered = true;
            break;
         }
      }

      if (gathered) self->count_datagram(*local, remote, datagram.size(), false);
      if (gathered or (self->policy & POLICY_RELAY_ONLY)) continue;

      auto feed_error = self->transport.feed(datagram, *local, remote, now);
      if (self->transport.accepted_input()) self->count_datagram(*local, remote, datagram.size(), false);
      if (feed_error != ERR::Okay) self->last_socket_error = feed_error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Processes timers.  Discovery requests are retransmitted up to seven times with exponential backoff.  Each relay is
// ticked, its failure reported once, its relayed candidate added once active, and its received data passed to the
// transport.  A failure of the relay that carries the selected pair is returned as an error.

ERR FullIceSocket::handle_timeout(uint64_t NowUs)
{
   for (auto it = discoveries.begin(); it != discoveries.end();) {
      if (NowUs < it->next) {
         ++it;
         continue;
      }

      if (it->attempts >= 7) {
         it = discoveries.erase(it);
         continue;
      }

      send_datagram(it->gatherer->request(), it->base, it->gatherer->server());
      ++it->attempts;
      it->rto = it->attempts IS 7 ? 8000000u : it->rto * 2u;
      it->next = NowUs + it->rto;
      ++it;
   }

   for (auto &relay : relays) {
      relay->tick(NowUs);

      if (relay->failure() != ERR::Okay and std::find(reported_failures.begin(), reported_failures.end(),
          relay->identity().allocation) IS reported_failures.end()) {
         reported_failures.push_back(relay->identity().allocation);
         auto route = relay->candidate().endpoint;
         if (auto selected = selected_local(); selected and *selected IS route) last_socket_error = relay->failure();
         transport.retire_local_route(route);
         std::erase(relay_candidates, route);
         kt::Log("TURN").warning("Allocation %llu: %s", (unsigned long long)relay->identity().allocation,
            relay->failure_reason());
      }

      if (not relay->active()) continue;

      auto candidate = relay->candidate();
      if (std::find(relay_candidates.begin(), relay_candidates.end(), candidate.endpoint) IS relay_candidates.end()) {
         if (transport.add_local_candidate(candidate)) {
            relay_candidates.push_back(candidate.endpoint);
            gathered_candidates.push_back(candidate);
         }
      }

      for (const auto &peer : remote_peers) relay->permit(peer, NowUs);

      for (auto &datagram : relay->take_input()) {
         auto error = transport.feed(datagram.payload, candidate.endpoint, datagram.peer, NowUs);
         if (transport.accepted_input())
            count_datagram(candidate.endpoint, datagram.peer, datagram.payload.size(), false);
         if (error != ERR::Okay) last_socket_error = error;
      }
   }

   try_start(NowUs);

   if (auto selected = selected_local(); selected and selected->route) {
      for (const auto &relay : relays) if (relay->identity().allocation IS selected->route and not relay->active()) {
         last_socket_error = relay->failure();
         return last_socket_error;
      }
   }

   return transport.handle_timeout(NowUs);
}

//********************************************************************************************************************

void FullIceSocket::try_start(uint64_t NowUs)
{
   if (requested_start and not agent_started) agent_started = transport.start_ice(NowUs);
}

//********************************************************************************************************************
// Starts a TURN allocation on the server at Endpoint.  Each allocation is identified by the ICE generation and a
// serial number, which is also used as the route of its relayed candidate.

bool FullIceSocket::start_relay(const IceServer &Server, const ice::Endpoint &Endpoint, uint64_t NowUs)
{
   if (relays.size() >= 16 or ((policy & POLICY_DISABLE_IPV6) and Endpoint.ipv6)) return false;

   // A 1080-byte inner record leaves room for IPv6/UDP and unbound Send-indication overhead within 1200 bytes.
   // Select the conservative ceiling before DTLS starts, so SCTP sees the actual application budget at creation.

   if (Server.transport IS ServerTransport::UDP) transport.set_mtu(1080);

   auto relay = std::make_unique<TurnSocket>(Server, turn::Identity{Endpoint, generation, next_allocation++});
   (void)relay->open(NowUs);
   relays.push_back(std::move(relay));
   return true;
}

//********************************************************************************************************************

size_t FullIceSocket::pending_relays() const
{
   return size_t(std::count_if(relays.begin(), relays.end(), [](const auto &Relay) { return Relay->pending(); }));
}

//********************************************************************************************************************
// Returns the output capacity of the selected route.  Only relayed routes are limited.

size_t FullIceSocket::output_room() const
{
   if (auto selected = selected_local(); selected and selected->route) {
      for (const auto &relay : relays) if (relay->identity().allocation IS selected->route) return relay->available();
      return 0;
   }

   return turn::MAX_QUEUED;
}

//********************************************************************************************************************
// Abandons any discovery or allocation that is still in progress.

void FullIceSocket::finish_gathering()
{
   discoveries.clear();
   std::erase_if(relays, [](const auto &Relay) { return Relay->pending(); });
}

//********************************************************************************************************************

bool FullIceSocket::gathering_pending() const
{
   return not discoveries.empty() or std::any_of(relays.begin(), relays.end(),
      [](const auto &Relay) { return Relay->pending(); });
}

//********************************************************************************************************************

ERR FullIceSocket::gathering_error() const
{
   for (const auto &relay : relays) if (relay->failure() != ERR::Okay) return relay->failure();
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns an error only if no relay is active.

ERR FullIceSocket::relay_error() const
{
   if (std::any_of(relays.begin(), relays.end(), [](const auto &Relay) { return Relay->active(); })) return ERR::Okay;
   for (const auto &relay : relays) if (relay->failure() != ERR::Okay) return relay->failure();
   return ERR::Timeout;
}

//********************************************************************************************************************
// Returns the server-reflexive and relayed candidates gathered since the last call.

std::vector<ice::Candidate> FullIceSocket::take_candidates()
{
   auto result = std::move(gathered_candidates);
   gathered_candidates.clear();
   return result;
}

//********************************************************************************************************************
// Returns the time until the next timer.  Active relays are polled at least every 10 ms.

int64_t FullIceSocket::timeout_us(uint64_t NowUs) const
{
   auto due = transport.timeout_us(NowUs);
   if (not relays.empty()) due = due < 0 ? 10000 : std::min(due, int64_t(10000));

   for (const auto &job : discoveries) {
      auto gather_due = job.next <= NowUs ? 0 : int64_t(job.next - NowUs);
      due = due < 0 ? gather_due : std::min(due, gather_due);
   }

   return due;
}

//********************************************************************************************************************

ERR FullIceSocket::send(std::span<const uint8_t> Data)
{
   return transport.send(Data);
}

//********************************************************************************************************************
// Accounts for a datagram in the totals and, if it belongs to a checklist pair, in that route's counters.  Each
// route is assigned a serial number so that the owner can detect when the selected route changes.

void FullIceSocket::count_datagram(const ice::Endpoint &Local, const ice::Endpoint &Remote, size_t Bytes, bool Sent)
{
   if (Sent) totals.sent(Bytes);
   else totals.received(Bytes);

   if (not transport.has_pair(Local, Remote)) return;

   auto found = std::find_if(route_counts.begin(), route_counts.end(), [&](const auto &Value) {
      return Value.local IS Local and Value.remote IS Remote;
   });

   if (found IS route_counts.end()) {
      // Unsolicited checks only affect cumulative totals.  Reclaim retired checklist pairs before admitting
      // another route so authenticated input cannot consume the selected route's accounting capacity.

      if (route_counts.size() >= 512) {
         std::erase_if(route_counts, [&](const RouteCount &Value) {
            return not transport.has_pair(Value.local, Value.remote);
         });
      }

      if (route_counts.size() >= 512) return;
      route_counts.push_back({Local, Remote, {}, next_route_serial++});
      found = route_counts.end() - 1;
   }

   if (Sent) found->counters.sent(Bytes);
   else found->counters.received(Bytes);
}

//********************************************************************************************************************
// Returns the cumulative counters, including the TURN traffic of active relays.

TransportCounters FullIceSocket::counters() const
{
   auto result = totals;
   for (const auto &relay : relays) {
      add_counter(result.turn_sent_bytes, relay->counters.turn_sent_bytes);
      add_counter(result.turn_received_bytes, relay->counters.turn_received_bytes);
   }
   return result;
}

//********************************************************************************************************************

const TransportCounters * FullIceSocket::selected_counters() const
{
   auto local = selected_local(), remote = selected_remote();
   if (not local or not remote) return nullptr;
   for (const auto &value : route_counts) if (value.local IS *local and value.remote IS *remote) return &value.counters;
   return nullptr;
}

//********************************************************************************************************************

uint64_t FullIceSocket::selected_route_serial() const
{
   auto local = selected_local(), remote = selected_remote();
   if (not local or not remote) return 0;
   for (const auto &value : route_counts) if (value.local IS *local and value.remote IS *remote) return value.serial;
   return 0;
}

//********************************************************************************************************************
// Returns the TURN allocation that carries the selected pair, or null if the pair is not relayed.

const TurnSocket * FullIceSocket::selected_allocation() const
{
   auto local = selected_local();
   if (not local or not local->route) return nullptr;
   for (const auto &relay : relays) if (relay->identity().allocation IS local->route) return relay.get();
   return nullptr;
}

} // namespace rtc
