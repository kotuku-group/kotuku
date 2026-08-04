// Runtime type-contract descriptor shared by the parser and VM.
// Copyright © 2026 Paul Manias

#pragma once

#include "lj_obj.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

inline constexpr uint8_t TIRI_CONTRACT_VERSION = 3;
inline constexpr uint8_t TIRI_CONTRACT_GLOBAL_HINT_VERSION = 4;
inline constexpr uint8_t TIRI_CONTRACT_OBJECT_VERSION = 2;

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
   Required = 1 << 1,
   Const = 1 << 2,
   Initialising = 1 << 3,
   GlobalHint = 1 << 4
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
   CLASSID object_class_id = CLASSID::NIL;
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

// Compact, prototype-owned derivative of grouped portable descriptors.  Text offsets address the first text byte in
// the rooted GCstr descriptor; its preceding byte remains the canonical length.  Single-entry descriptors use the
// portable decoder directly because caching them would add lifetime memory without amortising batching work.

struct CachedRuntimeContractEntry {
   uint32_t object_class_id;
   uint16_t struct_offset;
   uint16_t label_offset;
   TiriType type;
   uint8_t flags;
   uint8_t position;
   AET array_element_type;
};

struct CachedRuntimeContractRecord {
   uint32_t bytecode_position;
   ContractBoundary boundary;
   uint8_t flags;
   uint8_t static_value_count;
   uint8_t contract_count;
   uint16_t entry_index;
};

struct RuntimeContractCache {
   uint32_t byte_size;
   uint16_t record_count;
   uint16_t entry_count;
};

[[nodiscard]] inline CachedRuntimeContractRecord * runtime_contract_cache_records(
   RuntimeContractCache *Cache) noexcept
{
   return (CachedRuntimeContractRecord *)(Cache + 1);
}

[[nodiscard]] inline const CachedRuntimeContractRecord * runtime_contract_cache_records(
   const RuntimeContractCache *Cache) noexcept
{
   return (const CachedRuntimeContractRecord *)(Cache + 1);
}

[[nodiscard]] inline CachedRuntimeContractEntry * runtime_contract_cache_entries(RuntimeContractCache *Cache) noexcept
{
   return (CachedRuntimeContractEntry *)(runtime_contract_cache_records(Cache) + Cache->record_count);
}

[[nodiscard]] inline const CachedRuntimeContractEntry * runtime_contract_cache_entries(
   const RuntimeContractCache *Cache) noexcept
{
   return (const CachedRuntimeContractEntry *)(runtime_contract_cache_records(Cache) + Cache->record_count);
}

[[nodiscard]] inline RuntimeContractCache * proto_contract_cache(GCproto *Proto) noexcept
{
   return Proto->contract_cache.get<RuntimeContractCache>();
}

[[nodiscard]] inline const RuntimeContractCache * proto_contract_cache(const GCproto *Proto) noexcept
{
   return Proto->contract_cache.get<const RuntimeContractCache>();
}

[[nodiscard]] constexpr inline bool contract_entry_is_const(const RuntimeContractEntry &Entry) noexcept
{
   return (Entry.flags & contract_flag(ContractEntryFlag::Const)) != 0;
}

[[nodiscard]] constexpr inline bool contract_entry_is_initialising(const RuntimeContractEntry &Entry) noexcept
{
   return (Entry.flags & contract_flag(ContractEntryFlag::Initialising)) != 0;
}

[[nodiscard]] constexpr inline bool contract_entry_is_global_hint(const RuntimeContractEntry &Entry) noexcept
{
   return (Entry.flags & contract_flag(ContractEntryFlag::GlobalHint)) != 0;
}

[[nodiscard]] constexpr inline bool contract_entry_is_variant(const RuntimeContractEntry &Entry) noexcept
{
   return Entry.type IS TiriType::Any;
    //or (Entry.type IS TiriType::Array and Entry.array_element_type IS AET::ANY);
}

[[nodiscard]] inline bool contract_descriptor_is_global_variant_initialiser(
   const RuntimeContractDescriptor &Descriptor, const GCstr *Name) noexcept
{
   if (Descriptor.boundary != ContractBoundary::Global or Descriptor.contract_count != 1) return false;

   const RuntimeContractEntry &entry = Descriptor.entries[0];
   return contract_entry_is_initialising(entry) and not contract_entry_is_const(entry) and
      contract_entry_is_variant(entry) and entry.label.size() IS Name->len and
      not std::memcmp(entry.label.data(), strdata(Name), Name->len);
}

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

   [[nodiscard]] bool read_uleb32(uint32_t &Value) noexcept
   {
      Value = 0;
      uint32_t shift = 0;
      for (uint32_t count = 0; count < 5 and this->cursor_ < this->end_; ++count) {
         uint8_t byte = *this->cursor_++;
         if (count IS 4 and (byte & 0xf0)) return false;
         Value |= uint32_t(byte & 0x7f) << shift;
         if (not (byte & 0x80)) return true;
         shift += 7;
      }
      return false;
   }

   [[nodiscard]] bool at_end() const noexcept
   {
      return this->cursor_ IS this->end_;
   }

private:
   const uint8_t *cursor_;
   const uint8_t *end_;
};

// Result is usable only when decoding succeeds.  Avoid clearing the fixed-capacity entry array here because callers
// already default-initialise it and every live entry is overwritten below.
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
   if (not reader.read_byte(version) or
       (version != TIRI_CONTRACT_OBJECT_VERSION and
        version != TIRI_CONTRACT_VERSION and version != TIRI_CONTRACT_GLOBAL_HINT_VERSION) or
       not reader.read_byte(boundary) or boundary < uint8_t(ContractBoundary::Parameter) or
       boundary > uint8_t(ContractBoundary::Global) or not reader.read_byte(Result.flags) or
       (Result.flags & ~(contract_flag(ContractDescriptorFlag::DynamicCount) |
          contract_flag(ContractDescriptorFlag::Variadic))) != 0 or
       not reader.read_byte(Result.static_value_count) or not reader.read_byte(Result.contract_count) or
       Result.contract_count > MAX_RETURN_TYPES) {
      return fail(RuntimeContractDecodeError::Descriptor);
   }
   Result.boundary = ContractBoundary(boundary);
   bool has_global_hint = false;

   for (uint8_t i = 0; i < Result.contract_count; ++i) {
      auto &entry = Result.entries[i];
      uint8_t type;
      uint32_t object_class_id = 0;
      uint8_t allowed_entry_flags = contract_flag(ContractEntryFlag::Nullable) |
         contract_flag(ContractEntryFlag::Required) | contract_flag(ContractEntryFlag::Const) |
         contract_flag(ContractEntryFlag::Initialising);
      if (version IS TIRI_CONTRACT_GLOBAL_HINT_VERSION) {
         allowed_entry_flags |= contract_flag(ContractEntryFlag::GlobalHint);
      }
      if (not reader.read_byte(type) or type > uint8_t(TiriType::Unknown) or
          not reader.read_byte(entry.flags) or
          (entry.flags & ~allowed_entry_flags) != 0 or
          not reader.read_byte(entry.position) or entry.position IS 0 or
          (version >= TIRI_CONTRACT_OBJECT_VERSION and not reader.read_uleb32(object_class_id)) or
          //(version >= TIRI_CONTRACT_VERSION and
          // (not reader.read_byte(array_element_type) or array_element_type > uint8_t(AET::MAX) or
          //  not reader.read_text(entry.array_struct_name))) or
          not reader.read_text(entry.struct_name) or not reader.read_text(entry.label)) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      entry.type = TiriType(type);
      entry.object_class_id = CLASSID(object_class_id);
      bool is_const = contract_entry_is_const(entry);
      bool is_initialising = contract_entry_is_initialising(entry);
      bool is_global_hint = contract_entry_is_global_hint(entry);
      if (is_global_hint) has_global_hint = true;
      if ((is_const and Result.boundary != ContractBoundary::Global) or
          (is_initialising and Result.boundary != ContractBoundary::Global) or
          (is_global_hint and
             (Result.boundary != ContractBoundary::Global or Result.static_value_count != 1 or
              Result.contract_count != 1 or entry.position != 1 or entry.label.empty() or is_const or
              is_initialising))) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      if (not entry.struct_name.empty() and entry.type != TiriType::Struct) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      if (entry.object_class_id != CLASSID::NIL and entry.type != TiriType::Object) {
         return fail(RuntimeContractDecodeError::Entry);
      }
   }

   if ((version IS TIRI_CONTRACT_GLOBAL_HINT_VERSION) != has_global_hint) {
      return fail(RuntimeContractDecodeError::Descriptor);
   }
   if (not reader.at_end()) return fail(RuntimeContractDecodeError::Descriptor);
   return true;
}
