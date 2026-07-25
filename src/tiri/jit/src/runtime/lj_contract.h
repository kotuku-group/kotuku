// Runtime type-contract descriptor shared by the parser and VM.
// Copyright © 2026 Paul Manias

#pragma once

#include <cstdint>

inline constexpr uint8_t TIRI_CONTRACT_VERSION = 1;

enum class ContractBoundary : uint8_t {
   Parameter = 1,
   Result,
   Local,
   Upvalue,
   Global
};

enum class ContractDescriptorFlag : uint8_t {
   None = 0,
   DynamicCount = 1 << 0,
   Variadic = 1 << 1
};

enum class ContractEntryFlag : uint8_t {
   None = 0,
   Nullable = 1 << 0,
   Required = 1 << 1
};

[[nodiscard]] constexpr inline uint8_t contract_flag(ContractDescriptorFlag Flag) noexcept
{
   return uint8_t(Flag);
}

[[nodiscard]] constexpr inline uint8_t contract_flag(ContractEntryFlag Flag) noexcept
{
   return uint8_t(Flag);
}
