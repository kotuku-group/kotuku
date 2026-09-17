#pragma once

#include "cache_manifest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tiri::import_cache {

constexpr uint32_t SCHEMA_VERSION = 9;
constexpr size_t MAX_INTERFACE_SIZE = 8 * 1024 * 1024;
constexpr size_t MAX_INTERFACE_RECORDS = 4096;

enum class ValueKind : uint8_t {
   UNKNOWN = 1,
   NIL_VALUE,
   BOOLEAN,
   NUMBER,
   STRING,
   TABLE,
   ARRAY,
   FUNCTION,
   THREAD,
   OBJECT,
   STRUCTURE,
   ANY
};

enum class ProofKind : uint8_t { ADVISORY = 1, CLOSED, CHECKED, TRUSTED };
enum class ExportKind : uint8_t { GLOBAL = 1, EXTERN, ENUM_CONSTANT };
enum class NamespaceMode : uint8_t { DECLARE = 1, JOIN };
enum class ConstantKind : uint8_t { NONE = 1, NIL_VALUE, BOOLEAN, INTEGER, NUMBER, STRING };

struct ArrayDescriptor {
   uint8_t Storage = 0;
   ValueKind ElementKind = ValueKind::ANY;
   std::string ObjectClass;
   std::string Structure;
   std::string NestedIdentity;

   [[nodiscard]] bool operator==(const ArrayDescriptor &) const = default;
};

struct ValueDescriptor {
   ValueKind Kind = ValueKind::UNKNOWN;
   ProofKind Proof = ProofKind::ADVISORY;
   std::string ObjectClass;
   std::string Structure;
   ArrayDescriptor Array;
   bool Nullable = true;

   [[nodiscard]] bool operator==(const ValueDescriptor &) const = default;
};

struct ContractDescriptor {
   ValueDescriptor Value;
   std::string Label;
   uint8_t Position = 0;
   bool Required = false;
   bool IsConst = false;

   [[nodiscard]] bool operator==(const ContractDescriptor &) const = default;
};

struct CallableDescriptor {
   std::vector<ContractDescriptor> Parameters;
   std::vector<ValueDescriptor> Results;
   uint16_t DeclaredResults = 0;
   bool VariadicParameters = false;
   bool VariadicResults = false;

   [[nodiscard]] bool operator==(const CallableDescriptor &) const = default;
};

struct ConstantValue {
   ConstantKind Kind = ConstantKind::NONE;
   int64_t Integer = 0;
   uint64_t NumberBits = 0;
   std::string String;
   bool Boolean = false;

   [[nodiscard]] bool operator==(const ConstantValue &) const = default;
};

struct NamespaceDescriptor {
   std::string Name;
   NamespaceMode Mode = NamespaceMode::DECLARE;

   [[nodiscard]] bool operator==(const NamespaceDescriptor &) const = default;
};

struct ExportDescriptor {
   std::string Name;
   ExportKind Kind = ExportKind::GLOBAL;
   ValueDescriptor Value;
   std::optional<CallableDescriptor> Callable;
   ConstantValue Constant;
   bool IsConst = false;

   [[nodiscard]] bool operator==(const ExportDescriptor &) const = default;
};

struct StructureField {
   std::string Name;
   ValueDescriptor Value;
   bool Required = false;

   [[nodiscard]] bool operator==(const StructureField &) const = default;
};

struct StructureDescriptor {
   std::string Name;
   std::vector<StructureField> Fields;

   [[nodiscard]] bool operator==(const StructureDescriptor &) const = default;
};

struct EnumMember {
   std::string Name;
   ConstantValue Value;

   [[nodiscard]] bool operator==(const EnumMember &) const = default;
};

struct EnumDescriptor {
   std::string Name;
   std::vector<EnumMember> Members;

   [[nodiscard]] bool operator==(const EnumDescriptor &) const = default;
};

struct NativeDependency {
   std::string Module;
   std::vector<std::string> Functions;
   uint32_t ActivationOrder = 0;

   [[nodiscard]] bool operator==(const NativeDependency &) const = default;
};

struct SourceDescriptor {
   std::string ResolvedPath;
   std::string LogicalRequest;
   std::string Filename;
   std::string DeclaredNamespace;
   std::string ParentResolvedPath;
   uint32_t FirstLine = 1;
   uint32_t TotalLines = 1;
   uint32_t ImportLine = 0;

   [[nodiscard]] bool operator==(const SourceDescriptor &) const = default;
};

struct NestedModuleDescriptor {
   std::string LogicalRequest;
   std::string ResolvedPath;
   cache::Digest InterfaceDigest = {};

   [[nodiscard]] bool operator==(const NestedModuleDescriptor &) const = default;
};

struct Interface {
   std::vector<NamespaceDescriptor> Namespaces;
   std::vector<ExportDescriptor> Exports;
   std::vector<StructureDescriptor> Structures;
   std::vector<EnumDescriptor> Enums;
   std::vector<NativeDependency> NativeDependencies;
   std::vector<SourceDescriptor> Sources;
   std::vector<NestedModuleDescriptor> NestedModules;

   [[nodiscard]] bool operator==(const Interface &) const = default;
};

struct LocalImportIdentity {
   std::string ParentPath;
   std::string OriginalRequest;
   cache::SourceIdentity Source;
};

struct ModuleDependencyIdentity {
   std::string OriginalRequest;
   std::string ResolvedPath;
   cache::Digest InterfaceDigest = {};
   std::string CompiledIdentity;
};

struct Identity {
   uint32_t Schema = SCHEMA_VERSION;
   std::string LookupIdentity;
   std::string CompiledIdentity;
   std::string BuildIdentity;
   std::string LogicalRequest;
   cache::SourceIdentity Source;
   std::vector<cache::CompilationOption> Options;
   std::vector<LocalImportIdentity> LocalImports;
   std::vector<ModuleDependencyIdentity> ModuleDependencies;
   std::vector<cache::ResolutionInput> ResolutionInputs;
   std::vector<cache::ConditionalInput> ConditionalInputs;
   bool ImportedRoot = true;
};

struct EnvelopeView {
   Identity CompilationIdentity;
   std::string LookupIdentity;
   std::string CompiledIdentity;
   Interface CompileTimeInterface;
   cache::Digest InterfaceDigest = {};
   std::string_view Payload;
};

// Installation deliberately owns portable records rather than live parser or Lua values.  I04 may translate these
// records to transient analyser handles, while decoded interfaces can already be compared and staged atomically.
struct CompileTimeContext {
   std::vector<NamespaceDescriptor> Namespaces;
   std::vector<ExportDescriptor> Bindings;
   std::vector<StructureDescriptor> Structures;
   std::vector<EnumDescriptor> Enums;
   std::vector<NativeDependency> NativeDependencies;
   std::vector<SourceDescriptor> Sources;
   std::vector<NestedModuleDescriptor> NestedModules;
};

[[nodiscard]] bool is_envelope(std::string_view Input) noexcept;
[[nodiscard]] std::string file_key(const Identity &IdentityValue);
[[nodiscard]] std::string lookup_key(const Identity &IdentityValue);
[[nodiscard]] std::string compiled_key(const Identity &IdentityValue);
[[nodiscard]] cache::FormatError finalise_identity(Identity &IdentityValue);
[[nodiscard]] cache::Digest interface_digest(const Interface &InterfaceValue);
[[nodiscard]] cache::FormatError encode_interface(const Interface &, std::string &Output);
[[nodiscard]] cache::FormatError decode_interface(std::string_view Bytes, Interface &Output);
[[nodiscard]] cache::FormatError encode_envelope(
   const Identity &, const Interface &, std::string_view Payload, std::string &Output);
[[nodiscard]] cache::FormatError decode_envelope(std::string_view Input, EnvelopeView &Output);
[[nodiscard]] cache::FormatError install_interface(const Interface &, CompileTimeContext &Context);

} // namespace tiri::import_cache
