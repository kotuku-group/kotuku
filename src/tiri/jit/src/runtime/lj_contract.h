// Runtime type-contract descriptor shared by the parser and VM.
// Copyright © 2026 Paul Manias

#pragma once

#include "lj_obj.h"

#include <array>
#include <cstdint>
#include <string_view>

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

struct RuntimeContractEntry {
   TiriType type = TiriType::Unknown;
   uint8_t flags = 0;
   uint8_t position = 0;
   std::string_view struct_name;
   std::string_view label;
};

struct RuntimeContractDescriptor {
   ContractBoundary boundary = ContractBoundary::Parameter;
   uint8_t flags = 0;
   uint8_t static_value_count = 0;
   uint8_t contract_count = 0;
   std::array<RuntimeContractEntry, MAX_RETURN_TYPES> entries{};

   [[nodiscard]] bool dynamic_count() const noexcept
   {
      return (this->flags & contract_flag(ContractDescriptorFlag::DynamicCount)) != 0;
   }

   [[nodiscard]] bool variadic() const noexcept
   {
      return (this->flags & contract_flag(ContractDescriptorFlag::Variadic)) != 0;
   }

   [[nodiscard]] const RuntimeContractEntry * entry_for(uint32_t Position) const noexcept
   {
      if (Position < this->contract_count) return &this->entries[Position];
      if (this->variadic() and this->contract_count > 0) return &this->entries[this->contract_count - 1];
      return nullptr;
   }
};

enum class RuntimeContractDecodeError : uint8_t {
   None,
   Descriptor,
   Entry
};

class RuntimeContractReader {
public:
   explicit RuntimeContractReader(const GCstr *Descriptor) noexcept
      : cursor_(reinterpret_cast<const uint8_t *>(strdata(Descriptor))),
        end_(this->cursor_ + Descriptor->len) {}

   [[nodiscard]] bool read_byte(uint8_t &Value) noexcept
   {
      if (this->cursor_ >= this->end_) return false;
      Value = *this->cursor_++;
      return true;
   }

   [[nodiscard]] bool read_text(std::string_view &Value) noexcept
   {
      uint8_t length;
      if (not this->read_byte(length) or size_t(this->end_ - this->cursor_) < length) return false;
      Value = std::string_view(reinterpret_cast<const char *>(this->cursor_), length);
      this->cursor_ += length;
      return true;
   }

   [[nodiscard]] bool at_end() const noexcept
   {
      return this->cursor_ IS this->end_;
   }

private:
   const uint8_t *cursor_;
   const uint8_t *end_;
};

[[nodiscard]] inline bool decode_runtime_contract(
   const GCstr *Descriptor, RuntimeContractDescriptor &Result,
   RuntimeContractDecodeError *Error = nullptr) noexcept
{
   if (Error) *Error = RuntimeContractDecodeError::None;
   auto fail = [Error](RuntimeContractDecodeError Value) {
      if (Error) *Error = Value;
      return false;
   };

   RuntimeContractReader reader(Descriptor);
   uint8_t version;
   uint8_t boundary;
   if (not reader.read_byte(version) or version != TIRI_CONTRACT_VERSION or
       not reader.read_byte(boundary) or boundary < uint8_t(ContractBoundary::Parameter) or
       boundary > uint8_t(ContractBoundary::Global) or not reader.read_byte(Result.flags) or
       (Result.flags & ~(contract_flag(ContractDescriptorFlag::DynamicCount) |
          contract_flag(ContractDescriptorFlag::Variadic))) != 0 or
       not reader.read_byte(Result.static_value_count) or not reader.read_byte(Result.contract_count) or
       Result.contract_count > MAX_RETURN_TYPES) {
      return fail(RuntimeContractDecodeError::Descriptor);
   }
   Result.boundary = ContractBoundary(boundary);

   for (uint8_t i = 0; i < Result.contract_count; ++i) {
      auto &entry = Result.entries[i];
      uint8_t type;
      if (not reader.read_byte(type) or type > uint8_t(TiriType::Unknown) or
          not reader.read_byte(entry.flags) or
          (entry.flags & ~(contract_flag(ContractEntryFlag::Nullable) |
             contract_flag(ContractEntryFlag::Required))) != 0 or
          not reader.read_byte(entry.position) or entry.position IS 0 or
          not reader.read_text(entry.struct_name) or not reader.read_text(entry.label)) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      entry.type = TiriType(type);
      if (not entry.struct_name.empty() and entry.type != TiriType::Struct) {
         return fail(RuntimeContractDecodeError::Entry);
      }
   }

   if (not reader.at_end()) return fail(RuntimeContractDecodeError::Descriptor);
   return true;
}
