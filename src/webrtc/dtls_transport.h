// DTLS transport for WebRTC (RFC 6347, RFC 8827, RFC 5764).
//
// The transport never owns a socket.  The ICE layer demultiplexes each received datagram by its first byte (RFC 7983)
// and passes DTLS records to feed().  Outbound datagrams are handed to the Send callback, one datagram per call, and
// are never coalesced, so the ICE layer can transmit each one as-is on the selected candidate pair.
//
// The peer is authenticated by comparing the SHA-256 fingerprint of its certificate against the fingerprint signalled
// in SDP.  When the remote description has not yet arrived, the handshake is allowed to finish and the transport
// stays in CONNECTING until set_remote_fingerprint() confirms or rejects the peer.
//
// Callbacks are invoked synchronously from within start(), feed(), send(), handle_timeout() and close().  They must
// not call back into the same transport.
//
// This file has no object-system dependencies.

#pragma once

#include <kotuku/config.h>
#include <kotuku/system/errors.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtc {

// The local DTLS role.  In SDP terms, `a=setup:active` is the CLIENT and `a=setup:passive` is the SERVER.

enum class DtlsRole : uint8_t { CLIENT, SERVER };

enum class DtlsState : uint8_t {
   NEW,        // start() has not been called
   CONNECTING, // Handshake in progress, or complete but awaiting the remote fingerprint
   CONNECTED,  // Handshake complete and the peer fingerprint verified
   CLOSED,     // A close_notify was sent or received
   FAILED      // Handshake failure, fingerprint mismatch or fatal alert
};

// SRTP protection profiles negotiated through the use_srtp extension.  Values are the IANA identifiers.

enum class SrtpProfile : uint16_t {
   NONE              = 0x0000,
   AES128_CM_SHA1_80 = 0x0001,
   AES128_CM_SHA1_32 = 0x0002,
   AEAD_AES_128_GCM  = 0x0007,
   AEAD_AES_256_GCM  = 0x0008
};

// Master keys and salts for SRTP, split by direction.  The local values protect outbound packets.

struct SrtpKeyMaterial {
   SrtpProfile profile = SrtpProfile::NONE;
   std::vector<uint8_t> local_key;
   std::vector<uint8_t> local_salt;
   std::vector<uint8_t> remote_key;
   std::vector<uint8_t> remote_salt;
};

//********************************************************************************************************************
// A self-signed ECDSA P-256 certificate.  One certificate is normally generated per PeerConnection and shared by
// every DTLS transport that it owns.

class DtlsCertificate {
public:
   struct Native;

   [[nodiscard]] static ERR generate(std::shared_ptr<DtlsCertificate> &Result, std::string_view CommonName = "kotuku");

   // The SHA-256 fingerprint as it appears in an SDP `a=fingerprint:sha-256` line, e.g. `AB:CD:...`.

   [[nodiscard]] const std::string & fingerprint() const noexcept { return fingerprint_text; }
   [[nodiscard]] Native * native() const noexcept { return native_data.get(); }

   DtlsCertificate();
   ~DtlsCertificate();
   DtlsCertificate(const DtlsCertificate &) = delete;
   DtlsCertificate & operator=(const DtlsCertificate &) = delete;

private:
   std::unique_ptr<Native> native_data;
   std::string fingerprint_text;
};

//********************************************************************************************************************

struct DtlsCallbacks {
   std::function<void(std::span<const uint8_t>)> send;    // Transmit one outbound datagram
   std::function<void(std::span<const uint8_t>)> receive; // Deliver one decrypted application record
   std::function<void(DtlsState)> state;                  // Report a state change
};

class DtlsTransport {
public:
   struct Native;

   static constexpr int DEFAULT_MTU = 1200;               // Conservative path MTU used by browsers
   static constexpr int DEFAULT_INITIAL_TIMEOUT_MS = 100; // First retransmission interval
   static constexpr int MAX_TIMEOUT_MS = 6000;            // Retransmission back-off ceiling

   DtlsTransport(std::shared_ptr<DtlsCertificate> Certificate, DtlsRole Role, DtlsCallbacks Callbacks);
   ~DtlsTransport();
   DtlsTransport(const DtlsTransport &) = delete;
   DtlsTransport & operator=(const DtlsTransport &) = delete;

   // Configuration.  These must be called before start().

   void set_mtu(int Bytes) noexcept { mtu = Bytes; }
   void set_initial_timeout(int Milliseconds) noexcept { initial_timeout_ms = Milliseconds; }
   void set_srtp_profiles(std::span<const SrtpProfile> Profiles);

   // The fingerprint may be supplied before or after the handshake.  Only `sha-256` is accepted.

   [[nodiscard]] ERR set_remote_fingerprint(std::string_view Algorithm, std::string_view Fingerprint);

   [[nodiscard]] ERR start();
   [[nodiscard]] ERR feed(std::span<const uint8_t> Datagram);
   [[nodiscard]] ERR send(std::span<const uint8_t> Data);
   [[nodiscard]] ERR close();

   // Retransmission.  timeout_us() returns the microseconds until handle_timeout() must be called, or -1 if no timer
   // is running.  The caller arms its own timer from this value.

   [[nodiscard]] int64_t timeout_us() const;
   [[nodiscard]] ERR handle_timeout();

   [[nodiscard]] DtlsState state() const noexcept { return current_state; }
   [[nodiscard]] DtlsRole role() const noexcept { return local_role; }
   [[nodiscard]] const std::string & error_message() const noexcept { return last_error; }

   // The largest application payload that fits in one datagram at the configured MTU.  Valid once connected.

   [[nodiscard]] int max_payload() const;

   // SRTP keys exported with the `EXTRACTOR-dtls_srtp` label.  Fails if no profile was negotiated.

   [[nodiscard]] ERR srtp_keys(SrtpKeyMaterial &Keys) const;

   // Internal hooks for the backend.  Not part of the public contract.

   void emit_datagram(std::span<const uint8_t> Datagram);
   [[nodiscard]] std::deque<std::vector<uint8_t>> & inbound_queue() noexcept { return inbound; }
   [[nodiscard]] int link_mtu() const noexcept { return mtu; }
   [[nodiscard]] int next_timeout(int PreviousUs) const noexcept;
   [[nodiscard]] bool check_peer_fingerprint(std::string_view Fingerprint);

private:
   void set_state(DtlsState State);
   [[nodiscard]] ERR fail(std::string Message);
   [[nodiscard]] ERR drive();
   [[nodiscard]] ERR complete_handshake();

   std::shared_ptr<DtlsCertificate> certificate;
   std::unique_ptr<Native> native_data;
   DtlsCallbacks callbacks;
   std::deque<std::vector<uint8_t>> inbound;
   std::vector<SrtpProfile> srtp_profiles;
   std::string remote_fingerprint;
   std::string peer_fingerprint; // Fingerprint of the certificate presented by the peer
   std::string last_error;
   int mtu = DEFAULT_MTU;
   int initial_timeout_ms = DEFAULT_INITIAL_TIMEOUT_MS;
   DtlsRole local_role;
   DtlsState current_state = DtlsState::NEW;
   bool handshake_done = false;
};

//********************************************************************************************************************
// RFC 7983 first-byte classification, used by the ICE layer to route datagrams that share one socket.

enum class PacketClass : uint8_t { UNKNOWN, STUN, ZRTP, DTLS, TURN_CHANNEL, RTP };

[[nodiscard]] constexpr PacketClass classify_packet(uint8_t FirstByte) noexcept
{
   if (FirstByte <= 3) return PacketClass::STUN;
   if ((FirstByte >= 16) and (FirstByte <= 19)) return PacketClass::ZRTP;
   if ((FirstByte >= 20) and (FirstByte <= 63)) return PacketClass::DTLS;
   if ((FirstByte >= 64) and (FirstByte <= 79)) return PacketClass::TURN_CHANNEL;
   if ((FirstByte >= 128) and (FirstByte <= 191)) return PacketClass::RTP;
   return PacketClass::UNKNOWN;
}

[[nodiscard]] std::string_view srtp_profile_name(SrtpProfile Profile) noexcept;

} // namespace rtc
