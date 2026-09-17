#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/tiri.h>

#include "../defs.h"
#include "../import_module_bundle.h"
#include "../import_module_format.h"
#include "../lua.hpp"

#include <algorithm>
#include <cstring>
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
   result.NativeDependencies = { { "vector", { "length", "normalise" }, 1 } };
   result.Sources = {
      { "scripts:geometry.tiri", "geometry", "geometry.tiri", "geometry", "", 1, 80, 0 },
      { "scripts:geometry/local.tiri", "./local", "local.tiri", "", "scripts:geometry.tiri", 81, 12, 4 }
   };
   result.NestedModules = { { "math/constants", "scripts:math/constants.tiri",
      tiri::cache::content_digest("constants-interface") } };
   return result;
}

Identity sample_identity()
{
   Identity result;
   result.BuildIdentity = "build:0123456789abcdef";
   result.LogicalRequest = "geometry";
   result.Source = { "scripts:geometry.tiri", 123, 456, tiri::cache::content_digest("geometry source") };
   result.Options = { { "optimisation", "debug" }, { "language", "tiri" } };
   result.LocalImports = { { result.Source.ResolvedPath, "./local",
      { "scripts:geometry/local.tiri", 12, 457, tiri::cache::content_digest("local source") } } };
   result.ModuleDependencies = { { "math/constants", "scripts:math/constants.tiri",
      tiri::cache::content_digest("constants-interface"), "compiled-constants-v1" } };
   result.ResolutionInputs = { { "volume:scripts", result.Source.ResolvedPath, "/opt/kotuku/scripts/" } };
   result.ConditionalInputs = { { tiri::cache::ConditionalKind::IMPORTED, "imported",
      result.Source.ResolvedPath, "true" } };
   return result;
}

bool round_trip_and_determinism(kt::Log &Log)
{
   auto identity = sample_identity();
   auto interface_value = sample_interface();
   const std::string payload("\x1bLJ\x02module-bytecode", 18);
   std::string encoded;
   if (encode_envelope(identity, interface_value, payload, encoded) != tiri::cache::FormatError::OKAY or
       not is_envelope(encoded)) {
      Log.error("Import-module envelope encoding failed");
      return false;
   }

   EnvelopeView decoded;
   if (decode_envelope(encoded, decoded) != tiri::cache::FormatError::OKAY or
       (decoded.Payload != payload) or
       (decoded.CompilationIdentity.LogicalRequest != identity.LogicalRequest) or
       (decoded.CompilationIdentity.Source.ContentDigest != identity.Source.ContentDigest) or
       decoded.LookupIdentity != lookup_key(identity) or decoded.CompiledIdentity != compiled_key(identity) or
       (decoded.CompileTimeInterface.Exports.size() != interface_value.Exports.size()) or
       (decoded.InterfaceDigest != interface_digest(interface_value))) {
      Log.error("Import-module envelope did not preserve identity, interface and payload sections");
      return false;
   }

   std::ranges::reverse(identity.Options);
   std::ranges::reverse(interface_value.Exports);
   std::ranges::reverse(interface_value.Structures[0].Fields);
   std::ranges::reverse(interface_value.Enums[0].Members);
   std::ranges::reverse(interface_value.NativeDependencies[0].Functions);
   std::string reordered;
   if (encode_envelope(identity, interface_value, payload, reordered) != tiri::cache::FormatError::OKAY or
       reordered != encoded or file_key(identity) != file_key(sample_identity())) {
      Log.error("Canonical import-module output depends on source container ordering");
      return false;
   }

   auto changed = sample_interface();
   changed.Exports[0].Callable->Results[0].Nullable = true;
   if (interface_digest(changed) IS interface_digest(sample_interface())) {
      Log.error("A compile-time surface change did not change the interface digest");
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
      { "lookup-a", "compiled-parent-a", interface_bytes, { 2 }, 6 },
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 9 },
      { "lookup-leaf", "compiled-leaf", interface_bytes, {}, 2 }
   };
   std::vector<RootModuleRecord> assembled;
   if (assemble_root_module_graph(input, assembled) != tiri::cache::FormatError::OKAY or
       assembled.size() != 3 or assembled[0].CompiledIdentity != "compiled-leaf" or
       assembled[0].SourceIndex != 2 or assembled[1].Dependencies != std::vector<uint32_t>{ 0 } or
       assembled[2].Dependencies != std::vector<uint32_t>{ 0 }) {
      Log.error("Module graph assembly did not intern and order equivalent dependency records");
      return false;
   }

   auto conflict = input;
   conflict[3].LookupIdentity = "conflicting-lookup";
   if (assemble_root_module_graph(conflict, assembled) != tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("Module graph assembly accepted conflicting records for one compiled identity");
      return false;
   }

   std::string encoded;
   std::vector<RootModuleRecord> decoded;
   if (encode_root_module_bundle(input, encoded) != tiri::cache::FormatError::OKAY or
       decode_root_module_bundle(encoded, decoded) != tiri::cache::FormatError::OKAY or decoded.size() != 3) {
      Log.error("The assembled module graph did not survive its wire-format round trip");
      return false;
   }
   return true;
}

bool malformed_and_bounds(kt::Log &Log)
{
   auto identity = sample_identity();
   auto interface_value = sample_interface();
   std::string encoded;
   if (encode_envelope(identity, interface_value, std::string("\x1bLJ", 3), encoded) !=
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
   unsupported[8] = 2;
   if (not expect(unsupported, tiri::cache::FormatError::UNSUPPORTED_VERSION)) return false;

   auto damaged_interface = encoded;
   uint32_t identity_size = 0;
   for (int i = 0; i < 4; ++i) identity_size |= uint32_t(uint8_t(encoded[12 + i])) << (i * 8);
   damaged_interface[124 + identity_size] ^= 0x01;
   if (not expect(damaged_interface, tiri::cache::FormatError::INVALID_METADATA)) return false;

   auto damaged_payload = encoded;
   damaged_payload.back() ^= 0x01;
   if (not expect(damaged_payload, tiri::cache::FormatError::INVALID_PAYLOAD)) return false;

   auto unknown_decoded_kind = encoded;
   uint32_t interface_size = 0;
   for (int i = 0; i < 4; ++i) interface_size |= uint32_t(uint8_t(encoded[16 + i])) << (i * 8);
   const size_t interface_offset = 124 + identity_size;
   unknown_decoded_kind[interface_offset + 4 + 4 + std::string_view("geometry").size()] = char(255);
   auto interface_hash = tiri::cache::content_digest(
      std::string_view(unknown_decoded_kind).substr(interface_offset, interface_size));
   std::memcpy(unknown_decoded_kind.data() + 60, interface_hash.data(), interface_hash.size());
   if (not expect(unknown_decoded_kind, tiri::cache::FormatError::INVALID_ENUM)) return false;

   auto duplicate = sample_interface();
   duplicate.Exports.push_back(duplicate.Exports[0]);
   if (encode_envelope(identity, duplicate, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("A duplicate exported binding was accepted");
      return false;
   }

   auto mixed_kind_conflict = sample_interface();
   auto conflicting_export = mixed_kind_conflict.Exports[0];
   conflicting_export.Kind = ExportKind::EXTERN;
   mixed_kind_conflict.Exports.push_back(std::move(conflicting_export));
   if (encode_envelope(identity, mixed_kind_conflict, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("Conflicting export kinds for the same binding were accepted");
      return false;
   }

   auto conflicting_namespace = sample_interface();
   conflicting_namespace.Namespaces.push_back({ "geometry", NamespaceMode::DECLARE });
   if (encode_envelope(identity, conflicting_namespace, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_METADATA) {
      Log.error("A conflicting namespace declaration was accepted");
      return false;
   }

   auto too_many = sample_interface();
   too_many.Sources.resize(MAX_INTERFACE_RECORDS + 1);
   if (encode_envelope(identity, too_many, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::COUNT_LIMIT) {
      Log.error("The interface record count bound was not enforced");
      return false;
   }

   auto unknown_kind = sample_interface();
   unknown_kind.Exports[0].Kind = ExportKind(255);
   if (encode_envelope(identity, unknown_kind, std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::INVALID_ENUM) {
      Log.error("An unknown interface record kind was accepted");
      return false;
   }
   return true;
}

bool staged_installation(kt::Log &Log)
{
   auto source = sample_interface();
   CompileTimeContext context;
   context.Bindings.push_back({ "sentinel", ExportKind::GLOBAL, number_value(), {}, {}, false });
   if (install_interface(source, context) != tiri::cache::FormatError::OKAY or
       context.Namespaces != source.Namespaces or context.Bindings != source.Exports or
       context.Structures != source.Structures or context.Enums != source.Enums or
       context.NativeDependencies != source.NativeDependencies or
       context.Sources != source.Sources or context.NestedModules != source.NestedModules) {
      Log.error("A decoded interface did not install equivalent compile-time descriptors");
      return false;
   }

   auto invalid = source;
   invalid.Exports[0].Value.Structure = "MissingType";
   const auto snapshot = context.Bindings;
   if (install_interface(invalid, context) != tiri::cache::FormatError::INVALID_METADATA or
       context.Bindings != snapshot) {
      Log.error("Rejected interface installation mutated the compile-time context");
      return false;
   }

   auto mixed_kind_conflict = source;
   auto conflicting_export = mixed_kind_conflict.Exports[0];
   conflicting_export.Kind = ExportKind::EXTERN;
   mixed_kind_conflict.Exports.push_back(std::move(conflicting_export));
   if (install_interface(mixed_kind_conflict, context) != tiri::cache::FormatError::INVALID_METADATA or
       context.Bindings != snapshot) {
      Log.error("Mixed-kind export conflict was installed or mutated the compile-time context");
      return false;
   }
   return true;
}

bool direct_load_rejection(kt::Log &Log)
{
   std::string encoded;
   if (encode_envelope(sample_identity(), sample_interface(), std::string("\x1bLJ", 3), encoded) !=
       tiri::cache::FormatError::OKAY) return false;

   const std::string path = "temp:tiri-i02-import-only.tbc";
   DeleteFile(path, nullptr);
   struct Cleanup {
      const std::string &Path;
      ~Cleanup() { DeleteFile(Path, nullptr); }
   } cleanup { path };
   {
      objFile::create file = { fl::Path(path), fl::Flags(FL::NEW|FL::WRITE) };
      int written = 0;
      if (not file.ok() or
          file->write(std::span((const int8_t *)encoded.data(), encoded.size()), &written) != ERR::Okay or
          written != int(encoded.size()) or file->flush() != ERR::Okay) return false;
   }

   objTiri::create script = { fl::Path(path) };
   if (not script.ok()) return false;
   auto state = (extTiri *)*script;
   const int stack_top = lua_gettop(state->Lua);
   if (acQuery(*script) != ERR::InvalidData or lua_gettop(state->Lua) != stack_top or state->MainChunkRef) {
      Log.error("Direct Script loading accepted or partially installed an import-only artefact");
      return false;
   }
   return true;
}

} // namespace

void import_module_format_unit_tests(int &Passed, int &Total)
{
   kt::Log log("ImportModuleFormatTests");
   for (auto test : { round_trip_and_determinism, compiled_identity_and_graph_assembly,
      malformed_and_bounds, staged_installation, direct_load_rejection }) {
      Total++;
      if (test(log)) Passed++;
   }
}
#endif
