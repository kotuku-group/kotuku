// Portable imported-module interface extraction.

#include "import_interface_export.h"

#include "ast/nodes.h"
#include "import_interface.h"
#include "parser_context.h"
#include "static_type_descriptor.h"

#include <algorithm>
#include <bit>
#include <format>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

//********************************************************************************************************************
// Converts an interned Tiri symbol to its portable string representation.

std::string symbol_name(GCstr *Symbol)
{
   return Symbol ? std::string(strdata(Symbol), Symbol->len) : std::string();
}

//********************************************************************************************************************
// Returns a non-owning view of an interned Tiri symbol.

std::string_view symbol_view(GCstr *Symbol)
{
   return Symbol ? std::string_view(strdata(Symbol), Symbol->len) : std::string_view();
}

//********************************************************************************************************************
// Maintains owned name indexes while a portable interface is assembled.  The indexes are deliberately parser-local;
// only the descriptor vectors are transferred to the final immutable artefact.

struct AssemblyStringHash {
   using is_transparent = void;

   [[nodiscard]] size_t operator()(std::string_view Value) const noexcept {
      return std::hash<std::string_view>{}(Value);
   }
};

using AssemblyIndex = std::unordered_map<std::string, size_t, AssemblyStringHash, std::equal_to<>>;

class InterfaceAssembly {
public:
   InterfaceAssembly(tiri::import_cache::Interface &InterfaceValue, ImportedModuleCompilationCounters &Counters) :
      interface_(InterfaceValue), counters_(Counters)
   {
      for (size_t i = 0; i < this->interface_.Namespaces.size(); ++i) {
         this->namespace_index_.try_emplace(this->interface_.Namespaces[i].Name, i);
      }
      for (size_t i = 0; i < this->interface_.Exports.size(); ++i) {
         this->export_index_.try_emplace(this->interface_.Exports[i].Name, i);
      }
      for (size_t i = 0; i < this->interface_.Structures.size(); ++i) {
         this->structure_index_.try_emplace(this->interface_.Structures[i].Name, i);
      }
      for (size_t i = 0; i < this->interface_.Enums.size(); ++i) {
         this->enum_index_.try_emplace(this->interface_.Enums[i].Name, i);
         this->enum_member_indexes_.emplace_back();
         auto &member_index = this->enum_member_indexes_.back();
         member_index.reserve(this->interface_.Enums[i].Members.size());
         for (size_t member = 0; member < this->interface_.Enums[i].Members.size(); ++member) {
            member_index.try_emplace(this->interface_.Enums[i].Members[member].Name, member);
         }
      }
   }

   [[nodiscard]] std::pair<size_t, bool> distinct_export(std::string Name)
   {
      this->counters_.interface_assembly_probes++;
      auto found = this->export_index_.find(Name);
      if (found != this->export_index_.end()) return { found->second, false };

      size_t position = this->interface_.Exports.size();
      this->interface_.Exports.push_back({});
      this->interface_.Exports.back().Name = std::move(Name);
      this->export_index_.emplace(this->interface_.Exports.back().Name, position);
      return { position, true };
   }

   [[nodiscard]] tiri::import_cache::ExportDescriptor * find_export(std::string_view Name)
   {
      this->counters_.interface_assembly_probes++;
      auto found = this->export_index_.find(Name);
      return found IS this->export_index_.end() ? nullptr : &this->interface_.Exports[found->second];
   }

   void append_namespace(tiri::import_cache::NamespaceDescriptor Namespace)
   {
      size_t position = this->interface_.Namespaces.size();
      this->interface_.Namespaces.push_back(std::move(Namespace));
      this->counters_.interface_assembly_probes++;
      this->namespace_index_.try_emplace(this->interface_.Namespaces.back().Name, position);
   }

   void merge(const tiri::import_cache::Interface &Source)
   {
      this->namespace_index_.reserve(this->interface_.Namespaces.size() + Source.Namespaces.size());
      this->export_index_.reserve(this->interface_.Exports.size() + Source.Exports.size());
      this->structure_index_.reserve(this->interface_.Structures.size() + Source.Structures.size());
      this->enum_index_.reserve(this->interface_.Enums.size() + Source.Enums.size());

      for (const auto &item : Source.Namespaces) {
         this->counters_.interface_assembly_probes++;
         if (this->namespace_index_.contains(item.Name)) continue;
         size_t position = this->interface_.Namespaces.size();
         this->interface_.Namespaces.push_back(item);
         this->namespace_index_.emplace(this->interface_.Namespaces.back().Name, position);
      }

      for (const auto &item : Source.Exports) {
         this->counters_.interface_assembly_probes++;
         if (this->export_index_.contains(item.Name)) continue;
         size_t position = this->interface_.Exports.size();
         this->interface_.Exports.push_back(item);
         this->export_index_.emplace(this->interface_.Exports.back().Name, position);
      }

      for (const auto &item : Source.Structures) {
         this->counters_.interface_assembly_probes++;
         if (this->structure_index_.contains(item.Name)) continue;
         size_t position = this->interface_.Structures.size();
         this->interface_.Structures.push_back(item);
         this->structure_index_.emplace(this->interface_.Structures.back().Name, position);
      }

      for (const auto &item : Source.Enums) {
         this->counters_.interface_assembly_probes++;
         auto found = this->enum_index_.find(item.Name);
         if (found != this->enum_index_.end()) {
            auto &members = this->interface_.Enums[found->second].Members;
            auto &member_index = this->enum_member_indexes_[found->second];
            member_index.reserve(member_index.size() + item.Members.size());
            for (const auto &member : item.Members) {
               if (member_index.contains(member.Name)) continue;
               member_index.emplace(member.Name, members.size());
               members.push_back(member);
            }
            continue;
         }
         size_t position = this->interface_.Enums.size();
         this->interface_.Enums.push_back(item);
         this->enum_index_.emplace(this->interface_.Enums.back().Name, position);
         this->enum_member_indexes_.emplace_back();
         auto &member_index = this->enum_member_indexes_.back();
         member_index.reserve(item.Members.size());
         for (size_t member = 0; member < item.Members.size(); ++member) {
            member_index.try_emplace(item.Members[member].Name, member);
         }
      }
   }

   [[nodiscard]] tiri::import_cache::Interface & descriptors() noexcept { return this->interface_; }

private:
   tiri::import_cache::Interface &interface_;
   ImportedModuleCompilationCounters &counters_;
   AssemblyIndex namespace_index_;
   AssemblyIndex export_index_;
   AssemblyIndex structure_index_;
   AssemblyIndex enum_index_;
   std::vector<AssemblyIndex> enum_member_indexes_;
};

//********************************************************************************************************************
// Maps a parser value type to the equivalent import-cache value kind.

tiri::import_cache::ValueKind portable_kind(TiriType Type)
{
   using tiri::import_cache::ValueKind;
   switch (Type) {
      case TiriType::Nil:    return ValueKind::NIL_VALUE;
      case TiriType::Bool:   return ValueKind::BOOLEAN;
      case TiriType::Num:    return ValueKind::NUMBER;
      case TiriType::Str:    return ValueKind::STRING;
      case TiriType::Table:  return ValueKind::TABLE;
      case TiriType::Array:  return ValueKind::ARRAY;
      case TiriType::Func:   return ValueKind::FUNCTION;
      case TiriType::Object: return ValueKind::OBJECT;
      case TiriType::Struct: return ValueKind::STRUCTURE;
      case TiriType::Range:
      case TiriType::Userdata: return ValueKind::ANY;
      case TiriType::Any: return ValueKind::ANY;
      default: return ValueKind::UNKNOWN;
   }
}

//********************************************************************************************************************
// Maps a parser static-proof level to the portable import-cache representation.

tiri::import_cache::ProofKind portable_proof(StaticProof Proof)
{
   using tiri::import_cache::ProofKind;
   switch (Proof) {
      case StaticProof::Closed: return ProofKind::CLOSED;
      case StaticProof::Checked: return ProofKind::CHECKED;
      case StaticProof::Trusted: return ProofKind::TRUSTED;
      default: return ProofKind::ADVISORY;
   }
}

//********************************************************************************************************************
// Copies static type information into a value descriptor that can be cached independently of the parser.

tiri::import_cache::ValueDescriptor portable_value(const StaticValueDescriptor &Value)
{
   tiri::import_cache::ValueDescriptor result;
   result.Kind = portable_kind(Value.primary);
   result.Proof = portable_proof(Value.proof);
   result.Nullable = Value.nullable;
   if (Value.object_class_id != CLASSID::NIL) {
      if (auto name = ResolveClassID(Value.object_class_id)) result.ObjectClass = name;
   }

   if (Value.struct_def) result.Structure = Value.struct_def->Name;

   result.Array.Storage = uint8_t(Value.array_element.storage);
   result.Array.ElementKind = portable_kind(Value.array_element.logical_type);

   if (Value.array_element.object_class_id != CLASSID::NIL) {
      if (auto name = ResolveClassID(Value.array_element.object_class_id)) result.Array.ObjectClass = name;
   }

   if (Value.array_element.struct_def) result.Array.Structure = Value.array_element.struct_def->Name;

   if (Value.array_element.nested_array_identity) {
      result.Array.NestedIdentity = symbol_name(Value.array_element.nested_array_identity);
   }
   return result;
}

//********************************************************************************************************************
// Copies a named structure and its transitive field types into the portable compile-time interface.

void append_portable_structure(ParserContext &Context, const struct_record &Structure,
   tiri::import_cache::Interface &Output, std::unordered_set<std::string> &Visited)
{
   if (not Visited.insert(Structure.Name).second) return;

   for (const struct_field &field : Structure.Fields) {
      if (field.StructDefinition) append_portable_structure(Context, *field.StructDefinition, Output, Visited);
   }

   tiri::import_cache::StructureDescriptor descriptor;
   descriptor.Name = Structure.Name;
   for (const struct_field &field : Structure.Fields) {
      GCstr *name = Context.lex().keepstr(field.Name);
      StaticValueDescriptor value = describe_struct_field(&Structure, name);
      descriptor.Fields.push_back({ field.Name, portable_value(value), not value.nullable });
   }
   Output.Structures.push_back(std::move(descriptor));
}

//********************************************************************************************************************
// Reports whether a declaration source belongs to a module root or one of its inline local imports.

bool module_owns_source(const tiri::import_cache::Identity &Identity, std::string_view Source)
{
   if (Identity.Source.ResolvedPath IS Source) return true;
   for (const auto &local : Identity.LocalImports) {
      if (local.Source.ResolvedPath IS Source) return true;
   }
   return false;
}

//********************************************************************************************************************
// Returns the best available portable descriptor for an expression, inferring its type when no static value exists.

tiri::import_cache::ValueDescriptor expression_value(ParserContext &Context, const ExprNode *Expression)
{
   if (Expression and Expression->static_value) {
      return portable_value(Context.descriptors().value(Expression->static_value));
   }
   StaticValueDescriptor fallback;
   if (Expression) fallback.primary = infer_expression_type(*Expression);
   return portable_value(fallback);
}

//********************************************************************************************************************
// Creates a checked portable descriptor from an explicit source annotation.

tiri::import_cache::ValueDescriptor annotated_value(
   TiriType Type, struct_record *Structure, const ArrayElementDescriptor &Array, bool Nullable)
{
   StaticValueDescriptor value;
   value.primary       = Type;
   value.struct_def    = Structure;
   value.array_element = Array;
   value.proof         = StaticProof::Checked;
   value.nullable      = Nullable;
   return portable_value(value);
}

//********************************************************************************************************************
// Serialises a function signature into the portable representation used by import interfaces.

tiri::import_cache::CallableDescriptor portable_callable(const FunctionExprPayload &Function)
{
   tiri::import_cache::CallableDescriptor result;
   result.VariadicParameters = Function.is_vararg;
   result.VariadicResults    = Function.return_types.is_variadic;
   result.DeclaredResults    = Function.return_types.count;

   for (size_t i = 0; i < Function.parameters.size(); ++i) {
      const auto &parameter = Function.parameters[i];
      result.Parameters.push_back({
         annotated_value(parameter.type, parameter.struct_def, parameter.array_element, not parameter.required),
         symbol_name(parameter.name.symbol), uint8_t(i), parameter.required, parameter.name.has_const
      });
   }

   for (size_t i = 0; i < Function.return_types.count; ++i) {
      StaticValueDescriptor value;
      value.primary         = Function.return_types.types[i];
      value.object_class_id = Function.return_types.object_class_ids[i];
      value.struct_def      = Function.return_types.struct_defs[i];
      value.array_element   = Function.return_types.array_elements[i];
      value.proof           = Function.return_types.is_explicit ? StaticProof::Checked : StaticProof::Trusted;
      value.nullable        = not Function.return_types.required[i];
      result.Results.push_back(portable_value(value));
   }

   return result;
}

//********************************************************************************************************************

// Locates a function payload represented directly by an expression or indirectly through an identifier binding.
const FunctionExprPayload * function_expression(const ExprNode *Expression, ParserContext &Context)
{
   if (not Expression) return nullptr;
   if (Expression->kind IS AstNodeKind::FunctionExpr) return &std::get<FunctionExprPayload>(Expression->data);
   if (Expression->kind != AstNodeKind::IdentifierExpr) return nullptr;
   StaticBindingID binding_id = std::get<NameRef>(Expression->data).binding_id;
   if (not binding_id or binding_id.raw() >= Context.descriptors().binding_count()) return nullptr;
   return Context.descriptors().binding(binding_id).function;
}

//********************************************************************************************************************
// Returns the dotted name represented by an identifier or member-access expression.

std::string qualified_name(const ExprNode *Expression)
{
   if (not Expression) return {};

   if (Expression->kind IS AstNodeKind::IdentifierExpr) {
      return symbol_name(std::get<NameRef>(Expression->data).identifier.symbol);
   }

   if (Expression->kind IS AstNodeKind::MemberExpr) {
      const auto &member = std::get<MemberExprPayload>(Expression->data);
      std::string result = qualified_name(member.table.get());
      if (result.empty()) return {};
      result.push_back('.');
      result += symbol_name(member.member.symbol);
      return result;
   }
   return {};
}

//********************************************************************************************************************
// Records a literal expression's value when it can be represented as an exported constant.

void constant_value(const ExprNode *Expression, tiri::import_cache::ConstantValue &Output)
{
   using namespace tiri::import_cache;
   if (not Expression or Expression->kind != AstNodeKind::LiteralExpr) return;
   const auto &literal = std::get<LiteralValue>(Expression->data);
   switch (literal.kind) {
      case LiteralKind::Nil: Output.Kind = ConstantKind::NIL_VALUE; break;
      case LiteralKind::Boolean:
         Output.Kind = ConstantKind::BOOLEAN;
         Output.Boolean = literal.bool_value;
         break;
      case LiteralKind::Number:
         Output.Kind = ConstantKind::NUMBER;
         Output.NumberBits = std::bit_cast<uint64_t>(double(literal.number_value));
         break;
      case LiteralKind::String:
         Output.Kind = ConstantKind::STRING;
         Output.String = symbol_name(literal.string_value);
         break;
   }
}

//********************************************************************************************************************
// Adds a distinct export and records its value, callable signature, and constant metadata.

void add_export(InterfaceAssembly &Assembly, std::string Name,
   tiri::import_cache::ExportKind Kind, const ExprNode *Initialiser, const FunctionExprPayload *Function,
   bool IsConst, ParserContext &Context)
{
   if (Name.empty()) return;

   auto [position, inserted] = Assembly.distinct_export(std::move(Name));
   if (not inserted) return;

   tiri::import_cache::ExportDescriptor &exported = Assembly.descriptors().Exports[position];
   exported.Kind = Kind;
   exported.Value = expression_value(Context, Initialiser);

   if (not Function and exported.Value.Kind IS tiri::import_cache::ValueKind::FUNCTION) {
      exported.Value.Kind = tiri::import_cache::ValueKind::ANY;
      exported.Value.Proof = tiri::import_cache::ProofKind::ADVISORY;
      exported.Value.Nullable = true;
   }

   if (Function) {
      exported.Value.Kind = tiri::import_cache::ValueKind::FUNCTION;
      exported.Value.Proof = tiri::import_cache::ProofKind::CLOSED;
      exported.Value.Nullable = false;
      exported.Callable = portable_callable(*Function);
   }

   if (IsConst or Kind IS tiri::import_cache::ExportKind::ENUM_CONSTANT) {
      constant_value(Initialiser, exported.Constant);
   }

   exported.IsConst = IsConst;
}

//********************************************************************************************************************
// Refines an existing export with the type information declared for its source identifier.

void apply_declared_value(InterfaceAssembly &Assembly, const Identifier &Name,
   ParserContext &Context)
{
   auto *found = Assembly.find_export(symbol_view(Name.symbol));
   if (not found) return;
   if (Name.static_value) found->Value = portable_value(Context.descriptors().value(Name.static_value));
   TiriType type = Name.global_contract_type != TiriType::Unknown ? Name.global_contract_type : Name.type;

   if (type != TiriType::Unknown and type != TiriType::Any) {
      struct_record *structure = Name.global_contract_struct_def ? Name.global_contract_struct_def : Name.struct_def;
      const auto &array = Name.global_contract_type != TiriType::Unknown ?
         Name.global_contract_array_element : Name.array_element;
      found->Value = annotated_value(type, structure, array, false);
   }
}

//********************************************************************************************************************
// Adds the source interface's unique declarations to the target interface.

void merge_interface(InterfaceAssembly &Target, const tiri::import_cache::Interface &Source)
{
   Target.merge(Source);
}

//********************************************************************************************************************
// Builds and installs portable interfaces for imported modules contained in this block and its descendants.

bool prepare_block(ParserContext &Context, BlockStmt &Block, std::string &Diagnostic)
{
   using namespace tiri::import_cache;
   for (auto &statement : Block.statements) {
      if (not statement or statement->kind != AstNodeKind::ImportStmt) continue;
      auto &import = std::get<ImportStmtPayload>(statement->data);
      for (auto &entry : import.entries) {
         BlockStmt *body = entry.module_unit ? entry.module_unit->body.get() : entry.inlined_body.get();
         if (body and (not entry.module_unit or not entry.module_unit->interface_prepared) and
             not prepare_block(Context, *body, Diagnostic)) return false;
         if (not entry.module_initialiser) continue;
         ImportedModuleUnit &unit = *entry.module_unit;
         if (unit.interface_prepared) continue;
         if (unit.module_cache_hit) {
            unit.interface_prepared = true;
            unit.state = ImportedModuleState::InterfaceReady;
            Context.lex().imported_module_counters.interface_preparations++;
            continue;
         }

         Interface portable;
         portable.Package = unit.declared_package;
         std::vector<tiri::CompatibilityRecord> compatibility_records {
            { entry.lib_path, unit.dependency_requirements.value_or(tiri::DependencyRequirements {}) }
         };
         unit.module_cache_identity.DeclaredPackage = unit.declared_package;
         InterfaceAssembly assembly(portable, Context.lex().imported_module_counters);
         SourceDescriptor source;
         source.ResolvedPath = entry.lib_path;
         source.LogicalRequest = unit.module_cache_identity.LogicalRequest;
         auto separator = entry.lib_path.find_last_of("/\\:");
         source.Filename = separator IS std::string::npos ? entry.lib_path : entry.lib_path.substr(separator + 1);
         if (const FileSource *file = get_file_source(&Context.lua(), unit.file_source_idx)) {
            source.DeclaredNamespace = file->declared_namespace;
            source.TotalLines = file->total_lines.lineNumber();
         }
         portable.Sources.push_back(std::move(source));

         uint32_t activation_order = 0;
         for (const auto &dependency : unit.module_dependencies) {
            NativeDependency native;
            native.Module = symbol_name(dependency.name);
            native.ActivationOrder = activation_order++;
            for (GCstr *function : dependency.functions) native.Functions.push_back(symbol_name(function));
            portable.NativeDependencies.push_back(std::move(native));
         }

         for (auto &child_statement : body->statements) {
            if (not child_statement) continue;
            switch (child_statement->kind) {
               case AstNodeKind::ImportStmt:
                  for (const auto &nested : std::get<ImportStmtPayload>(child_statement->data).entries) {
                     if (nested.module_unit and nested.module_unit->installed_interface) {
                        const auto &nested_artifact = nested.module_unit->installed_interface->artifact();
                        const auto &nested_interface = nested_artifact.descriptors();
                        const auto &nested_digest = nested_artifact.digest();
                        std::vector<tiri::CompatibilityRecord> nested_compatibility;
                        if (tiri::decode_compatibility_manifest(
                               nested_interface.CompatibilityManifest, nested_compatibility)) {
                           Diagnostic = std::format("{}: nested module has invalid compatibility metadata",
                              nested.lib_path);
                           return false;
                        }
                        compatibility_records.insert(compatibility_records.end(),
                           nested_compatibility.begin(), nested_compatibility.end());
                        merge_interface(assembly, nested_interface);
                        portable.NestedModules.push_back({ nested.module_unit->module_cache_identity.LogicalRequest,
                           nested.lib_path, nested_digest });
                        unit.module_cache_identity.ModuleDependencies.push_back({
                           nested.module_unit->module_cache_identity.LogicalRequest, nested.lib_path, nested_digest,
                           nested.module_unit->module_identity
                        });
                     }
                  }
                  break;
               case AstNodeKind::NamespaceStmt: {
                  const auto &item = std::get<NamespaceStmtPayload>(child_statement->data);
                  std::string name = symbol_name(item.name.symbol);
                  assembly.append_namespace({ name, item.mode IS NamespaceDeclarationMode::Create ?
                     NamespaceMode::DECLARE : NamespaceMode::JOIN });
                  add_export(assembly, name, ExportKind::GLOBAL, item.initialiser.get(),
                     function_expression(item.initialiser.get(), Context), true, Context);
                  break;
               }
               case AstNodeKind::GlobalDeclStmt: {
                  const auto &item = std::get<GlobalDeclStmtPayload>(child_statement->data);
                  for (size_t i = 0; i < item.names.size(); ++i) {
                     const ExprNode *value = i < item.values.size() ? item.values[i].get() :
                        (item.values.empty() ? nullptr : item.values.back().get());
                     add_export(assembly, symbol_name(item.names[i].symbol), ExportKind::GLOBAL, value,
                        function_expression(value, Context), item.names[i].has_const, Context);
                     apply_declared_value(assembly, item.names[i], Context);
                  }
                  break;
               }
               case AstNodeKind::ExternStmt:
                  for (const auto &name : std::get<ExternDeclStmtPayload>(child_statement->data).names) {
                     add_export(assembly, symbol_name(name.symbol), ExportKind::EXTERN, nullptr, nullptr,
                        name.has_const, Context);
                  }
                  break;
               case AstNodeKind::FunctionStmt: {
                  const auto &item = std::get<FunctionStmtPayload>(child_statement->data);
                  if (item.name.is_explicit_global or item.name.segments.size() > 1) {
                     std::string name;
                     for (const auto &segment : item.name.segments) {
                        if (not name.empty()) name.push_back('.');
                        name += symbol_name(segment.symbol);
                     }
                     add_export(assembly, std::move(name), ExportKind::GLOBAL, nullptr,
                        item.function.get(), true, Context);
                  }
                  break;
               }
               case AstNodeKind::AssignmentStmt: {
                  const auto &item = std::get<AssignmentStmtPayload>(child_statement->data);
                  for (size_t i = 0; i < item.targets.size(); ++i) {
                     std::string name = qualified_name(item.targets[i].get());
                     if (name.find('.') IS std::string::npos) continue;
                     const ExprNode *value = i < item.values.size() ? item.values[i].get() :
                        (item.values.empty() ? nullptr : item.values.back().get());
                     add_export(assembly, std::move(name), ExportKind::GLOBAL, value,
                        function_expression(value, Context), false, Context);
                  }
                  break;
               }
               default: break;
            }
         }

         Interface declarations;
         std::unordered_set<std::string> declared_structures;
         for (const auto &[key, structure] : Context.lua().struct_declarations) {
            (void)key;
            if (module_owns_source(unit.module_cache_identity, structure.DeclarationSource)) {
               append_portable_structure(Context, structure, declarations, declared_structures);
            }
         }

         for (const ImportedEnumDeclaration &item : Context.lex().imported_enum_declarations) {
            if (module_owns_source(unit.module_cache_identity, item.source)) {
               declarations.Enums.push_back(item.descriptor);
            }
         }
         merge_interface(assembly, declarations);

         std::vector<std::string> structure_roots;
         for (const StructureDescriptor &structure : portable.Structures) {
            auto found = Context.lua().struct_declarations.find(struct_key(structure.Name));
            if (found != Context.lua().struct_declarations.end() and found->second.Name IS structure.Name) {
               structure_roots.push_back(structure.Name);
            }
         }

         std::vector<uint8_t> structure_manifest;
         std::string structure_diagnostic;
         ERR structure_error = build_declared_struct_manifest(&Context.lua(), structure_roots, structure_roots,
            false, structure_manifest, &structure_diagnostic);
         if (structure_error != ERR::Okay) {
            Diagnostic = std::format("{}: cannot preserve imported structures: {}", entry.lib_path,
               structure_diagnostic.empty() ? GetErrorMsg(structure_error) : structure_diagnostic);
            return false;
         }

         portable.StructureManifest.assign((const char *)structure_manifest.data(), structure_manifest.size());
         if (tiri::encode_compatibility_manifest(compatibility_records, portable.CompatibilityManifest)) {
            Diagnostic = std::format("{}: cannot encode compatibility metadata", entry.lib_path);
            return false;
         }

         InterfaceOperationCounters interface_counters;
         FinalisedInterfacePtr artifact;
         auto interface_error = FinalisedInterface::finalise(std::move(portable), artifact, &interface_counters);
         if (interface_error != cache::FormatError::OKAY) {
            Diagnostic = std::format("{}: invalid compile-time interface: {}", entry.lib_path,
               cache::format_error_name(interface_error));
            return false;
         }

         if (finalise_identity(unit.module_cache_identity) != cache::FormatError::OKAY) {
            Diagnostic = std::format("{}: compiled module identity is invalid", entry.lib_path);
            return false;
         }

         auto installed = InstalledImportInterface::create(Context, artifact, Diagnostic);
         if (not installed) {
            Diagnostic = std::format("{}: {}", entry.lib_path, Diagnostic);
            return false;
         }
         unit.interface_artifact  = std::move(artifact);
         unit.installed_interface = std::move(installed);
         unit.module_identity     = unit.module_cache_identity.CompiledIdentity;
         unit.interface_prepared  = true;
         unit.state = ImportedModuleState::InterfaceReady;

         Context.lex().imported_module_counters.interface_preparations++;
         Context.lex().imported_module_counters.interface_finalisations += interface_counters.ColdFinalisations;
         Context.lex().imported_module_counters.interface_encodes += interface_counters.Encodes;
      }
   }
   return true;
}

} // namespace

//********************************************************************************************************************
// Prepares portable import interfaces for every imported module in the parsed block.

bool prepare_import_interfaces(ParserContext &Context, BlockStmt &Block, std::string &Diagnostic)
{
   return prepare_block(Context, Block, Diagnostic);
}
