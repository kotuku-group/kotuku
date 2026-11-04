// Network NetServer adapter for the socket-independent ICE/DTLS transport.

#pragma once

#include "ice_dtls.h"
#include "ice_candidates.h"
#include "../turn/turn_socket.h"

#include <kotuku/modules/network.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace rtc {

//********************************************************************************************************************
// ICE-lite responder and DTLS server on a single UDP socket.

class IceSocket {
public:
   IceSocket(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
      std::string_view LocalPassword, std::string_view RemoteUfrag, IceDtlsCallbacks Callbacks);
   ~IceSocket();
   IceSocket(const IceSocket &) = delete;
   IceSocket & operator=(const IceSocket &) = delete;

   [[nodiscard]] ERR open(std::string_view Address, int Port);
   [[nodiscard]] ERR set_remote_fingerprint(std::string_view Fingerprint);
   [[nodiscard]] ERR handle_timeout();
   [[nodiscard]] int64_t timeout_us() const;

   [[nodiscard]] DtlsState state() const { return transport.state(); }
   [[nodiscard]] ERR socket_error() const { return last_socket_error; }
   [[nodiscard]] int port() const { return socket ? socket->Port : 0; }

private:
   static ERR incoming(objNetServer *Socket, APTR Context);
   void send(std::span<const uint8_t> Datagram, const ice::Endpoint &Destination);

   objNetServer *socket = nullptr;
   IceDtlsTransport transport;
   std::array<int8_t, 4096> receive_buffer{};
   ERR last_socket_error = ERR::Okay;
};

//********************************************************************************************************************
// Full ICE agent and DTLS transport over one or more host sockets, server-reflexive discovery and TURN relays.

class FullIceSocket {
public:
   FullIceSocket(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
      std::string_view LocalPassword, ice::Role IceRole, DtlsRole DtlsRole, IceDtlsCallbacks Callbacks);
   ~FullIceSocket();
   FullIceSocket(const FullIceSocket &) = delete;
   FullIceSocket & operator=(const FullIceSocket &) = delete;

   // Traffic accounting and the selected route

   TransportCounters counters() const;
   const TransportCounters * selected_counters() const;
   uint64_t selected_route_serial() const;
   const TurnSocket * selected_allocation() const;
   const ice::Candidate * selected_local_candidate() const { return transport.selected_local_candidate(); }
   const ice::Candidate * selected_remote_candidate() const { return transport.selected_remote_candidate(); }
   uint64_t selected_rtt_us() const { return transport.selected_rtt_us(); }
   bool selected_rtt_valid() const { return transport.selected_rtt_valid(); }

   // Candidate gathering

   void reset_routes();
   void set_policy(uint32_t Flags) { policy = Flags; }
   void defer_hosts(bool Value) { deferred_hosts = Value; }
   bool admit_host(const ice::Candidate &Value) { return transport.add_local_candidate(Value); }
   void retire_host(const ice::Endpoint &Value) { transport.retire_local_route(Value); }
   [[nodiscard]] bool start_relay(const IceServer &Server, const ice::Endpoint &Endpoint, uint64_t NowUs);
   [[nodiscard]] bool gathering_pending() const;
   [[nodiscard]] size_t pending_relays() const;
   [[nodiscard]] size_t output_room() const;
   void finish_gathering();
   std::vector<ice::Candidate> take_candidates();
   [[nodiscard]] ERR relay_error() const;
   [[nodiscard]] ERR gathering_error() const;

   // Sockets and connectivity

   [[nodiscard]] ERR open(std::string_view Address, int Port, uint32_t Priority = 2130706431u);
   [[nodiscard]] ERR add_interface(std::string_view Address, uint32_t Priority, ice::Endpoint &Bound);
   [[nodiscard]] ERR set_remote_fingerprint(std::string_view Fingerprint);
   [[nodiscard]] ERR set_dtls_role(DtlsRole Role);
   [[nodiscard]] bool set_remote_credentials(std::string_view Ufrag, std::string_view Password, bool Lite);
   [[nodiscard]] bool add_remote_candidate(const ice::Candidate &Value);
   void end_remote_candidates();
   [[nodiscard]] bool start_ice(uint64_t NowUs);
   void restart_ice(std::string_view LocalUfrag, std::string_view LocalPassword, ice::Role NewRole);
   [[nodiscard]] bool start_server_reflexive(const ice::Endpoint &Server, uint64_t NowUs);
   [[nodiscard]] ERR handle_timeout(uint64_t NowUs);
   [[nodiscard]] int64_t timeout_us(uint64_t NowUs) const;
   [[nodiscard]] ERR send(std::span<const uint8_t> Data);
   [[nodiscard]] int max_payload() const { return transport.max_payload(); }

   [[nodiscard]] const ice::Endpoint & local_address() const { return local_endpoint; }
   [[nodiscard]] const ice::Candidate * server_reflexive_candidate() const {
      return has_reflexive_candidate ? &reflexive_candidate : nullptr;
   }
   [[nodiscard]] bool server_reflexive_pending() const { return not discoveries.empty(); }
   [[nodiscard]] ice::State ice_state() const { return transport.ice_state(); }
   [[nodiscard]] ice::Role ice_role() const { return transport.ice_role(); }
   [[nodiscard]] DtlsRole dtls_role() const { return transport.dtls_role(); }
   [[nodiscard]] const ice::Endpoint * selected_local() const { return transport.selected_local(); }
   [[nodiscard]] const ice::Endpoint * selected_remote() const { return transport.selected_remote(); }
   [[nodiscard]] DtlsState state() const { return transport.state(); }
   [[nodiscard]] ERR socket_error() const { return last_socket_error; }

private:
   static ERR incoming(objNetServer *Socket, APTR Context);
   void send_datagram(std::span<const uint8_t> Datagram, const ice::Endpoint &Local,
      const ice::Endpoint &Destination);
   void count_datagram(const ice::Endpoint &Local, const ice::Endpoint &Remote, size_t Bytes, bool Sent);
   void try_start(uint64_t NowUs);

   struct RouteCount { ice::Endpoint local, remote; TransportCounters counters; uint64_t serial; };

   struct BoundSocket { // An additional socket bound to a specific interface
      objNetServer *socket = nullptr;
      ice::Endpoint endpoint;
   };

   struct Discovery { // An outstanding server-reflexive discovery transaction
      ice::Endpoint base;
      std::unique_ptr<ice::ServerReflexiveGatherer> gatherer;
      uint64_t next = 0;
      uint32_t rto = 500000;
      int attempts = 1;
   };

   TransportCounters totals;
   std::vector<RouteCount> route_counts;
   uint64_t next_route_serial = 1;
   objNetServer *socket = nullptr; // Primary host socket
   std::vector<BoundSocket> additional_sockets;
   FullIceDtlsTransport transport;
   ice::Endpoint local_endpoint;
   std::vector<Discovery> discoveries;
   std::vector<std::unique_ptr<TurnSocket>> relays;
   std::vector<ice::Endpoint> relay_candidates;   // Relayed candidates that have been added to the agent
   std::vector<uint64_t> reported_failures;       // Allocations whose failure has been reported
   std::vector<ice::Candidate> gathered_candidates;
   std::vector<ice::Endpoint> remote_peers;       // Remote candidate addresses, for relay permissions
   uint32_t policy = 0;
   uint64_t generation = 1, next_allocation = 1; // ICE generation and allocation serial
   bool deferred_hosts = false;
   bool requested_start = false;
   bool agent_started = false;
   ice::Candidate reflexive_candidate;
   bool has_reflexive_candidate = false;
   std::array<int8_t, 4096> receive_buffer{};
   ERR last_socket_error = ERR::Okay;
};

} // namespace rtc
