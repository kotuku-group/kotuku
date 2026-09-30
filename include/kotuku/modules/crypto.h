#pragma once

// Name:      crypto.h
// Copyright: Paul Manias © 2026
// Generator: idl-c

#include <kotuku/main.h>

#define MODVERSION_CRYPTO (1)

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
} // namespace
#else
namespace crypto {
extern ERR RandomBytes(const std::span<int8_t> &Output);
extern ERR SHA256(const std::span<const int8_t> &Input, const std::span<int8_t> &Output);
extern ERR Base64Encode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
extern ERR Base64Decode(const std::span<const int8_t> &Input, CC Mode, const std::span<int8_t> &Output, int *Result);
extern ERR ConstantTimeEqual(const std::span<const int8_t> &A, const std::span<const int8_t> &B, int *Equal);
extern ERR VerifyJWK(CSIG Algorithm, const std::string_view &N, const std::string_view &E, const std::span<const int8_t> &Message, const std::span<const int8_t> &Signature);
} // namespace
#endif // KOTUKU_STATIC