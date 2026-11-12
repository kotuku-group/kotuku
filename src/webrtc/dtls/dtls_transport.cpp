// Backend-independent parts of DtlsTransport: fingerprint handling, state reporting, retransmission back-off and SRTP
// key splitting.  The handshake and record layers are implemented by a backend (dtls_openssl.cpp).

#include "dtls_internal.h"

#include <algorithm>
#include <cctype>

namespace rtc {

//********************************************************************************************************************
// Formatting helpers

std::string dtls::format_fingerprint(std::span<const uint8_t> Digest)
{
   static constexpr char HEX[] = "0123456789ABCDEF";
   std::string text;
   text.reserve(Digest.size() * 3);
   for (size_t i = 0; i < Digest.size(); i++) {
      if (i > 0) text += ':';
      text += HEX[Digest[i] >> 4];
      text += HEX[Digest[i] & 0x0f];
   }
   return text;
}

// Normalise an SDP fingerprint to upper case.  Returns an empty string if the value is not 32 colon-separated octets.

static std::string normalise_fingerprint(std::string_view Fingerprint)
{
   if (Fingerprint.size() != 32 * 3 - 1) return std::string();
   std::string result(Fingerprint);
   for (size_t i = 0; i < result.size(); i++) {
      if ((i % 3) IS 2) {
         if (result[i] != ':') return std::string();
      }
      else if (std::isxdigit((unsigned char)result[i])) result[i] = char(std::toupper((unsigned char)result[i]));
      else return std::string();
   }
   return result;
}

std::string_view srtp_profile_name(SrtpProfile Profile) noexcept
{
   switch (Profile) {
      case SrtpProfile::AES128_CM_SHA1_80: return "SRTP_AES128_CM_SHA1_80";
      case SrtpProfile::AES128_CM_SHA1_32: return "SRTP_AES128_CM_SHA1_32";
      case SrtpProfile::AEAD_AES_128_GCM:  return "SRTP_AEAD_AES_128_GCM";
      case SrtpProfile::AEAD_AES_256_GCM:  return "SRTP_AEAD_AES_256_GCM";
      default: return std::string_view();
   }
}

bool dtls::srtp_lengths(SrtpProfile Profile, size_t &KeyLength, size_t &SaltLength)
{
   switch (Profile) {
      case SrtpProfile::AES128_CM_SHA1_80:
      case SrtpProfile::AES128_CM_SHA1_32: KeyLength = 16; SaltLength = 14; return true;
      case SrtpProfile::AEAD_AES_128_GCM:  KeyLength = 16; SaltLength = 12; return true;
      case SrtpProfile::AEAD_AES_256_GCM:  KeyLength = 32; SaltLength = 12; return true;
      default: return false;
   }
}

// RFC 5764 section 4.2: client_write_key | server_write_key | client_write_salt | server_write_salt

bool dtls::split_srtp_material(SrtpProfile Profile, DtlsRole Role, std::span<const uint8_t> Material,
   SrtpKeyMaterial &Keys)
{
   size_t key_length, salt_length;
   if (!srtp_lengths(Profile, key_length, salt_length)) return false;
   if (Material.size() != (key_length + salt_length) * 2) return false;

   auto at = Material.begin();
   std::vector<uint8_t> client_key(at, at + key_length);
   at += key_length;
   std::vector<uint8_t> server_key(at, at + key_length);
   at += key_length;
   std::vector<uint8_t> client_salt(at, at + salt_length);
   at += salt_length;
   std::vector<uint8_t> server_salt(at, at + salt_length);

   Keys.profile = Profile;
   if (Role IS DtlsRole::CLIENT) {
      Keys.local_key   = std::move(client_key);
      Keys.local_salt  = std::move(client_salt);
      Keys.remote_key  = std::move(server_key);
      Keys.remote_salt = std::move(server_salt);
   }
   else {
      Keys.local_key   = std::move(server_key);
      Keys.local_salt  = std::move(server_salt);
      Keys.remote_key  = std::move(client_key);
      Keys.remote_salt = std::move(client_salt);
   }
   return true;
}

//********************************************************************************************************************
// DtlsTransport

void DtlsTransport::set_srtp_profiles(std::span<const SrtpProfile> Profiles)
{
   srtp_profiles.assign(Profiles.begin(), Profiles.end());
}

void DtlsTransport::emit_datagram(std::span<const uint8_t> Datagram)
{
   if (callbacks.send) callbacks.send(Datagram);
}

int DtlsTransport::next_timeout(int PreviousUs) const noexcept
{
   if (PreviousUs <= 0) return initial_timeout_ms * 1000;
   return std::min(PreviousUs * 2, MAX_TIMEOUT_MS * 1000);
}

void DtlsTransport::set_state(DtlsState State)
{
   if (current_state IS State) return;
   current_state = State;
   if (callbacks.state) callbacks.state(State);
}

ERR DtlsTransport::fail(std::string Message)
{
   last_error = std::move(Message);
   inbound.clear();
   set_state(DtlsState::FAILED);
   return ERR::Failed;
}

// Called by the backend with the fingerprint of the peer's leaf certificate.  If the remote fingerprint is not yet
// known, the comparison is deferred to set_remote_fingerprint().

bool DtlsTransport::check_peer_fingerprint(std::string_view Fingerprint)
{
   peer_fingerprint = Fingerprint;
   if (remote_fingerprint.empty()) return true;
   if (peer_fingerprint IS remote_fingerprint) return true;
   last_error = "Peer certificate fingerprint does not match the signalled fingerprint.";
   return false;
}

ERR DtlsTransport::set_remote_fingerprint(std::string_view Algorithm, std::string_view Fingerprint)
{
   if ((Algorithm.size() != 7) or (!std::equal(Algorithm.begin(), Algorithm.end(), "sha-256",
      [](char A, char B) { return std::tolower((unsigned char)A) IS B; }))) {
      return ERR::NoSupport;
   }

   auto normalised = normalise_fingerprint(Fingerprint);
   if (normalised.empty()) return ERR::Args;
   remote_fingerprint = std::move(normalised);

   // If the handshake has already finished, this is the deferred verification step.

   if ((handshake_done) and (current_state IS DtlsState::CONNECTING)) {
      if (peer_fingerprint IS remote_fingerprint) set_state(DtlsState::CONNECTED);
      else return fail("Peer certificate fingerprint does not match the signalled fingerprint.");
   }
   return ERR::Okay;
}

ERR DtlsTransport::complete_handshake()
{
   handshake_done = true;
   if (remote_fingerprint.empty()) return ERR::Okay; // Wait for set_remote_fingerprint()
   if (peer_fingerprint IS remote_fingerprint) {
      set_state(DtlsState::CONNECTED);
      return ERR::Okay;
   }
   return fail("Peer certificate fingerprint does not match the signalled fingerprint.");
}

ERR DtlsTransport::feed(std::span<const uint8_t> Datagram)
{
   if ((current_state != DtlsState::CONNECTING) and (current_state != DtlsState::CONNECTED)) {
      return ERR::InvalidState;
   }
   if (Datagram.empty()) return ERR::Okay;
   if (classify_packet(Datagram[0]) != PacketClass::DTLS) return ERR::InvalidData;

   inbound.emplace_back(Datagram.begin(), Datagram.end());
   return drive();
}

} // namespace rtc
