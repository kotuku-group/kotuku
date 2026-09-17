#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GCproto;
struct lua_State;

namespace tiri::debug {

enum class ImportExecutableStorage : uint8_t {
   Prototype
};

struct ImportExecutableDefinition {
   std::string Identity;
   ImportExecutableStorage Storage = ImportExecutableStorage::Prototype;
   uint32_t Depth = 0;
};

struct ImportExecutableInspection {
   std::vector<ImportExecutableDefinition> Definitions;
};

// Inspects the root-owned executable directory without consulting its metadata bundle.  Callers can compare executable
// ownership with metadata ownership instead of inferring either one from filenames or source strings.
[[nodiscard]] bool inspect_import_executables(lua_State *, GCproto *, ImportExecutableInspection &,
   std::string &Detail);

} // namespace tiri::debug
