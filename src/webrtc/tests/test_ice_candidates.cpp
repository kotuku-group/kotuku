#include "../ice/ice_candidates.h"

#include <cstdio>
#include <array>
#include <string_view>
#include <vector>

using namespace rtc::ice;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

int main() {
   Endpoint numeric;
   if (not parse_numeric_endpoint("192.0.2.1:3478", numeric) or numeric.ipv6 or
       numeric.port != 3478 or numeric.address[0] != 192 or
       not parse_numeric_endpoint("[2001:db8::1]:3478", numeric) or not numeric.ipv6 or
       parse_numeric_endpoint("2001:db8::1:3478", numeric) or
       parse_numeric_endpoint("localhost:3478", numeric) or
       parse_numeric_endpoint("127.0.0.1:0", numeric) or
       parse_numeric_endpoint("127.0.0.1:65536", numeric)) return fail("Numeric STUN endpoint parse failed");
   CandidateLine host;
   if (not parse_candidate("candidate:1 1 udp 2130706431 127.0.0.1 54321 typ host generation 0", host) or
       host.candidate.endpoint.port != 54321 or host.candidate.endpoint.address[0] != 127 or
       host.candidate.type != CandidateType::HOST or host.foundation != "1") return fail("IPv4 host parse failed");
   CandidateLine round_trip;
   if (not parse_candidate(format_candidate(host), round_trip) or
       round_trip.candidate.endpoint != host.candidate.endpoint or
       round_trip.candidate.priority != host.candidate.priority) return fail("Host candidate round trip failed");

   CandidateLine reflexive;
   if (not parse_candidate("candidate:abc 1 UDP 1694498815 2001:db8::1 45678 typ srflx "
       "raddr 2001:db8::2 rport 12345 generation 0", reflexive) or
       not reflexive.candidate.endpoint.ipv6 or not reflexive.related or
       reflexive.related->port != 12345 or
       not parse_candidate(format_candidate(reflexive), round_trip) or
       round_trip.related != reflexive.related) return fail("IPv6 reflexive candidate round trip failed");

   if (parse_candidate("candidate:1 2 udp 1 127.0.0.1 1234 typ host", round_trip) or
       parse_candidate("candidate:1 1 udp 1 host.local 1234 typ relay", round_trip) or
       parse_candidate("candidate:1 1 udp 1 127.0.0.1 70000 typ host", round_trip) or
       parse_candidate("candidate:1 1 udp 1 127.0.0.1 1234 typ host raddr", round_trip)) {
      return fail("Malformed numeric candidate accepted");
   }

   if (not parse_candidate("candidate:1 1 udp 1 AbCd.local 1234 typ host", round_trip) or
       round_trip.mdns_name != "abcd.local" or round_trip.candidate.endpoint.address[0] != 0 or
       format_candidate(round_trip) != "candidate:1 1 udp 1 abcd.local 1234 typ host" or
       parse_candidate("candidate:1 1 udp 1 a.b.local 1234 typ host", round_trip) or
       parse_candidate("candidate:1 1 udp 1 example.com 1234 typ host", round_trip) or
       parse_candidate("candidate:1 1 udp 1 host.local 1234 typ host raddr 192.0.2.1 rport 1234", round_trip)) {
      return fail("mDNS candidate profile or identity failed");
   }
   std::vector<Candidate> families(3);
   families[0].endpoint.ipv6 = true;
   families[1].endpoint.ipv6 = true;
   families[2].endpoint.ipv6 = false;
   prioritise_host_candidates(families);
   if (not families[0].endpoint.ipv6 or families[1].endpoint.ipv6 or not families[2].endpoint.ipv6 or
       families[0].priority <= families[1].priority or families[1].priority <= families[2].priority) {
      return fail("Signalled family preferences did not interleave IPv6/IPv4");
   }
   auto hosts = gather_host_candidates(22222);
   bool ipv4_loopback = false;
   for (const auto &candidate : hosts) {
      if (candidate.endpoint.port != 22222 or candidate.priority IS 0) return fail("Host priority or port invalid");
      if (not candidate.endpoint.ipv6 and candidate.endpoint.address[0] IS 127) ipv4_loopback = true;
   }
   if (not ipv4_loopback) return fail("IPv4 loopback host candidate missing");
   auto no_loopback = gather_host_candidates(22222, false);
   for (const auto &candidate : no_loopback) {
      if (not candidate.endpoint.ipv6 and candidate.endpoint.address[0] IS 127) {
         return fail("Loopback appeared when excluded");
      }
   }
   Endpoint base, server;
   base.address = { 192, 0, 2, 2 };
   base.port = 40000;
   server.address = { 198, 51, 100, 1 };
   server.port = 3478;
   ServerReflexiveGatherer gatherer(base, server);
   if (not gatherer.start()) return fail("Server-reflexive request generation failed");
   rtc::stun::Message request;
   if (not rtc::stun::parse(gatherer.request(), request) or
       request.type != rtc::stun::BINDING_REQUEST) return fail("Invalid gathering request");
   uint16_t mapped_port = 50000;
   std::array<uint8_t, 8> xor_address{
      0, 1, uint8_t((mapped_port ^ 0x2112) >> 8), uint8_t(mapped_port ^ 0x2112),
      uint8_t(203 ^ 0x21), uint8_t(0 ^ 0x12), uint8_t(113 ^ 0xa4), uint8_t(9 ^ 0x42)
   };
   rtc::stun::Attribute mapped{ rtc::stun::ATTR_XOR_MAPPED_ADDRESS, xor_address, 0 };
   std::vector<uint8_t> response;
   if (not rtc::stun::encode(rtc::stun::BINDING_SUCCESS, request.transaction_id,
       std::span<const rtc::stun::Attribute>(&mapped, 1), {}, false, true, response)) {
      return fail("Server-reflexive response generation failed");
   }
   Candidate discovered;
   if (not gatherer.receive(response, server, discovered) or
       discovered.endpoint.address[0] != 203 or discovered.endpoint.address[3] != 9 or
       discovered.endpoint.port != mapped_port or discovered.base != base or
       discovered.type != CandidateType::SERVER_REFLEXIVE) return fail("Server-reflexive mapping failed");
   std::array<uint8_t, 4> unknown_value{1, 2, 3, 4};
   std::array<rtc::stun::Attribute, 2> unknown_attributes{{mapped, {0x1234, unknown_value}}};
   std::vector<uint8_t> unknown_response;
   if (not rtc::stun::encode(rtc::stun::BINDING_SUCCESS, request.transaction_id,
       unknown_attributes, {}, false, true, unknown_response) or
       gatherer.receive(unknown_response, server, discovered)) {
      return fail("Server-reflexive gathering accepted an unknown required attribute");
   }
   auto stranger = server;
   stranger.port++;
   if (gatherer.receive(response, stranger, discovered)) return fail("Wrong STUN server accepted");
   response.back() ^= 1;
   if (gatherer.receive(response, server, discovered)) return fail("Corrupt STUN fingerprint accepted");
   return 0;
}
