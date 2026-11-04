#include "socket_support.h"

#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/module.h>

#include "../peerconnection/peer_engine.h"
#include "../stun/stun.h"
#include "turn_adapter_support.h"


#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#ifndef _WIN32
#include <dlfcn.h>
#endif

JUMPTABLE_NETWORK
static OBJECTPTR glNetworkModule;
CSTRING ProgName = "WebRTCPeerConnectionTest";

using namespace rtc;
namespace {
class NumericPeerConnection : public rtc::PeerConnection {
public:
   NumericPeerConnection(PeerCallbacks Callbacks = {}) : rtc::PeerConnection(std::move(Callbacks)) {
      (void)set_policy(POLICY_NO_MDNS);
   }
};
}


static uint64_t now_us() {
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int run_test() {
   if (not rtc::test::turn_adapter_stall() or not rtc::test::turn_adapter_stall(false) or
       not rtc::test::turn_adapter_stall(false, true)) return 40;
   NumericPeerConnection *closing_pointer = nullptr;
   PeerCallbacks closing_callbacks;
   closing_callbacks.candidate = [&](std::string_view) { closing_pointer->close(); };
   NumericPeerConnection closing(closing_callbacks);
   closing_pointer = &closing;
   if (closing.create_offer() != ERR::InvalidState or closing.state() != PeerState::CLOSED) return 21;

   NumericPeerConnection *ice_closing_pointer = nullptr;
   PeerCallbacks ice_closing_callbacks;
   ice_closing_callbacks.ice_state = [&](ice::State) { ice_closing_pointer->close(); };
   NumericPeerConnection ice_closing(ice_closing_callbacks), ice_closing_answer;
   ice_closing_pointer = &ice_closing;
   if (ice_closing.create_offer() != ERR::Okay or
       ice_closing.set_local_description(ice_closing.local_description()) != ERR::Okay or
       ice_closing_answer.set_remote_description(ice_closing.local_description()) != ERR::Okay or
       ice_closing_answer.create_answer() != ERR::Okay or
       ice_closing.set_remote_description(ice_closing_answer.local_description()) != ERR::Okay or
       ice_closing.state() != PeerState::CLOSED) return 29;

   // Unsupported rejected sections must fail without committing negotiation state.
   NumericPeerConnection rejection_offer, rejection_answer;
   if (rejection_offer.create_offer() != ERR::Okay or
       rejection_offer.set_local_description(rejection_offer.local_description()) != ERR::Okay) return 23;
   NumericPeerConnection budget_answer;
   sdp::Description oversized_offer;
   if (not sdp::parse(rejection_offer.local_description(), oversized_offer)) return 33;
   oversized_offer.data.candidates.clear();
   for (int i = 0; i < 129; ++i) {
      oversized_offer.data.candidates.push_back("candidate:budget" + std::to_string(i) +
         " 1 udp 2122260223 203.0.113.1 " + std::to_string(10000 + i) + " typ host");
   }
   if (budget_answer.set_remote_description(sdp::serialize(oversized_offer)) != ERR::InvalidData or
       budget_answer.signaling_state() != SignalingState::STABLE or not budget_answer.remote_description().empty() or
       budget_answer.set_remote_description(rejection_offer.local_description()) != ERR::Okay or
       budget_answer.create_answer() != ERR::Okay) return 33;
   auto rejected = rejection_offer.local_description();
   auto media_port = rejected.find("m=application 9 ");
   if (media_port IS std::string::npos) return 23;
   rejected.replace(media_port, std::string_view("m=application 9 ").size(), "m=application 0 ");
   if (rejection_answer.set_remote_description(rejected) != ERR::InvalidData or
       rejection_answer.signaling_state() != SignalingState::STABLE or
       not rejection_answer.remote_description().empty() or
       rejection_answer.set_remote_description(rejection_offer.local_description()) != ERR::Okay or
       rejection_answer.create_answer() != ERR::Okay) return 23;
   rejected = rejection_answer.local_description();
   media_port = rejected.find("m=application 9 ");
   if (media_port IS std::string::npos) return 23;
   rejected.replace(media_port, std::string_view("m=application 9 ").size(), "m=application 0 ");
   if (rejection_offer.set_remote_description(rejected) != ERR::InvalidData or
       rejection_offer.signaling_state() != SignalingState::HAVE_LOCAL_OFFER or
       not rejection_offer.remote_description().empty() or
       rejection_offer.set_remote_description(rejection_answer.local_description()) != ERR::Okay) return 23;

   bool a_connected = false, b_connected = false;
   std::string a_candidate, b_candidate;
   PeerCallbacks a_callbacks, b_callbacks;
   a_callbacks.state = [&](PeerState State) { if (State IS PeerState::CONNECTED) a_connected = true; };
   b_callbacks.state = [&](PeerState State) { if (State IS PeerState::CONNECTED) b_connected = true; };
   a_callbacks.candidate = [&](std::string_view Value) { a_candidate.assign(Value); };
   b_callbacks.candidate = [&](std::string_view Value) { b_candidate.assign(Value); };
   // Hosts schedule tick() on demand; queued DTLS input must request a wake rather than await polling.
   unsigned a_wakes = 0, b_wakes = 0;
   a_callbacks.wake = [&]() { ++a_wakes; };
   b_callbacks.wake = [&]() { ++b_wakes; };
   NumericPeerConnection a(a_callbacks), b(b_callbacks);
   std::string mixed_offer;
   if (a.create_offer() != ERR::Okay) return 1;
   mixed_offer = a.local_description();
   auto end_candidates = mixed_offer.find("a=end-of-candidates\r\n");
   if (end_candidates IS std::string::npos) return 1;
   mixed_offer.insert(end_candidates,
      "a=candidate:tcp1 1 tcp 1518280447 192.0.2.10 9 typ host tcptype passive\r\n"
      "a=candidate:mdns1 1 udp 2122260223 peer.local 5000 typ host\r\n"
      "a=candidate:relay1 1 udp 16777215 192.0.2.11 6000 typ relay\r\n");
   auto first_candidate = mixed_offer.find("a=candidate:");
   if (first_candidate IS std::string::npos) return 1;
   auto first_candidate_end = mixed_offer.find("\r\n", first_candidate);
   if (first_candidate_end IS std::string::npos) return 1;
   auto duplicate_candidate = mixed_offer.substr(first_candidate, first_candidate_end - first_candidate + 2);
   end_candidates = mixed_offer.find("a=end-of-candidates\r\n");
   mixed_offer.insert(end_candidates, duplicate_candidate);
   auto trickle_offer = mixed_offer;
   trickle_offer.erase(trickle_offer.find("a=end-of-candidates\r\n"),
      std::string_view("a=end-of-candidates\r\n").size());
   NumericPeerConnection trickle_answer;
   if (trickle_answer.set_remote_description(trickle_offer) != ERR::Okay or
       trickle_answer.create_answer() != ERR::Okay) return 45;
   if (trickle_answer.add_ice_candidate("candidate:tcp1 1 tcp 1518280447 192.0.2.10 9 typ host tcptype passive")
       != ERR::Okay) return 46;
   if (trickle_answer.add_ice_candidate("candidate:bad- 1 udp 2122260223 192.0.2.10 5000 typ host")
       != ERR::InvalidData) return 47;
   if (trickle_answer.add_ice_candidate("candidate:good 1 udp 2122260223 bad.invalid 5000 typ host")
       != ERR::InvalidData) return 48;
   if (trickle_answer.add_ice_candidate("candidate:good 1 udp 2122260223 192.0.2.10 5000 typ host raddr")
       != ERR::InvalidData) return 49;
   if (trickle_answer.add_ice_candidate("candidate:good 1 udp 2122260223 192.0.2.10 5000 typ host")
       != ERR::Okay) return 50;
   if (a_candidate.empty() or
       a.set_local_description(a.local_description()) != ERR::Okay or
       a.signaling_state() != SignalingState::HAVE_LOCAL_OFFER or
       b.set_remote_description(mixed_offer) != ERR::Okay or
       b.signaling_state() != SignalingState::HAVE_REMOTE_OFFER or
       b.create_answer() != ERR::Okay or b_candidate.empty() or
       b.set_local_description(b.local_description()) != ERR::Okay or
       a.set_remote_description(b.local_description()) != ERR::Okay) return 1;
   if (b.add_ice_candidate("candidate:bad") != ERR::InvalidState) return 1;
   if (b.add_ice_candidate("") != ERR::Okay or
       b.add_ice_candidate("candidate:late 1 udp 2122260223 192.0.2.12 5001 typ host") != ERR::InvalidState) {
      return 1;
   }
   if (a.signaling_state() != SignalingState::STABLE or
       b.signaling_state() != SignalingState::STABLE or
       a.gathering_state() != GatheringState::COMPLETE or
       b.gathering_state() != GatheringState::COMPLETE) return 2;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (a.tick(now) != ERR::Okay or b.tick(now) != ERR::Okay) return 3;
      if (a_connected and b_connected and
          a.ice_state() IS ice::State::COMPLETED and b.ice_state() IS ice::State::COMPLETED) break;
   }
   if (not a_connected or not b_connected or a.state() != PeerState::CONNECTED or
       b.state() != PeerState::CONNECTED or a.ice_state() != ice::State::COMPLETED or
       b.ice_state() != ice::State::COMPLETED) {
      std::fprintf(stderr, "Peer states %d/%d, ICE states %d/%d\n",
         int(a.state()), int(b.state()), int(a.ice_state()), int(b.ice_state()));
      return 4;
   }

   // Ticks after gathering has completed must not re-serialise an unchanged local description.

   auto idle_offer = a.local_description();
   auto idle_serialisations = a.local_serialisations();
   auto idle_revision = a.description_revision();
   for (int tick = 0; tick < 500; ++tick) {
      ProcessMessages(PMF::NIL, 0);
      if (a.tick(now_us()) != ERR::Okay) return 3;
   }
   auto idle_work = a.local_serialisations() - idle_serialisations;
   std::fprintf(stderr, "Local SDP serialisations over 500 idle ticks: %llu\n", (unsigned long long)idle_work);
   if (idle_work != 0 or a.description_revision() != idle_revision or a.local_description() != idle_offer or
       idle_offer.find("a=" + a_candidate + "\r\n") IS std::string::npos or
       idle_offer.find("a=end-of-candidates\r\n") IS std::string::npos) return 51;

   // Reliable SCTP uses real ICE/DTLS sockets and survives a restart with DATA outstanding.
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline and
       (a.sctp_state() != SctpState::ESTABLISHED or b.sctp_state() != SctpState::ESTABLISHED)) {
      ProcessMessages(PMF::NIL, 1);
      auto now = now_us();
      if (a.tick(now) != ERR::Okay or b.tick(now) != ERR::Okay) return 34;
   }
   if (a.sctp_state() != SctpState::ESTABLISHED or b.sctp_state() != SctpState::ESTABLISHED) return 34;
   std::vector<uint8_t> transfer(200000);
   for (size_t i = 0; i < transfer.size(); ++i) transfer[i] = uint8_t(i * 13 + i / 113);
   if (a.send_message(0, 53, transfer, false) != ERR::Okay) return 35;
   auto a_fingerprint = a.certificate_fingerprint();
   auto b_fingerprint = b.certificate_fingerprint();
   auto first_offer = a.local_description();
   auto first_answer = b.local_description();
   sdp::Description partial_restart;
   if (not sdp::parse(first_offer, partial_restart)) return 32;
   partial_restart.data.ice_ufrag = "newufrag";
   if (b.set_remote_description(sdp::serialize(partial_restart)) != ERR::InvalidData or
       b.state() != PeerState::CONNECTED or b.remote_description() != mixed_offer) return 32;
   auto lost_at = now_us();
   a.network_changed(lost_at);
   a.network_changed(lost_at + 1); // A burst retains one request and its original deadline.
   if (not a.restart_required() or a.restart_generation() != a.candidate_generation() or
       a.tick(lost_at + 2) != ERR::Okay or a.state() != PeerState::DISCONNECTED) return 41;
   if (a.restart_ice() != ERR::Okay or a.state() != PeerState::CONNECTING or
       a.ice_state() != ice::State::NEW or a.local_description() IS first_offer or
       a.certificate_fingerprint() != a_fingerprint or
       a.set_local_description(a.local_description()) != ERR::Okay or
       b.set_remote_description(a.local_description()) != ERR::Okay or
       b.state() != PeerState::CONNECTING or b.ice_state() != ice::State::NEW or
       b.create_answer() != ERR::Okay or b.local_description() IS first_answer or
       b.certificate_fingerprint() != b_fingerprint or
       b.set_local_description(b.local_description()) != ERR::Okay) return 22;
   sdp::Description tampered_restart_answer;
   if (not sdp::parse(b.local_description(), tampered_restart_answer)) return 22;
   tampered_restart_answer.data.fingerprint[0] =
      tampered_restart_answer.data.fingerprint[0] IS 'A' ? 'B' : 'A';
   if (a.set_remote_description(sdp::serialize(tampered_restart_answer)) != ERR::InvalidData or
       a.set_remote_description(b.local_description()) != ERR::Okay) return 22;
   a_connected = false;
   b_connected = false;
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      auto a_error = a.tick(now), b_error = b.tick(now);
      if (a_error != ERR::Okay or b_error != ERR::Okay) {
         std::fprintf(stderr, "Restart errors %d/%d SCTP %d/%d peer %d/%d\n", int(a_error), int(b_error),
            int(a.sctp_state()), int(b.sctp_state()), int(a.state()), int(b.state()));
         return 23;
      }
      if (a.ice_state() IS ice::State::COMPLETED and b.ice_state() IS ice::State::COMPLETED) break;
   }
   if (a.state() != PeerState::CONNECTED or b.state() != PeerState::CONNECTED or
       a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED or
       a.certificate_fingerprint() != a_fingerprint or b.certificate_fingerprint() != b_fingerprint) return 24;

   if (a.restart_required()) return 42;
   sdp::Description new_offer, old_offer;
   if (not sdp::parse(a.local_description(), new_offer) or not sdp::parse(first_offer, old_offer) or
       new_offer.data.candidates IS old_offer.data.candidates) return 43;
   // A stale trickle candidate must not be interpreted in the negotiated replacement generation.
   auto stale = old_offer.data.candidates.front();
   if (a.add_ice_candidate(stale) != ERR::InvalidState) return 44;

   std::vector<SctpMessage> transferred;
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
   while (std::chrono::steady_clock::now() < deadline and transferred.empty()) {
      ProcessMessages(PMF::NIL, 1);
      auto now = now_us();
      if (a.tick(now) != ERR::Okay or b.tick(now) != ERR::Okay) return 36;
      transferred = b.take_messages();
   }
   if (transferred.size() != 1 or transferred.front().payload != transfer or
       a.sctp_state() != SctpState::ESTABLISHED or b.sctp_state() != SctpState::ESTABLISHED) return 36;
   if (not a_wakes or not b_wakes) return 38;
   if (b.send_message(2, 53, transfer, true) != ERR::Okay) return 37;
   transferred.clear();
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
   while (std::chrono::steady_clock::now() < deadline and transferred.empty()) {
      ProcessMessages(PMF::NIL, 1);
      auto now = now_us();
      if (a.tick(now) != ERR::Okay or b.tick(now) != ERR::Okay) return 37;
      transferred = a.take_messages();
   }
   if (transferred.size() != 1 or transferred.front().payload != transfer or
       not transferred.front().unordered) return 37;

   // The original answerer may initiate a restart.  ICE and established DTLS roles survive the offerer swap.
   if (b.restart_ice() != ERR::Okay or b.set_local_description(b.local_description()) != ERR::Okay or
       a.set_remote_description(b.local_description()) != ERR::Okay or a.create_answer() != ERR::Okay or
       a.set_local_description(a.local_description()) != ERR::Okay or
       b.set_remote_description(a.local_description()) != ERR::Okay) return 30;
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (a.tick(now) != ERR::Okay or b.tick(now) != ERR::Okay) return 30;
      if (a.ice_state() IS ice::State::COMPLETED and b.ice_state() IS ice::State::COMPLETED) break;
   }
   if (a.state() != PeerState::CONNECTED or b.state() != PeerState::CONNECTED or
       a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED) return 30;
   a.close();
   b.close();
   if (a.state() != PeerState::CLOSED or b.state() != PeerState::CLOSED) return 5;

   NumericPeerConnection active_offer, active_answer;
   if (active_offer.create_offer() != ERR::Okay or
       active_offer.set_local_description(active_offer.local_description()) != ERR::Okay or
       active_answer.set_remote_description(active_offer.local_description()) != ERR::Okay or
       active_answer.create_answer(true) != ERR::Okay or
       active_answer.set_local_description(active_answer.local_description()) != ERR::Okay or
       active_offer.set_remote_description(active_answer.local_description()) != ERR::Okay) return 6;
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (active_offer.tick(now) != ERR::Okay or active_answer.tick(now) != ERR::Okay) return 7;
      if (active_offer.state() IS PeerState::CONNECTED and active_answer.state() IS PeerState::CONNECTED) break;
   }
   if (active_offer.state() != PeerState::CONNECTED or active_answer.state() != PeerState::CONNECTED) return 8;
   if (active_offer.restart_ice() != ERR::Okay or
       active_offer.set_local_description(active_offer.local_description()) != ERR::Okay or
       active_answer.set_remote_description(active_offer.local_description()) != ERR::Okay or
       active_answer.create_answer() != ERR::Okay) return 25;
   sdp::Description active_restart_answer;
   if (not sdp::parse(active_answer.local_description(), active_restart_answer) or
       active_restart_answer.data.setup != sdp::Setup::ACTIVE or
       active_answer.set_local_description(active_answer.local_description()) != ERR::Okay or
       active_offer.set_remote_description(active_answer.local_description()) != ERR::Okay) return 26;
   deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (active_offer.tick(now) != ERR::Okay or active_answer.tick(now) != ERR::Okay) return 27;
      if (active_offer.ice_state() IS ice::State::COMPLETED and
          active_answer.ice_state() IS ice::State::COMPLETED) break;
   }
   if (active_offer.state() != PeerState::CONNECTED or active_answer.state() != PeerState::CONNECTED or
       active_offer.ice_state() != ice::State::COMPLETED or
       active_answer.ice_state() != ice::State::COMPLETED) return 28;

   // A synthetic recovery clock can start at zero; the pending flag owns deadline validity.
   active_offer.network_changed(0);
   if (active_offer.tick(30000000) != ERR::Timeout or active_offer.state() != PeerState::FAILED) return 45;

   auto hosts = ice::gather_host_candidates(1, false, false);
   if (hosts.empty()) hosts = ice::gather_host_candidates(1, true, false);
   if (hosts.empty()) return 0; // IPv6-only hosts are covered by the candidate and agent tests.
   char host_text[INET_ADDRSTRLEN]{};
   if (not inet_ntop(AF_INET, hosts.front().endpoint.address.data(), host_text, sizeof(host_text))) return 9;
   auto stun_fd = rtc::test::udp_socket();
   if (stun_fd IS rtc::test::INVALID_SOCKET_VALUE) return 10;
   auto close_stun = kt::Defer([&]() { rtc::test::close_socket(stun_fd); });
   sockaddr_in stun_address{};
   stun_address.sin_family = AF_INET;
   stun_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (::bind(stun_fd, (sockaddr *)&stun_address, sizeof(stun_address)) != 0) return 11;
   rtc::test::AddressSize stun_size = sizeof(stun_address);
   if (::getsockname(stun_fd, (sockaddr *)&stun_address, &stun_size) != 0) return 12;
   // localhost may resolve to either family.  Serve both on the same port so DNS order is immaterial.
   auto stun_ipv6_fd = rtc::test::udp_socket(true);
   auto close_ipv6 = kt::Defer([&]() {
      if (stun_ipv6_fd != rtc::test::INVALID_SOCKET_VALUE) rtc::test::close_socket(stun_ipv6_fd);
   });
   if (stun_ipv6_fd != rtc::test::INVALID_SOCKET_VALUE) {
      sockaddr_in6 address{};
      address.sin6_family = AF_INET6;
      address.sin6_addr = in6addr_loopback;
      address.sin6_port = stun_address.sin_port;
      if (::bind(stun_ipv6_fd, (sockaddr *)&address, sizeof(address)) != 0) return 12;
   }
   std::vector<std::string> gathered;
   PeerCallbacks gather_callbacks;
   gather_callbacks.candidate = [&](std::string_view Value) { gathered.emplace_back(Value); };
   NumericPeerConnection gathering(gather_callbacks);
   if (gathering.set_stun_server("STUNS:localhost") != ERR::NoSupport or
       gathering.set_stun_server("stun://localhost") != ERR::InvalidData or
       gathering.set_stun_server("stun:localhost:0") != ERR::InvalidData or
       gathering.set_stun_server("STUN:localhost:" +
          std::to_string(ntohs(stun_address.sin_port))) != ERR::Okay or
       gathering.create_offer() != ERR::Okay or gathering.gathering_state() != GatheringState::GATHERING or
       gathering.tick(now_us()) != ERR::Okay or gathering.gathering_state() != GatheringState::GATHERING or
       gathered.empty()) return 13;
   auto host_count = gathered.size();
   auto pending_offer = gathering.local_description();
   sdp::Description gathered_offer;
   if (not sdp::parse(gathering.local_description(), gathered_offer) or
       gathered_offer.data.candidates.size() != host_count) return 13;

   std::array<uint8_t, 1500> buffer{};
   sockaddr_storage requester{};
   rtc::test::AddressSize requester_size = sizeof(requester);
   int received = -1;
   auto response_fd = stun_fd;
   auto gather_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while (std::chrono::steady_clock::now() < gather_deadline and received < 0) {
      received = rtc::test::receive_from(stun_fd, buffer.data(), buffer.size(), 0,
         (sockaddr *)&requester, &requester_size);
      if (received < 0 and stun_ipv6_fd != rtc::test::INVALID_SOCKET_VALUE) {
         requester_size = sizeof(requester);
         received = rtc::test::receive_from(stun_ipv6_fd, buffer.data(), buffer.size(), 0,
            (sockaddr *)&requester, &requester_size);
         if (received >= 0) response_fd = stun_ipv6_fd;
      }
      if (received < 0) ProcessMessages(PMF::NIL, 10);
   }
   stun::Message request;
   if (received <= 0 or not stun::parse(std::span<const uint8_t>(buffer.data(), size_t(received)), request)) return 14;
   std::vector<uint8_t> mapped{ 0, 1, uint8_t((50000 ^ 0x2112) >> 8), uint8_t(50000 ^ 0x2112),
      uint8_t(203 ^ 0x21), uint8_t(0 ^ 0x12), uint8_t(113 ^ 0xa4), uint8_t(9 ^ 0x42) };
   bool ipv6_response = requester.ss_family IS AF_INET6;
   if (ipv6_response) {
      mapped.resize(20);
      mapped[1] = 2;
      std::array<uint8_t, 16> address{ 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9 };
      constexpr std::array<uint8_t, 4> cookie{ 0x21, 0x12, 0xa4, 0x42 };
      for (size_t i = 0; i < address.size(); ++i) {
         mapped[4 + i] = address[i] ^ (i < 4 ? cookie[i] : request.transaction_id[i - 4]);
      }
   }
   stun::Attribute attribute{ stun::ATTR_XOR_MAPPED_ADDRESS, mapped, 0 };
   std::vector<uint8_t> response;
   if (not stun::encode(stun::BINDING_SUCCESS, request.transaction_id,
       std::span<const stun::Attribute>(&attribute, 1), {}, false, true, response) or
       rtc::test::send_to(response_fd, response.data(), response.size(), 0,
          (sockaddr *)&requester, requester_size) != int(response.size())) return 15;
   while (std::chrono::steady_clock::now() < gather_deadline and gathered.size() <= host_count) {
      ProcessMessages(PMF::NIL, 10);
      if (gathering.tick(now_us()) != ERR::Okay) return 16;
   }
   ice::CandidateLine reflexive;
   if (gathered.size() != host_count + 1 or not ice::parse_candidate(gathered.back(), reflexive) or
       reflexive.candidate.type != ice::CandidateType::SERVER_REFLEXIVE or
       reflexive.candidate.endpoint.port != 50000 or
       reflexive.candidate.endpoint.ipv6 != ipv6_response or
       reflexive.candidate.endpoint.address[0] != (ipv6_response ? 0x20 : 203) or
       gathering.gathering_state() != GatheringState::COMPLETE) {
      std::fprintf(stderr, "Gathered %zu of %zu candidates; last=%s; state=%d\n", gathered.size(),
         host_count + 1, gathered.empty() ? "" : gathered.back().c_str(), int(gathering.gathering_state()));
      return 17;
   }
   if (gathering.set_local_description(pending_offer) != ERR::Okay or
       not sdp::parse(gathering.local_description(), gathered_offer) or
       gathered_offer.data.candidates.size() != host_count + 1 or not gathered_offer.data.end_of_candidates) {
      return 31;
   }

   NumericPeerConnection unanswered;
   if (unanswered.set_stun_server(std::string(host_text) + ":" +
       std::to_string(ntohs(stun_address.sin_port))) != ERR::Okay or
       unanswered.create_offer() != ERR::Okay or
       unanswered.set_stun_server("127.0.0.1:3478") != ERR::InvalidState) return 18;
   auto simulated_now = now_us();
   for (int i = 1; i <= 8; ++i) {
      if (unanswered.tick(simulated_now + uint64_t(i) * 10000000u) != ERR::Okay) return 19;
   }
   if (unanswered.gathering_state() != GatheringState::COMPLETE) return 20;
   return 0;
}

#ifndef _WIN32
static int transition(const std::string &Config) {
   using Switch = void (*)(int, bool);
   auto block = (Switch)dlsym(RTLD_DEFAULT, "rtc_test_select_path");
   auto dropped = (unsigned (*)())dlsym(RTLD_DEFAULT, "rtc_test_dropped");
   auto rejected = (unsigned (*)())dlsym(RTLD_DEFAULT, "rtc_test_rejected");
   if (not block or not dropped or not rejected) return 50;
   auto cleanup = kt::Defer([&]() { block(0, false); });
   NumericPeerConnection a, b;
   if (a.set_policy(POLICY_NO_MDNS | POLICY_DISABLE_IPV6) != ERR::Okay or
       b.set_policy(POLICY_NO_MDNS | POLICY_DISABLE_IPV6) != ERR::Okay) return 59;
   uint64_t handles[2][7] = {};
   PeerConnection *peers[] = {&a, &b};
   for (unsigned mode = 0; mode < 7; ++mode) {
      SctpChannelOptions options;
      options.label = "migration-" + std::to_string(mode);
      options.negotiated = true; options.id = uint16_t(42 + mode * 2);
      options.ordered = mode < 3;
      if (mode < 6 and mode % 3 IS 1) options.reliability.max_retransmits = 2;
      if (mode < 6 and mode % 3 IS 2) options.reliability.max_lifetime_ms = 5000;
      if (mode IS 6) options.reliability.max_lifetime_ms = 100;
      for (unsigned side = 0; side < 2; ++side) {
         if (peers[side]->create_channel(options, handles[side][mode]) != ERR::Okay) return 60;
      }
   }
   unsigned received[2][6] = {};
   bool invalid = false;
   std::vector<uint8_t> payload(200000, 91), small(256, 92);
   auto drain = [&]() {
      for (unsigned side = 0; side < 2; ++side) {
         auto events = peers[side]->take_channel_events();
         for (const auto &event : events) {
            if (event.type IS SctpChannelEventType::ERROR) {
               std::fprintf(stderr, "Migration channel error: side=%u handle=%llu error=%d\n", side,
                  (unsigned long long)event.handle, int(event.error));
               invalid = true;
            }
            if (event.type != SctpChannelEventType::MESSAGE or event.low) continue;
            bool found = false;
            for (unsigned mode = 0; mode < 6; ++mode) if (event.handle IS handles[side][mode]) {
               found = true;
               if (event.text or event.payload != (mode IS 0 ? payload : small)) invalid = true;
               ++received[side][mode];
            }
            if (not found) {
               std::fprintf(stderr, "Migration unexpected message: side=%u handle=%llu bytes=%zu\n", side,
                  (unsigned long long)event.handle, event.payload.size());
               invalid = true; // Includes the message whose lifetime expired during path loss.
            }
         }
         peers[side]->release_channel_events(events);
      }
   };
   auto pump = [&]() {
      ProcessMessages(PMF::NIL, 1);
      if (a.tick(now_us()) != ERR::Okay or b.tick(now_us()) != ERR::Okay) return false;
      drain(); return not invalid;
   };
   auto gather = [&](PeerConnection &Peer) {
      auto deadline = now_us() + 10000000;
      while (now_us() < deadline and Peer.gathering_state() != GatheringState::COMPLETE) {
         if (not pump()) return false;
      }
      return Peer.gathering_state() IS GatheringState::COMPLETE;
   };
   auto connect = [&](bool Relay) {
      auto deadline = now_us() + 10000000;
      while (now_us() < deadline) {
         if (not pump()) return false;
         if (a.state() IS PeerState::CONNECTED and b.state() IS PeerState::CONNECTED and
             a.sctp_state() IS SctpState::ESTABLISHED and b.sctp_state() IS SctpState::ESTABLISHED and
             a.selected_relay() IS Relay and b.selected_relay() IS Relay) return true;
      }
      return false;
   };
   if (a.set_ice_servers(Config) != ERR::Okay or b.set_ice_servers(Config) != ERR::Okay or
       a.create_offer() != ERR::Okay or not gather(a) or
       a.set_local_description(a.local_description()) != ERR::Okay or
       b.set_remote_description(a.local_description()) != ERR::Okay or b.create_answer() != ERR::Okay or
       not gather(b) or b.set_local_description(b.local_description()) != ERR::Okay or
       a.set_remote_description(b.local_description()) != ERR::Okay or not connect(false)) return 51;
   std::vector<IceServer> servers;
   if (parse_ice_servers(Config, servers) != ERR::Okay or servers.empty()) return 52;
   auto fingerprint = a.certificate_fingerprint();
   unsigned step = 0;
   // Both initiators for each direct->relay, relay->relay allocation replacement and relay->direct transition.
   for (bool relay : {true, true, false, true, true, false}) {
      auto previous_relay = a.selected_relay();
      uint64_t closing[2];
      SctpChannelOptions options;
      options.label = "pending-close"; options.negotiated = true; options.id = uint16_t(200 + step * 2);
      for (unsigned side = 0; side < 2; ++side) {
         if (peers[side]->create_channel(options, closing[side]) != ERR::Okay) return 61;
      }
      block(servers.front().port, relay);
      // Force silent direct loss before notification. Hard relay rejection is exercised by the subsequent gather;
      // retire that adapter before another send, matching the owner-task interface notification contract.
      if (a.send_channel(handles[0][0], payload, false) != ERR::Okay or
          (not previous_relay and not pump())) return 53;
      a.network_changed(now_us()); b.network_changed(now_us());
      for (unsigned side = 0; side < 2; ++side) {
         for (unsigned mode = 1; mode < 7; ++mode) {
            if (peers[side]->send_channel(handles[side][mode], small, false) != ERR::Okay) return 62;
         }
         if (peers[side]->close_channel(closing[side]) != ERR::Okay) return 63;
      }
      if (b.send_channel(handles[1][0], payload, false) != ERR::Okay) return 64;
      auto pause_until = now_us() + 200000;
      while (now_us() < pause_until) if (not pump()) return 65;
      auto &offerer = step % 2 ? b : a;
      auto &answerer = step % 2 ? a : b;
      if (offerer.restart_ice() != ERR::Okay or not gather(offerer) or
          offerer.set_local_description(offerer.local_description()) != ERR::Okay or
          answerer.set_remote_description(offerer.local_description()) != ERR::Okay or
          answerer.create_answer() != ERR::Okay or not gather(answerer) or
          answerer.set_local_description(answerer.local_description()) != ERR::Okay or
          offerer.set_remote_description(answerer.local_description()) != ERR::Okay or not connect(relay)) {
         std::fprintf(stderr, "Migration step=%u relay=%d state=%d/%d ICE=%d/%d SCTP=%d/%d gather=%d/%d\n",
            step, int(relay), int(a.state()), int(b.state()), int(a.ice_state()), int(b.ice_state()),
            int(a.sctp_state()), int(b.sctp_state()), int(a.gathering_error()), int(b.gathering_error()));
         std::fprintf(stderr, "Migration stats A: %s\nB: %s\n", a.stats().c_str(), b.stats().c_str());
         return 54;
      }
      if (a.selected_relay() != relay or b.selected_relay() != relay or a.restart_required() or
          b.restart_required() or (relay and not previous_relay and not dropped()) or
          (not relay and not rejected())) return 55;
      auto delivered = [&]() {
         for (const auto &side : received) for (auto count : side) if (count != step + 1) return false;
         return a.channel(closing[0])->state IS SctpChannelState::CLOSED and
            b.channel(closing[1])->state IS SctpChannelState::CLOSED;
      };
      auto deadline = now_us() + 10000000;
      while (now_us() < deadline and not delivered()) if (not pump()) return 56;
      if (not delivered() or a.certificate_fingerprint() != fingerprint) return 57;
      for (unsigned side = 0; side < 2; ++side) for (unsigned mode = 0; mode < 7; ++mode) {
         auto channel = peers[side]->channel(handles[side][mode]);
         if (not channel or channel->state != SctpChannelState::OPEN or channel->id != int(42 + mode * 2)) return 66;
      }
      auto quiet = now_us() + 100000;
      while (now_us() < quiet) if (not pump() or not delivered()) return 58;
      std::printf("PASS %s-to-%s initiator=%c: bidirectional reliable/partial modes, expiry, pending close, handles\n",
         previous_relay ? "relay" : "direct", relay ? "relay" : "direct", step % 2 ? 'b' : 'a');
      ++step;
   }
   return 0;
}
#endif

int main(int Argc, CSTRING *Argv) {
   if (auto message = init_kotuku(Argc, Argv)) {
      std::fprintf(stderr, "%s\n", message);
      return 1;
   }
   if (objModule::load("network", &glNetworkModule, &NetworkBase) != ERR::Okay) {
      close_kotuku();
      return 1;
   }
   int result = 0;
   if (Argc >= 4 and std::string_view(Argv[1]) IS "--turn-tls-stall") {
      if (not rtc::test::turn_adapter_stall(true, false, Argv[2], Argv[3]) or
          not rtc::test::turn_adapter_stall(false, false, Argv[2], Argv[3]) or
          not rtc::test::turn_adapter_stall(false, true, Argv[2], Argv[3]) or
          not rtc::test::turn_adapter_stall(true, false, Argv[2], Argv[3], true) or
          not rtc::test::turn_adapter_stall(true, false, Argv[2], Argv[3], false, true)) result = 40;
   } else
#ifndef _WIN32
   if (Argc > 1 and std::string_view(Argv[1]).ends_with(".json")) {
      std::ifstream input(Argv[1]);
      std::string config{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      result = transition(config);
   } else
#endif
   result = run_test();
   if (result) std::fprintf(stderr, "PeerConnection loopback failed at step %d\n", result);
   close_kotuku();
   return result ? 1 : 0;
}
