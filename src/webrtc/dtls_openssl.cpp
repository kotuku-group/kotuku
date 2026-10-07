// OpenSSL backend for DtlsTransport and DtlsCertificate.
//
// OpenSSL is attached through a custom BIO rather than BIO_s_mem().  A memory BIO is a byte stream, so a handshake
// flight written as several records would be read back by the ICE layer as one undivided blob and the datagram
// boundaries that DTLS depends on would be lost.  The custom BIO maps every BIO_write() to exactly one outbound
// datagram and every BIO_read() to exactly one inbound datagram.  BIO_s_dgram_mem() would also preserve boundaries,
// but it requires OpenSSL 3.2 and the project's minimum is 1.1.1.
//
// MTU discovery is disabled (SSL_OP_NO_QUERY_MTU) and the configured MTU is applied directly with SSL_set_mtu(), which
// OpenSSL treats as the maximum datagram payload.  Handshake messages larger than this are fragmented by OpenSSL.

#include "dtls_transport.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <mutex>

#if OPENSSL_VERSION_NUMBER < 0x10101000L
#error "The WebRTC DTLS transport requires OpenSSL 1.1.1 or newer."
#endif

namespace rtc {

struct DtlsCertificate::Native {
   EVP_PKEY *key = nullptr;
   X509 *cert = nullptr;

   ~Native() {
      if (cert) X509_free(cert);
      if (key) EVP_PKEY_free(key);
   }
};

struct DtlsTransport::Native {
   SSL_CTX *ctx = nullptr;
   SSL *ssl = nullptr;

   ~Native() {
      if (ssl) SSL_free(ssl); // Also frees the BIO
      if (ctx) SSL_CTX_free(ctx);
   }
};

//********************************************************************************************************************
// Error and formatting helpers

static std::string openssl_errors(std::string_view Context)
{
   std::string message(Context);
   while (auto code = ERR_get_error()) {
      std::array<char, 256> buffer;
      ERR_error_string_n(code, buffer.data(), buffer.size());
      message += "; ";
      message += buffer.data();
   }
   return message;
}

static std::string format_fingerprint(std::span<const uint8_t> Digest)
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

static std::string certificate_fingerprint(X509 *Cert)
{
   std::array<uint8_t, EVP_MAX_MD_SIZE> digest;
   unsigned int length = 0;
   if (X509_digest(Cert, EVP_sha256(), digest.data(), &length) != 1) return std::string();
   return format_fingerprint(std::span<const uint8_t>(digest.data(), length));
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

// Master key and salt lengths in bytes (RFC 3711, RFC 7714).

static bool srtp_lengths(SrtpProfile Profile, size_t &KeyLength, size_t &SaltLength)
{
   switch (Profile) {
      case SrtpProfile::AES128_CM_SHA1_80:
      case SrtpProfile::AES128_CM_SHA1_32: KeyLength = 16; SaltLength = 14; return true;
      case SrtpProfile::AEAD_AES_128_GCM:  KeyLength = 16; SaltLength = 12; return true;
      case SrtpProfile::AEAD_AES_256_GCM:  KeyLength = 32; SaltLength = 12; return true;
      default: return false;
   }
}

//********************************************************************************************************************
// Datagram BIO.  The BIO's data pointer refers to the owning DtlsTransport.

static int datagram_write(BIO *Bio, const char *Data, int Length)
{
   auto transport = (DtlsTransport *)BIO_get_data(Bio);
   if ((!transport) or (Length < 0)) return -1;
   BIO_clear_retry_flags(Bio);
   transport->emit_datagram(std::span<const uint8_t>((const uint8_t *)Data, size_t(Length)));
   return Length;
}

static int datagram_read(BIO *Bio, char *Buffer, int Length)
{
   auto transport = (DtlsTransport *)BIO_get_data(Bio);
   if ((!transport) or (Length < 0)) return -1;
   BIO_clear_retry_flags(Bio);

   auto &queue = transport->inbound_queue();
   if (queue.empty()) {
      BIO_set_retry_read(Bio);
      return -1;
   }

   // A datagram larger than the read buffer is truncated, as it would be by recvfrom().  OpenSSL always supplies a
   // buffer large enough for a maximum-size record, so this only discards oversized garbage.

   auto &datagram = queue.front();
   auto copy = std::min(size_t(Length), datagram.size());
   std::memcpy(Buffer, datagram.data(), copy);
   queue.pop_front();
   return int(copy);
}

static long datagram_ctrl(BIO *Bio, int Command, long Number, void *Pointer)
{
   auto transport = (DtlsTransport *)BIO_get_data(Bio);
   switch (Command) {
      case BIO_CTRL_FLUSH: return 1;
      case BIO_CTRL_DGRAM_QUERY_MTU: return transport ? transport->link_mtu() : 0;
      case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD: return 0; // The configured MTU already excludes UDP/IP headers
      case BIO_CTRL_WPENDING: return 0;                // Writes are never buffered
      case BIO_CTRL_PENDING: {
         if ((!transport) or transport->inbound_queue().empty()) return 0;
         return long(transport->inbound_queue().front().size());
      }
      default: return 0;
   }
}

static int datagram_create(BIO *Bio)
{
   BIO_set_init(Bio, 1);
   BIO_set_data(Bio, nullptr);
   return 1;
}

static int datagram_destroy(BIO *Bio)
{
   BIO_set_data(Bio, nullptr);
   return 1;
}

// The method table is created once and shared.  Function-local static initialisation is thread-safe.

static BIO_METHOD * datagram_method()
{
   static BIO_METHOD *glDatagramMethod = []() -> BIO_METHOD * {
      auto method = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "kotuku-webrtc-datagram");
      if (!method) return nullptr;
      BIO_meth_set_write(method, datagram_write);
      BIO_meth_set_read(method, datagram_read);
      BIO_meth_set_ctrl(method, datagram_ctrl);
      BIO_meth_set_create(method, datagram_create);
      BIO_meth_set_destroy(method, datagram_destroy);
      return method;
   }();
   return glDatagramMethod;
}

//********************************************************************************************************************
// OpenSSL callbacks

static DtlsTransport * transport_from_ssl(SSL *Ssl)
{
   return (DtlsTransport *)SSL_get_app_data(Ssl);
}

// Browsers present a single self-signed certificate, so chain validation is replaced by fingerprint comparison on the
// leaf certificate.  Returning zero aborts the handshake with a bad_certificate alert.

static int verify_peer(int PreverifyOk, X509_STORE_CTX *Context)
{
   if (X509_STORE_CTX_get_error_depth(Context) != 0) return 1;

   auto ssl = (SSL *)X509_STORE_CTX_get_ex_data(Context, SSL_get_ex_data_X509_STORE_CTX_idx());
   auto transport = ssl ? transport_from_ssl(ssl) : nullptr;
   auto cert = X509_STORE_CTX_get_current_cert(Context);
   if ((!transport) or (!cert)) return 0;

   return transport->check_peer_fingerprint(certificate_fingerprint(cert)) ? 1 : 0;
}

static unsigned int retransmit_timer(SSL *Ssl, unsigned int PreviousUs)
{
   auto transport = transport_from_ssl(Ssl);
   return transport ? (unsigned int)transport->next_timeout(int(PreviousUs)) : 1000000;
}

//********************************************************************************************************************
// DtlsCertificate

DtlsCertificate::DtlsCertificate() = default;
DtlsCertificate::~DtlsCertificate() = default;

ERR DtlsCertificate::generate(std::shared_ptr<DtlsCertificate> &Result, std::string_view CommonName)
{
   auto certificate = std::make_shared<DtlsCertificate>();
   certificate->native_data = std::make_unique<Native>();
   auto &native = *certificate->native_data;

   // ECDSA P-256 is what browsers generate by default and the cheapest to sign with.  The EVP_PKEY_CTX interface is
   // used because it is available in both OpenSSL 1.1.1 and 3.x.

   auto key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
   if (!key_ctx) return ERR::AllocMemory;
   bool key_ok = (EVP_PKEY_keygen_init(key_ctx) IS 1) and
      (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(key_ctx, NID_X9_62_prime256v1) IS 1) and
      (EVP_PKEY_keygen(key_ctx, &native.key) IS 1);
   EVP_PKEY_CTX_free(key_ctx);
   if (!key_ok) return ERR::Failed;

   if (!(native.cert = X509_new())) return ERR::AllocMemory;

   std::array<uint8_t, 8> serial_bytes;
   if (RAND_bytes(serial_bytes.data(), int(serial_bytes.size())) != 1) return ERR::Failed;
   serial_bytes[0] &= 0x7f; // Keep the serial positive

   auto serial = BN_bin2bn(serial_bytes.data(), int(serial_bytes.size()), nullptr);
   if (!serial) return ERR::AllocMemory;
   bool serial_ok = BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(native.cert)) != nullptr;
   BN_free(serial);
   if (!serial_ok) return ERR::Failed;

   // Browsers do not check validity periods on DTLS certificates, but a sane window avoids surprises with stricter
   // peers.  The start time is back-dated to tolerate clock skew.

   std::string common_name(CommonName);
   auto name = X509_get_subject_name(native.cert);
   if ((X509_set_version(native.cert, 2) != 1) or
       (!X509_gmtime_adj(X509_getm_notBefore(native.cert), -86400)) or
       (!X509_gmtime_adj(X509_getm_notAfter(native.cert), 30L * 86400)) or
       (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char *)common_name.c_str(), -1, -1, 0)
         != 1) or
       (X509_set_issuer_name(native.cert, name) != 1) or
       (X509_set_pubkey(native.cert, native.key) != 1) or
       (X509_sign(native.cert, native.key, EVP_sha256()) <= 0)) {
      return ERR::Failed;
   }

   certificate->fingerprint_text = certificate_fingerprint(native.cert);
   if (certificate->fingerprint_text.empty()) return ERR::Failed;

   Result = std::move(certificate);
   return ERR::Okay;
}

//********************************************************************************************************************
// DtlsTransport

DtlsTransport::DtlsTransport(std::shared_ptr<DtlsCertificate> Certificate, DtlsRole Role, DtlsCallbacks Callbacks) :
   certificate(std::move(Certificate)), callbacks(std::move(Callbacks)), local_role(Role)
{
   srtp_profiles = { SrtpProfile::AEAD_AES_128_GCM, SrtpProfile::AES128_CM_SHA1_80 };
}

DtlsTransport::~DtlsTransport()
{
   if ((native_data) and (native_data->ssl)) {
      if (auto bio = SSL_get_rbio(native_data->ssl)) BIO_set_data(bio, nullptr);
   }
}

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

// Called from the verify callback with the fingerprint of the peer's leaf certificate.  If the remote fingerprint is
// not yet known, the comparison is deferred to set_remote_fingerprint().

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

ERR DtlsTransport::start()
{
   if (current_state != DtlsState::NEW) return ERR::InvalidState;
   if ((!certificate) or (!certificate->native())) return ERR::NullArgs;
   if (mtu < 256) return ERR::OutOfRange; // Below OpenSSL's minimum DTLS MTU

   auto method = datagram_method();
   if (!method) return ERR::AllocMemory;

   native_data = std::make_unique<Native>();
   auto &native = *native_data;
   ERR_clear_error();

   if (!(native.ctx = SSL_CTX_new(DTLS_method()))) return fail(openssl_errors("SSL_CTX_new"));

   // WebRTC endpoints negotiate DTLS 1.2 with ECDHE-ECDSA suites (RFC 8827 section 6.5).  DTLS 1.3 is not yet
   // deployed by browsers and OpenSSL before 3.5 does not implement it.

   auto ctx = native.ctx;
   std::string profiles;
   for (auto profile : srtp_profiles) {
      if (auto name = srtp_profile_name(profile); !name.empty()) {
         if (!profiles.empty()) profiles += ':';
         profiles += name;
      }
   }

   if ((SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION) != 1) or
       (SSL_CTX_use_certificate(ctx, certificate->native()->cert) != 1) or
       (SSL_CTX_use_PrivateKey(ctx, certificate->native()->key) != 1) or
       (SSL_CTX_check_private_key(ctx) != 1) or
       (SSL_CTX_set_cipher_list(ctx,
         "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES256-GCM-SHA384") != 1) or
       (SSL_CTX_set1_groups_list(ctx, "X25519:P-256") != 1)) {
      return fail(openssl_errors("DTLS context configuration"));
   }

   // Note the inverted convention: SSL_CTX_set_tlsext_use_srtp() returns zero on success.

   if ((!profiles.empty()) and (SSL_CTX_set_tlsext_use_srtp(ctx, profiles.c_str()) != 0)) {
      return fail(openssl_errors("SSL_CTX_set_tlsext_use_srtp"));
   }

   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verify_peer);
   SSL_CTX_set_options(ctx, SSL_OP_NO_QUERY_MTU | SSL_OP_NO_TICKET);

   if (!(native.ssl = SSL_new(ctx))) return fail(openssl_errors("SSL_new"));
   SSL_set_app_data(native.ssl, this);

   auto bio = BIO_new(method);
   if (!bio) return fail(openssl_errors("BIO_new"));
   BIO_set_data(bio, this);
   SSL_set_bio(native.ssl, bio, bio);

   if (!SSL_set_mtu(native.ssl, mtu)) return fail(openssl_errors("SSL_set_mtu"));
   DTLS_set_timer_cb(native.ssl, retransmit_timer);

   if (local_role IS DtlsRole::CLIENT) SSL_set_connect_state(native.ssl);
   else SSL_set_accept_state(native.ssl);

   set_state(DtlsState::CONNECTING);

   if (local_role IS DtlsRole::CLIENT) return drive(); // Sends the ClientHello
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

// Advance the handshake and drain decrypted records.  Records received before the peer is verified are discarded,
// which is safe because a conforming peer sends no application data until it has seen our Finished message, and
// SCTP retransmits anything lost.

ERR DtlsTransport::drive()
{
   auto ssl = native_data->ssl;
   ERR_clear_error();

   if (!handshake_done) {
      auto result = SSL_do_handshake(ssl);
      if (result IS 1) {
         if (auto error = complete_handshake(); error != ERR::Okay) return error;
      }
      else {
         auto code = SSL_get_error(ssl, result);
         if ((code IS SSL_ERROR_WANT_READ) or (code IS SSL_ERROR_WANT_WRITE)) return ERR::Okay;
         if (last_error.empty()) return fail(openssl_errors("DTLS handshake failed"));
         return fail(last_error + openssl_errors(""));
      }
   }

   std::array<uint8_t, 16384> buffer;
   while (true) {
      auto result = SSL_read(ssl, buffer.data(), int(buffer.size()));
      if (result > 0) {
         if ((current_state IS DtlsState::CONNECTED) and (callbacks.receive)) {
            callbacks.receive(std::span<const uint8_t>(buffer.data(), size_t(result)));
         }
         continue;
      }

      auto code = SSL_get_error(ssl, result);
      if ((code IS SSL_ERROR_WANT_READ) or (code IS SSL_ERROR_WANT_WRITE)) return ERR::Okay;
      if (code IS SSL_ERROR_ZERO_RETURN) {
         inbound.clear();
         set_state(DtlsState::CLOSED);
         return ERR::Okay;
      }
      return fail(openssl_errors("DTLS read failed"));
   }
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

ERR DtlsTransport::send(std::span<const uint8_t> Data)
{
   if (current_state != DtlsState::CONNECTED) return ERR::InvalidState;
   if (Data.empty()) return ERR::Okay;
   if (Data.size() > size_t(max_payload())) return ERR::DataSize;

   ERR_clear_error();
   auto result = SSL_write(native_data->ssl, Data.data(), int(Data.size()));
   if (result IS int(Data.size())) return ERR::Okay;
   return fail(openssl_errors("DTLS write failed"));
}

ERR DtlsTransport::close()
{
   if ((current_state != DtlsState::CONNECTING) and (current_state != DtlsState::CONNECTED)) return ERR::Okay;
   ERR_clear_error();
   SSL_shutdown(native_data->ssl); // Sends close_notify; a reply is not awaited because DTLS is unreliable
   inbound.clear();
   set_state(DtlsState::CLOSED);
   return ERR::Okay;
}

int64_t DtlsTransport::timeout_us() const
{
   if ((!native_data) or (!native_data->ssl) or (handshake_done)) return -1;
   struct timeval remaining = {};
   if (DTLSv1_get_timeout(native_data->ssl, &remaining) <= 0) return -1;
   return (int64_t(remaining.tv_sec) * 1000000) + remaining.tv_usec;
}

ERR DtlsTransport::handle_timeout()
{
   if ((current_state != DtlsState::CONNECTING) or (handshake_done)) return ERR::Okay;
   ERR_clear_error();
   if (DTLSv1_handle_timeout(native_data->ssl) < 0) return fail(openssl_errors("DTLS handshake timed out"));
   return ERR::Okay;
}

int DtlsTransport::max_payload() const
{
   if ((!native_data) or (!native_data->ssl)) return 0;
   return int(DTLS_get_data_mtu(native_data->ssl));
}

ERR DtlsTransport::srtp_keys(SrtpKeyMaterial &Keys) const
{
   if (current_state != DtlsState::CONNECTED) return ERR::InvalidState;

   auto selected = SSL_get_selected_srtp_profile(native_data->ssl);
   if (!selected) return ERR::NoSupport;

   auto profile = SrtpProfile(selected->id);
   size_t key_length, salt_length;
   if (!srtp_lengths(profile, key_length, salt_length)) return ERR::NoSupport;

   // RFC 5764 section 4.2: client_write_key | server_write_key | client_write_salt | server_write_salt

   static constexpr char LABEL[] = "EXTRACTOR-dtls_srtp";
   std::vector<uint8_t> material((key_length + salt_length) * 2);
   if (SSL_export_keying_material(native_data->ssl, material.data(), material.size(), LABEL, sizeof(LABEL) - 1,
      nullptr, 0, 0) != 1) {
      return ERR::Failed;
   }

   auto at = material.begin();
   std::vector<uint8_t> client_key(at, at + key_length);
   at += key_length;
   std::vector<uint8_t> server_key(at, at + key_length);
   at += key_length;
   std::vector<uint8_t> client_salt(at, at + salt_length);
   at += salt_length;
   std::vector<uint8_t> server_salt(at, at + salt_length);

   Keys.profile = profile;
   if (local_role IS DtlsRole::CLIENT) {
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
   return ERR::Okay;
}

} // namespace rtc
