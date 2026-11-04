// Full ICE agent (RFC 8445) for a single bundled UDP component, with consent freshness (RFC 7675).  The agent forms
// a checklist from local and remote candidates, paces connectivity checks, resolves role conflicts, and nominates or
// accepts the nomination of a pair.  Once a pair is selected, consent is refreshed every four to six seconds and the
// pair is considered disconnected after 30 seconds without a response.

#include "ice_agent.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace rtc::ice {
namespace {

// ICE attributes and the STUN error response type

constexpr uint16_t ATTR_PRIORITY = 0x0024;
constexpr uint16_t ATTR_USE_CANDIDATE = 0x0025;
constexpr uint16_t ATTR_ICE_CONTROLLED = 0x8029;
constexpr uint16_t ATTR_ICE_CONTROLLING = 0x802a;
constexpr uint16_t BINDING_ERROR = 0x0111;

//********************************************************************************************************************
// The priority that a candidate would have as a peer-reflexive candidate, as sent in the PRIORITY attribute of a
// check (RFC 8445 §7.1.1).  Type preference 110 is used.

uint32_t peer_reflexive_priority(const Candidate &Value)
{
   return (110u << 24) | (Value.priority & 0x00ffffffu);
}

//********************************************************************************************************************
// Big-endian field accessors

void write32(uint8_t *Bytes, uint32_t Value)
{
   Bytes[0] = uint8_t(Value >> 24);
   Bytes[1] = uint8_t(Value >> 16);
   Bytes[2] = uint8_t(Value >> 8);
   Bytes[3] = uint8_t(Value);
}

void write64(uint8_t *Bytes, uint64_t Value)
{
   for (int i = 0; i < 8; ++i) Bytes[i] = uint8_t(Value >> (56 - i * 8));
}

uint32_t read32(const uint8_t *Bytes)
{
   return (uint32_t(Bytes[0]) << 24) | (uint32_t(Bytes[1]) << 16) |
      (uint32_t(Bytes[2]) << 8) | uint32_t(Bytes[3]);
}

uint64_t read64(const uint8_t *Bytes)
{
   uint64_t value = 0;
   for (int i = 0; i < 8; ++i) value = (value << 8) | Bytes[i];
   return value;
}

//********************************************************************************************************************

bool valid_endpoint(const Endpoint &Value)
{
   return Value.port != 0;
}

//********************************************************************************************************************
// Returns the local endpoint that a candidate sends from: its base for a reflexive candidate, and the candidate
// itself for host and relayed candidates.

const Endpoint & socket_endpoint(const Candidate &Value)
{
   return Value.type IS CandidateType::RELAY ? Value.endpoint :
      (valid_endpoint(Value.base) ? Value.base : Value.endpoint);
}

//********************************************************************************************************************

std::span<const uint8_t> password_bytes(const std::string &Password)
{
   return { (const uint8_t *)Password.data(), Password.size() };
}

} // namespace

//********************************************************************************************************************
// The tie-breaker is a random 64-bit value that resolves role conflicts.  The size of the checklist is limited to
// PairLimit pairs, within the range 1-2048.

FullAgent::FullAgent(std::string_view LocalUfrag, std::string_view LocalPassword, Role InitialRole, size_t PairLimit) :
   local_ufrag(LocalUfrag), local_password(LocalPassword), current_role(InitialRole),
   pair_limit(std::clamp(PairLimit, size_t(1), size_t(2048)))
{
   (void)RAND_bytes((uint8_t *)&tie_breaker, sizeof(tie_breaker));
}

//********************************************************************************************************************
// Sets the remote credentials.  A full agent is always controlling when the peer is an ICE-lite agent.

bool FullAgent::set_remote_credentials(std::string_view RemoteUfrag, std::string_view RemotePassword,
   bool RemoteLite)
{
   if (RemoteUfrag.empty() or RemotePassword.empty() or current_state != State::NEW) return false;

   remote_ufrag = RemoteUfrag;
   remote_password = RemotePassword;
   remote_lite = RemoteLite;

   if (remote_lite) {
      current_role = Role::CONTROLLING;
      recompute_priorities();
   }

   return true;
}

//********************************************************************************************************************
// Computes a pair priority from the candidate priorities (RFC 8445 §6.1.2.3).

uint64_t FullAgent::pair_priority(uint32_t Local, uint32_t Remote, Role CurrentRole)
{
   uint64_t controlling = CurrentRole IS Role::CONTROLLING ? Local : Remote;
   uint64_t controlled = CurrentRole IS Role::CONTROLLING ? Remote : Local;
   return (uint64_t(1) << 32) * std::min(controlling, controlled) +
      2 * std::max(controlling, controlled) + (controlling > controlled ? 1 : 0);
}

//********************************************************************************************************************
// Adds a pair to the checklist.  Redundant pairs that share a sending socket and remote endpoint are pruned.  If the
// checklist is full, the new pair replaces the lowest-priority pair that is idle and has not succeeded.

void FullAgent::form_pair(const Candidate &Local, const Candidate &Remote)
{
   if (Local.endpoint.ipv6 != Remote.endpoint.ipv6) return;

   for (auto &pair : pairs) {
      if (socket_endpoint(pair.local) IS socket_endpoint(Local) and pair.remote.endpoint IS Remote.endpoint) {
         // Reflexive candidates sharing a base describe the same outbound check route (RFC 8445 6.1.2.4).
         auto priority = pair_priority(Local.priority, Remote.priority, current_role);
         if (current_state IS State::NEW and priority > pair.priority) {
            pair.local = Local;
            pair.priority = priority;
         }
         return;
      }
   }

   Pair next{ Local, Remote, pair_priority(Local.priority, Remote.priority, current_role) };
   if (pairs.size() < pair_limit) pairs.push_back(std::move(next));
   else {
      size_t lowest = size_t(-1);
      for (size_t i = 0; i < pairs.size(); ++i) {
         if (i IS selected_index or pairs[i].state IS PairState::SUCCEEDED or
             std::any_of(transactions.begin(), transactions.end(), [i](const Transaction &Value) {
                return Value.pair_index IS i;
             })) continue;
         if (lowest IS size_t(-1) or pairs[i].priority < pairs[lowest].priority) lowest = i;
      }

      if (lowest != size_t(-1) and next.priority > pairs[lowest].priority) {
         std::erase(triggered_checks, lowest);
         pairs[lowest] = std::move(next);
      }
   }
}

//********************************************************************************************************************
// Pair priorities depend on the role, so they are recomputed whenever the role changes.

void FullAgent::recompute_priorities()
{
   for (auto &pair : pairs) {
      pair.priority = pair_priority(pair.local.priority, pair.remote.priority, current_role);
      if (valid_endpoint(pair.valid_local.endpoint)) {
         pair.valid_priority = pair_priority(pair.valid_local.priority, pair.remote.priority, current_role);
      }
   }
}

//********************************************************************************************************************
// Returns true if two pairs share a foundation.  The remote foundation is used when it was signalled; otherwise it is
// derived from the candidate type and address.

bool FullAgent::same_foundation(const Pair &Left, const Pair &Right)
{
   auto left_base = socket_endpoint(Left.local);
   auto right_base = socket_endpoint(Right.local);

   // One UDP component: host bases on different ports of the same IP share a foundation.

   if (Left.local.type != Right.local.type or left_base.route != right_base.route or
       left_base.ipv6 != right_base.ipv6 or left_base.address != right_base.address) return false;

   if (not Left.remote.foundation.empty() or not Right.remote.foundation.empty()) {
      return Left.remote.foundation IS Right.remote.foundation;
   }

   return Left.remote.type IS Right.remote.type and Left.remote.endpoint.ipv6 IS Right.remote.endpoint.ipv6 and
      Left.remote.endpoint.address IS Right.remote.endpoint.address;
}

//********************************************************************************************************************
// Moves frozen pairs to the waiting state (RFC 8445 §6.1.2.6).

void FullAgent::thaw_pairs()
{
   // For the single bundled component, start the highest-priority pair of each foundation.
   // Success unfreezes its foundation; exhaustion allows the next frozen route to be tried.

   for (size_t i = 0; i < pairs.size(); ++i) {
      if (pairs[i].state != PairState::FROZEN) continue;

      bool active = false, succeeded = false;
      size_t best = i;
      for (size_t j = 0; j < pairs.size(); ++j) {
         if (not same_foundation(pairs[i], pairs[j])) continue;
         active |= pairs[j].state IS PairState::WAITING or pairs[j].state IS PairState::IN_PROGRESS;
         succeeded |= pairs[j].state IS PairState::SUCCEEDED;
         if (pairs[j].state IS PairState::FROZEN and pairs[j].priority > pairs[best].priority) best = j;
      }

      if (succeeded) pairs[i].state = PairState::WAITING;
      else if (not active) pairs[best].state = PairState::WAITING;
   }
}

//********************************************************************************************************************
// Records an authenticated route, from which DTLS will be accepted.  Up to 128 routes are retained, and the selected
// route is never evicted.

void FullAgent::remember_source(const Endpoint &Source, const Endpoint &Local)
{
   if (validated(Source, Local)) return;

   if (valid_sources.size() >= 128) {
      auto selected = selected_index IS size_t(-1) ? nullptr : &pairs[selected_index].remote.endpoint;
      auto evict = std::find_if(valid_sources.begin(), valid_sources.end(), [selected](const auto &Value) {
         return not selected or Value.second != *selected;
      });
      if (evict IS valid_sources.end()) return;
      valid_sources.erase(evict);
   }

   valid_sources.push_back({Local, Source});
}

//********************************************************************************************************************
// Adds a local candidate and pairs it with every remote candidate.  Reflexive candidates require a base of the same
// address family.

bool FullAgent::add_local_candidate(const Candidate &Value)
{
   if (not valid_endpoint(Value.endpoint) or Value.priority IS 0 or local_candidates.size() >= 128) return false;

   if (Value.type != CandidateType::HOST and Value.type != CandidateType::RELAY and
       (not valid_endpoint(Value.base) or Value.base.ipv6 != Value.endpoint.ipv6)) return false;

   for (const auto &existing : local_candidates) if (existing.endpoint IS Value.endpoint) return false;

   local_candidates.push_back(Value);
   for (const auto &remote : remote_candidates) form_pair(Value, remote);
   return true;
}

//********************************************************************************************************************
// Removes every candidate that sends from Local, and fails its pairs.  If the selected pair is lost, the agent is
// DISCONNECTED when Recover is true, or FAILED otherwise.

void FullAgent::retire_local_route(const Endpoint &Local, bool Recover)
{
   std::erase_if(local_candidates, [&](const Candidate &Value) { return socket_endpoint(Value) IS Local; });

   for (size_t i = 0; i < pairs.size(); ++i) if (socket_endpoint(pairs[i].local) IS Local) {
      pairs[i].state = PairState::FAILED;
      std::erase_if(transactions, [i](const Transaction &Value) { return Value.pair_index IS i; });
      std::erase(triggered_checks, i);
      if (selected_index IS i) {
         selected_index = size_t(-1);
         current_state = Recover ? State::DISCONNECTED : State::FAILED;
         consent_transactions.clear();
      }
   }

   std::erase_if(valid_sources, [&](const auto &Value) { return Value.first IS Local; });
}

//********************************************************************************************************************

bool FullAgent::add_remote_candidate(const Candidate &Value)
{
   if (remote_complete) return true; // This gate applies only to candidates delivered by signalling.
   return learn_remote_candidate(Value);
}

//********************************************************************************************************************
// Adds a remote candidate, whether signalled or learnt from an authenticated check, and pairs it with every local
// candidate.

bool FullAgent::learn_remote_candidate(const Candidate &Value)
{
   if (not valid_endpoint(Value.endpoint) or Value.priority IS 0) return false;

   for (auto &existing : remote_candidates) if (existing.endpoint IS Value.endpoint) {
      // Authenticated checks may learn an endpoint before its signalled mDNS identity resolves.
      // Merge the host priority/foundation instead of retaining the temporary peer-reflexive preference.

      if (existing.type IS CandidateType::PEER_REFLEXIVE and Value.type != CandidateType::PEER_REFLEXIVE) {
         existing = Value;
         for (auto &pair : pairs) if (pair.remote.endpoint IS Value.endpoint) pair.remote = Value;
         recompute_priorities();

         if (current_role IS Role::CONTROLLED) for (size_t i = 0; i < pairs.size(); ++i) {
            if (pairs[i].remote.endpoint IS Value.endpoint and pairs[i].peer_nominated and
                pairs[i].state != PairState::SUCCEEDED and (selected_index IS size_t(-1) or
                pairs[i].priority > pairs[selected_index].valid_priority)) queue_triggered_check(i);
         }
      }
      return true;
   }

   if (remote_candidates.size() >= 128) return false;

   remote_candidates.push_back(Value);
   for (const auto &local : local_candidates) form_pair(local, Value);
   return true;
}

//********************************************************************************************************************

void FullAgent::end_remote_candidates()
{
   remote_complete = true;
}

//********************************************************************************************************************
// Starts connectivity checks.  Credentials and at least one local candidate are required.

bool FullAgent::start(uint64_t NowUs, std::vector<Outbound> &Output)
{
   if (current_state != State::NEW or local_ufrag.empty() or local_password.empty() or
       remote_ufrag.empty() or remote_password.empty() or local_candidates.empty() or tie_breaker IS 0) return false;

   current_state = State::CHECKING;
   next_check_us = NowUs;
   tick(NowUs, Output);
   return true;
}

//********************************************************************************************************************
// Sends a connectivity check on a pair.  Nomination adds USE-CANDIDATE, while Consent sends a consent freshness
// check on the selected pair, which is tracked separately from ordinary checks.  Ordinary checks are paced at one
// every 50 ms.

void FullAgent::send_check(size_t PairIndex, bool Nomination, bool Consent, uint64_t NowUs,
   std::vector<Outbound> &Output)
{
   auto &pair = pairs[PairIndex];
   std::string username = remote_ufrag + ":" + local_ufrag;
   std::array<uint8_t, 4> priority{};
   write32(priority.data(), peer_reflexive_priority(pair.local));
   std::array<uint8_t, 8> role_value{};
   write64(role_value.data(), tie_breaker);

   std::array<stun::Attribute, 4> attributes{
      stun::Attribute{ stun::ATTR_USERNAME,
         std::span<const uint8_t>((const uint8_t *)username.data(), username.size()), 0 },
      stun::Attribute{ ATTR_PRIORITY, priority, 0 },
      stun::Attribute{ uint16_t(current_role IS Role::CONTROLLING ? ATTR_ICE_CONTROLLING : ATTR_ICE_CONTROLLED),
         role_value, 0 },
      stun::Attribute{ ATTR_USE_CANDIDATE, {}, 0 }
   };

   Transaction next;
   if (RAND_bytes(next.id.data(), next.id.size()) != 1 or
       not stun::encode(stun::BINDING_REQUEST, next.id,
          std::span<const stun::Attribute>(attributes.data(), Nomination ? 4u : 3u),
          password_bytes(remote_password), false, true, next.datagram)) {
      current_state = State::FAILED;
      return;
   }

   next.sent_us = NowUs;
   next.pair_index = PairIndex;
   next.due_us = NowUs + next.rto_us;
   next.expires_us = NowUs + uint64_t(next.rto_us) * 79u;
   next.attempts = 1;
   next.nomination = Nomination;

   Output.push_back({ socket_endpoint(pair.local), pair.remote.endpoint, next.datagram });
   if (Consent) consent_transactions.push_back({next.id, NowUs});
   else transactions.push_back(std::move(next));

   if (not Consent and not Nomination) {
      pair.state = PairState::IN_PROGRESS;
      last_check_ipv6 = pair.remote.endpoint.ipv6;
      checked_family = true;
   }

   next_check_us = NowUs + 50000;
}

//********************************************************************************************************************
// Switches role after a role conflict (RFC 8445 §7.3.1.1).  Checks in progress are abandoned and requeued, and a new
// tie-breaker is generated.

bool FullAgent::switch_role()
{
   for (const auto &transaction : transactions) pairs[transaction.pair_index].state = PairState::WAITING;
   transactions.clear();
   triggered_checks.clear();
   current_role = current_role IS Role::CONTROLLING ? Role::CONTROLLED : Role::CONTROLLING;

   if (RAND_bytes((uint8_t *)&tie_breaker, sizeof(tie_breaker)) != 1 or tie_breaker IS 0) {
      current_state = State::FAILED;
      return false;
   }

   recompute_priorities();
   return true;
}

//********************************************************************************************************************
// Schedules the next consent check at a random interval of four to six seconds (RFC 7675 §5.1).

bool FullAgent::schedule_consent(uint64_t NowUs)
{
   uint32_t random = 0;
   if (RAND_bytes((uint8_t *)&random, sizeof(random)) != 1) {
      current_state = State::FAILED;
      return false;
   }

   next_consent_us = NowUs + 4000000u + random % 2000001u;
   return true;
}

//********************************************************************************************************************
// Processes a STUN datagram received on Local from Source.  input_accepted is set if the datagram authenticated
// successfully.

void FullAgent::receive(std::span<const uint8_t> Datagram, const Endpoint &Local, const Endpoint &Source,
   uint64_t NowUs, std::vector<Outbound> &Output)
{
   input_accepted = false;
   if (current_state IS State::NEW or current_state IS State::FAILED or not valid_endpoint(Source)) return;

   stun::Message parsed;
   if (not stun::parse(Datagram, parsed)) return;

   if (parsed.type IS stun::BINDING_REQUEST) process_request(Datagram, parsed, Local, Source, NowUs, Output);
   else if (parsed.type IS stun::BINDING_SUCCESS or parsed.type IS BINDING_ERROR) {
      process_response(Datagram, parsed, Local, Source, NowUs, Output);
   }
}

//********************************************************************************************************************
// Processes a connectivity check from the peer (RFC 8445 §7.3).  The request is authenticated with the local
// password and role conflicts are resolved by tie-breaker.  A successful check validates its route, may learn a
// peer-reflexive candidate, and triggers a check in the reverse direction.  When controlled, USE-CANDIDATE nominates
// the pair.

void FullAgent::process_request(std::span<const uint8_t> Datagram, const stun::Message &Parsed,
   const Endpoint &Local, const Endpoint &Source, uint64_t NowUs, std::vector<Outbound> &Output)
{
   auto local = std::find_if(local_candidates.begin(), local_candidates.end(), [&](const Candidate &Value) {
      return socket_endpoint(Value) IS Local;
   });
   if (local IS local_candidates.end()) return;

   if (not stun::valid_attribute(Parsed, stun::ATTR_USERNAME, local_ufrag.size() + 1 + remote_ufrag.size(), true) or
       not stun::valid_attribute(Parsed, stun::ATTR_MESSAGE_INTEGRITY, 20, true) or
       not stun::valid_attribute(Parsed, stun::ATTR_FINGERPRINT, 4, true) or
       not stun::valid_attribute(Parsed, ATTR_PRIORITY, 4, true) or
       not stun::valid_attribute(Parsed, ATTR_USE_CANDIDATE, 0, false) or
       not stun::valid_attribute(Parsed, ATTR_ICE_CONTROLLING, 8, false) or
       not stun::valid_attribute(Parsed, ATTR_ICE_CONTROLLED, 8, false)) return;

   auto username = Parsed.find(stun::ATTR_USERNAME);
   auto integrity = Parsed.find(stun::ATTR_MESSAGE_INTEGRITY);
   auto fingerprint = Parsed.find(stun::ATTR_FINGERPRINT);
   if (username->offset >= integrity->offset or integrity->offset >= fingerprint->offset) return;

   for (const auto &attribute : Parsed.attributes) {
      if (attribute.offset > integrity->offset and attribute.type != stun::ATTR_FINGERPRINT) return;
   }

   auto expected = local_ufrag + ":" + remote_ufrag;
   if (std::string_view((const char *)username->value.data(), username->value.size()) != expected or
       not stun::verify_integrity(Datagram, Parsed, password_bytes(local_password)) or
       not stun::verify_fingerprint(Datagram, Parsed)) return;

   auto unknown = stun::unknown_required_attributes(Parsed);
   if (not unknown.empty()) {
      std::vector<uint8_t> response;
      if (stun::encode_unknown_attributes_error(Parsed, unknown, password_bytes(local_password), response)) {
         Output.push_back({ Local, Source, std::move(response) });
      }
      return;
   }

   auto controlling = Parsed.find(ATTR_ICE_CONTROLLING);
   auto controlled = Parsed.find(ATTR_ICE_CONTROLLED);
   if (bool(controlling) IS bool(controlled)) return;

   // Role conflict resolution (RFC 8445 §7.3.1.1)

   bool conflict = false;
   if (current_role IS Role::CONTROLLING and controlling) {
      if (tie_breaker >= read64(controlling->value.data())) conflict = true;
      else if (not switch_role()) return;
   }
   else if (current_role IS Role::CONTROLLED and controlled) {
      if (tie_breaker >= read64(controlled->value.data())) {
         if (not switch_role()) return;
      }
      else conflict = true;
   }

   if (conflict) {
      std::array<uint8_t, 4> error_code{ 0, 0, 4, 87 };
      stun::Attribute error{ stun::ATTR_ERROR_CODE, error_code, 0 };
      std::vector<uint8_t> response;
      if (stun::encode(BINDING_ERROR, Parsed.transaction_id, std::span<const stun::Attribute>(&error, 1),
          password_bytes(local_password), false, true, response)) {
         Output.push_back({ Local, Source, std::move(response) });
      }
      return;
   }

   if (Parsed.find(ATTR_USE_CANDIDATE) and current_role != Role::CONTROLLED) return;

   auto mapped = stun::encode_xor_address(Source.address, Source.port, Source.ipv6, Parsed.transaction_id);
   stun::Attribute mapped_attribute{ stun::ATTR_XOR_MAPPED_ADDRESS,
      std::span<const uint8_t>(mapped.data(), Source.ipv6 ? 20u : 8u), 0 };
   std::vector<uint8_t> response;
   if (not stun::encode(stun::BINDING_SUCCESS, Parsed.transaction_id,
       std::span<const stun::Attribute>(&mapped_attribute, 1),
       password_bytes(local_password), false, true, response)) return;

   input_accepted = true;
   Output.push_back({ Local, Source, std::move(response) });
   remember_source(Source, Local);

   // A source that was not signalled is learnt as a peer-reflexive candidate (RFC 8445 §7.3.1.3).

   auto remote = std::find_if(remote_candidates.begin(), remote_candidates.end(), [&](const Candidate &Value) {
      return Value.endpoint IS Source;
   });

   if (remote IS remote_candidates.end()) {
      Candidate reflexive{ Source, read32(Parsed.find(ATTR_PRIORITY)->value.data()), CandidateType::PEER_REFLEXIVE };
      if (not learn_remote_candidate(reflexive)) return;
   }

   auto pair = std::find_if(pairs.begin(), pairs.end(), [&](const Pair &Value) {
      return socket_endpoint(Value.local) IS Local and Value.remote.endpoint IS Source;
   });
   if (pair IS pairs.end()) return;

   bool nominated = Parsed.find(ATTR_USE_CANDIDATE) != nullptr;
   if (nominated) pair->peer_nominated = true;

   auto pair_index = size_t(pair - pairs.begin());
   bool new_nomination = current_role IS Role::CONTROLLED and nominated and
      pair_index != selected_index;

   // The controller may nominate a lower-priority pair after a route change.
   // Retain the working route until the nominated replacement passes its own authenticated outbound check.

   if (new_nomination and pair->state IS PairState::SUCCEEDED) {
      selected_index = size_t(pair - pairs.begin());
      last_consent_us = NowUs;
      current_state = State::COMPLETED;
      consent_transactions.clear();
      if (not schedule_consent(NowUs)) return;
      transactions.clear();
      triggered_checks.clear();
   }

   if ((selected_index IS size_t(-1) or new_nomination) and pair->state != PairState::SUCCEEDED) {
      queue_triggered_check(size_t(pair - pairs.begin()));
   }

   tick(NowUs, Output);
}

//********************************************************************************************************************
// Processes a response to one of our checks (RFC 8445 §7.2.5).  Once a pair is selected, only consent responses
// are processed, and a 403 response revokes consent.  For ordinary checks, a 487 response switches role and retries
// the pair, and a success response validates the pair with the mapped address that the peer observed.

void FullAgent::process_response(std::span<const uint8_t> Datagram, const stun::Message &Parsed,
   const Endpoint &Local, const Endpoint &Source, uint64_t NowUs, std::vector<Outbound> &Output)
{
   if (selected_index != size_t(-1)) {
      if (current_state IS State::DISCONNECTED or
          (NowUs >= last_consent_us and NowUs - last_consent_us >= 30000000)) return;

      auto found = std::find_if(consent_transactions.begin(), consent_transactions.end(),
         [&](const ConsentTransaction &Value) { return Value.id IS Parsed.transaction_id; });

      if (found != consent_transactions.end()) {
         if (NowUs >= found->sent_us and NowUs - found->sent_us >= 30000000) return;

         auto &selected = pairs[selected_index];
         auto integrity = Parsed.find(stun::ATTR_MESSAGE_INTEGRITY);
         auto fingerprint = Parsed.find(stun::ATTR_FINGERPRINT);
         if (socket_endpoint(selected.local) != Local or selected.remote.endpoint != Source or
             not stun::unknown_required_attributes(Parsed).empty() or
             not stun::valid_attribute(Parsed, stun::ATTR_MESSAGE_INTEGRITY, 20, true) or
             not stun::valid_attribute(Parsed, stun::ATTR_FINGERPRINT, 4, true) or
             not integrity or not fingerprint or integrity->offset >= fingerprint->offset or
             not stun::verify_integrity(Datagram, Parsed, password_bytes(remote_password)) or
             not stun::verify_fingerprint(Datagram, Parsed)) return;

         for (const auto &attribute : Parsed.attributes) {
            if (attribute.offset > integrity->offset and attribute.type != stun::ATTR_FINGERPRINT) return;
         }

         auto sent_us = found->sent_us;
         consent_transactions.erase(found);

         if (Parsed.type IS BINDING_ERROR) {
            auto error = Parsed.find(stun::ATTR_ERROR_CODE);
            uint16_t code = 0;
            if (error and error->offset < integrity->offset and
                stun::decode_error_code(*error, code) and code IS 403) {
               current_state = State::DISCONNECTED;
               consent_transactions.clear();
            }
            return;
         }

         if (Parsed.type IS stun::BINDING_SUCCESS) {
            auto mapped = Parsed.find(stun::ATTR_XOR_MAPPED_ADDRESS);
            std::array<uint8_t, 16> address{};
            uint16_t port = 0;
            bool ipv6 = false;
            if (not mapped or mapped->offset >= integrity->offset or
                not stun::decode_xor_address(*mapped, Parsed, address, port, ipv6)) return;

            input_accepted = true;
            selected.rtt_us = NowUs >= sent_us ? NowUs - sent_us : 0;
            selected.rtt_valid = NowUs >= sent_us;
            last_consent_us = NowUs;
            current_state = State::COMPLETED;
         }
         return;
      }
   }

   // Ordinary and nomination checks

   auto found = std::find_if(transactions.begin(), transactions.end(),
      [&](const Transaction &Value) { return Value.id IS Parsed.transaction_id; });
   if (found IS transactions.end() or NowUs >= found->expires_us) return;
   if (not stun::unknown_required_attributes(Parsed).empty()) return;

   auto &pair = pairs[found->pair_index];
   if (not stun::valid_attribute(Parsed, stun::ATTR_MESSAGE_INTEGRITY, 20, true) or
       not stun::valid_attribute(Parsed, stun::ATTR_FINGERPRINT, 4, true)) return;

   auto integrity = Parsed.find(stun::ATTR_MESSAGE_INTEGRITY);
   auto fingerprint = Parsed.find(stun::ATTR_FINGERPRINT);
   if (integrity->offset >= fingerprint->offset) return;

   for (const auto &attribute : Parsed.attributes) {
      if (attribute.offset > integrity->offset and attribute.type != stun::ATTR_FINGERPRINT) return;
   }

   if (socket_endpoint(pair.local) != Local or pair.remote.endpoint != Source or
       not stun::verify_integrity(Datagram, Parsed, password_bytes(remote_password)) or
       not stun::verify_fingerprint(Datagram, Parsed)) return;

   auto sent_us = found->sent_us;
   auto attempts = found->attempts;
   auto nomination = found->nomination;
   auto pair_index = found->pair_index;
   if (found->cancelled and Parsed.type IS BINDING_ERROR) return;
   transactions.erase(found);

   if (Parsed.type IS BINDING_ERROR) {
      auto code = Parsed.find(stun::ATTR_ERROR_CODE);
      uint16_t error_code = 0;
      if (code and code->offset < integrity->offset and
          stun::decode_error_code(*code, error_code) and error_code IS 487) {
         if (not switch_role()) return;
         queue_triggered_check(pair_index);
      }
      else {
         pair.state = PairState::FAILED;
         if (nomination) {
            current_state = State::FAILED;
            return;
         }
      }

      update_state(NowUs);
      tick(NowUs, Output);
      return;
   }

   auto mapped = Parsed.find(stun::ATTR_XOR_MAPPED_ADDRESS);
   std::array<uint8_t, 16> address{};
   uint16_t port = 0;
   bool ipv6 = false;
   if (not mapped or mapped->offset >= integrity->offset or
       not stun::decode_xor_address(*mapped, Parsed, address, port, ipv6) or port IS 0 or
       ipv6 != pair.local.endpoint.ipv6) {
      pair.state = PairState::FAILED;
      if (nomination) {
         current_state = State::FAILED;
         return;
      }
      update_state(NowUs);
      return;
   }

   // The valid pair is formed from the mapped address (RFC 8445 §7.2.5.3.2).

   Endpoint mapped_endpoint{ address, port, ipv6, Local.route };
   auto candidate = std::find_if(local_candidates.begin(), local_candidates.end(), [&](const Candidate &Value) {
      return Value.endpoint IS mapped_endpoint and socket_endpoint(Value) IS Local;
   });

   if (candidate != local_candidates.end()) pair.valid_local = *candidate;
   else {
      // Keep the validated mapped candidate separate from the checklist's sending socket.
      // Do not form new ordinary checks from an internally discovered peer-reflexive candidate.
      pair.valid_local = { mapped_endpoint, peer_reflexive_priority(pair.local), CandidateType::PEER_REFLEXIVE, Local };
      if (local_candidates.size() < 128) local_candidates.push_back(pair.valid_local);
   }

   input_accepted = true;
   if (attempts IS 1 and NowUs >= sent_us) {
      pair.rtt_us = NowUs - sent_us;
      pair.rtt_valid = true;
   }

   pair.valid_priority = pair_priority(pair.valid_local.priority, pair.remote.priority, current_role);
   pair.state = PairState::SUCCEEDED;
   std::erase_if(transactions, [pair_index](const Transaction &Value) { return Value.pair_index IS pair_index; });
   std::erase(triggered_checks, pair_index);
   remember_source(Source, Local);

   if (nomination or (current_role IS Role::CONTROLLED and pair.peer_nominated and
       selected_index != pair_index)) {
      selected_index = pair_index;
      consent_transactions.clear();
      transactions.clear();
      triggered_checks.clear();
      last_consent_us = NowUs;
      current_state = State::COMPLETED;
      if (not schedule_consent(NowUs)) return;
   }
   else {
      current_state = selected_index != size_t(-1) ? State::COMPLETED : State::CONNECTED;
      tick(NowUs, Output);
   }
}

//********************************************************************************************************************
// Derives the agent state from the selected pair, or from the checklist if no pair is selected.  The agent fails
// once the remote candidates are complete and no pair remains to be checked.

void FullAgent::update_state(uint64_t NowUs)
{
   if (selected_index != size_t(-1)) {
      if (NowUs >= last_consent_us and NowUs - last_consent_us >= 30000000) current_state = State::DISCONNECTED;
      else current_state = State::COMPLETED;
      return;
   }

   bool waiting = false, succeeded = false;
   for (const auto &pair : pairs) {
      waiting |= pair.state IS PairState::FROZEN or pair.state IS PairState::WAITING or
         pair.state IS PairState::IN_PROGRESS;
      succeeded |= pair.state IS PairState::SUCCEEDED;
   }

   if (succeeded) current_state = State::CONNECTED;
   else if (remote_complete and not waiting) current_state = State::FAILED;
   else current_state = State::CHECKING;
}

//********************************************************************************************************************
// Queues a triggered check for a pair (RFC 8445 §7.3.1.4).  Any transaction in progress on the pair is cancelled.

void FullAgent::queue_triggered_check(size_t PairIndex)
{
   if (std::find(triggered_checks.begin(), triggered_checks.end(), PairIndex) != triggered_checks.end()) return;

   // Stop retransmission but retain matching responses until the original transaction would expire.
   // New checks are paced, so retaining cancelled transactions is bounded by that timeout window.

   for (auto &transaction : transactions) {
      if (transaction.pair_index != PairIndex or transaction.cancelled) continue;
      transaction.cancelled = true;
      transaction.due_us = transaction.expires_us;
   }

   pairs[PairIndex].state = PairState::WAITING;
   triggered_checks.push_back(PairIndex);
}

//********************************************************************************************************************
// Processes timers: consent expiry, check retransmission, selection of a peer-nominated pair, consent freshness, and
// the next paced check.  When controlling, the best valid pair is nominated once validated.  Check retransmissions
// use exponential backoff for up to seven attempts.

void FullAgent::tick(uint64_t NowUs, std::vector<Outbound> &Output)
{
   if (current_state IS State::NEW or current_state IS State::FAILED) return;

   // Network callbacks can use a newer timestamp than an already captured timer snapshot.

   if (selected_index != size_t(-1) and NowUs >= last_consent_us and NowUs - last_consent_us >= 30000000) {
      current_state = State::DISCONNECTED;
      consent_transactions.clear();
      transactions.clear();
      triggered_checks.clear();
   }

   if (current_state IS State::DISCONNECTED) return;

   // Retransmissions

   for (auto transaction = transactions.begin(); transaction != transactions.end();) {
      if (NowUs < transaction->due_us) {
         ++transaction;
         continue;
      }

      if (transaction->cancelled or transaction->attempts >= 7 or NowUs >= transaction->expires_us) {
         if (not transaction->cancelled) pairs[transaction->pair_index].state = PairState::FAILED;
         if (not transaction->cancelled and transaction->nomination) {
            current_state = State::FAILED;
            transactions.clear();
            triggered_checks.clear();
            return;
         }
         transaction = transactions.erase(transaction);
      }
      else {
         const auto &pair = pairs[transaction->pair_index];
         Output.push_back({ socket_endpoint(pair.local), pair.remote.endpoint, transaction->datagram });
         ++transaction->attempts;
         transaction->rto_us = transaction->attempts IS 7 ? 8000000u : transaction->rto_us * 2u;
         transaction->due_us = NowUs + transaction->rto_us;
         ++transaction;
      }
   }

   // A controlled agent selects the best valid pair that the peer has nominated.

   if (current_role IS Role::CONTROLLED and selected_index IS size_t(-1)) {
      auto best = selected_index;
      for (size_t i = 0; i < pairs.size(); ++i) {
         if (pairs[i].state IS PairState::SUCCEEDED and pairs[i].peer_nominated and
             (best IS size_t(-1) or pairs[i].valid_priority > pairs[best].valid_priority)) best = i;
      }

      if (best != selected_index) {
         selected_index = best;
         last_consent_us = NowUs;
         current_state = State::COMPLETED;
         transactions.clear();
         triggered_checks.clear();
         consent_transactions.clear();
         if (not schedule_consent(NowUs)) return;
      }
   }

   if (selected_index != size_t(-1)) {
      if (NowUs >= next_consent_us) {
         send_check(selected_index, false, true, NowUs, Output);
         if (current_state IS State::FAILED or not schedule_consent(NowUs)) return;
         std::erase_if(consent_transactions,
            [NowUs](const ConsentTransaction &Value) {
               return NowUs >= Value.sent_us and NowUs - Value.sent_us >= 30000000;
            });
      }

      if (current_role IS Role::CONTROLLED and NowUs >= next_check_us and not triggered_checks.empty()) {
         auto pair_index = triggered_checks.front();
         triggered_checks.pop_front();
         send_check(pair_index, false, false, NowUs, Output);
      }
      return;
   }

   thaw_pairs();
   if (NowUs < next_check_us) return;

   if (not triggered_checks.empty()) {
      auto pair_index = triggered_checks.front();
      triggered_checks.pop_front();
      send_check(pair_index, false, false, NowUs, Output);
      return;
   }

   if (current_role IS Role::CONTROLLING and
       std::none_of(transactions.begin(), transactions.end(), [](const Transaction &Value) {
          return Value.nomination and not Value.cancelled;
       })) {
      size_t best_valid = size_t(-1);
      for (size_t i = 0; i < pairs.size(); ++i) {
         if (pairs[i].state IS PairState::SUCCEEDED and
             (best_valid IS size_t(-1) or pairs[i].valid_priority > pairs[best_valid].valid_priority)) best_valid = i;
      }

      // Regular nomination: first validate the route, then nominate the best currently valid pair.
      // Unanswered checks on higher-priority routes must not block an already usable connection.

      if (best_valid != size_t(-1)) {
         send_check(best_valid, true, false, NowUs, Output);
         return;
      }
   }

   size_t best_waiting = size_t(-1);
   for (size_t i = 0; i < pairs.size(); ++i) {
      if (pairs[i].state IS PairState::WAITING and
          (best_waiting IS size_t(-1) or pairs[i].priority > pairs[best_waiting].priority)) best_waiting = i;
   }

   // Interleave ordinary checks across families.  A high-priority black hole cannot consume
   // every pacing slot while the other family is waiting; triggered checks retain ICE precedence.

   if (checked_family) {
      size_t alternate = size_t(-1);
      for (size_t i = 0; i < pairs.size(); ++i) {
         if (pairs[i].state IS PairState::WAITING and pairs[i].remote.endpoint.ipv6 != last_check_ipv6 and
             (alternate IS size_t(-1) or pairs[i].priority > pairs[alternate].priority)) alternate = i;
      }
      if (alternate != size_t(-1)) best_waiting = alternate;
   }

   if (best_waiting != size_t(-1)) {
      send_check(best_waiting, false, false, NowUs, Output);
      return;
   }

   update_state(NowUs);
}

//********************************************************************************************************************
// Restarts ICE with new local credentials (RFC 8445 §9).  Host, server-reflexive and relayed candidates are kept, but
// the checklist and all remote state are discarded.

void FullAgent::restart(std::string_view LocalUfrag, std::string_view LocalPassword, Role NewRole)
{
   local_ufrag = LocalUfrag;
   local_password = LocalPassword;
   remote_ufrag.clear();
   remote_password.clear();
   remote_candidates.clear();
   std::erase_if(local_candidates, [](const Candidate &Value) {
      return Value.type IS CandidateType::PEER_REFLEXIVE;
   });
   pairs.clear();
   valid_sources.clear();
   transactions.clear();
   triggered_checks.clear();
   consent_transactions.clear();
   current_role = NewRole;
   current_state = State::NEW;
   selected_index = size_t(-1);
   last_consent_us = 0;
   next_consent_us = 0;
   next_check_us = 0;
   checked_family = false;
   remote_lite = false;
   remote_complete = false;
   (void)RAND_bytes((uint8_t *)&tie_breaker, sizeof(tie_breaker));
}

//********************************************************************************************************************
// Accessors for the selected pair, which are only defined in the COMPLETED state.

const Endpoint * FullAgent::selected_remote() const
{
   if (selected_index IS size_t(-1) or current_state != State::COMPLETED) return nullptr;
   return &pairs[selected_index].remote.endpoint;
}

const Endpoint * FullAgent::selected_local() const
{
   if (selected_index IS size_t(-1) or current_state != State::COMPLETED) return nullptr;
   return &socket_endpoint(pairs[selected_index].local);
}

const Candidate * FullAgent::selected_local_candidate() const
{
   if (selected_index IS size_t(-1) or current_state != State::COMPLETED) return nullptr;
   return &pairs[selected_index].valid_local;
}

//********************************************************************************************************************
// Returns true if a remote endpoint has authenticated on any route, or on the given local route.

bool FullAgent::validated(const Endpoint &Source) const
{
   return std::any_of(valid_sources.begin(), valid_sources.end(),
      [&](const auto &Value) { return Value.second IS Source; });
}

bool FullAgent::validated(const Endpoint &Source, const Endpoint &Local) const
{
   return std::find(valid_sources.begin(), valid_sources.end(), std::pair{Local, Source}) != valid_sources.end();
}

//********************************************************************************************************************
// Returns the time until tick() next needs to be called, or -1 if no timer is armed.

int64_t FullAgent::timeout_us(uint64_t NowUs) const
{
   if (current_state IS State::NEW or current_state IS State::FAILED or
       current_state IS State::DISCONNECTED) return -1;

   uint64_t due = std::numeric_limits<uint64_t>::max();
   for (const auto &transaction : transactions) due = std::min(due, transaction.due_us);

   if (selected_index != size_t(-1)) {
      due = std::min(due, std::min(next_consent_us, last_consent_us + 30000000));
      if (current_role IS Role::CONTROLLED and not triggered_checks.empty()) due = std::min(due, next_check_us);
   }
   else {
      bool waiting = not triggered_checks.empty(), frozen = false, succeeded = false, nominating = false;
      for (const auto &pair : pairs) {
         waiting |= pair.state IS PairState::WAITING;
         frozen |= pair.state IS PairState::FROZEN;
         succeeded |= pair.state IS PairState::SUCCEEDED;
      }

      for (const auto &transaction : transactions) nominating |= transaction.nomination and not transaction.cancelled;

      if (waiting or (frozen and transactions.empty()) or
          (succeeded and current_role IS Role::CONTROLLING and not nominating)) {
         due = std::min(due, next_check_us);
      }
   }

   if (due IS std::numeric_limits<uint64_t>::max()) return -1;
   return due <= NowUs ? 0 : int64_t(due - NowUs);
}

//********************************************************************************************************************
// Returns true if the checklist contains a pair that sends from Local to Remote.

bool FullAgent::has_pair(const Endpoint &Local, const Endpoint &Remote) const
{
   return std::any_of(pairs.begin(), pairs.end(), [&](const Pair &Value) {
      return socket_endpoint(Value.local) IS Local and Value.remote.endpoint IS Remote;
   });
}

} // namespace rtc::ice
