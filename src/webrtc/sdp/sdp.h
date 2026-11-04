// Data-channel SDP subset used by JSEP (RFC 8829).  No object-system dependencies.

#pragma once

#include <kotuku/config.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rtc::sdp {

enum class Setup { ACTPASS, ACTIVE, PASSIVE };

struct DataChannel {
   std::string mid;
   std::string ice_ufrag;
   std::string ice_pwd;
   std::string fingerprint; // Upper-case colon-separated SHA-256 bytes, without the algorithm name.
   Setup setup = Setup::ACTPASS;
   uint16_t sctp_port = 5000;
   uint64_t max_message_size = 262144;
   bool rtcp_mux = false;
   std::vector<std::string> candidates; // Attribute values, beginning with "candidate:".
   bool end_of_candidates = false;
};

struct Description {
   std::string session_id;
   uint64_t session_version = 2;
   std::string origin_address = "127.0.0.1";
   bool origin_ipv6 = false;
   bool ice_lite = false;
   bool trickle = true;
   DataChannel data;
};

// Parse the first bundled application m-line.  Unknown SDP attributes are ignored, as JSEP requires.
// Rejected (port-zero) application sections are unsupported and return false.
// Both CRLF and LF line endings are accepted.  On failure Result is left unchanged.
bool parse(std::string_view Text, Description &Result);

// Validate the SDP candidate syntax and report whether its transport is UDP.
bool valid_candidate_line(std::string_view Text, bool &Udp);

// Serialise a validated data-channel description.  Empty string means invalid input.
std::string serialize(const Description &Value);

// Check the offer/answer roles, BUNDLE MID and ICE/DTLS parameters used for a data-channel negotiation.
bool validate_answer(const Description &Offer, const Description &Answer);

} // namespace rtc::sdp
