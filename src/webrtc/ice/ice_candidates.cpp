// ICE candidate attribute encoding (RFC 8839 §5.1), mDNS host candidate names, host candidate gathering and
// server-reflexive discovery.

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
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#endif

#include "ice_candidates.h"
#include "../../network/mdns/mdns.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <utility>

namespace rtc::ice {
namespace {

//********************************************************************************************************************
// Splits Text into space-separated words.  Empty words are not returned.

std::vector<std::string_view> words(std::string_view Text)
{
   std::vector<std::string_view> result;

   while (not Text.empty()) {
      auto start = Text.find_first_not_of(' ');
      if (start IS std::string_view::npos) break;
      Text.remove_prefix(start);
      auto end = Text.find(' ');
      result.push_back(Text.substr(0, end));
      if (end IS std::string_view::npos) break;
      Text.remove_prefix(end);
   }

   return result;
}

//********************************************************************************************************************
// Parses an unsigned decimal number that occupies the whole of Text.

bool number(std::string_view Text, uint32_t &Result)
{
   if (Text.empty()) return false;

   auto [end, error] = std::from_chars(Text.data(), Text.data() + Text.size(), Result);
   return error IS std::errc() and end IS Text.data() + Text.size();
}

//********************************************************************************************************************
// Parses an IPv4 or IPv6 address literal.  Host names are not resolved.

bool numeric_address(std::string_view Text, Endpoint &Result)
{
   if (Text.empty() or Text.size() >= INET6_ADDRSTRLEN) return false;

   std::string name(Text);
   Result = {};

   if (inet_pton(AF_INET, name.c_str(), Result.address.data()) IS 1) return true;
   if (inet_pton(AF_INET6, name.c_str(), Result.address.data()) IS 1) {
      Result.ipv6 = true;
      return true;
   }

   return false;
}

//********************************************************************************************************************
// Formats an endpoint's address in its canonical text form.

std::string address_text(const Endpoint &Value)
{
   std::array<char, INET6_ADDRSTRLEN> buffer{};
   if (not inet_ntop(Value.ipv6 ? AF_INET6 : AF_INET, Value.address.data(), buffer.data(), buffer.size())) return {};
   return buffer.data();
}

//********************************************************************************************************************

const char * type_name(CandidateType Type)
{
   switch (Type) {
      case CandidateType::RELAY: return "relay";
      case CandidateType::HOST: return "host";
      case CandidateType::SERVER_REFLEXIVE: return "srflx";
      case CandidateType::PEER_REFLEXIVE: return "prflx";
   }
   return "";
}

//********************************************************************************************************************
// A foundation is 1-32 ice-char characters.

bool valid_foundation(std::string_view Value)
{
   if (Value.empty() or Value.size() > 32) return false;

   for (char ch : Value) {
      if (not ((ch >= '0' and ch <= '9') or (ch >= 'A' and ch <= 'Z') or
          (ch >= 'a' and ch <= 'z') or ch IS '+' or ch IS '/')) return false;
   }

   return true;
}

} // namespace

//********************************************************************************************************************
// Parses "address:port", where an IPv6 address must be enclosed in brackets.

bool parse_numeric_endpoint(std::string_view Text, Endpoint &Result)
{
   std::string_view address, port_text;
   if (Text.starts_with('[')) {
      auto close = Text.find(']');
      if (close IS std::string_view::npos or close + 1 >= Text.size() or Text[close + 1] != ':') return false;
      address = Text.substr(1, close - 1);
      port_text = Text.substr(close + 2);
   }
   else {
      auto separator = Text.find(':');
      if (separator IS std::string_view::npos) return false;
      address = Text.substr(0, separator);
      port_text = Text.substr(separator + 1);
   }

   uint32_t port = 0;
   Endpoint parsed;
   if (not number(port_text, port) or port IS 0 or port > 65535 or
       not numeric_address(address, parsed) or
       (parsed.ipv6 != Text.starts_with('['))) return false;

   parsed.port = uint16_t(port);
   Result = parsed;
   return true;
}

//********************************************************************************************************************
// Parses a candidate attribute value.  Only UDP candidates for component 1 are accepted.  A host candidate may use an
// mDNS name in place of its address, which must then be resolved before use, and it may not reveal a related address.

bool parse_candidate(std::string_view Text, CandidateLine &Result)
{
   if (not Text.starts_with("candidate:") or Text.size() > 2048) return false;

   auto fields = words(Text.substr(10));
   if (fields.size() < 8 or ((fields.size() - 8) % 2) or
       not valid_foundation(fields[0]) or fields[6] != "typ") return false;
   if (fields[2] != "udp" and fields[2] != "UDP") return false;

   uint32_t component = 0, priority = 0, port = 0;
   if (not number(fields[1], component) or component != 1 or
       not number(fields[3], priority) or priority IS 0 or
       not number(fields[5], port) or port IS 0 or port > 65535) return false;

   CandidateLine parsed;
   if (not numeric_address(fields[4], parsed.candidate.endpoint)) {
      if (not net::mdns::valid_name(fields[4]) or fields[7] != "host") return false;
      parsed.mdns_name = fields[4];
      for (auto &ch : parsed.mdns_name) ch = char(std::tolower((unsigned char)ch));
   }

   parsed.candidate.endpoint.port = uint16_t(port);
   parsed.candidate.priority = priority;
   parsed.foundation = fields[0];
   parsed.candidate.foundation = parsed.foundation;

   if (fields[7] IS "host") parsed.candidate.type = CandidateType::HOST;
   else if (fields[7] IS "srflx") parsed.candidate.type = CandidateType::SERVER_REFLEXIVE;
   else if (fields[7] IS "relay") parsed.candidate.type = CandidateType::RELAY;
   else if (fields[7] IS "prflx") parsed.candidate.type = CandidateType::PEER_REFLEXIVE;
   else return false;

   // Extension attributes are name-value pairs; unrecognised names are ignored.

   std::string_view related_address;
   uint32_t related_port = 0;
   for (size_t i = 8; i + 1 < fields.size(); i += 2) {
      if (fields[i] IS "ufrag") {
         if (not parsed.ufrag.empty() or fields[i + 1].empty() or fields[i + 1].size() > 256) return false;
         parsed.ufrag = fields[i + 1];
      }
      else if (fields[i] IS "raddr") {
         if (not related_address.empty()) return false;
         related_address = fields[i + 1];
      }
      else if (fields[i] IS "rport") {
         if (related_port or not number(fields[i + 1], related_port) or related_port > 65535) return false;
      }
   }

   if (not parsed.mdns_name.empty() and (not related_address.empty() or related_port)) return false;

   if (not related_address.empty() or related_port) {
      Endpoint related;
      if (related_address.empty() or related_port IS 0 or
          not numeric_address(related_address, related) or
          related.ipv6 != parsed.candidate.endpoint.ipv6) return false;
      related.port = uint16_t(related_port);
      parsed.related = related;
   }

   Result = std::move(parsed);
   return true;
}

//********************************************************************************************************************
// Formats a candidate attribute value, returning an empty string if the candidate cannot be represented.

std::string format_candidate(const CandidateLine &Value)
{
   if (not valid_foundation(Value.foundation) or Value.candidate.endpoint.port IS 0 or
       Value.candidate.priority IS 0) return {};

   if (not Value.mdns_name.empty() and (not net::mdns::valid_name(Value.mdns_name) or
       Value.candidate.type != CandidateType::HOST or Value.related)) return {};

   auto address = Value.mdns_name.empty() ? address_text(Value.candidate.endpoint) : Value.mdns_name;
   auto type = type_name(Value.candidate.type);
   if (address.empty() or not *type) return {};

   auto result = "candidate:" + Value.foundation + " 1 udp " + std::to_string(Value.candidate.priority) +
      " " + address + " " + std::to_string(Value.candidate.endpoint.port) + " typ " + type;

   if (Value.related) {
      if (Value.related->port IS 0 or Value.related->ipv6 != Value.candidate.endpoint.ipv6) return {};
      auto related = address_text(*Value.related);
      if (related.empty()) return {};
      result += " raddr " + related + " rport " + std::to_string(Value.related->port);
   }

   if (not Value.ufrag.empty()) {
      if (Value.ufrag.find_first_of(" \r\n\t") != std::string::npos) return {};
      result += " ufrag " + Value.ufrag;
   }

   return result;
}

//********************************************************************************************************************
// Interleaves IPv6 and IPv4 host candidates (RFC 8421) and reassigns their local preferences in the resulting order.
// Type preference 126 is used for host candidates.

void prioritise_host_candidates(std::vector<Candidate> &Candidates)
{
   std::vector<Candidate> ordered;
   ordered.reserve(Candidates.size());
   size_t ipv4 = 0, ipv6 = 0;

   while (ordered.size() < Candidates.size()) {
      // Assign the interleaving in signalled local preferences as well as ordinary-check pacing.
      // Prefer IPv6 within each round; preserve interface order within an address family.
      for (bool family : {true, false}) {
         auto &index = family ? ipv6 : ipv4;
         while (index < Candidates.size() and Candidates[index].endpoint.ipv6 != family) ++index;
         if (index IS Candidates.size()) continue;
         auto candidate = Candidates[index++];
         auto preference = 65535u - uint32_t(ordered.size());
         candidate.priority = (126u << 24) | (preference << 8) | 255u;
         ordered.push_back(std::move(candidate));
      }
   }

   Candidates = std::move(ordered);
}

//********************************************************************************************************************
// Gathers up to 32 host candidates from the interfaces that are up.  Link-local IPv6 addresses are excluded because
// Endpoint has no scope identifier.

std::vector<Candidate> gather_host_candidates(uint16_t Port, bool IncludeLoopback, bool IncludeIPv6)
{
   std::vector<Candidate> result;
   if (Port IS 0) return result;

#ifdef _WIN32
   ULONG size = 16384;
   std::vector<uint8_t> buffer(size);
   ULONG status = ERROR_BUFFER_OVERFLOW;
   for (int attempt = 0; attempt < 3 and status IS ERROR_BUFFER_OVERFLOW; ++attempt) {
      buffer.resize(size);
      status = GetAdaptersAddresses(IncludeIPv6 ? AF_UNSPEC : AF_INET,
         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
         (IP_ADAPTER_ADDRESSES *)buffer.data(), &size);
   }

   if (status != NO_ERROR) return result;

   for (auto adapter = (IP_ADAPTER_ADDRESSES *)buffer.data(); adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp or
          (not IncludeLoopback and adapter->IfType IS IF_TYPE_SOFTWARE_LOOPBACK)) continue;
      for (auto scan = adapter->FirstUnicastAddress; scan and result.size() < 32; scan = scan->Next) {
         auto address = scan->Address.lpSockaddr;
         if (not address or scan->DadState != IpDadStatePreferred) continue;
         auto family = address->sa_family;
         if (family != AF_INET and (family != AF_INET6 or not IncludeIPv6)) continue;

         Candidate candidate;
         candidate.endpoint.port = Port;
         candidate.endpoint.ipv6 = family IS AF_INET6;
         if (candidate.endpoint.ipv6) {
            auto ipv6 = (sockaddr_in6 *)address;
            if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) continue;
            std::memcpy(candidate.endpoint.address.data(), &ipv6->sin6_addr, 16);
         }
         else {
            std::memcpy(candidate.endpoint.address.data(), &((sockaddr_in *)address)->sin_addr, 4);
         }

         bool duplicate = false;
         for (const auto &existing : result) if (existing.endpoint IS candidate.endpoint) duplicate = true;
         if (duplicate) continue;
         candidate.priority = (126u << 24) | ((65535u - uint32_t(result.size())) << 8) | 255u;
         result.push_back(candidate);
      }
   }
#else
   ifaddrs *interfaces = nullptr;
   if (getifaddrs(&interfaces) != 0) return result;

   for (auto scan = interfaces; scan and result.size() < 32; scan = scan->ifa_next) {
      if (not scan->ifa_addr or not (scan->ifa_flags & IFF_UP) or
          (not IncludeLoopback and (scan->ifa_flags & IFF_LOOPBACK))) continue;
      auto family = scan->ifa_addr->sa_family;
      if (family != AF_INET and (family != AF_INET6 or not IncludeIPv6)) continue;

      Candidate candidate;
      candidate.endpoint.port = Port;
      candidate.endpoint.ipv6 = family IS AF_INET6;
      if (candidate.endpoint.ipv6) {
         auto address = (sockaddr_in6 *)scan->ifa_addr;
         if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) continue; // Endpoint does not carry a scope identifier.
         std::memcpy(candidate.endpoint.address.data(), &address->sin6_addr, 16);
      }
      else {
         auto address = (sockaddr_in *)scan->ifa_addr;
         std::memcpy(candidate.endpoint.address.data(), &address->sin_addr, 4);
      }

      bool duplicate = false;
      for (const auto &existing : result) if (existing.endpoint IS candidate.endpoint) duplicate = true;
      if (duplicate) continue;

      uint32_t local_preference = 65535u - uint32_t(result.size());
      candidate.priority = (126u << 24) | (local_preference << 8) | 255u;
      result.push_back(candidate);
   }

   freeifaddrs(interfaces);
#endif

   prioritise_host_candidates(result);
   return result;
}

//********************************************************************************************************************

ServerReflexiveGatherer::ServerReflexiveGatherer(const Endpoint &Base, const Endpoint &Server) :
   base_endpoint(Base), server_endpoint(Server) { }

//********************************************************************************************************************
// Encodes an unauthenticated Binding request with a random transaction ID.

bool ServerReflexiveGatherer::start()
{
   request_bytes.clear();
   if (base_endpoint.port IS 0 or server_endpoint.port IS 0 or
       base_endpoint.ipv6 != server_endpoint.ipv6 or
       RAND_bytes(transaction_id.data(), transaction_id.size()) != 1) return false;

   return stun::encode(stun::BINDING_REQUEST, transaction_id, {}, {}, false, true, request_bytes);
}

//********************************************************************************************************************
// Accepts the server's Binding success response and returns the server-reflexive candidate.  The mapped address must be
// of the same family as the base.  Type preference 100 is used for server-reflexive candidates.

bool ServerReflexiveGatherer::receive(std::span<const uint8_t> Datagram, const Endpoint &Source,
   Candidate &Result) const
{
   if (request_bytes.empty() or Source != server_endpoint) return false;

   stun::Message parsed;
   if (not stun::parse(Datagram, parsed) or parsed.type != stun::BINDING_SUCCESS or
       parsed.transaction_id != transaction_id or
       not stun::unknown_required_attributes(parsed).empty()) return false;

   auto fingerprint = parsed.find(stun::ATTR_FINGERPRINT);
   if (fingerprint and not stun::verify_fingerprint(Datagram, parsed)) return false;

   size_t mapped_count = 0;
   for (const auto &attribute : parsed.attributes) {
      if (attribute.type IS stun::ATTR_XOR_MAPPED_ADDRESS) ++mapped_count;
   }
   if (mapped_count != 1) return false;

   auto mapped = parsed.find(stun::ATTR_XOR_MAPPED_ADDRESS);
   Candidate candidate;
   if (not stun::decode_xor_address(*mapped, parsed, candidate.endpoint.address,
       candidate.endpoint.port, candidate.endpoint.ipv6) or
       candidate.endpoint.port IS 0 or candidate.endpoint.ipv6 != base_endpoint.ipv6) return false;

   candidate.type = CandidateType::SERVER_REFLEXIVE;
   candidate.priority = (100u << 24) | (65535u << 8) | 255u;
   candidate.base = base_endpoint;
   Result = candidate;
   return true;
}

} // namespace rtc::ice
