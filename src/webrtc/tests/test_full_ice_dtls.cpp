#include "../ice/ice_dtls.h"

#include <array>
#include <cstdio>
#include <deque>
#include <vector>

using namespace rtc;

struct Datagram {
   ice::Endpoint source;
   ice::Endpoint destination;
   std::vector<uint8_t> bytes;
};

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

int main() {
   std::shared_ptr<DtlsCertificate> a_cert, b_cert;
   if (DtlsCertificate::generate(a_cert) != ERR::Okay or
       DtlsCertificate::generate(b_cert) != ERR::Okay) return fail("Certificate generation failed");
   ice::Endpoint a_address, b_address;
   a_address.address = { 127, 0, 0, 1 };
   b_address.address = { 127, 0, 0, 2 };
   a_address.port = 41000;
   b_address.port = 41001;
   std::deque<Datagram> wire;
   bool a_received = false, b_received = false;
   IceDtlsCallbacks a_callbacks, b_callbacks;
   a_callbacks.send = [&](std::span<const uint8_t> Data, const ice::Endpoint &Destination) {
      wire.push_back({ a_address, Destination, { Data.begin(), Data.end() } });
   };
   b_callbacks.send = [&](std::span<const uint8_t> Data, const ice::Endpoint &Destination) {
      wire.push_back({ b_address, Destination, { Data.begin(), Data.end() } });
   };
   a_callbacks.receive = [&](std::span<const uint8_t> Data) { a_received = Data.size() IS 2 and Data[0] IS 7; };
   b_callbacks.receive = [&](std::span<const uint8_t> Data) { b_received = Data.size() IS 2 and Data[0] IS 5; };
   FullIceDtlsTransport a(a_cert, "aaaa", "a-password-0123456789", ice::Role::CONTROLLING,
      DtlsRole::CLIENT, a_callbacks);
   FullIceDtlsTransport b(b_cert, "bbbb", "b-password-0123456789", ice::Role::CONTROLLED,
      DtlsRole::SERVER, b_callbacks);
   ice::Candidate a_candidate{ a_address, 2130706431, ice::CandidateType::HOST };
   ice::Candidate b_candidate{ b_address, 2130706431, ice::CandidateType::HOST };
   if (a.set_remote_fingerprint(b_cert->fingerprint()) != ERR::Okay or
       b.set_remote_fingerprint(a_cert->fingerprint()) != ERR::Okay or
       not a.set_remote_credentials("bbbb", "b-password-0123456789", false) or
       not b.set_remote_credentials("aaaa", "a-password-0123456789", false) or
       not a.add_local_candidate(a_candidate) or not a.add_remote_candidate(b_candidate) or
       not b.add_local_candidate(b_candidate) or not b.add_remote_candidate(a_candidate)) {
      return fail("Credential and candidate setup failed");
   }
   a.end_remote_candidates();
   b.end_remote_candidates();
   if (not a.start_ice(0) or not b.start_ice(0)) return fail("ICE did not start");
   auto stranger = a_address;
   stranger.port = 41002;
   std::array<uint8_t, 3> unvalidated_dtls{ 22, 0xfe, 0xfd };
   if (b.feed(unvalidated_dtls, b_address, stranger, 0) != ERR::Okay or
       b.state() != DtlsState::NEW) return fail("Unvalidated DTLS source was accepted");
   for (uint64_t now = 0; now < 10000000; now += 10000) {
      if (a.handle_timeout(now) != ERR::Okay or b.handle_timeout(now) != ERR::Okay) {
         return fail("ICE or DTLS timer failed");
      }
      for (int i = 0; i < 1000 and not wire.empty(); ++i) {
         auto packet = std::move(wire.front());
         wire.pop_front();
         if (packet.destination IS b_address) {
            if (b.feed(packet.bytes, b_address, packet.source, now) != ERR::Okay) return fail("B feed failed");
         } else if (packet.destination IS a_address) {
            if (a.feed(packet.bytes, a_address, packet.source, now) != ERR::Okay) return fail("A feed failed");
         } else return fail("Incorrect destination");
      }
      if (a.state() IS DtlsState::CONNECTED and b.state() IS DtlsState::CONNECTED) break;
   }
   if (a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED) {
      return fail("Full ICE and DTLS did not connect");
   }
   std::array<uint8_t, 2> from_a{ 5, 6 }, from_b{ 7, 8 };
   if (a.send(from_a) != ERR::Okay or b.send(from_b) != ERR::Okay) return fail("DTLS send failed");
   while (not wire.empty()) {
      auto packet = std::move(wire.front());
      wire.pop_front();
      if (packet.destination IS b_address) {
         if (b.feed(packet.bytes, b_address, packet.source, 10000000) != ERR::Okay) return fail("B data failed");
      } else if (a.feed(packet.bytes, a_address, packet.source, 10000000) != ERR::Okay) {
         return fail("A data failed");
      }
   }
   if (not a_received or not b_received) return fail("DTLS payload not delivered");

   a.restart_ice("a456", "a-restarted-password-0123456", ice::Role::CONTROLLING);
   b.restart_ice("b456", "b-restarted-password-0123456", ice::Role::CONTROLLED);
   if (a.ice_state() != ice::State::NEW or b.ice_state() != ice::State::NEW or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED or
       a.send(from_a) != ERR::NotInitialised or b.send(from_b) != ERR::NotInitialised or
       not a.set_remote_credentials("b456", "b-restarted-password-0123456", false) or
       not b.set_remote_credentials("a456", "a-restarted-password-0123456", false) or
       not a.add_remote_candidate(b_candidate) or not b.add_remote_candidate(a_candidate)) {
      return fail("ICE restart did not reset the selected route and credentials");
   }
   a.end_remote_candidates();
   b.end_remote_candidates();
   if (not a.start_ice(11000000) or not b.start_ice(11000000)) return fail("Restarted ICE did not start");
   for (uint64_t now = 11000000; now < 21000000; now += 10000) {
      if (a.handle_timeout(now) != ERR::Okay or b.handle_timeout(now) != ERR::Okay) {
         return fail("Restarted ICE timer failed");
      }
      for (int i = 0; i < 1000 and not wire.empty(); ++i) {
         auto packet = std::move(wire.front());
         wire.pop_front();
         if (packet.destination IS b_address) {
            if (b.feed(packet.bytes, b_address, packet.source, now) != ERR::Okay) {
               return fail("Restarted B feed failed");
            }
         } else if (packet.destination IS a_address) {
            if (a.feed(packet.bytes, a_address, packet.source, now) != ERR::Okay) {
               return fail("Restarted A feed failed");
            }
         } else return fail("Restarted ICE destination differs");
      }
      if (a.ice_state() IS ice::State::COMPLETED and b.ice_state() IS ice::State::COMPLETED) break;
   }
   if (a.ice_state() != ice::State::COMPLETED or b.ice_state() != ice::State::COMPLETED or
       a.state() != DtlsState::CONNECTED or b.state() != DtlsState::CONNECTED) {
      return fail("ICE restart did not preserve the DTLS session");
   }
   a_received = false;
   b_received = false;
   if (a.send(from_a) != ERR::Okay or b.send(from_b) != ERR::Okay) return fail("Post-restart DTLS send failed");
   while (not wire.empty()) {
      auto packet = std::move(wire.front());
      wire.pop_front();
      if (packet.destination IS b_address) {
         if (b.feed(packet.bytes, b_address, packet.source, 21000000) != ERR::Okay) {
            return fail("Post-restart B data failed");
         }
      } else if (a.feed(packet.bytes, a_address, packet.source, 21000000) != ERR::Okay) {
         return fail("Post-restart A data failed");
      }
   }
   return a_received and b_received ? 0 : fail("Post-restart DTLS payload not delivered");
}
