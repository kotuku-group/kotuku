#pragma once

// Name:      crypto.h
// Copyright: Paul Manias © 2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_CRYPTO (1)

class objHash;

// Canonical base64 alphabet and padding mode.

enum class CC : int {
   NIL = 0,
   STANDARD = 1,
   URL = 2,
};

// Supported public signature algorithms.

enum class CSIG : int {
   NIL = 0,
   RS256 = 1,
};

// Supported message digest algorithms.

enum class HASH : int {
   NIL = 0,
   MD5 = 1,
   SHA1 = 2,
   SHA256 = 3,
   SHA512 = 4,
};

// Hash class definition

#define VER_HASH (1.000000)

// Hash methods

namespace hsh {
struct Update { std::span<const int8_t> Data; static const AC id = AC(-1); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };
struct Digest { std::span<int8_t> Output; static const AC id = AC(-2); ERR call(OBJECTPTR Object) { return Action(id, Object, this); } };

} // namespace

class objHash : public Object {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::HASH;
   static constexpr CSTRING CLASS_NAME = "Hash";

   using create = kt::Create<objHash>;
   objHash(objMetaClass *pClass, OBJECTID pUID) noexcept : Object(pClass, pUID) {}

   HASH Algorithm;    // Message digest algorithm selected before initialisation.

   // Action stubs

   inline ERR init() noexcept { return InitObject(this); }
   inline ERR reset() noexcept { return Action(AC::Reset, this, nullptr); }
   inline ERR update(std::span<const int8_t> Data) noexcept {
      struct hsh::Update args = { Data };
      return Action(AC(-1), this, &args);
   }
   inline ERR digest(std::span<int8_t> Output) noexcept {
      struct hsh::Digest args = { Output };
      return Action(AC(-2), this, &args);
   }

   // Customised field getting

   inline ERR getAlgorithm(HASH &Value) noexcept {
      Value = this->Algorithm;
      return ERR::Okay;
   }


   // Customised field setting

   inline ERR setAlgorithm(const HASH Value) noexcept {
      if (this->initialised()) return ERR::ImmutableField;
      this->Algorithm = Value;
      return ERR::Okay;
   }

   inline ERR setKey(std::span<const int8_t> Value) noexcept {
      auto field = &this->Class->Dictionary[1];
      return field->WriteValue(this, field, 0x01101608, &Value);
   }

};

#ifdef KOTUKU_STATIC
#define JUMPTABLE_CRYPTO [[maybe_unused]] static struct CryptoBase *CryptoBase = nullptr;
#else
#define JUMPTABLE_CRYPTO struct CryptoBase *CryptoBase = nullptr;
#endif

struct CryptoBase {
#ifndef KOTUKU_STATIC
   ERR (*_RandomBytes)(const std::span<int8_t> &Output);
   ERR (*_SHA256)(const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
   ERR (*_Base64Encode)(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
   ERR (*_Base64Decode)(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
   ERR (*_ConstantTimeEqual)(const std::span<const int8_t> &A, const std::span<const int8_t> &B, int *Equal);
   ERR (*_VerifyJWK)(CSIG Algorithm, const std::string_view &N, const std::string_view &E, const std::span<const int8_t> &Message, const std::span<const int8_t> &Signature);
   ERR (*_WriteProtectedFile)(const std::string_view &Path, const std::span<const int8_t> &Data);
   ERR (*_Hash)(HASH Algorithm, const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
   ERR (*_HMAC)(HASH Algorithm, const std::span<const int8_t> &Key, const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
#endif // KOTUKU_STATIC
};

#if !defined(KOTUKU_STATIC) and !defined(PRV_CRYPTO_MODULE)
extern struct CryptoBase *CryptoBase;
namespace crypto {
inline ERR RandomBytes(const std::span<int8_t> &Output) { return CryptoBase->_RandomBytes(Output); }
inline ERR SHA256(const std::span<const int8_t> &Input, const std::span<int8_t> &Output) { return CryptoBase->_SHA256(Input,Output); }
inline ERR Base64Encode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result) { return CryptoBase->_Base64Encode(Input,Mode,Output,Result); }
inline ERR Base64Decode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result) { return CryptoBase->_Base64Decode(Input,Mode,Output,Result); }
inline ERR ConstantTimeEqual(const std::span<const int8_t> &A, const std::span<const int8_t> &B, int *Equal) { return CryptoBase->_ConstantTimeEqual(A,B,Equal); }
inline ERR VerifyJWK(CSIG Algorithm, const std::string_view &N, const std::string_view &E, const std::span<const int8_t> &Message, const std::span<const int8_t> &Signature) { return CryptoBase->_VerifyJWK(Algorithm,N,E,Message,Signature); }
inline ERR WriteProtectedFile(const std::string_view &Path, const std::span<const int8_t> &Data) { return CryptoBase->_WriteProtectedFile(Path,Data); }
inline ERR Hash(HASH Algorithm, const std::span<const int8_t> &Input, const std::span<int8_t> &Output) { return CryptoBase->_Hash(Algorithm,Input,Output); }
inline ERR HMAC(HASH Algorithm, const std::span<const int8_t> &Key, const std::span<const int8_t> &Input, const std::span<int8_t> &Output) { return CryptoBase->_HMAC(Algorithm,Key,Input,Output); }
} // namespace
#else
namespace crypto {
extern ERR RandomBytes(const std::span<int8_t> &Output);
extern ERR SHA256(const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
extern ERR Base64Encode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
extern ERR Base64Decode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
extern ERR ConstantTimeEqual(const std::span<const int8_t> &A, const std::span<const int8_t> &B, int *Equal);
extern ERR VerifyJWK(CSIG Algorithm, const std::string_view &N, const std::string_view &E, const std::span<const int8_t> &Message, const std::span<const int8_t> &Signature);
extern ERR WriteProtectedFile(const std::string_view &Path, const std::span<const int8_t> &Data);
extern ERR Hash(HASH Algorithm, const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
extern ERR HMAC(HASH Algorithm, const std::span<const int8_t> &Key, const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
} // namespace
#endif // KOTUKU_STATIC