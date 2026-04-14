// FileSource tracking for accurate error reporting in imported files.
// Copyright © 2025-2026 Paul Manias

#include "filesource.h"
#include <kotuku/main.h>

//********************************************************************************************************************
// Register a new file source in the lua_State.
// Returns the file index, or FILESOURCE_OVERFLOW_INDEX (255) if the limit is exceeded.
// NB: Path is the full path, including filename.

uint8_t register_file_source(lua_State *L, std::string &Path, const std::string &Filename, BCLine FirstLine,
   BCLine SourceLines, uint8_t ParentIndex, BCLine ImportLine)
{
   pf::Log log(__FUNCTION__);

   if (L->file_sources.size() >= FILESOURCE_MAX_INDEX) { // Check if we've hit the file limit
      if (L->file_sources.size() IS FILESOURCE_MAX_INDEX) { // Initialise the overflow entry if not already done
         log.msg("FileSource limit exceeded (%d files). Additional imports will show as 'unknown'.", FILESOURCE_MAX_INDEX);

         FileSource overflow;
         overflow.path               = "unknown";
         overflow.filename           = "unknown";
         overflow.declared_namespace = "";
         overflow.first_line         = 0;
         overflow.total_lines        = 0;
         overflow.path_hash          = 0;
         overflow.parent_file_index  = 0;
         overflow.import_line        = 0;

         L->file_sources.push_back(std::move(overflow));
         // Don't add to file_index_map - overflow is not looked up by hash
      }
      return FILESOURCE_OVERFLOW_INDEX;
   }

   std::string resolved_path;
   if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved_path) IS ERR::Okay) {
      Path = resolved_path;
   }

   auto path_hash = pf::strihash(Path);

   // Check if this file is already registered

   if (auto it = L->file_index_map.find(path_hash);it != L->file_index_map.end()) {
      log.msg("File already registered: %s $%.8x (index %d)", Filename.c_str(), path_hash, it->second);
      return it->second;
   }

   // Register the new file

   auto new_index = uint8_t(L->file_sources.size());

   FileSource source;
   source.path               = Path;
   source.filename           = Filename;
   source.declared_namespace = "";
   source.first_line         = FirstLine;
   source.total_lines        = SourceLines;
   source.path_hash          = path_hash;
   source.parent_file_index  = ParentIndex;
   source.import_line        = ImportLine;

   L->file_sources.push_back(std::move(source));
   L->file_index_map[path_hash] = new_index;

   log.msg("Registered file source: %s $%.8x (index %d, parent %d, import line %d)", Filename.c_str(), path_hash,
      new_index, ParentIndex, ImportLine.lineNumber());

   return new_index;
}

//********************************************************************************************************************
// Find a file source by path hash.

std::optional<uint8_t> find_file_source(lua_State *L, uint32_t PathHash)
{
   if (auto it = L->file_index_map.find(PathHash); it != L->file_index_map.end()) return it->second;
   return std::nullopt;
}

//********************************************************************************************************************
// Get a file source by index.

const FileSource* get_file_source(lua_State *L, uint8_t Index)
{
   if (Index < L->file_sources.size()) return &L->file_sources[Index];
   return nullptr;
}

//********************************************************************************************************************
// Register a file being parsed as a "main" file source.  Unlike imported files, main files have no parent.
// This is called for:
//   1. The initial script execution (file_sources will be empty)
//   2. Subsequent loadFile() calls during execution (file_sources already populated)
// The file_sources are not cleared in order to preserve import deduplication across loadFile() calls.

uint8_t register_main_file_source(lua_State *L, std::string &Path, const std::string &Filename, BCLine SourceLines)
{
   return register_file_source(L, Path, Filename, 1, SourceLines, 0, 0);
}

//********************************************************************************************************************
// Set the declared namespace for a file source.

bool set_file_source_namespace(lua_State *L, uint8_t Index, const std::string &Namespace)
{
   if (Index < L->file_sources.size()) {
      L->file_sources[Index].declared_namespace = Namespace;
      return true;
   }
   else return false;
}

//********************************************************************************************************************
// Find a file source by its declared namespace.

std::optional<uint8_t> find_file_source_by_namespace(lua_State *L, const std::string &Namespace)
{
   for (size_t i = 0; i < L->file_sources.size(); i++) {
      if (L->file_sources[i].declared_namespace IS Namespace) return uint8_t(i);
   }
   return std::nullopt;
}

//********************************************************************************************************************

int widest_file_source(lua_State *L, bool StripExt)
{
   int max_width = 0;
   for (const auto &fs : L->file_sources) {
      int len = int(fs.filename.length());
      if (StripExt and fs.filename.ends_with(".tiri")) len -= 6;
      if (len > max_width) max_width = len;
   }
   return max_width;
}
