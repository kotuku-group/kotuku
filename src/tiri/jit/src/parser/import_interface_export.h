#pragma once

#include <string>
#include <string_view>

struct BlockStmt;
struct FunctionExprPayload;
struct ImportedModuleUnit;
class ParserContext;

// Build and install the portable compile-time interfaces for every cold imported module after semantic analysis has
// completed.  Warm entries already own a decoded interface and are left unchanged.
[[nodiscard]] bool prepare_import_interfaces(ParserContext &, BlockStmt &, std::string &Diagnostic);

// Return the callable that a module compiled from source exports as Namespace.Member, applying the declarations and
// precedence of its eventual portable interface.  This is the cold-import counterpart to the installed interface's
// member lookup and is valid once the module body has completed static binding discovery.
[[nodiscard]] const FunctionExprPayload * source_module_member(ParserContext &, ImportedModuleUnit &,
   std::string_view Namespace, std::string_view Member);

// Return the callable that a module compiled from source exports as the global Name, including globals contributed
// by its nested modules.  This is the cold-import counterpart to the installed interface's global bindings, so that
// call contracts such as @NoDiscard do not depend on whether the module came from the cache.
[[nodiscard]] const FunctionExprPayload * source_module_global(ParserContext &, ImportedModuleUnit &,
   std::string_view Name);
