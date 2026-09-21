#pragma once

#include "cache_manifest.h"
#include "package_identity.h"
#include "version_constraints.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tiri::import_cache {

constexpr uint32_t SCHEMA_VERSION = 14;
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
   uint8_t Storage = 0;                          // Stores the array representation.
   ValueKind ElementKind = ValueKind::ANY;       // Identifies values stored in the array.
   std::string ObjectClass;                      // Names the object class of array elements.
   std::string Structure;                        // Names the structure of array elements.
   std::string NestedIdentity;                   // Identifies a nested module's array type.

   [[nodiscard]] bool operator==(const ArrayDescriptor &) const = default;
};

struct ValueDescriptor {
   ValueKind Kind = ValueKind::UNKNOWN;          // Identifies the value type.
   ProofKind Proof = ProofKind::ADVISORY;        // Specifies the strength of the type proof.
   std::string ObjectClass;                      // Names the value's object class.
   std::string Structure;                        // Names the value's structure.
   ArrayDescriptor Array;                        // Describes the value when it is an array.
   bool Nullable = true;                         // Indicates whether the value may be nil.

   [[nodiscard]] bool operator==(const ValueDescriptor &) const = default;
};

struct ContractDescriptor {
   ValueDescriptor Value;                        // Describes the contracted value.
   std::string Label;                            // Stores the parameter or result label.
   uint8_t Position = 0;                         // Stores the value's declared position.
   bool Required = false;                        // Indicates whether the value is mandatory.
   bool IsConst = false;                         // Indicates whether the value is immutable.

   [[nodiscard]] bool operator==(const ContractDescriptor &) const = default;
};

struct CallableDescriptor {
   std::vector<ContractDescriptor> Parameters;   // Describes the callable's parameters.
   std::vector<ValueDescriptor> Results;         // Describes the callable's result values.
   uint16_t DeclaredResults = 0;                  // Stores the number of declared results.
   bool VariadicParameters = false;               // Indicates whether parameters are variadic.
   bool VariadicResults = false;                  // Indicates whether results are variadic.

   [[nodiscard]] bool operator==(const CallableDescriptor &) const = default;
};

struct ConstantValue {
   ConstantKind Kind = ConstantKind::NONE;        // Identifies the constant value type.
   int64_t Integer = 0;                           // Stores an integer constant.
   uint64_t NumberBits = 0;                       // Stores the bit pattern of a numeric constant.
   std::string String;                            // Stores a string constant.
   bool Boolean = false;                          // Stores a Boolean constant.

   [[nodiscard]] bool operator==(const ConstantValue &) const = default;
};

struct NamespaceDescriptor {
   std::string Name;                              // Stores the namespace name.
   NamespaceMode Mode = NamespaceMode::DECLARE;   // Specifies how the namespace is introduced.

   [[nodiscard]] bool operator==(const NamespaceDescriptor &) const = default;
};

struct ExportDescriptor {
   std::string Name;                              // Stores the exported symbol name.
   ExportKind Kind = ExportKind::GLOBAL;          // Identifies the exported symbol kind.
   ValueDescriptor Value;                         // Describes the exported value.
   std::optional<CallableDescriptor> Callable;    // Describes the export when it is callable.
   ConstantValue Constant;                        // Stores the export's constant value.
   bool IsConst = false;                          // Indicates whether the export is immutable.

   [[nodiscard]] bool operator==(const ExportDescriptor &) const = default;
};

struct StructureField {
   std::string Name;                              // Stores the field name.
   ValueDescriptor Value;                         // Describes the field value.
   bool Required = false;                         // Indicates whether the field is required.

   [[nodiscard]] bool operator==(const StructureField &) const = default;
};

struct StructureDescriptor {
   std::string Name;                              // Stores the structure name.
   std::vector<StructureField> Fields;            // Describes the structure's fields.

   [[nodiscard]] bool operator==(const StructureDescriptor &) const = default;
};

struct EnumMember {
   std::string Name;                              // Stores the enumeration member name.
   ConstantValue Value;                           // Stores the enumeration member value.

   [[nodiscard]] bool operator==(const EnumMember &) const = default;
};

struct EnumDescriptor {
   std::string Name;                              // Stores the enumeration name.
   std::vector<EnumMember> Members;               // Stores the enumeration members.

   [[nodiscard]] bool operator==(const EnumDescriptor &) const = default;
};

struct NativeDependency {
   std::string Module;                            // Names the required native module.
   std::vector<std::string> Functions;            // Lists required functions from the module.
   uint32_t ActivationOrder = 0;                  // Specifies the module activation order.

   [[nodiscard]] bool operator==(const NativeDependency &) const = default;
};

struct SourceDescriptor {
   std::string ResolvedPath;                      // Stores the resolved source path.
   std::string LogicalRequest;                    // Stores the original source request.
   std::string Filename;                          // Stores the source filename.
   std::string DeclaredNamespace;                 // Stores the namespace declared by the source.
   std::string ParentResolvedPath;                // Stores the importing source path.
   uint32_t FirstLine = 1;                        // Stores the source's first line in its parent.
   uint32_t TotalLines = 1;                       // Stores the number of source lines.
   uint32_t ImportLine = 0;                       // Stores the importing line number.

   [[nodiscard]] bool operator==(const SourceDescriptor &) const = default;
};

struct NestedModuleDescriptor {
   std::string LogicalRequest;                    // Stores the nested module's original request.
   std::string ResolvedPath;                      // Stores the nested module's resolved path.
   cache::Digest InterfaceDigest = {};            // Identifies the nested module interface.

   [[nodiscard]] bool operator==(const NestedModuleDescriptor &) const = default;
};

struct Interface {
   std::optional<tiri::PackageIdentity> Package; // Declares the module's canonical package identity, when present.
   std::string CompatibilityManifest = std::string("\x01\x00", 2); // Canonical transitive runtime requirements.
   std::vector<NamespaceDescriptor> Namespaces;   // Describes namespaces declared by the module.
   std::vector<ExportDescriptor> Exports;         // Describes symbols exported by the module.
   std::vector<StructureDescriptor> Structures;   // Describes structures declared by the module.
   std::vector<EnumDescriptor> Enums;             // Describes enumerations declared by the module.
   std::string StructureManifest;                  // Preserves exact state-local structure layouts.
   std::vector<NativeDependency> NativeDependencies; // Describes native module dependencies.
   std::vector<SourceDescriptor> Sources;         // Describes source files in the module.
   std::vector<NestedModuleDescriptor> NestedModules; // Describes imported nested modules.

   [[nodiscard]] bool operator==(const Interface &) const = default;
};

struct InterfaceOperationCounters {
   uint32_t ColdFinalisations = 0;                // Counts cold interface finalisations.
   uint32_t Encodes = 0;                          // Counts interface encodes.
   uint32_t WarmDecodes = 0;                      // Counts cache-backed interface decodes.
};

class FinalisedInterface {
public:
   [[nodiscard]] static cache::FormatError finalise(Interface, std::shared_ptr<const FinalisedInterface> &,
      InterfaceOperationCounters *Counters = nullptr);
   [[nodiscard]] static cache::FormatError from_canonical_bytes(std::string_view, const cache::Digest &,
      std::shared_ptr<const FinalisedInterface> &, InterfaceOperationCounters *Counters = nullptr);

   [[nodiscard]] const Interface & descriptors() const noexcept { return this->descriptors_; }
   [[nodiscard]] std::string_view bytes() const noexcept { return this->bytes_; }
   [[nodiscard]] const cache::Digest & digest() const noexcept { return this->digest_; }

private:
   FinalisedInterface(Interface Descriptors, std::string Bytes, const cache::Digest &Digest) :
      descriptors_(std::move(Descriptors)), bytes_(std::move(Bytes)), digest_(Digest) { }

   Interface descriptors_;                        // Stores the canonical interface descriptors.
   std::string bytes_;                            // Stores the canonical serialised interface.
   cache::Digest digest_ = {};                    // Stores the digest of the canonical interface.
};

using FinalisedInterfacePtr = std::shared_ptr<const FinalisedInterface>;

struct LocalImportIdentity {
   std::string ParentPath;                        // Stores the importing source path.
   std::string OriginalRequest;                   // Stores the import request text.
   cache::SourceIdentity Source;                  // Identifies the imported source content.
};

struct ModuleDependencyIdentity {
   std::string OriginalRequest;                   // Stores the dependency request text.
   std::string ResolvedPath;                      // Stores the dependency's resolved path.
   cache::Digest InterfaceDigest = {};            // Identifies the dependency interface.
   std::string CompiledIdentity;                  // Identifies the compiled dependency.
};

struct Identity {
   uint32_t Schema = SCHEMA_VERSION;              // Identifies the cache schema version.
   std::string LookupIdentity;                    // Identifies the module for cache lookup.
   std::string CompiledIdentity;                  // Identifies the compiled module content.
   std::string BuildIdentity;                     // Identifies the Tiri build that compiled the module.
   std::string LogicalRequest;                    // Stores the original module request.
   std::optional<tiri::PackageIdentity> ExpectedPackage; // Stores the resolver-provided package identity.
   std::optional<tiri::PackageIdentity> DeclaredPackage; // Stores the package declared by the compiled source.
   cache::SourceIdentity Source;                  // Identifies the root source content.
   std::vector<cache::CompilationOption> Options; // Stores compilation options affecting output.
   std::vector<LocalImportIdentity> LocalImports; // Identifies imported local source files.
   std::vector<ModuleDependencyIdentity> ModuleDependencies; // Identifies imported module dependencies.
   std::vector<cache::ResolutionInput> ResolutionInputs; // Stores inputs that affect module resolution.
   std::vector<cache::ConditionalInput> ConditionalInputs; // Stores inputs that affect conditional compilation.
   bool ImportedRoot = true;                      // Indicates whether the root is imported.
};

struct EnvelopeView {
   Identity CompilationIdentity;                  // Identifies the compilation represented by the envelope.
   std::string LookupIdentity;                    // Identifies the envelope for cache lookup.
   std::string CompiledIdentity;                  // Identifies the compiled envelope content.
   FinalisedInterfacePtr CompileTimeInterface;    // Provides the interface used during compilation.
   std::string Payload;                           // Owns the verified serialised module payload.
};

[[nodiscard]] bool is_envelope(std::string_view Input) noexcept;
[[nodiscard]] std::string file_key(const Identity &IdentityValue);
[[nodiscard]] std::string lookup_key(const Identity &IdentityValue);
[[nodiscard]] std::string compiled_key(const Identity &IdentityValue);
[[nodiscard]] cache::FormatError finalise_identity(Identity &IdentityValue);
#ifdef UNIT_TESTS
[[nodiscard]] cache::FormatError encode_interface(const Interface &, std::string &Output);
#endif
[[nodiscard]] cache::FormatError decode_interface(std::string_view Bytes, Interface &Output);
[[nodiscard]] cache::FormatError encode_envelope(
   const Identity &, const FinalisedInterface &, std::string_view Payload, std::string &Output);
[[nodiscard]] cache::FormatError decode_envelope(std::string_view Input, EnvelopeView &Output,
   InterfaceOperationCounters *Counters = nullptr);

} // namespace tiri::import_cache
