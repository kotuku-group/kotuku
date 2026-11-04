#include "../sdp/sdp.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace rtc::sdp;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

static std::string fixture(const char *Name) {
   std::ifstream input(std::string(WEBRTC_FIXTURE_DIR) + "/" + Name, std::ios::binary);
   return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

static bool replace(std::string &Text, std::string_view Old, std::string_view New) {
   auto pos = Text.find(Old);
   if (pos IS std::string::npos) return false;
   Text.replace(pos, Old.size(), New);
   return true;
}

int main() {
   auto offer_text = fixture("chromium_141_datachannel_offer.sdp");
   auto answer_text = fixture("chromium_141_datachannel_answer.sdp");
   Description offer, answer;
   if (not parse(offer_text, offer) or not parse(answer_text, answer)) return fail("Chromium SDP fixture rejected");
   if (offer.data.setup != Setup::ACTPASS or answer.data.setup != Setup::ACTIVE or
       offer.data.sctp_port != 5000 or answer.data.max_message_size != 262144 or
       not validate_answer(offer, answer)) return fail("Chromium negotiation fields differ");

   auto rejected = answer_text;
   Description rejected_description;
   if (not replace(rejected, "m=application 9 ", "m=application 0 ") or
       parse(rejected, rejected_description)) return fail("Rejected data-channel answer accepted as active");
   rejected = offer_text;
   if (not replace(rejected, "m=application 9 ", "m=application 0 ") or
       parse(rejected, rejected_description)) return fail("Rejected data-channel offer accepted as active");
   if (parse(offer_text + "m=audio 9 UDP/TLS/RTP/SAVPF 111\nc=IN IP4 0.0.0.0\n", rejected_description)) {
      return fail("Data-channel parser accepted a media section it cannot answer");
   }

   Description round_trip;
   auto wire = serialize(offer);
   if (wire.empty() or wire.find("\r\n") IS std::string::npos or not parse(wire, round_trip) or
       round_trip.data.ice_pwd != offer.data.ice_pwd or round_trip.data.fingerprint != offer.data.fingerprint) {
      return fail("CRLF serialisation round trip failed");
   }

   auto inherited = offer_text;
   if (not replace(inherited, "a=ice-ufrag:meZ0\n", "") or
       not replace(inherited, "a=ice-pwd:u/tJCzNqCbQrUOLGSCW3/qLj\n", "") or
       not replace(inherited, "a=setup:actpass\n", "") or
       not replace(inherited, "t=0 0\n", "t=0 0\na=ice-ufrag:meZ0\na=ice-pwd:u/tJCzNqCbQrUOLGSCW3/qLj\n"
          "a=setup:actpass\n")) return fail("Fixture edit failed");
   Description inherited_result;
   if (not parse(inherited, inherited_result) or inherited_result.data.ice_pwd != offer.data.ice_pwd) {
      return fail("Session-level ICE inheritance failed");
   }

   auto candidate_text = offer_text + "a=candidate:1 1 udp 2122260223 192.0.2.1 5000 typ host\n"
      "a=end-of-candidates\n";
   Description candidates;
   if (not parse(candidate_text, candidates) or candidates.data.candidates.size() != 1 or
       not candidates.data.end_of_candidates or not parse(serialize(candidates), round_trip) or
       round_trip.data.candidates.size() != 1) return fail("Candidate round trip failed");

   auto mux_text = offer_text + "a=rtcp-mux\n";
   Description mux;
   if (not parse(mux_text, mux) or not mux.data.rtcp_mux or
       serialize(mux).find("a=rtcp-mux\r\n") IS std::string::npos or
       not parse(serialize(mux), round_trip) or not round_trip.data.rtcp_mux) {
      return fail("RTCP mux attribute was not preserved");
   }

   auto malformed = offer_text;
   if (not replace(malformed, "a=group:BUNDLE 0", "a=group:BUNDLE 1") or parse(malformed, round_trip)) {
      return fail("Unbundled data m-line accepted");
   }
   malformed = offer_text;
   if (not replace(malformed, "a=ice-pwd:u/tJCzNqCbQrUOLGSCW3/qLj", "a=ice-pwd:short") or
       parse(malformed, round_trip)) return fail("Short ICE password accepted");
   malformed = offer_text;
   if (not replace(malformed, "a=sctp-port:5000", "a=sctp-port:65536") or
       parse(malformed, round_trip)) return fail("Invalid SCTP port accepted");
   malformed = offer_text;
   if (not replace(malformed, "a=fingerprint:sha-256 A0", "a=fingerprint:sha-256 Z0") or
       parse(malformed, round_trip)) return fail("Invalid fingerprint accepted");
   malformed = candidate_text;
   if (not replace(malformed, "192.0.2.1 5000 typ host", "192.0.2.1 70000 typ host") or
       parse(malformed, round_trip)) return fail("Invalid candidate port accepted");
   malformed = offer_text;
   malformed += "a=ice-ufrag:duplicate\n";
   if (parse(malformed, round_trip)) return fail("Duplicate ICE attribute accepted");

   answer.data.setup = Setup::ACTPASS;
   if (validate_answer(offer, answer)) return fail("Actpass answer accepted");
   return 0;
}
