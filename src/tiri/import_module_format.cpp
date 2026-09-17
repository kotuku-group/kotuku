#include <kotuku/main.h>

#include "import_module_format.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <tuple>

namespace tiri::import_cache {
namespace {

constexpr std::array<uint8_t, 8> MODULE_MAGIC = { 'T', 'I', 'R', 'I', 'M', 'O', 'D', '1' };
constexpr size_t HEADER_SIZE = 8 + 4 + 4 + 4 + 8 + (cache::DIGEST_SIZE * 3);

//********************************************************************************************************************
// Serialises primitive values into the module cache's bounded little-endian byte representation.

class Encoder {
public:
   explicit Encoder(size_t Limit = std::numeric_limits<size_t>::max()) : limit_(Limit) { }

   std::string Bytes;
   cache::FormatError Error = cache::FormatError::OKAY;

   void byte(uint8_t Value) { if (available(1)) Bytes.push_back(char(Value)); }

   void boolean(bool Value) { byte(Value ? 1 : 0); }

   void u16(uint16_t Value) {
      if (not available(2)) return;
      for (int shift = 0; shift < 16; shift += 8) Bytes.push_back(char(uint8_t(Value >> shift)));
   }

   void u32(uint32_t Value) {
      if (not available(4)) return;
      for (int shift = 0; shift < 32; shift += 8) Bytes.push_back(char(uint8_t(Value >> shift)));
   }

   void u64(uint64_t Value) {
      if (not available(8)) return;
      for (int shift = 0; shift < 64; shift += 8) Bytes.push_back(char(uint8_t(Value >> shift)));
   }

   void i64(int64_t Value) { u64(std::bit_cast<uint64_t>(Value)); }

   void string(std::string_view Value) {
      if (Value.size() > cache::MAX_STRING_SIZE) {
         Error = cache::FormatError::STRING_LIMIT;
         return;
      }
      if (not available(4 + Value.size())) return;
      u32(uint32_t(Value.size()));
      Bytes.append(Value);
   }

   void digest(const cache::Digest &Value) {
      if (available(Value.size())) Bytes.append((const char *)Value.data(), Value.size());
   }

private:
   size_t limit_;

   bool available(size_t Size) {
      if (Error != cache::FormatError::OKAY) return false;
      if ((Bytes.size() > limit_) or (Size > limit_ - Bytes.size())) {
         Error = cache::FormatError::SIZE_LIMIT;
         return false;
      }
      return true;
   }
};

//********************************************************************************************************************
// Reads primitive values from the module cache format while retaining the first validation failure.

class Decoder {
public:
   explicit Decoder(std::string_view Input) : bytes_(Input) { }

   cache::FormatError Error = cache::FormatError::OKAY;
   size_t Position = 0;

   uint8_t byte() { return available(1) ? uint8_t(bytes_[Position++]) : 0; }

   bool boolean() {
      auto value = byte();
      if (value > 1) Error = cache::FormatError::INVALID_ENUM;
      return value != 0;
   }

   uint16_t u16() {
      if (not available(2)) return 0;
      uint16_t value = 0;
      for (int shift = 0; shift < 16; shift += 8) value |= uint16_t(byte()) << shift;
      return value;
   }

   uint32_t u32() {
      if (not available(4)) return 0;
      uint32_t value = 0;
      for (int shift = 0; shift < 32; shift += 8) value |= uint32_t(byte()) << shift;
      return value;
   }

   uint64_t u64() {
      if (not available(8)) return 0;
      uint64_t value = 0;
      for (int shift = 0; shift < 64; shift += 8) value |= uint64_t(byte()) << shift;
      return value;
   }

   int64_t i64() { return std::bit_cast<int64_t>(u64()); }

   std::string string() {
      auto size = u32();
      if (Error != cache::FormatError::OKAY) return {};
      if (size > cache::MAX_STRING_SIZE) {
         Error = cache::FormatError::STRING_LIMIT;
         return {};
      }
      if (not available(size)) return {};
      std::string result(bytes_.substr(Position, size));
      Position += size;
      return result;
   }

   cache::Digest digest() {
      cache::Digest result = {};
      if (not available(result.size())) return result;
      std::memcpy(result.data(), bytes_.data() + Position, result.size());
      Position += result.size();
      return result;
   }
   [[nodiscard]] size_t size() const { return bytes_.size(); }

private:
   std::string_view bytes_;

   bool available(size_t Size) {
      if ((Size > bytes_.size()) or (Position > bytes_.size() - Size)) {
         Error = cache::FormatError::TRUNCATED;
         return false;
      }
      return true;
   }
};

//********************************************************************************************************************
// Confirms that a decoded enumeration value lies within its contiguous valid range.

template <class Value> bool valid_enum(Value ValueToCheck, Value First, Value Last)
{
   return ValueToCheck >= First and ValueToCheck <= Last;
}

//********************************************************************************************************************
// Decodes a length-prefixed record collection while enforcing the interface record limit.

template <class Record, class Read>
void decode_records(Decoder &Input, std::vector<Record> &Output, Read Reader)
{
   auto count = Input.u32();
   if (Input.Error != cache::FormatError::OKAY) return;
   if (count > MAX_INTERFACE_RECORDS) {
      Input.Error = cache::FormatError::COUNT_LIMIT;
      return;
   }

   Output.reserve(count);
   for (uint32_t i = 0; (i < count) and (Input.Error IS cache::FormatError::OKAY); ++i) {
      Output.push_back(Reader(Input));
   }
}

//********************************************************************************************************************
// Writes the stable source attributes used to validate a cached module identity.

void encode_source(Encoder &Output, const cache::SourceIdentity &Source)
{
   Output.string(Source.ResolvedPath);
   Output.u64(Source.Size);
   Output.i64(Source.ModifiedHint);
   Output.digest(Source.ContentDigest);
}

//********************************************************************************************************************
// Reconstructs a source identity from its stable on-disk representation.

cache::SourceIdentity decode_source(Decoder &Input)
{
   return { Input.string(), Input.u64(), Input.i64(), Input.digest() };
}

//********************************************************************************************************************
// Serialises a value descriptor, including any nested array type information.

void encode_value(Encoder &Output, const ValueDescriptor &Value)
{
   Output.byte(uint8_t(Value.Kind));
   Output.byte(uint8_t(Value.Proof));
   Output.boolean(Value.Nullable);
   Output.byte(Value.Array.Storage);
   Output.string(Value.ObjectClass);
   Output.string(Value.Structure);
   Output.byte(uint8_t(Value.Array.ElementKind));
   Output.string(Value.Array.ObjectClass);
   Output.string(Value.Array.Structure);
   Output.string(Value.Array.NestedIdentity);
}

//********************************************************************************************************************
// Decodes and validates a value descriptor from the interface stream.

ValueDescriptor decode_value(Decoder &Input)
{
   ValueDescriptor result;
   result.Kind          = ValueKind(Input.byte());
   result.Proof         = ProofKind(Input.byte());
   result.Nullable      = Input.boolean();
   result.Array.Storage = Input.byte();
   result.ObjectClass   = Input.string();
   result.Structure     = Input.string();
   result.Array.ElementKind = ValueKind(Input.byte());
   result.Array.ObjectClass = Input.string();
   result.Array.Structure = Input.string();
   result.Array.NestedIdentity = Input.string();
   if (not valid_enum(result.Kind, ValueKind::UNKNOWN, ValueKind::ANY) or
       not valid_enum(result.Proof, ProofKind::ADVISORY, ProofKind::TRUSTED) or
       not valid_enum(result.Array.ElementKind, ValueKind::UNKNOWN, ValueKind::ANY)) {
      Input.Error = cache::FormatError::INVALID_ENUM;
   }
   return result;
}

//********************************************************************************************************************
// Serialises a callable's parameter and result contracts.

void encode_callable(Encoder &Output, const CallableDescriptor &Callable)
{
   Output.u32(uint32_t(Callable.Parameters.size()));
   for (const auto &parameter : Callable.Parameters) {
      encode_value(Output, parameter.Value);
      Output.string(parameter.Label);
      Output.byte(parameter.Position);
      Output.boolean(parameter.Required);
      Output.boolean(parameter.IsConst);
   }
   Output.u32(uint32_t(Callable.Results.size()));
   for (const auto &result : Callable.Results) encode_value(Output, result);
   Output.u16(Callable.DeclaredResults);
   Output.boolean(Callable.VariadicParameters);
   Output.boolean(Callable.VariadicResults);
}

//********************************************************************************************************************
// Reconstructs a callable contract from the interface stream.

CallableDescriptor decode_callable(Decoder &Input)
{
   CallableDescriptor result;
   decode_records<ContractDescriptor>(Input, result.Parameters, [](Decoder &Value) {
      ContractDescriptor parameter;
      parameter.Value    = decode_value(Value);
      parameter.Label    = Value.string();
      parameter.Position = Value.byte();
      parameter.Required = Value.boolean();
      parameter.IsConst  = Value.boolean();
      return parameter;
   });
   decode_records<ValueDescriptor>(Input, result.Results, decode_value);
   result.DeclaredResults = Input.u16();
   result.VariadicParameters = Input.boolean();
   result.VariadicResults = Input.boolean();
   return result;
}

//********************************************************************************************************************
// Validates interface limits, uniqueness constraints and cross-record type references.

cache::FormatError validate_interface(const Interface &Value)
{
   if ((Value.Namespaces.size() > MAX_INTERFACE_RECORDS) or (Value.Exports.size() > MAX_INTERFACE_RECORDS) or
       (Value.Structures.size() > MAX_INTERFACE_RECORDS) or
       (Value.Enums.size() > MAX_INTERFACE_RECORDS) or
       (Value.NativeDependencies.size() > MAX_INTERFACE_RECORDS) or
       (Value.Sources.size() > MAX_INTERFACE_RECORDS) or
       (Value.NestedModules.size() > MAX_INTERFACE_RECORDS)) return cache::FormatError::COUNT_LIMIT;

   std::set<std::string> declared_namespaces;
   for (const auto &entry : Value.Namespaces) {
      if (entry.Name.empty() or not valid_enum(entry.Mode, NamespaceMode::DECLARE, NamespaceMode::JOIN)) {
         return cache::FormatError::INVALID_ENUM;
      }

      if (entry.Mode IS NamespaceMode::DECLARE and not declared_namespaces.insert(entry.Name).second) {
         return cache::FormatError::INVALID_METADATA;
      }
   }

   std::set<std::string> exports;
   for (const auto &entry : Value.Exports) {
      if (not valid_enum(entry.Kind, ExportKind::GLOBAL, ExportKind::ENUM_CONSTANT) or
          not valid_enum(entry.Value.Kind, ValueKind::UNKNOWN, ValueKind::ANY) or
          not valid_enum(entry.Value.Proof, ProofKind::ADVISORY, ProofKind::TRUSTED) or
          not valid_enum(entry.Value.Array.ElementKind, ValueKind::UNKNOWN, ValueKind::ANY) or
          not valid_enum(entry.Constant.Kind, ConstantKind::NONE, ConstantKind::STRING)) {
         return cache::FormatError::INVALID_ENUM;
      }

      if (entry.Name.empty() or not exports.insert(entry.Name).second) {
         return cache::FormatError::INVALID_METADATA;
      }

      const bool has_constant = entry.Constant.Kind != ConstantKind::NONE;
      if ((entry.Kind IS ExportKind::ENUM_CONSTANT and not has_constant) or
          (entry.Kind != ExportKind::ENUM_CONSTANT and has_constant and
           (not entry.IsConst or entry.Kind IS ExportKind::EXTERN))) {
         return cache::FormatError::INVALID_METADATA;
      }

      if ((entry.Callable.has_value()) != (entry.Value.Kind IS ValueKind::FUNCTION)) {
         return cache::FormatError::INVALID_METADATA;
      }

      if (entry.Callable and ((entry.Callable->Parameters.size() > MAX_INTERFACE_RECORDS) or
          (entry.Callable->Results.size() > MAX_INTERFACE_RECORDS))) return cache::FormatError::COUNT_LIMIT;
   }

   std::set<std::string> structures;
   for (const auto &entry : Value.Structures) {
      if (entry.Name.empty() or not structures.insert(entry.Name).second) {
         return cache::FormatError::INVALID_METADATA;
      }

      if (entry.Fields.size() > MAX_INTERFACE_RECORDS) return cache::FormatError::COUNT_LIMIT;

      std::set<std::string> fields;
      for (const auto &field : entry.Fields) {
         if (field.Name.empty() or not fields.insert(field.Name).second) {
            return cache::FormatError::INVALID_METADATA;
         }
      }
   }

   std::set<std::string> enums;
   for (const auto &entry : Value.Enums) {
      if (entry.Name.empty() or not enums.insert(entry.Name).second or
          (entry.Members.size() > MAX_INTERFACE_RECORDS)) return cache::FormatError::INVALID_METADATA;
      std::set<std::string> members;
      for (const auto &member : entry.Members) {
         if (member.Name.empty() or not members.insert(member.Name).second) {
            return cache::FormatError::INVALID_METADATA;
         }
         if (not valid_enum(member.Value.Kind, ConstantKind::NIL_VALUE, ConstantKind::STRING)) {
            return cache::FormatError::INVALID_ENUM;
         }
      }
   }

   std::set<std::string> native_modules;
   for (const auto &entry : Value.NativeDependencies) {
      if (entry.Functions.size() > MAX_INTERFACE_RECORDS) return cache::FormatError::COUNT_LIMIT;
      if (entry.Module.empty() or not native_modules.insert(entry.Module).second) {
         return cache::FormatError::INVALID_METADATA;
      }
      std::set<std::string> functions;
      for (const auto &function : entry.Functions) {
         if (function.empty() or not functions.insert(function).second) return cache::FormatError::INVALID_METADATA;
      }
   }

   auto valid_value = [&](const ValueDescriptor &Descriptor) {
      return valid_enum(Descriptor.Kind, ValueKind::UNKNOWN, ValueKind::ANY) and
         valid_enum(Descriptor.Proof, ProofKind::ADVISORY, ProofKind::TRUSTED) and
         valid_enum(Descriptor.Array.ElementKind, ValueKind::UNKNOWN, ValueKind::ANY) and
         (Descriptor.Structure.empty() or structures.contains(Descriptor.Structure)) and
         (Descriptor.Array.Structure.empty() or structures.contains(Descriptor.Array.Structure));
   };

   for (const auto &entry : Value.Exports) {
      if (not valid_value(entry.Value)) return cache::FormatError::INVALID_METADATA;
      if (entry.Callable) {
         for (const auto &parameter : entry.Callable->Parameters) {
            if (not valid_value(parameter.Value)) return cache::FormatError::INVALID_METADATA;
         }
         for (const auto &result : entry.Callable->Results) {
            if (not valid_value(result)) return cache::FormatError::INVALID_METADATA;
         }
      }
   }

   for (const auto &entry : Value.Structures) {
      for (const auto &field : entry.Fields) if (not valid_value(field.Value)) {
         return cache::FormatError::INVALID_METADATA;
      }
   }

   for (const auto &entry : Value.Sources) {
      if (entry.ResolvedPath.empty() or entry.Filename.empty() or not entry.FirstLine or not entry.TotalLines) {
         return cache::FormatError::INVALID_METADATA;
      }
   }
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Places every order-independent collection in the stable wire order.

auto source_order_key(const SourceDescriptor &Entry)
{
   return std::tie(Entry.ResolvedPath, Entry.LogicalRequest, Entry.Filename, Entry.DeclaredNamespace,
      Entry.ParentResolvedPath, Entry.FirstLine, Entry.TotalLines, Entry.ImportLine);
}

auto nested_module_order_key(const NestedModuleDescriptor &Entry)
{
   return std::tie(Entry.LogicalRequest, Entry.ResolvedPath, Entry.InterfaceDigest);
}

void canonicalise_interface(Interface &Value)
{
   std::ranges::sort(Value.Namespaces, {}, [](const auto &Entry) { return std::tie(Entry.Name, Entry.Mode); });
   std::ranges::sort(Value.Exports, {}, [](const auto &Entry) { return std::tie(Entry.Name, Entry.Kind); });
   std::ranges::sort(Value.Structures, {}, &StructureDescriptor::Name);
   for (auto &entry : Value.Structures) std::ranges::sort(entry.Fields, {}, &StructureField::Name);
   std::ranges::sort(Value.Enums, {}, &EnumDescriptor::Name);
   for (auto &entry : Value.Enums) std::ranges::sort(entry.Members, {}, &EnumMember::Name);
   std::ranges::sort(Value.NativeDependencies, {}, &NativeDependency::Module);
   for (auto &entry : Value.NativeDependencies) std::ranges::sort(entry.Functions);
   std::ranges::sort(Value.Sources, {}, source_order_key);
   std::ranges::sort(Value.NestedModules, {}, nested_module_order_key);
}

//********************************************************************************************************************
// Confirms that decoded collections use the writer's exact canonical order.

bool interface_is_canonical(const Interface &Value)
{
   if (not std::ranges::is_sorted(Value.Namespaces, {}, [](const auto &Entry) {
          return std::tie(Entry.Name, Entry.Mode);
       }) or
       not std::ranges::is_sorted(Value.Exports, {}, [](const auto &Entry) {
          return std::tie(Entry.Name, Entry.Kind);
       }) or
       not std::ranges::is_sorted(Value.Structures, {}, &StructureDescriptor::Name) or
       not std::ranges::is_sorted(Value.Enums, {}, &EnumDescriptor::Name) or
       not std::ranges::is_sorted(Value.NativeDependencies, {}, &NativeDependency::Module) or
       not std::ranges::is_sorted(Value.Sources, {}, source_order_key) or
       not std::ranges::is_sorted(Value.NestedModules, {}, nested_module_order_key)) return false;

   for (const auto &entry : Value.Structures) {
      if (not std::ranges::is_sorted(entry.Fields, {}, &StructureField::Name)) return false;
   }
   for (const auto &entry : Value.Enums) {
      if (not std::ranges::is_sorted(entry.Members, {}, &EnumMember::Name)) return false;
   }
   for (const auto &entry : Value.NativeDependencies) {
      if (not std::ranges::is_sorted(entry.Functions)) return false;
   }
   return true;
}

//********************************************************************************************************************
// Produces the canonical interface byte stream from an already ordered and validated graph.

cache::FormatError encode_interface_impl(const Interface &Value, std::string &Output)
{
   Encoder encoder(MAX_INTERFACE_SIZE);

   encoder.u32(uint32_t(Value.Namespaces.size()));
   for (const auto &entry : Value.Namespaces) {
      encoder.string(entry.Name);
      encoder.byte(uint8_t(entry.Mode));
   }

   encoder.u32(uint32_t(Value.Exports.size()));
   for (const auto &entry : Value.Exports) {
      encoder.string(entry.Name);
      encoder.byte(uint8_t(entry.Kind));
      encoder.boolean(entry.IsConst);
      encode_value(encoder, entry.Value);
      encoder.boolean(entry.Callable.has_value());
      if (entry.Callable) encode_callable(encoder, *entry.Callable);
      encoder.byte(uint8_t(entry.Constant.Kind));
      encoder.i64(entry.Constant.Integer);
      encoder.u64(entry.Constant.NumberBits);
      encoder.string(entry.Constant.String);
      encoder.boolean(entry.Constant.Boolean);
   }

   encoder.u32(uint32_t(Value.Structures.size()));
   for (const auto &entry : Value.Structures) {
      encoder.string(entry.Name);
      encoder.u32(uint32_t(entry.Fields.size()));
      for (const auto &field : entry.Fields) {
         encoder.string(field.Name);
         encode_value(encoder, field.Value);
         encoder.boolean(field.Required);
      }
   }

   encoder.u32(uint32_t(Value.Enums.size()));
   for (const auto &entry : Value.Enums) {
      encoder.string(entry.Name);
      encoder.u32(uint32_t(entry.Members.size()));
      for (const auto &member : entry.Members) {
         encoder.string(member.Name);
         encoder.byte(uint8_t(member.Value.Kind));
         encoder.i64(member.Value.Integer);
         encoder.u64(member.Value.NumberBits);
         encoder.string(member.Value.String);
         encoder.boolean(member.Value.Boolean);
      }
   }

   encoder.u32(uint32_t(Value.NativeDependencies.size()));

   for (const auto &entry : Value.NativeDependencies) {
      encoder.string(entry.Module);
      encoder.u32(entry.ActivationOrder);
      encoder.u32(uint32_t(entry.Functions.size()));
      for (const auto &function : entry.Functions) encoder.string(function);
   }

   encoder.u32(uint32_t(Value.Sources.size()));
   for (const auto &entry : Value.Sources) {
      encoder.string(entry.ResolvedPath);
      encoder.string(entry.LogicalRequest);
      encoder.string(entry.Filename);
      encoder.string(entry.DeclaredNamespace);
      encoder.string(entry.ParentResolvedPath);
      encoder.u32(entry.FirstLine);
      encoder.u32(entry.TotalLines);
      encoder.u32(entry.ImportLine);
   }

   encoder.u32(uint32_t(Value.NestedModules.size()));
   for (const auto &entry : Value.NestedModules) {
      encoder.string(entry.LogicalRequest);
      encoder.string(entry.ResolvedPath);
      encoder.digest(entry.InterfaceDigest);
   }

   if (encoder.Error != cache::FormatError::OKAY) return encoder.Error;
   Output = std::move(encoder.Bytes);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Decodes a complete interface stream and rejects trailing or inconsistent metadata.

cache::FormatError decode_interface_impl(std::string_view Bytes, Interface &Output)
{
   Decoder input(Bytes);
   Interface result;

   decode_records<NamespaceDescriptor>(input, result.Namespaces, [](Decoder &Value) {
      NamespaceDescriptor entry { Value.string(), NamespaceMode(Value.byte()) };
      if (not valid_enum(entry.Mode, NamespaceMode::DECLARE, NamespaceMode::JOIN)) {
         Value.Error = cache::FormatError::INVALID_ENUM;
      }
      return entry;
   });

   decode_records<ExportDescriptor>(input, result.Exports, [](Decoder &Value) {
      ExportDescriptor entry;
      entry.Name    = Value.string();
      entry.Kind    = ExportKind(Value.byte());
      entry.IsConst = Value.boolean();
      entry.Value   = decode_value(Value);
      if (Value.boolean()) entry.Callable = decode_callable(Value);
      entry.Constant.Kind       = ConstantKind(Value.byte());
      entry.Constant.Integer    = Value.i64();
      entry.Constant.NumberBits = Value.u64();
      entry.Constant.String     = Value.string();
      entry.Constant.Boolean    = Value.boolean();
      if (not valid_enum(entry.Kind, ExportKind::GLOBAL, ExportKind::ENUM_CONSTANT) or
          not valid_enum(entry.Constant.Kind, ConstantKind::NONE, ConstantKind::STRING)) {
         Value.Error = cache::FormatError::INVALID_ENUM;
      }
      return entry;
   });

   decode_records<StructureDescriptor>(input, result.Structures, [](Decoder &Value) {
      StructureDescriptor entry;
      entry.Name = Value.string();
      decode_records<StructureField>(Value, entry.Fields, [](Decoder &Field) {
         return StructureField { Field.string(), decode_value(Field), Field.boolean() };
      });
      return entry;
   });

   decode_records<EnumDescriptor>(input, result.Enums, [](Decoder &Value) {
      EnumDescriptor entry;
      entry.Name = Value.string();
      decode_records<EnumMember>(Value, entry.Members, [](Decoder &Member) {
         EnumMember result;
         result.Name             = Member.string();
         result.Value.Kind       = ConstantKind(Member.byte());
         result.Value.Integer    = Member.i64();
         result.Value.NumberBits = Member.u64();
         result.Value.String     = Member.string();
         result.Value.Boolean    = Member.boolean();
         if (not valid_enum(result.Value.Kind, ConstantKind::NIL_VALUE, ConstantKind::STRING)) {
            Member.Error = cache::FormatError::INVALID_ENUM;
         }
         return result;
      });
      return entry;
   });

   decode_records<NativeDependency>(input, result.NativeDependencies, [](Decoder &Value) {
      NativeDependency entry;
      entry.Module = Value.string();
      entry.ActivationOrder = Value.u32();
      decode_records<std::string>(Value, entry.Functions, [](Decoder &Function) { return Function.string(); });
      return entry;
   });

   decode_records<SourceDescriptor>(input, result.Sources, [](Decoder &Value) {
      SourceDescriptor result;
      result.ResolvedPath       = Value.string();
      result.LogicalRequest     = Value.string();
      result.Filename           = Value.string();
      result.DeclaredNamespace  = Value.string();
      result.ParentResolvedPath = Value.string();
      result.FirstLine          = Value.u32();
      result.TotalLines         = Value.u32();
      result.ImportLine         = Value.u32();
      return result;
   });

   decode_records<NestedModuleDescriptor>(input, result.NestedModules, [](Decoder &Value) {
      return NestedModuleDescriptor { Value.string(), Value.string(), Value.digest() };
   });

   if (input.Error != cache::FormatError::OKAY) return input.Error;
   if (input.Position != input.size()) return cache::FormatError::INVALID_METADATA;
   if (auto error = validate_interface(result); error != cache::FormatError::OKAY) return error;
   if (not interface_is_canonical(result)) return cache::FormatError::INVALID_METADATA;
   Output = std::move(result);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Serialises canonical identity inputs.  Lookup identities stop before observations; compiled identities include
// every observed input and the immutable identities of executable dependencies.

cache::FormatError encode_identity_inputs(const Identity &Value, bool IncludeObservations, bool IncludeModifiedHints,
   std::string &Output)
{
   if (Value.Schema != SCHEMA_VERSION) return cache::FormatError::UNSUPPORTED_VERSION;
   if (not Value.ImportedRoot) return cache::FormatError::INVALID_METADATA;
   if ((Value.Options.size() > cache::MAX_OPTIONS) or (Value.LocalImports.size() > cache::MAX_IMPORTS) or
       (Value.ModuleDependencies.size() > cache::MAX_IMPORTS) or
       (Value.ResolutionInputs.size() > cache::MAX_RESOLUTION_INPUTS) or
       (Value.ConditionalInputs.size() > cache::MAX_CONDITIONAL_INPUTS)) return cache::FormatError::COUNT_LIMIT;

   Encoder encoder(cache::MAX_METADATA_SIZE);
   encoder.u32(Value.Schema);
   encoder.string(Value.BuildIdentity);
   encoder.string(Value.LogicalRequest);
   encoder.boolean(Value.ImportedRoot);
   cache::SourceIdentity source = Value.Source;
   if (not IncludeModifiedHints) source.ModifiedHint = 0;
   encode_source(encoder, source);

   auto options = Value.Options;
   std::ranges::sort(options, {}, [](const auto &Entry) { return std::tie(Entry.Name, Entry.Value); });
   encoder.u32(uint32_t(options.size()));
   for (const auto &entry : options) { encoder.string(entry.Name); encoder.string(entry.Value); }

   if (not IncludeObservations) {
      if (encoder.Error != cache::FormatError::OKAY) return encoder.Error;
      Output = std::move(encoder.Bytes);
      return cache::FormatError::OKAY;
   }

   auto local_imports = Value.LocalImports;
   std::ranges::sort(local_imports, {}, [](const auto &Entry) {
      return std::tie(Entry.ParentPath, Entry.OriginalRequest, Entry.Source.ResolvedPath);
   });
   encoder.u32(uint32_t(local_imports.size()));
   for (auto entry : local_imports) {
      encoder.string(entry.ParentPath);
      encoder.string(entry.OriginalRequest);
      if (not IncludeModifiedHints) entry.Source.ModifiedHint = 0;
      encode_source(encoder, entry.Source);
   }

   auto module_dependencies = Value.ModuleDependencies;
   std::ranges::sort(module_dependencies, {}, [](const auto &Entry) {
      return std::tie(Entry.OriginalRequest, Entry.ResolvedPath, Entry.CompiledIdentity);
   });
   encoder.u32(uint32_t(module_dependencies.size()));
   for (const auto &entry : module_dependencies) {
      if (entry.CompiledIdentity.empty()) return cache::FormatError::INVALID_METADATA;
      encoder.string(entry.OriginalRequest);
      encoder.string(entry.ResolvedPath);
      encoder.digest(entry.InterfaceDigest);
      encoder.string(entry.CompiledIdentity);
   }

   auto resolution_inputs = Value.ResolutionInputs;
   std::ranges::sort(resolution_inputs, {}, [](const auto &Entry) {
      return std::tie(Entry.Name, Entry.Context, Entry.Value);
   });
   encoder.u32(uint32_t(resolution_inputs.size()));
   for (const auto &entry : resolution_inputs) {
      encoder.string(entry.Name); encoder.string(entry.Context); encoder.string(entry.Value);
   }

   auto conditional_inputs = Value.ConditionalInputs;
   std::ranges::sort(conditional_inputs, {}, [](const auto &Entry) {
      return std::tie(Entry.Kind, Entry.Name, Entry.Context, Entry.Value);
   });
   encoder.u32(uint32_t(conditional_inputs.size()));
   for (const auto &entry : conditional_inputs) {
      if (not valid_enum(entry.Kind, cache::ConditionalKind::IMPORTED, cache::ConditionalKind::OTHER)) {
         return cache::FormatError::INVALID_ENUM;
      }
      encoder.byte(uint8_t(entry.Kind));
      encoder.string(entry.Name); encoder.string(entry.Context); encoder.string(entry.Value);
   }

   if (encoder.Error != cache::FormatError::OKAY) return encoder.Error;
   Output = std::move(encoder.Bytes);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Serialises a complete identity record with independently verifiable lookup and compiled identities.

cache::FormatError encode_identity(const Identity &Value, std::string &Output)
{
   std::string canonical;
   if (auto error = encode_identity_inputs(Value, true, true, canonical); error != cache::FormatError::OKAY) {
      return error;
   }
   if (Value.LookupIdentity.empty() or Value.CompiledIdentity.empty()) {
      return cache::FormatError::INVALID_METADATA;
   }

   Encoder encoder(cache::MAX_METADATA_SIZE);
   encoder.string(Value.LookupIdentity);
   encoder.string(Value.CompiledIdentity);
   if (encoder.Error IS cache::FormatError::OKAY and
       encoder.Bytes.size() + canonical.size() <= cache::MAX_METADATA_SIZE) {
      encoder.Bytes += canonical;
   }
   else if (encoder.Error IS cache::FormatError::OKAY) encoder.Error = cache::FormatError::SIZE_LIMIT;
   if (encoder.Error != cache::FormatError::OKAY) return encoder.Error;
   Output = std::move(encoder.Bytes);
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Decodes and validates an imported module's compilation identity.

cache::FormatError decode_identity(std::string_view Bytes, Identity &Output)
{
   Decoder input(Bytes);
   Identity result;

   result.LookupIdentity = input.string();
   result.CompiledIdentity = input.string();
   result.Schema = input.u32();
   result.BuildIdentity = input.string();
   result.LogicalRequest = input.string();
   result.ImportedRoot = input.boolean();
   result.Source = decode_source(input);

   decode_records<cache::CompilationOption>(input, result.Options, [](Decoder &Value) {
      return cache::CompilationOption { Value.string(), Value.string() };
   });

   decode_records<LocalImportIdentity>(input, result.LocalImports, [](Decoder &Value) {
      return LocalImportIdentity { Value.string(), Value.string(), decode_source(Value) };
   });

   decode_records<ModuleDependencyIdentity>(input, result.ModuleDependencies, [](Decoder &Value) {
      return ModuleDependencyIdentity { Value.string(), Value.string(), Value.digest(), Value.string() };
   });

   decode_records<cache::ResolutionInput>(input, result.ResolutionInputs, [](Decoder &Value) {
      return cache::ResolutionInput { Value.string(), Value.string(), Value.string() };
   });

   decode_records<cache::ConditionalInput>(input, result.ConditionalInputs, [](Decoder &Value) {
      auto kind = cache::ConditionalKind(Value.byte());
      if (not valid_enum(kind, cache::ConditionalKind::IMPORTED, cache::ConditionalKind::OTHER)) {
         Value.Error = cache::FormatError::INVALID_ENUM;
      }
      return cache::ConditionalInput { kind, Value.string(), Value.string(), Value.string() };
   });

   if (input.Error != cache::FormatError::OKAY) return input.Error;
   if (input.Position != input.size() or not result.ImportedRoot or result.Schema != SCHEMA_VERSION or
       result.LookupIdentity != lookup_key(result) or result.CompiledIdentity != compiled_key(result)) {
      return cache::FormatError::INVALID_METADATA;
   }
   Output = std::move(result);
   return cache::FormatError::OKAY;
}

} // namespace

//********************************************************************************************************************
// Encodes a portable compile-time interface in its canonical form.

#ifdef UNIT_TESTS
cache::FormatError encode_interface(const Interface &Value, std::string &Output)
{
   FinalisedInterfacePtr finalised;
   auto error = FinalisedInterface::finalise(Value, finalised);
   if (error != cache::FormatError::OKAY) return error;
   Output.assign(finalised->bytes());
   return cache::FormatError::OKAY;
}
#endif

//********************************************************************************************************************
// Decodes a portable compile-time interface and validates its metadata.

cache::FormatError decode_interface(std::string_view Bytes, Interface &Output)
{
   return decode_interface_impl(Bytes, Output);
}

//********************************************************************************************************************
// Validates, canonicalises and encodes one mutable cold interface before publishing it atomically.

cache::FormatError FinalisedInterface::finalise(
   Interface Value, FinalisedInterfacePtr &Output, InterfaceOperationCounters *Counters)
{
   if (auto error = validate_interface(Value); error != cache::FormatError::OKAY) return error;
   canonicalise_interface(Value);
   std::string bytes;
   if (auto error = encode_interface_impl(Value, bytes); error != cache::FormatError::OKAY) return error;
   auto digest = cache::content_digest(bytes);
   FinalisedInterfacePtr result(new FinalisedInterface(std::move(Value), std::move(bytes), digest));
   Output = std::move(result);
   if (Counters) {
      Counters->ColdFinalisations++;
      Counters->Encodes++;
   }
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Adopts verified canonical interface bytes without re-encoding the decoded descriptor graph.

cache::FormatError FinalisedInterface::from_canonical_bytes(std::string_view Bytes, const cache::Digest &Digest,
   FinalisedInterfacePtr &Output, InterfaceOperationCounters *Counters)
{
   if (Bytes.size() > MAX_INTERFACE_SIZE) return cache::FormatError::SIZE_LIMIT;
   if (cache::content_digest(Bytes) != Digest) return cache::FormatError::INVALID_METADATA;
   Interface descriptors;
   if (auto error = decode_interface_impl(Bytes, descriptors); error != cache::FormatError::OKAY) return error;
   FinalisedInterfacePtr result(
      new FinalisedInterface(std::move(descriptors), std::string(Bytes), Digest));
   Output = std::move(result);
   if (Counters) Counters->WarmDecodes++;
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Identifies data carrying the imported-module cache signature.

bool is_envelope(std::string_view Input) noexcept
{
   return Input.size() >= MODULE_MAGIC.size() and
      std::memcmp(Input.data(), MODULE_MAGIC.data(), MODULE_MAGIC.size()) IS 0;
}

//********************************************************************************************************************
// Derives the full cache filename key from a canonical module identity.

std::string file_key(const Identity &IdentityValue)
{
   return compiled_key(IdentityValue);
}

//********************************************************************************************************************
// Derives the preliminary lookup key from identity fields known before dependency discovery.

std::string lookup_key(const Identity &IdentityValue)
{
   std::string bytes;
   if (encode_identity_inputs(IdentityValue, false, true, bytes) != cache::FormatError::OKAY) return {};
   auto build = cache::digest_hex(cache::content_digest(IdentityValue.BuildIdentity));
   return "import-v9-" + build.substr(0, 16) + "-" + cache::digest_hex(cache::content_digest(bytes));
}

//********************************************************************************************************************
// Derives the immutable compiled identity from canonical semantic inputs and compiled dependency identities.

std::string compiled_key(const Identity &IdentityValue)
{
   std::string bytes;
   if (encode_identity_inputs(IdentityValue, true, false, bytes) != cache::FormatError::OKAY) return {};
   auto build = cache::digest_hex(cache::content_digest(IdentityValue.BuildIdentity));
   return "compiled-v9-" + build.substr(0, 16) + "-" + cache::digest_hex(cache::content_digest(bytes));
}

//********************************************************************************************************************
// Populates both explicit identities after dependency discovery has completed.

cache::FormatError finalise_identity(Identity &IdentityValue)
{
   IdentityValue.Schema = SCHEMA_VERSION;
   IdentityValue.LookupIdentity = lookup_key(IdentityValue);
   IdentityValue.CompiledIdentity = compiled_key(IdentityValue);
   if (IdentityValue.LookupIdentity.empty() or IdentityValue.CompiledIdentity.empty()) {
      return cache::FormatError::INVALID_METADATA;
   }
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Packages module identity, interface metadata and bytecode into a verified cache envelope.

cache::FormatError encode_envelope(const Identity &IdentityValue, const FinalisedInterface &InterfaceValue,
   std::string_view Payload, std::string &Output)
{
   Output.clear();
   if (Payload.size() > cache::MAX_PAYLOAD_SIZE) return cache::FormatError::SIZE_LIMIT;
   if (not Payload.starts_with("\x1bLJ")) return cache::FormatError::INVALID_PAYLOAD;
   Identity final_identity = IdentityValue;
   if (auto error = finalise_identity(final_identity); error != cache::FormatError::OKAY) return error;
   std::string identity;
   if (auto error = encode_identity(final_identity, identity); error != cache::FormatError::OKAY) return error;
   const std::string_view interface_bytes = InterfaceValue.bytes();

   Encoder header;
   header.Bytes.append((const char *)MODULE_MAGIC.data(), MODULE_MAGIC.size());
   header.u32(SCHEMA_VERSION);
   header.u32(uint32_t(identity.size()));
   header.u32(uint32_t(interface_bytes.size()));
   header.u64(Payload.size());
   header.digest(cache::content_digest(identity));
   header.digest(InterfaceValue.digest());
   header.digest(cache::content_digest(Payload));
   Output.reserve(header.Bytes.size() + identity.size() + interface_bytes.size() + Payload.size());
   Output = std::move(header.Bytes);
   Output += identity;
   Output += interface_bytes;
   Output += Payload;
   return cache::FormatError::OKAY;
}

//********************************************************************************************************************
// Validates a cache envelope and exposes its decoded metadata and borrowed bytecode payload.

cache::FormatError decode_envelope(
   std::string_view Input, EnvelopeView &Output, InterfaceOperationCounters *Counters)
{
   Output = {};

   if (not is_envelope(Input)) return cache::FormatError::NOT_CACHE;
   if (Input.size() < HEADER_SIZE) return cache::FormatError::TRUNCATED;

   Decoder header(Input.substr(MODULE_MAGIC.size(), HEADER_SIZE - MODULE_MAGIC.size()));
   auto schema         = header.u32();
   auto identity_size  = header.u32();
   auto interface_size = header.u32();
   auto payload_size   = header.u64();
   auto identity_hash  = header.digest();
   auto interface_hash = header.digest();
   auto payload_hash   = header.digest();

   if (schema != SCHEMA_VERSION) return cache::FormatError::UNSUPPORTED_VERSION;
   if ((identity_size > cache::MAX_METADATA_SIZE) or (interface_size > MAX_INTERFACE_SIZE) or
       (payload_size > cache::MAX_PAYLOAD_SIZE)) return cache::FormatError::SIZE_LIMIT;
   if ((uint64_t(HEADER_SIZE) + identity_size + interface_size + payload_size) != Input.size()) {
      return cache::FormatError::TRUNCATED;
   }

   auto identity = Input.substr(HEADER_SIZE, identity_size);
   auto interface_bytes = Input.substr(HEADER_SIZE + identity_size, interface_size);
   auto payload = Input.substr(HEADER_SIZE + identity_size + interface_size, size_t(payload_size));
   if (cache::content_digest(identity) != identity_hash) return cache::FormatError::INVALID_METADATA;

   if (cache::content_digest(payload) != payload_hash or not payload.starts_with("\x1bLJ")) {
      return cache::FormatError::INVALID_PAYLOAD;
   }

   EnvelopeView result;
   if (auto error = decode_identity(identity, result.CompilationIdentity);
       error != cache::FormatError::OKAY) return error;
   if (auto error = FinalisedInterface::from_canonical_bytes(
       interface_bytes, interface_hash, result.CompileTimeInterface, Counters);
       error != cache::FormatError::OKAY) return error;
   result.LookupIdentity = result.CompilationIdentity.LookupIdentity;
   result.CompiledIdentity = result.CompilationIdentity.CompiledIdentity;
   result.Payload = payload;
   Output = std::move(result);
   return cache::FormatError::OKAY;
}

} // namespace tiri::import_cache
