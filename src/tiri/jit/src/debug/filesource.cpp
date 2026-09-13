// FileSource tracking for accurate error reporting in imported files.
// Copyright © 2025-2026 Paul Manias

#include "filesource.h"
#include "../runtime/lj_gc.h"
#include "../runtime/lj_str.h"
#include <kotuku/main.h>

//********************************************************************************************************************
// Register a new file source in the lua_State.
// Returns the file index, or FILESOURCE_OVERFLOW_INDEX (255) if the limit is exceeded.
// NB: Path is the full path, including filename.

uint8_t register_file_source(lua_State *L, std::string &Path, const std::string &Filename, BCLine FirstLine,
   BCLine SourceLines, uint8_t ParentIndex, BCLine ImportLine)
{
   kt::Log log(__FUNCTION__);

   if (L->file_sources.size() >= FILESOURCE_MAX_COUNT) {
      log.msg("FileSource limit exceeded (%d files). Additional imports will show as 'unknown'.", FILESOURCE_MAX_COUNT);
      return FILESOURCE_OVERFLOW_INDEX;
   }

   std::string resolved_path;
   if (!ResolvePath(Path, RSF::NO_FILE_CHECK, &resolved_path)) {
      Path = resolved_path;
   }

   auto path_hash = kt::strihash(Path);

   // Check if this file is already registered

   if (auto it = L->file_index_map.find(path_hash); it != L->file_index_map.end() and
       it->second < L->file_sources.size() and L->file_sources[it->second].path IS Path) {
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

std::optional<uint8_t> find_file_source(lua_State *L, const std::string &Path)
{
   const uint32_t hash = kt::strihash(Path);
   if (auto index = find_file_source(L, hash); index and L->file_sources[index.value()].path IS Path) return index;
   for (size_t i = 0; i < L->file_sources.size(); ++i) {
      if (L->file_sources[i].path IS Path) return uint8_t(i);
   }
   return std::nullopt;
}

static void set_source_root(GCproto *Proto, GCproto *Root)
{
   setgcref(Proto->source_root, obj2gco(Root));
   if (Proto->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (MSize i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO) set_source_root(gco_to_proto(object), Root);
      }
   }
}

void attach_compilation_sources(lua_State *L, GCproto *Root, const std::vector<CompilationSourceRecord> &Records)
{
   if (not Root or Records.empty() or Records.size() > FILESOURCE_MAX_COUNT) return;
   const MSize bytes = MSize(sizeof(CompilationSourceMap) + Records.size() * sizeof(CompilationSourceEntry));
   auto map = (CompilationSourceMap *)lj_mem_new(L, bytes);
   map->version = COMPILATION_SOURCE_VERSION;
   map->count = uint8_t(Records.size());
   map->root = 0;
   map->reserved = 0;
   auto entries = compilation_source_entries(map);
   for (size_t i = 0; i < Records.size(); ++i) {
      setgcref(entries[i].canonical_path, obj2gco(&G(L)->strempty));
      setgcref(entries[i].display_filename, obj2gco(&G(L)->strempty));
      setgcref(entries[i].declared_namespace, obj2gco(&G(L)->strempty));
   }
   setmref(Root->compilation_sources, map);
   set_source_root(Root, Root);
   for (size_t i = 0; i < Records.size(); ++i) {
      const auto &source = Records[i];
      setgcref(entries[i].canonical_path,
         obj2gco(lj_str_new(L, source.canonical_path.data(), source.canonical_path.size())));
      setgcref(entries[i].display_filename,
         obj2gco(lj_str_new(L, source.display_filename.data(), source.display_filename.size())));
      setgcref(entries[i].declared_namespace,
         obj2gco(lj_str_new(L, source.declared_namespace.data(), source.declared_namespace.size())));
      entries[i].first_line = source.first_line;
      entries[i].total_lines = source.total_lines;
      entries[i].import_line = source.import_line;
      entries[i].runtime_index = source.runtime_index;
      entries[i].parent = source.parent;
      entries[i].role = source.role;
      entries[i].reserved = 0;
   }
}

static uint8_t loaded_source_id(uint8_t Wire, const std::vector<CompilationSourceRecord> &Records)
{
   if (Wire < Records.size()) return Records[Wire].runtime_index;
   return Wire;
}

static void remap_loaded_sources(GCproto *Proto, const std::vector<CompilationSourceRecord> &Records)
{
   Proto->file_source_idx = loaded_source_id(Proto->file_source_idx, Records);
   if (proto_lineinfo(Proto)) {
      BCLine *lines = (BCLine *)proto_lineinfo(Proto);
      for (MSize i = 0; i + 1 < Proto->sizebc; ++i) {
         lines[i] = BCLine::encode(loaded_source_id(lines[i].fileIndex(), Records), lines[i].lineNumber());
      }
   }
   if (Proto->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Proto->k) - 1;
      for (MSize i = 0; i < Proto->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO) remap_loaded_sources(gco_to_proto(object), Records);
      }
   }
}

void attach_loaded_compilation_sources(lua_State *L, GCproto *Root,
   const std::vector<CompilationSourceRecord> &Records)
{
   std::vector<CompilationSourceRecord> resolved = Records;
   L->file_sources.reserve(std::min<size_t>(FILESOURCE_MAX_COUNT, L->file_sources.size() + Records.size()));
   L->file_index_map.reserve(L->file_sources.size() + Records.size());
   for (size_t i = 0; i < resolved.size(); ++i) {
      auto &source = resolved[i];
      if (source.role IS CompilationSourceRole::Synthetic) {
         source.runtime_index = FILESOURCE_SYNTHETIC_INDEX;
         continue;
      }
      std::optional<uint8_t> existing;
      for (size_t state_index = 0; state_index < L->file_sources.size(); ++state_index) {
         if (L->file_sources[state_index].path IS source.canonical_path) {
            existing = uint8_t(state_index);
            break;
         }
      }
      if (existing) source.runtime_index = existing.value();
      else {
         uint8_t parent = source.parent < i ? resolved[source.parent].runtime_index : 0;
         std::string path = source.canonical_path;
         source.runtime_index = register_file_source(L, path, source.display_filename, source.first_line,
            source.total_lines, parent, source.import_line);
         set_file_source_namespace(L, source.runtime_index, source.declared_namespace);
      }
   }
   remap_loaded_sources(Root, resolved);
   attach_compilation_sources(L, Root, resolved);
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
