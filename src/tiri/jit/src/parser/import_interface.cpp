// This import code deserialises embedded bytecode information, such as struct definitions, back into runtime data
// that is usable by the parser.

#include "import_interface.h"

#include <algorithm>
#include <format>

#include "../../../defs.h"
#include "parser_context.h"

namespace {

//********************************************************************************************************************
// Convert a portable cache value kind to its parser type equivalent.

[[nodiscard]] TiriType value_kind(tiri::import_cache::ValueKind Kind)
{
   using tiri::import_cache::ValueKind;
   switch (Kind) {
      case ValueKind::NIL_VALUE: return TiriType::Nil;
      case ValueKind::BOOLEAN: return TiriType::Bool;
      case ValueKind::NUMBER: return TiriType::Num;
      case ValueKind::STRING: return TiriType::Str;
      case ValueKind::TABLE: return TiriType::Table;
      case ValueKind::ARRAY: return TiriType::Array;
      case ValueKind::FUNCTION: return TiriType::Func;
      case ValueKind::OBJECT: return TiriType::Object;
      case ValueKind::STRUCTURE: return TiriType::Struct;
      case ValueKind::THREAD:
      case ValueKind::ANY: return TiriType::Any;
      case ValueKind::UNKNOWN: return TiriType::Unknown;
   }
   return TiriType::Unknown;
}

//********************************************************************************************************************
// Convert a portable proof classification to its parser equivalent.

[[nodiscard]] StaticProof proof_kind(tiri::import_cache::ProofKind Proof)
{
   using tiri::import_cache::ProofKind;
   switch (Proof) {
      case ProofKind::ADVISORY: return StaticProof::Advisory;
      case ProofKind::CLOSED: return StaticProof::Closed;
      case ProofKind::CHECKED: return StaticProof::Checked;
      case ProofKind::TRUSTED: return StaticProof::Trusted;
   }
   return StaticProof::Advisory;
}

//********************************************************************************************************************
// Initialise the native storage metadata for a translated structure field.

void initialise_native_field(struct_field &Field, TiriType Type)
{
   switch (Type) {
      case TiriType::Bool:
         Field.Type = FD_BYTE;
         Field.NativeType = NativeStructType::Bool;
         break;
      case TiriType::Num:
         Field.Type = FD_DOUBLE;
         Field.NativeType = NativeStructType::Double;
         break;
      case TiriType::Str:
         Field.Type = FD_STRING|FD_CPP;
         Field.NativeType = NativeStructType::String;
         break;
      case TiriType::Func:
         Field.Type = FD_FUNCTION;
         Field.NativeType = NativeStructType::Function;
         break;
      case TiriType::Object:
         Field.Type = FD_OBJECT;
         Field.NativeType = NativeStructType::Object;
         break;
      case TiriType::Struct:
         Field.Type = FD_STRUCT;
         Field.NativeType = NativeStructType::Struct;
         break;
      case TiriType::Array:
         Field.Type = FD_VECTOR;
         Field.NativeType = NativeStructType::Legacy;
         Field.ArraySize = 1;
         break;
      default:
         Field.Type = 0;
         Field.NativeType = NativeStructType::Legacy;
         break;
   }
}

} // namespace

//********************************************************************************************************************
// Create and fully initialise a state-local representation of a portable import interface.

std::shared_ptr<const InstalledImportInterface> InstalledImportInterface::create(
   ParserContext &Context, tiri::import_cache::FinalisedInterfacePtr Artifact, std::string &Diagnostic)
{
   if (not Artifact) {
      Diagnostic = "Imported module has no finalised compile-time interface.";
      return {};
   }
   auto installed = std::shared_ptr<InstalledImportInterface>(
      new InstalledImportInterface(Context, std::move(Artifact)));
   if (not installed->initialise(Diagnostic)) return {};
   return installed;
}

//********************************************************************************************************************
// Validate and translate the complete portable interface into parser-owned records and indexes.

bool InstalledImportInterface::initialise(std::string &Diagnostic)
{
   using namespace tiri::import_cache;
   const Interface &interface = this->artifact_->descriptors();

   // Cached modules bypass their source-level include and module declarations.  Reinstall native definitions before
   // the parent resumes parsing so constants and structures have the same availability as a cold compilation.

   for (const NativeDependency &dependency : interface.NativeDependencies) {
      if (load_module_defs(dependency.Module) IS ERR::Okay) continue;
      Diagnostic = std::format("Imported module requires unavailable native module '{}'.", dependency.Module);
      return false;
   }

   // Allocate every definition first so transitive references can be resolved without ordering constraints.

   this->structures_.reserve(interface.Structures.size());
   this->structure_index_.reserve(interface.Structures.size());
   for (const StructureDescriptor &portable : interface.Structures) {
      auto record = std::make_unique<struct_record>(portable.Name);
      struct_record *address = record.get();
      this->structures_.push_back(std::move(record));
      this->structure_index_.emplace(portable.Name, address);
   }

   for (size_t i = 0; i < interface.Structures.size(); ++i) {
      const StructureDescriptor &portable = interface.Structures[i];
      struct_record &record = *this->structures_[i];
      for (const StructureField &portable_field : portable.Fields) {
         struct_field field;
         field.Name = portable_field.Name;
         initialise_native_field(field, value_kind(portable_field.Value.Kind));
         field.ObjectClassName  = portable_field.Value.ObjectClass;
         field.ObjectClassID    = this->object_class(portable_field.Value.ObjectClass);
         field.StructDefinition = this->structure(portable_field.Value.Structure);
         if (field.StructDefinition) field.StructRef = struct_key(field.StructDefinition->Name);
         field.precomputeNameHash();
         record.Fields.push_back(std::move(field));
      }

      if (struct_record *existing = find_struct(&this->parser_context_.lua(), portable.Name)) {
         bool compatible = existing->Fields.size() IS portable.Fields.size();
         for (const StructureField &portable_field : portable.Fields) {
            GCstr *name = this->parser_context_.lex().keepstr(portable_field.Name);
            StaticValueDescriptor actual   = describe_struct_field(existing, name);
            StaticValueDescriptor expected = this->static_value(portable_field.Value);
            compatible = compatible and actual.primary IS expected.primary;

            if (expected.primary IS TiriType::Object) {
               compatible = compatible and actual.object_class_id IS expected.object_class_id;
            }

            if (expected.primary IS TiriType::Struct) {
               compatible = compatible and actual.struct_def and expected.struct_def and
                  actual.struct_def->Name IS expected.struct_def->Name;
            }

            if (not compatible) break;
         }

         if (not compatible) {
            Diagnostic = std::format("Imported structure '{}' conflicts with an existing declaration.", portable.Name);
            return false;
         }

         this->structure_index_[portable.Name] = existing;
      }
   }

   // Resolve fields again after compatible state-local declarations have replaced private interface definitions.

   for (size_t i = 0; i < interface.Structures.size(); ++i) {
      const StructureDescriptor &portable = interface.Structures[i];
      for (size_t field_index = 0; field_index < portable.Fields.size(); ++field_index) {
         const ValueDescriptor &value = portable.Fields[field_index].Value;
         struct_field &field = this->structures_[i]->Fields[field_index];
         field.StructDefinition = this->structure(value.Structure);
         field.StructRef = field.StructDefinition ? struct_key(field.StructDefinition->Name) : 0;
      }
   }

   // Namespace records make member value descriptors available to the ordinary member-analysis path.  Callable
   // signatures are retained separately because struct fields intentionally carry no parser callable pointers.

   this->namespace_records_.reserve(interface.Namespaces.size());
   this->namespace_index_.reserve(interface.Namespaces.size());
   for (const NamespaceDescriptor &portable : interface.Namespaces) {
      if (this->namespace_index_.contains(portable.Name)) continue;
      auto record = std::make_unique<NamespaceRecord>(portable.Name);
      NamespaceRecord *address = record.get();
      this->namespace_records_.push_back(std::move(record));
      this->namespace_index_.emplace(portable.Name, address);
   }

   this->exports_.reserve(interface.Exports.size());
   for (const ExportDescriptor &exported : interface.Exports) {
      this->parser_context_.lex().imported_module_counters.interface_namespace_export_visits++;
      this->exports_.emplace(exported.Name, &exported);

      size_t separator = exported.Name.rfind('.');
      if (separator IS std::string::npos or separator IS 0 or separator + 1 >= exported.Name.size()) continue;
      std::string_view namespace_name(exported.Name.data(), separator);
      auto namespace_found = this->namespace_index_.find(namespace_name);
      if (namespace_found IS this->namespace_index_.end()) continue;

      std::string_view member_name(exported.Name.data() + separator + 1, exported.Name.size() - separator - 1);
      NamespaceRecord &record = *namespace_found->second;
      record.structure->Fields.push_back(this->export_field(exported, member_name));
      record.members.emplace(record.structure->Fields.back().Name, &exported);
   }

   for (const ExportDescriptor &exported : interface.Exports) {
      if (not exported.Callable) continue;

      auto function = std::make_unique<FunctionExprPayload>();
      function->is_vararg = exported.Callable->VariadicParameters;
      std::vector<const ContractDescriptor *> parameters;
      for (const ContractDescriptor &parameter : exported.Callable->Parameters) parameters.push_back(&parameter);
      std::ranges::sort(parameters, {}, &ContractDescriptor::Position);

      for (size_t parameter_index = 0; parameter_index < parameters.size(); ++parameter_index) {
         const ContractDescriptor &parameter = *parameters[parameter_index];
         if (parameter.Position != parameter_index) {
            Diagnostic = std::format("Imported callable '{}' has invalid parameter positions.", exported.Name);
            return false;
         }

         FunctionParameter translated;
         std::string label = parameter.Label.empty() ? std::format("import_arg_{}", parameter_index) : parameter.Label;
         translated.name             = Identifier::from_keepstr(this->parser_context_.lex().keepstr(label));
         translated.name.has_const   = parameter.IsConst;
         translated.type             = value_kind(parameter.Value.Kind);
         translated.struct_def       = this->structure(parameter.Value.Structure);
         translated.array_element    = this->array_element(parameter.Value.Array);
         translated.type_is_explicit = true;
         translated.required         = parameter.Required;
         function->parameters.push_back(std::move(translated));
      }

      FunctionReturnTypes &returns = function->return_types;
      if (exported.Callable->DeclaredResults > MAX_RETURN_TYPES or
          exported.Callable->DeclaredResults != exported.Callable->Results.size()) {
         Diagnostic = std::format("Imported callable '{}' has an invalid result count.", exported.Name);
         return false;
      }

      returns.count = uint8_t(std::min<size_t>(exported.Callable->Results.size(), MAX_RETURN_TYPES));
      returns.is_variadic = exported.Callable->VariadicResults;
      returns.is_explicit = true;
      for (size_t i = 0; i < returns.count; ++i) {
         const ValueDescriptor &result = exported.Callable->Results[i];
         returns.types[i]            = value_kind(result.Kind);
         returns.object_class_ids[i] = this->object_class(result.ObjectClass);
         returns.struct_defs[i]      = this->structure(result.Structure);
         returns.array_elements[i]   = this->array_element(result.Array);
         returns.required[i]         = not result.Nullable;
      }
      this->callables_.emplace(&exported, std::move(function));
   }
   return true;
}

//********************************************************************************************************************
// Find an exported binding by its fully qualified name.

const tiri::import_cache::ExportDescriptor * InstalledImportInterface::find_export(std::string_view Name) const
{
   auto found = this->exports_.find(Name);
   return found IS this->exports_.end() ? nullptr : found->second;
}

//********************************************************************************************************************
// Find an exported member through its installed namespace index.

const tiri::import_cache::ExportDescriptor * InstalledImportInterface::find_member(
   std::string_view Namespace, std::string_view Member) const
{
   auto namespace_found = this->namespace_index_.find(Namespace);
   if (namespace_found IS this->namespace_index_.end()) return nullptr;
   auto member_found = namespace_found->second->members.find(Member);
   return member_found IS namespace_found->second->members.end() ? nullptr : member_found->second;
}

//********************************************************************************************************************
// Return the parser-owned callable signature associated with an exported binding.

const FunctionExprPayload * InstalledImportInterface::callable(
   const tiri::import_cache::ExportDescriptor &Export) const
{
   auto found = this->callables_.find(&Export);
   return found IS this->callables_.end() ? nullptr : found->second.get();
}

//********************************************************************************************************************
// Resolve an imported structure name to its state-local parser record.

struct_record * InstalledImportInterface::structure(std::string_view Name) const
{
   if (Name.empty()) return nullptr;
   auto found = this->structure_index_.find(Name);
   return found IS this->structure_index_.end() ? nullptr : found->second;
}

//********************************************************************************************************************
// Resolve a portable object class name to its runtime class identifier.

CLASSID InstalledImportInterface::object_class(std::string_view Name) const
{
   return Name.empty() ? CLASSID::NIL : ResolveClassName(Name);
}

//********************************************************************************************************************
// Translate an exported value into a namespace structure field.

struct_field InstalledImportInterface::export_field(
   const tiri::import_cache::ExportDescriptor &Export, std::string_view Name) const
{
   struct_field field;
   field.Name.assign(Name);
   initialise_native_field(field, value_kind(Export.Value.Kind));
   field.ObjectClassName  = Export.Value.ObjectClass;
   field.ObjectClassID    = this->object_class(Export.Value.ObjectClass);
   field.StructDefinition = this->structure(Export.Value.Structure);
   if (field.StructDefinition) field.StructRef = struct_key(field.StructDefinition->Name);
   field.precomputeNameHash();
   return field;
}

//********************************************************************************************************************
// Translate portable array element metadata into its parser descriptor.

ArrayElementDescriptor InstalledImportInterface::array_element(
   const tiri::import_cache::ArrayDescriptor &Array) const
{
   ArrayElementDescriptor result;
   result.storage         = Array.Storage < uint8_t(AET::MAX) ? AET(Array.Storage) : AET::ANY;
   result.logical_type    = value_kind(Array.ElementKind);
   result.object_class_id = this->object_class(Array.ObjectClass);
   result.struct_def      = this->structure(Array.Structure);
   result.known           = Array.ElementKind != tiri::import_cache::ValueKind::ANY or Array.Storage != 0 or
      not Array.ObjectClass.empty() or not Array.Structure.empty() or not Array.NestedIdentity.empty();

   if (not Array.NestedIdentity.empty()) {
      result.nested_array_identity = this->parser_context_.lex().keepstr(Array.NestedIdentity);
   }
   return result;
}

//********************************************************************************************************************
// Translate a portable value descriptor for static descriptor analysis.

StaticValueDescriptor InstalledImportInterface::static_value(const tiri::import_cache::ValueDescriptor &Value) const
{
   StaticValueDescriptor result;
   result.primary         = value_kind(Value.Kind);
   result.proof           = proof_kind(Value.Proof);
   result.object_class_id = this->object_class(Value.ObjectClass);
   result.struct_def      = this->structure(Value.Structure);
   result.array_element   = this->array_element(Value.Array);
   result.nullable        = Value.Nullable;
   return result;
}

//********************************************************************************************************************
// Translate a portable value descriptor for inferred type analysis.

InferredType InstalledImportInterface::inferred_type(const tiri::import_cache::ValueDescriptor &Value) const
{
   InferredType result(value_kind(Value.Kind));
   result.is_constant     = Value.Proof != tiri::import_cache::ProofKind::ADVISORY;
   result.is_nullable     = Value.Nullable;
   result.is_fixed        = Value.Kind != tiri::import_cache::ValueKind::UNKNOWN;
   result.object_class_id = this->object_class(Value.ObjectClass);
   result.struct_def      = this->structure(Value.Structure);
   result.array_element   = this->array_element(Value.Array);
   return result;
}

//********************************************************************************************************************
// Describe a namespace binding, preferring an exact exported value when one exists.

StaticValueDescriptor InstalledImportInterface::namespace_value(std::string_view Namespace) const
{
   if (const auto *exact = this->find_export(Namespace)) return this->static_value(exact->Value);
   StaticValueDescriptor result;
   result.primary  = TiriType::Table;
   result.proof    = StaticProof::Closed;
   result.nullable = false;
   auto found = this->namespace_index_.find(Namespace);
   if (found != this->namespace_index_.end()) result.struct_def = found->second->structure.get();
   return result;
}

//********************************************************************************************************************
// Infer a namespace binding type, preferring an exact exported value when one exists.

InferredType InstalledImportInterface::namespace_type(std::string_view Namespace) const
{
   if (const auto *exact = this->find_export(Namespace)) return this->inferred_type(exact->Value);
   InferredType result(TiriType::Table, true, false, true);
   auto found = this->namespace_index_.find(Namespace);
   if (found != this->namespace_index_.end()) result.struct_def = found->second->structure.get();
   return result;
}
