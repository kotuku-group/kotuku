// ICE and DTLS datagram demultiplexers.  The caller owns the UDP socket and timer.

#pragma once

#include "../dtls/dtls_transport.h"
#include "ice.h"
#include "ice_agent.h"

#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace rtc {

struct IceDtlsCallbacks {
   std::function<void(std::span<const uint8_t>, const ice::Endpoint &)> send; // Datagram, destination
   std::function<void(std::span<const uint8_t>)> receive; // Decrypted application data
   std::function<void(DtlsState)> state;

   // Datagram, local route and destination.  Takes precedence over send if defined.

   std::function<void(std::span<const uint8_t>, const ice::Endpoint &, const ice::Endpoint &)> send_from;
};

//********************************************************************************************************************
// ICE-lite responder combined with a DTLS server.

class IceDtlsTransport {
public:
   IceDtlsTransport(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
      std::string_view LocalPassword, std::string_view RemoteUfrag, IceDtlsCallbacks Callbacks);

   [[nodiscard]] ERR set_remote_fingerprint(std::string_view Fingerprint);
   [[nodiscard]] ERR feed(std::span<const uint8_t> Datagram, const ice::Endpoint &Source);
   [[nodiscard]] ERR handle_timeout();
   [[nodiscard]] int64_t timeout_us() const;

   [[nodiscard]] DtlsState state() const { return dtls.state(); }
   [[nodiscard]] const ice::Endpoint * nominated() const { return ice_responder.nominated(); }
   [[nodiscard]] bool validated(const ice::Endpoint &Source) const { return ice_responder.validated(Source); }

private:
   ice::LiteResponder ice_responder;
   IceDtlsCallbacks callbacks;
   DtlsTransport dtls;
   ice::Endpoint active_source;
   bool has_active_source = false;
};

//********************************************************************************************************************
// Full ICE and DTLS transport for one bundled UDP component.  The caller supplies
// the local socket address and a monotonic timestamp for each received datagram.

class FullIceDtlsTransport {
public:
   FullIceDtlsTransport(std::shared_ptr<DtlsCertificate> Certificate, std::string_view LocalUfrag,
      std::string_view LocalPassword, ice::Role IceRole, DtlsRole DtlsRole, IceDtlsCallbacks Callbacks);

   [[nodiscard]] ERR set_remote_fingerprint(std::string_view Fingerprint);
   [[nodiscard]] ERR set_dtls_role(DtlsRole Role);
   [[nodiscard]] bool set_remote_credentials(std::string_view Ufrag, std::string_view Password, bool Lite);
   void set_mtu(int Bytes) { dtls.set_mtu(Bytes); }
   [[nodiscard]] bool add_local_candidate(const ice::Candidate &Value);
   void retire_local_route(const ice::Endpoint &Local, bool Recover = false);
   [[nodiscard]] bool add_remote_candidate(const ice::Candidate &Value);
   void end_remote_candidates();
   [[nodiscard]] bool start_ice(uint64_t NowUs);
   void restart_ice(std::string_view LocalUfrag, std::string_view LocalPassword, ice::Role NewRole);
   [[nodiscard]] ERR feed(std::span<const uint8_t> Datagram, const ice::Endpoint &Local,
      const ice::Endpoint &Source, uint64_t NowUs);
   [[nodiscard]] ERR handle_timeout(uint64_t NowUs);
   [[nodiscard]] int64_t timeout_us(uint64_t NowUs) const;
   [[nodiscard]] ERR send(std::span<const uint8_t> Data);
   [[nodiscard]] int max_payload() const { return dtls.max_payload(); }

   [[nodiscard]] DtlsState state() const { return dtls.state(); }
   [[nodiscard]] ice::State ice_state() const { return agent.state(); }
   [[nodiscard]] ice::Role ice_role() const { return agent.role(); }
   [[nodiscard]] DtlsRole dtls_role() const { return dtls.role(); }
   [[nodiscard]] const ice::Candidate * selected_local_candidate() const { return agent.selected_local_candidate(); }
   [[nodiscard]] const ice::Candidate * selected_remote_candidate() const { return agent.selected_remote_candidate(); }
   [[nodiscard]] uint64_t selected_rtt_us() const { return agent.selected_rtt_us(); }
   [[nodiscard]] bool selected_rtt_valid() const { return agent.selected_rtt_valid(); }
   [[nodiscard]] bool has_pair(const ice::Endpoint &Local, const ice::Endpoint &Remote) const {
      return agent.has_pair(Local, Remote);
   }
   [[nodiscard]] bool accepted_input() const { return input_accepted; }
   [[nodiscard]] const ice::Endpoint * selected_remote() const { return agent.selected_remote(); }
   [[nodiscard]] const ice::Endpoint * selected_local() const { return agent.selected_local(); }

private:
   void dispatch(std::vector<ice::Outbound> &Output);
   [[nodiscard]] ERR maybe_start_dtls();

   bool input_accepted = false;
   ice::FullAgent agent;
   IceDtlsCallbacks callbacks;
   DtlsTransport dtls;
   ice::Endpoint active_source;
   ice::Endpoint active_local;
   bool has_active_source = false;
};

} // namespace rtc
