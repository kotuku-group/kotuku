#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>

#include "../defs.h"
#include "../import_module_bundle.h"
#include "../import_module_format.h"
#include "../lua.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <ranges>

#ifdef UNIT_TESTS
namespace {

using namespace tiri::import_cache;

ValueDescriptor number_value()
{
   ValueDescriptor result;
   result.Kind = ValueKind::NUMBER;
   result.Proof = ProofKind::CHECKED;
   result.Nullable = false;
   return result;
}

ValueDescriptor structure_value(std::string Name)
{
   ValueDescriptor result;
   result.Kind = ValueKind::STRUCTURE;
   result.Proof = ProofKind::CLOSED;
   result.Structure = std::move(Name);
   result.Nullable = false;
   return result;
}

Interface sample_interface()
{
   Interface result;
   result.Package = tiri::PackageIdentity { "geometry", "2.4.1" };
   result.Namespaces = { { "geometry", NamespaceMode::DECLARE }, { "geometry", NamespaceMode::JOIN } };

   CallableDescriptor callable;
   callable.Parameters = { { structure_value("Point"), "point", 0, true, true } };
   callable.Results = { number_value() };
   callable.DeclaredResults = 1;
   result.Exports.push_back({ "geometry.length", ExportKind::GLOBAL,
      ValueDescriptor { .Kind = ValueKind::FUNCTION, .Proof = ProofKind::CLOSED, .Nullable = false },
      callable, {}, true });

   ConstantValue constant;
   constant.Kind = ConstantKind::INTEGER;
   constant.Integer = 42;
   result.Exports.push_back({ "geometry.answer", ExportKind::ENUM_CONSTANT, number_value(), {}, constant, true });
   result.Exports.push_back({ "external_scale", ExportKind::EXTERN, number_value(), {}, {}, false });

   StructureDescriptor point;
   point.Name = "Point";
   point.Fields = { { "y", number_value(), true }, { "x", number_value(), true } };
   result.Structures.push_back(std::move(point));
   result.Enums = { { "Axis", { { "Y", { .Kind = ConstantKind::INTEGER, .Integer = 1 } },
      { "X", { .Kind = ConstantKind::INTEGER, .Integer = 0 } } } } };
   result.StructureManifest = std::string("\x01\x00", 2);
   result.NativeDependencies = { { "vector", { "length", "normalise" }, 1 } };
   result.Sources = {
      { "packages:geometry.tiri", "geometry", "geometry.tiri", "geometry", "", 1, 80, 0 },
      { "packages:geometry/local.tiri", "./local", "local.tiri", "", "packages:geometry.tiri", 81, 12, 4 }
   };
   result.NestedModules = { { "math/constants", "packages:math/constants.tiri",
      tiri::cache::content_digest("constants-interface") } };
   return result;
}

Identity sample_identity()
{
   Identity result;
   result.BuildIdentity = "build:0123456789abcdef";
   result.LogicalRequest = "geometry";
   result.DeclaredPackage = tiri::PackageIdentity { "geometry", "2.4.1" };
   result.Source = { "packages:geometry.tiri", 123, 456, tiri::cache::content_digest("geometry source") };
   result.Options = { { "optimisation", "debug" }, { "language", "tiri" } };
   result.LocalImports = { { result.Source.ResolvedPath, "./local",
      { "packages:geometry/local.tiri", 12, 457, tiri::cache::content_digest("local source") } } };
   result.ModuleDependencies = { { "math/constants", "packages:math/constants.tiri",
      tiri::cache::content_digest("constants-interface"), "compiled-constants-v1" } };
   result.ResolutionInputs = { { "package:geometry", result.Source.ResolvedPath,
      "/opt/kotuku/packages/geometry/2.4.1/geometry.tiri", "2.4.1", "2.4.1", true } };
   result.ConditionalInputs = { { tiri::cache::ConditionalKind::IMPORTED, "imported",
      result.Source.ResolvedPath, "true" } };
   return result;
}

cache::FormatError encode_fixture_envelope(
   const Identity &IdentityValue, Interface InterfaceValue, std::string_view Payload, std::string &Output)
{
   FinalisedInterfacePtr finalised;
   auto error = FinalisedInterface::finalise(std::move(InterfaceValue), finalised);
   if (error != cache::FormatError::OKAY) return error;
   return encode_envelope(IdentityValue, *finalised, Payload, Output);
}

FinalisedInterfacePtr finalise_fixture(Interface InterfaceValue)
{
   FinalisedInterfacePtr result;
   if (FinalisedInterface::finalise(std::move(InterfaceValue), result) != cache::FormatError::OKAY) return {};
   return result;
}

std::string encode_raw_root_module_bundle(const std::vector<RootModuleRecord> &Records)
{
   auto append_uleb = [](std::string &Output, uint32_t Value) {
      do {
         uint8_t byte = uint8_t(Value & 0x7f);
         Value >>= 7;
         if (Value) byte |= 0x80;
         Output.push_back(char(byte));
      } while (Value);
   };

   std::string result { char(ROOT_BUNDLE_VERSION) };
   append_uleb(result, uint32_t(Records.size()));
   for (const auto &record : Records) {
      append_uleb(result, uint32_t(record.LookupIdentity.size()));
      result += record.LookupIdentity;
      append_uleb(result, uint32_t(record.CompiledIdentity.size()));
      result += record.CompiledIdentity;
      result.push_back(char(record.SourceIndex));
      append_uleb(result, uint32_t(record.Dependencies.size()));
      for (uint32_t dependency : record.Dependencies) append_uleb(result, dependency);
      append_uleb(result, uint32_t(record.InterfaceBytes.size()));
      result += record.InterfaceBytes;
   }
   return result;
}

bool round_trip_and_determinism(kt::Log &Log)
{
   auto identity = sample_identity();
   auto interface_value = sample_interface();
   const std::string payload("\x1bLJ\x02module-bytecode", 18);
   std::string encoded;
   if (encode_fixture_envelope(identity, interface_value, payload, encoded) != tiri::cache::FormatError::OKAY or
       not is_envelope(encoded)) {
      Log.error("Import-module envelope encoding failed");
      return false;
   }

   EnvelopeView decoded;
   if (decode_envelope(encoded, decoded) != tiri::cache::FormatError::OKAY or
       (decoded.Payload != payload) or
       (decoded.CompilationIdentity.LogicalRequest != identity.LogicalRequest) or
       (decoded.CompilationIdentity.Source.ContentDigest != identity.Source.ContentDigest) or
       (decoded.CompilationIdentity.ResolutionInputs != identity.ResolutionInputs) or
       decoded.LookupIdentity != lookup_key(identity) or decoded.CompiledIdentity != compiled_key(identity) or
       not decoded.CompileTimeInterface or
       (decoded.CompileTimeInterface->descriptors().Exports.size() != interface_value.Exports.size()) or
       (decoded.CompileTimeInterface->digest() != finalise_fixture(interface_value)->digest())) {
      Log.error("Import-module envelope did not preserve identity, interface and payload sections");
      return false;
   }

   std::ranges::reverse(identity.Options);
   std::ranges::reverse(interface_value.Exports);
   std::ranges::reverse(interface_value.Structures[0].Fields);
   std::ranges::reverse(interface_value.Enums[0].Members);
   std::ranges::reverse(interface_value.NativeDependencies[0].Functions);
   std::string reordered;
   if (encode_fixture_envelope(identity, interface_value, payload, reordered) != tiri::cache::FormatError::OKAY or
       reordered != encoded or file_key(identity) != file_key(sample_identity())) {
      Log.error("Canonical import-module output depends on source container ordering");
      return false;
   }

   auto changed = sample_interface();
   changed.Exports[0].Callable->Results[0].Nullable = true;
   if (finalise_fixture(changed)->digest() IS finalise_fixture(sample_interface())->digest()) {
      Log.error("A compile-time surface change did not change the interface digest");
      return false;
   }
   return true;
}

bool package_identity_validation(kt::Log &Log)
{
   constexpr std::string_view valid_names[] = {
      "a", "gui", "net/url", "a1/b2-c3"
   };
   for (auto name : valid_names) {
      if (tiri::validate_package_name(name) != tiri::PackageValidationError::OKAY) {
         Log.error("Valid package name '%.*s' was rejected", int(name.size()), name.data());
         return false;
      }
   }
   constexpr std::string_view invalid_names[] = {
      "", "Gui", "gui_tools", "/gui", "gui/", "gui//tools", "-gui", "gui-", "gui--tools", "../gui"
   };
   for (auto name : invalid_names) {
      if (tiri::validate_package_name(name) IS tiri::PackageValidationError::OKAY) {
         Log.error("Invalid package name '%.*s' was accepted", int(name.size()), name.data());
         return false;
      }
   }

   constexpr std::string_view ordered_versions[] = { "1", "1.0", "1.0.1", "1.1", "1.10", "2" };
   tiri::ParsedPackageVersion previous;
   if (tiri::parse_package_version(ordered_versions[0], &previous) != tiri::PackageValidationError::OKAY) return false;
   for (size_t i = 1; i < std::size(ordered_versions); ++i) {
      tiri::ParsedPackageVersion current;
      if (tiri::parse_package_version(ordered_versions[i], &current) != tiri::PackageValidationError::OKAY or
          not (previous < current)) {
         Log.error("Package version ordering failed at '%.*s'", int(ordered_versions[i].size()),
            ordered_versions[i].data());
         return false;
      }
      previous = current;
   }
   constexpr std::string_view invalid_versions[] = {
      "", "01", ".1", "1.", "1..2", "+1", "1-beta", "1 2", "4294967296"
   };
   for (auto version : invalid_versions) {
      if (tiri::parse_package_version(version) IS tiri::PackageValidationError::OKAY) {
         Log.error("Invalid package version '%.*s' was accepted", int(version.size()), version.data());
         return false;
      }
   }

   std::string longest_name(tiri::MAX_PACKAGE_NAME_COMPONENT_LENGTH, 'a');
   if (tiri::validate_package_name(longest_name) != tiri::PackageValidationError::OKAY) return false;
   longest_name.push_back('a');
   if (tiri::validate_package_name(longest_name) != tiri::PackageValidationError::COMPONENT_LENGTH) return false;

   std::string maximum_name;
   for (size_t i = 0; i < 4; ++i) {
      if (not maximum_name.empty()) maximum_name += '/';
      maximum_name.append(tiri::MAX_PACKAGE_NAME_COMPONENT_LENGTH, 'a');
   }
   if (maximum_name.size() != tiri::MAX_PACKAGE_NAME_LENGTH or
       tiri::validate_package_name(maximum_name) != tiri::PackageValidationError::OKAY or
       tiri::validate_package_name(maximum_name + "a") != tiri::PackageValidationError::TOTAL_LENGTH) {
      Log.error("Package name total-length boundary is incorrect");
      return false;
   }

   tiri::ParsedPackageVersion maximum_component;
   if (tiri::parse_package_version("4294967295", &maximum_component) != tiri::PackageValidationError::OKAY or
       maximum_component.Components[0] != (std::numeric_limits<uint32_t>::max)()) {
      Log.error("Package version component-value boundary is incorrect");
      return false;
   }

   std::string overlong_version(tiri::MAX_PACKAGE_VERSION_LENGTH + 1, '1');
   if (tiri::parse_package_version(overlong_version) != tiri::PackageValidationError::TOTAL_LENGTH) {
      Log.error("Package version total-length boundary is incorrect");
      return false;
   }

   std::string sixteen_components = "1";
   for (size_t i = 1; i < tiri::MAX_PACKAGE_VERSION_COMPONENTS; ++i) sixteen_components += ".1";
   if (tiri::parse_package_version(sixteen_components) != tiri::PackageValidationError::OKAY or
       tiri::parse_package_version(sixteen_components + ".1") != tiri::PackageValidationError::COMPONENT_COUNT) {
      Log.error("Package version component-count boundary is incorrect");
      return false;
   }
   return true;
}

bool compiled_identity_and_graph_assembly(kt::Log &Log)
{
   auto parent = sample_identity();
   auto reordered = parent;
   std::ranges::reverse(reordered.Options);
   std::ranges::reverse(reordered.ResolutionInputs);
   std::ranges::reverse(reordered.ConditionalInputs);
   if (compiled_key(parent) != compiled_key(reordered) or lookup_key(parent) != lookup_key(reordered)) {
      Log.error("Canonical module identities depend on source container ordering");
      return false;
   }

   auto changed_dependency = parent;
   changed_dependency.ModuleDependencies[0].CompiledIdentity = "compiled-constants-v2";
   if (lookup_key(parent) != lookup_key(changed_dependency) or
       compiled_key(parent) IS compiled_key(changed_dependency)) {
      Log.error("A dependency implementation change did not affect only the parent's compiled identity");
      return false;
   }

   auto changed_hint = parent;
   changed_hint.Source.ModifiedHint++;
   if (compiled_key(parent) != compiled_key(changed_hint) or lookup_key(parent) IS lookup_key(changed_hint)) {
      Log.error("A source timestamp hint became semantic or failed to select a fresh lookup candidate");
      return false;
   }

   auto changed_package = parent;
   changed_package.DeclaredPackage = tiri::PackageIdentity { "geometry", "2.4.2" };
   if (lookup_key(parent) != lookup_key(changed_package) or compiled_key(parent) IS compiled_key(changed_package)) {
      Log.error("A package declaration change did not affect only the compiled identity");
      return false;
   }

   auto undeclared_package = parent;
   undeclared_package.DeclaredPackage.reset();
   if (lookup_key(parent) != lookup_key(undeclared_package) or
       compiled_key(parent) IS compiled_key(undeclared_package)) {
      Log.error("Discovering a package declaration changed its lookup key or failed to change its compiled identity");
      return false;
   }

   auto expected_package = undeclared_package;
   expected_package.ExpectedPackage = parent.DeclaredPackage;
   if (lookup_key(undeclared_package) IS lookup_key(expected_package) or
       compiled_key(undeclared_package) IS compiled_key(expected_package)) {
      Log.error("A resolver package expectation failed to change lookup and compiled identities");
      return false;
   }

   constexpr std::array<void (*)(Identity &), 3> mutations = {
      [](Identity &Value) { Value.BuildIdentity += "-other"; },
      [](Identity &Value) { Value.Options[0].Value += "-other"; },
      [](Identity &Value) { Value.ConditionalInputs[0].Value = "false"; }
   };
   for (auto mutate : mutations) {
      auto distinct = parent;
      mutate(distinct);
      if (compiled_key(parent) IS compiled_key(distinct)) {
         Log.error("A build, option or observed condition failed to separate compiled identities");
         return false;
      }
   }

   std::string interface_bytes;
   if (encode_interface(sample_interface(), interface_bytes) != tiri::cache::FormatError::OKAY) return false;
   std::vector<RootModuleRecord> input = {
      { "lookup-b", "compiled-parent-b", interface_bytes, { 3 }, 7 },
      { "lookup-a", "compiled-parent-a", interface_bytes, { 2, 3 }, 6 },
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 9 },
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 2 }
   };
   RootModuleGraphAssembly assembled;
   if (assemble_root_module_graph(input, assembled) != tiri::cache::FormatError::OKAY or
       assembled.records().size() != 3 or assembled.records()[0].CompiledIdentity != "compiled-leaf" or
       assembled.records()[0].SourceIndex != 2 or
       assembled.records()[1].Dependencies != std::vector<uint32_t>{ 0 } or
       assembled.records()[2].Dependencies != std::vector<uint32_t>{ 0 } or
       assembled.input_to_canonical() != std::vector<uint32_t>({ 2, 1, 0, 0 })) {
      Log.error("Module graph assembly did not intern and order equivalent dependency records");
      return false;
   }

   auto conflict = input;
   conflict[3].LookupIdentity = "conflicting-lookup";
   RootModuleGraphAssembly rejected;
   if (assemble_root_module_graph(conflict, rejected) != tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("Module graph assembly accepted conflicting records for one compiled identity");
      return false;
   }

   std::string encoded;
   std::vector<RootModuleRecord> decoded;
   auto encode_error = encode_root_module_bundle(assembled, encoded);
   auto decode_error = encode_error IS tiri::cache::FormatError::OKAY ?
      decode_root_module_bundle(encoded, decoded) : encode_error;
   if (encode_error != tiri::cache::FormatError::OKAY or decode_error != tiri::cache::FormatError::OKAY or
       decoded.size() != 3) {
      Log.error("The assembled module graph did not survive its wire-format round trip: encode=%s decode=%s size=%d",
         tiri::cache::format_error_name(encode_error), tiri::cache::format_error_name(decode_error),
         int(decoded.size()));
      return false;
   }
   std::vector<RootModuleRecordLocation> locations;
   if (index_root_module_bundle(encoded, locations) != tiri::cache::FormatError::OKAY or
       locations.size() != assembled.records().size()) {
      Log.error("Root graph interfaces were not indexed");
      return false;
   }
   for (size_t i = 0; i < locations.size(); ++i) {
      const auto &location = locations[i];
      if (location.InterfaceOffset > encoded.size() or
          location.InterfaceSize > encoded.size() - location.InterfaceOffset or
          std::string_view(encoded).substr(location.InterfaceOffset, location.InterfaceSize) !=
             assembled.records()[i].InterfaceBytes) {
         Log.error("Root graph interface index %d did not identify its canonical bytes", int(i));
         return false;
      }
   }
   std::string expected = encode_raw_root_module_bundle(assembled.records());
   if (encoded != expected) {
      Log.error("Root graph encoding changed the locked version 3 field representation");
      return false;
   }

   std::vector<RootModuleRecord> permutation = {
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 2 },
      { "lookup-a", "compiled-parent-a", interface_bytes, { 0, 2 }, 6 },
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 9 },
      { "lookup-b", "compiled-parent-b", interface_bytes, { 2 }, 7 }
   };
   RootModuleGraphAssembly permuted;
   std::string permuted_bytes;
   if (assemble_root_module_graph(permutation, permuted) != tiri::cache::FormatError::OKAY or
       permuted.input_to_canonical() != std::vector<uint32_t>({ 0, 1, 0, 2 }) or
       encode_root_module_bundle(permuted, permuted_bytes) != tiri::cache::FormatError::OKAY or
       permuted_bytes != encoded) {
      Log.error("Equivalent graph permutations changed canonical version 3 bytes or index mappings");
      return false;
   }
   return true;
}

bool root_module_graph_validation(kt::Log &Log)
{
   std::string interface_bytes;
   if (encode_interface(Interface {}, interface_bytes) != tiri::cache::FormatError::OKAY) return false;
   auto record = [&](std::string Identity, std::vector<uint32_t> Dependencies = {}) {
      return RootModuleRecord { "lookup-" + Identity, std::move(Identity), interface_bytes,
         std::move(Dependencies), 1 };
   };
   auto expect_error = [&](std::vector<RootModuleRecord> Input, tiri::cache::FormatError Expected,
                           const char *Description) {
      RootModuleGraphAssembly result;
      if (assemble_root_module_graph({ record("sentinel") }, result) != tiri::cache::FormatError::OKAY) return false;
      auto error = assemble_root_module_graph(Input, result);
      if (error != Expected or not result.records().empty() or not result.input_to_canonical().empty()) {
         Log.error("Root graph %s returned %s and left %d records", Description,
            tiri::cache::format_error_name(error), int(result.records().size()));
         return false;
      }
      return true;
   };

   if (not expect_error({ record("self", { 0 }) }, tiri::cache::FormatError::INVALID_METADATA, "self edge") or
       not expect_error({ record("duplicate", { 1, 1 }), record("leaf") },
          tiri::cache::FormatError::INVALID_METADATA, "repeated input dependency") or
       not expect_error({ record("a", { 1 }), record("b", { 0 }) },
          tiri::cache::FormatError::INVALID_METADATA, "cycle") or
       not expect_error({ record("same", { 1 }), record("same") },
          tiri::cache::FormatError::INVALID_METADATA, "equivalent self edge")) return false;

   auto malformed = record("malformed");
   malformed.InterfaceBytes = "invalid";
   if (not expect_error({ malformed }, tiri::cache::FormatError::INVALID_METADATA, "malformed interface")) {
      return false;
   }
   std::string different_interface;
   if (encode_interface(sample_interface(), different_interface) != tiri::cache::FormatError::OKAY) return false;
   auto interface_conflict = std::vector<RootModuleRecord> { record("same"), record("same") };
   interface_conflict[1].InterfaceBytes = different_interface;
   auto dependency_conflict = std::vector<RootModuleRecord> {
      record("same", { 2 }), record("same"), record("leaf")
   };
   if (not expect_error(interface_conflict, tiri::cache::FormatError::INVALID_METADATA,
           "conflicting duplicate interface") or
       not expect_error(dependency_conflict, tiri::cache::FormatError::INVALID_METADATA,
          "conflicting duplicate dependencies")) return false;

   std::vector<RootModuleRecord> maximum_depth;
   maximum_depth.reserve(MAX_ROOT_MODULE_DEPTH);
   for (uint32_t i = 0; i < MAX_ROOT_MODULE_DEPTH; ++i) {
      maximum_depth.push_back(record(std::format("depth-{:03}", i), i ? std::vector<uint32_t>{ i - 1 } :
         std::vector<uint32_t>{}));
   }
   RootModuleGraphAssembly depth_assembly;
   if (assemble_root_module_graph(maximum_depth, depth_assembly) != tiri::cache::FormatError::OKAY) {
      Log.error("Root graph rejected the maximum supported depth");
      return false;
   }
   maximum_depth.push_back(record("depth-overflow", { MAX_ROOT_MODULE_DEPTH - 1 }));
   if (not expect_error(maximum_depth, tiri::cache::FormatError::COUNT_LIMIT, "depth overflow")) return false;

   std::vector<RootModuleRecord> no_edges;
   for (uint32_t i = 0; i < 512; ++i) no_edges.push_back(record(std::format("node-{:03}", 511 - i)));
   RootModuleGraphAssembly linear;
   if (assemble_root_module_graph(no_edges, linear) != tiri::cache::FormatError::OKAY or
       linear.work().RecordInspections != no_edges.size() or
       linear.work().InterfaceValidations != no_edges.size() or
       linear.work().QueuePops != no_edges.size() or linear.work().EdgeTraversals != 0) {
      Log.error("Root graph no-edge work did not remain linear");
      return false;
   }

   std::vector<RootModuleRecord> wire(linear.records().begin(), linear.records().begin() + 2);
   wire[0].Dependencies.clear();
   wire[1].Dependencies.clear();
   std::string valid = encode_raw_root_module_bundle(wire);
   auto expect_wire_rejection = [&](std::vector<RootModuleRecord> Invalid, const char *Description) {
      std::string bytes = encode_raw_root_module_bundle(Invalid);
      std::vector<RootModuleRecord> decoded;
      if (decode_root_module_bundle(bytes, decoded) IS tiri::cache::FormatError::OKAY) {
         Log.error("Root bundle decoder accepted %s", Description);
         return false;
      }
      return true;
   };
   auto noncanonical_order = wire;
   std::ranges::swap(noncanonical_order[0], noncanonical_order[1]);
   auto forward_dependency = wire;
   forward_dependency[0].Dependencies = { 1 };
   auto duplicate_dependency = wire;
   duplicate_dependency[1].Dependencies = { 0, 0 };
   std::vector<RootModuleRecord> decoded;
   std::vector<RootModuleRecordLocation> locations = { { 1, 1 } };
   if (not expect_wire_rejection(noncanonical_order, "non-canonical ready-node order") or
       not expect_wire_rejection(forward_dependency, "forward dependency") or
       not expect_wire_rejection(duplicate_dependency, "duplicate dependency") or
       decode_root_module_bundle(valid + "x", decoded) IS tiri::cache::FormatError::OKAY or
       index_root_module_bundle(valid + "x", locations) IS tiri::cache::FormatError::OKAY or
       not locations.empty()) return false;
   return true;
}

bool malformed_and_bounds(kt::Log &Log)
{
   auto identity = sample_identity();
   auto interface_value = sample_interface();
   std::string encoded;
   if (encode_fixture_envelope(identity, interface_value, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::OKAY) return false;

   EnvelopeView output;
   auto expect = [&](std::string_view Input, tiri::cache::FormatError Expected) {
      auto result = decode_envelope(Input, output);
      if (result != Expected) {
         Log.error("Malformed import module returned %s instead of %s",
            tiri::cache::format_error_name(result), tiri::cache::format_error_name(Expected));
         return false;
      }
      return true;
   };
   if (not expect(encoded.substr(0, 7), tiri::cache::FormatError::NOT_CACHE) or
       not expect(encoded.substr(0, 40), tiri::cache::FormatError::TRUNCATED)) return false;

   auto unsupported = encoded;
   unsupported[8] = 10;
   if (not expect(unsupported, tiri::cache::FormatError::UNSUPPORTED_VERSION)) return false;

   auto damaged_interface = encoded;
   uint32_t identity_size = 0;
   for (int i = 0; i < 4; ++i) identity_size |= uint32_t(uint8_t(encoded[12 + i])) << (i * 8);
   damaged_interface[132 + identity_size] ^= 0x01;
   if (not expect(damaged_interface, tiri::cache::FormatError::INVALID_METADATA)) return false;

   auto damaged_payload = encoded;
   damaged_payload.back() ^= 0x01;
   if (not expect(damaged_payload, tiri::cache::FormatError::INVALID_PAYLOAD)) return false;

   auto unknown_decoded_kind = encoded;
   uint32_t interface_size = 0;
   for (int i = 0; i < 4; ++i) interface_size |= uint32_t(uint8_t(encoded[16 + i])) << (i * 8);
   const size_t interface_offset = 132 + identity_size;
   unknown_decoded_kind[interface_offset + 1 + 4 + std::string_view("geometry").size() + 4 +
      std::string_view("2.4.1").size() + 4 + 2 + 4 + 4 + std::string_view("geometry").size()] = char(255);
   auto interface_hash = tiri::cache::content_digest(
      std::string_view(unknown_decoded_kind).substr(interface_offset, interface_size));
   std::memcpy(unknown_decoded_kind.data() + 68, interface_hash.data(), interface_hash.size());
   if (not expect(unknown_decoded_kind, tiri::cache::FormatError::INVALID_ENUM)) return false;

   auto duplicate = sample_interface();
   duplicate.Exports.push_back(duplicate.Exports[0]);
   if (encode_fixture_envelope(identity, duplicate, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("A duplicate exported binding was accepted");
      return false;
   }

   auto mixed_kind_conflict = sample_interface();
   auto conflicting_export = mixed_kind_conflict.Exports[0];
   conflicting_export.Kind = ExportKind::EXTERN;
   mixed_kind_conflict.Exports.push_back(std::move(conflicting_export));
   if (encode_fixture_envelope(identity, mixed_kind_conflict, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("Conflicting export kinds for the same binding were accepted");
      return false;
   }

   auto conflicting_namespace = sample_interface();
   conflicting_namespace.Namespaces.push_back({ "geometry", NamespaceMode::DECLARE });
   if (encode_fixture_envelope(identity, conflicting_namespace, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("A conflicting namespace declaration was accepted");
      return false;
   }

   auto too_many = sample_interface();
   too_many.Sources.resize(MAX_INTERFACE_RECORDS + 1);
   if (encode_fixture_envelope(identity, too_many, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::COUNT_LIMIT) {
      Log.error("The interface record count bound was not enforced");
      return false;
   }

   auto unknown_kind = sample_interface();
   unknown_kind.Exports[0].Kind = ExportKind(255);
   if (encode_fixture_envelope(identity, unknown_kind, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_ENUM) {
      Log.error("An unknown interface record kind was accepted");
      return false;
   }
   return true;
}



} // namespace

void import_module_format_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ImportModuleFormatTests");
   for (auto test : { package_identity_validation, round_trip_and_determinism, compiled_identity_and_graph_assembly,
      root_module_graph_validation, malformed_and_bounds }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
