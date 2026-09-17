#pragma once

#include "../../../cache_manifest.h"
#include "../../../import_module_format.h"

#include <span>
#include <string>
#include <string_view>

struct lua_State;
struct ActiveImportModuleRecord;

[[nodiscard]] std::string import_module_resolution_contract(std::string_view BuildIdentity,
   std::span<const tiri::cache::CompilationOption> Options, std::string_view LogicalRequest,
   std::string_view ResolvedPath, bool ImportedRoot);

[[nodiscard]] const ActiveImportModuleRecord * find_active_import_module(
   lua_State *, std::string_view Contract);

void publish_active_import_module(lua_State *, std::string_view Contract, std::string_view CompiledIdentity,
   tiri::import_cache::FinalisedInterfacePtr);
