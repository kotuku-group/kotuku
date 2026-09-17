#pragma once

#include "../../../import_module_bundle.h"

#include <string_view>
#include <vector>

struct lua_State;

// Portable metadata retained from a completely validated bytecode load.  Bytecode distinguishes a valid empty
// imported-module graph from source input or a failed load.
struct BytecodeLoadMetadata {
   std::vector<tiri::import_cache::RootModuleRecord> ImportedModules;
   bool Bytecode = false;
};

[[nodiscard]] int lj_load_with_bytecode_metadata(
   lua_State *, std::string_view, const char *, BytecodeLoadMetadata &);
