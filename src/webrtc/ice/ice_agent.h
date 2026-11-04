// Full ICE agent core for one bundled UDP component (RFC 8445).
// Sockets and timers belong to the caller; all timestamps are monotonic microseconds.

#pragma once

#include "ice.h"
#include "../stun/stun.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace rtc::ice {

enum class Role { CONTROLLING, CONTROLLED };
enum class State { NEW, CHECKING, CONNECTED, COMPLETED, DISCONNECTED, FAILED };
enum class CandidateType { HOST, SERVER_REFLEXIVE, PEER_REFLEXIVE, RELAY };

struct Candidate {
   Endpoint endpoint;
   uint32_t priority = 0;
   CandidateType type = CandidateType::HOST;
   Endpoint base; // Local socket address for a reflexive candidate; empty for a host candidate.
   std::string foundation; // Signalled remote foundation; empty uses the candidate's type and IP address.
};

struct Outbound {
   Endpoint local;
   Endpoint remote;
   std::vector<uint8_t> datagram;
};

//********************************************************************************************************************

class FullAgent {
public:
   FullAgent(std::string_view LocalUfrag, std::string_view LocalPassword, Role InitialRole, size_t PairLimit = 256);

   bool set_remote_credentials(std::string_view RemoteUfrag, std::string_view RemotePassword, bool RemoteLite);
   bool add_local_candidate(const Candidate &Value);
   bool add_remote_candidate(const Candidate &Value);
   void retire_local_route(const Endpoint &Local, bool Recover = false);
   void end_remote_candidates();

   bool start(uint64_t NowUs, std::vector<Outbound> &Output);
   void receive(std::span<const uint8_t> Datagram, const Endpoint &Local, const Endpoint &Source,
      uint64_t NowUs, std::vector<Outbound> &Output);
   void tick(uint64_t NowUs, std::vector<Outbound> &Output);
   void restart(std::string_view LocalUfrag, std::string_view LocalPassword, Role NewRole);

   [[nodiscard]] const Endpoint * selected_remote() const;
   [[nodiscard]] const Endpoint * selected_local() const;
   [[nodiscard]] const Candidate * selected_local_candidate() const;
   [[nodiscard]] const Candidate * selected_remote_candidate() const {
      return selected_remote() ? &pairs[selected_index].remote : nullptr;
   }
   [[nodiscard]] uint64_t selected_rtt_us() const {
      return selected_remote() ? pairs[selected_index].rtt_us : 0;
   }
   [[nodiscard]] bool selected_rtt_valid() const {
      return selected_remote() and pairs[selected_index].rtt_valid;
   }
   [[nodiscard]] bool accepted_input() const { return input_accepted; }
   [[nodiscard]] bool has_pair(const Endpoint &Local, const Endpoint &Remote) const;
   [[nodiscard]] bool validated(const Endpoint &Source) const;
   [[nodiscard]] bool validated(const Endpoint &Source, const Endpoint &Local) const;
   [[nodiscard]] State state() const { return current_state; }
   [[nodiscard]] Role role() const { return current_role; }
   [[nodiscard]] int64_t timeout_us(uint64_t NowUs) const;

private:
   enum class PairState { FROZEN, WAITING, IN_PROGRESS, SUCCEEDED, FAILED };

   struct Pair { // A checklist entry
      Candidate local;
      Candidate remote;
      uint64_t priority = 0;
      PairState state = PairState::FROZEN;
      bool peer_nominated = false;   // The peer sent USE-CANDIDATE for this pair
      Candidate valid_local;         // Local candidate of the valid pair, formed from the mapped address
      uint64_t valid_priority = 0;
      uint64_t rtt_us = 0;
      bool rtt_valid = false;
   };

   struct Transaction { // An outstanding connectivity check
      std::vector<uint8_t> datagram;
      std::array<uint8_t, 12> id{};
      size_t pair_index = 0;
      uint64_t sent_us = 0;
      uint64_t due_us = 0;
      uint64_t expires_us = 0;
      uint32_t rto_us = 500000;
      int attempts = 0;
      bool nomination = false;
      bool cancelled = false; // Superseded by a triggered check; retained only to match a late response
   };

   struct ConsentTransaction {
      std::array<uint8_t, 12> id{};
      uint64_t sent_us = 0;
   };

   bool learn_remote_candidate(const Candidate &Value);
   void queue_triggered_check(size_t PairIndex);
   void form_pair(const Candidate &Local, const Candidate &Remote);
   void recompute_priorities();
   void thaw_pairs();
   static bool same_foundation(const Pair &Left, const Pair &Right);
   void send_check(size_t PairIndex, bool Nomination, bool Consent, uint64_t NowUs,
      std::vector<Outbound> &Output);
   void process_request(std::span<const uint8_t> Datagram, const stun::Message &Parsed,
      const Endpoint &Local, const Endpoint &Source, uint64_t NowUs, std::vector<Outbound> &Output);
   void process_response(std::span<const uint8_t> Datagram, const stun::Message &Parsed,
      const Endpoint &Local, const Endpoint &Source, uint64_t NowUs, std::vector<Outbound> &Output);
   void update_state(uint64_t NowUs);
   bool switch_role();
   bool schedule_consent(uint64_t NowUs);
   void remember_source(const Endpoint &Source, const Endpoint &Local);
   static uint64_t pair_priority(uint32_t Local, uint32_t Remote, Role CurrentRole);

   std::string local_ufrag;
   std::string local_password;
   std::string remote_ufrag;
   std::string remote_password;
   std::vector<Candidate> local_candidates;
   std::vector<Candidate> remote_candidates;
   std::vector<Pair> pairs;
   std::vector<std::pair<Endpoint, Endpoint>> valid_sources; // Authenticated local route and peer pairs.
   std::vector<Transaction> transactions;
   std::deque<size_t> triggered_checks;
   std::vector<ConsentTransaction> consent_transactions;
   Role current_role;
   State current_state = State::NEW;
   uint64_t tie_breaker = 0;
   uint64_t last_consent_us = 0;
   uint64_t next_consent_us = 0;
   uint64_t next_check_us = 0;
   bool last_check_ipv6 = false, checked_family = false;
   size_t selected_index = size_t(-1);
   size_t pair_limit = 256;
   bool input_accepted = false;
   bool remote_lite = false;
   bool remote_complete = false; // No further remote candidates will be signalled
};

} // namespace rtc::ice
