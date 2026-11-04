// Numeric/mDNS ICE candidate identities and Linux/Windows host interface discovery.

#pragma once

#include "ice_agent.h"
#include "../stun/stun.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtc::ice {

struct CandidateLine {
   Candidate candidate;
   std::string mdns_name; // Signalled identity; endpoint.address stays empty until scoped resolution.
   std::string foundation;
   std::string ufrag;
   std::optional<Endpoint> related;
};

bool parse_candidate(std::string_view Text, CandidateLine &Result);
bool parse_numeric_endpoint(std::string_view Text, Endpoint &Result);
std::string format_candidate(const CandidateLine &Value);
void prioritise_host_candidates(std::vector<Candidate> &Candidates);
std::vector<Candidate> gather_host_candidates(uint16_t Port, bool IncludeLoopback = true,
   bool IncludeIPv6 = true);

//********************************************************************************************************************
// One unauthenticated STUN Binding transaction used to discover a server-reflexive address.
// The caller sends request() from Base to Server and passes replies to receive().
class ServerReflexiveGatherer {
public:
   ServerReflexiveGatherer(const Endpoint &Base, const Endpoint &Server);

   [[nodiscard]] bool start();
   [[nodiscard]] const std::vector<uint8_t> & request() const { return request_bytes; }
   [[nodiscard]] bool receive(std::span<const uint8_t> Datagram, const Endpoint &Source, Candidate &Result) const;
   [[nodiscard]] const Endpoint & server() const { return server_endpoint; }

private:
   Endpoint base_endpoint;
   Endpoint server_endpoint;
   std::array<uint8_t, 12> transaction_id{};
   std::vector<uint8_t> request_bytes;
};

} // namespace rtc::ice
