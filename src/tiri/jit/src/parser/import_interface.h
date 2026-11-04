// State-local translation of a portable imported-module interface into parser descriptors.

// The portable cache format contains no GC pointers or parser-owned indexes.  This object is attached to the import
// AST entry and owns every translated structure and callable for the complete parser pipeline lifetime.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/nodes.h"
#include "type_checker.h"
#include "../../../packaging/import_module_format.h"

class ParserContext;

class InstalledImportInterface {
public:
   [[nodiscard]] static std::shared_ptr<const InstalledImportInterface> create(
      ParserContext &, tiri::import_cache::FinalisedInterfacePtr, std::string &Diagnostic);

   [[nodiscard]] const std::vector<tiri::import_cache::ExportDescriptor> & bindings() const noexcept {
      return this->portable_interface().Exports;
   }
   [[nodiscard]] const tiri::import_cache::ExportDescriptor * find_export(std::string_view Name) const;
   [[nodiscard]] const tiri::import_cache::ExportDescriptor * find_member(
      std::string_view Namespace, std::string_view Member) const;
   [[nodiscard]] bool is_namespace(std::string_view Name) const {
      return this->namespace_index_.contains(Name);
   }
   [[nodiscard]] const FunctionExprPayload * callable(const tiri::import_cache::ExportDescriptor &) const;
   [[nodiscard]] StaticValueDescriptor static_value(const tiri::import_cache::ValueDescriptor &) const;
   [[nodiscard]] InferredType inferred_type(const tiri::import_cache::ValueDescriptor &) const;
   [[nodiscard]] StaticValueDescriptor namespace_value(std::string_view Namespace) const;
   [[nodiscard]] InferredType namespace_type(std::string_view Namespace) const;
   [[nodiscard]] const tiri::import_cache::Interface & portable_interface() const noexcept {
      return this->artifact_->descriptors();
   }
   [[nodiscard]] const tiri::import_cache::FinalisedInterface & artifact() const noexcept { return *this->artifact_; }
   [[nodiscard]] const std::vector<uint32_t> & inserted_structures() const noexcept {
      return this->inserted_structures_;
   }
   [[nodiscard]] const std::vector<uint32_t> & inserted_enum_constants() const noexcept {
      return this->inserted_enum_constants_;
   }

private:
   struct TransparentStringHash {
      using is_transparent = void;

      [[nodiscard]] size_t operator()(std::string_view Value) const noexcept {
         return std::hash<std::string_view>{}(Value);
      }
   };

   template<typename Value>
   using StringIndex = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

   struct NamespaceRecord {
      explicit NamespaceRecord(std::string Name) : structure(std::make_unique<struct_record>(std::move(Name))) { }

      std::unique_ptr<struct_record> structure;
      StringIndex<const tiri::import_cache::ExportDescriptor *> members;
   };

   InstalledImportInterface(ParserContext &Context, tiri::import_cache::FinalisedInterfacePtr Artifact) :
      parser_context_(Context), artifact_(std::move(Artifact)) { }

   [[nodiscard]] bool initialise(std::string &Diagnostic);
   [[nodiscard]] bool structure_compatible(
      const tiri::import_cache::StructureDescriptor &, const struct_record &) const;
   [[nodiscard]] struct_record * structure(std::string_view Name) const;
   [[nodiscard]] ArrayElementDescriptor array_element(const tiri::import_cache::ArrayDescriptor &) const;
   [[nodiscard]] CLASSID object_class(std::string_view Name) const;
   [[nodiscard]] struct_field export_field(
      const tiri::import_cache::ExportDescriptor &Export, std::string_view Name) const;

   ParserContext &parser_context_;
   // Declared before every descriptor-address index so it is released after those borrowers are destroyed.
   tiri::import_cache::FinalisedInterfacePtr artifact_;
   std::vector<std::unique_ptr<struct_record>> structures_;
   std::vector<std::unique_ptr<NamespaceRecord>> namespace_records_;
   StringIndex<const tiri::import_cache::ExportDescriptor *> exports_;
   std::unordered_map<const tiri::import_cache::ExportDescriptor *, std::unique_ptr<FunctionExprPayload>> callables_;
   StringIndex<struct_record *> structure_index_;
   StringIndex<NamespaceRecord *> namespace_index_;
   std::vector<uint32_t> inserted_structures_;
   std::vector<uint32_t> inserted_enum_constants_;
};
