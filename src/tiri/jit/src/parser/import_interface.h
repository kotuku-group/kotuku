// State-local translation of a portable imported-module interface into parser descriptors.

// The portable cache format contains no GC pointers or parser-owned indexes.  This object is attached to the import
// AST entry and owns every translated structure and callable for the complete parser pipeline lifetime.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast/nodes.h"
#include "type_checker.h"
#include "../../../import_module_format.h"

class ParserContext;

class InstalledImportInterface {
public:
   [[nodiscard]] static std::shared_ptr<const InstalledImportInterface> create(
      ParserContext &, const tiri::import_cache::Interface &, std::string &Diagnostic);

   [[nodiscard]] const tiri::import_cache::CompileTimeContext & context() const noexcept { return this->context_; }
   [[nodiscard]] const tiri::import_cache::ExportDescriptor * find_export(std::string_view Name) const;
   [[nodiscard]] const tiri::import_cache::ExportDescriptor * find_member(
      std::string_view Namespace, std::string_view Member) const;
   [[nodiscard]] bool is_namespace(std::string_view Name) const {
      return this->namespace_index_.contains(std::string(Name));
   }
   [[nodiscard]] const FunctionExprPayload * callable(const tiri::import_cache::ExportDescriptor &) const;
   [[nodiscard]] StaticValueDescriptor static_value(const tiri::import_cache::ValueDescriptor &) const;
   [[nodiscard]] InferredType inferred_type(const tiri::import_cache::ValueDescriptor &) const;
   [[nodiscard]] StaticValueDescriptor namespace_value(std::string_view Namespace) const;
   [[nodiscard]] InferredType namespace_type(std::string_view Namespace) const;
   [[nodiscard]] const tiri::import_cache::Interface & portable_interface() const noexcept {
      return this->portable_interface_;
   }

private:
   explicit InstalledImportInterface(ParserContext &Context) : parser_context_(Context) {}

   [[nodiscard]] bool initialise(const tiri::import_cache::Interface &, std::string &Diagnostic);
   [[nodiscard]] struct_record * structure(std::string_view Name) const;
   [[nodiscard]] ArrayElementDescriptor array_element(const tiri::import_cache::ArrayDescriptor &) const;
   [[nodiscard]] CLASSID object_class(std::string_view Name) const;

   ParserContext &parser_context_;
   tiri::import_cache::CompileTimeContext context_;
   tiri::import_cache::Interface portable_interface_;
   std::vector<std::unique_ptr<struct_record>> structures_;
   std::vector<std::unique_ptr<struct_record>> namespace_structures_;
   std::unordered_map<std::string, const tiri::import_cache::ExportDescriptor *> exports_;
   std::unordered_map<const tiri::import_cache::ExportDescriptor *, std::unique_ptr<FunctionExprPayload>> callables_;
   std::unordered_map<std::string, struct_record *> structure_index_;
   std::unordered_map<std::string, struct_record *> namespace_index_;
};
