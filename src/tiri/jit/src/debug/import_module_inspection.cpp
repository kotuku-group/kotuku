#include "import_module_inspection.h"

#include "lj_obj.h"

namespace tiri::debug {

bool inspect_import_executables(lua_State *Lua, GCproto *Root, ImportExecutableInspection &Output,
   std::string &Detail)
{
   Output = {};
   Detail.clear();
   if (not Lua or not Root) {
      Detail = "a Lua state and root prototype are required";
      return false;
   }
   const ImportModuleTable *table = proto_import_module_table(Root);
   if (not table) return true;
   if (table->version != IMPORT_MODULE_TABLE_VERSION) {
      Detail = "the imported-module executable directory has an unsupported version";
      return false;
   }
   const ImportModuleTableEntry *entries = import_module_table_entries(table);
   for (uint32_t i = 0; i < table->entry_count; ++i) {
      if (not gcref(entries[i].compiled_identity) or gcref(entries[i].compiled_identity)->gch.gct != ~LJ_TSTR or
          not gcref(entries[i].initialiser) or gcref(entries[i].initialiser)->gch.gct != ~LJ_TPROTO) {
         Detail = "the imported-module executable directory contains an invalid entry";
         return false;
      }
      GCstr *identity = gco_to_string(gcref(entries[i].compiled_identity));
      Output.Definitions.push_back({ std::string(strdata(identity), identity->len),
         ImportExecutableStorage::Prototype, 0 });
   }
   return true;
}

} // namespace tiri::debug
