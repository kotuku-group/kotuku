// ICE-lite connectivity check responder (RFC 8445 §2.5).  Binding requests are authenticated with the local password
// and answered with the source address, without any checks being sent in return.

#include "ice.h"
#include "../stun/stun.h"

#include <algorithm>
#include <array>

namespace rtc::ice {
namespace {

// ICE attributes and the STUN error response type

constexpr uint16_t ATTR_PRIORITY = 0x0024;
constexpr uint16_t ATTR_USE_CANDIDATE = 0x0025;
constexpr uint16_t ATTR_ICE_CONTROLLED = 0x8029;
constexpr uint16_t ATTR_ICE_CONTROLLING = 0x802a;
constexpr uint16_t ATTR_ERROR_CODE = 0x0009;
constexpr uint16_t BINDING_ERROR = 0x0111;

} // namespace

//********************************************************************************************************************

LiteResponder::LiteResponder(std::string_view LocalUfrag, std::string_view LocalPassword,
   std::string_view RemoteUfrag) :
   local_ufrag(LocalUfrag), local_password(LocalPassword), remote_ufrag(RemoteUfrag) { }

//********************************************************************************************************************
// Processes a datagram that may be a connectivity check.  The request must carry USERNAME, PRIORITY, MESSAGE-INTEGRITY
// and FINGERPRINT in that order.  A controlled peer is told of the role conflict with a 487 error, because a lite agent
// is always controlled.  An authenticated check validates its source address, and USE-CANDIDATE nominates it.

CheckResult LiteResponder::receive(std::span<const uint8_t> Datagram, const Endpoint &Source)
{
   CheckResult result;
   if (Source.port IS 0 or local_ufrag.empty() or local_password.empty() or remote_ufrag.empty()) return result;

   stun::Message request;
   if (not stun::parse(Datagram, request) or request.type != stun::BINDING_REQUEST) return result;
   if (not stun::valid_attribute(request, stun::ATTR_USERNAME, local_ufrag.size() + 1 + remote_ufrag.size(), true) or
       not stun::valid_attribute(request, stun::ATTR_MESSAGE_INTEGRITY, 20, true) or
       not stun::valid_attribute(request, stun::ATTR_FINGERPRINT, 4, true) or
       not stun::valid_attribute(request, ATTR_PRIORITY, 4, true) or
       not stun::valid_attribute(request, ATTR_USE_CANDIDATE, 0, false) or
       not stun::valid_attribute(request, ATTR_ICE_CONTROLLING, 8, false) or
       not stun::valid_attribute(request, ATTR_ICE_CONTROLLED, 8, false)) return result;

   auto username = request.find(stun::ATTR_USERNAME);
   auto integrity = request.find(stun::ATTR_MESSAGE_INTEGRITY);
   auto fingerprint = request.find(stun::ATTR_FINGERPRINT);
   if (integrity->offset >= fingerprint->offset or
       username->offset >= integrity->offset or
       request.find(ATTR_PRIORITY)->offset >= integrity->offset) return result;

   for (const auto &attribute : request.attributes) {
      if (attribute.offset > integrity->offset and attribute.type != stun::ATTR_FINGERPRINT) return result;
   }

   auto expected = local_ufrag + ":" + remote_ufrag;
   auto actual = std::string_view((const char *)username->value.data(), username->value.size());
   if (actual != expected) return result;

   auto key = std::span<const uint8_t>((const uint8_t *)local_password.data(), local_password.size());
   if (not stun::verify_integrity(Datagram, request, key) or
       not stun::verify_fingerprint(Datagram, request)) return result;
   result.authenticated = true;

   auto unknown = stun::unknown_required_attributes(request);
   if (not unknown.empty()) {
      (void)stun::encode_unknown_attributes_error(request, unknown, key, result.response);
      return result;
   }

   auto controlling = request.find(ATTR_ICE_CONTROLLING);
   auto controlled = request.find(ATTR_ICE_CONTROLLED);
   if ((controlling and controlled) or (not controlling and not controlled)) return result;

   if (controlled) {
      std::array<uint8_t, 4> error_code{ 0, 0, 4, 87 };
      stun::Attribute error{ ATTR_ERROR_CODE, error_code, 0 };
      result.role_conflict = stun::encode(BINDING_ERROR, request.transaction_id,
         std::span<const stun::Attribute>(&error, 1), key, false, true, result.response);
      return result;
   }

   auto mapped = stun::encode_xor_address(Source.address, Source.port, Source.ipv6, request.transaction_id);

   stun::Attribute address{ stun::ATTR_XOR_MAPPED_ADDRESS,
      std::span<const uint8_t>(mapped.data(), Source.ipv6 ? 20u : 8u), 0 };
   if (not stun::encode(stun::BINDING_SUCCESS, request.transaction_id,
       std::span<const stun::Attribute>(&address, 1), key, false, true, result.response)) {
      result.authenticated = false;
      return result;
   }

   // Retain up to 16 validated sources, evicting the oldest that is not nominated.

   if (not validated(Source)) {
      if (valid_sources.size() >= 16) {
         auto evict = std::find_if(valid_sources.begin(), valid_sources.end(), [this](const Endpoint &Candidate) {
            return not has_nomination or Candidate != nominated_source;
         });
         if (evict != valid_sources.end()) valid_sources.erase(evict);
         else return result;
      }
      valid_sources.push_back(Source);
   }

   if (request.find(ATTR_USE_CANDIDATE)) {
      nominated_source = Source;
      has_nomination = true;
      result.nominated = true;
   }

   return result;
}

//********************************************************************************************************************

bool LiteResponder::validated(const Endpoint &Source) const
{
   return std::find(valid_sources.begin(), valid_sources.end(), Source) != valid_sources.end();
}

//********************************************************************************************************************

const Endpoint * LiteResponder::nominated() const
{
   return has_nomination ? &nominated_source : nullptr;
}

} // namespace rtc::ice
