// Object-independent PeerConnection engine.  The engine implements JSEP offer/answer signalling for one bundled
// data-channel section, gathers host (mDNS), server-reflexive and relayed candidates, and drives ICE, DTLS, SCTP and
// the data channel layer.  The PeerConnection class calls tick() from a timer; nothing here runs on another thread.

#ifdef _WIN32
#define OBJECTID WindowsObjectID
#include <winsock2.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "peer_engine.h"
#include "../sctp/dcep.h"

#include <kotuku/modules/network.h>
#include <kotuku/strings.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <utility>

namespace rtc {
namespace {

//********************************************************************************************************************

uint64_t now_us()
{
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

//********************************************************************************************************************
// Generates random ice-char text for ICE credentials.  An empty string is returned if entropy is unavailable.

std::string random_ice_text(size_t Length)
{
   static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string result(Length, '\0');
   for (size_t i = 0; i < Length; ++i) {
      uint8_t value = 0;
      if (RAND_bytes(&value, 1) != 1) return {};
      result[i] = alphabet[value & 63];
   }
   return result;
}

//********************************************************************************************************************

std::string host_address(const ice::Endpoint &Endpoint)
{
   char text[INET6_ADDRSTRLEN]{};
   if (not inet_ntop(Endpoint.ipv6 ? AF_INET6 : AF_INET, Endpoint.address.data(), text, sizeof(text))) return {};
   return text;
}

//********************************************************************************************************************
// Parses a STUN server as a stun: URI (RFC 7064), or as a bare host and port.  The port defaults to 3478 for a URI.

bool stun_target(std::string_view Address, std::string_view &Host, uint16_t &Port)
{
   bool uri = Address.size() >= 5 and kt::iequals(Address.substr(0, 5), "stun:");
   if (uri) Address.remove_prefix(5);

   if (Address.empty() or Address.starts_with("//") or Address.find_first_of("/?#@") != std::string_view::npos) {
      return false;
   }

   std::string_view port_text;
   if (Address.starts_with('[')) {
      auto close = Address.find(']');
      if (close IS std::string_view::npos) return false;
      Host = Address.substr(1, close - 1);
      if (close + 1 < Address.size()) {
         if (Address[close + 1] != ':') return false;
         port_text = Address.substr(close + 2);
      }
      else if (not uri) return false;
   }
   else {
      auto colon = Address.find(':');
      if (colon IS std::string_view::npos) {
         if (not uri) return false;
         Host = Address;
      }
      else {
         if (Address.find(':', colon + 1) != std::string_view::npos) return false;
         Host = Address.substr(0, colon);
         port_text = Address.substr(colon + 1);
      }
   }

   if (Host.empty() or Host.size() > 253 or (port_text.empty() and Address.ends_with(':'))) return false;

   Port = 3478;
   if (not port_text.empty()) {
      uint32_t parsed = 0;
      auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
      if (error != std::errc() or end != port_text.data() + port_text.size() or parsed IS 0 or parsed > 65535) {
         return false;
      }
      Port = uint16_t(parsed);
   }

   return true;
}

} // namespace

//********************************************************************************************************************

PeerConnection::PeerConnection(PeerCallbacks Callbacks) : callbacks(std::move(Callbacks)) { }

PeerConnection::~PeerConnection() { close(); }

//********************************************************************************************************************
// Cancels the legacy StunServer lookup.

void PeerConnection::release_lookup()
{
   if (stun_lookup) {
      FreeResource(stun_lookup);
      stun_lookup = nullptr;
   }
   stun_lookup_pending = false;
}

//********************************************************************************************************************
// NetLookup callback for the legacy StunServer.  Results for a superseded lookup are ignored.

ERR PeerConnection::lookup_complete(objNetLookup *Lookup, ERR Error, std::string_view,
   const std::vector<IPAddress> &Addresses, APTR Context)
{
   auto self = (PeerConnection *)Context;
   if (self and Lookup IS self->stun_lookup and self->current_state != PeerState::CLOSED) {
      self->resolved_stun_server(Error, Addresses);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Selects the first resolved STUN server address whose family matches a local interface, preferring IPv4.  If the
// local sockets are already open, server-reflexive discovery starts immediately.

void PeerConnection::resolved_stun_server(ERR Error, const std::vector<IPAddress> &Addresses)
{
   if (not stun_lookup_pending) return;

   stun_lookup_pending = false;
   has_stun_server = false;
   stun_lookup_error = Error;
   if (Error != ERR::Okay) return;

   auto hosts = ice::gather_host_candidates(1, false, not (policy & POLICY_DISABLE_IPV6));
   if (hosts.empty()) hosts = ice::gather_host_candidates(1, true, not (policy & POLICY_DISABLE_IPV6));

   for (bool ipv6 : {false, true}) {
      if (std::none_of(hosts.begin(), hosts.end(), [ipv6](const ice::Candidate &Candidate) {
          return Candidate.endpoint.ipv6 IS ipv6;
      })) continue;

      for (auto ip : Addresses) {
         if (ip.Type != (ipv6 ? IPADDR::V6 : IPADDR::V4)) continue;

         std::string address_text;
         if (net::AddressToStr(&ip, &address_text) != ERR::Okay) continue;

         auto text = std::string(ipv6 ? "[" : "") + address_text +
            (ipv6 ? "]" : "") + ":" + std::to_string(stun_port);
         ice::Endpoint parsed;
         if (not ice::parse_numeric_endpoint(text, parsed)) continue;

         stun_server = parsed;
         has_stun_server = true;
         if (socket and local_sockets_ready and
             not socket->start_server_reflexive(stun_server, now_us())) {
            has_stun_server = false;
            stun_lookup_error = ERR::NoSupport;
         }
         return;
      }
   }

   stun_lookup_error = ERR::NoSupport;
}

//********************************************************************************************************************
// Configures the legacy StunServer.  A numeric address is used directly, while a host name is resolved
// asynchronously.  An empty Address removes the server.  STUNS is not supported.

ERR PeerConnection::set_stun_server(std::string_view Address)
{
   if (current_state != PeerState::NEW or socket) return ERR::InvalidState;

   if (Address.empty()) {
      release_lookup();
      has_stun_server = false;
      stun_lookup_error = ERR::Okay;
      return ERR::Okay;
   }

   if (Address.size() >= 6 and kt::iequals(Address.substr(0, 6), "stuns:")) return ERR::NoSupport;

   std::string_view host;
   uint16_t port = 0;
   if (not stun_target(Address, host, port)) return ERR::InvalidData;

   ice::Endpoint parsed;
   auto numeric = host.find(':') != std::string_view::npos;
   auto endpoint_text = std::string(numeric ? "[" : "") + std::string(host) +
      (numeric ? "]" : "") + ":" + std::to_string(port);
   if (ice::parse_numeric_endpoint(endpoint_text, parsed)) {
      release_lookup();
      stun_server = parsed;
      has_stun_server = true;
      stun_lookup_error = ERR::Okay;
      return ERR::Okay;
   }

   if (numeric or host.find_first_of("[]:") != std::string_view::npos) return ERR::InvalidData;

   auto lookup = objNetLookup::create::local();
   if (not lookup) return ERR::CreateObject;

   if (auto error = lookup->setCallback(C_FUNCTION(lookup_complete, this)); error != ERR::Okay) {
      FreeResource(lookup);
      return error;
   }

   release_lookup();
   stun_lookup = lookup;
   stun_lookup_pending = true;
   stun_lookup_error = ERR::Okay;
   stun_port = port;
   has_stun_server = false;

   if (auto error = lookup->resolveName(host); error != ERR::Okay) {
      release_lookup();
      return error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR PeerConnection::set_ice_servers(std::string_view Text)
{
   if (socket or current_state != PeerState::NEW) return ERR::InvalidState;
   return parse_ice_servers(Text, ice_servers);
}

//********************************************************************************************************************
// Sets the gathering policy, which is a combination of the POLICY flags.

ERR PeerConnection::set_policy(uint32_t Flags)
{
   if (socket or current_state != PeerState::NEW) return ERR::InvalidState;
   if (Flags & ~uint32_t(15)) return ERR::InvalidValue;
   policy = Flags;
   return ERR::Okay;
}

//********************************************************************************************************************
// Cancels all IceServers gathering jobs.

void PeerConnection::cancel_servers()
{
   for (auto &job : server_jobs) if (job->lookup) {
      FreeResource(job->lookup);
      job->lookup = nullptr;
   }
   server_jobs.clear();
}

//********************************************************************************************************************
// NetLookup callback for an IceServers job.  One address of each permitted family is retained, IPv4 first.

ERR PeerConnection::server_lookup_complete(objNetLookup *Lookup, ERR Error, std::string_view,
   const std::vector<IPAddress> &Addresses, APTR Context)
{
   auto job = (ServerJob *)Context;
   if (not job or Lookup != job->lookup or job->owner->current_state IS PeerState::CLOSED) return ERR::Okay;

   job->resolved = true;
   if (Error != ERR::Okay) {
      job->owner->stun_lookup_error = Error;
      return ERR::Okay;
   }

   for (bool ipv6 : {false, true}) {
      if (ipv6 and (job->owner->policy & POLICY_DISABLE_IPV6)) continue;
      for (auto ip : Addresses) {
         if (ip.Type != (ipv6 ? IPADDR::V6 : IPADDR::V4)) continue;
         std::string text;
         if (net::AddressToStr(&ip, &text) != ERR::Okay) continue;
         ice::Endpoint endpoint;
         if (ice::parse_numeric_endpoint((ipv6 ? "[" : "") + text + (ipv6 ? "]:" : ":") +
             std::to_string(job->server.port), endpoint)) {
            job->endpoints.push_back(endpoint);
            break;
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Advances the IceServers gathering jobs.  At most four jobs resolve or allocate concurrently, name resolution times
// out after five seconds, and the addresses of one server are tried 250 ms apart.

void PeerConnection::gather_servers(uint64_t NowUs)
{
   size_t outstanding = socket->pending_relays();
   for (const auto &job : server_jobs) if (job->scheduled and not job->dispatched) ++outstanding;

   for (auto &job : server_jobs) {
      if (not job->scheduled) {
         if (outstanding >= 4) continue;
         job->scheduled = true;
         job->started = NowUs;
         ++outstanding;

         ice::Endpoint numeric;
         auto text = (job->server.host.find(':') IS std::string::npos ? job->server.host + ":" :
            "[" + job->server.host + "]:") + std::to_string(job->server.port);
         if (ice::parse_numeric_endpoint(text, numeric)) {
            if (not numeric.ipv6 or not (policy & POLICY_DISABLE_IPV6)) job->endpoints.push_back(numeric);
            job->resolved = true;
         }
         else {
            job->lookup = objNetLookup::create::local();
            if (not job->lookup or job->lookup->setCallback(C_FUNCTION(server_lookup_complete, job.get())) !=
                ERR::Okay or job->lookup->resolveName(job->server.host) != ERR::Okay) {
               job->resolved = true;
               stun_lookup_error = ERR::Failed;
            }
         }
      }

      if (not job->resolved and NowUs - job->started >= 5000000) {
         if (job->lookup) {
            FreeResource(job->lookup);
            job->lookup = nullptr;
         }
         job->resolved = true;
         stun_lookup_error = ERR::Timeout;
      }

      if (not job->resolved or job->dispatched or socket->pending_relays() >= 4) continue;
      if (job->alternate and NowUs < job->alternate) continue;

      if (not job->endpoints.empty()) {
         auto endpoint = job->endpoints.front();
         job->endpoints.erase(job->endpoints.begin());
         if (job->server.transport IS ServerTransport::STUN) {
            if (not (policy & POLICY_RELAY_ONLY)) (void)socket->start_server_reflexive(endpoint, NowUs);
         }
         else if (not socket->start_relay(job->server, endpoint, NowUs)) stun_lookup_error = ERR::BufferOverflow;
         job->alternate = NowUs + 250000;
      }

      job->dispatched = job->endpoints.empty();
   }
}

//********************************************************************************************************************
// Creates the certificate, ICE credentials and transport on first use, then gathers local routes.  Received DTLS
// records are queued for SCTP, which processes them from tick().

ERR PeerConnection::ensure_socket(ice::Role IceRole, DtlsRole DtlsRole)
{
   if (socket) return ERR::Okay;

   local_sockets_ready = false;
   if (DtlsCertificate::generate(certificate) != ERR::Okay) return ERR::Failed;

   local_ufrag = random_ice_text(8);
   local_password = random_ice_text(32);
   if (local_ufrag.empty() or local_password.empty()) return ERR::Failed;

   IceDtlsCallbacks dtls_callbacks;
   dtls_callbacks.receive = [this](std::span<const uint8_t> Packet) {
      // Copy borrowed records; association output must wait until the DTLS call has returned.
      if (sctp_input.size() >= MAX_SCTP_INPUT_RECORDS or Packet.size() > MAX_SCTP_INPUT_BYTES - sctp_input_bytes) {
         sctp_input_overflow = true;
         return;
      }
      sctp_input.emplace_back(Packet.begin(), Packet.end());
      sctp_input_bytes += Packet.size();
      wake();
   };

   // Report transport changes from tick(), after the socket has finished dispatching its callbacks.
   // Application callbacks may close the connection and release the transport.

   socket = std::make_unique<FullIceSocket>(certificate, local_ufrag, local_password,
      IceRole, DtlsRole, dtls_callbacks);
   interfaces = std::make_unique<InterfaceMonitor>();
   (void)interfaces->poll(now_us());
   return gather_routes();
}

//********************************************************************************************************************
// Opens a socket on each host interface and starts gathering.  The primary socket is bound to the first host of the
// legacy STUN server's family.  Host candidates are registered with mDNS unless NO_MDNS is set, and the local
// description is prepared for the current credentials.

ERR PeerConnection::gather_routes()
{
   auto hosts = ice::gather_host_candidates(1, false, not (policy & POLICY_DISABLE_IPV6));
   if (hosts.empty()) hosts = ice::gather_host_candidates(1, true, not (policy & POLICY_DISABLE_IPV6));

   if (hosts.empty()) {
      socket.reset();
      return ERR::Failed;
   }

   auto host = hosts.begin();
   if (has_stun_server) {
      auto same_family = [this](const ice::Candidate &Value) {
         return Value.endpoint.ipv6 IS stun_server.ipv6;
      };

      host = std::find_if(hosts.begin(), hosts.end(), same_family);
      if (host IS hosts.end()) {
         hosts = ice::gather_host_candidates(1, true, not (policy & POLICY_DISABLE_IPV6));
         host = std::find_if(hosts.begin(), hosts.end(), same_family);
      }

      if (host IS hosts.end()) {
         socket.reset();
         return ERR::NoSupport;
      }
   }

   socket->set_policy(policy);
   socket->defer_hosts(not (policy & POLICY_NO_MDNS));
   mdns = std::make_unique<net::mdns::Service>();
   (void)mdns->open(not (policy & POLICY_DISABLE_IPV6));

   auto address = host_address(host->endpoint);
   if (address.empty()) {
      socket.reset();
      return ERR::Failed;
   }

   auto error = socket->open(address, 0, host->priority);
   if (error != ERR::Okay) {
      socket.reset();
      return error;
   }

   gathering_started = now_us();
   gather_state = (not (policy & (POLICY_NO_MDNS | POLICY_NO_HOST | POLICY_RELAY_ONLY)) or
      has_stun_server or stun_lookup_pending or not ice_servers.empty() or (policy & POLICY_RELAY_ONLY)) ?
      GatheringState::GATHERING : GatheringState::COMPLETE;

   if (local.session_id.empty()) local.session_id = "1";
   local.trickle = true;
   if (local.data.mid.empty()) local.data.mid = "0";
   local.data.ice_ufrag = local_ufrag;
   local.data.ice_pwd = local_password;
   local.data.fingerprint = certificate->fingerprint();
   local.data.end_of_candidates = gather_state IS GatheringState::COMPLETE;
   local_changed();

   ice::CandidateLine line;
   line.foundation = "1";
   line.candidate = *host;
   line.candidate.endpoint.port = socket->local_address().port;
   if (not (policy & (POLICY_RELAY_ONLY | POLICY_NO_HOST))) {
      queue_host(line, gathering_started);
   }

   if (not socket or current_state IS PeerState::CLOSED) return ERR::InvalidState;

   for (const auto &candidate : hosts) {
      if (&candidate IS &*host) continue;

      auto interface_address = host_address(candidate.endpoint);
      if (interface_address.empty()) continue;

      ice::Endpoint bound;
      if (socket->add_interface(interface_address, candidate.priority, bound) != ERR::Okay) continue;
      if (policy & (POLICY_RELAY_ONLY | POLICY_NO_HOST)) continue;

      line.foundation = std::to_string(local.data.candidates.size() + 1);
      line.candidate = candidate;
      line.candidate.endpoint.port = bound.port;
      queue_host(line, gathering_started);
      if (not socket or current_state IS PeerState::CLOSED) return ERR::InvalidState;
   }

   local_sockets_ready = true;

   if (has_stun_server and not (policy & POLICY_RELAY_ONLY) and
       not socket->start_server_reflexive(stun_server, now_us())) {
      local_sockets_ready = false;
      socket.reset();
      return ERR::NoSupport;
   }

   for (const auto &server : ice_servers) {
      auto job = std::make_unique<ServerJob>();
      job->owner = this;
      job->server = server;
      server_jobs.push_back(std::move(job));
   }

   gather_servers(gathering_started);
   return ERR::Okay;
}

//********************************************************************************************************************
// Publishes a host candidate.  With NO_MDNS the numeric candidate is signalled immediately.  Otherwise the address is
// registered under a random .local name (RFC 8828), and the candidate is signalled by pump_mdns() once registration
// has succeeded.

void PeerConnection::queue_host(ice::CandidateLine Value, uint64_t NowUs)
{
   Value.ufrag = local_ufrag;

   if (policy & POLICY_NO_MDNS) {
      auto text = ice::format_candidate(Value);
      if (text.empty()) return;
      local.data.candidates.push_back(text);
      local_changed();
      if (callbacks.candidate) callbacks.candidate(text);
      return;
   }

   Value.foundation = std::to_string(local_names.size() + 1);

   std::array<uint8_t, 16> random{};
   if (RAND_bytes(random.data(), int(random.size())) != 1) return;

   constexpr char hex[] = "0123456789abcdef";
   Value.mdns_name.clear();
   for (auto byte : random) {
      Value.mdns_name += hex[byte >> 4];
      Value.mdns_name += hex[byte & 15];
   }
   Value.mdns_name += ".local";

   net::mdns::Address address;
   address.bytes = Value.candidate.endpoint.address;
   address.ipv6 = Value.candidate.endpoint.ipv6;
   if (not mdns or not mdns->register_address(Value.mdns_name, address, NowUs)) return;

   local_names.push_back({std::move(Value)});
}

//********************************************************************************************************************
// Adds a remote candidate to ICE.  An mDNS candidate is resolved first, and is added by pump_mdns() once the name has
// resolved.  Duplicate candidates are ignored, and at most MAX_REMOTE_NAMES names are tracked per generation.

ERR PeerConnection::accept_remote(ice::CandidateLine Value, uint64_t NowUs)
{
   if (not Value.ufrag.empty() and Value.ufrag != remote.data.ice_ufrag) return ERR::InvalidData;

   if (Value.mdns_name.empty()) {
      return socket->add_remote_candidate(Value.candidate) ? ERR::Okay : ERR::InvalidData;
   }

   auto text = ice::format_candidate(Value);
   for (const auto &name : remote_names) if (ice::format_candidate(name.line) IS text) return ERR::Okay;
   if (remote_names.size() >= MAX_REMOTE_NAMES) return ERR::BufferOverflow;

   bool duplicate = std::any_of(remote_names.begin(), remote_names.end(), [&](const RemoteName &Name) {
      return Name.line.mdns_name IS Value.mdns_name;
   });

   // A name already completed in this generation can be used for another port only with a fresh scoped lookup.

   if (not duplicate or std::all_of(remote_names.begin(), remote_names.end(), [&](const RemoteName &Name) {
       return Name.line.mdns_name != Value.mdns_name or Name.done;
   })) {
      if (not mdns or not mdns->resolve(Value.mdns_name, NowUs)) {
         remote_names.push_back({std::move(Value), true});
         return ERR::Okay; // Unavailable multicast is terminal for this candidate, not for a usable relay.
      }
   }

   remote_names.push_back({std::move(Value)});
   return ERR::Okay;
}

//********************************************************************************************************************
// Ends remote candidates once signalling has ended them and every mDNS name has resolved or failed.

void PeerConnection::finish_remote()
{
   if (remote_end and std::all_of(remote_names.begin(), remote_names.end(), [](const RemoteName &Name) {
       return Name.done;
   })) socket->end_remote_candidates();
}

//********************************************************************************************************************
// Services mDNS.  Local names that complete registration are admitted to ICE and signalled; a name that later fails
// is withdrawn.  Resolved remote names are added to ICE as candidates.  Signalling callbacks may close the connection
// or restart ICE, so the loop stops if either occurs.

void PeerConnection::pump_mdns(uint64_t NowUs)
{
   if (not mdns or not socket) return;

   mdns->tick(NowUs);
   auto registrations = mdns->engine.registrations();
   auto generation = mdns_generation;

   for (auto &name : local_names) {
      auto registration = std::find_if(registrations.begin(), registrations.end(), [&](const auto &Value) {
         return Value.name IS name.line.mdns_name;
      });
      if (registration IS registrations.end() or registration->status IS net::mdns::Status::PROBING) continue;

      if (registration->status IS net::mdns::Status::FAILED) {
         if (not name.failed) {
            name.failed = true;
            if (name.reported) {
               socket->retire_host(name.line.candidate.endpoint);
               std::erase(local.data.candidates, ice::format_candidate(name.line));
               local_changed();
               refresh_local();
            }
         }
         continue;
      }

      if (name.reported or name.failed) continue;

      if (not socket->admit_host(name.line.candidate)) {
         name.failed = true;
         continue;
      }

      name.reported = true;
      auto text = ice::format_candidate(name.line);
      local.data.candidates.push_back(text);
      local_changed();
      refresh_local();
      if (callbacks.candidate) callbacks.candidate(text);
      if (current_state IS PeerState::CLOSED or not socket or generation != mdns_generation) return;
   }

   for (const auto &result : mdns->engine.take_results()) {
      for (auto &name : remote_names) {
         if (name.done or name.line.mdns_name != result.name) continue;
         name.done = true;
         name.addresses = result.addresses;
         for (const auto &address : name.addresses) {
            if ((policy & POLICY_DISABLE_IPV6) and address.ipv6) continue;
            auto candidate = name.line.candidate;
            candidate.endpoint.address = address.bytes;
            candidate.endpoint.ipv6 = address.ipv6;

            // Original SDP remains the public identity; resolved endpoints only enter the ICE checklist.

            if (not socket->add_remote_candidate(candidate)) mdns_error = ERR::BufferOverflow;
         }
      }
   }

   if (remote_configured) finish_remote();
}

//********************************************************************************************************************
// Serialises the local description and records the revision that the text represents.  The description revision
// only advances if the text has changed, so that owners can skip redundant copies.  Returns false if serialisation
// failed.

bool PeerConnection::serialise_local()
{
   auto text = sdp::serialize(local);
   ++serialisations;
   serialised_revision = local_revision;

   if (text != local_text) {
      local_text = std::move(text);
      ++text_revision;
   }

   return not local_text.empty();
}

//********************************************************************************************************************
// Re-serialises a committed local description if it has changed since it was last serialised.  An uncommitted
// description is left as created, because SetLocalDescription() must receive the exact text that was returned.

void PeerConnection::refresh_local()
{
   if (local_committed and serialised_revision != local_revision) (void)serialise_local();
}

//********************************************************************************************************************

void PeerConnection::clear_local_text()
{
   if (local_text.empty()) return;
   local_text.clear();
   ++text_revision;
}

//********************************************************************************************************************

void PeerConnection::set_remote_text(std::string_view Text)
{
   if (Text IS remote_text) return;
   remote_text.assign(Text);
   ++text_revision;
}

//********************************************************************************************************************
// Creates an offer.  The offerer is the controlling ICE agent and offers actpass for the DTLS role.

ERR PeerConnection::create_offer()
{
   local.data.max_message_size = receive_limit;
   local_changed();

   if (current_state IS PeerState::CLOSED or signal_state != SignalingState::STABLE or
       not local_text.empty()) return ERR::InvalidState;

   auto error = ensure_socket(ice::Role::CONTROLLING, DtlsRole::CLIENT);
   if (error != ERR::Okay) return error;

   local.data.setup = sdp::Setup::ACTPASS;
   local_changed();
   return serialise_local() ? ERR::Okay : ERR::InvalidData;
}

//********************************************************************************************************************
// Responds to a change of host interfaces.  Every local route is retired.  If DTLS has connected, the application is
// asked to perform an ICE restart, which must succeed within 30 seconds; otherwise the connection fails.

void PeerConnection::network_changed(uint64_t NowUs)
{
   if (not socket or current_state IS PeerState::CLOSED or current_state IS PeerState::FAILED) return;

   // Conservative invalidation: wildcard-bound TURN transports may have followed a removed OS route.
   // Do not reuse their allocations or permissions after an interface change.

   socket->reset_routes();
   cancel_servers();
   release_lookup();
   mdns.reset();
   local_names.clear();
   remote_names.clear();
   early_candidates.clear();
   local.data.candidates.clear();
   local_changed();
   local_sockets_ready = false;

   if (socket->state() != DtlsState::CONNECTED) {
      // Initial DTLS negotiation has no retained session to recover through the supported restart contract.
      mdns_error = ERR::Disconnected;
      current_state = PeerState::FAILED;
      terminate_channels(mdns_error);
      if (callbacks.state) callbacks.state(current_state);
      wake();
      return;
   }

   if (not needs_restart) {
      needs_restart = true;
      restart_request_generation = mdns_generation;
      recovery_started = NowUs;
   }

   wake();
}

//********************************************************************************************************************
// Starts a new ICE generation with the given credentials.  All routes are re-gathered, while the DTLS session and
// SCTP association are retained.

ERR PeerConnection::restart_routes(std::string Ufrag, std::string Password)
{
   cancel_servers();
   release_lookup();
   mdns.reset();
   local_names.clear();
   remote_names.clear();
   early_candidates.clear();
   ++mdns_generation;
   remote_end = false;
   remote_configured = false;
   mdns_error = ERR::Okay;
   stun_lookup_error = ERR::Okay;

   socket->reset_routes();
   socket->restart_ice(Ufrag, Password, socket->ice_role());
   local_ufrag = std::move(Ufrag);
   local_password = std::move(Password);
   local.data.candidates.clear();
   local_changed();
   sctp_path_reset = true;

   auto error = gather_routes();
   if (error != ERR::Okay and current_state != PeerState::CLOSED) {
      mdns_error = error;
      current_state = PeerState::FAILED;
      if (association) association->abort(error);
      terminate_channels(error);
      if (callbacks.state) callbacks.state(current_state);
      wake();
   }

   return error;
}

//********************************************************************************************************************
// Creates an ICE restart offer.  Signalling must be stable and DTLS connected.

ERR PeerConnection::restart_ice()
{
   if (current_state IS PeerState::CLOSED or signal_state != SignalingState::STABLE or
       not local_committed or not remote_committed or not socket or
       (socket->socket_error() != ERR::Okay and not needs_restart) or
       socket->state() != DtlsState::CONNECTED) return ERR::InvalidState;

   auto ufrag = random_ice_text(8);
   auto password = random_ice_text(32);
   if (ufrag.empty() or password.empty()) return ERR::Failed;

   auto error = restart_routes(std::move(ufrag), std::move(password));
   if (error != ERR::Okay) return error;

   local.data.ice_ufrag = local_ufrag;
   local.data.ice_pwd = local_password;
   local.data.setup = sdp::Setup::ACTPASS;
   ++local.session_version;
   local_changed();
   auto serialised = serialise_local();
   local_committed = false;
   remote_committed = false;
   ice_started = false;
   update_state();
   return serialised ? ERR::Okay : ERR::InvalidData;
}

//********************************************************************************************************************
// Creates an answer to the remote offer.  The answerer is the controlled ICE agent.  After an ICE restart the
// existing DTLS role is retained.

ERR PeerConnection::create_answer(bool Active)
{
   local.data.max_message_size = receive_limit;
   local_changed();

   if (current_state IS PeerState::CLOSED or signal_state != SignalingState::HAVE_REMOTE_OFFER or
       not local_text.empty()) return ERR::InvalidState;

   auto error = ensure_socket(ice::Role::CONTROLLED, Active ? DtlsRole::CLIENT : DtlsRole::SERVER);
   if (error != ERR::Okay) return error;

   if (socket->state() IS DtlsState::CONNECTED) Active = socket->dtls_role() IS DtlsRole::CLIENT;
   local.data.mid = remote.data.mid;
   local.data.setup = Active ? sdp::Setup::ACTIVE : sdp::Setup::PASSIVE;
   local_changed();
   return serialise_local() ? ERR::Okay : ERR::InvalidData;
}

//********************************************************************************************************************
// Commits the local description, which must be the text returned by the last offer or answer.

ERR PeerConnection::set_local_description(std::string_view Text)
{
   if (current_state IS PeerState::CLOSED or local_text.empty() or Text != local_text or local_committed) {
      return ERR::InvalidState;
   }

   local_committed = true;

   // Keep the created description usable until committed, then expose all candidates gathered so far.

   refresh_local();
   if (local.data.setup IS sdp::Setup::ACTPASS) signal_state = SignalingState::HAVE_LOCAL_OFFER;
   else signal_state = SignalingState::STABLE;

   if (remote_committed) return configure_remote();
   return ERR::Okay;
}

//********************************************************************************************************************
// Applies a remote offer or answer.  A remote offer that changes only the ICE credentials of a connected session is
// treated as an ICE restart.  An answer must be compatible with our offer, and after a restart it must retain the
// existing DTLS parameters.

ERR PeerConnection::set_remote_description(std::string_view Text)
{
   if (current_state IS PeerState::CLOSED) return ERR::InvalidState;

   sdp::Description parsed;
   if (not sdp::parse(Text, parsed)) return ERR::InvalidData;

   // Validate the usable candidate budget before changing signalling state or configuring the ICE agent.

   std::vector<std::string> endpoints;
   for (const auto &line : parsed.data.candidates) {
      ice::CandidateLine candidate;
      if (not ice::parse_candidate(line, candidate)) continue;
      auto identity = ice::format_candidate(candidate);
      if (std::find(endpoints.begin(), endpoints.end(), identity) != endpoints.end()) continue;
      if (endpoints.size() >= 128) return ERR::InvalidData;
      endpoints.push_back(std::move(identity));
   }

   if (remote_committed and signal_state IS SignalingState::STABLE and
       ((parsed.data.ice_ufrag != remote.data.ice_ufrag) != (parsed.data.ice_pwd != remote.data.ice_pwd))) {
      return ERR::InvalidData; // An ICE restart must replace both credentials.
   }

   // Remote ICE restart

   if (remote_committed and signal_state IS SignalingState::STABLE and
       socket and socket->state() IS DtlsState::CONNECTED and
       parsed.data.setup IS sdp::Setup::ACTPASS and
       parsed.data.mid IS remote.data.mid and parsed.data.fingerprint IS remote.data.fingerprint and
       parsed.data.sctp_port IS remote.data.sctp_port and
       (parsed.data.ice_ufrag != remote.data.ice_ufrag or parsed.data.ice_pwd != remote.data.ice_pwd)) {
      auto ufrag = random_ice_text(8);
      auto password = random_ice_text(32);
      if (ufrag.empty() or password.empty()) return ERR::Failed;

      auto error = restart_routes(std::move(ufrag), std::move(password));
      if (error != ERR::Okay) return error;

      local.data.ice_ufrag = local_ufrag;
      local.data.ice_pwd = local_password;
      local.data.mid = parsed.data.mid;
      ++local.session_version;
      local_changed();
      clear_local_text();
      local_committed = false;
      ice_started = false;
      signal_state = SignalingState::HAVE_REMOTE_OFFER;
      remote = std::move(parsed);
      set_remote_text(Text);
      update_state();
      return ERR::Okay;
   }

   if (remote_committed) return ERR::InvalidState;

   if (signal_state IS SignalingState::HAVE_LOCAL_OFFER) { // Answer
      if (not sdp::validate_answer(local, parsed)) return ERR::InvalidData;
      if (socket->state() IS DtlsState::CONNECTED) {
         auto expected_setup = socket->dtls_role() IS DtlsRole::CLIENT ? sdp::Setup::PASSIVE : sdp::Setup::ACTIVE;
         if (parsed.data.fingerprint != remote.data.fingerprint or
             parsed.data.sctp_port != remote.data.sctp_port or
             parsed.data.setup != expected_setup or parsed.data.ice_ufrag IS remote.data.ice_ufrag or
             parsed.data.ice_pwd IS remote.data.ice_pwd) return ERR::InvalidData;
      }
      else if (parsed.data.setup IS sdp::Setup::ACTIVE) {
         auto error = socket->set_dtls_role(DtlsRole::SERVER);
         if (error != ERR::Okay) return error;
      }
   }
   else if (signal_state IS SignalingState::STABLE and local_text.empty()) { // Offer
      if (parsed.data.setup != sdp::Setup::ACTPASS) return ERR::InvalidData;
      signal_state = SignalingState::HAVE_REMOTE_OFFER;
   }
   else return ERR::InvalidState;

   remote_end = parsed.data.end_of_candidates;
   remote = std::move(parsed);
   set_remote_text(Text);
   remote_committed = true;
   if (local_committed) return configure_remote();
   return ERR::Okay;
}

//********************************************************************************************************************
// Configures the transport from the committed remote description and starts ICE.  Configuration is deferred while
// gathering is in progress, and is completed from tick().

ERR PeerConnection::configure_remote()
{
   if (not socket or ice_started) return ERR::InvalidState;

   if (gather_state IS GatheringState::GATHERING) {
      signal_state = SignalingState::STABLE;
      return ERR::Okay;
   }

   auto error = socket->set_remote_fingerprint(remote.data.fingerprint);
   if (error != ERR::Okay) return error;

   if (not socket->set_remote_credentials(remote.data.ice_ufrag, remote.data.ice_pwd, remote.ice_lite)) {
      return ERR::InvalidData;
   }

   remote_configured = true;

   for (const auto &line : remote.data.candidates) {
      ice::CandidateLine candidate;
      if (not ice::parse_candidate(line, candidate)) continue;
      auto accepted = accept_remote(std::move(candidate), now_us());
      if (accepted != ERR::Okay) return accepted;
   }

   for (auto &candidate : early_candidates) {
      auto accepted = accept_remote(std::move(candidate), now_us());
      if (accepted != ERR::Okay) return accepted;
   }

   early_candidates.clear();
   remote_end = remote_end or remote.data.end_of_candidates;
   finish_remote();

   if (not socket->start_ice(now_us())) {
      // No registered host or relay remains.  Fail without exposing a numeric fallback.
      mdns_error = ERR::Timeout;
      current_state = PeerState::FAILED;
      terminate_channels(mdns_error);
      if (callbacks.state) callbacks.state(current_state);
      return mdns_error;
   }

   ice_started = true;
   signal_state = SignalingState::STABLE;
   update_state();
   return ERR::Okay;
}

//********************************************************************************************************************
// Adds a trickled remote candidate, or ends remote candidates.  Candidates that arrive before the remote description
// has been configured are held until it is.

ERR PeerConnection::add_ice_candidate(std::string_view Text)
{
   if (not remote_committed or not socket) return ERR::InvalidState;

   if (Text.empty() or Text IS "end-of-candidates") {
      remote_end = true;
      if (remote_configured) finish_remote();
      return ERR::Okay;
   }

   if (remote_end) return ERR::InvalidState;
   bool udp = false;
   if (not sdp::valid_candidate_line(Text, udp)) return ERR::InvalidData;
   if (not udp) return ERR::Okay; // Valid but unsupported, e.g. TCP

   ice::CandidateLine candidate;
   if (not ice::parse_candidate(Text, candidate)) return ERR::InvalidData;
   if (not candidate.ufrag.empty() and candidate.ufrag != remote.data.ice_ufrag) return ERR::InvalidData;

   if (not remote_configured) {
      if (early_candidates.size() >= 128) return ERR::BufferOverflow;
      early_candidates.push_back(std::move(candidate));
      return ERR::Okay;
   }

   return accept_remote(std::move(candidate), now_us());
}

//********************************************************************************************************************
// Derives the connection state from ICE, DTLS and SCTP, and reports changes through the state callbacks.  A failed
// connection aborts the association and closes every channel.

void PeerConnection::update_state()
{
   if (current_state IS PeerState::CLOSED or not socket) return;

   auto next_ice = socket->ice_state();
   if (next_ice != current_ice_state) {
      current_ice_state = next_ice;
      if (callbacks.ice_state) callbacks.ice_state(next_ice);
      if (current_state IS PeerState::CLOSED or not socket) return;
   }

   PeerState next = PeerState::NEW;
   if ((association and association->state() IS SctpState::FAILED) or sctp_input_overflow or
       socket->socket_error() != ERR::Okay or socket->state() IS DtlsState::FAILED or
       socket->state() IS DtlsState::CLOSED or
       next_ice IS ice::State::FAILED) next = PeerState::FAILED;
   else if (next_ice IS ice::State::DISCONNECTED) next = PeerState::DISCONNECTED;
   else if (socket->state() IS DtlsState::CONNECTED and
       (next_ice IS ice::State::CONNECTED or next_ice IS ice::State::COMPLETED)) next = PeerState::CONNECTED;
   else if (ice_started or socket->state() IS DtlsState::CONNECTED) next = PeerState::CONNECTING;

   if (next IS PeerState::CONNECTED and needs_restart and mdns_generation > restart_request_generation) {
      needs_restart = false;
      recovery_started = 0;
   }

   if (next IS PeerState::FAILED) terminate_channels(ERR::Failed);

   if (next IS PeerState::FAILED and association and association->state() != SctpState::FAILED) {
      association->abort();
      sctp_input.clear();
      sctp_input_bytes = 0;
   }

   if (next != current_state) {
      current_state = next;
      if (callbacks.state) callbacks.state(next);
   }
}

//********************************************************************************************************************
// Drives the connection: interface monitoring, restart recovery, mDNS, the transport timers, gathering and SCTP.
// Newly gathered server-reflexive and relayed candidates are signalled here.  Gathering completes when every job has
// finished, or after 30 seconds.

ERR PeerConnection::tick(uint64_t NowUs)
{
   if (current_state IS PeerState::CLOSED) return ERR::InvalidState;
   if (current_state IS PeerState::FAILED) return mdns_error != ERR::Okay ? mdns_error : stun_lookup_error;

   if (interfaces and interfaces->poll(NowUs)) network_changed(NowUs);

   if (needs_restart and NowUs - recovery_started >= 30000000) {
      mdns_error = ERR::Timeout;
      current_state = PeerState::FAILED;
      terminate_channels(ERR::Timeout);
      if (association) association->abort(ERR::Timeout);
      if (callbacks.state) callbacks.state(current_state);
      return ERR::Timeout;
   }

   pump_mdns(NowUs);
   if (current_state IS PeerState::CLOSED) return ERR::InvalidState;

   if (mdns_error != ERR::Okay) {
      current_state = PeerState::FAILED;
      terminate_channels(mdns_error);
      if (callbacks.state) callbacks.state(current_state);
      return mdns_error;
   }

   if (stun_lookup_pending and gathering_started and NowUs - gathering_started >= 5000000) {
      release_lookup();
      stun_lookup_error = ERR::Timeout;
   }

   if (socket and (ice_started or gather_state IS GatheringState::GATHERING)) {
      auto error = socket->handle_timeout(NowUs);
      if (error != ERR::Okay) {
         if (association) association->abort(error);
         terminate_channels(error);
         update_state();
         return error;
      }

      if (gather_state IS GatheringState::GATHERING) gather_servers(NowUs);

      for (const auto &candidate : socket->take_candidates()) {
         ice::CandidateLine line;
         line.foundation = "g" + std::to_string(local.data.candidates.size() + 1);
         line.candidate = candidate;

         // Related addresses are omitted: suppressed base addresses must not leak through relay/srflx SDP.

         auto text = ice::format_candidate(line);
         if (text.empty()) return ERR::Failed;
         local.data.candidates.push_back(text);
         local_changed();
         if (callbacks.candidate) callbacks.candidate(text);
         if (not socket or current_state IS PeerState::CLOSED) return ERR::InvalidState;
      }

      bool jobs_done = std::all_of(server_jobs.begin(), server_jobs.end(),
         [](const auto &Job) { return Job->dispatched; });

      if (gather_state IS GatheringState::GATHERING and ((jobs_done and not stun_lookup_pending and
          not socket->gathering_pending() and (not mdns or not mdns->engine.pending())) or
          NowUs - gathering_started >= 30000000)) {
         gather_state = GatheringState::COMPLETE;
         local.data.end_of_candidates = true;
         local_changed();
         cancel_servers();
         socket->finish_gathering();
         if (socket->gathering_error() != ERR::Okay) stun_lookup_error = socket->gathering_error();
         refresh_local();

         if (local.data.candidates.empty() and not (policy & (POLICY_NO_HOST | POLICY_RELAY_ONLY | POLICY_NO_MDNS))) {
            mdns_error = ERR::Timeout;
            current_state = PeerState::FAILED;
            terminate_channels(mdns_error);
            if (callbacks.state) callbacks.state(current_state);
            return mdns_error;
         }

         if ((policy & POLICY_RELAY_ONLY) and socket->relay_error() != ERR::Okay) {
            stun_lookup_error = socket->relay_error();
            current_state = PeerState::FAILED;
            terminate_channels(stun_lookup_error);
            if (callbacks.state) callbacks.state(current_state);
            return stun_lookup_error;
         }
      }
      else refresh_local();

      if (gather_state IS GatheringState::COMPLETE and local_committed and remote_committed and not ice_started) {
         auto configured = configure_remote();
         if (configured != ERR::Okay) return configured;
      }

      if (socket and current_state != PeerState::CLOSED) {
         auto sctp_error = pump_sctp(NowUs);
         if (sctp_error != ERR::Okay) {
            if (association) association->abort(sctp_error);
            terminate_channels(sctp_error);
         }
         update_state();
         if (sctp_error != ERR::Okay) return sctp_error;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Drives SCTP once DTLS has connected and ICE has a selected pair.  The association is created on first use and is
// informed when the selected path changes.  Received records are fed within a budget of PUMP_INPUT_CHUNKS per pump,
// and output is sent while ICE remains connected.  Pumping pauses while channel events or transport output are
// backed up.

ERR PeerConnection::pump_sctp(uint64_t NowUs)
{
   if (sctp_input_overflow) return ERR::BufferOverflow;
   if (channel_events.size() > EVENT_BACKPRESSURE) return ERR::Okay;

   // At most MAX_OUTPUT SCTP packets are emitted per pump.  Leave accepted messages in the association under stream
   // pressure.

   if (socket->output_room() < PUMP_OUTPUT_ROOM) return ERR::Okay;
   if (not local_committed or not remote_committed or socket->state() != DtlsState::CONNECTED) return ERR::Okay;

   if (not socket->selected_remote() or not socket->selected_local() or
       (socket->ice_state() != ice::State::CONNECTED and socket->ice_state() != ice::State::COMPLETED)) {
      return ERR::Okay;
   }

   if (not association) {
      SctpConfig config;
      config.local_port = local.data.sctp_port;
      config.remote_port = remote.data.sctp_port;
      config.packet_capacity = size_t(socket->max_payload());
      config.receive_limit = receive_limit;
      association = std::make_unique<SctpAssociation>(config);
      auto error = association->start(NowUs);
      if (error != ERR::Okay) return error;
   }

   if (auto remote_path = socket->selected_remote(); remote_path and
       (sctp_path_reset or not sctp_path or *sctp_path != *remote_path or not sctp_local_path or
        (socket->selected_local() and *sctp_local_path != *socket->selected_local()))) {
      sctp_path_reset = false;
      sctp_path = *remote_path;
      if (socket->selected_local()) sctp_local_path = *socket->selected_local();
      auto error = association->path_changed(size_t(socket->max_payload()), NowUs);
      if (error != ERR::Okay) return error;
   }

   size_t received_chunks = 0;
   while (not sctp_input.empty()) {
      auto &packet = sctp_input.front();
      auto count = sctp::chunk_count(packet);
      if (received_chunks + count > PUMP_INPUT_CHUNKS) break;
      received_chunks += count;
      auto error = association->feed(packet, NowUs);
      sctp_input_bytes -= packet.size();
      sctp_input.pop_front();
      if (error != ERR::Okay) return error;
   }

   if (channel_mode and not channels and association->state() IS SctpState::ESTABLISHED) {
      channels = std::make_unique<SctpChannels>(*association, socket->dtls_role() IS DtlsRole::CLIENT);

      // Reserve explicit IDs before automatic allocation so creation order cannot steal a reservation.

      for (bool explicit_id : {true, false}) for (auto &[handle, channel] : public_channels) {
         if (channel.state != SctpChannelState::CONNECTING or channel.options.id.has_value() != explicit_id) continue;
         uint16_t id;
         auto error = channels->open(channel.options, NowUs, id);
         if (error IS ERR::Okay) {
            channel.id = id;
            stream_handles[id] = handle;
         }
         else {
            channel.state = SctpChannelState::CLOSED;
            channel.error = error;
            channel_events.push_back({handle, SctpChannelEventType::ERROR, error});
            channel_events.push_back({handle, SctpChannelEventType::CLOSE});
         }
      }
   }

   auto error = association->tick(NowUs);
   if (channels) {
      auto channel_error = channels->pump(NowUs);
      collect_channel_events();
      if (error IS ERR::Okay) error = channel_error;
   }

   if (error != ERR::Okay) {
      terminate_channels(error);
      return error;
   }

   // Keep the association paused during ICE reselection; retransmittable DATA remains charged.

   if (socket->ice_state() IS ice::State::CONNECTED or socket->ice_state() IS ice::State::COMPLETED) {
      for (auto &packet : association->take_output()) {
         error = socket->send(packet);
         if (error != ERR::Okay) {
            association->abort(error);
            terminate_channels(error);
            return error;
         }
         association->output_accepted(packet);
      }
   }

   refresh_buffered();

   // Channel state has been collected; association state records need no separate public callback.

   (void)association->take_events();
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the time until tick() next needs to be called, or -1 if no timer is armed.

int64_t PeerConnection::timeout_us(uint64_t NowUs) const
{
   auto due = socket ? socket->timeout_us(NowUs) : -1;

   // Poll the bounded internal multicast service on the owner task, including while connected.

   if (mdns) due = due < 0 ? 10000 : std::min(due, int64_t(10000));
   if (not channel_events.empty() or (not sctp_input.empty() and socket and socket->selected_remote())) due = 0;

   if (channels) {
      if (auto deadline = channels->next_deadline_us()) {
         auto delay = *deadline > NowUs ? int64_t(*deadline - NowUs) : 0;
         if (due < 0 or delay < due) due = delay;
      }
   }

   if (association and socket and socket->selected_remote() and (socket->ice_state() IS ice::State::CONNECTED or
       socket->ice_state() IS ice::State::COMPLETED)) {
      if (auto deadline = association->next_deadline_us()) {
         auto delay = *deadline > NowUs ? int64_t(std::min(*deadline - NowUs, uint64_t(INT64_MAX))) : 0;
         if (due < 0 or delay < due) due = delay;
      }
   }

   return due;
}

//********************************************************************************************************************
// Sends a raw SCTP message, bypassing the channel layer.  The remote maximum message size is enforced.

ERR PeerConnection::send_message(uint16_t Stream, uint32_t PPID, std::span<const uint8_t> Payload, bool Unordered)
{
   if (not association or current_state != PeerState::CONNECTED) return ERR::InvalidState;

   auto limit = remote.data.max_message_size;
   if (Payload.size() > 16777216 or (limit and Payload.size() > limit)) return ERR::OutOfRange;
   return association->enqueue(Stream, PPID, Payload, Unordered, now_us());
}

//********************************************************************************************************************

std::vector<SctpMessage> PeerConnection::take_messages()
{
   return association ? association->take_messages() : std::vector<SctpMessage>{};
}

//********************************************************************************************************************
// Closes the connection immediately.  Transport counters are retained so that stats() remains valid.

void PeerConnection::close()
{
   cancel_servers();
   interfaces.reset();
   needs_restart = false;
   recovery_started = 0;
   if (current_state IS PeerState::CLOSED) return;

   release_lookup();
   mdns.reset();
   local_names.clear();
   remote_names.clear();
   early_candidates.clear();
   local_sockets_ready = false;

   if (socket) {
      socket->reset_routes();
      closed_counters = socket->counters();
   }

   socket.reset();
   terminate_channels(ERR::Okay);
   channels.reset();
   association.reset();
   sctp_input.clear();
   sctp_input_bytes = 0;
   signal_state = SignalingState::CLOSED;
   current_state = PeerState::CLOSED;
   if (callbacks.state) callbacks.state(current_state);
}

//********************************************************************************************************************
// Sets the receive limit that is advertised as the local max-message-size.

ERR PeerConnection::set_receive_limit(uint32_t Limit)
{
   if (current_state != PeerState::NEW or socket) return ERR::InvalidState;
   if (not Limit or Limit > 16777216) return ERR::OutOfRange;
   receive_limit = Limit;
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the maximum size of an outbound message.  A remote max-message-size of zero means unlimited (RFC 8841), and
// is capped at 16 MiB.

uint64_t PeerConnection::max_message_size() const
{
   if (not remote_committed) return 0;
   return remote.data.max_message_size ? std::min(uint64_t(16777216), remote.data.max_message_size) : 16777216;
}

//********************************************************************************************************************
// Public channel records.  Each WebChannel refers to its record by a handle that is never reused, whereas SCTP stream
// IDs can be reused once a stream has been reset.  stream_handles maps each active stream to its current handle.

const PublicChannel * PeerConnection::channel(uint64_t Handle) const
{
   auto it = public_channels.find(Handle);
   return it IS public_channels.end() ? nullptr : &it->second;
}

//********************************************************************************************************************
// Creates a channel.  Before the association is established, the channel is reserved and opened by pump_sctp().

ERR PeerConnection::create_channel(const SctpChannelOptions &Options, uint64_t &Handle)
{
   if (current_state IS PeerState::CLOSED or current_state IS PeerState::FAILED) return ERR::InvalidState;

   auto error = dcep::validate(Options);
   if (error != ERR::Okay) return error;

   size_t active = 0, label_bytes = Options.label.size() + Options.protocol.size();
   for (const auto &[handle, channel] : public_channels) {
      if (channel.state IS SctpChannelState::CLOSED) continue;
      ++active;
      label_bytes += channel.options.label.size() + channel.options.protocol.size();
      if (Options.id and channel.id IS int(*Options.id)) return ERR::InvalidValue;
   }

   if (active >= MAX_CHANNELS or label_bytes > MAX_LABEL_BYTES or channel_events.size() >= EVENT_ADMISSION) {
      return ERR::BufferOverflow;
   }

   PublicChannel channel;
   channel.options = Options;
   channel.id = Options.id ? int(*Options.id) : -1;

   if (channels) {
      uint16_t id;
      error = channels->open(Options, now_us(), id);
      if (error != ERR::Okay) return error;
      channel.id = id;
   }

   channel_mode = true;
   Handle = next_handle++;
   public_channels.emplace(Handle, std::move(channel));

   if (channels) {
      stream_handles[uint16_t(public_channels[Handle].id)] = Handle;
      collect_channel_events();
   }

   wake();
   return ERR::Okay;
}

//********************************************************************************************************************

ERR PeerConnection::send_channel(uint64_t Handle, std::span<const uint8_t> Payload, bool Text)
{
   auto it = public_channels.find(Handle);
   if (it IS public_channels.end() or it->second.detached or it->second.state != SctpChannelState::OPEN or
       not channels) return ERR::InvalidState;

   if (Payload.size() > max_message_size()) return ERR::OutOfRange;
   if (channel_events.size() >= EVENT_ADMISSION) return ERR::BufferOverflow;

   auto error = channels->send(uint16_t(it->second.id), Payload, Text, now_us());
   if (error IS ERR::Okay) {
      // Enqueueing can expire messages on any stream, so preserve low-water notifications for every channel.
      refresh_buffered();
      wake();
   }
   return error;
}

//********************************************************************************************************************
// Closes a channel.  A channel that has no stream yet is closed locally.

ERR PeerConnection::close_channel(uint64_t Handle)
{
   auto it = public_channels.find(Handle);
   if (it IS public_channels.end()) return ERR::InvalidState;

   auto &channel = it->second;
   if (channel.state IS SctpChannelState::CLOSED or channel.state IS SctpChannelState::CLOSING) return ERR::Okay;

   if (channels and stream_handles.contains(uint16_t(channel.id))) {
      auto error = channels->close(uint16_t(channel.id), now_us());
      if (error != ERR::Okay) return error;
      channel.state = SctpChannelState::CLOSING;
   }
   else {
      channel.state = SctpChannelState::CLOSED;
      channel.buffered = 0;
      channel_events.push_back({Handle, SctpChannelEventType::CLOSE});
   }

   wake();
   return ERR::Okay;
}

//********************************************************************************************************************
// Called when a WebChannel is freed.  The channel is closed and its events suppressed; the record is removed once
// the close has completed.

void PeerConnection::detach_channel(uint64_t Handle)
{
   auto it = public_channels.find(Handle);
   if (it IS public_channels.end()) return;

   (void)close_channel(Handle);
   it->second.detached = true;
   if (it->second.state IS SctpChannelState::CLOSED) public_channels.erase(it);
}

//********************************************************************************************************************

ERR PeerConnection::set_threshold(uint64_t Handle, size_t Value)
{
   if (Value > 16777216) return ERR::OutOfRange;

   auto it = public_channels.find(Handle);
   if (it IS public_channels.end()) return ERR::InvalidState;

   it->second.threshold = Value;
   return ERR::Okay;
}

//********************************************************************************************************************
// Returns the association's buffered amount for a channel that currently owns its stream.

size_t PeerConnection::channel_buffered(uint64_t Handle, const PublicChannel &Channel) const
{
   if (not association or Channel.id < 0 or Channel.state IS SctpChannelState::CLOSED) return 0;
   auto owner = stream_handles.find(uint16_t(Channel.id));
   if (owner IS stream_handles.end() or owner->second != Handle) return 0;
   return association->buffered_amount(uint16_t(Channel.id));
}

//********************************************************************************************************************
// Updates each channel's buffered amount, and queues a buffered-amount-low event (a MESSAGE event with low set) when
// an open channel crosses its threshold from above.

void PeerConnection::refresh_buffered()
{
   for (auto &[handle, channel] : public_channels) {
      auto amount = channel_buffered(handle, channel);

      if (channel.buffered > channel.threshold and amount <= channel.threshold and
          channel.state IS SctpChannelState::OPEN and not channel.detached) {
         channel_events.push_back({handle, SctpChannelEventType::MESSAGE, ERR::Okay, false, true});
      }

      channel.buffered = amount;
   }
}

//********************************************************************************************************************
// Moves events from the channel layer to the public event queue, creating records for remote channels.  Receive
// credit is retained until release_channel_events(), except for events that cannot be delivered.

void PeerConnection::collect_channel_events()
{
   for (auto &[handle, channel] : public_channels) {
      if (channel.id < 0 or channel.state IS SctpChannelState::CLOSED) continue;

      // A retained terminal object must never follow a later channel using the same stream ID.

      auto owner = stream_handles.find(uint16_t(channel.id));
      if (owner IS stream_handles.end() or owner->second != handle) continue;

      auto internal = channels->channel(uint16_t(channel.id));
      if (internal and internal->state IS SctpChannelState::CLOSING) channel.state = SctpChannelState::CLOSING;
   }

   for (auto &event : channels->take_events(true)) {
      if (event.type IS SctpChannelEventType::REMOTE_OPEN) {
         auto handle = next_handle++;
         PublicChannel channel;
         channel.options = std::move(event.options);
         channel.id = event.stream;
         public_channels.emplace(handle, std::move(channel));
         stream_handles[event.stream] = handle;
      }

      auto it = stream_handles.find(event.stream);
      if (it IS stream_handles.end()) {
         if (event.receive_credit) (void)association->release_message(event.stream, event.receive_credit);
         continue;
      }

      auto handle = it->second;
      auto &channel = public_channels.at(handle);
      if (event.type IS SctpChannelEventType::OPEN) channel.state = SctpChannelState::OPEN;
      if (event.type IS SctpChannelEventType::ERROR) channel.error = event.error;
      if (event.type IS SctpChannelEventType::CLOSE) {
         channel.state = SctpChannelState::CLOSED;
         channel.buffered = 0;
         stream_handles.erase(it);
      }

      if (not channel.detached) {
         channel_events.push_back({handle, event.type, event.error, event.text, false, std::move(event.payload),
            event.stream, event.receive_credit});
      }
      else {
         if (event.receive_credit) (void)association->release_message(event.stream, event.receive_credit);
         if (event.type IS SctpChannelEventType::CLOSE) public_channels.erase(handle);
      }
   }
}

//********************************************************************************************************************
// Closes every channel, reporting Error first unless it is ERR::Okay.

void PeerConnection::terminate_channels(ERR Error)
{
   for (auto &[handle, channel] : public_channels) {
      if (channel.state IS SctpChannelState::CLOSED) continue;

      channel.state = SctpChannelState::CLOSED;
      channel.buffered = 0;
      if (channel.detached) continue;

      channel.error = Error;
      if (Error != ERR::Okay) channel_events.push_back({handle, SctpChannelEventType::ERROR, Error});
      channel_events.push_back({handle, SctpChannelEventType::CLOSE});
   }
}

//********************************************************************************************************************
// Returns up to EVENT_BATCH pending channel events.  The caller must pass them to release_channel_events() after
// delivery.

std::vector<PublicChannelEvent> PeerConnection::take_channel_events()
{
   std::vector<PublicChannelEvent> result;
   for (size_t count = 0; count < EVENT_BATCH and not channel_events.empty(); ++count) {
      result.push_back(std::move(channel_events.front()));
      channel_events.pop_front();
   }
   return result;
}

//********************************************************************************************************************

void PeerConnection::release_channel_events(std::span<const PublicChannelEvent> Events)
{
   if (not association) return;

   for (const auto &event : Events) {
      if (event.receive_credit) (void)association->release_message(event.stream, event.receive_credit);
   }
}

//********************************************************************************************************************
// Builds the GetStats JSON document.  Endpoints of the selected route are reported only where signalling would
// already have revealed them, so mDNS hosts are named and private addresses are reported as null.

std::string PeerConnection::stats() const
{
   auto counters = socket ? socket->counters() : closed_counters;
   std::string result = "{\"version\":1,\"timestampUs\":\"" + std::to_string(now_us()) +
      "\",\"generation\":\"" + std::to_string(mdns_generation) + "\",\"connection\":" + counters.json();

   auto local = socket ? socket->selected_local_candidate() : nullptr;
   auto remote = socket ? socket->selected_remote_candidate() : nullptr;
   auto route = socket ? socket->selected_counters() : nullptr;

   if (local and remote and route) {
      auto type = [](ice::CandidateType Type) {
         switch (Type) {
            case ice::CandidateType::HOST: return "host";
            case ice::CandidateType::SERVER_REFLEXIVE: return "srflx";
            case ice::CandidateType::PEER_REFLEXIVE: return "prflx";
            case ice::CandidateType::RELAY: return "relay";
         }
         return "unknown";
      };

      auto endpoint = [&](const ice::Candidate &Candidate, bool Local) -> std::string {
         const auto &value = Candidate.endpoint;
         std::string name;
         if (Local) {
            for (const auto &entry : local_names) {
               const auto &host = entry.line.candidate.endpoint;
               if (host.address IS value.address and host.ipv6 IS value.ipv6 and host.port IS value.port)
                  name = entry.line.mdns_name;
            }
         }
         else {
            for (const auto &entry : remote_names) for (const auto &address : entry.addresses) {
               if (address.bytes IS value.address and address.ipv6 IS value.ipv6 and
                   entry.line.candidate.endpoint.port IS value.port) name = entry.line.mdns_name;
            }
         }

         // Private numeric mappings and peer-reflexive endpoints have no public identity.

         if (Candidate.type IS ice::CandidateType::PEER_REFLEXIVE) return "null";
         if ((policy & POLICY_RELAY_ONLY) and Candidate.type != ice::CandidateType::RELAY) return "null";
         if (Candidate.type IS ice::CandidateType::HOST) {
            if (Local and (policy & POLICY_NO_HOST)) return "null";
            if (not (policy & POLICY_NO_MDNS) and name.empty()) return "null";
         }

         if (name.empty()) {
            char buffer[INET6_ADDRSTRLEN]{};
            if (not inet_ntop(value.ipv6 ? AF_INET6 : AF_INET, value.address.data(), buffer, sizeof(buffer)))
               return "null";
            name = buffer;
         }

         return "{\"address\":\"" + name + "\",\"port\":" + std::to_string(value.port) + "}";
      };

      auto allocation = socket->selected_allocation();
      auto client = allocation ? allocation->client_transport() : ServerTransport::UDP;
      std::string transport = allocation ? (client IS ServerTransport::UDP ? "\"udp\"" :
         client IS ServerTransport::TCP ? "\"tcp\"" : "\"tls\"") : "null";

      auto selected = *route;
      if (allocation) {
         selected.turn_sent_bytes = allocation->counters.turn_sent_bytes;
         selected.turn_received_bytes = allocation->counters.turn_received_bytes;
      }

      result += ",\"selectedRoute\":{\"id\":\"" + std::to_string(mdns_generation) + "-" +
         std::to_string(socket->selected_route_serial()) + "\",\"localType\":\"" + type(local->type) +
         "\",\"remoteType\":\"" + type(remote->type) + "\",\"localEndpoint\":" + endpoint(*local, true) +
         ",\"remoteEndpoint\":" + endpoint(*remote, false) + ",\"turnTransport\":" + transport + "},\"route\":" +
         selected.json() + ",\"iceRttUs\":" +
         (socket->selected_rtt_valid() ? std::to_string(socket->selected_rtt_us()) : "null");
   }
   else result += ",\"selectedRoute\":null,\"route\":null,\"iceRttUs\":null";

   if (association) {
      auto diagnostics = association->diagnostics();
      result += ",\"sctp\":{\"rtoUs\":" + std::to_string(diagnostics.rto_us) + ",\"cwndBytes\":" +
         std::to_string(diagnostics.congestion_window) + ",\"pathMtuBytes\":" +
         std::to_string(diagnostics.path_mtu) + ",\"retransmissions\":\"" +
         std::to_string(diagnostics.retransmissions) + "\"}";
   }
   else result += ",\"sctp\":null";

   return result + "}";
}

} // namespace rtc
