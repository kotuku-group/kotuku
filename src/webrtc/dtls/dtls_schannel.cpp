// Schannel DTLS 1.2 backend.  See the Phase 0 findings in docs/plans/net/1_webrtc_windows_native_dtls.md for the
// parameter-buffer layout, final-flight replay and alert call shapes that differ from Schannel's TLS interface.

#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#define OBJECTID WindowsObjectID
#include <windows.h>
#undef OBJECTID
#undef __OBJECTID_DEFINED
#undef RELATIVE
#undef DELETE
#undef FD_READ
#undef FD_WRITE
#define STRING WindowsString
#include <subauth.h>
#undef STRING
#include <security.h>
#include <schannel.h>
#include <bcrypt.h>

#include "dtls_internal.h"
#include "../../crypto/protocol_cng.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace rtc {
namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;

std::string schannel_error(const char *Operation, SECURITY_STATUS Status)
{
   char code[32];
   std::snprintf(code, sizeof(code), " (0x%08lx)", (unsigned long)Status);

   char *message = nullptr;
   FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, DWORD(Status), 0, (LPSTR)&message, 0, nullptr);

   std::string result = std::string(Operation) + code;
   if (message) {
      result += ": ";
      result += message;
      LocalFree(message);
      while (not result.empty() and (result.back() IS '\r' or result.back() IS '\n')) result.pop_back();
   }
   return result;
}

UNICODE_STRING algorithm_name(const wchar_t *Name)
{
   auto length = USHORT(std::wcslen(Name) * sizeof(wchar_t));
   return { length, length, (PWSTR)Name };
}

SECURITY_STATUS acquire_credentials(CredHandle &Handle, PCCERT_CONTEXT Certificate, DtlsRole Role)
{
   // Network's win32/ssl_wrapper.cpp keeps a compatibility KOTUKU_SCH_CREDENTIALS layout for older SDKs.  DTLS
   // needs the actual SCH_CREDENTIALS/TLS_PARAMETERS/CRYPTO_SETTINGS definitions from the Windows 1809 SDK.
   // Keep these settings local rather than depending on the Network module's private implementation.

   auto cbc = algorithm_name(BCRYPT_CHAIN_MODE_CBC);
   CRYPTO_SETTINGS disabled[4]{};
   disabled[0].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
   disabled[0].strCngAlgId = algorithm_name(BCRYPT_RSA_ALGORITHM);
   disabled[1].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
   disabled[1].strCngAlgId = algorithm_name(BCRYPT_DH_ALGORITHM);
   disabled[2].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
   disabled[2].strCngAlgId = algorithm_name(BCRYPT_3DES_ALGORITHM);
   disabled[3].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
   disabled[3].strCngAlgId = algorithm_name(BCRYPT_AES_ALGORITHM);
   disabled[3].cChainingModes = 1;
   disabled[3].rgstrChainingModes = &cbc;

   bool server = Role IS DtlsRole::SERVER;
   TLS_PARAMETERS parameters{};
   parameters.grbitDisabledProtocols = DWORD(~(server ? SP_PROT_DTLS1_2_SERVER : SP_PROT_DTLS1_2_CLIENT));
   parameters.cDisabledCrypto = DWORD(std::size(disabled));
   parameters.pDisabledCrypto = disabled;

   SCH_CREDENTIALS credentials{};
   credentials.dwVersion = SCH_CREDENTIALS_VERSION;
   credentials.cCreds = 1;
   credentials.paCred = &Certificate;
   credentials.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_USE_DTLS_ONLY;
   if (server) credentials.dwFlags |= SCH_CRED_NO_SYSTEM_MAPPER;
   else credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
   credentials.cTlsParameters = 1;
   credentials.pTlsParameters = &parameters;

   TimeStamp expiry;
   return AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W,
      server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr, nullptr, &Handle, &expiry);
}

} // namespace

//********************************************************************************************************************

struct DtlsCertificate::Native {
   crypto_protocol::CngCertificate certificate;
};

struct DtlsTransport::Native {
   CredHandle credentials;
   CtxtHandle context;
   SecPkgContext_StreamSizes sizes{};
   std::vector<Bytes> final_flight;
   Clock::time_point deadline{}, last_replay{};
   int timeout_interval = 0;
   bool timer_armed = false;
   bool retransmitting = false;

   // A ClientHello under reassembly.  Schannel servers reject a fragmented ClientHello, before and after the cookie
   // exchange, whereas browsers fragment theirs once it carries a post-quantum key share.

   Bytes hello;                // Message body, excluding the handshake header
   std::vector<bool> received; // Body bytes present
   size_t hello_received = 0;
   int hello_sequence = -1;    // Handshake message_seq of the ClientHello under reassembly

   Native() {
      SecInvalidateHandle(&credentials);
      SecInvalidateHandle(&context);
   }

   ~Native() {
      if (SecIsValidHandle(&context)) DeleteSecurityContext(&context);
      if (SecIsValidHandle(&credentials)) FreeCredentialsHandle(&credentials);
   }

   SECURITY_STATUS handshake(DtlsRole Role, int Mtu, std::span<const SrtpProfile> Profiles,
      Bytes &Input, std::vector<Bytes> &Flight, Bytes &Extra) {
      // SRTP identifiers are network order, whereas the byte length and the MTU are native order.

      std::vector<uint16_t> profiles(1, uint16_t(Profiles.size() * sizeof(uint16_t)));
      for (auto profile : Profiles) {
         auto id = uint16_t(profile);
         profiles.push_back(uint16_t((id >> 8) | (id << 8)));
      }

      SECURITY_STATUS status;
      bool continuation = false;
      do {
         SEC_DTLS_MTU mtu_value{ USHORT(Mtu) };
         std::vector<SecBuffer> input = { { sizeof(mtu_value), SECBUFFER_DTLS_MTU, &mtu_value } };
         if (not Profiles.empty()) {
            input.push_back({ ULONG(profiles.size() * sizeof(uint16_t)), SECBUFFER_SRTP_PROTECTION_PROFILES,
               profiles.data() });
         }

         if (not Input.empty() and not continuation) {
            input.push_back({ ULONG(Input.size()), SECBUFFER_TOKEN, Input.data() });
            input.push_back({ 0, SECBUFFER_EMPTY, nullptr });
            input.push_back({ 0, SECBUFFER_EXTRA, nullptr });
         }

         SecBufferDesc input_desc{ SECBUFFER_VERSION, ULONG(input.size()), input.data() };
         SecBuffer output{ 0, SECBUFFER_TOKEN, nullptr };
         SecBufferDesc output_desc{ SECBUFFER_VERSION, 1, &output };
         ULONG attributes = 0;
         TimeStamp expiry;
         auto previous = SecIsValidHandle(&context) ? &context : nullptr;

         if (Role IS DtlsRole::SERVER) {
            ULONG flags = ASC_REQ_DATAGRAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_INTEGRITY | ASC_REQ_MUTUAL_AUTH |
               ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY;
            status = AcceptSecurityContext(&credentials, previous, &input_desc, flags, SECURITY_NATIVE_DREP,
               &context, &output_desc, &attributes, &expiry);
         }
         else {
            ULONG flags = ISC_REQ_DATAGRAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY | ISC_REQ_USE_SUPPLIED_CREDS |
               ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY;
            status = InitializeSecurityContextW(&credentials, previous, nullptr, flags, 0, SECURITY_NATIVE_DREP,
               &input_desc, 0, &context, &output_desc, &attributes, &expiry);
         }

         for (auto &buffer : input) {
            if (buffer.BufferType IS SECBUFFER_EXTRA and buffer.cbBuffer and buffer.cbBuffer < Input.size()) {
               Extra.assign(Input.end() - buffer.cbBuffer, Input.end());
            }
         }

         if (output.pvBuffer) {
            if ((status IS SEC_E_OK or status IS SEC_I_CONTINUE_NEEDED or status IS SEC_I_MESSAGE_FRAGMENT) and
                output.cbBuffer) {
               // Pack complete Schannel records into a datagram, preserving the configured MTU.  Without this,
               // deterministic packet-loss patterns can drop the same handshake message on every retry.

               auto data = (const uint8_t *)output.pvBuffer;
               if (not Flight.empty() and Flight.back().size() + output.cbBuffer <= size_t(Mtu)) {
                  Flight.back().insert(Flight.back().end(), data, data + output.cbBuffer);
               }
               else Flight.emplace_back(data, data + output.cbBuffer);
            }
            FreeContextBuffer(output.pvBuffer);
         }

         continuation = true;
      } while (status IS SEC_I_MESSAGE_FRAGMENT);

      return status;
   }

   // Replaces ClientHello fragments in Datagram with one unfragmented record once the whole message has arrived.
   // RFC 6347 lets a record carry several handshake fragments, so every fragment in each epoch-0 handshake record is
   // parsed.  DTLS hashes the reassembled message for Finished, so the handshake transcript is unchanged.  Records
   // holding only partial fragments are removed from Datagram, and every other record passes through unchanged.

   void assemble_hello(Bytes &Datagram) {
      static constexpr size_t RECORD_HEADER = 13;
      static constexpr size_t HANDSHAKE_HEADER = 12;
      static constexpr size_t MAX_BODY = 16384 - HANDSHAKE_HEADER; // Largest body that fits one DTLS record

      struct Fragment {
         const uint8_t *header;
         size_t total, offset, size;
      };

      auto read24 = [](const uint8_t *Data) { return (size_t(Data[0]) << 16) | (size_t(Data[1]) << 8) | Data[2]; };

      Bytes output;
      std::vector<Fragment> fragments;
      size_t position = 0;
      while (position + RECORD_HEADER <= Datagram.size()) {
         auto record = Datagram.data() + position;
         size_t length = (size_t(record[11]) << 8) | record[12];
         if (length + RECORD_HEADER > Datagram.size() - position) break; // Truncated datagram
         auto next = position + RECORD_HEADER + length;

         // Only records made entirely of well-formed ClientHello fragments are reassembled.

         fragments.clear();
         bool epoch_zero = not record[3] and not record[4];
         bool hello_only = record[0] IS 22 and epoch_zero and length > 0;
         for (size_t at = 0; hello_only and at < length; ) {
            auto header = record + RECORD_HEADER + at;
            if (length - at < HANDSHAKE_HEADER or header[0] != 1) {
               hello_only = false;
               break;
            }

            Fragment fragment = { header, read24(header + 1), read24(header + 6), read24(header + 9) };
            if ((fragment.size > length - at - HANDSHAKE_HEADER) or (fragment.total IS 0) or
                (fragment.total > MAX_BODY) or (fragment.offset + fragment.size > fragment.total)) {
               hello_only = false;
               break;
            }

            fragments.push_back(fragment);
            at += HANDSHAKE_HEADER + fragment.size;
         }

         // Schannel accepts a whole ClientHello in its own record, so that record is passed through untouched.

         bool whole = fragments.size() IS 1 and fragments[0].offset IS 0 and fragments[0].size IS fragments[0].total;
         if ((not hello_only) or whole) {
            output.insert(output.end(), record, Datagram.data() + next);
            position = next;
            continue;
         }

         for (auto &fragment : fragments) {
            int sequence = (int(fragment.header[4]) << 8) | fragment.header[5];
            if (sequence != hello_sequence or hello.size() != fragment.total) {
               hello.assign(fragment.total, 0);
               received.assign(fragment.total, false);
               hello_received = 0;
               hello_sequence = sequence;
            }

            std::memcpy(hello.data() + fragment.offset, fragment.header + HANDSHAKE_HEADER, fragment.size);
            for (size_t i = fragment.offset; i < fragment.offset + fragment.size; i++) {
               if (not received[i]) {
                  received[i] = true;
                  hello_received++;
               }
            }

            if (hello_received IS fragment.total) {
               // The completing record supplies the version, epoch and sequence number, so record sequences still
               // advance.  The handshake header keeps msg_type, length and message_seq, with one fragment spanning
               // the message.

               auto record_length = fragment.total + HANDSHAKE_HEADER;
               output.insert(output.end(), record, record + 11);
               output.push_back(uint8_t(record_length >> 8));
               output.push_back(uint8_t(record_length));
               output.insert(output.end(), fragment.header, fragment.header + 6);
               const uint8_t span[6] = { 0, 0, 0, fragment.header[1], fragment.header[2], fragment.header[3] };
               output.insert(output.end(), span, span + 6);
               output.insert(output.end(), hello.begin(), hello.end());

               hello.clear();
               received.clear();
               hello_received = 0;
               hello_sequence = -1;
            }
         }

         position = next;
      }

      if (position < Datagram.size()) output.insert(output.end(), Datagram.begin() + position, Datagram.end());
      Datagram = std::move(output);
   }

   SECURITY_STATUS encrypt(std::span<const uint8_t> Plain, ULONG Qop, Bytes &Output) {
      Bytes storage(size_t(sizes.cbHeader) + Plain.size() + sizes.cbTrailer);
      std::memcpy(storage.data() + sizes.cbHeader, Plain.data(), Plain.size());

      SecBuffer buffers[4] = {
         { sizes.cbHeader, SECBUFFER_STREAM_HEADER, storage.data() },
         { ULONG(Plain.size()), SECBUFFER_DATA, storage.data() + sizes.cbHeader },
         { sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, storage.data() + sizes.cbHeader + Plain.size() },
         { 0, SECBUFFER_EMPTY, nullptr }
      };
      SecBufferDesc desc{ SECBUFFER_VERSION, DWORD(std::size(buffers)), buffers };

      auto status = EncryptMessage(&context, Qop, &desc, 0);
      if (status IS SEC_E_OK) {
         for (auto &buffer : buffers) {
            if (buffer.cbBuffer) {
               auto data = (const uint8_t *)buffer.pvBuffer;
               Output.insert(Output.end(), data, data + buffer.cbBuffer);
            }
         }
      }
      return status;
   }
};

//********************************************************************************************************************

DtlsCertificate::DtlsCertificate() = default;
DtlsCertificate::~DtlsCertificate() = default;

ERR DtlsCertificate::generate(std::shared_ptr<DtlsCertificate> &Result, std::string_view CommonName)
{
   auto certificate = std::make_shared<DtlsCertificate>();
   certificate->native_data = std::make_unique<Native>();

   auto &native = certificate->native_data->certificate;
   if (not crypto_protocol::generate_p256_certificate_cng(CommonName, false, native, true)) return ERR::Failed;

   certificate->fingerprint_text = dtls::format_fingerprint(native.fingerprint);
   Result = std::move(certificate);
   return ERR::Okay;
}

//********************************************************************************************************************

DtlsTransport::DtlsTransport(std::shared_ptr<DtlsCertificate> Certificate, DtlsRole Role, DtlsCallbacks Callbacks) :
   certificate(std::move(Certificate)), callbacks(std::move(Callbacks)), local_role(Role)
{
   srtp_profiles = { SrtpProfile::AEAD_AES_128_GCM, SrtpProfile::AES128_CM_SHA1_80 };
}

DtlsTransport::~DtlsTransport() = default;

ERR DtlsTransport::start()
{
   if (current_state != DtlsState::NEW) return ERR::InvalidState;
   if (not certificate or not certificate->native()) return ERR::NullArgs;
   if (mtu < 256 or mtu > 65535 or initial_timeout_ms <= 0 or initial_timeout_ms > MAX_TIMEOUT_MS) {
      return ERR::OutOfRange;
   }
   if (srtp_profiles.size() > 32767) return ERR::OutOfRange;

   native_data = std::make_unique<Native>();
   auto status = acquire_credentials(native_data->credentials, certificate->native()->certificate.context, local_role);
   if (status != SEC_E_OK) return fail(schannel_error("DTLS credentials", status));

   set_state(DtlsState::CONNECTING);

   if (local_role IS DtlsRole::CLIENT) return drive();
   return ERR::Okay;
}

ERR DtlsTransport::drive()
{
   auto &native = *native_data;
   do {
      Bytes datagram;
      if (not inbound.empty()) {
         datagram = std::move(inbound.front());
         inbound.pop_front();
      }

      if (not handshake_done) {
         // An empty datagram is a retransmission request, so a datagram holding only buffered fragments is not
         // passed on.

         if (local_role IS DtlsRole::SERVER and not datagram.empty()) {
            native.assemble_hello(datagram);
            if (datagram.empty()) continue;
         }

         std::vector<Bytes> flight;
         Bytes extra;
         auto status = native.handshake(local_role, mtu, srtp_profiles, datagram, flight, extra);
         if (status != SEC_E_OK and status != SEC_I_CONTINUE_NEEDED and status != SEC_E_INCOMPLETE_MESSAGE) {
            native.timer_armed = false;
            return fail(schannel_error("DTLS handshake", status));
         }

         if (status IS SEC_E_OK) {
            PCCERT_CONTEXT peer = nullptr;
            status = QueryContextAttributesW(&native.context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer);
            if (status != SEC_E_OK or not peer) return fail(schannel_error("DTLS peer certificate", status));

            std::array<uint8_t, 32> digest;
            bool hashed = crypto_protocol::sha256({ peer->pbCertEncoded, peer->cbCertEncoded }, digest);
            CertFreeCertificateContext(peer);
            if (not hashed) return fail("DTLS peer certificate hash failed.");

            // Withhold the server's Finished on rejection, so the client cannot reach CONNECTED.

            if (not check_peer_fingerprint(dtls::format_fingerprint(digest))) return fail(last_error);

            status = QueryContextAttributesW(&native.context, SECPKG_ATTR_STREAM_SIZES, &native.sizes);
            if (status != SEC_E_OK) return fail(schannel_error("DTLS record sizes", status));

            SecPkgContext_CipherInfo cipher{};
            cipher.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
            status = QueryContextAttributesW(&native.context, SECPKG_ATTR_CIPHER_INFO, &cipher);
            if (status != SEC_E_OK) return fail(schannel_error("DTLS cipher suite", status));
            if (cipher.dwCipherSuite != 0xc02b and cipher.dwCipherSuite != 0xc02c) {
               return fail("DTLS requires ECDHE-ECDSA with AES-GCM.");
            }

            native.timer_armed = false;
            if (local_role IS DtlsRole::SERVER) native.final_flight = flight;

            // Publish the final flight before reporting CONNECTED, as the callback can immediately send SCTP.

            for (auto &packet : flight) {
               if (packet.size() > size_t(mtu)) return fail("DTLS handshake datagram exceeds the MTU.");
               emit_datagram(packet);
            }

            if (auto error = complete_handshake(); error != ERR::Okay) return error;
         }
         else if (not flight.empty()) {
            for (auto &packet : flight) {
               if (packet.size() > size_t(mtu)) return fail("DTLS handshake datagram exceeds the MTU.");
               emit_datagram(packet);
            }

            native.timeout_interval = next_timeout(native.retransmitting ? native.timeout_interval : 0);
            native.deadline = Clock::now() + std::chrono::microseconds(native.timeout_interval);
            native.timer_armed = true;
         }

         if (not extra.empty()) inbound.push_front(std::move(extra));
      }
      else {
         // Validate and split records before decrypting.  This also handles a coalesced retransmitted flight followed
         // by application data without handing late CCS/handshake records to Schannel, which destroys the context.

         size_t position = 0;
         bool replay = false;
         while (position + 13 <= datagram.size()) {
            size_t length = (size_t(datagram[position + 11]) << 8) | datagram[position + 12];
            if (length + 13 > datagram.size() - position) break; // Truncated datagram

            auto type = datagram[position];
            bool epoch_zero = not datagram[position + 3] and not datagram[position + 4];
            if (type IS 22 or type IS 20) {
               replay |= type IS 22 and epoch_zero;
               position += length + 13;
               continue;
            }

            if (epoch_zero or (type != 21 and type != 23)) {
               position += length + 13;
               continue;
            }

            Bytes record(datagram.begin() + position, datagram.begin() + position + length + 13);
            position += length + 13;

            SecBuffer buffers[4] = {
               { ULONG(record.size()), SECBUFFER_DATA, record.data() },
               { 0, SECBUFFER_EMPTY, nullptr },
               { 0, SECBUFFER_EMPTY, nullptr },
               { 0, SECBUFFER_EMPTY, nullptr }
            };
            SecBufferDesc desc{ SECBUFFER_VERSION, DWORD(std::size(buffers)), buffers };

            auto status = DecryptMessage(&native.context, &desc, 0, nullptr);
            if (status IS SEC_I_CONTEXT_EXPIRED) {
               inbound.clear();
               set_state(DtlsState::CLOSED);
               return ERR::Okay;
            }

            if (status IS SEC_E_MESSAGE_ALTERED or status IS SEC_E_DECRYPT_FAILURE or status IS SEC_E_OUT_OF_SEQUENCE or
                status IS SEC_E_INVALID_TOKEN or status IS SEC_E_INCOMPLETE_MESSAGE) continue;
            if (status != SEC_E_OK) return fail(schannel_error("DTLS read", status));

            if (type IS 23 and current_state IS DtlsState::CONNECTED and callbacks.receive) {
               for (auto &buffer : buffers) {
                  if (buffer.BufferType IS SECBUFFER_DATA and buffer.cbBuffer) {
                     callbacks.receive({ (const uint8_t *)buffer.pvBuffer, buffer.cbBuffer });
                  }
               }
            }
         }

         if (replay and not native.final_flight.empty() and
             Clock::now() - native.last_replay >= std::chrono::milliseconds(10)) {
            native.last_replay = Clock::now();
            for (auto &packet : native.final_flight) emit_datagram(packet);
         }
      }
   } while (not inbound.empty());

   return ERR::Okay;
}

ERR DtlsTransport::send(std::span<const uint8_t> Data)
{
   if (current_state != DtlsState::CONNECTED) return ERR::InvalidState;
   if (Data.empty()) return ERR::Okay;
   if (Data.size() > size_t(max_payload())) return ERR::DataSize;

   Bytes output;
   auto status = native_data->encrypt(Data, 0, output);
   if (status != SEC_E_OK) return fail(schannel_error("DTLS write", status));
   if (output.size() > size_t(mtu)) return fail("DTLS application datagram exceeds the MTU.");

   emit_datagram(output);
   return ERR::Okay;
}

ERR DtlsTransport::close()
{
   if (current_state != DtlsState::CONNECTING and current_state != DtlsState::CONNECTED) return ERR::Okay;

   if (handshake_done) {
      // SCHANNEL_SHUTDOWN + ISC/ASC is broken for DTLS; wrap the alert directly instead.

      static constexpr std::array<uint8_t, 2> CLOSE_NOTIFY = { TLS1_ALERT_WARNING, TLS1_ALERT_CLOSE_NOTIFY };
      Bytes output;
      auto status = native_data->encrypt(CLOSE_NOTIFY, SECQOP_WRAP_OOB_DATA, output);
      if (status != SEC_E_OK) return fail(schannel_error("DTLS close_notify", status));
      emit_datagram(output);
   }

   native_data->timer_armed = false;
   inbound.clear();
   set_state(DtlsState::CLOSED);
   return ERR::Okay;
}

int64_t DtlsTransport::timeout_us() const
{
   if (not native_data or not native_data->timer_armed or handshake_done or
       current_state != DtlsState::CONNECTING) return -1;

   return std::max<int64_t>(0,
      std::chrono::duration_cast<std::chrono::microseconds>(native_data->deadline - Clock::now()).count());
}

ERR DtlsTransport::handle_timeout()
{
   auto remaining = timeout_us();
   if (remaining != 0) return ERR::Okay;

   native_data->retransmitting = true;
   native_data->timer_armed = false;
   auto error = drive();
   native_data->retransmitting = false;
   return error;
}

int DtlsTransport::max_payload() const
{
   if (not native_data or not handshake_done) return 0;

   auto &sizes = native_data->sizes;
   return std::min(int(sizes.cbMaximumMessage), std::max(0, mtu - int(sizes.cbHeader) - int(sizes.cbTrailer)));
}

ERR DtlsTransport::srtp_keys(SrtpKeyMaterial &Keys) const
{
   if (current_state != DtlsState::CONNECTED) return ERR::InvalidState;

   auto &context = native_data->context;
   SecPkgContext_SrtpParameters parameters{};
   auto status = QueryContextAttributesW(&context, SECPKG_ATTR_SRTP_PARAMETERS, &parameters);
   if (status != SEC_E_OK) return ERR::NoSupport;

   auto id = parameters.ProtectionProfile;
   auto profile = SrtpProfile(uint16_t((id >> 8) | (id << 8)));
   if (parameters.MasterKeyIdentifier) FreeContextBuffer(parameters.MasterKeyIdentifier);

   size_t key_length, salt_length;
   if (not dtls::srtp_lengths(profile, key_length, salt_length)) return ERR::NoSupport;

   char label[] = "EXTRACTOR-dtls_srtp";
   SecPkgContext_KeyingMaterialInfo info{};
   info.cbLabel = WORD(sizeof(label)); // Schannel includes the NUL in this count, then removes it before the PRF
   info.pszLabel = label;
   info.cbKeyingMaterial = DWORD(2 * (key_length + salt_length));
   status = SetContextAttributesW(&context, SECPKG_ATTR_KEYING_MATERIAL_INFO, &info, sizeof(info));
   if (status != SEC_E_OK) return ERR::Failed;

   SecPkgContext_KeyingMaterial material{};
   status = QueryContextAttributesW(&context, SECPKG_ATTR_KEYING_MATERIAL, &material);
   if (status != SEC_E_OK) return ERR::Failed;

   bool split = material.pbKeyingMaterial and dtls::split_srtp_material(profile, local_role,
      { material.pbKeyingMaterial, material.cbKeyingMaterial }, Keys);
   if (material.pbKeyingMaterial) {
      crypto_protocol::secure_wipe(material.pbKeyingMaterial, material.cbKeyingMaterial);
      FreeContextBuffer(material.pbKeyingMaterial);
   }
   return split ? ERR::Okay : ERR::Failed;
}

} // namespace rtc
