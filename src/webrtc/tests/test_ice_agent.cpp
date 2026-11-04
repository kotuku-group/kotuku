#include "../ice/ice_agent.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

using namespace rtc::ice;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

static Endpoint endpoint(uint8_t Last, uint16_t Port) {
   Endpoint result;
   result.address = { 127, 0, 0, Last };
   result.port = Port;
   return result;
}

static void queue(std::deque<Outbound> &Queue, std::vector<Outbound> &Output) {
   for (auto &packet : Output) Queue.push_back(std::move(packet));
   Output.clear();
}

static bool drive(FullAgent &A, FullAgent &B, const Endpoint &AAddress, const Endpoint &BAddress,
   bool LoseFirst, bool Reverse, int &RegularChecks, int &Nominations) {
   std::deque<Outbound> packets;
   std::vector<Outbound> output;
   if (not A.start(0, output)) return false;
   queue(packets, output);
   if (not B.start(0, output)) return false;
   queue(packets, output);
   bool dropped = false;
   for (uint64_t now = 0; now < 15000000; now += 100000) {
      A.tick(now, output);
      queue(packets, output);
      B.tick(now, output);
      queue(packets, output);
      auto flight = std::move(packets);
      packets.clear();
      if (Reverse) std::reverse(flight.begin(), flight.end());
      for (auto &packet : flight) {
         rtc::stun::Message parsed;
         if (not rtc::stun::parse(packet.datagram, parsed)) return false;
         if (parsed.type IS rtc::stun::BINDING_REQUEST and packet.local IS AAddress) {
            if (parsed.find(0x0025)) ++Nominations;
            else ++RegularChecks;
         }
         if (LoseFirst and not dropped and packet.local IS AAddress and
             parsed.type IS rtc::stun::BINDING_REQUEST) {
            dropped = true;
            continue;
         }
         if (packet.local IS AAddress and packet.remote IS BAddress) {
            B.receive(packet.datagram, BAddress, AAddress, now, output);
         } else if (packet.local IS BAddress and packet.remote IS AAddress) {
            A.receive(packet.datagram, AAddress, BAddress, now, output);
         } else return false;
         queue(packets, output);
      }
      if (A.state() IS State::COMPLETED and B.state() IS State::COMPLETED) return true;
   }
   return false;
}

static bool delayed_nomination(bool Upgrade, bool Downgrade = false) {
   auto source = endpoint(10, 12000), replacement = endpoint(11, 12001);
   auto low = endpoint(12, 12002), high = endpoint(13, 12003);
   Candidate source_candidate{source, Upgrade ? 2130706000u : 2000u, CandidateType::HOST};
   Candidate low_candidate{low, Upgrade ? 2130700000u : 1000u, CandidateType::HOST};
   Candidate high_candidate{high, Downgrade ? 500u : (Upgrade ? 2130703000u : 3000u), CandidateType::HOST};
   FullAgent first("peer", "peer-password-0123456789", Role::CONTROLLING);
   FullAgent controlled("ctrl", "controlled-password-0123456789", Role::CONTROLLED);
   if (not first.set_remote_credentials("ctrl", "controlled-password-0123456789", false) or
       not controlled.set_remote_credentials("peer", "peer-password-0123456789", false) or
       not first.add_local_candidate(source_candidate) or not first.add_remote_candidate(low_candidate) or
       not controlled.add_local_candidate(low_candidate) or not controlled.add_remote_candidate(source_candidate)) {
      return false;
   }
   int regular = 0, nominations = 0;
   if (not drive(first, controlled, source, low, false, false, regular, nominations)) return false;
   if (not controlled.selected_local() or *controlled.selected_local() != low) return false;
   if (not controlled.add_local_candidate(high_candidate)) return false;
   // A second controlling core with the same credentials represents the browser's newly resolved socket/pair.
   FullAgent second("peer", "peer-password-0123456789", Role::CONTROLLING);
   source_candidate.endpoint = replacement;
   if (not second.set_remote_credentials("ctrl", "controlled-password-0123456789", false) or
       not second.add_local_candidate(source_candidate) or not second.add_remote_candidate(high_candidate)) {
      return false;
   }
   std::vector<Outbound> output;
   std::deque<Outbound> wire;
   if (not second.start(2000000, output)) return false;
   // Invalid authentication must not displace the existing selected route.
   auto forged = output.front().datagram; forged.back() ^= 1;
   std::vector<Outbound> rejected;
   controlled.receive(forged, high, replacement, 2000000, rejected);
   if (not rejected.empty() or *controlled.selected_local() != low) return false;
   queue(wire, output);
   bool checked_replacement = false;
   for (uint64_t now = 2000000; now < 5000000; now += 10000) {
      controlled.tick(now, output); queue(wire, output);
      second.tick(now, output); queue(wire, output);
      for (unsigned budget = 0; budget < 100 and not wire.empty(); ++budget) {
         auto packet = std::move(wire.front()); wire.pop_front();
         if (packet.local IS replacement and packet.remote IS high) {
            controlled.receive(packet.datagram, high, replacement, now, output);
            rtc::stun::Message request;
            if (Upgrade and rtc::stun::parse(packet.datagram, request) and request.find(0x0025)) {
               if (checked_replacement or *controlled.selected_local() != low or
                   not controlled.add_remote_candidate(source_candidate)) return false;
               if (controlled.timeout_us(now) < 0 or controlled.timeout_us(now) > 50000) return false;
            }
         } else if (packet.local IS high and packet.remote IS replacement) {
            rtc::stun::Message parsed;
            if (not rtc::stun::parse(packet.datagram, parsed)) return false;
            if (parsed.type IS rtc::stun::BINDING_REQUEST) checked_replacement = true;
            second.receive(packet.datagram, replacement, high, now, output);
         } else if (packet.local IS low and packet.remote IS source) {
            first.receive(packet.datagram, source, low, now, output);
         } else if (packet.local IS source and packet.remote IS low) {
            controlled.receive(packet.datagram, low, source, now, output);
         } else return false;
         queue(wire, output);
      }
      if (controlled.selected_local() and *controlled.selected_local() IS high and
          controlled.selected_remote() and *controlled.selected_remote() IS replacement) {
         return checked_replacement and controlled.state() IS State::COMPLETED;
      }
   }
   return false;
}

int main() {
   if (not delayed_nomination(false) or not delayed_nomination(true) or
       not delayed_nomination(false, true)) {
      return fail("Delayed authenticated nomination or lower-priority route did not update the selected pair");
   }
   auto a_address = endpoint(1, 10000);
   auto b_address = endpoint(2, 10001);
   Candidate a_candidate{ a_address, 2130706431, CandidateType::HOST };
   Candidate b_candidate{ b_address, 2130706431, CandidateType::HOST };
   FullAgent a("a123", "a-password-0123456789012345", Role::CONTROLLING);
   FullAgent b("b123", "b-password-0123456789012345", Role::CONTROLLED);
   if (not a.set_remote_credentials("b123", "b-password-0123456789012345", false) or
       not b.set_remote_credentials("a123", "a-password-0123456789012345", false) or
       not a.add_local_candidate(a_candidate) or not a.add_remote_candidate(b_candidate) or
       not b.add_local_candidate(b_candidate) or not b.add_remote_candidate(a_candidate)) {
      return fail("Candidate or credential setup failed");
   }
   a.end_remote_candidates();
   b.end_remote_candidates();
   int regular = 0, nominations = 0;
   if (not drive(a, b, a_address, b_address, true, true, regular, nominations)) {
      return fail("Host pair did not complete under loss and reordering");
   }
   if (regular < 2 or nominations < 1 or not a.selected_remote() or not b.selected_remote() or
       *a.selected_remote() != b_address or *b.selected_remote() != a_address) {
      return fail("Regular nomination or selected pair differs");
   }

   auto other_route = a_address; other_route.route = 77;
   if (not a.validated(b_address, a_address) or a.validated(b_address, other_route)) {
      return fail("Authentication on a direct socket authorised a different allocation route");
   }

   if (not a.add_local_candidate({other_route, 16777215u, CandidateType::RELAY})) {
      return fail("Distinct allocation route collided with the direct candidate endpoint");
   }
   a.retire_local_route(other_route);
   if (a.state() != State::COMPLETED or not a.validated(b_address, a_address)) {
      return fail("An unselected failed allocation invalidated the usable route");
   }
   auto retired = a;
   retired.retire_local_route(a_address);
   if (retired.state() != State::FAILED or retired.validated(b_address, a_address)) {
      return fail("Retiring the selected route retained authentication or dispatch state");
   }

   // A timer captured before the nomination callback cannot expire freshly granted consent.
   auto stale_timer = a;
   std::vector<Outbound> stale_output;
   stale_timer.tick(0, stale_output);
   if (stale_timer.state() != State::COMPLETED or not stale_timer.validated(b_address, a_address)) {
      return fail("An older timer snapshot expired current consent");
   }

   // Check both priority orderings: black-holed candidates must never stall the usable pair.
   for (uint32_t unused_priority : {2130706175u, 2130706432u}) {
      FullAgent many_a("ma", "ma-password-0123456789012345", Role::CONTROLLING);
      FullAgent many_b("mb", "mb-password-0123456789012345", Role::CONTROLLED);
      auto unused_a = endpoint(3, 10002);
      auto unused_b = endpoint(4, 10003);
      if (not many_a.set_remote_credentials("mb", "mb-password-0123456789012345", false) or
          not many_b.set_remote_credentials("ma", "ma-password-0123456789012345", false) or
          not many_a.add_local_candidate(a_candidate) or
          not many_a.add_local_candidate({ unused_a, unused_priority, CandidateType::HOST }) or
          not many_a.add_remote_candidate(b_candidate) or
          not many_a.add_remote_candidate({ unused_b, unused_priority, CandidateType::HOST }) or
          not many_b.add_local_candidate(b_candidate) or
          not many_b.add_local_candidate({ unused_b, unused_priority, CandidateType::HOST }) or
          not many_b.add_remote_candidate(a_candidate)) return fail("Multi-pair setup failed");
      many_a.end_remote_candidates();
      many_b.end_remote_candidates();
      std::deque<Outbound> pair_packets;
      std::vector<Outbound> pair_output;
      if (not many_a.start(0, pair_output)) return fail("Multi-pair controlling start failed");
      queue(pair_packets, pair_output);
      if (not many_b.start(0, pair_output)) return fail("Multi-pair controlled start failed");
      queue(pair_packets, pair_output);
      for (uint64_t now = 0; now < 1000000; now += 50000) {
         many_a.tick(now, pair_output);
         queue(pair_packets, pair_output);
         many_b.tick(now, pair_output);
         queue(pair_packets, pair_output);
         auto flight = std::move(pair_packets);
         pair_packets.clear();
         for (auto &packet : flight) {
            if (packet.local IS a_address and packet.remote IS b_address)
               many_b.receive(packet.datagram, b_address, a_address, now, pair_output);
            else if (packet.local IS b_address and packet.remote IS a_address)
               many_a.receive(packet.datagram, a_address, b_address, now, pair_output);
            queue(pair_packets, pair_output);
         }
         if (many_a.state() IS State::COMPLETED and many_b.state() IS State::COMPLETED) break;
      }
      if (many_a.state() != State::COMPLETED or many_b.state() != State::COMPLETED or
          not many_a.selected_local() or *many_a.selected_local() != a_address) {
         return fail("Usable pair waited for black-holed candidate timeouts");
      }
   }

   // An outstanding high-priority check must not consume the pacing slot or another check's retry timer.
   FullAgent paced("pace", "pace-password-012345678901", Role::CONTROLLING);
   auto unreachable = endpoint(8, 19999);
   if (not paced.set_remote_credentials("peer", "peer-password-012345678901", false) or
       not paced.add_local_candidate(a_candidate) or
       not paced.add_remote_candidate({ unreachable, 2130706432u, CandidateType::HOST }) or
       not paced.add_remote_candidate(b_candidate)) return fail("Pacing setup failed");
   std::vector<Outbound> paced_output;
   if (not paced.start(0, paced_output) or paced_output.size() != 1 or
       paced_output.front().remote != unreachable or paced.timeout_us(0) != 50000) {
      return fail("Initial check did not schedule the next pacing slot");
   }
   auto first_check = paced_output.front().datagram;
   paced_output.clear();
   paced.tick(49999, paced_output);
   if (not paced_output.empty()) return fail("Ordinary check was sent before its pacing slot");
   paced.tick(50000, paced_output);
   if (paced_output.size() != 1 or paced_output.front().remote != b_address or
       paced_output.front().datagram IS first_check or paced.timeout_us(50000) != 450000) {
      return fail("Outstanding check blocked a different candidate or reused its transaction");
   }
   auto second_check = paced_output.front().datagram;
   paced_output.clear();
   paced.tick(500000, paced_output);
   if (paced_output.size() != 1 or paced_output.front().datagram != first_check) {
      return fail("First check did not retransmit independently");
   }
   paced_output.clear();
   paced.tick(550000, paced_output);
   if (paced_output.size() != 1 or paced_output.front().datagram != second_check) {
      return fail("Second check lost its independent retransmission timer");
   }

   std::vector<Outbound> output;
   FullAgent reflexive_agent("srflx", "srflx-password-0123456789", Role::CONTROLLING);
   auto mapped_address = endpoint(9, 12000);
   Candidate reflexive_candidate{ mapped_address, 1694498815u, CandidateType::SERVER_REFLEXIVE, a_address };
   if (not reflexive_agent.set_remote_credentials("remote", "remote-password-0123456789", false) or
       not reflexive_agent.add_local_candidate(reflexive_candidate) or
       not reflexive_agent.add_remote_candidate(b_candidate)) return fail("Reflexive pair setup failed");
   output.clear();
   if (not reflexive_agent.start(0, output) or output.empty() or
       output.front().local != a_address) return fail("Reflexive check did not use its base socket");

   // An authenticated mapped response learns a local peer-reflexive valid pair, retaining its sending base.
   FullAgent nat_client("nat", "nat-password-012345678901", Role::CONTROLLING);
   LiteResponder nat_server("remote", "remote-password-0123456789", "nat");
   Candidate nat_host{ a_address, 2130450431u, CandidateType::HOST };
   if (not nat_client.set_remote_credentials("remote", "remote-password-0123456789", true) or
       not nat_client.add_local_candidate(reflexive_candidate) or not nat_client.add_local_candidate(nat_host) or
       not nat_client.add_remote_candidate(b_candidate)) return fail("Local reflexive setup failed");
   auto translated = endpoint(7, 13000);
   output.clear();
   if (not nat_client.start(0, output) or output.size() != 1) return fail("Local reflexive start failed");
   for (uint64_t now : {0u, 50000u}) {
      if (now) nat_client.tick(now, output);
      if (output.size() != 1 or output.front().local != a_address) {
         return fail("Redundant reflexive route was checked or mapped address was used as a socket");
      }
      rtc::stun::Message request;
      if (not rtc::stun::parse(output.front().datagram, request)) return fail("Mapped check parse failed");
      if (bool(request.find(0x0025)) != bool(now)) return fail("Mapped pair was not regularly nominated");
      auto reply = nat_server.receive(output.front().datagram, translated);
      output.clear();
      if (not reply.authenticated or reply.response.empty()) return fail("Mapped response failed");
      nat_client.receive(reply.response, a_address, b_address, now, output);
   }
   auto valid_local = nat_client.selected_local_candidate();
   if (not valid_local or valid_local->endpoint != translated or valid_local->base != a_address or
       valid_local->type != CandidateType::PEER_REFLEXIVE or
       valid_local->priority != ((110u << 24) | (nat_host.priority & 0x00ffffffu)) or
       not nat_client.selected_local() or *nat_client.selected_local() != a_address) {
      return fail("Local peer-reflexive valid candidate differs from the mapped response");
   }

   // A higher-priority host check can produce a lower-priority mapped valid pair.
   // Nomination must compare the resulting valid candidates rather than the original checklist entries.
   FullAgent mapped_priority("priority", "priority-password-01234567", Role::CONTROLLING);
   LiteResponder priority_server("remote", "remote-password-0123456789", "priority");
   auto other_base = endpoint(6, 14000);
   if (not mapped_priority.set_remote_credentials("remote", "remote-password-0123456789", true) or
       not mapped_priority.add_local_candidate(a_candidate) or
       not mapped_priority.add_local_candidate({other_base, 2130706175u, CandidateType::HOST}) or
       not mapped_priority.add_remote_candidate(b_candidate)) return fail("Mapped priority setup failed");
   output.clear();
   if (not mapped_priority.start(0, output) or output.size() != 1) return fail("Mapped priority start failed");
   auto translated_reply = priority_server.receive(output.front().datagram, translated);
   output.clear();
   mapped_priority.tick(50000, output);
   if (output.size() != 1 or output.front().local != other_base) return fail("Second mapped priority check failed");
   auto direct_reply = priority_server.receive(output.front().datagram, other_base);
   output.clear();
   mapped_priority.receive(translated_reply.response, a_address, b_address, 50000, output);
   mapped_priority.receive(direct_reply.response, other_base, b_address, 50000, output);
   mapped_priority.tick(100000, output);
   if (output.size() != 1 or output.front().local != other_base) {
      return fail("Nomination used checklist priority instead of valid pair priority");
   }
   rtc::stun::Message nomination_request;
   if (not rtc::stun::parse(output.front().datagram, nomination_request)) return fail("Nomination parse failed");
   std::array<uint8_t, 4> nomination_error{0, 0, 5, 0};
   rtc::stun::Attribute nomination_error_attribute{rtc::stun::ATTR_ERROR_CODE, nomination_error};
   auto nomination_password = std::string_view("remote-password-0123456789");
   std::vector<uint8_t> nomination_failure;
   if (not rtc::stun::encode(rtc::stun::BINDING_ERROR, nomination_request.transaction_id,
       std::span<const rtc::stun::Attribute>(&nomination_error_attribute, 1),
       std::span<const uint8_t>((const uint8_t *)nomination_password.data(), nomination_password.size()),
       false, true, nomination_failure)) return fail("Nomination failure encoding failed");
   output.clear();
   mapped_priority.receive(nomination_failure, other_base, b_address, 100000, output);
   if (mapped_priority.state() != State::FAILED or not output.empty()) {
      return fail("Nomination failure did not fail the checklist");
   }

   // Host and server-reflexive candidates on one base must not produce duplicate retry schedules.
   FullAgent pruned("prune", "prune-password-0123456789", Role::CONTROLLING);
   if (not pruned.set_remote_credentials("remote", "remote-password-0123456789", false) or
       not pruned.add_local_candidate(reflexive_candidate) or not pruned.add_local_candidate(a_candidate) or
       not pruned.add_remote_candidate(b_candidate)) return fail("Pair pruning setup failed");
   output.clear();
   if (not pruned.start(0, output) or output.size() != 1) return fail("Pair pruning start failed");
   output.clear();
   pruned.tick(50000, output);
   if (not output.empty() or pruned.timeout_us(50000) != 450000) {
      return fail("Server-reflexive base produced a redundant ordinary check");
   }

   FullAgent foundation_agent("foundation", "foundation-password-012345", Role::CONTROLLING);
   Candidate foundation_high{unreachable, 2130706432u, CandidateType::HOST};
   Candidate foundation_low = b_candidate;
   foundation_high.foundation = foundation_low.foundation = "shared";
   if (not foundation_agent.set_remote_credentials("remote", "remote-password-0123456789", false) or
       not foundation_agent.add_local_candidate(a_candidate) or
       not foundation_agent.add_remote_candidate(foundation_low) or
       not foundation_agent.add_remote_candidate(foundation_high)) return fail("Foundation setup failed");
   output.clear();
   if (not foundation_agent.start(0, output) or output.size() != 1 or
       output.front().remote != unreachable) return fail("Foundation did not start its highest-priority pair");
   rtc::stun::Message foundation_request;
   if (not rtc::stun::parse(output.front().datagram, foundation_request)) return fail("Foundation parse failed");
   output.clear();
   foundation_agent.tick(50000, output);
   if (not output.empty() or foundation_agent.timeout_us(50000) != 450000) {
      return fail("Frozen foundation pair bypassed its active check");
   }
   std::array<uint8_t, 4> server_error{0, 0, 5, 0};
   rtc::stun::Attribute server_error_attribute{rtc::stun::ATTR_ERROR_CODE, server_error};
   auto remote_password = std::string_view("remote-password-0123456789");
   std::vector<uint8_t> failed_check;
   if (not rtc::stun::encode(rtc::stun::BINDING_ERROR, foundation_request.transaction_id,
       std::span<const rtc::stun::Attribute>(&server_error_attribute, 1),
       std::span<const uint8_t>((const uint8_t *)remote_password.data(), remote_password.size()),
       false, true, failed_check)) return fail("Foundation failure encoding failed");
   foundation_agent.receive(failed_check, a_address, unreachable, 50000, output);
   if (output.size() != 1 or output.front().remote != b_address) {
      return fail("Foundation failure did not thaw its next route");
   }

   FullAgent limited("limit", "limit-password-0123456789", Role::CONTROLLING, 1);
   if (not limited.set_remote_credentials("remote", "remote-password-0123456789", false) or
       not limited.add_local_candidate(a_candidate) or not limited.add_remote_candidate(b_candidate) or
       not limited.add_remote_candidate({unreachable, 2130706432u, CandidateType::HOST})) {
      return fail("Checklist limit setup failed");
   }
   output.clear();
   if (not limited.start(0, output) or output.size() != 1 or output.front().remote != unreachable) {
      return fail("Checklist limit discarded its highest-priority pair");
   }
   output.clear();
   limited.tick(50000, output);
   if (not output.empty()) return fail("Checklist pair limit was exceeded");

   FullAgent expiring("expire", "expire-password-0123456789", Role::CONTROLLING);
   if (not expiring.set_remote_credentials("remote", "remote-password-0123456789", false) or
       not expiring.add_local_candidate(a_candidate) or not expiring.add_remote_candidate(b_candidate)) {
      return fail("Retransmission expiry setup failed");
   }
   expiring.end_remote_candidates();
   output.clear();
   if (not expiring.start(0, output) or output.size() != 1) return fail("Retransmission expiry start failed");
   auto expiring_request = output.front().datagram;
   for (uint64_t now : {500000u, 1500000u, 3500000u, 7500000u, 15500000u, 31500000u}) {
      output.clear();
      expiring.tick(now - 1, output);
      if (not output.empty()) return fail("Connectivity check retransmitted early");
      expiring.tick(now, output);
      if (output.size() != 1 or output.front().datagram != expiring_request) {
         return fail("Connectivity check backoff or transaction ID changed");
      }
   }
   output.clear();
   expiring.tick(39499999u, output);
   if (not output.empty() or expiring.state() IS State::FAILED) return fail("Final response wait ended early");
   LiteResponder expiry_server("remote", "remote-password-0123456789", "expire");
   auto expired_reply = expiry_server.receive(expiring_request, a_address);
   if (expired_reply.response.empty()) return fail("Expired response fixture failed");
   expiring.receive(expired_reply.response, a_address, b_address, 39500000u, output);
   if (expiring.state() != State::CHECKING or not output.empty()) return fail("Expired check response was accepted");
   expiring.tick(39500000u, output);
   if (not output.empty() or expiring.state() != State::FAILED) return fail("Check did not expire after 39.5 seconds");

   // Consent requests use new transaction IDs at 4-6 second intervals and are each transmitted once.
   output.clear();
   std::vector<uint8_t> late_consent_response;
   std::array<uint8_t, 12> previous_consent_id{};
   uint64_t previous_consent_us = 0;
   size_t consent_count = 0;
   for (uint64_t now = 6000000; now < 32000000; now += 100000) {
      a.tick(now, output);
      if (output.size() > 1) return fail("Consent sent more than one request at a time");
      if (not output.empty()) {
         rtc::stun::Message consent_request;
         if (not rtc::stun::parse(output.front().datagram, consent_request) or
             consent_request.type != rtc::stun::BINDING_REQUEST or
             (consent_count and (consent_request.transaction_id IS previous_consent_id or
                now - previous_consent_us < 4000000 or now - previous_consent_us > 6100000))) {
            return fail("Consent interval or transaction ID is invalid");
         }
         previous_consent_id = consent_request.transaction_id;
         previous_consent_us = now;
         ++consent_count;
         if (late_consent_response.empty()) {
            std::vector<Outbound> reply;
            b.receive(output.front().datagram, b_address, a_address, now, reply);
            if (reply.empty()) return fail("Peer did not answer consent request");
            late_consent_response = std::move(reply.front().datagram);
         }
      }
      output.clear(); // Simulate loss of all consent responses.
   }
   if (consent_count < 3 or a.state() != State::DISCONNECTED or a.selected_remote() or
       a.timeout_us(32000000) != -1) return fail("Consent did not expire after repeated unanswered checks");
   output.clear();
   a.receive(late_consent_response, a_address, b_address, 32000000, output);
   if (a.state() != State::DISCONNECTED or a.selected_remote()) {
      return fail("Late consent response restored expired consent");
   }

   a.restart("a456", "a-restarted-password-0123456", Role::CONTROLLING);
   b.restart("b456", "b-restarted-password-0123456", Role::CONTROLLED);
   if (a.state() != State::NEW or a.selected_remote() or
       not a.set_remote_credentials("b456", "b-restarted-password-0123456", false) or
       not b.set_remote_credentials("a456", "a-restarted-password-0123456", false) or
       not a.add_remote_candidate(b_candidate) or not b.add_remote_candidate(a_candidate)) {
      return fail("ICE restart did not reset credentials and nomination");
   }
   a.end_remote_candidates();
   b.end_remote_candidates();
   regular = 0;
   nominations = 0;
   if (not drive(a, b, a_address, b_address, false, false, regular, nominations)) {
      return fail("Restarted ICE pair did not complete");
   }

   // A response to an earlier outstanding consent request still refreshes consent.  Authenticated 403 revokes it.
   std::vector<uint8_t> earlier_response;
   bool delivered_earlier = false;
   bool revoked = false;
   output.clear();
   for (uint64_t now = 6000000; now < 25000000 and not revoked; now += 100000) {
      a.tick(now, output);
      if (output.empty()) continue;
      if (output.size() != 1) return fail("Unexpected consent request count after restart");
      rtc::stun::Message consent_request;
      if (not rtc::stun::parse(output.front().datagram, consent_request)) {
         return fail("Restarted consent request did not parse");
      }
      if (earlier_response.empty()) {
         std::vector<Outbound> reply;
         b.receive(output.front().datagram, b_address, a_address, now, reply);
         if (reply.empty()) return fail("Peer did not answer earlier consent request");
         earlier_response = std::move(reply.front().datagram);
      } else if (not delivered_earlier) {
         a.receive(earlier_response, a_address, b_address, now, output);
         if (a.state() != State::COMPLETED) return fail("Earlier consent response was rejected");
         delivered_earlier = true;
      } else {
         std::array<uint8_t, 4> forbidden{0, 0, 4, 3};
         rtc::stun::Attribute error{rtc::stun::ATTR_ERROR_CODE, forbidden};
         auto password = std::string_view("b-restarted-password-0123456");
         auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
         std::vector<uint8_t> rejection;
         if (not rtc::stun::encode(rtc::stun::BINDING_ERROR, consent_request.transaction_id,
             std::span(&error, 1), key, false, true, rejection)) {
            return fail("Consent revocation response could not be encoded");
         }
         a.receive(rejection, a_address, b_address, now, output);
         revoked = a.state() IS State::DISCONNECTED;
      }
      output.clear();
   }
   if (not delivered_earlier or not revoked or a.selected_remote()) {
      return fail("Delayed consent response or authenticated 403 revocation failed");
   }

   // Both peers initially claiming the controlling role resolve it using their tie breakers.
   FullAgent conflict_a("c123", "c-password-0123456789012345", Role::CONTROLLING);
   FullAgent conflict_b("d123", "d-password-0123456789012345", Role::CONTROLLING);
   if (not conflict_a.set_remote_credentials("d123", "d-password-0123456789012345", false) or
       not conflict_b.set_remote_credentials("c123", "c-password-0123456789012345", false) or
       not conflict_a.add_local_candidate(a_candidate) or not conflict_a.add_remote_candidate(b_candidate) or
       not conflict_b.add_local_candidate(b_candidate) or not conflict_b.add_remote_candidate(a_candidate)) {
      return fail("Role conflict setup failed");
   }
   conflict_a.end_remote_candidates();
   conflict_b.end_remote_candidates();
   regular = 0;
   nominations = 0;
   if (not drive(conflict_a, conflict_b, a_address, b_address, false, true, regular, nominations) or
       conflict_a.role() IS conflict_b.role()) return fail("Role conflict was not resolved");

   // Learn authenticated peer-reflexive candidates both before and after signalling completes.
   for (bool end_candidates : {false, true}) {
      FullAgent trickle_a("e123", "e-password-0123456789012345", Role::CONTROLLED);
      FullAgent trickle_b("f123", "f-password-0123456789012345", Role::CONTROLLING);
      if (not trickle_a.set_remote_credentials("f123", "f-password-0123456789012345", false) or
          not trickle_b.set_remote_credentials("e123", "e-password-0123456789012345", false) or
          not trickle_a.add_local_candidate(a_candidate) or
          not trickle_b.add_local_candidate(b_candidate) or not trickle_b.add_remote_candidate(a_candidate)) {
         return fail("Trickle setup failed");
      }
      if (end_candidates) {
         if (not trickle_a.add_remote_candidate({ endpoint(8, 19999), 2130706431, CandidateType::HOST })) {
            return fail("Unreachable signalled candidate was rejected");
         }
         trickle_a.end_remote_candidates();
      }
      output.clear();
      if (not trickle_a.start(0, output) or not trickle_b.start(0, output) or output.empty()) {
         return fail("Trickle check did not start");
      }
      if (not end_candidates and trickle_a.timeout_us(0) != -1) {
         return fail("Agent without remote candidates scheduled a busy timer");
      }
      std::deque<Outbound> packets;
      queue(packets, output);
      for (uint64_t now = 0; now < 15000000; now += 100000) {
         trickle_a.tick(now, output);
         queue(packets, output);
         trickle_b.tick(now, output);
         queue(packets, output);
         auto flight = std::move(packets);
         packets.clear();
         for (auto &packet : flight) {
            if (packet.local IS a_address) trickle_b.receive(packet.datagram, b_address, a_address, now, output);
            else trickle_a.receive(packet.datagram, a_address, b_address, now, output);
            queue(packets, output);
         }
         if (trickle_a.state() IS State::COMPLETED and trickle_b.state() IS State::COMPLETED) break;
      }
      if (trickle_a.state() != State::COMPLETED or trickle_b.state() != State::COMPLETED or
          not trickle_a.validated(b_address)) return fail("Peer-reflexive trickle check failed");

   }

   FullAgent ended("eoc1", "eoc-password-0123456789012345", Role::CONTROLLING);
   output.clear();
   if (not ended.set_remote_credentials("eoc2", "remote-password-0123456789", false) or
       not ended.add_local_candidate(a_candidate)) return fail("End-of-candidates setup failed");
   ended.end_remote_candidates();
   if (not ended.add_remote_candidate(b_candidate) or not ended.start(0, output) or
       not output.empty() or ended.state() != State::FAILED) {
      return fail("Candidate received after end-of-candidates was processed");
   }

   // An ordinary full agent must also nominate against a peer that only answers checks.
   FullAgent lite_client("g123", "g-password-0123456789012345", Role::CONTROLLED);
   LiteResponder lite_server("h123", "h-password-0123456789012345", "g123");
   if (not lite_client.set_remote_credentials("h123", "h-password-0123456789012345", true) or
       lite_client.role() != Role::CONTROLLING or not lite_client.add_local_candidate(a_candidate) or
       not lite_client.add_remote_candidate(b_candidate)) return fail("ICE-lite setup failed");
   lite_client.end_remote_candidates();
   output.clear();
   if (not lite_client.start(0, output)) return fail("ICE-lite check did not start");
   for (uint64_t now = 0; now < 15000000; now += 100000) {
      std::vector<Outbound> generated;
      for (auto &packet : output) {
         auto result = lite_server.receive(packet.datagram, a_address);
         if (not result.authenticated or result.response.empty()) return fail("ICE-lite check was rejected");
         lite_client.receive(result.response, a_address, b_address, now, generated);
      }
      output = std::move(generated);
      lite_client.tick(now, output);
      if (lite_client.state() IS State::COMPLETED and lite_server.nominated()) break;
   }
   if (lite_client.state() != State::COMPLETED or not lite_server.nominated() or
       *lite_server.nominated() != a_address) return fail("Regular nomination against ICE-lite failed");

   Endpoint a_ipv6;
   a_ipv6.address[15] = 1;
   a_ipv6.port = 20000;
   a_ipv6.ipv6 = true;
   Endpoint b_ipv6 = a_ipv6;
   b_ipv6.address[15] = 2;
   b_ipv6.port = 20001;
   Candidate a_v6_candidate{ a_ipv6, 2130706431, CandidateType::HOST };
   Candidate b_v6_candidate{ b_ipv6, 2130706431, CandidateType::HOST };
   FullAgent ipv6_a("i123", "i-password-0123456789012345", Role::CONTROLLING);
   FullAgent ipv6_b("j123", "j-password-0123456789012345", Role::CONTROLLED);
   if (not ipv6_a.set_remote_credentials("j123", "j-password-0123456789012345", false) or
       not ipv6_b.set_remote_credentials("i123", "i-password-0123456789012345", false) or
       not ipv6_a.add_local_candidate(a_v6_candidate) or not ipv6_a.add_remote_candidate(b_v6_candidate) or
       not ipv6_b.add_local_candidate(b_v6_candidate) or not ipv6_b.add_remote_candidate(a_v6_candidate)) {
      return fail("IPv6 setup failed");
   }
   ipv6_a.end_remote_candidates();
   ipv6_b.end_remote_candidates();
   regular = 0;
   nominations = 0;
   if (not drive(ipv6_a, ipv6_b, a_ipv6, b_ipv6, true, false, regular, nominations)) {
      return fail("IPv6 host pair did not complete");
   }
   // The driven wire queue crosses two 100 ms clock steps for a request/response.
   if (not ipv6_a.selected_rtt_valid() or ipv6_a.selected_rtt_us() != 200000) {
      return fail("ICE RTT did not use injected microseconds");
   }
   ipv6_a.restart("fresh", "fresh-password-0123456789", Role::CONTROLLING);
   if (ipv6_a.selected_rtt_valid()) return fail("Restart retained RTT");
   FullAgent mixed("mix", "mix-password-0123456789012345", Role::CONTROLLING);
   if (not mixed.set_remote_credentials("other", "other-password-0123456789", false) or
       not mixed.add_local_candidate(a_v6_candidate) or not mixed.add_remote_candidate(b_v6_candidate) or
       not mixed.add_local_candidate({a_address, 1000, CandidateType::HOST}) or
       not mixed.add_remote_candidate({b_address, 1000, CandidateType::HOST})) return fail("Dual-stack setup");
   output.clear();
   if (not mixed.start(0, output) or output.size() != 1 or not output.front().remote.ipv6) {
      return fail("Preferred IPv6 check did not start");
   }
   output.clear(); mixed.tick(49999, output);
   if (not output.empty()) return fail("Dual-stack violated check pacing");
   mixed.tick(50000, output);
   if (output.size() != 1 or output.front().remote.ipv6) return fail("IPv4 starved behind unreachable IPv6");
   return 0;
}
