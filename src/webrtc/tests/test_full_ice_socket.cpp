#include "socket_support.h"

#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/module.h>

#include "../ice/ice_socket.h"
#include "../stun/stun.h"


#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

JUMPTABLE_NETWORK
static OBJECTPTR glNetworkModule;
CSTRING ProgName = "WebRTCFullIceSocketTest";

using namespace rtc;

static uint64_t now_us() {
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int run_test() {
   std::shared_ptr<DtlsCertificate> a_cert, b_cert;
   if (DtlsCertificate::generate(a_cert) != ERR::Okay or
       DtlsCertificate::generate(b_cert) != ERR::Okay) return 1;
   FullIceSocket candidate_limit(a_cert, "limit", "limit-password-0123456789", ice::Role::CONTROLLING,
      DtlsRole::CLIENT, {});
   ice::Candidate candidate;
   candidate.endpoint.address = {127, 0, 0, 1}; candidate.priority = 2130706431u;
   for (int i = 0; i < 128; ++i) {
      candidate.endpoint.port = uint16_t(20000 + i);
      if (not candidate_limit.add_remote_candidate(candidate)) return 20;
   }
   if (not candidate_limit.add_remote_candidate(candidate)) return 21;
   candidate.endpoint.port = 20128;
   if (candidate_limit.add_remote_candidate(candidate)) return 22;
   bool a_received = false, b_received = false;
   IceDtlsCallbacks a_callbacks, b_callbacks;
   a_callbacks.receive = [&](std::span<const uint8_t> Data) { a_received = Data.size() IS 1 and Data[0] IS 2; };
   b_callbacks.receive = [&](std::span<const uint8_t> Data) { b_received = Data.size() IS 1 and Data[0] IS 1; };
   FullIceSocket a(a_cert, "aaaa", "a-password-0123456789", ice::Role::CONTROLLING,
      DtlsRole::CLIENT, a_callbacks);
   FullIceSocket b(b_cert, "bbbb", "b-password-0123456789", ice::Role::CONTROLLED,
      DtlsRole::SERVER, b_callbacks);
   if (a.open("127.0.0.1", 0) != ERR::Okay or b.open("127.0.0.1", 0) != ERR::Okay or
       a.set_remote_fingerprint(b_cert->fingerprint()) != ERR::Okay or
       b.set_remote_fingerprint(a_cert->fingerprint()) != ERR::Okay or
       not a.set_remote_credentials("bbbb", "b-password-0123456789", false) or
       not b.set_remote_credentials("aaaa", "a-password-0123456789", false) or
       not a.add_remote_candidate({ b.local_address(), 2130706431u, ice::CandidateType::HOST }) or
       not b.add_remote_candidate({ a.local_address(), 2130706431u, ice::CandidateType::HOST })) return 2;

   auto stun_fd = rtc::test::udp_socket();
   if (stun_fd IS rtc::test::INVALID_SOCKET_VALUE) return 8;
   sockaddr_in stun_address{};
   stun_address.sin_family = AF_INET;
   stun_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   stun_address.sin_port = 0;
   if (::bind(stun_fd, (sockaddr *)&stun_address, sizeof(stun_address)) != 0) {
      rtc::test::close_socket(stun_fd);
      return 9;
   }
   rtc::test::AddressSize stun_size = sizeof(stun_address);
   if (::getsockname(stun_fd, (sockaddr *)&stun_address, &stun_size) != 0) {
      rtc::test::close_socket(stun_fd);
      return 10;
   }
   ice::Endpoint stun_server;
   stun_server.address = { 127, 0, 0, 1 };
   stun_server.port = ntohs(stun_address.sin_port);
   if (not a.start_server_reflexive(stun_server, now_us())) {
      rtc::test::close_socket(stun_fd);
      return 11;
   }
   std::array<uint8_t, 1500> stun_buffer{};
   sockaddr_in requester{};
   rtc::test::AddressSize requester_size = sizeof(requester);
   int received = -1;
   auto gather_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
   while (std::chrono::steady_clock::now() < gather_deadline and received < 0) {
      received = rtc::test::receive_from(stun_fd, stun_buffer.data(), stun_buffer.size(), 0,
         (sockaddr *)&requester, &requester_size);
      if (received < 0) ProcessMessages(PMF::NIL, 10);
   }
   stun::Message request;
   if (received <= 0 or not stun::parse(std::span<const uint8_t>(stun_buffer.data(), size_t(received)), request)) {
      rtc::test::close_socket(stun_fd);
      return 12;
   }
   std::array<uint8_t, 8> mapped{ 0, 1, uint8_t((50000 ^ 0x2112) >> 8), uint8_t(50000 ^ 0x2112),
      uint8_t(203 ^ 0x21), uint8_t(0 ^ 0x12), uint8_t(113 ^ 0xa4), uint8_t(9 ^ 0x42) };
   stun::Attribute mapped_attribute{ stun::ATTR_XOR_MAPPED_ADDRESS, mapped, 0 };
   std::vector<uint8_t> response;
   if (not stun::encode(stun::BINDING_SUCCESS, request.transaction_id,
       std::span<const stun::Attribute>(&mapped_attribute, 1), {}, false, true, response) or
       rtc::test::send_to(stun_fd, response.data(), response.size(), 0,
          (sockaddr *)&requester, requester_size) != int(response.size())) {
      rtc::test::close_socket(stun_fd);
      return 13;
   }
   while (std::chrono::steady_clock::now() < gather_deadline and not a.server_reflexive_candidate()) {
      ProcessMessages(PMF::NIL, 10);
   }
   rtc::test::close_socket(stun_fd);
   auto reflexive = a.server_reflexive_candidate();
   if (not reflexive or reflexive->endpoint.port != 50000 or reflexive->base != a.local_address()) return 14;

   a.end_remote_candidates();
   b.end_remote_candidates();
   if (not a.start_ice(now_us()) or not b.start_ice(now_us())) return 3;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (a.handle_timeout(now) != ERR::Okay or b.handle_timeout(now) != ERR::Okay) return 4;
      if (a.state() IS DtlsState::CONNECTED and b.state() IS DtlsState::CONNECTED) break;
   }
   if (a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED or
       a.socket_error() != ERR::Okay or b.socket_error() != ERR::Okay) return 5;
   std::array<uint8_t, 1> from_a{ 1 }, from_b{ 2 };
   if (a.send(from_a) != ERR::Okay or b.send(from_b) != ERR::Okay) return 6;
   while (std::chrono::steady_clock::now() < deadline and (not a_received or not b_received)) {
      ProcessMessages(PMF::NIL, 10);
   }
   if (not a_received or not b_received) return 7;

   // The independent gathering fixture accounts for the only non-peer exchange.
   ProcessMessages(PMF::NIL, 10);
   auto a_stats = a.counters(), b_stats = b.counters();
   if (a_stats.sent_bytes != b_stats.received_bytes + uint64_t(received) or
       b_stats.sent_bytes + response.size() != a_stats.received_bytes or
       a_stats.sent_packets != b_stats.received_packets + 1 or
       b_stats.sent_packets + 1 != a_stats.received_packets or
       a_stats.turn_sent_bytes != 0 or not a.selected_counters()) return 23;
   // Saturation must preserve exact uint64_t values rather than wrapping JSON counters.
   TransportCounters saturated;
   saturated.sent_bytes = UINT64_MAX - 1;
   saturated.sent(8);
   if (saturated.sent_bytes != UINT64_MAX) return 24;

   a.restart_ice("a456", "a-restarted-password-0123456", ice::Role::CONTROLLING);
   b.restart_ice("b456", "b-restarted-password-0123456", ice::Role::CONTROLLED);
   if (a.ice_state() != ice::State::NEW or b.ice_state() != ice::State::NEW or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED or
       a.send(from_a) != ERR::NotInitialised or b.send(from_b) != ERR::NotInitialised or
       not a.set_remote_credentials("b456", "b-restarted-password-0123456", false) or
       not b.set_remote_credentials("a456", "a-restarted-password-0123456", false) or
       not a.add_remote_candidate({ b.local_address(), 2130706431u, ice::CandidateType::HOST }) or
       not b.add_remote_candidate({ a.local_address(), 2130706431u, ice::CandidateType::HOST })) return 15;
   a.end_remote_candidates();
   b.end_remote_candidates();
   if (not a.start_ice(now_us()) or not b.start_ice(now_us())) return 16;
   auto restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < restart_deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (a.handle_timeout(now) != ERR::Okay or b.handle_timeout(now) != ERR::Okay) return 17;
      if (a.ice_state() IS ice::State::COMPLETED and b.ice_state() IS ice::State::COMPLETED) break;
   }
   if (a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED) return 18;
   a_received = false;
   b_received = false;
   if (a.send(from_a) != ERR::Okay or b.send(from_b) != ERR::Okay) return 19;
   while (std::chrono::steady_clock::now() < restart_deadline and (not a_received or not b_received)) {
      ProcessMessages(PMF::NIL, 10);
   }
   return a_received and b_received ? 0 : 20;
}

static int run_routed_test() {
   std::shared_ptr<DtlsCertificate> a_cert, b_cert;
   if (DtlsCertificate::generate(a_cert) != ERR::Okay or
       DtlsCertificate::generate(b_cert) != ERR::Okay) return 1;
   FullIceSocket candidate_limit(a_cert, "limit", "limit-password-0123456789", ice::Role::CONTROLLING,
      DtlsRole::CLIENT, {});
   ice::Candidate candidate;
   candidate.endpoint.address = {127, 0, 0, 1}; candidate.priority = 2130706431u;
   for (int i = 0; i < 128; ++i) {
      candidate.endpoint.port = uint16_t(20000 + i);
      if (not candidate_limit.add_remote_candidate(candidate)) return 20;
   }
   if (not candidate_limit.add_remote_candidate(candidate)) return 21;
   candidate.endpoint.port = 20128;
   if (candidate_limit.add_remote_candidate(candidate)) return 22;
   bool a_received = false, b_received = false;
   IceDtlsCallbacks a_callbacks, b_callbacks;
   a_callbacks.receive = [&](std::span<const uint8_t> Data) { a_received = Data.size() IS 1 and Data[0] IS 2; };
   b_callbacks.receive = [&](std::span<const uint8_t> Data) { b_received = Data.size() IS 1 and Data[0] IS 1; };
   FullIceSocket a(a_cert, "aaaa", "a-password-0123456789", ice::Role::CONTROLLING,
      DtlsRole::CLIENT, a_callbacks);
   FullIceSocket b(b_cert, "bbbb", "b-password-0123456789", ice::Role::CONTROLLED,
      DtlsRole::SERVER, b_callbacks);
   ice::Endpoint alternate;
   if (a.open("127.0.0.1", 0, 1) != ERR::Okay or
       a.add_interface("127.0.0.2", 2130706431u, alternate) != ERR::Okay or
       b.open("127.0.0.1", 0) != ERR::Okay or
       a.set_remote_fingerprint(b_cert->fingerprint()) != ERR::Okay or
       b.set_remote_fingerprint(a_cert->fingerprint()) != ERR::Okay or
       not a.set_remote_credentials("bbbb", "b-password-0123456789", false) or
       not b.set_remote_credentials("aaaa", "a-password-0123456789", false) or
       not a.add_remote_candidate({ b.local_address(), 2130706431u, ice::CandidateType::HOST }) or
       not b.add_remote_candidate({ alternate, 2130706431u, ice::CandidateType::HOST })) return 2;
   a.end_remote_candidates();
   b.end_remote_candidates();
   if (not a.start_ice(now_us()) or not b.start_ice(now_us())) return 3;
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      auto now = now_us();
      if (a.handle_timeout(now) != ERR::Okay or b.handle_timeout(now) != ERR::Okay) return 4;
      if (a.state() IS DtlsState::CONNECTED and b.state() IS DtlsState::CONNECTED) break;
   }
   if (a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED or
       not a.selected_local() or *a.selected_local() != alternate) return 5;
   std::array<uint8_t, 1> from_a{ 1 }, from_b{ 2 };
   if (a.send(from_a) != ERR::Okay or b.send(from_b) != ERR::Okay) return 6;
   while (std::chrono::steady_clock::now() < deadline and (not a_received or not b_received)) {
      ProcessMessages(PMF::NIL, 10);
   }
   return a_received and b_received ? 0 : 7;
}

int main(int argc, CSTRING *argv) {
   if (auto message = init_kotuku(argc, argv)) {
      std::fprintf(stderr, "%s\n", message);
      return 1;
   }
   if (objModule::load("network", &glNetworkModule, &NetworkBase) != ERR::Okay) {
      close_kotuku();
      return 1;
   }
   auto result = run_test();
   if (result) std::fprintf(stderr, "Full ICE/DTLS UDP loopback failed at step %d\n", result);
   if (not result) {
      result = run_routed_test();
      if (result) std::fprintf(stderr, "Multi-socket ICE/DTLS loopback failed at step %d\n", result);
   }
   close_kotuku();
   return result ? 1 : 0;
}
