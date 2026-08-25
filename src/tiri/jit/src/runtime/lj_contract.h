// Runtime type-contract descriptor shared by the parser and VM.
// Copyright © 2026 Paul Manias

#pragma once

#include "lj_obj.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

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
   GlobalHint = 1 << 4,
   Negated = 1 << 5
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
   AET array_element_type = AET::MAX;
   uint8_t flags = 0;
   uint8_t position = 0;
   CLASSID object_class_id = CLASSID::NIL;
   std::string_view constraint_name; // Structure name or canonical nested-array member identity.
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

// Compact, prototype-owned derivative of portable descriptors.  Text offsets address the first text byte in the
// rooted GCstr descriptor; its preceding byte remains the canonical length.  Both single-entry and grouped records
// share this representation so hot contract checks never need to decode their portable descriptor.

struct CachedRuntimeContractEntry {
   union {
      uint32_t object_class_id;
      uint16_t constraint_offset; // Structure name or canonical nested-array identity, selected by type.
   };
   uint16_t label_offset;
   TiriType type;
   AET array_element_type;
   uint8_t flags;
   uint8_t position;
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

// Environment-owned derivative of a persisted global contract.  Name and descriptor are raw pointers because the
// authoritative contracts table roots both strings for at least as long as this cache.  Empty slots have a null name.

struct CachedGlobalContractRecord {
   const GCstr *name;
   GCstr *descriptor;
   CachedRuntimeContractEntry entry;
};

struct GlobalContractCache {
   uint32_t byte_size;
   uint32_t count;
   uint32_t capacity;
   uint32_t reserved;
};

static_assert(sizeof(RuntimeContractEntry) IS 40, "runtime contract entries must remain compact");
static_assert(sizeof(CachedRuntimeContractEntry) IS 12, "cached runtime contract entries must remain compact");
static_assert(sizeof(CachedRuntimeContractRecord) IS 12, "cached runtime contract records must remain compact");
static_assert(sizeof(RuntimeContractCache) IS 8, "runtime contract cache header must remain compact");
static_assert(sizeof(CachedGlobalContractRecord) IS 32, "cached global contract records must remain compact");
static_assert(sizeof(GlobalContractCache) IS 16, "global contract cache header must remain compact");

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

[[nodiscard]] inline CachedGlobalContractRecord * global_contract_cache_records(
   GlobalContractCache *Cache) noexcept
{
   return (CachedGlobalContractRecord *)(Cache + 1);
}

[[nodiscard]] inline const CachedGlobalContractRecord * global_contract_cache_records(
   const GlobalContractCache *Cache) noexcept
{
   return (const CachedGlobalContractRecord *)(Cache + 1);
}

[[nodiscard]] inline GlobalContractCache * table_global_contract_cache(GCtab *Table) noexcept
{
   return Table->global_contract_cache.get<GlobalContractCache>();
}

[[nodiscard]] inline const GlobalContractCache * table_global_contract_cache(const GCtab *Table) noexcept
{
   return Table->global_contract_cache.get<const GlobalContractCache>();
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

// Compare public global binding identity while ignoring descriptor lifecycle metadata.

[[nodiscard]] inline bool global_contract_entries_equivalent(
   const RuntimeContractEntry &Left, const RuntimeContractEntry &Right) noexcept
{
   constexpr uint8_t identity_flags = contract_flag(ContractEntryFlag::Nullable) |
      contract_flag(ContractEntryFlag::Required) | contract_flag(ContractEntryFlag::Const) |
      contract_flag(ContractEntryFlag::Negated);
   if (Left.type != Right.type or (Left.flags & identity_flags) != (Right.flags & identity_flags)) return false;

   switch (Left.type) {
      case TiriType::Object:
         return Left.object_class_id IS Right.object_class_id;
      case TiriType::Struct:
         return Left.constraint_name IS Right.constraint_name;
      case TiriType::Array:
         return Left.array_element_type IS Right.array_element_type and
            Left.constraint_name IS Right.constraint_name;
      default:
         return true;
   }
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

[[nodiscard]] inline bool runtime_array_identity_is_canonical(std::string_view Identity) noexcept
{
   auto identifier = [](std::string_view Text) {
      if (Text.empty()) return false;
      bool first_alpha = (Text[0] >= 'a' and Text[0] <= 'z') or
         (Text[0] >= 'A' and Text[0] <= 'Z') or Text[0] IS '_';
      if (not first_alpha) return false;
      for (char character : Text.substr(1)) {
         bool alpha = (character >= 'a' and character <= 'z') or
            (character >= 'A' and character <= 'Z') or character IS '_';
         bool digit = character >= '0' and character <= '9';
         if (not alpha and not digit) return false;
      }
      return true;
   };
   auto member_valid = [&identifier](auto &Self, std::string_view Member, uint8_t Depth) -> bool {
      if (Depth >= 32 or Member.empty()) return false;
      if (Member.starts_with("array<") and Member.ends_with('>')) {
         return Self(Self, Member.substr(6, Member.size() - 7), uint8_t(Depth + 1));
      }
      if (Member.starts_with("struct<") and Member.ends_with('>')) {
         return identifier(Member.substr(7, Member.size() - 8));
      }
      constexpr std::array<std::string_view, 16> names = {
         "any", "array", "byte", "double", "float", "int", "int8", "int16", "int64", "obj",
         "str", "table", "uint", "uint8", "uint16", "uint64"
      };
      for (std::string_view name : names) if (Member IS name) return true;
      return false;
   };
   return Identity.starts_with("array<") and Identity.ends_with('>') and
      member_valid(member_valid, Identity.substr(6, Identity.size() - 7), 0);
}

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
   uint8_t boundary;
   if (not reader.read_byte(boundary) or boundary < uint8_t(ContractBoundary::Parameter) or
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
      uint32_t object_class_id = 0;
      std::string_view struct_name;
      std::string_view array_struct_name;
      uint8_t allowed_entry_flags = contract_flag(ContractEntryFlag::Nullable) |
         contract_flag(ContractEntryFlag::Required) | contract_flag(ContractEntryFlag::Const) |
         contract_flag(ContractEntryFlag::Initialising) | contract_flag(ContractEntryFlag::GlobalHint);
      allowed_entry_flags |= contract_flag(ContractEntryFlag::Negated);
      if (not reader.read_byte(type) or type > uint8_t(TiriType::Unknown) or
          not reader.read_byte(entry.flags) or
          (entry.flags & ~allowed_entry_flags) != 0 or
          not reader.read_byte(entry.position) or entry.position IS 0 or
          not reader.read_uleb32(object_class_id) or
          not reader.read_text(struct_name)) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      entry.type = TiriType(type);
      entry.array_element_type = AET::MAX;
      entry.object_class_id = CLASSID::NIL;
      entry.constraint_name = {};
      if (entry.type IS TiriType::Array) {
         uint8_t array_element_type;
         if (not reader.read_byte(array_element_type) or array_element_type >= uint8_t(AET::MAX) or
             not reader.read_text(array_struct_name)) {
            return fail(RuntimeContractDecodeError::Entry);
         }
         entry.array_element_type = AET(array_element_type);
         if (entry.array_element_type IS AET::ARRAY and not array_struct_name.empty() and
             not runtime_array_identity_is_canonical(array_struct_name)) {
            return fail(RuntimeContractDecodeError::Entry);
         }
      }
      if (not reader.read_text(entry.label)) return fail(RuntimeContractDecodeError::Entry);
      bool is_const = contract_entry_is_const(entry);
      bool is_initialising = contract_entry_is_initialising(entry);
      bool is_global_hint = contract_entry_is_global_hint(entry);
      if ((is_const and Result.boundary != ContractBoundary::Global) or
          (is_initialising and Result.boundary != ContractBoundary::Global) or
          (is_global_hint and
             (Result.boundary != ContractBoundary::Global or Result.static_value_count != 1 or
              Result.contract_count != 1 or entry.position != 1 or entry.label.empty() or is_const or
              is_initialising))) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      if (not struct_name.empty() and entry.type != TiriType::Struct) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      if (object_class_id != uint32_t(CLASSID::NIL) and entry.type != TiriType::Object) {
         return fail(RuntimeContractDecodeError::Entry);
      }
      if (entry.type IS TiriType::Array) {
         if (entry.array_element_type IS AET::STRUCT) {
            if (array_struct_name.empty()) return fail(RuntimeContractDecodeError::Entry);
         }
         else if (entry.array_element_type != AET::ARRAY and not array_struct_name.empty()) {
            return fail(RuntimeContractDecodeError::Entry);
         }
      }
      if (entry.type IS TiriType::Object) entry.object_class_id = CLASSID(object_class_id);
      else if (entry.type IS TiriType::Struct) entry.constraint_name = struct_name;
      else if (entry.type IS TiriType::Array) entry.constraint_name = array_struct_name;
   }

   if (not reader.at_end()) return fail(RuntimeContractDecodeError::Descriptor);
   return true;
}
