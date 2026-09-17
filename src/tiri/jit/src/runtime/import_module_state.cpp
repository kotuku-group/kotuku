#include "import_module_state.h"

#include "lj_obj.h"
#include "../../../import_module_bundle.h"

#include <algorithm>
#include <charconv>
#include <tuple>

namespace {

void append_field(std::string &Output, std::string_view Value)
{
   char size[32];
   auto converted = std::to_chars(size, size + sizeof(size), Value.size());
   Output.append(size, converted.ptr);
   Output.push_back(':');
   Output.append(Value);
}

} // namespace

//********************************************************************************************************************
// Encodes only inputs available before a source snapshot.  Length prefixes prevent delimiter aliases.

std::string import_module_resolution_contract(std::string_view BuildIdentity,
   std::span<const tiri::cache::CompilationOption> Options, std::string_view LogicalRequest,
   std::string_view ResolvedPath, bool ImportedRoot)
{
   std::vector<tiri::cache::CompilationOption> options(Options.begin(), Options.end());
   std::ranges::sort(options, {}, [](const auto &Entry) { return std::tie(Entry.Name, Entry.Value); });

   std::string result;
   append_field(result, BuildIdentity);
   append_field(result, LogicalRequest);
   append_field(result, ResolvedPath);
   result.push_back(ImportedRoot ? '1' : '0');
   for (const auto &option : options) {
      append_field(result, option.Name);
      append_field(result, option.Value);
   }
   return result;
}

//********************************************************************************************************************

const ActiveImportModuleRecord * find_active_import_module(lua_State *L, std::string_view Contract)
{
   if (not L) return nullptr;
   lua_State *owner = mainthread(G(L));
   auto found = owner->active_import_modules.find(std::string(Contract));
   if (found IS owner->active_import_modules.end() or found->second.ambiguous or
       not found->second.record.interface_artifact) {
      owner->active_import_module_counters.fallbacks++;
      return nullptr;
   }
   owner->active_import_module_counters.reuse_hits++;
   return &found->second.record;
}

//********************************************************************************************************************
// Publish only portable C++ data.  A second compiled identity disables the contract instead of selecting by map order.

void publish_active_import_module(lua_State *L, std::string_view Contract, std::string_view CompiledIdentity,
   tiri::import_cache::FinalisedInterfacePtr Interface)
{
   if (not L or Contract.empty() or CompiledIdentity.empty() or not Interface) return;
   lua_State *owner = mainthread(G(L));
   owner->active_import_module_counters.candidates++;

   auto found = owner->active_import_modules.find(std::string(Contract));
   if (found != owner->active_import_modules.end()) {
      if (not found->second.ambiguous and found->second.record.compiled_identity != CompiledIdentity) {
         owner->active_import_module_bytes -= found->second.record.retained_bytes;
         found->second.record = {};
         found->second.ambiguous = true;
      }
      return;
   }

   const uint64_t retained = Interface->bytes().size();
   if (owner->active_import_modules.size() >= tiri::import_cache::MAX_ROOT_MODULES or
       retained > tiri::import_cache::MAX_ROOT_BUNDLE_SIZE -
          std::min<uint64_t>(owner->active_import_module_bytes, tiri::import_cache::MAX_ROOT_BUNDLE_SIZE)) {
      owner->active_import_module_counters.fallbacks++;
      return;
   }

   ActiveImportModuleBinding binding;
   binding.record.compiled_identity.assign(CompiledIdentity);
   binding.record.interface_artifact = std::move(Interface);
   binding.record.retained_bytes = retained;
   owner->active_import_module_bytes += retained;
   owner->active_import_modules.emplace(Contract, std::move(binding));
   owner->active_import_module_counters.publications++;
}
