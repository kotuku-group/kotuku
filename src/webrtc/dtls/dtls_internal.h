// Helpers shared by dtls_transport.cpp and the DTLS backends.  Not part of the public contract.

#pragma once

#include "dtls_transport.h"

namespace rtc::dtls {

// Formats a digest as upper-case, colon-separated hex octets, as used by SDP `a=fingerprint` lines.

[[nodiscard]] std::string format_fingerprint(std::span<const uint8_t> Digest);

// Master key and salt lengths in bytes (RFC 3711, RFC 7714).  Returns false for unknown profiles.

[[nodiscard]] bool srtp_lengths(SrtpProfile Profile, size_t &KeyLength, size_t &SaltLength);

// Splits `EXTRACTOR-dtls_srtp` keying material into local and remote keys for the given role.  Material must hold
// 2 * (key + salt) bytes for Profile, in RFC 5764 section 4.2 order.

[[nodiscard]] bool split_srtp_material(SrtpProfile Profile, DtlsRole Role, std::span<const uint8_t> Material,
   SrtpKeyMaterial &Keys);

} // namespace rtc::dtls
