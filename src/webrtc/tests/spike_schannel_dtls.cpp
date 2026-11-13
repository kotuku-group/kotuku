/*********************************************************************************************************************

Phase 0 spike: Schannel DTLS 1.2 for WebRTC (docs/plans/net/1_webrtc_windows_native_dtls.md).

Not shipped and not registered with ctest.  A client and a server Schannel context run in-process over a simulated
datagram link (loss, duplication, reordering, MTU checks), in the same style as test_dtls.cpp.  Every scenario prints
the evidence it collects so that the plan's Phase 0 questions can be answered from the output.

Build from a VS x64 developer prompt:

  cl /nologo /std:c++20 /EHsc /W3 /Zi /Fe:spike_schannel_dtls.exe spike_schannel_dtls.cpp
     secur32.lib crypt32.lib ncrypt.lib bcrypt.lib ws2_32.lib

Usage:

  spike_schannel_dtls [--named-key] [scenario ...]        In-process scenarios (none named = all)
  spike_schannel_dtls --named-key --udp-client PORT       Interop with e.g. `openssl s_server -dtls1_2` on 127.0.0.1
  spike_schannel_dtls --named-key --udp-server PORT       Interop with e.g. `openssl s_client -dtls1_2`

Without --named-key the certificates use an unnamed NCrypt key, which Schannel rejects (SEC_E_NO_CREDENTIALS).

With --named-key, two scenarios fail by design because they record limits rather than test the chosen design:
mtu_attr (the server cannot set SECPKG_ATTR_DTLS_MTU) and reject_client (the client sees the server certificate only
after its own final flight).

close_notify is sent with EncryptMessage(SECQOP_WRAP_OOB_DATA) over the two-byte alert; the SCHANNEL_SHUTDOWN and
SCHANNEL_ALERT control-token routes do not work for DTLS and remain reproducible with SPIKE_CLOSE_MODE=0..3.  For the
UDP modes, SPIKE_UDP_CLOSE_MS=N sends close_notify N ms after the handshake.

SPIKE_* environment variables switch individual behaviours back to the variants that were rejected, so each finding
can be reproduced; see env_int()/env_hex() call sites.  The defaults are the design recommended for Phase 2.  Results
are recorded in the plan's Phase 0 findings.

*********************************************************************************************************************/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <subauth.h>
#include <security.h>
#include <schannel.h>
#include <ncrypt.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define IS ==

namespace {

int glFailures = 0;

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// Experiment switches, read from the environment so that variants can be tried without rebuilding.

int env_int(const char *Name, int Default)
{
   char value[32];
   auto len = GetEnvironmentVariableA(Name, value, sizeof(value));
   if (not len or len >= sizeof(value)) return Default;
   return std::atoi(value);
}

DWORD env_hex(const char *Name, DWORD Default)
{
   char value[32];
   auto len = GetEnvironmentVariableA(Name, value, sizeof(value));
   if (not len or len >= sizeof(value)) return Default;
   return DWORD(std::strtoul(value, nullptr, 16));
}

#define CHECK(Condition, Message) \
   if (not (Condition)) { std::printf("  FAIL line %d: %s\n", __LINE__, Message); glFailures++; return; }

const char * status_name(SECURITY_STATUS Status)
{
   switch (Status) {
      case SEC_E_OK: return "SEC_E_OK";
      case SEC_I_CONTINUE_NEEDED: return "SEC_I_CONTINUE_NEEDED";
      case SEC_I_MESSAGE_FRAGMENT: return "SEC_I_MESSAGE_FRAGMENT";
      case SEC_E_INCOMPLETE_MESSAGE: return "SEC_E_INCOMPLETE_MESSAGE";
      case SEC_I_INCOMPLETE_CREDENTIALS: return "SEC_I_INCOMPLETE_CREDENTIALS";
      case SEC_I_CONTEXT_EXPIRED: return "SEC_I_CONTEXT_EXPIRED";
      case SEC_I_RENEGOTIATE: return "SEC_I_RENEGOTIATE";
      case SEC_E_MESSAGE_ALTERED: return "SEC_E_MESSAGE_ALTERED";
      case SEC_E_OUT_OF_SEQUENCE: return "SEC_E_OUT_OF_SEQUENCE";
      case SEC_E_DECRYPT_FAILURE: return "SEC_E_DECRYPT_FAILURE";
      case SEC_E_INVALID_TOKEN: return "SEC_E_INVALID_TOKEN";
      case SEC_E_ILLEGAL_MESSAGE: return "SEC_E_ILLEGAL_MESSAGE";
      case SEC_E_NO_CREDENTIALS: return "SEC_E_NO_CREDENTIALS";
      case SEC_E_UNKNOWN_CREDENTIALS: return "SEC_E_UNKNOWN_CREDENTIALS";
      case SEC_E_INTERNAL_ERROR: return "SEC_E_INTERNAL_ERROR";
      case SEC_E_ALGORITHM_MISMATCH: return "SEC_E_ALGORITHM_MISMATCH";
      case SEC_E_UNSUPPORTED_FUNCTION: return "SEC_E_UNSUPPORTED_FUNCTION";
      case SEC_E_CERT_UNKNOWN: return "SEC_E_CERT_UNKNOWN";
      case SEC_E_UNTRUSTED_ROOT: return "SEC_E_UNTRUSTED_ROOT";
      case SEC_E_INVALID_HANDLE: return "SEC_E_INVALID_HANDLE";
      case SEC_E_INVALID_PARAMETER: return "SEC_E_INVALID_PARAMETER";
      case SEC_E_BUFFER_TOO_SMALL: return "SEC_E_BUFFER_TOO_SMALL";
      case SEC_E_CONTEXT_EXPIRED: return "SEC_E_CONTEXT_EXPIRED";
      default: break;
   }
   static char buffer[32];
   std::snprintf(buffer, sizeof(buffer), "0x%08lx", (unsigned long)Status);
   return buffer;
}

std::string hex(const uint8_t *Data, size_t Size, char Separator = 0)
{
   static constexpr char HEX[] = "0123456789ABCDEF";
   std::string out;
   for (size_t i = 0; i < Size; i++) {
      if (Separator and i) out += Separator;
      out += HEX[Data[i] >> 4];
      out += HEX[Data[i] & 15];
   }
   return out;
}

// Summarise a DTLS datagram: each record's content type, and for plaintext handshake records the message type.

std::string describe(const Bytes &Datagram)
{
   std::string out;
   size_t pos = 0;
   int records = 0;
   while (pos + 13 <= Datagram.size() and records++ < 16) {
      uint8_t type = Datagram[pos];
      uint16_t epoch = uint16_t((Datagram[pos + 3] << 8) | Datagram[pos + 4]);
      size_t length = size_t((Datagram[pos + 11] << 8) | Datagram[pos + 12]);
      if (not out.empty()) out += ' ';
      if (length > Datagram.size() - pos - 13) { // The declared payload is not all present
         out += "truncated(" + std::to_string(type) + ")";
         break;
      }
      if (type IS 22 and epoch IS 0 and length >= 12) {
         static const std::map<int, const char *> names = { { 1, "CH" }, { 2, "SH" }, { 3, "HVR" }, { 11, "Cert" },
            { 12, "SKE" }, { 13, "CertReq" }, { 14, "SHD" }, { 15, "CV" }, { 16, "CKE" }, { 20, "Fin" } };
         int msg = Datagram[pos + 13];
         uint32_t frag_off = (Datagram[pos + 19] << 16) | (Datagram[pos + 20] << 8) | Datagram[pos + 21];
         uint32_t frag_len = (Datagram[pos + 22] << 16) | (Datagram[pos + 23] << 8) | Datagram[pos + 24];
         uint32_t msg_len = (Datagram[pos + 14] << 16) | (Datagram[pos + 15] << 8) | Datagram[pos + 16];
         auto it = names.find(msg);
         out += it != names.end() ? it->second : ("HS" + std::to_string(msg));
         if (frag_len != msg_len) out += "[" + std::to_string(frag_off) + "+" + std::to_string(frag_len) + "]";
      }
      else if (type IS 20) out += "CCS";
      else if (type IS 21) out += epoch ? "Alert*" : "Alert";
      else if (type IS 22) out += "HS*";
      else if (type IS 23) out += "App";
      else out += "?" + std::to_string(type);
      pos += 13 + length;
   }
   if (pos != Datagram.size()) out += " (trailing " + std::to_string(Datagram.size() - pos) + ")";
   return out;
}

//********************************************************************************************************************
// Certificates

enum class KeyMode { EPHEMERAL_HANDLE, NAMED_PERSISTED };

struct Certificate {
   PCCERT_CONTEXT context = nullptr;
   NCRYPT_PROV_HANDLE provider = 0;
   NCRYPT_KEY_HANDLE key = 0;
   std::wstring key_name;
   std::string fingerprint;

   ~Certificate() {
      if (context) CertFreeCertificateContext(context);
      if (key) {
         if (not key_name.empty()) NCryptDeleteKey(key, 0); // Also frees the handle
         else NCryptFreeObject(key);
      }
      if (provider) NCryptFreeObject(provider);
   }
};

std::string fingerprint_of(PCCERT_CONTEXT Context)
{
   std::array<uint8_t, 32> digest{};
   DWORD size = DWORD(digest.size());
   if (not CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, Context->pbCertEncoded,
       Context->cbCertEncoded, digest.data(), &size)) return {};
   return hex(digest.data(), size, ':');
}

SYSTEMTIME certificate_time(int Days)
{
   FILETIME time;
   GetSystemTimeAsFileTime(&time);
   ULARGE_INTEGER ticks;
   ticks.LowPart = time.dwLowDateTime;
   ticks.HighPart = time.dwHighDateTime;
   ticks.QuadPart += int64_t(Days) * 86400 * 10000000;
   time = { ticks.LowPart, ticks.HighPart };
   SYSTEMTIME result{};
   FileTimeToSystemTime(&time, &result);
   return result;
}

bool make_certificate(Certificate &Cert, const wchar_t *CommonName, KeyMode Mode)
{
   std::wstring x500 = std::wstring(L"CN=") + CommonName;
   DWORD subject_size = 0;
   if (not CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &subject_size,
       nullptr)) return false;
   Bytes subject_bytes(subject_size);
   if (not CertStrToNameW(X509_ASN_ENCODING, x500.c_str(), CERT_X500_NAME_STR, nullptr, subject_bytes.data(),
       &subject_size, nullptr)) return false;
   CERT_NAME_BLOB subject = { subject_size, subject_bytes.data() };

   if (NCryptOpenStorageProvider(&Cert.provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;

   if (Mode IS KeyMode::NAMED_PERSISTED) {
      uint8_t random[8];
      BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      auto suffix = hex(random, sizeof(random));
      Cert.key_name = L"kotuku-webrtc-spike-" + std::wstring(suffix.begin(), suffix.end());
   }

   auto status = NCryptCreatePersistedKey(Cert.provider, &Cert.key, NCRYPT_ECDSA_P256_ALGORITHM,
      Cert.key_name.empty() ? nullptr : Cert.key_name.c_str(), 0, NCRYPT_SILENT_FLAG);
   if (status != ERROR_SUCCESS) { std::printf("  NCryptCreatePersistedKey 0x%08lx\n", status); return false; }
   status = NCryptFinalizeKey(Cert.key, NCRYPT_SILENT_FLAG);
   if (status != ERROR_SUCCESS) { std::printf("  NCryptFinalizeKey 0x%08lx\n", status); return false; }

   CRYPT_ALGORITHM_IDENTIFIER signature{};
   signature.pszObjId = (LPSTR)szOID_ECDSA_SHA256;
   auto start = certificate_time(-1), end = certificate_time(30);

   CRYPT_KEY_PROV_INFO prov_info{};
   prov_info.pwszContainerName = (LPWSTR)Cert.key_name.c_str();
   prov_info.pwszProvName = (LPWSTR)MS_KEY_STORAGE_PROVIDER;
   prov_info.dwProvType = 0;
   prov_info.dwFlags = NCRYPT_SILENT_FLAG;
   prov_info.dwKeySpec = 0;

   Cert.context = CertCreateSelfSignCertificate(Cert.key, &subject,
      Mode IS KeyMode::EPHEMERAL_HANDLE ? CERT_CREATE_SELFSIGN_NO_KEY_INFO : 0,
      Mode IS KeyMode::NAMED_PERSISTED ? &prov_info : nullptr, &signature, &start, &end, nullptr);
   if (not Cert.context) { std::printf("  CertCreateSelfSignCertificate 0x%08lx\n", GetLastError()); return false; }

   if (Mode IS KeyMode::EPHEMERAL_HANDLE) {
      // Attach the in-process key handle.  The certificate context does not take ownership.
      if (not CertSetCertificateContextProperty(Cert.context, CERT_NCRYPT_KEY_HANDLE_PROP_ID,
          CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &Cert.key)) {
         std::printf("  CERT_NCRYPT_KEY_HANDLE_PROP_ID 0x%08lx\n", GetLastError());
         return false;
      }
   }

   Cert.fingerprint = fingerprint_of(Cert.context);
   return true;
}

//********************************************************************************************************************
// Credentials

struct Options {
   KeyMode key_mode = KeyMode::EPHEMERAL_HANDLE;
   int mtu = 1200;
   std::vector<uint16_t> client_srtp = { 0x0007, 0x0001 }; // AEAD_AES_128_GCM, AES128_CM_SHA1_80
   std::vector<uint16_t> server_srtp = { 0x0007, 0x0001 };
   bool srtp_in_first_call_only = true;
   bool mtu_via_buffer = true;       // SECBUFFER_DTLS_MTU in the first call, else SetContextAttributes
   bool set_client_address = true;   // SecAllocateAndSetIPAddress before every ASC call
   bool restrict_suites = false;     // Disable everything but ECDHE/ECDSA/AES-GCM through CRYPTO_SETTINGS
   bool server_rejects_client = false; // Server withholds its final flight on a fingerprint mismatch
   bool client_rejects_server = false;
   bool export_keys = true;
   std::function<bool(char, size_t)> drop;
   std::function<bool(char, size_t)> duplicate;
   bool reverse_flights = false;
   int initial_timeout_ms = 50;
   bool verbose = true;
   bool test_close = false;
};

bool acquire_credentials(CredHandle &Handle, Certificate &Cert, bool Server, const Options &Opt)
{
   PCCERT_CONTEXT certs[1] = { Cert.context };
   UNICODE_STRING disabled_algs[3];
   CRYPTO_SETTINGS crypto[3]{};
   TLS_PARAMETERS tls{};
   tls.grbitDisabledProtocols = DWORD(~(Server ? SP_PROT_DTLS1_2_SERVER : SP_PROT_DTLS1_2_CLIENT));

   if (Opt.restrict_suites) {
      static wchar_t rsa[] = L"RSA", dh[] = L"DH", cbc_aes[] = L"3DES";
      auto set = [](UNICODE_STRING &S, wchar_t *Text) {
         S.Buffer = Text;
         S.Length = USHORT(wcslen(Text) * sizeof(wchar_t));
         S.MaximumLength = S.Length;
      };
      set(disabled_algs[0], rsa);
      set(disabled_algs[1], dh);
      set(disabled_algs[2], cbc_aes);
      crypto[0].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
      crypto[0].strCngAlgId = disabled_algs[0];
      crypto[1].eAlgorithmUsage = TlsParametersCngAlgUsageKeyExchange;
      crypto[1].strCngAlgId = disabled_algs[1];
      crypto[2].eAlgorithmUsage = TlsParametersCngAlgUsageCipher;
      crypto[2].strCngAlgId = disabled_algs[2];
      tls.cDisabledCrypto = 3;
      tls.pDisabledCrypto = crypto;
   }

   SCH_CREDENTIALS cred{};
   cred.dwVersion = SCH_CREDENTIALS_VERSION;
   cred.cCreds = 1;
   cred.paCred = certs;
   cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_USE_DTLS_ONLY;
   if (not Server) cred.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
   else cred.dwFlags |= SCH_CRED_NO_SYSTEM_MAPPER;
   cred.cTlsParameters = 1;
   cred.pTlsParameters = &tls;

   // Overrides: SPIKE_FLAGS_SERVER / SPIKE_FLAGS_CLIENT replace dwFlags, SPIKE_DISABLED replaces the disabled mask
   // (0 = no TLS_PARAMETERS), SPIKE_LEGACY=1 uses SCHANNEL_CRED with grbitEnabledProtocols instead.

   cred.dwFlags = env_hex(Server ? "SPIKE_FLAGS_SERVER" : "SPIKE_FLAGS_CLIENT", cred.dwFlags);
   tls.grbitDisabledProtocols = env_hex("SPIKE_DISABLED", tls.grbitDisabledProtocols);
   if (not tls.grbitDisabledProtocols and not tls.cDisabledCrypto) {
      cred.cTlsParameters = 0;
      cred.pTlsParameters = nullptr;
   }

   SCHANNEL_CRED legacy{};
   legacy.dwVersion = SCHANNEL_CRED_VERSION;
   legacy.cCreds = 1;
   legacy.paCred = certs;
   legacy.grbitEnabledProtocols = Server ? SP_PROT_DTLS1_2_SERVER : SP_PROT_DTLS1_2_CLIENT;
   legacy.dwFlags = cred.dwFlags;
   bool use_legacy = env_int("SPIKE_LEGACY", 0);

   TimeStamp expiry;
   auto status = AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W,
      Server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, use_legacy ? (void *)&legacy : (void *)&cred,
      nullptr, nullptr, &Handle, &expiry);
   if (status != SEC_E_OK) {
      std::printf("  AcquireCredentialsHandle(%s) %s\n", Server ? "server" : "client", status_name(status));
      return false;
   }
   return true;
}

//********************************************************************************************************************
// One side of the connection

struct Peer {
   const char *name;
   bool server;
   CredHandle cred{};
   CtxtHandle ctx{};
   bool has_ctx = false;
   bool handshake_done = false;
   bool failed = false;
   bool closed = false;
   bool reject_peer = false;
   std::string expected_fingerprint;
   std::string seen_fingerprint;
   std::string verification_point;
   std::deque<Bytes> outbox;
   std::vector<Bytes> received;
   std::vector<Bytes> last_flight;
   Clock::time_point timer_deadline;
   bool timer_armed = false;
   int timer_ms = 0;
   size_t calls = 0;
   size_t datagrams_sent = 0;
   size_t largest = 0;
   int retransmits = 0;
   std::vector<std::string> trace;
   const Options *opt = nullptr;
   SecPkgContext_StreamSizes sizes{};
   bool attribute_tried_before = false;

   ~Peer() {
      if (has_ctx) DeleteSecurityContext(&ctx);
      SecInvalidateHandle(&ctx);
      FreeCredentialsHandle(&cred);
   }

   void log(const std::string &Line) {
      trace.push_back(Line);
      if (env_int("SPIKE_VERBOSE", opt->verbose ? 1 : 0)) std::printf("    [%s] %s\n", name, Line.c_str());
   }

   void emit(const uint8_t *Data, size_t Size, std::vector<Bytes> &Flight) {
      Bytes datagram(Data, Data + Size);
      Flight.push_back(datagram);
   }

   void flush(std::vector<Bytes> &Flight, bool NewFlight) {
      if (Flight.empty()) return;
      if (env_int("SPIKE_COALESCE", 1)) {
         // SPIKE_COALESCE=1: pack consecutive Schannel output tokens into one datagram while they fit in the MTU.
         std::vector<Bytes> packed;
         for (auto &dg : Flight) {
            if (not packed.empty() and packed.back().size() + dg.size() <= size_t(opt->mtu)) {
               packed.back().insert(packed.back().end(), dg.begin(), dg.end());
            }
            else packed.push_back(dg);
         }
         Flight = std::move(packed);
      }
      for (auto &dg : Flight) {
         datagrams_sent++;
         largest = std::max(largest, dg.size());
         log("  -> " + std::to_string(dg.size()) + " bytes: " + describe(dg) +
            (handshake_done ? " [" + hex(dg.data(), std::min<size_t>(dg.size(), 32), 0) + "]" : std::string()));
         outbox.push_back(dg);
      }
      if (NewFlight) last_flight = Flight;
      if (not handshake_done) arm_timer(NewFlight);
   }

   void arm_timer(bool Reset) {
      timer_ms = Reset ? opt->initial_timeout_ms : std::min(timer_ms * 2, 6000);
      timer_deadline = Clock::now() + std::chrono::milliseconds(timer_ms);
      timer_armed = true;
   }

   // Query the peer certificate.  Returns true if available.

   bool probe_remote_cert(const char *Point) {
      if (not has_ctx or not seen_fingerprint.empty()) return not seen_fingerprint.empty();
      PCCERT_CONTEXT remote = nullptr;
      auto status = QueryContextAttributesW(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote);
      if (status IS SEC_E_OK and remote) {
         seen_fingerprint = fingerprint_of(remote);
         CertFreeCertificateContext(remote);
         verification_point = std::string(Point) + " (REMOTE_CERT_CONTEXT)";
         log("remote certificate available " + verification_point);
         return true;
      }

      // SECPKG_ATTR_SERIALIZED_REMOTE_CERT_CONTEXT is documented as usable during the handshake loop.  The blob is a
      // serialized certificate store element.
      CERT_BLOB blob{};
      auto serial = QueryContextAttributesW(&ctx, SECPKG_ATTR_SERIALIZED_REMOTE_CERT_CONTEXT, &blob);
      if (serial IS SEC_E_OK and blob.pbData) {
         PCCERT_CONTEXT element = nullptr;
         DWORD type = 0;
         if (CertAddSerializedElementToStore(nullptr, blob.pbData, blob.cbData, CERT_STORE_ADD_ALWAYS, 0,
             CERT_STORE_CERTIFICATE_CONTEXT_FLAG, &type, (const void **)&element) and element) {
            seen_fingerprint = fingerprint_of(element);
            CertFreeCertificateContext(element);
         }
         else seen_fingerprint = "(unparsed blob " + std::to_string(blob.cbData) + " bytes)";
         FreeContextBuffer(blob.pbData);
         verification_point = std::string(Point) + " (SERIALIZED_REMOTE_CERT_CONTEXT)";
         log("remote certificate available " + verification_point);
         return true;
      }
      log(std::string("remote certificate not yet available ") + Point + ": REMOTE_CERT_CONTEXT " +
         status_name(status) + ", SERIALIZED " + status_name(serial));
      return false;
   }

   // Run one ISC/ASC call, collecting every output fragment.  Input is null for the initial client call, timer-driven
   // retransmission, or fragment continuation.

   SECURITY_STATUS step(const Bytes *Input, const char *Reason) {
      std::vector<Bytes> flight;
      bool first = not has_ctx;
      SECURITY_STATUS status = SEC_E_OK;
      bool continuing = false;

      while (true) {
         calls++;
         std::vector<SecBuffer> in_bufs;
         Bytes srtp_blob;
         SEC_DTLS_MTU mtu_value{ USHORT(opt->mtu) };

         // Buffer layout follows str0m-wincrypto: [DTLS_MTU, SRTP_PROFILES, TOKEN, EMPTY, EXTRA] when a datagram is
         // supplied, and only the leading parameter buffers otherwise.  SPIKE_PARAMS=0 omits MTU/SRTP after the
         // first call; SPIKE_SRTP_LE=1 sends host-order profile IDs instead of big-endian.

         bool params = not handshake_done and (first or env_int("SPIKE_PARAMS", 1));
         // SPIKE_CLOSE_PARAMS: bit 0 adds the MTU buffer and bit 1 the SRTP buffer to post-handshake (close) calls.
         int close_params = handshake_done ? env_int("SPIKE_CLOSE_PARAMS", 0) : 0;
         auto &profiles = server ? opt->server_srtp : opt->client_srtp;
         if ((params or (close_params & 1)) and opt->mtu_via_buffer) {
            in_bufs.push_back({ sizeof(mtu_value), SECBUFFER_DTLS_MTU, &mtu_value });
         }
         if ((params or (close_params & 2)) and not profiles.empty()) {
            srtp_blob.resize(sizeof(unsigned short) * (1 + profiles.size()));
            auto words = (unsigned short *)srtp_blob.data();
            words[0] = (unsigned short)(profiles.size() * sizeof(unsigned short));
            bool little = env_int("SPIKE_SRTP_LE", 0);
            for (size_t i = 0; i < profiles.size(); i++) {
               words[i + 1] = little ? profiles[i] : (unsigned short)((profiles[i] >> 8) | (profiles[i] << 8));
            }
            in_bufs.push_back({ ULONG(srtp_blob.size()), SECBUFFER_SRTP_PROTECTION_PROFILES, srtp_blob.data() });
         }
         size_t token_index = in_bufs.size();
         if (Input and not continuing) {
            in_bufs.push_back({ ULONG(Input->size()), SECBUFFER_TOKEN, (void *)Input->data() });
            in_bufs.push_back({ 0, SECBUFFER_EMPTY, nullptr });
            in_bufs.push_back({ 0, SECBUFFER_EXTRA, nullptr });
         }
         else if (handshake_done) {
            // Control-token calls (close/alert).  SPIKE_CTRL_INPUT: 0 = no input, 1 = one empty TOKEN,
            // 2 = TOKEN + EMPTY, 3 = one SECBUFFER_EMPTY.
            switch (env_int("SPIKE_CTRL_INPUT", 0)) {
               case 1: in_bufs.push_back({ 0, SECBUFFER_TOKEN, nullptr }); break;
               case 2:
                  in_bufs.push_back({ 0, SECBUFFER_TOKEN, nullptr });
                  in_bufs.push_back({ 0, SECBUFFER_EMPTY, nullptr });
                  break;
               case 3: in_bufs.push_back({ 0, SECBUFFER_EMPTY, nullptr }); break;
               default: break;
            }
         }

         SecBufferDesc in_desc = { SECBUFFER_VERSION, ULONG(in_bufs.size()), in_bufs.data() };
         bool no_input = in_bufs.empty();

         // SPIKE_ALLOC=1 asks Schannel to allocate output (ISC/ASC_REQ_ALLOCATE_MEMORY); the default supplies
         // caller-owned buffers.
         bool allocate = env_int("SPIKE_ALLOC", 0);
         static thread_local uint8_t token_space[65536], alert_space[1024];
         SecBuffer out_bufs[2] = { { 0, SECBUFFER_TOKEN, nullptr }, { 0, SECBUFFER_ALERT, nullptr } };
         if (not allocate) {
            out_bufs[0] = { ULONG(sizeof(token_space)), SECBUFFER_TOKEN, token_space };
            out_bufs[1] = { ULONG(sizeof(alert_space)), SECBUFFER_ALERT, alert_space };
         }
         SecBufferDesc out_desc = { SECBUFFER_VERSION, ULONG(env_int("SPIKE_ALERT_BUF", 1) ? 2 : 1), out_bufs };
         ULONG attrs = 0;
         TimeStamp expiry;

         // The same handle must be passed as phNewContext on every call, not only the first (contrary to the SSPI
         // documentation); otherwise SEC_I_MESSAGE_FRAGMENT continuations fail with SEC_E_INCOMPLETE_MESSAGE.
         bool null_new = env_int("SPIKE_NULL_NEWCTX", 0) or (handshake_done and env_int("SPIKE_CLOSE_MODE", 4) IS 3);
         CtxtHandle *new_ctx = (null_new and has_ctx) ? nullptr : &ctx;

         if (server) {
            if (opt->set_client_address and env_int("SPIKE_ADDR", 1)) {
               // SPIKE_ADDR=1: sockaddr_in (16 bytes); 2: raw IPv4 + port (6 bytes).
               static sockaddr_in sin = []() {
                  sockaddr_in s{};
                  s.sin_family = AF_INET;
                  s.sin_port = htons(5000);
                  s.sin_addr.S_un.S_un_b = { 192, 0, 2, 1 };
                  return s;
               }();
               // 3: SOCKADDR_STORAGE holding the IPv4 address; 4: sockaddr_in6 (::1).
               static UCHAR raw[] = { 192, 0, 2, 1, 0x13, 0x88 };
               static SOCKADDR_STORAGE storage = []() {
                  SOCKADDR_STORAGE s{};
                  std::memcpy(&s, &sin, sizeof(sin));
                  return s;
               }();
               static sockaddr_in6 sin6 = []() {
                  sockaddr_in6 s{};
                  s.sin6_family = AF_INET6;
                  s.sin6_port = htons(5000);
                  s.sin6_addr.u.Byte[15] = 1;
                  return s;
               }();
               UCHAR *address = (UCHAR *)&sin;
               ULONG address_size = sizeof(sin);
               switch (env_int("SPIKE_ADDR", 1)) {
                  case 2: address = raw; address_size = sizeof(raw); break;
                  case 3: address = (UCHAR *)&storage; address_size = sizeof(storage); break;
                  case 4: address = (UCHAR *)&sin6; address_size = sizeof(sin6); break;
                  default: break;
               }
               using SetAddressFn = SECURITY_STATUS (SEC_ENTRY *)(PUCHAR, ULONG, int *);
               static SetAddressFn set_address = []() -> SetAddressFn {
                  // The SDK declares SecAllocateAndSetIPAddress (OneCore apiset libs only); sspicli.dll exports it
                  // as SeciAllocateAndSetIPAddress.
                  for (auto fn_name : { "SecAllocateAndSetIPAddress", "SeciAllocateAndSetIPAddress" }) {
                     auto fn = (SetAddressFn)GetProcAddress(LoadLibraryW(L"sspicli.dll"), fn_name);
                     if (fn) { std::printf("    %s resolved from sspicli.dll\n", fn_name); return fn; }
                  }
                  std::printf("    SecAllocateAndSetIPAddress not exported\n");
                  return nullptr;
               }();
               int free_context = 0;
               if (set_address) {
                  auto ip = set_address(address, address_size, &free_context);
                  if (ip != SEC_E_OK) log(std::string("SecAllocateAndSetIPAddress ") + status_name(ip));
               }
            }
            ULONG req = ASC_REQ_DATAGRAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_INTEGRITY | ASC_REQ_EXTENDED_ERROR;
            if (allocate) req |= ASC_REQ_ALLOCATE_MEMORY;
            if (env_int("SPIKE_MUTUAL", 1)) req |= ASC_REQ_MUTUAL_AUTH;
            req = env_hex("SPIKE_ASC_REQ", req);
            status = AcceptSecurityContext(&cred, has_ctx ? &ctx : nullptr, &in_desc, req, SECURITY_NATIVE_DREP,
               new_ctx, &out_desc, &attrs, &expiry);
         }
         else {
            ULONG req = ISC_REQ_DATAGRAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY | ISC_REQ_USE_SUPPLIED_CREDS |
               ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_EXTENDED_ERROR;
            if (allocate) req |= ISC_REQ_ALLOCATE_MEMORY;
            status = InitializeSecurityContextW(&cred, has_ctx ? &ctx : nullptr, nullptr, req, 0,
               SECURITY_NATIVE_DREP, no_input ? nullptr : &in_desc, 0, new_ctx, &out_desc, &attrs,
               &expiry);
         }

         if (not has_ctx and (status IS SEC_E_OK or status IS SEC_I_CONTINUE_NEEDED or
             status IS SEC_I_MESSAGE_FRAGMENT or status IS SEC_E_INCOMPLETE_MESSAGE or
             (out_bufs[0].cbBuffer and out_bufs[0].pvBuffer))) {
            has_ctx = true;
         }

         std::string line = std::string(continuing ? "continue" : Reason) + ": " + status_name(status);
         if (Input and not continuing) line += " (in " + std::to_string(Input->size()) + ": " + describe(*Input) + ")";
         for (size_t i = token_index + 1; i < in_bufs.size(); i++) {
            if (in_bufs[i].BufferType IS SECBUFFER_EXTRA and in_bufs[i].cbBuffer) {
               line += " EXTRA " + std::to_string(in_bufs[i].cbBuffer);
               if (Input and in_bufs[i].cbBuffer < Input->size()) {
                  pending_extra.assign(Input->end() - in_bufs[i].cbBuffer, Input->end());
               }
            }
            if (in_bufs[i].BufferType IS SECBUFFER_MISSING) line += " MISSING " + std::to_string(in_bufs[i].cbBuffer);
         }
         if (out_bufs[1].cbBuffer and (allocate or out_bufs[1].cbBuffer != sizeof(alert_space))) {
            line += " alert " + std::to_string(out_bufs[1].cbBuffer);
         }
         log(line);

         bool output_ok = status IS SEC_E_OK or status IS SEC_I_CONTINUE_NEEDED or status IS SEC_I_MESSAGE_FRAGMENT or
            status IS SEC_I_CONTEXT_EXPIRED or FAILED(status);
         if (not allocate and out_bufs[0].cbBuffer IS sizeof(token_space)) output_ok = false; // Untouched
         if (output_ok and out_bufs[0].cbBuffer and out_bufs[0].pvBuffer and
             out_bufs[0].BufferType IS SECBUFFER_TOKEN) {
            emit((uint8_t *)out_bufs[0].pvBuffer, out_bufs[0].cbBuffer, flight);
         }
         if (out_bufs[1].pvBuffer and out_bufs[1].cbBuffer and out_bufs[1].BufferType IS SECBUFFER_ALERT and
             (allocate or out_bufs[1].cbBuffer != sizeof(alert_space))) {
            log("  alert buffer " + hex((uint8_t *)out_bufs[1].pvBuffer, std::min<size_t>(out_bufs[1].cbBuffer, 16)));
            emit((uint8_t *)out_bufs[1].pvBuffer, out_bufs[1].cbBuffer, flight);
         }
         if (allocate) {
            if (out_bufs[0].pvBuffer) FreeContextBuffer(out_bufs[0].pvBuffer);
            if (out_bufs[1].pvBuffer) FreeContextBuffer(out_bufs[1].pvBuffer);
         }

         if (first and has_ctx and not opt->mtu_via_buffer) {
            first = false;
            DWORD mtu_dword = DWORD(opt->mtu);
            bool as_dword = env_int("SPIKE_MTU_DWORD", 1); // A 2-byte SEC_DTLS_MTU is rejected
            auto st = as_dword ? SetContextAttributesW(&ctx, SECPKG_ATTR_DTLS_MTU, &mtu_dword, sizeof(mtu_dword)) :
               SetContextAttributesW(&ctx, SECPKG_ATTR_DTLS_MTU, &mtu_value, sizeof(mtu_value));
            log(std::string("SetContextAttributes(DTLS_MTU) ") + status_name(st));
         }
         first = false;

         if (status IS SEC_I_MESSAGE_FRAGMENT) { continuing = true; continue; }
         break;
      }

      // Peer verification hook: check the certificate before any output from this call leaves.

      if (has_ctx and not failed) {
         bool was_seen = not seen_fingerprint.empty();
         if (probe_remote_cert(status IS SEC_E_OK ? "at handshake completion" : "mid-handshake") and not was_seen) {
            std::string pending = "pending output " + std::to_string(flight.size()) + " datagram(s)";
            log("  " + pending);
            if (reject_peer and seen_fingerprint != expected_fingerprint) {
               log("fingerprint mismatch: withholding output and sending bad_certificate");
               flight.clear();
               send_alert(TLS1_ALERT_FATAL, TLS1_ALERT_BAD_CERTIFICATE);
               failed = true;
               timer_armed = false;
               return status;
            }
         }
      }

      bool progressed = Input != nullptr;
      flush(flight, progressed or std::strcmp(Reason, "timeout") != 0); // A timer resend keeps the back-off

      if (status IS SEC_E_OK and not handshake_done) {
         handshake_done = true;
         timer_armed = false;
         auto st = QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
         log("handshake complete; STREAM_SIZES " + std::string(status_name(st)) + " header " +
            std::to_string(sizes.cbHeader) + " trailer " + std::to_string(sizes.cbTrailer) + " max message " +
            std::to_string(sizes.cbMaximumMessage) + " block " + std::to_string(sizes.cbBlockSize));
      }
      else if (status != SEC_I_CONTINUE_NEEDED and status != SEC_E_OK and status != SEC_E_INCOMPLETE_MESSAGE) {
         failed = true;
         timer_armed = false;
      }
      return status;
   }

   void send_alert(DWORD Level, DWORD Code) {
      SCHANNEL_ALERT_TOKEN token = { SCHANNEL_ALERT, Level, Code };
      SecBuffer buf = { sizeof(token), SECBUFFER_TOKEN, &token };
      SecBufferDesc desc = { SECBUFFER_VERSION, 1, &buf };
      auto st = ApplyControlToken(&ctx, &desc);
      log(std::string("ApplyControlToken(ALERT) ") + status_name(st));
      // Schannel DTLS rejects SCHANNEL_ALERT (SEC_E_UNSUPPORTED_FUNCTION).  A token-less ISC/ASC would then start a
      // renegotiation instead of sending an alert, so nothing is emitted unless SPIKE_ALERT_STEP=1.
      if (st != SEC_E_OK and not env_int("SPIKE_ALERT_STEP", 0)) return;
      bool was_done = handshake_done;
      handshake_done = true; // Suppress timer/flight bookkeeping
      step(nullptr, "alert");
      handshake_done = was_done;
   }

   // Qop is SECQOP_WRAP_OOB_DATA for an alert, in which case Plain is the two-byte alert (level, description).

   SECURITY_STATUS encrypt(const Bytes &Plain, ULONG Qop = 0) {
      Bytes buffer(sizes.cbHeader + Plain.size() + sizes.cbTrailer);
      std::memcpy(buffer.data() + sizes.cbHeader, Plain.data(), Plain.size());
      SecBuffer bufs[4] = {
         { sizes.cbHeader, SECBUFFER_STREAM_HEADER, buffer.data() },
         { ULONG(Plain.size()), SECBUFFER_DATA, buffer.data() + sizes.cbHeader },
         { sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buffer.data() + sizes.cbHeader + Plain.size() },
         { 0, SECBUFFER_EMPTY, nullptr } };
      SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
      auto st = EncryptMessage(&ctx, Qop, &desc, 0);
      if (st IS SEC_E_OK) {
         size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
         // Trailer may be shorter than advertised; compact if the data length moved.
         Bytes out;
         out.insert(out.end(), (uint8_t *)bufs[0].pvBuffer, (uint8_t *)bufs[0].pvBuffer + bufs[0].cbBuffer);
         out.insert(out.end(), (uint8_t *)bufs[1].pvBuffer, (uint8_t *)bufs[1].pvBuffer + bufs[1].cbBuffer);
         out.insert(out.end(), (uint8_t *)bufs[2].pvBuffer, (uint8_t *)bufs[2].pvBuffer + bufs[2].cbBuffer);
         (void)total;
         datagrams_sent++;
         largest = std::max(largest, out.size());
         outbox.push_back(out);
      }
      return st;
   }

   SECURITY_STATUS decrypt(const Bytes &Datagram) {
      Bytes copy = Datagram;
      SecBuffer bufs[4] = { { ULONG(copy.size()), SECBUFFER_DATA, copy.data() }, { 0, SECBUFFER_EMPTY, nullptr },
         { 0, SECBUFFER_EMPTY, nullptr }, { 0, SECBUFFER_EMPTY, nullptr } };
      SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
      auto st = DecryptMessage(&ctx, &desc, 0, nullptr);
      std::string types;
      for (auto &b : bufs) {
         if (b.BufferType IS SECBUFFER_DATA and st IS SEC_E_OK) {
            received.emplace_back((uint8_t *)b.pvBuffer, (uint8_t *)b.pvBuffer + b.cbBuffer);
         }
         if (b.BufferType IS SECBUFFER_EXTRA) types += " EXTRA " + std::to_string(b.cbBuffer);
      }
      if (st IS SEC_I_CONTEXT_EXPIRED) closed = true;
      if (st IS SEC_I_RENEGOTIATE and env_int("SPIKE_RENEG", 0)) {
         // SPIKE_RENEG=1: hand the record back to ISC/ASC, as Schannel's TLS renegotiation contract requires.
         // SPIKE_RENEG=2: hand back only the SECBUFFER_EXTRA bytes.
         Bytes extra;
         for (auto &b : bufs) {
            if (b.BufferType IS SECBUFFER_EXTRA and b.cbBuffer) {
               extra.assign(Datagram.end() - b.cbBuffer, Datagram.end());
            }
         }
         const Bytes &input = (env_int("SPIKE_RENEG", 0) IS 2 and not extra.empty()) ? extra : Datagram;
         bool was_done = handshake_done;
         auto rs = step(&input, "renegotiate-in");
         handshake_done = was_done;
         log(std::string("  renegotiate hand-back ") + status_name(rs));
      }
      log(std::string("DecryptMessage ") + status_name(st) + " (" + describe(Datagram) + ")" + types);
      return st;
   }

   void feed(const Bytes &Datagram) {
      if (failed or closed) return;
      if (not handshake_done) {
         // A datagram may carry several records; Schannel consumes one per call and reports the rest as EXTRA.
         Bytes current = Datagram;
         for (int guard = 0; guard < 16; guard++) {
            pending_extra.clear();
            step(&current, guard ? "feed-extra" : "feed");
            if (pending_extra.empty() or failed) break;
            current = pending_extra;
            if (handshake_done) { decrypt(current); break; }
         }
      }
      else if (env_int("SPIKE_LATE", 3) IS 1 and Datagram.size() and Datagram[0] IS 22 and Datagram[3] IS 0 and
          Datagram[4] IS 0) {
         // An epoch-0 handshake record after completion is a retransmitted flight from the peer, which means our
         // final flight was lost.  Schannel must be given the record so that it can retransmit.
         step(&Datagram, "late-hs");
      }
      else if (env_int("SPIKE_LATE", 3) IS 2 and Datagram.size() and Datagram[3] IS 0 and Datagram[4] IS 0) {
         // SPIKE_LATE=2: discard the epoch-0 record and ask Schannel to retransmit the final flight with a
         // token-less call, once per peer flight (keyed on the first epoch-0 handshake record).
         if (Datagram[0] IS 22) step(nullptr, "late-retransmit");
         else log("  (late epoch-0 record discarded)");
      }
      else if (env_int("SPIKE_LATE", 3) IS 3 and Datagram.size() and (Datagram[0] IS 20 or Datagram[0] IS 22)) {
         // SPIKE_LATE=3: post-handshake CCS/handshake records are only retransmissions (or renegotiation, which WebRTC
         // refuses), so none reach DecryptMessage.  An epoch-0 handshake record means the peer missed our final
         // flight: replay the cached copy verbatim, at most once per 10 ms burst.
         bool epoch0 = Datagram.size() > 4 and Datagram[3] IS 0 and Datagram[4] IS 0;
         if (not epoch0 or Datagram[0] != 22) log("  (late " + describe(Datagram) + " discarded)");
         else if (Clock::now() - last_replay > std::chrono::milliseconds(10)) {
            last_replay = Clock::now();
            log("late retransmission from peer: replaying cached final flight (" +
               std::to_string(last_flight.size()) + " datagrams)");
            for (auto &dg : last_flight) outbox.push_back(dg);
         }
      }
      else decrypt(Datagram);
   }

   Clock::time_point last_replay{};
   Bytes pending_extra;

   void tick() {
      if (not timer_armed or failed or handshake_done) return;
      if (Clock::now() < timer_deadline) return;
      retransmits++;
      log("timer expired after " + std::to_string(timer_ms) + " ms");
      std::vector<Bytes> flight;
      size_t before = outbox.size();
      step(nullptr, "timeout");
      bool same = outbox.size() - before IS last_flight.size();
      if (same) {
         for (size_t i = 0; i < last_flight.size(); i++) {
            if (outbox[before + i].size() != last_flight[i].size()) same = false;
         }
      }
      log(std::string("  retransmission produced ") + std::to_string(outbox.size() - before) + " datagram(s)" +
         (same ? " matching the last flight size" : ""));
      if (timer_armed) arm_timer(false);
   }
};

struct Link {
   Peer client{ "client", false }, server{ "server", true };
   Options opt;
   size_t index_c = 0, index_s = 0;

   bool deliver() {
      auto from_c = std::move(client.outbox);
      auto from_s = std::move(server.outbox);
      client.outbox.clear();
      server.outbox.clear();
      auto transfer = [this](std::deque<Bytes> &Flight, char From, size_t &Index, Peer &To) {
         if (opt.reverse_flights) std::reverse(Flight.begin(), Flight.end());
         for (auto &dg : Flight) {
            auto index = Index++;
            if (opt.drop and opt.drop(From, index)) { To.log("  (dropped " + describe(dg) + ")"); continue; }
            To.feed(dg);
            if (opt.duplicate and opt.duplicate(From, index)) { To.log("  (duplicate)"); To.feed(dg); }
         }
      };
      transfer(from_c, 'c', index_c, server);
      transfer(from_s, 's', index_s, client);
      return not from_c.empty() or not from_s.empty();
   }

   bool pump(const std::function<bool()> &Done, int TimeoutMs = 5000) {
      auto deadline = Clock::now() + std::chrono::milliseconds(TimeoutMs);
      while (Clock::now() < deadline) {
         if (Done()) return true;
         if (deliver()) continue;
         client.tick();
         server.tick();
         if (client.outbox.empty() and server.outbox.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return Done();
   }

   bool connected() { return client.handshake_done and server.handshake_done and not client.failed and
      not server.failed; }
};

struct CertPair {
   Certificate a, b;
};

bool setup(Link &L, CertPair &Certs, const Options &Opt)
{
   L.opt = Opt;
   L.client.opt = &L.opt;
   L.server.opt = &L.opt;
   if (not acquire_credentials(L.client.cred, Certs.a, false, Opt)) return false;
   if (not acquire_credentials(L.server.cred, Certs.b, true, Opt)) return false;
   L.client.expected_fingerprint = Certs.b.fingerprint;
   L.server.expected_fingerprint = Certs.a.fingerprint;
   L.client.reject_peer = Opt.client_rejects_server;
   L.server.reject_peer = Opt.server_rejects_client;
   return true;
}

bool handshake(Link &L, int TimeoutMs = 5000)
{
   L.client.step(nullptr, "start");
   return L.pump([&]() { return L.connected() or L.client.failed or L.server.failed; }, TimeoutMs) and
      L.connected();
}

void report_keys(Link &L, Bytes &ClientKm, Bytes &ServerKm, uint16_t &ClientProfile, uint16_t &ServerProfile)
{
   for (auto *peer : { &L.client, &L.server }) {
      SecPkgContext_SrtpParameters srtp{};
      auto st = QueryContextAttributesW(&peer->ctx, SECPKG_ATTR_SRTP_PARAMETERS, &srtp);
      // ProtectionProfile is reported in network byte order, like the input buffer.
      uint16_t profile = st IS SEC_E_OK ? uint16_t((srtp.ProtectionProfile >> 8) | (srtp.ProtectionProfile << 8)) :
         0xffff;
      if (st IS SEC_E_OK and srtp.MasterKeyIdentifier) FreeContextBuffer(srtp.MasterKeyIdentifier);
      std::printf("    %s SRTP_PARAMETERS %s profile 0x%04x\n", peer->name, status_name(st), profile);
      (peer IS &L.client ? ClientProfile : ServerProfile) = profile;

      SecPkgContext_CipherInfo cipher{};
      cipher.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
      st = QueryContextAttributesW(&peer->ctx, SECPKG_ATTR_CIPHER_INFO, &cipher);
      if (st IS SEC_E_OK) {
         std::printf("    %s cipher %ls (proto 0x%lx, keyx %ls, sig %ls)\n", peer->name, cipher.szCipherSuite,
            cipher.dwProtocol, cipher.szExchange, cipher.szCertificate);
      }
      else std::printf("    %s CIPHER_INFO %s\n", peer->name, status_name(st));

      if (not L.opt.export_keys) continue;
      int key_len = profile IS 0x0008 ? 32 : 16, salt_len = (profile IS 0x0007 or profile IS 0x0008) ? 12 : 14;
      static char label[] = "EXTRACTOR-dtls_srtp";
      SecPkgContext_KeyingMaterialInfo info{};
      info.cbLabel = WORD(sizeof(label));
      info.pszLabel = label;
      info.cbKeyingMaterial = DWORD(2 * (key_len + salt_len));
      st = SetContextAttributesW(&peer->ctx, SECPKG_ATTR_KEYING_MATERIAL_INFO, &info, sizeof(info));
      std::printf("    %s SetContextAttributes(KEYING_MATERIAL_INFO) %s\n", peer->name, status_name(st));
      SecPkgContext_KeyingMaterial km{};
      st = QueryContextAttributesW(&peer->ctx, SECPKG_ATTR_KEYING_MATERIAL, &km);
      std::printf("    %s KEYING_MATERIAL %s (%lu bytes)\n", peer->name, status_name(st), km.cbKeyingMaterial);
      if (st IS SEC_E_OK) {
         (peer IS &L.client ? ClientKm : ServerKm).assign(km.pbKeyingMaterial,
            km.pbKeyingMaterial + km.cbKeyingMaterial);
         FreeContextBuffer(km.pbKeyingMaterial);
      }
   }
}

//********************************************************************************************************************
// Scenarios

CertPair *glCerts = nullptr;

void scenario_basic(Options Opt, const char *Title)
{
   std::printf("\n== %s ==\n", Title);
   Link link;
   CHECK(setup(link, *glCerts, Opt), "credential setup failed");
   auto started = Clock::now();
   bool ok = handshake(link);
   auto ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
   std::printf("  handshake %s in %.1f ms; client %zu datagrams (largest %zu, %zu calls, %d timer), "
      "server %zu (largest %zu, %zu calls, %d timer)\n", ok ? "complete" : "FAILED", ms, link.client.datagrams_sent,
      link.client.largest, link.client.calls, link.client.retransmits, link.server.datagrams_sent,
      link.server.largest, link.server.calls, link.server.retransmits);
   CHECK(ok, "handshake did not complete");
   link.opt.drop = nullptr; // Loss applies to the handshake only; DTLS does not retransmit application data
   CHECK(link.client.largest <= size_t(Opt.mtu) and link.server.largest <= size_t(Opt.mtu), "datagram exceeded MTU");
   std::printf("  client saw server cert %s; server saw client cert %s\n", link.client.verification_point.c_str(),
      link.server.verification_point.c_str());
   CHECK(link.client.seen_fingerprint IS glCerts->b.fingerprint, "client saw wrong server fingerprint");
   CHECK(link.server.seen_fingerprint IS glCerts->a.fingerprint, "server saw wrong client fingerprint (mutual auth)");

   Bytes ckm, skm;
   uint16_t cp = 0, sp = 0;
   report_keys(link, ckm, skm, cp, sp);
   if (Opt.export_keys and not Opt.client_srtp.empty() and not Opt.server_srtp.empty()) {
      CHECK(cp IS sp, "profiles disagree");
      CHECK(not ckm.empty() and ckm IS skm, "exported keying material differs between client and server");
      std::printf("  keying material identical on both sides: %s...\n", hex(ckm.data(), 8).c_str());
   }

   // Application data
   int max_payload = Opt.mtu - int(link.client.sizes.cbHeader) - int(link.client.sizes.cbTrailer);
   std::printf("  max payload estimate (mtu - header - trailer): %d; cbMaximumMessage %lu\n", max_payload,
      link.client.sizes.cbMaximumMessage);
   Bytes small = { 'h', 'e', 'l', 'l', 'o' };
   Bytes large(size_t(max_payload), 0x5a);
   auto st = link.client.encrypt(small);
   CHECK(st IS SEC_E_OK, status_name(st));
   auto replay = link.client.outbox.back();
   st = link.server.encrypt(large);
   std::printf("  EncryptMessage(%d bytes) %s, datagram %zu\n", max_payload, status_name(st),
      link.server.outbox.empty() ? 0 : link.server.outbox.back().size());
   Bytes over(size_t(max_payload) + 1, 0xa5);
   auto before = link.server.outbox.size();
   st = link.server.encrypt(over);
   std::printf("  EncryptMessage(%d bytes) %s, datagram %zu\n", max_payload + 1, status_name(st),
      link.server.outbox.size() > before ? link.server.outbox.back().size() : 0);
   if (link.server.outbox.size() > before) link.server.outbox.pop_back();
   link.pump([&]() { return link.client.received.size() >= 1 and link.server.received.size() >= 1; }, 1000);
   CHECK(link.server.received.size() IS 1 and link.server.received[0] IS small, "small record not delivered");
   CHECK(link.client.received.size() IS 1 and link.client.received[0] IS large, "large record not delivered");

   st = link.server.decrypt(replay);
   std::printf("  replayed record: %s, delivered %zu\n", status_name(st), link.server.received.size());
   st = link.client.encrypt(small);
   auto corrupt = link.client.outbox.back();
   link.client.outbox.pop_back();
   corrupt.back() ^= 1;
   st = link.server.decrypt(corrupt);
   std::printf("  corrupted record: %s, delivered %zu\n", status_name(st), link.server.received.size());
   st = link.client.encrypt(small);
   link.pump([&]() { return link.server.received.size() >= 2; }, 500);
   std::printf("  fresh record after replay/corruption: delivered %zu\n", link.server.received.size());
   CHECK(link.server.received.size() IS 2, "context unusable after replay or bad MAC");

   if (not Opt.test_close) { std::printf("  PASS\n"); return; }

   // Close.  SPIKE_CLOSE_SERVER=1 closes from the server; SPIKE_SKIP_SHUTDOWN=1 omits the control token to show what
   // a bare post-handshake ISC/ASC call produces.
   bool server_closes = env_int("SPIKE_CLOSE_SERVER", 0);
   auto &closer = server_closes ? link.server : link.client;
   auto &observer = server_closes ? link.client : link.server;
   // SPIKE_CLOSE_MODE (default 4): 0 = SHUTDOWN then ISC/ASC; 1 = SCHANNEL_ALERT warning close_notify then ISC/ASC;
   // 2 = SHUTDOWN then EncryptMessage with an empty payload; 3 = SHUTDOWN then ISC/ASC with the context
   // passed only as phContext.
   // 4 = EncryptMessage(SECQOP_WRAP_OOB_DATA) carrying the two-byte alert {warning, close_notify}, with no SHUTDOWN
   // unless SPIKE_OOB_SHUTDOWN=1 (applied before) or 2 (applied after).  This is the only mode that works.
   // SPIKE_OOB_LEVEL / SPIKE_OOB_CODE send a different alert, e.g. 2 / 42 for a fatal bad_certificate.
   // SPIKE_CLOSE_ROUNDS=N exchanges N more application records in each direction first.
   // SPIKE_CLOSE_RETRY=N repeats the ISC/ASC call N times after the first one returns.
   int close_mode = env_int("SPIKE_CLOSE_MODE", 4);
   for (int round = 0; round < env_int("SPIKE_CLOSE_ROUNDS", 0); round++) {
      auto cr = link.client.received.size(), sr = link.server.received.size();
      link.client.encrypt(small);
      link.server.encrypt(small);
      link.pump([&]() { return link.client.received.size() > cr and link.server.received.size() > sr; }, 500);
   }
   auto apply_shutdown = [&]() {
      DWORD shutdown = SCHANNEL_SHUTDOWN;
      SecBuffer buf = { sizeof(shutdown), SECBUFFER_TOKEN, &shutdown };
      SecBufferDesc desc = { SECBUFFER_VERSION, 1, &buf };
      auto as = ApplyControlToken(&closer.ctx, &desc);
      std::printf("  %s ApplyControlToken(SHUTDOWN) %s\n", closer.name, status_name(as));
   };
   if (close_mode IS 4) {
      int oob_shutdown = env_int("SPIKE_OOB_SHUTDOWN", 0);
      if (oob_shutdown IS 1) apply_shutdown();
      auto sent_before = closer.outbox.size();
      st = closer.encrypt(Bytes{ uint8_t(env_int("SPIKE_OOB_LEVEL", TLS1_ALERT_WARNING)),
         uint8_t(env_int("SPIKE_OOB_CODE", TLS1_ALERT_CLOSE_NOTIFY)) }, SECQOP_WRAP_OOB_DATA);
      std::printf("  %s EncryptMessage(SECQOP_WRAP_OOB_DATA) %s\n", closer.name, status_name(st));
      if (closer.outbox.size() > sent_before) {
         auto &dg = closer.outbox.back();
         std::printf("    -> %zu bytes %s: %s\n", dg.size(), describe(dg).c_str(), hex(dg.data(), dg.size()).c_str());
      }
      if (oob_shutdown IS 2) apply_shutdown();
      // The closing side must still be able to tell that it is closed: see what a later send returns.
      auto after = closer.encrypt(small);
      std::printf("  %s EncryptMessage(data) after the alert: %s\n", closer.name, status_name(after));
      link.pump([&]() { return observer.closed; }, 500);
      std::printf("  %s observed close_notify: %s; records delivered to it afterwards: %zu", observer.name,
         observer.closed ? "yes" : "no", observer.received.size());
      for (size_t i = 2; i < observer.received.size(); i++) {
         std::printf(" [%zu bytes: %s]", observer.received[i].size(),
            hex(observer.received[i].data(), observer.received[i].size()).c_str());
      }
      std::printf("\n");
      auto back = observer.encrypt(small);
      std::printf("  %s EncryptMessage(data) after receiving the alert: %s\n", observer.name, status_name(back));
      CHECK(observer.closed, "close_notify not observed");
      std::printf("  PASS\n");
      return;
   }
   if (not env_int("SPIKE_SKIP_SHUTDOWN", 0)) {
      if (close_mode IS 1) {
         SCHANNEL_ALERT_TOKEN token = { SCHANNEL_ALERT, TLS1_ALERT_WARNING, TLS1_ALERT_CLOSE_NOTIFY };
         SecBuffer buf = { sizeof(token), SECBUFFER_TOKEN, &token };
         SecBufferDesc desc = { SECBUFFER_VERSION, 1, &buf };
         st = ApplyControlToken(&closer.ctx, &desc);
         std::printf("  %s ApplyControlToken(ALERT close_notify) %s\n", closer.name, status_name(st));
      }
      else {
         DWORD shutdown = SCHANNEL_SHUTDOWN;
         SecBuffer buf = { sizeof(shutdown), SECBUFFER_TOKEN, &shutdown };
         SecBufferDesc desc = { SECBUFFER_VERSION, 1, &buf };
         st = ApplyControlToken(&closer.ctx, &desc);
         std::printf("  %s ApplyControlToken(SHUTDOWN) %s\n", closer.name, status_name(st));
      }
   }
   if (close_mode IS 2) {
      st = closer.encrypt(Bytes{});
      std::printf("  %s EncryptMessage(empty) after SHUTDOWN %s\n", closer.name, status_name(st));
      if (not closer.outbox.empty()) {
         auto &dg = closer.outbox.back();
         std::printf("    -> %zu bytes %s\n", dg.size(), hex(dg.data(), std::min<size_t>(dg.size(), 32)).c_str());
      }
   }
   else {
      closer.step(nullptr, "close");
      for (int retry = 0; retry < env_int("SPIKE_CLOSE_RETRY", 0); retry++) closer.step(nullptr, "close-retry");
   }
   link.pump([&]() { return observer.closed; }, 500);
   std::printf("  %s observed close_notify: %s\n", observer.name, observer.closed ? "yes" : "no");
   CHECK(observer.closed, "close_notify not observed");
   std::printf("  PASS\n");
}

void scenario_reject(bool ServerRejects)
{
   std::printf("\n== fingerprint mismatch, %s rejects ==\n", ServerRejects ? "server" : "client");
   Options opt;
   Link link;
   CHECK(setup(link, *glCerts, opt), "credential setup failed");
   if (ServerRejects) {
      link.server.reject_peer = true;
      link.server.expected_fingerprint = glCerts->b.fingerprint; // Wrong
   }
   else {
      link.client.reject_peer = true;
      link.client.expected_fingerprint = glCerts->a.fingerprint;
   }
   link.client.step(nullptr, "start");
   link.pump([&]() { return false; }, 1500);
   auto &rejector = ServerRejects ? link.server : link.client;
   auto &other = ServerRejects ? link.client : link.server;
   std::printf("  rejector failed=%d at '%s'; other handshake_done=%d failed=%d\n", rejector.failed,
      rejector.verification_point.c_str(), other.handshake_done, other.failed);
   CHECK(rejector.failed, "rejecting side did not fail");
   CHECK(not other.handshake_done, "peer completed the handshake against a rejecting side");
   std::printf("  PASS\n");
}

void scenario_retransmit_probe()
{
   // Drop the server's entire first response so the client times out, then watch what a token-less ISC produces.
   std::printf("\n== retransmission probe ==\n");
   Options opt;
   opt.drop = [](char From, size_t Index) { return Index IS 0; };
   Link link;
   CHECK(setup(link, *glCerts, opt), "credential setup failed");
   bool ok = handshake(link);
   std::printf("  handshake after first-datagram loss each way: %s (client timers %d, server timers %d)\n",
      ok ? "complete" : "FAILED", link.client.retransmits, link.server.retransmits);
   CHECK(ok, "no recovery from loss");
   std::printf("  PASS\n");
}

void scenario_final_flight_loss()
{
   // Drop the server's final flight (CCS+Finished).  The client must retransmit its flight 5 on timeout, and the
   // server, which has already completed, must retransmit its final flight when it sees the duplicate.
   std::printf("\n== final flight loss ==\n");
   Options opt;
   Link link;
   CHECK(setup(link, *glCerts, opt), "credential setup failed");
   // Custom pump: drop server output that is produced at handshake completion, once.
   link.client.step(nullptr, "start");
   bool dropped_final = false;
   auto deadline = Clock::now() + std::chrono::seconds(5);
   while (Clock::now() < deadline and not link.connected()) {
      if (link.server.handshake_done and not dropped_final and not link.server.outbox.empty()) {
         for (auto &dg : link.server.outbox) link.server.log("  (dropped final " + describe(dg) + ")");
         link.server.outbox.clear();
         dropped_final = true;
      }
      if (link.deliver()) continue;
      link.client.tick();
      link.server.tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   std::printf("  final flight dropped=%d, connected=%d, client timers %d\n", dropped_final, link.connected(),
      link.client.retransmits);
   CHECK(link.connected(), "no recovery from final-flight loss");
   std::printf("  PASS\n");
}

//********************************************************************************************************************
// Interop over real UDP on 127.0.0.1 against an independent DTLS stack (e.g. `openssl s_server -dtls1_2`).  The peer's
// exported keying material is printed so that it can be compared with the peer's own `-keymatexport` output.

int run_udp(bool ServerRole, int Port, int Seconds)
{
   std::printf("\n== UDP interop: Schannel %s on 127.0.0.1:%d ==\n", ServerRole ? "server" : "client", Port);
   WSADATA wsa;
   WSAStartup(MAKEWORD(2, 2), &wsa);
   SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   sockaddr_in local{}, remote{};
   local.sin_family = AF_INET;
   local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   remote = local;
   if (ServerRole) {
      local.sin_port = htons(u_short(Port));
      if (bind(sock, (sockaddr *)&local, sizeof(local))) {
         std::printf("  bind failed %d\n", WSAGetLastError());
         return 1;
      }
   }
   else remote.sin_port = htons(u_short(Port));
   bool have_remote = not ServerRole;

   Options opt;
   opt.initial_timeout_ms = 200;
   Peer peer{ ServerRole ? "server" : "client", ServerRole };
   peer.opt = &opt;
   if (not acquire_credentials(peer.cred, ServerRole ? glCerts->b : glCerts->a, ServerRole, opt)) return 1;
   if (not ServerRole) peer.step(nullptr, "start");

   auto send_all = [&]() {
      while (not peer.outbox.empty()) {
         auto &dg = peer.outbox.front();
         if (have_remote) sendto(sock, (const char *)dg.data(), int(dg.size()), 0, (sockaddr *)&remote, sizeof(remote));
         peer.outbox.pop_front();
      }
   };

   // SPIKE_UDP_CLOSE_MS=N sends close_notify (EncryptMessage with SECQOP_WRAP_OOB_DATA) N ms after the handshake
   // completes, then keeps listening so that the peer's own close_notify can be observed.
   int close_after_ms = env_int("SPIKE_UDP_CLOSE_MS", -1);
   bool close_sent = false;
   Clock::time_point connected_at{};
   bool greeted = false;
   auto deadline = Clock::now() + std::chrono::seconds(Seconds);
   while (Clock::now() < deadline and not peer.failed and not peer.closed) {
      send_all();
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(sock, &fds);
      timeval tv = { 0, 10000 };
      if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
         uint8_t buffer[65536];
         sockaddr_in from{};
         int from_len = sizeof(from);
         int n = recvfrom(sock, (char *)buffer, sizeof(buffer), 0, (sockaddr *)&from, &from_len);
         if (n > 0) {
            if (not have_remote) { remote = from; have_remote = true; }
            peer.feed(Bytes(buffer, buffer + n));
            for (; not peer.received.empty(); peer.received.erase(peer.received.begin())) {
               auto &msg = peer.received.front();
               std::printf("  received %zu bytes: %.*s\n", msg.size(), int(msg.size()), (const char *)msg.data());
            }
         }
      }
      peer.tick();
      if (peer.handshake_done and not greeted) {
         greeted = true;
         std::printf("  handshake complete; peer fingerprint %s\n", peer.seen_fingerprint.c_str());
         SecPkgContext_SrtpParameters srtp{};
         auto st = QueryContextAttributesW(&peer.ctx, SECPKG_ATTR_SRTP_PARAMETERS, &srtp);
         uint16_t profile = uint16_t((srtp.ProtectionProfile >> 8) | (srtp.ProtectionProfile << 8));
         if (st IS SEC_E_OK and srtp.MasterKeyIdentifier) FreeContextBuffer(srtp.MasterKeyIdentifier);
         SecPkgContext_CipherInfo cipher{ SECPKGCONTEXT_CIPHERINFO_V1 };
         QueryContextAttributesW(&peer.ctx, SECPKG_ATTR_CIPHER_INFO, &cipher);
         std::printf("  cipher %ls, SRTP profile 0x%04x (%s)\n", cipher.szCipherSuite, profile, status_name(st));
         DWORD length = profile IS 0x0007 ? 56 : (profile IS 0x0008 ? 88 : 60);
         static char label[] = "EXTRACTOR-dtls_srtp";
         SecPkgContext_KeyingMaterialInfo info{ WORD(sizeof(label)), label, 0, nullptr, length };
         st = SetContextAttributesW(&peer.ctx, SECPKG_ATTR_KEYING_MATERIAL_INFO, &info, sizeof(info));
         SecPkgContext_KeyingMaterial km{};
         auto qs = QueryContextAttributesW(&peer.ctx, SECPKG_ATTR_KEYING_MATERIAL, &km);
         if (qs IS SEC_E_OK) {
            std::printf("  Keying material (%lu): %s\n", km.cbKeyingMaterial,
               hex(km.pbKeyingMaterial, km.cbKeyingMaterial).c_str());
            FreeContextBuffer(km.pbKeyingMaterial);
         }
         else std::printf("  keying material export %s / %s\n", status_name(st), status_name(qs));
         std::string hello = std::string("hello from schannel ") + (ServerRole ? "server\n" : "client\n");
         peer.encrypt(Bytes(hello.begin(), hello.end()));
         connected_at = Clock::now();
      }
      if (greeted and close_after_ms >= 0 and not close_sent and
          Clock::now() - connected_at >= std::chrono::milliseconds(close_after_ms)) {
         close_sent = true;
         auto st = peer.encrypt(Bytes{ TLS1_ALERT_WARNING, TLS1_ALERT_CLOSE_NOTIFY }, SECQOP_WRAP_OOB_DATA);
         auto &dg = peer.outbox.back();
         std::printf("  close_notify sent: EncryptMessage(SECQOP_WRAP_OOB_DATA) %s -> %zu bytes %s\n", status_name(st),
            dg.size(), hex(dg.data(), dg.size()).c_str());
      }
   }
   if (peer.closed) std::printf("  peer close_notify received: DecryptMessage returned SEC_I_CONTEXT_EXPIRED\n");
   send_all();
   std::printf("  finished: handshake_done=%d failed=%d closed=%d\n", peer.handshake_done, peer.failed, peer.closed);
   closesocket(sock);
   return peer.handshake_done ? 0 : 1;
}

std::map<std::string, std::function<void()>> scenarios()
{
   std::map<std::string, std::function<void()>> list;
   list["basic"] = []() { scenario_basic(Options{}, "basic: DTLS 1.2, mutual auth, use_srtp GCM+CM, MTU 1200"); };
   list["close"] = []() { Options o; o.verbose = false; o.test_close = true; scenario_basic(o, "close_notify"); };
   list["mtu300"] = []() { Options o; o.mtu = 300; scenario_basic(o, "MTU 300 via SECBUFFER_DTLS_MTU"); };
   list["mtu_attr"] = []() {
      Options o; o.mtu = 300; o.mtu_via_buffer = false;
      scenario_basic(o, "MTU 300 via SetContextAttributes(SECPKG_ATTR_DTLS_MTU)");
   };
   list["cm_only"] = []() {
      Options o; o.server_srtp = { 0x0001 }; scenario_basic(o, "server offers only AES128_CM_SHA1_80");
   };
   list["no_srtp"] = []() {
      Options o; o.client_srtp.clear(); o.server_srtp.clear(); o.export_keys = false;
      scenario_basic(o, "data-channel only (no use_srtp)");
   };
   list["no_srtp_export"] = []() {
      Options o; o.client_srtp.clear(); o.server_srtp.clear();
      scenario_basic(o, "no use_srtp, keying material export attempted");
   };
   list["no_address"] = []() {
      Options o; o.set_client_address = false; scenario_basic(o, "server without SecAllocateAndSetIPAddress");
   };
   list["restricted"] = []() {
      Options o; o.restrict_suites = true; scenario_basic(o, "CRYPTO_SETTINGS restricts RSA/DH/3DES");
   };
   list["loss"] = []() { scenario_retransmit_probe(); };
   list["final_loss"] = []() { scenario_final_flight_loss(); };
   list["heavy_loss"] = []() {
      Options o; o.drop = [](char, size_t Index) { return (Index % 3) IS 1; }; o.verbose = false;
      scenario_basic(o, "33% loss both directions");
   };
   list["reorder"] = []() {
      Options o; o.mtu = 300; o.reverse_flights = true; o.verbose = false;
      scenario_basic(o, "MTU 300 with every flight reversed");
   };
   list["duplicate"] = []() {
      Options o; o.duplicate = [](char, size_t) { return true; }; o.verbose = false;
      scenario_basic(o, "every datagram duplicated");
   };
   list["reject_server"] = []() { scenario_reject(true); };
   list["reject_client"] = []() { scenario_reject(false); };
   return list;
}

} // namespace

int main(int ArgCount, char **Args)
{
   std::setvbuf(stdout, nullptr, _IONBF, 0);
   OSVERSIONINFOW version{ sizeof(version) };
   using RtlGetVersionFn = LONG (WINAPI *)(OSVERSIONINFOW *);
   auto rtl = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
   if (rtl) rtl(&version);
   std::printf("Schannel DTLS spike on Windows %lu.%lu build %lu\n", version.dwMajorVersion, version.dwMinorVersion,
      version.dwBuildNumber);

   KeyMode mode = KeyMode::EPHEMERAL_HANDLE;
   std::vector<std::string> selected;
   int udp_port = 0, udp_seconds = 10;
   bool udp_server = false;
   for (int i = 1; i < ArgCount; i++) {
      std::string arg = Args[i];
      if (arg IS "--named-key") mode = KeyMode::NAMED_PERSISTED;
      else if ((arg IS "--udp-client" or arg IS "--udp-server") and i + 1 < ArgCount) {
         udp_server = arg IS "--udp-server";
         udp_port = std::atoi(Args[++i]);
      }
      else if (arg IS "--seconds" and i + 1 < ArgCount) udp_seconds = std::atoi(Args[++i]);
      else selected.push_back(arg);
   }

   CertPair certs;
   if (not make_certificate(certs.a, L"kotuku-a", mode) or not make_certificate(certs.b, L"kotuku-b", mode)) {
      std::printf("certificate generation failed\n");
      return 1;
   }
   std::printf("key mode: %s\n  a %s\n  b %s\n", mode IS KeyMode::NAMED_PERSISTED ? "named persisted" :
      "ephemeral handle", certs.a.fingerprint.c_str(), certs.b.fingerprint.c_str());
   glCerts = &certs;
   if (udp_port) {
      std::fflush(stdout);
      return run_udp(udp_server, udp_port, udp_seconds);
   }

   auto list = scenarios();
   if (selected.empty()) for (auto &[name, fn] : list) selected.push_back(name);
   for (auto &name : selected) {
      auto it = list.find(name);
      if (it IS list.end()) { std::printf("unknown scenario %s\n", name.c_str()); glFailures++; continue; }
      it->second();
   }
   std::printf("\n%d failure(s)\n", glFailures);
   return glFailures ? 1 : 0;
}
