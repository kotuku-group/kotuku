// Data-channel-only PeerConnection state machine.

#pragma once

#include "../ice/ice_socket.h"
#include "../ice/interface_monitor.h"
#include "../sdp/sdp.h"
#include "../sctp/sctp_association.h"
#include "../../network/mdns/mdns.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rtc {

enum class PeerState { NEW, CONNECTING, CONNECTED, DISCONNECTED, FAILED, CLOSED };
enum class SignalingState { STABLE, HAVE_LOCAL_OFFER, HAVE_REMOTE_OFFER, CLOSED };
enum class GatheringState { NEW, GATHERING, COMPLETE };

// The engine's record of a channel, which backs one WebChannel object.

struct PublicChannel {
   SctpChannelOptions options;
   SctpChannelState state = SctpChannelState::CONNECTING;
   ERR error = ERR::Okay;
   int id = -1; // SCTP stream ID, or -1 if not yet assigned
   size_t buffered = 0, threshold = 0;
   bool detached = false; // The WebChannel object has been freed
};

struct PublicChannelEvent {
   uint64_t handle;
   SctpChannelEventType type;
   ERR error = ERR::Okay;
   bool text = false, low = false; // low marks a buffered-amount-low notification
   std::vector<uint8_t> payload;
   uint16_t stream = 0;
   size_t receive_credit = 0;
};

struct PeerCallbacks {
   std::function<void(PeerState)> state;
   std::function<void(ice::State)> ice_state;
   std::function<void(std::string_view)> candidate;

   // Requests a prompt tick() after queued input or accepted channel work.  Must not re-enter the connection.

   std::function<void()> wake;
};

//********************************************************************************************************************

class PeerConnection {
public:
   explicit PeerConnection(PeerCallbacks Callbacks = {});
   ~PeerConnection();
   PeerConnection(const PeerConnection &) = delete;
   PeerConnection & operator=(const PeerConnection &) = delete;

   // Data channels

   void enable_channels() { channel_mode = true; }
   [[nodiscard]] ERR set_receive_limit(uint32_t Limit);
   [[nodiscard]] ERR create_channel(const SctpChannelOptions &Options, uint64_t &Handle);
   [[nodiscard]] ERR send_channel(uint64_t Handle, std::span<const uint8_t> Payload, bool Text);
   [[nodiscard]] ERR close_channel(uint64_t Handle);
   void detach_channel(uint64_t Handle);
   [[nodiscard]] const PublicChannel * channel(uint64_t Handle) const;
   [[nodiscard]] ERR set_threshold(uint64_t Handle, size_t Value);
   [[nodiscard]] uint64_t max_message_size() const;
   std::vector<PublicChannelEvent> take_channel_events();
   void release_channel_events(std::span<const PublicChannelEvent> Events);

   // Signalling and configuration

   [[nodiscard]] ERR create_offer();
   [[nodiscard]] ERR restart_ice();
   [[nodiscard]] ERR create_answer(bool Active = false);
   [[nodiscard]] bool has_ice_servers() const { return not ice_servers.empty(); }
   [[nodiscard]] ERR set_ice_servers(std::string_view Text);
   [[nodiscard]] ERR set_policy(uint32_t Flags);
   [[nodiscard]] ERR set_stun_server(std::string_view Address);
   [[nodiscard]] ERR set_local_description(std::string_view Text);
   [[nodiscard]] ERR set_remote_description(std::string_view Text);
   [[nodiscard]] ERR add_ice_candidate(std::string_view Text);

   // Processing

   [[nodiscard]] ERR tick(uint64_t NowUs);
   std::string stats() const;
   void close();
   [[nodiscard]] int64_t timeout_us(uint64_t NowUs) const;
   [[nodiscard]] SctpState sctp_state() const { return association ? association->state() : SctpState::CLOSED; }
   [[nodiscard]] ERR send_message(uint16_t Stream, uint32_t PPID, std::span<const uint8_t> Payload, bool Unordered);
   std::vector<SctpMessage> take_messages();

   // State

   [[nodiscard]] const std::string & local_description() const { return local_text; }
   [[nodiscard]] const std::string & remote_description() const { return remote_text; }

   // Advances whenever the text of the local or remote description changes.

   [[nodiscard]] uint64_t description_revision() const { return text_revision; }

   // The number of times that the local description has been serialised, for performance diagnostics.

   [[nodiscard]] uint64_t local_serialisations() const { return serialisations; }
   [[nodiscard]] const std::string & certificate_fingerprint() const { return local.data.fingerprint; }
   [[nodiscard]] PeerState state() const { return current_state; }
   [[nodiscard]] SignalingState signaling_state() const { return signal_state; }
   [[nodiscard]] ice::State ice_state() const { return current_ice_state; }
   [[nodiscard]] GatheringState gathering_state() const { return gather_state; }
   [[nodiscard]] bool candidate_routes_ready() const { return local_sockets_ready; }
   [[nodiscard]] bool selected_relay() const {
      return socket and socket->selected_local() and socket->selected_local()->route != 0;
   }
   [[nodiscard]] bool restart_required() const { return needs_restart; }
   [[nodiscard]] uint64_t restart_generation() const { return restart_request_generation; }

   // Owner-task entry point shared by OS detection and deterministic route-loss fixtures.

   void network_changed(uint64_t NowUs);
   [[nodiscard]] uint64_t candidate_generation() const { return mdns_generation; }
   [[nodiscard]] ERR gathering_error() const { return mdns_error != ERR::Okay ? mdns_error : stun_lookup_error; }

private:
   struct LocalName { ice::CandidateLine line; bool reported = false, failed = false; };

   struct RemoteName {
      ice::CandidateLine line;
      bool done = false;
      std::vector<net::mdns::Address> addresses; // Preserve the private mapping for future selected-route privacy.
   };

   struct ServerJob { // Gathering from one IceServers entry
      PeerConnection *owner;
      IceServer server;
      objNetLookup *lookup = nullptr;
      std::vector<ice::Endpoint> endpoints; // Resolved addresses that have not yet been tried
      uint64_t started = 0, alternate = 0;
      bool scheduled = false, resolved = false, dispatched = false;
   };

   // Gathering and routes

   ERR gather_routes();
   ERR restart_routes(std::string Ufrag, std::string Password);
   void queue_host(ice::CandidateLine Value, uint64_t NowUs);
   void pump_mdns(uint64_t NowUs);
   ERR accept_remote(ice::CandidateLine Value, uint64_t NowUs);
   void finish_remote();
   static ERR server_lookup_complete(objNetLookup *Lookup, ERR Error, std::string_view HostName,
      const std::vector<IPAddress> &Addresses, APTR Context);
   void gather_servers(uint64_t NowUs);
   void cancel_servers();
   static ERR lookup_complete(objNetLookup *Lookup, ERR Error, std::string_view HostName,
      const std::vector<IPAddress> &Addresses, APTR Context);
   void resolved_stun_server(ERR Error, const std::vector<IPAddress> &Addresses);
   void release_lookup();

   // Transport and channels

   [[nodiscard]] ERR ensure_socket(ice::Role IceRole, DtlsRole DtlsRole);
   [[nodiscard]] ERR configure_remote();
   void update_state();
   [[nodiscard]] ERR pump_sctp(uint64_t NowUs);
   void collect_channel_events();
   void terminate_channels(ERR Error);
   void refresh_buffered();
   void wake() { if (callbacks.wake) callbacks.wake(); }

   // Description text.  Call local_changed() after any change to the local description.

   void local_changed() { ++local_revision; }
   [[nodiscard]] bool serialise_local();
   void refresh_local();
   void clear_local_text();
   void set_remote_text(std::string_view Text);

   std::unique_ptr<InterfaceMonitor> interfaces;
   bool needs_restart = false;
   uint64_t restart_request_generation = 0, recovery_started = 0;
   std::unique_ptr<net::mdns::Service> mdns;
   std::vector<LocalName> local_names;
   std::vector<RemoteName> remote_names;
   std::vector<ice::CandidateLine> early_candidates; // Trickled before the remote description was configured
   bool remote_end = false, remote_configured = false;
   ERR mdns_error = ERR::Okay;
   uint64_t mdns_generation = 1; // ICE candidate generation, incremented on each restart
   std::vector<IceServer> ice_servers;
   std::vector<std::unique_ptr<ServerJob>> server_jobs;
   uint32_t policy = 0;
   uint64_t gathering_started = 0;

   TransportCounters closed_counters;
   PeerCallbacks callbacks;
   std::shared_ptr<DtlsCertificate> certificate;
   std::unique_ptr<FullIceSocket> socket;
   std::unique_ptr<SctpAssociation> association;
   std::unique_ptr<SctpChannels> channels;
   std::map<uint64_t, PublicChannel> public_channels;
   std::map<uint16_t, uint64_t> stream_handles; // Active SCTP stream to channel handle
   std::deque<PublicChannelEvent> channel_events;
   uint64_t next_handle = 1;
   uint32_t receive_limit = 262144;
   bool channel_mode = false;
   std::deque<std::vector<uint8_t>> sctp_input; // Decrypted DTLS records awaiting the association
   size_t sctp_input_bytes = 0;
   bool sctp_input_overflow = false;
   bool sctp_path_reset = false;
   std::optional<ice::Endpoint> sctp_path;
   std::optional<ice::Endpoint> sctp_local_path;
   sdp::Description local;
   sdp::Description remote;
   std::string local_text;
   std::string remote_text;
   uint64_t local_revision = 0;      // Advanced by every change to the local description
   uint64_t serialised_revision = 0; // The local_revision that local_text was serialised from
   uint64_t text_revision = 0;       // Advanced by every change to local_text or remote_text
   uint64_t serialisations = 0;
   std::string local_ufrag;
   std::string local_password;
   ice::Endpoint stun_server;            // Legacy StunServer
   objNetLookup *stun_lookup = nullptr;
   uint16_t stun_port = 0;
   ERR stun_lookup_error = ERR::Okay;
   bool stun_lookup_pending = false;
   bool local_sockets_ready = false;
   bool has_stun_server = false;
   PeerState current_state = PeerState::NEW;
   SignalingState signal_state = SignalingState::STABLE;
   ice::State current_ice_state = ice::State::NEW;
   GatheringState gather_state = GatheringState::NEW;
   bool local_committed = false;
   bool remote_committed = false;
   bool ice_started = false;
};

} // namespace rtc
