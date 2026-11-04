/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This code utilises BearSSL and the Linux kernel cryptographic random source on Linux, and the native CNG API on
Windows.

**********************************************************************************************************************

-MODULE-
Crypto: Provides cryptographic primitives for binary data.

BETA: This module is not finalised for production use.

The Crypto module provides a small set of cryptographic primitives for binary data.  All inputs are treated as raw
byte arrays and may contain null bytes.  Hashing and signature verification use BearSSL on Linux and CNG on Windows.
Certificate generation uses OpenSSL on Linux and CNG on Windows.  Random data is obtained from the host platform.
MD5 and SHA-1 are available only for compatibility uses such as
checksums, ETags, git object identifiers, UUID version 5 and HTTP digest authentication.  They are not suitable for
security purposes; use SHA-256 or SHA-512 for new designs.

Each input array argument is limited to 1 MiB (1048576 bytes).  Array results are written to caller-supplied buffers.
Certificate generation instead returns PEM and fingerprint strings supplied by the caller.

~WriteProtectedFile() is the exception to the otherwise in-memory API.  It is a small persistence primitive for
security-sensitive records that need owner-only access and atomic replacement.

-END-

*********************************************************************************************************************/

#define PRV_CRYPTO_MODULE

#include <kotuku/main.h>
#include <kotuku/modules/crypto.h>
#include <kotuku/modules/module.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef CRYPTO_PROTOCOL_OPENSSL
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

#include "crypto_internal.h"
#include "protocol_primitives.h"

static ERR MODInit(OBJECTPTR, struct CoreBase *);
static ERR MODExpunge(void);
static ERR MODOpen(OBJECTPTR);
static ERR init_hash(void);
static OBJECTPTR clHash = nullptr;

#include "crypto_def.c"

JUMPTABLE_CORE

//********************************************************************************************************************
// Upper limit for the size of any single binary input or output buffer.

constexpr size_t MAX_CRYPTO_INPUT = 1024 * 1024;

//********************************************************************************************************************

static bool valid_mode(CC Mode)
{
   return (Mode IS CC::STANDARD) or (Mode IS CC::URL);
}

//********************************************************************************************************************
// Returns true if the Input and Output buffers share any bytes.

static bool overlaps(std::span<const int8_t> Input, std::span<int8_t> Output)
{
   if (Input.empty() or Output.empty()) return false;

   auto a = uintptr_t(Input.data());
   auto b = uintptr_t(Output.data());
   return ((a <= b) and (b - a < Input.size())) or ((b < a) and (a - b < Output.size()));
}

//********************************************************************************************************************
// Returns the 6-bit value of a base64 character in the given alphabet, or -1 if the character is not a member.

static int digit(uint8_t Character, CC Mode)
{
   if ((Character >= 'A') and (Character <= 'Z')) return Character - 'A';
   if ((Character >= 'a') and (Character <= 'z')) return Character - 'a' + 26;
   if ((Character >= '0') and (Character <= '9')) return Character - '0' + 52;
   if (Character IS uint8_t((Mode IS CC::URL) ? '-' : '+')) return 62;
   if (Character IS uint8_t((Mode IS CC::URL) ? '_' : '/')) return 63;
   return -1;
}

//********************************************************************************************************************
// Validates that Input is canonically encoded and computes the size of its decoded form.  Padding is mandatory for
// STANDARD and prohibited for URL.  Non-zero trailing bits are rejected so that each value has one valid encoding.

static ERR decoded_size(std::span<const int8_t> Input, CC Mode, size_t *Size)
{
   if ((not valid_mode(Mode)) or (Input.size() > MAX_CRYPTO_INPUT)) return ERR::Args;

   // Padding can only occupy the final one or two characters.  Any other '=' is not a member of the alphabet, so it is
   // rejected by the character check.

   size_t count = Input.size();
   size_t padding = 0;
   if (Mode IS CC::STANDARD) {
      if (count % 4) return ERR::InvalidData;
      if ((count) and (Input[count - 1] IS '=')) {
         padding++;
         if (Input[count - 2] IS '=') padding++;
      }
   }
   else if (count % 4 IS 1) return ERR::InvalidData;

   size_t chars = count - padding;
   for (size_t i=0; i < chars; i++) {
      if (digit(uint8_t(Input[i]), Mode) < 0) return ERR::InvalidData;
   }

   // Reject non-zero bits in the final partial character

   if ((chars % 4 IS 2) and (digit(uint8_t(Input[chars - 1]), Mode) & 15)) return ERR::InvalidData;
   if ((chars % 4 IS 3) and (digit(uint8_t(Input[chars - 1]), Mode) & 3)) return ERR::InvalidData;

   *Size = (chars / 4) * 3 + ((chars % 4) ? (chars % 4) - 1 : 0);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;
   return init_hash();
}

static ERR MODOpen(OBJECTPTR Module)
{
   ((objModule *)Module)->setFunctionList(glFunctions);
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   if (clHash) { FreeResource(clHash); clHash = nullptr; }
   return ERR::Okay;
}

namespace crypto {

/*********************************************************************************************************************

-FUNCTION-
Hash: Computes a message digest in one operation.

Computes the digest selected by `Algorithm` and writes the binary result to `Output`.  `Input` may be empty.  The
required output length is 16 bytes for MD5, 20 for SHA-1, 32 for SHA-256 and 64 for SHA-512.

MD5 and SHA-1 are provided for compatibility with existing formats and protocols.  They are not suitable for security
purposes.  Use SHA-256 or SHA-512 for new designs.

`Output` must have exactly the required length and must not share memory with `Input`.  If hashing fails, `Output` is
zeroed by the platform backend.

-INPUT-
int(HASH) Algorithm: The message digest algorithm.
array(char) Input: The data to hash, up to 1 MiB.
^array(char) Output: Receives the binary digest.

-ERRORS-
Okay
Args: `Input` exceeds 1 MiB, `Output` has the wrong length, or the arrays overlap.
NoSupport: `Algorithm` is not supported.
Failed: The hashing provider failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR Hash(HASH Algorithm, const std::span<const int8_t> &Input, const std::span<int8_t> &Output)
{
   auto size = crypto_backend::digest_size(Algorithm);
   if (not size) return ERR::NoSupport;
   if ((Input.size() > MAX_CRYPTO_INPUT) or (Output.size() != size) or (overlaps(Input, Output))) return ERR::Args;
   return crypto_backend::hash(Algorithm, Input, Output);
}

/*********************************************************************************************************************

-FUNCTION-
HMAC: Computes a keyed-hash message authentication code in one operation.

Computes HMAC with the digest selected by `Algorithm` and writes the binary result to `Output`.  `Key` and `Input` may
be empty.  The required output length is the digest length: 16 bytes for MD5, 20 for SHA-1, 32 for SHA-256 and 64 for
SHA-512.

HMAC-MD5 and HMAC-SHA1 are provided for compatibility with existing protocols and must not be selected for new security
designs.  Prefer HMAC-SHA256 or HMAC-SHA512.

`Output` must have exactly the required length and must not share memory with `Key` or `Input`.  If the operation fails,
`Output` is zeroed by the platform backend.

-INPUT-
int(HASH) Algorithm: The message digest algorithm.
array(char) Key: The authentication key, up to 1 MiB.
array(char) Input: The data to authenticate, up to 1 MiB.
^array(char) Output: Receives the binary authentication code.

-ERRORS-
Okay
Args: An input exceeds 1 MiB, `Output` has the wrong length, or it overlaps an input.
NoSupport: `Algorithm` is not supported.
Failed: The cryptography provider failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR HMAC(HASH Algorithm, const std::span<const int8_t> &Key, const std::span<const int8_t> &Input,
   const std::span<int8_t> &Output)
{
   auto size = crypto_backend::digest_size(Algorithm);
   if (not size) return ERR::NoSupport;
   if ((Key.size() > MAX_CRYPTO_INPUT) or (Input.size() > MAX_CRYPTO_INPUT) or (Output.size() != size) or
       (overlaps(Key, Output)) or (overlaps(Input, Output))) return ERR::Args;
#ifdef CRYPTO_PROTOCOL_OPENSSL
   if (Algorithm IS HASH::SHA1) {
      auto result = crypto_protocol::hmac_sha1(
         std::span<const uint8_t>((const uint8_t *)Key.data(), Key.size()),
         std::span<const uint8_t>((const uint8_t *)Input.data(), Input.size()),
         std::span<uint8_t, 20>((uint8_t *)Output.data(), 20));
      if (result) return ERR::Okay;
      std::fill(Output.begin(), Output.end(), 0);
      return ERR::Failed;
   }
#endif
   return crypto_backend::hmac(Algorithm, Key, Input, Output);
}

/*********************************************************************************************************************

-FUNCTION-
CRC32: Computes the IEEE CRC-32 checksum of a byte array.

The result uses the polynomial and initial/final complement required by STUN FINGERPRINT.  An empty array produces
zero.  This checksum detects accidental corruption; it is not an authentication code.

-INPUT-
array(char) Input: The bytes to checksum, up to 1 MiB.
&uint Result: Receives the checksum.

-ERRORS-
Okay
Args: `Result` is null or `Input` exceeds 1 MiB.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR CRC32(const std::span<const int8_t> &Input, uint32_t *Result)
{
   if ((not Result) or (Input.size() > MAX_CRYPTO_INPUT)) return ERR::Args;
   *Result = crypto_protocol::crc32(std::span<const uint8_t>((const uint8_t *)Input.data(), Input.size()));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
GenerateP256Certificate: Creates a self-signed ECDSA P-256 certificate for a DTLS peer.

Each call generates a new private key and a certificate valid for 30 days.  The certificate and unencrypted private
key are returned as PEM strings.  Keep the private key secret and clear it when it is no longer needed.  The SHA-256
fingerprint is returned as 32 upper-case hexadecimal octets separated by colons, suitable for an SDP fingerprint.

-INPUT-
strview CommonName: Certificate subject name, one to 128 bytes with no null bytes.
^&string CertificatePEM: Receives the certificate in PEM format.
^&string PrivateKeyPEM: Receives the unencrypted private key in PEM format.
^&string Fingerprint: Receives the SHA-256 certificate fingerprint.

-ERRORS-
Okay
Args: The common name is invalid or output strings alias.
NullArgs: An output string pointer is null.
NoSupport: No certificate backend is available on this platform.
Failed: Certificate generation or encoding failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR GenerateP256Certificate(const std::string_view &CommonName, std::string *CertificatePEM,
   std::string *PrivateKeyPEM, std::string *Fingerprint)
{
   if (not CertificatePEM or not PrivateKeyPEM or not Fingerprint) return ERR::NullArgs;
   if (CertificatePEM IS PrivateKeyPEM or CertificatePEM IS Fingerprint or PrivateKeyPEM IS Fingerprint or
       CommonName.empty() or CommonName.size() > 128 or CommonName.find('\0') != std::string_view::npos) {
      return ERR::Args;
   }
   std::string common_name(CommonName);
   CertificatePEM->clear();
   std::fill(PrivateKeyPEM->begin(), PrivateKeyPEM->end(), '\0');
   PrivateKeyPEM->clear();
   Fingerprint->clear();
#ifdef _WIN32
   return crypto_backend::generate_p256_certificate(common_name, *CertificatePEM, *PrivateKeyPEM, *Fingerprint);
#elif defined(CRYPTO_PROTOCOL_OPENSSL)
   EVP_PKEY *key_raw = nullptr;
   X509 *certificate_raw = nullptr;
   auto generated = crypto_protocol::generate_p256_certificate(common_name, key_raw, certificate_raw);
   auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(key_raw, EVP_PKEY_free);
   auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(certificate_raw, X509_free);
   if (not generated) return ERR::Failed;

   auto cert_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
   auto key_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
   auto clear_key_bio = kt::Defer([&]() {
      if (not key_bio) return;
      char *data = nullptr;
      auto length = BIO_get_mem_data(key_bio.get(), &data);
      if (length > 0 and data) OPENSSL_cleanse(data, size_t(length));
   });
   if (not cert_bio or not key_bio or
       PEM_write_bio_X509(cert_bio.get(), certificate.get()) != 1 or
       PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
      return ERR::Failed;
   }

   std::array<uint8_t, 32> digest{};
   unsigned int digest_size = 0;
   if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &digest_size) != 1 or
       digest_size != digest.size()) return ERR::Failed;
   static constexpr char HEX[] = "0123456789ABCDEF";
   std::string fingerprint;
   fingerprint.reserve(95);
   for (size_t i = 0; i < digest.size(); ++i) {
      if (i) fingerprint += ':';
      fingerprint += HEX[digest[i] >> 4];
      fingerprint += HEX[digest[i] & 15];
   }

   char *cert_data = nullptr, *key_data = nullptr;
   auto cert_length = BIO_get_mem_data(cert_bio.get(), &cert_data);
   auto key_length = BIO_get_mem_data(key_bio.get(), &key_data);
   if (cert_length <= 0 or key_length <= 0 or not cert_data or not key_data) return ERR::Failed;
   CertificatePEM->assign(cert_data, size_t(cert_length));
   PrivateKeyPEM->assign(key_data, size_t(key_length));
   *Fingerprint = std::move(fingerprint);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FUNCTION-
RandomBytes: Fills a byte array with cryptographically secure random data.

Fills `Output` with bytes from the host's cryptographically secure random number generator.  The size of `Output`
determines how many bytes are generated.  An empty array is accepted and left unchanged.

If the generator fails, `Output` is zeroed.  The content of `Output` must not be used unless the function returns
`ERR::Okay`.

-INPUT-
^array(char) Output: The array to fill, up to 1 MiB.

-ERRORS-
Okay
Args: `Output` exceeds 1 MiB.
Failed: The random number generator failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR RandomBytes(const std::span<int8_t> &Output)
{
   if (Output.size() > MAX_CRYPTO_INPUT) return ERR::Args;

   return crypto_backend::random(Output);
}

/*********************************************************************************************************************

-FUNCTION-
SHA256: Computes the SHA-256 digest of a byte array.

Computes the SHA-256 digest of `Input` and writes the 32-byte binary result to `Output`.  `Input` may be empty.  The
digest is not converted to text; use ~Base64Encode() if a text representation is required.

`Output` must be exactly 32 bytes long and must not share memory with `Input`.  If hashing fails, `Output` is zeroed.

-INPUT-
array(char) Input: The data to hash, up to 1 MiB.
^array(char) Output: A 32-byte array that receives the digest.

-ERRORS-
Okay
Args: `Input` exceeds 1 MiB, `Output` is not 32 bytes long, or the arrays overlap.
Failed: The hashing provider failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR SHA256(const std::span<const int8_t> &Input, const std::span<int8_t> &Output)
{
   return Hash(HASH::SHA256, Input, Output);
}

/*********************************************************************************************************************

-FUNCTION-
Base64Encode: Encodes binary data as base64 or base64url text.

Encodes `Input` as text, using the alphabet and padding rules selected by `Mode`:

!CC

To find the required output size, call the function with an empty `Output` array.  `Result` receives the size and
nothing is written.  Otherwise `Output` must be at least that size; the encoded text is written from the start of the
array and `Result` receives its length.  The text is not null-terminated.

`Result` is set to zero if the function fails.

-INPUT-
array(char) Input: The data to encode, up to 1 MiB.
int(CC) Mode: The encoding to produce.
^array(char) Output: Receives the encoded text.  Pass an empty array to query the required size.
&int Result: Receives the length of the encoded text, or the required size if `Output` is empty.

-ERRORS-
Okay
Args: `Result` is `NULL`, `Mode` is invalid, `Input` exceeds 1 MiB, or `Input` and `Output` overlap.
LowCapacity: `Output` is too small to hold the encoded text.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR Base64Encode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output,
   int *Result)
{
   constexpr char standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   constexpr char url[]      = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

   if (Result) *Result = 0;

   if ((not Result) or (not valid_mode(Mode)) or (Input.size() > MAX_CRYPTO_INPUT) or (overlaps(Input, Output))) {
      return ERR::Args;
   }

   size_t groups   = Input.size() / 3;
   size_t rem      = Input.size() % 3;
   size_t required = groups * 4 + (rem ? ((Mode IS CC::STANDARD) ? 4 : rem + 1) : 0);

   *Result = int(required);
   if (Output.empty()) return ERR::Okay; // Sizing query

   if (Output.size() < required) {
      *Result = 0;
      return ERR::LowCapacity;
   }

   const char *alphabet = (Mode IS CC::STANDARD) ? standard : url;
   size_t j = 0;
   for (size_t i=0; i < Input.size(); i += 3) {
      uint32_t a = uint8_t(Input[i]);
      uint32_t b = (i + 1 < Input.size()) ? uint8_t(Input[i + 1]) : 0;
      uint32_t c = (i + 2 < Input.size()) ? uint8_t(Input[i + 2]) : 0;

      Output[j++] = alphabet[a >> 2];
      Output[j++] = alphabet[((a & 3) << 4) | (b >> 4)];

      if (i + 1 < Input.size()) Output[j++] = alphabet[((b & 15) << 2) | (c >> 6)];
      else if (Mode IS CC::STANDARD) Output[j++] = '=';

      if (i + 2 < Input.size()) Output[j++] = alphabet[c & 63];
      else if (Mode IS CC::STANDARD) Output[j++] = '=';
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
Base64Decode: Decodes base64 or base64url text to binary data.

Decodes `Input` and writes the binary result to `Output`.  `Mode` selects the alphabet and padding rules that `Input`
must follow:

!CC

Decoding is strict, and only the canonical encoding of a value is accepted.  `Input` is rejected if it contains
whitespace, line breaks or characters outside the selected alphabet, if its padding is missing or misplaced in
`STANDARD` mode, if it contains padding in `URL` mode, or if the unused bits of its final character are not zero.

To find the required output size, call the function with an empty `Output` array.  `Result` receives the size and
nothing is written.  Otherwise `Output` must be at least that size; the decoded data is written from the start of the
array and `Result` receives its length.

`Input` is fully validated before any output is written.  If the function fails, `Result` is set to zero and no decoded
data is left in `Output`.

-INPUT-
array(char) Input: The text to decode, up to 1 MiB.
int(CC) Mode: The encoding used by `Input`.
^array(char) Output: Receives the decoded data.  Pass an empty array to query the required size.
&int Result: Receives the length of the decoded data, or the required size if `Output` is empty.

-ERRORS-
Okay
Args: `Result` is `NULL`, `Mode` is invalid, `Input` exceeds 1 MiB, or `Input` and `Output` overlap.
InvalidData: `Input` is not a canonical encoding in the selected alphabet.
LowCapacity: `Output` is too small to hold the decoded data.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR Base64Decode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output,
   int *Result)
{
   if (Result) *Result = 0;
   if ((not Result) or (overlaps(Input, Output))) return ERR::Args;

   size_t required = 0;
   if (auto error = decoded_size(Input, Mode, &required); error != ERR::Okay) return error;

   *Result = int(required);
   if (Output.empty()) return ERR::Okay; // Sizing query

   if (Output.size() < required) {
      *Result = 0;
      return ERR::LowCapacity;
   }

   // Input has been validated, so every character before the padding is a member of the alphabet and the unused bits
   // of the final character are zero.

   uint32_t bits = 0;
   int bit_count = 0;
   size_t j = 0;
   for (auto ch : Input) {
      if (ch IS '=') break;
      bits = (bits << 6) | uint32_t(digit(uint8_t(ch), Mode));
      bit_count += 6;
      if (bit_count >= 8) {
         bit_count -= 8;
         Output[j++] = int8_t(bits >> bit_count);
         bits &= (1u << bit_count) - 1;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ConstantTimeEqual: Compares two byte arrays in constant time.

Compares `A` with `B` and sets `Equal` to `1` if they are identical, otherwise `0`.  When the arrays are the same
length, the time taken depends only on that length and not on the content.  This makes the function suitable for
comparing secret values such as message authentication codes and access tokens, where a timing difference could reveal
how many leading bytes matched.

Array lengths are not treated as secret.  Arrays of different lengths are reported as unequal without comparing their
content.  Two empty arrays are equal.

-INPUT-
array(char) A: The first array, up to 1 MiB.
array(char) B: The second array, up to 1 MiB.
&int Equal: Receives `1` if the arrays are identical, otherwise `0`.

-ERRORS-
Okay: The comparison completed and the outcome is in `Equal`.
Args: `Equal` is `NULL`, or either array exceeds 1 MiB.

-TAGS-
pure-query, thread-safe

-END-

*********************************************************************************************************************/

ERR ConstantTimeEqual(const std::span<const int8_t> &A, const std::span<const int8_t> &B, int *Equal)
{
   if (Equal) *Equal = 0;
   if ((not Equal) or (A.size() > MAX_CRYPTO_INPUT) or (B.size() > MAX_CRYPTO_INPUT)) return ERR::Args;

   if (A.size() != B.size()) return ERR::Okay;

   if (A.empty()) {
      *Equal = 1;
      return ERR::Okay;
   }

   volatile uint32_t difference = 0;
   for (size_t i=0; i < A.size(); i++) difference = difference | uint32_t(uint8_t(A[i]) ^ uint8_t(B[i]));
   *Equal = (difference IS 0) ? 1 : 0;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
VerifyJWK: Verifies an RS256 signature against an RSA public key in JSON Web Key format.

Checks that `Signature` is a valid RS256 signature of `Message` for an RSA public key.  The key is supplied as the `n`
and `e` members of a JSON Web Key (JWK), which encode the modulus and public exponent in unpadded base64url.

The key is rejected with `ERR::InvalidData` unless all of the following are true:

<list type="bullet">
<li>`N` and `E` are canonical unpadded base64url values with no leading zero bytes.</li>
<li>The modulus is odd, and its length is a multiple of 8 bits between 2048 and 4096 bits.</li>
<li>The public exponent is odd and between 3 and 2^32-1.</li>
<li>`Signature` is the same length as the modulus.</li>
</list>

The function examines only the key material.  The caller is responsible for parsing the JWK and checking its other
members before calling this function, including:

<list type="bullet">
<li>Confirming that `kty` is `RSA`.</li>
<li>Confirming that `alg`, `use` and `key_ops`, where present, permit RS256 signature verification.</li>
<li>Rejecting a JWK that contains duplicate members or private key members such as `d`.</li>
<li>Selecting the key unambiguously, for example by matching its `kid` member.</li>
</list>

A successful result proves only that `Message` was signed with the private key that matches this public key.  It
does not validate token claims such as the issuer, audience or expiry time, and it does not establish whether the key
should be trusted.

To verify a JSON Web Token, pass the encoded header and payload joined by a period, exactly as received, in `Message`.
Decode the token's base64url signature segment and pass the raw bytes in `Signature`.

-INPUT-
int(CSIG) Algorithm: The signature algorithm.  Only `RS256` is supported.
strview N: The JWK `n` member, containing the RSA modulus.
strview E: The JWK `e` member, containing the RSA public exponent.
array(char) Message: The signed data, up to 1 MiB.
array(char) Signature: The raw signature bytes.

-ERRORS-
Okay: The signature is valid.
Mismatch: The signature is not valid for `Message` and the given key.
InvalidData: The key or signature does not meet the requirements listed above.
NoSupport: `Algorithm` is not `RS256`.
Args: `Message` exceeds 1 MiB.
Failed: The cryptography provider failed to complete the verification.

-TAGS-
pure-query, thread-safe

-END-

*********************************************************************************************************************/

ERR VerifyJWK(CSIG Algorithm, const std::string_view &N, const std::string_view &E,
   const std::span<const int8_t> &Message, const std::span<const int8_t> &Signature)
{
   if (Algorithm != CSIG::RS256) return ERR::NoSupport;

   if (Message.size() > MAX_CRYPTO_INPUT) return ERR::Args;

   // Reject oversized key material before decoding.  683 and 6 characters are the unpadded base64url lengths of a
   // 512-byte modulus and a 4-byte exponent.

   if ((N.size() > 683) or (E.size() > 6) or (Signature.size() > 512)) return ERR::InvalidData;

   // Decode the modulus and exponent

   auto n_bytes = std::span<const int8_t>((const int8_t *)N.data(), N.size());
   auto e_bytes = std::span<const int8_t>((const int8_t *)E.data(), E.size());

   size_t n_size = 0, e_size = 0;
   if ((decoded_size(n_bytes, CC::URL, &n_size) != ERR::Okay) or
       (decoded_size(e_bytes, CC::URL, &e_size) != ERR::Okay)) return ERR::InvalidData;

   if ((n_size < 256) or (n_size > 512) or (e_size < 1) or (e_size > 4) or (Signature.size() != n_size)) {
      return ERR::InvalidData;
   }

   std::array<uint8_t, 512> modulus{};
   std::array<uint8_t, 4> exponent{};
   auto modulus_out  = std::span<int8_t>((int8_t *)modulus.data(), n_size);
   auto exponent_out = std::span<int8_t>((int8_t *)exponent.data(), e_size);

   int written = 0;
   if ((Base64Decode(n_bytes, CC::URL, modulus_out, &written) != ERR::Okay) or
       (Base64Decode(e_bytes, CC::URL, exponent_out, &written) != ERR::Okay)) return ERR::InvalidData;

   // The modulus must be odd with its top bit set (i.e. no leading zero bytes); the exponent must be odd and have no
   // leading zero bytes.

   if (((modulus[0] & 128) IS 0) or ((modulus[n_size - 1] & 1) IS 0) or
       (exponent[0] IS 0) or ((exponent[e_size - 1] & 1) IS 0)) return ERR::InvalidData;

   uint64_t exponent_value = 0;
   for (size_t i=0; i < e_size; i++) exponent_value = (exponent_value << 8) | exponent[i];
   if (exponent_value < 3) return ERR::InvalidData;

   return crypto_backend::verify_rs256(std::span<const uint8_t>(modulus.data(), n_size),
      std::span<const uint8_t>(exponent.data(), e_size), Message, Signature);
}

/*********************************************************************************************************************

-FUNCTION-
WriteProtectedFile: Atomically writes an owner-only file.

Writes `Data` to a new temporary file in the destination directory, flushes it to durable storage, restricts access
to the current user and atomically replaces `Path`.  On systems that support directory synchronisation, the containing
directory is also flushed before the function returns.  Existing readers may continue to observe the old file while
new readers observe the replacement.

The destination must resolve to a host filesystem path.  The function does not create missing parent directories.
Temporary files are removed after a failed write and are never returned to the caller.  A failed call leaves an
existing destination unchanged unless the platform reports a failure after its atomic replacement operation has
already completed.

On Linux, the resulting mode is `0600`.  On Windows, the temporary file is created with a protected DACL granting full
access only to the current process user, and that descriptor is retained by the replacement file.

-INPUT-
strview Path: Destination file path.
array(char) Data: Complete file content, up to 1 MiB.

-ERRORS-
Okay
Args: `Path` is empty or `Data` exceeds 1 MiB.
ResolvePath: `Path` cannot be resolved to a host path.
NoPermission: The directory or destination cannot be accessed with the required protection.
OutOfSpace: The data cannot be written because storage is full.
Failed: A temporary file, flush or atomic replacement operation failed.

-TAGS-
thread-safe

-END-

*********************************************************************************************************************/

ERR WriteProtectedFile(const std::string_view &Path, const std::span<const int8_t> &Data) {
   if (Path.empty() or (Data.size() > MAX_CRYPTO_INPUT)) return ERR::Args;

   std::string resolved;
   if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved) != ERR::Okay) return ERR::ResolvePath;
   return crypto_backend::write_protected_file(resolved, Data);
}

} // namespace

//********************************************************************************************************************

#include "class_hash.cpp"

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_crypto_module() { return &ModHeader; }
